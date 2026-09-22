#ifndef slic3r_ContinuousFiberConfig_hpp_
#define slic3r_ContinuousFiberConfig_hpp_

#include "../Flow.hpp"
#include "FiberSource.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

class Layer;
class LayerRegion;

struct ContinuousFiberConfig {
    bool contour_enabled { false };
    bool contour_include_holes { true };
    bool infill_enabled { false };
    int layer_interval { 1 };
    int contour_count { 0 };
    InfillPattern infill_pattern { ipRectilinear };
    double infill_density { 0.0 };
    Flow contour_flow;
    Flow infill_flow;
    unsigned contour_material { 0 };
    unsigned infill_material { 0 };
    double minimum_path_length_mm { 0.0 };
    double cut_to_contact_length_mm { 0.0 };
    double prefeed_extra_length_mm { 0.0 };
    double prefeed_speed_mm_s { 10.0 };
    double z_hop_height_mm { 0.0 };
    double landing_length_mm { 0.0 };
    double landing_speed_mm_s { 3.0 };
    int adhesion_dwell_ms { 0 };
    double start_speed_mm_s { 10.0 };
    double start_stabilization_length_mm { 0.0 };
    double minimum_effective_length_mm { 0.0 };
    double finish_extension_length_mm { 0.0 };
    double outside_tolerance_mm2 { 0.01 };
    double contour_max_speed_mm_s { 10.0 };
    double infill_max_speed_mm_s { 10.0 };
    double contour_acceleration_mm_s2 { 300.0 };
    double infill_acceleration_mm_s2 { 300.0 };
    double contour_infill_clearance_mm { 0.0 };
    double resin_overlap_mm { 0.0 };
    // Independent line-feed model; Flow is geometry only.
    double contour_feed_ratio { 1.0 };
    double infill_feed_ratio { 1.0 };
    double contour_feed_correction { 1.0 };
    double infill_feed_correction { 1.0 };
    double contour_boundary_clearance_mm { 0.0 };
    double contour_bend_radius_mm { 0.0 };
    double contour_min_speed_mm_s { 3.0 };
    double infill_min_speed_mm_s { 3.0 };
    double corner_transition_length_mm { 5.0 };
    double speed_sampling_length_mm { 2.0 };
    double tail_min_speed_mm_s { 3.0 };
    double tail_max_speed_mm_s { 10.0 };
    double tail_speed_step_length_mm { 2.0 };
    double finish_overlap_length_mm { 0.0 };
    double finish_motion_speed_mm_s { 3.0 };

    bool enabled() const { return contour_enabled || infill_enabled; }
    bool active_on_layer(size_t layer_id) const
    {
        return enabled() && layer_interval > 0 && layer_id % size_t(layer_interval) == 0;
    }
};

bool continuous_fiber_enabled(const PrintRegionConfig& config);
bool is_fiber_filament(const GCodeConfig& config, unsigned filament);
// Material indices, logical extruders and physical tool IDs are distinct.
bool has_fiber_tool(const GCodeConfig& config);
bool tool_accepts_process(const GCodeConfig& config, unsigned physical, const std::string& process);
void validate_material_tool_bindings(const GCodeConfig& config);

struct ResolvedFiberTool {
    unsigned logical_filament_id;
    unsigned logical_extruder_id;
    unsigned physical_tool_id;
    double e_units_per_mm;
};
ResolvedFiberTool resolve_fiber_tool(const GCodeConfig& config, unsigned filament);

// Device protocols select command vocabulary, never an alternative geometry or
// speed algorithm. CFSYS uses the existing slicer's machine boundary scripts.
enum class FiberMachineProtocol { LinearE, Cfsys };
FiberMachineProtocol fiber_machine_protocol(const GCodeConfig& config);
void require_fiber_safe_script(const std::string& script, const char* name,
    FiberMachineProtocol protocol = FiberMachineProtocol::LinearE, bool expanded = false);
void validate_fiber_cut_event(const std::string& script,
    FiberMachineProtocol protocol = FiberMachineProtocol::LinearE);
// Validates common process values and only the requested path family.
void validate_fiber_process_config(const ContinuousFiberConfig& config, FiberPathPurpose purpose);
ContinuousFiberConfig resolve_continuous_fiber_config(const Layer& layer, const LayerRegion& region);

} // namespace Slic3r

#endif
