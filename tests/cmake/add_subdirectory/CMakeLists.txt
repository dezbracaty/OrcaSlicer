cmake_minimum_required(VERSION 3.19)

project(libslicer_add_subdirectory_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBSLICER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../.. libslicer-build)

add_executable(libslicer_consumer_smoke main.cpp)
target_link_libraries(libslicer_consumer_smoke
    PRIVATE
        libslicer::sdk_v1
)

get_target_property(_libslicer_sdk_links libslicer::sdk_v1 INTERFACE_LINK_LIBRARIES)
get_target_property(_libslicer_sdk_includes libslicer::sdk_v1 INTERFACE_INCLUDE_DIRECTORIES)
foreach(_link IN LISTS _libslicer_sdk_links)
    # LINK_ONLY/BUILD_INTERFACE dependencies are the private static-link
    # closure, not public SDK targets.  Reject only an unwrapped public target
    # or an internal target deliberately placed in INSTALL_INTERFACE.
    if(_link MATCHES "^(libslic3r|libslicer_worker|orcaslicer_worker|libslicer::libslicer)$" OR
       _link MATCHES "^\\$<INSTALL_INTERFACE:(libslic3r|libslicer_worker|orcaslicer_worker|libslicer::libslicer)>$")
        message(FATAL_ERROR "libslicer::sdk_v1 publicly leaks internal target ${_link}")
    endif()
endforeach()
foreach(_include IN LISTS _libslicer_sdk_includes)
    if(_include MATCHES "(/|^)(src/libslic3r|src/libslicer_sdk|deps_src)(/|$)")
        message(FATAL_ERROR "libslicer::sdk_v1 leaks internal include directory ${_include}")
    endif()
endforeach()

set(_libslicer_public_headers
    Config.hpp
    ConfigSchema.hpp
    Context.hpp
    Preset.hpp
    Project.hpp
    Result.hpp
    Slice.hpp
    Version.hpp
    json.hpp
)
set(_libslicer_header_targets)
foreach(_header IN LISTS _libslicer_public_headers)
    string(REPLACE ".hpp" "" _stem "${_header}")
    string(TOLOWER "${_stem}" _stem)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/header_${_stem}.cpp")
    file(WRITE "${_source}"
        "#include <libslicer/v1/${_header}>\nint libslicer_header_${_stem}() { return 0; }\n")
    set(_target "libslicer_header_${_stem}_add_subdirectory")
    add_library(${_target} OBJECT "${_source}")
    target_include_directories(${_target} PRIVATE
        $<TARGET_PROPERTY:libslicer::sdk_v1,INTERFACE_INCLUDE_DIRECTORIES>
    )
    target_compile_features(${_target} PRIVATE cxx_std_17)
    list(APPEND _libslicer_header_targets ${_target})
endforeach()

add_custom_target(libslicer_public_headers_smoke
    DEPENDS ${_libslicer_header_targets}
)

add_custom_target(run_libslicer_consumer_smoke
    COMMAND libslicer_consumer_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS libslicer_consumer_smoke libslicer_public_headers_smoke
    USES_TERMINAL
)
