#include "FiberCoverageLedger.hpp"

#include "../ClipperUtils.hpp"
#include "PreparedFiberPath.hpp"

#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include <map>
#include <utility>

namespace Slic3r {

namespace {

double area_mm2(const ExPolygons& polygons)
{
    return unscaled<double>(unscaled<double>(std::abs(area(polygons))));
}

using CoverageArea = boost::multiprecision::int256_t;

CoverageArea twice_signed_area(const Polygon& polygon)
{
    CoverageArea result = 0;
    if (polygon.points.empty())
        return result;
    const Point* previous = &polygon.points.back();
    for (const Point& point : polygon.points) {
        result += CoverageArea(previous->x()) * point.y() - CoverageArea(point.x()) * previous->y();
        previous = &point;
    }
    return result;
}

// Both callers use canonical boolean results, including their oriented holes.
// Keep the subtraction on the integer grid so small U areas are not obtained
// by subtracting two already-rounded floating-point totals.
CoverageArea twice_area(const ExPolygons& polygons)
{
    CoverageArea result = 0;
    for (const ExPolygon& polygon : polygons) {
        result += twice_signed_area(polygon.contour);
        for (const Polygon& hole : polygon.holes)
            result += twice_signed_area(hole);
    }
    return result < 0 ? -result : result;
}

CoverageArea twice_area(const Polygons& polygons)
{
    CoverageArea result = 0;
    for (const Polygon& polygon : polygons)
        result += twice_signed_area(polygon);
    return result < 0 ? -result : result;
}

double exact_area_mm2(const CoverageArea& twice_area)
{
    if (twice_area < 0)
        throw FiberExecutionError("Derived fiber envelope has a negative area");
    const long double value = twice_area.convert_to<long double>() *
        (0.5L * SCALING_FACTOR * SCALING_FACTOR);
    const double result = static_cast<double>(value);
    if (!std::isfinite(value) || !std::isfinite(result))
        throw FiberExecutionError("Derived fiber envelope has a nonfinite area");
    return result;
}

} // namespace

FiberCoverageEnvelope::FiberCoverageEnvelope(std::vector<std::shared_ptr<const PreparedFiberPath>> paths)
    : m_paths(std::move(paths))
{
}

FiberCoverageEnvelope FiberCoverageEnvelope::from_paths(std::vector<std::shared_ptr<const PreparedFiberPath>> paths)
{
    std::map<FiberFragmentId, const PreparedFiberPath*> identities;
    std::vector<std::shared_ptr<const PreparedFiberPath>> sources;
    sources.reserve(paths.size());
    for (auto& path : paths) {
        if (!path)
            throw FiberExecutionError("Fiber coverage envelope has a null sealed source");
        const auto inserted = identities.emplace(path->id, path.get());
        if (!inserted.second) {
            if (inserted.first->second != path.get())
                throw FiberExecutionError("Fiber coverage envelope has conflicting source identities");
            continue;
        }
        sources.push_back(std::move(path));
    }
    return FiberCoverageEnvelope(std::move(sources));
}

void FiberCoverageLedger::add(FiberCoverageRecord record)
{
    m_records.emplace_back(std::move(record));
}

FiberCoverageAudit FiberCoverageLedger::audit(FiberCoverageAuditMode mode) const
{
    FiberCoverageAudit result;
    if (mode == FiberCoverageAuditMode::Detailed)
        result.partition_statistics.emplace();
    for (const FiberCoverageRecord& record : m_records) {
        ExPolygons fiber = union_ex(record.accepted_contour_exclusion, record.accepted_infill_exclusion);
        ExPolygons assigned = union_ex(fiber, record.resin_domain);
        // The first union already canonicalizes all public record inputs.
        // Adding no void cannot change that canonical assigned set.
        if(!record.intentional_void.empty())
            assigned = union_ex(assigned, record.intentional_void);

        const CoverageArea fiber_area = twice_area(fiber);
        if (!record.execution_envelope && !fiber.empty())
            throw FiberExecutionError("Nonempty fiber coverage has no sealed execution envelope");
        if (record.execution_envelope) {
            const auto& sources = record.execution_envelope->m_paths;
            if (sources.empty() && !fiber.empty())
                throw FiberExecutionError("Nonempty fiber coverage has no sealed source paths");
            ExPolygons components, source_exclusion;
            for (const auto& path : sources) {
                if (!(path->id.parent.domain == record.source))
                    throw FiberExecutionError("Fiber coverage source belongs to a different domain");
                if (result.partition_statistics)
                    components.insert(components.end(), path->guaranteed_components.begin(), path->guaranteed_components.end());
                source_exclusion.insert(source_exclusion.end(), path->resin_exclusion.begin(), path->resin_exclusion.end());
            }
            // Independently verify the allocated mask against the sealed
            // source masks. Omissions changing that mask, or a tampered F,
            // must not silently become uncertainty. Identity completeness is
            // supplied by Fill's traversal and the later execution-table audit.
            source_exclusion = union_ex(source_exclusion);
            if (source_exclusion != fiber)
                result.invalid_execution_envelope_area_mm2 += exact_area_mm2(twice_area(xor_ex(source_exclusion, fiber)));

            if (result.partition_statistics) {
                // Only the aggregate diagnostic needs to materialize G. The
                // sealed components remain inputs to execution certification.
                // Keep the same NonZero intersection and integer-grid U=F-G,
                // including their negative-area and finite-value guards.
                const auto guaranteed = intersection(components, fiber);
                const CoverageArea guaranteed_area = twice_area(guaranteed);
                const CoverageArea uncertain_area = fiber_area - guaranteed_area;
                auto& statistics = *result.partition_statistics;
                statistics.guaranteed_fiber_area_mm2 += exact_area_mm2(guaranteed_area);
                statistics.uncertain_fiber_area_mm2 += exact_area_mm2(uncertain_area);
                if (!std::isfinite(statistics.guaranteed_fiber_area_mm2) || !std::isfinite(statistics.uncertain_fiber_area_mm2))
                    throw FiberExecutionError("Derived fiber envelope area accumulation is nonfinite");
            }
        }
        result.original_area_mm2 += area_mm2(record.source_domain);
        result.outside_area_mm2 += area_mm2(record.outside_domain);
        result.fiber_exclusion_area_mm2 += exact_area_mm2(fiber_area);
        result.resin_area_mm2 += area_mm2(record.resin_domain);
        result.intentional_void_area_mm2 += area_mm2(record.intentional_void);
        result.unassigned_area_mm2 += area_mm2(diff_ex(record.source_domain, assigned, ApplySafetyOffset::Yes));
        result.unexpected_overlap_area_mm2 += area_mm2(intersection_ex(fiber, record.resin_domain, ApplySafetyOffset::Yes));
    }
    return result;
}

} // namespace Slic3r
