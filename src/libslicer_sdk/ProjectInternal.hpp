#pragma once

#include <libslicer/v1/Project.hpp>
#include <libslicer/v1/Slice.hpp>

#include "ContextInternal.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace libslicer::v1::detail {

enum class SliceInputMode {
    inspect,
    submit
};

struct ContextAccess {
    static std::shared_ptr<ContextState> state(SdkContext &context) { return context.state_; }
};

struct ProjectIdentity { std::uint64_t serial; };
struct ProjectData;

struct ProjectTemporaryFiles {
    explicit ProjectTemporaryFiles(std::filesystem::path value) : root(std::move(value)) {}
    ~ProjectTemporaryFiles()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
};

struct ProjectIdAccess {
    static PlateId plate(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value);
    static ObjectId object(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value);
    static PartId part(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value);
    static InstanceId instance(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value);
    static bool belongs(const PlateId &id, const std::shared_ptr<const ProjectIdentity> &project);
    static bool belongs(const ObjectId &id, const std::shared_ptr<const ProjectIdentity> &project);
    static bool belongs(const PartId &id, const std::shared_ptr<const ProjectIdentity> &project);
    static bool belongs(const InstanceId &id, const std::shared_ptr<const ProjectIdentity> &project);
};

struct ProjectSnapshotAccess {
    static const std::shared_ptr<const ProjectData> &data(const ProjectSnapshot &snapshot);
    static const std::shared_ptr<ContextState> &context(const ProjectSnapshot &snapshot);
};

struct EffectiveConfigurationAccess {
    static EffectiveConfiguration make(ConfigSchema schema, ConfigValues values,
                                       EffectiveConfiguration::Provenance provenance,
                                       Slic3r::DynamicPrintConfig core);
    static const Slic3r::DynamicPrintConfig &core(const EffectiveConfiguration &configuration);
};

struct EmbeddedPresetRecord {
    PresetSummary summary;
    ConfigValues  inherited;
    ConfigValues  effective;
    ConfigPatch   overrides;
    Slic3r::Preset core;
};

struct ProjectData {
    ProjectData(ConfigSchema schema_value, ResourceLimits limits_value,
                PresetSelection selection_value,
                std::shared_ptr<const ProjectIdentity> identity_value)
        : identity(std::move(identity_value)), schema(std::move(schema_value)), limits(limits_value),
          selection(std::move(selection_value)) {}
    ProjectData(const ProjectData &) = default;

    ProjectRevision revision {1};
    std::shared_ptr<const ProjectIdentity> identity;
    std::filesystem::path source_path;
    std::shared_ptr<ProjectTemporaryFiles> temporary_files;
    ConfigSchema schema;
    ResourceLimits limits;

    Slic3r::Model model;
    Slic3r::DynamicPrintConfig project_config;
    std::vector<Slic3r::PlateData> plates;
    std::vector<Slic3r::Preset> embedded_core;

    ConfigPatch project_overrides;
    PresetSelection selection;
    std::map<std::string, EmbeddedPresetRecord> embedded;
};

Result<ConfigPatch> config_to_generic_patch(const Slic3r::ConfigBase &config,
                                            const ConfigSchema &schema);

struct TemporarySliceSelection {
    PresetSelection     selection;
    FilamentMapOverride complete_manual_map;
};

struct FrozenSliceInput {
    std::shared_ptr<ContextState> context;
    ProjectSnapshot project;
    PlateId plate;
    EffectiveConfiguration configuration;
    EffectiveFilamentMap filament_map;
    SliceOutputOptions output;
};

Result<FrozenSliceInput> resolve_slice_input(
    std::shared_ptr<ContextState> context, ProjectSnapshot project, PlateId plate,
    std::optional<TemporarySliceSelection> temporary_selection = std::nullopt,
    SliceInputMode mode = SliceInputMode::submit);

} // namespace libslicer::v1::detail

namespace libslicer::v1 {

struct ProjectSnapshot::State {
    State(std::shared_ptr<const detail::ProjectData> value,
          std::shared_ptr<detail::ContextState> context_value)
        : data(std::move(value)), context(std::move(context_value)) {}
    std::shared_ptr<const detail::ProjectData> data;
    std::shared_ptr<detail::ContextState> context;
};

struct Project::State {
    mutable std::mutex mutex;
    std::shared_ptr<const detail::ProjectData> current;
    std::shared_ptr<detail::ContextState> context;
};

struct ProjectEdit::State {
    std::shared_ptr<Project::State> project;
    std::shared_ptr<const detail::ProjectData> base;
    std::shared_ptr<detail::ProjectData> staged;
    std::thread::id owner;
    bool terminal {false};
    bool selection_set {false};
    std::optional<PresetSelection> target_selection;
    std::optional<SlotRemap> slot_remap;
    bool project_overrides_set {false};
    std::set<std::uint64_t> plate_overrides_set;
    std::set<std::uint64_t> object_overrides_set;
    std::set<std::uint64_t> part_overrides_set;
    std::set<std::uint64_t> layer_ranges_set;
    bool project_map_set {false};
    std::set<std::uint64_t> local_map_set;
};

struct ProjectBuilder::State {
    explicit State(std::shared_ptr<detail::ContextState> context_value)
        : context(std::move(context_value)), owner(std::this_thread::get_id()) {}
    std::shared_ptr<detail::ContextState> context;
    std::thread::id owner;
    bool terminal {false};
    std::shared_ptr<const detail::ProjectIdentity> identity;
    std::optional<PresetSelection> selection;
    std::optional<FilamentMapOverride> project_filament_map;
    ConfigPatch project_overrides;
    Slic3r::DynamicPrintConfig project_config;
    Slic3r::Model model;
    std::vector<Slic3r::PlateData> plates;
    std::uint64_t triangle_count {0};
};

struct EffectiveConfiguration::State {
    ConfigSchema schema;
    ConfigValues values;
    Provenance provenance;
    Slic3r::DynamicPrintConfig core;
};

} // namespace libslicer::v1
