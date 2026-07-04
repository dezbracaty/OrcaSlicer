#pragma once

#include "libslicer_worker/PreviewBinary.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace libslicer::worker::preview {

template <class T>
class ConstSpan {
public:
    ConstSpan() = default;
    ConstSpan(const T* data, std::size_t size) : m_data(data), m_size(size) {}

    const T* data() const { return m_data; }
    std::size_t size() const { return m_size; }
    bool empty() const { return m_size == 0; }
    const T* begin() const { return m_data; }
    const T* end() const { return m_data + m_size; }
    const T& operator[](std::size_t index) const { return m_data[index]; }

private:
    const T* m_data { nullptr };
    std::size_t m_size { 0 };
};

struct PreviewArtifactView {
    WireFileHeader header;
    std::string metadata_json;
    std::map<WireSectionType, WireSectionHeader> sections;
    ConstSpan<std::byte> storage;

    const WireSectionHeader* section(WireSectionType type) const
    {
        const auto it = sections.find(type);
        return it == sections.end() ? nullptr : &it->second;
    }

    ConstSpan<std::byte> bytes(WireSectionType type) const
    {
        const WireSectionHeader* header = section(type);
        if (header == nullptr || header->size == 0)
            return {};
        if (header->offset > storage.size() || header->size > storage.size() - header->offset)
            throw std::runtime_error("Preview artifact section is outside file bounds");
        return { storage.data() + static_cast<std::size_t>(header->offset), static_cast<std::size_t>(header->size) };
    }

    template <class Record>
    ConstSpan<Record> records(WireSectionType type) const
    {
        const WireSectionHeader* header = section(type);
        if (header == nullptr || header->count == 0)
            return {};
        if (header->record_size != sizeof(Record))
            throw std::runtime_error("Preview artifact record size mismatch");
        ConstSpan<std::byte> section_bytes = bytes(type);
        const std::size_t expected_size = static_cast<std::size_t>(header->count) * sizeof(Record);
        if (section_bytes.size() != expected_size)
            throw std::runtime_error("Preview artifact section byte size mismatch");
        return {
            reinterpret_cast<const Record*>(section_bytes.data()),
            static_cast<std::size_t>(header->count)
        };
    }

    std::string_view string_at(std::uint64_t offset, std::uint64_t size) const
    {
        ConstSpan<std::byte> table = bytes(WireSectionType::StringTable);
        if (offset > table.size() || size > table.size() - offset)
            throw std::runtime_error("Preview artifact string reference is outside string table");
        return {
            reinterpret_cast<const char*>(table.data() + offset),
            static_cast<std::size_t>(size)
        };
    }
};

class PreviewArtifactStorage {
public:
    void load(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input.good())
            throw std::runtime_error("Preview artifact not found: " + path.string());

        const std::streamsize file_size = input.tellg();
        if (file_size < static_cast<std::streamsize>(sizeof(WireFileHeader)))
            throw std::runtime_error("Preview artifact is too small");

        m_storage.resize(static_cast<std::size_t>(file_size));
        input.seekg(0);
        input.read(reinterpret_cast<char*>(m_storage.data()), file_size);
        if (!input)
            throw std::runtime_error("Failed to read preview artifact: " + path.string());
    }

    PreviewArtifactView view() const
    {
        if (m_storage.size() < sizeof(WireFileHeader))
            throw std::runtime_error("Preview artifact is empty");

        PreviewArtifactView view;
        view.storage = { m_storage.data(), m_storage.size() };
        view.header = read_object<WireFileHeader>(0);
        if (view.header.magic != binary_magic)
            throw std::runtime_error("Invalid preview artifact magic");
        if (view.header.header_size != sizeof(WireFileHeader))
            throw std::runtime_error("Unsupported preview artifact header size");
        if (view.header.version != schema_version)
            throw std::runtime_error("Unsupported preview artifact schema version");
        if (view.header.endian != binary_little_endian_marker)
            throw std::runtime_error("Unsupported preview artifact byte order");
        if (view.header.file_size != m_storage.size())
            throw std::runtime_error("Preview artifact file size mismatch");
        if (view.header.metadata_json_offset > m_storage.size() ||
            view.header.metadata_json_size > m_storage.size() - view.header.metadata_json_offset) {
            throw std::runtime_error("Preview artifact metadata is outside file bounds");
        }

        const std::uint64_t table_size = static_cast<std::uint64_t>(view.header.section_count) * sizeof(WireSectionHeader);
        if (view.header.section_table_offset > m_storage.size() ||
            table_size > m_storage.size() - view.header.section_table_offset) {
            throw std::runtime_error("Preview artifact section table is outside file bounds");
        }

        view.metadata_json.assign(
            reinterpret_cast<const char*>(m_storage.data() + view.header.metadata_json_offset),
            static_cast<std::size_t>(view.header.metadata_json_size));

        for (std::uint16_t i = 0; i < view.header.section_count; ++i) {
            const std::uint64_t offset = view.header.section_table_offset + i * sizeof(WireSectionHeader);
            WireSectionHeader section = read_object<WireSectionHeader>(offset);
            validate_known_section(section);
            if (section.size > 0 && (section.offset > m_storage.size() || section.size > m_storage.size() - section.offset))
                throw std::runtime_error("Preview artifact section is outside file bounds");
            if (section.size > 0 && section.offset % 8 != 0)
                throw std::runtime_error("Preview artifact section is not 8-byte aligned");
            if (section.record_size > 0 &&
                section.size != section.count * static_cast<std::uint64_t>(section.record_size)) {
                throw std::runtime_error("Preview artifact section size does not match record count");
            }
            if (!view.sections.emplace(section.section, section).second)
                throw std::runtime_error("Preview artifact contains duplicate section");
        }

        validate_required_sections(view);
        validate_section_ranges(view);
        validate_move_references(view);
        return view;
    }

private:
    struct Range {
        std::uint64_t begin { 0 };
        std::uint64_t end { 0 };
        WireSectionType section { WireSectionType::MetadataJson };
    };

    static bool ranges_overlap(std::uint64_t left_begin,
                               std::uint64_t left_end,
                               std::uint64_t right_begin,
                               std::uint64_t right_end)
    {
        return left_begin < right_end && right_begin < left_end;
    }

    static void validate_known_section(const WireSectionHeader& section)
    {
        auto expect_record_size = [&section](std::uint32_t expected) {
            if (section.record_size != expected)
                throw std::runtime_error("Preview artifact section record size mismatch");
        };

        switch (section.section) {
        case WireSectionType::MetadataJson:
        case WireSectionType::StringTable:
            expect_record_size(1);
            break;
        case WireSectionType::Layers:
            expect_record_size(sizeof(WireLayerRecord));
            break;
        case WireSectionType::Tools:
            expect_record_size(sizeof(WireToolRecord));
            break;
        case WireSectionType::Filaments:
            expect_record_size(sizeof(WireFilamentRecord));
            break;
        case WireSectionType::Colors:
            expect_record_size(sizeof(WireColorRecord));
            break;
        case WireSectionType::Objects:
            expect_record_size(sizeof(WireObjectRecord));
            break;
        case WireSectionType::Instances:
            expect_record_size(sizeof(WireInstanceRecord));
            break;
        case WireSectionType::Moves:
            expect_record_size(sizeof(WireMoveRecord));
            break;
        case WireSectionType::Events:
            expect_record_size(sizeof(WireEventRecord));
            break;
        default:
            throw std::runtime_error("Preview artifact contains unknown section type");
        }
    }

    static void validate_required_sections(const PreviewArtifactView& view)
    {
        if (view.section(WireSectionType::MetadataJson) == nullptr)
            throw std::runtime_error("Preview artifact is missing metadata section");
        if (view.section(WireSectionType::Layers) == nullptr)
            throw std::runtime_error("Preview artifact is missing layers section");
        if (view.section(WireSectionType::Moves) == nullptr)
            throw std::runtime_error("Preview artifact is missing moves section");
    }

    static void validate_section_ranges(const PreviewArtifactView& view)
    {
        const std::uint64_t header_begin = 0;
        const std::uint64_t header_end = view.header.header_size;
        const std::uint64_t table_begin = view.header.section_table_offset;
        const std::uint64_t table_end = table_begin + static_cast<std::uint64_t>(view.header.section_count) * sizeof(WireSectionHeader);
        const std::uint64_t metadata_begin = view.header.metadata_json_offset;
        const std::uint64_t metadata_end = metadata_begin + view.header.metadata_json_size;

        std::vector<Range> ranges;
        for (const auto& [type, section] : view.sections) {
            if (section.size == 0)
                continue;
            const std::uint64_t begin = section.offset;
            const std::uint64_t end = section.offset + section.size;
            if (ranges_overlap(begin, end, header_begin, header_end))
                throw std::runtime_error("Preview artifact section overlaps file header");
            if (ranges_overlap(begin, end, table_begin, table_end))
                throw std::runtime_error("Preview artifact section overlaps section table");
            if (type != WireSectionType::MetadataJson && ranges_overlap(begin, end, metadata_begin, metadata_end))
                throw std::runtime_error("Preview artifact section overlaps metadata");
            ranges.push_back({ begin, end, type });
        }

        std::sort(ranges.begin(), ranges.end(), [](const Range& left, const Range& right) {
            return left.begin < right.begin;
        });
        for (std::size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i - 1].end > ranges[i].begin)
                throw std::runtime_error("Preview artifact sections overlap");
        }

        const WireSectionHeader* metadata = view.section(WireSectionType::MetadataJson);
        if (metadata == nullptr ||
            metadata->offset != view.header.metadata_json_offset ||
            metadata->size != view.header.metadata_json_size) {
            throw std::runtime_error("Preview artifact metadata header does not match metadata section");
        }
    }

    static void validate_move_references(const PreviewArtifactView& view)
    {
        const ConstSpan<WireLayerRecord> layers = view.records<WireLayerRecord>(WireSectionType::Layers);
        const ConstSpan<WireMoveRecord> moves = view.records<WireMoveRecord>(WireSectionType::Moves);
        if (layers.empty())
            throw std::runtime_error("Preview artifact has no layer records");
        if (moves.empty())
            throw std::runtime_error("Preview artifact has no move records");

        std::set<std::uint32_t> layer_ids;
        for (const WireLayerRecord& layer : layers) {
            layer_ids.insert(layer.id);
            if (layer.move_begin > moves.size() || layer.move_count > moves.size() - layer.move_begin)
                throw std::runtime_error("Preview artifact layer move range is outside move records");
        }

        std::set<std::uint16_t> tool_ids;
        for (const WireToolRecord& tool : view.records<WireToolRecord>(WireSectionType::Tools))
            tool_ids.insert(tool.id);

        std::set<std::uint16_t> filament_ids;
        for (const WireFilamentRecord& filament : view.records<WireFilamentRecord>(WireSectionType::Filaments))
            filament_ids.insert(filament.id);

        std::set<std::uint16_t> color_ids;
        for (const WireColorRecord& color : view.records<WireColorRecord>(WireSectionType::Colors)) {
            color_ids.insert(color.id);
            (void)view.string_at(color.name_offset, color.name_size);
        }

        for (const WireObjectRecord& object : view.records<WireObjectRecord>(WireSectionType::Objects))
            (void)view.string_at(object.name_offset, object.name_size);

        std::set<std::uint64_t> move_ids;
        for (const WireMoveRecord& move : moves) {
            move_ids.insert(move.id);
            if (layer_ids.find(move.layer_id) == layer_ids.end())
                throw std::runtime_error("Preview artifact move references missing layer");
            if (move.tool_id != invalid_small_id && tool_ids.find(move.tool_id) == tool_ids.end())
                throw std::runtime_error("Preview artifact move references missing tool");
            if (move.filament_id != invalid_small_id && filament_ids.find(move.filament_id) == filament_ids.end())
                throw std::runtime_error("Preview artifact move references missing filament");
            if (has_flag(move.flags, MoveFlags::HasCpColor) && color_ids.find(move.cp_color_id) == color_ids.end())
                throw std::runtime_error("Preview artifact move references missing color");
        }

        for (const WireEventRecord& event : view.records<WireEventRecord>(WireSectionType::Events)) {
            if (event.move_id != invalid_large_id && move_ids.find(event.move_id) == move_ids.end())
                throw std::runtime_error("Preview artifact event references missing move");
            (void)view.string_at(event.message_offset, event.message_size);
        }
    }

    template <class T>
    T read_object(std::uint64_t offset) const
    {
        if (offset > m_storage.size() || sizeof(T) > m_storage.size() - offset)
            throw std::runtime_error("Preview artifact read is outside file bounds");
        T value;
        std::memcpy(&value, m_storage.data() + offset, sizeof(T));
        return value;
    }

    std::vector<std::byte> m_storage;
};

struct RenderSegmentView {
    WireVec3f start_mm;
    WireVec3f end_mm;
    std::uint32_t layer_id { invalid_id };
    std::uint32_t object_id { invalid_id };
    std::uint32_t instance_id { invalid_id };
    std::uint16_t tool_id { invalid_small_id };
    std::uint16_t filament_id { invalid_small_id };
    MoveType move_type { MoveType::Noop };
    PathKind path_kind { PathKind::Noop_move };
    ExtrusionRole extrusion_role { Slic3r::erNone };
    float width_mm { 0.0f };
    float height_mm { 0.0f };
    float feedrate_mm_s { 0.0f };
    float actual_feedrate_mm_s { 0.0f };
    float mm3_per_mm { 0.0f };
    float print_z_mm { 0.0f };
    std::uint32_t flags { 0 };
};

inline constexpr bool move_has_flag(const WireMoveRecord& move, MoveFlags flag)
{
    return has_flag(move.flags, flag);
}

inline constexpr bool move_is_drawable_segment(const WireMoveRecord& move)
{
    return move_has_flag(move, MoveFlags::Drawable) &&
        move_has_flag(move, MoveFlags::ValidStartPosition) &&
        move_has_flag(move, MoveFlags::ValidEndPosition);
}

inline constexpr RenderSegmentView make_render_segment_view(const WireMoveRecord& move)
{
    return RenderSegmentView {
        move.start_position_mm,
        move.end_position_mm,
        move.layer_id,
        move.object_id,
        move.instance_id,
        move.tool_id,
        move.filament_id,
        move.move_type,
        move.path_kind,
        move.extrusion_role,
        move.width_mm,
        move.height_mm,
        move.feedrate_mm_s,
        move.actual_feedrate_mm_s,
        move.mm3_per_mm,
        move.print_z_mm,
        move.flags
    };
}

inline constexpr std::string_view move_type_name(const WireMoveRecord& move)
{
    return Slic3r::move_type_name(move.move_type);
}

inline constexpr std::string_view path_kind_name(const WireMoveRecord& move)
{
    return Slic3r::move_path_type_name(move.path_kind);
}

inline constexpr std::string_view extrusion_role_name(const WireMoveRecord& move)
{
    return Slic3r::extrusion_role_name(move.extrusion_role);
}

} // namespace libslicer::worker::preview
