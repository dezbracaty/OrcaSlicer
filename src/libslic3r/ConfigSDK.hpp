#ifndef slic3r_ConfigSDK_hpp_
#define slic3r_ConfigSDK_hpp_

#include "PrintConfig.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::libslicer {

enum class ConfigScope
{
    Printer,
    Process,
    Filament,
    Project,
    Object,
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
    ConfigScope scope { ConfigScope::Unknown };
    ConfigCardinality cardinality { ConfigCardinality::Unknown };
};

struct ConfigValidationIssue
{
    std::string code;
    std::string field;
    std::string message;
    ConfigIssueSeverity severity { ConfigIssueSeverity::Error };
};

class ResolvedConfig
{
public:
    ResolvedConfig();
    explicit ResolvedConfig(DynamicPrintConfig config);

    std::string to_json() const;
    static ResolvedConfig from_json(std::string_view json_text, std::vector<ConfigValidationIssue>* issues = nullptr);
    static ResolvedConfig load_json_file(const std::filesystem::path& path, std::vector<ConfigValidationIssue>* issues = nullptr);

    bool apply_json(std::string_view json_text, std::vector<ConfigValidationIssue>* issues = nullptr);
    bool apply_json_file(const std::filesystem::path& path, std::vector<ConfigValidationIssue>* issues = nullptr);

    const DynamicPrintConfig& dynamic_config() const { return m_config; }
    DynamicPrintConfig& dynamic_config() { return m_config; }

private:
    DynamicPrintConfig m_config;
};

struct ConfigResolutionRequest
{
    std::filesystem::path resources_dir;
    std::filesystem::path data_dir;
    std::vector<std::filesystem::path> vendor_bundle_dirs;
    std::vector<std::filesystem::path> user_preset_dirs;
    std::vector<std::filesystem::path> project_preset_files;

    std::string printer_preset_id;
    std::string print_preset_id;
    std::vector<std::string> filament_preset_ids;

    ResolvedConfig project_config_edits;
    std::vector<int> filament_map;
    bool apply_extruder { false };
};

struct ConfigResolutionResult
{
    ResolvedConfig config;
    std::vector<std::string> warnings;
    std::vector<ConfigValidationIssue> issues;
};

std::vector<ConfigDefinition> get_config_definitions();
ConfigDefinition get_config_definition(const std::string& key);

ConfigResolutionResult resolve_fff_config(const ConfigResolutionRequest& request);

std::vector<ConfigValidationIssue> validate_resolved_config(const ResolvedConfig& config, int plate_index = 0);
std::vector<ConfigValidationIssue> validate_resolved_config(const DynamicPrintConfig& config, int plate_index = 0);

bool has_config_errors(const std::vector<ConfigValidationIssue>& issues);

} // namespace Slic3r::libslicer

#endif
