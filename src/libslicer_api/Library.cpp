#define NANOSVG_IMPLEMENTATION
#include <nanosvg/nanosvg.h>

#include <libslicer/Library.hpp>

#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/GCode/ThumbnailData.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/PNGReadWrite.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>
#include <libslic3r/miniz_extension.hpp>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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
        option.printable_area.clear();
        option.printable_area.reserve(area->values.size());
        double min_x = std::numeric_limits<double>::max();
        double max_x = std::numeric_limits<double>::lowest();
        double min_y = std::numeric_limits<double>::max();
        double max_y = std::numeric_limits<double>::lowest();
        for (const auto& point : area->values) {
            option.printable_area.push_back({point.x(), point.y()});
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
        } else {
            return {};
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

std::vector<std::string> serialized_option_values(const Slic3r::DynamicPrintConfig& config,
                                                  const std::string& key)
{
    const Slic3r::ConfigOption* option = config.option(key);
    if (option == nullptr) {
        return {};
    }
    if (const auto* vector = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(option)) {
        return vector->vserialize();
    }
    return {option->serialize()};
}

std::string option_value_at(const Slic3r::DynamicPrintConfig& config,
                            const std::string& key,
                            std::size_t index)
{
    const auto values = serialized_option_values(config, key);
    if (values.empty()) {
        return {};
    }
    return values[std::min(index, values.size() - 1)];
}

int hexadecimal_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

ProjectImportColor project_import_color(const std::string& value)
{
    ProjectImportColor result;
    if ((value.size() != 7 && value.size() != 9) || value.front() != '#') {
        return result;
    }
    const auto component = [&value](std::size_t offset) -> std::optional<float> {
        const int high = hexadecimal_digit(value[offset]);
        const int low = hexadecimal_digit(value[offset + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        return static_cast<float>((high << 4) | low) / 255.0f;
    };
    const auto red = component(1);
    const auto green = component(3);
    const auto blue = component(5);
    const auto alpha = value.size() == 9 ? component(7) : std::optional<float>{1.0f};
    if (!red || !green || !blue || !alpha) {
        return result;
    }
    result.red = *red;
    result.green = *green;
    result.blue = *blue;
    result.alpha = *alpha;
    return result;
}

std::vector<ProjectImportFilament> project_import_filaments(
    const Slic3r::DynamicPrintConfig& config,
    std::size_t minimum_count)
{
    std::size_t count = minimum_count;
    for (const std::string& key : {"filament_settings_id", "filament_type",
                                   "filament_vendor", "filament_colour"}) {
        count = std::max(count, serialized_option_values(config, key).size());
    }

    std::vector<ProjectImportFilament> result(count);
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto& filament = result[index];
        filament.id = "filament-" + std::to_string(index + 1);
        filament.preset_id = option_value_at(config, "filament_settings_id", index);
        filament.name = filament.preset_id.empty() ? filament.id : filament.preset_id;
        filament.vendor = option_value_at(config, "filament_vendor", index);
        filament.material_type = option_value_at(config, "filament_type", index);
        filament.color = project_import_color(
            option_value_at(config, "filament_colour", index));
    }

    return result;
}

void normalize_filament_identity(Slic3r::DynamicPrintConfig& config)
{
    const auto* variants = config.option<Slic3r::ConfigOptionStrings>("filament_extruder_variant");
    if (variants == nullptr || variants->values.empty()) {
        return;
    }

    auto* self_indices = config.option<Slic3r::ConfigOptionInts>("filament_self_index", true);
    if (self_indices->values.size() == variants->values.size()) {
        return;
    }

    self_indices->values.resize(variants->values.size());
    for (std::size_t index = 0; index < self_indices->values.size(); ++index) {
        self_indices->values[index] = static_cast<int>(index + 1);
    }
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

std::vector<std::string> config_vector_values(const Config& config, const std::string& key)
{
    return serialized_option_values(dynamic_config(config.snapshot()), key);
}

std::string serialize_strings(const std::vector<std::string>& values)
{
    return Slic3r::ConfigOptionStrings(values).serialize();
}

SettingsResult settings_failure(std::string key, std::string message)
{
    SettingsResult result;
    result.diagnostics.push_back({std::move(key), std::move(message)});
    return result;
}

void append_changed_items(std::vector<SettingItem>& target, std::vector<SettingItem> source)
{
    for (auto& item : source) {
        const auto existing = std::find_if(target.begin(), target.end(), [&item](const SettingItem& value) {
            return value.key == item.key;
        });
        if (existing == target.end()) {
            target.push_back(std::move(item));
        } else {
            *existing = std::move(item);
        }
    }
}

SettingsResult normalize_filament_colors(Config& config,
                                          std::size_t slot_count,
                                          const std::vector<std::string>& preferred_colors = {})
{
    if (slot_count == 0) {
        return settings_failure("filament", "The active configuration has no filament slots");
    }
    std::vector<std::string> colors = preferred_colors.empty()
        ? config_vector_values(config, "filament_colour")
        : preferred_colors;
    if (colors.empty()) {
        colors.push_back("#00AE42");
    }
    colors.resize(slot_count, colors.back());
    return config.set("filament_colour", serialize_strings(colors));
}

std::vector<ConfigDiagnostic> filament_cardinality_diagnostics(
    const Config& config, std::size_t slot_count)
{
    std::vector<ConfigDiagnostic> diagnostics;
    const auto check = [&config, slot_count, &diagnostics](const char* key, bool required) {
        const std::size_t count = config_vector_values(config, key).size();
        if ((required || count != 0) && count != slot_count) {
            diagnostics.push_back({
                key,
                "Expected " + std::to_string(slot_count) + " values, received " +
                    std::to_string(count)});
        }
    };
    check("filament_colour", true);
    check("filament_diameter", true);
    check("filament_settings_id", false);
    check("filament_type", false);
    const std::size_t tool_count = config_vector_values(config, "nozzle_diameter").size();
    const std::size_t flush_multiplier_count =
        config_vector_values(config, "flush_multiplier").size();
    const std::size_t flush_matrix_count =
        config_vector_values(config, "flush_volumes_matrix").size();
    if (slot_count > 1 && flush_multiplier_count != tool_count) {
        diagnostics.push_back({
            "flush_multiplier",
            "Expected " + std::to_string(tool_count) + " tool-head multipliers, received " +
                std::to_string(flush_multiplier_count)});
    }
    const std::size_t expected_flush_matrix_count =
        slot_count * slot_count * flush_multiplier_count;
    if (slot_count > 1 && flush_matrix_count != expected_flush_matrix_count) {
        diagnostics.push_back({
            "flush_volumes_matrix",
            "Expected " + std::to_string(expected_flush_matrix_count) +
                " purge-volume values, received " + std::to_string(flush_matrix_count)});
    }
    return diagnostics;
}

Rgba8 rgba8_color(const std::string& serialized)
{
    const ProjectImportColor parsed = project_import_color(serialized);
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
    };
    return {byte(parsed.red), byte(parsed.green), byte(parsed.blue), byte(parsed.alpha)};
}

std::string serialized_color(Rgba8 color)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output(9, '0');
    output[0] = '#';
    const auto write = [&output](std::size_t offset, std::uint8_t value) {
        output[offset] = digits[value >> 4];
        output[offset + 1] = digits[value & 0x0f];
    };
    write(1, color.red);
    write(3, color.green);
    write(5, color.blue);
    write(7, color.alpha);
    return output;
}

std::vector<ConfigDiagnostic> slice_filament_diagnostics(
    const Slic3r::DynamicPrintConfig& config)
{
    std::vector<ConfigDiagnostic> diagnostics;
    const std::size_t diameter_count = serialized_option_values(config, "filament_diameter").size();
    const std::size_t color_count = serialized_option_values(config, "filament_colour").size();
    const std::size_t tool_count = serialized_option_values(config, "nozzle_diameter").size();
    const std::size_t flush_multiplier_count =
        serialized_option_values(config, "flush_multiplier").size();
    const std::size_t flush_matrix_count =
        serialized_option_values(config, "flush_volumes_matrix").size();
    if (diameter_count == 0) {
        diagnostics.push_back({"filament_diameter", "The slicing configuration has no filament slots"});
    }
    if (color_count != diameter_count) {
        diagnostics.push_back({
            "filament_colour",
            "Expected " + std::to_string(diameter_count) + " filament colours, received " +
                std::to_string(color_count)});
    }
    if (color_count > 1 && flush_multiplier_count != tool_count) {
        diagnostics.push_back({
            "flush_multiplier",
            "Expected " + std::to_string(tool_count) + " tool-head multipliers, received " +
                std::to_string(flush_multiplier_count)});
    }
    const std::size_t expected_flush_matrix_count =
        color_count * color_count * flush_multiplier_count;
    if (color_count > 1 && flush_matrix_count != expected_flush_matrix_count) {
        diagnostics.push_back({
            "flush_volumes_matrix",
            "Expected " + std::to_string(expected_flush_matrix_count) +
                " purge-volume values, received " + std::to_string(flush_matrix_count)});
    }
    return diagnostics;
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

std::string temporary_output_path(std::string_view suffix)
{
    static std::atomic<unsigned long long> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() /
            ("libslicer_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + std::string(suffix))).string();
}

bool extract_zip_entry(mz_zip_archive& archive, const char* name, std::vector<unsigned char>& data)
{
    const int index = mz_zip_reader_locate_file(&archive, name, nullptr, 0);
    mz_zip_archive_file_stat stat;
    if (index < 0 || !mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat) ||
        stat.m_is_directory || stat.m_uncomp_size == 0 ||
        stat.m_uncomp_size > static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    data.resize(static_cast<std::size_t>(stat.m_uncomp_size));
    return mz_zip_reader_extract_to_mem(&archive, static_cast<mz_uint>(index), data.data(), data.size(), 0);
}

struct DigestWriter {
    EVP_MD_CTX* context{nullptr};
    mz_uint64 next_offset{0};
};

std::size_t update_digest(void* opaque, mz_uint64 offset, const void* buffer, std::size_t size)
{
    auto& writer = *static_cast<DigestWriter*>(opaque);
    if (writer.context == nullptr || offset != writer.next_offset ||
        EVP_DigestUpdate(writer.context, buffer, size) != 1) {
        return 0;
    }
    writer.next_offset += size;
    return size;
}

bool validate_gcode_3mf(const std::string& path, std::string& error)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!Slic3r::open_zip_reader(&archive, path)) {
        error = "the generated file is not a readable ZIP archive";
        return false;
    }
    struct ArchiveCloser {
        mz_zip_archive& archive;
        ~ArchiveCloser() { Slic3r::close_zip_reader(&archive); }
    } closer{archive};

    if (!mz_zip_validate_archive(&archive, MZ_ZIP_FLAG_VALIDATE_LOCATE_FILE_FLAG)) {
        error = "the generated ZIP archive failed CRC validation";
        return false;
    }

    constexpr std::array<const char*, 10> required_entries = {
        "[Content_Types].xml",
        "_rels/.rels",
        "3D/3dmodel.model",
        "Metadata/project_settings.config",
        "Metadata/model_settings.config",
        "Metadata/slice_info.config",
        "Metadata/plate_1.gcode",
        "Metadata/plate_1.gcode.md5",
        "Metadata/plate_1.png",
        "Metadata/plate_1_small.png",
    };
    for (const char* entry : required_entries) {
        mz_zip_archive_file_stat stat;
        const int index = mz_zip_reader_locate_file(&archive, entry, nullptr, 0);
        if (index < 0 || !mz_zip_reader_file_stat(&archive, static_cast<mz_uint>(index), &stat) ||
            stat.m_is_directory || stat.m_uncomp_size == 0) {
            error = std::string("the generated archive is missing required entry ") + entry;
            return false;
        }
    }

    std::vector<unsigned char> project_config_bytes;
    if (!extract_zip_entry(archive, "Metadata/project_settings.config", project_config_bytes)) {
        error = "the generated archive contains no readable project configuration";
        return false;
    }
    try {
        const auto project_config = nlohmann::json::parse(
            project_config_bytes.begin(), project_config_bytes.end());
        const auto variants = project_config.find("filament_extruder_variant");
        const auto self_indices = project_config.find("filament_self_index");
        if (variants != project_config.end() && variants->is_array() && !variants->empty() &&
            (self_indices == project_config.end() || !self_indices->is_array() ||
             self_indices->size() != variants->size())) {
            error = "the generated project configuration has inconsistent filament_extruder_variant and filament_self_index arrays";
            return false;
        }
    } catch (const std::exception& exception) {
        error = std::string("the generated project configuration is invalid JSON: ") + exception.what();
        return false;
    }

    std::vector<unsigned char> relationship_bytes;
    if (!extract_zip_entry(archive, "_rels/.rels", relationship_bytes)) {
        error = "the generated archive contains no readable root relationships";
        return false;
    }
    const std::string relationships(relationship_bytes.begin(), relationship_bytes.end());
    for (const char* target : {"/3D/3dmodel.model", "/Metadata/plate_1.png",
                               "/Metadata/plate_1_small.png"}) {
        if (relationships.find(std::string("Target=\"") + target + '"') == std::string::npos) {
            error = std::string("the generated archive has no relationship to ") + target;
            return false;
        }
    }

    constexpr std::array<unsigned char, 8> png_signature = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    for (const char* thumbnail : {"Metadata/plate_1.png", "Metadata/plate_1_small.png"}) {
        std::vector<unsigned char> bytes;
        if (!extract_zip_entry(archive, thumbnail, bytes) || bytes.size() < png_signature.size() ||
            !std::equal(png_signature.begin(), png_signature.end(), bytes.begin())) {
            error = std::string("the generated archive contains an invalid PNG entry ") + thumbnail;
            return false;
        }
        Slic3r::png::ImageColorscale image;
        if (!Slic3r::png::decode_colored_png({bytes.data(), bytes.size()}, image) ||
            image.rows == 0 || image.cols == 0 || image.bytes_per_pixel < 3) {
            error = std::string("the generated archive contains an unreadable thumbnail ") + thumbnail;
            return false;
        }
        const auto pixel_differs = [&image](std::size_t pixel) {
            for (int component = 0; component < image.bytes_per_pixel; ++component) {
                if (image.buf[pixel * image.bytes_per_pixel + component] != image.buf[component]) {
                    return true;
                }
            }
            return false;
        };
        const std::size_t pixel_count = image.rows * image.cols;
        bool has_image_content = false;
        for (std::size_t pixel = 1; pixel < pixel_count && !has_image_content; ++pixel) {
            has_image_content = pixel_differs(pixel);
        }
        if (!has_image_content) {
            error = std::string("the generated archive contains a blank thumbnail ") + thumbnail;
            return false;
        }
    }

    std::vector<unsigned char> expected_digest_bytes;
    if (!extract_zip_entry(archive, "Metadata/plate_1.gcode.md5", expected_digest_bytes)) {
        error = "the generated archive contains no readable G-code checksum";
        return false;
    }
    std::string expected_digest(expected_digest_bytes.begin(), expected_digest_bytes.end());
    expected_digest.erase(std::remove_if(expected_digest.begin(), expected_digest.end(),
                                         [](unsigned char value) { return std::isspace(value) != 0; }),
                          expected_digest.end());
    std::transform(expected_digest.begin(), expected_digest.end(), expected_digest.begin(),
                   [](unsigned char value) { return static_cast<char>(std::toupper(value)); });

    const int gcode_index = mz_zip_reader_locate_file(&archive, "Metadata/plate_1.gcode", nullptr, 0);
    EVP_MD_CTX* digest_context = EVP_MD_CTX_new();
    if (gcode_index < 0 || digest_context == nullptr || EVP_DigestInit_ex(digest_context, EVP_md5(), nullptr) != 1) {
        EVP_MD_CTX_free(digest_context);
        error = "the generated archive G-code checksum could not be initialized";
        return false;
    }
    DigestWriter writer{digest_context};
    const bool extracted = mz_zip_reader_extract_to_callback(
        &archive, static_cast<mz_uint>(gcode_index), update_digest, &writer, 0);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    const bool finalized = extracted && EVP_DigestFinal_ex(digest_context, digest.data(), &digest_size) == 1;
    EVP_MD_CTX_free(digest_context);
    if (!finalized || digest_size != 16) {
        error = "the generated archive G-code checksum could not be calculated";
        return false;
    }
    static constexpr char hex_digits[] = "0123456789ABCDEF";
    std::string actual_digest;
    actual_digest.reserve(digest_size * 2);
    for (unsigned int index = 0; index < digest_size; ++index) {
        actual_digest.push_back(hex_digits[digest[index] >> 4]);
        actual_digest.push_back(hex_digits[digest[index] & 0x0f]);
    }
    if (expected_digest != actual_digest) {
        error = "the generated archive G-code does not match its MD5 checksum";
        return false;
    }
    return true;
}

struct ProjectedThumbnailTriangle
{
    std::array<Slic3r::Vec3d, 3> world;
    std::array<Slic3r::Vec3d, 3> projected;
    std::array<unsigned char, 3> color;
    double shade{1.0};
};

struct ProjectedThumbnailScene
{
    std::vector<ProjectedThumbnailTriangle> triangles;
    double min_x{std::numeric_limits<double>::max()};
    double min_y{std::numeric_limits<double>::max()};
    double max_x{std::numeric_limits<double>::lowest()};
    double max_y{std::numeric_limits<double>::lowest()};

    bool valid() const noexcept
    {
        return !triangles.empty() && max_x > min_x && max_y > min_y;
    }
};

constexpr std::array<unsigned char, 3> default_thumbnail_color{42, 132, 210};

std::array<unsigned char, 3> parse_thumbnail_color(std::string value)
{
    if (!value.empty() && value.front() == '#') {
        value.erase(value.begin());
    }
    if (value.size() >= 6) {
        try {
            return {
                static_cast<unsigned char>(std::stoul(value.substr(0, 2), nullptr, 16)),
                static_cast<unsigned char>(std::stoul(value.substr(2, 2), nullptr, 16)),
                static_cast<unsigned char>(std::stoul(value.substr(4, 2), nullptr, 16)),
            };
        } catch (...) {
        }
    }
    return default_thumbnail_color;
}

std::vector<std::array<unsigned char, 3>> thumbnail_filament_colors(
    const Slic3r::DynamicPrintConfig& config)
{
    std::vector<std::array<unsigned char, 3>> result;
    if (const auto* colors = config.option<Slic3r::ConfigOptionStrings>("filament_colour");
        colors != nullptr) {
        result.reserve(colors->values.size());
        for (const std::string& color : colors->values) {
            result.push_back(parse_thumbnail_color(color));
        }
    }
    if (result.empty()) {
        result.push_back(default_thumbnail_color);
    }
    return result;
}

double thumbnail_edge(double ax, double ay, double bx, double by, double px, double py)
{
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

ProjectedThumbnailScene prepare_model_thumbnail_scene(
    const Slic3r::Model& model,
    const Slic3r::DynamicPrintConfig& config)
{
    ProjectedThumbnailScene output;

    // Keep the same camera and lighting as the existing headless thumbnail,
    // while using Orca's own facet expansion and filament-to-color mapping.
    const Slic3r::Vec3d camera_ray = Slic3r::Vec3d(-1.0, -1.0, -0.8).normalized();
    const Slic3r::Vec3d screen_right = camera_ray.cross(Slic3r::Vec3d::UnitZ()).normalized();
    const Slic3r::Vec3d screen_up = screen_right.cross(camera_ray).normalized();
    const Slic3r::Vec3d light_direction = Slic3r::Vec3d(-0.35, -0.45, 1.0).normalized();
    const auto filament_colors = thumbnail_filament_colors(config);
    const auto color_for_slot = [&filament_colors](int one_based_slot) {
        if (one_based_slot > 0 &&
            static_cast<std::size_t>(one_based_slot) <= filament_colors.size()) {
            return filament_colors[static_cast<std::size_t>(one_based_slot - 1)];
        }
        return filament_colors.front();
    };

    const auto append_mesh = [&](const indexed_triangle_set& mesh,
                                 const Slic3r::Transform3d& transform,
                                 const std::array<unsigned char, 3>& color) {
        for (const Slic3r::Vec3i32& indices : mesh.indices) {
            ProjectedThumbnailTriangle triangle;
            triangle.color = color;
            bool valid = true;
            for (int corner = 0; corner < 3; ++corner) {
                const int vertex_index = indices[corner];
                if (vertex_index < 0 ||
                    static_cast<std::size_t>(vertex_index) >= mesh.vertices.size()) {
                    valid = false;
                    break;
                }
                const Slic3r::Vec3d world =
                    transform * mesh.vertices[vertex_index].cast<double>();
                const Slic3r::Vec3d projected(world.dot(screen_right),
                                             world.dot(screen_up),
                                             world.dot(camera_ray));
                triangle.world[corner] = world;
                triangle.projected[corner] = projected;
                output.min_x = std::min(output.min_x, projected.x());
                output.min_y = std::min(output.min_y, projected.y());
                output.max_x = std::max(output.max_x, projected.x());
                output.max_y = std::max(output.max_y, projected.y());
            }
            if (!valid) {
                continue;
            }
            Slic3r::Vec3d normal = (triangle.world[1] - triangle.world[0])
                                       .cross(triangle.world[2] - triangle.world[0]);
            const double normal_length = normal.norm();
            if (normal_length <= std::numeric_limits<double>::epsilon()) {
                continue;
            }
            normal /= normal_length;
            if (normal.dot(-camera_ray) < 0.0) {
                normal = -normal;
            }
            triangle.shade = 0.38 + 0.62 * std::max(0.0, normal.dot(light_direction));
            output.triangles.push_back(std::move(triangle));
        }
    };

    for (const Slic3r::ModelObject* object : model.objects) {
        if (object == nullptr || !object->printable) {
            continue;
        }
        for (const Slic3r::ModelInstance* instance : object->instances) {
            if (instance == nullptr || !instance->printable) {
                continue;
            }
            for (const Slic3r::ModelVolume* volume : object->volumes) {
                if (volume == nullptr || !volume->is_model_part()) {
                    continue;
                }
                const Slic3r::Transform3d transform =
                    instance->get_matrix() * volume->get_matrix();
                if (volume->mmu_segmentation_facets.empty()) {
                    append_mesh(volume->mesh().its, transform,
                                color_for_slot(volume->extruder_id()));
                    continue;
                }

                std::vector<indexed_triangle_set> facets_per_color;
                volume->mmu_segmentation_facets.get_facets(*volume, facets_per_color);
                for (std::size_t color_index = 0;
                     color_index < facets_per_color.size(); ++color_index) {
                    const int filament_slot = color_index == 0
                        ? volume->extruder_id()
                        : static_cast<int>(color_index);
                    append_mesh(facets_per_color[color_index], transform,
                                color_for_slot(filament_slot));
                }
            }
        }
    }
    return output;
}

Slic3r::ThumbnailData render_model_thumbnail(const ProjectedThumbnailScene& scene,
                                             unsigned int width,
                                             unsigned int height,
                                             bool transparent_background)
{
    Slic3r::ThumbnailData output;
    if (width == 0 || height == 0) {
        return output;
    }

    if (!scene.valid()) {
        return output;
    }

    constexpr unsigned int sample_scale = 2;
    const unsigned int raster_width = width * sample_scale;
    const unsigned int raster_height = height * sample_scale;
    const double margin = std::max(2.0, 0.08 * static_cast<double>(std::min(raster_width, raster_height)));
    const double available_width = std::max(1.0, static_cast<double>(raster_width) - 2.0 * margin);
    const double available_height = std::max(1.0, static_cast<double>(raster_height) - 2.0 * margin);
    const double scale = std::min(available_width / (scene.max_x - scene.min_x),
                                  available_height / (scene.max_y - scene.min_y));
    const double offset_x =
        (static_cast<double>(raster_width) - (scene.max_x - scene.min_x) * scale) * 0.5;
    const double offset_y =
        (static_cast<double>(raster_height) - (scene.max_y - scene.min_y) * scale) * 0.5;

    const std::array<unsigned char, 4> background = transparent_background
        ? std::array<unsigned char, 4>{255, 255, 255, 0}
        : std::array<unsigned char, 4>{245, 247, 250, 255};
    std::vector<unsigned char> pixels(static_cast<std::size_t>(raster_width) * raster_height * 4);
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
        std::copy(background.begin(), background.end(), pixels.begin() + static_cast<std::ptrdiff_t>(index));
    }
    std::vector<double> depth(static_cast<std::size_t>(raster_width) * raster_height,
                              std::numeric_limits<double>::infinity());

    for (const ProjectedThumbnailTriangle& triangle : scene.triangles) {
        std::array<Slic3r::Vec3d, 3> screen;
        for (int corner = 0; corner < 3; ++corner) {
            screen[corner] = {
                offset_x + (triangle.projected[corner].x() - scene.min_x) * scale,
                offset_y + (triangle.projected[corner].y() - scene.min_y) * scale,
                triangle.projected[corner].z(),
            };
        }
        const double area = thumbnail_edge(screen[0].x(), screen[0].y(),
                                           screen[1].x(), screen[1].y(),
                                           screen[2].x(), screen[2].y());
        if (std::abs(area) <= std::numeric_limits<double>::epsilon()) {
            continue;
        }
        const int x_begin = std::max(0, static_cast<int>(std::floor(std::min({screen[0].x(), screen[1].x(), screen[2].x()}))));
        const int x_end = std::min(static_cast<int>(raster_width) - 1,
                                   static_cast<int>(std::ceil(std::max({screen[0].x(), screen[1].x(), screen[2].x()}))));
        const int y_begin = std::max(0, static_cast<int>(std::floor(std::min({screen[0].y(), screen[1].y(), screen[2].y()}))));
        const int y_end = std::min(static_cast<int>(raster_height) - 1,
                                   static_cast<int>(std::ceil(std::max({screen[0].y(), screen[1].y(), screen[2].y()}))));
        const std::array<unsigned char, 4> color = {
            static_cast<unsigned char>(std::clamp(triangle.shade * triangle.color[0], 0.0, 255.0)),
            static_cast<unsigned char>(std::clamp(triangle.shade * triangle.color[1], 0.0, 255.0)),
            static_cast<unsigned char>(std::clamp(triangle.shade * triangle.color[2], 0.0, 255.0)),
            255,
        };
        for (int y = y_begin; y <= y_end; ++y) {
            for (int x = x_begin; x <= x_end; ++x) {
                const double px = static_cast<double>(x) + 0.5;
                const double py = static_cast<double>(y) + 0.5;
                const double w0 = thumbnail_edge(screen[1].x(), screen[1].y(), screen[2].x(), screen[2].y(), px, py) / area;
                const double w1 = thumbnail_edge(screen[2].x(), screen[2].y(), screen[0].x(), screen[0].y(), px, py) / area;
                const double w2 = 1.0 - w0 - w1;
                if (w0 < -0.000001 || w1 < -0.000001 || w2 < -0.000001) {
                    continue;
                }
                const double value = w0 * screen[0].z() + w1 * screen[1].z() + w2 * screen[2].z();
                const std::size_t pixel_index = static_cast<std::size_t>(y) * raster_width + x;
                if (value >= depth[pixel_index]) {
                    continue;
                }
                depth[pixel_index] = value;
                std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(pixel_index * 4));
            }
        }
    }

    output.set(width, height);
    for (unsigned int y = 0; y < height; ++y) {
        for (unsigned int x = 0; x < width; ++x) {
            std::array<unsigned int, 4> sum{};
            for (unsigned int sample_y = 0; sample_y < sample_scale; ++sample_y) {
                for (unsigned int sample_x = 0; sample_x < sample_scale; ++sample_x) {
                    const std::size_t source =
                        (static_cast<std::size_t>(y * sample_scale + sample_y) * raster_width +
                         x * sample_scale + sample_x) * 4;
                    for (int component = 0; component < 4; ++component) {
                        sum[component] += pixels[source + component];
                    }
                }
            }
            const std::size_t target = (static_cast<std::size_t>(y) * width + x) * 4;
            for (int component = 0; component < 4; ++component) {
                output.pixels[target + component] = static_cast<unsigned char>(sum[component] / 4);
            }
        }
    }
    return output;
}

Slic3r::ThumbnailsList render_model_thumbnails(const ProjectedThumbnailScene& scene,
                                                const Slic3r::ThumbnailsParams& params)
{
    Slic3r::ThumbnailsList thumbnails;
    thumbnails.reserve(params.sizes.size());
    for (const Slic3r::Vec2d& size : params.sizes) {
        thumbnails.push_back(render_model_thumbnail(
            scene,
            static_cast<unsigned int>(std::max(0.0, std::round(size.x()))),
            static_cast<unsigned int>(std::max(0.0, std::round(size.y()))),
            params.transparent_background));
    }
    return thumbnails;
}

bool store_gcode_3mf(const std::string& output_path,
                     Slic3r::Model& model,
                     Slic3r::DynamicPrintConfig& config,
                     const std::string& gcode_path,
                     Slic3r::GCodeProcessorResult& processor_result,
                     const Slic3r::PrintStatistics& statistics,
                     bool support_used,
                     Slic3r::ThumbnailData plate_thumbnail)
{
    Slic3r::PlateData plate;
    plate.plate_index = 0;
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const auto* object = model.objects[object_index];
        for (std::size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            plate.objects_and_instances.emplace_back(static_cast<int>(object_index),
                                                     static_cast<int>(instance_index));
        }
    }
    plate.config.apply(config);
    plate.printer_model_id = config.opt_serialize("printer_model");
    plate.nozzle_diameters = config.opt_serialize("nozzle_diameter");
    plate.gcode_file = gcode_path;
    plate.gcode_prediction = Slic3r::get_time_dhms(
        static_cast<float>(processor_result.print_statistics.modes[static_cast<std::size_t>(
            Slic3r::PrintEstimatedStatistics::ETimeMode::Normal)].time));
    plate.gcode_weight = std::to_string(statistics.total_weight);
    plate.is_sliced_valid = true;
    plate.is_support_used = support_used;
    plate.toolpath_outside = processor_result.toolpath_outside;
    plate.is_label_object_enabled = processor_result.label_object_enabled;
    plate.timelapse_warning_code = processor_result.timelapse_warning_code;
    plate.filament_maps = processor_result.filament_maps;
    plate.limit_filament_maps = processor_result.limit_filament_maps;
    plate.layer_filaments = processor_result.layer_filaments;
    plate.filament_change_sequence = processor_result.filament_change_sequence;
    plate.nozzle_change_sequence = processor_result.nozzle_change_sequence;
    plate.optimal_assignment = processor_result.optimal_assignment;
    plate.parse_filament_info(&processor_result);
    plate.plate_thumbnail = std::move(plate_thumbnail);

    Slic3r::StoreParams params;
    params.path = output_path.c_str();
    params.model = &model;
    params.plate_data_list = {&plate};
    params.export_plate_idx = 0;
    params.config = &config;
    params.thumbnail_data = {&plate.plate_thumbnail};
    params.strategy = Slic3r::SaveStrategy::Silence |
                      Slic3r::SaveStrategy::SplitModel |
                      Slic3r::SaveStrategy::WithGcode |
                      Slic3r::SaveStrategy::SkipModel |
                      Slic3r::SaveStrategy::SkipAuxiliary |
                      Slic3r::SaveStrategy::Zip64;
    // The Orca exporter stages metadata beneath Model::get_backup_path() and
    // does not remove it. This model is private to the current slice, so its
    // exact staging directory can be released immediately after packaging.
    const fs::path staging_path = model.get_backup_path();
    const auto cleanup_staging = [&model, &staging_path] {
        std::error_code cleanup_error;
        try {
            model.remove_backup_path_if_exist();
        } catch (...) {
            fs::remove_all(staging_path, cleanup_error);
            model.set_backup_path("detach");
        }
        // Remove now-empty date/root folders without disturbing concurrent jobs.
        fs::remove(staging_path.parent_path(), cleanup_error);
        fs::remove(staging_path.parent_path().parent_path(), cleanup_error);
    };
    try {
        const bool stored = Slic3r::store_bbs_3mf(params);
        cleanup_staging();
        return stored;
    } catch (...) {
        cleanup_staging();
        throw;
    }
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
    const auto* configured_filament_colors = config == nullptr
        ? nullptr
        : config->option<Slic3r::ConfigOptionStrings>("filament_colour");
    for (std::size_t index = 0; index < filament_count; ++index) {
        ToolpathFilament filament;
        filament.id = static_cast<std::uint16_t>(index);
        filament.tool_id = static_cast<std::uint16_t>(tool_by_filament[index]);
        const bool has_configured_color = configured_filament_colors != nullptr &&
            index < configured_filament_colors->values.size() &&
            !configured_filament_colors->values[index].empty();
        filament.color = parse_color(has_configured_color
            ? configured_filament_colors->values[index]
            : index < source.extruder_colors.size()
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
    std::vector<std::unique_ptr<Slic3r::PresetBundle>> vendor_presets;
    std::vector<MachineModelOption> machines;
    std::vector<BuildPlateOption> build_plates;
    std::unique_ptr<Config> active_config;
    ResolvedSelection active_selection;
    std::vector<PresetOption> compatible_processes;
    std::vector<PresetOption> compatible_filaments;
    std::uint64_t active_revision{0};
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
        const fs::path working_directory = data_directory / "work";
        fs::create_directories(working_directory);
        Slic3r::set_temporary_dir(working_directory.string());

        const fs::path profiles_directory = fs::path(library->impl_->resource_directory) / "profiles";
        std::vector<std::string> vendors = options.vendors.empty() ?
                                               discover_vendors(profiles_directory) :
                                               options.vendors;
        if (vendors.empty()) {
            throw std::runtime_error("libslicer contains no vendor preset bundles");
        }

        constexpr const char* filament_library = "OrcaFilamentLibrary";
        const bool has_filament_library = fs::is_regular_file(profiles_directory / "OrcaFilamentLibrary.json");
        Slic3r::PresetBundle filament_presets;
        if (has_filament_library) {
            vendors.erase(std::remove(vendors.begin(), vendors.end(), filament_library), vendors.end());
            filament_presets.load_vendor_configs_from_json(
                profiles_directory.string(), filament_library, Slic3r::PresetBundle::LoadSystem,
                Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        }
        for (const std::string& vendor : vendors) {
            auto presets = std::make_unique<Slic3r::PresetBundle>();
            presets->load_vendor_configs_from_json(
                profiles_directory.string(), vendor, Slic3r::PresetBundle::LoadSystem,
                Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent,
                has_filament_library ? &filament_presets : nullptr);

            for (const auto& [vendor_id, vendor_profile] : presets->vendors) {
                for (const auto& model : vendor_profile.models) {
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
                    machine.bed_model_path   = resource_path(library->impl_->resource_directory,
                                                             vendor_id, model.bed_model);
                    machine.bed_texture_path = renderable_texture_path(library->impl_->resource_directory,
                                                                       vendor_id, model.bed_texture);

                    for (const auto& variant : model.variants) {
                        const auto* preset = presets->printers.find_system_preset_by_model_and_variant(
                            model.id, variant.name);
                        if (preset == nullptr) {
                            continue;
                        }
                        MachineVariantOption option;
                        option.id                = variant.name;
                        option.name              = variant.name + " mm";
                        option.nozzle_diameter   = nozzle_diameter(variant.name);
                        option.printer_preset_id = preset->name;
                        if (const auto* diameters =
                                preset->config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")) {
                            option.physical_tool_count = std::max<std::size_t>(1, diameters->values.size());
                        }
                        if (const auto* multi_material =
                                preset->config.option<Slic3r::ConfigOptionBool>("single_extruder_multi_material")) {
                            option.variable_filament_slots = multi_material->value;
                        }
                        // Current supported single-nozzle material systems expose
                        // four feed slots. Fixed multi-tool machines use exactly
                        // their physical tool count.
                        option.max_filament_slots = option.variable_filament_slots
                            ? 4
                            : option.physical_tool_count;
                        populate_printable_volume(preset->config, option);
                        machine.variants.push_back(std::move(option));
                    }
                    if (!machine.variants.empty()) {
                        library->impl_->machines.push_back(std::move(machine));
                    }
                }
            }
            library->impl_->vendor_presets.push_back(std::move(presets));
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
        const Slic3r::PresetBundle* source_presets = nullptr;
        for (const auto& presets : impl_->vendor_presets) {
            if (presets->printers.find_system_preset_by_model_and_variant(
                    selection.machine_model_id, selection.machine_variant_id) != nullptr) {
                source_presets = presets.get();
                break;
            }
        }
        if (source_presets == nullptr) {
            result.diagnostics.push_back({"machine", "Unknown machine model or nozzle variant"});
            return result;
        }
        auto selected = *source_presets;
        const Slic3r::Preset* printer = selected.printers.find_system_preset_by_model_and_variant(
            selection.machine_model_id, selection.machine_variant_id);

        std::string printer_name = printer->name;
        const bool same_active_machine =
            selection.machine_model_id == impl_->active_selection.machine_model_id &&
            selection.machine_variant_id == impl_->active_selection.machine_variant_id &&
            !impl_->active_selection.printer_preset_id.empty() &&
            selected.printers.find_preset(
                impl_->active_selection.printer_preset_id, false) != nullptr;
        if (same_active_machine) {
            // A project import may leave the selected preset edited with
            // custom values. Keep it while changing process or filament
            // choices for the same physical machine.
            printer_name = impl_->active_selection.printer_preset_id;
        }
        const bool keep_edited_printer = same_active_machine &&
            selected.printers.get_selected_preset_name() == printer_name;
        if (!keep_edited_printer) {
            selected.printers.select_preset_by_name(printer_name, true);
        }
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
        const bool keep_edited_process = keep_edited_printer &&
            process_name == impl_->active_selection.process_preset_id &&
            selected.prints.get_selected_preset_name() == process_name;
        if (!keep_edited_process) {
            selected.prints.select_preset_by_name(process_name, true);
        }
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
        const bool keep_edited_filament = keep_edited_process &&
            filament_names == impl_->active_selection.filament_preset_ids &&
            selected.filaments.get_selected_preset_name() == filament_names.front();
        if (!keep_edited_filament) {
            selected.filaments.select_preset_by_name(filament_names.front(), true);
        }
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

ConfigActivationResult Library::activate_config(
    const ConfigSelection& selection,
    const std::vector<std::pair<std::string, std::string>>& patch)
{
    ConfigActivationResult result;
    const std::vector<std::string> previous_colors = impl_->active_config
        ? config_vector_values(*impl_->active_config, "filament_colour")
        : std::vector<std::string>{};
    auto created = create_config(selection);
    if (!created) {
        result.diagnostics = std::move(created.diagnostics);
        return result;
    }

    const bool patch_supplies_colors = std::any_of(
        patch.begin(), patch.end(), [](const auto& entry) {
            return entry.first == "filament_colour";
        });
    if (!patch.empty()) {
        const SettingsResult patched = created.config->apply_patch(patch);
        if (!patched) {
            result.diagnostics = patched.diagnostics;
            return result;
        }
    }
    const SettingsResult normalized = normalize_filament_colors(
        *created.config, created.selection.filament_preset_ids.size(),
        patch_supplies_colors ? std::vector<std::string>{} : previous_colors);
    if (!normalized) {
        result.diagnostics = normalized.diagnostics;
        return result;
    }
    result.diagnostics = created.config->validate();
    auto cardinality = filament_cardinality_diagnostics(
        *created.config, created.selection.filament_preset_ids.size());
    result.diagnostics.insert(result.diagnostics.end(),
                              std::make_move_iterator(cardinality.begin()),
                              std::make_move_iterator(cardinality.end()));
    if (!result.diagnostics.empty()) {
        return result;
    }

    impl_->active_config = std::move(created.config);
    impl_->active_selection = std::move(created.selection);
    impl_->compatible_processes = std::move(created.compatible_processes);
    impl_->compatible_filaments = std::move(created.compatible_filaments);
    ++impl_->active_revision;
    result.success = true;
    result.view = *active_config();
    return result;
}

std::optional<ActiveConfigView> Library::active_config() const
{
    if (!impl_->active_config || impl_->active_revision == 0) {
        return std::nullopt;
    }
    ActiveConfigView view;
    view.revision = impl_->active_revision;
    view.selection = impl_->active_selection;
    view.compatible_processes = impl_->compatible_processes;
    view.compatible_filaments = impl_->compatible_filaments;
    view.settings = impl_->active_config->settings();

    const Slic3r::DynamicPrintConfig config = dynamic_config(impl_->active_config->snapshot());
    view.filament_slots.reserve(view.selection.filament_preset_ids.size());
    for (std::size_t index = 0; index < view.selection.filament_preset_ids.size(); ++index) {
        FilamentSlotInfo slot;
        slot.index = index;
        slot.preset_id = view.selection.filament_preset_ids[index];
        const auto preset = std::find_if(
            view.compatible_filaments.begin(), view.compatible_filaments.end(),
            [&slot](const PresetOption& option) { return option.id == slot.preset_id; });
        slot.preset_name = preset == view.compatible_filaments.end()
            ? slot.preset_id
            : preset->name;
        slot.vendor = option_value_at(config, "filament_vendor", index);
        slot.material_type = option_value_at(config, "filament_type", index);
        slot.color = rgba8_color(option_value_at(config, "filament_colour", index));
        if (const auto* diameters = config.option<Slic3r::ConfigOptionFloats>("filament_diameter");
            diameters != nullptr && index < diameters->values.size()) {
            slot.diameter_mm = diameters->values[index];
        }
        view.filament_slots.push_back(std::move(slot));
    }
    return view;
}

std::optional<ConfigSnapshot> Library::active_config_snapshot() const
{
    return impl_->active_config
        ? std::optional<ConfigSnapshot>{impl_->active_config->snapshot()}
        : std::nullopt;
}

SettingsResult Library::apply_active_config_patch(
    const std::vector<std::pair<std::string, std::string>>& patch)
{
    if (!impl_->active_config) {
        return settings_failure("configuration", "No slicing configuration is active");
    }
    Config candidate = *impl_->active_config;
    SettingsResult result = candidate.apply_patch(patch);
    if (!result) return result;
    SettingsResult normalized = normalize_filament_colors(
        candidate, impl_->active_selection.filament_preset_ids.size());
    if (!normalized) return normalized;
    append_changed_items(result.changed_items, std::move(normalized.changed_items));
    auto diagnostics = candidate.validate();
    auto cardinality = filament_cardinality_diagnostics(
        candidate, impl_->active_selection.filament_preset_ids.size());
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(cardinality.begin()),
                       std::make_move_iterator(cardinality.end()));
    if (!diagnostics.empty()) {
        result.success = false;
        result.diagnostics = std::move(diagnostics);
        result.changed_items.clear();
        return result;
    }
    impl_->active_config = std::make_unique<Config>(std::move(candidate));
    ++impl_->active_revision;
    return result;
}

SettingsResult Library::set_active_config_value(std::string_view key, std::string_view value)
{
    return apply_active_config_patch({{std::string(key), std::string(value)}});
}

SettingsResult Library::reset_active_config_value(std::string_view key)
{
    if (!impl_->active_config) {
        return settings_failure("configuration", "No slicing configuration is active");
    }
    Config candidate = *impl_->active_config;
    SettingsResult result = candidate.reset(key);
    if (!result) return result;
    SettingsResult normalized = normalize_filament_colors(
        candidate, impl_->active_selection.filament_preset_ids.size());
    if (!normalized) return normalized;
    append_changed_items(result.changed_items, std::move(normalized.changed_items));
    auto diagnostics = candidate.validate();
    auto cardinality = filament_cardinality_diagnostics(
        candidate, impl_->active_selection.filament_preset_ids.size());
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(cardinality.begin()),
                       std::make_move_iterator(cardinality.end()));
    if (!diagnostics.empty()) {
        result.success = false;
        result.diagnostics = std::move(diagnostics);
        result.changed_items.clear();
        return result;
    }
    impl_->active_config = std::make_unique<Config>(std::move(candidate));
    ++impl_->active_revision;
    return result;
}

ConfigActivationResult Library::set_active_filament_preset(
    std::size_t slot_index, std::string_view preset_id)
{
    if (!impl_->active_config) {
        ConfigActivationResult result;
        result.diagnostics.push_back({"configuration", "No slicing configuration is active"});
        return result;
    }
    if (slot_index >= impl_->active_selection.filament_preset_ids.size()) {
        ConfigActivationResult result;
        result.diagnostics.push_back({"filament", "Filament slot index is out of range"});
        return result;
    }
    if (impl_->active_selection.filament_preset_ids[slot_index] == preset_id) {
        ConfigActivationResult result;
        result.success = true;
        result.view = *active_config();
        return result;
    }
    ConfigSelection selection;
    selection.machine_model_id = impl_->active_selection.machine_model_id;
    selection.machine_variant_id = impl_->active_selection.machine_variant_id;
    selection.process_preset_id = impl_->active_selection.process_preset_id;
    selection.filament_preset_ids = impl_->active_selection.filament_preset_ids;
    selection.filament_preset_ids[slot_index] = std::string(preset_id);
    return activate_config(selection);
}

ConfigActivationResult Library::resize_active_filament_slots(std::size_t slot_count)
{
    ConfigActivationResult result;
    if (!impl_->active_config) {
        result.diagnostics.push_back({"configuration", "No slicing configuration is active"});
        return result;
    }
    const auto model = std::find_if(
        impl_->machines.begin(), impl_->machines.end(), [this](const MachineModelOption& option) {
            return option.id == impl_->active_selection.machine_model_id;
        });
    const MachineVariantOption* variant = nullptr;
    if (model != impl_->machines.end()) {
        const auto found = std::find_if(
            model->variants.begin(), model->variants.end(), [this](const MachineVariantOption& option) {
                return option.id == impl_->active_selection.machine_variant_id;
            });
        if (found != model->variants.end()) variant = &*found;
    }
    if (variant == nullptr) {
        result.diagnostics.push_back({"machine", "The active machine variant is unavailable"});
        return result;
    }
    if (slot_count == 0 || slot_count > variant->max_filament_slots) {
        result.diagnostics.push_back({
            "filament",
            "Filament slot count must be between 1 and " +
                std::to_string(variant->max_filament_slots)});
        return result;
    }
    if (!variant->variable_filament_slots &&
        slot_count != variant->physical_tool_count) {
        result.diagnostics.push_back({"filament", "This machine has a fixed filament slot count"});
        return result;
    }

    ConfigSelection selection;
    selection.machine_model_id = impl_->active_selection.machine_model_id;
    selection.machine_variant_id = impl_->active_selection.machine_variant_id;
    selection.process_preset_id = impl_->active_selection.process_preset_id;
    selection.filament_preset_ids = impl_->active_selection.filament_preset_ids;
    if (selection.filament_preset_ids.empty()) {
        result.diagnostics.push_back({"filament", "The active configuration has no filament preset"});
        return result;
    }
    selection.filament_preset_ids.resize(slot_count,
                                         selection.filament_preset_ids.back());
    return activate_config(selection);
}

SettingsResult Library::set_active_filament_color(std::size_t slot_index, Rgba8 color)
{
    if (!impl_->active_config) {
        return settings_failure("configuration", "No slicing configuration is active");
    }
    const std::size_t slot_count = impl_->active_selection.filament_preset_ids.size();
    if (slot_index >= slot_count) {
        return settings_failure("filament", "Filament slot index is out of range");
    }
    std::vector<std::string> colors = config_vector_values(*impl_->active_config, "filament_colour");
    if (colors.empty()) colors.push_back("#00AE42");
    colors.resize(slot_count, colors.back());
    colors[slot_index] = serialized_color(color);
    return set_active_config_value("filament_colour", serialize_strings(colors));
}

std::vector<ConfigDiagnostic> Library::validate_active_config() const
{
    if (!impl_->active_config) {
        return {{"configuration", "No slicing configuration is active"}};
    }
    auto diagnostics = impl_->active_config->validate();
    auto cardinality = filament_cardinality_diagnostics(
        *impl_->active_config, impl_->active_selection.filament_preset_ids.size());
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(cardinality.begin()),
                       std::make_move_iterator(cardinality.end()));
    return diagnostics;
}

SliceResult Library::slice(const SliceRequest& request, const SliceCallbacks& callbacks) const
{
    SliceResult result;
    std::vector<std::string> generated_temporary_paths;
    const auto discard_generated_temporary = [&generated_temporary_paths]() {
        for (const std::string& path : generated_temporary_paths) {
            std::error_code error;
            fs::remove(path, error);
        }
        generated_temporary_paths.clear();
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
        Slic3r::DynamicPrintConfig config = dynamic_config(request.config);
        normalize_filament_identity(config);
        const auto filament_diagnostics = slice_filament_diagnostics(config);
        if (!filament_diagnostics.empty()) {
            for (const auto& diagnostic : filament_diagnostics) {
                result.diagnostics.push_back({
                    "filament", diagnostic.key + ": " + diagnostic.message, false});
            }
            return result;
        }
        const std::size_t filament_slot_count =
            serialized_option_values(config, "filament_diameter").size();
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        report_progress(callbacks, 0.02f, "Loading models");
        Slic3r::Model plate_model;
        for (const SliceObjectInput& input : request.objects) {
            const bool has_file_input = !input.model_path.empty();
            const bool has_memory_input = !input.volumes.empty();
            if (has_file_input == has_memory_input) {
                result.diagnostics.push_back({
                    "model",
                    "Each slice object must provide exactly one of model_path or in-memory volumes",
                    false});
                return result;
            }
            Slic3r::ModelObject* support_target = nullptr;

            if (has_memory_input) {
                Slic3r::ModelObject* object = plate_model.add_object();
                object->name = input.name;
                for (const SliceVolumeInput& source_volume : input.volumes) {
                    if (source_volume.vertices.empty() || source_volume.triangles.empty()) {
                        result.diagnostics.push_back({"model", "In-memory slice volume has no geometry", false});
                        return result;
                    }
                    std::vector<Slic3r::Vec3f> vertices;
                    vertices.reserve(source_volume.vertices.size());
                    for (const auto& vertex : source_volume.vertices) {
                        vertices.emplace_back(vertex.x, vertex.y, vertex.z);
                    }
                    std::vector<Slic3r::Vec3i32> faces;
                    faces.reserve(source_volume.triangles.size());
                    for (const auto& triangle : source_volume.triangles) {
                        if (triangle.vertex_a >= vertices.size() ||
                            triangle.vertex_b >= vertices.size() ||
                            triangle.vertex_c >= vertices.size()) {
                            result.diagnostics.push_back({"model", "In-memory slice volume has an invalid triangle index", false});
                            return result;
                        }
                        faces.emplace_back(
                            static_cast<int>(triangle.vertex_a),
                            static_cast<int>(triangle.vertex_b),
                            static_cast<int>(triangle.vertex_c));
                    }

                    Slic3r::ModelVolumeType volume_type = Slic3r::ModelVolumeType::MODEL_PART;
                    if (source_volume.role == SliceVolumeRole::SupportEnforcer) {
                        volume_type = Slic3r::ModelVolumeType::SUPPORT_ENFORCER;
                    } else if (source_volume.role == SliceVolumeRole::SupportBlocker) {
                        volume_type = Slic3r::ModelVolumeType::SUPPORT_BLOCKER;
                    }
                    Slic3r::ModelVolume* volume = object->add_volume(
                        Slic3r::TriangleMesh(std::move(vertices), std::move(faces)),
                        volume_type, false);
                    if (source_volume.role == SliceVolumeRole::ModelPart) {
                        if (source_volume.default_filament_slot <= 0 ||
                            static_cast<std::size_t>(source_volume.default_filament_slot) >
                                filament_slot_count) {
                            result.diagnostics.push_back({"filament", "Model volume default filament slot is outside the active configuration", false});
                            return result;
                        }
                        volume->config.set("extruder", source_volume.default_filament_slot);
                    }
                    if (!source_volume.facet_labels.valid()) {
                        result.diagnostics.push_back({
                            "facet_labels",
                            "Facet painting roots and bitstream must either both be present or both be empty",
                            false});
                        return result;
                    }
                    if (!source_volume.facet_labels.empty()) {
                        Slic3r::TriangleSelector::TriangleSplittingData painting;
                        painting.triangles_to_split.reserve(source_volume.facet_labels.roots.size());
                        for (const auto& root : source_volume.facet_labels.roots) {
                            if (root.triangle_index >= source_volume.triangles.size() ||
                                root.bitstream_start_index >= source_volume.facet_labels.bitstream.size()) {
                                result.diagnostics.push_back({"facet_labels", "Facet painting references invalid triangle data", false});
                                return result;
                            }
                            painting.triangles_to_split.emplace_back(
                                static_cast<int>(root.triangle_index),
                                static_cast<int>(root.bitstream_start_index));
                        }
                        painting.bitstream.reserve(source_volume.facet_labels.bitstream.size());
                        for (const std::uint8_t bit : source_volume.facet_labels.bitstream) {
                            painting.bitstream.push_back(bit != 0u);
                        }
                        painting.update_used_states(0);
                        for (std::size_t label = filament_slot_count + 1;
                             label < painting.used_states.size(); ++label) {
                            if (painting.used_states[label]) {
                                result.diagnostics.push_back({"filament", "Facet painting references a missing filament slot", false});
                                return result;
                            }
                        }
                        volume->mmu_segmentation_facets.set_data(std::move(painting));
                    }
                }
                Slic3r::Transform3d transform = Slic3r::Transform3d::Identity();
                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        transform(row, column) = input.transform[static_cast<std::size_t>(row * 4 + column)];
                    }
                }
                object->add_instance()->set_transformation(
                    Slic3r::Geometry::Transformation(transform));
                support_target = object;
            } else {
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
                for (const Slic3r::ModelObject* object : source.objects) {
                    Slic3r::ModelObject* added = plate_model.add_object(*object);
                    if (support_target == nullptr) {
                        support_target = added;
                    }
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

        if (request.center_on_build_plate) {
            plate_model.center_instances_around_point(build_plate_center(config));
        }

        report_progress(callbacks, 0.08f, "Validating print");
        Slic3r::Print print;
        const auto* printer_model = config.option<Slic3r::ConfigOptionString>("printer_model");
        print.is_BBL_printer() = printer_model != nullptr &&
            printer_model->value.rfind("Bambu Lab", 0) == 0;
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
            result.diagnostics.push_back(
                {"validation", warning.string, true, warning.opt_key});
        }
        if (!validation_error.string.empty()) {
            result.diagnostics.push_back(
                {"validation", validation_error.string, false,
                 validation_error.opt_key});
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
        std::string output_path = library_temporary ? temporary_output_path(".gcode") : request.output_gcode_path;
        if (library_temporary) {
            generated_temporary_paths.push_back(output_path);
        }
        const fs::path output_parent = fs::path(output_path).parent_path();
        if (!output_parent.empty()) {
            fs::create_directories(output_parent);
        }

        report_progress(callbacks, 0.88f, "Exporting G-code");
        Slic3r::GCodeProcessorResult processor_result;
        const ProjectedThumbnailScene thumbnail_scene =
            prepare_model_thumbnail_scene(plate_model, config);
        const auto thumbnail_callback = [&thumbnail_scene](const Slic3r::ThumbnailsParams& params) {
            return render_model_thumbnails(thumbnail_scene, params);
        };
        result.output.path = print.export_gcode(output_path, &processor_result, thumbnail_callback);
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

        const bool generate_gcode_3mf = request.generate_gcode_3mf || !request.output_gcode_3mf_path.empty();
        if (generate_gcode_3mf) {
            const bool package_temporary = request.output_gcode_3mf_path.empty();
            const std::string package_path = package_temporary
                ? temporary_output_path(".gcode.3mf")
                : request.output_gcode_3mf_path;
            if (package_temporary) {
                generated_temporary_paths.push_back(package_path);
            }
            const fs::path package_parent = fs::path(package_path).parent_path();
            if (!package_parent.empty()) {
                fs::create_directories(package_parent);
            }

            report_progress(callbacks, 0.94f, "Packaging sliced G-code 3MF");
            Slic3r::ThumbnailData plate_thumbnail =
                render_model_thumbnail(thumbnail_scene, 256, 256, false);
            if (!plate_thumbnail.is_valid()) {
                result.diagnostics.push_back({"gcode_3mf", "Slicer could not render the sliced G-code 3MF thumbnail", false});
                discard_generated_temporary();
                result.output.path.clear();
                return result;
            }
            if (!store_gcode_3mf(package_path, plate_model, config, result.output.path,
                                 processor_result, print.print_statistics(),
                                 print.has_support_material(), std::move(plate_thumbnail))) {
                result.diagnostics.push_back({"gcode_3mf", "Slicer could not package the sliced G-code 3MF", false});
                discard_generated_temporary();
                result.output.path.clear();
                return result;
            }
            std::error_code package_error;
            if (!fs::is_regular_file(package_path, package_error) || package_error ||
                fs::file_size(package_path, package_error) == 0 || package_error) {
                result.diagnostics.push_back({"gcode_3mf", "Slicer generated an invalid sliced G-code 3MF", false});
                discard_generated_temporary();
                result.output.path.clear();
                return result;
            }
            std::string validation_message;
            if (!validate_gcode_3mf(package_path, validation_message)) {
                result.diagnostics.push_back({"gcode_3mf", "Slicer generated an invalid sliced G-code 3MF: " + validation_message, false});
                discard_generated_temporary();
                result.output.path.clear();
                return result;
            }
            result.gcode_3mf.path = package_path;
            result.gcode_3mf.ownership = package_temporary
                ? OutputArtifactOwnership::LibraryTemporary
                : OutputArtifactOwnership::CallerOwned;
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
                result.gcode_3mf.path.clear();
                result.preview.reset();
                return result;
            }
            result.summary.logical_motion_count =
                result.preview->statistics.logical_motion_count;
            result.summary.render_segment_count = result.preview->segments.size();
        }

        result.success = true;
        generated_temporary_paths.clear();
        report_progress(callbacks, 1.0f, "Slicing completed");
    } catch (const Slic3r::CanceledException&) {
        result.cancelled = true;
        discard_generated_temporary();
        result.output.path.clear();
        result.gcode_3mf.path.clear();
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"slice", error.what(), false});
        discard_generated_temporary();
        result.output.path.clear();
        result.gcode_3mf.path.clear();
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

ProjectImportResult Library::import_project(const ProjectImportRequest& request,
                                            const SliceCallbacks& callbacks) const
{
    ProjectImportResult result;
    Slic3r::PlateDataPtrs plate_data;
    std::vector<Slic3r::Preset*> project_presets;
    const auto release_import_resources = [&]() {
        Slic3r::release_PlateData_list(plate_data);
        for (Slic3r::Preset* preset : project_presets) {
            if (preset != nullptr) {
                delete preset->loading_substitutions;
                preset->loading_substitutions = nullptr;
                delete preset;
            }
        }
        project_presets.clear();
    };

    try {
        std::error_code file_error;
        if (request.path.empty() ||
            !fs::is_regular_file(request.path, file_error) || file_error) {
            result.diagnostics.push_back(
                {"input", "3MF project does not exist: " + request.path, false});
            release_import_resources();
            return result;
        }

        std::string lower_path = request.path;
        std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (lower_path.size() < 4 || lower_path.substr(lower_path.size() - 4) != ".3mf" ||
            (lower_path.size() >= 10 &&
             lower_path.substr(lower_path.size() - 10) == ".gcode.3mf")) {
            result.diagnostics.push_back(
                {"input", "Project import currently accepts model .3mf files only", false});
            release_import_resources();
            return result;
        }
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            release_import_resources();
            return result;
        }

        report_progress(callbacks, 0.02f, "Opening 3MF project");
        Slic3r::DynamicPrintConfig config;
        Slic3r::ConfigSubstitutionContext substitutions(
            Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
        Slic3r::En3mfType file_type = Slic3r::En3mfType::From_Other;
        Slic3r::Semver file_version;
        Slic3r::LoadStrategy strategy =
            Slic3r::LoadStrategy::LoadModel |
            Slic3r::LoadStrategy::LoadConfig |
            Slic3r::LoadStrategy::CheckVersion |
            Slic3r::LoadStrategy::AddDefaultInstances |
            Slic3r::LoadStrategy::Silence;
        const Slic3r::Import3mfProgressFn progress =
            [&callbacks](int stage, int current, int total, bool& cancel) {
                cancel = cancellation_requested(callbacks);
                if (cancel) return;
                const float stage_fraction = total > 0
                    ? std::clamp(static_cast<float>(current) / static_cast<float>(total),
                                 0.0f, 1.0f)
                    : 0.0f;
                const float progress_value = std::clamp(
                    (static_cast<float>(stage) + stage_fraction) /
                        static_cast<float>(Slic3r::IMPORT_STAGE_MAX),
                    0.02f, 0.72f);
                report_progress(callbacks, progress_value, "Reading 3MF project");
            };

        Slic3r::Model model = Slic3r::Model::read_from_archive(
            request.path, &config, &substitutions, file_type, strategy,
            &plate_data, &project_presets, &file_version, progress);
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            release_import_resources();
            return result;
        }

        report_progress(callbacks, 0.75f, "Converting model geometry");
        std::size_t maximum_filament_id = 0;
        for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
            const Slic3r::ModelObject* object = model.objects[object_index];
            if (object == nullptr) continue;
            for (std::size_t volume_index = 0; volume_index < object->volumes.size(); ++volume_index) {
                const Slic3r::ModelVolume* volume = object->volumes[volume_index];
                if (volume == nullptr || !volume->is_model_part() || volume->mesh().empty()) {
                    continue;
                }

                ProjectImportMesh mesh;
                mesh.id = "object-" + std::to_string(object_index + 1) +
                    "/mesh-" + std::to_string(volume_index + 1);
                mesh.name = !volume->name.empty() ? volume->name
                    : !object->name.empty() ? object->name : mesh.id;
                const int extruder_id = volume->extruder_id();
                if (extruder_id > 0) {
                    mesh.filament_id = "filament-" + std::to_string(extruder_id);
                    maximum_filament_id = std::max(
                        maximum_filament_id, static_cast<std::size_t>(extruder_id));
                }

                const auto& indexed = volume->mesh().its;
                mesh.vertices.reserve(indexed.vertices.size());
                for (const auto& vertex : indexed.vertices) {
                    mesh.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
                }
                mesh.triangles.reserve(indexed.indices.size());
                for (const auto& triangle : indexed.indices) {
                    mesh.triangles.push_back({
                        static_cast<std::uint32_t>(triangle.x()),
                        static_cast<std::uint32_t>(triangle.y()),
                        static_cast<std::uint32_t>(triangle.z())});
                }

                const auto& facet_labels = volume->mmu_segmentation_facets.get_data();
                mesh.facet_labels.roots.reserve(facet_labels.triangles_to_split.size());
                for (const auto& root : facet_labels.triangles_to_split) {
                    if (root.triangle_idx < 0 || root.bitstream_start_idx < 0) continue;
                    mesh.facet_labels.roots.push_back({
                        static_cast<std::uint32_t>(root.triangle_idx),
                        static_cast<std::uint32_t>(root.bitstream_start_idx)});
                }
                mesh.facet_labels.bitstream.reserve(facet_labels.bitstream.size());
                for (const bool bit : facet_labels.bitstream) {
                    mesh.facet_labels.bitstream.push_back(bit ? 1u : 0u);
                }

                const auto append_instance = [&](const Slic3r::Transform3d& instance_matrix,
                                                 bool printable,
                                                 std::size_t instance_index) {
                    ProjectImportInstance instance;
                    instance.mesh_id = mesh.id;
                    instance.name = !object->name.empty() ? object->name : mesh.name;
                    if (object->instances.size() > 1) {
                        instance.name += " " + std::to_string(instance_index + 1);
                    }
                    const Slic3r::Transform3d combined = instance_matrix * volume->get_matrix();
                    for (int row = 0; row < 4; ++row) {
                        for (int column = 0; column < 4; ++column) {
                            instance.transform[static_cast<std::size_t>(row * 4 + column)] =
                                combined(row, column);
                        }
                    }
                    instance.printable = object->printable && printable;
                    result.instances.push_back(std::move(instance));
                };

                if (object->instances.empty()) {
                    append_instance(Slic3r::Transform3d::Identity(), true, 0);
                } else {
                    for (std::size_t instance_index = 0;
                         instance_index < object->instances.size(); ++instance_index) {
                        const Slic3r::ModelInstance* instance = object->instances[instance_index];
                        if (instance != nullptr) {
                            append_instance(instance->get_matrix(), instance->printable,
                                            instance_index);
                        }
                    }
                }
                result.meshes.push_back(std::move(mesh));
            }
        }

        if (result.meshes.empty() || result.instances.empty()) {
            result.diagnostics.push_back(
                {"model", "3MF project contains no printable model geometry", false});
            release_import_resources();
            return result;
        }

        report_progress(callbacks, 0.88f, "Converting project settings");
        result.filaments = project_import_filaments(
            config, maximum_filament_id);
        for (const std::string& key : substitutions.unrecogized_keys) {
            result.diagnostics.push_back(
                {"config", "Unrecognized 3MF project setting: " + key, true});
        }

        // Match OrcaSlicer's project loading semantics: the resolved config
        // stored in the 3MF is authoritative. Installed preset names are used
        // when their content matches; otherwise PresetBundle creates temporary
        // project presets from the resolved config and selects them.
        if (!config.empty()) {
            report_progress(callbacks, 0.93f, "Activating 3MF project settings");
            const std::string printer_model = option_value_at(config, "printer_model", 0);
            const std::string printer_variant = option_value_at(config, "printer_variant", 0);
            const std::string printer_preset = option_value_at(config, "printer_settings_id", 0);
            const std::vector<std::string> inherited_presets =
                serialized_option_values(config, "inherits_group");

            const MachineModelOption* machine = nullptr;
            const MachineVariantOption* variant = nullptr;
            const auto select_machine = [&](const auto& candidate_machine,
                                            const auto& candidate_variant) {
                machine = &candidate_machine;
                variant = &candidate_variant;
            };
            for (const auto& candidate_machine : impl_->machines) {
                for (const auto& candidate_variant : candidate_machine.variants) {
                    const bool identity_match =
                        candidate_machine.id == printer_model &&
                        candidate_variant.id == printer_variant;
                    const bool preset_match =
                        candidate_variant.printer_preset_id == printer_preset ||
                        std::find(inherited_presets.begin(), inherited_presets.end(),
                                  candidate_variant.printer_preset_id) != inherited_presets.end();
                    if (identity_match || preset_match) {
                        select_machine(candidate_machine, candidate_variant);
                        break;
                    }
                }
                if (machine != nullptr) break;
            }

            Slic3r::PresetBundle* preset_bundle = nullptr;
            if (machine != nullptr && variant != nullptr) {
                for (const auto& candidate : impl_->vendor_presets) {
                    if (candidate->printers.find_system_preset_by_model_and_variant(
                            machine->id, variant->id) != nullptr) {
                        preset_bundle = candidate.get();
                        break;
                    }
                }
            }

            if (preset_bundle == nullptr) {
                result.diagnostics.push_back({
                    "config",
                    "3MF project settings reference an unavailable printer model; geometry was loaded without changing the active configuration",
                    true});
            } else {
                try {
                    Slic3r::DynamicPrintConfig resolved_config;
                    resolved_config.apply(Slic3r::FullPrintConfig::defaults());
                    resolved_config.apply(config);
                    Slic3r::Preset::normalize(resolved_config);

                    if (!project_presets.empty()) {
                        preset_bundle->load_project_embedded_presets(
                            project_presets,
                            Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
                    }

                    preset_bundle->load_config_model(
                        request.path, std::move(resolved_config), file_version);

                    auto active = std::unique_ptr<Config>(
                        new Config(serialized_values(preset_bundle->full_config())));
                    for (const auto& diagnostic : active->validate()) {
                        result.diagnostics.push_back({
                            diagnostic.key, diagnostic.message, true});
                    }

                    impl_->active_config = std::move(active);
                    impl_->active_selection.machine_model_id = machine->id;
                    impl_->active_selection.machine_variant_id = variant->id;
                    impl_->active_selection.printer_preset_id =
                        preset_bundle->printers.get_selected_preset_name();
                    impl_->active_selection.process_preset_id =
                        preset_bundle->prints.get_selected_preset_name();
                    impl_->active_selection.filament_preset_ids =
                        preset_bundle->filament_presets;
                    impl_->compatible_processes = compatible_presets(
                        preset_bundle->prints,
                        impl_->active_selection.process_preset_id);
                    impl_->compatible_filaments = compatible_presets(
                        preset_bundle->filaments,
                        impl_->active_selection.filament_preset_ids.empty()
                            ? std::string{}
                            : impl_->active_selection.filament_preset_ids.front());
                    ++impl_->active_revision;
                } catch (const std::exception& error) {
                    result.diagnostics.push_back({
                        "config",
                        std::string("Unable to activate 3MF project settings: ") + error.what(),
                        true});
                }
            }
        }

        result.success = true;
        release_import_resources();
        report_progress(callbacks, 1.0f, "3MF project ready");
    } catch (const Slic3r::CanceledException&) {
        result.cancelled = true;
        release_import_resources();
    } catch (const std::exception& error) {
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
        } else {
            result.diagnostics.push_back({"3mf", error.what(), false});
        }
        release_import_resources();
    }
    return result;
}

} // namespace libslicer
