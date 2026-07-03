# Orca Slice Artifact Worker Design

Status: implementation in progress.

本文档重新定义 `orcaslicer-worker` 的输出模型：worker 不应该被设计成“只输出 G-code 的程序”，而应该是 **按需 slice artifact producer**。G-code、preview artifact、未来其他 artifact 都应该是独立可选输出。

目标是让 GPlatform 这类宿主程序可以通过独立 worker 进程获得 OrcaSlicer 切片结果和高保真预览数据，而不是同进程链接 Orca slicer，也不是解析 `.gcode`。

## Core Decision

G-code 和 preview 是两个独立 artifact。

宿主程序可以请求：

- 只输出 G-code
- 只输出 preview
- 同时输出 G-code 和 preview

如果两者都不请求，worker 应拒绝该 job。

preview 的数据来源仍然必须是 Orca 内部切片结果，不能从 `.gcode` 反向解析。当前 Orca 管线里 `Slic3r::GCodeProcessorResult` 是在 `Print::export_gcode()` 路径中产生的，因此 preview-only 模式第一版可以使用 worker-owned 临时 G-code sink 来驱动 Orca 生成 `GCodeProcessorResult`，但这个临时文件不是 public artifact，必须自动清理。

## Current Implementation Baseline

本文档定义 worker artifact 输出架构。当前实现已经覆盖第一版 final preview artifact 的主路径：

- worker 支持 legacy G-code output。
- worker 支持 preview-only output，并使用 worker-owned temporary G-code sink 取得 `GCodeProcessorResult`。
- worker 支持 G-code + preview 同时输出。
- public preview headers 已补齐 `WireColorRecord`、`WireSectionType::Colors`、`WireSectionType::StringTable`、`MoveFlags::HasTool`。
- `WorkerEvent` 已增加并序列化 `phase`、`schema`、`format`、`complete`、`section`、`offset`、`count`、`record_size` 字段。
- final `.orcapv` 会写入同目录 `.tmp`、校验后 rename，失败时清理 `.tmp`。
- worker tests 覆盖 legacy G-code、preview-only、G-code + preview、event schema/format/complete、二进制 header/section/metadata。

仍未完成或不能承诺的范围：

- realtime preview chunk streaming。
- shared memory data plane。
- move 级 object/instance ownership。
- 独立 `libslicer::preview_reader` target。
- Orca GUI 100% 视觉一致；需要 canonical fixtures 覆盖 color-change、support、多工具、多耗材后才能冻结 schema v1。

## Goals

- worker 作为独立进程运行 Orca slicing，保护宿主进程免受 Orca 崩溃、全局状态、资源生命周期影响。
- 输出 artifact 按需生成，不强制 G-code 和 preview 绑定。
- preview artifact 使用公开 wire schema，宿主可以读取 typed records。
- preview 使用 `GCodeProcessorResult` 作为唯一真实数据源，不解析生成后的 G-code。
- 第一版只做 final preview artifact，不做实时 chunk。
- artifact event 明确告诉宿主哪些输出已经 ready。
- preview-only 模式不向宿主暴露 G-code。

## Non-Goals

- 第一版不做 realtime preview chunk streaming。
- 第一版不做 shared memory data plane。
- 第一版不要求宿主同进程链接 `libslicer::libslicer`。
- 第一版不承诺 move 级 object/instance ownership，除非能从 Orca 内部可靠证明。
- 第一版不承诺 Orca GUI 100% 视觉一致，直到 canonical fixtures 验证完成。
- 不把 GPlatform 专用逻辑写进 worker。

## Request Model

推荐新 request shape：

```json
{
  "version": 1,
  "kind": "slice",
  "job_id": "job-1",
  "working_dir": "./job",
  "resources_dir": "/path/to/resources",
  "data_dir": "./job/data",
  "input": {
    "type": "orca_3mf_project",
    "path": "./project.3mf",
    "plate_index": 0
  },
  "config": {
    "type": "project_embedded"
  },
  "output": {
    "artifacts_dir": "./artifacts",
    "gcode": {
      "enabled": false,
      "required": false,
      "path": "./output.gcode"
    },
    "preview": {
      "enabled": true,
      "required": true,
      "format": "orca-toolpath-preview-binary-v1",
      "publish": "final",
      "path": "preview.orcapv"
    }
  },
  "options": {
    "overwrite": true
  }
}
```

### Backward Compatibility

旧格式继续支持：

```json
{
  "output": {
    "gcode": "./output.gcode",
    "artifacts_dir": "./artifacts"
  }
}
```

解释为：

```json
{
  "output": {
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

这样不会破坏已有 worker 调用，同时新集成可以明确表达 preview-only。

## Output Semantics

### G-code Output

`output.gcode` 字段支持 string 或 object。

Object 字段：

- `enabled`: 是否输出 public G-code artifact。
- `required`: G-code 写入失败时是否让 job 失败。默认等于 `enabled`。
- `path`: 输出路径。相对路径以 `working_dir` 为基准。

如果 `gcode.enabled=false`，worker 不发 `kind=gcode` artifact event。

### Preview Output

`output.preview` 字段支持 object。

字段：

- `enabled`: 是否输出 public preview artifact。
- `required`: preview 失败时是否让 job 失败。默认等于 `enabled`。
- `format`: 第一版只接受 `orca-toolpath-preview-binary-v1`。
- `publish`: 第一版只接受 `final`。`chunked` 必须拒绝，直到实时预览完成。
- `path`: preview artifact 路径。相对路径以 `artifacts_dir` 为基准。
- `chunk_records`: 预留给未来 chunked 模式；第一版只做类型和正数校验。

### Output Validation

worker 必须拒绝：

- `output` 不是 object。
- `output.gcode` 既不是 string 也不是 object。
- `output.preview` 不是 object。
- `gcode.enabled=false` 且 `preview.enabled=false`。
- preview format 不支持。
- preview publish mode 不支持。
- preview path 为空且 preview enabled。
- gcode path 为空且 gcode enabled。

建议错误码：

- `output_request_invalid`
- `preview_request_invalid`
- `no_outputs_requested`

### Path Safety

所有 public artifact path 必须在解析 request 时规范化并验证。

规则：

- `output.artifacts_dir` 可以是绝对路径，也可以相对 `working_dir`。
- `output.gcode.path` 可以是绝对路径，也可以相对 `working_dir`。
- `output.preview.path` 可以是绝对路径，也可以相对 `artifacts_dir`。
- 相对路径规范化后不得通过 `..` 逃出对应 base directory。
- preview path 如果是相对路径，最终路径必须 containment 在 `artifacts_dir` 下。
- worker-owned temporary preview path 必须与 final preview path 在同一目录，文件名追加 `.tmp` 或使用同目录唯一临时名。
- finalization 必须在同一目录内 rename，避免跨文件系统 copy/rename 语义不一致。
- worker 不得删除 containment 校验之外的路径。
- cleanup 只能删除 worker 创建的 transient files，不能递归清理 caller-owned directories。

如果路径违反这些规则，worker 应以 `output_request_invalid` 或 `preview_request_invalid` 拒绝请求。

## Internal Pipeline

worker 内部流程应按“生成切片结果”和“发布 artifacts”分离。

```text
request
  -> parse requested artifacts
  -> load model/config
  -> print.process()
  -> produce Orca processing result
  -> artifact producers consume result
  -> publish artifact events
  -> result event
```

### Important Constraint: GCodeProcessorResult Source

Orca 当前的 `GCodeProcessorResult` 由 `Print::export_gcode()` 产生。也就是说，即使宿主只请求 preview，worker 仍可能需要调用 export path 来获得 preview 所需的 move/layer/role/color 数据。

这不代表必须向宿主输出 G-code。preview-only 第一版可以这样实现：

```text
gcode.enabled=false
preview.enabled=true

worker writes temporary internal G-code:
  data_dir/internal/preview-source.gcode.tmp

worker obtains GCodeProcessorResult
worker writes artifacts/preview.orcapv
worker deletes temporary internal G-code
worker emits only kind=preview artifact event
```

临时 G-code 是 worker-owned intermediate，不是 public output：

- 不发 artifact event。
- 成功后删除，除非 `keep_intermediate_files=true`。
- 失败/取消时删除。
- 不允许宿主依赖它的路径或内容。

长期更优方案是从 Orca core 拆出“不写 public G-code 文件但能产生 `GCodeProcessorResult`”的 API；这属于后续 core refactor，不阻塞第一版 worker artifact 设计。

## Artifact Events

preview ready event：

```json
{
  "type": "artifact",
  "job_id": "job-1",
  "kind": "preview",
  "phase": "ready",
  "schema": "orca.toolpath_preview",
  "format": "orca-toolpath-preview-binary-v1",
  "path": "/absolute/path/to/job/artifacts/preview.orcapv",
  "complete": true
}
```

gcode ready event：

```json
{
  "type": "artifact",
  "job_id": "job-1",
  "kind": "gcode",
  "phase": "ready",
  "path": "/absolute/path/to/job/output.gcode",
  "complete": true
}
```

`WorkerEvent` 需要扩展：

- `phase`
- `schema`
- `format`
- `complete`
- `section`
- `offset`
- `count`
- `record_size`

字段语义：

- `schema`: preview 语义 schema，例如 `orca.toolpath_preview`。
- `format`: preview 二进制格式，例如 `orca-toolpath-preview-binary-v1`。
- `complete`: final artifact 必须为 `true`。
- `section/offset/count/record_size`: 第一版只为未来 chunked preview 预留；final-only preview 不需要发送。

`schema` 和 `format` 不得混用。`schema` 对应 `PreviewTypes.hpp::schema_name`，`format` 对应 `PreviewTypes.hpp::binary_format_name`。

### Result Event Semantics

`result.success=true` 只表示 job 按请求的 required 规则完成，不代表某个 artifact 已经 ready。

宿主程序必须以 artifact event 为准：

- preview 可消费条件：收到 `type=artifact`、`kind=preview`、`phase=ready`、`complete=true`。
- G-code 可消费条件：收到 `type=artifact`、`kind=gcode`、`phase=ready`、`complete=true`。
- 如果 preview 是 optional，job 可以 `result.success=true` 但没有 preview ready event。
- 如果 preview 是 required，preview 失败必须让 `result.success=false`，并提供 preview 错误码。

第一版不要求 `result` event 携带 artifacts map；如果未来增加 artifacts summary，也只能作为辅助信息，不能替代 artifact ready event。

## Preview Artifact Binary Shape

文件扩展名：`.orcapv`

文件结构：

```text
WireFileHeader
WireSectionHeader[section_count]
MetadataJson bytes
WireLayerRecord[]
WireToolRecord[]
WireFilamentRecord[]
WireColorRecord[]
WireObjectRecord[]
WireInstanceRecord[]
WireMoveRecord[]
WireEventRecord[]
StringTable bytes
```

所有 fixed-size records 都必须：

- standard-layout
- trivially-copyable
- 使用固定宽度整数
- 不包含 pointer、`size_t`、`std::string`、`std::vector`
- 有 `sizeof` static_assert
- 有关键 `offsetof` static_assert

第一版 sections：

| Section | Required | Purpose |
| --- | --- | --- |
| MetadataJson | yes | schema、producer、source、coordinate、section summary |
| Layers | yes | layer 到 move range 的索引 |
| Tools | yes | nozzle/tool 信息 |
| Filaments | yes | filament 颜色、直径、成本 |
| Colors | yes, may be empty | filament color / color-change lookup |
| Objects | yes, may be empty | model object identity |
| Instances | yes, may be empty | model instance identity |
| Moves | yes | toolpath moves |
| Events | yes, may be empty | tool/color/pause/custom events |
| StringTable | yes, may be empty | names/messages |

## Public Protocol Types

公共头文件位于：

```text
src/libslicer_worker/include/libslicer_worker/
```

建议公开：

- `PreviewTypes.hpp`
- `PreviewBinary.hpp`
- `PreviewReader.hpp`
- `PreviewProtocol.hpp`

关键约束：

- `MoveType` alias `Slic3r::EMoveType`
- `PathKind` alias `Slic3r::EMovePathType`
- `ExtrusionRole` alias `Slic3r::ExtrusionRole`
- enum numeric values 必须 static_assert 固定
- wire section numeric values 必须 static_assert 固定
- schema 稳定后，任何 enum 重排或 record layout 改动必须 bump `schema_version`

## Required Preview Records

### WireMoveRecord

用于渲染路径。必须包含：

- move id
- gcode id
- layer id
- optional object id
- optional instance id
- optional tool id
- optional filament id
- move type
- path kind
- extrusion role
- optional CP color id
- flags
- start/end position
- arc data reserved or populated
- extrusion amount
- feedrate
- width/height
- volumetric rate
- fan/temperature
- acceleration/jerk
- time per mode
- print Z
- object label id
- reserved fields

宿主程序只能在对应 flag 存在时使用对应 ID：

- `HasTool`
- `HasFilament`
- `HasCpColor`
- `HasObject`
- `HasInstance`

### WireColorRecord

必须存在。否则 `cp_color_id` 对外不可解释。

字段：

- color id
- source
  - `Unknown = 0`
  - `Filament = 1`
  - `ColorChange = 2`
- RGBA
- name string reference
- flags/reserved

规则：

- filament colors 应生成 `ColorSource::Filament`。
- color-change colors 应生成 `ColorSource::ColorChange`。
- move 只有在 `cp_color_id` 能解析到 `WireColorRecord` 时才能设置 `HasCpColor`。

### WireToolRecord

用于多喷嘴/多工具渲染和筛选。

字段：

- tool id
- primary filament id, if known
- nozzle diameter
- tool offset
- flags/reserved

move 只有在 filament-to-tool 映射解析成功时才能设置 `HasTool`。

### WireFilamentRecord

用于耗材颜色、直径和材料信息。

字段：

- filament id
- tool id, if known
- RGBA
- diameter
- density
- cost
- flags

### WireObjectRecord / WireInstanceRecord

可以先导出 model-level identity，但不要伪造 move-level ownership。

规则：

- object/instance records 只能来自有效 Orca `ObjectID`。
- 如果 move 无法可靠映射到 object/instance，不设置 `HasObject` / `HasInstance`。
- 宿主可以显示整体路径，但不能宣称支持按对象选择路径，直到 move ownership 完成。

## Internal Components

新增内部目录：

```text
src/libslicer_worker/src/artifacts/
  OutputRequest.hpp/.cpp
  SliceResultProducer.hpp/.cpp
  GCodeArtifactProducer.hpp/.cpp
  PreviewArtifactProducer.hpp/.cpp
```

preview 细节可以放在：

```text
src/libslicer_worker/src/preview/
  PreviewRequest.hpp/.cpp
  PreviewProducer.hpp/.cpp
  PreviewProduceResult.hpp
  PreviewMappingContext.hpp/.cpp
  PreviewRecordMapper.hpp/.cpp
  PreviewRecords.hpp
  PreviewStringTable.hpp
  PreviewMetadataBuilder.hpp/.cpp
  PreviewArtifactWriter.hpp/.cpp
  PreviewArtifactValidator.hpp/.cpp
  PreviewArtifactPublisher.hpp/.cpp
```

### SliceRunner

只负责高层流程：

1. 解析 request。
2. 加载 model/config。
3. `print.process()`。
4. 生成 `GCodeProcessorResult`。
5. 按需运行 artifact producers。
6. 根据每个 artifact 的 `required` 语义决定 job 成败。
7. 发送 artifact/result events。

`SliceRunner` 不应该写 binary artifact，不应该包含 preview record 映射细节，也不应该知道 `.orcapv` section layout。

### OutputRequest

解析 `output` 并归一化新旧格式：

- legacy string `output.gcode`
- object `output.gcode`
- object `output.preview`
- artifact paths
- required semantics
- overwrite semantics

### SliceResultProducer

负责拿到 Orca 的 `GCodeProcessorResult`。

输入：

- `Print`
- normalized output request
- worker directories

输出：

- `GCodeProcessorResult`
- optional public gcode path
- optional internal temporary gcode path

规则：

- 如果 public G-code enabled，export 到 public path。
- 如果 public G-code disabled 但 preview enabled，export 到 worker-owned temporary path。
- 如果 both disabled，request 已经被拒绝。
- temporary path 必须按 cleanup policy 处理。

### GCodeArtifactProducer

只负责 public G-code artifact event 和 required failure semantics。

### PreviewProducer

编排 preview：

```text
build mapping context
map preview records
build metadata
write preview.orcapv.tmp
validate preview.orcapv.tmp
rename to preview.orcapv
return structured result
```

取消检查点：

- mapping 前
- mapping 大循环中
- 写文件前
- validate 前
- rename 前

rename 前必须再次检查 cancellation，避免取消后仍发 `phase=ready`。

### PreviewRecordMapper

唯一职责是把 `GCodeProcessorResult` 和必要 config 映射成 `PreviewRecords`。

不得：

- 读取/写入文件
- 发布事件
- 构造 JSON metadata
- 猜 object/instance ownership

### PreviewArtifactWriter

只负责 binary layout 和写文件：

- header
- section table
- aligned sections
- `.tmp`
- final rename

Writer 不负责 schema semantic validation。

### PreviewArtifactValidator

负责验证 writer 输出：

- magic
- endian marker
- schema version
- file size
- section table bounds
- section overlap
- record size
- required sections
- string references
- metadata section count 与 binary section table 一致

## File Ownership

Public outputs：

```text
output.gcode
artifacts/preview.orcapv
```

Worker-owned transient files：

```text
data/internal/preview-source.gcode.tmp
artifacts/preview.orcapv.tmp
```

规则：

- 外部不得消费 `.tmp` 或 `data/internal/*`。
- 写入失败必须删除 transient files。
- validation 失败必须删除 `.orcapv.tmp`。
- cancellation 必须删除 transient files 和未宣布的 final file。
- 只有 artifact event 宣布 `phase=ready` 后，宿主才能消费 final file。
- `keep_intermediate_files=true` 可以保留 debug intermediate，但仍不得发 artifact event。

## Metadata

Metadata JSON 应包含：

```json
{
  "schema": "orca.toolpath_preview",
  "schema_version": 1,
  "binary_format": "orca-toolpath-preview-binary-v1",
  "producer": {
    "name": "orcaslicer-worker",
    "orcaslicer_commit": null,
    "orcaslicer_version": null,
    "worker_protocol": 1
  },
  "source": {
    "input_type": "orca_3mf_project",
    "input_path": "/path/to/input.3mf",
    "plate_index": 0,
    "config_digest": null
  },
  "outputs": {
    "gcode_requested": false,
    "preview_requested": true
  },
  "coordinate_system": {
    "space": "orca_plate_world_mm",
    "unit": "mm",
    "plate_origin_mm": [0, 0, 0],
    "extruder_offsets_mm": []
  },
  "sections": {
    "layers": { "count": 0 },
    "tools": { "count": 0 },
    "filaments": { "count": 0 },
    "colors": { "count": 0, "status": "available" },
    "objects": { "count": 0, "mapping": "unavailable" },
    "instances": { "count": 0, "mapping": "unavailable" },
    "moves": { "count": 0 },
    "events": { "count": 0 },
    "string_table": { "bytes": 0 }
  },
  "style_policy": {
    "name": "orca-preview-style",
    "version": 1,
    "status": "partial"
  }
}
```

## CMake Targets

Current target：

```cmake
libslicer::preview_protocol
```

第一版可以继续把 `PreviewReader.hpp` 放在 `preview_protocol` 下。

后续稳定后再拆：

```cmake
libslicer::preview_reader
```

拆分前文档必须明确：宿主暂时只链接 `libslicer::preview_protocol`。

## Test Plan

### Request Tests

- legacy `output.gcode` string maps to enabled public G-code。
- object `output.gcode.enabled=false` disables public G-code artifact。
- preview-only request accepted。
- G-code-only request accepted。
- G-code + preview request accepted。
- both disabled rejected with `no_outputs_requested`。
- bad preview format rejected。
- bad preview publish mode rejected。
- bad field types rejected。

### Worker CLI Tests

- G-code-only STL request outputs only `kind=gcode`。
- preview-only STL request outputs only `kind=preview` and no public G-code。
- G-code + preview STL request outputs both events。
- preview-only generic 3MF request outputs valid preview。
- preview-only Orca project 3MF request outputs valid preview。
- optional preview failure with required G-code keeps job success and emits warning。
- required preview failure fails job。
- cancellation leaves no ready preview event and no stale public preview artifact。
- `keep_intermediate_files=false` cleans internal temporary G-code.

### Preview Semantic Tests

- moves preserve `MoveType` / `PathKind` / `ExtrusionRole`。
- layer records index move ranges correctly。
- support extrusion roles are present for support fixture。
- multi-filament fixture produces filament records and color records。
- multi-tool fixture produces tool records and `HasTool` only for resolved moves。
- color-change fixture produces `WireColorRecord` and `HasCpColor` only for resolved colors。
- object/instance records do not imply move-level ownership unless flags are set。

### Reader/Validator Tests

- valid artifact reads back。
- wrong magic rejected。
- wrong endian rejected。
- wrong schema rejected。
- wrong record size rejected。
- overlapping sections rejected。
- bad string reference rejected。
- metadata count mismatch rejected。

### CMake Consumer Tests

- `add_subdirectory` consumer can include preview protocol headers。
- `find_package(libslicer CONFIG REQUIRED)` consumer can include preview protocol headers。
- public reader can open a generated `.orcapv` fixture。

## Implementation Phases

### Phase 1: Output Request Model

- Add normalized output request model。
- Support legacy `output.gcode` string。
- Support object `output.gcode`。
- Support object `output.preview`。
- Reject no-output requests。
- Add event fields for artifact readiness。

### Phase 2: Slice Result Producer

- Preserve current public G-code behavior。
- Add preview-only temporary G-code sink。
- Ensure temporary files are cleaned。
- Ensure `GCodeProcessorResult` is available to artifact producers。

### Phase 3: Preview Protocol Completion

- Add `ColorSource`。
- Add `MoveFlags::HasTool`。
- Add `WireColorRecord`。
- Add `WireSectionType::Colors`。
- Add `WireSectionType::StringTable`。
- Add fixed numeric/static layout assertions。

### Phase 4: Preview Writer/Reader/Validator

- Implement binary writer。
- Implement validator。
- Implement public reader helpers。
- Add corrupt-file tests。

### Phase 5: Preview Mapper

- Map moves。
- Build layers。
- Build tools from nozzle config and filament maps。
- Build filaments from `GCodeProcessorResult`。
- Build colors from filament colors and color-change records。
- Build events from tool/color/pause/custom moves。
- Keep object/instance move ownership unavailable unless proven。

### Phase 6: Worker Integration

- Run artifact producers based on requested outputs。
- Emit only requested public artifact events。
- Apply required/optional semantics per artifact。
- Ensure cancellation and failure cleanup。

### Phase 7: Host SDK Stabilization

- Harden reader。
- Decide whether to split `libslicer::preview_reader`。
- Freeze schema v1 only after fixtures prove:
  - preview-only
  - multi-tool
  - multi-filament
  - color-change
  - support role
  - Orca project 3MF
  - install/package consumer compatibility

## Acceptance Criteria

第一版完成后，应该可以回答：

- worker 是否可以只输出 G-code？是。
- worker 是否可以只输出 preview？是。
- worker 是否可以同时输出 G-code 和 preview？是。
- preview-only 是否不会暴露 public G-code？是。
- GPlatform 是否能通过 worker event 得到 preview artifact 路径？是。
- GPlatform 是否能不用解析 G-code 就显示 toolpath？是。
- 多耗材颜色是否有 wire color table？是。
- 多工具是否有明确 tool records 和 `HasTool`？是。
- 支撑路径是否保留 support extrusion role？是。
- preview 失败是否有清晰错误语义？是。
- 是否承诺实时预览？否。
- 是否承诺 object-level path selection？否，直到 move ownership 完成。
- 是否承诺 Orca GUI 100% 视觉一致？只有 canonical fixture 通过后才能承诺。
