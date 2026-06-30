cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# NanoSVG版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED NANOSVG_VERSION)
    set(NANOSVG_VERSION "softfever" CACHE STRING "NanoSVG fork tag")
endif()
# NanoSVG 用 SoftFever fork 的特定 commit
if(NOT DEFINED NANOSVG_COMMIT)
    set(NANOSVG_COMMIT "863f6aa97ef62028126fa2c19bd4350394c2e15e")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" NANOSVG_VERSION_MAJOR "${NANOSVG_VERSION}")

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
set(NANOSVG_CACHE_DIR "${FETCH_CACHE_DIR}/nanosvg-${NANOSVG_VERSION}")

# NanoSVG构建类型配置 - 可以独立于主项目设置
if(DEFINED NANOSVG_BUILD_TYPE)
    # 如果明确指定了NANOSVG_BUILD_TYPE，使用它
    set(NANOSVG_ACTUAL_BUILD_TYPE "${NANOSVG_BUILD_TYPE}")
    message(STATUS "   使用指定的NanoSVG构建类型: ${NANOSVG_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(NANOSVG_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 NanoSVG")
    else()
        set(NANOSVG_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(NANOSVG_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(NANOSVG_BUILD_DIR "${NANOSVG_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${NANOSVG_ACTUAL_BUILD_TYPE}")

set(NANOSVG_INSTALL_DIR "${NANOSVG_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${NANOSVG_ACTUAL_BUILD_TYPE}")
set(NANOSVG_SOURCE_DIR "${NANOSVG_CACHE_DIR}/src")
set(NANOSVG_ZIP_FILE "${NANOSVG_CACHE_DIR}/nanosvg-${NANOSVG_COMMIT}.zip")

message(STATUS "")
message(STATUS "🔧 NanoSVG 库缓存管理系统")
message(STATUS "   NanoSVG版本: ${NANOSVG_VERSION}")
message(STATUS "   NanoSVG构建类型: ${NANOSVG_ACTUAL_BUILD_TYPE}")
if(DEFINED NANOSVG_BUILD_TYPE AND NOT NANOSVG_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: NanoSVG使用${NANOSVG_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   NanoSVG缓存目录: ${NANOSVG_CACHE_DIR}")
message(STATUS "   构建目录: ${NANOSVG_BUILD_DIR}")
message(STATUS "   安装目录: ${NANOSVG_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${NANOSVG_CACHE_DIR}")

# 检查NanoSVG库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# NanoSVG 安装后会有 include/nanosvg/nanosvg.h
if(EXISTS "${NANOSVG_INSTALL_DIR}/include/nanosvg/nanosvg.h"
   OR EXISTS "${NANOSVG_INSTALL_DIR}/include/nanosvg.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${NANOSVG_INSTALL_DIR}")
endif()

if(EXISTS "${NANOSVG_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _nano_built_libs
        "${NANOSVG_BUILD_DIR}/${NANOSVG_ACTUAL_BUILD_TYPE}/nanosvg*"
        "${NANOSVG_BUILD_DIR}/libnanosvg*"
        "${NANOSVG_BUILD_DIR}/nanosvg*.lib")
    if(_nano_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${NANOSVG_BUILD_DIR}")
    else()
        # NanoSVG 是 header-only，可能不产生 lib，CMakeCache.txt 即认为完成
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${NANOSVG_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${NANOSVG_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${NANOSVG_SOURCE_DIR}")
endif()

if(EXISTS "${NANOSVG_ZIP_FILE}")
    set(_expected "1e3f94a052dd2abb139993ed3496c90ed4458d6f907027bf03cbaed889fe2567")
    file(SHA256 "${NANOSVG_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${NANOSVG_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${NANOSVG_ZIP_FILE}")
        file(REMOVE "${NANOSVG_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(NANOSVG_STATUS "NONE")
set(NANOSVG_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(NANOSVG_STATUS "INSTALLED")
    set(NANOSVG_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的NanoSVG库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(NANOSVG_STATUS "BUILT_COMPLETE")
        set(NANOSVG_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的NanoSVG（无需安装）")
    else()
        # 构建未完成，需要继续
        set(NANOSVG_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(NANOSVG_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(NANOSVG_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(NANOSVG_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载NanoSVG")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${NANOSVG_STATUS}")

# 根据状态执行相应操作
if(NANOSVG_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的NanoSVG
    message(STATUS "🚀 使用缓存的 NanoSVG 库，跳过下载和编译")
    # NanoSVG: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(NANOSVG_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 NanoSVG...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${NANOSVG_BUILD_DIR} --target install --config ${NANOSVG_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${NANOSVG_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ NanoSVG 安装成功")
        # NanoSVG: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(NANOSVG_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ NanoSVG 安装失败")
    endif()

elseif(NANOSVG_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 NanoSVG...")

    # 先检查构建是否完整
    if(EXISTS "${NANOSVG_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${NANOSVG_BUILD_DIR} --target install --config ${NANOSVG_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${NANOSVG_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ NanoSVG 安装成功")
            # NanoSVG: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(NANOSVG_FOUND TRUE)
        else()
            message(WARNING "❌ NanoSVG 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${NANOSVG_BUILD_DIR}")
            set(NANOSVG_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${NANOSVG_BUILD_DIR}")
        set(NANOSVG_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(NANOSVG_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 NanoSVG...")

    # 检测可用的生成器
    set(NANOSVG_GENERATOR "")
    set(NANOSVG_GENERATOR_PLATFORM "")
    set(NANOSVG_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(NANOSVG_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(NANOSVG_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${NANOSVG_GENERATOR}")
        if(NANOSVG_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${NANOSVG_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(NANOSVG_GENERATOR "Ninja")
            set(NANOSVG_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(NANOSVG_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(NANOSVG_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(NANOSVG_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${NANOSVG_GENERATOR}")
    if(NANOSVG_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${NANOSVG_GENERATOR_PLATFORM})
    endif()
    if(NANOSVG_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${NANOSVG_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(NANOSVG_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 配置 NanoSVG（依赖 Boost，从 CMAKE_PREFIX_PATH 解析）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${NANOSVG_SOURCE_DIR}
            -B ${NANOSVG_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${NANOSVG_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${NANOSVG_INSTALL_DIR}
            "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ NanoSVG 配置成功")

        # 构建NanoSVG
        message(STATUS "🔨 正在编译 NanoSVG (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${NANOSVG_BUILD_DIR} --config ${NANOSVG_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${NANOSVG_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${NANOSVG_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${NANOSVG_BUILD_DIR} --config ${NANOSVG_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${NANOSVG_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ NanoSVG 编译成功")

            # 安装NanoSVG
            message(STATUS "📦 正在安装 NanoSVG...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${NANOSVG_BUILD_DIR} --target install --config ${NANOSVG_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${NANOSVG_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ NanoSVG 安装成功")
                # NanoSVG: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(NANOSVG_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ NanoSVG 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ NanoSVG 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${NANOSVG_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "NanoSVG 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ NanoSVG 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "NanoSVG 配置失败，请查看上面的错误信息")
    endif()

elseif(NANOSVG_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 NanoSVG 源码...")

    # 确保源码目录不存在
    if(EXISTS "${NANOSVG_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${NANOSVG_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${NANOSVG_ZIP_FILE}
        WORKING_DIRECTORY ${NANOSVG_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # GitHub archive 顶层目录恒为 <repo>-<commit>，按 commit 定位最稳；
        # 仓库可能改名（如 nanosvg → Orca-deps-nanosvg），不能依赖固定前缀。
        file(GLOB _candidates "${NANOSVG_CACHE_DIR}/*${NANOSVG_COMMIT}*")
        set(NANOSVG_EXTRACTED_DIR "")
        foreach(_c IN LISTS _candidates)
            if(IS_DIRECTORY "${_c}")
                set(NANOSVG_EXTRACTED_DIR "${_c}")
                break()
            endif()
        endforeach()

        if(NANOSVG_EXTRACTED_DIR)
            file(RENAME "${NANOSVG_EXTRACTED_DIR}" "${NANOSVG_SOURCE_DIR}")
            message(STATUS "✅ NanoSVG 解压成功")

            set(HAS_SOURCE TRUE)
            set(NANOSVG_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到包含 commit ${NANOSVG_COMMIT} 的目录")
        endif()
    else()
        message(WARNING "❌ NanoSVG 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${NANOSVG_ZIP_FILE}")
        set(NANOSVG_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 NanoSVG ${NANOSVG_VERSION}...")

    # NanoSVG SoftFever fork (specific commit)
    # 注意: SoftFever/nanosvg 已改名为 SoftFever/Orca-deps-nanosvg，
    # 直接指向新仓库名以避免依赖 GitHub 301 重定向（重定向后归档顶层目录名变化会导致 SHA256 不匹配）
    set(NANOSVG_DOWNLOAD_URL "https://github.com/SoftFever/Orca-deps-nanosvg/archive/${NANOSVG_COMMIT}.zip")

    message(STATUS "📥 正在下载 NanoSVG...")
    message(STATUS "   URL: ${NANOSVG_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${NANOSVG_DOWNLOAD_URL}
        ${NANOSVG_ZIP_FILE}
        EXPECTED_HASH SHA256=1e3f94a052dd2abb139993ed3496c90ed4458d6f907027bf03cbaed889fe2567
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ NanoSVG 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ NanoSVG 下载成功")

    # 解压
    message(STATUS "📦 解压 NanoSVG 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${NANOSVG_ZIP_FILE}
        WORKING_DIRECTORY ${NANOSVG_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ NanoSVG 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(NANOSVG_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ NanoSVG 解压失败")
    endif()
endif()

# 注册到主项目
if(NANOSVG_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${NANOSVG_INSTALL_DIR}")
    list(APPEND ORCA_DEPS_INSTALL_PATHS "${NANOSVG_INSTALL_DIR}")
    set(ORCA_DEPS_INSTALL_PATHS "${ORCA_DEPS_INSTALL_PATHS}" CACHE INTERNAL "Per-dep install roots for runtime DLL lookup" FORCE)
    set(NanoSVG_AVAILABLE TRUE CACHE BOOL "NanoSVG library is available")
    message(STATUS "")
    message(STATUS "🎯 NanoSVG 库已就绪")
    message(STATUS "   安装目录: ${NANOSVG_INSTALL_DIR}")
else()
    set(NanoSVG_AVAILABLE FALSE CACHE BOOL "NanoSVG library is not available")
    message(FATAL_ERROR "⚠️  NanoSVG 库不可用")
endif()