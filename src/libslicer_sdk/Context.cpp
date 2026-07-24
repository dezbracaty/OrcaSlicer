#include "ContextInternal.hpp"
#include "OrcaConfigAdapter.hpp"
#include "ProjectInternal.hpp"
#include "RuntimeCoordinator.hpp"
#include "SliceInternal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace libslicer::v1 {

SdkContext::SdkContext(std::shared_ptr<detail::ContextState> state) : state_(std::move(state)) {}

namespace {

template<class Target, class Source>
Result<Target> copy_failure(const Result<Source> &source)
{
    const auto &diagnostics = source.diagnostics();
    if (diagnostics.empty())
        return detail::ResultAccess::failure<Target>(
            source.error_code().value_or(ErrorCode::internal),
            "Operation failed without diagnostics",
            "/runtime");

    std::vector<Diagnostic> additional;
    additional.reserve(diagnostics.size() - 1);
    additional.insert(additional.end(), diagnostics.begin() + 1, diagnostics.end());
    return detail::ResultAccess::failure<Target>(
        source.error_code().value_or(diagnostics.front().code),
        diagnostics.front().message,
        diagnostics.front().field,
        std::move(additional));
}

Result<void> canonicalize_directory(std::filesystem::path &path,
                                    const std::string &field)
{
    std::error_code error;
    path = std::filesystem::canonical(path, error);
    if (error)
        return detail::ResultAccess::failure(
            ErrorCode::io,
            "Unable to canonicalize directory: " + error.message(),
            field);
    return detail::ResultAccess::success();
}

Result<void> verify_writable_directory(const std::filesystem::path &directory,
                                       const std::string &field)
{
    static std::atomic<std::uint64_t> next_probe {1};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path probe =
        directory / (".libslicer-sdk-v1-write-probe-" +
                     std::to_string(nonce) + "-" +
                     std::to_string(next_probe.fetch_add(1)));
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        if (!output)
            return detail::ResultAccess::failure(
                ErrorCode::io, "Directory is not writable", field);
        output.put('\0');
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(probe, ignored);
            return detail::ResultAccess::failure(
                ErrorCode::io, "Directory is not writable", field);
        }
    }

    std::error_code error;
    if (!std::filesystem::remove(probe, error) || error)
        return detail::ResultAccess::failure(
            ErrorCode::io,
            "Unable to remove directory write probe: " + error.message(),
            field);
    return detail::ResultAccess::success();
}

} // namespace

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
    error.clear();
    if (!std::filesystem::is_directory(options.resources_dir / "profiles", error))
        return detail::ResultAccess::failure<SdkContext>(
            ErrorCode::invalid_configuration,
            "Preset profiles directory is required",
            "/resources_dir/profiles");

    error.clear();
    std::filesystem::create_directories(options.data_dir, error);
    if (error)
        return detail::ResultAccess::failure<SdkContext>(ErrorCode::io,
                                                         "Unable to create data_dir: " + error.message(),
                                                         "/data_dir");
    error.clear();
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

    auto canonical = canonicalize_directory(options.resources_dir, "/resources_dir");
    if (!canonical.has_value())
        return copy_failure<SdkContext>(canonical);
    canonical = canonicalize_directory(options.data_dir, "/data_dir");
    if (!canonical.has_value())
        return copy_failure<SdkContext>(canonical);
    canonical = canonicalize_directory(options.temporary_dir, "/temporary_dir");
    if (!canonical.has_value())
        return copy_failure<SdkContext>(canonical);
    for (std::size_t index = 0; index < options.preset_dirs.size(); ++index) {
        canonical = canonicalize_directory(
            options.preset_dirs[index],
            "/preset_dirs/" + std::to_string(index));
        if (!canonical.has_value())
            return copy_failure<SdkContext>(canonical);
    }

    auto writable = verify_writable_directory(options.data_dir, "/data_dir");
    if (!writable.has_value())
        return copy_failure<SdkContext>(writable);
    writable = verify_writable_directory(options.temporary_dir, "/temporary_dir");
    if (!writable.has_value())
        return copy_failure<SdkContext>(writable);

    auto runtime = detail::acquire_shared_runtime(options);
    if (!runtime.has_value())
        return copy_failure<SdkContext>(runtime);
    auto state = std::make_shared<detail::ContextState>(
        std::move(options), runtime.value());
    return detail::ResultAccess::success(SdkContext(std::move(state)));
}

Result<PresetRepository> SdkContext::presets()
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return detail::ResultAccess::success(PresetRepository(
        std::make_shared<PresetRepository::State>(state_, state_->preset_catalog)));
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
