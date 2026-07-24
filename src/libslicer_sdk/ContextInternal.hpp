#pragma once

#include <libslicer/v1/Context.hpp>

#include "PresetInternal.hpp"
#include "RuntimeCoordinator.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace libslicer::v1::detail {

struct ContextState {
    ContextState(ContextOptions value, std::shared_ptr<SharedRuntime> runtime_value)
        : options(std::move(value)),
          shared_runtime(std::move(runtime_value)),
          preset_catalog(shared_runtime->preset_catalog)
    {}

    ContextOptions       options;
    std::atomic<bool>    closing {false};
    std::mutex           mutex;
    std::shared_ptr<SharedRuntime> shared_runtime;
    std::shared_ptr<detail::PresetCatalogState> preset_catalog;
};

} // namespace libslicer::v1::detail
