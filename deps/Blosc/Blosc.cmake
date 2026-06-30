cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# Blosc版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED BLOSC_VERSION)
    set(BLOSC_VERSION "1.17.0" CACHE STRING "Blosc version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" BLOSC_VERSION_MAJOR "${BLOSC_VERSION}")

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
set(BLOSC_CACHE_DIR "${FETCH_CACHE_DIR}/blosc-v${BLOSC_VERSION}")

# Blosc构建类型配置 - 可以独立于主项目设置
if(DEFINED BLOSC_BUILD_TYPE)
    # 如果明确指定了BLOSC_BUILD_TYPE，使用它
    set(BLOSC_ACTUAL_BUILD_TYPE "${BLOSC_BUILD_TYPE}")
    message(STATUS "   使用指定的Blosc构建类型: ${BLOSC_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(BLOSC_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 Blosc")
    else()
        set(BLOSC_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(BLOSC_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(BLOSC_BUILD_DIR "${BLOSC_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${BLOSC_ACTUAL_BUILD_TYPE}")

set(BLOSC_INSTALL_DIR "${BLOSC_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${BLOSC_ACTUAL_BUILD_TYPE}")
set(BLOSC_SOURCE_DIR "${BLOSC_CACHE_DIR}/src")
set(BLOSC_ZIP_FILE "${BLOSC_CACHE_DIR}/c-blosc-${BLOSC_VERSION}_tm.zip")

message(STATUS "")
message(STATUS "🔧 Blosc 库缓存管理系统")
message(STATUS "   Blosc版本: ${BLOSC_VERSION}")
message(STATUS "   Blosc构建类型: ${BLOSC_ACTUAL_BUILD_TYPE}")
if(DEFINED BLOSC_BUILD_TYPE AND NOT BLOSC_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: Blosc使用${BLOSC_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   Blosc缓存目录: ${BLOSC_CACHE_DIR}")
message(STATUS "   构建目录: ${BLOSC_BUILD_DIR}")
message(STATUS "   安装目录: ${BLOSC_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${BLOSC_CACHE_DIR}")

# 检查Blosc库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# Blosc 安装后会有 include/blosc.h
if(EXISTS "${BLOSC_INSTALL_DIR}/include/blosc.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${BLOSC_INSTALL_DIR}")
endif()

# CMake 项目：CMakeCache 即认为已配置，lib/ 下有产物即认为构建完成
if(EXISTS "${BLOSC_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _blosc_built_libs
        "${BLOSC_BUILD_DIR}/blosc/${BLOSC_ACTUAL_BUILD_TYPE}/blosc*"
        "${BLOSC_BUILD_DIR}/blosc/libblosc*"
        "${BLOSC_BUILD_DIR}/blosc/blosc*.lib")
    if(_blosc_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${BLOSC_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${BLOSC_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${BLOSC_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${BLOSC_SOURCE_DIR}")
endif()

if(EXISTS "${BLOSC_ZIP_FILE}")
    set(_expected "dcb48bf43a672fa3de6a4b1de2c4c238709dad5893d1e097b8374ad84b1fc3b3")
    file(SHA256 "${BLOSC_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${BLOSC_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${BLOSC_ZIP_FILE}")
        file(REMOVE "${BLOSC_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(BLOSC_STATUS "NONE")
set(BLOSC_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(BLOSC_STATUS "INSTALLED")
    set(BLOSC_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的Blosc库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(BLOSC_STATUS "BUILT_COMPLETE")
        set(BLOSC_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的Blosc（无需安装）")
    else()
        # 构建未完成，需要继续
        set(BLOSC_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(BLOSC_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(BLOSC_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(BLOSC_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载Blosc")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${BLOSC_STATUS}")

# 根据状态执行相应操作
if(BLOSC_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的Blosc
    message(STATUS "🚀 使用缓存的 Blosc 库，跳过下载和编译")
    # Blosc: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(BLOSC_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 Blosc...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${BLOSC_BUILD_DIR} --target install --config ${BLOSC_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${BLOSC_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ Blosc 安装成功")
        # Blosc: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(BLOSC_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ Blosc 安装失败")
    endif()

elseif(BLOSC_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 Blosc...")

    # 先检查构建是否完整
    if(EXISTS "${BLOSC_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${BLOSC_BUILD_DIR} --target install --config ${BLOSC_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${BLOSC_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ Blosc 安装成功")
            # Blosc: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(BLOSC_FOUND TRUE)
        else()
            message(WARNING "❌ Blosc 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${BLOSC_BUILD_DIR}")
            set(BLOSC_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${BLOSC_BUILD_DIR}")
        set(BLOSC_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(BLOSC_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 Blosc...")

    # 检测可用的生成器
    set(BLOSC_GENERATOR "")
    set(BLOSC_GENERATOR_PLATFORM "")
    set(BLOSC_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(BLOSC_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(BLOSC_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${BLOSC_GENERATOR}")
        if(BLOSC_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${BLOSC_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(BLOSC_GENERATOR "Ninja")
            set(BLOSC_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(BLOSC_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(BLOSC_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(BLOSC_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${BLOSC_GENERATOR}")
    if(BLOSC_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${BLOSC_GENERATOR_PLATFORM})
    endif()
    if(BLOSC_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${BLOSC_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(BLOSC_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # Blosc shared/static 由主项目 BUILD_SHARED_LIBS 决定
    if(BUILD_SHARED_LIBS)
        set(_blosc_shared ON)
        set(_blosc_static OFF)
    else()
        set(_blosc_shared OFF)
        set(_blosc_static ON)
    endif()

    # macOS 跨编译时关闭 SSE2/AVX2（保留原项目策略）
    set(_blosc_extra_args "")
    if(IS_CROSS_COMPILE AND APPLE)
        list(APPEND _blosc_extra_args
            -DDEACTIVATE_SSE2=ON
            -DDEACTIVATE_AVX2=ON)
    endif()

    # 配置 Blosc
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${BLOSC_SOURCE_DIR}
            -B ${BLOSC_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${BLOSC_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${BLOSC_INSTALL_DIR}
            -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
            -DBUILD_SHARED=${_blosc_shared}
            -DBUILD_STATIC=${_blosc_static}
            -DBUILD_TESTS=OFF
            -DBUILD_BENCHMARKS=OFF
            -DPREFER_EXTERNAL_ZLIB=ON
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            ${_blosc_extra_args}
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ Blosc 配置成功")

        # 构建Blosc
        message(STATUS "🔨 正在编译 Blosc (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${BLOSC_BUILD_DIR} --config ${BLOSC_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${BLOSC_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${BLOSC_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${BLOSC_BUILD_DIR} --config ${BLOSC_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${BLOSC_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ Blosc 编译成功")

            # 安装Blosc
            message(STATUS "📦 正在安装 Blosc...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${BLOSC_BUILD_DIR} --target install --config ${BLOSC_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${BLOSC_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ Blosc 安装成功")
                # Blosc: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(BLOSC_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ Blosc 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ Blosc 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${BLOSC_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "Blosc 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ Blosc 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "Blosc 配置失败，请查看上面的错误信息")
    endif()

elseif(BLOSC_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 Blosc 源码...")

    # 确保源码目录不存在
    if(EXISTS "${BLOSC_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${BLOSC_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${BLOSC_ZIP_FILE}
        WORKING_DIRECTORY ${BLOSC_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # Blosc 解压后目录名形如 c-blosc-1.17.0_tm
        file(GLOB BLOSC_EXTRACTED_DIRS "${BLOSC_CACHE_DIR}/c-blosc-*")
        list(FILTER BLOSC_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(GET BLOSC_EXTRACTED_DIRS 0 BLOSC_EXTRACTED_DIR)

        if(EXISTS "${BLOSC_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${BLOSC_EXTRACTED_DIR}" "${BLOSC_SOURCE_DIR}")
            message(STATUS "✅ Blosc 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(BLOSC_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到Blosc目录")
        endif()
    else()
        message(WARNING "❌ Blosc 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${BLOSC_ZIP_FILE}")
        set(BLOSC_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 Blosc ${BLOSC_VERSION}...")

    # Blosc fork (tamasmeszaros/c-blosc), branch zip
    set(BLOSC_DOWNLOAD_URL "https://github.com/tamasmeszaros/c-blosc/archive/refs/heads/v${BLOSC_VERSION}_tm.zip")

    message(STATUS "📥 正在下载 Blosc...")
    message(STATUS "   URL: ${BLOSC_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${BLOSC_DOWNLOAD_URL}
        ${BLOSC_ZIP_FILE}
        EXPECTED_HASH SHA256=dcb48bf43a672fa3de6a4b1de2c4c238709dad5893d1e097b8374ad84b1fc3b3
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ Blosc 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ Blosc 下载成功")

    # 解压
    message(STATUS "📦 解压 Blosc 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${BLOSC_ZIP_FILE}
        WORKING_DIRECTORY ${BLOSC_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ Blosc 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(BLOSC_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ Blosc 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(Blosc)
if(BLOSC_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${BLOSC_INSTALL_DIR}")
    set(Blosc_ROOT "${BLOSC_INSTALL_DIR}" CACHE PATH "Blosc root" FORCE)
    set(Blosc_AVAILABLE TRUE CACHE BOOL "Blosc library is available")
    message(STATUS "")
    message(STATUS "🎯 Blosc 库已就绪")
    message(STATUS "   版本: ${BLOSC_VERSION}")
    message(STATUS "   安装目录: ${BLOSC_INSTALL_DIR}")
else()
    set(Blosc_AVAILABLE FALSE CACHE BOOL "Blosc library is not available")
    message(FATAL_ERROR "⚠️  Blosc 库不可用")
endif()