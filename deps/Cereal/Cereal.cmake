cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Cereal (header-only serialization library)
#
# Fetches, configures, builds and installs cereal into the shared fetch cache
# at $ENV{CMAKE_FETCH_CACHE}/cereal-v${CEREAL_VERSION}. After a successful
# install, prepends the install prefix to CMAKE_PREFIX_PATH so that the main
# project's find_package(cereal REQUIRED) call can resolve it via the bundled
# cmake/modules/Findcereal.cmake (CONFIG first, then header-fallback).
# ----------------------------------------------------------------------------

if(NOT DEFINED CEREAL_VERSION)
    set(CEREAL_VERSION "1.3.0" CACHE STRING "Cereal version to build")
endif()

set(CEREAL_URL      "https://github.com/USCiLab/cereal/archive/refs/tags/v${CEREAL_VERSION}.zip")
set(CEREAL_URL_HASH "SHA256=71642cb54658e98c8f07a0f0d08bf9766f1c3771496936f6014169d3726d9657")

# Cache directory must be set (top-level CMakeLists.txt provides a default)
if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(CEREAL_CACHE_DIR "${FETCH_CACHE_DIR}/cereal-v${CEREAL_VERSION}")

# Pick build type. Visual Studio multi-config collapses to Debug since cereal
# is header-only and the install target does not vary by config.
if(DEFINED CEREAL_BUILD_TYPE)
    set(CEREAL_ACTUAL_BUILD_TYPE "${CEREAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(CEREAL_ACTUAL_BUILD_TYPE "Debug")
    else()
        set(CEREAL_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(CEREAL_ACTUAL_BUILD_TYPE "Release")
endif()

set(CEREAL_BUILD_DIR   "${CEREAL_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${CEREAL_ACTUAL_BUILD_TYPE}")
set(CEREAL_INSTALL_DIR "${CEREAL_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${CEREAL_ACTUAL_BUILD_TYPE}")
set(CEREAL_SOURCE_DIR  "${CEREAL_CACHE_DIR}/src")
set(CEREAL_ZIP_FILE    "${CEREAL_CACHE_DIR}/cereal-${CEREAL_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 Cereal 库缓存管理")
message(STATUS "   Cereal 版本: ${CEREAL_VERSION}")
message(STATUS "   构建类型: ${CEREAL_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   Cereal 缓存目录: ${CEREAL_CACHE_DIR}")
message(STATUS "   构建目录: ${CEREAL_BUILD_DIR}")
message(STATUS "   安装目录: ${CEREAL_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${CEREAL_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

# Cereal with JUST_INSTALL_CEREAL=ON only installs headers.
if(EXISTS "${CEREAL_INSTALL_DIR}/include/cereal/cereal.hpp")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${CEREAL_INSTALL_DIR}")
endif()

if(EXISTS "${CEREAL_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${CEREAL_BUILD_DIR}")
endif()

if(EXISTS "${CEREAL_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${CEREAL_SOURCE_DIR}")
endif()

if(EXISTS "${CEREAL_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${CEREAL_ZIP_FILE}")
endif()

set(CEREAL_STATUS "NONE")
set(CEREAL_FOUND FALSE)

if(HAS_INSTALL)
    set(CEREAL_STATUS "INSTALLED")
    set(CEREAL_FOUND TRUE)
elseif(HAS_BUILD)
    set(CEREAL_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(CEREAL_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(CEREAL_STATUS "ZIP_ONLY")
else()
    set(CEREAL_STATUS "NONE")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${CEREAL_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(CEREAL_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 Cereal 库，跳过下载和编译")

elseif(CEREAL_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 Cereal...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${CEREAL_BUILD_DIR} --target install --config ${CEREAL_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${CEREAL_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ Cereal 安装成功")
        set(CEREAL_FOUND TRUE)
    else()
        message(WARNING "❌ Cereal 安装失败，重新构建...")
        file(REMOVE_RECURSE "${CEREAL_BUILD_DIR}")
        set(CEREAL_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(CEREAL_STATUS STREQUAL "SOURCE_ONLY")
    message(STATUS "🔨 开始构建 Cereal...")

    # Pick generator. Mirror the main project where possible; prefer Ninja on
    # non-Windows if available for faster configure.
    set(CEREAL_GENERATOR "")
    set(CEREAL_GENERATOR_PLATFORM "")
    set(CEREAL_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(CEREAL_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(CEREAL_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(CEREAL_GENERATOR "Ninja")
            set(CEREAL_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(CEREAL_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(CEREAL_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(CEREAL_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${CEREAL_GENERATOR}")
    if(CEREAL_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${CEREAL_GENERATOR_PLATFORM})
    endif()
    if(CEREAL_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${CEREAL_MAKE_PROGRAM})
    endif()
    if(CEREAL_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # Configure
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${CEREAL_SOURCE_DIR}
            -B ${CEREAL_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${CEREAL_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${CEREAL_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DJUST_INSTALL_CEREAL=ON
            -DSKIP_PERFORMANCE_COMPARISON=ON
            -DBUILD_TESTS=OFF
        RESULT_VARIABLE config_result
    )

    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ Cereal 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ Cereal 配置成功")

    # Build (cereal is header-only; this is effectively a no-op but harmless)
    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${CEREAL_BUILD_DIR} --config ${CEREAL_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${CEREAL_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ Cereal 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ Cereal 编译成功")

    # Install
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${CEREAL_BUILD_DIR} --target install --config ${CEREAL_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${CEREAL_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ Cereal 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ Cereal 安装成功")
    set(CEREAL_FOUND TRUE)

elseif(CEREAL_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 Cereal 源码...")

    if(EXISTS "${CEREAL_SOURCE_DIR}")
        file(REMOVE_RECURSE "${CEREAL_SOURCE_DIR}")
    endif()

    # cmake -E tar handles .zip in modern CMake (libarchive-based).
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${CEREAL_ZIP_FILE}
        WORKING_DIRECTORY ${CEREAL_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # GitHub archives extract to cereal-<version>/
        file(GLOB CEREAL_EXTRACTED_DIRS "${CEREAL_CACHE_DIR}/cereal-*")
        list(FILTER CEREAL_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH CEREAL_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET CEREAL_EXTRACTED_DIRS 0 CEREAL_EXTRACTED_DIR)
            file(RENAME "${CEREAL_EXTRACTED_DIR}" "${CEREAL_SOURCE_DIR}")
            message(STATUS "✅ Cereal 解压成功")
            set(CEREAL_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 cereal-* 目录")
        endif()
    else()
        message(WARNING "❌ Cereal 解压失败: ${extract_error}")
        file(REMOVE "${CEREAL_ZIP_FILE}")
        set(CEREAL_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 Cereal ${CEREAL_VERSION}...")
    message(STATUS "   URL: ${CEREAL_URL}")

    file(DOWNLOAD
        ${CEREAL_URL}
        ${CEREAL_ZIP_FILE}
        EXPECTED_HASH ${CEREAL_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${CEREAL_ZIP_FILE}")
        message(FATAL_ERROR "❌ Cereal 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ Cereal 下载成功")

    set(CEREAL_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(CEREAL_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${CEREAL_INSTALL_DIR}")
    # Hint paths for CONFIG-mode find_package; harmless if config file is absent
    # (Findcereal.cmake then falls back to include-header detection).
    set(cereal_DIR     "${CEREAL_INSTALL_DIR}/share/cmake/cereal" CACHE PATH "cereal config dir" FORCE)
    set(cereal_ROOT    "${CEREAL_INSTALL_DIR}" CACHE PATH "cereal root" FORCE)
    set(CEREAL_ROOT    "${CEREAL_INSTALL_DIR}" CACHE PATH "cereal root" FORCE)
    # For the header-only fallback path in Findcereal.cmake.
    include_directories("${CEREAL_INSTALL_DIR}/include")
    set(CEREAL_AVAILABLE TRUE CACHE BOOL "Cereal library is available")
    message(STATUS "🎯 Cereal 就绪 (${CEREAL_VERSION}) -> ${CEREAL_INSTALL_DIR}")
else()
    set(CEREAL_AVAILABLE FALSE CACHE BOOL "Cereal library is not available")
    message(FATAL_ERROR "⚠️  Cereal 库不可用")
endif()
