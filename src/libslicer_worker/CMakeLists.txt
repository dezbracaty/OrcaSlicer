include(GNUInstallDirs)

add_library(libslicer_worker_runtime STATIC
    src/NanoSVG.cpp
    src/SliceRunner.cpp
    src/WorkerClient.cpp
    src/WorkerProtocol.cpp
    src/WorkerServer.cpp
)

target_compile_features(libslicer_worker_runtime PUBLIC cxx_std_17)

target_include_directories(libslicer_worker_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(libslicer_worker_runtime
    PUBLIC
        libslicer::libslicer
        nlohmann_json
    PRIVATE
        nanosvg
)

add_library(libslicer::worker_runtime ALIAS libslicer_worker_runtime)
add_library(libslicer::worker_client ALIAS libslicer_worker_runtime)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS libslicer_worker_runtime
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
