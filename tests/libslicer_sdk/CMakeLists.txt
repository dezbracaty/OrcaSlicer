add_executable(libslicer_sdk_tests
    test_config.cpp
    test_project_slice.cpp
)

target_link_libraries(libslicer_sdk_tests
    PRIVATE
        libslicer::sdk_v1
        Catch2::Catch2WithMain
)

target_compile_definitions(libslicer_sdk_tests PRIVATE
    LIBSLICER_TEST_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
)

set_property(TARGET libslicer_sdk_tests PROPERTY FOLDER "tests")
orcaslicer_discover_tests(libslicer_sdk_tests)
