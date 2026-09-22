#include <libslicer/Config.hpp>

#include <libslic3r/Config.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/ContinuousFiber/ContinuousFiberConfig.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace libslicer {
namespace {

SettingType public_scalar_type(Slic3r::ConfigOptionType type)
{
    const auto scalar_type = static_cast<Slic3r::ConfigOptionType>(static_cast<int>(type) & ~static_cast<int>(Slic3r::coVectorType));
    switch (scalar_type) {
    case Slic3r::coBool: return SettingType::Boolean;
    case Slic3r::coInt: return SettingType::Integer;
    case Slic3r::coFloat: return SettingType::Float;
    case Slic3r::coPercent: return SettingType::Percent;
    case Slic3r::coFloatOrPercent: return SettingType::FloatOrPercent;
    case Slic3r::coString: return SettingType::String;
    case Slic3r::coEnum: return SettingType::Enum;
    case Slic3r::coPoint: return SettingType::Point2;
    case Slic3r::coPoint3: return SettingType::Point3;
    default: return SettingType::List;
    }
}

SettingType public_type(const Slic3r::ConfigOptionDef& option)
{
    return option.is_scalar() ? public_scalar_type(option.type) : SettingType::List;
}

bool has_presentation_metadata(const Slic3r::ConfigOptionDef& option)
{
    return !option.label.empty() || !option.full_label.empty() || !option.category.empty() || !option.tooltip.empty() ||
           option.gui_type != Slic3r::ConfigOptionDef::GUIType::undefined;
}

SettingLevel public_level(Slic3r::ConfigOptionMode mode)
{
    switch (mode) {
    case Slic3r::comAdvanced: return SettingLevel::Advanced;
    case Slic3r::comExpert: return SettingLevel::Expert;
    case Slic3r::comDevelop: return SettingLevel::Developer;
    default: return SettingLevel::Simple;
    }
}

std::set<std::string> key_set(const std::vector<std::string>& keys) { return {keys.begin(), keys.end()}; }

bool is_scalar_numeric(SettingType type)
{
    return type == SettingType::Integer || type == SettingType::Float || type == SettingType::Percent ||
           type == SettingType::FloatOrPercent;
}

class Catalog final
{
public:
    Catalog()
    {
        const auto process_keys  = key_set(Slic3r::Preset::print_options());
        const auto filament_keys = key_set(Slic3r::Preset::filament_options());
        const auto printer_keys  = key_set(Slic3r::Preset::printer_options());

        // Orca stores the active plate in project state rather than in a
        // print preset. It is nevertheless an editable slicing input and is
        // presented alongside process settings by the public three-group API.
        const std::set<std::string> process_project_keys{"curr_bed_type"};
        // Per-slot colors are also project state (they are intentionally not
        // part of Preset::filament_options()), but must travel with a project
        // so facet labels continue to address the correct material colors.
        const std::set<std::string> filament_project_keys{"filament_colour"};

        items.reserve(process_keys.size() + filament_keys.size() + printer_keys.size());
        for (const auto& [key, option] : Slic3r::print_config_def.options) {
            const bool internal    = !has_presentation_metadata(option);
            const bool visible     = !internal && option.gui_type != Slic3r::ConfigOptionDef::GUIType::legend;
            const bool is_process  = process_keys.count(key) != 0 || process_project_keys.count(key) != 0;
            const bool is_filament = filament_keys.count(key) != 0 ||
                                     filament_project_keys.count(key) != 0;
            const bool is_printer  = printer_keys.count(key) != 0;
            const int group_count  = static_cast<int>(is_process) + static_cast<int>(is_filament) + static_cast<int>(is_printer);
            if (option.printer_technology == Slic3r::ptSLA || !visible || option.readonly) {
                continue;
            }
            // Shared keys are preset metadata (for example inherits and compatibility
            // expressions), not a single editable slicing value.
            if (group_count != 1) {
                continue;
            }

            SettingItem item;
            item.key           = key;
            item.type          = public_type(option);
            item.element_type  = public_scalar_type(option.type);
            item.default_value = option.default_value ? option.default_value->serialize() : std::string{};
            item.label         = option.full_label.empty() ? option.label : option.full_label;
            if (item.label.empty()) {
                item.label = key;
            }
            item.description = option.tooltip;
            item.category    = option.category.empty() ? "Other" : option.category;
            item.unit        = option.sidetext;
            item.level       = public_level(option.mode);
            item.group       = is_process ? SettingGroup::Process : is_filament ? SettingGroup::Filament : SettingGroup::Printer;
            item.nullable    = option.nullable;
            item.visible     = true;
            item.read_only   = option.readonly;
            item.multiline   = option.multiline;

            if (item.element_type == SettingType::Integer) {
                item.step      = 1.0;
                item.precision = 0;
            } else if (item.element_type == SettingType::Float || item.element_type == SettingType::Percent ||
                       item.element_type == SettingType::FloatOrPercent || item.element_type == SettingType::Point2 ||
                       item.element_type == SettingType::Point3) {
                item.precision = 4;
            }
            if (item.type == SettingType::Point2) {
                item.fixed_size = 2;
            } else if (item.type == SettingType::Point3) {
                item.fixed_size = 3;
            }

            if (is_scalar_numeric(item.element_type)) {
                if (option.min > -FLT_MAX) {
                    item.minimum = option.min;
                }
                if (option.max < FLT_MAX) {
                    item.maximum = option.max;
                }
            }

            item.enum_items.reserve(option.enum_values.size());
            for (std::size_t index = 0; index < option.enum_values.size(); ++index) {
                const std::string& value = option.enum_values[index];
                const std::string label  = index < option.enum_labels.size() && !option.enum_labels[index].empty() ?
                                               option.enum_labels[index] :
                                               value;
                item.enum_items.push_back({value, label});
            }

            index_by_key.emplace(item.key, items.size());
            items.push_back(std::move(item));
        }
    }

    const SettingItem* find(std::string_view key) const
    {
        const auto found = index_by_key.find(std::string(key));
        if (found == index_by_key.end()) {
            return nullptr;
        }
        return &items[found->second];
    }

    std::vector<SettingItem> items;

private:
    std::unordered_map<std::string, std::size_t> index_by_key;
};

const Catalog& catalog()
{
    static const Catalog instance;
    return instance;
}

bool boolean_value(const Slic3r::DynamicPrintConfig& config, const char* key)
{
    const auto* value = config.option<Slic3r::ConfigOptionBool>(key);
    return value != nullptr && value->value;
}

bool key_is(std::string_view key, std::initializer_list<std::string_view> candidates)
{
    return std::find(candidates.begin(), candidates.end(), key) != candidates.end();
}

void apply_dynamic_presentation(SettingItem& item, const Slic3r::DynamicPrintConfig& config)
{
    const bool contour_enabled = boolean_value(config, "generate_reinforced_perimeters");
    const bool infill_enabled  = boolean_value(config, "generate_reinforced_infills");

    if (key_is(item.key, {"outer_reinforced_perimeters_counts",
                          "reinforced_perimeters_filament",
                          "reinforced_perimeters_extrusion_width"})) {
        item.enabled = contour_enabled;
    } else if (key_is(item.key, {"reinforced_infill_density",
                                 "reinforced_infill_pattern",
                                 "reinforced_infill_filament",
                                 "reinforced_infill_extrusion_width"})) {
        item.enabled = infill_enabled;
    } else if (key_is(item.key, {"fiber_contour_boundary_clearance", "fiber_contour_bend_radius"})) {
        item.enabled = contour_enabled;
    } else if (item.key == "fiber_contour_infill_clearance") {
        item.enabled = contour_enabled && infill_enabled;
    } else if (key_is(item.key, {"fiber_layer_height_ratio",
                                 "fiber_fill_debug",
                                 "fiber_minimum_path_length",
                                 "fiber_minimum_effective_length",
                                 "fiber_prefeed_extra_length",
                                 "fiber_prefeed_speed",
                                 "fiber_z_hop_height",
                                 "fiber_landing_length",
                                 "fiber_landing_speed",
                                 "fiber_adhesion_dwell_ms",
                                 "fiber_start_speed",
                                 "fiber_start_stabilization_length",
                                 "fiber_finish_extension_length",
                                 "fiber_outside_tolerance",
                                 "fiber_contour_max_speed",
                                 "fiber_infill_max_speed",
                                 "fiber_contour_feed_ratio",
                                 "fiber_infill_feed_ratio",
                                 "fiber_contour_min_speed",
                                 "fiber_infill_min_speed",
                                 "fiber_corner_transition_length",
                                 "fiber_speed_sampling_length",
                                 "fiber_tail_min_speed",
                                 "fiber_tail_max_speed",
                                 "fiber_tail_speed_step_length",
                                 "fiber_finish_overlap_length",
                                 "fiber_finish_motion_speed",
                                 "fiber_contour_acceleration",
                                 "fiber_infill_acceleration",
                                 "fiber_resin_overlap"})) {
        item.enabled = contour_enabled || infill_enabled;
    }
}

} // namespace

class ConfigSnapshot::Impl
{
public:
    explicit Impl(std::vector<std::pair<std::string, std::string>> input, bool initialized)
        : entries(std::move(input)), valid(initialized)
    {
        for (const auto& [key, value] : entries) {
            index.emplace(key, value);
        }
    }

    std::vector<std::pair<std::string, std::string>> entries;
    std::unordered_map<std::string, std::string> index;
    bool valid{false};
};

ConfigSnapshot::ConfigSnapshot() : impl_(std::make_unique<Impl>(decltype(Impl::entries){}, false)) {}
ConfigSnapshot::ConfigSnapshot(std::vector<std::pair<std::string, std::string>> values) : impl_(std::make_unique<Impl>(std::move(values), true))
{}
ConfigSnapshot::ConfigSnapshot(const ConfigSnapshot& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
ConfigSnapshot::ConfigSnapshot(ConfigSnapshot&&) noexcept = default;
ConfigSnapshot& ConfigSnapshot::operator=(const ConfigSnapshot& other)
{
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}
ConfigSnapshot& ConfigSnapshot::operator=(ConfigSnapshot&&) noexcept = default;
ConfigSnapshot::~ConfigSnapshot()                                    = default;

bool ConfigSnapshot::valid() const noexcept { return impl_->valid; }

std::optional<std::string> ConfigSnapshot::value(std::string_view key) const
{
    const auto found = impl_->index.find(std::string(key));
    return found == impl_->index.end() ? std::nullopt : std::optional<std::string>(found->second);
}

const std::vector<std::pair<std::string, std::string>>& ConfigSnapshot::values() const noexcept { return impl_->entries; }

class Config::Impl
{
public:
    Impl()
    {
        defaults.apply(Slic3r::FullPrintConfig::defaults());
        current = defaults;
    }

    explicit Impl(const std::vector<std::pair<std::string, std::string>>& baseline)
    {
        defaults.apply(Slic3r::FullPrintConfig::defaults());
        for (const auto& [key, value] : baseline) {
            defaults.set_deserialize_strict(key, value);
        }
        current = defaults;
    }

    Slic3r::DynamicPrintConfig defaults;
    Slic3r::DynamicPrintConfig current;
};

Config::Config() : impl_(std::make_unique<Impl>()) {}
Config::Config(std::vector<std::pair<std::string, std::string>> baseline)
    : impl_(std::make_unique<Impl>(baseline))
{}
Config Config::defaults() { return Config(); }
Config::Config(const Config& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Config::Config(Config&&) noexcept = default;
Config& Config::operator=(const Config& other)
{
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}
Config& Config::operator=(Config&&) noexcept = default;
Config::~Config()                            = default;

std::vector<SettingItem> Config::settings() const
{
    std::vector<SettingItem> result;
    result.reserve(catalog().items.size());
    for (const SettingItem& definition : catalog().items) {
        const auto* option = impl_->current.option(definition.key);
        if (option == nullptr) {
            continue;
        }
        SettingItem item = definition;
        item.value       = option->serialize();
        if (const auto* baseline = impl_->defaults.option(definition.key)) {
            item.default_value = baseline->serialize();
        }
        apply_dynamic_presentation(item, impl_->current);
        result.push_back(std::move(item));
    }
    return result;
}

namespace {
SettingsResult failure(std::string key, std::string message)
{
    SettingsResult result;
    result.diagnostics.push_back({std::move(key), std::move(message)});
    return result;
}

std::optional<SettingItem> current_item(const Slic3r::DynamicPrintConfig& config,
                                        const Slic3r::DynamicPrintConfig& defaults,
                                        std::string_view key)
{
    const SettingItem* definition = catalog().find(key);
    const auto* option            = definition == nullptr ? nullptr : config.option(definition->key);
    if (option == nullptr) {
        return std::nullopt;
    }
    SettingItem item = *definition;
    item.value       = option->serialize();
    if (const auto* baseline = defaults.option(definition->key)) {
        item.default_value = baseline->serialize();
    }
    apply_dynamic_presentation(item, config);
    return item;
}

void append_presentation_changes(std::vector<std::string>& changed_keys,
                                 const Slic3r::DynamicPrintConfig& before,
                                 const Slic3r::DynamicPrintConfig& after)
{
    for (const SettingItem& definition : catalog().items) {
        SettingItem previous = definition;
        SettingItem current  = definition;
        apply_dynamic_presentation(previous, before);
        apply_dynamic_presentation(current, after);
        if ((previous.enabled != current.enabled || previous.visible != current.visible) &&
            std::find(changed_keys.begin(), changed_keys.end(), definition.key) == changed_keys.end()) {
            changed_keys.push_back(definition.key);
        }
    }
}
} // namespace

SettingsResult Config::set(std::string_view key, std::string_view serialized_value)
{
    return apply_patch({{std::string(key), std::string(serialized_value)}});
}

SettingsResult Config::apply_patch(const std::vector<std::pair<std::string, std::string>>& patch)
{
    const Slic3r::DynamicPrintConfig before = impl_->current;
    Slic3r::DynamicPrintConfig candidate = before;
    std::string last_key;
    try {
        for (const auto& [key, serialized_value] : patch) {
            last_key                      = key;
            const SettingItem* definition = catalog().find(key);
            if (definition == nullptr) {
                return failure(key, "Unknown configuration option");
            }
            if (definition->read_only) {
                return failure(key, "Configuration option is read-only");
            }

            candidate.set_deserialize_strict(key, serialized_value);
            const auto* option_definition = Slic3r::print_config_def.get(key);
            const auto* option            = candidate.option(key);
            if (option_definition != nullptr && option != nullptr) {
                std::optional<double> numeric_value;
                if (definition->type == SettingType::Integer) {
                    numeric_value = static_cast<double>(option->getInt());
                } else if (definition->type == SettingType::Float ||
                           definition->type == SettingType::Percent ||
                           definition->type == SettingType::FloatOrPercent) {
                    numeric_value = option->getFloat();
                }
                if (numeric_value && !option_definition->is_value_valid(*numeric_value)) {
                    return failure(key, "Configuration value is outside the allowed range");
                }
            }
        }
    } catch (const std::exception& error) {
        return failure(last_key, error.what());
    }

    auto changed_keys = candidate.diff(before);
    append_presentation_changes(changed_keys, before, candidate);
    impl_->current          = std::move(candidate);
    SettingsResult result;
    result.success = true;
    result.changed_items.reserve(changed_keys.size());
    for (const std::string& key : changed_keys) {
        if (auto item = current_item(impl_->current, impl_->defaults, key)) {
            result.changed_items.push_back(std::move(*item));
        }
    }
    return result;
}

SettingsResult Config::reset(std::string_view key)
{
    const std::string owned_key(key);
    const SettingItem* definition = catalog().find(owned_key);
    if (definition == nullptr || impl_->defaults.option(owned_key) == nullptr) {
        return failure(owned_key, "Unknown configuration option");
    }
    if (definition->read_only) {
        return failure(owned_key, "Configuration option is read-only");
    }
    const Slic3r::DynamicPrintConfig before = impl_->current;
    const bool changed = before.opt_serialize(owned_key) != impl_->defaults.opt_serialize(owned_key);
    impl_->current.apply_only(impl_->defaults, {owned_key});
    SettingsResult result;
    result.success = true;
    std::vector<std::string> changed_keys;
    if (changed)
        changed_keys.push_back(owned_key);
    append_presentation_changes(changed_keys, before, impl_->current);
    for (const std::string& changed_key : changed_keys) {
        if (auto item = current_item(impl_->current, impl_->defaults, changed_key)) {
            result.changed_items.push_back(std::move(*item));
        }
    }
    return result;
}

std::vector<ConfigDiagnostic> Config::validate() const
{
    std::vector<ConfigDiagnostic> diagnostics;
    try {
        const auto errors = impl_->current.validate(false);
        diagnostics.reserve(errors.size());
        for (const auto& [key, message] : errors) {
            diagnostics.push_back({key, message});
        }
        Slic3r::GCodeConfig tools;
        tools.apply(impl_->current, true);
        Slic3r::validate_material_tool_bindings(tools);
    } catch (const std::exception& error) {
        diagnostics.push_back({{}, error.what()});
    }
    return diagnostics;
}

ConfigSnapshot Config::snapshot() const
{
    std::vector<std::pair<std::string, std::string>> values;
    const auto keys = impl_->current.keys();
    values.reserve(keys.size());
    for (const std::string& key : keys) {
        values.emplace_back(key, impl_->current.opt_serialize(key));
    }
    return ConfigSnapshot(std::move(values));
}

} // namespace libslicer
