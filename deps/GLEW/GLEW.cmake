cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# GLEW (vendored source at deps/GLEW/glew/, static build)
#
# Requires a system OpenGL — checked here so the failure is reported by this
# script rather than later from the GLEW configure log.
# ----------------------------------------------------------------------------

if(NOT DEFINED GLEW_PSEUDO_VERSION)
    set(GLEW_PSEUDO_VERSION "vendored" CACHE STRING "GLEW version tag (vendored source)")
endif()

set(GLEW_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/glew")

if(NOT EXISTS "${GLEW_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "GLEW vendored source not found at ${GLEW_SOURCE_DIR}")
endif()

# Suppress the LEGACY/GLVND preference warning emitted by FindOpenGL on Linux.
set(OpenGL_GL_PREFERENCE "LEGACY")
find_package(OpenGL QUIET REQUIRED)

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(GLEW_CACHE_DIR "${FETCH_CACHE_DIR}/glew-${GLEW_PSEUDO_VERSION}")

if(DEFINED GLEW_BUILD_TYPE_OVERRIDE)
    set(GLEW_ACTUAL_BUILD_TYPE "${GLEW_BUILD_TYPE_OVERRIDE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(GLEW_ACTUAL_BUILD_TYPE "Release")
    else()
        set(GLEW_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(GLEW_ACTUAL_BUILD_TYPE "Release")
endif()

set(GLEW_BUILD_DIR   "${GLEW_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${GLEW_ACTUAL_BUILD_TYPE}")
set(GLEW_INSTALL_DIR "${GLEW_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${GLEW_ACTUAL_BUILD_TYPE}")

message(STATUS "")
message(STATUS "🔧 GLEW 库缓存管理")
message(STATUS "   GLEW: ${GLEW_PSEUDO_VERSION} (vendored)")
message(STATUS "   构建类型: ${GLEW_ACTUAL_BUILD_TYPE}")
message(STATUS "   源码目录: ${GLEW_SOURCE_DIR}")
message(STATUS "   构建目录: ${GLEW_BUILD_DIR}")
message(STATUS "   安装目录: ${GLEW_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${GLEW_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)

if(EXISTS "${GLEW_INSTALL_DIR}/include/GL/glew.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${GLEW_INSTALL_DIR}")
endif()

if(EXISTS "${GLEW_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${GLEW_BUILD_DIR}")
endif()

set(GLEW_STATUS "SOURCE_ONLY")
set(GLEW_FOUND FALSE)

if(HAS_INSTALL)
    set(GLEW_STATUS "INSTALLED")
    set(GLEW_FOUND TRUE)
elseif(HAS_BUILD)
    set(GLEW_STATUS "BUILT_NOT_INSTALLED")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}")
message(STATUS "🎯 执行策略: ${GLEW_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(GLEW_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 GLEW 库，跳过编译")

elseif(GLEW_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 GLEW...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GLEW_BUILD_DIR} --target install --config ${GLEW_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${GLEW_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ GLEW 安装成功")
        set(GLEW_FOUND TRUE)
    else()
        message(WARNING "❌ GLEW 安装失败，重新构建...")
        file(REMOVE_RECURSE "${GLEW_BUILD_DIR}")
        set(GLEW_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # SOURCE_ONLY
    message(STATUS "🔨 开始构建 GLEW...")

    set(GLEW_GENERATOR "")
    set(GLEW_GENERATOR_PLATFORM "")
    set(GLEW_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(GLEW_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(GLEW_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(GLEW_GENERATOR "Ninja")
            set(GLEW_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(GLEW_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(GLEW_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(GLEW_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${GLEW_GENERATOR}")
    if(GLEW_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${GLEW_GENERATOR_PLATFORM})
    endif()
    if(GLEW_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${GLEW_MAKE_PROGRAM})
    endif()
    if(GLEW_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${GLEW_SOURCE_DIR}
            -B ${GLEW_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${GLEW_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${GLEW_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
            -DOpenGL_GL_PREFERENCE=LEGACY
            -DGLEW_USE_EGL=OFF
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ GLEW 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ GLEW 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GLEW_BUILD_DIR} --config ${GLEW_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${GLEW_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ GLEW 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ GLEW 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GLEW_BUILD_DIR} --target install --config ${GLEW_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${GLEW_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ GLEW 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ GLEW 安装成功")
    set(GLEW_FOUND TRUE)
endif()

# ---- Register for find_package --------------------------------------------
if(GLEW_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${GLEW_INSTALL_DIR}")
    set(GLEW_ROOT "${GLEW_INSTALL_DIR}" CACHE PATH "GLEW root" FORCE)
    if(EXISTS "${GLEW_INSTALL_DIR}/lib/cmake/glew/glew-config.cmake")
        set(glew_DIR "${GLEW_INSTALL_DIR}/lib/cmake/glew" CACHE PATH "GLEW config dir" FORCE)
    endif()
    set(GLEW_AVAILABLE TRUE CACHE BOOL "GLEW library is available")
    message(STATUS "🎯 GLEW 就绪 -> ${GLEW_INSTALL_DIR}")
else()
    set(GLEW_AVAILABLE FALSE CACHE BOOL "GLEW library is not available")
    message(FATAL_ERROR "⚠️  GLEW 库不可用")
endif()
