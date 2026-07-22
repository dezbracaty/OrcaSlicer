# libslicer C++ SDK v1 公共 API 规范

状态：v1 公共面 freeze candidate，允许按本文实施和评审；在
`tests/data/libslicer_sdk/baseline_manifest.json` 及冻结 fixtures 提交并通过前，不得宣称稳定
v1 发布。默认构建不得安装稳定 SDK package，只有显式开启 `LIBSLICER_INSTALL_SDK_V1` 才允许
生成安装包用于预发布验证。

## 1. 目的和范围

libslicer v1 是供其他 App 直接链接的 C++17 FFF 切片 SDK。它提供以下公共能力：

1. 浏览、创建、编辑和删除 printer、process、filament 预设；
2. 加载、查看、修改并保存 Orca/Bambu 兼容的 FFF project 3MF；
3. 接收 App 已解析好的 mesh/scene，构建可切片 Project；
4. 读取和修改 project、plate、object、part、layer-range 的分层配置；
5. 按 Orca 配置语义执行 FFF 校验、切片和 G-code 导出；
6. 从同一次权威切片中返回诊断、统计、最终耗材映射和可选 toolpath preview。

v1 不提供 GUI、OpenGL 渲染、STL/OBJ/STEP importer、generic 3MF importer、模型编辑、自动排版、
云/设备连接、上传、SLA、后处理脚本或 C ABI。STL、OBJ、STEP 文件解析不属于 v1 输入；其他
App 如需支持这些格式，必须在 App 侧完成解析，并把已解析的 triangle mesh/scene DTO 交给
`ProjectBuilder`。

`Project::load()` 只接受 Orca/Bambu project 3MF。文件必须包含可恢复的 project config、
至少一个 plate、模型/instance 上下文，且 active printer technology 为 FFF。generic 3MF、
G-code-only 3MF、SLA project 3MF 和扩展名伪装文件在 load 阶段返回 `unsupported`，不得发布
部分工程。

SDK 的两个正式 project 输入路径是：

- `Project::load()`：加载 Orca/Bambu 兼容 project 3MF，并保留其完整切片上下文；
- `ProjectBuilder`：接收 App 已解析的 scene/mesh/config/preset selection，构建新的 Project。

两条路径构建出的 Project 后续都必须走同一套 `ProjectSnapshot -> SliceRequest -> SliceEngine`
闭环。SDK 不提供绕过 Project 的第二套切片入口。

构建树内公开 CMake target 为 `libslicer::sdk_v1`，公开头文件位于 `<libslicer/v1/...>`。安装态
`find_package(libslicer CONFIG)` 属于 release packaging 行为，必须受兼容性 baseline gate 控制。

## 2. 规范边界和兼容承诺

本文只规范：

- 公开类型、方法、参数和返回值；
- 错误、revision、事务、线程和内存所有权；
- 调用方可以观察的配置、工程、映射和切片行为。

本文不规定 SDK 内部类名、core 调用顺序、调度器、adapter、临时文件或输出缓冲实现。

v1 是 C++17 源码 API，不承诺跨编译器或跨标准库的二进制 ABI。所有公开符号位于
`libslicer::v1`。同一 major 版本内：

- 已有方法的参数、返回值、错误含义和可观察行为不得改变；
- 已有枚举值不得重定义；
- minor 版本只能增加不会破坏现有源码的新能力；
- schema 增量通过 `schema_version()` 明确标识。

## 3. 错误模型

```cpp
namespace libslicer::v1 {

enum class ErrorCode {
  invalid_argument,
  not_found,
  conflict,
  busy,
  unsupported,
  io,
  resource_limit_exceeded,
  invalid_configuration,
  cancelled,
  slicing_failed,
  internal
};

enum class Severity { warning, error };

struct Diagnostic {
  ErrorCode code;
  Severity severity;
  std::string message;
  std::string field;
};

template<class T> class Result {
public:
  bool has_value() const noexcept;
  std::optional<ErrorCode> error_code() const noexcept;
  T& value() &;
  const T& value() const &;
  T&& value() &&;
  const std::vector<Diagnostic>& diagnostics() const noexcept;
};

template<> class Result<void> {
public:
  bool has_value() const noexcept;
  std::optional<ErrorCode> error_code() const noexcept;
  void value() const;
  const std::vector<Diagnostic>& diagnostics() const noexcept;
};

}
```

失败 Result 至少包含一个 `Severity::error` diagnostic，`error_code()` 等于主错误的 code。
成功 Result 的 `error_code()` 为空，但可以携带 warning。`field` 使用稳定 JSON Pointer 风格
定位公开请求字段。

失败时调用 `value()` 属于调用方编程错误并抛出 `std::logic_error`。除此以外，正常运行失败
通过 Result 返回；标准内存分配失败不转换。

## 4. ID、revision 和基本值类型

```cpp
namespace libslicer::v1 {

using PresetRevision = std::uint64_t;
using ProjectRevision = std::uint64_t;

class OptionId {
public:
  explicit OptionId(std::string);
  std::string value() const;
  friend bool operator==(const OptionId&, const OptionId&) noexcept;
};

class PlateId {
public:
  std::uint64_t value() const noexcept;
  friend bool operator==(const PlateId&, const PlateId&) noexcept;
private:
  struct Binding;
  explicit PlateId(std::shared_ptr<const Binding>);
  std::shared_ptr<const Binding> binding_;
  friend class Project;
  friend class ProjectSnapshot;
  friend class ProjectEdit;
  friend class ProjectBuilder;
};

class ObjectId {
public:
  std::uint64_t value() const noexcept;
  friend bool operator==(const ObjectId&, const ObjectId&) noexcept;
private:
  struct Binding;
  explicit ObjectId(std::shared_ptr<const Binding>);
  std::shared_ptr<const Binding> binding_;
  friend class Project;
  friend class ProjectSnapshot;
  friend class ProjectEdit;
  friend class ProjectBuilder;
};

class PartId {
public:
  std::uint64_t value() const noexcept;
  friend bool operator==(const PartId&, const PartId&) noexcept;
private:
  struct Binding;
  explicit PartId(std::shared_ptr<const Binding>);
  std::shared_ptr<const Binding> binding_;
  friend class Project;
  friend class ProjectSnapshot;
  friend class ProjectEdit;
  friend class ProjectBuilder;
};

class InstanceId {
public:
  std::uint64_t value() const noexcept;
  friend bool operator==(const InstanceId&, const InstanceId&) noexcept;
private:
  struct Binding;
  explicit InstanceId(std::shared_ptr<const Binding>);
  std::shared_ptr<const Binding> binding_;
  friend class Project;
  friend class ProjectSnapshot;
  friend class ProjectEdit;
  friend class ProjectBuilder;
};

struct FilamentSlotId {
  std::uint32_t value;
  friend bool operator==(FilamentSlotId, FilamentSlotId) noexcept;
};

struct ToolId {
  std::uint32_t value;
  friend bool operator==(ToolId, ToolId) noexcept;
};

}
```

`FilamentSlotId` 始终为 0 基逻辑耗材槽位，在有明确 selection 的调用中必须小于当前 logical
filament count。`ToolId` 始终为 1 基物理 tool ID，因此 `ToolId{0}` 非法；使用它的调用还必须
验证其不大于当前 printer 的 physical tool count。所有工程 entity ID 都与产生它的工程及实体绑定。未删除实体的 ID 在同一 Project 的后续 revision 中保持
稳定；跨工程 ID 或已删除实体返回 `not_found`。并发修改冲突由 ProjectRevision 检测，不能
通过让全部 entity ID 在每次 commit 后失效来代替。

## 5. 强类型配置模型

```cpp
namespace libslicer::v1 {

enum class ConfigValueType {
  boolean,
  integer,
  decimal,
  percent,
  float_or_percent,
  string,
  enumeration,
  point2,
  point3,
  list
};

class ConfigValueShape {
public:
  static ConfigValueShape scalar(ConfigValueType);
  static ConfigValueShape list(ConfigValueShape item_shape);
  ConfigValueType type() const noexcept;
  std::optional<ConfigValueShape> item_shape() const;
  friend bool operator==(const ConfigValueShape&, const ConfigValueShape&) noexcept;
private:
  struct State;
  std::shared_ptr<const State> state_;
};

struct FloatOrPercent { double value; bool is_percent; };
struct Point2 { double x, y; };
struct Point3 { double x, y, z; };

class ConfigValue {
public:
  ConfigValueType type() const noexcept;
  ConfigValueShape shape() const;

  static ConfigValue boolean(bool);
  static ConfigValue integer(std::int64_t);
  static ConfigValue decimal(double);
  static ConfigValue percent(double);
  static ConfigValue float_or_percent(FloatOrPercent);
  static ConfigValue string(std::string);
  static ConfigValue enumeration(std::string);
  static ConfigValue point2(Point2);
  static ConfigValue point3(Point3);
  static Result<ConfigValue> list(ConfigValueShape item_shape,
                                  std::vector<ConfigValue>);
  static ConfigValue null(ConfigValueShape underlying_shape);

  bool is_null() const noexcept;
  std::optional<bool> as_boolean() const;
  std::optional<std::int64_t> as_integer() const;
  std::optional<double> as_decimal() const;
  std::optional<double> as_percent() const;
  std::optional<FloatOrPercent> as_float_or_percent() const;
  std::optional<std::string> as_string() const;
  std::optional<std::string> as_enumeration() const;
  std::optional<Point2> as_point2() const;
  std::optional<Point3> as_point3() const;
  std::optional<std::vector<ConfigValue>> as_list() const;

  friend bool operator==(const ConfigValue&, const ConfigValue&) noexcept;
private:
  struct State;
  std::shared_ptr<const State> state_;
};

struct ConfigEntry { OptionId option; ConfigValue value; };

class ConfigPatch {
public:
  void set(OptionId, ConfigValue);
  bool erase(const OptionId&);
  std::optional<ConfigValue> find(const OptionId&) const;
  const std::vector<ConfigEntry>& entries() const noexcept;
};

class ConfigValues {
public:
  std::optional<ConfigValue> find(const OptionId&) const;
  const std::vector<ConfigEntry>& entries() const noexcept;
};

}
```

`ConfigPatch` 是有序、无重复 OptionId 的差异集合，不是 JSON Patch。`ConfigValues` 是完整、
只读的值集。

`percent` 与 `float_or_percent` 是不同类型。list 的 shape 可递归，因此 group 类型能表达为
嵌套 list。null 保留 underlying shape。list 容器 nullable 与 list item nullable 是不同约束，
nullable vector 的 nil 必须保留在原索引，不能折叠成整个 list 为 null。

`as_string()` 只读取 `string`，`as_enumeration()` 只读取 `enumeration`；类型不匹配或值为 null
时返回 `nullopt`，不得把 enum 隐式降级成普通 string。

```cpp
namespace libslicer::v1 {

enum class PresetKind { printer, process, filament };
enum class OptionScope { project, plate, object, part, layer_range, preset };
enum class FilamentReferenceKind {
  none,
  logical_slot,
  logical_slot_list
};

struct NumericConstraint {
  std::optional<double> minimum;
  std::optional<double> maximum;
  std::string canonical_unit;
};

struct FloatOrPercentConstraint {
  NumericConstraint numeric;
  std::optional<OptionId> percent_base;
};

class ValueDescriptor;

class ListDescriptor {
public:
  ValueDescriptor item() const;
  std::size_t min_items() const noexcept;
  std::size_t max_items() const noexcept;
  bool fixed_length() const noexcept;
private:
  struct State;
  std::shared_ptr<const State> state_;
};

class ValueDescriptor {
public:
  ConfigValueShape shape() const;
  bool value_nullable() const noexcept;
  std::optional<NumericConstraint> numeric() const;
  std::optional<FloatOrPercentConstraint> float_or_percent() const;
  std::vector<std::string> enum_values() const;
  std::string canonical_unit() const;
  std::optional<ListDescriptor> list() const;
private:
  struct State;
  std::shared_ptr<const State> state_;
};

struct OptionDescriptor {
  OptionId id;
  ValueDescriptor value;
  std::string label;
  std::vector<OptionScope> allowed_scopes;
  std::vector<PresetKind> applicable_preset_kinds;
  FilamentReferenceKind filament_reference;
  bool editable;
};

struct ConfigValidationContext {
  OptionScope scope;
  std::optional<PresetKind> preset_kind;
  struct PresetValues {
    PresetRevision revision;
    ConfigValues effective_values;
  };
  std::optional<PresetValues> printer;
  std::optional<PresetValues> process;
  std::optional<std::size_t> filament_count;
};

struct OptionEvaluation {
  bool visible;
  bool compatible;
  std::vector<Diagnostic> diagnostics;
};

class ConfigSchema {
public:
  std::string schema_id() const;
  std::uint32_t schema_version() const noexcept;
  std::optional<OptionDescriptor> find(const OptionId&) const;
  std::vector<OptionDescriptor> options() const;
  Result<ConfigPatch> validate_and_normalize(
      const ConfigValues& base,
      ConfigPatch candidate_patch,
      const ConfigValidationContext&) const;
  Result<OptionEvaluation> evaluate_option(
      const OptionId&,
      const ConfigValues& base,
      const ConfigPatch& candidate_patch,
      const ConfigValidationContext&) const;
};

}
```

schema descriptor 必须描述全部 FFF option 的类型、递归 list shape、nullability、单位、
范围、枚举值和适用 scope。generic config、preset selection、filament map 与结构 metadata 的
内部所有权分类不是公共 schema 字段；SDK 仍必须在入口边界拒绝用 ConfigPatch 修改非 generic
字段。`ConfigValidationContext::preset_kind` 在 scope 为
`preset` 时必填，在其他 scope 必须为空。编辑 process preset 时必须提供固定 revision 的
printer values；编辑 filament preset 时必须提供 printer 与 process values。依赖当前耗材数量
的规则使用 `filament_count`。project/plate/object/part/layer-range 的 context 使用
`SliceEngine::inspect()`/`submit()` 同步捕获的 printer/process values 和工程逻辑耗材数量。

`validate_and_normalize()` 原子完成 shape/scope/nullability/range/reference 检查、把 candidate
patch 合并到完整 base 后执行跨 option/context 校验，并返回 canonical、无重复且保留输入顺序的
patch；失败不返回部分 patch。调用方不需要在四个相互重叠的方法之间选择。

Orca 的可见和兼容规则不降级成只能表达相等判断的公开条件数组。`evaluate_option()` 原子地把
candidate patch 合并到完整 base values，再在明确 scope/kind 上下文中求值，因此配置编辑器
可以对未提交修改即时刷新。结构字段、预设选择和 filament map 不能通过 generic
`ConfigPatch` 绕过强类型 API。

`FilamentReferenceKind` 只描述公共配置值是否引用 logical filament slot，不描述 core 的存储
编码。公共语义固定如下：

- `logical_slot` 的非 null 值必须是大于等于 0 的 `integer`，其数值就是 0 基 logical slot；
- 对存在“默认/自动/使用当前耗材”状态的 scalar option，`ValueDescriptor::value_nullable()`
  必须为 true，且 null 是该状态唯一的公开表示；
- `logical_slot_list` 的每个非 null item 必须是大于等于 0 的 `integer`；允许默认状态时由 item
  null 表示，并由 list item descriptor 的 nullability 声明；
- `none` 表示该 option 的整数不具有 logical slot 语义；
- `FilamentMapOverride::tools` 是由强类型 API 管理的 1 基 `ToolId` 列表，不是
  `logical_slot_list`；对应 core option 不进入公开 schema 的 editable generic option 集合，
  generic patch 必须拒绝它。

SDK 边界只接受和返回上述 0 基 logical slot/null 语义。任何源格式或实现内部的其他表示必须在
边界内完成规范化，不能出现在公开枚举、schema constraint、ConfigValue 或 diagnostic 中。
slot 插入、删除和重排只根据 `FilamentReferenceKind` 迁移公开的 0 基值/null。

`ConfigValidationContext::filament_count` 为空时只验证类型、非负值和 nullability，不验证 slot
上界；这用于不绑定当前工程/selection 的 preset 配置。其有值时，所有非 null reference 必须
小于该值。违反 reference 类型、nullability、非负或已知上界均返回
`invalid_configuration`，field 为 `/configuration/<escaped OptionId>`，其中 OptionId 按
RFC 6901 把 `~` 转义为 `~0`、`/` 转义为 `~1`。

JSON 只作为显式互操作格式，并只由独立头 `<libslicer/v1/json.hpp>` 声明；核心配置、工程和
切片头不得为 JSON helper 增加依赖：

```cpp
#include <libslicer/v1/json.hpp>

Result<std::string> export_json(const ConfigPatch&, const ConfigSchema&);
Result<ConfigPatch> import_json(std::string_view, const ConfigSchema&);
Result<std::string> export_json(const EffectiveConfiguration&);
```

JSON 文档必须携带 schema id/version。JSON 字符串不作为主要业务参数。

## 6. Context 和资源限制

```cpp
class PresetRepository;
class ProjectBuilder;
class SliceEngine;

struct ResourceLimits {
  std::uint64_t project_input_bytes;
  std::uint64_t project_uncompressed_bytes;
  std::uint64_t model_triangles;
  std::uint64_t gcode_bytes;
  std::uint64_t preview_bytes;
  std::uint64_t preview_moves;
  std::uint64_t temporary_disk_bytes;
};

struct ContextOptions {
  std::filesystem::path resources_dir;
  std::filesystem::path data_dir;
  std::filesystem::path temporary_dir;
  std::vector<std::filesystem::path> preset_dirs;
  ResourceLimits limits;
};

class SdkContext {
public:
  static Result<SdkContext> create(ContextOptions);
  Result<PresetRepository> presets();
  Result<ProjectBuilder> create_project_builder();
  Result<SliceEngine> create_slice_engine();
};
```

目录角色和 preset catalog 快照是公共行为：

- `resources_dir` 是只读的 SDK 资源根目录。SDK 从中读取内置 system/vendor profiles 和其他
  Orca 资源，绝不向其中写入；
- `data_dir` 是该 Context 唯一的可写持久化根目录。repository 中所有 `PresetOrigin::user`
  preset 都只从 SDK 在 `data_dir` 下拥有的 user preset store 加载，并且 create/commit/erase
  只能写入该 store。store 的内部子目录布局不是公共 API；
- `preset_dirs` 是按调用方给定顺序扫描的附加只读 preset catalog 根目录。SDK 绝不修改其中的
  文件；其中的 repository preset 作为 `PresetOrigin::vendor` 暴露，不能 edit/erase；
- 全局扫描顺序固定为 `resources_dir`、`data_dir` user store、`preset_dirs[0]` 到
  `preset_dirs[N-1]`；每个根目录内按 canonical path 的 UTF-8 byte sequence 升序扫描；
- preset 的唯一身份是 `(PresetKind, PresetOrigin, PresetRef::id())`。不同 origin 的相同 kind/id
  可以共存；任意已配置来源中出现相同身份时不做覆盖，也没有“后者/前者优先”，
  `SdkContext::create()` 返回 `conflict`。diagnostic field 对应后扫描来源：内置资源为
  `/resources_dir`，user store 为 `/data_dir`，附加目录为 `/preset_dirs/{index}`；
- 从 Orca profile 加载的 `PresetRef::id()` 固定为 profile JSON `name` 经 Orca-compatible string
  decoding 后的完整 bytes；不得改用 cloud `setting_id`、`filament_id`、display alias 或文件名。
  缺少或非法 canonical name 的 profile 返回 `invalid_configuration` 并指向其来源目录字段；
- 兼容 3MF 只保存了无 origin 的历史 preset 字符串时，该字符串只与 `PresetRef::id()` 作
  UTF-8 byte-for-byte、大小写敏感的精确比较，不与展示用 `PresetSummary::name` 匹配，不 trim、
  不做 Unicode normalization。origin 解析顺序固定为 project-embedded、user、vendor、system；
  在每个 origin 内先匹配当前 canonical id，未命中再按 profile 声明的 `renamed_from` 历史 id
  作 byte-exact 链式解析，然后才进入下一 origin。canonical id 优先于同 origin alias；不使用
  fuzzy、display-name 或 Generic fallback。同一 origin 内重复 identity或一个历史 id 指向多个
  当前 id 返回 `conflict`；alias 链循环返回 `invalid_configuration`；均在 catalog/project load
  阶段拒绝并指向来源字段。全部 origin 均未匹配时返回 `not_found`。legacy
  selection 错误的 field 都是 `/project/selected_presets/printer`、
  `/project/selected_presets/process` 或 `/project/selected_presets/filaments/{index}`；
- `SdkContext::create()` 在成功返回前完整建立 catalog 快照，并验证 `resources_dir` 与每个
  `preset_dirs` 可读、`data_dir` 可创建且 user preset store 可写。格式错误返回
  `invalid_configuration`，I/O/权限错误返回 `io`，重复身份返回 `conflict`；失败时不发布
  SdkContext。目录读取、解析、权限和 user-store 创建失败的 diagnostic field 精确指向产生
  错误的 `/resources_dir`、`/data_dir` 或 `/preset_dirs/{index}`；
- Context 创建后不监听、轮询或自动发现目录中的外部文件变化。repository 自身成功提交的变化
  立即进入该 Context 的新 catalog generation；其他进程或直接文件修改只有新建 SdkContext
  才会载入。现有 Context 的 edit/create/erase 在持久化前必须重新验证目标的存在状态和完整
  内容版本：修改、删除、同路径替换，以及 create 捕获“不存在”后被外部创建，均返回
  `conflict`，field 为 `/preset/storage_revision`，不得静默覆盖；
- 指向同一 canonical `data_dir` user store 的多个 Context/进程对同一 identity 并发执行 SDK
  transaction 时采用 compare-and-swap 语义：若都捕获同一旧状态，恰好一个成功，其余返回
  `conflict`。这不承诺协调绕过 SDK 直接写文件且不遵守 SDK 锁协议的并发 writer。

user preset create/edit commit 必须先通过单一原子提交点持久化新内容，再发布新的内存
revision/catalog generation；erase 必须通过同一原子提交点移除 catalog 中的 identity，再发布
内存删除。两种操作的持久化失败都返回 `io`、field `/data_dir`，内存视图/revision 和旧的
catalog 可见内容保持提交前状态。成功 commit 意味着新 Context 能读取新值；成功 erase 意味着
新 Context 返回 `not_found`。用于 staging 或回收的内部文件不构成 catalog 内容。

原子提交点之后的 directory flush 或不可达旧 content 回收失败不允许把已提交事务改报失败；
Result 成功，并且无论其中一个还是两者都失败，合并追加恰好一个
`{ErrorCode::io, Severity::warning, ..., "/data_dir"}` diagnostic，新 generation 保持已提交。
v1 的“持久化”承诺是进程可观察且新 Context 可读取，不承诺突然断电后的物理 durability。

所有 limit 必须非零。任何输入、解压、模型、临时空间或输出超限均返回
`resource_limit_exceeded`，不得返回部分 Project、部分 G-code 或部分 SliceResult。

资源计数是公共可观察语义：

- `project_input_bytes`：输入文件实际 byte length；
- `project_uncompressed_bytes`：decompressor 为每个 archive entry 实际产出的 bytes 之和，
  重名或重复 entry 每次计入；
- `model_triangles`：成功 materialize 的唯一 mesh geometry facet 数，instance 不重复计数；
- `temporary_disk_bytes`：同一时刻全部 SDK-owned temporary files 的 aggregate live-byte
  high-water，覆盖写不累计历史写入量；
- `gcode_bytes`：G-code 输出 sink 已 commit 的原始 byte 总数；
- `preview_bytes`：内存 preview DTO 或 preview artifact 已 commit 的 payload byte 总数，不包含
  调用方传入路径字符串；
- `preview_moves`：preview move record 的总数。

每项在下一次增长会使对应计数大于 limit 之前拒绝；等于 limit 允许。

## 7. 预设仓库

```cpp
enum class PresetOrigin { system, vendor, user, project_embedded };

class PresetRef {
public:
  static PresetRef system(PresetKind, std::string id);
  static PresetRef vendor(PresetKind, std::string id);
  static PresetRef user(PresetKind, std::string id);
  PresetKind kind() const noexcept;
  PresetOrigin origin() const noexcept;
  std::string id() const;
  friend bool operator==(const PresetRef&, const PresetRef&) noexcept;
private:
  struct Binding;
  std::shared_ptr<const Binding> binding_;
  explicit PresetRef(std::shared_ptr<const Binding>);
  friend class Project;
  friend class ProjectSnapshot;
  friend class ProjectEdit;
};

struct SelectedPreset { PresetRef ref; PresetRevision revision; };

struct PresetSummary {
  PresetRef ref;
  std::string name;
  std::string vendor;
  std::optional<SelectedPreset> parent;
  PresetRevision revision;
};

struct PresetSelection {
  SelectedPreset printer;
  SelectedPreset process;
  std::vector<SelectedPreset> filaments;
};

struct PresetCompatibilityContext {
  std::optional<SelectedPreset> printer;
  std::optional<SelectedPreset> process;
};

struct PresetCompatibility {
  bool compatible;
  std::vector<Diagnostic> diagnostics;
};

class PresetView {
public:
  PresetSummary metadata() const;
  ConfigValues inherited_values() const;
  ConfigValues effective_values() const;
  ConfigPatch overrides() const;
};

class PresetEditor {
public:
  PresetView snapshot() const;
  Result<void> set_override(OptionId, ConfigValue);
  Result<void> erase_override(const OptionId&);
  Result<PresetSummary> commit();
  void discard() noexcept;
};

class PresetRepository {
public:
  ConfigSchema schema() const;
  Result<std::vector<PresetSummary>> list(PresetKind) const;
  Result<PresetView> get(SelectedPreset) const;
  Result<PresetEditor> edit(PresetRef, PresetRevision expected);
  Result<PresetEditor> create(PresetKind,
                              std::string id,
                              std::string display_name,
                              std::optional<SelectedPreset> parent);
  Result<void> erase(PresetRef, PresetRevision expected);
  Result<PresetCompatibility> check_compatibility(
      SelectedPreset candidate,
      const PresetCompatibilityContext&) const;
};
```

repository 只管理 system/vendor/user 预设。`get()` 可读取任意 repository preset 的固定
revision 视图；只有 user preset 可通过 `edit()` 修改或通过 `erase()` 删除。create 始终创建
user preset。parent 与 child 必须同 kind，并由 revision 固定。编辑器打开时捕获自身、父链
和 schema revision；commit 时任一依赖变化均返回 `conflict`。

`create()` 的 `id` 直接成为 `PresetRef::id()`，`display_name` 成为 `PresetSummary::name`；两者
必须是非空、无内嵌 NUL 的有效 UTF-8，且不 trim、不做 Unicode normalization。id 按 byte-exact、
case-sensitive 比较且创建后不可变；display name 不参与 identity，允许重复。创建 editor 时
metadata 只在该 editor 中可见，成功 commit 后才进入 repository。创建时 catalog 已存在
`(kind, user, id)` 或 commit 前该 identity 被创建，返回 `conflict`，field `/preset/ref/id` 或
`/preset/storage_revision`；id/name 格式错误返回 `invalid_argument`，field `/preset/id` 或
`/preset/display_name`。

`PresetEditor::snapshot()` 返回当前 staged metadata/inherited/effective/overrides 的独立不可变
`PresetView`。它是 editor 的唯一读取入口；不会重复暴露四组与 `PresetView` 相同的 getter。

`set_override()` 对同一 OptionId 原位替换。`erase_override()` 删除本地覆盖；OptionId 当前
没有本地覆盖时幂等成功。成功 commit 直接返回包含新 revision 的 `PresetSummary`；不再包装
只有一个字段的 commit 类型。`discard()` 丢弃全部 staged 修改。
commit 或 discard 后 editor 进入终态，再次调用 mutator/commit 返回 `conflict`。

`inherited_values()` 返回完整父链合成值，`effective_values()` 返回 inherited 加当前本地
overrides。兼容性必须带 printer/process 上下文，不在 `PresetSummary` 中存放无上下文布尔值。

`check_compatibility()` 的输入错误固定为：缺少所需 printer/process 返回
`invalid_argument`；candidate 或 context revision 过期返回 `conflict`；kind 错误返回
`invalid_argument`。diagnostic field 分别指向 `/context/printer`、`/context/process`、对应
`/revision` 或 `/ref/kind`。输入合法但不兼容时 Result 成功，返回
`PresetCompatibility::compatible=false` 和原因 diagnostics。

project-embedded presets 由 Project 管理，不由 repository 接受。

`edit()`/`erase()` 的 expected revision 或 editor 捕获的自身/父链/catalog generation 过期时
返回 `conflict`，field `/expected_revision`；外部 user-store 存在状态或完整内容版本变化使用
`/preset/storage_revision`；这包括 target、任一 user-origin parent、create identity 占用状态或
捕获的 user-store manifest generation 被另一 Context/进程改变。同一 Context 自己发布的新
catalog generation 仍使用 `/expected_revision`。user preset 持久化失败使用 `/data_dir`。

## 8. 配置解析与分层语义

```cpp
class EffectiveConfiguration {
public:
  const ConfigSchema& schema() const;
  std::optional<ConfigValue> get(const OptionId&) const;
  const ConfigValues& values() const;

  struct Provenance {
    ProjectRevision project_revision;
    PlateId plate;
    std::vector<std::pair<PresetRef, PresetRevision>> presets;
  };

  Provenance provenance() const;
};
```

`EffectiveConfiguration::values()` 的含义严格限定为传给一次 plate 切片的全局/plate 配置：

```text
defaults
  → process
  → default filament
  → printer
  → project
  → selected filaments
  → plate
```

它不包含 object、part 或 layer-range overrides，也不声称代表任意几何位置的最终 region
配置。object、part 和 layer-range 始终保存在 `ProjectSnapshot` 的模型分层中，并与该
`EffectiveConfiguration` 一起参与切片。

调用方需要配置编辑器视图时，可以用 `EffectiveConfiguration` 作为 object 的 inherited
base，再按 snapshot 公开的 object/part/layer-range patch 显示本地覆盖。v1 不提供把这些
分支压成单一 `ConfigValues` 的 API。

## 9. Filament map

```cpp
enum class FilamentMapMode { auto_for_flush, auto_for_match, manual };

struct FilamentMapOverride {
  FilamentMapMode mode;
  std::vector<ToolId> tools;
};

struct EffectiveFilamentMap {
  FilamentMapMode mode;
  std::vector<ToolId> tools;
  enum class Source { project, plate, temporary } source;
};
```

map 索引与 0 基 logical filament slot 一一对应，值为 1 基 physical ToolId。project map 可被
plate local map 覆盖。切片入口的 atomic temporary selection 不写回工程，且必须和覆盖全部新
logical slots 的 complete manual map 一起出现；具体请求见第 11 节。

任一 mode 的 `tools.size()` 必须恰好等于当前 selection 的 logical filament count，且每个
`ToolId` 非零；多个 logical slots 允许引用同一 ToolId。结构/长度/零值错误返回
`invalid_argument`，field 为 `/filament_map/tools` 或 `/filament_map/tools/{index}`；超过当前
printer physical tool count 返回 `invalid_configuration` 并指向对应 index。auto mode 中 tools
只是需要无损保存的最近映射/候选映射，不是 SDK 承诺重新计算后的最终值；set/load/save 仍要求
它是完整且范围合法的列表，但切片 inspect/submit 按下述规则拒绝 auto mode。

v1 公共对象模型不接收设备、AMS 或 physical filament topology，因此 v1 只承诺 manual map
切片：

- load/snapshot/save 必须无损保留 `auto_for_flush` 和 `auto_for_match` mode 及已有 tools；
- `effective_filament_map()` 可以返回继承后仍为 auto 的持久化状态；
- `SliceEngine::inspect()` 或 `submit()` 解析到 auto mode 时返回 `unsupported`；
- SDK 不得把 auto mode 静默改成 manual 后继续切片；
- App 可在同一 ProjectEdit 中显式改为 manual 并提交完整 map 后切片。

未来若增加 auto mapping，必须新增带完整强类型物理耗材拓扑的公开输入，并另行冻结；不能在
现有方法中偷偷改变 auto mode 语义。

## 10. Project 输入、工程模型和事务

```cpp
struct PlateInfo { PlateId id; std::string name; ConfigPatch overrides; bool locked; };
struct ObjectInfo { ObjectId id; std::string name; ConfigPatch overrides; };
struct PartInfo { PartId id; ObjectId object; ConfigPatch overrides; };

struct LayerRange {
  double z_min_mm;
  double z_max_mm;
  ConfigPatch overrides;
};

struct SlotRemap {
  std::vector<std::optional<FilamentSlotId>> old_to_new;
};

struct Vec3d { double x, y, z; };

struct Triangle {
  std::uint32_t a;
  std::uint32_t b;
  std::uint32_t c;
};

struct Matrix4d {
  std::array<double, 16> row_major;
};

struct MeshData {
  std::vector<Vec3d> vertices_mm;
  std::vector<Triangle> triangles;
};

struct MeshPartInput {
  std::string name;
  MeshData mesh;
  ConfigPatch overrides;
};

struct ObjectInput {
  std::string name;
  std::vector<MeshPartInput> parts;
  ConfigPatch overrides;
};

struct InstanceInput {
  ObjectId object;
  Matrix4d transform;
};

class Project;

class ProjectSnapshot {
public:
  ProjectRevision revision() const;
  ConfigPatch project_overrides() const;
  std::vector<PlateInfo> plates() const;
  std::vector<ObjectInfo> objects() const;
  std::vector<PartInfo> parts() const;
  Result<std::vector<InstanceId>> instances(PlateId) const;
  Result<std::vector<LayerRange>> layer_ranges(ObjectId) const;
  PresetSelection project_selected_presets() const;

  FilamentMapOverride project_filament_map() const;
  Result<std::optional<FilamentMapOverride>>
      local_filament_map_override(PlateId) const;
  Result<EffectiveFilamentMap> effective_filament_map(PlateId) const;
};

class ProjectEdit {
public:
  Result<void> set_project_selected_presets(PresetSelection, SlotRemap);
  Result<void> set_project_overrides(ConfigPatch);
  Result<void> set_plate_overrides(PlateId, ConfigPatch);
  Result<void> set_object_overrides(ObjectId, ConfigPatch);
  Result<void> set_part_overrides(PartId, ConfigPatch);
  Result<void> set_layer_ranges(ObjectId, std::vector<LayerRange>);
  Result<void> set_project_filament_map(FilamentMapOverride);
  Result<void> set_local_filament_map_override(
      PlateId,
      std::optional<FilamentMapOverride>);
  Result<ProjectRevision> commit();
  void discard() noexcept;
};

class ProjectBuilder {
public:
  Result<void> set_selected_presets(PresetSelection);
  Result<void> set_project_overrides(ConfigPatch);
  Result<void> set_project_filament_map(FilamentMapOverride);

  Result<PlateId> add_plate(std::string name, ConfigPatch overrides = {});
  Result<ObjectId> add_object(ObjectInput);
  Result<InstanceId> add_instance(PlateId, ObjectId, Matrix4d transform);
  Result<void> set_layer_ranges(ObjectId, std::vector<LayerRange>);

  Result<Project> build();
  void discard() noexcept;
};

class Project {
public:
  static Result<Project> load(SdkContext&, const std::filesystem::path&);
  Result<ProjectSnapshot> snapshot() const;
  Result<ProjectEdit> begin_edit(ProjectRevision expected);
  Result<void> save(const std::filesystem::path&,
                    ProjectRevision expected) const;
};
```

`ProjectBuilder` 是外部 App 输入已解析 mesh/scene 的唯一正式入口。它不解析文件、不生成
generic 3MF、不做自动排版，也不提供 mesh 编辑 API。调用方负责 STL/OBJ/STEP/数据库模型的读取、
修复和排版；SDK 只接收已确定的 vertices、triangles、plate、object、part、instance、preset
selection 和配置 patch。

builder 坐标和几何规则固定如下：

- `MeshData::vertices_mm` 的坐标单位始终是 mm，SDK 不做 inch/meter 等单位猜测；
- `Triangle::{a,b,c}` 是 0 基 vertex index，必须全部小于 `vertices_mm.size()`；
- 所有 double 字段必须有限，NaN/Inf 返回 `invalid_argument`，field 指向对应公开字段；
- 空 object、空 parts、空 mesh、越界 triangle、退化三角形和超过 `model_triangles` limit 的输入
  必须拒绝；退化规则以 core 可稳定 materialize 的最小合法 triangle 为准，不能在切片中途崩溃；
- `Matrix4d::row_major` 是 4x4 row-major affine transform。不可逆、包含 NaN/Inf 或会产生
  非有限坐标的 transform 返回 `invalid_argument`；
- `add_instance()` 只把既有 object 放到目标 plate；它不复制 object mesh，也不做 build-volume
  自动修正；
- `build()` 必须完整校验 preset selection、manual filament map、配置 patch 和 geometry 后一次性
  发布 Project。失败不返回部分 Project；
- builder 生成的 `Project` 与 3MF load 生成的 `Project` 使用同一 `ProjectSnapshot`、
  `ProjectEdit`、`SliceRequest` 和 `SliceEngine` 行为。

builder 不暴露也不接受 `Slic3r::Model`、`TriangleMesh`、`ModelObject`、`ModelVolume` 等 core 私有
类型。外部调用方不能通过 include Orca 私有头来构造 ProjectSnapshot。

`ProjectEdit` 是唯一工程写入口。所有 staged 修改只有一次 commit；失败时工程和 revision 均
不变，成功返回新 revision。`discard()` 丢弃 staged state；commit 或 discard 后 editor 进入
终态，再次调用 mutator/commit 返回 `conflict`。`save()` 不修改 revision。

`set_project_selected_presets()` 必须同时提供覆盖所有旧 logical slots 的 SlotRemap。commit
原子迁移 map、schema 标记为 filament reference 的公开 scopes，以及工程内部保存的 embedded
preset/custom G-code references。无法安全迁移的隐藏数据不猜测改写，commit 失败且不发布。

`SlotRemap` 的新 slot count 等于新 `PresetSelection::filaments.size()`，规则固定如下：

- `old_to_new.size()` 必须等于旧 slot count；每个非 null target 必须小于新 slot count，且所有
  非 null target 互不重复。违反任一结构规则返回 `invalid_argument`，field 指向
  `/slot_remap/old_to_new` 或对应 index；
- null 表示删除旧 slot；没有任何 old slot 指向的 new slot 表示新增。配置值自身的 public null
  保持 null；其他 logical-slot scalar/list item 按 mapping 替换；
- 被删除 slot 仍由公开配置引用时，不自动改成 null/default。调用方可在同一个 ProjectEdit 中
  通过对应 scope patch 显式改成 null、其他有效 slot 或 erase；commit 时仍悬空返回
  `invalid_configuration` 并指向公开 scope 字段；
- 上述迁移覆盖 project、plate、object、part、layer-range；内部还必须以同一 reference 规则迁移
  project-embedded preset overrides 和 structured custom G-code。隐藏引用被删除时返回
  `invalid_configuration`，不得把内部字段变成新的 v1 公共 CRUD；
- project map 和每个已有 local map override 先按 slot index 迁移，删除项丢弃。只要存在新增
  slot，调用方必须在同一事务中分别提交完整的新 project map，并为每个已有 local override
  提交完整新 map；不得猜测新增 slot 的 ToolId；
- raw G-code 受 slot 变化影响且无法证明安全时，commit 返回 `unsupported`。

ProjectEdit mutator 允许暂存上述尚未修复的跨字段状态，完整的 dangling-reference、map 和
selection 交叉校验统一在 commit 执行，从而允许调用方在一次事务中原子修复。

只对 `begin_edit()` 捕获且未被本事务显式覆盖或 erase 的原始 state 应用 SlotRemap。只要事务
包含 selection change，所有 ProjectEdit mutator 显式传入的 ConfigPatch 和 filament map 都使用
目标 selection 的 0 基 slot namespace，不再经过 remap；这些 mutator 在 selection mutator 之前
或之后调用必须产生完全相同的结果。显式 erase
作为 overlay 保留，不能在迁移原始 state 后重新出现。SlotRemap 长度、target 范围和重复 target
在 `set_project_selected_presets()` 即时校验；dangling reference/map 等跨字段错误留到 commit。
v1 的每个 ProjectEdit 最多成功调用一次 `set_project_selected_presets()`。首次结构校验失败不修改
staged state、不建立 target namespace、也不消耗该机会；首次成功后的任何再次调用返回
`conflict`，field `/selection`，避免已按第一个 target namespace 暂存的 overlay 被重新解释。

load/save 必须保真保存模型、instance、plate、embedded preset、custom G-code、map 以及
SDK 未公开但能够保留的切片相关 3MF 数据。无法安全保留时必须拒绝操作，不能成功后静默
丢失。

instance transform/printability 查询、custom G-code CRUD 和 project-embedded preset 的完整
查询/CRUD 不属于 v1 核心公共面。它们仍是内部 ProjectState 的强制数据：必须 round-trip，按
目标 plate 参与切片，并遵守 SlotRemap；未来只能在独立扩展 API 重新审核后公开。

## 11. FFF 切片

```cpp
struct TemporarySliceSelection {
  PresetSelection selection;
  FilamentMapOverride complete_manual_map;
};

enum class PreviewDelivery { none, memory, artifact };

struct SliceOutputOptions {
  bool include_gcode = true;
  PreviewDelivery preview = PreviewDelivery::none;
  std::optional<std::filesystem::path> preview_artifact_path;
};

struct SliceRequest {
  ProjectSnapshot project;
  PlateId plate;
  std::optional<TemporarySliceSelection> temporary_selection;
  SliceOutputOptions output;
};

struct SliceInspection {
  EffectiveConfiguration effective_configuration;
  EffectiveFilamentMap effective_filament_map;
};

enum class SliceEventKind {
  preparing,
  validating,
  slicing,
  exporting,
  warning,
  completed,
  failed,
  cancelled
};

struct SliceEvent {
  SliceEventKind kind;
  int percent;
  std::optional<Diagnostic> diagnostic;
};

using SliceCallback = std::function<void(const SliceEvent&)>;

struct FilamentUsage {
  FilamentSlotId slot;
  double length_mm;
  double volume_mm3;
  double mass_g;
};

struct SliceStatistics {
  std::chrono::milliseconds elapsed;
  std::uint64_t layer_count;
  std::vector<FilamentUsage> filament_usage;
};

enum class PreviewMoveType {
  travel,
  extrude,
  retract,
  unretract,
  tool_change,
  color_change,
  custom,
  unknown
};

enum class PreviewPathKind { linear, arc, unknown };

enum class PreviewExtrusionRole {
  none,
  perimeter,
  external_perimeter,
  overhang_perimeter,
  internal_infill,
  solid_infill,
  top_solid_infill,
  bridge_infill,
  support_material,
  support_interface,
  skirt,
  brim,
  wipe_tower,
  custom,
  unknown
};

enum class PreviewColorSource { filament, color_change, custom, unknown };

struct PreviewColor {
  std::uint32_t id;
  std::array<std::uint8_t, 4> rgba;
  PreviewColorSource source;
  std::optional<FilamentSlotId> filament;
  std::string name;
};

struct PreviewMetadata {
  std::string key;
  std::string value;
};

struct PreviewObject {
  ObjectId object;
  std::string name;
};

struct PreviewInstance {
  InstanceId instance;
  ObjectId object;
  std::optional<PlateId> plate;
  Matrix4d transform;
};

struct PreviewLayer {
  std::uint32_t id;
  std::uint64_t move_begin;
  std::uint64_t move_count;
  double print_z_mm;
  double height_mm;
  double duration_s;
};

struct PreviewTool {
  ToolId tool;
  std::optional<FilamentSlotId> primary_filament;
  double nozzle_diameter_mm;
  Vec3d offset_mm;
};

struct PreviewFilament {
  FilamentSlotId slot;
  ToolId mapped_tool;
  std::array<std::uint8_t, 4> rgba;
  double diameter_mm;
  double density_g_cm3;
  double cost_per_kg;
};

struct PreviewMove {
  std::uint64_t id;
  std::optional<std::uint64_t> gcode_id;
  std::uint32_t layer_id;
  std::optional<ObjectId> object;
  std::optional<InstanceId> instance;
  std::optional<ToolId> tool;
  std::optional<FilamentSlotId> filament;
  std::optional<std::uint32_t> color_id;
  PreviewMoveType type;
  PreviewPathKind path_kind;
  PreviewExtrusionRole extrusion_role;
  Vec3d start_mm;
  Vec3d end_mm;
  std::optional<Vec3d> arc_center_mm;
  double extrusion_delta_mm;
  double feedrate_mm_s;
  double actual_feedrate_mm_s;
  double width_mm;
  double height_mm;
  double mm3_per_mm;
  double distance_mm;
  double fan_speed_percent;
  double temperature_c;
  double pressure_advance;
  double acceleration_mm_s2;
  double jerk_mm_s;
  double time_s;
  double layer_duration_s;
  double print_z_mm;
  std::optional<double> joint_angle_end_rad;
};

enum class PreviewEventType {
  tool_change,
  color_change,
  pause,
  custom_gcode,
  warning,
  unknown
};

struct PreviewEvent {
  std::uint64_t id;
  std::optional<std::uint64_t> move_id;
  PreviewEventType type;
  std::optional<ToolId> tool;
  std::optional<FilamentSlotId> filament;
  double print_z_mm;
  double time_s;
  std::string message;
};

struct SlicePreview {
  std::string schema_id;
  std::uint32_t schema_version;
  std::string coordinate_space;
  std::vector<PreviewMetadata> metadata;
  std::vector<PreviewLayer> layers;
  std::vector<PreviewTool> tools;
  std::vector<PreviewFilament> filaments;
  std::vector<PreviewColor> colors;
  std::vector<PreviewObject> objects;
  std::vector<PreviewInstance> instances;
  std::vector<PreviewMove> moves;
  std::vector<PreviewEvent> events;
};

struct SliceResult {
  std::optional<std::string> gcode_bytes;
  std::shared_ptr<const SlicePreview> preview;
  std::optional<std::filesystem::path> preview_artifact_path;
  EffectiveConfiguration effective_configuration;
  EffectiveFilamentMap effective_filament_map;
  SliceStatistics statistics;
  std::vector<Diagnostic> diagnostics;
};

class SliceJob {
public:
  Result<void> cancel();
  Result<std::shared_ptr<const SliceResult>> wait();
};

class SliceEngine {
public:
  Result<SliceInspection> inspect(const SliceRequest&) const;
  Result<SliceJob> submit(SliceRequest, SliceCallback = {});
};
```

`SliceRequest` 直接携带不可变工程 snapshot 和目标 plate。`temporary_selection` 为空时使用工程
固定的 selection 与 effective map；有值时 selection 与覆盖全部新 logical slots 的 manual map
作为一个原子对象出现，缺任一部分都无法构造请求，且不写回 Project。temporary map mode 不是
manual、长度/ToolId 范围不合法或持久 effective map 为 auto 时，`inspect()`/`submit()` 按第 9 节
失败。

`SliceOutputOptions` 控制本次切片产物：

- 默认只返回 G-code，不生成 preview；
- `include_gcode=false && preview=PreviewDelivery::none` 是 `invalid_argument`；
- `include_gcode=true` 时成功结果的 `SliceResult::gcode_bytes` 必须有值，且 byte count 与实际
  输出完全一致；
- `include_gcode=false` 时成功结果的 `SliceResult::gcode_bytes` 必须为 `std::nullopt`；
- `preview=memory` 时 `SliceResult::preview` 非空，`preview_artifact_path` 为空；
- `preview=artifact` 时必须提供 `preview_artifact_path`，成功后该路径存在完整 artifact，
  `SliceResult::preview_artifact_path` 等于最终路径，`preview` 可为空；
- `preview=none` 时不得额外计算或保留 preview 数据；
- 支持 preview-only，也支持同一次 slice 同时输出 G-code 和 preview。

preview 是 SDK 自己的公共 DTO/产物语义，复用旧 `.orcapv v2` 的字段含义，但不承诺旧 worker
wire ABI，也不暴露 `GCodeProcessorResult`、worker protocol 或任何 `Slic3r::*` 类型。preview 必须
由本次 `process/export` 产生的权威 `GCodeProcessorResult` 转换得到；禁止通过重新解析
`gcode_bytes` 作为 fallback。G-code 与 preview 同时请求时，二者必须来自同一次冻结输入和同一次
core 切片。

object/instance 归属只在 core 能可靠证明时填写。无法证明、被过滤或没有对应公开 entity 时，
`PreviewMove::object`/`instance` 使用 `std::nullopt`，不得猜测或伪造 ID；调用方以 optional 是否有值
作为归属 flag。`SlicePreview::objects`/`instances` 只列出可由冻结 ProjectSnapshot 证明的公开
entity；artifact 中对应 ID 仅表示同一次 SliceResult 所属 project 的公开 ID 值，不是跨工程身份。
preview 坐标空间固定为 Orca plate world mm，`SlicePreview::coordinate_space` 必须返回稳定字符串。

`inspect()` 执行与 submit 完全相同的同步解析和校验，只返回本次请求将使用的
`EffectiveConfiguration` 与 `EffectiveFilamentMap`，不创建 job。`submit()` 必须在返回前同步
捕获 preset revisions、合成 global+plate configuration、验证 manual map，并冻结工程 snapshot、
目标 plate 和解析结果；随后 job 只使用该冻结输入。submit 后 Project 或 repository 的变化不得
改变 job。切片同时使用工程快照保存的分层 object/part/layer-range 与内部 instance/custom
G-code/embedded preset 数据。

每个 engine 最多一个 active job；忙时 `submit()` 返回 `busy`，不进入第二个公开队列。
`cancel()` 幂等。`wait()` 可重复、可并发调用；只有 completed 返回 SliceResult，失败或取消
不返回部分结果。

callback 按单调事件顺序调用，恰有一个终态事件。callback 可调用 `cancel()`，不得在同一
callback 中调用该 job 的 `wait()`。callback 第一次抛异常时 SDK 捕获异常、停用该 callback，
继续 job，并在最终 `wait()` Result diagnostics 中追加且只追加一个
`{ErrorCode::internal, Severity::warning, ..., "/callback"}`；不递归投递 warning event，异常不
越过 SDK 边界。

公开错误映射固定为：输入形状/缺失上下文是 `invalid_argument`；找不到 ref/entity 是
`not_found`；revision/state 竞争是 `conflict`；engine 已有 active job 是 `busy`；格式或 v1
未承诺能力是 `unsupported`；文件系统失败是 `io`；预算超限是
`resource_limit_exceeded`；schema、项目交叉约束或 validate 阶段拒绝是
`invalid_configuration`；取消是 `cancelled`；validate 已成功后 process/export 的 core 失败是
`slicing_failed`；未被其他 code 覆盖的 SDK 异常是 `internal`。同一失败点不得在
`invalid_configuration` 与 `slicing_failed` 之间任选。

`gcode_bytes` 有值时由 `std::string::size()` 定界，不追加 NUL，不验证或转换 UTF-8。SDK 不执行
post-process 脚本；配置中存在非空 post-process 时返回 `invalid_configuration`。

preview 内存结果和 artifact 均受 `preview_bytes` 与 `preview_moves` 限制；artifact 写入同时计入
`temporary_disk_bytes`。任一 preview 限制或 artifact 写入超限返回 `resource_limit_exceeded`，
不得返回半截 preview、半截 artifact 或部分 SliceResult。取消或失败时 artifact 路径不得留下
可被误认为成功结果的完整文件；实现应使用临时文件加原子发布或等价机制。

## 12. 线程和内存所有权

- `SdkContext`、`PresetRepository`、`PresetView`、`ProjectSnapshot`、`ConfigSchema`、
  `EffectiveConfiguration`、`SliceInspection` 和完成后的 `SliceResult` 可复制并安全并发读；
- `PresetEditor`、`ProjectEdit` 和 `ProjectBuilder` 只能由创建它的线程串行使用；
- `Project` 允许并发 snapshot/save/begin_edit，但 revision 冲突必须显式返回；
- `SliceEngine::inspect()`/`submit()`、`SliceJob::cancel()` 和 `SliceJob::wait()` 遵守第 11 节语义；
- 按值返回的 string、vector、ConfigValue、mesh DTO、preview DTO、snapshot 和 result 拥有自己的生命周期，不借用
  core 指针；
- `Result::diagnostics()`、`ConfigPatch/ConfigValues::entries()`、
  `EffectiveConfiguration::schema()/values()` 的 const-reference getter
  返回对其公开 owner state 的只读借用；引用只在 owner 未被销毁、移动赋值或重新赋值期间
  有效，调用方需要独立生命周期时必须复制；
- `PresetRepository`、`PresetView`、`PresetEditor`、`Project`、`ProjectEdit`、`ProjectBuilder`、
  `SliceEngine` 和已提交 `SliceJob` 都强持有其所需 context
  state；销毁最初的 SdkContext 或创建它们的 parent handle 后仍可按本文继续使用，直至这些
  handle 自身释放；
- 销毁 App 持有的 engine/context handle 不得使已经返回的 SliceJob 或不可变结果悬空；
- SDK 不保留调用方传入 `std::string_view`、callback 参数引用或临时容器的借用引用。

## 13. 安装和版本检查

默认安装只导出 `libslicer::sdk_v1`、v1 公共头和 CMake package。不得要求调用方包含 Orca
内部头文件或链接 GUI/worker target。静态包所需链接依赖由 CMake target 传递。

版本检查分为两个边界：

- CMake package version 与公共头 SDK major/minor 在 configure/compile/link 阶段检查；不要求
  runtime context 推断调用方曾使用的 package version；
- `ConfigSchema::schema_id()` 和 `schema_version()` 在 schema 查询及 JSON import/export 时
  检查。

package/header major 不匹配必须使 consumer configure、compile 或 link 失败；JSON/schema
不匹配必须在 import 或 schema 使用入口返回明确错误，不能等到切片中途失败。
