include(GNUInstallDirs)

add_executable(orcaslicer-worker
    main.cpp
)

target_link_libraries(orcaslicer-worker
    PRIVATE
        libslicer::worker_runtime
)

set_target_properties(orcaslicer-worker PROPERTIES
    OUTPUT_NAME orcaslicer-worker
)

install(TARGETS orcaslicer-worker
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
