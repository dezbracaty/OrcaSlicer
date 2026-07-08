#ifndef slic3r_ConfigSDK_hpp_
#define slic3r_ConfigSDK_hpp_

// Public, DTO-only ConfigSDK surface.
//
// This header is the installed, externally consumable API. It must NOT expose
// any Orca internal type (DynamicPrintConfig, PresetBundle, Preset, ...) or pull
// in any Orca internal header (PrintConfig.hpp, ...). Everything crosses the
// boundary as plain data / JSON strings. In-tree callers that need the
// DynamicPrintConfig bridge include ConfigSDK_internal.hpp instead.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Export/visibility control. When built as the shared config_sdk library the
// build defines LIBSLICER_CONFIG_SDK_SHARED (+ LIBSLICER_CONFIG_SDK_EXPORTS while
// compiling the library itself). For in-tree static use the macro is empty.
#if defined(LIBSLICER_CONFIG_SDK_SHARED)
#  if defined(_WIN32)
#    if defined(LIBSLICER_CONFIG_SDK_EXPORTS)
#      define LIBSLICER_CONFIG_SDK_API __declspec(dllexport)
#    else
#      define LIBSLICER_CONFIG_SDK_API __declspec(dllimport)
#    endif
#  else
#    define LIBSLICER_CONFIG_SDK_API __attribute__((visibility("default")))
#  endif
#else
#  define LIBSLICER_CONFIG_SDK_API
#endif

namespace Slic3r::libslicer {

enum class ConfigScope
{
    Printer,
    Process,
    Filament,
    Project,
    Object,
    Part,
    Internal,
    Unknown
};

enum class ConfigCardinality
{
    Scalar,
    Filament,
    PhysicalExtruder,
    PrinterVariantLookup,
    FilamentExtruderVariant,
    ProcessExtruderVariant,
    Plate,
    Matrix,
    Unknown
};

enum class ConfigValueType
{
    Bool,
    Int,
    Float,
    String,
    Enum,
    Point,
    Percent,
    Vector,
    Matrix,
    Unknown
};

enum class ConfigIssueSeverity
{
    Warning,
    Error
};

enum class UnknownKeyPolicy
{
    Warning,
    Error
};

// How parse/resolve APIs treat config keys that are required (by the schema or
// by the resolver's own invariants, e.g. printer_settings_id) but absent from
// the input. Warning: report an issue but keep going with whatever is present
// (no silent default substitution — the field is simply left unset).
// Error: same report, but the issue is Error severity so has_config_errors()
// trips and callers know the result should not be trusted.
enum class MissingRequiredPolicy
{
    Warning,
    Error
};

enum class PresetKind
{
    Printer,
    Process,
    Filament,
    SlaPrinter,
    SlaProcess,
    SlaMaterial
};

struct ConfigEnumOption
{
    std::string value;
    std::string label;
};

struct ConfigDefinition
{
    std::string key;
    ConfigValueType type { ConfigValueType::Unknown };
    std::string label;
    std::vector<ConfigEnumOption> enum_options;
    std::string unit;
    std::optional<double> min;
    std::optional<double> max;
    std::string default_value;
    std::string default_json;
    bool nullable { false };
    bool internal { false };
    ConfigScope scope { ConfigScope::Unknown };
    ConfigCardinality cardinality { ConfigCardinality::Unknown };
};

struct ConfigIssue
{
    std::string code;
    std::string field;
    std::string message;
    ConfigIssueSeverity severity { ConfigIssueSeverity::Error };
};

// Back-compat alias for in-tree callers written against the old name.
using ConfigValidationIssue = ConfigIssue;

struct ConfigParseOptions
{
    UnknownKeyPolicy unknown_key_policy { UnknownKeyPolicy::Error };
    MissingRequiredPolicy missing_required_policy { MissingRequiredPolicy::Warning };
    bool allow_legacy_substitution { true };
    bool canonicalize { true };
};

struct ConfigParseResult
{
    std::string canonical_json;
    std::vector<ConfigIssue> issues;
};

struct FilamentSlotRequest
{
    int slot_index { 0 };
    std::string filament_preset_id;
    std::string color;
    std::string color_type;
    std::string filament_type;
    std::string slot_overrides_json;
};

struct ConfigResolutionRequest
{
    std::filesystem::path resources_dir;
    std::filesystem::path data_dir;
    std::vector<std::filesystem::path> vendor_bundle_dirs;
    std::vector<std::filesystem::path> user_preset_dirs;
    std::vector<std::filesystem::path> project_preset_files;

    std::string printer_preset_id;
    std::string process_preset_id;
    std::vector<FilamentSlotRequest> filament_slots;

    std::string project_overrides_json;
    std::string printer_overrides_json;
    std::string process_overrides_json;

    int plate_index { -1 };
    bool apply_extruder { false };
    bool strict { true };
};

struct ConfigResolutionResult
{
    std::string full_config_json;
    std::string normalized_diff_json;
    std::vector<ConfigIssue> issues;
};

struct PresetCatalogRequest
{
    std::filesystem::path resources_dir;
    std::filesystem::path data_dir;
    std::vector<std::filesystem::path> vendor_bundle_dirs;
    std::vector<std::filesystem::path> user_preset_dirs;
    std::vector<std::filesystem::path> project_preset_files;

    // Optional active selections. When provided, the catalog marks process and
    // filament compatibility using Orca's native compatibility logic.
    std::string printer_preset_id;
    std::string process_preset_id;

    bool strict { true };
    bool compatible_only { false };
};

struct PresetCatalogEntry
{
    PresetKind kind { PresetKind::Printer };
    std::string preset_id;
    std::string name;
    std::string alias;
    std::string vendor_id;
    std::string vendor_name;
    std::string inherits;
    std::string setting_id;
    std::string filament_id;
    std::string base_id;
    bool is_system { false };
    bool is_user { false };
    bool is_project_embedded { false };
    bool is_default { false };
    bool is_visible { true };
    bool is_compatible { true };
};

struct PresetCatalogResult
{
    std::vector<PresetCatalogEntry> printers;
    std::vector<PresetCatalogEntry> processes;
    std::vector<PresetCatalogEntry> filaments;
    std::vector<ConfigIssue> issues;
};

struct Project3mfExtractionRequest
{
    std::filesystem::path project_file;
    std::filesystem::path data_dir;
    int plate_index { 0 };
    bool strict { true };
};

struct Project3mfPlate
{
    int plate_index { -1 };
    std::string plate_name;
    std::string printer_model_id;
    std::string nozzle_diameters;
    std::string config_json;
    std::vector<int> filament_maps;
};

struct Project3mfExtractionResult
{
    std::string project_config_json;
    std::string printer_overrides_json;
    std::string process_overrides_json;
    std::string project_overrides_json;
    std::string object_overrides_json;
    std::string part_overrides_json;
    std::string printer_preset_id;
    std::string process_preset_id;
    std::vector<FilamentSlotRequest> filament_slots;
    std::vector<PresetCatalogEntry> embedded_presets;
    std::vector<std::filesystem::path> extracted_project_preset_files;
    std::vector<Project3mfPlate> plates;
    bool is_bbl_3mf { false };
    bool is_orca_3mf { false };
    std::string file_version;
    std::vector<ConfigIssue> issues;
};

struct ConfigValidationRequest
{
    std::string full_config_json;
    int plate_index { -1 };
    bool run_print_validate { false };
};

struct ConfigDiffRequest
{
    // Canonical full_config_json documents (as produced by resolve_fff_config /
    // parse_config_json), NOT raw preset files.
    std::string base_config_json;
    std::string target_config_json;
};

struct ConfigDiffResult
{
    // Only keys present in both documents whose values differ, keyed/sorted the
    // same way as full_config_json. Re-parses through parse_config_json.
    std::string diff_json;
    std::vector<ConfigIssue> issues;
};

// --- SDK identity / capabilities (spec §6.1) ---------------------------------

LIBSLICER_CONFIG_SDK_API std::string config_sdk_version();
LIBSLICER_CONFIG_SDK_API std::vector<std::string> config_sdk_capabilities();

// --- Schema (spec §6.2) ------------------------------------------------------

LIBSLICER_CONFIG_SDK_API std::vector<ConfigDefinition> list_config_definitions();
LIBSLICER_CONFIG_SDK_API std::optional<ConfigDefinition> find_config_definition(std::string_view key);

// Retained names (existing callers/tests). get_config_definition returns a
// definition whose key is empty when the key is unknown.
LIBSLICER_CONFIG_SDK_API std::vector<ConfigDefinition> get_config_definitions();
LIBSLICER_CONFIG_SDK_API ConfigDefinition get_config_definition(const std::string& key);

// --- Strict parse / canonicalize (spec §6.3) ---------------------------------

LIBSLICER_CONFIG_SDK_API ConfigParseResult parse_config_json(std::string_view json_text,
                                                              const ConfigParseOptions& options = {});
LIBSLICER_CONFIG_SDK_API ConfigParseResult parse_config_file(const std::filesystem::path& path,
                                                              const ConfigParseOptions& options = {});

// --- Preset catalog (spec §6.4) ----------------------------------------------

LIBSLICER_CONFIG_SDK_API PresetCatalogResult load_preset_catalog(const PresetCatalogRequest& request);

// --- Full config resolution (spec §6.5) --------------------------------------

LIBSLICER_CONFIG_SDK_API ConfigResolutionResult resolve_fff_config(const ConfigResolutionRequest& request);

// --- Validation (spec §6.6) --------------------------------------------------

LIBSLICER_CONFIG_SDK_API std::vector<ConfigIssue> validate_resolved_config(const ConfigValidationRequest& request);

LIBSLICER_CONFIG_SDK_API bool has_config_errors(const std::vector<ConfigIssue>& issues);

// --- 3MF project extraction (spec §6.7) --------------------------------------

LIBSLICER_CONFIG_SDK_API Project3mfExtractionResult extract_project_3mf_config(
    const Project3mfExtractionRequest& request);

// --- Diff (spec §6.8) ---------------------------------------------------------

LIBSLICER_CONFIG_SDK_API ConfigDiffResult diff_config(const ConfigDiffRequest& request);

} // namespace Slic3r::libslicer

#endif
