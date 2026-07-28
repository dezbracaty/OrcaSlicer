#include <catch2/catch_all.hpp>

#include "libslic3r/ConfigSDK_internal.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace Slic3r;
using namespace Slic3r::libslicer;
using json = nlohmann::json;

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

std::filesystem::path repo_root()
{
    return std::filesystem::path(TEST_DATA_DIR).parent_path().parent_path();
}

std::filesystem::path repo_resources_dir()
{
    return repo_root() / "resources";
}

std::filesystem::path make_temp_data_dir()
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("libslicer-config-sdk-test-" + std::to_string(suffix));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path write_project_preset_file(const std::filesystem::path& data_dir,
                                                const std::string& name,
                                                const std::string& content)
{
    const std::filesystem::path file = data_dir / name;
    std::ofstream out(file);
    out << content;
    return file;
}

std::filesystem::path write_user_process_preset(const std::filesystem::path& user_dir,
                                                const std::string& name,
                                                const std::string& inherits)
{
    PresetBundle bundle;
    DynamicPrintConfig config(bundle.prints.default_preset().config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    const std::filesystem::path file = user_dir / PRESET_PRINT_NAME / (name + ".json");
    std::filesystem::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
    return file;
}

std::string embedded_process_preset_json()
{
    return R"json({
        "type": "process",
        "name": "SDK Embedded Process",
        "version": "1.0.0",
        "from": "project",
        "inherits": "0.20mm Standard @BBL A1",
        "print_settings_id": "SDK Embedded Process",
        "layer_height": "0.23"
    })json";
}

std::filesystem::path make_3mf_with_embedded_process_preset(const std::filesystem::path& data_dir)
{
    const std::filesystem::path source =
        repo_resources_dir() / "calib" / "pressure_advance" / "auto_pa_line_single.3mf";
    const std::filesystem::path project_file = data_dir / "embedded-process-preset.3mf";
    std::filesystem::copy_file(source, project_file, std::filesystem::copy_options::overwrite_existing);

    const std::string preset_json = embedded_process_preset_json();
    const mz_bool added = mz_zip_add_mem_to_archive_file_in_place(
        project_file.string().c_str(),
        "Metadata/process_settings_1.config",
        preset_json.data(),
        preset_json.size(),
        nullptr,
        0,
        MZ_DEFAULT_LEVEL);
    REQUIRE(added != 0);
    return project_file;
}

ConfigResolutionRequest bbl_x1c_request(const std::filesystem::path& data_dir)
{
    ConfigResolutionRequest request;
    request.resources_dir = repo_resources_dir();
    request.data_dir = data_dir;
    request.vendor_bundle_dirs.push_back(request.resources_dir / "profiles" / "OrcaFilamentLibrary");
    request.vendor_bundle_dirs.push_back(request.resources_dir / "profiles" / "BBL");
    request.printer_preset_id = "Bambu Lab X1 Carbon 0.4 nozzle";
    request.process_preset_id = "0.20mm Standard @BBL X1C";
    request.filament_slots.push_back(FilamentSlotRequest {
        0,
        "Bambu PLA Basic @BBL X1C",
        "#FFFFFF",
        "",
        "PLA",
        ""
    });
    return request;
}

} // namespace

TEST_CASE("Config SDK exposes config definitions", "[ConfigSDK]")
{
    const std::vector<ConfigDefinition> definitions = get_config_definitions();
    REQUIRE_FALSE(definitions.empty());
    for (const ConfigDefinition& definition : definitions) {
        INFO(definition.key);
        REQUIRE(definition.scope != ConfigScope::Unknown);
        REQUIRE(definition.cardinality != ConfigCardinality::Unknown);
    }

    const ConfigDefinition printer_model = get_config_definition("printer_model");
    REQUIRE(printer_model.key == "printer_model");
    REQUIRE(printer_model.type == ConfigValueType::String);
    REQUIRE(printer_model.scope == ConfigScope::Printer);
    REQUIRE(json::parse(printer_model.default_json).is_string());
    REQUIRE_FALSE(printer_model.nullable);
    REQUIRE_FALSE(printer_model.internal);

    const ConfigDefinition filament_flow_ratio = get_config_definition("filament_flow_ratio");
    REQUIRE(filament_flow_ratio.nullable);
    REQUIRE(json::parse(filament_flow_ratio.default_json).is_array());

    const ConfigDefinition filament_map = get_config_definition("filament_map");
    REQUIRE(filament_map.scope == ConfigScope::Filament);
    REQUIRE(filament_map.cardinality == ConfigCardinality::Filament);
    REQUIRE(filament_map.internal);

    REQUIRE(get_config_definition("printer_settings_id").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("print_settings_id").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("filament_settings_id").scope == ConfigScope::Filament);
    REQUIRE(get_config_definition("filament_settings_id").cardinality == ConfigCardinality::Filament);
    REQUIRE(get_config_definition("filament_colour").scope == ConfigScope::Filament);
    REQUIRE(get_config_definition("filament_colour").cardinality == ConfigCardinality::Filament);
    REQUIRE(get_config_definition("filament_colour_type").scope == ConfigScope::Filament);
    REQUIRE(get_config_definition("filament_multi_colour").scope == ConfigScope::Filament);
    REQUIRE(get_config_definition("filament_map_mode").scope == ConfigScope::Project);

    REQUIRE(get_config_definition("nozzle_diameter").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("nozzle_diameter").cardinality == ConfigCardinality::PhysicalExtruder);
    REQUIRE(get_config_definition("extruder_offset").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("extruder_offset").cardinality == ConfigCardinality::PhysicalExtruder);
    REQUIRE(get_config_definition("min_layer_height").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("min_layer_height").cardinality == ConfigCardinality::PhysicalExtruder);
    REQUIRE(get_config_definition("max_layer_height").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("max_layer_height").cardinality == ConfigCardinality::PhysicalExtruder);
    REQUIRE(get_config_definition("brim_width").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("filament_diameter").scope == ConfigScope::Filament);
    REQUIRE(get_config_definition("filament_diameter").cardinality == ConfigCardinality::Filament);
    REQUIRE(get_config_definition("curr_bed_type").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("flush_multiplier").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("flush_volumes_matrix").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("flush_volumes_matrix").cardinality == ConfigCardinality::Matrix);
    REQUIRE(get_config_definition("flush_volumes_vector").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("flush_volumes_vector").cardinality == ConfigCardinality::Matrix);
    REQUIRE(get_config_definition("wipe_tower_x").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("wipe_tower_x").cardinality == ConfigCardinality::Plate);
    REQUIRE(get_config_definition("wipe_tower_y").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("wipe_tower_y").cardinality == ConfigCardinality::Plate);
    REQUIRE(get_config_definition("wipe_tower_rotation_angle").scope == ConfigScope::Project);
    REQUIRE(get_config_definition("support_filament").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("support_filament").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("support_interface_filament").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("support_interface_filament").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("wipe_tower_filament").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("wipe_tower_filament").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("extruder_type").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("extruder_type").cardinality == ConfigCardinality::PrinterVariantLookup);
    REQUIRE(get_config_definition("nozzle_volume_type").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("nozzle_volume_type").cardinality == ConfigCardinality::PrinterVariantLookup);
    REQUIRE(get_config_definition("default_nozzle_volume_type").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("default_nozzle_volume_type").cardinality == ConfigCardinality::PrinterVariantLookup);
    REQUIRE(get_config_definition("retraction_length").cardinality == ConfigCardinality::PrinterVariantLookup);
    REQUIRE(get_config_definition("nozzle_volume").cardinality == ConfigCardinality::PrinterVariantLookup);
    REQUIRE(get_config_definition("extruder_printable_height").cardinality == ConfigCardinality::PhysicalExtruder);
    REQUIRE(get_config_definition("filament_flow_ratio").cardinality == ConfigCardinality::FilamentExtruderVariant);
    REQUIRE(get_config_definition("filament_retract_lift_enforce").scope == ConfigScope::Filament);
    REQUIRE(get_config_definition("filament_retract_lift_enforce").cardinality == ConfigCardinality::FilamentExtruderVariant);
    REQUIRE(get_config_definition("filament_self_index").cardinality == ConfigCardinality::FilamentExtruderVariant);
    REQUIRE(get_config_definition("filament_extruder_variant").cardinality == ConfigCardinality::FilamentExtruderVariant);
    REQUIRE(get_config_definition("print_extruder_id").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("print_extruder_id").cardinality == ConfigCardinality::ProcessExtruderVariant);
    REQUIRE(get_config_definition("print_extruder_variant").scope == ConfigScope::Process);
    REQUIRE(get_config_definition("print_extruder_variant").cardinality == ConfigCardinality::ProcessExtruderVariant);
    REQUIRE(get_config_definition("filament_type").cardinality == ConfigCardinality::Filament);
    REQUIRE(get_config_definition("gcode_flavor").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("gcode_flavor").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("use_relative_e_distances").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("use_relative_e_distances").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("before_layer_change_gcode").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("before_layer_change_gcode").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("layer_change_gcode").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("layer_change_gcode").cardinality == ConfigCardinality::Scalar);
    REQUIRE(get_config_definition("printable_area").type == ConfigValueType::Vector);
    REQUIRE(get_config_definition("printable_area").scope == ConfigScope::Printer);
    REQUIRE(get_config_definition("printable_area").cardinality == ConfigCardinality::Scalar);
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

TEST_CASE("Config SDK parses strict JSON and emits canonical typed values", "[ConfigSDK]")
{
    const char* json_text = R"json({
        "printer_settings_id": "Printer A",
        "print_settings_id": "Process A",
        "brim_width": 5,
        "bed_temperature_formula": 0,
        "extruder_type": [0, 1],
        "bridge_line_width": "100%",
        "unknown_parse_key": 1
    })json";

    ConfigParseResult strict_result = parse_config_json(json_text);
    REQUIRE(has_config_errors(strict_result.issues));
    REQUIRE(has_issue(strict_result.issues, "unknown_config_key", "unknown_parse_key"));
    REQUIRE(strict_result.canonical_json.empty());

    ConfigParseOptions warning_options;
    warning_options.unknown_key_policy = UnknownKeyPolicy::Warning;
    ConfigParseResult warning_result = parse_config_json(json_text, warning_options);
    REQUIRE_FALSE(has_config_errors(warning_result.issues));
    REQUIRE(has_issue(warning_result.issues, "unknown_config_key", "unknown_parse_key"));

    const json canonical = json::parse(warning_result.canonical_json);
    REQUIRE(canonical.at("printer_settings_id") == "Printer A");
    REQUIRE(canonical.at("print_settings_id") == "Process A");
    REQUIRE(canonical.at("brim_width") == 5);
    REQUIRE(canonical.at("bed_temperature_formula") == "by_first_filament");
    REQUIRE(canonical.at("extruder_type") == json::array({"Direct Drive", "Bowden"}));
    REQUIRE(canonical.at("bridge_line_width") == "100%");
    REQUIRE_FALSE(canonical.contains("unknown_parse_key"));
}

TEST_CASE("Config SDK diffs configs and returns parseable canonical JSON", "[ConfigSDK]")
{
    ConfigDiffRequest request;
    request.base_config_json = R"json({
        "printer_settings_id": "Printer A",
        "print_settings_id": "Process A",
        "printer_model": "Model A",
        "brim_width": 5
    })json";
    request.target_config_json = R"json({
        "printer_settings_id": "Printer A",
        "print_settings_id": "Process A",
        "printer_model": "Model B",
        "brim_width": 7
    })json";

    const ConfigDiffResult diff = diff_config(request);
    REQUIRE_FALSE(has_config_errors(diff.issues));
    REQUIRE_FALSE(diff.diff_json.empty());

    const json diff_json = json::parse(diff.diff_json);
    REQUIRE(diff_json.at("printer_model") == "Model B");
    REQUIRE(diff_json.at("brim_width") == 7);

    const ConfigParseResult reparsed = parse_config_json(diff.diff_json);
    REQUIRE_FALSE(has_config_errors(reparsed.issues));
}

TEST_CASE("Resolved config validator accepts a complete BBL multi-filament config", "[ConfigSDK]")
{
    std::vector<ConfigValidationIssue> load_issues;
    ResolvedConfig config = ResolvedConfig::from_json(valid_bbl_two_filament_json(), &load_issues);

    REQUIRE_FALSE(has_config_errors(load_issues));
    const std::vector<ConfigValidationIssue> issues = validate_resolved_config(config, 0);
    REQUIRE_FALSE(has_config_errors(issues));
}

TEST_CASE("Resolved config validator reports unsupported print validation requests", "[ConfigSDK]")
{
    const std::vector<ConfigIssue> issues =
        validate_resolved_config(ConfigValidationRequest { valid_bbl_two_filament_json(), 0, true });

    REQUIRE(has_config_errors(issues));
    REQUIRE(has_issue(issues, "unsupported_config_feature", "run_print_validate"));
}

TEST_CASE("Resolved config validator checks process extruder variant metadata when present", "[ConfigSDK]")
{
    json valid = json::parse(valid_bbl_two_filament_json());
    valid["print_extruder_id"] = { 1, 2 };
    valid["print_extruder_variant"] = { "Direct Drive Standard", "Direct Drive Standard" };

    std::vector<ConfigValidationIssue> load_issues;
    ResolvedConfig config = ResolvedConfig::from_json(valid.dump(), &load_issues);
    REQUIRE_FALSE(has_config_errors(load_issues));
    REQUIRE_FALSE(has_config_errors(validate_resolved_config(config, 0)));

    json mismatched = valid;
    mismatched["print_extruder_variant"] = { "Direct Drive Standard" };
    config = ResolvedConfig::from_json(mismatched.dump(), &load_issues);
    REQUIRE_FALSE(has_config_errors(load_issues));
    std::vector<ConfigValidationIssue> issues = validate_resolved_config(config, 0);
    REQUIRE(has_config_errors(issues));
    REQUIRE(has_issue(issues, "invalid_config_cardinality", "print_extruder_id"));

    json missing_variant = valid;
    missing_variant.erase("print_extruder_variant");
    config = ResolvedConfig::from_json(missing_variant.dump(), &load_issues);
    REQUIRE_FALSE(has_config_errors(load_issues));
    issues = validate_resolved_config(config, 0);
    REQUIRE(has_config_errors(issues));
    REQUIRE(has_issue(issues, "invalid_config_cardinality", "print_extruder_id"));

    json invalid_id = valid;
    invalid_id["print_extruder_id"] = { 1, 0 };
    config = ResolvedConfig::from_json(invalid_id.dump(), &load_issues);
    REQUIRE_FALSE(has_config_errors(load_issues));
    issues = validate_resolved_config(config, 0);
    REQUIRE(has_config_errors(issues));
    REQUIRE(has_issue(issues, "invalid_config_value", "print_extruder_id"));
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

TEST_CASE("Preset resolver API reports missing required fields without throwing", "[ConfigSDK]")
{
    const ConfigResolutionResult result = resolve_fff_config(ConfigResolutionRequest {});
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "missing_required_config", "data_dir"));
    REQUIRE(has_issue(result.issues, "missing_required_config", "resources_dir"));
    REQUIRE(has_issue(result.issues, "missing_required_config", "printer_preset_id"));
    REQUIRE(has_issue(result.issues, "missing_required_config", "process_preset_id"));
    REQUIRE(has_issue(result.issues, "missing_required_config", "filament_slots"));
}

TEST_CASE("Preset catalog enumerates bundled FFF presets with active compatibility", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    const ConfigResolutionRequest resolution_request = bbl_x1c_request(data_dir);

    PresetCatalogRequest request;
    request.resources_dir = resolution_request.resources_dir;
    request.data_dir = resolution_request.data_dir;
    request.vendor_bundle_dirs = resolution_request.vendor_bundle_dirs;
    request.printer_preset_id = resolution_request.printer_preset_id;
    request.process_preset_id = resolution_request.process_preset_id;

    const PresetCatalogResult catalog = load_preset_catalog(request);
    INFO((catalog.issues.empty() ? "" : catalog.issues.front().message));
    REQUIRE_FALSE(has_config_errors(catalog.issues));
    REQUIRE_FALSE(catalog.printers.empty());
    REQUIRE_FALSE(catalog.processes.empty());
    REQUIRE_FALSE(catalog.filaments.empty());

    const auto printer_it = std::find_if(catalog.printers.begin(), catalog.printers.end(), [&](const PresetCatalogEntry& entry) {
        return entry.preset_id == resolution_request.printer_preset_id;
    });
    REQUIRE(printer_it != catalog.printers.end());
    REQUIRE(printer_it->kind == PresetKind::Printer);
    REQUIRE(printer_it->is_system);
    REQUIRE(printer_it->vendor_id == "BBL");

    const auto process_it = std::find_if(catalog.processes.begin(), catalog.processes.end(), [&](const PresetCatalogEntry& entry) {
        return entry.preset_id == resolution_request.process_preset_id;
    });
    REQUIRE(process_it != catalog.processes.end());
    REQUIRE(process_it->kind == PresetKind::Process);
    REQUIRE(process_it->is_compatible);

    const auto filament_it = std::find_if(catalog.filaments.begin(), catalog.filaments.end(), [&](const PresetCatalogEntry& entry) {
        return entry.preset_id == resolution_request.filament_slots.front().filament_preset_id;
    });
    REQUIRE(filament_it != catalog.filaments.end());
    REQUIRE(filament_it->kind == PresetKind::Filament);
    REQUIRE(filament_it->is_compatible);

    const auto incompatible_process_it = std::find_if(catalog.processes.begin(), catalog.processes.end(), [](const PresetCatalogEntry& entry) {
        return entry.preset_id == "0.48mm Draft @BBL A1M 0.8 nozzle";
    });
    REQUIRE(incompatible_process_it != catalog.processes.end());
    REQUIRE_FALSE(incompatible_process_it->is_compatible);

    const auto incompatible_filament_it = std::find_if(catalog.filaments.begin(), catalog.filaments.end(), [](const PresetCatalogEntry& entry) {
        return entry.preset_id == "Bambu ASA-CF @BBL A1";
    });
    REQUIRE(incompatible_filament_it != catalog.filaments.end());
    REQUIRE_FALSE(incompatible_filament_it->is_compatible);

    request.compatible_only = true;
    const PresetCatalogResult compatible_catalog = load_preset_catalog(request);
    REQUIRE_FALSE(has_config_errors(compatible_catalog.issues));
    REQUIRE_FALSE(compatible_catalog.processes.empty());
    REQUIRE_FALSE(compatible_catalog.filaments.empty());
    REQUIRE(std::all_of(compatible_catalog.processes.begin(), compatible_catalog.processes.end(),
        [](const PresetCatalogEntry& entry) { return entry.is_compatible; }));
    REQUIRE(std::all_of(compatible_catalog.filaments.begin(), compatible_catalog.filaments.end(),
        [](const PresetCatalogEntry& entry) { return entry.is_compatible; }));
    REQUIRE(std::none_of(compatible_catalog.processes.begin(), compatible_catalog.processes.end(), [](const PresetCatalogEntry& entry) {
        return entry.preset_id == "0.48mm Draft @BBL A1M 0.8 nozzle";
    }));
    REQUIRE(std::none_of(compatible_catalog.filaments.begin(), compatible_catalog.filaments.end(), [](const PresetCatalogEntry& entry) {
        return entry.preset_id == "Bambu ASA-CF @BBL A1";
    }));

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset catalog reports missing active preset selections without discarding catalog", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    const ConfigResolutionRequest resolution_request = bbl_x1c_request(data_dir);

    PresetCatalogRequest request;
    request.resources_dir = resolution_request.resources_dir;
    request.data_dir = resolution_request.data_dir;
    request.vendor_bundle_dirs = resolution_request.vendor_bundle_dirs;
    request.printer_preset_id = "not a real printer preset";

    const PresetCatalogResult catalog = load_preset_catalog(request);
    REQUIRE(has_config_errors(catalog.issues));
    REQUIRE(has_issue(catalog.issues, "preset_not_found", "printer_preset_id"));
    REQUIRE_FALSE(catalog.printers.empty());
    REQUIRE_FALSE(catalog.processes.empty());
    REQUIRE_FALSE(catalog.filaments.empty());

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Project 3MF extraction reads project config and selections", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    Project3mfExtractionRequest request;
    request.project_file = repo_resources_dir() / "calib" / "pressure_advance" / "auto_pa_line_single.3mf";
    request.data_dir = data_dir;

    const Project3mfExtractionResult result = extract_project_3mf_config(request);
    INFO((result.issues.empty() ? "" : result.issues.front().message));
    REQUIRE_FALSE(has_config_errors(result.issues));
    REQUIRE_FALSE(result.project_config_json.empty());
    REQUIRE(result.printer_preset_id == "Bambu Lab A1 0.4 nozzle");
    REQUIRE(result.process_preset_id == "0.20mm Standard @BBL A1");
    REQUIRE_FALSE(result.filament_slots.empty());
    REQUIRE(result.filament_slots.front().filament_preset_id == "Bambu PLA Basic @BBL A1");
    REQUIRE(result.filament_slots.front().slot_index == 0);

    const json project_config = json::parse(result.project_config_json);
    REQUIRE(project_config.at("printer_model") == "Bambu Lab A1");
    REQUIRE(project_config.at("brim_width") == 5);
    REQUIRE(project_config.at("filament_settings_id").is_array());

    const json printer_overrides = json::parse(result.printer_overrides_json);
    REQUIRE(printer_overrides.at("printer_model") == "Bambu Lab A1");
    REQUIRE_FALSE(printer_overrides.contains("brim_width"));

    const json process_overrides = json::parse(result.process_overrides_json);
    REQUIRE(process_overrides.at("brim_width") == 5);
    REQUIRE_FALSE(process_overrides.contains("printer_model"));

    const json project_overrides = json::parse(result.project_overrides_json);
    REQUIRE(project_overrides.contains("flush_volumes_matrix"));
    REQUIRE_FALSE(project_overrides.contains("printer_model"));
    REQUIRE_FALSE(project_overrides.contains("brim_width"));

    const json object_overrides = json::parse(result.object_overrides_json);
    REQUIRE_FALSE(object_overrides.empty());
    REQUIRE(object_overrides.front().at("config").at("extruder") == 1);

    const json part_overrides = json::parse(result.part_overrides_json);
    REQUIRE_FALSE(part_overrides.empty());
    REQUIRE(std::any_of(part_overrides.begin(), part_overrides.end(), [](const json& part) {
        return part.at("config").contains("top_surface_speed") &&
               part.at("config").at("top_surface_speed") == 80;
    }));

    REQUIRE_FALSE(result.plates.empty());
    REQUIRE(result.plates.front().plate_index == 0);

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Project 3MF extraction covers additional bundled calibration projects", "[ConfigSDK]")
{
    struct FixtureCase {
        std::filesystem::path relative_path;
        std::string printer_preset_id;
        std::string process_preset_id;
        std::string printer_model;
        std::size_t filament_slot_count;
        std::size_t flush_matrix_count;
    };

    const std::vector<FixtureCase> fixtures {
        {
            std::filesystem::path("calib") / "pressure_advance" / "auto_pa_line_dual.3mf",
            "Bambu Lab H2D 0.4 nozzle",
            "0.20mm Standard @BBL H2D",
            "Bambu Lab H2D",
            8,
            128
        },
        {
            std::filesystem::path("calib") / "pressure_advance" / "pa_pattern.3mf",
            "Bambu Lab N1 0.4 nozzle",
            "0.20mm Standard @BBL N1",
            "Bambu Lab N1",
            1,
            1
        }
    };

    for (const FixtureCase& fixture : fixtures) {
        const std::filesystem::path data_dir = make_temp_data_dir();
        Project3mfExtractionRequest request;
        request.project_file = repo_resources_dir() / fixture.relative_path;
        request.data_dir = data_dir;

        const Project3mfExtractionResult result = extract_project_3mf_config(request);
        INFO(fixture.relative_path.string());
        INFO((result.issues.empty() ? "" : result.issues.front().message));
        REQUIRE_FALSE(has_config_errors(result.issues));
        REQUIRE(result.printer_preset_id == fixture.printer_preset_id);
        REQUIRE(result.process_preset_id == fixture.process_preset_id);
        REQUIRE(result.filament_slots.size() == fixture.filament_slot_count);
        REQUIRE_FALSE(result.project_config_json.empty());
        REQUIRE_FALSE(result.printer_overrides_json.empty());
        REQUIRE_FALSE(result.process_overrides_json.empty());
        REQUIRE_FALSE(result.project_overrides_json.empty());
        REQUIRE_FALSE(result.object_overrides_json.empty());
        REQUIRE_FALSE(result.part_overrides_json.empty());
        REQUIRE_FALSE(result.plates.empty());
        REQUIRE(result.plates.front().plate_index == 0);
        REQUIRE_FALSE(result.file_version.empty());
        REQUIRE((result.is_bbl_3mf || result.is_orca_3mf));

        const json project_config = json::parse(result.project_config_json);
        REQUIRE(project_config.at("printer_model") == fixture.printer_model);
        REQUIRE(project_config.at("filament_settings_id").is_array());
        REQUIRE(project_config.at("filament_settings_id").size() == fixture.filament_slot_count);
        REQUIRE(project_config.at("flush_volumes_matrix").is_array());
        REQUIRE(project_config.at("flush_volumes_matrix").size() == fixture.flush_matrix_count);

        const json printer_overrides = json::parse(result.printer_overrides_json);
        REQUIRE(printer_overrides.at("printer_model") == fixture.printer_model);
        const json process_overrides = json::parse(result.process_overrides_json);
        REQUIRE(process_overrides.contains("brim_width"));
        const json project_overrides = json::parse(result.project_overrides_json);
        REQUIRE(project_overrides.contains("flush_volumes_matrix"));
        REQUIRE_FALSE(project_overrides.contains("printer_model"));
        const json object_overrides = json::parse(result.object_overrides_json);
        REQUIRE(object_overrides.is_array());
        const json part_overrides = json::parse(result.part_overrides_json);
        REQUIRE(part_overrides.is_array());

        std::filesystem::remove_all(data_dir);
    }
}

TEST_CASE("Project 3MF extraction materializes embedded presets for resolver reuse", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    Project3mfExtractionRequest request;
    request.project_file = make_3mf_with_embedded_process_preset(data_dir);
    request.data_dir = data_dir;

    const Project3mfExtractionResult result = extract_project_3mf_config(request);
    INFO((result.issues.empty() ? "" : result.issues.front().message));
    REQUIRE_FALSE(has_config_errors(result.issues));
    REQUIRE(result.embedded_presets.size() == 1);
    REQUIRE(result.embedded_presets.front().name == "SDK Embedded Process");
    REQUIRE(result.embedded_presets.front().kind == PresetKind::Process);
    REQUIRE(result.extracted_project_preset_files.size() == 1);
    REQUIRE(std::filesystem::exists(result.extracted_project_preset_files.front()));

    std::ifstream embedded_file(result.extracted_project_preset_files.front());
    json embedded_json;
    embedded_file >> embedded_json;
    REQUIRE(embedded_json.at("type") == "process");
    REQUIRE(embedded_json.at("name") == "SDK Embedded Process");
    REQUIRE(embedded_json.at("print_settings_id") == "SDK Embedded Process");

    PresetCatalogRequest catalog_request;
    catalog_request.resources_dir = repo_resources_dir();
    catalog_request.data_dir = data_dir;
    catalog_request.vendor_bundle_dirs.push_back(catalog_request.resources_dir / "profiles" / "OrcaFilamentLibrary");
    catalog_request.vendor_bundle_dirs.push_back(catalog_request.resources_dir / "profiles" / "BBL");
    catalog_request.project_preset_files = result.extracted_project_preset_files;

    const PresetCatalogResult catalog = load_preset_catalog(catalog_request);
    INFO((catalog.issues.empty() ? "" : catalog.issues.front().message));
    REQUIRE_FALSE(has_config_errors(catalog.issues));
    REQUIRE(std::any_of(catalog.processes.begin(), catalog.processes.end(), [](const PresetCatalogEntry& entry) {
        return entry.name == "SDK Embedded Process";
    }));

    ConfigResolutionRequest resolution_request = bbl_x1c_request(data_dir);
    resolution_request.project_preset_files = result.extracted_project_preset_files;
    resolution_request.process_preset_id = "SDK Embedded Process";

    const ConfigResolutionResult resolved = resolve_fff_config(resolution_request);
    INFO((resolved.issues.empty() ? "" : resolved.issues.front().message));
    REQUIRE_FALSE(has_config_errors(resolved.issues));
    const json full_config = json::parse(resolved.full_config_json);
    REQUIRE(full_config.at("print_settings_id") == "SDK Embedded Process");
    REQUIRE(full_config.at("layer_height") == 0.23);

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Firehorse project extraction resolves and validates its complete BBL configuration", "[ConfigSDK][firehorse]")
{
    const auto issue_summary = [](const std::vector<ConfigIssue>& issues) {
        std::string summary;
        for (const ConfigIssue& issue : issues) {
            if (!summary.empty())
                summary += '\n';
            summary += issue.code + " " + issue.field + ": " + issue.message;
        }
        return summary;
    };

    const std::filesystem::path data_dir = make_temp_data_dir();
    Project3mfExtractionRequest extraction_request;
    extraction_request.project_file = std::filesystem::path(TEST_DATA_DIR) / "test_3mf" / "firehorse.3mf";
    extraction_request.data_dir = data_dir;
    extraction_request.plate_index = 0;
    extraction_request.strict = true;

    const Project3mfExtractionResult extracted = extract_project_3mf_config(extraction_request);
    INFO(issue_summary(extracted.issues));
    REQUIRE_FALSE(has_config_errors(extracted.issues));
    REQUIRE(extracted.printer_preset_id == "Bambu Lab X1 Carbon 0.4 nozzle");
    REQUIRE(extracted.process_preset_id == "0.20mm Standard @BBL X1C");
    REQUIRE(extracted.filament_slots.size() == 2);
    REQUIRE_FALSE(extracted.project_config_json.empty());
    const json printer_overrides = json::parse(extracted.printer_overrides_json);
    REQUIRE(printer_overrides.at("bed_temperature_formula") == "by_first_filament");

    ConfigResolutionRequest resolution_request;
    resolution_request.resources_dir = repo_resources_dir();
    resolution_request.data_dir = data_dir;
    resolution_request.vendor_bundle_dirs.push_back(
        resolution_request.resources_dir / "profiles" / "OrcaFilamentLibrary");
    resolution_request.vendor_bundle_dirs.push_back(
        resolution_request.resources_dir / "profiles" / "BBL");
    resolution_request.project_preset_files = extracted.extracted_project_preset_files;
    resolution_request.printer_preset_id = extracted.printer_preset_id;
    resolution_request.process_preset_id = extracted.process_preset_id;
    resolution_request.filament_slots = extracted.filament_slots;
    resolution_request.printer_overrides_json = extracted.printer_overrides_json;
    resolution_request.process_overrides_json = extracted.process_overrides_json;
    resolution_request.project_overrides_json = extracted.project_overrides_json;
    resolution_request.plate_index = extraction_request.plate_index;
    resolution_request.strict = true;

    const ConfigResolutionResult resolved = resolve_fff_config(resolution_request);
    INFO(issue_summary(resolved.issues));
    REQUIRE_FALSE(has_config_errors(resolved.issues));
    REQUIRE_FALSE(resolved.full_config_json.empty());

    const json full_config = json::parse(resolved.full_config_json);
    REQUIRE(full_config.at("printer_model") == "Bambu Lab X1 Carbon");
    REQUIRE(full_config.at("printer_settings_id") == extracted.printer_preset_id);
    REQUIRE(full_config.at("print_settings_id") == extracted.process_preset_id);
    REQUIRE(full_config.at("bed_temperature_formula") == "by_first_filament");
    REQUIRE(full_config.at("bridge_line_width") == "100%");
    REQUIRE(full_config.at("filament_settings_id").is_array());
    REQUIRE(full_config.at("filament_settings_id").size() == extracted.filament_slots.size());
    REQUIRE(full_config.at("filament_colour").is_array());
    REQUIRE(full_config.at("filament_colour").size() == extracted.filament_slots.size());
    REQUIRE(full_config.at("filament_map").is_array());
    REQUIRE(full_config.at("filament_map").size() == extracted.filament_slots.size());
    for (std::size_t index = 0; index < extracted.filament_slots.size(); ++index) {
        INFO("filament slot " << index);
        REQUIRE(full_config.at("filament_settings_id").at(index) ==
                extracted.filament_slots.at(index).filament_preset_id);
    }

    const std::vector<ConfigIssue> validation_issues = validate_resolved_config(
        ConfigValidationRequest { resolved.full_config_json, extraction_request.plate_index, false });
    INFO(issue_summary(validation_issues));
    REQUIRE_FALSE(has_config_errors(validation_issues));

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Project 3MF extraction reports missing required request fields", "[ConfigSDK]")
{
    const Project3mfExtractionResult result = extract_project_3mf_config(Project3mfExtractionRequest {});
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "missing_required_config", "project_file"));
    REQUIRE(has_issue(result.issues, "missing_required_config", "data_dir"));
}

TEST_CASE("Project 3MF extraction reports loadable non-project 3MF files explicitly", "[ConfigSDK]")
{
    struct FixtureCase {
        std::filesystem::path path;
        bool expect_empty_project_config;
    };

    const std::vector<FixtureCase> fixtures {
        { repo_resources_dir() / "handy_models" / "OrcaSliced.3mf", false },
        { repo_resources_dir() / "calib" / "filament_flow" / "Orca-LinearFlow.3mf", true },
        { repo_resources_dir() / "calib" / "filament_flow" / "Orca-LinearFlow_fine.3mf", true },
        { repo_resources_dir() / "calib" / "filament_flow" / "flowrate-test-pass1.3mf", true },
        { repo_resources_dir() / "calib" / "filament_flow" / "flowrate-test-pass2.3mf", true },
        { repo_resources_dir() / "calib" / "filament_flow" / "pass1.3mf", true },
        { std::filesystem::path(TEST_DATA_DIR) / "test_3mf" / "Geräte" / u8"Büchse.3mf", true }
    };

    for (const FixtureCase& fixture : fixtures) {
        const std::filesystem::path data_dir = make_temp_data_dir();
        Project3mfExtractionRequest request;
        request.project_file = fixture.path;
        request.data_dir = data_dir;

        const Project3mfExtractionResult result = extract_project_3mf_config(request);
        INFO(fixture.path.string());
        INFO((result.issues.empty() ? "" : result.issues.front().message));
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "missing_project_3mf_config", "printer_settings_id"));
        REQUIRE(has_issue(result.issues, "missing_project_3mf_config", "print_settings_id"));
        REQUIRE(has_issue(result.issues, "missing_project_3mf_config", "filament_settings_id"));
        REQUIRE(result.printer_preset_id.empty());
        REQUIRE(result.process_preset_id.empty());
        REQUIRE(result.filament_slots.empty());

        if (fixture.expect_empty_project_config) {
            REQUIRE(json::parse(result.project_config_json).empty());
            REQUIRE(json::parse(result.printer_overrides_json).empty());
            REQUIRE(json::parse(result.process_overrides_json).empty());
            REQUIRE(json::parse(result.project_overrides_json).empty());
        }

        std::filesystem::remove_all(data_dir);
    }
}

TEST_CASE("Preset resolver builds a full BBL config from bundled resources", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);

    const ConfigResolutionResult result = resolve_fff_config(request);
    INFO((result.issues.empty() ? "" : result.issues.front().message));
    REQUIRE_FALSE(has_config_errors(result.issues));
    REQUIRE_FALSE(result.full_config_json.empty());

    const json full_config = json::parse(result.full_config_json);
    REQUIRE(full_config.at("printer_model") == "Bambu Lab X1 Carbon");
    REQUIRE(full_config.at("printer_settings_id") == "Bambu Lab X1 Carbon 0.4 nozzle");
    REQUIRE(full_config.at("print_settings_id") == "0.20mm Standard @BBL X1C");
    const json& filament_settings_id = full_config.at("filament_settings_id");
    if (filament_settings_id.is_array())
        REQUIRE(filament_settings_id.front() == "Bambu PLA Basic @BBL X1C");
    else
        REQUIRE(filament_settings_id == "Bambu PLA Basic @BBL X1C");

    const std::vector<ConfigIssue> validation_issues =
        validate_resolved_config(ConfigValidationRequest { result.full_config_json, 0, false });
    REQUIRE_FALSE(has_config_errors(validation_issues));

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver builds a BBL multi-filament config from bundled resources", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);
    request.filament_slots.push_back(FilamentSlotRequest {
        1,
        "Bambu PLA Basic @BBL X1C",
        "#000000",
        "",
        "PLA",
        ""
    });

    const ConfigResolutionResult result = resolve_fff_config(request);
    INFO((result.issues.empty() ? "" : result.issues.front().message));
    REQUIRE_FALSE(has_config_errors(result.issues));
    REQUIRE_FALSE(result.full_config_json.empty());

    const json full_config = json::parse(result.full_config_json);
    REQUIRE(full_config.at("filament_settings_id").is_array());
    REQUIRE(full_config.at("filament_settings_id").size() == 2);
    REQUIRE(full_config.at("filament_settings_id").front() == "Bambu PLA Basic @BBL X1C");
    REQUIRE(full_config.at("filament_settings_id").back() == "Bambu PLA Basic @BBL X1C");
    REQUIRE(full_config.at("filament_map").is_array());
    REQUIRE(full_config.at("filament_map").size() == 2);
    REQUIRE(std::all_of(full_config.at("filament_map").begin(), full_config.at("filament_map").end(), [](const json& value) {
        return value.is_number_integer() && value.get<int>() > 0;
    }));

    const std::vector<ConfigIssue> validation_issues =
        validate_resolved_config(ConfigValidationRequest { result.full_config_json, 0, false });
    REQUIRE_FALSE(has_config_errors(validation_issues));

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver reflects strict project overrides in normalized diff", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);
    request.project_overrides_json = R"json({
        "brim_width": 7
    })json";

    const ConfigResolutionResult result = resolve_fff_config(request);
    INFO((result.issues.empty() ? "" : result.issues.front().message));
    REQUIRE_FALSE(has_config_errors(result.issues));

    const json full_config = json::parse(result.full_config_json);
    const json diff = json::parse(result.normalized_diff_json);
    REQUIRE(full_config.at("brim_width") == 7);
    REQUIRE(diff.at("brim_width") == 7);

    request.project_overrides_json = R"json({
        "unknown_project_key": 1
    })json";
    const ConfigResolutionResult strict_result = resolve_fff_config(request);
    REQUIRE(has_config_errors(strict_result.issues));
    REQUIRE(has_issue(strict_result.issues, "unknown_config_key", "unknown_project_key"));

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver rejects missing preset selections", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);
    request.process_preset_id = "not a real process preset";

    const ConfigResolutionResult result = resolve_fff_config(request);
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "preset_not_found", "process_preset_id"));
    REQUIRE(result.full_config_json.empty());

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver reports invalid preset search paths explicitly", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();

    SECTION("vendor bundle directory")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.vendor_bundle_dirs = { data_dir / "missing-vendor" };

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "preset_search_path_invalid", "vendor_bundle_dirs[0]"));
        REQUIRE(result.full_config_json.empty());
    }

    SECTION("user preset directory")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.user_preset_dirs.push_back(data_dir / "missing-user-presets");

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "preset_search_path_invalid", "user_preset_dirs[0]"));
        REQUIRE(result.full_config_json.empty());
    }

    SECTION("project preset file")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.project_preset_files.push_back(data_dir / "missing-project-preset.config");

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "preset_search_path_invalid", "project_preset_files[0]"));
        REQUIRE(result.full_config_json.empty());
    }

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver reports invalid project preset files explicitly", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();

    SECTION("malformed JSON")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.project_preset_files.push_back(write_project_preset_file(data_dir, "bad-json.config", "{"));

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "project_preset_invalid", "project_preset_files[0]"));
        REQUIRE(result.full_config_json.empty());
    }

    SECTION("unknown preset type")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.project_preset_files.push_back(write_project_preset_file(data_dir, "bad-type.config", R"json({
            "type": "not-a-preset",
            "name": "Bad Preset"
        })json"));

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "project_preset_invalid", "project_preset_files[0]"));
        REQUIRE(result.full_config_json.empty());
    }

    SECTION("unknown inherits target")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.project_preset_files.push_back(write_project_preset_file(data_dir, "bad-inherits.config", R"json({
            "type": "process",
            "name": "Bad Embedded Process",
            "inherits": "not a real process preset",
            "print_settings_id": "Bad Embedded Process"
        })json"));

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "project_preset_invalid", "project_preset_files[0]"));
        REQUIRE(result.full_config_json.empty());
    }

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver rejects ambiguous project preset names", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);
    request.process_preset_id = "SDK Duplicate Process";
    request.project_preset_files.push_back(write_project_preset_file(data_dir, "duplicate-a.config", R"json({
        "type": "process",
        "name": "SDK Duplicate Process",
        "inherits": "0.20mm Standard @BBL X1C",
        "print_settings_id": "SDK Duplicate Process",
        "layer_height": "0.21"
    })json"));
    request.project_preset_files.push_back(write_project_preset_file(data_dir, "duplicate-b.config", R"json({
        "type": "process",
        "name": "SDK Duplicate Process",
        "inherits": "0.20mm Standard @BBL X1C",
        "print_settings_id": "SDK Duplicate Process",
        "layer_height": "0.22"
    })json"));

    const ConfigResolutionResult result = resolve_fff_config(request);
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "preset_ambiguous", "process_preset_id"));
    REQUIRE(result.full_config_json.empty());

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver rejects ambiguous preset names across vendor bundles", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);
    request.vendor_bundle_dirs = {
        request.resources_dir / "profiles" / "OrcaFilamentLibrary",
        request.resources_dir / "profiles" / "OrcaFilamentLibrary",
    };
    request.filament_slots.front().filament_preset_id = "Generic PETG @System";

    const ConfigResolutionResult result = resolve_fff_config(request);
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "preset_ambiguous", "filament_slots[0].filament_preset_id"));
    REQUIRE(result.full_config_json.empty());

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver rejects ambiguous user preset names", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();
    const std::filesystem::path user_dir = data_dir / "user-presets";
    ConfigResolutionRequest request = bbl_x1c_request(data_dir);
    request.user_preset_dirs.push_back(user_dir);
    write_user_process_preset(user_dir, "0.20mm Standard @BBL X1C", "0.20mm Standard @BBL X1C");

    const ConfigResolutionResult result = resolve_fff_config(request);
    REQUIRE(has_config_errors(result.issues));
    REQUIRE(has_issue(result.issues, "preset_ambiguous", "process_preset_id"));
    REQUIRE(result.full_config_json.empty());

    std::filesystem::remove_all(data_dir);
}

TEST_CASE("Preset resolver rejects incompatible preset selections", "[ConfigSDK]")
{
    const std::filesystem::path data_dir = make_temp_data_dir();

    SECTION("process incompatible with printer")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.process_preset_id = "0.48mm Draft @BBL A1M 0.8 nozzle";

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "preset_incompatible", "process_preset_id"));
        REQUIRE(result.full_config_json.empty());
    }

    SECTION("filament incompatible with printer")
    {
        ConfigResolutionRequest request = bbl_x1c_request(data_dir);
        request.filament_slots.front().filament_preset_id = "Bambu ASA-CF @BBL A1";

        const ConfigResolutionResult result = resolve_fff_config(request);
        REQUIRE(has_config_errors(result.issues));
        REQUIRE(has_issue(result.issues, "preset_incompatible", "filament_slots[0].filament_preset_id"));
        REQUIRE(result.full_config_json.empty());
    }

    std::filesystem::remove_all(data_dir);
}
