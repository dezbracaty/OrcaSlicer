#include "libslic3r.h"

double SCALING_FACTOR = SCALING_FACTOR_INTERNAL;

namespace Slic3r {

const char* core_version() noexcept
{
    return SoftFever_VERSION;
}

} // namespace Slic3r
