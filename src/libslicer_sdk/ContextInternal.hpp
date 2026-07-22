#pragma once

#include <libslicer/v1/Context.hpp>

#include "PresetInternal.hpp"

#include <atomic>
#include <mutex>
#include <utility>

namespace libslicer::v1::detail {

struct ContextState {
    explicit ContextState(ContextOptions value) : options(std::move(value)) {}

    ContextOptions       options;
    std::atomic<bool>    closing {false};
    std::mutex           mutex;
    std::shared_ptr<detail::PresetCatalogState> preset_catalog;
};

} // namespace libslicer::v1::detail
