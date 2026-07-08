# Worker Config Resolution Design

Status: draft for review.

本文档设计 `libslicer` / `orcaslicer-worker` 在脱离 Orca GUI 后的配置解析边界。核心问题是：Orca GUI 路径原本通过 `PresetBundle`、当前 preset 选择、project config 和若干 `Print` 内部状态共同建立切片上下文；worker 提取成库和独立进程后，不能假设这些上下文天然存在。

## Problem

当前 worker 的 `resolved_orca_json` 路径大致是：

```text
load model / project config
create DynamicPrintConfig::full_print_config()
apply project-embedded config
apply resolved JSON config
normalize selected worker-owned fields
Print::apply(model, config)
Print::validate()
Print::process()
export artifacts
```

这条路径没有运行 `PresetBundle::full_fff_config()`，也没有把所有由最终 config 可推导的 `Print` 状态补齐。因此曾出现两个需要库侧闭环的问题：

- `Print::m_isBBLPrinter` 未初始化，且 worker 没有在 `print.validate()` 前从 `printer_model` 或 vendor metadata 推导 BBL 状态。BBL profile 的 relative-E 校验因此可能误判。当前已在 `Print::apply()` 中从最终 config 重新推导，并把成员默认初始化为 `false`。
- `Print::m_origin` 未初始化。当前代码中 `Print::set_plate_origin()` 只有定义，未找到调用方；`PlateData` 也没有明确的 plate origin 字段。该值会影响 G-code offset、wipe tower、support/brim 和 print-space 坐标。

同时还存在一个配置契约风险：`PresetBundle::full_fff_config()` 会合成并展开 print、printer、filament、project config，包括多耗材数组、variant、`filament_self_index`、preset id 和若干 filament id clamp。`resolved_orca_json` 如果不是等价于这个结果，库内部可能在后续阶段出现错误或生成错误结果。

## Goals

- 明确 worker 和调用端之间的配置所有权。
- 让 `libslicer` 通过 `find_package` 被其他程序使用时，不要求调用端复制 Orca 内部 preset 合成逻辑。
- 保持现有 `resolved_orca_json` 模式可用，并给出严格契约和校验。
- 把 `Print` 必须自洽的派生状态放回库侧，而不是要求 GPlatform 这类宿主补私有状态。
- 避免在已有 resolved config 上重复运行 `PresetBundle::full_fff_config()` 导致覆盖、重复展开或使用错误默认 preset。

## Current Implementation Status

当前分支已经落地公共 DTO-only config SDK：

- `src/libslic3r/ConfigSDK.hpp` 暴露 `ConfigDefinition`、`ConfigIssue` / `ConfigValidationIssue`、`ConfigResolutionRequest`、`ConfigResolutionResult`、`ConfigValidationRequest` 和 3MF/preset DTO。
- `ResolvedConfig` 和 `DynamicPrintConfig` 桥接只存在于 `ConfigSDK_internal.hpp`，不是安装给外部 host 的 public API。
- `get_config_definitions()` / `get_config_definition()` 从 Orca 内部 schema 映射公共 schema 信息。
- `validate_resolved_config()` 校验 worker-critical resolved config contract，包括 filament vector 维度、physical extruder / variant lookup、plate-scoped wipe tower 字段和 layer-change G-code 字段。
- `orcaslicer-worker` 的 `resolved_orca_json` 路径已经复用同一套内部 loader 和 validator；校验在 worker-owned normalization 之后、`Print::apply()` 之前执行。
- `Print::apply()` 已经从最终 config 的 `printer_model` 推导 BBL 状态，并且每次 apply 都重新覆盖 `m_isBBLPrinter`。测试覆盖 BBL relative-E 无 `G92 E0` 通过、复用同一 `Print` 切换到非 BBL 后失败，以及 worker `resolved_orca_json` 的 BBL 无 `G92 E0` 路径。
- `resolve_fff_config()` 已实现第一版 preset-selection resolver：加载 vendor bundle、user preset dir、project preset file，选择 printer/process/filament，应用 overrides，产出 full resolved config JSON 和 normalized diff。
- `orcaslicer-worker` 已接入 `config.type=preset_selection`。worker parser 从 `config.path` 读取扁平 selection JSON，构造 `ConfigResolutionRequest`，调用同一个 `resolve_fff_config()`，再走现有 resolved config loader、normalization 和 validator；CLI e2e 同时覆盖系统 preset selection 和 `project_preset_files` 中 project-local process preset 的实际切片路径。

## Non-Goals

- 不把 GPlatform 的 DB、Qt 类型或业务对象引入 `libslic3r` / worker。
- 不让 worker 在 `resolved_orca_json` 模式下猜测宿主当前选中的 preset。
- 不用注入 `G92 E0` 这类方式绕过 BBL 检测失败。
- 不在第一阶段重构所有 Orca 全局状态，例如 `GCodeProcessor::s_IsBBLPrinter` 的全局静态设计。

## Core Decision

`resolved_orca_json` 必须表示已经完成 preset resolution 的最终配置。worker 不应该在该模式下直接再执行 `PresetBundle::full_fff_config()`。

原因是 `PresetBundle::full_fff_config()` 的输入不是一个已 resolved config，而是一组 Orca preset 状态：

- selected print preset
- selected printer preset
- selected filament presets
- project config
- filament maps
- extruder / filament variant 信息
- vendor bundle 和系统 preset 上下文

如果 worker 对一个已 resolved JSON 再运行 `full_fff_config()`，它必须重新构造这些上下文。上下文缺失或不一致时，结果可能覆盖宿主已经解析好的值，或者把 filament vector / variant 再展开一次。

因此边界分成两层：

- `resolved_orca_json`: 调用端提交最终配置，worker 只做 schema conversion、必要 normalization、contract validation 和 slicing。
- `preset_selection`: 调用端提交 preset 选择信息，由库侧 resolver 加载 Orca preset bundle 并生成最终配置。

同一套 resolver 和 validator 必须同时服务 worker 和外部 SDK。不能在 worker 内实现一套解析逻辑，又给外部应用暴露另一套逻辑；否则 BBL、多色、`filament_self_index`、variant lookup 等行为会再次漂移。

外部应用不应该自己拼完整 Orca config。GPlatform 这类宿主只应提交：

- printer preset id
- process preset id
- filament preset ids
- project config edits
- filament map

完整 resolved config 必须由库侧 resolver 生成。

## Library Responsibilities

### Print Self-Contained State

`Print` 应保证默认构造后处于确定状态：

```cpp
bool  m_isBBLPrinter { false };
Vec3d m_origin { Vec3d::Zero() };
```

`Print::apply()` 应从最终 config 中推导 BBL 状态，并在 `Print::validate()` 前完成。该推导必须在每次 `Print::apply()` 调用时重新执行，不能只放在构造函数或 worker 调用点。`Print` 是库 API 对象，调用方可能复用同一个 `Print` 实例并切换 config；旧 config 的 BBL 状态不能污染下一次 apply。

```text
if config has strong Orca vendor metadata:
  use vendor/model metadata
else if printer_model starts with "Bambu Lab":
  BBL printer
else:
  non-BBL printer
```

这属于库侧职责，因为 `Print::validate()`、wipe tower type、tool ordering 和 G-code export 都读取 `Print::is_BBL_printer()`。宿主不应该通过修改 layer-change G-code 或直接写 `Print` 私有状态来修复。

### Plate Origin Contract

当前 v1 worker / library contract 将 plate origin 定义为 zero：

```cpp
Vec3d m_origin { Vec3d::Zero() };
```

依据：

- 当前代码中 `Print::set_plate_origin()` 未找到调用方。
- `load_bbs_3mf()` 返回的 `PlateData` 包含 plate index、plate name、thumbnail、G-code 文件、filament info 和 per-plate config，但没有明确的 plate origin 字段。
- worker 已经通过 `model.curr_plate_index` 和 `print.set_plate_index()` 选择 plate；没有可靠来源可额外设置 non-zero origin。

因此第一阶段不把 plate origin 视为 GUI 必须传入的状态，也不从 3MF metadata 中猜测 origin。库侧必须初始化为 zero，worker 不设置 origin。

如果未来确认 Orca GUI 有真实 non-zero plate origin 来源，需要新增显式协议字段，而不是隐式读取模糊 metadata：

```json
{
  "input": {
    "type": "orca_3mf_project",
    "path": "./project.3mf",
    "plate_index": 0,
    "plate_origin": [0.0, 0.0, 0.0]
  }
}
```

该字段的单位为 mm，默认 `[0, 0, 0]`。新增前不得改变现有 worker 的 zero-origin 行为。

### Config Resolver API

库应提供一个公开 resolver API，供 `find_package` 用户使用。调用端传 preset 选择，而不是复制 `PresetBundle::full_fff_config()`。

建议形态：

```cpp
namespace Slic3r::libslicer {

enum class ConfigScope
{
    Printer,
    Process,
    Filament,
    Project,
    Object,
    Internal
};

enum class ConfigCardinality
{
    Scalar,
    Filament,
    PhysicalExtruder,
    PrinterVariantLookup,
    FilamentExtruderVariant,
    Plate,
    Matrix
};

enum class ConfigValueType
{
    Bool,
    Int,
    Float,
    String,
    Enum,
    Point,
    Percent,
    Vector,
    Matrix,
    Unknown
};

struct ConfigEnumOption
{
    std::string value;
    std::string label;
};

struct ConfigDefinition
{
    std::string key;
    ConfigValueType type;
    std::string label;
    std::vector<ConfigEnumOption> enum_options;
    std::string unit;
    std::optional<double> min;
    std::optional<double> max;
    std::string default_value; // Orca legacy serialized string
    std::string default_json;
    bool nullable;
    bool internal;
    ConfigScope scope;
    ConfigCardinality cardinality;
};

struct ConfigResolutionRequest
{
    std::filesystem::path resources_dir;
    std::filesystem::path data_dir;
    std::vector<std::filesystem::path> vendor_bundle_dirs;
    std::vector<std::filesystem::path> user_preset_dirs;
    std::vector<std::filesystem::path> project_preset_files;

    std::string printer_preset_id;
    std::string process_preset_id;
    std::vector<FilamentSlotRequest> filament_slots;

    std::string project_overrides_json;
    std::string printer_overrides_json;
    std::string process_overrides_json;

    int plate_index;
    bool apply_extruder { false };
    bool strict { true };
};

enum class ConfigIssueSeverity
{
    Warning,
    Error
};

struct ConfigValidationIssue
{
    std::string code;
    std::string field;
    std::string message;
    ConfigIssueSeverity severity { ConfigIssueSeverity::Error };
};

struct ConfigResolutionResult
{
    std::string full_config_json;
    std::string normalized_diff_json;
    std::vector<ConfigIssue> issues;
};

struct ConfigValidationRequest
{
    std::string full_config_json;
    int plate_index;
    bool run_print_validate { false };
};

std::vector<ConfigDefinition> get_config_definitions();
ConfigResolutionResult resolve_fff_config(const ConfigResolutionRequest& request);
std::vector<ConfigIssue> validate_resolved_config(const ConfigValidationRequest& request);

} // namespace Slic3r::libslicer
```

内部可以复用 `PresetBundle::full_fff_config()`、`DynamicPrintConfig`、`ConfigOptionDef` 和 `print_config_def`，但主 API 边界必须是库级的、无 GUI 类型的、可被 worker 和外部 host 调用的。

`DynamicPrintConfig` / `ResolvedConfig` 只能存在于库内 bridge。主路径必须使用 JSON DTO、`ConfigResolutionRequest`、`ConfigResolutionResult`、`ConfigValidationRequest` 和 `ConfigIssue`，避免外部应用直接依赖 Orca 内部 config 类型。

### Config Definition / Schema API

外部应用需要构建 profile UI、表单、项目编辑器和本地校验，因此库必须暴露 config definition / schema。该 API 应从现有内部 `ConfigOptionDef` / `print_config_def` 映射出来，但不能要求外部应用 include 或理解这些内部类型。

每个 public `ConfigDefinition` 至少包含：

- key
- value type
- label
- enum options
- unit
- min/max
- legacy default value string
- typed default JSON
- nullable
- internal/develop-facing marker
- scope: `printer` / `process` / `filament` / `project` / `object` / `internal`
- vector cardinality: scalar、filament-scoped、nozzle/extruder-scoped、plate-scoped、variant-scoped、matrix

schema API 要满足两个使用场景：

- SDK host 可以知道哪些 keys 可编辑，以及它们属于 printer/process/filament/project/object 哪个层级。
- validator 可以复用同一份 cardinality 信息，避免 UI、resolver、worker 各自维护不同的 key 分类。

第一阶段允许部分字段为 unknown，但必须对 worker slicing 所需的关键 keys 提供准确 scope 和 cardinality。

### Resolver Input Boundary

外部应用不能自己拼 Orca 的完整 resolved config。它只能提交 preset selection 和 project edits：

```cpp
ConfigResolutionRequest request;
request.printer_preset_id = "Bambu Lab X1 Carbon 0.4 nozzle";
request.process_preset_id = "0.20mm Standard @BBL X1C";
request.filament_slots = {
    {0, "Bambu PLA Basic @BBL X1C", "#FFFFFF", "", "PLA", ""},
    {1, "Bambu PLA Basic @BBL X1C", "#000000", "", "PLA", ""}
};
request.project_overrides_json = R"({
  "layer_height": 0.2
})";
```

resolver 输出的 `full_config_json` 才能进入 worker 或直接进入 slicing API。调用端手写完整 `filament_self_index`、`filament_extruder_variant`、printer variant arrays、preset id group 等字段不是支持的主路径。

resolver 需要支持明确的 search policy：

- System/vendor presets: 从 `resources_dir` 下的 Orca vendor bundles 加载，或从 `vendor_bundle_dirs` 中显式加载。
- User presets: 从 `data_dir` 下的用户 preset 目录加载，或从 `user_preset_dirs` 中显式加载。
- Project-local presets: 从 `orca_3mf_project` 解出的 project preset 或 `project_preset_files` 加载，优先级高于 system preset，低于 request 中直接给出的 `printer_overrides_json` / `process_overrides_json` / `project_overrides_json` override。
- Name resolution: preset id 必须支持 Orca canonical name；如果 bare name 命中多个 bundle，应返回 ambiguous 错误，除非 request 指定 vendor/bundle id。
- Compatibility: resolver 只负责合成 config，不自动替换不兼容 preset；第一版对 system/user process 和 filament 在合成前执行 Orca compatibility check，不兼容时返回 `preset_incompatible`。`project_preset_files` 加载的 project-local preset 视为项目权威，不因继承到的 system whitelist 单独拒绝。

### Resolved Config Validator

库应提供一个 resolved config 校验函数。worker 和外部 SDK 都必须调用同一个 validator。该函数不重新合成 preset，只验证最终配置是否满足切片前置条件。

validator 必须按 key 的 cardinality 分类校验，不能把所有 vector 都当成 filament slot vector。第一版至少需要维护一张显式 key 分类表：

| Category | Count Source | Examples | Validation |
| --- | --- | --- | --- |
| Scalar identity | exactly 1 | `printer_model`, `printer_settings_id`, `print_settings_id` | must exist for worker slicing; non-empty string |
| Filament-slot scoped | filament slot count | `filament_settings_id`, `filament_colour`, `filament_diameter`, filament temperature/material fields | length equals filament slot count |
| Filament map | filament slot count | `filament_map` | length equals filament slot count; each value is 1-based and within physical extruder/nozzle count accepted by printer config |
| Physical nozzle/extruder scoped | physical nozzle/extruder count | `nozzle_diameter`, `extruder_offset`, `min_layer_height`, `max_layer_height` | length equals physical nozzle/extruder count; do not force to filament slot count |
| Printer variant lookup scoped | indexable by `filament_map` / extruder id | `extruder_type`, `nozzle_volume_type`, `default_nozzle_volume_type` | length must cover every 1-based extruder id referenced by `filament_map`; BBL single-nozzle multi-filament jobs may still require resolved values that are indexable for each mapped filament |
| Process-extruder variant scoped | `print_extruder_variant` table length | `print_extruder_id`, `print_extruder_variant` | schema/cardinality must distinguish process variant vectors from filament-slot and physical-extruder vectors; non-empty process variant metadata must have matching id/variant lengths and positive 1-based extruder ids |
| Filament-extruder variant scoped | variant table length | `filament_extruder_variant`, `filament_self_index` | both exist; lengths match; every `(filament_self_index, filament_extruder_variant)` lookup needed by `update_values_to_printer_extruders_for_multiple_filaments()` must resolve |
| Plate scoped | plate count or selected plate index | `wipe_tower_x`, `wipe_tower_y`, other per-plate fields | length must include selected `plate_index`; do not force to filament slot count |
| Matrix scoped | square or explicitly shaped by source count | `flush_volumes_matrix`, `flush_volumes_vector` | normalized to filament slot count where Orca profile resize behavior requires it |
| Filament id scalar | 0 or 1..filament slot count | `support_filament`, `support_interface_filament`, `wipe_tower_filament` | value 0 means auto/default; positive values must not exceed filament slot count |
| G-code semantic scalar | exactly 1 | `gcode_flavor`, `use_relative_e_distances`, `before_layer_change_gcode`, `layer_change_gcode` | must exist and be parseable before `Print::validate()` |

校验失败应返回可读错误，而不是在 `Print::apply()` 深处 assert 或生成难以定位的切片失败。

错误对象建议包含：

```json
{
  "code": "invalid_resolved_config",
  "field": "filament_self_index",
  "message": "filament_self_index length must match filament_extruder_variant length",
  "severity": "error"
}
```

warning 可用于历史兼容字段缺失；会导致错误切片、assert、越界或语义误判的字段必须是 error。

validator 是切片前强制 gate。worker 必须在 `Print::apply()` 前执行：

```text
request.full_config_json = full_config_json
request.plate_index = plate_index
issues = validate_resolved_config(request)
if any issue.severity == Error:
  fail job with invalid_resolved_config
Print::apply()
```

外部应用如果直接调用库内 slicing API，也必须走同一 gate；不能只在 worker 路径校验。

## Worker Request Modes

### Existing Mode: `resolved_orca_json`

该模式继续支持。语义收紧为：

```text
The config JSON must be equivalent to the output of Orca's full FFF preset
resolution for the selected printer, process, filaments, project config, and
filament map.
```

worker 处理步骤：

```text
load project/model
load project-embedded config when present
apply resolved JSON
normalize worker-owned compatibility fields
validate resolved config contract
Print::apply()
Print::validate()
slice/export
```

worker 可以做有限 normalization，例如当前已有的 flush volume resize，因为这是 worker 输入兼容性处理。但 worker 不应在该模式下选择 preset 或重新运行 full preset merge。

### New Mode: `preset_selection`

当前 worker parser 接受 `resolved_orca_json`、`project_embedded` 和 `preset_selection`。第一版 `preset_selection` 保持 request `version: 1`，`config.path` 指向一个扁平 selection JSON 文件。

新增模式用于让非 Orca GUI 调用端只提交选择信息。建议协议：

```json
{
  "version": 1,
  "config": {
    "type": "preset_selection",
    "path": "./preset-selection.json"
  }
}
```

`preset-selection.json`:

```json
{
  "vendor_bundle_dirs": [
    "/absolute/path/to/resources/profiles/OrcaFilamentLibrary",
    "/absolute/path/to/resources/profiles/BBL"
  ],
  "user_preset_dirs": [],
  "project_preset_files": [],
  "printer_preset_id": "Bambu Lab X1 Carbon 0.4 nozzle",
  "process_preset_id": "0.20mm Standard @BBL X1C",
  "filament_slots": [
    {
      "slot_index": 0,
      "filament_preset_id": "Bambu PLA Basic @BBL X1C",
      "color": "#FFFFFF",
      "filament_type": "PLA",
      "slot_overrides": {}
    }
  ],
  "printer_overrides": {},
  "process_overrides": {},
  "project_overrides": {},
  "strict": true,
  "apply_extruder": false
}
```

字段语义：

- `vendor_bundle_dirs`: 可选。为空时 resolver 会扫描 `resources_dir/profiles` 下的默认 vendor bundle；显式提供时按给定顺序加载。BBL 路径需要先加载 `OrcaFilamentLibrary`，再加载 `BBL`。
- `user_preset_dirs`: 可选。为空时不额外加载 user preset。
- `project_preset_files`: 可选。用于 3MF 解出的 project-local presets 或调用端导出的 project preset overlay。
- `printer_preset_id`: 必填。第一版按 preset name 精确选择。
- `process_preset_id`: 必填。
- `filament_slots`: 必填，长度定义 filament slot count；顺序即 slot order。每项支持 `slot_index`、`filament_preset_id`、`color`、`color_type`、`filament_type` 和 `slot_overrides`。
- `printer_overrides` / `process_overrides` / `project_overrides`: 可选，作为对应层级 override 应用。值可为 JSON object 或 JSON string。
- `strict`: 可选，默认 `true`。strict 模式下 unknown override key 会转为 error。
- `apply_extruder`: 可选，默认 `false`，语义传给 resolver 的 full config 合成路径。

错误码：

| Code | Meaning |
| --- | --- |
| `unsupported_config_type` | worker 不支持 `preset_selection` 或 request version 过低 |
| `preset_search_path_invalid` | search path 不存在、不可读或不是目录/文件 |
| `preset_not_found` | 指定 printer/process/filament preset 无法解析 |
| `preset_ambiguous` | bare preset name 命中多个 bundle/user/project preset |
| `preset_incompatible` | preset 组合不满足 Orca compatibility rules |
| `project_preset_invalid` | project-local preset 文件无法解析或 schema 不兼容 |
| `preset_resolution_failed` | `PresetBundle::full_fff_config()` 或等价 resolver 失败 |
| `invalid_resolved_config` | resolver 产物未通过 resolved config validator |

worker 处理步骤：

```text
load project/model
construct ConfigResolutionRequest
resolve_fff_config()
apply resolved JSON through internal ConfigSDK loader
worker-owned normalization
validate resolved config contract
Print::apply()
Print::validate()
slice/export
```

该模式由库侧 resolver 保证 `PresetBundle::full_fff_config()` 等价行为。worker 必须调用与外部 SDK 相同的 `resolve_fff_config()` 和 `validate_resolved_config()`，不能维护 worker-private resolution 逻辑。GPlatform 如果已经有可靠 resolver，可以继续使用 `resolved_orca_json`；其他程序可以使用 `preset_selection` 或直接调用库 resolver。

## BBL Detection Contract

worker 必须保持 Orca 的 BBL printer 语义。对 `orca_3mf_project` 使用 resolved config 切片时，`Print::validate()` 中的 relative-E 校验只应对非 BBL 的 Marlin printer 要求 `G92 E0`。

BBL profile 例如 `Bambu Lab X1 Carbon 0.4 nozzle` 不应为了通过校验而向 `layer_change_gcode` 注入 `G92 E0`。

正确来源是 resolved config 中的 printer identity：

- 首选 Orca vendor/model metadata，如果库暴露强类型字段。
- fallback 使用 `printer_model`，例如以 `Bambu Lab` 开头。

该逻辑应位于 `Print::apply()` 或一个被 `Print::apply()` 调用的库内 helper 中，而不是放在 GPlatform。每次 `Print::apply()` 都必须重新计算并覆盖 `m_isBBLPrinter`，保证复用 `Print` 实例时状态不会串 job。

## Migration Plan

### Phase 1: Stabilize Current Worker Path

- 初始化 `Print::m_isBBLPrinter` 为 `false`。（已实现）
- 初始化 `Print::m_origin` 为 `Vec3d::Zero()`。
- 在 `Print::apply()` 中从最终 config 推导 BBL 状态。（已实现）
- 增加 BBL relative-E validate 测试：
  - BBL Marlin relative-E 且无 `G92 E0` 应通过。（已实现）
  - 非 BBL Marlin relative-E 且无 `G92 E0` 应失败。（已实现）
- 增加 `Print` 复用测试：
  - 同一 `Print` 先 apply BBL config，再 apply 非 BBL config，第二次 validate 不得沿用 BBL 状态。（已实现）
  - 同一 `Print` 先 apply 非 BBL config，再 apply BBL config，第二次 validate 必须使用 BBL 状态。
- 增加 worker fixture：
  - worker `resolved_orca_json` + BBL `printer_model` + 无 `G92 E0` 必须成功。（最小 STL smoke 已实现）
  - `firehorse.3mf` + `resolved_orca_json` + BBL `printer_model` + 无 `G92 E0` 必须成功。
  - 同一 fixture 必须同时产出 `output.gcode` 和 `output.orcapv`。
  - 测试不得通过修改 `layer_change_gcode` 注入 `G92 E0`。（最小 STL smoke 已实现）
- 在 `docs/worker_api.md` 中明确 `resolved_orca_json` 是 full resolved config。

### Phase 2: Add Resolved Config Contract Validation

- 增加 public `validate_resolved_config(ConfigValidationRequest)`。
- worker 在 `Print::apply()` 前调用 validator。
- 外部 slicing API 在 `Print::apply()` 前调用同一 validator。
- 对常见配置缺失返回明确错误码和字段名。
- 覆盖多耗材 BBL fixture，例如 `firehorse.3mf` 对应的 resolved config。

### Phase 3: Add Public Config SDK API

- 新增 DTO-only `ConfigSDK.hpp`，`ResolvedConfig` / `DynamicPrintConfig` bridge 放到 `ConfigSDK_internal.hpp`。
- 新增 `ConfigDefinition` / schema API，映射内部 `ConfigOptionDef` / `print_config_def`。
- 明确 public API 不暴露 `DynamicPrintConfig`。
- 安装导出 public headers，保证 `find_package(libslicer)` 用户可用。

### Phase 4: Add Library Resolver API

- 新增无 GUI 类型的 preset resolution API。（已实现）
- 内部复用 `PresetBundle` 和 `full_fff_config()` 等价合成路径。（已实现）
- 外部 SDK 和 worker 共用 `resolve_fff_config()`。（已实现）
- 实现 worker `preset_selection` parser。（第一版已实现，request version 仍为 1，selection JSON 为扁平结构）
- 实现 preset search path、project-local preset overlay 和错误码。（第一版已实现；显式 vendor/user/project 搜索路径错误已覆盖 `preset_search_path_invalid`，project preset 解析/type/inherits 错误已覆盖 `project_preset_invalid`，显式 vendor bundle、user preset dir 和 project preset 文件重名选择已覆盖 `preset_ambiguous`，resolver 合成失败映射为 `preset_resolution_failed`）
- worker 新增 `preset_selection` config type。（已实现，CLI e2e 覆盖 BBL X1C + 双 PLA slot system preset，并覆盖通过相对 `project_preset_files` 选择 project-local process preset 后实际切片）
- worker 现在把 ConfigSDK 的首个 error issue code 透传到 error/result event；例如 `preset_not_found`、`preset_incompatible`、`preset_ambiguous`、`unknown_config_key`、`invalid_config_cardinality`，CLI e2e 覆盖缺失 filament preset 的 `preset_not_found` 透传，以及 BBL X1C 下不兼容 process / filament selection 的 `preset_incompatible` 透传。
- 后续仍需补强更多真实项目/多耗材 fixture；resolver 和 worker CLI 已有 bundled BBL 单槽/双槽基础覆盖和基础 compatibility 错误覆盖。

### Phase 5: Reduce Global State Risk

- 审计 `GCodeProcessor::s_IsBBLPrinter` 等 process-global 状态。
- 如果 worker 未来支持并发 job，把这些状态改为 processor/context-local。
- 在此之前，worker 继续保持单 active job 语义。

## Open Questions

- Orca 是否已有比 `printer_model starts_with("Bambu Lab")` 更强的 vendor/model helper？如果有，应作为 BBL 判断首选。
- validator 的严格程度如何分级？建议第一版区分 error 和 warning，避免一次性阻断所有历史兼容配置。
- Orca 是否存在当前未导出的 project-local preset 格式，需要在 `preset_selection` 中单独建模？
- `search.ambiguity=first_match` 是否应该支持？第一版建议只接受默认 `error`，避免跨 bundle 误选。
