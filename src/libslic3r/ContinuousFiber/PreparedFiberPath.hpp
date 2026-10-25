#ifndef slic3r_PreparedFiberPath_hpp_
#define slic3r_PreparedFiberPath_hpp_

#include "FiberSource.hpp"
#include "../ExPolygon.hpp"
#include "../Polyline.hpp"
#include "../GCodeWriter.hpp"
#include <stdexcept>
#include <limits>
#include <map>

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

enum class FiberExecutionFailure : uint8_t { InvalidPlan, Transform, Quantization, TailBudget, Feed, Motion, Coverage, Context };
class FiberExecutionError : public std::runtime_error {
public:
    explicit FiberExecutionError(const std::string& message,FiberExecutionFailure kind=FiberExecutionFailure::InvalidPlan)
        :std::runtime_error(message),failure(kind) {}
    FiberExecutionError(const FiberExecutionError& cause,size_t instance,const FiberFragmentId& source,unsigned tool)
        :std::runtime_error(std::string(cause.what())+": instance="+std::to_string(instance)+
            " object="+std::to_string(source.parent.domain.object_id)+" layer="+std::to_string(source.parent.domain.layer_id)+
            " path="+std::to_string(source.parent.path_ordinal)+" physical_tool="+std::to_string(tool)),
         failure(cause.failure),instance_id(instance),path(source),physical_tool_id(tool) {}
    FiberExecutionFailure failure {FiberExecutionFailure::InvalidPlan};
    size_t instance_id {0};
    FiberFragmentId path;
    unsigned physical_tool_id {std::numeric_limits<unsigned>::max()};
};

struct FiberPlacement {
    size_t instance_id {0}, object_id {0};
    Vec2d origin {Vec2d::Zero()};
};

// Geometry inputs are independent of physical tool assignment and machine frame.
class FiberGeometryCoverageCache;
std::shared_ptr<FiberGeometryCoverageCache> make_fiber_geometry_coverage_cache();
struct FiberGeometryCoverageCacheStatistics {size_t hits{0},misses{0},entries{0},retained_bytes{0};};
FiberGeometryCoverageCacheStatistics fiber_geometry_coverage_cache_statistics(const std::shared_ptr<FiberGeometryCoverageCache>&);
struct FiberGeometryContext {
    PrintConfig config;
    bool export_arcs {false};
    std::shared_ptr<FiberGeometryCoverageCache> coverage_cache;
};

// Task-owned snapshot. No mutable Writer is retained by planning workers.
struct FiberMachineMotionLimits;
struct FiberExecutionContext {
    PrintConfig config;
    GCodeCoordinateTransform transform;
    std::vector<FiberPlacement> placements;
    std::vector<std::shared_ptr<const FiberMachineMotionLimits>> machine_limits;
    bool export_arcs {false};
    bool check_machine {true};
};

struct FrozenFiberCommand {
    Vec2d target {Vec2d::Zero()};
    double dE {0}, speed_mm_s {0}, station_mm {0};
    uint32_t source_primitive {0};
    int32_t arc_index {-1};
};
struct FrozenFiberArc { Vec2d ij {Vec2d::Zero()}; bool ccw {false}; };
struct FrozenFiberSpan {
    Vec2d from {Vec2d::Zero()};
    FiberMotionKind kind {FiberMotionKind::PoweredDepositing};
    std::vector<FrozenFiberCommand> commands;
};
struct FiberMachineMotionLimits {
    ExPolygons tip_xy;
    ExPolygons line_xy, arc_xy, arc_line_xy; // Precomputed once, never offset per emitted edge.
    Vec2d command_to_tip_offset {Vec2d::Zero()};
    double maximum_z_mm {0};
    Vec3d maximum_axis_speed_mm_s {Vec3d::Zero()}; // Zero disables that axis limit.
    void prepare();
    void validate_motion(const Vec3d& from, const Vec3d& to, double speed, EMovePathType type, const Vec2d& ij) const;
    void validate_move(const Vec3d& from, const Vec3d& to) const;
    void validate_feedrate(const Vec3d& from, const Vec3d& to, double speed_mm_s,
        EMovePathType type = EMovePathType::Linear_move, const Vec2d& center = Vec2d::Zero()) const;
};

struct FrozenFiberCommands {
    std::vector<FrozenFiberSpan> spans;
    std::vector<FrozenFiberArc> arcs;
};
bool equal_fiber_commands(const FrozenFiberCommands& a,const FrozenFiberCommands& b);

struct FrozenFiberExecution {
    FiberPlacement placement;
    unsigned logical_filament_id {0}, logical_extruder_id {0}, physical_tool_id {0};
    double e_units_per_mm {1}, layer_z {0}, prefeed_dE {0}, prefeed_F {0};
    double source_deviation_mm {0};
    Vec2d nozzle_offset {Vec2d::Zero()};
    // Quantized relative targets are shared only after full command equality.
    Vec2d command_origin {Vec2d::Zero()};
    std::shared_ptr<const FrozenFiberCommands> commands;
    const std::vector<FrozenFiberSpan>& spans() const { return commands->spans; }
    const std::vector<FrozenFiberArc>& arcs() const { return commands->arcs; }
    Vec3d machine_target(const Vec2d& relative) const;
    std::vector<Vec3d> entry_targets;
    std::vector<double> entry_stations_mm;
    Vec3d entry_start {Vec3d::Zero()};
    std::shared_ptr<const FiberMachineMotionLimits> limits;
};

struct FiberPathDraft {
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
    // Set union of correctly oriented inner components; overlaps are intentional.
    // Materialize with a NonZero boolean before area or canonical polygon queries.
    ExPolygons guaranteed_components;
    ExPolygons physical_coverage;
    ExPolygons allowed_domain;
    double outside_tolerance_mm2 {0.0};
    ExPolygons resin_exclusion;
    ExPolygons outside_domain;
    ExPolygons contour_to_infill_keepout;
    ExPolygons contour_resin_exclusion;

    std::shared_ptr<const FiberPolicyBoundary> policy_boundary;
    std::shared_ptr<const FiberGeometryContext> geometry_context;
    std::shared_ptr<const Polyline> reference_deposition;
    // Inclusive reference endpoint + semantic span group (entry=0, span=i+1).
    std::vector<std::pair<size_t,size_t>> reference_groups;
    std::shared_ptr<const Polyline3> expected_legacy_view;

    double total_depositing_length_mm() const;
    double passive_tail_length_mm() const;
    // Builder-only operation, before publishing shared_ptr<const PreparedFiberPath>.
    void finalize_actions();
    // Full preflight; also called by the binder before writer mutation.
    void validate() const;
};

class FiberPathFinalizer;
// Published plans cannot be default constructed, copied into a mutable object,
// or edited through the legacy extrusion view. Only Finalizer can publish.
class PreparedFiberPath final {
    const FiberPathDraft m_data;
    explicit PreparedFiberPath(FiberPathDraft&& data) : m_data(std::move(data)),
        id(m_data.id),
        logical_filament_id(m_data.logical_filament_id),
        finish_strategy(m_data.finish_strategy),
        acceleration_mm_s2(m_data.acceleration_mm_s2),
        geometric_mm3_per_mm(m_data.geometric_mm3_per_mm),
        width_mm(m_data.width_mm),
        height_mm(m_data.height_mm),
        spans(m_data.spans),
        actions(m_data.actions),
        command_pieces(m_data.command_pieces),
        start_procedure(m_data.start_procedure),
        entry(m_data.entry),
        execution_deviation_mm(m_data.execution_deviation_mm),
        passive_length_error_mm(m_data.passive_length_error_mm),
        requested_passive_length_mm(m_data.requested_passive_length_mm),
        possible_coverage(m_data.possible_coverage),
        guaranteed_components(m_data.guaranteed_components),
        physical_coverage(m_data.physical_coverage),
        allowed_domain(m_data.allowed_domain),
        outside_tolerance_mm2(m_data.outside_tolerance_mm2),
        resin_exclusion(m_data.resin_exclusion),
        outside_domain(m_data.outside_domain),
        contour_to_infill_keepout(m_data.contour_to_infill_keepout),
        contour_resin_exclusion(m_data.contour_resin_exclusion),
        policy_boundary(m_data.policy_boundary),
        geometry_context(m_data.geometry_context),
        reference_deposition(m_data.reference_deposition),
        reference_groups(m_data.reference_groups),
        expected_legacy_view(m_data.expected_legacy_view) {}
    friend class FiberPathFinalizer;
public:
    PreparedFiberPath(const PreparedFiberPath&)=delete;
    PreparedFiberPath& operator=(const PreparedFiberPath&)=delete;
    const FiberPathDraft& draft() const { return m_data; }
    const decltype(FiberPathDraft::id)& id;
    const decltype(FiberPathDraft::logical_filament_id)& logical_filament_id;
    const decltype(FiberPathDraft::finish_strategy)& finish_strategy;
    const decltype(FiberPathDraft::acceleration_mm_s2)& acceleration_mm_s2;
    const decltype(FiberPathDraft::geometric_mm3_per_mm)& geometric_mm3_per_mm;
    const decltype(FiberPathDraft::width_mm)& width_mm;
    const decltype(FiberPathDraft::height_mm)& height_mm;
    const decltype(FiberPathDraft::spans)& spans;
    const decltype(FiberPathDraft::actions)& actions;
    const decltype(FiberPathDraft::command_pieces)& command_pieces;
    const decltype(FiberPathDraft::start_procedure)& start_procedure;
    const decltype(FiberPathDraft::entry)& entry;
    const decltype(FiberPathDraft::execution_deviation_mm)& execution_deviation_mm;
    const decltype(FiberPathDraft::passive_length_error_mm)& passive_length_error_mm;
    const decltype(FiberPathDraft::requested_passive_length_mm)& requested_passive_length_mm;
    const decltype(FiberPathDraft::possible_coverage)& possible_coverage;
    const decltype(FiberPathDraft::guaranteed_components)& guaranteed_components;
    const decltype(FiberPathDraft::physical_coverage)& physical_coverage;
    const decltype(FiberPathDraft::allowed_domain)& allowed_domain;
    const decltype(FiberPathDraft::outside_tolerance_mm2)& outside_tolerance_mm2;
    const decltype(FiberPathDraft::resin_exclusion)& resin_exclusion;
    const decltype(FiberPathDraft::outside_domain)& outside_domain;
    const decltype(FiberPathDraft::contour_to_infill_keepout)& contour_to_infill_keepout;
    const decltype(FiberPathDraft::contour_resin_exclusion)& contour_resin_exclusion;
    const decltype(FiberPathDraft::policy_boundary)& policy_boundary;
    const decltype(FiberPathDraft::geometry_context)& geometry_context;
    const decltype(FiberPathDraft::reference_deposition)& reference_deposition;
    const decltype(FiberPathDraft::reference_groups)& reference_groups;
    const decltype(FiberPathDraft::expected_legacy_view)& expected_legacy_view;
    double total_depositing_length_mm() const { return m_data.total_depositing_length_mm(); }
    double passive_tail_length_mm() const { return m_data.passive_tail_length_mm(); }
    void validate() const { m_data.validate(); }
};

struct FiberExecutionStatistics {
    size_t compiled_candidates {0}, translated_coverage {0}, swept_coverage {0};
};
struct CompiledFiberPath {
    std::shared_ptr<const PreparedFiberPath> geometry;
    std::shared_ptr<const FiberExecutionContext> context;
    std::vector<std::shared_ptr<const FrozenFiberExecution>> variants;
    const FrozenFiberExecution& execution(size_t instance_id) const;
};
struct FiberExecutionPlanTable {
    uint64_t geometry_generation {0}, execution_generation {0};
    std::vector<int> filament_map;
    std::map<FiberFragmentId,CompiledFiberPath> paths;
    FiberExecutionStatistics statistics;
};

double fiber_reference_chord_tolerance(const FiberPathDraft& draft,const FiberMotionSpan& span);
std::pair<ExPolygons,ExPolygons> fiber_geometry_coverage_bounds(const FiberPathDraft& draft);
bool certify_fiber_coverage(const PreparedFiberPath& prepared,const FrozenFiberExecution& execution,
    const FiberExecutionContext& context);
Polylines fiber_geometry_contour_runs(const FiberPathDraft& prepared);

FrozenFiberExecution compile_fiber_execution(const FiberPathDraft& draft,
    const FiberExecutionContext& context, const FiberPlacement& placement, double layer_z,
    const std::function<void()>& cancel = {});
// Keep the published geometry/material contract and requested context intact.
// A native arc that cannot be represented on the command grid may use a fully
// certified G1 representation; ordinary G1 compilation retains its schedule.
FrozenFiberExecution compile_certified_fiber_execution(const PreparedFiberPath& prepared,
    const FiberExecutionContext& context, const FiberPlacement& placement, double layer_z,
    FiberExecutionStatistics& statistics, const std::function<void()>& cancel = {});
Polyline frozen_fiber_deposition(const FrozenFiberExecution& execution,
    const FiberExecutionContext& context);
Polylines frozen_fiber_contour_runs(const FiberPathDraft& prepared,const FrozenFiberExecution& execution,
    const FiberExecutionContext& context);
std::shared_ptr<const FiberMachineMotionLimits> fiber_machine_limits(const PrintConfig& config,unsigned extruder);

} // namespace Slic3r

#endif
