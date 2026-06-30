cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED EIGEN_VERSION)
    set(EIGEN_VERSION "5.0.1" CACHE STRING "Eigen version to build")
endif()

set(EIGEN_URL      "https://gitlab.com/libeigen/eigen/-/archive/${EIGEN_VERSION}/eigen-${EIGEN_VERSION}.zip")
set(EIGEN_URL_HASH "SHA256=0dbb1f9e3aaad66f352c03227d8c983f6f0b49e0b07e71a7300f4abcc01aee12")

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(EIGEN_CACHE_DIR "${FETCH_CACHE_DIR}/eigen-v${EIGEN_VERSION}")

if(DEFINED EIGEN_BUILD_TYPE)
    set(EIGEN_ACTUAL_BUILD_TYPE "${EIGEN_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    set(EIGEN_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
else()
    set(EIGEN_ACTUAL_BUILD_TYPE "Release")
endif()

set(EIGEN_BUILD_DIR   "${EIGEN_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${EIGEN_ACTUAL_BUILD_TYPE}")
set(EIGEN_INSTALL_DIR "${EIGEN_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${EIGEN_ACTUAL_BUILD_TYPE}")
set(EIGEN_SOURCE_DIR  "${EIGEN_CACHE_DIR}/src")
set(EIGEN_ZIP_FILE    "${EIGEN_CACHE_DIR}/eigen-${EIGEN_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 Eigen 库缓存管理")
message(STATUS "   Eigen 版本: ${EIGEN_VERSION}")
message(STATUS "   构建类型: ${EIGEN_ACTUAL_BUILD_TYPE}")
message(STATUS "   缓存目录: ${EIGEN_CACHE_DIR}")
message(STATUS "   构建目录: ${EIGEN_BUILD_DIR}")
message(STATUS "   安装目录: ${EIGEN_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${EIGEN_CACHE_DIR}")

set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

if(EXISTS "${EIGEN_INSTALL_DIR}/include/eigen3/Eigen/Core")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${EIGEN_INSTALL_DIR}")
endif()

if(EXISTS "${EIGEN_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${EIGEN_BUILD_DIR}")
endif()

if(EXISTS "${EIGEN_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${EIGEN_SOURCE_DIR}")
endif()

if(EXISTS "${EIGEN_ZIP_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${EIGEN_ZIP_FILE}")
endif()

set(EIGEN_STATUS "NONE")
set(EIGEN_FOUND FALSE)

if(HAS_INSTALL)
    set(EIGEN_STATUS "INSTALLED")
    set(EIGEN_FOUND TRUE)
elseif(HAS_BUILD)
    set(EIGEN_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(EIGEN_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(EIGEN_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${EIGEN_STATUS}")

if(EIGEN_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 Eigen 库，跳过下载和编译")

elseif(EIGEN_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 Eigen...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${EIGEN_BUILD_DIR} --target install --config ${EIGEN_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${EIGEN_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        set(EIGEN_FOUND TRUE)
        message(STATUS "✅ Eigen 安装成功")
    else()
        message(WARNING "❌ Eigen 安装失败，重新构建...")
        file(REMOVE_RECURSE "${EIGEN_BUILD_DIR}")
        set(EIGEN_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(EIGEN_STATUS STREQUAL "SOURCE_ONLY")
    message(STATUS "🔨 开始构建 Eigen...")

    set(EIGEN_GENERATOR "")
    set(EIGEN_GENERATOR_PLATFORM "")
    set(EIGEN_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(EIGEN_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(EIGEN_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(EIGEN_GENERATOR "Ninja")
            set(EIGEN_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(EIGEN_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(EIGEN_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(EIGEN_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${EIGEN_GENERATOR}")
    if(EIGEN_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${EIGEN_GENERATOR_PLATFORM})
    endif()
    if(EIGEN_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${EIGEN_MAKE_PROGRAM})
    endif()
    if(EIGEN_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    set(_eigen_extra_flags "")
    if(MSVC)
        list(APPEND _eigen_extra_flags "-DCMAKE_CXX_FLAGS:STRING=/bigobj")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${EIGEN_SOURCE_DIR}
            -B ${EIGEN_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            ${_eigen_extra_flags}
            -DCMAKE_BUILD_TYPE=${EIGEN_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${EIGEN_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_TESTING=OFF
            -DEIGEN_BUILD_TESTING=OFF
        RESULT_VARIABLE config_result
    )
    unset(_eigen_extra_flags)

    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ Eigen 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ Eigen 配置成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${EIGEN_BUILD_DIR} --target install --config ${EIGEN_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${EIGEN_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ Eigen 安装失败，返回码: ${install_result}")
    endif()
    set(EIGEN_FOUND TRUE)
    message(STATUS "✅ Eigen 安装成功")

elseif(EIGEN_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 Eigen 源码...")
    if(EXISTS "${EIGEN_SOURCE_DIR}")
        file(REMOVE_RECURSE "${EIGEN_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${EIGEN_ZIP_FILE}
        WORKING_DIRECTORY ${EIGEN_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        file(GLOB EIGEN_EXTRACTED_DIRS "${EIGEN_CACHE_DIR}/eigen-*")
        list(FILTER EIGEN_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH EIGEN_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET EIGEN_EXTRACTED_DIRS 0 EIGEN_EXTRACTED_DIR)
            file(RENAME "${EIGEN_EXTRACTED_DIR}" "${EIGEN_SOURCE_DIR}")
            message(STATUS "✅ Eigen 解压成功")
            set(EIGEN_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        endif()
        message(FATAL_ERROR "❌ 解压后未找到 eigen-* 目录")
    else()
        message(WARNING "❌ Eigen 解压失败: ${extract_error}")
        file(REMOVE "${EIGEN_ZIP_FILE}")
        set(EIGEN_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else()
    message(STATUS "⬇️  开始下载 Eigen ${EIGEN_VERSION}...")
    message(STATUS "   URL: ${EIGEN_URL}")

    file(DOWNLOAD
        ${EIGEN_URL}
        ${EIGEN_ZIP_FILE}
        EXPECTED_HASH ${EIGEN_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${EIGEN_ZIP_FILE}")
        message(FATAL_ERROR "❌ Eigen 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ Eigen 下载成功")

    set(EIGEN_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

if(EIGEN_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${EIGEN_INSTALL_DIR}")
    set(Eigen3_DIR "${EIGEN_INSTALL_DIR}/share/eigen3/cmake" CACHE PATH "Eigen3 config dir" FORCE)
    set(EIGEN3_INCLUDE_DIR "${EIGEN_INSTALL_DIR}/include/eigen3" CACHE PATH "Eigen include dir" FORCE)
    set(EIGEN_AVAILABLE TRUE CACHE BOOL "Eigen library is available")
    message(STATUS "🎯 Eigen 就绪 (${EIGEN_VERSION}) -> ${EIGEN_INSTALL_DIR}")
else()
    set(EIGEN_AVAILABLE FALSE CACHE BOOL "Eigen library is not available")
    message(FATAL_ERROR "⚠️  Eigen 库不可用")
endif()
