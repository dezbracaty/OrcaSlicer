#include <libslicer/Library.hpp>

#include <libslic3r/libslic3r.h>

namespace libslicer {

const char* version() noexcept
{
    return Slic3r::core_version();
}

} // namespace libslicer
