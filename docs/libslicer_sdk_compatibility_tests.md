# libslicer C++ SDK v1 兼容性与验收规范

状态：v1 release Gate。本文不增加公共 API；它验证
[`libslicer_sdk_api.md`](./libslicer_sdk_api.md) 的行为，并约束实现不得偏离 Orca 基线。缺少
已提交 baseline manifest 或冻结 fixtures 时，SDK 只能作为开发预览，不能作为稳定 v1 发布。

实施状态：当前工作树已有进程级 runtime/shared internal loader 和 schema index 候选，
但仍未通过 release Gate。schema index 修复后的 cold create 现状为 `24.553 s`；第二阶段
record conversion、等价性/确定性/并发/RSS 测试和修复后的真实 create 性能测试尚未实施或
通过。以下新增条目是目标验收规范，不是当前通过状态。

## 1. 基线和测试原则

GUI 行为基线取自抽取最小 libslicer 分支之前的提交：

```text
71c30dd1feb73efa40d824ee1e4be05fce7b0827
```

这是提交 `1666b286a8bfdee57df448647971674a83e36ab2` 的父提交。历史 GUI 文件已从当前
工作树删除，测试说明中的 GUI 行号以该提交为准。

测试分三层：

1. public contract tests：只包含 `<libslicer/v1/...>`，验证外部调用行为；
2. adapter conformance tests：允许包含内部头，验证 SDK/core 无损转换；
3. compatibility baseline tests：用固定 3MF 和配置分别驱动 SDK 与基线 core，比较结果。

验收不能只检查“没有崩溃”。每项测试必须比较明确的值、错误、revision、诊断或输出摘要。

### 1.1 可执行历史 baseline oracle

兼容性 baseline 不允许用“SDK 与同一工作树的当前 core 相互比较”代替历史事实。冻结流程为：

1. 在独立 worktree checkout `71c30dd1feb73efa40d824ee1e4be05fce7b0827`；
2. 用只链接该提交 libslic3r 的非 GUI baseline harness 执行第 1.2 节 fixtures；harness 严格复现
   第 2 节记录的 full config、plate apply、validate/process/export 调用链；
3. 生成 golden configuration、diagnostics、final map、statistics 和 normalized G-code SHA-256；
4. 把输出写入 `tests/data/libslicer_sdk/baseline_manifest.json` 并提交；
5. SDK 测试只读取已提交 manifest，正常测试不得在当前工作树重新生成 golden。

manifest 每个 case 必须记录：历史 source commit、baseline harness source SHA-256、fixture
SHA-256、resources/preset tree SHA-256、platform/architecture、compiler及版本、locale、timezone、
线程数、所有输入配置、输出字段和 normalization rules。缺少或 hash 不匹配时测试失败，不能
自动更新。

golden 更新必须由显式 `update_libslicer_baseline` 开发者目标完成，并在变更中同时提交旧/新
差异；该目标不进入默认 build/ctest。

G-code normalization 只允许两项逐字节替换：

- generator comment 中 ISO-8601 生成时间替换为 `<GENERATED_AT>`；
- manifest 记录的精确 temporary root byte sequence 替换为 `<TEMP_ROOT>`。

不得删除整行、排序、统一空白或忽略其他字段。fixture 没有对应动态字段时该规则必须为空。

### 1.2 冻结 fixtures

首版 manifest 至少包含以下文件及 SHA-256：

- `single_plate_basic.3mf`；
- `multi_plate_scopes.3mf`；
- `embedded_presets.3mf`；
- `manual_multitool.3mf`；
- `auto_map_roundtrip.3mf`；
- `custom_gcode_bytes.3mf`；
- `metadata_roundtrip.3mf`。

跨平台可能产生合法差异时使用同一 fixture 的平台专属 golden entry；不能在一个 entry 中用
宽松比较掩盖差异。

## 2. 原 GUI 能力基线

### 2.1 预设

- 历史 GUI baseline `71c30dd1feb73efa40d824ee1e4be05fce7b0827` 的
  `src/slic3r/GUI/GUI_App.cpp:3039` 在应用 preset 初始化阶段调用
  `PresetBundle::load_presets()`；
- 该调用发生在 GUI 应用 preset 初始化阶段，建立由 GUI App 长期持有并复用的
  `PresetBundle`；原 GUI 并不是每次导入 3MF 都创建一个独立 SDK context 并重新完整加载
  catalog。该历史调用只证明 GUI 生命周期；SDK 测试必须断言完整
  `PresetBundle::load_presets(AppConfig&)` 调用次数为 0；
- 当前 core `src/libslic3r/PresetBundle.cpp:510-540` 的 `PresetBundle::load_presets()` 在 `:520`
  进入 `load_system_presets_from_json()`，随后加载 user presets、更新兼容性并恢复 selection；
- 当前 `load_system_presets_from_json()` 位于 `:2165-2287`：`:2198-2223` 同步先加载
  `OrcaFilamentLibrary`，`:2225-2247` 用独立 bundle 和 `tbb::parallel_for` 解析其他 vendor，
  `:2249-2277` 按 `other_vendors` 的索引顺序合并；merge helper 位于 `:2401-2420`；
- 当前 `:2187-2197` 从 `directory_iterator` 收集 vendor name，未显式排序。因此测试必须把
  Orca-first、其他 vendor 并行和确定性 merge 分开断言；前两项是 current core 事实，确定性
  vendor 顺序是冻结目标且仍待实施，不能把“按目录枚举捕获顺序 merge”误写成跨文件系统已经
  稳定；
- `src/slic3r/GUI/Tab.cpp:6716` 调用 `save_current_preset()`；
- `src/libslic3r/PresetBundle.cpp:3872` 附近的 `full_fff_config()` 合成顺序为：

```text
defaults
  → selected process
  → default filament
  → selected printer
  → project_config
  → selected filaments
```

SDK baseline 必须覆盖预设加载、父链继承、编辑、另存、删除、选择和带上下文兼容性。

上述 `GUI_App.cpp`/`Tab.cpp` 行号和 GUI 生命周期结论来自第 1 节冻结历史基线提交；当前最小
libslicer 工作树已删除历史 GUI 可执行源码。上述 `PresetBundle.cpp` loader 行号来自当前工作树，
只证明 current core 算法，不能虚构成历史提交已经具有相同并行实现，也不能宣称当前 SDK 已经
调用了该入口。

### 2.2 工程加载

历史 `load_bbs_3mf()` 位于：

- `src/libslic3r/Format/bbs_3mf.hpp:253`；
- `src/libslic3r/Format/bbs_3mf.cpp:8952`；
- `src/libslic3r/Model.cpp:326` 和 `:396` 的加载路径。

固定 fixture 必须至少包含：

- 单 plate 和多 plate；
- project config 与 selected presets；
- embedded presets；
- object、volume/part、instance transform 和 printable/excluded；
- object/part/layer-range overrides；
- plate local config、custom G-code 和 filament map；
- SDK 未公开但 importer/exporter 应保留的 Orca metadata。

### 2.3 配置分层和 apply

原 GUI 调用链：

```text
Plater::priv::update_background_process()
  ├─ PartPlate::get_real_filament_maps(project_config)
  ├─ PresetBundle::full_config(false, filament_maps)
  ├─ BackgroundSlicingProcess::apply(model, full_config)
  │    ├─ full_config + current_plate->config()
  │    └─ Print::apply(model, new_config)
  └─ Print::set_extruder_filament_info(...)
```

证据：

- 历史 `src/slic3r/GUI/Plater.cpp:7963-7988`；
- 历史 `src/slic3r/GUI/BackgroundSlicingProcess.cpp:689-697`；
- 历史 `src/libslic3r/PresetBundle.cpp:3872` 附近。

object、part 和 layer-range 不在调用 `Print::apply()` 前压平：

- 当前 `src/libslic3r/PrintApply.cpp:1482` 附近单独处理 object config；
- 当前 `src/libslic3r/PrintApply.cpp:1497-1498` 单独复制 volume/part config；
- 当前 `src/libslic3r/PrintApply.cpp:1499` 单独复制 layer-range config。

兼容测试必须证明 SDK 传入的 Model 层级仍然产生与基线一致的 object/region 配置和 G-code，
不能只比较一份 global config。

### 2.4 filament map

原 GUI 多喷头流程：

- `Plater.cpp:7980-7984` 传入已有 plate map，并设置物理耗材信息；
- ByObject 路径在当前 `src/libslic3r/Print.cpp:2483-2493` 的 auto mode 中计算推荐 map；
- 常规 ByLayer 路径在当前及历史
  `src/libslic3r/GCode/ToolOrdering.cpp:1290-1304` 计算推荐 map 并写回配置；
- 历史 `BackgroundSlicingProcess.cpp:229-235` 在切片后读取最终 map 并写回 plate。

auto 算法的输入证据：

- `src/libslic3r/FilamentGroupUtils.hpp:21-40`；
- `src/libslic3r/FilamentGroup.hpp:70-95`；
- `src/libslic3r/GCode/ToolOrdering.cpp:1107-1205`；
- `src/libslic3r/PresetBundle.cpp:3624-3640`。

这些代码表明算法读取 physical tool、external 标记、物理耗材材质/颜色/support、feeder
group topology、model layer usage 和几何/物理限制。v1 没有公开这些输入，因此本版本的
兼容目标是：manual 切片与基线一致，auto 状态 load/save 无损，但 auto slice 明确返回
`unsupported`。不得用强制 manual 冒充 auto 成功。

### 2.5 切片闭环

基线闭环为：

```text
apply(Model, global_plus_plate_config)
  → validate
  → process
  → read final filament map
  → export G-code
  → collect statistics/diagnostics
```

历史位置包括 `BackgroundSlicingProcess.cpp:680` 附近的 validate、`:225-235` 的 process/map
和 `:241` 附近的 export。

## 3. Public API 编译与安装测试

### API-01：独立消费者 add_subdirectory

一个只包含公开头的 C++17 consumer 通过 `add_subdirectory()` 链接
`libslicer::sdk_v1`，不得添加内部 include path 或内部 target。

### API-02：独立消费者 find_package

安装后在全新 build tree 中执行 `find_package(libslicer CONFIG REQUIRED)` 并链接
`libslicer::sdk_v1`。验证 Release/Debug 配置和静态库传递依赖。

### API-03：公开头自包含

每个 `<libslicer/v1/*.hpp>` 单独作为 translation unit 的第一个 include 编译。公开 include
闭包不得出现 wx、GUI、worker 或 libslic3r 头。递归扫描安装头，禁止重新引入独立配置解析
facade/预解析输入 handle、单字段 preset/project commit wrapper、instance transform 查询和
custom-G-code/embedded-preset CRUD；`json.hpp` 单独自包含，其他核心头不传递 JSON codec 依赖。

### API-04：版本检查

验证 package/header major 不匹配在 configure/compile/link 阶段失败；schema id/version 不匹配
在 schema/JSON 使用入口返回明确错误，不要求 context 猜测 CMake package version。

### API-05：Result 与 Diagnostic

错误矩阵固定如下；除明确 fault injection 项外均使用真实公开入口：

| ErrorCode | 固定入口与输入 | 主 field |
| --- | --- | --- |
| invalid_argument | `SdkContext::create()` 任一 limit 为 0 | `/limits/<field>` |
| not_found | snapshot 查询另一个 Project 的 entity ID | `/entity` |
| conflict | 旧 expected revision commit | `/expected_revision` |
| busy | active job 未终态时同 engine 再 submit | `/engine` |
| unsupported | `SliceEngine::submit()` 接收 auto filament mode | `/filament_map/mode` |
| io | load 不存在或不可读 path | `/path` |
| resource_limit_exceeded | G-code 或 preview 写入超过 limit | `/limits/gcode_bytes`、`/limits/preview_bytes` 或 `/limits/preview_moves` |
| invalid_configuration | validate 拒绝已解析配置 | 对应 `/configuration/<OptionId>` |
| cancelled | StageBarrier 到达后 cancel 并 `wait_for()` 观察取消终态 | `/job` |
| slicing_failed | test hook 在 validate 成功后的 process/export 注入 core failure | `/slice` |
| internal | test hook 注入未被其他 code 覆盖的 SDK exception | `/internal` |

每项验证：

- 失败 Result 至少一个 error diagnostic；
- `error_code()` 等于第一个主 error 的 code；
- `field` 是对应公开请求字段的稳定 JSON Pointer；
- 失败 Result 调用 `value()` 抛 `std::logic_error`；
- 成功 Result 的 `error_code()` 为空；warning 不改变成功/失败主状态；
- `Result<void>` 与 `Result<T>` 遵守相同规则。

## 4. ConfigValue 和 ConfigSchema 测试

### CFG-01：全部 FFF option 类型无损往返

枚举当前 FFF config definition 中每个已知 option type，执行：

```text
core value → ConfigValue → core value
```

比较原始类型和序列化值。覆盖：

- scalar/list bool、int、float、string、enum；
- `coPercent/coPercents`；
- `coFloatOrPercent/coFloatsOrPercents`；
- point/points/point3；
- `coPointsGroups` 和 `coIntsGroups`；
- empty list、empty group；
- 当前 FFF definition 中实际存在的 nullable vector item nil。

任何已知类型被跳过即失败。未来未知类型必须得到 `unsupported`。

公开值模型允许 nullable scalar；另用 synthetic schema case 验证 nullable scalar，不把它伪称为
当前 FFF definition 已存在的 option。

### CFG-02：shape 与 nullability

验证递归 list shape 相等比较；list container null 和 list item null 分离；nil item 的索引在
往返后不变。

分别构造 string、enumeration、string null 和 enumeration null：string 仅能由
`as_string()` 读取，enum 仅能由 `as_enumeration()` 读取；null 和所有错误 accessor 返回
`nullopt`，证明 enum 不会降级成 string。

### CFG-03：schema 约束

分别验证未知 OptionId、错误类型、越界数值、非法枚举、错误单位、长度错误、scope/preset
kind 错误。对同一 base values 和未提交 candidate patch 在不同上下文调用
`validate_and_normalize()`，验证结构/上下文校验与 canonical patch 在单一入口完成；调用
`evaluate_option()` 验证 patch 被原子合并并触发真实可见/兼容规则。错误 field 指向对应
OptionId 或 context 字段。

adapter conformance test 枚举每个公开 editable FFF option，要求内部 versioned rule registry 有
显式规则或显式 `always_visible`/`core_validation_only` 标记，覆盖率必须为 100%。

### CFG-04：generic patch 边界

通过 generic ConfigPatch 修改 preset selection、filament map 或 structural metadata 必须失败。
`OptionDescriptor` 和全部安装公开头不得出现内部 ownership enum/field；adapter conformance test
单独验证内部 channel 能稳定拒绝这些 key。

同时验证 `ConfigPatch` 保持首次插入顺序、同 OptionId 的 `set()` 原位覆盖且不产生重复 entry、
`erase()` 返回值和 `find()` 结果；验证 `ConfigValues` 表示完整只读集合，公开 API 不提供 set/
erase，复制后值和顺序不变。

### CFG-05：JSON 互操作

只包含 `<libslicer/v1/json.hpp>` 即可使用 JSON helper，且其他核心公开头不传递 JSON codec
依赖。ConfigPatch export/import 同 schema 无损；`export_json(EffectiveConfiguration)` 包含完整
values、provenance 和 schema id/version；schema id/version 不兼容、未知字段和错误值类型明确
失败。业务 API 测试中不得用 JSON 代替 ConfigPatch。

### CFG-06：filament reference 公共规范化

提交独立、随 schema version 冻结的
`tests/data/libslicer_sdk/fff_filament_reference_manifest.json`。每项至少记录 canonical OptionId、
`FilamentReferenceKind`、shape、value/item nullability 和内部 conversion category。测试断言
manifest 与 adapter registry 双向完全相等，缺项、多项、alias 未 canonicalize 或分类不同均失败；
不得通过枚举 registry 自己证明 registry 完整。每个 manifest entry 都必须按自身 conversion
category 使用真实 option/core value 验证 descriptor、public value、往返、nullability 和边界；
仅当当前 manifest 没有某个 codec category 时，才用 synthetic rule 补该代码路径覆盖。

对 manifest 中使用内部 default sentinel 的每个 scalar reference 固定验证：

```text
core 0 ↔ public null
core 1 ↔ public logical slot 0
core N ↔ public logical slot N-1
```

并验证 public null/slot 往返后 core 类型和序列化值不变。对 list reference 同样验证 item null、
0 基索引和原索引位置。public 负整数、已知 `filament_count` 上界外的值、
descriptor 不允许的 null 均固定返回 `invalid_configuration`、Severity `error`，field 为
`/configuration/<escaped OptionId>`；`filament_count` 为空时不做上界检查。diagnostic 不得出现
内部编码术语。

`filament_map` 不得出现在 editable generic descriptor 集合，且其 reference classification 为
`FilamentReferenceKind::none`；generic patch 修改它失败，强类型 map 仍按 logical slot index 保存
1 基 `ToolId`。SlotRemap 测试只依据 reference kind 迁移上述公开值。

安装面测试递归扫描全部安装公开头，禁止出现 `FilamentReferenceEncoding`、
`core_extruder_one_based`、`logical_slot_zero_based` 和 `logical_slot_list_zero_based` 等旧符号。

### CFG-07：schema find index 等价、唯一性与并发只读

adapter conformance test 必须保留一个仅用于测试的旧线性 lookup oracle，以同一 ordered
`OptionDescriptor` vector 对照索引实现：

- 对首项、中间项、末项和多个真实 FFF OptionId，public `find()` 的存在性及返回 descriptor 全字段
  与线性 oracle 相等；未知、空和大小写不同的 OptionId 都与 oracle 一样返回 `nullopt`；
- 对同一 OptionId 重复查询，并从多个复制的 `ConfigSchema` 查询，public 值始终相等；private
  descriptor lookup 每次都指向共享 immutable state 中同一个 vector entry，证明 index 不保存
  descriptor copy 或不稳定地址；
- synthetic descriptor vector 含重复 OptionId 时，线性 oracle 明确记录其 first-match 结果，
  但 private schema builder 必须在发布前失败，返回 `ErrorCode::internal`、Severity `error`、
  field `/schema/options/<escaped OptionId>`。不得以 first-wins/last-wins、覆盖或 fallback 线性
  扫描构造可用 schema；
- `options()` 在索引前后逐 entry、逐字段、逐顺序相等；schema id/version 不变，安装 public
  headers 的 checksum/API surface 不变；
- 至少 16 个线程用 barrier 同时在同一 schema 及其 copies 上循环执行 present/missing
  `find()`、`options()`、`validate_and_normalize()` 和 `evaluate_option()`；结果逐字段相同，
  private probe 证明 index build count 恰好为 1、发布后 mutation count 为 0，ThreadSanitizer
  或等价 race 检查不得报告 data race。

### CFG-08：config diff/patch 与完整 catalog golden 等价

对 printer/process/filament 的 default、单层继承和多层继承 fixtures，同时执行修复前冻结的
linear lookup oracle 与索引实现：

- `core_config_diff_to_patch()` 的成功/失败状态、diagnostics、ordered `ConfigPatch` entries、
  OptionId、ConfigValue shape/nullability/value 和 core round-trip bytes 逐项完全相同；
- `core_config_to_values()`、`apply_patch_to_core_config()` 和所有同类 descriptor lookup 热路径
  继续使用 `OrcaConfigAdapter` 的单一转换权威；静态检查禁止在 `Preset.cpp` 新增
  `ConfigOptionType`/descriptor/value conversion switch 或第二份 ownership table；
- 对完整 production catalog 比较 record 数量、公开排序、identity、revision、name、vendor、
  parent、inherited values、effective values、overrides 和 frozen catalog digest，索引前后必须
  逐 record 完全相同；Orca-first、non-Orca vendor 并行 parse、stable merge 和 strict diagnostics
  的 probes/计数/顺序不变；
- 可以增加 schema lookup 或 config diff 微基准帮助发现复杂度回归，但微基准只能作为辅助证据，
  不能替代 PRE-07 在完整 production resources 上执行的真实 cold/reuse
  `SdkContext::create()` Gate。

### CFG-09：fused conversion 与 immutable values 共享

以旧 `core_config_to_values()` + `core_config_diff_to_patch()` 作为 test-only oracle，覆盖
printer/process/filament 的 root、default、单层继承、多层继承、nullable/list/group、未知 option
和 conversion failure：

- fused conversion 的 success/failure、primary diagnostic、全部 diagnostics 顺序、完整
  effective values、changed patch、OptionId/value 顺序及 core round-trip 逐字段等价；
- changed patch 中每个 changed `ConfigValue` 与同一次 effective result 的对应 value 共享同一
  immutable identity；不得二次转换；
- child inherited `ConfigValues` 与 parent effective `ConfigValues` 共享 identity，且与旧
  inherited oracle 值等价；相同实际 default config identity 的 root/default records 共享
  identity；
- 构造名称相同但 actual default authority/value 不同的 fixtures，必须不共享；禁止名称/vendor/
  kind 猜测。采用 content-addressed identity 时，完整 config 值不等的 fixtures 也必须不共享；
- ThreadSanitizer 或等价 race 检查覆盖 immutable shared values 的并发读取，不得出现 mutation
  或 data race。

### CFG-10：record conversion worker 等价、失败确定性与 mutation probes

通过 `LIBSLICER_SDK_TESTING` 下 `PresetInternal.hpp` 的 internal seam 分别固定 `1`、`2`、`4`
个 worker，在同一完整 fixture 和 production catalog 上构建 records。seam 不安装、不导出、
不读取环境变量；普通 production build 不编译该开关：

- 三种 worker 数的 record 全字段、collection/global 顺序、diagnostics、revision、duplicate
  结果及 frozen catalog digest 完全相同，并与旧串行 oracle 相同；
- 分别在不同 global ordinal 注入 freeze、parent/default、identity、duplicate 和 conversion
  failure，并至少构造一组 freeze/duplicate/conversion 交错失败；安排较大 ordinal 的 freeze 或
  conversion 先完成，public Result 仍只选择最小 ordinal failure 为唯一 primary；
- 构造 `child ordinal < parent ordinal` fixture。parent 分别发生 duplicate 和 metadata/revision
  failure，但 parent effective artifact conversion 成功；child 必须使用该 artifact 转换成功，
  不得记录 dependency failure，最终 primary 仍是 parent 原 global ordinal 及旧 oracle 对应的
  原错误类型；
- 同一反向 ordinal fixture 中令 parent effective conversion 失败；此时 child 必须记录
  dependency conversion failure。new scheduler 可先计算 parent artifact，但最终各 ordinal
  status、primary ordinal/type 和 diagnostics 必须与旧串行直接读取 parent core config 的 oracle
  一致，不能受 artifact 执行顺序影响；
- probe 断言任何 record-local freeze/duplicate failure 都写入对应 result slot，不从 freeze
  提前返回；不受影响的 ordinals 仍被处理。只有注入的无 ordinal allocator/arena/init failure
  立即失败；
- probe 断言 worker 内 `get_preset_parent()`、`Preset::inherits()` 及等价 lazy parent access
  调用数为 `0`，可能创建 option/cache 的 helper 调用数为 `0`，worker 只调用 const
  `keys()`/`optptr()`；catalog/global/core preset mutation 数和 `catalog.next_revision` 写入数
  都为 `0`；
- probe 证明 system-loader TBB 在 record arena 创建前已经 join/quiescent；production arena
  `max_concurrency == min(4, record_count)`，每个 dependency batch 在下一批前已经 join，
  adapter 内 task/arena creation count 为 `0`，arena 销毁先于 commit；
- 对无法证明稳定 immutable 的 config fixture，peak simultaneous owning copies 不超过 `4`，
  每批 join 后 copy 数回到 `0`；稳定 config 使用 attempt lease 下 const pointer。任何路径都不
  一次性复制完整 catalog；
- `RecordCandidateStatus` 是唯一 public record candidate staging owner；
  `EffectiveArtifactSlot` 仅持 DAG artifact。frozen inputs 的 complete
  `ConfigValues`/candidate ownership count 为 `0`，空 `catalog.records` 只在全部成功后按
  ordinal 接收 move，commit 后 statuses/artifact slots 的 candidate/default/effective ownership
  count 均为 `0`；
- 任一失败后已发布 record 数为 `0`、catalog generation/revision/digest 不变；成功时主线程按
  global ordinal 一次提交；
- 在 TSAN 或等价配置下重复执行并发转换，且至少重复运行完整 catalog 构建，digest、diagnostics
  和排序每次稳定。

## 5. 预设测试

### PRE-01：list 和 origin

按 kind 列出 system/vendor/user，确保 origin 无非法组合，project-embedded 不进入 repository。

### PRE-02：继承视图

构造多级 parent chain，通过 `PresetRepository::get()` 读取 system/vendor/user 固定 revision
视图，并比较 `inherited_values()`、`effective_values()` 和 `overrides()`；覆盖无父 preset 的
kind defaults。system/vendor 不需要 edit 即可完整读取配置。

### PRE-03：事务冲突

打开两个 editor。第一个提交成功后，第二个基于旧自身 revision、旧 parent revision 或旧
schema generation 提交必须返回 `conflict`，不能用新父链静默重算。

### PRE-04：可修改范围

user preset 可 create/edit/erase；system/vendor 返回 `unsupported`；parent kind 不匹配返回
`invalid_argument`。

对 user editor 依次验证 `set_override()` 新增和覆盖、`erase_override()` 删除存在项；删除不
存在项必须幂等成功。每一步只通过 `PresetEditor::snapshot()` 取得 staged `PresetView`，并验证
editor 不重复公开 metadata/inherited/effective/overrides getters。成功 commit 直接返回
`PresetSummary`，revision 更新且 `get(new revision)` 视图一致；安装头不得出现单字段 commit
wrapper。单独 editor
调用 `discard()` 后修改不可见、revision 不变，且该 editor 再 commit 返回 `conflict`。

create 使用独立 id 和 display name：验证 commit 后 `PresetRef::id()`/`PresetSummary::name` 分别
保持原 UTF-8 bytes，重复 display name 允许；空值、内嵌 NUL、非法 UTF-8 返回
`invalid_argument` 和 `/preset/id` 或 `/preset/display_name`。同 kind/user/id 的 byte-exact 冲突
返回 `conflict`、field `/preset/ref/id`；大小写不同 id 可共存。create editor 未 commit 前不进入
repository，commit 后立即可见。

### PRE-05：兼容性上下文

逐项固定断言：

- process candidate 缺 printer：`invalid_argument`，field `/context/printer`；
- filament candidate 缺 printer：`invalid_argument`，field `/context/printer`；
- filament candidate 缺 process：`invalid_argument`，field `/context/process`；
- candidate revision 过期：`conflict`，field `/candidate/revision`；
- context revision 过期：`conflict`，field `/context/printer/revision` 或
  `/context/process/revision`；
- candidate/context kind 错误：`invalid_argument`，field 指向对应 `/ref/kind`。

输入合法但不兼容时查询本身成功，`PresetCompatibility::compatible=false` 并携带原因；有效
上下文结果与历史兼容表达式一致。

### PRE-06：目录角色、冲突和 catalog 快照

以只读 fixture 分别构造 `resources_dir` 和两个 `preset_dirs`，以独立可写 fixture 构造
`data_dir`：

- create-time 负例分别覆盖空 path、不可读 root、缺 `resources_dir/profiles`、缺
  `resources_dir/profiles/OrcaFilamentLibrary.json`、不可创建/不可写 data/temp、不可读
  `preset_dirs/{index}`、任一 vendor JSON 语法/option/parent 错误、duplicate identity、
  alias 一对多/循环和缺 dependency。`SdkContext::create()` 必须返回精确 `io`、
  `invalid_configuration`、`conflict` 或 `not_found` 及对应 field，不发布 Context、空 catalog
  或 default-only catalog；
- 成功 create 后第一次 `presets()` 必须只构造 repository handle。`RuntimeInitializationProbe`
  断言该调用不产生 schema build、bundle construction、vendor load 或 merge；
- create/edit/erase user preset 后只允许 `data_dir` 下 SDK-owned user store 发生变化；
  `resources_dir` 和 `preset_dirs` 的递归内容 hash 必须不变；同时用 `PresetIoProbe` 断言所有
  writable open/create/remove/rename/replace 都在 canonical data user store 内，其他 source 没有
  writable open，即使最终内容被恢复也失败；
- `preset_dirs` 中的 preset 以 vendor origin 列出，edit/erase 返回 `unsupported`；
- 对 resources、preset_dirs 和既有 data user profile 分别构造 JSON `name`、cloud `setting_id`、
  `filament_id`、display alias 与 filename 全部不同的 fixture，断言 PresetRef id 只等于解析后的
  JSON `name` bytes；缺少/非法 name 在 create 边界按来源字段返回
  `invalid_configuration`；
- 不同 origin 的相同 kind/id 可同时通过各自 PresetRef 读取。duplicate fixtures 分别覆盖同一
  resources、同一 data store、同一 preset dir、resources vendor 与 preset-dir vendor、两个
  preset dirs；都必须在 create 的固定扫描顺序下返回 `conflict`，field 精确为后扫描来源对应的
  `/resources_dir`、`/data_dir` 或 `/preset_dirs/{index}`，且没有覆盖；
- 成功 create 后新增、修改、删除或同路径替换外部 source，既有 repository 和同配置后续
  Context 保持共享的已发布 catalog，不自动刷新、不重载。该 Context 自己成功 commit 后新
  repository 查询立即可见，旧 PresetView 仍保持原不可变捕获；
- 分别覆盖 edit 后目标被外部修改、删除、同路径替换；create editor 捕获不存在后被外部创建；
  erase 前目标被外部修改或删除。包含同尺寸且恢复原 mtime 的内容修改，全部返回 `conflict`、
  Severity `error`、field `/preset/storage_revision`，且不覆盖外部内容。
- child editor 捕获后由另一 Context 修改/删除任一 user parent，child commit 返回
  `/preset/storage_revision`；另一 Context 修改无关 identity、使捕获的 manifest generation 变化，
  同样冲突。同一 Context 发布任何新 catalog generation 后，旧 editor 返回
  `/expected_revision`。

每个失败 case 同时断言 Severity `error`。不得把具体来源错误 field 折叠为 `/presets`。

### PRE-07：进程 runtime、system loader、失败原子性和性能

测试必须在独立测试进程运行，以免前序 case 的进程级 runtime 污染结果。使用
`RuntimeInitializationBarrier` 和 `RuntimeInitializationProbe` 覆盖：

- canonical catalog key 精确等于 canonical `resources_dir`、canonical `data_dir` 和保持调用顺序
  的 canonical ordered `preset_dirs`。用 symlink/`..` 等价路径断言 canonical 后同 key；交换
  `preset_dirs` 顺序断言不同 key；
- 同 key 顺序 create 至少 20 次：全部成功，共享同一 `SharedRuntime` identity 和成功 catalog
  generation；system loader invocation count、`OrcaFilamentLibrary` load count 和完整 catalog
  publish count 均恰好为 1。其余 vendor 的单 vendor load count 各为 1，merge count 等于
  non-Orca vendor 数，不得把“每 vendor 一次”误断言为总 vendor load count=1。销毁全部
  Context/handle 后再次 create 仍取得同一 `SharedRuntime`，上述计数不增加；
- 同 key 并发首次 create：用 barrier 同时释放至少 16 个线程，恰好一个线程进入 system
  loader，其余等待同一个 attempt；成功 case 的所有调用取得同一 `SharedRuntime`/成功
  generation，system loader invocation count=1；
- different key 分别改变 `resources_dir`、`data_dir`、`preset_dirs` 长度、元素和顺序。在
  `Initializing` 期间必须立即返回 `conflict`，field 精确为 `/resources_dir`、`/data_dir`、
  `/preset_dirs` 或首个 `/preset_dirs/{index}`；已有 live `SharedRuntime` 的 `Ready` 状态同样
  返回该错误。每个 case 断言无第二个 loader、无全局路径切换、无第二 catalog、无 catalog
  reset；
- `temporary_dir` 与七个 `ResourceLimits` 明确不进入 key：对同一 key 分别改变 temporary root
  和每个非零 limit，所有 Context 都成功共享同一 `SharedRuntime`。`ResourceProbe` 和
  `PresetIoProbe` 断言 Project/Slice handle 使用来源 Context 自己的 limit/temporary root，
  不读写另一个 Context 的 temporary directory；
- `tests/libslic3r/test_preset_bundle_loading.cpp` 用两个内容不同的显式 fixture root 分别调用
  `load_system_presets_from_json_at(profiles_dir, compatibility_rule, policy)`，同时把 GUI/core
  全局 data path 指向第三个 sentinel root；结果必须只来自传入的 `profiles_dir`，I/O probe 不得
  访问 sentinel root，证明共享实现不从全局状态重新推导 profiles 目录；
- 同一 test source 对 GUI wrapper 建立冻结回归：wrapper 必须把 GUI `data_dir/system`、原
  `compatibility_rule` 和 `GuiBestEffort` 传给共享实现。正常、单 vendor parse 失败和多 vendor
  累计失败 fixture 的 substitutions、cumulative error bytes、继续加载行为及调用顺序与重构前
  golden 完全相同；唯一允许的顺序变化是本文冻结的 canonical vendor 排序。不得把 GUI 改成
  strict，也不得让 SDK 经过该 wrapper；
- probe 证明 SDK 首次加载只调用
  `load_system_presets_from_json_at(resources_dir/profiles, compatibility_rule, SdkStrict)`：
  显式 `profiles_dir` 与 policy 逐字段匹配，完整 `PresetBundle::load_presets(AppConfig&)` 调用
  计数和 SDK 串行逐 vendor loader 计数均为 0；`OrcaFilamentLibrary` 恰好先加载一次，其余 vendor
  使用 core 独立 bundle 并行路径；
- `SdkStrict` fixture 分别触发 `io`、`parse`、`duplicate`、`alias_cycle`、
  `alias_ambiguous`、`missing_dependency` 和 `invalid_identity`。core result 的每个 issue 都
  必须含预期 kind、vendor、存在目标的 canonical path（缺失目标为 canonical profiles root 下的
  lexically-normalized expected path）和非空精确 message；SDK 映射必须逐项等于
  `io -> (io, /resources_dir)`、`parse -> (invalid_configuration, /resources_dir)`、
  `duplicate -> (conflict, /resources_dir)`、
  `alias_cycle -> (invalid_configuration, /resources_dir)`、
  `alias_ambiguous -> (conflict, /resources_dir)`、
  `missing_dependency -> (not_found, /resources_dir)`、
  `invalid_identity -> (invalid_configuration, /resources_dir)`。任一 issue 都失败，Severity 为
  `error`，message 保留 core issue message，candidate/publish count/success generation 不变；
- Orca-first 的明确边界是 Orca load 完成后才允许任何其他 vendor parse 开始；并行断言要求至少
  两个 non-Orca vendor 的 parse interval 实际重叠，不能只检查创建了多个 future/thread；
  non-Orca vendor 必须在调度并行 parse 前完成 canonical vendor UTF-8 byte 排序，merge 必须在
  所有并行 parse 完成后按同一排序顺序发生；
- 随机化 vendor 目录枚举/创建顺序并在独立进程重复至少 10 次，公开 preset identity/revision、
  diagnostics 顺序和 catalog digest 完全一致。duplicate、alias、user store、ordered
  `preset_dirs`、strict parse error 和 no-fallback 负例继续保持 PRE-06 的精确错误；任一 vendor
  失败时不得成功返回 default-only、Orca-only、空或部分 catalog；
- 在 schema、Orca library、并行 vendor、中间 merge、SDK strict validation 和 publish 前分别
  注入失败：同一波至少 16 个同 key 等待者的 `error_code`、Severity、message、field 和
  diagnostics 顺序逐字段完全相同；candidate 对 `presets()`/Project/inspect/submit 不可见，
  成功 generation 和 publish count 不变，TBB 全部 quiescent，loader scratch/candidate 被销毁；
- 失败 attempt 清除后才释放测试 barrier。失败波次中的等待者不得各自重试，system loader
  invocation count 只包含该失败 attempt 的一次调用。解除注入后，下一次显式同 key create 从
  全新 candidate 真实重试并成功；另一个独立 case 在失败清除后用 different key 显式 create，
  也必须允许启动新 attempt。probe 证明两种 retry 都不读取上次 candidate/残留 mutable cache；
- 用 I/O barrier 阻塞 core loader，断言其他线程仍可取得 registry mutex 并立即完成 different-key
  conflict 检查；用等待者 barrier 断言其等待期间不持有 registry/catalog mutex；catalog
  transaction 与 attempt wait 交错时不得死锁。probe 必须显示 loader I/O 只在 core runtime
  mutex 下，registry mutex 只覆盖查表/发布，catalog mutex 只覆盖已发布 catalog；
- 分别创建 `PresetRepository`、`Project`、`ProjectSnapshot`、`SliceEngine` 和已提交
  `SliceJob` 后销毁原始 `SdkContext` 及其 parent facade；逐一调用 list/get、snapshot、
  inspect、submit/wait，断言继续有效且结果仍引用同一 `SharedRuntime`。释放最后一个公开
  lease 后 coordinator 仍持有 live runtime，再次 create 不重载。

性能 Gate 使用 GPlatform 当前测试机、同 Debug 构建、同一 production resources。当前 schema
index 修复后的 profiling 证据固定记录为：

| 现状分段 | 观测值 |
| --- | ---: |
| cold `SdkContext::create()` wall time | `24.553 s` |
| shared core system loader wall time | `5.262 s` |
| `append_collection` wall time | `19.275 s` |
| printer record conversion wall time | `1.709 s` |
| process record conversion wall time | `9.082 s` |
| filament record conversion wall time | `8.484 s` |
| inherited values conversion sampled CPU time | `5.783 CPU-s` |
| effective values conversion sampled CPU time | `5.808 CPU-s` |
| diff/patch conversion sampled CPU time | `5.190 CPU-s` |

wall time 与 sampled CPU time 是不同口径，不相加、不相减。该证据表明 schema index 已移除原
lookup 热点，但 record conversion 仍主导；只共享 inherited values 的理论 cold 约
`18.77 s`，仍不足 Gate。使用
monotonic clock 在三个全新测试进程各跑一次 cold case，再在每个进程的 Ready runtime 上重复
reuse；逐次报告且每次都必须过线，禁止用平均值、后台预热或异步返回掩盖超标：

- cold `SdkContext::create()` 每次 `<= 15000 ms`；
- Ready 后同 key `SdkContext::create()` 每次 `<= 100 ms`；
- cold create 进程 peak RSS 每次 `<= 2.2 GB`（`2,200,000,000` bytes）；
- 每进程 system loader invocation count 恰好为 `1`；reuse 与 Project load 均不得增加；
- Ready 后 `firehorse.3mf` 的 `Project::load()` 每次 `<= 10000 ms`。

时间单位固定为毫秒。cold create 测量从紧邻 `SdkContext::create(options)` 调用前到返回后，包含
preflight、canonicalization、attempt 登记/等待、schema、core system loader、SDK strict
validation/merge 和 publish；reuse 使用同一完整 API 边界。每次必须分别记录 preflight/
canonicalization、schema、core system loader、Orca load、parallel vendor parse、deterministic
merge、user store/ordered `preset_dirs`、SDK strict validation、publish、reuse 和 Project load
分段。报告必须记录测试机型号、CPU/核心数、内存、OS、构建类型、编译器及版本、TBB/线程设置、
locale/timezone、resources/preset tree SHA-256、资源总字节数、vendor 数和 OS file-cache policy；
资源 hash 或机器条件不一致时结果不得冒充同一 Gate 记录。

RSS/时间 release authority 固定为新增的 dedicated
`tests/libslicer_sdk/libslicer_sdk_cold_gate.cpp` executable target
`libslicer_sdk_cold_gate`，不使用 Catch 隐藏参数，也不复用 `libslicer_sdk_tests` 主进程：

- test-only CLI 固定为
  `libslicer_sdk_cold_gate --resources <path> --data-root <path> --fixture <firehorse.3mf> --result <json>`；
  child 在独立 `data-root` 下创建 SDK data/temporary directories。三个 CTest 分别使用独立
  data-root/result 路径，但使用同一冻结 production resources tree 和同一 fixture bytes；
- 每个 child 在同一全新进程依次执行一次 cold `SdkContext::create()` 和一次同 key Ready reuse，
  再在 Ready runtime 上执行一次 `Project::load(fixture)`，读取 loader invocation count，并从
  进程启动覆盖到 Project load 返回获取 OS peak RSS；
- macOS 使用 `getrusage(RUSAGE_SELF).ru_maxrss`，其值按 bytes 解释；Linux 使用同一 API，
  `ru_maxrss` 按 KiB 乘 `1024`；Windows 使用 `GetProcessMemoryInfo()` 的
  `PeakWorkingSetSize` bytes。所有平台最终统一为 `peak_rss_bytes`；
- child 写一个结构化 JSON result file，至少包含 `cold_ms`、`reuse_ms`、`project_load_ms`、
  `loader_count`、`peak_rss_bytes`、pass/fail、失败 diagnostics，以及机器型号、CPU/核心数、
  内存、OS、构建类型、编译器、TBB/worker limit、resources tree SHA-256、fixture SHA-256/
  bytes、总资源字节数、vendor 数、locale/timezone 和 OS file-cache policy metadata；结果文件
  写失败同样返回非零；
- child 自行判定 cold `<=15000 ms`、reuse `<=100 ms`、loader count `==1`、
  `Project::load()<=10000 ms`、`peak_rss_bytes<=2200000000`，任一失败退出非零。三项 timing
  Gate 与 loader/RSS Gate 全部决定 exit code。shell `time`、父进程瞬时采样、微基准或只测
  worker 子阶段都不是 release authority；
- `tests/libslicer_sdk/CMakeLists.txt` 注册
  `libslicer_sdk_cold_gate_1`、`libslicer_sdk_cold_gate_2`、
  `libslicer_sdk_cold_gate_3` 三个 CTest，每个启动独立 child、使用独立 result file并设置
  `RUN_SERIAL TRUE` 和 `TIMEOUT 60`。三次不可并行且必须逐次通过，禁止取平均值；CTest 或
  child 被 timeout/信号终止均为失败。

第二阶段验收必须同时通过 CFG-07/08 的既有 schema/catalog invariance、CFG-09/10 的 fused/
sharing/worker 等价与确定性，以及上述真实 cold/reuse/RSS Gate。微基准只能辅助定位，不能替代
完整 production resources 的真实 create；RSS 必须覆盖整个 cold create 进程而非只测单条
record。任何一个 Gate 失败都不能写成完成，也不能通过降低 catalog 内容、后台预热或放宽阈值
规避。

### PRE-08：持久化原子性

分别验证：create editor → commit 后新 Context 能读取完整 metadata/parent/overrides/effective
values；edit → commit 后新 Context 读取新值；erase 后新 Context 的 get 返回 `not_found`。跨
Context 不比较进程内 PresetRevision。

用 `PresetFailureInjectionHook` 分别在 `payload_staging`、`payload_flush_close`、
`manifest_staging`、`manifest_flush_close`、`before_manifest_replace` 失败，并断言 probe 命中指定
阶段而非任意 `io`：API 返回 `io`、Severity `error`、field `/data_dir`，内存 catalog
generation/revision 不变，旧 manifest/可达文件逐字节不变，目录中无残留 staging/tombstone。

原子 replace 成功即越过提交点。分别注入 `after_commit_directory_flush`、
`after_commit_content_reclaim` 和二者同时失败：Result 均成功，恰好一个
`{ErrorCode::io, Severity::warning, ..., "/data_dir"}` diagnostic，新 Context 读取新 generation，
不可达旧 content 不被加载。

两个 editor 先捕获同一旧版本再并发提交，固定为恰好一个成功、另一个 `conflict` 且 field
`/preset/storage_revision`。`PresetTransactionBarrier` 让第一个 transaction 取得跨进程锁后
暂停，断言第二个不能越过锁；释放后第二个在锁内重读并冲突。至少运行一个双进程 case，不能
只依赖 RuntimeCoordinator mutex。edit/update、create-absent 和 erase 都执行该竞争测试。

## 6. Project 3MF 测试

### PRJ-01：格式边界

Orca/Bambu FFF project 3MF 成功；STL、OBJ、generic 3MF、G-code-only 3MF、SLA 3MF 和伪装
扩展名均在 load 阶段 `unsupported`，不发布 Project。

### PRJ-02：load-save-reload 无修改往返

对每个固定 fixture 执行 load → snapshot → save → reload。逐项比较：

- 通过 public contract 比较 plate/object/part 数量、plate/object 名称、`PlateInfo::locked` 和 ID
  对应实体；
- project/plate/object/part/layer-range patches；
- selected presets；
- project/plate filament map mode/tools；
- 通过 adapter conformance probe 比较未公开的 instance transform/printable/excluded/plate 归属、
  embedded preset parent/revision/config、custom G-code mode/位置/slot/color/raw bytes 和扩展
  metadata。隐藏数据仍必须逐字段 round-trip，但公共测试不得为此包含内部类型。

### PRJ-03：分层编辑

在一个 ProjectEdit 中同时修改 project、plate、object、part 和 layer-range patch。commit 后
每个修改只出现在目标 scope；save/reload 后保持。

### PRJ-04：唯一事务入口

所有 project 修改都通过 ProjectEdit。成功 commit revision 恰好增加一次；任一 staged 修改
非法时全部回滚。commit 直接返回 `ProjectRevision`，安装头不得出现单字段 commit wrapper。
`discard()` 后 staged 修改全部不可见、revision 不变，editor 不得再次 commit。

### PRJ-05：ID binding

跨工程 ID、已删除实体 ID、错误 object 的 part 返回明确
`not_found`。同一工程中未删除实体的旧 snapshot ID 在后续 revision 仍指向同一实体；并发
state 冲突由 expected ProjectRevision 返回 `conflict`。

### PRJ-06：selection 与 SlotRemap

逐项验证 `old_to_new` 长度、target 越界和重复 target 返回 `invalid_argument` 及精确 field；验证
插入、删除、重排 logical slots 时，public null 保持 null，scalar/list item reference 在 project、
plate、object、part、layer-range 中按同一规则迁移；adapter probe 验证隐藏 embedded preset
overrides 和 structured custom G-code 同步迁移。

删除后仍被公开配置引用时 commit 返回 `invalid_configuration`，直到同一 edit 中通过对应 scope
patch 显式设为 null、其他 slot 或 erase。隐藏 embedded/structured G-code 引用被删除时同样返回
`invalid_configuration`，且 v1 不提供 CRUD 绕过；raw G-code 无法证明安全时返回 `unsupported`。
新增 slot 时，manual project map 或任一已有 manual local map 未提交完整新 tools 返回
`invalid_configuration`；补全全部 manual map 后成功。auto project/local map 的 tools 是 dense
prefix：只 remap 已有 tools，删除项丢弃，新增 slot 不补齐；迁移后仍是 dense prefix 时保留并只
校验已有 positive ToolId 与已知 printer tool 上限。增加非前缀缺口用例，例如旧 slot 0 remap 到
新 slot 2；若同一 ProjectEdit 未提交新的合法 auto vector 或 complete manual map 覆盖该 map，
commit 必须返回 `invalid_configuration`，field `/filament_map/tools`。提交覆盖 map 后成功。不得
猜测 ToolId。所有失败均断言工程 revision/state 完全不变。

增加 swap remap `[1,0]`：对同一显式 ConfigPatch 和 map 修复，分别在 selection mutator 前和后
调用，结果逐字段完全相同且保持调用方给出的目标
namespace slot，证明没有 double-remap。显式 erase 在两种调用顺序下都不复活；未显式覆盖的
original 值只迁移一次。同一 ProjectEdit 第二次调用 selection mutator 返回 `conflict`、Severity
`error`、field `/selection`，第一次 target 和全部 overlay 不变。另验证首次结构校验失败完全不改
staged state、不建立 target namespace；修正参数后可作为首次成功调用。

### PRJ-07：隐藏工程数据边界

递归扫描安装公开头，禁止 instance transform 查询、custom G-code CRUD 和 embedded preset 完整
CRUD。用 adapter conformance probe 验证它们仍保存在内部 ProjectState、参与 target-plate 切片、
无修改 save/reload 不丢失；任何失败不发布半完成 revision。

### PRJ-08：并发 save

save(expected revision) 与并发 edit 同时发生，成功文件必须完整对应某一个 expected state，
不得混合字段；revision 不符返回 `conflict`。成功 save 前后 Project revision 完全不变。

### PRJ-09：查询结果所有权

销毁生成查询结果的临时 path/string/container、ProjectEdit 或局部 snapshot 副本后，已返回的
ProjectSnapshot、PlateInfo/ObjectInfo/PartInfo、ConfigPatch、ConfigValues 和 layer-range patch
仍可读取且逐字节不变。

### PRJ-10：目标 plate 上下文

`multi_plate_scopes.3mf` 中每个 plate 使用不同 bed shape、custom G-code、instance printable/
excluded、object scope 和 wipe-tower 参数。分别切每个 PlateId，验证只包含目标 plate 实例，
custom G-code 和 plate index 正确，输出分别匹配各自 historical golden；连续交替切 plate 不得
复用上一 plate 状态。

### PRJ-11：GPlatform-style mesh builder

public contract test 只包含 `<libslicer/v1/...>`，用 `ProjectBuilder` 从已解析 triangle mesh 构建
Project，不经过 3MF 临时文件、不调用 STL/OBJ importer、不 include Orca/libslic3r 私有头。
fixture 至少包含两个 object、多个 mesh part、两个 plate、多个 instance、object/part/layer-range
overrides。分别覆盖 complete manual map 和 project/plate persistent auto partial map。manual map
`build()` 成功后必须能 snapshot、edit、inspect 和 submit；auto partial map `build()` 成功后必须
能 snapshot、edit、inspect，且 submit 按 MAP-04 返回明确 unsupported。

### PRJ-12：builder 与等价 Orca 3MF baseline

为同一 scene 准备等价 Orca/Bambu project 3MF golden。分别通过 `ProjectBuilder` 和
`Project::load()` 构建 Project，比较 project/plate/object/part/layer-range patch、selected
presets、manual map、切片 diagnostics、statistics 和 normalized G-code SHA-256。允许 3MF metadata
只存在于 load 路径，但不得影响切片结果。

### PRJ-13：builder 输入边界

分别验证空 mesh、空 object、空 part 列表、triangle index 越界、退化 triangle、NaN/Inf 坐标、
NaN/Inf transform、跨工程/过期 ObjectId、无 selection、缺 filament map、非法 manual map、非法
auto dense-prefix tools、超出 `model_triangles` limit 均返回规定错误，不发布部分 Project。
`ResourceProbe` 证明 triangle 计数按 unique geometry 而不是 instance 数累计。

## 7. 切片 inspection 与配置解析测试

### RES-01：全局/plate 合成顺序

为每一层设置可区分值，验证 `SliceEngine::inspect()` 返回的 EffectiveConfiguration 严格等于：

```text
defaults → process → default filament → printer → project
         → selected filaments → target plate
```

### RES-02：不压平 Model scopes

创建两个 object、多个 part 和互不相同的 layer ranges。验证
`EffectiveConfiguration::values()` 不包含这些局部 patch；切片结果仍体现各自局部设置。

### RES-03：revision 和 provenance

inspection provenance 记录 snapshot revision、plate 和全部 selected preset revisions，且不公开
catalog generation。任一依赖过期或被删除时返回 `conflict`，不能从 catalog 取较新版本。

### RES-04：temporary selection

`SliceRequest::temporary_selection` 以单一 optional 原子携带 selection 和 complete manual map，
不修改 Project。mode 非 manual 固定返回 `invalid_argument`、
field `/temporary_selection/complete_manual_map/mode`；长度不完整或包含 `ToolId{0}` 返回
`invalid_argument` 并指向 `/temporary_selection/complete_manual_map/tools` 或对应 index；超过新
printer physical tool 上限返回 `invalid_configuration` 并指向对应 tools index。公开 API 不允许
只传 temporary selection 或只传 map。temporary auto 在 `inspect()` 与 `submit()` 中都必须被拒绝；
只有 project/plate 持久状态中的 auto map 可进入 inspection boundary。

## 8. Filament map 测试

### MAP-01：project/plate inheritance

无 plate override 时 source=project；存在 override 时 source=plate；清除 override 后恢复
project。manual map 的 tools 长度等于 logical filament slots，ToolId 为 1 基且在 printer tool
范围内。persistent project/plate auto map 保留原始 partial tools vector，允许短于 logical slots，
但必须是 dense prefix；只断言已有 positive ToolId 和已知 printer tool 上限；不得补齐或转 manual。

### MAP-02：manual baseline

单/多耗材、单/多 tool 的 manual map 通过 SDK 和基线 core 切片，最终 tools、G-code tool
选择和统计一致。

### MAP-03：auto 状态往返

带 `auto_for_flush` 和 `auto_for_match` 的 3MF load/save/reload 后 mode 和已有 tools 不变。

### MAP-04：auto inspection 成功，auto slice 被明确拒绝

两个 auto mode 的 `inspect()` 均成功返回 `SliceInspection`，其中 `EffectiveFilamentMap::mode`
保持原 auto mode，已有 dense-prefix tools 原样保留，且 diagnostics 中 field
`/filament_map/mode` 的 auto-map not slice-compatible warning 必须恰好一个：
`code=unsupported`、`severity=warning`。允许其他字段的独立 warnings 同时存在；测试不得断言
diagnostics size 等于 1。不得补齐 tools，不得产生偷偷转换后的 manual map。

两个 auto mode 的 `submit()` 均返回 `unsupported`，field 为 `/filament_map/mode`；不得产生
SliceJob、G-code、preview 或偷偷转换后的 manual map。

### MAP-05：显式改为 manual

App 在 ProjectEdit 中把 auto mode 改为 manual 并提供完整 tools，commit 后可成功 inspect 和
submit。

## 9. 切片与 G-code baseline

### SLC-01：FFF 基线

对固定 3MF、固定 preset 资源和固定环境，SDK 输出与第 1.1 节已提交的 historical golden
比较；另以当前 core adapter conformance case 定位转换问题，但后者不能替代 historical
golden。比较：

- validation diagnostics 的 code/field；
- layer count；
- per-slot length/volume/mass；
- final manual map；
- SliceResult 中 effective configuration 的全部 values/provenance（不含 catalog generation）；
- 成功结果 diagnostics（包括 warning 顺序）；
- G-code 正规化摘要。

`elapsed` 不与历史 golden 做逐毫秒相等比较；验证其非负、不超过测试进程测得 wall time 加明确
调度容差，并在重复 `wait()` 后保持同一结果值。

G-code 中若包含允许变化的时间戳或临时路径，测试只规范化明确列出的非确定字段。不得通过
大范围删除 G-code 行掩盖差异。

### SLC-02：一次输入快照

对同一 SliceRequest，`inspect()` 返回的 configuration/map 必须与随后未发生依赖变化时 submit
的 SliceResult 完全相同。submit 返回 SliceJob 前用 barrier 证明 preset revisions、global+plate
configuration、manual map 和 project snapshot 已同步冻结；submit 返回后并发修改 Project/preset，
不得改变已提交 job 的 effective configuration、工程快照分层状态、map 或 G-code。依赖在同步
冻结前已变化时 submit 直接返回 conflict，不发布 job。

### SLC-03：invalid configuration

core validation 拒绝固定映射为 `invalid_configuration`；validate 已成功后 process/export 的
core failure 固定映射为 `slicing_failed`。两者均验证 API-05 的 field，且不返回部分
SliceResult。

### SLC-04：原始 G-code bytes

用内部 fake byte exporter 分别产生 embedded NUL、非 UTF-8、高位 bytes 和末尾 NUL，验证
`gcode_bytes.has_value()` 且 `gcode_bytes->size()` 与输入长度完全相等并逐字节比较；不追加 NUL、
不做编码转换。3MF/XML 无法合法承载的 byte pattern 只用于 adapter conformance test，不伪装成
3MF fixture。

### SLC-05：post-process

非空 post-process 配置在执行任何外部命令前返回 `invalid_configuration`。

### SLC-06：输出模式

同一 fixture 分别运行 gcode-only、preview-only 和 gcode+preview。gcode-only 不生成 preview；
preview-only 不返回 `gcode_bytes` 但返回完整 preview；gcode+preview 的 G-code、statistics、final
map 和 preview 必须来自同一次 slice。`include_gcode=false && preview=none` 返回
`invalid_argument`。

### SLC-07：preview 来源

用 `PreviewProbe` 证明 preview 由本次 `GCodeProcessorResult` 转换，不通过重新解析 G-code。
测试注入一个会让 G-code reparse 与 processor 结果可区分的 move/event case，断言公开 preview
匹配 processor oracle，而不是 G-code 文本 oracle。不得链接或恢复 worker target。

### SLC-08：preview 字段语义

固定 preview fixtures 覆盖 layers、tools、filaments、colors、moves 和 events。逐字段比较
metadata、object/instance 列表、layer print Z/height/duration、tool/nozzle/offset、
filament/tool/color、move type/path kind/extrusion role、start/end/arc、width/height、speed、
temperature、fan、time、print Z 和 joint_angle_end_rad。object/instance 只有可证明时填 ID；
不可证明时必须为空 optional，不能猜测。

### SLC-09：preview 复杂场景

多耗材、多 tool、color change、tool change、support、retract/unretract、wipe tower 和带 joint
angle 的 fixture 必须产生稳定 preview。public enum 映射未知 core 值时返回 `unknown` 加 warning，
不能泄漏 core enum 数值或 worker wire 常量。

## 10. Job、取消和线程测试

### JOB-01：busy

一个 engine 有 active job 时第二次 submit 返回 `busy`，第二个 job 不进入内部执行队列。

### JOB-02：取消阶段

实现状态：`wait_for()` 已进入 actual public header 与实现；其有界等待、轮询、终态复制、
callback 冲突及取消观察语义已由本节 compatibility tests 覆盖。

`LIBSLICER_TEST_CANCEL_TIMEOUT` 和 `LIBSLICER_TEST_POLL_TIMEOUT` 单位固定为毫秒。未设置时测试
使用默认常量：cancel timeout 5000ms，poll timeout 0ms。设置时值必须是非负十进制整数；非法值
或负值属于测试 setup failure，不进入被测 SDK 行为。poll timeout 默认 0ms，用于验证
`wait_for(0ms)` 的非阻塞轮询语义；cancel timeout 用于有界观察取消终态，不能替代
`StageBarrier`。

分别在 queued、preparing、validating、slicing 和 exporting 阶段取消。queued case 先用
另一 engine 的 core task 占用 coordinator，再提交目标 engine job，并等待 StageBarrier 确认其已
进入 queued。cancel 幂等；使用 `LIBSLICER_TEST_CANCEL_TIMEOUT` 调用 `wait_for()` 观察
`cancelled` 终态；不发布部分结果；只有一个 cancelled 终态事件。每个 case 使用内部
`StageBarrier` 等待目标阶段已到达后再 cancel，禁止依靠 sleep 或概率性时序碰撞。
另用 `LIBSLICER_TEST_POLL_TIMEOUT` 验证 job 未终态时 `wait_for()` 成功返回未完成状态，不改变
job，后续仍可等待到同一个最终结果。

### JOB-03：wait

多个线程并发 wait/wait_for 同一 job，completed 时得到同一个 immutable result；失败/取消得到
一致错误。终态后重复
wait/wait_for 不改变结果。`timeout == 0` 作为非阻塞轮询测试；负 timeout 返回
`invalid_argument`，field 固定为 `/timeout`。callback 内调用同一 job 的 wait/wait_for 均返回
`conflict /callback`。

### JOB-04：callback

验证事件顺序、percent 单调、唯一终态、callback 中 cancel、安全释放公开 handle。callback
第一次抛异常后被停用，job 继续；最终 wait diagnostics 恰有一个 code=`internal`、
severity=`warning`、field=`/callback` 的 warning，且不递归产生 warning event。

### JOB-05：生命周期

submit 后释放外部 engine/context handle，job 仍能 wait/cancel；完成结果在 job/engine 销毁后
仍有效。

### JOB-06：公开 handle 并发读取

复制并在多个线程并发读取 `SdkContext`、PresetRepository/PresetView、ProjectSnapshot、
ConfigSchema、EffectiveConfiguration、SliceInspection 和完成后的 SliceResult，结果一致且
无 data race。

### JOB-07：Project 并发入口

用 barrier 同时执行 snapshot、save 和多个 begin_edit。每个成功结果对应完整 revision；旧
expected revision 的 commit 返回 `conflict`；不死锁、不混合 state。

### JOB-08：输入不借用

所有接收 string/string_view/vector/ConfigPatch/mesh DTO/callback 的入口在返回或提交后立即销毁、
覆盖原调用方 buffer，并用 `LifetimeProbe` 验证后续任务不再读取原地址；公开结果仍保持原值。

### JOB-09：初始 context handle 生命周期

分别创建 repository、PresetView、PresetEditor、Project、ProjectEdit、engine 和
SliceInspection 后销毁最初的 SdkContext 以及创建 editor 的 repository/project parent，
再逐个执行 `get/list`、preset commit、snapshot、project commit、inspect、submit/wait 和读取
inspection fields；所有操作仍按正常契约成功。最后一个相关 facade/job lease 释放后由
LifetimeProbe 验证 context state 才关闭。

## 11. 资源限制测试

分别设置刚好允许和差一个单位超限的预算：

- project input bytes；
- archive uncompressed bytes；
- model triangles；
- temporary disk bytes；
- G-code bytes；
- preview bytes；
- preview moves。

首先对每个 limit 字段逐一设置 0，其他字段保持有效，`SdkContext::create()` 必须返回
`invalid_argument`，field 精确为 `/limits/<field>`。

按 API 第 6 节冻结的计数口径构造边界 oracle：

- project input：输入文件实际 byte length；
- archive uncompressed：decompressor 为每个 archive entry 实际产出的 bytes 之和，重名/重复
  entry 每次计入；
- model triangles：成功 materialize 的唯一 mesh geometry facets 数，instance 不重复计数；
- temporary disk：同一时刻全部 SDK-owned temporary files 的 aggregate live-byte high-water，
  覆盖写不累计历史写入量；
- G-code：sink 已 commit 的原始 bytes，总数也即单次结果 high-water；
- preview bytes：memory DTO 或 artifact payload 已 commit bytes；
- preview moves：公开 PreviewMove record 数。

每项超限必须返回 `resource_limit_exceeded`，不发布部分 Project、SliceResult、preview 或
artifact。G-code/preview 超限测试通过 `ResourceProbe` 断言每次 reserve/write/append 前检查，
sink high-water mark 不超过 limit，并证明没有先生成超预算完整输出再 stat/read-back/truncate。
artifact case 还必须证明临时文件未原子发布，取消或超限不留下可被误认为成功结果的文件。

加入 zip-bomb 风格 archive、极大 XML 声明值和恶意 entry size fixture，验证在实际扩张前
拒绝。`ResourceProbe` 分别记录 declared bytes、requested growth 和 committed growth；断言
返回错误时 committed growth 未超过 limit。测试 hooks 仅在 `LIBSLICER_SDK_TESTING` 内部构建
启用，不安装、不改变生产计算分支。

## 12. 验收 Gate

实施分阶段使用以下 gate：

| Gate | 必须通过 |
| --- | --- |
| G1 公共基础 | API-01..05、CFG-01..08 |
| G2 runtime 与预设 | PRE-01..08，包括 explicit-directory system loader、GUI wrapper 回归、single-flight、conflict、retry、lease 和性能 |
| G3 工程 | PRJ-01..13、RES-01..04、MAP-01/03/04/05 |
| G4 切片 | MAP-02、SLC-01..09、JOB-01..09 |
| G5 发布 | 全部资源限制、安装 consumer、完整 ctest |

任何 gate 失败都不能通过删除断言、放宽 baseline、跳过已知 ConfigOptionType 或改写 fixture
规避。若基线行为本身存在缺陷，必须先修改公共 API/内部设计文档并重新审核，而不是让实现
和文档悄悄分叉。
