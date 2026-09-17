#ifndef slic3r_FiberPolicyKey_hpp_
#define slic3r_FiberPolicyKey_hpp_

#include "ContinuousFiberConfig.hpp"

#include <cmath>
#include <stdexcept>
#include <tuple>

namespace Slic3r {

// Region identity is deliberately absent. Two contributors may share a fiber
// domain only when every value affecting candidate geometry, validation, or
// process output is identical.
struct FiberPolicyKey {
    bool contour_enabled { false };
    bool infill_enabled { false };
    int layer_interval { 1 };
    int contour_count { 0 };
    InfillPattern infill_pattern { ipRectilinear };
    double infill_density { 0.0 };
    double infill_direction { 0.0 };
    bool fixed_direction { false };

    double contour_width { 0.0 };
    double contour_spacing { 0.0 };
    double contour_height { 0.0 };
    double contour_nozzle { 0.0 };
    double infill_width { 0.0 };
    double infill_spacing { 0.0 };
    double infill_height { 0.0 };
    double infill_nozzle { 0.0 };

    unsigned contour_material { 0 };
    unsigned infill_material { 0 };
    double minimum_path_length_mm { 0.0 };
    double minimum_segment_length_mm { 0.0 };
    double maximum_turn_angle_degrees { 180.0 };
    double cut_to_contact_length_mm { 0.0 };
    double prefeed_extra_length_mm { 0.0 };
    double prefeed_speed_mm_s { 0.0 };
    double z_hop_height_mm { 0.0 };
    double landing_length_mm { 0.0 };
    double landing_speed_mm_s { 0.0 };
    int adhesion_dwell_ms { 0 };
    double start_speed_mm_s { 0.0 };
    double start_stabilization_length_mm { 0.0 };
    double minimum_effective_length_mm { 0.0 };
    double finish_extension_length_mm { 0.0 };
    double outside_tolerance_mm2 { 0.0 };
    double contour_max_speed_mm_s { 0.0 };
    double infill_max_speed_mm_s { 0.0 };
    double contour_acceleration_mm_s2 { 0.0 };
    double infill_acceleration_mm_s2 { 0.0 };
    double contour_infill_clearance_mm { 0.0 };
    double resin_overlap_mm { 0.0 };

    double contour_feed_ratio { 0.0 };
    double infill_feed_ratio { 0.0 };
    double contour_min_speed_mm_s { 0.0 };
    double infill_min_speed_mm_s { 0.0 };
    double corner_transition_length_mm { 0.0 };
    double speed_sampling_length_mm { 0.0 };
    double tail_min_speed_mm_s { 0.0 };
    double tail_max_speed_mm_s { 0.0 };
    double tail_speed_step_length_mm { 0.0 };
    double finish_overlap_length_mm { 0.0 };
    double finish_motion_speed_mm_s { 0.0 };
    double contour_feed_correction { 0.0 };
    double infill_feed_correction { 0.0 };
    double contour_boundary_clearance_mm { 0.0 };

    auto values() const
    {
        return std::tie(
            contour_enabled, infill_enabled, layer_interval, contour_count,
            infill_pattern, infill_density, infill_direction, fixed_direction,
            contour_width, contour_spacing, contour_height, contour_nozzle,
            infill_width, infill_spacing, infill_height, infill_nozzle,
            contour_material, infill_material, minimum_path_length_mm,
            minimum_segment_length_mm, maximum_turn_angle_degrees,
            cut_to_contact_length_mm, prefeed_extra_length_mm, prefeed_speed_mm_s,
            z_hop_height_mm, landing_length_mm, landing_speed_mm_s, adhesion_dwell_ms,
            start_speed_mm_s, start_stabilization_length_mm,
            minimum_effective_length_mm, finish_extension_length_mm,
            outside_tolerance_mm2, contour_max_speed_mm_s, infill_max_speed_mm_s,
            contour_acceleration_mm_s2, infill_acceleration_mm_s2,
            contour_infill_clearance_mm, resin_overlap_mm,
            contour_feed_ratio, infill_feed_ratio, contour_min_speed_mm_s, infill_min_speed_mm_s, corner_transition_length_mm, speed_sampling_length_mm, tail_min_speed_mm_s, tail_max_speed_mm_s, tail_speed_step_length_mm, finish_overlap_length_mm, finish_motion_speed_mm_s, contour_feed_correction, infill_feed_correction, contour_boundary_clearance_mm);
    }

    bool operator<(const FiberPolicyKey& rhs) const { return values() < rhs.values(); }
    bool operator==(const FiberPolicyKey& rhs) const { return values() == rhs.values(); }
};

inline FiberPolicyKey fiber_policy_key(
    const ContinuousFiberConfig& config,
    double infill_direction,
    bool fixed_direction)
{
    if (!std::isfinite(infill_direction))
        throw std::runtime_error("Continuous fiber infill direction must be finite");
    return {
        config.contour_enabled,
        config.infill_enabled,
        config.layer_interval,
        config.contour_count,
        config.infill_pattern,
        config.infill_density,
        infill_direction,
        fixed_direction,
        config.contour_flow.width(),
        config.contour_flow.spacing(),
        config.contour_flow.height(),
        config.contour_flow.nozzle_diameter(),
        config.infill_flow.width(),
        config.infill_flow.spacing(),
        config.infill_flow.height(),
        config.infill_flow.nozzle_diameter(),
        config.contour_material,
        config.infill_material,
        config.minimum_path_length_mm,
        config.minimum_segment_length_mm,
        config.maximum_turn_angle_degrees,
        config.cut_to_contact_length_mm,
        config.prefeed_extra_length_mm,
        config.prefeed_speed_mm_s,
        config.z_hop_height_mm,
        config.landing_length_mm,
        config.landing_speed_mm_s,
        config.adhesion_dwell_ms,
        config.start_speed_mm_s,
        config.start_stabilization_length_mm,
        config.minimum_effective_length_mm,
        config.finish_extension_length_mm,
        config.outside_tolerance_mm2,
        config.contour_max_speed_mm_s,
        config.infill_max_speed_mm_s,
        config.contour_acceleration_mm_s2,
        config.infill_acceleration_mm_s2,
        config.contour_infill_clearance_mm,
        config.resin_overlap_mm,
        config.contour_feed_ratio,
        config.infill_feed_ratio,
        config.contour_min_speed_mm_s,
        config.infill_min_speed_mm_s,
        config.corner_transition_length_mm,
        config.speed_sampling_length_mm,
        config.tail_min_speed_mm_s,
        config.tail_max_speed_mm_s,
        config.tail_speed_step_length_mm,
        config.finish_overlap_length_mm,
        config.finish_motion_speed_mm_s,
        config.contour_feed_correction,
        config.infill_feed_correction,
        config.contour_boundary_clearance_mm
    };
}

} // namespace Slic3r

#endif
