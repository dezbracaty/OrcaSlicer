# libslicer

Standalone slicing library extracted from OrcaSlicer's `src/libslic3r`, with a
public C++17 shared-library API and an internal static core.
This branch intentionally removes the wxWidgets GUI, app entry points, packaging
targets, legacy shell build scripts, and unused third-party subprojects.

## Build Commands

Use CMake presets directly.

```bash
# macOS arm64 release
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release

# macOS arm64 debug
cmake --preset macos-arm64-debug
cmake --build --preset macos-arm64-debug

# Linux
cmake --preset linux-release
cmake --build --preset linux-release

# Windows
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
```

The macOS arm64 release public artifact is:

```bash
build-libslicer/release/arm64/src/libslicer_api/liblibslicer.dylib
```

## Tests

The top-level project registers core and public API Catch2 suites, continuous-
fiber model acceptance, and a CMake consumer smoke test. Build all test
executables before running unfiltered CTest:

```bash
cmake --preset macos-arm64-release -DLIBSLICER_BUILD_TESTS=ON
cmake --build --preset macos-arm64-release --target libslic3r_tests libslicer_api_tests fiber_4xiao_acceptance
ctest --test-dir build-libslicer/release/arm64 --output-on-failure
```

## Scope

- Build target: `libslicer` (depends on `libslicer_api`)
- Public API: `include/libslicer/`, implemented in `src/libslicer_api/`
- Application link target: `libslicer::libslicer` (shared; installed CMake package)
- Internal link target: `libslicer::core` (static; source builds only)
- Core source: `src/libslic3r/`
- Bundled source dependencies: `deps_src/`
- Fetched dependencies: managed by `deps/fetch_deps.cmake`
- Tests: `tests/libslic3r`, `tests/libslicer_api`, `tests/cmake`, `tests/catch2`,
  `tests/data`, `models/continuous_fiber`, and shared helpers
- No GUI, application-bundle, or installer targets are expected in this branch.
- See `README.md` for SDK installation, runtime resources, and public API usage.

## Code Style

- C++17, selective C++20 where already used.
- PascalCase classes, snake_case functions and variables.
- Prefer RAII and existing local helpers.
- Parallelization uses TBB; be careful with shared state.

## Constraints

- Keep `.3mf` and printer-profile compatibility in libslic3r behavior.
- Do not reintroduce GUI, app bundle, installer, or shell-script build logic.
- Keep dependency additions scoped to code that is actually linked by `libslicer`.
- Avoid adding new source files unless an existing libslic3r file cannot be kept
  self-contained.
