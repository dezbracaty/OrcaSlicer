#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "../test_utils.hpp"

#include <boost/filesystem/operations.hpp>

#include <catch2/catch_all.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

struct SliceResult
{
    std::string               content;
    std::vector<unsigned int> used_extruders;
    std::vector<int>          filament_maps;
    bool                      has_wipe_tower { false };
    bool                      has_wipe_tower_tool_ordering { false };
    int                       total_toolchanges { 0 };
    size_t                    support_layer_count { 0 };
    size_t                    tower_toolchange_count { 0 };
};

void setup_test_dirs()
{
    boost::filesystem::path resources_dir =
        boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    Slic3r::set_resources_dir(resources_dir.string());
    Slic3r::set_data_dir(boost::filesystem::temp_directory_path().string());
}

DynamicPrintConfig base_print_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));
    return config;
}

Model load_obj_model(const std::string &filename)
{
    Model model;
    ObjInfo obj_info;
    std::string message;
    const std::string path = std::string(TEST_DATA_DIR) + "/" + filename;

    CAPTURE(path);
    REQUIRE(load_obj(path.c_str(), &model, obj_info, message));
    CAPTURE(message);
    REQUIRE(model.add_default_instances());
    model.center_instances_around_point({ 100.0, 100.0 });
    return model;
}

Model load_two_extruder_model()
{
    Model model;
    ObjInfo obj_info;
    std::string message;

    const std::string cube_path = std::string(TEST_DATA_DIR) + "/20mm_cube.obj";
    CAPTURE(cube_path);
    REQUIRE(load_obj(cube_path.c_str(), &model, obj_info, message));
    CAPTURE(message);

    const std::string pyramid_path = std::string(TEST_DATA_DIR) + "/pyramid.obj";
    CAPTURE(pyramid_path);
    REQUIRE(load_obj(pyramid_path.c_str(), &model, obj_info, message));
    CAPTURE(message);

    REQUIRE(model.objects.size() == 2);
    model.objects[0]->config.set_key_value("extruder", new ConfigOptionInt(1));
    model.objects[1]->config.set_key_value("extruder", new ConfigOptionInt(2));

    REQUIRE(model.add_default_instances());
    model.objects[0]->instances.front()->set_offset({ 80.0, 100.0, 0.0 });
    model.objects[1]->instances.front()->set_offset({ 120.0, 100.0, 0.0 });
    return model;
}

SliceResult slice_to_gcode(const Model &model, const DynamicPrintConfig &config)
{
    Print print;
    print.apply(model, config);

    StringObjectException warning;
    StringObjectException error = print.validate(&warning);
    CAPTURE(error.string);
    CAPTURE(warning.string);
    REQUIRE(error.string.empty());

    print.process();

    size_t support_layer_count = 0;
    REQUIRE_FALSE(print.objects().empty());
    for (const PrintObject *object : print.objects())
        support_layer_count += object->support_layer_count();
    const std::vector<unsigned int> used_extruders = print.extruders(false);
    const std::vector<int> filament_maps = print.get_filament_maps();
    const bool has_wipe_tower = print.has_wipe_tower();
    const bool has_wipe_tower_tool_ordering = print.get_tool_ordering().has_wipe_tower();

    ScopedTemporaryFile temp_gcode(".gcode");
    GCodeProcessorResult result;
    const std::string output_path = print.export_gcode(temp_gcode.string(), &result);

    REQUIRE(output_path == temp_gcode.string());
    REQUIRE(boost::filesystem::exists(temp_gcode.path()));
    REQUIRE(boost::filesystem::file_size(temp_gcode.path()) > 0);

    std::ifstream gcode(temp_gcode.string());
    REQUIRE(gcode.good());

    SliceResult slice_result;
    slice_result.used_extruders = used_extruders;
    slice_result.filament_maps = filament_maps;
    slice_result.has_wipe_tower = has_wipe_tower;
    slice_result.has_wipe_tower_tool_ordering = has_wipe_tower_tool_ordering;
    slice_result.total_toolchanges = print.print_statistics().total_toolchanges;
    slice_result.support_layer_count = support_layer_count;
    for (const auto& layer : print.wipe_tower_data().tool_changes)
        for (const auto& action : layer)
            slice_result.tower_toolchange_count += action.gcode.find("; CP TOOLCHANGE START") != std::string::npos;
    slice_result.content.assign(std::istreambuf_iterator<char>(gcode), std::istreambuf_iterator<char>());
    return slice_result;
}

DynamicPrintConfig support_print_config(SupportType support_type, SupportMaterialStyle support_style)
{
    DynamicPrintConfig config = base_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(support_type));
    config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(support_style));
    config.set_key_value("support_base_pattern", new ConfigOptionEnum<SupportMaterialPattern>(smpRectilinear));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(89));
    config.set_key_value("support_remove_small_overhang", new ConfigOptionBool(false));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    return config;
}

DynamicPrintConfig multi_nozzle_print_config()
{
    DynamicPrintConfig config = base_print_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75, 1.75 }));
    config.set_key_value("filament_colour", new ConfigOptionStrings({ "#FF0000", "#0000FF" }));
    config.set_key_value("default_filament_colour", new ConfigOptionStrings({ "#FF0000", "#0000FF" }));
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings({ "", "" }));
    config.set_key_value("filament_end_gcode", new ConfigOptionStrings({ "", "" }));
    config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PLA-SUPPORT" }));
    config.set_key_value("filament_soluble", new ConfigOptionBools({ false, true }));
    config.set_key_value("extruder_offset", new ConfigOptionPoints({ Vec2d(0.0, 0.0), Vec2d(0.0, 0.0) }));
    config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2 }));
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1, 2 }));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({ 140.0, 140.0, 140.0, 140.0,
                                                                           140.0, 140.0, 140.0, 140.0 }));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({ 0.0, 280.0, 280.0, 0.0,
                                                                           0.0, 280.0, 280.0, 0.0 }));
    config.set_key_value("flush_multiplier", new ConfigOptionFloats({ 0.3, 0.3 }));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    return config;
}

DynamicPrintConfig single_nozzle_multi_material_print_config()
{
    DynamicPrintConfig config = base_print_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75, 1.75 }));
    config.set_key_value("filament_colour", new ConfigOptionStrings({ "#FF0000", "#0000FF" }));
    config.set_key_value("default_filament_colour", new ConfigOptionStrings({ "#FF0000", "#0000FF" }));
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings({ "", "" }));
    config.set_key_value("filament_end_gcode", new ConfigOptionStrings({ "", "" }));
    config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PLA" }));
    config.set_key_value("filament_soluble", new ConfigOptionBools({ false, false }));
    config.set_key_value("extruder_offset", new ConfigOptionPoints({ Vec2d(0.0, 0.0) }));
    config.set_key_value("filament_map", new ConfigOptionInts({ 1, 1 }));
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1 }));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({ 140.0, 140.0, 140.0, 140.0 }));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({ 0.0, 280.0, 280.0, 0.0 }));
    config.set_key_value("flush_multiplier", new ConfigOptionFloats({ 0.3 }));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    return config;
}

DynamicPrintConfig support_with_second_nozzle_print_config()
{
    DynamicPrintConfig config = multi_nozzle_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsGrid));
    config.set_key_value("support_base_pattern", new ConfigOptionEnum<SupportMaterialPattern>(smpRectilinear));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(89));
    config.set_key_value("support_remove_small_overhang", new ConfigOptionBool(false));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_filament", new ConfigOptionInt(2));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(2));
    config.set_key_value("brim_width", new ConfigOptionFloat(0.0));
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    return config;
}

void expect_printable_gcode(const SliceResult &result)
{
    CHECK(result.content.find("G1") != std::string::npos);
    CHECK(result.content.find("; filament used") != std::string::npos);
}

void expect_support_gcode(const SliceResult &result)
{
    CHECK(result.support_layer_count > 0);
    CHECK(result.content.find("; support material extrusion width") != std::string::npos);
    CHECK(result.content.find("; support material\n") != std::string::npos);
}

bool used_extruder(const SliceResult &result, unsigned int extruder_id)
{
    return std::find(result.used_extruders.begin(), result.used_extruders.end(), extruder_id) !=
           result.used_extruders.end();
}

} // namespace

SCENARIO("Slice model to G-code without support", "[slicing][gcode]")
{
    setup_test_dirs();
    Model model = load_obj_model("20mm_cube.obj");

    SliceResult result = slice_to_gcode(model, base_print_config());

    expect_printable_gcode(result);
    CHECK(result.support_layer_count == 0);
}

SCENARIO("Slice model to G-code with normal support", "[slicing][support][gcode]")
{
    setup_test_dirs();
    Model model = load_obj_model("overhang.obj");
    DynamicPrintConfig config = support_print_config(stNormalAuto, smsGrid);

    SliceResult result = slice_to_gcode(model, config);

    expect_printable_gcode(result);
    expect_support_gcode(result);
}

SCENARIO("Slice model to G-code with tree support", "[slicing][support][gcode]")
{
    setup_test_dirs();
    Model model = load_obj_model("overhang.obj");
    DynamicPrintConfig config = support_print_config(stTreeAuto, smsTreeOrganic);

    SliceResult result = slice_to_gcode(model, config);

    expect_printable_gcode(result);
    expect_support_gcode(result);
}

SCENARIO("Slice model to G-code with multiple nozzles", "[slicing][multinozzle][gcode]")
{
    setup_test_dirs();
    Model model = load_two_extruder_model();
    DynamicPrintConfig config = multi_nozzle_print_config();

    SliceResult result = slice_to_gcode(model, config);

    expect_printable_gcode(result);
    REQUIRE(result.used_extruders.size() == 2);
    CHECK(used_extruder(result, 0));
    CHECK(used_extruder(result, 1));
    CHECK(result.content.find("T1") != std::string::npos);
}

SCENARIO("Slice model to G-code with single nozzle multi material", "[slicing][multimaterial][gcode]")
{
    setup_test_dirs();
    Model model = load_two_extruder_model();
    DynamicPrintConfig config = single_nozzle_multi_material_print_config();

    SliceResult result = slice_to_gcode(model, config);

    expect_printable_gcode(result);
    REQUIRE(result.used_extruders.size() == 2);
    CHECK(used_extruder(result, 0));
    CHECK(used_extruder(result, 1));
    REQUIRE(result.filament_maps.size() == 2);
    CHECK(result.filament_maps[0] == 1);
    CHECK(result.filament_maps[1] == 1);
    CHECK_FALSE(result.has_wipe_tower);
    CHECK(result.total_toolchanges > 0);
    CHECK(result.content.find("T1") != std::string::npos);
}

SCENARIO("Slice support material to G-code with dedicated second nozzle", "[slicing][support][multinozzle][gcode]")
{
    setup_test_dirs();
    Model model = load_obj_model("overhang.obj");
    DynamicPrintConfig config = support_with_second_nozzle_print_config();

    SliceResult result = slice_to_gcode(model, config);

    expect_printable_gcode(result);
    expect_support_gcode(result);
    REQUIRE(result.used_extruders.size() == 2);
    CHECK(used_extruder(result, 0));
    CHECK(used_extruder(result, 1));
    CHECK(result.content.find("T1") != std::string::npos);
}


TEST_CASE("changing fiber contour or resin settings preserves slice and walls", "[slicing][ContinuousFiber][hole-contours][resin-fill]")
{
    setup_test_dirs();
    Model model = load_obj_model("20mm_cube.obj");
    auto config = base_print_config();
    Print print;
    print.apply(model, config);
    print.process();
    REQUIRE(print.objects().size() == 1);
    const PrintObject* object = print.objects().front();
    REQUIRE(object->is_step_done(posSlice));
    REQUIRE(object->is_step_done(posPerimeters));
    REQUIRE(object->is_step_done(posPrepareInfill));
    REQUIRE(object->is_step_done(posInfill));
    const auto slice_stamp = object->step_state_with_timestamp(posSlice).timestamp;
    const auto wall_stamp = object->step_state_with_timestamp(posPerimeters).timestamp;
    const auto preparation_stamp = object->step_state_with_timestamp(posPrepareInfill).timestamp;
    const auto fill_stamp = object->step_state_with_timestamp(posInfill).timestamp;

    const auto edit = GENERATE(
        std::make_pair("fiber_contour_include_holes", "1"),
        std::make_pair("fiber_resin_fill_density", "60%"),
        std::make_pair("fiber_resin_fill_line_width", "0.6"),
        std::make_pair("fiber_resin_fill_speed", "35"),
        std::make_pair("fiber_resin_fill_acceleration", "500"));
    INFO(edit.first);
    config.set_deserialize_strict(edit.first, edit.second);
    CHECK(print.apply(model, config) == PrintBase::APPLY_STATUS_INVALIDATED);
    REQUIRE(print.objects().size() == 1);
    REQUIRE(print.objects().front() == object);
    CHECK(object->is_step_done(posSlice));
    CHECK(object->is_step_done(posPerimeters));
    CHECK(object->is_step_done(posPrepareInfill));
    CHECK_FALSE(object->is_step_done(posInfill));
    CHECK_FALSE(object->is_step_done(posSimplifyInfill));
    CHECK_FALSE(print.is_step_done(psGCodeExport));

    print.process();
    CHECK(object->is_step_done(posInfill));
    CHECK(object->step_state_with_timestamp(posInfill).timestamp > fill_stamp);
    CHECK(object->step_state_with_timestamp(posSlice).timestamp == slice_stamp);
    CHECK(object->step_state_with_timestamp(posPerimeters).timestamp == wall_stamp);
    CHECK(object->step_state_with_timestamp(posPrepareInfill).timestamp == preparation_stamp);
}

TEST_CASE("wipe tower exclusion invalidates and restores cached scheduling", "[slicing][wipe-tower-exclusion][cache]")
{
    setup_test_dirs();
    auto model = load_two_extruder_model();
    auto config = multi_nozzle_print_config();
    const auto tower_type = GENERATE(WipeTowerType::Type1, WipeTowerType::Type2);
    config.set_key_value("wipe_tower_type", new ConfigOptionEnum<WipeTowerType>(tower_type));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("filament_soluble", new ConfigOptionBools({false, false}));
    config.set_key_value("wipe_tower_wall_type", new ConfigOptionEnum<WipeTowerWallType>(wtwRectangle));
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    CHECK(print.wipe_tower_data().height == 0.f);
    auto& tower_data = const_cast<WipeTowerData&>(print.wipe_tower_data());
    tower_data.height = std::numeric_limits<float>::quiet_NaN();
    tower_data.clear();
    CHECK(tower_data.height == 0.f);
    print.apply(model, config);
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE_FALSE(print.get_tool_ordering().has_wipe_tower_exclusions());
    const auto original_actions = print.wipe_tower_data().tool_changes;
    config.set_key_value("filament_exclude_from_wipe_tower", new ConfigOptionBools({false, true}));
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(psWipeTower));
    CHECK(print.objects().front()->is_step_done(posSlice));
    auto& fake = const_cast<FakeWipeTower&>(print.get_fake_wipe_tower());
    fake.cone_angle = std::numeric_limits<float>::quiet_NaN();
    fake.rotation_angle = std::numeric_limits<float>::quiet_NaN();
    fake.z_and_depth_pairs.clear();
    print.process();
    REQUIRE(print.get_tool_ordering().has_wipe_tower_exclusions());
    if (fake.outer_wall.empty()) {
        CHECK(std::isfinite(fake.cone_angle));
        CHECK(std::isfinite(fake.rotation_angle));
        CHECK_FALSE(fake.z_and_depth_pairs.empty());
    }
    REQUIRE_FALSE(print.wipe_tower_data().tool_changes.empty());
    for (const auto& layer : print.wipe_tower_data().tool_changes)
        for (const auto& action : layer) CHECK(action.new_tool == 0);
    config.set_key_value("filament_exclude_from_wipe_tower", new ConfigOptionBools({false, false}));
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(psWipeTower));
    CHECK(print.objects().front()->is_step_done(posSlice));
    print.process();
    CHECK_FALSE(print.get_tool_ordering().has_wipe_tower_exclusions());
    bool second_tool_restored = false;
    for (const auto& layer : print.wipe_tower_data().tool_changes)
        for (const auto& action : layer) second_tool_restored |= action.new_tool == 1;
    CHECK(second_tool_restored);
    const auto& restored_actions = print.wipe_tower_data().tool_changes;
    REQUIRE(restored_actions.size() == original_actions.size());
    for (size_t layer = 0; layer < original_actions.size(); ++layer) {
        REQUIRE(restored_actions[layer].size() == original_actions[layer].size());
        for (size_t step = 0; step < original_actions[layer].size(); ++step) {
            const auto& original = original_actions[layer][step];
            const auto& restored = restored_actions[layer][step];
            CHECK_FALSE(restored.prime_only);
            CHECK(restored.initial_tool == original.initial_tool);
            CHECK(restored.new_tool == original.new_tool);
            CHECK(restored.print_z == original.print_z);
            CHECK(restored.gcode == original.gcode);
        }
    }
}

TEST_CASE("wipe tower exclusion retains native infill and object flushing", "[slicing][wipe-tower-exclusion][flushing]")
{
    setup_test_dirs();
    const auto tower_type = GENERATE(WipeTowerType::Type1, WipeTowerType::Type2);
    const std::string destination = GENERATE(std::string("flush_into_infill"), std::string("flush_into_objects"));
    CAPTURE(tower_type, destination);
    auto model = load_two_extruder_model();
    auto* excluded = model.add_object(*model.objects.front());
    excluded->config.set_key_value("extruder", new ConfigOptionInt(3));
    excluded->instances.front()->set_offset({160.0, 100.0, 0.0});
    for (auto* object : model.objects) {
        object->config.set_key_value("flush_into_objects", new ConfigOptionBool(false));
        object->config.set_key_value("flush_into_infill", new ConfigOptionBool(false));
    }
    auto* wiping_object = model.add_object(*model.objects.front());
    wiping_object->instances.front()->set_offset({200.0, 100.0, 0.0});
    auto config = multi_nozzle_print_config();
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75, 1.75}));
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FF0000", "#0000FF", "#00FF00"}));
    config.set_key_value("default_filament_colour", config.option("filament_colour")->clone());
    config.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PLA", "PLA"}));
    config.set_key_value("filament_soluble", new ConfigOptionBools({false, false, false}));
    config.set_key_value("filament_start_gcode", new ConfigOptionStrings({"", "", ""}));
    config.set_key_value("filament_end_gcode", new ConfigOptionStrings({"", "", ""}));
    config.set_key_value("filament_map", new ConfigOptionInts({1, 1, 2}));
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("wipe_tower_type", new ConfigOptionEnum<WipeTowerType>(tower_type));
    config.set_key_value("wipe_tower_wall_type", new ConfigOptionEnum<WipeTowerWallType>(wtwRectangle));
    config.set_key_value("filament_exclude_from_wipe_tower", new ConfigOptionBools({false, false, true}));
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    config.set_key_value("brim_width", new ConfigOptionFloat(0));
    config.set_key_value("purge_in_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0,400,400,400,0,400,400,400,0,
                                                                        0,400,400,400,0,400,400,400,0}));
    config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    config.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    config.set_key_value("top_shell_thickness", new ConfigOptionFloat(0));
    config.set_key_value("bottom_shell_thickness", new ConfigOptionFloat(0));
    config.set_key_value("flush_into_infill", new ConfigOptionBool(false));
    config.set_key_value("flush_into_objects", new ConfigOptionBool(false));
    const auto run = [&](bool flushing) {
        wiping_object->config.set_key_value(destination, new ConfigOptionBool(flushing));
        Print print;
        print.set_plate_origin(Vec3d::Zero());
        print.apply(model, config);
        print.process();
        REQUIRE(print.get_filament_maps() == std::vector<int>{1, 1, 2});
        REQUIRE(print.get_tool_ordering().has_wipe_tower_exclusions());
        size_t overridden_layers = 0;
        for (auto& layer : print.wipe_tower_data().tool_ordering.layer_tools()) {
            overridden_layers += layer.wiping_extrusions().is_anything_overridden();
            for (const auto* object : print.objects()) {
                const auto* object_layer = object->get_layer_at_printz(layer.print_z, EPSILON);
                if (!object_layer) continue;
                for (const auto* region : object_layer->regions())
                    for (const auto* collection : {&region->fills, &region->perimeters})
                        for (const auto* entity : collection->entities) {
                            const auto* extrusion = dynamic_cast<const ExtrusionEntityCollection*>(entity);
                            REQUIRE(extrusion != nullptr);
                            const auto* overrides = layer.wiping_extrusions().get_extruder_overrides(
                                entity, object, layer.extruder(*extrusion, region->region()), object->instances().size());
                            if (overrides)
                                for (int filament : *overrides)
                                    if (filament >= 0) CHECK(filament < 2);
                        }
            }
        }
        CHECK((overridden_layers > 0) == flushing);
        double purge = 0.;
        for (const auto& layer : print.wipe_tower_data().tool_changes)
            for (const auto& action : layer) {
                CHECK(action.new_tool != 2);
                purge += action.purge_volume;
            }
        const auto& used = print.wipe_tower_data().used_filament;
        return tower_type == WipeTowerType::Type1 ? purge : std::accumulate(used.begin(), used.end(), 0.0);
    };
    const double unallocated = run(false);
    CHECK(unallocated > 0.);
    CHECK(run(true) < unallocated);
}

TEST_CASE("wipe tower exclusion preserves pre-tower machine hooks", "[slicing][wipe-tower-exclusion][hooks]")
{
    setup_test_dirs();
    const auto tower_type = GENERATE(WipeTowerType::Type1, WipeTowerType::Type2);
    auto model = load_two_extruder_model();
    auto config = multi_nozzle_print_config();
    config.set_key_value("filament_soluble", new ConfigOptionBools({false, false}));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
    config.set_key_value("wipe_tower_type", new ConfigOptionEnum<WipeTowerType>(tower_type));
    config.set_key_value("wipe_tower_wall_type", new ConfigOptionEnum<WipeTowerWallType>(wtwRectangle));
    config.set_key_value("filament_exclude_from_wipe_tower", new ConfigOptionBools({false, true}));
    config.set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(TimelapseType::tlTraditional));
    config.set_key_value("time_lapse_gcode", new ConfigOptionString(";TEST_TIMELAPSE {layer_num}"));
    config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(true));
    config.set_key_value("wrapping_detection_gcode", new ConfigOptionString(";TEST_WRAPPING {layer_num}"));
    const auto result = slice_to_gcode(model, config);
    const auto first_tower = result.content.find("; WIPE_TOWER_START");
    REQUIRE(first_tower != std::string::npos);
    const auto first_timelapse = result.content.find(";TEST_TIMELAPSE 0\n");
    const auto first_wrapping = result.content.find(";TEST_WRAPPING 0\n");
    CHECK(first_timelapse < first_tower);
    CHECK(first_wrapping < first_tower);
    CHECK(result.content.find(";TEST_WRAPPING 0\n", first_wrapping + 1) == std::string::npos);
    size_t emitted_changes = 0, offset = 0;
    while ((offset = result.content.find("; CP TOOLCHANGE START", offset)) != std::string::npos) {
        ++emitted_changes;
        ++offset;
    }
    CHECK(emitted_changes == result.tower_toolchange_count);
}
