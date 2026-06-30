cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ORCA_UNIVERSAL_SOURCE_DIR OR ORCA_UNIVERSAL_SOURCE_DIR STREQUAL "")
    get_filename_component(ORCA_UNIVERSAL_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

if(NOT DEFINED ORCA_UNIVERSAL_ARM64_APP OR ORCA_UNIVERSAL_ARM64_APP STREQUAL "")
    set(ORCA_UNIVERSAL_ARM64_APP "${ORCA_UNIVERSAL_SOURCE_DIR}/build-release/arm64/OrcaSlicer/OrcaSlicer.app")
endif()

if(NOT DEFINED ORCA_UNIVERSAL_X86_64_APP OR ORCA_UNIVERSAL_X86_64_APP STREQUAL "")
    set(ORCA_UNIVERSAL_X86_64_APP "${ORCA_UNIVERSAL_SOURCE_DIR}/build-release/x86_64/OrcaSlicer/OrcaSlicer.app")
endif()

if(NOT DEFINED ORCA_UNIVERSAL_OUTPUT_DIR OR ORCA_UNIVERSAL_OUTPUT_DIR STREQUAL "")
    set(ORCA_UNIVERSAL_OUTPUT_DIR "${ORCA_UNIVERSAL_SOURCE_DIR}/build-release/universal/OrcaSlicer")
endif()

find_program(LIPO_EXECUTABLE lipo REQUIRED)
find_program(FILE_EXECUTABLE file REQUIRED)

function(_orca_lipo_dir universal_dir x86_dir)
    file(GLOB_RECURSE _universal_files LIST_DIRECTORIES FALSE "${universal_dir}/*")
    foreach(_universal_file IN LISTS _universal_files)
        execute_process(
            COMMAND "${FILE_EXECUTABLE}" "${_universal_file}"
            OUTPUT_VARIABLE _file_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )

        if(NOT _file_output MATCHES "Mach-O")
            continue()
        endif()

        file(RELATIVE_PATH _relative_path "${universal_dir}" "${_universal_file}")
        set(_x86_file "${x86_dir}/${_relative_path}")
        if(EXISTS "${_x86_file}")
            message(STATUS "lipo: ${_relative_path}")
            execute_process(
                COMMAND "${LIPO_EXECUTABLE}" -create "${_universal_file}" "${_x86_file}" -output "${_universal_file}.tmp"
                RESULT_VARIABLE _lipo_result
            )
            if(NOT _lipo_result EQUAL 0)
                message(FATAL_ERROR "lipo failed for ${_relative_path}")
            endif()
            file(RENAME "${_universal_file}.tmp" "${_universal_file}")
        else()
            message(WARNING "No x86_64 counterpart for ${_relative_path}; keeping arm64 only")
        endif()
    endforeach()
endfunction()

function(_orca_create_universal_bundle bundle_name arm64_app x86_64_app required)
    if(NOT EXISTS "${arm64_app}")
        if(required)
            message(FATAL_ERROR "Missing arm64 bundle: ${arm64_app}")
        endif()
        return()
    endif()

    if(NOT EXISTS "${x86_64_app}")
        if(required)
            message(FATAL_ERROR "Missing x86_64 bundle: ${x86_64_app}")
        endif()
        return()
    endif()

    set(_output_app "${ORCA_UNIVERSAL_OUTPUT_DIR}/${bundle_name}")
    file(REMOVE_RECURSE "${_output_app}")
    file(MAKE_DIRECTORY "${ORCA_UNIVERSAL_OUTPUT_DIR}")
    file(COPY "${arm64_app}" DESTINATION "${ORCA_UNIVERSAL_OUTPUT_DIR}")
    _orca_lipo_dir("${_output_app}" "${x86_64_app}")
    message(STATUS "Created universal bundle: ${_output_app}")
endfunction()

_orca_create_universal_bundle("OrcaSlicer.app" "${ORCA_UNIVERSAL_ARM64_APP}" "${ORCA_UNIVERSAL_X86_64_APP}" TRUE)

get_filename_component(_arm64_stage_dir "${ORCA_UNIVERSAL_ARM64_APP}" DIRECTORY)
get_filename_component(_x86_64_stage_dir "${ORCA_UNIVERSAL_X86_64_APP}" DIRECTORY)
_orca_create_universal_bundle(
    "OrcaSlicer_profile_validator.app"
    "${_arm64_stage_dir}/OrcaSlicer_profile_validator.app"
    "${_x86_64_stage_dir}/OrcaSlicer_profile_validator.app"
    FALSE
)
