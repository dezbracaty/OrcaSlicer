#ifndef slic3r_PreparedFiberPath_hpp_
#define slic3r_PreparedFiberPath_hpp_

#include "FiberSource.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class GCodeWriter;

// LayerXY: every stored Z is a placeholder zero, never a machine coordinate.
enum class FiberFinishStrategy : uint8_t { None, TangentExtension, LoopOverlap };

enum class FiberExecutionPieceKind : uint8_t { RetainedSource, Transition };

// Ordered positive source ranges. A cyclic seam is represented by two ranges,
// never by reversing a source or by joining separated retained arcs.
struct FiberExecutionPiece {
    FiberExecutionPieceKind kind {FiberExecutionPieceKind::RetainedSource};
    FiberFragmentId source, other_source;
    double source_begin_mm {0.0}, source_end_mm {0.0};
    double execution_begin_mm {0.0}, execution_end_mm {0.0};
    FiberPathPurpose display_purpose {FiberPathPurpose::Contour};
    size_t transition_id {0};
    // Execution station -> frozen source station. Shared by clipped command
    // pieces so normalization does not force an affine source approximation or
    // copy an entire source chart for every sampled edge. Empty means affine.
    std::shared_ptr<const std::vector<std::pair<double,double>>> source_station_knots;
};

struct FiberCommandPiece {
    FiberExecutionPiece origin;
    size_t span_index {0}, begin_edge {0}, end_edge {0};
    double command_begin_mm {0.0}, command_end_mm {0.0};
};

struct FiberEdgeProcess {
    double speed_mm_s { 0.0 };
    double feed_mm_per_xy_mm { 0.0 };
};

enum class FiberEntryMode : uint8_t { PathLanding, VerticalLanding, PoweredFirstEdge };

struct FiberEntryPlan {
    FiberEntryMode mode {FiberEntryMode::PoweredFirstEdge};
    Point3 start {Point3::Zero()};
    Polyline3 geometry; // Analytic XY landing route; Z is applied during binding.
    Point3 contact {Point3::Zero()};
    double end_station_mm {0.0};
    // Landing owns [0,end_station]; spans start at contact. Source stations and
    // machine-rounded XY lengths are different quantities, especially for Z-only entry.
    std::vector<FiberExecutionPiece> pieces;
    bool lowers() const { return mode != FiberEntryMode::PoweredFirstEdge; }
};

enum class FiberMotionKind : uint8_t {
    PoweredStart,
    PoweredEntry,
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
        return kind == FiberMotionKind::PoweredStart || kind == FiberMotionKind::PoweredEntry || kind == FiberMotionKind::PoweredDepositing;
    }

    bool deposits_fiber() const
    {
        return kind == FiberMotionKind::PoweredStart ||
               kind == FiberMotionKind::PoweredEntry ||
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

struct FiberPolicyBoundary {
    ExPolygons forbidden_core;
    bool requires_resin_first { false };
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
    // Empty for unchanged independent paths; otherwise partitions actual edges,
    // including the explicitly supplied non-depositing terminal source route.
    std::vector<FiberCommandPiece> command_pieces;
    FiberStartProcedure start_procedure;
    FiberEntryPlan entry;
    // Conservative execution envelope, determined before material allocation.
    double execution_deviation_mm {0.0};
    double passive_length_error_mm {0.0};
    double requested_passive_length_mm {0.0};
    ExPolygons possible_coverage;
    ExPolygons guaranteed_coverage;
    ExPolygons uncertain_coverage;
    ExPolygons physical_coverage;
    ExPolygons allowed_domain;
    double outside_tolerance_mm2 {0.0};
    ExPolygons resin_exclusion;
    ExPolygons outside_domain;
    ExPolygons contour_to_infill_keepout;

    std::shared_ptr<const FiberPolicyBoundary> policy_boundary;

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
    std::vector<FiberMotionSpan> spans;
    std::vector<std::vector<Vec3d>> writer_targets, machine_targets;
    std::vector<std::vector<Vec2d>> arc_offsets;
    std::vector<std::vector<EMovePathType>> command_types;
    std::vector<std::vector<size_t>> source_primitives;
    std::vector<std::vector<double>> command_stations_mm;
    std::function<Vec2d(const Point&)> point_to_writer;
    double prefeed_dE { 0.0 };
    double prefeed_F { 0.0 };
    std::string cut_gcode;
    struct EntryMotion {
        Vec3d from {Vec3d::Zero()}, to {Vec3d::Zero()};
        double speed_mm_s {0.0};
        bool bound {false};
        std::vector<Vec3d> writer_targets, machine_targets;
    } entry_motion;
};

// Bind once after the instance transform and writer quantization. Every machine
// check and the emitter consume this exact motion, never reconstruct its Z.
void bind_fiber_entry_motion(BoundFiberExecutionPlan& plan, const GCodeWriter& writer, const Vec2d& start_xy,
    const Vec2d& contact_xy, double layer_z);


// Available machine limits, not a model/deposition domain or a full collision
// envelope. Positions are emitted G-code coordinates; XY limits describe the
// tool tip, hence command_to_tip_offset is applied before the check.
struct FiberMachineMotionLimits {
    ExPolygons tip_xy;
    Vec2d command_to_tip_offset {Vec2d::Zero()};
    double maximum_z_mm {0};
    Vec3d maximum_axis_speed_mm_s {Vec3d::Zero()}; // Zero disables that axis limit.
    void validate_move(const Vec3d& from, const Vec3d& to) const;
    void validate_feedrate(const Vec3d& from, const Vec3d& to, double speed_mm_s,
        EMovePathType type = EMovePathType::Linear_move, const Vec2d& center = Vec2d::Zero()) const;
};

BoundFiberExecutionPlan bind_fiber_execution(
    std::shared_ptr<const PreparedFiberPath> prepared,
    unsigned filament_id, unsigned extruder_id, unsigned physical_tool_id,
    double e_units_per_mm, std::string cut_gcode,
    const GCodeWriter* writer = nullptr,
    std::function<Vec2d(const Point&)> point_to_writer = {}, double layer_z = 0.0,
    bool export_arcs = false);

} // namespace Slic3r

#endif
