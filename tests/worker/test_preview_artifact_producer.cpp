#include "preview/PreviewArtifactProducer.hpp"

#include "libslicer_worker/PreviewReader.hpp"
#include "libslicer_worker/SliceJob.hpp"

#include <catch2/catch_all.hpp>

#include <array>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace libslicer::worker;
using namespace libslicer::worker::preview;

artifacts::PreviewArtifactOutput preview_output(const std::filesystem::path& path)
{
    artifacts::PreviewArtifactOutput output;
    output.enabled = true;
    output.required = true;
    output.path = path;
    output.format = std::string(binary_format_name);
    output.publish = "final";
    return output;
}

PreviewProducerContext preview_context(const std::filesystem::path& root)
{
    PreviewProducerContext context;
    context.job_id = "preview-producer-test";
    context.working_dir = root;
    context.data_dir = root / "data";
    context.artifacts_dir = root;
    context.input_type = "stl";
    context.input_path = root / "input.stl";
    return context;
}

Slic3r::GCodeProcessorResult::PreviewColorFact color_fact(std::uint16_t id,
                                                           std::uint16_t filament_id,
                                                           Slic3r::GCodeProcessorResult::PreviewColorSource source,
                                                           std::array<float, 4> rgba,
                                                           std::string name)
{
    Slic3r::GCodeProcessorResult::PreviewColorFact fact;
    fact.id = id;
    fact.filament_id = filament_id;
    fact.source = source;
    fact.color_rgba = rgba;
    fact.name = std::move(name);
    return fact;
}

Slic3r::ToolpathMoveVertex move(unsigned int gcode_id,
                                unsigned int layer_id,
                                std::uint8_t filament_id,
                                std::uint16_t color_id,
                                Slic3r::EMoveType type,
                                const Slic3r::Vec3f& position)
{
    Slic3r::ToolpathMoveVertex out;
    out.gcode_id = gcode_id;
    out.layer_id = layer_id;
    out.extruder_id = filament_id;
    out.cp_color_id = static_cast<unsigned char>(color_id);
    out.type = type;
    out.extrusion_role = Slic3r::erPerimeter;
    out.position = position;
    out.feedrate = 30.0f;
    out.width = 0.42f;
    out.height = 0.2f;
    out.mm3_per_mm = type == Slic3r::EMoveType::Extrude ? 0.08f : 0.0f;
    out.delta_extruder = type == Slic3r::EMoveType::Extrude ? 0.1f : 0.0f;
    out.print_z = position.z();
    return out;
}

void make_result(Slic3r::GCodeProcessorResult& result,
                 const std::vector<int>& filament_to_tool_map,
                 bool include_color_change = false)
{
    result.filaments_count = filament_to_tool_map.size();
    result.filament_to_tool_map = filament_to_tool_map;
    result.filament_maps = filament_to_tool_map;
    result.extruder_colors.resize(filament_to_tool_map.size());
    result.filament_diameters.assign(filament_to_tool_map.size(), 1.75f);
    result.filament_densities.assign(filament_to_tool_map.size(), 1.24f);
    result.filament_costs.assign(filament_to_tool_map.size(), 20.0f);

    for (std::size_t i = 0; i < filament_to_tool_map.size(); ++i) {
        const float channel = static_cast<float>(i + 1) / static_cast<float>(filament_to_tool_map.size() + 1);
        result.extruder_colors[i] = i == 0 ? "#112233" : "#445566";
        result.preview_colors[static_cast<std::uint16_t>(i)] = color_fact(
            static_cast<std::uint16_t>(i),
            static_cast<std::uint16_t>(i),
            Slic3r::GCodeProcessorResult::PreviewColorSource::Filament,
            { channel, 0.25f, 0.75f, 1.0f },
            result.extruder_colors[i]);
    }

    result.moves.push_back(move(0, 0, 0, 0, Slic3r::EMoveType::Travel, { 0.0f, 0.0f, 0.2f }));
    result.moves.push_back(move(1, 0, 0, 0, Slic3r::EMoveType::Extrude, { 1.0f, 0.0f, 0.2f }));
    if (filament_to_tool_map.size() > 1) {
        const std::uint16_t color_id = include_color_change ? 42 : 1;
        if (include_color_change) {
            result.preview_colors[color_id] = color_fact(
                color_id,
                1,
                Slic3r::GCodeProcessorResult::PreviewColorSource::ColorChange,
                { 0.9f, 0.1f, 0.2f, 1.0f },
                "#E61933");
        }
        result.moves.push_back(move(2, 0, 1, color_id, Slic3r::EMoveType::Tool_change, { 1.0f, 0.0f, 0.2f }));
        result.moves.push_back(move(3, 0, 1, color_id, Slic3r::EMoveType::Extrude, { 2.0f, 0.0f, 0.2f }));
    }
}

PreviewArtifactView write_and_read(const std::vector<int>& filament_to_tool_map,
                                   bool include_color_change,
                                   PreviewArtifactStorage& storage)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, filament_to_tool_map, include_color_change);
    CancellationToken cancellation;
    const std::filesystem::path path = root / "preview.orcapv";
    PreviewProducerResult produced = produce_preview_artifact(
        preview_output(path), preview_context(root), config, result, cancellation);
    REQUIRE(produced.success);
    REQUIRE(produced.path == path);

    storage.load(path);
    return storage.view();
}

void assert_mapping(const PreviewArtifactView& view, const std::vector<int>& expected_map)
{
    std::map<std::uint16_t, std::uint16_t> filament_to_tool;
    for (const WireFilamentRecord& filament : view.records<WireFilamentRecord>(WireSectionType::Filaments))
        filament_to_tool[filament.id] = filament.tool_id;

    REQUIRE(filament_to_tool.size() == expected_map.size());
    for (std::size_t filament_id = 0; filament_id < expected_map.size(); ++filament_id)
        CHECK(filament_to_tool[static_cast<std::uint16_t>(filament_id)] == expected_map[filament_id]);

    for (const WireMoveRecord& move : view.records<WireMoveRecord>(WireSectionType::Moves)) {
        REQUIRE(move.filament_id < expected_map.size());
        CHECK(move.tool_id == expected_map[move.filament_id]);
    }
}

} // namespace

TEST_CASE("Preview producer serializes explicit filament-to-tool mappings", "[worker][preview_artifact]")
{
    const std::vector<std::vector<int>> maps {
        { 0 },
        { 0, 0 },
        { 0, 1 },
        { 1, 0 },
        { 0, 0, 0, 0 },
    };

    for (const std::vector<int>& map : maps) {
        PreviewArtifactStorage storage;
        const PreviewArtifactView view = write_and_read(map, false, storage);
        assert_mapping(view, map);
    }
}

TEST_CASE("Preview producer serializes exact color ids referenced by moves", "[worker][preview_artifact]")
{
    PreviewArtifactStorage storage;
    const PreviewArtifactView view = write_and_read({ 0, 0 }, true, storage);

    std::set<std::uint16_t> color_ids;
    for (const WireColorRecord& color : view.records<WireColorRecord>(WireSectionType::Colors)) {
        color_ids.insert(color.id);
        if (color.id == 42) {
            CHECK(color.filament_id == 1);
            CHECK(color.source == ColorSource::ColorChange);
            CHECK(color.color_rgba[0] == Catch::Approx(0.9f));
            CHECK(color.color_rgba[1] == Catch::Approx(0.1f));
            CHECK(color.color_rgba[2] == Catch::Approx(0.2f));
            CHECK(color.color_rgba[3] == Catch::Approx(1.0f));
        }
    }
    REQUIRE(color_ids.count(42) == 1);

    bool saw_referenced_color_change = false;
    for (const WireMoveRecord& move : view.records<WireMoveRecord>(WireSectionType::Moves)) {
        if (move.cp_color_id == 42) {
            saw_referenced_color_change = true;
            CHECK(has_flag(move.flags, MoveFlags::HasCpColor));
        }
        CHECK(color_ids.count(move.cp_color_id) == 1);
    }
    CHECK(saw_referenced_color_change);
}

TEST_CASE("Preview producer rejects missing mapping facts", "[worker][preview_artifact]")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "libslicer-preview-producer-invalid-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::GCodeProcessorResult result;
    make_result(result, { 0, 0 }, false);
    result.filament_to_tool_map.pop_back();
    CancellationToken cancellation;

    PreviewProducerResult produced = produce_preview_artifact(
        preview_output(root / "preview.orcapv"), preview_context(root), config, result, cancellation);
    CHECK_FALSE(produced.success);
    CHECK(produced.code == "preview_mapping_invalid");
}
