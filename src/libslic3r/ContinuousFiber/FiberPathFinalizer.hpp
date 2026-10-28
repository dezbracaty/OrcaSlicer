#ifndef slic3r_FiberPathFinalizer_hpp_
#define slic3r_FiberPathFinalizer_hpp_

#include "ContinuousFiberConfig.hpp"
#include "PreparedFiberPath.hpp"
#include "ContinuousFiberFillStrategy.hpp"

#include <memory>
#include <functional>
#include <optional>
#include <string>

namespace Slic3r {

class ExtrusionPath;
// Error-bounded short-edge and collinear normalization. Preserves
// endpoints/seam; this geometry becomes the source for length and coverage.
Polyline3 normalize_fiber_geometry(const Polyline3& input, const ExPolygons* centerline_domain = nullptr);

// Keep every source/bridge seam as an exact knot. Source ranges are unchanged;
// execution stations are rebound to the normalized geometry. The optional map
// records the old station of every returned knot for arc/bend rebinding.
Polyline3 normalize_fiber_chain_geometry(
    const Polyline3& input, std::vector<FiberExecutionPiece>& pieces,
    const ExPolygons* centerline_domain = nullptr,
    std::vector<double>* original_stations_mm = nullptr);

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
// Rectilinear infill uses TangentExtension. Zero requests have no route and
// retain the selected policy; finish never changes the depositing endpoint.
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

namespace continuous_fiber_detail {
struct FiberInfillFinalizationContext;
class FiberDomainIndex;
struct FiberFinalizationDomains {
    const ExPolygons* planned_source;
    mutable const ExPolygons* comparison_source;
    std::function<const ExPolygons&()> comparison_resolver;
    std::optional<FiberCandidateId> band_source;
    double band_width_mm {0},band_sigma {0},native_query_delta_grid {0},proof_radius_grid {0};
    mutable std::optional<ExPolygons> arc_query_storage;
    mutable std::shared_ptr<const FiberDomainIndex> planned_index_storage;
    mutable std::shared_ptr<const FiberDomainIndex> comparison_index_storage;
    mutable std::shared_ptr<const FiberDomainIndex> arc_query_index_storage;
    FiberFinalizationDomains(const ExPolygons& planned,const ExPolygons& comparison)
        : planned_source(&planned),comparison_source(&comparison) {}
    FiberFinalizationDomains(const ExPolygons& planned,const FiberCandidateId& source,double width,
        double original_delta_grid,std::function<const ExPolygons&()> resolver)
        : planned_source(&planned),comparison_source(nullptr),comparison_resolver(std::move(resolver)),
          band_source(source),band_width_mm(width),band_sigma(SCALING_FACTOR),
          native_query_delta_grid(original_delta_grid),proof_radius_grid(original_delta_grid/2) {}
    bool is_lazy_band_context() const { return bool(comparison_resolver); }
    bool band_matches(const ExPolygons* current,const FiberCandidateId& source,double width) const;
    bool proves_planned_band(const Polyline& path) const;
    const ExPolygons& comparison_domain() const;
    const ExPolygons& arc_query_domain() const;
    const FiberDomainIndex& planned_index() const;
    const FiberDomainIndex& comparison_index() const;
    const FiberDomainIndex& arc_query_index() const;
};
}

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
        const ExPolygons* planned_centerline_domain = nullptr,
        const std::vector<FiberExecutionPiece>& pieces = {},
        const std::vector<FiberExecutionPiece>& finish_pieces = {},
        const continuous_fiber_detail::FiberFinalizationDomains* domains = nullptr,
        const continuous_fiber_detail::FiberInfillFinalizationContext* material_context = nullptr);
};

} // namespace Slic3r

#endif
