#pragma once

#include "../Point.hpp"
#include <optional>
#include <vector>

namespace Slic3r {
struct FiberCalibrationPlan {
    struct Turn {
        double radius_mm, leg_length_mm, feed_multiplier;
        std::optional<double> return_pitch_mm;
    };
    struct Trial {
        Vec2d start_mm, end_mm;
        double height_mm, length_mm, speed_mm_s, extra_prefeed_mm;
        int temperature_c{0};
        std::optional<Turn> turn;
    };
    struct TemperatureControl {
        unsigned physical_tool;
        const char* sensor;
        int tolerance_tenths_c;
    };
    unsigned base_slot{0}, fiber_slot{0};
    std::vector<int> filament_map;
    double base_top_mm{0}, layer_height_mm{0};
    std::vector<Trial> trials;
    std::optional<TemperatureControl> temperature;
};
}
