# libslicer C++ SDK v1 兼容性与验收规范

状态：v1 release Gate。本文不增加公共 API；它验证
[`libslicer_sdk_api.md`](./libslicer_sdk_api.md) 的行为，并约束实现不得偏离 Orca 基线。缺少
已提交 baseline manifest 或冻结 fixtures 时，SDK 只能作为开发预览，不能作为稳定 v1 发布。

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

- `src/slic3r/GUI/GUI_App.cpp:3039` 调用 `PresetBundle::load_presets()`；
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
| unsupported | `SliceEngine::inspect()` 接收 auto filament mode | `/filament_map/mode` |
| io | load 不存在或不可读 path | `/path` |
| resource_limit_exceeded | G-code 写入超过 limit | `/limits/gcode_bytes` |
| invalid_configuration | validate 拒绝已解析配置 | 对应 `/configuration/<OptionId>` |
| cancelled | StageBarrier 到达后 cancel 并 wait | `/job` |
| slicing_failed | test hook 在 validate 成功后的 process/export 注入 core failure | `/slice` |
| internal | test hook 注入未被其他 code 覆盖的 SDK exception | `/internal` |

每项验证：

- 失败 Result 至少一个 error diagnostic；
- `error_code()` 等于第一个主 error 的 code；
- `field` 是对应公开请求字段的稳定 JSON Pointer；
- 失败 Result 调用 `value()` 抛 `std::logic_error`；
- 成功 Result 的 `error_code()` 为空，且可携带 warning；
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

- create/edit/erase user preset 后只允许 `data_dir` 下 SDK-owned user store 发生变化；
  `resources_dir` 和 `preset_dirs` 的递归内容 hash 必须不变；同时用 `PresetIoProbe` 断言所有
  writable open/create/remove/rename/replace 都在 canonical data user store 内，其他 source 没有
  writable open，即使最终内容被恢复也失败；
- `preset_dirs` 中的 preset 以 vendor origin 列出，edit/erase 返回 `unsupported`；
- 对 resources、preset_dirs 和既有 data user profile 分别构造 JSON `name`、cloud `setting_id`、
  `filament_id`、display alias 与 filename 全部不同的 fixture，断言 PresetRef id 只等于解析后的
  JSON `name` bytes；缺少/非法 name 按来源字段返回 `invalid_configuration`；
- 不同 origin 的相同 kind/id 可同时通过各自 PresetRef 读取。duplicate fixtures 分别覆盖同一
  resources、同一 data store、同一 preset dir、resources vendor 与 preset-dir vendor、两个
  preset dirs；都必须在固定扫描顺序下返回 `conflict`，field 精确为后扫描来源对应的
  `/resources_dir`、`/data_dir` 或 `/preset_dirs/{index}`，且没有覆盖；
- 对缺 origin 的 legacy 3MF 字符串验证不按 PresetSummary name、cloud/filament id、filename、
  trim 或 Unicode normalization 匹配；分别验证 embedded → user → vendor → system 的 origin
  选择，断言完整 PresetRef/origin/revision。每个 origin 内覆盖 current canonical id、单级和多级
  `renamed_from` chain、canonical-id 优先于 alias；无匹配返回 `not_found`，同-origin duplicate/
  alias 一对多返回 `conflict`，alias cycle 返回 `invalid_configuration`；都指向来源或对应
  `/project/selected_presets/...`；
- Context 创建后新增、修改或删除外部 preset，既有 repository 视图保持原 catalog snapshot；
  每次变化后都对同一 Context 重新取得 `presets()` 并执行新 list/get，结果仍不刷新；新建
  Context 才观察到变化。该 Context 自己成功 commit 后新 repository 查询立即可见，旧
  PresetView 仍保持原不可变捕获；
- 分别覆盖 edit 后目标被外部修改、删除、同路径替换；create editor 捕获不存在后被外部创建；
  erase 前目标被外部修改或删除。包含同尺寸且恢复原 mtime 的内容修改，全部返回 `conflict`、
  Severity `error`、field `/preset/storage_revision`，且不覆盖外部内容。
- child editor 捕获后由另一 Context 修改/删除任一 user parent，child commit 返回
  `/preset/storage_revision`；另一 Context 修改无关 identity、使捕获的 manifest generation 变化，
  同样冲突。同一 Context 发布任何新 catalog generation 后，旧 editor 返回
  `/expected_revision`。

另验证不可读 resources/preset dir、不可创建或不可写 data user store 返回 `io`，坏 preset 返回
`invalid_configuration`，失败时不发布 Context。field 精确对应 `/resources_dir`、`/data_dir` 或
`/preset_dirs/{index}`；每个 case 同时断言 Severity `error`。

### PRE-07：持久化原子性

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
新增 slot 时未提交完整 project map 或任一已有 local map
返回 `invalid_configuration`；补全全部 map 后成功，且不猜测 ToolId。所有失败均断言工程
revision/state 完全不变。

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
不修改 Project。长度和值域按新 printer/filament selection 校验；非法返回
`invalid_configuration`。公开 API 不允许只传 temporary selection 或只传 map。

## 8. Filament map 测试

### MAP-01：project/plate inheritance

无 plate override 时 source=project；存在 override 时 source=plate；清除 override 后恢复
project。tools 长度等于 logical filament slots，ToolId 为 1 基且在 printer tool 范围内。

### MAP-02：manual baseline

单/多耗材、单/多 tool 的 manual map 通过 SDK 和基线 core 切片，最终 tools、G-code tool
选择和统计一致。

### MAP-03：auto 状态往返

带 `auto_for_flush` 和 `auto_for_match` 的 3MF load/save/reload 后 mode 和已有 tools 不变。

### MAP-04：auto slice 被明确拒绝

两个 auto mode 的 inspect/submit 均返回 `unsupported`；不得产生 SliceInspection、SliceJob、
G-code 或偷偷转换后的 manual map。

### MAP-05：显式改为 manual

App 在 ProjectEdit 中把 auto mode 改为 manual 并提供完整 tools，commit 后可成功 inspect/submit。

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
`gcode_bytes.size()` 与输入长度完全相等并逐字节比较；不追加 NUL、不做编码转换。3MF/XML
无法合法承载的 byte pattern 只用于 adapter conformance test，不伪装成 3MF fixture。

### SLC-05：post-process

非空 post-process 配置在执行任何外部命令前返回 `invalid_configuration`。

## 10. Job、取消和线程测试

### JOB-01：busy

一个 engine 有 active job 时第二次 submit 返回 `busy`，第二个 job 不进入内部执行队列。

### JOB-02：取消阶段

分别在 queued、preparing、validating、slicing 和 exporting 阶段取消。queued case 先用另一
engine 的 core task 占用 coordinator，再提交目标 engine job，并等待 StageBarrier 确认其已
进入 queued。cancel 幂等；wait 返回
`cancelled`；不发布部分结果；只有一个 cancelled 终态事件。每个 case 使用内部
`StageBarrier` 等待目标阶段已到达后再 cancel，禁止依靠 sleep 或概率性时序碰撞。

### JOB-03：wait

多个线程并发 wait 同一 job，completed 时得到同一个 immutable result；失败/取消得到一致
错误。终态后重复 wait 不改变结果。

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

所有接收 string/string_view/vector/ConfigPatch/callback 的入口在返回或提交后立即销毁、覆盖
原调用方 buffer，并用 `LifetimeProbe` 验证后续任务不再读取原地址；公开结果仍保持原值。

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
- G-code bytes。

首先对五个字段逐一设置 0，其他字段保持有效，`SdkContext::create()` 必须返回
`invalid_argument`，field 精确为 `/limits/<field>`。

按 API 第 6 节冻结的计数口径构造边界 oracle：

- project input：输入文件实际 byte length；
- archive uncompressed：decompressor 为每个 archive entry 实际产出的 bytes 之和，重名/重复
  entry 每次计入；
- model triangles：成功 materialize 的唯一 mesh geometry facets 数，instance 不重复计数；
- temporary disk：同一时刻全部 SDK-owned temporary files 的 aggregate live-byte high-water，
  覆盖写不累计历史写入量；
- G-code：sink 已 commit 的原始 bytes，总数也即单次结果 high-water。

每项超限必须返回 `resource_limit_exceeded`，不发布部分 Project 或 SliceResult。G-code 超限
测试通过 `ResourceProbe` 断言每次 reserve/write 前检查，sink high-water mark 不超过 limit，
并证明没有先生成超预算完整输出再 stat/read-back/truncate。

加入 zip-bomb 风格 archive、极大 XML 声明值和恶意 entry size fixture，验证在实际扩张前
拒绝。`ResourceProbe` 分别记录 declared bytes、requested growth 和 committed growth；断言
返回错误时 committed growth 未超过 limit。测试 hooks 仅在 `LIBSLICER_SDK_TESTING` 内部构建
启用，不安装、不改变生产计算分支。

## 12. 验收 Gate

实施分阶段使用以下 gate：

| Gate | 必须通过 |
| --- | --- |
| G1 公共基础 | API-01..05、CFG-01..06 |
| G2 预设 | PRE-01..07 |
| G3 工程 | PRJ-01..10、RES-01..04、MAP-01/03/04/05 |
| G4 切片 | MAP-02、SLC-01..05、JOB-01..09 |
| G5 发布 | 全部资源限制、安装 consumer、完整 ctest |

任何 gate 失败都不能通过删除断言、放宽 baseline、跳过已知 ConfigOptionType 或改写 fixture
规避。若基线行为本身存在缺陷，必须先修改公共 API/内部设计文档并重新审核，而不是让实现
和文档悄悄分叉。
