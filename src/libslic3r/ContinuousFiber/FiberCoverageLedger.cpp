#include "FiberCoverageLedger.hpp"

#include "../ClipperUtils.hpp"

namespace Slic3r {

namespace {

double area_mm2(const ExPolygons& polygons)
{
    return unscaled<double>(unscaled<double>(std::abs(area(polygons))));
}

} // namespace

void FiberCoverageLedger::add(FiberCoverageRecord record)
{
    m_records.emplace_back(std::move(record));
}

FiberCoverageAudit FiberCoverageLedger::audit() const
{
    FiberCoverageAudit result;
    for (const FiberCoverageRecord& record : m_records) {
        ExPolygons fiber = union_ex(record.accepted_contour_exclusion, record.accepted_infill_exclusion);
        ExPolygons assigned = union_ex(fiber, record.resin_domain);
        assigned = union_ex(assigned, record.intentional_void);

        if(record.has_execution_envelope) {
            const auto guaranteed=intersection_ex(record.guaranteed_fiber,fiber);
            const auto uncertain=intersection_ex(record.uncertain_fiber,fiber);
            result.guaranteed_fiber_area_mm2+=area_mm2(guaranteed);
            result.uncertain_fiber_area_mm2+=area_mm2(uncertain);
            const auto classified=union_ex(guaranteed,uncertain);
            result.invalid_execution_envelope_area_mm2+=area_mm2(diff_ex(fiber,classified));
            result.invalid_execution_envelope_area_mm2+=area_mm2(intersection_ex(guaranteed,uncertain));
        }
        result.original_area_mm2 += area_mm2(record.source_domain);
        result.outside_area_mm2 += area_mm2(record.outside_domain);
        result.fiber_exclusion_area_mm2 += area_mm2(fiber);
        result.resin_area_mm2 += area_mm2(record.resin_domain);
        result.intentional_void_area_mm2 += area_mm2(record.intentional_void);
        result.unassigned_area_mm2 += area_mm2(diff_ex(record.source_domain, assigned, ApplySafetyOffset::Yes));
        result.unexpected_overlap_area_mm2 += area_mm2(intersection_ex(fiber, record.resin_domain, ApplySafetyOffset::Yes));
    }
    return result;
}

} // namespace Slic3r
