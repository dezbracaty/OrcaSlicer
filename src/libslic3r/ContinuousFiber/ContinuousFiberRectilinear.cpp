#include <boost/multiprecision/cpp_int.hpp>
#include <CGAL/Gmpq.h>
#include <CGAL/Sqrt_extension.h>
#include <cfenv>
#include <cstring>
#include "Geometry/ConvexHull.hpp"
#include "ContinuousFiberFillStrategy.hpp"
#include "FiberInfillMaterialCertificate.hpp"
#include "FiberDomainIndex.hpp"
#include "../AABBTreeLines.hpp"
#include "../ClipperUtils.hpp"
#include "../Int128.hpp"
#include "../Geometry/ArcWelder.hpp"

#include <array>
#include <cmath>
#include <cfenv>
#include <sstream>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace Slic3r {
namespace {
constexpr double chord_error_mm = 0.00005;
const double coordinate_error_mm = 4 * SCALING_FACTOR;
Vec2d mm(const Point& p) { return p.cast<double>() * SCALING_FACTOR; }
Point scaled(const Vec2d& p) { return Point::new_scale(p.x(), p.y()); }

struct Interval { double lo, hi; };
struct Scan { double lo, hi; bool used = false; };
struct Row { double y; coord_t scaled_y = 0; std::vector<Scan> scans; };
struct ReturnShape {
    double radius, pitch;
    coord_t scaled_pitch;
    double units_per_mm;
    std::vector<Vec2d> samples;
    std::array<std::pair<size_t, size_t>, 2> arc_indices;
};

double return_units_for_scale(double);

ReturnShape make_return(double radius, double pitch, coord_t scaled_pitch, double units_per_mm=0)
{
    ReturnShape shape{radius, pitch, scaled_pitch, units_per_mm>0?units_per_mm:return_units_for_scale(SCALING_FACTOR), {}, {}};
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

// Only the explicitly overwritten quarter supports carry integer provenance.
// Interior trigonometric samples continue through the original fractional map.
std::optional<coord_t> return_sample_offset(const ReturnShape& shape, size_t sample)
{
    if (sample == shape.arc_indices[0].first) return coord_t(0);
    if (sample == shape.arc_indices[0].second || sample == shape.arc_indices[1].first)
        return shape.scaled_pitch / 2;
    if (sample == shape.arc_indices[1].second) return shape.scaled_pitch;
    return std::nullopt;
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

// A fixed material boundary and each strip's occupied boundary are indexed
// independently. No D - O polygon is needed to place a local return.
struct ReturnBounds {
    double lo, hi;
    explicit ReturnBounds(double v) : lo(v), hi(v) {}
    ReturnBounds(double a, double b) : lo(a), hi(b) {}
    bool finite() const { return std::isfinite(lo) && std::isfinite(hi) && lo <= hi; }
};
ReturnBounds return_outward(double lo, double hi) {
    return {std::nextafter(lo, -INFINITY), std::nextafter(hi, INFINITY)};
}
ReturnBounds operator+(ReturnBounds a, ReturnBounds b) { return return_outward(a.lo+b.lo,a.hi+b.hi); }
ReturnBounds operator-(ReturnBounds a, ReturnBounds b) { return return_outward(a.lo-b.hi,a.hi-b.lo); }
ReturnBounds operator*(ReturnBounds a, ReturnBounds b) {
    const std::array<double,4> v{a.lo*b.lo,a.lo*b.hi,a.hi*b.lo,a.hi*b.hi};
    return return_outward(*std::min_element(v.begin(),v.end()),*std::max_element(v.begin(),v.end()));
}
ReturnBounds operator/(ReturnBounds a, ReturnBounds b) {
    if (b.lo<=0 && b.hi>=0) return {-INFINITY,INFINITY};
    const auto inverse=return_outward(1/b.hi,1/b.lo);
    return a*inverse;
}
ReturnBounds return_sqrt(ReturnBounds a, bool known_nonnegative) {
    if (!a.finite() || a.hi<0 || (!known_nonnegative && a.lo<0)) return {-INFINITY,INFINITY};
    return return_outward(std::sqrt(std::max(0.,a.lo)),std::sqrt(a.hi));
}
ReturnBounds return_min(ReturnBounds a,ReturnBounds b) { return {std::min(a.lo,b.lo),std::min(a.hi,b.hi)}; }
ReturnBounds return_max(ReturnBounds a,ReturnBounds b) { return {std::max(a.lo,b.lo),std::max(a.hi,b.hi)}; }
double return_units_for_scale(double scale) {
    if(scale!=SCALING_FACTOR_INTERNAL && scale!=SCALING_FACTOR_INTERNAL_LARGE_PRINTER) return 0;
    if(!std::isfinite(scale) || scale<=0) return 0;
    const double units=std::round(1./scale);
    return std::isfinite(units) && units>0 && units<=ClipperLib::loRange && 1./units==scale ? units:0;
}
ReturnBounds return_grid(coord_t v,double units) { return ReturnBounds(double(v))/ReturnBounds(units); }

// Grow an exact floating expansion. FMA supplies the integer-to-mm product
// residual; twoSum supplies every addition residual. Rounded equality alone
// never establishes an axial contact.
std::pair<double,double> return_two_sum(double a,double b) {
    const double sum=a+b, z=sum-a;
    return {sum,(a-(sum-z))+(b-z)};
}
int return_exact_grid_difference(const std::vector<double>& terms,coord_t grid,double units) {
    std::vector<double> expansion;
    const auto append=[&](double term) {
        std::vector<double> next;
        for (double x:expansion) {
            const auto s=return_two_sum(term,x);
            if (s.second!=0) next.push_back(s.second);
            term=s.first;
        }
        if (term!=0) next.push_back(term);
        expansion=std::move(next);
    };
    for(double term:terms) {
        if(term!=0 && std::abs(term)<std::sqrt(std::numeric_limits<double>::min()))
            throw std::runtime_error("Fiber return NumericalFailure: axis scalar exponent is unsupported");
        const double product=term*units;
        append(product);append(std::fma(term,units,-product));
    }
    append(-double(grid));
    return expansion.empty()?0:expansion.back()>0?1:-1;
}

using ReturnSide=continuous_fiber_detail::DomainSide;
struct ReturnDomain {
    ExPolygons domain;
    continuous_fiber_detail::FiberDomainIndex integer;
    double units_per_mm;
    explicit ReturnDomain(ExPolygons value,double units=0) : domain(std::move(value)),integer(domain),
        units_per_mm(units>0?units:return_units_for_scale(SCALING_FACTOR)) {}
    template<class Callback> void query(double min_x,double min_y,double max_x,double max_y,Callback callback) const {
        integer.query(std::nextafter(min_x*units_per_mm,-INFINITY)-4,
            std::nextafter(min_y*units_per_mm,-INFINITY)-4,
            std::nextafter(max_x*units_per_mm,INFINITY)+4,
            std::nextafter(max_y*units_per_mm,INFINITY)+4,callback);
    }
    ReturnSide side_bounded(ReturnBounds x,ReturnBounds y) const {
        if(domain.empty()) return ReturnSide::Outside;
        if(!x.finite() || !y.finite()) return ReturnSide::Uncertain;
        int winding=0;bool uncertain=false;
        query(x.lo,y.lo,std::max(x.hi,double(integer.bounds.max.x())/units_per_mm),y.hi,[&](const Line& e) {
            const auto ax=return_grid(e.a.x(),units_per_mm), ay=return_grid(e.a.y(),units_per_mm);
            const auto bx=return_grid(e.b.x(),units_per_mm), by=return_grid(e.b.y(),units_per_mm);
            const bool upward=ay.hi<=y.lo && by.lo>y.hi;
            const bool downward=by.hi<=y.lo && ay.lo>y.hi;
            const bool below=std::max(ay.hi,by.hi)<y.lo;
            const bool above=std::min(ay.lo,by.lo)>y.hi;
            if(below || above) return true;
            if(!upward && !downward) { uncertain=true;return false; }
            const auto cross=(bx-ax)*(y-ay)-(by-ay)*(x-ax);
            if(cross.lo>0) { if(upward) ++winding; }
            else if(cross.hi<0) { if(downward) --winding; }
            else { uncertain=true;return false; }
            return true;
        });
        return uncertain?ReturnSide::Uncertain:winding?ReturnSide::Inside:ReturnSide::Outside;
    }
    bool permits(const Polyline& path,bool occupied) const { return integer.permits(path,occupied); }

};

struct ReturnContact {
    ReturnBounds minimum,maximum;
    Line edge;
    size_t piece;
    ReturnBounds first_y,last_y;
    std::vector<ReturnBounds> witnesses;
};
// Certified images of boundary_x(y)-return_x(y). These use original integer
// edges, including conversion, direction and row arithmetic, not rounded Linef.
// The ideal source frame retains scan/pitch integer provenance. Rounded
// row/circle-center expressions used by emit realize it within the existing
// source-coordinate budget; they do not redefine a quarter-circle endpoint.
void return_contact_bounds(const Line& edge,const ReturnShape& shape,coord_t row,int direction,std::vector<ReturnContact>& out) {
    const double units_per_mm=shape.units_per_mm;
    const auto ax=ReturnBounds(double(direction))*return_grid(edge.a.x(),units_per_mm);
    const auto bx=ReturnBounds(double(direction))*return_grid(edge.b.x(),units_per_mm);
    const coord_t a_y=edge.a.y()-row,b_y=edge.b.y()-row;
    // These edges touch supports only. Their ownership is checked on the
    // complete return, not represented as an interior forbidden interval.
    if(a_y==b_y && (a_y==0 || a_y==shape.scaled_pitch)) return;
    const auto ay=return_grid(a_y,units_per_mm),by=return_grid(b_y,units_per_mm);
    const auto pitch=return_grid(shape.scaled_pitch,units_per_mm),radius=ReturnBounds(shape.radius);
    for(size_t piece=0;piece<3;++piece) {
        if(piece==2 && return_exact_grid_difference({shape.radius,shape.radius},shape.scaled_pitch,units_per_mm)==0) continue;
        const auto cy=piece==0?radius:pitch-radius;
        const auto first=piece==0?ReturnBounds(0):piece==1?cy:radius;
        const auto last=piece==0?radius:piece==1?pitch:pitch-radius;
        auto lo=return_max(first,return_min(ay,by));
        auto hi=return_min(last,return_max(ay,by));
        if(lo.lo>hi.hi) continue;
        const auto x_at=[&](ReturnBounds y) {
            if(piece==2) return radius;
            const auto dy=y-cy;
            // y is restricted to this ideal quarter's parameter domain.
            return return_sqrt(radius*radius-dy*dy,true);
        };
        std::vector<ReturnBounds> values,witnesses;
        if(a_y==b_y) {
            const auto y=return_min(return_max(ay,first),last),x=x_at(y);
            values.push_back(return_min(ax,bx)-x);values.push_back(return_max(ax,bx)-x);
            witnesses.push_back(y);
        } else {
            const auto t_at=[&](ReturnBounds y) { return ax+(bx-ax)*((y-ay)/(by-ay))-x_at(y); };
            values.push_back(t_at(lo));values.push_back(t_at(hi));
            witnesses.push_back(lo);witnesses.push_back(hi);
            if(piece!=2) {
                const auto slope=(bx-ax)/(by-ay);
                const auto stationary=cy-radius*slope/return_sqrt(ReturnBounds(1)+slope*slope,true);
                const auto overlap=ReturnBounds(std::max(lo.lo,stationary.lo),std::min(hi.hi,stationary.hi));
                if(overlap.lo<=overlap.hi) { values.push_back(t_at(overlap));witnesses.push_back(overlap); }
            }
        }
        ReturnBounds minimum(INFINITY,INFINITY),maximum(-INFINITY,-INFINITY);
        for(const auto& v:values) { minimum=return_min(minimum,v);maximum=return_max(maximum,v); }
        out.push_back({minimum,maximum,edge,piece,lo,hi,std::move(witnesses)});
    }
}

bool return_supported(const ExPolygons& local,double radius,double pitch,double width,coord_t scaled_pitch,double units) {
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
    return false;
#endif
    if(!std::numeric_limits<double>::is_iec559 || std::fegetround()!=FE_TONEAREST) return false;
    if(units<=0 || (radius!=0 && std::abs(radius)<std::sqrt(std::numeric_limits<double>::min()))) return false;
    if(return_exact_grid_difference({radius,radius},scaled_pitch,units)>0) return false;
    const double margin=2*(radius+pitch+width)*units+64;
    if(!std::isfinite(margin) || margin>=ClipperLib::loRange) return false;
    for(const auto* ring:to_polygon_ptrs(local)) for(const Point& p:ring->points)
        if(std::abs(double(p.x()))+margin>=ClipperLib::loRange || std::abs(double(p.y()))+margin>=ClipperLib::loRange) return false;
    const auto box=get_extents(local);
    const double extent=std::max({std::abs(double(box.min.x())),std::abs(double(box.min.y())),
        std::abs(double(box.max.x())),std::abs(double(box.max.y()))})/units;
    if(16*std::numeric_limits<double>::epsilon()*(extent+pitch+radius+1./units)>1./(64*units)) return false;
    return true;
}

enum class ReturnFit { Yes, No, Uncertain };
ReturnFit return_analytic_fit(const ReturnShape& shape,double t,coord_t row,int direction,
    const ReturnDomain& domain,const std::vector<ReturnContact>& contacts,bool occupied,
    const ReturnContact** uncertain_contact)
{
    if(domain.domain.empty()) return occupied?ReturnFit::Yes:ReturnFit::No;
    const double units=shape.units_per_mm;
    struct SourceY { coord_t grid; std::vector<double> mm; };
    const SourceY first_middle{0,{shape.radius}},last_middle{shape.scaled_pitch,{-shape.radius}};
    const auto compare=[&](const SourceY& y,coord_t grid) { return return_exact_grid_difference(y.mm,grid-y.grid,units); };
    const auto exact=[&](const std::vector<double>& mm,coord_t grid) { return return_exact_grid_difference(mm,grid,units); };
    const auto bound=[&](const SourceY& y) { auto b=return_grid(y.grid,units);for(double x:y.mm)b=b+ReturnBounds(x);return b; };
    std::vector<const ReturnContact*> middle_edges;
    std::vector<coord_t> middle_vertices;
    const auto covers=[&](const SourceY& first,const SourceY& last,const Line& edge) {
        return compare(first,std::min(edge.a.y(),edge.b.y())-row)>=0 &&
            compare(last,std::max(edge.a.y(),edge.b.y())-row)<=0;
    };
    for(const auto& c:contacts) {
        const auto uncertain=[&] { *uncertain_contact=&c;return ReturnFit::Uncertain; };
        if(!c.minimum.finite() || !c.maximum.finite()) return uncertain();
        if(t<c.minimum.lo || t>c.maximum.hi) continue;
        if(t>c.minimum.hi && t<c.maximum.lo) return ReturnFit::No;
        bool endpoint_contact=false;
        // A line has at most one non-collinear intersection. This certificate
        // is NOT valid for a quarter-circle, which may cross the same edge twice.
        if(c.piece==2) for(const Point& point:{c.edge.a,c.edge.b}) {
            const coord_t y=point.y()-row,x=direction*point.x();
            if(exact({t,shape.radius},x)==0 && compare(first_middle,y)<=0 && compare(last_middle,y)>=0) {
                middle_vertices.push_back(y);endpoint_contact=true;
            }
        }
        if(c.edge.a.x()!=c.edge.b.x()) {
            if(endpoint_contact) continue;
            return uncertain();
        }
        const coord_t x=direction*c.edge.a.x();
        if(exact({t,shape.radius},x)==0) {
            if(c.piece==2) { middle_edges.push_back(&c);continue; }
            const auto& y=c.piece==0?first_middle:last_middle;
            if(covers(y,y,c.edge)) continue;
        }
        const SourceY support{c.piece==0?0:shape.scaled_pitch,{}};
        if(c.piece<2 && exact({t},x)==0 && covers(support,support,c.edge)) continue;
        return uncertain();
    }
    const auto pitch=return_grid(shape.scaled_pitch,units),radius=ReturnBounds(shape.radius);
    for(size_t piece=0;piece<3;++piece) {
        if(piece==2 && exact({shape.radius,shape.radius},shape.scaled_pitch)==0) continue;
        const auto cy=piece==0?radius:pitch-radius;
        const SourceY first=piece==0?SourceY{0,{}}:piece==1?last_middle:first_middle;
        const SourceY last=piece==0?first_middle:piece==1?SourceY{shape.scaled_pitch,{}}:last_middle;
        std::vector<SourceY> events{first};
        if(piece==2) {
            std::vector<coord_t> ys=middle_vertices;
            for(const auto* c:middle_edges) for(coord_t y:{c->edge.a.y()-row,c->edge.b.y()-row}) ys.push_back(y);
            ys.erase(std::remove_if(ys.begin(),ys.end(),[&](coord_t y) { return compare(first_middle,y)>=0 || compare(last_middle,y)<=0; }),ys.end());
            std::sort(ys.begin(),ys.end());ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
            for(coord_t y:ys) events.push_back({y,{}});
        }
        events.push_back(last);
        for(size_t i=1;i<events.size();++i) {
            bool boundary=false;
            if(piece==2) for(const auto* c:middle_edges)
                if(covers(events[i-1],events[i],c->edge)) { boundary=true;break; }
            if(boundary) continue;
            bool proved=false;
            for(double fraction:{.5,1./3,2./3}) {
                const auto start=bound(events[i-1]),finish=bound(events[i]);
                const auto y=start+(finish-start)*ReturnBounds(fraction),dy=y-cy;
                const auto x=piece==2?radius:return_sqrt(radius*radius-dy*dy,true);
                const auto side=domain.side_bounded(ReturnBounds(double(direction))*(ReturnBounds(t)+x),return_grid(row,units)+y);
                if(side==ReturnSide::Uncertain || side==ReturnSide::Boundary) continue;
                if(side==(occupied?ReturnSide::Inside:ReturnSide::Outside)) return ReturnFit::No;
                proved=true;break;
            }
            if(!proved) return ReturnFit::Uncertain;
        }
    }
    return ReturnFit::Yes;
}

std::optional<double> place_return_separate(const ReturnShape& shape,const ReturnDomain& material,
    const ReturnDomain* occupied,coord_t row_grid,int direction,double lo,double hi)
{
    if(hi<lo || material.domain.empty()) return std::nullopt;
    const double row=double(row_grid)*(1./shape.units_per_mm);
    const double inset_budget=std::nextafter(4./shape.units_per_mm,0.);
    std::vector<Interval> nominal;
    std::vector<ReturnContact> material_contacts,occupied_contacts;
    const double left=std::min(direction*lo,direction*hi)-shape.radius;
    const double right=std::max(direction*lo,direction*hi)+shape.radius;
    const auto collect=[&](const ReturnDomain& domain,std::vector<ReturnContact>& contacts) {
        domain.query(left,row,right,row+shape.pitch,[&](const Line& edge) {
            const auto a=mm(edge.a),b=mm(edge.b);
            contact_intervals(Linef({direction*a.x(),a.y()-row},{direction*b.x(),b.y()-row}),shape,nominal);
            return_contact_bounds(edge,shape,row_grid,direction,contacts);
            return true;
        });
    };
    collect(material,material_contacts);
    if(occupied) collect(*occupied,occupied_contacts);
    // Only proven-open interiors may be discarded. Nominal merging must not
    // silently swallow a feasible critical point inside a roundoff envelope.
    std::vector<Interval> blocked;
    const auto add_proven=[&](const std::vector<ReturnContact>& contacts) {
        for(const auto& c:contacts) if(c.minimum.finite() && c.maximum.finite() && c.minimum.hi<c.maximum.lo)
            blocked.push_back({c.minimum.hi,c.maximum.lo});
    };
    add_proven(material_contacts);add_proven(occupied_contacts);
    std::sort(blocked.begin(),blocked.end(),[](const Interval& a,const Interval& b) {
        return a.lo!=b.lo?a.lo<b.lo:a.hi<b.hi;
    });
    std::vector<Interval> merged;
    for(const auto& interval:blocked) {
        if(interval.hi<lo || interval.lo>hi) continue;
        if(!merged.empty() && interval.lo<merged.back().hi) merged.back().hi=std::max(merged.back().hi,interval.hi);
        else merged.push_back(interval);
    }
    const ReturnContact* uncertain_contact=nullptr;
    const auto fits=[&](double t) {
        const auto line=translated_return(shape,t,row,direction);
        if(!material.permits(line,false) || (occupied && !occupied->permits(line,true))) return ReturnFit::No;
        const auto inside=return_analytic_fit(shape,t,row_grid,direction,material,material_contacts,false,&uncertain_contact);
        if(inside!=ReturnFit::Yes) return inside;
        return occupied?return_analytic_fit(shape,t,row_grid,direction,*occupied,occupied_contacts,true,&uncertain_contact):ReturnFit::Yes;
    };
    const auto try_interval=[&](double begin,double end)->std::optional<double> {
        if(end<begin) return std::nullopt;
        std::vector<double> geometry_candidates{end};
        for(const auto& interval:nominal) for(double t:{interval.lo,interval.hi})
            if(t>=begin && t<=end) geometry_candidates.push_back(t);
        std::sort(geometry_candidates.begin(),geometry_candidates.end(),std::greater<double>());
        geometry_candidates.erase(std::unique(geometry_candidates.begin(),geometry_candidates.end()),geometry_candidates.end());
        std::vector<double> candidates;
        for(double t:geometry_candidates) {
            candidates.push_back(t);
            const double inset=std::min(inset_budget,(t-begin)/2);
            // Round toward the event so an intended four-grid inset cannot
            // exceed its budget by the final subtraction rounding.
            candidates.push_back(inset>0?std::nextafter(t-inset,t):t);
        }
        std::sort(candidates.begin(),candidates.end(),std::greater<double>());
        candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
        bool uncertain=false;
        double uncertain_t=end;
        for(double candidate:candidates) {
            if(uncertain && candidate<std::nextafter(uncertain_t-inset_budget,uncertain_t)) break;
            const auto result=fits(candidate);
            if(result==ReturnFit::Yes) return candidate;
            if(result==ReturnFit::Uncertain && !uncertain) { uncertain=true;uncertain_t=candidate; }
        }
        if(uncertain) {
            std::ostringstream message;message.precision(17);
            message<<"Fiber return NumericalFailure: uncertified analytic contact, t="<<end
                <<" range=["<<begin<<','<<end<<"] row="<<row<<" direction="<<direction
                <<" radius="<<shape.radius<<" pitch="<<shape.pitch;
            if(uncertain_contact) {
                const auto& c=*uncertain_contact;
                message<<" edge=["<<c.edge.a.x()<<','<<c.edge.a.y()<<';'<<c.edge.b.x()<<','<<c.edge.b.y()
                    <<"] piece="<<c.piece<<" min=["<<c.minimum.lo<<','<<c.minimum.hi
                    <<"] max=["<<c.maximum.lo<<','<<c.maximum.hi<<"]";
                for(const auto& y:c.witnesses) message<<" witness_y=["<<y.lo<<','<<y.hi<<"]";
            }
            throw std::runtime_error(message.str());
        }
        return std::nullopt;
    };
    double end=hi;
    for(auto i=merged.rbegin();i!=merged.rend();++i) {
        if(auto found=try_interval(std::max(lo,i->hi),end)) return found;
        end=std::min(end,i->lo);
    }
    return try_interval(lo,end);
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
        rows[i].scaled_y = y;
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
#include "ContinuousFiberClearanceQuery.hpp"

struct MaterialQueryResult {
    FiberInfillCandidates candidates;
    std::vector<continuous_fiber_detail::FinalSourceOccupiedResult> occupied;
};
MaterialQueryResult generate_rectilinear_material_query(
    const ExPolygons& material, const ContinuousFiberConfig& config,
    double rotation_radians, const Point& grid_origin)
{
    const double radius = config.infill_bend_radius_mm, width = config.infill_flow.width();
    const double stable_length = config.corner_stabilization_length_mm;
    if (!std::isfinite(radius) || radius < 0 || !std::isfinite(width) || width <= 0 ||
        !std::isfinite(config.infill_density) || config.infill_density <= 0 || config.infill_density > 100 ||
        !std::isfinite(rotation_radians) || !std::isfinite(stable_length) || stable_length < 0)
        throw std::invalid_argument("Invalid rectilinear fiber infill parameters");
    const double requested_pitch = width * 100 / config.infill_density;
    if (requested_pitch > unscale<double>(std::numeric_limits<coord_t>::max()) / 4)
        throw std::invalid_argument("Fiber scanline spacing is outside the supported coordinate range");
    coord_t pitch_scaled;
    if(!local_clearance::exact_cardinal_radius(requested_pitch*2,pitch_scaled))pitch_scaled=scale_(requested_pitch);
    const double pitch = unscale<double>(pitch_scaled);
    if (pitch_scaled <= 0 || pitch < 2 * radius)
        throw std::invalid_argument("fiber_infill_bend_radius requires scanline spacing >= twice the configured radius");
    FiberInfillCandidates result;
    std::vector<continuous_fiber_detail::FinalSourceOccupiedResult> occupied;
    if (pitch_scaled!=coord_t(std::llround(2*radius*local_clearance::units())) || radius<=0 || stable_length<=0) throw std::runtime_error("material query requires positive radius and stability with pitch equal to twice the radius");
    if (material.empty()) return {std::move(result),std::move(occupied)};
    // Columns are the scan and transverse axes. This frame has determinant -1.
    Eigen::Matrix2d frame;
    frame << -std::sin(rotation_radians), std::cos(rotation_radians),
              std::cos(rotation_radians), std::sin(rotation_radians);
    const continuous_fiber_detail::MaterialQueryCoordinates coordinates(rotation_radians);
    if (coordinates.canonical()) frame = coordinates.frame;
    if (coordinates.canonical() && pitch_scaled % 2 != 0)
        throw std::runtime_error("Canonical return requires an integral half pitch");
    ExPolygons local = material;
    for (auto& region : local) {
        const auto transform = [&](Polygon& ring) {
            for (auto& p : ring.points) p = coordinates.canonical() ? coordinates.input_point(p) : scaled(frame.transpose() * mm(p));
            ring.reverse();
        };
        transform(region.contour); for (auto& hole : region.holes) transform(hole);
    }
    // The material query supports the declared finite pitch=2R template.
    // Unsupported inputs report an explicit error; no old-C/native fallback.
    const double units_per_mm=return_units_for_scale(SCALING_FACTOR);
    const bool separate=true;

    local_clearance::LocalQuery local_query(material,local,width,frame,coordinates);

    auto rows=local_query.rows((coordinates.canonical() ? coordinates.input_point(grid_origin) : scaled(frame.transpose()*mm(grid_origin))).y(),pitch_scaled,config.infill_density==100);
    size_t scan_count=0;for(const auto&r:rows)scan_count+=r.scans.size();
    // Proven native retained/chunk consequence, not a process-length heuristic.
    bool all_below_stable=true;double maximum_scan_bound=0;
    for(const auto&row:rows)for(const auto&scan:row.scans){
        const Point a=coordinates.point(frame*Vec2d(scan.lo,row.y),row.scaled_y),b=coordinates.point(frame*Vec2d(scan.hi,row.y),row.scaled_y);
        const double bound=std::nextafter(unscale<double>((b-a).cast<double>().norm()),INFINITY);
        maximum_scan_bound=std::max(maximum_scan_bound,bound);
        if(!std::isfinite(bound)||!(bound<stable_length))all_below_stable=false;
    }

    if(all_below_stable){
        for(const auto&row:rows)for(const auto&scan:row.scans){
            const Point a=coordinates.point(frame*Vec2d(scan.lo,row.y),row.scaled_y),b=coordinates.point(frame*Vec2d(scan.hi,row.y),row.scaled_y);if(a==b)continue;
            FiberInfillPath path;path.geometry.points.emplace_back(a.x(),a.y(),coord_t(0));path.geometry.points.emplace_back(b.x(),b.y(),coord_t(0));
            std::string why;if(!local_query.certify(path,&why))throw std::runtime_error("LOCAL_SINGLETON_CERTIFICATE_FAILED:"+why);
            occupied.push_back({continuous_fiber_detail::FinalSourceOccupiedKind::Clear,0});
            result.paths.push_back(std::move(path));
        }

        return {std::move(result),std::move(occupied)};
    }
    const auto edges = separate ? Linesf{} : boundary_lines(local);
    const auto tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    const auto shape = radius > 0 ? std::optional<ReturnShape>(make_return(radius, pitch, pitch_scaled, units_per_mm)) : std::nullopt;
    const bool stabilized = shape && stable_length > 0;
    const auto boundary_links = radius == 0 ? boundary_returns(local, rows, edges, tree) : BoundaryReturns{};
    // Active actual U reservations replace dead raw-strip polygon work here.
    for (size_t first = 0; first < rows.size(); ++first) for (size_t seed = 0; seed < rows[first].scans.size(); ++seed) {
        if (rows[first].scans[seed].used) continue;
        local_query.begin_chain();
        struct Visit { size_t row, scan; };
        struct Link { double position; int direction; Polyline boundary; size_t reservation_id=0; };
        std::vector<Visit> visits{{first, seed}};
        std::vector<Link> links;
        std::set<std::pair<size_t,size_t>> claimed;
        if (stabilized) claimed.emplace(first,seed);
        else rows[first].scans[seed].used = true;
        // Grow both open ends before emitting geometry. Otherwise a short
        // forward chain can strand a feasible connection at its starting end.
        const auto grow = [&](int direction,bool reverse_for_emit) {
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
                    for (size_t next = 0; next < rows[target].scans.size(); ++next) {
                        const auto& other = rows[target].scans[next];
                        if (other.used || (stabilized && claimed.count({target,next}))) continue;
                        const double lo = std::max({direction * entry + (stabilized && !links.empty() ? stable_length : 0.0),
                            direction > 0 ? current.lo : -current.hi,
                            direction > 0 ? other.lo : -other.hi});
                        const double hi = std::min(direction > 0 ? current.hi : -current.lo,
                            direction > 0 ? other.hi : -other.lo);
                        const auto boundary = shape ? boundary_links.end() :
                            boundary_links.find({visit.row, visit.scan, target, next, direction});
                        if (shape ? hi < lo : boundary == boundary_links.end()) continue;
                        if (shape) {
                            const double row=rows[std::min(visit.row,target)].y;
                            local_clearance::PlacementContext context{true,rows[visit.row].y,rows[target].y,current.lo,current.hi,other.lo,other.hi,entry,stable_length,rows[visit.row].scaled_y,rows[target].scaled_y};
                            auto t=local_query.place(*shape,rows[std::min(visit.row,target)].scaled_y,direction,lo,hi,((target>visit.row)!=reverse_for_emit),context);
                            if (t && (!best || *t > *best + (separate ? 4./units_per_mm : coordinate_error_mm))) { best = t; next_visit = {target, next}; }
                        } else for (const auto& path : boundary->second) {
                            if (direction * (unscale<double>(path.points.front().x()) - entry) < -coordinate_error_mm) continue;
                            // The arc already lies on the original domain boundary.
                            // Check only newly occupied material: subtracting it
                            // from the domain can round unrelated boundary edges
                            // and falsely disconnect otherwise valid scanlines.
                            const double score = -unscale<double>(path.length());
                            if (!best || score > *best + coordinate_error_mm) {
                                best = score; best_boundary = path; next_visit = {target, next};
                            }
                        }
                    }
                }
                if (!best) return;
                const size_t strip = std::min(visit.row, next_visit.row);
                const size_t reservation_id=local_query.reserve(*shape,*best,rows[strip].scaled_y,direction,((next_visit.row>visit.row)!=reverse_for_emit));
                links.push_back({direction * *best,direction,std::move(best_boundary),reservation_id});
                visits.push_back(next_visit);
                if (stabilized) claimed.emplace(next_visit.row,next_visit.scan);
                else rows[next_visit.row].scans[next_visit.scan].used = true;
                direction = -direction;
            }
        };
        grow(1,true);
        std::reverse(visits.begin(), visits.end());
        std::reverse(links.begin(), links.end());
        if (!shape) for (auto& link : links) link.boundary.reverse();
        grow(links.empty() ? -1 : -links.back().direction,false);

        // A Link is one complete optimized bend: both quarter-circles and the
        // connecting segment between them. Only the scan path before the next
        // Link contributes to the stabilization length. Split at an invalid
        // Link so every visited scan still becomes exactly one candidate.
        std::vector<bool> retained(links.size(), false);
        std::vector<std::pair<size_t,size_t>> chunks;
        size_t begin = 0;
        for (size_t j = 0; j < links.size(); ++j) {
            if (stabilized) {
                const auto& row = rows[visits[j].row];
                const auto& scan = row.scans[visits[j].scan];
                const double previous = j == begin ?
                    (links[j].direction > 0 ? scan.lo : scan.hi) : links[j - 1].position;
                // These two contacts lie on the same scanline. Measure the
                // emitted, integer-coordinate path, including rotation error.
                const Point start = coordinates.point(frame * Vec2d(previous, row.y),row.scaled_y);
                const Point finish = coordinates.point(frame * Vec2d(links[j].position, row.y),row.scaled_y);
                const double length = unscale<double>((finish - start).cast<double>().norm());
                if (length < stable_length) {
                    chunks.emplace_back(begin, j + 1);
                    begin = j + 1;
                    continue;
                }
            }
            retained[j] = true;
        }
        chunks.emplace_back(begin, visits.size());

        if (stabilized) for(size_t j=0;j<links.size();++j)if(!retained[j])local_query.discard(links[j].reservation_id);

        const auto emit = [&](size_t first_visit, size_t after_last_visit) {
            FiberInfillPath path;
            struct Identity{size_t id,first,last,arc;};std::vector<Identity> identities;std::set<size_t> self_ids;std::vector<std::pair<size_t,size_t>> expected_arc_indices;
            double station = 0;
            std::vector<Point> arc_centers;
            const auto append_point = [&](const Vec2d& p, std::optional<coord_t> transverse = {}) {
                const Point q = coordinates.point(frame * p,transverse);
                const Point3 next(q.x(), q.y(), coord_t(0));
                if (!path.geometry.points.empty()) {
                    if (path.geometry.points.back() == next) return;
                    station += unscale<double>((next - path.geometry.points.back()).cast<double>().norm());
                }
                path.geometry.points.push_back(next);
            };
            const auto& head = rows[visits[first_visit].row].scans[visits[first_visit].scan];
            append_point({after_last_visit == first_visit + 1 || links[first_visit].direction > 0 ?
                head.lo : head.hi, rows[visits[first_visit].row].y},rows[visits[first_visit].row].scaled_y);
            for (size_t j = first_visit; j + 1 < after_last_visit; ++j) {
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
                size_t return_first=0;const size_t return_arc=path.arcs.size();
                for (size_t k = 0; k < 2; ++k) {
                    const auto indices = shape->arc_indices[k];
                    const coord_t origin_row = rows[visits[j].row].scaled_y;
                    const auto transverse = [&](size_t sample) -> std::optional<coord_t> {
                        const auto offset = return_sample_offset(*shape,sample);
                        return coordinates.canonical() && offset ? std::optional<coord_t>(coordinates.row_offset(origin_row,*offset,step)) : std::nullopt;
                    };
                    append_point(local_point(shape->samples[indices.first]),transverse(indices.first));
                    const size_t known_begin=path.geometry.points.size()-1;if(k==0)return_first=known_begin;
                    ContourArc arc;
                    arc.center_mm = world({0, k == 0 ? radius : pitch - radius});
                    if (coordinates.canonical())
                        arc_centers.push_back(coordinates.point(arc.center_mm,
                            coordinates.row_offset(origin_row,shape->scaled_pitch/2,step)));
                    arc.start_mm = world(shape->samples[indices.first]); arc.end_mm = world(shape->samples[indices.second]);
                    arc.radius_mm = radius; arc.sweep_radians = arc.source_sweep_radians = -link.direction * step * PI / 2;
                    arc.begin_mm = station;
                    for (size_t i = indices.first + 1; i <= indices.second; ++i) append_point(local_point(shape->samples[i]),transverse(i));
                    arc.end_distance_mm = station; path.arcs.push_back(arc);expected_arc_indices.push_back({known_begin,path.geometry.points.size()-1});
                }
                identities.push_back({link.reservation_id,return_first,path.geometry.points.size()-1,return_arc});self_ids.insert(link.reservation_id);
            }
            const auto& tail = rows[visits[after_last_visit - 1].row].scans[visits[after_last_visit - 1].scan];
            append_point({after_last_visit == first_visit + 1 || links[after_last_visit - 2].direction < 0 ?
                tail.hi : tail.lo, rows[visits[after_last_visit - 1].row].y},rows[visits[after_last_visit - 1].row].scaled_y);
            if (path.geometry.points.size() > 1) {
                continuous_fiber_detail::bind_contour_arcs(path.geometry,path.arcs,nullptr,coordinates.canonical() ? &arc_centers : nullptr);
                size_t ai=0;for(const auto&f:path.geometry.fitting_result)if(f.path_type==EMovePathType::Arc_move_cw||f.path_type==EMovePathType::Arc_move_ccw){if(ai>=expected_arc_indices.size()||expected_arc_indices[ai++]!=std::make_pair(f.start_point_index,f.end_point_index))throw std::runtime_error("final bind indices differ from known emitted endpoint");}if(ai!=expected_arc_indices.size())throw std::runtime_error("final bind lost arc");
                for(const auto&i:identities)if(!local_query.identity(i.id,path,i.first,i.last,i.arc))throw std::runtime_error("final return identity differs from actual reserved state");
                std::string failure;
                const auto final_cached=local_clearance::cache_curve(path);
                if(!local_query.certify_material_cached(final_cached,&failure))
                    throw std::runtime_error("LOCAL_FINAL_CERTIFICATE_FAILED:"+failure);
                const bool occupied_clear=local_query.clear_occupied_cached(final_cached,self_ids);
                const bool two_point_linear=path.geometry.points.size()==2 &&
                    path.geometry.points[0]!=path.geometry.points[1] && path.arcs.empty() &&
                    path.geometry.points[0].z()==0 && path.geometry.points[1].z()==0 &&
                    (path.geometry.fitting_result.empty() || (path.geometry.fitting_result.size()==1 &&
                        path.geometry.fitting_result[0].path_type==EMovePathType::Linear_move &&
                        path.geometry.fitting_result[0].start_point_index==0 &&
                        path.geometry.fitting_result[0].end_point_index==1));
                if(!occupied_clear && !two_point_linear)
                    throw std::runtime_error("LOCAL_FINAL_CERTIFICATE_FAILED:actual_occupied_full_curve_overlap");
                // Retain the source. Only the original native process guard decides
                // whether this execution precondition will ever be consumed.
                occupied.push_back({occupied_clear?continuous_fiber_detail::FinalSourceOccupiedKind::Clear:
                    continuous_fiber_detail::FinalSourceOccupiedKind::Collision,
                    occupied_clear?0:local_query.last_actual_collider});
                result.paths.push_back(std::move(path));
            }
        };
        for (const auto& [first_visit, after_last_visit] : chunks)
            emit(first_visit, after_last_visit);
        if (stabilized)
            for (const auto& visit : visits) rows[visit.row].scans[visit.scan].used = true;
    }

    return {std::move(result),std::move(occupied)};
}


} // namespace

FiberInfillCandidates ContinuousFiberFillStrategy::generate_rectilinear(
    const ExPolygons& material, const ContinuousFiberConfig& config,
    double rotation_radians, const Point& grid_origin)
{
    const double radius = config.infill_bend_radius_mm, width = config.infill_flow.width();
    const double stable_length = config.corner_stabilization_length_mm;
    if (!std::isfinite(radius) || radius < 0 || !std::isfinite(width) || width <= 0 ||
        !std::isfinite(config.infill_density) || config.infill_density <= 0 || config.infill_density > 100 ||
        !std::isfinite(rotation_radians) || !std::isfinite(stable_length) || stable_length < 0)
        throw std::invalid_argument("Invalid rectilinear fiber infill parameters");
    if (radius==0.5 && width==1.0 && stable_length==5.0 && config.infill_density==100.0) {
        if (std::fegetround()!=FE_TONEAREST)
            throw std::invalid_argument("Material-certified fiber mode requires nearest rounding");
        auto generated=generate_rectilinear_material_query(material,config,rotation_radians,grid_origin);
        auto result=std::move(generated.candidates);
        result.domain_mode=FiberInfillDomainMode::MaterialCertified2R;
        result.material_certificate=std::shared_ptr<const continuous_fiber_detail::FiberInfillMaterialCertificate>(new continuous_fiber_detail::FiberInfillMaterialCertificate(material,width,result.paths,generated.occupied));
        return result;
    }
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
    // Unsupported coordinates/rounding modes retain the original whole path,
    // before canonicalization or scanline selection. The bound includes O's
    // width sweep as well as every potential translated return.
    const double units_per_mm=return_units_for_scale(SCALING_FACTOR);
    const bool separate = radius > 0 && stable_length > 0 && return_supported(local,radius,pitch,width,pitch_scaled,units_per_mm);
    if (separate) local = diff_ex(local,union_ex(ExPolygons{}));
    std::unique_ptr<ReturnDomain> material_index;
    if (separate) material_index=std::make_unique<ReturnDomain>(local,units_per_mm);
    auto rows = scan_rows(local, scaled(frame.transpose() * mm(grid_origin)).y(), pitch_scaled, config.infill_density == 100);
    const auto edges = separate ? Linesf{} : boundary_lines(local);
    const auto tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    const auto shape = radius > 0 ? std::optional<ReturnShape>(make_return(radius, pitch, pitch_scaled, units_per_mm)) : std::nullopt;
    const bool stabilized = shape && stable_length > 0;
    const auto boundary_links = radius == 0 ? boundary_returns(local, rows, edges, tree) : BoundaryReturns{};
    // Each row interval is visited once. Turns in different strips are separated
    // by the scan grid; independent turns sharing a strip also reserve width.
    struct Strip {
        Polylines pending;
        Polylines tentative;
        ExPolygons occupied, domain;
        Linesf edges;
        AABBTreeIndirect::Tree<2, double> tree;
        std::unique_ptr<ReturnDomain> obstacle_index;
        bool dirty = true;
    };
    std::vector<std::unique_ptr<Strip>> strips(rows.size());
    for (size_t first = 0; first < rows.size(); ++first) for (size_t seed = 0; seed < rows[first].scans.size(); ++seed) {
        if (rows[first].scans[seed].used) continue;
        struct Visit { size_t row, scan; };
        struct Link { double position; int direction; Polyline boundary; size_t strip; Polyline footprint; };
        std::vector<Visit> visits{{first, seed}};
        std::vector<Link> links;
        std::set<std::pair<size_t,size_t>> claimed;
        if (stabilized) claimed.emplace(first,seed);
        else rows[first].scans[seed].used = true;
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
                        if (other.used || (stabilized && claimed.count({target,next}))) continue;
                        const double lo = std::max({direction * entry + (stabilized && !links.empty() ? stable_length : 0.0),
                            direction > 0 ? current.lo : -current.hi,
                            direction > 0 ? other.lo : -other.hi});
                        const double hi = std::min(direction > 0 ? current.hi : -current.lo,
                            direction > 0 ? other.hi : -other.lo);
                        const auto boundary = shape ? boundary_links.end() :
                            boundary_links.find({visit.row, visit.scan, target, next, direction});
                        if (shape ? hi < lo : boundary == boundary_links.end()) continue;
                        // Stabilized chains include tentative returns in the
                        // collision domain; only retained returns are committed.
                        // The zero-length setting keeps the original batching.
                        if (stabilized && strip && strip->dirty) {
                            ExPolygons occupied = strip->occupied;
                            for (const auto& previous : strip->tentative)
                                append(occupied, fiber_contour_coverage(previous, width));
                            if (separate) {
                                auto obstacles=union_ex(occupied);
                                strip->obstacle_index=obstacles.empty()?nullptr:std::make_unique<ReturnDomain>(std::move(obstacles),units_per_mm);
                            } else {
                                strip->domain = diff_ex(local, union_ex(occupied));
                                strip->edges = boundary_lines(strip->domain);
                                strip->tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(strip->edges);
                            }
                            strip->dirty = false;
                        } else if (!stabilized && strip && !strip->pending.empty()) {
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
                            const double row=rows[std::min(visit.row,target)].y;
                            auto t = separate ? place_return_separate(*shape,*material_index,
                                strip?strip->obstacle_index.get():nullptr,rows[std::min(visit.row,target)].scaled_y,direction,lo,hi) :
                                place_return(*shape,strip?strip->domain:local,
                                    strip?strip->edges:edges,strip?strip->tree:tree,row,direction,lo,hi);
                            if (t && (!best || *t > *best + (separate ? 4./units_per_mm : coordinate_error_mm))) { best = t; next_visit = {target, next}; }
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
                Polyline footprint;
                if (stabilized) {
                    footprint = translated_return(*shape, *best, rows[strip].y, direction);
                    strips[strip]->tentative.push_back(footprint);
                    strips[strip]->dirty = true;
                } else strips[strip]->pending.push_back(
                    shape ? translated_return(*shape, *best, rows[strip].y, direction) : best_boundary);
                links.push_back({shape ? direction * *best : 0, direction,
                    std::move(best_boundary), strip, std::move(footprint)});
                visits.push_back(next_visit);
                if (stabilized) claimed.emplace(next_visit.row,next_visit.scan);
                else rows[next_visit.row].scans[next_visit.scan].used = true;
                direction = -direction;
            }
        };
        grow(1);
        std::reverse(visits.begin(), visits.end());
        std::reverse(links.begin(), links.end());
        if (!shape) for (auto& link : links) link.boundary.reverse();
        grow(links.empty() ? -1 : -links.back().direction);

        // A Link is one complete optimized bend: both quarter-circles and the
        // connecting segment between them. Only the scan path before the next
        // Link contributes to the stabilization length. Split at an invalid
        // Link so every visited scan still becomes exactly one candidate.
        std::vector<bool> retained(links.size(), false);
        std::vector<std::pair<size_t,size_t>> chunks;
        size_t begin = 0;
        for (size_t j = 0; j < links.size(); ++j) {
            if (stabilized) {
                const auto& row = rows[visits[j].row];
                const auto& scan = row.scans[visits[j].scan];
                const double previous = j == begin ?
                    (links[j].direction > 0 ? scan.lo : scan.hi) : links[j - 1].position;
                // These two contacts lie on the same scanline. Measure the
                // emitted, integer-coordinate path, including rotation error.
                const Point start = scaled(frame * Vec2d(previous, row.y));
                const Point finish = scaled(frame * Vec2d(links[j].position, row.y));
                const double length = unscale<double>((finish - start).cast<double>().norm());
                if (length < stable_length) {
                    chunks.emplace_back(begin, j + 1);
                    begin = j + 1;
                    continue;
                }
            }
            retained[j] = true;
        }
        chunks.emplace_back(begin, visits.size());

        if (stabilized) {
            std::set<size_t> changed;
            for (const auto& link : links) changed.insert(link.strip);
            for (size_t index : changed) strips[index]->tentative.clear();
            for (size_t j = 0; j < links.size(); ++j) if (retained[j])
                append(strips[links[j].strip]->occupied, fiber_contour_coverage(links[j].footprint, width));
            for (size_t index : changed) {
                strips[index]->occupied = union_ex(strips[index]->occupied);
                strips[index]->dirty = true;
            }
        }

        const auto emit = [&](size_t first_visit, size_t after_last_visit) {
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
            const auto& head = rows[visits[first_visit].row].scans[visits[first_visit].scan];
            append_point({after_last_visit == first_visit + 1 || links[first_visit].direction > 0 ?
                head.lo : head.hi, rows[visits[first_visit].row].y});
            for (size_t j = first_visit; j + 1 < after_last_visit; ++j) {
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
            const auto& tail = rows[visits[after_last_visit - 1].row].scans[visits[after_last_visit - 1].scan];
            append_point({after_last_visit == first_visit + 1 || links[after_last_visit - 2].direction < 0 ?
                tail.hi : tail.lo, rows[visits[after_last_visit - 1].row].y});
            if (path.geometry.points.size() > 1) {
                continuous_fiber_detail::bind_contour_arcs(path.geometry,path.arcs);
                result.paths.push_back(std::move(path));
            }
        };
        for (const auto& [first_visit, after_last_visit] : chunks)
            emit(first_visit, after_last_visit);
        if (stabilized)
            for (const auto& visit : visits) rows[visit.row].scans[visit.scan].used = true;
    }
    return result;
}
} // namespace Slic3r
