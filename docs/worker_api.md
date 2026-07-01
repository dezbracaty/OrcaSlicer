# libslicer Worker API and Protocol

This document defines the public API and socket protocol for the
`orcaslicer-worker` executable and its host-side client library.

The command-line worker and Unix domain socket worker are implemented in this
branch. The install package currently exposes the host-side worker client API
and the worker executable; it does not export the full in-process `libslic3r`
slicing engine as a stable install-tree SDK yet.

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
- `libslicer::orcaslicer_worker`: imported executable target for the installed
  `orcaslicer-worker` binary.
- `LIBSLICER_ORCASLICER_WORKER_EXECUTABLE`: absolute path to the installed
  worker executable.
- `LIBSLICER_WORKER_EXECUTABLE`: compatibility alias for the same executable
  path.

When the repository is embedded from source with `add_subdirectory`, the build
tree also provides `libslicer::libslicer` and `libslicer::worker_runtime` for
internal development and tests.

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
    "gcode": "./output.gcode",
    "artifacts_dir": "./artifacts"
  },
  "options": {
    "emit_preview": false,
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
  model/project, then applies the resolved config JSON.
- `project_embedded`: does not use `config.path`. This is accepted only with
  `input.type=orca_3mf_project`; the worker uses the config loaded from the
  OrcaSlicer 3MF project plus slicer defaults.

The worker does not resolve preset bundles or host application database state.
Hosts that do not submit `project_embedded` must submit a fully resolved config
JSON.

### File Ownership and Cleanup

The worker treats these files as caller-owned inputs and never deletes them:

- `request.json`
- `input.path`
- `config.path`

The worker keeps final output files:

- `output.gcode`
- non-empty files under `output.artifacts_dir`

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

The worker converts this JSON into `Slic3r::DynamicPrintConfig`.

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
  "path": "/path/to/job/output.gcode"
}
```

Known artifact kinds:

- `gcode`
- `stats`
- `preview`
- `warnings`
- `log`

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
  "code": "slice_failed",
  "message": "Slicing failed",
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
    std::filesystem::path path;
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
