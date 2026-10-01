#ifndef slic3r_FiberPathValidator_hpp_
#define slic3r_FiberPathValidator_hpp_

#include "ContinuousFiberConfig.hpp"
#include "FiberPathFinalizer.hpp"
#include "ContinuousFiberFillStrategy.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../ExPolygon.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace Slic3r {

enum class FiberAssignmentKind : uint8_t {
    AcceptedFiber,
    Rejected,
    IntentionalVoid
};

enum class FiberRejectionReason : uint8_t {
    None,
    TooShort,
    UnsupportedEntity,
    InvalidParameter,
    InvalidGeometry,
    DegenerateSegment,
    SelfIntersection,
    DuplicateSegment,
    OutsideDomain,
    ProcessBudgetTooShort,
    FinalizedPathOutsideDomain,
    IntervalMappingFailure,
    FinishUnavailable,
    SamplingLimit,
    ContourRoundingUnresolved,
    OccupiedContourRegion,
    UnavailableContourRegion,
    OpenOuterContour,
    BendStabilizationTooShort
};

const char* fiber_rejection_reason_name(FiberRejectionReason reason);

// Measure only the paths between explicit radius-treated connections. Ordinary
// curves count toward the length; arcs/links in each connection do not.
// No marked bends means there is no stability constraint to check.
std::optional<double> minimum_closed_loop_stable_length_mm(
    double loop_length_mm, const std::vector<ContourBend>& bends);

struct FiberFragmentAssignment {
    FiberFragmentId id;
    double source_begin_mm { 0.0 };
    double source_end_mm { 0.0 };
    FiberAssignmentKind kind { FiberAssignmentKind::Rejected };
    FiberRejectionReason reason { FiberRejectionReason::None };
    std::string detail;
    std::vector<ContourIssue> contour_issues;
    std::optional<ExtrusionPath> centerline;
    std::shared_ptr<const PreparedFiberPath> prepared;
    std::optional<FiberPathPurpose> display_purpose;
};

struct FiberCandidateExtent {
    FiberCandidateId id;
    double length_mm { 0.0 };
    bool requires_closed_loop { false };
};

struct FiberAssignmentAudit {
    double gap_length_mm { 0.0 };
    double overlap_length_mm { 0.0 };
    size_t missing_candidate_count { 0 };
    size_t invalid_assignment_count { 0 };

    bool valid() const
    {
        return gap_length_mm <= 1e-5 && overlap_length_mm <= 1e-5 &&
               missing_candidate_count == 0 && invalid_assignment_count == 0;
    }
};

struct FiberValidationResult {
    std::vector<FiberCandidateExtent> candidates;
    std::vector<ExtrusionPath> reference_paths; // Populated only for fiber debug.
    std::vector<FiberFragmentAssignment> assignments;
    ExPolygons physical_footprint;
    ExPolygons resin_exclusion;
    ExPolygons outside_domain;
    ExPolygons contour_to_infill_keepout;
    ExtrusionRole output_role { erNone };

    size_t accepted_count() const;
    size_t rejected_count() const;
    FiberAssignmentAudit audit_assignments() const;
    void release_to(ExtrusionEntitiesPtr& destination);
};

struct FiberContourPlanNode {
    FiberCandidateId id;
    std::optional<FiberCandidateId> parent;
    size_t depth {0};
    FiberContourSide side {FiberContourSide::Outer};
    size_t region_id {0}, boundary_id {0}, part_id {0};
    std::optional<Polyline> source; // Debug only; never used for allocation.
};

struct FiberContourBranchStop {
    std::optional<FiberCandidateId> parent;
    size_t depth {0};
    FiberContourSide side {FiberContourSide::Outer};
    std::string reason; // A stop is not a material void or rejected path.
};

struct FiberContourPlanResult {
    FiberValidationResult validation;
    std::vector<FiberContourPlanNode> nodes;
    std::vector<FiberContourBranchStop> stops;
    bool audit_lineage() const;
    void set_display_cutoff(size_t contour_depths);
};

class FiberPathValidator {
public:
    static FiberContourPlanResult plan_contours(
        const ExPolygons& original_area, const ContinuousFiberConfig& config,
        const FiberDomainId& domain_id, bool collect_debug = false,
        size_t fill_start_depth = fiber_contour_depth_limit, double extra_spacing_mm = 0.0,
        double infill_stabilization_length_mm = 0.0);

    static FiberValidationResult validate_infill(
        const FiberInfillCandidates& candidates, const ExPolygons& allowed_domain,
        const ContinuousFiberConfig& config, const FiberDomainId& domain_id);

    static FiberValidationResult validate(
        const ExtrusionEntitiesPtr& candidates,
        const ExPolygons& allowed_domain,
        const ContinuousFiberConfig& config,
        FiberPathPurpose purpose,
        ExtrusionRole output_role,
        const FiberDomainId& domain_id,
        size_t job_ordinal = 0);

private:
    static FiberValidationResult validate_impl(
        const ExtrusionEntitiesPtr& candidates, const ExPolygons& allowed_domain,
        const ContinuousFiberConfig& config, FiberPathPurpose purpose,
        ExtrusionRole output_role, const FiberDomainId& domain_id, size_t job_ordinal,
        const ExPolygons* planned_centerline_domain, const ExPolygons* geometry_domain,
        const ExPolygons* physical_centerline_domain, bool collect_debug,
        bool requires_closed_loop = false, const std::vector<ContourArc>* source_arcs = nullptr,
        double bend_stabilization_length_mm = 0.0);
};

} // namespace Slic3r

#endif
