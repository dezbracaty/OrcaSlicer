#pragma once

#include <libslicer/Calibration.hpp>
#include "Plater.hpp"
#include "libslic3r/ContinuousFiber/FiberCalibrationPlan.hpp"

namespace libslicer {
class CalibrationTask::Impl
{
public:
    CalibrationRequest request;
    std::unique_ptr<orca::Plater> host;
    std::vector<Slic3r::Model> models;
    std::vector<CalibrationPlate> plates;
    Slic3r::Calib_Params params;
    std::optional<orca::FlowCalibrationTarget> flow_target;
    std::optional<Slic3r::DynamicPrintConfig> fiber_config;
    std::shared_ptr<const Slic3r::FiberCalibrationPlan> fiber_plan;
};
namespace fiber_calibration {
CalibrationParameters parameters(CalibMode, const ConfigSnapshot&,
    const std::vector<std::pair<std::string, std::string>>&);
CalibrationValidationResult validate(const CalibrationParameters&);
std::shared_ptr<const Slic3r::FiberCalibrationPlan> prepare(const CalibrationRequest&,
    Slic3r::DynamicPrintConfig&, Slic3r::Model&, std::vector<FiberCalibrationSample>&);
std::vector<std::uint64_t> read_executions(const std::string&, std::size_t,
    const std::function<void()>& check_cancelled);
}
namespace orca {
Slic3r::Calib_Params dialog_parameters(const CalibrationRequest& request, const Slic3r::DynamicPrintConfig& config);
std::vector<SliceDiagnostic> calibration_warnings(const CalibrationRequest& request, const Slic3r::DynamicPrintConfig& config);
CalibrationPlate describe_plate(const Slic3r::Model& model, std::size_t index,
    const std::optional<FlowCalibrationTarget>& flow_target = std::nullopt);
} // namespace orca
} // namespace libslicer
