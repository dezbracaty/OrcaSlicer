cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# libpng (static build, symbol-prefixed with "prusaslicer_")
#
# Depends on the zlib built by deps/ZLIB/ZLIB.cmake. ZLIB_ROOT is consumed
# from the CMake cache and re-passed into the child configure process.
# ----------------------------------------------------------------------------

if(NOT DEFINED PNG_VERSION)
    set(PNG_VERSION "1.6.35" CACHE STRING "libpng version to build")
endif()

set(PNG_URL      "https://github.com/glennrp/libpng/archive/refs/tags/v${PNG_VERSION}.zip")
set(PNG_URL_HASH "SHA256=3d22d46c566b1761a0e15ea397589b3a5f36ac09b7c785382e6470156c04247f")

# libpng 1.6.35 on Apple needs local patches; no patch is required on Windows
# or Linux. The two patch sets mirror the old deps/CMakeLists.txt logic.
set(PNG_PATCHES "")
set(_png_is_cross_compile FALSE)
if(APPLE)
    if(CMAKE_OSX_ARCHITECTURES)
        list(FIND CMAKE_OSX_ARCHITECTURES "${CMAKE_SYSTEM_PROCESSOR}" _arch_idx)
        if(_arch_idx LESS 0)
            set(_png_is_cross_compile TRUE)
        endif()
    endif()
    if(_png_is_cross_compile)
        list(APPEND PNG_PATCHES "${CMAKE_CURRENT_LIST_DIR}/macos-arm64.patch")
    else()
        list(APPEND PNG_PATCHES "${CMAKE_CURRENT_LIST_DIR}/PNG.patch")
    endif()
    list(APPEND PNG_PATCHES "${CMAKE_CURRENT_LIST_DIR}/0002-clang19-macos.patch")
endif()

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

# ZLIB must have been fetched first; the aggregator (deps/fetch_deps.cmake)
# includes ZLIB before PNG so this should hold.
if(NOT ZLIB_AVAILABLE OR NOT ZLIB_ROOT)
    message(FATAL_ERROR
        "PNG requires ZLIB to be fetched first. "
        "Include deps/ZLIB/ZLIB.cmake before deps/PNG/PNG.cmake in fetch_deps.cmake.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(PNG_CACHE_DIR "${FETCH_CACHE_DIR}/png-v${PNG_VERSION}")

if(DEFINED PNG_BUILD_TYPE)
    set(PNG_ACTUAL_BUILD_TYPE "${PNG_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(PNG_ACTUAL_BUILD_TYPE "Release")
    else()
        set(PNG_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(PNG_ACTUAL_BUILD_TYPE "Release")
endif()

set(PNG_BUILD_DIR   "${PNG_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${PNG_ACTUAL_BUILD_TYPE}")
set(PNG_INSTALL_DIR "${PNG_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${PNG_ACTUAL_BUILD_TYPE}")
set(PNG_SOURCE_DIR  "${PNG_CACHE_DIR}/src")
set(PNG_ZIP_FILE    "${PNG_CACHE_DIR}/libpng-${PNG_VERSION}.zip")
set(PNG_PATCH_MARK  "${PNG_SOURCE_DIR}/.orca_patch_applied")

message(STATUS "")
message(STATUS "🔧 PNG 库缓存管理")
message(STATUS "   PNG 版本: ${PNG_VERSION}")
message(STATUS "   构建类型: ${PNG_ACTUAL_BUILD_TYPE}")
message(STATUS "   ZLIB 路径: ${ZLIB_ROOT}")
message(STATUS "   缓存目录: ${PNG_CACHE_DIR}")
message(STATUS "   构建目录: ${PNG_BUILD_DIR}")
message(STATUS "   安装目录: ${PNG_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${PNG_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

# libpng 1.6.35 installs a CMake config file in lib/libpng/ (or share/) but
# the header marker is always present and platform-independent.
if(EXISTS "${PNG_INSTALL_DIR}/include/png.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${PNG_INSTALL_DIR}")
endif()

if(EXISTS "${PNG_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${PNG_BUILD_DIR}")
endif()

if(EXISTS "${PNG_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${PNG_SOURCE_DIR}")
endif()

if(EXISTS "${PNG_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${PNG_ZIP_FILE}")
endif()

set(PNG_STATUS "NONE")
set(PNG_FOUND FALSE)

if(HAS_INSTALL)
    set(PNG_STATUS "INSTALLED")
    set(PNG_FOUND TRUE)
elseif(HAS_BUILD)
    set(PNG_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(PNG_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(PNG_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${PNG_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(PNG_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 PNG 库，跳过下载和编译")

elseif(PNG_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 PNG...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${PNG_BUILD_DIR} --target install --config ${PNG_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${PNG_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ PNG 安装成功")
        set(PNG_FOUND TRUE)
    else()
        message(WARNING "❌ PNG 安装失败，重新构建...")
        file(REMOVE_RECURSE "${PNG_BUILD_DIR}")
        set(PNG_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(PNG_STATUS STREQUAL "SOURCE_ONLY")
    if(NOT EXISTS "${PNG_PATCH_MARK}")
        if(PNG_PATCHES)
            find_package(Git REQUIRED)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} init -q
                WORKING_DIRECTORY ${PNG_SOURCE_DIR}
                RESULT_VARIABLE _git_init_result
            )
            if(NOT _git_init_result EQUAL 0)
                message(FATAL_ERROR "❌ git init 失败于 ${PNG_SOURCE_DIR}")
            endif()
            foreach(_patch ${PNG_PATCHES})
                message(STATUS "🩹 应用 PNG patch: ${_patch}")
                execute_process(
                    COMMAND ${GIT_EXECUTABLE} apply --verbose --ignore-space-change --whitespace=fix ${_patch}
                    WORKING_DIRECTORY ${PNG_SOURCE_DIR}
                    RESULT_VARIABLE _patch_result
                )
                if(NOT _patch_result EQUAL 0)
                    message(FATAL_ERROR "❌ PNG patch 应用失败: ${_patch}")
                endif()
            endforeach()
        endif()
        file(WRITE "${PNG_PATCH_MARK}" "patched\n")
        if(PNG_PATCHES)
            message(STATUS "✅ PNG patches 应用成功")
        endif()
    endif()

    message(STATUS "🔨 开始构建 PNG...")

    set(PNG_GENERATOR "")
    set(PNG_GENERATOR_PLATFORM "")
    set(PNG_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(PNG_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(PNG_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(PNG_GENERATOR "Ninja")
            set(PNG_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(PNG_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(PNG_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(PNG_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${PNG_GENERATOR}")
    if(PNG_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${PNG_GENERATOR_PLATFORM})
    endif()
    if(PNG_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${PNG_MAKE_PROGRAM})
    endif()
    if(PNG_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # Apple-only: disable NEON extension.
    set(PNG_EXTRA_ARGS "")
    if(APPLE)
        list(APPEND PNG_EXTRA_ARGS -DPNG_ARM_NEON=off)
    endif()

    # CMAKE_PREFIX_PATH must be passed with escaped semicolons so the nested
    # cmake invocation sees it as a single list argument, not multiple.
    set(_prefix_path_escaped "${CMAKE_PREFIX_PATH}")
    string(REPLACE ";" "\\;" _prefix_path_escaped "${_prefix_path_escaped}")

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${PNG_SOURCE_DIR}
            -B ${PNG_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${PNG_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${PNG_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
            -DCMAKE_PREFIX_PATH=${_prefix_path_escaped}
            -DZLIB_ROOT=${ZLIB_ROOT}
            -DPNG_SHARED=OFF
            -DPNG_STATIC=ON
            -DPNG_PREFIX=prusaslicer_
            -DPNG_TESTS=OFF
            -DDISABLE_DEPENDENCY_TRACKING=OFF
            ${PNG_EXTRA_ARGS}
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ PNG 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ PNG 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${PNG_BUILD_DIR} --config ${PNG_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${PNG_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ PNG 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ PNG 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${PNG_BUILD_DIR} --target install --config ${PNG_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${PNG_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ PNG 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ PNG 安装成功")
    set(PNG_FOUND TRUE)

elseif(PNG_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 PNG 源码...")

    if(EXISTS "${PNG_SOURCE_DIR}")
        file(REMOVE_RECURSE "${PNG_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${PNG_ZIP_FILE}
        WORKING_DIRECTORY ${PNG_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # libpng extracts to libpng-<version>/ (GitHub repo name, not "png-...")
        file(GLOB PNG_EXTRACTED_DIRS "${PNG_CACHE_DIR}/libpng-*")
        list(FILTER PNG_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH PNG_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET PNG_EXTRACTED_DIRS 0 PNG_EXTRACTED_DIR)
            file(RENAME "${PNG_EXTRACTED_DIR}" "${PNG_SOURCE_DIR}")
            message(STATUS "✅ PNG 解压成功")
            set(PNG_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 libpng-* 目录")
        endif()
    else()
        message(WARNING "❌ PNG 解压失败: ${extract_error}")
        file(REMOVE "${PNG_ZIP_FILE}")
        set(PNG_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 PNG ${PNG_VERSION}...")
    message(STATUS "   URL: ${PNG_URL}")

    file(DOWNLOAD
        ${PNG_URL}
        ${PNG_ZIP_FILE}
        EXPECTED_HASH ${PNG_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${PNG_ZIP_FILE}")
        message(FATAL_ERROR "❌ PNG 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ PNG 下载成功")

    set(PNG_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(PNG_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${PNG_INSTALL_DIR}")
    # Built-in FindPNG honors PNG_ROOT.
    set(PNG_ROOT "${PNG_INSTALL_DIR}" CACHE PATH "libpng root" FORCE)
    set(PNG_AVAILABLE TRUE CACHE BOOL "PNG library is available")
    message(STATUS "🎯 PNG 就绪 (${PNG_VERSION}) -> ${PNG_INSTALL_DIR}")
else()
    set(PNG_AVAILABLE FALSE CACHE BOOL "PNG library is not available")
    message(FATAL_ERROR "⚠️  PNG 库不可用")
endif()
