include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_library(libslicer_preview_protocol INTERFACE)

target_compile_features(libslicer_preview_protocol INTERFACE cxx_std_17)

target_include_directories(libslicer_preview_protocol
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${LIBSLICER_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(libslicer_preview_protocol PROPERTIES
    EXPORT_NAME preview_protocol
)

add_library(libslicer_worker_client STATIC
    src/WorkerClient.cpp
    src/WorkerProtocol.cpp
)

target_compile_features(libslicer_worker_client PUBLIC cxx_std_17)

target_include_directories(libslicer_worker_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../deps_src
)

target_link_libraries(libslicer_worker_client
    PUBLIC
        Threads::Threads
)

target_link_libraries(libslicer_preview_protocol
    INTERFACE
        Eigen3::Eigen
)

set_target_properties(libslicer_worker_client PROPERTIES
    EXPORT_NAME worker_client
)

add_library(libslicer_worker_runtime STATIC
    src/NanoSVG.cpp
    src/SliceRunner.cpp
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
        libslicer_worker_client
        nlohmann_json
    PRIVATE
        nanosvg
)

add_library(libslicer::worker_client ALIAS libslicer_worker_client)
add_library(libslicer::worker_runtime ALIAS libslicer_worker_runtime)
add_library(libslicer::preview_protocol ALIAS libslicer_preview_protocol)

install(FILES
    include/libslicer_worker/PreviewBinary.hpp
    include/libslicer_worker/PreviewProtocol.hpp
    include/libslicer_worker/PreviewReader.hpp
    include/libslicer_worker/PreviewTypes.hpp
    include/libslicer_worker/SliceJob.hpp
    include/libslicer_worker/WorkerClient.hpp
    include/libslicer_worker/WorkerEvent.hpp
    include/libslicer_worker/WorkerProtocol.hpp
    include/libslicer_worker/WorkerTypes.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libslicer_worker
)

install(TARGETS libslicer_preview_protocol
    EXPORT libslicerTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS libslicer_worker_client
    EXPORT libslicerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
