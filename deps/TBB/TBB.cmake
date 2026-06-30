cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Intel oneTBB (static build)
#
# The original PrusaSlicer/Orca flow patches one source file (cmake/compilers/
# GNU.cmake) only when building inside Flatpak with GCC; that conditional is
# preserved here so the fetch flow matches the historical behavior.
# ----------------------------------------------------------------------------

if(NOT DEFINED TBB_VERSION)
    set(TBB_VERSION "2021.5.0" CACHE STRING "oneTBB version to build")
endif()

set(TBB_URL      "https://github.com/oneapi-src/oneTBB/archive/refs/tags/v${TBB_VERSION}.zip")
set(TBB_URL_HASH "SHA256=83ea786c964a384dd72534f9854b419716f412f9d43c0be88d41874763e7bb47")

# Conditional patch: Flatpak + GCC only (overrides cmake/compilers/GNU.cmake).
set(TBB_PATCH_NEEDED FALSE)
if(FLATPAK AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(TBB_PATCH_NEEDED TRUE)
    set(TBB_GNU_OVERRIDE "${CMAKE_CURRENT_LIST_DIR}/GNU.cmake")
endif()

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(TBB_CACHE_DIR "${FETCH_CACHE_DIR}/tbb-v${TBB_VERSION}")

if(DEFINED TBB_BUILD_TYPE_OVERRIDE)
    set(TBB_ACTUAL_BUILD_TYPE "${TBB_BUILD_TYPE_OVERRIDE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(TBB_ACTUAL_BUILD_TYPE "Release")
    else()
        set(TBB_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(TBB_ACTUAL_BUILD_TYPE "Release")
endif()

set(TBB_BUILD_DIR   "${TBB_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${TBB_ACTUAL_BUILD_TYPE}")
set(TBB_INSTALL_DIR "${TBB_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${TBB_ACTUAL_BUILD_TYPE}")
set(TBB_SOURCE_DIR  "${TBB_CACHE_DIR}/src")
set(TBB_ZIP_FILE    "${TBB_CACHE_DIR}/oneTBB-${TBB_VERSION}.zip")
set(TBB_PATCH_MARK  "${TBB_SOURCE_DIR}/.orca_patch_applied")

message(STATUS "")
message(STATUS "🔧 TBB 库缓存管理")
message(STATUS "   TBB 版本: ${TBB_VERSION}")
message(STATUS "   构建类型: ${TBB_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存目录: ${TBB_CACHE_DIR}")
message(STATUS "   构建目录: ${TBB_BUILD_DIR}")
message(STATUS "   安装目录: ${TBB_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${TBB_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

if(EXISTS "${TBB_INSTALL_DIR}/include/tbb/tbb.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${TBB_INSTALL_DIR}")
endif()

if(EXISTS "${TBB_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${TBB_BUILD_DIR}")
endif()

if(EXISTS "${TBB_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${TBB_SOURCE_DIR}")
endif()

if(EXISTS "${TBB_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${TBB_ZIP_FILE}")
endif()

set(TBB_STATUS "NONE")
set(TBB_FOUND FALSE)

if(HAS_INSTALL)
    set(TBB_STATUS "INSTALLED")
    set(TBB_FOUND TRUE)
elseif(HAS_BUILD)
    set(TBB_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(TBB_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(TBB_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${TBB_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(TBB_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 TBB 库，跳过下载和编译")

elseif(TBB_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 TBB...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${TBB_BUILD_DIR} --target install --config ${TBB_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${TBB_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ TBB 安装成功")
        set(TBB_FOUND TRUE)
    else()
        message(WARNING "❌ TBB 安装失败，重新构建...")
        file(REMOVE_RECURSE "${TBB_BUILD_DIR}")
        set(TBB_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(TBB_STATUS STREQUAL "SOURCE_ONLY")
    if(TBB_PATCH_NEEDED AND NOT EXISTS "${TBB_PATCH_MARK}")
        message(STATUS "🩹 应用 TBB GNU.cmake 覆盖 (Flatpak+GCC)")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy ${TBB_GNU_OVERRIDE} ${TBB_SOURCE_DIR}/cmake/compilers/GNU.cmake
            RESULT_VARIABLE _patch_result
        )
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "❌ TBB GNU.cmake 覆盖失败")
        endif()
        file(WRITE "${TBB_PATCH_MARK}" "patched\n")
        message(STATUS "✅ TBB GNU.cmake 覆盖成功")
    elseif(NOT TBB_PATCH_NEEDED)
        # Touch marker so next configure doesn't re-evaluate the condition.
        file(WRITE "${TBB_PATCH_MARK}" "no-patch-needed\n")
    endif()

    message(STATUS "🔨 开始构建 TBB...")

    set(TBB_GENERATOR "")
    set(TBB_GENERATOR_PLATFORM "")
    set(TBB_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(TBB_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(TBB_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(TBB_GENERATOR "Ninja")
            set(TBB_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(TBB_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(TBB_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(TBB_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${TBB_GENERATOR}")
    if(TBB_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${TBB_GENERATOR_PLATFORM})
    endif()
    if(TBB_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${TBB_MAKE_PROGRAM})
    endif()
    if(TBB_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${TBB_SOURCE_DIR}
            -B ${TBB_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${TBB_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${TBB_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DTBB_BUILD_SHARED=OFF
            -DTBB_BUILD_TESTS=OFF
            -DTBB_TEST=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=_debug
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ TBB 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ TBB 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${TBB_BUILD_DIR} --config ${TBB_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${TBB_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ TBB 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ TBB 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${TBB_BUILD_DIR} --target install --config ${TBB_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${TBB_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ TBB 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ TBB 安装成功")
    set(TBB_FOUND TRUE)

elseif(TBB_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 TBB 源码...")

    if(EXISTS "${TBB_SOURCE_DIR}")
        file(REMOVE_RECURSE "${TBB_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${TBB_ZIP_FILE}
        WORKING_DIRECTORY ${TBB_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # GitHub archive of oneTBB extracts to oneTBB-<version>/
        file(GLOB TBB_EXTRACTED_DIRS "${TBB_CACHE_DIR}/oneTBB-*")
        list(FILTER TBB_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH TBB_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET TBB_EXTRACTED_DIRS 0 TBB_EXTRACTED_DIR)
            file(RENAME "${TBB_EXTRACTED_DIR}" "${TBB_SOURCE_DIR}")
            message(STATUS "✅ TBB 解压成功")
            set(TBB_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 oneTBB-* 目录")
        endif()
    else()
        message(WARNING "❌ TBB 解压失败: ${extract_error}")
        file(REMOVE "${TBB_ZIP_FILE}")
        set(TBB_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 TBB ${TBB_VERSION}...")
    message(STATUS "   URL: ${TBB_URL}")

    file(DOWNLOAD
        ${TBB_URL}
        ${TBB_ZIP_FILE}
        EXPECTED_HASH ${TBB_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${TBB_TAR_FILE}")
        message(FATAL_ERROR "❌ TBB 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ TBB 下载成功")

    set(TBB_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(TBB_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${TBB_INSTALL_DIR}")
    set(TBB_ROOT "${TBB_INSTALL_DIR}" CACHE PATH "TBB root" FORCE)
    set(TBBROOT  "${TBB_INSTALL_DIR}" CACHE PATH "TBB root (legacy var)" FORCE)
    set(TBB_INCLUDE_DIRS "${TBB_INSTALL_DIR}/include" CACHE PATH "TBB include dirs" FORCE)
    include_directories(SYSTEM "${TBB_INSTALL_DIR}/include")
    if(EXISTS "${TBB_INSTALL_DIR}/lib/cmake/TBB/TBBConfig.cmake")
        set(TBB_DIR "${TBB_INSTALL_DIR}/lib/cmake/TBB" CACHE PATH "TBB config dir" FORCE)
    endif()
    set(TBB_AVAILABLE TRUE CACHE BOOL "TBB library is available")
    message(STATUS "🎯 TBB 就绪 (${TBB_VERSION}) -> ${TBB_INSTALL_DIR}")
else()
    set(TBB_AVAILABLE FALSE CACHE BOOL "TBB library is not available")
    message(FATAL_ERROR "⚠️  TBB 库不可用")
endif()
