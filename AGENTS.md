# libslicer

Minimal C++17 static-library build extracted from OrcaSlicer's `src/libslic3r`.
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

The macOS arm64 release artifact is:

```bash
build-libslicer/release/arm64/src/libslic3r/liblibslicer.a
```

## Scope

- Primary target: `libslicer`
- Core source: `src/libslic3r/`
- Bundled source dependencies: `deps_src/`
- Fetched dependencies: managed by `deps/fetch_deps.cmake`
- No GUI/app/package targets are expected in this branch.

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
