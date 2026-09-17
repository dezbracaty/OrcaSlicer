#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <libslicer/Config.hpp>
#include <libslicer/Library.hpp>

#include <miniz.h>
#include <png.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace {
// Real-model validation must use the same explicit mesh placement as the App.
// Compatibility file input does not apply SliceObjectInput::transform.
libslicer::SliceObjectInput binary_stl_on_bed(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    char header[80]; input.read(header, sizeof(header));
    std::uint32_t triangles = 0;
    input.read(reinterpret_cast<char*>(&triangles), sizeof(triangles));
    REQUIRE(std::filesystem::file_size(path) == 84ull + 50ull*triangles);
    REQUIRE(triangles > 0);
    libslicer::SliceObjectInput result;
    result.name = std::filesystem::path(path).stem().string();
    libslicer::SliceVolumeInput volume;
    std::map<std::array<float,3>,std::uint32_t> indices;
    double bottom = std::numeric_limits<double>::max();
    for (std::uint32_t i = 0; i < triangles; ++i) {
        char facet[50]; input.read(facet, sizeof(facet));
        REQUIRE(input.good());
        std::array<std::uint32_t,3> ids;
        for (size_t j = 0; j < 3; ++j) {
            std::array<float,3> xyz;
            std::memcpy(xyz.data(), facet+12+j*12, 12);
            for (float value : xyz) REQUIRE(std::isfinite(value));
            bottom = std::min(bottom, double(xyz[2]));
            auto entry = indices.emplace(xyz, static_cast<std::uint32_t>(volume.vertices.size()));
            if (entry.second) volume.vertices.push_back({xyz[0],xyz[1],xyz[2]});
            ids[j] = entry.first->second;
        }
        volume.triangles.push_back({ids[0],ids[1],ids[2]});
    }
    result.transform[11] = -bottom;
    result.volumes.push_back(std::move(volume));
    return result;
}

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

TEST_CASE("continuous fiber settings expose their UI dependencies", "[libslicer_api][config][fiber]")
{
    auto config = libslicer::Config::defaults();
    auto items = config.settings();

    const auto* contour_toggle = find_item(items, "generate_reinforced_perimeters");
    const auto* contour_count = find_item(items, "outer_reinforced_perimeters_counts");
    const auto* infill_density = find_item(items, "reinforced_infill_density");
    const auto* layer_interval = find_item(items, "fiber_layer_height_ratio");
    const auto* fill_debug = find_item(items, "fiber_fill_debug");
    REQUIRE(fill_debug != nullptr);
    CHECK(fill_debug->type == libslicer::SettingType::Boolean);
    CHECK(fill_debug->group == libslicer::SettingGroup::Process);
    CHECK(fill_debug->level == libslicer::SettingLevel::Simple);
    CHECK(fill_debug->category == "Continuous fiber");
    CHECK(fill_debug->visible);
    CHECK(fill_debug->default_value == "0");
    CHECK_FALSE(fill_debug->enabled);
    const auto* minimum_segment = find_item(items, "fiber_minimum_segment_length");
    const auto* maximum_turn = find_item(items, "fiber_maximum_turn_angle");
    const auto* contour_infill_clearance = find_item(items, "fiber_contour_infill_clearance");
    REQUIRE(contour_toggle != nullptr);
    REQUIRE(contour_count != nullptr);
    REQUIRE(infill_density != nullptr);
    REQUIRE(layer_interval != nullptr);
    REQUIRE(minimum_segment != nullptr);
    REQUIRE(maximum_turn != nullptr);
    REQUIRE(contour_infill_clearance != nullptr);
    CHECK(contour_toggle->enabled);
    CHECK_FALSE(contour_count->enabled);
    CHECK_FALSE(infill_density->enabled);
    CHECK_FALSE(layer_interval->enabled);
    CHECK_FALSE(minimum_segment->enabled);
    CHECK_FALSE(maximum_turn->enabled);
    CHECK_FALSE(contour_infill_clearance->enabled);

    const auto contour_enabled = config.set("generate_reinforced_perimeters", "1");
    REQUIRE(contour_enabled.success);
    CHECK(find_item(contour_enabled.changed_items, "generate_reinforced_perimeters") != nullptr);
    CHECK(find_item(contour_enabled.changed_items, "outer_reinforced_perimeters_counts") != nullptr);
    CHECK(find_item(contour_enabled.changed_items, "fiber_layer_height_ratio") != nullptr);

    items = config.settings();
    CHECK(find_item(items, "outer_reinforced_perimeters_counts")->enabled);
    CHECK(find_item(items, "fiber_layer_height_ratio")->enabled);
    CHECK(find_item(items, "fiber_fill_debug")->enabled);
    REQUIRE(config.set("fiber_fill_debug", "1").success);
    const auto debug_snapshot = config.snapshot();
    REQUIRE(config.reset("fiber_fill_debug").success);
    CHECK(debug_snapshot.value("fiber_fill_debug") == "1");
    CHECK(config.snapshot().value("fiber_fill_debug") == "0");
    CHECK(find_item(items, "fiber_minimum_segment_length")->enabled);
    CHECK(find_item(items, "fiber_maximum_turn_angle")->enabled);
    CHECK_FALSE(find_item(items, "reinforced_infill_density")->enabled);
    CHECK_FALSE(find_item(items, "fiber_contour_infill_clearance")->enabled);

    const auto infill_enabled = config.set("generate_reinforced_infills", "1");
    REQUIRE(infill_enabled.success);
    CHECK(find_item(infill_enabled.changed_items, "reinforced_infill_density") != nullptr);
    CHECK(find_item(infill_enabled.changed_items, "fiber_contour_infill_clearance") != nullptr);

    items = config.settings();
    CHECK(find_item(items, "reinforced_infill_density")->enabled);
    CHECK(find_item(items, "fiber_contour_infill_clearance")->enabled);
}

TEST_CASE("CFSYS profiles expose the canonical continuous fiber contract", "[libslicer_api][config][fiber][cfsys]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection);
    const std::string diagnostic = activated.diagnostics.empty()
        ? std::string{}
        : activated.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(activated.success);

    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());
    const auto cut_gcode = config->value("fiber_cut_gcode");
    const auto cut_to_contact = config->value("fiber_cut_to_contact_length");
    const auto prefeed_extra = config->value("fiber_prefeed_extra_length");
    const auto z_hop_height = config->value("fiber_z_hop_height");
    const auto landing_length = config->value("fiber_landing_length");
    const auto landing_speed = config->value("fiber_landing_speed");
    const auto start_speed = config->value("fiber_start_speed");
    const auto minimum_effective = config->value("fiber_minimum_effective_length");
    const auto minimum_segment = config->value("fiber_minimum_segment_length");
    const auto maximum_turn = config->value("fiber_maximum_turn_angle");
    const auto contour_speed = config->value("fiber_contour_max_speed");
    const auto infill_speed = config->value("fiber_infill_max_speed");
    const auto contour_acceleration = config->value("fiber_contour_acceleration");
    const auto infill_acceleration = config->value("fiber_infill_acceleration");
    REQUIRE(cut_gcode.has_value());
    REQUIRE(cut_to_contact.has_value());
    REQUIRE(prefeed_extra.has_value());
    REQUIRE(z_hop_height.has_value());
    REQUIRE(landing_length.has_value());
    REQUIRE(landing_speed.has_value());
    REQUIRE(start_speed.has_value());
    REQUIRE(minimum_effective.has_value());
    REQUIRE(minimum_segment.has_value());
    REQUIRE(maximum_turn.has_value());
    REQUIRE(contour_speed.has_value());
    REQUIRE(infill_speed.has_value());
    REQUIRE(contour_acceleration.has_value());
    REQUIRE(infill_acceleration.has_value());
    // ConfigSnapshot exposes the serialized representation; multiline strings
    // therefore contain C-style escaped newlines until a slice config is built.
    CHECK(*cut_gcode == "M400\\nS0\\nM400\\n");
    CHECK(*cut_to_contact == "23");
    CHECK(*prefeed_extra == "0.5");
    CHECK(*z_hop_height == "2");
    CHECK(*landing_length == "2");
    CHECK(*landing_speed == "3");
    CHECK(*start_speed == "10");
    CHECK(*minimum_effective == "0.5");
    CHECK(config->value("fiber_minimum_path_length") == std::optional<std::string>{"0"});
    CHECK(*minimum_segment == "0");
    CHECK(*maximum_turn == "180");
    CHECK(*contour_speed == "10");
    CHECK(*infill_speed == "10");
    CHECK(*contour_acceleration == "500");
    CHECK(*infill_acceleration == "500");
    CHECK(config->value("physical_extruder_map") == std::optional<std::string>{"0,1"});
    CHECK(config->value("toolhead_fiber_protocol_id") == std::optional<std::string>{";cfsys-v1"});

    // The new algorithm has one canonical configuration contract. Historical
    // CFSYS names must not survive as runtime aliases.
    CHECK_FALSE(config->value("fibercut_length").has_value());
    CHECK_FALSE(config->value("fiber_restart_extra_length").has_value());
    CHECK_FALSE(config->value("fiber_z_hop").has_value());
    CHECK_FALSE(config->value("fiber_start_min_length").has_value());
    CHECK_FALSE(config->value("fiber_normal_max_speed").has_value());
    CHECK_FALSE(config->value("fiber_perimeter_acceleration").has_value());
}

TEST_CASE("CFSYS material slots are explicitly owned by physical tools", "[libslicer_api][cfsys][tool-materials]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS PLA", "CFSYS CCF"};
    const auto activated = library->activate_config(selection);
    for (const auto& diagnostic : activated.diagnostics) INFO(diagnostic.message);
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 2);
    const auto contains = [](const auto& options, const std::string& id) {
        return std::any_of(options.begin(), options.end(), [&id](const auto& value) { return value.id == id; });
    };
    CHECK(contains(activated.view.filament_slots[0].compatible_presets, "CFSYS PLA"));
    CHECK_FALSE(contains(activated.view.filament_slots[0].compatible_presets, "CFSYS CCF"));
    CHECK(contains(activated.view.filament_slots[1].compatible_presets, "CFSYS CCF"));
    CHECK_FALSE(contains(activated.view.filament_slots[1].compatible_presets, "CFSYS PLA"));
    const auto initial_revision = activated.view.revision;
    CHECK_FALSE(library->set_active_filament_preset(0, "CFSYS CCF").success);
    CHECK_FALSE(library->set_active_filament_preset(1, "CFSYS PLA").success);
    CHECK_FALSE(library->add_active_filament(1).success);
    CHECK_FALSE(library->add_active_filament(0, "CFSYS CCF").success);
    CHECK_FALSE(library->resize_active_filament_slots(3).success);
    CHECK(library->active_config()->revision == initial_revision);
    REQUIRE(library->set_active_config_value("layer_height", "0.15").success);
    REQUIRE(library->set_active_filament_color(0, {20, 40, 60, 255}).success);
    for (size_t count = 3; count <= 5; ++count) {
        const auto added = library->add_active_filament(0, "CFSYS PLA");
        for (const auto& diagnostic : added.diagnostics) INFO(diagnostic.message);
        REQUIRE(added.success);
        REQUIRE(added.view.filament_slots.size() == count);
        CHECK(added.view.filament_slots.back().physical_tool_index == 0);
        CHECK(added.view.filament_slots[1].physical_tool_index == 1);
        CHECK(added.view.filament_slots[0].color.red == 20);
        CHECK(library->active_config_snapshot()->value("layer_height") == "0.15");
    }
    CHECK_FALSE(library->add_active_filament(0).success);
    CHECK(library->active_config_snapshot()->value("filament_map") == "1,2,1,1,1");
    CHECK(library->validate_active_config().empty());

    SECTION("arbitrary explicit material order is retained") {
        auto second = libslicer::Library::open(options);
        selection.filament_preset_ids = {"CFSYS PLA", "CFSYS PLA", "CFSYS CCF"};
        selection.filament_physical_tools = {0, 0, 1};
        const auto reordered = second->activate_config(selection, {
            {"reinforced_perimeters_filament", "3"}, {"reinforced_infill_filament", "3"}});
        for (const auto& diagnostic : reordered.diagnostics) INFO(diagnostic.message);
        REQUIRE(reordered.success);
        CHECK(second->active_config_snapshot()->value("filament_map") == "1,1,2");
        CHECK(reordered.view.filament_slots[2].physical_tool_index == 1);
        CHECK_FALSE(second->set_active_filament_preset(2, "CFSYS PLA").success);
    }
}

TEST_CASE("CFSYS original machine scripts produce real fiber G-code and reimport", "[libslicer_api][fiber][cfsys][slice]")
{
    const unsigned fiber_slot = GENERATE(0u, 1u, 2u, 4u);
    INFO("fiber material index=" << fiber_slot);
    // Alpha500 is the machine registered by the current CFSYS vendor catalog.
    const std::string machine = "CFSYS Alpha500 Printer";
    INFO(machine);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = machine;
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    const unsigned material_count = std::max(2u, fiber_slot + 1);
    selection.filament_preset_ids.assign(material_count, "CFSYS PLA");
    selection.filament_preset_ids[fiber_slot] = "CFSYS CCF";
    selection.filament_physical_tools.assign(material_count, 0);
    selection.filament_physical_tools[fiber_slot] = 1;
    const auto activated = library->activate_config(selection, {
        {"reinforced_perimeters_filament", std::to_string(fiber_slot + 1)},
        {"reinforced_infill_filament", std::to_string(fiber_slot + 1)},
        {"outer_wall_filament_id", fiber_slot == 0 ? "2" : "1"},
        {"inner_wall_filament_id", fiber_slot == 0 ? "2" : "1"},
        {"sparse_infill_filament_id", fiber_slot != 1 ? "2" : "1"},
        {"internal_solid_filament_id", fiber_slot != 1 ? "2" : "1"},
        {"top_surface_filament_id", fiber_slot != 1 ? "2" : "1"},
        {"bottom_surface_filament_id", fiber_slot != 1 ? "2" : "1"}});
    std::string activation_diagnostics;
    for (const auto& issue : activated.diagnostics) activation_diagnostics += issue.key + ": " + issue.message + "\n";
    INFO(activation_diagnostics);
    REQUIRE(activated.success);
    std::optional<double> reference_feed;
    for (bool relative_e : {false, true}) {
        INFO("relative_e=" << relative_e);
        REQUIRE(library->apply_active_config_patch({{"use_relative_e_distances", relative_e ? "1" : "0"}}).success);
        libslicer::SliceRequest request;
        const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
        if (model) request.objects.push_back(binary_stl_on_bed(model));
        else request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/two_20mm_cubes.obj", {}}};
        request.config = *library->active_config_snapshot();
        const auto result = library->slice(request);
        std::string diagnostics;
        for (const auto& issue : result.diagnostics) diagnostics += issue.message + "\n";
        INFO(diagnostics);
        REQUIRE(result.success);
        REQUIRE(result.preview);
        REQUIRE(result.preview->statistics.total_fiber_feed_mm > 0);
        std::size_t powered = 0, passive = 0, finish = 0;
        std::set<unsigned> resin_materials;
        for (const auto& segment : result.preview->segments) {
            if (segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
                CHECK(segment.tool_id == 0);
                resin_materials.insert(segment.filament_id);
            }
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered) {
                ++powered;
                REQUIRE(segment.tool_id == 1);
                REQUIRE(segment.filament_id == fiber_slot);
                REQUIRE(segment.fiber_feed_delta_mm > 0);
                REQUIRE(segment.end_mm.z > 0);
            }
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
                ++passive;
                REQUIRE(segment.width_mm > 0);
                REQUIRE(segment.height_mm > 0);
                REQUIRE(segment.fiber_feed_delta_mm == 0);
            }
            if (segment.fiber_phase == libslicer::ToolpathFiberPhase::Finish) {
                ++finish;
                REQUIRE(segment.deposition == libslicer::ToolpathDepositionKind::None);
            }
        }
        REQUIRE(powered > 0);
        if (fiber_slot > 1) CHECK(resin_materials.count(1) > 0);
        REQUIRE(passive > 0);
        REQUIRE(finish > 0);
        std::ifstream input(result.output.path);
        const std::string gcode((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto first_block = gcode.find(";FIBER_BEGIN");
        REQUIRE(first_block != std::string::npos);
        CHECK(gcode.find(machine == "CFSYS Alpha500 Printer" ? "DF1004" : "PRINT_START ") < first_block);
        CHECK(gcode.find("PRINT_END") > gcode.rfind(";FIBER_END"));
        std::size_t cursor = 0, cuts = 0;
        const std::string expected_cut = ";FIBER_CUT\nM400\nS0\nM400\n;FIBER_TAIL_BEGIN\n";
        while ((cursor = gcode.find(";FIBER_CUT\n", cursor)) != std::string::npos) {
            REQUIRE(gcode.compare(cursor, expected_cut.size(), expected_cut) == 0);
            ++cuts;
            cursor += 11;
        }
        REQUIRE(cuts > 0);
        std::istringstream commands(gcode);
        std::string line;
        double acceleration = 0;
        while (std::getline(commands, line)) {
            const auto setting = line.find("ACCEL=");
            if (line.rfind("SET_VELOCITY_LIMIT", 0) == 0 && setting != std::string::npos)
                acceleration = std::stod(line.substr(setting+6));
            if (line == ";FIBER_LANDING_BEGIN" || line == ";FIBER_START" || line == ";FIBER_TAIL_BEGIN")
                REQUIRE(acceleration == Catch::Approx(500.0));
        }
        libslicer::GCodePreviewRequest import_request;
        import_request.gcode_path = result.output.path;
        const auto imported = library->load_gcode_preview(import_request);
        REQUIRE(imported.success);
        REQUIRE(imported.preview);
        CHECK(imported.preview->statistics.total_fiber_feed_mm ==
              Catch::Approx(result.preview->statistics.total_fiber_feed_mm).margin(0.02));
        CHECK(imported.preview->statistics.total_fiber_deposited_path_mm ==
              Catch::Approx(result.preview->statistics.total_fiber_deposited_path_mm).margin(0.02));
        if (reference_feed)
            CHECK(result.preview->statistics.total_fiber_feed_mm == Catch::Approx(*reference_feed).margin(0.05));
        else reference_feed = result.preview->statistics.total_fiber_feed_mm;
        std::error_code error;
        std::filesystem::remove(result.output.path, error);
    }
}

TEST_CASE("CFSYS reference model retains reinforced contours on every reference layer", "[libslicer_api][fiber][reference]")
{
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    const char* reference = std::getenv("LIBSLICER_FIBER_REFERENCE_GCODE");
    if (!model || !reference) SKIP("Supply the reference STL and G-code to run the stage-one acceptance test");
    std::ifstream input(reference);
    REQUIRE(input.good());
    std::map<size_t,size_t> reference_contours;
    std::string line, role;
    size_t layers = 0;
    while (std::getline(input,line)) {
        if (line == ";LAYER_CHANGE") ++layers;
        if (line.rfind(";TYPE:",0) == 0) role = line.substr(6);
        if (line == "S0" && role == "Fiber wall") ++reference_contours[layers];
    }
    REQUIRE_FALSE(reference_contours.empty());
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS PLA", "CFSYS CCF"};
    REQUIRE(library->activate_config(selection).success);
    REQUIRE(library->apply_active_config_patch({{"generate_reinforced_infills","1"},
        {"reinforced_infill_density","100%"},{"curr_bed_type","High Temp Plate"},
        {"precise_outer_wall","0"},{"wall_direction","auto"}}).success);
    libslicer::SliceRequest request;
    request.objects.push_back(binary_stl_on_bed(model));
    request.config = *library->active_config_snapshot();
    if (const char* output = std::getenv("LIBSLICER_FIBER_VALIDATION_OUTPUT")) request.output_gcode_path = output;
    const auto result = library->slice(request);
    for (const auto& d : result.diagnostics) INFO(d.message);
    REQUIRE(result.success);
    REQUIRE(result.summary.layer_count == layers);
    std::ifstream generated(result.output.path);
    std::map<size_t,size_t> contours;
    const std::regex occurrence("^;FIBER_BEGIN .* layer=([0-9]+) .*purpose=contour ");
    std::smatch match;
    while (std::getline(generated,line))
        if (std::regex_search(line,match,occurrence)) ++contours[std::stoul(match[1])+1];
    for (const auto& expected : reference_contours) {
        INFO("Reference layer=" << expected.first << ", reference contours=" << expected.second);
        CHECK(contours[expected.first] >= expected.second);
    }
    if (request.output_gcode_path.empty()) {
        std::error_code error; std::filesystem::remove(result.output.path,error);
    }
}

namespace {
libslicer::SliceObjectInput fiber_infill_block()
{
    libslicer::SliceObjectInput object;
    object.name = "fiber-infill-block";
    libslicer::SliceVolumeInput volume;
    volume.vertices = {{100,100,0},{160,100,0},{160,140,0},{100,140,0},
                       {100,100,8},{160,100,8},{160,140,8},{100,140,8}};
    volume.triangles = {{0,2,1},{0,3,2},{4,5,6},{4,6,7},{0,1,5},{0,5,4},
                        {1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}};
    object.volumes.push_back(std::move(volume));
    return object;
}
}

TEST_CASE("fiber fill debug is opt-in and does not change print output", "[libslicer_api][fiber-fill-debug]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0,1};
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    // Synthetic fixture deliberately rejects candidates; the optional real model
    // uses the unmodified production process budget.
    REQUIRE(library->activate_config(selection, {{"generate_reinforced_infills", "1"}}).success);
    if (!model) REQUIRE(library->apply_active_config_patch({{"fiber_minimum_path_length", "1000"}}).success);
    libslicer::SliceRequest request;
    request.config = *library->active_config_snapshot();
    request.center_on_build_plate = model != nullptr;
    request.objects.push_back(model ? binary_stl_on_bed(model) : fiber_infill_block());
    const auto normal = library->slice(request);
    for (const auto& d : normal.diagnostics) INFO(d.message);
    REQUIRE(normal.success);
    REQUIRE(normal.preview);
    CHECK(normal.preview->fiber_fill_diagnostics.empty());
    REQUIRE(request.config.value("fiber_fill_debug") == "0");
    REQUIRE(library->apply_active_config_patch({{"fiber_fill_debug", "1"}}).success);
    request.config = *library->active_config_snapshot();
    REQUIRE(request.config.value("fiber_fill_debug") == "1");
    // A later UI edit cannot change the already captured slice configuration.
    REQUIRE(library->apply_active_config_patch({{"fiber_fill_debug", "0"}}).success);
    const auto debug = library->slice(request);
    for (const auto& d : debug.diagnostics) INFO(d.message);
    REQUIRE(debug.success);
    REQUIRE(debug.preview);
    REQUIRE_FALSE(debug.preview->fiber_fill_diagnostics.empty());
    size_t contour_count = 0, infill_count = 0;
    for (const auto& path : debug.preview->fiber_fill_diagnostics) {
        REQUIRE(path.points.size() >= 2);
        CHECK_FALSE(path.reason.empty());
        CHECK(path.source_length_mm > 0);
        REQUIRE(path.layer_index < debug.preview->layers.size());
        CHECK(path.object_index == 0);
        CHECK(path.instance_index == 0);
        path.contour ? ++contour_count : ++infill_count;
        for (const auto& p : path.points) {
            CHECK(std::isfinite(p.x)); CHECK(std::isfinite(p.y));
            CHECK(p.z == Catch::Approx(debug.preview->layers[path.layer_index].print_z_mm));
            if (!model) {
                CHECK(p.x >= 100); CHECK(p.x <= 160);
                CHECK(p.y >= 100); CHECK(p.y <= 140);
            }
        }
    }
    CHECK(contour_count > 0);
    CHECK(infill_count > 0);
    CHECK(debug.preview->segments.size() == normal.preview->segments.size());
    CHECK(debug.summary.filament_used_mm == Catch::Approx(normal.summary.filament_used_mm));
    CHECK(debug.summary.estimated_time_seconds == Catch::Approx(normal.summary.estimated_time_seconds));
    const auto commands = [](const std::string& path) {
        std::ifstream input(path);
        std::vector<std::string> result;
        for (std::string line; std::getline(input, line);) {
            // Timestamps and file names live in comments, never compare those.
            line = line.substr(0, line.find(';'));
            if (!line.empty()) result.push_back(std::move(line));
        }
        return result;
    };
    CHECK(commands(debug.output.path) == commands(normal.output.path));
    std::cout << "[FiberFillDebug] contour_rejected=" << contour_count
              << " infill_rejected=" << infill_count << " commands_unchanged\n";
    for (const auto& diagnostic : debug.diagnostics)
        if (diagnostic.code == "fiber_infill_summary" || diagnostic.code == "fiber_infill_empty") {
            std::cout << "[FiberFillDebug] " << diagnostic.message << '\n';
            const auto start = diagnostic.message.find("Rejected fragments:");
            REQUIRE(start != std::string::npos);
            std::istringstream counts(diagnostic.message.substr(start + 19));
            size_t rejected = 0;
            for (std::string entry; counts >> entry;) {
                const auto equal = entry.find('=');
                REQUIRE(equal != std::string::npos);
                rejected += std::stoul(entry.substr(equal+1));
            }
            CHECK(infill_count == rejected);
        }
    CHECK(std::none_of(debug.diagnostics.begin(), debug.diagnostics.end(), [](const auto& d) { return d.code == "fiber_debug_layer"; }));
    libslicer::GCodePreviewRequest imported_request;
    imported_request.gcode_path = debug.output.path;
    const auto imported = library->load_gcode_preview(imported_request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview);
    CHECK(imported.preview->fiber_fill_diagnostics.empty());
    std::filesystem::remove(normal.output.path);
    std::filesystem::remove(debug.output.path);
}

TEST_CASE("CFSYS default finish permits internal fiber output on T1", "[libslicer_api][fiber][fiber-infill-output]")
{
    const unsigned fiber_slot = GENERATE(1u, 4u);
    const bool relative_e = GENERATE(false, true);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids.assign(5, "CFSYS CIRON");
    selection.filament_physical_tools.assign(5, 0);
    selection.filament_preset_ids[fiber_slot] = "CFSYS CCF";
    selection.filament_physical_tools[fiber_slot] = 1;
    REQUIRE(library->activate_config(selection, {
        {"generate_reinforced_infills", "1"},
        {"reinforced_perimeters_filament", std::to_string(fiber_slot+1)},
        {"reinforced_infill_filament", std::to_string(fiber_slot+1)},
        {"use_relative_e_distances", relative_e ? "1" : "0"}}).success);
    libslicer::SliceRequest request;
    const char* model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    request.objects.push_back(model ? binary_stl_on_bed(model) : fiber_infill_block());
    request.config = *library->active_config_snapshot();
    if (const char* dir = std::getenv("LIBSLICER_FIBER_INFILL_OUTPUT_DIR"))
        request.output_gcode_path = (std::filesystem::path(dir) /
            ("fiber-infill-T1-slot" + std::to_string(fiber_slot) + (relative_e ? "-relative.gcode" : "-absolute.gcode"))).string();
    const auto result = library->slice(request);
    std::string diagnostics;
    for (const auto& issue : result.diagnostics) diagnostics += issue.message + "\n";
    INFO(diagnostics);
    if (model) std::cout << "[FiberOutputValidation] slot=" << fiber_slot << " relative_e=" << relative_e << " " << diagnostics;
    REQUIRE(result.success);
    REQUIRE(result.preview);
    CHECK(std::none_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) { return d.code == "fiber_infill_empty"; }));

    std::ifstream output(result.output.path);
    REQUIRE(output.good());
    std::string line, phase;
    bool infill = false, relative = false, absolute_xyz = true;
    unsigned tool = 0;
    double x = 0, y = 0, z = 0, e = 0, tail = 0, finish = 0, powered = 0;
    size_t infills = 0;
    std::map<size_t,size_t> contours;
    while (std::getline(output,line)) {
        if (line.rfind(";FIBER_BEGIN ",0) == 0) {
            infill = line.find("purpose=infill") != std::string::npos;
            if (infill) { ++infills; CHECK(tool == 1); tail = finish = powered = 0; }
            else if (line.find("purpose=contour") != std::string::npos) {
                const auto pos = line.find(" layer="); REQUIRE(pos != std::string::npos);
                ++contours[std::stoul(line.substr(pos+7))];
            }
        }
        if (line == ";FIBER_START") phase = "powered";
        if (line == ";FIBER_TAIL_BEGIN") phase = "tail";
        if (line == ";FIBER_DEPLETED") phase = "finish";
        if (line == ";FIBER_END") {
            if (infill) {
                CHECK(powered > 0);
                CHECK(tail == Catch::Approx(23).margin(0.03));
                CHECK(finish == Catch::Approx(23).margin(0.003));
            }
            infill = false; phase.clear();
        }
        std::istringstream fields(line);
        std::string cmd; fields >> cmd;
        if (cmd == "M82") relative = false;
        if (cmd == "M83") relative = true;
        if (cmd == "G90") absolute_xyz = true;
        if (cmd == "G91") absolute_xyz = false;
        if (cmd.size() > 1 && cmd[0] == 'T' && std::isdigit(static_cast<unsigned char>(cmd[1]))) tool = std::stoul(cmd.substr(1));
        if (cmd != "G0" && cmd != "G1" && cmd != "G92") continue;
        double nx=x, ny=y, nz=z, ne=e; bool has_e=false;
        std::string field;
        while (fields >> field) {
            if (field[0] == ';') break;
            if (field.size() < 2 || std::string("XYZE").find(field[0]) == std::string::npos) continue;
            const double value = std::stod(field.substr(1));
            if (field[0] == 'X') nx = value + (absolute_xyz || cmd == "G92" ? 0 : x);
            if (field[0] == 'Y') ny = value + (absolute_xyz || cmd == "G92" ? 0 : y);
            if (field[0] == 'Z') nz = value + (absolute_xyz || cmd == "G92" ? 0 : z);
            if (field[0] == 'E') { ne = value + (relative && cmd != "G92" ? e : 0); has_e = true; }
        }
        if (infill && cmd != "G92" && !phase.empty()) {
            CHECK(tool == 1);
            if (phase == "powered") { if (has_e) powered += ne-e; }
            else {
                CHECK_FALSE(has_e);
                CHECK(nz == Catch::Approx(z).margin(0.0001));
                if (phase == "tail") tail += std::hypot(nx-x,ny-y);
                else finish += std::hypot(nx-x,ny-y);
            }
        }
        x=nx; y=ny; z=nz; e=ne;
    }
    CHECK(infills > 0);
    if (const char* baseline = std::getenv("LIBSLICER_FIBER_CONTOUR_BASELINE_GCODE")) {
        std::ifstream input(baseline); REQUIRE(input.good());
        std::map<size_t,size_t> expected;
        while (std::getline(input,line))
            if (line.rfind(";FIBER_BEGIN ",0) == 0 && line.find("purpose=contour") != std::string::npos) {
                const auto pos = line.find(" layer="); REQUIRE(pos != std::string::npos);
                ++expected[std::stoul(line.substr(pos+7))];
            }
        REQUIRE_FALSE(expected.empty());
        for (const auto& [layer,count] : expected) { INFO("layer=" << layer); CHECK(contours[layer] >= count); }
    }
    const auto verify_preview = [&](const libslicer::ToolpathPreview& preview) {
        size_t powered_segments = 0, passive_segments = 0;
        for (const auto& segment : preview.segments) {
            if (segment.extrusion_role != libslicer::ToolpathExtrusionRole::ContinuousFiberInfill) continue;
            CHECK(segment.tool_id == 1);
            CHECK(segment.filament_id == fiber_slot);
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered) {
                ++powered_segments; CHECK(segment.fiber_feed_delta_mm > 0);
            }
            if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) ++passive_segments;
        }
        CHECK(powered_segments > 0); CHECK(passive_segments > 0);
    };
    verify_preview(*result.preview);
    const auto imported = library->load_gcode_preview({result.output.path});
    REQUIRE(imported.success); REQUIRE(imported.preview);
    verify_preview(*imported.preview);
    if (request.output_gcode_path.empty()) { std::error_code ec; std::filesystem::remove(result.output.path,ec); }
}

TEST_CASE("CFSYS rejects a finish outside machine limits instead of dropping fiber", "[libslicer_api][fiber][fiber-motion-limit]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"CFSYS"};
    auto library = libslicer::Library::open(options);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    selection.filament_physical_tools = {0,1};
    REQUIRE(library->activate_config(selection, {{"generate_reinforced_infills","1"},
        {"fiber_finish_extension_length","500"}}).success);
    libslicer::SliceRequest request;
    request.objects.push_back(fiber_infill_block());
    request.config = *library->active_config_snapshot();
    const auto result = library->slice(request);
    CHECK_FALSE(result.success);
    CHECK(result.output.path.empty());
    CHECK(std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const auto& d) {
        return !d.warning && d.message.find("Fiber motion exceeds") != std::string::npos;
    }));
}

TEST_CASE("continuous fiber rejects a machine without a cut command", "[libslicer_api][fiber-missing-cut][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "1"},
        {"outer_reinforced_perimeters_counts", "1"},
        {"reinforced_perimeters_filament", "2"},
        {"fiber_minimum_path_length", "2"},
        {"fiber_cut_gcode", ""}
    });
    REQUIRE(activated.success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    request.config = *config;
    request.generate_preview = false;
    const auto sliced = library->slice(request);

    CHECK_FALSE(sliced.success);
    REQUIRE_FALSE(sliced.diagnostics.empty());
    const bool reported_missing_cut = std::any_of(
        sliced.diagnostics.begin(), sliced.diagnostics.end(),
        [](const libslicer::SliceDiagnostic& diagnostic) {
            return diagnostic.message.find("no fiber cut command") != std::string::npos;
        });
    CHECK(reported_missing_cut);
}

TEST_CASE("continuous fiber composite fill survives an end-to-end slice", "[libslicer_api][fiber][slice]")
{
    const char* validation_model = std::getenv("LIBSLICER_FIBER_VALIDATION_MODEL");
    const int execution_case = GENERATE(0, 1, 2);
    const bool swapped_physical_tools = execution_case == 2;
    INFO("execution_case=" << execution_case);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "1"},
        {"outer_reinforced_perimeters_counts", "2"},
        {"generate_reinforced_infills", "1"},
        {"reinforced_infill_density", "40%"},
        {"reinforced_infill_pattern", "rectilinear"},
        {"reinforced_perimeters_filament", "2"},
        {"reinforced_infill_filament", "2"},
        {"fiber_layer_height_ratio", "1"},
        {"fiber_minimum_path_length", "2"},
        {"fiber_contour_infill_clearance", "0"},
        {"fiber_resin_overlap", "0.05"},
        {"fiber_cut_to_contact_length", "5"},
        {"fiber_prefeed_extra_length", "0.5"},
        {"fiber_prefeed_speed", "10"},
        {"fiber_z_hop_height", "2"},
        {"fiber_landing_length", "2"},
        {"fiber_landing_speed", "3"},
        {"fiber_adhesion_dwell_ms", "25"},
        {"fiber_start_stabilization_length", "2"},
        {"fiber_start_speed", "10"},
        {"fiber_cut_gcode", "M400\nM42 P4 S255\nG4 P100\n; TEST_FIBER_CUT"},
        {"filament_process_type", "thermoplastic;continuous_fiber"},
        {"filament_fiber_feed_correction", "1,1"},
        {"toolhead_process_capabilities", swapped_physical_tools ? "continuous_fiber;thermoplastic" : "thermoplastic;continuous_fiber"},
        {"toolhead_fiber_protocol_id", swapped_physical_tools ? "linear-e-v1;" : ";linear-e-v1"},
        {"toolhead_fiber_e_units_per_mm", swapped_physical_tools ? "2,1" : "1,2"},
        {"use_relative_e_distances", execution_case == 0 ? "0" : "1"},
        {"z_offset", "0.7"},
        {"fiber_contour_feed_ratio", "1.02"},
        {"fiber_infill_feed_ratio", "1.05"},
        {"physical_extruder_map", swapped_physical_tools ? "1,0" : "0,1"},
        {"fiber_tail_min_speed", "3"},
        {"fiber_tail_max_speed", "3"},
        {"enable_prime_tower", "0"},
        {"wipe", "1"}
    });
    std::string activation_diagnostics;
    for (const auto& issue : activated.diagnostics) activation_diagnostics += issue.key + ": " + issue.message + "\n";
    INFO(activation_diagnostics);
    REQUIRE(activated.success);
    // Synthetic, explicitly command-only machine. Production CFSYS firmware
    // macros are deliberately not treated as an audited software fixture.
    std::vector<std::pair<std::string, std::string>> script_patch;
    const auto before_scripts = library->active_config_snapshot();
    REQUIRE(before_scripts.has_value());
    for (const auto& [key, value] : before_scripts->values())
        if (key.size() >= 6 && key.compare(key.size()-6, 6, "_gcode") == 0 &&
            key != "fiber_cut_gcode" && key != "emit_machine_limits_to_gcode")
            script_patch.emplace_back(key, "");
    const auto cleared_scripts = library->apply_active_config_patch(script_patch);
    std::string script_diagnostics;
    for (const auto& issue : cleared_scripts.diagnostics) script_diagnostics += issue.key + ": " + issue.message + "\n";
    INFO(script_diagnostics);
    REQUIRE(cleared_scripts.success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_continuous_fiber_cube.gcode";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);

    libslicer::SliceRequest request;
    if (validation_model) request.objects.push_back(binary_stl_on_bed(validation_model));
    else request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/two_20mm_cubes.obj", {}}};
    request.config = *config;
    request.output_gcode_path = output.string();
    const auto sliced = library->slice(request);
    const std::string diagnostic = sliced.diagnostics.empty()
        ? std::string{}
        : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview != nullptr);

    std::size_t contour_segments = 0;
    std::size_t infill_segments = 0;
    std::size_t resin_segments = 0;
    std::size_t passive_segments = 0;
    for (const auto& segment : sliced.preview->segments) {
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive) {
            ++passive_segments;
            CHECK(segment.motion == libslicer::ToolpathMotionKind::Travel);
            CHECK(segment.fiber_feed_delta_mm == 0);
            CHECK(segment.width_mm > 0);
            CHECK(segment.height_mm > 0);
        }
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered) {
            CHECK(segment.filament_id == 1);
            REQUIRE(segment.tool_id == (swapped_physical_tools ? 0 : 1));
            const double ratio = segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour ? 1.02 : 1.05;
            CHECK(segment.fiber_feed_delta_mm == Catch::Approx(segment.deposited_path_length_mm * ratio).margin(0.002));
            CHECK(segment.end_mm.z > 0);
            CHECK(segment.start_mm.z == Catch::Approx(segment.end_mm.z));
            CHECK(segment.extrusion_delta_mm == 0); // not plastic filament millimeters
        }
        if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberContour)
            ++contour_segments;
        else if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::ContinuousFiberInfill)
            ++infill_segments;
        else if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::SparseInfill)
            ++resin_segments;
    }
    INFO("contour_segments=" << contour_segments
         << " infill_segments=" << infill_segments
         << " resin_segments=" << resin_segments);
    CHECK(contour_segments > 0);
    CHECK(infill_segments > 0);
    CHECK(resin_segments > 0);
    CHECK(passive_segments > 0);
    CHECK(sliced.preview->statistics.total_fiber_feed_mm > 0);
    CHECK(sliced.preview->statistics.total_fiber_prefeed_mm > 0);
    libslicer::GCodePreviewRequest import_request;
    import_request.gcode_path = output.string();
    const auto imported_fiber = library->load_gcode_preview(import_request);
    for (const auto& issue : imported_fiber.diagnostics) UNSCOPED_INFO(issue.message);
    REQUIRE(imported_fiber.success);
    REQUIRE(imported_fiber.preview);
    for (const auto& segment : imported_fiber.preview->segments)
        if (segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPowered ||
            segment.deposition == libslicer::ToolpathDepositionKind::ContinuousFiberPassive)
            REQUIRE(segment.tool_id == (swapped_physical_tools ? 0 : 1));
    CHECK(imported_fiber.preview->statistics.total_fiber_feed_mm ==
          Catch::Approx(sliced.preview->statistics.total_fiber_feed_mm).margin(0.02));
    CHECK(imported_fiber.preview->statistics.total_fiber_deposited_path_mm ==
          Catch::Approx(sliced.preview->statistics.total_fiber_deposited_path_mm).margin(0.02));

    std::ifstream generated(output);
    const std::string gcode((std::istreambuf_iterator<char>(generated)),
                            std::istreambuf_iterator<char>());
    CHECK(gcode.find(";FIBER_BEGIN") != std::string::npos);
    CHECK(gcode.find("purpose=contour") != std::string::npos);
    CHECK(gcode.find("purpose=infill") != std::string::npos);
    CHECK(gcode.find("component=0") != std::string::npos);
    if (validation_model == nullptr)
        CHECK(gcode.find("component=1") != std::string::npos);
    CHECK(gcode.find(";FIBER_CUT") != std::string::npos);
    CHECK(gcode.find("; TEST_FIBER_CUT") != std::string::npos);

    std::istringstream gcode_lines(gcode);
    std::string gcode_line;
    bool inside_fiber_block = false;
    bool awaiting_first_motion_after_end = false;
    bool invalid_block_nesting = false;
    bool protected_block_rewritten = false;
    bool post_end_wipe = false;
    bool block_saw_prefeed = false;
    bool block_saw_landing = false;
    bool block_saw_dwell = false;
    bool block_saw_start = false;
    bool block_saw_powered_deposition = false;
    bool invalid_start_sequence = false;
    std::size_t fiber_begin_count = 0;
    std::size_t fiber_end_count = 0;
    std::size_t valid_start_sequence_count = 0;
    while (std::getline(gcode_lines, gcode_line)) {
        if (gcode_line.rfind(";FIBER_BEGIN", 0) == 0) {
            invalid_block_nesting = invalid_block_nesting || inside_fiber_block;
            inside_fiber_block = true;
            awaiting_first_motion_after_end = false;
            block_saw_prefeed = false;
            block_saw_landing = false;
            block_saw_dwell = false;
            block_saw_start = false;
            block_saw_powered_deposition = false;
            ++fiber_begin_count;
        }
        if (inside_fiber_block) {
            protected_block_rewritten = protected_block_rewritten ||
                gcode_line.rfind(";WIPE", 0) == 0 ||
                gcode_line.find(";_WIPE") != std::string::npos ||
                gcode_line.rfind("M106", 0) == 0 ||
                gcode_line.rfind("M107", 0) == 0;

            const bool is_linear_move = gcode_line.rfind("G1 ", 0) == 0;
            const bool has_x = gcode_line.find(" X") != std::string::npos;
            const bool has_y = gcode_line.find(" Y") != std::string::npos;
            const bool has_z = gcode_line.find(" Z") != std::string::npos;
            const bool has_e = gcode_line.find(" E") != std::string::npos;
            if (!block_saw_start && is_linear_move && has_e && !has_x && !has_y)
                block_saw_prefeed = true;
            if (!block_saw_start && block_saw_prefeed && is_linear_move &&
                (has_x || has_y) && has_z && !has_e)
                block_saw_landing = true;
            if (!block_saw_start && gcode_line.rfind("G4 P25", 0) == 0)
                block_saw_dwell = true;
            if (gcode_line.rfind(";FIBER_START", 0) == 0) {
                invalid_start_sequence = invalid_start_sequence || block_saw_start ||
                    !block_saw_prefeed || !block_saw_landing || !block_saw_dwell;
                block_saw_start = true;
            } else if (block_saw_start && is_linear_move && (has_x || has_y)) {
                if (!block_saw_powered_deposition) {
                    invalid_start_sequence = invalid_start_sequence || !has_e;
                    block_saw_powered_deposition = has_e;
                }
            } else if (!block_saw_start && is_linear_move && (has_x || has_y) && has_e) {
                invalid_start_sequence = true;
            }
        }
        if (gcode_line.rfind(";FIBER_END", 0) == 0) {
            invalid_block_nesting = invalid_block_nesting || !inside_fiber_block;
            invalid_start_sequence = invalid_start_sequence || !block_saw_start ||
                !block_saw_powered_deposition;
            if (block_saw_start && block_saw_powered_deposition)
                ++valid_start_sequence_count;
            inside_fiber_block = false;
            awaiting_first_motion_after_end = true;
            ++fiber_end_count;
            continue;
        }
        if (awaiting_first_motion_after_end &&
            (gcode_line.rfind(";WIPE", 0) == 0 ||
             gcode_line.find(";_WIPE") != std::string::npos))
            post_end_wipe = true;
        if (awaiting_first_motion_after_end &&
            (gcode_line.rfind("G0 ", 0) == 0 || gcode_line.rfind("G1 ", 0) == 0)) {
            awaiting_first_motion_after_end = false;
        }
    }
    CHECK_FALSE(inside_fiber_block);
    CHECK_FALSE(invalid_block_nesting);
    CHECK_FALSE(protected_block_rewritten);
    CHECK_FALSE(post_end_wipe);
    CHECK_FALSE(invalid_start_sequence);
    CHECK(fiber_begin_count > 0);
    CHECK(fiber_begin_count == fiber_end_count);
    CHECK(valid_start_sequence_count == fiber_begin_count);

    const std::size_t tail_begin = gcode.find(";FIBER_TAIL_BEGIN");
    const std::size_t depleted = tail_begin == std::string::npos ?
        std::string::npos : gcode.find(";FIBER_DEPLETED", tail_begin);
    REQUIRE(tail_begin != std::string::npos);
    REQUIRE(depleted != std::string::npos);
    const std::string passive_tail = gcode.substr(tail_begin, depleted - tail_begin);
    std::istringstream passive_tail_lines(passive_tail);
    std::string passive_line;
    bool has_passive_xy_motion = false;
    while (std::getline(passive_tail_lines, passive_line)) {
        if (passive_line.rfind("G1 ", 0) != 0)
            continue;
        has_passive_xy_motion = has_passive_xy_motion ||
            (passive_line.find(" X") != std::string::npos || passive_line.find(" Y") != std::string::npos);
        CHECK(passive_line.find(" E") == std::string::npos);
    }
    CHECK(has_passive_xy_motion);
    if (execution_case == 1) {
        // A rejected machine event must not destroy a previously valid output.
        REQUIRE(library->apply_active_config_patch({{"fiber_cut_gcode", "UNKNOWN_CUT_MACRO"}}).success);
        request.config = *library->active_config_snapshot();
        const auto rejected = library->slice(request);
        REQUIRE_FALSE(rejected.success);
        std::ifstream retained(output);
        const std::string retained_gcode((std::istreambuf_iterator<char>(retained)), std::istreambuf_iterator<char>());
        CHECK(retained_gcode == gcode);
        CHECK_FALSE(std::filesystem::exists(output.string() + ".tmp"));
    }
    std::filesystem::remove(output, remove_error);
}

TEST_CASE("disabling continuous fiber preserves the ordinary Orca fill path", "[libslicer_api][fiber-disabled][slice]")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::ConfigSelection selection;
    selection.machine_model_id = "Flashforge Creator 5";
    selection.machine_variant_id = "0.4";
    const auto activated = library->activate_config(selection, {
        {"generate_reinforced_perimeters", "0"},
        {"generate_reinforced_infills", "0"}
    });
    REQUIRE(activated.success);
    const auto config = library->active_config_snapshot();
    REQUIRE(config.has_value());

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() / "libslicer_api_fiber_disabled_cube.gcode";
    std::error_code remove_error;
    std::filesystem::remove(output, remove_error);

    libslicer::SliceRequest request;
    request.objects = {{std::string(LIBSLICER_TEST_DATA_DIR) + "/20mm_cube.obj", {}}};
    request.config = *config;
    request.output_gcode_path = output.string();
    const auto sliced = library->slice(request);
    const std::string diagnostic = sliced.diagnostics.empty()
        ? std::string{}
        : sliced.diagnostics.front().message;
    INFO(diagnostic);
    REQUIRE(sliced.success);
    REQUIRE(sliced.preview != nullptr);

    std::size_t contour_segments = 0;
    std::size_t infill_segments = 0;
    std::size_t sparse_segments = 0;
    for (const auto& segment : sliced.preview->segments) {
        contour_segments += segment.extrusion_role ==
            libslicer::ToolpathExtrusionRole::ContinuousFiberContour;
        infill_segments += segment.extrusion_role ==
            libslicer::ToolpathExtrusionRole::ContinuousFiberInfill;
        sparse_segments += segment.extrusion_role ==
            libslicer::ToolpathExtrusionRole::SparseInfill;
    }
    CHECK(contour_segments == 0);
    CHECK(infill_segments == 0);
    CHECK(sparse_segments > 0);

    std::ifstream generated(output);
    const std::string gcode((std::istreambuf_iterator<char>(generated)),
                            std::istreambuf_iterator<char>());
    CHECK(gcode.find(";FIBER_BEGIN") == std::string::npos);
    CHECK(gcode.find(";FIBER_END") == std::string::npos);
    std::filesystem::remove(output, remove_error);
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

TEST_CASE("library imports generic G-code without embedded Orca configuration",
          "[libslicer_api][gcode][preview]")
{
    const auto gcode_path =
        std::filesystem::temp_directory_path() /
        "libslicer_api_generic_external.gcode";
    std::error_code remove_error;
    std::filesystem::remove(gcode_path, remove_error);

    {
        std::ofstream gcode(gcode_path, std::ios::binary);
        REQUIRE(gcode.good());
        gcode << R"(;Generated with Cura_SteamEngine 4.7.0
G21
G90
M82
G92 E0
;LAYER_COUNT:2
;LAYER:0
G1 Z0.2 F1200
;TYPE:WALL-OUTER
G1 X0 Y0 F3000
G1 X20 Y0 E1 F1200
G1 X20 Y20 E2
G1 X0 Y20 E3
G1 X0 Y0 E4
;LAYER:1
G1 Z0.4 F1200
;TYPE:WALL-OUTER
G1 X20 Y0 E5 F1200
G1 X20 Y20 E6
G1 X0 Y20 E7
G1 X0 Y0 E8
)";
        REQUIRE(gcode.good());
    }

    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::GCodePreviewRequest request;
    request.gcode_path = gcode_path.string();
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->statistics.total_layers == 2);
    CHECK(imported.preview->statistics.render_segment_count > 0);
    CHECK_FALSE(imported.preview->segments.empty());
    for (const auto& segment : imported.preview->segments) {
        if (segment.motion == libslicer::ToolpathMotionKind::Extrusion) {
            CHECK(segment.layer_index == (segment.end_mm.z < 0.3f ? 0 : 1));
        }
    }

    std::filesystem::remove(gcode_path, remove_error);
}

TEST_CASE("external layer tags assign motions without double counting",
          "[libslicer_api][gcode][preview][layer_tags]")
{
    std::string first = "; LAYER:1 [0.2]\r\n";
    std::string second = "; LAYER:2 [0.4]\r\n";
    std::string duplicate = first;
    bool elevated_first_move = false;
    SECTION("FibreSeek numbered layers") {}
    SECTION("whitespace and zero-based layers") {
        first = ";\t LAYER: 0 [ 0.2 ] \t\r\n";
        second = "; LAYER: 1 [0.4]\r\n";
        duplicate = first;
    }
    SECTION("sparse external numbers are contiguous internally") {
        first = ";LAYER:10\n";
        second = ";LAYER:20\n";
        duplicate = first;
    }
    SECTION("numbered tags take precedence when encountered first") {
        first += ";LAYER_CHANGE\n";
        second += "; CHANGE_LAYER\n";
    }
    SECTION("Orca change tags take precedence when encountered first") {
        first = "; CHANGE_LAYER\n;LAYER:1\n";
        second = "; CHANGE_LAYER\n;LAYER:2\n";
        duplicate = ";LAYER:1\n";
    }
    SECTION("compatible change tags remain supported") {
        first = ";LAYER_CHANGE\n";
        second = ";LAYER_CHANGE\n";
        duplicate.clear();
    }
    SECTION("compatible nominal Z is independent of landing height") {
        first = ";LAYER_CHANGE\n;Z:0.2\n";
        second = ";LAYER_CHANGE\n;Z:0.4\n";
        duplicate.clear();
        elevated_first_move = true;
    }
    SECTION("Orca nominal Z is independent of landing height") {
        first = "; CHANGE_LAYER\n; Z_HEIGHT: 0.2\n";
        second = "; CHANGE_LAYER\n; Z_HEIGHT: 0.4\n";
        duplicate.clear();
        elevated_first_move = true;
    }

    const auto path = std::filesystem::temp_directory_path() / "libslicer_layer_tags.gcode";
    {
        std::ofstream gcode(path, std::ios::binary);
        REQUIRE(gcode.good());
        gcode << "; Generated with FibreSeek Rocket Slicer\r\n"
                 "G21\nG90\nM83\nG92 E0\n; LAYER_COUNT:363\n"
                 "; LAYER:invalid\n; LAYER:9999999999999999999999\n"
                 "; LAYER:12garbage\n; LAYER:12 [bad]\n"
              << first
              << (elevated_first_move ? "G0 X0 Y0 Z2 F1200\nG1 X10 E1\n" : "G0 X0 Y0 Z0.2 F1200\nG1 X10 E1\n")
              << duplicate
              << "G0 Z2\nG0 Z0.2\nG1 X20 E1\n"
              << second
              << "G0 Z0.4\nG1 X30 E1\n";
        REQUIRE(gcode.good());
    }
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);
    libslicer::GCodePreviewRequest request;
    request.gcode_path = path.string();
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    CHECK(imported.preview->statistics.total_layers == 2);
    REQUIRE(imported.preview->layers.size() == 2);
    CHECK(imported.preview->layers[0].print_z_mm == Catch::Approx(0.2));
    CHECK(imported.preview->layers[1].print_z_mm == Catch::Approx(0.4));
    std::set<std::uint32_t> extrusion_layers;
    for (const auto& segment : imported.preview->segments) {
        if (segment.motion == libslicer::ToolpathMotionKind::Extrusion) {
            CHECK(segment.layer_index == (segment.end_mm.x <= 20.0f ? 0 : 1));
            extrusion_layers.insert(segment.layer_index);
        }
    }
    CHECK(extrusion_layers == std::set<std::uint32_t>{0, 1});
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

TEST_CASE("FibreSeek file preserves every segment's source layer",
          "[.][libslicer_api][gcode_external]")
{
    const char* path = std::getenv("LIBSLICER_TEST_GCODE_PATH");
    REQUIRE(path != nullptr);
    std::ifstream input(path);
    REQUIRE(input.good());
    std::vector<std::uint32_t> source_layers{0}; // Source command IDs are 1-based lines.
    unsigned int layer_count = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("; LAYER:", 0) == 0) {
            unsigned int source_number = 0;
            std::istringstream number(line.substr(8));
            REQUIRE(bool(number >> source_number));
            REQUIRE(source_number == layer_count + 1);
            ++layer_count;
        }
        source_layers.push_back(layer_count == 0 ? 0 : layer_count - 1);
    }
    REQUIRE(layer_count == 363);
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);
    libslicer::GCodePreviewRequest request;
    request.gcode_path = path;
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);
    REQUIRE(imported.preview->layers.size() == layer_count);
    REQUIRE(imported.preview->statistics.total_layers == layer_count);
    REQUIRE_FALSE(imported.preview->segments.empty());
    std::set<std::uint32_t> populated_layers;
    for (const auto& segment : imported.preview->segments) {
        REQUIRE(segment.source_command_id < source_layers.size());
        CHECK(segment.layer_index == source_layers[segment.source_command_id]);
        populated_layers.insert(segment.layer_index);
    }
    CHECK(populated_layers.size() == layer_count);
}

TEST_CASE("belt G-code preview reconstructs world coordinates across G92 Z reset",
          "[libslicer_api][gcode][preview][belt]")
{
    const auto gcode_path =
        std::filesystem::temp_directory_path() /
        "libslicer_api_belt_world_preview.gcode";
    std::error_code remove_error;
    std::filesystem::remove(gcode_path, remove_error);

    {
        std::ofstream gcode(gcode_path, std::ios::binary);
        REQUIRE(gcode.good());
        gcode << R"(;SLICING_KINEMATICS:BELT
;BELT_COORDINATE_VERSION:1
;BELT_GANTRY_ANGLE:45
;BELT_PLATE_MAX_WORLD_Y:100
G21
G90
M83
G1 Z20 F1200
G92 Z0
;LAYER_CHANGE
; Z_HEIGHT: 10
;TYPE:Outer wall
G1 X0 Y2 Z14.142136 F1200
G1 X1 Y2 E1
)";
        REQUIRE(gcode.good());
    }

    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors = {"Flashforge"};
    const auto library = libslicer::Library::open(options);
    REQUIRE(library != nullptr);

    libslicer::GCodePreviewRequest request;
    request.gcode_path = gcode_path.string();
    const auto imported = library->load_gcode_preview(request);
    REQUIRE(imported.success);
    REQUIRE(imported.preview != nullptr);

    const auto segment = std::find_if(
        imported.preview->segments.begin(), imported.preview->segments.end(),
        [](const libslicer::ToolpathSegment& candidate) {
            return candidate.motion == libslicer::ToolpathMotionKind::Extrusion;
        });
    REQUIRE(segment != imported.preview->segments.end());
    CHECK(std::abs(segment->start_mm.x - 0.0f) < 0.001f);
    CHECK(std::abs(segment->end_mm.x - 1.0f) < 0.001f);
    CHECK(std::abs(segment->start_mm.y - 87.27208f) < 0.001f);
    CHECK(std::abs(segment->end_mm.y - 87.27208f) < 0.001f);
    CHECK(std::abs(segment->start_mm.z - 1.414214f) < 0.001f);
    CHECK(std::abs(segment->end_mm.z - 1.414214f) < 0.001f);
    CHECK(std::abs(segment->print_z_mm - 10.0f) < 0.001f);

    std::filesystem::remove(gcode_path, remove_error);
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
