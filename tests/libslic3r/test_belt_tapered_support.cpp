#include "libslic3r/Belt/BeltCoordinateSystem.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/BeltSupportDebug.hpp"
#include "libslic3r/Utils.hpp"
#include "../test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

using namespace Slic3r;

namespace {

void setup_belt_test_dirs()
{
    const boost::filesystem::path resources_dir =
        boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    Slic3r::set_resources_dir(resources_dir.string());
    Slic3r::set_data_dir(boost::filesystem::temp_directory_path().string());
}

Model load_belt_overhang_model()
{
    Model model;
    ObjInfo info;
    std::string message;
    const std::string path = std::string(TEST_DATA_DIR) + "/overhang.obj";
    REQUIRE(load_obj(path.c_str(), &model, info, message));
    CAPTURE(message);
    REQUIRE(model.add_default_instances());
    model.center_instances_around_point({100.0, 100.0});
    return model;
}

double support_reference_y(const Model& model)
{
    double reference = std::numeric_limits<double>::lowest();
    for (const ModelObject* object : model.objects) {
        for (const ModelInstance* instance : object->instances) {
            for (const ModelVolume* volume : object->volumes) {
                if (!volume->is_model_part())
                    continue;
                const Transform3d transform = instance->get_matrix() * volume->get_matrix();
                for (const Vec3f& vertex : volume->mesh().its.vertices) {
                    const Vec3d world = transform * vertex.cast<double>();
                    reference = std::max(reference, world.y());
                }
            }
        }
    }
    return reference;
}

DynamicPrintConfig belt_support_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));
    config.set_key_value("printer_structure",
                         new ConfigOptionEnum<PrinterStructure>(PrinterStructure::psBelt));
    config.set_key_value("belt_gantry_angle", new ConfigOptionFloat(45.0));
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
    config.set_key_value("support_style",
                         new ConfigOptionEnum<SupportMaterialStyle>(smsTreeOrganic));
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(30));
    config.set_key_value("support_remove_small_overhang", new ConfigOptionBool(false));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("raft_layers", new ConfigOptionInt(0));
    config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btNoBrim));
    config.set_key_value("skirt_loops", new ConfigOptionInt(0));
    config.set_key_value("skirt_height", new ConfigOptionInt(0));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("z_hop", new ConfigOptionFloats({0.0}));
    config.set_key_value("filament_map", new ConfigOptionInts({1}));
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({1}));
    return config;
}

} // namespace

TEST_CASE("Belt contact projects vertically to the world build plate", "[belt][support]")
{
    const BeltCoordinateSystem belt = BeltCoordinateSystem::create(45.0, 250.0);
    const Vec3d contact_world(12.0, 87.0, 31.0);
    const Vec3d contact = belt.world_to_oriented(contact_world);
    const Vec3d expected_root_world(contact_world.x(), contact_world.y(), 0.0);
    const Vec3d root = belt.world_to_oriented(expected_root_world);
    const Vec3d root_world = belt.oriented_to_world(root);

    CHECK(root_world.x() == Catch::Approx(contact_world.x()));
    CHECK(root_world.y() == Catch::Approx(contact_world.y()));
    CHECK(root_world.z() == Catch::Approx(0.0).margin(1e-9));
    CHECK(root.y() == Catch::Approx(belt.belt_boundary_v(root.z())).margin(1e-9));
    CHECK((contact.y() - root.y()) / (contact.z() - root.z()) ==
          Catch::Approx(std::tan(M_PI / 4.0)).margin(1e-9));
}

TEST_CASE("Belt build plate slice line maps to zero world Z and machine Y",
          "[belt][support]")
{
    const BeltCoordinateSystem belt = BeltCoordinateSystem::create(45.0, 250.0);
    const double slice_s = 23.5;
    const double boundary_v = belt.belt_boundary_v(slice_s);
    for (const double u : {-12.0, 19.0}) {
        const Vec3d oriented(u, boundary_v, slice_s);
        CHECK(belt.oriented_to_world(oriented).z() == Catch::Approx(0.0).margin(1e-9));
        CHECK(belt.oriented_to_machine(oriented).y() == Catch::Approx(0.0).margin(1e-9));
    }
}

TEST_CASE("Belt tapered support starts with one open build plate contact path",
          "[belt][support][integration]")
{
    setup_belt_test_dirs();
    Model model = load_belt_overhang_model();
    const auto original_vertices = model.objects.front()->volumes.front()->mesh().its.vertices;
    DynamicPrintConfig config = belt_support_config();
    const double reference_y = support_reference_y(model);
    const BeltCoordinateSystem belt = BeltCoordinateSystem::create(45.0, reference_y);

    Print print;
    print.set_belt_coordinate_system(belt);
    print.apply(model, config);
    StringObjectException warning;
    const StringObjectException validation_error = print.validate(&warning);
    CAPTURE(validation_error.string);
    CAPTURE(warning.string);
    REQUIRE(validation_error.string.empty());
    BeltSupportDebugRecorder support_debug;
    {
        ScopedBeltSupportDebugRecorder debug_scope(&support_debug);
        print.process();
    }

    const auto& final_metrics = support_debug
        .stage(BeltSupportDebugStageId::FinalSupport).metrics;
    REQUIRE(final_metrics.count("final_support_contract_valid") == 1);
    CHECK(final_metrics.at("final_support_contract_valid") == 1.0);
    REQUIRE(final_metrics.count(
        "final_unrooted_structural_component_count") == 1);
    CHECK(final_metrics.at(
        "final_unrooted_structural_component_count") == 0.0);
    REQUIRE(final_metrics.count("required_uncovered_raw_witness_count") == 1);
    CHECK(final_metrics.at("required_uncovered_raw_witness_count") == 0.0);
    const auto& root_stage = support_debug.stage(
        BeltSupportDebugStageId::RootProjection);
    REQUIRE(root_stage.metrics.count("maximum_projection_xy_shift_mm") == 1);
    CHECK(root_stage.metrics.at("maximum_projection_xy_shift_mm") ==
          Catch::Approx(0.0).margin(1e-5));
    REQUIRE(root_stage.metrics.count("maximum_projection_abs_world_z_mm") == 1);
    CHECK(root_stage.metrics.at("maximum_projection_abs_world_z_mm") ==
          Catch::Approx(0.0).margin(1e-5));
    const auto& route_metrics = support_debug.stage(
        BeltSupportDebugStageId::RouteClassification).metrics;
    REQUIRE(route_metrics.count(
        "branch_angle_contract_violation_route_count") == 1);
    CHECK(route_metrics.at(
        "branch_angle_contract_violation_route_count") == 0.0);
    const auto& tree_stage = support_debug.stage(
        BeltSupportDebugStageId::TreeTopology);
    REQUIRE(tree_stage.metrics.count("root_trunk_count") == 1);
    REQUIRE(tree_stage.metrics.count(
        "already_world_vertical_root_count") == 1);
    REQUIRE(tree_stage.metrics.count("world_verticalized_root_count") == 1);
    REQUIRE(tree_stage.metrics.count(
        "world_vertical_root_failure_count") == 1);
    CHECK(tree_stage.metrics.at("world_vertical_root_failure_count") == 0.0);
    CHECK(tree_stage.metrics.at("root_trunk_count") ==
          tree_stage.metrics.at("already_world_vertical_root_count") +
              tree_stage.metrics.at("world_verticalized_root_count"));
    size_t audited_root_trunk_count = 0;
    for (const BeltSupportDebugLine& line : tree_stage.lines) {
        if (line.category != "tree_branch" ||
            std::abs(line.start_world.z()) > 1e-5) {
            continue;
        }
        ++audited_root_trunk_count;
        const double height = line.end_world.z() - line.start_world.z();
        REQUIRE(height > 0.0);
        CHECK(std::hypot(
                  line.end_world.x() - line.start_world.x(),
                  line.end_world.y() - line.start_world.y()) /
              height == Catch::Approx(0.0).margin(1e-5));
    }
    CHECK(static_cast<double>(audited_root_trunk_count) ==
          tree_stage.metrics.at("root_trunk_count"));
    const BeltCoordinateSystem* final_belt = print.belt_coordinate_system();
    REQUIRE(final_belt != nullptr);
    CHECK(final_belt->print_origin_s() == Catch::Approx(0.0).margin(1e-9));

    REQUIRE(print.objects().size() == 1);
    const PrintObject* object = print.objects().front();
    const SupportLayer* first = nullptr;
    for (const SupportLayer* layer : object->support_layers()) {
        if (!layer->support_fills.entities.empty()) {
            first = layer;
            break;
        }
    }
    REQUIRE(first != nullptr);
    REQUIRE(first->support_fills.entities.size() == 1);
    CHECK_FALSE(first->support_fills.entities.front()->is_loop());
    CHECK(first->print_z == Catch::Approx(
        config.opt_float("initial_layer_print_height")).margin(1e-9));
    CHECK(first->slice_z == Catch::Approx(0.5 * first->height).margin(1e-9));

    const Polylines paths = first->support_fills.as_polylines();
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().points.size() >= 2);
    const double center_u = unscaled<double>(object->center_offset().x());
    const double center_v = unscaled<double>(object->center_offset().y());
    const double slice_s = first->slice_z;
    const double print_s =
        first->print_z - object->slicing_parameters().object_print_z_min;
    const double center_offset_s = print_s - slice_s;
    CHECK(center_offset_s == Catch::Approx(0.5 * first->height).margin(1e-9));
    for (const Point& point : paths.front().points) {
        const double u = unscaled<double>(point.x()) + center_u;
        const double v = unscaled<double>(point.y()) + center_v;

        // Root width is sampled at slice_s, while the actual extrusion is
        // emitted at print_s. That emitted path must be the platform line.
        const Vec3d root_extrusion(u, v, print_s);
        CHECK(final_belt->oriented_to_world(root_extrusion).z() ==
              Catch::Approx(0.0).margin(1e-5));
        CHECK(final_belt->oriented_to_machine(root_extrusion).y() ==
              Catch::Approx(0.0).margin(1e-5));
    }

    const auto& vertices_after = model.objects.front()->volumes.front()->mesh().its.vertices;
    REQUIRE(vertices_after.size() == original_vertices.size());
    for (size_t index = 0; index < original_vertices.size(); ++index)
        CHECK((vertices_after[index] - original_vertices[index]).norm() == Catch::Approx(0.0));
}

TEST_CASE("Belt support V1 rejects multiple support-enabled objects",
          "[belt][support]")
{
    setup_belt_test_dirs();
    Model model = load_belt_overhang_model();
    model.add_object(*model.objects.front());
    DynamicPrintConfig config = belt_support_config();
    const double reference_y = support_reference_y(model);

    Print print;
    print.set_belt_coordinate_system(
        BeltCoordinateSystem::create(45.0, reference_y));
    print.apply(model, config);
    StringObjectException warning;
    const StringObjectException validation_error = print.validate(&warning);

    CHECK(validation_error.opt_key == "enable_support");
    CHECK(validation_error.string.find(
              "exactly one support-enabled object") != std::string::npos);
}
