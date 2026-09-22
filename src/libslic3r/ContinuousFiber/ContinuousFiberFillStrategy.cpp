#include "ContinuousFiberFillStrategy.hpp"

#include "../ClipperUtils.hpp"
#include "../AABBTreeLines.hpp"
#include "../Geometry.hpp"
#include "../Geometry/ArcWelder.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <stdexcept>
#include <nlopt.hpp>
#include <functional>
#include <variant>
#include <queue>

namespace Slic3r {

namespace {
using namespace continuous_fiber_detail;
constexpr double straight_tolerance_mm = 0.00002;
constexpr size_t maximum_samples = 200000;
Vec2d left_normal(const Vec2d& v) { return {-v.y(), v.x()}; }
double turn(const Vec2d& a, const Vec2d& b) { return std::atan2(cross2(a,b), a.dot(b)); }
Point scaled_point(const Vec2d& p) { return Point::new_scale(p.x(), p.y()); }
Vec2d mm_point(const Point& p) { return {unscale<double>(p.x()), unscale<double>(p.y())}; }

std::vector<Vec2d> canonical_ring(const Polyline& source)
{
    Points points = source.points;
    if (points.size() > 1 && points.front() == points.back()) points.pop_back();
    points.erase(std::unique(points.begin(), points.end()), points.end());
    Polygon polygon(points);
    polygon.make_counter_clockwise();
    std::vector<Vec2d> p;
    for (const Point& point : polygon.points) p.push_back(mm_point(point));
    const auto rotate_start = [&] {
        const auto first = std::min_element(p.begin(), p.end(), [](const Vec2d& a, const Vec2d& b) {
            return a.x() == b.x() ? a.y() < b.y() : a.x() < b.x();
        });
        std::rotate(p.begin(), first, p.end());
    };
    rotate_start();
    const auto original=p;
    std::vector<size_t> retained;
    for (size_t i=0;i<p.size();++i) retained.push_back(i);
    bool changed = true;
    while (changed && p.size() > 3) {
        changed = false;
        for (size_t i = 0; i < p.size(); ++i) {
            const Vec2d u = p[i]-p[(i+p.size()-1)%p.size()];
            const Vec2d v = p[(i+1)%p.size()]-p[i];
            if (u.norm() < 1e-9 || (u.dot(v) > 0 &&
                std::abs(cross2(u,v))/(u+v).norm() <= straight_tolerance_mm)) {
                const size_t before=(i+p.size()-1)%p.size(),after=(i+1)%p.size();
                const Vec2d a=p[before],delta=p[after]-a;
                bool within=delta.squaredNorm()>1e-18;
                for (size_t j=(retained[before]+1)%original.size();within && j!=retained[after];j=(j+1)%original.size()) {
                    const Vec2d offset=original[j]-a;
                    const double t=std::clamp(offset.dot(delta)/delta.squaredNorm(),0.0,1.0);
                    within=(offset-t*delta).norm()<=straight_tolerance_mm;
                }
                if (!within) continue;
                p.erase(p.begin()+i); retained.erase(retained.begin()+i); changed = true; break;
            }
        }
    }
    rotate_start();
    return p;
}

bool inside(const Polyline& path, const ExPolygons& domain)
{
    return diff_pl(Polylines{path}, domain).empty();
}

bool simple(const Points& p)
{
    std::vector<size_t> edges;
    for (size_t i=1;i<p.size();++i) edges.push_back(i);
    std::stable_sort(edges.begin(),edges.end(),[&](size_t a,size_t b) {
        return std::min(p[a-1].x(),p[a].x())<std::min(p[b-1].x(),p[b].x());
    });
    for (size_t a=0;a<edges.size();++a) {
        const size_t i=edges[a];
        for (size_t b=a+1;b<edges.size();++b) {
            const size_t j=edges[b];
            if (std::max(p[i-1].x(),p[i].x())<std::min(p[j-1].x(),p[j].x())) break;
            if (i+1==j || j+1==i || (p.front()==p.back() && std::min(i,j)==1 && std::max(i,j)==p.size()-1)) continue;
            if (std::max(p[i-1].y(),p[i].y())<std::min(p[j-1].y(),p[j].y()) ||
                std::max(p[j-1].y(),p[j].y())<std::min(p[i-1].y(),p[i].y())) continue;
            if (Geometry::segments_intersect(p[i-1],p[i],p[j-1],p[j])) return false;
        }
    }
    return true;
}

struct TangentSupports {
    Vec2d a, c, u, v, base, slope;
    double incoming_length, outgoing_length, angle;
};
std::optional<TangentSupports> supports(const std::vector<Vec2d>& p, CornerGroup g)
{
    const size_t n=p.size(), last=(g.first+g.count-1)%n;
    TangentSupports s;
    s.a=p[(g.first+n-1)%n]; s.c=p[last];
    s.u=p[g.first]-s.a; s.v=p[(last+1)%n]-s.c;
    s.incoming_length=s.u.norm(); s.outgoing_length=s.v.norm();
    if (s.incoming_length < 1e-9 || s.outgoing_length < 1e-9) return {};
    s.u/=s.incoming_length; s.v/=s.outgoing_length;
    s.angle=turn(s.u,s.v);
    if (std::abs(s.angle)<1e-7 || std::abs(s.angle)>PI-1e-7) return {};
    const auto solve = [&](const Vec2d& b) -> Vec2d {
        return {cross2(b,s.v)/cross2(s.u,s.v), cross2(b,s.u)/cross2(s.u,s.v)};
    };
    s.base=solve(s.c-s.a);
    s.slope=solve(std::copysign(1.0,s.angle)*(left_normal(s.v)-left_normal(s.u)));
    return s;
}

std::optional<std::pair<double,double>> radius_interval(const TangentSupports& s, double minimum,
    double incoming_min=0,double outgoing_max=HUGE_VAL)
{
    double lo=minimum, hi=std::numeric_limits<double>::infinity();
    for (size_t i=0; i<2; ++i) {
        const double z=s.base[i], k=s.slope[i];
        const double lower=i==0?std::max(0.0,incoming_min):0.0;
        const double upper=i==0?s.incoming_length:std::min(s.outgoing_length,outgoing_max);
        if (upper<lower) return {};
        if (std::abs(k)<1e-12) { if (z < lower-1e-8 || z>upper+1e-8) return {}; }
        else {
            const double a=(lower-z)/k, b=(upper-z)/k;
            lo=std::max(lo,std::min(a,b)); hi=std::min(hi,std::max(a,b));
        }
    }
    if (!std::isfinite(hi) || hi<lo) return {};
    return {{lo,hi}};
}

TangentSolution circle(const TangentSupports& s, double radius)
{
    const Vec2d ab=s.base+radius*s.slope;
    TangentSolution result;
    result.incoming_remaining=ab.x(); result.outgoing_consumed=ab.y();
    result.arcs.emplace_back();
    auto& arc=result.arcs.back();
    arc.start_mm=s.a+ab.x()*s.u; arc.end_mm=s.c+ab.y()*s.v;
    arc.center_mm=arc.start_mm+std::copysign(radius,s.angle)*left_normal(s.u);
    arc.radius_mm=radius; arc.sweep_radians=s.angle;
    return result;
}

Vec2d arc_point(const ContourArc& arc, double t)
{
    if (t==0) return arc.start_mm;
    if (t==1) return arc.end_mm;
    const Vec2d radial=arc.start_mm-arc.center_mm;
    const double a=std::atan2(radial.y(),radial.x())+t*arc.sweep_radians;
    return arc.center_mm+arc.radius_mm*Vec2d(std::cos(a),std::sin(a));
}
Polyline discretize(const ContourArc& arc, double tolerance)
{
    const size_t n=std::max(size_t(1),Geometry::ArcWelder::arc_discretization_steps(
        arc.radius_mm,std::abs(arc.sweep_radians),tolerance));
    if (n>maximum_samples) throw std::length_error("Contour arc exceeds sampling budget");
    Polyline line;
    for (size_t i=0;i<=n;++i) {
        const Point p=scaled_point(arc_point(arc,double(i)/n));
        if (line.points.empty() || line.points.back()!=p) line.points.push_back(p);
    }
    return line;
}

// Contacts are computed analytically. Sampling converts common tangents into
// line primitives and omits degenerate arcs.
ContourArc connecting_arc(const Vec2d& center, const Vec2d& a, const Vec2d& b,
                          double radius, int direction)
{
    ContourArc arc;
    arc.center_mm=center; arc.start_mm=a; arc.end_mm=b;
    arc.radius_mm=radius;
    double angle=direction*turn(a-center,b-center);
    if (angle < -1e-10) angle+=2*PI;
    arc.sweep_radians=direction*std::max(0.0,angle);
    return arc;
}

using ContourPrimitive=std::variant<Linef,ContourArc>;

std::vector<ContourPrimitive> connection_primitives(const TangentSolution& value)
{
    std::vector<ContourPrimitive> pieces;
    for (size_t i=0;i<value.arcs.size();++i) {
        const auto& arc=value.arcs[i];
        if (i && (value.arcs[i-1].end_mm-arc.start_mm).norm()>1e-9)
            pieces.emplace_back(Linef(value.arcs[i-1].end_mm,arc.start_mm));
        if (std::abs(arc.sweep_radians)*arc.radius_mm>1e-9) pieces.emplace_back(arc);
    }
    return pieces;
}

Polyline sample_connection(const TangentSolution& value, double tolerance)
{
    Polyline line;
    const auto append=[&](const Point& p) {
        if (line.points.empty() || line.points.back()!=p) line.points.push_back(p);
        if (line.points.size()>maximum_samples) throw std::length_error("Contour exceeds sampling budget");
    };
    for (const auto& piece:connection_primitives(value)) {
        if (const auto* segment=std::get_if<Linef>(&piece)) {
            append(scaled_point(segment->a));append(scaled_point(segment->b));
        } else for (const auto& p:discretize(std::get<ContourArc>(piece),tolerance).points) append(p);
    }
    return line;
}

// A connection is C, CSC or CCC. The supports and all three radii are
// independent; Rmin is a lower bound, not the radius of every arc.
struct ConnectionFamily { int first, last, branch; }; // branch == 0: CSC
struct ConnectionProblem {
    Vec2d entry, exit, u, v;
    double incoming_length, outgoing_length, minimum_radius;
    double turn_radians=0, absolute_turn=0;
    double heading_min=0, heading_max=0;
};
std::optional<TangentSolution> connection(const ConnectionProblem& p,
    ConnectionFamily family, const std::vector<double>& x, double& violation)
{
    const double retreat=x[0]*p.minimum_radius, advance=x[1]*p.minimum_radius;
    const double r0=x[2]*p.minimum_radius, rm=(family.branch==0?0:x[3])*p.minimum_radius, r1=x.back()*p.minimum_radius;
    const Vec2d a=p.entry-retreat*p.u, b=p.exit+advance*p.v;
    const Vec2d c0=a+family.first*r0*left_normal(p.u);
    const Vec2d c1=b+family.last*r1*left_normal(p.v), delta=c1-c0;
    const double d=delta.norm();
    TangentSolution result{{},p.incoming_length-retreat,advance};
    violation=0;
    if (family.branch==0) {
        const double normal=family.last*r1-family.first*r0;
        violation=std::max(0.0,std::abs(normal)-d);
        if (violation>0 || d<1e-10) { violation=std::max(violation,1e-8); return {}; }
        const double angle=std::atan2(delta.y(),delta.x())-std::asin(std::clamp(normal/d,-1.0,1.0));
        const Vec2d tangent(std::cos(angle),std::sin(angle));
        const Vec2d t0=c0-family.first*r0*left_normal(tangent),t1=c1-family.last*r1*left_normal(tangent);
        result.arcs={connecting_arc(c0,a,t0,r0,family.first),
                     connecting_arc(c1,t1,b,r1,family.last)};
    } else {
        const double a_radius=r0+rm,b_radius=r1+rm;
        violation=std::max({0.0,d-a_radius-b_radius,std::abs(a_radius-b_radius)-d});
        if (violation>0 || d<1e-10) { violation=std::max(violation,1e-8); return {}; }
        const double along=(d*d+a_radius*a_radius-b_radius*b_radius)/(2*d);
        const double height=std::sqrt(std::max(0.0,a_radius*a_radius-along*along));
        const Vec2d middle=c0+along*delta/d+family.branch*height*left_normal(delta/d);
        const Vec2d t0=(rm*c0+r0*middle)/(r0+rm),t1=(rm*c1+r1*middle)/(r1+rm);
        result.arcs={connecting_arc(c0,a,t0,r0,family.first),
                     connecting_arc(middle,t0,t1,rm,-family.first),
                     connecting_arc(c1,t1,b,r1,family.last)};
    }
    return result;
}

double connection_length(const TangentSolution& value)
{
    double length=0;
    for (size_t i=0;i<value.arcs.size();++i) {
        length+=value.arcs[i].radius_mm*std::abs(value.arcs[i].sweep_radians);
        if (i) length+=(value.arcs[i].start_mm-value.arcs[i-1].end_mm).norm();
    }
    return length;
}

// Signed distance gives the local optimizer a direction even when a candidate
// crosses a boundary. Clipper and topology checks remain the acceptance test.
struct DomainDistance {
    struct Edge { Vec2d a, delta; double length_squared; };
    std::vector<Edge> edges;
    Linesf lines;
    AABBTreeIndirect::Tree<2,double> tree;
    explicit DomainDistance(const ExPolygons& domain) {
        const auto append=[&](const Polygon& ring) {
            for (size_t i=0;i<ring.points.size();++i) {
                const Vec2d a=mm_point(ring.points[i]),d=mm_point(ring.points[(i+1)%ring.points.size()])-a;
                if (d.squaredNorm()>0) edges.push_back({a,d,d.squaredNorm()});
            }
        };
        for (const auto& region:domain) { append(region.contour); for (const auto& h:region.holes) append(h); }
        lines.reserve(edges.size());
        for (const auto& edge:edges) lines.emplace_back(edge.a,edge.a+edge.delta);
        tree=AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    }
    double outside_distance(const Vec2d& p) const {
        size_t nearest=size_t(-1);Vec2d point=Vec2d::Zero();
        const double squared=AABBTreeLines::squared_distance_to_indexed_lines(lines,tree,p,nearest,point);
        return squared<0?HUGE_VAL:std::sqrt(squared)*AABBTreeLines::point_outside_closed_contours(lines,tree,p);
    }
    void nearby_edges(const Vec2d& center,double radius,std::vector<size_t>& found) const {
        // A vertex or perpendicular foot within the radial band must lie in
        // this box. Expand conservatively for floating-point boundary contacts.
        const double roundoff=8*std::numeric_limits<double>::epsilon()*(radius+center.cwiseAbs().maxCoeff());
        const Vec2d extent=Vec2d::Constant(radius+std::max(1e-9,roundoff));
        const Eigen::AlignedBox<double,2> box(center-extent,center+extent);
        found.clear();
        AABBTreeIndirect::traverse(tree,AABBTreeIndirect::intersecting(box),[&](const auto& node) {
            found.push_back(node.idx);return true;
        });
        // Keep the original evaluation order, including ties on the boundary.
        std::sort(found.begin(),found.end());
    }
};


double point_segment_distance_squared(const Vec2d& p,const Vec2d& a,const Vec2d& b)
{
    const Vec2d d=b-a;
    return (p-a-std::clamp((p-a).dot(d)/std::max(d.squaredNorm(),1e-20),0.0,1.0)*d).squaredNorm();
}

// A local rounding must preserve the source's direction of progress. If the
// source tangents fit in a half-plane, the connector and source must share such
// a half-plane too. This excludes winding branches without an arbitrary cap on
// total turning: a necessary S bend may still turn in both directions. Windows
// which genuinely turn back retain their original (unwrapped) heading envelope.
double turn_violation(const TangentSolution& value,double source_turn,
                      double source_min,double source_max)
{
    double heading=0,lo=source_min,hi=source_max;
    for (const auto& arc:value.arcs) {
        heading+=arc.sweep_radians;
        lo=std::min(lo,heading); hi=std::max(hi,heading);
    }
    return std::max(std::abs(heading-source_turn)-1e-6,
                    hi-lo-std::max(PI,source_max-source_min)-1e-6);
}

double source_deviation(const TangentSolution& value,const std::vector<Vec2d>& knots)
{
    double error=0;
    for (const auto& p:knots) {
        double best=HUGE_VAL;
        for (size_t k=0;k<value.arcs.size();++k) {
            const auto& arc=value.arcs[k];
            best=std::min({best,(p-arc.start_mm).squaredNorm(),(p-arc.end_mm).squaredNorm()});
            const Vec2d radial=p-arc.center_mm;
            double angle=std::copysign(1.0,arc.sweep_radians)*turn(arc.start_mm-arc.center_mm,radial);
            if (angle<0) angle+=2*PI;
            if (angle<=std::abs(arc.sweep_radians)) {
                const double d=radial.norm()-arc.radius_mm;best=std::min(best,d*d);
            }
            if (k) best=std::min(best,point_segment_distance_squared(p,value.arcs[k-1].end_mm,arc.start_mm));
        }
        error+=best;
    }
    error/=std::max(size_t(1),knots.size());
    double reverse_error=0;size_t samples=0;
    const auto inspect=[&](const Vec2d& point) {
        double best=point_segment_distance_squared(point,value.arcs.front().start_mm,knots.front());
        for (size_t i=1;i<knots.size();++i) best=std::min(best,point_segment_distance_squared(point,knots[i-1],knots[i]));
        best=std::min(best,point_segment_distance_squared(point,knots.back(),value.arcs.back().end_mm));
        reverse_error+=best;++samples;
    };
    for (const auto& arc:value.arcs) for (size_t i=1;i<8;++i) inspect(arc_point(arc,double(i)/8));
    for (size_t i=1;i<value.arcs.size();++i) inspect((value.arcs[i-1].end_mm+value.arcs[i].start_mm)*.5);
    return error+reverse_error/std::max(size_t(1),samples);
}

// Use the same objective for optimization, candidate pruning and the cycle.
// Subtract the replaced source length so windows of different sizes compare
// consistently; support occupation alone is not a path quality score.
double connection_score(const TangentSolution& value,const std::vector<Vec2d>& knots,
                        double radius,double incoming_length,double source_variation)
{
    double replaced=incoming_length-value.incoming_remaining+value.outgoing_consumed;
    for (size_t i=1;i<knots.size();++i) replaced+=(knots[i]-knots[i-1]).norm();
    double variation=0;
    for (const auto& arc:value.arcs) variation+=std::abs(arc.sweep_radians);
    const double extra_turn=std::max(0.0,variation-source_variation);
    return source_deviation(value,knots)/(radius*radius)+extra_turn*extra_turn+
        .001*(connection_length(value)-replaced)/radius;
}

bool same_connection(const TangentSolution& a,const TangentSolution& b)
{
    if (a.arcs.size()!=b.arcs.size() || std::abs(a.incoming_remaining-b.incoming_remaining)>1e-8 ||
        std::abs(a.outgoing_consumed-b.outgoing_consumed)>1e-8) return false;
    for (size_t i=0;i<a.arcs.size();++i)
        if ((a.arcs[i].center_mm-b.arcs[i].center_mm).norm()>1e-8 ||
            (a.arcs[i].start_mm-b.arcs[i].start_mm).norm()>1e-8 ||
            (a.arcs[i].end_mm-b.arcs[i].end_mm).norm()>1e-8 ||
            std::abs(a.arcs[i].sweep_radians-b.arcs[i].sweep_radians)>1e-8) return false;
    return true;
}

void prune_candidates(LocalSolutions& solutions)
{
    constexpr size_t capacity=48;
    auto& values=solutions.values;
    std::stable_sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.score<b.score;});
    if (values.size()<=capacity) return;
    // Preserve the support/quality Pareto frontier before filling spare slots
    // with other shapes. A lower score alone cannot dominate a connection.
    std::vector<size_t> frontier,remaining;
    for (size_t i=0;i<values.size();++i) {
        bool dominated=false;
        for (size_t j=0;j<i && !dominated;++j)
            dominated=values[j].incoming_remaining>=values[i].incoming_remaining &&
                values[j].outgoing_consumed<=values[i].outgoing_consumed;
        (dominated?remaining:frontier).push_back(i);
    }
    if (frontier.size()>capacity) {
        // A finite search cannot retain an arbitrarily large frontier. Keep
        // both support extremes and spread the remaining budget across ports.
        std::vector<size_t> selected{frontier.front()};
        const auto keep=[&](size_t i) {
            if (std::find(selected.begin(),selected.end(),i)==selected.end()) selected.push_back(i);
        };
        keep(*std::max_element(frontier.begin(),frontier.end(),[&](size_t a,size_t b){return values[a].incoming_remaining<values[b].incoming_remaining;}));
        keep(*std::min_element(frontier.begin(),frontier.end(),[&](size_t a,size_t b){return values[a].outgoing_consumed<values[b].outgoing_consumed;}));
        while (selected.size()<capacity) {
            size_t best=frontier.front();double separation=-1;
            for (size_t i:frontier) {
                double nearest=HUGE_VAL;
                for (size_t j:selected) nearest=std::min(nearest,
                    std::hypot(values[i].incoming_remaining-values[j].incoming_remaining,
                               values[i].outgoing_consumed-values[j].outgoing_consumed));
                if (nearest>separation) {separation=nearest;best=i;}
            }
            if (std::find(selected.begin(),selected.end(),best)!=selected.end()) break;
            selected.push_back(best);
        }
        frontier=std::move(selected);
    }
    for (size_t i:remaining) if (frontier.size()<capacity) frontier.push_back(i);
    std::sort(frontier.begin(),frontier.end());
    std::vector<TangentSolution> kept;kept.reserve(frontier.size());
    for (size_t i:frontier) kept.push_back(std::move(values[i]));
    values=std::move(kept);
    solutions.candidates_pruned=true; // A later failure is not a proof of infeasibility.
}

LocalSolutions solve_group(const std::vector<Vec2d>& p, CornerGroup group,
    const ContourRoundingOptions& options, const ExPolygons& domain, std::optional<DomainDistance>& distances,
    double incoming_min=0,double outgoing_max=HUGE_VAL)
{
    LocalSolutions result;
    std::vector<Vec2d> knots;
    double source_turn=0,absolute_turn=0,heading_min=0,heading_max=0;
    for (size_t k=0;k<group.count;++k) {
        const size_t i=(group.first+k)%p.size();
        knots.push_back(p[i]);
        const double angle=turn(p[i]-p[(i+p.size()-1)%p.size()],p[(i+1)%p.size()]-p[i]);
        source_turn+=angle;absolute_turn+=std::abs(angle);
        heading_min=std::min(heading_min,source_turn); heading_max=std::max(heading_max,source_turn);
    }
    const auto feasible=[&](const TangentSolution& value) {
        if (value.incoming_remaining<incoming_min-1e-8 || value.outgoing_consumed>outgoing_max+1e-8) return false;
        if (turn_violation(value,source_turn,heading_min,heading_max)>1e-8) return false;
        const auto line=sample_connection(value,options.chord_tolerance_mm);
        return line.points.size()>1 && inside(line,domain) && simple(line.points);
    };
    const auto add=[&](TangentSolution value,std::optional<double> known_score=std::nullopt) {
        // Repeated optimizer poses must not repeat clipping and self-intersection checks.
        for (const auto& old:result.values) if (same_connection(old,value)) return;
        if (!feasible(value)) return;
        const double incoming_length=(p[group.first]-p[(group.first+p.size()-1)%p.size()]).norm();
        value.score=known_score?*known_score:connection_score(value,knots,options.minimum_radius_mm,incoming_length,absolute_turn);
        result.values.push_back(std::move(value));
    };
    if (const auto s=supports(p,group))
        if (const auto interval=radius_interval(*s,options.minimum_radius_mm,incoming_min,outgoing_max)) {
            add(circle(*s,interval->first));
            if (group.count>1 && interval->second-interval->first>1e-10) {
                // Do not assume a unimodal objective. Sample the entire support
                // interval, then refine around the best sampled score. Retain
                // distinct valid poses so neighbouring windows still have choices.
                constexpr size_t samples=16;
                const double step=(interval->second-interval->first)/samples;
                double best_radius=interval->first,best_score=HUGE_VAL;
                const double incoming_length=s->incoming_length;
                const auto inspect=[&](double radius) {
                    auto value=circle(*s,radius);
                    const double score=connection_score(value,knots,options.minimum_radius_mm,incoming_length,absolute_turn);
                    if (score<best_score) { best_score=score;best_radius=radius; }
                    add(std::move(value),score);
                };
                for (size_t i=0;i<=samples;++i) inspect(interval->first+i*step);
                double width=step;
                for (size_t pass=0;pass<3;++pass) {
                    const double lo=std::max(interval->first,best_radius-width);
                    const double hi=std::min(interval->second,best_radius+width);
                    for (size_t i=1;i<samples;++i) inspect(lo+(hi-lo)*double(i)/samples);
                    width=(hi-lo)/samples;
                }
            }
            if (!result.values.empty()) { prune_candidates(result);return result; }
        }
    const size_t n=p.size(),last=(group.first+group.count-1)%n;
    ConnectionProblem problem;
    problem.entry=p[group.first]; problem.exit=p[last];
    problem.u=problem.entry-p[(group.first+n-1)%n]; problem.v=p[(last+1)%n]-problem.exit;
    problem.incoming_length=problem.u.norm(); problem.outgoing_length=problem.v.norm();
    problem.minimum_radius=options.minimum_radius_mm;
    problem.turn_radians=source_turn;problem.absolute_turn=absolute_turn;
    problem.heading_min=heading_min;problem.heading_max=heading_max;
    if (problem.incoming_length<1e-9 || problem.outgoing_length<1e-9) return result;
    problem.u/=problem.incoming_length; problem.v/=problem.outgoing_length;
    const double half=std::abs(turn(problem.u,problem.v))*.5,sine=std::sin(half);
    const double anchor=sine+std::sqrt(sine*sine+2*(1-std::cos(half)));
    const std::vector<double> upper={(problem.incoming_length-incoming_min)/options.minimum_radius_mm,
        std::min(problem.outgoing_length,outgoing_max)/options.minimum_radius_mm,HUGE_VAL,HUGE_VAL,HUGE_VAL};
    if (upper[0]<1e-8 || upper[1]<1e-8) return result;
    struct Search {
        const ConnectionProblem* problem;
        ConnectionFamily family;
        const DomainDistance* distances;
        double tolerance;
        const std::vector<Vec2d>* knots;
        std::function<void(TangentSolution)> retain;
        mutable std::vector<size_t> nearby;
        mutable std::vector<double> last_parameters;
        mutable std::optional<TangentSolution> last_connection;
        mutable double last_violation=0;
        const std::optional<TangentSolution>& evaluate(const std::vector<double>& x) const {
            // COBYLA evaluates objective and constraint at the same pose. Share
            // only that exact pose, within this optimizer run and this thread.
            if (x!=last_parameters) {
                last_connection=connection(*problem,family,x,last_violation);
                last_parameters=x;
            }
            return last_connection;
        }
        double constraint(const std::vector<double>& x) const {
            const auto& value=evaluate(x);
            if (!value) return 1+last_violation/problem->minimum_radius;
            const double winding_error=turn_violation(*value,problem->turn_radians,problem->heading_min,problem->heading_max);
            if (winding_error>1e-8) return winding_error;
            double outside=-HUGE_VAL;
            for (size_t k=0;k<value->arcs.size();++k) {
                const auto& arc=value->arcs[k];
                // Search samples are augmented at boundary vertices and edge normals.
                // Final acceptance uses the error-bounded discretization.
                const size_t count=32;
                for (size_t i=0;i<=count;++i)
                    outside=std::max(outside,distances->outside_distance(arc_point(arc,double(i)/count)));
                const auto inspect_direction=[&](const Vec2d& radial) {
                    const double direction=std::copysign(1.0,arc.sweep_radians);
                    double angle=direction*turn(arc.start_mm-arc.center_mm,radial);
                    if (angle<0) angle+=2*PI;
                    if (angle<=std::abs(arc.sweep_radians) && radial.squaredNorm()>1e-18)
                        outside=std::max(outside,distances->outside_distance(arc.center_mm+arc.radius_mm*radial.normalized()));
                };
                const double band=arc.radius_mm*std::abs(arc.sweep_radians)/count;
                distances->nearby_edges(arc.center_mm,arc.radius_mm+band,nearby);
                for (size_t edge_index:nearby) {
                    const auto& edge=distances->edges[edge_index];
                    const Vec2d radial=edge.a-arc.center_mm;
                    if (std::abs(radial.norm()-arc.radius_mm)<=band) inspect_direction(radial);
                    const double t=(-radial).dot(edge.delta)/edge.length_squared;
                    if (t>=0 && t<=1) {
                        const Vec2d normal=radial+t*edge.delta;
                        if (std::abs(normal.norm()-arc.radius_mm)<=band) inspect_direction(normal);
                    }
                }
                if (k) {
                    const Vec2d a=value->arcs[k-1].end_mm,b=arc.start_mm;
                    for (size_t i=1;i<16;++i)
                        outside=std::max(outside,distances->outside_distance(a+(b-a)*(double(i)/16)));
                }
            }
            if (outside<=0) retain(*value);
            return (outside+tolerance)/problem->minimum_radius;
        }
        static double boundary(const std::vector<double>& x,std::vector<double>&,void* data) {
            return static_cast<Search*>(data)->constraint(x);
        }
        static double objective(const std::vector<double>& x,std::vector<double>&,void* data) {
            const auto& self=*static_cast<Search*>(data);
            const auto& value=self.evaluate(x);
            if (!value) return 100+self.last_violation/self.problem->minimum_radius;
            return connection_score(*value,*self.knots,self.problem->minimum_radius,self.problem->incoming_length,self.problem->absolute_turn);
        }
    };
    // A wedge has a closed-form equal-radius solution. Check all branches
    // before invoking the numerical solver for coupled/irregular windows.
    for (int first:{1,-1}) for (int branch:{1,-1}) {
        std::vector<double> x={std::min(upper[0],anchor+2*options.geometry_tolerance_mm/options.minimum_radius_mm),
            std::min(upper[1],anchor+2*options.geometry_tolerance_mm/options.minimum_radius_mm),1,1,1};
        double violation;
        if (auto value=connection(problem,{first,first,branch},x,violation)) add(*value);
    }
    // An isolated wedge uses the analytic symmetric construction. For coupled
    // turns that construction is only a seed, not an acceptable stopping rule.
    if (group.count==1 && !result.values.empty()) return result;
    if (!distances) distances.emplace(domain);
    for (int first:{1,-1}) for (int kind=0;kind<4;++kind) {
        ConnectionFamily family{first,kind<2?(kind==0?first:-first):first,kind<2?0:(kind==2?1:-1)};
        // Two geometric poses: wedge tangency and the middle of each available
        // support. Optimization moves the two contacts independently.
        for (int seed=0;seed<2;++seed) {
            std::vector<double> x={std::min(upper[0],anchor),std::min(upper[1],anchor),1,1,1};
            if (seed) { x[0]=upper[0]*.5; x[1]=upper[1]*.5; }
            Search search{&problem,family,&*distances,options.chord_tolerance_mm*.5,&knots,add};
            const size_t dimensions=family.branch==0?4:5;
            x.resize(dimensions);
            auto bounds=upper;bounds.resize(dimensions);
            std::vector<double> lower(dimensions,1.0);lower[0]=lower[1]=0;
            nlopt::opt optimizer(nlopt::LN_COBYLA,unsigned(dimensions));
            optimizer.set_lower_bounds(lower); optimizer.set_upper_bounds(bounds);
            optimizer.set_min_objective(Search::objective,&search);
            optimizer.add_inequality_constraint(Search::boundary,&search,1e-8);
            std::vector<double> step(dimensions,.25);
            step[0]=std::min(.5,upper[0]*.25);step[1]=std::min(.5,upper[1]*.25);
            optimizer.set_initial_step(step);
            optimizer.set_xtol_abs(1e-5); optimizer.set_maxeval(180);
            double score;
            try { result.optimizer_limit_reached|=optimizer.optimize(x,score)==nlopt::MAXEVAL_REACHED; }
            catch (const nlopt::roundoff_limited&) { result.numerical_failure=true; }
            double violation;
            if (auto value=connection(problem,family,x,violation)) add(*value);
        }
    }
    prune_candidates(result);
    return result;
}

// Adjacent turns share a finite support. Form connected conflict windows before
// searching, including a turn whose nominal single arc falls outside the domain.
std::vector<CornerGroup> conflict_windows(const std::vector<Vec2d>& p,
    const ContourRoundingOptions& options,const ExPolygons& domain)
{
    const size_t n=p.size();
    std::vector<double> retreat(n);
    std::vector<bool> linked(n);
    for (size_t i=0;i<n;++i) {
        const double angle=turn(p[i]-p[(i+n-1)%n],p[(i+1)%n]-p[i]);
        const double half=std::min(std::abs(angle),PI-1e-7)*.5;
        retreat[i]=options.minimum_radius_mm*std::tan(half);
        if (const auto s=supports(p,{i,1})) {
            const auto value=circle(*s,options.minimum_radius_mm);
            if (!inside(sample_connection(value,options.chord_tolerance_mm),domain)) {
                const double sine=std::sin(half);
                retreat[i]=std::max(retreat[i],options.minimum_radius_mm*(sine+std::sqrt(sine*sine+2*(1-std::cos(half)))));
            }
        }
    }
    for (size_t i=0;i<n;++i) linked[i]=retreat[i]+retreat[(i+1)%n]>(p[(i+1)%n]-p[i]).norm()+1e-8;
    auto gap=std::find(linked.begin(),linked.end(),false);
    if (gap==linked.end()) return {{0,n}};
    const size_t start=(size_t(gap-linked.begin())+1)%n;
    std::vector<CornerGroup> groups;
    for (size_t offset=0;offset<n;) {
        CornerGroup group{(start+offset)%n,1};
        while (offset+group.count<n && linked[(group.first+group.count-1)%n]) ++group.count;
        groups.push_back(group); offset+=group.count;
    }
    return groups;
}

} // namespace

namespace continuous_fiber_detail {
CycleChoice compatible_cycle(const std::vector<LocalSolutions>& candidates,
    const std::vector<size_t>& fixed,const std::vector<std::vector<size_t>>& banned)
{
    if (candidates.empty()) return {};
    const auto compatible=[](const auto& a,const auto& b) { return a.outgoing_consumed<=b.incoming_remaining+1e-8; };
    // Anchor the cycle at its smallest pool. Sort each predecessor pool once:
    // compatibility is an interval, so a prefix minimum replaces the quadratic
    // all-pairs transition without changing the optimal discrete result.
    size_t anchor=0;
    for (size_t i=1;i<candidates.size();++i)
        if (candidates[i].values.size()<candidates[anchor].values.size()) anchor=i;
    const auto group=[&](size_t i) { return (anchor+i)%candidates.size(); };
    const auto values=[&](size_t i)->const std::vector<TangentSolution>& { return candidates[group(i)].values; };
    std::vector<std::vector<size_t>> incoming_order(candidates.size()),outgoing_order(candidates.size());
    for (size_t i=0;i<candidates.size();++i) {
        for (size_t j=0;j<values(i).size();++j) incoming_order[i].push_back(j);
        outgoing_order[i]=incoming_order[i];
        std::stable_sort(incoming_order[i].begin(),incoming_order[i].end(),[&](size_t a,size_t b) {
            return values(i)[a].incoming_remaining<values(i)[b].incoming_remaining;
        });
        std::stable_sort(outgoing_order[i].begin(),outgoing_order[i].end(),[&](size_t a,size_t b) {
            return values(i)[a].outgoing_consumed<values(i)[b].outgoing_consumed;
        });
    }
    CycleChoice result;
    const auto allowed=[&](size_t i,size_t j) {
        i=group(i);
        return (fixed.empty() || fixed[i]==size_t(-1) || fixed[i]==j) &&
            (banned.empty() || std::find(banned[i].begin(),banned[i].end(),j)==banned[i].end());
    };
    for (size_t start=0;start<values(0).size();++start) {
        if (!allowed(0,start)) continue;
        std::vector<std::vector<double>> cost(candidates.size());
        std::vector<std::vector<size_t>> previous(candidates.size());
        for (size_t i=0;i<candidates.size();++i) {
            cost[i].assign(values(i).size(),HUGE_VAL);
            previous[i].resize(values(i).size());
        }
        cost[0][start]=values(0)[start].score;
        for (size_t i=1;i<candidates.size();++i) {
            size_t cursor=0,best=size_t(-1);
            for (size_t j:incoming_order[i]) {
                while (cursor<outgoing_order[i-1].size()) {
                    const size_t k=outgoing_order[i-1][cursor];
                    if (!compatible(values(i-1)[k],values(i)[j])) break;
                    if (std::isfinite(cost[i-1][k]) && (best==size_t(-1) ||
                        cost[i-1][k]<cost[i-1][best] || (cost[i-1][k]==cost[i-1][best] && k<best))) best=k;
                    ++cursor;
                }
                if (best!=size_t(-1) && allowed(i,j)) {
                    cost[i][j]=cost[i-1][best]+values(i)[j].score;
                    previous[i][j]=best;
                }
            }
        }
        for (size_t end=0;end<values(candidates.size()-1).size();++end)
            if (cost.back()[end]<result.score && compatible(values(candidates.size()-1)[end],values(0)[start])) {
                result.score=cost.back()[end]; result.indices.resize(candidates.size());
                size_t j=end;
                for (size_t i=candidates.size();i-->0;) { result.indices[group(i)]=j; if (i) j=previous[i][j]; }
            }
    }
    return result;
}

ContourRoundingResult validate_cycle(const std::vector<TangentSolution>& values,
    const Polyline& source,const ExPolygons& domain,const ExPolygons& output_domain,
    const ContourRoundingOptions& options)
{
    ContourRoundingResult result;
    Polyline output;
    double distance=0;
    const auto append=[&](const Point& p) {
        if (!output.points.empty()) {
            if (output.points.back()==p) return;
            distance+=unscale<double>((p-output.points.back()).cast<double>().norm());
        }
        output.points.push_back(p);
        if (output.points.size()>maximum_samples) throw std::length_error("Contour exceeds sampling budget");
    };
    for (const auto& value:values) for (const auto& piece:connection_primitives(value)) {
        if (const auto* line=std::get_if<Linef>(&piece)) {
            append(scaled_point(line->a));append(scaled_point(line->b));
        } else {
            auto arc=std::get<ContourArc>(piece);
            const auto sampled=discretize(arc,options.chord_tolerance_mm);
            append(sampled.points.front());arc.begin_mm=distance;
            for (const auto& p:sampled.points) append(p);
            arc.end_distance_mm=distance;
            if (arc.end_distance_mm>arc.begin_mm) result.arcs.push_back(arc);
        }
    }
    if (output.points.back()!=output.points.front()) output.points.push_back(output.points.front());
    if (!simple(output.points)) result.issues.push_back({ContourRoundingFailure::SelfIntersection,source});
    if (!inside(output,output_domain)) {
        for (const auto& value:values) {
            const auto line=sample_connection(value,options.chord_tolerance_mm);
            if (!inside(line,domain)) result.issues.push_back({ContourRoundingFailure::OutsideDomain,line});
        }
        for (size_t i=0;i<values.size();++i) {
            const auto& a=values[i].arcs.back();const auto& b=values[(i+1)%values.size()].arcs.front();
            const Polyline gap(Points{scaled_point(a.end_mm),scaled_point(b.start_mm)});
            if (gap.points.front()!=gap.points.back() && !inside(gap,output_domain))
                result.issues.push_back({ContourRoundingFailure::OutsideDomain,gap});
        }
        if (result.issues.empty()) result.issues.push_back({ContourRoundingFailure::OutsideDomain,source});
    }
    // A locally valid connector must not change which forbidden holes
    // are enclosed by this loop. Compare areas to avoid boundary-point
    // classification changing because of integer rounding.
    Polygon original_polygon(Points(source.points.begin(),source.points.end()-1));
    Polygon output_polygon(Points(output.points.begin(),output.points.end()-1));
    original_polygon.make_counter_clockwise(); output_polygon.make_counter_clockwise();
    for (const auto& region:domain) for (auto hole:region.holes) {
        hole.make_counter_clockwise();
        const double before=area(intersection_ex(Polygons{original_polygon},Polygons{hole}));
        const double after=area(intersection_ex(Polygons{output_polygon},Polygons{hole}));
        if (std::abs(after-before)>scale_(options.geometry_tolerance_mm)*hole.length()) {
            result.issues.push_back({ContourRoundingFailure::TopologyChange,source});
            break;
        }
    }
    if (!result.issues.empty()) { result.arcs.clear(); return result; }
    result.path=Polyline3(output);
    return result;
}

} // namespace continuous_fiber_detail

namespace {
// Miter sections partition a smooth swept strip without the cap overlap of
// separately buffered line segments. Intersections of non-neighbouring sections
// locate overlap spatially, so disappearing source overlap cannot compensate
// for a new overlap somewhere else. Source sections also account for pre-existing
// sharp bends using the intersection of their straight swept rectangles.
// These include the existing inside-corner overlap, not only distant branches.
ExPolygons strip_overlap(const Polyline& line,double width,bool source=false)
{
    Points points=line.points;
    if (points.size()>1 && points.front()==points.back()) points.pop_back();
    points.erase(std::unique(points.begin(),points.end()),points.end());
    const size_t n=points.size();
    if (n<3 || width<=0) return {};
    std::vector<Vec2d> normals(n);
    for (size_t i=0;!source && i<n;++i) {
        const Vec2d u=(mm_point(points[i])-mm_point(points[(i+n-1)%n])).normalized();
        const Vec2d v=(mm_point(points[(i+1)%n])-mm_point(points[i])).normalized();
        normals[i]=(.5*width/std::max(1e-12,1+u.dot(v)))*(left_normal(u)+left_normal(v));
    }
    struct Section { Polygon polygon;Vec2d lo,hi; };
    std::vector<Section> sections;sections.reserve(n);
    for (size_t i=0;i<n;++i) {
        const size_t j=(i+1)%n;
        const Vec2d a=mm_point(points[i]),b=mm_point(points[j]);
        const Vec2d edge_normal=.5*width*left_normal((b-a).normalized());
        const Vec2d first=source?edge_normal:normals[i],last=source?edge_normal:normals[j];
        Section section{Polygon(Points{scaled_point(a+first),scaled_point(b+last),
            scaled_point(b-last),scaled_point(a-first)}),Vec2d::Constant(HUGE_VAL),Vec2d::Constant(-HUGE_VAL)};
        section.polygon.make_counter_clockwise();
        for (const auto& p:section.polygon.points) {
            section.lo=section.lo.cwiseMin(mm_point(p));section.hi=section.hi.cwiseMax(mm_point(p));
        }
        sections.push_back(std::move(section));
    }
    std::stable_sort(sections.begin(),sections.end(),[](const auto& a,const auto& b){return a.lo.x()<b.lo.x();});
    Polygons overlaps;
    for (size_t i=0;i<n;++i) for (size_t j=i+1;j<n && sections[j].lo.x()<sections[i].hi.x();++j) {
        const auto& a=sections[i];const auto& b=sections[j];
        if (a.hi.y()<=b.lo.y() || b.hi.y()<=a.lo.y()) continue;
        auto intersection=intersection_ex(Polygons{a.polygon},Polygons{b.polygon});
        append(overlaps,to_polygons(intersection));
    }
    return union_ex(overlaps);
}

// Enumerate disjoint next-best combinations only after full validation fails.
// Validation stays separate from support compatibility and never weakens it.
template<class Validate>
ContourRoundingResult validated_cycle(const std::vector<LocalSolutions>& candidates,
    CycleChoice& choice,const Polyline& source,size_t& searches_left,Validate&& validate)
{
    ContourRoundingResult result;
    struct Alternative {
        CycleChoice choice;
        size_t first_free=0;
        std::vector<size_t> fixed;
        std::vector<std::vector<size_t>> banned;
    };
    const auto later=[](const Alternative& a,const Alternative& b){return a.choice.score>b.choice.score;};
    std::priority_queue<Alternative,std::vector<Alternative>,decltype(later)> pending(later);
    pending.push({choice,0,std::vector<size_t>(candidates.size(),size_t(-1)),std::vector<std::vector<size_t>>(candidates.size())});
    // Partition the remaining combinations by their first changed window.
    // These subproblems are disjoint: no rejected complete choice is retried.
    bool limited=false;
    while (!pending.empty()) {
        auto current=pending.top();pending.pop();
        std::vector<TangentSolution> values;values.reserve(candidates.size());
        for (size_t i=0;i<candidates.size();++i) values.push_back(candidates[i].values[current.choice.indices[i]]);
        result=validate(values);
        if (result.path) { choice=current.choice;break; }
        for (size_t i=current.first_free;i<candidates.size();++i) {
            if (candidates[i].values.size()>1) {
                if (searches_left==0) {limited=true;break;}
                auto next=current;
                next.first_free=i;
                next.banned[i].push_back(current.choice.indices[i]);
                next.choice=compatible_cycle(candidates,next.fixed,next.banned);
                --searches_left;
                if (!next.choice.indices.empty()) pending.push(std::move(next));
            }
            current.fixed[i]=current.choice.indices[i];
        }
    }
    if (!result.path) {
        if (limited) result.issues.push_back({ContourRoundingFailure::SearchBudgetExceeded,source});
        return result;
    }
    return result;
}

ContourRoundingResult solve_ring(const Polyline& source,const ExPolygons& domain,
    const ExPolygons& output_domain,const ContourRoundingOptions& options)
{
    ContourRoundingResult result;
    const auto p=canonical_ring(source);
    if (p.size()<3 || !simple(source.points)) { result.issues.push_back({ContourRoundingFailure::InvalidInput,source}); return result; }
    std::optional<DomainDistance> distances;
    std::map<std::pair<size_t,size_t>,LocalSolutions> cache;
    size_t revisions_left=2*p.size(),local_queries_left=2*p.size(),searches_left=128;
    bool budget_exhausted=false,optimizer_limit=false,numerical_failure=false;
    const auto solve=[&](CornerGroup group,double lo=0.0,double hi=HUGE_VAL) {
        if (local_queries_left==0) {
            budget_exhausted=true;
            // Cached partitions may still form a valid ring without another
            // local solve. Do not consume their independent revision budget.
            return LocalSolutions{};
        }
        --local_queries_left;
        auto local=solve_group(p,group,options,domain,distances,lo,hi);
        optimizer_limit|=local.optimizer_limit_reached;
        numerical_failure|=local.values.empty() && local.numerical_failure;
        return local;
    };
    const auto cached=[&](CornerGroup group)->LocalSolutions& {
        const auto key=std::make_pair(group.first,group.count);
        auto it=cache.find(key);
        if (it==cache.end()) it=cache.emplace(key,solve(group)).first;
        return it->second;
    };
    using Partition=std::vector<CornerGroup>;
    const auto merged_partition=[](Partition groups,size_t first,size_t span) {
        std::rotate(groups.begin(),groups.begin()+first,groups.end());
        for (size_t i=1;i<span;++i) groups.front().count+=groups[i].count;
        groups.erase(groups.begin()+1,groups.begin()+span);
        return groups;
    };
    const auto priority=[&](const Partition& groups) {
        size_t unresolved=0;double deficit=0;
        for (const auto& group:groups) if (cached(group).values.empty()) {
            ++unresolved;
            if (const auto support=supports(p,group)) {
                const Vec2d contact=support->base+options.minimum_radius_mm*support->slope;
                deficit+=std::max({0.0,-contact.x(),contact.x()-support->incoming_length})+
                    std::max({0.0,-contact.y(),contact.y()-support->outgoing_length});
            }
        }
        return std::make_pair(unresolved,deficit);
    };
    const auto attempt=[&](Partition groups,const auto& enqueue,size_t queued_partitions) {
        ContourRoundingResult result;
        std::vector<LocalSolutions> candidates;
        CycleChoice choice;
        for (;;) {
            // Refining compatible candidates need not reduce the window count.
            // Bound those revisions as well as each local optimizer invocation.
            if (revisions_left==0) {
                budget_exhausted=true;
                result.issues.push_back({ContourRoundingFailure::SearchBudgetExceeded,source});return result;
            }
            --revisions_left;
            candidates.clear();
            for (auto group:groups) {
                if (group.count>=p.size()-1) {
                    result.issues.push_back({ContourRoundingFailure::SearchNotFound,source}); return result;
                }
                candidates.push_back(cached(group));
            }
            bool unresolved=false;
            for (size_t i=0;i<groups.size();++i) if (candidates[i].values.empty()) {
                unresolved=true;
                // An unresolved window may need support from BOTH sides. Keep
                // intermediate partitions even when their local solve fails; a
                // single-circle contact cannot rule out a later CSC/CCC solution.
                for (size_t before:{(i+groups.size()-1)%groups.size(),i}) {
                    const size_t count=groups[before].count+groups[(before+1)%groups.size()].count;
                    if (count<p.size()-1) {
                        auto merged=merged_partition(groups,before,2);
                        const auto order=priority(merged);
                        enqueue(std::move(merged),order);
                    }
                }
                break;
            }
            if (unresolved) break;
            // Propagate support bounds before selecting a cycle. Pairwise compatibility
            // alone is insufficient: one candidate must fit both neighbours at once.
            auto active=candidates;
            size_t blocked=groups.size();double incoming_min=0,outgoing_max=HUGE_VAL;
            bool changed=true;
            while (changed && blocked==groups.size()) {
                changed=false;
                for (size_t i=0;i<groups.size();++i) {
                    const size_t prev=(i+groups.size()-1)%groups.size(),next=(i+1)%groups.size();
                    double lo=HUGE_VAL,hi=0;
                    for (const auto& v:active[prev].values) lo=std::min(lo,v.outgoing_consumed);
                    for (const auto& v:active[next].values) hi=std::max(hi,v.incoming_remaining);
                    auto& values=active[i].values;
                    const size_t before=values.size();
                    values.erase(std::remove_if(values.begin(),values.end(),[&](const auto& v){
                        return v.incoming_remaining<lo-1e-8 || v.outgoing_consumed>hi+1e-8;
                    }),values.end());
                    if (values.empty()) { blocked=i;incoming_min=lo;outgoing_max=hi;break; }
                    changed|=values.size()!=before;
                }
            }
            if (blocked==groups.size()) {
                candidates=std::move(active);
                if (searches_left==0) {
                    budget_exhausted=true;
                    result.issues.push_back({ContourRoundingFailure::SearchBudgetExceeded,source});return result;
                }
                --searches_left;
                choice=compatible_cycle(candidates);
                break;
            }
            const auto group=groups[blocked];
            auto constrained=solve(group,incoming_min,outgoing_max);
            if (!constrained.values.empty()) {
                auto& retained=cache.at({group.first,group.count});
                for (auto& value:constrained.values)
                    if (std::none_of(retained.values.begin(),retained.values.end(),[&](const auto& old){return same_connection(old,value);})) {
                        retained.values.push_back(std::move(value));
                    }
                retained.optimizer_limit_reached|=constrained.optimizer_limit_reached;
                retained.numerical_failure|=constrained.numerical_failure;
                prune_candidates(retained);
                if (std::any_of(retained.values.begin(),retained.values.end(),[&](const auto& value) {
                    return std::none_of(candidates[blocked].values.begin(),candidates[blocked].values.end(),
                        [&](const auto& old){return same_connection(old,value);});
                })) continue;
            }
            // Verified connections identify which shared supports overlap.
            double left_overlap=HUGE_VAL,right_overlap=HUGE_VAL;
            for (const auto& value:candidates[blocked].values) {
                const double left=incoming_min-value.incoming_remaining;
                const double right=value.outgoing_consumed-outgoing_max;
                if (left>1e-8) left_overlap=std::min(left_overlap,left+std::max(0.0,right));
                if (right>1e-8) right_overlap=std::min(right_overlap,right+std::max(0.0,left));
            }
            const size_t previous=(blocked+groups.size()-1)%groups.size();
            for (const auto& side:{std::make_pair(previous,left_overlap),std::make_pair(blocked,right_overlap)})
                if (std::isfinite(side.second) && groups[side.first].count+groups[(side.first+1)%groups.size()].count<p.size()-1) {
                    auto merged=merged_partition(groups,side.first,2);
                    const auto order=priority(merged);
                    enqueue(std::move(merged),order);
                }
            result.issues.push_back({ContourRoundingFailure::SupportConflict,source});
            return result;
        }
        for (size_t k=0;k<groups.size();++k) if (candidates[k].values.empty()) {
            Polyline issue;
            const auto group=groups[k];
            for (size_t i=0;i<group.count+2;++i)
                issue.points.push_back(scaled_point(p[(group.first+p.size()-1+i)%p.size()]));
            result.issues.push_back({candidates[k].optimizer_limit_reached?ContourRoundingFailure::OptimizerLimit:
                candidates[k].numerical_failure?ContourRoundingFailure::NumericalFailure:ContourRoundingFailure::SearchNotFound,std::move(issue)});
        }
        if (!result.issues.empty()) return result;
        if (choice.indices.empty()) {
            result.issues.push_back({ContourRoundingFailure::SupportConflict,source});
            return result;
        }
        // Reserve a share for queued partitions instead of letting a failing
        // first partition consume every remaining combination search.
        const size_t allowance=searches_left/(queued_partitions+1);
        size_t remaining=allowance;
        result=validated_cycle(candidates,choice,source,remaining,[&](const auto& values) {
            return validate_cycle(values,source,domain,output_domain,options);
        });
        searches_left-=allowance-remaining;
        if (!result.path) return result;
        // Improvement is optional and transactional: immutable candidates retain
        // the accepted baseline even if a better local pose crosses a distant edge.
        std::vector<TangentSolution> proposal;
        for (size_t i=0;i<groups.size();++i) proposal.push_back(candidates[i].values[choice.indices[i]]);
        return refine_validated_cycle(std::move(result),std::move(proposal),[&](auto& proposal) {
            bool improved=false;
            for (size_t i=0;i<groups.size() && local_queries_left>0;++i) {
                auto& selected=proposal[i];
                const auto& unconstrained=cache.at({groups[i].first,groups[i].count}).values;
                const auto best=std::min_element(unconstrained.begin(),unconstrained.end(),
                    [](const auto& a,const auto& b){return a.score<b.score;});
                if (best==unconstrained.end() || selected.score<=best->score+1e-8) continue;
                const double lo=proposal[(i+groups.size()-1)%groups.size()].outgoing_consumed;
                const double hi=proposal[(i+1)%groups.size()].incoming_remaining;
                auto refined=solve(groups[i],lo,hi);
                for (auto& value:refined.values) if (value.score<selected.score-1e-8) {
                    selected=std::move(value);improved=true;
                }
            }
            return improved;
        },[&](const auto& proposal) {
            return validate_cycle(proposal,source,domain,output_domain,options);
        });
    };
    result=search_contour_partitions(conflict_windows(p,options,domain),source,
        revisions_left,searches_left,attempt);
    if (!result.path) {
        const auto report=[&](bool occurred,ContourRoundingFailure reason) {
            if (occurred && std::none_of(result.issues.begin(),result.issues.end(),[&](const auto& issue){return issue.reason==reason;}))
                result.issues.push_back({reason,source});
        };
        report(numerical_failure,ContourRoundingFailure::NumericalFailure);
        report(optimizer_limit,ContourRoundingFailure::OptimizerLimit);
        report(std::any_of(cache.begin(),cache.end(),[](const auto& item){return item.second.candidates_pruned;}),
            ContourRoundingFailure::CandidateLimit);
        report(budget_exhausted,ContourRoundingFailure::SearchBudgetExceeded);
        return result;
    }
    Polyline output=result.path->to_polyline();
    Points original=source.points; original.pop_back();
    if (Polygon(original).is_clockwise()) {
        const double length=unscale<double>(output.length());
        output.reverse(); std::reverse(result.arcs.begin(),result.arcs.end());
        for (auto& arc:result.arcs) {
            std::swap(arc.start_mm,arc.end_mm); arc.sweep_radians=-arc.sweep_radians;
            const double start=length-arc.end_distance_mm;
            arc.end_distance_mm=length-arc.begin_mm; arc.begin_mm=start;
        }
    }
    result.path=Polyline3(output);
    return result;
}
} // namespace

std::pair<double,double> ContinuousFiberFillStrategy::contour_coverage_overlap(
    const Polyline3& source,const Polyline3& rounded,double width)
{
    if (width<=0) return {0,0};
    const double geometry_tolerance=ContourRoundingOptions{}.geometry_tolerance_mm;
    const auto original=strip_overlap(source.to_polyline(),width,true);
    const auto added=diff_ex(strip_overlap(rounded.to_polyline(),width),
        offset_ex(original,float(scale_(2*geometry_tolerance))));
    double source_area=area(original)*SCALING_FACTOR*SCALING_FACTOR;
    double added_area=area(added)*SCALING_FACTOR*SCALING_FACTOR;
    const double tolerance=8*geometry_tolerance*width;
    return {source_area<=tolerance?0:source_area,added_area<=tolerance?0:added_area};
}

const char* contour_rounding_failure_name(ContourRoundingFailure reason)
{
    switch(reason) {
    case ContourRoundingFailure::InvalidInput:return "invalid_contour";
    case ContourRoundingFailure::SearchNotFound:return "rounding_search_not_found";
    case ContourRoundingFailure::OutsideDomain:return "rounded_contour_outside_domain";
    case ContourRoundingFailure::TopologyChange:return "rounded_contour_topology_change";
    case ContourRoundingFailure::SelfIntersection:return "rounded_contour_self_intersection";
    case ContourRoundingFailure::SupportConflict:return "rounding_support_conflict";
    case ContourRoundingFailure::SearchBudgetExceeded:return "rounding_search_budget_exceeded";
    case ContourRoundingFailure::NumericalFailure:return "rounding_numerical_failure";
    case ContourRoundingFailure::OptimizerLimit:return "rounding_optimizer_limit";
    case ContourRoundingFailure::CandidateLimit:return "rounding_candidate_limit";
    case ContourRoundingFailure::SamplingLimit:return "rounding_sampling_limit";
    }
    return "unknown";
}

ContourRoundingResult ContinuousFiberFillStrategy::round_contour(
    const Polyline3& source,const ExPolygons& centerline_domain,const ContourRoundingOptions& options)
{
    if (options.minimum_radius_mm==0) {
        ContourRoundingResult result; result.path=source; return result;
    }
    const Polyline line=source.to_polyline();
    ContourRoundingResult invalid;
    invalid.issues.push_back({ContourRoundingFailure::InvalidInput,line});
    if (!std::isfinite(options.minimum_radius_mm) || options.minimum_radius_mm<0 ||
        !std::isfinite(options.chord_tolerance_mm) || options.chord_tolerance_mm<=0 ||
        !std::isfinite(options.geometry_tolerance_mm) || options.geometry_tolerance_mm<=options.chord_tolerance_mm ||
        !std::isfinite(options.fiber_width_mm) || options.fiber_width_mm<0 ||
        line.points.size()<4 || line.points.front()!=line.points.back() || centerline_domain.empty() ||
        std::any_of(source.points.begin(),source.points.end(),[](const Point3& p){return p.z()!=0;})) return invalid;
    try {
        const ExPolygons comparison_domain=offset_ex(centerline_domain,float(scale_(
            options.geometry_tolerance_mm-options.chord_tolerance_mm)));
        // Curved primitives reserve chord error for their analytic arcs. Straight
        // support gaps have no chord error and use the full geometry tolerance.
        const ExPolygons output_domain=offset_ex(centerline_domain,float(scale_(options.geometry_tolerance_mm)));
        auto effective=options;
        if (options.fiber_width_mm>0)
            effective.minimum_radius_mm=std::max(options.minimum_radius_mm,
                .5*options.fiber_width_mm+options.geometry_tolerance_mm);
        return solve_ring(line,comparison_domain,output_domain,effective);
    } catch (const std::length_error&) {
        invalid.issues.front().reason=ContourRoundingFailure::SamplingLimit; return invalid;
    }
}

ExPolygons ContinuousFiberFillStrategy::build_infill_domain(
    const ExPolygons& original_area,
    const ExPolygons& contour_to_infill_keepout)
{
    if (contour_to_infill_keepout.empty())
        return original_area;
    return diff_ex(original_area, contour_to_infill_keepout, ApplySafetyOffset::Yes);
}

ExPolygons ContinuousFiberFillStrategy::build_resin_area(
    const ExPolygons& original_area,
    const ExPolygons& accepted_fiber_resin_exclusion)
{
    if (accepted_fiber_resin_exclusion.empty())
        return original_area;
    return diff_ex(original_area, accepted_fiber_resin_exclusion, ApplySafetyOffset::Yes);
}

} // namespace Slic3r
