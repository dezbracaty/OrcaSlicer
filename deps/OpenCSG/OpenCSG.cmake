cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# OpenCSG (constructive solid geometry rendering library, v1.4.2)
#
# 来源 https://github.com/floriankirsch/OpenCSG/archive/refs/tags/opencsg-1-4-2-release.zip
# 上游 CMakeLists.txt 不适用，配置阶段用 deps/OpenCSG/CMakeLists.txt.in 整体替换
# （行为对齐已删除的 OpenCSG.cmake 的 PATCH_COMMAND）。
# 依赖 GLEW（由先于本脚本 include 的 deps/GLEW/GLEW.cmake 提供）。
# ----------------------------------------------------------------------------

if(NOT DEFINED OPENCSG_VERSION)
    set(OPENCSG_VERSION "1.4.2" CACHE STRING "OpenCSG version to build")
endif()
set(OPENCSG_TAG       "opencsg-1-4-2-release")
set(OPENCSG_URL       "https://github.com/floriankirsch/OpenCSG/archive/refs/tags/${OPENCSG_TAG}.zip")
set(OPENCSG_URL_HASH  "SHA256=51afe0db79af8386e2027d56d685177135581e0ee82ade9d7f2caff8deab5ec5")
set(OPENCSG_PATCH_SRC "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt.in")

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE 环境变量未设置；顶层 CMakeLists.txt 应该已经设置默认值。")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(OPENCSG_CACHE_DIR "${FETCH_CACHE_DIR}/opencsg-v${OPENCSG_VERSION}")

if(DEFINED OPENCSG_BUILD_TYPE)
    set(OPENCSG_ACTUAL_BUILD_TYPE "${OPENCSG_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(OPENCSG_ACTUAL_BUILD_TYPE "Release")
    else()
        set(OPENCSG_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(OPENCSG_ACTUAL_BUILD_TYPE "Release")
endif()

set(OPENCSG_BUILD_DIR   "${OPENCSG_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${OPENCSG_ACTUAL_BUILD_TYPE}")
set(OPENCSG_INSTALL_DIR "${OPENCSG_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${OPENCSG_ACTUAL_BUILD_TYPE}")
set(OPENCSG_SOURCE_DIR  "${OPENCSG_CACHE_DIR}/src")
set(OPENCSG_ZIP_FILE    "${OPENCSG_CACHE_DIR}/opencsg-${OPENCSG_TAG}.zip")
set(OPENCSG_PATCH_MARK  "${OPENCSG_SOURCE_DIR}/.orca_patch_applied")

message(STATUS "")
message(STATUS "🔧 OpenCSG 库缓存管理")
message(STATUS "   OpenCSG 版本: ${OPENCSG_VERSION}")
message(STATUS "   构建类型: ${OPENCSG_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存目录: ${OPENCSG_CACHE_DIR}")
message(STATUS "   构建目录: ${OPENCSG_BUILD_DIR}")
message(STATUS "   安装目录: ${OPENCSG_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${OPENCSG_CACHE_DIR}")

# ---- 状态检测 -------------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

# CMakeLists.txt.in 把 install 输出固定成 lib/cmake/OpenCSG/OpenCSGConfig.cmake
if(EXISTS "${OPENCSG_INSTALL_DIR}/lib/cmake/OpenCSG/OpenCSGConfig.cmake")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${OPENCSG_INSTALL_DIR}")
endif()

if(EXISTS "${OPENCSG_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${OPENCSG_BUILD_DIR}")
endif()

# OpenCSG 上游不带 CMakeLists.txt（用 Makefile/sln），解压后 src 子目录是稳定标志；
# 是否打过 patch 由 SOURCE_ONLY 分支内通过 OPENCSG_PATCH_MARK 自行判定。
if(EXISTS "${OPENCSG_SOURCE_DIR}/src")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${OPENCSG_SOURCE_DIR}")
endif()

if(EXISTS "${OPENCSG_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${OPENCSG_ZIP_FILE}")
endif()

set(OPENCSG_STATUS "NONE")
set(OPENCSG_FOUND  FALSE)

if(HAS_INSTALL)
    set(OPENCSG_STATUS "INSTALLED")
    set(OPENCSG_FOUND TRUE)
elseif(HAS_BUILD)
    set(OPENCSG_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(OPENCSG_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(OPENCSG_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${OPENCSG_STATUS}")

# ---- 状态机执行 -----------------------------------------------------------
if(OPENCSG_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 OpenCSG 库，跳过下载和编译")

elseif(OPENCSG_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 OpenCSG...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${OPENCSG_BUILD_DIR} --target install --config ${OPENCSG_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${OPENCSG_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ OpenCSG 安装成功")
        set(OPENCSG_FOUND TRUE)
    else()
        message(WARNING "❌ OpenCSG 安装失败，重新构建...")
        file(REMOVE_RECURSE "${OPENCSG_BUILD_DIR}")
        set(OPENCSG_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(OPENCSG_STATUS STREQUAL "SOURCE_ONLY")
    if(NOT EXISTS "${OPENCSG_PATCH_MARK}")
        message(STATUS "🩹 应用 OpenCSG patch (拷贝 CMakeLists.txt.in)")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy ${OPENCSG_PATCH_SRC} ${OPENCSG_SOURCE_DIR}/CMakeLists.txt
            RESULT_VARIABLE _patch_result
        )
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "❌ OpenCSG patch 应用失败")
        endif()
        file(WRITE "${OPENCSG_PATCH_MARK}" "patched\n")
        message(STATUS "✅ OpenCSG patch 应用成功")
    endif()

    message(STATUS "🔨 开始构建 OpenCSG...")

    set(OPENCSG_GENERATOR "")
    set(OPENCSG_GENERATOR_PLATFORM "")
    set(OPENCSG_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(OPENCSG_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(OPENCSG_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(OPENCSG_GENERATOR "Ninja")
            set(OPENCSG_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(OPENCSG_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(OPENCSG_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(OPENCSG_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${OPENCSG_GENERATOR}")
    if(OPENCSG_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${OPENCSG_GENERATOR_PLATFORM})
    endif()
    if(OPENCSG_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${OPENCSG_MAKE_PROGRAM})
    endif()
    if(OPENCSG_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 配置 OpenCSG（GLEW 来自 CMAKE_PREFIX_PATH，BUILD_SHARED_LIBS=OFF 走静态）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${OPENCSG_SOURCE_DIR}
            -B ${OPENCSG_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${OPENCSG_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${OPENCSG_INSTALL_DIR}
            "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ OpenCSG 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ OpenCSG 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${OPENCSG_BUILD_DIR} --config ${OPENCSG_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${OPENCSG_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ OpenCSG 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ OpenCSG 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${OPENCSG_BUILD_DIR} --target install --config ${OPENCSG_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${OPENCSG_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ OpenCSG 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ OpenCSG 安装成功")
    set(OPENCSG_FOUND TRUE)

elseif(OPENCSG_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 OpenCSG 源码...")

    if(EXISTS "${OPENCSG_SOURCE_DIR}")
        file(REMOVE_RECURSE "${OPENCSG_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${OPENCSG_ZIP_FILE}
        WORKING_DIRECTORY ${OPENCSG_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # GitHub archive 顶层目录恒为 <repo>-<tag>，按 tag 定位最稳
        file(GLOB _candidates "${OPENCSG_CACHE_DIR}/*${OPENCSG_TAG}*")
        set(OPENCSG_EXTRACTED_DIR "")
        foreach(_c IN LISTS _candidates)
            if(IS_DIRECTORY "${_c}")
                set(OPENCSG_EXTRACTED_DIR "${_c}")
                break()
            endif()
        endforeach()

        if(OPENCSG_EXTRACTED_DIR)
            file(RENAME "${OPENCSG_EXTRACTED_DIR}" "${OPENCSG_SOURCE_DIR}")
            message(STATUS "✅ OpenCSG 解压成功")
            set(OPENCSG_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到包含 tag ${OPENCSG_TAG} 的目录")
        endif()
    else()
        message(WARNING "❌ OpenCSG 解压失败: ${extract_error}")
        file(REMOVE "${OPENCSG_ZIP_FILE}")
        set(OPENCSG_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 OpenCSG ${OPENCSG_VERSION}...")
    message(STATUS "   URL: ${OPENCSG_URL}")

    file(DOWNLOAD
        ${OPENCSG_URL}
        ${OPENCSG_ZIP_FILE}
        EXPECTED_HASH ${OPENCSG_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${OPENCSG_ZIP_FILE}")
        message(FATAL_ERROR "❌ OpenCSG 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ OpenCSG 下载成功")

    set(OPENCSG_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- 注册到主项目 ---------------------------------------------------------
if(OPENCSG_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${OPENCSG_INSTALL_DIR}")
    list(APPEND ORCA_DEPS_INSTALL_PATHS "${OPENCSG_INSTALL_DIR}")
    set(ORCA_DEPS_INSTALL_PATHS "${ORCA_DEPS_INSTALL_PATHS}" CACHE INTERNAL "Per-dep install roots for runtime DLL lookup" FORCE)
    set(OpenCSG_DIR "${OPENCSG_INSTALL_DIR}/lib/cmake/OpenCSG" CACHE PATH "OpenCSG cmake config dir" FORCE)
    set(OPENCSG_AVAILABLE TRUE CACHE BOOL "OpenCSG library is available")
    message(STATUS "🎯 OpenCSG 就绪 (${OPENCSG_VERSION}) -> ${OPENCSG_INSTALL_DIR}")
else()
    set(OPENCSG_AVAILABLE FALSE CACHE BOOL "OpenCSG library is not available")
    message(FATAL_ERROR "⚠️  OpenCSG 库不可用")
endif()
