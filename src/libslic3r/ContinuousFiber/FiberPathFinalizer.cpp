#include "FiberInfillValidationDomains.hpp"
#include "FiberPathFinalizer.hpp"
#include "FiberDomainIndex.hpp"
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
    std::vector<PathFittingData> primitives;
    std::vector<double> ends;
    explicit ArcGeometry(const Polyline3& line) : geometry(line)
    {
        positions=Geometry::stations_with_arcs_mm(line);
        const auto add = [&](const PathFittingData& record) {
            primitives.push_back(record);
            ends.push_back(positions[record.end_point_index]);
        };
        if (line.fitting_result.empty()) {
            for (size_t i=1;i<line.points.size();++i)
                add({i-1,i,EMovePathType::Linear_move,{}});
        } else for (const auto& record:line.fitting_result) {
            if (record.path_type==EMovePathType::Arc_move_cw || record.path_type==EMovePathType::Arc_move_ccw)
                add(record);
            else for (size_t i=record.start_point_index+1;i<=record.end_point_index;++i)
                add({i-1,i,EMovePathType::Linear_move,{}});
        }
    }
    size_t index_at(double s, bool incoming) const
    {
        const auto it = incoming ? std::lower_bound(ends.begin(),ends.end(),s) :
                                   std::upper_bound(ends.begin(),ends.end(),s);
        return std::min(size_t(it-ends.begin()),primitives.size()-1);
    }
    Polyline3 primitive(size_t index) const
    {
        auto record=primitives[index];
        Polyline3 result;
        result.points={geometry.points[record.start_point_index],geometry.points[record.end_point_index]};
        record.start_point_index=0;record.end_point_index=1;
        result.fitting_result.push_back(std::move(record));
        return result;
    }
    double begin(size_t index) const {return index==0?0.0:ends[index-1];}
    Point3 at(double s) const
    {
        if (primitives.empty()) return geometry.points.front();
        const size_t index=index_at(s,true);
        if (s==begin(index)) return geometry.points[primitives[index].start_point_index];
        if (s==ends[index]) return geometry.points[primitives[index].end_point_index];
        return Geometry::point_at_length_with_arcs_mm(primitive(index),s-begin(index));
    }
    Vec2d tangent(double s, bool incoming=false) const
    {
        if (primitives.empty()) return Vec2d::Zero();
        const size_t index=index_at(s,incoming);
        return Geometry::tangent_at_length_with_arcs_mm(primitive(index),s-begin(index),incoming);
    }
    Polyline3 slice(double from, double to, size_t& cursor) const
    {
        while (cursor+1<ends.size() && ends[cursor]<=from) ++cursor;
        Polyline3 result;
        // Cut deduplication can leave a tiny overlap with an adjacent primitive
        // whose rounded endpoints coincide. Apply the original slice semantics
        // locally, including dropping that zero-command fragment.
        for (;cursor<primitives.size();++cursor) {
            const double a=std::max(from,begin(cursor)),b=std::min(to,ends[cursor]);
            if (b>a) {
                const auto local=primitive(cursor);
                const double end=b==ends[cursor]?Geometry::length_with_arcs_mm(local):b-begin(cursor);
                const auto part=Geometry::slice_with_arcs_mm(local,a-begin(cursor),end);
                if (part.points.size()>1) Geometry::append_with_arcs(result,part);
            }
            if (ends[cursor]>=to) break;
        }
        if (result.points.empty()) result.points.push_back(at(from));
        return result;
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
                return previous = arc.positions[*next];
            }
        }
        const double station=Geometry::project_with_arcs_mm(geometry,point,previous);
        if (unscale<double>((Geometry::point_at_length_with_arcs_mm(geometry,station)-point).cast<double>().norm())
                <=ContourRoundingOptions{}.geometry_tolerance_mm) return previous=station;
        throw std::invalid_argument("Fiber command point has no ordered planning station");
    }
};

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
        : length(Geometry::length_with_arcs_mm(path)), minimum(vmin), maximum(vmax), transition(distance),
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
            const Vec2d incoming=arc.tangent(i==0?length:arc.positions[i],true);
            const Vec2d outgoing=arc.tangent(arc.positions[i]);
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

void plan_edges(FiberPathDraft& prepared, const Polyline3& depositing, const ContinuousFiberConfig& config,
                const std::vector<ContourArc>& arcs,
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
    double offset=prepared.entry.lowers()?prepared.entry.end_station_mm:0.0;
    double command_offset=0;
    for(size_t span_index=0;span_index<prepared.spans.size();++span_index) {
        auto& span=prepared.spans[span_index];
        const auto& origins=span.deposits_fiber()?pieces:finish_pieces;
        const double length=Geometry::length_with_arcs_mm(span.geometry);
        span.geometry=Geometry::slice_with_arcs_mm(span.geometry,0,length);
        const ArcGeometry arc(span.geometry);
        std::vector<double> cuts=arc.positions;
        const bool tail=span.kind==FiberMotionKind::PassiveDepositingAfterCut;
        const size_t cells=tail?std::max(size_t(2),size_t(std::ceil(length/config.tail_speed_step_length_mm))):0;
        if(cells>1000000 || length/config.speed_sampling_length_mm>1000000)
            throw std::length_error("Fiber speed interval budget exceeded");
        const auto add=[&](double s) {
            if(s<=command_resolution_mm || s>=length-command_resolution_mm) return;
            const auto next=std::lower_bound(arc.positions.begin(),arc.positions.end(),s);
            if ((next!=arc.positions.end() && std::abs(*next-s)<=command_resolution_mm) ||
                (next!=arc.positions.begin() && std::abs(*(next-1)-s)<=command_resolution_mm)) return;
            cuts.push_back(s);
        };
        if(tail) for(size_t i=1;i<cells;++i) add(length*double(i)/cells);
        else if(span.actively_feeds_fiber())
            for(double s=(std::floor(offset/config.speed_sampling_length_mm)+1)*config.speed_sampling_length_mm;
                s<offset+length;s+=config.speed_sampling_length_mm) add(s-offset);
        // Source boundaries are exact process knots, even inside an arc.
        for(const auto& origin:origins)
            for(double v:{origin.execution_begin_mm,origin.execution_end_mm})
                if(v>offset+1e-7 && v<offset+length-1e-7) cuts.push_back(v-offset);
        std::sort(cuts.begin(),cuts.end());
        cuts.erase(std::unique(cuts.begin(),cuts.end(),[](double a,double b){return std::abs(a-b)<1e-7;}),cuts.end());
        Polyline3 planned;std::vector<FiberEdgeProcess> edges;
        size_t previous_owner=origins.size();
        std::vector<double> global;
        for(double cut:cuts) {
            const Point3 point=arc.at(cut);
            double station=origins.empty()?offset+cut:
                (span.deposits_fiber()?deposition_map->at(point):finish_map->at(point));
            for(const auto& piece:origins)for(double boundary:{piece.execution_begin_mm,piece.execution_end_mm})
                if(std::abs(station-boundary)<4*std::sqrt(2.)*SCALING_FACTOR)station=boundary;
            if(global.empty())station=span.deposits_fiber()?offset:0.0;
            global.push_back(station);
        }
        size_t primitive_cursor=0;
        for(size_t i=1;i<cuts.size();++i) {
            auto part=arc.slice(cuts[i-1],cuts[i],primitive_cursor);
            if(part.points.size()!=2 || part.fitting_result.size()!=1)
                throw std::invalid_argument("Fiber interval does not own exactly one primitive");
            if (planned.points.empty()) planned.points.push_back(part.points.front());
            if (planned.points.back()!=part.points.front())
                throw std::invalid_argument("Fiber planned primitive has a gap");
            auto record=part.fitting_result.front();
            record.start_point_index=planned.points.size()-1;
            record.end_point_index=planned.points.size();
            planned.points.push_back(part.points.back());
            planned.fitting_result.push_back(std::move(record));
            double speed=config.finish_motion_speed_mm_s;
            if(span.actively_feeds_fiber()) {
                speed=speeds.limit(global[i-1],global[i]);
                if(span.kind==FiberMotionKind::PoweredStart) speed=std::min(speed,config.start_speed_mm_s);
                if(span.kind==FiberMotionKind::PoweredEntry) speed=std::min(speed,config.start_speed_mm_s);
            } else if(tail) {
                const size_t cell=std::min(cells-1,size_t((cuts[i-1]+cuts[i])*0.5/length*cells));
                speed=config.tail_min_speed_mm_s+(config.tail_max_speed_mm_s-config.tail_min_speed_mm_s)*double(cell)/double(cells-1);
                speed=std::min(speed,tail_limits.limit(global[i-1],global[i]));
            }
            edges.push_back({speed,span.actively_feeds_fiber()?ratio:0.0});
            if(!origins.empty()) {
                const double middle=(global[i-1]+global[i])*0.5;
                const auto owner=std::find_if(origins.begin(),origins.end(),[&](const auto& piece){
                    return middle>=piece.execution_begin_mm && middle<piece.execution_end_mm;});
                if(owner==origins.end() || global[i-1]<owner->execution_begin_mm-1e-5 || global[i]>owner->execution_end_mm+1e-5)
                    throw std::invalid_argument("Fiber primitive crosses a source boundary");
                const auto clipped=clipped_piece(*owner,std::max(global[i-1],owner->execution_begin_mm),
                    std::min(global[i],owner->execution_end_mm));
                const size_t owner_index=size_t(owner-origins.begin());
                const double command_length=Geometry::length_with_arcs_mm(part);
                // Provenance groups consecutive commands of the same source;
                // every primitive still retains its own geometry, speed and E.
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
                        previous.command_end_mm=command_offset+command_length;
                        command_offset+=command_length;
                        continue;
                    }
                }
                prepared.command_pieces.push_back({clipped,span_index,i-1,i,command_offset,command_offset+command_length});
                previous_owner=owner_index;
                command_offset+=command_length;
            }
        }
        Geometry::validate_with_arcs(planned);
        span.geometry=std::move(planned);span.edges=std::move(edges);
        if(span.deposits_fiber()) offset=global.back();
    }
}

// The generic Polyline split treats a short positive prefix as zero and uses
// approximate vertex lookup. Entry needs an ordered exact source split, even
// when that positive prefix will round to a pure-Z machine command.
bool split_entry_span(const Polyline3& source,double distance,Polyline3& before,Polyline3& after)
{
    return Geometry::split_at_length_with_arcs_mm(source,unscale<double>(distance),before,after);
}

bool split_process_span(const Polyline3& source,double distance,Polyline3& before,Polyline3& after)
{
    return split_entry_span(source,distance,before,after);
}

// Finish is a tool motion after the depositing tail, not another material path.
// Its machine-space limits are checked at execution binding, never against the
// deposition domain (or its contour keepouts) and never using fiber line width.
void append_finish(FiberPathDraft& prepared, const FiberFinishRoute& finish)
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
        const Vec2d tangent=Geometry::tangent_at_length_with_arcs_mm(source.polyline,Geometry::length_with_arcs_mm(source.polyline),true);
        const Vec3d terminal(tangent.x(),tangent.y(),0.0);
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
        if (finish.requested_length_mm > Geometry::length_with_arcs_mm(source.polyline)) return finish;
        finish.geometry=Geometry::slice_with_arcs_mm(source.polyline,0,finish.requested_length_mm);
    }
    return finish;
}

Polyline3 normalize_fiber_geometry(const Polyline3& input, const ExPolygons* centerline_domain)
{
    if(!input.fitting_result.empty()) {
        Geometry::validate_with_arcs(input);
        if(std::any_of(input.fitting_result.begin(),input.fitting_result.end(),[](const auto& p) {
            return p.path_type!=EMovePathType::Linear_move;
        })) return input;
    }
    return normalized_xy(input,nullptr,ContourRoundingOptions{}.chord_tolerance_mm,centerline_domain);
}

Polyline3 normalize_fiber_chain_geometry(const Polyline3& input,
    std::vector<FiberExecutionPiece>& pieces, const ExPolygons* centerline_domain,
    std::vector<double>* original_stations_mm)
{
    if (input.points.size()<2 || pieces.empty())
        throw std::invalid_argument("Fiber chain normalization requires geometry and source pieces");
    if(!input.fitting_result.empty()) {
        Geometry::validate_with_arcs(input);
        if(original_stations_mm) *original_stations_mm=Geometry::stations_with_arcs_mm(input);
        return input;
    }
    auto keep=piece_knots(input,pieces);
    if (keep.size()>1) keep[1]=true; // final chain entry is a protected source edge
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

bool continuous_fiber_detail::FiberFinalizationDomains::band_matches(
    const ExPolygons* current,const FiberCandidateId& source,double width) const
{
    return is_lazy_band_context() && band_source && *band_source==source &&
        planned_source==current && width==band_width_mm && SCALING_FACTOR==band_sigma &&
        source.purpose==FiberPathPurpose::Contour && std::isfinite(proof_radius_grid) &&
        proof_radius_grid>=0 && proof_radius_grid<=ClipperLib::loRange &&
        std::floor(proof_radius_grid)==proof_radius_grid &&
        native_query_delta_grid==double(float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm))) &&
        proof_radius_grid==native_query_delta_grid/2;
}
const ExPolygons& continuous_fiber_detail::FiberFinalizationDomains::comparison_domain() const
{
    if(!comparison_source){if(!comparison_resolver)throw std::logic_error("Missing real fiber comparison domain");comparison_source=&comparison_resolver();}
    return *comparison_source;
}
const continuous_fiber_detail::FiberDomainIndex& continuous_fiber_detail::FiberFinalizationDomains::planned_index() const
{
    if(!planned_index_storage)planned_index_storage=std::make_shared<const FiberDomainIndex>(*planned_source);
    return *planned_index_storage;
}
bool continuous_fiber_detail::FiberFinalizationDomains::proves_planned_band(const Polyline& path) const
{
    return band_source && band_matches(planned_source,*band_source,band_width_mm) &&
        planned_index().proves_sample_band(path,proof_radius_grid);
}
const ExPolygons& continuous_fiber_detail::FiberFinalizationDomains::arc_query_domain() const
{
    if(!arc_query_storage)arc_query_storage=offset_ex(comparison_domain(),
        float(scale_(ContourRoundingOptions{}.chord_tolerance_mm)));
    return *arc_query_storage;
}
const continuous_fiber_detail::FiberDomainIndex& continuous_fiber_detail::FiberFinalizationDomains::comparison_index() const
{
    if(!comparison_index_storage)comparison_index_storage=std::make_shared<const FiberDomainIndex>(comparison_domain());
    return *comparison_index_storage;
}
const continuous_fiber_detail::FiberDomainIndex& continuous_fiber_detail::FiberFinalizationDomains::arc_query_index() const
{
    if(!arc_query_index_storage)arc_query_index_storage=std::make_shared<const FiberDomainIndex>(arc_query_domain());
    return *arc_query_index_storage;
}

FiberFinalizationResult FiberPathFinalizer::finalize(
    const ExtrusionPath& input,
    const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,
    const FiberFragmentId& id, const FiberFinishRoute& finish,
    const std::vector<ContourArc>& arcs, const ExPolygons* planned_centerline_domain,
    const std::vector<FiberExecutionPiece>& pieces,
    const std::vector<FiberExecutionPiece>& finish_pieces,
    const continuous_fiber_detail::FiberFinalizationDomains* domains,
    const continuous_fiber_detail::FiberInfillFinalizationContext* material_context)
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
    result.length_mm = Geometry::length_with_arcs_mm(candidate.polyline);
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
    // Entry owns a continuous route, independent of source tessellation and Z height.
    const Point3 first=candidate.polyline.points.front();
    const double entry_length=config.z_hop_height_mm>0?config.landing_length_mm:0.0;
    const double passive_begin=result.length_mm-config.cut_to_contact_length_mm;
    const double powered_begin=config.z_hop_height_mm>0 ? entry_length : 0.0;
    const double required_powered=config.start_stabilization_length_mm+config.minimum_effective_length_mm;
    if (result.length_mm+EPSILON<config.minimum_path_length_mm ||
        passive_begin<=powered_begin+EPSILON || passive_begin-powered_begin+EPSILON<required_powered) {
        result.failure=FiberFinalizationFailure::TooShort;
        result.detail="Entry and active process budget must finish before CUT";
        return result;
    }

    // New material-certified infill has already proved its unchanged source
    // against post-contour M. Only an eligible process needs the real legacy
    // C -> comparison+100nm -> arc-query+50nm certification domains.
    if (material_context) {
        const auto& resolved=material_context->resolve();
        planned_centerline_domain=resolved.planned_source;
        domains=&resolved;
    }

    auto prepared = std::make_unique<FiberPathDraft>();
    prepared->id = id;
    prepared->execution_deviation_mm=4*ContourRoundingOptions{}.chord_tolerance_mm+8*std::sqrt(2.)*SCALING_FACTOR;
    prepared->policy_boundary = config.policy_boundary ? std::make_shared<const FiberPolicyBoundary>(*config.policy_boundary) : nullptr;
    prepared->allowed_domain = allowed_domain;
    prepared->outside_tolerance_mm2 = config.outside_tolerance_mm2;
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
        const double split_distance = scale_(result.length_mm) - tail_scaled;
        if (split_distance <= 0.0 ||
            !split_process_span(candidate.polyline, split_distance, before_tail, passive) ||
            before_tail.points.size() < 2 || passive.points.size() < 2) {
            result.failure = FiberFinalizationFailure::TooShort;
            return result;
        }
    }

    prepared->entry.start=first;
    prepared->entry.contact=first;
    prepared->entry.geometry.points={first};
    prepared->entry.end_station_mm=entry_length;
    prepared->entry.mode=config.z_hop_height_mm==0 ? FiberEntryMode::PoweredFirstEdge :
        entry_length>0 ? FiberEntryMode::PathLanding : FiberEntryMode::VerticalLanding;
    Polyline3 after_landing = std::move(before_tail);
    double consumed_stabilization=0.0;
    double process_entry_length=entry_length;
    if(config.z_hop_height_mm==0 && candidate.polyline.points[0]!=candidate.polyline.points[1]) {
        const auto primitives=Geometry::slice_with_arcs_mm(candidate.polyline,0,result.length_mm);
        const auto stations=Geometry::stations_with_arcs_mm(primitives);
        if(stations.size()>1)process_entry_length=std::min(stations[1],passive_begin);
    }
    if (process_entry_length>0) {
        Polyline3 entry, remaining;
        // Unlike command-budget splits, the old first-edge endpoint is exact:
        // never snap it onto a different geometry knot.
        const bool whole=config.z_hop_height_mm==0 && std::abs(Geometry::length_with_arcs_mm(after_landing)-process_entry_length)<EPSILON;
        if (whole) entry=after_landing;
        if ((!whole && !split_entry_span(after_landing,scale_(process_entry_length),entry,remaining)) ||
            entry.points.size()<2 || (!whole && remaining.points.size()<2)) {
            result.failure=FiberFinalizationFailure::TooShort;
            return result;
        }
        if(config.z_hop_height_mm>0) {
            prepared->entry.contact=entry.points.back();
            prepared->entry.geometry=entry;
        }
        if (config.z_hop_height_mm==0) {
            prepared->spans.push_back({std::move(entry),FiberMotionKind::PoweredEntry});
            consumed_stabilization=process_entry_length;
        } else if (!pieces.empty()) {
            for (const auto& piece:pieces) {
                const double end=std::min(entry_length,piece.execution_end_mm);
                if (end>piece.execution_begin_mm)
                    prepared->entry.pieces.push_back(clipped_piece(piece,piece.execution_begin_mm,end));
                if (piece.execution_end_mm>=entry_length) break;
            }
        }
        after_landing=std::move(remaining);
    }

    const double stabilization_scaled=scale_(std::max(0.0,config.start_stabilization_length_mm-consumed_stabilization));
    if (stabilization_scaled>0) {
        Polyline3 powered_start, remaining;
        if (!split_process_span(after_landing,stabilization_scaled,powered_start,remaining) ||
            powered_start.points.size()<2 || remaining.points.size()<2) {
            result.failure=FiberFinalizationFailure::TooShort;
            return result;
        }
        prepared->spans.push_back({std::move(powered_start),FiberMotionKind::PoweredStart});
        after_landing=std::move(remaining);
    }

    if (after_landing.points.size()>=2)
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
            std::abs(Geometry::length_with_arcs_mm(finish.geometry)-finish.requested_length_mm) <= command_resolution_mm);
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
    // A mismatched lazy context is unusable for ALL cached/resolved domains,
    // not merely for its band proof. Current owned/native domains take over.
    const bool lazy_context_valid=!domains || !domains->is_lazy_band_context() ||
        (id.parent.purpose==FiberPathPurpose::Contour && config.contour_bend_radius_mm>0 &&
         domains->band_matches(planned_centerline_domain,id.parent,candidate.width));
    const bool reuse_domains=domains && lazy_context_valid && domains->planned_source==planned_centerline_domain &&
        (is_rectilinear_fiber_infill(config,id.parent.purpose) ||
         (id.parent.purpose==FiberPathPurpose::Contour && config.contour_bend_radius_mm>0));
    const ExPolygons owned_centerline_domain = reuse_domains ? ExPolygons{} : planned_centerline_domain ?
        offset_ex(*planned_centerline_domain, float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm))) :
        check_centerline ? offset_ex(allowed_domain, -float(scale_(0.5*candidate.width +
            (contour ? config.contour_boundary_clearance_mm : 0.0) - ContourRoundingOptions{}.geometry_tolerance_mm))) : ExPolygons{};
    const auto comparison_domain=[&]() -> const ExPolygons& {
        return reuse_domains?domains->comparison_domain():owned_centerline_domain;
    };
    try {
        plan_edges(*prepared, candidate.polyline, config, arcs,
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

    // A strict certificate in the actual comparison domain is sufficient on
    // its own. Only an unproved curved route needs the original extra chord
    // query budget; material, policy and physical domains remain unchanged.
    std::optional<ExPolygons> owned_arc_query_storage;
    std::unique_ptr<continuous_fiber_detail::FiberDomainIndex> owned_comparison_index,owned_arc_query_index;
    const auto centerline_inside=[&](const Polyline3& route) {
        const bool curved=std::any_of(route.fitting_result.begin(),route.fitting_result.end(),[](const auto& p) {
            return p.path_type!=EMovePathType::Linear_move;
        });
        const Polyline sampled=Geometry::sample_with_arcs(route,ContourRoundingOptions{}.chord_tolerance_mm).to_polyline();
        if(reuse_domains && domains->is_lazy_band_context() && domains->proves_planned_band(sampled))return true;
        const ExPolygons& centerline_domain=comparison_domain();
        const continuous_fiber_detail::FiberDomainIndex* base_index;
        if(reuse_domains) base_index=&domains->comparison_index();
        else {
            if(!owned_comparison_index) owned_comparison_index=
                std::make_unique<continuous_fiber_detail::FiberDomainIndex>(centerline_domain);
            base_index=owned_comparison_index.get();
        }
        if(base_index->proves_strict_containment(sampled)) return true;
        if(!curved) return diff_pl(Polylines{sampled},centerline_domain).empty();

        if(!reuse_domains && !owned_arc_query_storage) owned_arc_query_storage=offset_ex(centerline_domain,
            float(scale_(ContourRoundingOptions{}.chord_tolerance_mm)));
        const ExPolygons& arc_query_domain=reuse_domains?domains->arc_query_domain():*owned_arc_query_storage;
        const continuous_fiber_detail::FiberDomainIndex* arc_index;
        if(reuse_domains) arc_index=&domains->arc_query_index();
        else {
            if(!owned_arc_query_index) owned_arc_query_index=
                std::make_unique<continuous_fiber_detail::FiberDomainIndex>(arc_query_domain);
            arc_index=owned_arc_query_index.get();
        }
        if(arc_index->proves_strict_containment(sampled)) return true;
        return diff_pl(Polylines{sampled},arc_query_domain).empty();
    };
    if (check_centerline && prepared->entry.lowers() && prepared->entry.end_station_mm>0 &&
        !centerline_inside(prepared->entry.geometry)) {
        result.failure=FiberFinalizationFailure::OutsideDomain;
        result.detail="Entry leaves its planned centerline domain";
        return result;
    }
    if (check_centerline) {
        // Validate the actual command geometry, including the boundary clearance.
        for (const auto& span:prepared->spans) if (span.deposits_fiber() &&
            !centerline_inside(span.geometry)) {
            result.failure=FiberFinalizationFailure::OutsideDomain;
            result.detail="Fiber path left its planned centerline domain during command preparation";
            return result;
        }
    }
    prepared->requested_passive_length_mm=config.cut_to_contact_length_mm;
    // A sum of endpoint representation errors, separate from shape/coverage
    // tolerance. Both source and emitted coordinates use the six-decimal source grid.
    size_t passive_primitives=0;
    for(const auto& span:prepared->spans)if(span.kind==FiberMotionKind::PassiveDepositingAfterCut)
        passive_primitives+=span.edges.size();
    prepared->passive_length_error_mm=std::min(ContourRoundingOptions{}.geometry_tolerance_mm,
        4*std::sqrt(2.)*SCALING_FACTOR*std::max(size_t(1),passive_primitives));
    prepared->geometry_context=config.geometry_context;
    Polyline reference;
    const auto append_reference=[&](const Polyline3& route,double tolerance,size_t group) {
        const auto points=Geometry::sample_with_arcs(route,tolerance).to_polyline().points;
        for(const auto& point:points)
            if(reference.points.empty() || reference.points.back()!=point)reference.points.push_back(point);
        if(!reference.points.empty())prepared->reference_groups.emplace_back(reference.points.size()-1,group);
    };
    if(prepared->entry.lowers())append_reference(prepared->entry.geometry,ContourRoundingOptions{}.chord_tolerance_mm,0);
    for(size_t i=0;i<prepared->spans.size();++i)if(prepared->spans[i].deposits_fiber())
        append_reference(prepared->spans[i].geometry,fiber_reference_chord_tolerance(*prepared,prepared->spans[i]),i+1);
    prepared->reference_deposition=std::make_shared<const Polyline>(std::move(reference));
    // Display/source occupancy uses the original chord schedule. The material
    // contract below uses the frozen execution reference and its own bounds;
    // do not send the denser passive representation through a second offset.
    Polyline3 display;
    if(prepared->entry.lowers())display=prepared->entry.geometry;
    for(const auto& span:prepared->spans)if(span.deposits_fiber())Geometry::append_with_arcs(display,span.geometry);
    const auto display_line=Geometry::sample_with_arcs(display,ContourRoundingOptions{}.chord_tolerance_mm).to_polyline();
    prepared->physical_coverage=fiber_contour_coverage(display_line,0.5*candidate.width);
    auto bounds=fiber_geometry_coverage_bounds(*prepared);
    prepared->guaranteed_components=std::move(bounds.first);
    prepared->possible_coverage=std::move(bounds.second);
    // Material uncertainty stays inside the existing geometry-normalization
    // contract; it is recorded explicitly in the coverage ledger below.
    if(prepared->execution_deviation_mm>normalization_error_mm)
        throw std::invalid_argument("Fiber material uncertainty exceeds its geometry budget");
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
    prepared->resin_exclusion = intersection_ex(prepared->possible_coverage, allowed_domain);
    ExPolygons contour_occupancy,contour_keepout;
    const bool same_contour_envelope=config.contour_infill_clearance_mm==0;
    for(const auto& run:fiber_geometry_contour_runs(*prepared)) {
        append(contour_occupancy,fiber_contour_coverage(run,0.5*candidate.width));
        if(!same_contour_envelope)
            append(contour_keepout,fiber_contour_coverage(run,0.5*candidate.width+config.contour_infill_clearance_mm));
    }
    const ExPolygons contour_envelope=fiber_material_offset(union_ex(contour_occupancy),prepared->execution_deviation_mm);
    prepared->contour_resin_exclusion=intersection_ex(contour_envelope,prepared->resin_exclusion);
    // The two final clips have different domains/safety policies. Only their
    // identical pre-clip envelope can be shared when no clearance was requested.
    if(same_contour_envelope)
        prepared->contour_to_infill_keepout=intersection_ex(contour_envelope,allowed_domain,ApplySafetyOffset::Yes);
    else
        prepared->contour_to_infill_keepout=intersection_ex(
            fiber_material_offset(union_ex(contour_keepout),prepared->execution_deviation_mm),
            allowed_domain,ApplySafetyOffset::Yes);
    Polyline3 legacy;
    if(prepared->entry.lowers())legacy=prepared->entry.geometry;
    for(const auto& span:prepared->spans)Geometry::append_with_arcs(legacy,span.geometry);
    prepared->expected_legacy_view=std::make_shared<const Polyline3>(Geometry::sample_with_arcs(legacy,0.00005));
    result.prepared = std::shared_ptr<const PreparedFiberPath>(new PreparedFiberPath(std::move(*prepared)));
    return result;
}

} // namespace Slic3r
