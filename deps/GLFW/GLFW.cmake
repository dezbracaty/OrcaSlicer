cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# GLFW (windowing/OpenGL context, static build)
# ----------------------------------------------------------------------------

if(NOT DEFINED GLFW_VERSION)
    set(GLFW_VERSION "3.3.7" CACHE STRING "GLFW version to build")
endif()

set(GLFW_URL      "https://github.com/glfw/glfw/archive/refs/tags/${GLFW_VERSION}.zip")
set(GLFW_URL_HASH "SHA256=e02d956935e5b9fb4abf90e2c2e07c9a0526d7eacae8ee5353484c69a2a76cd0")

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(GLFW_CACHE_DIR "${FETCH_CACHE_DIR}/glfw-v${GLFW_VERSION}")

if(DEFINED GLFW_BUILD_TYPE_OVERRIDE)
    set(GLFW_ACTUAL_BUILD_TYPE "${GLFW_BUILD_TYPE_OVERRIDE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(GLFW_ACTUAL_BUILD_TYPE "Release")
    else()
        set(GLFW_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(GLFW_ACTUAL_BUILD_TYPE "Release")
endif()

set(GLFW_BUILD_DIR   "${GLFW_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${GLFW_ACTUAL_BUILD_TYPE}")
set(GLFW_INSTALL_DIR "${GLFW_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${GLFW_ACTUAL_BUILD_TYPE}")
set(GLFW_SOURCE_DIR  "${GLFW_CACHE_DIR}/src")
set(GLFW_ZIP_FILE    "${GLFW_CACHE_DIR}/glfw-${GLFW_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 GLFW 库缓存管理")
message(STATUS "   GLFW 版本: ${GLFW_VERSION}")
message(STATUS "   构建类型: ${GLFW_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存目录: ${GLFW_CACHE_DIR}")
message(STATUS "   构建目录: ${GLFW_BUILD_DIR}")
message(STATUS "   安装目录: ${GLFW_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${GLFW_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

if(EXISTS "${GLFW_INSTALL_DIR}/include/GLFW/glfw3.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${GLFW_INSTALL_DIR}")
endif()

if(EXISTS "${GLFW_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${GLFW_BUILD_DIR}")
endif()

if(EXISTS "${GLFW_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${GLFW_SOURCE_DIR}")
endif()

if(EXISTS "${GLFW_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${GLFW_ZIP_FILE}")
endif()

set(GLFW_STATUS "NONE")
set(GLFW_FOUND FALSE)

if(HAS_INSTALL)
    set(GLFW_STATUS "INSTALLED")
    set(GLFW_FOUND TRUE)
elseif(HAS_BUILD)
    set(GLFW_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(GLFW_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(GLFW_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${GLFW_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(GLFW_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 GLFW 库，跳过下载和编译")

elseif(GLFW_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 GLFW...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GLFW_BUILD_DIR} --target install --config ${GLFW_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${GLFW_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ GLFW 安装成功")
        set(GLFW_FOUND TRUE)
    else()
        message(WARNING "❌ GLFW 安装失败，重新构建...")
        file(REMOVE_RECURSE "${GLFW_BUILD_DIR}")
        set(GLFW_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(GLFW_STATUS STREQUAL "SOURCE_ONLY")
    message(STATUS "🔨 开始构建 GLFW...")

    set(GLFW_GENERATOR "")
    set(GLFW_GENERATOR_PLATFORM "")
    set(GLFW_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(GLFW_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(GLFW_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(GLFW_GENERATOR "Ninja")
            set(GLFW_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(GLFW_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(GLFW_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(GLFW_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${GLFW_GENERATOR}")
    if(GLFW_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${GLFW_GENERATOR_PLATFORM})
    endif()
    if(GLFW_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${GLFW_MAKE_PROGRAM})
    endif()
    if(GLFW_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # Linux/Wayland; original had a typo (=FF) which CMake silently treated as
    # false. Use OFF explicitly here.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_glfw_wayland_arg -DGLFW_USE_WAYLAND=ON)
    else()
        set(_glfw_wayland_arg -DGLFW_USE_WAYLAND=OFF)
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${GLFW_SOURCE_DIR}
            -B ${GLFW_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${GLFW_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${GLFW_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
            -DGLFW_BUILD_DOCS=OFF
            -DGLFW_BUILD_EXAMPLES=OFF
            -DGLFW_BUILD_TESTS=OFF
            ${_glfw_wayland_arg}
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ GLFW 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ GLFW 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GLFW_BUILD_DIR} --config ${GLFW_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${GLFW_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ GLFW 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ GLFW 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GLFW_BUILD_DIR} --target install --config ${GLFW_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${GLFW_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ GLFW 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ GLFW 安装成功")
    set(GLFW_FOUND TRUE)

elseif(GLFW_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 GLFW 源码...")

    if(EXISTS "${GLFW_SOURCE_DIR}")
        file(REMOVE_RECURSE "${GLFW_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${GLFW_ZIP_FILE}
        WORKING_DIRECTORY ${GLFW_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        file(GLOB GLFW_EXTRACTED_DIRS "${GLFW_CACHE_DIR}/glfw-*")
        list(FILTER GLFW_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH GLFW_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET GLFW_EXTRACTED_DIRS 0 GLFW_EXTRACTED_DIR)
            file(RENAME "${GLFW_EXTRACTED_DIR}" "${GLFW_SOURCE_DIR}")
            message(STATUS "✅ GLFW 解压成功")
            set(GLFW_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 glfw-* 目录")
        endif()
    else()
        message(WARNING "❌ GLFW 解压失败: ${extract_error}")
        file(REMOVE "${GLFW_ZIP_FILE}")
        set(GLFW_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 GLFW ${GLFW_VERSION}...")
    message(STATUS "   URL: ${GLFW_URL}")

    file(DOWNLOAD
        ${GLFW_URL}
        ${GLFW_ZIP_FILE}
        EXPECTED_HASH ${GLFW_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${GLFW_ZIP_FILE}")
        message(FATAL_ERROR "❌ GLFW 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ GLFW 下载成功")

    set(GLFW_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(GLFW_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${GLFW_INSTALL_DIR}")
    set(glfw3_ROOT "${GLFW_INSTALL_DIR}" CACHE PATH "GLFW root" FORCE)
    if(EXISTS "${GLFW_INSTALL_DIR}/lib/cmake/glfw3/glfw3Config.cmake")
        set(glfw3_DIR "${GLFW_INSTALL_DIR}/lib/cmake/glfw3" CACHE PATH "GLFW config dir" FORCE)
    endif()
    set(GLFW_AVAILABLE TRUE CACHE BOOL "GLFW library is available")
    message(STATUS "🎯 GLFW 就绪 (${GLFW_VERSION}) -> ${GLFW_INSTALL_DIR}")
else()
    set(GLFW_AVAILABLE FALSE CACHE BOOL "GLFW library is not available")
    message(FATAL_ERROR "⚠️  GLFW 库不可用")
endif()
