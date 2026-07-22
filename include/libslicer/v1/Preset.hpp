#pragma once

#include "ConfigSchema.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libslicer::v1 {

namespace detail { struct PresetRefAccess; }

enum class PresetOrigin { system, vendor, user, project_embedded };

class PresetRef {
public:
    static PresetRef system(PresetKind kind, std::string id);
    static PresetRef vendor(PresetKind kind, std::string id);
    static PresetRef user(PresetKind kind, std::string id);

    PresetKind kind() const noexcept;
    PresetOrigin origin() const noexcept;
    std::string id() const;

    friend bool operator==(const PresetRef &, const PresetRef &) noexcept;
    friend bool operator!=(const PresetRef &lhs, const PresetRef &rhs) noexcept { return !(lhs == rhs); }

private:
    struct Binding;
    explicit PresetRef(std::shared_ptr<const Binding> binding);
    std::shared_ptr<const Binding> binding_;

    friend class PresetRepository;
    friend class PresetView;
    friend class PresetEditor;
    friend struct detail::PresetRefAccess;
};

struct SelectedPreset {
    PresetRef      ref;
    PresetRevision revision;
};

struct PresetSummary {
    PresetRef                     ref;
    std::string                   name;
    std::string                   vendor;
    std::optional<SelectedPreset> parent;
    PresetRevision               revision;
};

struct PresetSelection {
    SelectedPreset              printer;
    SelectedPreset              process;
    std::vector<SelectedPreset> filaments;
};

struct PresetCompatibilityContext {
    std::optional<SelectedPreset> printer;
    std::optional<SelectedPreset> process;
};

struct PresetCompatibility {
    bool                    compatible;
    std::vector<Diagnostic> diagnostics;
};

class PresetView {
public:
    PresetSummary metadata() const;
    ConfigValues inherited_values() const;
    ConfigValues effective_values() const;
    ConfigPatch overrides() const;

private:
    struct State;
    explicit PresetView(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend class PresetRepository;
    friend class PresetEditor;
};

class PresetEditor {
public:
    PresetView snapshot() const;
    Result<void> set_override(OptionId option, ConfigValue value);
    Result<void> erase_override(const OptionId &option);
    Result<PresetSummary> commit();
    void discard() noexcept;

private:
    struct State;
    explicit PresetEditor(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    friend class PresetRepository;
};

class PresetRepository {
public:
    ConfigSchema schema() const;
    Result<std::vector<PresetSummary>> list(PresetKind kind) const;
    Result<PresetView> get(SelectedPreset preset) const;
    Result<PresetEditor> edit(PresetRef preset, PresetRevision expected);
    Result<PresetEditor> create(PresetKind kind, std::string id, std::string display_name,
                                std::optional<SelectedPreset> parent);
    Result<void> erase(PresetRef preset, PresetRevision expected);
    Result<PresetCompatibility> check_compatibility(
        SelectedPreset candidate, const PresetCompatibilityContext &context) const;

private:
    struct State;
    explicit PresetRepository(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    friend class SdkContext;
};

} // namespace libslicer::v1
