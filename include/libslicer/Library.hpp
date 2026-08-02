#pragma once

#if defined(_WIN32)
#  if defined(LIBSLICER_BUILDING_LIBRARY)
#    define LIBSLICER_API __declspec(dllexport)
#  else
#    define LIBSLICER_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define LIBSLICER_API __attribute__((visibility("default")))
#else
#  define LIBSLICER_API
#endif

namespace libslicer {

// Stable facade entry point. Future CuraEngine-style capabilities are exposed
// from this namespace without leaking OrcaSlicer's internal dependency graph.
LIBSLICER_API const char* version() noexcept;

} // namespace libslicer
