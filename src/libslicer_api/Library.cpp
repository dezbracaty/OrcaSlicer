#include <nanosvg/nanosvg.h>

#include <libslicer/Library.hpp>
#include "OrcaCalibrationInternal.hpp"

#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/AppConfig.hpp>
#include <libslic3r/ParameterUtils.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/BuildVolume.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/Layer.hpp>
#include <libslic3r/Tesselate.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/ContinuousFiber/ContinuousFiberConfig.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libslic3r/GCode/ThumbnailData.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/PNGReadWrite.hpp>
#include <libslic3r/Support/BeltSupportDebug.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>
#include <Eigen/SVD>
#include <libslic3r/miniz_extension.hpp>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
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

Slic3r::FullPrintConfig tool_config(const Slic3r::DynamicPrintConfig& config)
{
    Slic3r::FullPrintConfig tools;
    tools.apply(config, true);
    return tools;
}

std::string preset_process(const Slic3r::Preset& preset)
{
    const auto* types = preset.config.option<Slic3r::ConfigOptionStrings>("filament_process_type");
    return types && !types->values.empty() ? types->values.front() : "thermoplastic";
}

bool tool_owned_setting(std::string_view key)
{
    return key == "filament_process_type" || key == "filament_map" || key == "physical_extruder_map" ||
        key == "toolhead_process_capabilities" || key == "toolhead_filament_capacity" ||
        key == "toolhead_fiber_protocol_id" || key == "filament_slots_bound_to_physical_tools";
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

Slic3r::Transform3d mask_matrix(const std::array<double, 16>& values)
{
    Slic3r::Transform3d matrix = Slic3r::Transform3d::Identity();
    for (int row = 0; row != 4; ++row)
        for (int column = 0; column != 4; ++column)
            matrix(row, column) = values[std::size_t(row * 4 + column)];
    if (!matrix.matrix().allFinite() ||
        !matrix.matrix().row(3).isApprox(Eigen::RowVector4d(0, 0, 0, 1), 1e-7))
        throw std::invalid_argument("Transform must be a finite affine matrix");
    return matrix;
}

void validate_mask_geometry(const SliceFiberMaskInput& mask)
{
    if (mask.id.empty()) throw std::invalid_argument("Mask ID must not be empty");
    for (double size : mask.dimensions_mm)
        if (!std::isfinite(size) || size <= 0 || size > 1000000.0)
            throw std::invalid_argument("Mask dimensions must be positive finite millimeters within coordinate limits");
    const auto pose = mask_matrix(mask.mask_to_object);
    const Eigen::Matrix3d rotation = pose.linear();
    if (!(rotation.transpose() * rotation).isApprox(Eigen::Matrix3d::Identity(), 1e-5) ||
        std::abs(rotation.determinant() - 1.0) > 1e-5)
        throw std::invalid_argument("Mask pose must be rigid; dimensions carry the scale");
    const auto equal_size = [&](int a, int b) {
        return std::abs(mask.dimensions_mm[a] - mask.dimensions_mm[b]) <=
            1e-6 * std::max(mask.dimensions_mm[a], mask.dimensions_mm[b]);
    };
    switch (mask.shape) {
    case FiberMaskShape::Box: break;
    case FiberMaskShape::Cylinder:
        if (!equal_size(0, 1)) throw std::invalid_argument("Cylinder XY diameters must match");
        break;
    case FiberMaskShape::Sphere:
        if (!equal_size(0, 1) || !equal_size(0, 2))
            throw std::invalid_argument("Sphere XYZ diameters must match");
        break;
    default: throw std::invalid_argument("Unknown mask shape");
    }
}

Slic3r::DynamicPrintConfig mask_process_patch(const SliceFiberMaskInput& mask,
    const Slic3r::DynamicPrintConfig& active)
{
    Config validator = Config::defaults();
    const auto checked = validator.apply_patch(ConfigScope::FiberMask, mask.settings);
    if (!checked) {
        const auto& error = checked.diagnostics.front();
        throw std::invalid_argument(error.key + ": " + error.message);
    }
    const auto normalized = validator.snapshot();
    const auto tools = tool_config(active);
    Slic3r::DynamicPrintConfig patch;
    for (const auto& [key, value] : mask.settings) {
        patch.set_deserialize_strict(key, normalized.value(key).value());
        if (key == "reinforced_perimeters_filament" || key == "reinforced_infill_filament") {
            const int material = patch.opt_int(key);
            if (material <= 0 || std::size_t(material) > tools.filament_diameter.size())
                throw std::invalid_argument(key + ": material is outside the configured slots");
        }
    }
    return patch;
}

Slic3r::TriangleMesh make_mask_mesh(const SliceFiberMaskInput& mask,
    const Slic3r::Transform3d& parent, double resolution)
{
    const Eigen::Matrix3d linear = parent.linear();
    const auto singular = Eigen::JacobiSVD<Eigen::Matrix3d>(linear).singularValues();
    if (!singular.allFinite() || singular.minCoeff() <= 1e-12)
        throw std::invalid_argument("Mask parent transform must be invertible");
    const double scale = singular.maxCoeff();
    if (*std::max_element(mask.dimensions_mm.begin(), mask.dimensions_mm.end()) * scale > 1000000.0)
        throw std::invalid_argument("Transformed mask exceeds coordinate limits");
    double angle = 2.0 * PI / 32.0;
    if (mask.shape != FiberMaskShape::Box) {
        // The sphere has two angular directions. A factor of four bounds both
        // directions conservatively; the cylinder needs only the radial one.
        const double radius = 0.5 * mask.dimensions_mm[0] * scale;
        const double tolerance = std::max(1e-5, resolution);
        angle = std::min(angle, 2.0 * std::acos(std::clamp(
            1.0 - tolerance / (4.0 * radius), -1.0, 1.0)));
        if (!std::isfinite(angle) || angle < 2.0 * PI / 1024.0)
            throw std::invalid_argument("Mask tessellation exceeds the angular resource limit");
    }
    Slic3r::TriangleMesh mesh;
    switch (mask.shape) {
    case FiberMaskShape::Box:
        mesh = Slic3r::make_cube(mask.dimensions_mm[0], mask.dimensions_mm[1], mask.dimensions_mm[2]);
        break;
    case FiberMaskShape::Cylinder:
        mesh = Slic3r::make_cylinder(0.5 * mask.dimensions_mm[0], mask.dimensions_mm[2], angle);
        break;
    case FiberMaskShape::Sphere:
        mesh = Slic3r::make_sphere(0.5 * mask.dimensions_mm[0], angle);
        break;
    }
    if (mesh.its.indices.size() > 1000000)
        throw std::invalid_argument("Mask tessellation exceeds the triangle resource limit");
    // Primitive origins are known analytically. A tessellated circle with an
    // odd sector count has an asymmetric bounding box; recentering that box
    // would move the requested cylinder/sphere and violate its true boundary.
    if (mask.shape == FiberMaskShape::Box)
        mesh.translate(Slic3r::Vec3f(-0.5 * mask.dimensions_mm[0], -0.5 * mask.dimensions_mm[1],
                                   -0.5 * mask.dimensions_mm[2]));
    else if (mask.shape == FiberMaskShape::Cylinder)
        mesh.translate(Slic3r::Vec3f(0, 0, -0.5 * mask.dimensions_mm[2]));
    const auto world = parent * mask_matrix(mask.mask_to_object);
    for (const auto& vertex : mesh.its.vertices)
        if ((world * vertex.cast<double>()).cwiseAbs().maxCoeff() > 1000000.0)
            throw std::invalid_argument("Mask position exceeds coordinate limits");
    return mesh;
}

void validate_effective_mask_regions(Slic3r::Print& print, const std::set<size_t>& mask_volume_ids)
{
    for (auto* object : print.objects()) {
        const auto* regions = object->shared_regions();
        if (!regions) continue;
        std::set<const Slic3r::PrintRegion*> checked;
        for (const auto& range : regions->layer_ranges)
            for (const auto& entry : range.volume_regions) {
                if (!entry.region || !entry.model_volume ||
                    mask_volume_ids.count(entry.model_volume->id().id) == 0) continue;
                const auto* root = &entry;
                while (root->parent >= 0)
                    root = &range.volume_regions.at(size_t(root->parent));
                if (!root->region || !root->model_volume->is_model_part()) continue;
                // Native modifier merging has already applied all overlapping patches.
                // A restored or identical projection needs no mask-specific process validation.
                const auto& keys = Slic3r::fiber_mask_parameter_keys();
                const bool same = std::all_of(keys.begin(), keys.end(), [&](std::string_view key) {
                    return entry.region->config().option(std::string(key))->serialize() ==
                           root->region->config().option(std::string(key))->serialize();
                });
                if (same || !checked.insert(entry.region).second) continue;
                const auto& config = entry.region->config();
                const std::string owner = "object=" + object->model_object()->name +
                    " mask=" + entry.model_volume->name + ": ";
                try {
                    if (config.infill_combination.value || object->config().interface_shells.value ||
                        Slic3r::PrintObject::infill_only_where_needed)
                        throw std::invalid_argument("Masks do not support infill_combination, interface_shells or infill_only_where_needed");
                    Slic3r::resolve_resin_fill_config(config);
                    if (config.fiber_resin_fill_density.value > 0) {
                        const int resin = config.sparse_infill_filament_id.value;
                        if (resin <= 0 || std::size_t(resin) > print.config().filament_diameter.size() ||
                            Slic3r::is_fiber_filament(print.config(), unsigned(resin - 1)))
                            throw std::invalid_argument("Residual resin material is not a valid thermoplastic slot");
                    }
                    if (Slic3r::continuous_fiber_enabled(config)) {
                        Slic3r::validate_continuous_fiber_region_config(
                            print.config(), config, object->config().layer_height.value);
                        const bool concentric = Slic3r::fiber_concentric_uses_contour_process(config);
                        if (config.generate_reinforced_perimeters.value || concentric)
                            Slic3r::resolve_fiber_tool(print.config(), unsigned(config.reinforced_perimeters_filament.value - 1));
                        if (config.generate_reinforced_infills.value && !concentric)
                            Slic3r::resolve_fiber_tool(print.config(), unsigned(config.reinforced_infill_filament.value - 1));
                    }
                } catch (const std::exception& error) {
                    throw std::invalid_argument(owner + error.what());
                }
            }
    }
}

std::vector<std::string> config_vector_values(const Config& config, const std::string& key)
{
    return serialized_option_values(dynamic_config(config.snapshot()), key);
}

bool fixed_physical_filament_slots(const Slic3r::DynamicPrintConfig& config)
{
    const auto* fixed = config.option<Slic3r::ConfigOptionBool>(
        "filament_slots_bound_to_physical_tools");
    return fixed != nullptr && fixed->value;
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

struct ModelWorldBounds {
    Slic3r::Vec3d minimum{Slic3r::Vec3d::Constant(std::numeric_limits<double>::max())};
    Slic3r::Vec3d maximum{Slic3r::Vec3d::Constant(std::numeric_limits<double>::lowest())};
    bool valid{false};
};

bool is_belt_config(const Slic3r::DynamicPrintConfig& config)
{
    const auto* structure = config.option<Slic3r::ConfigOptionEnum<Slic3r::PrinterStructure>>(
        "printer_structure");
    return structure != nullptr && structure->value == Slic3r::PrinterStructure::psBelt;
}

ModelWorldBounds model_world_bounds(const Slic3r::Model& model)
{
    ModelWorldBounds bounds;
    for (const Slic3r::ModelObject* object : model.objects) {
        const std::size_t instance_count = std::max<std::size_t>(1, object->instances.size());
        for (std::size_t instance_index = 0; instance_index < instance_count; ++instance_index) {
            const Slic3r::Transform3d instance = object->instances.empty()
                ? Slic3r::Transform3d::Identity()
                : object->instances[instance_index]->get_matrix();
            for (const Slic3r::ModelVolume* volume : object->volumes) {
                if (!volume->is_model_part())
                    continue;
                const Slic3r::Transform3d local_to_world = instance * volume->get_matrix();
                for (const Slic3r::Vec3f& vertex : volume->mesh().its.vertices) {
                    const Slic3r::Vec3d world = local_to_world * vertex.cast<double>();
                    bounds.minimum = bounds.minimum.cwiseMin(world);
                    bounds.maximum = bounds.maximum.cwiseMax(world);
                    bounds.valid = true;
                }
            }
        }
    }
    return bounds;
}

void center_belt_model_on_x(Slic3r::Model& model, double target_x)
{
    const ModelWorldBounds bounds = model_world_bounds(model);
    if (!bounds.valid)
        return;
    const double delta_x = target_x - 0.5 * (bounds.minimum.x() + bounds.maximum.x());
    for (Slic3r::ModelObject* object : model.objects) {
        if (object->instances.empty())
            object->add_instance();
        for (Slic3r::ModelInstance* instance : object->instances)
            instance->set_offset(Slic3r::X, instance->get_offset().x() + delta_x);
        object->invalidate_bounding_box();
    }
}

std::string temporary_output_path(std::string_view suffix)
{
    static std::atomic<unsigned long long> sequence{0};
    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() /
            ("libslicer_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + std::string(suffix))).string();
}

nlohmann::json belt_support_debug_json(
    const Slic3r::BeltSupportDebugRecorder& recorder,
    bool include_lines,
    bool include_records,
    const std::string& output_path)
{
    nlohmann::json root;
    root["schema"] = "gplatform.belt_support_algorithm_audit.v1";
    const Slic3r::BeltSupportDebugStage& final_stage =
        recorder.stage(Slic3r::BeltSupportDebugStageId::FinalSupport);
    const auto generated_it = final_stage.metrics.find(
        "final_support_generated");
    const auto contract_it = final_stage.metrics.find(
        "final_support_contract_valid");
    root["final_support_generated"] = generated_it != final_stage.metrics.end() &&
        generated_it->second > 0.5;
    root["final_support_contract_valid"] =
        contract_it != final_stage.metrics.end() &&
        contract_it->second > 0.5;
    root["output_path"] = output_path;
    root["stages"] = nlohmann::json::array();
    for (const Slic3r::BeltSupportDebugStage& stage : recorder.stages()) {
        nlohmann::json item;
        item["id"] = stage.id;
        item["name"] = stage.name;
        item["purpose"] = stage.purpose;
        item["expected"] = stage.expected;
        item["elapsed_ms"] = stage.elapsed_ms;
        item["line_count"] = stage.lines.size();
        item["record_count"] = stage.records.size();
        item["metrics"] = stage.metrics;
        if (include_records) {
            item["records"] = nlohmann::json::array();
            for (const Slic3r::BeltSupportDebugRecord& record : stage.records) {
                item["records"].push_back({
                    {"kind", record.kind},
                    {"reason", record.reason},
                    {"values", record.values},
                });
            }
        }
        if (include_lines) {
            item["lines"] = nlohmann::json::array();
            for (const Slic3r::BeltSupportDebugLine& line : stage.lines) {
                item["lines"].push_back({
                    {"category", line.category},
                    {"start", {line.start_world.x(), line.start_world.y(), line.start_world.z()}},
                    {"end", {line.end_world.x(), line.end_world.y(), line.end_world.z()}},
                });
            }
        }
        root["stages"].push_back(std::move(item));
    }
    return root;
}

ToolpathColorValue belt_support_debug_color(std::string_view category)
{
    if (category == "model_slice") return {0.45f, 0.48f, 0.52f, 0.42f};
    if (category == "overhang") return {1.0f, 0.45f, 0.05f, 1.0f};
    if (category == "raw_contact") return {1.0f, 0.15f, 0.15f, 1.0f};
    if (category == "contact_safe_region") return {1.0f, 0.4f, 0.15f, 0.8f};
    if (category == "effective_contact") return {1.0f, 0.85f, 0.05f, 1.0f};
    if (category == "root_projection") return {0.0f, 0.85f, 1.0f, 1.0f};
    if (category == "projected_root") return {0.1f, 0.65f, 1.0f, 1.0f};
    if (category == "reachable_corridor") return {0.95f, 0.75f, 0.1f, 0.8f};
    if (category == "collision_accepted") return {0.1f, 0.95f, 0.25f, 1.0f};
    if (category == "collision_rejected" || category == "candidate_section_rejected")
        return {1.0f, 0.05f, 0.1f, 1.0f};
    if (category == "collision_blocker") return {0.65f, 0.1f, 0.8f, 1.0f};
    if (category == "tree_branch") return {0.1f, 1.0f, 0.45f, 1.0f};
    if (category == "frustum_axis") return {0.95f, 0.1f, 0.95f, 1.0f};
    if (category == "frustum_root_section") return {0.45f, 0.2f, 1.0f, 1.0f};
    if (category == "frustum_tip_section") return {1.0f, 0.25f, 0.75f, 1.0f};
    if (category == "base_section" || category == "base_region_input")
        return {0.1f, 0.55f, 1.0f, 1.0f};
    if (category == "interface_section" || category == "interface_region_input")
        return {1.0f, 0.25f, 0.55f, 1.0f};
    if (category == "final_root_open_path")
        return {0.05f, 1.0f, 0.2f, 1.0f};
    if (category == "final_support_extrusion_path")
        return {0.05f, 0.75f, 1.0f, 1.0f};
    if (category == "final_path_footprint_gap_component")
        return {1.0f, 0.65f, 0.05f, 0.9f};
    if (category == "final_path_footprint_gap")
        return {1.0f, 0.85f, 0.15f, 1.0f};
    if (category == "final_disconnected_support_island")
        return {1.0f, 0.0f, 0.0f, 1.0f};
    if (category == "final_disconnected_support_island_gap")
        return {1.0f, 0.1f, 0.8f, 1.0f};
    if (category == "raw_witness_excluded_by_build_plate_policy")
        return {0.7f, 0.25f, 1.0f, 1.0f};
    if (category == "root_open_path_input") return {1.0f, 1.0f, 1.0f, 1.0f};
    return {0.8f, 0.8f, 0.8f, 1.0f};
}

std::shared_ptr<ToolpathPreview> make_belt_support_debug_preview(
    const Slic3r::BeltSupportDebugRecorder& recorder,
    const std::string& output_path)
{
    auto preview = std::make_shared<ToolpathPreview>();
    preview->supported_view_types = {ToolpathViewType::Summary};
    // The complete records remain in the audit artifact. The interactive
    // preview only needs the twelve stage summaries and must stay cheap to
    // query while the layer slider is moving.
    preview->metadata_json =
        belt_support_debug_json(recorder, false, false, output_path).dump();
    std::unordered_map<std::string, std::uint16_t> color_ids;
    std::uint64_t segment_id = 0;

    const auto color_id_for = [&](const std::string& category) {
        const auto found = color_ids.find(category);
        if (found != color_ids.end())
            return found->second;
        const auto id = static_cast<std::uint16_t>(preview->colors.size());
        preview->colors.push_back({id, invalid_toolpath_small_id,
                                   ToolpathColorSource::Custom,
                                   belt_support_debug_color(category), category});
        color_ids.emplace(category, id);
        return id;
    };

    const auto update_bounds = [&preview](const ToolpathPoint& point) {
        if (!preview->bounds.valid) {
            preview->bounds.minimum = point;
            preview->bounds.maximum = point;
            preview->bounds.valid = true;
            return;
        }
        preview->bounds.minimum.x = std::min(preview->bounds.minimum.x, point.x);
        preview->bounds.minimum.y = std::min(preview->bounds.minimum.y, point.y);
        preview->bounds.minimum.z = std::min(preview->bounds.minimum.z, point.z);
        preview->bounds.maximum.x = std::max(preview->bounds.maximum.x, point.x);
        preview->bounds.maximum.y = std::max(preview->bounds.maximum.y, point.y);
        preview->bounds.maximum.z = std::max(preview->bounds.maximum.z, point.z);
    };

    for (std::size_t stage_index = 0; stage_index < recorder.stages().size(); ++stage_index) {
        const auto& stage = recorder.stages()[stage_index];
        ToolpathLayer layer;
        layer.index = static_cast<std::uint32_t>(stage_index);
        layer.segment_begin = preview->segments.size();
        layer.segment_count = stage.lines.size();
        layer.print_z_mm = static_cast<float>(stage_index);
        layer.height_mm = 1.0f;
        layer.duration_seconds = static_cast<float>(stage.elapsed_ms / 1000.0);
        preview->layers.push_back(layer);
        for (const Slic3r::BeltSupportDebugLine& line : stage.lines) {
            ToolpathSegment segment;
            segment.id = segment_id;
            segment.run_id = segment_id++;
            segment.layer_index = static_cast<std::uint32_t>(stage_index);
            segment.color_id = color_id_for(line.category);
            segment.motion = ToolpathMotionKind::Extrusion;
            segment.extrusion_role = ToolpathExtrusionRole::Custom;
            segment.start_mm = {static_cast<float>(line.start_world.x()),
                                static_cast<float>(line.start_world.y()),
                                static_cast<float>(line.start_world.z())};
            segment.end_mm = {static_cast<float>(line.end_world.x()),
                              static_cast<float>(line.end_world.y()),
                              static_cast<float>(line.end_world.z())};
            segment.width_mm = 0.35f;
            segment.height_mm = 0.35f;
            segment.print_z_mm = static_cast<float>(stage_index);
            update_bounds(segment.start_mm);
            update_bounds(segment.end_mm);
            preview->segments.push_back(segment);
        }
    }
    preview->statistics.total_layers = preview->layers.size();
    preview->statistics.logical_motion_count = preview->segments.size();
    preview->statistics.render_segment_count = preview->segments.size();
    return preview;
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
    // Plater's preset loader may retain one self-index for several material
    // variants. Repair only the exported project metadata: the native task,
    // Print configuration and generated calibration G-code must stay intact.
    auto package_config = config;
    normalize_filament_identity(package_config);
    if (model.curr_plate_index > 0) {
        const auto index = std::size_t(model.curr_plate_index);
        for (const char* key : {"wipe_tower_x", "wipe_tower_y"}) {
            auto* coordinate = package_config.option<Slic3r::ConfigOptionFloats>(key);
            const double value = coordinate->get_at(index);
            coordinate->values.assign(1, value);
        }
        auto& modes = package_config.option<Slic3r::ConfigOptionEnumsGeneric>("wipe_tower_position_mode")->values;
        if (!modes.empty()) {
            const int mode = index < modes.size() ? modes[index] : modes.front();
            modes.assign(1, mode);
        }
    }
    Slic3r::PlateData plate;
    plate.plate_index = 0;
    for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
        const auto* object = model.objects[object_index];
        for (std::size_t instance_index = 0; instance_index < object->instances.size(); ++instance_index) {
            plate.objects_and_instances.emplace_back(static_cast<int>(object_index),
                                                     static_cast<int>(instance_index));
        }
    }
    plate.config.apply(package_config);
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
    params.config = &package_config;
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

void report_progress(const SliceCallbacks& callbacks, float progress, std::string_view stage,
                     std::size_t completed_layers = 0, std::size_t total_layers = 0,
                     std::string_view detail = {})
{
    if (callbacks.progress_details) {
        callbacks.progress_details({std::clamp(progress, 0.0f, 1.0f), stage,
                                    completed_layers, total_layers, detail});
    } else if (callbacks.progress) {
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
    case Slic3r::erContinuousFiberContour:   return ToolpathExtrusionRole::ContinuousFiberContour;
    case Slic3r::erContinuousFiberInfill:    return ToolpathExtrusionRole::ContinuousFiberInfill;
    case Slic3r::erResinInfill:              return ToolpathExtrusionRole::ResinInfill;
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
    if (const auto* physical = config->option<Slic3r::ConfigOptionInts>("physical_extruder_map"))
        for (auto& tool : result)
            if (tool >= 0 && size_t(tool) < physical->values.size())
                tool = physical->values[tool];
    return result;
}

// Value-only material descriptions shared by active views and remapped
// scenes. A snapshot has no preset catalog, so compatibility choices are added
// only by active_config after this conversion.
std::vector<FilamentSlotInfo> project_filament_slots(const Slic3r::DynamicPrintConfig& config, std::size_t count)
{
    std::vector<FilamentSlotInfo> slots;
    slots.reserve(count);
    const auto tool_map = filament_tool_map(&config,count);
    for (std::size_t index = 0; index < count; ++index) {
        FilamentSlotInfo slot;
        slot.index=index;
        slot.preset_id=option_value_at(config,"filament_settings_id",index);
        slot.preset_name=slot.preset_id;
        slot.vendor = option_value_at(config, "filament_vendor", index);
        slot.material_type = option_value_at(config, "filament_type", index);
        slot.color = rgba8_color(option_value_at(config, "filament_colour", index));
        if (const auto* diameters = config.option<Slic3r::ConfigOptionFloats>("filament_diameter");
            diameters != nullptr && index < diameters->values.size()) {
            slot.diameter_mm = diameters->values[index];
        }
        slot.physical_tool_index = static_cast<std::size_t>(tool_map[index]);
        slot.physical_tool_name = "Nozzle " + std::to_string(slot.physical_tool_index);
        if (const auto* roles = config.option<Slic3r::ConfigOptionStrings>("toolhead_process_capabilities");
            roles != nullptr && slot.physical_tool_index < roles->values.size()) {
            slot.physical_tool_role = roles->values[slot.physical_tool_index];
        }
        if (const auto* sides = config.option<Slic3r::ConfigOptionStrings>("physical_tool_sides");
            sides != nullptr && slot.physical_tool_index < sides->values.size()) {
            slot.physical_tool_side = sides->values[slot.physical_tool_index];
        }
        if (const auto* nozzles = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
            nozzles != nullptr && slot.physical_tool_index < nozzles->values.size()) {
            slot.nozzle_diameter_mm = nozzles->values[slot.physical_tool_index];
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}

std::optional<Slic3r::BeltCoordinateSystem> belt_coordinates_from_gcode(const std::string& source_path)
{
    std::ifstream input(source_path);
    if (!input)
        return std::nullopt;

    constexpr const char* kinematics = ";SLICING_KINEMATICS:BELT";
    constexpr const char* version_prefix = ";BELT_COORDINATE_VERSION:";
    constexpr const char* angle_prefix = ";BELT_GANTRY_ANGLE:";
    constexpr const char* max_y_prefix = ";BELT_PLATE_MAX_WORLD_Y:";
    constexpr const char* print_origin_prefix = ";BELT_PRINT_ORIGIN_S:";
    bool is_belt = false;
    int coordinate_version = 0;
    std::optional<double> angle;
    std::optional<double> max_world_y;
    std::optional<double> print_origin_s;
    std::string line;
    for (std::size_t line_count = 0; line_count < 200 && std::getline(input, line); ++line_count) {
        try {
            if (line == kinematics) {
                is_belt = true;
            } else if (line.rfind(version_prefix, 0) == 0) {
                coordinate_version = std::stoi(line.substr(
                    std::char_traits<char>::length(version_prefix)));
            } else if (line.rfind(angle_prefix, 0) == 0) {
                angle = std::stod(line.substr(
                    std::char_traits<char>::length(angle_prefix)));
            } else if (line.rfind(max_y_prefix, 0) == 0) {
                max_world_y = std::stod(line.substr(
                    std::char_traits<char>::length(max_y_prefix)));
            } else if (line.rfind(print_origin_prefix, 0) == 0) {
                print_origin_s = std::stod(line.substr(
                    std::char_traits<char>::length(print_origin_prefix)));
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (!is_belt || (coordinate_version != 1 && coordinate_version != 2) ||
        !angle || !max_world_y ||
        (coordinate_version == 2 && !print_origin_s))
        return std::nullopt;
    Slic3r::BeltCoordinateSystem coordinates =
        Slic3r::BeltCoordinateSystem::create(*angle, *max_world_y);
    coordinates.set_print_origin_s(print_origin_s.value_or(0.0));
    return coordinates;
}

std::shared_ptr<ToolpathPreview> make_toolpath_preview(
    const Slic3r::GCodeProcessorResult& source,
    const Slic3r::DynamicPrintConfig* config,
    const std::string& source_path,
    const Slic3r::BeltCoordinateSystem* belt_coordinates = nullptr,
    const std::vector<int>* resolved_tools = nullptr)
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
    auto tool_by_filament = filament_tool_map(config, filament_count);
    if (resolved_tools != nullptr)
        for (std::size_t i = 0; i < std::min(filament_count, resolved_tools->size()); ++i)
            tool_by_filament[i] = resolved_tools->at(i);

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
        std::size_t logical_index = index;
        if (config != nullptr) {
            if (const auto* physical_map = config->option<Slic3r::ConfigOptionInts>("physical_extruder_map")) {
                const auto found = std::find(physical_map->values.begin(), physical_map->values.end(), int(index));
                if (found != physical_map->values.end())
                    logical_index = std::size_t(found - physical_map->values.begin());
            }
        }
        if (nozzle_diameters != nullptr && logical_index < nozzle_diameters->values.size()) {
            tool.nozzle_diameter_mm = static_cast<float>(nozzle_diameters->values[logical_index]);
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
    Slic3r::ToolpathDeposition previous_deposition = Slic3r::ToolpathDeposition::None;
    Slic3r::FiberProcessPhase previous_phase = Slic3r::FiberProcessPhase::None;
    uint64_t previous_occurrence = 0;
    ToolpathExtrusionRole previous_role = ToolpathExtrusionRole::None;
    std::uint32_t previous_layer = invalid_toolpath_id;
    std::uint32_t previous_object = invalid_toolpath_id;
    std::uint16_t previous_tool = invalid_toolpath_small_id;
    std::uint16_t previous_color = invalid_toolpath_small_id;

    for (const auto& input : source.moves) {
        if (!input.internal_only && input.fiber_phase != Slic3r::FiberProcessPhase::None) {
            preview->statistics.total_fiber_feed_mm += std::max(0.0f, input.fiber_feed_delta_mm);
            if (input.fiber_phase == Slic3r::FiberProcessPhase::Prefeed)
                preview->statistics.total_fiber_prefeed_mm += std::max(0.0f, input.fiber_feed_delta_mm);
        }
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
        Slic3r::Vec3f preview_position = input.position;
        if (has_end && belt_coordinates != nullptr) {
            Slic3r::Vec3d logical_machine_position = input.position.cast<double>();
            // GCodeProcessor resolves G92 into its physical axis position. Belt
            // start G-code commonly resets Z after loading the belt, so that
            // physical value is not the logical machine Z emitted for a slice
            // plane. Phase-one Belt printing has no Z hop; print_z is therefore
            // the authoritative physical distance along the slicing normal.
            if (input.print_z > 0.0f) {
                logical_machine_position.z() =
                    belt_coordinates->physical_s_to_machine_z(input.print_z);
            }
            preview_position = belt_coordinates->machine_to_world(
                logical_machine_position).cast<float>();
        }
        const ToolpathPoint end = has_end ? to_toolpath_point(preview_position) : ToolpathPoint{};

        if (motion && previous_position && has_end &&
            point_distance(*previous_position, end) > 0.000001) {
            const ToolpathExtrusionRole role = input.deposition != Slic3r::ToolpathDeposition::None
                ? to_extrusion_role(input.extrusion_role)
                : ToolpathExtrusionRole::None;
            const bool continues_run = previous_was_motion &&
                previous_motion == *motion &&
                previous_deposition == input.deposition && previous_phase == input.fiber_phase &&
                previous_occurrence == input.fiber_occurrence &&
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
            switch (input.deposition) {
            case Slic3r::ToolpathDeposition::None: segment.deposition = ToolpathDepositionKind::None; break;
            case Slic3r::ToolpathDeposition::Thermoplastic: segment.deposition = ToolpathDepositionKind::Thermoplastic; break;
            case Slic3r::ToolpathDeposition::ContinuousFiberPowered: segment.deposition = ToolpathDepositionKind::ContinuousFiberPowered; break;
            case Slic3r::ToolpathDeposition::ContinuousFiberPassive: segment.deposition = ToolpathDepositionKind::ContinuousFiberPassive; break;
            }
            switch (input.fiber_phase) {
            case Slic3r::FiberProcessPhase::None: segment.fiber_phase = ToolpathFiberPhase::None; break;
            case Slic3r::FiberProcessPhase::Approach: segment.fiber_phase = ToolpathFiberPhase::Approach; break;
            case Slic3r::FiberProcessPhase::Prefeed: segment.fiber_phase = ToolpathFiberPhase::Prefeed; break;
            case Slic3r::FiberProcessPhase::Ready: segment.fiber_phase = ToolpathFiberPhase::Ready; break;
            case Slic3r::FiberProcessPhase::Landing: segment.fiber_phase = ToolpathFiberPhase::Landing; break;
            case Slic3r::FiberProcessPhase::Powered: segment.fiber_phase = ToolpathFiberPhase::Powered; break;
            case Slic3r::FiberProcessPhase::Cut: segment.fiber_phase = ToolpathFiberPhase::Cut; break;
            case Slic3r::FiberProcessPhase::Tail: segment.fiber_phase = ToolpathFiberPhase::Tail; break;
            case Slic3r::FiberProcessPhase::Depleted: segment.fiber_phase = ToolpathFiberPhase::Depleted; break;
            case Slic3r::FiberProcessPhase::Finish: segment.fiber_phase = ToolpathFiberPhase::Finish; break;
            case Slic3r::FiberProcessPhase::Complete: segment.fiber_phase = ToolpathFiberPhase::Complete; break;
            }
            segment.fiber_occurrence = input.fiber_occurrence;
            segment.fiber_feed_delta_mm = input.fiber_feed_delta_mm;
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
            if (input.travel_dist > 0)
                segment.fiber_feed_delta_mm *= float(distance / input.travel_dist);
            if (segment.deposition != ToolpathDepositionKind::None) {
                segment.deposited_path_length_mm = float(std::hypot(end.x-previous_position->x, end.y-previous_position->y));
                if (segment.deposition != ToolpathDepositionKind::Thermoplastic)
                    preview->statistics.total_fiber_deposited_path_mm += segment.deposited_path_length_mm;
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
                segment.extrusion_delta_mm = segment.deposition == ToolpathDepositionKind::Thermoplastic && filament_area > 0.0
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
            previous_deposition = input.deposition;
            previous_phase = input.fiber_phase;
            previous_occurrence = input.fiber_occurrence;
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
        if (segment.deposition == ToolpathDepositionKind::None ||
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

PlaneCutResult cut_mesh_with_plane(const PlaneCutRequest& request)
{
    PlaneCutResult result;
    if (request.mesh.vertices.empty() || request.mesh.triangles.empty()) {
        result.error = "Plane cut input mesh has no geometry";
        return result;
    }

    Eigen::Matrix4d mesh_to_plane;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const double value = request.mesh_to_plane[
                static_cast<std::size_t>(row * 4 + column)];
            if (!std::isfinite(value)) {
                result.error = "Plane cut transform contains a non-finite value";
                return result;
            }
            mesh_to_plane(row, column) = value;
        }
    }
    if (std::abs(mesh_to_plane.determinant()) <= 1.0e-12) {
        result.error = "Plane cut transform is not invertible";
        return result;
    }
    const Eigen::Matrix4d plane_to_mesh = mesh_to_plane.inverse();

    indexed_triangle_set source;
    source.vertices.reserve(request.mesh.vertices.size());
    for (const SliceVertex& vertex : request.mesh.vertices) {
        const Eigen::Vector4d transformed = mesh_to_plane * Eigen::Vector4d(
            static_cast<double>(vertex.x), static_cast<double>(vertex.y),
            static_cast<double>(vertex.z), 1.0);
        source.vertices.emplace_back(
            static_cast<float>(transformed.x()),
            static_cast<float>(transformed.y()),
            static_cast<float>(transformed.z()));
    }
    source.indices.reserve(request.mesh.triangles.size());
    for (const SliceTriangle& triangle : request.mesh.triangles) {
        if (triangle.vertex_a >= source.vertices.size() ||
            triangle.vertex_b >= source.vertices.size() ||
            triangle.vertex_c >= source.vertices.size()) {
            result.error = "Plane cut input contains an invalid triangle index";
            return result;
        }
        source.indices.emplace_back(
            static_cast<int>(triangle.vertex_a),
            static_cast<int>(triangle.vertex_b),
            static_cast<int>(triangle.vertex_c));
    }
    // GPlatform may provide triangle soup for legacy meshes. Welding exact
    // duplicates restores the shared-edge topology required for reliable cap
    // construction without changing the model's geometry.
    Slic3r::its_merge_vertices(source);

    indexed_triangle_set upper;
    indexed_triangle_set lower;
    try {
        Slic3r::cut_mesh(source, 0.0f, &upper, &lower, true);
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    } catch (...) {
        result.error = "Plane cutting failed with an unknown error";
        return result;
    }

    const auto publish_mesh = [&plane_to_mesh](
                                  const indexed_triangle_set& source_mesh,
                                  PlaneCutMesh& destination) {
        destination.vertices.reserve(source_mesh.vertices.size());
        for (const Slic3r::Vec3f& vertex : source_mesh.vertices) {
            const Eigen::Vector4d transformed = plane_to_mesh * Eigen::Vector4d(
                static_cast<double>(vertex.x()),
                static_cast<double>(vertex.y()),
                static_cast<double>(vertex.z()), 1.0);
            destination.vertices.push_back(SliceVertex{
                static_cast<float>(transformed.x()),
                static_cast<float>(transformed.y()),
                static_cast<float>(transformed.z())});
        }
        destination.triangles.reserve(source_mesh.indices.size());
        for (const Slic3r::Vec3i32& triangle : source_mesh.indices) {
            destination.triangles.push_back(SliceTriangle{
                static_cast<std::uint32_t>(triangle.x()),
                static_cast<std::uint32_t>(triangle.y()),
                static_cast<std::uint32_t>(triangle.z())});
        }
    };
    publish_mesh(upper, result.upper);
    publish_mesh(lower, result.lower);
    if (result.upper.empty() || result.lower.empty()) {
        result.error = "The cutting plane does not split the mesh into two parts";
        return result;
    }
    result.success = true;
    return result;
}

class ConfigurationState::Impl {
public:
    std::unique_ptr<Config> config;
    ResolvedSelection selection;
    std::vector<PresetOption> compatible_processes, compatible_filaments;
    std::shared_ptr<Slic3r::PresetBundle> bundle;
    // Saved definitions for embedded presets edited through the flat multi-slot
    // API. The bundle always owns their current working values. No Qt/App state.
    std::map<std::string, Slic3r::DynamicPrintConfig> filament_saved_configs;
    ProjectMachine machine;
    ActiveConfigView prepared_view;
    std::string resource_directory;
    Impl() = default;
    Impl(const Impl& other) :
        config(other.config ? std::make_unique<Config>(*other.config) : nullptr),
        selection(other.selection), compatible_processes(other.compatible_processes),
        compatible_filaments(other.compatible_filaments),
        bundle(other.bundle ? std::make_shared<Slic3r::PresetBundle>(*other.bundle) : nullptr),
        filament_saved_configs(other.filament_saved_configs),
        machine(other.machine), prepared_view(other.prepared_view),
        resource_directory(other.resource_directory) {}
};
ConfigurationState::ConfigurationState() = default;
bool ConfigurationState::valid() const noexcept { return impl_ && bool(impl_->config); }
const ActiveConfigView& ConfigurationState::view() const {
    if (!valid()) throw std::logic_error("No configuration in state");
    return impl_->prepared_view;
}
const ProjectMachine& ConfigurationState::machine() const {
    if (!valid()) throw std::logic_error("No configuration in state");
    return impl_->machine;
}
ConfigSnapshot ConfigurationState::snapshot() const { return valid() ? impl_->config->snapshot() : ConfigSnapshot{}; }

class Library::Impl
{
public:
    std::string resource_directory;
    std::vector<std::unique_ptr<Slic3r::PresetBundle>> vendor_presets;
    fs::path user_preset_directory;
    void load_user_presets(Slic3r::PresetBundle&, std::string_view vendor,
                           std::vector<ConfigDiagnostic>* diagnostics) const;
    std::string vendor_for(std::string_view model, std::string_view variant) const;
    Slic3r::PresetBundle* installed_presets(const std::string& model, const std::string& variant) const;
    std::vector<MachineModelOption> machines;
    std::vector<BuildPlateOption> build_plates;
    std::shared_ptr<ConfigurationState::Impl> active = std::make_shared<ConfigurationState::Impl>();
    std::uint64_t active_revision{0};
    std::size_t filament_color_cursor{0};
    std::shared_ptr<const void> capture_origin=std::make_shared<const char>(0);
    void apply_installed_machine_capabilities(ProjectMachine& machine, const ResolvedSelection& selection) const;
    void prepare(const std::shared_ptr<ConfigurationState::Impl>& state, bool import_full_config = true);
    void prepare_preset_edit(const std::shared_ptr<ConfigurationState::Impl>& state);
    Slic3r::DynamicPrintConfig restored_config(const ConfigurationState::Impl& state,
        const Slic3r::DynamicPrintConfig& current, SettingGroup group,
        std::optional<std::size_t> filament_slot = std::nullopt) const;
    ConfigCreateResult resolve_config(const ConfigSelection& selection,
        std::shared_ptr<Slic3r::PresetBundle>* switch_bundle = nullptr) const;
};


namespace {
using ProjectJson = nlohmann::json;

const std::set<std::string>& project_material_keys() {
    static const std::set<std::string> keys={"extruder","outer_wall_filament_id","inner_wall_filament_id","sparse_infill_filament_id","internal_solid_filament_id","top_surface_filament_id","bottom_surface_filament_id","support_filament","support_interface_filament","reinforced_infill_filament","reinforced_perimeters_filament","wipe_tower_filament"};
    return keys;
}
ProjectJson settings_json(const ProjectSettings& settings, bool strip_extruder = true) {
    auto j = ProjectJson::object();
    for (const auto& entry : settings) if (!strip_extruder || entry.first != "extruder") j[entry.first] = entry.second;
    return j;
}
ProjectSettings project_settings(const Slic3r::DynamicPrintConfig& config, bool strip_extruder = true) {
    auto values = serialized_values(config);
    if (strip_extruder) values.erase(std::remove_if(values.begin(), values.end(), [](const auto& v) { return v.first == "extruder"; }), values.end());
    return values;
}
ProjectJson facets_json(const SliceFacetLabels& facets) {
    auto roots = ProjectJson::array();
    for (const auto& r : facets.roots) roots.push_back({{"triangle_index",r.triangle_index},{"bitstream_start_index",r.bitstream_start_index}});
    return {{"roots",roots},{"bitstream",facets.bitstream}};
}
SliceFacetLabels export_facets(const Slic3r::FacetsAnnotation& facets, const SliceCallbacks* callbacks = nullptr) {
    SliceFacetLabels result;
    std::size_t checked=0;
    for (const auto& r : facets.get_data().triangles_to_split) {
        if(callbacks&&((checked++&0x3fff)==0)&&cancellation_requested(*callbacks))throw Slic3r::CanceledException();
        if (r.triangle_idx < 0 || r.bitstream_start_idx < 0) throw std::invalid_argument("Negative facet index");
        result.roots.push_back({unsigned(r.triangle_idx),unsigned(r.bitstream_start_idx)});
    }
    for (bool b : facets.get_data().bitstream) {if(callbacks&&((checked++&0x3fff)==0)&&cancellation_requested(*callbacks))throw Slic3r::CanceledException();result.bitstream.push_back(b ? 1 : 0);}
    return result;
}
SliceFacetLabels facets_from_json(const ProjectJson& j) {
    SliceFacetLabels out;
    for (const auto& r : j.at("roots")) {
        const auto index=r.at("triangle_index"),offset=r.at("bitstream_start_index");
        if(!index.is_number_integer()||!offset.is_number_integer()||index<0||offset<0||index>std::numeric_limits<std::uint32_t>::max()||offset>std::numeric_limits<std::uint32_t>::max())throw std::invalid_argument("Facet root indices must be unsigned integers");
        out.roots.push_back({index.get<unsigned>(),offset.get<unsigned>()});
    }
    for(const auto& bit:j.at("bitstream")){if(!bit.is_number_integer()||(bit!=0&&bit!=1))throw std::invalid_argument("Facet bitstream values must be binary integers");out.bitstream.push_back(bit.get<std::uint8_t>());}
    return out;
}
Slic3r::TriangleSelector::TriangleSplittingData native_facets(const SliceFacetLabels& data, std::size_t triangles, int maximum_state) {
    if (!data.valid()) throw std::invalid_argument("Facet roots/bitstream mismatch");
    Slic3r::TriangleSelector::TriangleSplittingData out;
    for (const auto& r : data.roots) {
        if (r.triangle_index > unsigned(std::numeric_limits<int>::max()) ||
            r.bitstream_start_index > unsigned(std::numeric_limits<int>::max()))
            throw std::invalid_argument("Facet root exceeds native index range");
        out.triangles_to_split.emplace_back(int(r.triangle_index),int(r.bitstream_start_index));
    }
    for (auto bit : data.bitstream) {
        if (bit > 1) throw std::invalid_argument("Facet bit is not binary");
        out.bitstream.push_back(bit != 0);
    }
    out.validate(triangles,static_cast<Slic3r::EnforcerBlockerType>(maximum_state));
    out.update_used_states(0);
    return out;
}
ProjectJson semantic_json(std::string_view payload, const char* scope) {
    if (payload.empty()) return ProjectJson::object();
    auto j=ProjectJson::parse(payload);
    if (!j.is_object() || j.at("version")!=1 || j.at("scope")!=scope) throw std::invalid_argument("Unknown project semantics version/scope");
    std::set<std::string> fields={"version","scope"};
    const std::string owner=scope;
    if(owner=="object")fields.insert({"settings","layer_ranges","layer_height_profile","brim_points"});
    else if(owner=="part")fields.insert("settings");
    else if(owner=="part_annotations")fields.insert({"support_facets","seam_facets","fuzzy_skin_facets"});
    else if(owner=="plate")fields.insert({"settings","native_plate_index","custom_gcode_mode","custom_gcodes"});
    for(auto it=j.begin();it!=j.end();++it)if(!fields.count(it.key()))throw std::invalid_argument("Unknown project semantic field: "+it.key());
    return j;
}
Slic3r::DynamicPrintConfig semantic_settings(const ProjectJson& j) {
    Slic3r::DynamicPrintConfig out;
    if (!j.contains("settings")) return out;
    if (!j.at("settings").is_object()) throw std::invalid_argument("Project settings must be an object");
    for (auto it=j.at("settings").begin();it!=j.at("settings").end();++it) {
        if (!it.value().is_string() || !Slic3r::print_config_def.get(it.key())) throw std::invalid_argument("Unknown project setting: "+it.key());
        out.set_deserialize_strict(it.key(),it.value().get<std::string>());
    }
    return out;
}
void apply_object_semantics(Slic3r::ModelObject& object, const std::string& payload) {
    const auto j=semantic_json(payload,"object"); if (j.empty()) return;
    if (j.contains("settings") && j.at("settings").contains("extruder")) throw std::invalid_argument("Object material slot must use its dedicated field");
    object.config.apply(semantic_settings(j));
    for (const auto& r : j.at("layer_ranges")) {
        const double low=r.at("minimum_z"), high=r.at("maximum_z");
        if (!std::isfinite(low)||!std::isfinite(high)||low<0||high<=low) throw std::invalid_argument("Invalid layer range");
        auto& target=object.layer_config_ranges[{low,high}]; target.assign_config(semantic_settings(r));
    }
    const auto profile=j.at("layer_height_profile").get<std::vector<double>>();
    if (profile.size()%2) throw std::invalid_argument("Layer profile must contain Z/height pairs");
    double previous=-1;
    for (std::size_t i=0;i<profile.size();i+=2) {
        if (!std::isfinite(profile[i])||profile[i]<0||profile[i]<previous||!std::isfinite(profile[i+1])||profile[i+1]<=0) throw std::invalid_argument("Invalid layer profile");
        previous=profile[i];
    }
    object.layer_height_profile.set(profile);
    for (const auto& b : j.at("brim_points")) {
        const float x=b.at("x"),y=b.at("y"),z=b.at("z"),radius=b.at("head_front_radius");
        if (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(radius)||radius<0) throw std::invalid_argument("Invalid brim point");
        object.brim_points.emplace_back(x,y,z,radius);
    }
}
void apply_part_annotations(Slic3r::ModelVolume& volume, const std::string& payload) {
    const auto j=semantic_json(payload,"part_annotations"); if(j.empty())return;
    const auto count=volume.mesh().its.indices.size();
    volume.supported_facets.set_data(native_facets(facets_from_json(j.at("support_facets")),count,2));
    volume.seam_facets.set_data(native_facets(facets_from_json(j.at("seam_facets")),count,2));
    volume.fuzzy_skin_facets.set_data(native_facets(facets_from_json(j.at("fuzzy_skin_facets")),count,2));
}
Slic3r::DynamicPrintConfig effective_plate_settings(const ProjectJson& j) {
    auto settings = semantic_settings(j);
    // PartPlate::get_real_filament_maps / get_real_filament_map_mode:
    // an empty map and Default mode inherit independently from the global config.
    if (const auto* maps = settings.option<Slic3r::ConfigOptionInts>("filament_map"); maps && maps->values.empty())
        settings.erase("filament_map");
    if (const auto* mode = settings.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>("filament_map_mode");
        mode && mode->value == Slic3r::FilamentMapMode::fmmDefault)
        settings.erase("filament_map_mode");
    return settings;
}

// Original Orca PartPlateList::MAX_PLATES_COUNT.
constexpr std::size_t native_plate_limit = 36;

void restore_project_tower_modes(Slic3r::DynamicPrintConfig& raw, std::size_t plate_count)
{
    using namespace Slic3r;
    const auto* x = raw.option<ConfigOptionFloats>("wipe_tower_x");
    const auto* y = raw.option<ConfigOptionFloats>("wipe_tower_y");
    const bool legacy_coordinates = !raw.has("wipe_tower_position_mode") &&
        ((x && !x->values.empty()) || (y && !y->values.empty()));
    const std::size_t count = std::max({std::size_t(1), plate_count, x ? x->size() : 0, y ? y->size() : 0});
    auto& modes = raw.option<ConfigOptionEnumsGeneric>("wipe_tower_position_mode", true)->values;
    if (legacy_coordinates)
        modes.assign(count, int(WipeTowerPositionMode::Manual));
    else if (modes.size() < count) {
        const int fallback = modes.empty() ? int(WipeTowerPositionMode::Auto) : modes.front();
        modes.resize(count, fallback);
    }
}

void apply_plate_semantics(Slic3r::Model& model, Slic3r::DynamicPrintConfig& config, const std::string& payload) {
    const auto j=semantic_json(payload,"plate"); if (j.empty()) return;
    const auto settings = effective_plate_settings(j);
    config.apply(settings);
    const int mode=j.at("custom_gcode_mode");
    if (mode<0||mode>int(Slic3r::CustomGCode::Mode::MultiExtruder)) throw std::invalid_argument("Invalid custom G-code mode");
    const int index=j.value("native_plate_index",0);
    if(index<0)throw std::invalid_argument("Invalid native plate index");
    model.curr_plate_index=index;
    if (!settings.has("wipe_tower_position_mode") && (settings.has("wipe_tower_x") || settings.has("wipe_tower_y"))) {
        auto& modes = config.option<Slic3r::ConfigOptionEnumsGeneric>("wipe_tower_position_mode", true)->values;
        const std::size_t plate = std::size_t(index);
        const std::size_t target = plate < native_plate_limit || plate < modes.size() ? plate : 0;
        const int fallback = modes.empty() ? int(Slic3r::WipeTowerPositionMode::Auto) : modes.front();
        if (modes.size() <= target)
            modes.resize(target + 1, fallback);
        modes[target] = int(Slic3r::WipeTowerPositionMode::Manual);
    }
    auto& code=model.plates_custom_gcodes[index]; code.mode=static_cast<Slic3r::CustomGCode::Mode>(mode);
    for (const auto& c : j.at("custom_gcodes")) {
        const double z=c.at("print_z");const int type=c.at("type"),slot=c.at("filament_slot");
        if(!std::isfinite(z)||z<0||type<0||type>=int(Slic3r::CustomGCode::Unknown))throw std::invalid_argument("Invalid custom G-code item");
        // Native Custom/Pause/Template never consume Item::extruder. Historical
        // archives may contain an uninitialized sentinel there; preserve it.
        if((type==int(Slic3r::CustomGCode::ColorChange)&&slot<0)||
           (type==int(Slic3r::CustomGCode::ToolChange)&&slot<1))throw std::invalid_argument("Invalid custom G-code material reference");
        code.gcodes.push_back({z,static_cast<Slic3r::CustomGCode::Type>(type),slot,c.at("color").get<std::string>(),c.at("extra").get<std::string>()});
    }
}
// Bed3D::set_shape / calc_printable_bounding_box -> Plater::set_bed_shape ->
// PartPlateList::reset_size(int,int,int). In particular, do not retain .25 mm
// or infer a stride directly from a floating-point printable-area width.
// Bed3D::detect_type and its effective STL check, without rendering/UI state.
std::pair<bool,std::string> project_bed_model(Slic3r::PresetBundle& bundle,const Slic3r::DynamicPrintConfig& config) {
    using namespace Slic3r;
    const auto shape=make_counter_clockwise(config.option<ConfigOptionPoints>("printable_area")->values);
    bool system=false;std::string model;
    for(const Preset* preset=&bundle.printers.get_selected_preset();preset;preset=bundle.printers.get_preset_parent(*preset)) {
        const auto* area=preset->config.option<ConfigOptionPoints>("printable_area");
        if(!area || shape!=make_counter_clockwise(area->values))continue;
        if(preset->is_system)model=PresetUtils::system_printer_bed_model(*preset);
        else if(const auto* name=preset->config.option<ConfigOptionString>("printer_model");name&&!name->value.empty())
            model=bundle.get_stl_model_for_printer_model(name->value);
        if(!model.empty()){system=true;break;}
    }
    if(const auto* custom=config.option<ConfigOptionString>("bed_custom_model");custom&&!custom->value.empty())model=custom->value;
    std::error_code error;auto extension=fs::u8path(model).extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(extension!=".stl" || !fs::exists(fs::u8path(model),error) || error)model.clear();
    return {system,model};
}
bool project_bed_inputs_changed(const Slic3r::DynamicPrintConfig& before,const Slic3r::DynamicPrintConfig& after) {
    // Exclusion/clearance changes refresh PartPlate but Bed3D keeps its cached
    // bounding box; they must not recompute the axes-dependent dimensions.
    for(const char* key:{"printable_area","printable_height","extruder_printable_area","extruder_printable_height"}) {
        const auto* a=before.option(key);const auto* b=after.option(key);
        if(bool(a)!=bool(b))return true;
        if(a) {
            if(std::string_view(key)=="printable_area") {
                if(Slic3r::make_counter_clockwise(static_cast<const Slic3r::ConfigOptionPoints*>(a)->values)!=
                   Slic3r::make_counter_clockwise(static_cast<const Slic3r::ConfigOptionPoints*>(b)->values))return true;
            } else if(a->serialize()!=b->serialize())return true;
        }
    }
    return false;
}
float project_axes_length(const MachineVariantOption& machine) {
    Slic3r::Pointfs shape;
    for (const auto& p:machine.printable_area) shape.emplace_back(p.x,p.y);
    return .1f*float(Slic3r::BuildVolume(shape,machine.printable_height,{},{}).bounding_volume().max_size());
}
std::array<int,3> project_native_plate_size(const MachineVariantOption& machine, float axes_length) {
    Slic3r::Pointfs shape;
    for (const auto& p:machine.printable_area) shape.emplace_back(p.x,p.y);
    auto bounds=Slic3r::BuildVolume(shape,machine.printable_height,{},{}).bounding_volume();
    const auto size=bounds.size();
    if (size.x()>0 && size.y()>0 && !bounds.defined) bounds.defined=true;
    bounds.min.z()=bounds.max.z()=0;
    bounds.merge(Slic3r::Vec3d(axes_length,axes_length,axes_length));
    constexpr double tip_radius=1.25;
    bounds.merge(bounds.min+Slic3r::Vec3d(-tip_radius,-tip_radius,bounds.max.z()));
    return {int(bounds.max.x()-bounds.min.x()-tip_radius),
            int(bounds.max.y()-bounds.min.y()-tip_radius),int(machine.printable_height)};
}
ProjectMachine describe_project_machine(const Slic3r::DynamicPrintConfig& config, bool validate = true) {
    ProjectMachine machine; machine.project_local=true;
    machine.model.id=option_value_at(config,"printer_model",0);
    machine.model.name=option_value_at(config,"printer_settings_id",0);
    if(machine.model.id.empty()) machine.model.id=machine.model.name.empty()?"project-printer":machine.model.name;
    if(machine.model.name.empty())machine.model.name=machine.model.id;
    auto& v=machine.variant;
    v.id=option_value_at(config,"printer_variant",0);
    v.name=v.id;v.printer_preset_id=option_value_at(config,"printer_settings_id",0);
    const auto* nozzles=config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    if(!nozzles||nozzles->values.empty())throw std::invalid_argument("Project has no nozzle diameter");
    v.physical_tool_count=nozzles->values.size();v.nozzle_diameter=nozzles->values.front();
    if(v.id.empty())v.id=nozzles->serialize();if(v.name.empty())v.name=v.id;
    for(double diameter:nozzles->values)if(validate && (!std::isfinite(diameter)||diameter<=0))throw std::invalid_argument("Invalid project nozzle diameter");
    populate_printable_volume(config,v);
    if(v.printable_area.size()<3||!std::isfinite(v.printable_height)||v.printable_height<0)throw std::invalid_argument("Project has no valid printable volume");
    const auto* multi=config.option<Slic3r::ConfigOptionBool>("single_extruder_multi_material");
    v.variable_filament_slots=multi&&multi->value;
    const auto* fixed=config.option<Slic3r::ConfigOptionBool>("filament_slots_bound_to_physical_tools");
    v.filament_slots_bound_to_physical_tools=fixed&&fixed->value;
    const auto* capacity=config.option<Slic3r::ConfigOptionInts>("toolhead_filament_capacity");
    v.max_filament_slots=std::max(v.physical_tool_count,serialized_option_values(config,"filament_diameter").size());
    if(capacity&&!capacity->values.empty()) {
        v.toolhead_filament_capacity=capacity->values;v.max_filament_slots=0;
        if(validate && capacity->size()!=v.physical_tool_count)throw std::invalid_argument("Project material capacity does not match nozzles");
        for(int c:capacity->values){if(validate && c<1)throw std::invalid_argument("Invalid material capacity");if(c>0)v.max_filament_slots+=std::size_t(c);}
        v.variable_filament_slots=v.max_filament_slots>v.physical_tool_count;
    }
    machine.model.variants.push_back(v);return machine;
}
void mark_full_config_authoritative(Slic3r::DynamicPrintConfig& config) {
    const auto count=serialized_option_values(config,"filament_colour").size();
    std::vector<std::string> dirty(count+2,Slic3r::escape_strings_cstyle(Slic3r::Preset::filament_options()));
    dirty.front()=Slic3r::escape_strings_cstyle(Slic3r::Preset::print_options());
    dirty.back()=Slic3r::escape_strings_cstyle(Slic3r::Preset::printer_options());
    config.set_key_value("different_settings_to_system",new Slic3r::ConfigOptionStrings(std::move(dirty)));
}

}
std::string encode_project_object_semantics(const ProjectObject& object) {
    auto ranges=ProjectJson::array(), brim=ProjectJson::array();
    for(const auto& r:object.layer_ranges)ranges.push_back({{"minimum_z",r.minimum_z},{"maximum_z",r.maximum_z},{"settings",settings_json(r.settings,false)}});
    for(const auto& b:object.brim_points)brim.push_back({{"x",b.x},{"y",b.y},{"z",b.z},{"head_front_radius",b.head_front_radius}});
    return ProjectJson{{"version",1},{"scope","object"},{"settings",settings_json(object.settings)},{"layer_ranges",ranges},{"layer_height_profile",object.layer_height_profile},{"brim_points",brim}}.dump();
}

std::string encode_project_part_semantics(const ProjectPart& part) {return ProjectJson{{"version",1},{"scope","part"},{"settings",settings_json(part.settings)}}.dump();}

Config Config::for_project_overrides(const ConfigSnapshot& base, std::string_view plate,
    std::string_view target, std::optional<std::string_view> parent_object)
{
    if (!base.valid())
        throw std::invalid_argument("Model setting baseline is unavailable");
    auto baseline = dynamic_config(base);
    baseline.apply(effective_plate_settings(semantic_json(plate, "plate")));
    if (parent_object)
        baseline.apply(semantic_settings(semantic_json(*parent_object, "object")));
    auto config = for_overrides(Config(serialized_values(baseline)).snapshot(),
        parent_object ? ConfigScope::ModelPart : ConfigScope::ModelObject);
    config.load_native_overrides(serialized_values(
        semantic_settings(semantic_json(target, parent_object ? "part" : "object"))));
    return config;
}

SettingsResult edit_project_override(const ConfigSnapshot& base, std::string_view plate,
    std::string& target, std::optional<std::string_view> parent_object,
    std::string_view key, std::optional<std::string_view> value)
{
    try {
        auto config = Config::for_project_overrides(base, plate, target, parent_object);
        auto result = value ? config.set(key, *value) : config.reset(key);
        if (!result)
            return result;

        auto document = semantic_json(target, parent_object ? "part" : "object");
        if (document.empty()) {
            if (!value)
                return result;
            document = ProjectJson::parse(parent_object
                ? encode_project_part_semantics({}) : encode_project_object_semantics({}));
        }
        const std::string name(key);
        if (value) {
            const auto normalized = config.snapshot().value(key).value();
            const auto& settings = document["settings"];
            const auto previous = settings.find(name);
            if (previous != settings.end() && *previous == normalized)
                return result;
            document["settings"][name] = normalized;
        } else {
            const auto settings = document.find("settings");
            if (settings == document.end() || !settings->erase(name))
                return result;
        }
        target = document.dump();
        return result;
    } catch (const std::exception& error) {
        return settings_failure(std::string(key), error.what());
    }
}

std::string encode_project_part_annotations(const ProjectPart& part) {
    return ProjectJson{{"version",1},{"scope","part_annotations"},{"support_facets",facets_json(part.support_facets)},{"seam_facets",facets_json(part.seam_facets)},{"fuzzy_skin_facets",facets_json(part.fuzzy_skin_facets)}}.dump();
}
std::string encode_project_plate_semantics(const ProjectPlate& plate) {
    auto code=ProjectJson::array();for(const auto& c:plate.custom_gcodes)code.push_back({{"print_z",c.print_z},{"type",c.type},{"filament_slot",c.filament_slot},{"color",c.color},{"extra",c.extra}});
    return ProjectJson{{"version",1},{"scope","plate"},{"native_plate_index",plate.native_plate_index},{"settings",settings_json(plate.settings,false)},{"custom_gcode_mode",plate.custom_gcode_mode},{"custom_gcodes",code}}.dump();
}

std::string synchronize_project_model_materials(
    std::string_view payload, std::string_view scope, std::size_t count)
{
    if (scope != "object" && scope != "part")
        throw std::invalid_argument("Model material cleanup requires object or part semantics");
    auto document = semantic_json(payload, std::string(scope).c_str());
    if (document.empty()) return std::string(payload);
    const auto settings = semantic_settings(document);
    bool changed = false;
    for (const auto* key : {"support_filament", "support_interface_filament"}) {
        const auto* slot = settings.option<Slic3r::ConfigOptionInt>(key);
        if (slot && slot->value > 0 && std::size_t(slot->value) > count) {
            document["settings"].erase(key);
            changed = true;
        }
    }
    return changed ? document.dump() : std::string(payload);
}

std::string synchronize_project_plate_materials(std::string_view payload,
    const ConfigSnapshot& target, std::vector<ConfigDiagnostic>* diagnostics,
    bool update_print_sequence, bool reset_single_tool_mapping) {
    if (!target.valid()) throw std::invalid_argument("Plate material synchronization requires a configuration");
    auto j = semantic_json(payload, "plate");
    if (j.empty()) return std::string(payload);
    auto settings = effective_plate_settings(j);
    auto global = dynamic_config(target);
    const auto tools = tool_config(global);
    const auto count = tools.filament_diameter.size();
    Slic3r::validate_material_tool_bindings(tools);
    const auto report = [&](const std::string& message) {
        if (diagnostics) diagnostics->push_back({"filament_map", message});
    };
    auto* maps = settings.option<Slic3r::ConfigOptionInts>("filament_map");
    bool reset = false;
    if (maps) {
        const auto previous_count = maps->size();
        // Native PartPlate::set_filament_count resizes ordinary maps with 1.
        // Capacity-limited machines must instead use their resolved tool owners.
        maps->values.resize(count, 1);
        if (!tools.toolhead_filament_capacity.values.empty())
            for (size_t i = previous_count; i < count; ++i)
                maps->values[i] = tools.filament_map.values.at(i);
        try {
            auto effective = global;
            effective.apply(settings);
            const auto bindings = tool_config(effective);
            for (int logical : maps->values)
                if (logical < 1 || size_t(logical) > bindings.nozzle_diameter.size())
                    throw std::invalid_argument("Plate mapping references a missing tool");
            if (fixed_physical_filament_slots(global) && maps->values != tools.filament_map.values)
                throw std::invalid_argument("Plate mapping conflicts with fixed physical material slots");
            Slic3r::validate_material_tool_bindings(bindings);
            if (previous_count != count) report("Resized plate material mapping to " + std::to_string(count) + " slots");
        } catch (const std::invalid_argument& error) {
            reset = true;
            report(std::string(error.what()) + "; inheriting target material bindings and mode");
        }
        // A fixed-slot machine cannot retain a partial foreign mapping/mode.
        if (fixed_physical_filament_slots(global) && previous_count != count) {
            reset = true;
            report("Foreign plate material domain replaced by target fixed-slot bindings and mode");
        }
    }
    if (fixed_physical_filament_slots(global)) {
        const auto* mode = settings.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>("filament_map_mode");
        const auto* target_mode = global.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>("filament_map_mode");
        if (mode && mode->value != Slic3r::FilamentMapMode::fmmManual && mode->value != target_mode->value) {
            reset = true;
            report("Plate automatic mode is incompatible with target fixed-slot bindings; inheriting target mode");
        }
    }
    if (reset) {
        settings.erase("filament_map");
        settings.erase("filament_map_mode");
    }
    // PartPlate::on_extruder_count_changed resets even a valid manual map.
    if (reset_single_tool_mapping) {
        settings.erase("filament_map");
        settings.set_key_value("filament_map_mode",
            new Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>(Slic3r::FilamentMapMode::fmmAutoForFlush));
    }
    // Only write affected fields so unrelated authored settings stay verbatim.
    for (const char* key : {"filament_map", "filament_map_mode"}) {
        j["settings"].erase(key);
        if (const auto* value = settings.option(key)) j["settings"][key] = value->serialize();
    }
    if (!update_print_sequence) return j.dump();
    const auto remove_expired_slots = [count](std::vector<int>& order) {
        order.erase(std::remove_if(order.begin(), order.end(), [count](int slot) {
            return std::size_t(slot) > count;
        }), order.end());
    };
    const auto append_slots = [count](std::vector<int>& order) {
        while (order.size() < count) order.push_back(int(order.size() + 1));
    };
    // PartPlate::update_first_layer_print_sequence does not pad a pruned first layer.
    if (const auto* first = settings.option<Slic3r::ConfigOptionInts>("first_layer_print_sequence");
        first && !first->values.empty() && first->values.front() != 0 && first->size() != count) {
        auto order = first->values;
        if (order.size() > count)
            remove_expired_slots(order);
        else
            append_slots(order);
        j["settings"]["first_layer_print_sequence"] = Slic3r::ConfigOptionInts(std::move(order)).serialize();
    }
    if (const auto* other = settings.option<Slic3r::ConfigOptionInts>("other_layers_print_sequence")) {
        const auto* segments = settings.option<Slic3r::ConfigOptionInt>("other_layers_print_sequence_nums");
        if (segments && segments->value > 0 &&
            other->size() % std::size_t(segments->value) == 0 &&
            other->size() / std::size_t(segments->value) >= 3) {
            auto sequences = Slic3r::get_other_layers_print_sequence(segments->value, other->values);
            bool changed = false;
            for (auto& [layer_range, order] : sequences) {
                if (order.size() == count) continue;
                if (order.size() > count)
                    remove_expired_slots(order);
                append_slots(order);
                changed = true;
            }
            if (changed) {
                int segment_count = 0;
                std::vector<int> values;
                Slic3r::get_other_layers_print_sequence(sequences, segment_count, values);
                j["settings"]["other_layers_print_sequence"] = Slic3r::ConfigOptionInts(std::move(values)).serialize();
                j["settings"]["other_layers_print_sequence_nums"] = std::to_string(segment_count);
            }
        }
    }
    return j.dump();
}


void Library::Impl::apply_installed_machine_capabilities(ProjectMachine& machine, const ResolvedSelection& selection) const {
    if (machine.project_local) return;
    for (const auto& installed : machines) {
        if (installed.id != selection.machine_model_id) continue;
        const auto catalog = std::find_if(installed.variants.begin(), installed.variants.end(),
            [&](const MachineVariantOption& variant) { return variant.id == selection.machine_variant_id; });
        auto& effective = machine.variant;
        // Older installed MMU profiles declare their feed capacity in the
        // catalog, independently of the current material palette. Preserve the
        // full configuration's geometry, tool mode and explicit capacities.
        if (catalog != installed.variants.end() && effective.toolhead_filament_capacity.empty() &&
            catalog->toolhead_filament_capacity.empty() && effective.variable_filament_slots &&
            catalog->variable_filament_slots && !effective.filament_slots_bound_to_physical_tools &&
            !catalog->filament_slots_bound_to_physical_tools &&
            effective.physical_tool_count == catalog->physical_tool_count)
            effective.max_filament_slots = std::max(effective.max_filament_slots, catalog->max_filament_slots);
        machine.model = installed;
        break;
    }
}
void Library::Impl::prepare(const std::shared_ptr<ConfigurationState::Impl>& state, bool import_full_config) {
    state->resource_directory=resource_directory;
    if (!state->config) return;
    auto full=dynamic_config(state->config->snapshot());
    if(state->bundle) {
        if (import_full_config) {
            auto imported = full;
            mark_full_config_authoritative(imported);
            state->bundle->load_config_model("GPlatform active project.3mf", std::move(imported));
            // A modified secondary slot may become an Orca project preset.
            // Carry its ID into the snapshot so the next edit reuses it.
            const auto& selected_filaments = state->bundle->filament_presets;
            const auto* filament_ids = full.option<Slic3r::ConfigOptionStrings>("filament_settings_id");
            if (filament_ids && filament_ids->values != selected_filaments) {
                full.option<Slic3r::ConfigOptionStrings>("filament_settings_id", true)->values = selected_filaments;
                state->config.reset(new Config(serialized_values(full)));
            }
        }
        state->selection.printer_preset_id=state->bundle->printers.get_selected_preset_name();
        state->selection.process_preset_id=state->bundle->prints.get_selected_preset_name();
        state->selection.filament_preset_ids=state->bundle->filament_presets;
        state->compatible_processes=compatible_presets(state->bundle->prints,state->selection.process_preset_id);
        state->compatible_filaments=compatible_presets(state->bundle->filaments,state->selection.filament_preset_ids.empty()?"":state->selection.filament_preset_ids.front());
    }
    const bool local=state->machine.project_local;
    const auto previous_capacity=state->machine.variant.max_filament_slots;
    state->machine=describe_project_machine(full);state->machine.project_local=local;
    if(local && state->machine.variant.toolhead_filament_capacity.empty()) state->machine.variant.max_filament_slots=std::max(state->machine.variant.max_filament_slots,previous_capacity);
    apply_installed_machine_capabilities(state->machine,state->selection);
    Library viewer;viewer.impl_->resource_directory=resource_directory;viewer.impl_->active=state;viewer.impl_->active_revision=1;
    state->prepared_view=*viewer.active_config();state->prepared_view.revision=0;
}

void Library::Impl::prepare_preset_edit(const std::shared_ptr<ConfigurationState::Impl>& state)
{
    if (!state->bundle) {
        prepare(state);
        return;
    }
    const auto before = dynamic_config(active->config->snapshot());
    const auto edited = dynamic_config(state->config->snapshot());
    const auto changed = edited.diff(before);
    const auto& material_keys = Slic3r::Preset::filament_options();
    if (std::any_of(changed.begin(), changed.end(), [&](const auto& key) {
            return std::find(material_keys.begin(), material_keys.end(), key) != material_keys.end() &&
                   !state->bundle->project_config.has(key);
        })) {
        auto saved = *state->bundle;
        const auto old_ids = saved.filament_presets;
        prepare(state);
        // Orca's model splitter materializes independently edited secondary
        // slots as embedded presets. Preserve the original definition once,
        // without overwriting the embedded preset's current configuration.
        for (std::size_t slot = 0; slot < old_ids.size() && slot < state->bundle->filament_presets.size(); ++slot) {
            const auto& id = state->bundle->filament_presets[slot];
            const auto* live = state->bundle->filaments.find_preset(id, false, true);
            const auto* original = saved.filaments.find_preset(old_ids[slot], false, true);
            if (!original || !live) throw std::runtime_error("Cannot retain the saved material preset");
            if (!live->is_project_embedded || state->filament_saved_configs.count(id)) continue;
            const auto prior = state->filament_saved_configs.find(old_ids[slot]);
            auto baseline = prior == state->filament_saved_configs.end() ? original->config : prior->second;
            baseline.option<Slic3r::ConfigOptionStrings>("filament_settings_id", true)->values = {id};
            if (baseline.diff(live->config).empty()) continue;
            state->filament_saved_configs.emplace(id, std::move(baseline));
        }
        prepare(state, false);
        return;
    }
    auto bundle = state->bundle;
    // Use Orca to split the flat settings back into their owning presets.
    // Keep the selected presets and apply only the edited fields. Reloading
    // the whole project here also marks unrelated imported presets dirty.
    state->bundle = std::make_shared<Slic3r::PresetBundle>(*bundle);
    auto split = edited;
    mark_full_config_authoritative(split);
    state->bundle->load_config_model("GPlatform active project.3mf", std::move(split));
    for (const auto& [destination, source, keys] : {
         std::tuple<Slic3r::PresetCollection*, const Slic3r::PresetCollection*, const std::vector<std::string>*>{
             &bundle->printers, &state->bundle->printers, &Slic3r::Preset::printer_options()},
         {&bundle->prints, &state->bundle->prints, &Slic3r::Preset::print_options()}}) {
        std::vector<std::string> own_changes;
        for (const auto& key : changed)
            if (std::find(keys->begin(), keys->end(), key) != keys->end()) own_changes.push_back(key);
        if (own_changes.empty()) continue;
        destination->get_edited_preset().config.apply_only(source->get_edited_preset().config, own_changes, true);
        destination->update_dirty();
    }
    bundle->project_config = std::move(state->bundle->project_config);
    state->bundle = std::move(bundle);
    prepare(state, false);
}

Slic3r::DynamicPrintConfig Library::Impl::restored_config(
    const ConfigurationState::Impl& state, const Slic3r::DynamicPrintConfig& current,
    SettingGroup group, std::optional<std::size_t> filament_slot) const
{
    if (!state.bundle) return current;
    const auto& bundle = *state.bundle;
    auto printer = bundle.printers.get_edited_preset();
    auto process = bundle.prints.get_edited_preset();
    std::vector<Slic3r::Preset> materials;
    for (const auto& id : state.selection.filament_preset_ids) {
        const auto* preset = bundle.filaments.find_preset(id, false);
        if (!preset) throw std::runtime_error("The active material preset is missing");
        materials.push_back(*preset);
    }
    if (group == SettingGroup::Process) {
        process.config = bundle.prints.get_selected_preset().config;
    } else if (group == SettingGroup::Printer) {
        printer.config = bundle.printers.get_selected_preset().config;
    } else {
        const auto& id = state.selection.filament_preset_ids.at(filament_slot.value());
        const auto saved = state.filament_saved_configs.find(id);
        const auto* original = bundle.filaments.get_selected_preset_name() == id
            ? &bundle.filaments.get_selected_preset() : bundle.filaments.find_preset(id, false);
        if (!original) throw std::runtime_error("The saved material preset is missing");
        const auto& values = saved == state.filament_saved_configs.end() ? original->config : saved->second;
        for (auto& material : materials) if (material.name == id) material.config = values;
    }
    const auto* map = current.option<Slic3r::ConfigOptionInts>("filament_map");
    const auto restored = Slic3r::PresetBundle::construct_full_config(
        printer, process, bundle.project_config, materials, true,
        map ? std::optional(map->values) : std::nullopt);
    auto full = current;
    const auto& keys = group == SettingGroup::Process ? Slic3r::Preset::print_options()
                     : group == SettingGroup::Printer ? Slic3r::Preset::printer_options()
                                                      : Slic3r::Preset::filament_options();
    full.apply_only(restored, keys, true);
    full.apply_only(current, bundle.project_config.keys(), true);
    return full;
}

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
        if (!options.user_preset_directory.empty())
            library->impl_->user_preset_directory = fs::absolute(options.user_preset_directory);
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
                        if (const auto* fixed = preset->config.option<Slic3r::ConfigOptionBool>(
                                "filament_slots_bound_to_physical_tools")) {
                            option.filament_slots_bound_to_physical_tools = fixed->value;
                        }
                        // Current supported single-nozzle material systems expose
                        // four feed slots. Fixed multi-tool machines use exactly
                        // their physical tool count.
                        option.max_filament_slots = option.variable_filament_slots
                            ? 4
                            : option.physical_tool_count;
                        if (const auto* capacities = preset->config.option<Slic3r::ConfigOptionInts>("toolhead_filament_capacity");
                            capacities && !capacities->values.empty()) {
                            if (capacities->size() != option.physical_tool_count ||
                                std::any_of(capacities->values.begin(), capacities->values.end(), [](int count) { return count < 1; }))
                                throw std::invalid_argument("Invalid per-tool material capacity: " + preset->name);
                            option.max_filament_slots = 0;
                            option.toolhead_filament_capacity = capacities->values;
                            for (int count : capacities->values) option.max_filament_slots += size_t(count);
                            option.variable_filament_slots = option.max_filament_slots > option.physical_tool_count;
                        }
                        populate_printable_volume(preset->config, option);
                        machine.variants.push_back(std::move(option));
                    }
                    if (!machine.variants.empty()) {
                        library->impl_->machines.push_back(std::move(machine));
                    }
                }
            }
            library->impl_->load_user_presets(*presets, vendor, diagnostics);
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
    return impl_->resolve_config(selection);
}

namespace {
const Slic3r::Preset* requested_printer(const Slic3r::PrinterPresetCollection& printers, const ConfigSelection& selection)
{
    if (selection.printer_preset_id.empty())
        return printers.find_system_preset_by_model_and_variant(selection.machine_model_id, selection.machine_variant_id);

    const auto* preset = printers.find_preset(selection.printer_preset_id, false);
    return preset && preset->is_visible && preset->config.opt_string("printer_model") == selection.machine_model_id &&
        preset->config.opt_string("printer_variant") == selection.machine_variant_id ? preset : nullptr;
}
} // namespace

ConfigCreateResult Library::Impl::resolve_config(const ConfigSelection& selection,
    std::shared_ptr<Slic3r::PresetBundle>* switch_bundle) const
{
    ConfigCreateResult result;
    try {
        const bool editing_project = !switch_bundle && active->bundle &&
            selection.machine_model_id == active->selection.machine_model_id &&
            selection.machine_variant_id == active->selection.machine_variant_id &&
            (selection.printer_preset_id.empty() || selection.printer_preset_id == active->selection.printer_preset_id);
        // Tab::select_preset updates compatibility in the active bundle before
        // optionally restoring the target printer's remembered selections.
        const bool keep_compatible = switch_bundle && active->bundle &&
            selection.process_preset_id.empty() && selection.filament_preset_ids.empty() &&
            requested_printer(active->bundle->printers, selection);
        const auto* source_presets = editing_project || keep_compatible ? active->bundle.get()
            : installed_presets(selection.machine_model_id, selection.machine_variant_id);
        if (source_presets == nullptr) {
            result.diagnostics.push_back({"machine", "Unknown machine model or nozzle variant"});
            return result;
        }
        const auto* printer = requested_printer(source_presets->printers, selection);
        if (!printer && !selection.printer_preset_id.empty())
            throw std::invalid_argument("Unknown printer preset: " + selection.printer_preset_id);
        std::string printer_name = printer ? printer->name
            : editing_project ? active->selection.printer_preset_id : std::string{};
        if (printer_name.empty()) throw std::invalid_argument("Machine has no printer preset");
        auto selected = *source_presets;
        const bool same_active_machine = editing_project &&
            selection.machine_model_id == active->selection.machine_model_id &&
            selection.machine_variant_id == active->selection.machine_variant_id &&
            !active->selection.printer_preset_id.empty() &&
            selected.printers.find_preset(
                active->selection.printer_preset_id, false) != nullptr;
        if (same_active_machine) {
            // A project import may leave the selected preset edited with
            // custom values. Keep it while changing process or filament
            // choices for the same physical machine.
            printer_name = active->selection.printer_preset_id;
        }
        const bool keep_edited_printer = same_active_machine &&
            selected.printers.get_selected_preset_name() == printer_name;
        if (!keep_edited_printer) {
            selected.printers.select_preset_by_name(printer_name, true);
        }
        selected.update_compatible(Slic3r::PresetSelectCompatibleType::Never);

        const Slic3r::Preset& active_printer = selected.printers.get_edited_preset();
        std::string process_name = selection.process_preset_id;
        std::vector<std::string> filament_names = selection.filament_preset_ids;
        if (keep_compatible) {
            selected.update_compatible(Slic3r::PresetSelectCompatibleType::Always);
            process_name = selected.prints.get_selected_preset_name();
            filament_names = selected.filament_presets;
            const auto tools = tool_config(active_printer.config);
            if (tools.filament_slots_bound_to_physical_tools.value &&
                filament_names.size() != tools.nozzle_diameter.size())
                filament_names.clear();
        } else if (switch_bundle && (!process_name.empty() || !filament_names.empty())) {
            Slic3r::AppConfig remembered;
            remembered.set_printer_setting(printer_name, PRESET_PRINT_NAME, process_name);
            if (!filament_names.empty()) {
                const auto remembered_filament = [](const std::string& name) {
                    return name.empty() ? std::string(Slic3r::PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER) : name;
                };
                remembered.set_printer_setting(printer_name, PRESET_FILAMENT_NAME, remembered_filament(filament_names.front()));
                for (std::size_t slot = 1; slot < filament_names.size(); ++slot) {
                    char key[32];
                    std::snprintf(key, sizeof(key), "filament_%02zu", slot);
                    remembered.set_printer_setting(printer_name, key, remembered_filament(filament_names[slot]));
                }
            }
            selected.update_selections(remembered);
            process_name = selected.prints.get_selected_preset_name();
            filament_names = selected.filament_presets;
        }
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
        const bool keep_edited_process = (keep_edited_printer || keep_compatible) &&
            process_name == active->selection.process_preset_id &&
            selected.prints.get_selected_preset_name() == process_name;
        if (!keep_edited_process) {
            selected.prints.select_preset_by_name(process_name, true);
        }
        selected.update_compatible(Slic3r::PresetSelectCompatibleType::Always);

        auto remembered_tools = selection.filament_physical_tools;
        if (keep_compatible && selection.machine_model_id == active->selection.machine_model_id &&
            selection.machine_variant_id == active->selection.machine_variant_id &&
            filament_names.size() == active->selection.filament_preset_ids.size())
            remembered_tools = active->selection.filament_physical_tools;
        const auto printer_tools = tool_config(active_printer.config);
        if (switch_bundle && std::any_of(remembered_tools.begin(), remembered_tools.end(), [&](unsigned owner) {
                const auto& map = printer_tools.physical_extruder_map.values;
                return owner >= printer_tools.nozzle_diameter.size() ||
                    std::count(map.begin(), map.end(), int(owner)) != 1;
            })) {
            // These owners were saved for a different hardware layout.
            // Fall back to the target's normal physical-tool assignment.
            remembered_tools.clear();
            if (!printer_tools.toolhead_filament_capacity.values.empty() &&
                filament_names.size() > printer_tools.nozzle_diameter.size())
                filament_names.resize(printer_tools.nozzle_diameter.size());
        }
        if (!filament_names.empty()) {
            const auto& tools = printer_tools;
            for (std::size_t slot = 0; slot < filament_names.size(); ++slot) {
                std::string& filament_name = filament_names[slot];
                if (switch_bundle && Slic3r::has_fiber_tool(tools)) {
                    const auto physical = slot < remembered_tools.size() ? remembered_tools[slot] : slot;
                    const auto accepts = [&](const Slic3r::Preset& preset) {
                        return preset.is_visible && preset.is_compatible &&
                            Slic3r::tool_accepts_process(tools, unsigned(physical), preset_process(preset));
                    };
                    const auto* current = selected.filaments.find_preset(filament_name, false);
                    if (!current || !accepts(*current)) {
                        const auto* defaults = active_printer.config.option<Slic3r::ConfigOptionStrings>("default_filament_profile");
                        const auto* preferred = defaults && physical < defaults->size()
                            ? selected.filaments.find_preset(defaults->values[physical], false) : nullptr;
                        if (preferred && accepts(*preferred))
                            filament_name = preferred->name;
                        else {
                            const auto replacement = std::find_if(selected.filaments.begin(), selected.filaments.end(), accepts);
                            if (replacement == selected.filaments.end())
                                throw std::invalid_argument("No compatible material for physical tool " + std::to_string(physical));
                            filament_name = replacement->name;
                        }
                    }
                }
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
            const auto tools = tool_config(active_printer.config);
            if (Slic3r::has_fiber_tool(tools)) {
                filament_names.resize(tools.nozzle_diameter.size());
                for (size_t physical = 0; physical < filament_names.size(); ++physical) {
                    const auto accepts = [&](const Slic3r::Preset& preset) {
                        return preset.is_visible && preset.is_compatible &&
                            Slic3r::tool_accepts_process(tools, unsigned(physical), preset_process(preset));
                    };
                    const auto* preferred = selected.filaments.find_preset(filament_names[physical], false);
                    if (!preferred || !accepts(*preferred)) {
                        const auto replacement = std::find_if(selected.filaments.begin(), selected.filaments.end(), accepts);
                        if (replacement == selected.filaments.end())
                            throw std::invalid_argument("No compatible material for physical tool " + std::to_string(physical));
                        filament_names[physical] = replacement->name;
                    }
                }
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
        const auto& active_filaments = active->selection.filament_preset_ids;
        const bool keep_edited_filament = selected.filaments.get_selected_preset_name() == filament_names.front() &&
            (keep_compatible ? !active_filaments.empty() && active_filaments.front() == filament_names.front()
                             : keep_edited_process && filament_names == active_filaments);
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

        auto full_config = selected.full_config();
        auto physical_tools = std::move(remembered_tools);
        const auto tools = tool_config(full_config);
        if (switch_bundle && !physical_tools.empty() &&
            physical_tools.size() != result.selection.filament_preset_ids.size()) {
            physical_tools.resize(std::min(physical_tools.size(), result.selection.filament_preset_ids.size()));
            for (std::size_t slot = physical_tools.size(); slot < result.selection.filament_preset_ids.size(); ++slot)
                physical_tools.push_back(unsigned(std::min(slot, tools.nozzle_diameter.size() - 1)));
        }
        if (physical_tools.empty() && same_active_machine && active->config &&
            result.selection.filament_preset_ids.size() == active->selection.filament_preset_ids.size()) {
            const auto previous = dynamic_config(active->config->snapshot());
            const auto previous_tools = filament_tool_map(&previous, result.selection.filament_preset_ids.size());
            physical_tools.assign(previous_tools.begin(), previous_tools.end());
        }
        if (physical_tools.empty() && !tools.toolhead_filament_capacity.values.empty()) {
            // A new machine starts with one slot for each physical tool, in
            // machine-declared order. Additional slots require an explicit owner.
            if (result.selection.filament_preset_ids.size() != tools.nozzle_diameter.size())
                throw std::invalid_argument("Specify a physical tool for each material slot");
            for (size_t physical = 0; physical < tools.nozzle_diameter.size(); ++physical)
                physical_tools.push_back(unsigned(physical));
        }
        if (!physical_tools.empty()) {
            if (physical_tools.size() != result.selection.filament_preset_ids.size())
                throw std::invalid_argument("Physical tool count must match material slot count");
            std::vector<int> mapping;
            for (unsigned physical : physical_tools) {
                const auto& physical_map = tools.physical_extruder_map.values;
                const auto logical = std::find(physical_map.begin(), physical_map.end(), int(physical));
                if (physical >= tools.nozzle_diameter.size() || logical == physical_map.end() ||
                    std::count(physical_map.begin(), physical_map.end(), int(physical)) != 1)
                    throw std::invalid_argument("Invalid physical tool owner: " + std::to_string(physical));
                mapping.push_back(int(logical - physical_map.begin()) + 1);
            }
            // Construct variant-dependent material values with the requested
            // owners, rather than changing the map after preset resolution.
            full_config = selected.full_config(true, mapping);
        }
        if (!tools.toolhead_filament_capacity.values.empty())
            Slic3r::validate_material_tool_bindings(tool_config(full_config));
        const auto resolved_tools = filament_tool_map(&full_config, result.selection.filament_preset_ids.size());
        result.selection.filament_physical_tools.assign(resolved_tools.begin(), resolved_tools.end());
        result.config = std::unique_ptr<Config>(new Config(serialized_values(full_config)));
        if (switch_bundle) *switch_bundle = std::make_shared<Slic3r::PresetBundle>(std::move(selected));
        result.success = true;
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"selection", error.what()});
    }
    return result;
}

void Library::Impl::load_user_presets(Slic3r::PresetBundle& bundle, std::string_view vendor,
                                      std::vector<ConfigDiagnostic>* diagnostics) const
{
    if (user_preset_directory.empty() || vendor.empty()) return;
    const auto directory = user_preset_directory / std::string(vendor);
    Slic3r::PresetsConfigSubstitutions substitutions;
    for (const auto& [collection, section] : {
        std::pair<Slic3r::PresetCollection*, const char*>{&bundle.printers, PRESET_PRINTER_NAME},
        {&bundle.prints, PRESET_PRINT_NAME}, {&bundle.filaments, PRESET_FILAMENT_NAME}}) {
        try {
            if (fs::is_directory(directory / section))
                collection->load_presets(directory.string(), section, substitutions,
                    Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent, nullptr, {}, true);
        } catch (const std::runtime_error& error) {
            append_diagnostic(diagnostics, "user_presets", error.what());
        }
    }
}

std::string Library::Impl::vendor_for(std::string_view model, std::string_view variant) const
{
    for (const auto& machine : machines) {
        if (machine.id == model && std::any_of(machine.variants.begin(), machine.variants.end(),
                [variant](const auto& item) { return item.id == variant; }))
            return machine.vendor_id;
    }
    return {};
}

Slic3r::PresetBundle* Library::Impl::installed_presets(const std::string& model, const std::string& variant) const
{
    for (const auto& bundle : vendor_presets)
        if (bundle->printers.find_system_preset_by_model_and_variant(model, variant))
            return bundle.get();
    return nullptr;
}

std::vector<PresetOption> Library::printer_presets(std::string_view model, std::string_view variant) const
{
    const auto* bundle = impl_->installed_presets(std::string(model), std::string(variant));
    if (!bundle) return {};
    const auto* system = bundle->printers.find_system_preset_by_model_and_variant(
        std::string(model), std::string(variant));
    std::vector<PresetOption> presets{{system->name, system->name, true}};
    for (const auto& preset : bundle->printers)
        if (preset.is_user() && preset.is_visible && preset.config.opt_string("printer_model") == model &&
            preset.config.opt_string("printer_variant") == variant)
            presets.push_back({preset.name, preset.name, false});
    return presets;
}

namespace {
using SwitchPreset = PrinterSwitchPreset;
using SwitchAction = PrinterSwitchAction;
constexpr std::array switch_preset_order{SwitchPreset::Printer, SwitchPreset::Process, SwitchPreset::Filament};

Slic3r::PresetCollection& switch_collection(Slic3r::PresetBundle& bundle, SwitchPreset type)
{
    if (type == SwitchPreset::Printer) return bundle.printers;
    if (type == SwitchPreset::Process) return bundle.prints;
    return bundle.filaments;
}

std::string switch_option_value(const Slic3r::DynamicPrintConfig& config, const std::string& key)
{
    const auto index = key.find('#');
    const auto* option = config.option(key.substr(0, index));
    if (!option) return {};
    if (index == std::string::npos) return option->serialize();
    const auto* vector = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(option);
    if (!vector) return {};
    const auto values = vector->vserialize();
    const auto slot = std::stoul(key.substr(index + 1));
    return slot < values.size() ? values[slot] : std::string{};
}

std::vector<PrinterSwitchReview::Preset> native_switch_differences(
    Slic3r::PresetBundle& source, const Slic3r::PresetWithVendorProfile& new_printer)
{
    const auto old_printer = source.printers.get_edited_preset_with_vendor_profile();
    const auto& old_variants = old_printer.preset.config.option<Slic3r::ConfigOptionStrings>(
        "printer_extruder_variant")->values;
    const auto& new_variants = new_printer.preset.config.option<Slic3r::ConfigOptionStrings>(
        "printer_extruder_variant")->values;
    const bool different_variant = old_variants != new_variants;
    const bool same_technology = old_printer.preset.printer_technology() == new_printer.preset.printer_technology();

    std::vector<PrinterSwitchReview::Preset> result;
    for (const auto type : switch_preset_order) {
        auto& presets = switch_collection(source, type);
        if (!presets.current_is_dirty()) continue;
        const auto& edited = presets.get_edited_preset();
        const auto& selected = presets.get_selected_preset();
        const bool compatible = type != SwitchPreset::Printer && same_technology &&
            Slic3r::is_compatible_with_printer(presets.get_edited_preset_with_vendor_profile(), new_printer);
        PrinterSwitchReview::Preset review{
            type, edited.name, type != SwitchPreset::Printer && same_technology,
            !compatible, !edited.can_overwrite() || edited.is_project_embedded, {}};
        if (type == SwitchPreset::Printer) {
            const auto old_count = selected.config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->size();
            const auto edited_count = edited.config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter")->size();
            if (old_count != edited_count)
                review.changes.push_back({"extruders_count", std::to_string(old_count), std::to_string(edited_count), false});
        }
        for (const auto& key : presets.current_dirty_options(type != SwitchPreset::Process)) {
            const auto index = key.find('#');
            const auto base = key.substr(0, index);
            // Orca's settings search does not register the process extruder metadata.
            if (base == "print_extruder_id" || base == "print_extruder_variant") continue;
            const auto* def = Slic3r::print_config_def.get(base);
            if (!def || (def->label.empty() && def->full_label.empty())) continue;
            // TabFilament registers variant-specific options as indexed keys
            // only. Tab::may_discard_current_dirty_preset drops those keys
            // across extruder variants even when the target has that index.
            const bool variant_specific = type == SwitchPreset::Filament &&
                index != std::string::npos && Slic3r::filament_options_with_variant.count(base) != 0;
            const bool transferable = review.transfer_allowed && !(different_variant && variant_specific);
            auto before = switch_option_value(selected.config, key);
            auto after = switch_option_value(edited.config, key);
            if (before != after)
                review.changes.push_back({key, std::move(before), std::move(after), transferable});
        }
        if (!review.changes.empty())
            result.push_back(std::move(review));
    }
    return result;
}
// Activate a material in a temporary native bundle. Preserve the live working
// values while presenting its retained saved definition to Orca's dirty/save code.
Slic3r::PresetBundle filament_edit_bundle(const Slic3r::PresetBundle& source,
    const std::map<std::string, Slic3r::DynamicPrintConfig>& saved, const std::string& id)
{
    if (std::find(source.filament_presets.begin(), source.filament_presets.end(), id) == source.filament_presets.end())
        throw std::invalid_argument("The material preset is no longer referenced");
    const auto* live = source.filaments.find_preset(id, false);
    if (!live) throw std::invalid_argument("The material preset is missing");
    const auto current = live->config;
    auto bundle = source;
    auto& presets = bundle.filaments;
    presets.select_preset_by_name(id, true);
    if (presets.get_selected_preset_name() != id)
        throw std::invalid_argument("The material preset is missing");
    if (const auto original = saved.find(id); original != saved.end())
        presets.get_selected_preset().config = original->second;
    presets.discard_current_changes();
    presets.get_edited_preset().config = current;
    presets.update_dirty();
    return bundle;
}

std::vector<PrinterSwitchReview::Preset> switch_differences(Slic3r::PresetBundle& source,
    const Slic3r::PresetWithVendorProfile& target,
    const std::map<std::string, Slic3r::DynamicPrintConfig>& saved)
{
    auto selected = filament_edit_bundle(source, saved, source.filaments.get_selected_preset_name());
    auto result = native_switch_differences(selected, target);
    std::set<std::string> seen{source.filaments.get_selected_preset_name()};
    for (const auto& id : source.filament_presets) {
        if (!seen.insert(id).second || !saved.count(id)) continue;
        auto material = filament_edit_bundle(source, saved, id);
        for (auto& review : native_switch_differences(material, target)) {
            if (review.type != SwitchPreset::Filament) continue;
            // Orca transfers the current material to the target current material.
            // Secondary identities have no such target; never invent a slot map.
            review.transfer_allowed = false;
            review.cancel_switch = true;
            for (auto& change : review.changes) change.transferable = false;
            result.push_back(std::move(review));
        }
    }
    return result;
}
} // namespace

PrinterSwitchReview Library::review_printer_switch(const ConfigSelection& selection, bool after_save) const
{
    PrinterSwitchReview result;
    result.revision = impl_->active_revision;
    const auto* catalog = impl_->installed_presets(selection.machine_model_id, selection.machine_variant_id);
    if (!catalog) {
        result.diagnostics.push_back({"machine", "Unknown machine model or nozzle variant"});
        return result;
    }
    const auto& printers = catalog->printers;
    const auto* target = requested_printer(printers, selection);
    if (!target) {
        result.diagnostics.push_back({"printer", "Unknown printer preset: " + selection.printer_preset_id});
        return result;
    }
    const auto& current = impl_->active->selection;
    if (impl_->active->bundle && (after_save || target->name != current.printer_preset_id ||
        selection.machine_model_id != current.machine_model_id || selection.machine_variant_id != current.machine_variant_id)) {
        try {
            result.presets = switch_differences(*impl_->active->bundle, printers.get_preset_with_vendor_profile(*target), impl_->active->filament_saved_configs);
        } catch (const std::exception& error) {
            result.diagnostics.push_back({"printer_switch", error.what()});
        }
    }
    return result;
}

PrinterSwitchSaveResult Library::save_printer_switch_preset(
    std::uint64_t expected_revision, PrinterSwitchPreset type, std::string_view save_name,
    bool validate_only, std::string_view source_preset_id)
{
    PrinterSwitchSaveResult result;
    try {
        if (expected_revision != impl_->active_revision || !impl_->active->bundle)
            throw std::runtime_error("The configuration changed before saving the preset");
        const auto& selection = impl_->active->selection;
        const auto vendor = impl_->vendor_for(selection.machine_model_id, selection.machine_variant_id);
        if (vendor.empty())
            throw std::runtime_error("The project printer has no installed base for a user preset");
        if (impl_->user_preset_directory.empty())
            throw std::runtime_error("A writable user preset directory is required");

        auto next = std::make_shared<ConfigurationState::Impl>(*impl_->active);
        if (!source_preset_id.empty()) {
            if (type != SwitchPreset::Filament) {
                if (switch_collection(*next->bundle, type).get_selected_preset_name() != source_preset_id)
                    throw std::invalid_argument("The preset is no longer selected");
            } else {
                next->bundle = std::make_shared<Slic3r::PresetBundle>(filament_edit_bundle(
                    *next->bundle, next->filament_saved_configs, std::string(source_preset_id)));
            }
        }
        auto& source = switch_collection(*next->bundle, type);
        const auto source_name = source.get_selected_preset_name();
        const auto& edited = source.get_edited_preset();
        if (!source.current_is_dirty())
            throw std::invalid_argument("The preset has no unsaved changes");
        const bool save_as = !edited.can_overwrite() || edited.is_project_embedded;
        const std::string name = save_as ? std::string(save_name) : edited.name;
        if (name.empty() || name.front() == ' ' || name.back() == ' ' ||
            name.find_first_of("<>[]:\"/\\|?*\r\n\t") != std::string::npos ||
            name.find(Slic3r::PresetCollection::get_suffix_modified()) != std::string::npos ||
            name == "." || name == ".." || name == "Default Printer" ||
            name == "Default Setting" || name == Slic3r::PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER ||
            source.get_preset_name_by_alias(name) != name)
            throw std::invalid_argument("Invalid preset name");
        const auto* collision = source.find_preset(name, false);
        if (collision && !collision->can_overwrite())
            throw std::invalid_argument("Overwriting a system profile is not allowed");
        if (collision && name != source.get_selected_preset_name())
            result.overwrite_warning = collision->is_compatible
                ? "Saving will overwrite this user preset."
                : "Saving will overwrite this user preset, which is incompatible with the current printer.";
        if (validate_only) return result;

        auto& bundle = *next->bundle;
        auto& collection = switch_collection(bundle, type);
        const auto user_directory = impl_->user_preset_directory / vendor;
        const char* section = type == SwitchPreset::Printer ? PRESET_PRINTER_NAME
            : type == SwitchPreset::Process ? PRESET_PRINT_NAME : PRESET_FILAMENT_NAME;
        fs::create_directories(user_directory);
        collection.update_user_presets_directory(user_directory.string(), section);
        if (!collision) {
            const auto bare_name = Slic3r::get_preset_bare_name(name);
            if (fs::exists(collection.path_from_name(bare_name)) ||
                fs::exists(collection.path_from_name(bare_name, true)))
                throw std::invalid_argument("A preset with this name already exists");
        } else {
            auto* existing = collection.find_preset(name, false, true);
            const auto path = fs::u8path(collection.path_for_preset(*existing));
            if (fs::is_regular_file(path)) {
                // Orca's writer does not report a failed stream open.
                if (!std::ofstream(path, std::ios::app))
                    throw std::runtime_error("Could not save user preset: " + name);
                existing->file = path.u8string();
            }
        }

        bundle.save_changes_for_preset(name, collection.type(), {});
        const auto& saved = collection.get_selected_preset();
        if (saved.name != name || !saved.is_user() || !fs::is_regular_file(saved.file))
            throw std::runtime_error("Could not save user preset: " + name);
        std::ifstream written(fs::u8path(saved.file));
        if (ProjectJson::parse(written, nullptr, false).is_discarded())
            throw std::runtime_error("Could not save user preset: " + name);
        auto* catalog = impl_->installed_presets(selection.machine_model_id, selection.machine_variant_id);
        auto& catalog_collection = switch_collection(*catalog, type);
        auto& installed = catalog_collection.load_preset(saved.file, saved.name, saved.config, false, saved.version);
        installed = saved;
        if (installed.inherits().empty()) {
            for (auto& child : catalog_collection)
                if (child.inherits() == installed.name) child.reload(installed);
        }
        if (type == SwitchPreset::Filament) {
            // The native GUI helper updates slot 0. Rebind only references to
            // the material actually saved, retaining the user's current editor.
            bundle.filament_presets = impl_->active->bundle->filament_presets;
            for (auto& id : bundle.filament_presets) if (id == source_name) id = saved.name;
            next->filament_saved_configs.erase(source_name);
            next->filament_saved_configs.erase(saved.name);
            const auto& previous = impl_->active->bundle->filaments;
            if (previous.get_selected_preset_name() != source_name) {
                collection.select_preset_by_name(previous.get_selected_preset_name(), true);
                if (previous.get_selected_preset_name() != saved.name) {
                    collection.get_edited_preset().config = previous.get_edited_preset().config;
                    collection.update_dirty();
                }
            }
        }
        // Save selects the new preset in the source immediately, even when
        // a later preset dialog cancels the switch.
        auto full = dynamic_config(next->config->snapshot());
        const auto* map = full.option<Slic3r::ConfigOptionInts>("filament_map");
        const auto composed = bundle.full_config(true, map ? std::optional(map->values) : std::nullopt);
        if (type == SwitchPreset::Filament)
            full.apply_only(composed, Slic3r::Preset::filament_options(), true);
        full.apply_only(composed,
            {"printer_settings_id", "print_settings_id", "filament_settings_id",
             "inherits_group", "different_settings_to_system"});
        next->config.reset(new Config(serialized_values(full)));
        impl_->prepare(next, false);
        result.view = next->prepared_view;
        result.view.revision = ++impl_->active_revision;
        impl_->active = std::move(next);
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"preset", error.what()});
    }
    return result;
}

PrinterSwitchResult Library::prepare_printer_switch(
    const ConfigSelection& selection, std::uint64_t expected_revision,
    const std::vector<PrinterSwitchDecision>& decisions,
    const std::vector<std::pair<std::string, std::string>>& remembered_project_settings)
{
    PrinterSwitchResult result;
    try {
        if (expected_revision != impl_->active_revision)
            throw std::runtime_error("The configuration changed before the printer switch");

        std::shared_ptr<Slic3r::PresetBundle> target;
        auto resolved = impl_->resolve_config(selection, &target);
        if (!resolved) {
            result.diagnostics = std::move(resolved.diagnostics);
            return result;
        }
        auto source = impl_->active->bundle;
        const auto& current = impl_->active->selection;
        const auto changes = source && (!decisions.empty() ||
            resolved.selection.printer_preset_id != current.printer_preset_id ||
            resolved.selection.machine_model_id != current.machine_model_id ||
            resolved.selection.machine_variant_id != current.machine_variant_id)
            ? switch_differences(*source, target->printers.get_edited_preset_with_vendor_profile(), impl_->active->filament_saved_configs)
            : std::vector<PrinterSwitchReview::Preset>{};
        if (changes.size() != decisions.size())
            throw std::invalid_argument("The printer switch decisions are incomplete");


        for (std::size_t i = 0; i < changes.size(); ++i) {
            const auto& change = changes[i];
            const auto& decision = decisions[i];
            if (decision.type != change.type)
                throw std::invalid_argument("Unexpected preset decision");
            auto& from = switch_collection(*source, change.type);
            if (decision.action == SwitchAction::Cancel && change.cancel_switch)
                throw std::invalid_argument("Printer switch cancelled");
            if (decision.action == SwitchAction::Transfer) {
                if (!change.transfer_allowed)
                    throw std::invalid_argument("This preset cannot be transferred");
                auto& to = switch_collection(*target, change.type);
                std::vector<std::string> keys;
                for (const auto& item : change.changes)
                    if (item.transferable) keys.push_back(item.key);
                to.get_edited_preset().config.apply_only(from.get_edited_preset().config, keys);
                to.update_dirty();
            }
        }

        const auto resolved_full = dynamic_config(resolved.config->snapshot());
        const auto* map = resolved_full.option<Slic3r::ConfigOptionInts>("filament_map");
        auto& project = target->project_config;
        const auto bed = target->printers.get_edited_preset().get_default_bed_type(target.get());
        project.set_key_value("curr_bed_type", new Slic3r::ConfigOptionEnum<Slic3r::BedType>(bed));
        const std::vector<std::string> purge_keys{"flush_volumes_matrix", "flush_volumes_vector", "flush_multiplier"};
        project.apply_only(resolved_full, purge_keys);
        project.apply_only(resolved_full, {"filament_colour", "filament_multi_colour", "filament_colour_type"});
        // A fixed-slot machine's palette initializes its project slot colors.
        // Remembered project colors are restored below.
        const auto* tool_colors = resolved_full.option<Slic3r::ConfigOptionStrings>("extruder_colour");
        const auto* nozzles = resolved_full.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
        const bool seed_palette = fixed_physical_filament_slots(resolved_full) && map && tool_colors && nozzles &&
            tool_colors->size() == nozzles->size() &&
            map->size() == resolved.selection.filament_preset_ids.size() &&
            std::all_of(map->values.begin(), map->values.end(), [&](int tool) {
                return tool > 0 && std::size_t(tool) <= tool_colors->size() &&
                    !tool_colors->values[tool - 1].empty();
            });
        if (seed_palette) {
            auto& colors = project.option<Slic3r::ConfigOptionStrings>("filament_colour")->values;
            colors.clear();
            for (int tool : map->values)
                colors.push_back(tool_colors->values[tool - 1]);
            project.option<Slic3r::ConfigOptionStrings>("filament_colour_type")->values.assign(colors.size(), "1");
        }
        bool restored_multi_color = false;
        for (const auto& [key, value] : remembered_project_settings) {
            if (key != "curr_bed_type" && key != "filament_colour" && key != "filament_multi_colour" &&
                key != "filament_colour_type" && key != "flush_volumes_matrix" &&
                key != "flush_volumes_vector" && key != "flush_multiplier")
                throw std::invalid_argument("Unexpected remembered printer setting: " + key);
            // A malformed stored field should not discard the other memory.
            // Parse it separately so a failed vector parse cannot alter the
            // destination's defaults.
            Slic3r::DynamicPrintConfig field;
            try {
                field.set_deserialize_strict(key, value);
            } catch (const Slic3r::ConfigurationError&) {
                continue;
            }
            project.apply_only(field, {key});
            if (key == "filament_multi_colour")
                restored_multi_color = true;
        }
        if (seed_palette && !restored_multi_color)
            project.option<Slic3r::ConfigOptionStrings>("filament_multi_colour")->values =
                project.option<Slic3r::ConfigOptionStrings>("filament_colour")->values;
        const auto heads = project.option<Slic3r::ConfigOptionFloats>("flush_multiplier")->size();
        const auto matrix = project.option<Slic3r::ConfigOptionFloats>("flush_volumes_matrix")->size();
        const auto slots = heads ? std::size_t(std::sqrt(double(matrix) / heads)) : 0;
        const auto vector_size = project.option<Slic3r::ConfigOptionFloats>("flush_volumes_vector")->size();
        if (slots == 0 || matrix != slots * slots * heads || vector_size % 2 != 0)
            project.apply_only(resolved_full, purge_keys);
        target->update_multi_material_filament_presets();
        auto full = target->full_config(true, map ? std::optional(map->values) : std::nullopt);

        auto candidate = std::make_shared<ConfigurationState::Impl>();
        candidate->config = std::unique_ptr<Config>(new Config(serialized_values(full)));
        const auto count = target->filament_presets.size();
        auto normalized = normalize_filament_colors(*candidate->config, count, {});
        if (!normalized) {
            result.diagnostics = std::move(normalized.diagnostics);
            return result;
        }
        result.diagnostics = candidate->config->validate();
        auto cardinality = filament_cardinality_diagnostics(*candidate->config, count);
        result.diagnostics.insert(result.diagnostics.end(), cardinality.begin(), cardinality.end());
        if (!result.diagnostics.empty()) return result;
        candidate->selection = std::move(resolved.selection);
        candidate->bundle = std::move(target);
        impl_->prepare(candidate, false);

        result.configuration.impl_ = std::move(candidate);
        result.configuration.captured_revision_ = expected_revision;
        result.configuration.capture_origin_ = impl_->capture_origin;
        result.configuration.filament_color_cursor_ = impl_->filament_color_cursor;
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"printer_switch", error.what()});
    }
    return result;
}

ConfigActivationResult Library::activate_config(
    const ConfigSelection& selection,
    const std::vector<std::pair<std::string, std::string>>& patch)
{
    ConfigActivationResult result;
    const bool same_printer = selection.machine_model_id == impl_->active->selection.machine_model_id &&
        selection.machine_variant_id == impl_->active->selection.machine_variant_id;
    const std::vector<std::string> previous_colors = same_printer && impl_->active->config
        ? config_vector_values(*impl_->active->config, "filament_colour")
        : std::vector<std::string>{};
    auto created = impl_->resolve_config(selection);
    if (!created) {
        result.diagnostics = std::move(created.diagnostics);
        return result;
    }

    if (impl_->active->config &&
        created.selection.machine_model_id == impl_->active->selection.machine_model_id &&
        created.selection.machine_variant_id == impl_->active->selection.machine_variant_id) {
        const auto previous = dynamic_config(impl_->active->config->snapshot());
        auto candidate = dynamic_config(created.config->snapshot());
        candidate.apply_only(previous, {"wipe_tower_position_mode", "wipe_tower_x", "wipe_tower_y"});
        if (selection.process_preset_id == impl_->active->selection.process_preset_id) {
            for (const auto* keys : {&Slic3r::Preset::print_options(), &Slic3r::Preset::printer_options()})
                for (const auto& key : *keys)
                    if (const auto* value = previous.option(key)) candidate.set_key_value(key, value->clone());
            const auto& old_ids = impl_->active->selection.filament_preset_ids;
            const auto& new_ids = created.selection.filament_preset_ids;
            for (const auto& key : Slic3r::Preset::filament_options()) {
                const auto* old_value = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(previous.option(key));
                auto* new_value = dynamic_cast<Slic3r::ConfigOptionVectorBase*>(candidate.option(key));
                if (!old_value || !new_value || old_value->size() != old_ids.size() || new_value->size() != new_ids.size()) continue;
                for (size_t slot = 0; slot < std::min(old_ids.size(), new_ids.size()); ++slot)
                    if (old_ids[slot] == new_ids[slot]) new_value->set_at(old_value, slot, slot);
            }
        }
        created.config = std::unique_ptr<Config>(new Config(serialized_values(candidate)));
    }

    const bool patch_supplies_colors = std::any_of(
        patch.begin(), patch.end(), [](const auto& entry) {
            return entry.first == "filament_colour";
        });
    if (!patch.empty()) {
        const auto tools = tool_config(dynamic_config(created.config->snapshot()));
        if (!tools.toolhead_filament_capacity.values.empty()) {
            for (const auto& entry : patch) if (tool_owned_setting(entry.first)) {
                result.diagnostics.push_back({entry.first, "Material ownership and tool capabilities are managed by the machine and material selection"});
                return result;
            }
        }
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

    auto next_state = std::make_shared<ConfigurationState::Impl>(*impl_->active);
    next_state->config = std::move(created.config);
    next_state->selection = std::move(created.selection);
    next_state->compatible_processes = std::move(created.compatible_processes);
    next_state->compatible_filaments = std::move(created.compatible_filaments);
    const bool same_context = next_state->bundle && next_state->selection.machine_model_id == impl_->active->selection.machine_model_id && next_state->selection.machine_variant_id == impl_->active->selection.machine_variant_id;
    if (!same_context) {
        next_state->bundle.reset();next_state->machine.project_local=false;
        next_state->filament_saved_configs.clear();
        if (const auto* catalog = impl_->installed_presets(next_state->selection.machine_model_id,
                                                           next_state->selection.machine_variant_id))
            next_state->bundle = std::make_shared<Slic3r::PresetBundle>(*catalog);
    }
    try {
        impl_->prepare(next_state);
    } catch(const std::exception& error) {
        result.diagnostics.push_back({"configuration",error.what()});
        return result;
    }
    impl_->active = std::move(next_state);
    ++impl_->active_revision;
    result.success = true;
    result.view = *active_config();
    return result;
}

std::optional<ActiveConfigView> Library::active_config() const
{
    if (!impl_->active->config || impl_->active_revision == 0) {
        return std::nullopt;
    }
    ActiveConfigView view;
    view.revision = impl_->active_revision;
    view.selection = impl_->active->selection;
    view.compatible_processes = impl_->active->compatible_processes;
    view.compatible_filaments = impl_->active->compatible_filaments;
    view.settings = impl_->active->config->settings();
    view.selection.filament_physical_tools.clear();

    const Slic3r::DynamicPrintConfig config = dynamic_config(impl_->active->config->snapshot());
    view.modified_process_keys = config.diff(impl_->restored_config(*impl_->active, config, SettingGroup::Process));
    view.printer_preset_modified = !config.diff(impl_->restored_config(*impl_->active, config, SettingGroup::Printer)).empty();
    view.filament_slots=project_filament_slots(config,view.selection.filament_preset_ids.size());
    for (std::size_t index = 0; index < view.filament_slots.size(); ++index) {
        auto& slot=view.filament_slots[index];
        slot.preset_id=view.selection.filament_preset_ids[index];
        slot.preset_modified = !config.diff(impl_->restored_config(*impl_->active, config, SettingGroup::Filament, index)).empty();
        const auto preset=std::find_if(view.compatible_filaments.begin(),view.compatible_filaments.end(),
            [&slot](const PresetOption& option){return option.id==slot.preset_id;});
        slot.preset_name=preset==view.compatible_filaments.end()?slot.preset_id:preset->name;
        view.selection.filament_physical_tools.push_back(unsigned(slot.physical_tool_index));
        const auto tools = tool_config(config);
        for (const auto& option : view.compatible_filaments) {
            if (impl_->active->bundle) {
                const auto* preset=impl_->active->bundle->filaments.find_preset(option.id,false);
                if(preset && Slic3r::tool_accepts_process(tools,unsigned(slot.physical_tool_index),preset_process(*preset))) slot.compatible_presets.push_back({option.id,option.name,option.id==slot.preset_id});
                continue;
            }
            for (const auto& bundle : impl_->vendor_presets) {
                if (!bundle->printers.find_system_preset_by_model_and_variant(
                        view.selection.machine_model_id, view.selection.machine_variant_id)) continue;
                const auto* preset = bundle->filaments.find_preset(option.id, false);
                if (preset && Slic3r::tool_accepts_process(tools, unsigned(slot.physical_tool_index), preset_process(*preset)))
                    slot.compatible_presets.push_back({option.id, option.name, option.id == slot.preset_id});
                break;
            }
        }
    }
    return view;
}

std::optional<ConfigSnapshot> Library::active_config_snapshot() const
{
    return impl_->active->config
        ? std::optional<ConfigSnapshot>{impl_->active->config->snapshot()}
        : std::nullopt;
}

SettingsResult Library::apply_active_config_patch(
    const std::vector<std::pair<std::string, std::string>>& patch)
{
    if (!impl_->active->config) {
        return settings_failure("configuration", "No slicing configuration is active");
    }
    if (!tool_config(dynamic_config(impl_->active->config->snapshot())).toolhead_filament_capacity.values.empty()) {
        for (const auto& entry : patch) if (tool_owned_setting(entry.first))
            return settings_failure(entry.first, "Use material selection to change a material; physical tool ownership is preserved");
    }
    if (fixed_physical_filament_slots(dynamic_config(impl_->active->config->snapshot()))) {
        const auto mapping_edit = std::find_if(patch.begin(), patch.end(), [](const auto& entry) {
            return entry.first == "filament_map" || entry.first == "filament_map_mode";
        });
        if (mapping_edit != patch.end()) {
            return settings_failure(mapping_edit->first,
                                    "Filament-to-tool mapping is fixed by the selected machine");
        }
    }
    Config candidate = *impl_->active->config;
    SettingsResult result = candidate.apply_patch(patch);
    if (!result) return result;
    SettingsResult normalized = normalize_filament_colors(
        candidate, impl_->active->selection.filament_preset_ids.size());
    if (!normalized) return normalized;
    append_changed_items(result.changed_items, std::move(normalized.changed_items));
    auto diagnostics = candidate.validate();
    auto cardinality = filament_cardinality_diagnostics(
        candidate, impl_->active->selection.filament_preset_ids.size());
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(cardinality.begin()),
                       std::make_move_iterator(cardinality.end()));
    if (!diagnostics.empty()) {
        result.success = false;
        result.diagnostics = std::move(diagnostics);
        result.changed_items.clear();
        return result;
    }
    auto next_state = std::make_shared<ConfigurationState::Impl>(*impl_->active);
    next_state->config = std::make_unique<Config>(std::move(candidate));
    try {
        impl_->prepare_preset_edit(next_state);
    } catch(const std::exception& error) {
        // Native preset splitting may reject structurally incomplete vectors.
        // Return an ordinary failed edit; never publish a partial candidate.
        return settings_failure("configuration",error.what());
    }
    impl_->active = std::move(next_state);
    ++impl_->active_revision;
    return result;
}

SettingsResult Library::set_active_config_value(std::string_view key, std::string_view value)
{
    return apply_active_config_patch({{std::string(key), std::string(value)}});
}

ConfigActivationResult Library::restore_active_preset(SettingGroup group, std::optional<std::size_t> filament_slot)
{
    ConfigActivationResult result;
    try {
        if (!impl_->active->config || !impl_->active->bundle)
            throw std::runtime_error("No slicing preset is active");
        if (group != SettingGroup::Process && group != SettingGroup::Filament && group != SettingGroup::Printer)
            throw std::invalid_argument("Unknown preset domain");
        const auto& ids = impl_->active->selection.filament_preset_ids;
        if ((group == SettingGroup::Filament && (!filament_slot || *filament_slot >= ids.size())) ||
            (group != SettingGroup::Filament && filament_slot))
            throw std::invalid_argument("Choose the material slot whose preset should be restored");

        const auto current = dynamic_config(impl_->active->config->snapshot());
        const auto full = impl_->restored_config(*impl_->active, current, group, filament_slot);
        auto next = std::make_shared<ConfigurationState::Impl>(*impl_->active);
        if (group == SettingGroup::Process) {
            next->bundle->prints.discard_current_changes();
        } else if (group == SettingGroup::Printer) {
            next->bundle->printers.discard_current_changes();
        } else {
            const auto& name = ids[*filament_slot];
            auto rollback = *next->bundle;
            auto& presets = rollback.filaments;
            presets.select_preset_by_name(name, true);
            if (presets.get_selected_preset_name() != name)
                throw std::runtime_error("The saved material preset is missing");
            const auto saved = next->filament_saved_configs.find(name);
            if (saved != next->filament_saved_configs.end())
                presets.get_selected_preset().config = saved->second;
            presets.discard_current_changes();
            auto* live = next->bundle->filaments.find_preset(name, false, true);
            live->config = presets.get_edited_preset().config;
            if (next->bundle->filaments.get_selected_preset_name() == name)
                next->bundle->filaments.discard_current_changes();
            next->filament_saved_configs.erase(name);
        }
        next->config = std::unique_ptr<Config>(new Config(serialized_values(full)));
        result.diagnostics = next->config->validate();
        auto cardinality = filament_cardinality_diagnostics(*next->config, ids.size());
        result.diagnostics.insert(result.diagnostics.end(), cardinality.begin(), cardinality.end());
        if (!result.diagnostics.empty()) return result;
        impl_->prepare(next, false);
        const bool changed = next->config->snapshot().values() != impl_->active->config->snapshot().values();
        if (!changed) {
            // A no-op must preserve snapshot identity as well as its revision.
            result.view = *active_config();
            result.success = true;
            return result;
        }
        const auto revision = impl_->active_revision + 1;
        result.view = next->prepared_view;
        result.view.revision = revision;
        impl_->active = std::move(next);
        impl_->active_revision = revision;
        result.success = true;
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"preset", error.what()});
    }
    return result;
}

SettingsResult Library::reset_active_config_value(std::string_view key)
{
    if (!impl_->active->config) {
        return settings_failure("configuration", "No slicing configuration is active");
    }
    if (tool_owned_setting(key) &&
        !tool_config(dynamic_config(impl_->active->config->snapshot())).toolhead_filament_capacity.values.empty())
        return settings_failure(std::string(key), "Cannot reset material ownership or physical tool capabilities");
    if ((key == "filament_map" || key == "filament_map_mode") &&
        fixed_physical_filament_slots(dynamic_config(impl_->active->config->snapshot()))) {
        return settings_failure(std::string(key),
                                "Filament-to-tool mapping is fixed by the selected machine");
    }
    Config candidate = *impl_->active->config;
    SettingsResult result = candidate.reset(key);
    if (!result) return result;
    SettingsResult normalized = normalize_filament_colors(
        candidate, impl_->active->selection.filament_preset_ids.size());
    if (!normalized) return normalized;
    append_changed_items(result.changed_items, std::move(normalized.changed_items));
    auto diagnostics = candidate.validate();
    auto cardinality = filament_cardinality_diagnostics(
        candidate, impl_->active->selection.filament_preset_ids.size());
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(cardinality.begin()),
                       std::make_move_iterator(cardinality.end()));
    if (!diagnostics.empty()) {
        result.success = false;
        result.diagnostics = std::move(diagnostics);
        result.changed_items.clear();
        return result;
    }
    auto next_state = std::make_shared<ConfigurationState::Impl>(*impl_->active);
    next_state->config = std::make_unique<Config>(std::move(candidate));
    try {
        impl_->prepare_preset_edit(next_state);
    } catch(const std::exception& error) {
        // Native preset splitting may reject structurally incomplete vectors.
        // Return an ordinary failed edit; never publish a partial candidate.
        return settings_failure("configuration",error.what());
    }
    impl_->active = std::move(next_state);
    ++impl_->active_revision;
    return result;
}

ConfigActivationResult Library::set_active_filament_preset(
    std::size_t slot_index, std::string_view preset_id)
{
    if (!impl_->active->config) {
        ConfigActivationResult result;
        result.diagnostics.push_back({"configuration", "No slicing configuration is active"});
        return result;
    }
    if (slot_index >= impl_->active->selection.filament_preset_ids.size()) {
        ConfigActivationResult result;
        result.diagnostics.push_back({"filament", "Filament slot index is out of range"});
        return result;
    }
    const auto current = active_config();
    const auto& allowed = current->filament_slots[slot_index].compatible_presets;
    if (std::none_of(allowed.begin(), allowed.end(), [preset_id](const PresetOption& option) { return option.id == preset_id; })) {
        ConfigActivationResult result;
        result.diagnostics.push_back({"filament", "Material is incompatible with this physical tool"});
        return result;
    }
    if (impl_->active->selection.filament_preset_ids[slot_index] == preset_id) {
        ConfigActivationResult result;
        result.success = true;
        result.view = *active_config();
        return result;
    }
    ConfigSelection selection;
    selection.machine_model_id = impl_->active->selection.machine_model_id;
    selection.machine_variant_id = impl_->active->selection.machine_variant_id;
    selection.process_preset_id = impl_->active->selection.process_preset_id;
    selection.filament_preset_ids = impl_->active->selection.filament_preset_ids;
    selection.filament_preset_ids[slot_index] = std::string(preset_id);
    return activate_config(selection);
}

ConfigActivationResult Library::resize_active_filament_slots(std::size_t slot_count)
{
    ConfigActivationResult result;
    if (!impl_->active->config) {
        result.diagnostics.push_back({"configuration", "No slicing configuration is active"});
        return result;
    }
    const auto model = std::find_if(
        impl_->machines.begin(), impl_->machines.end(), [this](const MachineModelOption& option) {
            return option.id == impl_->active->selection.machine_model_id;
        });
    // A project may reuse an installed preset with different effective tools.
    // Enforce the same prepared capacity that the caller can inspect.
    const MachineVariantOption* variant = impl_->active->machine.model.id.empty()
        ? nullptr : &impl_->active->machine.variant;
    if (variant == nullptr && model != impl_->machines.end()) {
        const auto found = std::find_if(
            model->variants.begin(), model->variants.end(), [this](const MachineVariantOption& option) {
                return option.id == impl_->active->selection.machine_variant_id;
            });
        if (found != model->variants.end()) variant = &*found;
    }
    if (variant == nullptr && impl_->active->bundle) variant = &impl_->active->machine.variant;
    if (variant == nullptr) {
        result.diagnostics.push_back({"machine", "The active machine variant is unavailable"});
        return result;
    }
    if (!variant->toolhead_filament_capacity.empty()) {
        result.diagnostics.push_back({"filament", "Add materials to an explicit physical tool; total slot count does not identify a tool"});
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
    selection.machine_model_id = impl_->active->selection.machine_model_id;
    selection.machine_variant_id = impl_->active->selection.machine_variant_id;
    selection.process_preset_id = impl_->active->selection.process_preset_id;
    selection.filament_preset_ids = impl_->active->selection.filament_preset_ids;
    if (selection.filament_preset_ids.empty()) {
        result.diagnostics.push_back({"filament", "The active configuration has no filament preset"});
        return result;
    }
    selection.filament_preset_ids.resize(slot_count, selection.filament_preset_ids.back());
    return activate_config(selection);
}

ConfigActivationResult Library::add_active_filament(std::size_t physical_tool, std::string_view preset_id)
{
    ConfigActivationResult result;
    const auto view = active_config();
    if (!view) {
        result.diagnostics.push_back({"configuration", "No slicing configuration is active"});
        return result;
    }
    const auto config = dynamic_config(impl_->active->config->snapshot());
    const auto tools = tool_config(config);
    const auto& capacities = tools.toolhead_filament_capacity.values;
    if (physical_tool >= capacities.size()) {
        result.diagnostics.push_back({"filament", "No material capacity declared for this physical tool"});
        return result;
    }
    const auto count = std::count_if(view->filament_slots.begin(), view->filament_slots.end(),
        [physical_tool](const FilamentSlotInfo& slot) { return slot.physical_tool_index == physical_tool; });
    if (count >= capacities[physical_tool]) {
        result.diagnostics.push_back({"filament", "Material capacity exceeded for physical tool " + std::to_string(physical_tool)});
        return result;
    }
    const auto existing = std::find_if(view->filament_slots.begin(), view->filament_slots.end(),
        [physical_tool](const FilamentSlotInfo& slot) { return slot.physical_tool_index == physical_tool; });
    // Each tool has an initial slot. Adding another defaults to its selected
    // material; the operation's target tool, not the material, owns the slot.
    if (existing == view->filament_slots.end()) {
        result.diagnostics.push_back({"filament", "Physical tool has no initialized material slot"});
        return result;
    }
    const std::string requested = preset_id.empty() ? existing->preset_id : std::string(preset_id);
    if (std::none_of(existing->compatible_presets.begin(), existing->compatible_presets.end(),
        [&requested](const PresetOption& option) { return option.id == requested; })) {
        result.diagnostics.push_back({"filament", "Material is incompatible with this physical tool"});
        return result;
    }
    ConfigSelection selection;
    selection.machine_model_id = view->selection.machine_model_id;
    selection.machine_variant_id = view->selection.machine_variant_id;
    selection.process_preset_id = view->selection.process_preset_id;
    selection.filament_preset_ids = view->selection.filament_preset_ids;
    for (const auto& slot : view->filament_slots) selection.filament_physical_tools.push_back(unsigned(slot.physical_tool_index));
    selection.filament_preset_ids.push_back(requested);
    selection.filament_physical_tools.push_back(unsigned(physical_tool));
    return activate_config(selection);
}

SettingsResult Library::set_active_filament_color(std::size_t slot_index, Rgba8 color)
{
    if (!impl_->active->config) {
        return settings_failure("configuration", "No slicing configuration is active");
    }
    const std::size_t slot_count = impl_->active->selection.filament_preset_ids.size();
    if (slot_index >= slot_count) {
        return settings_failure("filament", "Filament slot index is out of range");
    }
    std::vector<std::string> colors = config_vector_values(*impl_->active->config, "filament_colour");
    if (colors.empty()) colors.push_back("#00AE42");
    colors.resize(slot_count, colors.back());
    colors[slot_index] = serialized_color(color);
    return set_active_config_value("filament_colour", serialize_strings(colors));
}

std::vector<ConfigDiagnostic> Library::validate_active_config() const
{
    if (!impl_->active->config) {
        return {{"configuration", "No slicing configuration is active"}};
    }
    auto diagnostics = impl_->active->config->validate();
    auto cardinality = filament_cardinality_diagnostics(
        *impl_->active->config, impl_->active->selection.filament_preset_ids.size());
    diagnostics.insert(diagnostics.end(),
                       std::make_move_iterator(cardinality.begin()),
                       std::make_move_iterator(cardinality.end()));
    return diagnostics;
}

SliceResult Library::slice(const SliceRequest& request, const SliceCallbacks& callbacks) const
{
    return slice_impl(request, callbacks, nullptr, 0);
}

std::vector<CalibMode> Library::calibration_modes() const
{
    return {libslicer::CalibMode::Calib_PA_Line,
            libslicer::CalibMode::Calib_PA_Pattern,
            libslicer::CalibMode::Calib_PA_Tower,
            libslicer::CalibMode::Calib_Flow_Rate,
            libslicer::CalibMode::Calib_Temp_Tower,
            libslicer::CalibMode::Calib_Vol_speed_Tower,
            libslicer::CalibMode::Calib_VFA_Tower,
            libslicer::CalibMode::Calib_Retraction_tower,
            libslicer::CalibMode::Calib_Input_shaping_freq,
            libslicer::CalibMode::Calib_Input_shaping_damp,
            libslicer::CalibMode::Calib_Cornering,
            libslicer::CalibMode::Calib_Tolerance_Model};
}

CalibrationPrepareResult Library::prepare_calibration(const CalibrationRequest& request, const SliceCallbacks& callbacks) const
{
    CalibrationPrepareResult result;
    try {
        if (!request.config.valid())
            throw std::invalid_argument("Calibration has no configuration");
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }
        auto config = dynamic_config(request.config);
        Slic3r::set_resources_dir(impl_->resource_directory);
        const auto params                = orca::dialog_parameters(request, config);
        // Flow selects a particular material; the embedded-preset loader needs
        // complete per-variant identity metadata before splitting that material.
        // This repairs a private copy, never the caller's configuration.
        if (request.mode == libslicer::CalibMode::Calib_Flow_Rate) {
            normalize_filament_identity(config);
            // A request snapshot is authoritative. Orca's project loader would
            // otherwise replace values not marked dirty with vendor defaults
            // (including an already calibrated ratio or an edited nozzle).
            const auto count = config.option<Slic3r::ConfigOptionFloats>("filament_diameter")->size();
            std::vector<std::string> changed(count + 2,
                Slic3r::escape_strings_cstyle(Slic3r::Preset::filament_options()));
            changed.front() = Slic3r::escape_strings_cstyle(Slic3r::Preset::print_options());
            changed.back() = Slic3r::escape_strings_cstyle(Slic3r::Preset::printer_options());
            config.set_key_value("different_settings_to_system", new Slic3r::ConfigOptionStrings(std::move(changed)));
        }
        const Slic3r::PresetBundle* seed = nullptr;
        const auto model                 = config.opt_string("printer_model");
        for (const auto& candidate : impl_->vendor_presets)
            for (const auto& vendor : candidate->vendors)
                if (std::any_of(vendor.second.models.begin(), vendor.second.models.end(),
                                [&](const auto& item) { return item.id == model; }))
                    seed = candidate.get();
        if (impl_->active->bundle && model == impl_->active->selection.machine_model_id) seed=impl_->active->bundle.get();
        if (request.mode == libslicer::CalibMode::Calib_Flow_Rate) {
            const auto count = config.option<Slic3r::ConfigOptionFloats>("filament_diameter")->size();
            auto* names = config.option<Slic3r::ConfigOptionStrings>("filament_settings_id", true);
            names->values.resize(count, names->values.empty() ? std::string{} : names->values.front());
            const auto original_names = names->values;
            auto* inherits = config.option<Slic3r::ConfigOptionStrings>("inherits_group", true);
            inherits->values.resize(count + 2);
            // Orca has one edited copy per preset name. A slot matching the
            // vendor default would otherwise share that copy with another slot
            // whose flow was edited, losing one of the captured values. Give
            // repeated materials private identities before the original loader
            // splits them; request.config retains their real preset identities.
            for (std::size_t slot = 0; slot < count; ++slot) {
                if (std::count(original_names.begin(), original_names.end(), original_names[slot]) < 2)
                    continue;
                std::string name = "Flow calibration material " + std::to_string(slot + 1);
                while (std::find(names->values.begin(), names->values.end(), name) != names->values.end() ||
                       (seed && seed->filaments.find_preset(name, false)))
                    name += "_";
                names->values[slot] = std::move(name);
                inherits->values[slot + 1].clear();
            }
        }
        auto task      = std::unique_ptr<CalibrationTask>(new CalibrationTask());
        auto& native   = *task->impl_;
        native.request = request;
        native.host    = std::make_unique<orca::Plater>(config, seed);
        report_progress(callbacks, 0.1f, "Running original Plater calibration");
        if (request.mode == libslicer::CalibMode::Calib_Flow_Rate) {
            native.flow_target = orca::resolve_flow_target(config, request.flow_filament_slot);
            native.host->prepare_flow(request.flow_linear, request.flow_pass,
                                      request.flow_pattern == CalibrationRequest::FlowPattern::ArchimedeanChords ?
                                          Slic3r::ipArchimedeanChords :
                                          Slic3r::ipMonotonic, *native.flow_target);
        }
        else if (request.mode == libslicer::CalibMode::Calib_Tolerance_Model)
            native.host->prepare_tolerance();
        else
            native.host->prepare(params);
        for (const auto& message : native.host->notifications())
            result.diagnostics.push_back({"calibration_info", message, true});
        const auto warnings = orca::calibration_warnings(request, config);
        result.diagnostics.insert(result.diagnostics.end(), warnings.begin(), warnings.end());
        native.params   = native.host->print_params();
        const auto full = native.host->full_config();
        std::vector<std::pair<std::string, std::string>> values;
        for (const auto& key : full.keys())
            values.emplace_back(key, full.option(key)->serialize());
        const auto snapshot = Config(std::move(values)).snapshot();
        for (std::size_t i = 0; i < native.host->plate_count(); ++i) {
            if (cancellation_requested(callbacks)) {
                result.cancelled = true;
                return result;
            }
            native.models.push_back(native.host->plate_model(i));
            auto plate   = orca::describe_plate(native.models.back(), i, native.flow_target);
            plate.config = snapshot;
            native.plates.push_back(std::move(plate));
        }
        report_progress(callbacks, 1.f, "Original Plater preparation complete");
        result.task    = std::move(task);
        result.success = true;
    } catch (const std::exception& error) {
        result.diagnostics.push_back({"calibration", error.what(), false});
    }
    return result;
}

SliceResult Library::slice_calibration(const CalibrationTask& task,
                                       std::size_t plate_index,
                                       const CalibrationOutputOptions& output,
                                       const SliceCallbacks& callbacks) const
{
    if (plate_index >= task.impl_->plates.size()) {
        SliceResult result;
        result.diagnostics.push_back({"plate", "Calibration plate index is outside the task", false});
        return result;
    }
    SliceRequest request;
    request.config                = task.impl_->plates[plate_index].config;
    request.center_on_build_plate = false;
    request.output_gcode_path     = output.output_gcode_path;
    request.generate_preview      = output.generate_preview;
    request.generate_gcode_3mf    = output.generate_gcode_3mf;
    request.output_gcode_3mf_path = output.output_gcode_3mf_path;
    return slice_impl(request, callbacks, &task, plate_index);
}

SliceResult Library::slice_impl(const SliceRequest& request,
                                const SliceCallbacks& caller_callbacks,
                                const CalibrationTask* calibration,
                                std::size_t plate_index) const
{
    // Native percentages are stage markers and may repeat for each object or
    // during export. Keep one monotonic, serialized observer for this request,
    // without reordering native work or estimating elapsed/remaining time.
    std::mutex progress_mutex;
    float progress_high_water = 0.0f;
    SliceCallbacks callbacks = caller_callbacks;
    callbacks.progress_details = [&](const SliceProgress& progress) {
        std::lock_guard<std::mutex> lock(progress_mutex);
        progress_high_water = std::max(progress_high_water, progress.progress);
        report_progress(caller_callbacks, progress_high_water, progress.stage,
                        progress.completed_layers, progress.total_layers, progress.detail);
    };
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
        if (request.objects.empty() && calibration == nullptr) {
            result.diagnostics.push_back({"input", "Slice request contains no models", false});
            return result;
        }
        if (!request.config.valid()) {
            result.diagnostics.push_back({"config", "Slice request has no initialized configuration", false});
            return result;
        }
        // Keep the original native values: serializing a float-derived layer
        // height and parsing it back can move a layer across a calibration
        // boundary (for example floor(z - 0.4) in the retraction tower).
        Slic3r::DynamicPrintConfig config = calibration
            ? calibration->impl_->host->full_config() : dynamic_config(request.config);
        if (!calibration) normalize_filament_identity(config);
        Slic3r::Model plate_model;
        if (!calibration) apply_plate_semantics(plate_model, config, request.project_semantics);
        // Imported projects may intentionally retain editable invalid values.
        // Reject them before Print::validate builds layers (negative heights
        // can otherwise enter the native layer-generation loop).
        for (const auto& problem:config.validate())
            result.diagnostics.push_back({"config",problem.first+": "+problem.second,false});
        if (!result.diagnostics.empty()) return result;
        Slic3r::validate_material_tool_bindings(tool_config(config));
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
        if (calibration) plate_model=calibration->impl_->models[plate_index];
        std::set<size_t> mask_volume_ids;
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
            if (has_file_input && !input.fiber_masks.empty())
                throw std::invalid_argument("object=" + input.name + ": masks require in-memory volumes");
            Slic3r::ModelObject* support_target = nullptr;

            if (has_memory_input) {
                Slic3r::ModelObject* object = plate_model.add_object();
                object->name = input.name;
                apply_object_semantics(*object, input.project_semantics);
                const int object_slot = input.default_filament_slot > 0 ? input.default_filament_slot : request.default_filament_slot;
                if (object_slot <= 0 || std::size_t(object_slot) > filament_slot_count) throw std::invalid_argument("Object material slot outside active configuration");
                object->config.set("extruder", object_slot);
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

                    Slic3r::ModelVolumeType volume_type;
                    switch (source_volume.role) {
                    case SliceVolumeRole::ModelPart: volume_type=Slic3r::ModelVolumeType::MODEL_PART;break;
                    case SliceVolumeRole::SupportEnforcer: volume_type=Slic3r::ModelVolumeType::SUPPORT_ENFORCER;break;
                    case SliceVolumeRole::SupportBlocker: volume_type=Slic3r::ModelVolumeType::SUPPORT_BLOCKER;break;
                    case SliceVolumeRole::NegativeVolume: volume_type=Slic3r::ModelVolumeType::NEGATIVE_VOLUME;break;
                    case SliceVolumeRole::ParameterModifier: volume_type=Slic3r::ModelVolumeType::PARAMETER_MODIFIER;break;
                    default: throw std::invalid_argument("Unknown slice volume role");
                    }
                    Slic3r::ModelVolume* volume = object->add_volume(
                        Slic3r::TriangleMesh(std::move(vertices), std::move(faces)), volume_type, false);
                    volume->set_transformation(mask_matrix(source_volume.local_transform));
                    const auto semantics=semantic_json(source_volume.project_semantics,"part");
                    if (semantics.contains("settings") && semantics.at("settings").contains("extruder")) throw std::invalid_argument("Part material slot must use its dedicated field");
                    volume->config.apply(semantic_settings(semantics));
                    apply_part_annotations(*volume, source_volume.surface_annotations);
                    if (!source_volume.inherit_object_filament) {
                        if (source_volume.default_filament_slot <= 0 || std::size_t(source_volume.default_filament_slot)>filament_slot_count)
                            throw std::invalid_argument("Volume material slot outside active configuration");
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
                        auto painting=native_facets(source_volume.facet_labels,source_volume.triangles.size(),int(std::min(filament_slot_count,std::size_t(Slic3r::EnforcerBlockerType::ExtruderMax))));
                        volume->mmu_segmentation_facets.set_data(std::move(painting));
                    }
                }
                Slic3r::Transform3d transform = Slic3r::Transform3d::Identity();
                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        transform(row, column) = input.transform[static_cast<std::size_t>(row * 4 + column)];
                    }
                }
                std::set<std::string> mask_ids;
                for (const auto& mask : input.fiber_masks) {
                    try {
                        validate_mask_geometry(mask);
                        if (!mask_ids.insert(mask.id).second)
                            throw std::invalid_argument("Duplicate mask ID");
                        if (!mask.enabled) continue;
                        const auto patch = mask_process_patch(mask, config);
                        const auto parent = mask_matrix(input.transform);
                        auto mesh = make_mask_mesh(mask, parent, config.opt_float("resolution"));
                        // SDK volumes carry vertices in the common object frame. Native region
                        // broad-phase discards volume XY offsets, so bake the rigid local pose.
                        mesh.transform(mask_matrix(mask.mask_to_object));
                        auto* volume = object->add_volume(std::move(mesh),
                            Slic3r::ModelVolumeType::PARAMETER_MODIFIER, false);
                        volume->name = mask.id;
                        volume->config.apply(patch);
                        volume->config.set("fiber_mask_process", true);
                        mask_volume_ids.insert(volume->id().id);
                    } catch (const std::exception& error) {
                        throw std::invalid_argument("object=" + input.name + " mask=" + mask.id + ": " + error.what());
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

        const bool belt_printer = is_belt_config(config);
        if (request.center_on_build_plate) {
            if (belt_printer)
                center_belt_model_on_x(plate_model, build_plate_center(config).x());
            else
                plate_model.center_instances_around_point(build_plate_center(config));
        }

        report_progress(callbacks, 0.08f, "Validating print");
        Slic3r::Print print;
        // The SDK has one plate in request coordinates; native Print does not
        // initialize its plate origin, which validation and export both read.
        print.set_plate_origin(Slic3r::Vec3d::Zero());
        print.set_plate_index(plate_model.curr_plate_index);
        if (belt_printer) {
            const ModelWorldBounds belt_bounds = model_world_bounds(plate_model);
            if (!belt_bounds.valid) {
                result.diagnostics.push_back({"model", "Belt slicing found no printable model vertices", false});
                return result;
            }
            const auto* angle = config.option<Slic3r::ConfigOptionFloat>("belt_gantry_angle");
            const double angle_degrees = angle == nullptr ? 45.0 : angle->value;
            // Support trunks are rooted by a world-vertical projection. Their
            // build-plate Y is therefore the contact Y, not Y + Z*tan(alpha).
            // The real maximum model Y remains the authoritative task front.
            const double reference_world_y = belt_bounds.maximum.y();
            print.set_belt_coordinate_system(Slic3r::BeltCoordinateSystem::create(
                angle_degrees, reference_world_y));
        }
        const auto* printer_model = config.option<Slic3r::ConfigOptionString>("printer_model");
        print.is_BBL_printer() = printer_model != nullptr &&
            printer_model->value.rfind("Bambu Lab", 0) == 0;
        if (calibration) print.is_BBL_printer()=calibration->impl_->host->presets().is_bbl_vendor();
        print.set_status_callback([&](const Slic3r::PrintBase::SlicingStatus& status) {
            if (cancellation_requested(callbacks)) {
                print.cancel();
            }
            // Preserve native status/warning delivery. The per-request relay
            // holds the current percentage for notifications without a value.
            const float engine_progress = status.percent < 0 ? 0.0f : static_cast<float>(status.percent) / 100.0f;
            report_progress(callbacks, 0.08f + engine_progress * 0.78f, status.text,
                            status.completed_layers, status.total_layers, status.detail);
        });
        print.apply(plate_model, config);
        // The first native apply normalizes the tower before building regions.
        // A single volume can therefore look like one material even when its
        // walls and infill use different tools. Once regions exist, reapply the
        // requested config so the native extruder count decides tower usage.
        const auto* requested_tower = config.option<Slic3r::ConfigOptionBool>("enable_prime_tower");
        if (!calibration && requested_tower && requested_tower->value &&
            config.opt_enum<Slic3r::PrintSequence>("print_sequence") == Slic3r::PrintSequence::ByLayer &&
            !print.config().enable_prime_tower.value && print.extruders(true).size() > 1)
            print.apply(plate_model, config);
        bool tower_placement_failed = false;
        const std::size_t native_plate_index = print.get_plate_index();
        const auto& modes = print.config().wipe_tower_position_mode;
        const bool auto_tower = modes.values.empty() ||
            modes.get_at(native_plate_index) == int(Slic3r::WipeTowerPositionMode::Auto);
        if (auto_tower && print.has_wipe_tower() &&
            print.config().print_sequence.value == Slic3r::PrintSequence::ByLayer) {
            if (const auto position = print.find_wipe_tower_position(callbacks.is_cancelled)) {
                auto* tower_x = config.option<Slic3r::ConfigOptionFloats>("wipe_tower_x", true);
                auto* tower_y = config.option<Slic3r::ConfigOptionFloats>("wipe_tower_y", true);
                if (tower_x->get_at(native_plate_index) != position->x() || tower_y->get_at(native_plate_index) != position->y()) {
                    // Orca PartPlateList caps new plate indices at 36. Do not
                    // grow the coordinate vectors for an arbitrary SDK index.
                    if (native_plate_index >= native_plate_limit &&
                        native_plate_index >= std::min(tower_x->size(), tower_y->size())) {
                        result.diagnostics.push_back({"validation", "Invalid native plate index for automatic tower placement",
                                                      false, "wipe_tower_position_mode"});
                        return result;
                    }
                    const Slic3r::ConfigOptionFloat x(position->x()), y(position->y());
                    tower_x->set_at(&x, native_plate_index, 0);
                    tower_y->set_at(&y, native_plate_index, 0);
                    print.apply(plate_model, config);
                }
            } else if (cancellation_requested(callbacks)) {
                result.cancelled = true;
                return result;
            } else {
                tower_placement_failed = true;
            }
        }
        if (calibration) print.set_calib_params(calibration->impl_->params);
        validate_effective_mask_regions(print, mask_volume_ids);

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
        if (tower_placement_failed) {
            result.diagnostics.push_back({"validation", "No free position for the prime tower on this plate",
                                          false, "wipe_tower_position_mode"});
            return result;
        }
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        report_progress(callbacks, 0.1f, "Slicing model");
        std::unique_ptr<Slic3r::BeltSupportDebugRecorder> belt_support_debug;
        ToolpathPreviewPtr belt_support_debug_preview;
        std::string belt_support_debug_gcode_path;
        if (request.belt_support_debug_only) {
            if (!belt_printer) {
                result.diagnostics.push_back({
                    "belt_support_debug",
                    "Belt support audit requires an active Belt printer configuration",
                    false});
                return result;
            }
            belt_support_debug = std::make_unique<Slic3r::BeltSupportDebugRecorder>();
        }
        {
            Slic3r::ScopedBeltSupportDebugRecorder debug_scope(belt_support_debug.get());
            print.process();
        }
        if (calibration && calibration->impl_->flow_target) {
            const auto& target = *calibration->impl_->flow_target;
            const auto used = print.get_slice_used_filaments(false);
            const auto& effective = print.config();
            if (used != std::vector<unsigned>{unsigned(target.slot)} ||
                effective.filament_map.get_at(target.slot) != int(target.logical_extruder + 1) ||
                effective.physical_extruder_map.get_at(target.logical_extruder) != int(target.physical_tool))
                throw std::invalid_argument("Flow slicing changed the selected material or tool");
        }
        // Keep planner statistics internal; diagnostics are the existing public
        // channel. Candidates and accepted/rejected fragments are distinct units.
        std::map<std::string, size_t> fiber_rejections;
        size_t fiber_candidates = 0, fiber_accepted = 0, fiber_split = 0;
        for (const auto* object : print.objects())
            for (const auto* layer : object->layers()) {
                if (!layer->fiber_outer_contour_failures.empty()) {
                    std::string message = "Some complete fiber outer loops were omitted on layer " +
                        std::to_string(layer->id() + 1) + "; partial outer strands were not emitted. Reasons:";
                    for (const auto& failure : layer->fiber_outer_contour_failures)
                        message += " " + failure.first + "=" + std::to_string(failure.second);
                    result.diagnostics.push_back({"fiber_outer_contour_rejected", std::move(message), true,
                        "generate_reinforced_perimeters"});
                }
                if (!layer->fiber_hole_contour_failures.empty()) {
                    std::string message = "Some complete fiber hole loops were omitted on layer " +
                        std::to_string(layer->id() + 1) + "; partial hole contours are disabled. Reasons:";
                    for (const auto& failure : layer->fiber_hole_contour_failures)
                        message += " " + failure.first + "=" + std::to_string(failure.second);
                    result.diagnostics.push_back({"fiber_hole_contour_rejected", std::move(message), true,
                        "fiber_contour_allow_partial_holes"});
                }
                const auto& stats = layer->fiber_infill_statistics;
                fiber_candidates += stats.candidates;
                fiber_accepted += stats.accepted_fragments;
                fiber_split += stats.split_candidates;
                for (const auto& entry : stats.rejected_fragments)
                    fiber_rejections[entry.first] += entry.second;
            }
        if (fiber_candidates > 0) {
            std::string message = "Continuous fiber infill: candidates=" + std::to_string(fiber_candidates) +
                ", accepted_fragments=" + std::to_string(fiber_accepted) +
                ", split_candidates=" + std::to_string(fiber_split) + ". Rejected fragments:";
            for (const auto& entry : fiber_rejections)
                message += " " + entry.first + "=" + std::to_string(entry.second);
            result.diagnostics.push_back({fiber_accepted == 0 ? "fiber_infill_empty" : "fiber_infill_summary",
                std::move(message), fiber_accepted == 0, "generate_reinforced_infills"});
        }
        if (cancellation_requested(callbacks)) {
            result.cancelled = true;
            return result;
        }

        if (belt_support_debug) {
            report_progress(callbacks, 0.9f, "Saving Belt support audit stages");
            const bool temporary_debug = request.belt_support_debug_output_path.empty();
            const std::string debug_path = temporary_debug
                ? temporary_output_path(".belt-support-audit.json")
                : request.belt_support_debug_output_path;
            const fs::path debug_parent = fs::path(debug_path).parent_path();
            if (!debug_parent.empty())
                fs::create_directories(debug_parent);
            std::ofstream stream(debug_path, std::ios::binary | std::ios::trunc);
            if (!stream) {
                result.diagnostics.push_back({
                    "belt_support_debug",
                    "Unable to create Belt support audit file: " + debug_path,
                    false});
                return result;
            }
            stream << belt_support_debug_json(
                *belt_support_debug, true, true, debug_path).dump(2);
            stream.close();
            if (!stream) {
                result.diagnostics.push_back({
                    "belt_support_debug",
                    "Unable to write Belt support audit file: " + debug_path,
                    false});
                return result;
            }
            result.belt_support_debug.path = debug_path;
            result.belt_support_debug.ownership = temporary_debug
                ? OutputArtifactOwnership::LibraryTemporary
                : OutputArtifactOwnership::CallerOwned;
            result.preview = make_belt_support_debug_preview(*belt_support_debug, debug_path);
            if (!result.preview || result.preview->segments.empty()) {
                result.diagnostics.push_back({
                    "belt_support_debug",
                    "Belt support audit produced no drawable intermediate geometry",
                    false});
                result.preview.reset();
                return result;
            }
            result.summary.layer_count = result.preview->layers.size();
            result.summary.logical_motion_count = result.preview->segments.size();
            result.summary.render_segment_count = result.preview->segments.size();
            belt_support_debug_preview = result.preview;
            if (!temporary_debug) {
                belt_support_debug_gcode_path =
                    (fs::path(debug_path).parent_path() /
                     "belt-support.gcode").string();
            }
        }

        const bool library_temporary = request.output_gcode_path.empty() &&
            belt_support_debug_gcode_path.empty();
        std::string output_path = !request.output_gcode_path.empty()
            ? request.output_gcode_path
            : (!belt_support_debug_gcode_path.empty()
                ? belt_support_debug_gcode_path
                : temporary_output_path(".gcode"));
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
        // Tool ordering may resolve a different filament assignment than the request.
        // Package and preview the actual execution mapping, not the input proposal.
        config.set_key_value("filament_map", print.config().filament_map.clone());
        config.set_key_value("physical_extruder_map", print.config().physical_extruder_map.clone());
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
            auto preview = make_toolpath_preview(processor_result, &config, result.output.path,
                                                   print.belt_coordinate_system());
            if (preview && print.config().fiber_fill_debug.value) {
                for (size_t oi = 0; oi < print.objects().size(); ++oi) {
                    const auto* object = print.objects()[oi];
                    for (const auto* layer : object->layers()) {
                        const auto preview_layer = std::find_if(preview->layers.begin(), preview->layers.end(),
                            [&](const auto& value) { return std::abs(value.print_z_mm - layer->print_z) < 1e-4; });
                        if (preview_layer == preview->layers.end()) {
                            if (!layer->fiber_fill_diagnostics.empty())
                                result.diagnostics.push_back({"fiber_debug_layer", "Unmapped debug layer z=" + std::to_string(layer->print_z), true});
                            continue;
                        }
                        for (size_t ii = 0; ii < object->instances().size(); ++ii) {
                            const auto shift = object->instances()[ii].shift;
                            for (const auto& source : layer->fiber_fill_diagnostics) {
                                FiberFillDiagnosticPath path;
                                path.reason = source.reason;
                                path.contour = source.contour;
                                path.source_length_mm = source.source_length_mm;
                                path.layer_index = preview_layer->index;
                                path.object_index = oi;
                                path.instance_index = ii;
                                switch (source.kind) {
                                case Slic3r::Layer::FiberDiagnosticKind::RejectedPath:
                                    path.kind = FiberDiagnosticKind::RejectedPath; break;
                                case Slic3r::Layer::FiberDiagnosticKind::OriginalContourRegion:
                                    path.kind = FiberDiagnosticKind::OriginalContourRegion; break;
                                case Slic3r::Layer::FiberDiagnosticKind::ContourCandidate:
                                    path.kind = FiberDiagnosticKind::ContourCandidate; break;
                                case Slic3r::Layer::FiberDiagnosticKind::RoundedContourCandidate:
                                    path.kind = FiberDiagnosticKind::RoundedContourCandidate; break;
                                case Slic3r::Layer::FiberDiagnosticKind::MissingContourRegion:
                                    path.kind = FiberDiagnosticKind::MissingContourRegion; break;
                                }
                                path.policy_group_id = source.policy_group_id;
                                path.component_id = source.component_id;
                                const auto world_point = [&](double x, double y) {
                                    Slic3r::Vec3d position(x + Slic3r::unscale<double>(shift.x()),
                                                         y + Slic3r::unscale<double>(shift.y()), layer->print_z);
                                    if (const auto* belt = print.belt_coordinate_system())
                                        position = belt->oriented_to_world(position);
                                    return ToolpathPoint{float(position.x()), float(position.y()), float(position.z())};
                                };
                                for (const auto& point : source.geometry.points)
                                    path.points.push_back(world_point(Slic3r::unscale<double>(point.x()), Slic3r::unscale<double>(point.y())));
                                const auto append_loop = [&](const Slic3r::Polygon& polygon) {
                                    if (polygon.points.size() < 3) return;
                                    std::vector<ToolpathPoint> loop;
                                    for (const auto& point : polygon.points)
                                        loop.push_back(world_point(Slic3r::unscale<double>(point.x()), Slic3r::unscale<double>(point.y())));
                                    loop.push_back(loop.front());
                                    path.boundaries.push_back(std::move(loop));
                                };
                                append_loop(source.region.contour);
                                for (const auto& hole : source.region.holes) append_loop(hole);
                                if (path.kind == FiberDiagnosticKind::MissingContourRegion)
                                    for (const auto& point : Slic3r::triangulate_expolygon_2d(source.region))
                                        path.triangles.push_back(world_point(point.x(), point.y()));
                                if (path.points.size() >= 2 || !path.boundaries.empty())
                                    preview->fiber_fill_diagnostics.push_back(std::move(path));
                            }
                        }
                    }
                }
            }
            result.preview = std::move(preview);
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

        // The audit UI must keep exposing the twelve process stages. The
        // production preview above is still built and validated from the
        // exported G-code before it is replaced here.
        if (belt_support_debug_preview)
            result.preview = std::move(belt_support_debug_preview);

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
        processor.init_filament_maps_and_nozzle_type_when_import_only_gcode();
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
        const auto belt_coordinates = belt_coordinates_from_gcode(request.gcode_path);
        const auto resolved_tools = processor.physical_tool_by_filament();
        result.preview = make_toolpath_preview(processor.get_result(), nullptr, request.gcode_path,
                                               belt_coordinates ? &*belt_coordinates : nullptr, &resolved_tools);
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

std::uint64_t Library::configuration_revision() const noexcept { return impl_->active_revision; }
ConfigurationState Library::capture_configuration() const {
    ConfigurationState state;
    state.impl_=impl_->active;
    state.captured_revision_=impl_->active_revision;
    state.filament_color_cursor_=impl_->filament_color_cursor;
    state.capture_origin_=impl_->capture_origin;
    return state;
}
ConfigurationInstallResult Library::install_configuration(const ConfigurationState& state, std::uint64_t expected_revision) {
    ConfigurationInstallResult result;
    if (impl_->active_revision!=expected_revision) { result.diagnostics.push_back({"revision","Configuration changed before project commit"});return result; }
    if (!state.impl_ || (!state.impl_->resource_directory.empty()&&state.impl_->resource_directory!=impl_->resource_directory)) {
        result.diagnostics.push_back({"configuration","Configuration belongs to a different library resource context"});return result;
    }
    if(impl_->active_revision==std::numeric_limits<std::uint64_t>::max()) {result.diagnostics.push_back({"revision","Configuration revision exhausted"});return result;}
    result.previous=capture_configuration();
    impl_->active=state.impl_;
    impl_->filament_color_cursor=state.filament_color_cursor_;
    result.revision=++impl_->active_revision;
    result.success=true;return result;
}
std::optional<ProjectMachine> Library::active_project_machine() const {
    if(!impl_->active->config)return std::nullopt;
    if(!impl_->active->machine.model.id.empty())return impl_->active->machine;
    auto machine=describe_project_machine(dynamic_config(impl_->active->config->snapshot()));
    machine.project_local=false;
    impl_->apply_installed_machine_capabilities(machine,impl_->active->selection);
    return machine;
}
ProjectReadResult Library::read_project(const ProjectReadRequest& request, const SliceCallbacks& callbacks) const {
    return read_project(request, callbacks, {});
}

ProjectReadResult Library::read_project(const ProjectReadRequest& request, const SliceCallbacks& callbacks,
                                      const ProjectDecisionCallback& decisions) const {
    using namespace Slic3r;
    ProjectReadResult result;
    result.replace_document = request.mode == ProjectReadMode::Project;
    PlateDataPtrs plates;
    std::vector<Preset*> presets;
    struct ImportOwner {
        PlateDataPtrs& plates;
        std::vector<Preset*>& presets;
        ~ImportOwner() {
            release_PlateData_list(plates);
            for (auto* p : presets) if (p) { delete p->loading_substitutions; p->loading_substitutions=nullptr; delete p; }
        }
    } owner{plates,presets};
    struct DecisionRequired {};
    const auto check_cancel=[&] { if (cancellation_requested(callbacks)) throw CanceledException(); };
    const auto cancelled_after_exception=[&]() noexcept {
        try { return cancellation_requested(callbacks); } catch (...) { return false; }
    };
    const auto diagnostic=[&](std::string code, std::string message, std::string context={}, std::string severity="warning") {
        result.diagnostics.push_back({std::move(severity),std::move(code),std::move(context),std::move(message)});
    };
    const auto ask=[&](ProjectDecisionKind kind, const char* key, const char* message,
                       std::size_t object=std::size_t(-1), std::size_t instance=std::size_t(-1)) {
        check_cancel();
        ProjectDecisionAnswer answer;
        if (decisions) answer=decisions({kind,object,instance,message});
        else if (kind==ProjectDecisionKind::LegacyInfillRotation && request.accept_legacy_infill_rotation_fix)
            answer=ProjectDecisionAnswer::Yes;
        else {
            result.required_decisions.push_back(key);
            diagnostic("project.decision_required",message,key,"info");
            throw DecisionRequired{};
        }
        check_cancel();
        if (answer==ProjectDecisionAnswer::Cancel) throw CanceledException();
        if (answer!=ProjectDecisionAnswer::Yes && answer!=ProjectDecisionAnswer::No)
            throw std::invalid_argument("Invalid project decision answer");
        return answer==ProjectDecisionAnswer::Yes;
    };
    try {
        check_cancel();
        std::error_code error;
        if (request.path.empty() || !fs::is_regular_file(fs::u8path(request.path),error) || error) {
            diagnostic("project.input","3MF input is not a readable regular file",{},"error"); return result;
        }
        const auto context=request.configuration ? *request.configuration : capture_configuration();
        if (context.capture_origin_.lock()!=impl_->capture_origin || context.impl_!=impl_->active || context.captured_revision_!=impl_->active_revision)
            throw std::invalid_argument("Project context is stale or belongs to a different library");
        result.configuration_revision=context.captured_revision_;
        result.filament_color_cursor=request.filament_color_cursor.value_or(context.filament_color_cursor_);
        auto state=std::make_shared<ConfigurationState::Impl>(*context.impl_);
        state->resource_directory=impl_->resource_directory;
        // A GUI always has a selected bundle. A fresh SDK Library may bootstrap
        // a project from its installed catalog, without activating global state.
        if (!state->bundle) {
            if (request.mode==ProjectReadMode::Geometry)
                throw std::invalid_argument("Geometry import requires a captured current configuration");
            state->bundle=impl_->vendor_presets.empty() ? std::make_shared<PresetBundle>()
                : std::make_shared<PresetBundle>(*impl_->vendor_presets.front());
        }
        auto& bundle=*state->bundle;
        // Plater::load_project -> reset: discard embedded presets locally. The
        // document replacement itself is performed by the caller at commit.
        const auto entry_config=bundle.full_config();
        const auto entry_bed_model=project_bed_model(bundle,entry_config);
        if (result.replace_document) bundle.reset_project_embedded_presets();
        auto current=bundle.full_config();
        const auto sync_nozzle_count=[&] {
            const auto& printer=bundle.printers.get_edited_preset();
            if (printer.printer_technology()==ptFFF && !printer.config.opt_bool("single_extruder_multi_material")) {
                if (const auto* nozzles=printer.config.option<ConfigOptionFloats>("nozzle_diameter"))
                    bundle.set_num_filaments(nozzles->values.size());
            }
        };
        if (result.replace_document) { sync_nozzle_count(); current=bundle.full_config(); }
        auto current_machine=describe_project_machine(current,false);
        ProjectBedContext bed;
        bed.minimum_x=bed.minimum_y=std::numeric_limits<double>::max();
        bed.maximum_x=bed.maximum_y=-std::numeric_limits<double>::max();
        for (const auto& point:current_machine.variant.printable_area) {
            bed.minimum_x=std::min(bed.minimum_x,point.x); bed.minimum_y=std::min(bed.minimum_y,point.y);
            bed.maximum_x=std::max(bed.maximum_x,point.x); bed.maximum_y=std::max(bed.maximum_y,point.y);
        }
        // force_print_bed_update still respects Bed3D::set_shape's equal-input
        // short-circuit. Preserve the caller's cached dimensions in that case.
        const auto current_bed_model=project_bed_model(bundle,current);
        const bool reset_bed_changed=result.replace_document &&
            (project_bed_inputs_changed(entry_config,current)||entry_bed_model!=current_bed_model||
             (request.bed&&(request.bed->shape_position_mm.x!=0||request.bed->shape_position_mm.y!=0)));
        if(reset_bed_changed) {
            // An explicit snapshot belongs to the entry printer, before reset.
            // set_shape consumes that printer's old axes, then updates them.
            const auto entry_machine=describe_project_machine(entry_config,false);
            const float old_axes=request.bed ? request.bed->axes_length_mm.value_or(project_axes_length(entry_machine.variant))
                : project_axes_length(entry_machine.variant);
            bed.native_plate_size=project_native_plate_size(current_machine.variant,old_axes);
            bed.axes_length_mm=project_axes_length(current_machine.variant);
        } else if(request.bed) bed=*request.bed;
        if (!std::isfinite(bed.minimum_x)||!std::isfinite(bed.minimum_y)||!std::isfinite(bed.maximum_x)||!std::isfinite(bed.maximum_y)||
            bed.maximum_x<=bed.minimum_x||bed.maximum_y<=bed.minimum_y)
            throw std::invalid_argument("Project current bed context is invalid");
        if (result.replace_document) { bed.current_plate_empty=true; bed.occupied_hulls.clear(); }
        Vec2d bed_center((bed.minimum_x+bed.maximum_x)/2,(bed.minimum_y+bed.maximum_y)/2);
        Vec3d bed_size(bed.maximum_x-bed.minimum_x-2,bed.maximum_y-bed.minimum_y-2,-1);
        bool load_model=true, load_config=result.replace_document;
        LoadStrategy strategy=LoadStrategy::LoadModel;
        if (load_config) strategy=strategy|LoadStrategy::LoadConfig|LoadStrategy::CheckVersion|LoadStrategy::LoadAuxiliary;
        DynamicPrintConfig loaded;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
        En3mfType type=En3mfType::From_BBS;
        Semver version;
        report_progress(callbacks,0.03f,"Reading 3MF without modifying the active project");
        auto model=Model::read_from_archive(request.path,&loaded,&substitutions,type,strategy,&plates,&presets,&version,
            [&](int stage,int position,int total,bool& cancel) {
                cancel=cancellation_requested(callbacks);
                if (!cancel) report_progress(callbacks,std::clamp((float(stage)+(total?float(position)/float(total):0))/float(IMPORT_STAGE_MAX),0.03f,0.70f),"Reading 3MF");
            });
        check_cancel();
        auto scene=std::make_shared<ProjectScene>();
        scene->producer=type==En3mfType::From_Orca?"OrcaSlicer":type==En3mfType::From_BBS?"BambuStudio":type==En3mfType::From_Prusa?"PrusaSlicer":"Other";
        scene->version=version.to_string(); scene->kind=loaded.empty()?ProjectContainerKind::Model:ProjectContainerKind::Project;
        // Plater.cpp:6092. This must precede producer fallback, and logical
        // filament IDs must not be clamped to a physical nozzle capacity.
        bool expanded_filaments=false;
        if (type==En3mfType::From_Prusa || (load_model && !load_config)) {
            std::set<int> extruderIds;
            for (auto* object:model.objects) {
                if (object->config.option("extruder")) extruderIds.insert(object->config.extruder());
                for (auto* volume:object->volumes) {
                    if (volume->config.option("extruder")) extruderIds.insert(volume->config.extruder());
                    for (int extruder:volume->get_extruders()) extruderIds.insert(extruder);
                }
            }
            const int size=extruderIds.empty()?0:*extruderIds.rbegin();
            static constexpr const char* colors[]={"#00C1AE","#F4E2C1","#ED1C24","#00FF7F","#F26722","#FFEB31","#7841CE","#115877","#ED1E79","#2EBDEF","#345B2F","#800080","#FA8173","#800000","#F7B763","#A4C41E"};
            int filament_size=int(bundle.filament_presets.size());
            while (filament_size<MAXIMUM_EXTRUDER_NUMBER && filament_size<size) {
                bundle.set_num_filaments(filament_size+1,colors[result.filament_color_cursor++ % std::size(colors)]);
                ++filament_size; expanded_filaments=true;
            }
        }
        // Original Plater.cpp:6139-6297. Preserve this if/else-if tree: an
        // earlier version branch deliberately prevents the later empty check.
        const Semver app_version=*Semver::parse(SoftFever_VERSION);
        const auto rotation_template=[&]() -> std::string& {
            auto* option=loaded.option<ConfigOptionString>("sparse_infill_rotate_template");
            if (!option) throw std::invalid_argument("Legacy alpha project is missing sparse_infill_rotate_template");
            return option->value;
        };
        const auto infill_pattern=[&] {
            const auto* option=loaded.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern");
            if (!option) throw std::invalid_argument("Legacy alpha project is missing sparse_infill_pattern");
            return option->value;
        };
        // ORCA_PROJECT_COMPATIBILITY_BEGIN
        if (type == En3mfType::From_Prusa) {
            // do not reset the model config
            load_config = false;
            if (request.mode != ProjectReadMode::Geometry)
                diagnostic("project.geometry_fallback", "The 3MF is not supported by OrcaSlicer, loading geometry data only.", {}, "info");
        }
        else if (type == En3mfType::From_Orca) {
            // OrcaSlicer file (has OrcaSlicer tag) - compare version with SoftFever_VERSION
            // Migration fix for OrcaSlicer 2.3.1-alpha sparse infill rotation template
            if (load_config && (version < app_version) && version == Semver("2.3.1-alpha")) {
                if (!rotation_template().empty()) {
                    const auto _sparse_infill_pattern =
                        infill_pattern();
                    bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                             _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                             _sparse_infill_pattern == ipLockedZag;
                    if (!is_safe_to_rotate) {
                        if (ask(ProjectDecisionKind::LegacyInfillRotation, "legacy_infill_rotation_fix", "Clear the legacy OrcaSlicer 2.3.1-alpha infill rotation template?")) {
                            rotation_template() = "";
                        }
                    }
                }
            } else if (load_config && (version > app_version)) {
                if (substitutions.unrecogized_keys.size() > 0) {
                    diagnostic("project.newer_version", "The 3MF version may not be fully compatible.", version.to_string());
                }
                else {
                    //if the minor version is not matched
                    if (version.min() != app_version.min()) {
                        diagnostic("project.newer_version", "The 3MF version may not be fully compatible.", version.to_string());
                    }
                }
            }
            else if (load_config && loaded.empty()) {
                load_config = false;
                diagnostic("project.geometry_fallback", "The 3MF file was generated by an old OrcaSlicer version, loading geometry data only.", {}, "info");
            }
        }
        else if (type == En3mfType::From_BBS) {
            // No OrcaSlicer tag - check Bambu/Application version
            Semver orca_tag_start_version(2, 3, 2);
            if (version <= orca_tag_start_version) {
                // Compatible old version (before OrcaSlicer tagging was introduced after 2.3.2).
                // Any version prior or equal to 2.3.2 is older than the current one, no version warnings needed.
                // Still apply migration fixes for known old versions.
                if (load_config && (version == Semver("2.3.1-alpha"))) {
                    if (!rotation_template().empty()) {
                        const auto _sparse_infill_pattern =
                            infill_pattern();
                        bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                                 _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                                 _sparse_infill_pattern == ipLockedZag;
                        if (!is_safe_to_rotate) {
                            if (ask(ProjectDecisionKind::LegacyInfillRotation, "legacy_infill_rotation_fix", "Clear the legacy OrcaSlicer 2.3.1-alpha infill rotation template?")) {
                                rotation_template() = "";
                            }
                        }
                    }
                }
                else if (load_config && loaded.empty()) {
                    load_config = false;
                    diagnostic("project.geometry_fallback", "The 3MF file was generated by an older version, loading geometry data only.", {}, "info");
                }
            } else {
                // BambuStudio project (version > 2.3.2 without OrcaSlicer tag)
                // Report that a BambuStudio project is being imported and compare with SLIC3R_VERSION
                Semver slic3r_version = *(Semver::parse(SLIC3R_VERSION));
                if (load_config && loaded.empty()) {
                    load_config = false;
                    diagnostic("project.geometry_fallback", "The 3MF file was generated by BambuStudio, loading geometry data only.", {}, "info");
                }
                else if (load_config && (version > slic3r_version)) {
                    // BambuStudio file version is newer than our compatible SLIC3R_VERSION
                    if (substitutions.unrecogized_keys.size() > 0) {
                        diagnostic("project.bambu_version", "The 3MF version may not be fully compatible.", version.to_string());
                    } else {
                        diagnostic("project.bambu_version", "The 3MF version may not be fully compatible.", version.to_string());
                    }
                } else if (load_config) {
                    // BambuStudio version is older or same as our SLIC3R_VERSION
                    diagnostic("project.bambu_version", "The 3MF was created by BambuStudio. Some settings may differ from OrcaSlicer.", {}, "info");
                }
            }
        }
        else if (type == En3mfType::From_Other) {
            // Generic CAD/other 3MF without slicer metadata: import geometry silently.
            if (load_config && loaded.empty()) {
                load_config = false;
            }
        }
        else if (load_config && loaded.empty()) {
            load_config = false;
            diagnostic("project.geometry_fallback", "The 3MF file was generated by an old OrcaSlicer version, loading geometry data only.", {}, "info");
        }
        else if (!load_config) {
            // reset config except color
            for (ModelObject *model_object : model.objects) {
                bool has_extruder = model_object->config.has("extruder");
                int  extruder_id  = -1;
                // save the extruder information before reset
                if (has_extruder) { extruder_id = model_object->config.extruder(); }

                model_object->config.reset();

                // restore the extruder after reset
                if (has_extruder) { model_object->config.set("extruder", extruder_id); }

                // Is there any modifier or advanced config data?
                for (ModelVolume *model_volume : model_object->volumes) {
                    has_extruder = model_volume->config.has("extruder");
                    if (has_extruder) { extruder_id = model_volume->config.extruder(); }

                    model_volume->config.reset();

                    if (has_extruder) { model_volume->config.set("extruder", extruder_id); }
                }
            }
        }
        // ORCA_PROJECT_COMPATIBILITY_END
        for (const auto& key:substitutions.unrecogized_keys) {
            if (PrintConfigDef::is_obsolete_key(key)) diagnostic("project.obsolete_setting","Native Orca retired this setting",key,"info");
            else { result.ignored_settings.push_back(key); diagnostic("project.unknown_setting_ignored","Native Orca omitted an unrecognized setting",key,"info"); }
        }
        if (!substitutions.empty()) diagnostic("project.settings_migrated","Native Orca compatibility substitutions were applied",{},"info");
        const bool legacy_layout=load_model&&load_config&&!loaded.empty()&&!plates.empty()&&
            (type==En3mfType::From_BBS||type==En3mfType::From_Orca)&&version<Semver(1,5,9);
        const float current_axes=project_axes_length(current_machine.variant);
        const float captured_axes=bed.axes_length_mm.value_or(current_axes);
        const auto previous_plate_size=bed.native_plate_size ? *bed.native_plate_size : project_native_plate_size(current_machine.variant,captured_axes);
        DynamicPrintConfig resolved=current;
        bool is_project_file=false;
        if (!presets.empty() && load_config) {
            const auto changed=bundle.load_project_embedded_presets(presets,ForwardCompatibilitySubstitutionRule::Enable);
            if (!changed.empty()) diagnostic("project.preset_substitutions","Native Orca migrated embedded presets",{},"info");
        }
        if (load_config && !loaded.empty()) {
            restore_project_tower_modes(loaded, plates.size());
            DynamicPrintConfig config; config.apply(FullPrintConfig::defaults()); config+=std::move(loaded);
            for (const auto& problem:config.validate()) diagnostic("project.invalid_configuration",problem.second,problem.first);
            Preset::normalize(config);
            // Plater.cpp:6402-6447: original legacy prime tower migration.
            if ((type==En3mfType::From_BBS||type==En3mfType::From_Orca)&&version<Semver(2,0,0)) {
                auto* colors=config.option<ConfigOptionStrings>("filament_colour",true);
                const auto count=colors->values.size();
                if (auto* prime=config.option<ConfigOptionFloats>("filament_prime_volume");prime&&!prime->values.empty()&&count>1)
                    prime->values.resize(count,prime->values.front());
                config.option<ConfigOptionEnum<WipeTowerWallType>>("wipe_tower_wall_type",true)->value=wtwRectangle;
                config.option<ConfigOptionPercent>("prime_tower_infill_gap",true)->value=100;
                config.option<ConfigOptionInts>("filament_adhesiveness_category",true)->values.assign(count,100);
                auto& dirty=config.option<ConfigOptionStrings>("different_settings_to_system",true)->values; dirty.resize(count+2);
                const auto add_dirty=[&](std::size_t i,std::initializer_list<const char*> keys) {
                    std::vector<std::string> values; unescape_strings_cstyle(dirty[i],values);
                    for (const auto* key:keys) values.emplace_back(key); dirty[i]=escape_strings_cstyle(values);
                };
                add_dirty(0,{"wipe_tower_wall_type","prime_tower_infill_gap"});
                for (std::size_t i=1;i<=count;++i) add_dirty(i,{"filament_adhesiveness_category"});
            }
            std::set<std::string> modified_gcodes;
            const int validity=bundle.validate_presets(fs::u8path(request.path).filename().string(),config,modified_gcodes);
            if (validity!=VALIDATE_PRESETS_SUCCESS) {
                std::string keys; for (const auto& key:modified_gcodes) { if (!keys.empty()) keys+='\n'; keys+=key; }
                diagnostic("project.preset_gcode_warning","The 3MF contains modified G-code or customized printer/filament presets.",keys);
            }
            std::optional<ConfigOptionFloats> tower_x,tower_y;
            if (const auto* value=config.opt<ConfigOptionFloats>("wipe_tower_x")) tower_x=*value;
            if (const auto* value=config.opt<ConfigOptionFloats>("wipe_tower_y")) tower_y=*value;
            bundle.load_config_model(fs::u8path(request.path).filename().string(),std::move(config),version);
            sync_nozzle_count();
            auto& project=bundle.project_config;
            if (tower_x) *project.opt<ConfigOptionFloats>("wipe_tower_x")=*tower_x;
            if (tower_y) *project.opt<ConfigOptionFloats>("wipe_tower_y")=*tower_y;
            if (const auto* colors=project.opt<ConfigOptionStrings>("filament_colour")) {
                const auto count=colors->size();
                auto* map=project.opt<ConfigOptionInts>("filament_map",true);
                if (map->size()!=count) map->values.resize(count,1);
                auto* multi=project.opt<ConfigOptionStrings>("filament_multi_colour",true);
                if (multi->size()!=count) multi->values.resize(count);
                for (std::size_t i=0;i<count;++i) {
                    const auto values=split_string(multi->values[i],' ');
                    if (values.empty()||values.front()!=colors->values[i]) multi->values[i]=colors->values[i];
                }
                auto* types=project.opt<ConfigOptionStrings>("filament_colour_type",true);
                if (types->size()!=count) {
                    types->values.resize(count);
                    for (auto& value:types->values) if (value.empty()) value="1";
                }
            }
            is_project_file=true;
        }
        resolved=bundle.full_config();
        if (is_project_file) {
            // load_current_presets updates the visible bed before load_model_objects.
            const auto loaded_machine=describe_project_machine(resolved,false);
            bed.minimum_x=bed.minimum_y=std::numeric_limits<double>::max();
            bed.maximum_x=bed.maximum_y=-std::numeric_limits<double>::max();
            for (const auto& p:loaded_machine.variant.printable_area) {
                bed.minimum_x=std::min(bed.minimum_x,p.x); bed.minimum_y=std::min(bed.minimum_y,p.y);
                bed.maximum_x=std::max(bed.maximum_x,p.x); bed.maximum_y=std::max(bed.maximum_y,p.y);
            }
            bed_center={(bed.minimum_x+bed.maximum_x)/2,(bed.minimum_y+bed.maximum_y)/2};
            bed_size={bed.maximum_x-bed.minimum_x-2,bed.maximum_y-bed.minimum_y-2,-1};
        }
        result.load_config=load_config;
        scene->mode=load_config?ProjectReadMode::Project:ProjectReadMode::Geometry;
        // Plater.cpp:6767-6882 and 7114-7243: native model postprocessing.
        if (!is_project_file) {
            if (const int removed=model.removed_objects_with_zero_volume();removed>0)
                diagnostic("project.zero_volume_removed","Objects with zero volume removed",std::to_string(removed),"info");
            if (model.looks_like_saved_in_meters()) {
                if (ask(ProjectDecisionKind::ConvertMeters,"convert_meters","The object may be in meters. Scale to millimetres?")) model.convert_from_meters(true);
            } else if (model.looks_like_imperial_units()) {
                if (ask(ProjectDecisionKind::ConvertInches,"convert_inches","The object may be in inches. Scale to millimetres?")) model.convert_from_imperial_units(true);
            }
        }
        if (!is_project_file && model.looks_like_multipart_object()) {
            if (ask(ProjectDecisionKind::MultipartObject,"multipart_object","Load objects positioned at multiple heights as one object with multiple parts?"))
                model.convert_multipart_object(int(serialized_option_values(current,"filament_colour").size()));
        }
        for (auto* object:model.objects) if (!object->instances.empty()) object->ensure_on_bed(is_project_file);
        if (!is_project_file) model.center_instances_around_point(bed_center);
        if (!is_project_file) model.plates_custom_gcodes.clear();
        std::vector<ModelInstance*> new_instances;
        for (std::size_t oi=0;oi<model.objects.size();++oi) {
            check_cancel(); auto* object=model.objects[oi]; object->sort_volumes(true);
            if (object->instances.empty()) { object->center_around_origin(); new_instances.push_back(object->add_instance()); }
            for (std::size_t ii=0;ii<object->instances.size();++ii) {
                auto* instance=object->instances[ii];
                const Vec3d ratio=object->instance_bounding_box(ii).size().cwiseQuotient(bed_size);
                const double max_ratio=std::max(ratio(0),ratio(1));
                if (max_ratio>10000) {
                    diagnostic("project.oversized_mesh_scaled","The object was scaled down to avoid coordinate overflow.",object->name,"info");
                    object->scale_mesh_after_creation(1./max_ratio); object->origin_translation=Vec3d::Zero(); object->center_around_origin(); break;
                } else if (max_ratio>10 && ask(ProjectDecisionKind::ScaleOversizedInstance,"scale_oversized_instance",
                    "Scale the oversized object down to fit the print bed?",oi,ii))
                    instance->set_scaling_factor(instance->get_scaling_factor()/max_ratio);
            }
            object->ensure_on_bed(is_project_file);
            for (auto* instance:object->instances) if (!instance->is_assemble_initialized())
                instance->set_assemble_transformation(instance->get_transformation());
        }
        // AUTOPLACEMENT_ON_LOAD is defined in original Plater.cpp:7112. Keep
        // GLCanvas3D::get_empty_cells' original per-hull accumulation/order.
        if (!new_instances.empty()) {
            for (auto* instance:new_instances) {
            std::vector<Polygon> hulls;
            for (const auto& values:bed.occupied_hulls) {
                Polygon polygon;
                for (const auto& p:values) {
                    if (!std::isfinite(p.x)||!std::isfinite(p.y)) throw std::invalid_argument("Non-finite occupied hull");
                    polygon.points.emplace_back(scale_(p.x),scale_(p.y));
                }
                if (!polygon.empty()) hulls.push_back(std::move(polygon));
            }
            for (auto* object:model.objects) {
                auto* first=object->instances.front();
                Polygon hull=object->convex_hull_2d(Geometry::assemble_transform({0.,0.,first->get_offset().z()},first->get_rotation(),first->get_scaling_factor(),first->get_mirror()));
                if (hull.empty()) continue;
                for (auto* instance:object->instances) {
                    Geometry::Transformation transformation;
                    transformation.set_offset({scale_(instance->get_offset().x()),scale_(instance->get_offset().y()),0.});
                    transformation.set_rotation(Z,instance->get_rotation().z()-first->get_rotation().z());
                    hulls.push_back(hull.transform(transformation.get_matrix()));
                }
            }
                Vec2f start=bed_center.cast<float>(), target=start;
                if (!bed.current_plate_empty) {
                    std::vector<Vec2f> cells,empty;
                    const float step=10;
                    const auto min_x=start.x()-step*int((start.x()-bed.minimum_x)/step);
                    const auto min_y=start.y()-step*int((start.y()-bed.minimum_y)/step);
                    for (float x=min_x;x<bed.maximum_x-step/2;x+=step)
                        for (float y=min_y;y<bed.maximum_y-step/2;y+=step) cells.emplace_back(x,y);
                    for (const auto& hull:hulls) for (const auto& cell:cells)
                        if (!hull.contains(Point(scale_(cell.x()),scale_(cell.y())))) empty.push_back(cell);
                    std::sort(empty.begin(),empty.end(),[start](const Vec2f& a,const Vec2f& b) { return (a-start).norm()<(b-start).norm(); });
                    if (!empty.empty()) target=empty.front();
                    else { const float offset=float(.05*std::max(bed.maximum_x-bed.minimum_x,bed.maximum_y-bed.minimum_y)); target+=Vec2f(offset,offset); }
                }
                instance->set_offset({target.x(),target.y(),instance->get_offset().z()});
            }
        }
        // Native project file settings are already processed. DTO conversion
        // must not introduce a second producer/mode-based cleanup policy.
        ProjectMachine machine=describe_project_machine(resolved,false);
        const std::size_t count=load_config?std::max<std::size_t>(1,plates.size()):1;
        const float root=std::sqrt(float(count)), rounded=std::round(root);
        const int columns=root>rounded?int(rounded)+1:int(rounded);
        // Temporary pre-config legacy reset_size(int(old_width+1.25),...) has
        // no local model instances yet. Loading the new presets updates plate
        // dimensions before load_model_objects assigns the imported instances.
        // Bed3D updates its cached bounds in update_model_offset BEFORE it
        // changes the axes length to the new volume. Project reset has already
        // refreshed the current printer's axes; consume that previous length.
        const auto loaded_plate_size=load_config &&
            (project_bed_inputs_changed(current,resolved)||current_bed_model!=project_bed_model(bundle,resolved))
            ? project_native_plate_size(machine.variant,current_axes):previous_plate_size;
        const double source_width=loaded_plate_size[0], source_depth=loaded_plate_size[1];
        std::vector<Vec3d> origins;
        for (std::size_t i=0;i<count;++i) {
            ProjectPlate plate; plate.id="plate-"+std::to_string(i+1); plate.name="Plate "+std::to_string(i+1); plate.native_plate_index=int(i);
            const Vec3d origin(double(i%columns)*source_width*1.2,-double(i/columns)*source_depth*1.2,0);
            origins.push_back(origin);
            if (load_config && i<plates.size()) {
                plate.name=plates[i]->plate_name.empty()?plate.name:plates[i]->plate_name;
                plate.locked=plates[i]->locked; plate.settings=project_settings(plates[i]->config,false);
                const auto& path=plates[i]->gcode_file;
                if (!path.empty() && fs::exists(fs::u8path(path))) {
                    std::ifstream input(fs::u8path(path),std::ios::binary);
                    if (!input) throw std::runtime_error("Cannot read extracted plate G-code");
                    plate.gcode.assign(std::istreambuf_iterator<char>(input),{});
                    // Original load_files activates embedded jobs only in the
                    // no-model branch. A model project's cached bytes are an
                    // attachment, not an already valid native slice result.
                    if (model.objects.empty()) {
                        plate.slice_result_valid=true;
                        // Bytes and detached previews outlive the reader's
                        // temporary backup folder.
                        auto preview_callbacks=callbacks;
                        preview_callbacks.progress_details=[&](const SliceProgress& progress) {
                            report_progress(callbacks,.8f+.15f*progress.progress,progress.stage,
                                progress.completed_layers,progress.total_layers,progress.detail);
                        };
                        const auto parsed=load_gcode_preview({path},preview_callbacks);
                        check_cancel();
                        if (parsed.preview) {
                            auto detached=std::make_shared<ToolpathPreview>(*parsed.preview);
                            detached->source_path.clear(); plate.preview=std::move(detached);
                        }
                        if (!parsed.success) diagnostic("project.preview_unavailable","Plate G-code is valid but has no drawable preview.",plate.id);
                    }
                }
            }
            if (load_config) {
                const auto it=model.plates_custom_gcodes.find(int(i));
                if (it!=model.plates_custom_gcodes.end()) {
                    plate.custom_gcode_mode=int(it->second.mode);
                    for (const auto& c:it->second.gcodes) plate.custom_gcodes.push_back({c.print_z,int(c.type),c.extruder,c.color,c.extra});
                }
            }
            const Vec3d final_origin(legacy_layout?double(i%columns)*previous_plate_size[0]*1.2:origin.x(),
                legacy_layout?-double(i/columns)*previous_plate_size[1]*1.2:origin.y(),0);
            plate.native_size_mm=legacy_layout?previous_plate_size:loaded_plate_size;
            plate.origin_mm={final_origin.x(),final_origin.y(),0}; scene->plates.push_back(std::move(plate));
        }
        if (model.objects.empty() && load_model && load_config) {
            for (std::size_t i=0;i<scene->plates.size();++i) if (scene->plates[i].slice_result_valid) {
                scene->active_plate_index=i; scene->preview_only=true; scene->kind=ProjectContainerKind::GCodeJob; break;
            }
        }
        int max_slot=1;
        for(std::size_t oi=0;oi<model.objects.size();++oi){
            check_cancel();const auto* object=model.objects[oi];ProjectObject out;out.id="object-"+std::to_string(oi+1);out.name=object->name;
            if(object->config.has("extruder"))out.default_filament_slot=object->config.extruder();
            if(out.default_filament_slot<0)diagnostic("project.material_reference","Object material slot is invalid",out.id);
            max_slot=std::max(max_slot,out.default_filament_slot);
            {out.settings=project_settings(object->config.get());for(const auto& r:object->layer_config_ranges)out.layer_ranges.push_back({r.first.first,r.first.second,project_settings(r.second.get(),false)});out.layer_height_profile=object->layer_height_profile.get();for(const auto& b:object->brim_points)out.brim_points.push_back({b.pos.x(),b.pos.y(),b.pos.z(),b.head_front_radius});}

            for(std::size_t pi=0;pi<object->volumes.size();++pi){
                check_cancel();const auto* volume=object->volumes[pi];ProjectPart part;part.id=out.id+"/part-"+std::to_string(pi+1);part.name=volume->name;
                switch(volume->type()){
                case Slic3r::ModelVolumeType::MODEL_PART:part.role=ProjectPartRole::Model;break;
                case Slic3r::ModelVolumeType::NEGATIVE_VOLUME:part.role=ProjectPartRole::NegativeVolume;break;
                case Slic3r::ModelVolumeType::PARAMETER_MODIFIER:part.role=ProjectPartRole::ParameterModifier;break;
                case Slic3r::ModelVolumeType::SUPPORT_ENFORCER:part.role=ProjectPartRole::SupportEnforcer;break;
                case Slic3r::ModelVolumeType::SUPPORT_BLOCKER:part.role=ProjectPartRole::SupportBlocker;break;
                default:throw std::invalid_argument("Unsupported model volume role");}
                if(volume->config.has("extruder")&&volume->config.extruder()<0)diagnostic("project.material_reference","Part material slot is invalid",part.id);
                part.explicit_filament_binding=volume->config.has("extruder")&&volume->config.extruder()!=0;
                if(part.explicit_filament_binding)part.default_filament_slot=volume->config.extruder();max_slot=std::max(max_slot,part.default_filament_slot);
                const auto matrix=volume->get_matrix();for(int r=0;r<4;++r)for(int c=0;c<4;++c)part.local_transform[std::size_t(r*4+c)]=matrix(r,c);if (!matrix.matrix().allFinite()) throw std::invalid_argument("Non-finite part transform");
                auto mesh=std::make_shared<ProjectMesh>();const auto& native=volume->mesh().its;
                std::size_t mesh_checks=0;
                for(const auto& v:native.vertices){if((mesh_checks++&0x3fff)==0)check_cancel();if(!v.allFinite())throw std::invalid_argument("Non-finite mesh vertex");mesh->vertices.push_back({v.x(),v.y(),v.z()});}
                for(const auto& t:native.indices){if((mesh_checks++&0x3fff)==0)check_cancel();if(t.minCoeff()<0||std::size_t(t.maxCoeff())>=mesh->vertices.size())throw std::invalid_argument("Invalid triangle index");mesh->triangles.push_back({unsigned(t(0)),unsigned(t(1)),unsigned(t(2))});}
                if(mesh->vertices.empty()||mesh->triangles.empty()) { diagnostic("project.empty_volume","Skipping empty model volume",part.id,"info"); continue; }
                part.settings=project_settings(volume->config.get());
                part.material_facets=export_facets(volume->mmu_segmentation_facets,&callbacks);part.support_facets=export_facets(volume->supported_facets,&callbacks);part.seam_facets=export_facets(volume->seam_facets,&callbacks);part.fuzzy_skin_facets=export_facets(volume->fuzzy_skin_facets,&callbacks);
                const auto material=native_facets(part.material_facets,mesh->triangles.size(),int(Slic3r::EnforcerBlockerType::ExtruderMax));
                for(std::size_t state=1;state<material.used_states.size();++state)if(material.used_states[state])max_slot=std::max(max_slot,int(state));
                native_facets(part.support_facets,mesh->triangles.size(),2);native_facets(part.seam_facets,mesh->triangles.size(),2);native_facets(part.fuzzy_skin_facets,mesh->triangles.size(),2);
                part.mesh=mesh;out.parts.push_back(std::move(part));
            }
            for(std::size_t ii=0;ii<object->instances.size();++ii){
                ProjectInstance instance;instance.id=out.id+"/instance-"+std::to_string(ii+1);instance.name=object->name;instance.printable=object->printable&&object->instances[ii]->printable;
                auto matrix=object->instances[ii]->get_matrix();int plate_index=-1;
                if(!load_config || plates.empty())plate_index=0;
                else {
                    // Original PartPlate::intersect_instance uses plate/instance bounding-box intersection.
                    const auto bounds=object->instance_convex_hull_bounding_box(ii);
                    double min_x=std::numeric_limits<double>::max(),min_y=min_x,max_x=-min_x,max_y=-min_x;
                    for(const auto& p:machine.variant.printable_area){min_x=std::min(min_x,p.x);min_y=std::min(min_y,p.y);max_x=std::max(max_x,p.x);max_y=std::max(max_y,p.y);}
                    for(std::size_t p=0;p<count;++p){Slic3r::BoundingBoxf3 bed(Slic3r::Vec3d(min_x-BuildVolume::SceneEpsilon,min_y-BuildVolume::SceneEpsilon,-BuildVolume::SceneEpsilon)+origins[p],Slic3r::Vec3d(max_x+BuildVolume::SceneEpsilon,max_y+BuildVolume::SceneEpsilon,machine.variant.printable_height+BuildVolume::SceneEpsilon)+origins[p]);if(bed.intersects(bounds)){plate_index=int(p);break;}}
                }
                if(plate_index>=0){instance.plate_id=scene->plates[std::size_t(plate_index)].id;matrix.translation()-=origins[std::size_t(plate_index)];}
                for(int r=0;r<4;++r)for(int c=0;c<4;++c)instance.transform[std::size_t(r*4+c)]=matrix(r,c);if (!matrix.matrix().allFinite()) throw std::invalid_argument("Non-finite instance transform");out.instances.push_back(std::move(instance));
            }
            scene->objects.push_back(std::move(out));
        }
        // Build configuration from the actual working bundle, including a
        // geometry-only palette expansion or a project-entry reset fallback.
        result.configuration_changed=result.replace_document || expanded_filaments;
        if (result.configuration_changed) {
            state->config=std::unique_ptr<Config>(new Config(serialized_values(resolved)));
            state->machine=machine;
            state->machine.project_local=true;
            state->selection.machine_model_id=machine.model.id;
            state->selection.machine_variant_id=machine.variant.id;
            state->selection.printer_preset_id=bundle.printers.get_selected_preset_name();
            state->selection.process_preset_id=bundle.prints.get_selected_preset_name();
            state->selection.filament_preset_ids=bundle.filament_presets;
            for (const auto& seed:impl_->vendor_presets)
                if (seed->printers.find_system_preset_by_model_and_variant(machine.model.id,machine.variant.id)) {
                    state->machine.project_local=false; break;
                }
            impl_->apply_installed_machine_capabilities(state->machine,state->selection);
            state->compatible_processes=compatible_presets(bundle.prints,state->selection.process_preset_id);
            state->compatible_filaments=compatible_presets(bundle.filaments,state->selection.filament_preset_ids.empty()?"":state->selection.filament_preset_ids.front());
            Library viewer; viewer.impl_->resource_directory=impl_->resource_directory;
            viewer.impl_->active=state; viewer.impl_->active_revision=1;
            state->prepared_view=*viewer.active_config(); state->prepared_view.revision=0;
            result.configuration.impl_=state;
            result.configuration.filament_color_cursor_=result.filament_color_cursor;
        }
        if (result.configuration_changed) scene->source_filaments=state->prepared_view.filament_slots;
        else if (context.valid()) scene->source_filaments=context.view().filament_slots;
        // These are diagnostics, not a new file-acceptance policy. Native slice
        // validation is responsible for invalid settings and material bindings.
        if (max_slot>int(scene->source_filaments.size()))
            diagnostic("project.material_reference","Model references a filament outside the current palette",std::to_string(max_slot));
        check_cancel(); report_progress(callbacks,1.f,"3MF candidate prepared"); check_cancel();
        result.scene=std::move(scene); result.success=true;
    } catch (const DecisionRequired&) {
        result.scene.reset(); result.configuration={}; result.ignored_settings.clear();
    } catch (const CanceledException&) {
        result.cancelled=true; result.scene.reset(); result.configuration={}; result.ignored_settings.clear();
    } catch (const std::exception& e) {
        if (cancelled_after_exception()) result.cancelled=true;
        else diagnostic("project.read_failed",e.what(),{},"error");
        result.scene.reset(); result.configuration={}; result.ignored_settings.clear();
    } catch (...) {
        result.cancelled=cancelled_after_exception();
        if (!result.cancelled) diagnostic("project.read_failed","Project callback or native reader threw a non-standard exception",{},"error");
        result.scene.reset(); result.configuration={}; result.ignored_settings.clear();
    }
    return result;
}
ProjectReadResult Library::remap_project_materials(const ProjectReadResult& input,const std::vector<unsigned>& mapping,const ConfigSnapshot& target,const SliceCallbacks& callbacks) const {
    ProjectReadResult result;
    try {
        if(!input.success||!input.scene||input.scene->mode!=ProjectReadMode::Geometry||!target.valid())throw std::invalid_argument("Material mapping requires a successful geometry candidate and target configuration");
        const auto config=dynamic_config(target);const auto count=serialized_option_values(config,"filament_diameter").size();
        if(mapping.size()!=input.scene->source_filaments.size())throw std::invalid_argument("Material mapping does not cover every source slot");
        for(unsigned slot:mapping)if(slot==0||slot>count)throw std::invalid_argument("Mapped material slot is outside the target configuration");
        auto scene=std::make_shared<ProjectScene>(*input.scene);
        const auto map_slot=[&](int slot){if(slot==0)return 0;if(slot<0||std::size_t(slot)>mapping.size())throw std::invalid_argument("Source material reference outside mapping");return int(mapping[std::size_t(slot-1)]);};
        const auto& keys=project_material_keys();
        const auto map_settings=[&](ProjectSettings& settings){for(auto& p:settings)if(keys.count(p.first)){std::size_t end=0;int slot=std::stoi(p.second,&end);if(end!=p.second.size())throw std::invalid_argument("Invalid serialized material reference");p.second=std::to_string(map_slot(slot));}};
        for(auto& object:scene->objects){
            if(cancellation_requested(callbacks)){result.cancelled=true;return result;}
            // A zero source object slot inherits source project material 1.
            // Geometry import must anchor that inheritance to the user's map,
            // rather than accidentally inheriting the target project's default.
            const int source_slot=scene->mode==ProjectReadMode::Geometry&&object.default_filament_slot==0?1:object.default_filament_slot;
            object.default_filament_slot=map_slot(source_slot);map_settings(object.settings);for(auto& range:object.layer_ranges)map_settings(range.settings);
            for(auto& part:object.parts){
                if(cancellation_requested(callbacks)){result.cancelled=true;return result;}
                part.default_filament_slot=map_slot(part.default_filament_slot);map_settings(part.settings);
                if(!part.material_facets.empty()){
                    std::vector<Slic3r::Vec3f> vertices;std::vector<Slic3r::Vec3i32> triangles;for(const auto& v:part.mesh->vertices)vertices.emplace_back(v.x,v.y,v.z);for(const auto& t:part.mesh->triangles)triangles.emplace_back(int(t.vertex_a),int(t.vertex_b),int(t.vertex_c));
                    Slic3r::TriangleMesh mesh(std::move(vertices),std::move(triangles));Slic3r::TriangleSelector selector(mesh);
                    selector.deserialize(native_facets(part.material_facets,part.mesh->triangles.size(),int(Slic3r::EnforcerBlockerType::ExtruderMax)));
                    Slic3r::EnforcerBlockerStateMap states{};states[0]=Slic3r::EnforcerBlockerType::NONE;
                    for(std::size_t i=1;i<states.size();++i){const int slot=i<=mapping.size()?int(mapping[i-1]):0;if(slot>int(Slic3r::EnforcerBlockerType::ExtruderMax))throw std::invalid_argument("Mapped painted material exceeds native painting capacity");states[i]=static_cast<Slic3r::EnforcerBlockerType>(slot);}
                    if(cancellation_requested(callbacks)){result.cancelled=true;return result;}
                    selector.remap_triangle_state(states);
                    if(cancellation_requested(callbacks)){result.cancelled=true;return result;}
                    const auto data=selector.serialize();part.material_facets={};for(const auto& r:data.triangles_to_split)part.material_facets.roots.push_back({unsigned(r.triangle_idx),unsigned(r.bitstream_start_idx)});for(bool bit:data.bitstream)part.material_facets.bitstream.push_back(bit?1:0);
                }
            }
        }
        if(cancellation_requested(callbacks)){result.cancelled=true;return result;}
        // The returned geometry now belongs to target's material domain.
        // Discard the source palette-expansion candidate: installing it would
        // undo the caller's explicit mapping back to the existing materials.
        scene->source_filaments=project_filament_slots(config,count);
        result=input;
        result.configuration={};result.configuration_changed=false;
        result.filament_color_cursor=impl_->filament_color_cursor;
        result.scene=std::move(scene);result.success=true;
    } catch(const std::exception& e){result.diagnostics.push_back({"error","project.material_mapping",{},e.what()});}
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
        // API paths are UTF-8; implicit narrow paths use the Windows ANSI code page.
        if (request.path.empty() ||
            !fs::is_regular_file(fs::u8path(request.path), file_error) || file_error) {
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

            ProjectImportObject imported_object;
            imported_object.id = "object-" + std::to_string(object_index + 1);
            imported_object.name = !object->name.empty()
                ? object->name : imported_object.id;
            for (std::size_t volume_index = 0; volume_index < object->volumes.size(); ++volume_index) {
                const Slic3r::ModelVolume* volume = object->volumes[volume_index];
                if (volume == nullptr) continue;
                if (volume->mesh().empty()) {
                    result.diagnostics.push_back({
                        "model",
                        "Skipping empty 3MF volume " +
                            std::to_string(volume_index + 1) + " in " +
                            imported_object.id,
                        true});
                    continue;
                }

                ProjectImportPart part;
                part.id = imported_object.id +
                    "/part-" + std::to_string(volume_index + 1);
                part.name = !volume->name.empty() ? volume->name
                    : imported_object.name;
                if (volume->is_model_part()) {
                    part.role = ProjectImportPartRole::Model;
                } else if (volume->is_support_enforcer()) {
                    part.role = ProjectImportPartRole::SupportEnforcer;
                } else if (volume->is_support_blocker()) {
                    part.role = ProjectImportPartRole::SupportBlocker;
                } else {
                    result.diagnostics.push_back({
                        "model",
                        "Unsupported 3MF volume role in " + part.id,
                        false});
                    release_import_resources();
                    return result;
                }
                const Slic3r::Transform3d volume_matrix = volume->get_matrix();
                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        part.local_transform[
                            static_cast<std::size_t>(row * 4 + column)] =
                            volume_matrix(row, column);
                    }
                }
                const int extruder_id = volume->extruder_id();
                if (extruder_id > 0) {
                    part.filament_id = "filament-" + std::to_string(extruder_id);
                    maximum_filament_id = std::max(
                        maximum_filament_id, static_cast<std::size_t>(extruder_id));
                }

                const auto& indexed = volume->mesh().its;
                part.vertices.reserve(indexed.vertices.size());
                for (const auto& vertex : indexed.vertices) {
                    part.vertices.push_back({vertex.x(), vertex.y(), vertex.z()});
                }
                part.triangles.reserve(indexed.indices.size());
                for (const auto& triangle : indexed.indices) {
                    part.triangles.push_back({
                        static_cast<std::uint32_t>(triangle.x()),
                        static_cast<std::uint32_t>(triangle.y()),
                        static_cast<std::uint32_t>(triangle.z())});
                }

                const auto& facet_labels = volume->mmu_segmentation_facets.get_data();
                part.facet_labels.roots.reserve(facet_labels.triangles_to_split.size());
                for (const auto& root : facet_labels.triangles_to_split) {
                    if (root.triangle_idx < 0 || root.bitstream_start_idx < 0) continue;
                    part.facet_labels.roots.push_back({
                        static_cast<std::uint32_t>(root.triangle_idx),
                        static_cast<std::uint32_t>(root.bitstream_start_idx)});
                }
                part.facet_labels.bitstream.reserve(facet_labels.bitstream.size());
                for (const bool bit : facet_labels.bitstream) {
                    part.facet_labels.bitstream.push_back(bit ? 1u : 0u);
                }
                imported_object.parts.push_back(std::move(part));
            }

            if (imported_object.parts.empty()) {
                result.diagnostics.push_back({
                    "model",
                    "Skipping 3MF object without non-empty supported volumes: " +
                        imported_object.id,
                    true});
                continue;
            }

            const auto append_instance = [&](const Slic3r::Transform3d& matrix,
                                             bool printable,
                                             std::size_t instance_index) {
                ProjectImportInstance instance;
                instance.name = imported_object.name;
                if (object->instances.size() > 1) {
                    instance.name += " " + std::to_string(instance_index + 1);
                }
                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        instance.transform[
                            static_cast<std::size_t>(row * 4 + column)] =
                            matrix(row, column);
                    }
                }
                instance.printable = object->printable && printable;
                imported_object.instances.push_back(std::move(instance));
            };
            if (object->instances.empty()) {
                append_instance(Slic3r::Transform3d::Identity(), true, 0);
            } else {
                for (std::size_t instance_index = 0;
                     instance_index < object->instances.size(); ++instance_index) {
                    const Slic3r::ModelInstance* instance =
                        object->instances[instance_index];
                    if (instance != nullptr) {
                        append_instance(
                            instance->get_matrix(), instance->printable,
                            instance_index);
                    }
                }
            }
            if (imported_object.instances.empty()) {
                append_instance(Slic3r::Transform3d::Identity(), true, 0);
            }
            result.objects.push_back(std::move(imported_object));
        }

        if (result.objects.empty()) {
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
                    auto candidate_bundle = *preset_bundle;
                    // Migrate only absent machine-output options, before defaults erase
                    // the distinction between missing and explicitly disabled values.
                    const auto *official = candidate_bundle.printers.find_system_preset_by_model_and_variant(
                        machine->id, variant->id);
                    const bool known_lineage = official && official->is_system &&
                        (printer_preset == official->name ||
                         std::find(inherited_presets.begin(), inherited_presets.end(), official->name) != inherited_presets.end()) &&
                        (printer_model.empty() || printer_model == machine->id) &&
                        (printer_variant.empty() || printer_variant == variant->id);
                    for (const char *key : {"toolchange_z_lift", "part_cooling_fan_index"}) {
                        if (config.has(key)) continue;
                        if (known_lineage) {
                            if (const auto *value = official->config.option(key)) {
                                config.set_key_value(key, value->clone());
                                result.diagnostics.push_back({"config", std::string("Inherited missing machine setting from installed preset: ") + key, true});
                            }
                        } else {
                            result.diagnostics.push_back({"config", std::string("Machine preset origin is unknown; keeping the legacy default for ") + key +
                                ". Select an updated machine preset to enable its configured behavior.", true});
                        }
                    }
                    restore_project_tower_modes(config, plate_data.size());
                    Slic3r::DynamicPrintConfig resolved_config;
                    resolved_config.apply(Slic3r::FullPrintConfig::defaults());
                    resolved_config.apply(config);
                    Slic3r::Preset::normalize(resolved_config);

                    if (!project_presets.empty()) {
                        candidate_bundle.load_project_embedded_presets(
                            project_presets,
                            Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
                    }

                    // The preset loader may refresh non-dirty values from a newer
                    // system preset. These resolved machine options are authoritative,
                    // including an explicitly disabled value in the project.
                    Slic3r::DynamicPrintConfig machine_output_options;
                    machine_output_options.apply_only(resolved_config,
                        {"toolchange_z_lift", "part_cooling_fan_index"}, true);
                    candidate_bundle.load_config_model(
                        request.path, std::move(resolved_config), file_version);
                    candidate_bundle.printers.get_edited_preset().config.apply(machine_output_options);

                    auto active = std::unique_ptr<Config>(
                        new Config(serialized_values(candidate_bundle.full_config())));
                    // Ownership is loaded from the project, not inferred from
                    // materials. Reject invalid assignments before any commit.
                    Slic3r::validate_material_tool_bindings(tool_config(candidate_bundle.full_config()));
                    for (const auto& diagnostic : active->validate()) {
                        result.diagnostics.push_back({
                            diagnostic.key, diagnostic.message, true});
                    }

                    auto next=std::make_shared<ConfigurationState::Impl>();
                    next->resource_directory=impl_->resource_directory;
                    next->bundle=std::make_shared<Slic3r::PresetBundle>(std::move(candidate_bundle));
                    next->config=std::move(active);
                    next->machine=describe_project_machine(dynamic_config(next->config->snapshot()));
                    next->machine.project_local=false;
                    next->selection.machine_model_id=machine->id;
                    next->selection.machine_variant_id=variant->id;
                    next->selection.printer_preset_id=next->bundle->printers.get_selected_preset_name();
                    next->selection.process_preset_id=next->bundle->prints.get_selected_preset_name();
                    next->selection.filament_preset_ids=next->bundle->filament_presets;
                    impl_->apply_installed_machine_capabilities(next->machine,next->selection);
                    const auto imported=next->bundle->full_config();
                    const auto imported_tools=filament_tool_map(&imported,next->bundle->filament_presets.size());
                    next->selection.filament_physical_tools.assign(imported_tools.begin(),imported_tools.end());
                    next->compatible_processes=compatible_presets(next->bundle->prints,next->selection.process_preset_id);
                    next->compatible_filaments=compatible_presets(next->bundle->filaments,next->selection.filament_preset_ids.empty()?std::string{}:next->selection.filament_preset_ids.front());
                    Library viewer;viewer.impl_->resource_directory=impl_->resource_directory;viewer.impl_->active=next;viewer.impl_->active_revision=1;
                    next->prepared_view=*viewer.active_config();next->prepared_view.revision=0;
                    if(cancellation_requested(callbacks)){result.cancelled=true;release_import_resources();return result;}
                    impl_->active=std::move(next);
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
