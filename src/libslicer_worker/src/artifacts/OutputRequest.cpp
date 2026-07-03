#include "artifacts/OutputRequest.hpp"

#include "libslicer_worker/PreviewTypes.hpp"

#include <nlohmann/json.hpp>

#include <system_error>

namespace libslicer::worker::artifacts {
namespace {

std::filesystem::path resolve_path(const std::filesystem::path& base, const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute())
        return path;
    return base / path;
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

bool optional_string_field(const nlohmann::json& json,
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
        error = key + " must be a string";
        return false;
    }
    value = json.at(key).get<std::string>();
    return true;
}

bool parse_file_artifact(const nlohmann::json& json,
                         const std::filesystem::path& base_dir,
                         const std::string& default_filename,
                         FileArtifactOutput& output,
                         bool legacy_string_enabled,
                         std::string& error)
{
    if (json.is_null()) {
        output.enabled = false;
        return true;
    }

    if (json.is_string()) {
        output.enabled = legacy_string_enabled;
        output.required = true;
        const std::filesystem::path raw_path = json.get<std::string>();
        output.path = resolve_path(base_dir, raw_path);
        if (!raw_path.is_absolute() && !is_path_inside_or_same(base_dir, output.path)) {
            error = "relative output artifact path escapes its base directory";
            return false;
        }
    } else if (json.is_object()) {
        output.enabled = json.value("enabled", false);
        output.required = json.value("required", true);
        std::string path;
        if (!optional_string_field(json, "path", default_filename, path, error))
            return false;
        const std::filesystem::path raw_path = path;
        output.path = resolve_path(base_dir, raw_path);
        if (!raw_path.is_absolute() && !is_path_inside_or_same(base_dir, output.path)) {
            error = "relative output artifact path escapes its base directory";
            return false;
        }
    } else {
        error = "output artifact must be a string, object, or null";
        return false;
    }

    if (!output.enabled)
        return true;
    if (output.path.empty()) {
        error = "enabled output artifact path is required";
        return false;
    }
    return true;
}

bool parse_preview_artifact(const nlohmann::json& json,
                            const std::filesystem::path& artifacts_dir,
                            PreviewArtifactOutput& output,
                            std::string& error)
{
    if (json.is_null()) {
        output.enabled = false;
        return true;
    }

    if (!json.is_object()) {
        error = "output.preview must be an object or null";
        return false;
    }

    output.enabled = json.value("enabled", false);
    output.required = json.value("required", true);
    std::string path;
    if (!optional_string_field(json, "path", "preview.orcapv", path, error))
        return false;
    const std::filesystem::path raw_path = path;
    output.path = resolve_path(artifacts_dir, raw_path);
    if (!optional_string_field(json, "format", std::string(preview::binary_format_name), output.format, error))
        return false;
    if (!optional_string_field(json, "publish", "final", output.publish, error))
        return false;
    output.chunk_records = json.value("chunk_records", 0);

    if (!output.enabled)
        return true;
    if (output.path.empty()) {
        error = "enabled output.preview path is required";
        return false;
    }
    if (output.format != preview::binary_format_name) {
        error = "unsupported output.preview format: " + output.format;
        return false;
    }
    if (output.publish != "final") {
        error = "unsupported output.preview publish mode: " + output.publish;
        return false;
    }
    if (output.chunk_records < 0) {
        error = "output.preview.chunk_records must be non-negative";
        return false;
    }
    if (!is_path_inside_or_same(artifacts_dir, output.path)) {
        error = "output.preview.path must be contained by output.artifacts_dir";
        return false;
    }
    return true;
}

} // namespace

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

bool is_path_inside_or_same(const std::filesystem::path& parent, const std::filesystem::path& child)
{
    return is_same_path(parent, child) || is_path_inside(parent, child);
}

bool parse_output_request(const nlohmann::json& output_json,
                          const std::filesystem::path& working_dir,
                          OutputRequest& output,
                          std::string& error)
{
    if (!output_json.is_object()) {
        error = "output must be an object";
        return false;
    }

    std::string artifacts_dir;
    if (!optional_string_field(output_json, "artifacts_dir", "./artifacts", artifacts_dir, error))
        return false;
    const std::filesystem::path raw_artifacts_dir = artifacts_dir;
    output.artifacts_dir = resolve_path(working_dir, raw_artifacts_dir);
    if (output.artifacts_dir.empty()) {
        error = "output.artifacts_dir is required";
        return false;
    }
    if (!raw_artifacts_dir.is_absolute() && !is_path_inside_or_same(working_dir, output.artifacts_dir)) {
        error = "relative output.artifacts_dir escapes working_dir";
        return false;
    }

    const bool has_gcode = output_json.contains("gcode");
    const bool has_preview = output_json.contains("preview");
    if (has_gcode) {
        if (!parse_file_artifact(output_json.at("gcode"), working_dir, "output.gcode", output.gcode, true, error)) {
            error = "Invalid output.gcode: " + error;
            return false;
        }
    }
    if (has_preview) {
        if (!parse_preview_artifact(output_json.at("preview"), output.artifacts_dir, output.preview, error)) {
            error = "Invalid output.preview: " + error;
            return false;
        }
    }

    if (!has_gcode && !has_preview) {
        output.gcode.enabled = true;
        output.gcode.required = true;
        output.gcode.path = resolve_path(working_dir, "output.gcode");
    }

    if (!output.gcode.enabled && !output.preview.enabled) {
        error = "At least one output artifact must be enabled";
        return false;
    }

    return true;
}

} // namespace libslicer::worker::artifacts
