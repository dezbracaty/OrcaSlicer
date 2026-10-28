#include "OrcaCalibrationInternal.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <iterator>
#include <locale>
#include <sstream>

namespace libslicer {
// Keep public names and ordinals aligned with the original core enum.
static_assert(static_cast<int>(CalibMode::Calib_None) == static_cast<int>(Slic3r::CalibMode::Calib_None));
static_assert(static_cast<int>(CalibMode::Calib_PA_Line) == static_cast<int>(Slic3r::CalibMode::Calib_PA_Line));
static_assert(static_cast<int>(CalibMode::Calib_PA_Pattern) == static_cast<int>(Slic3r::CalibMode::Calib_PA_Pattern));
static_assert(static_cast<int>(CalibMode::Calib_PA_Tower) == static_cast<int>(Slic3r::CalibMode::Calib_PA_Tower));
static_assert(static_cast<int>(CalibMode::Calib_Auto_PA_Line) == static_cast<int>(Slic3r::CalibMode::Calib_Auto_PA_Line));
static_assert(static_cast<int>(CalibMode::Calib_Flow_Rate) == static_cast<int>(Slic3r::CalibMode::Calib_Flow_Rate));
static_assert(static_cast<int>(CalibMode::Calib_Temp_Tower) == static_cast<int>(Slic3r::CalibMode::Calib_Temp_Tower));
static_assert(static_cast<int>(CalibMode::Calib_Vol_speed_Tower) == static_cast<int>(Slic3r::CalibMode::Calib_Vol_speed_Tower));
static_assert(static_cast<int>(CalibMode::Calib_VFA_Tower) == static_cast<int>(Slic3r::CalibMode::Calib_VFA_Tower));
static_assert(static_cast<int>(CalibMode::Calib_Retraction_tower) == static_cast<int>(Slic3r::CalibMode::Calib_Retraction_tower));
static_assert(static_cast<int>(CalibMode::Calib_Input_shaping_freq) == static_cast<int>(Slic3r::CalibMode::Calib_Input_shaping_freq));
static_assert(static_cast<int>(CalibMode::Calib_Input_shaping_damp) == static_cast<int>(Slic3r::CalibMode::Calib_Input_shaping_damp));
static_assert(static_cast<int>(CalibMode::Calib_Cornering) == static_cast<int>(Slic3r::CalibMode::Calib_Cornering));
CalibrationTask::CalibrationTask() : impl_(std::make_unique<Impl>()) {}
CalibrationTask::~CalibrationTask() = default;
const std::vector<CalibrationPlate>& CalibrationTask::plates() const noexcept { return impl_->plates; }
const CalibrationRequest& CalibrationTask::request() const noexcept { return impl_->request; }

namespace orca {
using namespace Slic3r;
static void check(bool valid, const char* message)
{
    if (!valid)
        throw std::invalid_argument(message);
}

// Tab.cpp::input_shaper_types_for_flavor at the same revision as Plater.
static std::vector<InputShaperType> input_shaper_types_for_flavor(GCodeFlavor flavor)
{
    switch (flavor) {
    case gcfKlipper:
        return {InputShaperType::Default, InputShaperType::ZV,        InputShaperType::MZV,         InputShaperType::ZVD,
                InputShaperType::EI,      InputShaperType::TwoHumpEI, InputShaperType::ThreeHumpEI, InputShaperType::Disable};
    case gcfRepRapFirmware:
        return {InputShaperType::Default, InputShaperType::MZV, InputShaperType::ZVD, InputShaperType::ZVDD,   InputShaperType::ZVDDD,
                InputShaperType::EI2,     InputShaperType::EI3, InputShaperType::DAA, InputShaperType::Disable};
    case gcfMarlinFirmware: return {InputShaperType::ZV, InputShaperType::Disable};
    default: return {InputShaperType::Default, InputShaperType::Disable};
    }
}
Calib_Params dialog_parameters(const CalibrationRequest& request, const DynamicPrintConfig& config)
{
    Calib_Params p{};
    p.extruder_id   = request.extruder_id;
    p.start         = request.start;
    p.end           = request.end;
    p.step          = request.step;
    p.print_numbers = request.print_numbers;
    p.freqStartX    = request.freqStartX;
    p.freqEndX      = request.freqEndX;
    p.freqStartY    = request.freqStartY;
    p.freqEndY      = request.freqEndY;
    p.test_model    = request.test_model;
    p.shaper_type   = request.shaper_type;
    std::copy_if(request.accelerations.begin(), request.accelerations.end(), std::back_inserter(p.accelerations),
                 [](int value) { return value > 0; });
    std::copy_if(request.speeds.begin(), request.speeds.end(), std::back_inserter(p.speeds), [](int value) { return value > 0; });
    // The transport receives parsed numeric values. Reject nonnumeric payloads
    // before calling the source methods, as the original ToDouble/ToULong do.
    for (double value : {p.start, p.end, p.step, p.freqStartX, p.freqEndX, p.freqStartY, p.freqEndY})
        check(std::isfinite(value), "Calibration input is not a number");
    const auto flavor = config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    switch (request.mode) {
    case libslicer::CalibMode::Calib_PA_Line:
    case libslicer::CalibMode::Calib_PA_Pattern:
    case libslicer::CalibMode::Calib_PA_Tower:
        // calib_dlg.cpp: PA_Calibration_Dlg::on_start, including the original
        // EPSILON-based minimum step and acceleration/speed input guard.
        check(!(p.start < 0 || p.step < 10 * EPSILON || p.end < p.start + p.step), "Invalid pressure advance range");
        if (!p.speeds.empty() && !p.accelerations.empty())
            check(*std::min_element(p.accelerations.begin(), p.accelerations.end()) > *std::max_element(p.speeds.begin(), p.speeds.end()),
                  "Acceleration values must be greater than speed values");
        p.mode = request.mode == libslicer::CalibMode::Calib_PA_Line    ? Slic3r::CalibMode::Calib_PA_Line :
                 request.mode == libslicer::CalibMode::Calib_PA_Pattern ? Slic3r::CalibMode::Calib_PA_Pattern :
                                                                          Slic3r::CalibMode::Calib_PA_Tower;
        break;
    case libslicer::CalibMode::Calib_Temp_Tower:
        check(p.start >= 0 && p.end >= 0 && std::floor(p.start) == p.start && std::floor(p.end) == p.end,
              "Temperature input must be unsigned integers");
        check(!(p.start > 500 || p.end < 155 || p.end > p.start - 5), "Invalid temperature range");
        p.mode = Slic3r::CalibMode::Calib_Temp_Tower;
        break;
    case libslicer::CalibMode::Calib_Vol_speed_Tower:
        check(!(p.start <= 0 || p.step <= 0 || p.end < p.start + p.step), "Invalid volumetric speed range");
        p.mode = Slic3r::CalibMode::Calib_Vol_speed_Tower;
        break;
    case libslicer::CalibMode::Calib_VFA_Tower:
        check(!(p.start <= 10 || p.step <= 0 || p.end < p.start + p.step), "Invalid VFA range");
        p.mode = Slic3r::CalibMode::Calib_VFA_Tower;
        break;
    case libslicer::CalibMode::Calib_Retraction_tower:
        check(!(p.start < 0 || p.step <= 0 || p.end < p.start + p.step), "Invalid retraction range");
        p.mode = Slic3r::CalibMode::Calib_Retraction_tower;
        break;
    case libslicer::CalibMode::Calib_Input_shaping_freq:
        if (flavor == gcfRepRapFirmware) {
            p.freqStartY = p.freqStartX;
            p.freqEndY   = p.freqEndX;
        }
        check(!(p.freqStartX < 0 || p.freqEndX > 500 || p.freqStartY < 0 || p.freqEndY > 500 || p.freqStartX >= p.freqEndX ||
                p.freqStartY >= p.freqEndY),
              "Invalid input shaping frequency range");
        check(!(p.start < 0 || p.start >= 1), "Invalid damping factor");
        p.mode = Slic3r::CalibMode::Calib_Input_shaping_freq;
        break;
    case libslicer::CalibMode::Calib_Input_shaping_damp:
        if (flavor == gcfRepRapFirmware)
            p.freqStartY = p.freqStartX;
        check(!(p.freqStartX < 0 || p.freqStartX > 500 || p.freqStartY < 0 || p.freqStartY > 500), "Invalid input shaping frequency");
        check(!(p.start < 0 || p.end > 1 || p.start >= p.end), "Invalid damping range");
        p.mode = Slic3r::CalibMode::Calib_Input_shaping_damp;
        break;
    case libslicer::CalibMode::Calib_Cornering: {
        check(p.test_model >= 0 && p.test_model <= 2, "Unknown cornering model");
        const auto* jd       = config.option<ConfigOptionFloats>("machine_max_junction_deviation");
        const double max_end = flavor == gcfMarlinFirmware && jd && !jd->values.empty() && jd->values.front() > 0 ? .3 : 100.;
        check(!(p.start < 0 || p.end > max_end || p.start >= p.end), "Invalid cornering range");
        p.mode = Slic3r::CalibMode::Calib_Cornering;
        break;
    }
    case libslicer::CalibMode::Calib_Flow_Rate:
        check(request.flow_pass == 1 || request.flow_pass == 2, "Flow pass must be 1 or 2");
        check(request.flow_pattern == CalibrationRequest::FlowPattern::ArchimedeanChords ||
                  request.flow_pattern == CalibrationRequest::FlowPattern::Monotonic,
              "Unknown flow pattern");
        p.mode = Slic3r::CalibMode::Calib_None;
        break;
    case libslicer::CalibMode::Calib_Tolerance_Model: p.mode = Slic3r::CalibMode::Calib_None; break;
    default: throw std::invalid_argument("Choose an original calibration entry");
    }
    if (p.mode == Slic3r::CalibMode::Calib_Input_shaping_freq || p.mode == Slic3r::CalibMode::Calib_Input_shaping_damp) {
        check(p.test_model == 0 || p.test_model == 1, "Unknown input shaping model");
        const auto allowed = input_shaper_types_for_flavor(flavor);
        const auto* def    = print_config_def.get("input_shaping_type");
        if (p.shaper_type.empty())
            p.shaper_type = def->enum_values.at(static_cast<std::size_t>(allowed.front()));
        check(std::any_of(allowed.begin(), allowed.end(),
                          [&](auto type) {
                              return type != InputShaperType::Disable &&
                                     def->enum_values.at(static_cast<std::size_t>(type)) == p.shaper_type;
                          }),
              "Unknown calibration shaper type");
    }
    return p;
}

std::vector<SliceDiagnostic> calibration_warnings(const CalibrationRequest& request, const DynamicPrintConfig& config)
{
    if (request.mode == libslicer::CalibMode::Calib_Cornering) {
        const auto* jd = config.option<ConfigOptionFloats>("machine_max_junction_deviation");
        const bool use_jd = config.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value == gcfMarlinFirmware &&
                            jd && !jd->values.empty() && jd->values.front() > 0;
        if (request.end > (use_jd ? .25 : 20.))
            return {{"calibration_warning", "High cornering values may cause layer shifts", true}};
    }
    return {};
}

CalibrationPlate describe_plate(const Model& model, std::size_t index)
{
    CalibrationPlate plate;
    plate.index = index;
    for (const auto* object : model.objects) {
        for (const auto* instance : object->instances) {
            CalibrationObject result;
            result.geometry.name = object->name;
            const auto matrix    = instance->get_matrix();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    result.geometry.transform[r * 4 + c] = matrix(r, c);
            for (const auto& key : object->config.keys())
                result.object_config.emplace_back(key, object->config.get().option(key)->serialize());
            for (const auto* volume : object->volumes) {
                std::vector<std::pair<std::string, std::string>> settings;
                for (const auto& key : volume->config.keys())
                    settings.emplace_back(key, volume->config.get().option(key)->serialize());
                result.volume_config.push_back(std::move(settings));
                auto mesh = volume->mesh();
                mesh.transform(volume->get_matrix());
                SliceVolumeInput geometry;
                geometry.default_filament_slot = volume->extruder_id();
                if (volume->is_support_enforcer())
                    geometry.role = SliceVolumeRole::SupportEnforcer;
                else if (volume->is_support_blocker())
                    geometry.role = SliceVolumeRole::SupportBlocker;
                for (const auto& v : mesh.its.vertices)
                    geometry.vertices.push_back({v.x(), v.y(), v.z()});
                for (const auto& f : mesh.its.indices)
                    geometry.triangles.push_back(
                        {static_cast<std::uint32_t>(f.x()), static_cast<std::uint32_t>(f.y()), static_cast<std::uint32_t>(f.z())});
                result.geometry.volumes.push_back(std::move(geometry));
            }
            plate.objects.push_back(std::move(result));
        }
    }
    const auto code = model.plates_custom_gcodes.find(0);
    if (code != model.plates_custom_gcodes.end())
        for (const auto& layer : code->second.gcodes)
            plate.custom_gcodes.push_back({layer.print_z, layer.extra});
    return plate;
}
} // namespace orca

namespace {
using Slic3r::DynamicPrintConfig;
using Slic3r::GCodeFlavor;

DynamicPrintConfig parameter_config(const ConfigSnapshot& snapshot)
{
    if (!snapshot.valid()) throw std::invalid_argument("Calibration has no configuration");
    DynamicPrintConfig config;
    // Form behavior only depends on these two printer settings. It never
    // consults a process-wide active configuration or a GUI singleton.
    for (const char* key : {"gcode_flavor", "machine_max_junction_deviation"})
        config.set_deserialize_strict(key, snapshot.value(key).value_or(Slic3r::FullPrintConfig::defaults().opt_serialize(key)));
    return config;
}
bool is_pa(CalibMode mode)
{
    return mode == CalibMode::Calib_PA_Line || mode == CalibMode::Calib_PA_Pattern || mode == CalibMode::Calib_PA_Tower;
}
SettingItem& field(CalibrationParameters& p, std::string_view key)
{
    const auto it = std::find_if(p.fields.begin(), p.fields.end(), [&](const auto& f) { return f.key == key; });
    if (it == p.fields.end()) throw std::invalid_argument("Unknown calibration parameter: " + std::string(key));
    return *it;
}
const SettingItem& field(const CalibrationParameters& p, std::string_view key)
{
    const auto it = std::find_if(p.fields.begin(), p.fields.end(), [&](const auto& f) { return f.key == key; });
    if (it == p.fields.end()) throw std::invalid_argument("Unknown calibration parameter: " + std::string(key));
    return *it;
}
template<class T> bool parse_number(const std::string& text, T& value)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    if (!(in >> value)) return false;
    in >> std::ws;
    return in.eof();
}
int choice(const CalibrationParameters& p, const char* key)
{
    int n = -1;
    return parse_number(field(p, key).value, n) ? n : -1;
}
void reset_pa(CalibrationParameters& p)
{
    const int method = choice(p, "pa_method"), extruder = choice(p, "extruder_type");
    if (method < 0 || method > 2 || extruder < 0 || extruder > 1) return;
    p.mode = method == 0 ? CalibMode::Calib_PA_Tower : method == 1 ? CalibMode::Calib_PA_Line : CalibMode::Calib_PA_Pattern;
    field(p, "start").value = "0";
    field(p, "end").value = extruder == 1 ? "1.0" : method == 2 ? "0.08" : "0.1";
    field(p, "step").value = extruder == 1 ? (method == 2 ? "0.05" : "0.02") : (method == 2 ? "0.005" : "0.002");
    field(p, "print_numbers").value = method == 0 ? "0" : "1";
}
void refresh_parameters(CalibrationParameters& p)
{
    const auto config = parameter_config(p.config);
    const auto flavor = config.option<Slic3r::ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    const auto* jd = config.option<Slic3r::ConfigOptionFloats>("machine_max_junction_deviation");
    const bool junction = flavor == Slic3r::gcfMarlinFirmware && jd && !jd->values.empty() && jd->values.front() > 0;
    const bool pa = is_pa(p.mode), flow = p.mode == CalibMode::Calib_Flow_Rate;
    const bool temp = p.mode == CalibMode::Calib_Temp_Tower, freq = p.mode == CalibMode::Calib_Input_shaping_freq;
    const bool damp = p.mode == CalibMode::Calib_Input_shaping_damp, corner = p.mode == CalibMode::Calib_Cornering;
    const bool shaping = freq || damp, rrf = flavor == Slic3r::gcfRepRapFirmware;
    for (auto& f : p.fields) { f.visible = false; f.enabled = true; f.read_only = false; }
    const auto show = [&](const char* key, bool enabled = true) {
        auto& f = field(p, key); f.visible = true; f.enabled = enabled; f.read_only = !enabled;
    };
    if (pa) {
        for (const char* key : {"extruder_type", "pa_method", "print_numbers"}) show(key);
        auto& print = field(p, "print_numbers");
        print.enabled = p.mode == CalibMode::Calib_PA_Line; print.read_only = !print.enabled;
        if (!print.enabled) print.value = p.mode == CalibMode::Calib_PA_Pattern ? "1" : "0";
        if (p.mode == CalibMode::Calib_PA_Pattern) { show("accelerations"); show("speeds"); }
    }
    if (flow) { show("flow_type"); show("flow_pattern"); }
    if (temp) { show("filament_type"); field(p, "step").value = "5"; }
    if (shaping || corner) show("test_model");
    auto& models = field(p, "test_model").enum_items;
    models = {{"0", "Ringing Tower"}, {"1", "Fast Tower"}};
    if (corner) models.push_back({"2", "SCV-V2"});
    if (shaping) {
        show("shaper_type");
        auto& shapers = field(p, "shaper_type").enum_items;
        shapers.clear();
        const auto* def = Slic3r::print_config_def.get("input_shaping_type");
        for (const auto type : orca::input_shaper_types_for_flavor(flavor))
            if (type != Slic3r::InputShaperType::Disable) {
                const auto& v = def->enum_values.at(static_cast<std::size_t>(type));
                shapers.push_back({v, v});
            }
        show("freqStartX"); show("freqStartY", !rrf);
        if (freq) { show("freqEndX"); show("freqEndY", !rrf); }
        field(p, "freqStartX").label = freq ? "X start frequency" : "X fixed frequency";
        field(p, "freqStartY").label = freq ? "Y start frequency" : "Y fixed frequency";
        if (rrf) {
            field(p, "freqStartY").value = field(p, "freqStartX").value;
            if (freq) field(p, "freqEndY").value = field(p, "freqEndX").value;
        }
    }
    if (!flow && p.mode != CalibMode::Calib_Tolerance_Model) {
        show("start"); if (!freq) show("end");
        if (!shaping && !corner) show("step", !temp);
    }
    const std::string unit = temp ? "℃" : p.mode == CalibMode::Calib_Vol_speed_Tower ? "mm³/s" :
        p.mode == CalibMode::Calib_Retraction_tower || (corner && junction) ? "mm" :
        p.mode == CalibMode::Calib_VFA_Tower || corner ? "mm/s" : "";
    for (const char* key : {"start", "end", "step"}) field(p, key).unit = unit;
    field(p, "start").label = pa ? "Start PA" : freq ? "Fixed damping" : damp ? "Start damping" : "Start value";
    field(p, "end").label = pa ? "End PA" : damp ? "End damping" : "End value";
    field(p, "step").label = pa ? "PA step" : "Step";
    field(p, "start").type = temp ? SettingType::Integer : SettingType::Float;
    field(p, "end").type = temp ? SettingType::Integer : SettingType::Float;
    field(p, "start").minimum = p.mode == CalibMode::Calib_VFA_Tower ? 10 : p.mode == CalibMode::Calib_Vol_speed_Tower ? 0 : temp ? 155 : 0;
    field(p, "start").maximum = temp ? std::optional<double>(500) : shaping ? std::optional<double>(1) : std::nullopt;
    field(p, "end").minimum = temp ? 155 : 0;
    field(p, "end").maximum = temp ? std::optional<double>(500) : damp ? std::optional<double>(1) :
        corner ? std::optional<double>(junction ? .3 : 100) : std::nullopt;
    field(p, "step").minimum = pa ? 10 * EPSILON : 0;
    p.can_refine = flow && (choice(p, "flow_type") == 0 || choice(p, "flow_type") == 2);
}
}

CalibrationParameters calibration_parameters(CalibMode mode, const ConfigSnapshot& config,
    const std::vector<std::pair<std::string, std::string>>& values)
{
    // Original dialog defaults, not CalibrationRequest's transport zero values.
    switch (mode) {
    case CalibMode::Calib_PA_Line: case CalibMode::Calib_PA_Pattern: case CalibMode::Calib_PA_Tower:
    case CalibMode::Calib_Flow_Rate: case CalibMode::Calib_Temp_Tower: case CalibMode::Calib_Vol_speed_Tower:
    case CalibMode::Calib_VFA_Tower: case CalibMode::Calib_Retraction_tower: case CalibMode::Calib_Input_shaping_freq:
    case CalibMode::Calib_Input_shaping_damp: case CalibMode::Calib_Cornering: case CalibMode::Calib_Tolerance_Model: break;
    default: throw std::invalid_argument("Choose an original calibration entry");
    }
    CalibrationParameters p;
    p.mode = mode; p.config = config;
    const auto add = [&](const char* key, const char* v, const char* label, SettingType type, const char* unit = "") {
        SettingItem f; f.key = key; f.value = v; f.label = label; f.type = type; f.unit = unit; p.fields.push_back(std::move(f));
    };
    const auto enumeration = [&](const char* key, const char* v, const char* label, std::vector<EnumItem> choices) {
        add(key, v, label, SettingType::Enum); p.fields.back().enum_items = std::move(choices);
    };
    enumeration("extruder_type", "0", "Extruder type", {{"0", "DDE"}, {"1", "Bowden"}});
    enumeration("pa_method", mode == CalibMode::Calib_PA_Line ? "1" : mode == CalibMode::Calib_PA_Pattern ? "2" : "0", "Method",
        {{"0", "PA Tower"}, {"1", "PA Line"}, {"2", "PA Pattern"}});
    enumeration("flow_type", "2", "Calibration type", {{"0", "Pass 1"}, {"1", "Pass 2"}, {"2", "YOLO (Recommended)"}, {"3", "YOLO (Perfectionist)"}});
    enumeration("flow_pattern", "0", "Top surface pattern", {{"0", "Archimedean Chords"}, {"1", "Monotonic"}});
    enumeration("filament_type", "0", "Material temperature range", {{"0", "PLA"}, {"1", "ABS/ASA"}, {"2", "PETG"}, {"3", "PCTG"}, {"4", "TPU"}, {"5", "PA-CF"}, {"6", "PET-CF"}, {"7", "Custom"}});
    enumeration("test_model", "0", "Test model", {});
    enumeration("shaper_type", "", "Shaper type", {});
    for (const auto& spec : std::vector<std::pair<const char*, const char*>>{
        {"freqStartX", "X start frequency"}, {"freqEndX", "X end frequency"}, {"freqStartY", "Y start frequency"}, {"freqEndY", "Y end frequency"}}) {
        add(spec.first, "0", spec.second, SettingType::Float, "Hz");
        p.fields.back().minimum = 0; p.fields.back().maximum = 500;
    }
    add("start", "0", "Start value", SettingType::Float);
    add("end", "0", "End value", SettingType::Float);
    add("step", "0", "Step", SettingType::Float);
    add("accelerations", "", "Accelerations", SettingType::String, "mm/s²");
    add("speeds", "", "Speeds", SettingType::String, "mm/s");
    add("print_numbers", "1", "Print numbers", SettingType::Boolean);
    if (is_pa(mode)) reset_pa(p);
    const auto range = [&](const char* start, const char* end, const char* step) {
        field(p, "start").value = start; field(p, "end").value = end; field(p, "step").value = step;
    };
    if (mode == CalibMode::Calib_Temp_Tower) range("230", "190", "5");
    if (mode == CalibMode::Calib_Vol_speed_Tower) range("5", "20", "0.5");
    if (mode == CalibMode::Calib_VFA_Tower) range("40", "200", "10");
    if (mode == CalibMode::Calib_Retraction_tower) range("0", "2", "0.1");
    if (mode == CalibMode::Calib_Input_shaping_freq || mode == CalibMode::Calib_Input_shaping_damp) {
        const bool freq = mode == CalibMode::Calib_Input_shaping_freq;
        for (const char* key : {"freqStartX", "freqStartY"}) field(p, key).value = freq ? "15" : "30";
        if (freq) for (const char* key : {"freqEndX", "freqEndY"}) field(p, key).value = "110";
        field(p, "start").value = freq ? "0.15" : "0.00";
        if (!freq) field(p, "end").value = "0.40";
    }
    refresh_parameters(p);
    if (mode == CalibMode::Calib_Cornering) {
        const bool junction = field(p, "start").unit == "mm";
        range(junction ? "0.000" : "1.000", junction ? "0.250" : "15.000", "0");
    }
    const auto& shapers = field(p, "shaper_type").enum_items;
    if (!shapers.empty()) field(p, "shaper_type").value = shapers.front().value;
    for (auto& f : p.fields) f.default_value = f.value;
    for (const auto& [key, v] : values) field(p, key).value = v;
    if (is_pa(mode)) {
        const auto method = choice(p, "pa_method");
        if (method >= 0 && method <= 2)
            p.mode = method == 0 ? CalibMode::Calib_PA_Tower : method == 1 ? CalibMode::Calib_PA_Line : CalibMode::Calib_PA_Pattern;
    }
    refresh_parameters(p);
    return p;
}

CalibrationParameters update_calibration_parameters(CalibrationParameters p, std::string_view key, std::string_view value, bool focus_out)
{
    p.diagnostics.clear();
    if (key == "next_pass") {
        if (!p.can_refine) throw std::invalid_argument("This flow pass has no refinement pass");
        field(p, "flow_type").value = choice(p, "flow_type") == 0 ? "1" : "3";
    } else {
        auto& f = field(p, key);
        const bool changed = f.value != value;
        f.value = std::string(value);
        if (changed && is_pa(p.mode) && (key == "extruder_type" || key == "pa_method")) reset_pa(p);
        if (changed && p.mode == CalibMode::Calib_Temp_Tower && key == "filament_type") {
            static constexpr int ranges[][2] = {{230,190},{270,230},{250,230},{280,240},{240,210},{320,280},{320,280},{230,190}};
            const int index = choice(p, "filament_type");
            if (index >= 0 && index < 8) {
                field(p, "start").value = std::to_string(ranges[index][0]);
                field(p, "end").value = std::to_string(ranges[index][1]);
            }
        }
        if (focus_out && p.mode == CalibMode::Calib_Temp_Tower && (key == "start" || key == "end")) {
            unsigned long temperature = 0;
            // ToULong rejects a minus sign. Do not alter incomplete/invalid text.
            if (f.value.find('-') == std::string::npos && parse_number(f.value, temperature)) {
                if (temperature > 500 || temperature < 155) {
                    p.diagnostics.push_back({"temperature_range", "Supported temperature range: 155-500 C", true});
                    temperature = std::clamp(temperature, 155ul, 500ul);
                }
                f.value = std::to_string((temperature / 5) * 5);
            }
        }
    }
    refresh_parameters(p);
    return p;
}

CalibrationValidationResult validate_calibration_parameters(const CalibrationParameters& p)
{
    CalibrationValidationResult result;
    std::string option_key;
    try {
        auto& r = result.request; r.mode = p.mode; r.config = p.config;
        for (const auto& f : p.fields) {
            if (!f.visible || f.type != SettingType::Enum) continue;
            if (std::none_of(f.enum_items.begin(), f.enum_items.end(), [&](const auto& v) { return v.value == f.value; })) {
                option_key = f.key;
                throw std::invalid_argument("Unknown calibration choice: " + f.label);
            }
        }
        const auto number = [&](const char* key, double& target) {
            option_key = key;
            if (!parse_number(field(p, key).value, target) || !std::isfinite(target))
                throw std::invalid_argument("Invalid numeric calibration input: " + field(p, key).label);
        };
        number("start", r.start); number("end", r.end); number("step", r.step);
        number("freqStartX", r.freqStartX); number("freqEndX", r.freqEndX);
        number("freqStartY", r.freqStartY); number("freqEndY", r.freqEndY);
        if (p.mode == CalibMode::Calib_Temp_Tower) {
            for (const char* key : {"start", "end"}) {
                option_key = key;
                unsigned long n = 0;
                const auto& text = field(p, key).value;
                if (text.find('-') != std::string::npos || !parse_number(text, n))
                    throw std::invalid_argument("Temperature input must be unsigned integers");
            }
        }
        const auto& print = field(p, "print_numbers").value;
        option_key = "print_numbers";
        if (print != "0" && print != "1") throw std::invalid_argument("Invalid print numbers flag");
        r.print_numbers = print == "1";
        r.test_model = choice(p, "test_model"); r.shaper_type = field(p, "shaper_type").value;
        const auto batch = [&](const char* key, std::vector<int>& out) {
            option_key = key;
            auto text = field(p, key).value;
            if (text.find_first_not_of("0123456789,") != std::string::npos)
                throw std::invalid_argument("Expected comma-separated positive integers: " + field(p, key).label);
            std::replace(text.begin(), text.end(), ',', ' ');
            std::istringstream in(text); in.imbue(std::locale::classic());
            // Original ParseStringValues: an int stream stops at an overflow,
            // retaining already parsed positive values rather than inventing
            // another rejection rule or sampling limit.
            std::copy_if(std::istream_iterator<int>(in), std::istream_iterator<int>(), std::back_inserter(out),
                         [](int n) { return n > 0; });
        };
        batch("accelerations", r.accelerations); batch("speeds", r.speeds);
        const int flow = choice(p, "flow_type");
        r.flow_linear = flow >= 2; r.flow_pass = flow % 2 + 1;
        r.flow_pattern = choice(p, "flow_pattern") == 1 ? CalibrationRequest::FlowPattern::Monotonic : CalibrationRequest::FlowPattern::ArchimedeanChords;
        const auto config = parameter_config(p.config);
        option_key.clear();
        // Keep all mode-specific bounds and firmware rules in the existing
        // source adapter used by Library::prepare_calibration.
        const auto native = orca::dialog_parameters(r, config);
        r.freqStartY = native.freqStartY; r.freqEndY = native.freqEndY; r.shaper_type = native.shaper_type;
        result.diagnostics = orca::calibration_warnings(r, config);
        result.success = true;
    } catch (const std::exception& e) {
        result.diagnostics.push_back({"calibration", e.what(), false, option_key});
    }
    return result;
}
} // namespace libslicer
