#include "FiberPathValidator.hpp"

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"

#include <algorithm>
#include <cmath>
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
    for (size_t first = 0; first < segment_count; ++first) {
        for (size_t second = first + 1; second < segment_count; ++second) {
            if (second == first + 1)
                continue;
            if (closed && first == 0 && second + 1 == segment_count)
                continue;
            if (Geometry::segments_intersect(
                    points[first], points[first + 1],
                    points[second], points[second + 1]))
                return FiberRejectionReason::SelfIntersection;
        }
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
        if (end < begin)
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

FiberRejectionReason finalization_reason(FiberFinalizationFailure failure)
{
    switch (failure) {
    case FiberFinalizationFailure::TooShort:
        return FiberRejectionReason::ProcessBudgetTooShort;
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
    for (const FiberCandidateExtent& candidate : candidates) {
        if (!expected.emplace(candidate.id, candidate.length_mm).second)
            ++audit.invalid_assignment_count;
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
    auto accepted = std::make_unique<ExtrusionEntityCollection>();
    accepted->no_sort = true;
    for (FiberFragmentAssignment& assignment : assignments) {
        if (assignment.kind != FiberAssignmentKind::AcceptedFiber)
            continue;
        if (!assignment.centerline)
            throw std::runtime_error("Accepted continuous fiber assignment has no centerline");
        accepted->entities.push_back(new ExtrusionFiberPath(
            *assignment.centerline, output_role, assignment.prepared));
    }
    if (!accepted->entities.empty())
        destination.emplace_back(accepted.release());
}

FiberValidationResult FiberPathValidator::validate(
    const ExtrusionEntitiesPtr& candidate_roots,
    const ExPolygons& allowed_domain,
    const ContinuousFiberConfig& config,
    FiberPathPurpose purpose,
    ExtrusionRole output_role,
    const FiberDomainId& domain_id,
    size_t job_ordinal)
{
    FiberValidationResult result;
    result.output_role = output_role;
    size_t path_ordinal = 0;
    const bool rounds_contours = purpose == FiberPathPurpose::Contour && config.contour_bend_radius_mm > 0;
    std::map<double, ExPolygons> centerline_domains;
    const auto centerline_domain_for_width = [&](double width) -> const ExPolygons& {
        auto found = centerline_domains.find(width);
        if (found == centerline_domains.end())
            found = centerline_domains.emplace(width, offset_ex(allowed_domain, -float(scale_(
                0.5*width + (purpose == FiberPathPurpose::Contour ? config.contour_boundary_clearance_mm : 0.0))))).first;
        return found->second;
    };

    const auto reject_unsupported = [&](const ExtrusionEntity& entity) {
        const FiberCandidateId candidate_id {domain_id, purpose, job_ordinal, path_ordinal++};
        const double length_mm = unscale<double>(entity.length());
        result.candidates.push_back({candidate_id, std::isfinite(length_mm) ? length_mm : 0.0});
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
        try { path.polyline = normalize_fiber_geometry(input.polyline); }
        catch (const std::invalid_argument&) { planar = false; }
        const FiberCandidateId candidate_id {domain_id, purpose, job_ordinal, path_ordinal++};
        const double source_length_mm = unscale<double>(path.length());
        result.candidates.push_back({candidate_id, std::isfinite(source_length_mm) ? source_length_mm : 0.0});

        FiberRejectionReason whole_path_reason = !planar ? FiberRejectionReason::InvalidGeometry :
            validate_path_geometry(path, !rounds_contours);
        bool mapping_valid = true;
        std::vector<SourceInterval> intervals;
        if (whole_path_reason == FiberRejectionReason::None) {
            if (purpose == FiberPathPurpose::Contour)
                intervals = {{0.0, path.length(), FiberAssignmentKind::AcceptedFiber, FiberRejectionReason::None}};
            else
                intervals = partition_candidate(path, centerline_domain_for_width(path.width), mapping_valid);
        }
        if (!mapping_valid)
            whole_path_reason = FiberRejectionReason::IntervalMappingFailure;
        if (whole_path_reason != FiberRejectionReason::None) {
            intervals = {{0.0, path.length(), FiberAssignmentKind::Rejected, whole_path_reason}};
        } else if (intervals.empty()) {
            intervals = {{0.0, path.length(), FiberAssignmentKind::Rejected,
                          FiberRejectionReason::OutsideDomain}};
        }

        size_t fragment_ordinal = 0;
        for (const SourceInterval& interval : intervals) {
            FiberFragmentAssignment assignment;
            assignment.id = {candidate_id, fragment_ordinal++};
            assignment.source_begin_mm = unscale<double>(interval.begin_scaled);
            assignment.source_end_mm = unscale<double>(interval.end_scaled);
            assignment.kind = interval.kind;
            assignment.reason = interval.reason;

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

            if (assignment.kind == FiberAssignmentKind::AcceptedFiber) {
                // Clipping can change geometry; an unchanged full candidate was already checked.
                const bool whole_candidate = interval.begin_scaled == 0.0 && interval.end_scaled == path.length();
                if (!whole_candidate)
                    assignment.centerline->polyline = normalize_fiber_geometry(assignment.centerline->polyline);
                const FiberRejectionReason fragment_reason = whole_candidate ? FiberRejectionReason::None :
                    validate_path_geometry(*assignment.centerline);
                if (fragment_reason != FiberRejectionReason::None) {
                    assignment.kind = FiberAssignmentKind::Rejected;
                    assignment.reason = fragment_reason;
                } else {
                    std::vector<ContourArc> arcs;
                    Polyline3 rounding_source;
                    if (rounds_contours) {
                        const ExPolygons& centerline_domain = centerline_domain_for_width(path.width);
                        ContourRoundingOptions rounding{config.contour_bend_radius_mm};
                        rounding.fiber_width_mm = path.width;
                        auto rounded = ContinuousFiberFillStrategy::round_contour(
                            assignment.centerline->polyline, centerline_domain, rounding);
                        if (!rounded.path) {
                            assignment.kind = FiberAssignmentKind::Rejected;
                            assignment.reason = FiberRejectionReason::ContourRoundingUnresolved;
                            assignment.contour_issues = std::move(rounded.issues);
                            result.assignments.emplace_back(std::move(assignment));
                            continue;
                        }
                        rounding_source = std::move(assignment.centerline->polyline);
                        assignment.centerline->polyline = std::move(*rounded.path);
                        arcs = std::move(rounded.arcs);
                        // round_contour has checked the complete replacement geometry.
                        // Extrusion metadata and original source ownership remain unchanged.
                    }
                    const FiberFinalizationResult finalized = FiberPathFinalizer::finalize(
                        *assignment.centerline, allowed_domain, config, assignment.id, arcs);
                    if (!finalized.prepared) {
                        assignment.kind = FiberAssignmentKind::Rejected;
                        assignment.reason = finalization_reason(finalized.failure);
                        assignment.detail = finalized.detail;
                    } else {
                        assignment.prepared = finalized.prepared;
                        if (!arcs.empty()) {
                            const auto overlap = ContinuousFiberFillStrategy::contour_coverage_overlap(
                                rounding_source, assignment.centerline->polyline, path.width);
                            assignment.contour_source_overlap_mm2 = overlap.first;
                            assignment.contour_added_overlap_mm2 = overlap.second;
                        }
                        append_coverage(result, *assignment.prepared);
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

} // namespace Slic3r
