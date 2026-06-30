cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Boost (modular CMake build, 1.84.0)
#
# Depends on the zlib built by deps/ZLIB/ZLIB.cmake (boost::iostreams uses it).
# Note that BOOST_EXCLUDE_LIBRARIES is a CMake list value; the original flow
# used ExternalProject's LIST_SEPARATOR=| trick to feed it through. For our
# execute_process-based flow we keep the literal semicolons by writing the
# whole -D argument with bracket syntax and passing it quoted.
# ----------------------------------------------------------------------------

if(NOT DEFINED BOOST_VERSION)
    set(BOOST_VERSION "1.84.0" CACHE STRING "Boost version to build")
endif()

set(BOOST_URL      "https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}.tar.gz")
set(BOOST_URL_HASH "SHA256=4d27e9efed0f6f152dc28db6430b9d3dfb40c0345da7342eaa5a987dde57bd95")

if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE environment variable is not set. "
        "The top-level CMakeLists.txt should provide a default.")
endif()

if(NOT ZLIB_AVAILABLE OR NOT ZLIB_ROOT)
    message(FATAL_ERROR
        "Boost requires ZLIB to be fetched first. "
        "Include deps/ZLIB/ZLIB.cmake before deps/Boost/Boost.cmake in fetch_deps.cmake.")
endif()

file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(BOOST_CACHE_DIR "${FETCH_CACHE_DIR}/boost-v${BOOST_VERSION}")

if(DEFINED BOOST_BUILD_TYPE)
    set(BOOST_ACTUAL_BUILD_TYPE "${BOOST_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(BOOST_ACTUAL_BUILD_TYPE "Release")
    else()
        set(BOOST_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    set(BOOST_ACTUAL_BUILD_TYPE "Release")
endif()

set(BOOST_BUILD_DIR   "${BOOST_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${BOOST_ACTUAL_BUILD_TYPE}")
set(BOOST_INSTALL_DIR "${BOOST_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${BOOST_ACTUAL_BUILD_TYPE}")
set(BOOST_SOURCE_DIR  "${BOOST_CACHE_DIR}/src")
set(BOOST_TAR_FILE    "${BOOST_CACHE_DIR}/boost-${BOOST_VERSION}.tar.gz")

message(STATUS "")
message(STATUS "🔧 Boost 库缓存管理")
message(STATUS "   Boost 版本: ${BOOST_VERSION}")
message(STATUS "   构建类型: ${BOOST_ACTUAL_BUILD_TYPE}")
message(STATUS "   ZLIB 路径: ${ZLIB_ROOT}")
message(STATUS "   缓存目录: ${BOOST_CACHE_DIR}")
message(STATUS "   构建目录: ${BOOST_BUILD_DIR}")
message(STATUS "   安装目录: ${BOOST_INSTALL_DIR}")

file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${BOOST_CACHE_DIR}")

# ---- State detection -------------------------------------------------------
set(HAS_INSTALL FALSE)
set(HAS_BUILD   FALSE)
set(HAS_SOURCE  FALSE)
set(HAS_ZIP     FALSE)

# Boost CMake 模块构建在 Windows 上将头文件安装到带版本号的子目录
# 例如 include/boost-1_84/boost/version.hpp，而非 include/boost/version.hpp
file(GLOB _boost_version_hpp_candidates
    "${BOOST_INSTALL_DIR}/include/boost/version.hpp"
    "${BOOST_INSTALL_DIR}/include/boost-*/boost/version.hpp"
)
if(_boost_version_hpp_candidates)
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${BOOST_INSTALL_DIR}")
endif()

if(EXISTS "${BOOST_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    message(STATUS "📁 发现构建目录: ${BOOST_BUILD_DIR}")
endif()

if(EXISTS "${BOOST_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${BOOST_SOURCE_DIR}")
endif()

if(EXISTS "${BOOST_TAR_FILE}")
    set(HAS_ZIP TRUE)
    message(STATUS "📦 发现压缩包: ${BOOST_TAR_FILE}")
endif()

set(BOOST_STATUS "NONE")
set(BOOST_FOUND FALSE)

if(HAS_INSTALL)
    set(BOOST_STATUS "INSTALLED")
    set(BOOST_FOUND TRUE)
elseif(HAS_BUILD)
    set(BOOST_STATUS "BUILT_NOT_INSTALLED")
elseif(HAS_SOURCE)
    set(BOOST_STATUS "SOURCE_ONLY")
elseif(HAS_ZIP)
    set(BOOST_STATUS "ZIP_ONLY")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${BOOST_STATUS}")

# ---- Strategy execution ----------------------------------------------------
if(BOOST_STATUS STREQUAL "INSTALLED")
    message(STATUS "🚀 使用缓存的 Boost 库，跳过下载和编译")

elseif(BOOST_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    message(STATUS "📦 开始安装 Boost...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${BOOST_BUILD_DIR} --target install --config ${BOOST_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${BOOST_BUILD_DIR}
    )
    if(install_result EQUAL 0)
        message(STATUS "✅ Boost 安装成功")
        set(BOOST_FOUND TRUE)
    else()
        message(WARNING "❌ Boost 安装失败，重新构建...")
        file(REMOVE_RECURSE "${BOOST_BUILD_DIR}")
        set(BOOST_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

elseif(BOOST_STATUS STREQUAL "SOURCE_ONLY")
    message(STATUS "🔨 开始构建 Boost (这可能需要 10+ 分钟)...")

    set(BOOST_GENERATOR "")
    set(BOOST_GENERATOR_PLATFORM "")
    set(BOOST_MAKE_PROGRAM "")

    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(BOOST_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(BOOST_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
    else()
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(BOOST_GENERATOR "Ninja")
            set(BOOST_MAKE_PROGRAM "${NINJA_EXECUTABLE}")
        elseif(CMAKE_GENERATOR)
            set(BOOST_GENERATOR "${CMAKE_GENERATOR}")
            if(CMAKE_MAKE_PROGRAM)
                set(BOOST_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        else()
            set(BOOST_GENERATOR "Unix Makefiles")
        endif()
    endif()

    set(GENERATOR_ARGS -G "${BOOST_GENERATOR}")
    if(BOOST_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${BOOST_GENERATOR_PLATFORM})
    endif()
    if(BOOST_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${BOOST_MAKE_PROGRAM})
    endif()
    if(BOOST_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # Apple-specific context settings (mirrors original Boost.cmake).
    set(BOOST_EXTRA_ARGS "")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        if(CMAKE_OSX_ARCHITECTURES MATCHES "x86")
            list(APPEND BOOST_EXTRA_ARGS -DBOOST_CONTEXT_ABI=sysv)
        elseif(CMAKE_OSX_ARCHITECTURES MATCHES "arm")
            list(APPEND BOOST_EXTRA_ARGS -DBOOST_CONTEXT_ABI=aapcs)
        endif()
        list(APPEND BOOST_EXTRA_ARGS -DBOOST_CONTEXT_ARCHITECTURE=${CMAKE_OSX_ARCHITECTURES})
    endif()

    # Bracket-quote (verbatim) preserves the embedded semicolons so the child
    # cmake receives a single -D argument whose value is the full list.
    set(_boost_exclude_arg [[-DBOOST_EXCLUDE_LIBRARIES=contract;fiber;numpy;stacktrace;wave;test]])

    # Propagate prefix path so any nested find_package (e.g. ZLIB) resolves.
    set(_prefix_path_escaped "${CMAKE_PREFIX_PATH}")
    string(REPLACE ";" "\\;" _prefix_path_escaped "${_prefix_path_escaped}")

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${BOOST_SOURCE_DIR}
            -B ${BOOST_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${BOOST_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${BOOST_INSTALL_DIR}
            -DCMAKE_POLICY_VERSION_MINIMUM=3.5
            -DBUILD_SHARED_LIBS=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_DEBUG_POSTFIX=d
            -DCMAKE_PREFIX_PATH=${_prefix_path_escaped}
            -DZLIB_ROOT=${ZLIB_ROOT}
            "${_boost_exclude_arg}"
            -DBOOST_LOCALE_ENABLE_ICU=OFF
            -DBUILD_TESTING=OFF
            -DBOOST_IOSTREAMS_ENABLE_ZSTD=OFF
            -DBOOST_IOSTREAMS_ENABLE_BZIP2=OFF
            -DBOOST_IOSTREAMS_ENABLE_LZMA=OFF
            -DBOOST_LOCALE_ENABLE_ICONV=OFF
            ${BOOST_EXTRA_ARGS}
        RESULT_VARIABLE config_result
    )
    if(NOT config_result EQUAL 0)
        message(FATAL_ERROR "❌ Boost 配置失败，返回码: ${config_result}")
    endif()
    message(STATUS "✅ Boost 配置成功")

    include(ProcessorCount)
    ProcessorCount(N_CORES)
    if(N_CORES EQUAL 0)
        set(N_CORES 4)
    endif()
    message(STATUS "   使用 ${N_CORES} 个并行任务")
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${BOOST_BUILD_DIR} --config ${BOOST_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
        RESULT_VARIABLE build_result
        WORKING_DIRECTORY ${BOOST_BUILD_DIR}
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "❌ Boost 编译失败，返回码: ${build_result}")
    endif()
    message(STATUS "✅ Boost 编译成功")

    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${BOOST_BUILD_DIR} --target install --config ${BOOST_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${BOOST_BUILD_DIR}
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR "❌ Boost 安装失败，返回码: ${install_result}")
    endif()
    message(STATUS "✅ Boost 安装成功")
    set(BOOST_FOUND TRUE)

elseif(BOOST_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 Boost 源码...")

    if(EXISTS "${BOOST_SOURCE_DIR}")
        file(REMOVE_RECURSE "${BOOST_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${BOOST_TAR_FILE}
        WORKING_DIRECTORY ${BOOST_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # Archive extracts to boost-1.84.0/
        file(GLOB BOOST_EXTRACTED_DIRS "${BOOST_CACHE_DIR}/boost-*")
        list(FILTER BOOST_EXTRACTED_DIRS EXCLUDE REGEX "\\.tar\\.gz$")
        list(FILTER BOOST_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(LENGTH BOOST_EXTRACTED_DIRS _n_extracted)
        if(_n_extracted GREATER 0)
            list(GET BOOST_EXTRACTED_DIRS 0 BOOST_EXTRACTED_DIR)
            file(RENAME "${BOOST_EXTRACTED_DIR}" "${BOOST_SOURCE_DIR}")
            message(STATUS "✅ Boost 解压成功")
            set(BOOST_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
            return()
        else()
            message(FATAL_ERROR "❌ 解压后未找到 boost-* 目录")
        endif()
    else()
        message(WARNING "❌ Boost 解压失败: ${extract_error}")
        file(REMOVE "${BOOST_TAR_FILE}")
        set(BOOST_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    endif()

else() # NONE
    message(STATUS "⬇️  开始下载 Boost ${BOOST_VERSION} (约 150MB)...")
    message(STATUS "   URL: ${BOOST_URL}")

    file(DOWNLOAD
        ${BOOST_URL}
        ${BOOST_TAR_FILE}
        EXPECTED_HASH ${BOOST_URL_HASH}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)
    if(NOT status_code EQUAL 0)
        file(REMOVE "${BOOST_TAR_FILE}")
        message(FATAL_ERROR "❌ Boost 下载失败: ${status_msg}\n   ${download_log}")
    endif()
    message(STATUS "✅ Boost 下载成功")

    set(BOOST_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()
endif()

# ---- Register for find_package --------------------------------------------
if(BOOST_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${BOOST_INSTALL_DIR}")
    set(BOOST_ROOT  "${BOOST_INSTALL_DIR}" CACHE PATH "boost root" FORCE)
    set(Boost_ROOT  "${BOOST_INSTALL_DIR}" CACHE PATH "Boost root (modern hint)" FORCE)
    # Hint the modular CMake config layout so find_package(Boost CONFIG) finds it.
    file(GLOB _boost_cfg_dirs "${BOOST_INSTALL_DIR}/lib/cmake/Boost-*")
    list(LENGTH _boost_cfg_dirs _n_cfg)
    if(_n_cfg GREATER 0)
        list(GET _boost_cfg_dirs 0 _boost_cfg_dir)
        set(Boost_DIR "${_boost_cfg_dir}" CACHE PATH "Boost config dir" FORCE)
    endif()
    set(BOOST_AVAILABLE TRUE CACHE BOOL "Boost library is available")
    message(STATUS "🎯 Boost 就绪 (${BOOST_VERSION}) -> ${BOOST_INSTALL_DIR}")
else()
    set(BOOST_AVAILABLE FALSE CACHE BOOL "Boost library is not available")
    message(FATAL_ERROR "⚠️  Boost 库不可用")
endif()
