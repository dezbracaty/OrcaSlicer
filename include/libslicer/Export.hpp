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
