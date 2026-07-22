#pragma once

#include "Config.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libslicer::v1 {

using PresetRevision = std::uint64_t;

enum class PresetKind { printer, process, filament };
enum class OptionScope { project, plate, object, part, layer_range, preset };
enum class FilamentReferenceKind {
    none,
    logical_slot,
    logical_slot_list
};

struct NumericConstraint {
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::string           canonical_unit;
};

struct FloatOrPercentConstraint {
    NumericConstraint        numeric;
    std::optional<OptionId>  percent_base;
};

namespace detail {
struct ConfigSchemaAccess;
}

class ValueDescriptor;

class ListDescriptor {
public:
    ValueDescriptor item() const;
    std::size_t min_items() const noexcept;
    std::size_t max_items() const noexcept;
    bool fixed_length() const noexcept;

private:
    struct State;
    explicit ListDescriptor(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::ConfigSchemaAccess;
};

class ValueDescriptor {
public:
    ConfigValueShape shape() const;
    bool value_nullable() const noexcept;
    std::optional<NumericConstraint> numeric() const;
    std::optional<FloatOrPercentConstraint> float_or_percent() const;
    std::vector<std::string> enum_values() const;
    std::string canonical_unit() const;
    std::optional<ListDescriptor> list() const;

private:
    struct State;
    explicit ValueDescriptor(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::ConfigSchemaAccess;
};

struct OptionDescriptor {
    OptionId                           id;
    ValueDescriptor                    value;
    std::string                        label;
    std::vector<OptionScope>           allowed_scopes;
    std::vector<PresetKind>            applicable_preset_kinds;
    FilamentReferenceKind             filament_reference;
    bool                               editable;
};

struct ConfigValidationContext {
    OptionScope               scope;
    std::optional<PresetKind> preset_kind;

    struct PresetValues {
        PresetRevision revision;
        ConfigValues   effective_values;
    };

    std::optional<PresetValues> printer;
    std::optional<PresetValues> process;
    std::optional<std::size_t>  filament_count;
};

struct OptionEvaluation {
    bool                    visible;
    bool                    compatible;
    std::vector<Diagnostic> diagnostics;
};

class ConfigSchema {
public:
    std::string schema_id() const;
    std::uint32_t schema_version() const noexcept;
    std::optional<OptionDescriptor> find(const OptionId &option) const;
    std::vector<OptionDescriptor> options() const;

    Result<ConfigPatch> validate_and_normalize(
        const ConfigValues &base,
        ConfigPatch candidate_patch,
        const ConfigValidationContext &context) const;
    Result<OptionEvaluation> evaluate_option(
        const OptionId &option,
        const ConfigValues &base,
        const ConfigPatch &candidate_patch,
        const ConfigValidationContext &context) const;

private:
    struct State;
    explicit ConfigSchema(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::ConfigSchemaAccess;
};

} // namespace libslicer::v1
