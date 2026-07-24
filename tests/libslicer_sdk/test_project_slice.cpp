#include <catch2/catch_test_macros.hpp>

#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Project.hpp>
#include <libslicer/v1/Slice.hpp>

#include "SliceTesting.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

ConfigPatch percent_patch(const std::string &option, double value)
{
    ConfigPatch patch;
    patch.set(OptionId(option), ConfigValue::percent(value));
    return patch;
}

struct RemapProject {
    Project project;
    PlateId plate;
};

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
    auto plate = builder.value().add_plate("Plate 1");
    REQUIRE(plate.has_value());
    ObjectInput object;
    object.name = "SDK cube";
    object.parts.push_back({"cube", cube_mesh(20.0), {}});
    auto object_id = builder.value().add_object(std::move(object));
    REQUIRE(object_id.has_value());
    REQUIRE(builder.value().set_layer_ranges(
        object_id.value(), {LayerRange{0.0, 10.0, ConfigPatch{}}}).has_value());
    auto instance = builder.value().add_instance(plate.value(), object_id.value(),
                                                 identity_matrix());
    REQUIRE(instance.has_value());
    auto missing_map = builder.value().build();
    REQUIRE_FALSE(missing_map.has_value());
    CHECK(missing_map.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(missing_map.diagnostics().empty());
    CHECK(missing_map.diagnostics().front().field == "/filament_map");
    REQUIRE(builder.value().set_project_filament_map(
        {FilamentMapMode::manual, {}}).has_value());
    auto incomplete_builder_map = builder.value().build();
    REQUIRE_FALSE(incomplete_builder_map.has_value());
    CHECK(incomplete_builder_map.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(incomplete_builder_map.diagnostics().empty());
    CHECK(incomplete_builder_map.diagnostics().front().field ==
          "/filament_map/tools");
    REQUIRE(builder.value().set_project_filament_map(
        {FilamentMapMode::manual, {ToolId{1}}}).has_value());
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
    CHECK(inspection.value().effective_configuration.values().entries().size() > 100);

    SliceRequest temporary_request{snapshot.value(), plate.value(),
                                   TemporarySliceSelection{
                                       selection,
                                       {FilamentMapMode::manual, {ToolId{1}}}}};
    auto temporary = engine.value().inspect(temporary_request);
    REQUIRE(temporary.has_value());
    CHECK(temporary.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::temporary);
    CHECK(temporary.value().effective_filament_map.tools ==
          std::vector<ToolId>{ToolId{1}});
    for (FilamentMapMode mode :
         {FilamentMapMode::auto_for_flush, FilamentMapMode::auto_for_match}) {
        SliceRequest invalid_temporary{
            snapshot.value(), plate.value(),
            TemporarySliceSelection{selection, {mode, {ToolId{1}}}}};
        auto inspected = engine.value().inspect(invalid_temporary);
        REQUIRE_FALSE(inspected.has_value());
        CHECK(inspected.error_code() == ErrorCode::invalid_argument);
        REQUIRE_FALSE(inspected.diagnostics().empty());
        CHECK(inspected.diagnostics().front().field ==
              "/temporary_selection/complete_manual_map/mode");
        auto submitted = engine.value().submit(invalid_temporary);
        REQUIRE_FALSE(submitted.has_value());
        CHECK(submitted.error_code() == ErrorCode::invalid_argument);
        REQUIRE_FALSE(submitted.diagnostics().empty());
        CHECK(submitted.diagnostics().front().field ==
              "/temporary_selection/complete_manual_map/mode");
    }
    SliceRequest incomplete_temporary{
        snapshot.value(), plate.value(),
        TemporarySliceSelection{
            selection, {FilamentMapMode::manual, {}}}};
    auto incomplete = engine.value().inspect(incomplete_temporary);
    REQUIRE_FALSE(incomplete.has_value());
    CHECK(incomplete.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(incomplete.diagnostics().empty());
    CHECK(incomplete.diagnostics().front().field ==
          "/temporary_selection/complete_manual_map/tools");

    auto edit = project.value().begin_edit(snapshot.value().revision());
    REQUIRE(edit.has_value());
    REQUIRE(edit.value().set_project_overrides(
        percent_patch("sparse_infill_density", 15.0)).has_value());
    REQUIRE(edit.value().set_plate_overrides(
        plate.value(), percent_patch("sparse_infill_density", 25.0)).has_value());
    auto override_commit = edit.value().commit();
    REQUIRE(override_commit.has_value());
    auto overridden_snapshot = project.value().snapshot();
    REQUIRE(overridden_snapshot.has_value());

    SliceRequest overridden_request{overridden_snapshot.value(), plate.value(),
                                    std::nullopt};
    auto persisted_overrides = engine.value().inspect(overridden_request);
    REQUIRE(persisted_overrides.has_value());
    CHECK(persisted_overrides.value().effective_configuration.get(
        OptionId("sparse_infill_density")) == ConfigValue::percent(25.0));

    auto project_auto_edit = project.value().begin_edit(
        overridden_snapshot.value().revision());
    REQUIRE(project_auto_edit.has_value());
    REQUIRE(project_auto_edit.value().set_project_filament_map(
        {FilamentMapMode::auto_for_flush, {}}).has_value());
    auto project_auto_revision = project_auto_edit.value().commit();
    REQUIRE(project_auto_revision.has_value());
    auto project_auto_snapshot = project.value().snapshot();
    REQUIRE(project_auto_snapshot.has_value());
    auto project_auto = engine.value().inspect(
        {project_auto_snapshot.value(), plate.value(), std::nullopt});
    REQUIRE(project_auto.has_value());
    CHECK(project_auto.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::project);
    CHECK(project_auto.value().effective_filament_map.mode ==
          FilamentMapMode::auto_for_flush);
    CHECK(project_auto.value().effective_filament_map.tools.empty());
    CHECK(std::count_if(
        project_auto.diagnostics().begin(), project_auto.diagnostics().end(),
        [](const Diagnostic &diagnostic) {
            return diagnostic.code == ErrorCode::unsupported &&
                   diagnostic.severity == Severity::warning &&
                   diagnostic.field == "/filament_map/mode";
        }) == 1);

    auto plate_auto_edit = project.value().begin_edit(
        project_auto_snapshot.value().revision());
    REQUIRE(plate_auto_edit.has_value());
    REQUIRE(plate_auto_edit.value().set_local_filament_map_override(
        plate.value(),
        FilamentMapOverride{FilamentMapMode::auto_for_match, {}}).has_value());
    REQUIRE(plate_auto_edit.value().commit().has_value());
    auto plate_auto_snapshot = project.value().snapshot();
    REQUIRE(plate_auto_snapshot.has_value());
    auto plate_auto = engine.value().inspect(
        {plate_auto_snapshot.value(), plate.value(), std::nullopt});
    REQUIRE(plate_auto.has_value());
    CHECK(plate_auto.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::plate);
    CHECK(plate_auto.value().effective_filament_map.mode ==
          FilamentMapMode::auto_for_match);
    CHECK(plate_auto.value().effective_filament_map.tools.empty());
    CHECK(std::count_if(
        plate_auto.diagnostics().begin(), plate_auto.diagnostics().end(),
        [](const Diagnostic &diagnostic) {
            return diagnostic.code == ErrorCode::unsupported &&
                   diagnostic.severity == Severity::warning &&
                   diagnostic.field == "/filament_map/mode";
        }) == 1);

    auto clear_plate_edit = project.value().begin_edit(
        plate_auto_snapshot.value().revision());
    REQUIRE(clear_plate_edit.has_value());
    REQUIRE(clear_plate_edit.value().set_local_filament_map_override(
        plate.value(), std::nullopt).has_value());
    REQUIRE(clear_plate_edit.value().commit().has_value());
    auto inherited_snapshot = project.value().snapshot();
    REQUIRE(inherited_snapshot.has_value());
    auto inherited = engine.value().inspect(
        {inherited_snapshot.value(), plate.value(), std::nullopt});
    REQUIRE(inherited.has_value());
    CHECK(inherited.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::project);
    CHECK(inherited.value().effective_filament_map.mode ==
          FilamentMapMode::auto_for_flush);

    auto auto_builder = context.value().create_project_builder();
    REQUIRE(auto_builder.has_value());
    REQUIRE(auto_builder.value().set_selected_presets(selection).has_value());
    auto auto_plate = auto_builder.value().add_plate("Auto Plate");
    REQUIRE(auto_plate.has_value());
    ObjectInput auto_object;
    auto_object.name = "Auto map cube";
    auto_object.parts.push_back({"cube", cube_mesh(5.0), {}});
    auto auto_object_id = auto_builder.value().add_object(std::move(auto_object));
    REQUIRE(auto_object_id.has_value());
    REQUIRE(auto_builder.value().add_instance(
        auto_plate.value(), auto_object_id.value(), identity_matrix()).has_value());
    REQUIRE(auto_builder.value().set_project_filament_map(
        {FilamentMapMode::auto_for_match, {ToolId{1}, ToolId{1}}}).has_value());
    auto invalid_auto_project = auto_builder.value().build();
    REQUIRE_FALSE(invalid_auto_project.has_value());
    CHECK(invalid_auto_project.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(invalid_auto_project.diagnostics().empty());
    CHECK(invalid_auto_project.diagnostics().front().field ==
          "/filament_map/tools");
    REQUIRE(auto_builder.value().set_project_filament_map(
        {FilamentMapMode::auto_for_match, {}}).has_value());
    auto auto_project = auto_builder.value().build();
    REQUIRE(auto_project.has_value());
    auto auto_snapshot = auto_project.value().snapshot();
    REQUIRE(auto_snapshot.has_value());
    SliceRequest auto_request{
        auto_snapshot.value(), auto_plate.value(), std::nullopt};
    auto auto_builder_inspection = engine.value().inspect(auto_request);
    REQUIRE(auto_builder_inspection.has_value());
    CHECK(auto_builder_inspection.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::project);
    CHECK(auto_builder_inspection.value().effective_filament_map.mode ==
          FilamentMapMode::auto_for_match);
    CHECK(auto_builder_inspection.value().effective_filament_map.tools.empty());
    CHECK(std::count_if(
        auto_builder_inspection.diagnostics().begin(),
        auto_builder_inspection.diagnostics().end(),
        [](const Diagnostic &diagnostic) {
            return diagnostic.code == ErrorCode::unsupported &&
                   diagnostic.severity == Severity::warning &&
                   diagnostic.field == "/filament_map/mode";
        }) == 1);

    const auto auto_project_path = root.path / "auto-project-empty.3mf";
    REQUIRE(auto_project.value().save(
        auto_project_path, auto_snapshot.value().revision()).has_value());
    auto reloaded_auto_project = Project::load(context.value(), auto_project_path);
    REQUIRE(reloaded_auto_project.has_value());
    auto reloaded_auto_snapshot = reloaded_auto_project.value().snapshot();
    REQUIRE(reloaded_auto_snapshot.has_value());
    CHECK(reloaded_auto_snapshot.value().project_filament_map().mode ==
          FilamentMapMode::auto_for_match);
    CHECK(reloaded_auto_snapshot.value().project_filament_map().tools.empty());
    REQUIRE(reloaded_auto_snapshot.value().plates().size() == 1);
    const PlateId reloaded_auto_plate =
        reloaded_auto_snapshot.value().plates().front().id;
    auto reloaded_project_local =
        reloaded_auto_snapshot.value().local_filament_map_override(
            reloaded_auto_plate);
    REQUIRE(reloaded_project_local.has_value());
    CHECK_FALSE(reloaded_project_local.value().has_value());
    auto reloaded_project_effective =
        reloaded_auto_snapshot.value().effective_filament_map(
            reloaded_auto_plate);
    REQUIRE(reloaded_project_effective.has_value());
    CHECK(reloaded_project_effective.value().mode ==
          FilamentMapMode::auto_for_match);
    CHECK(reloaded_project_effective.value().tools.empty());
    CHECK(reloaded_project_effective.value().source ==
          EffectiveFilamentMap::Source::project);

    auto auto_plate_edit = auto_project.value().begin_edit(
        auto_snapshot.value().revision());
    REQUIRE(auto_plate_edit.has_value());
    REQUIRE(auto_plate_edit.value().set_local_filament_map_override(
        auto_plate.value(),
        FilamentMapOverride{FilamentMapMode::auto_for_flush, {}}).has_value());
    auto auto_plate_revision = auto_plate_edit.value().commit();
    REQUIRE(auto_plate_revision.has_value());
    auto auto_plate_snapshot = auto_project.value().snapshot();
    REQUIRE(auto_plate_snapshot.has_value());
    const auto auto_plate_path = root.path / "auto-plate-empty.3mf";
    REQUIRE(auto_project.value().save(
        auto_plate_path, auto_plate_revision.value()).has_value());
    auto reloaded_auto_plate_project = Project::load(
        context.value(), auto_plate_path);
    REQUIRE(reloaded_auto_plate_project.has_value());
    auto reloaded_auto_plate_snapshot =
        reloaded_auto_plate_project.value().snapshot();
    REQUIRE(reloaded_auto_plate_snapshot.has_value());
    CHECK(reloaded_auto_plate_snapshot.value().project_filament_map().mode ==
          FilamentMapMode::auto_for_match);
    CHECK(reloaded_auto_plate_snapshot.value().project_filament_map().tools.empty());
    REQUIRE(reloaded_auto_plate_snapshot.value().plates().size() == 1);
    const PlateId reloaded_plate_id =
        reloaded_auto_plate_snapshot.value().plates().front().id;
    auto reloaded_plate_local =
        reloaded_auto_plate_snapshot.value().local_filament_map_override(
            reloaded_plate_id);
    REQUIRE(reloaded_plate_local.has_value());
    REQUIRE(reloaded_plate_local.value().has_value());
    CHECK(reloaded_plate_local.value()->mode ==
          FilamentMapMode::auto_for_flush);
    CHECK(reloaded_plate_local.value()->tools.empty());
    auto reloaded_plate_effective =
        reloaded_auto_plate_snapshot.value().effective_filament_map(
            reloaded_plate_id);
    REQUIRE(reloaded_plate_effective.has_value());
    CHECK(reloaded_plate_effective.value().mode ==
          FilamentMapMode::auto_for_flush);
    CHECK(reloaded_plate_effective.value().tools.empty());
    CHECK(reloaded_plate_effective.value().source ==
          EffectiveFilamentMap::Source::plate);

    auto auto_builder_submit = engine.value().submit(auto_request);
    REQUIRE_FALSE(auto_builder_submit.has_value());
    CHECK(auto_builder_submit.error_code() == ErrorCode::unsupported);
    REQUIRE_FALSE(auto_builder_submit.diagnostics().empty());
    CHECK(auto_builder_submit.diagnostics().front().field == "/filament_map/mode");
}

TEST_CASE("Auto filament map remapping preserves prefixes rejects gaps and honors explicit maps",
          "[libslicer_sdk][project][filament_map][remap]")
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot root{std::filesystem::temp_directory_path() /
                       ("libslicer-sdk-map-remap-" + unique)};
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

    const SelectedPreset selected_printer{
        printers.value().front().ref, printers.value().front().revision};
    const SelectedPreset selected_process{
        processes.value().front().ref, processes.value().front().revision};
    const SelectedPreset selected_filament{
        filaments.value().front().ref, filaments.value().front().revision};
    const auto selection_with_slots = [&](std::size_t slots) {
        return PresetSelection{
            selected_printer, selected_process,
            std::vector<SelectedPreset>(slots, selected_filament)};
    };
    const auto build_project =
        [&](FilamentMapOverride project_map,
            std::optional<FilamentMapOverride> local_map = std::nullopt) {
            auto builder = context.value().create_project_builder();
            REQUIRE(builder.has_value());
            REQUIRE(builder.value().set_selected_presets(
                selection_with_slots(2)).has_value());
            REQUIRE(builder.value().set_project_filament_map(
                std::move(project_map)).has_value());
            auto plate = builder.value().add_plate("Remap plate");
            REQUIRE(plate.has_value());
            ObjectInput object;
            object.name = "Remap cube";
            object.parts.push_back({"cube", cube_mesh(6.0), {}});
            auto object_id = builder.value().add_object(std::move(object));
            REQUIRE(object_id.has_value());
            REQUIRE(builder.value().add_instance(
                plate.value(), object_id.value(), identity_matrix()).has_value());
            auto project = builder.value().build();
            REQUIRE(project.has_value());
            if (local_map) {
                auto snapshot = project.value().snapshot();
                REQUIRE(snapshot.has_value());
                auto edit = project.value().begin_edit(snapshot.value().revision());
                REQUIRE(edit.has_value());
                REQUIRE(edit.value().set_local_filament_map_override(
                    plate.value(), std::move(local_map)).has_value());
                REQUIRE(edit.value().commit().has_value());
            }
            return RemapProject{project.value(), plate.value()};
        };
    const auto require_project_map =
        [](Project &project, FilamentMapMode mode,
           const std::vector<ToolId> &tools) {
            auto snapshot = project.snapshot();
            REQUIRE(snapshot.has_value());
            const auto map = snapshot.value().project_filament_map();
            CHECK(map.mode == mode);
            CHECK(map.tools == tools);
            CHECK(std::none_of(map.tools.begin(), map.tools.end(),
                               [](ToolId tool) { return tool.value == 0; }));
            return snapshot.value();
        };
    const auto require_local_map =
        [](Project &project, PlateId plate, FilamentMapMode mode,
           const std::vector<ToolId> &tools) {
            auto snapshot = project.snapshot();
            REQUIRE(snapshot.has_value());
            auto map = snapshot.value().local_filament_map_override(plate);
            REQUIRE(map.has_value());
            REQUIRE(map.value().has_value());
            CHECK(map.value()->mode == mode);
            CHECK(map.value()->tools == tools);
            CHECK(std::none_of(
                map.value()->tools.begin(), map.value()->tools.end(),
                [](ToolId tool) { return tool.value == 0; }));
            return snapshot.value();
        };

    auto persistent = build_project(
        {FilamentMapMode::auto_for_flush, {ToolId{1}}});
    auto snapshot = require_project_map(
        persistent.project, FilamentMapMode::auto_for_flush, {ToolId{1}});
    auto add_slot = persistent.project.begin_edit(snapshot.revision());
    REQUIRE(add_slot.has_value());
    REQUIRE(add_slot.value().set_project_selected_presets(
        selection_with_slots(3),
        SlotRemap{{FilamentSlotId{0}, FilamentSlotId{1}}}).has_value());
    REQUIRE(add_slot.value().commit().has_value());
    snapshot = require_project_map(
        persistent.project, FilamentMapMode::auto_for_flush, {ToolId{1}});

    auto delete_slot = persistent.project.begin_edit(snapshot.revision());
    REQUIRE(delete_slot.has_value());
    REQUIRE(delete_slot.value().set_project_selected_presets(
        selection_with_slots(2),
        SlotRemap{{FilamentSlotId{0}, std::nullopt,
                   FilamentSlotId{1}}}).has_value());
    REQUIRE(delete_slot.value().commit().has_value());
    snapshot = require_project_map(
        persistent.project, FilamentMapMode::auto_for_flush, {ToolId{1}});

    const ProjectRevision before_gap_revision = snapshot.revision();
    const FilamentMapOverride before_gap_map =
        snapshot.project_filament_map();
    auto gap = persistent.project.begin_edit(before_gap_revision);
    REQUIRE(gap.has_value());
    REQUIRE(gap.value().set_project_selected_presets(
        selection_with_slots(3),
        SlotRemap{{FilamentSlotId{2}, FilamentSlotId{0}}}).has_value());
    auto gap_commit = gap.value().commit();
    REQUIRE_FALSE(gap_commit.has_value());
    CHECK(gap_commit.error_code() == ErrorCode::invalid_configuration);
    REQUIRE_FALSE(gap_commit.diagnostics().empty());
    CHECK(gap_commit.diagnostics().front().field == "/filament_map/tools");
    auto after_gap = persistent.project.snapshot();
    REQUIRE(after_gap.has_value());
    CHECK(after_gap.value().revision() == before_gap_revision);
    CHECK(after_gap.value().project_filament_map().mode == before_gap_map.mode);
    CHECK(after_gap.value().project_filament_map().tools == before_gap_map.tools);

    for (bool map_first : {true, false}) {
        CAPTURE(map_first);
        auto explicit_project = build_project(
            {FilamentMapMode::auto_for_flush, {ToolId{1}}});
        auto base = explicit_project.project.snapshot();
        REQUIRE(base.has_value());
        auto edit = explicit_project.project.begin_edit(base.value().revision());
        REQUIRE(edit.has_value());
        const auto set_map = [&] {
            return edit.value().set_project_filament_map(
                {FilamentMapMode::auto_for_match, {ToolId{1}}});
        };
        const auto set_selection = [&] {
            return edit.value().set_project_selected_presets(
                selection_with_slots(3),
                SlotRemap{{FilamentSlotId{2}, FilamentSlotId{0}}});
        };
        if (map_first) {
            REQUIRE(set_map().has_value());
            REQUIRE(set_selection().has_value());
        } else {
            REQUIRE(set_selection().has_value());
            REQUIRE(set_map().has_value());
        }
        REQUIRE(edit.value().commit().has_value());
        require_project_map(explicit_project.project,
                            FilamentMapMode::auto_for_match, {ToolId{1}});
    }

    auto persistent_local = build_project(
        {FilamentMapMode::auto_for_match, {}},
        FilamentMapOverride{FilamentMapMode::auto_for_flush, {ToolId{1}}});
    auto local_snapshot = require_local_map(
        persistent_local.project, persistent_local.plate,
        FilamentMapMode::auto_for_flush, {ToolId{1}});
    auto local_add = persistent_local.project.begin_edit(local_snapshot.revision());
    REQUIRE(local_add.has_value());
    REQUIRE(local_add.value().set_project_selected_presets(
        selection_with_slots(3),
        SlotRemap{{FilamentSlotId{0}, FilamentSlotId{1}}}).has_value());
    REQUIRE(local_add.value().commit().has_value());
    local_snapshot = require_local_map(
        persistent_local.project, persistent_local.plate,
        FilamentMapMode::auto_for_flush, {ToolId{1}});
    auto local_delete =
        persistent_local.project.begin_edit(local_snapshot.revision());
    REQUIRE(local_delete.has_value());
    REQUIRE(local_delete.value().set_project_selected_presets(
        selection_with_slots(2),
        SlotRemap{{FilamentSlotId{0}, std::nullopt,
                   FilamentSlotId{1}}}).has_value());
    REQUIRE(local_delete.value().commit().has_value());
    local_snapshot = require_local_map(
        persistent_local.project, persistent_local.plate,
        FilamentMapMode::auto_for_flush, {ToolId{1}});

    const ProjectRevision before_local_gap_revision = local_snapshot.revision();
    auto local_gap =
        persistent_local.project.begin_edit(before_local_gap_revision);
    REQUIRE(local_gap.has_value());
    REQUIRE(local_gap.value().set_project_selected_presets(
        selection_with_slots(3),
        SlotRemap{{FilamentSlotId{2}, FilamentSlotId{0}}}).has_value());
    auto local_gap_commit = local_gap.value().commit();
    REQUIRE_FALSE(local_gap_commit.has_value());
    CHECK(local_gap_commit.error_code() == ErrorCode::invalid_configuration);
    REQUIRE_FALSE(local_gap_commit.diagnostics().empty());
    CHECK(local_gap_commit.diagnostics().front().field ==
          "/filament_map/tools");
    auto after_local_gap = require_local_map(
        persistent_local.project, persistent_local.plate,
        FilamentMapMode::auto_for_flush, {ToolId{1}});
    CHECK(after_local_gap.revision() == before_local_gap_revision);

    for (bool map_first : {true, false}) {
        CAPTURE(map_first);
        auto explicit_local = build_project(
            {FilamentMapMode::auto_for_match, {}},
            FilamentMapOverride{
                FilamentMapMode::auto_for_flush, {ToolId{1}}});
        auto base = explicit_local.project.snapshot();
        REQUIRE(base.has_value());
        auto edit =
            explicit_local.project.begin_edit(base.value().revision());
        REQUIRE(edit.has_value());
        const auto set_map = [&] {
            return edit.value().set_local_filament_map_override(
                explicit_local.plate,
                FilamentMapOverride{
                    FilamentMapMode::auto_for_match, {ToolId{1}}});
        };
        const auto set_selection = [&] {
            return edit.value().set_project_selected_presets(
                selection_with_slots(3),
                SlotRemap{{FilamentSlotId{2}, FilamentSlotId{0}}});
        };
        if (map_first) {
            REQUIRE(set_map().has_value());
            REQUIRE(set_selection().has_value());
        } else {
            REQUIRE(set_selection().has_value());
            REQUIRE(set_map().has_value());
        }
        REQUIRE(edit.value().commit().has_value());
        require_local_map(explicit_local.project, explicit_local.plate,
                          FilamentMapMode::auto_for_match, {ToolId{1}});
    }
}

TEST_CASE("SliceJob cancellation is deterministic at every internal stage",
          "[libslicer_sdk][job][cancel]")
{
    const auto parse_timeout =
        [](const char *name, std::chrono::milliseconds default_value) {
            const char *raw = std::getenv(name);
            if (!raw) return default_value;
            const std::string_view text(raw);
            unsigned long long value = 0;
            const auto parsed =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != text.data() + text.size() ||
                value > static_cast<unsigned long long>(
                    std::numeric_limits<std::chrono::milliseconds::rep>::max())) {
                FAIL("Invalid nonnegative decimal millisecond value for "
                     << name << ": " << text);
            }
            return std::chrono::milliseconds{
                static_cast<std::chrono::milliseconds::rep>(value)};
        };
    const auto cancel_timeout = parse_timeout(
        "LIBSLICER_TEST_CANCEL_TIMEOUT", std::chrono::milliseconds{5000});
    const auto poll_timeout = parse_timeout(
        "LIBSLICER_TEST_POLL_TIMEOUT", std::chrono::milliseconds{0});
    constexpr auto barrier_timeout = std::chrono::seconds{30};

    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot root{std::filesystem::temp_directory_path() /
                       ("libslicer-sdk-job-cancel-" + unique)};
    REQUIRE(std::filesystem::create_directories(root.path));

    auto context = SdkContext::create(integration_options(root.path));
    REQUIRE(context.has_value());
    const auto fixture = std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR) /
                         "calib/pressure_advance/auto_pa_line_single.3mf";
    auto project = Project::load(context.value(), fixture);
    REQUIRE(project.has_value());
    auto original = project.value().snapshot();
    REQUIRE(original.has_value());
    REQUIRE_FALSE(original.value().plates().empty());
    const PlateId plate = original.value().plates().front().id;
    const std::size_t filament_count =
        original.value().project_selected_presets().filaments.size();
    REQUIRE(filament_count > 0);
    const FilamentMapOverride manual{
        FilamentMapMode::manual,
        std::vector<ToolId>(filament_count, ToolId{1})};
    auto edit = project.value().begin_edit(original.value().revision());
    REQUIRE(edit.has_value());
    REQUIRE(edit.value().set_project_filament_map(manual).has_value());
    for (const auto &candidate : original.value().plates()) {
        auto local =
            original.value().local_filament_map_override(candidate.id);
        REQUIRE(local.has_value());
        if (local.value())
            REQUIRE(edit.value().set_local_filament_map_override(
                candidate.id, manual).has_value());
    }
    REQUIRE(edit.value().commit().has_value());
    auto snapshot = project.value().snapshot();
    REQUIRE(snapshot.has_value());
    const SliceRequest request{
        snapshot.value(), plate, std::nullopt};

    using libslicer::v1::detail::testing::SliceStage;
    using libslicer::v1::detail::testing::StageBarrier;
    struct RecordedEvents {
        std::mutex mutex;
        std::vector<SliceEventKind> values;
    };
    struct BlockedJobCleanup {
        SliceJob &job;
        StageBarrier &barrier;
        std::chrono::milliseconds timeout;
        bool active {true};

        ~BlockedJobCleanup()
        {
            if (!active) return;
            job.cancel();
            barrier.release();
            try {
                job.wait_for(timeout);
            } catch (...) {
            }
        }

        void dismiss() noexcept { active = false; }
    };
    const auto submit_recorded =
        [&](SliceEngine &engine, RecordedEvents &events) {
            return engine.submit(request, [&](const SliceEvent &event) {
                std::lock_guard<std::mutex> lock(events.mutex);
                events.values.push_back(event.kind);
            });
        };
    const auto require_reached =
        [&](SliceJob &job, StageBarrier &barrier) {
            const bool reached =
                barrier.wait_until_reached(barrier_timeout);
            if (!reached) {
                auto terminal =
                    job.wait_for(std::chrono::milliseconds{0});
                if (!terminal.has_value() &&
                    !terminal.diagnostics().empty())
                    INFO("job ended before barrier: "
                         << terminal.diagnostics().front().message << " at "
                         << terminal.diagnostics().front().field);
                REQUIRE(job.cancel().has_value());
                barrier.release();
                job.wait_for(cancel_timeout);
            }
            REQUIRE(reached);
        };
    const auto require_cancelled =
        [&](SliceJob &job, StageBarrier &barrier,
            RecordedEvents &events) {
            auto poll = job.wait_for(poll_timeout);
            REQUIRE(poll.has_value());
            CHECK_FALSE(poll.value().has_value());
            REQUIRE(job.cancel().has_value());
            REQUIRE(job.cancel().has_value());
            barrier.release();

            auto cancelled = job.wait_for(cancel_timeout);
            REQUIRE_FALSE(cancelled.has_value());
            CHECK(cancelled.error_code() == ErrorCode::cancelled);
            REQUIRE(cancelled.diagnostics().size() == 1);
            CHECK(cancelled.diagnostics().front().field == "/job");

            const auto require_same_cancelled =
                [&](const auto &observed) {
                    REQUIRE_FALSE(observed.has_value());
                    CHECK(observed.error_code() == cancelled.error_code());
                    REQUIRE(observed.diagnostics().size() ==
                            cancelled.diagnostics().size());
                    for (std::size_t index = 0;
                         index < cancelled.diagnostics().size(); ++index) {
                        CHECK(observed.diagnostics()[index].code ==
                              cancelled.diagnostics()[index].code);
                        CHECK(observed.diagnostics()[index].severity ==
                              cancelled.diagnostics()[index].severity);
                        CHECK(observed.diagnostics()[index].message ==
                              cancelled.diagnostics()[index].message);
                        CHECK(observed.diagnostics()[index].field ==
                              cancelled.diagnostics()[index].field);
                    }
                };
            auto repeated_wait = job.wait();
            require_same_cancelled(repeated_wait);
            auto repeated_wait_for =
                job.wait_for(std::chrono::milliseconds{0});
            require_same_cancelled(repeated_wait_for);

            SliceJob concurrent_wait_job = job;
            SliceJob concurrent_wait_for_job = job;
            std::optional<Result<std::shared_ptr<const SliceResult>>>
                concurrent_wait;
            std::optional<Result<
                std::optional<std::shared_ptr<const SliceResult>>>>
                concurrent_wait_for;
            std::thread waiter([&] {
                concurrent_wait.emplace(concurrent_wait_job.wait());
            });
            std::thread bounded_waiter([&] {
                concurrent_wait_for.emplace(
                    concurrent_wait_for_job.wait_for(cancel_timeout));
            });
            waiter.join();
            bounded_waiter.join();
            REQUIRE(concurrent_wait.has_value());
            require_same_cancelled(*concurrent_wait);
            REQUIRE(concurrent_wait_for.has_value());
            require_same_cancelled(*concurrent_wait_for);

            std::lock_guard<std::mutex> lock(events.mutex);
            CHECK(std::count(events.values.begin(), events.values.end(),
                             SliceEventKind::cancelled) == 1);
            CHECK(std::count_if(
                events.values.begin(), events.values.end(),
                [](SliceEventKind kind) {
                    return kind == SliceEventKind::completed ||
                           kind == SliceEventKind::failed ||
                           kind == SliceEventKind::cancelled;
                }) == 1);
        };

    {
        CAPTURE("queued");
        StageBarrier blocker_barrier(SliceStage::slicing);
        auto blocker_engine = context.value().create_slice_engine();
        REQUIRE(blocker_engine.has_value());
        auto blocker_result = blocker_engine.value().submit(request);
        REQUIRE(blocker_result.has_value());
        SliceJob blocker_job = blocker_result.value();
        BlockedJobCleanup blocker_cleanup{
            blocker_job, blocker_barrier, cancel_timeout};
        require_reached(blocker_job, blocker_barrier);

        StageBarrier queued_barrier(SliceStage::queued);
        auto target_engine = context.value().create_slice_engine();
        REQUIRE(target_engine.has_value());
        RecordedEvents target_events;
        auto target_result =
            submit_recorded(target_engine.value(), target_events);
        REQUIRE(target_result.has_value());
        SliceJob target_job = target_result.value();
        BlockedJobCleanup target_cleanup{
            target_job, queued_barrier, cancel_timeout};
        require_reached(target_job, queued_barrier);
        require_cancelled(target_job, queued_barrier, target_events);
        target_cleanup.dismiss();

        REQUIRE(blocker_job.cancel().has_value());
        blocker_barrier.release();
        auto blocker_cancelled =
            blocker_job.wait_for(cancel_timeout);
        REQUIRE_FALSE(blocker_cancelled.has_value());
        CHECK(blocker_cancelled.error_code() == ErrorCode::cancelled);
        REQUIRE_FALSE(blocker_cancelled.diagnostics().empty());
        CHECK(blocker_cancelled.diagnostics().front().field == "/job");
        blocker_cleanup.dismiss();
    }

    const std::vector<std::pair<SliceStage, const char *>> stages{
        {SliceStage::preparing, "preparing"},
        {SliceStage::validating, "validating"},
        {SliceStage::slicing, "slicing"},
        {SliceStage::exporting, "exporting"}};

    for (const auto &[stage, name] : stages) {
        CAPTURE(name);
        {
            StageBarrier barrier(stage);
            auto engine = context.value().create_slice_engine();
            REQUIRE(engine.has_value());
            RecordedEvents events;
            auto result = submit_recorded(engine.value(), events);
            REQUIRE(result.has_value());
            SliceJob job = result.value();
            BlockedJobCleanup cleanup{job, barrier, cancel_timeout};
            require_reached(job, barrier);
            require_cancelled(job, barrier, events);
            cleanup.dismiss();
        }
    }
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
    SliceRequest automatic_request{original.value(), plate, std::nullopt};
    auto automatic = engine.value().inspect(automatic_request);
    REQUIRE(automatic.has_value());
    auto original_auto_map = original.value().effective_filament_map(plate);
    REQUIRE(original_auto_map.has_value());
    CHECK(automatic.value().effective_filament_map.mode != FilamentMapMode::manual);
    CHECK(automatic.value().effective_filament_map.tools ==
          original_auto_map.value().tools);
    CHECK(std::count_if(
        automatic.diagnostics().begin(), automatic.diagnostics().end(),
        [](const Diagnostic &diagnostic) {
            return diagnostic.code == ErrorCode::unsupported &&
                   diagnostic.severity == Severity::warning &&
                   diagnostic.field == "/filament_map/mode";
        }) == 1);

    auto automatic_job = engine.value().submit(automatic_request);
    REQUIRE_FALSE(automatic_job.has_value());
    CHECK(automatic_job.error_code() == ErrorCode::unsupported);
    REQUIRE_FALSE(automatic_job.diagnostics().empty());
    CHECK(automatic_job.diagnostics().front().field == "/filament_map/mode");

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
    INFO("reload diagnostics=" << reloaded.diagnostics().size()
         << " error=" << (reloaded.error_code()
                 ? static_cast<int>(*reloaded.error_code()) : -1));
    const std::string reload_diagnostic = reloaded.diagnostics().empty()
        ? "no reload diagnostic"
        : reloaded.diagnostics().front().message + " at " +
              reloaded.diagnostics().front().field;
    INFO(reload_diagnostic);
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
    auto negative_timeout = job.value().wait_for(std::chrono::milliseconds{-1});
    REQUIRE_FALSE(negative_timeout.has_value());
    CHECK(negative_timeout.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(negative_timeout.diagnostics().empty());
    CHECK(negative_timeout.diagnostics().front().field == "/timeout");
    auto busy = engine.value().submit(request);
    REQUIRE_FALSE(busy.has_value());
    CHECK(busy.error_code() == ErrorCode::busy);
    auto sliced = job.value().wait();
    const std::string slice_diagnostic = sliced.diagnostics().empty()
        ? "no diagnostic"
        : sliced.diagnostics().front().message + " at " +
              sliced.diagnostics().front().field;
    INFO(slice_diagnostic);
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
    auto bounded_repeated = job.value().wait_for(std::chrono::milliseconds{0});
    REQUIRE(bounded_repeated.has_value());
    REQUIRE(bounded_repeated.value().has_value());
    CHECK(*bounded_repeated.value() == sliced.value());

    std::optional<Result<std::shared_ptr<const SliceResult>>> concurrent_wait;
    std::optional<Result<std::optional<std::shared_ptr<const SliceResult>>>>
        concurrent_wait_for;
    std::thread waiter([&] { concurrent_wait.emplace(job.value().wait()); });
    std::thread bounded_waiter([&] {
        concurrent_wait_for.emplace(
            job.value().wait_for(std::chrono::milliseconds{0}));
    });
    waiter.join();
    bounded_waiter.join();
    REQUIRE(concurrent_wait.has_value());
    REQUIRE(concurrent_wait->has_value());
    CHECK(concurrent_wait->value() == sliced.value());
    REQUIRE(concurrent_wait_for.has_value());
    REQUIRE(concurrent_wait_for->has_value());
    REQUIRE(concurrent_wait_for->value().has_value());
    CHECK(*concurrent_wait_for->value() == sliced.value());

    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool callback_entered = false;
    bool callback_handle_ready = false;
    bool callback_conflict_observed = false;
    bool callback_release = false;
    std::optional<SliceJob> callback_job_handle;
    auto callback_job = engine.value().submit(
        request, [&](const SliceEvent &event) {
            if (event.kind != SliceEventKind::preparing) return;
            std::unique_lock<std::mutex> lock(callback_mutex);
            callback_entered = true;
            callback_condition.notify_all();
            callback_condition.wait(lock, [&] { return callback_handle_ready; });
            lock.unlock();
            auto nested_wait = callback_job_handle->wait();
            auto nested_wait_for =
                callback_job_handle->wait_for(std::chrono::milliseconds{0});
            auto nested_negative_wait_for =
                callback_job_handle->wait_for(std::chrono::milliseconds{-1});
            lock.lock();
            callback_conflict_observed =
                !nested_wait.has_value() &&
                nested_wait.error_code() == ErrorCode::conflict &&
                !nested_wait.diagnostics().empty() &&
                nested_wait.diagnostics().front().field == "/callback" &&
                !nested_wait_for.has_value() &&
                nested_wait_for.error_code() == ErrorCode::conflict &&
                !nested_wait_for.diagnostics().empty() &&
                nested_wait_for.diagnostics().front().field == "/callback" &&
                !nested_negative_wait_for.has_value() &&
                nested_negative_wait_for.error_code() == ErrorCode::conflict &&
                !nested_negative_wait_for.diagnostics().empty() &&
                nested_negative_wait_for.diagnostics().front().field ==
                    "/callback";
            callback_condition.notify_all();
            callback_condition.wait(lock, [&] { return callback_release; });
        });
    REQUIRE(callback_job.has_value());
    callback_job_handle.emplace(callback_job.value());
    {
        std::unique_lock<std::mutex> lock(callback_mutex);
        callback_handle_ready = true;
        callback_condition.notify_all();
        callback_condition.wait(lock, [&] {
            return callback_entered && callback_conflict_observed;
        });
    }
    auto poll = callback_job.value().wait_for(std::chrono::milliseconds{0});
    REQUIRE(poll.has_value());
    CHECK_FALSE(poll.value().has_value());
    REQUIRE(callback_job.value().cancel().has_value());
    {
        std::lock_guard<std::mutex> lock(callback_mutex);
        callback_release = true;
    }
    callback_condition.notify_all();
    auto cancelled = callback_job.value().wait_for(std::chrono::seconds{30});
    REQUIRE_FALSE(cancelled.has_value());
    CHECK(cancelled.error_code() == ErrorCode::cancelled);
    REQUIRE_FALSE(cancelled.diagnostics().empty());
    CHECK(cancelled.diagnostics().front().field == "/job");
    auto repeated_cancelled = callback_job.value().wait();
    REQUIRE_FALSE(repeated_cancelled.has_value());
    CHECK(repeated_cancelled.error_code() == ErrorCode::cancelled);
    REQUIRE(repeated_cancelled.diagnostics().size() ==
            cancelled.diagnostics().size());
    CHECK(repeated_cancelled.diagnostics().front().code ==
          cancelled.diagnostics().front().code);
    CHECK(repeated_cancelled.diagnostics().front().severity ==
          cancelled.diagnostics().front().severity);
    CHECK(repeated_cancelled.diagnostics().front().message ==
          cancelled.diagnostics().front().message);
    CHECK(repeated_cancelled.diagnostics().front().field ==
          cancelled.diagnostics().front().field);
}

TEST_CASE("Context-created handles keep SDK state alive after SdkContext destruction",
          "[libslicer_sdk][context][lifecycle]")
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot root{std::filesystem::temp_directory_path() /
                       ("libslicer-sdk-context-lease-" + unique)};
    REQUIRE(std::filesystem::create_directories(root.path));

    std::optional<PresetRepository> repository;
    std::optional<Project> project;
    std::optional<ProjectSnapshot> snapshot;
    std::optional<SliceEngine> engine;
    std::optional<PlateId> plate;

    {
        auto context = SdkContext::create(integration_options(root.path));
        REQUIRE(context.has_value());
        auto repository_result = context.value().presets();
        REQUIRE(repository_result.has_value());
        repository.emplace(std::move(repository_result).value());
        const PresetSelection selection = [&] {
            auto printers = repository->list(PresetKind::printer);
            auto processes = repository->list(PresetKind::process);
            auto filaments = repository->list(PresetKind::filament);
            REQUIRE(printers.has_value());
            REQUIRE(processes.has_value());
            REQUIRE(filaments.has_value());
            REQUIRE_FALSE(printers.value().empty());
            REQUIRE_FALSE(processes.value().empty());
            REQUIRE_FALSE(filaments.value().empty());
            return PresetSelection{
                {printers.value().front().ref, printers.value().front().revision},
                {processes.value().front().ref, processes.value().front().revision},
                {{filaments.value().front().ref, filaments.value().front().revision}}};
        }();

        auto builder = context.value().create_project_builder();
        REQUIRE(builder.has_value());
        REQUIRE(builder.value().set_selected_presets(selection).has_value());
        REQUIRE(builder.value().set_project_filament_map(
            {FilamentMapMode::manual, {ToolId{1}}}).has_value());
        auto plate_result = builder.value().add_plate("Plate 1");
        REQUIRE(plate_result.has_value());
        ObjectInput object;
        object.name = "leased cube";
        object.parts.push_back({"cube", cube_mesh(8.0), {}});
        auto object_id = builder.value().add_object(std::move(object));
        REQUIRE(object_id.has_value());
        REQUIRE(builder.value().add_instance(plate_result.value(), object_id.value(),
                                             identity_matrix()).has_value());
        auto project_result = builder.value().build();
        REQUIRE(project_result.has_value());
        project.emplace(std::move(project_result).value());
        auto snapshot_result = project->snapshot();
        REQUIRE(snapshot_result.has_value());
        snapshot.emplace(std::move(snapshot_result).value());
        plate.emplace(plate_result.value());
        auto engine_result = context.value().create_slice_engine();
        REQUIRE(engine_result.has_value());
        engine.emplace(std::move(engine_result).value());
    }

    auto filaments = repository->list(PresetKind::filament);
    REQUIRE(filaments.has_value());
    CHECK_FALSE(filaments.value().empty());

    auto refreshed = project->snapshot();
    REQUIRE(refreshed.has_value());
    CHECK(refreshed.value().revision() == snapshot->revision());

    SliceRequest request{*snapshot, *plate, std::nullopt};
    auto inspection = engine->inspect(request);
    REQUIRE(inspection.has_value());
    CHECK(inspection.value().effective_filament_map.tools ==
          std::vector<ToolId>{ToolId{1}});
}

TEST_CASE("SliceEngine inspect is the stable effective configuration boundary",
          "[libslicer_sdk][slice][inspect][public]")
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot root{std::filesystem::temp_directory_path() /
                       ("libslicer-sdk-inspect-" + unique)};
    REQUIRE(std::filesystem::create_directories(root.path));

    auto context = SdkContext::create(integration_options(root.path));
    REQUIRE(context.has_value());
    auto repository_result = context.value().presets();
    REQUIRE(repository_result.has_value());
    PresetRepository repository = std::move(repository_result).value();
    const PresetSelection selection = [&] {
        auto printers = repository.list(PresetKind::printer);
        auto processes = repository.list(PresetKind::process);
        auto filaments = repository.list(PresetKind::filament);
        REQUIRE(printers.has_value());
        REQUIRE(processes.has_value());
        REQUIRE(filaments.has_value());
        REQUIRE_FALSE(printers.value().empty());
        REQUIRE_FALSE(processes.value().empty());
        REQUIRE_FALSE(filaments.value().empty());
        return PresetSelection{
            {printers.value().front().ref, printers.value().front().revision},
            {processes.value().front().ref, processes.value().front().revision},
            {{filaments.value().front().ref, filaments.value().front().revision}}};
    }();

    auto builder = context.value().create_project_builder();
    REQUIRE(builder.has_value());
    REQUIRE(builder.value().set_selected_presets(selection).has_value());
    auto invalid_map = builder.value().set_project_filament_map(
        {FilamentMapMode::manual, {ToolId{0}}});
    REQUIRE_FALSE(invalid_map.has_value());
    CHECK(invalid_map.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(invalid_map.diagnostics().empty());
    CHECK(invalid_map.diagnostics().front().field == "/filament_map/tools/0");
    REQUIRE(builder.value().set_project_filament_map(
        {FilamentMapMode::manual, {ToolId{1}}}).has_value());
    auto plate = builder.value().add_plate("Plate 1");
    REQUIRE(plate.has_value());
    ObjectInput object;
    object.name = "SDK inspect cube";
    object.parts.push_back({"cube", cube_mesh(10.0), {}});
    auto object_id = builder.value().add_object(std::move(object));
    REQUIRE(object_id.has_value());
    REQUIRE(builder.value().add_instance(plate.value(), object_id.value(),
                                         identity_matrix()).has_value());
    auto project = builder.value().build();
    REQUIRE(project.has_value());
    auto snapshot = project.value().snapshot();
    REQUIRE(snapshot.has_value());

    auto engine = context.value().create_slice_engine();
    REQUIRE(engine.has_value());
    SliceRequest request{snapshot.value(), plate.value(), std::nullopt};
    auto inspection = engine.value().inspect(request);
    REQUIRE(inspection.has_value());
    CHECK(inspection.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::project);
    CHECK(inspection.value().effective_filament_map.tools ==
          std::vector<ToolId>{ToolId{1}});
    CHECK(inspection.value().effective_configuration.provenance().project_revision ==
          snapshot.value().revision());

    SliceRequest temporary{snapshot.value(), plate.value(),
                           TemporarySliceSelection{
                               selection,
                               {FilamentMapMode::manual, {ToolId{1}}}}};
    auto temporary_inspection = engine.value().inspect(temporary);
    REQUIRE(temporary_inspection.has_value());
    CHECK(temporary_inspection.value().effective_filament_map.source ==
          EffectiveFilamentMap::Source::temporary);
}
