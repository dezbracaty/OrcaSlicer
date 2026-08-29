add_executable(libslicer_api_tests
    test_config_api.cpp
)

target_link_libraries(libslicer_api_tests
    PRIVATE
        libslicer::libslicer
        Catch2::Catch2WithMain
        miniz
        PNG::PNG
)

target_include_directories(libslicer_api_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(libslicer_api_tests PRIVATE
    LIBSLICER_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    LIBSLICER_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
)
set_property(TARGET libslicer_api_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(libslicer_api_tests)
