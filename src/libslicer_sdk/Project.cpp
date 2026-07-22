#include <libslicer/v1/Project.hpp>

#include "ConfigSchemaInternal.hpp"
#include "OrcaConfigAdapter.hpp"
#include "ProjectInternal.hpp"
#include "RuntimeCoordinator.hpp"

#include "libslic3r/Utils.hpp"
#include "miniz.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace libslicer::v1 {

#define LIBSLICER_DEFINE_PROJECT_ID(Name)                                            \
struct Name::Binding {                                                               \
    std::shared_ptr<const detail::ProjectIdentity> project;                           \
    std::uint64_t value;                                                              \
};                                                                                    \
Name::Name(std::shared_ptr<const Binding> binding) : binding_(std::move(binding)) {}  \
std::uint64_t Name::value() const noexcept { return binding_->value; }                 \
bool operator==(const Name &lhs, const Name &rhs) noexcept                             \
{ return lhs.binding_->project == rhs.binding_->project && lhs.binding_->value == rhs.binding_->value; }

LIBSLICER_DEFINE_PROJECT_ID(PlateId)
LIBSLICER_DEFINE_PROJECT_ID(ObjectId)
LIBSLICER_DEFINE_PROJECT_ID(PartId)
LIBSLICER_DEFINE_PROJECT_ID(InstanceId)
#undef LIBSLICER_DEFINE_PROJECT_ID

namespace detail {

PlateId ProjectIdAccess::plate(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value)
{
    return PlateId(std::make_shared<const PlateId::Binding>(PlateId::Binding{std::move(project), value}));
}
ObjectId ProjectIdAccess::object(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value)
{
    return ObjectId(std::make_shared<const ObjectId::Binding>(ObjectId::Binding{std::move(project), value}));
}
PartId ProjectIdAccess::part(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value)
{
    return PartId(std::make_shared<const PartId::Binding>(PartId::Binding{std::move(project), value}));
}
InstanceId ProjectIdAccess::instance(std::shared_ptr<const ProjectIdentity> project, std::uint64_t value)
{
    return InstanceId(std::make_shared<const InstanceId::Binding>(InstanceId::Binding{std::move(project), value}));
}
bool ProjectIdAccess::belongs(const PlateId &id, const std::shared_ptr<const ProjectIdentity> &project)
{ return id.binding_->project == project; }
bool ProjectIdAccess::belongs(const ObjectId &id, const std::shared_ptr<const ProjectIdentity> &project)
{ return id.binding_->project == project; }
bool ProjectIdAccess::belongs(const PartId &id, const std::shared_ptr<const ProjectIdentity> &project)
{ return id.binding_->project == project; }
bool ProjectIdAccess::belongs(const InstanceId &id, const std::shared_ptr<const ProjectIdentity> &project)
{ return id.binding_->project == project; }

const std::shared_ptr<const ProjectData> &ProjectSnapshotAccess::data(
    const ProjectSnapshot &snapshot)
{
    return snapshot.state_->data;
}

const std::shared_ptr<ContextState> &ProjectSnapshotAccess::context(
    const ProjectSnapshot &snapshot)
{
    return snapshot.state_->context;
}

EffectiveConfiguration EffectiveConfigurationAccess::make(
    ConfigSchema schema, ConfigValues values, EffectiveConfiguration::Provenance provenance,
    Slic3r::DynamicPrintConfig core)
{
    return EffectiveConfiguration(std::make_shared<const EffectiveConfiguration::State>(
        EffectiveConfiguration::State{std::move(schema), std::move(values),
                                      std::move(provenance), std::move(core)}));
}

const Slic3r::DynamicPrintConfig &EffectiveConfigurationAccess::core(
    const EffectiveConfiguration &configuration)
{
    return configuration.state_->core;
}

Result<ConfigPatch> config_to_generic_patch(const Slic3r::ConfigBase &config,
                                            const ConfigSchema &schema)
{
    const Slic3r::DynamicPrintConfig empty;
    auto converted = core_config_diff_to_patch(empty, config, schema);
    if (!converted.has_value()) return converted;
    ConfigPatch filtered;
    for (const ConfigEntry &entry : converted.value().entries()) {
        const auto descriptor = schema.find(entry.option);
        if (descriptor && detail::ConfigSchemaAccess::is_generic_option(schema, entry.option))
            filtered.set(entry.option, entry.value);
    }
    return ResultAccess::success(std::move(filtered));
}

} // namespace detail

namespace {

template<class T>
Result<T> failure(ErrorCode code, std::string message, std::string field)
{
    return detail::ResultAccess::failure<T>(code, std::move(message), std::move(field));
}

ConfigValues merge_values(const ConfigValues &base, const ConfigPatch &patch)
{
    std::vector<ConfigEntry> entries = base.entries();
    for (const auto &entry : patch.entries()) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&](const ConfigEntry &value) {
            return value.option == entry.option;
        });
        if (found == entries.end()) entries.push_back(entry);
        else found->value = entry.value;
    }
    return detail::ConfigValuesAccess::make(std::move(entries));
}

std::string embedded_key(PresetKind kind, const std::string &id)
{
    return std::to_string(static_cast<int>(kind)) + ":" + id;
}

bool same_project(const PlateId &id, const detail::ProjectData &data)
{
    return detail::ProjectIdAccess::belongs(id, data.identity);
}
bool same_project(const ObjectId &id, const detail::ProjectData &data)
{
    return detail::ProjectIdAccess::belongs(id, data.identity);
}
bool same_project(const PartId &id, const detail::ProjectData &data)
{
    return detail::ProjectIdAccess::belongs(id, data.identity);
}
bool same_project(const InstanceId &id, const detail::ProjectData &data)
{
    return detail::ProjectIdAccess::belongs(id, data.identity);
}

std::shared_ptr<const detail::ProjectIdentity> new_project_identity()
{
    static std::atomic<std::uint64_t> next_identity{1};
    return std::make_shared<const detail::ProjectIdentity>(
        detail::ProjectIdentity{next_identity.fetch_add(1)});
}

const Slic3r::ModelObject *find_object(const detail::ProjectData &data, ObjectId id)
{
    if (!same_project(id, data)) return nullptr;
    const auto found = std::find_if(data.model.objects.begin(), data.model.objects.end(),
        [id](const Slic3r::ModelObject *object) { return object->id().id == id.value(); });
    return found == data.model.objects.end() ? nullptr : *found;
}

Slic3r::ModelObject *find_object(detail::ProjectData &data, ObjectId id)
{
    return const_cast<Slic3r::ModelObject *>(find_object(
        static_cast<const detail::ProjectData &>(data), id));
}

std::optional<std::size_t> object_position(const Slic3r::Model &model, ObjectId id,
                                           const std::shared_ptr<const detail::ProjectIdentity> &identity)
{
    if (!detail::ProjectIdAccess::belongs(id, identity)) return std::nullopt;
    for (std::size_t index = 0; index < model.objects.size(); ++index)
        if (model.objects[index]->id().id == id.value()) return index;
    return std::nullopt;
}

Slic3r::ModelVolume *find_part(detail::ProjectData &data, PartId id)
{
    if (!same_project(id, data)) return nullptr;
    for (Slic3r::ModelObject *object : data.model.objects)
        for (Slic3r::ModelVolume *volume : object->volumes)
            if (volume->id().id == id.value()) return volume;
    return nullptr;
}

std::optional<std::size_t> plate_position(const detail::ProjectData &data, PlateId id)
{
    if (!same_project(id, data) || id.value() == 0) return std::nullopt;
    for (std::size_t index = 0; index < data.plates.size(); ++index)
        if (static_cast<std::uint64_t>(data.plates[index].plate_index + 1) == id.value()) return index;
    return std::nullopt;
}

FilamentMapMode public_map_mode(const Slic3r::DynamicPrintConfig &config)
{
    const auto *option = config.option<Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>>(
        "filament_map_mode");
    if (!option || option->value == Slic3r::fmmManual) return FilamentMapMode::manual;
    if (option->value == Slic3r::fmmAutoForMatch) return FilamentMapMode::auto_for_match;
    return FilamentMapMode::auto_for_flush;
}

Slic3r::FilamentMapMode core_map_mode(FilamentMapMode mode)
{
    if (mode == FilamentMapMode::manual) return Slic3r::fmmManual;
    if (mode == FilamentMapMode::auto_for_match) return Slic3r::fmmAutoForMatch;
    return Slic3r::fmmAutoForFlush;
}

FilamentMapOverride project_map(const detail::ProjectData &data)
{
    std::vector<ToolId> tools;
    if (const auto *option = data.project_config.option<Slic3r::ConfigOptionInts>("filament_map")) {
        tools.reserve(option->values.size());
        for (int tool : option->values) tools.push_back({static_cast<std::uint32_t>(tool)});
    }
    return {public_map_mode(data.project_config), std::move(tools)};
}

Result<void> validate_map(const FilamentMapOverride &map, std::size_t slots,
                          const std::string &field)
{
    if (map.tools.size() != slots)
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Filament map must cover every logical slot", field + "/tools");
    for (std::size_t index = 0; index < map.tools.size(); ++index)
        if (map.tools[index].value == 0)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "ToolId is one-based and must be non-zero",
                                                 field + "/tools/" + std::to_string(index));
    return detail::ResultAccess::success();
}

Result<void> validate_map_values(const FilamentMapOverride &map, const std::string &field)
{
    for (std::size_t index = 0; index < map.tools.size(); ++index)
        if (map.tools[index].value == 0)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                "ToolId is one-based and must be non-zero",
                field + "/tools/" + std::to_string(index));
    return detail::ResultAccess::success();
}

void apply_project_map(detail::ProjectData &data, const FilamentMapOverride &map)
{
    std::vector<int> tools;
    tools.reserve(map.tools.size());
    for (ToolId tool : map.tools) tools.push_back(static_cast<int>(tool.value));
    data.project_config.set_key_value("filament_map", new Slic3r::ConfigOptionInts(std::move(tools)));
    data.project_config.set_key_value("filament_map_mode",
        new Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>(core_map_mode(map.mode)));
}

void apply_project_map(Slic3r::DynamicPrintConfig &config, const FilamentMapOverride &map)
{
    std::vector<int> tools;
    tools.reserve(map.tools.size());
    for (ToolId tool : map.tools) tools.push_back(static_cast<int>(tool.value));
    config.set_key_value("filament_map", new Slic3r::ConfigOptionInts(std::move(tools)));
    config.set_key_value("filament_map_mode",
        new Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>(core_map_mode(map.mode)));
}

Result<void> validate_patch(const ConfigSchema &schema, const ConfigPatch &patch, OptionScope scope)
{
    ConfigValidationContext context{scope, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
    return detail::ConfigSchemaAccess::validate_structure(schema, patch, context);
}

Result<void> replace_generic_config(Slic3r::DynamicPrintConfig &config,
                                    const ConfigSchema &schema,
                                    const ConfigPatch &patch)
{
    const std::vector<std::string> keys = config.keys();
    for (const std::string &key : keys) {
        const auto descriptor = schema.find(OptionId(key));
        if (descriptor && detail::ConfigSchemaAccess::is_generic_option(schema, OptionId(key)))
            config.erase(key);
    }
    return detail::apply_patch_to_core_config(patch, config);
}

bool remap_is_identity(const SlotRemap &remap, std::size_t new_count)
{
    if (remap.old_to_new.size() != new_count) return false;
    for (std::size_t index = 0; index < remap.old_to_new.size(); ++index)
        if (!remap.old_to_new[index] || remap.old_to_new[index]->value != index) return false;
    return true;
}

Result<ConfigValue> remap_reference_value(const ConfigValue &value,
                                          FilamentReferenceKind kind,
                                          const SlotRemap &remap,
                                          const std::string &field)
{
    if (value.is_null()) return detail::ResultAccess::success(value);
    const auto remap_scalar = [&](const ConfigValue &item,
                                  const std::string &item_field) -> Result<ConfigValue> {
        if (item.is_null()) return detail::ResultAccess::success(item);
        const auto old_slot = item.as_integer();
        if (!old_slot || *old_slot < 0 || static_cast<std::size_t>(*old_slot) >= remap.old_to_new.size())
            return failure<ConfigValue>(ErrorCode::invalid_configuration,
                                        "Filament reference is outside the old selection", item_field);
        const auto target = remap.old_to_new[static_cast<std::size_t>(*old_slot)];
        if (!target)
            return failure<ConfigValue>(ErrorCode::invalid_configuration,
                                        "Deleted filament slot is still referenced", item_field);
        return detail::ResultAccess::success(
            ConfigValue::integer(static_cast<std::int64_t>(target->value)));
    };
    if (kind == FilamentReferenceKind::logical_slot) return remap_scalar(value, field);
    if (kind != FilamentReferenceKind::logical_slot_list)
        return detail::ResultAccess::success(value);
    const auto items = value.as_list();
    const auto item_shape = value.shape().item_shape();
    if (!items || !item_shape)
        return failure<ConfigValue>(ErrorCode::invalid_configuration,
                                    "Filament reference list has an invalid shape", field);
    std::vector<ConfigValue> migrated;
    migrated.reserve(items->size());
    for (std::size_t index = 0; index < items->size(); ++index) {
        auto item = remap_scalar((*items)[index], field + "/" + std::to_string(index));
        if (!item.has_value()) return item;
        migrated.push_back(std::move(item).value());
    }
    return ConfigValue::list(*item_shape, std::move(migrated));
}

Result<ConfigPatch> remap_patch(const ConfigPatch &original,
                                const ConfigSchema &schema,
                                const SlotRemap &remap,
                                const std::string &field,
                                const std::set<std::string> *skip = nullptr)
{
    ConfigPatch migrated;
    for (const ConfigEntry &entry : original.entries()) {
        if (skip && skip->count(entry.option.value())) continue;
        const auto descriptor = schema.find(entry.option);
        if (!descriptor || descriptor->filament_reference == FilamentReferenceKind::none) {
            migrated.set(entry.option, entry.value);
            continue;
        }
        auto value = remap_reference_value(entry.value, descriptor->filament_reference, remap,
                                           field + "/" + entry.option.value());
        if (!value.has_value())
            return failure<ConfigPatch>(*value.error_code(), value.diagnostics().front().message,
                                        value.diagnostics().front().field);
        migrated.set(entry.option, std::move(value).value());
    }
    return detail::ResultAccess::success(std::move(migrated));
}

Result<void> validate_patch_for_commit(const ConfigSchema &schema, const ConfigPatch &patch,
                                       OptionScope scope, std::optional<PresetKind> preset_kind,
                                       std::size_t slots, const std::string &field)
{
    ConfigValidationContext context{scope, preset_kind, std::nullopt, std::nullopt, slots};
    auto valid = detail::ConfigSchemaAccess::validate_structure(schema, patch, context);
    if (valid.has_value()) return valid;
    const auto &diagnostic = valid.diagnostics().front();
    const std::string nested = diagnostic.field.empty() ? field : field + diagnostic.field;
    return detail::ResultAccess::failure(ErrorCode::invalid_configuration,
                                         diagnostic.message, nested);
}

void migrate_map(const std::vector<int> &original, const SlotRemap &remap,
                 std::size_t new_count, std::vector<int> &output)
{
    output.assign(new_count, 0);
    const std::size_t count = std::min(original.size(), remap.old_to_new.size());
    for (std::size_t old_slot = 0; old_slot < count; ++old_slot)
        if (remap.old_to_new[old_slot])
            output[remap.old_to_new[old_slot]->value] = original[old_slot];
}

template<class EditorState>
Result<void> check_editor(const EditorState &state)
{
    if (state.owner != std::this_thread::get_id())
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "ProjectEdit must be used from its creating thread", "/edit");
    if (state.terminal)
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "ProjectEdit is already closed", "/edit");
    return detail::ResultAccess::success();
}

PresetKind preset_kind(Slic3r::Preset::Type type)
{
    if (type == Slic3r::Preset::TYPE_PRINTER) return PresetKind::printer;
    if (type == Slic3r::Preset::TYPE_FILAMENT) return PresetKind::filament;
    return PresetKind::process;
}

Slic3r::Preset::Type preset_type(PresetKind kind)
{
    if (kind == PresetKind::printer) return Slic3r::Preset::TYPE_PRINTER;
    if (kind == PresetKind::filament) return Slic3r::Preset::TYPE_FILAMENT;
    return Slic3r::Preset::TYPE_PRINT;
}

const detail::PresetRecord *catalog_by_name(const detail::PresetCatalogState &catalog,
                                             PresetKind kind, const std::string &name)
{
    for (PresetOrigin origin : {PresetOrigin::user, PresetOrigin::vendor,
                                PresetOrigin::system}) {
        const auto exact = catalog.records.find(detail::PresetKey{kind, origin, name});
        if (exact != catalog.records.end()) return &exact->second;
        std::string alias = name;
        std::set<std::string> visited;
        while (visited.insert(alias).second) {
            const detail::PresetRecord *matched = nullptr;
            for (const auto &[key, record] : catalog.records) {
                if (key.kind != kind || key.origin != origin || !record.core) continue;
                if (std::find(record.core->renamed_from.begin(), record.core->renamed_from.end(),
                              alias) == record.core->renamed_from.end())
                    continue;
                if (matched) return nullptr;
                matched = &record;
            }
            if (!matched) break;
            const auto current = catalog.records.find(detail::PresetKey{
                kind, origin, matched->summary.ref.id()});
            if (current != catalog.records.end()) return &current->second;
            alias = matched->summary.ref.id();
        }
    }
    return nullptr;
}

std::optional<std::uint64_t> archive_uncompressed_bytes(const std::filesystem::path &path)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, path.string().c_str(), 0)) return std::nullopt;
    std::uint64_t total = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
            mz_zip_reader_end(&archive);
            return std::nullopt;
        }
        if (std::numeric_limits<std::uint64_t>::max() - total < stat.m_uncomp_size) {
            mz_zip_reader_end(&archive);
            return std::nullopt;
        }
        total += stat.m_uncomp_size;
    }
    mz_zip_reader_end(&archive);
    return total;
}

bool archive_has_project_payload(const std::filesystem::path &path)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, path.string().c_str(), 0)) return false;
    const bool result =
        mz_zip_reader_locate_file(&archive, "Metadata/project_settings.config", nullptr, 0) >= 0 &&
        mz_zip_reader_locate_file(&archive, "Metadata/model_settings.config", nullptr, 0) >= 0 &&
        mz_zip_reader_locate_file(&archive, "3D/3dmodel.model", nullptr, 0) >= 0;
    mz_zip_reader_end(&archive);
    return result;
}

bool prepare_archive_directories(const std::filesystem::path &archive_path,
                                 const std::filesystem::path &destination)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_file(&archive, archive_path.string().c_str(), 0)) return false;
    bool valid = true;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count && valid; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) { valid = false; break; }
        std::string normalized_name = stat.m_filename;
        std::replace(normalized_name.begin(), normalized_name.end(), '\\', '/');
        const std::filesystem::path relative = std::filesystem::u8path(normalized_name);
        if (relative.is_absolute()) { valid = false; break; }
        for (const auto &component : relative)
            if (component == "..") { valid = false; break; }
        if (!valid) break;
        std::error_code error;
        std::filesystem::create_directories(destination / relative.parent_path(), error);
        if (error) valid = false;
    }
    mz_zip_reader_end(&archive);
    return valid;
}

void apply_selected_filaments(Slic3r::DynamicPrintConfig &out,
                              const Slic3r::DynamicPrintConfig &default_filament,
                              const std::vector<Slic3r::DynamicPrintConfig> &filaments)
{
    if (filaments.empty()) return;
    if (filaments.size() == 1) {
        out.apply(filaments.front());
        const auto *variants = out.option<Slic3r::ConfigOptionStrings>(
            "filament_extruder_variant");
        out.option<Slic3r::ConfigOptionInts>("filament_self_index", true)->values.assign(
            variants ? variants->values.size() : 1, 1);
        return;
    }
    for (const std::string &key : default_filament.keys()) {
        if (key == "compatible_prints" || key == "compatible_printers") continue;
        Slic3r::ConfigOption *destination = out.option(key, true);
        if (!destination) continue;
        if (destination->is_scalar()) {
            if (const auto *source = filaments.front().option(key)) destination->set(source);
            continue;
        }
        auto *vector_destination = dynamic_cast<Slic3r::ConfigOptionVectorBase *>(destination);
        if (!vector_destination) continue;
        bool first = true;
        for (const auto &filament : filaments) {
            const auto *source = dynamic_cast<const Slic3r::ConfigOptionVectorBase *>(
                filament.option(key));
            if (!source) continue;
            if (first) {
                vector_destination->set(source);
                first = false;
            } else {
                vector_destination->append(source);
            }
        }
    }
    std::vector<int> self_indices;
    for (std::size_t index = 0; index < filaments.size(); ++index) {
        const auto *variants = filaments[index].option<Slic3r::ConfigOptionStrings>(
            "filament_extruder_variant");
        self_indices.insert(self_indices.end(), variants ? variants->values.size() : 1,
                            static_cast<int>(index + 1));
    }
    out.option<Slic3r::ConfigOptionInts>("filament_self_index", true)->values =
        std::move(self_indices);
}

std::size_t physical_tool_count(const Slic3r::DynamicPrintConfig &config)
{
    const auto *diameters = config.option<Slic3r::ConfigOptionFloats>("nozzle_diameter");
    return diameters ? diameters->values.size() : 0;
}

bool publish_file_atomically(const std::filesystem::path &temporary,
                             const std::filesystem::path &destination,
                             std::error_code &error)
{
#ifdef _WIN32
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error.clear();
        return true;
    }
    error = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

} // namespace

ProjectSnapshot::ProjectSnapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
ProjectRevision ProjectSnapshot::revision() const { return state_->data->revision; }
ConfigPatch ProjectSnapshot::project_overrides() const { return state_->data->project_overrides; }

std::vector<PlateInfo> ProjectSnapshot::plates() const
{
    std::vector<PlateInfo> result;
    result.reserve(state_->data->plates.size());
    for (const auto &plate : state_->data->plates) {
        auto patch = detail::config_to_generic_patch(plate.config, state_->data->schema);
        result.push_back({detail::ProjectIdAccess::plate(state_->data->identity,
                              static_cast<std::uint64_t>(plate.plate_index + 1)),
                          plate.plate_name, patch.has_value() ? std::move(patch).value() : ConfigPatch{},
                          plate.locked});
    }
    return result;
}

std::vector<ObjectInfo> ProjectSnapshot::objects() const
{
    std::vector<ObjectInfo> result;
    result.reserve(state_->data->model.objects.size());
    for (const auto *object : state_->data->model.objects) {
        auto patch = detail::config_to_generic_patch(object->config.get(), state_->data->schema);
        result.push_back({detail::ProjectIdAccess::object(state_->data->identity, object->id().id),
                          object->name, patch.has_value() ? std::move(patch).value() : ConfigPatch{}});
    }
    return result;
}

std::vector<PartInfo> ProjectSnapshot::parts() const
{
    std::vector<PartInfo> result;
    for (const auto *object : state_->data->model.objects) {
        const ObjectId object_id = detail::ProjectIdAccess::object(state_->data->identity, object->id().id);
        for (const auto *volume : object->volumes) {
            auto patch = detail::config_to_generic_patch(volume->config.get(), state_->data->schema);
            result.push_back({detail::ProjectIdAccess::part(state_->data->identity, volume->id().id),
                              object_id, patch.has_value() ? std::move(patch).value() : ConfigPatch{}});
        }
    }
    return result;
}

Result<std::vector<LayerRange>> ProjectSnapshot::layer_ranges(ObjectId id) const
{
    const auto *object = find_object(*state_->data, id);
    if (!object) return failure<std::vector<LayerRange>>(ErrorCode::not_found,
                                                         "Object was not found", "/entity");
    std::vector<LayerRange> result;
    result.reserve(object->layer_config_ranges.size());
    for (const auto &[range, config] : object->layer_config_ranges) {
        auto patch = detail::config_to_generic_patch(config.get(), state_->data->schema);
        if (!patch.has_value()) return failure<std::vector<LayerRange>>(
            *patch.error_code(), patch.diagnostics().front().message,
            patch.diagnostics().front().field);
        result.push_back({range.first, range.second, std::move(patch).value()});
    }
    return detail::ResultAccess::success(std::move(result));
}

Result<std::vector<InstanceId>> ProjectSnapshot::instances(PlateId id) const
{
    const auto position = plate_position(*state_->data, id);
    if (!position) return failure<std::vector<InstanceId>>(
        ErrorCode::not_found, "Plate was not found", "/entity");
    std::vector<InstanceId> result;
    const auto &plate = state_->data->plates[*position];
    result.reserve(plate.objects_and_instances.size());
    for (const auto &[object_index, instance_index] : plate.objects_and_instances) {
        if (object_index < 0 || instance_index < 0) continue;
        const std::size_t object_pos = static_cast<std::size_t>(object_index);
        const std::size_t instance_pos = static_cast<std::size_t>(instance_index);
        if (object_pos >= state_->data->model.objects.size()) continue;
        const auto *object = state_->data->model.objects[object_pos];
        if (instance_pos >= object->instances.size()) continue;
        result.push_back(detail::ProjectIdAccess::instance(
            state_->data->identity, object->instances[instance_pos]->id().id));
    }
    return detail::ResultAccess::success(std::move(result));
}

PresetSelection ProjectSnapshot::project_selected_presets() const { return state_->data->selection; }

FilamentMapOverride ProjectSnapshot::project_filament_map() const { return project_map(*state_->data); }

Result<std::optional<FilamentMapOverride>> ProjectSnapshot::local_filament_map_override(PlateId id) const
{
    const auto position = plate_position(*state_->data, id);
    if (!position) return failure<std::optional<FilamentMapOverride>>(
        ErrorCode::not_found, "Plate was not found", "/entity");
    const auto &plate = state_->data->plates[*position];
    if (plate.filament_maps.empty())
        return detail::ResultAccess::success(std::optional<FilamentMapOverride>{});
    std::vector<ToolId> tools;
    for (int tool : plate.filament_maps) tools.push_back({static_cast<std::uint32_t>(tool)});
    const FilamentMapMode mode = plate.config.has("filament_map_mode")
        ? public_map_mode(plate.config) : project_map(*state_->data).mode;
    return detail::ResultAccess::success(
        std::optional<FilamentMapOverride>{{mode, std::move(tools)}});
}

Result<EffectiveFilamentMap> ProjectSnapshot::effective_filament_map(PlateId id) const
{
    auto local = local_filament_map_override(id);
    if (!local.has_value())
        return failure<EffectiveFilamentMap>(*local.error_code(),
                                             local.diagnostics().front().message,
                                             local.diagnostics().front().field);
    if (local.value())
        return detail::ResultAccess::success(EffectiveFilamentMap{
            local.value()->mode, local.value()->tools, EffectiveFilamentMap::Source::plate});
    auto map = project_map(*state_->data);
    return detail::ResultAccess::success(EffectiveFilamentMap{
        map.mode, std::move(map.tools), EffectiveFilamentMap::Source::project});
}

Project::Project(std::shared_ptr<State> state) : state_(std::move(state)) {}

Result<Project> Project::load(SdkContext &context, const std::filesystem::path &path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".3mf")
        return failure<Project>(ErrorCode::unsupported,
                                "v1 only loads Orca-compatible 3MF projects", "/path");
    std::error_code error;
    const auto input_bytes = std::filesystem::file_size(path, error);
    if (error) return failure<Project>(ErrorCode::io, "Unable to read project file", "/path");
    auto context_state = detail::ContextAccess::state(context);
    if (input_bytes > context_state->options.limits.project_input_bytes)
        return failure<Project>(ErrorCode::resource_limit_exceeded,
                                "Project input byte limit exceeded", "/limits/project_input_bytes");
    const auto uncompressed = archive_uncompressed_bytes(path);
    if (!uncompressed)
        return failure<Project>(ErrorCode::unsupported, "Project is not a readable 3MF archive", "/path");
    if (!archive_has_project_payload(path))
        return failure<Project>(ErrorCode::unsupported,
                                "3MF is not an Orca/Bambu project payload", "/path");
    if (*uncompressed > context_state->options.limits.project_uncompressed_bytes)
        return failure<Project>(ErrorCode::resource_limit_exceeded,
                                "Project uncompressed byte limit exceeded",
                                "/limits/project_uncompressed_bytes");

    auto repository = context.presets();
    if (!repository.has_value())
        return failure<Project>(*repository.error_code(),
                                repository.diagnostics().front().message,
                                repository.diagnostics().front().field);
    auto catalog = context_state->preset_catalog;

    Slic3r::DynamicPrintConfig project_config;
    Slic3r::ConfigSubstitutionContext substitutions{
        Slic3r::ForwardCompatibilitySubstitutionRule::Disable};
    Slic3r::Model model;
    Slic3r::PlateDataPtrs raw_plates;
    std::vector<Slic3r::Preset *> raw_presets;
    struct RawCleanup {
        Slic3r::PlateDataPtrs &plates;
        std::vector<Slic3r::Preset *> &presets;
        ~RawCleanup() { Slic3r::release_PlateData_list(plates); for (auto *preset : presets) delete preset; }
    } cleanup{raw_plates, raw_presets};
    bool is_bbl = false, is_orca = false;
    Slic3r::Semver file_version;
    std::shared_ptr<detail::ProjectTemporaryFiles> project_temporary_files;

    std::lock_guard<std::mutex> runtime(detail::runtime_mutex());
    const std::string old_resources = Slic3r::resources_dir();
    const std::string old_data = Slic3r::data_dir();
    const std::string old_temporary = Slic3r::temporary_dir();
    struct RestorePaths {
        std::string resources, data, temporary;
        ~RestorePaths() { Slic3r::set_resources_dir(resources); Slic3r::set_data_dir(data); Slic3r::set_temporary_dir(temporary); }
    } restore{old_resources, old_data, old_temporary};
    try {
        Slic3r::set_resources_dir(context_state->options.resources_dir.string());
        Slic3r::set_data_dir(context_state->options.data_dir.string());
        Slic3r::set_temporary_dir(context_state->options.temporary_dir.string());
        static std::atomic<std::uint64_t> next_load{1};
        const auto extraction = context_state->options.temporary_dir /
            ("project-load-" + std::to_string(
                 std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(next_load.fetch_add(1)));
        project_temporary_files =
            std::make_shared<detail::ProjectTemporaryFiles>(extraction);
        std::filesystem::create_directories(extraction, error);
        if (error || !prepare_archive_directories(path, extraction))
            return failure<Project>(ErrorCode::invalid_configuration,
                                    "3MF archive contains an invalid extraction path", "/path");
        model.set_backup_path(extraction.string());
        const auto strategy = Slic3r::LoadStrategy::LoadModel | Slic3r::LoadStrategy::LoadConfig |
                              Slic3r::LoadStrategy::Silence;
        if (!Slic3r::load_bbs_3mf(path.string().c_str(), &project_config, &substitutions,
                                  &model, &raw_plates, &raw_presets, &is_bbl, &is_orca,
                                  &file_version, nullptr, strategy))
            return failure<Project>(ErrorCode::invalid_configuration,
                                    "Failed to load 3MF project", "/path");
        model.set_backup_path("detach");
    } catch (const std::exception &exception) {
        return failure<Project>(ErrorCode::invalid_configuration, exception.what(), "/path");
    }
    if (!is_bbl && !is_orca)
        return failure<Project>(ErrorCode::unsupported,
                                "3MF archive is not an Orca/Bambu project", "/path");
    std::size_t instance_count = 0;
    for (const Slic3r::ModelObject *object : model.objects)
        instance_count += object->instances.size();
    if (model.objects.empty() || instance_count == 0 || raw_plates.empty())
        return failure<Project>(ErrorCode::unsupported,
                                "3MF does not contain a complete Orca/Bambu project", "/path");
    std::set<int> plate_indices;
    std::set<std::pair<int, int>> assigned_instances;
    for (Slic3r::PlateData *plate : raw_plates) {
        if (!plate || plate->plate_index < 0 || !plate_indices.insert(plate->plate_index).second)
            return failure<Project>(ErrorCode::invalid_configuration,
                                    "Project contains invalid plate identities", "/plates");
        if (plate->objects_and_instances.empty()) {
            for (const auto &[object_file_id, instance_and_identity] : plate->obj_inst_map) {
                (void) object_file_id;
                const std::size_t identity = static_cast<std::size_t>(instance_and_identity.second);
                if (identity == 0) continue;
                bool matched = false;
                for (std::size_t object_index = 0; object_index < model.objects.size() && !matched;
                     ++object_index) {
                    const auto *object = model.objects[object_index];
                    for (std::size_t instance_index = 0; instance_index < object->instances.size();
                         ++instance_index)
                        if (object->instances[instance_index]->loaded_id == identity) {
                            plate->objects_and_instances.emplace_back(
                                static_cast<int>(object_index), static_cast<int>(instance_index));
                            matched = true;
                            break;
                        }
                }
            }
        }
        for (const auto &item : plate->objects_and_instances) {
            if (item.first < 0 || item.second < 0 ||
                static_cast<std::size_t>(item.first) >= model.objects.size() ||
                static_cast<std::size_t>(item.second) >=
                    model.objects[static_cast<std::size_t>(item.first)]->instances.size() ||
                !assigned_instances.insert(item).second)
                return failure<Project>(ErrorCode::invalid_configuration,
                                        "Project contains invalid plate instance bindings", "/plates");
        }
    }
    if (raw_plates.size() == 1 && assigned_instances.size() != instance_count) {
        for (std::size_t object_index = 0; object_index < model.objects.size(); ++object_index)
            for (std::size_t instance_index = 0;
                 instance_index < model.objects[object_index]->instances.size(); ++instance_index) {
                const auto item = std::make_pair(static_cast<int>(object_index),
                                                 static_cast<int>(instance_index));
                if (assigned_instances.insert(item).second)
                    raw_plates.front()->objects_and_instances.push_back(item);
            }
    }
    if (assigned_instances.size() != instance_count)
        return failure<Project>(ErrorCode::invalid_configuration,
                                "Every model instance must belong to exactly one plate", "/plates");

    std::set<const Slic3r::TriangleMesh *> meshes;
    std::uint64_t triangles = 0;
    for (const auto *object : model.objects)
        for (const auto *volume : object->volumes)
            if (meshes.insert(volume->mesh_ptr().get()).second) {
                const auto count = volume->mesh().facets_count();
                if (std::numeric_limits<std::uint64_t>::max() - triangles < count)
                    return failure<Project>(ErrorCode::resource_limit_exceeded,
                                            "Model triangle count overflow", "/limits/model_triangles");
                triangles += count;
            }
    if (triangles > context_state->options.limits.model_triangles)
        return failure<Project>(ErrorCode::resource_limit_exceeded,
                                "Model triangle limit exceeded", "/limits/model_triangles");

    std::lock_guard<std::mutex> catalog_lock(catalog->mutex);
    std::map<std::string, detail::EmbeddedPresetRecord> embedded;
    std::map<std::string, const Slic3r::Preset *> embedded_sources;
    std::map<std::string, PresetRevision> embedded_revisions;
    PresetRevision next_embedded_revision = 1;
    for (const auto *preset : raw_presets) {
        if (!preset || preset->printer_technology() == Slic3r::ptSLA) continue;
        const PresetKind kind = preset_kind(preset->type);
        const std::string key = embedded_key(kind, preset->name);
        if (!embedded_sources.emplace(key, preset).second)
            return failure<Project>(ErrorCode::conflict,
                                    "Duplicate project-embedded preset identity",
                                    "/embedded_presets");
        embedded_revisions.emplace(key, next_embedded_revision++);
    }
    std::set<std::string> building;
    std::function<Result<void>(const std::string &)> build_embedded;
    build_embedded = [&](const std::string &key) -> Result<void> {
        if (embedded.count(key)) return detail::ResultAccess::success();
        if (!building.insert(key).second)
            return detail::ResultAccess::failure(ErrorCode::invalid_configuration,
                                                 "Embedded preset inheritance contains a cycle",
                                                 "/embedded_presets");
        const Slic3r::Preset *preset = embedded_sources.at(key);
        const PresetKind kind = preset_kind(preset->type);
        Slic3r::DynamicPrintConfig inherited_core;
        std::optional<SelectedPreset> parent;
        if (!preset->inherits().empty()) {
            const std::string parent_key = embedded_key(kind, preset->inherits());
            const auto embedded_parent = embedded_sources.find(parent_key);
            if (embedded_parent != embedded_sources.end()) {
                auto built = build_embedded(parent_key);
                if (!built.has_value()) return built;
                const auto &record = embedded.at(parent_key);
                inherited_core = record.core.config;
                parent = SelectedPreset{record.summary.ref, record.summary.revision};
            } else {
                const detail::PresetRecord *record = catalog_by_name(*catalog, kind,
                                                                      preset->inherits());
                if (!record || !record->core)
                    return detail::ResultAccess::failure(
                        ErrorCode::not_found, "Embedded preset parent was not found",
                        "/embedded_presets/parent");
                inherited_core = record->core->config;
                parent = SelectedPreset{record->summary.ref, record->summary.revision};
            }
        } else if (catalog->bundle) {
            if (kind == PresetKind::printer)
                inherited_core = catalog->bundle->printers.default_preset().config;
            else if (kind == PresetKind::filament)
                inherited_core = catalog->bundle->filaments.default_preset().config;
            else
                inherited_core = catalog->bundle->prints.default_preset().config;
        }
        auto inherited = detail::core_config_to_values(inherited_core);
        auto effective = detail::core_config_to_values(preset->config);
        auto overrides = detail::core_config_diff_to_patch(inherited_core, preset->config, catalog->schema);
        if (!inherited.has_value() || !effective.has_value() || !overrides.has_value())
            return detail::ResultAccess::failure(ErrorCode::invalid_configuration,
                                                 "Unable to convert embedded preset",
                                                 "/embedded_presets");
        PresetRef ref = detail::PresetRefAccess::project_embedded(kind, preset->name);
        PresetSummary summary{ref, preset->name, preset->vendor ? preset->vendor->name : std::string{},
                              parent, embedded_revisions.at(key)};
        embedded.emplace(key, detail::EmbeddedPresetRecord{
            summary, std::move(inherited).value(), std::move(effective).value(),
            std::move(overrides).value(), *preset});
        building.erase(key);
        return detail::ResultAccess::success();
    };
    for (const auto &[key, unused] : embedded_sources) {
        (void) unused;
        auto built = build_embedded(key);
        if (!built.has_value())
            return failure<Project>(*built.error_code(), built.diagnostics().front().message,
                                    built.diagnostics().front().field);
    }

    const auto string_option = [&project_config](const char *key) -> std::string {
        const auto *option = project_config.option<Slic3r::ConfigOptionString>(key);
        return option ? option->value : std::string{};
    };
    const auto select = [&](PresetKind kind, const std::string &name,
                            const std::string &field) -> Result<SelectedPreset> {
        const auto embedded_it = embedded.find(embedded_key(kind, name));
        if (embedded_it != embedded.end())
            return detail::ResultAccess::success(SelectedPreset{
                embedded_it->second.summary.ref, embedded_it->second.summary.revision});
        const auto *record = catalog_by_name(*catalog, kind, name);
        if (!record) return failure<SelectedPreset>(ErrorCode::not_found,
                                                     "Selected preset was not found", field);
        return detail::ResultAccess::success(SelectedPreset{record->summary.ref, record->summary.revision});
    };
    auto printer = select(PresetKind::printer, string_option("printer_settings_id"),
                          "/project/selected_presets/printer");
    auto process = select(PresetKind::process, string_option("print_settings_id"),
                          "/project/selected_presets/process");
    const auto *filament_ids = project_config.option<Slic3r::ConfigOptionStrings>("filament_settings_id");
    if (!printer.has_value())
        return failure<Project>(*printer.error_code(), printer.diagnostics().front().message,
                                printer.diagnostics().front().field);
    if (!process.has_value())
        return failure<Project>(*process.error_code(), process.diagnostics().front().message,
                                process.diagnostics().front().field);
    if (!filament_ids || filament_ids->values.empty())
        return failure<Project>(ErrorCode::invalid_configuration,
                                "Project selected presets are incomplete",
                                "/project/selected_presets/filaments");
    std::vector<SelectedPreset> filaments;
    for (const std::string &name : filament_ids->values) {
        auto selected = select(PresetKind::filament, name,
            "/project/selected_presets/filaments/" + std::to_string(filaments.size()));
        if (!selected.has_value())
            return failure<Project>(*selected.error_code(), selected.diagnostics().front().message,
                                    selected.diagnostics().front().field);
        filaments.push_back(std::move(selected).value());
    }
    PresetSelection selection{std::move(printer).value(), std::move(process).value(),
                              std::move(filaments)};
    const Slic3r::Preset *selected_printer_core = nullptr;
    if (selection.printer.ref.origin() == PresetOrigin::project_embedded) {
        const auto found = embedded.find(embedded_key(PresetKind::printer,
                                                       selection.printer.ref.id()));
        if (found != embedded.end()) selected_printer_core = &found->second.core;
    } else {
        const auto found = catalog->records.find(detail::PresetKey{
            PresetKind::printer, selection.printer.ref.origin(), selection.printer.ref.id()});
        if (found != catalog->records.end() && found->second.core)
            selected_printer_core = found->second.core.get();
    }
    if (!selected_printer_core || selected_printer_core->printer_technology() != Slic3r::ptFFF)
        return failure<Project>(ErrorCode::unsupported,
                                "v1 only supports FFF project 3MF",
                                "/project/selected_presets/printer");

    auto identity = new_project_identity();
    auto data = std::make_shared<detail::ProjectData>(catalog->schema, context_state->options.limits,
                                                      std::move(selection), identity);
    data->source_path = path;
    data->temporary_files = std::move(project_temporary_files);
    data->model = std::move(model);
    data->project_config = std::move(project_config);
    data->embedded = std::move(embedded);
    for (const auto *plate : raw_plates) if (plate) data->plates.push_back(*plate);
    for (const auto *preset : raw_presets) if (preset) data->embedded_core.push_back(*preset);
    auto project_patch = detail::config_to_generic_patch(data->project_config, data->schema);
    if (!project_patch.has_value())
        return failure<Project>(*project_patch.error_code(),
                                project_patch.diagnostics().front().message,
                                project_patch.diagnostics().front().field);
    data->project_overrides = std::move(project_patch).value();
    const auto valid_map = validate_map(project_map(*data), data->selection.filaments.size(),
                                        "/filament_map");
    if (!valid_map.has_value())
        return failure<Project>(ErrorCode::invalid_configuration,
                                valid_map.diagnostics().front().message,
                                valid_map.diagnostics().front().field);
    const std::size_t tool_count = physical_tool_count(selected_printer_core->config);
    const auto validate_loaded_tools = [&](const std::vector<ToolId> &tools,
                                           const std::string &field) -> Result<void> {
        for (std::size_t index = 0; index < tools.size(); ++index)
            if (tool_count == 0 || tools[index].value > tool_count)
                return detail::ResultAccess::failure(ErrorCode::invalid_configuration,
                    "Filament map references an unavailable physical tool",
                    field + "/tools/" + std::to_string(index));
        return detail::ResultAccess::success();
    };
    auto loaded_tools_valid = validate_loaded_tools(project_map(*data).tools, "/filament_map");
    if (!loaded_tools_valid.has_value())
        return failure<Project>(*loaded_tools_valid.error_code(),
            loaded_tools_valid.diagnostics().front().message,
            loaded_tools_valid.diagnostics().front().field);
    for (std::size_t index = 0; index < data->plates.size(); ++index) {
        if (data->plates[index].filament_maps.empty()) continue;
        FilamentMapOverride local{
            data->plates[index].config.has("filament_map_mode")
                ? public_map_mode(data->plates[index].config) : project_map(*data).mode, {}};
        for (int tool : data->plates[index].filament_maps)
            local.tools.push_back({static_cast<std::uint32_t>(tool)});
        auto local_shape = validate_map(local, data->selection.filaments.size(),
            "/plates/" + std::to_string(index) + "/filament_map");
        if (!local_shape.has_value())
            return failure<Project>(ErrorCode::invalid_configuration,
                local_shape.diagnostics().front().message, local_shape.diagnostics().front().field);
        loaded_tools_valid = validate_loaded_tools(local.tools,
            "/plates/" + std::to_string(index) + "/filament_map");
        if (!loaded_tools_valid.has_value())
            return failure<Project>(*loaded_tools_valid.error_code(),
                loaded_tools_valid.diagnostics().front().message,
                loaded_tools_valid.diagnostics().front().field);
    }

    auto state = std::make_shared<Project::State>();
    state->current = std::move(data);
    state->context = std::move(context_state);
    return detail::ResultAccess::success(Project(std::move(state)));
}

Result<ProjectSnapshot> Project::snapshot() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return detail::ResultAccess::success(ProjectSnapshot(
        std::make_shared<const ProjectSnapshot::State>(state_->current, state_->context)));
}

ProjectEdit::ProjectEdit(std::shared_ptr<State> state) : state_(std::move(state)) {}

ProjectBuilder::ProjectBuilder(std::shared_ptr<State> state) : state_(std::move(state))
{
    if (!state_->identity) state_->identity = new_project_identity();
    state_->model.set_backup_path("detach");
}

namespace {

template<class BuilderState>
Result<void> check_builder(const BuilderState &state)
{
    if (state.owner != std::this_thread::get_id())
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "ProjectBuilder must be used from its creating thread",
                                             "/builder");
    if (state.terminal)
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "ProjectBuilder is already closed", "/builder");
    return detail::ResultAccess::success();
}

Result<Slic3r::TriangleMesh> builder_mesh(const MeshData &mesh, const std::string &field)
{
    if (mesh.vertices_mm.empty())
        return detail::ResultAccess::failure<Slic3r::TriangleMesh>(
            ErrorCode::invalid_argument, "Mesh must contain vertices", field + "/vertices_mm");
    if (mesh.triangles.empty())
        return detail::ResultAccess::failure<Slic3r::TriangleMesh>(
            ErrorCode::invalid_argument, "Mesh must contain triangles", field + "/triangles");
    if (mesh.vertices_mm.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return detail::ResultAccess::failure<Slic3r::TriangleMesh>(
            ErrorCode::resource_limit_exceeded, "Mesh vertex count exceeds core limits",
            field + "/vertices_mm");

    std::vector<Slic3r::Vec3f> vertices;
    vertices.reserve(mesh.vertices_mm.size());
    for (std::size_t index = 0; index < mesh.vertices_mm.size(); ++index) {
        const auto &vertex = mesh.vertices_mm[index];
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z))
            return detail::ResultAccess::failure<Slic3r::TriangleMesh>(
                ErrorCode::invalid_argument, "Mesh vertices must be finite",
                field + "/vertices_mm/" + std::to_string(index));
        vertices.emplace_back(static_cast<float>(vertex.x), static_cast<float>(vertex.y),
                              static_cast<float>(vertex.z));
    }

    std::vector<Slic3r::Vec3i32> faces;
    faces.reserve(mesh.triangles.size());
    for (std::size_t index = 0; index < mesh.triangles.size(); ++index) {
        const auto &triangle = mesh.triangles[index];
        if (triangle.a >= mesh.vertices_mm.size() || triangle.b >= mesh.vertices_mm.size() ||
            triangle.c >= mesh.vertices_mm.size())
            return detail::ResultAccess::failure<Slic3r::TriangleMesh>(
                ErrorCode::invalid_argument, "Triangle index is outside vertex array",
                field + "/triangles/" + std::to_string(index));
        if (triangle.a == triangle.b || triangle.a == triangle.c || triangle.b == triangle.c)
            return detail::ResultAccess::failure<Slic3r::TriangleMesh>(
                ErrorCode::invalid_argument, "Degenerate triangle is not allowed",
                field + "/triangles/" + std::to_string(index));
        faces.emplace_back(static_cast<int>(triangle.a), static_cast<int>(triangle.b),
                           static_cast<int>(triangle.c));
    }
    return detail::ResultAccess::success(
        Slic3r::TriangleMesh(std::move(vertices), std::move(faces)));
}

Result<Slic3r::Geometry::Transformation> builder_transform(
    const Matrix4d &matrix, const std::string &field)
{
    for (std::size_t index = 0; index < matrix.row_major.size(); ++index)
        if (!std::isfinite(matrix.row_major[index]))
            return detail::ResultAccess::failure<Slic3r::Geometry::Transformation>(
                ErrorCode::invalid_argument, "Transform matrix values must be finite",
                field + "/" + std::to_string(index));
    const auto close = [](double lhs, double rhs) {
        return std::abs(lhs - rhs) <= 1e-9;
    };
    if (!close(matrix.row_major[12], 0.0) || !close(matrix.row_major[13], 0.0) ||
        !close(matrix.row_major[14], 0.0) || !close(matrix.row_major[15], 1.0))
        return detail::ResultAccess::failure<Slic3r::Geometry::Transformation>(
            ErrorCode::invalid_argument, "Transform matrix must be affine", field);
    Slic3r::Transform3d transform = Slic3r::Transform3d::Identity();
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            transform.matrix()(row, column) =
                matrix.row_major[static_cast<std::size_t>(row * 4 + column)];
    return detail::ResultAccess::success(Slic3r::Geometry::Transformation(transform));
}

template<class BuilderState>
Result<void> validate_builder_selection(const BuilderState &state,
                                        std::optional<Slic3r::DynamicPrintConfig> &printer_config)
{
    if (!state.selection)
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Preset selection is required", "/selection");
    const auto &selection = *state.selection;
    if (selection.printer.ref.kind() != PresetKind::printer ||
        selection.process.ref.kind() != PresetKind::process ||
        selection.filaments.empty())
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Preset selection kinds are invalid", "/selection");
    auto catalog = state.context->preset_catalog;
    std::lock_guard<std::mutex> catalog_lock(catalog->mutex);
    const auto validate_selected = [&](const SelectedPreset &selected, PresetKind kind,
                                       const std::string &field) -> Result<void> {
        if (selected.ref.kind() != kind)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Selected preset kind is invalid",
                                                 field + "/ref/kind");
        if (selected.ref.origin() == PresetOrigin::project_embedded)
            return detail::ResultAccess::failure(ErrorCode::unsupported,
                                                 "ProjectBuilder v1 does not create embedded presets",
                                                 field);
        const auto found = catalog->records.find(detail::PresetKey{
            kind, selected.ref.origin(), selected.ref.id()});
        if (found == catalog->records.end())
            return detail::ResultAccess::failure(ErrorCode::not_found,
                                                 "Selected repository preset was not found", field);
        if (found->second.summary.revision != selected.revision)
            return detail::ResultAccess::failure(ErrorCode::conflict,
                                                 "Selected repository preset revision changed",
                                                 field + "/revision");
        if (kind == PresetKind::printer && found->second.core) {
            if (found->second.core->printer_technology() != Slic3r::ptFFF)
                return detail::ResultAccess::failure(ErrorCode::unsupported,
                                                     "v1 only supports FFF slicing",
                                                     field);
            printer_config = found->second.core->config;
        }
        return detail::ResultAccess::success();
    };
    auto valid = validate_selected(selection.printer, PresetKind::printer, "/selection/printer");
    if (!valid.has_value()) return valid;
    valid = validate_selected(selection.process, PresetKind::process, "/selection/process");
    if (!valid.has_value()) return valid;
    for (std::size_t index = 0; index < selection.filaments.size(); ++index) {
        valid = validate_selected(selection.filaments[index], PresetKind::filament,
                                  "/selection/filaments/" + std::to_string(index));
        if (!valid.has_value()) return valid;
    }
    return detail::ResultAccess::success();
}

} // namespace

Result<void> ProjectBuilder::set_selected_presets(PresetSelection selection)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value()) return valid;
    if (selection.printer.ref.kind() != PresetKind::printer ||
        selection.process.ref.kind() != PresetKind::process ||
        selection.filaments.empty())
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Preset selection kinds are invalid", "/selection");
    for (std::size_t index = 0; index < selection.filaments.size(); ++index)
        if (selection.filaments[index].ref.kind() != PresetKind::filament)
            return detail::ResultAccess::failure(
                ErrorCode::invalid_argument, "Filament selection kind is invalid",
                "/selection/filaments/" + std::to_string(index));
    state_->selection = std::move(selection);
    return detail::ResultAccess::success();
}

Result<void> ProjectBuilder::set_project_overrides(ConfigPatch patch)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value()) return valid;
    valid = validate_patch(state_->context->preset_catalog->schema, patch, OptionScope::project);
    if (!valid.has_value()) return valid;
    Slic3r::DynamicPrintConfig config = state_->project_config;
    auto applied = replace_generic_config(config, state_->context->preset_catalog->schema, patch);
    if (!applied.has_value()) return applied;
    state_->project_overrides = std::move(patch);
    state_->project_config = std::move(config);
    return detail::ResultAccess::success();
}

Result<void> ProjectBuilder::set_project_filament_map(FilamentMapOverride map)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value()) return valid;
    valid = validate_map_values(map, "/filament_map");
    if (!valid.has_value()) return valid;
    state_->project_filament_map = std::move(map);
    return detail::ResultAccess::success();
}

Result<PlateId> ProjectBuilder::add_plate(std::string name, ConfigPatch overrides)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value())
        return detail::ResultAccess::failure<PlateId>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);
    valid = validate_patch(state_->context->preset_catalog->schema, overrides, OptionScope::plate);
    if (!valid.has_value())
        return detail::ResultAccess::failure<PlateId>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);
    Slic3r::DynamicPrintConfig config;
    auto applied = replace_generic_config(config, state_->context->preset_catalog->schema, overrides);
    if (!applied.has_value())
        return detail::ResultAccess::failure<PlateId>(*applied.error_code(),
            applied.diagnostics().front().message, applied.diagnostics().front().field);
    Slic3r::PlateData plate;
    plate.plate_index = static_cast<int>(state_->plates.size());
    plate.plate_name = std::move(name);
    plate.locked = false;
    plate.config = std::move(config);
    state_->plates.push_back(std::move(plate));
    return detail::ResultAccess::success(detail::ProjectIdAccess::plate(
        state_->identity, static_cast<std::uint64_t>(state_->plates.back().plate_index + 1)));
}

Result<ObjectId> ProjectBuilder::add_object(ObjectInput input)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value())
        return detail::ResultAccess::failure<ObjectId>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);
    if (input.parts.empty())
        return detail::ResultAccess::failure<ObjectId>(
            ErrorCode::invalid_argument, "Object must contain at least one mesh part",
            "/object/parts");
    std::uint64_t added_triangles = 0;
    for (std::size_t index = 0; index < input.parts.size(); ++index) {
        const auto count = input.parts[index].mesh.triangles.size();
        if (std::numeric_limits<std::uint64_t>::max() - added_triangles < count)
            return detail::ResultAccess::failure<ObjectId>(
                ErrorCode::resource_limit_exceeded, "Mesh triangle count overflow",
                "/object/parts/" + std::to_string(index) + "/mesh/triangles");
        added_triangles += count;
    }
    if (state_->triangle_count > state_->context->options.limits.model_triangles ||
        added_triangles > state_->context->options.limits.model_triangles - state_->triangle_count)
        return detail::ResultAccess::failure<ObjectId>(
            ErrorCode::resource_limit_exceeded, "Model triangle limit exceeded",
            "/limits/model_triangles");
    valid = validate_patch(state_->context->preset_catalog->schema, input.overrides,
                           OptionScope::object);
    if (!valid.has_value())
        return detail::ResultAccess::failure<ObjectId>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);

    Slic3r::ModelObject *object = state_->model.add_object();
    object->name = std::move(input.name);
    if (!object->config.has("extruder") || object->config.extruder() == 0)
        object->config.set_key_value("extruder", new Slic3r::ConfigOptionInt(1));
    for (std::size_t index = 0; index < input.parts.size(); ++index) {
        auto mesh = builder_mesh(input.parts[index].mesh,
                                 "/object/parts/" + std::to_string(index) + "/mesh");
        if (!mesh.has_value())
            return detail::ResultAccess::failure<ObjectId>(*mesh.error_code(),
                mesh.diagnostics().front().message, mesh.diagnostics().front().field);
        valid = validate_patch(state_->context->preset_catalog->schema,
                               input.parts[index].overrides, OptionScope::part);
        if (!valid.has_value())
            return detail::ResultAccess::failure<ObjectId>(*valid.error_code(),
                valid.diagnostics().front().message, valid.diagnostics().front().field);
        Slic3r::ModelVolume *volume = object->add_volume(
            std::move(mesh).value(), Slic3r::ModelVolumeType::MODEL_PART, false);
        volume->name = std::move(input.parts[index].name);
        volume->source.object_idx = static_cast<int>(state_->model.objects.size() - 1);
        volume->source.volume_idx = static_cast<int>(index);
        Slic3r::DynamicPrintConfig part_config;
        auto applied = replace_generic_config(part_config, state_->context->preset_catalog->schema,
                                              input.parts[index].overrides);
        if (!applied.has_value())
            return detail::ResultAccess::failure<ObjectId>(*applied.error_code(),
                applied.diagnostics().front().message, applied.diagnostics().front().field);
        volume->config.assign_config(std::move(part_config));
    }
    Slic3r::DynamicPrintConfig object_config = object->config.get();
    auto applied = replace_generic_config(object_config, state_->context->preset_catalog->schema,
                                          input.overrides);
    if (!applied.has_value())
        return detail::ResultAccess::failure<ObjectId>(*applied.error_code(),
            applied.diagnostics().front().message, applied.diagnostics().front().field);
    object->config.assign_config(std::move(object_config));
    state_->triangle_count += added_triangles;
    return detail::ResultAccess::success(detail::ProjectIdAccess::object(
        state_->identity, object->id().id));
}

Result<InstanceId> ProjectBuilder::add_instance(PlateId plate, ObjectId object_id,
                                                Matrix4d transform)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value())
        return detail::ResultAccess::failure<InstanceId>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);
    if (!detail::ProjectIdAccess::belongs(plate, state_->identity))
        return detail::ResultAccess::failure<InstanceId>(
            ErrorCode::not_found, "Plate was not found", "/plate");
    if (plate.value() == 0 || plate.value() > state_->plates.size())
        return detail::ResultAccess::failure<InstanceId>(
            ErrorCode::not_found, "Plate was not found", "/plate");
    const auto object_index = object_position(state_->model, object_id, state_->identity);
    if (!object_index)
        return detail::ResultAccess::failure<InstanceId>(
            ErrorCode::not_found, "Object was not found", "/object");
    auto core_transform = builder_transform(transform, "/transform");
    if (!core_transform.has_value())
        return detail::ResultAccess::failure<InstanceId>(*core_transform.error_code(),
            core_transform.diagnostics().front().message,
            core_transform.diagnostics().front().field);
    Slic3r::ModelObject *object = state_->model.objects[*object_index];
    const std::size_t instance_index = object->instances.size();
    Slic3r::ModelInstance *instance = object->add_instance();
    instance->set_transformation(std::move(core_transform).value());
    state_->plates[static_cast<std::size_t>(plate.value() - 1)].objects_and_instances.emplace_back(
        static_cast<int>(*object_index), static_cast<int>(instance_index));
    return detail::ResultAccess::success(detail::ProjectIdAccess::instance(
        state_->identity, instance->id().id));
}

Result<void> ProjectBuilder::set_layer_ranges(ObjectId object_id, std::vector<LayerRange> ranges)
{
    auto valid = check_builder(*state_);
    if (!valid.has_value()) return valid;
    const auto object_index = object_position(state_->model, object_id, state_->identity);
    if (!object_index)
        return detail::ResultAccess::failure(ErrorCode::not_found,
                                             "Object was not found", "/object");
    Slic3r::t_layer_config_ranges staged;
    double previous_end = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto &range = ranges[index];
        if (!std::isfinite(range.z_min_mm) || !std::isfinite(range.z_max_mm) ||
            range.z_min_mm < 0.0 || range.z_min_mm >= range.z_max_mm ||
            range.z_min_mm < previous_end)
            return detail::ResultAccess::failure(
                ErrorCode::invalid_argument,
                "Layer ranges must be finite, ordered, and non-overlapping",
                "/layer_ranges/" + std::to_string(index));
        valid = validate_patch(state_->context->preset_catalog->schema, range.overrides,
                               OptionScope::layer_range);
        if (!valid.has_value()) return valid;
        Slic3r::DynamicPrintConfig config;
        auto applied = detail::apply_patch_to_core_config(range.overrides, config);
        if (!applied.has_value()) return applied;
        Slic3r::ModelConfig model_config;
        model_config.assign_config(std::move(config));
        staged.emplace(Slic3r::t_layer_height_range{range.z_min_mm, range.z_max_mm},
                       std::move(model_config));
        previous_end = range.z_max_mm;
    }
    state_->model.objects[*object_index]->layer_config_ranges = std::move(staged);
    return detail::ResultAccess::success();
}

Result<Project> ProjectBuilder::build()
{
    auto valid = check_builder(*state_);
    if (!valid.has_value())
        return detail::ResultAccess::failure<Project>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);
    if (state_->plates.empty())
        return detail::ResultAccess::failure<Project>(
            ErrorCode::invalid_argument, "Project must contain at least one plate", "/plates");
    std::size_t instance_count = 0;
    std::set<std::pair<int, int>> assigned_instances;
    for (std::size_t plate_index = 0; plate_index < state_->plates.size(); ++plate_index) {
        auto &plate = state_->plates[plate_index];
        if (plate.plate_index != static_cast<int>(plate_index))
            return detail::ResultAccess::failure<Project>(
                ErrorCode::invalid_configuration, "Plate identities are invalid", "/plates");
        for (const auto &binding : plate.objects_and_instances) {
            if (!assigned_instances.insert(binding).second)
                return detail::ResultAccess::failure<Project>(
                    ErrorCode::invalid_configuration,
                    "Every model instance must belong to exactly one plate", "/plates");
        }
    }
    for (std::size_t object_index = 0; object_index < state_->model.objects.size(); ++object_index) {
        const auto *object = state_->model.objects[object_index];
        if (object->volumes.empty())
            return detail::ResultAccess::failure<Project>(
                ErrorCode::invalid_argument, "Object must contain at least one part",
                "/objects/" + std::to_string(object_index) + "/parts");
        if (object->instances.empty())
            return detail::ResultAccess::failure<Project>(
                ErrorCode::invalid_argument, "Object must have at least one plate instance",
                "/objects/" + std::to_string(object_index) + "/instances");
        for (std::size_t instance_index = 0; instance_index < object->instances.size();
             ++instance_index) {
            const auto binding = std::make_pair(static_cast<int>(object_index),
                                                static_cast<int>(instance_index));
            if (!assigned_instances.count(binding))
                return detail::ResultAccess::failure<Project>(
                    ErrorCode::invalid_configuration,
                    "Every model instance must belong to exactly one plate", "/plates");
            ++instance_count;
        }
    }
    if (state_->model.objects.empty() || instance_count == 0)
        return detail::ResultAccess::failure<Project>(
            ErrorCode::invalid_argument, "Project must contain printable model instances",
            "/objects");

    std::optional<Slic3r::DynamicPrintConfig> selected_printer_config;
    valid = validate_builder_selection(*state_, selected_printer_config);
    if (!valid.has_value())
        return detail::ResultAccess::failure<Project>(*valid.error_code(),
            valid.diagnostics().front().message, valid.diagnostics().front().field);

    const std::size_t slot_count = state_->selection->filaments.size();
    Slic3r::DynamicPrintConfig project_config = state_->project_config;
    const FilamentMapOverride map = state_->project_filament_map.value_or(
        FilamentMapOverride{FilamentMapMode::manual,
                            std::vector<ToolId>(slot_count, ToolId{1})});
    auto map_valid = validate_map(map, slot_count, "/filament_map");
    if (!map_valid.has_value())
        return detail::ResultAccess::failure<Project>(ErrorCode::invalid_configuration,
            map_valid.diagnostics().front().message, map_valid.diagnostics().front().field);
    const std::size_t tool_count = selected_printer_config
        ? physical_tool_count(*selected_printer_config) : 0;
    for (std::size_t index = 0; index < map.tools.size(); ++index)
        if (tool_count == 0 || map.tools[index].value > tool_count)
            return detail::ResultAccess::failure<Project>(
                ErrorCode::invalid_configuration,
                "Filament map references an unavailable physical tool",
                "/filament_map/tools/" + std::to_string(index));
    apply_project_map(project_config, map);
    project_config.option<Slic3r::ConfigOptionString>(
        "printer_settings_id", true)->value = state_->selection->printer.ref.id();
    project_config.option<Slic3r::ConfigOptionString>(
        "print_settings_id", true)->value = state_->selection->process.ref.id();
    auto &filament_ids = project_config.option<Slic3r::ConfigOptionStrings>(
        "filament_settings_id", true)->values;
    filament_ids.clear();
    for (const auto &filament : state_->selection->filaments)
        filament_ids.push_back(filament.ref.id());

    auto data = std::make_shared<detail::ProjectData>(
        state_->context->preset_catalog->schema, state_->context->options.limits,
        *state_->selection, state_->identity);
    data->project_overrides = state_->project_overrides;
    data->project_config = std::move(project_config);
    data->model = std::move(state_->model);
    data->plates = std::move(state_->plates);

    auto patch_valid = validate_patch_for_commit(data->schema, data->project_overrides,
        OptionScope::project, std::nullopt, slot_count, "/project_overrides");
    if (!patch_valid.has_value())
        return detail::ResultAccess::failure<Project>(*patch_valid.error_code(),
            patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
    for (std::size_t plate_index = 0; plate_index < data->plates.size(); ++plate_index) {
        auto patch = detail::config_to_generic_patch(data->plates[plate_index].config, data->schema);
        if (!patch.has_value())
            return detail::ResultAccess::failure<Project>(*patch.error_code(),
                patch.diagnostics().front().message, patch.diagnostics().front().field);
        patch_valid = validate_patch_for_commit(data->schema, patch.value(), OptionScope::plate,
            std::nullopt, slot_count,
            "/plates/" + std::to_string(plate_index) + "/overrides");
        if (!patch_valid.has_value())
            return detail::ResultAccess::failure<Project>(*patch_valid.error_code(),
                patch_valid.diagnostics().front().message,
                patch_valid.diagnostics().front().field);
    }
    for (const Slic3r::ModelObject *object : data->model.objects) {
        auto patch = detail::config_to_generic_patch(object->config.get(), data->schema);
        if (!patch.has_value())
            return detail::ResultAccess::failure<Project>(*patch.error_code(),
                patch.diagnostics().front().message, patch.diagnostics().front().field);
        patch_valid = validate_patch_for_commit(data->schema, patch.value(), OptionScope::object,
            std::nullopt, slot_count,
            "/objects/" + std::to_string(object->id().id) + "/overrides");
        if (!patch_valid.has_value())
            return detail::ResultAccess::failure<Project>(*patch_valid.error_code(),
                patch_valid.diagnostics().front().message,
                patch_valid.diagnostics().front().field);
        for (const Slic3r::ModelVolume *volume : object->volumes) {
            patch = detail::config_to_generic_patch(volume->config.get(), data->schema);
            if (!patch.has_value())
                return detail::ResultAccess::failure<Project>(*patch.error_code(),
                    patch.diagnostics().front().message, patch.diagnostics().front().field);
            patch_valid = validate_patch_for_commit(data->schema, patch.value(),
                OptionScope::part, std::nullopt, slot_count,
                "/parts/" + std::to_string(volume->id().id) + "/overrides");
            if (!patch_valid.has_value())
                return detail::ResultAccess::failure<Project>(*patch_valid.error_code(),
                    patch_valid.diagnostics().front().message,
                    patch_valid.diagnostics().front().field);
        }
        std::size_t range_index = 0;
        for (const auto &[range, config] : object->layer_config_ranges) {
            (void) range;
            patch = detail::config_to_generic_patch(config.get(), data->schema);
            if (!patch.has_value())
                return detail::ResultAccess::failure<Project>(*patch.error_code(),
                    patch.diagnostics().front().message, patch.diagnostics().front().field);
            patch_valid = validate_patch_for_commit(data->schema, patch.value(),
                OptionScope::layer_range, std::nullopt, slot_count,
                "/objects/" + std::to_string(object->id().id) + "/layer_ranges/" +
                    std::to_string(range_index++) + "/overrides");
            if (!patch_valid.has_value())
                return detail::ResultAccess::failure<Project>(*patch_valid.error_code(),
                    patch_valid.diagnostics().front().message,
                    patch_valid.diagnostics().front().field);
        }
    }

    auto project_state = std::make_shared<Project::State>();
    project_state->current = std::move(data);
    project_state->context = state_->context;
    state_->terminal = true;
    return detail::ResultAccess::success(Project(std::move(project_state)));
}

void ProjectBuilder::discard() noexcept
{
    state_->terminal = true;
}

Result<void> ProjectEdit::set_project_selected_presets(PresetSelection selection, SlotRemap remap)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    if (state_->selection_set)
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "Preset selection was already changed", "/selection");
    const std::size_t old_count = state_->base->selection.filaments.size();
    const std::size_t new_count = selection.filaments.size();
    if (selection.printer.ref.kind() != PresetKind::printer ||
        selection.process.ref.kind() != PresetKind::process || new_count == 0)
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Preset selection kinds are invalid", "/selection");
    for (std::size_t index = 0; index < new_count; ++index)
        if (selection.filaments[index].ref.kind() != PresetKind::filament)
            return detail::ResultAccess::failure(
                ErrorCode::invalid_argument, "Filament selection kind is invalid",
                "/selection/filaments/" + std::to_string(index));
    if (remap.old_to_new.size() != old_count)
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Slot remap must cover every old slot",
                                             "/slot_remap/old_to_new");
    std::set<std::uint32_t> targets;
    for (std::size_t index = 0; index < remap.old_to_new.size(); ++index) {
        if (!remap.old_to_new[index]) continue;
        const auto target = remap.old_to_new[index]->value;
        if (target >= new_count)
            return detail::ResultAccess::failure(
                ErrorCode::invalid_argument, "Slot remap target is outside the new selection",
                "/slot_remap/old_to_new/" + std::to_string(index));
        if (!targets.insert(target).second)
            return detail::ResultAccess::failure(
                ErrorCode::invalid_argument, "Slot remap targets must be unique",
                "/slot_remap/old_to_new/" + std::to_string(index));
    }
    state_->selection_set = true;
    state_->target_selection = std::move(selection);
    state_->slot_remap = std::move(remap);
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_project_overrides(ConfigPatch patch)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    auto valid = validate_patch(state_->staged->schema, patch, OptionScope::project);
    if (!valid.has_value()) return valid;
    Slic3r::DynamicPrintConfig config = state_->staged->project_config;
    auto applied = replace_generic_config(config, state_->staged->schema, patch);
    if (!applied.has_value()) return applied;
    state_->staged->project_overrides = std::move(patch);
    state_->staged->project_config = std::move(config);
    state_->project_overrides_set = true;
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_plate_overrides(PlateId plate, ConfigPatch patch)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    const auto position = plate_position(*state_->staged, plate);
    if (!position) return detail::ResultAccess::failure(ErrorCode::not_found,
                                                         "Plate was not found", "/plate");
    auto valid = validate_patch(state_->staged->schema, patch, OptionScope::plate);
    if (!valid.has_value()) return valid;
    Slic3r::DynamicPrintConfig config = state_->staged->plates[*position].config;
    auto applied = replace_generic_config(config, state_->staged->schema, patch);
    if (!applied.has_value()) return applied;
    state_->staged->plates[*position].config = std::move(config);
    state_->plate_overrides_set.insert(plate.value());
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_object_overrides(ObjectId object, ConfigPatch patch)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    auto *target = find_object(*state_->staged, object);
    if (!target) return detail::ResultAccess::failure(ErrorCode::not_found,
                                                       "Object was not found", "/object");
    auto valid = validate_patch(state_->staged->schema, patch, OptionScope::object);
    if (!valid.has_value()) return valid;
    Slic3r::DynamicPrintConfig config = target->config.get();
    auto applied = replace_generic_config(config, state_->staged->schema, patch);
    if (!applied.has_value()) return applied;
    target->config.assign_config(std::move(config));
    state_->object_overrides_set.insert(object.value());
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_part_overrides(PartId part, ConfigPatch patch)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    auto *target = find_part(*state_->staged, part);
    if (!target) return detail::ResultAccess::failure(ErrorCode::not_found,
                                                       "Part was not found", "/part");
    auto valid = validate_patch(state_->staged->schema, patch, OptionScope::part);
    if (!valid.has_value()) return valid;
    Slic3r::DynamicPrintConfig config = target->config.get();
    auto applied = replace_generic_config(config, state_->staged->schema, patch);
    if (!applied.has_value()) return applied;
    target->config.assign_config(std::move(config));
    state_->part_overrides_set.insert(part.value());
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_layer_ranges(ObjectId object, std::vector<LayerRange> ranges)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    auto *target = find_object(*state_->staged, object);
    if (!target) return detail::ResultAccess::failure(ErrorCode::not_found,
                                                       "Object was not found", "/object");
    Slic3r::t_layer_config_ranges staged;
    double previous_end = -std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        const auto &range = ranges[index];
        if (!std::isfinite(range.z_min_mm) || !std::isfinite(range.z_max_mm) ||
            range.z_min_mm < 0.0 || range.z_min_mm >= range.z_max_mm ||
            range.z_min_mm < previous_end)
            return detail::ResultAccess::failure(
                ErrorCode::invalid_argument, "Layer ranges must be finite, ordered, and non-overlapping",
                "/layer_ranges/" + std::to_string(index));
        auto valid = validate_patch(state_->staged->schema, range.overrides,
                                    OptionScope::layer_range);
        if (!valid.has_value()) return valid;
        Slic3r::DynamicPrintConfig config;
        auto applied = detail::apply_patch_to_core_config(range.overrides, config);
        if (!applied.has_value()) return applied;
        Slic3r::ModelConfig model_config;
        model_config.assign_config(std::move(config));
        staged.emplace(Slic3r::t_layer_height_range{range.z_min_mm, range.z_max_mm},
                       std::move(model_config));
        previous_end = range.z_max_mm;
    }
    target->layer_config_ranges = std::move(staged);
    state_->layer_ranges_set.insert(object.value());
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_project_filament_map(FilamentMapOverride map)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    auto valid = validate_map_values(map, "/filament_map");
    if (!valid.has_value()) return valid;
    apply_project_map(*state_->staged, map);
    state_->project_map_set = true;
    return detail::ResultAccess::success();
}

Result<void> ProjectEdit::set_local_filament_map_override(
    PlateId plate, std::optional<FilamentMapOverride> map)
{
    auto usable = check_editor(*state_);
    if (!usable.has_value()) return usable;
    const auto position = plate_position(*state_->staged, plate);
    if (!position) return detail::ResultAccess::failure(ErrorCode::not_found,
                                                         "Plate was not found", "/plate");
    if (map) {
        auto valid = validate_map_values(*map, "/filament_map");
        if (!valid.has_value()) return valid;
        auto &target = state_->staged->plates[*position];
        target.filament_maps.clear();
        for (ToolId tool : map->tools) target.filament_maps.push_back(static_cast<int>(tool.value));
        target.config.set_key_value("filament_map_mode",
            new Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>(core_map_mode(map->mode)));
    } else {
        state_->staged->plates[*position].filament_maps.clear();
        state_->staged->plates[*position].config.erase("filament_map_mode");
    }
    state_->local_map_set.insert(plate.value());
    return detail::ResultAccess::success();
}

Result<ProjectRevision> ProjectEdit::commit()
{
    auto usable = check_editor(*state_);
    if (!usable.has_value())
        return failure<ProjectRevision>(*usable.error_code(), usable.diagnostics().front().message,
                                      usable.diagnostics().front().field);

    auto candidate = std::make_shared<detail::ProjectData>(*state_->staged);
    if (state_->target_selection) {
        candidate->selection = *state_->target_selection;
        const SlotRemap &remap = *state_->slot_remap;
        const std::size_t new_count = candidate->selection.filaments.size();

        const auto migrate_config = [&](const Slic3r::ConfigBase &original,
                                        Slic3r::DynamicPrintConfig &target,
                                        const std::string &field) -> Result<void> {
            auto original_patch = detail::config_to_generic_patch(original, candidate->schema);
            if (!original_patch.has_value())
                return detail::ResultAccess::failure(*original_patch.error_code(),
                    original_patch.diagnostics().front().message,
                    original_patch.diagnostics().front().field);
            auto migrated = remap_patch(original_patch.value(), candidate->schema, remap, field);
            if (!migrated.has_value())
                return detail::ResultAccess::failure(*migrated.error_code(),
                    migrated.diagnostics().front().message, migrated.diagnostics().front().field);
            return replace_generic_config(target, candidate->schema, migrated.value());
        };

        if (!state_->project_overrides_set) {
            auto migrated = remap_patch(state_->base->project_overrides, candidate->schema, remap,
                                        "/project_overrides");
            if (!migrated.has_value())
                return failure<ProjectRevision>(*migrated.error_code(),
                    migrated.diagnostics().front().message, migrated.diagnostics().front().field);
            candidate->project_overrides = migrated.value();
            auto replaced = replace_generic_config(candidate->project_config, candidate->schema,
                                                   candidate->project_overrides);
            if (!replaced.has_value())
                return failure<ProjectRevision>(*replaced.error_code(),
                    replaced.diagnostics().front().message, replaced.diagnostics().front().field);
        }
        for (std::size_t index = 0; index < candidate->plates.size(); ++index) {
            const auto plate_id = static_cast<std::uint64_t>(candidate->plates[index].plate_index + 1);
            if (state_->plate_overrides_set.count(plate_id)) continue;
            auto migrated = migrate_config(state_->base->plates[index].config,
                                            candidate->plates[index].config,
                                            "/plates/" + std::to_string(index) + "/overrides");
            if (!migrated.has_value())
                return failure<ProjectRevision>(*migrated.error_code(),
                    migrated.diagnostics().front().message, migrated.diagnostics().front().field);
        }
        for (Slic3r::ModelObject *object : candidate->model.objects) {
            const auto object_id = static_cast<std::uint64_t>(object->id().id);
            const Slic3r::ModelObject *original = find_object(
                *state_->base, detail::ProjectIdAccess::object(state_->base->identity, object_id));
            if (!original) continue;
            if (!state_->object_overrides_set.count(object_id)) {
                Slic3r::DynamicPrintConfig target = object->config.get();
                auto migrated = migrate_config(original->config.get(), target,
                                                "/objects/" + std::to_string(object_id) + "/overrides");
                if (!migrated.has_value())
                    return failure<ProjectRevision>(*migrated.error_code(),
                        migrated.diagnostics().front().message, migrated.diagnostics().front().field);
                object->config.assign_config(std::move(target));
            }
            for (Slic3r::ModelVolume *volume : object->volumes) {
                const auto part_id = static_cast<std::uint64_t>(volume->id().id);
                if (state_->part_overrides_set.count(part_id)) continue;
                const Slic3r::ModelVolume *original_volume = nullptr;
                for (const Slic3r::ModelVolume *candidate_volume : original->volumes)
                    if (candidate_volume->id().id == volume->id().id) {
                        original_volume = candidate_volume;
                        break;
                    }
                if (!original_volume) continue;
                Slic3r::DynamicPrintConfig target = volume->config.get();
                auto migrated = migrate_config(original_volume->config.get(), target,
                                                "/parts/" + std::to_string(part_id) + "/overrides");
                if (!migrated.has_value())
                    return failure<ProjectRevision>(*migrated.error_code(),
                        migrated.diagnostics().front().message, migrated.diagnostics().front().field);
                volume->config.assign_config(std::move(target));
            }
            if (!state_->layer_ranges_set.count(object_id)) {
                Slic3r::t_layer_config_ranges ranges;
                for (const auto &[range, original_config] : original->layer_config_ranges) {
                    Slic3r::DynamicPrintConfig target = original_config.get();
                    auto migrated = migrate_config(original_config.get(), target,
                        "/objects/" + std::to_string(object_id) + "/layer_ranges");
                    if (!migrated.has_value())
                        return failure<ProjectRevision>(*migrated.error_code(),
                            migrated.diagnostics().front().message, migrated.diagnostics().front().field);
                    Slic3r::ModelConfig model_config;
                    model_config.assign_config(std::move(target));
                    ranges.emplace(range, std::move(model_config));
                }
                object->layer_config_ranges = std::move(ranges);
            }
        }

        for (const auto &[key, original] : state_->base->embedded) {
            auto target = candidate->embedded.find(key);
            if (target == candidate->embedded.end()) continue;
            auto migrated = remap_patch(original.overrides, candidate->schema, remap,
                                        "/embedded_presets/" + key + "/overrides");
            if (!migrated.has_value())
                return failure<ProjectRevision>(*migrated.error_code(),
                    migrated.diagnostics().front().message, migrated.diagnostics().front().field);
            target->second.overrides = migrated.value();
            target->second.effective = merge_values(target->second.inherited, target->second.overrides);
            Slic3r::DynamicPrintConfig core = target->second.core.config;
            ConfigPatch effective_patch;
            for (const ConfigEntry &entry : target->second.effective.entries())
                effective_patch.set(entry.option, entry.value);
            auto applied = replace_generic_config(core, candidate->schema, effective_patch);
            if (!applied.has_value())
                return failure<ProjectRevision>(*applied.error_code(),
                    applied.diagnostics().front().message, applied.diagnostics().front().field);
            target->second.core.config = std::move(core);
        }

        const bool identity = remap_is_identity(remap, new_count);
        for (std::size_t index = 0; index < candidate->plates.size(); ++index) {
            const auto plate_id = static_cast<std::uint64_t>(candidate->plates[index].plate_index + 1);
            if (!state_->local_map_set.count(plate_id)) {
                if (state_->base->plates[index].filament_maps.empty())
                    candidate->plates[index].filament_maps.clear();
                else
                    migrate_map(state_->base->plates[index].filament_maps, remap, new_count,
                                candidate->plates[index].filament_maps);
            }
            const int core_index = candidate->plates[index].plate_index;
            auto found = candidate->model.plates_custom_gcodes.find(core_index);
            if (found == candidate->model.plates_custom_gcodes.end()) continue;
            if (!identity && std::any_of(found->second.gcodes.begin(), found->second.gcodes.end(),
                    [](const Slic3r::CustomGCode::Item &item) {
                        return !item.extra.empty() &&
                            (item.type == Slic3r::CustomGCode::Custom ||
                             item.type == Slic3r::CustomGCode::Template);
                    }))
                return failure<ProjectRevision>(ErrorCode::unsupported,
                    "Raw custom G-code must be replaced when logical slots change",
                    "/plates/" + std::to_string(index) + "/custom_gcode");
            for (std::size_t item_index = 0; item_index < found->second.gcodes.size(); ++item_index) {
                auto &item = found->second.gcodes[item_index];
                if (item.extruder <= 0) continue;
                const std::size_t old_slot = static_cast<std::size_t>(item.extruder - 1);
                if (old_slot >= remap.old_to_new.size() || !remap.old_to_new[old_slot])
                    return failure<ProjectRevision>(ErrorCode::invalid_configuration,
                        "Deleted filament slot is still referenced by custom G-code",
                        "/plates/" + std::to_string(index) + "/custom_gcode/items/" +
                            std::to_string(item_index) + "/filament_slot");
                item.extruder = static_cast<int>(remap.old_to_new[old_slot]->value + 1);
            }
        }
        if (!state_->project_map_set) {
            const FilamentMapOverride original = project_map(*state_->base);
            std::vector<int> old_tools;
            old_tools.reserve(original.tools.size());
            for (ToolId tool : original.tools) old_tools.push_back(static_cast<int>(tool.value));
            std::vector<int> new_tools;
            migrate_map(old_tools, remap, new_count, new_tools);
            FilamentMapOverride migrated{original.mode, {}};
            for (int tool : new_tools)
                migrated.tools.push_back({static_cast<std::uint32_t>(tool)});
            apply_project_map(*candidate, migrated);
        }
    }

    auto catalog = state_->project->context->preset_catalog;
    std::lock_guard<std::mutex> catalog_lock(catalog->mutex);
    std::optional<Slic3r::DynamicPrintConfig> selected_printer_config;
    const auto validate_selected = [&](const SelectedPreset &selected, PresetKind kind,
                                       const std::string &field) -> Result<void> {
        if (selected.ref.kind() != kind)
            return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                                 "Selected preset kind is invalid", field + "/ref/kind");
        if (selected.ref.origin() == PresetOrigin::project_embedded) {
            const auto found = candidate->embedded.find(embedded_key(kind, selected.ref.id()));
            if (found == candidate->embedded.end())
                return detail::ResultAccess::failure(ErrorCode::not_found,
                                                     "Selected embedded preset was not found", field);
            if (found->second.summary.revision != selected.revision)
                return detail::ResultAccess::failure(ErrorCode::conflict,
                                                     "Selected embedded preset revision changed",
                                                     field + "/revision");
            if (kind == PresetKind::printer) selected_printer_config = found->second.core.config;
            return detail::ResultAccess::success();
        }
        const auto found = catalog->records.find(detail::PresetKey{
            kind, selected.ref.origin(), selected.ref.id()});
        if (found == catalog->records.end())
            return detail::ResultAccess::failure(ErrorCode::not_found,
                                                 "Selected repository preset was not found", field);
        if (found->second.summary.revision != selected.revision)
            return detail::ResultAccess::failure(ErrorCode::conflict,
                                                 "Selected repository preset revision changed",
                                                 field + "/revision");
        if (kind == PresetKind::printer && found->second.core)
            selected_printer_config = found->second.core->config;
        return detail::ResultAccess::success();
    };
    auto selected_valid = validate_selected(candidate->selection.printer, PresetKind::printer,
                                            "/selection/printer");
    if (!selected_valid.has_value())
        return failure<ProjectRevision>(*selected_valid.error_code(),
                                      selected_valid.diagnostics().front().message,
                                      selected_valid.diagnostics().front().field);
    selected_valid = validate_selected(candidate->selection.process, PresetKind::process,
                                       "/selection/process");
    if (!selected_valid.has_value())
        return failure<ProjectRevision>(*selected_valid.error_code(),
                                      selected_valid.diagnostics().front().message,
                                      selected_valid.diagnostics().front().field);
    for (std::size_t index = 0; index < candidate->selection.filaments.size(); ++index) {
        selected_valid = validate_selected(candidate->selection.filaments[index], PresetKind::filament,
                                           "/selection/filaments/" + std::to_string(index));
        if (!selected_valid.has_value())
            return failure<ProjectRevision>(*selected_valid.error_code(),
                                          selected_valid.diagnostics().front().message,
                                          selected_valid.diagnostics().front().field);
    }
    for (const auto &[key, record] : candidate->embedded) {
        if (!record.summary.parent) continue;
        const SelectedPreset &parent = *record.summary.parent;
        if (parent.ref.origin() == PresetOrigin::project_embedded) {
            const auto found = candidate->embedded.find(
                embedded_key(parent.ref.kind(), parent.ref.id()));
            if (found == candidate->embedded.end())
                return failure<ProjectRevision>(ErrorCode::not_found,
                    "Embedded preset parent was not found",
                    "/embedded_presets/" + key + "/parent");
            if (found->second.summary.revision != parent.revision)
                return failure<ProjectRevision>(ErrorCode::conflict,
                    "Embedded preset parent revision changed",
                    "/embedded_presets/" + key + "/parent/revision");
        } else {
            const auto found = catalog->records.find(detail::PresetKey{
                parent.ref.kind(), parent.ref.origin(), parent.ref.id()});
            if (found == catalog->records.end())
                return failure<ProjectRevision>(ErrorCode::not_found,
                    "Repository preset parent was not found",
                    "/embedded_presets/" + key + "/parent");
            if (found->second.summary.revision != parent.revision)
                return failure<ProjectRevision>(ErrorCode::conflict,
                    "Repository preset parent revision changed",
                    "/embedded_presets/" + key + "/parent/revision");
        }
    }

    const std::size_t slot_count = candidate->selection.filaments.size();
    auto patch_valid = validate_patch_for_commit(candidate->schema, candidate->project_overrides,
        OptionScope::project, std::nullopt, slot_count, "/project_overrides");
    if (!patch_valid.has_value())
        return failure<ProjectRevision>(*patch_valid.error_code(),
            patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
    for (std::size_t index = 0; index < candidate->plates.size(); ++index) {
        auto patch = detail::config_to_generic_patch(candidate->plates[index].config, candidate->schema);
        if (!patch.has_value())
            return failure<ProjectRevision>(*patch.error_code(), patch.diagnostics().front().message,
                                          patch.diagnostics().front().field);
        patch_valid = validate_patch_for_commit(candidate->schema, patch.value(), OptionScope::plate,
            std::nullopt, slot_count, "/plates/" + std::to_string(index) + "/overrides");
        if (!patch_valid.has_value())
            return failure<ProjectRevision>(*patch_valid.error_code(),
                patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
    }
    for (const Slic3r::ModelObject *object : candidate->model.objects) {
        const std::string object_field = "/objects/" + std::to_string(object->id().id);
        auto patch = detail::config_to_generic_patch(object->config.get(), candidate->schema);
        if (!patch.has_value())
            return failure<ProjectRevision>(*patch.error_code(), patch.diagnostics().front().message,
                                          patch.diagnostics().front().field);
        patch_valid = validate_patch_for_commit(candidate->schema, patch.value(), OptionScope::object,
            std::nullopt, slot_count, object_field + "/overrides");
        if (!patch_valid.has_value())
            return failure<ProjectRevision>(*patch_valid.error_code(),
                patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
        for (const Slic3r::ModelVolume *volume : object->volumes) {
            patch = detail::config_to_generic_patch(volume->config.get(), candidate->schema);
            if (!patch.has_value())
                return failure<ProjectRevision>(*patch.error_code(), patch.diagnostics().front().message,
                                              patch.diagnostics().front().field);
            patch_valid = validate_patch_for_commit(candidate->schema, patch.value(), OptionScope::part,
                std::nullopt, slot_count,
                "/parts/" + std::to_string(volume->id().id) + "/overrides");
            if (!patch_valid.has_value())
                return failure<ProjectRevision>(*patch_valid.error_code(),
                    patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
        }
        std::size_t range_index = 0;
        for (const auto &[range, config] : object->layer_config_ranges) {
            (void) range;
            patch = detail::config_to_generic_patch(config.get(), candidate->schema);
            if (!patch.has_value())
                return failure<ProjectRevision>(*patch.error_code(), patch.diagnostics().front().message,
                                              patch.diagnostics().front().field);
            patch_valid = validate_patch_for_commit(candidate->schema, patch.value(),
                OptionScope::layer_range, std::nullopt, slot_count,
                object_field + "/layer_ranges/" + std::to_string(range_index++) + "/overrides");
            if (!patch_valid.has_value())
                return failure<ProjectRevision>(*patch_valid.error_code(),
                    patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
        }
    }
    for (const auto &[key, record] : candidate->embedded) {
        patch_valid = validate_patch_for_commit(candidate->schema, record.overrides,
            OptionScope::preset, record.summary.ref.kind(), slot_count,
            "/embedded_presets/" + key + "/overrides");
        if (!patch_valid.has_value())
            return failure<ProjectRevision>(*patch_valid.error_code(),
                patch_valid.diagnostics().front().message, patch_valid.diagnostics().front().field);
    }

    auto map_valid = validate_map(project_map(*candidate), slot_count, "/filament_map");
    if (!map_valid.has_value())
        return failure<ProjectRevision>(ErrorCode::invalid_configuration,
                                      map_valid.diagnostics().front().message,
                                      map_valid.diagnostics().front().field);
    for (std::size_t index = 0; index < candidate->plates.size(); ++index) {
        if (candidate->plates[index].filament_maps.empty()) continue;
        FilamentMapOverride local{candidate->plates[index].config.has("filament_map_mode")
                                      ? public_map_mode(candidate->plates[index].config)
                                      : project_map(*candidate).mode,
                                  {}};
        for (int tool : candidate->plates[index].filament_maps)
            local.tools.push_back({static_cast<std::uint32_t>(tool)});
        map_valid = validate_map(local, slot_count,
                                 "/plates/" + std::to_string(index) + "/filament_map");
        if (!map_valid.has_value())
            return failure<ProjectRevision>(ErrorCode::invalid_configuration,
                                          map_valid.diagnostics().front().message,
                                          map_valid.diagnostics().front().field);
    }
    for (std::size_t plate_index = 0; plate_index < candidate->plates.size(); ++plate_index) {
        const int core_index = candidate->plates[plate_index].plate_index;
        const auto found = candidate->model.plates_custom_gcodes.find(core_index);
        if (found == candidate->model.plates_custom_gcodes.end()) continue;
        for (std::size_t item_index = 0; item_index < found->second.gcodes.size(); ++item_index) {
            const int extruder = found->second.gcodes[item_index].extruder;
            if (extruder < 0 || static_cast<std::size_t>(extruder) > slot_count)
                return failure<ProjectRevision>(ErrorCode::invalid_configuration,
                    "Custom G-code filament slot is outside the selection",
                    "/plates/" + std::to_string(plate_index) + "/custom_gcode/items/" +
                        std::to_string(item_index) + "/filament_slot");
        }
    }
    const std::size_t tool_count = selected_printer_config
        ? physical_tool_count(*selected_printer_config) : 0;
    const auto validate_tool_bounds = [&](const FilamentMapOverride &map,
                                          const std::string &field) -> Result<void> {
        for (std::size_t index = 0; index < map.tools.size(); ++index)
            if (tool_count == 0 || map.tools[index].value > tool_count)
                return detail::ResultAccess::failure(
                    ErrorCode::invalid_configuration,
                    "Filament map references an unavailable physical tool",
                    field + "/tools/" + std::to_string(index));
        return detail::ResultAccess::success();
    };
    auto bounds_valid = validate_tool_bounds(project_map(*candidate), "/filament_map");
    if (!bounds_valid.has_value())
        return failure<ProjectRevision>(*bounds_valid.error_code(),
            bounds_valid.diagnostics().front().message, bounds_valid.diagnostics().front().field);
    for (std::size_t index = 0; index < candidate->plates.size(); ++index) {
        if (candidate->plates[index].filament_maps.empty()) continue;
        FilamentMapOverride local{candidate->plates[index].config.has("filament_map_mode")
                                      ? public_map_mode(candidate->plates[index].config)
                                      : project_map(*candidate).mode, {}};
        for (int tool : candidate->plates[index].filament_maps)
            local.tools.push_back({static_cast<std::uint32_t>(tool)});
        bounds_valid = validate_tool_bounds(local,
            "/plates/" + std::to_string(index) + "/filament_map");
        if (!bounds_valid.has_value())
            return failure<ProjectRevision>(*bounds_valid.error_code(),
                bounds_valid.diagnostics().front().message, bounds_valid.diagnostics().front().field);
    }

    candidate->project_config.option<Slic3r::ConfigOptionString>(
        "printer_settings_id", true)->value = candidate->selection.printer.ref.id();
    candidate->project_config.option<Slic3r::ConfigOptionString>(
        "print_settings_id", true)->value = candidate->selection.process.ref.id();
    auto &filament_ids = candidate->project_config.option<Slic3r::ConfigOptionStrings>(
        "filament_settings_id", true)->values;
    filament_ids.clear();
    for (const auto &filament : candidate->selection.filaments)
        filament_ids.push_back(filament.ref.id());

    std::set<std::string> synchronized_embedded;
    for (Slic3r::Preset &core : candidate->embedded_core) {
        const std::string key = embedded_key(preset_kind(core.type), core.name);
        const auto found = candidate->embedded.find(key);
        if (found != candidate->embedded.end()) {
            core = found->second.core;
            synchronized_embedded.insert(key);
        }
    }
    for (const auto &[key, record] : candidate->embedded)
        if (!synchronized_embedded.count(key)) candidate->embedded_core.push_back(record.core);

    std::lock_guard<std::mutex> project_lock(state_->project->mutex);
    if (state_->project->current != state_->base ||
        state_->project->current->revision != state_->base->revision)
        return failure<ProjectRevision>(ErrorCode::conflict,
                                      "Project revision changed", "/expected_revision");
    candidate->revision = state_->base->revision + 1;
    state_->project->current = candidate;
    state_->terminal = true;
    return detail::ResultAccess::success(candidate->revision);
}

void ProjectEdit::discard() noexcept { state_->terminal = true; }

Result<ProjectEdit> Project::begin_edit(ProjectRevision expected)
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->current->revision != expected)
        return failure<ProjectEdit>(ErrorCode::conflict, "Project revision changed",
                                    "/expected_revision");
    auto edit = std::make_shared<ProjectEdit::State>();
    edit->project = state_;
    edit->base = state_->current;
    edit->staged = std::make_shared<detail::ProjectData>(*state_->current);
    edit->owner = std::this_thread::get_id();
    return detail::ResultAccess::success(ProjectEdit(std::move(edit)));
}

Result<void> Project::save(const std::filesystem::path &path, ProjectRevision expected) const
{
    std::shared_ptr<const detail::ProjectData> captured;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->current->revision != expected)
            return detail::ResultAccess::failure(ErrorCode::conflict,
                                                 "Project revision changed", "/expected_revision");
        captured = state_->current;
    }
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (extension != ".3mf")
        return detail::ResultAccess::failure(ErrorCode::unsupported,
                                             "v1 only saves Orca-compatible 3MF projects", "/path");
    detail::ProjectData copy(*captured);
    std::vector<Slic3r::PlateData *> plates;
    for (auto &plate : copy.plates) plates.push_back(&plate);
    std::vector<Slic3r::Preset *> presets;
    for (auto &preset : copy.embedded_core) presets.push_back(&preset);
    static std::atomic<std::uint64_t> next_save{1};
    const auto temporary = path.parent_path() /
        (path.filename().string() + ".libslicer." + std::to_string(
             std::chrono::high_resolution_clock::now().time_since_epoch().count()) + "." +
         std::to_string(next_save.fetch_add(1)) + ".tmp");
    struct TemporaryCleanup {
        std::filesystem::path path;
        bool active {true};
        ~TemporaryCleanup()
        {
            if (!active) return;
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(std::filesystem::path(path.string() + ".tmp"), ignored);
        }
    } cleanup{temporary};
    Slic3r::StoreParams params;
    const std::string temporary_string = temporary.string();
    params.path = temporary_string.c_str();
    params.model = &copy.model;
    params.plate_data_list = plates;
    params.project_presets = presets;
    params.config = &copy.project_config;
    params.strategy = Slic3r::SaveStrategy::Zip64 | Slic3r::SaveStrategy::Silence |
                      Slic3r::SaveStrategy::UseLoadedId;
    try {
        std::lock_guard<std::mutex> runtime(detail::runtime_mutex());
        const std::string old_resources = Slic3r::resources_dir();
        const std::string old_data = Slic3r::data_dir();
        const std::string old_temporary = Slic3r::temporary_dir();
        struct RestorePaths {
            std::string resources, data, temporary;
            ~RestorePaths()
            {
                Slic3r::set_resources_dir(resources);
                Slic3r::set_data_dir(data);
                Slic3r::set_temporary_dir(temporary);
            }
        } restore{old_resources, old_data, old_temporary};
        Slic3r::set_resources_dir(state_->context->options.resources_dir.string());
        Slic3r::set_data_dir(state_->context->options.data_dir.string());
        Slic3r::set_temporary_dir(state_->context->options.temporary_dir.string());
        if (!Slic3r::store_bbs_3mf(params))
            return detail::ResultAccess::failure(ErrorCode::io,
                                                 "Failed to save 3MF project", "/path");
        std::error_code error;
        if (!publish_file_atomically(temporary, path, error)) {
            return detail::ResultAccess::failure(ErrorCode::io,
                                                 "Failed to publish saved project: " + error.message(),
                                                 "/path");
        }
        cleanup.active = false;
    } catch (const std::exception &exception) {
        return detail::ResultAccess::failure(ErrorCode::io, exception.what(), "/path");
    }
    return detail::ResultAccess::success();
}

EffectiveConfiguration::EffectiveConfiguration(std::shared_ptr<const State> state)
    : state_(std::move(state)) {}

const ConfigSchema &EffectiveConfiguration::schema() const { return state_->schema; }
std::optional<ConfigValue> EffectiveConfiguration::get(const OptionId &option) const
{
    return state_->values.find(option);
}
const ConfigValues &EffectiveConfiguration::values() const { return state_->values; }
EffectiveConfiguration::Provenance EffectiveConfiguration::provenance() const
{
    return state_->provenance;
}

namespace {

Result<detail::FrozenSliceInput> resolve_impl(
    const std::shared_ptr<detail::ContextState> &context,
    const ProjectSnapshot &snapshot, PlateId plate,
    const PresetSelection &selection, EffectiveFilamentMap map)
{
    if (detail::ProjectSnapshotAccess::context(snapshot) != context)
        return failure<detail::FrozenSliceInput>(
            ErrorCode::invalid_argument,
            "Project snapshot belongs to a different SDK context", "/project");
    const auto &project_data = detail::ProjectSnapshotAccess::data(snapshot);
    if (!detail::ProjectIdAccess::belongs(plate, project_data->identity))
        return failure<detail::FrozenSliceInput>(ErrorCode::not_found,
                                           "Plate does not belong to the project", "/plate");
    const auto plate_index = plate_position(*project_data, plate);
    if (!plate_index)
        return failure<detail::FrozenSliceInput>(ErrorCode::not_found,
                                           "Plate was not found", "/plate");
    if (map.mode != FilamentMapMode::manual)
        return failure<detail::FrozenSliceInput>(ErrorCode::unsupported,
                                           "v1 slicing requires a manual filament map",
                                           "/filament_map/mode");
    auto map_valid = validate_map(FilamentMapOverride{map.mode, map.tools},
                                  selection.filaments.size(), "/filament_map");
    if (!map_valid.has_value())
        return failure<detail::FrozenSliceInput>(*map_valid.error_code(),
                                           map_valid.diagnostics().front().message,
                                           map_valid.diagnostics().front().field);

    auto catalog = context->preset_catalog;
    std::lock_guard<std::mutex> catalog_lock(catalog->mutex);
    struct CapturedPreset {
        Slic3r::DynamicPrintConfig core;
        PresetRef ref;
        PresetRevision revision;
    };
    const auto capture = [&](const SelectedPreset &selected, PresetKind kind,
                             const std::string &field) -> Result<CapturedPreset> {
        if (selected.ref.kind() != kind)
            return failure<CapturedPreset>(ErrorCode::invalid_argument,
                                           "Selected preset kind is invalid", field + "/ref/kind");
        if (selected.ref.origin() == PresetOrigin::project_embedded) {
            const auto found = project_data->embedded.find(
                embedded_key(kind, selected.ref.id()));
            if (found == project_data->embedded.end())
                return failure<CapturedPreset>(ErrorCode::not_found,
                                               "Embedded preset was not found", field);
            if (found->second.summary.revision != selected.revision)
                return failure<CapturedPreset>(ErrorCode::conflict,
                                               "Embedded preset revision changed", field + "/revision");
            return detail::ResultAccess::success(CapturedPreset{
                found->second.core.config, selected.ref, selected.revision});
        }
        const auto found = catalog->records.find(detail::PresetKey{
            kind, selected.ref.origin(), selected.ref.id()});
        if (found == catalog->records.end() || !found->second.core)
            return failure<CapturedPreset>(ErrorCode::not_found,
                                           "Repository preset was not found", field);
        if (found->second.summary.revision != selected.revision)
            return failure<CapturedPreset>(ErrorCode::conflict,
                                           "Repository preset revision changed", field + "/revision");
        return detail::ResultAccess::success(CapturedPreset{
            found->second.core->config, selected.ref, selected.revision});
    };

    auto process = capture(selection.process, PresetKind::process, "/selection/process");
    auto printer = capture(selection.printer, PresetKind::printer, "/selection/printer");
    if (!process.has_value())
        return failure<detail::FrozenSliceInput>(*process.error_code(),
                                           process.diagnostics().front().message,
                                           process.diagnostics().front().field);
    if (!printer.has_value())
        return failure<detail::FrozenSliceInput>(*printer.error_code(),
                                           printer.diagnostics().front().message,
                                           printer.diagnostics().front().field);
    std::vector<CapturedPreset> filament_records;
    std::vector<Slic3r::DynamicPrintConfig> filament_configs;
    for (std::size_t index = 0; index < selection.filaments.size(); ++index) {
        auto filament = capture(selection.filaments[index], PresetKind::filament,
                                "/selection/filaments/" + std::to_string(index));
        if (!filament.has_value())
            return failure<detail::FrozenSliceInput>(*filament.error_code(),
                                               filament.diagnostics().front().message,
                                               filament.diagnostics().front().field);
        filament_configs.push_back(filament.value().core);
        filament_records.push_back(std::move(filament).value());
    }

    Slic3r::DynamicPrintConfig config;
    config.apply(Slic3r::FullPrintConfig::defaults());
    config.apply(process.value().core);
    const Slic3r::DynamicPrintConfig default_filament =
        catalog->bundle ? catalog->bundle->filaments.default_preset().config
                        : Slic3r::DynamicPrintConfig{};
    config.apply(default_filament);
    config.apply(printer.value().core);
    config.apply(project_data->project_config);
    apply_selected_filaments(config, default_filament, filament_configs);
    config.apply(project_data->plates[*plate_index].config);
    for (const char *key : {"compatible_prints", "compatible_prints_condition",
                            "compatible_printers", "compatible_printers_condition",
                            "inherits", "different_settings_to_system"})
        config.erase(key);
    config.option<Slic3r::ConfigOptionEnumGeneric>("printer_technology", true)->value =
        Slic3r::ptFFF;

    const std::size_t tools = physical_tool_count(config);
    for (std::size_t index = 0; index < map.tools.size(); ++index)
        if (tools == 0 || map.tools[index].value > tools)
            return failure<detail::FrozenSliceInput>(
                ErrorCode::invalid_configuration, "Filament map references an unavailable physical tool",
                "/filament_map/tools/" + std::to_string(index));

    std::vector<int> core_map;
    for (ToolId tool : map.tools) core_map.push_back(static_cast<int>(tool.value));
    config.set_key_value("filament_map", new Slic3r::ConfigOptionInts(std::move(core_map)));
    config.set_key_value("filament_map_mode",
        new Slic3r::ConfigOptionEnum<Slic3r::FilamentMapMode>(Slic3r::fmmManual));
    config.option<Slic3r::ConfigOptionString>("printer_settings_id", true)->value =
        selection.printer.ref.id();
    config.option<Slic3r::ConfigOptionString>("print_settings_id", true)->value =
        selection.process.ref.id();
    auto &filament_ids = config.option<Slic3r::ConfigOptionStrings>(
        "filament_settings_id", true)->values;
    filament_ids.clear();
    for (const auto &filament : selection.filaments) filament_ids.push_back(filament.ref.id());

    auto values = detail::core_config_to_values(config);
    if (!values.has_value())
        return failure<detail::FrozenSliceInput>(*values.error_code(),
                                           values.diagnostics().front().message,
                                           values.diagnostics().front().field);
    std::vector<std::pair<PresetRef, PresetRevision>> provenance_presets;
    provenance_presets.emplace_back(process.value().ref, process.value().revision);
    provenance_presets.emplace_back(printer.value().ref, printer.value().revision);
    for (const auto &filament : filament_records)
        provenance_presets.emplace_back(filament.ref, filament.revision);
    EffectiveConfiguration::Provenance provenance{
        snapshot.revision(), plate, std::move(provenance_presets)};
    EffectiveConfiguration effective = detail::EffectiveConfigurationAccess::make(
        catalog->schema, std::move(values).value(), std::move(provenance), std::move(config));
    return detail::ResultAccess::success(detail::FrozenSliceInput{
        context, snapshot, plate, std::move(effective), std::move(map), {}});
}

} // namespace

Result<detail::FrozenSliceInput> detail::resolve_slice_input(
    std::shared_ptr<detail::ContextState> context, ProjectSnapshot project, PlateId plate,
    std::optional<detail::TemporarySliceSelection> temporary_selection)
{
    if (temporary_selection) {
        EffectiveFilamentMap map{temporary_selection->complete_manual_map.mode,
                                 temporary_selection->complete_manual_map.tools,
                                 EffectiveFilamentMap::Source::temporary};
        return resolve_impl(context, project, plate, temporary_selection->selection,
                            std::move(map));
    }

    auto map = project.effective_filament_map(plate);
    if (!map.has_value())
        return failure<detail::FrozenSliceInput>(*map.error_code(),
                                                 map.diagnostics().front().message,
                                                 map.diagnostics().front().field);
    return resolve_impl(context, project, plate, project.project_selected_presets(),
                        std::move(map).value());
}

} // namespace libslicer::v1
