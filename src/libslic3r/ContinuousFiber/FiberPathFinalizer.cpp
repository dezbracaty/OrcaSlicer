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

double area_mm2(const ExPolygons& polygons)
{
    return unscaled<double>(unscaled<double>(std::abs(area(polygons))));
}

void append_coverage(const ExtrusionPath& source, const FiberMotionSpan& span, Polygons& physical, Polygons& exclusion,
                     Polygons& keepout, const ContinuousFiberConfig& config)
{
    if (!span.deposits_fiber() || span.geometry.points.size() < 2)
        return;

    ExtrusionPath path(span.geometry, source);
    path.polygons_covered_by_width(physical, 0.0f);
    path.polygons_covered_by_width(exclusion, -float(scale_(config.resin_overlap_mm)));
    path.polygons_covered_by_width(keepout, float(scale_(config.contour_infill_clearance_mm)));
}


Polyline3 normalized_xy(const Polyline3& input)
{
    Polyline3 result;
    for (size_t index = 0; index < input.points.size(); ++index) {
        const Point3& point = input.points[index];
        if (point.z() != 0)
            throw std::invalid_argument("Fiber LayerXY geometry has nonzero Z");
        if (!result.points.empty() &&
            (result.points.back()-point).cast<double>().norm() <= scale_(command_resolution_mm)) {
            // Preserve the original endpoint (and closed-loop seam). Replacing
            // its near neighbour also keeps neighbouring process spans joined.
            if (index + 1 != input.points.size()) continue;
            while (result.points.size() > 1 &&
                (result.points.back()-point).cast<double>().norm() <= scale_(command_resolution_mm))
                result.points.pop_back();
            if (result.points.back() == point) continue;
        }
        while (result.points.size() >= 2) {
            const Vec2d a = (result.points.back() - result.points[result.points.size() - 2]).head<2>().cast<double>();
            const Vec2d b = (point - result.points.back()).head<2>().cast<double>();
            if (a.dot(b) <= 0 || std::abs(a.x()*b.y() - a.y()*b.x()) > 1e-12*a.norm()*b.norm())
                break;
            result.points.pop_back();
        }
        result.points.push_back(point);
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
    std::vector<std::pair<double, double>> corners;
    double length, minimum, maximum, transition;
    bool closed;
    FiberSpeedPlanner(const Polyline3& path, double vmin, double vmax, double distance)
        : length(unscale<double>(path.length())), minimum(vmin), maximum(vmax), transition(distance),
          closed(path.points.front() == path.points.back())
    {
        const ArcGeometry arc(path);
        const size_t n = path.points.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            if (i == 0 && !closed) continue;
            const auto& prev = path.points[i == 0 ? n-2 : i-1];
            const Vec2d incoming = (path.points[i]-prev).head<2>().cast<double>().normalized();
            const Vec2d outgoing = (path.points[i+1]-path.points[i]).head<2>().cast<double>().normalized();
            const double angle = std::acos(std::clamp(incoming.dot(outgoing), -1.0, 1.0));
            if (angle > 1e-12) corners.emplace_back(arc.positions[i], angle);
        }
    }
    double limit(double begin, double end) const
    {
        double result = maximum;
        for (const auto& corner : corners) {
            double distance = std::numeric_limits<double>::max();
            for (int lap = closed ? -1 : 0; lap <= (closed ? 1 : 0); ++lap) {
                const double s = corner.first + lap*length;
                distance = std::min(distance, s < begin ? begin-s : s > end ? s-end : 0.0);
            }
            const double corner_speed = maximum - (maximum-minimum)*corner.second/PI;
            result = std::min(result, corner_speed + (maximum-corner_speed)*std::min(1.0, distance/transition));
        }
        return result;
    }
};

void plan_edges(PreparedFiberPath& prepared, const Polyline3& depositing, const ContinuousFiberConfig& config)
{
    const bool contour = prepared.id.parent.purpose == FiberPathPurpose::Contour;
    const double ratio = contour ? config.contour_feed_ratio*config.contour_feed_correction :
                                   config.infill_feed_ratio*config.infill_feed_correction;
    FiberSpeedPlanner speeds(depositing,
        contour ? config.contour_min_speed_mm_s : config.infill_min_speed_mm_s,
        contour ? config.contour_max_speed_mm_s : config.infill_max_speed_mm_s,
        config.corner_transition_length_mm);
    double offset = 0.0;
    for (FiberMotionSpan& span : prepared.spans) {
        span.geometry = normalized_xy(span.geometry);
        const ArcGeometry arc(span.geometry);
        const double length = arc.positions.back();
        std::vector<double> cuts = arc.positions;
        const bool tail = span.kind == FiberMotionKind::PassiveDepositingAfterCut;
        const size_t cells = tail ? std::max(size_t(2), size_t(std::ceil(length/config.tail_speed_step_length_mm))) : 0;
        if (cells > 1000000 || length/config.speed_sampling_length_mm > 1000000)
            throw std::invalid_argument("Fiber command sampling exceeds the supported budget");
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
                speed = speeds.limit(offset+cuts[i-1], offset+cuts[i]);
                if (span.kind == FiberMotionKind::PoweredStart) speed = std::min(speed, config.start_speed_mm_s);
            } else if (span.kind == FiberMotionKind::PrefedLanding) {
                speed = config.landing_speed_mm_s;
            } else if (tail) {
                const size_t cell = std::min(cells-1, size_t(std::floor((cuts[i-1]+cuts[i])*0.5/length*cells)));
                speed = config.tail_min_speed_mm_s + (config.tail_max_speed_mm_s-config.tail_min_speed_mm_s)*double(cell)/double(cells-1);
                // Tail has its own speed range; turns obey the same angular safety model.
                FiberSpeedPlanner tail_limits(depositing, config.tail_min_speed_mm_s,
                                             config.tail_max_speed_mm_s, config.corner_transition_length_mm);
                if (speed > tail_limits.limit(offset+cuts[i-1], offset+cuts[i]) + EPSILON)
                    throw std::invalid_argument("Fiber tail ramp exceeds the corner speed limit");
            }
            edges.push_back({speed, span.actively_feeds_fiber() ? ratio : 0.0});
        }
        span.geometry = std::move(sampled);
        span.edges = std::move(edges);
        if (span.deposits_fiber()) offset += length;
    }
}

// Finish is a tool motion after the depositing tail, not another material path.
// Its machine-space limits are checked at execution binding, never against the
// deposition domain (or its contour keepouts) and never using fiber line width.
bool plan_finish(PreparedFiberPath& prepared, const ExtrusionPath& candidate,
                 const ContinuousFiberConfig& config)
{
    const bool tangent = prepared.id.parent.purpose == FiberPathPurpose::Infill ||
        (config.infill_enabled && config.infill_pattern == ipConcentric);
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

Polyline3 normalize_fiber_geometry(const Polyline3& input) { return normalized_xy(input); }

FiberFinalizationResult FiberPathFinalizer::finalize(
    const ExtrusionPath& input,
    const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,
    const FiberFragmentId& id)
{
    FiberFinalizationResult result;
    ExtrusionPath candidate(input);
    try { candidate.polyline = normalized_xy(input.polyline); }
    catch (const std::invalid_argument&) {
        result.failure = FiberFinalizationFailure::InvalidGeometry;
        return result;
    }
    result.length_mm = unscale<double>(candidate.length());

    const double process_values[] = {
        result.length_mm,
        config.minimum_path_length_mm,
        config.landing_length_mm,
        config.start_stabilization_length_mm,
        config.minimum_effective_length_mm,
        config.cut_to_contact_length_mm,
        config.prefeed_extra_length_mm,
        config.prefeed_speed_mm_s,
        config.z_hop_height_mm,
        config.landing_speed_mm_s,
        config.start_speed_mm_s,
        config.finish_extension_length_mm,
        config.finish_overlap_length_mm,
        config.outside_tolerance_mm2,
        config.resin_overlap_mm,
        config.contour_infill_clearance_mm
    };
    if (!std::all_of(std::begin(process_values), std::end(process_values), [](double value) {
            return std::isfinite(value) && value >= 0.0;
        })) {
        result.failure = FiberFinalizationFailure::InvalidParameter;
        return result;
    }
    if ((config.cut_to_contact_length_mm + config.prefeed_extra_length_mm > 0.0 && config.prefeed_speed_mm_s <= 0.0) ||
        (config.landing_length_mm > 0.0 && config.landing_speed_mm_s <= 0.0) ||
        (config.start_stabilization_length_mm > 0.0 && config.start_speed_mm_s <= 0.0)) {
        result.failure = FiberFinalizationFailure::InvalidParameter;
        return result;
    }

    const double positive_values[] = {
        config.contour_feed_ratio, config.infill_feed_ratio, config.contour_feed_correction,
        config.infill_feed_correction, config.contour_min_speed_mm_s, config.contour_max_speed_mm_s,
        config.infill_min_speed_mm_s, config.infill_max_speed_mm_s, config.corner_transition_length_mm,
        config.speed_sampling_length_mm, config.tail_min_speed_mm_s, config.tail_max_speed_mm_s,
        config.tail_speed_step_length_mm, config.finish_motion_speed_mm_s
    };
    if (!std::all_of(std::begin(positive_values), std::end(positive_values),
                    [](double v){return std::isfinite(v) && v > 0;}) ||
        config.contour_min_speed_mm_s > config.contour_max_speed_mm_s ||
        config.infill_min_speed_mm_s > config.infill_max_speed_mm_s ||
        config.tail_min_speed_mm_s > config.tail_max_speed_mm_s) {
        result.failure = FiberFinalizationFailure::InvalidParameter;
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
    if (candidate.polyline.points.size() < 2 || allowed_domain.empty()) {
        result.failure = FiberFinalizationFailure::InvalidGeometry;
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
            !candidate.polyline.split_at_length(split_distance, &before_tail, &passive) ||
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
        if (!after_landing.split_at_length(landing_scaled, &landing, &remaining) ||
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
        if (!after_landing.split_at_length(stabilization_scaled, &powered_start, &remaining) ||
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

    if (!plan_finish(*prepared, candidate, config))
        throw std::runtime_error("Invalid fiber finish geometry at layer " + std::to_string(id.parent.domain.layer_id) +
            ": loop overlap requires a closed path at least as long as its configured finish");
    try {
        plan_edges(*prepared, candidate.polyline, config);
    } catch (const std::invalid_argument&) {
        result.failure = FiberFinalizationFailure::InvalidParameter;
        return result;
    }
    try { prepared->finalize_actions(); prepared->validate(); }
    catch (const std::invalid_argument&) {
        result.failure = FiberFinalizationFailure::InvalidGeometry;
        return result;
    }

    Polygons physical;
    Polygons exclusion;
    Polygons keepout;
    for (const FiberMotionSpan& span : prepared->spans)
        append_coverage(candidate, span, physical, exclusion, keepout, config);

    prepared->physical_coverage = union_ex(physical);
    prepared->outside_domain = diff_ex(prepared->physical_coverage, allowed_domain, ApplySafetyOffset::Yes);
    if (area_mm2(prepared->outside_domain) > config.outside_tolerance_mm2) {
        BOOST_LOG_TRIVIAL(debug) << "[FiberCoverageRejected] layer=" << prepared->id.parent.domain.layer_id
            << " outside_mm2=" << area_mm2(prepared->outside_domain);
        result.failure = FiberFinalizationFailure::OutsideDomain;
        return result;
    }

    prepared->resin_exclusion = intersection_ex(union_ex(exclusion), allowed_domain, ApplySafetyOffset::Yes);
    prepared->contour_to_infill_keepout = intersection_ex(union_ex(keepout), allowed_domain, ApplySafetyOffset::Yes);
    result.prepared = std::move(prepared);
    return result;
}

} // namespace Slic3r
