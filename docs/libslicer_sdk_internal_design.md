# libslicer C++ SDK v1 内部设计

状态：v1 public API freeze candidate 的内部实施说明。本文是非公共契约，可以在不改变
[`libslicer_sdk_api.md`](./libslicer_sdk_api.md) 可观察行为的前提下调整。

实施状态：当前工作树已有 `RuntimeCoordinator`、共享 explicit-directory system loader 和
不可变 schema option index 候选，但整体仍未通过 release Gate。索引修复后 cold create 为
`24.553 s`，其中 `append_collection` 为 `19.275 s`；第二阶段 record conversion 设计尚未实施，
性能 Gate 未通过，不能标记为完成。

## 1. 文档职责

本文描述如何把公共 API 适配到现有 Orca/libslic3r 实现，供开发和代码评审使用。本文中的
内部类名、调用顺序和存储方式不导出、不安装，也不构成 v1 兼容承诺。

内部实现必须满足两条硬边界：

1. 公共头不得包含 `libslic3r`、GUI、wx 或 worker 类型；
2. 不能为了适配方便而改变公共 API 文档规定的配置分层、filament map 或切片行为。

## 2. 内部组件

```text
public v1 facade
  SdkContext / PresetRepository / Project / SliceEngine
                         │
internal implementation │
  RuntimeCoordinator
    ├─ PresetCatalogAdapter
    ├─ Orca3mfAdapter
    ├─ SceneBuilderAdapter
    ├─ OrcaConfigAdapter
    ├─ SliceInputResolver
    ├─ PrintAdapter
    ├─ PreviewAdapter
    └─ BoundedGcodeSink
```

建议职责：

- `RuntimeCoordinator`：绑定唯一 canonical runtime 配置，single-flight 初始化并进程级长期
  持有完整 schema/catalog `SharedRuntime`，同时协调依赖进程级状态或非线程安全 core 的任务；
- `PresetCatalogAdapter`：复用从原 GUI system preset 路径抽取的 libslic3r internal loader，
  增加 SDK 严格校验，并负责 repository preset 的继承、兼容性、编辑和保存；
- `Orca3mfAdapter`：Orca/Bambu project 3MF load/save 与 project state 转换；
- `SceneBuilderAdapter`：把公开 mesh/scene DTO materialize 成内部 Model/ProjectState；
- `OrcaConfigAdapter`：schema、ConfigValue 与 core option 的无损双向转换；
- `SliceInputResolver`：由 SliceEngine inspect/submit 调用，固定 revision 并解析 global/plate 配置；
- `PrintAdapter`：从不可变 snapshot 构造一次性切片状态并驱动 validate/process/export；
- `PreviewAdapter`：把同一次切片产生的 `GCodeProcessorResult` 转换成公共 preview DTO 或 artifact；
- `BoundedGcodeSink`：在 G-code/preview/artifact 增长过程中执行输出预算检查。

这些名称允许修改，但职责不能遗漏。

## 3. RuntimeCoordinator 与进程状态隔离

现有 core 使用 resources/data/temp 路径和部分静态状态。所有触及以下状态的任务必须在统一
协调边界内执行：

- preset 加载、兼容性表达式所依赖的全局资源；
- 3MF importer/exporter；
- Print 构造、apply、validate、process 和 G-code export；
- 可能读取或写入进程级路径的延迟任务。

catalog key 严格是 canonical `resources_dir`、canonical `data_dir` 和保持调用顺序的 canonical
ordered `preset_dirs`。`preset_dirs` 不排序、不去重。`temporary_dir` 与 `ResourceLimits` 均为
`ContextState` local policy，不进入 key。首个 key 由进程级 `RuntimeCoordinator` single-flight
初始化；已有 live `SharedRuntime` 时只有同 key 可以取得 lease，不同 key 返回 `conflict`，
不得并存第二份 catalog、切换已发布 runtime 的全局 resources/data 或启动第二个 loader。

`temporary_dir` 虽不参与 catalog identity，仍必须在 Context create 时 canonicalize/验证。后续
确实读取 core process-global temporary path 的任务，在 core runtime mutex 下激活来源 Context
的 temporary root，并在任务边界恢复；临时文件预算也始终取自该 `ContextState`。不得把第一个
Context 的 temporary root 固化到 `SharedRuntime`，也不得把错误目录当 fallback。

协调器维护以下初始化状态：

```text
Empty -> Initializing -> Ready
             |
             +---- failure ----> publish failure to this attempt's waiters
                                  -> discard attempt -> Empty
```

- `Empty`：没有 live runtime，也没有 initialization attempt；preflight 失败保持 `Empty`；
- `Initializing`：registry 中恰好一个 attempt，持有自己的 canonical key、mutex/condition
  variable、staged candidate 和 attempt result；同 key 调用等待该 attempt，不同 key 立即
  conflict；
- `Ready`：原子发布 `SharedRuntime`，coordinator 持有进程生命周期强引用；后续同 key create
  直接取得同一 state；
- 初始化失败不是持久 coordinator 状态。loader 只向该 attempt 写入一次失败 Result 并唤醒所有
  同波等待者；每个等待者观察逐字段相同 diagnostics。candidate quiescent/销毁后从 registry
  清除 attempt，回到 `Empty`，不保留失败 key。

SDK 内部不得在同一次 create 或等待路径偷偷重试。下一次显式 `SdkContext::create()` 才能创建
新 attempt；它可以使用上次失败的同 key，也可以在没有 live runtime 时使用其他 key。每次 retry
必须从全新 `ConfigSchema`、`PresetBundle` 和 `PresetCatalogState` candidate 开始。上次
attempt-local cache、TBB work 和 loader scratch 必须全部 quiescent/销毁，并通过 probe 验证没有
可见 preset、成功 generation 或残留 mutable state。若 core 失败后不能恢复到可证明的干净状态，
当前 attempt 必须失败并保持不发布；实现必须修复 reset 边界，不能用 fallback catalog 或内部
循环重试掩盖。

`SharedRuntime` 至少持有：

- canonical catalog key；
- immutable FFF `ConfigSchema`；
- 完整 `PresetCatalogState`、`PresetBundle`、identity/alias authority 和 catalog generation；
- user-store revision/transaction authority；
- 成功 generation、性能分段和测试可观察计数。

每次成功 create 产生独立 `ContextState`，它持有 `shared_ptr<SharedRuntime>`、本次 canonical
`temporary_dir` 和 `ContextOptions::limits`。Project、ProjectSnapshot、SliceEngine 和 SliceJob
沿其来源 `ContextState` 传播 temporary root/limits，不能把另一个共享 runtime Context 的策略
拿来执行。

初始化使用 staged state：schema、core bundle、catalog、user store/preset_dirs merge、strict
diagnostics 和 alias/dependency authority 全部只属于 attempt。所有步骤成功后才在 registry mutex
下把 candidate 原子变为 live `SharedRuntime` 并递增成功 generation。任何失败都不发布
default-only、空或部分 catalog，也不递增成功 generation。

锁边界固定如下：

- **registry mutex**：只检查 live key、查找/登记 attempt、发布/移除 attempt 或
  `SharedRuntime`；绝不在其下 canonicalize、做 I/O、调用 core loader 或等待；
- **attempt mutex/condition variable**：只保存单次 attempt 的完成状态和 success/failure Result；
  同 key 等待者在这里等待，等待前不持有 registry/catalog mutex；
- **core runtime mutex**：保护 Orca process-global resources/data/temporary 激活、core loader 及
  其他依赖非线程安全 core 全局状态的任务；loader I/O 只允许发生在该边界内，不持有 registry
  或已发布 catalog mutex；
- **catalog mutex**：只保护已经发布 catalog 的 generation/transaction/read view；初始化
  candidate 不使用已发布 catalog mutex，且禁止持有 catalog mutex 等待 attempt 或 core task。

不得用一个覆盖所有层的全局锁替代这些边界。实现和测试必须证明 registry/catalog mutex 下没有
loader I/O，也没有 condition-variable/future wait。

协调器仍可使用全局 FIFO 队列串行化真正依赖非线程安全 core 状态的后续任务。公共
`SliceEngine` 的 one-active-job/busy 语义在进入内部队列前判定；内部队列不能被解释成第二个
公开 job 队列。任何 detached core/TBB work 都不得越过协调任务或初始化 candidate 生命周期。

context state 采用共享所有权。`SdkContext`、`PresetRepository`、`PresetView`、`PresetEditor`、
`Project`、`ProjectSnapshot`、`ProjectEdit`、`ProjectBuilder`、`SliceEngine`、`SliceInspection`
和已提交 SliceJob 都持有其操作所需的 `ContextState`/`SharedRuntime` lease；销毁最初的
SdkContext 或 parent facade 不关闭 state，这些公开 handle 仍可按 API 查询或提交事务。最后一个
公开 handle 释放后，context-local policy 可以销毁，但 coordinator 对成功 `SharedRuntime` 的
强引用保留到进程结束，下一次 create 不得因此重新加载 catalog。

## 4. ConfigValue 与 core option 转换

内部 schema 不能只由单一配置定义表生成，权威来源分为三层：

1. FFF `ConfigOptionDef` 提供类型、nullable、默认值、数值范围、单位和 enum 候选；
2. preset option lists、object/region config definitions 提供 allowed scopes 和 applicable
   preset kinds；
3. versioned `FffOptionRuleRegistry` 提供可见性和兼容性求值规则。首版 registry 必须从抽库前
   GUI 的 ConfigManipulation/Tab show-hide 规则及 preset compatibility 表达式逐项迁移，并以
   OptionId 为 key 维护；registry revision 与公开 schema version 绑定。

`FffOptionRuleRegistry` 是内部无 GUI 依赖的数据/谓词表，不复制 wx 控件代码。每个公开 editable
FFF option 必须有显式规则条目或显式 `always_visible`/`core_validation_only` 标记，禁止缺省时
静默猜测。公开 `ConfigSchema::evaluate_option()` 先合并 base 与 candidate patch，再把 context 中固定
revision 的 printer/process effective values 和 filament count 一起交给 registry，从而覆盖
printer capability、gcode flavor、chamber control 和耗材数量等跨 preset 规则。缺少当前规则
所需 context 返回 `invalid_argument`，不得用默认 printer 猜测。preset compatibility 仍使用
固定 revision 的真实 preset compatibility expression。adapter conformance tests 枚举全部公开
option，保证规则覆盖率为 100%。

### 4.1 不可变 OptionId descriptor index

第一阶段 profiling 曾发现 `ConfigSchema::find()` 对 `ConfigSchema::State::options` 执行线性
扫描，`core_config_diff_to_patch()` 又对每个 effective option 调用该查询。当前工作树已经有
不可变 index 候选，但尚未完成 release 验收；`Preset.cpp` 仍不得复制 option ownership、
descriptor 或 core value 转换逻辑。

`ConfigSchema::State` 必须在 schema 构建时一次性建立
`OptionId -> OptionDescriptor` O(1) index，设计固定如下：

- `State` 同时拥有按既有顺序保存的 immutable `std::vector<OptionDescriptor>` 和以
  `OptionId::value()` 完整 bytes 为 key 的 private hash index。index value 保存 vector index，
  不保存会因 vector move/reallocation 失效的 owning/raw pointer，也不复制 descriptor；
- `detail::ConfigSchemaAccess::make()` 在 vector 最终定型后构建完整 index，随后才创建并发布
  `shared_ptr<const State>`。构建后 options 和 index 都不可变，不使用 lazy cache、`call_once`、
  读时补表或 fallback 线性扫描；
- 相同 OptionId 第二次插入时 schema 构建立即失败，固定返回 `ErrorCode::internal`，field 为
  `/schema/options/<escaped OptionId>`；不得采用 first-wins、last-wins、覆盖或保留一个线性
  fallback。`ConfigSchemaAccess::make()` 的 private/internal 构建接口必须能够把该失败传回
  `build_orca_fff_schema()`，失败 schema 不进入 runtime candidate；
- public `ConfigSchema::find()` 通过同一 index 定位 vector 中的稳定 entry，再按原 public
  contract 返回 `std::optional<OptionDescriptor>` 的值拷贝；不存在仍返回 `std::nullopt`，
  `options()` 的内容和顺序完全不变；
- private `ConfigSchemaAccess` 可以提供只读 descriptor pointer/reference lookup，使
  `core_config_diff_to_patch()` 避免重复复制 descriptor。该引用只指向共享 `const State` 拥有的
  vector entry，生命周期不超过其 `ConfigSchema`/state lease，不得缓存到 catalog state 之外；
- `core_config_diff_to_patch()`、`ConfigSchema::validate_structure()`、
  `ConfigSchema::evaluate_option()` 及其他现有 `schema.find()` 热路径必须复用这一份 index。
  `OrcaConfigAdapter.cpp` 继续独占 core option 与 `ConfigValue` 的转换权威；
  `Preset.cpp` 只调用 adapter，不新增或复制转换分支；
- `ConfigSchema` copy 继续共享同一 `shared_ptr<const State>`。index 在 state 发布前完成，所有查询
  只读、无锁、无 mutation，因而同一或复制 schema 上的并发查询不产生 data race，也不需要每线程
  cache。

第一阶段只允许上述索引优化。其候选实现后的 profiling 已触发并完成第二阶段设计审核前置条件；
第二阶段边界见 4.2。两个阶段均不得改变 eager full catalog、strict/no-fallback、
Orca-first + non-Orca vendor parallel parse + stable merge、`RuntimeCoordinator`、public record
内容或排序。

### 4.2 有界、确定性的 catalog record conversion

第二阶段只优化 `append_collection()` 的重复转换和串行执行，不重开 public API、loader 或
runtime 架构。修复后 profiling 为：cold `24.553 s`、loader `5.262 s`、append `19.275 s`；
append 中 printer `1.709 s`、process `9.082 s`、filament `8.484 s`。sampled CPU time 分别为
inherited `5.783 CPU-s`、effective `5.808 CPU-s`、diff `5.190 CPU-s`。只共享 inherited values
的理论 cold 约 `18.77 s`，不足以通过 `15 s` Gate，因此必须采用以下完整方案。

#### 4.2.1 单线程冻结输入

主线程必须按既有 printer、process、filament collection 顺序及各 collection 既有顺序遍历，
为每条 record 分配连续、唯一的 global ordinal，并分别预分配 `EffectiveArtifactSlot` 与
`RecordCandidateStatus`，再构造只含 source pointer/小 metadata 的 immutable
`FrozenPresetRecordInput`。artifact slot 保存 effective conversion Result；candidate status
保存该 ordinal 的 record-local errors 和可选完整 record candidate。

所有可归属 record 的错误，包括 source freeze、parent/default resolution、identity、duplicate、
revision metadata 和后续 conversion 错误，都必须记录到该 global ordinal 的 result slot。freeze
不得因 record-local 错误提前返回。duplicate 可在单线程 freeze 中检测，但只能把失败写入当前
ordinal status，不能插入或发布 record；随后继续处理所有仍可安全处理的 ordinal。metadata、
revision、identity 或 duplicate failure 不阻止本 ordinal 的 effective artifact conversion，也
不阻止 child 使用该 artifact。只有 parent effective artifact conversion 失败时，child status
才记录确定性的 dependency conversion failure；不受影响的 dependency branch 继续
freeze/convert。最终 public failure 统一在全部可安全处理的工作完成后按 ordinal 选择，最小
失败 ordinal 是唯一 primary；同一 ordinal 有多个错误时保持旧串行 oracle 的错误类型优先级。
只有无法归属任何 ordinal 的 allocator failure、
`tbb::task_arena` construction failure 或 attempt-global arena/init failure 才允许立即返回。

任何 worker 启动前，主线程必须完成所有 lazy parent/default resolution，并触发、完成所有可能
创建 core option 或内部 cache 的 helper。至少包括 `get_preset_parent()`、`Preset::inherits()`、
default config resolution 及其等价 helper。冻结后：

- 每个成功 frozen input 固定 bundle/catalog attempt lifetime lease 下的 stable const
  `Slic3r::DynamicPrintConfig`/Preset config pointer、parent/default authority identity、parent
  global ordinal、dependency level、revision、public `PresetRef`、summary metadata 和 global
  ordinal；
- bundle、collections、Preset config 和 default config 从第一个 worker 启动前开始保持
  immutable，直到所有 dependency batch join、arena 销毁、slot 检查和 commit 全部完成；
- worker 只允许直接对 frozen config 执行 const `keys()`/`optptr()` 读取。禁止调用 `Preset`
  方法、parent/default lookup、可能创建 option/cache 的 core helper，或回到 collection/catalog
  重新解析 identity；
- frozen inputs 不持有完整 `ConfigValues`，也不得一次性 owning-copy 全部 records/configs。
  若某 config 不能证明 attempt 内稳定 immutable，主线程只在其 dependency batch 启动前为最多
  `4` 个 in-flight record 创建 owning copy，把 copy lifetime 绑定到该 batch，并在 batch join 后
  立即释放。copy 前不得再执行 lazy helper。

parent effective dependency 必须在 freeze 阶段形成确定 DAG。已有 record metadata/duplicate
failure 的 ordinal 仍进入 artifact worker；只有 artifact conversion 自身失败才阻断 dependent
artifact。其余 artifact 按 dependency batch 调度；parent 的 successful effective
`ConfigValues` 只从 parent `EffectiveArtifactSlot` 读取并作为 batch-local immutable handle 传给
child conversion，不写回 frozen input。child ordinal 可以小于 parent ordinal：scheduler 可先
计算 parent artifact，再计算 child，但两者都不得在最终 status 扫描前发布 record。

#### 4.2.2 immutable ConfigValues 共享与 fused conversion

child 的 inherited values 必须直接复制其真实 parent record 的 effective `ConfigValues` value
handle，从而共享同一个 immutable `ConfigValues::State`；值语义与原 child inherited conversion
完全相同。root/default inherited values 以冻结的实际 default config authority identity 为 key，
每个 identity 只转换一次并共享同一个 `ConfigValues` immutable state。每个 default identity 的
转换 owner 固定为 freeze 成功解析该 authority 的最小 global ordinal
`EffectiveArtifactSlot`；即使该 record 随后因其他 record-local 原因失败，default values 仍由
artifact slot 独立于 `RecordCandidateStatus` 持有。其他 artifact slots 只复制该 immutable
handle，不允许建立独立 public candidate cache。
identity 必须来自实际 core default authority，不得按 preset 名称、vendor、kind 或其他启发式
字段猜测。默认只在 authority identity 完全相同时共享；若实现采用 content-addressed identity，
则建立 identity 前还必须比较完整 config 值相等。不同 identity 或值不相等时绝对禁止跨 config
共享。

`OrcaConfigAdapter` 增加单一 internal fused conversion。它对每条 effective config 只遍历一次，
同时产生：

- 完整、原顺序的 immutable effective `ConfigValues`；
- 相对 inherited/default baseline 的 changed generic `ConfigPatch`。

changed option 必须复用本次 effective conversion 生成的同一个 `ConfigValue`，不得再次执行
core option conversion。fused 结果的 success/failure、primary diagnostic、全部 diagnostics 顺序、
OptionId 顺序、shape/nullability/value、round-trip 行为必须与旧
`core_config_to_values()` + `core_config_diff_to_patch()` oracle 完全相同。adapter 继续是唯一
core-to-public value conversion 权威，`Preset.cpp` 只组织冻结、调度和提交。

#### 4.2.3 有界 worker 与确定性提交

system loader 使用的全部 TBB 工作必须已经 join 并达到 quiescent，随后才允许创建 record
executor。record executor 固定为独立
`tbb::task_arena(max_concurrency=min(configured_test_limit, record_count))`；production
`configured_test_limit` 编译期固定为 `4`，永远不能由 `hardware_concurrency()`、环境变量或
调用方改变。测试构建可通过 4.2.4 的 internal seam 固定为 `1`、`2`、`4`。

每个 dependency batch 在该 arena 内使用 `parallel_for`/`parallel_invoke`；调用返回并确认本批
join 后，主线程才准备下一批。adapter 内部不得创建 task、arena 或其他并行。worker 只读 frozen
input、batch-local owning copy/parent handle、immutable schema 和 `print_config_def`，只写预分配
的本 ordinal `EffectiveArtifactSlot`/`RecordCandidateStatus`。worker 内任何异常都捕获并转换为
该 ordinal 的 record-local failure；
异常不得越过 arena 边界。所有 batch 必须在 arena scope 离开前 join；arena 销毁后才允许检查
commit 条件。

`RecordCandidateStatus` 是唯一 public record candidate staging owner；
`EffectiveArtifactSlot` 只拥有 effective/default DAG artifacts。frozen inputs 只持 source
pointer/小 metadata，不持 inherited/effective/default `ConfigValues` 或完整 candidate。changed
patch 的 `ConfigValue` 共享 effective entry state，parent/default 的 `ConfigValues` 共享只发生在
artifact slots 与最终 candidate 之间。除最多 `4` 个 batch-local owning config copies 和每
worker 一份有界、非 public-candidate 的 conversion scratch 外，不得创建第二份全量
record/config staging。worker 直接在对应 slots 内完成 artifact/candidate，batch join 后释放
scratch/copies。

arena 销毁后，主线程按 global ordinal 扫描所有 candidate statuses。freeze、parent/default、identity、
duplicate 和 conversion failure 使用同一选择规则；即使较大 ordinal 更早完成，仍只以最小失败
ordinal 作为唯一 public primary error。任一失败都丢弃全部 candidate statuses/artifact slots，
`catalog.records`、generation 和 `next_revision` 保持未发布状态。全部成功时，
`catalog.records` 必须初始为空，主线程严格按 ordinal 将 candidate 从 statuses move/emplace 到
该空容器，再按旧串行路径提交 revision/generation；record 全字段、diagnostics、duplicate
语义、排序和 frozen catalog digest 必须不变。move 完成后必须清空 artifact slots 的 auxiliary
default/effective handles；commit 后 statuses/artifact slots 为空，不保留第二份 public
candidate/state owner。

#### 4.2.4 Test-only scheduler seam

scheduler test seam 固定放在现有 `src/libslicer_sdk/PresetInternal.hpp`。声明与实现只能在
`LIBSLICER_SDK_TESTING` 下编译；该宏只由 `tests/libslicer_sdk/CMakeLists.txt` 对 test SDK build
私有定义，不进入普通 production build。该 internal header 不安装、不导出，seam 不读取环境
变量，production 不存在运行时开关。

seam 只允许测试固定 worker limit 为 `1`、`2`、`4`，注入指定 ordinal 的 freeze/conversion/
duplicate failure，并读取 parent/default lazy-access、catalog/core mutation、in-flight candidate、
arena join/quiescence、effective-artifact DAG 和 candidate-status ownership probes。不得允许
测试替换 public API、跳过 strict validation、改变 loader 或把任意 worker 数带入 production。

### 4.3 Core option shape mapping

每个已知 option 必须映射到唯一公开 shape：

| core 类型族 | SDK shape |
| --- | --- |
| bool/bools | boolean / list<boolean> |
| int/ints | integer / list<integer> |
| float/floats | decimal / list<decimal> |
| percent/percents | percent / list<percent> |
| float-or-percent(s) | float_or_percent / list<float_or_percent> |
| string/strings | string / list<string> |
| enum/enums | enumeration / list<enumeration> |
| point/points | point2 / list<point2> |
| point3 | point3 |
| points groups | list<list<point2>> |
| ints groups | list<list<integer>> |

nullable scalar 映射为 value nullable；nullable vector 映射为 list item nullable。空 list、空
group、nil item、纯 percent 和 float-or-percent 的 percent flag 必须原样往返。未来未知
option type 返回 `unsupported`，不能跳过或伪装成 string。

filament reference 的公开规范化是 adapter 的职责，而不是 core 类型直接映射。内部使用按
canonical OptionId 索引、随 schema version 冻结的规则表：

```cpp
enum class CoreReferenceEncoding {
  zero_based,
  one_based_zero_sentinel
};

struct FilamentReferenceRule {
  FilamentReferenceKind public_kind;
  CoreReferenceEncoding core_encoding;
  bool value_nullable;
  bool item_nullable;
};
```

- `FffOptionRuleRegistry` 为所有 logical-slot reference option 标记
  `FilamentReferenceKind::logical_slot` 或 `logical_slot_list`；
- core 中“0 表示默认/自动、正数表示 1 基耗材”的 scalar 在读出时转换为 public null 或
  `value - 1`，schema 对这类 option 报告 `value_nullable=true` 且非 null minimum 为 0；写回时
  public null 转成 core 0，public slot `n` 转成 core `n + 1`；
- list 采用同样规则，但默认项通过 item nullability 表达；
- core 中已是 0 基的 reference 仍对外保持原数值，但该事实不进入公开 descriptor；
- `filament_map` 由强类型 map adapter 单独转换 logical-slot-index → 1 基 ToolId，不得被错误标记
  为 logical-slot reference，也不进入 generic patch。

adapter 只能查上述规则表，禁止根据 option 名称、后缀或 public kind 猜测内部编码。legacy alias
必须先 canonicalize，再用 canonical key 查表。首版 schema 的
`one_based_zero_sentinel` canonical key 完整集合固定为：`extruder`、`support_filament`、
`support_interface_filament`、
`wipe_tower_filament`、`outer_wall_filament_id`、`inner_wall_filament_id`、
`sparse_infill_filament_id`、`internal_solid_filament_id`、`top_surface_filament_id`、
`bottom_surface_filament_id`。`filament_map`、`physical_extruder_map`、`master_extruder_id`、
`printer_extruder_id` 和 `print_extruder_id` 是 map/physical-tool/internal 数据，不能登记为 logical
slot reference。新增/删除 canonical key 必须更新 schema version 和独立测试 manifest。

adapter 转换阶段只校验类型、整数溢出、负值和 nullability；repository preset 没有当前 selection，
不能在加载/转换时猜 slot count。slot 上界仅在 `ConfigValidationContext::filament_count` 有值、
ProjectEdit commit、SlotRemap、SliceEngine inspect/submit 或其他已绑定明确 selection 的阶段校验。公开 diagnostic
只描述 0 基 logical slot/null 语义，不出现内部编码术语。

内部转换器分为：

1. schema descriptor 构建；
2. core option → ConfigValue；
3. ConfigValue → core option；
4. core config diff → ConfigPatch；
5. `validate_and_normalize()` 的单一校验/规范化 pipeline；
6. ConfigPatch 在指定 scope 的应用。

adapter 内部维护 `OptionChannel`（generic/preset-selection/filament-map/structural）以路由 core
keys，但不写入 `OptionDescriptor`，也不安装该枚举。只有 internal channel 为 generic 且
scope 合法的 option 可通过 ConfigPatch；其他字段在公共边界拒绝。

`inherits`、preset identity、project entity identity、filament map 和 preset selection 不走
generic patch 通道。

JSON codec 单独实现并只由公开 `<libslicer/v1/json.hpp>` 声明；核心 Config/Project/Slice headers
不包含 JSON 库或 codec 声明。

## 5. Preset adapter

### 5.1 原 GUI preset 生命周期复用

`SdkContext::create()` 在首次 canonical runtime 初始化中完整建立 catalog。实现基线来自历史
GUI baseline `71c30dd1feb73efa40d824ee1e4be05fce7b0827` 的应用初始化阶段：
`src/slic3r/GUI/GUI_App.cpp:3039` 调用长期持有的 `PresetBundle::load_presets()`。当前最小
libslicer 工作树已删除 GUI 可执行源码，因此该行只证明 GUI 生命周期，不证明当前 headless SDK
已复用 loader，也不授权 SDK 调用完整 `PresetBundle::load_presets(AppConfig&)`。该完整入口包含
GUI user preset、selection、配置迁移等生命周期；SDK **禁止调用**，只允许调用下文冻结的显式
目录 system loader。

当前 core 算法必须按实际代码引用：

- `src/libslic3r/PresetBundle.cpp:510-540` 的 `PresetBundle::load_presets()` 在 `:520` 调用
  `load_system_presets_from_json()`，随后加载 user presets、更新兼容性并恢复 selection；
- `:2165-2287` 是当前 `load_system_presets_from_json()`；`:2198-2223` 分离并同步先加载
  `OrcaFilamentLibrary`；
- `:2225-2247` 为其他 vendor 各建独立 `PresetBundle` 并通过 `tbb::parallel_for` 解析；
- `:2249-2277` 按 `other_vendors` 索引顺序顺序 merge，实际 merge helper 位于
  `:2401-2420`；
- 当前 `:2187-2197` 从 `directory_iterator` 收集 vendor name，未显式排序。因此
  Orca-first/并行解析/索引顺序 merge 是当前事实，而“跨文件系统确定性 vendor 顺序”仍是待实施
  约束，不能提前宣称已经由 current core 保证。

从现有 `load_system_presets_from_json()` 抽取的共享实现冻结为以下 libslic3r internal 接口。
名称和语义属于 `Slic3r` core，不位于 `libslicer::v1`、不依赖任何 libslicer 类型、不安装，也不
进入 public headers：

```cpp
enum class SystemPresetLoadPolicy {
    GuiBestEffort,
    SdkStrict
};

enum class SystemPresetIssueKind {
    io,
    parse,
    duplicate,
    alias_cycle,
    alias_ambiguous,
    missing_dependency,
    invalid_identity
};

struct SystemPresetLoadIssue {
    SystemPresetIssueKind kind;
    std::string vendor;
    std::filesystem::path path;
    std::string message;
};

struct SystemPresetLoadResult {
    PresetsConfigSubstitutions substitutions;
    std::vector<SystemPresetLoadIssue> issues;
};

SystemPresetLoadResult PresetBundle::load_system_presets_from_json_at(
    const std::filesystem::path &profiles_dir,
    ForwardCompatibilitySubstitutionRule compatibility_rule,
    SystemPresetLoadPolicy policy);
```

`profiles_dir` 是已经验证的显式 profiles 根目录；共享实现禁止从 GUI/SDK 全局路径重新推导目录。
现有 `load_system_presets_from_json(compatibility_rule)` 保留为 GUI wrapper：它传入 GUI
`data_dir/system` 和 `GuiBestEffort`，再把结构化 issues 按稳定顺序转换回现有 cumulative error
表现，保持 substitutions、累计错误、继续加载和 GUI lifecycle 不变。SDK adapter 直接传入
canonical `resources_dir/profiles`、相同 compatibility rule 和 `SdkStrict`，不得经过 GUI
wrapper，更不得调用完整 `load_presets(AppConfig&)`。

两种 policy 使用同一 parser、Orca-first、并行和 merge 实现，不允许复制 loader：

- `OrcaFilamentLibrary` 必须同步完成；其失败产生结构化 issue，不能启动其他 vendor；
- non-Orca vendor 先按 canonical vendor key 的 UTF-8 byte 顺序排序，再以独立
  `PresetBundle` 并行解析；全部 parse quiescent 后按同一排序顺序 merge；
- 每个 issue 都填写 `kind`、`vendor`、`path` 和非空 `message`。存在的目标使用 canonical path；
  缺失目标使用 canonical `profiles_dir` 下的 lexically-normalized expected path。issues 以 vendor
  key、path UTF-8 bytes、kind、message 的固定顺序返回，不能采用线程完成顺序；
- `GuiBestEffort` 保持现有累计行为；单个 vendor issue 不改变 GUI wrapper 继续处理其余 vendor
  的既有行为；
- `SdkStrict` 中任一 issue 都使 system load 失败。adapter 的映射固定为
  `io -> ErrorCode::io`、`parse -> ErrorCode::invalid_configuration`、
  `duplicate -> ErrorCode::conflict`、`alias_cycle -> ErrorCode::invalid_configuration`、
  `alias_ambiguous -> ErrorCode::conflict`、`missing_dependency -> ErrorCode::not_found`、
  `invalid_identity -> ErrorCode::invalid_configuration`；这些 system-loader diagnostics 的 field
  精确为 `/resources_dir`，message 原样保留 core issue `message`。SDK 等待者取得同一排序后的
  diagnostics；
- `SdkStrict` issue、后续 SDK strict validation issue 或 user/preset-dir issue 都只能写入
  attempt candidate；任一 issue 存在时 staged catalog 不发布，不能返回 Orca-only、
  default-only、空或部分 catalog。

```text
RuntimeCoordinator first create
  -> validate/canonicalize ContextOptions
  -> create private ConfigSchema + PresetCatalogState candidate
  -> configure core resources/data for the canonical catalog key
  -> invoke load_system_presets_from_json_at(resources_dir/profiles, rule, SdkStrict)
       -> load OrcaFilamentLibrary synchronously
       -> load remaining vendors in independent bundles via core TBB path
       -> merge in stable, deterministic vendor order
  -> load SDK-owned user store
  -> load ordered preset_dirs through SDK read-only adapter
  -> validate identity/alias/dependency/user-store/no-fallback contract
  -> atomically publish SharedRuntime
```

`PresetCatalogAdapter` 负责准备 SDK-owned data 布局并在 core system load 前后做适配，但不得
构造 headless `AppConfig` 或调用完整 `PresetBundle::load_presets(AppConfig&)`；必须调用上述
显式目录 internal loader，不得保留
`Preset.cpp` 中 SDK 自己枚举全部 vendor、逐个创建 bundle、串行 load/merge 的第二套实现。
core 已有并行加载是被复用的 GUI 实现，不是为 SDK 临时增加的性能补丁。

core loader 的错误累计行为不能弱化 SDK 契约。adapter 必须把任何 vendor parse/load error、
缺失 `OrcaFilamentLibrary`、duplicate identity、alias 歧义/循环、缺 dependency、非法 user
store 或只得到 default bundle 的情况转换成精确失败；只有完整合法 catalog 才能发布。合法
catalog 中的 Orca default presets 正常保留，但不能作为资源失败 fallback。

加载和 merge 顺序必须可重复。在进入 parallel parse 前必须按 canonical vendor key 的 UTF-8 byte
顺序固定 `other_vendors`，并以同一索引顺序 merge。测试 fixture 随机化目录枚举顺序时，最终
公开 identity、revision、diagnostics 和 catalog digest 必须相同；如 current core 入口不能接收
稳定输入，实施必须在原 core loader 的 vendor 收集边界补齐确定性排序，不能在 SDK 重写 vendor
parser、另建串行 loader，或把现有未排序目录枚举写成已保证确定性。

### 5.2 Catalog 发布与后续复用

首次 loader 在私有 candidate 中完成全部 schema/catalog/user-store 建立和严格验证。只有最后
一个成功点把 candidate 作为 `SharedRuntime` 原子发布；失败 candidate 对
`SdkContext::presets()`、Project、inspect 和 slice 全部不可见。

成功后：

- `RuntimeCoordinator` 长期强持有 `SharedRuntime`；
- `SdkContext::presets()` 只构造共享 catalog 上的 repository handle，不做 I/O 或 vendor load；
- `Project::load()`、ProjectBuilder、ProjectEdit、inspect 和 submit 从其 Context lease 取得同一
  catalog/revision authority，不重新初始化；
- user preset transaction 在同一共享 catalog 上原子发布新 generation；旧 PresetView、
  ProjectSnapshot 和其他不可变捕获保持原 revision；
- external file change 不被既有 runtime 自动重扫；SDK 不提供运行时 resources 切换。

初始化 instrumentation 使用 monotonic clock，至少记录 schema、core system preset load、
OrcaFilamentLibrary、并行 vendor load、稳定 merge、user/preset_dirs load、SDK strict validation、
printer/process/filament record conversion 和 publish。当前同一 GPlatform 测试机、Debug build、
production resources 的 schema-index 修复后现状是：cold create `24.553 s`、system loader
`5.262 s`、`append_collection` `19.275 s`；append 中 printer `1.709 s`、process `9.082 s`、
filament `8.484 s`；sampling profile 中 inherited/effective/diff 分别为 `5.783`/`5.808`/
`5.190 CPU-s`。wall time 和 CPU time 是不同口径，只用于定位热点，不能直接相减。cold create
必须 `<= 15000 ms`，Ready 后同 key create 必须 `<= 100 ms`，cold create 进程 peak RSS 必须
`<= 2.2 GB`，且每进程 system loader invocation count 必须为 1。所有阈值单位、计时边界和测试
机器/资源集记录要求以 API 第 6 节与 compatibility PRE-07 为准。后台启动只能改善交互时机，
不能把超过 Gate 的初始化标记为通过。

release Gate 使用新增的 dedicated `libslicer_sdk_cold_gate` test executable，不复用 Catch 主
进程，也不以 shell `time` 为 authority。CLI 固定为
`libslicer_sdk_cold_gate` 加
`--resources <path> --data-root <path> --fixture <firehorse.3mf> --result <json>`。该 executable
在一次全新进程内完成 cold create、same-key Ready reuse、Ready runtime 上的
`Project::load(firehorse.3mf)`、loader count 和 peak RSS 判定，写出含
`project_load_ms` 的结构化结果并以退出码表达 Gate；CTest 注册三个独立实例，均设置
`RUN_SERIAL TRUE` 和 `TIMEOUT 60`。平台 RSS API、单位归一化、结果字段和三次全通过规则见
compatibility PRE-07。

### 5.3 User store generations

Runtime 不创建 file watcher，也不在 repository 查询或 Project load 时重扫外部目录。成功的 SDK
user transaction 在持久化后原子发布新的 repository generation；外部文件变化不进入既有
runtime snapshot，已经发布的不可变 Project/PresetView 继续引用旧 generation。

SDK-owned user store 使用 immutable content files 加 generation manifest（或具有相同单一原子
提交点的等价布局）。manifest 是新 Context 唯一读取的 catalog 索引；unreferenced staging/旧
content 不可被加载。每个 target fingerprint 至少包含存在状态和完整 payload bytes 的 SHA-256，
不得只依赖 path、size 或 mtime。

PresetEditor 捕获 target、全部 user-origin parent chain、create identity 的存在/不存在状态以及
user-store manifest generation 的 storage fingerprints；同一 Context 的 preset/catalog revision
另外保存。跨进程锁内必须重新读取并比较全部 storage fingerprints，而不只比较 target。任一
外部 storage 变化映射 `/preset/storage_revision`，同 Context revision/generation 变化映射
`/expected_revision`。

所有 SDK Context/进程以 canonical user-store path 为键获取跨进程排他锁；同进程协调不能替代
该锁。锁内重新读取 manifest/fingerprint，并覆盖依赖验证、payload/manifest staging、文件
flush/close、manifest 原子 replace、父目录 durable flush 和内存 catalog generation 发布。
create 验证 identity 仍不存在，edit/erase 验证 identity 和完整 bytes 仍匹配捕获版本。erase 通过
新 manifest 删除映射，不直接删除当前可见 payload；旧 content 只能在提交后回收。

所有可能使 API 返回失败的 staging、payload flush/close、manifest 构造和权限检查都必须在
manifest 原子 replace 之前完成；该 replace 是唯一逻辑提交点。replace 成功后事务返回成功并
发布内存 generation。提交点后的父目录 flush/旧 content 回收采用 best effort；其中一个或
两者失败都合并成 success Result 中恰好一个
`{ErrorCode::io, Severity::warning, ..., "/data_dir"}` diagnostic，不能把已经提交的新 generation
报成失败。
v1 保证进程可观察的原子 catalog 语义，不承诺突然断电后的物理 durability。

任一提交点前失败必须清理 staging，保持旧 manifest/catalog/revision；提交点后的 content 回收
失败不回滚成功事务，但遗留文件必须是 manifest 不可达且下次加载忽略的内部文件。SDK 永不以
writable mode 打开或修改 `resources_dir`/`preset_dirs`。不遵守该锁协议的外部 writer 只在
transaction 锁内重读发生于其修改之后时保证被检测。

repository 内部保存 catalog generation，并为每个公开 PresetRevision 建立稳定映射。
PresetEditor 打开时复制：

- 当前 preset metadata 和 overrides；
- 完整 parent chain 及各 revision；
- kind defaults；
- schema generation。

`PresetEditor::snapshot()` 从这份捕获生成 `PresetView`；metadata/inherited/effective/overrides
不在 editor 上重复实现 getter。commit 在同一个协调任务中重新
验证 preset、自身父链、`(kind, user, id)` identity 占用和 schema generation，成功后原子保存并
发布新 revision。create 的 id/display name 按公共 UTF-8/NUL 规则验证，display name 不参与唯一性。

`PresetRepository::get()` 为 system/vendor/user 创建相同的不可变捕获，但不创建写事务；
PresetView 因而能完整读取任意 repository preset，且不需要绕过 system/vendor 的只读限制。

兼容性查询使用 candidate 和显式 context 的固定 revision，调用现有兼容表达式计算。不得
从 GUI active state 隐式读取 printer/process。

project-embedded presets 存在 Project state 中，不能混入 repository catalog。

## 6. Orca 3MF project state

`Orca3mfAdapter` 将一次 load 的全部切片相关状态存入内部不可变 `ProjectState`：

- project config、selected presets 和 embedded presets；
- selected preset revisions 对共享 runtime catalog generation 的不可变捕获；
- plates、plate config、custom G-code 和 filament map；
- Model、objects、volumes/parts、instances 和 layer ranges；
- importer 能保留但公共 API 未暴露的 Orca metadata；
- project identity 和 revision。

instance transform/printability、custom G-code 和 project-embedded preset 的完整状态不再映射为
v1 核心公共 CRUD，但绝不能从 ProjectState 删除：load/save 必须 round-trip，target-plate 准备与
PrintAdapter 必须消费，SlotRemap 必须迁移或明确拒绝无法安全迁移的数据。

公开 entity ID 的 binding 只包含 project identity 与持久 entity identity，不包含 snapshot
revision。未删除实体跨 revision 保持同一 ID；expected ProjectRevision 单独负责乐观并发。

load 分两阶段：先在私有 staged state 中完整解析 archive，并以来源 Context 的共享 runtime
catalog 解析 selected/embedded presets 和配置，再发布 Project。失败不能发布半成品。此路径
不得重新枚举 vendor、重新加载 catalog，也不要求调用方为了取得 schema 先额外调用
`SdkContext::presets()`。

ProjectSnapshot 引用不可变 ProjectState。ProjectEdit 从 expected revision 复制 staged state，
所有 mutator 只修改 staged state。commit 完整校验后用一次原子替换发布新 ProjectState。

save 捕获恰好 expected revision 的完整 state 并输出临时文件；完整成功后再替换目标文件。
具体替换手段按平台实现，但不能让成功文件混合两个 revision。

### 6.1 SceneBuilderAdapter

`SceneBuilderAdapter` 是 `ProjectBuilder` 的唯一实现入口，负责把公开 DTO 转换成与
`Orca3mfAdapter` load 后等价的内部 `ProjectState`。它不能要求调用方 include `libslic3r` 私有头，
也不能把公开 DTO 临时序列化成 3MF 再调用 importer 作为主路径。

转换顺序固定为：

```text
validate public DTO
  → materialize unique mesh geometry
  → create ModelObject / volume-part state
  → create plate and instance state
  → attach project/plate/object/part/layer-range ConfigPatch
  → attach preset selection and project/plate filament map
  → run same ProjectState validation as 3MF load
  → publish Project
```

校验必须在 publish 前完成：UTF-8/NUL 字符串、finite 坐标、triangle index、退化 triangle、空
mesh、transform、plate/object binding、selection revision、filament map 结构和资源 limit 均不得
延迟到切片中途才失败。ProjectBuilder/ProjectEdit 接受合法 complete manual map，也接受合法
project/plate persistent auto partial map；auto tools 是 dense prefix，只校验已有 positive `ToolId`
和已知 printer tool 上限。complete manual map 只在 `SliceEngine::submit()` 的 slice-compatible
execution boundary 强制要求。`model_triangles` 统计 materialized unique geometry，不按 instance
重复计数。

builder 不做自动排版、不移动模型、不执行 mesh repair 的猜测行为。若 core materialize 需要
修正 winding、法线或内部缓存，这属于不可观察实现细节；不得改变公开坐标单位、instance
transform 或配置分层。

## 7. 配置解析：禁止压平 Model scope

`PresetBundle::full_fff_config()` 的等价逻辑只负责产生全局 preset/project 基线；随后只叠加
目标 plate config，转换为公开 `EffectiveConfiguration`。

`SliceInputResolver` 由 `SliceEngine::inspect()` 和 `submit()` 复用；内部解析结果由两部分组成：

```text
FrozenSliceInput
  ├─ global + plate effective config
  └─ immutable project snapshot
       └─ Model 中的 object / volume-part / layer-range configs
```

不得把全部 object、part 和 layer-range patch 依次 apply 到一个全局 config。原因是这些配置
分别作用于不同对象、volume 和 Z 区间，且 part 与 layer-range 可能在几何区域上组合。

`PrintAdapter` 应把 global+plate config 与完整 Model 分别交给现有 apply 逻辑，让 core 按原
结构生成 object/region config。

### 7.1 SlotRemap 迁移

ProjectEdit 分开保存 immutable original state 与 target-namespace staged overlay。selection
mutator 最多成功调用一次，第二次按公共 field 返回 conflict；首次立即验证 mapping 长度、目标
范围和 injective 约束。commit 只对 original state 中未被
overlay 显式 set/replace/erase 的字段应用 SlotRemap，再按目标 selection 的 0 基 namespace 应用
overlay。显式 erase 使用 tombstone 记录，不能因迁移 original 而复活。所有公开显式
ConfigPatch 和 map 无论在 selection mutator 前后传入都属于 target namespace，绝不 double-remap。

迁移 original 时，对所有 public `FilamentReferenceKind` scalar/list item 应用 mapping；public
null 不变，映射为删除的非 null reference 记录为 dangling，等待 staged overlay 显式修复。

遍历范围必须包含 project、每个 plate/object/part/layer-range，以及隐藏的全部 project-embedded
preset overrides 和 structured custom G-code。公开 scopes 的 dangling reference 可由同一 edit 的
ConfigPatch 修复；隐藏 dangling reference 无公共 CRUD 可修复，commit 必须返回
`invalid_configuration`。map 按旧 logical-slot index 搬迁；删除项丢弃。manual map 遇到新增
slot 时保持 missing，commit 要求调用方提交完整 project map 和每个原本存在的 local map
override。auto tools 是 dense prefix，迁移后只有仍能表达为 dense prefix 时才保留；新增 slot
不补齐。若 remap 造成非前缀缺口，例如旧 slot 0 迁移到新 slot 2，且同一 edit 没有显式提交新的
合法 auto vector 或 complete manual map 覆盖该 map，commit 返回 `invalid_configuration`，field
固定为 `/filament_map/tools`。最后统一检查无 dangling reference、manual map 完整性、auto map
已有 ToolId 范围合法，再发布 state。raw G-code 受影响且无法证明安全时返回 `unsupported`。禁止用
null/default、slot 0 或任意 ToolId 自动填洞。

## 8. Filament map 决策

v1 公共 API 不包含完整 physical filament/AMS topology，因此切片入口只接受 manual mode。

内部行为：

- load/save 原样保留三种 mode 和已有 tools；
- project/plate inheritance 只计算持久化 map 的来源，不改变 mode；
- SliceInputResolver 在 inspect 阶段遇到 auto mode 时返回 effective map 并附带 auto-map
  not slice-compatible warning，不补齐 tools，不转 manual；diagnostics 必须包含且只包含一个
  field 为 `/filament_map/mode` 的该 warning，且使用 `code=unsupported`、`severity=warning`；
  其他字段的独立 warnings 可以同时存在；
- SliceInputResolver 在 submit 同步冻结阶段遇到 auto mode 时返回 `unsupported`，field 为
  `/filament_map/mode`；
- temporary selection 不继承该 inspect 宽限：`TemporarySliceSelection::complete_manual_map`
  在 inspect 与 submit 中都必须是完整 manual map，temporary auto 直接失败；
- manual mode 将完整 tools 传给切片配置，并保持 mode 为 manual；
- SliceResult 的 map 与本次 manual 输入一致；
- SliceEngine 永不自动写回 Project。

### 已否决方案：auto mode 强制改为 manual

旧设计曾计划保留公开 `auto_for_flush/auto_for_match`，但在切片副本中强制写成 manual，并把
已有 tools 当成最终结果。该方案已否决，因为它绕过原 GUI 在 process 阶段重新计算 map 的
行为，却仍向调用方报告 auto 语义。

实现中不得出现“收到 auto mode 后静默设置 manual 并继续”的路径。未来支持 auto mapping
时，需要先扩展公共 API，提供算法实际读取的物理槽位、tool、external 标记、材质、颜色、
support 和 feeder group topology，再恢复对应的 core 自动计算链路。

## 9. PrintAdapter

每次 slice 使用独立的内部 Print/Model 副本，输入只来自 submit 同步冻结的
`FrozenSliceInput`。`SliceEngine::inspect()` 调用同一解析 pipeline 生成 effective inspection，
但不发布 FrozenSliceInput、job 或其他切片产物，也不执行 submit 的 manual-only execution gate。
submit 在返回 SliceJob 前完成 revision 捕获、global+plate 合成和 manual map 校验；异步阶段不得再次读取 mutable Project/catalog。在 apply 前必须
完成目标 plate 上下文准备：

- 从 snapshot 解析目标 PlateId 对应的稳定 plate index，并设置副本的 current plate index；
- 绑定该 plate 的 custom G-code、plate config、bed/printable shape、printable height、extruder
  areas/heights 和 wipe-tower plate 参数；
- 在模型副本上执行与 GUI `update_print_volume_state()` 等价的 build-volume 判定；
- 只启用属于目标 plate 且 `printable && !excluded` 的 instances，其他 plate instances 不进入
  本次 Print；
- 为目标 plate 建立独立 Print 状态，不能复用上一 plate 的 Print/cache。

随后执行：

```text
prepare immutable input copy
  → prepare target plate context and instance printability
  → apply(global + plate config, layered Model)
  → validate
  → process
  → export requested G-code and fill GCodeProcessorResult
  → convert requested preview from GCodeProcessorResult
  → collect statistics and diagnostics
  → publish immutable SliceResult
```

任一阶段失败或取消都丢弃 staged output。只有全部阶段成功才发布 SliceResult。

请求的 G-code、statistics、final map 和 preview 必须来自同一个 `FrozenSliceInput`、同一个内部
Print/Model 副本和同一次 process/export。preview-only 仍使用同一次 export 阶段填充的
`GCodeProcessorResult`，但不得向 SliceResult 发布 `gcode_bytes`。禁止在 `PreviewAdapter` 中重新
解析已生成的 G-code 作为 fallback，也禁止把旧 worker 的 `.orcapv` wire type、shared-memory
transport 或 protocol headers 重新暴露给公共 SDK。

取消依赖 core 已有取消检查和 SDK 阶段边界。`cancel()` 只请求取消，不能被文档或实现解释为
已完成；core/SDK 取消检查负责推进取消，`SliceJob::wait_for(timeout)` 只通过 `SliceJobState` 的
mutex/condition_variable 有界观察同一个终态，不是取消实现机制。`wait()` 与 `wait_for()` 必须
复用同一终态复制路径，保证成功 result、失败 diagnostics 和 callback warning 不漂移；负 timeout
在等待前返回 `invalid_argument /timeout`，0ms 只轮询。callback 内调用同一 job 的 `wait()` 或
`wait_for()` 必须立即返回 `conflict /callback`，不得等待 dispatcher 或 core lock。终态只能发布
一次。callback 由独立 dispatcher 读取事件队列，避免在 core 内部栈上执行 App 代码。

实现状态：`wait_for()` 已进入实际 public header 与实现，并由 compatibility tests 覆盖；它只
有界观察共享终态，不是取消机制的一部分。

### 9.1 PreviewAdapter

`PreviewAdapter` 的输入是本次 export 填充的 `GCodeProcessorResult`、冻结的 project entity
mapping、final manual filament map 和 printer/filament metadata。输出是公共 `SlicePreview` DTO
或 SDK-owned artifact。

字段语义以旧 preview artifact v2 为基础：metadata、layers、tools、filaments、colors、objects/
instances 可证明映射、moves 和 events。公共 enum 必须由 SDK 自己定义；内部 extrusion role、
move type、color source 和 path kind 通过显式转换表映射，未知值映射到公共 `unknown` 并保留
diagnostic warning，不能把 core enum 值透传给调用方。

object/instance 归属必须通过冻结 project state 与 processor move 的稳定关联证明。无法证明时
公共 optional 置空；不得根据空间位置、名称或索引猜测。颜色、tool、filament、layer、width、
height、speed、temperature、fan、time 和 joint angle 等字段按 processor 原始单位转换成 API
规定的 mm、秒、摄氏度和百分比。

artifact 输出只是公共 DTO 的一种持久化传输形式。它可以复用旧 `.orcapv v2` 的 schema 字段语义
和二进制布局经验，但不承诺旧 worker ABI，不安装旧 worker headers，也不恢复 worker target。
写 artifact 必须先写 SDK-owned 临时文件，完成 preview bytes/moves/temporary disk 预算检查后
原子发布到 `preview_artifact_path`。

## 10. BoundedGcodeSink 与资源预算

计数严格使用 API 第 6 节口径：输入实际 byte length；每个 archive entry 的 decompressor output
累计且重复 entry 重复计数；triangle 按 materialized unique mesh geometry facets；temporary
disk 按全部 SDK-owned live files 的 aggregate high-water；G-code 按 sink committed bytes；
preview bytes 按内存 DTO/artifact payload committed bytes；preview moves 按 move record 数。
所有计数在下一次增长前检查，等于 limit 允许。

G-code writer 应直接写入受限 sink。sink 每次 reserve/write 前检查剩余预算，超限立即取消
export 并返回 `resource_limit_exceeded`。禁止先完整写到无界 path/vector，再通过 stat、读取
或截断伪装成限制。

如果现有 exporter 只能写文件，应为其增加内部受限输出抽象或严格受控的 file sink；不能把
内部文件路径暴露成公共结果。`include_gcode=true` 成功后，`gcode_bytes` 以精确 byte count
移交 SliceResult；`include_gcode=false` 时该 sink 只允许作为内部 processor 填充通道存在，最终
不得发布 G-code bytes。

preview memory builder 和 artifact writer 使用同一预算模型。`preview_moves` 在追加 move 前检查；
`preview_bytes` 在追加序列化 payload 或 DTO owned storage 前检查。artifact 临时文件还必须计入
`temporary_disk_bytes`。超限、取消或转换失败时删除临时 artifact，并且不得发布 `SlicePreview`、
artifact path 或半截 SliceResult。

## 11. 线程实现要求

- immutable handles 的内部 state 使用共享只读所有权；
- runtime 首次初始化使用 mutex/condition_variable 或等价 single-flight；并发同配置等待者只
  观察同一发布结果，失败不保留 candidate catalog、key 或成功 generation，下一次显式 create
  才能创建新 attempt；
- `SharedRuntime` 成功后只读共享；repository user transaction 以 immutable generation 原子发布；
- Project load 只读取来源 Context 的已发布 runtime/catalog lease，不与初始化并发，也不共享
  staged `PresetBundle`；
- Project 发布新 revision 时不修改旧 snapshot；
- PresetEditor/ProjectEdit 记录创建线程并在 debug/test 中检查误用；
- SliceJob state 使用 mutex/condition_variable 或等价机制保护终态、结果和取消；
- callback 不持有阻止 cancel/wait/wait_for 进展的 core lock；
- preset loader 和切片 TBB 工作访问的对象在对应 coordinator task/candidate 结束前全部
  quiescent；
- 不依赖进程退出清理临时文件或 worker。

## 12. 测试注入点

仅在内部测试构建选项 `LIBSLICER_SDK_TESTING` 下提供不安装、不导出的 hooks：

- `StageBarrier`：在 queued/preparing/validating/slicing/exporting 的确定边界阻塞并确认已到达，
  用于无竞态取消测试；
- `ResourceProbe`：记录 archive 声明/实际扩张 bytes、unique-mesh triangle 计数、全部临时文件的
  aggregate live/high-water、G-code sink reserve/write/committed high-water、preview bytes/moves
  reserve/write/committed high-water；
- `CoreCallProbe`：记录 plate index、参与切片的 instance IDs、apply/process/export 次数，以及
  auto-map 计算入口是否被调用；
- `PreviewProbe`：记录 preview 是否由本次 `GCodeProcessorResult` 转换、转换的 layer/tool/
  filament/color/move/event 数、object/instance 归属缺失原因，以及 artifact 临时文件发布/清理；
- `LifetimeProbe`：验证公开输入复制完成后不再借用调用方 buffer。
- `FailureInjectionHook`：只在 test target 中分别于 validate 成功后的 process、export，以及
  facade 未分类异常边界注入确定性失败，用于验证 `slicing_failed` 和 `internal` 映射。
- `PresetIoProbe`：记录 preset source 的 readable/writable open、create、remove、rename、replace、
  lock 和 durable flush 的 canonical path 与 mode，证明只有 data user store 发生 mutation；
- `PresetTransactionBarrier`：在跨进程锁已取得、锁内 fingerprint 重读前后和 manifest commit 前
  提供确定性 barrier，用于验证两个 Context/进程的 compare-and-swap 顺序。
- `RuntimeInitializationProbe`：记录 preflight、canonical key bind、schema、core system loader、
  explicit `profiles_dir`、policy、完整 `PresetBundle::load_presets(AppConfig&)` 禁用计数、
  `OrcaFilamentLibrary`、parallel vendor load、stable merge、structured issues、SDK validation、
  publish/discard、reuse 和 Project load 分段；同时记录 system loader/bundle/vendor 次数、
  线程、mutex acquire/release 边界和 monotonic elapsed time；
- `RuntimeInitializationBarrier`：在 key bind、loader start、candidate publish 和 failure discard
  前提供确定性 barrier，用于相同配置 single-flight、不同配置 conflict、失败原子性和 retry；
- `RuntimeInitializationFailureHook`：在 schema、Orca library、并行 vendor、中间 merge、SDK strict
  validation 和 publish 前注入失败；每个 case 必须证明 candidate 销毁、TBB quiescent、无公开
  catalog，并可从全新 candidate 对同配置重试；
- `PresetFailureInjectionHook`：精确阶段枚举为 `payload_staging`、`payload_flush_close`、
  `manifest_staging`、`manifest_flush_close`、`before_manifest_replace`、
  `after_commit_directory_flush` 和 `after_commit_content_reclaim`。前五项在唯一提交点前注入确定性
  `io` 并记录命中阶段，cleanup probe 验证 staging 已清理；后两项只产生公共规定的合并 warning，
  不能伪装成失败。

除 `FailureInjectionHook` 和 `PresetFailureInjectionHook` 外，hooks 只观察或暂停既有阶段。
两个 injection hook 仅编译进 test target，生产 target 不含注入分支；未启用注入时不得改变
计算结果。compatibility tests 必须用
这些计数器证明超限发生在扩张前、G-code/preview high-water mark 不超过 limit，并确定性触发各
阶段取消和错误映射。

## 13. 安装边界

公开 target 只编译/安装 v1 facade headers。内部 headers 不安装。`libslicer::sdk_v1` 私有链接
libslic3r，并通过 CMake 的 link-only 依赖向静态库调用方传递必要库，不传递内部 include
directory。

旧配置解析接口和 worker 接口不属于 v1，拆除后不得重新进入默认 SDK target，也不能影响 v1
公共符号。

## 14. 实施顺序

1. 公共基础类型、Result、ID、ConfigValue、ConfigSchema；
2. ContextState/SharedRuntime、RuntimeCoordinator canonical key 与 context-local temporary/资源限制；
3. 原 GUI core preset loader 适配、single-flight 发布、进程级长期持有与 repository 事务；
4. Orca 3MF load/snapshot/edit/save 对共享 runtime catalog 的复用；
5. ProjectBuilder 与 SceneBuilderAdapter；
6. SliceInputResolver 与 SliceEngine inspect/submit 同步冻结，严格保持 global/plate 与 Model scopes 分离；
7. filament map validation；
8. PrintAdapter、PreviewAdapter、取消、G-code/preview sink 和统计；
9. 安装导出和兼容性测试。

每一阶段必须先通过
[`libslicer_sdk_compatibility_tests.md`](./libslicer_sdk_compatibility_tests.md) 对应 gate，
再进入下一阶段。

既有 RuntimeCoordinator、explicit-directory loader、Orca-first/parallel/stable merge、strict
catalog 和第一阶段 schema index 架构保持冻结；第二阶段 record conversion 尚未实施。本阶段
write set 严格限定为：

- `src/libslicer_sdk/Preset.cpp`
- `src/libslicer_sdk/PresetInternal.hpp`，仅承载
  `LIBSLICER_SDK_TESTING` 条件编译的 scheduler seam；不安装、不导出
- `src/libslicer_sdk/OrcaConfigAdapter.cpp`
- `src/libslicer_sdk/OrcaConfigAdapter.hpp`，仅用于新增 private/internal fused conversion，
  不得改变 public API
- `tests/libslicer_sdk/test_config.cpp`
- `tests/libslicer_sdk/libslicer_sdk_cold_gate.cpp`，dedicated cold/reuse/RSS child executable
- `tests/libslicer_sdk/CMakeLists.txt`，仅用于 test-only
  `LIBSLICER_SDK_TESTING` private compile definition、test source/target、三个 CTest、
  `RUN_SERIAL TRUE` 和 `TIMEOUT 60` 登记

禁止修改 `include/libslicer/v1/**`、`src/libslicer_sdk/ConfigSchema*`、
`src/libslicer_sdk/Context*`、
`src/libslicer_sdk/RuntimeCoordinator*`、`src/libslic3r/**`、loader、public `Preset` contract、
app 代码、packaging/install/export 或 production CMake target。禁止增加 fallback、lazy/partial
catalog、首次 `presets()` 加载、按名称猜 parent/default、无界 worker、环境变量测试开关或另一
份 conversion authority。`PresetInternal.hpp` 中除上述 test seam 条件声明外不得改变 catalog
模型。若实施发现必须超出上述 write set，必须先修订设计并重新审核，不能直接改代码。
