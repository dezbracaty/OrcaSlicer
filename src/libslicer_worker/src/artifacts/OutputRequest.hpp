#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace libslicer::worker::artifacts {

struct FileArtifactOutput {
    bool enabled { false };
    bool required { true };
    std::filesystem::path path;
};

struct PreviewArtifactOutput : FileArtifactOutput {
    std::string transport { "file" };
    std::string format;
    std::string publish;
    int chunk_records { 0 };
};

struct OutputRequest {
    std::filesystem::path artifacts_dir;
    FileArtifactOutput gcode;
    PreviewArtifactOutput preview;
};

std::filesystem::path normalized_absolute_path(const std::filesystem::path& path);
bool is_same_path(const std::filesystem::path& left, const std::filesystem::path& right);
bool is_path_inside_or_same(const std::filesystem::path& parent, const std::filesystem::path& child);

bool parse_output_request(const nlohmann::json& output_json,
                          const std::filesystem::path& working_dir,
                          OutputRequest& output,
                          std::string& error);

} // namespace libslicer::worker::artifacts
