find_package(Python3 COMPONENTS Interpreter REQUIRED)

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
