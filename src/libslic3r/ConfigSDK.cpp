#include "ConfigSDK.hpp"
#include "ConfigSDK_internal.hpp"

#include "Config.hpp"
#include "Format/bbs_3mf.hpp"
#include "Model.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "Semver.hpp"
#include "Utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <system_error>

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
                                 std::vector<ConfigValidationIssue>* issues,
                                 UnknownKeyPolicy unknown_key_policy = UnknownKeyPolicy::Warning)
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

    const ConfigIssueSeverity unknown_key_severity =
        unknown_key_policy == UnknownKeyPolicy::Error ? ConfigIssueSeverity::Error : ConfigIssueSeverity::Warning;

    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
    bool ok = true;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const std::string key = it.key();
        const ConfigOptionDef* option_def = config_def->get(key);
        if (option_def == nullptr) {
            add_issue(issues, "unknown_config_key", key, "Unknown config key: " + key, unknown_key_severity);
            ok = ok && unknown_key_severity != ConfigIssueSeverity::Error;
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

json numeric_string_to_json(std::string value, bool integer)
{
    if (value == "nil")
        return value;
    if (!value.empty() && value.back() == '%')
        value.pop_back();

    char* end = nullptr;
    if (integer) {
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == '\0')
            return parsed;
    } else {
        const double parsed = std::strtod(value.c_str(), &end);
        if (end != value.c_str() && *end == '\0')
            return parsed;
    }
    return value;
}

json float_or_percent_string_to_json(const std::string& value)
{
    if (!value.empty() && value.back() == '%')
        return value;
    return numeric_string_to_json(value, false);
}

json point_string_to_json(const std::string& value)
{
    const size_t sep = value.find_first_of("x,");
    if (sep == std::string::npos)
        return value;

    char* end_x = nullptr;
    const double x = std::strtod(value.substr(0, sep).c_str(), &end_x);
    char* end_y = nullptr;
    const std::string y_text = value.substr(sep + 1);
    const double y = std::strtod(y_text.c_str(), &end_y);
    if (end_x != nullptr && *end_x == '\0' && end_y != y_text.c_str() && *end_y == '\0')
        return json::array({ x, y });
    return value;
}

json vector_option_to_json(const ConfigOptionVectorBase& option)
{
    const std::vector<std::string> values = option.vserialize();
    json out = json::array();
    for (const std::string& value : values) {
        switch (option.type()) {
        case coStrings:
            out.push_back(value);
            break;
        case coInts:
            out.push_back(numeric_string_to_json(value, true));
            break;
        case coEnums:
            // ConfigSDK's public JSON contract uses the stable enum key, never
            // the internal integer ordinal. ConfigOptionEnumsGeneric::vserialize()
            // already resolves each ordinal through its enum key map.
            out.push_back(value);
            break;
        case coBools:
            out.push_back(value == "1" || value == "true");
            break;
        case coFloats:
        case coPercents:
            out.push_back(numeric_string_to_json(value, false));
            break;
        case coFloatsOrPercents:
            // This is a tagged union: retaining '%' distinguishes a ratio
            // from an absolute distance across SDK/App round-trips.
            out.push_back(float_or_percent_string_to_json(value));
            break;
        case coPoints:
            out.push_back(point_string_to_json(value));
            break;
        default:
            out.push_back(value);
            break;
        }
    }
    return out;
}

json option_to_json(const ConfigOption& option)
{
    if (const auto* vector = dynamic_cast<const ConfigOptionVectorBase*>(&option); vector != nullptr)
        return vector_option_to_json(*vector);

    switch (option.type()) {
    case coBool:
        return option.getBool();
    case coInt:
        return option.getInt();
    case coEnum:
        // Integer enum storage is an Orca implementation detail. Public JSON
        // carries the schema key so extract -> App -> resolve is lossless.
        return option.serialize();
    case coFloat:
    case coPercent:
        return option.getFloat();
    case coFloatOrPercent:
        return float_or_percent_string_to_json(option.serialize());
    case coString:
        if (const auto* string_option = dynamic_cast<const ConfigOptionString*>(&option); string_option != nullptr)
            return string_option->value;
        break;
    case coPoint:
        return point_string_to_json(option.serialize());
    default:
        break;
    }

    return option.serialize();
}

json config_to_json(const DynamicPrintConfig& config)
{
    json out = json::object();
    for (const std::string& key : config.keys()) {
        const ConfigOption* option = config.option(key);
        if (option != nullptr)
            out[key] = option_to_json(*option);
    }
    return out;
}

// Keys present in both `base` and `target` whose serialized value differs,
// serialized from `target`. nlohmann::json's default object type is key-sorted
// (std::map-backed), so the result is canonically ordered without extra work.
json config_diff_to_json(const DynamicPrintConfig& base, const DynamicPrintConfig& target)
{
    json out = json::object();
    for (const std::string& key : base.diff(target)) {
        const ConfigOption* option = target.option(key);
        if (option != nullptr)
            out[key] = option_to_json(*option);
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
    static const std::map<std::string, ConfigScope> scope_overrides {
        // Synthetic SDK / resolved-config identifiers are not present in the
        // preset key lists, but they still have stable ownership.
        { "printer_settings_id", ConfigScope::Printer },
        { "print_settings_id", ConfigScope::Process },
        { "filament_settings_id", ConfigScope::Filament },
        { "filament_colour", ConfigScope::Filament },
        { "filament_colour_type", ConfigScope::Filament },
        { "filament_multi_colour", ConfigScope::Filament },
        { "filament_map", ConfigScope::Filament },
        { "filament_map_mode", ConfigScope::Project },
        { "filament_self_index", ConfigScope::Filament },
        { "filament_extruder_variant", ConfigScope::Filament },
        { "extruder_type", ConfigScope::Printer },
        { "nozzle_volume_type", ConfigScope::Printer },
        { "default_nozzle_volume_type", ConfigScope::Printer },
        // These are project-level slot selectors even though their values point
        // to filament slots.
        { "support_filament", ConfigScope::Process },
        { "support_interface_filament", ConfigScope::Process },
        { "wipe_tower_filament", ConfigScope::Process },
        // These live in PresetBundle::project_config even if their definitions
        // originate from printer/process option groups.
        { "curr_bed_type", ConfigScope::Project },
        { "flush_multiplier", ConfigScope::Project },
        { "flush_volumes_matrix", ConfigScope::Project },
        { "flush_volumes_vector", ConfigScope::Project },
        { "wipe_tower_x", ConfigScope::Project },
        { "wipe_tower_y", ConfigScope::Project },
        { "wipe_tower_rotation_angle", ConfigScope::Project }
    };
    if (const auto it = scope_overrides.find(key); it != scope_overrides.end())
        return it->second;

    static const std::set<std::string> printer_keys = [] {
        std::set<std::string> keys(Preset::printer_options().begin(), Preset::printer_options().end());
        keys.insert(Preset::sla_printer_options().begin(), Preset::sla_printer_options().end());
        return keys;
    }();
    static const std::set<std::string> process_keys = [] {
        std::set<std::string> keys(Preset::print_options().begin(), Preset::print_options().end());
        keys.insert(Preset::sla_print_options().begin(), Preset::sla_print_options().end());
        return keys;
    }();
    static const std::set<std::string> filament_keys = [] {
        std::set<std::string> keys(Preset::filament_options().begin(), Preset::filament_options().end());
        keys.insert(Preset::sla_material_options().begin(), Preset::sla_material_options().end());
        return keys;
    }();

    if (printer_keys.count(key) != 0)
        return ConfigScope::Printer;
    if (process_keys.count(key) != 0)
        return ConfigScope::Process;
    if (filament_keys.count(key) != 0)
        return ConfigScope::Filament;
    if (key.rfind("object_", 0) == 0)
        return ConfigScope::Object;
    if (key.rfind("part_", 0) == 0)
        return ConfigScope::Part;
    return ConfigScope::Project;
}

ConfigCardinality cardinality_for_key(const std::string& key, const ConfigOptionDef& def)
{
    const auto vector_contains = [](const std::vector<std::string>& keys, const std::string& value) {
        return std::find(keys.begin(), keys.end(), value) != keys.end();
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
    if (variant_lookup_keys.count(key) != 0)
        return ConfigCardinality::PrinterVariantLookup;
    if (plate_keys.count(key) != 0)
        return ConfigCardinality::Plate;
    if (matrix_keys.count(key) != 0)
        return ConfigCardinality::Matrix;
    if (printer_options_with_variant_1.count(key) != 0 || printer_options_with_variant_2.count(key) != 0)
        return ConfigCardinality::PrinterVariantLookup;
    if (filament_options_with_variant.count(key) != 0)
        return ConfigCardinality::FilamentExtruderVariant;
    if (print_options_with_variant.count(key) != 0)
        return ConfigCardinality::ProcessExtruderVariant;
    if (vector_contains(print_config_def.extruder_option_keys(), key))
        return ConfigCardinality::PhysicalExtruder;
    if (!def.is_scalar() && scope_for_key(key) == ConfigScope::Filament)
        return ConfigCardinality::Filament;
    return ConfigCardinality::Scalar;
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
    out.scope = scope_for_key(def.opt_key);
    out.cardinality = cardinality_for_key(def.opt_key, def);
    if (def.default_value) {
        out.default_value = def.default_value->serialize();
        out.default_json = option_to_json(*def.default_value).dump();
    }
    out.nullable = def.nullable;
    out.internal = def.mode == comDevelop || out.scope == ConfigScope::Internal;

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

json config_to_json_for_scope(const DynamicPrintConfig& config, ConfigScope scope)
{
    json out = json::object();
    for (const std::string& key : config.keys()) {
        if (scope_for_key(key) != scope)
            continue;
        const ConfigOption* option = config.option(key);
        if (option != nullptr)
            out[key] = option_to_json(*option);
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

// --- resolve_fff_config plumbing (spec §6.5) ---------------------------------
//
// Preset loading here deliberately does not go through PresetBundle::load_presets
// (AppConfig&, ...): that path scans data_dir()/system, which assumes a prior
// GUI-driven copy of resources_dir()/profiles into data_dir()/system that has no
// equivalent in this headless SDK. Instead:
//   - vendor_bundle_dirs are loaded directly via load_vendor_configs_from_json,
//     which reads a vendor's <Vendor>.json index plus its machine/process/filament
//     sub_path files straight out of resources_dir()/profiles.
//   - user_preset_dirs are loaded via PresetCollection::load_presets(dir, ...)
//     against the same process/filament/machine subdirectory convention Orca's
//     own user-preset loader uses (PresetBundle::load_user_presets).
//   - project_preset_files are single already-exported preset json files (e.g.
//     extracted from a 3mf); each carries its own "type"/"inherits" and is
//     loaded by hand since there is no bulk directory to scan.

ForwardCompatibilitySubstitutionRule substitution_rule_for(bool strict)
{
    // Never throw: resolve_fff_config reports structured issues, not exceptions.
    // `strict` only affects logging verbosity of value-level substitutions; the
    // strict/permissive behavior that matters to callers (unknown *keys* in the
    // *_overrides_json documents) is controlled separately, see override_policy_for.
    return strict ? ForwardCompatibilitySubstitutionRule::Enable : ForwardCompatibilitySubstitutionRule::EnableSilent;
}

UnknownKeyPolicy override_policy_for(bool strict)
{
    return strict ? UnknownKeyPolicy::Error : UnknownKeyPolicy::Warning;
}

bool read_json_file(const std::filesystem::path& path, json& out, std::string& error)
{
    std::ifstream input(path);
    if (!input.good()) {
        error = "File not found: " + path.string();
        return false;
    }
    try {
        input >> out;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool write_json_file(const std::filesystem::path& path, const json& object, std::string& error)
{
    std::ofstream output(path);
    if (!output.good()) {
        error = "Cannot open file for writing: " + path.string();
        return false;
    }
    try {
        output << object.dump(1, '\t') << '\n';
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

// Applies an *_overrides_json document (may be empty) onto `config`. Returns
// false if the document itself was invalid or (in strict mode) contained an
// unknown key; known-key failures are still applied best-effort so a caller can
// see the rest of the merged config alongside the reported issues.
bool apply_overrides_json(const std::string& overrides_json,
                          DynamicPrintConfig& config,
                          UnknownKeyPolicy unknown_key_policy,
                          std::vector<ConfigValidationIssue>& issues)
{
    if (overrides_json.empty())
        return true;

    json object;
    try {
        object = json::parse(overrides_json);
    } catch (const std::exception& e) {
        add_issue(&issues, "invalid_config_json", "", std::string("Failed to parse overrides JSON: ") + e.what());
        return false;
    }
    return apply_json_object_to_config(object, config, &issues, unknown_key_policy);
}

void set_single_string_vector(DynamicPrintConfig& config, const std::string& key, const std::string& value)
{
    if (value.empty())
        return;
    config.option<ConfigOptionStrings>(key, true)->values = { value };
}

void apply_filament_slot_metadata(const FilamentSlotRequest& slot, DynamicPrintConfig& config)
{
    set_single_string_vector(config, "filament_colour", slot.color);
    set_single_string_vector(config, "filament_colour_type", slot.color_type);
    set_single_string_vector(config, "filament_type", slot.filament_type);
}

bool load_vendor_bundle_dir(PresetBundle& bundle,
                            const std::filesystem::path& vendor_dir,
                            ForwardCompatibilitySubstitutionRule rule,
                            std::vector<ConfigValidationIssue>& issues,
                            const PresetBundle* base_bundle = nullptr)
{
    const std::string vendor_name = vendor_dir.filename().string();
    const std::filesystem::path root = vendor_dir.parent_path();
    const std::filesystem::path index_file = root / (vendor_name + ".json");
    if (vendor_name.empty() || !std::filesystem::exists(index_file)) {
        add_issue(&issues, "vendor_bundle_not_found", vendor_dir.string(),
                  "Vendor bundle index not found: " + index_file.string());
        return false;
    }
    try {
        bundle.load_vendor_configs_from_json(root.string(), vendor_name, PresetBundle::LoadSystem, rule, base_bundle);
    } catch (const std::exception& e) {
        add_issue(&issues, "vendor_bundle_load_failed", vendor_dir.string(), e.what());
        return false;
    }
    return true;
}

struct AmbiguousPresetNames
{
    std::set<std::string> printers;
    std::set<std::string> processes;
    std::set<std::string> filaments;
};

void collect_ambiguous_preset_names(const PresetCollection& existing,
                                    const PresetCollection& incoming,
                                    std::set<std::string>& names)
{
    for (const Preset& preset : incoming.get_presets()) {
        if (preset.is_default || preset.is_external)
            continue;
        if (existing.find_preset(preset.name, false) != nullptr)
            names.insert(preset.name);
    }
}

void collect_ambiguous_preset_names(const PresetBundle& existing,
                                    const PresetBundle& incoming,
                                    AmbiguousPresetNames& names)
{
    collect_ambiguous_preset_names(existing.printers, incoming.printers, names.printers);
    collect_ambiguous_preset_names(existing.prints, incoming.prints, names.processes);
    collect_ambiguous_preset_names(existing.filaments, incoming.filaments, names.filaments);
}

bool load_vendor_bundle_dirs(PresetBundle& bundle,
                             const std::vector<std::filesystem::path>& vendor_dirs,
                             ForwardCompatibilitySubstitutionRule rule,
                             std::vector<ConfigValidationIssue>& issues,
                             AmbiguousPresetNames* ambiguous_names = nullptr)
{
    bool ok = true;
    bool first = true;
    for (const std::filesystem::path& vendor_dir : vendor_dirs) {
        if (first) {
            ok = load_vendor_bundle_dir(bundle, vendor_dir, rule, issues) && ok;
            first = false;
            continue;
        }

        PresetBundle other;
        if (!load_vendor_bundle_dir(other, vendor_dir, rule, issues, &bundle)) {
            ok = false;
            continue;
        }

        if (ambiguous_names != nullptr)
            collect_ambiguous_preset_names(bundle, other, *ambiguous_names);
        const std::vector<std::string> duplicates = bundle.merge_vendor_bundle_for_config_sdk(std::move(other));
        if (!duplicates.empty()) {
            add_issue(&issues, "duplicate_preset", vendor_dir.string(),
                      "Vendor bundle contains duplicate preset names", ConfigIssueSeverity::Warning);
        }
    }
    return ok;
}

std::vector<std::filesystem::path> discover_vendor_bundle_dirs(const std::filesystem::path& resources_dir)
{
    std::vector<std::filesystem::path> vendor_dirs;
    const std::filesystem::path profiles_dir = resources_dir / "profiles";
    std::error_code ec;
    if (!std::filesystem::is_directory(profiles_dir, ec))
        return vendor_dirs;

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(profiles_dir, ec)) {
        if (ec)
            break;
        if (!entry.is_directory(ec))
            continue;
        const std::filesystem::path vendor_dir = entry.path();
        const std::filesystem::path index_file = profiles_dir / (vendor_dir.filename().string() + ".json");
        if (std::filesystem::exists(index_file, ec))
            vendor_dirs.push_back(vendor_dir);
    }
    std::sort(vendor_dirs.begin(), vendor_dirs.end());
    return vendor_dirs;
}

bool validate_search_directory(const std::filesystem::path& path,
                               const std::string& field,
                               std::vector<ConfigValidationIssue>& issues)
{
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return true;

    add_issue(&issues, "preset_search_path_invalid", field,
              "Preset search path is not a readable directory: " + path.string());
    return false;
}

bool validate_search_file(const std::filesystem::path& path,
                          const std::string& field,
                          std::vector<ConfigValidationIssue>& issues)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec))
        return true;

    add_issue(&issues, "preset_search_path_invalid", field,
              "Preset search path is not a readable file: " + path.string());
    return false;
}

void load_user_preset_dir(PresetBundle& bundle,
                          const std::filesystem::path& dir,
                          ForwardCompatibilitySubstitutionRule rule,
                          PresetsConfigSubstitutions& substitutions,
                          AmbiguousPresetNames* ambiguous_names = nullptr);

bool load_project_preset_file(PresetBundle& bundle,
                              const std::filesystem::path& file,
                              const std::string& field,
                              ForwardCompatibilitySubstitutionRule rule,
                              std::vector<ConfigValidationIssue>& issues,
                              AmbiguousPresetNames* ambiguous_names = nullptr);

bool load_sdk_preset_bundle(PresetBundle& bundle,
                            const std::filesystem::path& resources_dir,
                            const std::filesystem::path& data_dir,
                            const std::vector<std::filesystem::path>& vendor_bundle_dirs,
                            const std::vector<std::filesystem::path>& user_preset_dirs,
                            const std::vector<std::filesystem::path>& project_preset_files,
                            ForwardCompatibilitySubstitutionRule rule,
                            std::vector<ConfigValidationIssue>& issues,
                            AmbiguousPresetNames* ambiguous_names = nullptr)
{
    if (!resources_dir.empty())
        Slic3r::set_resources_dir(resources_dir.string());
    if (!data_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(data_dir, ec);
        Slic3r::set_data_dir(data_dir.string());
    }

    bundle.setup_directories();

    std::vector<std::filesystem::path> vendors = vendor_bundle_dirs;
    if (vendors.empty()) {
        const std::filesystem::path profiles_dir = resources_dir / "profiles";
        if (!validate_search_directory(profiles_dir, "resources_dir", issues))
            return false;
        vendors = discover_vendor_bundle_dirs(resources_dir);
    }

    bool load_ok = true;
    for (size_t i = 0; i < vendors.size(); ++i)
        load_ok = validate_search_directory(vendors[i], "vendor_bundle_dirs[" + std::to_string(i) + "]", issues) && load_ok;
    if (load_ok)
        load_ok = load_vendor_bundle_dirs(bundle, vendors, rule, issues, ambiguous_names);

    PresetsConfigSubstitutions user_substitutions;
    for (size_t i = 0; i < user_preset_dirs.size(); ++i) {
        const std::filesystem::path& dir = user_preset_dirs[i];
        if (!validate_search_directory(dir, "user_preset_dirs[" + std::to_string(i) + "]", issues)) {
            load_ok = false;
            continue;
        }
        load_user_preset_dir(bundle, dir, rule, user_substitutions, ambiguous_names);
    }

    for (size_t i = 0; i < project_preset_files.size(); ++i) {
        const std::filesystem::path& file = project_preset_files[i];
        if (!validate_search_file(file, "project_preset_files[" + std::to_string(i) + "]", issues)) {
            load_ok = false;
            continue;
        }
        load_ok = load_project_preset_file(bundle, file, "project_preset_files[" + std::to_string(i) + "]",
                                           rule, issues, ambiguous_names) && load_ok;
    }

    return load_ok;
}

void collect_ambiguous_user_preset_names(const PresetCollection& collection,
                                         const std::filesystem::path& dir,
                                         const std::string& subdir,
                                         std::set<std::string>& names)
{
    const std::filesystem::path preset_dir = dir / subdir;
    std::error_code ec;
    if (!std::filesystem::is_directory(preset_dir, ec))
        return;

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(preset_dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
            continue;

        const std::string preset_name = entry.path().stem().string();
        if (collection.find_preset(preset_name, false) != nullptr)
            names.insert(preset_name);
    }
}

void collect_ambiguous_user_preset_names(const PresetBundle& bundle,
                                         const std::filesystem::path& dir,
                                         AmbiguousPresetNames& names)
{
    collect_ambiguous_user_preset_names(bundle.prints, dir, PRESET_PRINT_NAME, names.processes);
    collect_ambiguous_user_preset_names(bundle.filaments, dir, PRESET_FILAMENT_NAME, names.filaments);
    collect_ambiguous_user_preset_names(bundle.printers, dir, PRESET_PRINTER_NAME, names.printers);
}

void load_user_preset_dir(PresetBundle& bundle,
                          const std::filesystem::path& dir,
                          ForwardCompatibilitySubstitutionRule rule,
                          PresetsConfigSubstitutions& substitutions,
                          AmbiguousPresetNames* ambiguous_names)
{
    if (ambiguous_names != nullptr)
        collect_ambiguous_user_preset_names(bundle, dir, *ambiguous_names);

    bundle.prints.load_presets(dir.string(), PRESET_PRINT_NAME, substitutions, rule);
    bundle.filaments.load_presets(dir.string(), PRESET_FILAMENT_NAME, substitutions, rule);
    bundle.printers.load_presets(dir.string(), PRESET_PRINTER_NAME, substitutions, rule);
}

// A project_preset_files entry is a single already-exported preset JSON (e.g.
// extracted from a 3mf's embedded presets). It carries its own "type" (machine /
// process / filament) and may declare "inherits" against a preset that must
// already be loaded (system or user). This mirrors, at reduced fidelity (no
// extruder-variant-length extension pass), the inherits-merge PresetCollection::
// load_presets performs for bulk directory loads.
bool load_project_preset_file(PresetBundle& bundle,
                              const std::filesystem::path& file,
                              const std::string& field,
                              ForwardCompatibilitySubstitutionRule rule,
                              std::vector<ConfigValidationIssue>& issues,
                              AmbiguousPresetNames* ambiguous_names)
{
    json root;
    std::string error;
    if (!read_json_file(file, root, error)) {
        add_issue(&issues, "project_preset_invalid", field,
                  "Project preset file is invalid: " + file.string() + ": " + error);
        return false;
    }

    const std::string type = root.value("type", std::string());
    const std::string name = root.value("name", file.stem().string());

    PresetCollection* collection = nullptr;
    if (type == "machine")
        collection = &bundle.printers;
    else if (type == "process")
        collection = &bundle.prints;
    else if (type == "filament")
        collection = &bundle.filaments;
    else {
        add_issue(&issues, "project_preset_invalid", field,
                  "Unknown or missing preset 'type' in project preset file: " + file.string());
        return false;
    }

    if (const Preset* existing = collection->find_preset(name, false);
        existing != nullptr && existing->is_project_embedded) {
        if (ambiguous_names != nullptr) {
            if (type == "machine")
                ambiguous_names->printers.insert(name);
            else if (type == "process")
                ambiguous_names->processes.insert(name);
            else if (type == "filament")
                ambiguous_names->filaments.insert(name);
        }
        return true;
    }

    DynamicPrintConfig file_config;
    std::map<std::string, std::string> key_values;
    std::string reason;
    file_config.load_from_json(file.string(), rule, key_values, reason);
    if (!reason.empty()) {
        add_issue(&issues, "project_preset_invalid", field,
                  "Project preset config is invalid: " + file.string() + ": " + reason);
        return false;
    }

    DynamicPrintConfig merged = collection->default_preset_for(file_config).config;
    if (const auto* inherits_opt = file_config.opt<ConfigOptionString>("inherits");
        inherits_opt != nullptr && !inherits_opt->value.empty()) {
        const Preset* parent = collection->find_preset(inherits_opt->value, false);
        if (parent != nullptr) {
            merged = parent->config;
        } else {
            add_issue(&issues, "project_preset_invalid", field,
                      "Project preset '" + name + "' inherits unknown preset '" + inherits_opt->value + "'");
            return false;
        }
    }
    merged.apply(std::move(file_config));
    Preset& loaded = collection->load_preset(file.string(), name, std::move(merged), /*select=*/false);
    loaded.is_project_embedded = true;
    return true;
}

// slot_index values must be a duplicate-free, contiguous 0- or 1-based range
// covering exactly the loaded slots — anything else means the caller's
// filament_slots array doesn't line up with the printer's extruder/slot count.
bool filament_slot_indices_valid(const std::vector<int>& slot_indices)
{
    if (slot_indices.empty())
        return true;
    std::vector<int> sorted_indices = slot_indices;
    std::sort(sorted_indices.begin(), sorted_indices.end());
    sorted_indices.erase(std::unique(sorted_indices.begin(), sorted_indices.end()), sorted_indices.end());
    if (sorted_indices.size() != slot_indices.size())
        return false;
    const int base = sorted_indices.front();
    if (base != 0 && base != 1)
        return false;
    return static_cast<int>(sorted_indices.size()) == (sorted_indices.back() - base + 1);
}

const Preset* find_requested_preset(const PresetCollection& collection,
                                    const std::set<std::string>& ambiguous_names,
                                    const std::string& preset_id,
                                    const std::string& field,
                                    const std::string& label,
                                    std::vector<ConfigValidationIssue>& issues)
{
    if (ambiguous_names.find(preset_id) != ambiguous_names.end()) {
        add_issue(&issues, "preset_ambiguous", field,
                  label + " preset name is ambiguous across loaded preset bundles: " + preset_id);
        return nullptr;
    }

    const Preset* preset = collection.find_preset(preset_id, false);
    if (preset == nullptr)
        add_issue(&issues, "preset_not_found", field, label + " preset not found: " + preset_id);
    return preset;
}

bool validate_requested_preset_compatibility(const Preset& printer_preset,
                                             const Preset& process_preset,
                                             const std::vector<Preset>& filament_presets,
                                             const std::vector<std::string>& filament_fields,
                                             std::vector<ConfigValidationIssue>& issues)
{
    bool ok = true;
    const PresetWithVendorProfile active_printer(printer_preset, printer_preset.vendor);
    const PresetWithVendorProfile active_process(process_preset, process_preset.vendor);

    if (!process_preset.is_project_embedded && !is_compatible_with_printer(active_process, active_printer)) {
        add_issue(&issues, "preset_incompatible", "process_preset_id",
                  "Process preset is not compatible with printer preset: " + process_preset.name);
        ok = false;
    }

    for (size_t i = 0; i < filament_presets.size(); ++i) {
        const Preset& filament = filament_presets[i];
        const std::string& field = filament_fields[i];
        if (filament.is_project_embedded)
            continue;
        const PresetWithVendorProfile active_filament(filament, filament.vendor);
        if (!is_compatible_with_printer(active_filament, active_printer)) {
            add_issue(&issues, "preset_incompatible", field,
                      "Filament preset is not compatible with printer preset: " + filament.name);
            ok = false;
            continue;
        }
        if (!process_preset.is_project_embedded &&
            !is_compatible_with_print(active_filament, active_process, active_printer)) {
            add_issue(&issues, "preset_incompatible", field,
                      "Filament preset is not compatible with process preset: " + filament.name);
            ok = false;
        }
    }

    return ok;
}

PresetKind preset_kind_for(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_PRINTER:
        return PresetKind::Printer;
    case Preset::TYPE_PRINT:
        return PresetKind::Process;
    case Preset::TYPE_FILAMENT:
        return PresetKind::Filament;
    case Preset::TYPE_SLA_PRINT:
        return PresetKind::SlaProcess;
    case Preset::TYPE_SLA_MATERIAL:
        return PresetKind::SlaMaterial;
    default:
        return PresetKind::Printer;
    }
}

PresetCatalogEntry preset_catalog_entry_from(const Preset& preset)
{
    PresetCatalogEntry entry;
    entry.kind = preset_kind_for(preset.type);
    entry.preset_id = preset.name;
    entry.name = preset.name;
    entry.alias = preset.alias;
    entry.inherits = preset.inherits();
    entry.setting_id = preset.setting_id;
    entry.filament_id = preset.filament_id;
    entry.base_id = preset.base_id;
    entry.is_system = preset.is_system;
    entry.is_user = preset.is_user();
    entry.is_project_embedded = preset.is_project_embedded;
    entry.is_default = preset.is_default;
    entry.is_visible = preset.is_visible;
    entry.is_compatible = preset.is_compatible;
    if (preset.vendor != nullptr) {
        entry.vendor_id = preset.vendor->id;
        entry.vendor_name = preset.vendor->name;
    }
    return entry;
}

void append_catalog_entries(const PresetCollection& collection,
                            bool compatible_only,
                            std::vector<PresetCatalogEntry>& out)
{
    for (size_t i = 0; i < collection.size(); ++i) {
        const Preset& preset = collection.preset(i);
        if (preset.is_default)
            continue;
        if (compatible_only && !preset.is_compatible)
            continue;
        out.push_back(preset_catalog_entry_from(preset));
    }
}

struct Project3mfResources
{
    PlateDataPtrs plates;
    std::vector<Preset*> presets;

    ~Project3mfResources()
    {
        release_PlateData_list(plates);
        for (Preset* preset : presets)
            delete preset;
        presets.clear();
    }
};

std::string scalar_string_option(const DynamicPrintConfig& config, const std::string& key)
{
    const auto* option = config.opt<ConfigOptionString>(key);
    return option != nullptr ? option->value : std::string();
}

std::vector<std::string> string_vector_option(const DynamicPrintConfig& config, const std::string& key)
{
    const auto* option = config.opt<ConfigOptionStrings>(key);
    return option != nullptr ? option->values : std::vector<std::string>();
}

std::string vector_value_or_empty(const std::vector<std::string>& values, size_t index)
{
    return index < values.size() ? values[index] : std::string();
}

std::string sanitized_filename_component(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '/':
        case '\\':
        case ':':
        case '*':
        case '?':
        case '"':
        case '<':
        case '>':
        case '|':
            out.push_back('_');
            break;
        default:
            out.push_back(static_cast<unsigned char>(ch) < 0x20 ? '_' : ch);
            break;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out.empty() ? "preset" : out;
}

std::filesystem::path unique_project_preset_file(const std::filesystem::path& root,
                                                 const Preset& preset,
                                                 std::vector<ConfigValidationIssue>* issues)
{
    const std::string type = Preset::get_type_string(preset.type);
    if (type.empty()) {
        add_issue(issues, "unknown_preset_type", preset.name,
                  "Cannot materialize embedded project preset with unknown type: " + preset.name);
        return {};
    }

    const std::filesystem::path dir = root / "project_presets" / type;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        add_issue(issues, "project_preset_export_failed", dir.string(), ec.message());
        return {};
    }

    const std::string base = sanitized_filename_component(preset.name);
    std::filesystem::path candidate = dir / (base + ".json");
    for (int suffix = 2; std::filesystem::exists(candidate, ec); ++suffix)
        candidate = dir / (base + "_" + std::to_string(suffix) + ".json");
    return candidate;
}

std::filesystem::path materialize_project_preset(const Preset& preset,
                                                 const std::filesystem::path& data_dir,
                                                 std::vector<ConfigValidationIssue>* issues)
{
    const std::filesystem::path output_file = unique_project_preset_file(data_dir, preset, issues);
    if (output_file.empty())
        return {};

    preset.config.save_to_json(output_file.string(), preset.name, "project", preset.version.to_string());

    json root;
    std::string error;
    if (!read_json_file(output_file, root, error)) {
        add_issue(issues, "project_preset_export_failed", output_file.string(), error);
        return {};
    }

    root[BBL_JSON_KEY_TYPE] = Preset::get_type_string(preset.type);
    root[BBL_JSON_KEY_NAME] = preset.name;
    root[BBL_JSON_KEY_FROM] = "project";
    root[BBL_JSON_KEY_VERSION] = preset.version.to_string();

    if (!write_json_file(output_file, root, error)) {
        add_issue(issues, "project_preset_export_failed", output_file.string(), error);
        return {};
    }
    return output_file;
}

std::vector<FilamentSlotRequest> filament_slots_from_project_config(const DynamicPrintConfig& config)
{
    const std::vector<std::string> preset_ids = string_vector_option(config, "filament_settings_id");
    const std::vector<std::string> colors = string_vector_option(config, "filament_colour");
    const std::vector<std::string> types = string_vector_option(config, "filament_type");

    std::vector<FilamentSlotRequest> slots;
    slots.reserve(preset_ids.size());
    for (size_t i = 0; i < preset_ids.size(); ++i) {
        if (preset_ids[i].empty())
            continue;
        FilamentSlotRequest slot;
        slot.slot_index = static_cast<int>(i);
        slot.filament_preset_id = preset_ids[i];
        slot.color = vector_value_or_empty(colors, i);
        slot.filament_type = vector_value_or_empty(types, i);
        slots.push_back(std::move(slot));
    }
    return slots;
}

Project3mfPlate project_plate_from(const PlateData& plate)
{
    Project3mfPlate out;
    out.plate_index = plate.plate_index;
    out.plate_name = plate.plate_name;
    out.printer_model_id = plate.printer_model_id;
    out.nozzle_diameters = plate.nozzle_diameters;
    out.filament_maps = plate.filament_maps;
    out.config_json = config_to_json(plate.config).dump(2);
    return out;
}

json model_object_overrides_to_json(const Model& model)
{
    json out = json::array();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject* object = model.objects[object_index];
        if (object == nullptr || object->config.empty())
            continue;

        out.push_back({
            { "object_index", object_index },
            { "object_name", object->name },
            { "config", config_to_json(object->config.get()) }
        });
    }
    return out;
}

json model_part_overrides_to_json(const Model& model)
{
    json out = json::array();
    for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const ModelObject* object = model.objects[object_index];
        if (object == nullptr)
            continue;

        for (size_t part_index = 0; part_index < object->volumes.size(); ++part_index) {
            const ModelVolume* volume = object->volumes[part_index];
            if (volume == nullptr || volume->config.empty())
                continue;

            out.push_back({
                { "object_index", object_index },
                { "object_name", object->name },
                { "part_index", part_index },
                { "part_name", volume->name },
                { "part_type", ModelVolume::type_to_string(volume->type()) },
                { "config", config_to_json(volume->config.get()) }
            });
        }
    }
    return out;
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

std::string config_sdk_version()
{
    return "0.1.0";
}

std::vector<std::string> config_sdk_capabilities()
{
    // Only advertise SDK surfaces that are implemented and covered by focused
    // tests, so external callers can feature-detect conservatively.
    return {
        "schema.v1",
        "validate_resolved_config.v1",
        "resolve_fff_config.v1",
        "preset_catalog.v1",
        "project_3mf_config.v1",
        "parse_json.v1",
        "diff_config.v1"
    };
}

std::vector<ConfigDefinition> get_config_definitions()
{
    std::vector<ConfigDefinition> definitions;
    definitions.reserve(print_config_def.options.size());
    for (const auto& item : print_config_def.options)
        definitions.push_back(definition_from_option(item.second));
    return definitions;
}

std::vector<ConfigDefinition> list_config_definitions()
{
    return get_config_definitions();
}

ConfigDefinition get_config_definition(const std::string& key)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    return def != nullptr ? definition_from_option(*def) : ConfigDefinition { key };
}

std::optional<ConfigDefinition> find_config_definition(std::string_view key)
{
    const ConfigOptionDef* def = print_config_def.get(std::string(key));
    if (def == nullptr)
        return std::nullopt;
    return definition_from_option(*def);
}

PresetCatalogResult load_preset_catalog(const PresetCatalogRequest& request)
{
    PresetCatalogResult result;
    std::vector<ConfigValidationIssue>& issues = result.issues;

    if (request.data_dir.empty())
        add_issue(&issues, "missing_required_config", "data_dir", "data_dir is required");
    if (request.resources_dir.empty())
        add_issue(&issues, "missing_required_config", "resources_dir", "resources_dir is required");
    if (has_config_errors(issues))
        return result;

    PresetBundle bundle;
    const ForwardCompatibilitySubstitutionRule rule = substitution_rule_for(request.strict);
    const bool load_ok = load_sdk_preset_bundle(bundle, request.resources_dir, request.data_dir,
                                                request.vendor_bundle_dirs, request.user_preset_dirs,
                                                request.project_preset_files, rule, issues);

    const Preset* active_printer = nullptr;
    if (!request.printer_preset_id.empty()) {
        active_printer = bundle.printers.find_preset(request.printer_preset_id, false);
        if (active_printer == nullptr) {
            add_issue(&issues, "preset_not_found", "printer_preset_id",
                      "Printer preset not found: " + request.printer_preset_id);
        } else {
            bundle.printers.select_preset_by_name(request.printer_preset_id, true);
        }
    }

    const Preset* active_process = nullptr;
    if (!request.process_preset_id.empty()) {
        active_process = bundle.prints.find_preset(request.process_preset_id, false);
        if (active_process == nullptr) {
            add_issue(&issues, "preset_not_found", "process_preset_id",
                      "Process preset not found: " + request.process_preset_id);
        } else {
            bundle.prints.select_preset_by_name(request.process_preset_id, true);
        }
    }

    if (active_printer != nullptr) {
        try {
            bundle.update_multi_material_filament_presets();
            bundle.update_compatible(PresetSelectCompatibleType::Never);
        } catch (const std::exception& e) {
            add_issue(&issues, "catalog_compatibility_failed", "", e.what());
        }
    }

    if (!load_ok && has_config_errors(issues))
        return result;

    append_catalog_entries(bundle.printers, request.compatible_only, result.printers);
    append_catalog_entries(bundle.prints, request.compatible_only, result.processes);
    append_catalog_entries(bundle.filaments, request.compatible_only, result.filaments);
    return result;
}

ConfigResolutionResult resolve_fff_config(const ConfigResolutionRequest& request)
{
    ConfigResolutionResult result;
    std::vector<ConfigValidationIssue>& issues = result.issues;

    if (request.data_dir.empty())
        add_issue(&issues, "missing_required_config", "data_dir", "data_dir is required");
    if (request.resources_dir.empty())
        add_issue(&issues, "missing_required_config", "resources_dir", "resources_dir is required");
    if (request.printer_preset_id.empty())
        add_issue(&issues, "missing_required_config", "printer_preset_id", "printer_preset_id is required");
    if (request.process_preset_id.empty())
        add_issue(&issues, "missing_required_config", "process_preset_id", "process_preset_id is required");
    if (request.filament_slots.empty())
        add_issue(&issues, "missing_required_config", "filament_slots", "At least one filament slot is required");

    if (has_config_errors(issues))
        return result;

    const ForwardCompatibilitySubstitutionRule rule = substitution_rule_for(request.strict);
    const UnknownKeyPolicy override_policy = override_policy_for(request.strict);

    // 1) Load bundle: vendor system presets, user presets, project-embedded presets.
    PresetBundle bundle;
    AmbiguousPresetNames ambiguous_names;
    bool load_ok = load_sdk_preset_bundle(bundle, request.resources_dir, request.data_dir,
                                          request.vendor_bundle_dirs, request.user_preset_dirs,
                                          request.project_preset_files, rule, issues, &ambiguous_names);

    // 2) Select printer/process presets by id. Missing -> structured error, no
    // default substitution.
    const Preset* printer_preset = find_requested_preset(bundle.printers, ambiguous_names.printers,
                                                         request.printer_preset_id, "printer_preset_id", "Printer", issues);

    const Preset* process_preset = find_requested_preset(bundle.prints, ambiguous_names.processes,
                                                         request.process_preset_id, "process_preset_id", "Process", issues);

    // 3) Select each filament_slots[].filament_preset_id. Missing -> error; slot
    // index cardinality mismatch -> error.
    bool filament_ok = true;
    std::vector<int> slot_indices;
    std::vector<Preset> filament_presets_raw;        // presets as loaded, no slot_overrides_json applied
    std::vector<Preset> filament_presets_overridden;  // presets with slot_overrides_json applied
    std::vector<std::string> filament_fields;
    for (size_t i = 0; i < request.filament_slots.size(); ++i) {
        const FilamentSlotRequest& slot = request.filament_slots[i];
        const std::string field = "filament_slots[" + std::to_string(i) + "].filament_preset_id";
        if (slot.filament_preset_id.empty()) {
            add_issue(&issues, "missing_required_config", field, field + " is required");
            filament_ok = false;
            continue;
        }
        const Preset* filament_preset = find_requested_preset(bundle.filaments, ambiguous_names.filaments,
                                                              slot.filament_preset_id, field, "Filament", issues);
        if (filament_preset == nullptr) {
            filament_ok = false;
            continue;
        }
        filament_presets_raw.push_back(*filament_preset);
        filament_fields.push_back(field);
        Preset overridden = *filament_preset;
        apply_filament_slot_metadata(slot, overridden.config);
        if (!apply_overrides_json(slot.slot_overrides_json, overridden.config, override_policy, issues))
            filament_ok = false;
        filament_presets_overridden.push_back(std::move(overridden));
        slot_indices.push_back(slot.slot_index);
    }

    if (!filament_slot_indices_valid(slot_indices)) {
        add_issue(&issues, "invalid_config_cardinality", "filament_slots",
                  "filament_slots[].slot_index values must be a duplicate-free, contiguous 0- or 1-based range");
    }

    if (!load_ok || printer_preset == nullptr || process_preset == nullptr || !filament_ok || filament_presets_overridden.empty())
        return result;

    if (!validate_requested_preset_compatibility(*printer_preset, *process_preset, filament_presets_raw, filament_fields, issues))
        return result;

    // 4) Apply printer/process_overrides_json onto preset copies; project_overrides_json
    // is applied after merge (below) so it wins over filament-preset values for
    // shared keys, matching "project" being the outermost/highest-priority layer.
    Preset printer_copy = *printer_preset;
    apply_overrides_json(request.printer_overrides_json, printer_copy.config, override_policy, issues);
    Preset process_copy = *process_preset;
    apply_overrides_json(request.process_overrides_json, process_copy.config, override_policy, issues);

    Preset printer_baseline = *printer_preset;
    Preset process_baseline = *process_preset;

    const int used_filaments = static_cast<int>(filament_presets_overridden.size());

    DynamicPrintConfig merged;
    DynamicPrintConfig baseline;
    try {
        // 5) PresetBundle::construct_full_config -> DynamicPrintConfig::normalize_fdm.
        merged = PresetBundle::construct_full_config(printer_copy, process_copy, DynamicPrintConfig(),
                                                      filament_presets_overridden, request.apply_extruder, std::nullopt);
        if (!apply_overrides_json(request.project_overrides_json, merged, override_policy, issues))
            filament_ok = false;
        merged.normalize_fdm(used_filaments);

        // Same pipeline without any of the *_overrides_json layers, for normalized_diff_json.
        baseline = PresetBundle::construct_full_config(printer_baseline, process_baseline, DynamicPrintConfig(),
                                                        filament_presets_raw, request.apply_extruder, std::nullopt);
        baseline.normalize_fdm(used_filaments);
    } catch (const std::exception& e) {
        add_issue(&issues, "preset_resolution_failed", "", e.what());
        return result;
    }

    // 6) Reuse validate_resolved_config.
    std::vector<ConfigValidationIssue> validation_issues = validate_resolved_config(merged, request.plate_index);
    issues.insert(issues.end(), std::make_move_iterator(validation_issues.begin()), std::make_move_iterator(validation_issues.end()));

    // 7) Canonical full_config_json / normalized_diff_json (nlohmann's default
    // object type is key-sorted, so this is already golden-diff stable).
    result.full_config_json = config_to_json(merged).dump(2);
    result.normalized_diff_json = config_diff_to_json(baseline, merged).dump(2);
    return result;
}

ConfigParseResult parse_config_json(std::string_view json_text, const ConfigParseOptions& options)
{
    ConfigParseResult result;
    json object;
    try {
        object = json::parse(json_text.begin(), json_text.end());
    } catch (const std::exception& e) {
        add_issue(&result.issues, "invalid_config_json", "", std::string("Failed to parse config JSON: ") + e.what());
        return result;
    }
    if (!object.is_object()) {
        add_issue(&result.issues, "invalid_config_json", "", "Config JSON must be an object");
        return result;
    }

    // Parse against the full schema (needed so set_deserialize/enum maps work),
    // but only emit the keys that were actually present in the input — spec
    // §6.3 forbids default-filling absent keys.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    if (!apply_json_object_to_config(object, config, &result.issues, options.unknown_key_policy) &&
        options.unknown_key_policy == UnknownKeyPolicy::Error) {
        return result;
    }

    static const char* const required_keys[] = { "printer_settings_id", "print_settings_id" };
    const ConfigIssueSeverity missing_required_severity =
        options.missing_required_policy == MissingRequiredPolicy::Error ? ConfigIssueSeverity::Error : ConfigIssueSeverity::Warning;
    for (const char* key : required_keys) {
        if (!object.contains(key))
            add_issue(&result.issues, "missing_required_config", key, std::string(key) + " is required", missing_required_severity);
    }

    if (options.canonicalize) {
        json canonical = json::object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const ConfigOption* option = config.option(it.key());
            if (option != nullptr)
                canonical[it.key()] = option_to_json(*option);
        }
        result.canonical_json = canonical.dump(2);
    } else {
        result.canonical_json = object.dump(2);
    }
    return result;
}

ConfigParseResult parse_config_file(const std::filesystem::path& path, const ConfigParseOptions& options)
{
    std::ifstream input(path);
    if (!input.good()) {
        ConfigParseResult result;
        add_issue(&result.issues, "config_file_not_found", "", "Config file not found: " + path.string());
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_config_json(buffer.str(), options);
}

std::vector<ConfigIssue> validate_resolved_config(const ConfigValidationRequest& request)
{
    std::vector<ConfigIssue> issues;
    ResolvedConfig config;
    if (!config.apply_json(request.full_config_json, &issues))
        return issues;

    std::vector<ConfigIssue> validation_issues =
        validate_resolved_config(config.dynamic_config(), request.plate_index);
    issues.insert(issues.end(),
                  std::make_move_iterator(validation_issues.begin()),
                  std::make_move_iterator(validation_issues.end()));
    if (request.run_print_validate) {
        add_issue(&issues, "unsupported_config_feature", "run_print_validate",
                  "run_print_validate is not supported by the ConfigSDK validation API yet");
    }
    return issues;
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

    const auto* print_extruder_id = config.opt<ConfigOptionInts>("print_extruder_id");
    const auto* print_extruder_variant = config.opt<ConfigOptionStrings>("print_extruder_variant");
    const size_t print_extruder_id_size = print_extruder_id != nullptr ? print_extruder_id->values.size() : 0;
    const size_t print_extruder_variant_size = print_extruder_variant != nullptr ? print_extruder_variant->values.size() : 0;
    if ((print_extruder_id_size == 0) != (print_extruder_variant_size == 0)) {
        issues.push_back(make_issue("missing_required_config",
            print_extruder_id_size == 0 ? "print_extruder_id" : "print_extruder_variant",
            "print_extruder_id and print_extruder_variant must be provided together"));
    } else if (print_extruder_id_size != 0) {
        if (print_extruder_id_size != print_extruder_variant_size) {
            issues.push_back(make_issue("invalid_config_cardinality", "print_extruder_id",
                "print_extruder_id length must match print_extruder_variant length"));
        }
        for (int value : print_extruder_id->values) {
            if (value <= 0) {
                issues.push_back(make_issue("invalid_config_value", "print_extruder_id",
                    "print_extruder_id values must be 1-based positive extruder ids"));
                break;
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

Project3mfExtractionResult extract_project_3mf_config(const Project3mfExtractionRequest& request)
{
    Project3mfExtractionResult result;
    std::vector<ConfigValidationIssue>& issues = result.issues;

    if (request.project_file.empty())
        add_issue(&issues, "missing_required_config", "project_file", "project_file is required");
    else if (!std::filesystem::exists(request.project_file))
        add_issue(&issues, "project_3mf_not_found", "project_file",
                  "3MF project file not found: " + request.project_file.string());
    if (request.data_dir.empty())
        add_issue(&issues, "missing_required_config", "data_dir", "data_dir is required");
    if (has_config_errors(issues))
        return result;

    std::error_code ec;
    std::filesystem::create_directories(request.data_dir, ec);

    DynamicPrintConfig project_config;
    ConfigSubstitutionContext substitutions { substitution_rule_for(request.strict) };
    Model model;
    model.set_backup_path((request.data_dir / "config_sdk_3mf_extract").string());
    std::filesystem::create_directories(model.get_backup_path(), ec);

    Project3mfResources resources;
    Semver file_version;

    try {
        const LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence;
        if (!load_bbs_3mf(request.project_file.string().c_str(),
                          &project_config,
                          &substitutions,
                          &model,
                          &resources.plates,
                          &resources.presets,
                          &result.is_bbl_3mf,
                          &result.is_orca_3mf,
                          &file_version,
                          nullptr,
                          strategy,
                          nullptr,
                          request.plate_index)) {
            add_issue(&issues, "project_3mf_load_failed", "project_file",
                      "Failed to load 3MF project: " + request.project_file.string());
            return result;
        }
    } catch (const std::exception& e) {
        add_issue(&issues, "project_3mf_load_failed", "project_file", e.what());
        return result;
    }

    result.file_version = file_version.to_string();
    result.project_config_json = config_to_json(project_config).dump(2);
    result.printer_overrides_json = config_to_json_for_scope(project_config, ConfigScope::Printer).dump(2);
    result.process_overrides_json = config_to_json_for_scope(project_config, ConfigScope::Process).dump(2);
    result.project_overrides_json = config_to_json_for_scope(project_config, ConfigScope::Project).dump(2);
    result.object_overrides_json = model_object_overrides_to_json(model).dump(2);
    result.part_overrides_json = model_part_overrides_to_json(model).dump(2);
    result.printer_preset_id = scalar_string_option(project_config, "printer_settings_id");
    result.process_preset_id = scalar_string_option(project_config, "print_settings_id");
    result.filament_slots = filament_slots_from_project_config(project_config);
    if (result.printer_preset_id.empty()) {
        add_issue(&issues, "missing_project_3mf_config", "printer_settings_id",
                  "3MF project config does not contain printer_settings_id");
    }
    if (result.process_preset_id.empty()) {
        add_issue(&issues, "missing_project_3mf_config", "print_settings_id",
                  "3MF project config does not contain print_settings_id");
    }
    if (result.filament_slots.empty()) {
        add_issue(&issues, "missing_project_3mf_config", "filament_settings_id",
                  "3MF project config does not contain filament_settings_id");
    }

    result.embedded_presets.reserve(resources.presets.size());
    result.extracted_project_preset_files.reserve(resources.presets.size());
    for (const Preset* preset : resources.presets) {
        if (preset != nullptr) {
            result.embedded_presets.push_back(preset_catalog_entry_from(*preset));
            const std::filesystem::path extracted_file = materialize_project_preset(*preset, request.data_dir, &issues);
            if (!extracted_file.empty())
                result.extracted_project_preset_files.push_back(extracted_file);
        }
    }

    result.plates.reserve(resources.plates.size());
    for (const PlateData* plate : resources.plates) {
        if (plate != nullptr)
            result.plates.push_back(project_plate_from(*plate));
    }

    model.set_backup_path("detach");
    return result;
}

ConfigDiffResult diff_config(const ConfigDiffRequest& request)
{
    ConfigDiffResult result;

    ResolvedConfig base;
    std::vector<ConfigValidationIssue> base_issues;
    if (!base.apply_json(request.base_config_json, &base_issues)) {
        result.issues = std::move(base_issues);
        return result;
    }

    ResolvedConfig target;
    std::vector<ConfigValidationIssue> target_issues;
    if (!target.apply_json(request.target_config_json, &target_issues)) {
        result.issues = std::move(target_issues);
        return result;
    }

    result.issues.insert(result.issues.end(), base_issues.begin(), base_issues.end());
    result.issues.insert(result.issues.end(), target_issues.begin(), target_issues.end());

    // Orca-native comparison (DynamicConfig::diff, per-option operator!=), only
    // non-base values, serialized the same way parse_config_json emits values so
    // the output re-parses through it.
    result.diff_json = config_diff_to_json(base.dynamic_config(), target.dynamic_config()).dump(2);
    return result;
}

} // namespace Slic3r::libslicer
