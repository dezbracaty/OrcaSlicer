// Exact material queries for stabilized pitch-2R fiber returns.
// All mutable state belongs to one LocalQuery; no external diagnostic dependencies.
#pragma once
namespace local_clearance {
using Wide = boost::multiprecision::int256_t;
inline Wide cross(const Point& a, const Point& b, const Point& c)
{
    return Wide(b.x() - a.x()) * Wide(c.y() - a.y()) - Wide(b.y() - a.y()) * Wide(c.x() - a.x());
}
struct Fraction
{
    Wide n, d;
};
inline bool less(const Fraction& a, const Fraction& b) { return a.n * b.d < b.n * a.d; }
// Exact clipping of a COMPLETE original edge against the strict interior of a
// convex CCW footprint. Any nonzero subsegment counts, not only whole edges.
inline bool enters(const Line& e, const Polygon& p)
{
    Fraction lo{0, 1}, hi{1, 1};
    for (size_t i = 0; i < p.points.size(); ++i) {
        const Point &a = p.points[i], &b = p.points[(i + 1) % p.points.size()];
        const Wide A = cross(a, b, e.a), B = cross(a, b, e.b) - A;
        if (B == 0) {
            if (A <= 0)
                return false;
            continue;
        }
        if (B > 0) {
            Fraction f{-A, B};
            if (less(lo, f))
                lo = f;
        } else {
            Fraction f{A, -B};
            if (less(f, hi))
                hi = f;
        }
        if (!less(lo, hi))
            return false;
    }
    return less(lo, hi);
}
inline Polygon hull(Points points)
{
    auto p = Geometry::convex_hull(std::move(points));
    if (p.area() < 0)
        p.reverse();
    return p;
}
inline Point seed(const Polygon& p)
{
    coord_t x = 0, y = 0;
    for (const Point& q : p.points) {
        x += q.x();
        y += q.y();
    }
    return {coord_t(std::llround(double(x) / p.points.size())), coord_t(std::llround(double(y) / p.points.size()))};
}
inline double units()
{
    const double K = std::round(1. / SCALING_FACTOR);
    if ((SCALING_FACTOR != SCALING_FACTOR_INTERNAL && SCALING_FACTOR != SCALING_FACTOR_INTERNAL_LARGE_PRINTER) || K <= 0)
        throw std::runtime_error("unsupported native runtime grid");
    return K;
}
inline bool exact_cardinal_radius(double width, coord_t& r)
{
    const CGAL::Gmpq q = CGAL::Gmpq(width) * CGAL::Gmpq(units()) / CGAL::Gmpq(2);
    if (q <= 0 || q >= CGAL::Gmpq(ClipperLib::loRange) || q.denominator() != 1)
        return false;
    r = coord_t(CGAL::to_double(q));
    return true;
}

inline void bound_point(Points& points, Slic3r::ReturnBounds x, Slic3r::ReturnBounds y)
{
    if (!x.finite() || !y.finite() || std::max({std::abs(x.lo), std::abs(x.hi), std::abs(y.lo), std::abs(y.hi)}) >= ClipperLib::loRange - 16)
        throw std::runtime_error("unsupported outward footprint coordinate");
    for (coord_t a : {coord_t(std::floor(x.lo)), coord_t(std::ceil(x.hi))})
        for (coord_t b : {coord_t(std::floor(y.lo)), coord_t(std::ceil(y.hi))})
            points.emplace_back(a, b);
}
inline Polygon butt(const Point& a, const Point& b, double width)
{
    coord_t r = 0;
    if (a == b)
        throw std::runtime_error("degenerate straight");
    if (exact_cardinal_radius(width, r) && (a.x() == b.x() || a.y() == b.y())) {
        const Point normal = a.x() == b.x() ? Point(r, 0) : Point(0, r);
        return hull({a + normal, a - normal, b + normal, b - normal});
    }
    using B = Slic3r::ReturnBounds;
    const B dx(double(b.x() - a.x())), dy(double(b.y() - a.y()));
    const B length = Slic3r::return_sqrt(dx * dx + dy * dy, true), radius = B(width) * B(units()) / B(2.);
    const B nx = B(-1.) * dy / length * radius, ny = dx / length * radius;
    Points points;
    for (const Point& q : {a, b})
        for (double s : {-1., 1.})
            bound_point(points, B(double(q.x())) + B(s) * nx, B(double(q.y())) + B(s) * ny);
    return hull(std::move(points));
}
inline Polygon box(double xmin, double ymin, double xmax, double ymax)
{
    using B = Slic3r::ReturnBounds;
    Points p;
    bound_point(p, B(xmin) / B(SCALING_FACTOR), B(ymin) / B(SCALING_FACTOR));
    bound_point(p, B(xmax) / B(SCALING_FACTOR), B(ymax) / B(SCALING_FACTOR));
    return hull(std::move(p));
}
inline Polygon round_cover(const Point& a, const Point& b, double width)
{
    const double r = width / 2;
    return box(std::min(a.x(), b.x()) * SCALING_FACTOR - r, std::min(a.y(), b.y()) * SCALING_FACTOR - r,
               std::max(a.x(), b.x()) * SCALING_FACTOR + r, std::max(a.y(), b.y()) * SCALING_FACTOR + r);
}
struct Domain
{
    const ExPolygons& m;
    continuous_fiber_detail::FiberDomainIndex index;
    const Domain* occupied = nullptr;
    explicit Domain(const ExPolygons& v, const Domain* o = nullptr) : m(v), index(v), occupied(o)
    {
        if (!index.supported)
            throw std::runtime_error("unsupported original integer domain");
    }
    bool fit(const Polygon& p, std::string* reason = nullptr, bool is_occupied = false) const
    {
        if (m.empty())
            return is_occupied;
        auto fail = [&](const char* s) {
            if (reason)
                *reason = s;
            return false;
        };
        if (p.points.size() < 3 || p.area() <= 0)
            return fail("degenerate_footprint");
        const Point middle = seed(p);
        for (size_t i = 0; i < p.points.size(); ++i)
            if (cross(p.points[i], p.points[(i + 1) % p.points.size()], middle) <= 0)
                return fail("uncertain_interior_seed");
        const auto side = index.side_doubled(2 * middle);
        if ((!is_occupied && side != continuous_fiber_detail::DomainSide::Inside) ||
            (is_occupied && side != continuous_fiber_detail::DomainSide::Outside))
            return fail(is_occupied ? "occupied_seed" : "material_seed_outside");
        Points closed = p.points;
        closed.push_back(closed.front());
        if (!index.permits(Polyline(closed), is_occupied))
            return fail(is_occupied ? "occupied_boundary_crossing" : "material_boundary_crossing");
        const auto bounds      = get_extents(p);
        bool interior_boundary = false;
        index.query(bounds.min.x(), bounds.min.y(), bounds.max.x(), bounds.max.y(), [&](const Line& e) {
            if (enters(e, p)) {
                interior_boundary = true;
                return false;
            }
            return true;
        });
        if (interior_boundary)
            return fail(is_occupied ? "enclosed_occupied_boundary" : "enclosed_material_boundary");
        if (occupied && !occupied->fit(p, reason, true))
            return false;
        return true;
    }
    bool footprint(const ExPolygon& f, const Point& seed_doubled, std::string* reason = nullptr, bool occupied_mode = false) const
    {
        const auto allowed = occupied_mode ? continuous_fiber_detail::DomainSide::Outside : continuous_fiber_detail::DomainSide::Inside;
        ExPolygons test_footprint{f};
        continuous_fiber_detail::FiberDomainIndex seed_index(test_footprint);
        if (seed_index.side_doubled(seed_doubled) != continuous_fiber_detail::DomainSide::Inside) {
            if (reason)
                *reason = "seed_not_inside_footprint_component";
            return false;
        }
        if (m.empty())
            return occupied_mode;
        if (index.side_doubled(seed_doubled) != allowed) {
            if (reason)
                *reason = "footprint_seed_side";
            return false;
        }
        for (const Polygon* ring : to_polygon_ptrs(f)) {
            Points p = ring->points;
            if (p.empty())
                continue;
            p.push_back(p.front());
            if (!index.permits(Polyline(p), occupied_mode)) {
                if (reason)
                    *reason = "full_footprint_boundary";
                return false;
            }
        }
        ExPolygons one{f};
        continuous_fiber_detail::FiberDomainIndex fi(one);
        bool enters_footprint = false;
        const auto bb         = get_extents(f);
        index.query(bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y(), [&](const Line& e) {
            if (!fi.permits(Polyline(Points{e.a, e.b}), true)) {
                enters_footprint = true;
                return false;
            }
            return true;
        });
        if (enters_footprint) {
            if (reason)
                *reason = "full_boundary_inside_footprint";
            return false;
        }
        return true;
    }
    bool straight(const Point& a, const Point& b, double width, bool round = false, std::string* reason = nullptr) const
    {
        return fit(round ? round_cover(a, b, width) : butt(a, b, width), reason);
    }
};

}

namespace local_clearance {

using Q = CGAL::Gmpq;
using X = CGAL::Sqrt_extension<Q, Q, CGAL::Tag_true, CGAL::Tag_true>;
struct XY
{
    Q x, y;
};
struct EX
{
    X lo, hi;
};
inline Q rat(double a)
{
    if (!std::isfinite(a))
        throw std::runtime_error("nonfinite exact scalar");
    return Q(a);
}
inline X root(const Q& q)
{
    if (q < 0)
        throw std::runtime_error("negative exact root");
    if (q == 0)
        return X(Q(0));
    return X(Q(0), Q(1), q);
}
inline X xmin(X a, const X& b) { return a < b ? a : b; }
inline X xmax(X a, const X& b) { return a > b ? a : b; }
inline bool in01(const X& u) { return u >= X(0) && u <= X(1); }
inline XY edgepoint(const Point& p) { return {Q(p.x()) / rat(units()), Q(p.y()) / rat(units())}; }
inline std::optional<EX> capsule_exact(XY a, XY b, Q y, Q r, bool strict = true)
{
    Q vlo = std::min(a.y, b.y), vhi = std::max(a.y, b.y);
    if (strict && (y + r <= vlo || y - r >= vhi))
        return {};
    std::vector<X> values;
    for (XY p : {a, b}) {
        const Q dy = y - p.y;
        if (dy * dy <= r * r) {
            const X reach = root(r * r - dy * dy);
            values.push_back(X(p.x) - reach);
            values.push_back(X(p.x) + reach);
        }
    }
    const Q dx = b.x - a.x, dy = b.y - a.y, K = dx * dx + dy * dy;
    if (K != 0 && dy != 0)
        for (int s : {-1, 1}) {
            const X u = X((y - a.y) / dy) - X(Q(s) * r * dx / (dy * K)) * root(K);
            if (in01(u))
                values.push_back(X(a.x + (y - a.y) * dx / dy) - X(Q(s) * r / dy) * root(K));
        }
    if (values.empty())
        return {};
    X lo = values.front(), hi = lo;
    for (const X& v : values) {
        lo = xmin(lo, v);
        hi = xmax(hi, v);
    }
    return EX{lo, hi};
}
// Full extrema of finite-edge / monotone quarter-arc swept-disk contact.
// Exact quadratic scalars keep both disconnected legal gaps and isolated
// contacts. Strict vertical-domain precheck preserves continuous tangency.
inline std::optional<EX> arc_block(XY a, XY b, Q R, Q cy, Q r, int quarter)
{
    const Q arc_lo = quarter == 0 ? cy - R : cy, arc_hi = quarter == 0 ? cy : cy + R;
    if (std::min(a.y, b.y) - arc_hi >= r || arc_lo - std::max(a.y, b.y) >= r)
        return {};
    std::vector<X> values;
    auto add = [&](X t) { values.push_back(t); };
    for (XY endpoint : {XY{Q(0), quarter == 0 ? cy - R : cy + R}, XY{R, cy}})
        if (auto q = capsule_exact(a, b, endpoint.y, r, false)) {
            add(q->lo - X(endpoint.x));
            add(q->hi - X(endpoint.x));
        }
    // Edge endpoints against outer/inner offset circles. R==r -> inner point.
    for (XY p : {a, b})
        for (int kind : {0, 1}) {
            const Q D = kind == 0 ? R + r : CGAL::abs(R - r), dy = p.y - cy;
            if (dy * dy > D * D)
                continue;
            if (D == 0) {
                if (dy == 0)
                    add(X(p.x));
                continue;
            }
            const int radial = (kind == 1 && r > R) ? -1 : 1;
            for (int sign : {-1, 1}) {
                const X dx = X(sign) * root(D * D - dy * dy);
                if (X(radial) * dx < X(0))
                    continue;
                if (quarter == 0 ? Q(radial) * dy > 0 : Q(radial) * dy < 0)
                    continue;
                add(X(p.x) - dx);
            }
        }
    const Q dx = b.x - a.x, dy = b.y - a.y, K = dx * dx + dy * dy;
    if (K != 0 && dy != 0)
        for (int s : {-1, 1}) {
            if (Q(-s) * dy < 0 || (quarter == 0 ? Q(s) * dx > 0 : Q(s) * dx < 0))
                continue;
            for (int d : {-1, 1}) {
                const Q k = Q(s) * R + Q(d) * r;
                const X u = X((cy - a.y) / dy) + X(k * dx / (dy * K)) * root(K);
                if (in01(u))
                    add(X(a.x + (cy - a.y) * dx / dy) + X(k / dy) * root(K));
            }
        }
    if (values.empty())
        throw std::runtime_error("missing quarter contact extrema");
    X lo = values.front(), hi = lo;
    for (const auto& v : values) {
        lo = xmin(lo, v);
        hi = xmax(hi, v);
    }
    return EX{lo, hi};
}
inline std::vector<EX> merge_open(std::vector<EX> blocks)
{
    std::sort(blocks.begin(), blocks.end(), [](const EX& a, const EX& b) { return a.lo < b.lo; });
    std::vector<EX> out;
    for (const auto& b : blocks) {
        if (b.hi < b.lo)
            throw std::runtime_error("inverted exact interval");
        if (b.lo == b.hi) {
            continue;
        }
        if (!out.empty() && b.lo < out.back().hi)
            out.back().hi = xmax(out.back().hi, b.hi);
        else
            out.push_back(b);
    }
    return out;
}
using Bounds = Slic3r::ReturnBounds;
inline Bounds bounds_of_q(const Q& q);
inline Q rat(const Q& q) { return q; }
struct ActualPoint {
    Q xv, yv;
    const Q& x() const { return xv; }
    const Q& y() const { return yv; }
};
struct ActualArc {
    ArcSegment native;
    ContourArc compatibility; // A derived mm view, never an exact predicate input.
    Q unit, radius_mm;
    ActualPoint center_mm, start_mm, end_mm;
    double sweep_radians;
    ActualArc(const ArcSegment& arc, const ContourArc& view) : native(arc), compatibility(view), unit(rat(units()))
    {
        const auto supported = [](const Point& p) {
            return p.x() >= -ClipperLib::loRange && p.x() <= ClipperLib::loRange &&
                   p.y() >= -ClipperLib::loRange && p.y() <= ClipperLib::loRange;
        };
        if (!native.is_arc || !supported(native.center) || !supported(native.start_point) || !supported(native.end_point) ||
            !std::isfinite(native.radius) || native.radius <= 0 || !std::isfinite(native.angle_radians) ||
            native.center == native.start_point || native.center == native.end_point ||
            (native.direction != ArcDirection::Arc_Dir_CCW && native.direction != ArcDirection::Arc_Dir_CW))
            throw std::runtime_error("Invalid authoritative actual arc fitting");
        const auto point = [&](const Point& p) -> ActualPoint { return {Q(p.x())/unit,Q(p.y())/unit}; };
        center_mm=point(native.center); start_mm=point(native.start_point); end_mm=point(native.end_point);
        radius_mm=rat(native.radius)/unit; // Do not round/snap the native-unit double radius.
        sweep_radians=native.angle_radians;
    }
};

inline bool arc_dir(const X& x, const X& y, const ActualArc& a)
{
    const Q sx = rat(a.start_mm.x()) - rat(a.center_mm.x()), sy = rat(a.start_mm.y()) - rat(a.center_mm.y()),
            ex = rat(a.end_mm.x()) - rat(a.center_mm.x()), ey = rat(a.end_mm.y()) - rat(a.center_mm.y());
    const X first = X(sx) * y - X(sy) * x, last = x * X(ey) - y * X(ex);
    return a.sweep_radians < 0 ? (first <= X(0) && last <= X(0)) : (first >= X(0) && last >= X(0));
}
struct NXY
{
    X x, y;
};
inline std::vector<NXY> normalized_endpoints(const ActualArc& a)
{
    std::vector<NXY> out;
    const Q cx = rat(a.center_mm.x()), cy = rat(a.center_mm.y()), R = rat(a.radius_mm);
    for (const auto& p : {a.start_mm, a.end_mm}) {
        const Q x = rat(p.x()) - cx, y = rat(p.y()) - cy, K = x * x + y * y;
        if (K == 0)
            throw std::runtime_error("invalid zero radial endpoint");
        out.push_back({X(cx) + X(R * x / K) * root(K), X(cy) + X(R * y / K) * root(K)});
    }
    return out;
}
struct MaterialArcBounds
{
    Slic3r::ReturnBounds x{0}, y{0};
    bool valid = false;
};
inline MaterialArcBounds material_arc_bounds_grid(const ActualArc& arc, double width)
{
    MaterialArcBounds out;
    if (!std::isfinite(width) || width < 0) return out;
    const Q h=rat(width)/Q(2);
    const Q xmin=std::min({arc.center_mm.x()-arc.radius_mm,arc.start_mm.x(),arc.end_mm.x()})-h,
            xmax=std::max({arc.center_mm.x()+arc.radius_mm,arc.start_mm.x(),arc.end_mm.x()})+h,
            ymin=std::min({arc.center_mm.y()-arc.radius_mm,arc.start_mm.y(),arc.end_mm.y()})-h,
            ymax=std::max({arc.center_mm.y()+arc.radius_mm,arc.start_mm.y(),arc.end_mm.y()})+h;
    out.x={bounds_of_q(xmin*arc.unit).lo,bounds_of_q(xmax*arc.unit).hi};
    out.y={bounds_of_q(ymin*arc.unit).lo,bounds_of_q(ymax*arc.unit).hi};
    out.valid=out.x.finite() && out.y.finite();
    return out;
}

inline bool arc_edge_intrudes(const ActualArc& arc, const Line& e, double width)
{
    const auto box = material_arc_bounds_grid(arc, width);
    if (box.valid && (std::max(e.a.x(), e.b.x()) < box.x.lo || std::min(e.a.x(), e.b.x()) > box.x.hi ||
                      std::max(e.a.y(), e.b.y()) < box.y.lo || std::min(e.a.y(), e.b.y()) > box.y.hi)) {
        return false;
    }
    const Q cx = rat(arc.center_mm.x()), cy = rat(arc.center_mm.y()), R = rat(arc.radius_mm), r = rat(width) / Q(2);
    const XY A = edgepoint(e.a), B = edgepoint(e.b);
    const Q ax = A.x - cx, ay = A.y - cy, bx = B.x - cx, by = B.y - cy, dx = bx - ax, dy = by - ay, K = dx * dx + dy * dy;
    auto point_near = [&](Q x, Q y) {
        const Q d = x * x + y * y;
        if (d == 0)
            return R < r;
        if (!arc_dir(X(x), X(y), arc))
            return false;
        if (d >= (R + r) * (R + r))
            return false;
        return R < r || d > (R - r) * (R - r);
    };
    if (point_near(ax, ay) || point_near(bx, by))
        return true;
    // Actual arc endpoints -> complete edge (metadata endpoints plus rounded G1
    // source are separately covered by native local footprint certificate).
    for (const auto& p : {arc.start_mm, arc.end_mm}) {
        const Q x = rat(p.x()) - A.x, y = rat(p.y()) - A.y;
        Q u        = K == 0 ? Q(0) : (x * dx + y * dy) / K;
        u          = std::max(Q(0), std::min(Q(1), u));
        const Q px = x - u * dx, py = y - u * dy;
        if (px * px + py * py < r * r)
            return true;
    }
    for (const NXY& p : normalized_endpoints(arc)) {
        const X x = p.x - X(A.x), y = p.y - X(A.y);
        X u        = K == 0 ? X(0) : (x * X(dx) + y * X(dy)) / X(K);
        u          = xmax(X(0), xmin(X(1), u));
        const X px = x - u * X(dx), py = y - u * X(dy);
        if (px * px + py * py < X(r * r))
            return true;
    }
    if (K == 0)
        return false;
    const Q dot = ax * dx + ay * dy, disc = dot * dot - K * (ax * ax + ay * ay - R * R);
    if (disc >= 0)
        for (int sign : {-1, 1}) {
            const X u = X(-dot / K) + X(Q(sign) / K) * root(disc);
            if (in01(u) && arc_dir(X(ax) + u * X(dx), X(ay) + u * X(dy), arc))
                return true;
        }
    const Q u0 = -dot / K;
    if (u0 >= 0 && u0 <= 1)
        for (int s : {-1, 1}) {
            if (!arc_dir(X(Q(-s) * dy), X(Q(s) * dx), arc))
                continue;
            const X signed_distance = X((dy * ax - dx * ay) / K) * root(K) + X(Q(s) * R);
            if (signed_distance > X(-r) && signed_distance < X(r))
                return true;
        }
    return false;
}
inline bool point_arc_near(XY p, const ActualArc& a, Q r)
{
    for (const auto& e : {a.start_mm, a.end_mm}) {
        const Q dx = p.x - rat(e.x()), dy = p.y - rat(e.y());
        if (dx * dx + dy * dy < r * r)
            return true;
    }
    const Q x = p.x - rat(a.center_mm.x()), y = p.y - rat(a.center_mm.y()), R = rat(a.radius_mm), D = x * x + y * y;
    if (D == 0)
        return R < r;
    if (!arc_dir(X(x), X(y), a))
        return false;
    return D < (R + r) * (R + r) && (r > R || D > (R - r) * (R - r));
}
inline bool algebraic_point_arc_near(const NXY& p, const ActualArc& a, Q r)
{
    for (const auto& e : {a.start_mm, a.end_mm}) {
        const X dx = p.x - X(rat(e.x())), dy = p.y - X(rat(e.y()));
        if (dx * dx + dy * dy < X(r * r))
            return true;
    }
    const X x = p.x - X(rat(a.center_mm.x())), y = p.y - X(rat(a.center_mm.y())), D = x * x + y * y;
    const Q R = rat(a.radius_mm);
    if (D == X(0))
        return R < r;
    if (!arc_dir(x, y, a))
        return false;
    return D < X((R + r) * (R + r)) && (r > R || D > X((R - r) * (R - r)));
}
inline bool normalized_endpoints_near_exact(const ActualArc& a, const ActualArc& b, Q r)
{
    using Y     = CGAL::Sqrt_extension<X, Q, CGAL::Tag_true, CGAL::Tag_false>;
    const Q acx = rat(a.center_mm.x()), acy = rat(a.center_mm.y()), bcx = rat(b.center_mm.x()), bcy = rat(b.center_mm.y()),
            RA = rat(a.radius_mm), RB = rat(b.radius_mm);
    for (const auto& ea : {a.start_mm, a.end_mm})
        for (const auto& eb : {b.start_mm, b.end_mm}) {
            const Q ax = rat(ea.x()) - acx, ay = rat(ea.y()) - acy, bx = rat(eb.x()) - bcx, by = rat(eb.y()) - bcy, KA = ax * ax + ay * ay,
                    KB = bx * bx + by * by;
            if (KA == 0 || KB == 0)
                throw std::runtime_error("zero normalized endpoint field");
            const X x = X(acx - bcx) + X(RA * ax / KA) * root(KA), y = X(acy - bcy) + X(RA * ay / KA) * root(KA);
            const Y dx(x, X(-RB * bx / KB), KB), dy(y, X(-RB * by / KB), KB);
            if (dx * dx + dy * dy < Y(X(r * r)))
                return true;
        }
    return false;
}
inline bool normalized_endpoints_near(const ActualArc& a, const ActualArc& b, Q r);
inline bool normalized_source_points_near_arc(const ActualArc& source, const ActualArc& target, Q r);
inline bool arcs_near(const ActualArc& a, const ActualArc& b, double sum_width_radius)
{
    const Q r = rat(sum_width_radius), RA = rat(a.radius_mm), RB = rat(b.radius_mm);
    if (normalized_source_points_near_arc(a, b, r))
        return true;
    if (normalized_source_points_near_arc(b, a, r))
        return true;
    if (normalized_endpoints_near(a, b, r))
        return true;
    for (const auto& p : {a.start_mm, a.end_mm})
        if (point_arc_near({rat(p.x()), rat(p.y())}, b, r))
            return true;
    for (const auto& p : {b.start_mm, b.end_mm})
        if (point_arc_near({rat(p.x()), rat(p.y())}, a, r))
            return true;
    const Q dx = rat(b.center_mm.x()) - rat(a.center_mm.x()), dy = rat(b.center_mm.y()) - rat(a.center_mm.y()), K = dx * dx + dy * dy;
    if (K == 0) {
        if (CGAL::abs(RA - RB) >= r)
            return false;
        for (const auto& p : {a.start_mm, a.end_mm})
            if (arc_dir(X(rat(p.x()) - rat(a.center_mm.x())), X(rat(p.y()) - rat(a.center_mm.y())), b))
                return true;
        for (const auto& p : {b.start_mm, b.end_mm})
            if (arc_dir(X(rat(p.x()) - rat(b.center_mm.x())), X(rat(p.y()) - rat(b.center_mm.y())), a))
                return true;
        return false;
    }
    for (int s : {-1, 1})
        for (int t : {-1, 1})
            if (arc_dir(X(Q(s) * dx), X(Q(s) * dy), a) && arc_dir(X(Q(t) * dx), X(Q(t) * dy), b)) {
                const X dist = root(K) + X(Q(t) * RB - Q(s) * RA);
                if (dist > X(-r) && dist < X(r))
                    return true;
            }
    const Q alpha = (RA * RA - RB * RB + K) / (Q(2) * K), h = RA * RA / K - alpha * alpha;
    if (h >= 0)
        for (int s : {-1, 1}) {
            const X ax = X(alpha * dx) - X(Q(s) * dy) * root(h), ay = X(alpha * dy) + X(Q(s) * dx) * root(h);
            if (arc_dir(ax, ay, a) && arc_dir(ax - X(dx), ay - X(dy), b))
                return true;
        }
    return false;
}
struct TypedCurve
{
    Lines lines;
    std::vector<Point> joins, arc_seeds;
    std::vector<ActualArc> arcs;
};
inline TypedCurve typed_curve(const FiberInfillPath& p)
{
    TypedCurve out;
    const auto& points = p.geometry.points;
    auto linear        = [&](size_t begin, size_t end) {
        for (size_t j = begin + 1; j <= end; ++j) {
            const Point a = points[j - 1].to_point(), b = points[j].to_point();
            if (a != b)
                out.lines.emplace_back(a, b);
            if (j < end)
                out.joins.push_back(b);
        }
    };
    if (p.geometry.fitting_result.empty())
        linear(0, points.size() - 1);
    else {
        size_t arc = 0;
        for (const auto& f : p.geometry.fitting_result) {
            if (f.path_type == EMovePathType::Linear_move)
                linear(f.start_point_index, f.end_point_index);
            else if (f.path_type == EMovePathType::Arc_move_cw || f.path_type == EMovePathType::Arc_move_ccw) {
                if (arc >= p.arcs.size())
                    throw std::runtime_error("missing typed arc metadata");
                const auto expected_direction=f.path_type==EMovePathType::Arc_move_cw ? ArcDirection::Arc_Dir_CW : ArcDirection::Arc_Dir_CCW;
                if (f.arc_data.direction!=expected_direction)
                    throw std::runtime_error("Actual fitting direction differs from its execution primitive");
                if (f.start_point_index >= points.size() || f.end_point_index >= points.size() ||
                    f.arc_data.start_point != points[f.start_point_index].to_point() ||
                    f.arc_data.end_point != points[f.end_point_index].to_point())
                    throw std::runtime_error("Actual fitting endpoints differ from emitted source");
                out.arcs.emplace_back(f.arc_data,p.arcs[arc++]);
                out.arc_seeds.push_back(points[f.start_point_index].to_point());
            } else
                throw std::runtime_error("unknown actual primitive");
        }
        if (arc != p.arcs.size())
            throw std::runtime_error("typed arc metadata count");
    }
    return out;
}
struct CurveBounds
{
    double xmin = INFINITY, ymin = INFINITY, xmax = -INFINITY, ymax = -INFINITY;
    bool valid = true;
};
inline CurveBounds typed_bounds(const TypedCurve& typed)
{
    using B = Slic3r::ReturnBounds;
    CurveBounds out;
    auto add = [&](B x, B y) {
        if (!x.finite() || !y.finite()) {
            out.valid = false;
            return;
        }
        out.xmin = std::min(out.xmin, x.lo);
        out.xmax = std::max(out.xmax, x.hi);
        out.ymin = std::min(out.ymin, y.lo);
        out.ymax = std::max(out.ymax, y.hi);
    };
    for (const auto& line : typed.lines)
        for (const Point& q : {line.a, line.b})
            add(B(double(q.x())), B(double(q.y())));
    for (const auto& a : typed.arcs) {
        const auto x=bounds_of_q(a.center_mm.x()*a.unit), y=bounds_of_q(a.center_mm.y()*a.unit),
                   r=bounds_of_q(a.radius_mm*a.unit);
        add(x-r,y-r); add(x+r,y+r);
        for (const auto& q : {a.start_mm,a.end_mm})
            add(bounds_of_q(q.x()*a.unit),bounds_of_q(q.y()*a.unit));
    }
    return out;
}
inline CurveBounds typed_bounds(const FiberInfillPath& p) { return typed_bounds(typed_curve(p)); }
// Strict interior overlap of two complete nominal butt rectangles. SAT axes
// are both segment tangents and normals; squared positive-root sums avoid
// floating decisions or incompatible two-root arithmetic.
inline bool less_root_sum(Q d, Q u, Q v)
{
    if (d < 0)
        return true;
    if (d == 0)
        return u > 0 || v > 0;
    const Q z = d * d - u - v;
    if (z < 0)
        return true;
    return z * z < Q(4) * u * v;
}
inline bool butt_overlap(const Line& A, const Line& B, double wa, double wb)
{
    const XY a = edgepoint(A.a), ae = edgepoint(A.b), b = edgepoint(B.a), be = edgepoint(B.b);
    const Q ax = ae.x - a.x, ay = ae.y - a.y, bx = be.x - b.x, by = be.y - b.y, KA = ax * ax + ay * ay, KB = bx * bx + by * by,
            ra = rat(wa) / 2, rb = rat(wb) / 2;
    if (KA == 0 || KB == 0)
        return false;
    auto axis = [&](Q vx, Q vy) {
        const Q a0 = a.x * vx + a.y * vy, a1 = ae.x * vx + ae.y * vy, b0 = b.x * vx + b.y * vy, b1 = be.x * vx + be.y * vy;
        const Q gap = std::max(std::min(b0, b1) - std::max(a0, a1), std::min(a0, a1) - std::max(b0, b1));
        const Q ca = -ay * vx + ax * vy, cb = -by * vx + bx * vy;
        return less_root_sum(gap, ra * ra * ca * ca / KA, rb * rb * cb * cb / KB);
    };
    return axis(ax, ay) && axis(-ay, ax) && axis(bx, by) && axis(-by, bx);
}

inline Bounds sqr_bounds(Bounds x)
{
    if (!x.finite())
        return {-INFINITY, INFINITY};
    const double lo = x.lo <= 0 && x.hi >= 0 ? 0. : std::min(x.lo * x.lo, x.hi * x.hi);
    const double hi = std::max(x.lo * x.lo, x.hi * x.hi);
    return {lo == 0 ? 0 : std::nextafter(lo, -INFINITY), std::nextafter(hi, INFINITY)};
}
inline Bounds norm_bounds(Bounds x, Bounds y) { return Slic3r::return_sqrt(sqr_bounds(x) + sqr_bounds(y), true); }
inline double arc_outer_radius(const ActualArc& a)
{
    double r=bounds_of_q(a.radius_mm).hi;
    for (const auto& p : {a.start_mm,a.end_mm}) {
        const auto d=norm_bounds(bounds_of_q(p.x()-a.center_mm.x()),bounds_of_q(p.y()-a.center_mm.y()));
        if (!d.finite()) return INFINITY;
        r=std::max(r,d.hi);
    }
    return std::nextafter(r,INFINITY);
}

struct CachedCurve
{
    TypedCurve typed;
    CurveBounds bounds;
    std::vector<double> arc_rho;
};
inline CachedCurve cache_curve(const FiberInfillPath& p)
{
    CachedCurve out;
    out.typed  = typed_curve(p);
    out.bounds = typed_bounds(out.typed);
    for (const auto& a : out.typed.arcs)
        out.arc_rho.push_back(arc_outer_radius(a));
    return out;
}
inline bool arc_circles_proven_clear(const ActualArc& a, double ra, const ActualArc& b, double rb, double sumr)
{
    const auto d2 = sqr_bounds(bounds_of_q(a.center_mm.x()) - bounds_of_q(b.center_mm.x())) +
                    sqr_bounds(bounds_of_q(a.center_mm.y()) - bounds_of_q(b.center_mm.y()));
    const auto radius  = Bounds(ra) + Bounds(rb) + Bounds(sumr);
    const auto radius2 = sqr_bounds(radius);
    return d2.finite() && radius2.finite() && d2.lo >= radius2.hi;
}
inline bool paths_overlap_cached(const CachedCurve& A, const CachedCurve& B, double wa, double wb)
{
    const double sum = (wa + wb) / 2;
    for (const auto& aa : A.typed.lines)
        for (const auto& bb : B.typed.lines)
            if (butt_overlap(aa, bb, wa, wb)) {
                return true;
            }
    for (const auto& arc : A.typed.arcs)
        for (const auto& line : B.typed.lines)
            if (arc_edge_intrudes(arc, line, 2 * sum)) {
                return true;
            }
    for (const auto& arc : B.typed.arcs)
        for (const auto& line : A.typed.lines)
            if (arc_edge_intrudes(arc, line, 2 * sum)) {
                return true;
            }
    for (size_t i = 0; i < A.typed.arcs.size(); ++i)
        for (size_t j = 0; j < B.typed.arcs.size(); ++j) {
            if (arc_circles_proven_clear(A.typed.arcs[i], A.arc_rho[i], B.typed.arcs[j], B.arc_rho[j], sum)) {
                continue;
            }
            if (arcs_near(A.typed.arcs[i], B.typed.arcs[j], sum)) {
                return true;
            }
        }
    return false;
}
inline bool paths_overlap(const FiberInfillPath& a, const FiberInfillPath& b, double wa, double wb)
{
    return paths_overlap_cached(cache_curve(a), cache_curve(b), wa, wb);
}
struct ReturnState
{
    FiberInfillPath path;
    std::vector<std::pair<size_t, size_t>> known_indices;
};
inline ReturnState actual_return(const ReturnShape& shape, double t, coord_t row, int direction, bool up, const Eigen::Matrix2d& frame,
                                 const continuous_fiber_detail::MaterialQueryCoordinates& coordinates)
{
    ReturnState out;
    const int step     = up ? 1 : -1;
    const double start = unscale<double>(up ? row : row + shape.scaled_pitch);
    double station     = 0;
    std::vector<Point> arc_centers;
    const coord_t origin_row = coordinates.canonical() ? (up ? row : coordinates.row_offset(row,shape.scaled_pitch,1)) : coord_t(0);
    auto local         = [&](const Vec2d& p) -> Vec2d { return {direction * (t + p.x()), start + step * p.y()}; };
    auto world         = [&](const Vec2d& p) -> Vec2d { return frame * local(p); };
    auto append        = [&](const Vec2d& p, size_t sample) {
        const auto offset = return_sample_offset(shape,sample);
        const auto transverse = coordinates.canonical() && offset ? std::optional<coord_t>(coordinates.row_offset(origin_row,*offset,step)) : std::nullopt;
        const Point q = coordinates.point(world(p),transverse);
        const Point3 v(q.x(), q.y(), coord_t(0));
        if (!out.path.geometry.points.empty()) {
            if (v == out.path.geometry.points.back())
                return;
            station += unscale<double>((v - out.path.geometry.points.back()).cast<double>().norm());
        }
        out.path.geometry.points.push_back(v);
    };
    for (size_t k = 0; k < 2; ++k) {
        const auto ids = shape.arc_indices[k];
        append(shape.samples[ids.first],ids.first);
        const size_t begin = out.path.geometry.points.size() - 1;
        ContourArc a;
        a.center_mm     = world({0, k == 0 ? shape.radius : shape.pitch - shape.radius});
        if (coordinates.canonical())
            arc_centers.push_back(coordinates.point(a.center_mm,
                coordinates.row_offset(origin_row,shape.scaled_pitch/2,step)));
        a.start_mm      = world(shape.samples[ids.first]);
        a.end_mm        = world(shape.samples[ids.second]);
        a.radius_mm     = shape.radius;
        a.sweep_radians = a.source_sweep_radians = -direction * step * PI / 2;
        a.begin_mm                               = station;
        for (size_t i = ids.first + 1; i <= ids.second; ++i)
            append(shape.samples[i],i);
        a.end_distance_mm = station;
        out.path.arcs.push_back(a);
        out.known_indices.push_back({begin, out.path.geometry.points.size() - 1});
    }
    continuous_fiber_detail::bind_contour_arcs(out.path.geometry, out.path.arcs, nullptr, coordinates.canonical() ? &arc_centers : nullptr);
    size_t k = 0;
    for (const auto& f : out.path.geometry.fitting_result)
        if (f.path_type == EMovePathType::Arc_move_cw || f.path_type == EMovePathType::Arc_move_ccw) {
            if (k >= out.known_indices.size() || out.known_indices[k++] != std::make_pair(f.start_point_index, f.end_point_index))
                throw std::runtime_error("actual bind differs from known emitted endpoint indices");
        }
    if (k != 2)
        throw std::runtime_error("actual U lost arcs");
    return out;
}
struct QuantNode
{
    Vec2d p;
    bool up;
    int axis;
    coord_t transverse_offset;
};
inline std::vector<QuantNode> quant_nodes(const ReturnShape& shape, bool actual_up)
{
    std::vector<QuantNode> nodes;
    std::set<size_t> endpoints;
    for (const auto& ids : shape.arc_indices) {
        endpoints.insert(ids.first);
        endpoints.insert(ids.second);
    }
    for (size_t j : endpoints)
        for (int axis : {0, 1})
            nodes.push_back({shape.samples[j], actual_up, axis, *return_sample_offset(shape,j)});
    for (Vec2d p : {Vec2d(0, shape.radius), Vec2d(0, shape.pitch - shape.radius)})
        for (int axis : {0, 1})
            nodes.push_back({p, actual_up, axis, shape.scaled_pitch/2});
    return nodes;
}
inline coord_t quant_value(
    const QuantNode& node, const ReturnShape& shape, double t, coord_t row, int direction, const Eigen::Matrix2d& frame,
    const continuous_fiber_detail::MaterialQueryCoordinates& coordinates)
{
    const double start = unscale<double>(node.up ? row : row + shape.scaled_pitch);
    const Vec2d local(direction * (t + node.p.x()), start + (node.up ? 1 : -1) * node.p.y());
    std::optional<coord_t> transverse;
    if (coordinates.canonical()) {
        const coord_t origin_row = node.up ? row : coordinates.row_offset(row,shape.scaled_pitch,1);
        transverse = coordinates.row_offset(origin_row,node.transverse_offset,node.up ? 1 : -1);
    }
    const Point q = coordinates.point(frame * local,transverse);
    return node.axis ? q.y() : q.x();
}
// G1 A: exact proposal roots are ordering aids; only actual certify accepts.
template<class Exact> inline Bounds adjacent_bounds(const Exact& q)
{
    const auto exact   = [](double v) { return Exact(rat(v)); };
    const auto bracket = [&](double a, double b) {
        if (exact(a) == q)
            return Bounds(a, a);
        if (exact(b) == q)
            return Bounds(b, b);
        return Bounds(a, b);
    };
    const double x = CGAL::to_double(q);
    if (std::isfinite(x)) {
        const auto at = exact(x);
        if (at == q)
            return {x, x};
        const double next = std::nextafter(x, at < q ? INFINITY : -INFINITY);
        if (std::isfinite(next)) {
            if (at < q && exact(next) >= q)
                return bracket(x, next);
            if (at > q && exact(next) <= q)
                return bracket(next, x);
        }
    }
    const auto key = [](double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        return (bits >> 63) ? ~bits : (bits ^ uint64_t(0x8000000000000000ULL));
    };
    const auto value = [](uint64_t k) {
        uint64_t bits = (k >> 63) ? (k ^ uint64_t(0x8000000000000000ULL)) : ~k;
        double v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    };
    const double max = std::numeric_limits<double>::max();
    if (q < exact(-max) || q > exact(max))
        return {-INFINITY, INFINITY};
    uint64_t low = key(-max), high = key(max);
    while (low < high) {
        const uint64_t distance = high - low, middle = low + distance / 2 + distance % 2;
        if (exact(value(middle)) <= q)
            low = middle;
        else
            high = middle - 1;
    }
    const double floor = value(low);
    if (exact(floor) == q)
        return {floor, floor};
    return bracket(floor, value(low + 1));
}
inline Bounds bounds_of_q(const Q& q) { return adjacent_bounds(q); }
inline Bounds bounds_of_x(const X& q) { return adjacent_bounds(q); }
inline double upper_x(const X& q) { return bounds_of_x(q).hi; }
struct PlacementContext
{
    bool present     = false;
    double current_y = 0, target_y = 0, current_lo = 0, current_hi = 0, target_lo = 0, target_hi = 0, entry = 0, stable = 0;
    coord_t current_row = 0, target_row = 0;
};
struct ReferenceArc
{
    Q vx, vy, bx, by;
    Q radius{0};
    double rho = 0;
    Bounds ecx{0}, ecy{0}, vx_bound{0}, vy_bound{0}, bx_bound{0}, by_bound{0}, sweep_x{0}, sweep_y{0}, axis_square{0};
    std::array<std::pair<Bounds, Bounds>, 2> relative{{std::make_pair(Bounds(0), Bounds(0)), std::make_pair(Bounds(0), Bounds(0))}};
};
// Nominal native/K and floating world evaluation use the same source controls.
// For both supported grids eta=|1/(K*sigma)-1|<u (u=2^-53), checked below.
// The world expression has at most five elementary rounding operations;
// division by sigma and conversion to the nominal unit add at most three.
// Thus |native/K-reference| <= sigma*(1+gamma_8)+gamma_8*L plus
// subnormal reserve. The existing gamma_12 bound below remains conservative.
inline Bounds control_error(double F0, double F1, double start, double px, double py, double begin, double end)
{
    const Bounds u(std::ldexp(1., -53)), g = Bounds(12.) * u / (Bounds(1.) - Bounds(12.) * u), T(std::max(std::abs(begin), std::abs(end)));
    const Bounds L = Bounds(std::abs(F0)) * (T + Bounds(std::abs(px))) +
                     Bounds(std::abs(F1)) * (Bounds(std::abs(start)) + Bounds(std::abs(py)));
    const double sigma = std::abs(SCALING_FACTOR), tiny = 64 * std::numeric_limits<double>::min() * std::max({1., sigma, 1 / sigma});
    const Bounds e = Bounds(sigma) * (Bounds(1.) + g) + g * L + Bounds(tiny);
    return {0, e.hi};
}
inline ReferenceArc reference_arc(
    const ReturnShape& shape, size_t i, coord_t row, int direction, bool up, const Eigen::Matrix2d& F, double begin, double end)
{
    ReferenceArc out;
    const int step     = up ? 1 : -1;
    const double start = unscale<double>(up ? row : row + shape.scaled_pitch), cy = i == 0 ? shape.radius : shape.pitch - shape.radius,
                 ly = start + step * cy;
    out.vx          = rat(F(0, 0)) * Q(direction);
    out.vy          = rat(F(1, 0)) * Q(direction);
    out.bx          = rat(F(0, 1)) * rat(ly);
    out.by          = rat(F(1, 1)) * rat(ly);
    out.radius      = rat(scale_(shape.radius))/rat(units());
    out.ecx         = control_error(F(0, 0), F(0, 1), start, 0, cy, begin, end);
    out.ecy         = control_error(F(1, 0), F(1, 1), start, 0, cy, begin, end);
    out.rho         = (bounds_of_q(out.radius) + norm_bounds(out.ecx, out.ecy)).hi;
    const auto ids  = shape.arc_indices[i];
    size_t k        = 0;
    for (size_t index : {ids.first, ids.second}) {
        const Vec2d p    = shape.samples[index];
        const double epy = start + step * p.y();
        const Q dx       = rat(F(0, 0)) * Q(direction) * rat(p.x()) + rat(F(0, 1)) * (rat(epy) - rat(ly)),
                dy       = rat(F(1, 0)) * Q(direction) * rat(p.x()) + rat(F(1, 1)) * (rat(epy) - rat(ly));
        const Bounds ex  = control_error(F(0, 0), F(0, 1), start, p.x(), p.y(), begin, end),
                     ey  = control_error(F(1, 0), F(1, 1), start, p.x(), p.y(), begin, end);
        out.rho          = std::max(out.rho, (norm_bounds(bounds_of_q(dx), bounds_of_q(dy)) + norm_bounds(ex, ey)).hi);
        const double rx = (ex + out.ecx).hi, ry = (ey + out.ecy).hi;
        out.relative[k++] = {bounds_of_q(dx) + Bounds(-rx, rx), bounds_of_q(dy) + Bounds(-ry, ry)};
    }
    if (!std::isfinite(out.rho) || rat(out.rho) < out.radius)
        throw std::runtime_error("invalid proposal envelope");
    out.vx_bound    = bounds_of_q(out.vx);
    out.vy_bound    = bounds_of_q(out.vy);
    out.bx_bound    = bounds_of_q(out.bx);
    out.by_bound    = bounds_of_q(out.by);
    out.sweep_x     = out.vx_bound * Bounds(begin, end) + out.bx_bound;
    out.sweep_y     = out.vy_bound * Bounds(begin, end) + out.by_bound;
    out.axis_square = sqr_bounds(out.vx_bound) + sqr_bounds(out.vy_bound);
    return out;
}
struct ArcBox
{
    Bounds x{0}, y{0}, R{0};
    std::array<std::pair<Bounds, Bounds>, 2> relative{{std::make_pair(Bounds(0), Bounds(0)), std::make_pair(Bounds(0), Bounds(0))}};
    double sweep = 0;
};
inline ArcBox actual_arc_box(const ActualArc& a)
{
    ArcBox out;
    out.x     = bounds_of_q(a.center_mm.x());
    out.y     = bounds_of_q(a.center_mm.y());
    out.R     = bounds_of_q(a.radius_mm);
    out.sweep = a.sweep_radians;
    size_t i  = 0;
    for (const auto& p : {a.start_mm, a.end_mm})
        out.relative[i++] = {bounds_of_q(p.x()) - out.x, bounds_of_q(p.y()) - out.y};
    return out;
}
inline ArcBox reference_arc_box(const ReferenceArc& a,
                                const std::vector<QuantNode>& nodes,
                                const ReturnShape& shape,
                                double lo,
                                double hi,
                                coord_t row,
                                int direction,
                                const Eigen::Matrix2d& frame,
                                bool up,
                                size_t arc,
                                const continuous_fiber_detail::MaterialQueryCoordinates& coordinates)
{
    ArcBox out;
    const QuantNode nx{{0, arc == 0 ? shape.radius : shape.pitch - shape.radius}, up, 0, shape.scaled_pitch/2},
        ny{{0, arc == 0 ? shape.radius : shape.pitch - shape.radius}, up, 1, shape.scaled_pitch/2};
    const coord_t ax = quant_value(nx, shape, lo, row, direction, frame, coordinates), bx = quant_value(nx, shape, hi, row, direction, frame, coordinates),
                  ay = quant_value(ny, shape, lo, row, direction, frame, coordinates), by = quant_value(ny, shape, hi, row, direction, frame, coordinates);
    out.x        = Bounds(bounds_of_q(Q(std::min(ax,bx))/rat(units())).lo,bounds_of_q(Q(std::max(ax,bx))/rat(units())).hi);
    out.y        = Bounds(bounds_of_q(Q(std::min(ay,by))/rat(units())).lo,bounds_of_q(Q(std::max(ay,by))/rat(units())).hi);
    out.R        = bounds_of_q(a.radius);
    out.relative = a.relative;
    out.sweep    = -direction * (up ? 1 : -1) * PI / 2;
    return out;
}
inline std::vector<std::pair<Bounds, Bounds>> endpoint_boxes(const ArcBox& a)
{
    std::vector<std::pair<Bounds, Bounds>> out;
    for (auto e : a.relative) {
        out.push_back({a.x + e.first, a.y + e.second});
        const auto length = norm_bounds(e.first, e.second);
        if (length.finite() && length.lo > 0)
            out.push_back({a.x + a.R * e.first / length, a.y + a.R * e.second / length});
    }
    return out;
}

enum class PointArcDecision { Unknown, Far, Near };
inline bool finite_arc_metadata(const ActualArc& a)
{
    // ActualArc has already validated its finite native fitting source.
    return a.radius_mm>0 && std::isfinite(a.sweep_radians);
}

inline NXY exact_normalized_endpoint(const ActualArc& a, size_t i)
{
    const auto& p = i == 0 ? a.start_mm : a.end_mm;
    const Q cx = rat(a.center_mm.x()), cy = rat(a.center_mm.y()), R = rat(a.radius_mm), x = rat(p.x()) - cx, y = rat(p.y()) - cy,
            K = x * x + y * y;
    if (K == 0)
        throw std::runtime_error("invalid zero radial endpoint");
    return {X(cx) + X(R * x / K) * root(K), X(cy) + X(R * y / K) * root(K)};
}
inline Bounds absolute_bounds(Bounds x)
{
    if (!x.finite())
        return {-INFINITY, INFINITY};
    const double lo = x.lo <= 0 && x.hi >= 0 ? 0. : std::min(std::abs(x.lo), std::abs(x.hi)), hi = std::max(std::abs(x.lo), std::abs(x.hi));
    return {lo, hi};
}
inline PointArcDecision bounded_normalized_point_arc(const std::pair<Bounds, Bounds>& p,
                                                     const ActualArc& target,
                                                     const ArcBox& box,
                                                     Bounds radius)
{
    // Invalid target metadata must never be swallowed by a different true OR arm.
    if (!finite_arc_metadata(target) || !p.first.finite() || !p.second.finite() || !radius.finite() || radius.lo <= 0 ||
        target.radius_mm <= 0)
        return PointArcDecision::Unknown;
    const auto dx = p.first - box.x, dy = p.second - box.y, D = norm_bounds(dx, dy);
    if (!D.finite() || D.lo <= 0)
        return PointArcDecision::Unknown;
    const auto first = box.relative[0].first * dy - box.relative[0].second * dx,
               last  = dx * box.relative[1].second - dy * box.relative[1].first;
    if (!first.finite() || !last.finite() || (first.lo <= 0 && first.hi >= 0) || (last.lo <= 0 && last.hi >= 0))
        return PointArcDecision::Unknown;
    const bool owned   = target.sweep_radians < 0 ? (first.hi < 0 && last.hi < 0) : (first.lo > 0 && last.lo > 0),
               outside = target.sweep_radians < 0 ? (first.lo > 0 || last.lo > 0) : (first.hi < 0 || last.hi < 0);
    bool unknown       = false;
    const auto radius2 = sqr_bounds(radius);
    if (!radius2.finite())
        return PointArcDecision::Unknown;
    for (const auto& e : {target.start_mm, target.end_mm}) {
        const auto d2 = sqr_bounds(p.first - bounds_of_q(e.x())) + sqr_bounds(p.second - bounds_of_q(e.y()));
        if (!d2.finite()) {
            unknown = true;
            continue;
        }
        if (d2.hi < radius2.lo)
            return PointArcDecision::Near;
        if (d2.lo >= radius2.hi)
            continue;
        unknown = true;
    }
    if (owned) {
        const auto distance = absolute_bounds(D - box.R);
        if (!distance.finite())
            unknown = true;
        else if (distance.hi < radius.lo)
            return PointArcDecision::Near;
        else if (!(distance.lo >= radius.hi))
            unknown = true;
    } else if (!outside)
        unknown = true;
    return unknown ? PointArcDecision::Unknown : PointArcDecision::Far;
}
inline bool normalized_source_points_near_arc(const ActualArc& source, const ActualArc& target, Q r)
{
    const auto original_group = [&]() {
        for (const NXY& p : normalized_endpoints(source))
            if (algebraic_point_arc_near(p, target, r))
                return true;
        return false;
    };

    const auto points = endpoint_boxes(actual_arc_box(source));
    bool valid        = finite_arc_metadata(source) && points.size() == 4;
    for (const auto& p : points)
        valid = valid && p.first.finite() && p.second.finite();
    if (!valid) {
        return original_group();
    }
    const auto target_box = actual_arc_box(target);
    const auto radius     = bounds_of_q(r);
    for (size_t i = 0; i < 2; ++i) {
        const auto decision = bounded_normalized_point_arc(points[2 * i + 1], target, target_box, radius);
        bool result;
        if (decision == PointArcDecision::Unknown) {
            result = algebraic_point_arc_near(exact_normalized_endpoint(source, i), target, r);
        } else {
            result = decision == PointArcDecision::Near;
        }

        if (result)
            return true;
    }
    return false;
}
inline bool normalized_endpoints_near(const ActualArc& a, const ActualArc& b, Q r)
{
    const auto A = endpoint_boxes(actual_arc_box(a)), B = endpoint_boxes(actual_arc_box(b));
    const Bounds threshold = sqr_bounds(bounds_of_q(r));
    const std::array<ActualPoint, 2> ae{{a.start_mm, a.end_mm}}, be{{b.start_mm, b.end_mm}};
    const auto exact_pair = [&](size_t i, size_t j) {
        using Y     = CGAL::Sqrt_extension<X, Q, CGAL::Tag_true, CGAL::Tag_false>;
        const Q acx = rat(a.center_mm.x()), acy = rat(a.center_mm.y()), bcx = rat(b.center_mm.x()), bcy = rat(b.center_mm.y()),
                RA = rat(a.radius_mm), RB = rat(b.radius_mm), ax = rat(ae[i].x()) - acx, ay = rat(ae[i].y()) - acy,
                bx = rat(be[j].x()) - bcx, by = rat(be[j].y()) - bcy, KA = ax * ax + ay * ay, KB = bx * bx + by * by;
        if (KA == 0 || KB == 0)
            throw std::runtime_error("zero normalized endpoint field");
        const X x = X(acx - bcx) + X(RA * ax / KA) * root(KA), y = X(acy - bcy) + X(RA * ay / KA) * root(KA);
        const Y dx(x, X(-RB * bx / KB), KB), dy(y, X(-RB * by / KB), KB);
        return dx * dx + dy * dy < Y(X(r * r));
    };
    const auto filtered = [&]() {
        for (size_t i = 0; i < 2; ++i)
            for (size_t j = 0; j < 2; ++j) {
                if (A.size() == 4 && B.size() == 4 && threshold.finite()) {
                    const auto& u       = A[2 * i + 1];
                    const auto& v       = B[2 * j + 1];
                    const auto distance = sqr_bounds(u.first - v.first) + sqr_bounds(u.second - v.second);
                    if (distance.finite()) {
                        if (distance.lo >= threshold.hi) {
                            continue;
                        }
                        if (distance.hi < threshold.lo) {
                            return true;
                        }
                    }
                }
                if (exact_pair(i, j))
                    return true;
            }
        return false;
    };
    const bool result = filtered();
    return result;
}

inline bool box_sector_strict(const ArcBox& a, Bounds x, Bounds y)
{
    const auto first = a.relative[0].first * y - a.relative[0].second * x, last = x * a.relative[1].second - y * a.relative[1].first;
    return first.finite() && last.finite() && (a.sweep < 0 ? (first.hi < 0 && last.hi < 0) : (first.lo > 0 && last.lo > 0));
}
inline bool arc_boxes_proven_overlap(const ArcBox& a, const ArcBox& b, double sumr)
{
    const auto rr = sqr_bounds(Bounds(sumr));
    for (auto pa : endpoint_boxes(a))
        for (auto pb : endpoint_boxes(b)) {
            const auto d2 = sqr_bounds(pa.first - pb.first) + sqr_bounds(pa.second - pb.second);
            if (d2.finite() && d2.hi < rr.lo)
                return true;
        }
    const auto dx = b.x - a.x, dy = b.y - a.y, d = norm_bounds(dx, dy), radii = a.R + b.R, outer = radii + Bounds(sumr);
    if (d.finite() && d.lo > 0 && radii.finite() && outer.finite() && d.lo >= radii.hi && d.hi < outer.lo) {
        const auto nx = dx / d, ny = dy / d;
        if (box_sector_strict(a, nx, ny) && box_sector_strict(b, Bounds(-1.) * nx, Bounds(-1.) * ny))
            return true;
    }
    return false;
}
inline bool candidate_preserves_native_stability(
    double frontier, double candidate, const PlacementContext& c, int direction, const Eigen::Matrix2d& F,
    const continuous_fiber_detail::MaterialQueryCoordinates& coordinates)
{
    if (!c.present)
        return false;
    const auto same = [&](double y, coord_t row, double anchor) {
        const Point a = coordinates.point(F * Vec2d(anchor, y),row), p = coordinates.point(F * Vec2d(direction * frontier, y),row),
                    q       = coordinates.point(F * Vec2d(direction * candidate, y),row);
        const double before = unscale<double>((p - a).cast<double>().norm()), after = unscale<double>((q - a).cast<double>().norm());
        return std::isfinite(before) && std::isfinite(after) && ((before < c.stable) == (after < c.stable));
    };
    const bool equal = same(c.current_y, c.current_row, c.entry) && same(c.current_y, c.current_row, c.current_lo) && same(c.current_y, c.current_row, c.current_hi) &&
                       same(c.target_y, c.target_row, c.target_lo) && same(c.target_y, c.target_row, c.target_hi);
    return equal;
}

inline bool proposal_pair_proven_separate(const ReferenceArc& ref, const ActualArc& old, double old_rho, double sumr)
{
    const Bounds nx = ref.sweep_x + Bounds(-ref.rho, ref.rho), ny = ref.sweep_y + Bounds(-ref.rho, ref.rho),
                 extent = Bounds(old_rho) + Bounds(sumr), ox = bounds_of_q(old.center_mm.x()) + Bounds(-extent.hi, extent.hi),
                 oy = bounds_of_q(old.center_mm.y()) + Bounds(-extent.hi, extent.hi);
    if (nx.finite() && ny.finite() && ox.finite() && oy.finite() && (nx.hi < ox.lo || ox.hi < nx.lo || ny.hi < oy.lo || oy.hi < ny.lo)) {
        return true;
    }
    const Bounds dx = bounds_of_q(old.center_mm.x()) - ref.bx_bound, dy = bounds_of_q(old.center_mm.y()) - ref.by_bound,
                 cross = ref.vx_bound * dy - ref.vy_bound * dx, S = Bounds(ref.rho) + Bounds(old_rho) + Bounds(sumr),
                 left = sqr_bounds(cross), right = ref.axis_square * sqr_bounds(S);
    if (left.finite() && right.finite() && left.lo > right.hi) {
        return true;
    }
    return false;
}

inline uint64_t geometry_double_bits(double x)
{
    uint64_t bits;
    std::memcpy(&bits, &x, sizeof bits);
    return bits;
}
inline bool same_native_arc(const ArcSegment& a,const ArcSegment& b)
{
    const auto scalar=[](double x,double y){return geometry_double_bits(x)==geometry_double_bits(y);};
    return a.center==b.center && a.start_point==b.start_point && a.end_point==b.end_point &&
           a.is_arc==b.is_arc && a.direction==b.direction && scalar(a.radius,b.radius) &&
           scalar(a.length,b.length) && scalar(a.angle_radians,b.angle_radians) &&
           scalar(a.polar_start_theta,b.polar_start_theta) && scalar(a.polar_end_theta,b.polar_end_theta);
}

inline bool same_typed_geometry(const TypedCurve& a, const TypedCurve& b)
{
    if (a.lines.size() != b.lines.size() || a.joins != b.joins || a.arc_seeds != b.arc_seeds || a.arcs.size() != b.arcs.size())
        return false;
    for (size_t i = 0; i < a.lines.size(); ++i)
        if (a.lines[i].a != b.lines[i].a || a.lines[i].b != b.lines[i].b)
            return false;
    const auto scalar=[](double x,double y){return geometry_double_bits(x)==geometry_double_bits(y);};
    const auto point=[&](const Vec2d& x,const Vec2d& y){return scalar(x.x(),y.x())&&scalar(x.y(),y.y());};
    for(size_t i=0;i<a.arcs.size();++i){
        const auto& A=a.arcs[i];const auto& B=b.arcs[i];const auto& n=A.native;const auto& m=B.native;
        if(A.unit!=B.unit || !same_native_arc(n,m)) return false;
        const auto& x=A.compatibility;const auto& y=B.compatibility;
        if(!point(x.center_mm,y.center_mm)||!point(x.start_mm,y.start_mm)||!point(x.end_mm,y.end_mm)||
           !scalar(x.radius_mm,y.radius_mm)||!scalar(x.sweep_radians,y.sweep_radians)||
           !scalar(x.source_sweep_radians,y.source_sweep_radians)||!scalar(x.begin_mm,y.begin_mm)||
           !scalar(x.end_distance_mm,y.end_distance_mm)) return false;
    }
    return true;
}

struct LocalQuery
{
    const ExPolygons& world;
    const ExPolygons& local;
    Domain world_index;
    Domain local_index;
    double width;
    Eigen::Matrix2d frame;
    const continuous_fiber_detail::MaterialQueryCoordinates coordinates;
    size_t isolated_supports = 0;
    LocalQuery(const ExPolygons& W, const ExPolygons& L, double w, const Eigen::Matrix2d& F,
               const continuous_fiber_detail::MaterialQueryCoordinates& mapping)
        : world(W), local(L), world_index(W), local_index(L), width(w), frame(F), coordinates(mapping)
    {
        const Q eta=CGAL::abs(Q(1)/(rat(units())*rat(SCALING_FACTOR))-Q(1));
        if(eta>=rat(std::ldexp(1.,-53)))
            throw std::runtime_error("Unsupported nominal lattice conversion bound");
    }
    struct Reservation
    {
        size_t id, owner;
        coord_t row;
        bool active;
        FiberInfillPath actual;
        CachedCurve cache;
        Reservation(size_t i, size_t o, coord_t y, bool a, FiberInfillPath p)
            : id(i), owner(o), row(y), active(a), actual(std::move(p)), cache(cache_curve(actual))
        {}
    };
    size_t owner = 0, next_reservation = 1, last_actual_collider = 0;
    std::vector<Reservation> reservations;
    size_t occupied_generation = 0;
    std::map<std::vector<uint64_t>, CachedCurve> positive_proofs;
    void invalidate_positive_proofs()
    {
        if (occupied_generation == std::numeric_limits<size_t>::max())
            throw std::runtime_error("occupied proof generation overflow");
        ++occupied_generation;
        positive_proofs.clear();
    }
    std::vector<uint64_t> proof_key(const ReturnShape& shape, double t, coord_t row, int direction, bool up) const
    {
        std::vector<uint64_t> key{geometry_double_bits(t),
                                  uint64_t(row),
                                  uint64_t(direction),
                                  uint64_t(up),
                                  uint64_t(occupied_generation),
                                  uint64_t(reinterpret_cast<uintptr_t>(&shape)),
                                  geometry_double_bits(shape.radius),
                                  geometry_double_bits(shape.pitch),
                                  uint64_t(shape.scaled_pitch),
                                  geometry_double_bits(shape.units_per_mm),
                                  uint64_t(shape.samples.size()),
                                  geometry_double_bits(SCALING_FACTOR),
                                  geometry_double_bits(width)};
        key.push_back(uint64_t(coordinates.canonical_quarter + 1));
        for (const auto& i : shape.arc_indices) {
            key.push_back(i.first);
            key.push_back(i.second);
        }
        for (int a = 0; a < 2; ++a)
            for (int b = 0; b < 2; ++b)
                key.push_back(geometry_double_bits(frame(a, b)));
        return key;
    }
    void begin_chain()
    {
        ++owner;
        invalidate_positive_proofs();
    }
    size_t reserve(const ReturnShape& shape, double t, coord_t row, int direction, bool up)
    {
        const size_t id  = next_reservation++;
        auto actual      = actual_return(shape, t, row, direction, up, frame, coordinates).path;
        const auto key   = proof_key(shape, t, row, direction, up);
        const auto proof = positive_proofs.find(key);
        bool reused      = false;
        if (proof != positive_proofs.end()) {
            const auto rebuilt = cache_curve(actual);
            if (same_typed_geometry(rebuilt.typed, proof->second.typed)) {
                reused               = true;
                last_actual_collider = 0;
            }
        }
        if (!reused) {
            std::string why;
            if (!certify(actual, &why))
                throw std::runtime_error("reserve lost actual certificate:" + why);
        }
        reservations.push_back({id, owner, row, true, std::move(actual)});
        invalidate_positive_proofs();
        return id;
    }
    void discard(size_t id)
    {
        for (auto& r : reservations)
            if (r.id == id) {
                r.active = false;
                break;
            }
        invalidate_positive_proofs();
    }
    bool identity(size_t id, const FiberInfillPath& path, size_t first, size_t last, size_t arc_first)
    {
        for (const auto& r : reservations)
            if (r.id == id && r.active) {
                const auto& expected = r.actual;
                if (last - first + 1 != expected.geometry.points.size() || arc_first + 2 > path.arcs.size())
                    return false;
                for (size_t j = 0; j < expected.geometry.points.size(); ++j)
                    if (path.geometry.points[first + j] != expected.geometry.points[j])
                        return false;
                // The native fitting is authoritative; compatibility mm views
                // can merge distinct controls after floating multiplication.
                using Fitting=typename std::decay_t<decltype(path.geometry.fitting_result)>::value_type;
                std::vector<const Fitting*> source_fittings, final_fittings;
                for (const auto& f : expected.geometry.fitting_result)
                    if (f.path_type==EMovePathType::Arc_move_cw || f.path_type==EMovePathType::Arc_move_ccw)
                        source_fittings.push_back(&f);
                size_t ordinal=0;
                for (const auto& f : path.geometry.fitting_result)
                    if (f.path_type==EMovePathType::Arc_move_cw || f.path_type==EMovePathType::Arc_move_ccw) {
                        if (ordinal>=arc_first && ordinal<arc_first+2) final_fittings.push_back(&f);
                        ++ordinal;
                    }
                if (source_fittings.size()!=2 || final_fittings.size()!=2) return false;
                for (size_t k=0;k<2;++k) {
                    const auto& a=*source_fittings[k];const auto& b=*final_fittings[k];
                    if (a.path_type!=b.path_type || first+a.start_point_index!=b.start_point_index ||
                        first+a.end_point_index!=b.end_point_index || !same_native_arc(a.arc_data,b.arc_data))
                        return false;
                }
                for (size_t k = 0; k < 2; ++k) {
                    const auto& a = path.arcs[arc_first + k];
                    const auto& b = expected.arcs[k];
                    if (a.center_mm != b.center_mm || a.start_mm != b.start_mm || a.end_mm != b.end_mm || a.radius_mm != b.radius_mm ||
                        a.sweep_radians != b.sweep_radians)
                        return false;
                }
                return true;
            }
        return false;
    }
    bool clear_occupied_cached(const CachedCurve& candidate, const std::set<size_t>& self = {})
    {
        last_actual_collider = 0;
        for (const auto& r : reservations)
            if (r.active && !self.count(r.id)) {
                const auto& a      = candidate.bounds;
                const auto& b      = r.cache.bounds;
                const Bounds delta = Bounds(width) * Bounds(units());
                if (a.valid && b.valid && delta.finite() &&
                    ((Bounds(a.xmax) + delta).hi < b.xmin || (Bounds(b.xmax) + delta).hi < a.xmin || (Bounds(a.ymax) + delta).hi < b.ymin ||
                     (Bounds(b.ymax) + delta).hi < a.ymin))
                    continue;
                if (paths_overlap_cached(candidate, r.cache, width, width)) {
                    last_actual_collider = r.id;
                    return false;
                }
            }
        return true;
    }
    bool clear_occupied(const FiberInfillPath& p, const std::set<size_t>& self = {}) { return clear_occupied_cached(cache_curve(p), self); }
    std::vector<Row> rows(coord_t origin, coord_t pitch, bool full)
    {
        auto rows       = scan_rows(local, origin, pitch, full);
        const Q radius  = rat(width) * rat(units()) / Q(2);
        const double rg = CGAL::to_double(radius);
        for (auto& row : rows) {
            std::vector<EX> blocks;
            local_index.index.query(local_index.index.bounds.min.x() - rg, row.scaled_y - rg, local_index.index.bounds.max.x() + rg,
                                    row.scaled_y + rg, [&](const Line& e) {
                                        if (auto b = capsule_exact({Q(e.a.x()), Q(e.a.y())}, {Q(e.b.x()), Q(e.b.y())}, Q(row.scaled_y),
                                                                   radius, true))
                                            blocks.push_back(*b);
                                        return true;
                                    });
            auto merged = merge_open(std::move(blocks));
            std::vector<Scan> result;
            for (const auto& s : row.scans) {
                X at(Q(coord_t(std::llround(s.lo * units())))), end(Q(coord_t(std::llround(s.hi * units()))));
                for (const auto& b : merged) {
                    if (b.hi < at || b.lo > end)
                        continue;
                    if (b.lo >= at) {
                        const X finish = xmin(b.lo, end);
                        if (finish == at)
                            ++isolated_supports;
                        result.push_back({CGAL::to_double(at) / units(), CGAL::to_double(finish) / units(), false});
                    }
                    at = xmax(at, b.hi);
                    if (at > end)
                        break;
                }
                if (at <= end) {
                    if (at == end)
                        ++isolated_supports;
                    result.push_back({CGAL::to_double(at) / units(), CGAL::to_double(end) / units(), false});
                }
            }
            row.scans = std::move(result);
        }
        return rows;
    }

    // Shared complete material proof. Final sources must run this even when O fails.
    bool certify_material_cached(const CachedCurve& cached, std::string* reason = nullptr)
    {
        const auto& typed = cached.typed;
        {
            for (const Line& line : typed.lines)
                if (!world_index.straight(line.a, line.b, width, false, reason)) {
                    return false;
                }
            for (const Point& join : typed.joins) {
                if (world_index.index.side_doubled(2 * join) != continuous_fiber_detail::DomainSide::Inside) {
                    if (reason)
                        *reason = "join_center_outside";
                    return false;
                }
                const Q r       = rat(width) / Q(2);
                const XY c      = edgepoint(join);
                bool hit        = false;
                const double rg = width / 2 / SCALING_FACTOR;
                world_index.index.query(join.x() - rg, join.y() - rg, join.x() + rg, join.y() + rg, [&](const Line& e) {
                    const XY a = edgepoint(e.a), b = edgepoint(e.b);
                    const Q dx = b.x - a.x, dy = b.y - a.y, K = dx * dx + dy * dy;
                    Q u       = K == 0 ? Q(0) : ((c.x - a.x) * dx + (c.y - a.y) * dy) / K;
                    u         = std::max(Q(0), std::min(Q(1), u));
                    const Q x = c.x - a.x - u * dx, y = c.y - a.y - u * dy;
                    if (x * x + y * y < r * r) {
                        hit = true;
                        return false;
                    }
                    return true;
                });
                if (hit) {
                    if (reason)
                        *reason = "actual_round_join_intrudes";
                    return false;
                }
            }
        }
        {
            for (size_t ai = 0; ai < typed.arcs.size(); ++ai) {
                const auto& a = typed.arcs[ai];
                if (world_index.index.side_doubled(2 * typed.arc_seeds[ai]) != continuous_fiber_detail::DomainSide::Inside) {
                    if (reason)
                        *reason = "actual_arc_seed_outside";
                    return false;
                }
                if (std::abs(a.sweep_radians) > PI) {
                    if (reason)
                        *reason = "unsupported_final_arc";
                    return false;
                }
                const auto box  = material_arc_bounds_grid(a, width);
                bool hit        = false;
                const auto edge = [&](const Line& e) {
                    if (arc_edge_intrudes(a, e, width)) {
                        hit = true;
                        return false;
                    }
                    return true;
                };
                if (box.valid)
                    world_index.index.query(box.x.lo, box.y.lo, box.x.hi, box.y.hi, edge);
                else
                    for (const Line& e : world_index.index.boundaries)
                        if (!edge(e))
                            break;
                if (hit) {
                    if (reason)
                        *reason = "actual_arc_full_sweep_intrudes";
                    return false;
                }
            }
        }
        return true;
    }
    bool certify(const FiberInfillPath& p,
                 std::string* reason           = nullptr,
                 const std::set<size_t>& self  = {},
                 CachedCurve* successful_proof = nullptr)
    {
        auto cached = cache_curve(p);
        if (!clear_occupied_cached(cached, self)) {
            if (reason)
                *reason = "actual_occupied_full_curve_overlap";
            return false;
        }
        if (!certify_material_cached(cached, reason))
            return false;
        if (successful_proof)
            *successful_proof = std::move(cached);
        return true;
    }

    std::optional<double> place(
        const ReturnShape& shape, coord_t row, int direction, double lo, double hi, bool final_up, const PlacementContext& context = {})
    {
        if (shape.pitch != 2 * shape.radius)
            throw std::runtime_error("material query requires pitch equal to twice the bend radius");
        std::vector<EX> blocks;
        const Q R = rat(shape.radius), r = rat(width) / Q(2), Y = Q(row) / rat(units());
        local_index.index.query((std::min(direction * lo, direction * hi) - shape.radius - width) / SCALING_FACTOR,
                                CGAL::to_double(Y - r) / SCALING_FACTOR,
                                (std::max(direction * lo, direction * hi) + shape.radius + width) / SCALING_FACTOR,
                                CGAL::to_double(Y + rat(shape.pitch) + r) / SCALING_FACTOR, [&](const Line& e) {
                                    XY a = edgepoint(e.a), b = edgepoint(e.b);
                                    a.x *= Q(direction);
                                    b.x *= Q(direction);
                                    a.y -= Y;
                                    b.y -= Y;
                                    for (int q : {0, 1})
                                        if (auto v = arc_block(a, b, R, R, r, q))
                                            blocks.push_back(*v);
                                    return true;
                                });
        auto merged = merge_open(std::move(blocks));
        X last(hi), lower(lo);
        std::vector<std::pair<X, X>> gaps;
        for (auto i = merged.rbegin(); i != merged.rend(); ++i) {
            if (i->lo > last || i->hi < lower)
                continue;
            if (i->hi <= last)
                gaps.push_back({xmax(lower, i->hi), last});
            last = xmin(last, i->lo);
        }
        if (last >= lower)
            gaps.push_back({lower, last});
        const bool prefilter_enabled = true;
        const auto nodes             = quant_nodes(shape, final_up);
        struct Trial
        {
            bool fits;
            std::vector<ContourArc> arcs;
            std::string reason;
            size_t collider_id;
            uint64_t evaluated_t;
            std::optional<CachedCurve> proof;
        };
        std::map<std::vector<coord_t>, Trial> trials;
        const auto state = [&](double t) {
            std::vector<coord_t> key;
            for (const auto& n : nodes)
                key.push_back(quant_value(n, shape, t, row, direction, frame, coordinates));
            return key;
        };
        const auto trial = [&](double t) -> const Trial& {
            auto key   = state(t);
            auto found = trials.find(key);
            if (found != trials.end()) {
                return found->second;
            }
            auto actual = actual_return(shape, t, row, direction, final_up, frame, coordinates);
            std::string why;
            CachedCurve successful;
            const bool fits = certify(actual.path, &why, {}, &successful);
            return trials
                .emplace(std::move(key),
                         Trial{fits, std::move(actual.path.arcs), std::move(why), last_actual_collider, geometry_double_bits(t),
                               fits ? std::optional<CachedCurve>(std::move(successful)) : std::nullopt})
                .first->second;
        };
        const auto finish = [&](double t, double begin, double end, double delta, double frontier, const char* method) {
            const auto& selected = trials.at(state(t));
            if (selected.fits && selected.proof && selected.evaluated_t == geometry_double_bits(t)) {
                positive_proofs[proof_key(shape, t, row, direction, final_up)] = *selected.proof;
            }
            return std::optional<double>(t);
        };
        for (const auto& gap : gaps) {
            if (gap.second < gap.first)
                continue;
            const auto begin_bound = bounds_of_x(gap.first), end_bound = bounds_of_x(gap.second);
            if (!begin_bound.finite() || !end_bound.finite())
                throw std::runtime_error("nonfinite supported material gap root");
            const double begin = begin_bound.hi, end = end_bound.lo;
            if (end < begin)
                continue;
            if (trial(end).fits)
                return finish(end, begin, end, 0, end, "original_outer");

            const std::array<ReferenceArc, 2> refs{{reference_arc(shape, 0, row, direction, final_up, frame, begin, end),
                                                    reference_arc(shape, 1, row, direction, final_up, frame, begin, end)}};
            std::vector<EX> proposal_blocks;
            std::vector<double> candidates;
            std::set<double> endpoint_candidates;
            std::set<double> boundary_candidates{begin};
            double delta = 0, conversion = 0;
            for (const auto& ref : refs)
                for (const auto& occupied : reservations)
                    if (occupied.active)
                        for (size_t j = 0; j < occupied.cache.typed.arcs.size(); ++j) {
                            const auto& old = occupied.cache.typed.arcs[j];
                            if (prefilter_enabled && proposal_pair_proven_separate(ref, old, occupied.cache.arc_rho[j], width))
                                continue;
                            const Q vx = ref.vx, vy = ref.vy, dx = ref.bx - rat(old.center_mm.x()), dy = ref.by - rat(old.center_mm.y()),
                                    A = vx * vx + vy * vy, B = vx * dx + vy * dy,
                                    S  = rat(ref.rho) + rat(occupied.cache.arc_rho[j]) + rat(width),
                                    S0 = rat(ref.radius) + rat(old.radius_mm) + rat(width), C = dx * dx + dy * dy - S * S,
                                    disc = B * B - A * C;
                            if (A <= 0)
                                throw std::runtime_error("invalid proposal translation axis");
                            if (disc < 0)
                                continue;
                            EX contact{X(-B / A) - X(Q(1) / A) * root(disc), X(-B / A) + X(Q(1) / A) * root(disc)};
                            if (contact.hi < X(begin) || contact.lo > X(end))
                                continue;
                            proposal_blocks.push_back(contact);
                            if (S < S0)
                                throw std::runtime_error("proposal radius envelope below actual radius");
                            delta = std::max(delta, upper_x(root((S * S - S0 * S0) / A)));
                            for (const auto& event : {contact.lo, contact.hi}) {
                                const auto bound = bounds_of_x(event);
                                if (!bound.finite())
                                    continue;
                                conversion = std::max(conversion, (Bounds(bound.hi) - Bounds(bound.lo)).hi);
                                for (double t : {bound.lo, bound.hi})
                                    if (t >= begin && t <= end)
                                        candidates.push_back(t);
                            }
                            // These finite-feature roots only propose points. They must not inherit
                            // the conservative complete-circle proposal-domain exclusion.
                            for (const auto& p : {old.start_mm, old.end_mm}) {
                                const auto radial = norm_bounds(bounds_of_q(p.x()) - bounds_of_q(old.center_mm.x()),
                                                                bounds_of_q(p.y()) - bounds_of_q(old.center_mm.y())),
                                           error  = radial - bounds_of_q(old.radius_mm);
                                if (!error.finite())
                                    continue;
                                const double ep_error = std::nextafter(std::max(std::abs(error.lo), std::abs(error.hi)), INFINITY);
                                const Q fx = ref.bx - rat(p.x()), fy = ref.by - rat(p.y()), fB = vx * fx + vy * fy,
                                        fS = rat(ref.rho) + rat(width) + rat(ep_error), fC = fx * fx + fy * fy - fS * fS,
                                        fD = fB * fB - A * fC;
                                if (fD < 0)
                                    continue;
                                for (const X& e : {X(-fB / A) - X(Q(1) / A) * root(fD), X(-fB / A) + X(Q(1) / A) * root(fD)}) {
                                    const auto bound = bounds_of_x(e);
                                    if (!bound.finite())
                                        continue;
                                    for (double t : {bound.lo, bound.hi})
                                        if (t >= begin && t <= end) {
                                            candidates.push_back(t);
                                            endpoint_candidates.insert(t);
                                        }
                                }
                            }
                        }
            delta = delta > 0 ? (Bounds(delta) + Bounds(conversion)).hi : 0;
            candidates.push_back(begin);
            std::sort(candidates.begin(), candidates.end(), std::greater<double>());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            std::optional<double> best;
            for (double t : candidates) {
                bool envelope_clear = true;
                for (const auto& b : proposal_blocks)
                    if (X(rat(t)) > b.lo && X(rat(t)) < b.hi) {
                        envelope_clear = false;
                        break;
                    }
                if (!envelope_clear && !endpoint_candidates.count(t) && !boundary_candidates.count(t))
                    continue;
                if (trial(t).fits) {
                    if (!best || t > *best)
                        best = t;
                    if (rat(end) - rat(*best) <= rat(delta) && candidate_preserves_native_stability(end, *best, context, direction, frame, coordinates))
                        return finish(*best, begin, end, delta, end,
                                      boundary_candidates.count(t) ? "near_material_boundary" :
                                      endpoint_candidates.count(t) ? "near_endpoint_event" :
                                                                     "near_contact_event");
                }
            }
            struct Interval
            {
                double lo, hi;
            };
            std::vector<Interval> queue{{begin, end}};
            const auto obstructed = [&](double a, double b, size_t collider) {
                if (!collider) {
                    return false;
                }
                bool proven = false;
                for (const auto& old : reservations)
                    if (old.id == collider && old.active) {
                        for (size_t i = 0; i < refs.size() && !proven; ++i) {
                            const auto interval = reference_arc_box(refs[i], nodes, shape, a, b, row, direction, frame, final_up, i, coordinates);
                            for (const auto& arc : old.cache.typed.arcs)
                                if (arc_boxes_proven_overlap(interval, actual_arc_box(arc), width)) {
                                    proven = true;
                                    break;
                                }
                        }
                        break;
                    }
                return proven;
            };

            {
                while (!queue.empty()) {
                    const Interval current = queue.back();
                    queue.pop_back();
                    if (best && current.hi <= *best)
                        continue;
                    if (best && rat(current.hi) - rat(*best) <= rat(delta) &&
                        candidate_preserves_native_stability(current.hi, *best, context, direction, frame, coordinates))
                        return finish(*best, begin, end, delta, current.hi, "outer_frontier_contact");

                    const auto& high_trial = trial(current.hi);
                    if (high_trial.fits)
                        return finish(current.hi, begin, end, 0, current.hi, "outer_exact");
                    if (state(current.lo) == state(current.hi)) {
                        continue;
                    }
                    if (obstructed(current.lo, current.hi, high_trial.collider_id))
                        continue;
                    double middle = current.lo + (current.hi - current.lo) / 2;
                    if (!(middle > current.lo && middle < current.hi)) {
                        if (trial(current.lo).fits)
                            return finish(current.lo, begin, end, 0, current.lo, "adjacent_exact");
                        continue;
                    }
                    // Outer-first: midpoint is only a split. Never bypass its upper child.
                    queue.push_back({current.lo, middle});
                    queue.push_back({std::nextafter(middle, INFINITY), current.hi});
                }
            }
            if (best)
                return finish(*best, begin, end, 0, *best, "all_outer_proven");
        }
        return {};
    }
};
}
