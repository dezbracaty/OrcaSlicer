#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <libslicer/Library.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <utility>

namespace {
namespace fs = std::filesystem;
using Json   = nlohmann::json;

template<class Diagnostics> std::string describe(const Diagnostics& issues)
{
    std::string text;
    for (const auto& issue : issues)
        text += issue.message + "\n";
    return text;
}

std::unique_ptr<libslicer::Library> active_library(std::string machine = "Flashforge AD5X", std::string variant = "0.4")
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors            = {"Flashforge"};
    auto library               = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id   = std::move(machine);
    selection.machine_variant_id = std::move(variant);
    const auto activated         = library->activate_config(selection);
    INFO(describe(activated.diagnostics));
    REQUIRE(activated.success);
    REQUIRE(activated.view.filament_slots.size() == 1);
    return library;
}

libslicer::ModelSnapshot load_empty(const libslicer::Library& library)
{
    libslicer::ModelLoadRequest request;
    request.config    = *library.active_config_snapshot();
    const auto loaded = library.load_model(request);
    REQUIRE(loaded.success);
    REQUIRE(loaded.model.valid());
    return loaded.model;
}

libslicer::FilamentOperationResult append(libslicer::Library& library, const libslicer::ModelSnapshot& model)
{
    libslicer::AddFilamentRequest request;

    request.model = model;
    return library.add_active_filament(request);
}

libslicer::FilamentOperationResult erase(libslicer::Library& library,
                                         const libslicer::ModelSnapshot& model,
                                         size_t index,
                                         std::optional<size_t> replacement = std::nullopt)
{
    libslicer::RemoveFilamentRequest request;

    request.model                  = model;
    request.slot_index             = index;
    request.replacement_slot_index = replacement;
    return library.remove_active_filament(request);
}

std::shared_ptr<fs::path> temporary_directory()
{
    const auto path = fs::temp_directory_path() /
                      ("libslicer-filament-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(path));
    return {new fs::path(path), [](fs::path* directory) {
                std::error_code error;
                fs::remove_all(*directory, error);
                delete directory;
            }};
}

libslicer::SliceVolumeInput cube_input(int slot, std::string id = {})
{
    libslicer::SliceVolumeInput cube;
    cube.default_filament_slot = slot;
    cube.vertices              = {{0, 0, 0}, {8, 0, 0}, {8, 8, 0}, {0, 8, 0}, {0, 0, 2}, {8, 0, 2}, {8, 8, 2}, {0, 8, 2}};
    cube.triangles             = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                                  {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    cube.id                    = std::move(id);
    return cube;
}

libslicer::ModelSnapshot load_cube(const libslicer::Library& library, int slot)
{
    libslicer::ModelLoadRequest request;
    request.config = *library.active_config_snapshot();
    libslicer::SliceObjectInput object;
    object.name = "filament-bound-cube";
    object.volumes.push_back(cube_input(slot));
    request.objects.push_back(std::move(object));
    const auto loaded = library.load_model(request);
    INFO(describe(loaded.diagnostics));
    REQUIRE(loaded.success);
    return loaded.model;
}

libslicer::SliceResult slice(const libslicer::Library& library, const libslicer::ModelSnapshot& model, const fs::path& path)
{
    libslicer::SliceRequest request;
    request.model = model;

    request.output_gcode_path = path.string();
    request.generate_preview  = false;
    auto result               = library.slice(request);
    INFO(describe(result.diagnostics));
    REQUIRE(result.success);
    CHECK(result.summary.filament_used_mm > 0);
    CHECK(result.summary.layer_count > 0);
    return result;
}

std::vector<double> used_filament(const fs::path& path)
{
    std::ifstream input(path);
    REQUIRE(input);
    std::string line;
    const std::string prefix = "; filament used [mm] = ";
    while (std::getline(input, line)) {
        if (line.rfind(prefix, 0) != 0)
            continue;
        auto text = line.substr(prefix.size());
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream values(text);
        std::vector<double> result;
        double value;
        while (values >> value)
            result.push_back(value);
        return result;
    }
    FAIL("G-code has no filament consumption metadata");
    return {};
}
std::string tetra_model(const std::string& build_items)
{
    return R"xml(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
<metadata name="Application">BambuStudio-2.0.0</metadata>
<metadata name="BambuStudio:3mfVersion">1</metadata>
<resources><object id="1" type="model"><mesh><vertices>
<vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/>
<vertex x="0" y="10" z="0"/><vertex x="0" y="0" z="2"/>
</vertices><triangles><triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="1" v3="3"/>
<triangle v1="0" v2="3" v3="2"/><triangle v1="1" v2="2" v3="3"/>
</triangles></mesh></object></resources><build>)xml" +
           build_items + "</build></model>";
}

void write_project(const fs::path& path,
                   const Json& settings,
                   const std::string& model,
                   const std::string& model_settings = {},
                   const std::string& custom_gcodes  = {})
{
    const std::string relationships =
        R"xml(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)xml";
    const std::string content_types =
        R"xml(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)xml";
    const auto serialized = settings.dump();
    mz_zip_archive archive{};
    REQUIRE(mz_zip_writer_init_file(&archive, path.string().c_str(), 0));
    auto owner = std::shared_ptr<mz_zip_archive>(&archive, [](mz_zip_archive* value) { mz_zip_writer_end(value); });
    REQUIRE(mz_zip_writer_add_mem(&archive, "_rels/.rels", relationships.data(), relationships.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&archive, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_add_mem(&archive, "3D/3dmodel.model", model.data(), model.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(
        mz_zip_writer_add_mem(&archive, "Metadata/project_settings.config", serialized.data(), serialized.size(), MZ_DEFAULT_COMPRESSION));
    if (!model_settings.empty())
        REQUIRE(mz_zip_writer_add_mem(&archive, "Metadata/model_settings.config", model_settings.data(), model_settings.size(),
                                      MZ_DEFAULT_COMPRESSION));
    if (!custom_gcodes.empty())
        REQUIRE(mz_zip_writer_add_mem(&archive, "Metadata/custom_gcode_per_layer.xml", custom_gcodes.data(), custom_gcodes.size(),
                                      MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&archive));
    owner.reset();
}

std::string plate_xml(int plate, int instance, const std::string& map)
{
    return "<plate><metadata key=\"plater_id\" value=\"" + std::to_string(plate) + "\"/>" +
           "<metadata key=\"filament_map_mode\" value=\"Manual\"/>" + "<metadata key=\"filament_maps\" value=\"" + map + "\"/>" +
           "<model_instance><metadata key=\"object_id\" value=\"1\"/>" + "<metadata key=\"instance_id\" value=\"" +
           std::to_string(instance) + "\"/>" + "</model_instance></plate>";
}

Json project_settings(const libslicer::ConfigSnapshot& snapshot)
{
    Json settings = {{"name", "project_settings"}};
    for (const auto& [key, value] : snapshot.values())
        settings[key] = value;
    return settings;
}

libslicer::ToolpathPreviewPtr read_preview(const libslicer::Library& library, const fs::path& path)
{
    libslicer::GCodePreviewRequest request;
    request.gcode_path = path.string();
    const auto loaded  = library.load_gcode_preview(request);
    INFO(describe(loaded.diagnostics));
    REQUIRE(loaded.success);
    REQUIRE(loaded.preview);
    return loaded.preview;
}

std::pair<std::unique_ptr<libslicer::Library>, libslicer::ProjectImportResult> import_permuted_alpha500(const fs::path& path,
                                                                                                        const std::string& plate_map)
{
    libslicer::LibraryOptions options;
    options.resource_directory = LIBSLICER_TEST_RESOURCE_DIR;
    options.vendors            = {"CFSYS"};
    auto library               = libslicer::Library::open(options);
    REQUIRE(library);
    libslicer::ConfigSelection selection;
    selection.machine_model_id        = "CFSYS Alpha500 Printer";
    selection.machine_variant_id      = "0.4";
    selection.process_preset_id       = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids     = {"CFSYS PLA", "CFSYS CCF"};
    selection.filament_physical_tools = {0, 1};
    REQUIRE(library->activate_config(selection).success);
    auto settings = project_settings(*library->active_config_snapshot());
    // This full project declares the same physical T0/T1 hardware in the
    // opposite logical ordering. No read-only patch or production profile edit.
    settings["physical_extruder_map"]          = "1,0";
    settings["filament_map"]                   = "2,1";
    settings["filament_map_mode"]              = "Manual";
    settings["generate_reinforced_perimeters"] = "0";
    settings["generate_reinforced_infills"]    = "0";
    settings["enable_support"]                 = "0";
    // Retain explicitly customized project values when installed presets load.
    settings["different_settings_to_system"] = Json::array({"generate_reinforced_perimeters;generate_reinforced_infills;enable_support", "",
                                                            "", "physical_extruder_map;filament_map;filament_map_mode"});
    write_project(path, settings, tetra_model(R"xml(<item objectid="1" transform="1 0 0 0 1 0 0 0 1 80 80 0"/>)xml"),
                  "<config>" + plate_xml(1, 0, plate_map) + "</config>");
    libslicer::ProjectImportRequest import_request;
    import_request.path = path.string();
    auto imported       = library->import_project(import_request);
    INFO(describe(imported.diagnostics));
    INFO(describe(imported.filament_edit_diagnostics));
    REQUIRE(imported.success);
    REQUIRE(imported.can_edit_filaments);
    REQUIRE(library->active_config_snapshot()->value("physical_extruder_map") == "1,0");
    REQUIRE(library->active_config_snapshot()->value("filament_map") == "2,1");
    return {std::move(library), std::move(imported)};
}

} // namespace

TEST_CASE("filament JSON sequences preserve material order and reject invalid changes", "[libslicer_api][filament-operations][json]")
{
    std::ifstream input(fs::path(LIBSLICER_TEST_DATA_DIR) / "filament_operations/cases.json");
    REQUIRE(input);
    const auto scenarios = Json::parse(input);
    for (const auto& scenario : scenarios.at("cases")) {
        DYNAMIC_SECTION(scenario.at("id").get<std::string>())
        {
            auto library        = active_library(scenarios.at("machine").get<std::string>(), scenarios.at("variant").get<std::string>());
            const auto& initial = scenarios.at("initial_color");
            REQUIRE(library
                        ->set_active_filament_color(0, {initial.at(0).get<uint8_t>(), initial.at(1).get<uint8_t>(),
                                                        initial.at(2).get<uint8_t>(), initial.at(3).get<uint8_t>()})
                        .success);
            auto model = load_empty(*library);
            for (const auto& step : scenario.at("steps")) {
                CAPTURE(step.dump());
                const auto before   = *library->active_config_snapshot();
                const auto revision = library->active_config()->revision;
                libslicer::FilamentOperationResult result;
                if (step.at("op") == "add") {
                    libslicer::AddFilamentRequest request;

                    request.model = model;
                    if (step.contains("color")) {
                        const auto& color = step.at("color");
                        request.color = libslicer::Rgba8{color.at(0).get<uint8_t>(), color.at(1).get<uint8_t>(), color.at(2).get<uint8_t>(),
                                                         color.at(3).get<uint8_t>()};
                    }
                    result = library->add_active_filament(request);
                } else {
                    std::optional<size_t> replacement;
                    if (step.contains("replacement"))
                        replacement = step.at("replacement").get<size_t>();
                    result = erase(*library, model, step.at("index").get<size_t>(), replacement);
                }
                INFO(describe(result.diagnostics));
                REQUIRE(result.success == step.at("success").get<bool>());
                if (result.success) {
                    REQUIRE(result.model.valid());
                    CHECK(result.view.revision == revision + 1);
                    CHECK(result.config.values() == library->active_config_snapshot()->values());
                    model = result.model;
                } else {
                    CHECK_FALSE(result.diagnostics.empty());
                    CHECK(library->active_config()->revision == revision);
                    CHECK(library->active_config_snapshot()->values() == before.values());
                }
            }
            const auto view = *library->active_config();
            REQUIRE(view.filament_slots.size() == scenario.at("colors").size());
            for (size_t i = 0; i < view.filament_slots.size(); ++i) {
                const auto& actual   = view.filament_slots[i].color;
                const auto& expected = scenario.at("colors").at(i);
                CHECK(actual.red == expected.at(0).get<uint8_t>());
                CHECK(actual.green == expected.at(1).get<uint8_t>());
                CHECK(actual.blue == expected.at(2).get<uint8_t>());
                CHECK(actual.alpha == expected.at(3).get<uint8_t>());
            }
            CHECK(library->validate_active_config().empty());
        }
    }
}

TEST_CASE("sequential material operations use each returned current snapshot", "[libslicer_api][filament-operations]")
{
    auto library        = active_library();
    const auto original = append(*library, load_empty(*library));
    INFO(describe(original.diagnostics));
    REQUIRE(original.success);
    REQUIRE(original.view.filament_slots.size() == 2);
    const auto original_values = original.config.values();
    const auto reduced         = erase(*library, original.model, 1);
    INFO(describe(reduced.diagnostics));
    REQUIRE(reduced.success);
    REQUIRE(reduced.view.filament_slots.size() == 1);
    const auto restored_count = append(*library, reduced.model);
    INFO(describe(restored_count.diagnostics));
    REQUIRE(restored_count.success);
    REQUIRE(restored_count.view.filament_slots.size() == 2);
    const auto removed_again = erase(*library, restored_count.model, 1);
    INFO(describe(removed_again.diagnostics));
    REQUIRE(removed_again.success);
    CHECK(removed_again.view.filament_slots.size() == 1);
    CHECK(removed_again.config.values() == library->active_config_snapshot()->values());
    CHECK(original.config.values() == original_values);
    CHECK(original.model.valid());
}

TEST_CASE("ordinary edits and replacing a preset in the same slot preserve model usability", "[libslicer_api][filament-operations]")
{
    auto library     = active_library();
    const auto model = load_empty(*library);
    REQUIRE(library->set_active_config_value("layer_height", "0.24").success);
    REQUIRE(library->set_active_filament_color(0, {12, 34, 56, 255}).success);
    const auto view        = *library->active_config();
    const auto& slot       = view.filament_slots.front();
    const auto alternative = std::find_if(slot.compatible_presets.begin(), slot.compatible_presets.end(),
                                          [&](const auto& preset) { return preset.id != slot.preset_id; });
    REQUIRE(alternative != slot.compatible_presets.end());
    REQUIRE(library->set_active_filament_preset(0, alternative->id).success);
    const auto added = append(*library, model);
    INFO(describe(added.diagnostics));
    REQUIRE(added.success);
    CHECK(added.config.value("layer_height") == "0.24");
    CHECK(added.view.filament_slots.front().preset_id == alternative->id);
}

TEST_CASE("a model snapshot freezes slicing parameters until the model is captured again", "[libslicer_api][filament-operations][slice]")
{
    auto library = active_library();
    REQUIRE(library->set_active_config_value("layer_height", "0.2").success);
    const auto original_model = load_cube(*library, 1);
    const auto directory      = temporary_directory();
    const auto original       = slice(*library, original_model, *directory / "before-settings-change.gcode");
    REQUIRE(original.summary.layer_count > 1);

    REQUIRE(library->set_active_config_value("layer_height", "0.1").success);
    const auto frozen = slice(*library, original_model, *directory / "frozen-settings.gcode");
    CHECK(frozen.summary.layer_count == original.summary.layer_count);
    CHECK(frozen.summary.filament_used_mm == Catch::Approx(original.summary.filament_used_mm));
    CHECK(frozen.summary.logical_motion_count == original.summary.logical_motion_count);

    const auto recaptured_model = load_cube(*library, 1);
    const auto refreshed        = slice(*library, recaptured_model, *directory / "recaptured-settings.gcode");
    CHECK(refreshed.summary.layer_count > original.summary.layer_count);
}

TEST_CASE("deleting the assigned material slices the remapped model without changing the old snapshot",
          "[libslicer_api][filament-operations][slice]")
{
    auto library     = active_library();
    const auto added = append(*library, load_empty(*library));
    REQUIRE(added.success);
    const auto original_model = load_cube(*library, 2);
    const auto directory      = temporary_directory();
    const auto original       = slice(*library, original_model, *directory / "before.gcode");
    const auto original_usage = used_filament(*directory / "before.gcode");
    REQUIRE(original_usage.size() == 2);
    CHECK(original_usage[0] == Catch::Approx(0));
    CHECK(original_usage[1] > 0);
    const auto removed = erase(*library, original_model, 1, 0);
    INFO(describe(removed.diagnostics));
    REQUIRE(removed.success);
    const auto changed       = slice(*library, removed.model, *directory / "after.gcode");
    const auto changed_usage = used_filament(*directory / "after.gcode");
    REQUIRE(changed_usage.size() == 1);
    CHECK(changed_usage[0] > 0);
    CHECK(changed.summary.filament_used_mm == Catch::Approx(original.summary.filament_used_mm));
    const auto again = slice(*library, original_model, *directory / "original-again.gcode");
    CHECK(again.summary.filament_used_mm == Catch::Approx(original.summary.filament_used_mm));
    CHECK(again.summary.logical_motion_count == original.summary.logical_motion_count);
    CHECK(used_filament(*directory / "original-again.gcode") == original_usage);
}

TEST_CASE("a real 3MF archive retains editable native state through add remove and slicing", "[libslicer_api][filament-operations][3mf]")
{
    auto library               = active_library();
    const auto directory       = temporary_directory();
    const auto path            = *directory / "filament-project.3mf";
    Json settings              = {{"name", "project_settings"}};
    const auto original_config = *library->active_config_snapshot();
    for (const auto& [key, value] : original_config.values())
        settings[key] = value;
    write_project(path, settings, tetra_model(R"xml(<item objectid="1" transform="1 0 0 0 1 0 0 0 1 80 80 0"/>)xml"));
    libslicer::ProjectImportRequest request;
    request.path        = path.string();
    const auto imported = library->import_project(request);
    INFO(describe(imported.diagnostics));
    INFO(describe(imported.filament_edit_diagnostics));
    REQUIRE(imported.success);
    REQUIRE(imported.can_edit_filaments);
    REQUIRE(imported.model.valid());
    const auto added = append(*library, imported.model);
    INFO(describe(added.diagnostics));
    REQUIRE(added.success);
    const auto removed = erase(*library, added.model, 0, 1);
    REQUIRE(removed.success);
    const auto changed  = slice(*library, removed.model, *directory / "changed.gcode");
    const auto original = slice(*library, imported.model, *directory / "original.gcode");
    CHECK(changed.summary.layer_count == original.summary.layer_count);
    CHECK(changed.summary.filament_used_mm == Catch::Approx(original.summary.filament_used_mm));
}

TEST_CASE("two imported plates retain separate instances and export plate-local coordinates",
          "[libslicer_api][filament-operations][3mf][plate]")
{
    auto library         = active_library();
    const auto directory = temporary_directory();
    const auto path      = *directory / "two-plates.3mf";
    // Orca's two-plate grid has two columns, with origin(plate 2) = 220 * 1.2 = 264.
    // The same object has one instance on each plate, at the same plate-local XY.
    const auto model = tetra_model(
        R"xml(<item objectid="1" transform="1 0 0 0 1 0 0 0 1 80 80 0"/>
<item objectid="1" transform="1 0 0 0 1 0 0 0 1 344 80 0"/>)xml");
    const auto metadata      = "<config>" + plate_xml(1, 0, "1") + plate_xml(2, 1, "1") + "</config>";
    const std::string events = R"xml(<custom_gcodes_per_layer>
<plate><plate_info id="1"/><layer top_z="0.8" type="4" extruder="0" color="" extra="M118 SNAPSHOT_PLATE_1"/><mode value="SingleExtruder"/></plate>
<plate><plate_info id="2"/><layer top_z="0.8" type="4" extruder="0" color="" extra="M118 SNAPSHOT_PLATE_2"/><mode value="SingleExtruder"/></plate>
</custom_gcodes_per_layer>)xml";
    write_project(path, project_settings(*library->active_config_snapshot()), model, metadata, events);
    libslicer::ProjectImportRequest import_request;
    import_request.path = path.string();
    const auto imported = library->import_project(import_request);
    INFO(describe(imported.diagnostics));
    REQUIRE(imported.success);
    REQUIRE(imported.can_edit_filaments);
    const auto added = append(*library, imported.model);
    REQUIRE(added.success);
    const auto removed = erase(*library, added.model, 1);
    REQUIRE(removed.success);
    std::vector<libslicer::SliceResult> outputs;
    for (size_t plate = 0; plate < 2; ++plate) {
        libslicer::SliceRequest request;
        request.model = removed.model;

        request.plate_index       = plate;
        request.output_gcode_path = (*directory / ("plate-" + std::to_string(plate) + ".gcode")).string();
        request.generate_preview  = false;
        auto result               = library->slice(request);
        INFO(describe(result.diagnostics));
        REQUIRE(result.success);
        std::ifstream generated(request.output_gcode_path);
        const std::string text{std::istreambuf_iterator<char>(generated), std::istreambuf_iterator<char>()};
        CHECK(text.find("M118 SNAPSHOT_PLATE_" + std::to_string(plate + 1)) != std::string::npos);
        CHECK(text.find("M118 SNAPSHOT_PLATE_" + std::to_string(2 - plate)) == std::string::npos);
        const auto preview = read_preview(*library, request.output_gcode_path);
        size_t walls       = 0;
        for (const auto& segment : preview->segments) {
            if (segment.motion != libslicer::ToolpathMotionKind::Extrusion ||
                (segment.extrusion_role != libslicer::ToolpathExtrusionRole::OuterWall &&
                 segment.extrusion_role != libslicer::ToolpathExtrusionRole::InnerWall))
                continue;
            ++walls;
            for (const auto point : {segment.start_mm, segment.end_mm}) {
                CHECK(point.x >= 79.5f);
                CHECK(point.x <= 90.5f);
                CHECK(point.y >= 79.5f);
                CHECK(point.y <= 90.5f);
            }
        }
        CHECK(walls > 0);
        outputs.push_back(std::move(result));
    }
    REQUIRE(outputs.size() == 2);
    CHECK(outputs[0].summary.filament_used_mm > 0);
    CHECK(outputs[0].summary.filament_used_mm == Catch::Approx(outputs[1].summary.filament_used_mm));
    CHECK(outputs[0].summary.layer_count == outputs[1].summary.layer_count);
    libslicer::SliceRequest missing_plate;
    missing_plate.model = removed.model;

    missing_plate.plate_index = 2;
    CHECK_FALSE(library->slice(missing_plate).success);
}

TEST_CASE("an imported CFSYS plate keeps an appended T0 material on a permuted logical tool",
          "[libslicer_api][filament-operations][3mf][tool-map]")
{
    const auto directory     = temporary_directory();
    auto [library, imported] = import_permuted_alpha500(*directory / "permuted-alpha500.3mf", "2 1");
    libslicer::AddFilamentRequest add;
    add.model = imported.model;

    add.physical_tool = 0;
    const auto added  = library->add_active_filament(add);
    INFO(describe(added.diagnostics));
    REQUIRE(added.success);
    REQUIRE(added.view.filament_slots.size() == 3);
    CHECK(added.view.filament_slots.back().physical_tool_index == 0);
    REQUIRE(added.config.value("filament_map") == "2,1,2");
    const auto output  = *directory / "permuted.gcode";
    const auto result  = slice(*library, added.model, output);
    const auto preview = read_preview(*library, output);
    REQUIRE(preview->filaments.size() == 3);
    CHECK(preview->filaments[0].tool_id == 0);
    CHECK(preview->filaments[1].tool_id == 1);
    CHECK(preview->filaments[2].tool_id == 0);
    size_t printed = 0;
    for (const auto& segment : preview->segments) {
        if (segment.motion == libslicer::ToolpathMotionKind::Extrusion &&
            segment.deposition == libslicer::ToolpathDepositionKind::Thermoplastic) {
            ++printed;
            CHECK(segment.tool_id == 0);
        }
    }
    CHECK(printed > 0);
    std::ifstream gcode(output);
    const std::string text((std::istreambuf_iterator<char>(gcode)), std::istreambuf_iterator<char>());
    CHECK(text.find("; filament_map = 2,1,2") != std::string::npos);
    CHECK(text.find("; physical_extruder_map = 1,0") != std::string::npos);
}

TEST_CASE("an invalid imported plate tool map is rejected after plate overlay without changing active state",
          "[libslicer_api][filament-operations][3mf][tool-map][invalid-plate]")
{
    const auto directory = temporary_directory();
    // Global ownership is valid (2,1). Only the plate override is malicious.
    auto [library, imported] = import_permuted_alpha500(*directory / "invalid-plate-map.3mf", "99 1");
    const auto original      = *library->active_config_snapshot();
    const auto revision      = library->active_config()->revision;
    REQUIRE(library->validate_active_config().empty());

    libslicer::SliceRequest request;
    request.model = imported.model;

    request.output_gcode_path = (*directory / "invalid-plate.gcode").string();
    request.generate_preview  = false;
    const auto sliced         = library->slice(request);
    INFO(describe(sliced.diagnostics));
    CHECK_FALSE(sliced.success);
    REQUIRE_FALSE(sliced.diagnostics.empty());
    CHECK(describe(sliced.diagnostics).find("Invalid tool mapping") != std::string::npos);
    CHECK_FALSE(fs::exists(request.output_gcode_path));
    CHECK_FALSE(fs::exists(request.output_gcode_path + ".tmp"));
    CHECK(library->active_config()->revision == revision);
    CHECK(library->active_config_snapshot()->values() == original.values());

    libslicer::AddFilamentRequest add;
    add.model = imported.model;

    add.physical_tool = 0;
    const auto added  = library->add_active_filament(add);
    INFO(describe(added.diagnostics));
    CHECK_FALSE(added.success);
    REQUIRE_FALSE(added.diagnostics.empty());
    CHECK(describe(added.diagnostics).find("Invalid tool mapping") != std::string::npos);
    CHECK(library->active_config()->revision == revision);
    CHECK(library->active_config_snapshot()->values() == original.values());
    CHECK(library->active_config()->filament_slots.size() == 2);
}

TEST_CASE("material edits project caller identities raw bindings and remapped facets before actual slicing",
          "[libslicer_api][filament-operations][caller-projection]")
{
    auto library = active_library();
    auto first   = append(*library, load_empty(*library));
    REQUIRE(first.success);
    auto second = append(*library, first.model);
    REQUIRE(second.success);
    REQUIRE(second.view.filament_slots.size() == 3);

    libslicer::ModelLoadRequest input;
    input.config                = *library->active_config_snapshot();
    input.center_on_build_plate = false;
    input.filament_defaults     = {{"config-2", 2}, {"config-3", 3}, {"automatic", 0}};
    libslicer::SliceObjectInput object;
    object.id                    = "object-db-17";
    object.default_filament_slot = 3;
    // Explicit instances form the complete list. This ignored legacy pose
    // would place the model outside the bed if accidentally added as a third.
    object.transform[3] = 5000;
    auto pose           = object.transform;
    pose[3]             = 20;
    pose[7]             = 20;
    object.instances.push_back(pose);
    pose[3] = 50;
    object.instances.push_back(pose);
    auto inherited = cube_input(0, "part-db-22");
    // Leaf states 2 and 3 in TriangleSelector's public nibble encoding.
    inherited.facet_labels.roots     = {{2, 0}, {3, 4}};
    inherited.facet_labels.bitstream = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0};
    object.volumes.push_back(inherited);
    auto assigned = cube_input(2, "part-db-23");
    for (auto& vertex : assigned.vertices)
        vertex.x += 12;
    object.volumes.push_back(std::move(assigned));
    input.objects.push_back(std::move(object));
    const auto loaded = library->load_model(input);
    INFO(describe(loaded.diagnostics));
    REQUIRE(loaded.success);
    const auto removed = erase(*library, loaded.model, 1);
    INFO(describe(removed.diagnostics));
    REQUIRE(removed.success);
    REQUIRE(removed.objects.size() == 1);
    const auto& projected = removed.objects.front();
    CHECK(projected.id == "object-db-17");
    CHECK(projected.default_filament_slot == 2);
    REQUIRE(projected.volumes.size() == 2);
    const auto& inherited_result = projected.volumes[0];
    CHECK(inherited_result.id == "part-db-22");
    CHECK(inherited_result.default_filament_slot == 0);
    CHECK(inherited_result.effective_filament_slot == 2);
    REQUIRE(inherited_result.facet_labels.roots.size() == 1);
    CHECK(inherited_result.facet_labels.roots[0].triangle_index == 3);
    CHECK(inherited_result.facet_labels.roots[0].bitstream_start_index == 0);
    CHECK((inherited_result.facet_labels.bitstream == std::vector<std::uint8_t>{0, 0, 0, 1}));
    CHECK(projected.volumes[1].id == "part-db-23");
    CHECK(projected.volumes[1].default_filament_slot == 1);
    CHECK(projected.volumes[1].effective_filament_slot == 1);
    CHECK(projected.volumes[1].facet_labels.empty());
    CHECK((removed.filament_defaults == std::vector<std::pair<std::string, int>>{{"config-2", 1}, {"config-3", 2}, {"automatic", 0}}));

    const auto directory = temporary_directory();
    const auto output    = *directory / "caller-projection.gcode";
    slice(*library, removed.model, output);
    const auto used = used_filament(output);
    REQUIRE(used.size() == 2);
    CHECK(used[0] > 0);
    CHECK(used[1] > 0);
}

TEST_CASE("caller material defaults use native object rules without creating printable carriers",
          "[libslicer_api][filament-operations][caller-projection]")
{
    auto library = active_library();
    libslicer::ModelLoadRequest input;
    input.config            = *library->active_config_snapshot();
    input.filament_defaults = {{"default", 1}, {"automatic", 0}, {"stale-default", 9}};
    const auto loaded       = library->load_model(input);
    REQUIRE(loaded.success);
    const auto added = append(*library, loaded.model);
    INFO(describe(added.diagnostics));
    REQUIRE(added.success);
    CHECK(added.objects.empty());
    CHECK((added.filament_defaults == std::vector<std::pair<std::string, int>>{{"default", 1}, {"automatic", 0}, {"stale-default", 1}}));
    libslicer::SliceRequest slice_request;
    slice_request.model    = added.model;
    const auto empty_slice = library->slice(slice_request);
    CHECK_FALSE(empty_slice.success);
    CHECK(describe(empty_slice.diagnostics).find("no printable") != std::string::npos);
}

TEST_CASE("multi-instance caller objects retain Orca single-volume tree semantics",
          "[libslicer_api][filament-operations][caller-projection]")
{
    for (bool multi_instance : {false, true}) {
        CAPTURE(multi_instance);
        auto library = active_library();
        auto first   = append(*library, load_empty(*library));
        REQUIRE(first.success);
        auto second = append(*library, first.model);
        REQUIRE(second.success);
        libslicer::ModelLoadRequest input;
        input.config = *library->active_config_snapshot();
        libslicer::SliceObjectInput object;
        object.id                    = "grouped-object";
        object.default_filament_slot = 3;
        object.volumes.push_back(cube_input(2, "grouped-part"));
        object.instances.push_back(object.transform);
        if (multi_instance) {
            auto other = object.transform;
            other[3]   = 20;
            object.instances.push_back(other);
        }
        input.objects.push_back(object);
        const auto loaded = library->load_model(input);
        REQUIRE(loaded.success);
        const auto removed = erase(*library, loaded.model, 1);
        INFO(describe(removed.diagnostics));
        REQUIRE(removed.success);
        REQUIRE(removed.objects.size() == 1);
        REQUIRE(removed.objects[0].volumes.size() == 1);
        CHECK(removed.objects[0].default_filament_slot == 2);
        CHECK(removed.objects[0].volumes[0].default_filament_slot == (multi_instance ? 2 : 1));
        CHECK(removed.objects[0].volumes[0].effective_filament_slot == (multi_instance ? 2 : 1));
    }
}

TEST_CASE("invalid caller identities defaults and grouped fiber masks leave active configuration unchanged",
          "[libslicer_api][filament-operations][caller-projection]")
{
    auto library        = active_library();
    const auto revision = library->active_config()->revision;
    const auto values   = library->active_config_snapshot()->values();
    libslicer::ModelLoadRequest input;
    input.config = *library->active_config_snapshot();
    libslicer::SliceObjectInput object;
    object.id = "caller-object";
    object.volumes.push_back(cube_input(1, "caller-part"));
    input.objects.push_back(object);
    SECTION("duplicate object identity") { input.objects.push_back(object); }
    SECTION("duplicate part identity") { input.objects[0].volumes.push_back(object.volumes[0]); }
    SECTION("duplicate configuration identity") { input.filament_defaults = {{"config", 1}, {"config", 1}}; }
    SECTION("negative configuration reference") { input.filament_defaults = {{"config", -1}}; }
    SECTION("grouped instances cannot share instance-specific fiber masks")
    {
        input.objects[0].instances.push_back(object.transform);
        input.objects[0].fiber_masks.emplace_back();
    }
    const auto loaded = library->load_model(input);
    CHECK_FALSE(loaded.success);
    CHECK_FALSE(loaded.model.valid());
    CHECK_FALSE(loaded.diagnostics.empty());
    CHECK(library->active_config()->revision == revision);
    CHECK(library->active_config_snapshot()->values() == values);
}
