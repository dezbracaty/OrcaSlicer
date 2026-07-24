#pragma once

#ifdef LIBSLICER_SDK_TESTING

#include <chrono>
#include <cstdint>

namespace libslicer::v1::detail::testing {

enum class SliceStage {
    queued,
    preparing,
    validating,
    slicing,
    exporting
};

class StageBarrier {
public:
    explicit StageBarrier(SliceStage stage);
    ~StageBarrier();

    StageBarrier(const StageBarrier &) = delete;
    StageBarrier &operator=(const StageBarrier &) = delete;

    bool wait_until_reached(std::chrono::milliseconds timeout);
    void release();

private:
    SliceStage stage_;
    std::uint64_t generation_ {0};
};

void wait_at_stage(SliceStage stage);

} // namespace libslicer::v1::detail::testing

#endif
