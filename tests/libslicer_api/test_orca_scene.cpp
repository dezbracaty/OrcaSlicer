#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <libslicer/Calibration.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <cmath>
#include <libslicer_api/OrcaCalibrationInternal.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>

TEST_CASE("flow adapter preserves the original shared selection pivot", "[orca_plater][scene]")
{
    using namespace Slic3r;
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors            = {"Flashforge"};
    auto library               = libslicer::Library::open(options);
    REQUIRE(library);
    auto config = libslicer::Config::defaults();
    REQUIRE(config.apply_patch(
        {{"nozzle_diameter", "0.8"}, {"printable_area", "0x0,320x0,320x320,0x320"}, {"initial_layer_print_height", "0.4"}}));
    libslicer::CalibrationRequest request;
    request.config = config.snapshot();
    request.mode   = libslicer::CalibMode::Calib_Flow_Rate;
    auto prepared  = library->prepare_calibration(request);
    REQUIRE(prepared);

    DynamicPrintConfig ignored;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
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
    } cleanup{plates, presets};
    Semver version;
    En3mfType type  = En3mfType::From_Other;
    const auto path = std::filesystem::path(LIBSLICER_TEST_RESOURCE_DIR) / "calib/filament_flow/flowrate-test-pass1.3mf";
    auto original = Model::read_from_archive(path.string(), &ignored, &substitutions, type, LoadStrategy::LoadModel | LoadStrategy::Silence,
                                             &plates, &presets, &version);
    original.center_instances_around_point(Vec2d(160, 160));
    BoundingBoxf3 bounds;
    for (auto* object : original.objects) {
        object->sort_volumes(true);
        object->ensure_on_bed();
        bounds.merge(object->bounding_box_exact());
    }
    const auto pivot          = bounds.center();
    const float nozzle        = 0.8;
    const float xy_scale      = nozzle / 0.6;
    const double layer_height = nozzle / 2.;
    const float z_scale       = (std::max(0.4, layer_height) + 9 * layer_height) / 2;
    const Transform3d scaling = Geometry::translation_transform(pivot) * Geometry::scale_transform(Vec3d(xy_scale, xy_scale, z_scale)) *
                                Geometry::translation_transform(-pivot);
    const auto& actual = prepared.task->plates().front().objects;
    REQUIRE(actual.size() == original.objects.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto expected = scaling * original.objects[i]->instances.front()->get_matrix();
        // do_scale drops the scaled objects onto the bed; it does not alter XY.
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 4; ++c)
                CHECK(actual[i].geometry.transform[r * 4 + c] == Catch::Approx(expected(r, c)).margin(1e-8));
    }
}

namespace {
libslicer::LibraryOptions flow_options();
libslicer::Config flow_config()
{
    auto library = libslicer::Library::open(flow_options());
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X"; selection.machine_variant_id = "0.4";
    auto created = library->create_config(selection); REQUIRE(created);
    selection.filament_preset_ids.assign(2, created.selection.filament_preset_ids.front());
    selection.filament_physical_tools = {0, 0};
    auto activated = library->activate_config(selection); REQUIRE(activated);
    auto config = libslicer::Config::for_overrides(*library->active_config_snapshot(), libslicer::ConfigScope::Full);
    const auto result = config.apply_patch({{"filament_flow_ratio", "1.1,0.98"}, {"nozzle_diameter", "0.8"},
        {"filament_max_volumetric_speed", "1,12"},
        {"printable_area", "0x0,320x0,320x320,0x320"}, {"top_surface_speed", "200"},
        {"internal_solid_infill_speed", "200"}, {"top_surface_filament_id", "1"},
        {"sparse_infill_filament_id", "1"}, {"enable_support", "1"}});
    for (const auto& d : result.diagnostics) UNSCOPED_INFO(d.key << ": " << d.message);
    REQUIRE(result);
    return config;
}
libslicer::LibraryOptions flow_options()
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    return options;
}
}

TEST_CASE("flow targets bind all features and derive task-owned sample results", "[flow-calibration]")
{
    auto library = libslicer::Library::open(flow_options());
    REQUIRE(library);
    for (bool linear : {false, true}) for (int pass : {1, 2}) {
        auto config = flow_config();
        const auto before = config.snapshot().values();
        libslicer::CalibrationRequest request;
        request.config = config.snapshot(); request.mode = libslicer::CalibMode::Calib_Flow_Rate;
        request.flow_filament_slot = 1; request.flow_linear = linear; request.flow_pass = pass;
        const auto prepared = library->prepare_calibration(request);
        for (const auto& d : prepared.diagnostics) UNSCOPED_INFO(d.message);
        REQUIRE(prepared);
        const auto& plate = prepared.task->plates().at(0);
        CHECK(plate.config.value("filament_map_mode") == "Manual");
        CHECK(plate.config.value("enable_support") == "0");
        CHECK(plate.config.value("layer_height") == "0.4");
        REQUIRE_FALSE(plate.objects.empty());
        for (std::size_t i = 0; i < plate.objects.size(); ++i) {
            const auto& object = plate.objects[i];
            REQUIRE(object.flow_sample);
            const auto& sample = *object.flow_sample;
            CHECK(sample.baseline_flow_ratio == Catch::Approx(.98));
            const auto result = prepared.task->flow_result(0, i);
            REQUIRE(result);
            CHECK(result.logical_extruder_index == 0); CHECK(result.physical_tool_index == 0);
            CHECK(result.nozzle_diameter_mm == Catch::Approx(.8));
            REQUIRE(result.config_patch.size() == 1);
            CHECK(result.config_patch.front().first == "filament_flow_ratio");
            auto after = libslicer::Config::for_overrides(request.config, libslicer::ConfigScope::Full);
            REQUIRE(after.apply_patch(result.config_patch));
            const auto serialized = after.snapshot().value("filament_flow_ratio").value();
            CHECK(serialized.substr(0, serialized.find(',')) == "1.1");
            const double modifier = std::stod(sample.label);
            const double expected = linear ? .98 + modifier : .98 * (1 + modifier / 100);
            CHECK(sample.calibrated_flow_ratio == Catch::Approx(expected).margin(1e-7));
            CHECK(std::stod(serialized.substr(serialized.find(',') + 1)) == Catch::Approx(expected).margin(1e-6));
            for (const auto& volume : object.geometry.volumes) CHECK(volume.default_filament_slot == 2);
            for (const auto& [key, value] : object.object_config)
                if (key == "extruder" || key.find("filament_id") != std::string::npos) CHECK(value == "2");
        }
        CHECK(config.snapshot().values() == before);
        CHECK_FALSE(prepared.task->flow_result(1, 0));
        CHECK_FALSE(prepared.task->flow_result(0, plate.objects.size()));
    }
}

TEST_CASE("flow material preflight and refinement share the selected snapshot", "[flow-calibration]")
{
    auto config = flow_config();
    auto parameters = libslicer::calibration_parameters(libslicer::CalibMode::Calib_Flow_Rate, config.snapshot(),
        {{"flow_filament_slot", "1"}, {"flow_type", "0"}, {"flow_pattern", "1"}, {"flow_baseline_ratio", "999"}});
    const auto baseline = [](const auto& p) {
        for (const auto& f : p.fields) if (f.key == "flow_baseline_ratio") return f.value;
        return std::string{};
    };
    CHECK(baseline(parameters) == "0.98");
    auto checked = libslicer::validate_calibration_parameters(parameters);
    REQUIRE(checked); CHECK(checked.request.flow_filament_slot == 1);
    parameters = libslicer::update_calibration_parameters(parameters, "next_pass", "");
    checked = libslicer::validate_calibration_parameters(parameters);
    REQUIRE(checked); CHECK(checked.request.flow_pass == 2);
    CHECK(checked.request.flow_pattern == libslicer::CalibrationRequest::FlowPattern::Monotonic);
    CHECK(checked.request.flow_filament_slot == 1);
    parameters = libslicer::update_calibration_parameters(parameters, "flow_filament_slot", "2");
    CHECK_FALSE(libslicer::validate_calibration_parameters(parameters));
    auto library = libslicer::Library::open(flow_options()); REQUIRE(library);
    auto request = checked.request;
    request.flow_filament_slot = 2; CHECK_FALSE(library->prepare_calibration(request));
    request.flow_filament_slot = 1;
    REQUIRE(config.set("generate_reinforced_infills", "1")); request.config = config.snapshot();
    CHECK_FALSE(library->prepare_calibration(request));
}

TEST_CASE("flow slice uses the selected material on a shared physical tool", "[flow-calibration][slice]")
{
    auto library = libslicer::Library::open(flow_options()); REQUIRE(library);
    auto config = flow_config();
    // Isolate generated calibration deposition from the vendor's custom purge
    // macro, whose unassigned extrusion has no material-selection command.
    REQUIRE(config.apply_patch({{"machine_start_gcode", "G90\nM83"}, {"machine_end_gcode", ""},
        {"filament_start_gcode", ";"}, {"filament_end_gcode", ";"}}));
    libslicer::CalibrationRequest request;
    request.config = config.snapshot(); request.mode = libslicer::CalibMode::Calib_Flow_Rate;
    request.flow_filament_slot = 1;
    auto prepared = library->prepare_calibration(request);
    for (const auto& d : prepared.diagnostics) UNSCOPED_INFO(d.message);
    REQUIRE(prepared);
    libslicer::CalibrationOutputOptions output;
    const auto sliced = library->slice_calibration(*prepared.task, 0, output);
    for (const auto& d : sliced.diagnostics) INFO(d.message);
    REQUIRE(sliced); REQUIRE(sliced.preview);
    bool extrusion = false;
    for (const auto& segment : sliced.preview->segments) if (segment.extrusion_delta_mm > 0) {
        INFO("role=" << int(segment.extrusion_role) << " object=" << segment.object_id << " command=" << segment.source_command_id);
        extrusion = true; CHECK(segment.filament_id == 1); CHECK(segment.tool_id == 0);
    }
    CHECK(extrusion);
    std::filesystem::remove(sliced.output.path);
}

TEST_CASE("flow keeps a repeated material slot that matches its vendor defaults", "[flow-calibration]")
{
    auto library = libslicer::Library::open(flow_options()); REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X"; selection.machine_variant_id = "0.4";
    REQUIRE(library->activate_config(selection));
    const auto baseline = library->active_config_snapshot()->value("filament_flow_ratio").value();
    REQUIRE(library->resize_active_filament_slots(2));
    REQUIRE(library->apply_active_config_patch({{"filament_flow_ratio", "1.1," + baseline}}));
    const auto captured = *library->active_config_snapshot();
    const auto active = library->active_config(); REQUIRE(active);
    REQUIRE(active->selection.filament_preset_ids.size() == 2);
    REQUIRE(active->selection.filament_preset_ids[0] == active->selection.filament_preset_ids[1]);
    libslicer::CalibrationRequest request;
    request.config = captured; request.mode = libslicer::CalibMode::Calib_Flow_Rate;
    request.flow_filament_slot = 1;
    const auto prepared = library->prepare_calibration(request);
    for (const auto& d : prepared.diagnostics) UNSCOPED_INFO(d.message);
    REQUIRE(prepared);
    CHECK(prepared.task->plates().at(0).config.value("filament_flow_ratio") == captured.value("filament_flow_ratio"));
    CHECK(prepared.task->request().config.values() == captured.values());
    CHECK(library->active_config_snapshot()->values() == captured.values());
    bool found_zero = false;
    for (std::size_t i = 0; i < prepared.task->plates()[0].objects.size(); ++i) {
        const auto& sample = prepared.task->plates()[0].objects[i].flow_sample;
        REQUIRE(sample);
        CHECK(sample->baseline_flow_ratio == Catch::Approx(std::stod(baseline)));
        if (sample->label == "0") {
            found_zero = true;
            const auto result = prepared.task->flow_result(0, i); REQUIRE(result);
            auto checked = libslicer::Config::for_overrides(captured, libslicer::ConfigScope::Full);
            REQUIRE(checked.apply_patch(result.config_patch));
            CHECK(checked.snapshot().value("filament_flow_ratio") == captured.value("filament_flow_ratio"));
        }
    }
    CHECK(found_zero);
}

TEST_CASE("public flow slicing preserves a reordered physical tool and nozzle", "[flow-calibration][slice]")
{
    auto options = flow_options(); options.vendors = {"BBL"};
    auto library = libslicer::Library::open(options); REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Bambu Lab H2D"; selection.machine_variant_id = "0.4";
    selection.filament_preset_ids.assign(2, "Bambu PLA Basic @BBL H2D");
    // H2D's captured factory mapping is logical 0 -> physical 1, 1 -> 0.
    selection.filament_physical_tools = {1, 0};
    const auto activated = library->activate_config(selection);
    for (const auto& d : activated.diagnostics) UNSCOPED_INFO(d.message);
    REQUIRE(activated);
    auto config = libslicer::Config::for_overrides(*library->active_config_snapshot(), libslicer::ConfigScope::Full);
    const auto changed = config.apply_patch({{"nozzle_diameter", "0.4,0.8"}, {"max_layer_height", "0.32,0.64"},
        {"bridge_line_width", "0"},
        {"filament_flow_ratio", "1.1,0.98"}, {"filament_max_volumetric_speed", "1,12"},
        {"printable_area", "0x0,320x0,320x320,0x320"}, {"enable_prime_tower", "0"},
        // Keep the original Bambu material-selection command while removing
        // purge/camera routines unrelated to the generated calibration paths.
        {"machine_start_gcode", "G90\nM83\nT[initial_no_support_extruder]"}, {"machine_end_gcode", ""},
        {"filament_start_gcode", ";"}, {"filament_end_gcode", ";"}, {"change_filament_gcode", ""}});
    for (const auto& d : changed.diagnostics) UNSCOPED_INFO(d.key << ": " << d.message);
    REQUIRE(changed);
    const auto validation = config.validate();
    for (const auto& d : validation) UNSCOPED_INFO(d.key << ": " << d.message);
    REQUIRE(validation.empty());
    const auto captured = config.snapshot();
    libslicer::CalibrationRequest request;
    request.config = captured; request.mode = libslicer::CalibMode::Calib_Flow_Rate;
    request.flow_filament_slot = 1;
    const auto prepared = library->prepare_calibration(request);
    for (const auto& d : prepared.diagnostics) UNSCOPED_INFO(d.message);
    REQUIRE(prepared);
    const auto& plate = prepared.task->plates().at(0);
    CHECK(plate.config.value("layer_height") == "0.4");
    CHECK(plate.config.value("filament_flow_ratio") == captured.value("filament_flow_ratio"));
    CHECK(plate.config.value("filament_map") == "1,2");
    CHECK(plate.config.value("physical_extruder_map") == "1,0");
    const auto result = prepared.task->flow_result(0, 0);
    std::string result_diagnostics;
    for (const auto& d : result.diagnostics) result_diagnostics += d.message + "\n";
    INFO(result_diagnostics);
    REQUIRE(result);
    CHECK(result.logical_extruder_index == 1); CHECK(result.physical_tool_index == 0);
    CHECK(result.nozzle_diameter_mm == Catch::Approx(.8));
    libslicer::CalibrationOutputOptions output;
    const auto sliced = library->slice_calibration(*prepared.task, 0, output);
    for (const auto& d : sliced.diagnostics) UNSCOPED_INFO(d.message);
    REQUIRE(sliced); REQUIRE(sliced.preview);
    bool extrusion = false;
    for (const auto& segment : sliced.preview->segments) if (segment.extrusion_delta_mm > 0) {
        extrusion = true;
        CHECK(segment.filament_id == 1); CHECK(segment.tool_id == 0);
    }
    CHECK(extrusion);
    std::filesystem::remove(sliced.output.path);
    CHECK(config.snapshot().values() == captured.values());
}

TEST_CASE("native flow scene preserves a reordered physical tool and nozzle", "[flow-calibration][slice]")
{
    using namespace Slic3r;
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"filament_diameter", "1.75,1.75"}, {"filament_flow_ratio", "1.1,0.98"},
        {"filament_map", "1,2"}, {"physical_extruder_map", "1,0"}, {"nozzle_diameter", "0.4,0.8"},
        {"filament_max_volumetric_speed", "1,12"}, {"filament_process_type", "thermoplastic;thermoplastic"},
        {"filament_colour", "#FFFFFF;#000000"}, {"filament_extruder_variant", "Direct Drive Standard;Direct Drive Standard"},
        {"filament_self_index", "1,2"},
        {"printable_area", "0x0,320x0,320x320,0x320"}, {"gcode_flavor", "klipper"},
        {"machine_start_gcode", "G90\nM83"}, {"machine_end_gcode", ""}, {"enable_prime_tower", "0"},
        {"generate_reinforced_infills", "0"}, {"generate_reinforced_perimeters", "0"}})
        config.set_deserialize_strict(key, value);
    const auto target = libslicer::orca::resolve_flow_target(config, 1);
    CHECK(target.slot == 1); CHECK(target.logical_extruder == 1); CHECK(target.physical_tool == 0);
    CHECK(target.nozzle_diameter == Catch::Approx(.8));
    set_resources_dir(LIBSLICER_TEST_RESOURCE_DIR);
    libslicer::orca::Plater host(config);
    host.prepare_flow(false, 1, ipMonotonic, target);
    auto model = host.plate_model(0);
    Print print; print.set_plate_origin(Vec3d::Zero());
    print.apply(model, host.full_config());
    StringObjectException warning;
    CHECK(print.validate(&warning).string.empty());
    print.process();
    CHECK(print.get_slice_used_filaments(false) == std::vector<unsigned>{1});
    CHECK(print.config().filament_map.get_at(1) == 2);
    CHECK(print.config().physical_extruder_map.get_at(1) == 0);
    const auto path = std::filesystem::temp_directory_path() / "libslicer-flow-reordered.gcode";
    GCodeProcessorResult processor;
    print.export_gcode(path.string(), &processor);
    REQUIRE_FALSE(processor.filament_change_sequence.empty());
    CHECK(std::all_of(processor.filament_change_sequence.begin(), processor.filament_change_sequence.end(), [](auto id) { return id == 1; }));
    std::filesystem::remove(path);
    // Invalid mappings cannot use ConfigOption::get_at's broadcast fallback.
    config.set_deserialize_strict("filament_map", "1,3");
    CHECK_THROWS(libslicer::orca::resolve_flow_target(config, 1));
}

// Independent native-reader/postprocessing reference for the fixed GUI entry.
// The source provenance check pins the original Plater blocks; this path never
// calls the production adapter's compatibility or conversion helpers.
TEST_CASE("3mf geometry and project configuration match native import processing", "[project-orca-reference]")
{
    using namespace Slic3r;
    // This executable links the native core directly as an independent
    // reference; its process-global resource paths are separate from the DLL.
    const auto native_work=std::filesystem::temp_directory_path()/"libslicer-3mf-native-reference";
    std::filesystem::create_directories(native_work);
    set_resources_dir(LIBSLICER_TEST_RESOURCE_DIR);set_data_dir(native_work.string());set_temporary_dir(native_work.string());
    auto library=libslicer::Library::open(flow_options()); REQUIRE(library);
    libslicer::ConfigSelection selection;selection.machine_model_id="Flashforge AD5X";selection.machine_variant_id="0.4";
    REQUIRE(library->activate_config(selection));
    const auto machine=*library->active_project_machine();
    PresetBundle filament_catalog, reference_seed;
    const auto profiles=std::filesystem::path(LIBSLICER_TEST_RESOURCE_DIR)/"profiles";
    filament_catalog.load_vendor_configs_from_json(profiles.string(),"OrcaFilamentLibrary",PresetBundle::LoadSystem,ForwardCompatibilitySubstitutionRule::EnableSilent);
    reference_seed.load_vendor_configs_from_json(profiles.string(),"Flashforge",PresetBundle::LoadSystem,ForwardCompatibilitySubstitutionRule::EnableSilent,&filament_catalog);
    DynamicPrintConfig captured;
    const auto captured_snapshot=*library->active_config_snapshot();
    for(const auto& [key,value]:captured_snapshot.values())captured.set_deserialize_strict(key,value);
    const auto filament_count=captured.option<ConfigOptionStrings>("filament_colour")->size();
    std::vector<std::string> dirty(filament_count+2,escape_strings_cstyle(Preset::filament_options()));
    dirty.front()=escape_strings_cstyle(Preset::print_options());dirty.back()=escape_strings_cstyle(Preset::printer_options());
    captured.set_key_value("different_settings_to_system",new ConfigOptionStrings(dirty));
    reference_seed.load_config_model("native captured context.3mf",captured);
    double min_x=1e30,min_y=1e30,max_x=-1e30,max_y=-1e30;
    for (const auto& p:machine.variant.printable_area) {min_x=std::min(min_x,p.x);min_y=std::min(min_y,p.y);max_x=std::max(max_x,p.x);max_y=std::max(max_y,p.y);}
    const Vec2d center((min_x+max_x)/2,(min_y+max_y)/2);
    const Vec3d bed_size(max_x-min_x-2,max_y-min_y-2,-1);
    std::vector<std::filesystem::path> sources;
    if (const char* directory=std::getenv("LIBSLICER_PROJECT_CORPUS")) {
        for (const auto& file:std::filesystem::recursive_directory_iterator(directory))
            if (file.is_regular_file()&&file.path().extension()==".3mf") sources.push_back(file.path());
    } else {
        sources={std::filesystem::path(LIBSLICER_TEST_RESOURCE_DIR)/"calib/pressure_advance/pa_pattern.3mf",
                 std::filesystem::path(LIBSLICER_TEST_RESOURCE_DIR)/"calib/filament_flow/flowrate-test-pass1.3mf"};
    }
    if(const char* external=std::getenv("LIBSLICER_PROJECT_TEST_INPUT"))sources.emplace_back(external);
    std::sort(sources.begin(),sources.end()); REQUIRE_FALSE(sources.empty());
    for (const auto& source:sources) {
        CAPTURE(source.string());
        for(const auto mode:{libslicer::ProjectReadMode::Geometry,libslicer::ProjectReadMode::Project}) {
            CAPTURE(mode);
            DynamicPrintConfig loaded;ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
            En3mfType type=En3mfType::From_BBS;Semver version;PlateDataPtrs plates;std::vector<Preset*> presets;
            struct Cleanup {PlateDataPtrs& plates;std::vector<Preset*>& presets;~Cleanup(){release_PlateData_list(plates);for(auto* p:presets){delete p->loading_substitutions;p->loading_substitutions=nullptr;delete p;}}} cleanup{plates,presets};
            Model original;std::string original_error;
            try {
                auto strategy=LoadStrategy::LoadModel;
                if(mode==libslicer::ProjectReadMode::Project)strategy=strategy|LoadStrategy::LoadConfig|LoadStrategy::CheckVersion|LoadStrategy::LoadAuxiliary;
                original=Model::read_from_archive(source.string(),&loaded,&substitutions,type,strategy,&plates,&presets,&version);
            } catch(const std::exception& e) {original_error=e.what();}
            const auto actual=library->read_project({source.string(),mode},{},[](const auto&){return libslicer::ProjectDecisionAnswer::No;});
            for(const auto& d:actual.diagnostics) UNSCOPED_INFO(d.code<<": "<<d.message);
            INFO("Original reader error: "<<original_error);
            // A native read failure must not be disguised as success. Valid
            // corpus projects must not acquire new adapter rejection policies.
            REQUIRE(actual.success==original_error.empty());
            if(!actual.success)continue;
            if(mode==libslicer::ProjectReadMode::Project) {
                // Independent native PresetBundle path. Compatibility-tree
                // branches are separately pinned and exercised by controlled
                // tests; full nonempty BBS/Orca configs reach this native block.
                PresetBundle bundle=reference_seed;bundle.reset_project_embedded_presets();
                if(type==En3mfType::From_Prusa) {
                    // Plater.cpp grows logical materials before downgrading a
                    // Prusa project to geometry. The palette cursor starts at
                    // zero here because candidates are never installed.
                    std::set<int> ids;
                    for(const auto* object:original.objects) {
                        if(object->config.option("extruder"))ids.insert(object->config.extruder());
                        for(const auto* volume:object->volumes) {
                            if(volume->config.option("extruder"))ids.insert(volume->config.extruder());
                            for(int id:volume->get_extruders())ids.insert(id);
                        }
                    }
                    const int needed=ids.empty()?0:*ids.rbegin();
                    const char* palette[]={"#00C1AE","#F4E2C1","#ED1C24","#00FF7F","#F26722","#FFEB31","#7841CE","#115877","#ED1E79","#2EBDEF","#345B2F","#800080","#FA8173","#800000","#F7B763","#A4C41E"};
                    unsigned cursor=0;
                    while(bundle.filament_presets.size()<64 && int(bundle.filament_presets.size())<needed)
                        bundle.set_num_filaments(bundle.filament_presets.size()+1,palette[cursor++%16]);
                }
                bool effective_config=type!=En3mfType::From_Prusa;
                if(loaded.empty() && !(type==En3mfType::From_Orca && version>*Semver::parse(SoftFever_VERSION))) effective_config=false;
                CHECK(actual.load_config==effective_config);
                if(effective_config&&!loaded.empty()) {
                    if(!presets.empty())bundle.load_project_embedded_presets(presets,ForwardCompatibilitySubstitutionRule::Enable);
                    DynamicPrintConfig config;config.apply(FullPrintConfig::defaults());auto file_config=loaded;config+=std::move(file_config);Preset::normalize(config);
                    if((type==En3mfType::From_BBS||type==En3mfType::From_Orca)&&version<Semver(2,0,0)) {
                        const auto count=config.option<ConfigOptionStrings>("filament_colour",true)->size();
                        if(auto* prime=config.option<ConfigOptionFloats>("filament_prime_volume");prime&&!prime->values.empty()&&count>1)prime->values.resize(count,prime->values.front());
                        config.option<ConfigOptionEnum<WipeTowerWallType>>("wipe_tower_wall_type",true)->value=wtwRectangle;
                        config.option<ConfigOptionPercent>("prime_tower_infill_gap",true)->value=100;
                        config.option<ConfigOptionInts>("filament_adhesiveness_category",true)->values.assign(count,100);
                        auto& differences=config.option<ConfigOptionStrings>("different_settings_to_system",true)->values;differences.resize(count+2);
                        std::vector<std::string> keys;unescape_strings_cstyle(differences[0],keys);keys.emplace_back("wipe_tower_wall_type");keys.emplace_back("prime_tower_infill_gap");differences[0]=escape_strings_cstyle(keys);
                        for(std::size_t i=1;i<=count;++i){keys.clear();unescape_strings_cstyle(differences[i],keys);keys.emplace_back("filament_adhesiveness_category");differences[i]=escape_strings_cstyle(keys);}
                    }
                    std::set<std::string> modified_gcodes;bundle.validate_presets(source.filename().string(),config,modified_gcodes);
                    std::optional<ConfigOptionFloats> x,y;
                    if(const auto* v=config.opt<ConfigOptionFloats>("wipe_tower_x"))x=*v;
                    if(const auto* v=config.opt<ConfigOptionFloats>("wipe_tower_y"))y=*v;
                    bundle.load_config_model(source.filename().string(),std::move(config),version);
                    const auto& printer=bundle.printers.get_edited_preset();
                    if(printer.printer_technology()==ptFFF&&!printer.config.opt_bool("single_extruder_multi_material"))bundle.set_num_filaments(printer.config.option<ConfigOptionFloats>("nozzle_diameter")->size());
                    auto& project=bundle.project_config;
                    if(x)*project.opt<ConfigOptionFloats>("wipe_tower_x")=*x;if(y)*project.opt<ConfigOptionFloats>("wipe_tower_y")=*y;
                    if(const auto* colors=project.opt<ConfigOptionStrings>("filament_colour")) {
                        const auto count=colors->size();auto* map=project.opt<ConfigOptionInts>("filament_map",true);if(map->size()!=count)map->values.resize(count,1);
                        auto* multi=project.opt<ConfigOptionStrings>("filament_multi_colour",true);if(multi->size()!=count)multi->values.resize(count);
                        for(std::size_t i=0;i<count;++i){const auto parts=split_string(multi->values[i],' ');if(parts.empty()||parts[0]!=colors->values[i])multi->values[i]=colors->values[i];}
                        auto* types=project.opt<ConfigOptionStrings>("filament_colour_type",true);if(types->size()!=count){types->values.resize(count);for(auto& value:types->values)if(value.empty())value="1";}
                    }
                }
                auto expected=bundle.full_config();REQUIRE(actual.configuration.valid());
                const auto converted=actual.configuration.snapshot();
                std::vector<std::string> mismatches;
                for(const auto& key:expected.keys()) {
                    const auto value=converted.value(key);
                    if(!value || *value!=expected.option(key)->serialize())mismatches.push_back(key);
                }
                for(const auto& key:mismatches)UNSCOPED_INFO("Native project config differs: "<<key);
                const auto expected_colors=expected.option("filament_colour")->serialize();
                const auto actual_colors=converted.value("filament_colour");
                CAPTURE(mismatches,expected_colors,actual_colors);CHECK(mismatches.empty());
                continue;
            }
            // Fixed Plater.cpp 6767-6882, 7114-7207: unit/multipart choices are
            // explicitly No. Corpus files define their own instances.
            original.removed_objects_with_zero_volume();
            for(auto* object:original.objects)if(!object->instances.empty())object->ensure_on_bed(false);
            original.center_instances_around_point(center);
            for(auto* object:original.objects){
                object->sort_volumes(true);
                REQUIRE_FALSE(object->instances.empty());
                for(std::size_t i=0;i<object->instances.size();++i){
                    const Vec3d ratio=object->instance_bounding_box(i).size().cwiseQuotient(bed_size);
                    const double maximum=std::max(ratio.x(),ratio.y());
                    if(maximum>10000){object->scale_mesh_after_creation(1./maximum);object->origin_translation=Vec3d::Zero();object->center_around_origin();break;}
                }
                object->ensure_on_bed(false);
            }
            REQUIRE(actual.scene); REQUIRE(actual.scene->objects.size()==original.objects.size());
            for(std::size_t oi=0;oi<original.objects.size();++oi){
                const auto* native=original.objects[oi]; const auto& converted=actual.scene->objects[oi];
                CHECK(converted.default_filament_slot==(native->config.has("extruder")?native->config.extruder():0));
                std::vector<const ModelVolume*> nonempty;for(const auto* v:native->volumes)if(!v->mesh().empty())nonempty.push_back(v);
                REQUIRE(converted.parts.size()==nonempty.size());REQUIRE(converted.instances.size()==native->instances.size());
                for(std::size_t vi=0;vi<nonempty.size();++vi){
                    const auto* volume=nonempty[vi];const auto& part=converted.parts[vi];
                    // SDK enum order is independent of the native enum order.
                    const std::map<ModelVolumeType,libslicer::ProjectPartRole> roles={
                        {ModelVolumeType::MODEL_PART,libslicer::ProjectPartRole::Model},
                        {ModelVolumeType::NEGATIVE_VOLUME,libslicer::ProjectPartRole::NegativeVolume},
                        {ModelVolumeType::PARAMETER_MODIFIER,libslicer::ProjectPartRole::ParameterModifier},
                        {ModelVolumeType::SUPPORT_ENFORCER,libslicer::ProjectPartRole::SupportEnforcer},
                        {ModelVolumeType::SUPPORT_BLOCKER,libslicer::ProjectPartRole::SupportBlocker}};
                    CHECK(part.role==roles.at(volume->type()));REQUIRE(part.mesh);
                    CHECK(part.mesh->vertices.size()==volume->mesh().its.vertices.size());CHECK(part.mesh->triangles.size()==volume->mesh().its.indices.size());
                    const auto expected=volume->get_matrix();
                    for(int r=0;r<4;++r)for(int c=0;c<4;++c)CHECK(part.local_transform[r*4+c]==Catch::Approx(expected(r,c)).margin(1e-7));
                    const auto& painting=volume->mmu_segmentation_facets.get_data();
                    CHECK(part.material_facets.bitstream.size()==painting.bitstream.size());CHECK(part.material_facets.roots.size()==painting.triangles_to_split.size());
                    CHECK(std::equal(part.material_facets.bitstream.begin(),part.material_facets.bitstream.end(),painting.bitstream.begin(),painting.bitstream.end(),[](auto a,bool b){return bool(a)==b;}));
                    bool roots_match=part.material_facets.roots.size()==painting.triangles_to_split.size();
                    for(std::size_t i=0;roots_match&&i<painting.triangles_to_split.size();++i)
                        roots_match=part.material_facets.roots[i].triangle_index==painting.triangles_to_split[i].triangle_idx&&part.material_facets.roots[i].bitstream_start_index==painting.triangles_to_split[i].bitstream_start_idx;
                    CHECK(roots_match);
                    bool vertices_match=part.mesh->vertices.size()==volume->mesh().its.vertices.size();
                    for(std::size_t i=0;vertices_match&&i<part.mesh->vertices.size();++i){const auto& a=part.mesh->vertices[i];const auto& b=volume->mesh().its.vertices[i];vertices_match=a.x==b.x()&&a.y==b.y()&&a.z==b.z();}
                    CHECK(vertices_match);
                    bool triangles_match=part.mesh->triangles.size()==volume->mesh().its.indices.size();
                    for(std::size_t i=0;triangles_match&&i<part.mesh->triangles.size();++i){const auto& a=part.mesh->triangles[i];const auto& b=volume->mesh().its.indices[i];triangles_match=a.vertex_a==unsigned(b.x())&&a.vertex_b==unsigned(b.y())&&a.vertex_c==unsigned(b.z());}
                    CHECK(triangles_match);
                }
                for(std::size_t ii=0;ii<native->instances.size();++ii){
                    const auto expected=native->instances[ii]->get_matrix();
                    for(int r=0;r<4;++r)for(int c=0;c<4;++c)CHECK(converted.instances[ii].transform[r*4+c]==Catch::Approx(expected(r,c)).margin(1e-6));
                }
            }
        }
    }
}
