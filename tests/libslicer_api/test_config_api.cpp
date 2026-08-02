#include <catch2/catch_test_macros.hpp>

#include <libslicer/Config.hpp>
#include <libslicer/Library.hpp>

#include <algorithm>
#include <filesystem>
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
    auto config = libslicer::Config::defaults();
    REQUIRE(config.set("layer_height", "0.18").success);
    const auto snapshot = config.snapshot();

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
}
