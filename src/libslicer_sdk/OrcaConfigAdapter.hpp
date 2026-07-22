#pragma once

#include <libslicer/v1/ConfigSchema.hpp>

#include "libslic3r/PrintConfig.hpp"

namespace libslicer::v1::detail {

enum class CoreReferenceEncoding { zero_based, one_based_zero_sentinel };

struct FilamentReferenceRule {
    FilamentReferenceKind public_kind;
    CoreReferenceEncoding core_encoding;
    bool                  value_nullable;
    bool                  item_nullable;
};

const FilamentReferenceRule *filament_reference_rule(const std::string &canonical_option_id);

Result<ConfigSchema> build_orca_fff_schema();

Result<ConfigValue> core_option_to_value(const Slic3r::ConfigOptionDef &definition,
                                         const Slic3r::ConfigOption &option);

Result<Slic3r::ConfigOptionUniquePtr> value_to_core_option(
    const Slic3r::ConfigOptionDef &definition, const ConfigValue &value);

Result<ConfigValues> core_config_to_values(const Slic3r::ConfigBase &config);

Result<ConfigPatch> core_config_diff_to_patch(const Slic3r::ConfigBase &inherited,
                                              const Slic3r::ConfigBase &effective,
                                              const ConfigSchema &schema);

Result<void> apply_patch_to_core_config(const ConfigPatch &patch,
                                        Slic3r::DynamicPrintConfig &config);

} // namespace libslicer::v1::detail
