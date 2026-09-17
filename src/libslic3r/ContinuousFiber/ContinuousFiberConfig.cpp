#include "ContinuousFiberConfig.hpp"

#include "../Layer.hpp"
#include "../Print.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <set>

namespace Slic3r {

FiberMachineProtocol fiber_machine_protocol(const GCodeConfig& config)
{
    return std::find(config.toolhead_fiber_protocol_id.values.begin(),
                     config.toolhead_fiber_protocol_id.values.end(), "cfsys-v1") !=
                     config.toolhead_fiber_protocol_id.values.end() ?
        FiberMachineProtocol::Cfsys : FiberMachineProtocol::LinearE;
}

bool is_fiber_filament(const GCodeConfig& config, unsigned filament)
{
    return filament < config.filament_process_type.values.size() &&
           config.filament_process_type.values[filament] == "continuous_fiber";
}

ResolvedFiberTool resolve_fiber_tool(const GCodeConfig& config, unsigned filament)
{
    if (!is_fiber_filament(config, filament))
        throw std::invalid_argument("Fiber path requires an explicitly declared continuous_fiber material");
    if (filament >= config.filament_map.values.size())
        throw std::invalid_argument("Missing fiber filament mapping");
    const int logical = config.filament_map.values[filament]-1;
    if (logical < 0 || size_t(logical) >= config.physical_extruder_map.values.size())
        throw std::invalid_argument("Missing fiber physical extruder mapping");
    if (std::count(config.physical_extruder_map.values.begin(), config.physical_extruder_map.values.end(),
                   config.physical_extruder_map.values[logical]) != 1)
        throw std::invalid_argument("Fiber physical tool mapping is ambiguous");
    const int physical = config.physical_extruder_map.values[logical];
    if (physical < 0 || size_t(physical) >= config.toolhead_process_capabilities.values.size() ||
        config.toolhead_process_capabilities.values[physical] != "continuous_fiber")
        throw std::invalid_argument("Fiber material is not mapped to a fiber-only physical tool");
    if (size_t(physical) >= config.toolhead_fiber_e_units_per_mm.values.size() ||
        size_t(physical) >= config.toolhead_fiber_protocol_id.values.size())
        throw std::invalid_argument("Missing physical fiber tool protocol or E units");
    const auto& protocol = config.toolhead_fiber_protocol_id.values[physical];
    if (protocol != "linear-e-v1" && protocol != "cfsys-v1")
        throw std::invalid_argument("Unsupported fiber machine protocol");
    const double units = config.toolhead_fiber_e_units_per_mm.values[physical];
    if (!std::isfinite(units) || units <= 0)
        throw std::invalid_argument("Invalid physical fiber E units/mm");
    // Existing CFSYS startup macros explicitly address the two material slots
    // and T0/T1. Do not silently reinterpret that device interface as a remap.
    if (protocol == "cfsys-v1" && (config.gcode_flavor != gcfKlipper || units != 1.0 ||
        filament != 1 || logical != 1 || physical != 1 || config.filament_map.values.front() != 1 ||
        config.physical_extruder_map.values.front() != 0))
        throw std::invalid_argument("CFSYS requires resin slot/T0 and fiber slot/T1 with linear millimeter E");
    if (config.filament_slots_bound_to_physical_tools.value && filament != unsigned(physical))
        throw std::invalid_argument("Fiber mapping conflicts with physical slot binding");
    return {filament, unsigned(logical), unsigned(physical), units};
}

void require_fiber_safe_script(const std::string& script, const char* name,
    FiberMachineProtocol protocol, bool expanded)
{
    const std::string scope(name);
    const bool startup = scope == "machine_start_gcode";
    const bool shutdown = scope == "machine_end_gcode";
    // Explicit command list from the old CFSYS CF1/Alpha500 machine profiles.
    // Opaque firmware setup/teardown is confined to machine boundaries, not
    // smuggled into a deposited Fiber span or a material-change script.
    static const std::set<std::string> startup_macros {
        "CF1004", "CF1005", "CF1006", "CF1008", "CF1009", "CF1010", "CF1011", "CF1012",
        "CF1013", "CF1015", "CF1016", "CF1018", "CF1019",
        "DF1004", "DF1005", "DF1006", "DF1008", "DF1009", "DF1010", "DF1011", "DF1012",
        "DF1013", "DF1015", "DF1016", "DF1018", "DF1019",
        "CLEAR_PAUSE", "Wipe_the_nozzle", "PRINT_START", "t_z_offset_calibrate", "SET_HEATER_TEMPERATURE"
    };
    static const std::set<std::string> setup_commands {
        "G28", "M104", "M109", "M140", "M190", "M141", "M191", "M106", "M107", "M400", "G4"
    };
    std::istringstream input(script);
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find(';');
        if (comment != std::string::npos &&
            (line.substr(comment + 1).find("FIBER_") == 0 || line.substr(comment + 1).find("TOOL_BINDING ") == 0))
            throw std::invalid_argument("Custom scripts cannot inject Fiber process metadata");
        line = line.substr(0, comment);
        std::istringstream words(line);
        std::string command;
        if (!(words >> command)) continue;
        bool allowed = false;
        if (protocol == FiberMachineProtocol::Cfsys) {
            allowed = (startup && startup_macros.count(command)) ||
                (shutdown && command == "PRINT_END") ||
                ((startup || shutdown) && setup_commands.count(command)) ||
                (shutdown && command == "M84") ||
                (scope == "machine_pause_gcode" && (command == "PAUSE" || command == "M601"));
            if (startup && (command == "T0" || command == "T1" || command == "G90" || command == "M82" || command == "M83")) {
                std::string extra;
                allowed = !(words >> extra);
            }
            if (scope == "before_layer_change_gcode" && command == "G92") {
                char axis = 0; double position = 1; std::string extra;
                allowed = bool(words >> axis >> position) && axis == 'E' && position == 0 && !(words >> extra);
            }
            if (allowed && expanded && setup_commands.count(command)) {
                // Standard commands must have numeric, command-specific arguments.
                const std::string axes = command == "G28" ? "XYZ" : command == "G4" ? "PS" :
                    (command == "M104" || command == "M109") ? "STR" :
                    command == "M106" ? "PS" : (command == "M400" || command == "M107") ? "" : "SR";
                char axis; double value;
                while (words >> axis) {
                    if (axes.find(axis) == std::string::npos || !(words >> value) || !std::isfinite(value)) {
                        allowed = false;
                        break;
                    }
                }
            }
        }
        if (!allowed)
            throw std::invalid_argument(std::string("Unsupported Fiber machine script command in ") + name + ": " + command);
    }
}

void validate_fiber_cut_event(const std::string& script, FiberMachineProtocol protocol)
{
    if (protocol == FiberMachineProtocol::Cfsys) {
        std::vector<std::string> commands;
        std::istringstream input(script);
        std::string line;
        while (std::getline(input, line)) {
            line = line.substr(0, line.find(';'));
            std::istringstream words(line);
            std::string command, extra;
            if (!(words >> command)) continue;
            if (words >> extra) throw std::invalid_argument("Unexpected CFSYS cut argument");
            commands.push_back(command);
        }
        if (commands != std::vector<std::string>{"M400", "S0", "M400"})
            throw std::invalid_argument("CFSYS cut event must be M400 / S0 / M400");
        return;
    }
    // A deliberately small standard-command protocol, not a macro whitelist.
    // No templates, modal changes, tool selection, axis movement or hidden macros.
    bool actuator = false, waited = false;
    std::istringstream input(script);
    std::string line;
    while (std::getline(input, line)) {
        line = line.substr(0, line.find(';'));
        std::istringstream words(line);
        std::string command;
        if (!(words >> command)) continue;
        if (command != "M400" && command != "M42" && command != "G4")
            throw std::invalid_argument("Unverified fiber cut command: " + command);
        char key; double value; bool pin = false, state = false, duration = false;
        while (words >> key) {
            if (!(words >> value) || !std::isfinite(value) || value < 0)
                throw std::invalid_argument("Invalid fiber cut parameter");
            if (command == "M42" && key == 'P') pin = true;
            else if (command == "M42" && key == 'S' && value <= 255) state = true;
            else if (command == "G4" && (key == 'P' || key == 'S') && value > 0) duration = true;
            else throw std::invalid_argument("Unsupported fiber cut parameter");
        }
        if (command == "M42") {
            if (!pin || !state) throw std::invalid_argument("Fiber cut needs an explicit pin and state");
            actuator = true; waited = false;
        }
        if (command == "G4") {
            if (!duration) throw std::invalid_argument("Fiber cut needs a positive settling time");
            if (actuator) waited = true;
        }
    }
    if (!actuator || !waited)
        throw std::invalid_argument("Fiber cut requires an explicit actuator and subsequent settling wait");
}

bool continuous_fiber_enabled(const PrintRegionConfig& config)
{
    return config.generate_reinforced_perimeters.value || config.generate_reinforced_infills.value;
}

ContinuousFiberConfig resolve_continuous_fiber_config(const Layer& layer, const LayerRegion& region)
{
    const PrintRegionConfig& source = region.region().config();
    const auto finite_nonnegative = [](double value, const char* key) {
        if (!std::isfinite(value) || value < 0.0)
            throw std::runtime_error(std::string(key) + " must be finite and non-negative");
        return value;
    };
    const auto finite_positive = [](double value, const char* key) {
        if (!std::isfinite(value) || value <= 0.0)
            throw std::runtime_error(std::string(key) + " must be finite and positive");
        return value;
    };
    ContinuousFiberConfig result;
    result.contour_enabled = source.generate_reinforced_perimeters.value;
    result.infill_enabled = source.generate_reinforced_infills.value;
    result.layer_interval = std::max(1, source.fiber_layer_height_ratio.value);
    result.contour_count = std::max(0, source.outer_reinforced_perimeters_counts.value);
    result.infill_pattern = source.reinforced_infill_pattern.value;
    if (!std::isfinite(source.reinforced_infill_density.value))
        throw std::runtime_error("reinforced_infill_density must be finite");
    result.infill_density = std::clamp(source.reinforced_infill_density.value, 0.0, 100.0);
    result.contour_material = unsigned(std::max(1, source.reinforced_perimeters_filament.value));
    result.infill_material = unsigned(std::max(1, source.reinforced_infill_filament.value));
    result.minimum_path_length_mm = finite_nonnegative(source.fiber_minimum_path_length.value, "fiber_minimum_path_length");
    result.minimum_segment_length_mm = finite_nonnegative(source.fiber_minimum_segment_length.value, "fiber_minimum_segment_length");
    result.maximum_turn_angle_degrees = finite_nonnegative(source.fiber_maximum_turn_angle.value, "fiber_maximum_turn_angle");
    if (result.maximum_turn_angle_degrees > 180.0)
        throw std::runtime_error("fiber_maximum_turn_angle must not exceed 180 degrees");
    result.minimum_effective_length_mm = finite_nonnegative(source.fiber_minimum_effective_length.value, "fiber_minimum_effective_length");
    result.contour_infill_clearance_mm = finite_nonnegative(source.fiber_contour_infill_clearance.value, "fiber_contour_infill_clearance");
    result.contour_boundary_clearance_mm = finite_nonnegative(source.fiber_contour_boundary_clearance.value, "fiber_contour_boundary_clearance");
    result.resin_overlap_mm = finite_nonnegative(source.fiber_resin_overlap.value, "fiber_resin_overlap");
    result.prefeed_extra_length_mm = finite_nonnegative(source.fiber_prefeed_extra_length.value, "fiber_prefeed_extra_length");
    result.prefeed_speed_mm_s = finite_nonnegative(source.fiber_prefeed_speed.value, "fiber_prefeed_speed");
    result.z_hop_height_mm = finite_nonnegative(source.fiber_z_hop_height.value, "fiber_z_hop_height");
    result.landing_length_mm = finite_nonnegative(source.fiber_landing_length.value, "fiber_landing_length");
    result.landing_speed_mm_s = finite_nonnegative(source.fiber_landing_speed.value, "fiber_landing_speed");
    result.adhesion_dwell_ms = std::max(0, source.fiber_adhesion_dwell_ms.value);
    result.start_speed_mm_s = finite_nonnegative(source.fiber_start_speed.value, "fiber_start_speed");
    result.start_stabilization_length_mm = finite_nonnegative(source.fiber_start_stabilization_length.value, "fiber_start_stabilization_length");
    result.finish_extension_length_mm = finite_nonnegative(source.fiber_finish_extension_length.value, "fiber_finish_extension_length");
    result.outside_tolerance_mm2 = finite_nonnegative(source.fiber_outside_tolerance.value, "fiber_outside_tolerance");
    result.contour_max_speed_mm_s = finite_nonnegative(source.fiber_contour_max_speed.value, "fiber_contour_max_speed");
    result.infill_max_speed_mm_s = finite_nonnegative(source.fiber_infill_max_speed.value, "fiber_infill_max_speed");
    result.contour_acceleration_mm_s2 = finite_nonnegative(source.fiber_contour_acceleration.value, "fiber_contour_acceleration");
    result.infill_acceleration_mm_s2 = finite_nonnegative(source.fiber_infill_acceleration.value, "fiber_infill_acceleration");

    result.contour_feed_ratio = finite_positive(source.fiber_contour_feed_ratio.value, "fiber_contour_feed_ratio");
    result.infill_feed_ratio = finite_positive(source.fiber_infill_feed_ratio.value, "fiber_infill_feed_ratio");
    result.contour_min_speed_mm_s = finite_positive(source.fiber_contour_min_speed.value, "fiber_contour_min_speed");
    result.infill_min_speed_mm_s = finite_positive(source.fiber_infill_min_speed.value, "fiber_infill_min_speed");
    result.corner_transition_length_mm = finite_positive(source.fiber_corner_transition_length.value, "fiber_corner_transition_length");
    result.speed_sampling_length_mm = finite_positive(source.fiber_speed_sampling_length.value, "fiber_speed_sampling_length");
    result.tail_min_speed_mm_s = finite_positive(source.fiber_tail_min_speed.value, "fiber_tail_min_speed");
    result.tail_max_speed_mm_s = finite_positive(source.fiber_tail_max_speed.value, "fiber_tail_max_speed");
    result.tail_speed_step_length_mm = finite_positive(source.fiber_tail_speed_step_length.value, "fiber_tail_speed_step_length");
    result.finish_overlap_length_mm = finite_nonnegative(source.fiber_finish_overlap_length.value, "fiber_finish_overlap_length");
    result.finish_motion_speed_mm_s = finite_positive(source.fiber_finish_motion_speed.value, "fiber_finish_motion_speed");
    if (result.contour_min_speed_mm_s > result.contour_max_speed_mm_s ||
        result.infill_min_speed_mm_s > result.infill_max_speed_mm_s ||
        result.tail_min_speed_mm_s > result.tail_max_speed_mm_s)
        throw std::runtime_error("Continuous fiber minimum speed exceeds maximum speed");

    const PrintConfig& print_config = layer.object()->print()->config();
    if (print_config.fiber_cut_gcode.value.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::runtime_error(
            "Continuous fiber is enabled, but the selected machine has no fiber cut command");
    result.cut_to_contact_length_mm = finite_nonnegative(print_config.fiber_cut_to_contact_length.value, "fiber_cut_to_contact_length");
    const auto correction_for = [&](unsigned material) {
        if (material == 0 || material > print_config.filament_fiber_feed_correction.values.size())
            throw std::runtime_error("Missing fiber feed correction for material");
        return finite_positive(print_config.filament_fiber_feed_correction.values[material - 1],
                               "filament_fiber_feed_correction");
    };
    if (result.contour_enabled)
        result.contour_feed_correction = correction_for(result.contour_material);
    if (result.infill_enabled)
        result.infill_feed_correction = correction_for(result.infill_material);
    const auto flow_for = [&](unsigned material, const ConfigOptionFloatOrPercent& width) {
        if (material == 0 || material > print_config.filament_map.values.size())
            throw std::runtime_error("Continuous fiber material has no extruder mapping");
        const int extruder = print_config.filament_map.values[material - 1] - 1;
        if (extruder < 0 || size_t(extruder) >= print_config.nozzle_diameter.values.size())
            throw std::runtime_error("Continuous fiber extruder is outside the configured nozzle set");
        const float nozzle = float(print_config.nozzle_diameter.values[extruder]);
        return Flow::new_from_config_width(frInfill, width, nozzle, float(layer.height));
    };
    result.contour_flow = flow_for(result.contour_material, source.reinforced_perimeters_extrusion_width);
    result.infill_flow = flow_for(result.infill_material, source.reinforced_infill_extrusion_width);

    const double minimum_half_width = 0.5 * std::min(result.contour_flow.width(), result.infill_flow.width());
    if (result.resin_overlap_mm >= minimum_half_width)
        throw std::runtime_error("fiber_resin_overlap must be smaller than half of the continuous fiber width");
    if (result.infill_pattern != ipRectilinear && result.infill_pattern != ipConcentric)
        throw std::runtime_error("Continuous fiber infill supports only rectilinear and concentric patterns");
    if (result.cut_to_contact_length_mm + result.prefeed_extra_length_mm > 0.0 && result.prefeed_speed_mm_s <= 0.0)
        throw std::runtime_error("fiber_prefeed_speed must be positive when continuous fiber prefeed is enabled");
    if ((result.z_hop_height_mm > 0.0 || result.landing_length_mm > 0.0) && result.landing_speed_mm_s <= 0.0)
        throw std::runtime_error("fiber_landing_speed must be positive when continuous fiber landing is enabled");
    if (result.start_stabilization_length_mm > 0.0 && result.start_speed_mm_s <= 0.0)
        throw std::runtime_error("fiber_start_speed must be positive when a start stabilization span is enabled");
    if (result.contour_enabled && result.contour_max_speed_mm_s <= 0.0)
        throw std::runtime_error("fiber_contour_max_speed must be positive when continuous fiber contours are enabled");
    if (result.infill_enabled && result.infill_max_speed_mm_s <= 0.0)
        throw std::runtime_error("fiber_infill_max_speed must be positive when continuous fiber infill is enabled");
    if (result.contour_enabled && result.contour_acceleration_mm_s2 <= 0.0)
        throw std::runtime_error("fiber_contour_acceleration must be positive when continuous fiber contours are enabled");
    if (result.infill_enabled && result.infill_acceleration_mm_s2 <= 0.0)
        throw std::runtime_error("fiber_infill_acceleration must be positive when continuous fiber infill is enabled");

    if (result.contour_enabled)
        finite_positive(result.contour_flow.width(), "continuous fiber contour width");
    if (result.infill_enabled)
        finite_positive(result.infill_flow.width(), "continuous fiber infill width");

    return result;
}

} // namespace Slic3r
