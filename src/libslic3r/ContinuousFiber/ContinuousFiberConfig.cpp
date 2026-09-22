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

bool has_fiber_tool(const GCodeConfig& config)
{
    return std::find(config.toolhead_process_capabilities.values.begin(),
                     config.toolhead_process_capabilities.values.end(), "continuous_fiber") !=
           config.toolhead_process_capabilities.values.end();
}

bool tool_accepts_process(const GCodeConfig& config, unsigned physical, const std::string& process)
{
    if (physical >= config.physical_extruder_map.size()) return false;
    // Older thermoplastic profiles have a single default capability entry.
    const auto& capabilities = config.toolhead_process_capabilities.values;
    const std::string capability = physical < capabilities.size() ? capabilities[physical] :
        (has_fiber_tool(config) ? "" : "thermoplastic");
    return (process == "thermoplastic" || process == "continuous_fiber") && capability == process;
}

void validate_material_tool_bindings(const GCodeConfig& config)
{
    const bool cfsys = std::find(config.toolhead_fiber_protocol_id.values.begin(),
        config.toolhead_fiber_protocol_id.values.end(), "cfsys-v1") != config.toolhead_fiber_protocol_id.values.end();
    // Other machines may still have provisional auto-mapping and broadcast
    // material vectors here; their final fiber paths use resolve_fiber_tool.
    if (!cfsys && config.toolhead_filament_capacity.values.empty()) return;
    if (!has_fiber_tool(config) && !cfsys &&
        std::find(config.filament_process_type.values.begin(), config.filament_process_type.values.end(),
                  "continuous_fiber") == config.filament_process_type.values.end()) return;
    if (cfsys && (config.physical_extruder_map.size() != 2 ||
        config.toolhead_process_capabilities.values != std::vector<std::string>{"thermoplastic", "continuous_fiber"}))
        throw std::invalid_argument("CFSYS requires thermoplastic T0 and continuous-fiber T1");
    if (config.filament_map.size() != config.filament_diameter.size() ||
        config.filament_process_type.size() != config.filament_diameter.size())
        throw std::invalid_argument("Every material must have a process type and tool mapping");
    std::vector<size_t> counts(config.physical_extruder_map.size(), 0);
    const auto& capacities = config.toolhead_filament_capacity.values;
    if (!capacities.empty() && capacities.size() != counts.size())
        throw std::invalid_argument("Material capacities must match the physical tool count");
    for (size_t filament = 0; filament < config.filament_map.size(); ++filament) {
        const int logical = config.filament_map.values[filament] - 1;
        if (logical < 0 || size_t(logical) >= config.physical_extruder_map.size())
            throw std::invalid_argument("Invalid tool mapping for material " + std::to_string(filament));
        const int physical = config.physical_extruder_map.values[logical];
        if (physical < 0 || !tool_accepts_process(config, unsigned(physical), config.filament_process_type.values[filament]))
            throw std::invalid_argument("Material " + std::to_string(filament) + " is incompatible with physical tool " + std::to_string(physical));
        if (is_fiber_filament(config, unsigned(filament))) resolve_fiber_tool(config, unsigned(filament));
        if (!capacities.empty() && (capacities[physical] < 1 || ++counts[physical] > size_t(capacities[physical])))
            throw std::invalid_argument("Material capacity exceeded for physical tool " + std::to_string(physical));
    }
    if (!capacities.empty() && std::find(counts.begin(), counts.end(), 0) != counts.end())
        throw std::invalid_argument("Each configured physical tool requires an initial material slot");
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
    // The device contract fixes physical T1, not the material's list position.
    if (protocol == "cfsys-v1" && (config.gcode_flavor != gcfKlipper || units != 1.0 ||
        physical != 1))
        throw std::invalid_argument("CFSYS requires physical fiber T1 with linear millimeter E");
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

void validate_fiber_process_config(const ContinuousFiberConfig& config, FiberPathPurpose purpose, bool closed_path)
{
    const auto check = [](double value, const char* name, bool positive = false) {
        if (!std::isfinite(value) || (positive ? value <= 0 : value < 0))
            throw std::invalid_argument(std::string(name) + (positive ? " must be finite and positive" : " must be finite and non-negative"));
    };
    const std::pair<const char*, double> common[] = {
        {"fiber_minimum_path_length", config.minimum_path_length_mm},
        {"fiber_minimum_effective_length", config.minimum_effective_length_mm},
        {"fiber_cut_to_contact_length", config.cut_to_contact_length_mm},
        {"fiber_prefeed_extra_length", config.prefeed_extra_length_mm},
        {"fiber_prefeed_speed", config.prefeed_speed_mm_s},
        {"fiber_z_hop_height", config.z_hop_height_mm},
        {"fiber_landing_length", config.landing_length_mm},
        {"fiber_landing_speed", config.landing_speed_mm_s},
        {"fiber_start_speed", config.start_speed_mm_s},
        {"fiber_start_stabilization_length", config.start_stabilization_length_mm},
        {"fiber_outside_tolerance", config.outside_tolerance_mm2},
        {"fiber_resin_overlap", config.resin_overlap_mm}
    };
    for (const auto& [name, value] : common) check(value, name);
    check(config.corner_transition_length_mm, "fiber_corner_transition_length", true);
    check(config.speed_sampling_length_mm, "fiber_speed_sampling_length", true);
    if (config.layer_interval <= 0 || config.adhesion_dwell_ms < 0)
        throw std::invalid_argument("Invalid continuous fiber layer interval or adhesion dwell");
    if (config.cut_to_contact_length_mm + config.prefeed_extra_length_mm > 0)
        check(config.prefeed_speed_mm_s, "fiber_prefeed_speed", true);
    if (config.z_hop_height_mm > 0 || config.landing_length_mm > 0)
        check(config.landing_speed_mm_s, "fiber_landing_speed", true);
    if (config.start_stabilization_length_mm > 0)
        check(config.start_speed_mm_s, "fiber_start_speed", true);
    if (config.cut_to_contact_length_mm > 0) {
        check(config.tail_min_speed_mm_s, "fiber_tail_min_speed", true);
        check(config.tail_max_speed_mm_s, "fiber_tail_max_speed", true);
        check(config.tail_speed_step_length_mm, "fiber_tail_speed_step_length", true);
        if (config.tail_min_speed_mm_s > config.tail_max_speed_mm_s)
            throw std::invalid_argument("Continuous fiber tail minimum speed exceeds maximum speed");
    }
    const bool contour = purpose == FiberPathPurpose::Contour;
    const double minimum = contour ? config.contour_min_speed_mm_s : config.infill_min_speed_mm_s;
    const double maximum = contour ? config.contour_max_speed_mm_s : config.infill_max_speed_mm_s;
    check(minimum, contour ? "fiber_contour_min_speed" : "fiber_infill_min_speed", true);
    check(maximum, contour ? "fiber_contour_max_speed" : "fiber_infill_max_speed", true);
    if (minimum > maximum) throw std::invalid_argument("Continuous fiber minimum speed exceeds maximum speed");
    check(contour ? config.contour_feed_ratio : config.infill_feed_ratio, "fiber feed ratio", true);
    check(contour ? config.contour_feed_correction : config.infill_feed_correction, "fiber feed correction", true);
    check(contour ? config.contour_acceleration_mm_s2 : config.infill_acceleration_mm_s2, "fiber acceleration", true);
    const bool loop_finish = contour && closed_path;
    const double finish = loop_finish ? config.finish_overlap_length_mm : config.finish_extension_length_mm;
    check(finish, loop_finish ? "fiber_finish_overlap_length" : "fiber_finish_extension_length");
    if (finish > 0) check(config.finish_motion_speed_mm_s, "fiber_finish_motion_speed", true);
    if (contour) {
        check(config.contour_boundary_clearance_mm, "fiber_contour_boundary_clearance");
        check(config.contour_bend_radius_mm, "fiber_contour_bend_radius");
        if (config.contour_count < 0) throw std::invalid_argument("Invalid fiber contour count");
    }
    if (config.contour_enabled && config.infill_enabled)
        check(config.contour_infill_clearance_mm, "fiber_contour_infill_clearance");
}

ContinuousFiberConfig resolve_continuous_fiber_config(const Layer& layer, const LayerRegion& region)
{
    const PrintRegionConfig& source = region.region().config();
    ContinuousFiberConfig result;
    result.contour_enabled = source.generate_reinforced_perimeters.value;
    result.infill_enabled = source.generate_reinforced_infills.value;
    result.layer_interval = std::max(1, source.fiber_layer_height_ratio.value);
    result.minimum_path_length_mm = source.fiber_minimum_path_length.value;
    result.minimum_effective_length_mm = source.fiber_minimum_effective_length.value;
    result.resin_overlap_mm = source.fiber_resin_overlap.value;
    result.prefeed_extra_length_mm = source.fiber_prefeed_extra_length.value;
    result.prefeed_speed_mm_s = source.fiber_prefeed_speed.value;
    result.z_hop_height_mm = source.fiber_z_hop_height.value;
    result.landing_length_mm = source.fiber_landing_length.value;
    result.landing_speed_mm_s = source.fiber_landing_speed.value;
    result.adhesion_dwell_ms = std::max(0, source.fiber_adhesion_dwell_ms.value);
    result.start_speed_mm_s = source.fiber_start_speed.value;
    result.start_stabilization_length_mm = source.fiber_start_stabilization_length.value;
    result.outside_tolerance_mm2 = source.fiber_outside_tolerance.value;
    result.corner_transition_length_mm = source.fiber_corner_transition_length.value;
    result.speed_sampling_length_mm = source.fiber_speed_sampling_length.value;
    result.tail_min_speed_mm_s = source.fiber_tail_min_speed.value;
    result.tail_max_speed_mm_s = source.fiber_tail_max_speed.value;
    result.tail_speed_step_length_mm = source.fiber_tail_speed_step_length.value;
    result.finish_motion_speed_mm_s = source.fiber_finish_motion_speed.value;
    result.finish_extension_length_mm = source.fiber_finish_extension_length.value;
    if (result.contour_enabled && result.infill_enabled)
        result.contour_infill_clearance_mm = source.fiber_contour_infill_clearance.value;

    const PrintConfig& print_config = layer.object()->print()->config();
    if (print_config.fiber_cut_gcode.value.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::runtime_error("Continuous fiber is enabled, but the selected machine has no fiber cut command");
    result.cut_to_contact_length_mm = print_config.fiber_cut_to_contact_length.value;
    const auto correction_for = [&](unsigned material) {
        if (material == 0 || material > print_config.filament_fiber_feed_correction.values.size())
            throw std::runtime_error("Missing fiber feed correction for material");
        return print_config.filament_fiber_feed_correction.values[material - 1];
    };
    const auto flow_for = [&](unsigned material, const ConfigOptionFloatOrPercent& width) {
        if (material == 0 || material > print_config.filament_map.values.size())
            throw std::runtime_error("Continuous fiber material has no extruder mapping");
        const int extruder = print_config.filament_map.values[material - 1] - 1;
        if (extruder < 0 || size_t(extruder) >= print_config.nozzle_diameter.values.size())
            throw std::runtime_error("Continuous fiber extruder is outside the configured nozzle set");
        const float nozzle = float(print_config.nozzle_diameter.values[extruder]);
        const Flow flow = Flow::new_from_config_width(frInfill, width, nozzle, float(layer.height));
        if (!std::isfinite(flow.width()) || flow.width() <= 0 || result.resin_overlap_mm >= 0.5 * flow.width())
            throw std::runtime_error("Fiber width must be positive and fiber_resin_overlap smaller than half its width");
        return flow;
    };
    if (result.contour_enabled) {
        result.contour_include_holes = source.fiber_contour_include_holes.value;
        result.contour_count = std::max(0, source.outer_reinforced_perimeters_counts.value);
        result.contour_material = unsigned(std::max(1, source.reinforced_perimeters_filament.value));
        result.contour_boundary_clearance_mm = source.fiber_contour_boundary_clearance.value;
        result.contour_bend_radius_mm = source.fiber_contour_bend_radius.value;
        result.contour_min_speed_mm_s = source.fiber_contour_min_speed.value;
        result.contour_max_speed_mm_s = source.fiber_contour_max_speed.value;
        result.contour_acceleration_mm_s2 = source.fiber_contour_acceleration.value;
        result.contour_feed_ratio = source.fiber_contour_feed_ratio.value;
        result.contour_feed_correction = correction_for(result.contour_material);
        result.finish_overlap_length_mm = source.fiber_finish_overlap_length.value;
        validate_fiber_process_config(result, FiberPathPurpose::Contour);
        validate_fiber_process_config(result, FiberPathPurpose::Contour, false);
        result.contour_flow = flow_for(result.contour_material, source.reinforced_perimeters_extrusion_width);
    }
    if (result.infill_enabled) {
        result.infill_pattern = source.reinforced_infill_pattern.value;
        if (result.infill_pattern != ipRectilinear && result.infill_pattern != ipConcentric)
            throw std::runtime_error("Continuous fiber infill supports only rectilinear and concentric patterns");
        if (!std::isfinite(source.reinforced_infill_density.value))
            throw std::runtime_error("reinforced_infill_density must be finite");
        result.infill_density = std::clamp(source.reinforced_infill_density.value, 0.0, 100.0);
        result.infill_material = unsigned(std::max(1, source.reinforced_infill_filament.value));
        result.infill_min_speed_mm_s = source.fiber_infill_min_speed.value;
        result.infill_max_speed_mm_s = source.fiber_infill_max_speed.value;
        result.infill_acceleration_mm_s2 = source.fiber_infill_acceleration.value;
        result.infill_feed_ratio = source.fiber_infill_feed_ratio.value;
        result.infill_feed_correction = correction_for(result.infill_material);
        validate_fiber_process_config(result, FiberPathPurpose::Infill);
        result.infill_flow = flow_for(result.infill_material, source.reinforced_infill_extrusion_width);
    }
    return result;
}

} // namespace Slic3r
