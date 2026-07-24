#include "RuntimeCoordinator.hpp"

#include "OrcaConfigAdapter.hpp"
#include "PresetInternal.hpp"

#include <condition_variable>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libslicer::v1::detail {

namespace {

struct InitializationAttempt {
    explicit InitializationAttempt(CatalogKey value) : key(std::move(value)) {}

    CatalogKey key;
    std::mutex mutex;
    std::condition_variable condition;
    bool complete {false};
    std::optional<Result<std::shared_ptr<SharedRuntime>>> result;
};

struct CoordinatorState {
    std::mutex registry_mutex;
    std::shared_ptr<SharedRuntime> live_runtime;
    std::shared_ptr<InitializationAttempt> attempt;
    std::uint64_t successful_generation {0};
#ifdef LIBSLICER_SDK_TESTING
    RuntimeCoordinatorTestStats stats;
    std::mutex hook_mutex;
    RuntimeInitializationTestHook hook;
#endif
};

CoordinatorState &coordinator()
{
    static CoordinatorState state;
    return state;
}

CatalogKey catalog_key(const ContextOptions &options)
{
    return {options.resources_dir, options.data_dir, options.preset_dirs};
}

std::string conflict_field(const CatalogKey &bound, const CatalogKey &requested)
{
    if (bound.resources_dir != requested.resources_dir)
        return "/resources_dir";
    if (bound.data_dir != requested.data_dir)
        return "/data_dir";
    if (bound.preset_dirs.size() != requested.preset_dirs.size())
        return "/preset_dirs";
    for (std::size_t index = 0; index < bound.preset_dirs.size(); ++index)
        if (bound.preset_dirs[index] != requested.preset_dirs[index])
            return "/preset_dirs/" + std::to_string(index);
    return "/runtime";
}

template<class Target, class Source>
Result<Target> copy_failure(const Result<Source> &source)
{
    const auto &diagnostics = source.diagnostics();
    if (diagnostics.empty())
        return ResultAccess::failure<Target>(
            source.error_code().value_or(ErrorCode::internal),
            "Runtime initialization failed without diagnostics",
            "/runtime");

    std::vector<Diagnostic> additional;
    additional.reserve(diagnostics.size() - 1);
    additional.insert(additional.end(), diagnostics.begin() + 1, diagnostics.end());
    return ResultAccess::failure<Target>(
        source.error_code().value_or(diagnostics.front().code),
        diagnostics.front().message,
        diagnostics.front().field,
        std::move(additional));
}

#ifdef LIBSLICER_SDK_TESTING
void invoke_test_hook(RuntimeInitializationTestStage stage)
{
    RuntimeInitializationTestHook hook;
    CoordinatorState &state = coordinator();
    {
        std::lock_guard<std::mutex> lock(state.hook_mutex);
        hook = state.hook;
    }
    if (hook)
        hook(stage);
}
#else
void invoke_test_hook(int) {}
#endif

Result<std::shared_ptr<SharedRuntime>> initialize_runtime(
    const ContextOptions &options, CatalogKey key)
{
    try {
#ifdef LIBSLICER_SDK_TESTING
        {
            CoordinatorState &state = coordinator();
            std::lock_guard<std::mutex> lock(state.registry_mutex);
            ++state.stats.loader_invocations;
        }
#endif

        std::lock_guard<std::mutex> core(runtime_mutex());
        auto schema = build_orca_fff_schema();
        if (!schema.has_value())
            return copy_failure<std::shared_ptr<SharedRuntime>>(schema);

#ifdef LIBSLICER_SDK_TESTING
        invoke_test_hook(RuntimeInitializationTestStage::loader_start);
#endif

        auto catalog = load_preset_catalog_with_core_locked(
            options, std::move(schema).value());
        if (!catalog.has_value())
            return copy_failure<std::shared_ptr<SharedRuntime>>(catalog);

        auto runtime = std::make_shared<SharedRuntime>();
        runtime->key = std::move(key);
        runtime->preset_catalog = std::move(catalog).value();
        return ResultAccess::success(std::move(runtime));
    } catch (const std::exception &error) {
        return ResultAccess::failure<std::shared_ptr<SharedRuntime>>(
            ErrorCode::internal, error.what(), "/runtime");
    } catch (...) {
        return ResultAccess::failure<std::shared_ptr<SharedRuntime>>(
            ErrorCode::internal, "Unknown runtime initialization failure", "/runtime");
    }
}

} // namespace

std::mutex &runtime_mutex()
{
    static std::mutex mutex;
    return mutex;
}

Result<std::shared_ptr<SharedRuntime>> acquire_shared_runtime(
    const ContextOptions &canonical_options)
{
    CoordinatorState &state = coordinator();
    const CatalogKey requested = catalog_key(canonical_options);
    std::shared_ptr<InitializationAttempt> attempt;
    bool initializer = false;

    {
        std::lock_guard<std::mutex> lock(state.registry_mutex);
        if (state.live_runtime) {
            if (!(state.live_runtime->key == requested))
                return ResultAccess::failure<std::shared_ptr<SharedRuntime>>(
                    ErrorCode::conflict,
                    "A different process runtime catalog is already active",
                    conflict_field(state.live_runtime->key, requested));
#ifdef LIBSLICER_SDK_TESTING
            ++state.stats.reuses;
#endif
            return ResultAccess::success(state.live_runtime);
        }

        if (state.attempt) {
            if (!(state.attempt->key == requested))
                return ResultAccess::failure<std::shared_ptr<SharedRuntime>>(
                    ErrorCode::conflict,
                    "A different process runtime catalog is initializing",
                    conflict_field(state.attempt->key, requested));
            attempt = state.attempt;
#ifdef LIBSLICER_SDK_TESTING
            ++state.stats.waiters;
#endif
        } else {
            attempt = std::make_shared<InitializationAttempt>(requested);
            state.attempt = attempt;
            initializer = true;
#ifdef LIBSLICER_SDK_TESTING
            ++state.stats.attempts;
            state.stats.initializing = true;
#endif
        }
    }

    if (!initializer) {
        std::unique_lock<std::mutex> lock(attempt->mutex);
        attempt->condition.wait(lock, [&attempt] { return attempt->complete; });
        return *attempt->result;
    }

    Result<std::shared_ptr<SharedRuntime>> result =
        initialize_runtime(canonical_options, requested);

    if (result.has_value()) {
#ifdef LIBSLICER_SDK_TESTING
        try {
            invoke_test_hook(RuntimeInitializationTestStage::before_publish);
        } catch (const std::exception &error) {
            result = ResultAccess::failure<std::shared_ptr<SharedRuntime>>(
                ErrorCode::internal, error.what(), "/runtime");
        } catch (...) {
            result = ResultAccess::failure<std::shared_ptr<SharedRuntime>>(
                ErrorCode::internal,
                "Unknown failure before runtime publication",
                "/runtime");
        }
#endif
    }

    if (result.has_value()) {
        std::lock_guard<std::mutex> lock(state.registry_mutex);
        std::shared_ptr<SharedRuntime> runtime = result.value();
        runtime->generation = ++state.successful_generation;
        state.live_runtime = runtime;
        if (state.attempt == attempt)
            state.attempt.reset();
#ifdef LIBSLICER_SDK_TESTING
        ++state.stats.publishes;
        state.stats.generation = runtime->generation;
        state.stats.live_runtime_identity =
            reinterpret_cast<std::uintptr_t>(runtime.get());
        state.stats.initializing = false;
        state.stats.ready = true;
#endif
    } else {
#ifdef LIBSLICER_SDK_TESTING
        invoke_test_hook(RuntimeInitializationTestStage::failure_discard);
#endif
        {
            std::lock_guard<std::mutex> lock(attempt->mutex);
            attempt->result = result;
            attempt->complete = true;
        }
        {
            std::lock_guard<std::mutex> lock(state.registry_mutex);
            if (state.attempt == attempt)
                state.attempt.reset();
#ifdef LIBSLICER_SDK_TESTING
            ++state.stats.failures;
            state.stats.initializing = false;
#endif
        }
        attempt->condition.notify_all();
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(attempt->mutex);
        attempt->result = result;
        attempt->complete = true;
    }
    attempt->condition.notify_all();
    return result;
}

#ifdef LIBSLICER_SDK_TESTING

RuntimeCoordinatorTestStats runtime_coordinator_test_stats()
{
    CoordinatorState &state = coordinator();
    std::lock_guard<std::mutex> lock(state.registry_mutex);
    RuntimeCoordinatorTestStats result = state.stats;
    result.initializing = state.attempt != nullptr;
    result.ready = state.live_runtime != nullptr;
    result.live_runtime_identity = reinterpret_cast<std::uintptr_t>(
        state.live_runtime.get());
    return result;
}

bool reset_runtime_coordinator_for_testing()
{
    CoordinatorState &state = coordinator();
    std::lock_guard<std::mutex> lock(state.registry_mutex);
    if (state.attempt || (state.live_runtime && state.live_runtime.use_count() != 1))
        return false;
    state.live_runtime.reset();
    state.successful_generation = 0;
    state.stats = {};
    return true;
}

void set_runtime_initialization_hook_for_testing(
    RuntimeInitializationTestHook hook)
{
    CoordinatorState &state = coordinator();
    std::lock_guard<std::mutex> lock(state.hook_mutex);
    state.hook = std::move(hook);
}

#endif

} // namespace libslicer::v1::detail
