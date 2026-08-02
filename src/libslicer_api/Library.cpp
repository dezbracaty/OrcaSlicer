#include <libslicer/Library.hpp>

#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Utils.hpp>
#include <libslic3r/libslic3r.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
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
        if (process_name.empty()) {
            process_name = active_printer.config.opt_string("default_print_profile");
        }
        if (process_name.empty() || selected.prints.find_preset(process_name, false) == nullptr) {
            process_name = first_compatible_preset_name(selected.prints);
        }
        if (process_name.empty()) {
            throw std::runtime_error("Selected machine has no compatible process preset");
        }
        selected.prints.select_preset_by_name(process_name, true);

        std::vector<std::string> filament_names = selection.filament_preset_ids;
        if (filament_names.empty()) {
            if (const auto* defaults = active_printer.config.option<Slic3r::ConfigOptionStrings>("default_filament_profile")) {
                filament_names = defaults->values;
            }
        }
        if (filament_names.empty() || selected.filaments.find_preset(filament_names.front(), false) == nullptr) {
            const std::string fallback = first_compatible_preset_name(selected.filaments);
            if (fallback.empty()) {
                throw std::runtime_error("Selected machine has no compatible filament preset");
            }
            filament_names = {fallback};
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

} // namespace libslicer
