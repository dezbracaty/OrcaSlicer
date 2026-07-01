# libslicer Worker Architecture

This document describes the planned worker/runtime layer around `libslicer`.
It is an architecture target, not a statement that the worker already exists.

## Goals

The worker layer should let host applications use OrcaSlicer slicing without
embedding the slicing engine directly into their UI process.

Required use cases:

- Run one slicing job from a command line.
- Start a long-lived worker process from a host application.
- Submit slicing jobs over a socket.
- Report progress, warnings, errors, cancellation, and output artifacts.
- Keep the core `libslicer` library usable from CMake without forcing socket or
  process-management dependencies on every consumer.

## Non-Goals

- Do not move socket, process, or host-application code into `src/libslic3r`.
- Do not make the UI process own slicer global state such as resource/data
  directories.
- Do not stream large mesh, 3MF, preview, or G-code payloads through the socket
  in the first version.
- Do not require GPlatform-specific DB or Qt types in the worker.

## Component Layout

```text
src/libslic3r/
  Core slicing engine.
  No process, socket, Qt, or host-application ownership.

src/libslicer_worker/
  Worker runtime and protocol layer.
  Owns job model, process client API, socket server/client, cancellation,
  progress events, and artifact reporting.

src/orcaslicer_worker/
  Executable entry point.
  Links libslicer and libslicer_worker.
  Supports one-shot CLI mode and socket server mode.

tests/worker/
  CLI, socket protocol, cancellation, and artifact tests.
```

The public CMake targets should be:

```cmake
libslicer::libslicer          # existing core slicing target
libslicer::worker_runtime     # worker protocol/server support
libslicer::worker_client      # host-side process/socket client
orcaslicer-worker             # executable
```

`libslicer::worker_client` is the API a host application such as GPlatform would
link. It starts the external worker process and communicates with it over a
socket. It must not run slicing inside the host process.

## Runtime Modes

The same executable should support two modes.

### One-Shot CLI

```bash
orcaslicer-worker slice --job /path/to/job/request.json
```

The process loads one request, runs one slicing job, writes artifacts to the job
directory, emits progress/result events to stdout, and exits.

This mode is used by:

- Shell users.
- CI tests.
- Debugging.
- Host applications that want the simplest integration boundary.

### Socket Server

```bash
orcaslicer-worker serve --socket /tmp/orcaslicer-worker.sock
```

The process opens a socket, receives protocol messages, runs jobs, reports
events, and remains alive until the client stops it or the process exits.

This mode is used by:

- GUI applications that want progress and cancellation.
- Applications that submit multiple jobs.
- Integrations that need intermediate artifacts while slicing is running.

## Process Boundary

The worker process boundary is intentional:

- A slicer crash does not crash the host UI process.
- `libslicer` global state stays isolated in one process.
- Dependency and symbol conflicts are contained in the worker.
- The worker can be upgraded or replaced without rebuilding the host app if the
  protocol remains compatible.

The host application should own only:

- Worker executable discovery.
- Job directory creation.
- Input file export.
- Worker process lifetime through `libslicer::worker_client`.
- Final G-code import or upload.

The worker process should own:

- `Slic3r::set_resources_dir`.
- `Slic3r::set_data_dir`.
- Config loading and validation.
- Model/project loading.
- Slicing and G-code export.
- Progress and artifact events.

## Data Flow

```text
Host application
  |
  | create job directory
  | write request.json, config.json, input.stl or input.3mf
  v
libslicer worker client
  |
  | start process if needed
  | connect socket or run one-shot CLI
  v
orcaslicer-worker
  |
  | load request
  | initialize resources/data dir
  | load model/project
  | resolve config
  | slice
  | write output.gcode and optional artifacts
  v
Host application
  |
  | import output.gcode
  v
Preview / upload / printer workflow
```

Large payloads should be exchanged through files in the job directory. The
socket carries control and metadata only.

## Job Directory

A job directory is the file-protocol contract between the host and worker.

```text
job/
  request.json
  input.3mf
  input.stl
  config.json
  output.gcode
  artifacts/
    stats.json
    preview.json
    warnings.json
```

Only `request.json` is mandatory. The request points to the actual input,
config, output, and artifact paths. Paths may be absolute or relative to the job
directory.

Ownership is split deliberately:

- The host owns `request.json`, `input.*`, and `config.json`.
- The worker owns scratch paths such as `data/`.
- `output.gcode` and non-empty artifacts are final outputs and are preserved.
- Empty scratch artifact directories are removed by default.

`options.keep_intermediate_files=true` disables worker scratch cleanup for
debugging.

## Initial Slicing Scope

Phase 1 should support:

- One input STL or one input 3MF.
- One output G-code file.
- Resolved full print config JSON.
- Resource and data directory initialization.
- Progress events.
- Cancellation request.
- Structured error reporting.
- CLI one-shot tests.
- Socket smoke tests.

Phase 2 should add:

- Multi-model jobs.
- Plate selection for 3MF.
- Multi-material and multi-nozzle validation through worker tests.
- Intermediate preview/stat artifacts.
- Stronger protocol version negotiation.

Phase 3 should add:

- Long-lived worker pooling if startup cost is proven significant.
- Binary artifact format for dense preview data.
- Optional protobuf transport if JSON Lines becomes insufficient.

## Threading Model

One-shot mode can run the slicing job on the main worker thread.

Socket mode should separate:

- Socket I/O thread.
- Job execution thread.
- Cancellation token shared between protocol and slicer job execution.

The first socket implementation should run at most one active slicing job per
worker process. That keeps global slicer state, resource paths, temporary files,
and progress reporting simple. Parallel slicing can be achieved by starting
multiple worker processes.

## Cancellation

Cancellation is cooperative:

- The client sends `cancel`.
- The worker marks the job cancellation token.
- The slicer job checks the token at known progress points.
- The worker emits a terminal `result` event with `success=false` and
  `code="cancelled"`.

The host may kill the worker process if graceful cancellation times out.

## Error Handling

Errors should be structured and stable:

```json
{
  "type": "error",
  "job_id": "job-1",
  "code": "invalid_config",
  "message": "Missing required option: nozzle_diameter",
  "recoverable": false
}
```

Recommended error codes:

- `bad_protocol`
- `unsupported_protocol_version`
- `invalid_request`
- `input_not_found`
- `invalid_config`
- `model_load_failed`
- `slice_failed`
- `gcode_export_failed`
- `cancelled`
- `internal_error`

## Security

The socket server is local-only by default.

Recommended defaults:

- macOS/Linux: Unix domain socket.
- Windows: named pipe.
- Optional fallback: `127.0.0.1` TCP with a random auth token.
- Reject remote TCP by default.
- Require a per-session token for TCP transport.
- Resolve and validate all paths before opening them.

## GPlatform Integration Boundary

GPlatform should not call `Slic3r::Print` directly.

Recommended integration:

```text
GPlatform SlicingHandler
  exports model/project to job directory
  asks OrcaPresetSessionProvider for resolved config
  calls libslicer::worker_client
  receives progress/result events
  imports output.gcode through existing GCodeImportHandler
```

This keeps the current GPlatform slicing preview path intact while moving real
slicing into an isolated worker process.
