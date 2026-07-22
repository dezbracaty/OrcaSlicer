#include "ConfigSchemaInternal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace libslicer::v1 {

struct ListDescriptor::State {
    ValueDescriptor item;
    std::size_t     min_items;
    std::size_t     max_items;
    bool            fixed_length;
};

struct ValueDescriptor::State {
    ConfigValueShape                         shape;
    bool                                     value_nullable;
    std::optional<NumericConstraint>         numeric;
    std::optional<FloatOrPercentConstraint>  float_or_percent;
    std::vector<std::string>                 enum_values;
    std::string                              canonical_unit;
    std::optional<ListDescriptor>            list;
};

struct ConfigSchema::State {
    std::string                    schema_id;
    std::uint32_t                  schema_version;
    std::vector<OptionDescriptor>  options;
    std::set<std::string>          typed_owned_options;
    detail::OptionEvaluator        evaluator;
};

ListDescriptor::ListDescriptor(std::shared_ptr<const State> state) : state_(std::move(state)) {}
ValueDescriptor::ValueDescriptor(std::shared_ptr<const State> state) : state_(std::move(state)) {}
ConfigSchema::ConfigSchema(std::shared_ptr<const State> state) : state_(std::move(state)) {}

ValueDescriptor ListDescriptor::item() const { return state_->item; }
std::size_t ListDescriptor::min_items() const noexcept { return state_->min_items; }
std::size_t ListDescriptor::max_items() const noexcept { return state_->max_items; }
bool ListDescriptor::fixed_length() const noexcept { return state_->fixed_length; }

ConfigValueShape ValueDescriptor::shape() const { return state_->shape; }
bool ValueDescriptor::value_nullable() const noexcept { return state_->value_nullable; }
std::optional<NumericConstraint> ValueDescriptor::numeric() const { return state_->numeric; }
std::optional<FloatOrPercentConstraint> ValueDescriptor::float_or_percent() const
{
    return state_->float_or_percent;
}
std::vector<std::string> ValueDescriptor::enum_values() const { return state_->enum_values; }
std::string ValueDescriptor::canonical_unit() const { return state_->canonical_unit; }
std::optional<ListDescriptor> ValueDescriptor::list() const { return state_->list; }

std::string ConfigSchema::schema_id() const { return state_->schema_id; }
std::uint32_t ConfigSchema::schema_version() const noexcept { return state_->schema_version; }

std::optional<OptionDescriptor> ConfigSchema::find(const OptionId &option) const
{
    const auto it = std::find_if(state_->options.begin(), state_->options.end(),
                                 [&option](const OptionDescriptor &descriptor) {
                                     return descriptor.id == option;
                                 });
    return it == state_->options.end() ? std::nullopt : std::optional<OptionDescriptor>{*it};
}

std::vector<OptionDescriptor> ConfigSchema::options() const { return state_->options; }

namespace {

Result<void> validate_context(const ConfigValidationContext &context)
{
    if (context.scope == OptionScope::preset && !context.preset_kind) {
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "preset_kind is required for preset scope",
                                             "/context/preset_kind");
    }
    if (context.scope != OptionScope::preset && context.preset_kind) {
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "preset_kind is only valid for preset scope",
                                             "/context/preset_kind");
    }
    if (context.scope == OptionScope::preset && context.preset_kind == PresetKind::process &&
        !context.printer) {
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "printer values are required for process evaluation",
                                             "/context/printer");
    }
    if (context.scope == OptionScope::preset && context.preset_kind == PresetKind::filament) {
        if (!context.printer)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "printer values are required for filament evaluation",
                                                 "/context/printer");
        if (!context.process)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "process values are required for filament evaluation",
                                                 "/context/process");
    }
    return detail::ResultAccess::success();
}

bool contains_scope(const std::vector<OptionScope> &scopes, OptionScope scope)
{
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

bool contains_kind(const std::vector<PresetKind> &kinds, PresetKind kind)
{
    return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

std::string json_pointer_segment(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        if (character == '~') escaped += "~0";
        else if (character == '/') escaped += "~1";
        else escaped.push_back(character);
    }
    return escaped;
}

Result<void> validate_filament_reference(const ConfigValue &value,
                                         const ValueDescriptor &descriptor,
                                         FilamentReferenceKind kind,
                                         const std::optional<std::size_t> &filament_count,
                                         const std::string &field)
{
    const auto invalid = [&](std::string message, const std::string &location = {}) {
        return detail::ResultAccess::failure(
            ErrorCode::invalid_configuration, std::move(message), field + location);
    };
    const auto validate_slot = [&](const ConfigValue &slot, const ValueDescriptor &slot_descriptor,
                                   const std::string &location) -> Result<void> {
        if (slot.is_null())
            return slot_descriptor.value_nullable()
                ? detail::ResultAccess::success()
                : invalid("Logical filament slot is not nullable", location);
        const auto integer = slot.as_integer();
        if (!integer || *integer < 0)
            return invalid("Logical filament slot must be a non-negative integer", location);
        if (filament_count && static_cast<std::uint64_t>(*integer) >= *filament_count)
            return invalid("Logical filament slot is outside the current filament selection", location);
        return detail::ResultAccess::success();
    };

    if (kind == FilamentReferenceKind::logical_slot)
        return validate_slot(value, descriptor, {});
    if (kind == FilamentReferenceKind::logical_slot_list) {
        if (value.is_null())
            return descriptor.value_nullable()
                ? detail::ResultAccess::success()
                : invalid("Logical filament slot list is not nullable");
        const auto list_descriptor = descriptor.list();
        const auto items = value.as_list();
        if (!list_descriptor || !items)
            return invalid("Logical filament slot list has the wrong shape");
        for (std::size_t index = 0; index < items->size(); ++index) {
            auto result = validate_slot((*items)[index], list_descriptor->item(),
                                        "/" + std::to_string(index));
            if (!result.has_value()) return result;
        }
    }
    return detail::ResultAccess::success();
}

Result<void> validate_value(const ConfigValue &value, const ValueDescriptor &descriptor,
                            const std::string &field)
{
    if (value.shape() != descriptor.shape())
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Configuration value has the wrong shape", field);
    if (value.is_null()) {
        if (!descriptor.value_nullable())
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration value is not nullable", field);
        return detail::ResultAccess::success();
    }

    if (const auto list = descriptor.list()) {
        const auto items = value.as_list();
        if (!items)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration value is not a list", field);
        if (items->size() < list->min_items() || items->size() > list->max_items())
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration list length is out of range", field);
        for (std::size_t index = 0; index < items->size(); ++index) {
            auto checked = validate_value((*items)[index], list->item(),
                                          field + "/" + std::to_string(index));
            if (!checked.has_value())
                return checked;
        }
        return detail::ResultAccess::success();
    }

    if (descriptor.shape().type() == ConfigValueType::enumeration) {
        const auto candidate = value.as_enumeration();
        const auto allowed = descriptor.enum_values();
        if (!candidate || std::find(allowed.begin(), allowed.end(), *candidate) == allowed.end())
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration enum value is not allowed", field);
    }

    std::optional<double> number;
    switch (descriptor.shape().type()) {
    case ConfigValueType::integer:
        if (const auto candidate = value.as_integer()) number = static_cast<double>(*candidate);
        break;
    case ConfigValueType::decimal: number = value.as_decimal(); break;
    case ConfigValueType::percent: number = value.as_percent(); break;
    case ConfigValueType::float_or_percent:
        if (const auto candidate = value.as_float_or_percent()) number = candidate->value;
        break;
    default: break;
    }

    if (number && !std::isfinite(*number))
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Configuration number must be finite", field);
    if (number) {
        std::optional<NumericConstraint> constraint = descriptor.numeric();
        if (const auto fop = descriptor.float_or_percent()) constraint = fop->numeric;
        if (constraint && constraint->minimum && *number < *constraint->minimum)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration number is below the minimum", field);
        if (constraint && constraint->maximum && *number > *constraint->maximum)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration number is above the maximum", field);
    }

    return detail::ResultAccess::success();
}

ConfigValues merge_values(const ConfigValues &base, const ConfigPatch &patch)
{
    std::vector<ConfigEntry> merged = base.entries();
    for (const ConfigEntry &entry : patch.entries()) {
        const auto it = std::find_if(merged.begin(), merged.end(), [&entry](const ConfigEntry &candidate) {
            return candidate.option == entry.option;
        });
        if (it == merged.end())
            merged.push_back(entry);
        else
            it->value = entry.value;
    }
    return detail::ConfigValuesAccess::make(std::move(merged));
}

} // namespace

Result<void> detail::ConfigSchemaAccess::validate_structure(
    const ConfigSchema &schema, const ConfigPatch &patch,
    const ConfigValidationContext &context)
{
    auto context_result = validate_context(context);
    if (!context_result.has_value())
        return context_result;

    for (const ConfigEntry &entry : patch.entries()) {
        const auto descriptor = schema.find(entry.option);
        const std::string field = "/configuration/" + json_pointer_segment(entry.option.value());
        if (!descriptor)
            return detail::ResultAccess::failure(ErrorCode::invalid_configuration,
                                                 "Unknown configuration option", field);
        if (!contains_scope(descriptor->allowed_scopes, context.scope))
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration option is not valid in this scope", field);
        if (context.scope == OptionScope::preset &&
            !contains_kind(descriptor->applicable_preset_kinds, *context.preset_kind))
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration option is not valid for this preset kind", field);
        if (!descriptor->editable || !is_generic_option(schema, entry.option))
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Configuration option is owned by a typed API", field);
        if (descriptor->filament_reference != FilamentReferenceKind::none) {
            auto reference_result = validate_filament_reference(
                entry.value, descriptor->value, descriptor->filament_reference,
                context.filament_count, field);
            if (!reference_result.has_value()) return reference_result;
        }
        auto value_result = validate_value(entry.value, descriptor->value, field);
        if (!value_result.has_value())
            return value_result;
    }
    return detail::ResultAccess::success();
}

Result<ConfigPatch> ConfigSchema::validate_and_normalize(
    const ConfigValues &base, ConfigPatch candidate_patch,
    const ConfigValidationContext &context) const
{
    auto structure = detail::ConfigSchemaAccess::validate_structure(
        *this, candidate_patch, context);
    if (!structure.has_value())
        return detail::ResultAccess::failure<ConfigPatch>(
            *structure.error_code(), structure.diagnostics().front().message,
            structure.diagnostics().front().field);
    const ConfigValues candidate = merge_values(base, candidate_patch);
    for (const ConfigEntry &entry : candidate_patch.entries()) {
        if (!state_->evaluator)
            return detail::ResultAccess::failure<ConfigPatch>(
                ErrorCode::internal, "Configuration schema has no rule registry", "/schema/rules");
        Result<OptionEvaluation> state = state_->evaluator(entry.option, candidate, context);
        if (!state.has_value())
            return detail::ResultAccess::failure<ConfigPatch>(
                *state.error_code(), state.diagnostics().front().message,
                state.diagnostics().front().field);
        if (!state.value().compatible)
            return detail::ResultAccess::failure<ConfigPatch>(
                ErrorCode::invalid_configuration, "Configuration option is incompatible",
                "/configuration/" + entry.option.value(), state.value().diagnostics);
    }
    return detail::ResultAccess::success(std::move(candidate_patch));
}

Result<OptionEvaluation> ConfigSchema::evaluate_option(
    const OptionId &option, const ConfigValues &base,
    const ConfigPatch &candidate_patch,
    const ConfigValidationContext &context) const
{
    auto structure = detail::ConfigSchemaAccess::validate_structure(
        *this, candidate_patch, context);
    if (!structure.has_value())
        return detail::ResultAccess::failure<OptionEvaluation>(*structure.error_code(),
                                                               structure.diagnostics().front().message,
                                                               structure.diagnostics().front().field);
    if (!find(option))
        return detail::ResultAccess::failure<OptionEvaluation>(ErrorCode::invalid_argument,
                                                               "Unknown configuration option",
                                                               "/option");
    const ConfigValues candidate = merge_values(base, candidate_patch);
    if (!state_->evaluator)
        return detail::ResultAccess::failure<OptionEvaluation>(
            ErrorCode::internal, "Configuration schema has no rule registry", "/schema/rules");
    return state_->evaluator(option, candidate, context);
}

bool detail::ConfigSchemaAccess::is_generic_option(const ConfigSchema &schema,
                                                    const OptionId &option)
{
    return schema.state_->typed_owned_options.count(option.value()) == 0;
}

ValueDescriptor detail::ConfigSchemaAccess::scalar(
    ConfigValueShape shape, bool value_nullable, std::optional<NumericConstraint> numeric,
    std::optional<FloatOrPercentConstraint> float_or_percent, std::vector<std::string> enum_values,
    std::string canonical_unit)
{
    return ValueDescriptor(std::make_shared<const ValueDescriptor::State>(ValueDescriptor::State{
        std::move(shape), value_nullable, std::move(numeric), std::move(float_or_percent),
        std::move(enum_values), std::move(canonical_unit), std::nullopt}));
}

ValueDescriptor detail::ConfigSchemaAccess::list(ValueDescriptor item, std::size_t min_items,
                                                  std::size_t max_items, bool value_nullable,
                                                  bool fixed_length)
{
    auto list_state = std::make_shared<const ListDescriptor::State>(
        ListDescriptor::State{std::move(item), min_items, max_items, fixed_length});
    ListDescriptor list_descriptor(std::move(list_state));
    return ValueDescriptor(std::make_shared<const ValueDescriptor::State>(ValueDescriptor::State{
        ConfigValueShape::list(list_descriptor.item().shape()), value_nullable, std::nullopt, std::nullopt,
        {}, {}, std::move(list_descriptor)}));
}

ConfigSchema detail::ConfigSchemaAccess::make(std::string schema_id, std::uint32_t schema_version,
                                               std::vector<OptionDescriptor> options,
                                               std::vector<std::string> typed_owned_options,
                                               OptionEvaluator evaluator)
{
    std::set<std::string> typed_owned(typed_owned_options.begin(), typed_owned_options.end());
    return ConfigSchema(std::make_shared<const ConfigSchema::State>(ConfigSchema::State{
        std::move(schema_id), schema_version, std::move(options), std::move(typed_owned),
        std::move(evaluator)}));
}

} // namespace libslicer::v1
