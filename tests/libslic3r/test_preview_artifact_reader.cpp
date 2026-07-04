#include "libslicer_worker/PreviewReader.hpp"

#include <catch2/catch_all.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using namespace libslicer::worker::preview;

std::uint64_t align8(std::uint64_t value)
{
    return (value + 7u) & ~std::uint64_t(7u);
}

void append_padding(std::vector<std::byte>& bytes, std::uint64_t aligned_offset)
{
    while (bytes.size() < aligned_offset)
        bytes.push_back(std::byte { 0 });
}

template <class T>
void append_object(std::vector<std::byte>& bytes, const T& value)
{
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(T));
}

struct ArtifactFixture {
    std::vector<std::byte> bytes;
    std::uint64_t section_table_offset { 0 };
};

ArtifactFixture make_minimal_artifact()
{
    const std::string metadata = R"({"schema":"orca.toolpath_preview","format":"orca-toolpath-preview-binary-v1"})";

    WireLayerRecord layer;
    layer.id = 0;
    layer.move_begin = 0;
    layer.move_count = 1;

    WireToolRecord tool;
    tool.id = 0;
    tool.filament_id = 0;

    WireFilamentRecord filament;
    filament.id = 0;
    filament.tool_id = 0;

    WireColorRecord color;
    color.id = 0;
    color.filament_id = 0;
    color.source = ColorSource::Filament;

    WireMoveRecord move;
    move.id = 0;
    move.layer_id = 0;
    move.tool_id = 0;
    move.filament_id = 0;
    move.cp_color_id = 0;
    move.flags = static_cast<std::uint32_t>(MoveFlags::HasTool) |
        static_cast<std::uint32_t>(MoveFlags::HasFilament) |
        static_cast<std::uint32_t>(MoveFlags::HasCpColor);

    struct Payload {
        WireSectionType type;
        std::uint32_t record_size;
        const void* data;
        std::uint64_t size;
    };
    const std::vector<Payload> payloads = {
        { WireSectionType::MetadataJson, 1, metadata.data(), metadata.size() },
        { WireSectionType::Layers, sizeof(WireLayerRecord), &layer, sizeof(layer) },
        { WireSectionType::Tools, sizeof(WireToolRecord), &tool, sizeof(tool) },
        { WireSectionType::Filaments, sizeof(WireFilamentRecord), &filament, sizeof(filament) },
        { WireSectionType::Colors, sizeof(WireColorRecord), &color, sizeof(color) },
        { WireSectionType::Moves, sizeof(WireMoveRecord), &move, sizeof(move) },
    };

    ArtifactFixture fixture;
    WireFileHeader header;
    header.section_count = static_cast<std::uint16_t>(payloads.size());
    fixture.bytes.resize(sizeof(WireFileHeader));

    std::vector<WireSectionHeader> sections;
    for (const Payload& payload : payloads) {
        const std::uint64_t offset = align8(fixture.bytes.size());
        append_padding(fixture.bytes, offset);

        WireSectionHeader section;
        section.section = payload.type;
        section.record_size = payload.record_size;
        section.offset = offset;
        section.size = payload.size;
        section.count = payload.size / payload.record_size;
        sections.push_back(section);

        const auto* first = reinterpret_cast<const std::byte*>(payload.data);
        fixture.bytes.insert(fixture.bytes.end(), first, first + payload.size);

        if (payload.type == WireSectionType::MetadataJson) {
            header.metadata_json_offset = section.offset;
            header.metadata_json_size = section.size;
        }
    }

    fixture.section_table_offset = align8(fixture.bytes.size());
    header.section_table_offset = fixture.section_table_offset;
    append_padding(fixture.bytes, fixture.section_table_offset);
    for (const WireSectionHeader& section : sections)
        append_object(fixture.bytes, section);
    header.file_size = fixture.bytes.size();
    std::memcpy(fixture.bytes.data(), &header, sizeof(header));
    return fixture;
}

std::filesystem::path write_artifact(const std::vector<std::byte>& bytes, const std::string& name)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

template <class Mutator>
void require_rejected(const std::string& name, Mutator mutator)
{
    ArtifactFixture fixture = make_minimal_artifact();
    mutator(fixture);
    const std::filesystem::path path = write_artifact(fixture.bytes, name);
    PreviewArtifactStorage storage;
    storage.load(path);
    REQUIRE_THROWS(storage.view());
    std::filesystem::remove(path);
}

WireSectionHeader& section_at(ArtifactFixture& fixture, std::size_t index)
{
    return *reinterpret_cast<WireSectionHeader*>(fixture.bytes.data() + fixture.section_table_offset + index * sizeof(WireSectionHeader));
}

template <class Record>
Record& first_record(ArtifactFixture& fixture, std::size_t section_index)
{
    WireSectionHeader& section = section_at(fixture, section_index);
    return *reinterpret_cast<Record*>(fixture.bytes.data() + section.offset);
}

} // namespace

TEST_CASE("PreviewArtifactStorage accepts minimal valid artifact", "[preview_artifact]")
{
    ArtifactFixture fixture = make_minimal_artifact();
    const std::filesystem::path path = write_artifact(fixture.bytes, "orcapv-reader-valid.orcapv");
    PreviewArtifactStorage storage;
    storage.load(path);
    const PreviewArtifactView view = storage.view();
    CHECK(view.records<WireMoveRecord>(WireSectionType::Moves).size() == 1);
    std::filesystem::remove(path);
}

TEST_CASE("PreviewArtifactStorage rejects corrupted section tables", "[preview_artifact]")
{
    require_rejected("orcapv-reader-duplicate.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 1).section = WireSectionType::MetadataJson;
    });

    require_rejected("orcapv-reader-unknown.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 1).section = static_cast<WireSectionType>(9999);
    });

    require_rejected("orcapv-reader-overlap.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 2).offset = section_at(fixture, 1).offset;
    });

    require_rejected("orcapv-reader-metadata-overlap.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 1).offset = section_at(fixture, 0).offset;
    });

    require_rejected("orcapv-reader-wrong-record-size.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 5).record_size = 1;
    });

    require_rejected("orcapv-reader-misaligned.orcapv", [](ArtifactFixture& fixture) {
        ++section_at(fixture, 5).offset;
    });

    require_rejected("orcapv-reader-missing-layer.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 1).section = WireSectionType::Objects;
    });

    require_rejected("orcapv-reader-missing-moves.orcapv", [](ArtifactFixture& fixture) {
        WireSectionHeader& moves = section_at(fixture, 5);
        moves.section = WireSectionType::Events;
        moves.record_size = sizeof(WireEventRecord);
        moves.offset = 0;
        moves.size = 0;
        moves.count = 0;
    });
}

TEST_CASE("PreviewArtifactStorage rejects invalid move references", "[preview_artifact]")
{
    require_rejected("orcapv-reader-missing-layer-ref.orcapv", [](ArtifactFixture& fixture) {
        first_record<WireMoveRecord>(fixture, 5).layer_id = 99;
    });

    require_rejected("orcapv-reader-missing-tool.orcapv", [](ArtifactFixture& fixture) {
        first_record<WireMoveRecord>(fixture, 5).tool_id = 99;
    });

    require_rejected("orcapv-reader-missing-filament.orcapv", [](ArtifactFixture& fixture) {
        first_record<WireMoveRecord>(fixture, 5).filament_id = 99;
    });

    require_rejected("orcapv-reader-missing-color.orcapv", [](ArtifactFixture& fixture) {
        section_at(fixture, 4).count = 0;
        section_at(fixture, 4).size = 0;
        section_at(fixture, 4).offset = 0;
    });

    require_rejected("orcapv-reader-bad-layer-range.orcapv", [](ArtifactFixture& fixture) {
        first_record<WireLayerRecord>(fixture, 1).move_count = 2;
    });

    require_rejected("orcapv-reader-bad-string-ref.orcapv", [](ArtifactFixture& fixture) {
        first_record<WireColorRecord>(fixture, 4).name_size = 1;
    });
}
