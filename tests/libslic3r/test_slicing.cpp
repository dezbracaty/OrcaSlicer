#include <algorithm>

#include "libslic3r/GCode/GCodeProcessor.hpp"
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
    std::vector<int>          processor_filament_to_tool_map;
    size_t                    processor_preview_color_count { 0 };
    bool                      has_wipe_tower { false };
    bool                      has_wipe_tower_tool_ordering { false };
    int                       total_toolchanges { 0 };
    size_t                    support_layer_count { 0 };
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
    slice_result.processor_filament_to_tool_map = result.filament_to_tool_map;
    slice_result.processor_preview_color_count = result.preview_colors.size();
    slice_result.has_wipe_tower = has_wipe_tower;
    slice_result.has_wipe_tower_tool_ordering = has_wipe_tower_tool_ordering;
    slice_result.total_toolchanges = print.print_statistics().total_toolchanges;
    slice_result.support_layer_count = support_layer_count;
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
    REQUIRE(result.processor_filament_to_tool_map.size() == 2);
    CHECK(result.processor_filament_to_tool_map[0] == 0);
    CHECK(result.processor_filament_to_tool_map[1] == 0);
    CHECK(result.processor_preview_color_count >= 2);
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
