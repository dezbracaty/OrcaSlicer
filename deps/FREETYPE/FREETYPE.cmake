cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# FREETYPE版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED FREETYPE_VERSION)
    set(FREETYPE_VERSION "2.12.1" CACHE STRING "FREETYPE version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" FREETYPE_VERSION_MAJOR "${FREETYPE_VERSION}")

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
set(FREETYPE_CACHE_DIR "${FETCH_CACHE_DIR}/freetype-v${FREETYPE_VERSION}")

# FREETYPE构建类型配置 - 可以独立于主项目设置
if(DEFINED FREETYPE_BUILD_TYPE)
    # 如果明确指定了FREETYPE_BUILD_TYPE，使用它
    set(FREETYPE_ACTUAL_BUILD_TYPE "${FREETYPE_BUILD_TYPE}")
    message(STATUS "   使用指定的FREETYPE构建类型: ${FREETYPE_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(FREETYPE_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 FREETYPE")
    else()
        set(FREETYPE_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(FREETYPE_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(FREETYPE_BUILD_DIR "${FREETYPE_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${FREETYPE_ACTUAL_BUILD_TYPE}")

set(FREETYPE_INSTALL_DIR "${FREETYPE_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${FREETYPE_ACTUAL_BUILD_TYPE}")
set(FREETYPE_SOURCE_DIR "${FREETYPE_CACHE_DIR}/src")
set(FREETYPE_ZIP_FILE "${FREETYPE_CACHE_DIR}/freetype-${FREETYPE_VERSION}.tar.gz")

message(STATUS "")
message(STATUS "🔧 FREETYPE 库缓存管理系统")
message(STATUS "   FREETYPE版本: ${FREETYPE_VERSION}")
message(STATUS "   FREETYPE构建类型: ${FREETYPE_ACTUAL_BUILD_TYPE}")
if(DEFINED FREETYPE_BUILD_TYPE AND NOT FREETYPE_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: FREETYPE使用${FREETYPE_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   FREETYPE缓存目录: ${FREETYPE_CACHE_DIR}")
message(STATUS "   构建目录: ${FREETYPE_BUILD_DIR}")
message(STATUS "   安装目录: ${FREETYPE_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${FREETYPE_CACHE_DIR}")

# 检查FREETYPE库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# FREETYPE 安装后会有 include/freetype2/ft2build.h
if(EXISTS "${FREETYPE_INSTALL_DIR}/include/freetype2/ft2build.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${FREETYPE_INSTALL_DIR}")
endif()

# CMake 项目：CMakeCache 即认为已配置，lib/ 下有产物即认为构建完成
if(EXISTS "${FREETYPE_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _ft_built_libs
        "${FREETYPE_BUILD_DIR}/${FREETYPE_ACTUAL_BUILD_TYPE}/freetype*"
        "${FREETYPE_BUILD_DIR}/libfreetype*"
        "${FREETYPE_BUILD_DIR}/freetype*.dll")
    if(_ft_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${FREETYPE_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${FREETYPE_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${FREETYPE_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${FREETYPE_SOURCE_DIR}")
endif()

if(EXISTS "${FREETYPE_ZIP_FILE}")
    set(_expected "efe71fd4b8246f1b0b1b9bfca13cfff1c9ad85930340c27df469733bbb620938")
    file(SHA256 "${FREETYPE_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${FREETYPE_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${FREETYPE_ZIP_FILE}")
        file(REMOVE "${FREETYPE_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(FREETYPE_STATUS "NONE")
set(FREETYPE_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(FREETYPE_STATUS "INSTALLED")
    set(FREETYPE_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的FREETYPE库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(FREETYPE_STATUS "BUILT_COMPLETE")
        set(FREETYPE_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的FREETYPE（无需安装）")
    else()
        # 构建未完成，需要继续
        set(FREETYPE_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(FREETYPE_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(FREETYPE_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(FREETYPE_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载FREETYPE")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${FREETYPE_STATUS}")

# 根据状态执行相应操作
if(FREETYPE_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的FREETYPE
    message(STATUS "🚀 使用缓存的 FREETYPE 库，跳过下载和编译")
    # FREETYPE: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(FREETYPE_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 FREETYPE...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${FREETYPE_BUILD_DIR} --target install --config ${FREETYPE_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${FREETYPE_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ FREETYPE 安装成功")
        # FREETYPE: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(FREETYPE_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ FREETYPE 安装失败")
    endif()

elseif(FREETYPE_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 FREETYPE...")

    # 先检查构建是否完整
    if(EXISTS "${FREETYPE_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${FREETYPE_BUILD_DIR} --target install --config ${FREETYPE_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${FREETYPE_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ FREETYPE 安装成功")
            # FREETYPE: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(FREETYPE_FOUND TRUE)
        else()
            message(WARNING "❌ FREETYPE 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${FREETYPE_BUILD_DIR}")
            set(FREETYPE_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${FREETYPE_BUILD_DIR}")
        set(FREETYPE_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(FREETYPE_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 FREETYPE...")

    # 检测可用的生成器
    set(FREETYPE_GENERATOR "")
    set(FREETYPE_GENERATOR_PLATFORM "")
    set(FREETYPE_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(FREETYPE_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(FREETYPE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${FREETYPE_GENERATOR}")
        if(FREETYPE_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${FREETYPE_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(FREETYPE_GENERATOR "Ninja")
            set(FREETYPE_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(FREETYPE_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(FREETYPE_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(FREETYPE_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${FREETYPE_GENERATOR}")
    if(FREETYPE_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${FREETYPE_GENERATOR_PLATFORM})
    endif()
    if(FREETYPE_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${FREETYPE_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(FREETYPE_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # FREETYPE 在 Windows 上构建为 shared，其他平台 static (沿用原项目设置)
    if(WIN32)
        set(_ft_shared ON)
    else()
        set(_ft_shared OFF)
    endif()

    # zlib 仅在 Linux 上启用 (原项目策略)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_ft_disable_zlib FALSE)
    else()
        set(_ft_disable_zlib TRUE)
    endif()

    # 配置 FREETYPE
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${FREETYPE_SOURCE_DIR}
            -B ${FREETYPE_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${FREETYPE_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${FREETYPE_INSTALL_DIR}
            -DBUILD_SHARED_LIBS=${_ft_shared}
            -DFT_DISABLE_ZLIB=${_ft_disable_zlib}
            -DFT_DISABLE_BZIP2=TRUE
            -DFT_DISABLE_PNG=TRUE
            -DFT_DISABLE_HARFBUZZ=TRUE
            -DFT_DISABLE_BROTLI=TRUE
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ FREETYPE 配置成功")

        # 构建FREETYPE
        message(STATUS "🔨 正在编译 FREETYPE (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${FREETYPE_BUILD_DIR} --config ${FREETYPE_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${FREETYPE_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${FREETYPE_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${FREETYPE_BUILD_DIR} --config ${FREETYPE_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${FREETYPE_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ FREETYPE 编译成功")

            # 安装FREETYPE
            message(STATUS "📦 正在安装 FREETYPE...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${FREETYPE_BUILD_DIR} --target install --config ${FREETYPE_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${FREETYPE_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ FREETYPE 安装成功")
                # FREETYPE: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(FREETYPE_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ FREETYPE 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ FREETYPE 编译失败，返回码: ${build_result}")

            # 检查是否有输出
            if(build_output)
                message(STATUS "编译输出（最后1000字符）:")
                string(LENGTH "${build_output}" output_len)
                if(output_len GREATER 1000)
                    string(SUBSTRING "${build_output}" ${output_len}-1000 -1 build_output_tail)
                    message(STATUS "${build_output_tail}")
                else()
                    message(STATUS "${build_output}")
                endif()
            endif()

            if(build_error)
                message(STATUS "错误信息:")
                message(STATUS "${build_error}")
            endif()

            # 尝试查看最后的错误日志
            file(GLOB error_logs "${FREETYPE_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "FREETYPE 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ FREETYPE 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "FREETYPE 配置失败，请查看上面的错误信息")
    endif()

elseif(FREETYPE_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 FREETYPE 源码...")

    # 确保源码目录不存在
    if(EXISTS "${FREETYPE_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${FREETYPE_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${FREETYPE_ZIP_FILE}
        WORKING_DIRECTORY ${FREETYPE_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # FREETYPE 解压后目录名为 freetype-2.12.1
        file(GLOB FREETYPE_EXTRACTED_DIRS "${FREETYPE_CACHE_DIR}/freetype-*")
        list(FILTER FREETYPE_EXTRACTED_DIRS EXCLUDE REGEX "\\.tar\\.gz$")
        list(GET FREETYPE_EXTRACTED_DIRS 0 FREETYPE_EXTRACTED_DIR)

        if(EXISTS "${FREETYPE_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${FREETYPE_EXTRACTED_DIR}" "${FREETYPE_SOURCE_DIR}")
            message(STATUS "✅ FREETYPE 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(FREETYPE_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到FREETYPE目录")
        endif()
    else()
        message(WARNING "❌ FREETYPE 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${FREETYPE_ZIP_FILE}")
        set(FREETYPE_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 FREETYPE ${FREETYPE_VERSION}...")

    # FREETYPE 镜像（SoftFever 自托管）
    set(FREETYPE_DOWNLOAD_URL "https://github.com/SoftFever/orca_deps/releases/download/freetype-${FREETYPE_VERSION}.tar.gz/freetype-${FREETYPE_VERSION}.tar.gz")

    message(STATUS "📥 正在下载 FREETYPE...")
    message(STATUS "   URL: ${FREETYPE_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${FREETYPE_DOWNLOAD_URL}
        ${FREETYPE_ZIP_FILE}
        EXPECTED_HASH SHA256=efe71fd4b8246f1b0b1b9bfca13cfff1c9ad85930340c27df469733bbb620938
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ FREETYPE 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ FREETYPE 下载成功")

    # 解压
    message(STATUS "📦 解压 FREETYPE 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${FREETYPE_ZIP_FILE}
        WORKING_DIRECTORY ${FREETYPE_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ FREETYPE 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(FREETYPE_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ FREETYPE 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(Freetype)
if(FREETYPE_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${FREETYPE_INSTALL_DIR}")
    set(FREETYPE_DIR "${FREETYPE_INSTALL_DIR}" CACHE PATH "FREETYPE root" FORCE)
    list(APPEND ORCA_DEPS_INSTALL_PATHS "${FREETYPE_INSTALL_DIR}")
    set(ORCA_DEPS_INSTALL_PATHS "${ORCA_DEPS_INSTALL_PATHS}" CACHE INTERNAL "Per-dep install roots for runtime DLL lookup" FORCE)
    set(FREETYPE_AVAILABLE TRUE CACHE BOOL "FREETYPE library is available")
    message(STATUS "")
    message(STATUS "🎯 FREETYPE 库已就绪")
    message(STATUS "   版本: ${FREETYPE_VERSION}")
    message(STATUS "   安装目录: ${FREETYPE_INSTALL_DIR}")
else()
    set(FREETYPE_AVAILABLE FALSE CACHE BOOL "FREETYPE library is not available")
    message(FATAL_ERROR "⚠️  FREETYPE 库不可用")
endif()