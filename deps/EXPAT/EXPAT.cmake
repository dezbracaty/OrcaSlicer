cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# EXPAT (static build from vendored source at deps/EXPAT/expat/)
#
# Unlike the other fetch scripts this one has no download/extract/patch path:
# the source is already in-tree and pinned to a specific snapshot. Only the
# build and install directories live in the shared fetch cache, keyed by
# the current build type so they don't collide across Debug/Release runs.
# ----------------------------------------------------------------------------

# Pseudo-version used only to key the cache directory. Bump if the vendored
# source is replaced with a different upstream snapshot.
if(NOT DEFINED EXPAT_VERSION)
    set(EXPAT_VERSION "vendored" CACHE STRING "EXPAT version tag (vendored source)")
endif()

set(EXPAT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/expat")

if(NOT EXISTS "${EXPAT_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "EXPAT vendored source not found at ${EXPAT_SOURCE_DIR}")
endif()

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(EXPAT_CACHE_DIR "${FETCH_CACHE_DIR}/expat-${EXPAT_VERSION}")

if(DEFINED EXPAT_BUILD_TYPE)
    set(EXPAT_ACTUAL_BUILD_TYPE "${EXPAT_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(EXPAT_ACTUAL_BUILD_TYPE "Release")
    else()
        set(EXPAT_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(EXPAT_ACTUAL_BUILD_TYPE "Release")
endif()

set(EXPAT_BUILD_DIR   "${EXPAT_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${EXPAT_ACTUAL_BUILD_TYPE}")
set(EXPAT_INSTALL_DIR "${EXPAT_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${EXPAT_ACTUAL_BUILD_TYPE}")

message(STATUS "")
message(STATUS "🔧 EXPAT 库缓存管理")
message(STATUS "   EXPAT 版本: ${EXPAT_VERSION} (vendored)")
message(STATUS "   构建类型: ${EXPAT_ACTUAL_BUILD_TYPE}")
message(STATUS "   源码目录: ${EXPAT_SOURCE_DIR}")
message(STATUS "   构建目录: ${EXPAT_BUILD_DIR}")
message(STATUS "   安装目录: ${EXPAT_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${EXPAT_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)

# EXPAT installs expat.h and an expat-config.cmake in lib/cmake/expat/.
# The header is the simplest reliable marker.
if(EXISTS "${EXPAT_INSTALL_DIR}/include/expat.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${EXPAT_INSTALL_DIR}")
endif()

if(EXISTS "${EXPAT_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${EXPAT_BUILD_DIR}")
endif()

set(EXPAT_STATUS "SOURCE_ONLY")
set(EXPAT_FOUND FALSE)

if(HAS_INSTALL)
    set(EXPAT_STATUS "INSTALLED")
    set(EXPAT_FOUND TRUE)
elseif(HAS_BUILD)
    set(EXPAT_STATUS "BUILT_NOT_INSTALLED")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}")
message(STATUS "🎯 执行策略: ${EXPAT_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(EXPAT_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 EXPAT 库，跳过编译")

elseif(EXPAT_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 EXPAT...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${EXPAT_BUILD_DIR} --target install --config ${EXPAT_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${EXPAT_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ EXPAT 安装成功")
        set(EXPAT_FOUND TRUE)
    else()
        message(WARNING "❌ EXPAT 安装失败，重新构建...")
        file(REMOVE_RECURSE "${EXPAT_BUILD_DIR}")
        set(EXPAT_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # SOURCE_ONLY
    message(STATUS "🔨 开始构建 EXPAT...")

    set(EXPAT_GENERATOR "")
    set(EXPAT_GENERATOR_PLATFORM "")
    set(EXPAT_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(EXPAT_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(EXPAT_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(EXPAT_GENERATOR "Ninja")
            set(EXPAT_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(EXPAT_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(EXPAT_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(EXPAT_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${EXPAT_GENERATOR}")
    if(EXPAT_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${EXPAT_GENERATOR_PLATFORM})
    endif()
    if(EXPAT_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${EXPAT_MAKE_PROGRAM})
    endif()
    if(EXPAT_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${EXPAT_SOURCE_DIR}
            -B ${EXPAT_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${EXPAT_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${EXPAT_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ EXPAT 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ EXPAT 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${EXPAT_BUILD_DIR} --config ${EXPAT_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${EXPAT_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ EXPAT 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ EXPAT 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${EXPAT_BUILD_DIR} --target install --config ${EXPAT_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${EXPAT_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ EXPAT 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ EXPAT 安装成功")
    set(EXPAT_FOUND TRUE)
endif()

# ---- Register for find_package --------------------------------------------
if(EXPAT_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${EXPAT_INSTALL_DIR}")
    set(EXPAT_ROOT "${EXPAT_INSTALL_DIR}" CACHE PATH "expat root" FORCE)
    set(EXPAT_AVAILABLE TRUE CACHE BOOL "EXPAT library is available")
    message(STATUS "🎯 EXPAT 就绪 (${EXPAT_VERSION}) -> ${EXPAT_INSTALL_DIR}")
else()
    set(EXPAT_AVAILABLE FALSE CACHE BOOL "EXPAT library is not available")
    message(FATAL_ERROR "⚠️  EXPAT 库不可用")
endif()
