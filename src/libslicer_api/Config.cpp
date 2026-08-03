#include <libslicer/Config.hpp>

#include <libslic3r/Config.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PrintConfig.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
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

        items.reserve(process_keys.size() + filament_keys.size() + printer_keys.size());
        for (const auto& [key, option] : Slic3r::print_config_def.options) {
            const bool internal    = !has_presentation_metadata(option);
            const bool visible     = !internal && option.gui_type != Slic3r::ConfigOptionDef::GUIType::legend;
            const bool is_process  = process_keys.count(key) != 0 || process_project_keys.count(key) != 0;
            const bool is_filament = filament_keys.count(key) != 0;
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
    return item;
}
} // namespace

SettingsResult Config::set(std::string_view key, std::string_view serialized_value)
{
    return apply_patch({{std::string(key), std::string(serialized_value)}});
}

SettingsResult Config::apply_patch(const std::vector<std::pair<std::string, std::string>>& patch)
{
    Slic3r::DynamicPrintConfig candidate = impl_->current;
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
            if (option_definition != nullptr && option != nullptr && definition->type != SettingType::List &&
                is_scalar_numeric(definition->element_type) && !option_definition->is_value_valid(option->getFloat())) {
                return failure(key, "Configuration value is outside the allowed range");
            }
        }
    } catch (const std::exception& error) {
        return failure(last_key, error.what());
    }

    const auto changed_keys = candidate.diff(impl_->current);
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
    const bool changed = impl_->current.opt_serialize(owned_key) != impl_->defaults.opt_serialize(owned_key);
    impl_->current.apply_only(impl_->defaults, {owned_key});
    SettingsResult result;
    result.success = true;
    if (changed) {
        if (auto item = current_item(impl_->current, impl_->defaults, owned_key)) {
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
