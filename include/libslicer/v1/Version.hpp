#pragma once

#include <cstdint>

namespace libslicer::v1 {

inline constexpr std::uint32_t sdk_version_major = 1;
inline constexpr std::uint32_t sdk_version_minor = 0;
inline constexpr std::uint32_t sdk_version_patch = 0;

#if defined(LIBSLICER_PACKAGE_VERSION_MAJOR)
static_assert(LIBSLICER_PACKAGE_VERSION_MAJOR == sdk_version_major,
              "libslicer CMake package and public headers have different SDK major versions");
#endif
#if defined(LIBSLICER_PACKAGE_VERSION_MINOR)
static_assert(LIBSLICER_PACKAGE_VERSION_MINOR == sdk_version_minor,
              "libslicer CMake package and public headers have different SDK minor versions");
#endif

} // namespace libslicer::v1
