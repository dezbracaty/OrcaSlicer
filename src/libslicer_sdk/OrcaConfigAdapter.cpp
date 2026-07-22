#include "OrcaConfigAdapter.hpp"

#include "ConfigSchemaInternal.hpp"

#include "libslic3r/Preset.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace libslicer::v1::detail {

const FilamentReferenceRule *filament_reference_rule(const std::string &canonical_option_id)
{
    static const std::map<std::string, FilamentReferenceRule> rules{
        {"extruder", {FilamentReferenceKind::logical_slot,
                       CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"support_filament", {FilamentReferenceKind::logical_slot,
                               CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"support_interface_filament", {FilamentReferenceKind::logical_slot,
                                         CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"wipe_tower_filament", {FilamentReferenceKind::logical_slot,
                                  CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"outer_wall_filament_id", {FilamentReferenceKind::logical_slot,
                                     CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"inner_wall_filament_id", {FilamentReferenceKind::logical_slot,
                                     CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"sparse_infill_filament_id", {FilamentReferenceKind::logical_slot,
                                        CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"internal_solid_filament_id", {FilamentReferenceKind::logical_slot,
                                         CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"top_surface_filament_id", {FilamentReferenceKind::logical_slot,
                                      CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
        {"bottom_surface_filament_id", {FilamentReferenceKind::logical_slot,
                                         CoreReferenceEncoding::one_based_zero_sentinel, true, false}},
    };
    const auto found = rules.find(canonical_option_id);
    return found == rules.end() ? nullptr : &found->second;
}

namespace {

template<class T>
Result<T> failure(ErrorCode code, std::string message, const std::string &option)
{
    return ResultAccess::failure<T>(code, std::move(message), "/configuration/" + option);
}

template<class T>
const T *checked_option(const Slic3r::ConfigOption &option)
{
    return dynamic_cast<const T *>(&option);
}

Result<ConfigValue> make_list(ConfigValueShape item_shape, std::vector<ConfigValue> items,
                              const std::string &option)
{
    auto result = ConfigValue::list(std::move(item_shape), std::move(items));
    if (!result.has_value())
        return failure<ConfigValue>(ErrorCode::internal,
                                    "Core configuration produced an inconsistent list shape", option);
    return result;
}

template<class Vector, class Factory>
Result<ConfigValue> vector_to_value(const Slic3r::ConfigOption &option,
                                    ConfigValueShape item_shape, Factory factory,
                                    const std::string &option_id)
{
    const auto *typed = checked_option<Vector>(option);
    const auto *base = dynamic_cast<const Slic3r::ConfigOptionVectorBase *>(&option);
    if (!typed || !base)
        return failure<ConfigValue>(ErrorCode::internal,
                                    "Core configuration option has an unexpected runtime type", option_id);

    std::vector<ConfigValue> items;
    items.reserve(typed->values.size());
    for (std::size_t index = 0; index < typed->values.size(); ++index) {
        if (base->nullable() && base->is_nil(index))
            items.push_back(ConfigValue::null(item_shape));
        else
            items.push_back(factory(typed->values[index]));
    }
    return make_list(std::move(item_shape), std::move(items), option_id);
}

Result<ConfigValue> enum_vector_to_value(const Slic3r::ConfigOptionDef &definition,
                                         const Slic3r::ConfigOption &option)
{
    const auto *typed = checked_option<Slic3r::ConfigOptionVector<int>>(option);
    const auto *base = dynamic_cast<const Slic3r::ConfigOptionVectorBase *>(&option);
    if (!typed || !base || !definition.enum_keys_map)
        return failure<ConfigValue>(ErrorCode::internal,
                                    "Core enum option is missing its value map", definition.opt_key);

    std::vector<ConfigValue> items;
    items.reserve(typed->values.size());
    const auto shape = ConfigValueShape::scalar(ConfigValueType::enumeration);
    for (std::size_t index = 0; index < typed->values.size(); ++index) {
        if (base->nullable() && base->is_nil(index)) {
            items.push_back(ConfigValue::null(shape));
            continue;
        }
        const int value = typed->values[index];
        const auto it = std::find_if(definition.enum_keys_map->begin(), definition.enum_keys_map->end(),
                                     [value](const auto &entry) { return entry.second == value; });
        if (it == definition.enum_keys_map->end())
            return failure<ConfigValue>(ErrorCode::invalid_configuration,
                                        "Core enum option contains an unknown value", definition.opt_key);
        items.push_back(ConfigValue::enumeration(it->first));
    }
    return make_list(shape, std::move(items), definition.opt_key);
}

template<class Getter, class Output>
Result<Slic3r::ConfigOptionUniquePtr> build_vector_option(
    const Slic3r::ConfigOptionDef &definition, const ConfigValue &value,
    Getter getter, Output *output)
{
    const auto items = value.as_list();
    if (!items)
        return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                      "Configuration value is not a list",
                                                      definition.opt_key);
    output->values.reserve(items->size());
    for (std::size_t index = 0; index < items->size(); ++index) {
        const ConfigValue &item = (*items)[index];
        if (item.is_null()) {
            if (!definition.nullable) {
                delete output;
                return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                              "Configuration list item is not nullable",
                                                              definition.opt_key);
            }
            output->values.push_back(Output::nil_value());
            continue;
        }
        auto converted = getter(item);
        if (!converted) {
            delete output;
            return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                          "Configuration list item has the wrong type",
                                                          definition.opt_key);
        }
        output->values.push_back(*converted);
    }
    return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(output));
}

template<class Getter, class Output>
Result<Slic3r::ConfigOptionUniquePtr> build_nonnullable_vector_option(
    const Slic3r::ConfigOptionDef &definition, const ConfigValue &value,
    Getter getter, Output *output)
{
    const auto items = value.as_list();
    if (!items)
        return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                      "Configuration value is not a list",
                                                      definition.opt_key);
    output->values.reserve(items->size());
    for (const ConfigValue &item : *items) {
        auto converted = getter(item);
        if (!converted) {
            delete output;
            return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                          "Configuration list item has the wrong type",
                                                          definition.opt_key);
        }
        output->values.push_back(*converted);
    }
    return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(output));
}

std::optional<double> finite_bound(float value)
{
    return value <= -FLT_MAX / 2 || value >= FLT_MAX / 2
        ? std::nullopt
        : std::optional<double>{static_cast<double>(value)};
}

NumericConstraint numeric_constraint(const Slic3r::ConfigOptionDef &definition)
{
    return {finite_bound(definition.min), finite_bound(definition.max), definition.sidetext};
}

ValueDescriptor descriptor_for(const Slic3r::ConfigOptionDef &definition)
{
    const auto scalar = [&definition](ConfigValueType type, bool nullable = false) {
        const bool numeric = type == ConfigValueType::integer || type == ConfigValueType::decimal ||
                             type == ConfigValueType::percent;
        return ConfigSchemaAccess::scalar(
            ConfigValueShape::scalar(type), nullable,
            numeric ? std::optional<NumericConstraint>{numeric_constraint(definition)} : std::nullopt,
            std::nullopt,
            type == ConfigValueType::enumeration ? definition.enum_values : std::vector<std::string>{},
            definition.sidetext);
    };
    const auto float_or_percent = [&definition]() {
        FloatOrPercentConstraint constraint{numeric_constraint(definition), std::nullopt};
        if (!definition.ratio_over.empty())
            constraint.percent_base = OptionId(definition.ratio_over);
        return ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::float_or_percent),
                                          false, std::nullopt, std::move(constraint), {},
                                          definition.sidetext);
    };
    const auto list = [](ValueDescriptor item) {
        return ConfigSchemaAccess::list(std::move(item), 0,
                                        std::numeric_limits<std::size_t>::max());
    };

    if (const auto *reference = filament_reference_rule(definition.opt_key)) {
        if (definition.type != Slic3r::coInt)
            throw Slic3r::ConfigurationError("Scalar filament reference is not an integer");
        NumericConstraint constraint = numeric_constraint(definition);
        constraint.minimum = 0.0;
        if (constraint.maximum &&
            reference->core_encoding == CoreReferenceEncoding::one_based_zero_sentinel)
            constraint.maximum = std::max(0.0, *constraint.maximum - 1.0);
        return ConfigSchemaAccess::scalar(
            ConfigValueShape::scalar(ConfigValueType::integer), reference->value_nullable,
            std::move(constraint), std::nullopt, {}, definition.sidetext);
    }

    switch (definition.type) {
    case Slic3r::coFloat: return scalar(ConfigValueType::decimal);
    case Slic3r::coFloats:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::decimal),
                                               definition.nullable,
                                               numeric_constraint(definition), std::nullopt, {},
                                               definition.sidetext));
    case Slic3r::coInt: return scalar(ConfigValueType::integer);
    case Slic3r::coInts:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::integer),
                                               definition.nullable,
                                               numeric_constraint(definition), std::nullopt, {},
                                               definition.sidetext));
    case Slic3r::coString: return scalar(ConfigValueType::string);
    case Slic3r::coStrings:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::string)));
    case Slic3r::coPercent: return scalar(ConfigValueType::percent);
    case Slic3r::coPercents:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::percent),
                                               definition.nullable,
                                               numeric_constraint(definition), std::nullopt, {},
                                               definition.sidetext));
    case Slic3r::coFloatOrPercent: return float_or_percent();
    case Slic3r::coFloatsOrPercents: {
        FloatOrPercentConstraint constraint{numeric_constraint(definition), std::nullopt};
        if (!definition.ratio_over.empty())
            constraint.percent_base = OptionId(definition.ratio_over);
        return list(ConfigSchemaAccess::scalar(
            ConfigValueShape::scalar(ConfigValueType::float_or_percent), definition.nullable,
            std::nullopt, std::move(constraint), {}, definition.sidetext));
    }
    case Slic3r::coPoint: return scalar(ConfigValueType::point2);
    case Slic3r::coPoints:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::point2)));
    case Slic3r::coPoint3: return scalar(ConfigValueType::point3);
    case Slic3r::coBool: return scalar(ConfigValueType::boolean);
    case Slic3r::coBools:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::boolean),
                                               definition.nullable));
    case Slic3r::coEnum:
        return ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::enumeration),
                                          false, std::nullopt, std::nullopt,
                                          definition.enum_values);
    case Slic3r::coEnums:
        return list(ConfigSchemaAccess::scalar(ConfigValueShape::scalar(ConfigValueType::enumeration),
                                               definition.nullable, std::nullopt, std::nullopt,
                                               definition.enum_values));
    case Slic3r::coPointsGroups:
        return list(list(ConfigSchemaAccess::scalar(
            ConfigValueShape::scalar(ConfigValueType::point2))));
    case Slic3r::coIntsGroups:
        return list(list(ConfigSchemaAccess::scalar(
            ConfigValueShape::scalar(ConfigValueType::integer))));
    default:
        throw Slic3r::ConfigurationError("Unsupported FFF configuration option type");
    }
}

std::set<std::string> option_set(const std::vector<std::string> &options)
{
    return {options.begin(), options.end()};
}

enum class InternalOptionOwnership {
    generic_config,
    preset_selection,
    filament_map,
    structural_metadata
};

InternalOptionOwnership ownership_for(const std::string &key)
{
    static const std::set<std::string> preset_selection{
        "printer_settings_id", "print_settings_id", "filament_settings_id"};
    static const std::set<std::string> filament_map{"filament_map", "filament_map_mode"};
    static const std::set<std::string> structural{
        "inherits", "compatible_printers", "compatible_printers_condition",
        "compatible_prints", "compatible_prints_condition"};
    if (preset_selection.count(key)) return InternalOptionOwnership::preset_selection;
    if (filament_map.count(key)) return InternalOptionOwnership::filament_map;
    if (structural.count(key)) return InternalOptionOwnership::structural_metadata;
    return InternalOptionOwnership::generic_config;
}

FilamentReferenceKind filament_reference_for(const std::string &key)
{
    const auto *rule = filament_reference_rule(key);
    return rule ? rule->public_kind : FilamentReferenceKind::none;
}

// Visibility in the old GUI was implemented by ConfigManipulation callbacks.  The
// SDK keeps an independent, versioned registry so an option can never become
// implicitly visible merely because a rule was forgotten.  Rules which depend on
// GUI/device state, or which are only constrained by Print::validate(), are
// deliberately classified as core_validation_only.
constexpr std::uint32_t fff_rule_registry_version = 2;

enum class OptionRuleClass {
    unclassified,
    real_predicate,
    always_visible,
    core_validation_only
};

using VisibilityCondition = std::function<std::optional<bool>(
    const ConfigValues &, const ConfigValidationContext &)>;

struct OptionRule {
    std::uint32_t                 version;
    OptionRuleClass               classification;
    std::vector<std::string>      dependencies;
    VisibilityCondition           visibility;
    VisibilityCondition           compatibility;
};

using OptionRuleRegistry = std::map<std::string, OptionRule>;

std::optional<ConfigValue> lookup_rule_value(const std::string &key,
                                             const ConfigValues &candidate,
                                             const ConfigValidationContext &context)
{
    if (auto value = candidate.find(OptionId(key)))
        return value;
    if (context.process) {
        if (auto value = context.process->effective_values.find(OptionId(key)))
            return value;
    }
    if (context.printer) {
        if (auto value = context.printer->effective_values.find(OptionId(key)))
            return value;
    }
    return std::nullopt;
}

std::optional<double> rule_number(const std::string &key, const ConfigValues &candidate,
                                  const ConfigValidationContext &context)
{
    const auto value = lookup_rule_value(key, candidate, context);
    if (!value || value->is_null()) return std::nullopt;
    if (const auto integer = value->as_integer()) return static_cast<double>(*integer);
    if (const auto decimal = value->as_decimal()) return *decimal;
    if (const auto percent = value->as_percent()) return *percent;
    if (const auto mixed = value->as_float_or_percent()) return mixed->value;
    if (const auto list = value->as_list()) {
        for (const ConfigValue &item : *list) {
            if (item.is_null()) continue;
            if (const auto integer = item.as_integer()) return static_cast<double>(*integer);
            if (const auto decimal = item.as_decimal()) return *decimal;
            if (const auto percent = item.as_percent()) return *percent;
            if (const auto mixed = item.as_float_or_percent()) return mixed->value;
            break;
        }
    }
    return std::nullopt;
}

std::optional<bool> rule_boolean(const std::string &key, const ConfigValues &candidate,
                                 const ConfigValidationContext &context)
{
    const auto value = lookup_rule_value(key, candidate, context);
    if (!value || value->is_null()) return std::nullopt;
    if (const auto boolean = value->as_boolean()) return *boolean;
    return std::nullopt;
}

std::optional<std::string> rule_enum(const std::string &key, const ConfigValues &candidate,
                                     const ConfigValidationContext &context)
{
    const auto value = lookup_rule_value(key, candidate, context);
    return value && !value->is_null() ? value->as_enumeration() : std::nullopt;
}

std::optional<std::string> rule_string(const std::string &key, const ConfigValues &candidate,
                                       const ConfigValidationContext &context)
{
    const auto value = lookup_rule_value(key, candidate, context);
    return value && !value->is_null() ? value->as_string() : std::nullopt;
}

VisibilityCondition positive(std::string key)
{
    return [key = std::move(key)](const ConfigValues &values,
                                  const ConfigValidationContext &context) -> std::optional<bool> {
        const auto value = rule_number(key, values, context);
        return value ? std::optional<bool>{*value > 0.0} : std::nullopt;
    };
}

VisibilityCondition greater_than(std::string key, double threshold)
{
    return [key = std::move(key), threshold](
               const ConfigValues &values,
               const ConfigValidationContext &context) -> std::optional<bool> {
        const auto value = rule_number(key, values, context);
        return value ? std::optional<bool>{*value > threshold} : std::nullopt;
    };
}

VisibilityCondition boolean_is(std::string key, bool expected = true)
{
    return [key = std::move(key), expected](const ConfigValues &values,
                                            const ConfigValidationContext &context) -> std::optional<bool> {
        const auto value = rule_boolean(key, values, context);
        return value ? std::optional<bool>{*value == expected} : std::nullopt;
    };
}

VisibilityCondition enum_is(std::string key, std::set<std::string> accepted)
{
    return [key = std::move(key), accepted = std::move(accepted)](
               const ConfigValues &values,
               const ConfigValidationContext &context) -> std::optional<bool> {
        const auto value = rule_enum(key, values, context);
        return value ? std::optional<bool>{accepted.count(*value) != 0} : std::nullopt;
    };
}

VisibilityCondition enum_is_not(std::string key, std::set<std::string> rejected)
{
    return [key = std::move(key), rejected = std::move(rejected)](
               const ConfigValues &values,
               const ConfigValidationContext &context) -> std::optional<bool> {
        const auto value = rule_enum(key, values, context);
        return value ? std::optional<bool>{rejected.count(*value) == 0} : std::nullopt;
    };
}

VisibilityCondition string_is_empty(std::string key)
{
    return [key = std::move(key)](const ConfigValues &values,
                                  const ConfigValidationContext &context) -> std::optional<bool> {
        const auto value = rule_string(key, values, context);
        return value ? std::optional<bool>{value->empty()} : std::nullopt;
    };
}

VisibilityCondition all_of(std::vector<VisibilityCondition> conditions)
{
    return [conditions = std::move(conditions)](
               const ConfigValues &values,
               const ConfigValidationContext &context) -> std::optional<bool> {
        for (const VisibilityCondition &condition : conditions) {
            const auto result = condition(values, context);
            if (!result) return std::nullopt;
            if (!*result) return false;
        }
        return true;
    };
}

VisibilityCondition any_of(std::vector<VisibilityCondition> conditions)
{
    return [conditions = std::move(conditions)](
               const ConfigValues &values,
               const ConfigValidationContext &context) -> std::optional<bool> {
        bool saw_value = false;
        for (const VisibilityCondition &condition : conditions) {
            const auto result = condition(values, context);
            if (!result) continue;
            saw_value = true;
            if (*result) return true;
        }
        return saw_value ? std::optional<bool>{false} : std::nullopt;
    };
}

VisibilityCondition negate(VisibilityCondition condition)
{
    return [condition = std::move(condition)](
               const ConfigValues &values,
               const ConfigValidationContext &context) -> std::optional<bool> {
        const auto result = condition(values, context);
        return result ? std::optional<bool>{!*result} : std::nullopt;
    };
}

OptionRuleRegistry build_fff_rule_registry(const std::vector<OptionDescriptor> &descriptors)
{
    OptionRuleRegistry registry;
    for (const OptionDescriptor &descriptor : descriptors) {
        registry.emplace(descriptor.id.value(), OptionRule{
            fff_rule_registry_version,
            descriptor.editable ? OptionRuleClass::unclassified
                                : OptionRuleClass::always_visible,
            {}, {}, {}});
    }

    const auto merge_dependencies = [](std::vector<std::string> &destination,
                                       const std::vector<std::string> &source) {
        for (const std::string &dependency : source) {
            if (std::find(destination.begin(), destination.end(), dependency) == destination.end())
                destination.push_back(dependency);
        }
    };
    const auto set_visibility = [&](std::initializer_list<const char *> targets,
                                    const std::vector<std::string> &dependencies,
                                    const VisibilityCondition &condition) {
        for (const char *target : targets) {
            auto found = registry.find(target);
            if (found == registry.end()) continue;
            found->second.version = fff_rule_registry_version;
            found->second.classification = OptionRuleClass::real_predicate;
            merge_dependencies(found->second.dependencies, dependencies);
            found->second.visibility = condition;
        }
    };
    const auto set_compatibility = [&](std::initializer_list<const char *> targets,
                                       const std::vector<std::string> &dependencies,
                                       const VisibilityCondition &condition) {
        for (const char *target : targets) {
            auto found = registry.find(target);
            if (found == registry.end()) continue;
            found->second.version = fff_rule_registry_version;
            found->second.classification = OptionRuleClass::real_predicate;
            merge_dependencies(found->second.dependencies, dependencies);
            found->second.compatibility = condition;
        }
    };

    // Historical ConfigManipulation::toggle_print_fff_options() predicates.
    set_compatibility({"enable_arc_fitting"}, {"max_volumetric_extrusion_rate_slope"},
                      negate(positive("max_volumetric_extrusion_rate_slope")));
    set_visibility({"max_volumetric_extrusion_rate_slope_segment_length",
                    "extrusion_rate_smoothing_external_perimeter_only"},
                   {"max_volumetric_extrusion_rate_slope"},
                   positive("max_volumetric_extrusion_rate_slope"));

    set_compatibility({"extra_perimeters_on_overhangs", "ensure_vertical_shell_thickness",
                    "detect_thin_wall", "detect_overhang_wall", "seam_position",
                    "staggered_inner_seams", "wall_sequence", "outer_wall_line_width",
                    "inner_wall_speed", "outer_wall_speed", "small_perimeter_speed",
                    "small_perimeter_threshold", "gap_infill_speed"},
                   {"wall_loops"}, positive("wall_loops"));

    set_visibility({"sparse_infill_pattern", "infill_combination", "fill_multiline",
                    "infill_direction", "minimum_sparse_infill_area",
                    "sparse_infill_filament_id", "infill_anchor", "infill_anchor_max",
                    "infill_shift_step", "sparse_infill_rotate_template",
                    "symmetric_infill_y_axis"},
                   {"sparse_infill_density"}, positive("sparse_infill_density"));
    set_visibility({"infill_combination_max_layer_height"},
                   {"sparse_infill_density", "infill_combination"},
                   all_of({positive("sparse_infill_density"), boolean_is("infill_combination")}));
    set_visibility({"gyroid_optimized"},
                   {"sparse_infill_density", "sparse_infill_pattern"},
                   all_of({positive("sparse_infill_density"),
                           enum_is("sparse_infill_pattern", {"gyroid"})}));
    set_visibility({"skeleton_infill_density", "skin_infill_density", "infill_lock_depth",
                    "skin_infill_depth", "skin_infill_line_width",
                    "skeleton_infill_line_width"},
                   {"sparse_infill_pattern"},
                   enum_is("sparse_infill_pattern", {"lockedzag"}));
    const VisibilityCondition non_adaptive_infill = enum_is_not(
        "sparse_infill_pattern", {"adaptivecubic", "supportcubic"});
    set_visibility({"infill_shift_step"}, {"sparse_infill_pattern"},
                   enum_is("sparse_infill_pattern", {"crosszag", "lockedzag"}));
    set_visibility({"symmetric_infill_y_axis"}, {"sparse_infill_pattern"},
                   enum_is("sparse_infill_pattern", {"zigzag", "crosszag", "lockedzag"}));
    set_compatibility({"fill_multiline"},
                      {"sparse_infill_density", "sparse_infill_pattern"},
                      all_of({positive("sparse_infill_density"),
                              enum_is("sparse_infill_pattern",
                                      {"gyroid", "grid", "rectilinear", "tpmsd", "tpmsfk",
                                       "crosshatch", "honeycomb", "lateral-lattice",
                                       "lateral-honeycomb", "concentric", "cubic", "stars",
                                       "alignedrectilinear", "lightning", "3dhoneycomb",
                                       "adaptivecubic", "supportcubic", "triangles",
                                       "quartercubic", "archimedeanchords", "hilbertcurve",
                                       "octagramspiral"})}));
    set_compatibility({"infill_anchor_max"},
                      {"sparse_infill_density", "sparse_infill_pattern"},
                      all_of({positive("sparse_infill_density"),
                              enum_is_not("sparse_infill_pattern", {"line"})}));
    set_compatibility({"infill_anchor"},
                      {"sparse_infill_density", "sparse_infill_pattern", "infill_anchor_max"},
                      all_of({positive("sparse_infill_density"),
                              enum_is_not("sparse_infill_pattern", {"line"}),
                              positive("infill_anchor_max")}));
    set_compatibility({"sparse_infill_rotate_template"}, {"sparse_infill_pattern"},
                      non_adaptive_infill);
    set_compatibility({"infill_direction"},
                      {"sparse_infill_density", "sparse_infill_pattern",
                       "sparse_infill_rotate_template"},
                      all_of({positive("sparse_infill_density"), non_adaptive_infill,
                              string_is_empty("sparse_infill_rotate_template")}));
    set_compatibility({"solid_infill_direction"}, {"solid_infill_rotate_template"},
                      string_is_empty("solid_infill_rotate_template"));

    set_visibility({"spiral_mode_smooth", "spiral_starting_flow_ratio",
                    "spiral_finishing_flow_ratio"},
                   {"spiral_mode"}, boolean_is("spiral_mode"));
    set_visibility({"spiral_mode_max_xy_smoothing"},
                   {"spiral_mode", "spiral_mode_smooth"},
                   all_of({boolean_is("spiral_mode"), boolean_is("spiral_mode_smooth")}));

    const VisibilityCondition top_shell_enabled = any_of({
        positive("top_shell_layers"),
        all_of({boolean_is("spiral_mode"), greater_than("bottom_shell_layers", 1.0)})});
    set_compatibility({"top_surface_pattern", "top_surface_density", "top_surface_line_width",
                       "top_surface_speed"},
                      {"top_shell_layers", "spiral_mode", "bottom_shell_layers"},
                      top_shell_enabled);
    set_compatibility({"bottom_surface_pattern", "bottom_surface_density"},
                   {"bottom_shell_layers"}, positive("bottom_shell_layers"));
    set_compatibility({"top_shell_thickness"},
                      {"spiral_mode", "top_shell_layers", "bottom_shell_layers"},
                      all_of({boolean_is("spiral_mode", false), top_shell_enabled}));
    set_compatibility({"bottom_shell_thickness"}, {"spiral_mode", "bottom_shell_layers"},
                   all_of({boolean_is("spiral_mode", false), positive("bottom_shell_layers")}));

    set_compatibility({"outer_wall_acceleration", "inner_wall_acceleration",
                    "initial_layer_acceleration", "initial_layer_travel_acceleration",
                    "top_surface_acceleration", "travel_acceleration", "bridge_acceleration",
                    "sparse_infill_acceleration", "internal_solid_infill_acceleration"},
                   {"default_acceleration"}, positive("default_acceleration"));
    const VisibilityCondition junction_deviation_enabled = all_of({
        enum_is("gcode_flavor", {"marlin2"}),
        positive("machine_max_junction_deviation")});
    set_visibility({"default_junction_deviation"}, {"gcode_flavor"},
                   enum_is("gcode_flavor", {"marlin2"}));
    set_compatibility({"default_junction_deviation"},
                      {"gcode_flavor", "machine_max_junction_deviation"},
                      junction_deviation_enabled);
    set_compatibility({"default_jerk"},
                      {"gcode_flavor", "machine_max_junction_deviation"},
                      negate(junction_deviation_enabled));
    set_visibility({"outer_wall_jerk", "inner_wall_jerk", "initial_layer_jerk",
                    "initial_layer_travel_jerk", "top_surface_jerk", "travel_jerk",
                    "infill_jerk"},
                   {"gcode_flavor", "machine_max_junction_deviation"},
                   negate(junction_deviation_enabled));
    set_compatibility({"outer_wall_jerk", "inner_wall_jerk", "initial_layer_jerk",
                       "initial_layer_travel_jerk", "top_surface_jerk", "travel_jerk",
                       "infill_jerk"},
                      {"gcode_flavor", "machine_max_junction_deviation", "default_jerk"},
                      all_of({negate(junction_deviation_enabled), positive("default_jerk")}));

    set_compatibility({"skirt_type", "min_skirt_length", "skirt_distance", "skirt_start_angle",
                       "skirt_speed", "draft_shield"},
                   {"skirt_loops"}, positive("skirt_loops"));
    set_compatibility({"skirt_height"}, {"skirt_loops", "draft_shield"},
                      all_of({positive("skirt_loops"),
                              enum_is_not("draft_shield", {"enabled"})}));
    set_visibility({"single_loop_draft_shield"}, {"skirt_loops"}, positive("skirt_loops"));
    set_compatibility({"brim_object_gap", "brim_use_efc_outline", "combine_brims",
                       "brim_flow_ratio"},
                   {"brim_type"}, enum_is_not("brim_type", {"no_brim"}));
    set_compatibility({"brim_width"}, {"brim_type"},
                   enum_is_not("brim_type", {"no_brim", "auto_brim", "painted"}));
    set_visibility({"brim_ears_max_angle", "brim_ears_detection_length"},
                   {"brim_type"}, enum_is("brim_type", {"brim_ears"}));
    set_compatibility({"brim_ears_max_angle", "brim_ears_detection_length"},
                      {"brim_width"}, positive("brim_width"));
    const VisibilityCondition brim_enabled = enum_is_not("brim_type", {"no_brim"});
    set_compatibility({"outer_wall_filament_id", "inner_wall_filament_id"},
                      {"wall_loops", "brim_type"},
                      any_of({positive("wall_loops"), brim_enabled}));
    set_compatibility({"inner_wall_line_width"},
                      {"wall_loops", "skirt_loops", "brim_type"},
                      any_of({positive("wall_loops"), positive("skirt_loops"), brim_enabled}));

    const VisibilityCondition support_enabled = any_of({boolean_is("enable_support"),
                                                         positive("raft_layers")});
    set_compatibility({"support_style", "support_base_pattern", "support_base_pattern_spacing",
                    "support_expansion", "support_angle", "support_interface_pattern",
                    "support_interface_top_layers", "support_interface_bottom_layers",
                    "support_top_z_distance", "support_bottom_z_distance", "support_type",
                    "support_on_build_plate_only", "support_object_xy_distance",
                    "support_object_first_layer_gap", "raft_first_layer_density"},
                   {"enable_support", "raft_layers"}, support_enabled);
    const VisibilityCondition support_interface = all_of({
        support_enabled,
        any_of({positive("support_interface_top_layers"),
                positive("support_interface_bottom_layers")})});
    set_compatibility({"support_interface_filament", "support_interface_loop_pattern",
                    "support_bottom_interface_spacing", "support_interface_spacing"},
                   {"enable_support", "raft_layers", "support_interface_top_layers",
                    "support_interface_bottom_layers"}, support_interface);
    set_compatibility({"support_filament"}, {"enable_support", "raft_layers", "skirt_loops"},
                      any_of({support_enabled, positive("skirt_loops")}));

    const VisibilityCondition tree_support = all_of({
        boolean_is("enable_support"),
        enum_is("support_type", {"tree(auto)", "tree(manual)"})});
    const VisibilityCondition automatic_support = enum_is(
        "support_type", {"normal(auto)", "tree(auto)"});
    const VisibilityCondition organic_tree = all_of({
        tree_support, enum_is("support_style", {"default", "organic"})});
    const VisibilityCondition normal_tree = all_of({
        tree_support, enum_is_not("support_style", {"default", "organic"})});
    set_visibility({"tree_support_branch_angle", "tree_support_branch_distance",
                    "tree_support_branch_diameter", "tree_support_auto_brim",
                    "tree_support_brim_width"},
                   {"enable_support", "support_type", "support_style"}, normal_tree);
    set_visibility({"tree_support_branch_angle_organic", "tree_support_branch_distance_organic",
                    "tree_support_branch_diameter_organic", "tree_support_angle_slow",
                    "tree_support_tip_diameter", "tree_support_top_rate",
                    "tree_support_branch_diameter_angle"},
                   {"enable_support", "support_type", "support_style"}, organic_tree);
    set_compatibility({"support_threshold_angle"},
                      {"enable_support", "raft_layers", "support_type"},
                      all_of({support_enabled, automatic_support}));
    set_visibility({"support_threshold_overlap"},
                   {"enable_support", "support_type"}, negate(tree_support));
    set_compatibility({"support_threshold_overlap"},
                      {"enable_support", "raft_layers", "support_type",
                       "support_threshold_angle"},
                      all_of({support_enabled, automatic_support,
                              negate(positive("support_threshold_angle"))}));
    set_visibility({"independent_support_layer_height"},
                   {"enable_support", "raft_layers", "support_type", "support_style"},
                   all_of({support_enabled, negate(organic_tree)}));
    set_compatibility({"tree_support_brim_width"},
                      {"enable_support", "support_type", "tree_support_auto_brim"},
                      all_of({tree_support, boolean_is("tree_support_auto_brim", false)}));
    set_visibility({"max_bridge_length"}, {"enable_support", "support_type"}, tree_support);
    set_visibility({"bridge_no_support"}, {"enable_support", "support_type"},
                   negate(tree_support));
    set_visibility({"support_critical_regions_only"},
                   {"enable_support", "support_type"},
                   all_of({tree_support, automatic_support}));

    const VisibilityCondition can_support_ironing = any_of({
        positive("raft_layers"),
        all_of({boolean_is("enable_support"), positive("support_interface_top_layers")})});
    set_compatibility({"support_ironing"},
                      {"raft_layers", "enable_support", "support_interface_top_layers"},
                      can_support_ironing);
    const VisibilityCondition support_ironing_enabled = all_of({
        can_support_ironing, boolean_is("support_ironing")});
    set_visibility({"support_ironing_pattern", "support_ironing_flow",
                    "support_ironing_spacing"},
                   {"raft_layers", "enable_support", "support_interface_top_layers",
                    "support_ironing"}, support_ironing_enabled);
    set_compatibility({"support_interface_spacing"},
                      {"enable_support", "raft_layers", "support_interface_top_layers",
                       "support_interface_bottom_layers", "support_ironing"},
                      all_of({support_interface, negate(support_ironing_enabled)}));
    set_visibility({"raft_contact_distance"}, {"raft_layers", "support_top_z_distance"},
                   all_of({positive("raft_layers"), positive("support_top_z_distance")}));

    set_visibility({"ironing_pattern", "ironing_flow", "ironing_spacing", "ironing_angle",
                    "ironing_inset", "ironing_angle_fixed", "ironing_speed"},
                   {"ironing_type"}, enum_is_not("ironing_type", {"no ironing"}));
    set_visibility({"ironing_speed"},
                   {"ironing_type", "raft_layers", "enable_support",
                    "support_interface_top_layers", "support_ironing"},
                   any_of({enum_is_not("ironing_type", {"no ironing"}),
                           support_ironing_enabled}));
    set_compatibility({"ironing_angle", "ironing_angle_fixed"},
                      {"ironing_type", "ironing_pattern"},
                      all_of({enum_is_not("ironing_type", {"no ironing"}),
                              enum_is("ironing_pattern", {"rectilinear"})}));
    set_visibility({"zaa_minimize_perimeter_height", "zaa_min_z",
                    "zaa_dont_alternate_fill_direction", "ironing_expansion"},
                   {"zaa_enabled"}, boolean_is("zaa_enabled"));
    set_compatibility({"print_order"}, {"print_sequence"},
                      enum_is_not("print_sequence", {"by object"}));

    set_visibility({"standby_temperature_delta", "preheat_time", "preheat_steps"},
                   {"ooze_prevention"}, boolean_is("ooze_prevention"));
    set_visibility({"prime_tower_width", "prime_tower_brim_width", "prime_tower_skip_points",
                    "wipe_tower_wall_type", "prime_tower_infill_gap",
                    "prime_tower_enable_framework", "enable_tower_interface_features"},
                   {"enable_prime_tower"}, boolean_is("enable_prime_tower"));
    set_compatibility({"flush_into_infill", "flush_into_support", "flush_into_objects"},
                      {"enable_prime_tower"}, boolean_is("enable_prime_tower"));
    set_visibility({"flush_into_objects"}, {},
                   [](const ConfigValues &, const ConfigValidationContext &context) {
                       return std::optional<bool>{context.scope == OptionScope::plate ||
                                                  context.scope == OptionScope::object ||
                                                  context.scope == OptionScope::part ||
                                                  context.scope == OptionScope::layer_range};
                   });
    set_visibility({"enable_tower_interface_cooldown_during_tower"},
                   {"enable_prime_tower", "enable_tower_interface_features"},
                   all_of({boolean_is("enable_prime_tower"),
                           boolean_is("enable_tower_interface_features")}));

    set_visibility({"max_travel_detour_distance"}, {"reduce_crossing_wall"},
                   boolean_is("reduce_crossing_wall"));
    set_visibility({"first_layer_flow_ratio", "outer_wall_flow_ratio", "inner_wall_flow_ratio",
                    "overhang_flow_ratio", "sparse_infill_flow_ratio",
                    "internal_solid_infill_flow_ratio", "gap_fill_flow_ratio",
                    "support_flow_ratio", "support_interface_flow_ratio"},
                   {"set_other_flow_ratios"}, boolean_is("set_other_flow_ratios"));
    set_visibility({"overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed",
                    "overhang_4_4_speed", "slowdown_for_curled_perimeters"},
                   {"enable_overhang_speed"}, boolean_is("enable_overhang_speed"));

    const VisibilityCondition fuzzy_enabled =
        enum_is_not("fuzzy_skin", {"disabled_fuzzy"});
    set_visibility({"fuzzy_skin_mode", "fuzzy_skin_noise_type", "fuzzy_skin_point_distance",
                    "fuzzy_skin_thickness", "fuzzy_skin_first_layer"},
                   {"fuzzy_skin"}, fuzzy_enabled);
    set_visibility({"fuzzy_skin_scale"}, {"fuzzy_skin", "fuzzy_skin_noise_type"},
                   all_of({fuzzy_enabled,
                           enum_is_not("fuzzy_skin_noise_type", {"classic", "ripple"})}));
    set_visibility({"fuzzy_skin_octaves"}, {"fuzzy_skin", "fuzzy_skin_noise_type"},
                   all_of({fuzzy_enabled,
                           enum_is_not("fuzzy_skin_noise_type",
                                       {"classic", "voronoi", "ripple"})}));
    set_visibility({"fuzzy_skin_persistence"}, {"fuzzy_skin", "fuzzy_skin_noise_type"},
                   all_of({fuzzy_enabled,
                           enum_is("fuzzy_skin_noise_type", {"perlin", "billow"})}));
    set_visibility({"fuzzy_skin_ripples_per_layer", "fuzzy_skin_ripple_offset",
                    "fuzzy_skin_layers_between_ripple_offset"},
                   {"fuzzy_skin", "fuzzy_skin_noise_type"},
                   all_of({fuzzy_enabled, enum_is("fuzzy_skin_noise_type", {"ripple"})}));

    set_visibility({"wall_transition_length", "wall_transition_filter_deviation",
                    "wall_transition_angle", "min_feature_size", "min_length_factor",
                    "min_bead_width", "wall_distribution_count", "initial_layer_min_bead_width",
                    "wall_maximum_resolution", "wall_maximum_deviation"},
                   {"wall_generator"}, enum_is("wall_generator", {"arachne"}));
    set_compatibility({"detect_thin_wall"}, {"wall_generator"},
                      enum_is_not("wall_generator", {"arachne"}));
    set_compatibility({"wipe_speed"}, {"role_based_wipe_speed"},
                      boolean_is("role_based_wipe_speed", false));
    set_visibility({"accel_to_decel_enable", "accel_to_decel_factor"}, {"gcode_flavor"},
                   enum_is("gcode_flavor", {"klipper"}));
    set_compatibility({"accel_to_decel_factor"}, {"accel_to_decel_enable"},
                      boolean_is("accel_to_decel_enable"));
    set_visibility({"make_overhang_printable_angle", "make_overhang_printable_hole_size"},
                   {"make_overhang_printable"}, boolean_is("make_overhang_printable"));
    set_visibility({"min_width_top_surface"},
                   {"only_one_wall_top", "min_length_factor", "wall_generator"},
                   any_of({boolean_is("only_one_wall_top"),
                           all_of({greater_than("min_length_factor", 0.5),
                                   enum_is("wall_generator", {"arachne"})})}));
    set_visibility({"hole_to_polyhole_threshold", "hole_to_polyhole_twisted"},
                   {"hole_to_polyhole"}, boolean_is("hole_to_polyhole"));
    set_visibility({"small_area_infill_flow_compensation_model"},
                   {"small_area_infill_flow_compensation"},
                   boolean_is("small_area_infill_flow_compensation"));

    const VisibilityCondition scarf_enabled = all_of({
        boolean_is("spiral_mode", false), enum_is_not("seam_slope_type", {"none"})});
    set_compatibility({"seam_slope_type"}, {"spiral_mode"},
                      boolean_is("spiral_mode", false));
    set_visibility({"seam_slope_conditional", "seam_slope_start_height",
                    "seam_slope_entire_loop", "seam_slope_min_length", "seam_slope_steps",
                    "seam_slope_inner_walls", "scarf_joint_speed", "scarf_joint_flow_ratio"},
                   {"spiral_mode", "seam_slope_type"}, scarf_enabled);
    set_compatibility({"seam_slope_min_length"}, {"seam_slope_entire_loop"},
                      boolean_is("seam_slope_entire_loop", false));
    set_visibility({"scarf_angle_threshold", "scarf_overhang_threshold"},
                   {"spiral_mode", "seam_slope_type", "seam_slope_conditional"},
                   all_of({scarf_enabled, boolean_is("seam_slope_conditional")}));

    set_visibility({"mmu_segmented_region_interlocking_depth"}, {"interlocking_beam"},
                   boolean_is("interlocking_beam", false));
    set_visibility({"interlocking_beam_width", "interlocking_orientation",
                    "interlocking_beam_layer_count", "interlocking_depth",
                    "interlocking_boundary_avoidance"},
                   {"interlocking_beam"}, boolean_is("interlocking_beam"));
    set_visibility({"lateral_lattice_angle_1", "lateral_lattice_angle_2"},
                   {"sparse_infill_pattern"},
                   enum_is("sparse_infill_pattern", {"lateral-lattice"}));
    set_visibility({"lightning_overhang_angle", "lightning_prune_angle",
                    "lightning_straightening_angle"},
                   {"sparse_infill_pattern"}, enum_is("sparse_infill_pattern", {"lightning"}));
    set_visibility({"infill_overhang_angle"}, {"sparse_infill_pattern"},
                   enum_is("sparse_infill_pattern", {"lateral-honeycomb"}));
    const VisibilityCondition overhang_reverse_enabled = all_of({
        boolean_is("spiral_mode", false), boolean_is("overhang_reverse")});
    set_visibility({"overhang_reverse"}, {"spiral_mode"},
                   boolean_is("spiral_mode", false));
    set_visibility({"overhang_reverse_internal_only"},
                   {"spiral_mode", "overhang_reverse"}, overhang_reverse_enabled);
    set_visibility({"overhang_reverse_threshold"},
                   {"detect_overhang_wall", "spiral_mode", "overhang_reverse",
                    "overhang_reverse_internal_only"},
                   all_of({boolean_is("detect_overhang_wall"), overhang_reverse_enabled,
                           boolean_is("overhang_reverse_internal_only", false)}));

    return registry;
}

OptionEvaluator make_fff_rule_evaluator(std::uint32_t schema_version,
                                        const std::vector<OptionDescriptor> &descriptors)
{
    auto registry = std::make_shared<const OptionRuleRegistry>(
        build_fff_rule_registry(descriptors));
    return [schema_version, registry](const OptionId &option, const ConfigValues &candidate,
                                      const ConfigValidationContext &context)
               -> Result<OptionEvaluation> {
        if (schema_version != fff_rule_registry_version)
            return ResultAccess::failure<OptionEvaluation>(
                ErrorCode::internal, "Configuration rule registry version does not match schema",
                "/schema/rules/version");
        const auto found = registry->find(option.value());
        if (found == registry->end())
            return ResultAccess::failure<OptionEvaluation>(
                ErrorCode::internal, "Configuration option has no registered evaluation rule",
                "/schema/rules/" + option.value());
        const OptionRule &rule = found->second;
        if (rule.version != schema_version)
            return ResultAccess::failure<OptionEvaluation>(
                ErrorCode::internal, "Configuration option rule has the wrong version",
                "/schema/rules/" + option.value());
        if (rule.classification == OptionRuleClass::unclassified)
            return ResultAccess::failure<OptionEvaluation>(
                ErrorCode::internal,
                "Configuration option has no explicit evaluation rule or classification",
                "/schema/rules/" + option.value());
        if (rule.classification != OptionRuleClass::real_predicate)
            return ResultAccess::success(OptionEvaluation{true, true, {}});
        if (!rule.visibility && !rule.compatibility)
            return ResultAccess::failure<OptionEvaluation>(
                ErrorCode::internal, "Configuration predicate is missing",
                "/schema/rules/" + option.value());
        const auto visible = rule.visibility
            ? rule.visibility(candidate, context) : std::optional<bool>{true};
        const auto compatible = rule.compatibility
            ? rule.compatibility(candidate, context) : std::optional<bool>{true};
        if (!visible || !compatible) {
            const std::string dependency = rule.dependencies.empty()
                ? option.value() : rule.dependencies.front();
            return ResultAccess::failure<OptionEvaluation>(
                ErrorCode::invalid_configuration,
                "Configuration evaluation requires a complete typed value context",
                "/context/configuration/" + dependency);
        }
        std::vector<Diagnostic> diagnostics;
        if (!*compatible)
            diagnostics.push_back({ErrorCode::invalid_configuration, Severity::error,
                                   "Configuration option is disabled by its dependencies",
                                   "/configuration/" + option.value()});
        return ResultAccess::success(OptionEvaluation{*visible, *compatible,
                                                       std::move(diagnostics)});
    };
}

} // namespace

Result<ConfigValue> core_option_to_value(const Slic3r::ConfigOptionDef &definition,
                                         const Slic3r::ConfigOption &option)
{
    if (option.type() != definition.type)
        return failure<ConfigValue>(ErrorCode::internal,
                                    "Core configuration definition and value types disagree",
                                    definition.opt_key);

    try {
        if (const auto *reference = filament_reference_rule(definition.opt_key)) {
            if (definition.type != Slic3r::coInt)
                return failure<ConfigValue>(ErrorCode::internal,
                                            "Filament reference has an invalid core type",
                                            definition.opt_key);
            const int raw = option.getInt();
            if (reference->core_encoding == CoreReferenceEncoding::one_based_zero_sentinel) {
                if (raw == 0)
                    return ResultAccess::success(ConfigValue::null(
                        ConfigValueShape::scalar(ConfigValueType::integer)));
                if (raw < 0)
                    return failure<ConfigValue>(ErrorCode::invalid_configuration,
                                                "Filament reference is invalid", definition.opt_key);
                return ResultAccess::success(ConfigValue::integer(
                    static_cast<std::int64_t>(raw) - 1));
            }
            if (raw < 0)
                return failure<ConfigValue>(ErrorCode::invalid_configuration,
                                            "Filament reference is invalid", definition.opt_key);
            return ResultAccess::success(ConfigValue::integer(raw));
        }
        switch (definition.type) {
        case Slic3r::coFloat:
            return ResultAccess::success(ConfigValue::decimal(option.getFloat()));
        case Slic3r::coFloats:
            return vector_to_value<Slic3r::ConfigOptionVector<double>>(
                option, ConfigValueShape::scalar(ConfigValueType::decimal),
                [](double item) { return ConfigValue::decimal(item); }, definition.opt_key);
        case Slic3r::coInt:
            return ResultAccess::success(ConfigValue::integer(option.getInt()));
        case Slic3r::coInts:
            return vector_to_value<Slic3r::ConfigOptionVector<int>>(
                option, ConfigValueShape::scalar(ConfigValueType::integer),
                [](int item) { return ConfigValue::integer(item); }, definition.opt_key);
        case Slic3r::coString: {
            const auto *typed = checked_option<Slic3r::ConfigOptionString>(option);
            if (!typed) break;
            return ResultAccess::success(ConfigValue::string(typed->value));
        }
        case Slic3r::coStrings:
            return vector_to_value<Slic3r::ConfigOptionStrings>(
                option, ConfigValueShape::scalar(ConfigValueType::string),
                [](const std::string &item) { return ConfigValue::string(item); }, definition.opt_key);
        case Slic3r::coPercent:
            return ResultAccess::success(ConfigValue::percent(option.getFloat()));
        case Slic3r::coPercents:
            return vector_to_value<Slic3r::ConfigOptionVector<double>>(
                option, ConfigValueShape::scalar(ConfigValueType::percent),
                [](double item) { return ConfigValue::percent(item); }, definition.opt_key);
        case Slic3r::coFloatOrPercent: {
            const auto *typed = checked_option<Slic3r::ConfigOptionFloatOrPercent>(option);
            if (!typed) break;
            return ResultAccess::success(ConfigValue::float_or_percent(
                FloatOrPercent{typed->value, typed->percent}));
        }
        case Slic3r::coFloatsOrPercents:
            return vector_to_value<Slic3r::ConfigOptionVector<Slic3r::FloatOrPercent>>(
                option, ConfigValueShape::scalar(ConfigValueType::float_or_percent),
                [](const Slic3r::FloatOrPercent &item) {
                    return ConfigValue::float_or_percent(FloatOrPercent{item.value, item.percent});
                }, definition.opt_key);
        case Slic3r::coPoint: {
            const auto *typed = checked_option<Slic3r::ConfigOptionPoint>(option);
            if (!typed) break;
            return ResultAccess::success(ConfigValue::point2({typed->value.x(), typed->value.y()}));
        }
        case Slic3r::coPoints:
            return vector_to_value<Slic3r::ConfigOptionPoints>(
                option, ConfigValueShape::scalar(ConfigValueType::point2),
                [](const Slic3r::Vec2d &item) { return ConfigValue::point2({item.x(), item.y()}); },
                definition.opt_key);
        case Slic3r::coPoint3: {
            const auto *typed = checked_option<Slic3r::ConfigOptionPoint3>(option);
            if (!typed) break;
            return ResultAccess::success(
                ConfigValue::point3({typed->value.x(), typed->value.y(), typed->value.z()}));
        }
        case Slic3r::coBool:
            return ResultAccess::success(ConfigValue::boolean(option.getBool()));
        case Slic3r::coBools:
            return vector_to_value<Slic3r::ConfigOptionVector<unsigned char>>(
                option, ConfigValueShape::scalar(ConfigValueType::boolean),
                [](unsigned char item) { return ConfigValue::boolean(item != 0); }, definition.opt_key);
        case Slic3r::coEnum:
            return ResultAccess::success(ConfigValue::enumeration(option.serialize()));
        case Slic3r::coEnums:
            return enum_vector_to_value(definition, option);
        case Slic3r::coPointsGroups: {
            const auto *typed = checked_option<Slic3r::ConfigOptionPointsGroups>(option);
            if (!typed) break;
            const auto point_shape = ConfigValueShape::scalar(ConfigValueType::point2);
            const auto group_shape = ConfigValueShape::list(point_shape);
            std::vector<ConfigValue> groups;
            groups.reserve(typed->values.size());
            for (const auto &group : typed->values) {
                std::vector<ConfigValue> points;
                points.reserve(group.size());
                for (const auto &point : group)
                    points.push_back(ConfigValue::point2({point.x(), point.y()}));
                auto converted = ConfigValue::list(point_shape, std::move(points));
                if (!converted.has_value()) break;
                groups.push_back(std::move(converted).value());
            }
            return make_list(group_shape, std::move(groups), definition.opt_key);
        }
        case Slic3r::coIntsGroups: {
            const auto *typed = checked_option<Slic3r::ConfigOptionIntsGroups>(option);
            if (!typed) break;
            const auto integer_shape = ConfigValueShape::scalar(ConfigValueType::integer);
            const auto group_shape = ConfigValueShape::list(integer_shape);
            std::vector<ConfigValue> groups;
            groups.reserve(typed->values.size());
            for (const auto &group : typed->values) {
                std::vector<ConfigValue> integers;
                integers.reserve(group.size());
                for (int item : group) integers.push_back(ConfigValue::integer(item));
                auto converted = ConfigValue::list(integer_shape, std::move(integers));
                if (!converted.has_value()) break;
                groups.push_back(std::move(converted).value());
            }
            return make_list(group_shape, std::move(groups), definition.opt_key);
        }
        default:
            return failure<ConfigValue>(ErrorCode::unsupported,
                                        "Unsupported core configuration option type", definition.opt_key);
        }
    } catch (const std::exception &error) {
        return failure<ConfigValue>(ErrorCode::invalid_configuration, error.what(), definition.opt_key);
    }
    return failure<ConfigValue>(ErrorCode::internal,
                                "Core configuration option has an unexpected runtime type",
                                definition.opt_key);
}

Result<Slic3r::ConfigOptionUniquePtr> value_to_core_option(
    const Slic3r::ConfigOptionDef &definition, const ConfigValue &value)
{
    if (const auto *reference = filament_reference_rule(definition.opt_key)) {
        if (definition.type != Slic3r::coInt)
            return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::internal,
                                                          "Filament reference has an invalid core type",
                                                          definition.opt_key);
        if (value.is_null()) {
            if (!reference->value_nullable ||
                reference->core_encoding != CoreReferenceEncoding::one_based_zero_sentinel)
                return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_configuration,
                                                              "Filament reference is not nullable",
                                                              definition.opt_key);
            return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
                new Slic3r::ConfigOptionInt(0)));
        }
        const auto slot = value.as_integer();
        if (!slot || *slot < 0)
            return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_configuration,
                                                          "Filament reference must be a non-negative logical slot",
                                                          definition.opt_key);
        std::int64_t raw = *slot;
        if (reference->core_encoding == CoreReferenceEncoding::one_based_zero_sentinel)
            ++raw;
        if (raw > std::numeric_limits<int>::max())
            return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_configuration,
                                                          "Filament reference is outside the supported range",
                                                          definition.opt_key);
        return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
            new Slic3r::ConfigOptionInt(static_cast<int>(raw))));
    }
    if (value.is_null())
        return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                      "Core FFF scalar options are not nullable",
                                                      definition.opt_key);
    try {
        switch (definition.type) {
        case Slic3r::coFloat:
            if (const auto v = value.as_decimal())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(new Slic3r::ConfigOptionFloat(*v)));
            break;
        case Slic3r::coFloats:
            if (definition.nullable)
                return build_vector_option(definition, value,
                    [](const ConfigValue &v) { return v.as_decimal(); },
                    new Slic3r::ConfigOptionFloatsNullable());
            return build_nonnullable_vector_option(definition, value,
                [](const ConfigValue &v) { return v.as_decimal(); }, new Slic3r::ConfigOptionFloats());
        case Slic3r::coInt:
            if (const auto v = value.as_integer()) {
                if (*v < std::numeric_limits<int>::min() || *v > std::numeric_limits<int>::max())
                    return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                                  "Integer is outside the core range",
                                                                  definition.opt_key);
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(new Slic3r::ConfigOptionInt(static_cast<int>(*v))));
            }
            break;
        case Slic3r::coInts: {
            const auto getter = [](const ConfigValue &v) -> std::optional<int> {
                const auto item = v.as_integer();
                if (!item || *item < std::numeric_limits<int>::min() ||
                    *item > std::numeric_limits<int>::max()) return std::nullopt;
                return static_cast<int>(*item);
            };
            if (definition.nullable)
                return build_vector_option(definition, value, getter,
                                           new Slic3r::ConfigOptionIntsNullable());
            return build_nonnullable_vector_option(definition, value, getter,
                                                    new Slic3r::ConfigOptionInts());
        }
        case Slic3r::coString:
            if (const auto v = value.as_string())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(new Slic3r::ConfigOptionString(*v)));
            break;
        case Slic3r::coStrings:
            return build_nonnullable_vector_option(definition, value,
                [](const ConfigValue &v) { return v.as_string(); }, new Slic3r::ConfigOptionStrings());
        case Slic3r::coPercent:
            if (const auto v = value.as_percent())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(new Slic3r::ConfigOptionPercent(*v)));
            break;
        case Slic3r::coPercents:
            if (definition.nullable)
                return build_vector_option(definition, value,
                    [](const ConfigValue &v) { return v.as_percent(); },
                    new Slic3r::ConfigOptionPercentsNullable());
            return build_nonnullable_vector_option(definition, value,
                [](const ConfigValue &v) { return v.as_percent(); }, new Slic3r::ConfigOptionPercents());
        case Slic3r::coFloatOrPercent:
            if (const auto v = value.as_float_or_percent())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
                    new Slic3r::ConfigOptionFloatOrPercent(v->value, v->is_percent)));
            break;
        case Slic3r::coFloatsOrPercents: {
            const auto getter = [](const ConfigValue &v) -> std::optional<Slic3r::FloatOrPercent> {
                const auto item = v.as_float_or_percent();
                if (!item) return std::nullopt;
                return Slic3r::FloatOrPercent{item->value, item->is_percent};
            };
            if (definition.nullable)
                return build_vector_option(definition, value, getter,
                    new Slic3r::ConfigOptionFloatsOrPercentsNullable());
            return build_nonnullable_vector_option(definition, value, getter,
                new Slic3r::ConfigOptionFloatsOrPercents());
        }
        case Slic3r::coPoint:
            if (const auto v = value.as_point2())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
                    new Slic3r::ConfigOptionPoint(Slic3r::Vec2d(v->x, v->y))));
            break;
        case Slic3r::coPoints:
            return build_nonnullable_vector_option(definition, value,
                [](const ConfigValue &v) -> std::optional<Slic3r::Vec2d> {
                    const auto item = v.as_point2();
                    return item ? std::optional<Slic3r::Vec2d>{Slic3r::Vec2d(item->x, item->y)} : std::nullopt;
                }, new Slic3r::ConfigOptionPoints());
        case Slic3r::coPoint3:
            if (const auto v = value.as_point3())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
                    new Slic3r::ConfigOptionPoint3(Slic3r::Vec3d(v->x, v->y, v->z))));
            break;
        case Slic3r::coBool:
            if (const auto v = value.as_boolean())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(new Slic3r::ConfigOptionBool(*v)));
            break;
        case Slic3r::coBools: {
            const auto getter = [](const ConfigValue &v) -> std::optional<unsigned char> {
                const auto item = v.as_boolean();
                return item ? std::optional<unsigned char>{static_cast<unsigned char>(*item)} : std::nullopt;
            };
            if (definition.nullable)
                return build_vector_option(definition, value, getter,
                                           new Slic3r::ConfigOptionBoolsNullable());
            return build_nonnullable_vector_option(definition, value, getter,
                                                    new Slic3r::ConfigOptionBools());
        }
        case Slic3r::coEnum:
            if (const auto v = value.as_enumeration()) {
                auto output = Slic3r::ConfigOptionUniquePtr(
                    new Slic3r::ConfigOptionEnumGeneric(definition.enum_keys_map));
                if (!output->deserialize(*v))
                    return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                                  "Unknown enum value", definition.opt_key);
                return ResultAccess::success(std::move(output));
            }
            break;
        case Slic3r::coEnums: {
            const auto items = value.as_list();
            if (!items || !definition.enum_keys_map)
                return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                              "Enum list has the wrong type or no value map",
                                                              definition.opt_key);
            Slic3r::ConfigOptionUniquePtr output(definition.nullable
                ? static_cast<Slic3r::ConfigOption *>(new Slic3r::ConfigOptionEnumsGenericNullable(definition.enum_keys_map))
                : static_cast<Slic3r::ConfigOption *>(new Slic3r::ConfigOptionEnumsGeneric(definition.enum_keys_map)));
            auto *vector = dynamic_cast<Slic3r::ConfigOptionVector<int> *>(output.get());
            for (const ConfigValue &item : *items) {
                if (item.is_null()) {
                    if (!definition.nullable)
                        return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                                      "Enum list item is not nullable",
                                                                      definition.opt_key);
                    vector->values.push_back(Slic3r::ConfigOptionIntsNullable::nil_value());
                    continue;
                }
                const auto name = item.as_enumeration();
                const auto found = name ? definition.enum_keys_map->find(*name) : definition.enum_keys_map->end();
                if (!name || found == definition.enum_keys_map->end())
                    return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                                  "Unknown enum list value", definition.opt_key);
                vector->values.push_back(found->second);
            }
            return ResultAccess::success(std::move(output));
        }
        case Slic3r::coPointsGroups: {
            const auto groups = value.as_list();
            if (!groups) break;
            std::vector<Slic3r::Vec2ds> converted_groups;
            converted_groups.reserve(groups->size());
            for (const ConfigValue &group : *groups) {
                const auto points = group.as_list();
                if (!points) break;
                Slic3r::Vec2ds converted_points;
                converted_points.reserve(points->size());
                for (const ConfigValue &point : *points) {
                    const auto converted = point.as_point2();
                    if (!converted) break;
                    converted_points.emplace_back(converted->x, converted->y);
                }
                if (converted_points.size() != points->size()) break;
                converted_groups.push_back(std::move(converted_points));
            }
            if (converted_groups.size() == groups->size())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
                    new Slic3r::ConfigOptionPointsGroups(converted_groups)));
            break;
        }
        case Slic3r::coIntsGroups: {
            const auto groups = value.as_list();
            if (!groups) break;
            std::vector<std::vector<int>> converted_groups;
            converted_groups.reserve(groups->size());
            for (const ConfigValue &group : *groups) {
                const auto integers = group.as_list();
                if (!integers) break;
                std::vector<int> converted_integers;
                converted_integers.reserve(integers->size());
                for (const ConfigValue &integer : *integers) {
                    const auto converted = integer.as_integer();
                    if (!converted || *converted < std::numeric_limits<int>::min() ||
                        *converted > std::numeric_limits<int>::max()) break;
                    converted_integers.push_back(static_cast<int>(*converted));
                }
                if (converted_integers.size() != integers->size()) break;
                converted_groups.push_back(std::move(converted_integers));
            }
            if (converted_groups.size() == groups->size())
                return ResultAccess::success(Slic3r::ConfigOptionUniquePtr(
                    new Slic3r::ConfigOptionIntsGroups(converted_groups)));
            break;
        }
        default:
            return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::unsupported,
                                                          "Unsupported core configuration option type",
                                                          definition.opt_key);
        }
    } catch (const std::exception &error) {
        return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument, error.what(),
                                                      definition.opt_key);
    }
    return failure<Slic3r::ConfigOptionUniquePtr>(ErrorCode::invalid_argument,
                                                  "Configuration value has the wrong type",
                                                  definition.opt_key);
}

Result<ConfigSchema> build_orca_fff_schema()
{
    try {
        static const std::set<std::string> printer = option_set(Slic3r::Preset::printer_options());
        static const std::set<std::string> process = option_set(Slic3r::Preset::print_options());
        static const std::set<std::string> filament = option_set(Slic3r::Preset::filament_options());
        static const std::set<std::string> object = option_set(Slic3r::PrintObjectConfig().keys());
        static const std::set<std::string> region = option_set(Slic3r::PrintRegionConfig().keys());

        std::vector<OptionDescriptor> descriptors;
        std::vector<std::string> typed_owned_options;
        descriptors.reserve(Slic3r::print_config_def.options.size());
        for (const auto &[key, definition] : Slic3r::print_config_def.options) {
            if (definition.printer_technology == Slic3r::ptSLA)
                continue;

            std::vector<PresetKind> kinds;
            if (printer.count(key)) kinds.push_back(PresetKind::printer);
            if (process.count(key)) kinds.push_back(PresetKind::process);
            if (filament.count(key)) kinds.push_back(PresetKind::filament);

            std::vector<OptionScope> scopes{OptionScope::project, OptionScope::plate};
            if (!kinds.empty()) scopes.push_back(OptionScope::preset);
            // `extruder` is a legacy model override normalized into the
            // feature-specific region fields by DynamicPrintConfig::normalize_fdm().
            // It is nevertheless valid at every model override level in Orca 3MF.
            const bool model_extruder = key == "extruder";
            if (object.count(key) || region.count(key) || model_extruder)
                scopes.push_back(OptionScope::object);
            if (region.count(key) || model_extruder) {
                scopes.push_back(OptionScope::part);
                scopes.push_back(OptionScope::layer_range);
            }

            const InternalOptionOwnership ownership = ownership_for(key);
            if (ownership != InternalOptionOwnership::generic_config)
                typed_owned_options.push_back(key);
            descriptors.push_back({OptionId(key), descriptor_for(definition),
                                   definition.full_label.empty() ? definition.label : definition.full_label,
                                   std::move(scopes), std::move(kinds), filament_reference_for(key),
                                   !definition.readonly &&
                                       ownership == InternalOptionOwnership::generic_config});
        }
        OptionEvaluator evaluator = make_fff_rule_evaluator(fff_rule_registry_version, descriptors);
        return ResultAccess::success(ConfigSchemaAccess::make(
            "orca.fff.config", fff_rule_registry_version, std::move(descriptors),
            std::move(typed_owned_options),
            std::move(evaluator)));
    } catch (const std::exception &error) {
        return ResultAccess::failure<ConfigSchema>(ErrorCode::internal, error.what(), "/schema");
    }
}

Result<ConfigValues> core_config_to_values(const Slic3r::ConfigBase &config)
{
    std::vector<ConfigEntry> entries;
    const auto keys = config.keys();
    entries.reserve(keys.size());
    for (const std::string &key : keys) {
        const auto *definition = Slic3r::print_config_def.get(key);
        const auto *option = config.optptr(key);
        if (!definition || !option)
            return failure<ConfigValues>(ErrorCode::invalid_configuration,
                                         "Core configuration contains an unknown option", key);
        auto converted = core_option_to_value(*definition, *option);
        if (!converted.has_value())
            return ResultAccess::failure<ConfigValues>(*converted.error_code(),
                                                       converted.diagnostics().front().message,
                                                       converted.diagnostics().front().field);
        entries.push_back({OptionId(key), std::move(converted).value()});
    }
    return ResultAccess::success(ConfigValuesAccess::make(std::move(entries)));
}

Result<ConfigPatch> core_config_diff_to_patch(const Slic3r::ConfigBase &inherited,
                                              const Slic3r::ConfigBase &effective,
                                              const ConfigSchema &schema)
{
    ConfigPatch patch;
    for (const std::string &key : effective.keys()) {
        const auto descriptor = schema.find(OptionId(key));
        if (!descriptor || !descriptor->editable ||
            ownership_for(key) != InternalOptionOwnership::generic_config)
            continue;
        const auto *current = effective.optptr(key);
        const auto *base = inherited.optptr(key);
        if (!current || (base && *current == *base))
            continue;
        const auto *definition = Slic3r::print_config_def.get(key);
        if (!definition)
            return failure<ConfigPatch>(ErrorCode::invalid_configuration,
                                        "Core configuration contains an unknown option", key);
        auto converted = core_option_to_value(*definition, *current);
        if (!converted.has_value())
            return ResultAccess::failure<ConfigPatch>(*converted.error_code(),
                                                      converted.diagnostics().front().message,
                                                      converted.diagnostics().front().field);
        patch.set(OptionId(key), std::move(converted).value());
    }
    return ResultAccess::success(std::move(patch));
}

Result<void> apply_patch_to_core_config(const ConfigPatch &patch,
                                        Slic3r::DynamicPrintConfig &config)
{
    Slic3r::DynamicPrintConfig staged(config);
    for (const ConfigEntry &entry : patch.entries()) {
        const std::string key = entry.option.value();
        const auto *definition = Slic3r::print_config_def.get(key);
        if (!definition)
            return failure<void>(ErrorCode::invalid_argument,
                                 "Unknown configuration option", key);
        if (ownership_for(key) != InternalOptionOwnership::generic_config)
            return failure<void>(ErrorCode::invalid_argument,
                                 "Configuration option is owned by a typed API", key);
        auto converted = value_to_core_option(*definition, entry.value);
        if (!converted.has_value())
            return ResultAccess::failure(*converted.error_code(),
                                         converted.diagnostics().front().message,
                                         converted.diagnostics().front().field);
        staged.set_key_value(key, std::move(converted).value().release());
    }
    config = std::move(staged);
    return ResultAccess::success();
}

} // namespace libslicer::v1::detail
