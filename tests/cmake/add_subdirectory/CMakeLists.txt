cmake_minimum_required(VERSION 3.19)

project(libslicer_add_subdirectory_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBSLICER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../.. libslicer-build)

add_executable(libslicer_consumer_smoke main.cpp)
target_compile_definitions(libslicer_consumer_smoke
    PRIVATE
        TEST_LIBSLICER_RESOURCES_DIR="${CMAKE_CURRENT_LIST_DIR}/../../../resources"
)
target_link_libraries(libslicer_consumer_smoke
    PRIVATE
        libslicer::libslicer
        libslicer::worker_client
)

add_custom_target(run_libslicer_consumer_smoke
    COMMAND libslicer_consumer_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS libslicer_consumer_smoke
    USES_TERMINAL
)
