#ifndef slic3r_FiberPathFinalizer_hpp_
#define slic3r_FiberPathFinalizer_hpp_

#include "ContinuousFiberConfig.hpp"
#include "PreparedFiberPath.hpp"

#include <memory>
#include <string>

namespace Slic3r {

class ExtrusionPath;
// Grid-resolution duplicate/short-edge and collinear normalization. Preserves
// endpoints/seam; this geometry becomes the source for length and coverage.
Polyline3 normalize_fiber_geometry(const Polyline3& input);

enum class FiberFinalizationFailure : uint8_t {
    None,
    TooShort,
    InvalidParameter,
    InvalidGeometry,
    OutsideDomain,
    FinishUnavailable,
    SamplingLimit
};

struct FiberFinalizationResult {
    std::shared_ptr<const PreparedFiberPath> prepared;
    FiberFinalizationFailure failure { FiberFinalizationFailure::None };
    double length_mm { 0.0 };
    std::string detail;
};

class FiberPathFinalizer {
public:
    // Candidate geometry must be normalized before validation and source mapping.
    static FiberFinalizationResult finalize(
        const ExtrusionPath& candidate,
        const ExPolygons& allowed_domain,
        const ContinuousFiberConfig& config,
        const FiberFragmentId& id);
};

} // namespace Slic3r

#endif
