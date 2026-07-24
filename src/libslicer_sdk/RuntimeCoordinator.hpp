#pragma once

#include <libslicer/v1/Context.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#ifdef LIBSLICER_SDK_TESTING
#include <functional>
#endif

namespace libslicer::v1::detail {

struct PresetCatalogState;

struct CatalogKey {
    std::filesystem::path              resources_dir;
    std::filesystem::path              data_dir;
    std::vector<std::filesystem::path> preset_dirs;

    friend bool operator==(const CatalogKey &lhs, const CatalogKey &rhs) noexcept
    {
        return lhs.resources_dir == rhs.resources_dir &&
               lhs.data_dir == rhs.data_dir &&
               lhs.preset_dirs == rhs.preset_dirs;
    }
};

struct SharedRuntime {
    CatalogKey                              key;
    std::shared_ptr<PresetCatalogState>     preset_catalog;
    std::uint64_t                           generation {0};
};

std::mutex &runtime_mutex();

Result<std::shared_ptr<SharedRuntime>> acquire_shared_runtime(
    const ContextOptions &canonical_options);

#ifdef LIBSLICER_SDK_TESTING

enum class RuntimeInitializationTestStage {
    loader_start,
    before_publish,
    failure_discard
};

struct RuntimeCoordinatorTestStats {
    std::uint64_t attempts {0};
    std::uint64_t loader_invocations {0};
    std::uint64_t publishes {0};
    std::uint64_t failures {0};
    std::uint64_t reuses {0};
    std::uint64_t waiters {0};
    std::uint64_t generation {0};
    std::uintptr_t live_runtime_identity {0};
    bool initializing {false};
    bool ready {false};
};

using RuntimeInitializationTestHook =
    std::function<void(RuntimeInitializationTestStage)>;

RuntimeCoordinatorTestStats runtime_coordinator_test_stats();
bool reset_runtime_coordinator_for_testing();
void set_runtime_initialization_hook_for_testing(
    RuntimeInitializationTestHook hook);

#endif

} // namespace libslicer::v1::detail
