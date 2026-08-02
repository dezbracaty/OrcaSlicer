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

The macOS arm64 internal core artifact is:

```bash
build-libslicer/release/arm64/src/libslic3r/liblibslicer_core.a
```

Other presets are available in `CMakePresets.json` for macOS, Linux, and
Windows release/debug builds.

## Tests

The CMake project keeps the `tests/libslic3r` Catch2 suite connected by default.

```bash
cmake --build --preset macos-arm64-release --target libslic3r_tests
ctest --test-dir build-libslicer/release/arm64 --output-on-failure
```

When consumed from another CMake project with `add_subdirectory`, tests are
disabled by default. Link raw internal APIs against `libslicer::core`:

```cmake
set(LIBSLICER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/libslicer)

target_link_libraries(my_app PRIVATE libslicer::core)
```

Application integrations should install this project and consume the stable
facade package using `find_package(libslicer CONFIG REQUIRED)`, then link
`libslicer::libslicer`. The facade deliberately hides OrcaSlicer's internal
third-party dependency graph.

Applications using profile or resource-backed APIs must initialize runtime
paths before loading presets or related data:

```cpp
Slic3r::set_resources_dir("/path/to/libslicer/resources");
Slic3r::set_data_dir("/path/to/app-data");
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
