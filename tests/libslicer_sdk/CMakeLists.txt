add_executable(libslicer_sdk_tests
    test_config.cpp
    test_project_slice.cpp
)

target_compile_definitions(libslicer_sdk_v1 PRIVATE
    LIBSLICER_SDK_TESTING
)

target_link_libraries(libslicer_sdk_tests
    PRIVATE
        libslicer::sdk_v1
        libslic3r
        Catch2::Catch2WithMain
)

target_compile_definitions(libslicer_sdk_tests PRIVATE
    LIBSLICER_SDK_TESTING
    LIBSLICER_TEST_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
)
target_include_directories(libslicer_sdk_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/libslicer_sdk"
)

set_property(TARGET libslicer_sdk_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(libslicer_sdk_tests)

add_executable(libslicer_sdk_cold_gate
    libslicer_sdk_cold_gate.cpp
)
target_link_libraries(libslicer_sdk_cold_gate
    PRIVATE
        libslicer::sdk_v1
        libslic3r
        OpenSSL::Crypto
)
target_compile_definitions(libslicer_sdk_cold_gate PRIVATE
    LIBSLICER_SDK_TESTING
)
target_include_directories(libslicer_sdk_cold_gate PRIVATE
    "${PROJECT_SOURCE_DIR}/src/libslicer_sdk"
)
set_property(TARGET libslicer_sdk_cold_gate PROPERTY FOLDER "tests")

foreach(_cold_gate_run RANGE 1 3)
    set(_cold_gate_root
        "${CMAKE_CURRENT_BINARY_DIR}/cold_gate_${_cold_gate_run}")
    add_test(
        NAME libslicer_sdk_cold_gate_${_cold_gate_run}
        COMMAND libslicer_sdk_cold_gate
            --resources "${PROJECT_SOURCE_DIR}/resources"
            --data-root "${_cold_gate_root}"
            --fixture "${PROJECT_SOURCE_DIR}/../../firehorse.3mf"
            --result "${_cold_gate_root}/result.json"
    )
    set_tests_properties(libslicer_sdk_cold_gate_${_cold_gate_run}
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 60
    )
endforeach()
