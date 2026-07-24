#pragma once

#include "Project.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libslicer::v1 {

namespace detail { struct SliceJobState; struct SliceEngineState; }

struct TemporarySliceSelection {
    PresetSelection     selection;
    FilamentMapOverride complete_manual_map;
};

enum class PreviewDelivery { none, memory, artifact };

struct SliceOutputOptions {
    bool include_gcode {true};
    PreviewDelivery preview {PreviewDelivery::none};
    std::optional<std::filesystem::path> preview_artifact_path;
};

struct SliceRequest {
    ProjectSnapshot                       project;
    PlateId                               plate;
    std::optional<TemporarySliceSelection> temporary_selection;
    SliceOutputOptions                    output;
};

struct SliceInspection {
    EffectiveConfiguration effective_configuration;
    EffectiveFilamentMap   effective_filament_map;
};

enum class SliceEventKind {
    preparing,
    validating,
    slicing,
    exporting,
    warning,
    completed,
    failed,
    cancelled
};

struct SliceEvent {
    SliceEventKind kind;
    int percent;
    std::optional<Diagnostic> diagnostic;
};

using SliceCallback = std::function<void(const SliceEvent &)>;

struct FilamentUsage {
    FilamentSlotId slot;
    double length_mm;
    double volume_mm3;
    double mass_g;
};

struct SliceStatistics {
    std::chrono::milliseconds elapsed;
    std::uint64_t layer_count;
    std::vector<FilamentUsage> filament_usage;
};

enum class PreviewMoveType {
    travel,
    extrude,
    retract,
    unretract,
    tool_change,
    color_change,
    custom,
    unknown
};

enum class PreviewPathKind { linear, arc, unknown };

enum class PreviewExtrusionRole {
    none,
    perimeter,
    external_perimeter,
    overhang_perimeter,
    internal_infill,
    solid_infill,
    top_solid_infill,
    bridge_infill,
    support_material,
    support_interface,
    skirt,
    brim,
    wipe_tower,
    custom,
    unknown
};

enum class PreviewColorSource { filament, color_change, custom, unknown };

struct PreviewColor {
    std::uint32_t id;
    std::array<std::uint8_t, 4> rgba;
    PreviewColorSource source;
    std::optional<FilamentSlotId> filament;
    std::string name;
};

struct PreviewLayer {
    std::uint32_t id;
    std::uint64_t move_begin;
    std::uint64_t move_count;
    double print_z_mm;
    double height_mm;
    double duration_s;
};

struct PreviewTool {
    ToolId tool;
    std::optional<FilamentSlotId> primary_filament;
    double nozzle_diameter_mm;
    Vec3d offset_mm;
};

struct PreviewFilament {
    FilamentSlotId slot;
    ToolId mapped_tool;
    std::array<std::uint8_t, 4> rgba;
    double diameter_mm;
    double density_g_cm3;
    double cost_per_kg;
};

struct PreviewObject {
    ObjectId object;
    std::string name;
};

struct PreviewInstance {
    InstanceId instance;
    ObjectId object;
};

struct PreviewMove {
    std::uint64_t id;
    std::optional<std::uint64_t> gcode_id;
    std::uint32_t layer_id;
    std::optional<ObjectId> object;
    std::optional<InstanceId> instance;
    std::optional<ToolId> tool;
    std::optional<FilamentSlotId> filament;
    std::optional<std::uint32_t> color_id;
    PreviewMoveType type;
    PreviewPathKind path_kind;
    PreviewExtrusionRole extrusion_role;
    Vec3d start_mm;
    Vec3d end_mm;
    std::optional<Vec3d> arc_center_mm;
    double extrusion_delta_mm;
    double feedrate_mm_s;
    double actual_feedrate_mm_s;
    double width_mm;
    double height_mm;
    double mm3_per_mm;
    double distance_mm;
    double fan_speed_percent;
    double temperature_c;
    double pressure_advance;
    double acceleration_mm_s2;
    double jerk_mm_s;
    double time_s;
    double layer_duration_s;
    double print_z_mm;
    std::optional<double> joint_angle_end_rad;
};

enum class PreviewEventType {
    tool_change,
    color_change,
    pause,
    custom_gcode,
    warning,
    unknown
};

struct PreviewEvent {
    std::uint64_t id;
    std::optional<std::uint64_t> move_id;
    PreviewEventType type;
    std::optional<ToolId> tool;
    std::optional<FilamentSlotId> filament;
    double print_z_mm;
    double time_s;
    std::string message;
};

struct SlicePreview {
    std::string schema_id;
    std::uint32_t schema_version;
    std::string coordinate_space;
    std::vector<PreviewLayer> layers;
    std::vector<PreviewTool> tools;
    std::vector<PreviewFilament> filaments;
    std::vector<PreviewColor> colors;
    std::vector<PreviewObject> objects;
    std::vector<PreviewInstance> instances;
    std::vector<PreviewMove> moves;
    std::vector<PreviewEvent> events;
};

struct SliceResult {
    std::optional<std::string> gcode_bytes;
    std::shared_ptr<const SlicePreview> preview;
    std::optional<std::filesystem::path> preview_artifact_path;
    EffectiveConfiguration effective_configuration;
    EffectiveFilamentMap effective_filament_map;
    SliceStatistics statistics;
    std::vector<Diagnostic> diagnostics;
};

class SliceJob {
public:
    Result<void> cancel();
    Result<std::shared_ptr<const SliceResult>> wait();
    Result<std::optional<std::shared_ptr<const SliceResult>>>
        wait_for(std::chrono::milliseconds timeout);

private:
    explicit SliceJob(std::shared_ptr<detail::SliceJobState> state);
    std::shared_ptr<detail::SliceJobState> state_;
    friend class SliceEngine;
};

class SliceEngine {
public:
    Result<SliceInspection> inspect(const SliceRequest &request) const;
    Result<SliceJob> submit(SliceRequest request, SliceCallback callback = {});

private:
    explicit SliceEngine(std::shared_ptr<detail::SliceEngineState> state);
    std::shared_ptr<detail::SliceEngineState> state_;
    friend class SdkContext;
};

} // namespace libslicer::v1
