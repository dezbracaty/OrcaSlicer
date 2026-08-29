#include <catch2/catch_test_macros.hpp>

#include <libslicer/Config.hpp>
#include <libslicer/Library.hpp>

#include <miniz.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
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

std::vector<unsigned char> read_zip_entry(const std::filesystem::path& archive_path,
                                          const char* entry_name)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, archive_path.string().c_str(), 0)) {
        return {};
    }
    std::size_t size = 0;
    void* data = mz_zip_reader_extract_file_to_heap(
        &archive, entry_name, &size, 0);
    std::vector<unsigned char> result;
    if (data != nullptr && size > 0) {
        const auto* begin = static_cast<const unsigned char*>(data);
        result.assign(begin, begin + size);
    }
    mz_free(data);
    mz_zip_reader_end(&archive);
    return result;
}

struct DecodedPng
{
    unsigned int width{0};
    unsigned int height{0};
    std::vector<unsigned char> rgba;
};

std::optional<DecodedPng> decode_png(const std::vector<unsigned char>& encoded)
{
    if (encoded.empty()) {
        return std::nullopt;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, encoded.data(), encoded.size())) {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGBA;
    DecodedPng result;
    result.width = image.width;
    result.height = image.height;
    result.rgba.resize(PNG_IMAGE_SIZE(image));
    const bool decoded = png_image_finish_read(
        &image, nullptr, result.rgba.data(), 0, nullptr) != 0;
    png_image_free(&image);
    return decoded ? std::optional<DecodedPng>(std::move(result)) : std::nullopt;
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
    REQUIRE(nozzle->printable_area.size() == 4);
    CHECK(nozzle->printable_area.front().x == 0.0);
    CHECK(nozzle->printable_area.front().y == 0.0);
    CHECK(nozzle->printable_area[2].x == 220.0);
    CHECK(nozzle->printable_area[2].y == 220.0);
    CHECK_FALSE(nozzle->printer_preset_id.empty());
    CHECK(nozzle->variable_filament_slots);
    CHECK(nozzle->max_filament_slots == 4);

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

TEST_CASE("library resizes variable filament slots atomically", "[libslicer_api][filaments]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge AD5X";
    selection.machine_variant_id = "0.4";
    auto activated = library->activate_config(selection);
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 1);
    REQUIRE(library->set_active_filament_color(0, {0x24, 0x74, 0xd8, 0xff}));

    const auto resized = library->resize_active_filament_slots(4);
    REQUIRE(resized.success);
    REQUIRE(resized.view.filament_slots.size() == 4);
    CHECK(resized.view.selection.filament_preset_ids.size() == 4);
    CHECK(resized.view.filament_slots[0].color.red == 0x24);
    CHECK(resized.view.filament_slots[3].color.blue == 0xd8);
    CHECK(library->validate_active_config().empty());

    const auto rejected = library->resize_active_filament_slots(5);
    CHECK_FALSE(rejected.success);
    REQUIRE_FALSE(rejected.diagnostics.empty());
    CHECK(library->active_config()->filament_slots.size() == 4);
}

TEST_CASE("library owns one normalized active filament configuration", "[libslicer_api][filaments]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    const auto machine = std::find_if(
        library->machine_models().begin(), library->machine_models().end(),
        [](const libslicer::MachineModelOption& item) {
            return item.id == "Flashforge Creator 5";
        });
    REQUIRE(machine != library->machine_models().end());
    const auto variant = std::find_if(
        machine->variants.begin(), machine->variants.end(),
        [](const libslicer::MachineVariantOption& item) { return item.id == "0.4"; });
    REQUIRE(variant != machine->variants.end());
    CHECK(variant->physical_tool_count == 4);
    CHECK_FALSE(variant->variable_filament_slots);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = machine->id;
    selection.machine_variant_id = variant->id;
    auto activated = library->activate_config(
        selection, {{"filament_colour", "#11223344"}});
    const std::string diagnostic = activated.diagnostics.empty()
        ? std::string{}
        : activated.diagnostics.front().key + ": " + activated.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 4);
    CHECK(activated.view.selection.filament_preset_ids.size() == 4);
    CHECK(activated.view.filament_slots.front().color.red == 0x11);
    CHECK(activated.view.filament_slots.front().color.green == 0x22);
    CHECK(activated.view.filament_slots.front().color.blue == 0x33);
    CHECK(activated.view.filament_slots.front().color.alpha == 0x44);
    CHECK(library->validate_active_config().empty());
    const auto snapshot = library->active_config_snapshot();
    REQUIRE(snapshot.has_value());
    const auto flush_multipliers = snapshot->value("flush_multiplier");
    const auto flush_matrix = snapshot->value("flush_volumes_matrix");
    REQUIRE(flush_multipliers.has_value());
    REQUIRE(flush_matrix.has_value());
    CHECK(std::count(flush_multipliers->begin(), flush_multipliers->end(), ',') + 1 == 4);
    CHECK(std::count(flush_matrix->begin(), flush_matrix->end(), ',') + 1 == 64);

    const auto initial_revision = activated.view.revision;
    const auto changed_color = library->set_active_filament_color(
        3, {0xaa, 0xbb, 0xcc, 0xdd});
    REQUIRE(changed_color.success);
    const auto view = library->active_config();
    REQUIRE(view.has_value());
    CHECK(view->revision > initial_revision);
    REQUIRE(view->filament_slots.size() == 4);
    CHECK(view->filament_slots[3].color.red == 0xaa);
    CHECK(view->filament_slots[3].color.green == 0xbb);
    CHECK(view->filament_slots[3].color.blue == 0xcc);
    CHECK(view->filament_slots[3].color.alpha == 0xdd);

    const auto invalid_slot = library->set_active_filament_color(
        4, {0, 0, 0, 255});
    CHECK_FALSE(invalid_slot.success);
    REQUIRE_FALSE(invalid_slot.diagnostics.empty());
    CHECK(invalid_slot.diagnostics.front().key == "filament");
}

TEST_CASE("library rejects mismatched filament cardinality before slicing", "[libslicer_api][slice][filaments]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    auto created = library->create_config(selection);
    REQUIRE(created.success);
    REQUIRE(created.config != nullptr);
    REQUIRE(created.config->set("filament_colour", "#11223344").success);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    request.config = created.config->snapshot();
    const auto sliced = library->slice(request);
    CHECK_FALSE(sliced.success);
    REQUIRE_FALSE(sliced.diagnostics.empty());
    CHECK(sliced.diagnostics.front().code == "filament");
    CHECK(sliced.diagnostics.front().message.find("filament colours") != std::string::npos);
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
    const std::filesystem::path packaged_output =
        std::filesystem::temp_directory_path() / "libslicer_api_20mm_cube.gcode.3mf";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    const auto missing_config = library->slice(request);
    CHECK_FALSE(missing_config.success);
    REQUIRE_FALSE(missing_config.diagnostics.empty());
    CHECK(missing_config.diagnostics.front().code == "config");

    request.config = created.config->snapshot();
    request.output_gcode_path = output.string();
    request.output_gcode_3mf_path = packaged_output.string();

    const auto original_relative = created.config->snapshot().value(
        "use_relative_e_distances");
    const auto original_before_layer = created.config->snapshot().value(
        "before_layer_change_gcode");
    const auto original_layer = created.config->snapshot().value(
        "layer_change_gcode");
    REQUIRE(original_relative.has_value());
    REQUIRE(original_before_layer.has_value());
    REQUIRE(original_layer.has_value());
    REQUIRE(created.config->set("use_relative_e_distances", "1").success);
    REQUIRE(created.config->set("before_layer_change_gcode", "").success);
    REQUIRE(created.config->set("layer_change_gcode", "").success);
    request.config = created.config->snapshot();
    const auto invalid = library->slice(request);
    REQUIRE_FALSE(invalid.success);
    const auto validation = std::find_if(
        invalid.diagnostics.begin(), invalid.diagnostics.end(),
        [](const libslicer::SliceDiagnostic& diagnostic) {
            return diagnostic.code == "validation" && !diagnostic.warning;
        });
    REQUIRE(validation != invalid.diagnostics.end());
    CHECK(validation->option_key == "before_layer_change_gcode");

    REQUIRE(created.config->set("use_relative_e_distances", *original_relative).success);
    REQUIRE(created.config->set("before_layer_change_gcode", *original_before_layer).success);
    REQUIRE(created.config->set("layer_change_gcode", *original_layer).success);
    request.config = created.config->snapshot();

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
    CHECK(sliced.gcode_3mf.path == packaged_output.string());
    CHECK(sliced.gcode_3mf.ownership == libslicer::OutputArtifactOwnership::CallerOwned);
    CHECK(std::filesystem::file_size(output) > 0);
    CHECK(std::filesystem::file_size(packaged_output) > 0);
    std::ifstream packaged_stream(packaged_output, std::ios::binary);
    char zip_signature[4]{};
    packaged_stream.read(zip_signature, sizeof(zip_signature));
    CHECK(std::string(zip_signature, sizeof(zip_signature)) == std::string("PK\x03\x04", 4));
    packaged_stream.clear();
    packaged_stream.seekg(0);
    const std::string packaged_content((std::istreambuf_iterator<char>(packaged_stream)),
                                       std::istreambuf_iterator<char>());
    CHECK(packaged_content.find("Metadata/plate_1.gcode") != std::string::npos);
    CHECK(packaged_content.find("Metadata/plate_1.gcode.md5") != std::string::npos);
    CHECK(packaged_content.find("Metadata/plate_1.png") != std::string::npos);
    CHECK(packaged_content.find("Metadata/plate_1_small.png") != std::string::npos);
    CHECK(sliced.summary.layer_count > 0);
    CHECK(sliced.summary.logical_motion_count > 0);
    CHECK(sliced.summary.render_segment_count > 0);
    REQUIRE(sliced.preview != nullptr);
    CHECK(sliced.preview->statistics.total_layers == sliced.summary.layer_count);
    CHECK(sliced.preview->statistics.logical_motion_count == sliced.summary.logical_motion_count);
    CHECK(sliced.preview->statistics.render_segment_count == sliced.summary.render_segment_count);
    CHECK(sliced.preview->schema_version == libslicer::toolpath_schema_version);
    const std::vector<libslicer::ToolpathViewType> expected_view_types = {
        libslicer::ToolpathViewType::Summary,
        libslicer::ToolpathViewType::FeatureType,
        libslicer::ToolpathViewType::Filament,
        libslicer::ToolpathViewType::Speed,
        libslicer::ToolpathViewType::ActualSpeed,
        libslicer::ToolpathViewType::Acceleration,
        libslicer::ToolpathViewType::Jerk,
        libslicer::ToolpathViewType::LayerHeight,
        libslicer::ToolpathViewType::LineWidth,
        libslicer::ToolpathViewType::VolumetricFlow,
        libslicer::ToolpathViewType::ActualVolumetricFlow,
        libslicer::ToolpathViewType::LayerTime,
        libslicer::ToolpathViewType::LayerTimeLogarithmic,
        libslicer::ToolpathViewType::FanSpeed,
        libslicer::ToolpathViewType::Temperature,
        libslicer::ToolpathViewType::PressureAdvance
    };
    CHECK(sliced.preview->supported_view_types == expected_view_types);
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
        const bool scalar_values_valid = finite(segment.duration_seconds) &&
            finite(segment.layer_duration_seconds) && finite(segment.fan_speed_percent) &&
            finite(segment.temperature_c) && finite(segment.pressure_advance) &&
            finite(segment.acceleration_mm_s2) && finite(segment.jerk_mm_s);
        return endpoints_valid && role_valid && dimensions_valid && scalar_values_valid;
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
    CHECK(sparse_stats->duration_seconds > 0.0);
    CHECK(sparse_stats->filament_length_m > 0.0);
    CHECK(sparse_stats->filament_weight_g > 0.0);
    CHECK_FALSE(sliced.preview->statistics.filament_usage.empty());
    CHECK(sliced.preview->statistics.total_filament_length_mm > 0.0);
    CHECK(sliced.preview->statistics.total_filament_weight_g > 0.0);
    CHECK_FALSE(sliced.preview->statistics.options.empty());
    CHECK(sliced.preview->statistics.max_actual_speed_mm_s > 0.0f);
    CHECK(sliced.preview->statistics.max_actual_volumetric_flow_mm3_s > 0.0f);
    CHECK(sliced.preview->statistics.max_layer_time_seconds > 0.0f);

    std::ifstream gcode(output);
    const std::string content((std::istreambuf_iterator<char>(gcode)), std::istreambuf_iterator<char>());
    CHECK(content.find("G1") != std::string::npos);

    libslicer::GCodePreviewRequest preview_request;
    preview_request.gcode_path = output.string();
    const auto imported = library->load_gcode_preview(preview_request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->schema_version == libslicer::toolpath_schema_version);
    CHECK(imported.preview->supported_view_types == expected_view_types);
    CHECK(imported.preview->statistics.total_layers == sliced.preview->statistics.total_layers);
    CHECK(imported.preview->statistics.render_segment_count > 0);
    CHECK_FALSE(imported.preview->statistics.features.empty());
    CHECK_FALSE(imported.preview->statistics.filament_usage.empty());
    CHECK(imported.preview->statistics.total_filament_length_mm > 0.0);
    CHECK(imported.preview->source_path == output.string());
    CHECK(count_extrusion_role(*imported.preview,
                               libslicer::ToolpathExtrusionRole::SparseInfill) > 0);
    CHECK(count_extrusion_role(*imported.preview,
                               libslicer::ToolpathExtrusionRole::InternalSolidInfill) > 0);
    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);
}

TEST_CASE("painted model thumbnails preserve filament colors", "[libslicer_api][slice][thumbnail]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(
        selection, {{"filament_colour", "#FF0000FF"}});
    REQUIRE(activated.success);
    REQUIRE(library->set_active_filament_color(0, {255, 0, 0, 255}).success);
    REQUIRE(library->set_active_filament_color(1, {0, 255, 0, 255}).success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    libslicer::SliceVolumeInput volume;
    volume.default_filament_slot = 1;
    volume.vertices = {
        {0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 0.0f},
        {20.0f, 20.0f, 0.0f}, {0.0f, 20.0f, 0.0f},
        {0.0f, 0.0f, 20.0f}, {20.0f, 0.0f, 20.0f},
        {20.0f, 20.0f, 20.0f}, {0.0f, 20.0f, 20.0f},
    };
    volume.triangles = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7},
    };
    // A leaf with state 2 is encoded as the little-endian nibble 0b1000.
    // Paint both top triangles with filament slot 2; all other faces retain slot 1.
    volume.facet_labels.roots = {{2, 0}, {3, 4}};
    volume.facet_labels.bitstream = {0, 0, 0, 1, 0, 0, 0, 1};

    libslicer::SliceObjectInput object;
    object.name = "painted-cube";
    object.volumes.push_back(std::move(volume));

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_painted_cube.gcode";
    const std::filesystem::path packaged_output =
        std::filesystem::temp_directory_path() / "libslicer_api_painted_cube.gcode.3mf";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);

    libslicer::SliceRequest request;
    request.config = *config;
    request.objects.push_back(std::move(object));
    request.output_gcode_path = output.string();
    request.output_gcode_3mf_path = packaged_output.string();
    const auto sliced = library->slice(request);
    const std::string diagnostic = sliced.diagnostics.empty()
        ? std::string{}
        : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);

    const auto thumbnail_bytes = read_zip_entry(
        packaged_output, "Metadata/plate_1.png");
    const auto thumbnail = decode_png(thumbnail_bytes);
    REQUIRE(thumbnail.has_value());
    REQUIRE(thumbnail->rgba.size() ==
            static_cast<std::size_t>(thumbnail->width) * thumbnail->height * 4);

    std::size_t red_pixels = 0;
    std::size_t green_pixels = 0;
    for (std::size_t pixel = 0; pixel < thumbnail->rgba.size(); pixel += 4) {
        const unsigned int red = thumbnail->rgba[pixel];
        const unsigned int green = thumbnail->rgba[pixel + 1];
        const unsigned int blue = thumbnail->rgba[pixel + 2];
        if (red > 48 && red > green + 32 && red > blue + 32) {
            ++red_pixels;
        }
        if (green > 48 && green > red + 32 && green > blue + 32) {
            ++green_pixels;
        }
    }
    CHECK(red_pixels > 100);
    CHECK(green_pixels > 100);

    std::filesystem::remove(output, remove_error);
    std::filesystem::remove(packaged_output, remove_error);
}
