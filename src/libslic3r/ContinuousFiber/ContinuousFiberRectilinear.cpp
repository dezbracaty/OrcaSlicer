#include "ContinuousFiberFillStrategy.hpp"
#include "../AABBTreeLines.hpp"
#include "../ClipperUtils.hpp"
#include "../Geometry/ArcWelder.hpp"

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>

namespace Slic3r {
namespace {
constexpr double chord_error_mm = 0.00005;
const double coordinate_error_mm = 4 * SCALING_FACTOR;
Vec2d mm(const Point& p) { return p.cast<double>() * SCALING_FACTOR; }
Point scaled(const Vec2d& p) { return Point::new_scale(p.x(), p.y()); }

struct Interval { double lo, hi; };
struct Scan { double lo, hi; bool used = false; };
struct Row { double y; std::vector<Scan> scans; };
struct ReturnShape {
    double radius, pitch;
    std::vector<Vec2d> samples;
    std::array<std::pair<size_t, size_t>, 2> arc_indices;
};

ReturnShape make_return(double radius, double pitch)
{
    ReturnShape shape{radius, pitch, {}, {}};
    const size_t steps = Geometry::ArcWelder::arc_discretization_steps(radius, PI / 2, chord_error_mm);
    if (steps > 200000) throw std::length_error("Fiber return exceeds arc sampling capacity");
    for (size_t k = 0; k < 2; ++k) {
        const Vec2d center(0, k == 0 ? radius : pitch - radius);
        const double begin = k == 0 ? -PI / 2 : 0;
        const size_t first = shape.samples.size();
        for (size_t i = 0; i <= steps; ++i) {
            const double angle = begin + (PI / 2) * double(i) / steps;
            shape.samples.push_back(center + radius * Vec2d(std::cos(angle), std::sin(angle)));
        }
        shape.arc_indices[k] = {first, shape.samples.size() - 1};
    }
    // Exact tangent coordinates avoid trigonometric residue at the supports.
    shape.samples.front() = {0, 0};
    shape.samples[shape.arc_indices[0].second] = {radius, radius};
    shape.samples[shape.arc_indices[1].first] = {radius, pitch - radius};
    shape.samples.back() = {0, pitch};
    return shape;
}

// At a fixed transverse coordinate y, contact occurs at
// t = boundary_x(y) - return_x(y). Each edge/quarter-circle pair produces one
// continuous interval of forbidden translations. Its extrema are endpoints
// or a tangent contact. This is a one-dimensional collision calculation, not
// a search that assumes the entire concave domain is monotone.
void contact_intervals(const Linef& edge, const ReturnShape& shape, std::vector<Interval>& out)
{
    const double edge_lo = std::min(edge.a.y(), edge.b.y());
    const double edge_hi = std::max(edge.a.y(), edge.b.y());
    if (edge_hi <= 0 || edge_lo >= shape.pitch) return; // Endpoints are constrained by their scan intervals.
    const auto project = [&](double y0, double y1, const auto& x_at, std::optional<double> stationary) {
        const double lo = std::max(y0, edge_lo), hi = std::min(y1, edge_hi);
        if (hi < lo) return;
        if (edge_hi == edge_lo) {
            const double x = x_at(lo);
            out.push_back({std::min(edge.a.x(), edge.b.x()) - x, std::max(edge.a.x(), edge.b.x()) - x});
            return;
        }
        const auto t_at = [&](double y) {
            return edge.a.x() + (edge.b.x() - edge.a.x()) * ((y - edge.a.y()) / (edge.b.y() - edge.a.y())) - x_at(y);
        };
        const double a = t_at(lo), b = t_at(hi);
        Interval interval{std::min(a, b), std::max(a, b)};
        if (stationary && *stationary > lo && *stationary < hi) {
            const double t = t_at(*stationary);
            interval.lo = std::min(interval.lo, t); interval.hi = std::max(interval.hi, t);
        }
        out.push_back(interval);
    };
    for (size_t k = 0; k < 2; ++k) {
        const double center_y = k == 0 ? shape.radius : shape.pitch - shape.radius;
        const double y0 = k == 0 ? 0 : center_y, y1 = k == 0 ? center_y : shape.pitch;
        const auto x_at = [&](double y) {
            const double dy = y - center_y;
            return std::sqrt(std::max(0.0, shape.radius * shape.radius - dy * dy));
        };
        std::optional<double> stationary;
        if (edge_hi != edge_lo) {
            const double slope = (edge.b.x() - edge.a.x()) / (edge.b.y() - edge.a.y());
            stationary = center_y - shape.radius * slope / std::hypot(1.0, slope);
        }
        project(y0, y1, x_at, stationary);
    }
    if (shape.pitch > 2 * shape.radius)
        project(shape.radius, shape.pitch - shape.radius, [&](double) { return shape.radius; }, std::nullopt);
    // Command geometry is the bounded-error polyline. Check its contacts too:
    // a chord around a hole must not cut through that hole even when the arc fits.
    for (size_t i = 1; i < shape.samples.size(); ++i) {
        const Vec2d& a = shape.samples[i - 1]; const Vec2d& b = shape.samples[i];
        if (b.y() <= a.y() || edge_hi < a.y() || edge_lo > b.y()) continue;
        project(a.y(), b.y(), [&](double y) { return a.x() + (b.x() - a.x()) * ((y - a.y()) / (b.y() - a.y())); }, std::nullopt);
    }
}

Polyline translated_return(const ReturnShape& shape, double t, double row, int direction)
{
    Polyline line;
    for (const Vec2d& p : shape.samples) {
        const Point next = scaled({direction * (t + p.x()), row + p.y()});
        if (line.points.empty() || line.points.back() != next) line.points.push_back(next);
    }
    return line;
}

std::optional<double> place_return(const ReturnShape& shape, const ExPolygons& domain,
    const Linesf& edges, const AABBTreeIndirect::Tree<2, double>& tree,
    double row, int direction, double lo, double hi)
{
    if (hi < lo || domain.empty()) return std::nullopt;
    std::vector<Interval> blocked;
    const Vec2d a(std::min(direction * lo, direction * hi) - shape.radius, row);
    const Vec2d b(std::max(direction * lo, direction * hi) + shape.radius, row + shape.pitch);
    const Eigen::AlignedBox<double, 2> box(a - Vec2d::Constant(coordinate_error_mm), b + Vec2d::Constant(coordinate_error_mm));
    AABBTreeIndirect::traverse(tree, AABBTreeIndirect::intersecting(box), [&](const auto& node) {
        const auto& edge = edges[node.idx];
        contact_intervals(Linef({direction * edge.a.x(), edge.a.y() - row},
                               {direction * edge.b.x(), edge.b.y() - row}), shape, blocked);
        return true;
    });
    std::sort(blocked.begin(), blocked.end(), [](const auto& a, const auto& b) {
        return a.lo != b.lo ? a.lo < b.lo : a.hi < b.hi;
    });
    std::vector<Interval> merged;
    for (const auto& interval : blocked) {
        if (interval.hi < lo || interval.lo > hi) continue;
        if (!merged.empty() && interval.lo < merged.back().hi)
            merged.back().hi = std::max(merged.back().hi, interval.hi);
        else merged.push_back(interval);
    }
    const auto fits = [&](double t) {
        return diff_pl(Polylines{translated_return(shape, t, row, direction)}, domain).empty();
    };
    const auto try_interval = [&](double begin, double end) -> std::optional<double> {
        if (end < begin) return std::nullopt;
        if (fits(end)) return end;
        // Integer rounding at contact is bounded in the same coordinates used
        // by the collision calculation. No manufacturing clearance is added.
        const double inset = std::min(coordinate_error_mm, (end - begin) / 2);
        if (inset > 0 && fits(end - inset)) return end - inset;
        return std::nullopt;
    };
    double end = hi;
    for (auto i = merged.rbegin(); i != merged.rend(); ++i) {
        if (auto found = try_interval(std::max(lo, i->hi), end)) return found;
        end = std::min(end, i->lo);
    }
    return try_interval(lo, end);
}

Linesf boundary_lines(const ExPolygons& domain)
{
    Linesf result;
    for (const auto& p : to_polygons(domain))
        for (const auto& line : p.lines()) result.emplace_back(mm(line.a), mm(line.b));
    return result;
}

std::vector<Row> scan_rows(const ExPolygons& domain, coord_t origin, coord_t pitch, bool full)
{
    const auto box = get_extents(domain);
    if (empty(box)) return {};
    coord_t first = origin + coord_t(std::floor(double(box.min.y() - origin) / pitch)) * pitch;
    if (full) first += (pitch + coord_t(SCALED_EPSILON)) / 2; // Same grid phase as FillRectilinear.
    if (first > box.max.y()) return {};
    const size_t count = size_t((box.max.y() - first) / pitch) + 1;
    if (count > 1000000) throw std::length_error("Fiber infill exceeds scanline capacity");
    Polylines scans;
    std::vector<Row> rows(count);
    for (size_t i = 0; i < count; ++i) {
        const coord_t y = first + coord_t(i) * pitch;
        rows[i].y = unscale<double>(y);
        scans.emplace_back(Points{{box.min.x() - 1, y}, {box.max.x() + 1, y}});
    }
    for (const auto& line : intersection_pl(scans, domain)) {
        const Point& a = line.points.front(); const Point& b = line.points.back();
        if (a == b) continue;
        rows.at(size_t((a.y() - first) / pitch)).scans.push_back(
            {unscale<double>(std::min(a.x(), b.x())), unscale<double>(std::max(a.x(), b.x()))});
    }
    for (auto& row : rows)
        std::sort(row.scans.begin(), row.scans.end(), [](const auto& a, const auto& b) { return a.lo < b.lo; });
    return rows;
}

using BoundaryKey = std::tuple<size_t, size_t, size_t, size_t, int>;
using BoundaryReturns = std::map<BoundaryKey, Polylines>;

// A zero-radius turn follows the same boundary that clipped the scanlines.
// Consecutive endpoints on a ring delimit an unused boundary arc: connecting
// them cannot jump across a hole or pass another scan endpoint.
BoundaryReturns boundary_returns(const ExPolygons& domain, const std::vector<Row>& rows,
    const Linesf& edges, const AABBTreeIndirect::Tree<2, double>& tree)
{
    const Polygons rings = to_polygons(domain);
    std::vector<std::pair<size_t, size_t>> edge_locations;
    for (size_t ring = 0; ring < rings.size(); ++ring)
        for (size_t edge = 0; edge < rings[ring].points.size(); ++edge)
            edge_locations.emplace_back(ring, edge);
    struct End { size_t row, scan; int side; double position; Point point; };
    std::vector<std::vector<End>> ends(rings.size());
    for (size_t row = 0; row < rows.size(); ++row)
        for (size_t scan = 0; scan < rows[row].scans.size(); ++scan)
            for (int side : {-1, 1}) {
                const auto& interval = rows[row].scans[scan];
                const Vec2d point(side < 0 ? interval.lo : interval.hi, rows[row].y);
                size_t edge = 0;
                Vec2d nearest;
                const double distance = AABBTreeLines::squared_distance_to_indexed_lines(
                    edges, tree, point, edge, nearest, coordinate_error_mm * coordinate_error_mm);
                if (distance < 0)
                    throw std::runtime_error("Fiber scan endpoint is not on its centerline boundary");
                const auto [ring, segment] = edge_locations[edge];
                const auto delta = edges[edge].b - edges[edge].a;
                const double t = delta.squaredNorm() == 0 ? 0 :
                    std::clamp((nearest - edges[edge].a).dot(delta) / delta.squaredNorm(), 0.0, 1.0);
                double position = double(segment) + t;
                if (position == rings[ring].points.size()) position = 0;
                ends[ring].push_back({row, scan, side, position, scaled(point)});
            }
    BoundaryReturns result;
    for (size_t ring = 0; ring < rings.size(); ++ring) {
        auto& points = ends[ring];
        std::sort(points.begin(), points.end(), [](const End& a, const End& b) {
            return std::tie(a.position, a.row, a.scan, a.side) < std::tie(b.position, b.row, b.scan, b.side);
        });
        const auto& boundary = rings[ring].points;
        for (size_t i = 0; i < points.size(); ++i) {
            const auto& a = points[i];
            const auto& b = points[(i + 1) % points.size()];
            if (a.side != b.side || (a.row + 1 != b.row && b.row + 1 != a.row)) continue;
            double end = b.position;
            if (end <= a.position) end += boundary.size();
            Polyline path(Points{a.point});
            const coord_t lo = std::min(a.point.y(), b.point.y()), hi = std::max(a.point.y(), b.point.y());
            bool inside_strip = true;
            for (size_t vertex = size_t(std::floor(a.position)) + 1; double(vertex) < end; ++vertex) {
                const Point& point = boundary[vertex % boundary.size()];
                if (point.y() < lo || point.y() > hi) { inside_strip = false; break; }
                if (path.points.back() != point) path.points.push_back(point);
            }
            if (!inside_strip) continue;
            if (path.points.back() != b.point) path.points.push_back(b.point);
            result[{a.row, a.scan, b.row, b.scan, a.side}].push_back(path);
            path.reverse();
            result[{b.row, b.scan, a.row, a.scan, a.side}].push_back(std::move(path));
        }
    }
    return result;
}
} // namespace

FiberInfillCandidates ContinuousFiberFillStrategy::generate_rectilinear(
    const ExPolygons& material, const ContinuousFiberConfig& config,
    double rotation_radians, const Point& grid_origin)
{
    const double radius = config.infill_bend_radius_mm, width = config.infill_flow.width();
    if (!std::isfinite(radius) || radius < 0 || !std::isfinite(width) || width <= 0 ||
        !std::isfinite(config.infill_density) || config.infill_density <= 0 || config.infill_density > 100 ||
        !std::isfinite(rotation_radians)) throw std::invalid_argument("Invalid rectilinear fiber infill parameters");
    const double requested_pitch = width * 100 / config.infill_density;
    if (requested_pitch > unscale<double>(std::numeric_limits<coord_t>::max()) / 4)
        throw std::invalid_argument("Fiber scanline spacing is outside the supported coordinate range");
    const coord_t pitch_scaled = scale_(requested_pitch);
    const double pitch = unscale<double>(pitch_scaled);
    if (pitch_scaled <= 0 || pitch < 2 * radius)
        throw std::invalid_argument("fiber_infill_bend_radius requires scanline spacing >= twice the configured radius");
    FiberInfillCandidates result;
    result.centerline_domain = fiber_material_offset(material, -.5 * width);
    if (result.centerline_domain.empty()) return result;
    // Columns are the scan and transverse axes. This frame has determinant -1.
    Eigen::Matrix2d frame;
    frame << -std::sin(rotation_radians), std::cos(rotation_radians),
              std::cos(rotation_radians), std::sin(rotation_radians);
    ExPolygons local = result.centerline_domain;
    for (auto& region : local) {
        const auto transform = [&](Polygon& ring) {
            for (auto& p : ring.points) p = scaled(frame.transpose() * mm(p));
            ring.reverse();
        };
        transform(region.contour); for (auto& hole : region.holes) transform(hole);
    }
    auto rows = scan_rows(local, scaled(frame.transpose() * mm(grid_origin)).y(), pitch_scaled, config.infill_density == 100);
    const auto edges = boundary_lines(local);
    const auto tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    const auto shape = radius > 0 ? std::optional<ReturnShape>(make_return(radius, pitch)) : std::nullopt;
    const auto boundary_links = radius == 0 ? boundary_returns(local, rows, edges, tree) : BoundaryReturns{};
    // Each row interval is visited once. Turns in different strips are separated
    // by the scan grid; independent turns sharing a strip also reserve width.
    struct Strip {
        Polylines pending;
        ExPolygons occupied, domain;
        Linesf edges;
        AABBTreeIndirect::Tree<2, double> tree;
    };
    std::vector<std::unique_ptr<Strip>> strips(rows.size());
    for (size_t first = 0; first < rows.size(); ++first) for (size_t seed = 0; seed < rows[first].scans.size(); ++seed) {
        if (rows[first].scans[seed].used) continue;
        struct Visit { size_t row, scan; };
        struct Link { double position; int direction; Polyline boundary; };
        std::vector<Visit> visits{{first, seed}};
        std::vector<Link> links;
        rows[first].scans[seed].used = true;
        // Grow both open ends before emitting geometry. Otherwise a short
        // forward chain can strand a feasible connection at its starting end.
        const auto grow = [&](int direction) {
            for (;;) {
                const auto visit = visits.back();
                const auto& current = rows[visit.row].scans[visit.scan];
                const double entry = links.empty() ? (direction > 0 ? current.lo : current.hi) :
                    (shape ? links.back().position : unscale<double>(links.back().boundary.points.back().x()));
                std::optional<double> best;
                Polyline best_boundary;
                Visit next_visit{};
                for (int step : {1, -1}) {
                    if ((step < 0 && visit.row == 0) || (step > 0 && visit.row + 1 == rows.size())) continue;
                    const size_t target = step > 0 ? visit.row + 1 : visit.row - 1;
                    auto& strip = strips[std::min(visit.row, target)];
                    for (size_t next = 0; next < rows[target].scans.size(); ++next) {
                        const auto& other = rows[target].scans[next];
                        if (other.used) continue;
                        const double lo = std::max({direction * entry, direction > 0 ? current.lo : -current.hi,
                            direction > 0 ? other.lo : -other.hi});
                        const double hi = std::min(direction > 0 ? current.hi : -current.lo,
                            direction > 0 ? other.hi : -other.lo);
                        const auto boundary = shape ? boundary_links.end() :
                            boundary_links.find({visit.row, visit.scan, target, next, direction});
                        if (shape ? hi < lo : boundary == boundary_links.end()) continue;
                        // A strip changes only when a return is committed. Keep
                        // the same batch union and boundary order on rebuilds.
                        if (strip && !strip->pending.empty()) {
                            for (const auto& previous : strip->pending)
                                append(strip->occupied, fiber_contour_coverage(previous, width));
                            strip->pending.clear();
                            if (shape) {
                                strip->domain = diff_ex(local, union_ex(strip->occupied));
                                strip->edges = boundary_lines(strip->domain);
                                strip->tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(strip->edges);
                            } else {
                                strip->occupied = union_ex(strip->occupied);
                            }
                        }
                        if (shape) {
                            auto t = place_return(*shape, strip ? strip->domain : local,
                                strip ? strip->edges : edges, strip ? strip->tree : tree,
                                rows[std::min(visit.row, target)].y, direction, lo, hi);
                            if (t && (!best || *t > *best + coordinate_error_mm)) { best = t; next_visit = {target, next}; }
                        } else for (const auto& path : boundary->second) {
                            if (direction * (unscale<double>(path.points.front().x()) - entry) < -coordinate_error_mm) continue;
                            // The arc already lies on the original domain boundary.
                            // Check only newly occupied material: subtracting it
                            // from the domain can round unrelated boundary edges
                            // and falsely disconnect otherwise valid scanlines.
                            if (strip && !intersection_pl(Polylines{path}, strip->occupied).empty()) continue;
                            const double score = -unscale<double>(path.length());
                            if (!best || score > *best + coordinate_error_mm) {
                                best = score; best_boundary = path; next_visit = {target, next};
                            }
                        }
                    }
                }
                if (!best) return;
                const size_t strip = std::min(visit.row, next_visit.row);
                if (!strips[strip]) strips[strip] = std::make_unique<Strip>();
                strips[strip]->pending.push_back(shape ? translated_return(*shape, *best, rows[strip].y, direction) : best_boundary);
                links.push_back({shape ? direction * *best : 0, direction, std::move(best_boundary)});
                visits.push_back(next_visit);
                rows[next_visit.row].scans[next_visit.scan].used = true;
                direction = -direction;
            }
        };
        grow(1);
        std::reverse(visits.begin(), visits.end());
        std::reverse(links.begin(), links.end());
        if (!shape) for (auto& link : links) link.boundary.reverse();
        grow(links.empty() ? -1 : -links.back().direction);

        FiberInfillPath path;
        double station = 0;
        const auto append_point = [&](const Vec2d& p) {
            const Point q = scaled(frame * p);
            const Point3 next(q.x(), q.y(), coord_t(0));
            if (!path.geometry.points.empty()) {
                if (path.geometry.points.back() == next) return;
                station += unscale<double>((next - path.geometry.points.back()).cast<double>().norm());
            }
            path.geometry.points.push_back(next);
        };
        const auto& head = rows[visits.front().row].scans[visits.front().scan];
        append_point({links.empty() || links.front().direction > 0 ? head.lo : head.hi, rows[visits.front().row].y});
        for (size_t j = 0; j < links.size(); ++j) {
            const auto& link = links[j];
            if (!shape) {
                for (const auto& point : link.boundary.points) append_point(mm(point));
                continue;
            }
            const int step = visits[j + 1].row > visits[j].row ? 1 : -1;
            const auto local_point = [&](const Vec2d& p) -> Vec2d {
                return {link.position + link.direction * p.x(), rows[visits[j].row].y + step * p.y()};
            };
            const auto world = [&](const Vec2d& p) -> Vec2d { return frame * local_point(p); };
            for (size_t k = 0; k < 2; ++k) {
                const auto indices = shape->arc_indices[k];
                append_point(local_point(shape->samples[indices.first]));
                ContourArc arc;
                arc.center_mm = world({0, k == 0 ? radius : pitch - radius});
                arc.start_mm = world(shape->samples[indices.first]); arc.end_mm = world(shape->samples[indices.second]);
                arc.radius_mm = radius; arc.sweep_radians = arc.source_sweep_radians = -link.direction * step * PI / 2;
                arc.begin_mm = station;
                for (size_t i = indices.first + 1; i <= indices.second; ++i) append_point(local_point(shape->samples[i]));
                arc.end_distance_mm = station; path.arcs.push_back(arc);
            }
        }
        const auto& tail = rows[visits.back().row].scans[visits.back().scan];
        append_point({links.empty() || links.back().direction < 0 ? tail.hi : tail.lo, rows[visits.back().row].y});
        if (path.geometry.points.size() > 1) result.paths.push_back(std::move(path));
    }
    return result;
}
} // namespace Slic3r
