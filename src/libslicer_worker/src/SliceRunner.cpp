#include "libslicer_worker/WorkerServer.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include "artifacts/OutputRequest.hpp"
#include "preview/PreviewArtifactProducer.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace libslicer::worker {
namespace {

using Clock = std::chrono::steady_clock;

struct JobRequest {
    std::string job_id;
    std::filesystem::path request_path;
    std::filesystem::path working_dir;
    std::filesystem::path resources_dir;
    std::filesystem::path data_dir;
    std::string input_type;
    std::filesystem::path input_path;
    int plate_index { -1 };
    std::string config_type;
    std::filesystem::path config_path;
    artifacts::OutputRequest output;
    bool overwrite { true };
    bool keep_intermediate_files { false };
};

void emit_event(const EventCallback& events, WorkerEvent event)
{
    if (events)
        events(event);
}

std::filesystem::path resolve_path(const std::filesystem::path& base, const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute())
        return path;
    return base / path;
}

std::string json_scalar_to_config_string(const nlohmann::json& value)
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

std::string json_point_to_config_string(const nlohmann::json& value)
{
    if (value.is_array() && value.size() >= 2 && value[0].is_number() && value[1].is_number()) {
        std::ostringstream out;
        out << value[0].get<double>() << 'x' << value[1].get<double>();
        return out.str();
    }
    return json_scalar_to_config_string(value);
}

std::string json_array_to_config_string(const nlohmann::json& value, Slic3r::ConfigOptionType type)
{
    char separator = ',';
    if (type == Slic3r::coStrings)
        separator = ';';
    if (type == Slic3r::coPointsGroups)
        separator = '#';

    std::ostringstream out;
    bool first = true;
    for (const nlohmann::json& item : value) {
        if (!first)
            out << separator;
        first = false;

        if (type == Slic3r::coPoints || type == Slic3r::coPoint)
            out << json_point_to_config_string(item);
        else if (item.is_array())
            out << json_array_to_config_string(item, type);
        else if (type == Slic3r::coStrings)
            out << '"' << Slic3r::escape_string_cstyle(json_scalar_to_config_string(item)) << '"';
        else
            out << json_scalar_to_config_string(item);
    }
    return out.str();
}

bool config_enum_value_from_json(const nlohmann::json& value,
                                 const Slic3r::t_config_enum_values& enum_values,
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
        enum_value = Slic3r::ConfigOptionInts::nil_value();
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
                             const nlohmann::json& value_json,
                             const Slic3r::ConfigOptionDef& option_def,
                             Slic3r::DynamicPrintConfig& config,
                             std::string& error)
{
    if (option_def.enum_keys_map == nullptr) {
        error = "Config option " + key + " has no enum value map";
        return false;
    }

    if (option_def.type == Slic3r::coEnum) {
        const nlohmann::json& scalar = value_json.is_array() && !value_json.empty() ? value_json.front() : value_json;
        int enum_value = 0;
        if (!config_enum_value_from_json(scalar, *option_def.enum_keys_map, false, enum_value, error)) {
            error = "Invalid config value for " + key + ": " + error;
            return false;
        }

        Slic3r::ConfigOption* option = config.option(key, true);
        auto* enum_option = dynamic_cast<Slic3r::ConfigOptionEnumGeneric*>(option);
        if (enum_option != nullptr)
            enum_option->keys_map = option_def.enum_keys_map;
        option->setInt(enum_value);
        return true;
    }

    if (!value_json.is_array()) {
        error = "Invalid config value for " + key + ": enum array value must be an array";
        return false;
    }

    Slic3r::ConfigOption* option = config.option(key, true);
    auto* enum_option = dynamic_cast<Slic3r::ConfigOptionEnumsGeneric*>(option);
    if (enum_option != nullptr)
        enum_option->keys_map = option_def.enum_keys_map;
    if (auto* enum_nullable_option = dynamic_cast<Slic3r::ConfigOptionEnumsGenericNullable*>(option); enum_nullable_option != nullptr)
        enum_nullable_option->keys_map = option_def.enum_keys_map;

    std::vector<int> enum_values;
    enum_values.reserve(value_json.size());
    for (const nlohmann::json& item : value_json) {
        int enum_value = 0;
        if (!config_enum_value_from_json(item, *option_def.enum_keys_map, option->nullable(), enum_value, error)) {
            error = "Invalid config value for " + key + ": " + error;
            return false;
        }
        enum_values.push_back(enum_value);
    }

    auto* int_values = dynamic_cast<Slic3r::ConfigOptionInts*>(option);
    if (int_values == nullptr) {
        error = "Invalid config value for " + key + ": enum option storage is not an integer vector";
        return false;
    }
    int_values->values = std::move(enum_values);
    return true;
}

bool apply_resolved_config(const std::filesystem::path& config_path,
                           Slic3r::DynamicPrintConfig& config,
                           const EventCallback& events,
                           const std::string& job_id,
                           std::string& error)
{
    std::ifstream input(config_path);
    if (!input.good()) {
        error = "Config file not found: " + config_path.string();
        return false;
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const std::exception& e) {
        error = std::string("Failed to parse config JSON: ") + e.what();
        return false;
    }
    if (!json.is_object()) {
        error = "Config JSON must be an object";
        return false;
    }

    const Slic3r::ConfigDef* config_def = config.def();
    if (config_def == nullptr) {
        error = "DynamicPrintConfig has no config definition";
        return false;
    }

    Slic3r::ConfigSubstitutionContext substitutions(Slic3r::ForwardCompatibilitySubstitutionRule::Disable);
    for (auto it = json.begin(); it != json.end(); ++it) {
        const std::string key = it.key();
        const Slic3r::ConfigOptionDef* option_def = config_def->get(key);
        if (option_def == nullptr) {
            emit_event(events, { WorkerEventType::Warning, job_id, -1, "", "", "unknown_config_key",
                                 "Unknown config key: " + key });
            continue;
        }

        if (option_def->type == Slic3r::coEnum || option_def->type == Slic3r::coEnums) {
            if (!apply_enum_config_value(key, it.value(), *option_def, config, error))
                return false;
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
            error = "Invalid config value for " + key + ": " + e.what();
            return false;
        }
    }

    return true;
}

void normalize_flush_volumes_config(Slic3r::DynamicPrintConfig& config)
{
    auto* matrix = config.option<Slic3r::ConfigOptionFloats>("flush_volumes_matrix", true);
    auto* vector = config.option<Slic3r::ConfigOptionFloats>("flush_volumes_vector", true);
    auto* multiplier = config.option<Slic3r::ConfigOptionFloats>("flush_multiplier", true);
    auto* filament_colours = config.option<Slic3r::ConfigOptionStrings>("filament_colour", true);
    auto* filament_diameters = config.option<Slic3r::ConfigOptionFloats>("filament_diameter", true);
    auto* nozzle_diameters = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter", true);

    size_t filament_count = filament_colours->values.size();
    if (filament_count == 0)
        filament_count = filament_diameters->values.size();
    if (filament_count <= 1)
        return;

    const std::vector<double> old_matrix = matrix->values;
    const size_t old_nozzle_count = std::max<size_t>(1, multiplier->values.size());
    const size_t nozzle_count = std::max<size_t>(1, nozzle_diameters->values.size());
    const size_t old_matrix_size = old_matrix.size() / old_nozzle_count;
    const size_t old_filament_count = size_t(std::sqrt(double(old_matrix_size)) + EPSILON);
    const size_t new_matrix_size = filament_count * filament_count;

    if (multiplier->values.size() != nozzle_count)
        multiplier->values.resize(nozzle_count, 1.0);

    while (vector->values.size() < 2 * filament_count) {
        vector->values.push_back(vector->values.size() > 1 ? vector->values[0] : 140.0);
        vector->values.push_back(vector->values.size() > 1 ? vector->values[1] : 140.0);
    }
    while (vector->values.size() > 2 * filament_count)
        vector->values.pop_back();

    if (old_matrix.size() == new_matrix_size * nozzle_count)
        return;

    std::vector<double> new_matrix(new_matrix_size * nozzle_count, 0.0);
    for (size_t i = 0; i < filament_count; ++i) {
        for (size_t j = 0; j < filament_count; ++j) {
            for (size_t nozzle_id = 0; nozzle_id < nozzle_count; ++nozzle_id) {
                const size_t new_index = i * filament_count + j + new_matrix_size * nozzle_id;
                const size_t old_index = i * old_filament_count + j + old_matrix_size * nozzle_id;
                if (i < old_filament_count && j < old_filament_count &&
                    nozzle_id < old_nozzle_count && old_index < old_matrix.size()) {
                    new_matrix[new_index] = old_matrix[old_index];
                } else {
                    new_matrix[new_index] = i == j ? 0.0 : vector->values[2 * i] + vector->values[2 * j + 1];
                }
            }
        }
    }
    matrix->values = std::move(new_matrix);
}

void release_project_presets(std::vector<Slic3r::Preset*>& project_presets)
{
    for (Slic3r::Preset* preset : project_presets)
        delete preset;
    project_presets.clear();
}

bool load_orca_project_3mf(const std::string& input,
                           const std::filesystem::path& data_dir,
                           Slic3r::Model& model,
                           Slic3r::DynamicPrintConfig& loaded_config,
                           std::string& error)
{
    Slic3r::ConfigSubstitutionContext ctxt { Slic3r::ForwardCompatibilitySubstitutionRule::Enable };
    Slic3r::PlateDataPtrs plate_data;
    std::vector<Slic3r::Preset*> project_presets;
    Slic3r::Semver file_version;
    bool is_bbl_3mf = false;
    bool is_orca_3mf = false;

    model.set_backup_path((data_dir / "orca_3mf_model").string());

    try {
        if (!Slic3r::load_bbs_3mf(input.c_str(),
                                  &loaded_config,
                                  &ctxt,
                                  &model,
                                  &plate_data,
                                  &project_presets,
                                  &is_bbl_3mf,
                                  &is_orca_3mf,
                                  &file_version,
                                  nullptr,
                                  Slic3r::LoadStrategy::LoadModel | Slic3r::LoadStrategy::LoadConfig)) {
            error = "Failed to load Orca 3MF project: " + input;
            Slic3r::release_PlateData_list(plate_data);
            release_project_presets(project_presets);
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("Failed to load Orca 3MF project: ") + input + ": " + e.what();
        Slic3r::release_PlateData_list(plate_data);
        release_project_presets(project_presets);
        return false;
    }

    Slic3r::release_PlateData_list(plate_data);
    release_project_presets(project_presets);
    model.set_backup_path("detach");
    return true;
}

bool load_model(const JobRequest& request, Slic3r::Model& model, Slic3r::DynamicPrintConfig& loaded_config, std::string& error)
{
    const std::string input = request.input_path.string();
    if (request.input_type == "stl") {
        if (!Slic3r::load_stl(input.c_str(), &model)) {
            error = "Failed to load STL: " + input;
            return false;
        }
    } else if (request.input_type == "3mf") {
        Slic3r::ConfigSubstitutionContext ctxt { Slic3r::ForwardCompatibilitySubstitutionRule::Disable };
        if (!Slic3r::load_3mf(input.c_str(), loaded_config, ctxt, &model, false)) {
            error = "Failed to load 3MF: " + input;
            return false;
        }
    } else if (request.input_type == "orca_3mf_project") {
        if (!load_orca_project_3mf(input, request.data_dir, model, loaded_config, error))
            return false;
    } else {
        error = "Unsupported input type: " + request.input_type;
        return false;
    }

    if (!model.add_default_instances()) {
        error = "Failed to add default model instances";
        return false;
    }
    return true;
}

Slic3r::Vec2d bed_center_from_config(const Slic3r::DynamicPrintConfig& config)
{
    const Slic3r::ConfigOptionPoints* printable_area = config.opt<Slic3r::ConfigOptionPoints>("printable_area");
    if (printable_area == nullptr || printable_area->values.empty())
        return { 100.0, 100.0 };

    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();
    for (const Slic3r::Vec2d& point : printable_area->values) {
        min_x = std::min(min_x, point.x());
        min_y = std::min(min_y, point.y());
        max_x = std::max(max_x, point.x());
        max_y = std::max(max_y, point.y());
    }
    return { (min_x + max_x) * 0.5, (min_y + max_y) * 0.5 };
}

bool optional_object_field(const nlohmann::json& json,
                           const std::string& path,
                           const std::string& key,
                           nlohmann::json& value,
                           std::string& error)
{
    if (!json.contains(key)) {
        value = nlohmann::json::object();
        return true;
    }
    if (!json.at(key).is_object()) {
        error = path + "." + key + " must be an object";
        return false;
    }
    value = json.at(key);
    return true;
}

bool optional_string_field(const nlohmann::json& json,
                           const std::string& path,
                           const std::string& key,
                           const std::string& default_value,
                           std::string& value,
                           std::string& error)
{
    if (!json.contains(key)) {
        value = default_value;
        return true;
    }
    if (!json.at(key).is_string()) {
        error = path + "." + key + " must be a string";
        return false;
    }
    value = json.at(key).get<std::string>();
    return true;
}

bool optional_int_field(const nlohmann::json& json,
                        const std::string& path,
                        const std::string& key,
                        int default_value,
                        int& value,
                        std::string& error)
{
    if (!json.contains(key)) {
        value = default_value;
        return true;
    }
    if (!json.at(key).is_number_integer()) {
        error = path + "." + key + " must be an integer";
        return false;
    }
    value = json.at(key).get<int>();
    return true;
}

bool optional_bool_field(const nlohmann::json& json,
                         const std::string& path,
                         const std::string& key,
                         bool default_value,
                         bool& value,
                         std::string& error)
{
    if (!json.contains(key)) {
        value = default_value;
        return true;
    }
    if (!json.at(key).is_boolean()) {
        error = path + "." + key + " must be a boolean";
        return false;
    }
    value = json.at(key).get<bool>();
    return true;
}

bool parse_request(const std::filesystem::path& request_path, JobRequest& request, std::string& error_code, std::string& error)
{
    error_code = "invalid_request";
    std::ifstream input(request_path);
    if (!input.good()) {
        error = "Request file not found: " + request_path.string();
        return false;
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const std::exception& e) {
        error = std::string("Failed to parse request JSON: ") + e.what();
        return false;
    }
    if (!json.is_object()) {
        error = "Request JSON must be an object";
        return false;
    }

    int version = -1;
    if (!optional_int_field(json, "request", "version", -1, version, error))
        return false;
    if (version != WORKER_JOB_REQUEST_VERSION) {
        error_code = "unsupported_protocol_version";
        error = "Unsupported job request version: " + std::to_string(version);
        return false;
    }

    std::string kind;
    if (!optional_string_field(json, "request", "kind", "", kind, error))
        return false;
    if (kind != "slice") {
        error = "Unsupported job kind: " + kind;
        return false;
    }

    const std::filesystem::path base = request_path.parent_path();
    request.request_path = request_path;
    if (!optional_string_field(json, "request", "job_id", "", request.job_id, error))
        return false;
    if (request.job_id.empty())
        request.job_id = request_path.stem().string();
    std::string working_dir;
    std::string resources_dir;
    std::string data_dir;
    if (!optional_string_field(json, "request", "working_dir", ".", working_dir, error))
        return false;
    request.working_dir = resolve_path(base, working_dir);
    if (!optional_string_field(json, "request", "resources_dir", "", resources_dir, error))
        return false;
    request.resources_dir = resolve_path(request.working_dir, resources_dir);
    if (!optional_string_field(json, "request", "data_dir", "./data", data_dir, error))
        return false;
    request.data_dir = resolve_path(request.working_dir, data_dir);

    nlohmann::json input_json;
    if (!optional_object_field(json, "request", "input", input_json, error))
        return false;
    if (!optional_string_field(input_json, "input", "type", "", request.input_type, error))
        return false;
    std::string input_path;
    if (!optional_string_field(input_json, "input", "path", "", input_path, error))
        return false;
    request.input_path = resolve_path(request.working_dir, input_path);
    if (!optional_int_field(input_json, "input", "plate_index", -1, request.plate_index, error))
        return false;
    if (request.input_type != "stl" && request.input_type != "3mf" && request.input_type != "orca_3mf_project") {
        error = "Unsupported input type: " + request.input_type;
        return false;
    }

    nlohmann::json config_json;
    if (!optional_object_field(json, "request", "config", config_json, error))
        return false;
    if (!optional_string_field(config_json, "config", "type", "", request.config_type, error))
        return false;
    if (request.config_type != "resolved_orca_json" && request.config_type != "project_embedded") {
        error = "Unsupported config type: " + request.config_type;
        return false;
    }
    if (request.config_type == "project_embedded" && request.input_type != "orca_3mf_project") {
        error = "config.type=project_embedded requires input.type=orca_3mf_project";
        return false;
    }
    std::string config_path;
    if (!optional_string_field(config_json, "config", "path", "", config_path, error))
        return false;
    request.config_path = resolve_path(request.working_dir, config_path);

    const nlohmann::json output_json = json.contains("output") ? json.at("output") : nlohmann::json::object();
    if (!artifacts::parse_output_request(output_json, request.working_dir, request.output, error)) {
        error_code = "invalid_output";
        return false;
    }

    nlohmann::json options_json;
    if (!optional_object_field(json, "request", "options", options_json, error))
        return false;
    if (!optional_bool_field(options_json, "options", "overwrite", true, request.overwrite, error))
        return false;
    if (!optional_bool_field(options_json, "options", "keep_intermediate_files", false, request.keep_intermediate_files, error))
        return false;

    if (request.resources_dir.empty()) {
        error = "resources_dir is required";
        return false;
    }
    if (request.input_type.empty() || request.input_path.empty()) {
        error = "input.type and input.path are required";
        return false;
    }
    if (request.config_type == "resolved_orca_json" && request.config_path.empty()) {
        error = "config.path is required";
        return false;
    }
    if (request.plate_index < -1) {
        error = "input.plate_index must be -1 or greater";
        return false;
    }
    return true;
}

bool is_path_inside(const std::filesystem::path& parent, const std::filesystem::path& child)
{
    const std::filesystem::path normalized_parent = artifacts::normalized_absolute_path(parent);
    const std::filesystem::path normalized_child = artifacts::normalized_absolute_path(child);

    auto parent_it = normalized_parent.begin();
    auto child_it = normalized_child.begin();
    for (; parent_it != normalized_parent.end(); ++parent_it, ++child_it) {
        if (child_it == normalized_child.end() || *parent_it != *child_it)
            return false;
    }
    return child_it != normalized_child.end();
}

void remove_directory_tree_if_safe(const std::filesystem::path& working_dir,
                                   const std::filesystem::path& output_parent,
                                   const std::filesystem::path& directory)
{
    if (directory.empty() ||
        artifacts::is_same_path(directory, working_dir) ||
        (!output_parent.empty() && artifacts::is_same_path(directory, output_parent)) ||
        !is_path_inside(working_dir, directory)) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
        return;

    std::filesystem::remove_all(directory, ec);
}

void remove_empty_directory_if_safe(const std::filesystem::path& working_dir,
                                    const std::filesystem::path& output_parent,
                                    const std::filesystem::path& directory)
{
    if (directory.empty() ||
        artifacts::is_same_path(directory, working_dir) ||
        (!output_parent.empty() && artifacts::is_same_path(directory, output_parent)) ||
        !is_path_inside(working_dir, directory)) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_empty(directory, ec))
        return;

    std::filesystem::remove(directory, ec);
}

class JobIntermediateCleanup {
public:
    explicit JobIntermediateCleanup(const JobRequest& request)
        : m_request(request)
    {
    }

    ~JobIntermediateCleanup()
    {
        if (m_request.keep_intermediate_files)
            return;

        const std::filesystem::path output_parent = m_request.output.gcode.enabled ?
            m_request.output.gcode.path.parent_path() :
            std::filesystem::path();
        remove_directory_tree_if_safe(m_request.working_dir, output_parent, m_request.data_dir);
        remove_empty_directory_if_safe(m_request.working_dir, output_parent, m_request.output.artifacts_dir);
    }

private:
    const JobRequest& m_request;
};

void emit_result(const EventCallback& events,
                 const std::string& job_id,
                 bool success,
                 const std::string& code,
                 const std::string& message,
                 const std::filesystem::path& gcode,
                 Clock::time_point started)
{
    WorkerEvent result;
    result.type = WorkerEventType::Result;
    result.job_id = job_id;
    result.success = success;
    result.code = code;
    result.message = message;
    result.path = gcode;
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    emit_event(events, result);
}

} // namespace

int run_slice_job_from_request(const std::filesystem::path& request_path,
                               const EventCallback& events,
                               CancellationToken& cancellation)
{
    const Clock::time_point started = Clock::now();
    JobRequest request;
    std::string error_code;
    std::string error;
    if (!parse_request(request_path, request, error_code, error)) {
        emit_event(events, { WorkerEventType::Error, "", -1, "", "", error_code, error });
        emit_result(events, "", false, error_code, error, {}, started);
        return 3;
    }

    const std::string job_id = request.job_id;
    JobIntermediateCleanup cleanup(request);
    if (!request.overwrite && request.output.gcode.enabled && boost::filesystem::exists(request.output.gcode.path.string())) {
        error = "Output G-code already exists: " + request.output.gcode.path.string();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "output_exists", error });
        emit_result(events, job_id, false, "output_exists", error, request.output.gcode.path, started);
        return 3;
    }
    if (!request.overwrite && request.output.preview.enabled && boost::filesystem::exists(request.output.preview.path.string())) {
        error = "Output preview already exists: " + request.output.preview.path.string();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "output_exists", error });
        emit_result(events, job_id, false, "output_exists", error, request.output.preview.path, started);
        return 3;
    }

    emit_event(events, { WorkerEventType::Progress, job_id, 0, "initializing", "", "", "Initializing" });

    if (cancellation.cancelled()) {
        emit_result(events, job_id, false, "cancelled", "Job was cancelled", {}, started);
        return 6;
    }

    Slic3r::set_resources_dir(request.resources_dir.string());
    Slic3r::set_data_dir(request.data_dir.string());
    boost::filesystem::create_directories(request.data_dir.string());
    if (request.output.preview.enabled)
        boost::filesystem::create_directories(request.output.artifacts_dir.string());
    if (request.output.gcode.enabled)
        boost::filesystem::create_directories(request.output.gcode.path.parent_path().string());

    emit_event(events, { WorkerEventType::Progress, job_id, 10, "loading_input", "", "", "Loading input" });
    Slic3r::Model model;
    Slic3r::DynamicPrintConfig loaded_config;
    if (!load_model(request, model, loaded_config, error)) {
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "model_load_failed", error });
        emit_result(events, job_id, false, "model_load_failed", error, {}, started);
        return 4;
    }

    emit_event(events, { WorkerEventType::Progress, job_id, 20, "loading_config", "", "", "Loading config" });
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    config.apply(loaded_config, true);
    if (request.config_type == "resolved_orca_json" && !apply_resolved_config(request.config_path, config, events, job_id, error)) {
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "invalid_config", error });
        emit_result(events, job_id, false, "invalid_config", error, {}, started);
        return 4;
    }
    normalize_flush_volumes_config(config);

    if (cancellation.cancelled()) {
        emit_result(events, job_id, false, "cancelled", "Job was cancelled", {}, started);
        return 6;
    }

    if (request.input_type == "stl")
        model.center_instances_around_point(bed_center_from_config(config));
    if (request.plate_index >= 0)
        model.curr_plate_index = request.plate_index;

    emit_event(events, { WorkerEventType::Progress, job_id, 35, "preparing_model", "", "", "Preparing model" });
    Slic3r::Print print;
    try {
        if (request.plate_index >= 0)
            print.set_plate_index(request.plate_index);
        print.apply(model, config);
        Slic3r::StringObjectException warning;
        Slic3r::StringObjectException validation_error = print.validate(&warning);
        if (!warning.string.empty())
            emit_event(events, { WorkerEventType::Warning, job_id, -1, "", "", "validation_warning", warning.string });
        if (!validation_error.string.empty()) {
            error = validation_error.string;
            emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "invalid_config", error });
            emit_result(events, job_id, false, "invalid_config", error, {}, started);
            return 4;
        }
    } catch (const std::exception& e) {
        error = e.what();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "slice_failed", error });
        emit_result(events, job_id, false, "slice_failed", error, {}, started);
        return 5;
    }

    emit_event(events, { WorkerEventType::Progress, job_id, 45, "slicing", "", "", "Slicing" });
    try {
        print.process();
    } catch (const std::exception& e) {
        error = e.what();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "slice_failed", error });
        emit_result(events, job_id, false, "slice_failed", error, {}, started);
        return 5;
    }

    if (cancellation.cancelled()) {
        emit_result(events, job_id, false, "cancelled", "Job was cancelled", {}, started);
        return 6;
    }

    emit_event(events, { WorkerEventType::Progress, job_id, 85, "processing", "", "", "Processing toolpaths" });
    Slic3r::GCodeProcessorResult result;
    try {
        print.export_gcode_result(&result);
    } catch (const std::exception& e) {
        error = e.what();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "slice_processing_failed", error });
        emit_result(events, job_id, false, "slice_processing_failed", error, {}, started);
        return 5;
    }

    std::filesystem::path result_path;
    if (request.output.gcode.enabled) {
        emit_event(events, { WorkerEventType::Progress, job_id, 88, "gcode", "", "", "Publishing G-code artifact" });
        bool published_gcode = false;
        try {
            const std::string output_path = print.export_gcode(request.output.gcode.path.string(), nullptr, nullptr, true);
            published_gcode = !output_path.empty() && boost::filesystem::exists(output_path);
            if (!published_gcode)
                error = "G-code export did not create the requested output file";
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!published_gcode) {
            if (request.output.gcode.required) {
                emit_event(events, { WorkerEventType::Error, job_id, -1, "", "gcode", "gcode_publish_failed", error, request.output.gcode.path });
                emit_result(events, job_id, false, "gcode_publish_failed", error, request.output.gcode.path, started);
                return 5;
            }
            emit_event(events, { WorkerEventType::Warning, job_id, -1, "", "gcode", "gcode_publish_failed", error, request.output.gcode.path });
        } else {
            WorkerEvent gcode_event;
            gcode_event.type = WorkerEventType::Artifact;
            gcode_event.job_id = job_id;
            gcode_event.kind = "gcode";
            gcode_event.path = request.output.gcode.path;
            gcode_event.phase = "ready";
            gcode_event.complete = true;
            emit_event(events, gcode_event);
            result_path = request.output.gcode.path;
        }
    }

    if (request.output.preview.enabled) {
        emit_event(events, { WorkerEventType::Progress, job_id, 92, "preview", "", "", "Writing preview artifact" });
        preview::PreviewProducerContext context;
        context.job_id = job_id;
        context.request_path = request.request_path;
        context.working_dir = request.working_dir;
        context.resources_dir = request.resources_dir;
        context.data_dir = request.data_dir;
        context.artifacts_dir = request.output.artifacts_dir;
        context.input_type = request.input_type;
        context.input_path = request.input_path;
        context.plate_index = request.plate_index;
        context.gcode_requested = request.output.gcode.enabled;
        context.gcode_path = request.output.gcode.path;
        preview::PreviewProducerResult preview_result = preview::produce_preview_artifact(
            request.output.preview, context, config, result, cancellation);
        if (!preview_result.success) {
            if (preview_result.code == "cancelled") {
                emit_result(events, job_id, false, "cancelled", "Job was cancelled", {}, started);
                return 6;
            }
            if (request.output.preview.required) {
                emit_event(events, { WorkerEventType::Error, job_id, -1, "", "preview", preview_result.code, preview_result.message, request.output.preview.path });
                emit_result(events, job_id, false, preview_result.code, preview_result.message, request.output.preview.path, started);
                return 5;
            }
            emit_event(events, { WorkerEventType::Warning, job_id, -1, "", "preview", preview_result.code, preview_result.message, request.output.preview.path });
        } else {
            emit_event(events, preview::make_preview_ready_event(job_id, preview_result.path));
            if (result_path.empty())
                result_path = preview_result.path;
        }
    }

    emit_event(events, { WorkerEventType::Progress, job_id, 100, "done", "", "", "Done" });
    emit_result(events, job_id, true, "", "", result_path, started);
    return 0;
}

} // namespace libslicer::worker
