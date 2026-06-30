# deps/fetch_deps.cmake
#
# Aggregator for the fetch-based dependency system.
#
# Each included file under deps/<Pkg>/ is responsible for fetching, building
# and installing one third-party library into the fetch cache
# (${CMAKE_FETCH_CACHE}) and for registering itself so that subsequent
# find_package(<Pkg>) calls in the main project succeed.
#
# The include order below is the dependency order: a package that is needed
# by another must appear first, because the per-package scripts build
# immediately at configure time via execute_process().

if (NOT DEFINED CMAKE_FETCH_CACHE OR CMAKE_FETCH_CACHE STREQUAL "")
    message(FATAL_ERROR
        "CMAKE_FETCH_CACHE is not set. The top-level CMakeLists.txt is expected "
        "to set a default before including deps/fetch_deps.cmake.")
endif ()

message(STATUS "Fetching third-party dependencies into ${CMAKE_FETCH_CACHE}")

# ----------------------------------------------------------------------------
# Architecture tag for the per-package cache layout, uniform across platforms.
#
# Each package's build/install tree lives under
#   ${CMAKE_FETCH_CACHE}/<pkg>-<ver>/${FETCH_CACHE_ARCH}/{build,install}-<type>
#
# so that building the same project for different architectures on the same
# machine (e.g. arm64 vs x86_64) keeps fully independent build/install trees.
# Source archives and unpacked sources stay at <pkg>-<ver>/{src,*.tar.gz},
# since they are arch-neutral.
#
# 平台来源（按优先级）：
#   macOS  : CMAKE_OSX_ARCHITECTURES（原生构建下 CMAKE_SYSTEM_PROCESSOR 仍跟随
#            宿主机，不会自动同步 OSX_ARCHITECTURES，必须显式读它，否则在
#            arm64 机器上跑 x86_64 preset 会静默链 arm64 deps）
#   其它   : CMAKE_SYSTEM_PROCESSOR（Windows VS 由 -A 推导，Linux 由 host）
#
# macOS universal（CMAKE_OSX_ARCHITECTURES 多值）有意不支持：每个 arch
# 单独构建，外部 lipo 合并。
# ----------------------------------------------------------------------------
if (APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _arch_count)
    if (_arch_count GREATER 1)
        message(FATAL_ERROR
            "Universal builds (multi-arch CMAKE_OSX_ARCHITECTURES='${CMAKE_OSX_ARCHITECTURES}') "
            "are not supported by fetch_deps; produce per-arch builds and lipo-merge externally.")
    endif ()
    string(TOLOWER "${CMAKE_OSX_ARCHITECTURES}" FETCH_CACHE_ARCH)
    unset(_arch_count)
elseif (CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" FETCH_CACHE_ARCH)
elseif (WIN32)
    # CMAKE_SYSTEM_PROCESSOR 在 Windows+Ninja+MSVC 场景下可能为空（VS 开发者环境
    # 未初始化时 CMake 还没完成编译器探测）。按优先级回退：
    #   1. VSCMD_ARG_TGT_ARCH — vcvarsall.bat 设置的目标架构（最准确）
    #   2. PROCESSOR_ARCHITECTURE — Windows 系统变量（x64/AMD64）
    #   3. 默认 x64
    if (DEFINED ENV{VSCMD_ARG_TGT_ARCH} AND NOT "$ENV{VSCMD_ARG_TGT_ARCH}" STREQUAL "")
        string(TOLOWER "$ENV{VSCMD_ARG_TGT_ARCH}" FETCH_CACHE_ARCH)
        message(STATUS "Fetch cache arch derived from VSCMD_ARG_TGT_ARCH: ${FETCH_CACHE_ARCH}")
    elseif (DEFINED ENV{PROCESSOR_ARCHITECTURE} AND NOT "$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "")
        string(TOLOWER "$ENV{PROCESSOR_ARCHITECTURE}" FETCH_CACHE_ARCH)
        message(STATUS "Fetch cache arch derived from PROCESSOR_ARCHITECTURE: ${FETCH_CACHE_ARCH}")
    else ()
        set(FETCH_CACHE_ARCH "x64")
        message(WARNING "Cannot determine architecture; defaulting to x64.")
    endif ()
else ()
    message(FATAL_ERROR
        "Cannot derive FETCH_CACHE_ARCH: CMAKE_SYSTEM_PROCESSOR is empty and "
        "CMAKE_OSX_ARCHITECTURES is unset. "
        "On macOS pass -DCMAKE_OSX_ARCHITECTURES=arm64|x86_64; "
        "on Windows VS pass -A x64|ARM64|Win32.")
endif ()

# Normalize Windows arch aliases so the cache layout matches CMakePresets
# binaryDir / Visual Studio / MSBuild naming. CMake reports 64-bit Intel/AMD
# as "AMD64" on Windows, but VS / Ninja / MSBuild and our presets all call it
# "x64". Keep both layouts aligned so build/<arch>/ matches <cache>/<pkg>/<arch>/.
if (FETCH_CACHE_ARCH STREQUAL "amd64")
    set(FETCH_CACHE_ARCH "x64")
endif ()

set(ENV{FETCH_CACHE_ARCH} "${FETCH_CACHE_ARCH}")
message(STATUS "Fetch cache architecture tag: ${FETCH_CACHE_ARCH}")

# ----------------------------------------------------------------------------
# 通用 forward args：每个 dep 的 .cmake 在 execute_process(${CMAKE_COMMAND} -S ... -B ...)
# 启动子 cmake 配置时，必须把宿主项目的关键编译条件原样透传。否则在 macOS arm64
# 主机上跑 x86_64 preset，子 cmake 会默认按 host arch 编出 arm64 静态库，再被
# 装进 cache 的 x86_64/install-Release/ 路径里造成静默不一致 → slicer 链接报错。
#
# 用法：每个 dep 在自己的 configure execute_process 里展开 ${FETCH_DEPS_FORWARD_ARGS}：
#
#   execute_process(
#       COMMAND ${CMAKE_COMMAND}
#           -S <src> -B <build>
#           ${FETCH_DEPS_FORWARD_ARGS}
#           -DCMAKE_INSTALL_PREFIX=<install>
#           ...
#   )
#
# 不要把 -DCMAKE_BUILD_TYPE 放进来——dep 各自有 BOOST_ACTUAL_BUILD_TYPE 之类
# 的 per-dep 控制；混进 forward args 会覆盖它们。
# ----------------------------------------------------------------------------
set(FETCH_DEPS_FORWARD_ARGS "")

# 编译器：让子 cmake 用同一套 toolchain，避免主项目用 Xcode clang 而子 cmake
# 自己再去探测一遍（探测到 Apple Clang 没问题，但发行编译标志一致性更强）。
if (CMAKE_C_COMPILER)
    list(APPEND FETCH_DEPS_FORWARD_ARGS "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif ()
if (CMAKE_CXX_COMPILER)
    list(APPEND FETCH_DEPS_FORWARD_ARGS "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif ()

# macOS：必须显式透传 OSX_ARCHITECTURES 与 DEPLOYMENT_TARGET，子 cmake 不会
# 自动从父进程继承这些 cache 变量。
if (APPLE)
    if (CMAKE_OSX_ARCHITECTURES)
        list(APPEND FETCH_DEPS_FORWARD_ARGS
            "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    endif ()
    if (CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND FETCH_DEPS_FORWARD_ARGS
            "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif ()
    if (CMAKE_OSX_SYSROOT)
        list(APPEND FETCH_DEPS_FORWARD_ARGS
            "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}")
    endif ()
endif ()

# CMAKE_IGNORE_PREFIX_PATH：阻止子 cmake 的 find_package 解析到 /opt/homebrew
# 之类宿主目录（boost / wxWidgets 这种会 find_package(ZLIB/PNG/...) 的 dep 受影响）。
if (CMAKE_IGNORE_PREFIX_PATH)
    string(REPLACE ";" "\\;" _ignore_prefix_escaped "${CMAKE_IGNORE_PREFIX_PATH}")
    list(APPEND FETCH_DEPS_FORWARD_ARGS
        "-DCMAKE_IGNORE_PREFIX_PATH=${_ignore_prefix_escaped}")
    unset(_ignore_prefix_escaped)
endif ()

# CMAKE_POLICY_VERSION_MINIMUM：CMake 4.x 兼容老 dep 项目（如 boost）的 hatch。
# 主项目顶层若设过则继承下去，dep 自己也可叠加自己的策略。
if (CMAKE_POLICY_VERSION_MINIMUM)
    list(APPEND FETCH_DEPS_FORWARD_ARGS
        "-DCMAKE_POLICY_VERSION_MINIMUM=${CMAKE_POLICY_VERSION_MINIMUM}")
endif ()

message(STATUS "Fetch deps forward args: ${FETCH_DEPS_FORWARD_ARGS}")

# macOS：直接调用 toolchain 内部 cc 的绝对路径（autoconf-style 的 GMP/MPFR、
# perl-Configure 的 OpenSSL 都会这么做）时，clang/ld 不会自动 xcrun 出 SDKROOT，
# 表现为 "library 'System' not found" 或 "stdio.h not found"。把 SDKROOT 注入
# 当前 cmake 进程的 env，后续所有 execute_process 子进程会继承到正确 SDK。
# CMAKE_OSX_SYSROOT 在 project() 后由 cmake 自动填充。
if (APPLE AND CMAKE_OSX_SYSROOT)
    set(ENV{SDKROOT} "${CMAKE_OSX_SYSROOT}")
    message(STATUS "Exported SDKROOT=${CMAKE_OSX_SYSROOT} to fetch_deps subprocess env")
endif ()

set(_deps_root "${CMAKE_CURRENT_LIST_DIR}")

# Prepend the fetch cache to CMAKE_PREFIX_PATH so any find_package() calls
# that per-package scripts make (or that the main project makes later) can
# resolve the freshly-installed libraries. Individual scripts are still
# responsible for setting <Pkg>_DIR when a package uses a non-standard layout.
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_FETCH_CACHE}")

# ----------------------------------------------------------------------------
# Order mirrors deps/CMakeLists.txt (the old ExternalProject flow), which has
# already been validated. Do not reorder without checking the dependency graph.
# ----------------------------------------------------------------------------

include(${_deps_root}/ZLIB/ZLIB.cmake)
include(${_deps_root}/PNG/PNG.cmake)
include(${_deps_root}/EXPAT/EXPAT.cmake)
include(${_deps_root}/Boost/Boost.cmake)
include(${_deps_root}/Eigen/Eigen.cmake)

include(${_deps_root}/Cereal/Cereal.cmake)

include(${_deps_root}/Qhull/Qhull.cmake)
include(${_deps_root}/GLEW/GLEW.cmake)
include(${_deps_root}/GLFW/GLFW.cmake)
include(${_deps_root}/OpenCSG/OpenCSG.cmake)
include(${_deps_root}/TBB/TBB.cmake)
include(${_deps_root}/Blosc/Blosc.cmake)
include(${_deps_root}/OpenEXR/OpenEXR.cmake)
include(${_deps_root}/OpenVDB/OpenVDB.cmake)
include(${_deps_root}/GMP/GMP.cmake)
include(${_deps_root}/MPFR/MPFR.cmake)
include(${_deps_root}/CGAL/CGAL.cmake)
include(${_deps_root}/NLopt/NLopt.cmake)
include(${_deps_root}/libnoise/libnoise.cmake)
include(${_deps_root}/Draco/Draco.cmake)
include(${_deps_root}/OpenSSL/OpenSSL.cmake)
include(${_deps_root}/CURL/CURL.cmake)
include(${_deps_root}/JPEG/JPEG.cmake)
include(${_deps_root}/wxWidgets/wxWidgets.cmake)
include(${_deps_root}/NanoSVG/NanoSVG.cmake)
include(${_deps_root}/FREETYPE/FREETYPE.cmake)
include(${_deps_root}/OCCT/OCCT.cmake)
include(${_deps_root}/OpenCV/OpenCV.cmake)

unset(_deps_root)
