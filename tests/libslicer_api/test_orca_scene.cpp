#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <libslicer/Calibration.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <filesystem>
#include <fstream>
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
