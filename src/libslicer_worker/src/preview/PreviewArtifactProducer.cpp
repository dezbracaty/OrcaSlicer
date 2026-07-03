#include "preview/PreviewArtifactProducer.hpp"

#include "libslicer_worker/PreviewReader.hpp"
#include "libslicer_worker/SliceJob.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include <libslic3r/CustomGCode.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <system_error>

namespace libslicer::worker::preview {
namespace {

struct StringRef {
    std::uint64_t offset { 0 };
    std::uint64_t size { 0 };
};

struct PreviewStringTable {
    std::vector<std::byte> bytes;
    std::map<std::string, StringRef> refs;

    StringRef add(const std::string& value)
    {
        const auto it = refs.find(value);
        if (it != refs.end())
            return it->second;

        StringRef ref;
        ref.offset = bytes.size();
        ref.size = value.size();
        const auto* first = reinterpret_cast<const std::byte*>(value.data());
        bytes.insert(bytes.end(), first, first + value.size());
        refs.emplace(value, ref);
        return ref;
    }
};

struct PreviewRecords {
    PreviewStringTable strings;
    std::vector<WireLayerRecord> layers;
    std::vector<WireToolRecord> tools;
    std::vector<WireFilamentRecord> filaments;
    std::vector<WireColorRecord> colors;
    std::vector<WireObjectRecord> objects;
    std::vector<WireInstanceRecord> instances;
    std::vector<WireMoveRecord> moves;
    std::vector<WireEventRecord> events;
};

struct SectionPayload {
    WireSectionType type { WireSectionType::MetadataJson };
    std::uint32_t record_size { 0 };
    std::vector<std::byte> bytes;
};

bool is_finite(const Slic3r::Vec3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

std::uint32_t flag(MoveFlags flag)
{
    return static_cast<std::uint32_t>(flag);
}

std::array<float, 4> parse_color_rgba(const std::string& color)
{
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        return -1;
    };

    if (color.size() != 7 && color.size() != 9)
        return { 1.0f, 0.5f, 0.0f, 1.0f };
    if (color[0] != '#')
        return { 1.0f, 0.5f, 0.0f, 1.0f };

    std::array<int, 4> channel { 255, 128, 0, 255 };
    for (std::size_t i = 0; i < (color.size() - 1) / 2; ++i) {
        const int high = hex_value(color[1 + i * 2]);
        const int low = hex_value(color[2 + i * 2]);
        if (high < 0 || low < 0)
            return { 1.0f, 0.5f, 0.0f, 1.0f };
        channel[i] = high * 16 + low;
    }
    return {
        static_cast<float>(channel[0]) / 255.0f,
        static_cast<float>(channel[1]) / 255.0f,
        static_cast<float>(channel[2]) / 255.0f,
        static_cast<float>(channel[3]) / 255.0f
    };
}

std::uint16_t filament_id_for_tool(const Slic3r::GCodeProcessorResult& result, unsigned int tool_id)
{
    if (tool_id < result.filament_maps.size() && result.filament_maps[tool_id] > 0)
        return static_cast<std::uint16_t>(result.filament_maps[tool_id] - 1);
    if (tool_id < result.filaments_count)
        return static_cast<std::uint16_t>(tool_id);
    return invalid_small_id;
}

std::uint16_t first_tool_for_filament(const Slic3r::GCodeProcessorResult& result, std::uint16_t filament_id)
{
    for (std::size_t tool_id = 0; tool_id < result.filament_maps.size(); ++tool_id) {
        if (result.filament_maps[tool_id] > 0 && static_cast<std::uint16_t>(result.filament_maps[tool_id] - 1) == filament_id)
            return static_cast<std::uint16_t>(tool_id);
    }
    if (filament_id < result.filaments_count)
        return filament_id;
    return invalid_small_id;
}

std::size_t tool_count_from_config(const Slic3r::DynamicPrintConfig& config, const Slic3r::GCodeProcessorResult& result)
{
    std::size_t count = std::max<std::size_t>(1, result.filament_maps.size());
    if (const auto* nozzle_diameters = config.opt<Slic3r::ConfigOptionFloats>("nozzle_diameter"))
        count = std::max(count, nozzle_diameters->values.size());
    return count;
}

void map_tools(const Slic3r::DynamicPrintConfig& config, const Slic3r::GCodeProcessorResult& result, PreviewRecords& records)
{
    const auto* nozzle_diameters = config.opt<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    const std::size_t tool_count = tool_count_from_config(config, result);
    records.tools.reserve(tool_count);
    for (std::size_t i = 0; i < tool_count; ++i) {
        WireToolRecord record;
        record.id = static_cast<std::uint16_t>(i);
        record.filament_id = filament_id_for_tool(result, static_cast<unsigned int>(i));
        if (nozzle_diameters != nullptr && i < nozzle_diameters->values.size())
            record.nozzle_diameter_mm = static_cast<float>(nozzle_diameters->values[i]);
        records.tools.push_back(record);
    }
}

void map_filaments_and_colors(const Slic3r::GCodeProcessorResult& result, PreviewRecords& records)
{
    const std::size_t filament_count = std::max<std::size_t>({
        result.filaments_count,
        result.extruder_colors.size(),
        result.filament_diameters.size(),
        result.filament_densities.size(),
        result.filament_costs.size()
    });

    records.filaments.reserve(filament_count);
    records.colors.reserve(filament_count + result.custom_gcode_per_print_z.size());
    for (std::size_t i = 0; i < filament_count; ++i) {
        const std::string color = i < result.extruder_colors.size() ? result.extruder_colors[i] : "#FF8000";

        WireFilamentRecord filament;
        filament.id = static_cast<std::uint16_t>(i);
        filament.tool_id = first_tool_for_filament(result, filament.id);
        filament.color_rgba = parse_color_rgba(color);
        if (i < result.filament_diameters.size())
            filament.diameter_mm = result.filament_diameters[i];
        if (i < result.filament_densities.size())
            filament.density = result.filament_densities[i];
        if (i < result.filament_costs.size())
            filament.cost = result.filament_costs[i];
        records.filaments.push_back(filament);

        WireColorRecord color_record;
        color_record.id = filament.id;
        color_record.filament_id = filament.id;
        color_record.source = ColorSource::Filament;
        color_record.color_rgba = filament.color_rgba;
        const StringRef name = records.strings.add(color);
        color_record.name_offset = name.offset;
        color_record.name_size = name.size;
        records.colors.push_back(color_record);
    }

    std::uint16_t next_color_id = static_cast<std::uint16_t>(records.colors.size());
    for (const Slic3r::CustomGCode::Item& item : result.custom_gcode_per_print_z) {
        if (item.type != Slic3r::CustomGCode::ColorChange || item.color.empty())
            continue;
        WireColorRecord color_record;
        color_record.id = next_color_id++;
        if (item.extruder > 0)
            color_record.filament_id = static_cast<std::uint16_t>(item.extruder - 1);
        color_record.source = ColorSource::ColorChange;
        color_record.color_rgba = parse_color_rgba(item.color);
        const StringRef name = records.strings.add(item.color);
        color_record.name_offset = name.offset;
        color_record.name_size = name.size;
        records.colors.push_back(color_record);
    }
}

void map_moves_and_events(const Slic3r::GCodeProcessorResult& result, PreviewRecords& records)
{
    records.moves.reserve(result.moves.size());
    records.events.reserve(result.custom_gcode_per_print_z.size());

    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> layer_ranges;
    std::optional<Slic3r::Vec3f> previous_position;
    std::uint64_t event_id = 0;

    for (std::size_t i = 0; i < result.moves.size(); ++i) {
        const auto& source = result.moves[i];
        WireMoveRecord move;
        move.id = i;
        move.gcode_id = source.gcode_id;
        move.layer_id = source.layer_id;
        move.tool_id = source.extruder_id;
        move.filament_id = filament_id_for_tool(result, source.extruder_id);
        move.move_type = source.type;
        move.extrusion_role = source.extrusion_role;
        move.cp_color_id = source.cp_color_id;
        move.delta_extruder_mm = source.delta_extruder;
        move.feedrate_mm_s = source.feedrate;
        move.actual_feedrate_mm_s = source.actual_feedrate;
        move.width_mm = source.width;
        move.height_mm = source.height;
        move.mm3_per_mm = source.mm3_per_mm;
        move.travel_dist_mm = source.travel_dist;
        move.fan_speed_percent = source.fan_speed;
        move.temperature_celsius = source.temperature;
        move.pressure_advance = source.pressure_advance;
        move.acceleration_mm_s2 = source.acceleration;
        move.jerk_mm_s = source.jerk;
        move.time_s = source.time;
        move.layer_duration_s = source.layer_duration;
        move.print_z_mm = source.print_z;
        move.object_label_id = source.object_label_id;
        move.flags |= flag(MoveFlags::HasTool);
        if (move.filament_id != invalid_small_id)
            move.flags |= flag(MoveFlags::HasFilament);
        if (move.cp_color_id < records.colors.size())
            move.flags |= flag(MoveFlags::HasCpColor);
        if (source.internal_only)
            move.flags |= flag(MoveFlags::InternalOnly);
        if (source.type == MoveType::Retract || source.type == MoveType::Unretract)
            move.flags |= flag(MoveFlags::Retraction);
        if (source.type == MoveType::Tool_change)
            move.flags |= flag(MoveFlags::ToolChange);
        if (source.type == MoveType::Color_change)
            move.flags |= flag(MoveFlags::ColorChange);

        if (previous_position.has_value() && is_finite(*previous_position)) {
            move.start_position_mm = to_wire_vec3f(*previous_position);
            move.flags |= flag(MoveFlags::ValidStartPosition);
        }
        if (is_finite(source.position)) {
            move.end_position_mm = to_wire_vec3f(source.position);
            move.flags |= flag(MoveFlags::ValidEndPosition);
            previous_position = source.position;
        }
        if (source.type == MoveType::Extrude || source.type == MoveType::Travel || source.type == MoveType::Wipe) {
            move.path_kind = PathKind::Linear_move;
            if (has_flag(move.flags, MoveFlags::ValidStartPosition) && has_flag(move.flags, MoveFlags::ValidEndPosition))
                move.flags |= flag(MoveFlags::Drawable);
        }

        auto& range = layer_ranges[move.layer_id];
        if (range.second == 0)
            range.first = static_cast<std::uint32_t>(records.moves.size());
        ++range.second;

        if (source.type == MoveType::Tool_change ||
            source.type == MoveType::Color_change ||
            source.type == MoveType::Pause_Print ||
            source.type == MoveType::Custom_GCode) {
            WireEventRecord event;
            event.id = event_id++;
            event.move_id = move.id;
            event.event_type = source.type;
            event.tool_id = move.tool_id;
            event.filament_id = move.filament_id;
            event.print_z_mm = source.print_z;
            event.time_s = source.time.empty() ? 0.0f : source.time[0];
            records.events.push_back(event);
        }

        records.moves.push_back(move);
    }

    records.layers.reserve(layer_ranges.size());
    for (const auto& [layer_id, range] : layer_ranges) {
        WireLayerRecord layer;
        layer.id = layer_id;
        layer.move_begin = range.first;
        layer.move_count = range.second;
        if (range.first < records.moves.size()) {
            const WireMoveRecord& first = records.moves[range.first];
            layer.print_z_mm = first.print_z_mm;
            layer.height_mm = first.height_mm;
            layer.duration_s = first.layer_duration_s;
        }
        records.layers.push_back(layer);
    }
}

PreviewRecords map_preview_records(const Slic3r::DynamicPrintConfig& config, const Slic3r::GCodeProcessorResult& result)
{
    PreviewRecords records;
    map_tools(config, result, records);
    map_filaments_and_colors(result, records);
    map_moves_and_events(result, records);
    return records;
}

template <class Record>
std::vector<std::byte> record_bytes(const std::vector<Record>& records)
{
    std::vector<std::byte> bytes(records.size() * sizeof(Record));
    if (!records.empty())
        std::memcpy(bytes.data(), records.data(), bytes.size());
    return bytes;
}

std::uint64_t align8(std::uint64_t value)
{
    return (value + 7u) & ~std::uint64_t(7u);
}

nlohmann::json build_metadata(const artifacts::PreviewArtifactOutput& request,
                              const PreviewProducerContext& context,
                              const PreviewRecords& records)
{
    nlohmann::json metadata;
    metadata["schema"] = std::string(schema_name);
    metadata["schema_version"] = schema_version;
    metadata["format"] = std::string(binary_format_name);
    metadata["coordinate_space"] = std::string(coordinate_space_name);
    metadata["job_id"] = context.job_id;
    metadata["input"] = {
        { "type", context.input_type },
        { "path", context.input_path.string() },
        { "plate_index", context.plate_index }
    };
    metadata["output"] = {
        { "preview", request.path.string() },
        { "gcode_requested", context.gcode_requested },
        { "gcode", context.gcode_path.string() }
    };
    metadata["counts"] = {
        { "strings_bytes", records.strings.bytes.size() },
        { "layers", records.layers.size() },
        { "tools", records.tools.size() },
        { "filaments", records.filaments.size() },
        { "colors", records.colors.size() },
        { "objects", records.objects.size() },
        { "instances", records.instances.size() },
        { "moves", records.moves.size() },
        { "events", records.events.size() }
    };
    return metadata;
}

std::vector<SectionPayload> make_payloads(const PreviewRecords& records, const std::string& metadata_json)
{
    return {
        { WireSectionType::MetadataJson, 1, std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(metadata_json.data()),
            reinterpret_cast<const std::byte*>(metadata_json.data()) + metadata_json.size()) },
        { WireSectionType::StringTable, 1, records.strings.bytes },
        { WireSectionType::Layers, sizeof(WireLayerRecord), record_bytes(records.layers) },
        { WireSectionType::Tools, sizeof(WireToolRecord), record_bytes(records.tools) },
        { WireSectionType::Filaments, sizeof(WireFilamentRecord), record_bytes(records.filaments) },
        { WireSectionType::Colors, sizeof(WireColorRecord), record_bytes(records.colors) },
        { WireSectionType::Objects, sizeof(WireObjectRecord), record_bytes(records.objects) },
        { WireSectionType::Instances, sizeof(WireInstanceRecord), record_bytes(records.instances) },
        { WireSectionType::Moves, sizeof(WireMoveRecord), record_bytes(records.moves) },
        { WireSectionType::Events, sizeof(WireEventRecord), record_bytes(records.events) }
    };
}

void append_padding(std::vector<std::byte>& bytes, std::uint64_t aligned_offset)
{
    while (bytes.size() < aligned_offset)
        bytes.push_back(std::byte { 0 });
}

void append_bytes(std::vector<std::byte>& target, const void* data, std::size_t size)
{
    const auto* first = reinterpret_cast<const std::byte*>(data);
    target.insert(target.end(), first, first + size);
}

void validate_preview_artifact(const std::filesystem::path& path)
{
    PreviewArtifactStorage storage;
    storage.load(path);
    const PreviewArtifactView view = storage.view();
    const nlohmann::json metadata = nlohmann::json::parse(view.metadata_json);
    if (metadata.value("schema", "") != std::string(schema_name))
        throw std::runtime_error("Preview artifact metadata schema mismatch");
    if (metadata.value("format", "") != std::string(binary_format_name))
        throw std::runtime_error("Preview artifact metadata format mismatch");
    (void)view.records<WireMoveRecord>(WireSectionType::Moves);
    (void)view.records<WireColorRecord>(WireSectionType::Colors);
}

void write_preview_artifact(const std::filesystem::path& path,
                            const artifacts::PreviewArtifactOutput& request,
                            const PreviewProducerContext& context,
                            const PreviewRecords& records)
{
    const std::string metadata_json = build_metadata(request, context, records).dump();
    std::vector<SectionPayload> payloads = make_payloads(records, metadata_json);

    WireFileHeader header;
    header.section_count = static_cast<std::uint16_t>(payloads.size());
    std::vector<WireSectionHeader> section_headers;
    section_headers.reserve(payloads.size());

    std::vector<std::byte> file(sizeof(WireFileHeader));
    for (const SectionPayload& payload : payloads) {
        const std::uint64_t offset = align8(file.size());
        append_padding(file, offset);
        const std::uint64_t size = payload.bytes.size();

        WireSectionHeader section;
        section.section = payload.type;
        section.record_size = payload.record_size;
        section.offset = size == 0 ? 0 : offset;
        section.size = size;
        section.count = payload.record_size == 0 ? 0 : size / payload.record_size;
        section_headers.push_back(section);
        if (!payload.bytes.empty())
            file.insert(file.end(), payload.bytes.begin(), payload.bytes.end());

        if (payload.type == WireSectionType::MetadataJson) {
            header.metadata_json_offset = section.offset;
            header.metadata_json_size = section.size;
        }
    }

    header.section_table_offset = align8(file.size());
    append_padding(file, header.section_table_offset);
    for (const WireSectionHeader& section : section_headers)
        append_bytes(file, &section, sizeof(section));
    header.file_size = file.size();
    std::memcpy(file.data(), &header, sizeof(header));

    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
        if (!output.good())
            throw std::runtime_error("Failed to open preview artifact temp file: " + tmp_path.string());
        output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (!output.good())
            throw std::runtime_error("Failed to write preview artifact temp file: " + tmp_path.string());
    }

    validate_preview_artifact(tmp_path);

    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, path, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp_path);
        throw std::runtime_error("Failed to finalize preview artifact: " + ec.message());
    }
}

} // namespace

PreviewProducerResult produce_preview_artifact(const artifacts::PreviewArtifactOutput& request,
                                               const PreviewProducerContext& context,
                                               const Slic3r::DynamicPrintConfig& config,
                                               const Slic3r::GCodeProcessorResult& gcode_result,
                                               CancellationToken& cancellation)
{
    PreviewProducerResult result;
    result.path = request.path;
    try {
        if (cancellation.cancelled()) {
            result.code = "cancelled";
            result.message = "Job was cancelled";
            return result;
        }

        PreviewRecords records = map_preview_records(config, gcode_result);
        if (cancellation.cancelled()) {
            result.code = "cancelled";
            result.message = "Job was cancelled";
            return result;
        }

        write_preview_artifact(request.path, request, context, records);
        if (cancellation.cancelled()) {
            std::error_code ec;
            std::filesystem::remove(request.path, ec);
            result.code = "cancelled";
            result.message = "Job was cancelled";
            return result;
        }

        result.success = true;
    } catch (const std::exception& e) {
        std::error_code ec;
        std::filesystem::remove(request.path.string() + ".tmp", ec);
        result.code = "preview_write_failed";
        result.message = e.what();
    }
    return result;
}

WorkerEvent make_preview_ready_event(const std::string& job_id, const std::filesystem::path& path)
{
    WorkerEvent event;
    event.type = WorkerEventType::Artifact;
    event.job_id = job_id;
    event.kind = "preview";
    event.path = path;
    event.phase = "ready";
    event.schema = std::string(schema_name);
    event.format = std::string(binary_format_name);
    event.complete = true;
    return event;
}

} // namespace libslicer::worker::preview
