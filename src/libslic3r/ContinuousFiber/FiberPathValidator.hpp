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
    ContourRoundingUnresolved
};

const char* fiber_rejection_reason_name(FiberRejectionReason reason);

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
};

struct FiberCandidateExtent {
    FiberCandidateId id;
    double length_mm { 0.0 };
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

class FiberPathValidator {
public:
    static FiberValidationResult validate(
        const ExtrusionEntitiesPtr& candidates,
        const ExPolygons& allowed_domain,
        const ContinuousFiberConfig& config,
        FiberPathPurpose purpose,
        ExtrusionRole output_role,
        const FiberDomainId& domain_id,
        size_t job_ordinal = 0);
};

} // namespace Slic3r

#endif
