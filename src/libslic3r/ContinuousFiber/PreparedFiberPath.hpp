#ifndef slic3r_PreparedFiberPath_hpp_
#define slic3r_PreparedFiberPath_hpp_

#include "FiberSource.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// LayerXY: every stored Z is a placeholder zero, never a machine coordinate.
enum class FiberFinishStrategy : uint8_t { None, TangentExtension, LoopOverlap };

struct FiberEdgeProcess {
    double speed_mm_s { 0.0 };
    double feed_mm_per_xy_mm { 0.0 };
};

enum class FiberMotionKind : uint8_t {
    PrefedLanding,
    PoweredStart,
    PoweredDepositing,
    PassiveDepositingAfterCut,
    NonDepositingFinish
};

struct FiberMotionSpan {
    Polyline3 geometry;
    FiberMotionKind kind { FiberMotionKind::PoweredDepositing };
    std::vector<FiberEdgeProcess> edges;

    bool actively_feeds_fiber() const
    {
        return kind == FiberMotionKind::PoweredStart || kind == FiberMotionKind::PoweredDepositing;
    }

    bool deposits_fiber() const
    {
        return kind == FiberMotionKind::PrefedLanding ||
               kind == FiberMotionKind::PoweredStart ||
               kind == FiberMotionKind::PoweredDepositing ||
               kind == FiberMotionKind::PassiveDepositingAfterCut;
    }
};

enum class FiberActionType : uint8_t {
    Begin,
    Approach,
    ZHop,
    Prefeed,
    LandingSpan,
    LowerToLayer,
    AdhesionDwell,
    Start,
    MotionSpan,
    Cut,
    FiberDepleted,
    Finish,
    End
};

struct FiberProcessAction {
    FiberActionType type { FiberActionType::Begin };
    size_t span_index { 0 };
};

struct FiberStartProcedure {
    double prefeed_length_mm { 0.0 };
    double prefeed_speed_mm_s { 0.0 };
    double z_hop_height_mm { 0.0 };
    double landing_speed_mm_s { 0.0 };
    double start_speed_mm_s { 0.0 };
    int adhesion_dwell_ms { 0 };
};

struct PreparedFiberPath {
    FiberFragmentId id;
    unsigned logical_filament_id { 0 };
    FiberFinishStrategy finish_strategy { FiberFinishStrategy::None };
    double acceleration_mm_s2 { 300.0 };
    double geometric_mm3_per_mm { 0.0 };
    float width_mm { 0.0f };
    float height_mm { 0.0f };
    std::vector<FiberMotionSpan> spans;
    std::vector<FiberProcessAction> actions;
    FiberStartProcedure start_procedure;
    ExPolygons physical_coverage;
    ExPolygons resin_exclusion;
    ExPolygons outside_domain;
    ExPolygons contour_to_infill_keepout;

    double total_depositing_length_mm() const;
    double passive_tail_length_mm() const;
    // Builder-only operation, before publishing shared_ptr<const PreparedFiberPath>.
    void finalize_actions();
    // Full preflight; also called by the binder before writer mutation.
    void validate() const;
};

struct BoundFiberExecutionPlan {
    std::shared_ptr<const PreparedFiberPath> prepared;
    unsigned logical_filament_id { 0 };
    unsigned logical_extruder_id { 0 };
    unsigned physical_tool_id { 0 };
    double e_units_per_mm { 1.0 };
    std::vector<std::vector<double>> edge_dE;
    double prefeed_dE { 0.0 };
    double prefeed_F { 0.0 };
    std::string cut_gcode;
};

BoundFiberExecutionPlan bind_fiber_execution(
    std::shared_ptr<const PreparedFiberPath> prepared,
    unsigned filament_id, unsigned extruder_id, unsigned physical_tool_id,
    double e_units_per_mm, std::string cut_gcode);

} // namespace Slic3r

#endif
