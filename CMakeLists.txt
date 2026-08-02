if(${CMAKE_VERSION} VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.13 CACHE STRING "" FORCE)
endif()

cmake_minimum_required(VERSION 3.13)

project(libslicer LANGUAGES C CXX)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(LIBSLICER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(LIBSLICER_IS_TOP_LEVEL ON)
else()
    set(LIBSLICER_IS_TOP_LEVEL OFF)
endif()

include("version.inc")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.3" CACHE STRING "Minimum macOS deployment version" FORCE)
endif ()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

if (NOT DEFINED CMAKE_FETCH_CACHE OR CMAKE_FETCH_CACHE STREQUAL "")
    if (DEFINED ENV{CMAKE_FETCH_CACHE} AND NOT "$ENV{CMAKE_FETCH_CACHE}" STREQUAL "")
        set(_default_fetch_cache "$ENV{CMAKE_FETCH_CACHE}")
    elseif (WIN32)
        set(_default_fetch_cache "$ENV{USERPROFILE}/.cmake_fetch_cache")
    else ()
        set(_default_fetch_cache "$ENV{HOME}/.cmake_fetch_cache")
    endif ()
    set(CMAKE_FETCH_CACHE "${_default_fetch_cache}" CACHE PATH "Cache directory for fetched third-party dependencies")
    unset(_default_fetch_cache)
endif ()
file(TO_CMAKE_PATH "${CMAKE_FETCH_CACHE}" CMAKE_FETCH_CACHE)
set(ENV{CMAKE_FETCH_CACHE} "${CMAKE_FETCH_CACHE}")
message(STATUS "Fetch cache directory: ${CMAKE_FETCH_CACHE}")

set(ORCA_DEPS_INSTALL_PATHS "" CACHE INTERNAL "Per-dep install roots")
include("${CMAKE_CURRENT_SOURCE_DIR}/deps/fetch_deps.cmake")

find_package(Git)
if(DEFINED ENV{git_commit_hash} AND NOT "$ENV{git_commit_hash}" STREQUAL "")
    if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short "$ENV{git_commit_hash}"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    else()
        string(SUBSTRING "$ENV{git_commit_hash}" 0 7 GIT_COMMIT_HASH)
    endif()
    add_compile_definitions("GIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
elseif(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} log -1 --format=%h
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    add_compile_definitions("GIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")
endif()

if (DEFINED BBL_RELEASE_TO_PUBLIC)
    add_compile_definitions("BBL_RELEASE_TO_PUBLIC=${BBL_RELEASE_TO_PUBLIC}")
else ()
    add_compile_definitions("BBL_RELEASE_TO_PUBLIC=$<CONFIG:Release>")
endif ()

if(DEFINED ENV{SLIC3R_STATIC})
    set(SLIC3R_STATIC_INITIAL $ENV{SLIC3R_STATIC})
else()
    set(SLIC3R_STATIC_INITIAL 1)
endif()

option(SLIC3R_STATIC "Link fetched dependencies statically where supported" ${SLIC3R_STATIC_INITIAL})
option(SLIC3R_PCH "Use precompiled headers" ON)
option(SLIC3R_ASAN "Enable ASan on Clang and GCC" OFF)
option(LIBSLICER_BUILD_TESTS "Build libslicer tests" ${LIBSLICER_IS_TOP_LEVEL})

if (APPLE)
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_FIND_APPBUNDLE LAST)
    set(CMAKE_MACOSX_RPATH ON CACHE BOOL "")
endif ()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/modules/)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LIBDIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(LIBDIR_BIN ${CMAKE_CURRENT_BINARY_DIR}/src)
message(STATUS "LIBDIR: ${LIBDIR}")
message(STATUS "LIBDIR_BIN: ${LIBDIR_BIN}")

include_directories(SYSTEM ${LIBDIR})
include_directories(SYSTEM ${LIBDIR}/libigl)

function(encoding_check TARGET)
endfunction()

if (MSVC)
    add_compile_options(-bigobj /Zi /FS /wd4244 /wd4267 /wd4018 /wd4305)
    add_definitions(-D_USE_MATH_DEFINES -D_WIN32 -D_CRT_SECURE_NO_WARNINGS -D_SCL_SECURE_NO_WARNINGS)
    add_definitions(-DBOOST_ALL_NO_LIB -DBOOST_USE_WINAPI_VERSION=0x602 -DBOOST_SYSTEM_USE_UTF8)
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
elseif (MINGW)
    add_compile_options(-Wa,-mbig-obj)
else ()
    add_compile_options(-fsigned-char)
endif ()

if (${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang" AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 15)
    add_compile_definitions(BOOST_NO_CXX98_FUNCTION_BASE _HAS_AUTO_PTR_ETC=0)
endif()

if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fext-numeric-literals")
endif()

if (NOT MSVC AND ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" OR "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang"))
    if (NOT MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
    endif ()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-reorder")
    add_compile_options(
        -Werror=return-type
        -Wno-unused-function
        -Wno-unused-variable
        -Wno-unused-but-set-variable
        -Wno-unused-label
        -Wno-unused-local-typedefs
        -Wno-sign-compare
        -Wno-misleading-indentation
        -Wno-switch
    )
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 6.0)
        add_compile_options(-Wno-ignored-attributes)
    endif()
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        add_compile_options(-Wno-deprecated-declarations)
    endif()
    if((${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang" OR ${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang") AND ${CMAKE_CXX_COMPILER_VERSION} VERSION_GREATER 15)
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag(-Wno-error=enum-constexpr-conversion HAS_WNO_ERROR_ENUM_CONSTEXPR_CONV)
        if(HAS_WNO_ERROR_ENUM_CONSTEXPR_CONV)
            add_compile_options(-Wno-error=enum-constexpr-conversion)
        endif()
    endif()
    if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
        add_compile_options(-Wno-unknown-pragmas)
    endif()
endif()

if (SLIC3R_ASAN)
    add_compile_options(-fsanitize=address)
    if (NOT MSVC)
        add_compile_options(-fno-omit-frame-pointer)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
        set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address")
    else()
        add_compile_definitions(_DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
    endif ()
endif ()

if (APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror=partial-availability -Werror=unguarded-availability -Werror=unguarded-availability-new")
endif ()

add_definitions(-DwxUSE_UNICODE -D_UNICODE -DUNICODE -DWXINTL_NO_GETTEXT_MACRO -DwxNO_UNSAFE_WXSTRING_CONV)

if(SLIC3R_STATIC)
    set(Boost_USE_STATIC_LIBS ON)
    set(TBB_STATIC 1)
    set(OPENVDB_USE_STATIC_LIBS ON)
    set(USE_BLOSC TRUE)
else()
    add_compile_definitions(BOOST_LOG_DYN_LINK)
endif()

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(Boost_NO_SYSTEM_PATHS TRUE)
find_package(Boost 1.83.0 REQUIRED COMPONENTS system filesystem thread log log_setup locale regex chrono atomic date_time iostreams program_options nowide)
find_package(Eigen3 5.0.1 REQUIRED)
find_package(TBB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(EXPAT REQUIRED)
find_package(PNG REQUIRED)
find_package(Freetype REQUIRED)
find_package(cereal REQUIRED)
find_package(NLopt 1.4 REQUIRED)
find_package(libnoise REQUIRED)

set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release "")
find_package(OpenVDB 5.0 COMPONENTS openvdb REQUIRED)

add_library(boost_headeronly INTERFACE)
add_library(boost_libs INTERFACE)

if (APPLE)
    target_compile_definitions(boost_headeronly INTERFACE BOOST_ASIO_DISABLE_KQUEUE)
endif()
if(NOT SLIC3R_STATIC)
    target_compile_definitions(boost_headeronly INTERFACE BOOST_LOG_DYN_LINK)
endif()

target_include_directories(boost_headeronly SYSTEM INTERFACE ${Boost_INCLUDE_DIRS})
target_link_libraries(boost_libs INTERFACE
    boost_headeronly
    Boost::atomic
    Boost::chrono
    Boost::date_time
    Boost::filesystem
    Boost::iostreams
    Boost::locale
    Boost::log
    Boost::log_setup
    Boost::nowide
    Boost::program_options
    Boost::thread
)

if (NOT TARGET cereal::cereal)
    set_target_properties(cereal PROPERTIES IMPORTED_GLOBAL TRUE)
    add_library(cereal::cereal ALIAS cereal)
else ()
    set_target_properties(cereal::cereal PROPERTIES IMPORTED_GLOBAL TRUE)
endif ()

function(slic3r_remap_configs targets from_Cfg to_Cfg)
    if(MSVC)
        string(TOUPPER ${from_Cfg} from_CFG)
        foreach(tgt ${targets})
            if(TARGET ${tgt})
                set_target_properties(${tgt} PROPERTIES MAP_IMPORTED_CONFIG_${from_CFG} ${to_Cfg})
            endif()
        endforeach()
    endif()
endfunction()

slic3r_remap_configs(IlmBase::Half RelWithDebInfo Release)
slic3r_remap_configs(Blosc::blosc RelWithDebInfo Release)

add_subdirectory(deps_src)
add_subdirectory(src/libslic3r)
add_subdirectory(src/libslicer_api)

set_target_properties(libslic3r PROPERTIES OUTPUT_NAME libslicer_core)
add_library(libslicer::core ALIAS libslic3r)
add_library(libslicer::libslicer ALIAS libslicer_api)
add_custom_target(libslicer DEPENDS libslicer_api)

set(LIBSLICER_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/libslicer"
    CACHE STRING "Install directory for the libslicer CMake package")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/libslicerConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/libslicerConfig.cmake"
    INSTALL_DESTINATION "${LIBSLICER_INSTALL_CMAKEDIR}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/libslicerConfigVersion.cmake"
    VERSION "${SoftFever_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(TARGETS libslicer_api
    EXPORT libslicerTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/libslicer"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(EXPORT libslicerTargets
    FILE libslicerTargets.cmake
    NAMESPACE libslicer::
    DESTINATION "${LIBSLICER_INSTALL_CMAKEDIR}"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/libslicerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/libslicerConfigVersion.cmake"
    DESTINATION "${LIBSLICER_INSTALL_CMAKEDIR}"
)

if(LIBSLICER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
