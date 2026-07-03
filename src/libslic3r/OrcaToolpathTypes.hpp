#ifndef slic3r_OrcaToolpathTypes_hpp_
#define slic3r_OrcaToolpathTypes_hpp_

#include <cstdint>
#include <string_view>

namespace Slic3r {

// Linear/arc shape of a G-code movement used by Orca toolpath preview.
enum class EMovePathType : unsigned char
{
    Noop_move,
    Linear_move,
    Arc_move_cw,
    Arc_move_ccw,
    Count
};

// Semantic movement type emitted by the G-code processor and preview pipeline.
enum class EMoveType : unsigned char
{
    Noop,
    Retract,
    Unretract,
    Seam,
    Tool_change,
    Color_change,
    Pause_Print,
    Custom_GCode,
    Travel,
    Wipe,
    Extrude,
    Count
};

// Each ExtrusionRole value identifies a distinct set of { extruder, speed }.
enum ExtrusionRole : std::uint8_t {
    erNone,
    erPerimeter,
    erExternalPerimeter,
    erOverhangPerimeter,
    erInternalInfill,
    erSolidInfill,
    erTopSolidInfill,
    erBottomSurface,
    erIroning,
    erBridgeInfill,
    erInternalBridgeInfill,
    erGapFill,
    erSkirt,
    erBrim,
    erSupportMaterial,
    erSupportMaterialInterface,
    erSupportTransition,
    erWipeTower,
    erCustom,
    // Extrusion role for a collection with multiple extrusion roles.
    erMixed,
    erCount
};

inline constexpr std::string_view move_type_name(EMoveType type)
{
    switch (type) {
    case EMoveType::Noop: return "noop";
    case EMoveType::Retract: return "retract";
    case EMoveType::Unretract: return "unretract";
    case EMoveType::Seam: return "seam";
    case EMoveType::Tool_change: return "tool_change";
    case EMoveType::Color_change: return "color_change";
    case EMoveType::Pause_Print: return "pause_print";
    case EMoveType::Custom_GCode: return "custom_gcode";
    case EMoveType::Travel: return "travel";
    case EMoveType::Wipe: return "wipe";
    case EMoveType::Extrude: return "extrude";
    case EMoveType::Count: return "count";
    }
    return "unknown";
}

inline constexpr std::string_view move_path_type_name(EMovePathType type)
{
    switch (type) {
    case EMovePathType::Noop_move: return "noop";
    case EMovePathType::Linear_move: return "linear";
    case EMovePathType::Arc_move_cw: return "arc_cw";
    case EMovePathType::Arc_move_ccw: return "arc_ccw";
    case EMovePathType::Count: return "count";
    }
    return "unknown";
}

inline constexpr std::string_view extrusion_role_name(ExtrusionRole role)
{
    switch (role) {
    case erNone: return "none";
    case erPerimeter: return "perimeter";
    case erExternalPerimeter: return "external_perimeter";
    case erOverhangPerimeter: return "overhang_perimeter";
    case erInternalInfill: return "internal_infill";
    case erSolidInfill: return "solid_infill";
    case erTopSolidInfill: return "top_solid_infill";
    case erBottomSurface: return "bottom_surface";
    case erIroning: return "ironing";
    case erBridgeInfill: return "bridge_infill";
    case erInternalBridgeInfill: return "internal_bridge_infill";
    case erGapFill: return "gap_fill";
    case erSkirt: return "skirt";
    case erBrim: return "brim";
    case erSupportMaterial: return "support_material";
    case erSupportMaterialInterface: return "support_material_interface";
    case erSupportTransition: return "support_transition";
    case erWipeTower: return "wipe_tower";
    case erCustom: return "custom";
    case erMixed: return "mixed";
    case erCount: return "count";
    }
    return "unknown";
}

} // namespace Slic3r

#endif
