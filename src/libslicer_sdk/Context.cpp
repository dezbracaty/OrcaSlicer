#include "ContextInternal.hpp"
#include "OrcaConfigAdapter.hpp"
#include "ProjectInternal.hpp"
#include "SliceInternal.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace libslicer::v1 {

SdkContext::SdkContext(std::shared_ptr<detail::ContextState> state) : state_(std::move(state)) {}

Result<SdkContext> SdkContext::create(ContextOptions options)
{
    const std::array<std::pair<std::uint64_t, const char *>, 7> limits {{
        {options.limits.project_input_bytes, "/limits/project_input_bytes"},
        {options.limits.project_uncompressed_bytes, "/limits/project_uncompressed_bytes"},
        {options.limits.model_triangles, "/limits/model_triangles"},
        {options.limits.gcode_bytes, "/limits/gcode_bytes"},
        {options.limits.preview_bytes, "/limits/preview_bytes"},
        {options.limits.preview_moves, "/limits/preview_moves"},
        {options.limits.temporary_disk_bytes, "/limits/temporary_disk_bytes"},
    }};
    for (const auto &limit : limits) {
        if (limit.first == 0)
            return detail::ResultAccess::failure<SdkContext>(ErrorCode::invalid_argument,
                                                             "Resource limit must be non-zero",
                                                             limit.second);
    }

    if (options.resources_dir.empty())
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::invalid_argument,
                                                         "resources_dir must not be empty",
                                                         "/resources_dir");
    if (options.data_dir.empty())
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::invalid_argument,
                                                         "data_dir must not be empty",
                                                         "/data_dir");
    if (options.temporary_dir.empty())
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::invalid_argument,
                                                         "temporary_dir must not be empty",
                                                         "/temporary_dir");

    std::error_code error;
    if (!std::filesystem::is_directory(options.resources_dir, error))
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::io,
                                                         "resources_dir is not a readable directory",
                                                         "/resources_dir");

    std::filesystem::create_directories(options.data_dir, error);
    if (error)
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::io,
                                                         "Unable to create data_dir: " + error.message(),
                                                         "/data_dir");
    std::filesystem::create_directories(options.temporary_dir, error);
    if (error)
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::io,
                                                         "Unable to create temporary_dir: " + error.message(),
                                                         "/temporary_dir");

    for (std::size_t index = 0; index < options.preset_dirs.size(); ++index) {
        error.clear();
        if (!std::filesystem::is_directory(options.preset_dirs[index], error))
            return detail::ResultAccess::failure<SdkContext>(
                ErrorCode::io, "preset_dirs entry is not a readable directory",
                "/preset_dirs/" + std::to_string(index));
    }

    auto schema = detail::build_orca_fff_schema();
    if (!schema.has_value())
        return detail::ResultAccess::failure<SdkContext>(
            *schema.error_code(), schema.diagnostics().front().message,
            schema.diagnostics().front().field);
    auto catalog = detail::load_preset_catalog(options, std::move(schema).value());
    if (!catalog.has_value())
        return detail::ResultAccess::failure<SdkContext>(
            *catalog.error_code(), catalog.diagnostics().front().message,
            catalog.diagnostics().front().field);
    auto state = std::make_shared<detail::ContextState>(std::move(options));
    state->preset_catalog = std::move(catalog).value();
    return detail::ResultAccess::success(SdkContext(std::move(state)));
}

Result<PresetRepository> SdkContext::presets()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return detail::ResultAccess::success(PresetRepository(
        std::make_shared<PresetRepository::State>(state_->preset_catalog)));
}

Result<ProjectBuilder> SdkContext::create_project_builder()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return detail::ResultAccess::success(ProjectBuilder(
        std::make_shared<ProjectBuilder::State>(state_)));
}

Result<SliceEngine> SdkContext::create_slice_engine()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return detail::ResultAccess::success(SliceEngine(
        std::make_shared<detail::SliceEngineState>(state_)));
}

} // namespace libslicer::v1
