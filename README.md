# libslicer

`libslicer` embeds OrcaSlicer's slicing engine in C++ applications without the
wxWidgets application, GUI, or installer targets. This branch includes preset
management, mesh and 3MF input, G-code export and preview, continuous-fiber
slicing, and belt-printer support.

Applications use the public C++17 API in [`include/libslicer`](include/libslicer).
The implementation keeps OrcaSlicer and its third-party types behind that API.

| CMake target | Purpose |
| --- | --- |
| `libslicer::libslicer` | Public shared library; exported by the installed CMake package. |
| `libslicer::core` | Internal static core; source builds only, with internal dependencies. |
| `libslicer` | Build target for the public library; not a link target. |

## Build and install

Run these commands from this repository's root. Prerequisites are CMake 3.21 or
newer (for the preset schema), Ninja, Git, and a C++ compiler with the platform
SDK. The public API requires C++17; parts of the implementation use C++20.
On macOS, install the Xcode command-line tools. On Windows, run from an x64
MSVC developer environment with `cl.exe` and Ninja on `PATH`.

```sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --parallel 6
cmake --install build-libslicer/release/arm64 --prefix /absolute/path/to/libslicer-sdk
```

Configuration downloads and builds dependencies through
[`deps/fetch_deps.cmake`](deps/fetch_deps.cmake); the first configure requires
network access and may take a while. Dependency artifacts are cached under
`~/.cmake_fetch_cache` (`%USERPROFILE%/.cmake_fetch_cache` on Windows).
Override this with `-DCMAKE_FETCH_CACHE=/absolute/path/to/cache`, or the
`CMAKE_FETCH_CACHE` environment variable.

Choose the preset for the target architecture; use its build directory for
installation and CTest:

| Release preset | Build directory | Debug preset |
| --- | --- | --- |
| `macos-arm64-release` | `build-libslicer/release/arm64` | `macos-arm64-debug` |
| `macos-x86_64-release` | `build-libslicer/release/x86_64` | `macos-x86_64-debug` |
| `linux-release` | `build-libslicer/release/linux` | `linux-debug` |
| `windows-x64-release` | `build-libslicer/release/x64` | `windows-x64-debug` |

Debug directories replace `release` with `debug`. `cmake --list-presets` lists
presets available on the current host. macOS presets default to deployment
target 11.3; override with `-DCMAKE_OSX_DEPLOYMENT_TARGET=<version>` when needed,
and keep the SDK and application's deployment targets compatible.

| Configure option | Default | Effect |
| --- | --- | --- |
| `LIBSLICER_BUILD_TESTS` | `ON` standalone, `OFF` as a subdirectory | Enable test targets. Build presets select only `libslicer`; build tests explicitly. |
| `SLIC3R_PCH` | `ON` | Use precompiled headers. |
| `SLIC3R_STATIC` | `ON`, unless overridden by the environment | Link fetched dependencies statically where supported. The public library remains shared. |

The macOS arm64 public artifact is
`build-libslicer/release/arm64/src/libslicer_api/liblibslicer.dylib`; the internal
archive is `src/libslic3r/liblibslicer_core.a` under the same build directory.
Linux produces `liblibslicer.so`; Windows produces `libslicer.dll` and its
import library. Installation provides the shared library, public headers,
`lib/cmake/libslicer` package files, and `share/libslicer` runtime data (locations
follow CMake's install-directory settings).

## Use the installed SDK

Create an application directory with this `CMakeLists.txt` and the `main.cpp`
below. Consumers link only the public target:

```cmake
cmake_minimum_required(VERSION 3.21)
project(libslicer_example LANGUAGES CXX)

find_package(libslicer CONFIG REQUIRED)
add_executable(libslicer_example main.cpp)
target_link_libraries(libslicer_example PRIVATE libslicer::libslicer)
```

```cpp
#include <libslicer/Library.hpp>
#include <iostream>
#include <utility>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 6) {
        std::cerr << "Usage: libslicer_example RESOURCE_DIR"
                     " [MODEL_PATH OUTPUT_GCODE MACHINE_ID VARIANT_ID]\n";
        return 2;
    }
    const auto report = [](const auto& diagnostics) {
        for (const auto& diagnostic : diagnostics)
            std::cerr << diagnostic.message << '\n';
    };

    libslicer::LibraryOptions options;
    options.resource_directory = argv[1]; // Directory containing profiles/.
    std::vector<libslicer::ConfigDiagnostic> diagnostics;
    auto library = libslicer::Library::open(options, &diagnostics);
    report(diagnostics);
    if (!library)
        return 1;

    if (argc == 2) {
        for (const auto& machine : library->machine_models())
            for (const auto& variant : machine.variants)
                std::cout << machine.id << '\t' << variant.id << '\n';
        return 0;
    }

    libslicer::ConfigSelection selection;
    selection.machine_model_id = argv[4];
    selection.machine_variant_id = argv[5];
    // Empty process/material IDs select compatible defaults for this machine.
    auto configured = library->create_config(selection);
    report(configured.diagnostics);
    if (!configured)
        return 1;

    libslicer::SliceRequest request;
    request.config = configured.config->snapshot();
    request.output_gcode_path = argv[3];
    libslicer::SliceObjectInput object;
    object.model_path = argv[2];
    request.objects.push_back(std::move(object));

    auto result = library->slice(request);
    report(result.diagnostics);
    if (!result)
        return 1;
    std::cout << result.output.path << '\n';
    return 0;
}
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/path/to/libslicer-sdk
cmake --build build
# List machine and nozzle-variant IDs before selecting a real printer.
./build/libslicer_example /absolute/path/to/libslicer-sdk/share/libslicer
# Replace MACHINE_ID and VARIANT_ID with an appropriate pair from that list.
./build/libslicer_example /absolute/path/to/libslicer-sdk/share/libslicer model.stl output.gcode "MACHINE_ID" "VARIANT_ID"
```

On Windows, run `build\libslicer_example.exe` and make the SDK's `bin` directory
available on `PATH`, or deploy its DLLs beside the executable. For installed
applications on macOS/Linux, configure the runtime loader search path or bundle
the shared library. `CMAKE_PREFIX_PATH` finds the SDK at configure time; it does
not deploy runtime libraries or data.

Source-tree integration can use the same public API:

```cmake
add_subdirectory(external/libslicer)
target_link_libraries(my_app PRIVATE libslicer::libslicer)
```

Pass `external/libslicer/resources` as `LibraryOptions::resource_directory`
during development. Use `libslicer::core` only for intentional dependencies on
internal `Slic3r` APIs; it is not part of the installed SDK.

## Runtime resources and API behavior

Ship the installed `share/libslicer` directory with the library. It contains the
OrcaFilamentLibrary, Flashforge, BBL, Ideaformer, CFSYS, and P3 profile bundles,
bed images, and nozzle metadata. The source `resources/` directory contains
additional data; installation deliberately selects a subset.

`Library::open()` initializes internal resource paths. Without an explicit
directory, discovery checks executable-relative paths on macOS/Linux (including
`../Resources/libslicer` on macOS and `../share/libslicer`), the configured install
location, and working-directory-relative `share/libslicer` or
`resources/libslicer`. For embedding or relocated deployment, pass the actual
resource root explicitly, as in the example. `find_package` also exposes
`LIBSLICER_RESOURCE_DIR` to the consuming CMake project.

- [`Library.hpp`](include/libslicer/Library.hpp): machine/preset selection, active
  configuration, slicing, G-code preview, 3MF import, and mesh cutting. Check
  result success, cancellation, and diagnostics; warnings may accompany success.
  `Library::open` returns null on initialization failure.
- [`Config.hpp`](include/libslicer/Config.hpp): settings metadata, edits,
  validation, and independent configuration snapshots. Pass a snapshot to each
  slice. Material slots and physical tool IDs are distinct; use explicit tool
  mappings rather than inferring tools from material types.
- [`Toolpath.hpp`](include/libslicer/Toolpath.hpp): preview data for rendering.
  Logical motions and render segments have separate counts; retain the preview
  handle while using its data.
- `slice`, `load_gcode_preview`, and `import_project` are synchronous. Run long
  operations on an application worker; callbacks do not marshal to the GUI
  thread. Progress is in `[0, 1]`; cancellation is cooperative through
  `SliceCallbacks::is_cancelled`. Keep callbacks thread-safe and copy any
  `string_view` text needed after a callback returns.
- The core uses process-wide resource paths. Initialize libraries consistently
  and serialize access to shared mutable library/configuration state; the API
  does not promise unrestricted concurrent use of a `Library` instance.
- Each `SliceObjectInput` supplies either a model path or in-memory volumes.
  Transforms are row-major affine matrices in millimetres. Preserve volume
  roles and facet labels when transferring imported project geometry.
- An explicit output path is `CallerOwned` and is never removed by the library.
  An omitted path produces a `LibraryTemporary` artifact: the consumer must
  retain it while needed and remove it when no preview needs it. The ownership
  label does not provide automatic file deletion.

The API uses C++ standard-library types. Build the SDK and application with
compatible architectures, compilers, and C++ runtimes; it is not a C ABI.

## Tests

Standalone configuration enables tests by default. Reconfigure with
`-DLIBSLICER_BUILD_TESTS=ON` if an existing build disabled them. Build all three
test executables before running the complete registered suite:

```sh
cmake --preset macos-arm64-release -DLIBSLICER_BUILD_TESTS=ON
cmake --build --preset macos-arm64-release --target libslic3r_tests libslicer_api_tests fiber_4xiao_acceptance --parallel 6
ctest --test-dir build-libslicer/release/arm64 --output-on-failure
```

The suite covers the core (`tests/libslic3r`), public API (`tests/libslicer_api`),
full-model fiber acceptance, and an `add_subdirectory` consumer smoke test. Some
full-model cases take several minutes. The bracket fixture notes below record
known resin regression limitations and the outstanding acceptance criteria.
Catch2 tags are also CTest labels:

```sh
ctest --test-dir build-libslicer/release/arm64 --print-labels
ctest --test-dir build-libslicer/release/arm64 -L '^libslicer_api$' --output-on-failure
ctest --test-dir build-libslicer/release/arm64 -L '^ContinuousFiber$' --output-on-failure
```

Python's standard-library entry-comparison tests are registered when Python is
found. Independent resin geometry checks additionally require Python with
Shapely 2 or newer, selected with
`-DFIBER_GEOMETRY_PYTHON=/absolute/path/to/python`.
See the [continuous-fiber model notes](models/continuous_fiber/README.md) and
[bracket fixture notes](tests/data/continuous_fiber/bracket1/README.md) for shared
regression inputs.

## License

See [LICENSE.txt](LICENSE.txt) for the GNU Affero General Public License v3.
Bundled dependencies retain their own license files.
