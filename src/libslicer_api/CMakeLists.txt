add_library(libslicer_api_objects OBJECT
    Config.cpp
    Library.cpp
    Plater.cpp
    OrcaCalibrationApi.cpp
)

# Reuse these exact production objects in the isolated reference-test runtime.
# The shipped shared library never contains the test runtime's scheduler hook.
set_target_properties(libslicer_api_objects PROPERTIES
    POSITION_INDEPENDENT_CODE YES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(libslicer_api_objects PRIVATE cxx_std_17)
target_compile_definitions(libslicer_api_objects PRIVATE
    LIBSLICER_BUILDING_LIBRARY
    LIBSLICER_DEFAULT_RESOURCE_DIR="${CMAKE_INSTALL_FULL_DATADIR}/libslicer"
)
target_include_directories(libslicer_api_objects PRIVATE
    "${LIBSLICER_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/../libslic3r"
)
target_link_libraries(libslicer_api_objects PRIVATE libslic3r)

add_library(libslicer_api SHARED $<TARGET_OBJECTS:libslicer_api_objects>)

set_target_properties(libslicer_api PROPERTIES
    EXPORT_NAME libslicer
    OUTPUT_NAME libslicer
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

target_compile_features(libslicer_api PUBLIC cxx_std_17)
target_include_directories(libslicer_api
    PUBLIC
        $<BUILD_INTERFACE:${LIBSLICER_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/../libslic3r"
)
target_link_libraries(libslicer_api PRIVATE libslic3r)

# libslicer is assembled from static third-party archives. On Darwin, global
# symbols pulled from those archives would otherwise remain interposable and
# may bind to a different version loaded by the host process (notably OCCT).
# Export only the public libslicer namespace so all bundled dependencies stay
# private to this dylib.
if(APPLE)
    target_link_options(libslicer_api PRIVATE
        "LINKER:-exported_symbol,__Z*9libslicer*")
endif()
