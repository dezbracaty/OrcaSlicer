cmake_minimum_required(VERSION 3.19)

project(libslicer_find_package_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(libslicer CONFIG REQUIRED)

if(NOT TARGET libslicer::worker_client)
    message(FATAL_ERROR "libslicer::worker_client target is missing")
endif()

if(NOT TARGET libslicer::preview_protocol)
    message(FATAL_ERROR "libslicer::preview_protocol target is missing")
endif()

if(NOT TARGET libslicer::orcaslicer_worker)
    message(FATAL_ERROR "libslicer::orcaslicer_worker executable target is missing")
endif()

if(NOT LIBSLICER_ORCASLICER_WORKER_EXECUTABLE)
    message(FATAL_ERROR "LIBSLICER_ORCASLICER_WORKER_EXECUTABLE is missing")
endif()

add_executable(libslicer_find_package_smoke main.cpp)
target_compile_definitions(libslicer_find_package_smoke
    PRIVATE
        LIBSLICER_WORKER_EXE="${LIBSLICER_ORCASLICER_WORKER_EXECUTABLE}"
)
target_link_libraries(libslicer_find_package_smoke
    PRIVATE
        libslicer::preview_protocol
        libslicer::worker_client
)

add_custom_target(run_libslicer_find_package_smoke
    COMMAND libslicer_find_package_smoke
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS libslicer_find_package_smoke
    USES_TERMINAL
)
