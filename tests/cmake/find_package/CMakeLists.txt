cmake_minimum_required(VERSION 3.19)

project(libslicer_find_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(libslicer CONFIG REQUIRED)

if(NOT TARGET libslicer::sdk_v1)
    message(FATAL_ERROR "libslicer::sdk_v1 target is missing")
endif()
foreach(_private_target IN ITEMS libslicer::libslicer libslicer::config_sdk
                                 libslicer::worker libslicer::orcaslicer_worker)
    if(TARGET "${_private_target}")
        message(FATAL_ERROR "Installed package exposes private target ${_private_target}")
    endif()
endforeach()
get_target_property(_libslicer_sdk_links libslicer::sdk_v1 INTERFACE_LINK_LIBRARIES)
get_target_property(_libslicer_sdk_includes libslicer::sdk_v1 INTERFACE_INCLUDE_DIRECTORIES)
foreach(_link IN LISTS _libslicer_sdk_links)
    # Relocatable paths under lib/libslicer/private are implementation archive
    # files, not exported CMake targets.  Only a target-valued public item is a
    # package-surface leak.
    if(_link MATCHES "^(libslic3r|libslicer_worker|orcaslicer_worker|libslicer::libslicer)$" OR
       _link MATCHES "^\\$<INSTALL_INTERFACE:(libslic3r|libslicer_worker|orcaslicer_worker|libslicer::libslicer)>$")
        message(FATAL_ERROR "Installed SDK publicly leaks internal target ${_link}")
    endif()
endforeach()
foreach(_include IN LISTS _libslicer_sdk_includes)
    if(_include MATCHES "(/|^)(src/libslic3r|src/libslicer_sdk|deps_src)(/|$)")
        message(FATAL_ERROR "Installed SDK leaks internal include directory ${_include}")
    endif()
endforeach()

add_executable(libslicer_find_package_smoke main.cpp)
target_link_libraries(libslicer_find_package_smoke
    PRIVATE
        libslicer::sdk_v1
)

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
    set(_target "libslicer_header_${_stem}_find_package")
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

add_custom_target(run_libslicer_find_package_smoke
    COMMAND libslicer_find_package_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS libslicer_find_package_smoke libslicer_public_headers_smoke
    USES_TERMINAL
)
