#include <catch2/catch_test_macros.hpp>

#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Project.hpp>
#include <libslicer/v1/Slice.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace libslicer::v1;

namespace {

struct TemporaryRoot {
    std::filesystem::path path;
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(path, error); }
};

ContextOptions integration_options(const std::filesystem::path &root)
{
    return {std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR),
            root / "data", root / "temporary", {},
            {64ull * 1024 * 1024, 256ull * 1024 * 1024, 2'000'000,
             128ull * 1024 * 1024, 5'000'000,
             128ull * 1024 * 1024, 128ull * 1024 * 1024}};
}

MeshData cube_mesh(double size)
{
    return {{{0, 0, 0}, {size, 0, 0}, {size, size, 0}, {0, size, 0},
             {0, 0, size}, {size, 0, size}, {size, size, size}, {0, size, size}},
            {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
             {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
             {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}}};
}

Matrix4d identity_matrix()
{
    return {{{1.0, 0.0, 0.0, 0.0,
              0.0, 1.0, 0.0, 0.0,
              0.0, 0.0, 1.0, 0.0,
              0.0, 0.0, 0.0, 1.0}}};
}

} // namespace

TEST_CASE("Public SDK ProjectBuilder accepts app-owned mesh scene input",
          "[libslicer_sdk][project][builder]")
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot root{std::filesystem::temp_directory_path() /
                       ("libslicer-sdk-builder-" + unique)};
    REQUIRE(std::filesystem::create_directories(root.path));

    auto context = SdkContext::create(integration_options(root.path));
    REQUIRE(context.has_value());
    auto repository = context.value().presets();
    REQUIRE(repository.has_value());
    auto printers = repository.value().list(PresetKind::printer);
    auto processes = repository.value().list(PresetKind::process);
    auto filaments = repository.value().list(PresetKind::filament);
    REQUIRE(printers.has_value());
    REQUIRE(processes.has_value());
    REQUIRE(filaments.has_value());
    REQUIRE_FALSE(printers.value().empty());
    REQUIRE_FALSE(processes.value().empty());
    REQUIRE_FALSE(filaments.value().empty());

    PresetSelection selection{
        {printers.value().front().ref, printers.value().front().revision},
        {processes.value().front().ref, processes.value().front().revision},
        {{filaments.value().front().ref, filaments.value().front().revision}}};

    auto builder = context.value().create_project_builder();
    REQUIRE(builder.has_value());
    REQUIRE(builder.value().set_selected_presets(selection).has_value());
    REQUIRE(builder.value().set_project_filament_map(
        {FilamentMapMode::manual, {ToolId{1}}}).has_value());
    auto plate = builder.value().add_plate("Plate 1");
    REQUIRE(plate.has_value());
    ObjectInput object;
    object.name = "SDK cube";
    object.parts.push_back({"cube", cube_mesh(20.0), {}});
    auto object_id = builder.value().add_object(std::move(object));
    REQUIRE(object_id.has_value());
    auto instance = builder.value().add_instance(plate.value(), object_id.value(),
                                                 identity_matrix());
    REQUIRE(instance.has_value());
    auto project = builder.value().build();
    if (!project.has_value() && !project.diagnostics().empty())
        INFO(project.diagnostics().front().message << " at " <<
             project.diagnostics().front().field);
    REQUIRE(project.has_value());

    auto snapshot = project.value().snapshot();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot.value().plates().size() == 1);
    CHECK(snapshot.value().objects().size() == 1);
    CHECK(snapshot.value().parts().size() == 1);
    auto instances = snapshot.value().instances(plate.value());
    REQUIRE(instances.has_value());
    REQUIRE(instances.value().size() == 1);
    CHECK(instances.value().front() == instance.value());

    auto engine = context.value().create_slice_engine();
    REQUIRE(engine.has_value());
    auto inspection = engine.value().inspect({snapshot.value(), plate.value(), std::nullopt});
    if (!inspection.has_value() && !inspection.diagnostics().empty())
        INFO(inspection.diagnostics().front().message << " at " <<
             inspection.diagnostics().front().field);
    REQUIRE(inspection.has_value());
    CHECK(inspection.value().effective_filament_map.mode == FilamentMapMode::manual);
    CHECK(inspection.value().effective_filament_map.tools == std::vector<ToolId>{ToolId{1}});
}

TEST_CASE("Public SDK loads edits resolves saves and slices an Orca FFF project",
          "[libslicer_sdk][project][slice][integration]")
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot root{std::filesystem::temp_directory_path() /
                       ("libslicer-sdk-project-" + unique)};
    REQUIRE(std::filesystem::create_directories(root.path));

    auto context = SdkContext::create(integration_options(root.path));
    REQUIRE(context.has_value());
    const auto fixture = std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR) /
                         "calib/pressure_advance/auto_pa_line_single.3mf";
    auto project = Project::load(context.value(), fixture);
    if (!project.has_value() && !project.diagnostics().empty())
        INFO(project.diagnostics().front().message << " at " <<
             project.diagnostics().front().field);
    REQUIRE(project.has_value());
    auto original = project.value().snapshot();
    REQUIRE(original.has_value());
    REQUIRE_FALSE(original.value().plates().empty());
    REQUIRE_FALSE(original.value().objects().empty());
    const std::size_t filament_count =
        original.value().project_selected_presets().filaments.size();
    REQUIRE(filament_count > 0);

    auto engine = context.value().create_slice_engine();
    REQUIRE(engine.has_value());
    const PlateId plate = original.value().plates().front().id;
    auto automatic = engine.value().inspect({original.value(), plate, std::nullopt});
    REQUIRE_FALSE(automatic.has_value());
    CHECK(automatic.error_code() == ErrorCode::unsupported);

    auto edit = project.value().begin_edit(original.value().revision());
    REQUIRE(edit.has_value());
    const FilamentMapOverride manual{
        FilamentMapMode::manual, std::vector<ToolId>(filament_count, ToolId{1})};
    REQUIRE(edit.value().set_project_filament_map(manual).has_value());
    for (const auto &plate : original.value().plates()) {
        auto local = original.value().local_filament_map_override(plate.id);
        REQUIRE(local.has_value());
        if (local.value())
            REQUIRE(edit.value().set_local_filament_map_override(plate.id, manual).has_value());
    }
    auto committed = edit.value().commit();
    const std::string commit_diagnostic = committed.diagnostics().empty()
        ? "no diagnostic"
        : committed.diagnostics().front().message + " at " +
              committed.diagnostics().front().field;
    INFO(commit_diagnostic);
    REQUIRE(committed.has_value());
    CHECK(committed.value() == original.value().revision() + 1);

    auto snapshot = project.value().snapshot();
    REQUIRE(snapshot.has_value());
    SliceRequest request{snapshot.value(), plate, std::nullopt};
    request.output.preview = PreviewDelivery::memory;
    auto inspection = engine.value().inspect(request);
    REQUIRE(inspection.has_value());
    CHECK(inspection.value().effective_filament_map.mode == FilamentMapMode::manual);
    CHECK(inspection.value().effective_filament_map.tools == manual.tools);
    CHECK(inspection.value().effective_configuration.provenance().project_revision ==
          committed.value());

    const auto saved = root.path / "roundtrip.3mf";
    REQUIRE(project.value().save(saved, committed.value()).has_value());
    auto reloaded = Project::load(context.value(), saved);
    REQUIRE(reloaded.has_value());
    auto reloaded_snapshot = reloaded.value().snapshot();
    REQUIRE(reloaded_snapshot.has_value());
    CHECK(reloaded_snapshot.value().objects().size() == snapshot.value().objects().size());
    CHECK(reloaded_snapshot.value().plates().size() == snapshot.value().plates().size());

    std::vector<SliceEventKind> events;
    auto job = engine.value().submit(request, [&](const SliceEvent &event) {
        events.push_back(event.kind);
    });
    REQUIRE(job.has_value());
    auto busy = engine.value().submit(request);
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error_code() == ErrorCode::busy);
    auto sliced = job.value().wait();
    REQUIRE(sliced.has_value());
    REQUIRE(sliced.value());
    REQUIRE(sliced.value()->gcode_bytes.has_value());
    REQUIRE(sliced.value()->preview);
    CHECK_FALSE(sliced.value()->gcode_bytes->empty());
    CHECK(sliced.value()->gcode_bytes->find("G1") != std::string::npos);
    CHECK(sliced.value()->preview->schema_id == "libslicer.preview");
    CHECK(sliced.value()->preview->coordinate_space == "orca_plate_world_mm");
    CHECK_FALSE(sliced.value()->preview->layers.empty());
    CHECK_FALSE(sliced.value()->preview->filaments.empty());
    CHECK_FALSE(sliced.value()->preview->moves.empty());
    CHECK(sliced.value()->effective_filament_map.mode == FilamentMapMode::manual);
    CHECK(sliced.value()->statistics.layer_count > 0);
    REQUIRE_FALSE(events.empty());
    CHECK(events.front() == SliceEventKind::preparing);
    CHECK(events.back() == SliceEventKind::completed);
    auto repeated = job.value().wait();
    REQUIRE(repeated.has_value());
    CHECK(repeated.value() == sliced.value());
}
