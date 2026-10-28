#pragma once

#include "Library.hpp"

namespace libslicer {

// Orca's CalibMode names and numeric values. Auto_PA_Line is reserved for
// source compatibility and is not implemented by this manual API.
enum class CalibMode : int {
    Calib_None               = 0,
    Calib_PA_Line            = 1,
    Calib_PA_Pattern         = 2,
    Calib_PA_Tower           = 3,
    Calib_Auto_PA_Line       = 4,
    Calib_Flow_Rate          = 5,
    Calib_Temp_Tower         = 6,
    Calib_Vol_speed_Tower    = 7,
    Calib_VFA_Tower          = 8,
    Calib_Retraction_tower   = 9,
    Calib_Input_shaping_freq = 10,
    Calib_Input_shaping_damp = 11,
    Calib_Cornering          = 12,
    // SDK entry for Orca's handy model, which has no original CalibMode.
    Calib_Tolerance_Model = 13
};

// Values passed by the original calib_dlg.cpp to Plater. Units and sampling
// are the original per-mode units; the adapter never rounds or clamps them.
struct CalibrationRequest
{
    ConfigSnapshot config;
    CalibMode mode{libslicer::CalibMode::Calib_None};
    int extruder_id{0};
    double start{0}, end{0}, step{0};
    bool print_numbers{true};
    double freqStartX{0}, freqEndX{0}, freqStartY{0}, freqEndY{0};
    int test_model{0};
    std::string shaper_type;
    // Original ParseStringValues reads positive integers for batch inputs.
    std::vector<int> accelerations, speeds;
    // Plater::calib_flowrate arguments. pass is 1 or 2 in both modes.
    bool flow_linear{false};
    int flow_pass{1};
    enum class FlowPattern { ArchimedeanChords, Monotonic };
    FlowPattern flow_pattern{FlowPattern::ArchimedeanChords};
};

// GUI-independent state of the original calib_dlg.cpp parameter forms. Values
// remain text while editing (including incomplete input). Existing SettingItem
// describes choices, units, visibility and enabled state; labels are source
// strings for the host's translation system. config is an immutable snapshot.
struct CalibrationParameters
{
    ConfigSnapshot config;
    CalibMode mode{CalibMode::Calib_None};
    std::vector<SettingItem> fields;
    std::vector<SliceDiagnostic> diagnostics;
    bool can_refine{false};
};
struct CalibrationValidationResult
{
    bool success{false};
    CalibrationRequest request;
    std::vector<SliceDiagnostic> diagnostics;
    explicit operator bool() const noexcept { return success; }
};

// Defaults and optional saved text values, without simulating edit events.
// Throws invalid_argument for unsupported modes, unknown fields or config.
LIBSLICER_API CalibrationParameters calibration_parameters(
    CalibMode mode, const ConfigSnapshot& config,
    const std::vector<std::pair<std::string, std::string>>& values = {});
// Change one field, performing Orca's PA reset, material shortcut or RRF
// linkage. focus_out applies only the original temperature kill-focus rule;
// prepare_calibration never applies that rule implicitly. "next_pass" advances
// an eligible flow form to its corresponding refinement pass.
LIBSLICER_API CalibrationParameters update_calibration_parameters(
    CalibrationParameters parameters, std::string_view key,
    std::string_view value, bool focus_out = false);
// Parses text and uses the same dialog_parameters validation as native task
// preparation. Invalid user input is returned as diagnostics, not an exception.
LIBSLICER_API CalibrationValidationResult validate_calibration_parameters(
    const CalibrationParameters& parameters);

struct CalibrationObject
{
    SliceObjectInput geometry;
    std::vector<std::pair<std::string, std::string>> object_config;
    std::vector<std::vector<std::pair<std::string, std::string>>> volume_config;
};
struct CalibrationLayerCode
{
    double print_z{0};
    std::string gcode;
};
struct CalibrationPlate
{
    std::size_t index{0};
    ConfigSnapshot config;
    std::vector<CalibrationObject> objects;
    std::vector<CalibrationLayerCode> custom_gcodes;
};

// Owns the source Plater host and all of its prepared plates. Multiple plates
// are retained, exactly as produced by the original PA Pattern arranger.
class LIBSLICER_API CalibrationTask final
{
public:
    ~CalibrationTask();
    CalibrationTask(const CalibrationTask&)            = delete;
    CalibrationTask& operator=(const CalibrationTask&) = delete;
    const std::vector<CalibrationPlate>& plates() const noexcept;
    const CalibrationRequest& request() const noexcept;

private:
    CalibrationTask();
    class Impl;
    std::unique_ptr<Impl> impl_;
    friend class Library;
};
struct CalibrationPrepareResult
{
    bool success{false}, cancelled{false};
    std::unique_ptr<CalibrationTask> task;
    std::vector<SliceDiagnostic> diagnostics;
    explicit operator bool() const noexcept { return success; }
};
struct CalibrationOutputOptions
{
    std::string output_gcode_path;
    bool generate_preview{true};
    bool generate_gcode_3mf{false};
    std::string output_gcode_3mf_path;
};
} // namespace libslicer
