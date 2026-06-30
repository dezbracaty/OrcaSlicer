cmake_minimum_required(VERSION 3.16)
include(FetchContent)

# OpenSSL版本配置 - 可以通过CMAKE选项覆盖
if(NOT DEFINED OPENSSL_VERSION)
    set(OPENSSL_VERSION "1.1.1w" CACHE STRING "OpenSSL version to build")
endif()

# 自动计算主版本号
string(REGEX MATCH "^[0-9]+\\.[0-9]+" OPENSSL_VERSION_MAJOR "${OPENSSL_VERSION}")

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
set(OPENSSL_CACHE_DIR "${FETCH_CACHE_DIR}/openssl-v${OPENSSL_VERSION}")

# OpenSSL构建类型配置 - 可以独立于主项目设置
if(DEFINED OPENSSL_BUILD_TYPE)
    # 如果明确指定了OPENSSL_BUILD_TYPE，使用它
    set(OPENSSL_ACTUAL_BUILD_TYPE "${OPENSSL_BUILD_TYPE}")
    message(STATUS "   使用指定的OpenSSL构建类型: ${OPENSSL_ACTUAL_BUILD_TYPE}")
elseif(CMAKE_BUILD_TYPE)
    # 否则使用主项目的构建类型
    # 如果是 Visual Studio 多配置生成器，只使用 Debug
    if(CMAKE_GENERATOR MATCHES "Visual Studio" AND CMAKE_BUILD_TYPE MATCHES ";")
        set(OPENSSL_ACTUAL_BUILD_TYPE "Debug")
        message(STATUS "   检测到 Visual Studio 多配置生成器，使用 Debug 构建 OpenSSL")
    else()
        set(OPENSSL_ACTUAL_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
    endif()
else()
    # 默认Debug
    set(OPENSSL_ACTUAL_BUILD_TYPE "Debug")
    message(STATUS "   未指定构建类型，默认使用 Debug")
endif()

set(OPENSSL_BUILD_DIR "${OPENSSL_CACHE_DIR}/${FETCH_CACHE_ARCH}/build-${OPENSSL_ACTUAL_BUILD_TYPE}")

set(OPENSSL_INSTALL_DIR "${OPENSSL_CACHE_DIR}/${FETCH_CACHE_ARCH}/install-${OPENSSL_ACTUAL_BUILD_TYPE}")
set(OPENSSL_SOURCE_DIR "${OPENSSL_CACHE_DIR}/src")
string(REPLACE "." "_" OPENSSL_TAG_SUFFIX "${OPENSSL_VERSION}")
set(OPENSSL_GIT_TAG "OpenSSL_${OPENSSL_TAG_SUFFIX}")
set(OPENSSL_ZIP_FILE "${OPENSSL_CACHE_DIR}/openssl-${OPENSSL_GIT_TAG}.tar.gz")

message(STATUS "")
message(STATUS "🔧 OpenSSL 库缓存管理系统")
message(STATUS "   OpenSSL版本: ${OPENSSL_VERSION}")
message(STATUS "   OpenSSL构建类型: ${OPENSSL_ACTUAL_BUILD_TYPE}")
if(DEFINED OPENSSL_BUILD_TYPE AND NOT OPENSSL_BUILD_TYPE STREQUAL CMAKE_BUILD_TYPE)
    message(STATUS "   ⚠️  注意: OpenSSL使用${OPENSSL_ACTUAL_BUILD_TYPE}，主项目使用${CMAKE_BUILD_TYPE}")
endif()

message(STATUS "   缓存根目录: ${FETCH_CACHE_DIR}")
message(STATUS "   OpenSSL缓存目录: ${OPENSSL_CACHE_DIR}")
message(STATUS "   源码目录(in-source build): ${OPENSSL_SOURCE_DIR}")
message(STATUS "   安装目录: ${OPENSSL_INSTALL_DIR}")
message(STATUS "")

# 创建必要的目录
file(MAKE_DIRECTORY "${FETCH_CACHE_DIR}")
file(MAKE_DIRECTORY "${OPENSSL_CACHE_DIR}")

# 检查OpenSSL库的状态 - 分别检查各个组件的存在性
set(HAS_INSTALL FALSE)
set(HAS_BUILD FALSE)
set(HAS_SOURCE FALSE)
set(HAS_ZIP FALSE)

# 检查各个组件
# OpenSSL: 安装后会有 include/openssl/opensslv.h
if(EXISTS "${OPENSSL_INSTALL_DIR}/include/openssl/opensslv.h")
    set(HAS_INSTALL TRUE)
    message(STATUS "✅ 发现安装目录: ${OPENSSL_INSTALL_DIR}")
endif()

# OpenSSL 是 in-source build：所有 arch 共享同一个 src 目录，构建产物（.o/.a）
# 直接落在 src 里。标记文件必须按 arch 区分，否则前一次 arm64 build 留下的
# .orca_built-Release 会让本次 x86_64 build 误判"已构建"，跳过实际重编 →
# install dir 拿不到本 arch 的 .a → 主项目 find_package 回落到系统 ssl。
if(EXISTS "${OPENSSL_SOURCE_DIR}/.orca_built-${OPENSSL_ACTUAL_BUILD_TYPE}-${FETCH_CACHE_ARCH}")
    set(HAS_BUILD TRUE)
    set(BUILD_COMPLETE TRUE)
    message(STATUS "✅ 发现已完成的构建 (${FETCH_CACHE_ARCH}): ${OPENSSL_SOURCE_DIR}")
elseif(EXISTS "${OPENSSL_SOURCE_DIR}/.orca_configured-${OPENSSL_ACTUAL_BUILD_TYPE}-${FETCH_CACHE_ARCH}")
    set(HAS_BUILD TRUE)
    set(BUILD_COMPLETE FALSE)
    message(STATUS "📁 发现已配置但未完成的构建 (${FETCH_CACHE_ARCH}): ${OPENSSL_SOURCE_DIR}")
endif()

if(EXISTS "${OPENSSL_SOURCE_DIR}/Configure")
    set(HAS_SOURCE TRUE)
    message(STATUS "📄 发现源码目录: ${OPENSSL_SOURCE_DIR}")
endif()

if(EXISTS "${OPENSSL_ZIP_FILE}")
    set(_expected "2130e8c2fb3b79d1086186f78e59e8bc8d1a6aedf17ab3907f4cb9ae20918c41")
    file(SHA256 "${OPENSSL_ZIP_FILE}" _actual)
    string(TOLOWER "${_actual}" _actual)
    if(_actual STREQUAL _expected)
        set(HAS_ZIP TRUE)
        message(STATUS "📦 发现压缩包: ${OPENSSL_ZIP_FILE}")
    else()
        message(STATUS "⚠️  压缩包校验失败（已损坏），删除重新下载: ${OPENSSL_ZIP_FILE}")
        file(REMOVE "${OPENSSL_ZIP_FILE}")
    endif()
endif()

# 决定操作策略
set(OPENSSL_STATUS "NONE")
set(OPENSSL_FOUND FALSE)

# 优先级判断：install > build > source > zip > none
if(HAS_INSTALL)
    # 有install目录，直接使用
    set(OPENSSL_STATUS "INSTALLED")
    set(OPENSSL_FOUND TRUE)
    message(STATUS "🚀 将使用已安装的OpenSSL库")

elseif(HAS_BUILD AND NOT HAS_INSTALL)
    # 检查构建是否完成
    if(BUILD_COMPLETE)
        # 构建已完成，可以直接使用
        set(OPENSSL_STATUS "BUILT_COMPLETE")
        set(OPENSSL_FOUND TRUE)
        message(STATUS "✅ 将直接使用已编译的OpenSSL（无需安装）")
    else()
        # 构建未完成，需要继续
        set(OPENSSL_STATUS "BUILT_NOT_INSTALLED")
        message(STATUS "⚠️  已构建但未完成，将继续编译")
    endif()

elseif(HAS_SOURCE AND NOT HAS_BUILD)
    # 有源码但没有build，需要构建
    set(OPENSSL_STATUS "SOURCE_ONLY")
    message(STATUS "🔨 有源码无构建，将进行构建和安装")

elseif(HAS_ZIP AND NOT HAS_SOURCE)
    # 只有zip，需要解压
    set(OPENSSL_STATUS "ZIP_ONLY")
    message(STATUS "📦 只有压缩包，将解压并构建")

else()
    # 什么都没有，需要下载
    set(OPENSSL_STATUS "NONE")
    message(STATUS "⬇️  无缓存文件，将下载OpenSSL")
endif()

message(STATUS "📊 状态汇总: Install=${HAS_INSTALL}, Build=${HAS_BUILD}, Source=${HAS_SOURCE}, Zip=${HAS_ZIP}")
message(STATUS "🎯 执行策略: ${OPENSSL_STATUS}")

# OpenSSL 平台相关工具：Windows 用 perl + nmake，Unix 用 make
include(ProcessorCount)
ProcessorCount(N_CORES)
if(N_CORES EQUAL 0)
    set(N_CORES 4)
endif()

if(WIN32)
    if(NOT DEFINED OPENSSL_ARCH)
        set(OPENSSL_ARCH "VC-WIN64A")
    endif()

    # OpenSSL 的 Configure 脚本要求 Windows 原生 perl (Strawberry / ActivePerl)
    # msys/cygwin/Git 自带 perl 会因路径分隔符问题失败 ("This perl implementation
    # doesn't produce Windows like paths")。

    # 内部辅助函数：判断给定 perl 是否为 Windows 原生 perl
    # 使用 function 而非 macro，避免宏文本替换时反斜杠路径被 CMake 解析为转义字符
    function(_orca_is_native_perl _perl_path _result_var)
        file(TO_CMAKE_PATH "${_perl_path}" _perl_path_normalized)
        set(${_result_var} FALSE PARENT_SCOPE)
        if(EXISTS "${_perl_path_normalized}")
            execute_process(
                COMMAND "${_perl_path_normalized}" -V
                OUTPUT_VARIABLE _pv_out
                ERROR_QUIET
            )
            # Strawberry/ActivePerl 的 -V 输出含 "MSWin32" 且不含 msys/cygwin
            if(_pv_out MATCHES "MSWin32" AND NOT _pv_out MATCHES "msys|cygwin")
                set(${_result_var} TRUE PARENT_SCOPE)
            endif()
        endif()
    endfunction()

    set(PERL_EXECUTABLE "")

    # 1. 优先尊重用户通过环境变量或 CMake 变量指定的 perl
    if(DEFINED ENV{OPENSSL_PERL} AND EXISTS "$ENV{OPENSSL_PERL}")
        set(PERL_EXECUTABLE "$ENV{OPENSSL_PERL}")
        message(STATUS "   使用环境变量 OPENSSL_PERL 指定的 perl: ${PERL_EXECUTABLE}")
    elseif(DEFINED OPENSSL_PERL AND EXISTS "${OPENSSL_PERL}")
        set(PERL_EXECUTABLE "${OPENSSL_PERL}")
        message(STATUS "   使用 CMake 变量 OPENSSL_PERL 指定的 perl: ${PERL_EXECUTABLE}")
    endif()

    # 2. 若 CMake 本身来自 Strawberry 安装目录，尝试从同一目录树定位 perl
    #    Strawberry 结构: <root>/c/bin/cmake.exe  <root>/perl/bin/perl.exe
    if(NOT PERL_EXECUTABLE)
        get_filename_component(_cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)   # <root>/c/bin
        get_filename_component(_cmake_c_dir   "${_cmake_bin_dir}" DIRECTORY)  # <root>/c
        get_filename_component(_strawberry_root "${_cmake_c_dir}" DIRECTORY)  # <root>
        set(_cmake_sibling_perl "${_strawberry_root}/perl/bin/perl.exe")
        _orca_is_native_perl("${_cmake_sibling_perl}" _is_native)
        if(_is_native)
            set(PERL_EXECUTABLE "${_cmake_sibling_perl}")
            message(STATUS "   从 CMAKE_COMMAND 同级目录自动定位到 Strawberry Perl: ${PERL_EXECUTABLE}")
        endif()
    endif()

    # 3. 检查常见固定安装路径
    if(NOT PERL_EXECUTABLE)
        set(_perl_candidates
            "C:/Strawberry/perl/bin/perl.exe"
            "C:/Perl64/bin/perl.exe"
            "C:/Perl/bin/perl.exe"
        )
        foreach(_p IN LISTS _perl_candidates)
            _orca_is_native_perl("${_p}" _is_native)
            if(_is_native)
                set(PERL_EXECUTABLE "${_p}")
                break()
            endif()
        endforeach()
    endif()

    # 4. 遍历 PATH 中所有 perl，取第一个 Windows 原生 perl
    if(NOT PERL_EXECUTABLE)
        execute_process(
            COMMAND cmd /c where perl
            OUTPUT_VARIABLE _where_perl_out
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REPLACE "\r\n" "\n" _where_perl_out "${_where_perl_out}")
        string(REPLACE "\r"   "\n" _where_perl_out "${_where_perl_out}")
        string(REPLACE "\n"   ";"  _all_perls      "${_where_perl_out}")
        foreach(_p IN LISTS _all_perls)
            if(NOT _p STREQUAL "")
                _orca_is_native_perl("${_p}" _is_native)
                if(_is_native)
                    set(PERL_EXECUTABLE "${_p}")
                    message(STATUS "   从 PATH 中找到 Windows 原生 perl: ${PERL_EXECUTABLE}")
                    break()
                endif()
            endif()
        endforeach()
    endif()

    if(NOT PERL_EXECUTABLE)
        message(FATAL_ERROR
            "❌ 未找到适用于 OpenSSL 的 Windows 原生 perl。\n"
            "   OpenSSL Configure 不接受 msys/Git/cygwin 自带的 perl。\n"
            "   请安装 Strawberry Perl: https://strawberryperl.com/\n"
            "   默认安装到 C:/Strawberry/perl/bin/perl.exe\n"
            "   若已安装于非标准路径，可通过以下方式指定:\n"
            "     环境变量: set OPENSSL_PERL=C:/your/path/perl.exe\n"
            "     CMake 参数: -DOPENSSL_PERL=C:/your/path/perl.exe")
    endif()
    message(STATUS "   Perl: ${PERL_EXECUTABLE}")

    # 通过 vswhere 定位 VS 安装路径，进而拼出 vcvarsall.bat
    # vswhere 是 VS 2017+ 自带的官方定位工具，路径固定
    # 注意：CMake 里 $ENV{ProgramFiles(x86)} 的括号需要绕开
    set(_pf86 "")
    if(DEFINED ENV{ProgramFiles\(x86\)})
        set(_pf86 "$ENV{ProgramFiles\(x86\)}")
    endif()
    set(_vswhere_candidates
        "${_pf86}/Microsoft Visual Studio/Installer/vswhere.exe"
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
        "C:/Program Files/Microsoft Visual Studio/Installer/vswhere.exe"
    )
    set(VSWHERE_EXECUTABLE "")
    foreach(_c IN LISTS _vswhere_candidates)
        if(EXISTS "${_c}")
            set(VSWHERE_EXECUTABLE "${_c}")
            break()
        endif()
    endforeach()
    if(NOT VSWHERE_EXECUTABLE)
        find_program(VSWHERE_EXECUTABLE vswhere)
    endif()
    if(NOT VSWHERE_EXECUTABLE)
        message(FATAL_ERROR "❌ 找不到 vswhere.exe (VS 2017+ 应自带于 Installer 目录)")
    endif()

    execute_process(
        COMMAND "${VSWHERE_EXECUTABLE}"
            -latest -products *
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
            -property installationPath
        OUTPUT_VARIABLE VS_INSTALL_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _vswhere_result
    )
    if(NOT _vswhere_result EQUAL 0 OR VS_INSTALL_DIR STREQUAL "")
        message(FATAL_ERROR "❌ vswhere 未能定位 Visual Studio 安装")
    endif()
    file(TO_CMAKE_PATH "${VS_INSTALL_DIR}" VS_INSTALL_DIR)

    set(VCVARSALL "${VS_INSTALL_DIR}/VC/Auxiliary/Build/vcvarsall.bat")
    if(NOT EXISTS "${VCVARSALL}")
        message(FATAL_ERROR "❌ vcvarsall.bat 不存在: ${VCVARSALL}")
    endif()
    message(STATUS "   VS 安装目录: ${VS_INSTALL_DIR}")
    message(STATUS "   vcvarsall:   ${VCVARSALL}")

    # 把 OpenSSL 的目标三元组映射到 vcvarsall 的 arch 参数
    if(OPENSSL_ARCH STREQUAL "VC-WIN64A")
        set(VCVARS_ARCH "amd64")
    elseif(OPENSSL_ARCH STREQUAL "VC-WIN32")
        set(VCVARS_ARCH "x86")
    else()
        set(VCVARS_ARCH "amd64")
    endif()

    # 一次性生成 wrapper bat：先 call vcvarsall，再透传参数
    # 这样后续 execute_process 直接当普通可执行调用，不用纠结 cmd 引号规则
    set(VSENV_RUNNER "${OPENSSL_CACHE_DIR}/_orca_vsenv_runner.bat")
    file(WRITE "${VSENV_RUNNER}"
"@echo off\r\n"
"call \"${VCVARSALL}\" ${VCVARS_ARCH} >nul\r\n"
"if errorlevel 1 (echo [orca] vcvarsall failed & exit /b %errorlevel%)\r\n"
"%*\r\n"
"exit /b %errorlevel%\r\n"
    )
    message(STATUS "   VS 环境包装器: ${VSENV_RUNNER}")
elseif(APPLE)
    if(NOT DEFINED OPENSSL_ARCH)
        set(OPENSSL_ARCH "darwin64-${CMAKE_OSX_ARCHITECTURES}-cc")
    endif()
endif()

# 根据状态执行相应操作
if(OPENSSL_STATUS STREQUAL "INSTALLED")
    # 直接使用已安装的 OpenSSL，下面的 OPENSSL_ROOT_DIR 会让 find_package(OpenSSL) 命中
    message(STATUS "🚀 使用缓存的 OpenSSL 库，跳过下载和编译")

elseif(OPENSSL_STATUS STREQUAL "BUILT_COMPLETE")
    # 构建已完成，执行 install_sw 步骤（OpenSSL 是 in-source build）
    message(STATUS "📦 开始安装 OpenSSL...")
    if(WIN32)
        execute_process(
            COMMAND "${VSENV_RUNNER}" nmake install_sw
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE install_result
        )
    else()
        execute_process(
            COMMAND make -j${N_CORES} install_sw
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE install_result
        )
    endif()

    if(install_result EQUAL 0)
        message(STATUS "✅ OpenSSL 安装成功")
        set(OPENSSL_FOUND TRUE)
    else()
        message(FATAL_ERROR "❌ OpenSSL 安装失败")
    endif()

elseif(OPENSSL_STATUS STREQUAL "BUILT_NOT_INSTALLED")
    # 已配置但没构建完，回退到 SOURCE_ONLY 走完整流程。
    # 这里必须先把残留的 .orca_* marker 清掉，否则下面 include 自己时
    # 顶层状态检测又会基于这些 marker 判定为 BUILT_NOT_INSTALLED → 死循环。
    message(STATUS "⚠️  构建未完成，回退到完整 build/install 流程")
    file(GLOB _stale_marks "${OPENSSL_SOURCE_DIR}/.orca_configured-*"
                           "${OPENSSL_SOURCE_DIR}/.orca_built-*")
    if(_stale_marks)
        file(REMOVE ${_stale_marks})
    endif()
    unset(_stale_marks)
    set(OPENSSL_STATUS "SOURCE_ONLY")
    include(${CMAKE_CURRENT_LIST_FILE})
    return()

elseif(OPENSSL_STATUS STREQUAL "SOURCE_ONLY")
    # OpenSSL: in-source build, perl Configure + nmake/make
    message(STATUS "🔨 开始构建 OpenSSL...")

    set(_openssl_config_mark "${OPENSSL_SOURCE_DIR}/.orca_configured-${OPENSSL_ACTUAL_BUILD_TYPE}-${FETCH_CACHE_ARCH}")
    set(_openssl_build_mark  "${OPENSSL_SOURCE_DIR}/.orca_built-${OPENSSL_ACTUAL_BUILD_TYPE}-${FETCH_CACHE_ARCH}")

    # in-source build 的 src 树里可能残留前次其它 arch 的 .o/.a。configure 前
    # 跑 make distclean，把旧 obj 全清掉（distclean 比 clean 更彻底，连 Makefile
    # 都重置）。如果还没 configure 过就没有 Makefile，distclean 会报错但无害，
    # 因此忽略其返回码。
    if(NOT EXISTS "${_openssl_config_mark}")
        execute_process(
            COMMAND make distclean
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE _ignored
            OUTPUT_QUIET
            ERROR_QUIET
        )
        # 也清掉其它 arch 留下的 .orca_* 标记，确保下次切回时也会重做 distclean
        file(GLOB _stale_marks "${OPENSSL_SOURCE_DIR}/.orca_configured-*"
                               "${OPENSSL_SOURCE_DIR}/.orca_built-*")
        if(_stale_marks)
            file(REMOVE ${_stale_marks})
        endif()
        unset(_stale_marks)
    endif()

    if(WIN32)
        # ---- Windows: perl Configure VC-WIN64A + nmake (通过 vcvarsall 包装) ----
        if(NOT EXISTS "${_openssl_config_mark}")
            message(STATUS "   ⚙️  配置: perl Configure ${OPENSSL_ARCH}")
            execute_process(
                COMMAND "${VSENV_RUNNER}" "${PERL_EXECUTABLE}" Configure ${OPENSSL_ARCH}
                    "--prefix=${OPENSSL_INSTALL_DIR}"
                    "--openssldir=${OPENSSL_INSTALL_DIR}"
                    no-shared
                    no-asm
                    no-ssl3-method
                    no-dynamic-engine
                WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
                RESULT_VARIABLE config_result
            )
            if(NOT config_result EQUAL 0)
                message(FATAL_ERROR "❌ OpenSSL 配置失败，返回码: ${config_result}")
            endif()
            file(WRITE "${_openssl_config_mark}" "configured\n")
            message(STATUS "✅ OpenSSL 配置成功")
        endif()

        message(STATUS "🔨 正在编译 OpenSSL (这可能需要较长时间)...")
        execute_process(
            COMMAND "${VSENV_RUNNER}" nmake
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE build_result
        )
        if(NOT build_result EQUAL 0)
            message(FATAL_ERROR "❌ OpenSSL 编译失败，返回码: ${build_result}")
        endif()
        file(WRITE "${_openssl_build_mark}" "built\n")
        message(STATUS "✅ OpenSSL 编译成功")

        message(STATUS "📦 正在安装 OpenSSL...")
        execute_process(
            COMMAND "${VSENV_RUNNER}" nmake install_sw
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE install_result
        )
        if(NOT install_result EQUAL 0)
            message(FATAL_ERROR "❌ OpenSSL 安装失败，返回码: ${install_result}")
        endif()
        message(STATUS "✅ OpenSSL 安装成功")
        set(OPENSSL_FOUND TRUE)

    else()
        # ---- Unix/macOS: ./Configure 或 ./config + make ----
        if(NOT EXISTS "${_openssl_config_mark}")
            if(APPLE)
                set(_openssl_conf_cmd ./Configure ${OPENSSL_ARCH}
                        -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
            else()
                set(_openssl_conf_cmd ./config)
            endif()
            message(STATUS "   ⚙️  配置: ${_openssl_conf_cmd}")
            execute_process(
                COMMAND ${_openssl_conf_cmd}
                    "--prefix=${OPENSSL_INSTALL_DIR}"
                    "--openssldir=${OPENSSL_INSTALL_DIR}"
                    no-shared
                    no-asm
                    no-ssl3-method
                    no-dynamic-engine
                WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
                RESULT_VARIABLE config_result
            )
            if(NOT config_result EQUAL 0)
                message(FATAL_ERROR "❌ OpenSSL 配置失败，返回码: ${config_result}")
            endif()
            file(WRITE "${_openssl_config_mark}" "configured\n")
            message(STATUS "✅ OpenSSL 配置成功")
        endif()

        message(STATUS "🔨 正在编译 OpenSSL (使用 ${N_CORES} 个并行任务)...")
        execute_process(
            COMMAND make -j${N_CORES}
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE build_result
        )
        if(NOT build_result EQUAL 0)
            message(FATAL_ERROR "❌ OpenSSL 编译失败，返回码: ${build_result}")
        endif()
        file(WRITE "${_openssl_build_mark}" "built\n")
        message(STATUS "✅ OpenSSL 编译成功")

        message(STATUS "📦 正在安装 OpenSSL...")
        execute_process(
            COMMAND make -j${N_CORES} install_sw
            WORKING_DIRECTORY ${OPENSSL_SOURCE_DIR}
            RESULT_VARIABLE install_result
        )
        if(NOT install_result EQUAL 0)
            message(FATAL_ERROR "❌ OpenSSL 安装失败，返回码: ${install_result}")
        endif()
        message(STATUS "✅ OpenSSL 安装成功")
        set(OPENSSL_FOUND TRUE)
    endif()

    # 拷贝项目自带的 CMake 配置文件，提供 OpenSSL::SSL / OpenSSL::Crypto target
    if(OPENSSL_FOUND AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/openssl")
        file(COPY "${CMAKE_CURRENT_LIST_DIR}/openssl"
             DESTINATION "${OPENSSL_INSTALL_DIR}/lib/cmake")
        message(STATUS "✅ OpenSSL CMake 配置文件已就位")
    endif()

elseif(OPENSSL_STATUS STREQUAL "ZIP_ONLY")
    # 解压并构建
    message(STATUS "📦 解压 OpenSSL 源码...")

    # 确保源码目录不存在
    if(EXISTS "${OPENSSL_SOURCE_DIR}")
        message(STATUS "⚠️  清理旧的源码目录...")
        file(REMOVE_RECURSE "${OPENSSL_SOURCE_DIR}")
    endif()

    # 解压到临时目录
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${OPENSSL_ZIP_FILE}
        WORKING_DIRECTORY ${OPENSSL_CACHE_DIR}
        RESULT_VARIABLE extract_result
        OUTPUT_QUIET
        ERROR_VARIABLE extract_error
    )

    if(extract_result EQUAL 0)
        # OpenSSL 的 GitHub archive 解压后目录名形如 openssl-OpenSSL_1_1_1w
        file(GLOB OPENSSL_EXTRACTED_DIRS "${OPENSSL_CACHE_DIR}/openssl-*")
        list(FILTER OPENSSL_EXTRACTED_DIRS EXCLUDE REGEX "\\.tar\\.gz$")
        list(GET OPENSSL_EXTRACTED_DIRS 0 OPENSSL_EXTRACTED_DIR)

        if(EXISTS "${OPENSSL_EXTRACTED_DIR}")
            # 移动到标准源码目录
            file(RENAME "${OPENSSL_EXTRACTED_DIR}" "${OPENSSL_SOURCE_DIR}")
            message(STATUS "✅ OpenSSL 解压成功")

            # 更新状态并重新处理
            set(HAS_SOURCE TRUE)
            set(OPENSSL_STATUS "SOURCE_ONLY")
            message(STATUS "🔄 切换到源码构建模式...")

            # 递归调用处理SOURCE_ONLY状态
            include(${CMAKE_CURRENT_LIST_FILE})
        else()
            message(FATAL_ERROR "❌ 解压后未找到OpenSSL目录")
        endif()
    else()
        message(WARNING "❌ OpenSSL 解压失败: ${extract_error}")
        message(STATUS "🔄 删除损坏的压缩包并重新下载...")
        file(REMOVE "${OPENSSL_ZIP_FILE}")
        set(OPENSSL_STATUS "NONE")
        include(${CMAKE_CURRENT_LIST_FILE})
    endif()

else()
    # 需要下载
    message(STATUS "⬇️  开始下载 OpenSSL ${OPENSSL_VERSION}...")

    # OpenSSL 1.1.1w GitHub archive
    set(OPENSSL_DOWNLOAD_URL "https://github.com/openssl/openssl/archive/${OPENSSL_GIT_TAG}.tar.gz")

    message(STATUS "📥 正在下载 OpenSSL...")
    message(STATUS "   URL: ${OPENSSL_DOWNLOAD_URL}")

    file(DOWNLOAD
        ${OPENSSL_DOWNLOAD_URL}
        ${OPENSSL_ZIP_FILE}
        EXPECTED_HASH SHA256=2130E8C2FB3B79D1086186F78E59E8BC8D1A6AEDF17AB3907F4CB9AE20918C41
        STATUS download_status
        LOG download_log
        SHOW_PROGRESS
    )

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_msg)

    if(NOT status_code EQUAL 0)
        message(FATAL_ERROR "❌ OpenSSL 下载失败: ${status_msg}\n   ${download_log}")
    endif()

    message(STATUS "✅ OpenSSL 下载成功")

    # 解压
    message(STATUS "📦 解压 OpenSSL 源码...")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${OPENSSL_ZIP_FILE}
        WORKING_DIRECTORY ${OPENSSL_CACHE_DIR}
        RESULT_VARIABLE extract_result
    )

    if(extract_result EQUAL 0)
        message(STATUS "✅ OpenSSL 下载和解压成功")
        # 递归调用自己来处理SOURCE_ONLY状态
        set(OPENSSL_STATUS "SOURCE_ONLY")
        include(${CMAKE_CURRENT_LIST_FILE})
    else()
        message(FATAL_ERROR "❌ OpenSSL 解压失败")
    endif()
endif()

# 注册到 find_package(OpenSSL)
if(OPENSSL_FOUND)
    list(PREPEND CMAKE_PREFIX_PATH "${OPENSSL_INSTALL_DIR}")
    set(OPENSSL_ROOT_DIR "${OPENSSL_INSTALL_DIR}" CACHE PATH "OpenSSL root" FORCE)
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_INSTALL_DIR}/include" CACHE PATH "OpenSSL include dir" FORCE)
    set(OPENSSL_INCLUDE_DIRS "${OPENSSL_INSTALL_DIR}/include" CACHE PATH "OpenSSL include dirs" FORCE)
    include_directories(SYSTEM "${OPENSSL_INSTALL_DIR}/include")
    set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "Use static OpenSSL libs" FORCE)
    set(OPENSSL_AVAILABLE TRUE CACHE BOOL "OpenSSL library is available")
    message(STATUS "")
    message(STATUS "🎯 OpenSSL 库已就绪")
    message(STATUS "   版本: ${OPENSSL_VERSION}")
    message(STATUS "   安装目录: ${OPENSSL_INSTALL_DIR}")
else()
    set(OPENSSL_AVAILABLE FALSE CACHE BOOL "OpenSSL library is not available")
    message(FATAL_ERROR "⚠️  OpenSSL 库不可用")
endif()
