add_subdirectory(catch2)

# The top-level project defines _UNICODE globally, which makes Catch2's bundled
# main emit wmain instead of main. Test executables link with the default
# /SUBSYSTEM:CONSOLE entry point (mainCRTStartup, which calls main), so force
# Catch2 to emit main to keep the CRT entry point happy.
if(MSVC)
    target_compile_definitions(Catch2WithMain PRIVATE DO_NOT_USE_WMAIN)
endif()

set(TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)
file(TO_NATIVE_PATH "${TEST_DATA_DIR}" TEST_DATA_DIR)

include(CTest)
include(Catch)

set(CATCH_EXTRA_ARGS "" CACHE STRING "Extra arguments for catch2 test suites.") # Unknown if this still works and/or should be replaced with something else.

add_library(test_common INTERFACE)
target_compile_definitions(test_common INTERFACE TEST_DATA_DIR=R"\(${TEST_DATA_DIR}\)" CATCH_CONFIG_FAST_COMPILE)
target_include_directories(test_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if (APPLE)
    target_link_libraries(test_common INTERFACE "-liconv -framework IOKit" "-framework CoreFoundation" -lc++)
endif()

# Deploy linked DLLs, including the API's private dependencies, before Catch2
# discovery. Vendored GMP has no CMake DLL target; include its runtime explicitly.
function(orcaslicer_copy_test_dlls TARGET)
    if (NOT WIN32)
        return()
    endif()
    set(_runtime_dlls "$<REMOVE_DUPLICATES:$<TARGET_RUNTIME_DLLS:${TARGET}>;$<TARGET_RUNTIME_DLLS:libslicer_api>>")
    # An API DLL implementation can change without changing its import library.
    get_target_property(_links ${TARGET} LINK_LIBRARIES)
    if("libslicer::libslicer" IN_LIST _links)
        set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:libslicer_api>")
    endif()
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GMP_ROOT}/bin/libgmp-10.dll"
            "${_runtime_dlls}"
            "$<TARGET_FILE_DIR:${TARGET}>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endfunction()

# Register Catch2 tags as CTest labels so `ctest -L`/`-LE` can filter by tag.
function(orcaslicer_discover_tests TARGET)
    catch_discover_tests(${TARGET} ADD_TAGS_AS_LABELS)
endfunction()

add_subdirectory(libslic3r)
add_subdirectory(libslicer_api)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/add_subdirectory/run_smoke.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/add_subdirectory/run_smoke.cmake
    @ONLY
)

add_test(
    NAME cmake_add_subdirectory_smoke
    COMMAND ${CMAKE_COMMAND}
        "-DLIBSLICER_SMOKE_CONFIG=$<CONFIG>"
        -P ${CMAKE_CURRENT_BINARY_DIR}/cmake/add_subdirectory/run_smoke.cmake
)
set_tests_properties(cmake_add_subdirectory_smoke PROPERTIES LABELS "CMake")
