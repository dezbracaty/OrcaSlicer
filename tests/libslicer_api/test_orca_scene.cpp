#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <libslicer/Calibration.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <filesystem>

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
