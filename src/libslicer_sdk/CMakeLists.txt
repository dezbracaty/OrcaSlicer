add_library(libslicer_sdk_v1 STATIC
    Config.cpp
    ConfigSchema.cpp
    Context.cpp
    OrcaConfigAdapter.cpp
    Preset.cpp
    Project.cpp
    Slice.cpp
    ../libslicer_worker/src/NanoSVG.cpp
)

add_library(libslicer::sdk_v1 ALIAS libslicer_sdk_v1)

target_compile_features(libslicer_sdk_v1 PUBLIC cxx_std_17)
target_include_directories(libslicer_sdk_v1
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
# A static facade does not copy objects from its private static dependencies.
# Collect that archive closure for installation so the exported SDK target can
# link it by private archive path without exporting Orca's implementation
# targets or include directories.
add_dependencies(libslicer_sdk_v1 libslic3r)
target_link_libraries(libslicer_sdk_v1 PRIVATE $<BUILD_INTERFACE:libslic3r>)

set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE "")
set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_VISITED "")

function(_libslicer_collect_static_archives _target)
    if(NOT TARGET "${_target}")
        return()
    endif()

    get_target_property(_aliased "${_target}" ALIASED_TARGET)
    if(_aliased)
        set(_target "${_aliased}")
    endif()

    get_property(_visited GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_VISITED)
    if("${_target}" IN_LIST _visited)
        return()
    endif()
    list(APPEND _visited "${_target}")
    set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_VISITED "${_visited}")

    get_target_property(_type "${_target}" TYPE)
    get_target_property(_imported "${_target}" IMPORTED)
    set(_is_static FALSE)
    if(_type STREQUAL "STATIC_LIBRARY")
        set(_is_static TRUE)
    elseif(SLIC3R_STATIC AND _imported AND
           (_type STREQUAL "UNKNOWN_LIBRARY" OR _type STREQUAL "SHARED_LIBRARY"))
        # A number of third-party packages declare their imported archive as
        # UNKNOWN/SHARED even in the all-static dependency configuration.
        set(_is_static TRUE)
    elseif(_type STREQUAL "UNKNOWN_LIBRARY")
        get_target_property(_location "${_target}" IMPORTED_LOCATION)
        get_target_property(_imported_configs "${_target}" IMPORTED_CONFIGURATIONS)
        foreach(_config IN LISTS _imported_configs)
            string(TOUPPER "${_config}" _config_upper)
            get_target_property(_config_location "${_target}" "IMPORTED_LOCATION_${_config_upper}")
            if(_config_location)
                list(APPEND _candidate_locations "${_config_location}")
            endif()
        endforeach()
        if(_location)
            list(APPEND _candidate_locations "${_location}")
        endif()
        foreach(_candidate IN LISTS _candidate_locations)
            if(_candidate MATCHES "\\.(a|lib)$")
                set(_is_static TRUE)
            endif()
        endforeach()
    endif()

    if(_is_static)
        get_property(_archives GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE)
        set(_archive_path)
        if(_imported)
            if(CMAKE_BUILD_TYPE)
                string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_config_upper)
                get_target_property(_archive_path "${_target}"
                    "IMPORTED_LOCATION_${_build_config_upper}")
            endif()
            if(NOT _archive_path)
                get_target_property(_archive_path "${_target}" IMPORTED_LOCATION)
            endif()
            if(NOT _archive_path)
                get_target_property(_archive_configs "${_target}" IMPORTED_CONFIGURATIONS)
                foreach(_archive_config IN LISTS _archive_configs)
                    string(TOUPPER "${_archive_config}" _archive_config_upper)
                    get_target_property(_archive_path "${_target}"
                        "IMPORTED_LOCATION_${_archive_config_upper}")
                    if(_archive_path)
                        break()
                    endif()
                endforeach()
            endif()
            if(_archive_path AND EXISTS "${_archive_path}")
                file(REAL_PATH "${_archive_path}" _archive_path)
            endif()
        endif()
        if(_archive_path)
            list(APPEND _archives "${_archive_path}")
        else()
            list(APPEND _archives "$<TARGET_FILE:${_target}>")
        endif()
        set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE "${_archives}")
    endif()

    get_target_property(_direct_links "${_target}" LINK_LIBRARIES)
    get_target_property(_interface_links "${_target}" INTERFACE_LINK_LIBRARIES)
    get_target_property(_imported_configs "${_target}" IMPORTED_CONFIGURATIONS)
    foreach(_config IN LISTS _imported_configs)
        string(TOUPPER "${_config}" _config_upper)
        get_target_property(_dependent_links "${_target}"
            "IMPORTED_LINK_DEPENDENT_LIBRARIES_${_config_upper}")
        if(_dependent_links)
            list(APPEND _imported_dependent_links ${_dependent_links})
        endif()
    endforeach()
    foreach(_link IN LISTS _direct_links _interface_links _imported_dependent_links)
        if(NOT _link OR _link MATCHES "^::@" OR
           _link STREQUAL "debug" OR _link STREQUAL "optimized" OR _link STREQUAL "general")
            continue()
        endif()
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _link "${_link}")
        string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _link "${_link}")
        if(TARGET "${_link}")
            _libslicer_collect_static_archives("${_link}")
        elseif(IS_ABSOLUTE "${_link}" AND _link MATCHES "\\.(a|lib)$")
            get_property(_archives GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE)
            list(APPEND _archives "${_link}")
            set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE "${_archives}")
        elseif(_link IN_LIST OCCT_LIBS)
            get_filename_component(_occt_library_hint
                "${OpenCASCADE_DIR}/../.." ABSOLUTE)
            unset(_occt_archive CACHE)
            find_library(_occt_archive NAMES "${_link}"
                HINTS ${OpenCASCADE_LIBRARY_DIR} ${OpenCASCADE_LIBRARY_DIRS}
                      "${_occt_library_hint}")
            if(_occt_archive)
                get_property(_archives GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE)
                list(APPEND _archives "${_occt_archive}")
                set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE "${_archives}")
            endif()
        endif()
    endforeach()
endfunction()

_libslicer_collect_static_archives(libslic3r)

# Some package configurations expose their static archives through plain
# library names or cross-directory link wrappers.  Resolve those implementation
# dependencies here so the installed facade remains self-contained without
# exporting any of the underlying Orca targets.
find_package(OpenCV REQUIRED core)
find_package(JPEG REQUIRED)
find_package(draco REQUIRED)
find_package(Qhull 7.2 REQUIRED)
set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_VISITED "")
foreach(_libslicer_external_target IN ITEMS
        opencv_world
        draco::draco
        JPEG::JPEG
        qhull)
    _libslicer_collect_static_archives("${_libslicer_external_target}")
endforeach()

set(_libslicer_occt_libraries
    TKXDESTEP TKSTEP TKSTEP209 TKSTEPAttr TKSTEPBase TKXCAF TKXSBase
    TKVCAF TKCAF TKLCAF TKCDF TKV3d TKService TKMesh TKBO TKPrim TKHLR
    TKShHealing TKTopAlgo TKGeomAlgo TKBRep TKGeomBase TKG3d TKG2d
    TKMath TKernel)
get_filename_component(_libslicer_occt_library_dir
    "${OpenCASCADE_DIR}/../.." ABSOLUTE)
foreach(_libslicer_occt_library IN LISTS _libslicer_occt_libraries)
    unset(_libslicer_occt_archive CACHE)
    find_library(_libslicer_occt_archive NAMES "${_libslicer_occt_library}"
        HINTS "${_libslicer_occt_library_dir}" NO_DEFAULT_PATH)
    if(NOT _libslicer_occt_archive)
        message(FATAL_ERROR
            "Unable to resolve static OCCT archive ${_libslicer_occt_library}")
    endif()
    get_property(_libslicer_archives GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE)
    list(APPEND _libslicer_archives "${_libslicer_occt_archive}")
    set_property(GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE
        "${_libslicer_archives}")
endforeach()

get_property(_libslicer_sdk_private_archives GLOBAL PROPERTY LIBSLICER_SDK_ARCHIVE_CLOSURE)
list(REMOVE_DUPLICATES _libslicer_sdk_private_archives)

# FreeType in the fetched static dependency set embeds selected zlib objects.
# On Apple use the SDK's system zlib, matching the working libslic3r link; on
# other platforms keep the standalone archive last so only missing members are
# pulled after FreeType.
set(_libslicer_sdk_zlib_archives)
foreach(_libslicer_archive IN LISTS _libslicer_sdk_private_archives)
    get_filename_component(_libslicer_archive_name "${_libslicer_archive}" NAME)
    if(_libslicer_archive_name STREQUAL "libz.a" OR
       _libslicer_archive_name STREQUAL "zlibstatic.lib")
        list(APPEND _libslicer_sdk_zlib_archives "${_libslicer_archive}")
    endif()
endforeach()
list(REMOVE_ITEM _libslicer_sdk_private_archives ${_libslicer_sdk_zlib_archives})
if(NOT APPLE)
    list(APPEND _libslicer_sdk_private_archives ${_libslicer_sdk_zlib_archives})
endif()

# Keep private dependency archives separate.  Combining every object into one
# archive breaks libraries such as FreeType that intentionally embed selected
# zlib objects; normal archive ordering resolves those symbols correctly.
set(_libslicer_sdk_private_install_dir "${CMAKE_INSTALL_LIBDIR}/libslicer/private")
if(LIBSLICER_INSTALL_SDK_V1)
install(FILES ${_libslicer_sdk_private_archives}
    DESTINATION "${_libslicer_sdk_private_install_dir}"
)
endif()

# TARGET_FILE_NAME must be evaluated while the implementation targets still
# exist.  Leaving that generator expression in the exported target would make
# find_package() consumers try to resolve private Orca targets.  Generate an
# ordered archive-name manifest and let libslicerConfig.cmake attach the
# installed paths to the one public imported target.
set(_libslicer_sdk_private_archive_names)
foreach(_archive IN LISTS _libslicer_sdk_private_archives)
    if(_archive MATCHES "^\\$<TARGET_FILE:([^>]+)>$")
        list(APPEND _libslicer_sdk_private_archive_names
            "$<TARGET_FILE_NAME:${CMAKE_MATCH_1}>")
    else()
        get_filename_component(_archive_name "${_archive}" NAME)
        list(APPEND _libslicer_sdk_private_archive_names "${_archive_name}")
    endif()
endforeach()
set(_libslicer_sdk_private_manifest
    "${CMAKE_CURRENT_BINARY_DIR}/libslicerPrivateArchives-$<CONFIG>.cmake")
file(GENERATE OUTPUT "${_libslicer_sdk_private_manifest}" CONTENT
    "set(_libslicer_private_archive_names \"$<JOIN:${_libslicer_sdk_private_archive_names},;>\")\n")
if(LIBSLICER_INSTALL_SDK_V1)
install(FILES "${_libslicer_sdk_private_manifest}"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/libslicer"
    RENAME libslicerPrivateArchives.cmake
)
endif()

if(APPLE)
    target_link_libraries(libslicer_sdk_v1 INTERFACE
        "$<INSTALL_INTERFACE:-framework AppKit>"
        "$<INSTALL_INTERFACE:-framework Foundation>"
        "$<INSTALL_INTERFACE:-framework ModelIO>"
        $<INSTALL_INTERFACE:z>
    )
elseif(WIN32)
    target_link_libraries(libslicer_sdk_v1 INTERFACE
        $<INSTALL_INTERFACE:Psapi.lib>
        $<INSTALL_INTERFACE:bcrypt.lib>
    )
else()
    target_link_libraries(libslicer_sdk_v1 INTERFACE
        $<INSTALL_INTERFACE:${CMAKE_DL_LIBS}>
        $<INSTALL_INTERFACE:m>
        $<INSTALL_INTERFACE:fontconfig>
        $<INSTALL_INTERFACE:pthread>
    )
endif()
target_compile_definitions(libslicer_sdk_v1
    PUBLIC
        LIBSLICER_PACKAGE_VERSION_MAJOR=1
        LIBSLICER_PACKAGE_VERSION_MINOR=0
)

set_target_properties(libslicer_sdk_v1 PROPERTIES
    EXPORT_NAME sdk_v1
    OUTPUT_NAME libslicer_sdk_v1
)
