#pragma once

#include <libslic3r/OrcaGeometryTypes.hpp>
#include <libslic3r/OrcaToolpathTypes.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace libslicer::worker::preview {

inline constexpr std::uint32_t schema_version = 2;
inline constexpr std::string_view schema_name = "orca.toolpath_preview";
inline constexpr std::string_view binary_format_name = "orca-toolpath-preview-binary-v2";
inline constexpr std::string_view coordinate_space_name = "orca_plate_world_mm";

using MoveType = Slic3r::EMoveType;
using PathKind = Slic3r::EMovePathType;
using ExtrusionRole = Slic3r::ExtrusionRole;

enum class MoveFlags : std::uint32_t {
    None = 0,
    ValidStartPosition = 1u << 0,
    ValidEndPosition = 1u << 1,
    Drawable = 1u << 2,
    InternalOnly = 1u << 3,
    Retraction = 1u << 4,
    ToolChange = 1u << 5,
    ColorChange = 1u << 6,
    HasArc = 1u << 7,
    HasObject = 1u << 8,
    HasInstance = 1u << 9,
    HasFilament = 1u << 10,
    HasCpColor = 1u << 11,
    HasTool = 1u << 12,
};

enum class ColorSource : std::uint8_t {
    Unknown = 0,
    Filament = 1,
    ColorChange = 2,
    Custom = 3,
};

inline constexpr MoveFlags operator|(MoveFlags left, MoveFlags right)
{
    return static_cast<MoveFlags>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

inline constexpr MoveFlags operator&(MoveFlags left, MoveFlags right)
{
    return static_cast<MoveFlags>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
}

inline constexpr bool has_flag(std::uint32_t flags, MoveFlags flag)
{
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

inline constexpr bool has_flag(MoveFlags flags, MoveFlags flag)
{
    return has_flag(static_cast<std::uint32_t>(flags), flag);
}

inline constexpr std::uint32_t invalid_id = 0xffffffffu;
inline constexpr std::uint64_t invalid_large_id = 0xffffffffffffffffull;
inline constexpr std::uint16_t invalid_small_id = 0xffffu;

struct WireVec3f {
    float x { 0.0f };
    float y { 0.0f };
    float z { 0.0f };
};

struct WireBoundingBox3f {
    WireVec3f min;
    WireVec3f max;
};

static_assert(sizeof(MoveType) == 1);
static_assert(sizeof(PathKind) == 1);
static_assert(sizeof(ExtrusionRole) == 1);
static_assert(sizeof(ColorSource) == 1);
static_assert(static_cast<unsigned char>(MoveType::Noop) == 0);
static_assert(static_cast<unsigned char>(MoveType::Retract) == 1);
static_assert(static_cast<unsigned char>(MoveType::Unretract) == 2);
static_assert(static_cast<unsigned char>(MoveType::Seam) == 3);
static_assert(static_cast<unsigned char>(MoveType::Tool_change) == 4);
static_assert(static_cast<unsigned char>(MoveType::Color_change) == 5);
static_assert(static_cast<unsigned char>(MoveType::Pause_Print) == 6);
static_assert(static_cast<unsigned char>(MoveType::Custom_GCode) == 7);
static_assert(static_cast<unsigned char>(MoveType::Travel) == 8);
static_assert(static_cast<unsigned char>(MoveType::Wipe) == 9);
static_assert(static_cast<unsigned char>(MoveType::Extrude) == 10);
static_assert(static_cast<unsigned char>(MoveType::Count) == 11);
static_assert(static_cast<unsigned char>(PathKind::Noop_move) == 0);
static_assert(static_cast<unsigned char>(PathKind::Linear_move) == 1);
static_assert(static_cast<unsigned char>(PathKind::Arc_move_cw) == 2);
static_assert(static_cast<unsigned char>(PathKind::Arc_move_ccw) == 3);
static_assert(static_cast<unsigned char>(PathKind::Count) == 4);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erNone) == 0);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erPerimeter) == 1);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erExternalPerimeter) == 2);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erOverhangPerimeter) == 3);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erInternalInfill) == 4);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erSolidInfill) == 5);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erTopSolidInfill) == 6);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erBottomSurface) == 7);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erIroning) == 8);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erBridgeInfill) == 9);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erInternalBridgeInfill) == 10);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erGapFill) == 11);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erSkirt) == 12);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erBrim) == 13);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erSupportMaterial) == 14);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erSupportMaterialInterface) == 15);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erSupportTransition) == 16);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erWipeTower) == 17);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erCustom) == 18);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erMixed) == 19);
static_assert(static_cast<std::uint8_t>(ExtrusionRole::erCount) == 20);
static_assert(static_cast<std::uint32_t>(MoveFlags::HasFilament) == (1u << 10));
static_assert(static_cast<std::uint32_t>(MoveFlags::HasCpColor) == (1u << 11));
static_assert(static_cast<std::uint32_t>(MoveFlags::HasTool) == (1u << 12));
static_assert(static_cast<std::uint8_t>(ColorSource::Unknown) == 0);
static_assert(static_cast<std::uint8_t>(ColorSource::Filament) == 1);
static_assert(static_cast<std::uint8_t>(ColorSource::ColorChange) == 2);
static_assert(static_cast<std::uint8_t>(ColorSource::Custom) == 3);
static_assert(std::is_trivially_copyable_v<WireVec3f>);
static_assert(std::is_trivially_copyable_v<WireBoundingBox3f>);
static_assert(sizeof(WireVec3f) == 12);
static_assert(sizeof(WireBoundingBox3f) == 24);

inline WireVec3f to_wire_vec3f(const Slic3r::Vec3f& value)
{
    return { value.x(), value.y(), value.z() };
}

inline Slic3r::Vec3f to_orca_vec3f(const WireVec3f& value)
{
    return { value.x, value.y, value.z };
}

inline constexpr std::string_view move_type_name(MoveType type)
{
    return Slic3r::move_type_name(type);
}

inline constexpr std::string_view path_kind_name(PathKind kind)
{
    return Slic3r::move_path_type_name(kind);
}

inline constexpr std::string_view extrusion_role_name(ExtrusionRole role)
{
    return Slic3r::extrusion_role_name(role);
}

inline constexpr bool is_extrusion(MoveType type)
{
    return type == MoveType::Extrude;
}

inline constexpr bool is_travel_like(MoveType type)
{
    return type == MoveType::Travel || type == MoveType::Wipe;
}

inline constexpr bool is_support_role(ExtrusionRole role)
{
    return role == Slic3r::erSupportMaterial ||
        role == Slic3r::erSupportMaterialInterface ||
        role == Slic3r::erSupportTransition;
}

} // namespace libslicer::worker::preview
