cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# MPFR版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED MPFR_VERSION)
    set(MPFR_VERSION "vendored" CACHE STRING "MPFR version label")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" MPFR_VERSION_MAJOR "${MPFR_VERSION}")

# 检查缓存环境变量
if(NOT DEFINED ENV{CMAKE_FETCH_CACHE})
    message(FATAL_ERROR
        "❌ 错误: 未设置 CMAKE_FETCH_CACHE 环境变量！\n"
        "   \n"
        "   请设置缓存目录以提高编译效率:\n"
        "   \n"
        "   🔧 设置方法:\n"
        "   export CMAKE_FETCH_CACHE=/path/to/your/cache/dir\n"
        "   \n"
        "   💡 推荐设置:\n"
        "   export CMAKE_FETCH_CACHE=$HOME/.cmake_fetch_cache\n"
        "   \n"
        "   然后重新运行 cmake 配置命令。\n"
        "   这将大大加速后续编译过程！")
endif()

# 设置路径 - 使用 file(TO_CMAKE_PATH) 确保在 Windows 上正确处理反斜杠
file(TO_CMAKE_PATH "$ENV{CMAKE_FETCH_CACHE}" FETCH_CACHE_DIR)
set(MPFR_CACHE_DIR "${FETCH_CACHE_DIR}/mpfr-${MPFR_VERSION}")
# MPFR 是 vendored 预编译二进制，"源码"就是仓库里的 deps/MPFR/mpfr/
set(_MPFR_VENDORED_DIR "${CMAKE_CURRENT_LIST_DIR}/mpfr")

# MPFR构建类型配置 - 可以独立于主项目设置
if(DEFINED MPFR_BUILD_TYPE)
    # 如果明确指定了MPFR_BUILD_TYPE，使用它
    set(MPFR_ACTUAL_BUILD_TYPE "${MPFR_BUILD_TYPE}")
    message(STATUS "   使用指定的MPFR构建类型: ${MPFR_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(MPFR_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 MPFR")
    else()
        set(MPFR_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(MPFR_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(MPFR_BUILD_DIR "${MPFR_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${MPFR_ACTUAL_BUILD_TYPE}")

set(MPFR_INSTALL_DIR "${MPFR_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${MPFR_ACTUAL_BUILD_TYPE}")
set(MPFR_SOURCE_DIR "${MPFR_CACHE_DIR}/src")
set(MPFR_ZIP_FILE "${MPFR_CACHE_DIR}/mpfr-${MPFR_VERSION}.tar.gz")

# 平台相关常量（同 GMP.cmake 思路）
if(MSVC OR WIN32)
    set(_MPFR_FROM_VENDORED TRUE)
    set(_MPFR_LIB_FILENAME "libmpfr-4.lib")
    set(_MPFR_INSTALLED_LIB "${MPFR_INSTALL_DIR}/lib/${_MPFR_LIB_FILENAME}")
else()
    set(_MPFR_FROM_VENDORED FALSE)
    set(_MPFR_LIB_FILENAME "libmpfr.a")
    set(_MPFR_INSTALLED_LIB "${MPFR_INSTALL_DIR}/lib/${_MPFR_LIB_FILENAME}")
    set(_MPFR_SRC_VERSION "4.2.1")
    # 跟 main 上一致使用 mpfr.org 源；存在则用，主下载分支会做哈希校验
    set(_MPFR_SRC_URL "https://www.mpfr.org/mpfr-${_MPFR_SRC_VERSION}/mpfr-${_MPFR_SRC_VERSION}.tar.bz2")
    set(_MPFR_SRC_SHA256 "b9df93635b20e4089c29623b19420c4ac848a1b29df1cfd59f26cab0d2666aa0")
    set(MPFR_ZIP_FILE "${MPFR_CACHE_DIR}/mpfr-${_MPFR_SRC_VERSION}.tar.bz2")
endif()

message(STATUS "")
message(STATUS "🔧 MPFR 库缓存管理系统")
message(STATUS "   MPFR版本: ${MPFR_VERSION}")
message(STATUS "   MPFR构建类型: ${MPFR_ACTUAL_BUILD_TYPE}")
if(DEFINED MPFR_BUILD_TYPE AND NOT MPFR_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: MPFR使用${MPFR_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   MPFR缓存目录: ${MPFR_CACHE_DIR}")
message(STATUS "   构建目录: ${MPFR_BUILD_DIR}")
message(STATUS "   安装目录: ${MPFR_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${MPFR_CACHE_DIR}")

# 检查MPFR库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# MPFR 安装后会有 include/mpfr.h + 平台相关库
if(EXISTS "${MPFR_INSTALL_DIR}/include/mpfr.h"
   AND EXISTS "${_MPFR_INSTALLED_LIB}")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${MPFR_INSTALL_DIR}")
endif()

if(_MPFR_FROM_VENDORED)
    # Windows: vendored 总是有"源码"（仓库里 deps/MPFR/mpfr/）
    if(EXISTS "${_MPFR_VENDORED_DIR}/include/mpfr.h")
        set(HAS_SOURCE TRUE)
    endif()
else()
    # macOS/Linux: 解压 tarball 后的源码目录
    if(EXISTS "${MPFR_SOURCE_DIR}/configure")
        set(HAS_SOURCE TRUE)
        message(STATUS "📄 发现源码目录: ${MPFR_SOURCE_DIR}")
    endif()
    if(EXISTS "${MPFR_ZIP_FILE}")
        file(SHA256 "${MPFR_ZIP_FILE}" _mpfr_zip_actual)
        string(TOLOWER "${_mpfr_zip_actual}" _mpfr_zip_actual)
        if(_mpfr_zip_actual STREQUAL "${_MPFR_SRC_SHA256}")
            set(HAS_ZIP TRUE)
            message(STATUS "📦 发现压缩包: ${MPFR_ZIP_FILE}")
        else()
            message(STATUS "⚠️  MPFR 压缩包校验失败，删除重新下载: ${MPFR_ZIP_FILE}")
            file(REMOVE "${MPFR_ZIP_FILE}")
        endif()
    endif()
    if(EXISTS "${MPFR_BUILD_DIR}/Makefile")
        set(HAS_BUILD TRUE)
        message(STATUS "📁 发现构建目录: ${MPFR_BUILD_DIR}")
    endif()
endif()

# 兼容: 上面 vendored 分支的提示信息
if(_MPFR_FROM_VENDORED AND HAS_SOURCE)
    message(STATUS "📄 发现 vendored 源码: ${_MPFR_VENDORED_DIR}")
endif()

# 决定操作策略
set(MPFR_STATUS "NONE")
set(MPFR_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(MPFR_STATUS "INSTALLED")
    set(MPFR_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的MPFR库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(MPFR_STATUS "BUILT_COMPLETE")
        set(MPFR_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的MPFR（无需安装）")
    else()
        # 构建未完成，需要继续
        set(MPFR_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(MPFR_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(MPFR_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(MPFR_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载MPFR")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${MPFR_STATUS}")

# 根据状态执行相应操作
if(MPFR_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的MPFR
    message(STATUS "🚀 使用缓存的 MPFR 库，跳过下载和编译")
    # MPFR: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(MPFR_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 MPFR...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${MPFR_BUILD_DIR} --target install --config ${MPFR_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${MPFR_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ MPFR 安装成功")
        # MPFR: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(MPFR_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ MPFR 安装失败")
    endif()

elseif(MPFR_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 MPFR...")

    # 先检查构建是否完整
    if(EXISTS "${MPFR_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${MPFR_BUILD_DIR} --target install --config ${MPFR_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${MPFR_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ MPFR 安装成功")
            set(MPFR_DIR "${MPFR_INSTALL_DIR}/lib/cmake/vtk-${MPFR_VERSION_MAJOR}" CACHE PATH "MPFR directory")
            find_package(MPFR ${MPFR_VERSION} REQUIRED PATHS ${MPFR_DIR})
            set(MPFR_FOUND TRUE)
        else()
            message(WARNING "❌ MPFR 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${MPFR_BUILD_DIR}")
            set(MPFR_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${MPFR_BUILD_DIR}")
        set(MPFR_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(MPFR_STATUS STREQUAL "SOURCE_ONLY")
    if(_MPFR_FROM_VENDORED)
        # Windows: vendored 复制
        message(STATUS "🔨 开始安装 MPFR (vendored)...")

        if(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32" OR CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(_mpfr_arch "win-x86")
        else()
            set(_mpfr_arch "win-x64")
        endif()

        file(MAKE_DIRECTORY "${MPFR_INSTALL_DIR}/include")
        file(MAKE_DIRECTORY "${MPFR_INSTALL_DIR}/lib")
        file(MAKE_DIRECTORY "${MPFR_INSTALL_DIR}/bin")
        file(COPY "${_MPFR_VENDORED_DIR}/include/mpfr.h"
             DESTINATION "${MPFR_INSTALL_DIR}/include/")
        file(COPY "${_MPFR_VENDORED_DIR}/include/mpf2mpfr.h"
             DESTINATION "${MPFR_INSTALL_DIR}/include/")
        file(COPY "${_MPFR_VENDORED_DIR}/lib/${_mpfr_arch}/libmpfr-4.lib"
             DESTINATION "${MPFR_INSTALL_DIR}/lib/")
        file(COPY "${_MPFR_VENDORED_DIR}/lib/${_mpfr_arch}/libmpfr-4.dll"
             DESTINATION "${MPFR_INSTALL_DIR}/bin/")

        if(EXISTS "${_MPFR_INSTALLED_LIB}")
            message(STATUS "✅ MPFR 已就位")
            set(MPFR_FOUND TRUE)
        else()
            message(FATAL_ERROR "MPFR vendored 复制失败")
        endif()
    else()
        # macOS/Linux: 跑 ./configure --with-gmp && make && make install
        message(STATUS "🔨 开始构建 MPFR (源码)...")

        # 必须依赖 GMP (上层 fetch_deps.cmake 已先包含 GMP.cmake，GMP_INSTALL_DIR 就绪)
        if(NOT GMP_FOUND OR NOT EXISTS "${GMP_INSTALL_DIR}/include/gmp.h")
            message(FATAL_ERROR "❌ MPFR 编译依赖 GMP，但未找到 GMP_INSTALL_DIR/include/gmp.h")
        endif()

        set(_mpfr_ccflags "-O2 -DNDEBUG -fPIC -DPIC -fomit-frame-pointer -fno-common")
        if(APPLE)
            if(CMAKE_OSX_DEPLOYMENT_TARGET)
                set(_mpfr_ccflags "${_mpfr_ccflags} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            endif()
            if(CMAKE_OSX_ARCHITECTURES MATCHES "arm")
                set(_mpfr_build_tgt "--build=aarch64-apple-darwin")
                set(_mpfr_ccflags "${_mpfr_ccflags} -arch arm64")
            elseif(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
                set(_mpfr_build_tgt "--build=x86_64-apple-darwin")
                set(_mpfr_ccflags "${_mpfr_ccflags} -arch x86_64")
            else()
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
                    set(_mpfr_build_tgt "--build=aarch64-apple-darwin")
                else()
                    set(_mpfr_build_tgt "--build=${CMAKE_SYSTEM_PROCESSOR}-apple-darwin")
                endif()
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(_mpfr_build_tgt "--build=${CMAKE_SYSTEM_PROCESSOR}-pc-linux-gnu")
        else()
            set(_mpfr_build_tgt "")
        endif()

        if(NOT EXISTS "${MPFR_BUILD_DIR}")
            file(MAKE_DIRECTORY "${MPFR_BUILD_DIR}")
        endif()

        message(STATUS "   configure: ${MPFR_SOURCE_DIR}/configure --with-gmp=${GMP_INSTALL_DIR} ${_mpfr_build_tgt}")
        execute_process(
            COMMAND env "CFLAGS=${_mpfr_ccflags}" "CXXFLAGS=${_mpfr_ccflags}"
                "${MPFR_SOURCE_DIR}/configure"
                ${_mpfr_build_tgt}
                "--prefix=${MPFR_INSTALL_DIR}"
                --enable-shared=no
                --enable-static=yes
                "--with-gmp=${GMP_INSTALL_DIR}"
            WORKING_DIRECTORY "${MPFR_BUILD_DIR}"
            RESULT_VARIABLE _mpfr_cfg_result
        )
        if(NOT _mpfr_cfg_result EQUAL 0)
            message(FATAL_ERROR "❌ MPFR configure 失败 (exit ${_mpfr_cfg_result})")
        endif()

        include(ProcessorCount)
        ProcessorCount(_mpfr_n)
        if(_mpfr_n EQUAL 0)
            set(_mpfr_n 4)
        endif()
        message(STATUS "🔨 编译 MPFR (make -j${_mpfr_n})...")
        execute_process(
            COMMAND make -j${_mpfr_n}
            WORKING_DIRECTORY "${MPFR_BUILD_DIR}"
            RESULT_VARIABLE _mpfr_make_result
        )
        if(NOT _mpfr_make_result EQUAL 0)
            message(FATAL_ERROR "❌ MPFR make 失败 (exit ${_mpfr_make_result})")
        endif()

        message(STATUS "📦 安装 MPFR...")
        execute_process(
            COMMAND make install
            WORKING_DIRECTORY "${MPFR_BUILD_DIR}"
            RESULT_VARIABLE _mpfr_install_result
        )
        if(NOT _mpfr_install_result EQUAL 0)
            message(FATAL_ERROR "❌ MPFR make install 失败 (exit ${_mpfr_install_result})")
        endif()

        if(EXISTS "${_MPFR_INSTALLED_LIB}")
            message(STATUS "✅ MPFR 编译安装成功 -> ${_MPFR_INSTALLED_LIB}")
            set(MPFR_FOUND TRUE)
        else()
            message(FATAL_ERROR "❌ MPFR install 后未找到库文件: ${_MPFR_INSTALLED_LIB}")
        endif()
    endif()


elseif(MPFR_STATUS STREQUAL "ZIP_ONLY")
    message(STATUS "📦 解压 MPFR 源码 tarball...")

    if(EXISTS "${MPFR_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${MPFR_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xjf ${MPFR_ZIP_FILE}
        WORKING_DIRECTORY ${MPFR_CACHE_DIR}
        RESULT_VARIABLE extract_result
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # mpfr tarball 顶层目录是 mpfr-<version>
        file(GLOB _mpfr_extracted_dirs "${MPFR_CACHE_DIR}/mpfr-*")
        set(_mpfr_extracted "")
        foreach(_d IN LISTS _mpfr_extracted_dirs)
            if(IS_DIRECTORY "${_d}")
                set(_mpfr_extracted "${_d}")
                break()
            endif()
        endforeach()
        if(_mpfr_extracted)
            file(RENAME "${_mpfr_extracted}" "${MPFR_SOURCE_DIR}")
            message(STATUS "✅ MPFR 解压成功")
            set(HAS_SOURCE TRUE)
            set(MPFR_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到 mpfr-* 目录")
        endif()
    else()
        message(WARNING "❌ MPFR 解压失败: ${extract_error}")
        file(REMOVE "${MPFR_ZIP_FILE}")
        set(MPFR_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    if(_MPFR_FROM_VENDORED)
        message(FATAL_ERROR "❌ Windows 平台未在仓库找到 vendored MPFR: ${_MPFR_VENDORED_DIR}")
    endif()

    message(STATUS "⬇️  下载 MPFR ${_MPFR_SRC_VERSION} 源码...")
    message(STATUS "   URL: ${_MPFR_SRC_URL}")

    file(DOWNLOAD
        "${_MPFR_SRC_URL}"
        "${MPFR_ZIP_FILE}"
        EXPECTED_HASH SHA256=${_MPFR_SRC_SHA256}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ MPFR 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ MPFR 下载成功")
    set(MPFR_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
endif()

# 注册到主项目的 find_package(MPFR)
if(MPFR_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${MPFR_INSTALL_DIR}")
    set(MPFR_ROOT "${MPFR_INSTALL_DIR}" CACHE PATH "MPFR root" FORCE)
    set(MPFR_INCLUDE_DIR "${MPFR_INSTALL_DIR}/include" CACHE PATH "MPFR include" FORCE)
    set(MPFR_LIBRARY "${_MPFR_INSTALLED_LIB}" CACHE FILEPATH "MPFR library" FORCE)
    set(MPFR_LIBRARIES "${MPFR_LIBRARY}" CACHE STRING "MPFR libraries" FORCE)
    set(MPFR_AVAILABLE TRUE CACHE BOOL "MPFR library is available")
    message(STATUS "")
    message(STATUS "🎯 MPFR 库已就绪")
    message(STATUS "   安装目录: ${MPFR_INSTALL_DIR}")
else()
    set(MPFR_AVAILABLE FALSE CACHE BOOL "MPFR library is not available")
    message(FATAL_ERROR "⚠️  MPFR 库不可用")
endif()