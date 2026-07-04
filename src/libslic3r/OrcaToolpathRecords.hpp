#ifndef slic3r_OrcaToolpathRecords_hpp_
#define slic3r_OrcaToolpathRecords_hpp_

#include "OrcaGeometryTypes.hpp"
#include "OrcaToolpathTypes.hpp"

#include <array>
#include <cstddef>

namespace Slic3r {

enum class ToolpathTimeMode : unsigned char
{
    Normal,
    Stealth,
    Count
};

inline constexpr std::size_t toolpath_time_mode_count = static_cast<std::size_t>(ToolpathTimeMode::Count);

struct ToolpathMoveVertex
{
    unsigned int gcode_id{ 0 };
    EMoveType type{ EMoveType::Noop };
    ExtrusionRole extrusion_role{ erNone };
    // Deprecated name: this stores the active filament id, not the physical tool/nozzle id.
    unsigned char extruder_id{ 0 };
    unsigned char cp_color_id{ 0 };
    Vec3f position{ Vec3f::Zero() }; // mm
    float delta_extruder{ 0.0f }; // mm
    float feedrate{ 0.0f }; // mm/s
    float actual_feedrate{ 0.0f }; // mm/s
    float width{ 0.0f }; // mm
    float height{ 0.0f }; // mm
    float mm3_per_mm{ 0.0f };
    float travel_dist{ 0.0f }; // mm
    float fan_speed{ 0.0f }; // percentage
    float temperature{ 0.0f }; // Celsius degrees
    float pressure_advance{ 0.0f };
    float acceleration{ 0.0f }; // mm/s^2
    float jerk{ 0.0f }; // mm/s
    std::array<float, toolpath_time_mode_count> time{ 0.0f, 0.0f }; // s
    float layer_duration{ 0.0f }; // s
    unsigned int layer_id{ 0 };
    bool internal_only{ false };
    int object_label_id{ -1 };
    float print_z{ 0.0f };

    float volumetric_rate() const { return feedrate * mm3_per_mm; }
    float actual_volumetric_rate() const { return actual_feedrate * mm3_per_mm; }
};

} // namespace Slic3r

#endif
