#include "Plater.hpp"

#include <libslic3r/ModelArrange.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <boost/filesystem/path.hpp>
#include <stdexcept>

namespace libslicer::orca {
using namespace Slic3r;

void Selection::setup_cache()
{
    BoundingBoxf3 bounds;
    for (const auto* object : m_model.objects)
        bounds.merge(object->bounding_box_exact());
    m_center = bounds.center();
}
void Selection::scale(const Vec3d& scale)
{
    const Transform3d transform = Geometry::translation_transform(m_center) * Geometry::scale_transform(scale) *
                                  Geometry::translation_transform(-m_center);
    for (auto* object : m_model.objects) {
        for (auto* instance : object->instances)
            instance->set_transformation(Geometry::Transformation(transform * instance->get_matrix()));
        object->invalidate_bounding_box();
    }
}
ModelObjectPtrs PartPlate::get_objects_on_this_plate() const
{
    ModelObjectPtrs result;
    for (auto index : m_objects)
        result.push_back(m_model.objects.at(index));
    return result;
}
PartPlate* PartPlateList::get_plate(std::size_t index) { return index < m_plates.size() ? m_plates[index].get() : nullptr; }
std::size_t PartPlateList::create_plate()
{
    m_plates.push_back(std::make_unique<PartPlate>(m_model));
    return m_plates.size() - 1;
}
void PartPlateList::add_to_plate(std::size_t object, std::size_t, std::size_t plate) { m_plates.at(plate)->m_objects.push_back(object); }
void PartPlateList::select_plate(std::size_t index)
{
    if (!get_plate(index))
        throw std::out_of_range("Calibration plate index");
    m_current = index;
}
void PartPlateList::reset()
{
    m_plates.clear();
    create_plate();
    m_current = 0;
}
static bool has_junction_deviation(const DynamicPrintConfig* config)
{
    const auto* limit = config->option<ConfigOptionFloats>("machine_max_junction_deviation");
    return config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value == gcfMarlinFirmware && limit && !limit->values.empty() &&
           limit->values.front() > 0;
}

Plater::Plater(const DynamicPrintConfig& config, const PresetBundle* seed)
    : m_presets(seed ? *seed : PresetBundle()), m_selection(m_model), m_partplate_list(m_model)
{
    // Embedded project presets use the original loader without saving files.
    m_presets.load_config_model("calibration.3mf", config);
    m_params.mode  = CalibMode::Calib_None;
    m_params.start = m_params.end = m_params.step = 0;
    m_params.freqStartX = m_params.freqEndX = m_params.freqStartY = m_params.freqEndY = 0;
    m_params.print_numbers                                                            = false;
    m_params.test_model                                                               = 0;
    m_print.set_calib_params(m_params);
}
void Plater::new_project()
{
    m_model              = Model();
    m_print.calib_mode() = CalibMode::Calib_None;
    m_partplate_list.reset();
}
void Plater::load_generic_subobject()
{
    // ObjectList::load_generic_subobject's cube data operation. The original PA
    // method scales this handle to the requested pattern dimensions afterwards.
    auto* object = m_model.add_object("Cube", "", make_cube(20, 20, 20));
    object->center_around_origin();
    object->add_instance();
    object->ensure_on_bed();
}
void Plater::add_model(bool, std::string path)
{
    DynamicPrintConfig ignored;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plate_data;
    std::vector<Preset*> presets;
    Semver version;
    struct Cleanup
    {
        PlateDataPtrs& plates;
        std::vector<Preset*>& presets;
        ~Cleanup()
        {
            for (auto* p : plates)
                delete p;
            for (auto* p : presets)
                delete p;
        }
    } cleanup{plate_data, presets};
    const auto strategy = LoadStrategy::LoadModel | LoadStrategy::Silence;
    Model imported;
    if (boost::filesystem::path(path).extension() == ".3mf") {
        En3mfType type = En3mfType::From_Other;
        imported       = Model::read_from_archive(path, &ignored, &substitutions, type, strategy, &plate_data, &presets, &version);
    } else
        imported = Model::read_from_file(path, &ignored, &substitutions, strategy, &plate_data, &presets, nullptr, &version);
    const auto config = m_presets.full_config();
    const auto center = get_extents(config.option<ConfigOptionPoints>("printable_area")->values).center();
    if (boost::filesystem::path(path).extension() == ".3mf")
        imported.center_instances_around_point(center);
    for (auto* source : imported.objects) {
        auto* object = m_model.add_object(*source);
        object->sort_volumes(true);
        if (source->instances.empty()) {
            object->center_around_origin();
            object->add_instance()->set_offset(to_3d(center, 0.));
        }
        object->ensure_on_bed();
    }
    if (m_model.objects.empty())
        throw std::runtime_error("Calibration resource is empty: " + path);
}
void Plater::apply_cut_object_to_model(std::size_t index, const ModelObjectPtrs& pieces)
{
    m_model.delete_object(index);
    for (const auto* piece : pieces)
        m_model.add_object(*piece)->ensure_on_bed();
}
void Plater::prepare(const Calib_Params& params)
{
    m_params = params;
    switch (params.mode) {
    case CalibMode::Calib_PA_Line:
    case CalibMode::Calib_PA_Pattern:
    case CalibMode::Calib_PA_Tower: calib_pa(m_params); break;
    case CalibMode::Calib_Temp_Tower: calib_temp(m_params); break;
    case CalibMode::Calib_Vol_speed_Tower: calib_max_vol_speed(m_params); break;
    case CalibMode::Calib_Retraction_tower: calib_retraction(m_params); break;
    case CalibMode::Calib_VFA_Tower: calib_VFA(m_params); break;
    case CalibMode::Calib_Input_shaping_freq: calib_input_shaping_freq(m_params); break;
    case CalibMode::Calib_Input_shaping_damp: calib_input_shaping_damp(m_params); break;
    case CalibMode::Calib_Cornering: Calib_Cornering(m_params); break;
    default: throw std::invalid_argument("Unsupported Plater calibration entry");
    }
}
void Plater::prepare_flow(bool linear, int pass, InfillPattern pattern) { calib_flowrate(linear, pass, pattern); }
void Plater::prepare_tolerance()
{
    new_project();
    add_model(false, resources_dir() + "/handy_models/OrcaToleranceTest.drc");
}
std::size_t Plater::plate_count() const { return m_partplate_list.get_plate_count(); }
DynamicPrintConfig Plater::full_config() const { return m_presets.full_config(); }
const Calib_Params& Plater::print_params() const { return m_print.calib_params(); }
Model Plater::plate_model(std::size_t index)
{
    auto* plate = m_partplate_list.get_plate(index);
    if (!plate)
        throw std::out_of_range("Calibration plate index");
    if (!m_model.calib_pa_pattern)
        return m_model;
    m_partplate_list.select_plate(index);
    _calib_pa_pattern_gen_gcode();
    Model result;
    for (auto* object : plate->get_objects_on_this_plate())
        result.add_object(*object);
    result.plates_custom_gcodes[0] = m_model.plates_custom_gcodes[index];
    return result;
}

#include "orca/PlaterCalibration.inc"
} // namespace libslicer::orca
