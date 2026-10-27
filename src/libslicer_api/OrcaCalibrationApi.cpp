#include "OrcaCalibrationInternal.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <iterator>

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
} // namespace libslicer
