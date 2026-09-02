#include "BeltTaperedSupport.hpp"
#include "BeltSupportDebug.hpp"

#include "SupportCommon.hpp"
#include "SupportParameters.hpp"
#include "TreeSupport.hpp"

#include "libslic3r/Belt/BeltCoordinateSystem.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

struct BeltSupportContact
{
    Point  center_local;
    Point  witness_local;
    coord_t root_u_local{0};
    coord_t root_v_local{0};
    double tip_s{0.0};
    double root_s{0.0};
    double tip_half_width{0.0};
    double root_half_width{0.0};
    size_t source_contact_index{0};
    size_t source_route_index{std::numeric_limits<size_t>::max()};
    size_t parent_route_index{std::numeric_limits<size_t>::max()};
    size_t source_overhang_region_index{0};
    size_t source_surface_component_index{0};
    size_t source_layer_index{0};
    double tip_cap_model_overlap_area{0.0};
    double original_tip_cap_model_overlap_area{0.0};
    double tip_cap_overhang_overlap_area{0.0};
    std::vector<size_t> source_witness_indices;
    bool center_adjusted{false};
    bool root_edge{false};
};

struct BeltSupportLayerPlan
{
    double     slice_s{0.0};
    double     print_s{0.0};
    double     height{0.0};
    Polylines  root_paths;
    ExPolygons base_regions;
    ExPolygons interface_regions;
};

struct BeltForbiddenLayer
{
    ExPolygons regions;
    std::vector<BoundingBox> bounds;
};

struct BeltReachableInterval
{
    double minimum_u{0.0};
    double maximum_u{0.0};
};

struct BeltReachableLayer
{
    size_t plan_index{0};
    double slice_s{0.0};
    std::vector<BeltReachableInterval> intervals;
};

struct BeltRoutePoint
{
    double u{0.0};
    double v{0.0};
    double s{0.0};
};

struct BeltSupportRoute
{
    static constexpr size_t no_parent = std::numeric_limits<size_t>::max();

    size_t contact_index{0};
    coord_t v_local{0};
    double tip_s{0.0};
    double root_s{0.0};
    std::vector<BeltReachableLayer> reachable_layers;
    std::vector<BeltRoutePoint> points;
    size_t parent_route_index{no_parent};
    double merge_s{0.0};
    std::string failure_reason;
};

struct BeltRootFoundation
{
    Polygon footprint_us;
};

Polygon rectangle_at(const Point& center, double half_width_u, double half_width_v)
{
    const coord_t du = scaled<coord_t>(half_width_u);
    const coord_t dv = scaled<coord_t>(half_width_v);
    return Polygon({
        Point(center.x() - du, center.y() - dv),
        Point(center.x() + du, center.y() - dv),
        Point(center.x() + du, center.y() + dv),
        Point(center.x() - du, center.y() + dv),
    });
}

double overlap_area_mm2(const ExPolygons& first, const ExPolygons& second)
{
    return unscaled<double>(unscaled<double>(std::abs(
        area(intersection_ex(first, second)))));
}

double expolygons_area_mm2(const ExPolygons& polygons)
{
    return unscaled<double>(unscaled<double>(std::abs(area(polygons))));
}

std::optional<Point> nearest_clear_tip_center(
    const Point& witness, double half_width,
    const ExPolygons& model_at_tip)
{
    if (model_at_tip.empty())
        return witness;

    auto collision_area = [&](const Point& center) {
        const ExPolygons cap{
            ExPolygon(rectangle_at(center, half_width, half_width))};
        return overlap_area_mm2(cap, model_at_tip);
    };
    if (collision_area(witness) <= 1e-8)
        return witness;

    // The adjusted cap must still contain the original witness. Search a
    // deterministic square lattice inside exactly that admissible center
    // range and prefer the smallest displacement.
    const double search_step = std::min(0.05, std::max(0.01, half_width / 8.0));
    const int steps = std::max(1, static_cast<int>(std::ceil(half_width / search_step)));
    struct Offset
    {
        double du{0.0};
        double dv{0.0};
        double distance_squared{0.0};
    };
    std::vector<Offset> offsets;
    offsets.reserve(static_cast<size_t>((2 * steps + 1) * (2 * steps + 1)));
    for (int v_step = -steps; v_step <= steps; ++v_step) {
        for (int u_step = -steps; u_step <= steps; ++u_step) {
            const double du = std::clamp(
                static_cast<double>(u_step) * search_step,
                -half_width, half_width);
            const double dv = std::clamp(
                static_cast<double>(v_step) * search_step,
                -half_width, half_width);
            offsets.push_back({du, dv, du * du + dv * dv});
        }
    }
    std::sort(offsets.begin(), offsets.end(),
              [](const Offset& first, const Offset& second) {
                  if (std::abs(first.distance_squared - second.distance_squared) > EPSILON)
                      return first.distance_squared < second.distance_squared;
                  if (std::abs(first.dv - second.dv) > EPSILON)
                      return first.dv < second.dv;
                  return first.du < second.du;
              });
    for (const Offset& offset : offsets) {
        const Point candidate(
            witness.x() + scaled<coord_t>(offset.du),
            witness.y() + scaled<coord_t>(offset.dv));
        if (collision_area(candidate) <= 1e-8)
            return candidate;
    }
    return std::nullopt;
}

ExPolygons clip_section_to_world_build_halfspace(
    const ExPolygon& section,
    double slice_s,
    double center_v,
    const BeltCoordinateSystem& coordinates)
{
    const BoundingBox bounds = get_extents(section);
    if (!bounds.defined)
        return {};
    const coord_t minimum_printable_v = scaled<coord_t>(
        coordinates.belt_boundary_v(slice_s) - center_v);
    if (bounds.max.y() < minimum_printable_v)
        return {};
    if (bounds.min.y() >= minimum_printable_v)
        return {section};

    const coord_t margin = scale_(1.0);
    Polygon printable_halfspace({
        Point(bounds.min.x() - margin, minimum_printable_v),
        Point(bounds.max.x() + margin, minimum_printable_v),
        Point(bounds.max.x() + margin, bounds.max.y() + margin),
        Point(bounds.min.x() - margin, bounds.max.y() + margin),
    });
    return intersection_ex(
        ExPolygons{section},
        ExPolygons{ExPolygon(std::move(printable_halfspace))});
}

ExPolygons clip_regions_to_world_build_halfspace(
    const ExPolygons& regions,
    double slice_s,
    double center_v,
    const BeltCoordinateSystem& coordinates)
{
    ExPolygons clipped;
    for (const ExPolygon& region : regions) {
        ExPolygons pieces = clip_section_to_world_build_halfspace(
            region, slice_s, center_v, coordinates);
        clipped.insert(clipped.end(),
                       std::make_move_iterator(pieces.begin()),
                       std::make_move_iterator(pieces.end()));
    }
    return union_ex(clipped);
}

Point branch_center_at(const BeltSupportContact& contact, double slice_s)
{
    const double span = contact.tip_s - contact.root_s;
    const double ratio = span <= EPSILON
        ? 1.0
        : std::clamp((slice_s - contact.root_s) / span, 0.0, 1.0);
    const coord_t u = contact.root_u_local + static_cast<coord_t>(std::llround(
        static_cast<double>(contact.center_local.x() - contact.root_u_local) * ratio));
    const coord_t v = contact.root_v_local + static_cast<coord_t>(std::llround(
        static_cast<double>(contact.center_local.y() - contact.root_v_local) * ratio));
    return {u, v};
}

const Layer* closest_model_layer(const PrintObject& object, double slice_s, double tolerance)
{
    const Layer* closest = nullptr;
    double best_distance = std::numeric_limits<double>::max();
    for (const Layer* layer : object.layers()) {
        const double distance = std::abs(layer->slice_z - slice_s);
        if (distance < best_distance) {
            closest = layer;
            best_distance = distance;
        }
    }
    return best_distance <= tolerance ? closest : nullptr;
}

ExPolygons model_clearance_at(const PrintObject& object, double slice_s, double tolerance,
                              double clearance)
{
    const Layer* layer = closest_model_layer(object, slice_s, tolerance);
    if (layer == nullptr || layer->lslices.empty())
        return {};
    return clearance > EPSILON ? offset_ex(layer->lslices, scale_(clearance)) : layer->lslices;
}

std::vector<BeltReachableInterval> merge_intervals(
    std::vector<BeltReachableInterval> intervals)
{
    if (intervals.empty())
        return {};
    std::sort(intervals.begin(), intervals.end(),
              [](const BeltReachableInterval& first,
                 const BeltReachableInterval& second) {
                  return first.minimum_u < second.minimum_u;
              });
    std::vector<BeltReachableInterval> merged;
    merged.reserve(intervals.size());
    for (const BeltReachableInterval& interval : intervals) {
        if (interval.maximum_u + EPSILON < interval.minimum_u)
            continue;
        if (merged.empty() || interval.minimum_u > merged.back().maximum_u + EPSILON) {
            merged.push_back(interval);
        } else {
            merged.back().maximum_u =
                std::max(merged.back().maximum_u, interval.maximum_u);
        }
    }
    return merged;
}

std::vector<BeltReachableInterval> subtract_intervals(
    const std::vector<BeltReachableInterval>& source,
    std::vector<BeltReachableInterval> blocked)
{
    blocked = merge_intervals(std::move(blocked));
    std::vector<BeltReachableInterval> result;
    for (const BeltReachableInterval& allowed : source) {
        double cursor = allowed.minimum_u;
        for (const BeltReachableInterval& obstacle : blocked) {
            if (obstacle.maximum_u <= cursor + EPSILON)
                continue;
            if (obstacle.minimum_u >= allowed.maximum_u - EPSILON)
                break;
            if (obstacle.minimum_u > cursor + EPSILON)
                result.push_back({cursor, std::min(obstacle.minimum_u,
                                                   allowed.maximum_u)});
            cursor = std::max(cursor, obstacle.maximum_u);
            if (cursor >= allowed.maximum_u - EPSILON)
                break;
        }
        if (cursor < allowed.maximum_u - EPSILON)
            result.push_back({cursor, allowed.maximum_u});
    }
    return merge_intervals(std::move(result));
}

double closest_value_in_intervals(const std::vector<BeltReachableInterval>& intervals,
                                  double target)
{
    double best = target;
    double best_distance = std::numeric_limits<double>::max();
    for (const BeltReachableInterval& interval : intervals) {
        const double value = std::clamp(target, interval.minimum_u, interval.maximum_u);
        const double distance = std::abs(value - target);
        if (distance < best_distance) {
            best = value;
            best_distance = distance;
        }
    }
    return best;
}

bool interval_contains(const std::vector<BeltReachableInterval>& intervals,
                       double value)
{
    return std::any_of(
        intervals.begin(), intervals.end(),
        [value](const BeltReachableInterval& interval) {
            return value >= interval.minimum_u - EPSILON &&
                   value <= interval.maximum_u + EPSILON;
        });
}

bool route_segment_stays_in_corridor(const BeltSupportRoute& route,
                                     size_t first, size_t last)
{
    if (last <= first || last >= route.points.size() ||
        route.reachable_layers.size() != route.points.size()) {
        return false;
    }
    const BeltRoutePoint& upper = route.points[first];
    const BeltRoutePoint& lower = route.points[last];
    const double span = upper.s - lower.s;
    if (span <= EPSILON)
        return false;
    for (size_t index = first + 1; index < last; ++index) {
        const double ratio = (upper.s - route.points[index].s) / span;
        const double u = upper.u + (lower.u - upper.u) * ratio;
        if (!interval_contains(route.reachable_layers[index].intervals, u))
            return false;
    }
    return true;
}

std::vector<BeltRoutePoint> simplify_route_in_corridor(const BeltSupportRoute& route)
{
    if (route.points.size() < 3 ||
        route.reachable_layers.size() != route.points.size()) {
        return route.points;
    }
    std::vector<BeltRoutePoint> simplified;
    simplified.reserve(route.points.size());
    size_t first = 0;
    simplified.push_back(route.points.front());
    while (first + 1 < route.points.size()) {
        size_t last = route.points.size() - 1;
        while (last > first + 1 &&
               !route_segment_stays_in_corridor(route, first, last)) {
            --last;
        }
        simplified.push_back(route.points[last]);
        first = last;
    }
    return simplified;
}

double route_u_at(const BeltSupportRoute& route, double slice_s)
{
    if (route.points.empty())
        return 0.0;
    if (slice_s >= route.points.front().s)
        return route.points.front().u;
    if (slice_s <= route.points.back().s)
        return route.points.back().u;
    for (size_t index = 1; index < route.points.size(); ++index) {
        const BeltRoutePoint& upper = route.points[index - 1];
        const BeltRoutePoint& lower = route.points[index];
        if (slice_s > upper.s + EPSILON || slice_s < lower.s - EPSILON)
            continue;
        const double span = upper.s - lower.s;
        const double ratio = span <= EPSILON ? 0.0 : (upper.s - slice_s) / span;
        return upper.u + (lower.u - upper.u) * ratio;
    }
    return route.points.back().u;
}

double route_v_at(const BeltSupportRoute& route, double slice_s)
{
    if (route.points.empty())
        return unscaled<double>(route.v_local);
    if (slice_s >= route.points.front().s)
        return route.points.front().v;
    if (slice_s <= route.points.back().s)
        return route.points.back().v;
    for (size_t index = 1; index < route.points.size(); ++index) {
        const BeltRoutePoint& upper = route.points[index - 1];
        const BeltRoutePoint& lower = route.points[index];
        if (slice_s > upper.s + EPSILON || slice_s < lower.s - EPSILON)
            continue;
        const double span = upper.s - lower.s;
        const double ratio = span <= EPSILON ? 0.0 : (upper.s - slice_s) / span;
        return upper.v + (lower.v - upper.v) * ratio;
    }
    return route.points.back().v;
}

std::vector<BeltReachableInterval> blocked_u_intervals(
    const BeltForbiddenLayer& forbidden, double v, double half_width)
{
    std::vector<BeltReachableInterval> blocked;
    const coord_t minimum_v = scaled<coord_t>(v - half_width);
    const coord_t maximum_v = scaled<coord_t>(v + half_width);
    for (size_t obstacle_index = 0;
         obstacle_index < forbidden.regions.size(); ++obstacle_index) {
        const BoundingBox& bounds = forbidden.bounds[obstacle_index];
        if (!bounds.defined || maximum_v < bounds.min.y() ||
            minimum_v > bounds.max.y()) {
            continue;
        }
        const coord_t margin = scaled<coord_t>(std::max(half_width, 0.01));
        Polygon strip({
            Point(bounds.min.x() - margin, minimum_v),
            Point(bounds.max.x() + margin, minimum_v),
            Point(bounds.max.x() + margin, maximum_v),
            Point(bounds.min.x() - margin, maximum_v),
        });
        const ExPolygons clipped = intersection_ex(
            ExPolygons{forbidden.regions[obstacle_index]},
            ExPolygons{ExPolygon(std::move(strip))});
        for (const ExPolygon& component : clipped) {
            const BoundingBox component_bounds = get_extents(component);
            if (!component_bounds.defined)
                continue;
            blocked.push_back({
                unscaled<double>(component_bounds.min.x()) - half_width,
                unscaled<double>(component_bounds.max.x()) + half_width,
            });
        }
    }
    return merge_intervals(std::move(blocked));
}

Vec3d local_point_to_world(const Point& point, double slice_s, double center_u, double center_v,
                           const BeltCoordinateSystem& coordinates)
{
    return coordinates.oriented_to_world(Vec3d(
        unscaled<double>(point.x()) + center_u,
        unscaled<double>(point.y()) + center_v,
        slice_s));
}

void debug_polygon(BeltSupportDebugRecorder* recorder, BeltSupportDebugStageId stage,
                   const Polygon& polygon, double slice_s, double center_u, double center_v,
                   const BeltCoordinateSystem& coordinates, const std::string& category)
{
    if (recorder == nullptr || polygon.points.size() < 2)
        return;
    for (size_t index = 0; index < polygon.points.size(); ++index) {
        const Point& first = polygon.points[index];
        const Point& second = polygon.points[(index + 1) % polygon.points.size()];
        recorder->add_line(stage,
                           local_point_to_world(first, slice_s, center_u, center_v, coordinates),
                           local_point_to_world(second, slice_s, center_u, center_v, coordinates),
                           category);
    }
}

void debug_expolygons(BeltSupportDebugRecorder* recorder, BeltSupportDebugStageId stage,
                      const ExPolygons& polygons, double slice_s, double center_u, double center_v,
                      const BeltCoordinateSystem& coordinates, const std::string& category)
{
    if (recorder == nullptr)
        return;
    for (const ExPolygon& polygon : polygons) {
        debug_polygon(recorder, stage, polygon.contour, slice_s, center_u, center_v,
                      coordinates, category);
        for (const Polygon& hole : polygon.holes)
            debug_polygon(recorder, stage, hole, slice_s, center_u, center_v,
                          coordinates, category);
    }
}

Point safe_centroid(const ExPolygon& polygon)
{
    Point center = polygon.contour.centroid();
    if (polygon.contains(center))
        return center;

    const BoundingBox box = get_extents(polygon);
    center = (box.min + box.max) / 2;
    if (polygon.contains(center))
        return center;

    return polygon.contour.points.empty() ? Point() : polygon.contour.points.front();
}

void append_contact_samples(const ExPolygon& overhang, coord_t spacing, std::vector<Point>& out)
{
    std::set<std::pair<coord_t, coord_t>> unique_points;
    auto append_unique = [&out, &unique_points](const Point& point) {
        if (unique_points.emplace(point.x(), point.y()).second)
            out.emplace_back(point);
    };

    const BoundingBox box = get_extents(overhang);
    const Point center = safe_centroid(overhang);
    if (overhang.contains(center))
        append_unique(center);

    if (!box.defined || spacing <= 0)
        return;

    // A global grid alone misses long, thin or diagonal overhang strips. Sample
    // every contour by arc length so no boundary run longer than the configured
    // branch spacing can disappear merely because the grid did not cross it.
    for (const Point& point : overhang.contour.equally_spaced_points(spacing))
        append_unique(point);
    for (const Polygon& hole : overhang.holes)
        for (const Point& point : hole.equally_spaced_points(spacing))
            append_unique(point);

    const coord_t start_x = box.min.x() + spacing / 2;
    const coord_t start_y = box.min.y() + spacing / 2;
    for (coord_t y = start_y; y < box.max.y(); y += spacing) {
        for (coord_t x = start_x; x < box.max.x(); x += spacing) {
            const Point candidate(x, y);
            if (overhang.contains(candidate))
                append_unique(candidate);
        }
    }
}

Polylines make_root_paths(double slice_s,
                          double print_s,
                          const BeltRootFoundation& foundation,
                          double center_v,
                          const BeltCoordinateSystem& coordinates)
{
    if (foundation.footprint_us.points.size() < 3)
        return {};

    const BoundingBox bounds = get_extents(foundation.footprint_us);
    const coord_t scaled_s = scaled<coord_t>(slice_s);
    if (!bounds.defined || scaled_s < bounds.min.y() || scaled_s > bounds.max.y())
        return {};

    const coord_t margin = scale_(1.0);
    Polylines intersections = intersection_pl(
        Polylines{Polyline(Point(bounds.min.x() - margin, scaled_s),
                           Point(bounds.max.x() + margin, scaled_s))},
        Polygons{foundation.footprint_us});
    if (intersections.empty())
        return {};

    coord_t minimum_u = std::numeric_limits<coord_t>::max();
    coord_t maximum_u = std::numeric_limits<coord_t>::lowest();
    for (const Polyline& intersection : intersections) {
        for (const Point& point : intersection.points) {
            minimum_u = std::min(minimum_u, point.x());
            maximum_u = std::max(maximum_u, point.x());
        }
    }
    if (maximum_u <= minimum_u)
        return {};

    // Width is sampled at the layer centre, but G-code emits this path on
    // print_s. Put the actual extrusion plane on the physical build plate.
    const double boundary_v_local = coordinates.belt_boundary_v(print_s) - center_v;
    Polyline root(Point(minimum_u, scaled<coord_t>(boundary_v_local)),
                  Point(maximum_u, scaled<coord_t>(boundary_v_local)));
    return root.is_valid() ? Polylines{std::move(root)} : Polylines{};
}

Polylines center_lines_for_narrow_regions(const ExPolygons& regions)
{
    Polylines lines;
    for (const ExPolygon& region : regions) {
        const BoundingBox box = get_extents(region);
        if (!box.defined)
            continue;
        double mean_x = 0.0;
        double mean_y = 0.0;
        for (const Point& point : region.contour.points) {
            mean_x += unscaled<double>(point.x());
            mean_y += unscaled<double>(point.y());
        }
        const double point_count = static_cast<double>(
            std::max<size_t>(1, region.contour.points.size()));
        mean_x /= point_count;
        mean_y /= point_count;
        double covariance_xx = 0.0;
        double covariance_xy = 0.0;
        double covariance_yy = 0.0;
        for (const Point& point : region.contour.points) {
            const double x = unscaled<double>(point.x()) - mean_x;
            const double y = unscaled<double>(point.y()) - mean_y;
            covariance_xx += x * x;
            covariance_xy += x * y;
            covariance_yy += y * y;
        }
        const double principal_angle = 0.5 * std::atan2(
            2.0 * covariance_xy, covariance_xx - covariance_yy);
        const double extent = 2.0 * std::hypot(
            unscaled<double>(box.size().x()),
            unscaled<double>(box.size().y()));
        const coord_t delta_x = scaled<coord_t>(
            extent * std::cos(principal_angle));
        const coord_t delta_y = scaled<coord_t>(
            extent * std::sin(principal_angle));
        const Point center = safe_centroid(region);
        const Polyline axis(
            Point(center.x() - delta_x, center.y() - delta_y),
            Point(center.x() + delta_x, center.y() + delta_y));
        Polylines clipped = intersection_pl(axis, region);
        lines.insert(lines.end(),
                     std::make_move_iterator(clipped.begin()),
                     std::make_move_iterator(clipped.end()));
    }
    return lines;
}

struct InterfaceFillComponentAudit
{
    size_t component_index{0};
    size_t entity_count{0};
    size_t path_count{0};
    bool used_fallback{false};
    double region_area_mm2{0.0};
    double path_length_mm{0.0};
    double footprint_covered_area_mm2{0.0};
};

size_t append_interface_fill(ExtrusionEntitiesPtr& destination,
                             const ExPolygons& regions,
                             const Flow& flow,
                             const SupportParameters& support_parameters,
                             size_t layer_index,
                             std::vector<InterfaceFillComponentAudit>* component_audit = nullptr)
{
    if (regions.empty())
        return 0;
    size_t fallback_component_count = 0;
    std::unique_ptr<Fill> filler(
        Fill::new_from_type(support_parameters.contact_fill_pattern));
    const BoundingBox bounds = get_extents(regions);
    if (bounds.defined)
        filler->set_bounding_box(bounds);
    filler->spacing = flow.spacing();
    filler->angle = support_parameters.interface_angle;
    filler->layer_id = layer_index;
    for (size_t component_index = 0;
         component_index < regions.size(); ++component_index) {
        const ExPolygon& region = regions[component_index];
        const size_t path_count_before = destination.size();
        fill_expolygons_with_sheath_generate_paths(
            destination, to_polygons(ExPolygons{region}), filler.get(),
            static_cast<float>(support_parameters.top_interface_density),
            ExtrusionRole::erSupportMaterialInterface,
            flow, support_parameters, true, false);
        bool used_fallback = false;
        if (destination.size() == path_count_before) {
            used_fallback = true;
            ++fallback_component_count;
            Polylines fallback = center_lines_for_narrow_regions(
                ExPolygons{region});
            extrusion_entities_append_paths(
                destination, std::move(fallback),
                ExtrusionRole::erSupportMaterialInterface,
                flow.mm3_per_mm(), flow.width(), flow.height(), false);
        }
        if (component_audit != nullptr) {
            InterfaceFillComponentAudit audit;
            audit.component_index = component_index;
            audit.entity_count = destination.size() - path_count_before;
            audit.used_fallback = used_fallback;
            audit.region_area_mm2 = expolygons_area_mm2(ExPolygons{region});
            Polygons footprint_polygons;
            for (size_t entity_index = path_count_before;
                 entity_index < destination.size(); ++entity_index) {
                const ExtrusionEntity* entity = destination[entity_index];
                if (entity == nullptr)
                    continue;
                const Polylines paths = entity->as_polylines();
                audit.path_count += paths.size();
                for (const Polyline& path : paths)
                    audit.path_length_mm += unscaled<double>(path.length());
                Polygons covered = entity->polygons_covered_by_width();
                append(footprint_polygons, std::move(covered));
            }
            const ExPolygons footprint = union_ex(footprint_polygons);
            audit.footprint_covered_area_mm2 = overlap_area_mm2(
                ExPolygons{region}, footprint);
            component_audit->emplace_back(std::move(audit));
        }
    }
    return fallback_component_count;
}

struct PolygonGap
{
    double distance_mm{std::numeric_limits<double>::infinity()};
    Point current;
    Point previous;
};

PolygonGap boundary_gap(const ExPolygon& current,
                        const ExPolygons& previous)
{
    PolygonGap gap;
    if (previous.empty())
        return gap;
    const ExPolygons current_polygons{current};
    auto consider_current_point = [&gap, &previous](const Point& point) {
        const Point projected = projection_onto(previous, point);
        const double distance = unscaled<double>((projected - point).norm());
        if (distance < gap.distance_mm) {
            gap.distance_mm = distance;
            gap.current = point;
            gap.previous = projected;
        }
    };
    auto consider_previous_point = [&gap, &current_polygons](const Point& point) {
        const Point projected = projection_onto(current_polygons, point);
        const double distance = unscaled<double>((projected - point).norm());
        if (distance < gap.distance_mm) {
            gap.distance_mm = distance;
            gap.current = projected;
            gap.previous = point;
        }
    };
    for (const Point& point : current.contour.points)
        consider_current_point(point);
    for (const Polygon& hole : current.holes)
        for (const Point& point : hole.points)
            consider_current_point(point);
    for (const ExPolygon& polygon : previous) {
        for (const Point& point : polygon.contour.points)
            consider_previous_point(point);
        for (const Polygon& hole : polygon.holes)
            for (const Point& point : hole.points)
                consider_previous_point(point);
    }
    return gap;
}

ExPolygons entity_footprint(const ExtrusionEntitiesPtr& entities,
                            size_t begin_index,
                            size_t end_index)
{
    Polygons covered_polygons;
    end_index = std::min(end_index, entities.size());
    for (size_t entity_index = begin_index;
         entity_index < end_index; ++entity_index) {
        const ExtrusionEntity* entity = entities[entity_index];
        if (entity == nullptr)
            continue;
        Polygons covered = entity->polygons_covered_by_width();
        append(covered_polygons, std::move(covered));
    }
    return union_ex(covered_polygons);
}

size_t count_open_paths(const ExtrusionEntityCollection& collection)
{
    size_t count = 0;
    for (const ExtrusionEntity* entity : collection.entities)
        if (entity != nullptr && !entity->is_loop())
            ++count;
    return count;
}

} // namespace

BeltTaperedSupport::BeltTaperedSupport(PrintObject& object,
                                       const SlicingParameters& slicing_parameters)
    : m_object(object), m_slicing_parameters(slicing_parameters)
{}

void BeltTaperedSupport::generate()
{
    const BeltCoordinateSystem* coordinates = m_object.print()->belt_coordinate_system();
    if (coordinates == nullptr)
        throw SlicingError("Belt tapered support requires a valid belt coordinate system.");

    m_object.clear_support_layers();
    BeltSupportDebugRecorder* debug = current_belt_support_debug_recorder();

    TreeSupport detector(m_object, m_slicing_parameters);
    detector.support_type = m_object.config().support_type.value;
    detector.throw_on_cancel = [this]() {
        if (m_object.print()->canceled())
            throw CanceledException();
    };
    {
        BeltSupportDebugStageTimer timer(debug, BeltSupportDebugStageId::OverhangRegions);
        detector.detect_overhangs();
    }

    const SupportParameters support_parameters(m_object);
    const double center_u = unscaled<double>(m_object.center_offset().x());
    const double center_v = unscaled<double>(m_object.center_offset().y());
    const double top_gap = std::max(0.0, m_object.config().support_top_z_distance.value);
    const double configured_tree_tip_half_width =
        0.5 * m_object.config().tree_support_tip_diameter.value;
    const double tip_half_width =
        0.5 * support_parameters.support_material_flow.width();
    const double taper = std::tan(std::clamp(
        m_object.config().tree_support_branch_diameter_angle.value * M_PI / 180.0,
        0.0, 0.45 * M_PI));
    const double maximum_half_width = 6.0;
    const coord_t sample_spacing = scale_(std::max(
        1.5 * support_parameters.support_material_flow.width(),
        m_object.config().tree_support_branch_distance.value));
    const double contact_inset = tip_half_width;
    std::vector<ExPolygons> overhangs_by_layer;
    overhangs_by_layer.reserve(m_object.layers().size());
    size_t raw_overhang_region_count = 0;
    size_t unioned_overhang_region_count = 0;
    for (size_t layer_index = 0; layer_index < m_object.layers().size(); ++layer_index) {
        const Layer* layer = m_object.layers()[layer_index];
        raw_overhang_region_count += layer->loverhangs.size();
        ExPolygons unioned = union_ex(layer->loverhangs);
        unioned_overhang_region_count += unioned.size();
        if (debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::OverhangRegions,
                "overhang_layer_normalization", "same_layer_geometric_union",
                {{"layer_index", static_cast<double>(layer_index)},
                 {"layer_s_mm", layer->slice_z},
                 {"raw_region_count", static_cast<double>(layer->loverhangs.size())},
                 {"unioned_region_count", static_cast<double>(unioned.size())}});
        }
        overhangs_by_layer.emplace_back(std::move(unioned));
    }
    if (debug != nullptr) {
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "raw_overhang_region_count",
                          static_cast<double>(raw_overhang_region_count));
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "unioned_overhang_region_count",
                          static_cast<double>(unioned_overhang_region_count));
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "duplicate_or_overlapping_regions_removed",
                          static_cast<double>(raw_overhang_region_count) -
                              static_cast<double>(unioned_overhang_region_count));
    }
    std::vector<BeltSupportContact> raw_contacts;
    size_t overhang_region_count = 0;
    size_t contact_safe_region_count = 0;
    size_t narrow_overhang_count = 0;
    size_t contact_candidate_count = 0;
    size_t rejected_before_task_origin_count = 0;
    size_t rejected_without_support_height_count = 0;
    size_t accepted_overhang_region_count = 0;
    size_t original_tip_cap_model_collision_count = 0;
    size_t tip_cap_model_collision_count = 0;
    size_t tip_cap_clear_count = 0;
    size_t adjusted_contact_center_count = 0;
    size_t unplaceable_contact_center_count = 0;
    std::set<size_t> raw_contact_source_regions;
    struct OverhangRegionInfo
    {
        size_t layer_index{0};
        ExPolygon geometry;
    };
    std::vector<OverhangRegionInfo> overhang_region_infos;
    if (debug != nullptr) {
        BeltSupportDebugStageTimer timer(debug, BeltSupportDebugStageId::InputSlices);
        for (const Layer* layer : m_object.layers()) {
            detector.throw_on_cancel();
            debug_expolygons(debug, BeltSupportDebugStageId::InputSlices, layer->lslices,
                             layer->slice_z, center_u, center_v, *coordinates, "model_slice");
        }
    }
    if (debug != nullptr) {
        BeltSupportDebugStageTimer timer(debug, BeltSupportDebugStageId::OverhangRegions);
        for (size_t layer_index = 0; layer_index < m_object.layers().size(); ++layer_index) {
            const Layer* layer = m_object.layers()[layer_index];
            detector.throw_on_cancel();
            debug_expolygons(debug, BeltSupportDebugStageId::OverhangRegions, layer->loverhangs,
                             layer->slice_z, center_u, center_v, *coordinates, "overhang");
            debug_expolygons(debug, BeltSupportDebugStageId::OverhangRegions,
                             overhangs_by_layer[layer_index], layer->slice_z,
                             center_u, center_v, *coordinates, "unioned_overhang");
        }
    }
    {
        BeltSupportDebugStageTimer timer(debug, BeltSupportDebugStageId::RawContacts);
        for (size_t layer_index = 0; layer_index < m_object.layers().size(); ++layer_index) {
            const Layer* layer = m_object.layers()[layer_index];
            detector.throw_on_cancel();
            const ExPolygons& layer_overhangs = overhangs_by_layer[layer_index];
            if (layer_overhangs.empty())
                continue;
            const double layer_tip_s = layer->slice_z - top_gap;
            const ExPolygons model_at_tip = model_clearance_at(
                m_object, layer_tip_s,
                std::max(m_slicing_parameters.layer_height, 0.2), 0.0);

            for (const ExPolygon& overhang : layer_overhangs) {
                const size_t overhang_region_index = overhang_region_count++;
                overhang_region_infos.push_back({layer_index, overhang});
                std::vector<Point> samples;
                append_contact_samples(overhang, sample_spacing, samples);
                contact_candidate_count += samples.size();
                ExPolygons contact_safe_regions = offset_ex(overhang, -scale_(contact_inset));
                if (contact_safe_regions.empty()) {
                    ++narrow_overhang_count;
                } else {
                    // The eroded core is diagnostic only. Belt overhang strips
                    // are often thinner than the configured inset, but they
                    // still require contacts sampled from the original region.
                    contact_safe_region_count += contact_safe_regions.size();
                    debug_expolygons(debug, BeltSupportDebugStageId::RawContacts,
                                     contact_safe_regions, layer->slice_z,
                                     center_u, center_v, *coordinates, "contact_safe_region");
                }
                size_t accepted_sample_count = 0;
                for (const Point& sample : samples) {
                    const double tip_s = layer->slice_z - top_gap;
                    const ExPolygons original_cap{
                        ExPolygon(rectangle_at(
                            sample, tip_half_width, tip_half_width))};
                    const double original_model_overlap =
                        overlap_area_mm2(original_cap, model_at_tip);
                    if (original_model_overlap > 1e-8)
                        ++original_tip_cap_model_collision_count;

                    const std::optional<Point> clear_center =
                        nearest_clear_tip_center(
                            sample, tip_half_width, model_at_tip);
                    const Point center = clear_center.value_or(sample);
                    const bool center_adjusted = center != sample;
                    if (center_adjusted)
                        ++adjusted_contact_center_count;
                    if (!clear_center.has_value())
                        ++unplaceable_contact_center_count;

                    const double global_u = unscaled<double>(center.x()) + center_u;
                    const double global_v = unscaled<double>(center.y()) + center_v;
                    const double root_s = -global_v / coordinates->cot_angle();
                    const Vec3d witness_world = local_point_to_world(
                        sample, tip_s, center_u, center_v, *coordinates);
                    const Vec3d center_world = local_point_to_world(
                        center, tip_s, center_u, center_v, *coordinates);
                    if (root_s < -EPSILON) {
                        ++rejected_before_task_origin_count;
                        if (debug != nullptr) {
                            debug->add_record(
                                BeltSupportDebugStageId::RawContacts,
                                "contact_sample", "root_before_task_origin",
                                {{"overhang_region_index", static_cast<double>(overhang_region_index)},
                                 {"layer_index", static_cast<double>(layer_index)},
                                 {"layer_s_mm", layer->slice_z},
                                 {"tip_s_mm", tip_s},
                                 {"root_s_mm", root_s},
                                 {"witness_world_x_mm", witness_world.x()},
                                 {"witness_world_y_mm", witness_world.y()},
                                 {"witness_world_z_mm", witness_world.z()},
                                 {"center_world_x_mm", center_world.x()},
                                 {"center_world_y_mm", center_world.y()},
                                 {"center_world_z_mm", center_world.z()}});
                        }
                        continue;
                    }
                    if (tip_s <= root_s + 0.5 * m_slicing_parameters.layer_height) {
                        ++rejected_without_support_height_count;
                        if (debug != nullptr) {
                            debug->add_record(
                                BeltSupportDebugStageId::RawContacts,
                                "contact_sample", "no_printable_support_height",
                                {{"overhang_region_index", static_cast<double>(overhang_region_index)},
                                 {"layer_index", static_cast<double>(layer_index)},
                                 {"layer_s_mm", layer->slice_z},
                                 {"tip_s_mm", tip_s},
                                 {"root_s_mm", root_s},
                                 {"witness_world_x_mm", witness_world.x()},
                                 {"witness_world_y_mm", witness_world.y()},
                                 {"witness_world_z_mm", witness_world.z()},
                                 {"center_world_x_mm", center_world.x()},
                                 {"center_world_y_mm", center_world.y()},
                                 {"center_world_z_mm", center_world.z()}});
                        }
                        continue;
                    }

                    BeltSupportContact contact;
                    contact.center_local = center;
                    contact.witness_local = sample;
                    contact.root_u_local = center.x();
                    contact.root_v_local = center.y();
                    contact.tip_s = tip_s;
                    contact.root_s = std::max(0.0, root_s);
                    contact.source_overhang_region_index = overhang_region_index;
                    contact.source_layer_index = layer_index;
                    contact.center_adjusted = center_adjusted;
                    contact.original_tip_cap_model_overlap_area =
                        original_model_overlap;
                    const ExPolygons cap{
                        ExPolygon(rectangle_at(
                            center, tip_half_width, tip_half_width))};
                    contact.tip_cap_model_overlap_area =
                        overlap_area_mm2(cap, model_at_tip);
                    contact.tip_cap_overhang_overlap_area =
                        overlap_area_mm2(cap, ExPolygons{overhang});
                    if (contact.tip_cap_model_overlap_area > 1e-8)
                        ++tip_cap_model_collision_count;
                    else
                        ++tip_cap_clear_count;
                    const size_t raw_contact_index = raw_contacts.size();
                    contact.source_witness_indices = {raw_contact_index};
                    raw_contacts.push_back(contact);
                    raw_contact_source_regions.insert(overhang_region_index);
                    ++accepted_sample_count;
                    if (debug != nullptr) {
                        debug->add_point(
                            BeltSupportDebugStageId::RawContacts,
                            witness_world, 0.25, "raw_contact_witness");
                        debug->add_point(
                            BeltSupportDebugStageId::RawContacts,
                            center_world, 0.22,
                            clear_center.has_value()
                                ? "clear_contact_center"
                                : "unplaceable_contact_center");
                        if (center_adjusted) {
                            debug->add_line(
                                BeltSupportDebugStageId::RawContacts,
                                witness_world, center_world,
                                "contact_center_adjustment");
                        }
                        debug->add_record(
                            BeltSupportDebugStageId::RawContacts, "contact_sample",
                            clear_center.has_value()
                                ? (center_adjusted
                                    ? "accepted_with_adjusted_clear_center"
                                    : "accepted_with_original_clear_center")
                                : "accepted_without_clear_tip_center",
                            {{"raw_contact_index", static_cast<double>(raw_contact_index)},
                             {"overhang_region_index", static_cast<double>(overhang_region_index)},
                             {"layer_index", static_cast<double>(layer_index)},
                             {"layer_s_mm", layer->slice_z},
                             {"tip_s_mm", tip_s},
                             {"root_s_mm", root_s},
                             {"center_adjusted", center_adjusted ? 1.0 : 0.0},
                             {"center_offset_mm",
                              unscaled<double>((center - sample).norm())},
                             {"original_tip_cap_model_overlap_area_mm2",
                              contact.original_tip_cap_model_overlap_area},
                             {"tip_cap_model_overlap_area_mm2",
                              contact.tip_cap_model_overlap_area},
                             {"tip_cap_overhang_overlap_area_mm2",
                              contact.tip_cap_overhang_overlap_area},
                             {"witness_world_x_mm", witness_world.x()},
                             {"witness_world_y_mm", witness_world.y()},
                             {"witness_world_z_mm", witness_world.z()},
                             {"center_world_x_mm", center_world.x()},
                             {"center_world_y_mm", center_world.y()},
                             {"center_world_z_mm", center_world.z()}});
                    }

                }
                if (accepted_sample_count > 0)
                    ++accepted_overhang_region_count;
                if (debug != nullptr) {
                    debug->add_record(
                        BeltSupportDebugStageId::RawContacts,
                        "overhang_region_sampling",
                        accepted_sample_count > 0 ? "region_has_contact_candidate"
                                                  : "region_has_no_contact_candidate",
                        {{"overhang_region_index", static_cast<double>(overhang_region_index)},
                         {"layer_index", static_cast<double>(layer_index)},
                         {"layer_s_mm", layer->slice_z},
                         {"area_mm2", unscaled<double>(unscaled<double>(std::abs(overhang.area())))},
                         {"candidate_count", static_cast<double>(samples.size())},
                         {"accepted_count", static_cast<double>(accepted_sample_count)}});
                }
            }
        }
    }

    // Overhang detection emits a 2D cross-section on every slicing layer. A
    // continuous sloped surface therefore creates a dense sequence of raw
    // witnesses even though the configured branch/contact spacing is much
    // larger than one layer. Preserve every raw witness for audit, but connect
    // geometrically adjacent regions into surface components and select a
    // spacing-bounded set of real contact tips from those witnesses.
    std::vector<size_t> region_parents(overhang_region_infos.size());
    std::iota(region_parents.begin(), region_parents.end(), size_t{0});
    auto find_region_root = [&region_parents](size_t region) {
        size_t root = region;
        while (region_parents[root] != root)
            root = region_parents[root];
        while (region_parents[region] != region) {
            const size_t parent = region_parents[region];
            region_parents[region] = root;
            region = parent;
        }
        return root;
    };
    auto join_regions = [&region_parents, &find_region_root](size_t first, size_t second) {
        const size_t first_root = find_region_root(first);
        const size_t second_root = find_region_root(second);
        if (first_root != second_root)
            region_parents[second_root] = first_root;
    };
    const double surface_connection_tolerance = std::max(
        static_cast<double>(support_parameters.support_material_flow.width()),
        m_slicing_parameters.layer_height);
    std::vector<ExPolygons> expanded_overhang_regions;
    expanded_overhang_regions.reserve(overhang_region_infos.size());
    for (const OverhangRegionInfo& region : overhang_region_infos) {
        expanded_overhang_regions.emplace_back(offset_ex(
            region.geometry, scale_(surface_connection_tolerance)));
    }
    size_t cross_layer_surface_link_count = 0;
    for (size_t first = 0; first < overhang_region_infos.size(); ++first) {
        for (size_t second = first + 1;
             second < overhang_region_infos.size(); ++second) {
            const size_t first_layer = overhang_region_infos[first].layer_index;
            const size_t second_layer = overhang_region_infos[second].layer_index;
            if (second_layer > first_layer + 1)
                break;
            if (second_layer == first_layer)
                continue;
            if (intersection_ex(
                    expanded_overhang_regions[first],
                    ExPolygons{overhang_region_infos[second].geometry}).empty()) {
                continue;
            }
            join_regions(first, second);
            ++cross_layer_surface_link_count;
        }
    }

    std::map<size_t, size_t> compact_surface_components;
    std::vector<size_t> region_surface_components(overhang_region_infos.size());
    for (size_t region = 0; region < overhang_region_infos.size(); ++region) {
        const size_t root = find_region_root(region);
        const auto [component_it, inserted] = compact_surface_components.emplace(
            root, compact_surface_components.size());
        (void)inserted;
        region_surface_components[region] = component_it->second;
    }

    std::vector<Vec3d> raw_witness_world;
    raw_witness_world.reserve(raw_contacts.size());
    std::map<size_t, std::vector<size_t>> raw_contacts_by_surface_component;
    for (size_t raw_index = 0; raw_index < raw_contacts.size(); ++raw_index) {
        BeltSupportContact& raw = raw_contacts[raw_index];
        raw.source_surface_component_index =
            region_surface_components[raw.source_overhang_region_index];
        raw_contacts_by_surface_component[raw.source_surface_component_index]
            .push_back(raw_index);
        raw_witness_world.emplace_back(local_point_to_world(
            raw.witness_local, raw.tip_s,
            center_u, center_v, *coordinates));
    }

    const double maximum_lateral_angle = std::clamp(
        m_object.config().tree_support_branch_angle_organic.value *
            M_PI / 180.0,
        0.0, 0.5 * M_PI - EPSILON);
    const double maximum_lateral_slope = std::tan(maximum_lateral_angle);
    const double interface_depth =
        std::max(0, m_object.config().support_interface_top_layers.value) *
        m_slicing_parameters.layer_height;

    // Contact consolidation may remove duplicate samples on the same physical
    // layer only. Before routing, a point on another layer has no known branch
    // center at the witness plane; using the theoretical branch-angle budget
    // to merge it may select a route which moves in the opposite direction and
    // never covers the original witness.
    auto contact_covers_witness =
        [&](const BeltSupportContact& candidate,
            const BeltSupportContact& witness) {
            const double delta_s = candidate.tip_s - witness.tip_s;
            if (std::abs(delta_s) > EPSILON)
                return false;
            const double du = unscaled<double>(
                candidate.center_local.x() - witness.witness_local.x());
            const double dv = unscaled<double>(
                candidate.center_local.y() - witness.witness_local.y());
            const double permitted_lateral_distance = tip_half_width;
            return std::hypot(du, dv) <=
                permitted_lateral_distance + EPSILON;
        };

    std::vector<BeltSupportContact> contacts;
    std::vector<size_t> raw_to_effective_contact(
        raw_contacts.size(), BeltSupportRoute::no_parent);
    const double witness_mapping_radius = tip_half_width;
    size_t contacts_removed_by_spacing_coverage = 0;
    for (const auto& [surface_component, raw_indices] :
         raw_contacts_by_surface_component) {
        std::map<size_t, std::vector<size_t>> candidate_coverage;
        for (const size_t candidate_index : raw_indices) {
            std::vector<size_t>& covered = candidate_coverage[candidate_index];
            for (const size_t witness_index : raw_indices) {
                if (contact_covers_witness(
                        raw_contacts[candidate_index],
                        raw_contacts[witness_index])) {
                    covered.push_back(witness_index);
                }
            }
        }

        std::set<size_t> uncovered(raw_indices.begin(), raw_indices.end());
        while (!uncovered.empty()) {
            size_t best_raw_index = *uncovered.begin();
            std::vector<size_t> best_covered;
            for (const size_t candidate_index : raw_indices) {
                std::vector<size_t> covered;
                for (const size_t witness_index : candidate_coverage[candidate_index]) {
                    if (uncovered.count(witness_index) != 0)
                        covered.push_back(witness_index);
                }
                const BeltSupportContact& candidate = raw_contacts[candidate_index];
                const BeltSupportContact& best = raw_contacts[best_raw_index];
                if (covered.size() > best_covered.size() ||
                    (covered.size() == best_covered.size() &&
                     (candidate.tip_cap_model_overlap_area <
                          best.tip_cap_model_overlap_area - EPSILON ||
                      (std::abs(candidate.tip_cap_model_overlap_area -
                                best.tip_cap_model_overlap_area) <= EPSILON &&
                       candidate.tip_s > best.tip_s)))) {
                    best_raw_index = candidate_index;
                    best_covered = std::move(covered);
                }
            }
            if (best_covered.empty())
                best_covered.push_back(*uncovered.begin());

            BeltSupportContact effective = raw_contacts[best_raw_index];
            effective.source_surface_component_index = surface_component;
            effective.source_witness_indices = best_covered;
            const size_t effective_index = contacts.size();
            contacts.emplace_back(std::move(effective));
            for (const size_t raw_index : best_covered) {
                raw_to_effective_contact[raw_index] = effective_index;
                uncovered.erase(raw_index);
            }
            contacts_removed_by_spacing_coverage += best_covered.size() - 1;
        }
    }

    size_t unmapped_raw_witness_count = 0;
    double total_witness_mapping_distance = 0.0;
    double maximum_witness_mapping_distance = 0.0;
    std::set<size_t> mapped_source_regions;
    for (size_t raw_index = 0; raw_index < raw_contacts.size(); ++raw_index) {
        const size_t effective_index = raw_to_effective_contact[raw_index];
        if (effective_index == BeltSupportRoute::no_parent ||
            effective_index >= contacts.size()) {
            ++unmapped_raw_witness_count;
            continue;
        }
        const BeltSupportContact& effective = contacts[effective_index];
        const Vec3d effective_world = local_point_to_world(
            effective.center_local, effective.tip_s,
            center_u, center_v, *coordinates);
        const double mapping_distance =
            (raw_witness_world[raw_index] - effective_world).norm();
        total_witness_mapping_distance += mapping_distance;
        maximum_witness_mapping_distance = std::max(
            maximum_witness_mapping_distance, mapping_distance);
        mapped_source_regions.insert(
            raw_contacts[raw_index].source_overhang_region_index);
        if (debug != nullptr) {
            debug->add_line(
                BeltSupportDebugStageId::ConsolidatedContacts,
                raw_witness_world[raw_index], effective_world,
                "raw_witness_to_effective_contact");
            debug->add_record(
                BeltSupportDebugStageId::ConsolidatedContacts,
                "witness_contact_mapping", "mapped_within_configured_spacing",
                {{"raw_witness_index", static_cast<double>(raw_index)},
                 {"effective_contact_index", static_cast<double>(effective_index)},
                 {"overhang_region_index", static_cast<double>(
                      raw_contacts[raw_index].source_overhang_region_index)},
                 {"surface_component_index", static_cast<double>(
                      raw_contacts[raw_index].source_surface_component_index)},
                 {"mapping_distance_mm", mapping_distance},
                 {"mapping_radius_mm", witness_mapping_radius}});
        }
    }
    std::set<size_t> effective_contact_source_regions;
    {
        BeltSupportDebugStageTimer timer(debug, BeltSupportDebugStageId::ConsolidatedContacts);
        for (size_t contact_index = 0; contact_index < contacts.size(); ++contact_index) {
            const BeltSupportContact& contact = contacts[contact_index];
            effective_contact_source_regions.insert(
                contact.source_overhang_region_index);
            if (debug != nullptr) {
                const Vec3d world = local_point_to_world(
                    contact.center_local, contact.tip_s,
                    center_u, center_v, *coordinates);
                debug->add_point(
                    BeltSupportDebugStageId::ConsolidatedContacts,
                    world,
                    0.4, "effective_contact");
                debug->add_record(
                    BeltSupportDebugStageId::ConsolidatedContacts, "effective_contact",
                    "validated_printable_contact",
                    {{"contact_index", static_cast<double>(contact_index)},
                     {"tip_s_mm", contact.tip_s},
                     {"root_s_mm", contact.root_s},
                     {"overhang_region_index",
                      static_cast<double>(contact.source_overhang_region_index)},
                     {"surface_component_index",
                      static_cast<double>(contact.source_surface_component_index)},
                     {"covered_raw_witness_count",
                      static_cast<double>(contact.source_witness_indices.size())},
                     {"layer_index", static_cast<double>(contact.source_layer_index)},
                     {"world_x_mm", world.x()},
                     {"world_y_mm", world.y()},
                     {"world_z_mm", world.z()}});
            }
        }
    }
    if (debug != nullptr) {
        const double configured_threshold =
            m_object.config().support_threshold_angle.value;
        const double effective_threshold = configured_threshold > EPSILON
            ? std::min(configured_threshold + 1.0, 89.0)
            : 30.0;
        const double threshold_radians = Geometry::deg2rad(effective_threshold);
        const double allowed_layer_expansion =
            m_slicing_parameters.layer_height / std::tan(threshold_radians);
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "configured_threshold_angle_deg", configured_threshold);
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "effective_threshold_angle_deg", effective_threshold);
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "allowed_layer_expansion_mm", allowed_layer_expansion);
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "belt_gantry_angle_deg", coordinates->angle_degrees());
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "build_normal_x", coordinates->normal().x());
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "build_normal_y", coordinates->normal().y());
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions,
                          "build_normal_z", coordinates->normal().z());
        debug->set_metric(BeltSupportDebugStageId::InputSlices, "model_layer_count",
                          static_cast<double>(m_object.layers().size()));
        debug->set_metric(BeltSupportDebugStageId::OverhangRegions, "overhang_layer_count",
                          static_cast<double>(std::count_if(
                              m_object.layers().begin(), m_object.layers().end(),
                              [](const Layer* layer) { return !layer->loverhangs.empty(); })));
        debug->set_metric(BeltSupportDebugStageId::RawContacts, "raw_contact_count",
                          static_cast<double>(raw_contacts.size()));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "contact_candidate_count",
                          static_cast<double>(contact_candidate_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "accepted_overhang_region_count",
                          static_cast<double>(accepted_overhang_region_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "rejected_before_task_origin_count",
                          static_cast<double>(rejected_before_task_origin_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "rejected_without_support_height_count",
                          static_cast<double>(rejected_without_support_height_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts, "sample_spacing_mm",
                          unscaled<double>(sample_spacing));
        debug->set_metric(BeltSupportDebugStageId::RawContacts, "contact_inset_mm",
                          contact_inset);
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "configured_tree_tip_half_width_mm",
                          configured_tree_tip_half_width);
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "effective_rectangular_tip_half_width_mm",
                          tip_half_width);
        debug->set_metric(BeltSupportDebugStageId::RawContacts, "overhang_region_count",
                          static_cast<double>(overhang_region_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts, "contact_safe_region_count",
                          static_cast<double>(contact_safe_region_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "narrow_overhang_count",
                          static_cast<double>(narrow_overhang_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "original_tip_cap_model_collision_count",
                          static_cast<double>(original_tip_cap_model_collision_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "tip_cap_model_collision_count",
                          static_cast<double>(tip_cap_model_collision_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "tip_cap_clear_count",
                          static_cast<double>(tip_cap_clear_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "adjusted_contact_center_count",
                          static_cast<double>(adjusted_contact_center_count));
        debug->set_metric(BeltSupportDebugStageId::RawContacts,
                          "unplaceable_contact_center_count",
                          static_cast<double>(unplaceable_contact_center_count));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "effective_contact_count", static_cast<double>(contacts.size()));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "contacts_removed_by_spacing_coverage",
                          static_cast<double>(contacts_removed_by_spacing_coverage));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "surface_component_count",
                          static_cast<double>(compact_surface_components.size()));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "cross_layer_surface_link_count",
                          static_cast<double>(cross_layer_surface_link_count));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "surface_connection_tolerance_mm",
                          surface_connection_tolerance);
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "witness_mapping_radius_mm",
                          witness_mapping_radius);
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "unmapped_raw_witness_count",
                          static_cast<double>(unmapped_raw_witness_count));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "mean_witness_mapping_distance_mm",
                          raw_contacts.empty()
                              ? 0.0
                              : total_witness_mapping_distance /
                                    static_cast<double>(raw_contacts.size()));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "maximum_witness_mapping_distance_mm",
                          maximum_witness_mapping_distance);
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "raw_contact_source_region_count",
                          static_cast<double>(raw_contact_source_regions.size()));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "effective_contact_source_region_count",
                          static_cast<double>(effective_contact_source_regions.size()));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "source_regions_mapped_by_consolidation",
                          static_cast<double>(mapped_source_regions.size()));
        debug->set_metric(BeltSupportDebugStageId::ConsolidatedContacts,
                          "source_regions_unmapped_by_consolidation",
                          static_cast<double>(raw_contact_source_regions.size() -
                                              mapped_source_regions.size()));
    }

    if (contacts.empty()) {
        BOOST_LOG_TRIVIAL(info) << "BeltTaperedSupport: no printable support contacts";
        return;
    }

    {
        BeltSupportDebugStageTimer timer(debug, BeltSupportDebugStageId::RootProjection);
        for (size_t contact_index = 0; contact_index < contacts.size(); ++contact_index) {
            const BeltSupportContact& contact = contacts[contact_index];
            const Vec3d tip = local_point_to_world(
                contact.center_local, contact.tip_s, center_u, center_v, *coordinates);
            const Point root_local(contact.center_local.x(), contact.center_local.y());
            const Vec3d root = local_point_to_world(
                root_local, contact.root_s, center_u, center_v, *coordinates);
            if (debug != nullptr) {
                debug->add_line(BeltSupportDebugStageId::RootProjection, tip, root,
                                "root_projection");
                debug->add_point(BeltSupportDebugStageId::RootProjection, root, 0.35,
                                 "projected_root");
                debug->add_record(
                    BeltSupportDebugStageId::RootProjection, "root_projection",
                    "parallel_to_negative_belt_normal",
                    {{"contact_index", static_cast<double>(contact_index)},
                     {"tip_world_x_mm", tip.x()},
                     {"tip_world_y_mm", tip.y()},
                     {"tip_world_z_mm", tip.z()},
                     {"root_world_x_mm", root.x()},
                     {"root_world_y_mm", root.y()},
                     {"root_world_z_mm", root.z()}});
            }
        }
    }
    if (debug != nullptr)
        debug->set_metric(BeltSupportDebugStageId::RootProjection, "projection_count",
                          static_cast<double>(contacts.size()));

    // Rebuild the complete print schedule from the same authoritative layer
    // height profile used by PrintObject::slice(). PrintObject::layers() may
    // have had empty bottom layers removed, while passing an empty profile to
    // generate_object_layers() silently falls back to min_layer_height in
    // release builds. Both would give support a different schedule from the
    // model.
    std::vector<coordf_t> layer_height_profile;
    PrintObject::update_layer_height_profile(
        *m_object.model_object(), m_slicing_parameters,
        layer_height_profile);
    std::vector<coordf_t> layer_boundaries = generate_object_layers(
        m_slicing_parameters, layer_height_profile,
        m_object.config().precise_z_height.value);
    std::vector<BeltSupportLayerPlan> plans;
    plans.reserve(layer_boundaries.size() / 2);
    double minimum_schedule_layer_height = std::numeric_limits<double>::max();
    double maximum_schedule_layer_height = 0.0;
    for (size_t index = 0; index + 1 < layer_boundaries.size(); index += 2) {
        const double low = layer_boundaries[index];
        const double high = layer_boundaries[index + 1];
        minimum_schedule_layer_height = std::min(
            minimum_schedule_layer_height, high - low);
        maximum_schedule_layer_height = std::max(
            maximum_schedule_layer_height, high - low);
        plans.push_back({0.5 * (low + high), high + m_slicing_parameters.object_print_z_min,
                         high - low, {}, {}, {}});
    }
    if (debug != nullptr) {
        debug->set_metric(BeltSupportDebugStageId::InputSlices,
                          "support_layer_plan_count",
                          static_cast<double>(plans.size()));
        debug->set_metric(BeltSupportDebugStageId::InputSlices,
                          "support_minimum_layer_height_mm",
                          plans.empty() ? 0.0 : minimum_schedule_layer_height);
        debug->set_metric(BeltSupportDebugStageId::InputSlices,
                          "support_maximum_layer_height_mm",
                          maximum_schedule_layer_height);
        debug->set_metric(BeltSupportDebugStageId::InputSlices,
                          "support_layer_height_profile_value_count",
                          static_cast<double>(layer_height_profile.size()));
    }
    auto coverage_plan_index_for_s = [&plans](double slice_s) {
        const auto plan_it = std::lower_bound(
            plans.begin(), plans.end(), slice_s,
            [](const BeltSupportLayerPlan& plan, double value) {
                return plan.slice_s < value;
            });
        if (plan_it == plans.begin())
            return size_t{0};
        if (plan_it == plans.end())
            return plans.size() - 1;
        const size_t upper_index = static_cast<size_t>(
            std::distance(plans.begin(), plan_it));
        return plans[upper_index].slice_s <= slice_s + EPSILON
            ? upper_index : upper_index - 1;
    };

    const double collision_tolerance = std::max(m_slicing_parameters.layer_height, 0.2);
    const double model_clearance = std::max(0.0, support_parameters.gap_xy);
    std::vector<BeltForbiddenLayer> model_solid_by_plan;
    std::vector<BeltForbiddenLayer> forbidden_by_plan;
    model_solid_by_plan.reserve(plans.size());
    forbidden_by_plan.reserve(plans.size());
    for (const BeltSupportLayerPlan& plan : plans) {
        BeltForbiddenLayer solid;
        solid.regions = model_clearance_at(
            m_object, plan.slice_s, collision_tolerance, 0.0);
        solid.bounds.reserve(solid.regions.size());
        for (const ExPolygon& region : solid.regions)
            solid.bounds.emplace_back(get_extents(region));

        BeltForbiddenLayer clearance;
        clearance.regions = model_clearance > EPSILON
            ? offset_ex(solid.regions, scale_(model_clearance))
            : solid.regions;
        clearance.bounds.reserve(clearance.regions.size());
        for (const ExPolygon& region : clearance.regions)
            clearance.bounds.emplace_back(get_extents(region));
        model_solid_by_plan.emplace_back(std::move(solid));
        forbidden_by_plan.emplace_back(std::move(clearance));
    }
    const double contact_cap_depth = std::max(
        2.0 * tip_half_width,
        top_gap + 2.0 * m_slicing_parameters.layer_height);
    std::vector<BeltSupportRoute> routes;
    routes.reserve(contacts.size());
    size_t reachable_contact_count = 0;
    size_t unreachable_contact_count = 0;
    size_t reachable_interval_count = 0;
    size_t blocked_interval_count = 0;
    {
        BeltSupportDebugStageTimer corridor_timer(debug, BeltSupportDebugStageId::ReachableCorridors);
        BeltSupportDebugStageTimer collision_timer(
            debug, BeltSupportDebugStageId::RouteClassification);
        for (size_t contact_index = 0; contact_index < contacts.size(); ++contact_index) {
            const BeltSupportContact& contact = contacts[contact_index];
            detector.throw_on_cancel();
            BeltSupportRoute route;
            route.contact_index = contact_index;
            route.v_local = contact.center_local.y();
            route.tip_s = contact.tip_s;
            route.root_s = contact.root_s;
            const double tip_u = unscaled<double>(contact.center_local.x());
            route.reachable_layers.push_back(
                {plans.size(), contact.tip_s, {{tip_u, tip_u}}});

            double previous_s = contact.tip_s;
            std::vector<BeltReachableInterval> reachable{{tip_u, tip_u}};
            for (std::ptrdiff_t plan_index = static_cast<std::ptrdiff_t>(plans.size()) - 1;
                 plan_index >= 0; --plan_index) {
                const BeltSupportLayerPlan& plan = plans[static_cast<size_t>(plan_index)];
                if (plan.slice_s >= contact.tip_s - EPSILON)
                    continue;
                if (plan.slice_s <= contact.root_s + EPSILON)
                    break;

                const double step_shift =
                    maximum_lateral_slope * (previous_s - plan.slice_s);
                std::vector<BeltReachableInterval> expanded;
                expanded.reserve(reachable.size());
                for (const BeltReachableInterval& interval : reachable)
                    expanded.push_back({interval.minimum_u - step_shift,
                                        interval.maximum_u + step_shift});
                expanded = merge_intervals(std::move(expanded));

                const double total_shift =
                    maximum_lateral_slope * (contact.tip_s - plan.slice_s);
                for (BeltReachableInterval& interval : expanded) {
                    interval.minimum_u = std::max(interval.minimum_u, tip_u - total_shift);
                    interval.maximum_u = std::min(interval.maximum_u, tip_u + total_shift);
                }
                expanded = merge_intervals(std::move(expanded));

                std::vector<BeltReachableInterval> blocked;
                const double distance_to_tip = contact.tip_s - plan.slice_s;
                const double structural_half_width = std::min(
                    maximum_half_width, tip_half_width + distance_to_tip * taper);
                if (distance_to_tip > contact_cap_depth) {
                    const BeltForbiddenLayer& forbidden =
                        forbidden_by_plan[static_cast<size_t>(plan_index)];
                    const double v = unscaled<double>(contact.center_local.y());
                    blocked = blocked_u_intervals(
                        forbidden, v, tip_half_width);
                }
                blocked_interval_count += blocked.size();
                reachable = subtract_intervals(expanded, blocked);
                if (debug != nullptr) {
                    for (const BeltReachableInterval& interval : blocked) {
                        const Point first = Point::new_scale(
                            interval.minimum_u,
                            unscaled<double>(contact.center_local.y()));
                        const Point second = Point::new_scale(
                            interval.maximum_u,
                            unscaled<double>(contact.center_local.y()));
                        debug->add_line(
                            BeltSupportDebugStageId::RouteClassification,
                            local_point_to_world(first, plan.slice_s, center_u, center_v,
                                                 *coordinates),
                            local_point_to_world(second, plan.slice_s, center_u, center_v,
                                                 *coordinates),
                            "collision_blocker");
                    }
                }
                if (reachable.empty()) {
                    route.failure_reason = "reachable_corridor_exhausted";
                    if (debug != nullptr) {
                        debug->add_record(
                            BeltSupportDebugStageId::RouteClassification,
                            "route_classification", route.failure_reason,
                            {{"contact_index", static_cast<double>(contact_index)},
                             {"accepted", 0.0},
                             {"failure_plan_index", static_cast<double>(plan_index)},
                             {"failure_slice_s_mm", plan.slice_s},
                             {"tip_s_mm", contact.tip_s},
                             {"root_s_mm", contact.root_s}});
                    }
                    break;
                }
                reachable_interval_count += reachable.size();
                route.reachable_layers.push_back(
                    {static_cast<size_t>(plan_index), plan.slice_s, reachable});
                if (debug != nullptr) {
                    for (const BeltReachableInterval& interval : reachable) {
                        const Point first = Point::new_scale(
                            interval.minimum_u,
                            unscaled<double>(contact.center_local.y()));
                        const Point second = Point::new_scale(
                            interval.maximum_u,
                            unscaled<double>(contact.center_local.y()));
                        debug->add_line(
                            BeltSupportDebugStageId::ReachableCorridors,
                            local_point_to_world(first, plan.slice_s, center_u, center_v,
                                                 *coordinates),
                            local_point_to_world(second, plan.slice_s, center_u, center_v,
                                                 *coordinates),
                            "reachable_corridor");
                    }
                    debug->add_record(
                        BeltSupportDebugStageId::ReachableCorridors,
                        "reachable_layer", "slope_limited_free_corridor",
                        {{"contact_index", static_cast<double>(contact_index)},
                         {"plan_index", static_cast<double>(plan_index)},
                         {"slice_s_mm", plan.slice_s},
                         {"interval_count", static_cast<double>(reachable.size())},
                         {"structural_half_width_mm", structural_half_width},
                         {"collision_half_width_mm", tip_half_width}});
                }
                previous_s = plan.slice_s;
            }

            if (route.failure_reason.empty()) {
                const double final_shift =
                    maximum_lateral_slope * std::max(0.0, previous_s - contact.root_s);
                std::vector<BeltReachableInterval> root_intervals;
                root_intervals.reserve(reachable.size());
                for (const BeltReachableInterval& interval : reachable)
                    root_intervals.push_back({interval.minimum_u - final_shift,
                                              interval.maximum_u + final_shift});
                route.reachable_layers.push_back(
                    {plans.size(), contact.root_s,
                     merge_intervals(std::move(root_intervals))});

                double chosen_u = closest_value_in_intervals(
                    route.reachable_layers.back().intervals, tip_u);
                std::vector<BeltRoutePoint> reverse_points;
                const double route_v = unscaled<double>(route.v_local);
                reverse_points.push_back({chosen_u, route_v, contact.root_s});
                for (size_t layer_index = route.reachable_layers.size() - 1;
                     layer_index > 0; --layer_index) {
                    const BeltReachableLayer& lower = route.reachable_layers[layer_index];
                    const BeltReachableLayer& upper = route.reachable_layers[layer_index - 1];
                    const double permitted = maximum_lateral_slope *
                        std::max(0.0, upper.slice_s - lower.slice_s);
                    std::vector<BeltReachableInterval> feasible;
                    for (const BeltReachableInterval& interval : upper.intervals) {
                        const double minimum = std::max(interval.minimum_u,
                                                        chosen_u - permitted);
                        const double maximum = std::min(interval.maximum_u,
                                                        chosen_u + permitted);
                        if (minimum <= maximum + EPSILON)
                            feasible.push_back({minimum, maximum});
                    }
                    if (feasible.empty()) {
                        route.failure_reason = "corridor_backtracking_failed";
                        break;
                    }
                    chosen_u = closest_value_in_intervals(feasible, tip_u);
                    reverse_points.push_back({chosen_u, route_v, upper.slice_s});
                }
                if (route.failure_reason.empty()) {
                    route.points.assign(reverse_points.rbegin(), reverse_points.rend());
                    const size_t dense_point_count = route.points.size();
                    route.points = simplify_route_in_corridor(route);
                    ++reachable_contact_count;
                    if (debug != nullptr) {
                        debug->add_record(
                            BeltSupportDebugStageId::RouteClassification,
                            "route_classification", "reachable_without_model_collision",
                            {{"contact_index", static_cast<double>(contact_index)},
                             {"accepted", 1.0},
                             {"dense_route_point_count",
                              static_cast<double>(dense_point_count)},
                             {"route_point_count", static_cast<double>(route.points.size())},
                             {"tip_s_mm", contact.tip_s},
                             {"root_s_mm", contact.root_s}});
                    }
                }
            }
            if (!route.failure_reason.empty())
                ++unreachable_contact_count;
            routes.emplace_back(std::move(route));
        }
    }

    // A tree contact does not have to reach the build plate independently. If
    // its collision-free corridor overlaps a route which already has a path to
    // the plate (possibly through another parent), it may terminate at that
    // branch. The previous implementation discarded these contacts before the
    // topology stage and therefore could never form a real tree.
    constexpr size_t merge_stability_layers = 3;
    const size_t direct_reachable_contact_count = reachable_contact_count;
    size_t rescued_contact_count = 0;
    auto backtrack_to_reachable_layer = [maximum_lateral_slope](
        BeltSupportRoute& route, size_t target_layer_index, double target_u) {
        if (target_layer_index == 0 ||
            target_layer_index >= route.reachable_layers.size()) {
            return false;
        }
        double chosen_u = target_u;
        std::vector<BeltRoutePoint> reverse_points;
        const double route_v = unscaled<double>(route.v_local);
        reverse_points.push_back(
            {chosen_u, route_v,
             route.reachable_layers[target_layer_index].slice_s});
        for (size_t layer_index = target_layer_index; layer_index > 0; --layer_index) {
            const BeltReachableLayer& lower = route.reachable_layers[layer_index];
            const BeltReachableLayer& upper = route.reachable_layers[layer_index - 1];
            const double permitted = maximum_lateral_slope *
                std::max(0.0, upper.slice_s - lower.slice_s);
            std::vector<BeltReachableInterval> feasible;
            for (const BeltReachableInterval& interval : upper.intervals) {
                const double minimum = std::max(interval.minimum_u,
                                                chosen_u - permitted);
                const double maximum = std::min(interval.maximum_u,
                                                chosen_u + permitted);
                if (minimum <= maximum + EPSILON)
                    feasible.push_back({minimum, maximum});
            }
            if (feasible.empty())
                return false;
            const double contact_u = route.reachable_layers.front().intervals.front().minimum_u;
            chosen_u = closest_value_in_intervals(feasible, contact_u);
            reverse_points.push_back({chosen_u, route_v, upper.slice_s});
        }

        BeltSupportRoute partial = route;
        partial.reachable_layers.resize(target_layer_index + 1);
        partial.points.assign(reverse_points.rbegin(), reverse_points.rend());
        partial.points = simplify_route_in_corridor(partial);
        if (partial.points.size() < 2)
            return false;
        route.points = std::move(partial.points);
        return true;
    };

    // Contacts are ordered from lower to higher object layers. A single pass
    // therefore lets a higher contact attach to every lower route resolved
    // earlier in the same pass, without repeatedly rescanning the full graph.
    for (size_t child_index = 0; child_index < routes.size(); ++child_index) {
            BeltSupportRoute& child = routes[child_index];
            if (child.failure_reason.empty() || child.reachable_layers.size() < 2)
                continue;

            size_t best_parent = BeltSupportRoute::no_parent;
            size_t best_layer_index = 0;
            double best_merge_s = std::numeric_limits<double>::lowest();
            double best_merge_u = 0.0;
            for (size_t parent_index = 0; parent_index < routes.size(); ++parent_index) {
                if (parent_index == child_index)
                    continue;
                const BeltSupportRoute& parent = routes[parent_index];
                if (!parent.failure_reason.empty() || parent.points.size() < 2)
                    continue;

                size_t consecutive_overlap = 0;
                for (size_t layer_index = 1;
                     layer_index < child.reachable_layers.size(); ++layer_index) {
                    const BeltReachableLayer& layer = child.reachable_layers[layer_index];
                    const double slice_s = layer.slice_s;
                    if (slice_s > child.tip_s - contact_cap_depth + EPSILON ||
                        slice_s > parent.tip_s - contact_cap_depth + EPSILON ||
                        slice_s > parent.points.front().s + EPSILON ||
                        slice_s < parent.points.back().s - EPSILON) {
                        consecutive_overlap = 0;
                        continue;
                    }

                    const double child_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + (child.tip_s - slice_s) * taper);
                    const double parent_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + (parent.tip_s - slice_s) * taper);
                    const double combined_half_width =
                        child_half_width + parent_half_width;
                    const double parent_v = route_v_at(parent, slice_s);
                    if (std::abs(unscaled<double>(child.v_local) - parent_v) >
                        combined_half_width + EPSILON) {
                        consecutive_overlap = 0;
                        continue;
                    }

                    const double parent_u = route_u_at(parent, slice_s);
                    std::vector<BeltReachableInterval> merge_intervals;
                    for (const BeltReachableInterval& interval : layer.intervals) {
                        const double minimum = std::max(
                            interval.minimum_u, parent_u - combined_half_width);
                        const double maximum = std::min(
                            interval.maximum_u, parent_u + combined_half_width);
                        if (minimum <= maximum + EPSILON)
                            merge_intervals.push_back({minimum, maximum});
                    }
                    if (merge_intervals.empty()) {
                        consecutive_overlap = 0;
                        continue;
                    }

                    ++consecutive_overlap;
                    if (consecutive_overlap < merge_stability_layers)
                        continue;
                    if (slice_s > best_merge_s) {
                        best_parent = parent_index;
                        best_layer_index = layer_index;
                        best_merge_s = slice_s;
                        best_merge_u = closest_value_in_intervals(
                            merge_intervals, parent_u);
                    }
                    break;
                }
            }

            if (best_parent == BeltSupportRoute::no_parent ||
                !backtrack_to_reachable_layer(
                    child, best_layer_index, best_merge_u)) {
                continue;
            }
            child.parent_route_index = best_parent;
            child.merge_s = best_merge_s;
            child.failure_reason.clear();
            ++rescued_contact_count;
            --unreachable_contact_count;
            ++reachable_contact_count;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "route_rescue", "merged_into_grounded_route",
                    {{"contact_index", static_cast<double>(child.contact_index)},
                     {"parent_contact_index",
                      static_cast<double>(routes[best_parent].contact_index)},
                     {"merge_s_mm", best_merge_s},
                     {"stability_layer_count",
                      static_cast<double>(merge_stability_layers)}});
            }
    }

    // Fixed-V is only one radial direction inside the configured branch cone.
    // If that slice is blocked, try a bounded family of straight V slopes and
    // spend the remaining lateral slope budget on the existing U corridor
    // search. Every attempt is recorded; no failed contact is silently dropped.
    size_t sloped_v_attempt_count = 0;
    size_t sloped_v_invalid_root_count = 0;
    size_t sloped_v_corridor_rejected_count = 0;
    size_t sloped_v_backtrack_rejected_count = 0;
    size_t sloped_v_rescued_contact_count = 0;
    auto try_sloped_v_route = [&](const BeltSupportContact& contact,
                                  size_t contact_index, double v_slope,
                                  const std::vector<BeltForbiddenLayer>& obstacles,
                                  bool ignore_collision_inside_contact_cap,
                                  BeltSupportRoute& result,
                                  std::string& rejection_reason,
                                  double& failure_slice_s) {
        const double tip_u = unscaled<double>(contact.center_local.x());
        const double tip_v_local = unscaled<double>(contact.center_local.y());
        const double tip_v_global = tip_v_local + center_v;
        const double denominator = v_slope - coordinates->cot_angle();
        if (std::abs(denominator) <= EPSILON) {
            rejection_reason = "parallel_to_build_plate";
            return false;
        }

        const double root_s =
            (tip_v_global + v_slope * contact.tip_s) / denominator;
        if (!std::isfinite(root_s) || root_s < -EPSILON ||
            root_s >= contact.tip_s - 0.5 * m_slicing_parameters.layer_height) {
            rejection_reason = "root_outside_printable_task";
            return false;
        }

        const double maximum_u_slope = std::sqrt(std::max(
            0.0,
            maximum_lateral_slope * maximum_lateral_slope -
                v_slope * v_slope));
        auto v_at = [&](double slice_s) {
            return tip_v_local + v_slope * (contact.tip_s - slice_s);
        };

        BeltSupportRoute route;
        route.contact_index = contact_index;
        route.v_local = contact.center_local.y();
        route.tip_s = contact.tip_s;
        route.root_s = std::max(0.0, root_s);
        route.reachable_layers.push_back(
            {plans.size(), contact.tip_s, {{tip_u, tip_u}}});

        double previous_s = contact.tip_s;
        std::vector<BeltReachableInterval> reachable{{tip_u, tip_u}};
        for (std::ptrdiff_t plan_index =
                 static_cast<std::ptrdiff_t>(plans.size()) - 1;
             plan_index >= 0; --plan_index) {
            const BeltSupportLayerPlan& plan = plans[static_cast<size_t>(plan_index)];
            if (plan.slice_s >= contact.tip_s - EPSILON)
                continue;
            if (plan.slice_s <= route.root_s + EPSILON)
                break;

            const double step_shift =
                maximum_u_slope * (previous_s - plan.slice_s);
            std::vector<BeltReachableInterval> expanded;
            expanded.reserve(reachable.size());
            for (const BeltReachableInterval& interval : reachable) {
                expanded.push_back({interval.minimum_u - step_shift,
                                    interval.maximum_u + step_shift});
            }
            expanded = merge_intervals(std::move(expanded));

            const double total_shift =
                maximum_u_slope * (contact.tip_s - plan.slice_s);
            for (BeltReachableInterval& interval : expanded) {
                interval.minimum_u = std::max(
                    interval.minimum_u, tip_u - total_shift);
                interval.maximum_u = std::min(
                    interval.maximum_u, tip_u + total_shift);
            }
            expanded = merge_intervals(std::move(expanded));

            const double distance_to_tip = contact.tip_s - plan.slice_s;
            std::vector<BeltReachableInterval> blocked;
            if (!ignore_collision_inside_contact_cap ||
                distance_to_tip > contact_cap_depth) {
                blocked = blocked_u_intervals(
                    obstacles[static_cast<size_t>(plan_index)],
                    v_at(plan.slice_s), tip_half_width);
            }
            reachable = subtract_intervals(expanded, blocked);
            if (reachable.empty()) {
                rejection_reason = "reachable_corridor_exhausted";
                failure_slice_s = plan.slice_s;
                return false;
            }
            route.reachable_layers.push_back(
                {static_cast<size_t>(plan_index), plan.slice_s, reachable});
            previous_s = plan.slice_s;
        }

        const double final_shift =
            maximum_u_slope * std::max(0.0, previous_s - route.root_s);
        std::vector<BeltReachableInterval> root_intervals;
        root_intervals.reserve(reachable.size());
        for (const BeltReachableInterval& interval : reachable) {
            root_intervals.push_back({interval.minimum_u - final_shift,
                                      interval.maximum_u + final_shift});
        }
        route.reachable_layers.push_back(
            {plans.size(), route.root_s,
             merge_intervals(std::move(root_intervals))});

        double chosen_u = closest_value_in_intervals(
            route.reachable_layers.back().intervals, tip_u);
        std::vector<BeltRoutePoint> reverse_points;
        reverse_points.push_back(
            {chosen_u, v_at(route.root_s), route.root_s});
        for (size_t layer_index = route.reachable_layers.size() - 1;
             layer_index > 0; --layer_index) {
            const BeltReachableLayer& lower = route.reachable_layers[layer_index];
            const BeltReachableLayer& upper = route.reachable_layers[layer_index - 1];
            const double permitted = maximum_u_slope *
                std::max(0.0, upper.slice_s - lower.slice_s);
            std::vector<BeltReachableInterval> feasible;
            for (const BeltReachableInterval& interval : upper.intervals) {
                const double minimum = std::max(
                    interval.minimum_u, chosen_u - permitted);
                const double maximum = std::min(
                    interval.maximum_u, chosen_u + permitted);
                if (minimum <= maximum + EPSILON)
                    feasible.push_back({minimum, maximum});
            }
            if (feasible.empty()) {
                rejection_reason = "corridor_backtracking_failed";
                failure_slice_s = upper.slice_s;
                return false;
            }
            chosen_u = closest_value_in_intervals(feasible, tip_u);
            reverse_points.push_back(
                {chosen_u, v_at(upper.slice_s), upper.slice_s});
        }

        route.points.assign(reverse_points.rbegin(), reverse_points.rend());
        route.points = simplify_route_in_corridor(route);
        if (route.points.size() < 2) {
            rejection_reason = "degenerate_route";
            return false;
        }
        route.failure_reason.clear();
        result = std::move(route);
        return true;
    };

    size_t solid_only_rescued_contact_count = 0;
    size_t solid_only_rejected_contact_count = 0;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;
        BeltSupportRoute candidate;
        std::string rejection_reason;
        double failure_slice_s = 0.0;
        const bool accepted = try_sloped_v_route(
            contacts[route.contact_index], route.contact_index, 0.0,
            model_solid_by_plan, false,
            candidate, rejection_reason, failure_slice_s);
        if (debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "route_solid_only_attempt",
                accepted ? "reachable_without_model_collision"
                         : rejection_reason,
                {{"contact_index", static_cast<double>(route.contact_index)},
                 {"accepted", accepted ? 1.0 : 0.0},
                 {"configured_xy_gap_mm", model_clearance},
                 {"failure_slice_s_mm", failure_slice_s}});
        }
        if (!accepted) {
            ++solid_only_rejected_contact_count;
            continue;
        }
        // Preserve this as a diagnostic only. A path which intersects the
        // configured XY clearance is not printable merely because it avoids
        // the model's solid volume.
        ++solid_only_rescued_contact_count;
    }

    constexpr size_t sloped_v_step_count = 8;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;

        bool rescued = false;
        for (size_t step = 1; step <= sloped_v_step_count && !rescued; ++step) {
            const double magnitude = maximum_lateral_slope * 0.95 *
                static_cast<double>(step) /
                static_cast<double>(sloped_v_step_count);
            for (const double sign : {-1.0, 1.0}) {
                const double v_slope = sign * magnitude;
                ++sloped_v_attempt_count;
                BeltSupportRoute candidate;
                std::string rejection_reason;
                double failure_slice_s = 0.0;
                const bool accepted = try_sloped_v_route(
                    contacts[route.contact_index], route.contact_index, v_slope,
                    forbidden_by_plan, false,
                    candidate,
                    rejection_reason, failure_slice_s);
                if (!accepted) {
                    if (rejection_reason == "root_outside_printable_task" ||
                        rejection_reason == "parallel_to_build_plate") {
                        ++sloped_v_invalid_root_count;
                    } else if (rejection_reason == "corridor_backtracking_failed") {
                        ++sloped_v_backtrack_rejected_count;
                    } else {
                        ++sloped_v_corridor_rejected_count;
                    }
                }
                if (debug != nullptr) {
                    debug->add_record(
                        BeltSupportDebugStageId::RouteClassification,
                        "route_sloped_v_attempt",
                        accepted ? "accepted" : rejection_reason,
                        {{"contact_index", static_cast<double>(route.contact_index)},
                         {"v_slope", v_slope},
                         {"branch_angle_deg", std::atan(std::abs(v_slope)) *
                              180.0 / M_PI},
                         {"accepted", accepted ? 1.0 : 0.0},
                         {"root_s_mm", accepted ? candidate.root_s : -1.0},
                         {"failure_slice_s_mm", failure_slice_s}});
                }
                if (!accepted)
                    continue;

                route = std::move(candidate);
                ++sloped_v_rescued_contact_count;
                ++reachable_contact_count;
                --unreachable_contact_count;
                rescued = true;
                if (debug != nullptr) {
                    for (size_t point_index = 1;
                         point_index < route.points.size(); ++point_index) {
                        debug->add_line(
                            BeltSupportDebugStageId::RouteClassification,
                            local_point_to_world(
                                Point::new_scale(route.points[point_index - 1].u,
                                                 route.points[point_index - 1].v),
                                route.points[point_index - 1].s,
                                center_u, center_v, *coordinates),
                            local_point_to_world(
                                Point::new_scale(route.points[point_index].u,
                                                 route.points[point_index].v),
                                route.points[point_index].s,
                                center_u, center_v, *coordinates),
                            "reachable_sloped_v_route");
                    }
                }
                break;
            }
        }
    }

    // A failed fixed-V route may still be a valid tree branch: it can move in
    // both U and V and terminate on an already grounded route. First rank a
    // bounded set of geometric candidates, then spend exact Clipper collision
    // checks only on those candidates. This avoids the rejected
    // contact x parent x layer exhaustive search.
    struct UvParentCandidate
    {
        size_t parent_index{BeltSupportRoute::no_parent};
        size_t merge_plan_index{0};
        double merge_s{0.0};
        double target_u{0.0};
        double target_v{0.0};
        double lateral_distance{0.0};
    };
    constexpr size_t maximum_exact_parent_candidates = 8;
    size_t uv_parent_rescued_contact_count = 0;
    size_t uv_parent_candidate_count = 0;
    size_t uv_parent_exact_candidate_count = 0;
    size_t uv_parent_angle_rejected_count = 0;
    size_t uv_parent_collision_rejected_count = 0;
    size_t uv_parent_stability_rejected_count = 0;
    {
        BeltSupportDebugStageTimer topology_timer(
            debug, BeltSupportDebugStageId::TreeTopology);
        for (size_t child_index = 0; child_index < routes.size(); ++child_index) {
            BeltSupportRoute& child = routes[child_index];
            if (child.failure_reason.empty())
                continue;
            const BeltSupportContact& child_contact = contacts[child.contact_index];
            const double tip_u = unscaled<double>(child_contact.center_local.x());
            const double tip_v = unscaled<double>(child_contact.center_local.y());
            std::vector<UvParentCandidate> candidates;

            for (size_t parent_index = 0;
                 parent_index < child_index; ++parent_index) {
                const BeltSupportRoute& parent = routes[parent_index];
                if (!parent.failure_reason.empty() || parent.points.size() < 2)
                    continue;
                const double highest_merge_s = std::min({
                    child.tip_s - contact_cap_depth,
                    parent.tip_s - contact_cap_depth,
                    parent.points.front().s,
                });
                const double lowest_merge_s = parent.points.back().s;
                if (highest_merge_s <= lowest_merge_s + EPSILON)
                    continue;

                bool angle_candidate_found = false;
                for (std::ptrdiff_t plan_index =
                         static_cast<std::ptrdiff_t>(plans.size()) - 1;
                     plan_index >= 0; --plan_index) {
                    const double merge_s =
                        plans[static_cast<size_t>(plan_index)].slice_s;
                    if (merge_s > highest_merge_s + EPSILON)
                        continue;
                    if (merge_s < lowest_merge_s - EPSILON)
                        break;
                    const double target_u = route_u_at(parent, merge_s);
                    const double target_v = route_v_at(parent, merge_s);
                    const double branch_span = child.tip_s - merge_s;
                    const double lateral_distance = std::hypot(
                        target_u - tip_u, target_v - tip_v);
                    if (lateral_distance >
                        maximum_lateral_slope * branch_span + EPSILON) {
                        continue;
                    }
                    candidates.push_back({
                        parent_index,
                        static_cast<size_t>(plan_index),
                        merge_s,
                        target_u,
                        target_v,
                        lateral_distance,
                    });
                    angle_candidate_found = true;
                    break;
                }
                if (!angle_candidate_found)
                    ++uv_parent_angle_rejected_count;
            }

            std::sort(candidates.begin(), candidates.end(),
                      [](const UvParentCandidate& first,
                         const UvParentCandidate& second) {
                          if (std::abs(first.merge_s - second.merge_s) > EPSILON)
                              return first.merge_s > second.merge_s;
                          return first.lateral_distance < second.lateral_distance;
                      });
            uv_parent_candidate_count += candidates.size();
            bool attached = false;
            size_t child_collision_rejected_count = 0;
            size_t child_stability_rejected_count = 0;
            const size_t exact_candidate_count = std::min(
                candidates.size(), maximum_exact_parent_candidates);
            for (size_t candidate_index = 0;
                 candidate_index < exact_candidate_count && !attached;
                 ++candidate_index) {
                ++uv_parent_exact_candidate_count;
                const UvParentCandidate& candidate = candidates[candidate_index];
                const BeltSupportRoute& parent = routes[candidate.parent_index];
                const double branch_span = child.tip_s - candidate.merge_s;
                auto child_center_at = [&](double slice_s) {
                    const double ratio = std::clamp(
                        (child.tip_s - slice_s) / branch_span, 0.0, 1.0);
                    return std::pair<double, double>{
                        tip_u + (candidate.target_u - tip_u) * ratio,
                        tip_v + (candidate.target_v - tip_v) * ratio};
                };

                size_t stable_overlap_layers = 0;
                for (size_t offset = 0; offset < merge_stability_layers; ++offset) {
                    const size_t overlap_plan_index =
                        candidate.merge_plan_index + offset;
                    if (overlap_plan_index >= plans.size())
                        break;
                    const double slice_s = plans[overlap_plan_index].slice_s;
                    if (slice_s >= child.tip_s - contact_cap_depth + EPSILON ||
                        slice_s > parent.points.front().s + EPSILON ||
                        slice_s < parent.points.back().s - EPSILON) {
                        break;
                    }
                    const auto [child_u, child_v] = child_center_at(slice_s);
                    const double parent_u = route_u_at(parent, slice_s);
                    const double parent_v = route_v_at(parent, slice_s);
                    const double child_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + (child.tip_s - slice_s) * taper);
                    const double parent_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + (parent.tip_s - slice_s) * taper);
                    if (std::hypot(child_u - parent_u, child_v - parent_v) >
                        child_half_width + parent_half_width + EPSILON) {
                        break;
                    }
                    ++stable_overlap_layers;
                }
                if (stable_overlap_layers < merge_stability_layers) {
                    ++child_stability_rejected_count;
                    ++uv_parent_stability_rejected_count;
                    continue;
                }

                bool collision = false;
                size_t validated_layer_count = 0;
                for (size_t scan_index = candidate.merge_plan_index;
                     scan_index < plans.size(); ++scan_index) {
                    const BeltSupportLayerPlan& scan_plan = plans[scan_index];
                    if (scan_plan.slice_s >= child.tip_s - EPSILON)
                        break;
                    ++validated_layer_count;
                    const auto [branch_u, branch_v] =
                        child_center_at(scan_plan.slice_s);
                    const double minimum_printable_v =
                        coordinates->belt_boundary_v(scan_plan.slice_s) - center_v;
                    if (branch_v < minimum_printable_v - EPSILON) {
                        collision = true;
                        break;
                    }
                    const double distance_to_tip =
                        child.tip_s - scan_plan.slice_s;
                    if (distance_to_tip <= contact_cap_depth + EPSILON)
                        continue;
                    const ExPolygon section(rectangle_at(
                        Point::new_scale(branch_u, branch_v),
                        tip_half_width, tip_half_width));
                    const BoundingBox section_bounds = get_extents(section);
                    const BeltForbiddenLayer& forbidden =
                        forbidden_by_plan[scan_index];
                    bool bounds_overlap = false;
                    for (const BoundingBox& bounds : forbidden.bounds) {
                        if (!bounds.defined || !section_bounds.defined ||
                            bounds.max.x() < section_bounds.min.x() ||
                            bounds.min.x() > section_bounds.max.x() ||
                            bounds.max.y() < section_bounds.min.y() ||
                            bounds.min.y() > section_bounds.max.y()) {
                            continue;
                        }
                        bounds_overlap = true;
                        break;
                    }
                    if (bounds_overlap && !intersection_ex(
                            ExPolygons{section}, forbidden.regions).empty()) {
                        collision = true;
                        break;
                    }
                }
                if (collision) {
                    ++child_collision_rejected_count;
                    ++uv_parent_collision_rejected_count;
                    continue;
                }

                child.points = {
                    {tip_u, tip_v, child.tip_s},
                    {candidate.target_u, candidate.target_v, candidate.merge_s},
                };
                child.parent_route_index = candidate.parent_index;
                child.merge_s = candidate.merge_s;
                child.failure_reason.clear();
                ++uv_parent_rescued_contact_count;
                ++reachable_contact_count;
                --unreachable_contact_count;
                attached = true;
                if (debug != nullptr) {
                    debug->add_line(
                        BeltSupportDebugStageId::RouteClassification,
                        local_point_to_world(
                            child_contact.center_local, child.tip_s,
                            center_u, center_v, *coordinates),
                        local_point_to_world(
                            Point::new_scale(candidate.target_u,
                                             candidate.target_v),
                            candidate.merge_s, center_u, center_v,
                            *coordinates),
                        "reachable_uv_parent_branch");
                    debug->add_record(
                        BeltSupportDebugStageId::RouteClassification,
                        "route_uv_parent_rescue",
                        "layer_validated_branch_to_grounded_parent",
                        {{"contact_index", static_cast<double>(child.contact_index)},
                         {"parent_contact_index",
                          static_cast<double>(parent.contact_index)},
                         {"merge_s_mm", candidate.merge_s},
                         {"lateral_distance_mm", candidate.lateral_distance},
                         {"branch_angle_deg",
                          std::atan2(candidate.lateral_distance, branch_span) *
                              180.0 / M_PI},
                         {"validated_layer_count",
                          static_cast<double>(validated_layer_count)}});
                }
            }

            if (!attached && debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "route_uv_parent_rescue", "no_valid_grounded_parent_branch",
                    {{"contact_index", static_cast<double>(child.contact_index)},
                     {"ranked_candidate_count",
                      static_cast<double>(candidates.size())},
                     {"exact_candidate_count",
                      static_cast<double>(exact_candidate_count)},
                     {"collision_rejected_count",
                      static_cast<double>(child_collision_rejected_count)},
                     {"stability_rejected_count",
                      static_cast<double>(child_stability_rejected_count)}});
            }
        }
    }

    struct BeltSearchNode
    {
        BeltRoutePoint point;
        size_t parent_node{BeltSupportRoute::no_parent};
        size_t plan_index{0};
        double score{0.0};
    };
    auto section_collides = [&](const BeltForbiddenLayer& obstacle,
                                double u, double v, double half_width) {
        if (obstacle.regions.empty())
            return false;
        const ExPolygon section(rectangle_at(
            Point::new_scale(u, v), half_width, half_width));
        const BoundingBox section_bounds = get_extents(section);
        bool bounds_overlap = false;
        for (const BoundingBox& bounds : obstacle.bounds) {
            if (!bounds.defined || !section_bounds.defined ||
                bounds.max.x() < section_bounds.min.x() ||
                bounds.min.x() > section_bounds.max.x() ||
                bounds.max.y() < section_bounds.min.y() ||
                bounds.min.y() > section_bounds.max.y()) {
                continue;
            }
            bounds_overlap = true;
            break;
        }
        return bounds_overlap && !intersection_ex(
            ExPolygons{section}, obstacle.regions).empty();
    };
    auto segment_stays_outside_model = [&](const BeltRoutePoint& upper,
                                           const BeltRoutePoint& lower,
                                           double route_tip_s) {
        const double span = upper.s - lower.s;
        if (span <= EPSILON)
            return false;
        const double lateral_distance = std::hypot(
            lower.u - upper.u, lower.v - upper.v);
        if (lateral_distance > maximum_lateral_slope * span + EPSILON)
            return false;
        const auto first_it = std::lower_bound(
            plans.begin(), plans.end(), lower.s - EPSILON,
            [](const BeltSupportLayerPlan& plan, double slice_s) {
                return plan.slice_s < slice_s;
            });
        for (auto plan_it = first_it;
             plan_it != plans.end() && plan_it->slice_s < upper.s - EPSILON;
             ++plan_it) {
            if (plan_it->slice_s < lower.s - EPSILON)
                continue;
            const double ratio = std::clamp(
                (upper.s - plan_it->slice_s) / span, 0.0, 1.0);
            const double u = upper.u + (lower.u - upper.u) * ratio;
            const double v = upper.v + (lower.v - upper.v) * ratio;
            const size_t plan_index = static_cast<size_t>(
                std::distance(plans.begin(), plan_it));
            if (section_collides(
                    forbidden_by_plan[plan_index], u, v,
                    tip_half_width)) {
                return false;
            }
        }
        return true;
    };

    constexpr size_t search_plan_stride = 8;
    constexpr size_t search_beam_width = 12;
    constexpr size_t search_direction_count = 8;
    constexpr size_t search_frontier_record_stride = 2;
    constexpr double search_grid_resolution = 0.45;
    size_t search_attempted_contact_count = 0;
    size_t search_rescued_contact_count = 0;
    size_t search_parent_attached_contact_count = 0;
    size_t search_rooted_contact_count = 0;
    size_t search_failed_contact_count = 0;
    size_t search_expanded_state_count = 0;
    size_t search_collision_rejected_state_count = 0;
    size_t search_below_plate_rejected_state_count = 0;

    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;
        ++search_attempted_contact_count;
        const BeltSupportContact& contact = contacts[route.contact_index];
        const BeltRoutePoint tip{
            unscaled<double>(contact.center_local.x()),
            unscaled<double>(contact.center_local.y()),
            contact.tip_s};
        std::vector<BeltSearchNode> nodes;
        nodes.push_back({tip, BeltSupportRoute::no_parent, plans.size(), 0.0});
        std::vector<size_t> beam{0};

        const auto below_tip_it = std::lower_bound(
            plans.begin(), plans.end(), contact.tip_s,
            [](const BeltSupportLayerPlan& plan, double slice_s) {
                return plan.slice_s < slice_s;
            });
        if (below_tip_it == plans.begin()) {
            ++search_failed_contact_count;
            continue;
        }
        std::ptrdiff_t target_plan_index = static_cast<std::ptrdiff_t>(
            std::distance(plans.begin(), below_tip_it)) - 1;
        target_plan_index = std::max<std::ptrdiff_t>(
            0, target_plan_index - static_cast<std::ptrdiff_t>(search_plan_stride - 1));

        bool found = false;
        bool found_root = false;
        size_t found_node_index = BeltSupportRoute::no_parent;
        size_t found_parent_route = BeltSupportRoute::no_parent;
        size_t search_level = 0;
        std::string search_failure = "search_space_exhausted";
        while (!beam.empty() && target_plan_index >= 0 && !found) {
            detector.throw_on_cancel();
            const size_t plan_index = static_cast<size_t>(target_plan_index);
            const double target_s = plans[plan_index].slice_s;
            const double minimum_printable_v =
                coordinates->belt_boundary_v(target_s) - center_v;

            struct SearchCandidate
            {
                BeltRoutePoint point;
                size_t parent_node{BeltSupportRoute::no_parent};
                double score{0.0};
            };
            std::map<std::pair<long long, long long>, SearchCandidate> candidates;
            size_t level_expanded_count = 0;
            size_t level_collision_rejected_count = 0;
            size_t level_below_plate_rejected_count = 0;
            for (const size_t node_index : beam) {
                const BeltSearchNode& source = nodes[node_index];
                const double delta_s = source.point.s - target_s;
                if (delta_s <= EPSILON)
                    continue;
                const double maximum_step = maximum_lateral_slope * delta_s * 0.98;
                std::vector<std::pair<double, double>> offsets;
                offsets.reserve(1 + search_direction_count * 2);
                offsets.emplace_back(0.0, 0.0);
                for (size_t direction = 0;
                     direction < search_direction_count; ++direction) {
                    const double angle = 2.0 * M_PI *
                        static_cast<double>(direction) /
                        static_cast<double>(search_direction_count);
                    offsets.emplace_back(
                        maximum_step * std::cos(angle),
                        maximum_step * std::sin(angle));
                    if (direction % 2 == 0) {
                        offsets.emplace_back(
                            0.5 * maximum_step * std::cos(angle),
                            0.5 * maximum_step * std::sin(angle));
                    }
                }

                for (const auto& [du, dv] : offsets) {
                    ++level_expanded_count;
                    const BeltRoutePoint candidate{
                        source.point.u + du,
                        source.point.v + dv,
                        target_s};
                    if (candidate.v < minimum_printable_v - EPSILON) {
                        ++level_below_plate_rejected_count;
                        continue;
                    }
                    if (!segment_stays_outside_model(
                            source.point, candidate, contact.tip_s)) {
                        ++level_collision_rejected_count;
                        continue;
                    }

                    const double distance_to_tip = contact.tip_s - target_s;
                    const double child_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + distance_to_tip * taper);
                    size_t nearest_parent = BeltSupportRoute::no_parent;
                    double nearest_parent_gap = std::numeric_limits<double>::max();
                    if (distance_to_tip > contact_cap_depth + EPSILON) {
                        for (size_t parent_index = 0;
                             parent_index < routes.size(); ++parent_index) {
                            if (parent_index == route_index)
                                continue;
                            const BeltSupportRoute& parent = routes[parent_index];
                            if (!parent.failure_reason.empty() ||
                                parent.points.size() < 2 ||
                                target_s > parent.points.front().s + EPSILON ||
                                target_s < parent.points.back().s - EPSILON ||
                                target_s > parent.tip_s - contact_cap_depth + EPSILON) {
                                continue;
                            }
                            const double parent_half_width = std::min(
                                maximum_half_width,
                                tip_half_width +
                                    std::max(0.0, parent.tip_s - target_s) * taper);
                            const double center_distance = std::hypot(
                                candidate.u - route_u_at(parent, target_s),
                                candidate.v - route_v_at(parent, target_s));
                            const double gap = std::max(
                                0.0,
                                center_distance - child_half_width - parent_half_width);
                            if (gap < nearest_parent_gap) {
                                nearest_parent_gap = gap;
                                nearest_parent = parent_index;
                            }
                        }
                    }

                    const double height_above_plate =
                        candidate.v - minimum_printable_v;
                    const double score =
                        0.65 * std::max(0.0, height_above_plate) +
                        0.35 * (std::isfinite(nearest_parent_gap)
                            ? nearest_parent_gap : height_above_plate) +
                        0.01 * std::hypot(candidate.u - tip.u,
                                          candidate.v - tip.v);
                    const auto key = std::make_pair(
                        static_cast<long long>(std::llround(
                            candidate.u / search_grid_resolution)),
                        static_cast<long long>(std::llround(
                            candidate.v / search_grid_resolution)));
                    auto existing = candidates.find(key);
                    if (existing == candidates.end() || score < existing->second.score) {
                        candidates[key] = {candidate, node_index, score};
                    }

                    if (nearest_parent != BeltSupportRoute::no_parent &&
                        nearest_parent_gap <= EPSILON) {
                        nodes.push_back({candidate, node_index, plan_index, score});
                        found = true;
                        found_node_index = nodes.size() - 1;
                        found_parent_route = nearest_parent;
                        break;
                    }

                    const double root_s =
                        -(candidate.v + center_v) / coordinates->cot_angle();
                    if (root_s >= -EPSILON && root_s < target_s - EPSILON &&
                        target_s - root_s <=
                            1.5 * search_plan_stride *
                                m_slicing_parameters.layer_height) {
                        const BeltRoutePoint root_point{
                            candidate.u,
                            coordinates->belt_boundary_v(std::max(0.0, root_s)) - center_v,
                            std::max(0.0, root_s)};
                        if (segment_stays_outside_model(
                                candidate, root_point, contact.tip_s)) {
                            nodes.push_back({candidate, node_index, plan_index, score});
                            const size_t candidate_node_index = nodes.size() - 1;
                            nodes.push_back({root_point, candidate_node_index,
                                             plans.size(), score});
                            found = true;
                            found_root = true;
                            found_node_index = nodes.size() - 1;
                            break;
                        }
                    }
                }
                if (found)
                    break;
            }

            search_expanded_state_count += level_expanded_count;
            search_collision_rejected_state_count +=
                level_collision_rejected_count;
            search_below_plate_rejected_state_count +=
                level_below_plate_rejected_count;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::ReachableCorridors,
                    "route_2d_search_level",
                    found ? "goal_reached" :
                        (candidates.empty() ? "frontier_exhausted" : "frontier_retained"),
                    {{"contact_index", static_cast<double>(route.contact_index)},
                     {"search_level", static_cast<double>(search_level)},
                     {"plan_index", static_cast<double>(plan_index)},
                     {"slice_s_mm", target_s},
                     {"input_frontier_count", static_cast<double>(beam.size())},
                     {"expanded_state_count", static_cast<double>(level_expanded_count)},
                     {"collision_rejected_state_count",
                      static_cast<double>(level_collision_rejected_count)},
                     {"below_plate_rejected_state_count",
                      static_cast<double>(level_below_plate_rejected_count)},
                     {"deduplicated_candidate_count",
                      static_cast<double>(candidates.size())}});
            }
            if (found)
                break;
            if (candidates.empty()) {
                search_failure = "frontier_exhausted_by_model";
                break;
            }

            std::vector<SearchCandidate> ranked;
            ranked.reserve(candidates.size());
            for (const auto& [key, candidate] : candidates)
                ranked.push_back(candidate);
            std::sort(ranked.begin(), ranked.end(),
                      [](const SearchCandidate& first,
                         const SearchCandidate& second) {
                          return first.score < second.score;
                      });
            if (ranked.size() > search_beam_width)
                ranked.resize(search_beam_width);
            beam.clear();
            beam.reserve(ranked.size());
            for (const SearchCandidate& candidate : ranked) {
                nodes.push_back({candidate.point, candidate.parent_node,
                                 plan_index, candidate.score});
                beam.push_back(nodes.size() - 1);
                if (debug != nullptr &&
                    search_level % search_frontier_record_stride == 0) {
                    debug->add_point(
                        BeltSupportDebugStageId::ReachableCorridors,
                        local_point_to_world(
                            Point::new_scale(candidate.point.u, candidate.point.v),
                            candidate.point.s, center_u, center_v, *coordinates),
                        0.12, "route_2d_search_frontier");
                }
            }

            ++search_level;
            if (target_plan_index == 0)
                break;
            target_plan_index = std::max<std::ptrdiff_t>(
                0, target_plan_index -
                    static_cast<std::ptrdiff_t>(search_plan_stride));
        }

        if (!found || found_node_index == BeltSupportRoute::no_parent) {
            ++search_failed_contact_count;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "route_2d_search", search_failure,
                    {{"contact_index", static_cast<double>(route.contact_index)},
                     {"accepted", 0.0},
                     {"search_level_count", static_cast<double>(search_level)},
                     {"stored_node_count", static_cast<double>(nodes.size())}});
            }
            continue;
        }

        std::vector<BeltRoutePoint> reverse_points;
        for (size_t node_index = found_node_index;
             node_index != BeltSupportRoute::no_parent;
             node_index = nodes[node_index].parent_node) {
            reverse_points.push_back(nodes[node_index].point);
        }
        std::vector<BeltRoutePoint> dense_points(
            reverse_points.rbegin(), reverse_points.rend());
        std::vector<BeltRoutePoint> simplified_points;
        simplified_points.reserve(dense_points.size());
        size_t first = 0;
        simplified_points.push_back(dense_points.front());
        while (first + 1 < dense_points.size()) {
            size_t last = dense_points.size() - 1;
            while (last > first + 1 &&
                   !segment_stays_outside_model(
                       dense_points[first], dense_points[last], contact.tip_s)) {
                --last;
            }
            simplified_points.push_back(dense_points[last]);
            first = last;
        }

        route.points = std::move(simplified_points);
        route.reachable_layers.clear();
        route.root_s = route.points.back().s;
        route.parent_route_index = found_root
            ? BeltSupportRoute::no_parent : found_parent_route;
        route.merge_s = found_root ? 0.0 : route.points.back().s;
        route.failure_reason.clear();
        ++search_rescued_contact_count;
        ++reachable_contact_count;
        --unreachable_contact_count;
        if (found_root)
            ++search_rooted_contact_count;
        else
            ++search_parent_attached_contact_count;
        if (debug != nullptr) {
            for (size_t point_index = 1;
                 point_index < route.points.size(); ++point_index) {
                debug->add_line(
                    BeltSupportDebugStageId::RouteClassification,
                    local_point_to_world(
                        Point::new_scale(route.points[point_index - 1].u,
                                         route.points[point_index - 1].v),
                        route.points[point_index - 1].s,
                        center_u, center_v, *coordinates),
                    local_point_to_world(
                        Point::new_scale(route.points[point_index].u,
                                         route.points[point_index].v),
                        route.points[point_index].s,
                        center_u, center_v, *coordinates),
                    "reachable_2d_layer_search_route");
            }
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "route_2d_search",
                found_root ? "connected_to_world_build_plate"
                           : "connected_to_grounded_parent",
                {{"contact_index", static_cast<double>(route.contact_index)},
                 {"accepted", 1.0},
                 {"parent_contact_index",
                  found_root ? -1.0 :
                      static_cast<double>(routes[found_parent_route].contact_index)},
                 {"dense_route_point_count",
                  static_cast<double>(dense_points.size())},
                 {"route_point_count", static_cast<double>(route.points.size())},
                 {"search_level_count", static_cast<double>(search_level)},
                 {"stored_node_count", static_cast<double>(nodes.size())}});
        }
    }

    // The bounded beam above is diagnostic: it may discard the only valid
    // homotopy class. Resolve the remaining contacts with exact 2D reachable
    // areas, matching the area-propagation contract used by the regular tree
    // support implementation. Each layer expands by the branch angle budget,
    // subtracts the center-line obstacle, and clips to the world build-plate
    // halfspace. The full per-layer statistics and sampled outlines remain in
    // the audit recorder.
    struct BeltReachableAreaLayer
    {
        size_t plan_index{0};
        double slice_s{0.0};
        ExPolygons regions;
    };
    constexpr size_t exact_area_debug_stride = 16;
    const double exact_area_simplification_tolerance = 0.02;
    size_t exact_area_attempted_contact_count = 0;
    size_t exact_area_rescued_contact_count = 0;
    size_t exact_area_rooted_contact_count = 0;
    size_t exact_area_parent_attached_contact_count = 0;
    size_t exact_area_failed_contact_count = 0;
    size_t exact_area_processed_layer_count = 0;
    size_t exact_area_maximum_component_count = 0;
    size_t exact_area_maximum_vertex_count = 0;
    // Every reachable-area point is stored on Clipper's integer coordinate
    // grid. Reserve enough physical distance for the independent U/V rounding
    // performed while offsetting, intersecting and projecting a predecessor.
    // The final route is still checked against the configured angle; this is
    // a construction margin, not an acceptance tolerance.
    const double exact_route_coordinate_guard = 4.0 * SCALING_FACTOR;
    std::vector<size_t> exact_area_attempted_route_indices;
    for (std::ptrdiff_t route_cursor =
             static_cast<std::ptrdiff_t>(routes.size()) - 1;
         route_cursor >= 0; --route_cursor) {
        const size_t route_index = static_cast<size_t>(route_cursor);
        BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;
        exact_area_attempted_route_indices.push_back(route_index);
        ++exact_area_attempted_contact_count;
        const BeltSupportContact& contact = contacts[route.contact_index];
        const Point tip_point = contact.center_local;
        // Clipper needs an area rather than a mathematical point. A one-unit
        // polygon is removed by Clipper's offset cleanup, so use a 1 micron
        // half-width. Backtracking starts from the exact contact center while
        // it remains inside the propagated area, therefore this seed does not
        // spend the branch-angle budget.
        ExPolygons reachable{ExPolygon(rectangle_at(
            tip_point, 0.001, 0.001))};
        double previous_s = contact.tip_s;
        std::vector<BeltReachableAreaLayer> area_layers;
        area_layers.reserve(plans.size());
        bool found_goal = false;
        bool found_root = false;
        size_t found_parent = BeltSupportRoute::no_parent;
        Point goal_point;
        size_t consecutive_parent_layers = 0;
        size_t previous_parent = BeltSupportRoute::no_parent;

        const auto below_tip_it = std::lower_bound(
            plans.begin(), plans.end(), contact.tip_s,
            [](const BeltSupportLayerPlan& plan, double slice_s) {
                return plan.slice_s < slice_s;
            });
        std::ptrdiff_t plan_index = static_cast<std::ptrdiff_t>(
            std::distance(plans.begin(), below_tip_it)) - 1;
        for (; plan_index >= 0 && !found_goal; --plan_index) {
            detector.throw_on_cancel();
            const BeltSupportLayerPlan& plan =
                plans[static_cast<size_t>(plan_index)];
            const double delta_s = previous_s - plan.slice_s;
            if (delta_s <= EPSILON)
                continue;
            const double nominal_move_budget =
                maximum_lateral_slope * delta_s;
            const double move_budget = std::max(
                0.0, nominal_move_budget - exact_route_coordinate_guard);
            reachable = offset_ex(
                reachable, scale_(move_budget), jtRound);

            // The finite Clipper seed must not enlarge the mathematical cone.
            // Bound every propagated set by the cumulative Euclidean lateral
            // budget measured from the exact contact center.
            const double nominal_total_lateral_budget =
                maximum_lateral_slope *
                std::max(0.0, contact.tip_s - plan.slice_s);
            const double total_lateral_budget = std::max(
                0.0,
                nominal_total_lateral_budget - exact_route_coordinate_guard);
            if (total_lateral_budget > EPSILON && !reachable.empty()) {
                Polygon total_budget_circle = make_circle(
                    scale_(total_lateral_budget),
                    scale_(std::min(
                        0.005, 0.1 * total_lateral_budget)));
                total_budget_circle.translate(tip_point);
                reachable = intersection_ex(
                    reachable,
                    ExPolygons{ExPolygon(std::move(total_budget_circle))});
            }

            const double distance_to_tip = contact.tip_s - plan.slice_s;
            if (distance_to_tip > contact_cap_depth + EPSILON) {
                const ExPolygons& center_obstacles =
                    forbidden_by_plan[static_cast<size_t>(plan_index)].regions;
                if (!center_obstacles.empty()) {
                    reachable = diff_ex(
                        reachable,
                        offset_ex(center_obstacles,
                                  scale_(tip_half_width), jtRound));
                }
            }
            reachable = clip_regions_to_world_build_halfspace(
                reachable, plan.slice_s, center_v, *coordinates);
            // This is the authoritative reachability set. Simplifying it by
            // 0.02 mm used to move its boundary farther than one layer's
            // branch-angle budget, which made a geometrically valid forward
            // propagation impossible to backtrack without violating the
            // configured angle. Preserve the exact union for routing; the
            // tolerance remains available only as display metadata.
            if (!reachable.empty())
                reachable = union_ex(reachable);

            size_t vertex_count = 0;
            for (const ExPolygon& region : reachable) {
                vertex_count += region.contour.points.size();
                for (const Polygon& hole : region.holes)
                    vertex_count += hole.points.size();
            }
            ++exact_area_processed_layer_count;
            exact_area_maximum_component_count = std::max(
                exact_area_maximum_component_count, reachable.size());
            exact_area_maximum_vertex_count = std::max(
                exact_area_maximum_vertex_count, vertex_count);
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::ReachableCorridors,
                    "exact_reachable_area_layer",
                    reachable.empty() ? "area_exhausted"
                                      : "area_propagated",
                    {{"contact_index", static_cast<double>(route.contact_index)},
                     {"plan_index", static_cast<double>(plan_index)},
                     {"slice_s_mm", plan.slice_s},
                     {"nominal_move_budget_mm", nominal_move_budget},
                     {"move_budget_mm", move_budget},
                     {"coordinate_guard_mm", exact_route_coordinate_guard},
                     {"nominal_total_lateral_budget_mm",
                      nominal_total_lateral_budget},
                     {"total_lateral_budget_mm", total_lateral_budget},
                     {"component_count", static_cast<double>(reachable.size())},
                     {"vertex_count", static_cast<double>(vertex_count)},
                     {"area_mm2", expolygons_area_mm2(reachable)}});
                if (!reachable.empty() &&
                    area_layers.size() % exact_area_debug_stride == 0) {
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::ReachableCorridors,
                        reachable, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "exact_reachable_area_frontier");
                }
            }
            if (reachable.empty())
                break;

            area_layers.push_back({
                static_cast<size_t>(plan_index), plan.slice_s, reachable});

            size_t layer_parent = BeltSupportRoute::no_parent;
            Point layer_parent_goal;
            double best_parent_distance = std::numeric_limits<double>::max();
            if (distance_to_tip > contact_cap_depth + EPSILON) {
                for (size_t parent_index = 0;
                     parent_index < routes.size(); ++parent_index) {
                    if (parent_index == route_index)
                        continue;
                    const BeltSupportRoute& parent = routes[parent_index];
                    if (!parent.failure_reason.empty() ||
                        parent.points.size() < 2 ||
                        plan.slice_s > parent.points.front().s + EPSILON ||
                        plan.slice_s < parent.points.back().s - EPSILON ||
                        plan.slice_s > parent.tip_s - contact_cap_depth + EPSILON) {
                        continue;
                    }
                    const double parent_u = route_u_at(parent, plan.slice_s);
                    const double parent_v = route_v_at(parent, plan.slice_s);
                    const double child_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + distance_to_tip * taper);
                    const double parent_half_width = std::min(
                        maximum_half_width,
                        tip_half_width +
                            std::max(0.0, parent.tip_s - plan.slice_s) * taper);
                    const double merge_distance =
                        child_half_width + parent_half_width;
                    const ExPolygons overlap = intersection_ex(
                        reachable,
                        ExPolygons{ExPolygon(rectangle_at(
                            Point::new_scale(parent_u, parent_v),
                            merge_distance, merge_distance))});
                    if (overlap.empty())
                        continue;
                    const Point candidate = safe_centroid(overlap.front());
                    const double distance = unscaled<double>(
                        (candidate - tip_point).norm());
                    if (distance < best_parent_distance) {
                        best_parent_distance = distance;
                        layer_parent = parent_index;
                        layer_parent_goal = candidate;
                    }
                }
            }
            if (layer_parent != BeltSupportRoute::no_parent) {
                if (layer_parent == previous_parent)
                    ++consecutive_parent_layers;
                else
                    consecutive_parent_layers = 1;
                previous_parent = layer_parent;
                if (consecutive_parent_layers >= merge_stability_layers) {
                    found_goal = true;
                    found_parent = layer_parent;
                    goal_point = layer_parent_goal;
                }
            } else {
                consecutive_parent_layers = 0;
                previous_parent = BeltSupportRoute::no_parent;
            }

            if (!found_goal) {
                const double boundary_v =
                    coordinates->belt_boundary_v(plan.slice_s) - center_v;
                for (const ExPolygon& region : reachable) {
                    const BoundingBox bounds = get_extents(region);
                    if (!bounds.defined ||
                        unscaled<double>(bounds.min.y()) >
                            boundary_v + 0.02 + EPSILON) {
                        continue;
                    }
                    const coord_t root_minimum_u =
                        bounds.min.x() - scaled<coord_t>(0.1);
                    const coord_t root_maximum_u =
                        bounds.max.x() + scaled<coord_t>(0.1);
                    const coord_t root_minimum_v =
                        scaled<coord_t>(boundary_v);
                    const coord_t root_maximum_v =
                        scaled<coord_t>(boundary_v + 0.02);
                    Polygon root_band({
                        Point(root_minimum_u, root_minimum_v),
                        Point(root_maximum_u, root_minimum_v),
                        Point(root_maximum_u, root_maximum_v),
                        Point(root_minimum_u, root_maximum_v),
                    });
                    const ExPolygons root_overlap = intersection_ex(
                        ExPolygons{region},
                        ExPolygons{ExPolygon(std::move(root_band))});
                    if (root_overlap.empty())
                        continue;
                    const Point inside = safe_centroid(root_overlap.front());
                    goal_point = Point(
                        inside.x(), scaled<coord_t>(boundary_v));
                    found_goal = true;
                    found_root = true;
                    break;
                }
            }
            previous_s = plan.slice_s;
        }

        bool backtrack_failed = !found_goal || area_layers.empty();
        std::vector<BeltRoutePoint> reverse_points;
        if (!backtrack_failed) {
            Point current_point = goal_point;
            double current_s = area_layers.back().slice_s;
            reverse_points.push_back({
                unscaled<double>(current_point.x()),
                unscaled<double>(current_point.y()), current_s});
            for (std::ptrdiff_t layer_index =
                     static_cast<std::ptrdiff_t>(area_layers.size()) - 2;
                 layer_index >= 0; --layer_index) {
                const BeltReachableAreaLayer& upper =
                    area_layers[static_cast<size_t>(layer_index)];
                const double permitted = maximum_lateral_slope *
                    (upper.slice_s - current_s);
                Point chosen = current_point;
                const bool current_inside = std::any_of(
                    upper.regions.begin(), upper.regions.end(),
                    [&current_point](const ExPolygon& region) {
                        return region.contains(current_point);
                    });
                if (!current_inside)
                    chosen = projection_onto(upper.regions, current_point);
                const double actual_step =
                    unscaled<double>((chosen - current_point).norm());
                constexpr double backtrack_coordinate_tolerance = 0.00001;
                if (actual_step > permitted + backtrack_coordinate_tolerance) {
                    backtrack_failed = true;
                    if (debug != nullptr) {
                        debug->add_record(
                            BeltSupportDebugStageId::RouteClassification,
                            "exact_reachable_area_backtrack_step",
                            "nearest_predecessor_exceeds_branch_angle_budget",
                            {{"contact_index",
                              static_cast<double>(route.contact_index)},
                             {"slice_s_mm", upper.slice_s},
                             {"permitted_lateral_step_mm", permitted},
                             {"actual_lateral_step_mm", actual_step},
                             {"coordinate_tolerance_mm",
                              backtrack_coordinate_tolerance}});
                    }
                    break;
                }
                current_point = chosen;
                current_s = upper.slice_s;
                reverse_points.push_back({
                    unscaled<double>(chosen.x()),
                    unscaled<double>(chosen.y()), current_s});
            }
        }

        if (backtrack_failed) {
            ++exact_area_failed_contact_count;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "exact_reachable_area_route",
                    found_goal ? "backtracking_failed"
                               : "no_build_plate_or_parent_connection",
                    {{"contact_index", static_cast<double>(route.contact_index)},
                     {"stored_layer_count",
                      static_cast<double>(area_layers.size())}});
            }
            continue;
        }

        std::vector<BeltRoutePoint> dense_points;
        dense_points.reserve(reverse_points.size() + 1);
        dense_points.push_back({
            unscaled<double>(tip_point.x()),
            unscaled<double>(tip_point.y()), contact.tip_s});
        dense_points.insert(dense_points.end(),
                            reverse_points.rbegin(), reverse_points.rend());
        double dense_route_maximum_lateral_slope = 0.0;
        bool dense_route_angle_valid = true;
        constexpr double route_slope_tolerance = 1e-5;
        for (size_t point_index = 1;
             point_index < dense_points.size(); ++point_index) {
            const BeltRoutePoint& upper = dense_points[point_index - 1];
            const BeltRoutePoint& lower = dense_points[point_index];
            const double delta_s = upper.s - lower.s;
            if (delta_s <= EPSILON) {
                dense_route_angle_valid = false;
                break;
            }
            const double delta_u = upper.u - lower.u;
            const double delta_v = upper.v - lower.v;
            const double lateral_slope =
                std::sqrt(delta_u * delta_u + delta_v * delta_v) / delta_s;
            dense_route_maximum_lateral_slope = std::max(
                dense_route_maximum_lateral_slope, lateral_slope);
            if (lateral_slope >
                maximum_lateral_slope + route_slope_tolerance) {
                dense_route_angle_valid = false;
                break;
            }
        }
        if (!dense_route_angle_valid) {
            ++exact_area_failed_contact_count;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "exact_reachable_area_route",
                    "route_exceeds_combined_lateral_angle",
                    {{"contact_index", static_cast<double>(route.contact_index)},
                     {"maximum_lateral_slope",
                      dense_route_maximum_lateral_slope},
                     {"configured_maximum_lateral_slope",
                      maximum_lateral_slope},
                     {"slope_tolerance", route_slope_tolerance}});
            }
            continue;
        }
        route.points = std::move(dense_points);
        route.reachable_layers.clear();
        route.root_s = route.points.back().s;
        route.parent_route_index = found_root
            ? BeltSupportRoute::no_parent : found_parent;
        route.merge_s = found_root ? 0.0 : route.points.back().s;
        route.failure_reason.clear();
        ++exact_area_rescued_contact_count;
        ++reachable_contact_count;
        --unreachable_contact_count;
        if (found_root)
            ++exact_area_rooted_contact_count;
        else
            ++exact_area_parent_attached_contact_count;
        if (debug != nullptr) {
            for (size_t point_index = 1;
                 point_index < route.points.size(); ++point_index) {
                debug->add_line(
                    BeltSupportDebugStageId::RouteClassification,
                    local_point_to_world(
                        Point::new_scale(route.points[point_index - 1].u,
                                         route.points[point_index - 1].v),
                        route.points[point_index - 1].s,
                        center_u, center_v, *coordinates),
                    local_point_to_world(
                        Point::new_scale(route.points[point_index].u,
                                         route.points[point_index].v),
                        route.points[point_index].s,
                        center_u, center_v, *coordinates),
                    "exact_reachable_area_route");
            }
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "exact_reachable_area_route",
                found_root ? "connected_to_world_build_plate"
                           : "connected_to_grounded_parent",
                {{"contact_index", static_cast<double>(route.contact_index)},
                 {"parent_contact_index",
                  found_root ? -1.0 :
                      static_cast<double>(routes[found_parent].contact_index)},
                 {"dense_route_point_count",
                  static_cast<double>(route.points.size())},
                 {"maximum_lateral_slope",
                  dense_route_maximum_lateral_slope},
                 {"stored_layer_count",
                  static_cast<double>(area_layers.size())}});
        }
    }

    // Consolidation is geometric and intentionally precedes routing, but its
    // tie-break may select an unreachable witness while another witness in the
    // same surface component represents the exact same assigned area. Test
    // those coverage-preserving alternatives against every already grounded
    // parent route. A replacement is accepted only after angle, build-plate,
    // collision and multi-layer merge checks all pass.
    size_t route_aware_uv_alternative_contact_count = 0;
    size_t route_aware_uv_preserving_candidate_count = 0;
    size_t route_aware_uv_angle_candidate_count = 0;
    size_t route_aware_uv_collision_rejected_count = 0;
    size_t route_aware_uv_stability_rejected_count = 0;
    size_t route_aware_uv_rescued_contact_count = 0;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;
        ++route_aware_uv_alternative_contact_count;
        const BeltSupportContact original_contact =
            contacts[route.contact_index];
        const auto component_it = raw_contacts_by_surface_component.find(
            original_contact.source_surface_component_index);
        if (component_it == raw_contacts_by_surface_component.end())
            continue;

        bool rescued = false;
        for (const size_t raw_index : component_it->second) {
            const BeltSupportContact& candidate = raw_contacts[raw_index];
            bool preserves_assignment = true;
            double maximum_mapping_distance = 0.0;
            const Vec3d candidate_world = local_point_to_world(
                candidate.center_local, candidate.tip_s,
                center_u, center_v, *coordinates);
            for (const size_t witness_index :
                 original_contact.source_witness_indices) {
                if (witness_index >= raw_witness_world.size()) {
                    preserves_assignment = false;
                    break;
                }
                maximum_mapping_distance = std::max(
                    maximum_mapping_distance,
                    (raw_witness_world[witness_index] - candidate_world).norm());
                if (!contact_covers_witness(
                        candidate, raw_contacts[witness_index])) {
                    preserves_assignment = false;
                    break;
                }
            }
            if (!preserves_assignment)
                continue;
            ++route_aware_uv_preserving_candidate_count;

            const double tip_u =
                unscaled<double>(candidate.center_local.x());
            const double tip_v =
                unscaled<double>(candidate.center_local.y());
            for (size_t parent_index = 0;
                 parent_index < routes.size() && !rescued; ++parent_index) {
                if (parent_index == route_index)
                    continue;
                const BeltSupportRoute& parent = routes[parent_index];
                if (!parent.failure_reason.empty() ||
                    parent.points.size() < 2) {
                    continue;
                }
                const double highest_merge_s = std::min({
                    candidate.tip_s - contact_cap_depth,
                    parent.tip_s - contact_cap_depth,
                    parent.points.front().s,
                });
                const double lowest_merge_s = parent.points.back().s;
                if (highest_merge_s <= lowest_merge_s + EPSILON)
                    continue;

                for (std::ptrdiff_t plan_cursor =
                         static_cast<std::ptrdiff_t>(plans.size()) - 1;
                     plan_cursor >= 0 && !rescued; --plan_cursor) {
                    const size_t merge_plan_index =
                        static_cast<size_t>(plan_cursor);
                    const double merge_s = plans[merge_plan_index].slice_s;
                    if (merge_s > highest_merge_s + EPSILON)
                        continue;
                    if (merge_s < lowest_merge_s - EPSILON)
                        break;
                    const double target_u = route_u_at(parent, merge_s);
                    const double target_v = route_v_at(parent, merge_s);
                    const double branch_span = candidate.tip_s - merge_s;
                    const double lateral_distance = std::hypot(
                        target_u - tip_u, target_v - tip_v);
                    if (lateral_distance >
                        maximum_lateral_slope * branch_span + EPSILON) {
                        continue;
                    }
                    ++route_aware_uv_angle_candidate_count;
                    auto child_center_at = [&](double slice_s) {
                        const double ratio = std::clamp(
                            (candidate.tip_s - slice_s) / branch_span,
                            0.0, 1.0);
                        return std::pair<double, double>{
                            tip_u + (target_u - tip_u) * ratio,
                            tip_v + (target_v - tip_v) * ratio};
                    };

                    size_t stable_overlap_layers = 0;
                    for (size_t offset = 0;
                         offset < merge_stability_layers; ++offset) {
                        const size_t overlap_plan_index =
                            merge_plan_index + offset;
                        if (overlap_plan_index >= plans.size())
                            break;
                        const double slice_s =
                            plans[overlap_plan_index].slice_s;
                        if (slice_s >= candidate.tip_s - contact_cap_depth +
                                          EPSILON ||
                            slice_s > parent.points.front().s + EPSILON ||
                            slice_s < parent.points.back().s - EPSILON) {
                            break;
                        }
                        const auto [child_u, child_v] =
                            child_center_at(slice_s);
                        const double parent_u = route_u_at(parent, slice_s);
                        const double parent_v = route_v_at(parent, slice_s);
                        const double child_half_width = std::min(
                            maximum_half_width,
                            tip_half_width +
                                (candidate.tip_s - slice_s) * taper);
                        const double parent_half_width = std::min(
                            maximum_half_width,
                            tip_half_width +
                                (parent.tip_s - slice_s) * taper);
                        if (std::hypot(child_u - parent_u,
                                       child_v - parent_v) >
                            child_half_width + parent_half_width + EPSILON) {
                            break;
                        }
                        ++stable_overlap_layers;
                    }
                    if (stable_overlap_layers < merge_stability_layers) {
                        ++route_aware_uv_stability_rejected_count;
                        continue;
                    }

                    bool collision = false;
                    size_t validated_layer_count = 0;
                    for (size_t scan_index = merge_plan_index;
                         scan_index < plans.size(); ++scan_index) {
                        const BeltSupportLayerPlan& scan_plan =
                            plans[scan_index];
                        if (scan_plan.slice_s >= candidate.tip_s - EPSILON)
                            break;
                        ++validated_layer_count;
                        const auto [branch_u, branch_v] =
                            child_center_at(scan_plan.slice_s);
                        const double minimum_printable_v =
                            coordinates->belt_boundary_v(scan_plan.slice_s) -
                            center_v;
                        if (branch_v < minimum_printable_v - EPSILON) {
                            collision = true;
                            break;
                        }
                        if (candidate.tip_s - scan_plan.slice_s <=
                            contact_cap_depth + EPSILON) {
                            continue;
                        }
                        if (section_collides(
                                forbidden_by_plan[scan_index],
                                branch_u, branch_v, tip_half_width)) {
                            collision = true;
                            break;
                        }
                    }
                    if (collision) {
                        ++route_aware_uv_collision_rejected_count;
                        continue;
                    }

                    BeltSupportContact replacement = candidate;
                    replacement.source_witness_indices =
                        original_contact.source_witness_indices;
                    replacement.source_surface_component_index =
                        original_contact.source_surface_component_index;
                    contacts[route.contact_index] = replacement;
                    route.points = {
                        {tip_u, tip_v, candidate.tip_s},
                        {target_u, target_v, merge_s},
                    };
                    route.tip_s = candidate.tip_s;
                    route.root_s = merge_s;
                    route.parent_route_index = parent_index;
                    route.merge_s = merge_s;
                    route.failure_reason.clear();
                    ++route_aware_uv_rescued_contact_count;
                    ++reachable_contact_count;
                    --unreachable_contact_count;
                    rescued = true;
                    if (debug != nullptr) {
                        debug->add_line(
                            BeltSupportDebugStageId::RouteClassification,
                            candidate_world,
                            local_point_to_world(
                                Point::new_scale(target_u, target_v), merge_s,
                                center_u, center_v, *coordinates),
                            "route_aware_uv_alternative");
                        debug->add_record(
                            BeltSupportDebugStageId::RouteClassification,
                            "route_aware_uv_alternative",
                            "coverage_preserving_candidate_connected_to_parent",
                            {{"contact_index",
                              static_cast<double>(route.contact_index)},
                             {"raw_contact_index",
                              static_cast<double>(raw_index)},
                             {"parent_contact_index",
                              static_cast<double>(parent.contact_index)},
                             {"tip_s_mm", candidate.tip_s},
                             {"merge_s_mm", merge_s},
                             {"maximum_mapping_distance_mm",
                              maximum_mapping_distance},
                             {"lateral_distance_mm", lateral_distance},
                             {"branch_angle_deg",
                              std::atan2(lateral_distance, branch_span) *
                                  180.0 / M_PI},
                             {"validated_layer_count",
                              static_cast<double>(validated_layer_count)},
                             {"stable_overlap_layer_count",
                              static_cast<double>(stable_overlap_layers)}});
                    }
                }
            }
        }
        if (!rescued && debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "route_aware_uv_alternative",
                "no_coverage_preserving_candidate_connected_to_parent",
                {{"contact_index", static_cast<double>(route.contact_index)},
                 {"surface_component_index", static_cast<double>(
                      original_contact.source_surface_component_index)}});
        }
    }

    struct ExactAlternativeRouteResult
    {
        bool accepted{false};
        bool rooted{false};
        size_t parent_index{BeltSupportRoute::no_parent};
        double merge_s{0.0};
        double failure_s{0.0};
        double maximum_lateral_slope{0.0};
        size_t processed_layer_count{0};
        std::vector<BeltRoutePoint> points;
    };
    auto try_exact_alternative_route = [&] (
        const BeltSupportContact& candidate,
        size_t excluded_route_index,
        size_t raw_index,
        double lateral_slope_limit,
        const std::string& layer_audit_kind,
        bool render_audit_geometry) {
        ExactAlternativeRouteResult result;
        const Point tip_point = candidate.center_local;
        ExPolygons reachable{ExPolygon(rectangle_at(
            tip_point, 0.001, 0.001))};
        double previous_s = candidate.tip_s;
        std::vector<BeltReachableAreaLayer> area_layers;
        bool found_goal = false;
        Point goal_point;
        size_t consecutive_parent_layers = 0;
        size_t previous_parent = BeltSupportRoute::no_parent;

        const auto below_tip_it = std::lower_bound(
            plans.begin(), plans.end(), candidate.tip_s,
            [](const BeltSupportLayerPlan& plan, double slice_s) {
                return plan.slice_s < slice_s;
            });
        std::ptrdiff_t plan_cursor = static_cast<std::ptrdiff_t>(
            std::distance(plans.begin(), below_tip_it)) - 1;
        for (; plan_cursor >= 0 && !found_goal; --plan_cursor) {
            detector.throw_on_cancel();
            const size_t plan_index = static_cast<size_t>(plan_cursor);
            const BeltSupportLayerPlan& plan = plans[plan_index];
            const double delta_s = previous_s - plan.slice_s;
            if (delta_s <= EPSILON)
                continue;
            const double nominal_move_budget =
                lateral_slope_limit * delta_s;
            const double move_budget = std::max(
                0.0,
                nominal_move_budget - exact_route_coordinate_guard);
            reachable = offset_ex(
                reachable, scale_(move_budget), jtRound);

            const double distance_to_tip =
                candidate.tip_s - plan.slice_s;
            const double nominal_total_budget =
                lateral_slope_limit * std::max(0.0, distance_to_tip);
            const double total_budget = std::max(
                0.0,
                nominal_total_budget - exact_route_coordinate_guard);
            if (total_budget > EPSILON && !reachable.empty()) {
                Polygon total_budget_circle = make_circle(
                    scale_(total_budget),
                    scale_(std::min(0.005, 0.1 * total_budget)));
                total_budget_circle.translate(tip_point);
                reachable = intersection_ex(
                    reachable,
                    ExPolygons{ExPolygon(std::move(total_budget_circle))});
            }
            if (!reachable.empty())
                reachable = union_ex(reachable);
            const double angle_limited_area_mm2 =
                expolygons_area_mm2(reachable);
            const ExPolygons angle_limited_regions =
                debug != nullptr ? reachable : ExPolygons{};

            const bool model_constraint_applied =
                distance_to_tip > contact_cap_depth + EPSILON &&
                !forbidden_by_plan[plan_index].regions.empty();
            double forbidden_overlap_area_mm2 = 0.0;
            ExPolygons forbidden_overlap_regions;
            if (model_constraint_applied && !reachable.empty()) {
                const ExPolygons forbidden_clearance = offset_ex(
                    forbidden_by_plan[plan_index].regions,
                    scale_(tip_half_width), jtRound);
                if (debug != nullptr) {
                    forbidden_overlap_regions = intersection_ex(
                        reachable, forbidden_clearance);
                    forbidden_overlap_area_mm2 =
                        expolygons_area_mm2(forbidden_overlap_regions);
                }
                reachable = diff_ex(reachable, forbidden_clearance);
            }
            const double after_model_area_mm2 =
                expolygons_area_mm2(reachable);
            const ExPolygons before_build_halfspace_regions =
                debug != nullptr ? reachable : ExPolygons{};
            reachable = clip_regions_to_world_build_halfspace(
                reachable, plan.slice_s, center_v, *coordinates);
            if (!reachable.empty())
                reachable = union_ex(reachable);
            const double after_build_halfspace_area_mm2 =
                expolygons_area_mm2(reachable);
            std::string propagation_reason = "area_propagated";
            if (reachable.empty()) {
                if (angle_limited_area_mm2 <= 1e-12)
                    propagation_reason = "angle_cone_exhausted";
                else if (after_model_area_mm2 <= 1e-12)
                    propagation_reason = "model_clearance_exhausted";
                else
                    propagation_reason = "world_build_halfspace_exhausted";
            }
            ++result.processed_layer_count;
            result.failure_s = plan.slice_s;
            if (debug != nullptr) {
                size_t vertex_count = 0;
                for (const ExPolygon& region : reachable) {
                    vertex_count += region.contour.points.size();
                    for (const Polygon& hole : region.holes)
                        vertex_count += hole.points.size();
                }
                debug->add_record(
                    BeltSupportDebugStageId::ReachableCorridors,
                    layer_audit_kind,
                    propagation_reason,
                    {{"raw_contact_index", static_cast<double>(raw_index)},
                     {"plan_index", static_cast<double>(plan_index)},
                     {"slice_s_mm", plan.slice_s},
                     {"move_budget_mm", move_budget},
                     {"total_lateral_budget_mm", total_budget},
                     {"lateral_slope_limit", lateral_slope_limit},
                     {"model_constraint_applied",
                      model_constraint_applied ? 1.0 : 0.0},
                     {"angle_limited_area_mm2", angle_limited_area_mm2},
                     {"forbidden_overlap_area_mm2",
                      forbidden_overlap_area_mm2},
                     {"after_model_area_mm2", after_model_area_mm2},
                     {"after_build_halfspace_area_mm2",
                      after_build_halfspace_area_mm2},
                     {"component_count",
                      static_cast<double>(reachable.size())},
                     {"vertex_count", static_cast<double>(vertex_count)},
                     {"area_mm2", expolygons_area_mm2(reachable)}});
                if (render_audit_geometry && reachable.empty()) {
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::ReachableCorridors,
                        angle_limited_regions, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "route_aware_exact_exhaustion_input");
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::ReachableCorridors,
                        forbidden_overlap_regions, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "route_aware_exact_exhaustion_model_overlap");
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::ReachableCorridors,
                        before_build_halfspace_regions, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "route_aware_exact_exhaustion_after_model");
                }
                if (render_audit_geometry && !reachable.empty() &&
                    area_layers.size() % exact_area_debug_stride == 0) {
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::ReachableCorridors,
                        reachable, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "route_aware_exact_alternative_frontier");
                }
            }
            if (reachable.empty())
                break;
            area_layers.push_back({plan_index, plan.slice_s, reachable});

            size_t layer_parent = BeltSupportRoute::no_parent;
            Point layer_parent_goal;
            double best_parent_distance =
                std::numeric_limits<double>::max();
            if (distance_to_tip > contact_cap_depth + EPSILON) {
                for (size_t parent_index = 0;
                     parent_index < routes.size(); ++parent_index) {
                    if (parent_index == excluded_route_index)
                        continue;
                    const BeltSupportRoute& parent = routes[parent_index];
                    if (!parent.failure_reason.empty() ||
                        parent.points.size() < 2 ||
                        plan.slice_s > parent.points.front().s + EPSILON ||
                        plan.slice_s < parent.points.back().s - EPSILON ||
                        plan.slice_s > parent.tip_s - contact_cap_depth +
                                           EPSILON) {
                        continue;
                    }
                    const double parent_u =
                        route_u_at(parent, plan.slice_s);
                    const double parent_v =
                        route_v_at(parent, plan.slice_s);
                    const double child_half_width = std::min(
                        maximum_half_width,
                        tip_half_width + distance_to_tip * taper);
                    const double parent_half_width = std::min(
                        maximum_half_width,
                        tip_half_width +
                            std::max(0.0,
                                     parent.tip_s - plan.slice_s) * taper);
                    const double merge_distance =
                        child_half_width + parent_half_width;
                    const ExPolygons overlap = intersection_ex(
                        reachable,
                        ExPolygons{ExPolygon(rectangle_at(
                            Point::new_scale(parent_u, parent_v),
                            merge_distance, merge_distance))});
                    if (overlap.empty())
                        continue;
                    const Point point = safe_centroid(overlap.front());
                    const double distance = unscaled<double>(
                        (point - tip_point).norm());
                    if (distance < best_parent_distance) {
                        best_parent_distance = distance;
                        layer_parent = parent_index;
                        layer_parent_goal = point;
                    }
                }
            }
            if (layer_parent != BeltSupportRoute::no_parent) {
                if (layer_parent == previous_parent)
                    ++consecutive_parent_layers;
                else
                    consecutive_parent_layers = 1;
                previous_parent = layer_parent;
                if (consecutive_parent_layers >= merge_stability_layers) {
                    found_goal = true;
                    result.parent_index = layer_parent;
                    result.merge_s = plan.slice_s;
                    goal_point = layer_parent_goal;
                }
            } else {
                consecutive_parent_layers = 0;
                previous_parent = BeltSupportRoute::no_parent;
            }

            if (!found_goal) {
                const double boundary_v =
                    coordinates->belt_boundary_v(plan.slice_s) - center_v;
                for (const ExPolygon& region : reachable) {
                    const BoundingBox bounds = get_extents(region);
                    if (!bounds.defined ||
                        unscaled<double>(bounds.min.y()) >
                            boundary_v + 0.02 + EPSILON) {
                        continue;
                    }
                    const Polygon root_band({
                        Point(bounds.min.x() - scaled<coord_t>(0.1),
                              scaled<coord_t>(boundary_v)),
                        Point(bounds.max.x() + scaled<coord_t>(0.1),
                              scaled<coord_t>(boundary_v)),
                        Point(bounds.max.x() + scaled<coord_t>(0.1),
                              scaled<coord_t>(boundary_v + 0.02)),
                        Point(bounds.min.x() - scaled<coord_t>(0.1),
                              scaled<coord_t>(boundary_v + 0.02)),
                    });
                    const ExPolygons root_overlap = intersection_ex(
                        ExPolygons{region},
                        ExPolygons{ExPolygon(root_band)});
                    if (root_overlap.empty())
                        continue;
                    const Point inside = safe_centroid(root_overlap.front());
                    goal_point = Point(
                        inside.x(), scaled<coord_t>(boundary_v));
                    found_goal = true;
                    result.rooted = true;
                    result.merge_s = plan.slice_s;
                    break;
                }
            }
            previous_s = plan.slice_s;
        }

        if (!found_goal || area_layers.empty())
            return result;
        Point current_point = goal_point;
        double current_s = area_layers.back().slice_s;
        std::vector<BeltRoutePoint> reverse_points{{
            unscaled<double>(current_point.x()),
            unscaled<double>(current_point.y()), current_s}};
        for (std::ptrdiff_t layer_cursor =
                 static_cast<std::ptrdiff_t>(area_layers.size()) - 2;
             layer_cursor >= 0; --layer_cursor) {
            const BeltReachableAreaLayer& upper =
                area_layers[static_cast<size_t>(layer_cursor)];
            const double permitted =
                lateral_slope_limit * (upper.slice_s - current_s);
            Point chosen = current_point;
            const bool inside = std::any_of(
                upper.regions.begin(), upper.regions.end(),
                [&current_point](const ExPolygon& region) {
                    return region.contains(current_point);
                });
            if (!inside)
                chosen = projection_onto(upper.regions, current_point);
            if (unscaled<double>((chosen - current_point).norm()) >
                permitted + 0.00001) {
                return result;
            }
            current_point = chosen;
            current_s = upper.slice_s;
            reverse_points.push_back({
                unscaled<double>(chosen.x()),
                unscaled<double>(chosen.y()), current_s});
        }
        result.points.push_back({
            unscaled<double>(tip_point.x()),
            unscaled<double>(tip_point.y()), candidate.tip_s});
        result.points.insert(
            result.points.end(),
            reverse_points.rbegin(), reverse_points.rend());
        for (size_t point_index = 1;
             point_index < result.points.size(); ++point_index) {
            const BeltRoutePoint& upper = result.points[point_index - 1];
            const BeltRoutePoint& lower = result.points[point_index];
            const double delta_s = upper.s - lower.s;
            if (delta_s <= EPSILON) {
                result.points.clear();
                return result;
            }
            result.maximum_lateral_slope = std::max(
                result.maximum_lateral_slope,
                std::hypot(upper.u - lower.u, upper.v - lower.v) /
                    delta_s);
        }
        result.accepted =
            result.maximum_lateral_slope <=
                lateral_slope_limit + 1e-5;
        if (!result.accepted)
            result.points.clear();
        return result;
    };

    size_t route_aware_exact_alternative_candidate_count = 0;
    size_t route_aware_exact_alternative_rescued_contact_count = 0;
    size_t route_aware_exact_alternative_parent_attached_contact_count = 0;
    size_t route_aware_exact_alternative_rooted_contact_count = 0;
    size_t route_aware_exact_alternative_exhausted_count = 0;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;
        const BeltSupportContact original_contact =
            contacts[route.contact_index];
        const auto component_it = raw_contacts_by_surface_component.find(
            original_contact.source_surface_component_index);
        if (component_it == raw_contacts_by_surface_component.end())
            continue;
        bool rescued = false;
        for (const size_t raw_index : component_it->second) {
            const BeltSupportContact& candidate = raw_contacts[raw_index];
            const Vec3d candidate_world = local_point_to_world(
                candidate.center_local, candidate.tip_s,
                center_u, center_v, *coordinates);
            bool preserves_assignment = true;
            double maximum_mapping_distance = 0.0;
            for (const size_t witness_index :
                 original_contact.source_witness_indices) {
                if (witness_index >= raw_witness_world.size()) {
                    preserves_assignment = false;
                    break;
                }
                maximum_mapping_distance = std::max(
                    maximum_mapping_distance,
                    (raw_witness_world[witness_index] - candidate_world).norm());
                if (!contact_covers_witness(
                        candidate, raw_contacts[witness_index])) {
                    preserves_assignment = false;
                    break;
                }
            }
            if (!preserves_assignment)
                continue;
            ++route_aware_exact_alternative_candidate_count;
            ExactAlternativeRouteResult alternative =
                try_exact_alternative_route(
                    candidate, route_index, raw_index,
                    maximum_lateral_slope,
                    "route_aware_exact_alternative_layer", true);
            if (!alternative.accepted) {
                ++route_aware_exact_alternative_exhausted_count;
                if (debug != nullptr) {
                    debug->add_record(
                        BeltSupportDebugStageId::RouteClassification,
                        "route_aware_exact_alternative",
                        "exact_reachable_area_exhausted_or_backtrack_failed",
                        {{"contact_index",
                          static_cast<double>(route.contact_index)},
                         {"raw_contact_index",
                          static_cast<double>(raw_index)},
                         {"processed_layer_count", static_cast<double>(
                              alternative.processed_layer_count)},
                         {"failure_slice_s_mm", alternative.failure_s},
                         {"maximum_mapping_distance_mm",
                          maximum_mapping_distance}});
                }
                continue;
            }

            BeltSupportContact replacement = candidate;
            replacement.source_witness_indices =
                original_contact.source_witness_indices;
            replacement.source_surface_component_index =
                original_contact.source_surface_component_index;
            contacts[route.contact_index] = replacement;
            route.points = std::move(alternative.points);
            route.tip_s = candidate.tip_s;
            route.root_s = route.points.back().s;
            route.parent_route_index = alternative.rooted
                ? BeltSupportRoute::no_parent
                : alternative.parent_index;
            route.merge_s = alternative.merge_s;
            route.failure_reason.clear();
            ++route_aware_exact_alternative_rescued_contact_count;
            if (alternative.rooted)
                ++route_aware_exact_alternative_rooted_contact_count;
            else
                ++route_aware_exact_alternative_parent_attached_contact_count;
            ++reachable_contact_count;
            --unreachable_contact_count;
            rescued = true;
            if (debug != nullptr) {
                for (size_t point_index = 1;
                     point_index < route.points.size(); ++point_index) {
                    debug->add_line(
                        BeltSupportDebugStageId::RouteClassification,
                        local_point_to_world(
                            Point::new_scale(
                                route.points[point_index - 1].u,
                                route.points[point_index - 1].v),
                            route.points[point_index - 1].s,
                            center_u, center_v, *coordinates),
                        local_point_to_world(
                            Point::new_scale(
                                route.points[point_index].u,
                                route.points[point_index].v),
                            route.points[point_index].s,
                            center_u, center_v, *coordinates),
                        "route_aware_exact_alternative");
                }
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "route_aware_exact_alternative",
                    alternative.rooted
                        ? "coverage_preserving_candidate_reached_build_plate"
                        : "coverage_preserving_candidate_connected_to_parent",
                    {{"contact_index",
                      static_cast<double>(route.contact_index)},
                     {"raw_contact_index", static_cast<double>(raw_index)},
                     {"parent_contact_index",
                      alternative.rooted ? -1.0 : static_cast<double>(
                          routes[alternative.parent_index].contact_index)},
                     {"tip_s_mm", candidate.tip_s},
                     {"merge_s_mm", alternative.merge_s},
                     {"maximum_mapping_distance_mm",
                      maximum_mapping_distance},
                     {"maximum_lateral_slope",
                      alternative.maximum_lateral_slope},
                     {"processed_layer_count", static_cast<double>(
                          alternative.processed_layer_count)}});
            }
            break;
        }
        (void)rescued;
    }

    // Diagnostic only: preserve the configured result, but measure whether a
    // failed contact becomes geometrically reachable at larger branch-cone
    // angles supported by the existing option. This separates a configuration
    // limit from a topologically sealed free-space corridor.
    size_t route_aware_angle_sweep_contact_count = 0;
    size_t route_aware_angle_sweep_candidate_count = 0;
    size_t route_aware_angle_sweep_attempt_count = 0;
    size_t route_aware_angle_sweep_feasible_candidate_count = 0;
    size_t route_aware_angle_sweep_feasible_contact_count = 0;
    size_t route_aware_angle_sweep_unreachable_at_60_count = 0;
    double route_aware_angle_sweep_minimum_required_angle_deg =
        std::numeric_limits<double>::max();
    double route_aware_angle_sweep_maximum_required_angle_deg = 0.0;
    const double configured_lateral_angle_deg =
        maximum_lateral_angle * 180.0 / M_PI;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        const BeltSupportRoute& route = routes[route_index];
        if (route.failure_reason.empty())
            continue;
        ++route_aware_angle_sweep_contact_count;
        const BeltSupportContact& original_contact =
            contacts[route.contact_index];
        const auto component_it = raw_contacts_by_surface_component.find(
            original_contact.source_surface_component_index);
        if (component_it == raw_contacts_by_surface_component.end())
            continue;

        double best_angle_deg = std::numeric_limits<double>::max();
        size_t best_raw_index = std::numeric_limits<size_t>::max();
        double best_mapping_distance = 0.0;
        ExactAlternativeRouteResult best_alternative;
        for (const size_t raw_index : component_it->second) {
            const BeltSupportContact& candidate = raw_contacts[raw_index];
            const Vec3d candidate_world = local_point_to_world(
                candidate.center_local, candidate.tip_s,
                center_u, center_v, *coordinates);
            bool preserves_assignment = true;
            double maximum_mapping_distance = 0.0;
            for (const size_t witness_index :
                 original_contact.source_witness_indices) {
                if (witness_index >= raw_witness_world.size()) {
                    preserves_assignment = false;
                    break;
                }
                maximum_mapping_distance = std::max(
                    maximum_mapping_distance,
                    (raw_witness_world[witness_index] - candidate_world).norm());
                if (!contact_covers_witness(
                        candidate, raw_contacts[witness_index])) {
                    preserves_assignment = false;
                    break;
                }
            }
            if (!preserves_assignment)
                continue;
            ++route_aware_angle_sweep_candidate_count;

            bool candidate_feasible = false;
            for (double angle_deg = 45.0;
                 angle_deg <= 60.0 + EPSILON;
                 angle_deg += 5.0) {
                if (angle_deg <= configured_lateral_angle_deg + EPSILON ||
                    angle_deg >= best_angle_deg - EPSILON) {
                    continue;
                }
                ++route_aware_angle_sweep_attempt_count;
                const double slope_limit =
                    std::tan(angle_deg * M_PI / 180.0);
                ExactAlternativeRouteResult alternative =
                    try_exact_alternative_route(
                        candidate, route_index, raw_index,
                        slope_limit,
                        "route_aware_angle_sweep_layer", false);
                if (debug != nullptr) {
                    debug->add_record(
                        BeltSupportDebugStageId::RouteClassification,
                        "route_aware_angle_sweep",
                        alternative.accepted
                            ? "candidate_reachable_at_diagnostic_angle"
                            : "candidate_unreachable_at_diagnostic_angle",
                        {{"contact_index",
                          static_cast<double>(route.contact_index)},
                         {"raw_contact_index",
                          static_cast<double>(raw_index)},
                         {"diagnostic_angle_deg", angle_deg},
                         {"diagnostic_slope_limit", slope_limit},
                         {"accepted", alternative.accepted ? 1.0 : 0.0},
                         {"processed_layer_count", static_cast<double>(
                              alternative.processed_layer_count)},
                         {"failure_slice_s_mm", alternative.failure_s},
                         {"maximum_lateral_slope",
                          alternative.maximum_lateral_slope},
                         {"maximum_mapping_distance_mm",
                          maximum_mapping_distance},
                         {"rooted", alternative.rooted ? 1.0 : 0.0},
                         {"parent_contact_index",
                          alternative.accepted && !alternative.rooted
                              ? static_cast<double>(routes[
                                    alternative.parent_index].contact_index)
                              : -1.0}});
                }
                if (!alternative.accepted)
                    continue;
                candidate_feasible = true;
                best_angle_deg = angle_deg;
                best_raw_index = raw_index;
                best_mapping_distance = maximum_mapping_distance;
                best_alternative = std::move(alternative);
                break;
            }
            if (candidate_feasible)
                ++route_aware_angle_sweep_feasible_candidate_count;
        }

        if (best_raw_index == std::numeric_limits<size_t>::max()) {
            ++route_aware_angle_sweep_unreachable_at_60_count;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "route_aware_angle_sweep_contact",
                    "no_coverage_preserving_candidate_reachable_up_to_60_deg",
                    {{"contact_index",
                      static_cast<double>(route.contact_index)},
                     {"surface_component_index", static_cast<double>(
                          original_contact.source_surface_component_index)},
                     {"configured_angle_deg",
                      configured_lateral_angle_deg}});
            }
            continue;
        }

        ++route_aware_angle_sweep_feasible_contact_count;
        route_aware_angle_sweep_minimum_required_angle_deg = std::min(
            route_aware_angle_sweep_minimum_required_angle_deg,
            best_angle_deg);
        route_aware_angle_sweep_maximum_required_angle_deg = std::max(
            route_aware_angle_sweep_maximum_required_angle_deg,
            best_angle_deg);
        if (debug != nullptr) {
            for (size_t point_index = 1;
                 point_index < best_alternative.points.size(); ++point_index) {
                debug->add_line(
                    BeltSupportDebugStageId::RouteClassification,
                    local_point_to_world(
                        Point::new_scale(
                            best_alternative.points[point_index - 1].u,
                            best_alternative.points[point_index - 1].v),
                        best_alternative.points[point_index - 1].s,
                        center_u, center_v, *coordinates),
                    local_point_to_world(
                        Point::new_scale(
                            best_alternative.points[point_index].u,
                            best_alternative.points[point_index].v),
                        best_alternative.points[point_index].s,
                        center_u, center_v, *coordinates),
                    "route_aware_angle_sweep_feasible");
            }
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "route_aware_angle_sweep_contact",
                "reachable_only_above_configured_angle",
                {{"contact_index",
                  static_cast<double>(route.contact_index)},
                 {"raw_contact_index",
                  static_cast<double>(best_raw_index)},
                 {"surface_component_index", static_cast<double>(
                      original_contact.source_surface_component_index)},
                 {"configured_angle_deg", configured_lateral_angle_deg},
                 {"minimum_tested_reachable_angle_deg", best_angle_deg},
                 {"maximum_mapping_distance_mm", best_mapping_distance},
                 {"rooted", best_alternative.rooted ? 1.0 : 0.0}});
        }
    }

    // A consolidated contact is useful only if its full assigned witness set
    // can be represented by a contact which also has a printable fixed-V
    // route. Audit every contact that required the experimental 2D fallback
    // against all raw candidates in the same surface component. This does not
    // mutate the result: it tells the next consolidation revision whether the
    // failure belongs to contact selection or to the fixed-V route search.
    size_t alternative_audited_contact_count = 0;
    size_t contact_with_coverage_preserving_fixed_v_alternative_count = 0;
    size_t coverage_preserving_fixed_v_alternative_count = 0;
    for (const size_t route_index : exact_area_attempted_route_indices) {
        const BeltSupportRoute& route = routes[route_index];
        const BeltSupportContact& effective = contacts[route.contact_index];
        const auto component_it = raw_contacts_by_surface_component.find(
            effective.source_surface_component_index);
        if (component_it == raw_contacts_by_surface_component.end())
            continue;
        ++alternative_audited_contact_count;
        size_t preserving_candidate_count = 0;
        size_t viable_candidate_count = 0;
        double best_maximum_mapping_distance =
            std::numeric_limits<double>::max();
        size_t best_raw_contact_index = BeltSupportRoute::no_parent;
        for (const size_t raw_index : component_it->second) {
            const BeltSupportContact& candidate_contact = raw_contacts[raw_index];
            const Vec3d candidate_world = local_point_to_world(
                candidate_contact.center_local, candidate_contact.tip_s,
                center_u, center_v, *coordinates);
            double maximum_mapping_distance = 0.0;
            bool preserves_assignment = true;
            for (const size_t witness_index : effective.source_witness_indices) {
                if (witness_index >= raw_witness_world.size()) {
                    preserves_assignment = false;
                    break;
                }
                maximum_mapping_distance = std::max(
                    maximum_mapping_distance,
                    (raw_witness_world[witness_index] - candidate_world).norm());
                if (!contact_covers_witness(
                        candidate_contact,
                        raw_contacts[witness_index])) {
                    preserves_assignment = false;
                    break;
                }
            }
            if (!preserves_assignment)
                continue;
            ++preserving_candidate_count;

            BeltSupportRoute candidate_route;
            std::string rejection_reason;
            double failure_slice_s = 0.0;
            const bool fixed_v_reachable = try_sloped_v_route(
                candidate_contact, route.contact_index, 0.0,
                forbidden_by_plan, true,
                candidate_route, rejection_reason, failure_slice_s);
            if (!fixed_v_reachable)
                continue;
            ++viable_candidate_count;
            ++coverage_preserving_fixed_v_alternative_count;
            if (maximum_mapping_distance < best_maximum_mapping_distance) {
                best_maximum_mapping_distance = maximum_mapping_distance;
                best_raw_contact_index = raw_index;
            }
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "coverage_preserving_fixed_v_alternative",
                    "candidate_reaches_ground_without_v_drift",
                    {{"contact_index", static_cast<double>(route.contact_index)},
                     {"raw_contact_index", static_cast<double>(raw_index)},
                     {"maximum_mapping_distance_mm", maximum_mapping_distance},
                     {"mapping_radius_mm", witness_mapping_radius},
                     {"route_point_count",
                      static_cast<double>(candidate_route.points.size())}});
            }
        }
        if (viable_candidate_count > 0)
            ++contact_with_coverage_preserving_fixed_v_alternative_count;
        if (debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "fixed_v_alternative_summary",
                viable_candidate_count > 0
                    ? "coverage_preserving_alternative_exists"
                    : "no_coverage_preserving_alternative",
                {{"contact_index", static_cast<double>(route.contact_index)},
                 {"assigned_witness_count",
                  static_cast<double>(effective.source_witness_indices.size())},
                 {"preserving_candidate_count",
                  static_cast<double>(preserving_candidate_count)},
                 {"fixed_v_reachable_candidate_count",
                  static_cast<double>(viable_candidate_count)},
                 {"best_raw_contact_index",
                  best_raw_contact_index == BeltSupportRoute::no_parent
                      ? -1.0 : static_cast<double>(best_raw_contact_index)},
                 {"best_maximum_mapping_distance_mm",
                  best_raw_contact_index == BeltSupportRoute::no_parent
                      ? -1.0 : best_maximum_mapping_distance}});
        }
    }

    struct RouteAwareCandidateAudit
    {
        size_t raw_contact_index{0};
        std::vector<size_t> covered_witness_indices;
        BeltSupportRoute route;
    };
    size_t route_aware_raw_candidate_count = 0;
    size_t route_aware_fixed_v_reachable_candidate_count = 0;
    size_t route_aware_selected_contact_count = 0;
    size_t route_aware_covered_witness_count = 0;
    size_t route_aware_uncovered_witness_count = 0;
    size_t route_aware_fully_coverable_surface_component_count = 0;
    size_t route_aware_uncoverable_surface_component_count = 0;
    for (const auto& [surface_component, raw_indices] :
         raw_contacts_by_surface_component) {
        std::vector<RouteAwareCandidateAudit> candidates;
        candidates.reserve(raw_indices.size());
        for (const size_t raw_index : raw_indices) {
            ++route_aware_raw_candidate_count;
            const BeltSupportContact& candidate_contact = raw_contacts[raw_index];
            BeltSupportRoute candidate_route;
            std::string rejection_reason;
            double failure_slice_s = 0.0;
            if (!try_sloped_v_route(
                    candidate_contact, raw_index, 0.0,
                    forbidden_by_plan, true,
                    candidate_route, rejection_reason, failure_slice_s)) {
                continue;
            }
            ++route_aware_fixed_v_reachable_candidate_count;
            RouteAwareCandidateAudit candidate;
            candidate.raw_contact_index = raw_index;
            candidate.route = std::move(candidate_route);
            for (const size_t witness_index : raw_indices) {
                if (contact_covers_witness(
                        candidate_contact,
                        raw_contacts[witness_index])) {
                    candidate.covered_witness_indices.push_back(witness_index);
                }
            }
            candidates.emplace_back(std::move(candidate));
        }

        std::set<size_t> uncovered(raw_indices.begin(), raw_indices.end());
        std::vector<size_t> selected_candidate_indices;
        while (!uncovered.empty()) {
            size_t best_candidate = BeltSupportRoute::no_parent;
            size_t best_new_coverage = 0;
            for (size_t candidate_index = 0;
                 candidate_index < candidates.size(); ++candidate_index) {
                const size_t new_coverage = static_cast<size_t>(std::count_if(
                    candidates[candidate_index].covered_witness_indices.begin(),
                    candidates[candidate_index].covered_witness_indices.end(),
                    [&uncovered](size_t witness_index) {
                        return uncovered.count(witness_index) != 0;
                    }));
                if (new_coverage > best_new_coverage) {
                    best_candidate = candidate_index;
                    best_new_coverage = new_coverage;
                }
            }
            if (best_candidate == BeltSupportRoute::no_parent ||
                best_new_coverage == 0) {
                break;
            }
            selected_candidate_indices.push_back(best_candidate);
            for (const size_t witness_index :
                 candidates[best_candidate].covered_witness_indices) {
                uncovered.erase(witness_index);
            }
        }

        route_aware_selected_contact_count += selected_candidate_indices.size();
        route_aware_covered_witness_count += raw_indices.size() - uncovered.size();
        route_aware_uncovered_witness_count += uncovered.size();
        if (uncovered.empty())
            ++route_aware_fully_coverable_surface_component_count;
        else
            ++route_aware_uncoverable_surface_component_count;

        if (debug != nullptr) {
            for (const size_t selected_index : selected_candidate_indices) {
                const RouteAwareCandidateAudit& selected = candidates[selected_index];
                const BeltSupportContact& selected_contact =
                    raw_contacts[selected.raw_contact_index];
                const Vec3d selected_world = local_point_to_world(
                    selected_contact.center_local, selected_contact.tip_s,
                    center_u, center_v, *coordinates);
                debug->add_point(
                    BeltSupportDebugStageId::RouteClassification,
                    selected_world, 0.32,
                    "route_aware_fixed_v_contact");
                for (const size_t witness_index :
                     selected.covered_witness_indices) {
                    debug->add_line(
                        BeltSupportDebugStageId::RouteClassification,
                        raw_witness_world[witness_index], selected_world,
                        "route_aware_witness_coverage");
                }
                debug->add_record(
                    BeltSupportDebugStageId::RouteClassification,
                    "route_aware_selected_contact",
                    "fixed_v_reachable_set_cover_candidate",
                    {{"surface_component_index",
                      static_cast<double>(surface_component)},
                     {"raw_contact_index",
                      static_cast<double>(selected.raw_contact_index)},
                     {"covered_witness_count",
                      static_cast<double>(
                          selected.covered_witness_indices.size())},
                     {"route_point_count",
                      static_cast<double>(selected.route.points.size())}});
            }
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "route_aware_surface_component_coverage",
                uncovered.empty()
                    ? "all_witnesses_have_fixed_v_route_coverage"
                    : "some_witnesses_have_no_fixed_v_route_coverage",
                {{"surface_component_index",
                  static_cast<double>(surface_component)},
                 {"raw_witness_count", static_cast<double>(raw_indices.size())},
                 {"fixed_v_reachable_candidate_count",
                  static_cast<double>(candidates.size())},
                 {"selected_contact_count",
                  static_cast<double>(selected_candidate_indices.size())},
                 {"covered_witness_count",
                  static_cast<double>(raw_indices.size() - uncovered.size())},
                 {"uncovered_witness_count",
                  static_cast<double>(uncovered.size())}});
        }
    }

    // A child branch is structurally connected to a parent only when its
    // centerline actually terminates on the parent's centerline. Earlier
    // routing passes accepted overlapping cross-sections as a merge. With a
    // branch already using its full lateral slope, that small overlap is
    // progressively clipped away and leaves an apparently reachable tip
    // floating in air. Normalize every parent edge into a real, collision-free
    // centerline junction before any route is accepted by the topology audit.
    size_t parent_axis_connection_rewritten_count = 0;
    size_t parent_axis_connection_rejected_count = 0;
    for (size_t child_index = 0; child_index < routes.size(); ++child_index) {
        BeltSupportRoute& child = routes[child_index];
        if (!child.failure_reason.empty() || child.points.size() < 2 ||
            child.parent_route_index == BeltSupportRoute::no_parent) {
            continue;
        }
        const size_t parent_index = child.parent_route_index;
        if (parent_index >= routes.size() ||
            !routes[parent_index].failure_reason.empty() ||
            routes[parent_index].points.size() < 2) {
            child.failure_reason = "parent_route_not_structurally_connected";
            ++parent_axis_connection_rejected_count;
            --reachable_contact_count;
            ++unreachable_contact_count;
            continue;
        }
        const BeltSupportRoute& parent = routes[parent_index];

        bool connected = false;
        std::vector<BeltRoutePoint> connected_points;
        double connected_s = 0.0;
        // Prefer the highest valid junction. If the overlap point cannot be
        // snapped directly, replace as little of the child tail as necessary
        // and continue it down to an exact point on the parent axis.
        for (size_t child_point_count = child.points.size();
             child_point_count > 0 && !connected; --child_point_count) {
            const BeltRoutePoint& upper = child.points[child_point_count - 1];
            for (size_t reverse_plan_index = plans.size();
                 reverse_plan_index > 0; --reverse_plan_index) {
                const double candidate_s =
                    plans[reverse_plan_index - 1].slice_s;
                if (candidate_s >= upper.s - EPSILON ||
                    candidate_s > parent.points.front().s + EPSILON ||
                    candidate_s < parent.points.back().s - EPSILON ||
                    candidate_s > child.tip_s - contact_cap_depth + EPSILON ||
                    candidate_s > parent.tip_s - contact_cap_depth + EPSILON) {
                    continue;
                }
                const BeltRoutePoint junction{
                    route_u_at(parent, candidate_s),
                    route_v_at(parent, candidate_s),
                    candidate_s};
                if (!segment_stays_outside_model(
                        upper, junction, child.tip_s)) {
                    continue;
                }
                connected_points.assign(
                    child.points.begin(),
                    child.points.begin() +
                        static_cast<std::ptrdiff_t>(child_point_count));
                connected_points.push_back(junction);
                connected_s = candidate_s;
                connected = true;
                break;
            }
        }

        if (!connected) {
            child.failure_reason =
                "no_angle_and_collision_valid_parent_axis_connection";
            ++parent_axis_connection_rejected_count;
            --reachable_contact_count;
            ++unreachable_contact_count;
            continue;
        }
        child.points = std::move(connected_points);
        child.root_s = connected_s;
        child.merge_s = connected_s;
        ++parent_axis_connection_rewritten_count;
        if (debug != nullptr) {
            const BeltRoutePoint& junction = child.points.back();
            debug->add_point(
                BeltSupportDebugStageId::TreeTopology,
                local_point_to_world(
                    Point::new_scale(junction.u, junction.v), junction.s,
                    center_u, center_v, *coordinates),
                0.2, "parent_axis_junction");
            debug->add_record(
                BeltSupportDebugStageId::TreeTopology,
                "parent_axis_connection",
                "child_centerline_terminates_on_parent_centerline",
                {{"child_route_index", static_cast<double>(child_index)},
                 {"child_contact_index",
                  static_cast<double>(child.contact_index)},
                 {"parent_route_index", static_cast<double>(parent_index)},
                 {"parent_contact_index",
                  static_cast<double>(parent.contact_index)},
                 {"junction_s_mm", junction.s}});
        }
    }

    // Reject descendants of any connection which could not be normalized.
    bool rejected_parent_propagated = true;
    while (rejected_parent_propagated) {
        rejected_parent_propagated = false;
        for (BeltSupportRoute& route : routes) {
            if (!route.failure_reason.empty() ||
                route.parent_route_index == BeltSupportRoute::no_parent) {
                continue;
            }
            if (route.parent_route_index >= routes.size() ||
                !routes[route.parent_route_index].failure_reason.empty()) {
                route.failure_reason = "parent_route_not_structurally_connected";
                ++parent_axis_connection_rejected_count;
                --reachable_contact_count;
                ++unreachable_contact_count;
                rejected_parent_propagated = true;
            }
        }
    }

    // Every accepted branch must remain inside the configured lateral cone
    // around the Belt manufacturing normal. U and V are both physical lateral
    // axes in the oriented layer; their combined Euclidean slope is therefore
    // the quantity which must be bounded before structural frustums are built.
    constexpr double route_direction_tolerance = 1e-5;
    size_t direction_audited_route_count = 0;
    size_t branch_angle_contract_violation_route_count = 0;
    size_t route_with_v_drift_count = 0;
    double maximum_route_v_drift = 0.0;
    double maximum_route_v_slope = 0.0;
    double maximum_route_lateral_slope = 0.0;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        const BeltSupportRoute& route = routes[route_index];
        if (!route.failure_reason.empty() || route.points.size() < 2)
            continue;
        ++direction_audited_route_count;
        const double expected_v = unscaled<double>(
            contacts[route.contact_index].center_local.y());
        double route_v_drift = 0.0;
        double route_v_slope = 0.0;
        double route_lateral_slope = 0.0;
        for (size_t point_index = 0;
             point_index < route.points.size(); ++point_index) {
            route_v_drift = std::max(
                route_v_drift,
                std::abs(route.points[point_index].v - expected_v));
            if (point_index == 0)
                continue;
            const double delta_s = std::abs(
                route.points[point_index - 1].s - route.points[point_index].s);
            if (delta_s > EPSILON) {
                const double delta_u =
                    route.points[point_index - 1].u -
                    route.points[point_index].u;
                const double delta_v =
                    route.points[point_index - 1].v -
                    route.points[point_index].v;
                route_v_slope = std::max(
                    route_v_slope, std::abs(delta_v) / delta_s);
                route_lateral_slope = std::max(
                    route_lateral_slope,
                    std::sqrt(delta_u * delta_u + delta_v * delta_v) /
                        delta_s);
            }
        }
        if (route_v_drift > route_direction_tolerance)
            ++route_with_v_drift_count;
        maximum_route_v_drift = std::max(
            maximum_route_v_drift, route_v_drift);
        maximum_route_v_slope = std::max(
            maximum_route_v_slope, route_v_slope);
        maximum_route_lateral_slope = std::max(
            maximum_route_lateral_slope, route_lateral_slope);
        const bool violates_contract =
            route_lateral_slope >
                maximum_lateral_slope + route_direction_tolerance;
        if (violates_contract)
            ++branch_angle_contract_violation_route_count;
        if (debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::RouteClassification,
                "route_direction_contract",
                violates_contract
                    ? "exceeds_configured_lateral_branch_cone"
                    : "inside_configured_lateral_branch_cone",
                {{"contact_index", static_cast<double>(route.contact_index)},
                 {"maximum_abs_v_drift_mm", route_v_drift},
                 {"maximum_abs_v_slope", route_v_slope},
                 {"maximum_lateral_slope", route_lateral_slope},
                 {"configured_maximum_lateral_slope",
                  maximum_lateral_slope},
                 {"slope_tolerance", route_direction_tolerance}});
        }
    }
    // Topology decisions must use the same printable section which will be
    // installed later. A theoretical rectangle overlap before world-plate,
    // model and clearance clipping is not a physical branch connection.
    const auto printable_route_section =
        [&](const BeltSupportRoute& route, size_t plan_index) {
            ExPolygons printable;
            if (route.points.size() < 2 || plan_index >= plans.size())
                return printable;
            const double slice_s = plans[plan_index].slice_s;
            if (slice_s < route.points.back().s - EPSILON ||
                slice_s > route.points.front().s + EPSILON) {
                return printable;
            }
            const double half_width = std::min(
                maximum_half_width,
                tip_half_width +
                    std::max(0.0, route.tip_s - slice_s) * taper);
            const Point section_center = Point::new_scale(
                route_u_at(route, slice_s), route_v_at(route, slice_s));
            printable = clip_section_to_world_build_halfspace(
                ExPolygon(rectangle_at(
                    section_center, half_width, half_width)),
                slice_s, center_v, *coordinates);
            const ExPolygons& model_solid =
                model_solid_by_plan[plan_index].regions;
            if (!model_solid.empty())
                printable = diff_ex(printable, model_solid);
            const ExPolygons& forbidden =
                forbidden_by_plan[plan_index].regions;
            if (route.tip_s - slice_s > contact_cap_depth + EPSILON &&
                !forbidden.empty()) {
                printable = diff_ex(printable, forbidden);
            }

            // A model obstacle may split the analytical rectangle. Only the
            // component containing the route skeleton belongs to this route.
            ExPolygons center_connected;
            for (ExPolygon& component : printable) {
                if (component.contains(section_center))
                    center_connected.emplace_back(std::move(component));
            }
            return center_connected;
        };

    // Do not perform a second overlap-only merge here. Every accepted parent
    // edge has already been normalized to an exact centerline junction above;
    // creating another edge from section overlap would reintroduce the
    // floating-branch failure this topology contract is intended to prevent.

    // Parent edges created by the earlier rescue passes were selected from
    // reachable-area data. Revalidate those edges against the final trimmed
    // route sections as well; otherwise a child may start beside, rather than
    // on, its nominal parent. Invalid descendants are rejected explicitly and
    // remain available to the interface-roof coverage audit below.
    size_t post_trim_parent_connection_rejected_count = 0;
    bool parent_validation_changed = true;
    while (parent_validation_changed) {
        parent_validation_changed = false;
        for (size_t child_index = 0;
             child_index < routes.size(); ++child_index) {
            BeltSupportRoute& child = routes[child_index];
            if (!child.failure_reason.empty() || child.points.size() < 2 ||
                child.parent_route_index == BeltSupportRoute::no_parent) {
                continue;
            }
            const size_t parent_index = child.parent_route_index;
            const bool parent_exists = parent_index < routes.size();
            const bool parent_valid = parent_exists &&
                routes[parent_index].failure_reason.empty() &&
                routes[parent_index].points.size() >= 2;
            size_t connection_plan_index = plans.size();
            double connection_plan_delta =
                std::numeric_limits<double>::max();
            const double connection_s = child.points.back().s;
            for (size_t plan_index = 0;
                 plan_index < plans.size(); ++plan_index) {
                const double delta = std::abs(
                    plans[plan_index].slice_s - connection_s);
                if (delta < connection_plan_delta) {
                    connection_plan_delta = delta;
                    connection_plan_index = plan_index;
                }
            }

            ExPolygons child_printable;
            ExPolygons parent_printable;
            double printable_overlap_mm2 = 0.0;
            double centerline_junction_distance_mm =
                std::numeric_limits<double>::infinity();
            if (parent_valid && connection_plan_index < plans.size() &&
                connection_plan_delta <= 2.0 * SCALING_FACTOR + EPSILON) {
                centerline_junction_distance_mm = std::hypot(
                    child.points.back().u -
                        route_u_at(routes[parent_index], connection_s),
                    child.points.back().v -
                        route_v_at(routes[parent_index], connection_s));
                child_printable = printable_route_section(
                    child, connection_plan_index);
                parent_printable = printable_route_section(
                    routes[parent_index], connection_plan_index);
                printable_overlap_mm2 = overlap_area_mm2(
                    child_printable, parent_printable);
            }
            if (parent_valid &&
                centerline_junction_distance_mm <= 2.0 * SCALING_FACTOR &&
                printable_overlap_mm2 > 1e-12) {
                continue;
            }

            child.failure_reason = parent_valid
                ? "parent_connection_lost_after_section_trimming"
                : "parent_route_not_structurally_connected";
            ++post_trim_parent_connection_rejected_count;
            --reachable_contact_count;
            ++unreachable_contact_count;
            parent_validation_changed = true;
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::TreeTopology,
                    "parent_connection_rejected",
                    child.failure_reason,
                    {{"child_route_index",
                      static_cast<double>(child_index)},
                     {"child_contact_index",
                      static_cast<double>(child.contact_index)},
                     {"parent_route_index",
                      static_cast<double>(parent_index)},
                     {"connection_s_mm", connection_s},
                     {"connection_plan_index",
                      connection_plan_index == plans.size()
                          ? -1.0
                          : static_cast<double>(connection_plan_index)},
                     {"connection_plan_delta_mm",
                      connection_plan_delta},
                     {"centerline_junction_distance_mm",
                      std::isfinite(centerline_junction_distance_mm)
                          ? centerline_junction_distance_mm : -1.0},
                     {"child_printable_area_mm2",
                      expolygons_area_mm2(child_printable)},
                     {"parent_printable_area_mm2",
                      expolygons_area_mm2(parent_printable)},
                     {"printable_overlap_mm2",
                      printable_overlap_mm2}});
            }
        }
    }

    // Keep the selected contact tips as immutable audit facts. The route
    // branches below are rendering/slicing primitives and intentionally do
    // not carry witness identity; replacing the contact set with them must not
    // make later coverage validation read default witness coordinates.
    const std::vector<BeltSupportContact> effective_contacts = contacts;
    std::vector<BeltSupportContact> route_branches;
    for (size_t route_index = 0; route_index < routes.size(); ++route_index) {
        const BeltSupportRoute& route = routes[route_index];
        if (!route.failure_reason.empty() || route.points.size() < 2)
            continue;
        for (size_t point_index = 1; point_index < route.points.size(); ++point_index) {
            const BeltRoutePoint& upper = route.points[point_index - 1];
            const BeltRoutePoint& lower = route.points[point_index];
            if (upper.s <= lower.s + EPSILON)
                continue;
            BeltSupportContact branch;
            branch.center_local = Point::new_scale(
                upper.u, upper.v);
            branch.root_u_local = scaled<coord_t>(lower.u);
            branch.root_v_local = scaled<coord_t>(lower.v);
            branch.tip_s = upper.s;
            branch.root_s = lower.s;
            branch.tip_half_width = std::min(
                maximum_half_width,
                tip_half_width + std::max(0.0, route.tip_s - upper.s) * taper);
            branch.root_half_width = std::min(
                maximum_half_width,
                tip_half_width + std::max(0.0, route.tip_s - lower.s) * taper);
            branch.source_contact_index = route.contact_index;
            branch.source_route_index = route_index;
            branch.parent_route_index = route.parent_route_index;
            branch.root_edge =
                route.parent_route_index == BeltSupportRoute::no_parent &&
                point_index + 1 == route.points.size();
            route_branches.emplace_back(std::move(branch));
        }
    }
    contacts = std::move(route_branches);

    // Tree tips are load-bearing anchors. Interface material may only occupy
    // the part of the requested roof that is already carried by a real rooted
    // branch section. Never manufacture an in-plane bridge from a nearby
    // branch: that bridge would make the final 2D footprint connected without
    // proving that the roof was printable when its G-code was emitted.
    std::vector<ExPolygons> requested_interface_roofs_by_plan(plans.size());
    size_t interface_roof_source_witness_count = 0;
    size_t interface_roof_source_unreachable_witness_count = 0;
    size_t interface_roof_skipped_without_effective_contact_count = 0;
    size_t interface_roof_requested_plan_count = 0;
    if (interface_depth > EPSILON) {
        for (size_t raw_index = 0; raw_index < raw_contacts.size(); ++raw_index) {
            if (raw_index >= raw_to_effective_contact.size())
                continue;
            const size_t effective_index = raw_to_effective_contact[raw_index];
            if (effective_index == BeltSupportRoute::no_parent ||
                effective_index >= effective_contacts.size() ||
                effective_index >= routes.size()) {
                ++interface_roof_skipped_without_effective_contact_count;
                continue;
            }
            if (!routes[effective_index].failure_reason.empty())
                ++interface_roof_source_unreachable_witness_count;
            const BeltSupportContact& raw = raw_contacts[raw_index];
            if (raw.source_overhang_region_index >=
                overhang_region_infos.size()) {
                continue;
            }

            Polygon influence = make_circle(
                scale_(tip_half_width),
                scale_(std::min(0.01, 0.1 * tip_half_width)));
            influence.translate(raw.witness_local);
            const ExPolygons patch = intersection_ex(
                ExPolygons{overhang_region_infos[
                    raw.source_overhang_region_index].geometry},
                ExPolygons{ExPolygon(std::move(influence))});
            if (patch.empty())
                continue;

            ++interface_roof_source_witness_count;
            const double minimum_roof_s = raw.tip_s - interface_depth;
            const size_t first_plan = coverage_plan_index_for_s(minimum_roof_s);
            const size_t last_plan = coverage_plan_index_for_s(raw.tip_s);
            for (size_t plan_index = first_plan;
                 plan_index <= last_plan && plan_index < plans.size();
                 ++plan_index) {
                if (plans[plan_index].slice_s + EPSILON < minimum_roof_s ||
                    plans[plan_index].slice_s - EPSILON > raw.tip_s) {
                    continue;
                }
                append(requested_interface_roofs_by_plan[plan_index], patch);
            }
        }
        for (ExPolygons& requested : requested_interface_roofs_by_plan) {
            if (requested.empty())
                continue;
            requested = union_ex(requested);
            ++interface_roof_requested_plan_count;
        }
    }
    if (debug != nullptr) {
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "reachable_interval_count",
                          static_cast<double>(reachable_interval_count));
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "maximum_lateral_angle_deg",
                          maximum_lateral_angle * 180.0 / M_PI);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "reachable_contact_count",
                          static_cast<double>(reachable_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "direct_reachable_contact_count",
                          static_cast<double>(direct_reachable_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "unreachable_contact_count",
                          static_cast<double>(unreachable_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "rescued_contact_count",
                          static_cast<double>(rescued_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "solid_only_rescued_contact_count",
                          static_cast<double>(solid_only_rescued_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "solid_only_rejected_contact_count",
                          static_cast<double>(solid_only_rejected_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "sloped_v_attempt_count",
                          static_cast<double>(sloped_v_attempt_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "sloped_v_rescued_contact_count",
                          static_cast<double>(sloped_v_rescued_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "sloped_v_invalid_root_count",
                          static_cast<double>(sloped_v_invalid_root_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "sloped_v_corridor_rejected_count",
                          static_cast<double>(sloped_v_corridor_rejected_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "sloped_v_backtrack_rejected_count",
                          static_cast<double>(sloped_v_backtrack_rejected_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "search_attempted_contact_count",
                          static_cast<double>(search_attempted_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "search_rescued_contact_count",
                          static_cast<double>(search_rescued_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "search_parent_attached_contact_count",
                          static_cast<double>(search_parent_attached_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "search_rooted_contact_count",
                          static_cast<double>(search_rooted_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "search_failed_contact_count",
                          static_cast<double>(search_failed_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "exact_area_attempted_contact_count",
                          static_cast<double>(exact_area_attempted_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "exact_area_rescued_contact_count",
                          static_cast<double>(exact_area_rescued_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "exact_area_rooted_contact_count",
                          static_cast<double>(exact_area_rooted_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "exact_area_parent_attached_contact_count",
                          static_cast<double>(exact_area_parent_attached_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "exact_area_failed_contact_count",
                          static_cast<double>(exact_area_failed_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uv_alternative_contact_count",
            static_cast<double>(
                route_aware_uv_alternative_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uv_preserving_candidate_count",
            static_cast<double>(
                route_aware_uv_preserving_candidate_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uv_angle_candidate_count",
            static_cast<double>(route_aware_uv_angle_candidate_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uv_collision_rejected_count",
            static_cast<double>(
                route_aware_uv_collision_rejected_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uv_stability_rejected_count",
            static_cast<double>(
                route_aware_uv_stability_rejected_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uv_rescued_contact_count",
            static_cast<double>(route_aware_uv_rescued_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_exact_alternative_candidate_count",
            static_cast<double>(
                route_aware_exact_alternative_candidate_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_exact_alternative_rescued_contact_count",
            static_cast<double>(
                route_aware_exact_alternative_rescued_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_exact_alternative_parent_attached_contact_count",
            static_cast<double>(
                route_aware_exact_alternative_parent_attached_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_exact_alternative_rooted_contact_count",
            static_cast<double>(
                route_aware_exact_alternative_rooted_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_exact_alternative_exhausted_count",
            static_cast<double>(
                route_aware_exact_alternative_exhausted_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_contact_count",
            static_cast<double>(route_aware_angle_sweep_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_candidate_count",
            static_cast<double>(route_aware_angle_sweep_candidate_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_attempt_count",
            static_cast<double>(route_aware_angle_sweep_attempt_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_feasible_candidate_count",
            static_cast<double>(
                route_aware_angle_sweep_feasible_candidate_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_feasible_contact_count",
            static_cast<double>(
                route_aware_angle_sweep_feasible_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_unreachable_at_60_count",
            static_cast<double>(
                route_aware_angle_sweep_unreachable_at_60_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_minimum_required_angle_deg",
            route_aware_angle_sweep_minimum_required_angle_deg <
                    std::numeric_limits<double>::max()
                ? route_aware_angle_sweep_minimum_required_angle_deg
                : -1.0);
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_angle_sweep_maximum_required_angle_deg",
            route_aware_angle_sweep_maximum_required_angle_deg);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "alternative_audited_contact_count",
                          static_cast<double>(alternative_audited_contact_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "contact_with_coverage_preserving_fixed_v_alternative_count",
            static_cast<double>(
                contact_with_coverage_preserving_fixed_v_alternative_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "coverage_preserving_fixed_v_alternative_count",
            static_cast<double>(coverage_preserving_fixed_v_alternative_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "route_aware_raw_candidate_count",
                          static_cast<double>(route_aware_raw_candidate_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_fixed_v_reachable_candidate_count",
            static_cast<double>(
                route_aware_fixed_v_reachable_candidate_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "route_aware_selected_contact_count",
                          static_cast<double>(route_aware_selected_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "route_aware_covered_witness_count",
                          static_cast<double>(route_aware_covered_witness_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "route_aware_uncovered_witness_count",
                          static_cast<double>(route_aware_uncovered_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_fully_coverable_surface_component_count",
            static_cast<double>(
                route_aware_fully_coverable_surface_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::RouteClassification,
            "route_aware_uncoverable_surface_component_count",
            static_cast<double>(
                route_aware_uncoverable_surface_component_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "direction_audited_route_count",
                          static_cast<double>(direction_audited_route_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "branch_angle_contract_violation_route_count",
                          static_cast<double>(
                              branch_angle_contract_violation_route_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "route_with_v_drift_count",
                          static_cast<double>(route_with_v_drift_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "maximum_route_v_drift_mm",
                          maximum_route_v_drift);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "maximum_route_v_slope",
                          maximum_route_v_slope);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "maximum_route_lateral_slope",
                          maximum_route_lateral_slope);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "configured_maximum_lateral_slope",
                          maximum_lateral_slope);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "route_slope_tolerance",
                          route_direction_tolerance);
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "exact_area_processed_layer_count",
                          static_cast<double>(exact_area_processed_layer_count));
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "exact_area_maximum_component_count",
                          static_cast<double>(exact_area_maximum_component_count));
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "exact_area_maximum_vertex_count",
                          static_cast<double>(exact_area_maximum_vertex_count));
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "exact_area_simplification_tolerance_mm",
                          exact_area_simplification_tolerance);
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "search_expanded_state_count",
                          static_cast<double>(search_expanded_state_count));
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "search_collision_rejected_state_count",
                          static_cast<double>(search_collision_rejected_state_count));
        debug->set_metric(BeltSupportDebugStageId::ReachableCorridors,
                          "search_below_plate_rejected_state_count",
                          static_cast<double>(search_below_plate_rejected_state_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "uv_parent_rescued_contact_count",
                          static_cast<double>(uv_parent_rescued_contact_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "uv_parent_candidate_count",
                          static_cast<double>(uv_parent_candidate_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "uv_parent_exact_candidate_count",
                          static_cast<double>(uv_parent_exact_candidate_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "uv_parent_angle_rejected_count",
                          static_cast<double>(uv_parent_angle_rejected_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "uv_parent_collision_rejected_count",
                          static_cast<double>(uv_parent_collision_rejected_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "uv_parent_stability_rejected_count",
                          static_cast<double>(uv_parent_stability_rejected_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "blocked_interval_count",
                          static_cast<double>(blocked_interval_count));
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "contact_cap_depth_mm", contact_cap_depth);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "minimum_collision_half_width_mm",
                          tip_half_width);
        debug->set_metric(BeltSupportDebugStageId::RouteClassification,
                          "structural_radius_trimmed_after_routing", 1.0);
        debug->set_metric(BeltSupportDebugStageId::TreeTopology,
                          "route_branch_count",
                          static_cast<double>(contacts.size()));
        debug->set_metric(
            BeltSupportDebugStageId::TreeTopology,
            "parent_axis_connection_rewritten_count",
            static_cast<double>(parent_axis_connection_rewritten_count));
        debug->set_metric(
            BeltSupportDebugStageId::TreeTopology,
            "parent_axis_connection_rejected_count",
            static_cast<double>(parent_axis_connection_rejected_count));
        debug->set_metric(
            BeltSupportDebugStageId::TreeTopology,
            "post_trim_parent_connection_rejected_count",
            static_cast<double>(
                post_trim_parent_connection_rejected_count));
        debug->set_metric(BeltSupportDebugStageId::TreeTopology,
                          "root_trunk_count",
                          static_cast<double>(std::count_if(
                              routes.begin(), routes.end(),
                              [](const BeltSupportRoute& route) {
                                  return route.failure_reason.empty() &&
                                      route.points.size() >= 2 &&
                                      route.parent_route_index ==
                                          BeltSupportRoute::no_parent;
                              })));
    }

    if (contacts.empty() && debug != nullptr)
        return;
    if (contacts.empty())
        throw SlicingError("Belt tapered support could not connect any overhang contact to the build plate.");

    double minimum_root_s = std::numeric_limits<double>::max();
    double maximum_root_s = std::numeric_limits<double>::lowest();
    coord_t minimum_root_u = std::numeric_limits<coord_t>::max();
    coord_t maximum_root_u = std::numeric_limits<coord_t>::lowest();
    std::vector<const BeltSupportContact*> root_edges;
    {
        BeltSupportDebugStageTimer selected_timer(debug, BeltSupportDebugStageId::TreeTopology);
        BeltSupportDebugStageTimer primitive_timer(debug, BeltSupportDebugStageId::TaperedPrimitives);
        for (const BeltSupportContact& contact : contacts) {
            if (contact.root_edge) {
                root_edges.push_back(&contact);
                minimum_root_s = std::min(minimum_root_s, contact.root_s);
                maximum_root_s = std::max(maximum_root_s, contact.root_s);
                minimum_root_u = std::min(
                    minimum_root_u,
                    contact.root_u_local - scaled<coord_t>(contact.root_half_width));
                maximum_root_u = std::max(
                    maximum_root_u,
                    contact.root_u_local + scaled<coord_t>(contact.root_half_width));
            }

            if (debug != nullptr) {
                const Point root_local(contact.root_u_local, contact.root_v_local);
                const Vec3d root_world = local_point_to_world(
                    root_local, contact.root_s, center_u, center_v, *coordinates);
                const Vec3d tip_world = local_point_to_world(
                    contact.center_local, contact.tip_s, center_u, center_v, *coordinates);
                debug->add_line(BeltSupportDebugStageId::TreeTopology,
                                root_world, tip_world, "tree_branch");
                debug->add_line(BeltSupportDebugStageId::TaperedPrimitives,
                                root_world, tip_world, "frustum_axis");
                debug_expolygons(
                    debug, BeltSupportDebugStageId::TaperedPrimitives,
                    clip_section_to_world_build_halfspace(
                        ExPolygon(rectangle_at(
                            root_local, contact.root_half_width,
                            contact.root_half_width)),
                        contact.root_s, center_v, *coordinates),
                    contact.root_s, center_u, center_v, *coordinates,
                    "frustum_root_section");
                debug_expolygons(
                    debug, BeltSupportDebugStageId::TaperedPrimitives,
                    clip_section_to_world_build_halfspace(
                        ExPolygon(rectangle_at(
                            contact.center_local, contact.tip_half_width,
                            contact.tip_half_width)),
                        contact.tip_s, center_v, *coordinates),
                    contact.tip_s, center_u, center_v, *coordinates,
                    "frustum_tip_section");
            }
        }
    }
    if (minimum_root_s == std::numeric_limits<double>::max()) {
        if (debug != nullptr) {
            debug->add_record(BeltSupportDebugStageId::TreeTopology,
                              "root_foundation", "no_route_reaches_build_plate", {});
            return;
        }
        throw SlicingError("Belt tapered support has no route edge on the build plate.");
    }
    if (debug != nullptr) {
        debug->set_metric(
            BeltSupportDebugStageId::TreeTopology,
            "reachable_route_count",
            static_cast<double>(std::count_if(
                routes.begin(), routes.end(),
                [](const BeltSupportRoute& route) {
                    return route.failure_reason.empty() &&
                        route.points.size() >= 2;
                })));
        debug->set_metric(BeltSupportDebugStageId::TaperedPrimitives, "frustum_count",
                          static_cast<double>(contacts.size()));
    }
    const double maximum_plan_height = std::accumulate(
        plans.begin(), plans.end(), 0.0,
        [](double value, const BeltSupportLayerPlan& plan) {
            return std::max(value, plan.height);
        });
    const double foundation_depth_s = std::max(
        maximum_plan_height,
        static_cast<double>(support_parameters.first_layer_flow.width()));
    Points foundation_points;
    foundation_points.reserve(root_edges.size() * 4);
    for (const BeltSupportContact* root : root_edges) {
        const coord_t left_u =
            root->root_u_local - scaled<coord_t>(root->root_half_width);
        const coord_t right_u =
            root->root_u_local + scaled<coord_t>(root->root_half_width);
        const coord_t front_s = scaled<coord_t>(
            std::max(0.0, root->root_s - foundation_depth_s));
        const coord_t back_s = scaled<coord_t>(
            root->root_s + foundation_depth_s);
        foundation_points.emplace_back(left_u, front_s);
        foundation_points.emplace_back(right_u, front_s);
        foundation_points.emplace_back(right_u, back_s);
        foundation_points.emplace_back(left_u, back_s);
    }
    BeltRootFoundation root_foundation{
        Geometry::convex_hull(std::move(foundation_points))};
    const BoundingBox root_foundation_bounds =
        get_extents(root_foundation.footprint_us);
    if (!root_foundation_bounds.defined)
        throw SlicingError("Belt tapered support could not build a connected root foundation.");
    const double foundation_front_s =
        unscaled<double>(root_foundation_bounds.min.y());
    const double foundation_back_s =
        unscaled<double>(root_foundation_bounds.max.y());

    size_t first_nonempty_plan = plans.size();
    size_t section_count = 0;
    size_t build_plate_clipped_section_count = 0;
    size_t section_trimmed_by_model_count = 0;
    size_t section_trimmed_by_clearance_count = 0;
    size_t section_fully_removed_count = 0;
    size_t section_center_lost_count = 0;
    size_t section_detached_fragment_count = 0;
    double section_detached_fragment_area_mm2 = 0.0;
    size_t path_input_root_count = 0;
    size_t first_path_input_root_path_count = 0;
    size_t first_path_input_root_segment_count = 0;
    double first_path_input_base_area_before_birth_clear_mm2 = 0.0;
    double first_path_input_interface_area_before_birth_clear_mm2 = 0.0;
    size_t interface_fill_path_count = 0;
    size_t interface_fill_fallback_component_count = 0;
    double interface_fill_path_length_mm = 0.0;
    double interface_fill_region_area_mm2 = 0.0;
    double interface_fill_footprint_covered_area_mm2 = 0.0;
    double minimum_section_world_z = std::numeric_limits<double>::max();
    double maximum_root_input_abs_world_z = 0.0;
    double section_area_removed_by_model_mm2 = 0.0;
    double section_area_removed_by_clearance_mm2 = 0.0;
    double residual_section_model_overlap_mm2 = 0.0;
    size_t interface_roof_connected_plan_count = 0;
    size_t interface_roof_rejected_plan_count = 0;
    size_t interface_roof_direct_component_count = 0;
    double interface_roof_requested_area_mm2 = 0.0;
    double interface_roof_accepted_area_mm2 = 0.0;
    double interface_roof_rejected_area_mm2 = 0.0;
    size_t causal_section_trimmed_component_count = 0;
    double causal_section_trimmed_area_mm2 = 0.0;
    ExPolygons previous_causal_structural_region;
    size_t previous_causal_plan_index = plans.size();
    {
        BeltSupportDebugStageTimer section_timer(debug, BeltSupportDebugStageId::LayerSections);
        BeltSupportDebugStageTimer path_timer(debug, BeltSupportDebugStageId::PathInputs);
        for (size_t plan_index = 0; plan_index < plans.size(); ++plan_index) {
            BeltSupportLayerPlan& plan = plans[plan_index];
            detector.throw_on_cancel();

            plan.root_paths = make_root_paths(
                plan.slice_s,
                plan.print_s,
                root_foundation, center_v, *coordinates);

            const ExPolygons& model_solid = model_solid_by_plan[plan_index].regions;
            const ExPolygons& forbidden = forbidden_by_plan[plan_index].regions;
            if (!plan.root_paths.empty() && !forbidden.empty())
                plan.root_paths = diff_pl(plan.root_paths, to_polygons(forbidden));

            ExPolygons base_sections;
            ExPolygons interface_sections;
            for (const BeltSupportContact& contact : contacts) {
                if (plan.slice_s + EPSILON < contact.root_s ||
                    plan.slice_s - EPSILON > contact.tip_s)
                    continue;

                const double segment_span = contact.tip_s - contact.root_s;
                const double root_ratio = segment_span <= EPSILON
                    ? 0.0
                    : std::clamp((contact.tip_s - plan.slice_s) / segment_span,
                                 0.0, 1.0);
                const double half_width =
                    contact.tip_half_width +
                    (contact.root_half_width - contact.tip_half_width) * root_ratio;
                const double distance_to_contact_tip = std::max(
                    0.0,
                    contact.tip_s - plan.slice_s +
                    std::max(0.0,
                        (contact.tip_half_width - tip_half_width) / std::max(taper, EPSILON)));
                const Point section_center =
                    branch_center_at(contact, plan.slice_s);
                ExPolygon untrimmed_section(rectangle_at(
                    section_center, half_width, half_width));
                const BoundingBox untrimmed_bounds = get_extents(untrimmed_section);
                const coord_t minimum_printable_v = scaled<coord_t>(
                    coordinates->belt_boundary_v(plan.slice_s) - center_v);
                if (untrimmed_bounds.defined &&
                    untrimmed_bounds.min.y() < minimum_printable_v) {
                    ++build_plate_clipped_section_count;
                }
                ExPolygons printable_sections =
                    clip_section_to_world_build_halfspace(
                        untrimmed_section, plan.slice_s, center_v,
                        *coordinates);
                const double build_plate_clipped_area =
                    expolygons_area_mm2(printable_sections);
                if (!model_solid.empty()) {
                    printable_sections = diff_ex(
                        printable_sections, model_solid);
                    const double after_model_area =
                        expolygons_area_mm2(printable_sections);
                    const double removed_area = std::max(
                        0.0, build_plate_clipped_area - after_model_area);
                    if (removed_area > 1e-8) {
                        ++section_trimmed_by_model_count;
                        section_area_removed_by_model_mm2 += removed_area;
                    }
                }
                const double after_model_area =
                    expolygons_area_mm2(printable_sections);
                if (distance_to_contact_tip > contact_cap_depth + EPSILON &&
                    !forbidden.empty()) {
                    printable_sections = diff_ex(
                        printable_sections, forbidden);
                    const double after_clearance_area =
                        expolygons_area_mm2(printable_sections);
                    const double removed_area = std::max(
                        0.0, after_model_area - after_clearance_area);
                    if (removed_area > 1e-8) {
                        ++section_trimmed_by_clearance_count;
                        section_area_removed_by_clearance_mm2 += removed_area;
                    }
                }
                if (build_plate_clipped_area > 1e-8 &&
                    printable_sections.empty()) {
                    ++section_fully_removed_count;
                }
                const bool center_retained = std::any_of(
                    printable_sections.begin(), printable_sections.end(),
                    [&section_center](const ExPolygon& section) {
                        return section.contains(section_center);
                    });
                if (!center_retained) {
                    ++section_center_lost_count;
                    if (debug != nullptr) {
                        const Vec3d center_world = local_point_to_world(
                            section_center, plan.slice_s,
                            center_u, center_v, *coordinates);
                        debug->add_point(
                            BeltSupportDebugStageId::LayerSections,
                            center_world, 0.18, "trimmed_section_lost_center");
                        debug->add_record(
                            BeltSupportDebugStageId::LayerSections,
                            "section_trimming", "branch_center_not_retained",
                            {{"source_contact_index",
                              static_cast<double>(contact.source_contact_index)},
                             {"plan_index", static_cast<double>(plan_index)},
                             {"slice_s_mm", plan.slice_s},
                             {"structural_half_width_mm", half_width},
                             {"area_before_trim_mm2", build_plate_clipped_area},
                             {"area_after_trim_mm2",
                              expolygons_area_mm2(printable_sections)}});
                    }
                }
                ExPolygons center_connected_sections;
                center_connected_sections.reserve(printable_sections.size());
                for (size_t component_index = 0;
                     component_index < printable_sections.size();
                     ++component_index) {
                    ExPolygon& component = printable_sections[component_index];
                    if (component.contains(section_center)) {
                        center_connected_sections.emplace_back(
                            std::move(component));
                        continue;
                    }
                    ++section_detached_fragment_count;
                    const double detached_area_mm2 =
                        expolygons_area_mm2(ExPolygons{component});
                    section_detached_fragment_area_mm2 += detached_area_mm2;
                    if (debug != nullptr) {
                        debug_polygon(
                            debug, BeltSupportDebugStageId::LayerSections,
                            component.contour, plan.slice_s,
                            center_u, center_v, *coordinates,
                            "trimmed_section_detached_fragment");
                        for (const Polygon& hole : component.holes) {
                            debug_polygon(
                                debug,
                                BeltSupportDebugStageId::LayerSections,
                                hole, plan.slice_s,
                                center_u, center_v, *coordinates,
                                "trimmed_section_detached_fragment");
                        }
                        debug->add_record(
                            BeltSupportDebugStageId::LayerSections,
                            "section_trimming",
                            "detached_from_route_skeleton_after_trimming",
                            {{"source_route_index",
                              static_cast<double>(contact.source_route_index)},
                             {"source_contact_index",
                              static_cast<double>(contact.source_contact_index)},
                             {"plan_index", static_cast<double>(plan_index)},
                             {"slice_s_mm", plan.slice_s},
                             {"component_index",
                              static_cast<double>(component_index)},
                             {"detached_area_mm2", detached_area_mm2}});
                    }
                }
                printable_sections = std::move(center_connected_sections);
                residual_section_model_overlap_mm2 +=
                    overlap_area_mm2(printable_sections, model_solid);
                const double interface_height =
                    std::max(0, m_object.config().support_interface_top_layers.value) *
                    m_slicing_parameters.layer_height;
                const bool interface =
                    contact.tip_half_width <= tip_half_width + EPSILON &&
                    distance_to_contact_tip <= interface_height + EPSILON;
                for (size_t section_component_index = 0;
                     section_component_index < printable_sections.size();
                     ++section_component_index) {
                    ExPolygon& section = printable_sections[section_component_index];
                    const double section_area_mm2 =
                        expolygons_area_mm2(ExPolygons{section});
                    for (const Point& point : section.contour.points) {
                        const double world_z = local_point_to_world(
                            point, plan.slice_s, center_u, center_v,
                            *coordinates).z();
                        minimum_section_world_z = std::min(
                            minimum_section_world_z, world_z);
                        if (world_z < -1e-5) {
                            throw SlicingError(
                                "Belt tapered support section crosses below the world build plate.");
                        }
                    }
                    if (debug != nullptr) {
                        const Vec3d section_center_world = local_point_to_world(
                            safe_centroid(section), plan.slice_s,
                            center_u, center_v, *coordinates);
                        debug->add_record(
                            BeltSupportDebugStageId::LayerSections,
                            "branch_section_component",
                            interface
                                ? "printable_interface_section"
                                : "printable_base_section",
                            {{"source_route_index",
                              static_cast<double>(contact.source_route_index)},
                             {"source_contact_index",
                              static_cast<double>(contact.source_contact_index)},
                             {"parent_route_index",
                              contact.parent_route_index ==
                                      BeltSupportRoute::no_parent
                                  ? -1.0
                                  : static_cast<double>(
                                        contact.parent_route_index)},
                             {"plan_index", static_cast<double>(plan_index)},
                             {"section_component_index",
                              static_cast<double>(section_component_index)},
                             {"slice_s_mm", plan.slice_s},
                             {"branch_segment_tip_s_mm", contact.tip_s},
                             {"branch_segment_root_s_mm", contact.root_s},
                             {"distance_to_route_tip_mm",
                              distance_to_contact_tip},
                             {"structural_half_width_mm", half_width},
                             {"area_after_build_plate_clip_mm2",
                              build_plate_clipped_area},
                             {"area_after_model_clip_mm2",
                              after_model_area},
                             {"printable_component_area_mm2",
                              section_area_mm2},
                             {"root_edge", contact.root_edge ? 1.0 : 0.0},
                             {"center_world_x_mm", section_center_world.x()},
                             {"center_world_y_mm", section_center_world.y()},
                             {"center_world_z_mm", section_center_world.z()}});
                        debug_polygon(
                            debug, BeltSupportDebugStageId::LayerSections,
                            section.contour, plan.slice_s, center_u, center_v,
                            *coordinates,
                            interface ? "interface_section" : "base_section");
                    }
                    ++section_count;
                    if (interface)
                        interface_sections.emplace_back(std::move(section));
                    else
                        base_sections.emplace_back(std::move(section));
                }
            }

            plan.interface_regions = union_ex(interface_sections);
            plan.base_regions = diff_ex(union_ex(base_sections), plan.interface_regions);

            // A roof may start only from a real branch/root section on this
            // plane or material already printable on the preceding plane.
            // witness_mapping_radius is deliberately absent here: it is a
            // contact-consolidation radius, not a printable bridge length.
            ExPolygons requested_roof;
            for (const ExPolygon& requested :
                 requested_interface_roofs_by_plan[plan_index]) {
                ExPolygons printable =
                    clip_section_to_world_build_halfspace(
                        requested, plan.slice_s, center_v, *coordinates);
                if (!model_solid.empty())
                    printable = diff_ex(printable, model_solid);
                append(requested_roof, std::move(printable));
            }
            requested_roof = union_ex(requested_roof);
            const double requested_roof_area =
                expolygons_area_mm2(requested_roof);
            interface_roof_requested_area_mm2 += requested_roof_area;
            if (!requested_roof.empty()) {
                ExPolygons support_seeds = plan.base_regions;
                append(support_seeds, plan.interface_regions);

                const Flow plan_base_flow =
                    support_parameters.support_material_flow.with_height(
                        float(plan.height));
                if (!plan.root_paths.empty()) {
                    append(support_seeds, union_ex(offset(
                        plan.root_paths,
                        0.5f * plan_base_flow.scaled_width())));
                }
                if (previous_causal_plan_index != plans.size() &&
                    previous_causal_plan_index + 1 == plan_index &&
                    !previous_causal_structural_region.empty()) {
                    const double delta_s = std::max(
                        0.0,
                        plan.slice_s -
                            plans[previous_causal_plan_index].slice_s);
                    const double permitted_growth_mm = std::max(
                        0.001,
                        (maximum_lateral_slope + taper) * delta_s);
                    append(support_seeds, union_ex(offset(
                        previous_causal_structural_region,
                        scaled<coord_t>(permitted_growth_mm))));
                }
                support_seeds = union_ex(support_seeds);

                // Keep only the roof volume physically carried by a branch
                // section on this very slicing plane. Accepting an entire roof
                // component because one corner touched a seed produced large
                // one-ended sheets. Adding a same-layer connector afterwards
                // was even worse: it proved eventual 2D connectivity, not
                // causal printability. Unsupported roof area is deliberately
                // rejected here so route/contact generation remains the only
                // place allowed to create more load-bearing material.
                ExPolygons accepted_roof = intersection_ex(
                    requested_roof, support_seeds);
                ExPolygons rejected_roof = diff_ex(
                    requested_roof, accepted_roof);
                for (size_t component_index = 0;
                     component_index < requested_roof.size();
                     ++component_index) {
                    const ExPolygon& component = requested_roof[component_index];
                    const ExPolygons component_polygons{component};
                    const double carried_area_mm2 = overlap_area_mm2(
                        component_polygons, support_seeds);
                    if (carried_area_mm2 > 1e-12)
                        ++interface_roof_direct_component_count;
                    if (debug != nullptr) {
                        debug->add_record(
                            BeltSupportDebugStageId::LayerSections,
                            "interface_roof_component",
                            carried_area_mm2 > 1e-12
                                ? "clipped_to_rooted_branch_section"
                                : "rejected_without_rooted_branch_section",
                            {{"plan_index", static_cast<double>(plan_index)},
                             {"component_index", static_cast<double>(component_index)},
                             {"slice_s_mm", plan.slice_s},
                             {"component_area_mm2",
                              expolygons_area_mm2(component_polygons)},
                             {"carried_area_mm2", carried_area_mm2}});
                    }
                }
                accepted_roof = union_ex(accepted_roof);
                rejected_roof = union_ex(rejected_roof);
                const double accepted_area =
                    expolygons_area_mm2(accepted_roof);
                const double rejected_area =
                    expolygons_area_mm2(rejected_roof);
                interface_roof_accepted_area_mm2 += accepted_area;
                interface_roof_rejected_area_mm2 += rejected_area;
                if (!accepted_roof.empty()) {
                    ++interface_roof_connected_plan_count;
                    append(plan.interface_regions, accepted_roof);
                    plan.interface_regions = union_ex(plan.interface_regions);
                    plan.base_regions = diff_ex(
                        plan.base_regions, plan.interface_regions);
                }
                if (!rejected_roof.empty())
                    ++interface_roof_rejected_plan_count;
                if (debug != nullptr) {
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::LayerSections,
                        requested_roof, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "interface_roof_requested");
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::LayerSections,
                        accepted_roof, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "interface_roof_connected");
                    debug_expolygons(
                        debug, BeltSupportDebugStageId::LayerSections,
                        rejected_roof, plan.slice_s,
                        center_u, center_v, *coordinates,
                        "interface_roof_rejected");
                    debug->add_record(
                        BeltSupportDebugStageId::LayerSections,
                        "interface_roof_connectivity",
                        rejected_roof.empty()
                            ? "fully_connected_to_printable_support"
                            : (accepted_roof.empty()
                                ? "not_connected_to_printable_support"
                                : "partially_connected_to_printable_support"),
                        {{"plan_index", static_cast<double>(plan_index)},
                         {"slice_s_mm", plan.slice_s},
                         {"requested_area_mm2", requested_roof_area},
                         {"accepted_area_mm2", accepted_area},
                         {"rejected_area_mm2", rejected_area}});
                }
            }

            if (first_nonempty_plan == plans.size() &&
                (!plan.root_paths.empty() || !plan.base_regions.empty() ||
                 !plan.interface_regions.empty())) {
                first_nonempty_plan = plan_index;
                first_path_input_root_path_count = plan.root_paths.size();
                first_path_input_root_segment_count = std::accumulate(
                    plan.root_paths.begin(), plan.root_paths.end(), size_t{0},
                    [](size_t count, const Polyline& path) {
                        return count + (path.points.size() > 1
                            ? path.points.size() - 1 : 0);
                    });
                first_path_input_base_area_before_birth_clear_mm2 =
                    expolygons_area_mm2(plan.base_regions);
                first_path_input_interface_area_before_birth_clear_mm2 =
                    expolygons_area_mm2(plan.interface_regions);
                // The birth layer is represented by the build-plate intersection only.
                plan.base_regions.clear();
                plan.interface_regions.clear();
            }

            // Enforce causal growth while the cross-sections are still the
            // authoritative support geometry. A branch may start from this
            // plane's real build-plate line or grow from the preceding rooted
            // section by at most the configured branch slope plus its radial
            // taper. Anything outside that envelope is not deferred to the
            // path stage and is never repaired with an in-air connector.
            const Flow causal_flow = support_parameters.support_material_flow
                .with_height(float(plan.height));
            ExPolygons root_footprint;
            if (!plan.root_paths.empty()) {
                root_footprint = union_ex(offset(
                    plan.root_paths,
                    0.5f * causal_flow.scaled_width()));
            }
            ExPolygons structural_region = plan.base_regions;
            append(structural_region, plan.interface_regions);
            append(structural_region, root_footprint);
            structural_region = union_ex(structural_region);
            if (!structural_region.empty()) {
                const bool has_preceding_plane =
                    previous_causal_plan_index != plans.size() &&
                    previous_causal_plan_index + 1 == plan_index;
                const double delta_s = has_preceding_plane
                    ? std::max(
                          0.0,
                          plan.slice_s -
                              plans[previous_causal_plan_index].slice_s)
                    : plan.height;
                const double permitted_growth_mm = std::max(
                    0.001,
                    (maximum_lateral_slope + taper) * delta_s);
                ExPolygons causal_envelope = root_footprint;
                if (has_preceding_plane &&
                    !previous_causal_structural_region.empty()) {
                    append(causal_envelope, union_ex(offset(
                        previous_causal_structural_region,
                        scaled<coord_t>(permitted_growth_mm))));
                }
                causal_envelope = union_ex(causal_envelope);

                const ExPolygons rejected = causal_envelope.empty()
                    ? structural_region
                    : diff_ex(structural_region, causal_envelope);
                const double rejected_area_mm2 =
                    expolygons_area_mm2(rejected);
                if (rejected_area_mm2 > 1e-6) {
                    causal_section_trimmed_component_count +=
                        rejected.size();
                    causal_section_trimmed_area_mm2 +=
                        rejected_area_mm2;
                    plan.base_regions = intersection_ex(
                        plan.base_regions, causal_envelope);
                    plan.interface_regions = intersection_ex(
                        plan.interface_regions, causal_envelope);
                    if (debug != nullptr) {
                        debug_expolygons(
                            debug,
                            BeltSupportDebugStageId::LayerSections,
                            rejected, plan.print_s,
                            center_u, center_v, *coordinates,
                            "causal_section_trimmed");
                        debug->add_record(
                            BeltSupportDebugStageId::LayerSections,
                            "causal_section_growth",
                            "trimmed_beyond_preceding_printable_envelope",
                            {{"plan_index",
                              static_cast<double>(plan_index)},
                             {"slice_s_mm", plan.slice_s},
                             {"permitted_growth_mm",
                              permitted_growth_mm},
                             {"rejected_area_mm2",
                              rejected_area_mm2}});
                    }
                }

                previous_causal_structural_region = plan.base_regions;
                append(previous_causal_structural_region,
                       plan.interface_regions);
                append(previous_causal_structural_region,
                       root_footprint);
                previous_causal_structural_region = union_ex(
                    previous_causal_structural_region);
                previous_causal_plan_index = plan_index;
            }

            for (const Polyline& path : plan.root_paths) {
                for (size_t point_index = 1; point_index < path.points.size(); ++point_index) {
                    const Vec3d start_world = local_point_to_world(
                        path.points[point_index - 1], plan.print_s,
                        center_u, center_v, *coordinates);
                    const Vec3d end_world = local_point_to_world(
                        path.points[point_index], plan.print_s,
                        center_u, center_v, *coordinates);
                    maximum_root_input_abs_world_z = std::max(
                        maximum_root_input_abs_world_z,
                        std::max(std::abs(start_world.z()), std::abs(end_world.z())));
                    if (maximum_root_input_abs_world_z > 1e-5) {
                        throw SlicingError(
                            "Belt tapered support root path is not on the world build plate.");
                    }
                    if (debug != nullptr) {
                        debug->add_line(
                            BeltSupportDebugStageId::PathInputs,
                            start_world, end_world,
                            "root_open_path_input");
                    }
                    ++path_input_root_count;
                }
            }
            if (debug != nullptr) {
                debug_expolygons(debug, BeltSupportDebugStageId::PathInputs,
                                 plan.base_regions, plan.slice_s, center_u, center_v,
                                 *coordinates, "base_region_input");
                debug_expolygons(debug, BeltSupportDebugStageId::PathInputs,
                                 plan.interface_regions, plan.slice_s, center_u, center_v,
                                 *coordinates, "interface_region_input");
                if (!plan.interface_regions.empty()) {
                    const Flow audit_interface_flow =
                        support_parameters.support_material_interface_flow
                            .with_height(float(plan.height));
                    ExtrusionEntityCollection audit_interface_fill;
                    std::vector<InterfaceFillComponentAudit>
                        component_fill_audits;
                    const size_t fallback_component_count =
                        append_interface_fill(
                            audit_interface_fill.entities,
                            plan.interface_regions, audit_interface_flow,
                            support_parameters, plan_index,
                            &component_fill_audits);
                    interface_fill_fallback_component_count +=
                        fallback_component_count;
                    const Polylines audit_paths =
                        audit_interface_fill.as_polylines();
                    interface_fill_path_count += audit_paths.size();
                    double plan_path_length_mm = 0.0;
                    for (const Polyline& path : audit_paths) {
                        plan_path_length_mm +=
                            unscaled<double>(path.length());
                        for (size_t point_index = 1;
                             point_index < path.points.size(); ++point_index) {
                            debug->add_line(
                                BeltSupportDebugStageId::PathInputs,
                                local_point_to_world(
                                    path.points[point_index - 1],
                                    plan.slice_s, center_u, center_v,
                                    *coordinates),
                                local_point_to_world(
                                    path.points[point_index],
                                    plan.slice_s, center_u, center_v,
                                    *coordinates),
                                "interface_fill_path_input");
                        }
                    }
                    interface_fill_path_length_mm += plan_path_length_mm;
                    const double plan_region_area_mm2 =
                        expolygons_area_mm2(plan.interface_regions);
                    interface_fill_region_area_mm2 += plan_region_area_mm2;
                    const ExPolygons path_footprint = audit_paths.empty()
                        ? ExPolygons{}
                        : union_ex(offset(
                            audit_paths,
                            0.5f * audit_interface_flow.scaled_width()));
                    const double footprint_covered_area_mm2 =
                        overlap_area_mm2(
                            plan.interface_regions, path_footprint);
                    interface_fill_footprint_covered_area_mm2 +=
                        footprint_covered_area_mm2;
                    debug->add_record(
                        BeltSupportDebugStageId::PathInputs,
                        "interface_fill_paths",
                        audit_paths.empty()
                            ? "interface_region_generated_no_paths"
                            : "interface_region_filled_with_paths",
                        {{"plan_index", static_cast<double>(plan_index)},
                         {"slice_s_mm", plan.slice_s},
                         {"region_area_mm2", plan_region_area_mm2},
                         {"path_count",
                          static_cast<double>(audit_paths.size())},
                         {"fallback_component_count",
                          static_cast<double>(fallback_component_count)},
                         {"path_length_mm", plan_path_length_mm},
                         {"path_footprint_covered_area_mm2",
                          footprint_covered_area_mm2},
                         {"path_footprint_coverage_ratio",
                          plan_region_area_mm2 <= 1e-12
                              ? 1.0
                              : footprint_covered_area_mm2 /
                                    plan_region_area_mm2},
                         {"interface_density",
                          support_parameters.top_interface_density}});
                    for (const InterfaceFillComponentAudit& component :
                         component_fill_audits) {
                        debug->add_record(
                            BeltSupportDebugStageId::PathInputs,
                            "interface_fill_component",
                            component.path_count == 0
                                ? "component_generated_no_path"
                                : (component.used_fallback
                                    ? "component_filled_by_principal_axis_fallback"
                                    : "component_filled_by_configured_pattern"),
                            {{"plan_index", static_cast<double>(plan_index)},
                             {"slice_s_mm", plan.slice_s},
                             {"component_index", static_cast<double>(
                                  component.component_index)},
                             {"region_area_mm2", component.region_area_mm2},
                             {"entity_count", static_cast<double>(
                                  component.entity_count)},
                             {"path_count", static_cast<double>(
                                  component.path_count)},
                             {"used_fallback",
                              component.used_fallback ? 1.0 : 0.0},
                             {"path_length_mm", component.path_length_mm},
                             {"path_footprint_covered_area_mm2",
                              component.footprint_covered_area_mm2},
                             {"path_footprint_coverage_ratio",
                              component.region_area_mm2 <= 1e-12
                                  ? 1.0
                                  : component.footprint_covered_area_mm2 /
                                        component.region_area_mm2}});
                    }
                }
            }
        }
    }

    // Preserve coverage for every raw overhang witness, not only for the
    // contact retained by spacing consolidation. These facts decide whether
    // an unreachable contact genuinely needs another branch or may share a
    // nearby printable interface.
    struct RawWitnessCoverageStats
    {
        size_t total{0};
        size_t covered{0};
        size_t excluded_by_build_plate_policy{0};
        size_t mapped_to_reachable_route{0};
        size_t finite_nearest_distance_count{0};
        double nearest_distance_sum{0.0};
        double nearest_distance_maximum{0.0};
    };
    std::map<size_t, RawWitnessCoverageStats> raw_witness_component_coverage;
    size_t covered_raw_witness_count = 0;
    size_t uncovered_raw_witness_count = 0;
    size_t required_uncovered_raw_witness_count = 0;
    size_t build_plate_policy_excluded_raw_witness_count = 0;
    size_t uncovered_raw_witness_mapped_to_reachable_route_count = 0;
    for (size_t raw_index = 0; raw_index < raw_contacts.size(); ++raw_index) {
        const BeltSupportContact& raw = raw_contacts[raw_index];
        const size_t plan_index = coverage_plan_index_for_s(raw.tip_s);
        const BeltSupportLayerPlan& plan = plans[plan_index];
        const bool covered_by_interface = std::any_of(
            plan.interface_regions.begin(), plan.interface_regions.end(),
            [&raw](const ExPolygon& region) {
                return region.contains(raw.witness_local);
            });
        const bool covered_by_base = std::any_of(
            plan.base_regions.begin(), plan.base_regions.end(),
            [&raw](const ExPolygon& region) {
                return region.contains(raw.witness_local);
            });
        bool covered = covered_by_interface || covered_by_base;

        const size_t effective_index = raw_index < raw_to_effective_contact.size()
            ? raw_to_effective_contact[raw_index]
            : BeltSupportRoute::no_parent;
        const bool mapped_to_reachable_route =
            effective_index != BeltSupportRoute::no_parent &&
            effective_index < routes.size() &&
            routes[effective_index].failure_reason.empty();
        const bool mapped_to_proven_unreachable_route =
            effective_index != BeltSupportRoute::no_parent &&
            effective_index < routes.size() &&
            !routes[effective_index].failure_reason.empty();

        ExPolygons support_regions = plan.base_regions;
        append(support_regions, plan.interface_regions);
        double nearest_distance = std::numeric_limits<double>::infinity();
        Point nearest_point = raw.witness_local;
        if (!support_regions.empty()) {
            nearest_point = projection_onto(support_regions, raw.witness_local);
            nearest_distance = unscaled<double>(
                (nearest_point - raw.witness_local).norm());
        }
        const bool covered_with_coordinate_tolerance =
            !covered && std::isfinite(nearest_distance) &&
            nearest_distance <= 2.0 * SCALING_FACTOR;
        covered = covered || covered_with_coordinate_tolerance;
        if (covered)
            nearest_distance = 0.0;
        const bool excluded_by_build_plate_policy =
            !covered &&
            m_object.config().support_on_build_plate_only.value &&
            mapped_to_proven_unreachable_route;

        RawWitnessCoverageStats& component =
            raw_witness_component_coverage[
                raw.source_surface_component_index];
        ++component.total;
        if (covered) {
            ++component.covered;
            ++covered_raw_witness_count;
        } else {
            ++uncovered_raw_witness_count;
            if (excluded_by_build_plate_policy) {
                ++component.excluded_by_build_plate_policy;
                ++build_plate_policy_excluded_raw_witness_count;
            } else {
                ++required_uncovered_raw_witness_count;
            }
            if (mapped_to_reachable_route) {
                ++uncovered_raw_witness_mapped_to_reachable_route_count;
            }
        }
        if (mapped_to_reachable_route)
            ++component.mapped_to_reachable_route;
        if (std::isfinite(nearest_distance)) {
            ++component.finite_nearest_distance_count;
            component.nearest_distance_sum += nearest_distance;
            component.nearest_distance_maximum = std::max(
                component.nearest_distance_maximum, nearest_distance);
        }

        if (debug != nullptr) {
            const Vec3d witness_world = local_point_to_world(
                raw.witness_local, raw.tip_s,
                center_u, center_v, *coordinates);
            debug->add_point(
                BeltSupportDebugStageId::LayerSections,
                witness_world, 0.16,
                covered ? "raw_witness_covered_by_section"
                        : (excluded_by_build_plate_policy
                            ? "raw_witness_excluded_by_build_plate_policy"
                            : "raw_witness_uncovered_by_section"));
            if (!covered && std::isfinite(nearest_distance)) {
                debug->add_line(
                    BeltSupportDebugStageId::LayerSections,
                    witness_world,
                    local_point_to_world(
                        nearest_point, plan.slice_s,
                        center_u, center_v, *coordinates),
                    "raw_witness_to_nearest_support_section");
            }
            debug->add_record(
                BeltSupportDebugStageId::LayerSections,
                "raw_witness_section_coverage",
                covered_by_interface
                    ? "covered_by_interface_section"
                    : (covered_by_base
                        ? "covered_by_base_section"
                        : (covered_with_coordinate_tolerance
                            ? "covered_on_section_coordinate_boundary"
                            : (excluded_by_build_plate_policy
                                ? "excluded_unreachable_by_build_plate_only_policy"
                                : "required_but_not_covered_by_any_support_section"))),
                {{"raw_witness_index", static_cast<double>(raw_index)},
                 {"effective_contact_index",
                  effective_index == BeltSupportRoute::no_parent
                      ? -1.0 : static_cast<double>(effective_index)},
                 {"surface_component_index", static_cast<double>(
                      raw.source_surface_component_index)},
                 {"mapped_route_reachable",
                  mapped_to_reachable_route ? 1.0 : 0.0},
                 {"mapped_route_proven_unreachable",
                  mapped_to_proven_unreachable_route ? 1.0 : 0.0},
                 {"excluded_by_build_plate_only_policy",
                  excluded_by_build_plate_policy ? 1.0 : 0.0},
                 {"coverage_plan_index", static_cast<double>(plan_index)},
                 {"coverage_slice_s_mm", plan.slice_s},
                 {"tip_s_mm", raw.tip_s},
                 {"nearest_support_section_distance_mm",
                  std::isfinite(nearest_distance) ? nearest_distance : -1.0},
                 {"world_x_mm", witness_world.x()},
                 {"world_y_mm", witness_world.y()},
                 {"world_z_mm", witness_world.z()}});
        }
    }
    for (const auto& [surface_component, coverage] :
         raw_witness_component_coverage) {
        if (debug == nullptr)
            break;
        debug->add_record(
            BeltSupportDebugStageId::LayerSections,
            "raw_witness_surface_component_coverage",
            coverage.covered == coverage.total
                ? "fully_covered"
                : (coverage.covered > 0
                    ? "partially_covered" : "uncovered"),
            {{"surface_component_index", static_cast<double>(surface_component)},
             {"covered_raw_witness_count",
              static_cast<double>(coverage.covered)},
             {"build_plate_policy_excluded_raw_witness_count",
              static_cast<double>(
                  coverage.excluded_by_build_plate_policy)},
             {"total_raw_witness_count",
              static_cast<double>(coverage.total)},
             {"mapped_to_reachable_route_count",
              static_cast<double>(coverage.mapped_to_reachable_route)},
             {"mean_nearest_support_section_distance_mm",
              coverage.finite_nearest_distance_count == 0
                  ? -1.0
                  : coverage.nearest_distance_sum /
                        static_cast<double>(
                            coverage.finite_nearest_distance_count)},
             {"maximum_nearest_support_section_distance_mm",
              coverage.nearest_distance_maximum}});
    }

    size_t covered_effective_contact_count = 0;
    size_t uncovered_effective_contact_count = 0;
    size_t unreachable_but_covered_contact_count = 0;
    size_t unreachable_and_uncovered_contact_count = 0;
    std::map<size_t, std::pair<size_t, size_t>> surface_component_coverage;
    for (size_t effective_index = 0;
         effective_index < effective_contacts.size(); ++effective_index) {
        if (effective_index >= routes.size())
            break;
        const BeltSupportRoute& route = routes[effective_index];
        const BeltSupportContact& candidate = effective_contacts[effective_index];
        // A contact is supported from below. Never inspect the geometrically
        // closest plan when it lies above the contact tip: no branch section is
        // allowed to exist there, so doing so creates a false uncovered result.
        // Inspect the highest printable plan at or below the contact instead.
        const size_t plan_index = coverage_plan_index_for_s(candidate.tip_s);
        const BeltSupportLayerPlan& plan = plans[plan_index];
        bool covered = std::any_of(
            plan.interface_regions.begin(), plan.interface_regions.end(),
            [&candidate](const ExPolygon& region) {
                return region.contains(candidate.witness_local);
            }) || std::any_of(
            plan.base_regions.begin(), plan.base_regions.end(),
            [&candidate](const ExPolygon& region) {
                return region.contains(candidate.witness_local);
            });
        if (!covered) {
            ExPolygons support_regions = plan.base_regions;
            append(support_regions, plan.interface_regions);
            if (!support_regions.empty()) {
                const Point nearest = projection_onto(
                    support_regions, candidate.witness_local);
                covered = unscaled<double>(
                    (nearest - candidate.witness_local).norm()) <=
                    2.0 * SCALING_FACTOR;
            }
        }
        auto& [covered_in_component, total_in_component] =
            surface_component_coverage[candidate.source_surface_component_index];
        ++total_in_component;
        if (covered) {
            ++covered_in_component;
            ++covered_effective_contact_count;
            if (!route.failure_reason.empty())
                ++unreachable_but_covered_contact_count;
        } else {
            ++uncovered_effective_contact_count;
            if (!route.failure_reason.empty())
                ++unreachable_and_uncovered_contact_count;
        }
        if (debug != nullptr) {
            const Vec3d world = local_point_to_world(
                candidate.witness_local, candidate.tip_s,
                center_u, center_v, *coordinates);
            if (!covered) {
                debug->add_point(
                    BeltSupportDebugStageId::LayerSections,
                    world, 0.3, "uncovered_contact_candidate");
            }
            debug->add_record(
                BeltSupportDebugStageId::LayerSections,
                "effective_contact_coverage",
                covered ? "covered_by_support_section"
                        : "not_covered_by_support_section",
                {{"effective_contact_index", static_cast<double>(effective_index)},
                 {"overhang_region_index",
                  static_cast<double>(candidate.source_overhang_region_index)},
                 {"surface_component_index",
                  static_cast<double>(candidate.source_surface_component_index)},
                 {"mapped_raw_witness_count",
                  static_cast<double>(candidate.source_witness_indices.size())},
                 {"route_reachable", route.failure_reason.empty() ? 1.0 : 0.0},
                 {"coverage_plan_index", static_cast<double>(plan_index)},
                 {"coverage_slice_s_mm", plan.slice_s},
                 {"tip_s_mm", candidate.tip_s},
                 {"world_x_mm", world.x()},
                 {"world_y_mm", world.y()},
                 {"world_z_mm", world.z()}});
        }
    }
    size_t fully_covered_surface_component_count = 0;
    size_t partially_covered_surface_component_count = 0;
    size_t uncovered_surface_component_count = 0;
    for (const auto& [surface_component, coverage] : surface_component_coverage) {
        const auto [covered_count, total_count] = coverage;
        if (covered_count == total_count) {
            ++fully_covered_surface_component_count;
        } else if (covered_count > 0) {
            ++partially_covered_surface_component_count;
        } else {
            ++uncovered_surface_component_count;
        }
        if (debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::LayerSections,
                "surface_component_coverage",
                covered_count == total_count
                    ? "fully_covered"
                    : (covered_count > 0 ? "partially_covered" : "uncovered"),
                {{"surface_component_index", static_cast<double>(surface_component)},
                 {"covered_effective_contact_count", static_cast<double>(covered_count)},
                 {"total_effective_contact_count", static_cast<double>(total_count)}});
        }
    }

    if (debug != nullptr) {
        debug->set_metric(BeltSupportDebugStageId::LayerSections, "layer_plan_count",
                          static_cast<double>(plans.size()));
        debug->set_metric(BeltSupportDebugStageId::LayerSections, "section_count",
                          static_cast<double>(section_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_source_witness_count",
                          static_cast<double>(
                              interface_roof_source_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "interface_roof_source_unreachable_witness_count",
            static_cast<double>(
                interface_roof_source_unreachable_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "interface_roof_skipped_without_effective_contact_count",
            static_cast<double>(
                interface_roof_skipped_without_effective_contact_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_requested_plan_count",
                          static_cast<double>(
                              interface_roof_requested_plan_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_connected_plan_count",
                          static_cast<double>(
                              interface_roof_connected_plan_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_rejected_plan_count",
                          static_cast<double>(
                              interface_roof_rejected_plan_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_direct_component_count",
                          static_cast<double>(
                              interface_roof_direct_component_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_requested_area_mm2",
                          interface_roof_requested_area_mm2);
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_accepted_area_mm2",
                          interface_roof_accepted_area_mm2);
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "interface_roof_rejected_area_mm2",
                          interface_roof_rejected_area_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "causal_section_trimmed_component_count",
            static_cast<double>(
                causal_section_trimmed_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "causal_section_trimmed_area_mm2",
            causal_section_trimmed_area_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "build_plate_clipped_section_count",
            static_cast<double>(build_plate_clipped_section_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_trimmed_by_model_count",
            static_cast<double>(section_trimmed_by_model_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_trimmed_by_clearance_count",
            static_cast<double>(section_trimmed_by_clearance_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_fully_removed_count",
            static_cast<double>(section_fully_removed_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_center_lost_count",
            static_cast<double>(section_center_lost_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_detached_fragment_count",
            static_cast<double>(section_detached_fragment_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_detached_fragment_area_mm2",
            section_detached_fragment_area_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_area_removed_by_model_mm2",
            section_area_removed_by_model_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "section_area_removed_by_clearance_mm2",
            section_area_removed_by_clearance_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "residual_section_model_overlap_mm2",
            residual_section_model_overlap_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "minimum_section_world_z_mm",
            minimum_section_world_z == std::numeric_limits<double>::max()
                ? 0.0
                : minimum_section_world_z);
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "covered_effective_contact_count",
                          static_cast<double>(covered_effective_contact_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "uncovered_effective_contact_count",
                          static_cast<double>(uncovered_effective_contact_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "covered_raw_witness_count",
                          static_cast<double>(covered_raw_witness_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "uncovered_raw_witness_count",
                          static_cast<double>(uncovered_raw_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "required_uncovered_raw_witness_count",
            static_cast<double>(required_uncovered_raw_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "build_plate_policy_excluded_raw_witness_count",
            static_cast<double>(
                build_plate_policy_excluded_raw_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::LayerSections,
            "uncovered_raw_witness_mapped_to_reachable_route_count",
            static_cast<double>(
                uncovered_raw_witness_mapped_to_reachable_route_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "unreachable_but_covered_contact_count",
                          static_cast<double>(unreachable_but_covered_contact_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "unreachable_and_uncovered_contact_count",
                          static_cast<double>(unreachable_and_uncovered_contact_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "fully_covered_surface_component_count",
                          static_cast<double>(fully_covered_surface_component_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "partially_covered_surface_component_count",
                          static_cast<double>(partially_covered_surface_component_count));
        debug->set_metric(BeltSupportDebugStageId::LayerSections,
                          "uncovered_surface_component_count",
                          static_cast<double>(uncovered_surface_component_count));
        debug->set_metric(BeltSupportDebugStageId::PathInputs, "root_input_segment_count",
                          static_cast<double>(path_input_root_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "first_path_input_plan_index",
            first_nonempty_plan == plans.size()
                ? -1.0 : static_cast<double>(first_nonempty_plan));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "first_path_input_root_path_count",
            static_cast<double>(first_path_input_root_path_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "first_path_input_root_segment_count",
            static_cast<double>(first_path_input_root_segment_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "first_path_input_base_area_before_birth_clear_mm2",
            first_path_input_base_area_before_birth_clear_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "first_path_input_interface_area_before_birth_clear_mm2",
            first_path_input_interface_area_before_birth_clear_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "interface_fill_path_count",
            static_cast<double>(interface_fill_path_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "interface_fill_fallback_component_count",
            static_cast<double>(
                interface_fill_fallback_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "interface_fill_path_length_mm",
            interface_fill_path_length_mm);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "interface_fill_region_area_mm2",
            interface_fill_region_area_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "interface_fill_footprint_covered_area_mm2",
            interface_fill_footprint_covered_area_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "interface_fill_footprint_coverage_ratio",
            interface_fill_region_area_mm2 <= 1e-12
                ? 1.0
                : interface_fill_footprint_covered_area_mm2 /
                      interface_fill_region_area_mm2);
        debug->set_metric(BeltSupportDebugStageId::PathInputs,
                          "maximum_root_input_abs_world_z_mm",
                          maximum_root_input_abs_world_z);
        debug->set_metric(BeltSupportDebugStageId::PathInputs,
                          "final_support_generated", 0.0);
    }

    if (first_nonempty_plan == plans.size())
        throw SlicingError("Belt tapered support produced no printable layer intersections.");

    // The single-object V1 contract requires exactly one open path on the first
    // support layer. A clipped or disconnected root is a geometry failure, not a
    // reason to fall back to a closed tree perimeter.
    if (plans[first_nonempty_plan].root_paths.size() != 1)
    {
        BOOST_LOG_TRIVIAL(error)
            << "BeltTaperedSupport: disconnected root first_plan=" << first_nonempty_plan
            << " slice_s=" << plans[first_nonempty_plan].slice_s
            << " root_paths=" << plans[first_nonempty_plan].root_paths.size()
            << " root_s_range=(" << minimum_root_s << ',' << maximum_root_s << ')'
            << " foundation_s_range=(" << foundation_front_s << ',' << foundation_back_s << ')'
            << " root_u_range=(" << unscaled<double>(minimum_root_u) + center_u
            << ',' << unscaled<double>(maximum_root_u) + center_u << ')';
        throw SlicingError("Belt tapered support root is not one continuous build-plate line.");
    }

    // Validate the support as a causal sequence of cross-sections before any
    // extrusion paths are generated. A section is printable only when every
    // part of it lies inside either (a) a real build-plate root footprint on
    // this plane or (b) the preceding rooted section expanded by the maximum
    // lateral branch growth permitted for one layer. Merely touching the
    // previous section at one point is insufficient, and current/future paths
    // are never admitted as anchors.
    size_t final_unrooted_structural_component_count = 0;
    size_t first_unrooted_structural_plan_index = plans.size();
    double final_unrooted_structural_area_mm2 = 0.0;
    ExPolygons previous_rooted_structural_region;
    size_t previous_structural_plan_index = plans.size();
    for (size_t plan_index = 0; plan_index < plans.size(); ++plan_index) {
        const BeltSupportLayerPlan& plan = plans[plan_index];
        const Flow structural_flow = support_parameters.support_material_flow
            .with_height(float(plan.height));
        ExPolygons root_footprint;
        if (!plan.root_paths.empty()) {
            root_footprint = union_ex(offset(
                plan.root_paths, 0.5f * structural_flow.scaled_width()));
        }

        ExPolygons structural_region = plan.base_regions;
        append(structural_region, plan.interface_regions);
        append(structural_region, root_footprint);
        structural_region = union_ex(structural_region);
        if (structural_region.empty())
            continue;

        const bool has_preceding_plane =
            previous_structural_plan_index != plans.size() &&
            previous_structural_plan_index + 1 == plan_index;
        const double delta_s = has_preceding_plane
            ? std::max(0.0, plan.slice_s -
                plans[previous_structural_plan_index].slice_s)
            : plan.height;
        const double permitted_growth_mm = std::max(
            0.001, (maximum_lateral_slope + taper) * delta_s);

        ExPolygons causal_support = root_footprint;
        if (has_preceding_plane &&
            !previous_rooted_structural_region.empty()) {
            append(causal_support, union_ex(offset(
                previous_rooted_structural_region,
                scaled<coord_t>(permitted_growth_mm))));
        }
        causal_support = union_ex(causal_support);

        ExPolygons rooted_components;
        for (size_t component_index = 0;
             component_index < structural_region.size(); ++component_index) {
            const ExPolygon& component = structural_region[component_index];
            const ExPolygons component_polygons{component};
            const ExPolygons unsupported = causal_support.empty()
                ? component_polygons
                : diff_ex(component_polygons, causal_support);
            const double unsupported_area_mm2 =
                expolygons_area_mm2(unsupported);
            if (unsupported_area_mm2 <= 1e-6) {
                rooted_components.emplace_back(component);
                continue;
            }

            ++final_unrooted_structural_component_count;
            if (first_unrooted_structural_plan_index == plans.size()) {
                BOOST_LOG_TRIVIAL(error)
                    << "BeltTaperedSupport: first unrooted structural section"
                    << " plan=" << plan_index
                    << " component=" << component_index
                    << " slice_s=" << plan.slice_s
                    << " permitted_growth_mm=" << permitted_growth_mm
                    << " component_area_mm2="
                    << expolygons_area_mm2(component_polygons)
                    << " unsupported_area_mm2=" << unsupported_area_mm2
                    << " root_overlap_mm2="
                    << overlap_area_mm2(component_polygons, root_footprint)
                    << " previous_overlap_mm2="
                    << overlap_area_mm2(
                           component_polygons,
                           previous_rooted_structural_region);
            }
            first_unrooted_structural_plan_index = std::min(
                first_unrooted_structural_plan_index, plan_index);
            final_unrooted_structural_area_mm2 += unsupported_area_mm2;
            if (debug != nullptr) {
                debug_expolygons(
                    debug, BeltSupportDebugStageId::FinalSupport,
                    unsupported, plan.print_s,
                    center_u, center_v, *coordinates,
                    "unrooted_structural_support");
                debug->add_record(
                    BeltSupportDebugStageId::FinalSupport,
                    "structural_support_component",
                    "extends_beyond_causal_preceding_support",
                    {{"plan_index", static_cast<double>(plan_index)},
                     {"component_index",
                      static_cast<double>(component_index)},
                     {"slice_s_mm", plan.slice_s},
                     {"permitted_growth_mm", permitted_growth_mm},
                     {"component_area_mm2",
                      expolygons_area_mm2(component_polygons)},
                     {"unsupported_area_mm2", unsupported_area_mm2},
                     {"has_build_plate_root",
                      overlap_area_mm2(component_polygons,
                                       root_footprint) > 1e-12
                          ? 1.0 : 0.0},
                     {"has_preceding_plane",
                      has_preceding_plane ? 1.0 : 0.0}});
            }
        }

        previous_rooted_structural_region = union_ex(rooted_components);
        previous_structural_plan_index = plan_index;
    }

    m_object.clear_support_layers();
    size_t support_layer_id = 0;
    size_t installed_layer_count = 0;
    size_t first_open_path_count = 0;
    double first_support_s = 0.0;
    bool first_installed = true;
    size_t final_path_count = 0;
    size_t final_path_segment_count = 0;
    size_t final_loop_entity_count = 0;
    size_t final_open_entity_count = 0;
    size_t final_empty_layer_count = 0;
    size_t final_path_footprint_gap_component_count = 0;
    size_t final_path_footprint_component_count = 0;
    size_t final_interface_path_footprint_gap_component_count = 0;
    size_t final_base_path_footprint_gap_component_count = 0;
    size_t first_path_footprint_gap_plan_index = plans.size();
    double maximum_path_footprint_gap_mm = 0.0;
    size_t final_support_island_component_count = 0;
    size_t final_disconnected_support_island_count = 0;
    size_t first_disconnected_support_island_plan_index = plans.size();
    double maximum_disconnected_support_island_gap_mm = 0.0;
    size_t final_nonconsecutive_layer_count = 0;
    double final_path_model_overlap_mm2 = 0.0;
    double maximum_layer_path_model_overlap_mm2 = 0.0;
    size_t significant_path_model_overlap_layer_count = 0;
    constexpr double path_model_overlap_area_tolerance_mm2 =
        SUPPORT_RESOLUTION * SUPPORT_RESOLUTION;
    double final_minimum_world_z = std::numeric_limits<double>::max();
    size_t route_spine_path_count = 0;
    double route_spine_path_length_mm = 0.0;
    size_t rejected_unanchored_base_spine_count = 0;
    size_t rejected_unanchored_interface_spine_count = 0;
    size_t rejected_unanchored_base_entity_count = 0;
    size_t rejected_unanchored_interface_entity_count = 0;
    ExPolygons previous_path_footprint;
    ExPolygons previous_support_envelope;
    size_t previous_plan_index = plans.size();

    // The route centerline is the authoritative identity of a tree branch.
    // Build the extrusion spine from that route on every physical layer. Do
    // not infer a new centerline from the unioned section outline: unions,
    // clipping and parent/child junctions may rotate or move the outline's
    // principal axis even though the actual branch route remains continuous.
    const auto route_spines_for_regions =
        [&](const ExPolygons& regions, size_t plan_index, double path_width) {
            Polylines spines;
            if (regions.empty() || plan_index >= plans.size())
                return spines;

            const double slice_s = plans[plan_index].slice_s;
            const double previous_slice_s = plan_index > 0
                ? plans[plan_index - 1].slice_s
                : slice_s - plans[plan_index].height;
            std::set<std::pair<coord_t, coord_t>> emitted_centers;
            const double extension_mm = std::max(0.05, 0.5 * path_width);

            for (const BeltSupportRoute& route : routes) {
                if (!route.failure_reason.empty() || route.points.size() < 2 ||
                    slice_s < route.points.back().s - EPSILON ||
                    slice_s > route.points.front().s + EPSILON) {
                    continue;
                }

                const Point current = Point::new_scale(
                    route_u_at(route, slice_s), route_v_at(route, slice_s));
                if (!emitted_centers.emplace(current.x(), current.y()).second)
                    continue;

                const double lower_s = std::clamp(
                    previous_slice_s, route.points.back().s, slice_s);
                const Point lower = Point::new_scale(
                    route_u_at(route, lower_s), route_v_at(route, lower_s));
                double direction_u = unscaled<double>(current.x() - lower.x());
                double direction_v = unscaled<double>(current.y() - lower.y());
                const double direction_length = std::hypot(
                    direction_u, direction_v);
                if (direction_length <= EPSILON) {
                    direction_u = 1.0;
                    direction_v = 0.0;
                } else {
                    direction_u /= direction_length;
                    direction_v /= direction_length;
                }

                // Sweep through both the preceding and current route centers.
                // Clipping this short segment to the already validated branch
                // section keeps it inside the support solid while preserving
                // actual layer-to-layer footprint overlap.
                const Point start = Point::new_scale(
                    unscaled<double>(lower.x()) - direction_u * extension_mm,
                    unscaled<double>(lower.y()) - direction_v * extension_mm);
                const Point end = Point::new_scale(
                    unscaled<double>(current.x()) + direction_u * extension_mm,
                    unscaled<double>(current.y()) + direction_v * extension_mm);
                const Polyline candidate(start, end);
                Polylines clipped = intersection_pl(
                    Polylines{candidate}, regions);
                for (Polyline& clipped_path : clipped) {
                    if (!clipped_path.is_valid())
                        continue;
                    route_spine_path_length_mm +=
                        unscaled<double>(clipped_path.length());
                    ++route_spine_path_count;
                    if (debug != nullptr) {
                        for (size_t point_index = 1;
                             point_index < clipped_path.points.size();
                             ++point_index) {
                            debug->add_line(
                                BeltSupportDebugStageId::PathInputs,
                                local_point_to_world(
                                    clipped_path.points[point_index - 1],
                                    plans[plan_index].print_s,
                                    center_u, center_v, *coordinates),
                                local_point_to_world(
                                    clipped_path.points[point_index],
                                    plans[plan_index].print_s,
                                    center_u, center_v, *coordinates),
                                "route_center_spine");
                        }
                    }
                    spines.emplace_back(std::move(clipped_path));
                }
            }
            return spines;
        };

    const auto append_causally_anchored_entities =
        [&](ExtrusionEntitiesPtr& destination,
            ExtrusionEntityCollection& candidates,
            const ExPolygons& current_physical_anchor_footprint) {
            ExtrusionEntitiesPtr rejected;
            rejected.reserve(candidates.entities.size());
            size_t accepted_count = 0;
            size_t rejected_count = 0;
            const ExPolygons physical_anchor_footprint = union_ex(
                previous_path_footprint,
                current_physical_anchor_footprint);

            // Tree path generation may return a no-sort collection containing
            // two independent walls. Acceptance must be decided for each
            // physical extrusion, not for the collection as a whole: one
            // anchored wall must never admit a second floating wall.
            std::function<bool(ExtrusionEntity*)> retain_anchored_leaf_paths;
            retain_anchored_leaf_paths = [&](ExtrusionEntity* candidate) {
                if (candidate == nullptr)
                    return false;
                if (auto* collection =
                        dynamic_cast<ExtrusionEntityCollection*>(candidate)) {
                    ExtrusionEntitiesPtr retained_children;
                    retained_children.reserve(collection->entities.size());
                    for (ExtrusionEntity* child : collection->entities) {
                        if (retain_anchored_leaf_paths(child))
                            retained_children.emplace_back(child);
                        else
                            delete child;
                    }
                    collection->entities = std::move(retained_children);
                    return !collection->entities.empty();
                }

                const ExPolygons candidate_footprint = union_ex(
                    candidate->polygons_covered_by_width());
                const bool anchored = overlap_area_mm2(
                    candidate_footprint,
                    physical_anchor_footprint) > 1e-12;
                if (anchored)
                    ++accepted_count;
                else
                    ++rejected_count;
                return anchored;
            };

            for (ExtrusionEntity* candidate : candidates.entities) {
                if (retain_anchored_leaf_paths(candidate))
                    destination.emplace_back(candidate);
                else if (candidate != nullptr)
                    rejected.emplace_back(candidate);
            }
            candidates.entities = std::move(rejected);
            return std::make_pair(accepted_count, rejected_count);
        };

    for (size_t plan_index = 0; plan_index < plans.size(); ++plan_index) {
        BeltSupportLayerPlan& plan = plans[plan_index];
        if (plan.root_paths.empty() && plan.base_regions.empty() &&
            plan.interface_regions.empty())
            continue;

        SupportLayer* support_layer = m_object.add_tree_support_layer(
            int(support_layer_id++), plan.height, plan.print_s, plan.slice_s);
        const bool first_support_layer = first_installed;
        first_installed = false;
        const Flow base_flow = (first_support_layer ? support_parameters.first_layer_flow
                                                     : support_parameters.support_material_flow)
                                   .with_height(float(plan.height));
        const Flow interface_flow = support_parameters.support_material_interface_flow
                                        .with_height(float(plan.height));

        // The untrimmed root path is the physical intersection of this
        // oriented manufacturing plane with world Z=0. A structural path may
        // start from it even when the duplicate root extrusion is later
        // removed underneath an occupied branch section.
        const ExPolygons build_plate_root_footprint = plan.root_paths.empty()
            ? ExPolygons{}
            : union_ex(offset(
                  plan.root_paths, 0.5f * base_flow.scaled_width()));

        const size_t base_entity_begin =
            support_layer->support_fills.entities.size();
        if (!plan.base_regions.empty()) {
            Polylines structural_spines =
                route_spines_for_regions(
                    plan.base_regions, plan_index, base_flow.width());
            ExtrusionEntityCollection structural_spine_candidates;
            extrusion_entities_append_paths(
                structural_spine_candidates.entities,
                std::move(structural_spines),
                ExtrusionRole::erSupportMaterial, base_flow.mm3_per_mm(),
                base_flow.width(), base_flow.height(), false);
            const auto [accepted_spine_count, rejected_spine_count] =
                append_causally_anchored_entities(
                    support_layer->support_fills.entities,
                    structural_spine_candidates,
                    build_plate_root_footprint);
            (void)accepted_spine_count;
            rejected_unanchored_base_spine_count += rejected_spine_count;

            const ExPolygons route_spine_footprint = entity_footprint(
                support_layer->support_fills.entities,
                base_entity_begin,
                support_layer->support_fills.entities.size());
            ExtrusionEntityCollection base_candidates;
            // Generate the sheath from the first printable section onward so
            // it expands continuously with the tapered branch. The previous
            // narrow/mature threshold made a full perimeter appear suddenly
            // millimetres away from the existing centerline.
            tree_supports_generate_paths(
                base_candidates.entities, to_polygons(plan.base_regions),
                base_flow, support_parameters);
            const auto [accepted_count, rejected_count] =
                append_causally_anchored_entities(
                    support_layer->support_fills.entities,
                    base_candidates,
                    union_ex(route_spine_footprint,
                             build_plate_root_footprint));
            (void)accepted_count;
            rejected_unanchored_base_entity_count += rejected_count;
        }
        const size_t base_entity_end =
            support_layer->support_fills.entities.size();

        const size_t interface_entity_begin = base_entity_end;
        if (!plan.interface_regions.empty()) {
            Polylines interface_spines =
                route_spines_for_regions(
                    plan.interface_regions, plan_index,
                    interface_flow.width());
            ExtrusionEntityCollection interface_spine_candidates;
            extrusion_entities_append_paths(
                interface_spine_candidates.entities,
                std::move(interface_spines),
                ExtrusionRole::erSupportMaterialInterface,
                interface_flow.mm3_per_mm(), interface_flow.width(),
                interface_flow.height(), false);
            const auto [accepted_spine_count, rejected_spine_count] =
                append_causally_anchored_entities(
                    support_layer->support_fills.entities,
                    interface_spine_candidates,
                    build_plate_root_footprint);
            (void)accepted_spine_count;
            rejected_unanchored_interface_spine_count +=
                rejected_spine_count;
            const ExPolygons route_spine_footprint = entity_footprint(
                support_layer->support_fills.entities,
                interface_entity_begin,
                support_layer->support_fills.entities.size());
            ExtrusionEntityCollection interface_candidates;
            append_interface_fill(
                interface_candidates.entities,
                plan.interface_regions, interface_flow,
                support_parameters, installed_layer_count);
            const auto [accepted_count, rejected_count] =
                append_causally_anchored_entities(
                    support_layer->support_fills.entities,
                    interface_candidates,
                    union_ex(route_spine_footprint,
                             build_plate_root_footprint));
            (void)accepted_count;
            rejected_unanchored_interface_entity_count += rejected_count;
        }
        const size_t interface_entity_end =
            support_layer->support_fills.entities.size();

        const size_t root_entity_begin = interface_entity_end;
        if (!plan.root_paths.empty()) {
            const Polygons occupied = union_(to_polygons(plan.base_regions),
                                             to_polygons(plan.interface_regions));
            if (!occupied.empty())
                plan.root_paths = diff_pl(plan.root_paths, occupied);
            extrusion_entities_append_paths(
                support_layer->support_fills.entities, std::move(plan.root_paths),
                ExtrusionRole::erSupportMaterial, base_flow.mm3_per_mm(),
                base_flow.width(), base_flow.height(), false);
        }
        const size_t root_entity_end =
            support_layer->support_fills.entities.size();

        const ExPolygons base_path_footprint = entity_footprint(
            support_layer->support_fills.entities,
            base_entity_begin, base_entity_end);
        const ExPolygons interface_path_footprint = entity_footprint(
            support_layer->support_fills.entities,
            interface_entity_begin, interface_entity_end);
        const ExPolygons root_path_footprint = entity_footprint(
            support_layer->support_fills.entities,
            root_entity_begin, root_entity_end);

        const Polylines printed_paths = support_layer->support_fills.as_polylines();

        Polygons path_coverage_polygons;
        for (const ExtrusionEntity* entity : support_layer->support_fills.entities) {
            if (entity == nullptr)
                continue;
            if (entity->is_loop())
                ++final_loop_entity_count;
            else
                ++final_open_entity_count;
            Polygons covered = entity->polygons_covered_by_width();
            append(path_coverage_polygons, std::move(covered));
        }
        const ExPolygons path_footprint = union_ex(path_coverage_polygons);
        support_layer->support_islands = path_footprint;
        const ExPolygons& support_envelope = path_footprint;
        const double layer_model_overlap_mm2 = overlap_area_mm2(
            path_footprint, model_solid_by_plan[plan_index].regions);
        final_path_model_overlap_mm2 += layer_model_overlap_mm2;
        maximum_layer_path_model_overlap_mm2 = std::max(
            maximum_layer_path_model_overlap_mm2,
            layer_model_overlap_mm2);
        if (layer_model_overlap_mm2 >
            path_model_overlap_area_tolerance_mm2 + EPSILON) {
            ++significant_path_model_overlap_layer_count;
        }
        size_t path_footprint_gap_component_count = 0;
        const bool previous_layer_available = first_support_layer ||
            previous_plan_index + 1 == plan_index;
        if (!previous_layer_available)
            ++final_nonconsecutive_layer_count;
        final_path_footprint_component_count += path_footprint.size();
        for (size_t component_index = 0;
             component_index < path_footprint.size(); ++component_index) {
            const ExPolygon& component = path_footprint[component_index];
            const ExPolygons component_polygons{component};
            const double previous_overlap_mm2 = first_support_layer
                ? 0.0
                : overlap_area_mm2(component_polygons,
                                   previous_path_footprint);
            PolygonGap gap;
            if (!first_support_layer && previous_layer_available &&
                previous_overlap_mm2 <= 1e-12)
                gap = boundary_gap(component, previous_path_footprint);
            constexpr double path_contact_tolerance_mm = 0.001;
            const bool path_overlaps_previous_footprint =
                !first_support_layer && previous_layer_available &&
                 (previous_overlap_mm2 > 1e-12 ||
                  (std::isfinite(gap.distance_mm) &&
                   gap.distance_mm <=
                       path_contact_tolerance_mm + EPSILON));
            const double base_overlap_mm2 = overlap_area_mm2(
                component_polygons, base_path_footprint);
            const double interface_overlap_mm2 = overlap_area_mm2(
                component_polygons, interface_path_footprint);
            const double root_overlap_mm2 = overlap_area_mm2(
                component_polygons, root_path_footprint);
            const bool root_source = root_overlap_mm2 > 1e-12 &&
                root_overlap_mm2 >= base_overlap_mm2 &&
                root_overlap_mm2 >= interface_overlap_mm2;
            const bool interface_source = !root_source &&
                interface_overlap_mm2 > base_overlap_mm2;
            const Point center = safe_centroid(component);
            const Vec3d center_world = local_point_to_world(
                center, plan.print_s, center_u, center_v, *coordinates);
            const bool supported_by_build_plate =
                overlap_area_mm2(component_polygons,
                                 build_plate_root_footprint) > 1e-12;
            const bool has_direct_path_footprint_support =
                path_overlaps_previous_footprint || supported_by_build_plate;
            if (!has_direct_path_footprint_support) {
                ++path_footprint_gap_component_count;
                first_path_footprint_gap_plan_index = std::min(
                    first_path_footprint_gap_plan_index, plan_index);
                if (std::isfinite(gap.distance_mm))
                    maximum_path_footprint_gap_mm = std::max(
                        maximum_path_footprint_gap_mm,
                        gap.distance_mm);
                if (interface_source)
                    ++final_interface_path_footprint_gap_component_count;
                else
                    ++final_base_path_footprint_gap_component_count;
                if (debug != nullptr) {
                    debug_polygon(
                        debug, BeltSupportDebugStageId::FinalSupport,
                        component.contour, plan.print_s,
                        center_u, center_v, *coordinates,
                        "final_path_footprint_gap_component");
                    for (const Polygon& hole : component.holes) {
                        debug_polygon(
                            debug, BeltSupportDebugStageId::FinalSupport,
                            hole, plan.print_s, center_u, center_v,
                            *coordinates,
                            "final_path_footprint_gap_component");
                    }
                    if (std::isfinite(gap.distance_mm)) {
                        debug->add_line(
                            BeltSupportDebugStageId::FinalSupport,
                            local_point_to_world(
                                gap.current, plan.print_s,
                                center_u, center_v, *coordinates),
                            local_point_to_world(
                                gap.previous,
                                plans[previous_plan_index].print_s,
                                center_u, center_v, *coordinates),
                            "final_path_footprint_gap");
                    }
                }
            }
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::FinalSupport,
                    "final_path_footprint_component",
                    supported_by_build_plate
                        ? "world_build_plate_root_component"
                        : (path_overlaps_previous_footprint
                            ? (interface_source
                                ? "interface_path_overlaps_previous_footprint"
                                : "base_path_overlaps_previous_footprint")
                            : (interface_source
                                ? "interface_path_has_previous_footprint_gap"
                                : "base_path_has_previous_footprint_gap")),
                    {{"plan_index", static_cast<double>(plan_index)},
                     {"support_layer_index",
                      static_cast<double>(installed_layer_count)},
                     {"slice_s_mm", plan.slice_s},
                     {"component_index",
                      static_cast<double>(component_index)},
                     {"component_area_mm2",
                      expolygons_area_mm2(component_polygons)},
                     {"previous_layer_overlap_mm2",
                      previous_overlap_mm2},
                     {"previous_layer_gap_mm",
                      std::isfinite(gap.distance_mm)
                          ? gap.distance_mm : -1.0},
                     {"base_region_overlap_mm2", base_overlap_mm2},
                     {"interface_region_overlap_mm2",
                      interface_overlap_mm2},
                     {"root_path_overlap_mm2", root_overlap_mm2},
                     {"supported_by_world_build_plate",
                      supported_by_build_plate ? 1.0 : 0.0},
                     {"center_local_u_mm",
                      unscaled<double>(center.x())},
                     {"center_local_v_mm",
                      unscaled<double>(center.y())},
                     {"center_world_x_mm", center_world.x()},
                     {"center_world_y_mm", center_world.y()},
                     {"center_world_z_mm", center_world.z()}});
            }
        }
        final_path_footprint_gap_component_count +=
            path_footprint_gap_component_count;

        final_support_island_component_count += support_envelope.size();
        size_t disconnected_support_island_count = 0;
        for (size_t component_index = 0;
             component_index < support_envelope.size(); ++component_index) {
            const ExPolygon& component = support_envelope[component_index];
            const ExPolygons component_polygons{component};
            const double previous_envelope_overlap_mm2 = first_support_layer
                ? 0.0
                : overlap_area_mm2(
                    component_polygons, previous_support_envelope);
            const double root_overlap_mm2 = overlap_area_mm2(
                component_polygons, root_path_footprint);
            const double base_region_overlap_mm2 = overlap_area_mm2(
                component_polygons, plan.base_regions);
            const double interface_region_overlap_mm2 = overlap_area_mm2(
                component_polygons, plan.interface_regions);
            const double base_path_overlap_mm2 = overlap_area_mm2(
                component_polygons, base_path_footprint);
            const double interface_path_overlap_mm2 = overlap_area_mm2(
                component_polygons, interface_path_footprint);
            PolygonGap gap;
            if (previous_layer_available &&
                previous_envelope_overlap_mm2 <= 1e-12) {
                gap = boundary_gap(component, previous_support_envelope);
            }
            constexpr double structural_contact_tolerance_mm = 0.001;
            const bool previous_envelope_coordinate_touch =
                previous_layer_available &&
                std::isfinite(gap.distance_mm) &&
                gap.distance_mm <= structural_contact_tolerance_mm + EPSILON;
            double minimum_component_world_z =
                std::numeric_limits<double>::max();
            const auto accumulate_minimum_world_z =
                [&](const Polygon& polygon) {
                    for (const Point& point : polygon.points) {
                        minimum_component_world_z = std::min(
                            minimum_component_world_z,
                            local_point_to_world(
                                point, plan.print_s,
                                center_u, center_v, *coordinates).z());
                    }
                };
            accumulate_minimum_world_z(component.contour);
            for (const Polygon& hole : component.holes)
                accumulate_minimum_world_z(hole);
            const bool touches_world_build_plate =
                minimum_component_world_z <= 1e-5;
            const bool supported_by_build_plate =
                root_overlap_mm2 > 1e-12 || touches_world_build_plate;
            const bool supported_by_previous_envelope =
                previous_layer_available &&
                (previous_envelope_overlap_mm2 > 1e-12 ||
                 previous_envelope_coordinate_touch);
            const bool structurally_connected =
                supported_by_build_plate || supported_by_previous_envelope;
            const Point center = safe_centroid(component);
            const Vec3d center_world = local_point_to_world(
                center, plan.print_s, center_u, center_v, *coordinates);

            if (!structurally_connected) {
                ++disconnected_support_island_count;
                ++final_disconnected_support_island_count;
                first_disconnected_support_island_plan_index = std::min(
                    first_disconnected_support_island_plan_index,
                    plan_index);
                if (std::isfinite(gap.distance_mm)) {
                    maximum_disconnected_support_island_gap_mm = std::max(
                        maximum_disconnected_support_island_gap_mm,
                        gap.distance_mm);
                }
                if (debug != nullptr) {
                    debug_polygon(
                        debug, BeltSupportDebugStageId::FinalSupport,
                        component.contour, plan.print_s,
                        center_u, center_v, *coordinates,
                        "final_disconnected_support_island");
                    for (const Polygon& hole : component.holes) {
                        debug_polygon(
                            debug, BeltSupportDebugStageId::FinalSupport,
                            hole, plan.print_s, center_u, center_v,
                            *coordinates,
                            "final_disconnected_support_island");
                    }
                    if (std::isfinite(gap.distance_mm)) {
                        debug->add_line(
                            BeltSupportDebugStageId::FinalSupport,
                            local_point_to_world(
                                gap.current, plan.print_s,
                                center_u, center_v, *coordinates),
                            local_point_to_world(
                                gap.previous,
                                plans[previous_plan_index].print_s,
                                center_u, center_v, *coordinates),
                            "final_disconnected_support_island_gap");
                    }
                }
            }
            if (debug != nullptr) {
                debug->add_record(
                    BeltSupportDebugStageId::FinalSupport,
                    "final_support_island",
                    supported_by_build_plate
                        ? "connected_to_world_build_plate"
                        : (supported_by_previous_envelope
                            ? (previous_envelope_overlap_mm2 > 1e-12
                                ? "connected_to_previous_support_envelope"
                                : "connected_to_previous_support_envelope_with_coordinate_tolerance")
                            : "disconnected_from_printable_support"),
                    {{"plan_index", static_cast<double>(plan_index)},
                     {"support_layer_index",
                      static_cast<double>(installed_layer_count)},
                     {"slice_s_mm", plan.slice_s},
                     {"component_index",
                      static_cast<double>(component_index)},
                     {"component_area_mm2",
                      expolygons_area_mm2(component_polygons)},
                     {"previous_envelope_overlap_mm2",
                      previous_envelope_overlap_mm2},
                     {"previous_envelope_gap_mm",
                      std::isfinite(gap.distance_mm)
                          ? gap.distance_mm : -1.0},
                     {"root_path_overlap_mm2", root_overlap_mm2},
                     {"base_region_overlap_mm2",
                      base_region_overlap_mm2},
                     {"interface_region_overlap_mm2",
                      interface_region_overlap_mm2},
                     {"base_path_overlap_mm2",
                      base_path_overlap_mm2},
                     {"interface_path_overlap_mm2",
                      interface_path_overlap_mm2},
                     {"minimum_component_world_z_mm",
                      minimum_component_world_z ==
                              std::numeric_limits<double>::max()
                          ? 0.0
                          : minimum_component_world_z},
                     {"structural_contact_tolerance_mm",
                      structural_contact_tolerance_mm},
                     {"supported_by_world_build_plate",
                      supported_by_build_plate ? 1.0 : 0.0},
                     {"center_world_x_mm", center_world.x()},
                     {"center_world_y_mm", center_world.y()},
                     {"center_world_z_mm", center_world.z()}});
            }
        }
        if (printed_paths.empty())
            ++final_empty_layer_count;
        final_path_count += printed_paths.size();
        double layer_path_length_mm = 0.0;
        for (const Polyline& path : printed_paths) {
            layer_path_length_mm += unscaled<double>(path.length());
            if (path.points.size() > 1)
                final_path_segment_count += path.points.size() - 1;
            for (size_t point_index = 0;
                 point_index < path.points.size(); ++point_index) {
                const Vec3d world = local_point_to_world(
                    path.points[point_index], plan.print_s,
                    center_u, center_v, *coordinates);
                final_minimum_world_z = std::min(
                    final_minimum_world_z, world.z());
                if (debug != nullptr && point_index > 0) {
                    const Vec3d previous_world = local_point_to_world(
                        path.points[point_index - 1], plan.print_s,
                        center_u, center_v, *coordinates);
                    debug->add_line(
                        BeltSupportDebugStageId::FinalSupport,
                        previous_world, world,
                        first_support_layer
                            ? "final_root_open_path"
                            : "final_support_extrusion_path");
                }
            }
        }
        if (debug != nullptr) {
            debug->add_record(
                BeltSupportDebugStageId::FinalSupport,
                "final_support_layer",
                printed_paths.empty()
                    ? "no_extrusion_path"
                    : (disconnected_support_island_count > 0
                        ? "contains_disconnected_support_island"
                        : (layer_model_overlap_mm2 >
                               path_model_overlap_area_tolerance_mm2 + EPSILON
                            ? "path_footprint_overlaps_model"
                            : "printable_layer_paths")),
                {{"plan_index", static_cast<double>(plan_index)},
                 {"support_layer_index",
                  static_cast<double>(installed_layer_count)},
                 {"slice_s_mm", plan.slice_s},
                 {"print_s_mm", plan.print_s},
                 {"height_mm", plan.height},
                 {"entity_count", static_cast<double>(
                      support_layer->support_fills.entities.size())},
                 {"path_count", static_cast<double>(printed_paths.size())},
                 {"path_length_mm", layer_path_length_mm},
                 {"path_footprint_component_count",
                  static_cast<double>(path_footprint.size())},
                 {"path_footprint_gap_component_count",
                  static_cast<double>(
                      path_footprint_gap_component_count)},
                 {"support_island_component_count",
                  static_cast<double>(support_envelope.size())},
                 {"disconnected_support_island_count",
                  static_cast<double>(
                      disconnected_support_island_count)},
                 {"model_overlap_mm2", layer_model_overlap_mm2}});
        }
        previous_path_footprint = path_footprint;
        previous_support_envelope = support_envelope;
        previous_plan_index = plan_index;

        if (first_support_layer) {
            first_support_s = plan.slice_s;
            first_open_path_count = count_open_paths(support_layer->support_fills);
            if (support_layer->support_fills.entities.size() != 1 ||
                first_open_path_count != 1 ||
                support_layer->support_fills.entities.front()->is_loop()) {
                throw SlicingError("Belt tapered support first layer is not exactly one open extrusion path.");
            }
        }
        ++installed_layer_count;
    }

    const bool final_support_contract_valid =
        installed_layer_count > 0 &&
        first_open_path_count == 1 &&
        final_empty_layer_count == 0 &&
        final_unrooted_structural_component_count == 0 &&
        final_nonconsecutive_layer_count == 0 &&
        significant_path_model_overlap_layer_count == 0 &&
        final_minimum_world_z >= -1e-5 &&
        required_uncovered_raw_witness_count == 0 &&
        final_path_footprint_gap_component_count == 0 &&
        final_disconnected_support_island_count == 0;
    if (debug != nullptr) {
        debug->set_metric(BeltSupportDebugStageId::PathInputs,
                          "final_support_generated", 1.0);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "route_center_spine_path_count",
            static_cast<double>(route_spine_path_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "route_center_spine_path_length_mm",
            route_spine_path_length_mm);
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "rejected_unanchored_base_spine_count",
            static_cast<double>(rejected_unanchored_base_spine_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "rejected_unanchored_interface_spine_count",
            static_cast<double>(
                rejected_unanchored_interface_spine_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "rejected_unanchored_base_entity_count",
            static_cast<double>(rejected_unanchored_base_entity_count));
        debug->set_metric(
            BeltSupportDebugStageId::PathInputs,
            "rejected_unanchored_interface_entity_count",
            static_cast<double>(rejected_unanchored_interface_entity_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_support_generated", 1.0);
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_support_contract_valid",
                          final_support_contract_valid ? 1.0 : 0.0);
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "installed_support_layer_count",
                          static_cast<double>(installed_layer_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_path_count",
                          static_cast<double>(final_path_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_path_segment_count",
                          static_cast<double>(final_path_segment_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_loop_entity_count",
                          static_cast<double>(final_loop_entity_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_open_entity_count",
                          static_cast<double>(final_open_entity_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_empty_layer_count",
                          static_cast<double>(final_empty_layer_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_unrooted_structural_component_count",
            static_cast<double>(
                final_unrooted_structural_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "first_unrooted_structural_plan_index",
            first_unrooted_structural_plan_index == plans.size()
                ? -1.0
                : static_cast<double>(
                      first_unrooted_structural_plan_index));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_unrooted_structural_area_mm2",
            final_unrooted_structural_area_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_path_footprint_gap_component_count",
            static_cast<double>(
                final_path_footprint_gap_component_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_path_footprint_component_count",
                          static_cast<double>(
                              final_path_footprint_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_interface_path_footprint_gap_component_count",
            static_cast<double>(
                final_interface_path_footprint_gap_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_base_path_footprint_gap_component_count",
            static_cast<double>(
                final_base_path_footprint_gap_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "first_path_footprint_gap_plan_index",
            first_path_footprint_gap_plan_index == plans.size()
                ? -1.0
                : static_cast<double>(
                      first_path_footprint_gap_plan_index));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "maximum_path_footprint_gap_mm",
            maximum_path_footprint_gap_mm);
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_support_island_component_count",
            static_cast<double>(final_support_island_component_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "final_disconnected_support_island_count",
            static_cast<double>(
                final_disconnected_support_island_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "first_disconnected_support_island_plan_index",
            first_disconnected_support_island_plan_index == plans.size()
                ? -1.0
                : static_cast<double>(
                      first_disconnected_support_island_plan_index));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "maximum_disconnected_support_island_gap_mm",
            maximum_disconnected_support_island_gap_mm);
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_nonconsecutive_layer_count",
                          static_cast<double>(
                              final_nonconsecutive_layer_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_path_model_overlap_mm2",
                          final_path_model_overlap_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "maximum_layer_path_model_overlap_mm2",
            maximum_layer_path_model_overlap_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "path_model_overlap_area_tolerance_mm2",
            path_model_overlap_area_tolerance_mm2);
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "significant_path_model_overlap_layer_count",
            static_cast<double>(
                significant_path_model_overlap_layer_count));
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "final_minimum_world_z_mm",
                          final_minimum_world_z ==
                                  std::numeric_limits<double>::max()
                              ? 0.0 : final_minimum_world_z);
        debug->set_metric(BeltSupportDebugStageId::FinalSupport,
                          "uncovered_raw_witness_count",
                          static_cast<double>(
                              uncovered_raw_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "required_uncovered_raw_witness_count",
            static_cast<double>(required_uncovered_raw_witness_count));
        debug->set_metric(
            BeltSupportDebugStageId::FinalSupport,
            "build_plate_policy_excluded_raw_witness_count",
            static_cast<double>(
                build_plate_policy_excluded_raw_witness_count));
    }

    if (!final_support_contract_valid && debug == nullptr) {
        BOOST_LOG_TRIVIAL(error)
            << "BeltTaperedSupport: invalid final contract"
            << " unrooted_structural_components="
            << final_unrooted_structural_component_count
            << " unrooted_structural_area_mm2="
            << final_unrooted_structural_area_mm2
            << " first_unrooted_plan="
            << (first_unrooted_structural_plan_index == plans.size()
                    ? -1
                    : static_cast<long long>(
                          first_unrooted_structural_plan_index))
            << " nonconsecutive_layers="
            << final_nonconsecutive_layer_count
            << " empty_layers=" << final_empty_layer_count
            << " model_overlap_mm2=" << final_path_model_overlap_mm2
            << " minimum_world_z=" << final_minimum_world_z
            << " required_uncovered_witnesses="
            << required_uncovered_raw_witness_count;
        throw SlicingError(
            "Belt support contains a structural section that cannot be traced causally to the world build plate.");
    }

    BOOST_LOG_TRIVIAL(info)
        << "BeltTaperedSupport: reachable_contacts=" << reachable_contact_count
        << " unreachable_contacts=" << unreachable_contact_count
        << " route_segments=" << contacts.size()
        << " support_layers=" << installed_layer_count
        << " first_support_s=" << first_support_s
        << " first_open_paths=" << first_open_path_count;
}

} // namespace Slic3r
