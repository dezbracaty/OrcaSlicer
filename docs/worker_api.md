# libslicer Worker API and Protocol

This document defines the public API and socket protocol for the
`orcaslicer-worker` executable and its host-side client library.

The command-line worker and Unix domain socket worker are implemented in this
branch. The install package currently exposes the host-side worker client API
and the worker executable. The in-process `libslic3r` target is available when
embedding this repository with `add_subdirectory`; only the reviewed public
headers documented below should be treated as SDK surface.

## CMake Integration

After installing OrcaSlicer, external projects can consume the worker package
with CMake:

```cmake
find_package(libslicer CONFIG REQUIRED)

add_executable(host_app main.cpp)
target_link_libraries(host_app PRIVATE libslicer::worker_client)
```

The package exports:

- `libslicer::worker_client`: host-side process/socket client library.
- `libslicer::preview_protocol`: header-only preview/artifact protocol types.
- `libslicer::orcaslicer_worker`: imported executable target for the installed
  `orcaslicer-worker` binary.
- `LIBSLICER_ORCASLICER_WORKER_EXECUTABLE`: absolute path to the installed
  worker executable.
- `LIBSLICER_WORKER_EXECUTABLE`: compatibility alias for the same executable
  path.

When the repository is embedded from source with `add_subdirectory`, the build
tree also provides `libslicer::libslicer` and `libslicer::worker_runtime` for
internal development and tests.

The config SDK described in this document is part of `libslicer::libslicer`:

```cmake
add_subdirectory(path/to/OrcaSlicer libslicer-build)
target_link_libraries(host_app PRIVATE libslicer::libslicer)
```

## Public Orca Toolpath Types

The worker protocol does not invent a second set of preview enums. OrcaSlicer
and external consumers share the same public toolpath semantic definitions in:

```cpp
#include <libslic3r/OrcaToolpathTypes.hpp>
```

This header currently owns the common definitions for:

- `Slic3r::EMoveType`
- `Slic3r::EMovePathType`
- `Slic3r::ExtrusionRole`

Orca's shared coordinate types live in:

```cpp
#include <libslic3r/OrcaCoreTypes.hpp>
```

- `coord_t`
- `coordf_t`

Orca's shared vector/matrix aliases, including `Slic3r::Vec3f`, live in:

```cpp
#include <libslic3r/OrcaGeometryTypes.hpp>
```

Move records shared with Orca's G-code processor live in:

```cpp
#include <libslic3r/OrcaToolpathRecords.hpp>
```

This header currently owns:

- `Slic3r::ToolpathMoveVertex`

`Slic3r::GCodeProcessorResult::MoveVertex` is an alias of
`Slic3r::ToolpathMoveVertex`. External projects can therefore reason about
toolpath moves with the same semantic names, values, and record fields used by
Orca's G-code processor and preview pipeline.

The worker preview protocol exposes aliases to these exact types:

```cpp
#include <type_traits>

#include <libslicer_worker/PreviewProtocol.hpp>

static_assert(std::is_same_v<libslicer::worker::preview::MoveType,
                             Slic3r::EMoveType>);
static_assert(std::is_same_v<libslicer::worker::preview::PathKind,
                             Slic3r::EMovePathType>);
static_assert(std::is_same_v<libslicer::worker::preview::ExtrusionRole,
                             Slic3r::ExtrusionRole>);
```

This is the intended boundary: public, low-dependency Orca semantic types are
shared; heavy internal objects such as `Slic3r::Print`, `Slic3r::Model`, and
`Slic3r::GCodeProcessorResult` remain implementation details until they are
separately reviewed and stabilized.

Binary preview artifacts use explicitly named wire records from
`libslicer_worker/PreviewBinary.hpp`, such as
`libslicer::worker::preview::WireMoveRecord`. Wire records are POD transport
types with `sizeof`, key `offsetof`, and trivially-copyable compile-time
checks. Use helper functions such as `to_wire_vec3f()` and `to_orca_vec3f()`
when converting between wire vectors and Orca's Eigen-backed `Slic3r::Vec3f`.
The numeric values of the shared move/path/role enums are also part of
`schema_version == 2`; adding, removing, or reordering those enum values must
bump the preview schema version and update the protocol assertions.

## Preview Artifact Contract

The preview artifact is a strict binary artifact with extension `.orcapv`,
schema `orca.toolpath_preview`, and format
`orca-toolpath-preview-binary-v2` (`schema_version == 2`).

Wire ids are 0-based:

| Field | Meaning |
| --- | --- |
| `WireMoveRecord.filament_id` | Active filament for the move. |
| `WireMoveRecord.tool_id` | Physical tool/nozzle used for the move. |
| `WireFilamentRecord.id` | Filament id. |
| `WireFilamentRecord.tool_id` | Physical tool/nozzle selected for this filament. |
| `WireToolRecord.id` | Physical tool/nozzle id. |
| `WireToolRecord.filament_id` | Optional primary filament for that tool. It is not the authoritative mapping for multi-filament tools. |
| `WireMoveRecord.cp_color_id` | Key into the preview color table. |
| `WireMoveRecord.joint_angle_end_rad` | Signed miter turn angle (radians, 2D xy) at the move's end vertex vs. the next move that continues the same extrusion path; `0` at path breaks. Added in `schema_version == 2` (reuses a former `reserved_f32` slot; record size stays 216 bytes). See `docs/toolpath_preview_joint_angle.md`. |
| `WireColorRecord.id` | Exact color id referenced by moves. |

`filament_map` is normalized before preview publication and means:

```text
filament_id -> tool_id
```

The worker does not use `tool_id == filament_id` as a fallback. Missing,
out-of-range, or internally inconsistent mapping facts make preview production
fail with a preview mapping error.

Color records are serialized from Orca's typed preview color facts. The worker
does not infer color-change ids from custom G-code ordering. A move may set
`HasCpColor` only when its `cp_color_id` resolves to a `WireColorRecord`.

The public reader `PreviewArtifactStorage::view()` is strict. It rejects:

- unknown or duplicate sections
- missing required sections
- section ranges outside the file or overlapping other sections
- wrong record sizes or misaligned section offsets
- invalid layer move ranges
- move references to missing layers/tools/filaments/colors
- event references to missing moves
- invalid string table references

The preview producer validates records before writing, writes a same-directory
temporary file, validates that temporary file through the strict reader, then
renames it to the final path. The worker emits a preview artifact ready event
only after the final rename succeeds.

## Command Line

### One-Shot Slice

```bash
orcaslicer-worker slice --job /path/to/job/request.json
```

Optional arguments:

```bash
orcaslicer-worker slice \
  --job /path/to/job/request.json \
  --progress jsonl \
  --log-level info
```

Exit codes:

| Code | Meaning |
| ---: | --- |
| 0 | Success |
| 1 | Job failed |
| 2 | Invalid command line |
| 3 | Invalid request/protocol |
| 4 | Input or config file not found |
| 5 | Slicing failed |
| 6 | Cancelled |
| 70 | Internal error |

### Socket Server

```bash
orcaslicer-worker serve --socket /tmp/orcaslicer-worker.sock
```

The current implementation supports Unix domain sockets. Future Windows support
should use named pipes, and a future TCP fallback may bind to `127.0.0.1` with a
per-session auth token. If a future TCP mode uses an ephemeral port, the worker
should print the selected port as a JSON event on stdout:

```json
{"type":"listening","transport":"tcp","host":"127.0.0.1","port":49152}
```

## Job Request

`request.json` is the canonical job file.

```json
{
  "version": 1,
  "job_id": "job-1",
  "kind": "slice",
  "working_dir": ".",
  "resources_dir": "/absolute/path/to/resources",
  "data_dir": "./data",
  "input": {
    "type": "3mf",
    "path": "./input.3mf",
    "plate_index": 0
  },
  "config": {
    "type": "resolved_orca_json",
    "path": "./config.json"
  },
  "output": {
    "artifacts_dir": "./artifacts",
    "gcode": {
      "enabled": true,
      "required": true,
      "path": "./output.gcode"
    },
    "preview": {
      "enabled": false
    }
  },
  "options": {
    "overwrite": true,
    "keep_intermediate_files": false
  }
}
```

Supported `input.type` values:

- `stl`
- `3mf`: 3MF geometry/project input with an external resolved Orca config.
- `orca_3mf_project`: OrcaSlicer project 3MF that carries embedded project
  config.

`3mf` and `orca_3mf_project` intentionally use different loaders. Use `3mf`
for generic 3MF geometry plus an external resolved config. Use
`orca_3mf_project` for Orca/Bambu project files that rely on Orca-specific 3MF
extensions such as project config, plates, object paths, and embedded metadata.

Supported `config.type` values:

- `resolved_orca_json`: requires `config.path`. The worker loads the input
  model/project, then applies the resolved config JSON through the public
  config SDK loader and validator.
- `project_embedded`: does not use `config.path`. This is accepted only with
  `input.type=orca_3mf_project`; the worker uses the config loaded from the
  OrcaSlicer 3MF project plus slicer defaults.

The worker does not resolve preset bundles or host application database state.
Hosts that do not submit `project_embedded` must submit a fully resolved config
JSON.

### Output Request

`output.gcode` accepts either the legacy string form or the object form.

Legacy string form:

```json
{
  "output": {
    "gcode": "./output.gcode",
    "artifacts_dir": "./artifacts"
  }
}
```

This is equivalent to:

```json
{
  "output": {
    "artifacts_dir": "./artifacts",
    "gcode": {
      "enabled": true,
      "required": true,
      "path": "./output.gcode"
    },
    "preview": {
      "enabled": false
    }
  }
}
```

`output.gcode` object fields:

- `enabled`: whether to publish a public G-code artifact.
- `required`: whether G-code publishing failure fails the job. Defaults to
  `enabled`.
- `path`: output path. Relative paths resolve against `working_dir`.

`output.preview` object fields:

- `enabled`: whether to publish a public preview artifact.
- `required`: whether preview publishing failure fails the job. Defaults to
  `enabled`.
- `path`: preview artifact path. Relative paths resolve against
  `output.artifacts_dir`.
- `format`: accepts only `orca-toolpath-preview-binary-v2`.
- `publish`: v1 accepts only `final`.
- `chunk_records`: reserved for future chunked mode; ignored by final mode
  after type/range validation.

At least one public artifact must be enabled. If both G-code and preview are
disabled, the worker rejects the request with `no_outputs_requested`.

New object forms are strictly typed. For example, `output.preview.required=[]`
is rejected instead of coerced.

Path containment rules:

- `artifacts_dir` may be absolute or relative to `working_dir`.
- `gcode.path` may be absolute or relative to `working_dir`.
- `preview.path` may be absolute or relative to `artifacts_dir`.
- Relative paths must not escape their base with `..` after normalization.
- Preview output must remain inside `artifacts_dir`.
- Final artifacts are published only after the producer has finalized the file
  and the worker has emitted an artifact ready event.

### File Ownership and Cleanup

The worker treats these files as caller-owned inputs and never deletes them:

- `request.json`
- `input.path`
- `config.path`

The worker keeps final output files:

- `output.gcode`
- requested preview artifacts under `output.artifacts_dir`
- other non-empty files under `output.artifacts_dir`

By default, `options.keep_intermediate_files` is `false`. In that mode the worker
cleans worker-owned scratch paths after the job finishes:

- `data_dir` is removed when it is inside `working_dir` and is not the output
  directory.
- `output.artifacts_dir` is removed only when it is empty, inside `working_dir`,
  and is not the output directory.

Set `keep_intermediate_files` to `true` to preserve those scratch directories for
debugging.

## Resolved Config JSON

The resolved config file is a JSON object mapping Orca config keys to typed
values:

```json
{
  "printer_technology": "FFF",
  "bed_shape": [[0, 0], [220, 0], [220, 220], [0, 220]],
  "nozzle_diameter": [0.4],
  "filament_diameter": [1.75],
  "layer_height": 0.2,
  "first_layer_height": 0.2,
  "support_material": false
}
```

The worker converts this JSON into `Slic3r::DynamicPrintConfig` through
`Slic3r::libslicer::ResolvedConfig`, then runs
`Slic3r::libslicer::validate_resolved_config()` before `Print::apply()`.

Rules:

- Unknown keys are warnings by default.
- Missing required keys are errors.
- Values must be schema-coerced before slicing.
- Multi-material arrays must preserve slot order.
- Enum values may be submitted as Orca enum strings or integer enum values.
- Nullable per-filament/per-extruder arrays may use `"nil"` only when the Orca
  config schema marks that option as nullable.
- `flush_volumes_matrix`, `flush_volumes_vector`, and `flush_multiplier` are
  normalized to the current `filament_colour`/`filament_diameter` count and
  `nozzle_diameter` count before slicing, matching Orca's profile resize
  behavior when filament slots change.
- Layer G-code keys are the Orca config keys `before_layer_change_gcode` and
  `layer_change_gcode`. `layer_gcode` is not a worker request key.
- The config must be resolved before submission; the worker should not depend on
  host application DB state.

## Config SDK API

The public config SDK lives in:

```cpp
#include <libslic3r/ConfigSDK.hpp>
```

The API namespace is `Slic3r::libslicer`.

### Schema

Hosts can enumerate Orca config definitions without depending directly on
`ConfigOptionDef`:

```cpp
std::vector<Slic3r::libslicer::ConfigDefinition> defs =
    Slic3r::libslicer::get_config_definitions();

Slic3r::libslicer::ConfigDefinition printer_model =
    Slic3r::libslicer::get_config_definition("printer_model");
```

`ConfigDefinition` exposes:

- `key`
- `type`
- `label`
- `enum_options`
- `unit`
- `min`
- `max`
- `default_value`
- `scope`
- `cardinality`

The first implementation maps Orca's internal schema and provides explicit
scope/cardinality for worker-critical keys such as printer identity, process
identity, filament arrays, physical extruder arrays, variant lookup arrays,
plate-scoped wipe tower coordinates, and layer-change G-code fields. Unknown or
not-yet-classified fields are reported as `Unknown` rather than guessed.

### ResolvedConfig

`ResolvedConfig` is the public facade for already-resolved Orca FFF config:

```cpp
std::vector<Slic3r::libslicer::ConfigValidationIssue> load_issues;

Slic3r::libslicer::ResolvedConfig config =
    Slic3r::libslicer::ResolvedConfig::load_json_file("config.json",
                                                      &load_issues);

if (Slic3r::libslicer::has_config_errors(load_issues)) {
    // Show load_issues to the user and do not slice.
}
```

Supported operations:

- `ResolvedConfig::from_json(text, issues)`
- `ResolvedConfig::load_json_file(path, issues)`
- `ResolvedConfig::apply_json(text, issues)`
- `ResolvedConfig::apply_json_file(path, issues)`
- `ResolvedConfig::to_json()`
- `ResolvedConfig::dynamic_config()`

`dynamic_config()` is an escape hatch for advanced integrations and worker
internals. Normal host code should prefer `ResolvedConfig` plus schema and
validation APIs.

### Validation

Before slicing, callers must validate the resolved config:

```cpp
std::vector<Slic3r::libslicer::ConfigValidationIssue> issues =
    Slic3r::libslicer::validate_resolved_config(config, plate_index);

if (Slic3r::libslicer::has_config_errors(issues)) {
    // Do not call Print::apply().
}
```

The validator currently checks the worker-critical resolved config contract:

- required printer/process identity fields
- filament-scoped vector lengths
- positive 1-based `filament_map` values
- physical extruder arrays used by the selected filament map
- `filament_self_index` and `filament_extruder_variant`
- extruder variant lookup consistency
- support, support interface, and wipe tower filament ids
- plate-scoped `wipe_tower_x` and `wipe_tower_y`
- required layer-change G-code fields

Validation issues use stable `code`, `field`, `message`, and `severity`
members. Unknown JSON keys are warnings; schema conversion failures and invalid
resolved config contracts are errors.

### BBL Printer Semantics

The resolved config must preserve Orca's BBL printer identity. `Print::apply()`
derives `Print::is_BBL_printer()` from the final config before validation; the
current fallback source is `printer_model` beginning with `Bambu Lab`.

For BBL Marlin profiles using relative extruder addressing, the worker must not
require callers to inject `G92 E0` into `before_layer_change_gcode` or
`layer_change_gcode`. Non-BBL Marlin profiles still require an exact uppercase
`G92 E0` reset in relative-E mode.

### Preset Resolution Status

The SDK reserves the following types for the preset-selection resolver:

- `ConfigResolutionRequest`
- `ConfigResolutionResult`
- `resolve_fff_config(request)`

This entry point is intentionally present but not implemented yet. The current
implementation returns an error issue with code `preset_resolution_unsupported`.
Hosts that need slicing today must submit either:

- `config.type=project_embedded`, letting the worker use config embedded in an
  Orca project 3MF, or
- `config.type=resolved_orca_json`, where the host provides a fully resolved
  config JSON and may use `ResolvedConfig` plus `validate_resolved_config()` for
  preflight checks.

When the preset resolver is implemented, worker `preset_selection` and external
host APIs must call this same `resolve_fff_config()` path rather than maintaining
separate resolver logic.

## JSON Lines Event Stream

One-shot mode writes events to stdout when `--progress jsonl` is enabled.
Socket mode sends the same events over the socket.

Every message is one UTF-8 JSON object followed by `\n`.

### Hello

Client to server:

```json
{"type":"hello","protocol":1,"client":"gplatform","token":"optional"}
```

Server to client:

```json
{
  "type": "hello",
  "protocol": 1,
  "server": "orcaslicer-worker",
  "version": "0.1.0",
  "capabilities": ["slice", "cancel", "artifacts"]
}
```

### Start Job

Client to server:

```json
{"type":"start_job","job_id":"job-1","request_path":"/path/to/job/request.json"}
```

Server to client:

```json
{"type":"accepted","job_id":"job-1"}
```

### Progress

Server to client:

```json
{
  "type": "progress",
  "job_id": "job-1",
  "percent": 42,
  "stage": "support",
  "message": "Generating support"
}
```

`percent` is an integer from 0 to 100. It may stay unchanged between stage
updates.

Recommended stages:

- `initializing`
- `loading_input`
- `loading_config`
- `preparing_model`
- `slicing`
- `support`
- `gcode`
- `postprocess`
- `writing_artifacts`
- `done`

### Artifact

Server to client:

```json
{
  "type": "artifact",
  "job_id": "job-1",
  "kind": "gcode",
  "phase": "ready",
  "path": "/path/to/job/output.gcode",
  "complete": true
}
```

Known artifact kinds:

- `gcode`
- `stats`
- `preview`
- `warnings`
- `log`

Preview ready event:

```json
{
  "type": "artifact",
  "job_id": "job-1",
  "kind": "preview",
  "phase": "ready",
  "schema": "orca.toolpath_preview",
  "format": "orca-toolpath-preview-binary-v2",
  "path": "/path/to/job/artifacts/preview.orcapv",
  "complete": true
}
```

Hosts must treat artifact ready events as the consumption boundary. A
`result.success=true` event means the job completed under the requested
`required` policy; it is not a substitute for an artifact ready event.

### Warning

Server to client:

```json
{
  "type": "warning",
  "job_id": "job-1",
  "code": "unknown_config_key",
  "message": "Unknown config key: foo"
}
```

### Error

Server to client:

```json
{
  "type": "error",
  "job_id": "job-1",
  "code": "slice_processing_failed",
  "message": "Slicing or internal toolpath processing failed",
  "recoverable": false
}
```

### Result

Server to client:

```json
{
  "type": "result",
  "job_id": "job-1",
  "success": true,
  "gcode": "/path/to/job/output.gcode",
  "elapsed_ms": 12345
}
```

Failure result:

```json
{
  "type": "result",
  "job_id": "job-1",
  "success": false,
  "code": "cancelled",
  "message": "Job was cancelled",
  "elapsed_ms": 2100
}
```

### Cancel

Client to server:

```json
{"type":"cancel","job_id":"job-1"}
```

Server to client:

```json
{"type":"cancel_accepted","job_id":"job-1"}
```

The job is terminal only after a `result` event.

### Stop Server

Client to server:

```json
{"type":"stop"}
```

Server to client:

```json
{"type":"stopping"}
```

The worker should reject `stop` while a job is active unless `force` is true:

```json
{"type":"stop","force":true}
```

## Host-Side C++ API

The host-side API belongs in `libslicer::worker_client`.

```cpp
namespace libslicer::worker {

struct WorkerOptions {
    std::filesystem::path executable_path;
    std::filesystem::path working_dir;
    std::filesystem::path socket_path;
    std::chrono::milliseconds startup_timeout{5000};
};

struct SliceJob {
    std::string job_id;
    std::filesystem::path request_path;
};

struct WorkerEvent {
    enum class Type {
        Hello,
        Accepted,
        Progress,
        Artifact,
        Warning,
        Error,
        Result,
        Stopping
    };

    Type type;
    std::string job_id;
    int percent = -1;
    std::string stage;
    std::string code;
    std::string message;
    std::string kind;
    std::string phase;
    std::string schema;
    std::string format;
    std::filesystem::path path;
    bool complete = false;
    bool success = false;
};

class WorkerClient {
public:
    using EventCallback = std::function<void(const WorkerEvent&)>;

    explicit WorkerClient(EventCallback on_event);
    ~WorkerClient();

    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;
    WorkerClient(WorkerClient&& other) noexcept;
    WorkerClient& operator=(WorkerClient&& other) noexcept;

    bool start(const WorkerOptions& options);
    bool connect();
    bool submit(const SliceJob& job);
    bool cancel(const std::string& job_id);
    bool stop();
    void kill();

    bool running() const;
    std::string last_error() const;
};

} // namespace libslicer::worker
```

API rules:

- `WorkerClient` is move-only because it owns a process, socket, and I/O thread.
  Store it by value, `std::unique_ptr`, or another single-owner wrapper; do not
  copy it between adapters.
- `start()` starts the external `orcaslicer-worker serve` process.
- `connect()` connects to the worker socket and performs `hello`.
- `submit()` sends `start_job`.
- Event callbacks are invoked from the client's I/O thread unless a host
  adapter marshals them to another thread.
- Host adapters must not update UI state directly from the callback. Marshal the
  event to the UI/main thread first.
- Do not destroy the `WorkerClient` from inside its own callback. Schedule
  shutdown onto the owning thread instead.
- `stop()` requests graceful server shutdown.
- `kill()` terminates the external process and is safe to call during cleanup.
- For artifact events, `kind`, `phase`, `schema`, `format`, `path`, and
  `complete` carry the publication contract. Hosts should not consume files
  before `phase=="ready"` and `complete==true`.

Qt applications can wrap this API with signal dispatch, but the core client
should stay Qt-free.

## Worker Runtime API

The server-side runtime belongs in `libslicer::worker_runtime`.

```cpp
namespace libslicer::worker {

struct ServerOptions {
    std::filesystem::path socket_path;
    std::optional<std::string> tcp_host;
    uint16_t tcp_port = 0;
    std::optional<std::string> auth_token;
};

class WorkerServer {
public:
    explicit WorkerServer(ServerOptions options);
    int run();
    void request_stop();
};

int run_slice_job_from_request(const std::filesystem::path& request_path,
                               EventSink& events,
                               CancellationToken& cancellation);

} // namespace libslicer::worker
```

`run_slice_job_from_request()` is shared by one-shot CLI mode and socket server
mode.

## GPlatform Adapter Sketch

GPlatform should add a small adapter around `WorkerClient`:

```cpp
class OrcaSlicerWorkerAdapter : public QObject {
    Q_OBJECT

public:
    void startWorker();
    void submitSliceJob(const QString& requestPath);
    void cancelCurrentJob();

signals:
    void progressChanged(int percent, QString stage, QString message);
    void sliceCompleted(QString gcodePath);
    void sliceFailed(QString code, QString message);
};
```

`SlicingHandler::performSlice()` should eventually:

1. Export the selected model/project into a job directory.
2. Write resolved Orca config JSON.
3. Submit `request.json` through the adapter.
4. On success, call the existing G-code import path.

## Compatibility Policy

The protocol has an explicit integer version.

Rules:

- Increment `protocol` for breaking message changes.
- Add optional fields without breaking compatibility.
- Clients must ignore unknown event fields.
- Servers must reject unsupported protocol versions with
  `unsupported_protocol_version`.
- Job request `version` follows the same rule. The current implementation
  rejects any request version other than `1`.
- Socket `start_job` is rejected with `bad_protocol` until a valid `hello`
  handshake has completed.

Stable worker error codes include:

- `bad_protocol`
- `unsupported_protocol_version`
- `invalid_request`
- `output_request_invalid`
- `preview_request_invalid`
- `no_outputs_requested`
- `input_not_found`
- `invalid_config`
- `model_load_failed`
- `slice_failed`
- `slice_processing_failed`
- `gcode_publish_failed`
- `preview_mapping_invalid`
- `preview_color_table_invalid`
- `preview_artifact_invalid`
- `preview_write_failed`
- `cancelled`
- `internal_error`

## Test Plan

Required initial tests:

- `orcaslicer-worker slice --job ...` succeeds on a minimal STL.
- One-shot mode writes a non-empty G-code file.
- Socket `hello` succeeds.
- Socket `hello` rejects unsupported protocol versions.
- Socket `start_job` emits `accepted`, `progress`, `artifact`, and `result`.
- `cancel` produces a terminal cancelled result.
- Unsupported job request versions emit `unsupported_protocol_version`.
- Client disconnect after `hello` does not terminate the worker process.
- Invalid request emits a structured error.
- Unknown config keys produce warnings, not crashes.
- The worker slices an STL input with resolved config JSON.
- The worker slices a 3MF input with resolved config JSON.
- Host CMake smoke tests prove `WorkerClient` is move-only and installable with
  both `find_package(libslicer CONFIG REQUIRED)` and source-tree
  `add_subdirectory`.
