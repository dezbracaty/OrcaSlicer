#include "preview/PreviewArtifactProducer.hpp"

#include "libslicer_worker/PreviewReader.hpp"
#include "libslicer_worker/SliceJob.hpp"
#include "libslicer_worker/WorkerProtocol.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

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

class PreviewProducerException : public std::runtime_error {
public:
    PreviewProducerException(std::string code, const std::string& message)
        : std::runtime_error(message), m_code(std::move(code))
    {
    }

    const std::string& code() const { return m_code; }

private:
    std::string m_code;
};

bool is_finite(const Slic3r::Vec3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

std::uint32_t flag(MoveFlags flag)
{
    return static_cast<std::uint32_t>(flag);
}

ColorSource to_wire_color_source(Slic3r::GCodeProcessorResult::PreviewColorSource source)
{
    switch (source) {
    case Slic3r::GCodeProcessorResult::PreviewColorSource::Filament:
        return ColorSource::Filament;
    case Slic3r::GCodeProcessorResult::PreviewColorSource::ColorChange:
        return ColorSource::ColorChange;
    case Slic3r::GCodeProcessorResult::PreviewColorSource::Custom:
        return ColorSource::Custom;
    case Slic3r::GCodeProcessorResult::PreviewColorSource::Unknown:
    default:
        return ColorSource::Unknown;
    }
}

[[noreturn]] void throw_mapping_error(const std::string& message)
{
    throw PreviewProducerException("preview_mapping_invalid", message);
}

[[noreturn]] void throw_color_error(const std::string& message)
{
    throw PreviewProducerException("preview_color_table_invalid", message);
}

[[noreturn]] void throw_artifact_error(const std::string& message)
{
    throw PreviewProducerException("preview_artifact_invalid", message);
}

[[noreturn]] void throw_write_error(const std::string& message)
{
    throw PreviewProducerException("preview_write_failed", message);
}

std::uint16_t tool_id_for_filament(const Slic3r::GCodeProcessorResult& result,
                                   std::uint16_t filament_id,
                                   std::size_t tool_count)
{
    if (filament_id >= result.filament_to_tool_map.size())
        throw_mapping_error("Preview mapping missing filament_to_tool_map entry for filament " + std::to_string(filament_id));
    const int tool_id = result.filament_to_tool_map[filament_id];
    if (tool_id < 0 || tool_id > static_cast<int>(std::numeric_limits<std::uint16_t>::max()))
        throw_mapping_error("Preview mapping has invalid tool id for filament " + std::to_string(filament_id));
    if (static_cast<std::size_t>(tool_id) >= tool_count)
        throw_mapping_error("Preview mapping references missing tool " + std::to_string(tool_id) + " for filament " + std::to_string(filament_id));
    return static_cast<std::uint16_t>(tool_id);
}

std::uint16_t primary_filament_for_tool(const Slic3r::GCodeProcessorResult& result, std::uint16_t tool_id)
{
    for (std::size_t filament_id = 0; filament_id < result.filament_to_tool_map.size(); ++filament_id) {
        if (result.filament_to_tool_map[filament_id] == static_cast<int>(tool_id))
            return static_cast<std::uint16_t>(filament_id);
    }
    return invalid_small_id;
}

std::size_t tool_count_from_config(const Slic3r::DynamicPrintConfig& config, const Slic3r::GCodeProcessorResult& result)
{
    std::size_t count = 1;
    if (const auto* nozzle_diameters = config.opt<Slic3r::ConfigOptionFloats>("nozzle_diameter"))
        count = std::max(count, nozzle_diameters->values.size());
    for (const int tool_id : result.filament_to_tool_map) {
        if (tool_id >= 0)
            count = std::max(count, static_cast<std::size_t>(tool_id) + 1);
    }
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
        record.filament_id = primary_filament_for_tool(result, record.id);
        if (nozzle_diameters != nullptr && i < nozzle_diameters->values.size())
            record.nozzle_diameter_mm = static_cast<float>(nozzle_diameters->values[i]);
        records.tools.push_back(record);
    }
}

void map_filaments_and_colors(const Slic3r::GCodeProcessorResult& result, std::size_t tool_count, PreviewRecords& records)
{
    const std::size_t filament_count = std::max<std::size_t>({
        result.filaments_count,
        result.extruder_colors.size(),
        result.filament_diameters.size(),
        result.filament_densities.size(),
        result.filament_costs.size()
    });
    if (filament_count > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
        throw_mapping_error("Preview filament count exceeds wire id range");

    records.filaments.reserve(filament_count);
    records.colors.reserve(result.preview_colors.size());
    for (std::size_t i = 0; i < filament_count; ++i) {
        WireFilamentRecord filament;
        filament.id = static_cast<std::uint16_t>(i);
        filament.tool_id = tool_id_for_filament(result, filament.id, tool_count);
        auto color_it = result.preview_colors.find(filament.id);
        if (color_it == result.preview_colors.end())
            throw_color_error("Preview color table missing base color for filament " + std::to_string(filament.id));
        filament.color_rgba = color_it->second.color_rgba;
        if (i < result.filament_diameters.size())
            filament.diameter_mm = result.filament_diameters[i];
        if (i < result.filament_densities.size())
            filament.density = result.filament_densities[i];
        if (i < result.filament_costs.size())
            filament.cost = result.filament_costs[i];
        records.filaments.push_back(filament);
    }

    for (const auto& [id, fact] : result.preview_colors) {
        if (fact.filament_id != invalid_small_id && fact.filament_id >= filament_count)
            throw_color_error("Preview color table references missing filament " + std::to_string(fact.filament_id));
        WireColorRecord color_record;
        color_record.id = id;
        color_record.filament_id = fact.filament_id;
        color_record.source = to_wire_color_source(fact.source);
        color_record.color_rgba = fact.color_rgba;
        const StringRef name = records.strings.add(fact.name);
        color_record.name_offset = name.offset;
        color_record.name_size = name.size;
        records.colors.push_back(color_record);
    }
}

void map_moves_and_events(const Slic3r::GCodeProcessorResult& result, std::size_t tool_count, PreviewRecords& records)
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
        move.filament_id = source.extruder_id;
        move.tool_id = tool_id_for_filament(result, move.filament_id, tool_count);
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
        if (result.preview_colors.find(move.cp_color_id) == result.preview_colors.end())
            throw_color_error("Preview move references missing color id " + std::to_string(move.cp_color_id));
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
    const std::size_t tool_count = tool_count_from_config(config, result);
    if (tool_count > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
        throw_mapping_error("Preview tool count exceeds wire id range");
    map_tools(config, result, records);
    map_filaments_and_colors(result, tool_count, records);
    map_moves_and_events(result, tool_count, records);
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
    try {
        PreviewArtifactStorage storage;
        storage.load(path);
        const PreviewArtifactView view = storage.view();
        const nlohmann::json metadata = nlohmann::json::parse(view.metadata_json);
        if (metadata.value("schema", "") != std::string(schema_name))
            throw_artifact_error("Preview artifact metadata schema mismatch");
        if (metadata.value("format", "") != std::string(binary_format_name))
            throw_artifact_error("Preview artifact metadata format mismatch");
        (void)view.records<WireMoveRecord>(WireSectionType::Moves);
        (void)view.records<WireColorRecord>(WireSectionType::Colors);
    } catch (const PreviewProducerException&) {
        throw;
    } catch (const std::exception& e) {
        throw_artifact_error(e.what());
    }
}

void validate_preview_records(const PreviewRecords& records)
{
    if (records.layers.empty())
        throw_artifact_error("Preview artifact has no layer records");
    if (records.moves.empty())
        throw_artifact_error("Preview artifact has no move records");

    std::set<std::uint32_t> layer_ids;
    for (const WireLayerRecord& layer : records.layers) {
        layer_ids.insert(layer.id);
        if (layer.move_begin > records.moves.size() || layer.move_count > records.moves.size() - layer.move_begin)
            throw_artifact_error("Preview layer move range is outside move records");
    }

    std::set<std::uint16_t> tool_ids;
    for (const WireToolRecord& tool : records.tools)
        tool_ids.insert(tool.id);

    std::set<std::uint16_t> filament_ids;
    for (const WireFilamentRecord& filament : records.filaments) {
        filament_ids.insert(filament.id);
        if (filament.tool_id != invalid_small_id && tool_ids.find(filament.tool_id) == tool_ids.end())
            throw_mapping_error("Preview filament references missing tool");
    }

    std::set<std::uint16_t> color_ids;
    for (const WireColorRecord& color : records.colors) {
        color_ids.insert(color.id);
        if (color.filament_id != invalid_small_id && filament_ids.find(color.filament_id) == filament_ids.end())
            throw_color_error("Preview color references missing filament");
        if (color.name_offset > records.strings.bytes.size() || color.name_size > records.strings.bytes.size() - color.name_offset)
            throw_artifact_error("Preview color string reference is outside string table");
    }

    bool has_drawable_move = false;
    for (const WireMoveRecord& move : records.moves) {
        if (layer_ids.find(move.layer_id) == layer_ids.end())
            throw_artifact_error("Preview move references missing layer");
        if (has_flag(move.flags, MoveFlags::HasTool) && tool_ids.find(move.tool_id) == tool_ids.end())
            throw_mapping_error("Preview move references missing tool");
        if (has_flag(move.flags, MoveFlags::HasFilament) && filament_ids.find(move.filament_id) == filament_ids.end())
            throw_mapping_error("Preview move references missing filament");
        if (has_flag(move.flags, MoveFlags::HasCpColor) && color_ids.find(move.cp_color_id) == color_ids.end())
            throw_color_error("Preview move references missing color");
        has_drawable_move = has_drawable_move || has_flag(move.flags, MoveFlags::Drawable);
    }
    if (!has_drawable_move)
        throw_artifact_error("Preview artifact has no drawable moves");

    for (const WireEventRecord& event : records.events) {
        if (event.move_id >= records.moves.size())
            throw_artifact_error("Preview event references missing move");
        if (event.message_offset > records.strings.bytes.size() || event.message_size > records.strings.bytes.size() - event.message_offset)
            throw_artifact_error("Preview event string reference is outside string table");
    }
}

void write_preview_artifact(const std::filesystem::path& path,
                            const artifacts::PreviewArtifactOutput& request,
                            const PreviewProducerContext& context,
                            const PreviewRecords& records)
{
    const std::string metadata_json = build_metadata(request, context, records).dump();
    std::vector<SectionPayload> payloads = make_payloads(records, metadata_json);
    if (payloads.size() > static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))
        throw_artifact_error("Preview section count exceeds wire range");

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
        if (payload.record_size != 0 && section.count > std::numeric_limits<std::uint64_t>::max() / payload.record_size)
            throw_artifact_error("Preview section byte size overflow");
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
            throw_write_error("Failed to open preview artifact temp file: " + tmp_path.string());
        output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        if (!output.good())
            throw_write_error("Failed to write preview artifact temp file: " + tmp_path.string());
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
        throw_write_error("Failed to finalize preview artifact: " + ec.message());
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
        validate_preview_records(records);
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
    } catch (const PreviewProducerException& e) {
        std::error_code ec;
        std::filesystem::remove(request.path.string() + ".tmp", ec);
        result.code = e.code();
        result.message = e.what();
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
