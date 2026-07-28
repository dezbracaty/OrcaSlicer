cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# OCCT版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED OCCT_VERSION)
    set(OCCT_VERSION "V7_6_0" CACHE STRING "OCCT version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" OCCT_VERSION_MAJOR "${OCCT_VERSION}")

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
set(OCCT_CACHE_DIR "${FETCH_CACHE_DIR}/occt-${OCCT_VERSION}")

# OCCT构建类型配置 - 可以独立于主项目设置
if(DEFINED OCCT_BUILD_TYPE)
    # 如果明确指定了OCCT_BUILD_TYPE，使用它
    set(OCCT_ACTUAL_BUILD_TYPE "${OCCT_BUILD_TYPE}")
    message(STATUS "   使用指定的OCCT构建类型: ${OCCT_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(OCCT_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 OCCT")
    else()
        set(OCCT_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(OCCT_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(OCCT_BUILD_DIR "${OCCT_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${OCCT_ACTUAL_BUILD_TYPE}")

set(OCCT_INSTALL_DIR "${OCCT_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${OCCT_ACTUAL_BUILD_TYPE}")
set(OCCT_SOURCE_DIR "${OCCT_CACHE_DIR}/src")
set(OCCT_ZIP_FILE "${OCCT_CACHE_DIR}/OCCT-${OCCT_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 OCCT 库缓存管理系统")
message(STATUS "   OCCT版本: ${OCCT_VERSION}")
message(STATUS "   OCCT构建类型: ${OCCT_ACTUAL_BUILD_TYPE}")
if(DEFINED OCCT_BUILD_TYPE AND NOT OCCT_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: OCCT使用${OCCT_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   OCCT缓存目录: ${OCCT_CACHE_DIR}")
message(STATUS "   构建目录: ${OCCT_BUILD_DIR}")
message(STATUS "   安装目录: ${OCCT_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${OCCT_CACHE_DIR}")

# A cache directory name identifies the intended architecture and build type,
# but it does not prove that the existing objects were configured for them.
# Follow the same rule as GPlatform's fetch_vtk: validate the actual CMakeCache
# before accepting either a build tree or its install tree.
if(APPLE AND (EXISTS "${OCCT_BUILD_DIR}" OR EXISTS "${OCCT_INSTALL_DIR}"))
    set(_occt_cache_file "${OCCT_BUILD_DIR}/CMakeCache.txt")
    set(_occt_cache_abi_valid TRUE)
    set(_occt_cache_abi_error "")

    if(NOT EXISTS "${_occt_cache_file}")
        set(_occt_cache_abi_valid FALSE)
        set(_occt_cache_abi_error "missing ${_occt_cache_file}")
    else()
        foreach(_occt_cache_name
                CMAKE_BUILD_TYPE
                CMAKE_OSX_ARCHITECTURES
                CMAKE_OSX_DEPLOYMENT_TARGET
                CMAKE_OSX_SYSROOT)
            file(STRINGS "${_occt_cache_file}" _occt_cache_lines
                REGEX "^${_occt_cache_name}:[^=]*=")
            list(LENGTH _occt_cache_lines _occt_cache_line_count)
            if(NOT _occt_cache_line_count EQUAL 1)
                set(_occt_cache_abi_valid FALSE)
                set(_occt_cache_abi_error
                    "${_occt_cache_name}: expected one cache entry, found ${_occt_cache_line_count}")
                break()
            endif()
            list(GET _occt_cache_lines 0 _occt_cache_line)
            string(REGEX REPLACE "^[^=]*=" "" _occt_cache_value
                "${_occt_cache_line}")

            if(_occt_cache_name STREQUAL "CMAKE_BUILD_TYPE")
                set(_occt_expected_value "${OCCT_ACTUAL_BUILD_TYPE}")
            elseif(_occt_cache_name STREQUAL "CMAKE_OSX_ARCHITECTURES")
                set(_occt_expected_value "${CMAKE_OSX_ARCHITECTURES}")
            elseif(_occt_cache_name STREQUAL "CMAKE_OSX_DEPLOYMENT_TARGET")
                set(_occt_expected_value "${CMAKE_OSX_DEPLOYMENT_TARGET}")
            else()
                set(_occt_expected_value "${CMAKE_OSX_SYSROOT}")
            endif()

            if(NOT "${_occt_cache_value}" STREQUAL "${_occt_expected_value}")
                set(_occt_cache_abi_valid FALSE)
                set(_occt_cache_abi_error
                    "${_occt_cache_name}='${_occt_cache_value}', expected '${_occt_expected_value}'")
                break()
            endif()
        endforeach()
    endif()

    if(_occt_cache_abi_valid)
        message(STATUS
            "✅ OCCT macOS ABI cache verified: deployment=${CMAKE_OSX_DEPLOYMENT_TARGET}, "
            "architectures=${CMAKE_OSX_ARCHITECTURES}")
    else()
        message(STATUS
            "♻️  OCCT macOS ABI cache is incompatible (${_occt_cache_abi_error}); "
            "removing only ${OCCT_BUILD_DIR} and ${OCCT_INSTALL_DIR}")
        file(REMOVE_RECURSE "${OCCT_BUILD_DIR}" "${OCCT_INSTALL_DIR}")
    endif()

    unset(_occt_cache_file)
    unset(_occt_cache_abi_valid)
    unset(_occt_cache_abi_error)
    unset(_occt_cache_name)
    unset(_occt_cache_lines)
    unset(_occt_cache_line_count)
    unset(_occt_cache_line)
    unset(_occt_cache_value)
    unset(_occt_expected_value)
endif()

# 检查OCCT库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# OCCT 安装后会有 OpenCASCADEConfig.cmake，子目录名因版本而异（occt / opencascade）
file(GLOB _occt_config_candidates
    "${OCCT_INSTALL_DIR}/cmake/OpenCASCADEConfig.cmake"
    "${OCCT_INSTALL_DIR}/lib/cmake/occt/OpenCASCADEConfig.cmake"
    "${OCCT_INSTALL_DIR}/lib/cmake/opencascade/OpenCASCADEConfig.cmake"
)
if(_occt_config_candidates)
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${OCCT_INSTALL_DIR}")
endif()

if(EXISTS "${OCCT_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _occt_built_libs
        "${OCCT_BUILD_DIR}/win64/vc14/bin/${OCCT_ACTUAL_BUILD_TYPE}/TK*"
        "${OCCT_BUILD_DIR}/${OCCT_ACTUAL_BUILD_TYPE}/TK*"
        "${OCCT_BUILD_DIR}/lib/${OCCT_ACTUAL_BUILD_TYPE}/TK*"
        "${OCCT_BUILD_DIR}/lib/libTK*"
        "${OCCT_BUILD_DIR}/lib/TK*.lib")
    if(_occt_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${OCCT_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${OCCT_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${OCCT_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${OCCT_SOURCE_DIR}")
endif()

if(EXISTS "${OCCT_ZIP_FILE}")
    set(_expected "28334f0e98f1b1629799783e9b4d21e05349d89e695809d7e6dfa45ea43e1dbc")
    file(SHA256 "${OCCT_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${OCCT_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${OCCT_ZIP_FILE}")
        file(REMOVE "${OCCT_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(OCCT_STATUS "NONE")
set(OCCT_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(OCCT_STATUS "INSTALLED")
    set(OCCT_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的OCCT库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(OCCT_STATUS "BUILT_COMPLETE")
        set(OCCT_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的OCCT（无需安装）")
    else()
        # 构建未完成，需要继续
        set(OCCT_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(OCCT_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(OCCT_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(OCCT_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载OCCT")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${OCCT_STATUS}")

# 根据状态执行相应操作
if(OCCT_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的OCCT
    message(STATUS "🚀 使用缓存的 OCCT 库，跳过下载和编译")
    # OCCT: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(OCCT_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 OCCT...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${OCCT_BUILD_DIR} --target install --config ${OCCT_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${OCCT_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ OCCT 安装成功")
        # OCCT: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(OCCT_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ OCCT 安装失败")
    endif()

elseif(OCCT_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 OCCT...")

    # 先检查构建是否完整
    if(EXISTS "${OCCT_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${OCCT_BUILD_DIR} --target install --config ${OCCT_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${OCCT_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ OCCT 安装成功")
            # OCCT: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(OCCT_FOUND TRUE)
        else()
            message(WARNING "❌ OCCT 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${OCCT_BUILD_DIR}")
            set(OCCT_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${OCCT_BUILD_DIR}")
        set(OCCT_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(OCCT_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 OCCT...")

    # 应用 patch（首次解压时）
    set(_occt_patch_mark "${OCCT_SOURCE_DIR}/.orca_patched")
    if(NOT EXISTS "${_occt_patch_mark}")
        find_package(Git REQUIRED)
        set(_occt_patches "${CMAKE_CURRENT_LIST_DIR}/0001-OCCT-fix.patch")
        message(STATUS "🩹 应用 OCCT patch...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} init -q
            WORKING_DIRECTORY ${OCCT_SOURCE_DIR}
        )
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --verbose --ignore-space-change --whitespace=fix ${_occt_patches}
            WORKING_DIRECTORY ${OCCT_SOURCE_DIR}
            RESULT_VARIABLE _patch_result
        )
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "❌ OCCT patch 应用失败")
        endif()
        file(WRITE "${_occt_patch_mark}" "patched\n")
        message(STATUS "✅ OCCT patch 应用成功")
    endif()

    # Windows 上 OCCT 编译为 Shared
    if(WIN32)
        set(_occt_lib_type "Shared")
    else()
        set(_occt_lib_type "Static")
    endif()

    # 检测可用的生成器
    set(OCCT_GENERATOR "")
    set(OCCT_GENERATOR_PLATFORM "")
    set(OCCT_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(OCCT_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(OCCT_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${OCCT_GENERATOR}")
        if(OCCT_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${OCCT_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(OCCT_GENERATOR "Ninja")
            set(OCCT_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(OCCT_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(OCCT_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(OCCT_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${OCCT_GENERATOR}")
    if(OCCT_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${OCCT_GENERATOR_PLATFORM})
    endif()
    if(OCCT_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${OCCT_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(OCCT_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # MSBuild 在 VS generator 下默认会注入若干 /Zc: 一致性 flag。Ninja+裸 cl.exe
    # 没有这些默认值，会导致 OCCT 7.6.0 的某些模板代码在 MSVC 17.14 下触发 C2
    # 优化器 ICE (C1001 in ShapeConstruct_MakeTriangulation::Triangulate)。
    # 在 Ninja 路径上手动补回这些 flag，恢复与 VS generator 等价的编译行为。
    set(_occt_msbuild_compat_flags "")
    if(MSVC AND OCCT_GENERATOR STREQUAL "Ninja")
        set(_occt_msbuild_compat_flags
            "-DCMAKE_CXX_FLAGS=/permissive- /Zc:__cplusplus /Zc:wchar_t /Zc:inline /Zc:forScope /Zc:rvalueCast /Zc:throwingNew"
            "-DCMAKE_C_FLAGS=/Zc:inline"
        )
    endif()

    # 配置 OCCT（依赖 FREETYPE）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${OCCT_SOURCE_DIR}
            -B ${OCCT_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${OCCT_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${OCCT_INSTALL_DIR}
            "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            -DCMAKE_CXX_STANDARD=17
            -DBUILD_LIBRARY_TYPE=${_occt_lib_type}
            -DUSE_TK=OFF
            -DUSE_TBB=OFF
            -DUSE_FFMPEG=OFF
            -DUSE_OCCT=OFF
            -DBUILD_DOC_Overview=OFF
            -DBUILD_MODULE_ApplicationFramework=OFF
            -DBUILD_MODULE_Draw=OFF
            -DBUILD_MODULE_FoundationClasses=OFF
            -DBUILD_MODULE_ModelingAlgorithms=OFF
            -DBUILD_MODULE_ModelingData=OFF
            -DBUILD_MODULE_Visualization=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            ${_occt_msbuild_compat_flags}
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ OCCT 配置成功")

        # 构建OCCT
        message(STATUS "🔨 正在编译 OCCT (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${OCCT_BUILD_DIR} --config ${OCCT_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${OCCT_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${OCCT_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${OCCT_BUILD_DIR} --config ${OCCT_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${OCCT_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ OCCT 编译成功")

            # 安装OCCT
            message(STATUS "📦 正在安装 OCCT...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${OCCT_BUILD_DIR} --target install --config ${OCCT_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${OCCT_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ OCCT 安装成功")
                # OCCT: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(OCCT_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ OCCT 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ OCCT 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${OCCT_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "OCCT 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ OCCT 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "OCCT 配置失败，请查看上面的错误信息")
    endif()

elseif(OCCT_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 OCCT 源码...")

    # 确保源码目录不存在
    if(EXISTS "${OCCT_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${OCCT_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${OCCT_ZIP_FILE}
        WORKING_DIRECTORY ${OCCT_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # OCCT 解压后目录名形如 OCCT-V7_6_0
        file(GLOB OCCT_EXTRACTED_DIRS "${OCCT_CACHE_DIR}/OCCT-*")
        list(FILTER OCCT_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(GET OCCT_EXTRACTED_DIRS 0 OCCT_EXTRACTED_DIR)

        if(EXISTS "${OCCT_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${OCCT_EXTRACTED_DIR}" "${OCCT_SOURCE_DIR}")
            message(STATUS "✅ OCCT 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(OCCT_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到OCCT目录")
        endif()
    else()
        message(WARNING "❌ OCCT 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${OCCT_ZIP_FILE}")
        set(OCCT_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 OCCT ${OCCT_VERSION}...")

    # OCCT GitHub release zip
    set(OCCT_DOWNLOAD_URL "https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/${OCCT_VERSION}.zip")

    message(STATUS "📥 正在下载 OCCT...")
    message(STATUS "   URL: ${OCCT_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${OCCT_DOWNLOAD_URL}
        ${OCCT_ZIP_FILE}
        EXPECTED_HASH SHA256=28334f0e98f1b1629799783e9b4d21e05349d89e695809d7e6dfa45ea43e1dbc
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ OCCT 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ OCCT 下载成功")

    # 解压
    message(STATUS "📦 解压 OCCT 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${OCCT_ZIP_FILE}
        WORKING_DIRECTORY ${OCCT_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ OCCT 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(OCCT_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ OCCT 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(OpenCASCADE)
if(OCCT_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${OCCT_INSTALL_DIR}")
    set(OpenCASCADE_DIR "${OCCT_INSTALL_DIR}/lib/cmake/occt" CACHE PATH "OpenCASCADE config dir" FORCE)
    list(APPEND ORCA_DEPS_INSTALL_PATHS "${OCCT_INSTALL_DIR}")
    set(ORCA_DEPS_INSTALL_PATHS "${ORCA_DEPS_INSTALL_PATHS}" CACHE INTERNAL "Per-dep install roots for runtime DLL lookup" FORCE)
    set(OCCT_AVAILABLE TRUE CACHE BOOL "OCCT library is available")
    message(STATUS "")
    message(STATUS "🎯 OCCT 库已就绪")
    message(STATUS "   版本: ${OCCT_VERSION}")
    message(STATUS "   安装目录: ${OCCT_INSTALL_DIR}")
else()
    set(OCCT_AVAILABLE FALSE CACHE BOOL "OCCT library is not available")
    message(FATAL_ERROR "⚠️  OCCT 库不可用")
endif()
