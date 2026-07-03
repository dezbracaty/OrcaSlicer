#pragma once

#include "libslicer_worker/PreviewBinary.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <cstring>
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
            if (section.size > 0 && (section.offset > m_storage.size() || section.size > m_storage.size() - section.offset))
                throw std::runtime_error("Preview artifact section is outside file bounds");
            if (section.record_size > 0 && section.count > 0 &&
                section.size != section.count * static_cast<std::uint64_t>(section.record_size)) {
                throw std::runtime_error("Preview artifact section size does not match record count");
            }
            view.sections[section.section] = section;
        }

        return view;
    }

private:
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
