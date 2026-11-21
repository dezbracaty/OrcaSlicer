#pragma once

#include "../Point.hpp"
#include <vector>

namespace Slic3r {
struct FiberCalibrationPlan {
    struct Trial {
        Vec2d start_mm, end_mm;
        double height_mm, length_mm, speed_mm_s, extra_prefeed_mm;
    };
    unsigned base_slot{0}, fiber_slot{0};
    std::vector<int> filament_map;
    double base_top_mm{0}, layer_height_mm{0};
    std::vector<Trial> trials;
};
}
