#ifndef slic3r_FiberPathFinalizer_hpp_
#define slic3r_FiberPathFinalizer_hpp_

#include "ContinuousFiberConfig.hpp"
#include "PreparedFiberPath.hpp"
#include "ContinuousFiberFillStrategy.hpp"

#include <memory>
#include <string>

namespace Slic3r {

class ExtrusionPath;
// Error-bounded short-edge and collinear normalization. Preserves
// endpoints/seam; this geometry becomes the source for length and coverage.
Polyline3 normalize_fiber_geometry(const Polyline3& input, const ExPolygons* centerline_domain = nullptr);

enum class FiberFinalizationFailure : uint8_t {
    None,
    TooShort,
    InvalidParameter,
    InvalidGeometry,
    OutsideDomain,
    PolicyBoundaryRejected,
    FinishUnavailable,
    SamplingLimit
};

struct FiberFinalizationResult {
    std::shared_ptr<const PreparedFiberPath> prepared;
    FiberFinalizationFailure failure { FiberFinalizationFailure::None };
    double length_mm { 0.0 };
    std::string detail;
};

// Source geometry for non-depositing motion. The caller selects the strategy;
// command preparation happens once in the Finalizer, together with other spans.
// A zero request still carries LoopOverlap or TangentExtension explicitly.
struct FiberFinishRoute {
    Polyline3 geometry;
    FiberFinishStrategy strategy {FiberFinishStrategy::None};
    FiberFragmentId source;
    double requested_length_mm {0.0};
};

// Preserve the existing policy for independent sources. An unavailable loop
// leaves geometry empty so Finalizer retains its parameter/length error order.
FiberFinishRoute make_independent_fiber_finish_route(
    const ExtrusionPath& source, const ContinuousFiberConfig& config,
    const FiberFragmentId& id);

class FiberPathFinalizer {
public:
    // Candidate geometry must be normalized before validation and source mapping.
    static FiberFinalizationResult finalize(
        const ExtrusionPath& candidate,
        const ExPolygons& allowed_domain,
        const ContinuousFiberConfig& config,
        const FiberFragmentId& id,
        const FiberFinishRoute& finish,
        const std::vector<ContourArc>& arcs = {},
        const ExPolygons* planned_centerline_domain = nullptr);
};

} // namespace Slic3r

#endif
