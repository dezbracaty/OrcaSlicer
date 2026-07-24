#pragma once

#include <libslicer/v1/Slice.hpp>

#include "ContextInternal.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

#ifdef LIBSLICER_SDK_TESTING
#include "SliceTesting.hpp"
#endif

namespace Slic3r { class Print; }

namespace libslicer::v1::detail {

struct SliceEngineState {
    explicit SliceEngineState(std::shared_ptr<ContextState> value) : context(std::move(value)) {}
    std::shared_ptr<ContextState> context;
    std::mutex mutex;
    bool active {false};
};

struct SliceJobState {
    std::shared_ptr<SliceEngineState> engine;
    std::mutex mutex;
    std::condition_variable condition;
    bool cancel_requested {false};
    bool finished {false};
    bool callback_active {false};
    bool callback_failed {false};
    std::thread::id callback_thread;
    Slic3r::Print *print {nullptr};
    std::exception_ptr unhandled_exception;
    std::optional<Result<std::shared_ptr<const SliceResult>>> outcome;
};

} // namespace libslicer::v1::detail
