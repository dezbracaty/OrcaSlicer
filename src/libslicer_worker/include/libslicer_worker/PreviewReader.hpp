#pragma once

#include "libslicer_worker/PreviewBinary.hpp"

#include <string_view>

namespace libslicer::worker::preview {

struct RenderSegmentView {
    WireVec3f start_mm;
    WireVec3f end_mm;
    std::uint32_t layer_id { invalid_id };
    std::uint32_t object_id { invalid_id };
    std::uint32_t instance_id { invalid_id };
    std::uint16_t tool_id { invalid_small_id };
    std::uint16_t filament_id { invalid_small_id };
    MoveType move_type { MoveType::Noop };
    PathKind path_kind { PathKind::Noop_move };
    ExtrusionRole extrusion_role { Slic3r::erNone };
    float width_mm { 0.0f };
    float height_mm { 0.0f };
    float feedrate_mm_s { 0.0f };
    float actual_feedrate_mm_s { 0.0f };
    float mm3_per_mm { 0.0f };
    float print_z_mm { 0.0f };
    std::uint32_t flags { 0 };
};

inline constexpr bool move_has_flag(const WireMoveRecord& move, MoveFlags flag)
{
    return has_flag(move.flags, flag);
}

inline constexpr bool move_is_drawable_segment(const WireMoveRecord& move)
{
    return move_has_flag(move, MoveFlags::Drawable) &&
        move_has_flag(move, MoveFlags::ValidStartPosition) &&
        move_has_flag(move, MoveFlags::ValidEndPosition);
}

inline constexpr RenderSegmentView make_render_segment_view(const WireMoveRecord& move)
{
    return RenderSegmentView {
        move.start_position_mm,
        move.end_position_mm,
        move.layer_id,
        move.object_id,
        move.instance_id,
        move.tool_id,
        move.filament_id,
        move.move_type,
        move.path_kind,
        move.extrusion_role,
        move.width_mm,
        move.height_mm,
        move.feedrate_mm_s,
        move.actual_feedrate_mm_s,
        move.mm3_per_mm,
        move.print_z_mm,
        move.flags
    };
}

inline constexpr std::string_view move_type_name(const WireMoveRecord& move)
{
    return Slic3r::move_type_name(move.move_type);
}

inline constexpr std::string_view path_kind_name(const WireMoveRecord& move)
{
    return Slic3r::move_path_type_name(move.path_kind);
}

inline constexpr std::string_view extrusion_role_name(const WireMoveRecord& move)
{
    return Slic3r::extrusion_role_name(move.extrusion_role);
}

} // namespace libslicer::worker::preview
