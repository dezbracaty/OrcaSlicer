#include "libslic3r/Belt/BeltCoordinateSystem.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

#include <catch2/catch_all.hpp>

using namespace Slic3r;

TEST_CASE("Belt coordinate conversion is reversible", "[belt]")
{
    const BeltCoordinateSystem belt = BeltCoordinateSystem::create(45.0, 250.0);
    const Vec3d world(31.25, 207.5, 18.75);
    const Vec3d oriented = belt.world_to_oriented(world);
    const Vec3d machine = belt.oriented_to_machine(oriented);

    CHECK((belt.oriented_to_world(oriented) - world).norm() < 1e-9);
    CHECK((belt.machine_to_world(machine) - world).norm() < 1e-9);
}

TEST_CASE("Belt layer change emits coupled machine Y and Z", "[belt]")
{
    GCodeWriter writer;
    const BeltCoordinateSystem belt = BeltCoordinateSystem::create(45.0, 250.0);
    writer.set_belt_coordinate_system(&belt);
    writer.set_position(Vec3d(1.0, 2.0, 0.0));

    const std::string gcode = writer.travel_to_z(0.2, "belt layer");

    CHECK(gcode.find("Y2.2") != std::string::npos);
    CHECK(gcode.find("Z.283") != std::string::npos);
}

TEST_CASE("World oriented slicer intersects an oblique plane without mutating mesh", "[belt]")
{
    const TriangleMesh mesh = make_cube(10.0, 10.0, 10.0);
    const auto original_vertices = mesh.its.vertices;
    const BeltCoordinateSystem belt = BeltCoordinateSystem::create(45.0, 10.0);
    MeshSlicingParamsEx params;

    const std::vector<ExPolygons> slices = slice_mesh_ex_oriented(
        mesh.its, {5.0}, Transform3d::Identity(), belt.oriented_slice_frame(), Vec2d::Zero(), params);

    REQUIRE(slices.size() == 1);
    CHECK_FALSE(slices.front().empty());
    for (const ExPolygon& slice : slices.front()) {
        for (const Point& point : slice.contour.points) {
            CHECK(std::abs(unscaled<double>(point.x())) < 20.0);
            CHECK(std::abs(unscaled<double>(point.y())) < 20.0);
        }
    }
    REQUIRE(mesh.its.vertices.size() == original_vertices.size());
    for (std::size_t index = 0; index < original_vertices.size(); ++index)
        CHECK((mesh.its.vertices[index] - original_vertices[index]).norm() == Catch::Approx(0.0));
}
