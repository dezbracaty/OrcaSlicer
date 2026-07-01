#include "libslicer_worker/WorkerServer.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
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
    std::filesystem::path output_gcode;
    std::filesystem::path artifacts_dir;
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

bool load_model(const JobRequest& request, Slic3r::Model& model, Slic3r::DynamicPrintConfig& loaded_config, std::string& error)
{
    const std::string input = request.input_path.string();
    if (request.input_type == "stl") {
        if (!Slic3r::load_stl(input.c_str(), &model)) {
            error = "Failed to load STL: " + input;
            return false;
        }
    } else if (request.input_type == "3mf" || request.input_type == "orca_3mf_project") {
        Slic3r::ConfigSubstitutionContext ctxt { Slic3r::ForwardCompatibilitySubstitutionRule::Disable };
        if (!Slic3r::load_3mf(input.c_str(), loaded_config, ctxt, &model, false)) {
            error = "Failed to load 3MF: " + input;
            return false;
        }
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

    const int version = json.value("version", -1);
    if (version != WORKER_JOB_REQUEST_VERSION) {
        error_code = "unsupported_protocol_version";
        error = "Unsupported job request version: " + std::to_string(version);
        return false;
    }

    const std::string kind = json.value("kind", "");
    if (kind != "slice") {
        error = "Unsupported job kind: " + kind;
        return false;
    }

    const std::filesystem::path base = request_path.parent_path();
    request.request_path = request_path;
    request.job_id = json.value("job_id", "");
    if (request.job_id.empty())
        request.job_id = request_path.stem().string();
    request.working_dir = resolve_path(base, json.value("working_dir", "."));
    request.resources_dir = resolve_path(request.working_dir, json.value("resources_dir", ""));
    request.data_dir = resolve_path(request.working_dir, json.value("data_dir", "./data"));

    const nlohmann::json input_json = json.value("input", nlohmann::json::object());
    request.input_type = input_json.value("type", "");
    request.input_path = resolve_path(request.working_dir, input_json.value("path", ""));
    request.plate_index = input_json.value("plate_index", -1);
    if (request.input_type != "stl" && request.input_type != "3mf" && request.input_type != "orca_3mf_project") {
        error = "Unsupported input type: " + request.input_type;
        return false;
    }

    const nlohmann::json config_json = json.value("config", nlohmann::json::object());
    request.config_type = config_json.value("type", "");
    if (request.config_type != "resolved_orca_json" && request.config_type != "project_embedded") {
        error = "Unsupported config type: " + request.config_type;
        return false;
    }
    if (request.config_type == "project_embedded" && request.input_type != "orca_3mf_project") {
        error = "config.type=project_embedded requires input.type=orca_3mf_project";
        return false;
    }
    request.config_path = resolve_path(request.working_dir, config_json.value("path", ""));

    const nlohmann::json output_json = json.value("output", nlohmann::json::object());
    request.output_gcode = resolve_path(request.working_dir, output_json.value("gcode", "./output.gcode"));
    request.artifacts_dir = resolve_path(request.working_dir, output_json.value("artifacts_dir", "./artifacts"));

    const nlohmann::json options_json = json.value("options", nlohmann::json::object());
    request.overwrite = options_json.value("overwrite", true);
    request.keep_intermediate_files = options_json.value("keep_intermediate_files", false);

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

std::filesystem::path normalized_absolute_path(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec)
        absolute = path;
    return absolute.lexically_normal();
}

bool is_same_path(const std::filesystem::path& left, const std::filesystem::path& right)
{
    return normalized_absolute_path(left) == normalized_absolute_path(right);
}

bool is_path_inside(const std::filesystem::path& parent, const std::filesystem::path& child)
{
    const std::filesystem::path normalized_parent = normalized_absolute_path(parent);
    const std::filesystem::path normalized_child = normalized_absolute_path(child);

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
        is_same_path(directory, working_dir) ||
        is_same_path(directory, output_parent) ||
        !is_path_inside(working_dir, directory)) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
        return;

    boost::filesystem::remove_all(directory.string());
}

void remove_empty_directory_if_safe(const std::filesystem::path& working_dir,
                                    const std::filesystem::path& output_parent,
                                    const std::filesystem::path& directory)
{
    if (directory.empty() ||
        is_same_path(directory, working_dir) ||
        is_same_path(directory, output_parent) ||
        !is_path_inside(working_dir, directory)) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_empty(directory, ec))
        return;

    boost::filesystem::remove(directory.string());
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

        const std::filesystem::path output_parent = m_request.output_gcode.parent_path();
        remove_directory_tree_if_safe(m_request.working_dir, output_parent, m_request.data_dir);
        remove_empty_directory_if_safe(m_request.working_dir, output_parent, m_request.artifacts_dir);
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
    if (!request.overwrite && boost::filesystem::exists(request.output_gcode.string())) {
        error = "Output G-code already exists: " + request.output_gcode.string();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "output_exists", error });
        emit_result(events, job_id, false, "output_exists", error, request.output_gcode, started);
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
    boost::filesystem::create_directories(request.artifacts_dir.string());
    boost::filesystem::create_directories(request.output_gcode.parent_path().string());

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

    emit_event(events, { WorkerEventType::Progress, job_id, 85, "gcode", "", "", "Exporting G-code" });
    try {
        Slic3r::GCodeProcessorResult result;
        const std::string output_path = print.export_gcode(request.output_gcode.string(), &result);
        if (output_path.empty() || !boost::filesystem::exists(output_path)) {
            error = "G-code export did not create output file";
            emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "gcode_export_failed", error });
            emit_result(events, job_id, false, "gcode_export_failed", error, {}, started);
            return 5;
        }
    } catch (const std::exception& e) {
        error = e.what();
        emit_event(events, { WorkerEventType::Error, job_id, -1, "", "", "gcode_export_failed", error });
        emit_result(events, job_id, false, "gcode_export_failed", error, {}, started);
        return 5;
    }

    emit_event(events, { WorkerEventType::Artifact, job_id, -1, "", "gcode", "", "", request.output_gcode });
    emit_event(events, { WorkerEventType::Progress, job_id, 100, "done", "", "", "Done" });
    emit_result(events, job_id, true, "", "", request.output_gcode, started);
    return 0;
}

} // namespace libslicer::worker
