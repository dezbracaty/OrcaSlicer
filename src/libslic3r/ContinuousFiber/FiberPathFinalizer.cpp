#include "FiberPathFinalizer.hpp"
#include "ContinuousFiberFillStrategy.hpp"

#include "../ClipperUtils.hpp"
#include "../BoundingBox.hpp"
#include "../ExtrusionEntity.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

// Normalize before coverage/validation, never while emitting. Two microns is
// above the diagonal of the G-code coordinate grid and below Fill resolution.
constexpr double command_resolution_mm = 0.002;
constexpr double normalization_error_mm = command_resolution_mm;

double area_mm2(const ExPolygons& polygons)
{
    return unscaled<double>(unscaled<double>(std::abs(area(polygons))));
}

// Preserve endpoints and bound the deviation of EVERY removed source knot.
// The optional map relates retained points to their original arc length, so
// command cleanup cannot shift a bend's speed interval or a process boundary.
Polyline3 normalized_xy(const Polyline3& input, std::vector<double>* source_positions = nullptr,
                        double error_mm = normalization_error_mm, const ExPolygons* centerline_domain = nullptr,
                        const std::vector<bool>* protected_knots = nullptr)
{
    Polyline3 result;
    // Collapse exact straight runs first. Their endpoints bound the distance of
    // every interior point to any later chord (distance to a segment is convex).
    // The tolerance pass therefore never needs to rescan discarded straight knots.
    std::vector<size_t> knots;
    std::vector<double> positions;
    if (source_positions) positions.resize(input.points.size(),0.0);
    for (size_t i=0;i<input.points.size();++i) {
        const auto& point=input.points[i];
        if (point.z()!=0) throw std::invalid_argument("Fiber LayerXY geometry has nonzero Z");
        if (source_positions && i) positions[i]=positions[i-1]+
            unscale<double>((point-input.points[i-1]).cast<double>().norm());
        if (!knots.empty() && point==input.points[knots.back()]) continue;
        while (knots.size()>=2) {
            if (protected_knots && (*protected_knots)[knots.back()]) break;
            const Vec2d a=(input.points[knots.back()]-input.points[knots[knots.size()-2]]).head<2>().cast<double>();
            const Vec2d b=(point-input.points[knots.back()]).head<2>().cast<double>();
            if (a.dot(b)<=0 || a.x()*b.y()!=a.y()*b.x()) break;
            knots.pop_back();
        }
        knots.push_back(i);
    }
    std::vector<size_t> retained;
    const BoundingBox domain_bounds = centerline_domain ? get_extents(*centerline_domain) : BoundingBox{};
    std::optional<BoundingBox> clip_window;
    Polygons local_domain;
    const auto chord_inside_domain = [&](const Point& from, const Point& to) {
        if (!clip_window || !clip_window->contains(from) || !clip_window->contains(to)) {
            clip_window = BoundingBox(Points{from, to});
            // Reuse a horizontal slab. Keep its full X extent so Clipper sees
            // every scanbeam event between the chord endpoints, including
            // distant vertices at the same Y. A small XY crop can change
            // integer rounding decisions on nearly collinear boundary edges.
            // The margin controls cache reuse only, not geometric tolerance.
            clip_window->offset(scale_(0.25));
            clip_window->min.x() = std::min(clip_window->min.x(), domain_bounds.min.x());
            clip_window->max.x() = std::max(clip_window->max.x(), domain_bounds.max.x());
            local_domain = ClipperUtils::clip_clipper_polygons_with_subject_bbox(*centerline_domain, *clip_window);
        }
        return diff_pl(Polyline(Points{from, to}), local_domain).empty();
    };
    for (size_t index=0;index<knots.size();++index) {
        const auto& point=input.points[knots[index]];
        while (result.points.size()>=2) {
            if (protected_knots && (*protected_knots)[knots[retained.back()]]) break;
            const Vec2d a=(result.points.back()-result.points[result.points.size()-2]).head<2>().cast<double>();
            const Vec2d b=(point-result.points.back()).head<2>().cast<double>();
            if (std::min(a.norm(),b.norm())>scale_(command_resolution_mm)) break;
            const Vec2d begin=result.points[result.points.size()-2].head<2>().cast<double>();
            const Vec2d delta=point.head<2>().cast<double>()-begin;
            if (delta.squaredNorm()==0) break;
            bool within=true;
            for (size_t i=retained[retained.size()-2]+1;i<index;++i) {
                const Vec2d v=input.points[knots[i]].head<2>().cast<double>()-begin;
                const double t=std::clamp(v.dot(delta)/delta.squaredNorm(),0.0,1.0);
                if ((v-t*delta).norm()>scale_(error_mm)) { within=false;break; }
            }
            if (!within) break;
            // A short chord may satisfy the error bound yet cut across a concave
            // domain edge. Geometry cleanup must preserve the planned domain.
            if (centerline_domain && !chord_inside_domain(
                    Point(result.points[result.points.size()-2].x(), result.points[result.points.size()-2].y()),
                    Point(point.x(), point.y()))) break;
            result.points.pop_back();retained.pop_back();
        }
        result.points.push_back(point);retained.push_back(index);
    }
    if (source_positions) {
        source_positions->clear();
        for (size_t index:retained) source_positions->push_back(positions[knots[index]]);
    }
    return result;
}

struct ArcGeometry {
    const Polyline3& geometry;
    std::vector<double> positions {0.0};
    explicit ArcGeometry(const Polyline3& line) : geometry(line)
    {
        for (size_t i = 1; i < line.points.size(); ++i)
            positions.push_back(positions.back() + unscale<double>((line.points[i] - line.points[i-1]).cast<double>().norm()));
    }
    Point3 at(double s) const
    {
        if (s <= 0) return geometry.points.front();
        if (s >= positions.back()) return geometry.points.back();
        const size_t i = size_t(std::upper_bound(positions.begin(), positions.end(), s) - positions.begin());
        const double t = (s - positions[i-1]) / (positions[i] - positions[i-1]);
        const Vec3d p = (1.0-t)*geometry.points[i-1].cast<double>() + t*geometry.points[i].cast<double>();
        return Point3(p.x(), p.y(), 0.0);
    }
};

// Piece boundaries are geometry knots, not approximate cumulative command lengths.
// The tiny station allowance below only reconciles sum(unscale(edge)) with
// unscale(sum(edge)); it never accepts a negative source range.
constexpr double station_roundoff_mm = 1e-8;

double source_station_at(const FiberExecutionPiece& piece,double station)
{
    if (station<=piece.execution_begin_mm) return piece.source_begin_mm;
    if (station>=piece.execution_end_mm) return piece.source_end_mm;
    if (piece.source_station_knots && !piece.source_station_knots->empty()) {
        const auto& chart=*piece.source_station_knots;
        const auto next=std::upper_bound(chart.begin(),chart.end(),station,
            [](double s,const auto& knot){return s<knot.first;});
        if (next==chart.begin() || next==chart.end())
            throw std::invalid_argument("Fiber source chart does not cover its planning range");
        const auto& before=*(next-1);
        const double t=(station-before.first)/(next->first-before.first);
        return std::clamp(before.second+t*(next->second-before.second),piece.source_begin_mm,piece.source_end_mm);
    }
    const double ratio=std::clamp((station-piece.execution_begin_mm)/
        (piece.execution_end_mm-piece.execution_begin_mm),0.0,1.0);
    return std::clamp(piece.source_begin_mm+ratio*(piece.source_end_mm-piece.source_begin_mm),
                      piece.source_begin_mm,piece.source_end_mm);
}

std::vector<bool> piece_knots(const Polyline3& geometry,
                              const std::vector<FiberExecutionPiece>& pieces)
{
    const ArcGeometry arc(geometry);
    std::vector<bool> keep(geometry.points.size(), false);
    double previous = 0.0;
    for (const auto& piece : pieces) {
        if (!std::isfinite(piece.execution_begin_mm) || !std::isfinite(piece.execution_end_mm) ||
            piece.execution_begin_mm != previous || piece.execution_end_mm <= piece.execution_begin_mm ||
            (piece.kind == FiberExecutionPieceKind::RetainedSource &&
             (!std::isfinite(piece.source_begin_mm) || !std::isfinite(piece.source_end_mm) ||
              piece.source_begin_mm < 0 || piece.source_end_mm <= piece.source_begin_mm)))
            throw std::invalid_argument("Invalid ordered fiber planning piece");
        if (piece.source_station_knots) {
            const auto& chart=*piece.source_station_knots;
            if (piece.kind!=FiberExecutionPieceKind::RetainedSource || chart.size()<2 ||
                chart.front().first>piece.execution_begin_mm || chart.back().first<piece.execution_end_mm ||
                chart.front().second>piece.source_begin_mm || chart.back().second<piece.source_end_mm)
                throw std::invalid_argument("Fiber source chart has invalid bounds");
            for (size_t i=0;i<chart.size();++i)
                if (!std::isfinite(chart[i].first) || !std::isfinite(chart[i].second) ||
                    chart[i].first<0 || chart[i].second<0 ||
                    (i && (chart[i].first<=chart[i-1].first || chart[i].second<=chart[i-1].second)))
                    throw std::invalid_argument("Fiber source chart is not strictly positive and ordered");
        }
        for (double boundary : {piece.execution_begin_mm, piece.execution_end_mm}) {
            auto next = std::lower_bound(arc.positions.begin(), arc.positions.end(), boundary);
            size_t index = next == arc.positions.end() ? arc.positions.size()-1 : size_t(next-arc.positions.begin());
            if (index && std::abs(arc.positions[index-1]-boundary) < std::abs(arc.positions[index]-boundary)) --index;
            if (std::abs(arc.positions[index]-boundary) > station_roundoff_mm)
                throw std::invalid_argument("Fiber piece boundary is not an exact geometry knot");
            keep[index] = true;
        }
        previous = piece.execution_end_mm;
    }
    if (!pieces.empty() && std::abs(previous-arc.positions.back()) > station_roundoff_mm)
        throw std::invalid_argument("Fiber planning pieces do not cover their geometry");
    return keep;
}

// Locate exact original knots first. Only inserted process split points need a
// finite-segment projection. Advancing monotonically avoids confusing a cyclic
// source seam or a close neighboring ring with the next point on this chain.
struct PlanningStationMap {
    const Polyline3& geometry;
    ArcGeometry arc;
    std::map<std::pair<coord_t,coord_t>,std::vector<size_t>> vertices;
    double previous = 0.0;
    size_t segment = 0;
    explicit PlanningStationMap(const Polyline3& line) : geometry(line), arc(line)
    {
        for (size_t i=0;i<line.points.size();++i)
            vertices[{line.points[i].x(),line.points[i].y()}].push_back(i);
    }
    double at(const Point3& point)
    {
        const auto found = vertices.find({point.x(),point.y()});
        if (found != vertices.end()) {
            const auto next = std::lower_bound(found->second.begin(),found->second.end(),previous,
                [&](size_t index,double station){return arc.positions[index] < station;});
            if (next != found->second.end()) {
                segment = std::min(*next,geometry.points.size()-2);
                return previous = arc.positions[*next];
            }
        }
        const double tolerance = ContourRoundingOptions{}.geometry_tolerance_mm;
        for (size_t i=segment;i+1<geometry.points.size();++i) {
            const Vec2d a=geometry.points[i].head<2>().cast<double>();
            const Vec2d delta=(geometry.points[i+1]-geometry.points[i]).head<2>().cast<double>();
            if (delta.squaredNorm()==0) continue;
            const Vec2d from=point.head<2>().cast<double>()-a;
            const double t=std::clamp(from.dot(delta)/delta.squaredNorm(),0.0,1.0);
            const double station=arc.positions[i]+t*(arc.positions[i+1]-arc.positions[i]);
            if (station < previous || unscale<double>((from-t*delta).norm()) > tolerance) continue;
            segment=i;
            return previous=station;
        }
        throw std::invalid_argument("Fiber command point has no ordered planning station");
    }
};

double interpolate_stations(const std::vector<double>& from, const std::vector<double>& to, double station)
{
    if (station<=from.front()) return to.front();
    if (station>=from.back()) return to.back();
    const size_t i=size_t(std::upper_bound(from.begin(),from.end(),station)-from.begin());
    const double t=(station-from[i-1])/(from[i]-from[i-1]);
    return to[i-1]+t*(to[i]-to[i-1]);
}

FiberExecutionPiece clipped_piece(const FiberExecutionPiece& piece,double begin,double end)
{
    FiberExecutionPiece clipped=piece;
    clipped.execution_begin_mm=begin;
    clipped.execution_end_mm=end;
    if (piece.kind==FiberExecutionPieceKind::RetainedSource) {
        clipped.source_begin_mm=source_station_at(piece,begin);
        clipped.source_end_mm=source_station_at(piece,end);
    }
    return clipped;
}

// A speed field on the complete normalized path, including the cyclic seam.
// No source vertex number or input edge length enters the speed formula.
struct FiberSpeedPlanner {
    struct Bend { double begin, end, angle; };
    std::vector<Bend> bends;
    double length, minimum, maximum, transition;
    bool closed;
    FiberSpeedPlanner(const Polyline3& path, double vmin, double vmax, double distance,
                      const std::vector<ContourArc>& arcs, bool include_nonarc_turns = false)
        : length(unscale<double>(path.length())), minimum(vmin), maximum(vmax), transition(distance),
          closed(path.points.front() == path.points.back())
    {
        if (!arcs.empty()) {
            for (const auto& arc:arcs)
                bends.push_back({arc.begin_mm,arc.end_distance_mm,std::abs(arc.source_sweep_radians != 0 ? arc.source_sweep_radians : arc.sweep_radians)});
            if (!include_nonarc_turns) return;
        }
        const ArcGeometry arc(path);
        const size_t n = path.points.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            if (i == 0 && !closed) continue;
            if (include_nonarc_turns && std::any_of(arcs.begin(),arcs.end(),[&](const ContourArc& bend) {
                    return arc.positions[i]>bend.begin_mm+station_roundoff_mm &&
                           arc.positions[i]<bend.end_distance_mm-station_roundoff_mm;
                })) continue;
            const auto& prev = path.points[i == 0 ? n-2 : i-1];
            const Vec2d incoming = (path.points[i]-prev).head<2>().cast<double>().normalized();
            const Vec2d outgoing = (path.points[i+1]-path.points[i]).head<2>().cast<double>().normalized();
            const double angle = std::acos(std::clamp(incoming.dot(outgoing), -1.0, 1.0));
            if (angle > 1e-12) bends.push_back({arc.positions[i],arc.positions[i],angle});
        }
    }
    double limit(double begin, double end) const
    {
        double result = maximum;
        for (const auto& bend : bends) {
            double distance = std::numeric_limits<double>::max();
            for (int lap = closed ? -1 : 0; lap <= (closed ? 1 : 0); ++lap) {
                const double b=bend.begin+lap*length,e=bend.end+lap*length;
                distance=std::min(distance,e<begin?begin-e:b>end?b-end:0.0);
            }
            const double corner_speed = maximum - (maximum-minimum)*std::min(PI,bend.angle)/PI;
            result = std::min(result, corner_speed + (maximum-corner_speed)*std::min(1.0, distance/transition));
        }
        return result;
    }
};

void plan_edges(PreparedFiberPath& prepared, const Polyline3& depositing, const ContinuousFiberConfig& config,
                const std::vector<ContourArc>& arcs, const ExPolygons* centerline_domain,
                const std::vector<FiberExecutionPiece>& pieces,
                const Polyline3& finish, const std::vector<FiberExecutionPiece>& finish_pieces)
{
    const bool contour = prepared.id.parent.purpose == FiberPathPurpose::Contour;
    const double ratio = contour ? config.contour_feed_ratio*config.contour_feed_correction :
                                   config.infill_feed_ratio*config.infill_feed_correction;
    FiberSpeedPlanner speeds(depositing,
        contour ? config.contour_min_speed_mm_s : config.infill_min_speed_mm_s,
        contour ? config.contour_max_speed_mm_s : config.infill_max_speed_mm_s,
        config.corner_transition_length_mm, arcs, !pieces.empty());
    const FiberSpeedPlanner tail_limits(depositing, config.tail_min_speed_mm_s,
                                       config.tail_max_speed_mm_s, config.corner_transition_length_mm, arcs, !pieces.empty());
    std::optional<PlanningStationMap> deposition_map, finish_map;
    if (!pieces.empty()) {
        piece_knots(depositing,pieces);
        deposition_map.emplace(depositing);
        if (!finish.empty()) {
            if (finish_pieces.empty()) throw std::invalid_argument("Connected fiber finish has no source pieces");
            piece_knots(finish,finish_pieces);
            finish_map.emplace(finish);
        } else if (!finish_pieces.empty()) throw std::invalid_argument("Empty fiber finish has source pieces");
    } else if (!finish_pieces.empty()) throw std::invalid_argument("Fiber finish pieces require a connected plan");
    double offset = 0.0;
    double command_offset = 0.0;
    for (size_t span_index=0;span_index<prepared.spans.size();++span_index) {
        FiberMotionSpan& span=prepared.spans[span_index];
        const auto& origins=span.deposits_fiber()?pieces:finish_pieces;
        std::vector<double> planning_stations;
        std::vector<bool> protected_knots(span.geometry.points.size(),false);
        const ArcGeometry input_arc(span.geometry);
        if (!origins.empty()) {
            auto& map=span.deposits_fiber()?*deposition_map:*finish_map;
            for (size_t i=0;i<span.geometry.points.size();++i) {
                const double station=map.at(span.geometry.points[i]);
                planning_stations.push_back(station);
                protected_knots[i]=std::any_of(origins.begin(),origins.end(),[&](const auto& piece) {
                    return std::abs(station-piece.execution_begin_mm)<=station_roundoff_mm ||
                           std::abs(station-piece.execution_end_mm)<=station_roundoff_mm;
                });
            }
        }
        const double source_length=unscale<double>(span.geometry.length());
        std::vector<double> source_positions;
        span.geometry = normalized_xy(span.geometry,&source_positions,
            !origins.empty() || !arcs.empty() ? ContourRoundingOptions{}.chord_tolerance_mm : normalization_error_mm,
            span.deposits_fiber() ? centerline_domain : nullptr,
            origins.empty()?nullptr:&protected_knots);
        const ArcGeometry arc(span.geometry);
        const double length = arc.positions.back();
        const auto source_at = [&](double s) {
            if (s<=0) return 0.0;
            if (s>=length) return source_length;
            const size_t i=size_t(std::upper_bound(arc.positions.begin(),arc.positions.end(),s)-arc.positions.begin());
            const double t=(s-arc.positions[i-1])/(arc.positions[i]-arc.positions[i-1]);
            return source_positions[i-1]+t*(source_positions[i]-source_positions[i-1]);
        };
        const auto planning_at = [&](double s) {
            if (s<=0) return planning_stations.front();
            if (s>=length) return planning_stations.back();
            return interpolate_stations(input_arc.positions,planning_stations,source_at(s));
        };
        std::vector<double> cuts = arc.positions;
        const bool tail = span.kind == FiberMotionKind::PassiveDepositingAfterCut;
        const size_t cells = tail ? std::max(size_t(2), size_t(std::ceil(length/config.tail_speed_step_length_mm))) : 0;
        if (cells > 1000000 || length/config.speed_sampling_length_mm > 1000000)
            throw std::length_error("Fiber command sampling exceeds the supported budget");
        const auto add_sample = [&](double s) {
            const auto next = std::lower_bound(arc.positions.begin(), arc.positions.end(), s);
            if (next == arc.positions.end() || next == arc.positions.begin()) return;
            // A speed sample next to an existing geometry knot must reuse that
            // knot, not manufacture a sub-resolution edge and reject the path.
            const Point3 point = arc.at(s);
            const size_t i = size_t(next-arc.positions.begin());
            if (unscale<double>((point-span.geometry.points[i]).cast<double>().norm()) <= command_resolution_mm ||
                unscale<double>((point-span.geometry.points[i-1]).cast<double>().norm()) <= command_resolution_mm) return;
            cuts.push_back(s);
        };
        if (tail) {
            for (size_t i = 1; i < cells; ++i) add_sample(length*double(i)/double(cells));
        } else if (span.actively_feeds_fiber()) {
            const double h = config.speed_sampling_length_mm;
            for (double s = (std::floor(offset/h)+1)*h; s < offset+length-EPSILON; s += h)
                if (s > offset+EPSILON) add_sample(s-offset);
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double a, double b){return std::abs(a-b)<1e-7;}), cuts.end());
        Polyline3 sampled;
        std::vector<FiberEdgeProcess> edges;
        size_t previous_owner=origins.size();
        for (double s : cuts) sampled.points.push_back(arc.at(s));
        for (size_t i = 1; i < cuts.size(); ++i) {
            double speed = config.finish_motion_speed_mm_s;
            const double speed_begin=origins.empty()?offset+source_at(cuts[i-1]):planning_at(cuts[i-1]);
            const double speed_end=origins.empty()?offset+source_at(cuts[i]):planning_at(cuts[i]);
            if (span.actively_feeds_fiber()) {
                speed = speeds.limit(speed_begin,speed_end);
                if (span.kind == FiberMotionKind::PoweredStart) speed = std::min(speed, config.start_speed_mm_s);
            } else if (span.kind == FiberMotionKind::PrefedLanding) {
                speed = config.landing_speed_mm_s;
            } else if (tail) {
                const size_t cell = std::min(cells-1, size_t(std::floor((cuts[i-1]+cuts[i])*0.5/length*cells)));
                speed = config.tail_min_speed_mm_s + (config.tail_max_speed_mm_s-config.tail_min_speed_mm_s)*double(cell)/double(cells-1);
                // The requested ramp is capped by the path's corner limits.
                // A speed conflict does not invalidate an otherwise printable path.
                speed = std::min(speed, tail_limits.limit(speed_begin,speed_end));
            }
            edges.push_back({speed, span.actively_feeds_fiber() ? ratio : 0.0});
            if (!origins.empty()) {
                const double begin=planning_at(cuts[i-1]);
                const double end=planning_at(cuts[i]);
                const double middle=(begin+end)*0.5;
                const auto owner=std::find_if(origins.begin(),origins.end(),[&](const auto& piece) {
                    return middle>=piece.execution_begin_mm && middle<piece.execution_end_mm;
                });
                if (owner==origins.end() || begin<owner->execution_begin_mm-station_roundoff_mm ||
                    end>owner->execution_end_mm+station_roundoff_mm || end<=begin)
                    throw std::invalid_argument("Fiber command edge crosses a source-piece boundary");
                const auto clipped=clipped_piece(*owner,std::max(begin,owner->execution_begin_mm),
                                                        std::min(end,owner->execution_end_mm));
                const size_t owner_index=size_t(owner-origins.begin());
                const double edge_length=unscale<double>((sampled.points[i]-sampled.points[i-1]).cast<double>().norm());
                if (!prepared.command_pieces.empty()) {
                    auto& previous=prepared.command_pieces.back();
                    if (previous_owner==owner_index && previous.span_index==span_index && previous.end_edge==i-1 &&
                        previous.origin.kind==clipped.kind && previous.origin.source==clipped.source &&
                        previous.origin.other_source==clipped.other_source &&
                        previous.origin.transition_id==clipped.transition_id &&
                        previous.origin.display_purpose==clipped.display_purpose &&
                        previous.origin.source_station_knots==clipped.source_station_knots &&
                        previous.origin.execution_end_mm==clipped.execution_begin_mm &&
                        (clipped.kind!=FiberExecutionPieceKind::RetainedSource || previous.origin.source_end_mm==clipped.source_begin_mm)) {
                        previous.end_edge=i;
                        previous.origin.execution_end_mm=clipped.execution_end_mm;
                        previous.origin.source_end_mm=clipped.source_end_mm;
                        previous.command_end_mm=command_offset+edge_length;
                        command_offset+=edge_length;
                        continue;
                    }
                }
                prepared.command_pieces.push_back({clipped,span_index,i-1,i,command_offset,command_offset+edge_length});
                previous_owner=owner_index;
                command_offset+=edge_length;
            }
        }
        span.geometry = std::move(sampled);
        span.edges = std::move(edges);
        if (span.deposits_fiber()) offset += source_length;
    }
}

// Process boundaries must not create a command edge below coordinate resolution.
// Reuse a nearby interior geometry knot within the existing 2 micron command
// budget instead of replacing a bend by a chord. Endpoints are never moved.
bool split_process_span(const Polyline3& source, double distance, Polyline3& before, Polyline3& after)
{
    double station = 0.0;
    size_t nearest = 0;
    double error = scale_(command_resolution_mm);
    for (size_t i = 1; i + 1 < source.points.size(); ++i) {
        station += (source.points[i] - source.points[i-1]).cast<double>().norm();
        const double delta = std::abs(station-distance);
        if (delta < error) { nearest = i; error = delta; }
        if (station > distance + scale_(command_resolution_mm)) break;
    }
    if (nearest) {
        before.points.assign(source.points.begin(), source.points.begin()+nearest+1);
        after.points.assign(source.points.begin()+nearest, source.points.end());
        return true;
    }
    return source.split_at_length(distance, &before, &after);
}

// Finish is a tool motion after the depositing tail, not another material path.
// Its machine-space limits are checked at execution binding, never against the
// deposition domain (or its contour keepouts) and never using fiber line width.
void append_finish(PreparedFiberPath& prepared, const FiberFinishRoute& finish)
{
    if (finish.geometry.empty()) return;
    prepared.finish_strategy = finish.strategy;
    prepared.spans.push_back({finish.geometry, FiberMotionKind::NonDepositingFinish});
}

} // namespace

FiberFinishRoute make_independent_fiber_finish_route(
    const ExtrusionPath& source, const ContinuousFiberConfig& config,
    const FiberFragmentId& id)
{
    FiberFinishRoute finish;
    finish.source = id;
    const bool tangent = is_rectilinear_fiber_infill(config, id.parent.purpose) ||
        source.polyline.points.size() < 2 || !source.is_closed();
    finish.strategy = tangent ? FiberFinishStrategy::TangentExtension : FiberFinishStrategy::LoopOverlap;
    // Validate before coordinate conversion. Zero requests retain their selected
    // policy and do not start validating the other family's configuration.
    validate_fiber_process_config(config, id.parent.purpose, finish.strategy);
    finish.requested_length_mm = tangent ? config.finish_extension_length_mm : config.finish_overlap_length_mm;
    if (finish.requested_length_mm == 0 || source.polyline.points.size() < 2) return finish;
    if (tangent) {
        const auto& points = source.polyline.points;
        const Vec3d terminal = (points.back()-points[points.size()-2]).cast<double>();
        if (terminal.squaredNorm() == 0) return finish;
        const Vec3d direction = terminal.normalized();
        const long double scaled_length = static_cast<long double>(finish.requested_length_mm) / SCALING_FACTOR;
        if (scaled_length > std::numeric_limits<coord_t>::max())
            throw std::invalid_argument("Fiber finish length cannot be represented in source coordinates");
        const Vec3d endpoint = points.back().cast<double>() + scale_(finish.requested_length_mm)*direction;
        for (int axis = 0; axis < 2; ++axis)
            if (!std::isfinite(endpoint[axis]) ||
                static_cast<long double>(endpoint[axis]) < std::numeric_limits<coord_t>::min() ||
                static_cast<long double>(endpoint[axis]) > std::numeric_limits<coord_t>::max())
                throw std::invalid_argument("Fiber finish endpoint cannot be represented in source coordinates");
        finish.geometry.points = {points.back(), Point3(endpoint.x(), endpoint.y(), 0.0)};
    } else {
        if (finish.requested_length_mm > unscale<double>(source.length())) return finish;
        finish.geometry = source.polyline;
        finish.geometry.clip_end(source.length()-scale_(finish.requested_length_mm));
    }
    return finish;
}

Polyline3 normalize_fiber_geometry(const Polyline3& input, const ExPolygons* centerline_domain)
{
    return normalized_xy(input, nullptr, normalization_error_mm, centerline_domain);
}

Polyline3 normalize_fiber_chain_geometry(const Polyline3& input,
    std::vector<FiberExecutionPiece>& pieces, const ExPolygons* centerline_domain,
    std::vector<double>* original_stations_mm)
{
    if (input.points.size()<2 || pieces.empty())
        throw std::invalid_argument("Fiber chain normalization requires geometry and source pieces");
    const auto keep=piece_knots(input,pieces);
    std::vector<double> old_stations;
    Polyline3 normalized=normalized_xy(input,&old_stations,ContourRoundingOptions{}.chord_tolerance_mm,centerline_domain,&keep);
    const ArcGeometry output(normalized);
    const auto boundary_station=[&](double station) {
        auto next=std::lower_bound(old_stations.begin(),old_stations.end(),station);
        size_t index=next==old_stations.end()?old_stations.size()-1:size_t(next-old_stations.begin());
        if (index && std::abs(old_stations[index-1]-station)<std::abs(old_stations[index]-station)) --index;
        if (std::abs(old_stations[index]-station)>station_roundoff_mm)
            throw std::invalid_argument("Normalized fiber chain lost a protected piece boundary");
        return output.positions[index];
    };
    for (auto& piece:pieces) {
        const FiberExecutionPiece original=piece;
        piece.execution_begin_mm=boundary_station(piece.execution_begin_mm);
        piece.execution_end_mm=boundary_station(piece.execution_end_mm);
        if (piece.kind==FiberExecutionPieceKind::RetainedSource) {
            std::vector<std::pair<double,double>> chart;
            chart.emplace_back(piece.execution_begin_mm,piece.source_begin_mm);
            for (size_t i=0;i<old_stations.size();++i)
                if (output.positions[i]>piece.execution_begin_mm && output.positions[i]<piece.execution_end_mm)
                    chart.emplace_back(output.positions[i],source_station_at(original,old_stations[i]));
            chart.emplace_back(piece.execution_end_mm,piece.source_end_mm);
            piece.source_station_knots=std::make_shared<const std::vector<std::pair<double,double>>>(std::move(chart));
        }
    }
    if (original_stations_mm) *original_stations_mm=std::move(old_stations);
    piece_knots(normalized,pieces);
    return normalized;
}

FiberFinalizationResult FiberPathFinalizer::finalize(
    const ExtrusionPath& input,
    const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,
    const FiberFragmentId& id, const FiberFinishRoute& finish,
    const std::vector<ContourArc>& arcs, const ExPolygons* planned_centerline_domain,
    const std::vector<FiberExecutionPiece>& pieces,
    const std::vector<FiberExecutionPiece>& finish_pieces)
{
    FiberFinalizationResult result;
    // The validator owns source normalization, before geometry checks and
    // interval mapping. Splitting below may introduce short end edges, so each
    // process span still preserves its endpoints when preparing command samples.
    const ExtrusionPath& candidate = input;
    if (candidate.polyline.points.size() < 2 || allowed_domain.empty() ||
        std::any_of(candidate.polyline.points.begin(), candidate.polyline.points.end(),
                    [](const Point3& point) { return point.z() != 0; })) {
        result.failure = FiberFinalizationFailure::InvalidGeometry;
        result.detail = "Fiber finalization requires a normalized planar path and nonempty domain";
        return result;
    }
    result.length_mm = unscale<double>(candidate.length());
    if (!std::isfinite(result.length_mm)) {
        result.failure = FiberFinalizationFailure::InvalidGeometry;
        result.detail = "Non-finite fiber path length";
        return result;
    }
    try {
        validate_fiber_process_config(config, id.parent.purpose, finish.strategy);
        const double requested = finish.strategy == FiberFinishStrategy::None ? 0.0 :
            finish.strategy == FiberFinishStrategy::LoopOverlap ?
                config.finish_overlap_length_mm : config.finish_extension_length_mm;
        if (!std::isfinite(finish.requested_length_mm) || finish.requested_length_mm < 0 ||
            finish.requested_length_mm != requested)
            throw std::invalid_argument("Fiber finish request differs from its selected configuration");
    }
    catch (const std::invalid_argument& error) {
        result.failure = FiberFinalizationFailure::InvalidParameter;
        result.detail = error.what();
        return result;
    }
    const double required_length = std::max(
        config.minimum_path_length_mm,
        config.landing_length_mm + config.start_stabilization_length_mm +
            config.minimum_effective_length_mm + config.cut_to_contact_length_mm);
    if (result.length_mm + EPSILON < required_length) {
        result.failure = FiberFinalizationFailure::TooShort;
        return result;
    }

    auto prepared = std::make_shared<PreparedFiberPath>();
    prepared->id = id;
    prepared->policy_boundary = config.policy_boundary;
    prepared->geometric_mm3_per_mm = candidate.mm3_per_mm;
    prepared->width_mm = candidate.width;
    prepared->height_mm = candidate.height;
    const bool contour = id.parent.purpose == FiberPathPurpose::Contour;
    const unsigned material = contour ? config.contour_material : config.infill_material;
    prepared->logical_filament_id = material > 0 ? material-1 : 0;
    prepared->acceleration_mm_s2 = contour ? config.contour_acceleration_mm_s2 : config.infill_acceleration_mm_s2;
    prepared->start_procedure.prefeed_length_mm =
        config.cut_to_contact_length_mm + config.prefeed_extra_length_mm;
    prepared->start_procedure.prefeed_speed_mm_s = config.prefeed_speed_mm_s;
    prepared->start_procedure.z_hop_height_mm = config.z_hop_height_mm;
    prepared->start_procedure.landing_speed_mm_s = config.landing_speed_mm_s;
    prepared->start_procedure.start_speed_mm_s = config.start_speed_mm_s;
    prepared->start_procedure.adhesion_dwell_ms = config.adhesion_dwell_ms;

    const double tail_scaled = scale_(config.cut_to_contact_length_mm);
    Polyline3 before_tail = candidate.polyline;
    Polyline3 passive;
    if (tail_scaled > 0.0) {
        const double split_distance = candidate.length() - tail_scaled;
        if (split_distance <= 0.0 ||
            !split_process_span(candidate.polyline, split_distance, before_tail, passive) ||
            before_tail.points.size() < 2 || passive.points.size() < 2) {
            result.failure = FiberFinalizationFailure::TooShort;
            return result;
        }
    }

    Polyline3 after_landing = std::move(before_tail);
    const double landing_scaled = scale_(config.landing_length_mm);
    if (landing_scaled > 0.0) {
        Polyline3 landing;
        Polyline3 remaining;
        if (!split_process_span(after_landing, landing_scaled, landing, remaining) ||
            landing.points.size() < 2 || remaining.points.size() < 2) {
            result.failure = FiberFinalizationFailure::TooShort;
            return result;
        }
        prepared->spans.push_back({std::move(landing), FiberMotionKind::PrefedLanding});
        after_landing = std::move(remaining);
    }

    const double stabilization_scaled = scale_(config.start_stabilization_length_mm);
    if (stabilization_scaled > 0.0) {
        Polyline3 powered_start;
        Polyline3 remaining;
        if (!split_process_span(after_landing, stabilization_scaled, powered_start, remaining) ||
            powered_start.points.size() < 2 || remaining.points.size() < 2) {
            result.failure = FiberFinalizationFailure::TooShort;
            return result;
        }
        prepared->spans.push_back({std::move(powered_start), FiberMotionKind::PoweredStart});
        after_landing = std::move(remaining);
    }

    if (after_landing.points.size() < 2) {
        result.failure = FiberFinalizationFailure::TooShort;
        return result;
    }
    prepared->spans.push_back({std::move(after_landing), FiberMotionKind::PoweredDepositing});

    if (!passive.empty())
        prepared->spans.push_back({std::move(passive), FiberMotionKind::PassiveDepositingAfterCut});

    const bool finish_valid = finish.source.parent.purpose == id.parent.purpose &&
        finish.source.parent.domain == id.parent.domain &&
        (finish.requested_length_mm == 0 ? finish.geometry.empty() :
            finish.geometry.points.size() >= 2 &&
            finish.geometry.points.front() == candidate.polyline.points.back() &&
            std::none_of(finish.geometry.points.begin(), finish.geometry.points.end(),
                [](const Point3& point) { return point.z() != 0; }) &&
            std::abs(unscale<double>(finish.geometry.length())-finish.requested_length_mm) <= command_resolution_mm);
    if (!finish_valid) {
        result.failure = FiberFinalizationFailure::FinishUnavailable;
        result.detail = "Fiber finish route has invalid source, endpoint, length or LayerXY geometry";
        return result;
    }
    if (is_rectilinear_fiber_infill(config, id.parent.purpose)) {
        // Independent infill must use its own final tangent, even when a caller
        // supplies an explicit route. Finish never extends the depositing path
        // used above to locate CUT at candidate.length() - cutter-to-contact.
        const auto expected = make_independent_fiber_finish_route(candidate, config, id);
        if (!(finish.source == id) || finish.strategy != expected.strategy ||
            finish.geometry.points != expected.geometry.points) {
            result.failure = FiberFinalizationFailure::FinishUnavailable;
            result.detail = "Rectilinear fiber finish differs from its source tangent";
            return result;
        }
        // append_finish() does not append a span for a zero-length request.
        prepared->finish_strategy = finish.strategy;
    }
    append_finish(*prepared, finish);
    if (!pieces.empty()) prepared->finish_strategy=finish.strategy;
    const bool check_centerline = planned_centerline_domain || !arcs.empty();
    const ExPolygons centerline_domain = planned_centerline_domain ?
        offset_ex(*planned_centerline_domain, float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm))) :
        check_centerline ? offset_ex(allowed_domain, -float(scale_(0.5*candidate.width +
            (contour ? config.contour_boundary_clearance_mm : 0.0) - ContourRoundingOptions{}.geometry_tolerance_mm))) : ExPolygons{};
    try {
        plan_edges(*prepared, candidate.polyline, config, arcs, check_centerline ? &centerline_domain : nullptr,
                   pieces,finish.geometry,finish_pieces);
    } catch (const std::length_error& error) {
        result.failure = FiberFinalizationFailure::SamplingLimit;
        result.detail = error.what();
        return result;
    } catch (const std::invalid_argument& error) {
        result.failure = FiberFinalizationFailure::InvalidGeometry;
        result.detail = error.what();
        return result;
    }
    try {
        if (is_rectilinear_fiber_infill(config, id.parent.purpose)) {
            const auto last_depositing = std::find_if(prepared->spans.rbegin(), prepared->spans.rend(),
                [](const auto& span) { return span.deposits_fiber(); });
            if (last_depositing == prepared->spans.rend() ||
                last_depositing->geometry.points.back() != candidate.polyline.points.back())
                throw std::invalid_argument("Rectilinear fiber deposition must end at its source endpoint");
        }
        prepared->finalize_actions();
        prepared->validate();
    }
    catch (const std::invalid_argument& error) {
        result.detail = error.what();
        result.failure = FiberFinalizationFailure::InvalidGeometry;
        return result;
    }

    if (check_centerline) {
        // Validate the actual command geometry, including the boundary clearance.
        for (const auto& span:prepared->spans) if (span.deposits_fiber() &&
            !diff_pl(Polylines{span.geometry.to_polyline()},centerline_domain).empty()) {
            result.failure=FiberFinalizationFailure::OutsideDomain;
            result.detail="Fiber path left its planned centerline domain during command preparation";
            return result;
        }
    }
    Polygons physical;
    Polygons keepout;
    {
        // All fiber uses the circular material envelope used to construct its
        // centerline domain, including infill without explicit return arcs.
        // Process boundaries do not introduce physical end caps in a continuous
        // strand. Sweep connected deposition once, including the cyclic seam.
        Polyline depositing;
        const auto flush = [&] {
            const double physical_radius = 0.5 * candidate.width;
            const double keepout_radius = physical_radius + config.contour_infill_clearance_mm;
            auto footprint = to_polygons(fiber_contour_coverage(depositing, physical_radius));
            if (contour) append(keepout, keepout_radius == physical_radius ? footprint :
                to_polygons(fiber_contour_coverage(depositing, keepout_radius)));
            append(physical, std::move(footprint));
            depositing.points.clear();
        };
        for (const auto& span : prepared->spans) if (span.deposits_fiber()) {
            const auto points = span.geometry.to_polyline().points;
            if (points.empty()) continue;
            if (!depositing.points.empty() && depositing.points.back() != points.front()) flush();
            depositing.points.insert(depositing.points.end(),
                points.begin() + (depositing.points.empty() ? 0 : 1), points.end());
        }
        flush();
    }

    prepared->physical_coverage = union_ex(physical);
    if (config.enforce_policy_boundary && config.policy_boundary &&
        !config.policy_boundary->forbidden_core.empty()) {
        // Only a foreign process domain adds a mask boundary. Preserve the
        // existing model-edge tolerance below; allow four grids of clip noise.
        const auto numeric_foreign = offset_ex(config.policy_boundary->forbidden_core, -4.0f);
        if (!intersection_ex(prepared->physical_coverage, numeric_foreign, ApplySafetyOffset::No).empty()) {
            result.failure = FiberFinalizationFailure::PolicyBoundaryRejected;
            result.detail = "Depositing footprint crosses its fiber policy boundary";
            return result;
        }
    }
    prepared->outside_domain = diff_ex(prepared->physical_coverage, allowed_domain, ApplySafetyOffset::Yes);
    if (area_mm2(prepared->outside_domain) > config.outside_tolerance_mm2) {
        BOOST_LOG_TRIVIAL(debug) << "[FiberCoverageRejected] layer=" << prepared->id.parent.domain.layer_id
            << " outside_mm2=" << area_mm2(prepared->outside_domain);
        result.failure = FiberFinalizationFailure::OutsideDomain;
        return result;
    }

    // Resin may only occupy material not actually deposited by fiber. Shrinking
    // individual strands creates artificial channels between touching strands.
    // Keep the existing field as the material-allocation result, not a second
    // (overlap-reduced) representation of strand width.
    prepared->resin_exclusion = intersection_ex(prepared->physical_coverage, allowed_domain);
    if (contour)
        prepared->contour_to_infill_keepout = intersection_ex(union_ex(keepout), allowed_domain, ApplySafetyOffset::Yes);
    result.prepared = std::move(prepared);
    return result;
}

} // namespace Slic3r
