#pragma once

#include "libslicer_worker/PreviewTypes.hpp"

#include <libslic3r/OrcaToolpathRecords.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace libslicer::worker::preview {

inline constexpr std::array<char, 8> binary_magic { 'O', 'R', 'C', 'A', 'P', 'V', '1', '\0' };
inline constexpr std::uint16_t binary_little_endian_marker = 0x0102u;

enum class WireSectionType : std::uint32_t {
    MetadataJson = 1,
    StringTable = 2,
    Layers = 3,
    Tools = 4,
    Filaments = 5,
    Colors = 6,
    Objects = 7,
    Instances = 8,
    Moves = 9,
    Events = 10,
};

// Wire* records are the byte-for-byte preview artifact schema. Do not add
// owning fields or platform-sized types here; extend with explicit reserved
// bytes/fields and update the fixed layout assertions below.
struct WireFileHeader {
    std::array<char, 8> magic { binary_magic };
    std::uint16_t header_size { sizeof(WireFileHeader) };
    std::uint16_t version { schema_version };
    std::uint16_t endian { binary_little_endian_marker };
    std::uint16_t section_count { 0 };
    std::uint64_t section_table_offset { 0 };
    std::uint64_t file_size { 0 };
    std::uint64_t metadata_json_offset { 0 };
    std::uint64_t metadata_json_size { 0 };
    std::array<std::uint64_t, 4> reserved {};
};

struct WireSectionHeader {
    WireSectionType section { WireSectionType::MetadataJson };
    std::uint32_t record_size { 0 };
    std::uint64_t offset { 0 };
    std::uint64_t size { 0 };
    std::uint64_t count { 0 };
};

struct WireLayerRecord {
    std::uint32_t id { invalid_id };
    std::uint32_t move_begin { 0 };
    std::uint32_t move_count { 0 };
    float print_z_mm { 0.0f };
    float height_mm { 0.0f };
    float duration_s { 0.0f };
    std::uint32_t flags { 0 };
    std::uint32_t reserved { 0 };
};

struct WireToolRecord {
    std::uint16_t id { invalid_small_id };
    std::uint16_t filament_id { invalid_small_id };
    float nozzle_diameter_mm { 0.0f };
    std::array<float, 3> offset_mm {};
    std::uint32_t flags { 0 };
    std::uint32_t reserved { 0 };
};

struct WireFilamentRecord {
    std::uint16_t id { invalid_small_id };
    std::uint16_t tool_id { invalid_small_id };
    std::array<float, 4> color_rgba {};
    float diameter_mm { 0.0f };
    float density { 0.0f };
    float cost { 0.0f };
    std::uint32_t flags { 0 };
};

struct WireColorRecord {
    std::uint16_t id { invalid_small_id };
    std::uint16_t filament_id { invalid_small_id };
    ColorSource source { ColorSource::Unknown };
    std::array<std::uint8_t, 3> reserved_u8 {};
    std::array<float, 4> color_rgba {};
    std::uint64_t name_offset { 0 };
    std::uint64_t name_size { 0 };
    std::uint32_t flags { 0 };
    std::uint32_t reserved { 0 };
};

struct WireObjectRecord {
    std::uint32_t id { invalid_id };
    std::uint32_t parent_id { invalid_id };
    WireBoundingBox3f bounds_mm;
    std::uint64_t name_offset { 0 };
    std::uint64_t name_size { 0 };
    std::uint32_t flags { 0 };
    std::uint32_t reserved { 0 };
};

struct WireInstanceRecord {
    std::uint32_t id { invalid_id };
    std::uint32_t object_id { invalid_id };
    std::array<float, 16> transform_row_major {};
    WireBoundingBox3f bounds_mm;
    std::uint32_t flags { 0 };
    std::uint32_t reserved { 0 };
};

struct WireMoveRecord {
    std::uint64_t id { 0 };
    std::uint32_t gcode_id { 0 };
    std::uint32_t layer_id { invalid_id };
    std::uint32_t object_id { invalid_id };
    std::uint32_t instance_id { invalid_id };
    std::uint16_t tool_id { invalid_small_id };
    std::uint16_t filament_id { invalid_small_id };
    MoveType move_type { MoveType::Noop };
    PathKind path_kind { PathKind::Noop_move };
    ExtrusionRole extrusion_role { Slic3r::erNone };
    std::uint16_t cp_color_id { invalid_small_id };
    std::uint32_t flags { 0 };
    WireVec3f start_position_mm;
    WireVec3f end_position_mm;
    WireVec3f arc_center_mm;
    float arc_radius_mm { 0.0f };
    float arc_angle_rad { 0.0f };
    float delta_extruder_mm { 0.0f };
    float feedrate_mm_s { 0.0f };
    float actual_feedrate_mm_s { 0.0f };
    float width_mm { 0.0f };
    float height_mm { 0.0f };
    float mm3_per_mm { 0.0f };
    float travel_dist_mm { 0.0f };
    float fan_speed_percent { 0.0f };
    float temperature_celsius { 0.0f };
    float pressure_advance { 0.0f };
    float acceleration_mm_s2 { 0.0f };
    float jerk_mm_s { 0.0f };
    std::array<float, Slic3r::toolpath_time_mode_count> time_s {};
    float layer_duration_s { 0.0f };
    float print_z_mm { 0.0f };
    std::int32_t object_label_id { -1 };
    std::array<std::uint32_t, 8> reserved_u32 {};
    std::array<float, 8> reserved_f32 {};
};

struct WireEventRecord {
    std::uint64_t id { 0 };
    std::uint64_t move_id { invalid_large_id };
    MoveType event_type { MoveType::Noop };
    std::uint16_t tool_id { invalid_small_id };
    std::uint16_t filament_id { invalid_small_id };
    float print_z_mm { 0.0f };
    float time_s { 0.0f };
    std::uint64_t message_offset { 0 };
    std::uint64_t message_size { 0 };
    std::uint32_t flags { 0 };
    std::uint32_t reserved { 0 };
};

#define LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(Type) \
    static_assert(std::is_standard_layout_v<Type>); \
    static_assert(std::is_trivially_copyable_v<Type>)

LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireFileHeader);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireSectionHeader);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireLayerRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireToolRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireFilamentRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireColorRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireObjectRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireInstanceRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireMoveRecord);
LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD(WireEventRecord);

#undef LIBSLICER_PREVIEW_ASSERT_WIRE_RECORD

static_assert(sizeof(WireSectionType) == 4);
static_assert(sizeof(WireFileHeader) == 80);
static_assert(sizeof(WireSectionHeader) == 32);
static_assert(sizeof(WireLayerRecord) == 32);
static_assert(sizeof(WireToolRecord) == 28);
static_assert(sizeof(WireFilamentRecord) == 36);
static_assert(sizeof(WireColorRecord) == 48);
static_assert(sizeof(WireObjectRecord) == 56);
static_assert(sizeof(WireInstanceRecord) == 104);
static_assert(sizeof(WireMoveRecord) == 216);
static_assert(sizeof(WireEventRecord) == 56);

static_assert(offsetof(WireFileHeader, section_table_offset) == 16);
static_assert(offsetof(WireFileHeader, metadata_json_offset) == 32);
static_assert(offsetof(WireSectionHeader, offset) == 8);
static_assert(offsetof(WireColorRecord, color_rgba) == 8);
static_assert(offsetof(WireColorRecord, name_offset) == 24);
static_assert(offsetof(WireObjectRecord, bounds_mm) == 8);
static_assert(offsetof(WireInstanceRecord, transform_row_major) == 8);
static_assert(offsetof(WireMoveRecord, move_type) == 28);
static_assert(offsetof(WireMoveRecord, cp_color_id) == 32);
static_assert(offsetof(WireMoveRecord, flags) == 36);
static_assert(offsetof(WireMoveRecord, start_position_mm) == 40);
static_assert(offsetof(WireMoveRecord, time_s) == 132);
static_assert(offsetof(WireMoveRecord, reserved_u32) == 152);
static_assert(offsetof(WireEventRecord, move_id) == 8);
static_assert(offsetof(WireEventRecord, message_offset) == 32);

} // namespace libslicer::worker::preview
