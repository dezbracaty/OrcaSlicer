#pragma once

#include "Config.hpp"

#include <string>
#include <string_view>

namespace libslicer::v1 {

class ConfigSchema;
class EffectiveConfiguration;

Result<std::string> export_json(const ConfigPatch &patch, const ConfigSchema &schema);
Result<ConfigPatch> import_json(std::string_view json, const ConfigSchema &schema);
Result<std::string> export_json(const EffectiveConfiguration &configuration);

} // namespace libslicer::v1
