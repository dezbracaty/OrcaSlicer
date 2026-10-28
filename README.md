# libslicer

`libslicer` is a minimal static-library build of OrcaSlicer's core slicing
engine, extracted around `src/libslic3r`.

This branch intentionally excludes the wxWidgets application, GUI resources,
packaging scripts, installer workflows, and unrelated test suites. It keeps the
runtime profile data needed by `libslic3r` preset loading.

## Build

```bash
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release
```

The macOS arm64 internal core artifact is:

```bash
build-libslicer/release/arm64/src/libslic3r/liblibslicer_core.a
```

Other presets are available in `CMakePresets.json` for macOS, Linux, and
Windows release/debug builds.

## Tests

The CMake project keeps the `tests/libslic3r` Catch2 suite connected by default.

```bash
cmake --build --preset macos-arm64-release --target libslic3r_tests
ctest --test-dir build-libslicer/release/arm64 --output-on-failure
```

When consumed from another CMake project with `add_subdirectory`, tests are
disabled by default. Link raw internal APIs against `libslicer::core`:

```cmake
set(LIBSLICER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/libslicer)

target_link_libraries(my_app PRIVATE libslicer::core)
```

Application integrations should install this project and consume the stable
facade package using `find_package(libslicer CONFIG REQUIRED)`, then link
`libslicer::libslicer`. The facade deliberately hides OrcaSlicer's internal
third-party dependency graph.

Applications using profile or resource-backed APIs must initialize runtime
paths before loading presets or related data:

```cpp
Slic3r::set_resources_dir("/path/to/libslicer/resources");
Slic3r::set_data_dir("/path/to/app-data");
```

## Kept Runtime Data

The following resource groups are intentionally retained because they are used
or closely coupled to `libslic3r` runtime behavior:

- `resources/profiles`
- `resources/profiles_template`
- `resources/printers`
- `resources/fonts`
- `resources/flush`

Calibration and handy-model resources are installed with the manual calibration
facade documented below.

## Manual calibration API

本文对应 `include/libslicer/Calibration.hpp`、`Library.hpp` 及 GPlatform App 的 `SlicingBackend.hpp`，用于调用方接入。

### 范围与命名

提供手动校准的能力查询、任务准备、逐盘切片、预览和 G-code 3MF 输出。接口不执行设备自动测量，不读取打印机测量结果，不自动采纳结果或保存预设。

| SDK / 内部名称 | Orca 对应项 | 说明 |
| --- | --- | --- |
| `libslicer::CalibMode` | `Slic3r::CalibMode` | 原枚举名、枚举项和 0～12 数值保持一致；13 为 SDK 的公差模型入口。 |
| `libslicer::orca::Plater` | `Slic3r::GUI::Plater` | 内部校准场景，保留 `calib_pa / calib_flowrate / calib_temp / ...` 原入口名。 |
| 内部 `Selection / PartPlate / PartPlateList` | Orca 同名场景类 | 只实现校准所需的实际场景操作，不包含窗口或空 GUI 方法。 |
| 原生 `Slic3r::Calib_Params` | Orca 同名结构 | 内部直接使用原结构，不重新定义算法参数类型。 |
| `CalibrationRequest` | `Calib_Params` + 对话框其他实参 + 配置快照 | SDK 请求包装；额外包含配置、流量轮次等，不能冒充原 `Calib_Params`。 |
| `CalibrationTask` | 无直接同名类型 | SDK 的任务所有权对象，保存原生场景、精确配置及所有盘。 |
| `CalibrationObject / CalibrationPlate / CalibrationLayerCode` | 模型、盘及逐层代码的只读数据 | 供 App 展示的快照，不是原生 `ModelObject / PartPlate` 的替代实现。 |
| `CalibrationPrepareResult / CalibrationOutputOptions` | 无直接同名类型 | SDK 的返回值和输出路径包装。 |

此前开发中的 `CalibrationMode::PressureAdvanceLine` 等名称已替换为 `CalibMode::Calib_PA_Line` 等 Orca 原名，没有保留第二套别名。调用方需更新名称并重新编译。

### 头文件与链接

```cpp
#include <libslicer/Calibration.hpp>
```

该头文件包含 `Library.hpp`，无需引用内部 `libslic3r` 或 wxWidgets 头文件。

```cmake
find_package(libslicer CONFIG REQUIRED)
add_executable(calibration_demo main.cpp)
target_compile_features(calibration_demo PRIVATE cxx_std_17)
target_link_libraries(calibration_demo PRIVATE libslicer::libslicer)
```

运行目录需包含 SDK 的 DLL / 动态库及资源。显式 `resource_directory` 必须指向含 `profiles`、`calib`、`handy_models` 的资源根目录，例如 App 的 `resources/libslicer`。留空时使用 `Library::open` 的安装资源查找规则。

### 方法

参数表单规则也由库提供，调用方无需复制 `calib_dlg.cpp` 的数值或固件判断。以下三个函数不依赖 Qt、wxWidgets 或已打开的 `Library`，只使用调用方传入的配置快照；不会修改活动配置或准备模型。

```cpp
CalibrationParameters calibration_parameters(
    CalibMode mode, const ConfigSnapshot& config,
    const std::vector<std::pair<std::string, std::string>>& values = {});
CalibrationParameters update_calibration_parameters(
    CalibrationParameters parameters, std::string_view key,
    std::string_view value, bool focus_out = false);
CalibrationValidationResult validate_calibration_parameters(
    const CalibrationParameters& parameters);
```

`CalibrationParameters.fields` 复用 `SettingItem`：`key/value/default_value` 为字段及文本值，`enum_items` 为可选值，`unit` 为单位，`visible/enabled/read_only` 表达当前模式与固件的字段状态。`label` 和选项标签为源语言文本，翻译与控件布局留给 App。字段包括 `extruder_type`（DDE/Bowden，与材料槽 `extruder_id` 无关）、`pa_method`、`flow_type`、`flow_pattern`、`filament_type`、`test_model`、`shaper_type`、起止值、步长、X/Y 频率、批量速度/加速度及 `print_numbers`。布尔值使用 `"0"/"1"`，枚举值使用 `enum_items.value`，其余数值在编辑阶段也保留文本，允许暂存空值或未完成输入。

`calibration_parameters` 先建立原版默认值，再恢复可选的已保存字段；恢复过程不模拟单选框切换，不意外重置用户范围。`update_calibration_parameters` 执行一次明确的字段事件：PA 方法或 DDE/Bowden **发生变化**时按原版重置范围，材料温度快捷项发生变化时更新温度，RRF 的 Y 轴随 X。`can_refine` 表示当前流量首轮可继续对应精调，通过 `key="next_pass"` 进入下一轮。不支持的模式、无效配置和未知字段名抛出 `std::invalid_argument`；无效字段文本保留到预检时返回诊断。

温度输入框失焦时传 `focus_out=true`：库按原版限制到 155–500 ℃，向下取到 5 ℃，超范围提示返回在 `CalibrationParameters.diagnostics` 中。普通编辑与原生 `prepare_calibration` **不会隐式执行失焦取整**，因此原 API 接受合法、非 5 整除的整数端点这一行为保持不变。

预检返回 `CalibrationValidationResult`，成功时 `request` 可直接传给 `prepare_calibration`；失败或警告见 `diagnostics`。预检与任务准备共用同一 `dialog_parameters` 校验实现及转角警告条件，App 不重复维护校验。例：

```cpp
auto form = libslicer::calibration_parameters(
    libslicer::CalibMode::Calib_PA_Tower, config.snapshot());
form = libslicer::update_calibration_parameters(std::move(form), "pa_method", "1");
form = libslicer::update_calibration_parameters(std::move(form), "end", "0.07");
auto checked = libslicer::validate_calibration_parameters(form);
if (checked) {
    auto prepared = library->prepare_calibration(checked.request);
    // 后续按已有逐盘切片接口处理。
}
```

```cpp
std::vector<CalibMode> Library::calibration_modes() const;

CalibrationPrepareResult Library::prepare_calibration(
    const CalibrationRequest& request,
    const SliceCallbacks& callbacks = {}) const;

SliceResult Library::slice_calibration(
    const CalibrationTask& task,
    std::size_t plate_index,
    const CalibrationOutputOptions& output,
    const SliceCallbacks& callbacks = {}) const;
```

| 方法 | 前置条件 | 返回与副作用 |
| --- | --- | --- |
| `calibration_modes` | 已成功打开 Library | 返回下表的 12 个手动入口；这是实现入口列表，不是打印机硬件能力探测，也不按固件过滤。 |
| `prepare_calibration` | `request.config.valid()`；参数符合所选模式规则 | 成功返回独占任务；原版方法载入模型、修改任务内预设、裁剪、缩放和排盘。不切片，不修改 Library 的活动配置，不保存用户预设。 |
| `slice_calibration` | 准备成功；`plate_index < task.plates().size()` | 按该盘原生配置和模型切片，返回普通 `SliceResult`；保留校准参数及逐层自定义代码，不重新居中或排盘。 |

所有方法同步返回。`CalibrationTask` 不能自行构造、复制或赋值，由成功的准备结果创建。任务的入口为：

```cpp
const std::vector<CalibrationPlate>& CalibrationTask::plates() const noexcept;
const CalibrationRequest& CalibrationTask::request() const noexcept;
```

`request()` 返回调用方原始请求；它不是处理后的原生参数。例如 RRF 的轴联动、非正批量值过滤、体积流量到线速度的转换不会改写该原始请求。

### 模式、单位与输入规则

以下规则来自固定 Orca 版本的对话框及校准方法。所有 double 字段都必须为有限数，包括当前模式没有使用、保持默认 0 的字段。不要求范围恰好整除步长，不额外限制采样数量，不修正原版终值取整。

| `CalibMode` | 值 | 必填参数、单位与规则 |
| --- | --- | --- |
| `Calib_PA_Line` | 1 | `start/end/step` 为固件 PA/K 数值，不转换固件单位；`start >= 0`，`step >= 10*EPSILON`（当前为 0.001），`end >= start+step`。步长按原版逐线使用。 |
| `Calib_PA_Pattern` | 2 | 同上；可选 `speeds`（mm/s）和 `accelerations`（mm/s²）；原版按组合排盘，可能返回多个盘。 |
| `Calib_PA_Tower` | 3 | 同 PA 范围；原版按毫米高度分段改变 PA。 |
| `Calib_Flow_Rate` | 5 | 使用 `flow_linear / flow_pass / flow_pattern`；不使用 `start/end/step`。 |
| `Calib_Temp_Tower` | 6 | `start/end` 单位 ℃，必须为非负整数；`start <= 500`，`end >= 155`，`end <= start-5`。`step` 不参与计算，内部保留原版 5℃ 规则；不额外要求起止值为 5 的倍数。 |
| `Calib_Vol_speed_Tower` | 7 | `start/end` 为 mm³/s，`step` 为原版每毫米高度增加的体积流量系数；`start > 0`、`step > 0`、`end >= start+step`。内部按原截面积转换线速度，调用方不要预先转换。 |
| `Calib_VFA_Tower` | 8 | `start/end/step` 单位 mm/s；`start > 10`、`step > 0`、`end >= start+step`。原版按 5mm 高度段增加速度。 |
| `Calib_Retraction_tower` | 9 | `start/end/step` 为回抽长度 mm；`start >= 0`、`step > 0`、`end >= start+step`。按原版塔高度分段改变回抽长度。 |
| `Calib_Input_shaping_freq` | 10 | `freqStartX/freqEndX/freqStartY/freqEndY` 单位 Hz，每轴 `0 <= start < end <= 500`；`start` 字段为固定阻尼，范围 `[0,1)`；使用 `shaper_type / test_model`，不使用通用 `end/step`。 |
| `Calib_Input_shaping_damp` | 11 | `freqStartX/freqStartY` 为固定频率 Hz，范围 `[0,500]`；`start/end` 为无量纲阻尼，`0 <= start < end <= 1`；使用 `shaper_type / test_model`，不使用 `step/freqEndX/freqEndY`。 |
| `Calib_Cornering` | 12 | `start/end` 为固件的转角参数；Marlin 2（`marlin2`）且首个 `machine_max_junction_deviation > 0` 时是 JD（mm），`0 <= start < end <= 0.3`；其他情况是 jerk/SCV（mm/s），上限 100。使用 `test_model`，不使用 `step`。 |
| `Calib_Tolerance_Model` | 13 | SDK 扩展入口，载入原 `OrcaToleranceTest.drc`，无扫描参数。 |

`Calib_None = 0` 不能作为准备入口。`Calib_Auto_PA_Line = 4` 保留 Orca 原枚举位置，但本接口未实现；两者都不会出现在 `calibration_modes()` 中，传入会返回失败。

#### `CalibrationRequest` 全部字段

| 字段 | 类型、默认值 | 含义 |
| --- | --- | --- |
| `config` | `ConfigSnapshot`，无有效配置 | SDK 层必填，通常来自 `create_config`、活动配置或已有 Config 的 `snapshot()`。 |
| `mode` | `CalibMode::Calib_None` | 必须显式选择支持的入口。 |
| `extruder_id` | int，0 | 原 `Calib_Params` 字段。当前原版 Plater 温度塔用它选择喷嘴尺寸：负数按 0，越界按最后一个喷嘴；不能视为所有模式的物理喷头切换接口。其他模式继续使用原实现的首喷嘴 / 首耗材逻辑。 |
| `start/end/step` | double，均为 0 | 含义随模式变化，见上表。默认值不是可打印的完整参数方案。 |
| `print_numbers` | bool，true | 原版 PA 数字标识开关，由原生成器使用。 |
| `freqStartX/freqEndX/freqStartY/freqEndY` | double，均为 0 | 输入整形频率参数，单位 Hz。 |
| `test_model` | int，0 | 输入整形：0 Ringing Tower、1 Fast Tower；Cornering：0 Ringing Tower、1 Fast Tower、2 SCV-V2。 |
| `shaper_type` | string，空 | 输入整形算法，大小写敏感；空值选择原对话框的第一项，允许值见下表。 |
| `accelerations` | `vector<int>`，空 | PA 批量加速度。保留原解析器的正整数规则，非正值被过滤。 |
| `speeds` | `vector<int>`，空 | PA 批量速度，同样过滤非正值。两列表过滤后均非空时，必须 `min(accelerations) > max(speeds)`。Pattern 中空列表由原版预设及最优速度逻辑填充。 |
| `flow_linear` | bool，false | false 为比例法，true 为 YOLO 线性法。 |
| `flow_pass` | int，1 | 仅允许 1 或 2，见下表。 |
| `flow_pattern` | `FlowPattern::ArchimedeanChords` | 可选 `ArchimedeanChords` 或 `Monotonic`，对应原流量对话框两种顶面图案。 |

| `flow_linear` | `flow_pass` | 原版测试 |
| --- | --- | --- |
| false | 1 | Flow ratio pass 1 |
| false | 2 | Flow ratio pass 2 |
| true | 1 | Orca YOLO Flow |
| true | 2 | YOLO Perfectionist / fine |

#### 固件与输入整形

| 固件 | `shaper_type` 允许值（按原下拉顺序） | 轴行为 |
| --- | --- | --- |
| Klipper | `Default, ZV, MZV, ZVD, EI, 2HUMP_EI, 3HUMP_EI` | X/Y 可不同。 |
| RepRapFirmware | `Default, MZV, ZVD, ZVDD, ZVDDD, EI2, EI3, DAA` | 扫频时 Y 起止复制 X；阻尼测试时 Y 固定频率复制 X。 |
| Marlin 2 | `ZV` | 空类型选择 ZV。 |
| 其他固件 | `Default` | 保留原对话框入口规则；最终指令仍由原 GCodeWriter 的固件分支决定。 |

原对话框排除 `Disable`，本 API 同样不接受。Cornering 高值提示以 `calibration_warning` 返回：JD 终值大于 0.25、其他固件终值大于 20 时提示，不额外拒绝原有效范围。

### 返回对象与场景数据

`CalibrationPrepareResult` 包含 `success`、`cancelled`、`unique_ptr<CalibrationTask> task`、`vector<SliceDiagnostic> diagnostics`；`operator bool()` 等于 `success`。

| 状态 | `success` | `cancelled` | `task` |
| --- | --- | --- | --- |
| 成功 | true | false | 非空；可能同时带有提示。 |
| 取消 | false | true | 空；不要把它当作参数错误。 |
| 失败 | false | false | 空；检查 diagnostics。 |

`task.plates()` 至少包含一个盘。索引从 0 开始，PA Pattern 的全部排盘结果都在其中；不要只取第一盘。

| 数据 | 字段 | 说明 |
| --- | --- | --- |
| `CalibrationPlate` | `index` | 任务内从 0 开始的盘索引。 |
| | `config` | 原校准方法修改后的有效配置展示快照；序列化会损失浮点精度，实际切片使用任务内保留的原生配置。 |
| | `objects` | 模型实例的只读展示数据；一个原生实例对应一项。 |
| | `custom_gcodes` | 该盘原版逐层自定义指令，主要供 PA Pattern 使用；不是完整输出 G-code。 |
| `CalibrationObject` | `geometry` | 复用 `SliceObjectInput`；网格顶点为 mm，体积自身变换已烘焙进顶点；`transform` 是行主序 4×4 实例矩阵，转换到当前盘局部坐标。 |
| | `object_config` | 对象级设置的 `(key, serialized_value)` 列表。 |
| | `volume_config` | 与 `geometry.volumes` 同序的体积级设置列表。 |
| `CalibrationLayerCode` | `print_z / gcode` | 层高度 mm、该层的原始自定义代码。 |

盘局部原点为打印机热床原点；GUI 用于展示多个盘的空间偏移不会加入设备指令。上述引用只在任务存活期间有效。不能把展示网格重新交给普通 `slice()` 来替代 `slice_calibration()`，这样会丢失原生配置精度、对象设置或校准状态。

### 输出与文件所有权

| `CalibrationOutputOptions` 字段 | 默认值 | 行为 |
| --- | --- | --- |
| `output_gcode_path` | 空 | 空值生成 SDK 临时路径；非空输出到指定位置，父目录由现有导出流程创建。多盘应提供不同文件名。 |
| `generate_preview` | true | 生成内存中的 ToolpathPreview；成功后仍应检查返回指针再使用。false 不生成预览。 |
| `generate_gcode_3mf` | false | 同时生成含已切片 G-code 的 3MF。 |
| `output_gcode_3mf_path` | 空 | 非空路径也会启用 3MF 输出；启用后空路径生成 SDK 临时文件。 |

切片返回现有 `SliceResult`：`success/cancelled/diagnostics`、`output`、`gcode_3mf`、`summary`、`preview`。预览字段类型和普通切片一致。

`OutputArtifact::ownership` 为 `CallerOwned` 时保留调用方指定文件，为 `LibraryTemporary` 时属于临时产物。成功文件不会随 CalibrationTask 析构自动删除，应在不再需要时由应用的产物清理流程回收。失败 / 取消会清理由本次调用生成的 SDK 临时路径；指定输出路径按既有导出行为处理。同名输出可能覆盖已有文件，路径选择由调用方负责。

### 错误、进度与取消

诊断结构为 `{code, message, warning, option_key}`。`warning=true` 不代表调用失败，以结果的 `success/cancelled` 为准；不要依赖英文 message 的固定全文。

| code | 位置 | 含义 |
| --- | --- | --- |
| `calibration` | 准备 | 配置无效、模式或参数错误、模型加载及准备异常。 |
| `calibration_info` | 准备 | 原版 PA Pattern 默认速度、加速度通知等。 |
| `calibration_warning` | 准备 | 原版 Cornering 高值提示。 |
| `plate` | 切片 | 盘索引越界。 |
| `config / model / filament / validation / output / gcode_3mf` 等 | 切片 | 沿用普通切片诊断；参数准备成功后，仍可能因模型、热床或有效打印设置而切片失败。 |
| `initialization` | App 服务 | SlicingBackend 未成功初始化。 |

`SliceCallbacks::progress(float, string_view)` 提供 0～1 进度和阶段文本；不要把阶段文本当作稳定枚举，也不要要求跨所有阶段严格单调。`is_cancelled()` 返回 true 请求合作取消。准备在入口和盘边界检查取消，裁剪 / 排盘内部不保证立即中断；切片沿用 Print 状态回调取消机制。

SDK 回调在执行 API 的线程调用；App 服务则在切片工作线程调用。回调应保持轻量，UI 更新应投递回界面线程，取消标志可使用 `atomic_bool`。不要在回调里修改配置或重新进入切片。

### 完整 C++17 示例

保存为 `main.cpp`，参数为资源根目录和输出目录。示例使用 SDK 随附的 Flashforge AD5X 0.4 配置；接入应用时应替换为用户当前选择。

```cpp
#include <libslicer/Calibration.hpp>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: calibration_demo <resources> <output-directory>\n";
        return 1;
    }
    libslicer::LibraryOptions options;
    options.resource_directory = argv[1];
    options.vendors = {"Flashforge"};
    std::vector<libslicer::ConfigDiagnostic> errors;
    auto library = libslicer::Library::open(options, &errors);
    if (!library) {
        for (const auto& error : errors) std::cerr << error.message << '\n';
        return 1;
    }
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X";
    selection.machine_variant_id = "0.4";
    auto configured = library->create_config(selection);
    if (!configured) return 1;

    libslicer::CalibrationRequest request;
    request.config = configured.config->snapshot();
    request.mode = libslicer::CalibMode::Calib_PA_Line;
    request.start = 0;
    request.end = 0.07;
    request.step = 0.01;

    std::atomic_bool cancel{false}; // 实际程序由取消按钮设置。
    libslicer::SliceCallbacks callbacks;
    callbacks.is_cancelled = [&] { return cancel.load(); };
    callbacks.progress = [](float p, std::string_view stage) {
        std::cout << p << " " << stage << '\n';
    };
    auto prepared = library->prepare_calibration(request, callbacks);
    for (const auto& d : prepared.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    if (prepared.cancelled) return 2;
    if (!prepared) return 1;

    for (const auto& plate : prepared.task->plates()) {
        libslicer::CalibrationOutputOptions output;
        const auto stem = "pa-" + std::to_string(plate.index);
        output.output_gcode_path = (std::filesystem::path(argv[2]) / (stem + ".gcode")).string();
        output.generate_preview = true;
        output.output_gcode_3mf_path = (std::filesystem::path(argv[2]) / (stem + ".gcode.3mf")).string();
        auto sliced = library->slice_calibration(*prepared.task, plate.index, output, callbacks);
        for (const auto& d : sliced.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
        if (sliced.cancelled) return 2;
        if (!sliced) return 1;
        std::cout << sliced.output.path << '\n';
        if (sliced.preview) std::cout << sliced.preview->segments.size() << " preview segments\n";
    }
    return 0;
}
```

切换模式时使用新的默认构造请求，避免继承上一个模式的无关参数。例如：

```cpp
libslicer::CalibrationRequest flow;
flow.config = configured.config->snapshot();
flow.mode = libslicer::CalibMode::Calib_Flow_Rate;
flow.flow_linear = true;
flow.flow_pass = 2;
flow.flow_pattern = libslicer::CalibrationRequest::FlowPattern::Monotonic;

libslicer::CalibrationRequest shaping;
shaping.config = configured.config->snapshot(); // 配置中的固件决定支持的类型。
shaping.mode = libslicer::CalibMode::Calib_Input_shaping_freq;
shaping.start = 0.1;
shaping.freqStartX = 20;
shaping.freqEndX = 80;
shaping.freqStartY = 30;
shaping.freqEndY = 90;
shaping.shaper_type = "MZV"; // 适用于 Klipper / RRF；Marlin 2 使用 ZV。
shaping.test_model = 1;
```

### App 的 SlicingBackend 包装

```cpp
std::vector<libslicer::CalibMode> calibrationModes() const;
libslicer::CalibrationPrepareResult prepareCalibration(
    const libslicer::CalibrationRequest&, const libslicer::SliceCallbacks& = {}) const;
libslicer::SliceResult sliceCalibration(
    const libslicer::CalibrationTask&, std::size_t plateIndex,
    const libslicer::CalibrationOutputOptions& = {}, const libslicer::SliceCallbacks& = {}) const;
```

通过 `GPlatform::SlicingBackend::instance()` 调用。其行为与 SDK 相同，另有以下约定：

- SDK 的 `request.config` 必须有效；App 包装允许不填，此时在同一工作队列内读取活动配置。没有活动配置仍会失败。
- 所有调用在现有单切片工作线程执行；包装方法同步等待结果。界面应通过应用已有任务调度发起，避免直接阻塞 GUI 事件处理。
- 准备和切片参与原有长任务计数，在执行期间阻止配置修改；用户取消与应用关闭取消会合并。
- Library / 任务 / 配置访问应统一串行调度，不要并发操作同一个任务。底层仍有全局资源路径和 locale 状态，SDK 没有承诺多个 Library 并行安全。建议在同一 Library / 服务实例上完成任务生命周期。
- 预览发布仍使用现有 `SliceResult` 流程。用户最终选定参数后，用现有配置 setter 明确更新活动配置；预设持久化与设备参数保存由对应服务承接，本接口不提供这两个动作。

### 验证入口

`orca_plater_tests` 检查有效配置、实例矩阵、网格和逐条可执行 G-code；`OrcaPlaterSource` 检查原始副本、底层算法及 GUI 访问替换白名单。精简前后的 19 个逐盘输出基线可通过测试环境变量比较：

```text
LIBSLICER_CALIBRATION_WRITE_BASELINE=<directory>   # 用精简前版本保存
LIBSLICER_CALIBRATION_COMPARE_BASELINE=<directory> # 用当前版本逐条比较
orca_plater_tests "[orca_plater][slice]" --order lex
orca_plater_tests "[orca_plater]~[slice]"            # 独立验证参数与场景
```

基线录制与回放必须使用相同的测试集合和顺序；在 Windows 上，还应在加载 DLL 前限制测试进程的 CPU 亲和性，例如在 cmd 中运行 `start "" /b /wait /affinity 1 orca_plater_tests.exe "[orca_plater][slice]" --order lex`。仅在测试程序内部设置线程数不能约束另一份静态链接的 TBB。不同测试顺序的跨进程输出不能直接作为同一初始化条件的基线。

以上变量只影响测试程序，不是产品配置。迁移验证中还用 `LIBSLICER_CALIBRATION_LEGACY_ENUM` 让测试桥接保留的旧 DLL 的开发版枚举序号；它不参与产品执行，也不构成旧 API 别名。完整 wxWidgets 校准界面、设备自动校准及真实打印测量没有在这些测试中执行。

本轮实测：同条件下 19 个盘的旧、新 DLL 指令全部一致（1252514 条断言）；独立场景及参数接口 93 条断言、原预设与耗材回归 104 条断言全部通过。本文完整 C++17 示例已从文档提取、编译运行，生成 G-code、G-code 3MF 和 699 个预览线段。App Release 构建通过。

### 实现来源与精简边界

原始 GUI 参考版本固定为 `71c30dd1feb73efa40d824ee1e4be05fce7b0827`。`src/libslicer_api/orca/PlaterCalibration.original.inc` 保存原始校准函数，不参与编译；实际编译的 `PlaterCalibration.inc` 仅移除界面展示调用，并把预设、Print 和场景访问替换为 Plater 成员访问。`tests/libslicer_api/check_plater_source.py` 逐项检查 GUI 替换白名单，原生 `calib.cpp / calib.hpp` 保持原始校验值。

内部顶层类型由 18 个收敛为 `Plater / Selection / PartPlate / PartPlateList` 四个。模型导入、共同中心缩放、落床、裁剪及分盘保留；空的 App、MainFrame、Tab、Sidebar、Canvas 等 GUI 兼容类型已删除。这些内部类型实现校准子集，不是完整 wxWidgets GUI。公共枚举与 Orca 的 0～12 原值逐项通过编译期断言，SDK 请求、任务和快照没有直接同名的 Orca 类型，保留各自包装名称。
