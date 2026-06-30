cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# GMP版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED GMP_VERSION)
    set(GMP_VERSION "vendored" CACHE STRING "GMP version label")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" GMP_VERSION_MAJOR "${GMP_VERSION}")

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
set(GMP_CACHE_DIR "${FETCH_CACHE_DIR}/gmp-${GMP_VERSION}")
# GMP 是 vendored 预编译二进制，"源码"就是仓库里的 deps/GMP/gmp/
set(_GMP_VENDORED_DIR "${CMAKE_CURRENT_LIST_DIR}/gmp")

# GMP构建类型配置 - 可以独立于主项目设置
if(DEFINED GMP_BUILD_TYPE)
    # 如果明确指定了GMP_BUILD_TYPE，使用它
    set(GMP_ACTUAL_BUILD_TYPE "${GMP_BUILD_TYPE}")
    message(STATUS "   使用指定的GMP构建类型: ${GMP_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(GMP_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 GMP")
    else()
        set(GMP_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(GMP_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(GMP_BUILD_DIR "${GMP_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${GMP_ACTUAL_BUILD_TYPE}")

set(GMP_INSTALL_DIR "${GMP_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${GMP_ACTUAL_BUILD_TYPE}")
set(GMP_SOURCE_DIR "${GMP_CACHE_DIR}/src")
set(GMP_ZIP_FILE "${GMP_CACHE_DIR}/gmp-${GMP_VERSION}.tar.gz")

# 平台相关常量
# - Windows (MSVC): 用 vendored 预编译 .lib/.dll
# - macOS/Linux: 从 SoftFever/OrcaSlicer_deps releases 下源码 tarball 自行编译
if(MSVC OR WIN32)
    set(_GMP_FROM_VENDORED TRUE)
    set(_GMP_LIB_FILENAME "libgmp-10.lib")
    set(_GMP_INSTALLED_LIB "${GMP_INSTALL_DIR}/lib/${_GMP_LIB_FILENAME}")
else()
    set(_GMP_FROM_VENDORED FALSE)
    set(_GMP_LIB_FILENAME "libgmp.a")
    set(_GMP_INSTALLED_LIB "${GMP_INSTALL_DIR}/lib/${_GMP_LIB_FILENAME}")
    # macOS/Linux 源码 tarball
    set(_GMP_SRC_VERSION "6.2.1")
    set(_GMP_SRC_URL "https://github.com/SoftFever/OrcaSlicer_deps/releases/download/gmp-${_GMP_SRC_VERSION}/gmp-${_GMP_SRC_VERSION}.tar.bz2")
    set(_GMP_SRC_SHA256 "eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c")
    set(GMP_ZIP_FILE "${GMP_CACHE_DIR}/gmp-${_GMP_SRC_VERSION}.tar.bz2")
endif()

message(STATUS "")
message(STATUS "🔧 GMP 库缓存管理系统")
message(STATUS "   GMP版本: ${GMP_VERSION}")
message(STATUS "   GMP构建类型: ${GMP_ACTUAL_BUILD_TYPE}")
if(DEFINED GMP_BUILD_TYPE AND NOT GMP_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: GMP使用${GMP_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   GMP缓存目录: ${GMP_CACHE_DIR}")
message(STATUS "   构建目录: ${GMP_BUILD_DIR}")
message(STATUS "   安装目录: ${GMP_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${GMP_CACHE_DIR}")

# 检查GMP库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# GMP 安装后会有 include/gmp.h + 平台相关库文件
if(EXISTS "${GMP_INSTALL_DIR}/include/gmp.h"
   AND EXISTS "${_GMP_INSTALLED_LIB}")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${GMP_INSTALL_DIR}")
endif()

if(_GMP_FROM_VENDORED)
    # Windows: vendored 总是有"源码"（仓库里 deps/GMP/gmp/）
    if(EXISTS "${_GMP_VENDORED_DIR}/include/gmp.h")
        set(HAS_SOURCE TRUE)
        message(STATUS "📄 发现 vendored 源码: ${_GMP_VENDORED_DIR}")
    endif()
else()
    # macOS/Linux: 从 tarball 解压的源码
    if(EXISTS "${GMP_SOURCE_DIR}/configure")
        set(HAS_SOURCE TRUE)
        message(STATUS "📄 发现源码目录: ${GMP_SOURCE_DIR}")
    endif()
    # tarball 校验
    if(EXISTS "${GMP_ZIP_FILE}")
        file(SHA256 "${GMP_ZIP_FILE}" _gmp_zip_actual)
        string(TOLOWER "${_gmp_zip_actual}" _gmp_zip_actual)
        if(_gmp_zip_actual STREQUAL "${_GMP_SRC_SHA256}")
            set(HAS_ZIP TRUE)
            message(STATUS "📦 发现压缩包: ${GMP_ZIP_FILE}")
        else()
            message(STATUS "⚠️  GMP 压缩包校验失败，删除重新下载: ${GMP_ZIP_FILE}")
            file(REMOVE "${GMP_ZIP_FILE}")
        endif()
    endif()
    # 构建残留
    if(EXISTS "${GMP_BUILD_DIR}/Makefile")
        set(HAS_BUILD TRUE)
        message(STATUS "📁 发现构建目录: ${GMP_BUILD_DIR}")
    endif()
endif()

# 决定操作策略
set(GMP_STATUS "NONE")
set(GMP_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(GMP_STATUS "INSTALLED")
    set(GMP_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的GMP库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(GMP_STATUS "BUILT_COMPLETE")
        set(GMP_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的GMP（无需安装）")
    else()
        # 构建未完成，需要继续
        set(GMP_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(GMP_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(GMP_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(GMP_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载GMP")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${GMP_STATUS}")

# 根据状态执行相应操作
if(GMP_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的GMP
    message(STATUS "🚀 使用缓存的 GMP 库，跳过下载和编译")
    # GMP: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(GMP_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 GMP...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${GMP_BUILD_DIR} --target install --config ${GMP_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${GMP_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ GMP 安装成功")
        # GMP: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(GMP_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ GMP 安装失败")
    endif()

elseif(GMP_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 GMP...")

    # 先检查构建是否完整
    if(EXISTS "${GMP_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${GMP_BUILD_DIR} --target install --config ${GMP_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${GMP_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ GMP 安装成功")
            set(GMP_DIR "${GMP_INSTALL_DIR}/lib/cmake/vtk-${GMP_VERSION_MAJOR}" CACHE PATH "GMP directory")
            find_package(GMP ${GMP_VERSION} REQUIRED PATHS ${GMP_DIR})
            set(GMP_FOUND TRUE)
        else()
            message(WARNING "❌ GMP 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${GMP_BUILD_DIR}")
            set(GMP_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${GMP_BUILD_DIR}")
        set(GMP_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(GMP_STATUS STREQUAL "SOURCE_ONLY")
    if(_GMP_FROM_VENDORED)
        # Windows: 把预编译二进制从仓库复制到 install 目录
        message(STATUS "🔨 开始安装 GMP (vendored)...")

        # 选 arch
        if(CMAKE_GENERATOR_PLATFORM STREQUAL "Win32" OR CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(_gmp_arch "win-x86")
        else()
            set(_gmp_arch "win-x64")
        endif()

        file(MAKE_DIRECTORY "${GMP_INSTALL_DIR}/include")
        file(MAKE_DIRECTORY "${GMP_INSTALL_DIR}/lib")
        file(MAKE_DIRECTORY "${GMP_INSTALL_DIR}/bin")
        file(COPY "${_GMP_VENDORED_DIR}/include/gmp.h"
             DESTINATION "${GMP_INSTALL_DIR}/include/")
        file(COPY "${_GMP_VENDORED_DIR}/lib/${_gmp_arch}/libgmp-10.lib"
             DESTINATION "${GMP_INSTALL_DIR}/lib/")
        file(COPY "${_GMP_VENDORED_DIR}/lib/${_gmp_arch}/libgmp-10.dll"
             DESTINATION "${GMP_INSTALL_DIR}/bin/")

        if(EXISTS "${_GMP_INSTALLED_LIB}")
            message(STATUS "✅ GMP 已就位")
            set(GMP_FOUND TRUE)
        else()
            message(FATAL_ERROR "GMP vendored 复制失败")
        endif()
    else()
        # macOS/Linux: 跑 ./configure && make && make install
        message(STATUS "🔨 开始构建 GMP (源码)...")

        # CFLAGS / 平台 build target
        set(_gmp_ccflags "-O2 -DNDEBUG -fPIC -DPIC -fomit-frame-pointer -fno-common")
        if(APPLE)
            if(CMAKE_OSX_DEPLOYMENT_TARGET)
                set(_gmp_ccflags "${_gmp_ccflags} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            endif()
            if(CMAKE_OSX_ARCHITECTURES MATCHES "arm")
                set(_gmp_build_tgt "--build=aarch64-apple-darwin")
                set(_gmp_ccflags "${_gmp_ccflags} -arch arm64")
            elseif(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
                set(_gmp_build_tgt "--build=x86_64-apple-darwin")
                set(_gmp_ccflags "${_gmp_ccflags} -arch x86_64")
            else()
                if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
                    set(_gmp_build_tgt "--build=aarch64-apple-darwin")
                else()
                    set(_gmp_build_tgt "--build=${CMAKE_SYSTEM_PROCESSOR}-apple-darwin")
                endif()
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set(_gmp_build_tgt "--build=${CMAKE_SYSTEM_PROCESSOR}-pc-linux-gnu")
        else()
            set(_gmp_build_tgt "")
        endif()

        # 准备 build 目录（GMP 支持 out-of-source，但很多 autoconf 生成的 Makefile 仍偏好 in-source；
        # 这里采用 out-of-source 调用 ../src/configure 以保持与缓存目录隔离）
        if(NOT EXISTS "${GMP_BUILD_DIR}")
            file(MAKE_DIRECTORY "${GMP_BUILD_DIR}")
        endif()

        message(STATUS "   configure: ${GMP_SOURCE_DIR}/configure ${_gmp_build_tgt} --enable-static=yes --enable-cxx=yes --enable-shared=no")
        message(STATUS "   prefix: ${GMP_INSTALL_DIR}")
        message(STATUS "   CFLAGS: ${_gmp_ccflags}")

        execute_process(
            COMMAND env "CFLAGS=${_gmp_ccflags}" "CXXFLAGS=${_gmp_ccflags}"
                "${GMP_SOURCE_DIR}/configure"
                ${_gmp_build_tgt}
                --enable-shared=no
                --enable-static=yes
                --enable-cxx=yes
                "--prefix=${GMP_INSTALL_DIR}"
            WORKING_DIRECTORY "${GMP_BUILD_DIR}"
            RESULT_VARIABLE _gmp_cfg_result
        )
        if(NOT _gmp_cfg_result EQUAL 0)
            message(FATAL_ERROR "❌ GMP configure 失败 (exit ${_gmp_cfg_result})")
        endif()

        # 并行 make
        include(ProcessorCount)
        ProcessorCount(_gmp_n)
        if(_gmp_n EQUAL 0)
            set(_gmp_n 4)
        endif()
        message(STATUS "🔨 编译 GMP (make -j${_gmp_n})...")
        execute_process(
            COMMAND make -j${_gmp_n}
            WORKING_DIRECTORY "${GMP_BUILD_DIR}"
            RESULT_VARIABLE _gmp_make_result
        )
        if(NOT _gmp_make_result EQUAL 0)
            message(FATAL_ERROR "❌ GMP make 失败 (exit ${_gmp_make_result})")
        endif()

        message(STATUS "📦 安装 GMP...")
        execute_process(
            COMMAND make install
            WORKING_DIRECTORY "${GMP_BUILD_DIR}"
            RESULT_VARIABLE _gmp_install_result
        )
        if(NOT _gmp_install_result EQUAL 0)
            message(FATAL_ERROR "❌ GMP make install 失败 (exit ${_gmp_install_result})")
        endif()

        if(EXISTS "${_GMP_INSTALLED_LIB}")
            message(STATUS "✅ GMP 编译安装成功 -> ${_GMP_INSTALLED_LIB}")
            set(GMP_FOUND TRUE)
        else()
            message(FATAL_ERROR "❌ GMP install 后未找到库文件: ${_GMP_INSTALLED_LIB}")
        endif()
    endif()


elseif(GMP_STATUS STREQUAL "ZIP_ONLY")
    # 仅 macOS/Linux 路径会进到这里（Windows 上 vendored 总是被识别为 SOURCE）
    message(STATUS "📦 解压 GMP 源码 tarball...")

    if(EXISTS "${GMP_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${GMP_SOURCE_DIR}")
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xjf ${GMP_ZIP_FILE}
        WORKING_DIRECTORY ${GMP_CACHE_DIR}
        RESULT_VARIABLE extract_result
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # GMP tarball 顶层目录是 gmp-<version>
        file(GLOB _gmp_extracted_dirs "${GMP_CACHE_DIR}/gmp-*")
        set(_gmp_extracted "")
        foreach(_d IN LISTS _gmp_extracted_dirs)
            if(IS_DIRECTORY "${_d}")
                set(_gmp_extracted "${_d}")
                break()
            endif()
        endforeach()
        if(_gmp_extracted)
            file(RENAME "${_gmp_extracted}" "${GMP_SOURCE_DIR}")
            message(STATUS "✅ GMP 解压成功")
            set(HAS_SOURCE TRUE)
            set(GMP_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到 gmp-* 目录")
        endif()
    else()
        message(WARNING "❌ GMP 解压失败: ${extract_error}")
        file(REMOVE "${GMP_ZIP_FILE}")
        set(GMP_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # NONE 状态: macOS/Linux 上需下载源码 tarball; Windows 不会到这里(vendored 总是被识别为 SOURCE)
    if(_GMP_FROM_VENDORED)
        message(FATAL_ERROR "❌ Windows 平台未在仓库找到 vendored GMP: ${_GMP_VENDORED_DIR}")
    endif()

    message(STATUS "⬇️  下载 GMP ${_GMP_SRC_VERSION} 源码...")
    message(STATUS "   URL: ${_GMP_SRC_URL}")

    file(DOWNLOAD
        "${_GMP_SRC_URL}"
        "${GMP_ZIP_FILE}"
        EXPECTED_HASH SHA256=${_GMP_SRC_SHA256}
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ GMP 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ GMP 下载成功")
    set(GMP_STATUS "ZIP_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
endif()

# 注册到主项目的 find_package(GMP)
if(GMP_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${GMP_INSTALL_DIR}")
    set(GMP_ROOT "${GMP_INSTALL_DIR}" CACHE PATH "GMP root" FORCE)
    set(GMP_INCLUDE_DIR "${GMP_INSTALL_DIR}/include" CACHE PATH "GMP include" FORCE)
    set(GMP_LIBRARY "${_GMP_INSTALLED_LIB}" CACHE FILEPATH "GMP library" FORCE)
    set(GMP_LIBRARIES "${GMP_LIBRARY}" CACHE STRING "GMP libraries" FORCE)
    set(GMP_AVAILABLE TRUE CACHE BOOL "GMP library is available")
    message(STATUS "")
    message(STATUS "🎯 GMP 库已就绪")
    message(STATUS "   安装目录: ${GMP_INSTALL_DIR}")
else()
    set(GMP_AVAILABLE FALSE CACHE BOOL "GMP library is not available")
    message(FATAL_ERROR "⚠️  GMP 库不可用")
endif()