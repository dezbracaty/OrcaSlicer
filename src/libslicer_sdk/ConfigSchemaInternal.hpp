#pragma once

#include <libslicer/v1/ConfigSchema.hpp>

#include <functional>

namespace libslicer::v1::detail {

using OptionEvaluator = std::function<Result<OptionEvaluation>(
    const OptionId &, const ConfigValues &, const ConfigValidationContext &)>;

struct ConfigSchemaAccess {
    static ValueDescriptor scalar(ConfigValueShape shape,
                                  bool value_nullable = false,
                                  std::optional<NumericConstraint> numeric = std::nullopt,
                                  std::optional<FloatOrPercentConstraint> float_or_percent = std::nullopt,
                                  std::vector<std::string> enum_values = {},
                                  std::string canonical_unit = {});

    static ValueDescriptor list(ValueDescriptor item,
                                std::size_t min_items,
                                std::size_t max_items,
                                bool value_nullable = false,
                                bool fixed_length = false);

    static Result<ConfigSchema> make(std::string schema_id,
                                     std::uint32_t schema_version,
                                     std::vector<OptionDescriptor> options,
                                     std::vector<std::string> typed_owned_options,
                                     OptionEvaluator evaluator);

    static const OptionDescriptor *find_descriptor(const ConfigSchema &schema,
                                                   const OptionId &option);
    static const OptionDescriptor *find_descriptor(const ConfigSchema &schema,
                                                   const std::string &option);

    static Result<void> validate_structure(const ConfigSchema &schema,
                                           const ConfigPatch &patch,
                                           const ConfigValidationContext &context);

    static bool is_generic_option(const ConfigSchema &schema, const OptionId &option);

    static std::size_t option_index_build_count(const ConfigSchema &schema) noexcept;
    static std::size_t option_index_mutation_count(const ConfigSchema &schema) noexcept;
};

} // namespace libslicer::v1::detail
