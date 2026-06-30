# libslicer

`libslicer` is a minimal static-library build of OrcaSlicer's core slicing
engine, extracted around `src/libslic3r`.

This branch intentionally excludes the wxWidgets application, GUI resources,
packaging scripts, installer workflows, and unrelated test suites. It keeps the
runtime profile data needed by `libslic3r` preset loading.

## Build

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
```

The macOS arm64 release artifact is:

```bash
build-libslicer/release/arm64/src/libslic3r/liblibslicer.a
```

Other presets are available in `CMakePresets.json` for macOS, Linux, and
Windows release/debug builds.

## Tests

The CMake project keeps the `tests/libslic3r` Catch2 suite connected by default.

```bash
cmake --build --preset macos-arm64-release --target libslic3r_tests
ctest --test-dir build-libslicer/release/arm64 --output-on-failure
```

## Kept Runtime Data

The following resource groups are intentionally retained because they are used
or closely coupled to `libslic3r` runtime behavior:

- `resources/profiles`
- `resources/profiles_template`
- `resources/printers`
- `resources/fonts`
- `resources/flush`

Additional calibration and handy-model data is retained for now until its API
surface is explicitly removed or isolated.
