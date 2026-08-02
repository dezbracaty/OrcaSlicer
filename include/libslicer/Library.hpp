#pragma once

#include "Config.hpp"
#include "Export.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace libslicer {

LIBSLICER_API const char* version() noexcept;

struct MachineVariantOption
{
    std::string id;
    std::string name;
    double nozzle_diameter{0.0};
    double printable_width{0.0};
    double printable_depth{0.0};
    double printable_height{0.0};
    std::string printer_preset_id;
};

struct MachineModelOption
{
    std::string id;
    std::string vendor_id;
    std::string name;
    std::string family;
    std::string cover_image_path;
    std::string bed_model_path;
    std::string bed_texture_path;
    std::vector<MachineVariantOption> variants;
};

struct BuildPlateOption
{
    std::string value;
    std::string name;
    std::string image_path;
};

struct PresetOption
{
    std::string id;
    std::string name;
    bool is_default{false};
};

struct ConfigSelection
{
    std::string machine_model_id;
    std::string machine_variant_id;
    std::string process_preset_id;
    std::vector<std::string> filament_preset_ids;
};

struct ResolvedSelection
{
    std::string machine_model_id;
    std::string machine_variant_id;
    std::string printer_preset_id;
    std::string process_preset_id;
    std::vector<std::string> filament_preset_ids;
};

struct LibraryOptions
{
    // Empty in production: Library locates the resources installed with it.
    // Tests and SDK embedding may provide an explicit libslicer resource root.
    std::string resource_directory;
    std::vector<std::string> vendors;
};

struct ConfigCreateResult
{
    bool success{false};
    ResolvedSelection selection;
    std::vector<PresetOption> compatible_processes;
    std::vector<PresetOption> compatible_filaments;
    std::vector<ConfigDiagnostic> diagnostics;
    std::unique_ptr<Config> config;

    explicit operator bool() const noexcept { return success; }
};

class LIBSLICER_API Library final
{
public:
    static std::unique_ptr<Library> open(const LibraryOptions& options = {},
                                         std::vector<ConfigDiagnostic>* diagnostics = nullptr);

    Library(const Library&) = delete;
    Library(Library&&) noexcept;
    Library& operator=(const Library&) = delete;
    Library& operator=(Library&&) noexcept;
    ~Library();

    const std::string& resource_directory() const noexcept;
    const std::vector<MachineModelOption>& machine_models() const noexcept;
    const std::vector<BuildPlateOption>& build_plate_options() const noexcept;
    ConfigCreateResult create_config(const ConfigSelection& selection) const;

private:
    Library();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace libslicer
