#pragma once

#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Preset.hpp>

#include "libslic3r/PresetBundle.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace libslicer::v1 {

struct PresetRef::Binding {
    PresetKind   kind;
    PresetOrigin origin;
    std::string  id;
};

} // namespace libslicer::v1

namespace libslicer::v1::detail {

struct PresetRefAccess {
    static PresetRef project_embedded(PresetKind kind, std::string id)
    {
        return PresetRef(std::make_shared<const PresetRef::Binding>(
            PresetRef::Binding{kind, PresetOrigin::project_embedded, std::move(id)}));
    }
};

struct PresetKey {
    PresetKind   kind;
    PresetOrigin origin;
    std::string  id;

    friend bool operator<(const PresetKey &lhs, const PresetKey &rhs) noexcept
    {
        if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
        if (lhs.origin != rhs.origin) return lhs.origin < rhs.origin;
        return lhs.id < rhs.id;
    }
};

struct PresetRecord {
    PresetSummary                     summary;
    ConfigValues                      inherited;
    ConfigValues                      effective;
    ConfigPatch                       overrides;
    std::shared_ptr<Slic3r::Preset>   core;
};

struct PresetCatalogState {
    explicit PresetCatalogState(ConfigSchema value) : schema(std::move(value)) {}

    mutable std::mutex                  mutex;
    ConfigSchema                        schema;
    std::uint64_t                       generation {1};
    PresetRevision                     next_revision {1};
    std::shared_ptr<Slic3r::PresetBundle> bundle;
    std::map<PresetKey, PresetRecord>   records;
    std::filesystem::path               user_store;
    std::uint64_t                       storage_generation {0};
    std::string                         storage_fingerprint;
};

// The caller must hold detail::runtime_mutex(); catalog construction touches
// libslic3r process-global config caches and resource/data/temporary paths.
Result<std::shared_ptr<PresetCatalogState>> load_preset_catalog_with_core_locked(
    const ContextOptions &options, ConfigSchema schema);
Result<std::uint64_t> persist_user_preset(PresetCatalogState &catalog,
                                          const PresetRecord &record,
                                          std::uint64_t expected_storage_generation,
                                          const std::string &expected_storage_fingerprint);
Result<std::uint64_t> erase_user_preset(PresetCatalogState &catalog,
                                        const PresetKey &key,
                                        std::uint64_t expected_storage_generation,
                                        const std::string &expected_storage_fingerprint);

#ifdef LIBSLICER_SDK_TESTING

enum class RecordFailureInjectionStage {
    freeze,
    parent_default,
    identity,
    duplicate,
    effective_conversion
};

struct RecordFailureInjection {
    std::size_t ordinal;
    RecordFailureInjectionStage stage;
};

struct RecordSchedulerTestControl {
    std::size_t worker_limit {4};
    std::vector<RecordFailureInjection> failures;
};

struct RecordSchedulerTestStats {
    std::size_t configured_worker_limit {0};
    std::size_t arena_max_concurrency {0};
    std::size_t maximum_active_workers {0};
    std::size_t dependency_batches_started {0};
    std::size_t dependency_batches_joined {0};
    std::size_t effective_artifacts_attempted {0};
    std::size_t candidates_attempted {0};
    std::size_t candidates_staged {0};
    std::size_t committed_records {0};
    std::size_t default_conversions {0};
    std::size_t parent_effective_shares {0};
    std::size_t default_value_shares {0};
    std::size_t children_completed_from_failed_parent_record {0};
    std::size_t children_blocked_by_parent_effective_failure {0};
    std::size_t worker_lazy_helper_calls {0};
    std::size_t worker_catalog_mutations {0};
    std::size_t worker_core_mutations {0};
    std::size_t worker_next_revision_writes {0};
    std::size_t in_flight_candidates {0};
    std::size_t peak_in_flight_candidates {0};
    std::size_t status_candidate_owners_after_commit {0};
    std::size_t artifact_value_owners_after_commit {0};
    std::uint64_t system_loader_ns {0};
    std::uint64_t freeze_ns {0};
    std::uint64_t default_batch_ns {0};
    std::uint64_t effective_batch_ns {0};
    std::uint64_t candidate_batch_ns {0};
    std::uint64_t commit_ns {0};
    std::uint64_t user_store_ns {0};
    std::optional<std::size_t> primary_failure_ordinal;
    std::optional<RecordFailureInjectionStage> primary_failure_stage;
    std::optional<std::size_t> reverse_dependency_child_ordinal;
    std::optional<std::size_t> reverse_dependency_parent_ordinal;
    bool system_loader_quiescent_before_arena {false};
    bool arena_destroyed_before_commit {false};
};

void set_record_scheduler_test_control(RecordSchedulerTestControl control);
void reset_record_scheduler_test_control();
RecordSchedulerTestStats record_scheduler_test_stats();
std::uintptr_t config_values_identity_for_testing(const ConfigValues &values);

#endif

} // namespace libslicer::v1::detail

namespace libslicer::v1 {

struct PresetRepository::State {
    State(std::shared_ptr<detail::ContextState> context_value,
          std::shared_ptr<detail::PresetCatalogState> catalog_value)
        : context(std::move(context_value)), catalog(std::move(catalog_value)) {}
    std::shared_ptr<detail::ContextState> context;
    std::shared_ptr<detail::PresetCatalogState> catalog;
};

} // namespace libslicer::v1
