cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# wxWidgets版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED WX_VERSION)
    set(WX_VERSION "v3.3.2" CACHE STRING "wxWidgets fork tag")
endif()

# 自动计算主版本号，WX_VERSION 允许使用 v3.3.2 这类 tag 名称
if(WX_VERSION MATCHES "^[^0-9]+(.+)$")
    set(WX_VERSION_NUMBER "${CMAKE_MATCH_1}")
else()
    set(WX_VERSION_NUMBER "${WX_VERSION}")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" WX_VERSION_MAJOR "${WX_VERSION_NUMBER}")

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
set(WX_CACHE_DIR "${FETCH_CACHE_DIR}/wxwidgets-${WX_VERSION}")

# wxWidgets构建类型配置 - 可以独立于主项目设置
if(DEFINED WX_BUILD_TYPE)
    # 如果明确指定了WX_BUILD_TYPE，使用它
    set(WX_ACTUAL_BUILD_TYPE "${WX_BUILD_TYPE}")
    message(STATUS "   使用指定的wxWidgets构建类型: ${WX_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(WX_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 wxWidgets")
    else()
        set(WX_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(WX_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(WX_BUILD_DIR "${WX_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${WX_ACTUAL_BUILD_TYPE}")

set(WX_INSTALL_DIR "${WX_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${WX_ACTUAL_BUILD_TYPE}")
set(WX_SOURCE_DIR "${WX_CACHE_DIR}/src")
set(WX_ZIP_FILE "${WX_CACHE_DIR}/wxWidgets-${WX_VERSION}.tar.gz")

if(WX_VERSION_MAJOR)
    set(_WX_PRIVATE_HEADERS_INSTALL_DIR "${WX_INSTALL_DIR}/include/wx-${WX_VERSION_MAJOR}/wx")
else()
    set(_WX_PRIVATE_HEADERS_INSTALL_DIR "${WX_INSTALL_DIR}/include/wx")
endif()

function(orcaslicer_copy_wx_private_headers)
    if(EXISTS "${WX_SOURCE_DIR}/include/wx/private")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${WX_SOURCE_DIR}/include/wx/private"
                "${_WX_PRIVATE_HEADERS_INSTALL_DIR}/private"
            RESULT_VARIABLE _wx_copy_private_result
        )
        if(NOT _wx_copy_private_result EQUAL 0)
            message(FATAL_ERROR "❌ wxWidgets private headers 复制失败")
        endif()
    endif()

    foreach(_wx_private_subdir generic gtk osx)
        if(EXISTS "${WX_SOURCE_DIR}/include/wx/${_wx_private_subdir}/private")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${WX_SOURCE_DIR}/include/wx/${_wx_private_subdir}/private"
                    "${_WX_PRIVATE_HEADERS_INSTALL_DIR}/${_wx_private_subdir}/private"
                RESULT_VARIABLE _wx_copy_private_result
            )
            if(NOT _wx_copy_private_result EQUAL 0)
                message(FATAL_ERROR "❌ wxWidgets ${_wx_private_subdir}/private headers 复制失败")
            endif()
        endif()
    endforeach()
endfunction()

message(STATUS "")
message(STATUS "🔧 wxWidgets 库缓存管理系统")
message(STATUS "   wxWidgets版本: ${WX_VERSION}")
message(STATUS "   wxWidgets构建类型: ${WX_ACTUAL_BUILD_TYPE}")
if(DEFINED WX_BUILD_TYPE AND NOT WX_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: wxWidgets使用${WX_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   wxWidgets缓存目录: ${WX_CACHE_DIR}")
message(STATUS "   构建目录: ${WX_BUILD_DIR}")
message(STATUS "   安装目录: ${WX_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${WX_CACHE_DIR}")

# 检查wxWidgets库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# wxWidgets 安装后会有 include/wx/wx.h
# SoftFever fork 直接安装到 include/wx/，不带版本号子目录
file(GLOB _wx_wxh_candidates
    "${WX_INSTALL_DIR}/include/wx/wx.h"
    "${WX_INSTALL_DIR}/include/wx-*/wx/wx.h"
)
if(_wx_wxh_candidates)
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${WX_INSTALL_DIR}")
endif()

if(EXISTS "${WX_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _wx_built_libs
        "${WX_BUILD_DIR}/lib/${WX_ACTUAL_BUILD_TYPE}/wx*"
        "${WX_BUILD_DIR}/lib/libwx*"
        "${WX_BUILD_DIR}/lib/wx*.lib")
    if(_wx_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${WX_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${WX_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${WX_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${WX_SOURCE_DIR}")
endif()

# 决定操作策略
set(WX_STATUS "NONE")
set(WX_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(WX_STATUS "INSTALLED")
    set(WX_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的wxWidgets库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(WX_STATUS "BUILT_COMPLETE")
        set(WX_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的wxWidgets（无需安装）")
    else()
        # 构建未完成，需要继续
        set(WX_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(WX_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(WX_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(WX_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载wxWidgets")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${WX_STATUS}")

# 根据状态执行相应操作
if(WX_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的wxWidgets
    message(STATUS "🚀 使用缓存的 wxWidgets 库，跳过下载和编译")
    orcaslicer_copy_wx_private_headers()
    # wxWidgets: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(WX_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 wxWidgets...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${WX_BUILD_DIR} --target install --config ${WX_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${WX_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ wxWidgets 安装成功")
        orcaslicer_copy_wx_private_headers()
        # wxWidgets: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(WX_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ wxWidgets 安装失败")
    endif()

elseif(WX_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 wxWidgets...")

    # 先检查构建是否完整
    if(EXISTS "${WX_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${WX_BUILD_DIR} --target install --config ${WX_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${WX_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ wxWidgets 安装成功")
            orcaslicer_copy_wx_private_headers()
            # wxWidgets: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(WX_FOUND TRUE)
        else()
            message(WARNING "❌ wxWidgets 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${WX_BUILD_DIR}")
            set(WX_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${WX_BUILD_DIR}")
        set(WX_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(WX_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 wxWidgets...")

    # MSVC 上启用 WebView Edge
    if(MSVC)
        set(_wx_edge "ON")
    else()
        set(_wx_edge "OFF")
    endif()

    # 检测可用的生成器
    set(WX_GENERATOR "")
    set(WX_GENERATOR_PLATFORM "")
    set(WX_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(WX_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(WX_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${WX_GENERATOR}")
        if(WX_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${WX_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(WX_GENERATOR "Ninja")
            set(WX_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(WX_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(WX_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(WX_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${WX_GENERATOR}")
    if(WX_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${WX_GENERATOR_PLATFORM})
    endif()
    if(WX_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${WX_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(WX_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 配置 wxWidgets（依赖 PNG / ZLIB / EXPAT / JPEG，从 CMAKE_PREFIX_PATH 解析）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${WX_SOURCE_DIR}
            -B ${WX_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${WX_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${WX_INSTALL_DIR}
            "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
            -DZLIB_ROOT=${ZLIB_ROOT}
            -DwxBUILD_PRECOMP=ON
            -DCMAKE_DEBUG_POSTFIX=
            -DwxBUILD_DEBUG_LEVEL=0
            -DwxBUILD_SAMPLES=OFF
            -DwxBUILD_SHARED=OFF
            -DwxUSE_MEDIACTRL=ON
            -DwxUSE_DETECT_SM=OFF
            -DwxUSE_UNICODE=ON
            -DwxUSE_PRIVATE_FONTS=ON
            -DwxUSE_OPENGL=ON
            -DwxUSE_GLCANVAS_EGL=OFF
            -DwxUSE_WEBREQUEST=ON
            -DwxUSE_WEBVIEW=ON
            -DwxUSE_WEBVIEW_EDGE=${_wx_edge}
            -DwxUSE_WEBVIEW_IE=OFF
            -DwxUSE_REGEX=builtin
            -DwxUSE_LIBSDL=OFF
            -DwxUSE_XTEST=OFF
            -DwxUSE_STC=OFF
            -DwxUSE_AUI=ON
            -DwxUSE_LIBPNG=sys
            -DwxUSE_ZLIB=sys
            -DwxUSE_LIBJPEG=sys
            -DwxUSE_LIBTIFF=OFF
            -DwxUSE_LIBWEBP=builtin
            -DwxUSE_EXPAT=sys
            -DwxUSE_NANOSVG=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ wxWidgets 配置成功")

        # 构建wxWidgets
        message(STATUS "🔨 正在编译 wxWidgets (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${WX_BUILD_DIR} --config ${WX_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${WX_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${WX_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${WX_BUILD_DIR} --config ${WX_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${WX_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ wxWidgets 编译成功")

            # 安装wxWidgets
            message(STATUS "📦 正在安装 wxWidgets...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${WX_BUILD_DIR} --target install --config ${WX_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${WX_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ wxWidgets 安装成功")
                orcaslicer_copy_wx_private_headers()
                # wxWidgets: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(WX_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ wxWidgets 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ wxWidgets 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${WX_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "wxWidgets 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ wxWidgets 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "wxWidgets 配置失败，请查看上面的错误信息")
    endif()

elseif(WX_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 wxWidgets 源码...")

    # 确保源码目录不存在
    if(EXISTS "${WX_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${WX_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${WX_ZIP_FILE}
        WORKING_DIRECTORY ${WX_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # 检查解压后的目录名（可能是wxWidgets-9.3.1或其他）
        file(GLOB WX_EXTRACTED_DIRS "${WX_CACHE_DIR}/wxWidgets-*")
        list(GET WX_EXTRACTED_DIRS 0 WX_EXTRACTED_DIR)

        if(EXISTS "${WX_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${WX_EXTRACTED_DIR}" "${WX_SOURCE_DIR}")
            message(STATUS "✅ wxWidgets 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(WX_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到wxWidgets目录")
        endif()
    else()
        message(WARNING "❌ wxWidgets 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${WX_ZIP_FILE}")
        set(WX_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 wxWidgets ${WX_VERSION}...")

    # wxWidgets 使用 git clone（包含 submodules，--depth 1 浅克隆）
    set(WX_DOWNLOAD_URL "https://github.com/SoftFever/Orca-deps-wxWidgets")

    message(STATUS "⬇️  正在 git clone wxWidgets 源码...")
    message(STATUS "   URL: ${WX_DOWNLOAD_URL}")

    find_package(Git REQUIRED)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} clone --depth 1 --recurse-submodules --branch ${WX_VERSION}
                ${WX_DOWNLOAD_URL} ${WX_SOURCE_DIR}
        RESULT_VARIABLE clone_result
    )

    if(clone_result EQUAL 0)
        message(STATUS "✅ wxWidgets clone 成功")
        set(WX_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
        return()
    else()
        message(FATAL_ERROR "❌ wxWidgets clone 失败")
    endif()
endif()

# 注册到主项目的 find_package(wxWidgets)
if(WX_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${WX_INSTALL_DIR}")
    set(wxWidgets_ROOT_DIR "${WX_INSTALL_DIR}" CACHE PATH "wxWidgets root" FORCE)
    set(wxWidgets_AVAILABLE TRUE CACHE BOOL "wxWidgets library is available")
    message(STATUS "")
    message(STATUS "🎯 wxWidgets 库已就绪")
    message(STATUS "   安装目录: ${WX_INSTALL_DIR}")
else()
    set(wxWidgets_AVAILABLE FALSE CACHE BOOL "wxWidgets library is not available")
    message(FATAL_ERROR "⚠️  wxWidgets 库不可用")
endif()
