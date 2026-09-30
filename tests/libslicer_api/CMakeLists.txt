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

target_include_directories(libslicer_api_tests PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/deps_src)
target_compile_definitions(libslicer_api_tests PRIVATE
    LIBSLICER_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    LIBSLICER_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
)
set_property(TARGET libslicer_api_tests PROPERTY FOLDER "tests")

orcaslicer_copy_test_dlls()
orcaslicer_discover_tests(libslicer_api_tests)

# Shared 4xiao model runner for the closed-outer, infill-gap, and rounded-path regressions.
add_executable(fiber_4xiao_acceptance fiber_4xiao_acceptance.cpp)
target_compile_features(fiber_4xiao_acceptance PRIVATE cxx_std_17)
target_link_libraries(fiber_4xiao_acceptance PRIVATE libslicer::libslicer libslic3r nlohmann_json)
target_include_directories(fiber_4xiao_acceptance PRIVATE ${CMAKE_SOURCE_DIR}/deps_src)
target_compile_definitions(fiber_4xiao_acceptance PRIVATE
    LIBSLICER_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    LIBSLICER_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    FIBER_ACCEPTANCE_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/4xiao-acceptance")
add_test(NAME Fiber4xiaoClosedOuterChecker COMMAND fiber_4xiao_acceptance --self-test)
set_tests_properties(Fiber4xiaoClosedOuterChecker PROPERTIES LABELS "fiber_closed_outer" TIMEOUT 10)

# Current-settings regression for the closed outer contour.
add_test(NAME Fiber4xiaoClosedOuterRegression COMMAND fiber_4xiao_acceptance --closed-outer-regression)
set_tests_properties(Fiber4xiaoClosedOuterRegression PROPERTIES LABELS "fiber_closed_outer" TIMEOUT 120 RUN_SERIAL TRUE)

# Independent geometry check of the complete model; plastic leftovers remain enabled.
add_test(NAME Fiber4xiaoInfillGap COMMAND fiber_4xiao_acceptance --infill-gap)
add_test(NAME Fiber4xiaoInfillGapCurrent COMMAND fiber_4xiao_acceptance --infill-gap-current)
add_test(NAME Fiber4xiaoInfillGapChecker COMMAND fiber_4xiao_acceptance --infill-gap-self-test)
set_tests_properties(Fiber4xiaoInfillGap Fiber4xiaoInfillGapCurrent PROPERTIES LABELS "fiber_infill_gap" TIMEOUT 180 RUN_SERIAL TRUE)
set_tests_properties(Fiber4xiaoInfillGapChecker PROPERTIES LABELS "fiber_infill_gap" TIMEOUT 20)

# Both production-model fixtures must slice successfully with every supported
# contour count. Keep these registered unconditionally so CI cannot skip them.
add_test(NAME FiberRoundedChecker COMMAND fiber_4xiao_acceptance --rounded-self-test)
set_tests_properties(FiberRoundedChecker PROPERTIES LABELS "fiber_rounded" TIMEOUT 20)
set(FIBER_TEST_MODELS_DIR "${CMAKE_SOURCE_DIR}/models/continuous_fiber")
foreach(_count IN ITEMS 1 2 3 4)
    add_test(NAME Fiber4xiaoRounded${_count} COMMAND fiber_4xiao_acceptance --rounded-model
        "${FIBER_TEST_MODELS_DIR}/4xiao.stl" 32 ${_count}
        "${CMAKE_CURRENT_BINARY_DIR}/4xiao-acceptance/rounded-${_count}")
    set_tests_properties(Fiber4xiaoRounded${_count} PROPERTIES LABELS "fiber_rounded" TIMEOUT 180 RUN_SERIAL TRUE)
    add_test(NAME Fiber7xiaoRounded${_count} COMMAND fiber_4xiao_acceptance --rounded-model
        "${FIBER_TEST_MODELS_DIR}/7xiao.stl" 212 ${_count}
        "${CMAKE_CURRENT_BINARY_DIR}/7xiao-acceptance/rounded-${_count}")
    set_tests_properties(Fiber7xiaoRounded${_count} PROPERTIES LABELS "fiber_rounded" TIMEOUT 240 RUN_SERIAL TRUE)
endforeach()

add_test(NAME FiberBracketResinSlice COMMAND fiber_4xiao_acceptance --resin-model
    "${CMAKE_SOURCE_DIR}/tests/data/continuous_fiber/bracket1"
    "${CMAKE_CURRENT_BINARY_DIR}/bracket-resin")
set_tests_properties(FiberBracketResinSlice PROPERTIES LABELS "fiber_resin" TIMEOUT 240 RUN_SERIAL TRUE FIXTURES_SETUP bracket_resin)
# The independent final-G-code oracle uses GEOS via Shapely, not slicer geometry.
set(FIBER_GEOMETRY_PYTHON "" CACHE FILEPATH "Python with Shapely >= 2 for independent fiber/resin regression")
if(FIBER_GEOMETRY_PYTHON)
    add_test(NAME FiberResinChecker COMMAND "${FIBER_GEOMETRY_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/check_fiber_resin.py" --self-test)
    add_test(NAME FiberBracketResinGeometry COMMAND "${FIBER_GEOMETRY_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/check_fiber_resin.py"
        "${CMAKE_CURRENT_BINARY_DIR}/bracket-resin" --expected-layers 38)
    set_tests_properties(FiberBracketResinGeometry PROPERTIES LABELS "fiber_resin" TIMEOUT 240 FIXTURES_REQUIRED bracket_resin)
endif()

foreach(_mode IN ITEMS contour_only infill_only no_fiber sparse_resin resin_off zero_radius concentric_resin)
    add_test(NAME FiberBracket_${_mode} COMMAND fiber_4xiao_acceptance --resin-model
        "${CMAKE_SOURCE_DIR}/tests/data/continuous_fiber/bracket1"
        "${CMAKE_CURRENT_BINARY_DIR}/bracket-${_mode}"
        "${CMAKE_SOURCE_DIR}/tests/data/continuous_fiber/bracket1/${_mode}.json")
    set_tests_properties(FiberBracket_${_mode} PROPERTIES LABELS "fiber_resin_matrix" TIMEOUT 240 RUN_SERIAL TRUE FIXTURES_SETUP bracket_${_mode})
    if(FIBER_GEOMETRY_PYTHON)
        add_test(NAME FiberBracket_${_mode}_Geometry COMMAND "${FIBER_GEOMETRY_PYTHON}"
            "${CMAKE_CURRENT_SOURCE_DIR}/check_fiber_resin.py"
            "${CMAKE_CURRENT_BINARY_DIR}/bracket-${_mode}" --expected-layers 38)
        set_tests_properties(FiberBracket_${_mode}_Geometry PROPERTIES LABELS "fiber_resin_matrix" TIMEOUT 240 FIXTURES_REQUIRED bracket_${_mode})
    endif()
endforeach()
