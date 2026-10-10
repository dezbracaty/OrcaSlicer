#include "FiberPathValidator.hpp"

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
#include <stdexcept>
#include <tuple>

namespace Slic3r {

namespace {

constexpr double interval_epsilon_mm = 1e-5;

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

FiberRejectionReason validate_path_geometry(
    const ExtrusionPath& path, bool check_intersections = true)
{
    const Polyline polyline = path.polyline.to_polyline();
    const Points& points = polyline.points;
    if (points.size() < 2 ||
        !std::isfinite(path.width) || path.width <= 0.0f ||
        !std::isfinite(path.height) || path.height <= 0.0f ||
        !std::isfinite(path.mm3_per_mm) || path.mm3_per_mm <= 0.0)
        return FiberRejectionReason::InvalidGeometry;

    const double length_mm = unscale<double>(path.length());
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
            best_source_distance = cumulative[index] + parameter * std::sqrt(squared_length);
        }
    }

    const double mapping_tolerance_scaled = scale_(0.001);
    if (best_distance_squared > mapping_tolerance_scaled * mapping_tolerance_scaled)
        return false;
    distance_scaled = best_source_distance;
    return true;
}

bool extract_subpath(
    const Polyline3& source,
    double begin_scaled,
    double end_scaled,
    Polyline3& output)
{
    const double source_length = source.length();
    begin_scaled = std::clamp(begin_scaled, 0.0, source_length);
    end_scaled = std::clamp(end_scaled, 0.0, source_length);
    if (end_scaled - begin_scaled <= SCALED_EPSILON)
        return false;

    Polyline3 remaining = source;
    if (begin_scaled > SCALED_EPSILON) {
        Polyline3 prefix;
        Polyline3 suffix;
        if (!source.split_at_length(begin_scaled, &prefix, &suffix))
            return false;
        remaining = std::move(suffix);
    }

    const double requested_length = end_scaled - begin_scaled;
    if (requested_length + SCALED_EPSILON < remaining.length()) {
        Polyline3 selected;
        Polyline3 suffix;
        if (!remaining.split_at_length(requested_length, &selected, &suffix))
            return false;
        output = std::move(selected);
    } else {
        output = std::move(remaining);
    }
    return output.points.size() >= 2;
}

std::vector<SourceInterval> partition_candidate(
    const ExtrusionPath& path,
    const ExPolygons& safe_centerline_domain,
    bool& mapping_valid)
{
    mapping_valid = true;
    const double total_length = path.length();
    if (safe_centerline_domain.empty())
        return {{0.0, total_length, FiberAssignmentKind::Rejected, FiberRejectionReason::OutsideDomain}};

    const Polyline source = path.polyline.to_polyline();
    if (diff_pl(Polylines{source}, safe_centerline_domain).empty())
        return {{0.0, total_length, FiberAssignmentKind::AcceptedFiber, FiberRejectionReason::None}};
    std::vector<double> cumulative(source.points.size(), 0.0);
    for (size_t index = 1; index < source.points.size(); ++index)
        cumulative[index] = cumulative[index - 1] +
            (source.points[index] - source.points[index - 1]).cast<double>().norm();

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
    if (cut==intervals.end() || cut->end_scaled==path.length()) return;
    const double seam=cut->end_scaled, length=path.length();
    Polyline3 prefix,suffix;
    if (!path.polyline.split_at_length(seam,&prefix,&suffix))
        throw std::runtime_error("Cannot rotate clipped fiber reference");
    suffix.points.insert(suffix.points.end(),std::next(prefix.points.begin()),prefix.points.end());
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
    shifted.front().begin_scaled=0; shifted.back().end_scaled=path.length();
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
                    first->geometry.points.front() != last->geometry.points.back())
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
    std::vector<FiberFragmentId>* policy_rejected)
{
    FiberValidationResult result;
    result.output_role = output_role;
    size_t path_ordinal = 0;
    // Analytic geometry belongs to this one candidate, never to a separate
    // array indexed by a traversal order through extrusion collections.
    if (source_arcs && (candidate_roots.size() != 1 || !dynamic_cast<const ExtrusionPath*>(candidate_roots.front()) ||
        !planned_centerline_domain || !physical_centerline_domain))
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

    // Keep half the final tolerance for integer clipping and subpath extraction.
    std::map<double, ExPolygons> partition_domains;
    const auto partition_domain_for_width = [&](double width) -> const ExPolygons& {
        const auto& domain=centerline_domain_for_width(width);
        if (purpose!=FiberPathPurpose::Contour) return domain;
        auto found=partition_domains.find(width);
        if (found==partition_domains.end())
            found=partition_domains.emplace(width,offset_ex(domain,float(scale_(
                0.5*ContourRoundingOptions{}.geometry_tolerance_mm)))).first;
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
        bool planar = true;
        // Source shaping may ignore other boundaries, but simplification must
        // not create a shortcut outside the physical domain and then clip it
        // as an unavailable interval during allocation.
        // Empty arc metadata permits normalization, but its non-null pointer
        // still identifies a whole generated path for containment below.
        try { if (!source_arcs || source_arcs->empty()) path.polyline = normalize_fiber_geometry(input.polyline,
            requires_closed_loop && planned_centerline_domain ? planned_centerline_domain : physical_centerline_domain); }
        catch (const std::invalid_argument&) { planar = false; }
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
            const auto stable=minimum_closed_loop_stable_length_mm(unscale<double>(path.length()),reference_bends);
            if (stable) stability_detail="minimum_stable_path_mm="+std::to_string(*stable)+
                "; required_mm="+std::to_string(bend_stabilization_length_mm);
            if (stable && *stable+interval_epsilon_mm<bend_stabilization_length_mm) {
                whole_path_reason=FiberRejectionReason::BendStabilizationTooShort;
            } else if (stable) {
                // Begin this accepted ring at the end of its last marked bend.
                // The first bend then has the same stable entry as later bends.
                // This changes the seam only; no point, radius or route is replanned.
                const double seam=reference_bends.back().end_mm;
                const double total=unscale<double>(path.length());
                const Point3 contact(reference_bends.back().exit_point,0);
                const auto at=std::find(path.polyline.points.begin(),path.polyline.points.end()-1,contact);
                if (at==path.polyline.points.end()-1)
                    throw std::runtime_error("Marked contour bend endpoint is absent from its path");
                std::rotate(path.polyline.points.begin(),at,path.polyline.points.end()-1);
                path.polyline.points.back()=path.polyline.points.front();
                // All marked arcs precede this seam, so their order is retained.
                for (auto& arc:reference_arcs) {
                    arc.begin_mm+=total-seam;arc.end_distance_mm+=total-seam;
                }
            }
        }
        if (collect_debug && whole_path_reason==FiberRejectionReason::None) result.reference_paths.push_back(path);
        bool mapping_valid=true;
        std::vector<SourceInterval> intervals;
        if (whole_path_reason==FiberRejectionReason::None) {
            if (source_arcs) {
                // The generator already produces contained paths. Clipping an
                // arc here would hide a generation error and break its routing.
                if (!diff_pl(Polylines{path.polyline.to_polyline()}, *physical_centerline_domain).empty())
                    throw std::runtime_error("Generated fiber infill leaves its centerline domain");
                intervals={{0.0,path.length(),FiberAssignmentKind::AcceptedFiber,FiberRejectionReason::None}};
            } else if (requires_closed_loop) {
                // Do not partition an exterior into printable strands. Geometry,
                // occupancy and process validation belong to the entire loop;
                // no coverage is committed until that single outcome succeeds.
                if (!path.is_closed())
                    whole_path_reason=FiberRejectionReason::OpenOuterContour;
                else if (!diff_pl(Polylines{path.polyline.to_polyline()}, partition_domain_for_width(path.width)).empty())
                    whole_path_reason=physical_centerline_domain &&
                        diff_pl(Polylines{path.polyline.to_polyline()}, *physical_centerline_domain).empty() ?
                        FiberRejectionReason::OccupiedContourRegion : FiberRejectionReason::UnavailableContourRegion;
                else
                    intervals={{0.0,path.length(),FiberAssignmentKind::AcceptedFiber,FiberRejectionReason::None}};
            } else {
                intervals=partition_candidate(path,partition_domain_for_width(path.width),mapping_valid);
                if (!mapping_valid) whole_path_reason=FiberRejectionReason::IntervalMappingFailure;
                else if (allocates_contour) rotate_reference(path,reference_arcs,intervals);
            }
        }
        if (whole_path_reason!=FiberRejectionReason::None)
            intervals={{0.0,path.length(),FiberAssignmentKind::Rejected,whole_path_reason}};
        else if (intervals.empty())
            intervals={{0.0,path.length(),FiberAssignmentKind::Rejected,FiberRejectionReason::OutsideDomain}};
        // Ownership is measured on the shaped reference curve, not falsely
        // projected back to the original polygon after rounding changed its length.
        const double source_length_mm=unscale<double>(path.length());
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
                    part.end_scaled= i->end_scaled==fragment.length() ? interval.end_scaled : interval.begin_scaled+i->end_scaled;
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

            Polyline3 fragment_geometry;
            if (!extract_subpath(path.polyline, interval.begin_scaled, interval.end_scaled, fragment_geometry)) {
                assignment.kind = FiberAssignmentKind::Rejected;
                assignment.reason = FiberRejectionReason::InvalidGeometry;
                assignment.centerline.emplace(path);
                result.assignments.emplace_back(std::move(assignment));
                continue;
            }
            assignment.centerline.emplace(std::move(fragment_geometry), path);
            assignment.centerline->set_extrusion_role(output_role);

            if (allocates_contour && assignment.reason==FiberRejectionReason::OutsideDomain) {
                assignment.kind=FiberAssignmentKind::IntentionalVoid;
                assignment.reason=physical_centerline_domain &&
                    diff_pl(Polylines{assignment.centerline->polyline.to_polyline()},*physical_centerline_domain).empty() ?
                    FiberRejectionReason::OccupiedContourRegion : FiberRejectionReason::UnavailableContourRegion;
            }
            if (assignment.kind == FiberAssignmentKind::AcceptedFiber) {
                // Rounded geometry is immutable here. Preserve its arc stations;
                // command preparation handles process splits within its own budget.
                const auto arcs=trim_arcs(reference_arcs,assignment.source_begin_mm,assignment.source_end_mm);
                const auto finalized=FiberPathFinalizer::finalize(
                    *assignment.centerline,allowed_domain,config,assignment.id,arcs,final_domain);
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
    Points points;
    for (const auto& span : accepted->prepared->spans) if (span.deposits_fiber()) {
        const auto part = span.geometry.to_polyline().points;
        if (part.empty()) continue;
        if (!points.empty() && points.back() != part.front()) return std::nullopt;
        points.insert(points.end(), part.begin() + (points.empty() ? 0 : 1), part.end());
    }
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
    const auto comparison_domain = offset_ex(candidates.centerline_domain,
        float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
    for (size_t i = 0; i < candidates.paths.size(); ++i) {
        const auto& candidate = candidates.paths[i];
        ExtrusionPath path(erContinuousFiberInfill, config.infill_flow.mm3_per_mm(),
            config.infill_flow.width(), config.infill_flow.height());
        path.polyline = candidate.geometry;
        std::vector<FiberFragmentId> policy_rejected;
        auto validated = validate_impl({&path}, allowed_domain, config, FiberPathPurpose::Infill,
            erContinuousFiberInfill, domain_id, i, &candidates.centerline_domain, nullptr, &comparison_domain,
            false, false, &candidate.arcs, 0.0, &policy_rejected);
        for (const auto& assignment : validated.assignments)
            if (assignment.kind == FiberAssignmentKind::Rejected &&
                assignment.reason != FiberRejectionReason::TooShort &&
                assignment.reason != FiberRejectionReason::ProcessBudgetTooShort &&
                std::find(policy_rejected.begin(), policy_rejected.end(), assignment.id) == policy_rejected.end())
                throw std::runtime_error(std::string("Generated fiber infill failed preparation: ") +
                    fiber_rejection_reason_name(assignment.reason) + "; " + assignment.detail);
        merge_validation(result, std::move(validated));
    }
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
    validate_fiber_process_config(config, FiberPathPurpose::Contour);
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

} // namespace Slic3r
