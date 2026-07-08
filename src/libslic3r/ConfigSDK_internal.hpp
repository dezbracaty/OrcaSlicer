#ifndef slic3r_ConfigSDK_internal_hpp_
#define slic3r_ConfigSDK_internal_hpp_

// Internal ConfigSDK bridge. NOT installed and NOT part of the public ABI.
//
// In-tree callers that already hold a DynamicPrintConfig (e.g. the slicing
// worker) use this header to reach the config-SDK logic without paying a
// JSON round-trip. External consumers must use the DTO-only ConfigSDK.hpp.

#include "ConfigSDK.hpp"

#include "PrintConfig.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::libslicer {

// A DynamicPrintConfig holder with JSON <-> Orca-typed conversion. Lives in the
// internal surface because it exposes DynamicPrintConfig directly.
class ResolvedConfig
{
public:
    ResolvedConfig();
    explicit ResolvedConfig(DynamicPrintConfig config);

    std::string to_json() const;
    static ResolvedConfig from_json(std::string_view json_text, std::vector<ConfigIssue>* issues = nullptr);
    static ResolvedConfig load_json_file(const std::filesystem::path& path, std::vector<ConfigIssue>* issues = nullptr);

    bool apply_json(std::string_view json_text, std::vector<ConfigIssue>* issues = nullptr);
    bool apply_json_file(const std::filesystem::path& path, std::vector<ConfigIssue>* issues = nullptr);

    const DynamicPrintConfig& dynamic_config() const { return m_config; }
    DynamicPrintConfig& dynamic_config() { return m_config; }

private:
    DynamicPrintConfig m_config;
};

// Validation directly over a DynamicPrintConfig (used by the worker). The
// public ConfigValidationRequest overload in ConfigSDK.hpp funnels into this.
std::vector<ConfigIssue> validate_resolved_config(const DynamicPrintConfig& config, int plate_index = 0);
std::vector<ConfigIssue> validate_resolved_config(const ResolvedConfig& config, int plate_index = 0);

} // namespace Slic3r::libslicer

#endif
