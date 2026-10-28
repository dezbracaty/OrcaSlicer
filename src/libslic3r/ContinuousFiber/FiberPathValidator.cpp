#include <boost/multiprecision/cpp_int.hpp>
#include "FiberInfillValidationDomains.hpp"
#include "FiberPathValidator.hpp"
#include "FiberDomainIndex.hpp"
#include "../PrintBase.hpp"

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../AABBTreeLines.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Slic3r {

namespace continuous_fiber_detail {
namespace query_band_detail {
// Supported coordinates/radius bound affine coefficients to128 bits.
using Coefficient=boost::multiprecision::int128_t;
using Product=boost::multiprecision::int256_t;
struct Fraction { Coefficient n,d; };
struct InnerInterval { Fraction lo,hi; };
inline Fraction fraction(Coefficient n,Coefficient d) {
    if(d==0)throw std::logic_error("zero fraction denominator");
    if(d<0){n=-n;d=-d;}
    return {n,d};
}
inline bool less(const Fraction&a,const Fraction&b){return Product(a.n)*Product(b.d)<Product(b.n)*Product(a.d);}
inline bool equal(const Fraction&a,const Fraction&b){return !less(a,b)&&!less(b,a);}
inline Fraction maximum(Fraction a,const Fraction&b){return less(a,b)?b:a;}
inline Fraction minimum(Fraction a,const Fraction&b){return less(a,b)?a:b;}
inline uint64_t floor_sqrt(uint64_t n) {
    // Exact restoring integer square root; no floating estimate or correction.
    uint64_t remainder=n,result=0,bit=uint64_t(1)<<62;
    while(bit>remainder)bit>>=2;
    while(bit){
        if(remainder>=result+bit){remainder-=result+bit;result=(result>>1)+bit;}
        else result>>=1;
        bit>>=2;
    }
    if(Product(result)*result>Product(n)||Product(result+1)*(result+1)<=Product(n))
        throw std::logic_error("incorrect integer square root");
    return result;
}
inline bool affine(InnerInterval&i,const Coefficient&origin,const Coefficient&slope,const Coefficient&lower,const Coefficient&upper){
    if(slope==0)return origin>=lower&&origin<=upper;
    // Promotion precedes subtraction/negation. Bound-origin can exceed int64.
    Fraction lo=fraction(lower-origin,slope),hi=fraction(upper-origin,slope);
    if(slope<0)std::swap(lo,hi);
    i.lo=maximum(i.lo,lo);i.hi=minimum(i.hi,hi);
    return !less(i.hi,i.lo);
}
inline void diamond(std::vector<InnerInterval>&out,const Point&a,const Point&b,const Point&p,int64_t eps){
    const Coefficient wx=Coefficient(a.x())-Coefficient(p.x()),wy=Coefficient(a.y())-Coefficient(p.y()),dx=Coefficient(b.x())-Coefficient(a.x()),dy=Coefficient(b.y())-Coefficient(a.y()),r=Coefficient(eps);
    InnerInterval i{{Coefficient(0),Coefficient(1)},{Coefficient(1),Coefficient(1)}};
    if(affine(i,wx+wy,dx+dy,-r,r)&&affine(i,wx-wy,dx-dy,-r,r))out.push_back(i);
}
inline void intervals(std::vector<InnerInterval>&out,const Point&a,const Point&b,const Line&e,int64_t eps){
    const Coefficient vx=Coefficient(e.b.x())-Coefficient(e.a.x()),vy=Coefficient(e.b.y())-Coefficient(e.a.y()),K=vx*vx+vy*vy;
    diamond(out,a,b,e.a,eps);
    if(K==0)return;
    diamond(out,a,b,e.b,eps);
    if(K<0||K>=(Coefficient(1)<<63))throw std::logic_error("unsupported positive K");
    const Coefficient wx=Coefficient(a.x())-Coefficient(e.a.x()),wy=Coefficient(a.y())-Coefficient(e.a.y()),dx=Coefficient(b.x())-Coefficient(a.x()),dy=Coefficient(b.y())-Coefficient(a.y());
    const Coefficient band=Coefficient(eps)*Coefficient(floor_sqrt(K.convert_to<uint64_t>()));
    InnerInterval strip{{Coefficient(0),Coefficient(1)},{Coefficient(1),Coefficient(1)}};
    if(affine(strip,wx*vx+wy*vy,dx*vx+dy*vy,Coefficient(0),K)&&
       affine(strip,vx*wy-vy*wx,vx*dy-vy*dx,-band,band))out.push_back(strip);
}
inline bool covers(std::vector<InnerInterval> intervals,Fraction&reach){
    std::sort(intervals.begin(),intervals.end(),[](const InnerInterval&a,const InnerInterval&b){return equal(a.lo,b.lo)?less(a.hi,b.hi):less(a.lo,b.lo);});
    reach={Coefficient(0),Coefficient(1)};bool started=false;
    for(const auto&i:intervals){
        if(!started){if(less({Coefficient(0),Coefficient(1)},i.lo))break;reach=i.hi;started=true;}
        else{if(less(reach,i.lo))break;reach=maximum(reach,i.hi);}
        if(!less(reach,{Coefficient(1),Coefficient(1)}))return true;
    }
    return false;
}

bool in_capsule(const Point& p, const Line& e, int64_t eps)
{
    // The caller checked |coordinate|, eps <= loRange. K/h/cross fit int64;
    // cross^2 and eps^2*K require the full Int128 product, never int64 squares.
    const int64_t vx = e.b.x()-e.a.x(), vy = e.b.y()-e.a.y(),
                  wx = p.x()-e.a.x(), wy = p.y()-e.a.y(),
                  K = vx*vx+vy*vy, h = wx*vx+wy*vy, eps2 = eps*eps;
    if (K == 0 || h <= 0) return wx*wx+wy*wy <= eps2;
    if (h >= K) {
        const int64_t x = p.x()-e.b.x(), y = p.y()-e.b.y();
        return x*x+y*y <= eps2;
    }
    const int64_t c = vx*wy-vy*wx;
    return Int128::multiply(c, c) <= Int128::multiply(eps2, K);
}
}
// Only native regularized planned-C masks are eligible at the controlled
// rounded-contour call sites. False is unproved, never a rejection.
bool FiberDomainIndex::proves_sample_band(const Polyline&path,double radius_grid) const
{
    if(empty_domain||boundaries.empty()||!supports(path)||path.points.size()<2||
       !std::isfinite(radius_grid)||radius_grid<0||radius_grid>ClipperLib::loRange||std::floor(radius_grid)!=radius_grid)return false;
    const int64_t eps=int64_t(radius_grid);bool nonzero=false;
    for(size_t i=1;i<path.points.size();++i){const Point&a=path.points[i-1],&b=path.points[i];if(a==b)continue;nonzero=true;
        if(permits(Polyline(Points{a,b}),false))continue;
        bool witnessed=false;
        query(double(a.x()-eps),double(a.y()-eps),double(a.x()+eps),double(a.y()+eps),[&](const Line&e){if(query_band_detail::in_capsule(a,e,eps)&&query_band_detail::in_capsule(b,e,eps)){witnessed=true;return false;}return true;});
        if(witnessed)continue;
        std::vector<query_band_detail::InnerInterval> intervals;
        query(double(std::min(a.x(),b.x())-eps),double(std::min(a.y(),b.y())-eps),double(std::max(a.x(),b.x())+eps),double(std::max(a.y(),b.y())+eps),[&](const Line&e){query_band_detail::intervals(intervals,a,b,e,eps);return true;});
        query_band_detail::Fraction reach{query_band_detail::Coefficient(0),query_band_detail::Coefficient(1)};
        if(!query_band_detail::covers(std::move(intervals),reach))return false;
    }
    return nonzero;
}
}


namespace {

constexpr double interval_epsilon_mm = 1e-5;

Polyline check_view(const Polyline3& path)
{
    return Geometry::sample_with_arcs(path,ContourRoundingOptions{}.chord_tolerance_mm).to_polyline();
}


struct SourceInterval {
    double begin_scaled { 0.0 };
    double end_scaled { 0.0 };
    FiberAssignmentKind kind { FiberAssignmentKind::Rejected };
    FiberRejectionReason reason { FiberRejectionReason::None };
};

using SegmentKey = std::tuple<coord_t, coord_t, coord_t, coord_t>;

SegmentKey undirected_segment_key(const Point& a, const Point& b)
{
    if (std::make_tuple(a.x(), a.y()) <= std::make_tuple(b.x(), b.y()))
        return {a.x(), a.y(), b.x(), b.y()};
    return {b.x(), b.y(), a.x(), a.y()};
}

// Keep semantic source vertices through seam selection and clipping. Command
// normalization belongs after Finalizer has frozen the source entry. Exact
// Path entry is selected later from a complete analytic route.
Polyline3 fiber_source_geometry(const Polyline3& input)
{
    if (!input.fitting_result.empty()) { Geometry::validate_with_arcs(input); return input; }
    Polyline3 result;
    for(const auto& point:input.points) {
        if(point.z()!=0)throw std::invalid_argument("Fiber LayerXY source has nonzero Z");
        if(result.points.empty() || result.points.back()!=point)result.points.push_back(point);
    }
    return result;
}

FiberRejectionReason validate_path_geometry(
    const ExtrusionPath& path, bool check_intersections = true)
{
    const Polyline polyline = check_view(path.polyline);
    const Points& points = polyline.points;
    if (points.size() < 2 ||
        !std::isfinite(path.width) || path.width <= 0.0f ||
        !std::isfinite(path.height) || path.height <= 0.0f ||
        !std::isfinite(path.mm3_per_mm) || path.mm3_per_mm <= 0.0)
        return FiberRejectionReason::InvalidGeometry;

    const double length_mm = unscale<double>(scale_(Geometry::length_with_arcs_mm(path.polyline)));
    if (!std::isfinite(length_mm))
        return FiberRejectionReason::InvalidGeometry;

    std::set<SegmentKey> segments;
    for (size_t index = 0; index + 1 < points.size(); ++index) {
        const Point& a = points[index];
        const Point& b = points[index + 1];
        const double segment_length = (b - a).cast<double>().norm();
        if (!std::isfinite(segment_length) || segment_length <= 0.0)
            return FiberRejectionReason::DegenerateSegment;
        if (!segments.insert(undirected_segment_key(a, b)).second)
            return FiberRejectionReason::DuplicateSegment;
        if (index > 0) {
            const Vec2d previous = (a - points[index - 1]).cast<double>();
            const Vec2d current = (b - a).cast<double>();
            if (std::abs(cross2(previous, current)) <= EPSILON && previous.dot(current) < 0.0)
                return FiberRejectionReason::DuplicateSegment;
        }
    }

    if (!check_intersections) return FiberRejectionReason::None;

    const size_t segment_count = points.size() - 1;
    const bool closed = points.front() == points.back();
    // Repeated finite-width offsets can produce thousands of arc samples.
    // Cull disjoint boxes, then retain the exact integer intersection test.
    Linesf lines;
    lines.reserve(segment_count);
    for (size_t i = 0; i < segment_count; ++i)
        lines.emplace_back(points[i].cast<double>(), points[i + 1].cast<double>());
    const auto tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    for (size_t first = 0; first < segment_count; ++first) {
        AABBTreeIndirect::Tree<2, double>::BoundingBox box(lines[first].a, lines[first].a);
        box.extend(lines[first].b);
        bool intersects = false;
        AABBTreeIndirect::traverse(tree, [&](const auto& node) { return box.intersects(node.bbox); },
            [&](const auto& node) {
                const size_t second = node.idx;
                if (second <= first + 1 || (closed && first == 0 && second + 1 == segment_count)) return true;
                intersects = Geometry::segments_intersect(points[first], points[first + 1], points[second], points[second + 1]);
                return !intersects;
            });
        if (intersects) return FiberRejectionReason::SelfIntersection;
    }

    return FiberRejectionReason::None;
}

bool project_onto_source(
    const Point& point,
    const Polyline& source,
    const std::vector<double>& cumulative,
    double& distance_scaled)
{
    double best_distance_squared = std::numeric_limits<double>::max();
    double best_source_distance = 0.0;
    const Vec2d query = point.cast<double>();
    for (size_t index = 0; index + 1 < source.points.size(); ++index) {
        const Vec2d a = source.points[index].cast<double>();
        const Vec2d delta = source.points[index + 1].cast<double>() - a;
        const double squared_length = delta.squaredNorm();
        if (squared_length <= 0.0)
            continue;
        const double parameter = std::clamp((query - a).dot(delta) / squared_length, 0.0, 1.0);
        const Vec2d projection = a + parameter * delta;
        const double squared_distance = (query - projection).squaredNorm();
        if (squared_distance < best_distance_squared) {
            best_distance_squared = squared_distance;
            best_source_distance = cumulative[index] + parameter * (cumulative[index+1]-cumulative[index]);
        }
    }

    const double mapping_tolerance_scaled = scale_(0.001);
    if (best_distance_squared > mapping_tolerance_scaled * mapping_tolerance_scaled)
        return false;
    distance_scaled = best_source_distance;
    return true;
}

bool extract_subpath(const Polyline3& source,double begin_scaled,double end_scaled,Polyline3& output)
{
    const double total=Geometry::length_with_arcs_mm(source);
    const double begin=std::clamp(unscale<double>(begin_scaled),0.,total);
    const double end=std::clamp(unscale<double>(end_scaled),0.,total);
    if(end<=begin) return false;
    output=Geometry::slice_with_arcs_mm(source,begin,end);
    return output.points.size()>=2;
}

std::vector<SourceInterval> partition_candidate(
    const ExtrusionPath& path,
    const ExPolygons& safe_centerline_domain,
    bool& mapping_valid)
{
    mapping_valid = true;
    const double total_length = scale_(Geometry::length_with_arcs_mm(path.polyline));
    if (safe_centerline_domain.empty())
        return {{0.0, total_length, FiberAssignmentKind::Rejected, FiberRejectionReason::OutsideDomain}};

    std::vector<double> analytic_stations;
    const Polyline source = Geometry::sample_with_arcs(path.polyline,ContourRoundingOptions{}.chord_tolerance_mm,&analytic_stations).to_polyline();
    if (diff_pl(Polylines{source}, safe_centerline_domain).empty())
        return {{0.0, total_length, FiberAssignmentKind::AcceptedFiber, FiberRejectionReason::None}};
    std::vector<double> cumulative;
    for (double station:analytic_stations) cumulative.push_back(scale_(station));

    Polylines clipped = intersection_pl(Polylines {source}, safe_centerline_domain);
    std::vector<SourceInterval> inside;
    inside.reserve(clipped.size());
    for (Polyline& fragment : clipped) {
        if (fragment.points.size() < 2)
            continue;
        double begin = 0.0;
        double end = 0.0;
        if (!project_onto_source(fragment.points.front(), source, cumulative, begin) ||
            !project_onto_source(fragment.points.back(), source, cumulative, end)) {
            mapping_valid = false;
            return {};
        }
        if (source.points.front() == source.points.back()) {
            // A closed source has station 0 and station length at the same point.
            // Determine clipping orientation from its first edge, then split a
            // wrapped interval. Merely swapping endpoints invents the other arc.
            double next = 0;
            if (!project_onto_source(fragment.points[1], source, cumulative, next)) {
                mapping_valid = false;
                return {};
            }
            const double edge_length = (fragment.points[1]-fragment.points[0]).cast<double>().norm();
            const auto forward = [&](double a, double b) { return b >= a ? b-a : b-a+total_length; };
            if (std::abs(forward(next,begin)-edge_length) < std::abs(forward(begin,next)-edge_length))
                std::swap(begin,end);
            if (end < begin) {
                if (total_length-begin > SCALED_EPSILON)
                    inside.push_back({begin,total_length,FiberAssignmentKind::AcceptedFiber,FiberRejectionReason::None});
                begin = 0.0;
            }
        } else if (end < begin)
            std::swap(begin, end);
        begin = std::clamp(begin, 0.0, total_length);
        end = std::clamp(end, 0.0, total_length);
        if (end - begin > SCALED_EPSILON)
            inside.push_back({begin, end, FiberAssignmentKind::AcceptedFiber, FiberRejectionReason::None});
    }

    if (inside.empty())
        return {{0.0, total_length, FiberAssignmentKind::Rejected, FiberRejectionReason::OutsideDomain}};

    std::sort(inside.begin(), inside.end(), [](const SourceInterval& lhs, const SourceInterval& rhs) {
        if (lhs.begin_scaled != rhs.begin_scaled) return lhs.begin_scaled < rhs.begin_scaled;
        return lhs.end_scaled < rhs.end_scaled;
    });

    const double interval_epsilon_scaled = scale_(interval_epsilon_mm);
    std::vector<SourceInterval> merged;
    for (const SourceInterval& interval : inside) {
        if (merged.empty() || interval.begin_scaled > merged.back().end_scaled + interval_epsilon_scaled) {
            merged.push_back(interval);
            continue;
        }
        if (interval.begin_scaled < merged.back().end_scaled - interval_epsilon_scaled) {
            mapping_valid = false;
            return {};
        }
        merged.back().end_scaled = std::max(merged.back().end_scaled, interval.end_scaled);
    }

    std::vector<SourceInterval> result;
    double cursor = 0.0;
    for (SourceInterval interval : merged) {
        if (interval.begin_scaled > cursor + interval_epsilon_scaled)
            result.push_back({cursor, interval.begin_scaled, FiberAssignmentKind::Rejected,
                              FiberRejectionReason::OutsideDomain});
        else
            interval.begin_scaled = cursor;
        if (interval.end_scaled > interval.begin_scaled + SCALED_EPSILON)
            result.push_back(interval);
        cursor = std::max(cursor, interval.end_scaled);
    }
    if (cursor < total_length - interval_epsilon_scaled)
        result.push_back({cursor, total_length, FiberAssignmentKind::Rejected,
                          FiberRejectionReason::OutsideDomain});
    else if (!result.empty())
        result.back().end_scaled = total_length;
    return result;
}

// Arc stations refer to the sampled reference curve. Clipping preserves the
// original circle and speed constraint; it never fits a new arc or joins a gap.
std::vector<ContourArc> trim_arcs(const std::vector<ContourArc>& arcs, double begin, double end)
{
    std::vector<ContourArc> result;
    for (const auto& arc : arcs) {
        const double a = std::max(begin, arc.begin_mm), b = std::min(end, arc.end_distance_mm);
        if (b <= a) continue;
        auto part = arc;
        const double length = arc.end_distance_mm - arc.begin_mm;
        const auto at = [&](double position) -> Vec2d {
            const double angle = arc.sweep_radians * (position - arc.begin_mm) / length;
            const Vec2d v = arc.start_mm - arc.center_mm;
            return arc.center_mm + Vec2d(std::cos(angle)*v.x()-std::sin(angle)*v.y(),
                                        std::sin(angle)*v.x()+std::cos(angle)*v.y());
        };
        part.start_mm = at(a); part.end_mm = at(b);
        part.source_sweep_radians = arc.source_sweep_radians != 0 ? arc.source_sweep_radians : arc.sweep_radians;
        part.sweep_radians = arc.sweep_radians * (b-a)/length;
        part.begin_mm = a-begin; part.end_distance_mm = b-begin;
        result.push_back(part);
    }
    return result;
}

// Move the arbitrary seam to a rejected interval boundary. Reuse the one
// clipping result, including its ownership intervals, rather than intersecting
// the same curve a second time.
void rotate_reference(ExtrusionPath& path, std::vector<ContourArc>& arcs, std::vector<SourceInterval>& intervals)
{
    if (!path.is_closed() || intervals.size()<2) return;
    const auto cut=std::find_if(intervals.begin(),intervals.end(),[](const auto& i){return i.kind==FiberAssignmentKind::Rejected;});
    if (cut==intervals.end() || cut->end_scaled==scale_(Geometry::length_with_arcs_mm(path.polyline))) return;
    const double seam=cut->end_scaled, length=scale_(Geometry::length_with_arcs_mm(path.polyline));
    Polyline3 prefix,suffix;
    if (!Geometry::split_at_length_with_arcs_mm(path.polyline,unscale<double>(seam),prefix,suffix))
        throw std::runtime_error("Cannot rotate clipped fiber reference");
    Geometry::append_with_arcs(suffix,prefix);
    path.polyline=std::move(suffix);
    auto rotated=trim_arcs(arcs,unscale<double>(seam),unscale<double>(length));
    auto head=trim_arcs(arcs,0,unscale<double>(seam));
    for (auto& arc:head) {arc.begin_mm+=unscale<double>(length-seam);arc.end_distance_mm+=unscale<double>(length-seam);}
    rotated.insert(rotated.end(),head.begin(),head.end()); arcs=std::move(rotated);
    std::rotate(intervals.begin(),std::next(cut),intervals.end());
    std::vector<SourceInterval> shifted;
    for (auto part:intervals) {
        if (part.begin_scaled>=seam) {part.begin_scaled-=seam;part.end_scaled-=seam;}
        else {part.begin_scaled+=length-seam;part.end_scaled+=length-seam;}
        if (!shifted.empty() && shifted.back().kind==part.kind && shifted.back().reason==part.reason &&
            std::abs(shifted.back().end_scaled-part.begin_scaled)<=scale_(interval_epsilon_mm))
            shifted.back().end_scaled=part.end_scaled;
        else shifted.push_back(part);
    }
    shifted.front().begin_scaled=0; shifted.back().end_scaled=scale_(Geometry::length_with_arcs_mm(path.polyline));
    intervals=std::move(shifted);
}

FiberRejectionReason finalization_reason(FiberFinalizationFailure failure)
{
    switch (failure) {
    case FiberFinalizationFailure::TooShort:
        return FiberRejectionReason::ProcessBudgetTooShort;
    case FiberFinalizationFailure::PolicyBoundaryRejected:
        return FiberRejectionReason::OutsideDomain;
    case FiberFinalizationFailure::OutsideDomain:
        return FiberRejectionReason::FinalizedPathOutsideDomain;
    case FiberFinalizationFailure::InvalidParameter:
        return FiberRejectionReason::InvalidParameter;
    case FiberFinalizationFailure::InvalidGeometry:
        return FiberRejectionReason::InvalidGeometry;
    case FiberFinalizationFailure::FinishUnavailable:
        return FiberRejectionReason::FinishUnavailable;
    case FiberFinalizationFailure::SamplingLimit:
        return FiberRejectionReason::SamplingLimit;
    case FiberFinalizationFailure::None:
        break;
    }
    return FiberRejectionReason::InvalidGeometry;
}

void append_coverage(FiberValidationResult& result, const PreparedFiberPath& prepared)
{
    // These accepted paths are independent. Union once after all candidates,
    // rather than repeatedly clipping the growing accumulated coverage.
    append(result.physical_footprint, prepared.physical_coverage);
    append(result.resin_exclusion, prepared.resin_exclusion);
    append(result.outside_domain, prepared.outside_domain);
    append(result.contour_to_infill_keepout, prepared.contour_to_infill_keepout);
}

} // namespace

const char* fiber_rejection_reason_name(FiberRejectionReason reason)
{
    switch (reason) {
    case FiberRejectionReason::None: return "none";
    case FiberRejectionReason::BendStabilizationTooShort: return "bend_stabilization_too_short";
    case FiberRejectionReason::OpenOuterContour: return "open_outer_contour";
    case FiberRejectionReason::OccupiedContourRegion: return "occupied_by_prior_contour";
    case FiberRejectionReason::UnavailableContourRegion: return "outside_remaining_contour_region";
    case FiberRejectionReason::TooShort: return "too_short";
    case FiberRejectionReason::UnsupportedEntity: return "unsupported_entity";
    case FiberRejectionReason::InvalidParameter: return "invalid_parameter";
    case FiberRejectionReason::InvalidGeometry: return "invalid_geometry";
    case FiberRejectionReason::DegenerateSegment: return "degenerate_segment";
    case FiberRejectionReason::SelfIntersection: return "self_intersection";
    case FiberRejectionReason::DuplicateSegment: return "duplicate_segment";
    case FiberRejectionReason::OutsideDomain: return "outside_domain";
    case FiberRejectionReason::ProcessBudgetTooShort: return "process_budget_too_short";
    case FiberRejectionReason::FinalizedPathOutsideDomain: return "finalized_path_outside_domain";
    case FiberRejectionReason::IntervalMappingFailure: return "interval_mapping_failure";
    case FiberRejectionReason::FinishUnavailable: return "finish_unavailable";
    case FiberRejectionReason::SamplingLimit: return "sampling_limit";
    case FiberRejectionReason::ContourRoundingUnresolved: return "contour_rounding_unresolved";
    }
    return "unknown";
}

std::optional<double> minimum_closed_loop_stable_length_mm(
    double loop_length_mm, const std::vector<ContourBend>& bends)
{
    if (bends.empty()) return std::nullopt;
    double minimum=loop_length_mm-bends.back().end_mm+bends.front().begin_mm;
    for (size_t i=1;i<bends.size();++i)
        minimum=std::min(minimum,bends[i].begin_mm-bends[i-1].end_mm);
    return std::max(0.0,minimum);
}

size_t FiberValidationResult::accepted_count() const
{
    return std::count_if(assignments.begin(), assignments.end(), [](const FiberFragmentAssignment& assignment) {
        return assignment.kind == FiberAssignmentKind::AcceptedFiber;
    });
}

size_t FiberValidationResult::rejected_count() const
{
    return std::count_if(assignments.begin(), assignments.end(), [](const FiberFragmentAssignment& assignment) {
        return assignment.kind == FiberAssignmentKind::Rejected;
    });
}

FiberAssignmentAudit FiberValidationResult::audit_assignments() const
{
    FiberAssignmentAudit audit;
    std::map<FiberCandidateId, double> expected;
    std::set<FiberCandidateId> closed_loops;
    for (const FiberCandidateExtent& candidate : candidates) {
        if (!expected.emplace(candidate.id, candidate.length_mm).second)
            ++audit.invalid_assignment_count;
        if (candidate.requires_closed_loop) closed_loops.insert(candidate.id);
    }

    std::map<FiberCandidateId, std::vector<const FiberFragmentAssignment*>> grouped;
    for (const FiberFragmentAssignment& assignment : assignments) {
        grouped[assignment.id.parent].push_back(&assignment);
        const bool interval_valid = std::isfinite(assignment.source_begin_mm) &&
            std::isfinite(assignment.source_end_mm) &&
            assignment.source_begin_mm >= 0.0 &&
            assignment.source_end_mm + interval_epsilon_mm >= assignment.source_begin_mm;
        const bool state_valid =
            (assignment.kind == FiberAssignmentKind::AcceptedFiber && assignment.prepared &&
             assignment.reason == FiberRejectionReason::None) ||
            (assignment.kind == FiberAssignmentKind::Rejected && !assignment.prepared &&
             assignment.reason != FiberRejectionReason::None) ||
            (assignment.kind == FiberAssignmentKind::IntentionalVoid && !assignment.prepared);
        if (!interval_valid || !state_valid || expected.find(assignment.id.parent) == expected.end())
            ++audit.invalid_assignment_count;
    }

    for (const auto& [candidate_id, expected_length] : expected) {
        const auto found = grouped.find(candidate_id);
        if (found == grouped.end() || found->second.empty()) {
            ++audit.missing_candidate_count;
            audit.gap_length_mm += expected_length;
            continue;
        }
        std::vector<const FiberFragmentAssignment*> fragments = found->second;
        if (closed_loops.count(candidate_id)) {
            // A closed exterior has exactly one whole-path outcome. Neither a
            // partial acceptance nor an intentional void can satisfy closure.
            if (fragments.size() != 1 || fragments.front()->kind == FiberAssignmentKind::IntentionalVoid)
                ++audit.invalid_assignment_count;
            for (const auto* fragment : fragments) if (fragment->kind == FiberAssignmentKind::AcceptedFiber) {
                if (!fragment->centerline || !fragment->centerline->is_closed() || !fragment->prepared) {
                    ++audit.invalid_assignment_count;
                    continue;
                }
                const FiberMotionSpan* first = nullptr;
                const FiberMotionSpan* last = nullptr;
                for (const auto& span : fragment->prepared->spans) if (span.deposits_fiber()) {
                    if (!first) first = &span;
                    last = &span;
                }
                if (!first || first->geometry.points.empty() || last->geometry.points.empty() ||
                    fragment->prepared->entry.start != last->geometry.points.back())
                    ++audit.invalid_assignment_count;
            }
        }
        std::sort(fragments.begin(), fragments.end(), [](const auto* lhs, const auto* rhs) {
            if (lhs->source_begin_mm != rhs->source_begin_mm)
                return lhs->source_begin_mm < rhs->source_begin_mm;
            return lhs->source_end_mm < rhs->source_end_mm;
        });
        double cursor = 0.0;
        for (size_t index = 0; index < fragments.size(); ++index) {
            const FiberFragmentAssignment& fragment = *fragments[index];
            if (fragment.id.fragment_ordinal != index)
                ++audit.invalid_assignment_count;
            if (fragment.source_begin_mm > cursor + interval_epsilon_mm)
                audit.gap_length_mm += fragment.source_begin_mm - cursor;
            else if (fragment.source_begin_mm < cursor - interval_epsilon_mm)
                audit.overlap_length_mm += cursor - fragment.source_begin_mm;
            cursor = std::max(cursor, fragment.source_end_mm);
        }
        if (cursor < expected_length - interval_epsilon_mm)
            audit.gap_length_mm += expected_length - cursor;
        else if (cursor > expected_length + interval_epsilon_mm)
            audit.overlap_length_mm += cursor - expected_length;
    }
    return audit;
}

void FiberValidationResult::release_to(ExtrusionEntitiesPtr& destination)
{
    if (!audit_assignments().valid())
        throw std::runtime_error("Cannot release invalid continuous fiber assignments");
    auto accepted = std::make_unique<ExtrusionEntityCollection>();
    accepted->no_sort = true;
    if (!chain_executions.empty()) {
        if (!audit_open_chains()) throw std::runtime_error("Cannot release invalid open fiber chains");
        for (const auto& chain:chain_executions)
            accepted->entities.push_back(new ExtrusionFiberPath(chain.centerline,output_role,chain.prepared,
                chain.pieces.front().display_purpose));
        if (!accepted->entities.empty()) destination.emplace_back(accepted.release());
        return;
    }
    for (FiberFragmentAssignment& assignment : assignments) {
        if (assignment.kind != FiberAssignmentKind::AcceptedFiber)
            continue;
        if (!assignment.centerline)
            throw std::runtime_error("Accepted continuous fiber assignment has no centerline");
        accepted->entities.push_back(new ExtrusionFiberPath(
            *assignment.centerline, output_role, assignment.prepared,assignment.display_purpose));
    }
    if (!accepted->entities.empty())
        destination.emplace_back(accepted.release());
}

FiberValidationResult FiberPathValidator::validate_impl(
    const ExtrusionEntitiesPtr& candidate_roots,
    const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,
    FiberPathPurpose purpose,
    ExtrusionRole output_role,
    const FiberDomainId& domain_id,
    size_t job_ordinal, const ExPolygons* planned_centerline_domain, const ExPolygons* geometry_domain,
    const ExPolygons* physical_centerline_domain, bool collect_debug, bool requires_closed_loop,
    const std::vector<ContourArc>* source_arcs, double bend_stabilization_length_mm,
    std::vector<FiberFragmentId>* policy_rejected,
    const continuous_fiber_detail::FiberDomainIndex* containment,
    const continuous_fiber_detail::FiberFinalizationDomains* finalization_domains,
    const continuous_fiber_detail::FiberInfillValidationContext* material_context)
{
    FiberValidationResult result;
    result.output_role = output_role;
    size_t path_ordinal = 0;
    // Analytic geometry belongs to this one candidate, never to a separate
    // array indexed by a traversal order through extrusion collections.
    if (source_arcs && (candidate_roots.size() != 1 || !dynamic_cast<const ExtrusionPath*>(candidate_roots.front()) ||
        (!material_context && (!planned_centerline_domain || !physical_centerline_domain))))
        throw std::invalid_argument("Analytic fiber geometry requires one path and its centerline domains");
    const bool allocates_contour = purpose == FiberPathPurpose::Contour && planned_centerline_domain;
    const bool rounds_contours = purpose == FiberPathPurpose::Contour && config.contour_bend_radius_mm > 0;
    std::map<double, ExPolygons> centerline_domains;
    const auto centerline_domain_for_width = [&](double width) -> const ExPolygons& {
        if (planned_centerline_domain) return *planned_centerline_domain;
        auto found = centerline_domains.find(width);
        if (found == centerline_domains.end())
            found = centerline_domains.emplace(width, offset_ex(allowed_domain, -float(scale_(
                0.5*width + (purpose == FiberPathPurpose::Contour ? config.contour_boundary_clearance_mm : 0.0))))).first;
        return found->second;
    };

    // The rounded path's check chords need their sagitta allowance in addition
    // to the half-tolerance clipping reserve. Their sum is the existing full
    // geometry tolerance; the analytic source is never extended or gap-merged.
    std::map<double, ExPolygons> partition_domains;
    const auto partition_domain_for_width = [&](double width) -> const ExPolygons& {
        const auto& domain=centerline_domain_for_width(width);
        if (purpose!=FiberPathPurpose::Contour) return domain;
        auto found=partition_domains.find(width);
        if (found==partition_domains.end())
            found=partition_domains.emplace(width,offset_ex(domain,float(scale_(
                (rounds_contours?1.0:0.5)*ContourRoundingOptions{}.geometry_tolerance_mm)))).first;
        return found->second;
    };

    const auto reject_unsupported = [&](const ExtrusionEntity& entity) {
        const FiberCandidateId candidate_id {domain_id, purpose, job_ordinal, path_ordinal++};
        const double length_mm = unscale<double>(entity.length());
        result.candidates.push_back({candidate_id, std::isfinite(length_mm) ? length_mm : 0.0, requires_closed_loop});
        FiberFragmentAssignment assignment;
        assignment.id = {candidate_id, 0};
        assignment.source_end_mm = std::isfinite(length_mm) ? std::max(0.0, length_mm) : 0.0;
        assignment.kind = FiberAssignmentKind::Rejected;
        assignment.reason = FiberRejectionReason::UnsupportedEntity;
        result.assignments.emplace_back(std::move(assignment));
    };

    const auto process_path = [&](const ExtrusionPath& input) {
        ExtrusionPath path(input);
        if (material_context && (!source_arcs || purpose!=FiberPathPurpose::Infill ||
            !material_context->certificate.matches_source(material_context->source_index,path.polyline,*source_arcs)))
            throw std::runtime_error("Material-certified infill source identity mismatch");
        std::optional<continuous_fiber_detail::FiberFinalizationDomains> contour_domains;
        bool planar = true;
        // Source shaping may ignore other boundaries, but simplification must
        // not create a shortcut outside the physical domain and then clip it
        // as an unavailable interval during allocation.
        // Empty arc metadata permits normalization, but its non-null pointer
        // still identifies a whole generated path for containment below.
        try {
            if (!source_arcs || source_arcs->empty()) path.polyline = fiber_source_geometry(input.polyline);
            // Integer offsets can leave one-grid spurs in a linear candidate.
            // Clean these before freezing source stations, with the existing
            // domain-preserving normalizer. Never simplify analytic arcs.
            if (!rounds_contours) {
                if (purpose == FiberPathPurpose::Infill && source_arcs) {
                    // validate_infill already computed this exact expanded planned domain.
                    path.polyline = normalize_fiber_geometry(path.polyline, physical_centerline_domain);
                } else {
                    const auto cleanup_domain=offset_ex(centerline_domain_for_width(path.width),
                        float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
                    path.polyline = normalize_fiber_geometry(path.polyline, &cleanup_domain);
                }
            }
        }
        catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::invalid_argument&) { planar = false; }
        if (material_context && !material_context->certificate.matches_source(
                material_context->source_index,path.polyline,*source_arcs))
            throw std::runtime_error("Material-certified infill normalization changed source identity");
        const FiberCandidateId candidate_id {domain_id, purpose, job_ordinal, path_ordinal++};
        FiberRejectionReason whole_path_reason = !planar ? FiberRejectionReason::InvalidGeometry :
            validate_path_geometry(path, !rounds_contours);
        std::vector<ContourArc> reference_arcs = source_arcs ? *source_arcs : std::vector<ContourArc>{};
        std::vector<ContourIssue> rounding_issues;
        std::vector<ContourBend> reference_bends;
        std::string stability_detail;
        if (whole_path_reason == FiberRejectionReason::None && rounds_contours) {
            const ContourRoundingOptions options{config.contour_bend_radius_mm};
            const ExPolygons rounding_domain = requires_closed_loop && geometry_domain && planned_centerline_domain ?
                intersection_ex(*geometry_domain, *planned_centerline_domain) :
                (geometry_domain ? *geometry_domain : centerline_domain_for_width(path.width));
            auto rounded=requires_closed_loop ?
                ContinuousFiberFillStrategy::round_outer_contour(path.polyline, rounding_domain, options) :
                ContinuousFiberFillStrategy::round_hole_contour(path.polyline, rounding_domain, options);
            if (!rounded.path) {
                const bool single_issue=rounded.issues.size()==1;
                const auto reason=single_issue?rounded.issues.front().reason:ContourRoundingFailure::SearchNotFound;
                whole_path_reason=single_issue &&
                    (reason==ContourRoundingFailure::InsufficientSpace ||
                     reason==ContourRoundingFailure::SourceOutsideCurrentDomain) ?
                    FiberRejectionReason::UnavailableContourRegion :
                    single_issue && reason==ContourRoundingFailure::InvalidInput ?
                    FiberRejectionReason::InvalidParameter : FiberRejectionReason::ContourRoundingUnresolved;
                rounding_issues=std::move(rounded.issues);
            } else {
                path.polyline=std::move(*rounded.path);
                reference_arcs=std::move(rounded.arcs);
                reference_bends=std::move(rounded.bends);
            }
        }
        if (whole_path_reason==FiberRejectionReason::None && requires_closed_loop &&
            bend_stabilization_length_mm>0) {
            const auto stable=minimum_closed_loop_stable_length_mm(unscale<double>(scale_(Geometry::length_with_arcs_mm(path.polyline))),reference_bends);
            if (stable) stability_detail="minimum_stable_path_mm="+std::to_string(*stable)+
                "; required_mm="+std::to_string(bend_stabilization_length_mm);
            if (stable && *stable+interval_epsilon_mm<bend_stabilization_length_mm) {
                whole_path_reason=FiberRejectionReason::BendStabilizationTooShort;
            } else if (stable) {
                // Begin this accepted ring at the end of its last marked bend.
                // The first bend then has the same stable entry as later bends.
                // This changes the seam only; no point, radius or route is replanned.
                const double seam=reference_bends.back().end_mm;
                const double total=unscale<double>(scale_(Geometry::length_with_arcs_mm(path.polyline)));
                const Point3 contact(reference_bends.back().exit_point,0);
                const auto at=std::find(path.polyline.points.begin(),path.polyline.points.end()-1,contact);
                if (at==path.polyline.points.end()-1)
                    throw std::runtime_error("Marked contour bend endpoint is absent from its path");
                auto head=Geometry::slice_with_arcs_mm(path.polyline,0,seam);
                auto tail=Geometry::slice_with_arcs_mm(path.polyline,seam,total);
                Geometry::append_with_arcs(tail,head);path.polyline=std::move(tail);
                // All marked arcs precede this seam, so their order is retained.
                for (auto& arc:reference_arcs) {
                    arc.begin_mm+=total-seam;arc.end_distance_mm+=total-seam;
                }
                if (config.connect_adjacent_rings) for (auto& bend:reference_bends) {
                    bend.begin_mm+=total-seam;bend.end_mm+=total-seam;
                }
            }
        }
        if (collect_debug && whole_path_reason==FiberRejectionReason::None) result.reference_paths.push_back(path);
        bool mapping_valid=true;
        std::vector<SourceInterval> intervals;
        bool band_source_proved=false;
        if(whole_path_reason==FiberRejectionReason::None && allocates_contour && rounds_contours &&
            !source_arcs && (!requires_closed_loop || path.is_closed())) {
            const double native_delta=double(float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
            const double proof_radius=native_delta/2;
            if(std::isfinite(proof_radius) && proof_radius>=0 && proof_radius<=ClipperLib::loRange &&
                std::floor(proof_radius)==proof_radius) {
                continuous_fiber_detail::FiberFinalizationDomains tentative(*planned_centerline_domain,
                    candidate_id,path.width,native_delta,[&partition_domain_for_width,width=path.width]() -> const ExPolygons& {
                        return partition_domain_for_width(width);
                    });
                if(tentative.proves_planned_band(check_view(path.polyline))) {
                    band_source_proved=true;
                    contour_domains.emplace(std::move(tentative));
                }
            }
        }
        if (whole_path_reason==FiberRejectionReason::None) {
            if (source_arcs) {
                // The generator already produces contained paths. Clipping an
                // arc here would hide a generation error and break its routing.
                if (!material_context) {
                    const auto checked=check_view(path.polyline);
                    const bool contained=containment && containment->supports(checked)?
                        containment->permits(checked,false):diff_pl(Polylines{checked},*physical_centerline_domain).empty();
                    if (!contained)
                        throw std::runtime_error("Generated fiber infill leaves its centerline domain");
                }
                // Material context carries a full-width source proof. The
                // original whole-source C check is deferred until AFTER the
                // unchanged Finalizer process guard, never replaced by a fragment.
                intervals={{0.0,scale_(Geometry::length_with_arcs_mm(path.polyline)),FiberAssignmentKind::AcceptedFiber,FiberRejectionReason::None}};
            } else if (band_source_proved) {
                intervals={{0.0,scale_(Geometry::length_with_arcs_mm(path.polyline)),FiberAssignmentKind::AcceptedFiber,FiberRejectionReason::None}};
                if(allocates_contour && !requires_closed_loop)rotate_reference(path,reference_arcs,intervals);
            } else if (requires_closed_loop) {
                // Do not partition an exterior into printable strands. Geometry,
                // occupancy and process validation belong to the entire loop;
                // no coverage is committed until that single outcome succeeds.
                if (!path.is_closed())
                    whole_path_reason=FiberRejectionReason::OpenOuterContour;
                else if (!diff_pl(Polylines{check_view(path.polyline)}, partition_domain_for_width(path.width)).empty())
                    whole_path_reason=physical_centerline_domain &&
                        diff_pl(Polylines{check_view(path.polyline)}, *physical_centerline_domain).empty() ?
                        FiberRejectionReason::OccupiedContourRegion : FiberRejectionReason::UnavailableContourRegion;
                else
                    intervals={{0.0,scale_(Geometry::length_with_arcs_mm(path.polyline)),FiberAssignmentKind::AcceptedFiber,FiberRejectionReason::None}};
            } else {
                intervals=partition_candidate(path,partition_domain_for_width(path.width),mapping_valid);
                if (!mapping_valid) whole_path_reason=FiberRejectionReason::IntervalMappingFailure;
                else if (allocates_contour) {
                    rotate_reference(path,reference_arcs,intervals);
                }
            }
        }
        // Rejected nonplanar/invalid analytic input still needs an assignment.
        // Measuring it with the analytic validator would throw before the
        // InvalidGeometry outcome can be returned to the caller.
        const double source_length_mm = whole_path_reason == FiberRejectionReason::InvalidGeometry ?
            unscale<double>(path.polyline.length()) : unscale<double>(scale_(Geometry::length_with_arcs_mm(path.polyline)));
        if (whole_path_reason!=FiberRejectionReason::None)
            intervals={{0.0,scale_(source_length_mm),FiberAssignmentKind::Rejected,whole_path_reason}};
        else if (intervals.empty())
            intervals={{0.0,scale_(Geometry::length_with_arcs_mm(path.polyline)),FiberAssignmentKind::Rejected,FiberRejectionReason::OutsideDomain}};
        // Ownership is measured on the shaped reference curve, not falsely
        // projected back to the original polygon after rounding changed its length.
        result.candidates.push_back({candidate_id,std::isfinite(source_length_mm)?source_length_mm:0.0,requires_closed_loop});

        struct PendingInterval { SourceInterval interval; size_t revision; };
        std::deque<PendingInterval> pending;
        for (const auto& interval:intervals) pending.push_back({interval,0});
        size_t fragment_ordinal=0, allocation_revision=0;
        ExPolygons remaining_domain, remaining_partition;
        const ExPolygons* final_domain=planned_centerline_domain;
        while (!pending.empty()) {
            const auto next=pending.front(); pending.pop_front();
            const SourceInterval interval=next.interval;
            if (allocates_contour && next.revision!=allocation_revision &&
                interval.kind==FiberAssignmentKind::AcceptedFiber) {
                // Fragments from one reference have the same occupancy contract
                // as different candidates. Only revisit pending geometry when an
                // earlier fragment was actually accepted and changed the domain.
                ExtrusionPath fragment(path);
                if (!extract_subpath(path.polyline,interval.begin_scaled,interval.end_scaled,fragment.polyline))
                    throw std::runtime_error("Cannot extract pending fiber allocation interval");
                bool mapped=true;
                auto pieces=partition_candidate(fragment,remaining_partition,mapped);
                if (!mapped) throw std::runtime_error("Cannot map pending fiber allocation intervals");
                for (auto i=pieces.rbegin();i!=pieces.rend();++i) {
                    auto part=*i;
                    part.begin_scaled= i->begin_scaled==0 ? interval.begin_scaled : interval.begin_scaled+i->begin_scaled;
                    part.end_scaled= i->end_scaled==scale_(Geometry::length_with_arcs_mm(fragment.polyline)) ? interval.end_scaled : interval.begin_scaled+i->end_scaled;
                    pending.push_front({part,allocation_revision});
                }
                continue;
            }
            FiberFragmentAssignment assignment;
            assignment.id = {candidate_id, fragment_ordinal++};
            assignment.source_begin_mm = unscale<double>(interval.begin_scaled);
            assignment.source_end_mm = unscale<double>(interval.end_scaled);
            assignment.kind = interval.kind;
            assignment.reason = interval.reason;
            assignment.contour_issues = rounding_issues;
            assignment.detail = stability_detail;
            if (requires_closed_loop && assignment.kind == FiberAssignmentKind::Rejected)
                assignment.detail += (assignment.detail.empty()?"":"; ")+
                    std::string("Entire outer loop rejected; no partial deposition was committed");

            for (const auto& issue : rounding_issues) {
                if (!assignment.detail.empty()) assignment.detail += "; ";
                assignment.detail += contour_rounding_failure_name(issue.reason);
            }

            // A valid infill interval below extraction resolution is too short
            // to deposit, not invalid geometry. Retain its full assignment so
            // coverage and interval accounting use the existing short-path rules.
            const double interval_length = interval.end_scaled - interval.begin_scaled;
            if (purpose == FiberPathPurpose::Infill &&
                whole_path_reason == FiberRejectionReason::None &&
                assignment.kind == FiberAssignmentKind::AcceptedFiber &&
                !allowed_domain.empty() &&
                std::isfinite(interval.begin_scaled) && std::isfinite(interval.end_scaled) &&
                interval_length > 0.0 && interval_length <= SCALED_EPSILON &&
                std::all_of(path.polyline.points.begin(), path.polyline.points.end(),
                            [](const Point3& point) { return point.z() == 0; })) {
                assignment.kind = FiberAssignmentKind::Rejected;
                try {
                    validate_fiber_process_config(config, purpose, FiberFinishStrategy::TangentExtension);
                    assignment.reason = FiberRejectionReason::TooShort;
                    assignment.detail = "Infill interval is below path extraction resolution";
                } catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::invalid_argument& error) {
                    assignment.reason = FiberRejectionReason::InvalidParameter;
                    assignment.detail = error.what();
                }
                assignment.centerline.emplace(path);
                result.assignments.emplace_back(std::move(assignment));
                continue;
            }

            if (assignment.reason == FiberRejectionReason::InvalidGeometry) {
                assignment.centerline.emplace(path);
                result.assignments.emplace_back(std::move(assignment));
                continue;
            }

            Polyline3 fragment_geometry;
            if (!extract_subpath(path.polyline, interval.begin_scaled, interval.end_scaled, fragment_geometry)) {
                assignment.kind = FiberAssignmentKind::Rejected;
                assignment.reason = FiberRejectionReason::InvalidGeometry;
                assignment.detail = "Could not extract the assigned fiber path interval";
                assignment.centerline.emplace(path);
                result.assignments.emplace_back(std::move(assignment));
                continue;
            }
            assignment.centerline.emplace(std::move(fragment_geometry), path);
            assignment.centerline->set_extrusion_role(output_role);

            if (allocates_contour && assignment.reason==FiberRejectionReason::OutsideDomain) {
                assignment.kind=FiberAssignmentKind::IntentionalVoid;
                assignment.reason=physical_centerline_domain &&
                    diff_pl(Polylines{check_view(assignment.centerline->polyline)},*physical_centerline_domain).empty() ?
                    FiberRejectionReason::OccupiedContourRegion : FiberRejectionReason::UnavailableContourRegion;
            }
            if (assignment.kind == FiberAssignmentKind::AcceptedFiber) {
                // Rounded geometry is immutable here. Preserve its arc stations;
                // command preparation handles process splits within its own budget.
                const auto arcs=trim_arcs(reference_arcs,assignment.source_begin_mm,assignment.source_end_mm);
                if (config.connect_adjacent_rings) {
                    assignment.source_arcs=arcs;
                    for (auto bend:reference_bends) {
                        const double a=std::max(bend.begin_mm,assignment.source_begin_mm);
                        const double b=std::min(bend.end_mm,assignment.source_end_mm);
                        if (b<=a) continue;
                        bend.begin_mm=a-assignment.source_begin_mm;bend.end_mm=b-assignment.source_begin_mm;
                        assignment.source_bends.push_back(bend);
                    }
                }
                FiberFinalizationResult finalized;
                try {
                    const auto finish=make_independent_fiber_finish_route(*assignment.centerline,config,assignment.id);
                    const auto* effective_domains=finalization_domains;
                    if (rounds_contours && planned_centerline_domain && !source_arcs &&
                        final_domain==planned_centerline_domain) {
                        // This path's partition already owns the identical +100nm domain.
                        // A committed fragment may replace final_domain; recheck every time.
                        if (!contour_domains)
                            contour_domains.emplace(*planned_centerline_domain,
                                partition_domain_for_width(path.width));
                        effective_domains=&*contour_domains;
                    }
                    std::optional<continuous_fiber_detail::FiberInfillFinalizationContext> lazy_context;
                    if (material_context) {
                        lazy_context.emplace(continuous_fiber_detail::FiberInfillFinalizationContext{
                            *material_context,path.polyline,*source_arcs,[&](const continuous_fiber_detail::FiberDomainIndex& index,const ExPolygons& physical) {
                                if (!material_context->certificate.matches_source(material_context->source_index,path.polyline,*source_arcs))
                                    throw std::runtime_error("Material-certified infill lost whole-source identity");
                                const auto checked=check_view(path.polyline);
                                const bool contained=index.supports(checked)?index.permits(checked,false):diff_pl(Polylines{checked},physical).empty();
                                if (!contained)throw std::runtime_error("Generated fiber infill leaves its centerline domain");
                            }});
                    }
                    finalized=FiberPathFinalizer::finalize(
                        *assignment.centerline,allowed_domain,config,assignment.id,finish,arcs,final_domain,{}, {},effective_domains,
                        lazy_context?&*lazy_context:nullptr);
                } catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::invalid_argument& error) {
                    finalized.failure=FiberFinalizationFailure::InvalidParameter;
                    finalized.detail=error.what();
                }
                if (!finalized.prepared) {
                    assignment.kind=FiberAssignmentKind::Rejected;
                    assignment.reason=finalization_reason(finalized.failure);
                    assignment.detail=finalized.detail;
                    if (policy_rejected && finalized.failure == FiberFinalizationFailure::PolicyBoundaryRejected)
                        policy_rejected->push_back(assignment.id);
                } else {
                    assignment.prepared=finalized.prepared;
                    append_coverage(result,*assignment.prepared);
                    if (allocates_contour && !pending.empty()) {
                        remaining_domain=diff_ex(*planned_centerline_domain,
                            fiber_material_offset(result.physical_footprint,0.5*path.width));
                        remaining_partition=offset_ex(remaining_domain,float(scale_(
                            0.5*ContourRoundingOptions{}.geometry_tolerance_mm)));
                        final_domain=&remaining_domain;
                        ++allocation_revision;
                    }
                }
            }
            result.assignments.emplace_back(std::move(assignment));
        }
    };

    for (const ExtrusionEntity* root : candidate_roots) {
        if (root == nullptr)
            continue;
        if (const auto* path = dynamic_cast<const ExtrusionPath*>(root)) {
            process_path(*path);
            continue;
        }
        const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(root);
        if (collection == nullptr) {
            reject_unsupported(*root);
            continue;
        }
        ExtrusionEntityCollection flattened = collection->flatten(false);
        for (const ExtrusionEntity* entity : flattened.entities) {
            if (const auto* path = dynamic_cast<const ExtrusionPath*>(entity))
                process_path(*path);
            else if (entity != nullptr)
                reject_unsupported(*entity);
        }
    }

    result.physical_footprint = union_ex(result.physical_footprint);
    result.resin_exclusion = union_ex(result.resin_exclusion);
    result.outside_domain = union_ex(result.outside_domain);
    result.contour_to_infill_keepout = union_ex(result.contour_to_infill_keepout);
    const FiberAssignmentAudit audit = result.audit_assignments();
    if (!audit.valid())
        throw std::runtime_error("Continuous fiber candidate interval assignment audit failed");
    return result;
}


FiberValidationResult FiberPathValidator::validate(
    const ExtrusionEntitiesPtr& candidates, const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config, FiberPathPurpose purpose, ExtrusionRole output_role,
    const FiberDomainId& domain_id, size_t job_ordinal)
{
    return validate_impl(candidates, allowed_domain, config, purpose, output_role,
                         domain_id, job_ordinal, nullptr, nullptr, nullptr, false);
}

namespace {

// Only a single, fully accepted depositing cycle can own the next level.
std::optional<Polygon> accepted_cycle(const FiberValidationResult& result, const FiberCandidateId& id)
{
    const FiberFragmentAssignment* accepted = nullptr;
    for (const auto& assignment : result.assignments) if (assignment.id.parent == id) {
        if (accepted || assignment.kind != FiberAssignmentKind::AcceptedFiber || !assignment.prepared ||
            !assignment.centerline || !assignment.centerline->is_closed()) return std::nullopt;
        accepted = &assignment;
    }
    if (!accepted) return std::nullopt;
    Polyline3 geometry;
    if(accepted->prepared->entry.lowers())geometry=accepted->prepared->entry.geometry;
    for(const auto& span:accepted->prepared->spans)if(span.deposits_fiber())
        Geometry::append_with_arcs(geometry,span.geometry);
    Points points=check_view(geometry).points;
    if (points.size() < 4 || points.front() != points.back()) return std::nullopt;
    points.pop_back();
    Polygon polygon(std::move(points));
    polygon.make_counter_clockwise();
    return polygon;
}

void merge_validation(FiberValidationResult& into, FiberValidationResult&& from)
{
    append(into.physical_footprint, from.physical_footprint);
    append(into.resin_exclusion, from.resin_exclusion);
    append(into.outside_domain, from.outside_domain);
    append(into.contour_to_infill_keepout, from.contour_to_infill_keepout);
    into.candidates.insert(into.candidates.end(), from.candidates.begin(), from.candidates.end());
    into.assignments.insert(into.assignments.end(), std::make_move_iterator(from.assignments.begin()),
        std::make_move_iterator(from.assignments.end()));
    into.reference_paths.insert(into.reference_paths.end(), std::make_move_iterator(from.reference_paths.begin()),
        std::make_move_iterator(from.reference_paths.end()));
}

} // namespace

FiberValidationResult FiberPathValidator::validate_infill(
    const FiberInfillCandidates& candidates, const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config, const FiberDomainId& domain_id)
{
    FiberValidationResult result;
    result.output_role = erContinuousFiberInfill;
    const bool material_mode=candidates.domain_mode==FiberInfillDomainMode::MaterialCertified2R;
    std::optional<continuous_fiber_detail::FiberLazyInfillDomains> lazy_domains;
    std::optional<ExPolygons> comparison_domain;
    std::unique_ptr<continuous_fiber_detail::FiberDomainIndex> containment;
    std::optional<continuous_fiber_detail::FiberFinalizationDomains> finalization_domains;
    if (material_mode) {
        if (!candidates.material_certificate ||
            !candidates.material_certificate->matches_material(allowed_domain,config.infill_flow.width()) ||
            !candidates.material_certificate->matches_batch(candidates))
            throw std::runtime_error("Material-certified infill batch identity mismatch");
        lazy_domains.emplace(*candidates.material_certificate,config.infill_flow.width());
    } else {
        comparison_domain=offset_ex(candidates.centerline_domain,float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
        containment=std::make_unique<continuous_fiber_detail::FiberDomainIndex>(*comparison_domain);
        finalization_domains.emplace(candidates.centerline_domain,*comparison_domain);
    }
    for (size_t i = 0; i < candidates.paths.size(); ++i) {
        const auto& candidate = candidates.paths[i];
        ExtrusionPath path(erContinuousFiberInfill, config.infill_flow.mm3_per_mm(),
            config.infill_flow.width(), config.infill_flow.height());
        path.polyline = candidate.geometry;
        std::vector<FiberFragmentId> policy_rejected;
        std::optional<continuous_fiber_detail::FiberInfillValidationContext> material_context;
        if (material_mode) material_context.emplace(continuous_fiber_detail::FiberInfillValidationContext{
            *lazy_domains,*candidates.material_certificate,i});
        auto validated = validate_impl({&path}, allowed_domain, config, FiberPathPurpose::Infill,
            erContinuousFiberInfill, domain_id, i, material_mode?nullptr:&candidates.centerline_domain, nullptr,
            material_mode?nullptr:&*comparison_domain, false, false, &candidate.arcs, 0.0, &policy_rejected,
            containment.get(),finalization_domains?&*finalization_domains:nullptr,
            material_context?&*material_context:nullptr);
        for (const auto& assignment : validated.assignments)
            if (assignment.kind == FiberAssignmentKind::Rejected &&
                assignment.reason != FiberRejectionReason::TooShort &&
                assignment.reason != FiberRejectionReason::ProcessBudgetTooShort &&
                std::find(policy_rejected.begin(), policy_rejected.end(), assignment.id) == policy_rejected.end()) {
                std::ostringstream message;
                message.precision(12);
                message << "Generated fiber infill failed preparation: "
                        << fiber_rejection_reason_name(assignment.reason) << "; " << assignment.detail
                        << "; layer=" << domain_id.layer_id << "; component=" << domain_id.component_id
                        << "; candidate=" << i << "; interval_mm=[" << assignment.source_begin_mm
                        << ',' << assignment.source_end_mm << "]; extraction_epsilon_mm=" << EPSILON;
                throw std::runtime_error(message.str());
            }
        merge_validation(result, std::move(validated));
    }
    if (material_mode)
        BOOST_LOG_TRIVIAL(warning) << "FIBER_MATERIAL_QUERY_BATCH mode=MaterialCertified2R candidates=" << candidates.paths.size()
            << " accepted=" << result.accepted_count() << " rejected=" << result.rejected_count()
            << " true_C_resolutions=" << lazy_domains->resolution_count;
    result.physical_footprint = union_ex(result.physical_footprint);
    result.resin_exclusion = union_ex(result.resin_exclusion);
    result.outside_domain = union_ex(result.outside_domain);
    return result;
}

bool FiberContourPlanResult::audit_lineage() const
{
    if (!validation.audit_assignments().valid() || nodes.size() != validation.candidates.size()) return false;
    std::map<FiberCandidateId, const FiberContourPlanNode*> visited;
    for (const auto& node : nodes) {
        if (!visited.emplace(node.id, &node).second) return false;
        const auto candidate = std::find_if(validation.candidates.begin(), validation.candidates.end(),
            [&](const auto& value) { return value.id == node.id; });
        if (candidate == validation.candidates.end()) return false;
        if (!node.parent) {
            if (node.depth != 0) return false;
        } else {
            const auto found = visited.find(*node.parent);
            if (found == visited.end() || !accepted_cycle(validation, *node.parent)) return false;
            const auto& parent = *found->second;
            if (parent.depth + 1 != node.depth || parent.side != node.side ||
                parent.region_id != node.region_id || parent.boundary_id != node.boundary_id ||
                !(parent.id.domain == node.id.domain)) return false;
        }
    }
    return true;
}

void FiberContourPlanResult::set_display_cutoff(size_t contour_depths)
{
    std::map<FiberCandidateId,FiberPathPurpose> labels;
    for (const auto& node:nodes)
        labels.emplace(node.id,node.depth<contour_depths?FiberPathPurpose::Contour:FiberPathPurpose::Infill);
    for (auto& assignment:validation.assignments)
        assignment.display_purpose=labels.at(assignment.id.parent);
}

namespace {

struct RingStageOptions {
    const Flow& flow;
    double boundary_clearance_mm;
    double bend_radius_mm;
    size_t max_depth;
    bool include_holes;
    FiberPathPurpose purpose;
    ExtrusionRole output_role;
    size_t fill_start_depth {fiber_contour_depth_limit};
    double extra_spacing_mm {0.0};
};

// Shared branch traversal for ring-based fiber stages. Candidate validation
// remains with each caller so its geometry and deposition rules stay explicit.
template <class ValidateCandidate>
FiberContourPlanResult plan_ring_stage(
    const ExPolygons& original_area, const FiberDomainId& domain_id,
    bool collect_debug, const RingStageOptions& stage, ValidateCandidate&& validate_candidate)
{
    const double width = stage.flow.width();
    const double clearance = stage.boundary_clearance_mm;
    FiberContourPlanResult plan;
    auto& result = plan.validation;
    result.output_role = stage.output_role;

    struct Branch {
        ExPolygons material;
        ExPolygons hole_frontier;
        std::optional<FiberCandidateId> parent;
        size_t region, boundary;
    };
    // Canonical roots and sibling order make allocation independent of the
    // caller's polygon order and of scheduling of other layer/domain jobs.
    const auto canonical = [](Polygon& polygon) {
        polygon.make_counter_clockwise();
        if (!polygon.points.empty()) std::rotate(polygon.points.begin(),
            std::min_element(polygon.points.begin(), polygon.points.end()), polygon.points.end());
    };
    ExPolygons regions = original_area;
    for (auto& region : regions) {
        canonical(region.contour);
        for (auto& hole : region.holes) canonical(hole);
        std::sort(region.holes.begin(), region.holes.end(), [](const auto& a, const auto& b) {
            return a.points < b.points;
        });
        for (auto& hole : region.holes) hole.make_clockwise();
    }
    std::sort(regions.begin(), regions.end(), [](const auto& a, const auto& b) {
        return a.contour.points < b.contour.points;
    });
    const ExPolygons material = fiber_material_offset(regions, -clearance);
    const ExPolygons physical_domain = fiber_material_offset(material, -0.5 * width);
    const ExPolygons physical_partition = fiber_material_offset(physical_domain,
        0.5 * ContourRoundingOptions{}.geometry_tolerance_mm);
    size_t job = 0;
    for (const auto side : {FiberContourSide::Outer, FiberContourSide::Hole}) {
        if (side == FiberContourSide::Hole && !stage.include_holes) continue;
        std::vector<Branch> current;
        for (size_t region = 0; region < regions.size(); ++region) {
            const auto root = fiber_material_offset(ExPolygons{regions[region]}, -clearance);
            if (side == FiberContourSide::Outer) {
                current.push_back({root, {}, std::nullopt, region, 0});
            } else for (size_t hole = 0; hole < regions[region].holes.size(); ++hole) {
                Polygon boundary = regions[region].holes[hole]; boundary.make_counter_clockwise();
                current.push_back({root, fiber_material_offset(ExPolygons{ExPolygon(boundary)}, clearance),
                    std::nullopt, region, hole});
            }
        }
        for (size_t depth = 0; depth < stage.max_depth && !current.empty(); ++depth) {
            std::vector<Branch> next;
            for (const auto& branch : current) {
                const ExPolygons remaining = diff_ex(branch.material, result.physical_footprint);
                const auto candidates = ContinuousFiberFillStrategy::generate_contour_level(remaining, width,
                    side == FiberContourSide::Hole ? &branch.hole_frontier : nullptr);
                // Hole paths may become open strands. Shape against their own
                // frontier first; allocation then clips other holes and occupied
                // material. Requiring a complete ring inside that clipped domain
                // would reject valid strands before they can be allocated.
                ExPolygons hole_rounding_domain;
                if (side == FiberContourSide::Hole && stage.bend_radius_mm > 0) {
                    const auto frontier = fiber_material_offset(branch.hole_frontier, 0.5 * width);
                    auto bounds = get_extents(frontier);
                    // C/CSC/CCC arcs cannot reach farther than 4R from their
                    // support contacts. This finite bound represents the outside
                    // of the hole; actual material is checked during allocation.
                    bounds.offset(scale_(4 * stage.bend_radius_mm +
                        ContourRoundingOptions{}.geometry_tolerance_mm));
                    hole_rounding_domain = diff_ex(ExPolygons{ExPolygon(bounds.polygon())}, frontier);
                }
                if (candidates.paths.empty())
                    plan.stops.push_back({branch.parent, depth, side, "remaining_region_exhausted"});
                // Generation already eroded the current remaining material.
                // Reuse it until a sibling commits new physical coverage.
                const ExPolygons* available = &candidates.centerline_domain;
                ExPolygons updated_available;
                bool coverage_changed = false;
                for (const auto& candidate : candidates.paths) {
                    const FiberCandidateId id{domain_id, stage.purpose, job++, 0};
                    plan.nodes.push_back({id, branch.parent, depth, side, branch.region, branch.boundary,
                        candidate.part_id, collect_debug ? std::optional<Polyline>(candidate.geometry.to_polyline()) : std::nullopt});
                    ExtrusionPath path(stage.output_role, stage.flow.mm3_per_mm(),
                        stage.flow.width(), stage.flow.height());
                    path.polyline = candidate.geometry;
                    // Earlier siblings may have committed coverage after this
                    // level's geometry was extracted. Recheck against that state.
                    if (coverage_changed) {
                        updated_available = fiber_material_offset(
                            diff_ex(branch.material, result.physical_footprint), -0.5 * width);
                        available = &updated_available;
                    }
                    auto accepted = validate_candidate(path, id.job_ordinal, depth, available,
                        side == FiberContourSide::Outer ? &candidates.geometry_domains.at(candidate.geometry_domain_id) :
                            &hole_rounding_domain, &physical_partition, side == FiberContourSide::Outer);
                    const auto cycle = accepted_cycle(accepted, id);
                    if (cycle && depth + 1 < stage.max_depth) {
                        const ExPolygons enclosed{ExPolygon(*cycle)};
                        const bool spaced=stage.extra_spacing_mm>0 && depth>=stage.fill_start_depth;
                        if (side == FiberContourSide::Outer) {
                            auto child=intersection_ex(remaining,enclosed);
                            if (spaced) child=fiber_material_offset(child,-stage.extra_spacing_mm);
                            next.push_back({std::move(child),{},id,branch.region,branch.boundary});
                        } else {
                            auto frontier=union_ex(enclosed,accepted.physical_footprint);
                            if (spaced) frontier=fiber_material_offset(frontier,stage.extra_spacing_mm);
                            next.push_back({branch.material,std::move(frontier),id,branch.region,branch.boundary});
                        }
                    } else {
                        plan.stops.push_back({id, depth + 1, side, cycle ? "depth_limit" :
                            (accepted.accepted_count() ? "open_deposition_leaf" : "candidate_rejected")});
                    }
                    coverage_changed = !accepted.physical_footprint.empty();
                    merge_validation(result, std::move(accepted));
                    if (coverage_changed)
                        result.physical_footprint = union_ex(result.physical_footprint);
                }
            }
            current = std::move(next);
        }
    }
    result.resin_exclusion = union_ex(result.resin_exclusion);
    result.outside_domain = union_ex(result.outside_domain);
    result.contour_to_infill_keepout = union_ex(result.contour_to_infill_keepout);
    if (!plan.audit_lineage()) throw std::runtime_error("Continuous fiber contour lineage audit failed");
    return plan;
}

} // namespace

FiberContourPlanResult FiberPathValidator::plan_contours(
    const ExPolygons& original_area, const ContinuousFiberConfig& config,
    const FiberDomainId& domain_id, bool collect_debug,size_t fill_start_depth,double extra_spacing_mm,
    double infill_stabilization_length_mm)
{
    const double width = config.contour_flow.width();
    const double clearance = config.contour_boundary_clearance_mm;
    if (!std::isfinite(width) || width <= 0 || !std::isfinite(clearance) || clearance < 0 || config.contour_count < 0 ||
        !std::isfinite(infill_stabilization_length_mm) || infill_stabilization_length_mm<0 ||
        !std::isfinite(extra_spacing_mm) || extra_spacing_mm<0 ||
        extra_spacing_mm>=unscale<double>(std::numeric_limits<coord_t>::max()))
        throw std::invalid_argument("Invalid fiber contour geometry parameters");
    if (config.contour_count == 0) {
        FiberContourPlanResult plan;
        plan.validation.output_role = erContinuousFiberContour;
        return plan;
    }
    validate_fiber_process_config(config, FiberPathPurpose::Contour, FiberFinishStrategy::LoopOverlap);
    const RingStageOptions stage{config.contour_flow, config.contour_boundary_clearance_mm,
        config.contour_bend_radius_mm, size_t(config.contour_count), config.contour_include_holes,
        FiberPathPurpose::Contour, erContinuousFiberContour,fill_start_depth,extra_spacing_mm};
    const auto validate_candidate = [&](ExtrusionPath& path, size_t job_ordinal, size_t depth,
        const ExPolygons* available, const ExPolygons* geometry_domain,
        const ExPolygons* physical_partition, bool requires_closed_loop) {
        auto accepted = validate_impl({&path}, original_area, config, FiberPathPurpose::Contour,
            erContinuousFiberContour, domain_id, job_ordinal, available, geometry_domain,
            physical_partition, collect_debug, requires_closed_loop, nullptr,
            depth>=fill_start_depth?infill_stabilization_length_mm:0.0);
        for (const auto& assignment : accepted.assignments)
            if (assignment.reason == FiberRejectionReason::IntervalMappingFailure ||
                assignment.reason == FiberRejectionReason::InvalidParameter ||
                assignment.reason == FiberRejectionReason::FinalizedPathOutsideDomain)
                throw std::runtime_error("Fiber contour planning failed at layer " + std::to_string(domain_id.layer_id + 1) +
                    ", candidate " + std::to_string(job_ordinal) + ": " +
                    fiber_rejection_reason_name(assignment.reason) + "; " + assignment.detail);
        return accepted;
    };
    return plan_ring_stage(original_area, domain_id, collect_debug, stage, validate_candidate);
}

namespace {

constexpr double gate_numeric_tie_mm=1e-9;

struct FrozenRingView {
    const FiberFragmentAssignment* source;
    std::vector<double> stations {0};
    Polyline3 check_view;
    Linesf segments;
    AABBTreeIndirect::Tree<2,double> index;
    double length;

    explicit FrozenRingView(const FiberFragmentAssignment& value):source(&value),
        length(value.source_end_mm-value.source_begin_mm)
    {
        check_view=Geometry::sample_with_arcs(value.centerline->polyline,
            ContourRoundingOptions{}.chord_tolerance_mm,&stations);
        const auto& points=check_view.points;
        for (size_t i=1;i<points.size();++i)
            segments.emplace_back(points[i-1].head<2>().cast<double>()*SCALING_FACTOR,
                points[i].head<2>().cast<double>()*SCALING_FACTOR);
        if (std::abs(stations.back()-length)>ContourRoundingOptions{}.geometry_tolerance_mm)
            throw std::invalid_argument("Frozen fiber source length differs from its geometry");
        stations.back()=length;
        index=AABBTreeLines::build_aabb_tree_over_indexed_lines(segments);
    }
    double wrap(double s) const {const double r=std::fmod(s,length);return r<0?r+length:r;}
    double canonical(double s) const {
        if (s<=0) return source->source_begin_mm;
        if (s>=length) return source->source_end_mm;
        return std::clamp(source->source_begin_mm+s,source->source_begin_mm,source->source_end_mm);
    }
    Vec2d position(double s) const {return at(s).head<2>().cast<double>()*SCALING_FACTOR;}
    Point3 at(double s) const {
        return Geometry::point_at_length_with_arcs_mm(source->centerline->polyline,std::clamp(s,0.,length));
    }
    Polyline3 interval(double begin,double end) const {
        auto result=Geometry::slice_with_arcs_mm(source->centerline->polyline,
            std::clamp(begin,0.,length),std::clamp(end,0.,length));
        if(result.points.size()<2) throw std::invalid_argument("source_interval_below_coordinate_representation");
        return result;
    }
};

struct ChainGate {double parent_station,child_station,distance;Point3 parent,child;};
struct ContactInterval {double p0,p1,q0,q1,distance;};

double projection_fraction(const Vec2d& point,const Linef& line)
{
    const Vec2d v=line.b-line.a;
    return std::clamp((point-line.a).dot(v)/v.squaredNorm(),0.0,1.0);
}

// This is finite-segment proximity. Parallel contacts are joined into their
// complete continuous contact interval before choosing its midpoint.
ChainGate nearest_ring_gate(const FrozenRingView& parent,const FrozenRingView& child)
{
    size_t hit=0;Vec2d point;
    const double initial=AABBTreeLines::squared_distance_to_indexed_lines(
        child.segments,child.index,parent.segments.front().a,hit,point);
    if (initial<0) throw std::invalid_argument("empty_source_segment_index");
    double distance=std::sqrt(initial);
    std::vector<ContactInterval> contacts;
    const auto accept=[&](ContactInterval value) {
        if (value.distance<distance-gate_numeric_tie_mm) {distance=value.distance;contacts.clear();}
        if (value.distance<=distance+gate_numeric_tie_mm) contacts.push_back(value);
    };
    for (size_t i=0;i<parent.segments.size();++i) {
        const auto& p=parent.segments[i];
        AABBTreeIndirect::Tree<2,double>::BoundingBox box(p.a,p.a);box.extend(p.b);
        box.min().array()-=distance+gate_numeric_tie_mm;box.max().array()+=distance+gate_numeric_tie_mm;
        AABBTreeIndirect::traverse(child.index,[&](const auto& node){return box.intersects(node.bbox);},
            [&](const auto& node) {
                const size_t j=node.idx;const auto& q=child.segments[j];
                const double lp=parent.stations[i+1]-parent.stations[i],lq=child.stations[j+1]-child.stations[j];
                const auto add=[&](double u,double v) {
                    const double a=parent.stations[i]+u*lp,b=child.stations[j]+v*lq;
                    accept({a,a,b,b,(p.a+u*(p.b-p.a)-q.a-v*(q.b-q.a)).norm()});
                };
                add(0,projection_fraction(p.a,q));add(1,projection_fraction(p.b,q));
                add(projection_fraction(q.a,p),0);add(projection_fraction(q.b,p),1);
                const Vec2d t=(p.b-p.a).normalized();
                if (std::abs(cross2(t,(q.b-q.a).normalized()))<=1e-12) {
                    const double qa=(q.a-p.a).dot(t),qb=(q.b-p.a).dot(t);
                    const double a=std::max(0.0,std::min(qa,qb)),b=std::min((p.b-p.a).norm(),std::max(qa,qb));
                    if (b>=a) {
                        const Vec2d pa=p.a+a*t,pb=p.a+b*t;
                        accept({parent.stations[i]+a/(p.b-p.a).norm()*lp,
                            parent.stations[i]+b/(p.b-p.a).norm()*lp,
                            child.stations[j]+projection_fraction(pa,q)*lq,
                            child.stations[j]+projection_fraction(pb,q)*lq,std::abs(cross2(t,q.a-p.a))});
                    }
                }
                const auto& pp=parent.check_view.points;
                const auto& qp=child.check_view.points;
                if (Geometry::segments_intersect(pp[i].head<2>(),pp[i+1].head<2>(),qp[j].head<2>(),qp[j+1].head<2>())) distance=0;
                return true;
            });
    }
    contacts.erase(std::remove_if(contacts.begin(),contacts.end(),[&](const auto& c){
        return c.distance>distance+gate_numeric_tie_mm;}),contacts.end());
    std::sort(contacts.begin(),contacts.end(),[](const auto& a,const auto& b){
        return std::tie(a.p0,a.p1,a.q0,a.q1)<std::tie(b.p0,b.p1,b.q0,b.q1);});
    std::vector<ContactInterval> merged;
    for (auto value:contacts) {
        if (!merged.empty() && value.p0<=merged.back().p1+gate_numeric_tie_mm) {
            auto& last=merged.back();
            const double slope=last.p1>last.p0?(last.q1-last.q0)/(last.p1-last.p0):
                value.p1>value.p0?(value.q1-value.q0)/(value.p1-value.p0):0.0;
            const double expected=last.q0+slope*(value.p0-last.p0);
            const double shift=std::round((expected-value.q0)/child.length)*child.length;
            if (std::abs(value.q0+shift-expected)<=gate_numeric_tie_mm) {
                if (value.p1>last.p1) {last.p1=value.p1;last.q1=value.q1+shift;}continue;
            }
        }
        merged.push_back(value);
    }
    if (merged.size()>1 && merged.front().p0<=gate_numeric_tie_mm && merged.back().p1>=parent.length-gate_numeric_tie_mm) {
        const auto first=merged.front(),last=merged.back();
        const double shift=std::round((last.q1-first.q0)/child.length)*child.length;
        if (std::abs(last.q1-first.q0-shift)<=gate_numeric_tie_mm) {
            merged.back()={last.p0,first.p1+parent.length,last.q0,first.q1+shift,distance};merged.erase(merged.begin());
        }
    }
    if (merged.empty()) throw std::invalid_argument("finite_nearest_contact_unavailable");
    std::pair<double,double> station{std::numeric_limits<double>::infinity(),0};
    for (const auto& contact:merged) station=std::min(station,std::make_pair(
        parent.wrap(0.5*(contact.p0+contact.p1)),child.wrap(0.5*(contact.q0+contact.q1))));
    // The AABB/chord search selects candidates; publish exact points on the circles.
    const Point3 p=parent.at(station.first),q=child.at(station.second);
    return {station.first,station.second,unscale<double>((p-q).cast<double>().norm()),p,q};
}

ChainGate inherited_ring_gate(const FrozenRingView& parent,const FrozenRingView& child,double seam)
{
    const Point3 p=parent.at(parent.wrap(seam));const Vec2d query=parent.position(parent.wrap(seam));
    size_t hit=0;Vec2d nearest;
    const double squared=AABBTreeLines::squared_distance_to_indexed_lines(child.segments,child.index,query,hit,nearest);
    if (squared<0) throw std::invalid_argument("empty_source_segment_index");
    const double distance=std::sqrt(squared);
    double station=std::numeric_limits<double>::infinity();
    const auto matches=AABBTreeLines::all_lines_in_radius(child.segments,child.index,query,
        (distance+gate_numeric_tie_mm)*(distance+gate_numeric_tie_mm));
    for (const size_t i:matches) {
        const double t=projection_fraction(query,child.segments[i]);
        const Vec2d q=child.segments[i].a+t*(child.segments[i].b-child.segments[i].a);
        if ((q-query).norm()<=distance+gate_numeric_tie_mm)
            station=std::min(station,child.wrap(child.stations[i]+t*(child.stations[i+1]-child.stations[i])));
    }
    if (!std::isfinite(station)) throw std::invalid_argument("fixed_seam_projection_unavailable");
    station=Geometry::project_with_arcs_mm(child.source->centerline->polyline,p);
    const Point3 q=child.at(station);
    return {seam,station,unscale<double>((p-q).cast<double>().norm()),p,q};
}

struct RingChainDraft {std::vector<size_t> sources;std::vector<double> seams;std::vector<size_t> links;};
struct OpenBendEvent {double begin,end;size_t source,bend;bool applicable;};
struct OpenChainGeometry {
    Polyline3 path;
    std::vector<FiberExecutionPiece> pieces;
    std::vector<FiberChainSourceSlice> sources;
    std::vector<ContourArc> arcs;
    std::vector<OpenBendEvent> bends;
    std::vector<std::pair<Point3,Point3>> bridges;
};

void append_chain_geometry(Polyline3& into,const Polyline3& part)
{
    if (part.points.size()<2 || (!into.points.empty() && into.points.back()!=part.points.front()))
        throw std::invalid_argument("disconnected_open_chain_piece");
    Geometry::append_with_arcs(into,part);
}

std::vector<std::pair<double,double>> forward_source_intervals(const FrozenRingView& source,double start,double length)
{
    start=source.wrap(start);
    if (length==source.length) return start==0?std::vector<std::pair<double,double>>{{0,source.length}}:
        std::vector<std::pair<double,double>>{{start,source.length},{0,start}};
    std::vector<std::pair<double,double>> result;
    const double end=start+length;
    if (end<=source.length) result.emplace_back(start,end);
    else {result.emplace_back(start,source.length);result.emplace_back(0,std::min(source.length,end-source.length));}
    return result;
}

std::vector<std::pair<double,double>> retained_source_intervals(
    const FrozenRingView& source,double seam,double head,double tail)
{
    if (head+tail==0) return forward_source_intervals(source,seam,source.length);
    const double begin=source.wrap(seam+head),end=source.wrap(seam-tail);
    if (begin<end) return {{begin,end}};
    std::vector<std::pair<double,double>> result{{begin,source.length}};
    if (end>0) result.emplace_back(0,end);
    return result;
}

// Canonical source intervals remain unchanged when geometry is quantized or
// normalized. Execution stations always measure the actual planning polyline.
void append_frozen_intervals(OpenChainGeometry& out,const FrozenRingView& source,size_t source_index,
    const std::vector<std::pair<double,double>>& intervals,bool metadata=true)
{
    for (const auto& range:intervals) {
        if (range.second-range.first<=1e-12) continue;
        const auto geometry=source.interval(range.first,range.second);
        // Continue the same per-edge millimetre sum used by the finalizer.
        // Recomputing unscale(sum(edges)) at the next piece would produce a
        // different last bit from sum(unscale(edges)) at this boundary.
        const double begin=out.pieces.empty()?0.0:out.pieces.back().execution_end_mm;
        FiberExecutionPiece piece;piece.source=source.source->id;
        piece.source_begin_mm=source.canonical(range.first);piece.source_end_mm=source.canonical(range.second);
        piece.execution_begin_mm=begin;
        piece.display_purpose=source.source->display_purpose.value_or(source.source->id.parent.purpose);
        const double length=Geometry::length_with_arcs_mm(geometry);
        piece.execution_end_mm=begin+length;
        std::vector<std::pair<double,double>> chart{{begin,piece.source_begin_mm},
            {piece.execution_end_mm,piece.source_end_mm}};
        piece.source_station_knots=std::make_shared<const std::vector<std::pair<double,double>>>(std::move(chart));
        out.pieces.push_back(piece);append_chain_geometry(out.path,geometry);
        if (!metadata) continue;
        const auto execution_at=[&](double local_source_station) {
            const double station=source.canonical(std::clamp(local_source_station,range.first,range.second));
            const auto& knots=*piece.source_station_knots;
            if (station<=piece.source_begin_mm) return piece.execution_begin_mm;
            if (station>=piece.source_end_mm) return piece.execution_end_mm;
            const auto next=std::upper_bound(knots.begin(),knots.end(),station,
                [](double value,const auto& knot){return value<knot.second;});
            const auto& previous=*(next-1);
            const double t=(station-previous.second)/(next->second-previous.second);
            return previous.first+t*(next->first-previous.first);
        };
        for (auto arc:trim_arcs(source.source->source_arcs,range.first,range.second)) {
            arc.begin_mm=execution_at(range.first+arc.begin_mm);
            arc.end_distance_mm=execution_at(range.first+arc.end_distance_mm);out.arcs.push_back(arc);
        }
        for (size_t i=0;i<source.source->source_bends.size();++i) {
            const auto& bend=source.source->source_bends[i];
            const double a=std::max(range.first,bend.begin_mm),b=std::min(range.second,bend.end_mm);
            if (b>a) out.bends.push_back({execution_at(a),execution_at(b),
                source_index,i,piece.display_purpose==FiberPathPurpose::Infill});
        }
    }
}

void append_frozen_span(OpenChainGeometry& out,const FrozenRingView& source,size_t source_index,
    double start,double length,bool metadata=true)
{
    append_frozen_intervals(out,source,source_index,forward_source_intervals(source,start,length),metadata);
}

OpenChainGeometry build_open_chain(const RingChainDraft& draft,const std::vector<FrozenRingView>& sources,
    const ContinuousFiberConfig& config)
{
    OpenChainGeometry result;
    for (size_t i=0;i<draft.sources.size();++i) {
        const size_t index=draft.sources[i];const auto& source=sources[index];
        const bool connected=draft.sources.size()>1;
        const double seam=connected?draft.seams[i]:0;
        const double head=connected && i>0?config.connection_head_trim_widths*source.source->centerline->width:0;
        const double tail=connected && i+1<draft.sources.size()?config.connection_tail_trim_widths*source.source->centerline->width:0;
        if (!std::isfinite(head+tail) || head+tail>=source.length)
            throw std::invalid_argument("nonpositive_retained_source_length");
        FiberChainSourceSlice slice;slice.source=source.source->id;slice.seam_mm=source.canonical(seam);
        slice.head_mm=head;slice.tail_mm=tail;slice.original_mm=source.length;slice.retained_mm=source.length-head-tail;
        const auto retained=retained_source_intervals(source,seam,head,tail);
        for (const auto& range:retained)
            slice.retained_intervals.emplace_back(source.canonical(range.first),source.canonical(range.second));
        std::sort(slice.retained_intervals.begin(),slice.retained_intervals.end());
        if (head+tail==0) slice.retained_intervals={{source.source->source_begin_mm,source.source->source_end_mm}};
        result.sources.push_back(std::move(slice));
        if (i>0) {
            const auto& previous=sources[draft.sources[i-1]];
            const Point3 a=result.path.points.back(),b=source.at(source.wrap(seam+head));
            if (a==b) throw std::invalid_argument("zero_length_chain_bridge");
            FiberExecutionPiece bridge;bridge.kind=FiberExecutionPieceKind::Transition;
            bridge.source=previous.source->id;bridge.other_source=source.source->id;bridge.transition_id=draft.links[i-1];
            bridge.display_purpose=previous.source->display_purpose.value_or(previous.source->id.parent.purpose)==FiberPathPurpose::Contour ||
                source.source->display_purpose.value_or(source.source->id.parent.purpose)==FiberPathPurpose::Contour?
                FiberPathPurpose::Contour:FiberPathPurpose::Infill;
            bridge.execution_begin_mm=result.pieces.back().execution_end_mm;
            Polyline3 line;line.points={a,b};append_chain_geometry(result.path,line);
            bridge.execution_end_mm=bridge.execution_begin_mm+unscale<double>((b-a).cast<double>().norm());
            result.pieces.push_back(bridge);result.bridges.emplace_back(a,b);
        }
        const size_t first_source_piece=result.pieces.size();
        append_frozen_intervals(result,source,index,retained);
        if (result.pieces.size()==first_source_piece || result.path.points.size()<2)
            throw std::invalid_argument("source_interval_below_coordinate_representation");
    }
    return result;
}

bool open_chain_stability(const std::vector<OpenBendEvent>& input,double required)
{
    if (required<=0 || input.empty()) return true;
    auto ordered=input;
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return std::tie(a.begin,a.end)<std::tie(b.begin,b.end);});
    std::vector<OpenBendEvent> events;
    for (const auto& bend:ordered) {
        if (!events.empty() && events.back().source==bend.source && events.back().bend==bend.bend &&
            bend.begin<=events.back().end+interval_epsilon_mm) events.back().end=std::max(events.back().end,bend.end);
        else events.push_back(bend);
    }
    double previous_end=0;
    for (const auto& bend:events) {
        if (bend.applicable && bend.begin-previous_end+interval_epsilon_mm<required) return false;
        previous_end=bend.end;
    }
    return true;
}

bool intersects_other_chains(const Polyline3& candidate,const std::vector<std::optional<RingChainDraft>>& drafts,
    const std::vector<FrozenRingView>& sources,const ContinuousFiberConfig& config,size_t replaced,size_t child)
{
    Linesf lines;std::vector<std::pair<Point,Point>> exact;
    for (size_t i=0;i<drafts.size();++i) if (drafts[i] && i!=replaced && i!=child) {
        const auto path=check_view(build_open_chain(*drafts[i],sources,config).path);
        for (size_t j=1;j<path.points.size();++j) {
            lines.emplace_back(path.points[j-1].cast<double>(),path.points[j].cast<double>());
            exact.emplace_back(path.points[j-1],path.points[j]);
        }
    }
    if (lines.empty()) return false;
    const auto tree=AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    const auto tested=check_view(candidate);
    for (size_t i=1;i<tested.points.size();++i) {
        const auto& a=tested.points[i-1];const auto& b=tested.points[i];
        AABBTreeIndirect::Tree<2,double>::BoundingBox box(a.cast<double>(),a.cast<double>());box.extend(b.cast<double>());
        bool hit=false;
        AABBTreeIndirect::traverse(tree,[&](const auto& node){return box.intersects(node.bbox);},
            [&](const auto& node){hit=Geometry::segments_intersect(a,b,exact[node.idx].first,exact[node.idx].second);return !hit;});
        if (hit) return true;
    }
    return false;
}

std::vector<double> geometry_stations(const Polyline3& geometry)
{
    return Geometry::stations_with_arcs_mm(geometry);
}

void normalize_open_chain(OpenChainGeometry& geometry,const ExPolygons& domain)
{
    std::vector<double> old;
    geometry.path=normalize_fiber_chain_geometry(geometry.path,geometry.pieces,&domain,&old);
    const auto current=geometry_stations(geometry.path);
    const auto station=[&](double value) {
        if (value<=0) return 0.0;
        if (value>=old.back()) return current.back();
        const size_t i=size_t(std::upper_bound(old.begin(),old.end(),value)-old.begin());
        const double t=(value-old[i-1])/(old[i]-old[i-1]);
        return current[i-1]+t*(current[i]-current[i-1]);
    };
    for (auto& arc:geometry.arcs) {arc.begin_mm=station(arc.begin_mm);arc.end_distance_mm=station(arc.end_distance_mm);}
    for (auto& bend:geometry.bends) {bend.begin=station(bend.begin);bend.end=station(bend.end);}
}

Polyline3 source_chain_deposition(const PreparedFiberPath& prepared)
{
    Polyline3 result;
    if(prepared.entry.lowers())result=prepared.entry.geometry;
    for(const auto& span:prepared.spans)if(span.deposits_fiber())append_chain_geometry(result,span.geometry);
    return result;
}

std::vector<Polyline3> actual_chain_deposition(const PreparedFiberPath& prepared)
{
    return {source_chain_deposition(prepared)};
}

ExPolygons chain_contour_keepout(const FiberChainExecution& chain,const ContinuousFiberConfig& config)
{
    if(chain.prepared->command_pieces.empty() && chain.pieces.front().display_purpose!=FiberPathPurpose::Contour)return {};
    return chain.prepared->contour_to_infill_keepout;
}

double canonical_at(const FiberExecutionPiece& piece,double station)
{
    if (station<=piece.execution_begin_mm) return piece.source_begin_mm;
    if (station>=piece.execution_end_mm) return piece.source_end_mm;
    if (piece.source_station_knots) {
        const auto& chart=*piece.source_station_knots;
        const auto it=std::upper_bound(chart.begin(),chart.end(),station,
            [](double s,const auto& knot){return s<knot.first;});
        if (it==chart.begin()) return chart.front().second;
        if (it==chart.end()) return chart.back().second;
        const auto previous=std::prev(it);
        const double t=(station-previous->first)/(it->first-previous->first);
        return std::clamp(previous->second+t*(it->second-previous->second),piece.source_begin_mm,piece.source_end_mm);
    }
    return std::clamp(piece.source_begin_mm+(station-piece.execution_begin_mm)/
        (piece.execution_end_mm-piece.execution_begin_mm)*(piece.source_end_mm-piece.source_begin_mm),
        piece.source_begin_mm,piece.source_end_mm);
}

std::vector<std::pair<double,double>> coalesced_ranges(std::vector<std::pair<double,double>> ranges)
{
    std::sort(ranges.begin(),ranges.end());std::vector<std::pair<double,double>> result;
    for (const auto& range:ranges) {
        if (!std::isfinite(range.first) || !std::isfinite(range.second) || range.second<=range.first)
            throw std::invalid_argument("invalid_open_chain_source_interval");
        if (!result.empty() && range.first<result.back().second-interval_epsilon_mm)
            throw std::invalid_argument("duplicate_open_chain_source_interval");
        if (!result.empty() && std::abs(range.first-result.back().second)<=interval_epsilon_mm) result.back().second=range.second;
        else result.push_back(range);
    }
    return result;
}

bool same_ranges(std::vector<std::pair<double,double>> a,std::vector<std::pair<double,double>> b)
{
    a=coalesced_ranges(std::move(a));b=coalesced_ranges(std::move(b));
    if (a.size()!=b.size()) return false;
    for (size_t i=0;i<a.size();++i)
        if (std::abs(a[i].first-b[i].first)>interval_epsilon_mm || std::abs(a[i].second-b[i].second)>interval_epsilon_mm) return false;
    return true;
}

} // namespace

bool FiberValidationResult::audit_open_chains() const
{
    if (chain_executions.empty()) return true;
    std::map<FiberFragmentId,const FiberFragmentAssignment*> frozen;
    for (const auto& source:assignments) if (source.kind==FiberAssignmentKind::AcceptedFiber)
        if (!frozen.emplace(source.id,&source).second) return false;
    std::set<FiberFragmentId> visited;std::set<size_t> bridge_ids;
    const auto display=[&](const FiberFragmentId& id) {
        const auto& source=*frozen.at(id);
        return source.display_purpose.value_or(source.id.parent.purpose);
    };
    try {
        for (const auto& chain:chain_executions) {
            if (!chain.prepared || chain.sources.empty() || chain.pieces.empty()) return false;
            chain.prepared->validate();
            const bool connected=chain.sources.size()>1;
            if (connected && chain.centerline.is_closed()) return false;
            if (!connected && (chain.prepared!=frozen.at(chain.sources.front().source)->prepared ||
                chain.sources.front().head_mm!=0 || chain.sources.front().tail_mm!=0)) return false;
            std::map<FiberFragmentId,std::vector<std::pair<double,double>>> expected,observed,command_ranges;
            std::map<FiberFragmentId,FrozenRingView> views;
            for (size_t i=0;i<chain.sources.size();++i) {
                const auto& slice=chain.sources[i];
                if (!frozen.count(slice.source) || !visited.insert(slice.source).second) return false;
                const auto& source=*frozen.at(slice.source);
                const auto& view=views.emplace(slice.source,FrozenRingView(source)).first->second;
                if ((connected && !source.centerline->is_closed()) || !std::isfinite(slice.seam_mm) ||
                    !std::isfinite(slice.head_mm) || !std::isfinite(slice.tail_mm) || slice.head_mm<0 || slice.tail_mm<0 ||
                    !std::isfinite(slice.original_mm) || !std::isfinite(slice.retained_mm) ||
                    (i==0 && slice.head_mm!=0) || (i+1==chain.sources.size() && slice.tail_mm!=0) ||
                    slice.seam_mm<source.source_begin_mm || slice.seam_mm>source.source_end_mm ||
                    std::abs(slice.original_mm-view.length)>interval_epsilon_mm ||
                    std::abs(slice.retained_mm-(view.length-slice.head_mm-slice.tail_mm))>interval_epsilon_mm || slice.retained_mm<=0)
                    return false;
                auto& ranges=expected[slice.source];
                if (slice.head_mm+slice.tail_mm==0) ranges={{source.source_begin_mm,source.source_end_mm}};
                else for (const auto& range:retained_source_intervals(view,
                    slice.seam_mm-source.source_begin_mm,slice.head_mm,slice.tail_mm))
                    ranges.emplace_back(view.canonical(range.first),view.canonical(range.second));
                for (const auto& range:slice.retained_intervals)
                    if (range.first<source.source_begin_mm || range.second>source.source_end_mm) return false;
                if (!same_ranges(ranges,slice.retained_intervals)) return false;
            }
            double station=0;size_t source_index=0,bridge_count=0;
            const auto positions=geometry_stations(chain.centerline.polyline);
            for (const auto& piece:chain.pieces) {
                if ((piece.kind!=FiberExecutionPieceKind::RetainedSource && piece.kind!=FiberExecutionPieceKind::Transition) ||
                    (piece.display_purpose!=FiberPathPurpose::Contour && piece.display_purpose!=FiberPathPurpose::Infill) ||
                    !std::isfinite(piece.source_begin_mm) || !std::isfinite(piece.source_end_mm) ||
                    !std::isfinite(piece.execution_begin_mm) || !std::isfinite(piece.execution_end_mm) ||
                    std::abs(piece.execution_begin_mm-station)>interval_epsilon_mm || piece.execution_end_mm<=piece.execution_begin_mm ||
                    source_index>=chain.sources.size() || !(piece.source==chain.sources[source_index].source)) return false;
                station=piece.execution_end_mm;
                if (piece.kind==FiberExecutionPieceKind::Transition) {
                    if (source_index+1>=chain.sources.size() || !(piece.other_source==chain.sources[source_index+1].source) ||
                        !piece.transition_id || !bridge_ids.insert(piece.transition_id).second) return false;
                    const auto expected_display=display(piece.source)==FiberPathPurpose::Contour ||
                        display(piece.other_source)==FiberPathPurpose::Contour?FiberPathPurpose::Contour:FiberPathPurpose::Infill;
                    if (piece.display_purpose!=expected_display) return false;
                    ++source_index;++bridge_count;
                } else {
                    const auto& source=*frozen.at(piece.source);
                    if (piece.source_begin_mm<source.source_begin_mm || piece.source_end_mm>source.source_end_mm ||
                        piece.source_end_mm<=piece.source_begin_mm || piece.display_purpose!=display(piece.source)) return false;
                    observed[piece.source].emplace_back(piece.source_begin_mm,piece.source_end_mm);
                    const auto& view=views.at(piece.source);
                    for (size_t i=0;i<positions.size();++i) if (positions[i]>=piece.execution_begin_mm-1e-8 &&
                        positions[i]<=piece.execution_end_mm+1e-8) {
                        const double canonical=canonical_at(piece,positions[i])-source.source_begin_mm;
                        const Vec2d actual=chain.centerline.polyline.points[i].head<2>().cast<double>()*SCALING_FACTOR;
                        if ((actual-view.position(canonical)).norm()>ContourRoundingOptions{}.geometry_tolerance_mm) return false;
                    }
                }
            }
            if (bridge_count+1!=chain.sources.size() || source_index+1!=chain.sources.size() ||
                std::abs(station-unscale<double>(scale_(Geometry::length_with_arcs_mm(chain.centerline.polyline))))>interval_epsilon_mm) return false;
            for (const auto& slice:chain.sources) if (!same_ranges(expected[slice.source],observed[slice.source])) return false;
            for (const auto& entry_piece:chain.prepared->entry.pieces) {
                const auto planned=std::find_if(chain.pieces.begin(),chain.pieces.end(),[&](const auto& piece) {
                    return entry_piece.execution_begin_mm>=piece.execution_begin_mm &&
                           entry_piece.execution_end_mm<=piece.execution_end_mm;
                });
                if (planned==chain.pieces.end() || !(planned->source==entry_piece.source) ||
                    entry_piece.kind!=FiberExecutionPieceKind::RetainedSource ||
                    std::abs(entry_piece.source_begin_mm-canonical_at(*planned,entry_piece.execution_begin_mm))>interval_epsilon_mm ||
                    std::abs(entry_piece.source_end_mm-canonical_at(*planned,entry_piece.execution_end_mm))>interval_epsilon_mm) return false;
                command_ranges[entry_piece.source].emplace_back(entry_piece.source_begin_mm,entry_piece.source_end_mm);
            }
            for (const auto& command:chain.prepared->command_pieces) {
                const bool depositing=chain.prepared->spans.at(command.span_index).deposits_fiber();
                const auto& pieces=depositing?chain.pieces:chain.finish_pieces;
                const auto& actual=command.origin;
                const double middle=0.5*(actual.execution_begin_mm+actual.execution_end_mm);
                const auto planned=std::lower_bound(pieces.begin(),pieces.end(),middle,
                    [](const auto& p,double value){return p.execution_end_mm<value;});
                if (planned==pieces.end() || planned->kind!=actual.kind || !(planned->source==actual.source) ||
                    planned->display_purpose!=actual.display_purpose ||
                    actual.execution_begin_mm<planned->execution_begin_mm-interval_epsilon_mm ||
                    actual.execution_end_mm>planned->execution_end_mm+interval_epsilon_mm) return false;
                if (actual.kind==FiberExecutionPieceKind::Transition) {
                    if (actual.transition_id!=planned->transition_id || !(actual.other_source==planned->other_source)) return false;
                } else {
                    if (!frozen.count(actual.source)) return false;
                    const auto& source=*frozen.at(actual.source);
                    if (actual.source_begin_mm<source.source_begin_mm || actual.source_end_mm>source.source_end_mm ||
                        std::abs(actual.source_begin_mm-canonical_at(*planned,actual.execution_begin_mm))>interval_epsilon_mm ||
                        std::abs(actual.source_end_mm-canonical_at(*planned,actual.execution_end_mm))>interval_epsilon_mm) return false;
                    if (depositing) command_ranges[actual.source].emplace_back(actual.source_begin_mm,actual.source_end_mm);
                }
            }
            if (connected) {
                if (chain.prepared->command_pieces.empty()) return false;
                for (const auto& slice:chain.sources) if (!same_ranges(expected[slice.source],command_ranges[slice.source])) return false;
                for (const auto& piece:chain.finish_pieces)
                    if (piece.kind!=FiberExecutionPieceKind::RetainedSource || !(piece.source==chain.sources.back().source) ||
                        piece.display_purpose!=display(piece.source)) return false;
                if (!chain.finish_pieces.empty() && chain.prepared->finish_strategy!=FiberFinishStrategy::LoopOverlap) return false;
            }
        }
    } catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::exception&) {return false;}
    return visited.size()==frozen.size();
}

void FiberPathValidator::plan_open_ring_chains(FiberContourPlanResult& plan,const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,bool collect_debug)
{
    (void)collect_debug;
    if (!config.connect_adjacent_rings) return;
    if (!std::isfinite(config.connection_head_trim_widths) || config.connection_head_trim_widths<0 ||
        !std::isfinite(config.connection_tail_trim_widths) || config.connection_tail_trim_widths<0)
        throw std::invalid_argument("Invalid open fiber chain trim parameters");
    if (!plan.audit_lineage() || !plan.validation.chain_executions.empty())
        throw std::invalid_argument("Invalid or already published frozen fiber source plan");
    auto& original=plan.validation;
    std::vector<FrozenRingView> sources;std::map<FiberCandidateId,size_t> closed;
    std::vector<std::optional<RingChainDraft>> drafts;
    for (const auto& source:original.assignments) if (source.kind==FiberAssignmentKind::AcceptedFiber) {
        const size_t index=sources.size();sources.emplace_back(source);
        drafts.push_back(RingChainDraft{{index},{0},{}});
        if (source.centerline->is_closed()) closed.emplace(source.id.parent,index);
    }
    std::map<FiberCandidateId,const FiberContourPlanNode*> nodes;
    std::map<size_t,std::vector<std::pair<size_t,size_t>>> children;
    plan.chain_decisions.clear();
    for (const auto& node:plan.nodes) {
        nodes.emplace(node.id,&node);
        if (!node.parent) continue;
        FiberChainDecision decision;decision.parent={*node.parent,0};decision.child={node.id,0};
        const auto p=closed.find(*node.parent),c=closed.find(node.id);
        if (p==closed.end() || c==closed.end()) {
            decision.reason="accepted_complete_source_unavailable";
            const auto missing=c==closed.end()?node.id:*node.parent;
            for (const auto& source:original.assignments) if (source.id.parent==missing)
                decision.reason+=std::string("; ")+fiber_rejection_reason_name(source.reason);
        } else {
            decision.parent=sources[p->second].source->id;decision.child=sources[c->second].source->id;
            children[p->second].emplace_back(c->second,plan.chain_decisions.size());decision.reason="not_attempted";
        }
        plan.chain_decisions.push_back(std::move(decision));
    }
    std::vector<size_t> roots;
    for (const auto& [id,index]:closed) roots.push_back(index);
    std::sort(roots.begin(),roots.end(),[&](size_t a,size_t b){
        const auto& x=*sources[a].source;const auto& y=*sources[b].source;
        return std::make_pair(nodes.at(x.id.parent)->depth,x.id)<std::make_pair(nodes.at(y.id.parent)->depth,y.id);
    });
    std::vector<bool> used(sources.size(),false);
    std::map<double,ExPolygons> safe_domains;
    const auto safe_domain=[&](double width)->const ExPolygons& {
        auto found=safe_domains.find(width);
        if (found==safe_domains.end()) found=safe_domains.emplace(width,fiber_material_offset(
            fiber_material_offset(allowed_domain,-config.contour_boundary_clearance_mm),-0.5*width)).first;
        return found->second;
    };
    for (const size_t root:roots) {
        if (used[root]) continue;
        used[root]=true;
        while (true) {
            const auto& current=*drafts[root];const size_t parent_index=current.sources.back();
            const auto& parent=sources[parent_index];const bool inherited=current.sources.size()>1;
            struct Choice {size_t child,decision;ChainGate gate;};std::vector<Choice> choices;
            for (const auto& edge:children[parent_index]) {
                auto& decision=plan.chain_decisions[edge.second];
                if (used[edge.first]) {decision.reason="source_already_visited";continue;}
                const auto& child=sources[edge.first];const auto& p=*parent.source;const auto& c=*child.source;
                if (!(p.id.parent.domain==c.id.parent.domain) || p.id.parent.purpose!=c.id.parent.purpose ||
                    p.prepared->logical_filament_id!=c.prepared->logical_filament_id || p.centerline->width!=c.centerline->width ||
                    p.centerline->height!=c.centerline->height || p.centerline->mm3_per_mm!=c.centerline->mm3_per_mm ||
                    p.prepared->acceleration_mm_s2!=c.prepared->acceleration_mm_s2) {
                    decision.reason="incompatible_actual_process";continue;
                }
                try {
                    const auto gate=inherited?inherited_ring_gate(parent,child,current.seams.back()):nearest_ring_gate(parent,child);
                    decision.inherited_seam=inherited;decision.parent_contact=gate.parent;decision.child_contact=gate.child;
                    decision.parent_station_mm=parent.canonical(gate.parent_station);decision.child_station_mm=child.canonical(gate.child_station);
                    decision.gap_mm=gate.distance;choices.push_back({edge.first,edge.second,gate});
                } catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::exception& error) {decision.reason=error.what();}
            }
            std::sort(choices.begin(),choices.end(),[&](const auto& a,const auto& b){
                return std::make_tuple(a.gate.distance,sources[a.child].source->id,a.gate.parent_station,a.gate.child_station)<
                    std::make_tuple(b.gate.distance,sources[b.child].source->id,b.gate.parent_station,b.gate.child_station);});
            bool extended=false;
            for (size_t choice_index=0;choice_index<choices.size();++choice_index) {
                const auto& choice=choices[choice_index];auto& decision=plan.chain_decisions[choice.decision];
                RingChainDraft trial=current;
                if (!inherited) trial.seams.front()=choice.gate.parent_station;
                trial.sources.push_back(choice.child);trial.seams.push_back(choice.gate.child_station);trial.links.push_back(choice.decision+1);
                try {
                    const auto geometry=build_open_chain(trial,sources,config);
                    decision.bridge_begin=geometry.bridges.back().first;decision.bridge_end=geometry.bridges.back().second;
                    ExtrusionPath path(geometry.path,*sources[root].source->centerline);
                    const auto reason=validate_path_geometry(path);
                    if (path.is_closed() || reason!=FiberRejectionReason::None)
                        throw std::invalid_argument(std::string("open_chain_")+fiber_rejection_reason_name(reason));
                    if (intersects_other_chains(geometry.path,drafts,sources,config,root,choice.child))
                        throw std::invalid_argument("open_chain_intersects_another_source");
                    const auto domain=offset_ex(safe_domain(path.width),float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
                    if (!diff_pl(Polylines{check_view(geometry.path)},domain).empty())
                        throw std::invalid_argument("open_chain_outside_configured_material_clearance");
                    if (config.enforce_policy_boundary && config.policy_boundary && !config.policy_boundary->forbidden_core.empty() &&
                        !intersection_ex(fiber_contour_coverage(check_view(geometry.path),0.5*path.width),
                            offset_ex(config.policy_boundary->forbidden_core,-4.0f),ApplySafetyOffset::No).empty())
                        throw std::invalid_argument("open_chain_crosses_policy_boundary");
                    if (!open_chain_stability(geometry.bends,config.concentric_corner_stabilization_length_mm))
                        throw std::invalid_argument("open_chain_bend_stabilization_too_short");
                    drafts[root]=std::move(trial);drafts[choice.child].reset();used[choice.child]=true;
                    decision.planned=true;decision.reason="planned";extended=true;
                    for (size_t j=choice_index+1;j<choices.size();++j)
                        plan.chain_decisions[choices[j].decision].reason="one_successor_separate_root";
                    break;
                } catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::exception& error) {decision.reason=error.what();}
            }
            if (!extended) break;
        }
    }
    if (std::none_of(drafts.begin(),drafts.end(),[](const auto& draft){return draft && draft->sources.size()>1;})) return;
    FiberValidationResult publish;publish.assignments=original.assignments;publish.output_role=original.output_role;
    std::vector<std::vector<Polyline3>> actual_variants;std::vector<bool> connected;
    std::string failed;
    try {
        // Publication follows the same depth/ID root order used for selecting
        // chains; every untouched accepted source remains an independent item.
        std::vector<size_t> order;
        for (size_t i=0;i<drafts.size();++i) if (drafts[i]) order.push_back(i);
        std::sort(order.begin(),order.end(),[&](size_t a,size_t b){
            const auto da=nodes.at(sources[a].source->id.parent)->depth,db=nodes.at(sources[b].source->id.parent)->depth;
            return std::make_pair(da,sources[a].source->id)<std::make_pair(db,sources[b].source->id);});
        for (const size_t index:order) {
            const auto& draft=*drafts[index];const auto& root=*sources[index].source;
            auto geometry=build_open_chain(draft,sources,config);FiberChainExecution chain;
            if (draft.sources.size()==1) {
                chain.centerline=*root.centerline;chain.prepared=root.prepared;
            } else {
                normalize_open_chain(geometry,safe_domain(root.centerline->width));
                if (!open_chain_stability(geometry.bends,config.concentric_corner_stabilization_length_mm))
                    throw std::invalid_argument("normalized_open_chain_bend_stabilization_too_short");
                chain.centerline=ExtrusionPath(geometry.path,*root.centerline);
                const auto& terminal=sources[draft.sources.back()];
                FiberFinishRoute finish;finish.strategy=FiberFinishStrategy::LoopOverlap;finish.source=terminal.source->id;
                finish.requested_length_mm=config.finish_overlap_length_mm;
                if (!std::isfinite(finish.requested_length_mm) || finish.requested_length_mm<0 || finish.requested_length_mm>terminal.length)
                    throw std::invalid_argument("terminal_source_cannot_supply_requested_finish");
                if (finish.requested_length_mm>0) {
                    OpenChainGeometry route;
                    const double head=config.connection_head_trim_widths*terminal.source->centerline->width;
                    const double first=std::min(head,finish.requested_length_mm);
                    if (first>0) append_frozen_span(route,terminal,draft.sources.back(),draft.seams.back(),first,false);
                    if (finish.requested_length_mm>first) append_frozen_span(route,terminal,draft.sources.back(),
                        draft.seams.back()+first,finish.requested_length_mm-first,false);
                    finish.geometry=std::move(route.path);chain.finish_pieces=std::move(route.pieces);
                }
                const auto prepared=FiberPathFinalizer::finalize(chain.centerline,allowed_domain,config,root.id,finish,
                    geometry.arcs,&safe_domain(chain.centerline.width),geometry.pieces,chain.finish_pieces);
                if (!prepared.prepared) throw std::invalid_argument(std::string("open_chain_finalization_")+
                    fiber_rejection_reason_name(finalization_reason(prepared.failure))+": "+prepared.detail);
                chain.prepared=prepared.prepared;
            }
            chain.sources=std::move(geometry.sources);chain.pieces=std::move(geometry.pieces);
            // Every placement is audited below in its own canonical frame.
            actual_variants.push_back(actual_chain_deposition(*chain.prepared));connected.push_back(draft.sources.size()>1);
            if (connected.back())for(const auto& route:actual_variants.back()) {
                const ExtrusionPath actual(route,chain.centerline);
                const auto reason=validate_path_geometry(actual);
                if (actual.is_closed() || reason!=FiberRejectionReason::None)
                    throw std::invalid_argument(std::string("open_chain_actual_")+fiber_rejection_reason_name(reason));
            }
            publish.chain_executions.push_back(std::move(chain));
        }
        // Recheck every changed execution against all actual prepared paths,
        // including originals restored after rejected extension attempts.
        for(size_t placement=0;!actual_variants.empty() && placement<actual_variants.front().size();++placement) {
        Linesf lines;std::vector<std::tuple<size_t,Point,Point>> exact;
        for (size_t i=0;i<actual_variants.size();++i) {
            if(actual_variants[i].size()!=actual_variants.front().size())throw FiberExecutionError("Fiber chain placement sets differ");
            const auto path=check_view(actual_variants[i][placement]);
            for (size_t j=1;j<path.points.size();++j) {
                lines.emplace_back(path.points[j-1].cast<double>(),path.points[j].cast<double>());
                exact.emplace_back(i,path.points[j-1],path.points[j]);
            }
        }
        const auto tree=AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
        for (size_t i=0;i<lines.size();++i) if (connected[std::get<0>(exact[i])]) {
            AABBTreeIndirect::Tree<2,double>::BoundingBox box(lines[i].a,lines[i].a);box.extend(lines[i].b);bool hit=false;
            AABBTreeIndirect::traverse(tree,[&](const auto& node){return box.intersects(node.bbox);},[&](const auto& node){
                const auto& other=exact[node.idx];
                if (std::get<0>(other)!=std::get<0>(exact[i])) hit=Geometry::segments_intersect(
                    std::get<1>(exact[i]),std::get<2>(exact[i]),std::get<1>(other),std::get<2>(other));
                return !hit;});
            if(hit) {
                // Preserve an existing connection-candidate rejection. Only a
                // crossing introduced by final machine execution is fatal.
                bool source_crossing=false;
                const auto source_a=check_view(source_chain_deposition(*publish.chain_executions[std::get<0>(exact[i])].prepared));
                for(size_t other=0;other<publish.chain_executions.size() && !source_crossing;++other) {
                    if(other==std::get<0>(exact[i]))continue;
                    const auto source_b=check_view(source_chain_deposition(*publish.chain_executions[other].prepared));
                    for(size_t a=1;a<source_a.points.size() && !source_crossing;++a)
                        for(size_t b=1;b<source_b.points.size();++b)
                            if(Geometry::segments_intersect(source_a.points[a-1],source_a.points[a],source_b.points[b-1],source_b.points[b])) {
                                source_crossing=true;break;
                            }
                }
                if(source_crossing)throw std::invalid_argument("open_chain_actual_domain_crossing");
                throw FiberExecutionError("open_chain_actual_domain_crossing");
            }
        }
        }
        if (!publish.audit_open_chains()) throw std::invalid_argument("open_chain_source_or_command_audit_failed");
        for (const auto& chain:publish.chain_executions) {
            append(publish.physical_footprint,chain.prepared->physical_coverage);append(publish.resin_exclusion,chain.prepared->resin_exclusion);
            append(publish.outside_domain,chain.prepared->outside_domain);append(publish.contour_to_infill_keepout,chain_contour_keepout(chain,config));
        }
        publish.physical_footprint=union_ex(publish.physical_footprint);publish.resin_exclusion=union_ex(publish.resin_exclusion);
        publish.outside_domain=union_ex(publish.outside_domain);publish.contour_to_infill_keepout=union_ex(publish.contour_to_infill_keepout);
    } catch (const FiberExecutionError&) { throw; }
    catch (const CanceledException&) { throw; }
    catch (const std::exception& error) {failed=error.what();}
    if (!failed.empty()) {
        for (auto& decision:plan.chain_decisions) if (decision.planned)
            decision.reason="planned_but_domain_reverted: "+failed;
        return;
    }
    original.chain_executions=std::move(publish.chain_executions);original.physical_footprint=std::move(publish.physical_footprint);
    original.resin_exclusion=std::move(publish.resin_exclusion);original.outside_domain=std::move(publish.outside_domain);
    original.contour_to_infill_keepout=std::move(publish.contour_to_infill_keepout);
    for (auto& decision:plan.chain_decisions) if (decision.planned) {decision.published=true;decision.reason="connected";}
}

void audit_fiber_execution_connections(const std::map<FiberFragmentId,CompiledFiberPath>& paths)
{
    // Only connected routes issue queries below. With none present, rebuilding
    // every actual/reference polyline and its obstacle tree serves no check.
    // Keep the complete original obstacle set whenever any route is connected.
    if(std::none_of(paths.begin(),paths.end(),[](const auto& item) {
        return !item.second.variants.empty() && !item.second.geometry->command_pieces.empty();
    }))return;
    struct Route {const PreparedFiberPath* source;Polyline actual,reference;bool connected;};
    std::map<std::pair<FiberDomainId,size_t>,std::vector<Route>> groups;
    for(const auto& item:paths)for(const auto& variant:item.second.variants) {
        const auto& geometry=*item.second.geometry;
        auto actual=frozen_fiber_deposition(*variant,*item.second.context);
        const bool connected=!geometry.command_pieces.empty();
        if(connected) {
            ExtrusionPath check(erContinuousFiberInfill,geometry.geometric_mm3_per_mm,geometry.width_mm,geometry.height_mm);
            check.polyline=Polyline3(actual);
            if(check.is_closed() || validate_path_geometry(check)!=FiberRejectionReason::None)
                throw FiberExecutionError("Machine execution invalidates an accepted open connection",FiberExecutionFailure::Coverage);
        }
        groups[{geometry.id.parent.domain,variant->placement.instance_id}].push_back(
            {&geometry,std::move(actual),check_view(source_chain_deposition(geometry)),connected});
    }
    for(const auto& group:groups) {
        const auto& routes=group.second;Linesf lines;std::vector<std::tuple<size_t,Point,Point>> exact;
        for(size_t i=0;i<routes.size();++i)for(size_t j=1;j<routes[i].actual.points.size();++j) {
            const auto& a=routes[i].actual.points[j-1];const auto& b=routes[i].actual.points[j];
            lines.emplace_back(a.cast<double>(),b.cast<double>());exact.emplace_back(i,a,b);
        }
        const auto tree=AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
        std::set<std::pair<size_t,size_t>> crossings;
        for(size_t i=0;i<lines.size();++i)if(routes[std::get<0>(exact[i])].connected) {
            AABBTreeIndirect::Tree<2,double>::BoundingBox box(lines[i].a,lines[i].a);box.extend(lines[i].b);
            AABBTreeIndirect::traverse(tree,[&](const auto& node){return box.intersects(node.bbox);},[&](const auto& node) {
                const auto& a=exact[i];const auto& b=exact[node.idx];
                if(std::get<0>(a)!=std::get<0>(b) && Geometry::segments_intersect(
                    std::get<1>(a),std::get<2>(a),std::get<1>(b),std::get<2>(b)))
                    crossings.insert(std::minmax(std::get<0>(a),std::get<0>(b)));
                return true;
            });
        }
        for(const auto& pair:crossings) {
            bool existed=false;
            const auto& a=routes[pair.first].reference;const auto& b=routes[pair.second].reference;
            for(size_t i=1;i<a.points.size() && !existed;++i)for(size_t j=1;j<b.points.size();++j)
                if(Geometry::segments_intersect(a.points[i-1],a.points[i],b.points[j-1],b.points[j])){existed=true;break;}
            if(!existed)throw FiberExecutionError("Machine execution introduced a crossing between accepted fiber paths",FiberExecutionFailure::Coverage);
        }
    }
}

} // namespace Slic3r
