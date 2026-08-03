#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libslicer {

inline constexpr std::uint32_t invalid_toolpath_id = 0xffffffffu;
inline constexpr std::uint16_t invalid_toolpath_small_id = 0xffffu;
inline constexpr std::uint32_t toolpath_schema_version = 1u;

struct ToolpathPoint
{
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

struct ToolpathColorValue
{
    float red{1.0f};
    float green{1.0f};
    float blue{1.0f};
    float alpha{1.0f};
};

enum class ToolpathMotionKind : std::uint8_t
{
    Travel = 0,
    Extrusion,
    Wipe
};

enum class ToolpathExtrusionRole : std::uint8_t
{
    None = 0,
    InnerWall,
    OuterWall,
    OverhangWall,
    SparseInfill,
    InternalSolidInfill,
    TopSurface,
    BottomSurface,
    Ironing,
    Bridge,
    InternalBridge,
    GapInfill,
    Skirt,
    Brim,
    Support,
    SupportInterface,
    SupportTransition,
    WipeTower,
    Custom,
    Mixed
};

enum class ToolpathEventKind : std::uint8_t
{
    Seam = 0,
    ToolChange,
    ColorChange,
    Pause,
    CustomGCode
};

enum class ToolpathColorSource : std::uint8_t
{
    Unknown = 0,
    Filament,
    ColorChange,
    Custom
};

struct ToolpathTool
{
    std::uint16_t id{invalid_toolpath_small_id};
    std::uint16_t primary_filament_id{invalid_toolpath_small_id};
    ToolpathPoint offset_mm;
    float nozzle_diameter_mm{0.0f};
};

struct ToolpathFilament
{
    std::uint16_t id{invalid_toolpath_small_id};
    std::uint16_t tool_id{invalid_toolpath_small_id};
    ToolpathColorValue color;
    float diameter_mm{0.0f};
    float density_g_cm3{0.0f};
    float cost_per_kg{0.0f};
};

struct ToolpathColor
{
    std::uint16_t id{invalid_toolpath_small_id};
    std::uint16_t filament_id{invalid_toolpath_small_id};
    ToolpathColorSource source{ToolpathColorSource::Unknown};
    ToolpathColorValue color;
    std::string name;
};

// A normalized, directly drawable segment. Processor-only vertices such as
// actual-speed and arc subdivisions are absorbed by libslicer and never exposed
// as a special public state.
struct ToolpathSegment
{
    std::uint64_t id{0};
    std::uint64_t run_id{0};
    std::uint32_t source_command_id{0};
    std::uint32_t layer_index{0};
    std::uint32_t object_id{invalid_toolpath_id};
    std::uint32_t instance_id{invalid_toolpath_id};
    std::uint16_t tool_id{invalid_toolpath_small_id};
    std::uint16_t filament_id{invalid_toolpath_small_id};
    std::uint16_t color_id{invalid_toolpath_small_id};
    ToolpathMotionKind motion{ToolpathMotionKind::Travel};
    // Valid only for Extrusion. Travel and Wipe are guaranteed to use None.
    ToolpathExtrusionRole extrusion_role{ToolpathExtrusionRole::None};
    ToolpathPoint start_mm;
    ToolpathPoint end_mm;
    float extrusion_delta_mm{0.0f};
    float nominal_speed_mm_s{0.0f};
    float actual_speed_mm_s{0.0f};
    float width_mm{0.0f};
    float height_mm{0.0f};
    float mm3_per_mm{0.0f};
    float print_z_mm{0.0f};
};

struct ToolpathLayer
{
    std::uint32_t index{0};
    std::size_t segment_begin{0};
    std::size_t segment_count{0};
    std::size_t event_begin{0};
    std::size_t event_count{0};
    float print_z_mm{0.0f};
    float height_mm{0.0f};
    float duration_seconds{0.0f};
};

struct ToolpathEvent
{
    ToolpathEventKind kind{ToolpathEventKind::CustomGCode};
    std::size_t after_segment{0};
    std::uint32_t source_command_id{0};
    std::uint32_t layer_index{0};
    std::uint16_t tool_id{invalid_toolpath_small_id};
    std::uint16_t filament_id{invalid_toolpath_small_id};
    ToolpathPoint position_mm;
    float print_z_mm{0.0f};
    float time_seconds{0.0f};
    std::string message;
};

struct ToolpathBounds
{
    ToolpathPoint minimum;
    ToolpathPoint maximum;
    bool valid{false};
};

struct ToolpathFeatureStatistics
{
    ToolpathExtrusionRole role{ToolpathExtrusionRole::None};
    std::size_t render_segment_count{0};
    std::size_t path_count{0};
    double length_mm{0.0};
    double extrusion_volume_mm3{0.0};
};

struct ToolpathStatistics
{
    std::size_t total_layers{0};
    std::size_t logical_motion_count{0};
    std::size_t render_segment_count{0};
    double total_time_seconds{0.0};
    double total_extrusion_mm{0.0};
    double total_extrusion_volume_mm3{0.0};
    double total_print_distance_mm{0.0};
    double total_travel_distance_mm{0.0};
    float min_speed_mm_s{0.0f};
    float max_speed_mm_s{0.0f};
    float min_layer_height_mm{0.0f};
    float max_layer_height_mm{0.0f};
    float min_width_mm{0.0f};
    float max_width_mm{0.0f};
    float min_volumetric_flow_mm3_s{0.0f};
    float max_volumetric_flow_mm3_s{0.0f};
    std::vector<ToolpathFeatureStatistics> features;
};

struct ToolpathPreview
{
    std::uint32_t schema_version{toolpath_schema_version};
    std::string source_path;
    std::vector<ToolpathLayer> layers;
    std::vector<ToolpathSegment> segments;
    std::vector<ToolpathEvent> events;
    std::vector<ToolpathTool> tools;
    std::vector<ToolpathFilament> filaments;
    std::vector<ToolpathColor> colors;
    ToolpathStatistics statistics;
    ToolpathBounds bounds;
};

using ToolpathPreviewPtr = std::shared_ptr<const ToolpathPreview>;

} // namespace libslicer
