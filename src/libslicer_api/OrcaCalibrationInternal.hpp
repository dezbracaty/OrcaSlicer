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
};
namespace orca {
Slic3r::Calib_Params dialog_parameters(const CalibrationRequest& request, const Slic3r::DynamicPrintConfig& config);
CalibrationPlate describe_plate(const Slic3r::Model& model, std::size_t index);
} // namespace orca
} // namespace libslicer
