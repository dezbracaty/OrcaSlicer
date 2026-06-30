cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# Draco版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED DRACO_VERSION)
    set(DRACO_VERSION "1.5.7" CACHE STRING "Draco version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" DRACO_VERSION_MAJOR "${DRACO_VERSION}")

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
set(DRACO_CACHE_DIR "${FETCH_CACHE_DIR}/draco-v${DRACO_VERSION}")

# Draco构建类型配置 - 可以独立于主项目设置
if(DEFINED DRACO_BUILD_TYPE)
    # 如果明确指定了DRACO_BUILD_TYPE，使用它
    set(DRACO_ACTUAL_BUILD_TYPE "${DRACO_BUILD_TYPE}")
    message(STATUS "   使用指定的Draco构建类型: ${DRACO_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(DRACO_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 Draco")
    else()
        set(DRACO_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(DRACO_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(DRACO_BUILD_DIR "${DRACO_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${DRACO_ACTUAL_BUILD_TYPE}")

set(DRACO_INSTALL_DIR "${DRACO_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${DRACO_ACTUAL_BUILD_TYPE}")
set(DRACO_SOURCE_DIR "${DRACO_CACHE_DIR}/src")
set(DRACO_ZIP_FILE "${DRACO_CACHE_DIR}/draco-${DRACO_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 Draco 库缓存管理系统")
message(STATUS "   Draco版本: ${DRACO_VERSION}")
message(STATUS "   Draco构建类型: ${DRACO_ACTUAL_BUILD_TYPE}")
if(DEFINED DRACO_BUILD_TYPE AND NOT DRACO_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: Draco使用${DRACO_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   Draco缓存目录: ${DRACO_CACHE_DIR}")
message(STATUS "   构建目录: ${DRACO_BUILD_DIR}")
message(STATUS "   安装目录: ${DRACO_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${DRACO_CACHE_DIR}")

# 检查Draco库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# Draco 安装后会有 include/draco/draco_features.h
if(EXISTS "${DRACO_INSTALL_DIR}/include/draco/draco_features.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${DRACO_INSTALL_DIR}")
endif()

if(EXISTS "${DRACO_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _draco_built_libs
        "${DRACO_BUILD_DIR}/${DRACO_ACTUAL_BUILD_TYPE}/draco*"
        "${DRACO_BUILD_DIR}/libdraco*"
        "${DRACO_BUILD_DIR}/draco*.lib")
    if(_draco_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${DRACO_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${DRACO_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${DRACO_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${DRACO_SOURCE_DIR}")
endif()

if(EXISTS "${DRACO_ZIP_FILE}")
    set(_expected "27b72ba2d5ff3d0a9814ad40d4cb88f8dc89a35491c0866d952473f8f9416b77")
    file(SHA256 "${DRACO_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${DRACO_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${DRACO_ZIP_FILE}")
        file(REMOVE "${DRACO_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(DRACO_STATUS "NONE")
set(DRACO_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(DRACO_STATUS "INSTALLED")
    set(DRACO_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的Draco库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(DRACO_STATUS "BUILT_COMPLETE")
        set(DRACO_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的Draco（无需安装）")
    else()
        # 构建未完成，需要继续
        set(DRACO_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(DRACO_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(DRACO_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(DRACO_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载Draco")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${DRACO_STATUS}")

# 根据状态执行相应操作
if(DRACO_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的Draco
    message(STATUS "🚀 使用缓存的 Draco 库，跳过下载和编译")
    # Draco: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(DRACO_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 Draco...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${DRACO_BUILD_DIR} --target install --config ${DRACO_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${DRACO_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ Draco 安装成功")
        # Draco: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(DRACO_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ Draco 安装失败")
    endif()

elseif(DRACO_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 Draco...")

    # 先检查构建是否完整
    if(EXISTS "${DRACO_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${DRACO_BUILD_DIR} --target install --config ${DRACO_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${DRACO_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ Draco 安装成功")
            # Draco: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(DRACO_FOUND TRUE)
        else()
            message(WARNING "❌ Draco 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${DRACO_BUILD_DIR}")
            set(DRACO_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${DRACO_BUILD_DIR}")
        set(DRACO_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(DRACO_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 Draco...")

    # 检测可用的生成器
    set(DRACO_GENERATOR "")
    set(DRACO_GENERATOR_PLATFORM "")
    set(DRACO_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(DRACO_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(DRACO_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${DRACO_GENERATOR}")
        if(DRACO_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${DRACO_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(DRACO_GENERATOR "Ninja")
            set(DRACO_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(DRACO_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(DRACO_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(DRACO_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${DRACO_GENERATOR}")
    if(DRACO_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${DRACO_GENERATOR_PLATFORM})
    endif()
    if(DRACO_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${DRACO_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(DRACO_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 配置 Draco（默认参数即可，库本身简单）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${DRACO_SOURCE_DIR}
            -B ${DRACO_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${DRACO_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${DRACO_INSTALL_DIR}
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ Draco 配置成功")

        # 构建Draco
        message(STATUS "🔨 正在编译 Draco (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${DRACO_BUILD_DIR} --config ${DRACO_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${DRACO_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${DRACO_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${DRACO_BUILD_DIR} --config ${DRACO_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${DRACO_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ Draco 编译成功")

            # 安装Draco
            message(STATUS "📦 正在安装 Draco...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${DRACO_BUILD_DIR} --target install --config ${DRACO_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${DRACO_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ Draco 安装成功")
                # Draco: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(DRACO_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ Draco 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ Draco 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${DRACO_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "Draco 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ Draco 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "Draco 配置失败，请查看上面的错误信息")
    endif()

elseif(DRACO_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 Draco 源码...")

    # 确保源码目录不存在
    if(EXISTS "${DRACO_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${DRACO_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${DRACO_ZIP_FILE}
        WORKING_DIRECTORY ${DRACO_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # Draco 解压后目录名形如 draco-1.5.7
        file(GLOB DRACO_EXTRACTED_DIRS "${DRACO_CACHE_DIR}/draco-*")
        list(FILTER DRACO_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(GET DRACO_EXTRACTED_DIRS 0 DRACO_EXTRACTED_DIR)

        if(EXISTS "${DRACO_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${DRACO_EXTRACTED_DIR}" "${DRACO_SOURCE_DIR}")
            message(STATUS "✅ Draco 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(DRACO_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到Draco目录")
        endif()
    else()
        message(WARNING "❌ Draco 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${DRACO_ZIP_FILE}")
        set(DRACO_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 Draco ${DRACO_VERSION}...")

    # Draco GitHub release zip
    set(DRACO_DOWNLOAD_URL "https://github.com/google/draco/archive/refs/tags/${DRACO_VERSION}.zip")

    message(STATUS "📥 正在下载 Draco...")
    message(STATUS "   URL: ${DRACO_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${DRACO_DOWNLOAD_URL}
        ${DRACO_ZIP_FILE}
        EXPECTED_HASH SHA256=27b72ba2d5ff3d0a9814ad40d4cb88f8dc89a35491c0866d952473f8f9416b77
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ Draco 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ Draco 下载成功")

    # 解压
    message(STATUS "📦 解压 Draco 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${DRACO_ZIP_FILE}
        WORKING_DIRECTORY ${DRACO_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ Draco 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(DRACO_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ Draco 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(draco)
if(DRACO_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${DRACO_INSTALL_DIR}")
    if(EXISTS "${DRACO_INSTALL_DIR}/lib/cmake/draco/draco-config.cmake")
        set(draco_DIR "${DRACO_INSTALL_DIR}/lib/cmake/draco" CACHE PATH "draco config dir" FORCE)
    endif()
    set(Draco_AVAILABLE TRUE CACHE BOOL "Draco library is available")
    message(STATUS "")
    message(STATUS "🎯 Draco 库已就绪")
    message(STATUS "   版本: ${DRACO_VERSION}")
    message(STATUS "   安装目录: ${DRACO_INSTALL_DIR}")
else()
    set(Draco_AVAILABLE FALSE CACHE BOOL "Draco library is not available")
    message(FATAL_ERROR "⚠️  Draco 库不可用")
endif()