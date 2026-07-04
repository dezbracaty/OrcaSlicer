find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_executable(worker_preview_tests
    test_preview_artifact_producer.cpp
)

target_link_libraries(worker_preview_tests
    PRIVATE
        test_common
        libslicer::worker_runtime
        Catch2::Catch2WithMain
)

target_include_directories(worker_preview_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/libslicer_worker/src
)

orcaslicer_discover_tests(worker_preview_tests)

set(WORKER_TEST_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/run_worker_tests.py)
set(WORKER_TEST_WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/worker-test)

add_test(
    NAME worker_cli_socket_slice
    COMMAND ${Python3_EXECUTABLE} ${WORKER_TEST_SCRIPT}
        --worker $<TARGET_FILE:orcaslicer-worker>
        --source-root ${CMAKE_SOURCE_DIR}
        --work-dir ${WORKER_TEST_WORK_DIR}
)
set_tests_properties(worker_cli_socket_slice PROPERTIES
    LABELS "worker;slicing"
    TIMEOUT 180
)

add_custom_target(worker_tests
    COMMAND ${Python3_EXECUTABLE} ${WORKER_TEST_SCRIPT}
        --worker $<TARGET_FILE:orcaslicer-worker>
        --source-root ${CMAKE_SOURCE_DIR}
        --work-dir ${WORKER_TEST_WORK_DIR}
    DEPENDS orcaslicer-worker
    USES_TERMINAL
)
