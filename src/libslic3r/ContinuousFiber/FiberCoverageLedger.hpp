#ifndef slic3r_FiberCoverageLedger_hpp_
#define slic3r_FiberCoverageLedger_hpp_

#include "../ExPolygon.hpp"
#include "FiberSource.hpp"

#include <cstddef>
#include <vector>

namespace Slic3r {

struct FiberCoverageRecord {
    FiberDomainId source;
    ExPolygons source_domain;
    ExPolygons physical_fiber_coverage;
    ExPolygons accepted_contour_exclusion;
    ExPolygons accepted_infill_exclusion;
    ExPolygons resin_domain;
    ExPolygons intentional_void;
    ExPolygons outside_domain;
};

struct FiberCoverageAudit {
    double original_area_mm2 { 0.0 };
    double fiber_exclusion_area_mm2 { 0.0 };
    double resin_area_mm2 { 0.0 };
    double intentional_void_area_mm2 { 0.0 };
    double unassigned_area_mm2 { 0.0 };
    double unexpected_overlap_area_mm2 { 0.0 };
    double outside_area_mm2 { 0.0 };
};

class FiberCoverageLedger {
public:
    void add(FiberCoverageRecord record);
    FiberCoverageAudit audit() const;
    const std::vector<FiberCoverageRecord>& records() const { return m_records; }

private:
    std::vector<FiberCoverageRecord> m_records;
};

} // namespace Slic3r

#endif
