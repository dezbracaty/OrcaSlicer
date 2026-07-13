cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# NLopt版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED NLOPT_VERSION)
    set(NLOPT_VERSION "2.5.0" CACHE STRING "NLopt version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" NLOPT_VERSION_MAJOR "${NLOPT_VERSION}")

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
set(NLOPT_CACHE_DIR "${FETCH_CACHE_DIR}/nlopt-v${NLOPT_VERSION}")

# NLopt构建类型配置 - 可以独立于主项目设置
if(DEFINED NLOPT_BUILD_TYPE)
    # 如果明确指定了NLOPT_BUILD_TYPE，使用它
    set(NLOPT_ACTUAL_BUILD_TYPE "${NLOPT_BUILD_TYPE}")
    message(STATUS "   使用指定的NLopt构建类型: ${NLOPT_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(NLOPT_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 NLopt")
    else()
        set(NLOPT_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(NLOPT_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(NLOPT_BUILD_DIR "${NLOPT_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${NLOPT_ACTUAL_BUILD_TYPE}")

set(NLOPT_INSTALL_DIR "${NLOPT_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${NLOPT_ACTUAL_BUILD_TYPE}")
set(NLOPT_SOURCE_DIR "${NLOPT_CACHE_DIR}/src")
set(NLOPT_ZIP_FILE "${NLOPT_CACHE_DIR}/nlopt-${NLOPT_VERSION}.tar.gz")

message(STATUS "")
message(STATUS "🔧 NLopt 库缓存管理系统")
message(STATUS "   NLopt版本: ${NLOPT_VERSION}")
message(STATUS "   NLopt构建类型: ${NLOPT_ACTUAL_BUILD_TYPE}")
if(DEFINED NLOPT_BUILD_TYPE AND NOT NLOPT_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: NLopt使用${NLOPT_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   NLopt缓存目录: ${NLOPT_CACHE_DIR}")
message(STATUS "   构建目录: ${NLOPT_BUILD_DIR}")
message(STATUS "   安装目录: ${NLOPT_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${NLOPT_CACHE_DIR}")

# 检查NLopt库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

function(_nlopt_validate_install_contract install_dir out_valid out_package_dir out_error)
    file(GLOB _nlopt_config_candidates LIST_DIRECTORIES FALSE
        "${install_dir}/lib/cmake/nlopt/NLoptConfig.cmake"
        "${install_dir}/lib64/cmake/nlopt/NLoptConfig.cmake"
    )
    file(GLOB _nlopt_targets_candidates LIST_DIRECTORIES FALSE
        "${install_dir}/lib/cmake/nlopt/NLoptLibraryDepends.cmake"
        "${install_dir}/lib64/cmake/nlopt/NLoptLibraryDepends.cmake"
    )
    list(LENGTH _nlopt_config_candidates _nlopt_config_count)
    list(LENGTH _nlopt_targets_candidates _nlopt_targets_count)

    set(_valid TRUE)
    set(_package_dir "")
    set(_error "")
    if(NOT _nlopt_config_count EQUAL 1 OR NOT _nlopt_targets_count EQUAL 1)
        set(_valid FALSE)
        set(_error
            "expected one NLoptConfig.cmake and one NLoptLibraryDepends.cmake; "
            "found ${_nlopt_config_count} config and ${_nlopt_targets_count} targets files")
    else()
        list(GET _nlopt_config_candidates 0 _nlopt_config)
        list(GET _nlopt_targets_candidates 0 _nlopt_targets)
        get_filename_component(_package_dir "${_nlopt_config}" DIRECTORY)
        file(READ "${_nlopt_config}" _nlopt_config_content)
        file(READ "${_nlopt_targets}" _nlopt_targets_content)
        if(NOT _nlopt_targets_content MATCHES "NLopt::nlopt([ \t\r\n\\)]|$)")
            set(_valid FALSE)
            string(APPEND _error " required imported target NLopt::nlopt is missing;")
        endif()
        if(_nlopt_config_content MATCHES "NLopt::nlopt_cxx"
           OR _nlopt_targets_content MATCHES "NLopt::nlopt_cxx")
            set(_valid FALSE)
            string(APPEND _error " package exports NLopt::nlopt_cxx instead of the required C target;")
        endif()
    endif()

    set(${out_valid} "${_valid}" PARENT_SCOPE)
    set(${out_package_dir} "${_package_dir}" PARENT_SCOPE)
    set(${out_error} "${_error}" PARENT_SCOPE)
endfunction()

# NLopt 安装后会有 include/nlopt.h
if(EXISTS "${NLOPT_INSTALL_DIR}/include/nlopt.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${NLOPT_INSTALL_DIR}")
endif()

if(HAS_INSTALL)
    _nlopt_validate_install_contract("${NLOPT_INSTALL_DIR}"
        _nlopt_install_contract_valid _nlopt_package_dir _nlopt_install_contract_error)
    if(NOT _nlopt_install_contract_valid)
        message(WARNING
            "Rejecting incompatible NLopt ${NLOPT_ACTUAL_BUILD_TYPE} install:"
            "${_nlopt_install_contract_error} Rebuilding this build/install entry with NLOPT_CXX=OFF.")
        file(REMOVE_RECURSE "${NLOPT_BUILD_DIR}" "${NLOPT_INSTALL_DIR}")
        set(HAS_INSTALL FALSE)
        set(HAS_BUILD FALSE)
    endif()
endif()

# CMake 项目：CMakeCache 即认为已配置，lib/ 下有产物即认为构建完成
if(EXISTS "${NLOPT_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _nlopt_built_libs
        "${NLOPT_BUILD_DIR}/${NLOPT_ACTUAL_BUILD_TYPE}/nlopt*"
        "${NLOPT_BUILD_DIR}/libnlopt*"
        "${NLOPT_BUILD_DIR}/nlopt*.lib")
    if(_nlopt_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${NLOPT_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${NLOPT_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${NLOPT_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${NLOPT_SOURCE_DIR}")
endif()

if(EXISTS "${NLOPT_ZIP_FILE}")
    set(_expected "c6dd7a5701fff8ad5ebb45a3dc8e757e61d52658de3918e38bab233e7fd3b4ae")
    file(SHA256 "${NLOPT_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${NLOPT_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${NLOPT_ZIP_FILE}")
        file(REMOVE "${NLOPT_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(NLOPT_STATUS "NONE")
set(NLOPT_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(NLOPT_STATUS "INSTALLED")
    set(NLOPT_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的NLopt库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(NLOPT_STATUS "BUILT_COMPLETE")
        set(NLOPT_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的NLopt（无需安装）")
    else()
        # 构建未完成，需要继续
        set(NLOPT_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(NLOPT_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(NLOPT_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(NLOPT_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载NLopt")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${NLOPT_STATUS}")

# 根据状态执行相应操作
if(NLOPT_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的NLopt
    message(STATUS "🚀 使用缓存的 NLopt 库，跳过下载和编译")
    # NLopt: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(NLOPT_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 NLopt...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${NLOPT_BUILD_DIR} --target install --config ${NLOPT_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${NLOPT_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ NLopt 安装成功")
        # NLopt: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(NLOPT_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ NLopt 安装失败")
    endif()

elseif(NLOPT_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 NLopt...")

    # 先检查构建是否完整
    if(EXISTS "${NLOPT_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${NLOPT_BUILD_DIR} --target install --config ${NLOPT_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${NLOPT_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ NLopt 安装成功")
            # NLopt: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(NLOPT_FOUND TRUE)
        else()
            message(WARNING "❌ NLopt 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${NLOPT_BUILD_DIR}")
            set(NLOPT_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${NLOPT_BUILD_DIR}")
        set(NLOPT_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(NLOPT_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 NLopt...")

    # 检测可用的生成器
    set(NLOPT_GENERATOR "")
    set(NLOPT_GENERATOR_PLATFORM "")
    set(NLOPT_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(NLOPT_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(NLOPT_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${NLOPT_GENERATOR}")
        if(NLOPT_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${NLOPT_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(NLOPT_GENERATOR "Ninja")
            set(NLOPT_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(NLOPT_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(NLOPT_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(NLOPT_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${NLOPT_GENERATOR}")
    if(NLOPT_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${NLOPT_GENERATOR_PLATFORM})
    endif()
    if(NLOPT_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${NLOPT_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(NLOPT_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 配置 NLopt（关闭所有语言绑定和测试）
    if(BUILD_SHARED_LIBS)
        set(_nlopt_shared ON)
    else()
        set(_nlopt_shared OFF)
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${NLOPT_SOURCE_DIR}
            -B ${NLOPT_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${NLOPT_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${NLOPT_INSTALL_DIR}
            -DBUILD_SHARED_LIBS=${_nlopt_shared}
            -DNLOPT_CXX:BOOL=OFF
            -DNLOPT_PYTHON:BOOL=OFF
            -DNLOPT_OCTAVE:BOOL=OFF
            -DNLOPT_MATLAB:BOOL=OFF
            -DNLOPT_GUILE:BOOL=OFF
            -DNLOPT_SWIG:BOOL=OFF
            -DNLOPT_TESTS:BOOL=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ NLopt 配置成功")

        # 构建NLopt
        message(STATUS "🔨 正在编译 NLopt (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${NLOPT_BUILD_DIR} --config ${NLOPT_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${NLOPT_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${NLOPT_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${NLOPT_BUILD_DIR} --config ${NLOPT_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${NLOPT_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ NLopt 编译成功")

            # 安装NLopt
            message(STATUS "📦 正在安装 NLopt...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${NLOPT_BUILD_DIR} --target install --config ${NLOPT_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${NLOPT_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ NLopt 安装成功")
                # NLopt: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(NLOPT_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ NLopt 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ NLopt 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${NLOPT_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "NLopt 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ NLopt 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "NLopt 配置失败，请查看上面的错误信息")
    endif()

elseif(NLOPT_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 NLopt 源码...")

    # 确保源码目录不存在
    if(EXISTS "${NLOPT_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${NLOPT_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${NLOPT_ZIP_FILE}
        WORKING_DIRECTORY ${NLOPT_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # NLopt 解压后目录名为 nlopt-2.5.0
        file(GLOB NLOPT_EXTRACTED_DIRS "${NLOPT_CACHE_DIR}/nlopt-*")
        list(FILTER NLOPT_EXTRACTED_DIRS EXCLUDE REGEX "\\.tar\\.gz$")
        list(GET NLOPT_EXTRACTED_DIRS 0 NLOPT_EXTRACTED_DIR)

        if(EXISTS "${NLOPT_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${NLOPT_EXTRACTED_DIR}" "${NLOPT_SOURCE_DIR}")
            message(STATUS "✅ NLopt 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(NLOPT_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到NLopt目录")
        endif()
    else()
        message(WARNING "❌ NLopt 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${NLOPT_ZIP_FILE}")
        set(NLOPT_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 NLopt ${NLOPT_VERSION}...")

    # NLopt GitHub archive
    set(NLOPT_DOWNLOAD_URL "https://github.com/stevengj/nlopt/archive/v${NLOPT_VERSION}.tar.gz")

    message(STATUS "📥 正在下载 NLopt...")
    message(STATUS "   URL: ${NLOPT_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${NLOPT_DOWNLOAD_URL}
        ${NLOPT_ZIP_FILE}
        EXPECTED_HASH SHA256=c6dd7a5701fff8ad5ebb45a3dc8e757e61d52658de3918e38bab233e7fd3b4ae
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ NLopt 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ NLopt 下载成功")

    # 解压
    message(STATUS "📦 解压 NLopt 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${NLOPT_ZIP_FILE}
        WORKING_DIRECTORY ${NLOPT_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ NLopt 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(NLOPT_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ NLopt 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(NLopt)
if(NLOPT_FOUND)
    _nlopt_validate_install_contract("${NLOPT_INSTALL_DIR}"
        _nlopt_install_contract_valid _nlopt_package_dir _nlopt_install_contract_error)
    if(NOT _nlopt_install_contract_valid)
        message(FATAL_ERROR
            "NLopt install does not satisfy the required NLopt::nlopt target contract:"
            "${_nlopt_install_contract_error}")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${NLOPT_INSTALL_DIR}")
    set(NLopt_DIR "${_nlopt_package_dir}" CACHE PATH "NLopt cmake config dir" FORCE)
    set(NLopt_AVAILABLE TRUE CACHE BOOL "NLopt library is available")
    message(STATUS "")
    message(STATUS "🎯 NLopt 库已就绪")
    message(STATUS "   版本: ${NLOPT_VERSION}")
    message(STATUS "   安装目录: ${NLOPT_INSTALL_DIR}")
else()
    set(NLopt_AVAILABLE FALSE CACHE BOOL "NLopt library is not available")
    message(FATAL_ERROR "⚠️  NLopt 库不可用")
endif()
