#pragma once

#include <libslic3r/CutUtils.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/Print.hpp>

namespace libslicer::orca {

// Internal names and responsibilities follow Orca's scene classes. These are
// the calibration subset; no window, application singleton or GUI stand-in.
using CalibMode    = Slic3r::CalibMode;
using Calib_Params = Slic3r::Calib_Params;

class Selection
{
public:
    explicit Selection(Slic3r::Model& model) : m_model(model) {}
    void setup_cache();
    // Original Selection relative/world/joint branch for the imported objects.
    void scale(const Slic3r::Vec3d& scale);

private:
    Slic3r::Model& m_model;
    Slic3r::Vec3d m_center{Slic3r::Vec3d::Zero()};
};

class PartPlate
{
public:
    explicit PartPlate(Slic3r::Model& model) : m_model(model) {}
    Slic3r::Vec3d get_origin() const { return Slic3r::Vec3d::Zero(); }
    bool empty() const { return m_objects.empty(); }
    Slic3r::ModelObjectPtrs get_objects_on_this_plate() const;

private:
    Slic3r::Model& m_model;
    std::vector<std::size_t> m_objects;
    friend class PartPlateList;
};

class PartPlateList
{
public:
    explicit PartPlateList(Slic3r::Model& model) : m_model(model) { create_plate(); }
    PartPlate* get_plate(std::size_t index);
    PartPlate* get_curr_plate() { return get_plate(m_current); }
    std::size_t get_curr_plate_index() const { return m_current; }
    std::size_t get_plate_count() const { return m_plates.size(); }
    std::size_t create_plate();
    void add_to_plate(std::size_t object, std::size_t instance, std::size_t plate);
    void select_plate(std::size_t index);
    void reset();

private:
    Slic3r::Model& m_model;
    std::vector<std::unique_ptr<PartPlate>> m_plates;
    std::size_t m_current{0};
};

class Plater
{
public:
    explicit Plater(const Slic3r::DynamicPrintConfig& config, const Slic3r::PresetBundle* seed = nullptr);
    Plater(const Plater&)            = delete;
    Plater& operator=(const Plater&) = delete;

    void prepare(const Slic3r::Calib_Params& params);
    void prepare_flow(bool linear, int pass, Slic3r::InfillPattern pattern);
    void prepare_tolerance();
    std::size_t plate_count() const;
    Slic3r::Model plate_model(std::size_t index);
    Slic3r::DynamicPrintConfig full_config() const;
    const Slic3r::Calib_Params& print_params() const;
    const std::vector<std::string>& notifications() const { return m_notifications; }
    Slic3r::PresetBundle& presets() { return m_presets; }
    Slic3r::Model& model() { return m_model; }
    PartPlateList& get_partplate_list() { return m_partplate_list; }

    // Original Plater calibration entry names and parameter types.
    void calib_pa(const Slic3r::Calib_Params& params);
    void _calib_pa_pattern(const Slic3r::Calib_Params& params);
    void _calib_pa_tower(const Slic3r::Calib_Params& params);
    void _calib_pa_pattern_gen_gcode();
    void calib_flowrate(bool linear, int pass, Slic3r::InfillPattern pattern);
    void calib_temp(const Slic3r::Calib_Params& params);
    void calib_max_vol_speed(const Slic3r::Calib_Params& params);
    void calib_retraction(const Slic3r::Calib_Params& params);
    void calib_VFA(const Slic3r::Calib_Params& params);
    void calib_input_shaping_freq(const Slic3r::Calib_Params& params);
    void calib_input_shaping_damp(const Slic3r::Calib_Params& params);
    void Calib_Cornering(const Slic3r::Calib_Params& params);
    void cut_horizontal(std::size_t object, std::size_t instance, double z, Slic3r::ModelObjectCutAttributes attributes);

private:
    void new_project();
    void load_generic_subobject();
    void add_model(bool imperial_units, std::string path);
    void apply_cut_object_to_model(std::size_t index, const Slic3r::ModelObjectPtrs& pieces);
    void adjust_settings_for_flowrate_calib(Slic3r::ModelObjectPtrs& objects, bool linear, int pass, Slic3r::InfillPattern pattern);

    Slic3r::PresetBundle m_presets;
    Slic3r::Model m_model;
    Slic3r::Calib_Params m_params;
    Slic3r::Print m_print;
    Selection m_selection;
    PartPlateList m_partplate_list;
    std::vector<std::string> m_notifications;
};
} // namespace libslicer::orca
