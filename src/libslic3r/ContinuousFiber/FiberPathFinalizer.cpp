#include "FiberPathFinalizer.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntity.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
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

void append_coverage(const ExtrusionPath& source, const FiberMotionSpan& span, Polygons& physical, Polygons& exclusion,
                     Polygons& keepout, const ContinuousFiberConfig& config, bool contour)
{
    if (!span.deposits_fiber() || span.geometry.points.size() < 2)
        return;

    ExtrusionPath path(span.geometry, source);
    path.polygons_covered_by_width(physical, 0.0f);
    path.polygons_covered_by_width(exclusion, -float(scale_(config.resin_overlap_mm)));
    if (contour)
        path.polygons_covered_by_width(keepout, float(scale_(config.contour_infill_clearance_mm)));
}


// Preserve endpoints and bound the deviation of EVERY removed source knot.
// The optional map relates retained points to their original arc length, so
// command cleanup cannot shift a bend's speed interval or a process boundary.
Polyline3 normalized_xy(const Polyline3& input, std::vector<double>* source_positions = nullptr,
                        double error_mm = normalization_error_mm, const ExPolygons* centerline_domain = nullptr)
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
            const Vec2d a=(input.points[knots.back()]-input.points[knots[knots.size()-2]]).head<2>().cast<double>();
            const Vec2d b=(point-input.points[knots.back()]).head<2>().cast<double>();
            if (a.dot(b)<=0 || a.x()*b.y()!=a.y()*b.x()) break;
            knots.pop_back();
        }
        knots.push_back(i);
    }
    std::vector<size_t> retained;
    for (size_t index=0;index<knots.size();++index) {
        const auto& point=input.points[knots[index]];
        while (result.points.size()>=2) {
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
            if (centerline_domain && !diff_pl(Polylines{Polyline(Points{
                    Point(result.points[result.points.size()-2].x(), result.points[result.points.size()-2].y()),
                    Point(point.x(), point.y())})}, *centerline_domain).empty()) break;
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

// A speed field on the complete normalized path, including the cyclic seam.
// No source vertex number or input edge length enters the speed formula.
struct FiberSpeedPlanner {
    struct Bend { double begin, end, angle; };
    std::vector<Bend> bends;
    double length, minimum, maximum, transition;
    bool closed;
    FiberSpeedPlanner(const Polyline3& path, double vmin, double vmax, double distance,
                      const std::vector<ContourArc>& arcs)
        : length(unscale<double>(path.length())), minimum(vmin), maximum(vmax), transition(distance),
          closed(path.points.front() == path.points.back())
    {
        if (!arcs.empty()) {
            for (const auto& arc:arcs)
                bends.push_back({arc.begin_mm,arc.end_distance_mm,std::abs(arc.source_sweep_radians != 0 ? arc.source_sweep_radians : arc.sweep_radians)});
            return;
        }
        const ArcGeometry arc(path);
        const size_t n = path.points.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            if (i == 0 && !closed) continue;
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
                const std::vector<ContourArc>& arcs, const ExPolygons* centerline_domain)
{
    const bool contour = prepared.id.parent.purpose == FiberPathPurpose::Contour;
    const double ratio = contour ? config.contour_feed_ratio*config.contour_feed_correction :
                                   config.infill_feed_ratio*config.infill_feed_correction;
    FiberSpeedPlanner speeds(depositing,
        contour ? config.contour_min_speed_mm_s : config.infill_min_speed_mm_s,
        contour ? config.contour_max_speed_mm_s : config.infill_max_speed_mm_s,
        config.corner_transition_length_mm, arcs);
    const FiberSpeedPlanner tail_limits(depositing, config.tail_min_speed_mm_s,
                                       config.tail_max_speed_mm_s, config.corner_transition_length_mm, arcs);
    double offset = 0.0;
    for (FiberMotionSpan& span : prepared.spans) {
        const double source_length=unscale<double>(span.geometry.length());
        std::vector<double> source_positions;
        span.geometry = normalized_xy(span.geometry,&source_positions,
            arcs.empty() ? normalization_error_mm : ContourRoundingOptions{}.chord_tolerance_mm,
            span.deposits_fiber() ? centerline_domain : nullptr);
        const ArcGeometry arc(span.geometry);
        const double length = arc.positions.back();
        const auto source_at = [&](double s) {
            if (s<=0) return 0.0;
            if (s>=length) return source_length;
            const size_t i=size_t(std::upper_bound(arc.positions.begin(),arc.positions.end(),s)-arc.positions.begin());
            const double t=(s-arc.positions[i-1])/(arc.positions[i]-arc.positions[i-1]);
            return source_positions[i-1]+t*(source_positions[i]-source_positions[i-1]);
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
        for (double s : cuts) sampled.points.push_back(arc.at(s));
        for (size_t i = 1; i < cuts.size(); ++i) {
            double speed = config.finish_motion_speed_mm_s;
            if (span.actively_feeds_fiber()) {
                speed = speeds.limit(offset+source_at(cuts[i-1]), offset+source_at(cuts[i]));
                if (span.kind == FiberMotionKind::PoweredStart) speed = std::min(speed, config.start_speed_mm_s);
            } else if (span.kind == FiberMotionKind::PrefedLanding) {
                speed = config.landing_speed_mm_s;
            } else if (tail) {
                const size_t cell = std::min(cells-1, size_t(std::floor((cuts[i-1]+cuts[i])*0.5/length*cells)));
                speed = config.tail_min_speed_mm_s + (config.tail_max_speed_mm_s-config.tail_min_speed_mm_s)*double(cell)/double(cells-1);
                // The requested ramp is capped by the path's corner limits.
                // A speed conflict does not invalidate an otherwise printable path.
                speed = std::min(speed, tail_limits.limit(offset+source_at(cuts[i-1]), offset+source_at(cuts[i])));
            }
            edges.push_back({speed, span.actively_feeds_fiber() ? ratio : 0.0});
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
bool plan_finish(PreparedFiberPath& prepared, const ExtrusionPath& candidate,
                 const ContinuousFiberConfig& config)
{
    const bool tangent = !candidate.is_closed() || prepared.id.parent.purpose == FiberPathPurpose::Infill;
    const double length = tangent ? config.finish_extension_length_mm : config.finish_overlap_length_mm;
    if (length <= 0) return true;
    Polyline3 finish;
    if (tangent) {
        const auto& points = candidate.polyline.points;
        const Vec3d direction = (points.back()-points[points.size()-2]).cast<double>().normalized();
        const Vec3d endpoint = points.back().cast<double>() + scale_(length)*direction;
        finish.points = {points.back(), Point3(endpoint.x(), endpoint.y(), 0.0)};
        prepared.finish_strategy = FiberFinishStrategy::TangentExtension;
    } else {
        if (!candidate.is_closed() || length > unscale<double>(candidate.length())) {
            BOOST_LOG_TRIVIAL(debug) << "[FiberFinishRejected] layer=" << prepared.id.parent.domain.layer_id
                << " closed=" << candidate.is_closed() << " requested=" << length
                << " path=" << unscale<double>(candidate.length());
            return false;
        }
        finish = candidate.polyline;
        finish.clip_end(candidate.length()-scale_(length));
        prepared.finish_strategy = FiberFinishStrategy::LoopOverlap;
    }
    prepared.spans.push_back({std::move(finish), FiberMotionKind::NonDepositingFinish});
    return true;
}

} // namespace

Polyline3 normalize_fiber_geometry(const Polyline3& input, const ExPolygons* centerline_domain)
{
    return normalized_xy(input, nullptr, normalization_error_mm, centerline_domain);
}

FiberFinalizationResult FiberPathFinalizer::finalize(
    const ExtrusionPath& input,
    const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,
    const FiberFragmentId& id,
    const std::vector<ContourArc>& arcs, const ExPolygons* planned_centerline_domain)
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
    try { validate_fiber_process_config(config, id.parent.purpose, candidate.is_closed()); }
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

    if (!plan_finish(*prepared, candidate, config)) {
        result.failure = FiberFinalizationFailure::FinishUnavailable;
        result.detail = "Loop overlap requires a closed path at least as long as its configured finish";
        return result;
    }
    const bool check_centerline = planned_centerline_domain || !arcs.empty();
    const ExPolygons centerline_domain = planned_centerline_domain ?
        offset_ex(*planned_centerline_domain, float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm))) :
        check_centerline ? offset_ex(allowed_domain, -float(scale_(0.5*candidate.width +
            config.contour_boundary_clearance_mm - ContourRoundingOptions{}.geometry_tolerance_mm))) : ExPolygons{};
    try {
        plan_edges(*prepared, candidate.polyline, config, arcs, check_centerline ? &centerline_domain : nullptr);
    } catch (const std::length_error& error) {
        result.failure = FiberFinalizationFailure::SamplingLimit;
        result.detail = error.what();
        return result;
    }
    try { prepared->finalize_actions(); prepared->validate(); }
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
    Polygons exclusion;
    Polygons keepout;
    for (const FiberMotionSpan& span : prepared->spans)
        append_coverage(candidate, span, physical, exclusion, keepout, config, contour);

    prepared->physical_coverage = union_ex(physical);
    prepared->outside_domain = diff_ex(prepared->physical_coverage, allowed_domain, ApplySafetyOffset::Yes);
    if (area_mm2(prepared->outside_domain) > config.outside_tolerance_mm2) {
        BOOST_LOG_TRIVIAL(debug) << "[FiberCoverageRejected] layer=" << prepared->id.parent.domain.layer_id
            << " outside_mm2=" << area_mm2(prepared->outside_domain);
        result.failure = FiberFinalizationFailure::OutsideDomain;
        return result;
    }

    prepared->resin_exclusion = intersection_ex(union_ex(exclusion), allowed_domain, ApplySafetyOffset::Yes);
    if (contour)
        prepared->contour_to_infill_keepout = intersection_ex(union_ex(keepout), allowed_domain, ApplySafetyOffset::Yes);
    result.prepared = std::move(prepared);
    return result;
}

} // namespace Slic3r
