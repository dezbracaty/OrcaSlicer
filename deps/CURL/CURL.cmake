cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# CURL版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED CURL_VERSION)
    set(CURL_VERSION "7.75.0" CACHE STRING "CURL version to build")
endif()
# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" CURL_VERSION_MAJOR "${CURL_VERSION}")

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
set(CURL_CACHE_DIR "${FETCH_CACHE_DIR}/curl-v${CURL_VERSION}")

# CURL构建类型配置 - 可以独立于主项目设置
if(DEFINED CURL_BUILD_TYPE)
    # 如果明确指定了CURL_BUILD_TYPE，使用它
    set(CURL_ACTUAL_BUILD_TYPE "${CURL_BUILD_TYPE}")
    message(STATUS "   使用指定的CURL构建类型: ${CURL_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(CURL_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 CURL")
    else()
        set(CURL_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(CURL_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(CURL_BUILD_DIR "${CURL_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${CURL_ACTUAL_BUILD_TYPE}")

set(CURL_INSTALL_DIR "${CURL_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${CURL_ACTUAL_BUILD_TYPE}")
set(CURL_SOURCE_DIR "${CURL_CACHE_DIR}/src")
set(CURL_ZIP_FILE "${CURL_CACHE_DIR}/curl-${CURL_VERSION}.zip")

message(STATUS "")
message(STATUS "🔧 CURL 库缓存管理系统")
message(STATUS "   CURL版本: ${CURL_VERSION}")
message(STATUS "   CURL构建类型: ${CURL_ACTUAL_BUILD_TYPE}")
if(DEFINED CURL_BUILD_TYPE AND NOT CURL_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: CURL使用${CURL_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   CURL缓存目录: ${CURL_CACHE_DIR}")
message(STATUS "   构建目录: ${CURL_BUILD_DIR}")
message(STATUS "   安装目录: ${CURL_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${CURL_CACHE_DIR}")

# 检查CURL库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# CURL 安装后会有 include/curl/curl.h
if(EXISTS "${CURL_INSTALL_DIR}/include/curl/curl.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${CURL_INSTALL_DIR}")
endif()

# CURL 的构建目录：有 CMakeCache.txt 即认为已配置；
# 真正"完成"以是否产出 lib（lib/libcurl*.lib 或 lib/libcurl.a）判断
if(EXISTS "${CURL_BUILD_DIR}/CMakeCache.txt")
    set(HAS_BUILD TRUE)
    file(GLOB _curl_built_libs "${CURL_BUILD_DIR}/lib/libcurl*" "${CURL_BUILD_DIR}/lib/${CURL_ACTUAL_BUILD_TYPE}/libcurl*")
    if(_curl_built_libs)
        set(BUILD_COMPLETE TRUE)
        message(STATUS "✅ 发现已完成的构建目录: ${CURL_BUILD_DIR}")
    else()
        set(BUILD_COMPLETE FALSE)
        message(STATUS "📁 发现构建目录（可能未完成）: ${CURL_BUILD_DIR}")
    endif()
endif()

if(EXISTS "${CURL_SOURCE_DIR}/CMakeLists.txt")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${CURL_SOURCE_DIR}")
endif()

if(EXISTS "${CURL_ZIP_FILE}")
    set(_expected "a63ae025bb0a14f119e73250f2c923f4bf89aa93b8d4fafa4a9f5353a96a765a")
    file(SHA256 "${CURL_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${CURL_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${CURL_ZIP_FILE}")
        file(REMOVE "${CURL_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(CURL_STATUS "NONE")
set(CURL_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(CURL_STATUS "INSTALLED")
    set(CURL_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的CURL库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(CURL_STATUS "BUILT_COMPLETE")
        set(CURL_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的CURL（无需安装）")
    else()
        # 构建未完成，需要继续
        set(CURL_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(CURL_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(CURL_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(CURL_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载CURL")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${CURL_STATUS}")

# 根据状态执行相应操作
if(CURL_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的CURL
    message(STATUS "🚀 使用缓存的 CURL 库，跳过下载和编译")
    # CURL: 不在此处 find_package，由主项目的 FindCURL 通过 CMAKE_PREFIX_PATH 解析

elseif(CURL_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行安装步骤
    message(STATUS "📦 开始安装 CURL...")

    # 执行安装
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${CURL_BUILD_DIR} --target install --config ${CURL_ACTUAL_BUILD_TYPE}
        RESULT_VARIABLE install_result
        WORKING_DIRECTORY ${CURL_BUILD_DIR}
    )

    if(install_result EQUAL 0)
        message(STATUS "✅ CURL 安装成功")
        # CURL: 不在此处 find_package，由主项目的 FindCURL 通过 CMAKE_PREFIX_PATH 解析
        set(CURL_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ CURL 安装失败")
    endif()

elseif(CURL_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 执行安装
    message(STATUS "📦 开始安装 CURL...")

    # 先检查构建是否完整
    if(EXISTS "${CURL_BUILD_DIR}/lib")
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${CURL_BUILD_DIR} --target install --config ${CURL_ACTUAL_BUILD_TYPE}
            RESULT_VARIABLE install_result
            WORKING_DIRECTORY ${CURL_BUILD_DIR}
        )

        if(install_result EQUAL 0)
            message(STATUS "✅ CURL 安装成功")
            # CURL: 不在此处 find_package，由主项目的 FindCURL 通过 CMAKE_PREFIX_PATH 解析
            set(CURL_FOUND TRUE)
        else()
            message(WARNING "❌ CURL 安装失败，尝试重新构建...")
            # 删除不完整的构建并重新构建
            file(REMOVE_RECURSE "${CURL_BUILD_DIR}")
            set(CURL_STATUS "SOURCE_ONLY")
            include(${CMAKE_CURRENT_LIST_FILE})
        endif()
    else()
        message(WARNING "⚠️  构建目录不完整，重新构建...")
        file(REMOVE_RECURSE "${CURL_BUILD_DIR}")
        set(CURL_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

elseif(CURL_STATUS STREQUAL "SOURCE_ONLY")
    # 需要构建和安装
    message(STATUS "🔨 开始构建 CURL...")

    # 检测可用的生成器
    set(CURL_GENERATOR "")
    set(CURL_GENERATOR_PLATFORM "")
    set(CURL_MAKE_PROGRAM "")

    # 在 Windows 上，优先使用与主项目相同的生成器
    if(WIN32 AND CMAKE_GENERATOR MATCHES "Visual Studio")
        set(CURL_GENERATOR "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            set(CURL_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        message(STATUS "⚡ 使用项目生成器: ${CURL_GENERATOR}")
        if(CURL_GENERATOR_PLATFORM)
            message(STATUS "   平台: ${CURL_GENERATOR_PLATFORM}")
        endif()
    else()
        # 在非 Windows 平台上，优先使用 Ninja
        find_program(NINJA_EXECUTABLE ninja)
        if(NINJA_EXECUTABLE)
            set(CURL_GENERATOR "Ninja")
            set(CURL_MAKE_PROGRAM ${NINJA_EXECUTABLE})
            message(STATUS "🚀 使用 Ninja 生成器进行快速编译")
            message(STATUS "   Ninja路径: ${NINJA_EXECUTABLE}")
        else()
            # 如果没有 Ninja，使用与主项目相同的生成器
            if(CMAKE_GENERATOR)
                set(CURL_GENERATOR "${CMAKE_GENERATOR}")
                message(STATUS "⚡ 使用项目生成器: ${CMAKE_GENERATOR}")
            else()
                # 默认使用 Unix Makefiles
                set(CURL_GENERATOR "Unix Makefiles")
                message(STATUS "🔧 使用默认生成器: Unix Makefiles")
            endif()

            if(CMAKE_MAKE_PROGRAM)
                set(CURL_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
            endif()
        endif()
    endif()

    # 准备生成器参数 - 注意：-G 和生成器名称必须分开
    set(GENERATOR_ARGS -G "${CURL_GENERATOR}")
    if(CURL_GENERATOR_PLATFORM)
        list(APPEND GENERATOR_ARGS -A ${CURL_GENERATOR_PLATFORM})
    endif()
    if(CURL_MAKE_PROGRAM)
        list(APPEND GENERATOR_ARGS -DCMAKE_MAKE_PROGRAM=${CURL_MAKE_PROGRAM})
    endif()

    # 使用 Ninja 时需要显式指定编译器路径
    if(CURL_GENERATOR STREQUAL "Ninja")
        if(CMAKE_C_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER})
        endif()
        if(CMAKE_CXX_COMPILER)
            list(APPEND GENERATOR_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
        endif()
    endif()

    # 平台相关 CURL flags（参考原 deps/CURL/CURL.cmake）
    set(_curl_platform_flags
        -DENABLE_IPV6:BOOL=ON
        -DENABLE_VERSIONED_SYMBOLS:BOOL=ON
        -DENABLE_THREADED_RESOLVER:BOOL=ON
        -DENABLE_MANUAL:BOOL=OFF
        -DCURL_DISABLE_LDAP:BOOL=ON
        -DCURL_DISABLE_LDAPS:BOOL=ON
        -DCURL_DISABLE_RTSP:BOOL=ON
        -DCURL_DISABLE_DICT:BOOL=ON
        -DCURL_DISABLE_TELNET:BOOL=ON
        -DCURL_DISABLE_POP3:BOOL=ON
        -DCURL_DISABLE_IMAP:BOOL=ON
        -DCURL_DISABLE_SMB:BOOL=ON
        -DCURL_DISABLE_SMTP:BOOL=ON
        -DCURL_DISABLE_GOPHER:BOOL=ON
        -DCURL_DISABLE_TFTP:BOOL=ON
        -DCURL_DISABLE_MQTT:BOOL=ON
        -DCMAKE_USE_GSSAPI:BOOL=OFF
        -DCMAKE_USE_LIBSSH2:BOOL=OFF
        -DUSE_RTMP:BOOL=OFF
        -DUSE_NGHTTP2:BOOL=OFF
        -DUSE_MBEDTLS:BOOL=OFF
        -DCMAKE_USE_OPENSSL:BOOL=ON
        -DCURL_CA_PATH:STRING=none
    )
    if(NOT WIN32 AND NOT APPLE)
        list(APPEND _curl_platform_flags
            -DCURL_CA_BUNDLE:STRING=none
            -DCURL_CA_FALLBACK:BOOL=ON
        )
    endif()

    # 主项目编译为静态时 curl 也走静态
    if(BUILD_SHARED_LIBS)
        set(_curl_static OFF)
        set(_curl_shared ON)
    else()
        set(_curl_static ON)
        set(_curl_shared OFF)
    endif()

    # 配置CURL
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${CURL_SOURCE_DIR}
            -B ${CURL_BUILD_DIR}
            ${GENERATOR_ARGS}
            ${FETCH_DEPS_FORWARD_ARGS}
            -DCMAKE_BUILD_TYPE=${CURL_ACTUAL_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX=${CURL_INSTALL_DIR}
            -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
            -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR}
            -DBUILD_TESTING:BOOL=OFF
            -DBUILD_CURL_EXE:BOOL=OFF
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCURL_STATICLIB=${_curl_static}
            -DBUILD_SHARED_LIBS=${_curl_shared}
            ${_curl_platform_flags}
        RESULT_VARIABLE config_result
        OUTPUT_VARIABLE config_output
        ERROR_VARIABLE config_error
    )

    if(config_result EQUAL 0)
        message(STATUS "✅ CURL 配置成功")

        # 构建CURL
        message(STATUS "🔨 正在编译 CURL (这可能需要较长时间)...")

        # 获取可用的处理器数量
        include(ProcessorCount)
        ProcessorCount(N_CORES)
        if(N_CORES EQUAL 0)
            set(N_CORES 4)
        endif()
        message(STATUS "   使用 ${N_CORES} 个并行任务")

        # 先输出编译命令以便调试
        message(STATUS "   编译命令: ${CMAKE_COMMAND} --build ${CURL_BUILD_DIR} --config ${CURL_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}")

        # 先检查构建目录是否存在
        if(NOT EXISTS "${CURL_BUILD_DIR}")
            message(FATAL_ERROR "构建目录不存在: ${CURL_BUILD_DIR}")
        endif()

        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${CURL_BUILD_DIR} --config ${CURL_ACTUAL_BUILD_TYPE} --parallel ${N_CORES}
            RESULT_VARIABLE build_result
            WORKING_DIRECTORY ${CURL_BUILD_DIR}
        )

        if(build_result EQUAL 0)
            message(STATUS "✅ CURL 编译成功")

            # 安装CURL
            message(STATUS "📦 正在安装 CURL...")
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${CURL_BUILD_DIR} --target install --config ${CURL_ACTUAL_BUILD_TYPE}
                RESULT_VARIABLE install_result
                WORKING_DIRECTORY ${CURL_BUILD_DIR}
            )

            if(install_result EQUAL 0)
                message(STATUS "✅ CURL 安装成功")
                # CURL: 不在此处 find_package，由主项目的 FindCURL 通过 CMAKE_PREFIX_PATH 解析
                set(CURL_FOUND TRUE)
            else()
                message(FATAL_ERROR "❌ CURL 安装失败")
            endif()
        else()
            # 输出详细错误信息
            message(STATUS "❌ CURL 编译失败，返回码: ${build_result}")

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
            file(GLOB error_logs "${CURL_BUILD_DIR}/CMakeFiles/*.log")
            if(error_logs)
                message(STATUS "找到以下日志文件:")
                foreach(log ${error_logs})
                    message(STATUS "  ${log}")
                endforeach()
            endif()

            message(FATAL_ERROR "CURL 编译失败，请查看上面的错误信息")
        endif()
    else()
        message(STATUS "❌ CURL 配置失败，返回码: ${config_result}")
        if(config_output)
            message(STATUS "配置输出:")
            message(STATUS "${config_output}")
        endif()
        if(config_error)
            message(STATUS "错误信息:")
            message(STATUS "${config_error}")
        endif()
        message(FATAL_ERROR "CURL 配置失败，请查看上面的错误信息")
    endif()

elseif(CURL_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 CURL 源码...")

    # 确保源码目录不存在
    if(EXISTS "${CURL_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${CURL_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${CURL_ZIP_FILE}
        WORKING_DIRECTORY ${CURL_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # CURL 的 GitHub archive 解压后目录名形如 curl-curl-7_75_0
        file(GLOB CURL_EXTRACTED_DIRS "${CURL_CACHE_DIR}/curl-*")
        list(FILTER CURL_EXTRACTED_DIRS EXCLUDE REGEX "\\.zip$")
        list(GET CURL_EXTRACTED_DIRS 0 CURL_EXTRACTED_DIR)

        if(EXISTS "${CURL_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${CURL_EXTRACTED_DIR}" "${CURL_SOURCE_DIR}")
            message(STATUS "✅ CURL 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(CURL_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到CURL目录")
        endif()
    else()
        message(WARNING "❌ CURL 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${CURL_ZIP_FILE}")
        set(CURL_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 CURL ${CURL_VERSION}...")

    # CURL GitHub archive
    string(REPLACE "." "_" CURL_TAG_SUFFIX "${CURL_VERSION}")
    set(CURL_DOWNLOAD_URL "https://github.com/curl/curl/archive/refs/tags/curl-${CURL_TAG_SUFFIX}.zip")

    message(STATUS "📥 正在下载 CURL...")
    message(STATUS "   URL: ${CURL_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${CURL_DOWNLOAD_URL}
        ${CURL_ZIP_FILE}
        EXPECTED_HASH SHA256=a63ae025bb0a14f119e73250f2c923f4bf89aa93b8d4fafa4a9f5353a96a765a
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ CURL 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ CURL 下载成功")

    # 解压
    message(STATUS "📦 解压 CURL 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xf ${CURL_ZIP_FILE}
        WORKING_DIRECTORY ${CURL_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ CURL 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(CURL_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ CURL 解压失败")
    endif()
endif()

# 注册到主项目的 find_package(CURL)
if(CURL_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${CURL_INSTALL_DIR}")
    set(CURL_ROOT "${CURL_INSTALL_DIR}" CACHE PATH "CURL root" FORCE)
    set(CURL_AVAILABLE TRUE CACHE BOOL "CURL library is available")
    message(STATUS "")
    message(STATUS "🎯 CURL 库已就绪")
    message(STATUS "   版本: ${CURL_VERSION}")
    message(STATUS "   安装目录: ${CURL_INSTALL_DIR}")
else()
    set(CURL_AVAILABLE FALSE CACHE BOOL "CURL library is not available")
    message(FATAL_ERROR "⚠️  CURL 库不可用")
endif()
