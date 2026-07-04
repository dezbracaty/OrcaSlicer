#include <catch2/catch_all.hpp>

#include "libslic3r/ConfigSDK.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::libslicer;

namespace {

bool has_issue(const std::vector<ConfigValidationIssue>& issues, const std::string& code, const std::string& field)
{
    return std::any_of(issues.begin(), issues.end(), [&](const ConfigValidationIssue& issue) {
        return issue.code == code && issue.field == field;
    });
}

std::string valid_bbl_two_filament_json()
{
    return R"json({
        "printer_model": "Bambu Lab X1 Carbon",
        "printer_settings_id": "Bambu Lab X1 Carbon 0.4 nozzle",
        "print_settings_id": "0.20mm Standard @BBL X1C",
        "filament_settings_id": ["Bambu PLA Basic @BBL X1C", "Bambu PLA Basic @BBL X1C"],
        "filament_colour": ["#FFFFFF", "#000000"],
        "filament_diameter": [1.75, 1.75],
        "filament_map": [1, 2],
        "nozzle_diameter": [0.4, 0.4],
        "extruder_type": ["Direct Drive", "Direct Drive"],
        "nozzle_volume_type": ["Standard", "Standard"],
        "default_nozzle_volume_type": ["Standard", "Standard"],
        "filament_self_index": [1, 2],
        "filament_extruder_variant": ["Direct Drive Standard", "Direct Drive Standard"],
        "support_filament": 0,
        "support_interface_filament": 0,
        "wipe_tower_filament": 0,
        "gcode_flavor": 0,
        "use_relative_e_distances": true,
        "before_layer_change_gcode": "",
        "layer_change_gcode": "; layer"
    })json";
}

} // namespace

TEST_CASE("Config SDK exposes config definitions", "[ConfigSDK]")
{
    const std::vector<ConfigDefinition> definitions = get_config_definitions();
    REQUIRE_FALSE(definitions.empty());

    const ConfigDefinition printer_model = get_config_definition("printer_model");
    REQUIRE(printer_model.key == "printer_model");
    REQUIRE(printer_model.type == ConfigValueType::String);
    REQUIRE(printer_model.scope == ConfigScope::Printer);

    const ConfigDefinition filament_map = get_config_definition("filament_map");
    REQUIRE(filament_map.cardinality == ConfigCardinality::Filament);
}

TEST_CASE("ResolvedConfig loads typed JSON and reports unknown keys as warnings", "[ConfigSDK]")
{
    std::vector<ConfigValidationIssue> load_issues;
    ResolvedConfig config = ResolvedConfig::from_json(R"json({
        "printer_model": "Bambu Lab X1 Carbon",
        "extruder_type": ["Direct Drive", "Bowden"],
        "nozzle_diameter": [0.4, 0.6],
        "unknown_worker_key": 42
    })json", &load_issues);

    REQUIRE_FALSE(has_config_errors(load_issues));
    REQUIRE(has_issue(load_issues, "unknown_config_key", "unknown_worker_key"));
    REQUIRE(config.dynamic_config().opt<ConfigOptionString>("printer_model")->value == "Bambu Lab X1 Carbon");
    REQUIRE(config.dynamic_config().opt<ConfigOptionEnumsGeneric>("extruder_type")->values.size() == 2);
    REQUIRE(config.dynamic_config().opt<ConfigOptionEnumsGeneric>("extruder_type")->values[0] == etDirectDrive);
    REQUIRE(config.dynamic_config().opt<ConfigOptionEnumsGeneric>("extruder_type")->values[1] == etBowden);
}

TEST_CASE("Resolved config validator accepts a complete BBL multi-filament config", "[ConfigSDK]")
{
    std::vector<ConfigValidationIssue> load_issues;
    ResolvedConfig config = ResolvedConfig::from_json(valid_bbl_two_filament_json(), &load_issues);

    REQUIRE_FALSE(has_config_errors(load_issues));
    const std::vector<ConfigValidationIssue> issues = validate_resolved_config(config, 0);
    REQUIRE_FALSE(has_config_errors(issues));
}

TEST_CASE("Resolved config validator catches vector and variant lookup failures", "[ConfigSDK]")
{
    std::vector<ConfigValidationIssue> load_issues;
    ResolvedConfig config = ResolvedConfig::from_json(R"json({
        "printer_model": "Bambu Lab X1 Carbon",
        "printer_settings_id": "Bambu Lab X1 Carbon 0.4 nozzle",
        "print_settings_id": "0.20mm Standard @BBL X1C",
        "filament_settings_id": ["slot 1", "slot 2"],
        "filament_colour": ["#FFFFFF", "#000000"],
        "filament_diameter": [1.75, 1.75],
        "filament_map": [1, 2],
        "nozzle_diameter": [0.4],
        "extruder_type": ["Direct Drive"],
        "nozzle_volume_type": ["Standard"],
        "default_nozzle_volume_type": ["Standard"],
        "filament_self_index": [1],
        "filament_extruder_variant": ["Direct Drive Standard"],
        "gcode_flavor": 0,
        "use_relative_e_distances": true,
        "before_layer_change_gcode": "",
        "layer_change_gcode": ""
    })json", &load_issues);

    REQUIRE_FALSE(has_config_errors(load_issues));
    const std::vector<ConfigValidationIssue> issues = validate_resolved_config(config, 0);
    REQUIRE(has_config_errors(issues));
    REQUIRE(has_issue(issues, "invalid_config_cardinality", "extruder_type"));
    REQUIRE(has_issue(issues, "invalid_config_cardinality", "nozzle_volume_type"));
}

TEST_CASE("Resolved config validator catches variant lookup failures", "[ConfigSDK]")
{
    std::vector<ConfigValidationIssue> load_issues;
    ResolvedConfig config = ResolvedConfig::from_json(R"json({
        "printer_model": "Bambu Lab X1 Carbon",
        "printer_settings_id": "Bambu Lab X1 Carbon 0.4 nozzle",
        "print_settings_id": "0.20mm Standard @BBL X1C",
        "filament_settings_id": ["slot 1", "slot 2"],
        "filament_colour": ["#FFFFFF", "#000000"],
        "filament_diameter": [1.75, 1.75],
        "filament_map": [1, 2],
        "nozzle_diameter": [0.4, 0.4],
        "extruder_type": ["Direct Drive", "Direct Drive"],
        "nozzle_volume_type": ["Standard", "Standard"],
        "default_nozzle_volume_type": ["Standard", "Standard"],
        "filament_self_index": [1],
        "filament_extruder_variant": ["Direct Drive Standard"],
        "gcode_flavor": 0,
        "use_relative_e_distances": true,
        "before_layer_change_gcode": "",
        "layer_change_gcode": ""
    })json", &load_issues);

    REQUIRE_FALSE(has_config_errors(load_issues));
    const std::vector<ConfigValidationIssue> issues = validate_resolved_config(config, 0);
    REQUIRE(has_config_errors(issues));
    REQUIRE(has_issue(issues, "invalid_variant_lookup", "filament_extruder_variant"));
}

TEST_CASE("Preset resolver API returns an explicit unsupported issue until implemented", "[ConfigSDK]")
{
    const ConfigResolutionResult result = resolve_fff_config(ConfigResolutionRequest {});
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "preset_resolution_unsupported", ""));
}
