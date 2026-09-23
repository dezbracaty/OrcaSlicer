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

# Fixed 4xiao contract: every layer 4..24 must have exactly one complete outer
# contour. Mandatory acceptance gate for fiber-contour changes: both tests must
# pass before declaring the result accepted. Unit tests alone are insufficient.
# Known slicer failures stay red; do not use WILL_FAIL, SKIP, or weaken the fixed
# expectations to accommodate planner output.
add_executable(fiber_4xiao_acceptance fiber_4xiao_acceptance.cpp)
target_compile_features(fiber_4xiao_acceptance PRIVATE cxx_std_17)
target_link_libraries(fiber_4xiao_acceptance PRIVATE libslicer::libslicer nlohmann_json)
target_include_directories(fiber_4xiao_acceptance PRIVATE ${CMAKE_SOURCE_DIR}/deps_src)
target_compile_definitions(fiber_4xiao_acceptance PRIVATE
    LIBSLICER_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    LIBSLICER_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    FIBER_ACCEPTANCE_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/4xiao-acceptance")
add_test(NAME Fiber4xiaoAcceptance COMMAND fiber_4xiao_acceptance)
set_tests_properties(Fiber4xiaoAcceptance PROPERTIES LABELS "fiber_acceptance" TIMEOUT 120 RUN_SERIAL TRUE)
add_test(NAME Fiber4xiaoAcceptanceChecker COMMAND fiber_4xiao_acceptance --self-test)
set_tests_properties(Fiber4xiaoAcceptanceChecker PROPERTIES LABELS "fiber_acceptance" TIMEOUT 10)

# Additional current-settings regression; the fixed gate above stays unchanged.
add_test(NAME Fiber4xiaoClosedOuterRegression COMMAND fiber_4xiao_acceptance --closed-outer-regression)
set_tests_properties(Fiber4xiaoClosedOuterRegression PROPERTIES LABELS "fiber_closed_outer" TIMEOUT 120 RUN_SERIAL TRUE)
