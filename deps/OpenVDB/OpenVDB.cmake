cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# OpenVDB版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED OPENVDB_VERSION)
    set(OPENVDB_VERSION "8.2-tm-vs2022" CACHE STRING "OpenVDB version label")
endif()
# OpenVDB 用 tamasmeszaros fork 的特定 commit
if(NOT DEFINED OPENVDB_COMMIT)
    set(OPENVDB_COMMIT "a68fd58d0e2b85f01adeb8b13d7555183ab10aa5")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" OPENVDB_VERSION_MAJOR "${OPENVDB_VERSION}")

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
set(OPENVDB_CACHE_DIR "${FETCH_CACHE_DIR}/openvdb-v${OPENVDB_VERSION}")

# OpenVDB构建类型配置 - 可以独立于主项目设置
if(DEFINED OPENVDB_BUILD_TYPE)
    # 如果明确指定了OPENVDB_BUILD_TYPE，使用它
    set(OPENVDB_ACTUAL_BUILD_TYPE "${OPENVDB_BUILD_TYPE}")
    message(STATUS "   使用指定的OpenVDB构建类型: ${OPENVDB_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(OPENVDB_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 OpenVDB")
    else()
        set(OPENVDB_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(OPENVDB_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(OPENVDB_BUILD_DIR "${OPENVDB_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${OPENVDB_ACTUAL_BUILD_TYPE}")

set(OPENVDB_INSTALL_DIR "${OPENVDB_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${OPENVDB_ACTUAL_BUILD_TYPE}")
set(OPENVDB_SOURCE_DIR "${OPENVDB_CACHE_DIR}/src")
set(OPENVDB_ZIP_FILE "${OPENVDB_CACHE_DIR}/openvdb-${OPENVDB_COMMIT}.zip")
set(OPENVDB_PATCH "${CMAKE_CURRENT_LIST_DIR}/0001-clang19.patch")
set(OPENVDB_PATCH_MARK "${OPENVDB_SOURCE_DIR}/.orca_patch_applied")

message(STATUS "")
message(STATUS "🔧 OpenVDB 库缓存管理系统")
message(STATUS "   OpenVDB版本: ${OPENVDB_VERSION}")
message(STATUS "   OpenVDB构建类型: ${OPENVDB_ACTUAL_BUILD_TYPE}")
if(DEFINED OPENVDB_BUILD_TYPE AND NOT OPENVDB_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: OpenVDB使用${OPENVDB_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   OpenVDB缓存目录: ${OPENVDB_CACHE_DIR}")
message(STATUS "   构建目录: ${OPENVDB_BUILD_DIR}")
message(STATUS "   安装目录: ${OPENVDB_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${OPENVDB_CACHE_DIR}")

# ---- Patch invalidation ----------------------------------------------------
# Mark file holds the SHA256 of the applied patch. Two distinct cases:
#   1) Mark exists, hash differs → patch has been edited. The whole tree is
#      stale (src has the old patch baked in, build/install are based on it).
#      Wipe everything and let ZIP_ONLY re-extract a virgin tree.
#   2) Mark missing but src exists → either a fresh extract (no build yet) or a
#      legacy tree predating the patch system. Keep src so SOURCE_ONLY can
#      apply the patch on it; only wipe build/install if they exist, since
#      they would have been produced from unpatched sources.
# Wiping src here on case 2 would loop forever: ZIP_ONLY re-extracts → recurses
# → no mark → wipes src again.
file(SHA256 "${OPENVDB_PATCH}" OPENVDB_PATCH_HASH)

set(_openvdb_patch_outdated FALSE)
set(_openvdb_force_repatch FALSE)
if(EXISTS "${OPENVDB_PATCH_MARK}")
    file(READ "${OPENVDB_PATCH_MARK}" _openvdb_existing_mark)
    string(STRIP "${_openvdb_existing_mark}" _openvdb_existing_mark)
    if(NOT _openvdb_existing_mark STREQUAL "${OPENVDB_PATCH_HASH}")
        set(_openvdb_patch_outdated TRUE)
        message(STATUS "🔄 OpenVDB patch 已变化 (旧=${_openvdb_existing_mark}, 新=${OPENVDB_PATCH_HASH})，清理过时缓存...")
    endif()
elseif(EXISTS "${OPENVDB_SOURCE_DIR}")
    if(EXISTS "${OPENVDB_BUILD_DIR}/CMakeCache.txt" OR EXISTS "${OPENVDB_INSTALL_DIR}/include/openvdb/openvdb.h")
        set(_openvdb_force_repatch TRUE)
        message(STATUS "🔄 OpenVDB 源码未 patch 但已有 build/install，清理 build/install 让 SOURCE_ONLY 重新 patch 后重建...")
    endif()
endif()

if(_openvdb_patch_outdated)
    file(REMOVE_RECURSE "${OPENVDB_SOURCE_DIR}")
    file(REMOVE_RECURSE "${OPENVDB_BUILD_DIR}")
    file(REMOVE_RECURSE "${OPENVDB_INSTALL_DIR}")
endif()

if(_openvdb_force_repatch)
    file(REMOVE_RECURSE "${OPENVDB_BUILD_DIR}")
    file(REMOVE_RECURSE "${OPENVDB_INSTALL_DIR}")
endif()

# 检查OpenVDB库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# OpenVDB 安装后会有 include/openvdb/openvdb.h
if(EXISTS "${OPENVDB_INSTALL_DIR}/include/openvdb/openvdb.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${OPENVDB_INSTALL_DIR}")
endif()

# CMake 项目：CMakeCache 即认为已配置，lib/ 下有产物即认为构建完成
if(EXISTS "${OPENVDB_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _vdb_built_libs
        "${OPENVDB_BUILD_DIR}/openvdb/openvdb/${OPENVDB_ACTUAL_BUILD_TYPE}/openvdb*"
        "${OPENVDB_BUILD_DIR}/lib/${OPENVDB_ACTUAL_BUILD_TYPE}/openvdb*"
        "${OPENVDB_BUILD_DIR}/lib/libopenvdb*"
        "${OPENVDB_BUILD_DIR}/lib/openvdb*.lib")
    if(_vdb_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${OPENVDB_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${OPENVDB_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${OPENVDB_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${OPENVDB_SOURCE_DIR}")
endif()

if(EXISTS "${OPENVDB_ZIP_FILE}")
    set(_expected "f353e7b99bd0cbfc27ac9082de51acf32a8bc0b3e21ff9661ecca6f205ec1d81")
    file(SHA256 "${OPENVDB_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${OPENVDB_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${OPENVDB_ZIP_FILE}")
        file(REMOVE "${OPENVDB_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(OPENVDB_STATUS "NONE")
set(OPENVDB_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(OPENVDB_STATUS "INSTALLED")
    set(OPENVDB_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的OpenVDB库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(OPENVDB_STATUS "BUILT_COMPLETE")
        set(OPENVDB_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的OpenVDB（无需安装）")
    else()
        # 构建未完成，需要继续
        set(OPENVDB_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(OPENVDB_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(OPENVDB_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(OPENVDB_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载OpenVDB")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${OPENVDB_STATUS}")

# 根据状态执行相应操作
if(OPENVDB_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的OpenVDB
    message(STATUS "🚀 使用缓存的 OpenVDB 库，跳过下载和编译")
    # OpenVDB: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(OPENVDB_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 OpenVDB...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${OPENVDB_BUILD_DIR} --target install --config ${OPENVDB_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${OPENVDB_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ OpenVDB 安装成功")
        # OpenVDB: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(OPENVDB_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ OpenVDB 安装失败")
    endif()

elseif(OPENVDB_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 OpenVDB...")

    # 先检查构建是否完整
    if(EXISTS "${OPENVDB_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${OPENVDB_BUILD_DIR} --target install --config ${OPENVDB_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${OPENVDB_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ OpenVDB 安装成功")
            # OpenVDB: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(OPENVDB_FOUND TRUE)
        else()
            message(WARNING "❌ OpenVDB 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${OPENVDB_BUILD_DIR}")
            set(OPENVDB_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${OPENVDB_BUILD_DIR}")
        set(OPENVDB_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(OPENVDB_STATUS STREQUAL "SOURCE_ONLY")
    # Apply patch on first configure of a fresh source tree.
    if(NOT EXISTS "${OPENVDB_PATCH_MARK}")
        find_package(Git REQUIRED)
        message(STATUS "🩹 应用 OpenVDB patch: ${OPENVDB_PATCH}")
        # git init makes the extracted tree a standalone git context so that
        # git apply behaves the same regardless of whether a parent .git exists.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} init -q
            WORKING_DIRECTORY ${OPENVDB_SOURCE_DIR}
            RESULT_VARIABLE _git_init_result
        )
        if(NOT _git_init_result EQUAL 0)
            message(FATAL_ERROR "❌ git init 失败于 ${OPENVDB_SOURCE_DIR}")
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} apply --verbose --ignore-space-change --whitespace=fix ${OPENVDB_PATCH}
            WORKING_DIRECTORY ${OPENVDB_SOURCE_DIR}
            RESULT_VARIABLE _patch_result
        )
        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR "❌ OpenVDB patch 应用失败")
        endif()
        file(WRITE "${OPENVDB_PATCH_MARK}" "${OPENVDB_PATCH_HASH}\n")
        message(STATUS "✅ OpenVDB patch 应用成功")
    endif()

    # 需要构建和安装
    message(STATUS "🔨 开始构建 OpenVDB...")

    # 检测可用的生成器
    set(OPENVDB_GENERATOR "")
    set(OPENVDB_GENERATOR_PLATFORM "")
    set(OPENVDB_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(OPENVDB_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(OPENVDB_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${OPENVDB_GENERATOR}")
        if(OPENVDB_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${OPENVDB_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(OPENVDB_GENERATOR "Ninja")
            set(OPENVDB_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(OPENVDB_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(OPENVDB_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(OPENVDB_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${OPENVDB_GENERATOR}")
    if(OPENVDB_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${OPENVDB_GENERATOR_PLATFORM})
    endif()
    if(OPENVDB_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${OPENVDB_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(OPENVDB_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # OpenVDB shared/static
    if(BUILD_SHARED_LIBS)
        set(_vdb_shared ON)
        set(_vdb_static OFF)
    else()
        set(_vdb_shared OFF)
        set(_vdb_static ON)
    endif()

    # 配置 OpenVDB（依赖 TBB / Blosc / OpenEXR / Boost，全部从 CMAKE_PREFIX_PATH 解析）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${OPENVDB_SOURCE_DIR}
            -B ${OPENVDB_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${OPENVDB_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${OPENVDB_INSTALL_DIR}
            "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            -DZLIB_ROOT=${ZLIB_ROOT}
            -DOPENVDB_BUILD_PYTHON_MODULE=OFF
            -DUSE_BLOSC=ON
            -DOPENVDB_CORE_SHARED=${_vdb_shared}
            -DOPENVDB_CORE_STATIC=${_vdb_static}
            -DOPENVDB_ENABLE_RPATH:BOOL=OFF
            -DTBB_STATIC=${_vdb_static}
            -DOPENVDB_BUILD_VDB_PRINT=OFF
            -DDISABLE_DEPENDENCY_VERSION_CHECKS=ON
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DUSE_CCACHE=OFF
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ OpenVDB 配置成功")

        # 构建OpenVDB
        message(STATUS "🔨 正在编译 OpenVDB (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${OPENVDB_BUILD_DIR} --config ${OPENVDB_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${OPENVDB_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${OPENVDB_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${OPENVDB_BUILD_DIR} --config ${OPENVDB_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${OPENVDB_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ OpenVDB 编译成功")

            # 安装OpenVDB
            message(STATUS "📦 正在安装 OpenVDB...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${OPENVDB_BUILD_DIR} --target install --config ${OPENVDB_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${OPENVDB_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ OpenVDB 安装成功")
                # OpenVDB: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(OPENVDB_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ OpenVDB 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ OpenVDB 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${OPENVDB_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "OpenVDB 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ OpenVDB 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "OpenVDB 配置失败，请查看上面的错误信息")
    endif()

elseif(OPENVDB_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 OpenVDB 源码...")

    # 确保源码目录不存在
    if(EXISTS "${OPENVDB_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${OPENVDB_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${OPENVDB_ZIP_FILE}
        WORKING_DIRECTORY ${OPENVDB_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # OpenVDB 解压后目录名形如 openvdb-<commit-hash>
        file(GLOB OPENVDB_EXTRACTED_DIRS "${OPENVDB_CACHE_DIR}/openvdb-*")
        list(FILTER OPENVDB_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(GET OPENVDB_EXTRACTED_DIRS 0 OPENVDB_EXTRACTED_DIR)

        if(EXISTS "${OPENVDB_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${OPENVDB_EXTRACTED_DIR}" "${OPENVDB_SOURCE_DIR}")
            message(STATUS "✅ OpenVDB 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(OPENVDB_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到OpenVDB目录")
        endif()
    else()
        message(WARNING "❌ OpenVDB 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${OPENVDB_ZIP_FILE}")
        set(OPENVDB_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 OpenVDB ${OPENVDB_VERSION}...")

    # OpenVDB 是 tamasmeszaros 的 fork 在特定 commit 的 zip
    set(OPENVDB_DOWNLOAD_URL "https://github.com/tamasmeszaros/openvdb/archive/${OPENVDB_COMMIT}.zip")

    message(STATUS "📥 正在下载 OpenVDB...")
    message(STATUS "   URL: ${OPENVDB_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${OPENVDB_DOWNLOAD_URL}
        ${OPENVDB_ZIP_FILE}
        EXPECTED_HASH SHA256=f353e7b99bd0cbfc27ac9082de51acf32a8bc0b3e21ff9661ecca6f205ec1d81
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ OpenVDB 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ OpenVDB 下载成功")

    # 解压
    message(STATUS "📦 解压 OpenVDB 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${OPENVDB_ZIP_FILE}
        WORKING_DIRECTORY ${OPENVDB_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ OpenVDB 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(OPENVDB_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ OpenVDB 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(OpenVDB)
if(OPENVDB_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${OPENVDB_INSTALL_DIR}")
    set(OpenVDB_ROOT "${OPENVDB_INSTALL_DIR}" CACHE PATH "OpenVDB root" FORCE)
    set(OpenVDB_AVAILABLE TRUE CACHE BOOL "OpenVDB library is available")
    message(STATUS "")
    message(STATUS "🎯 OpenVDB 库已就绪")
    message(STATUS "   版本: ${OPENVDB_VERSION}")
    message(STATUS "   安装目录: ${OPENVDB_INSTALL_DIR}")
else()
    set(OpenVDB_AVAILABLE FALSE CACHE BOOL "OpenVDB library is not available")
    message(FATAL_ERROR "⚠️  OpenVDB 库不可用")
endif()