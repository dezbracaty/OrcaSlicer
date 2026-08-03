#pragma once

#include "Export.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libslicer {

enum class SettingType { Boolean, Integer, Float, Percent, FloatOrPercent, String, Enum, Point2, Point3, List };

enum class SettingLevel { Simple, Advanced, Expert, Developer };

enum class SettingGroup { Process, Filament, Printer };

struct EnumItem
{
    std::string value;
    std::string label;
};

struct SettingItem
{
    std::string key;
    std::string value;
    std::string default_value;

    std::string label;
    std::string description;
    std::string category;
    std::string unit;

    SettingType type{SettingType::String};
    SettingType element_type{SettingType::String};

    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<std::size_t> fixed_size;
    std::optional<double> step;
    std::optional<int> precision;
    std::vector<EnumItem> enum_items;

    SettingGroup group{SettingGroup::Process};
    SettingLevel level{SettingLevel::Simple};
    bool nullable{false};
    bool visible{true};
    bool enabled{true};
    bool read_only{false};
    bool multiline{false};
};

struct ConfigDiagnostic
{
    std::string key;
    std::string message;
};

struct SettingsResult
{
    bool success{false};
    std::vector<SettingItem> changed_items;
    std::vector<ConfigDiagnostic> diagnostics;

    explicit operator bool() const noexcept { return success; }
};

class LIBSLICER_API ConfigSnapshot final
{
public:
    ConfigSnapshot();
    ConfigSnapshot(const ConfigSnapshot&);
    ConfigSnapshot(ConfigSnapshot&&) noexcept;
    ConfigSnapshot& operator=(const ConfigSnapshot&);
    ConfigSnapshot& operator=(ConfigSnapshot&&) noexcept;
    ~ConfigSnapshot();

    bool valid() const noexcept;
    std::optional<std::string> value(std::string_view key) const;
    const std::vector<std::pair<std::string, std::string>>& values() const noexcept;

private:
    explicit ConfigSnapshot(std::vector<std::pair<std::string, std::string>> values);

    class Impl;
    std::unique_ptr<Impl> impl_;
    friend class Config;
};

class LIBSLICER_API Config final
{
public:
    static Config defaults();

    Config(const Config&);
    Config(Config&&) noexcept;
    Config& operator=(const Config&);
    Config& operator=(Config&&) noexcept;
    ~Config();

    // Full display snapshot. Call once when initializing or rebuilding a view.
    std::vector<SettingItem> settings() const;

    // Successful edits return only items whose current display state changed.
    SettingsResult set(std::string_view key, std::string_view serialized_value);
    SettingsResult apply_patch(const std::vector<std::pair<std::string, std::string>>& patch);
    SettingsResult reset(std::string_view key);
    std::vector<ConfigDiagnostic> validate() const;
    ConfigSnapshot snapshot() const;

private:
    Config();
    explicit Config(std::vector<std::pair<std::string, std::string>> baseline);

    class Impl;
    std::unique_ptr<Impl> impl_;
    friend class Library;
};

} // namespace libslicer
