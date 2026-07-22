#pragma once

#include "Context.hpp"
#include "Preset.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libslicer::v1 {

using ProjectRevision = std::uint64_t;

namespace detail {
struct ProjectIdAccess;
struct ProjectSnapshotAccess;
struct EffectiveConfigurationAccess;
}

#define LIBSLICER_DECLARE_PROJECT_ID(Name)                            \
class Name {                                                         \
public:                                                              \
    std::uint64_t value() const noexcept;                             \
    friend bool operator==(const Name &, const Name &) noexcept;      \
    friend bool operator!=(const Name &lhs, const Name &rhs) noexcept \
        { return !(lhs == rhs); }                                     \
private:                                                             \
    struct Binding;                                                   \
    explicit Name(std::shared_ptr<const Binding> binding);            \
    std::shared_ptr<const Binding> binding_;                          \
    friend class Project;                                             \
    friend class ProjectSnapshot;                                     \
    friend class ProjectEdit;                                         \
    friend struct detail::ProjectIdAccess;                            \
};

LIBSLICER_DECLARE_PROJECT_ID(PlateId)
LIBSLICER_DECLARE_PROJECT_ID(ObjectId)
LIBSLICER_DECLARE_PROJECT_ID(PartId)
#undef LIBSLICER_DECLARE_PROJECT_ID

struct PlateInfo { PlateId id; std::string name; ConfigPatch overrides; bool locked; };
struct ObjectInfo { ObjectId id; std::string name; ConfigPatch overrides; };
struct PartInfo { PartId id; ObjectId object; ConfigPatch overrides; };

struct LayerRange {
    double      z_min_mm;
    double      z_max_mm;
    ConfigPatch overrides;
};

struct SlotRemap { std::vector<std::optional<FilamentSlotId>> old_to_new; };

enum class FilamentMapMode { auto_for_flush, auto_for_match, manual };

struct FilamentMapOverride {
    FilamentMapMode    mode;
    std::vector<ToolId> tools;
};

struct EffectiveFilamentMap {
    FilamentMapMode    mode;
    std::vector<ToolId> tools;
    enum class Source { project, plate, temporary } source;
};

class ProjectSnapshot {
public:
    ProjectRevision revision() const;
    ConfigPatch project_overrides() const;
    std::vector<PlateInfo> plates() const;
    std::vector<ObjectInfo> objects() const;
    std::vector<PartInfo> parts() const;
    Result<std::vector<LayerRange>> layer_ranges(ObjectId object) const;
    PresetSelection project_selected_presets() const;

    FilamentMapOverride project_filament_map() const;
    Result<std::optional<FilamentMapOverride>> local_filament_map_override(PlateId plate) const;
    Result<EffectiveFilamentMap> effective_filament_map(PlateId plate) const;

private:
    struct State;
    explicit ProjectSnapshot(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend class Project;
    friend class ProjectEdit;
    friend class SliceEngine;
    friend struct detail::ProjectSnapshotAccess;
};

class ProjectEdit {
public:
    Result<void> set_project_selected_presets(PresetSelection selection, SlotRemap remap);
    Result<void> set_project_overrides(ConfigPatch patch);
    Result<void> set_plate_overrides(PlateId plate, ConfigPatch patch);
    Result<void> set_object_overrides(ObjectId object, ConfigPatch patch);
    Result<void> set_part_overrides(PartId part, ConfigPatch patch);
    Result<void> set_layer_ranges(ObjectId object, std::vector<LayerRange> ranges);
    Result<void> set_project_filament_map(FilamentMapOverride map);
    Result<void> set_local_filament_map_override(PlateId plate,
                                                 std::optional<FilamentMapOverride> map);

    Result<ProjectRevision> commit();
    void discard() noexcept;

private:
    struct State;
    explicit ProjectEdit(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    friend class Project;
};

class Project {
public:
    static Result<Project> load(SdkContext &context, const std::filesystem::path &path);
    Result<ProjectSnapshot> snapshot() const;
    Result<ProjectEdit> begin_edit(ProjectRevision expected);
    Result<void> save(const std::filesystem::path &path, ProjectRevision expected) const;

private:
    struct State;
    explicit Project(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    friend class ProjectEdit;
};

class EffectiveConfiguration {
public:
    const ConfigSchema &schema() const;
    std::optional<ConfigValue> get(const OptionId &option) const;
    const ConfigValues &values() const;

    struct Provenance {
        ProjectRevision project_revision;
        PlateId plate;
        std::vector<std::pair<PresetRef, PresetRevision>> presets;
    };
    Provenance provenance() const;

private:
    struct State;
    explicit EffectiveConfiguration(std::shared_ptr<const State> state);
    std::shared_ptr<const State> state_;
    friend struct detail::EffectiveConfigurationAccess;
};

} // namespace libslicer::v1
