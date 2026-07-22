#pragma once

#include "Result.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libslicer::v1 {

namespace detail {
struct ConfigValueAccess;
struct ConfigValuesAccess;
}

class OptionId {
public:
    explicit OptionId(std::string value);
    std::string value() const;
    friend bool operator==(const OptionId &, const OptionId &) noexcept;
    friend bool operator!=(const OptionId &lhs, const OptionId &rhs) noexcept { return !(lhs == rhs); }

private:
    std::string value_;
};

struct FilamentSlotId {
    std::uint32_t value;
    friend bool operator==(FilamentSlotId lhs, FilamentSlotId rhs) noexcept { return lhs.value == rhs.value; }
};

struct ToolId {
    std::uint32_t value;
    friend bool operator==(ToolId lhs, ToolId rhs) noexcept { return lhs.value == rhs.value; }
};

enum class ConfigValueType {
    boolean,
    integer,
    decimal,
    percent,
    float_or_percent,
    string,
    enumeration,
    point2,
    point3,
    list
};

class ConfigValueShape {
public:
    static ConfigValueShape scalar(ConfigValueType type);
    static ConfigValueShape list(ConfigValueShape item_shape);

    ConfigValueType type() const noexcept;
    std::optional<ConfigValueShape> item_shape() const;

    friend bool operator==(const ConfigValueShape &, const ConfigValueShape &) noexcept;
    friend bool operator!=(const ConfigValueShape &lhs, const ConfigValueShape &rhs) noexcept
    {
        return !(lhs == rhs);
    }

private:
    struct State;
    explicit ConfigValueShape(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::ConfigValueAccess;
};

struct FloatOrPercent {
    double value;
    bool   is_percent;
    friend bool operator==(const FloatOrPercent &lhs, const FloatOrPercent &rhs) noexcept
    {
        return lhs.value == rhs.value && lhs.is_percent == rhs.is_percent;
    }
};

struct Point2 {
    double x, y;
    friend bool operator==(const Point2 &lhs, const Point2 &rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y;
    }
};

struct Point3 {
    double x, y, z;
    friend bool operator==(const Point3 &lhs, const Point3 &rhs) noexcept
    {
        return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
    }
};

class ConfigValue {
public:
    ConfigValueType  type() const noexcept;
    ConfigValueShape shape() const;

    static ConfigValue boolean(bool value);
    static ConfigValue integer(std::int64_t value);
    static ConfigValue decimal(double value);
    static ConfigValue percent(double value);
    static ConfigValue float_or_percent(FloatOrPercent value);
    static ConfigValue string(std::string value);
    static ConfigValue enumeration(std::string value);
    static ConfigValue point2(Point2 value);
    static ConfigValue point3(Point3 value);
    static Result<ConfigValue> list(ConfigValueShape item_shape, std::vector<ConfigValue> values);
    static ConfigValue null(ConfigValueShape underlying_shape);

    bool is_null() const noexcept;
    std::optional<bool> as_boolean() const;
    std::optional<std::int64_t> as_integer() const;
    std::optional<double> as_decimal() const;
    std::optional<double> as_percent() const;
    std::optional<FloatOrPercent> as_float_or_percent() const;
    std::optional<std::string> as_string() const;
    std::optional<std::string> as_enumeration() const;
    std::optional<Point2> as_point2() const;
    std::optional<Point3> as_point3() const;
    std::optional<std::vector<ConfigValue>> as_list() const;

    friend bool operator==(const ConfigValue &, const ConfigValue &) noexcept;
    friend bool operator!=(const ConfigValue &lhs, const ConfigValue &rhs) noexcept { return !(lhs == rhs); }

private:
    struct State;
    explicit ConfigValue(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::ConfigValueAccess;
};

struct ConfigEntry {
    OptionId    option;
    ConfigValue value;
};

class ConfigPatch {
public:
    ConfigPatch();

    void set(OptionId option, ConfigValue value);
    bool erase(const OptionId &option);
    std::optional<ConfigValue> find(const OptionId &option) const;
    const std::vector<ConfigEntry> &entries() const noexcept;

private:
    struct State;
    void ensure_unique();
    std::shared_ptr<State> state_;
};

class ConfigValues {
public:
    std::optional<ConfigValue> find(const OptionId &option) const;
    const std::vector<ConfigEntry> &entries() const noexcept;

private:
    struct State;
    explicit ConfigValues(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::ConfigValuesAccess;
};

namespace detail {

struct ConfigValuesAccess {
    static ConfigValues make(std::vector<ConfigEntry> entries);
};

} // namespace detail

} // namespace libslicer::v1
