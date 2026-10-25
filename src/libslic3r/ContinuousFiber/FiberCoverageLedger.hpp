#ifndef slic3r_FiberCoverageLedger_hpp_
#define slic3r_FiberCoverageLedger_hpp_

#include "../ExPolygon.hpp"
#include "FiberSource.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Slic3r {

class PreparedFiberPath;

// The partition is derived, never supplied as independently mutable G/U masks:
// G = union(sealed source components) intersect allocated F; U = F minus G.
class FiberCoverageEnvelope {
public:
    static FiberCoverageEnvelope from_paths(std::vector<std::shared_ptr<const PreparedFiberPath>> paths);

private:
    explicit FiberCoverageEnvelope(std::vector<std::shared_ptr<const PreparedFiberPath>> paths);
    std::vector<std::shared_ptr<const PreparedFiberPath>> m_paths;
    friend class FiberCoverageLedger;
};

struct FiberCoverageRecord {
    FiberDomainId source;
    ExPolygons source_domain;
    ExPolygons accepted_contour_exclusion;
    ExPolygons accepted_infill_exclusion;
    ExPolygons resin_domain;
    ExPolygons intentional_void;
    ExPolygons outside_domain;
    std::optional<FiberCoverageEnvelope> execution_envelope;
};

enum class FiberCoverageAuditMode { MaterialIntegrity, Detailed };

struct FiberCoveragePartitionStatistics {
    double guaranteed_fiber_area_mm2 {0.0};
    double uncertain_fiber_area_mm2 {0.0};
};

struct FiberCoverageAudit {
    double original_area_mm2 { 0.0 };
    double fiber_exclusion_area_mm2 { 0.0 };
    double resin_area_mm2 { 0.0 };
    double intentional_void_area_mm2 { 0.0 };
    double unassigned_area_mm2 { 0.0 };
    double unexpected_overlap_area_mm2 { 0.0 };
    double outside_area_mm2 { 0.0 };
    double invalid_execution_envelope_area_mm2 {0.0};
    // Domain-wide G/U areas are diagnostic measurements, separate from the
    // material-integrity checks. Absence means they were not calculated.
    std::optional<FiberCoveragePartitionStatistics> partition_statistics;
};

class FiberCoverageLedger {
public:
    void add(FiberCoverageRecord record);
    FiberCoverageAudit audit(FiberCoverageAuditMode mode = FiberCoverageAuditMode::MaterialIntegrity) const;
    const std::vector<FiberCoverageRecord>& records() const { return m_records; }

private:
    std::vector<FiberCoverageRecord> m_records;
};

} // namespace Slic3r

#endif
