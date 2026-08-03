#include <catch2/catch_test_macros.hpp>

#include <libslicer/Config.hpp>
#include <libslicer/Library.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>
#include <vector>

namespace {
const libslicer::SettingItem* find_item(const std::vector<libslicer::SettingItem>& items, std::string_view key)
{
    for (const auto& item : items) {
        if (item.key == key)
            return &item;
    }
    return nullptr;
}
} // namespace

TEST_CASE("configuration exposes one grouped settings snapshot", "[libslicer_api][config]")
{
    const auto items = libslicer::Config::defaults().settings();
    REQUIRE(items.size() > 100);

    std::set<std::string> unique_keys;
    for (const auto& item : items) {
        CHECK(item.visible);
        CHECK(unique_keys.insert(item.key).second);
    }

    const auto* layer_height = find_item(items, "layer_height");
    REQUIRE(layer_height != nullptr);
    CHECK(layer_height->group == libslicer::SettingGroup::Process);
    CHECK(layer_height->type == libslicer::SettingType::Float);
    CHECK(layer_height->element_type == libslicer::SettingType::Float);
    CHECK(layer_height->precision == 4);
    CHECK_FALSE(layer_height->default_value.empty());

    const auto* nozzle_temperature = find_item(items, "nozzle_temperature");
    REQUIRE(nozzle_temperature != nullptr);
    CHECK(nozzle_temperature->group == libslicer::SettingGroup::Filament);

    const auto* nozzle_diameter = find_item(items, "nozzle_diameter");
    REQUIRE(nozzle_diameter != nullptr);
    CHECK(nozzle_diameter->group == libslicer::SettingGroup::Printer);
    CHECK(nozzle_diameter->type == libslicer::SettingType::List);
    CHECK(nozzle_diameter->element_type == libslicer::SettingType::Float);
    CHECK_FALSE(nozzle_diameter->fixed_size.has_value());

    const auto* bed_type = find_item(items, "curr_bed_type");
    REQUIRE(bed_type != nullptr);
    CHECK(bed_type->group == libslicer::SettingGroup::Process);
    CHECK(bed_type->type == libslicer::SettingType::Enum);
    CHECK(bed_type->enum_items.size() == 6);
    CHECK_FALSE(bed_type->value.empty());
    CHECK(find_item(items, "missing_option") == nullptr);
}

TEST_CASE("configuration edits return only changed items and stay atomic", "[libslicer_api][config]")
{
    auto config         = libslicer::Config::defaults();
    const auto original = config.snapshot().value("layer_height");
    REQUIRE(original.has_value());

    const auto accepted = config.set("layer_height", "0.24");
    REQUIRE(accepted.success);
    REQUIRE(accepted.changed_items.size() == 1);
    CHECK(accepted.changed_items.front().key == "layer_height");
    CHECK(accepted.changed_items.front().value == "0.24");
    CHECK(accepted.changed_items.front().group == libslicer::SettingGroup::Process);
    CHECK(config.snapshot().value("layer_height") == "0.24");

    const auto no_change = config.set("layer_height", "0.24");
    REQUIRE(no_change.success);
    CHECK(no_change.changed_items.empty());

    const auto rejected = config.apply_patch({
        {"layer_height", "0.28"},
        {"missing_option", "1"},
    });
    CHECK_FALSE(rejected.success);
    CHECK(rejected.changed_items.empty());
    CHECK(config.snapshot().value("layer_height") == "0.24");

    CHECK_FALSE(config.set("layer_height", "not-a-number").success);
    CHECK(config.snapshot().value("layer_height") == "0.24");

    const auto plate_change = config.set("curr_bed_type", "Textured PEI Plate");
    REQUIRE(plate_change.success);
    REQUIRE(plate_change.changed_items.size() == 1);
    CHECK(plate_change.changed_items.front().key == "curr_bed_type");
    CHECK(config.snapshot().value("curr_bed_type") == "Textured PEI Plate");

    const auto reset = config.reset("layer_height");
    REQUIRE(reset.success);
    REQUIRE(reset.changed_items.size() == 1);
    CHECK(reset.changed_items.front().key == "layer_height");
    CHECK(reset.changed_items.front().value == original);

    const auto redundant_reset = config.reset("layer_height");
    REQUIRE(redundant_reset.success);
    CHECK(redundant_reset.changed_items.empty());

    const auto missing_reset = config.reset("missing_option");
    CHECK_FALSE(missing_reset.success);
    REQUIRE_FALSE(missing_reset.diagnostics.empty());
    CHECK_FALSE(config.snapshot().value("missing_option").has_value());
}

TEST_CASE("configuration snapshot is independent", "[libslicer_api][config]")
{
    CHECK_FALSE(libslicer::ConfigSnapshot{}.valid());
    auto config = libslicer::Config::defaults();
    REQUIRE(config.set("layer_height", "0.18").success);
    const auto snapshot = config.snapshot();
    CHECK(snapshot.valid());

    REQUIRE(config.set("layer_height", "0.22").success);
    CHECK(snapshot.value("layer_height") == "0.18");
    CHECK(config.snapshot().value("layer_height") == "0.22");
    CHECK_FALSE(snapshot.value("missing_option").has_value());
}

TEST_CASE("library owns machine presets and builds a selected configuration", "[libslicer_api][presets]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};

    std::vector<libslicer::ConfigDiagnostic> diagnostics;
    auto library = libslicer::Library::open(options, &diagnostics);
    const std::string open_diagnostic = diagnostics.empty() ? std::string{} : diagnostics.front().message;
    INFO(open_diagnostic);
    REQUIRE(library != nullptr);
    REQUIRE_FALSE(library->machine_models().empty());
    REQUIRE(library->build_plate_options().size() == 6);
    for (const auto& plate : library->build_plate_options()) {
        CHECK_FALSE(plate.value.empty());
        CHECK_FALSE(plate.name.empty());
        CHECK(std::filesystem::is_regular_file(plate.image_path));
    }

    const auto machine = std::find_if(library->machine_models().begin(), library->machine_models().end(),
                                      [](const libslicer::MachineModelOption& item) {
                                          return item.id == "Flashforge AD5X";
                                      });
    REQUIRE(machine != library->machine_models().end());
    CHECK(std::filesystem::is_regular_file(machine->cover_image_path));
    CHECK_FALSE(machine->bed_model_path.empty());
    CHECK(std::filesystem::is_regular_file(machine->bed_model_path));
    CHECK(std::filesystem::path(machine->bed_texture_path).extension() == ".png");
    CHECK(std::filesystem::is_regular_file(machine->bed_texture_path));
    const auto nozzle = std::find_if(machine->variants.begin(), machine->variants.end(),
                                     [](const libslicer::MachineVariantOption& item) {
                                         return item.id == "0.4";
                                     });
    REQUIRE(nozzle != machine->variants.end());
    CHECK(nozzle->nozzle_diameter == 0.4);
    CHECK(nozzle->printable_width == 220.0);
    CHECK(nozzle->printable_depth == 220.0);
    CHECK(nozzle->printable_height == 220.0);
    CHECK_FALSE(nozzle->printer_preset_id.empty());

    libslicer::ConfigSelection selection;
    selection.machine_model_id = machine->id;
    selection.machine_variant_id = nozzle->id;
    auto created = library->create_config(selection);
    const std::string create_diagnostic = created.diagnostics.empty() ? std::string{} : created.diagnostics.front().message;
    INFO(create_diagnostic);
    REQUIRE(created.success);
    REQUIRE(created.config != nullptr);
    CHECK(created.selection.machine_model_id == "Flashforge AD5X");
    CHECK(created.selection.machine_variant_id == "0.4");
    CHECK_FALSE(created.selection.process_preset_id.empty());
    REQUIRE_FALSE(created.selection.filament_preset_ids.empty());
    CHECK(created.config->snapshot().value("nozzle_diameter") == "0.4");
    CHECK(created.config->snapshot().value("printable_height") == "220");

    selection.process_preset_id = "missing process preset";
    const auto missing_process = library->create_config(selection);
    CHECK_FALSE(missing_process.success);
    REQUIRE_FALSE(missing_process.diagnostics.empty());
    CHECK(missing_process.diagnostics.front().key == "process");

    selection.process_preset_id.clear();
    selection.filament_preset_ids = {"missing filament preset"};
    const auto missing_filament = library->create_config(selection);
    CHECK_FALSE(missing_filament.success);
    REQUIRE_FALSE(missing_filament.diagnostics.empty());
    CHECK(missing_filament.diagnostics.front().key == "filament");
}

TEST_CASE("library slices a model with a preset-backed configuration", "[libslicer_api][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X";
    selection.machine_variant_id = "0.4";
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config != nullptr);

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_20mm_cube.gcode";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    const auto missing_config = library->slice(request);
    CHECK_FALSE(missing_config.success);
    REQUIRE_FALSE(missing_config.diagnostics.empty());
    CHECK(missing_config.diagnostics.front().code == "config");

    request.config = created.config->snapshot();
    request.output_gcode_path = output.string();

    float last_progress = 0.0f;
    libslicer::SliceCallbacks callbacks;
    callbacks.progress = [&last_progress](float progress, std::string_view) {
        CHECK(progress >= 0.0f);
        CHECK(progress <= 1.0f);
        last_progress = std::max(last_progress, progress);
    };
    const auto sliced = library->slice(request, callbacks);
    const std::string diagnostic = sliced.diagnostics.empty() ? std::string{} : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);
    CHECK_FALSE(sliced.cancelled);
    CHECK(sliced.output.path == output.string());
    CHECK(sliced.output.ownership == libslicer::OutputArtifactOwnership::CallerOwned);
    CHECK(std::filesystem::file_size(output) > 0);
    CHECK(sliced.summary.layer_count > 0);
    CHECK(sliced.summary.logical_motion_count > 0);
    CHECK(sliced.summary.render_segment_count > 0);
    REQUIRE(sliced.preview != nullptr);
    CHECK(sliced.preview->statistics.total_layers == sliced.summary.layer_count);
    CHECK(sliced.preview->statistics.logical_motion_count == sliced.summary.logical_motion_count);
    CHECK(sliced.preview->statistics.render_segment_count == sliced.summary.render_segment_count);
    CHECK(sliced.preview->schema_version == libslicer::toolpath_schema_version);
    CHECK_FALSE(sliced.preview->segments.empty());
    CHECK(sliced.preview->bounds.valid);
    CHECK_FALSE(sliced.preview->layers.empty());
    CHECK_FALSE(sliced.preview->colors.empty());
    CHECK(last_progress == 1.0f);

    const auto count_extrusion_role = [](const libslicer::ToolpathPreview& preview,
                                         libslicer::ToolpathExtrusionRole role) {
        return std::count_if(preview.segments.begin(), preview.segments.end(), [role](const auto& segment) {
            return segment.motion == libslicer::ToolpathMotionKind::Extrusion &&
                segment.extrusion_role == role && segment.extrusion_delta_mm > 0.0f;
        });
    };
    const auto sliced_sparse_infill = count_extrusion_role(
        *sliced.preview, libslicer::ToolpathExtrusionRole::SparseInfill);
    const auto sliced_solid_infill = count_extrusion_role(
        *sliced.preview, libslicer::ToolpathExtrusionRole::InternalSolidInfill);
    CHECK(sliced_sparse_infill > 0);
    CHECK(sliced_solid_infill > 0);
    CHECK(std::all_of(sliced.preview->segments.begin(), sliced.preview->segments.end(), [](const auto& segment) {
        const auto finite = [](float value) { return std::isfinite(value); };
        const bool endpoints_valid = finite(segment.start_mm.x) && finite(segment.start_mm.y) &&
            finite(segment.start_mm.z) && finite(segment.end_mm.x) && finite(segment.end_mm.y) &&
            finite(segment.end_mm.z);
        const bool is_extrusion = segment.motion == libslicer::ToolpathMotionKind::Extrusion;
        const bool role_valid = is_extrusion ||
            segment.extrusion_role == libslicer::ToolpathExtrusionRole::None;
        const bool dimensions_valid = is_extrusion ||
            (segment.width_mm == 0.0f && segment.height_mm == 0.0f &&
             segment.mm3_per_mm == 0.0f && segment.extrusion_delta_mm == 0.0f);
        return endpoints_valid && role_valid && dimensions_valid;
    }));
    for (std::size_t index = 1; index < sliced.preview->segments.size(); ++index) {
        const auto& previous = sliced.preview->segments[index - 1];
        const auto& current = sliced.preview->segments[index];
        if (previous.run_id != current.run_id)
            continue;
        CHECK(previous.layer_index == current.layer_index);
        CHECK(previous.motion == current.motion);
        CHECK(previous.extrusion_role == current.extrusion_role);
        CHECK(std::abs(previous.end_mm.x - current.start_mm.x) < 0.000001f);
        CHECK(std::abs(previous.end_mm.y - current.start_mm.y) < 0.000001f);
        CHECK(std::abs(previous.end_mm.z - current.start_mm.z) < 0.000001f);
    }
    for (std::size_t layer_index = 0; layer_index < sliced.preview->layers.size(); ++layer_index) {
        const auto& layer = sliced.preview->layers[layer_index];
        CHECK(layer.index == layer_index);
        CHECK(layer.segment_begin + layer.segment_count <= sliced.preview->segments.size());
        CHECK(layer.event_begin + layer.event_count <= sliced.preview->events.size());
        if (layer_index + 1 < sliced.preview->layers.size()) {
            CHECK(layer.segment_begin + layer.segment_count ==
                  sliced.preview->layers[layer_index + 1].segment_begin);
        }
    }
    const auto sparse_stats = std::find_if(
        sliced.preview->statistics.features.begin(), sliced.preview->statistics.features.end(),
        [](const auto& feature) {
            return feature.role == libslicer::ToolpathExtrusionRole::SparseInfill;
        });
    REQUIRE(sparse_stats != sliced.preview->statistics.features.end());
    CHECK(sparse_stats->path_count > 0);
    CHECK(sparse_stats->length_mm > 0.0);
    CHECK(sparse_stats->extrusion_volume_mm3 > 0.0);

    std::ifstream gcode(output);
    const std::string content((std::istreambuf_iterator<char>(gcode)), std::istreambuf_iterator<char>());
    CHECK(content.find("G1") != std::string::npos);

    libslicer::GCodePreviewRequest preview_request;
    preview_request.gcode_path = output.string();
    const auto imported = library->load_gcode_preview(preview_request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->statistics.total_layers == sliced.preview->statistics.total_layers);
    CHECK(imported.preview->statistics.render_segment_count > 0);
    CHECK(imported.preview->source_path == output.string());
    CHECK(count_extrusion_role(*imported.preview,
                               libslicer::ToolpathExtrusionRole::SparseInfill) > 0);
    CHECK(count_extrusion_role(*imported.preview,
                               libslicer::ToolpathExtrusionRole::InternalSolidInfill) > 0);
    std::filesystem::remove(output, remove_error);
}
