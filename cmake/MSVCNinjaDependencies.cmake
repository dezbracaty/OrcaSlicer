include_guard(GLOBAL)

# Ninja parses /showIncludes output to maintain header dependencies. Probe the
# compiler's actual byte prefix because localized MSVC installations may ignore
# VSLANG and older CMake versions can decode UTF-8 diagnostics as the ANSI code
# page. Apply this only to standalone libslicer builds; an embedding project is
# responsible for its own compiler launcher and dependency configuration.
if(MSVC AND CMAKE_GENERATOR MATCHES "^Ninja" AND
   CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/MSVCNinjaDependencies")
    file(MAKE_DIRECTORY "${_probe_dir}")
    file(WRITE "${_probe_dir}/libslicer_dependency_probe.h" "#pragma once\n")
    file(WRITE "${_probe_dir}/probe.cpp"
        "#include \"libslicer_dependency_probe.h\"\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=VS_UNICODE_OUTPUT VSLANG=1033
            "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
            "${_probe_dir}/probe.cpp" "/Fo${_probe_dir}/probe.obj"
        RESULT_VARIABLE _probe_result OUTPUT_VARIABLE _probe_output
        ERROR_VARIABLE _probe_error ENCODING NONE)
    if(NOT _probe_result EQUAL 0)
        message(FATAL_ERROR
            "MSVC dependency probe failed: ${_probe_output}${_probe_error}")
    endif()
    string(REGEX MATCH
        "(^|\n)([^\r\n]*: +)[A-Za-z]:[^\r\n]*libslicer_dependency_probe\\.h"
        _probe_match "${_probe_output}")
    if(NOT _probe_match)
        message(FATAL_ERROR
            "Cannot detect MSVC /showIncludes prefix: ${_probe_output}")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
    foreach(_language C CXX)
        set(CMAKE_${_language}_CL_SHOWINCLUDES_PREFIX
            "${CMAKE_CL_SHOWINCLUDES_PREFIX}")
        set(_launcher
            "${CMAKE_COMMAND};-E;env;--unset=VS_UNICODE_OUTPUT;VSLANG=1033")
        if(CMAKE_${_language}_COMPILER_LAUNCHER)
            list(APPEND _launcher ${CMAKE_${_language}_COMPILER_LAUNCHER})
        endif()
        set(CMAKE_${_language}_COMPILER_LAUNCHER "${_launcher}")
    endforeach()
endif()
