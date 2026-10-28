#pragma once
#include "FiberInfillMaterialCertificate.hpp"
#include "FiberPathFinalizer.hpp"
#include "FiberDomainIndex.hpp"
#include "../ClipperUtils.hpp"
#include <functional>
namespace Slic3r { namespace continuous_fiber_detail {
// One validate_infill invocation owns these domains. No shared mutable cache.
struct FiberLazyInfillDomains
{
    const FiberInfillMaterialCertificate& certificate;
    double width;
    std::optional<ExPolygons> planned, comparison;
    std::unique_ptr<FiberDomainIndex> containment;
    std::optional<FiberFinalizationDomains> finalization;
    size_t resolution_count = 0;
    FiberLazyInfillDomains(const FiberInfillMaterialCertificate& c, double w) : certificate(c), width(w) {}
    void ensure()
    {
        if (finalization)
            return;
        planned     = fiber_material_offset(certificate.material(), -0.5 * width);
        comparison  = offset_ex(*planned, float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
        containment = std::make_unique<FiberDomainIndex>(*comparison);
        finalization.emplace(*planned, *comparison);
        ++resolution_count;
    }
};
struct FiberInfillValidationContext
{
    FiberLazyInfillDomains& batch;
    const FiberInfillMaterialCertificate& certificate;
    size_t source_index;
};
class FiberInfillFinalizationContext
{
    FiberLazyInfillDomains& batch;
    const size_t source_index;
    const Polyline3& whole_geometry;
    const std::vector<ContourArc>& whole_arcs;
    std::function<void(const FiberDomainIndex&, const ExPolygons&)> check_whole_source;

public:
    FiberInfillFinalizationContext(const FiberInfillValidationContext& source,
                                   const Polyline3& geometry,
                                   const std::vector<ContourArc>& arcs,
                                   std::function<void(const FiberDomainIndex&, const ExPolygons&)> check)
        : batch(source.batch)
        , source_index(source.source_index)
        , whole_geometry(geometry)
        , whole_arcs(arcs)
        , check_whole_source(std::move(check))
    {
        if (&source.certificate != &batch.certificate)
            throw std::runtime_error("Material-certified infill context certificate mismatch");
    }
    const FiberFinalizationDomains& resolve() const
    {
        // Called only after the unchanged native Finalizer parameter/process guard.
        // Bind the O record to the very same complete source, never another Clear index.
        if (!batch.certificate.matches_source(source_index, whole_geometry, whole_arcs))
            throw std::runtime_error("Material-certified infill occupied source identity mismatch");
        batch.certificate.require_source_occupied_clear(source_index);
        batch.ensure();
        check_whole_source(*batch.containment, *batch.comparison);
        return *batch.finalization;
    }
};
}}
