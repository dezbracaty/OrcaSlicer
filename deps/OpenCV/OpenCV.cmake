cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# OpenCV版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED OPENCV_VERSION)
    set(OPENCV_VERSION "4.6.0" CACHE STRING "OpenCV version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" OPENCV_VERSION_MAJOR "${OPENCV_VERSION}")

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
set(OPENCV_CACHE_DIR "${FETCH_CACHE_DIR}/opencv-v${OPENCV_VERSION}")

# OpenCV构建类型配置 - 可以独立于主项目设置
if(DEFINED OPENCV_BUILD_TYPE)
    # 如果明确指定了OPENCV_BUILD_TYPE，使用它
    set(OPENCV_ACTUAL_BUILD_TYPE "${OPENCV_BUILD_TYPE}")
    message(STATUS "   使用指定的OpenCV构建类型: ${OPENCV_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(OPENCV_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 OpenCV")
    else()
        set(OPENCV_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(OPENCV_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(OPENCV_BUILD_DIR "${OPENCV_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${OPENCV_ACTUAL_BUILD_TYPE}")

set(OPENCV_INSTALL_DIR "${OPENCV_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${OPENCV_ACTUAL_BUILD_TYPE}")
set(OPENCV_SOURCE_DIR "${OPENCV_CACHE_DIR}/src")
set(OPENCV_ZIP_FILE "${OPENCV_CACHE_DIR}/opencv-${OPENCV_VERSION}.tar.gz")

if(NOT ZLIB_AVAILABLE OR NOT ZLIB_ROOT)
    message(FATAL_ERROR
        "OpenCV requires ZLIB to be fetched first. "
        "Include deps/ZLIB/ZLIB.cmake before deps/OpenCV/OpenCV.cmake in fetch_deps.cmake.")
endif()

message(STATUS "")
message(STATUS "🔧 OpenCV 库缓存管理系统")
message(STATUS "   OpenCV版本: ${OPENCV_VERSION}")
message(STATUS "   OpenCV构建类型: ${OPENCV_ACTUAL_BUILD_TYPE}")
if(DEFINED OPENCV_BUILD_TYPE AND NOT OPENCV_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: OpenCV使用${OPENCV_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   OpenCV缓存目录: ${OPENCV_CACHE_DIR}")
message(STATUS "   构建目录: ${OPENCV_BUILD_DIR}")
message(STATUS "   安装目录: ${OPENCV_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${OPENCV_CACHE_DIR}")

# 检查OpenCV库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# OpenCV 安装后 OpenCVConfig.cmake 路径因平台/构建方式而异：
#   - Unix/macOS: lib/cmake/opencv4/OpenCVConfig.cmake
#   - Windows 静态库 install: x64/vc17/staticlib/OpenCVConfig.cmake
#   - 部分非标准布局: 直接落在 install 根目录
if(EXISTS "${OPENCV_INSTALL_DIR}/OpenCVConfig.cmake"
   OR EXISTS "${OPENCV_INSTALL_DIR}/lib/cmake/opencv4/OpenCVConfig.cmake"
   OR EXISTS "${OPENCV_INSTALL_DIR}/x64/vc17/staticlib/OpenCVConfig.cmake")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${OPENCV_INSTALL_DIR}")
endif()

if(EXISTS "${OPENCV_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _cv_built_libs
        "${OPENCV_BUILD_DIR}/lib/${OPENCV_ACTUAL_BUILD_TYPE}/opencv_world*"
        "${OPENCV_BUILD_DIR}/lib/libopencv_world*"
        "${OPENCV_BUILD_DIR}/lib/opencv_world*.lib")
    if(_cv_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${OPENCV_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${OPENCV_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${OPENCV_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${OPENCV_SOURCE_DIR}")
endif()

if(EXISTS "${OPENCV_ZIP_FILE}")
    set(_expected "1ec1cba65f9f20fe5a41fda1586e01c70ea0c9a6d7b67c9e13edf0cfe2239277")
    file(SHA256 "${OPENCV_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${OPENCV_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${OPENCV_ZIP_FILE}")
        file(REMOVE "${OPENCV_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(OPENCV_STATUS "NONE")
set(OPENCV_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(OPENCV_STATUS "INSTALLED")
    set(OPENCV_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的OpenCV库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(OPENCV_STATUS "BUILT_COMPLETE")
        set(OPENCV_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的OpenCV（无需安装）")
    else()
        # 构建未完成，需要继续
        set(OPENCV_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(OPENCV_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(OPENCV_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(OPENCV_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载OpenCV")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${OPENCV_STATUS}")

# 根据状态执行相应操作
if(OPENCV_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的OpenCV
    message(STATUS "🚀 使用缓存的 OpenCV 库，跳过下载和编译")
    # OpenCV: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析

elseif(OPENCV_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 OpenCV...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${OPENCV_BUILD_DIR} --target install --config ${OPENCV_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${OPENCV_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ OpenCV 安装成功")
        # OpenCV: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
        set(OPENCV_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ OpenCV 安装失败")
    endif()

elseif(OPENCV_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 OpenCV...")

    # 先检查构建是否完整
    if(EXISTS "${OPENCV_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${OPENCV_BUILD_DIR} --target install --config ${OPENCV_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${OPENCV_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ OpenCV 安装成功")
            # OpenCV: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
            set(OPENCV_FOUND TRUE)
        else()
            message(WARNING "❌ OpenCV 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${OPENCV_BUILD_DIR}")
            set(OPENCV_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${OPENCV_BUILD_DIR}")
        set(OPENCV_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(OPENCV_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 OpenCV...")

    # 在 Windows 上启用 Intel IPP 加速
    if(MSVC)
        set(_opencv_use_ipp ON)
    else()
        set(_opencv_use_ipp OFF)
    endif()

    # 应用 patch（按 patch 集版本递增；旧 marker 仍可向前兼容）
    # v1: 0001-vs.patch
    # v2: 加入 0002-clang19-macos.patch（仅 APPLE，修内置 libpng pngpriv.h 误入
    #     TARGET_OS_MAC 分支 include 已不存在的 <fp.h> 的历史 bug）
    set(_opencv_patch_mark_v1 "${OPENCV_SOURCE_DIR}/.orca_patched")
    set(_opencv_patch_mark_v2 "${OPENCV_SOURCE_DIR}/.orca_patched_v2")
    if(NOT EXISTS "${_opencv_patch_mark_v2}")
        find_package(Git REQUIRED)

        # 仅在没有任何 marker 时才需要初始化 git 仓库 + apply 0001
        if(NOT EXISTS "${_opencv_patch_mark_v1}")
            message(STATUS "🩹 应用 OpenCV patch 0001-vs...")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} init -q
                WORKING_DIRECTORY ${OPENCV_SOURCE_DIR}
                RESULT_VARIABLE _git_init
            )
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply --verbose --ignore-space-change --whitespace=fix
                    "${CMAKE_CURRENT_LIST_DIR}/0001-vs.patch"
                WORKING_DIRECTORY ${OPENCV_SOURCE_DIR}
                RESULT_VARIABLE _patch_result
            )
            if(NOT _patch_result EQUAL 0)
                message(FATAL_ERROR "❌ OpenCV 0001-vs.patch 应用失败")
            endif()
            file(WRITE "${_opencv_patch_mark_v1}" "patched\n")
        endif()

        # v2 增量：仅 APPLE 需要
        if(APPLE)
            message(STATUS "🩹 应用 OpenCV patch 0002-clang19-macos...")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} apply --verbose --ignore-space-change --whitespace=fix
                    "${CMAKE_CURRENT_LIST_DIR}/0002-clang19-macos.patch"
                WORKING_DIRECTORY ${OPENCV_SOURCE_DIR}
                RESULT_VARIABLE _patch_result
            )
            if(NOT _patch_result EQUAL 0)
                message(FATAL_ERROR "❌ OpenCV 0002-clang19-macos.patch 应用失败")
            endif()
        endif()

        file(WRITE "${_opencv_patch_mark_v2}" "patched\n")
        message(STATUS "✅ OpenCV patch 应用成功")
    endif()

    # 检测可用的生成器
    set(OPENCV_GENERATOR "")
    set(OPENCV_GENERATOR_PLATFORM "")
    set(OPENCV_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(OPENCV_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(OPENCV_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${OPENCV_GENERATOR}")
        if(OPENCV_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${OPENCV_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(OPENCV_GENERATOR "Ninja")
            set(OPENCV_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(OPENCV_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(OPENCV_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(OPENCV_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${OPENCV_GENERATOR}")
    if(OPENCV_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${OPENCV_GENERATOR_PLATFORM})
    endif()
    if(OPENCV_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${OPENCV_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(OPENCV_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 必须显式向 OpenCV 子配置传 CMAKE_SYSTEM_PROCESSOR：
    # OpenCV 用 CMAKE_SYSTEM_PROCESSOR 正则匹配设置 X86_64，进而决定
    # 3rdparty/ippicv/ippicv.cmake 下载 intel64 还是 ia32 IPP 包。
    # IDE/Ninja 子配置下该变量可能为空，会默认下到 ia32（x86）的 ippicvmt.lib，
    # 装到 x64 install 目录里和 x64 opencv_world460.lib 混在一起，链接 x64
    # 主工程时 ippicv* 全是 LNK2001。
    if(CMAKE_HOST_SYSTEM_PROCESSOR)
        list(APPEND GENERATOR_ARGS -DCMAKE_SYSTEM_PROCESSOR=${CMAKE_HOST_SYSTEM_PROCESSOR})
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND WIN32)
        list(APPEND GENERATOR_ARGS -DCMAKE_SYSTEM_PROCESSOR=AMD64)
    endif()

    # 配置 OpenCV（仅启用 core/imgcodecs/imgproc/world，关掉所有可选功能）
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${OPENCV_SOURCE_DIR}
            -B ${OPENCV_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${OPENCV_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${OPENCV_INSTALL_DIR}
            -DBUILD_SHARED_LIBS=0
            -DBUILD_PERE_TESTS=OFF
            -DBUILD_TESTS=OFF
            -DBUILD_opencv_python_tests=OFF
            -DBUILD_EXAMPLES=OFF
            -DBUILD_JASPER=OFF
            -DBUILD_JAVA=OFF
            -DBUILD_JPEG=ON
            -DBUILD_APPS_LIST=version
            -DBUILD_opencv_apps=OFF
            -DBUILD_opencv_java=OFF
            -DBUILD_OPENEXR=OFF
            -DBUILD_PNG=ON
            -DBUILD_TBB=OFF
            -DBUILD_WEBP=OFF
            -DBUILD_ZLIB=OFF
            -DWITH_1394=OFF
            -DWITH_CUDA=OFF
            -DWITH_EIGEN=OFF
            -DWITH_IPP=${_opencv_use_ipp}
            -DWITH_ITT=OFF
            -DWITH_FFMPEG=OFF
            -DWITH_GPHOTO2=OFF
            -DWITH_GSTREAMER=OFF
            -DOPENCV_GAPI_GSTREAMER=OFF
            -DWITH_GTK_2_X=OFF
            -DWITH_JASPER=OFF
            -DWITH_LAPACK=OFF
            -DWITH_MATLAB=OFF
            -DWITH_MFX=OFF
            -DWITH_DIRECTX=OFF
            -DWITH_DIRECTML=OFF
            -DWITH_OPENCL=OFF
            -DWITH_OPENCL_D3D11_NV=OFF
            -DWITH_OPENCLAMDBLAS=OFF
            -DWITH_OPENCLAMDFFT=OFF
            -DWITH_OPENEXR=OFF
            -DWITH_OPENJPEG=OFF
            -DWITH_QUIRC=OFF
            -DWITH_OpenCV=OFF
            -DWITH_JPEG=OFF
            -DWITH_WEBP=OFF
            -DENABLE_PRECOMPILED_HEADERS=OFF
            -DINSTALL_TESTS=OFF
            -DINSTALL_C_EXAMPLES=OFF
            -DINSTALL_PYTHON_EXAMPLES=OFF
            -DOPENCV_GENERATE_SETUPVARS=OFF
            -DOPENCV_INSTALL_FFMPEG_DOWNLOAD_SCRIPT=OFF
            -DBUILD_opencv_python2=OFF
            -DBUILD_opencv_python3=OFF
            -DWITH_OPENVINO=OFF
            -DWITH_INF_ENGINE=OFF
            -DWITH_NGRAPH=OFF
            -DBUILD_WITH_STATIC_CRT=OFF
            -DBUILD_LIST=core,imgcodecs,imgproc,world
            -DBUILD_opencv_highgui=OFF
            -DWITH_ADE=OFF
            -DBUILD_opencv_world=ON
            -DWITH_PROTOBUF=OFF
            -DWITH_WIN32UI=OFF
            -DHAVE_WIN32UI=FALSE
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ OpenCV 配置成功")

        # 构建OpenCV
        message(STATUS "🔨 正在编译 OpenCV (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${OPENCV_BUILD_DIR} --config ${OPENCV_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${OPENCV_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${OPENCV_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${OPENCV_BUILD_DIR} --config ${OPENCV_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${OPENCV_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ OpenCV 编译成功")

            # 安装OpenCV
            message(STATUS "📦 正在安装 OpenCV...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${OPENCV_BUILD_DIR} --target install --config ${OPENCV_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${OPENCV_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ OpenCV 安装成功")
                # OpenCV: 不在此处 find_package，由主项目通过 CMAKE_PREFIX_PATH 解析
                set(OPENCV_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ OpenCV 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ OpenCV 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${OPENCV_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "OpenCV 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ OpenCV 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "OpenCV 配置失败，请查看上面的错误信息")
    endif()

elseif(OPENCV_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 OpenCV 源码...")

    # 确保源码目录不存在
    if(EXISTS "${OPENCV_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${OPENCV_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${OPENCV_ZIP_FILE}
        WORKING_DIRECTORY ${OPENCV_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # OpenCV 解压后目录名形如 opencv-4.6.0
        file(GLOB OPENCV_EXTRACTED_DIRS "${OPENCV_CACHE_DIR}/opencv-*")
        list(FILTER OPENCV_EXTRACTED_DIRS EXCLUDE REGEX "\\.tar\\.gz$")
        list(GET OPENCV_EXTRACTED_DIRS 0 OPENCV_EXTRACTED_DIR)

        if(EXISTS "${OPENCV_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${OPENCV_EXTRACTED_DIR}" "${OPENCV_SOURCE_DIR}")
            message(STATUS "✅ OpenCV 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(OPENCV_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到OpenCV目录")
        endif()
    else()
        message(WARNING "❌ OpenCV 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${OPENCV_ZIP_FILE}")
        set(OPENCV_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 OpenCV ${OPENCV_VERSION}...")

    # OpenCV GitHub release tarball
    set(OPENCV_DOWNLOAD_URL "https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz")

    message(STATUS "📥 正在下载 OpenCV...")
    message(STATUS "   URL: ${OPENCV_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${OPENCV_DOWNLOAD_URL}
        ${OPENCV_ZIP_FILE}
        EXPECTED_HASH SHA256=1ec1cba65f9f20fe5a41fda1586e01c70ea0c9a6d7b67c9e13edf0cfe2239277
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ OpenCV 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ OpenCV 下载成功")

    # 解压
    message(STATUS "📦 解压 OpenCV 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${OPENCV_ZIP_FILE}
        WORKING_DIRECTORY ${OPENCV_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ OpenCV 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(OPENCV_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ OpenCV 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(OpenCV)
if(OPENCV_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${OPENCV_INSTALL_DIR}")
    set(OpenCV_DIR "${OPENCV_INSTALL_DIR}" CACHE PATH "OpenCV root" FORCE)
    # 上面 BUILD_SHARED_LIBS=0 编出来的是静态库；告诉根 OpenCVConfig.cmake 走
    # staticlib 子目录，否则它跳过 ippicv 等 imported target 的定义，导致链接时
    # 出现 LNK2001 unresolved external symbol ippicv*。
    set(OpenCV_STATIC ON CACHE BOOL "OpenCV is built as static libraries" FORCE)
    set(OpenCV_AVAILABLE TRUE CACHE BOOL "OpenCV library is available")
    message(STATUS "")
    message(STATUS "🎯 OpenCV 库已就绪")
    message(STATUS "   版本: ${OPENCV_VERSION}")
    message(STATUS "   安装目录: ${OPENCV_INSTALL_DIR}")
else()
    set(OpenCV_AVAILABLE FALSE CACHE BOOL "OpenCV library is not available")
    message(FATAL_ERROR "⚠️  OpenCV 库不可用")
endif()