cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Qhull (computational geometry, static build)
# ----------------------------------------------------------------------------

if(NOT DEFINED QHULL_VERSION)
    set(QHULL_VERSION "8.0.2" CACHE STRING "Qhull version to build")
endif()

set(QHULL_URL      "https://github.com/qhull/qhull/archive/v${QHULL_VERSION}.zip")
set(QHULL_URL_HASH "SHA256=a378e9a39e718e289102c20d45632f873bfdc58a7a5f924246ea4b176e185f1e")

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(QHULL_CACHE_DIR "${FETCH_CACHE_DIR}/qhull-v${QHULL_VERSION}")

if(DEFINED QHULL_BUILD_TYPE)
    set(QHULL_ACTUAL_BUILD_TYPE "${QHULL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(QHULL_ACTUAL_BUILD_TYPE "Release")
    else()
        set(QHULL_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(QHULL_ACTUAL_BUILD_TYPE "Release")
endif()

set(QHULL_BUILD_DIR   "${QHULL_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${QHULL_ACTUAL_BUILD_TYPE}")
set(QHULL_INSTALL_DIR "${QHULL_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${QHULL_ACTUAL_BUILD_TYPE}")
set(QHULL_SOURCE_DIR  "${QHULL_CACHE_DIR}/src")
set(QHULL_ZIP_FILE    "${QHULL_CACHE_DIR}/qhull-${QHULL_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 Qhull 库缓存管理")
message(STATUS "   Qhull 版本: ${QHULL_VERSION}")
message(STATUS "   构建类型: ${QHULL_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存目录: ${QHULL_CACHE_DIR}")
message(STATUS "   构建目录: ${QHULL_BUILD_DIR}")
message(STATUS "   安装目录: ${QHULL_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${QHULL_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

if(EXISTS "${QHULL_INSTALL_DIR}/include/libqhull_r/libqhull_r.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${QHULL_INSTALL_DIR}")
endif()

if(EXISTS "${QHULL_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${QHULL_BUILD_DIR}")
endif()

if(EXISTS "${QHULL_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${QHULL_SOURCE_DIR}")
endif()

if(EXISTS "${QHULL_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${QHULL_ZIP_FILE}")
endif()

set(QHULL_STATUS "NONE")
set(QHULL_FOUND FALSE)

if(HAS_INSTALL)
    set(QHULL_STATUS "INSTALLED")
    set(QHULL_FOUND TRUE)
elseif(HAS_BUILD)
    set(QHULL_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(QHULL_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(QHULL_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${QHULL_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(QHULL_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 Qhull 库，跳过下载和编译")

elseif(QHULL_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 Qhull...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${QHULL_BUILD_DIR} --target install --config ${QHULL_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${QHULL_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ Qhull 安装成功")
        set(QHULL_FOUND TRUE)
    else()
        message(WARNING "❌ Qhull 安装失败，重新构建...")
        file(REMOVE_RECURSE "${QHULL_BUILD_DIR}")
        set(QHULL_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(QHULL_STATUS STREQUAL "SOURCE_ONLY")
    message(STATUS "🔨 开始构建 Qhull...")

    set(QHULL_GENERATOR "")
    set(QHULL_GENERATOR_PLATFORM "")
    set(QHULL_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(QHULL_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(QHULL_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(QHULL_GENERATOR "Ninja")
            set(QHULL_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(QHULL_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(QHULL_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(QHULL_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${QHULL_GENERATOR}")
    if(QHULL_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${QHULL_GENERATOR_PLATFORM})
    endif()
    if(QHULL_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${QHULL_MAKE_PROGRAM})
    endif()
    if(QHULL_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${QHULL_SOURCE_DIR}
            -B ${QHULL_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${QHULL_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${QHULL_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
            -DINCLUDE_INSTALL_DIR=include
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ Qhull 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ Qhull 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${QHULL_BUILD_DIR} --config ${QHULL_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${QHULL_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ Qhull 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ Qhull 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${QHULL_BUILD_DIR} --target install --config ${QHULL_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${QHULL_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ Qhull 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ Qhull 安装成功")
    set(QHULL_FOUND TRUE)

elseif(QHULL_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 Qhull 源码...")

    if(EXISTS "${QHULL_SOURCE_DIR}")
        file(REMOVE_RECURSE "${QHULL_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${QHULL_ZIP_FILE}
        WORKING_DIRECTORY ${QHULL_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        file(GLOB QHULL_EXTRACTED_DIRS "${QHULL_CACHE_DIR}/qhull-*")
        list(FILTER QHULL_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH QHULL_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET QHULL_EXTRACTED_DIRS 0 QHULL_EXTRACTED_DIR)
            file(RENAME "${QHULL_EXTRACTED_DIR}" "${QHULL_SOURCE_DIR}")
            message(STATUS "✅ Qhull 解压成功")
            set(QHULL_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 qhull-* 目录")
        endif()
    else()
        message(WARNING "❌ Qhull 解压失败: ${extract_error}")
        file(REMOVE "${QHULL_ZIP_FILE}")
        set(QHULL_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 Qhull ${QHULL_VERSION}...")
    message(STATUS "   URL: ${QHULL_URL}")

    file(DOWNLOAD
        ${QHULL_URL}
        ${QHULL_ZIP_FILE}
        EXPECTED_HASH ${QHULL_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${QHULL_ZIP_FILE}")
        message(FATAL_ERROR "❌ Qhull 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ Qhull 下载成功")

    set(QHULL_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(QHULL_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${QHULL_INSTALL_DIR}")
    set(Qhull_ROOT "${QHULL_INSTALL_DIR}" CACHE PATH "Qhull root" FORCE)
    if(EXISTS "${QHULL_INSTALL_DIR}/lib/cmake/Qhull/QhullConfig.cmake")
        set(Qhull_DIR "${QHULL_INSTALL_DIR}/lib/cmake/Qhull" CACHE PATH "Qhull config dir" FORCE)
    endif()
    set(QHULL_AVAILABLE TRUE CACHE BOOL "Qhull library is available")
    message(STATUS "🎯 Qhull 就绪 (${QHULL_VERSION}) -> ${QHULL_INSTALL_DIR}")
else()
    set(QHULL_AVAILABLE FALSE CACHE BOOL "Qhull library is not available")
    message(FATAL_ERROR "⚠️  Qhull 库不可用")
endif()
