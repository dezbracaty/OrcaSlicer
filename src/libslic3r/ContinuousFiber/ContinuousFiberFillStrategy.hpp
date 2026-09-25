#ifndef slic3r_ContinuousFiberFillStrategy_hpp_
#define slic3r_ContinuousFiberFillStrategy_hpp_

#include "ContinuousFiberConfig.hpp"
#include "../ExPolygon.hpp"
#include <optional>
#include <limits>
#include <utility>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <set>

namespace Slic3r {

struct ContourRoundingOptions {
    double minimum_radius_mm {0.0};
    double chord_tolerance_mm {0.00005};
    double geometry_tolerance_mm {0.0001};
    // Process width, not the preview scale. Zero requests centerline geometry only.
    double fiber_width_mm {0.0};
};

// Analytic arc plus its interval on the discretized result, not the source ring.
struct ContourArc {
    Vec2d center_mm, start_mm, end_mm;
    double radius_mm {0.0}, sweep_radians {0.0};
    double begin_mm {0.0}, end_distance_mm {0.0};
    double source_sweep_radians {0.0}; // Preserve the speed limit when an arc is trimmed.
};

enum class ContourRoundingFailure { InvalidInput, SearchNotFound, OutsideDomain, SelfIntersection, TopologyChange, SamplingLimit, SupportConflict, SearchBudgetExceeded, NumericalFailure, OptimizerLimit, CandidateLimit };
struct ContourIssue {
    ContourRoundingFailure reason;
    Polyline source;
};
struct ContourRoundingResult {
    std::optional<Polyline3> path;
    std::vector<ContourArc> arcs;
    std::vector<ContourIssue> issues;
};

// Internal solver data shared with its exhaustive regression tests.
namespace continuous_fiber_detail {
struct CornerGroup { size_t first, count; };
struct TangentSolution {
    std::vector<ContourArc> arcs;
    double incoming_remaining, outgoing_consumed;
    double score=0;
};
struct LocalSolutions {
    std::vector<TangentSolution> values;
    bool optimizer_limit_reached=false;
    bool candidates_pruned=false;
    bool numerical_failure=false;
};
struct CycleChoice {
    std::vector<size_t> indices;
    double score=std::numeric_limits<double>::infinity();
};
CycleChoice compatible_cycle(const std::vector<LocalSolutions>& candidates,
    const std::vector<size_t>& fixed={},const std::vector<std::vector<size_t>>& banned={});
ContourRoundingResult validate_cycle(const std::vector<TangentSolution>& values,
    const Polyline& source,const ExPolygons& domain,const ExPolygons& output_domain,
    const ContourRoundingOptions& options);

// Different partitions share their caller's budgets. Canonical keys prevent
// retries caused only by a rotated seam. Prefer fewer unresolved windows, then
// the single-circle support deficit and fewer merged boundaries. The deficit
// is only a queue-order heuristic, never a proof that CSC/CCC is infeasible.
template<class Attempt>
ContourRoundingResult search_contour_partitions(std::vector<CornerGroup> initial,
    const Polyline& source,size_t& revisions_left,size_t& searches_left,Attempt&& attempt)
{
    using Partition=std::vector<CornerGroup>;
    using Priority=std::pair<size_t,double>; // unresolved windows, support deficit
    std::vector<std::pair<Priority,Partition>> pending;
    std::set<std::vector<std::pair<size_t,size_t>>> seen;
    const auto enqueue=[&](Partition groups,Priority priority) {
        std::vector<std::pair<size_t,size_t>> key;
        for (auto group:groups) key.emplace_back(group.first,group.count);
        if (key.empty()) return;
        std::rotate(key.begin(),std::min_element(key.begin(),key.end()),key.end());
        if (seen.insert(std::move(key)).second) pending.emplace_back(priority,std::move(groups));
    };
    enqueue(std::move(initial),{0,0});
    ContourRoundingResult result;
    while (!pending.empty() && revisions_left>0 && searches_left>0) {
        const auto next=std::min_element(pending.begin(),pending.end(),[](const auto& a,const auto& b){return a.first!=b.first?a.first<b.first:a.second.size()>b.second.size();});
        auto groups=std::move(next->second);pending.erase(next);
        result=attempt(std::move(groups),enqueue,pending.size());
        if (result.path) return result;
    }
    if (!pending.empty()) result.issues.push_back({ContourRoundingFailure::SearchBudgetExceeded,source});
    return result;
}

// Never replace an accepted ring until the complete proposal passes validation.
// A sampling limit during optional improvement leaves the accepted ring intact.
template<class Refine,class Validate>
ContourRoundingResult refine_validated_cycle(ContourRoundingResult baseline,
    std::vector<TangentSolution> proposal,Refine&& refine,Validate&& validate)
{
    if (!baseline.path) return baseline;
    try {
        if (refine(proposal)) {
            auto improved=validate(proposal);
            if (improved.path) return improved;
        }
    } catch (const std::length_error&) {
        // Only sampling-budget exhaustion is an expected unsuccessful improvement.
    }
    return baseline;
}
} // namespace continuous_fiber_detail

const char* contour_rounding_failure_name(ContourRoundingFailure reason);

enum class FiberContourSide : uint8_t { Outer, Hole };

struct FiberContourCandidate {
    Polyline3 geometry;
    FiberContourSide side;
    size_t boundary_id;
    size_t depth;
    size_t region_id {0};
    size_t part_id {0};
    size_t geometry_domain_id {0};
    // Original hole identities absorbed into the reconstructed exterior.
    // Independent hole-loop selection never controls these obstacles.
    std::vector<size_t> rerouted_hole_ids;
};

struct FiberContourCandidates {
    ExPolygons centerline_domain;
    std::vector<ExPolygons> geometry_domains;
    std::vector<FiberContourCandidate> paths;
};

// Circular finite-width tool envelope. These operations share a bounded arc
// error and do not use the general offset's distance-dependent decimation.
ExPolygons fiber_material_offset(const ExPolygons& material, double distance_mm);
ExPolygons fiber_contour_coverage(const Polyline& path, double radius_mm);

class ContinuousFiberFillStrategy {
public:
    // Clearance has already been applied to remaining_material. A hole
    // frontier encloses the original hole and previously accepted rings.
    static FiberContourCandidates generate_contour_level(
        const ExPolygons& remaining_material, double width_mm,
        const ExPolygons* hole_frontier = nullptr);

    // Independent geometric reference offsets, also used by geometry tests.
    // This low-level utility does not enforce ancestry: production slicing uses
    // FiberPathValidator::plan_contours and generate_contour_level instead.
    static FiberContourCandidates generate_contours(
        const ExPolygons& original_area, const ContinuousFiberConfig& config);

    // Shape closed candidates before allocation; exteriors must remain closed.
    static ContourRoundingResult round_contour(
        const Polyline3& source, const ExPolygons& centerline_domain,
        const ContourRoundingOptions& options);

    // Explicit, expensive diagnostic; not part of slicing or path acceptance.
    // Returns {source overlap, added overlap} in square millimetres. No result
    // means it has not been requested, not that the overlap is zero.
    static std::pair<double,double> contour_coverage_overlap(
        const Polyline3& source,const Polyline3& rounded,double width);

    static ExPolygons build_infill_domain(
        const ExPolygons& original_area,
        const ExPolygons& contour_to_infill_keepout);

    static ExPolygons build_resin_area(
        const ExPolygons& original_area,
        const ExPolygons& accepted_fiber_resin_exclusion);
};

} // namespace Slic3r

#endif
