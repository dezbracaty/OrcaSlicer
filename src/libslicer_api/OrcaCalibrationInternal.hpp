#pragma once

#include <libslicer/Calibration.hpp>
#include "Plater.hpp"

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
};
namespace orca {
Slic3r::Calib_Params dialog_parameters(const CalibrationRequest& request, const Slic3r::DynamicPrintConfig& config);
std::vector<SliceDiagnostic> calibration_warnings(const CalibrationRequest& request, const Slic3r::DynamicPrintConfig& config);
CalibrationPlate describe_plate(const Slic3r::Model& model, std::size_t index,
    const std::optional<FlowCalibrationTarget>& flow_target = std::nullopt);
} // namespace orca
} // namespace libslicer
