cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# ZLIB (compression library, built static with a local patch that collapses
# the "zlib" and "zlibstatic" targets so BUILD_SHARED_LIBS is respected)
#
# Fetches, patches, configures, builds and installs zlib into the shared fetch
# cache. After a successful install the install prefix is pushed onto
# CMAKE_PREFIX_PATH and ZLIB_ROOT is set so that the main project's
# find_package(ZLIB) (which uses CMake's built-in FindZLIB.cmake) resolves
# against this build.
# ----------------------------------------------------------------------------

if(NOT DEFINED ZLIB_VERSION)
    set(ZLIB_VERSION "1.2.13" CACHE STRING "ZLIB version to build")
endif()

set(ZLIB_URL      "https://github.com/madler/zlib/archive/refs/tags/v${ZLIB_VERSION}.zip")
set(ZLIB_URL_HASH "SHA256=c2856951bbf30e30861ace3765595d86ba13f2cf01279d901f6c62258c57f4ff")
set(ZLIB_PATCH    "${CMAKE_CURRENT_LIST_DIR}/0001-Respect-BUILD_SHARED_LIBS.patch")

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(ZLIB_CACHE_DIR "${FETCH_CACHE_DIR}/zlib-v${ZLIB_VERSION}")

if(DEFINED ZLIB_BUILD_TYPE)
    set(ZLIB_ACTUAL_BUILD_TYPE "${ZLIB_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(ZLIB_ACTUAL_BUILD_TYPE "Release")
    else()
        set(ZLIB_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(ZLIB_ACTUAL_BUILD_TYPE "Release")
endif()

set(ZLIB_BUILD_DIR   "${ZLIB_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${ZLIB_ACTUAL_BUILD_TYPE}")
set(ZLIB_INSTALL_DIR "${ZLIB_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${ZLIB_ACTUAL_BUILD_TYPE}")
set(ZLIB_SOURCE_DIR  "${ZLIB_CACHE_DIR}/src")
set(ZLIB_ZIP_FILE    "${ZLIB_CACHE_DIR}/zlib-${ZLIB_VERSION}.zip")
set(ZLIB_PATCH_MARK  "${ZLIB_SOURCE_DIR}/.orca_patch_applied")

message(STATUS "")
message(STATUS "🔧 ZLIB 库缓存管理")
message(STATUS "   ZLIB 版本: ${ZLIB_VERSION}")
message(STATUS "   构建类型: ${ZLIB_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存目录: ${ZLIB_CACHE_DIR}")
message(STATUS "   构建目录: ${ZLIB_BUILD_DIR}")
message(STATUS "   安装目录: ${ZLIB_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${ZLIB_CACHE_DIR}")

# ---- Patch invalidation ----------------------------------------------------
# The patch mark file stores the SHA256 of the patch that was applied. When the
# patch on disk changes (e.g. we add hunks), the cached source tree is stale —
# git apply would fail re-applying onto an already-patched tree, and the cached
# build/install would still reflect the old patch. Clear all three so the
# normal flow re-extracts, re-patches and rebuilds.
file(SHA256 "${ZLIB_PATCH}" ZLIB_PATCH_HASH)

set(_zlib_patch_outdated FALSE)
if(EXISTS "${ZLIB_PATCH_MARK}")
    file(READ "${ZLIB_PATCH_MARK}" _zlib_existing_mark)
    string(STRIP "${_zlib_existing_mark}" _zlib_existing_mark)
    if(NOT _zlib_existing_mark STREQUAL "${ZLIB_PATCH_HASH}")
        set(_zlib_patch_outdated TRUE)
        message(STATUS "🔄 ZLIB patch 已变化 (旧=${_zlib_existing_mark}, 新=${ZLIB_PATCH_HASH})，清理过时缓存...")
    endif()
endif()

if(EXISTS "${ZLIB_PATCH_MARK}" AND EXISTS "${ZLIB_SOURCE_DIR}/zutil.h")
    file(READ "${ZLIB_SOURCE_DIR}/zutil.h" _zlib_zutil_h)
    string(FIND "${_zlib_zutil_h}" "#if defined(MACOS) || defined(TARGET_OS_MAC)" _zlib_unpatched_idx)
    if(NOT _zlib_unpatched_idx EQUAL -1)
        set(_zlib_patch_outdated TRUE)
        message(STATUS "🔄 ZLIB source tree is missing the TARGET_OS_MAC patch，清理过时缓存...")
    endif()
    unset(_zlib_zutil_h)
    unset(_zlib_unpatched_idx)
endif()

if(_zlib_patch_outdated)
    file(REMOVE_RECURSE "${ZLIB_SOURCE_DIR}")
    file(REMOVE_RECURSE "${ZLIB_BUILD_DIR}")
    file(REMOVE_RECURSE "${ZLIB_INSTALL_DIR}")
endif()

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

# zlib installs neither a config file in 1.2.13 (the main project uses CMake's
# built-in FindZLIB module); the header is the most reliable install marker.
if(EXISTS "${ZLIB_INSTALL_DIR}/include/zlib.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${ZLIB_INSTALL_DIR}")
endif()

if(EXISTS "${ZLIB_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${ZLIB_BUILD_DIR}")
endif()

if(EXISTS "${ZLIB_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${ZLIB_SOURCE_DIR}")
endif()

if(EXISTS "${ZLIB_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${ZLIB_ZIP_FILE}")
endif()

set(ZLIB_STATUS "NONE")
set(ZLIB_FOUND FALSE)

if(HAS_INSTALL)
    set(ZLIB_STATUS "INSTALLED")
    set(ZLIB_FOUND TRUE)
elseif(HAS_BUILD)
    set(ZLIB_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(ZLIB_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(ZLIB_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${ZLIB_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(ZLIB_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 ZLIB 库，跳过下载和编译")

elseif(ZLIB_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 ZLIB...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${ZLIB_BUILD_DIR} --target install --config ${ZLIB_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${ZLIB_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ ZLIB 安装成功")
        set(ZLIB_FOUND TRUE)
    else()
        message(WARNING "❌ ZLIB 安装失败，重新构建...")
        file(REMOVE_RECURSE "${ZLIB_BUILD_DIR}")
        set(ZLIB_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(ZLIB_STATUS STREQUAL "SOURCE_ONLY")
    # Apply patch on first configure of a fresh source tree.
    if(NOT EXISTS "${ZLIB_PATCH_MARK}")
        find_package(Git REQUIRED)
        message(STATUS "🩹 应用 ZLIB patch: ${ZLIB_PATCH}")
        # git init makes the extracted tree a standalone git context so that
        # git apply behaves the same regardless of whether a parent .git exists.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} init -q
            WORKING_DIRECTORY ${ZLIB_SOURCE_DIR}
            RESULT_VARIABLE _git_init_result
        )
        if(NOT _git_init_result EQUAL 0)
            message(FATAL_ERROR "❌ git init 失败于 ${ZLIB_SOURCE_DIR}")
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --verbose --ignore-space-change --whitespace=fix ${ZLIB_PATCH}
            WORKING_DIRECTORY ${ZLIB_SOURCE_DIR}
            RESULT_VARIABLE _patch_result
        )
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "❌ ZLIB patch 应用失败")
        endif()
        file(WRITE "${ZLIB_PATCH_MARK}" "${ZLIB_PATCH_HASH}\n")
        message(STATUS "✅ ZLIB patch 应用成功")
    endif()

    message(STATUS "🔨 开始构建 ZLIB...")

    # Generator selection mirrors the Cereal script.
    set(ZLIB_GENERATOR "")
    set(ZLIB_GENERATOR_PLATFORM "")
    set(ZLIB_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(ZLIB_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(ZLIB_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(ZLIB_GENERATOR "Ninja")
            set(ZLIB_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(ZLIB_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(ZLIB_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(ZLIB_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${ZLIB_GENERATOR}")
    if(ZLIB_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${ZLIB_GENERATOR_PLATFORM})
    endif()
    if(ZLIB_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${ZLIB_MAKE_PROGRAM})
    endif()
    if(ZLIB_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${ZLIB_SOURCE_DIR}
            -B ${ZLIB_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${ZLIB_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${ZLIB_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DSKIP_INSTALL_FILES=ON
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ ZLIB 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ ZLIB 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${ZLIB_BUILD_DIR} --config ${ZLIB_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${ZLIB_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ ZLIB 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ ZLIB 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${ZLIB_BUILD_DIR} --target install --config ${ZLIB_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${ZLIB_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ ZLIB 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ ZLIB 安装成功")
    set(ZLIB_FOUND TRUE)

elseif(ZLIB_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 ZLIB 源码...")

    if(EXISTS "${ZLIB_SOURCE_DIR}")
        file(REMOVE_RECURSE "${ZLIB_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${ZLIB_ZIP_FILE}
        WORKING_DIRECTORY ${ZLIB_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        file(GLOB ZLIB_EXTRACTED_DIRS "${ZLIB_CACHE_DIR}/zlib-*")
        list(FILTER ZLIB_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH ZLIB_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET ZLIB_EXTRACTED_DIRS 0 ZLIB_EXTRACTED_DIR)
            file(RENAME "${ZLIB_EXTRACTED_DIR}" "${ZLIB_SOURCE_DIR}")
            message(STATUS "✅ ZLIB 解压成功")
            set(ZLIB_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 zlib-* 目录")
        endif()
    else()
        message(WARNING "❌ ZLIB 解压失败: ${extract_error}")
        file(REMOVE "${ZLIB_ZIP_FILE}")
        set(ZLIB_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 ZLIB ${ZLIB_VERSION}...")
    message(STATUS "   URL: ${ZLIB_URL}")

    file(DOWNLOAD
        ${ZLIB_URL}
        ${ZLIB_ZIP_FILE}
        EXPECTED_HASH ${ZLIB_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${ZLIB_ZIP_FILE}")
        message(FATAL_ERROR "❌ ZLIB 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ ZLIB 下载成功")

    set(ZLIB_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(ZLIB_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${ZLIB_INSTALL_DIR}")
    # Built-in FindZLIB honors ZLIB_ROOT.
    set(ZLIB_ROOT "${ZLIB_INSTALL_DIR}" CACHE PATH "zlib root" FORCE)
    set(ZLIB_AVAILABLE TRUE CACHE BOOL "ZLIB library is available")
    # 同步更新进程环境变量，使后续依赖的子 cmake 进程（execute_process 启动的）
    # 在自己的 find_package 里也能找到此库，无需每个子 cmake 单独传 -DZLIB_ROOT。
    set(ENV{CMAKE_PREFIX_PATH} "${ZLIB_INSTALL_DIR};$ENV{CMAKE_PREFIX_PATH}")
    message(STATUS "🎯 ZLIB 就绪 (${ZLIB_VERSION}) -> ${ZLIB_INSTALL_DIR}")
else()
    set(ZLIB_AVAILABLE FALSE CACHE BOOL "ZLIB library is not available")
    message(FATAL_ERROR "⚠️  ZLIB 库不可用")
endif()
