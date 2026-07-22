#include <libslicer/v1/json.hpp>
#include <libslicer/v1/ConfigSchema.hpp>
#include <libslicer/v1/Project.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace libslicer::v1 {

namespace {

using Json = nlohmann::json;

bool has_only_keys(const Json &object, std::initializer_list<const char *> allowed)
{
    if (!object.is_object()) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        const bool known = std::any_of(allowed.begin(), allowed.end(), [&](const char *key) {
            return it.key() == key;
        });
        if (!known) return false;
    }
    return true;
}

const char *preset_kind_name(PresetKind kind)
{
    switch (kind) {
    case PresetKind::printer: return "printer";
    case PresetKind::process: return "process";
    case PresetKind::filament: return "filament";
    }
    return "unknown";
}

const char *preset_origin_name(PresetOrigin origin)
{
    switch (origin) {
    case PresetOrigin::system: return "system";
    case PresetOrigin::vendor: return "vendor";
    case PresetOrigin::user: return "user";
    case PresetOrigin::project_embedded: return "project_embedded";
    }
    return "unknown";
}

const char *type_name(ConfigValueType type)
{
    switch (type) {
    case ConfigValueType::boolean: return "boolean";
    case ConfigValueType::integer: return "integer";
    case ConfigValueType::decimal: return "decimal";
    case ConfigValueType::percent: return "percent";
    case ConfigValueType::float_or_percent: return "float_or_percent";
    case ConfigValueType::string: return "string";
    case ConfigValueType::enumeration: return "enumeration";
    case ConfigValueType::point2: return "point2";
    case ConfigValueType::point3: return "point3";
    case ConfigValueType::list: return "list";
    }
    return "unknown";
}

Result<ConfigValueType> parse_type(const std::string &name)
{
    if (name == "boolean") return detail::ResultAccess::success(ConfigValueType::boolean);
    if (name == "integer") return detail::ResultAccess::success(ConfigValueType::integer);
    if (name == "decimal") return detail::ResultAccess::success(ConfigValueType::decimal);
    if (name == "percent") return detail::ResultAccess::success(ConfigValueType::percent);
    if (name == "float_or_percent") return detail::ResultAccess::success(ConfigValueType::float_or_percent);
    if (name == "string") return detail::ResultAccess::success(ConfigValueType::string);
    if (name == "enumeration") return detail::ResultAccess::success(ConfigValueType::enumeration);
    if (name == "point2") return detail::ResultAccess::success(ConfigValueType::point2);
    if (name == "point3") return detail::ResultAccess::success(ConfigValueType::point3);
    if (name == "list") return detail::ResultAccess::success(ConfigValueType::list);
    return detail::ResultAccess::failure<ConfigValueType>(
        ErrorCode::invalid_argument, "Unknown configuration value type", "/json/type");
}

Json shape_json(const ConfigValueShape &shape)
{
    Json result{{"type", type_name(shape.type())}};
    if (auto item = shape.item_shape()) result["item"] = shape_json(*item);
    return result;
}

Result<ConfigValueShape> parse_shape(const Json &json)
{
    if (!has_only_keys(json, {"type", "item"}) || !json.contains("type") ||
        !json.at("type").is_string())
        return detail::ResultAccess::failure<ConfigValueShape>(
            ErrorCode::invalid_argument, "Configuration shape is malformed", "/json/shape");
    auto type = parse_type(json.at("type").get<std::string>());
    if (!type.has_value())
        return detail::ResultAccess::failure<ConfigValueShape>(
            *type.error_code(), type.diagnostics().front().message, "/json/shape/type");
    if (type.value() != ConfigValueType::list)
        return detail::ResultAccess::success(ConfigValueShape::scalar(type.value()));
    if (!json.contains("item"))
        return detail::ResultAccess::failure<ConfigValueShape>(
            ErrorCode::invalid_argument, "List shape is missing its item shape", "/json/shape/item");
    auto item = parse_shape(json.at("item"));
    if (!item.has_value()) return item;
    return detail::ResultAccess::success(ConfigValueShape::list(std::move(item).value()));
}

Json value_json(const ConfigValue &value)
{
    Json result{{"shape", shape_json(value.shape())}, {"null", value.is_null()}};
    if (value.is_null()) return result;
    switch (value.type()) {
    case ConfigValueType::boolean: result["value"] = *value.as_boolean(); break;
    case ConfigValueType::integer: result["value"] = *value.as_integer(); break;
    case ConfigValueType::decimal: result["value"] = *value.as_decimal(); break;
    case ConfigValueType::percent: result["value"] = *value.as_percent(); break;
    case ConfigValueType::float_or_percent: {
        const auto item = *value.as_float_or_percent();
        result["value"] = {{"value", item.value}, {"is_percent", item.is_percent}};
        break;
    }
    case ConfigValueType::string: result["value"] = *value.as_string(); break;
    case ConfigValueType::enumeration: result["value"] = *value.as_enumeration(); break;
    case ConfigValueType::point2: {
        const auto point = *value.as_point2(); result["value"] = {point.x, point.y}; break;
    }
    case ConfigValueType::point3: {
        const auto point = *value.as_point3(); result["value"] = {point.x, point.y, point.z}; break;
    }
    case ConfigValueType::list: {
        result["value"] = Json::array();
        const auto items = value.as_list();
        for (const auto &item : *items) result["value"].push_back(value_json(item));
        break;
    }
    }
    return result;
}

Result<ConfigValue> parse_value(const Json &json)
{
    try {
        if (!has_only_keys(json, {"shape", "null", "value"}) ||
            !json.contains("shape") || !json.contains("null") ||
            !json.at("null").is_boolean())
            return detail::ResultAccess::failure<ConfigValue>(
                ErrorCode::invalid_argument, "Configuration value contains unknown or missing fields",
                "/json/value");
        auto shape = parse_shape(json.at("shape"));
        if (!shape.has_value())
            return detail::ResultAccess::failure<ConfigValue>(
                *shape.error_code(), shape.diagnostics().front().message,
                shape.diagnostics().front().field);
        if (json.value("null", false))
            return detail::ResultAccess::success(ConfigValue::null(std::move(shape).value()));
        const auto &value = json.at("value");
        switch (shape.value().type()) {
        case ConfigValueType::boolean:
            return detail::ResultAccess::success(ConfigValue::boolean(value.get<bool>()));
        case ConfigValueType::integer:
            return detail::ResultAccess::success(ConfigValue::integer(value.get<std::int64_t>()));
        case ConfigValueType::decimal:
            return detail::ResultAccess::success(ConfigValue::decimal(value.get<double>()));
        case ConfigValueType::percent:
            return detail::ResultAccess::success(ConfigValue::percent(value.get<double>()));
        case ConfigValueType::float_or_percent:
            if (!has_only_keys(value, {"value", "is_percent"}))
                return detail::ResultAccess::failure<ConfigValue>(
                    ErrorCode::invalid_argument, "Float-or-percent value contains an unknown field",
                    "/json/value");
            return detail::ResultAccess::success(ConfigValue::float_or_percent(
                {value.at("value").get<double>(), value.at("is_percent").get<bool>()}));
        case ConfigValueType::string:
            return detail::ResultAccess::success(ConfigValue::string(value.get<std::string>()));
        case ConfigValueType::enumeration:
            return detail::ResultAccess::success(ConfigValue::enumeration(value.get<std::string>()));
        case ConfigValueType::point2:
            return detail::ResultAccess::success(ConfigValue::point2(
                {value.at(0).get<double>(), value.at(1).get<double>()}));
        case ConfigValueType::point3:
            return detail::ResultAccess::success(ConfigValue::point3(
                {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()}));
        case ConfigValueType::list: {
            std::vector<ConfigValue> items;
            for (const auto &item_json : value) {
                auto item = parse_value(item_json);
                if (!item.has_value()) return item;
                items.push_back(std::move(item).value());
            }
            return ConfigValue::list(*shape.value().item_shape(), std::move(items));
        }
        }
    } catch (const std::exception &error) {
        return detail::ResultAccess::failure<ConfigValue>(
            ErrorCode::invalid_argument, error.what(), "/json/value");
    }
    return detail::ResultAccess::failure<ConfigValue>(
        ErrorCode::invalid_argument, "Unsupported configuration value", "/json/value");
}

Json entries_json(const std::vector<ConfigEntry> &entries)
{
    Json result = Json::array();
    for (const auto &entry : entries)
        result.push_back({{"option", entry.option.value()}, {"value", value_json(entry.value)}});
    return result;
}

} // namespace

OptionId::OptionId(std::string value) : value_(std::move(value)) {}

std::string OptionId::value() const { return value_; }

bool operator==(const OptionId &lhs, const OptionId &rhs) noexcept { return lhs.value_ == rhs.value_; }

struct ConfigValueShape::State {
    ConfigValueType                    type;
    std::shared_ptr<const ConfigValueShape> item;
};

ConfigValueShape::ConfigValueShape(std::shared_ptr<const State> state) : state_(std::move(state)) {}

ConfigValueShape ConfigValueShape::scalar(ConfigValueType type)
{
    if (type == ConfigValueType::list)
        throw std::invalid_argument("ConfigValueShape::scalar() does not accept list");
    return ConfigValueShape(std::make_shared<State>(State{type, {}}));
}

ConfigValueShape ConfigValueShape::list(ConfigValueShape item_shape)
{
    return ConfigValueShape(std::make_shared<State>(
        State{ConfigValueType::list, std::make_shared<const ConfigValueShape>(std::move(item_shape))}));
}

ConfigValueType ConfigValueShape::type() const noexcept { return state_->type; }

std::optional<ConfigValueShape> ConfigValueShape::item_shape() const
{
    return state_->item ? std::optional<ConfigValueShape>{*state_->item} : std::nullopt;
}

bool operator==(const ConfigValueShape &lhs, const ConfigValueShape &rhs) noexcept
{
    if (lhs.state_->type != rhs.state_->type)
        return false;
    if (!lhs.state_->item || !rhs.state_->item)
        return !lhs.state_->item && !rhs.state_->item;
    return *lhs.state_->item == *rhs.state_->item;
}

using ConfigPayload = std::variant<std::monostate, bool, std::int64_t, double, FloatOrPercent,
                                   std::string, Point2, Point3, std::vector<ConfigValue>>;

struct ConfigValue::State {
    ConfigValueShape shape;
    bool             is_null;
    ConfigPayload    payload;
};

ConfigValue::ConfigValue(std::shared_ptr<const State> state) : state_(std::move(state)) {}

ConfigValueType ConfigValue::type() const noexcept { return state_->shape.type(); }
ConfigValueShape ConfigValue::shape() const { return state_->shape; }

#define LIBSLICER_DEFINE_CONFIG_FACTORY(name, value_type, config_type)                                  \
    ConfigValue ConfigValue::name(value_type value)                                                     \
    {                                                                                                   \
        return ConfigValue(std::make_shared<State>(                                                     \
            State{ConfigValueShape::scalar(config_type), false, ConfigPayload{std::move(value)}}));     \
    }

LIBSLICER_DEFINE_CONFIG_FACTORY(boolean, bool, ConfigValueType::boolean)
LIBSLICER_DEFINE_CONFIG_FACTORY(integer, std::int64_t, ConfigValueType::integer)
LIBSLICER_DEFINE_CONFIG_FACTORY(decimal, double, ConfigValueType::decimal)
LIBSLICER_DEFINE_CONFIG_FACTORY(percent, double, ConfigValueType::percent)
LIBSLICER_DEFINE_CONFIG_FACTORY(point2, Point2, ConfigValueType::point2)
LIBSLICER_DEFINE_CONFIG_FACTORY(point3, Point3, ConfigValueType::point3)

ConfigValue ConfigValue::float_or_percent(FloatOrPercent value)
{
    return ConfigValue(std::make_shared<State>(State{ConfigValueShape::scalar(ConfigValueType::float_or_percent),
                                                      false, ConfigPayload{value}}));
}
ConfigValue ConfigValue::string(std::string value)
{
    return ConfigValue(std::make_shared<State>(State{ConfigValueShape::scalar(ConfigValueType::string), false,
                                                      ConfigPayload{std::move(value)}}));
}
ConfigValue ConfigValue::enumeration(std::string value)
{
    return ConfigValue(std::make_shared<State>(State{ConfigValueShape::scalar(ConfigValueType::enumeration),
                                                      false, ConfigPayload{std::move(value)}}));
}

#undef LIBSLICER_DEFINE_CONFIG_FACTORY

Result<ConfigValue> ConfigValue::list(ConfigValueShape item_shape, std::vector<ConfigValue> values)
{
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (values[index].shape() != item_shape) {
            return detail::ResultAccess::failure<ConfigValue>(
                ErrorCode::invalid_argument,
                "List item shape does not match the declared item shape",
                "/values/" + std::to_string(index));
        }
    }
    auto state = std::make_shared<State>(State{ConfigValueShape::list(std::move(item_shape)), false,
                                                ConfigPayload{std::move(values)}});
    return detail::ResultAccess::success(ConfigValue(std::move(state)));
}

ConfigValue ConfigValue::null(ConfigValueShape underlying_shape)
{
    return ConfigValue(std::make_shared<State>(
        State{std::move(underlying_shape), true, ConfigPayload{std::monostate{}}}));
}

bool ConfigValue::is_null() const noexcept { return state_->is_null; }

#define LIBSLICER_DEFINE_CONFIG_ACCESSOR(name, value_type, config_type)                                 \
    std::optional<value_type> ConfigValue::name() const                                                 \
    {                                                                                                   \
        if (state_->is_null || state_->shape.type() != config_type)                                     \
            return std::nullopt;                                                                        \
        if (const auto *value = std::get_if<value_type>(&state_->payload))                              \
            return *value;                                                                              \
        return std::nullopt;                                                                            \
    }

LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_boolean, bool, ConfigValueType::boolean)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_integer, std::int64_t, ConfigValueType::integer)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_decimal, double, ConfigValueType::decimal)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_percent, double, ConfigValueType::percent)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_float_or_percent, FloatOrPercent, ConfigValueType::float_or_percent)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_string, std::string, ConfigValueType::string)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_enumeration, std::string, ConfigValueType::enumeration)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_point2, Point2, ConfigValueType::point2)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_point3, Point3, ConfigValueType::point3)
LIBSLICER_DEFINE_CONFIG_ACCESSOR(as_list, std::vector<ConfigValue>, ConfigValueType::list)

#undef LIBSLICER_DEFINE_CONFIG_ACCESSOR

bool operator==(const ConfigValue &lhs, const ConfigValue &rhs) noexcept
{
    return lhs.state_->shape == rhs.state_->shape && lhs.state_->is_null == rhs.state_->is_null &&
           lhs.state_->payload == rhs.state_->payload;
}

struct ConfigPatch::State {
    std::vector<ConfigEntry> entries;
};

ConfigPatch::ConfigPatch() : state_(std::make_shared<State>()) {}

void ConfigPatch::ensure_unique()
{
    if (!state_.unique())
        state_ = std::make_shared<State>(*state_);
}

void ConfigPatch::set(OptionId option, ConfigValue value)
{
    ensure_unique();
    auto it = std::find_if(state_->entries.begin(), state_->entries.end(),
                           [&option](const ConfigEntry &entry) { return entry.option == option; });
    if (it == state_->entries.end())
        state_->entries.push_back({std::move(option), std::move(value)});
    else
        it->value = std::move(value);
}

bool ConfigPatch::erase(const OptionId &option)
{
    ensure_unique();
    auto it = std::find_if(state_->entries.begin(), state_->entries.end(),
                           [&option](const ConfigEntry &entry) { return entry.option == option; });
    if (it == state_->entries.end())
        return false;
    state_->entries.erase(it);
    return true;
}

std::optional<ConfigValue> ConfigPatch::find(const OptionId &option) const
{
    auto it = std::find_if(state_->entries.begin(), state_->entries.end(),
                           [&option](const ConfigEntry &entry) { return entry.option == option; });
    return it == state_->entries.end() ? std::nullopt : std::optional<ConfigValue>{it->value};
}

const std::vector<ConfigEntry> &ConfigPatch::entries() const noexcept { return state_->entries; }

struct ConfigValues::State {
    std::vector<ConfigEntry> entries;
};

ConfigValues::ConfigValues(std::shared_ptr<const State> state) : state_(std::move(state)) {}

std::optional<ConfigValue> ConfigValues::find(const OptionId &option) const
{
    auto it = std::find_if(state_->entries.begin(), state_->entries.end(),
                           [&option](const ConfigEntry &entry) { return entry.option == option; });
    return it == state_->entries.end() ? std::nullopt : std::optional<ConfigValue>{it->value};
}

const std::vector<ConfigEntry> &ConfigValues::entries() const noexcept { return state_->entries; }

ConfigValues detail::ConfigValuesAccess::make(std::vector<ConfigEntry> entries)
{
    return ConfigValues(std::make_shared<const ConfigValues::State>(ConfigValues::State{std::move(entries)}));
}

Result<std::string> export_json(const ConfigPatch &patch, const ConfigSchema &schema)
{
    try {
        Json document{{"schema_id", schema.schema_id()},
                      {"schema_version", schema.schema_version()},
                      {"kind", "config_patch"},
                      {"entries", entries_json(patch.entries())}};
        return detail::ResultAccess::success(document.dump());
    } catch (const std::exception &error) {
        return detail::ResultAccess::failure<std::string>(
            ErrorCode::internal, error.what(), "/json");
    }
}

Result<ConfigPatch> import_json(std::string_view text, const ConfigSchema &schema)
{
    try {
        const Json document = Json::parse(text.begin(), text.end());
        if (!has_only_keys(document, {"schema_id", "schema_version", "kind", "entries"}))
            return detail::ResultAccess::failure<ConfigPatch>(
                ErrorCode::invalid_argument, "ConfigPatch JSON contains an unknown field", "/json");
        if (document.value("schema_id", std::string{}) != schema.schema_id() ||
            document.value("schema_version", std::uint32_t{0}) != schema.schema_version())
            return detail::ResultAccess::failure<ConfigPatch>(
                ErrorCode::invalid_configuration, "Configuration schema id or version differs",
                "/json/schema_version");
        if (document.value("kind", std::string{}) != "config_patch" ||
            !document.contains("entries") || !document.at("entries").is_array())
            return detail::ResultAccess::failure<ConfigPatch>(
                ErrorCode::invalid_argument, "JSON document is not a ConfigPatch", "/json/kind");
        ConfigPatch patch;
        std::vector<std::string> seen;
        std::size_t index = 0;
        for (const auto &entry : document.at("entries")) {
            if (!has_only_keys(entry, {"option", "value"}) || !entry.contains("option") ||
                !entry.contains("value"))
                return detail::ResultAccess::failure<ConfigPatch>(
                    ErrorCode::invalid_argument, "ConfigPatch entry contains unknown or missing fields",
                    "/json/entries/" + std::to_string(index));
            const std::string option = entry.at("option").get<std::string>();
            if (std::find(seen.begin(), seen.end(), option) != seen.end())
                return detail::ResultAccess::failure<ConfigPatch>(
                    ErrorCode::invalid_argument, "ConfigPatch JSON contains a duplicate option",
                    "/json/entries/" + std::to_string(index) + "/option");
            seen.push_back(option);
            if (!schema.find(OptionId(option)))
                return detail::ResultAccess::failure<ConfigPatch>(
                    ErrorCode::invalid_configuration, "ConfigPatch JSON contains an unknown option",
                    "/json/entries/" + std::to_string(index) + "/option");
            auto value = parse_value(entry.at("value"));
            if (!value.has_value())
                return detail::ResultAccess::failure<ConfigPatch>(
                    *value.error_code(), value.diagnostics().front().message,
                    "/json/entries/" + std::to_string(index) + "/value");
            patch.set(OptionId(option), std::move(value).value());
            ++index;
        }
        return detail::ResultAccess::success(std::move(patch));
    } catch (const std::exception &error) {
        return detail::ResultAccess::failure<ConfigPatch>(
            ErrorCode::invalid_argument, error.what(), "/json");
    }
}

Result<std::string> export_json(const EffectiveConfiguration &configuration)
{
    try {
        const auto provenance = configuration.provenance();
        Json presets = Json::array();
        for (const auto &preset : provenance.presets) {
            presets.push_back({{"kind", preset_kind_name(preset.first.kind())},
                               {"origin", preset_origin_name(preset.first.origin())},
                               {"id", preset.first.id()},
                               {"revision", preset.second}});
        }
        Json document{{"schema_id", configuration.schema().schema_id()},
                      {"schema_version", configuration.schema().schema_version()},
                      {"kind", "effective_configuration"},
                      {"entries", entries_json(configuration.values().entries())},
                      {"provenance", {{"project_revision", provenance.project_revision},
                                      {"plate", provenance.plate.value()},
                                      {"presets", std::move(presets)}}}};
        return detail::ResultAccess::success(document.dump());
    } catch (const std::exception &error) {
        return detail::ResultAccess::failure<std::string>(
            ErrorCode::internal, error.what(), "/json");
    }
}

} // namespace libslicer::v1
