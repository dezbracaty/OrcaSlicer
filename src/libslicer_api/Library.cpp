#define NANOSVG_IMPLEMENTATION
#include <nanosvg/nanosvg.h>

#include <libslicer/Library.hpp>

#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace libslicer {
namespace {

namespace fs = std::filesystem;

void append_diagnostic(std::vector<ConfigDiagnostic>* diagnostics, std::string key, std::string message)
{
    if (diagnostics != nullptr) {
        diagnostics->push_back({std::move(key), std::move(message)});
    }
}

bool is_resource_root(const fs::path& path)
{
    std::error_code error;
    return !path.empty() && fs::is_directory(path / "profiles", error) && !error;
}

fs::path executable_path()
{
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return fs::weakly_canonical(fs::path(buffer.c_str()));
    }
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const auto size = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size > 0) {
        buffer.resize(static_cast<std::size_t>(size));
        return fs::path(buffer);
    }
#endif
    return {};
}

std::string locate_resource_directory(const std::string& override_directory)
{
    if (!override_directory.empty()) {
        const fs::path requested = fs::absolute(override_directory).lexically_normal();
        if (is_resource_root(requested)) {
            return requested.string();
        }
        throw std::runtime_error("libslicer resource directory has no profiles folder: " + requested.string());
    }

    std::vector<fs::path> candidates;
    if (const fs::path executable = executable_path(); !executable.empty()) {
#if defined(__APPLE__)
        candidates.push_back(executable.parent_path() / "../Resources/libslicer");
#endif
        candidates.push_back(executable.parent_path() / "resources/libslicer");
        candidates.push_back(executable.parent_path() / "../share/libslicer");
    }
#ifdef LIBSLICER_DEFAULT_RESOURCE_DIR
    candidates.emplace_back(LIBSLICER_DEFAULT_RESOURCE_DIR);
#endif
    candidates.push_back(fs::current_path() / "share/libslicer");
    candidates.push_back(fs::current_path() / "resources/libslicer");

    for (const fs::path& candidate : candidates) {
        const fs::path normalized = fs::absolute(candidate).lexically_normal();
        if (is_resource_root(normalized)) {
            return normalized.string();
        }
    }
    throw std::runtime_error("Unable to locate the resources installed with libslicer");
}

std::vector<std::string> discover_vendors(const fs::path& profiles_directory)
{
    std::vector<std::string> vendors;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(profiles_directory, error)) {
        if (error) {
            break;
        }
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            vendors.push_back(entry.path().stem().string());
        }
    }
    std::sort(vendors.begin(), vendors.end());
    return vendors;
}

double nozzle_diameter(const std::string& variant)
{
    try {
        std::size_t parsed = 0;
        const double value = std::stod(variant, &parsed);
        return parsed == 0 ? 0.0 : value;
    } catch (...) {
        return 0.0;
    }
}

void populate_printable_volume(const Slic3r::DynamicPrintConfig& config,
                               MachineVariantOption& option)
{
    const auto* area = config.option<Slic3r::ConfigOptionPoints>("printable_area");
    if (area != nullptr && !area->values.empty()) {
        double min_x = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double min_y = std::numeric_limits<double>::max();
        double max_y = std::numeric_limits<double>::lowest();
        for (const auto& point : area->values) {
            min_x = std::min(min_x, point.x());
            max_x = std::max(max_x, point.x());
            min_y = std::min(min_y, point.y());
            max_y = std::max(max_y, point.y());
        }
        option.printable_width = std::max(0.0, max_x - min_x);
        option.printable_depth = std::max(0.0, max_y - min_y);
    }
    if (const auto* height = config.option<Slic3r::ConfigOptionFloat>("printable_height")) {
        option.printable_height = height->value;
    }
}

std::string resource_path(const fs::path& resource_root,
                          const std::string& vendor,
                          const std::string& resource)
{
    if (resource.empty()) {
        return {};
    }
    return (resource_root / "profiles" / vendor / resource).lexically_normal().string();
}

std::string build_plate_image_path(const fs::path& resource_root, const std::string& value)
{
    static const std::pair<const char*, const char*> images[] = {
        {"Cool Plate", "bed_cool.png"},
        {"Engineering Plate", "bed_engineering.png"},
        {"High Temp Plate", "bed_high_templ.png"},
        {"Textured PEI Plate", "bed_pei.png"},
        {"Textured Cool Plate", "bed_pei_cool.png"},
        {"Supertack Plate", "bed_cool_supertack.png"}
    };
    const auto found = std::find_if(std::begin(images), std::end(images), [&value](const auto& image) {
        return value == image.first;
    });
    return found == std::end(images)
        ? std::string{}
        : (resource_root / "images" / found->second).lexically_normal().string();
}

std::string renderable_texture_path(const fs::path& resource_root,
                                    const std::string& vendor,
                                    const std::string& resource)
{
    fs::path path = resource_root / "profiles" / vendor / resource;
    if (path.extension() == ".svg") {
        fs::path raster_path = path;
        raster_path.replace_extension(".png");
        std::error_code error;
        if (fs::is_regular_file(raster_path, error) && !error) {
            path = std::move(raster_path);
        }
    }
    return resource.empty() ? std::string{} : path.lexically_normal().string();
}

std::vector<PresetOption> compatible_presets(const Slic3r::PresetCollection& presets,
                                             const std::string& selected)
{
    std::vector<PresetOption> result;
    for (const Slic3r::Preset& preset : presets) {
        if (preset.is_visible && preset.is_compatible) {
            result.push_back({preset.name, preset.name, preset.name == selected});
        }
    }
    return result;
}

std::string first_compatible_preset_name(const Slic3r::PresetCollection& presets)
{
    for (const Slic3r::Preset& preset : presets) {
        if (preset.is_visible && preset.is_compatible) {
            return preset.name;
        }
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> serialized_values(const Slic3r::DynamicPrintConfig& config)
{
    std::vector<std::pair<std::string, std::string>> values;
    const auto keys = config.keys();
    values.reserve(keys.size());
    for (const std::string& key : keys) {
        if (Slic3r::print_config_def.get(key) != nullptr) {
            values.emplace_back(key, config.opt_serialize(key));
        }
    }
    return values;
}

Slic3r::DynamicPrintConfig dynamic_config(const ConfigSnapshot& snapshot)
{
    Slic3r::DynamicPrintConfig config;
    config.apply(Slic3r::FullPrintConfig::defaults());
    for (const auto& [key, value] : snapshot.values()) {
        if (Slic3r::print_config_def.get(key) != nullptr) {
            config.set_deserialize_strict(key, value);
        }
    }
    return config;
}

Slic3r::Vec2d build_plate_center(const Slic3r::DynamicPrintConfig& config)
{
    const auto* area = config.option<Slic3r::ConfigOptionPoints>("printable_area");
    if (area == nullptr || area->values.empty()) {
        return {100.0, 100.0};
    }
    double min_x = std::numeric_limits<double>::max();
    double min_y = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    double max_y = std::numeric_limits<double>::lowest();
    for (const auto& point : area->values) {
        min_x = std::min(min_x, point.x());
        min_y = std::min(min_y, point.y());
        max_x = std::max(max_x, point.x());
        max_y = std::max(max_y, point.y());
    }
    return {(min_x + max_x) * 0.5, (min_y + max_y) * 0.5};
}

std::string temporary_gcode_path()
{
    static std::atomic<unsigned long long> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() /
            ("libslicer_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".gcode")).string();
}

bool cancellation_requested(const SliceCallbacks& callbacks)
{
    return callbacks.is_cancelled && callbacks.is_cancelled();
}

void report_progress(const SliceCallbacks& callbacks, float progress, std::string_view stage)
{
    if (callbacks.progress) {
        callbacks.progress(std::max(0.0f, std::min(progress, 1.0f)), stage);
    }
}

ToolpathColorValue parse_color(std::string value)
{
    ToolpathColorValue color;
    if (!value.empty() && value.front() == '#') {
        value.erase(value.begin());
    }
    if (value.size() != 6 && value.size() != 8) {
        color.red = 1.0f;
        color.green = 0.5f;
        color.blue = 0.0f;
        return color;
    }
    try {
        const auto component = [&value](std::size_t offset) {
            return static_cast<float>(std::stoul(value.substr(offset, 2), nullptr, 16)) / 255.0f;
        };
        color.red = component(0);
        color.green = component(2);
        color.blue = component(4);
        color.alpha = value.size() == 8 ? component(6) : 1.0f;
    } catch (...) {
        color.red = 1.0f;
        color.green = 0.5f;
        color.blue = 0.0f;
        color.alpha = 1.0f;
    }
    return color;
}

ToolpathPoint to_toolpath_point(const Slic3r::Vec3f& value)
{
    return {value.x(), value.y(), value.z()};
}

bool finite_point(const Slic3r::Vec3f& value)
{
    return std::isfinite(value.x()) && std::isfinite(value.y()) && std::isfinite(value.z());
}

double point_distance(const ToolpathPoint& first, const ToolpathPoint& second)
{
    const double x = static_cast<double>(second.x) - first.x;
    const double y = static_cast<double>(second.y) - first.y;
    const double z = static_cast<double>(second.z) - first.z;
    return std::sqrt(x * x + y * y + z * z);
}

void include_point(ToolpathBounds& bounds, const ToolpathPoint& point)
{
    if (!bounds.valid) {
        bounds.minimum = point;
        bounds.maximum = point;
        bounds.valid = true;
        return;
    }
    bounds.minimum.x = std::min(bounds.minimum.x, point.x);
    bounds.minimum.y = std::min(bounds.minimum.y, point.y);
    bounds.minimum.z = std::min(bounds.minimum.z, point.z);
    bounds.maximum.x = std::max(bounds.maximum.x, point.x);
    bounds.maximum.y = std::max(bounds.maximum.y, point.y);
    bounds.maximum.z = std::max(bounds.maximum.z, point.z);
}

std::optional<ToolpathMotionKind> to_motion_kind(Slic3r::EMoveType type)
{
    switch (type) {
    case Slic3r::EMoveType::Travel:  return ToolpathMotionKind::Travel;
    case Slic3r::EMoveType::Extrude: return ToolpathMotionKind::Extrusion;
    case Slic3r::EMoveType::Wipe:    return ToolpathMotionKind::Wipe;
    default:                         return std::nullopt;
    }
}

std::optional<ToolpathEventKind> to_event_kind(Slic3r::EMoveType type)
{
    switch (type) {
    case Slic3r::EMoveType::Seam:         return ToolpathEventKind::Seam;
    case Slic3r::EMoveType::Tool_change:  return ToolpathEventKind::ToolChange;
    case Slic3r::EMoveType::Color_change: return ToolpathEventKind::ColorChange;
    case Slic3r::EMoveType::Pause_Print:  return ToolpathEventKind::Pause;
    case Slic3r::EMoveType::Custom_GCode: return ToolpathEventKind::CustomGCode;
    default:                              return std::nullopt;
    }
}

ToolpathExtrusionRole to_extrusion_role(Slic3r::ExtrusionRole role)
{
    switch (role) {
    case Slic3r::erNone:                     return ToolpathExtrusionRole::None;
    case Slic3r::erPerimeter:                return ToolpathExtrusionRole::InnerWall;
    case Slic3r::erExternalPerimeter:        return ToolpathExtrusionRole::OuterWall;
    case Slic3r::erOverhangPerimeter:        return ToolpathExtrusionRole::OverhangWall;
    case Slic3r::erInternalInfill:           return ToolpathExtrusionRole::SparseInfill;
    case Slic3r::erSolidInfill:              return ToolpathExtrusionRole::InternalSolidInfill;
    case Slic3r::erTopSolidInfill:           return ToolpathExtrusionRole::TopSurface;
    case Slic3r::erBottomSurface:            return ToolpathExtrusionRole::BottomSurface;
    case Slic3r::erIroning:                  return ToolpathExtrusionRole::Ironing;
    case Slic3r::erBridgeInfill:             return ToolpathExtrusionRole::Bridge;
    case Slic3r::erInternalBridgeInfill:     return ToolpathExtrusionRole::InternalBridge;
    case Slic3r::erGapFill:                  return ToolpathExtrusionRole::GapInfill;
    case Slic3r::erSkirt:                    return ToolpathExtrusionRole::Skirt;
    case Slic3r::erBrim:                     return ToolpathExtrusionRole::Brim;
    case Slic3r::erSupportMaterial:          return ToolpathExtrusionRole::Support;
    case Slic3r::erSupportMaterialInterface: return ToolpathExtrusionRole::SupportInterface;
    case Slic3r::erSupportTransition:        return ToolpathExtrusionRole::SupportTransition;
    case Slic3r::erWipeTower:                return ToolpathExtrusionRole::WipeTower;
    case Slic3r::erCustom:                   return ToolpathExtrusionRole::Custom;
    case Slic3r::erMixed:                    return ToolpathExtrusionRole::Mixed;
    default:                                 return ToolpathExtrusionRole::None;
    }
}

std::vector<int> filament_tool_map(const Slic3r::DynamicPrintConfig* config,
                                   std::size_t filament_count)
{
    std::vector<int> result(filament_count, 0);
    if (config == nullptr) {
        for (std::size_t index = 0; index < filament_count; ++index) {
            result[index] = static_cast<int>(index);
        }
        return result;
    }
    if (const auto* option = config->option<Slic3r::ConfigOptionInts>("filament_map")) {
        for (std::size_t index = 0; index < std::min(filament_count, option->values.size()); ++index) {
            result[index] = std::max(0, option->values[index] - 1);
        }
    }
    return result;
}

std::shared_ptr<ToolpathPreview> make_toolpath_preview(
    const Slic3r::GCodeProcessorResult& source,
    const Slic3r::DynamicPrintConfig* config,
    const std::string& source_path)
{
    auto preview = std::make_shared<ToolpathPreview>();
    preview->source_path = source_path;
    preview->supported_view_types = {
        ToolpathViewType::Summary,
        ToolpathViewType::FeatureType,
        ToolpathViewType::Filament,
        ToolpathViewType::Speed,
        ToolpathViewType::ActualSpeed,
        ToolpathViewType::Acceleration,
        ToolpathViewType::Jerk,
        ToolpathViewType::LayerHeight,
        ToolpathViewType::LineWidth,
        ToolpathViewType::VolumetricFlow,
        ToolpathViewType::ActualVolumetricFlow,
        ToolpathViewType::LayerTime,
        ToolpathViewType::LayerTimeLogarithmic,
        ToolpathViewType::FanSpeed,
        ToolpathViewType::Temperature,
        ToolpathViewType::PressureAdvance
    };

    std::size_t filament_count = std::max({
        source.filaments_count,
        source.extruder_colors.size(),
        source.filament_diameters.size(),
        source.filament_densities.size(),
        source.filament_costs.size()
    });
    for (const auto& move : source.moves) {
        filament_count = std::max(filament_count, static_cast<std::size_t>(move.extruder_id) + 1);
    }
    filament_count = std::max<std::size_t>(filament_count, 1);
    const auto tool_by_filament = filament_tool_map(config, filament_count);

    std::size_t tool_count = 1;
    for (const int tool : tool_by_filament) {
        tool_count = std::max(tool_count, static_cast<std::size_t>(std::max(0, tool)) + 1);
    }
    const Slic3r::ConfigOptionFloats* nozzle_diameters = config == nullptr
        ? nullptr
        : config->option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    if (nozzle_diameters != nullptr) {
        tool_count = std::max(tool_count, nozzle_diameters->values.size());
    }
    preview->tools.reserve(tool_count);
    for (std::size_t index = 0; index < tool_count; ++index) {
        ToolpathTool tool;
        tool.id = static_cast<std::uint16_t>(index);
        for (std::size_t filament = 0; filament < tool_by_filament.size(); ++filament) {
            if (tool_by_filament[filament] == static_cast<int>(index)) {
                tool.primary_filament_id = static_cast<std::uint16_t>(filament);
                break;
            }
        }
        if (nozzle_diameters != nullptr && index < nozzle_diameters->values.size()) {
            tool.nozzle_diameter_mm = static_cast<float>(nozzle_diameters->values[index]);
        }
        preview->tools.push_back(tool);
    }

    preview->filaments.reserve(filament_count);
    for (std::size_t index = 0; index < filament_count; ++index) {
        ToolpathFilament filament;
        filament.id = static_cast<std::uint16_t>(index);
        filament.tool_id = static_cast<std::uint16_t>(tool_by_filament[index]);
        filament.color = parse_color(index < source.extruder_colors.size()
                                         ? source.extruder_colors[index]
                                         : std::string{"#FF8000"});
        if (index < source.filament_diameters.size()) filament.diameter_mm = source.filament_diameters[index];
        if (index < source.filament_densities.size()) filament.density_g_cm3 = source.filament_densities[index];
        if (index < source.filament_costs.size()) filament.cost_per_kg = source.filament_costs[index];
        preview->filaments.push_back(filament);
    }

    std::map<std::uint16_t, std::uint16_t> color_filament;
    for (const auto& move : source.moves) {
        color_filament.emplace(move.cp_color_id, static_cast<std::uint16_t>(move.extruder_id));
    }
    for (const auto& [color_id, filament_id] : color_filament) {
        ToolpathColor color;
        color.id = color_id;
        color.filament_id = filament_id;
        color.source = color_id < filament_count
            ? ToolpathColorSource::Filament
            : ToolpathColorSource::ColorChange;
        color.color = filament_id < preview->filaments.size()
            ? preview->filaments[filament_id].color
            : parse_color("#FF8000");
        color.name = color.source == ToolpathColorSource::Filament
            ? "Filament " + std::to_string(static_cast<unsigned int>(filament_id) + 1)
            : "Color " + std::to_string(static_cast<unsigned int>(color_id) + 1);
        preview->colors.push_back(std::move(color));
    }

    struct LayerBuilder {
        std::vector<ToolpathSegment> segments;
        std::vector<ToolpathEvent> events;
        float print_z_mm{0.0f};
        float height_mm{0.0f};
        float duration_seconds{0.0f};
        bool has_print_z{false};
        bool has_height{false};
    };

    float min_speed = std::numeric_limits<float>::max();
    float min_actual_speed = std::numeric_limits<float>::max();
    float min_height = std::numeric_limits<float>::max();
    float min_width = std::numeric_limits<float>::max();
    float min_flow = std::numeric_limits<float>::max();
    float min_actual_flow = std::numeric_limits<float>::max();
    float min_fan_speed = std::numeric_limits<float>::max();
    float min_temperature = std::numeric_limits<float>::max();
    float min_pressure_advance = std::numeric_limits<float>::max();
    float min_acceleration = std::numeric_limits<float>::max();
    float min_jerk = std::numeric_limits<float>::max();
    std::map<std::uint32_t, LayerBuilder> layer_builders;
    std::optional<ToolpathPoint> previous_position;
    std::unordered_set<std::uint32_t> logical_motion_commands;
    std::uint64_t next_run_id = 0;
    bool previous_was_motion = false;
    ToolpathMotionKind previous_motion = ToolpathMotionKind::Travel;
    ToolpathExtrusionRole previous_role = ToolpathExtrusionRole::None;
    std::uint32_t previous_layer = invalid_toolpath_id;
    std::uint32_t previous_object = invalid_toolpath_id;
    std::uint16_t previous_tool = invalid_toolpath_small_id;
    std::uint16_t previous_color = invalid_toolpath_small_id;

    for (const auto& input : source.moves) {
        const std::uint32_t source_layer = input.layer_id;
        auto& layer = layer_builders[source_layer];
        layer.duration_seconds += input.time[static_cast<std::size_t>(
            Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)];

        const std::uint16_t filament_id = input.extruder_id;
        const std::uint16_t tool_id = input.extruder_id < tool_by_filament.size()
            ? static_cast<std::uint16_t>(tool_by_filament[input.extruder_id])
            : invalid_toolpath_small_id;
        const std::uint32_t object_id = input.object_label_id >= 0
            ? static_cast<std::uint32_t>(input.object_label_id)
            : invalid_toolpath_id;
        const auto motion = to_motion_kind(input.type);
        const bool has_end = finite_point(input.position);
        const ToolpathPoint end = has_end ? to_toolpath_point(input.position) : ToolpathPoint{};

        if (motion && previous_position && has_end &&
            point_distance(*previous_position, end) > 0.000001) {
            const ToolpathExtrusionRole role = *motion == ToolpathMotionKind::Extrusion
                ? to_extrusion_role(input.extrusion_role)
                : ToolpathExtrusionRole::None;
            const bool continues_run = previous_was_motion &&
                previous_motion == *motion &&
                previous_role == role &&
                previous_layer == source_layer &&
                previous_object == object_id &&
                previous_tool == tool_id &&
                previous_color == input.cp_color_id;
            if (!continues_run) {
                ++next_run_id;
            }

            ToolpathSegment segment;
            segment.run_id = next_run_id;
            segment.source_command_id = input.gcode_id;
            segment.object_id = object_id;
            segment.tool_id = tool_id;
            segment.filament_id = filament_id;
            segment.color_id = input.cp_color_id;
            segment.motion = *motion;
            segment.extrusion_role = role;
            segment.start_mm = *previous_position;
            segment.end_mm = end;
            segment.nominal_speed_mm_s = input.feedrate;
            segment.actual_speed_mm_s = input.actual_feedrate > 0.0f
                ? input.actual_feedrate
                : input.feedrate;
            segment.print_z_mm = input.print_z;
            segment.duration_seconds = input.time[static_cast<std::size_t>(
                Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)];
            segment.layer_duration_seconds = input.layer_duration;
            segment.fan_speed_percent = input.fan_speed;
            segment.temperature_c = input.temperature;
            segment.pressure_advance = input.pressure_advance;
            segment.acceleration_mm_s2 = input.acceleration;
            segment.jerk_mm_s = input.jerk;

            const double distance = point_distance(segment.start_mm, segment.end_mm);
            if (*motion == ToolpathMotionKind::Extrusion) {
                segment.width_mm = std::max(0.0f, input.width);
                segment.height_mm = std::max(0.0f, input.height);
                segment.mm3_per_mm = std::max(0.0f, input.mm3_per_mm);
                const double volume = distance * segment.mm3_per_mm;
                const float diameter = filament_id < preview->filaments.size() &&
                        preview->filaments[filament_id].diameter_mm > 0.0f
                    ? preview->filaments[filament_id].diameter_mm
                    : 1.75f;
                const double filament_area =
                    0.25 * 3.14159265358979323846 * diameter * diameter;
                segment.extrusion_delta_mm = filament_area > 0.0
                    ? static_cast<float>(volume / filament_area)
                    : 0.0f;

                preview->statistics.total_print_distance_mm += distance;
                preview->statistics.total_extrusion_volume_mm3 += volume;
                preview->statistics.total_extrusion_mm += segment.extrusion_delta_mm;
                if (segment.height_mm > 0.0f) {
                    min_height = std::min(min_height, segment.height_mm);
                    preview->statistics.max_layer_height_mm =
                        std::max(preview->statistics.max_layer_height_mm, segment.height_mm);
                    if (!layer.has_height) {
                        layer.height_mm = segment.height_mm;
                        layer.has_height = true;
                    }
                }
                if (segment.width_mm > 0.0f) {
                    min_width = std::min(min_width, segment.width_mm);
                    preview->statistics.max_width_mm =
                        std::max(preview->statistics.max_width_mm, segment.width_mm);
                }
                if (!layer.has_print_z) {
                    layer.print_z_mm = segment.print_z_mm > 0.0f
                        ? segment.print_z_mm
                        : segment.end_mm.z;
                    layer.has_print_z = true;
                }
            } else {
                preview->statistics.total_travel_distance_mm += distance;
            }

            const float speed = segment.nominal_speed_mm_s;
            if (speed > 0.0f) {
                min_speed = std::min(min_speed, speed);
                preview->statistics.max_speed_mm_s =
                    std::max(preview->statistics.max_speed_mm_s, speed);
            }
            const float actual_speed = segment.actual_speed_mm_s;
            if (actual_speed > 0.0f) {
                min_actual_speed = std::min(min_actual_speed, actual_speed);
                preview->statistics.max_actual_speed_mm_s =
                    std::max(preview->statistics.max_actual_speed_mm_s, actual_speed);
            }
            const float flow = *motion == ToolpathMotionKind::Extrusion
                ? segment.mm3_per_mm * speed
                : 0.0f;
            if (flow > 0.0f) {
                min_flow = std::min(min_flow, flow);
                preview->statistics.max_volumetric_flow_mm3_s =
                    std::max(preview->statistics.max_volumetric_flow_mm3_s, flow);
            }
            const float actual_flow = *motion == ToolpathMotionKind::Extrusion
                ? segment.mm3_per_mm * actual_speed
                : 0.0f;
            if (actual_flow > 0.0f) {
                min_actual_flow = std::min(min_actual_flow, actual_flow);
                preview->statistics.max_actual_volumetric_flow_mm3_s =
                    std::max(preview->statistics.max_actual_volumetric_flow_mm3_s, actual_flow);
            }
            const auto include_positive_range = [](float value, float& minimum, float& maximum) {
                if (value > 0.0f) {
                    minimum = std::min(minimum, value);
                    maximum = std::max(maximum, value);
                }
            };
            include_positive_range(segment.fan_speed_percent, min_fan_speed,
                                   preview->statistics.max_fan_speed_percent);
            include_positive_range(segment.temperature_c, min_temperature,
                                   preview->statistics.max_temperature_c);
            include_positive_range(segment.pressure_advance, min_pressure_advance,
                                   preview->statistics.max_pressure_advance);
            include_positive_range(segment.acceleration_mm_s2, min_acceleration,
                                   preview->statistics.max_acceleration_mm_s2);
            include_positive_range(segment.jerk_mm_s, min_jerk,
                                   preview->statistics.max_jerk_mm_s);

            logical_motion_commands.insert(input.gcode_id);
            include_point(preview->bounds, segment.start_mm);
            include_point(preview->bounds, segment.end_mm);
            layer.segments.push_back(std::move(segment));

            previous_was_motion = true;
            previous_motion = *motion;
            previous_role = role;
            previous_layer = source_layer;
            previous_object = object_id;
            previous_tool = tool_id;
            previous_color = input.cp_color_id;
        } else {
            previous_was_motion = false;
        }

        if (const auto event_kind = to_event_kind(input.type); event_kind && has_end) {
            ToolpathEvent event;
            event.kind = *event_kind;
            event.after_segment = layer.segments.size();
            event.source_command_id = input.gcode_id;
            event.tool_id = tool_id;
            event.filament_id = filament_id;
            event.position_mm = end;
            event.print_z_mm = input.print_z;
            event.time_seconds = input.time[static_cast<std::size_t>(
                Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)];
            layer.events.push_back(std::move(event));
        }

        if (has_end) {
            previous_position = end;
        }
    }

    preview->layers.reserve(layer_builders.size());
    preview->segments.reserve(source.moves.size());
    for (auto& [source_layer, builder] : layer_builders) {
        (void)source_layer;
        ToolpathLayer layer;
        layer.index = static_cast<std::uint32_t>(preview->layers.size());
        layer.segment_begin = preview->segments.size();
        layer.segment_count = builder.segments.size();
        layer.event_begin = preview->events.size();
        layer.event_count = builder.events.size();
        layer.print_z_mm = builder.print_z_mm;
        layer.height_mm = builder.height_mm;
        layer.duration_seconds = builder.duration_seconds;

        if (layer.duration_seconds > 0.0f) {
            preview->statistics.min_layer_time_seconds =
                preview->statistics.min_layer_time_seconds <= 0.0f
                    ? layer.duration_seconds
                    : std::min(preview->statistics.min_layer_time_seconds, layer.duration_seconds);
            preview->statistics.max_layer_time_seconds =
                std::max(preview->statistics.max_layer_time_seconds, layer.duration_seconds);
        }

        for (auto& segment : builder.segments) {
            segment.id = preview->segments.size();
            segment.layer_index = layer.index;
            preview->segments.push_back(std::move(segment));
        }
        for (auto& event : builder.events) {
            event.after_segment += layer.segment_begin;
            event.layer_index = layer.index;
            preview->events.push_back(std::move(event));
        }
        preview->layers.push_back(layer);
    }

    std::map<ToolpathExtrusionRole, ToolpathFeatureStatistics> feature_stats;
    std::map<ToolpathExtrusionRole, std::uint64_t> last_feature_run;
    for (const auto& segment : preview->segments) {
        if (segment.motion != ToolpathMotionKind::Extrusion ||
            segment.extrusion_role == ToolpathExtrusionRole::None) {
            continue;
        }
        auto& feature = feature_stats[segment.extrusion_role];
        feature.role = segment.extrusion_role;
        ++feature.render_segment_count;
        if (last_feature_run[segment.extrusion_role] != segment.run_id) {
            ++feature.path_count;
            last_feature_run[segment.extrusion_role] = segment.run_id;
        }
        const double distance = point_distance(segment.start_mm, segment.end_mm);
        feature.length_mm += distance;
        feature.extrusion_volume_mm3 += distance * segment.mm3_per_mm;
        feature.duration_seconds += segment.duration_seconds;
    }
    for (const auto& [source_role, filament] : source.print_statistics.used_filaments_per_role) {
        auto& feature = feature_stats[to_extrusion_role(source_role)];
        feature.role = to_extrusion_role(source_role);
        feature.filament_length_m += filament.first;
        feature.filament_weight_g += filament.second;
    }
    for (const auto& [role, feature] : feature_stats) {
        (void)role;
        preview->statistics.features.push_back(feature);
    }

    preview->statistics.total_layers = preview->layers.size();
    preview->statistics.logical_motion_count = logical_motion_commands.size();
    preview->statistics.render_segment_count = preview->segments.size();
    preview->statistics.total_time_seconds =
        source.print_statistics.modes[static_cast<std::size_t>(
            Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)].time;
    preview->statistics.total_travel_distance_mm = source.print_statistics.total_travel_distance;
    preview->statistics.total_filament_changes = source.print_statistics.total_filament_changes;
    preview->statistics.total_tool_changes = source.print_statistics.total_extruder_changes;
    preview->statistics.min_speed_mm_s =
        min_speed == std::numeric_limits<float>::max() ? 0.0f : min_speed;
    preview->statistics.min_actual_speed_mm_s =
        min_actual_speed == std::numeric_limits<float>::max() ? 0.0f : min_actual_speed;
    preview->statistics.min_layer_height_mm =
        min_height == std::numeric_limits<float>::max() ? 0.0f : min_height;
    preview->statistics.min_width_mm =
        min_width == std::numeric_limits<float>::max() ? 0.0f : min_width;
    preview->statistics.min_volumetric_flow_mm3_s =
        min_flow == std::numeric_limits<float>::max() ? 0.0f : min_flow;
    preview->statistics.min_actual_volumetric_flow_mm3_s =
        min_actual_flow == std::numeric_limits<float>::max() ? 0.0f : min_actual_flow;
    preview->statistics.min_fan_speed_percent =
        min_fan_speed == std::numeric_limits<float>::max() ? 0.0f : min_fan_speed;
    preview->statistics.min_temperature_c =
        min_temperature == std::numeric_limits<float>::max() ? 0.0f : min_temperature;
    preview->statistics.min_pressure_advance =
        min_pressure_advance == std::numeric_limits<float>::max() ? 0.0f : min_pressure_advance;
    preview->statistics.min_acceleration_mm_s2 =
        min_acceleration == std::numeric_limits<float>::max() ? 0.0f : min_acceleration;
    preview->statistics.min_jerk_mm_s =
        min_jerk == std::numeric_limits<float>::max() ? 0.0f : min_jerk;

    const auto normal_time = static_cast<std::size_t>(
        Slic3r::PrintEstimatedStatistics::ETimeMode::Normal);
    ToolpathOptionStatistics travel;
    travel.kind = ToolpathOptionKind::Travel;
    travel.occurrence_count = source.print_statistics.total_travel_moves;
    travel.distance_mm = source.print_statistics.total_travel_distance;
    ToolpathOptionStatistics wipe;
    wipe.kind = ToolpathOptionKind::Wipe;
    ToolpathOptionStatistics seam;
    seam.kind = ToolpathOptionKind::Seam;
    seam.distance_mm = source.print_statistics.total_seam_gap_distance +
        source.print_statistics.total_seam_scarf_distance;
    for (const auto& move : source.moves) {
        if (move.type == Slic3r::EMoveType::Travel) {
            travel.duration_seconds += move.time[normal_time];
        } else if (move.type == Slic3r::EMoveType::Wipe) {
            ++wipe.occurrence_count;
            wipe.duration_seconds += move.time[normal_time];
            wipe.distance_mm += move.travel_dist;
        } else if (move.type == Slic3r::EMoveType::Seam) {
            ++seam.occurrence_count;
            seam.duration_seconds += move.time[normal_time];
        }
    }
    if (travel.occurrence_count > 0) preview->statistics.options.push_back(travel);
    if (wipe.occurrence_count > 0) preview->statistics.options.push_back(wipe);
    if (seam.occurrence_count > 0) preview->statistics.options.push_back(seam);

    const auto map_value = [](const auto& values, std::size_t index) {
        const auto it = values.find(index);
        return it == values.end() ? 0.0 : it->second;
    };
    for (std::size_t index = 0; index < filament_count; ++index) {
        ToolpathFilamentUsage usage;
        usage.filament_id = static_cast<std::uint16_t>(index);
        usage.model_volume_mm3 = map_value(source.print_statistics.model_volumes_per_extruder, index);
        usage.support_volume_mm3 = map_value(source.print_statistics.support_volumes_per_extruder, index);
        usage.flushed_volume_mm3 = map_value(source.print_statistics.flush_per_filament, index);
        usage.tower_volume_mm3 = map_value(source.print_statistics.wipe_tower_volumes_per_extruder, index);
        usage.total_volume_mm3 = map_value(source.print_statistics.total_volumes_per_extruder, index);
        if (usage.total_volume_mm3 <= 0.0) {
            usage.total_volume_mm3 = usage.model_volume_mm3 + usage.support_volume_mm3 +
                usage.flushed_volume_mm3 + usage.tower_volume_mm3;
        }
        if (usage.total_volume_mm3 <= 0.0 && usage.model_volume_mm3 <= 0.0 &&
            usage.support_volume_mm3 <= 0.0 && usage.flushed_volume_mm3 <= 0.0 &&
            usage.tower_volume_mm3 <= 0.0) {
            continue;
        }
        preview->statistics.filament_usage.push_back(usage);

        const auto& filament = preview->filaments[index];
        const double diameter = filament.diameter_mm > 0.0f ? filament.diameter_mm : 1.75;
        const double area = 0.25 * 3.14159265358979323846 * diameter * diameter;
        const double length_mm = area > 0.0 ? usage.total_volume_mm3 / area : 0.0;
        const double weight_g = usage.total_volume_mm3 * filament.density_g_cm3 / 1000.0;
        preview->statistics.total_filament_length_mm += length_mm;
        preview->statistics.total_filament_weight_g += weight_g;
        preview->statistics.total_filament_cost += weight_g * filament.cost_per_kg / 1000.0;
    }
    return preview;
}

} // namespace

class Library::Impl
{
public:
    std::string resource_directory;
    Slic3r::PresetBundle presets;
    std::vector<MachineModelOption> machines;
    std::vector<BuildPlateOption> build_plates;
};

const char* version() noexcept
{
    return Slic3r::core_version();
}

Library::Library() : impl_(std::make_unique<Impl>()) {}
Library::Library(Library&&) noexcept = default;
Library& Library::operator=(Library&&) noexcept = default;
Library::~Library() = default;

std::unique_ptr<Library> Library::open(const LibraryOptions& options,
                                       std::vector<ConfigDiagnostic>* diagnostics)
{
    try {
        auto library = std::unique_ptr<Library>(new Library());
        library->impl_->resource_directory = locate_resource_directory(options.resource_directory);
        Slic3r::set_resources_dir(library->impl_->resource_directory);
        const fs::path data_directory = fs::temp_directory_path() / "libslicer";
        fs::create_directories(data_directory);
        Slic3r::set_data_dir(data_directory.string());

        const fs::path profiles_directory = fs::path(library->impl_->resource_directory) / "profiles";
        std::vector<std::string> vendors = options.vendors.empty() ?
                                               discover_vendors(profiles_directory) :
                                               options.vendors;
        if (vendors.empty()) {
            throw std::runtime_error("libslicer contains no vendor preset bundles");
        }

        constexpr const char* filament_library = "OrcaFilamentLibrary";
        const bool has_filament_library = fs::is_regular_file(profiles_directory / "OrcaFilamentLibrary.json");
        if (has_filament_library) {
            vendors.erase(std::remove(vendors.begin(), vendors.end(), filament_library), vendors.end());
            library->impl_->presets.load_vendor_configs_from_json(
                profiles_directory.string(), filament_library, Slic3r::PresetBundle::LoadSystem,
                Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        }
        for (const std::string& vendor : vendors) {
            library->impl_->presets.load_vendor_configs_from_json(
                profiles_directory.string(), vendor, Slic3r::PresetBundle::LoadSystem,
                Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent,
                has_filament_library ? &library->impl_->presets : nullptr);
        }

        for (const auto& [vendor_id, vendor] : library->impl_->presets.vendors) {
            for (const auto& model : vendor.models) {
                if (model.technology != Slic3r::ptFFF) {
                    continue;
                }
                MachineModelOption machine;
                machine.id               = model.id;
                machine.vendor_id        = vendor_id;
                machine.name             = model.name.empty() ? model.id : model.name;
                machine.family           = model.family;
                machine.cover_image_path = resource_path(library->impl_->resource_directory,
                                                         vendor_id, machine.name + "_cover.png");
                machine.bed_model_path   = resource_path(library->impl_->resource_directory, vendor_id, model.bed_model);
                machine.bed_texture_path = renderable_texture_path(library->impl_->resource_directory,
                                                                   vendor_id, model.bed_texture);

                for (const auto& variant : model.variants) {
                    const auto* preset = library->impl_->presets.printers.find_system_preset_by_model_and_variant(
                        model.id, variant.name);
                    if (preset == nullptr) {
                        continue;
                    }
                    MachineVariantOption option;
                    option.id                = variant.name;
                    option.name              = variant.name + " mm";
                    option.nozzle_diameter   = nozzle_diameter(variant.name);
                    option.printer_preset_id = preset->name;
                    populate_printable_volume(preset->config, option);
                    machine.variants.push_back(std::move(option));
                }
                if (!machine.variants.empty()) {
                    library->impl_->machines.push_back(std::move(machine));
                }
            }
        }
        std::sort(library->impl_->machines.begin(), library->impl_->machines.end(),
                  [](const MachineModelOption& left, const MachineModelOption& right) {
                      return left.name < right.name;
                  });
        if (library->impl_->machines.empty()) {
            throw std::runtime_error("libslicer loaded no FFF machine variants");
        }

        const auto settings = Config::defaults().settings();
        const auto plate_setting = std::find_if(settings.begin(), settings.end(), [](const SettingItem& item) {
            return item.key == "curr_bed_type";
        });
        if (plate_setting != settings.end()) {
            for (const EnumItem& plate : plate_setting->enum_items) {
                library->impl_->build_plates.push_back({
                    plate.value,
                    plate.label,
                    build_plate_image_path(library->impl_->resource_directory, plate.value)
                });
            }
        }
        return library;
    } catch (const std::exception& error) {
        append_diagnostic(diagnostics, "resources", error.what());
        return nullptr;
    }
}

const std::string& Library::resource_directory() const noexcept
{
    return impl_->resource_directory;
}

const std::vector<MachineModelOption>& Library::machine_models() const noexcept
{
    return impl_->machines;
}

const std::vector<BuildPlateOption>& Library::build_plate_options() const noexcept
{
    return impl_->build_plates;
}

ConfigCreateResult Library::create_config(const ConfigSelection& selection) const
{
    ConfigCreateResult result;
    try {
        auto selected = impl_->presets;
        const Slic3r::Preset* printer = selected.printers.find_system_preset_by_model_and_variant(
            selection.machine_model_id, selection.machine_variant_id);
        if (printer == nullptr) {
            result.diagnostics.push_back({"machine", "Unknown machine model or nozzle variant"});
            return result;
        }

        const std::string printer_name = printer->name;
        selected.printers.select_preset_by_name(printer_name, true);
        selected.update_compatible(Slic3r::PresetSelectCompatibleType::Never);

        const Slic3r::Preset& active_printer = selected.printers.get_edited_preset();
        std::string process_name = selection.process_preset_id;
        if (!process_name.empty()) {
            const Slic3r::Preset* requested = selected.prints.find_preset(process_name, false);
            if (requested == nullptr) {
                result.diagnostics.push_back({"process", "Unknown process preset: " + process_name});
                return result;
            }
            if (!requested->is_visible || !requested->is_compatible) {
                result.diagnostics.push_back({"process", "Process preset is incompatible with the selected machine: " + process_name});
                return result;
            }
        } else {
            process_name = active_printer.config.opt_string("default_print_profile");
            const Slic3r::Preset* preferred = process_name.empty()
                ? nullptr
                : selected.prints.find_preset(process_name, false);
            if (preferred == nullptr || !preferred->is_visible || !preferred->is_compatible) {
                process_name = first_compatible_preset_name(selected.prints);
            }
        }
        if (process_name.empty()) {
            throw std::runtime_error("Selected machine has no compatible process preset");
        }
        selected.prints.select_preset_by_name(process_name, true);
        selected.update_compatible(Slic3r::PresetSelectCompatibleType::Always);

        std::vector<std::string> filament_names = selection.filament_preset_ids;
        if (!filament_names.empty()) {
            for (const std::string& filament_name : filament_names) {
                const Slic3r::Preset* requested = selected.filaments.find_preset(filament_name, false);
                if (requested == nullptr) {
                    result.diagnostics.push_back({"filament", "Unknown filament preset: " + filament_name});
                    return result;
                }
                if (!requested->is_visible || !requested->is_compatible) {
                    result.diagnostics.push_back({"filament", "Filament preset is incompatible with the selected machine and process: " + filament_name});
                    return result;
                }
            }
        } else {
            if (const auto* defaults = active_printer.config.option<Slic3r::ConfigOptionStrings>("default_filament_profile")) {
                filament_names = defaults->values;
            }
            const bool defaults_valid = !filament_names.empty() &&
                std::all_of(filament_names.begin(), filament_names.end(), [&selected](const std::string& name) {
                    const Slic3r::Preset* preset = selected.filaments.find_preset(name, false);
                    return preset != nullptr && preset->is_visible && preset->is_compatible;
                });
            if (!defaults_valid) {
                const std::string default_name = first_compatible_preset_name(selected.filaments);
                if (default_name.empty()) {
                    throw std::runtime_error("Selected machine has no compatible filament preset");
                }
                filament_names = {default_name};
            }
        }
        if (filament_names.empty()) {
                throw std::runtime_error("Selected machine has no compatible filament preset");
        }
        selected.filaments.select_preset_by_name(filament_names.front(), true);
        selected.filament_presets = filament_names;
        selected.update_compatible(Slic3r::PresetSelectCompatibleType::Always);
        selected.update_multi_material_filament_presets();

        result.selection.machine_model_id    = selection.machine_model_id;
        result.selection.machine_variant_id  = selection.machine_variant_id;
        result.selection.printer_preset_id   = selected.printers.get_selected_preset_name();
        result.selection.process_preset_id   = selected.prints.get_selected_preset_name();
        result.selection.filament_preset_ids = selected.filament_presets;
        result.compatible_processes = compatible_presets(selected.prints, result.selection.process_preset_id);
        result.compatible_filaments = compatible_presets(selected.filaments,
                                                         result.selection.filament_preset_ids.empty() ? std::string{} :
                                                                                                        result.selection.filament_preset_ids.front());

        result.config = std::unique_ptr<Config>(new Config(serialized_values(selected.full_config())));
        result.success = true;
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"selection", error.what()});
    }
    return result;
}

SliceResult Library::slice(const SliceRequest& request, const SliceCallbacks& callbacks) const
{
    SliceResult result;
    std::string generated_temporary_path;
    const auto discard_generated_temporary = [&generated_temporary_path]() {
        if (generated_temporary_path.empty()) {
            return;
        }
        std::error_code error;
        fs::remove(generated_temporary_path, error);
        generated_temporary_path.clear();
    };
    try {
        if (request.objects.empty()) {
            result.diagnostics.push_back({"input", "Slice request contains no models", false});
            return result;
        }
        if (!request.config.valid()) {
            result.diagnostics.push_back({"config", "Slice request has no initialized configuration", false});
            return result;
        }
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        report_progress(callbacks, 0.02f, "Loading models");
        Slic3r::Model plate_model;
        for (const SliceObjectInput& input : request.objects) {
            const std::string& path = input.model_path;
            std::error_code error;
            if (!fs::is_regular_file(path, error) || error) {
                result.diagnostics.push_back({"model", "Model file does not exist: " + path, false});
                return result;
            }
            Slic3r::Model source = Slic3r::Model::read_from_file(path);
            if (!input.support_enforcer_paths.empty() && source.objects.size() != 1) {
                result.diagnostics.push_back({
                    "support",
                    "A model with support enforcers must contain exactly one printable object: " + path,
                    false});
                return result;
            }
            Slic3r::ModelObject* support_target = nullptr;
            for (const Slic3r::ModelObject* object : source.objects) {
                Slic3r::ModelObject* added = plate_model.add_object(*object);
                if (support_target == nullptr) {
                    support_target = added;
                }
            }

            if (!input.support_enforcer_paths.empty()) {
                const Slic3r::Transform3d target_instance = support_target->instances.empty()
                    ? Slic3r::Transform3d::Identity()
                    : support_target->instances.front()->get_matrix();
                const Slic3r::Transform3d world_to_target = target_instance.inverse();

                for (const std::string& support_path : input.support_enforcer_paths) {
                    std::error_code support_error;
                    if (!fs::is_regular_file(support_path, support_error) || support_error) {
                        result.diagnostics.push_back({
                            "support", "Support enforcer file does not exist: " + support_path, false});
                        return result;
                    }
                    Slic3r::Model support_source = Slic3r::Model::read_from_file(support_path);
                    for (const Slic3r::ModelObject* object : support_source.objects) {
                        const std::size_t instance_count = std::max<std::size_t>(1, object->instances.size());
                        for (std::size_t instance_index = 0; instance_index < instance_count; ++instance_index) {
                            Slic3r::TriangleMesh mesh = object->mesh();
                            if (!object->instances.empty()) {
                                mesh.transform(object->instances[instance_index]->get_matrix());
                            }
                            mesh.transform(world_to_target);
                            support_target->add_volume(
                                std::move(mesh), Slic3r::ModelVolumeType::SUPPORT_ENFORCER, false);
                        }
                    }
                    if (cancellation_requested(callbacks)) {
                        result.cancelled = true;
                        return result;
                    }
                }
                support_target->invalidate_bounding_box();
            }
            if (cancellation_requested(callbacks)) {
                result.cancelled = true;
                return result;
            }
        }
        if (plate_model.objects.empty()) {
            result.diagnostics.push_back({"model", "Loaded models contain no printable objects", false});
            return result;
        }

        Slic3r::DynamicPrintConfig config = dynamic_config(request.config);
        if (request.center_on_build_plate) {
            plate_model.center_instances_around_point(build_plate_center(config));
        }

        report_progress(callbacks, 0.08f, "Validating print");
        Slic3r::Print print;
        print.set_status_callback([&](const Slic3r::PrintBase::SlicingStatus& status) {
            if (cancellation_requested(callbacks)) {
                print.cancel();
            }
            const float engine_progress = status.percent < 0 ? 0.0f : static_cast<float>(status.percent) / 100.0f;
            report_progress(callbacks, 0.08f + engine_progress * 0.78f, status.text);
        });
        print.apply(plate_model, config);

        Slic3r::StringObjectException warning;
        const Slic3r::StringObjectException validation_error = print.validate(&warning);
        if (!warning.string.empty()) {
            result.diagnostics.push_back({"validation", warning.string, true});
        }
        if (!validation_error.string.empty()) {
            result.diagnostics.push_back({"validation", validation_error.string, false});
            return result;
        }
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        report_progress(callbacks, 0.1f, "Slicing model");
        print.process();
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        const bool library_temporary = request.output_gcode_path.empty();
        std::string output_path = library_temporary ? temporary_gcode_path() : request.output_gcode_path;
        if (library_temporary) {
            generated_temporary_path = output_path;
        }
        const fs::path output_parent = fs::path(output_path).parent_path();
        if (!output_parent.empty()) {
            fs::create_directories(output_parent);
        }

        report_progress(callbacks, 0.88f, "Exporting G-code");
        Slic3r::GCodeProcessorResult processor_result;
        result.output.path = print.export_gcode(output_path, &processor_result);
        result.output.ownership = library_temporary
            ? OutputArtifactOwnership::LibraryTemporary
            : OutputArtifactOwnership::CallerOwned;
        std::error_code file_error;
        if (result.output.path.empty() || !fs::is_regular_file(result.output.path, file_error) || file_error ||
            fs::file_size(result.output.path, file_error) == 0 || file_error) {
            result.diagnostics.push_back({"output", "Slicer did not generate a valid G-code file", false});
            discard_generated_temporary();
            result.output.path.clear();
            return result;
        }

        const auto& statistics = print.print_statistics();
        result.summary.filament_used_mm = statistics.total_used_filament;
        result.summary.filament_weight_g = statistics.total_weight;
        std::unordered_set<unsigned int> logical_motion_commands;
        for (const auto& move : processor_result.moves) {
            result.summary.layer_count = std::max(result.summary.layer_count,
                                                  static_cast<std::size_t>(move.layer_id) + 1);
            if (to_motion_kind(move.type)) {
                logical_motion_commands.insert(move.gcode_id);
            }
        }
        result.summary.logical_motion_count = logical_motion_commands.size();
        result.summary.estimated_time_seconds =
            processor_result.print_statistics.modes[static_cast<std::size_t>(
                Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)].time;

        if (request.generate_preview) {
            report_progress(callbacks, 0.96f, "Preparing toolpath preview");
            result.preview = make_toolpath_preview(processor_result, &config, result.output.path);
            if (!result.preview || result.preview->layers.empty() ||
                result.preview->segments.empty()) {
                result.diagnostics.push_back({"preview", "Slicer generated no drawable toolpath preview", false});
                discard_generated_temporary();
                result.output.path.clear();
                result.preview.reset();
                return result;
            }
            result.summary.logical_motion_count =
                result.preview->statistics.logical_motion_count;
            result.summary.render_segment_count = result.preview->segments.size();
        }

        result.success = true;
        generated_temporary_path.clear();
        report_progress(callbacks, 1.0f, "Slicing completed");
    } catch (const Slic3r::CanceledException&) {
        result.cancelled = true;
        discard_generated_temporary();
        result.output.path.clear();
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"slice", error.what(), false});
        discard_generated_temporary();
        result.output.path.clear();
    }
    return result;
}

GCodePreviewResult Library::load_gcode_preview(const GCodePreviewRequest& request,
                                               const SliceCallbacks& callbacks) const
{
    GCodePreviewResult result;
    try {
        std::error_code file_error;
        if (request.gcode_path.empty() ||
            !fs::is_regular_file(request.gcode_path, file_error) || file_error) {
            result.diagnostics.push_back({"input", "G-code file does not exist: " + request.gcode_path, false});
            return result;
        }
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        report_progress(callbacks, 0.05f, "Loading G-code");
        Slic3r::GCodeProcessor processor;
        processor.process_file(request.gcode_path, [&callbacks]() {
            if (cancellation_requested(callbacks)) {
                throw Slic3r::CanceledException();
            }
        });
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        report_progress(callbacks, 0.9f, "Preparing toolpath preview");
        result.preview = make_toolpath_preview(processor.get_result(), nullptr, request.gcode_path);
        if (!result.preview || result.preview->layers.empty() ||
            result.preview->segments.empty()) {
            result.diagnostics.push_back({"preview", "G-code contains no drawable toolpath", false});
            result.preview.reset();
            return result;
        }
        result.success = true;
        report_progress(callbacks, 1.0f, "G-code preview ready");
    } catch (const Slic3r::CanceledException&) {
        result.cancelled = true;
        result.preview.reset();
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"gcode", error.what(), false});
        result.preview.reset();
    }
    return result;
}

} // namespace libslicer
