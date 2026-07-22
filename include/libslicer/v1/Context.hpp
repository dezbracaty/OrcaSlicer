#pragma once

#include "Result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace libslicer::v1 {

namespace detail { struct ContextAccess; struct ContextState; }

class PresetRepository;
class ProjectBuilder;
class SliceEngine;

struct ResourceLimits {
    std::uint64_t project_input_bytes;
    std::uint64_t project_uncompressed_bytes;
    std::uint64_t model_triangles;
    std::uint64_t gcode_bytes;
    std::uint64_t preview_bytes;
    std::uint64_t preview_moves;
    std::uint64_t temporary_disk_bytes;
};

struct ContextOptions {
    std::filesystem::path              resources_dir;
    std::filesystem::path              data_dir;
    std::filesystem::path              temporary_dir;
    std::vector<std::filesystem::path> preset_dirs;
    ResourceLimits                     limits;
};

class SdkContext {
public:
    static Result<SdkContext> create(ContextOptions options);

    Result<PresetRepository> presets();
    Result<ProjectBuilder> create_project_builder();
    Result<SliceEngine> create_slice_engine();

private:
    explicit SdkContext(std::shared_ptr<detail::ContextState> state);
    std::shared_ptr<detail::ContextState> state_;
    friend struct detail::ContextAccess;
};

} // namespace libslicer::v1
