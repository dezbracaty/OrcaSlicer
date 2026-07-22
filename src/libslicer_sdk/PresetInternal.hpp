#pragma once

#include <libslicer/v1/Preset.hpp>
#include <libslicer/v1/Context.hpp>

#include "libslic3r/PresetBundle.hpp"

#include <map>
#include <mutex>
#include <thread>

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
    std::map<PresetKey, PresetRecord>   records;
    std::shared_ptr<Slic3r::PresetBundle> bundle;
    std::filesystem::path               user_store;
    std::uint64_t                       storage_generation {0};
    std::string                         storage_fingerprint;
};

Result<std::shared_ptr<PresetCatalogState>> load_preset_catalog(const ContextOptions &options,
                                                                 ConfigSchema schema);
Result<std::uint64_t> persist_user_preset(PresetCatalogState &catalog,
                                          const PresetRecord &record,
                                          std::uint64_t expected_storage_generation,
                                          const std::string &expected_storage_fingerprint);
Result<std::uint64_t> erase_user_preset(PresetCatalogState &catalog,
                                        const PresetKey &key,
                                        std::uint64_t expected_storage_generation,
                                        const std::string &expected_storage_fingerprint);

} // namespace libslicer::v1::detail

namespace libslicer::v1 {

struct PresetRepository::State {
    explicit State(std::shared_ptr<detail::PresetCatalogState> value) : catalog(std::move(value)) {}
    std::shared_ptr<detail::PresetCatalogState> catalog;
};

} // namespace libslicer::v1
