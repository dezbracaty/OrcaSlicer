add_library(libslicer_api SHARED
    Library.cpp
)

set_target_properties(libslicer_api PROPERTIES
    EXPORT_NAME libslicer
    OUTPUT_NAME libslicer
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_compile_features(libslicer_api PUBLIC cxx_std_17)
target_compile_definitions(libslicer_api PRIVATE LIBSLICER_BUILDING_LIBRARY)
target_include_directories(libslicer_api
    PUBLIC
        $<BUILD_INTERFACE:${LIBSLICER_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/../libslic3r"
)
target_link_libraries(libslicer_api PRIVATE libslic3r)
