#pragma once

#include <mutex>

namespace libslicer::v1::detail {

inline std::mutex &runtime_mutex()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace libslicer::v1::detail
