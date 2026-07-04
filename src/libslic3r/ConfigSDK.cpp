#include "ConfigSDK.hpp"

#include "Config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace Slic3r::libslicer {
namespace {

using json = nlohmann::json;

ConfigValidationIssue make_issue(std::string code,
                                 std::string field,
                                 std::string message,
                                 ConfigIssueSeverity severity = ConfigIssueSeverity::Error)
{
    return { std::move(code), std::move(field), std::move(message), severity };
}

void add_issue(std::vector<ConfigValidationIssue>* issues,
               std::string code,
               std::string field,
               std::string message,
               ConfigIssueSeverity severity = ConfigIssueSeverity::Error)
{
    if (issues != nullptr)
        issues->push_back(make_issue(std::move(code), std::move(field), std::move(message), severity));
}

std::string json_scalar_to_config_string(const json& value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_boolean())
        return value.get<bool>() ? "1" : "0";
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        std::ostringstream out;
        out << value.get<double>();
        return out.str();
    }
    return value.dump();
}

std::string json_point_to_config_string(const json& value)
{
    if (value.is_array() && value.size() >= 2 && value[0].is_number() && value[1].is_number()) {
        std::ostringstream out;
        out << value[0].get<double>() << 'x' << value[1].get<double>();
        return out.str();
    }
    return json_scalar_to_config_string(value);
}

std::string json_array_to_config_string(const json& value, ConfigOptionType type)
{
    char separator = ',';
    if (type == coStrings)
        separator = ';';
    if (type == coPointsGroups)
        separator = '#';

    std::ostringstream out;
    bool first = true;
    for (const json& item : value) {
        if (!first)
            out << separator;
        first = false;

        if (type == coPoints || type == coPoint)
            out << json_point_to_config_string(item);
        else if (item.is_array())
            out << json_array_to_config_string(item, type);
        else if (type == coStrings)
            out << '"' << escape_string_cstyle(json_scalar_to_config_string(item)) << '"';
        else
            out << json_scalar_to_config_string(item);
    }
    return out.str();
}

bool config_enum_value_from_json(const json& value,
                                 const t_config_enum_values& enum_values,
                                 bool allow_nil,
                                 int& enum_value,
                                 std::string& error)
{
    if (value.is_number_integer()) {
        enum_value = value.get<int>();
        return true;
    }

    if (!value.is_string()) {
        error = "enum value must be a string or integer";
        return false;
    }

    const std::string text = value.get<std::string>();
    if (text == "nil" && allow_nil) {
        enum_value = ConfigOptionInts::nil_value();
        return true;
    }

    const auto it = enum_values.find(text);
    if (it == enum_values.end()) {
        error = "unknown enum value '" + text + "'";
        return false;
    }

    enum_value = it->second;
    return true;
}

bool apply_enum_config_value(const std::string& key,
                             const json& value_json,
                             const ConfigOptionDef& option_def,
                             DynamicPrintConfig& config,
                             std::vector<ConfigValidationIssue>* issues)
{
    if (option_def.enum_keys_map == nullptr) {
        add_issue(issues, "invalid_config_schema", key, "Config option has no enum value map");
        return false;
    }

    if (option_def.type == coEnum) {
        const json& scalar = value_json.is_array() && !value_json.empty() ? value_json.front() : value_json;
        int enum_value = 0;
        std::string error;
        if (!config_enum_value_from_json(scalar, *option_def.enum_keys_map, false, enum_value, error)) {
            add_issue(issues, "invalid_config_value", key, error);
            return false;
        }

        ConfigOption* option = config.option(key, true);
        if (auto* enum_option = dynamic_cast<ConfigOptionEnumGeneric*>(option); enum_option != nullptr)
            enum_option->keys_map = option_def.enum_keys_map;
        option->setInt(enum_value);
        return true;
    }

    if (!value_json.is_array()) {
        add_issue(issues, "invalid_config_value", key, "Enum vector value must be an array");
        return false;
    }

    ConfigOption* option = config.option(key, true);
    if (auto* enum_option = dynamic_cast<ConfigOptionEnumsGeneric*>(option); enum_option != nullptr)
        enum_option->keys_map = option_def.enum_keys_map;
    if (auto* enum_nullable_option = dynamic_cast<ConfigOptionEnumsGenericNullable*>(option); enum_nullable_option != nullptr)
        enum_nullable_option->keys_map = option_def.enum_keys_map;

    std::vector<int> enum_values;
    enum_values.reserve(value_json.size());
    for (const json& item : value_json) {
        int enum_value = 0;
        std::string error;
        if (!config_enum_value_from_json(item, *option_def.enum_keys_map, option->nullable(), enum_value, error)) {
            add_issue(issues, "invalid_config_value", key, error);
            return false;
        }
        enum_values.push_back(enum_value);
    }

    auto* int_values = dynamic_cast<ConfigOptionInts*>(option);
    if (int_values == nullptr) {
        add_issue(issues, "invalid_config_value", key, "Enum option storage is not an integer vector");
        return false;
    }
    int_values->values = std::move(enum_values);
    return true;
}

bool apply_json_object_to_config(const json& object,
                                 DynamicPrintConfig& config,
                                 std::vector<ConfigValidationIssue>* issues)
{
    if (!object.is_object()) {
        add_issue(issues, "invalid_config_json", "", "Config JSON must be an object");
        return false;
    }

    const ConfigDef* config_def = config.def();
    if (config_def == nullptr) {
        add_issue(issues, "invalid_config_schema", "", "DynamicPrintConfig has no config definition");
        return false;
    }

    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
    bool ok = true;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const std::string key = it.key();
        const ConfigOptionDef* option_def = config_def->get(key);
        if (option_def == nullptr) {
            add_issue(issues, "unknown_config_key", key, "Unknown config key: " + key, ConfigIssueSeverity::Warning);
            continue;
        }

        if (option_def->type == coEnum || option_def->type == coEnums) {
            ok = apply_enum_config_value(key, it.value(), *option_def, config, issues) && ok;
            continue;
        }

        std::string value;
        if (it.value().is_array())
            value = json_array_to_config_string(it.value(), option_def->type);
        else
            value = json_scalar_to_config_string(it.value());

        try {
            config.set_deserialize(key, value, substitutions);
        } catch (const std::exception& e) {
            add_issue(issues, "invalid_config_value", key, e.what());
            ok = false;
        }
    }

    return ok;
}

json config_to_json(const DynamicPrintConfig& config)
{
    json out = json::object();
    for (const std::string& key : config.keys()) {
        const ConfigOption* option = config.option(key);
        if (option != nullptr)
            out[key] = option->serialize();
    }
    return out;
}

ConfigValueType value_type_from_option(ConfigOptionType type)
{
    switch (type) {
    case coBool:
    case coBools:
        return type == coBool ? ConfigValueType::Bool : ConfigValueType::Vector;
    case coInt:
    case coInts:
    case coIntsGroups:
        return type == coInt ? ConfigValueType::Int : ConfigValueType::Vector;
    case coFloat:
    case coFloats:
    case coFloatOrPercent:
    case coFloatsOrPercents:
        return (type == coFloat || type == coFloatOrPercent) ? ConfigValueType::Float : ConfigValueType::Vector;
    case coPercent:
    case coPercents:
        return type == coPercent ? ConfigValueType::Percent : ConfigValueType::Vector;
    case coString:
    case coStrings:
        return type == coString ? ConfigValueType::String : ConfigValueType::Vector;
    case coEnum:
    case coEnums:
        return type == coEnum ? ConfigValueType::Enum : ConfigValueType::Vector;
    case coPoint:
    case coPoint3:
    case coPoints:
    case coPointsGroups:
        return (type == coPoint || type == coPoint3) ? ConfigValueType::Point : ConfigValueType::Vector;
    default:
        return ConfigValueType::Unknown;
    }
}

ConfigScope scope_for_key(const std::string& key)
{
    static const std::set<std::string> printer_keys {
        "printer_model", "printer_settings_id", "nozzle_diameter", "extruder_offset",
        "extruder_type", "nozzle_volume_type", "default_nozzle_volume_type",
        "min_layer_height", "max_layer_height", "bed_shape", "gcode_flavor"
    };
    static const std::set<std::string> process_keys {
        "print_settings_id", "layer_height", "first_layer_height", "support_filament",
        "support_interface_filament", "wipe_tower_filament", "wipe_tower_x", "wipe_tower_y",
        "before_layer_change_gcode", "layer_change_gcode", "use_relative_e_distances"
    };
    static const std::set<std::string> filament_keys {
        "filament_settings_id", "filament_colour", "filament_diameter", "filament_map",
        "filament_self_index", "filament_extruder_variant", "filament_type"
    };

    if (printer_keys.count(key) != 0)
        return ConfigScope::Printer;
    if (process_keys.count(key) != 0)
        return ConfigScope::Process;
    if (filament_keys.count(key) != 0 || key.rfind("filament_", 0) == 0)
        return ConfigScope::Filament;
    if (key.find("object") != std::string::npos)
        return ConfigScope::Object;
    return ConfigScope::Project;
}

ConfigCardinality cardinality_for_key(const std::string& key, const ConfigOptionDef& def)
{
    static const std::set<std::string> filament_keys {
        "filament_settings_id", "filament_colour", "filament_diameter", "filament_type"
    };
    static const std::set<std::string> physical_extruder_keys {
        "nozzle_diameter", "extruder_offset", "min_layer_height", "max_layer_height"
    };
    static const std::set<std::string> variant_lookup_keys {
        "extruder_type", "nozzle_volume_type", "default_nozzle_volume_type"
    };
    static const std::set<std::string> plate_keys {
        "wipe_tower_x", "wipe_tower_y"
    };
    static const std::set<std::string> matrix_keys {
        "flush_volumes_matrix", "flush_volumes_vector"
    };

    if (key == "filament_map")
        return ConfigCardinality::Filament;
    if (key == "filament_self_index" || key == "filament_extruder_variant")
        return ConfigCardinality::FilamentExtruderVariant;
    if (filament_keys.count(key) != 0)
        return ConfigCardinality::Filament;
    if (physical_extruder_keys.count(key) != 0)
        return ConfigCardinality::PhysicalExtruder;
    if (variant_lookup_keys.count(key) != 0)
        return ConfigCardinality::PrinterVariantLookup;
    if (plate_keys.count(key) != 0)
        return ConfigCardinality::Plate;
    if (matrix_keys.count(key) != 0)
        return ConfigCardinality::Matrix;
    return def.is_scalar() ? ConfigCardinality::Scalar : ConfigCardinality::Unknown;
}

std::optional<double> bound_or_empty(float value)
{
    if (value <= -FLT_MAX / 2 || value >= FLT_MAX / 2)
        return std::nullopt;
    return static_cast<double>(value);
}

ConfigDefinition definition_from_option(const ConfigOptionDef& def)
{
    ConfigDefinition out;
    out.key = def.opt_key;
    out.type = value_type_from_option(def.type);
    out.label = def.full_label.empty() ? def.label : def.full_label;
    out.unit = def.sidetext;
    out.min = bound_or_empty(def.min);
    out.max = bound_or_empty(def.max);
    out.default_value = def.default_value ? def.default_value->serialize() : std::string();
    out.scope = scope_for_key(def.opt_key);
    out.cardinality = cardinality_for_key(def.opt_key, def);

    const size_t enum_count = std::max(def.enum_values.size(), def.enum_labels.size());
    out.enum_options.reserve(enum_count);
    for (size_t i = 0; i < enum_count; ++i) {
        ConfigEnumOption option;
        option.value = i < def.enum_values.size() ? def.enum_values[i] : std::string();
        option.label = i < def.enum_labels.size() ? def.enum_labels[i] : option.value;
        out.enum_options.push_back(std::move(option));
    }
    if (out.enum_options.empty() && def.enum_keys_map != nullptr) {
        out.enum_options.reserve(def.enum_keys_map->size());
        for (const auto& item : *def.enum_keys_map)
            out.enum_options.push_back({ item.first, item.first });
    }
    return out;
}

size_t filament_count_from_config(const DynamicPrintConfig& config)
{
    size_t count = 0;
    if (const auto* opt = config.opt<ConfigOptionStrings>("filament_settings_id"); opt != nullptr)
        count = std::max(count, opt->values.size());
    if (const auto* opt = config.opt<ConfigOptionStrings>("filament_colour"); opt != nullptr)
        count = std::max(count, opt->values.size());
    if (const auto* opt = config.opt<ConfigOptionFloats>("filament_diameter"); opt != nullptr)
        count = std::max(count, opt->values.size());
    return count;
}

size_t option_size(const DynamicPrintConfig& config, const std::string& key)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr)
        return 0;
    if (const auto* vector = dynamic_cast<const ConfigOptionVectorBase*>(option); vector != nullptr)
        return vector->size();
    return 1;
}

void require_string(const DynamicPrintConfig& config,
                    const std::string& key,
                    std::vector<ConfigValidationIssue>& issues)
{
    const auto* option = config.opt<ConfigOptionString>(key);
    if (option == nullptr || option->value.empty())
        issues.push_back(make_issue("missing_required_config", key, key + " is required"));
}

void validate_vector_size(const DynamicPrintConfig& config,
                          const std::string& key,
                          size_t expected,
                          std::vector<ConfigValidationIssue>& issues)
{
    const ConfigOption* option = config.option(key);
    if (option == nullptr) {
        issues.push_back(make_issue("missing_required_config", key, key + " is required"));
        return;
    }
    const auto* vector = dynamic_cast<const ConfigOptionVectorBase*>(option);
    if (vector == nullptr) {
        issues.push_back(make_issue("invalid_config_cardinality", key, key + " must be a vector"));
        return;
    }
    if (vector->size() != expected) {
        issues.push_back(make_issue("invalid_config_cardinality", key,
            key + " length " + std::to_string(vector->size()) + " does not match expected " + std::to_string(expected)));
    }
}

} // namespace

ResolvedConfig::ResolvedConfig() : m_config(DynamicPrintConfig::full_print_config()) {}

ResolvedConfig::ResolvedConfig(DynamicPrintConfig config) : m_config(std::move(config)) {}

std::string ResolvedConfig::to_json() const
{
    return config_to_json(m_config).dump(2);
}

ResolvedConfig ResolvedConfig::from_json(std::string_view json_text, std::vector<ConfigValidationIssue>* issues)
{
    ResolvedConfig config;
    config.apply_json(json_text, issues);
    return config;
}

ResolvedConfig ResolvedConfig::load_json_file(const std::filesystem::path& path, std::vector<ConfigValidationIssue>* issues)
{
    ResolvedConfig config;
    config.apply_json_file(path, issues);
    return config;
}

bool ResolvedConfig::apply_json(std::string_view json_text, std::vector<ConfigValidationIssue>* issues)
{
    json object;
    try {
        object = json::parse(json_text.begin(), json_text.end());
    } catch (const std::exception& e) {
        add_issue(issues, "invalid_config_json", "", std::string("Failed to parse config JSON: ") + e.what());
        return false;
    }
    return apply_json_object_to_config(object, m_config, issues);
}

bool ResolvedConfig::apply_json_file(const std::filesystem::path& path, std::vector<ConfigValidationIssue>* issues)
{
    std::ifstream input(path);
    if (!input.good()) {
        add_issue(issues, "config_file_not_found", "", "Config file not found: " + path.string());
        return false;
    }

    json object;
    try {
        input >> object;
    } catch (const std::exception& e) {
        add_issue(issues, "invalid_config_json", "", std::string("Failed to parse config JSON: ") + e.what());
        return false;
    }
    return apply_json_object_to_config(object, m_config, issues);
}

std::vector<ConfigDefinition> get_config_definitions()
{
    std::vector<ConfigDefinition> definitions;
    definitions.reserve(print_config_def.options.size());
    for (const auto& item : print_config_def.options)
        definitions.push_back(definition_from_option(item.second));
    return definitions;
}

ConfigDefinition get_config_definition(const std::string& key)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    return def != nullptr ? definition_from_option(*def) : ConfigDefinition { key };
}

ConfigResolutionResult resolve_fff_config(const ConfigResolutionRequest& request)
{
    ConfigResolutionResult result;
    result.issues.push_back(make_issue(
        "preset_resolution_unsupported",
        "",
        "resolve_fff_config is reserved for the preset-selection resolver and is not implemented yet"));
    (void)request;
    return result;
}

std::vector<ConfigValidationIssue> validate_resolved_config(const ResolvedConfig& config, int plate_index)
{
    return validate_resolved_config(config.dynamic_config(), plate_index);
}

std::vector<ConfigValidationIssue> validate_resolved_config(const DynamicPrintConfig& config, int plate_index)
{
    std::vector<ConfigValidationIssue> issues;

    require_string(config, "printer_model", issues);
    require_string(config, "printer_settings_id", issues);
    require_string(config, "print_settings_id", issues);

    const size_t filament_count = filament_count_from_config(config);
    if (filament_count == 0) {
        issues.push_back(make_issue("missing_required_config", "filament_settings_id",
            "At least one filament slot is required"));
        return issues;
    }

    validate_vector_size(config, "filament_settings_id", filament_count, issues);
    validate_vector_size(config, "filament_colour", filament_count, issues);
    validate_vector_size(config, "filament_diameter", filament_count, issues);
    validate_vector_size(config, "filament_map", filament_count, issues);

    const auto* filament_map = config.opt<ConfigOptionInts>("filament_map");
    int max_mapped_extruder = 0;
    if (filament_map != nullptr) {
        for (size_t i = 0; i < filament_map->values.size(); ++i) {
            const int value = filament_map->values[i];
            if (value <= 0) {
                issues.push_back(make_issue("invalid_config_value", "filament_map",
                    "filament_map values must be 1-based positive extruder ids"));
            }
            max_mapped_extruder = std::max(max_mapped_extruder, value);
        }
    }

    const size_t extruder_type_size = option_size(config, "extruder_type");
    const size_t nozzle_volume_type_size = option_size(config, "nozzle_volume_type");
    const size_t default_nozzle_volume_type_size = option_size(config, "default_nozzle_volume_type");
    const size_t nozzle_diameter_size = option_size(config, "nozzle_diameter");
    const size_t required_extruder_slots = static_cast<size_t>(std::max(1, max_mapped_extruder));

    for (const std::string& key : { "extruder_type", "nozzle_volume_type", "default_nozzle_volume_type" }) {
        const size_t size = option_size(config, key);
        if (size < required_extruder_slots) {
            issues.push_back(make_issue("invalid_config_cardinality", key,
                key + " length " + std::to_string(size) + " does not cover filament_map max extruder id " +
                std::to_string(required_extruder_slots)));
        }
    }
    if (nozzle_diameter_size == 0) {
        issues.push_back(make_issue("missing_required_config", "nozzle_diameter", "nozzle_diameter is required"));
    }

    const auto* filament_self_index = config.opt<ConfigOptionInts>("filament_self_index");
    const auto* filament_extruder_variant = config.opt<ConfigOptionStrings>("filament_extruder_variant");
    if (filament_self_index == nullptr || filament_extruder_variant == nullptr) {
        issues.push_back(make_issue("missing_required_config", "filament_self_index",
            "filament_self_index and filament_extruder_variant are required"));
    } else {
        if (filament_self_index->values.size() != filament_extruder_variant->values.size()) {
            issues.push_back(make_issue("invalid_config_cardinality", "filament_self_index",
                "filament_self_index length must match filament_extruder_variant length"));
        }

        if (filament_map != nullptr &&
            extruder_type_size >= required_extruder_slots &&
            nozzle_volume_type_size >= required_extruder_slots) {
            const auto* extruder_type = config.opt<ConfigOptionEnumsGeneric>("extruder_type");
            const auto* nozzle_volume_type = config.opt<ConfigOptionEnumsGeneric>("nozzle_volume_type");
            if (extruder_type != nullptr && nozzle_volume_type != nullptr) {
                for (size_t f = 0; f < filament_map->values.size(); ++f) {
                    const int extruder_id = filament_map->values[f];
                    if (extruder_id <= 0)
                        continue;
                    const std::string expected_variant = get_extruder_variant_string(
                        static_cast<ExtruderType>(extruder_type->get_at(extruder_id - 1)),
                        static_cast<NozzleVolumeType>(nozzle_volume_type->get_at(extruder_id - 1)));
                    bool found = false;
                    for (size_t i = 0; i < filament_self_index->values.size() && i < filament_extruder_variant->values.size(); ++i) {
                        if (filament_self_index->values[i] == static_cast<int>(f + 1) &&
                            filament_extruder_variant->values[i] == expected_variant) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        issues.push_back(make_issue("invalid_variant_lookup", "filament_extruder_variant",
                            "No filament variant entry for filament slot " + std::to_string(f + 1) +
                            " and extruder variant " + expected_variant));
                    }
                }
            }
        }
    }

    for (const std::string& key : { "support_filament", "support_interface_filament", "wipe_tower_filament" }) {
        const auto* option = config.opt<ConfigOptionInt>(key);
        if (option != nullptr && (option->value < 0 || option->value > static_cast<int>(filament_count))) {
            issues.push_back(make_issue("invalid_config_value", key,
                key + " must be 0 or between 1 and filament slot count"));
        }
    }

    if (plate_index >= 0) {
        const size_t selected_plate = static_cast<size_t>(plate_index);
        for (const std::string& key : { "wipe_tower_x", "wipe_tower_y" }) {
            const size_t size = option_size(config, key);
            if (size != 0 && selected_plate >= size) {
                issues.push_back(make_issue("invalid_config_cardinality", key,
                    key + " length " + std::to_string(size) + " does not include selected plate index " +
                    std::to_string(plate_index)));
            }
        }
    }

    if (config.option("gcode_flavor") == nullptr)
        issues.push_back(make_issue("missing_required_config", "gcode_flavor", "gcode_flavor is required"));
    if (config.option("use_relative_e_distances") == nullptr)
        issues.push_back(make_issue("missing_required_config", "use_relative_e_distances", "use_relative_e_distances is required"));
    if (config.option("before_layer_change_gcode") == nullptr)
        issues.push_back(make_issue("missing_required_config", "before_layer_change_gcode", "before_layer_change_gcode is required"));
    if (config.option("layer_change_gcode") == nullptr)
        issues.push_back(make_issue("missing_required_config", "layer_change_gcode", "layer_change_gcode is required"));

    (void)default_nozzle_volume_type_size;
    return issues;
}

bool has_config_errors(const std::vector<ConfigValidationIssue>& issues)
{
    return std::any_of(issues.begin(), issues.end(), [](const ConfigValidationIssue& issue) {
        return issue.severity == ConfigIssueSeverity::Error;
    });
}

} // namespace Slic3r::libslicer
