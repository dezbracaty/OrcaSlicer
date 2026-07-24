#include <libslicer/v1/Preset.hpp>

#include "ConfigSchemaInternal.hpp"
#include "OrcaConfigAdapter.hpp"
#include "PresetInternal.hpp"

#include "libslic3r/Utils.hpp"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace libslicer::v1 {

PresetRef::PresetRef(std::shared_ptr<const Binding> binding) : binding_(std::move(binding)) {}

namespace {

detail::PresetKey key_for(const PresetRef &ref)
{
    return {ref.kind(), ref.origin(), ref.id()};
}

ConfigValues merge(const ConfigValues &base, const ConfigPatch &patch)
{
    std::vector<ConfigEntry> entries = base.entries();
    for (const ConfigEntry &entry : patch.entries()) {
        const auto found = std::find_if(entries.begin(), entries.end(), [&entry](const ConfigEntry &candidate) {
            return candidate.option == entry.option;
        });
        if (found == entries.end()) entries.push_back(entry);
        else found->value = entry.value;
    }
    return detail::ConfigValuesAccess::make(std::move(entries));
}

Result<void> validate_editor_thread(const std::thread::id &owner)
{
    if (owner != std::this_thread::get_id())
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "PresetEditor must be used from its creating thread",
                                             "/editor");
    return detail::ResultAccess::success();
}

bool valid_utf8_without_nul(const std::string &value)
{
    if (value.empty() || value.find('\0') != std::string::npos) return false;
    const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const unsigned char lead = bytes[index++];
        if (lead < 0x80) continue;
        std::size_t continuation = 0;
        std::uint32_t codepoint = 0;
        if ((lead & 0xe0) == 0xc0) { continuation = 1; codepoint = lead & 0x1f; }
        else if ((lead & 0xf0) == 0xe0) { continuation = 2; codepoint = lead & 0x0f; }
        else if ((lead & 0xf8) == 0xf0) { continuation = 3; codepoint = lead & 0x07; }
        else return false;
        if (index + continuation > value.size()) return false;
        for (std::size_t offset = 0; offset < continuation; ++offset) {
            const unsigned char byte = bytes[index++];
            if ((byte & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if ((continuation == 1 && codepoint < 0x80) ||
            (continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
    }
    return true;
}

Result<void> validate_override(const ConfigSchema &schema, PresetKind kind,
                               const OptionId &option, const ConfigValue &value)
{
    const auto descriptor = schema.find(option);
    if (!descriptor)
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Unknown configuration option",
                                             "/configuration/" + option.value());
    if (!descriptor->editable)
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Configuration option is owned by a typed API",
                                             "/configuration/" + option.value());
    if (std::find(descriptor->applicable_preset_kinds.begin(),
                  descriptor->applicable_preset_kinds.end(), kind) ==
        descriptor->applicable_preset_kinds.end())
        return detail::ResultAccess::failure(ErrorCode::invalid_argument,
                                             "Configuration option is not valid for this preset kind",
                                             "/configuration/" + option.value());

    ConfigPatch patch;
    patch.set(option, value);
    ConfigValidationContext context{OptionScope::preset, kind, std::nullopt, std::nullopt,
                                    std::nullopt};
    // Cross-preset rules are evaluated later with an explicit compatibility context.
    // Supply fixed empty captures here only for structural shape/range validation.
    if (kind == PresetKind::process)
        context.printer = ConfigValidationContext::PresetValues{
            0, detail::ConfigValuesAccess::make({})};
    if (kind == PresetKind::filament) {
        context.printer = ConfigValidationContext::PresetValues{
            0, detail::ConfigValuesAccess::make({})};
        context.process = ConfigValidationContext::PresetValues{
            0, detail::ConfigValuesAccess::make({})};
    }
    return detail::ConfigSchemaAccess::validate_structure(schema, patch, context);
}

const Slic3r::PresetCollection &preset_collection(
    const detail::PresetCatalogState &catalog, PresetKind kind)
{
    switch (kind) {
    case PresetKind::printer: return catalog.bundle->printers;
    case PresetKind::process: return catalog.bundle->prints;
    case PresetKind::filament: return catalog.bundle->filaments;
    }
    throw Slic3r::ConfigurationError("Unknown preset kind");
}

Slic3r::DynamicPrintConfig default_core_config(
    const detail::PresetCatalogState &catalog, PresetKind kind)
{
    return preset_collection(catalog, kind).default_preset().config;
}

} // namespace

PresetRef PresetRef::system(PresetKind kind, std::string id)
{
    return PresetRef(std::make_shared<const Binding>(Binding{kind, PresetOrigin::system, std::move(id)}));
}

PresetRef PresetRef::vendor(PresetKind kind, std::string id)
{
    return PresetRef(std::make_shared<const Binding>(Binding{kind, PresetOrigin::vendor, std::move(id)}));
}

PresetRef PresetRef::user(PresetKind kind, std::string id)
{
    return PresetRef(std::make_shared<const Binding>(Binding{kind, PresetOrigin::user, std::move(id)}));
}

PresetKind PresetRef::kind() const noexcept { return binding_->kind; }
PresetOrigin PresetRef::origin() const noexcept { return binding_->origin; }
std::string PresetRef::id() const { return binding_->id; }

bool operator==(const PresetRef &lhs, const PresetRef &rhs) noexcept
{
    return lhs.binding_->kind == rhs.binding_->kind && lhs.binding_->origin == rhs.binding_->origin &&
           lhs.binding_->id == rhs.binding_->id;
}


struct PresetView::State {
    PresetSummary summary;
    ConfigValues  inherited;
    ConfigValues  effective;
    ConfigPatch   overrides;
};

PresetView::PresetView(std::shared_ptr<const State> state) : state_(std::move(state)) {}
PresetSummary PresetView::metadata() const { return state_->summary; }
ConfigValues PresetView::inherited_values() const { return state_->inherited; }
ConfigValues PresetView::effective_values() const { return state_->effective; }
ConfigPatch PresetView::overrides() const { return state_->overrides; }

struct PresetEditor::State {
    std::shared_ptr<detail::PresetCatalogState> catalog;
    PresetSummary                              summary;
    ConfigValues                               inherited;
    ConfigPatch                                overrides;
    std::optional<SelectedPreset>              captured_parent;
    std::thread::id                            owner;
    std::uint64_t                              captured_catalog_generation {0};
    std::uint64_t                              captured_storage_generation {0};
    std::string                                captured_storage_fingerprint;
    bool                                       terminal {false};
    bool                                       creating {false};
};

PresetEditor::PresetEditor(std::shared_ptr<State> state) : state_(std::move(state)) {}
PresetView PresetEditor::snapshot() const
{
    auto view = std::make_shared<const PresetView::State>(PresetView::State{
        state_->summary, state_->inherited, merge(state_->inherited, state_->overrides),
        state_->overrides});
    return PresetView(std::move(view));
}

Result<void> PresetEditor::set_override(OptionId option, ConfigValue value)
{
    auto thread = validate_editor_thread(state_->owner);
    if (!thread.has_value()) return thread;
    if (state_->terminal)
        return detail::ResultAccess::failure(ErrorCode::conflict, "PresetEditor is already closed", "/editor");
    auto valid = validate_override(state_->catalog->schema, state_->summary.ref.kind(), option, value);
    if (!valid.has_value()) return valid;
    state_->overrides.set(std::move(option), std::move(value));
    return detail::ResultAccess::success();
}

Result<void> PresetEditor::erase_override(const OptionId &option)
{
    auto thread = validate_editor_thread(state_->owner);
    if (!thread.has_value()) return thread;
    if (state_->terminal)
        return detail::ResultAccess::failure(ErrorCode::conflict, "PresetEditor is already closed", "/editor");
    state_->overrides.erase(option);
    return detail::ResultAccess::success();
}

Result<PresetSummary> PresetEditor::commit()
{
    auto thread = validate_editor_thread(state_->owner);
    if (!thread.has_value())
        return detail::ResultAccess::failure<PresetSummary>(*thread.error_code(),
                                                            thread.diagnostics().front().message,
                                                            thread.diagnostics().front().field);
    if (state_->terminal)
        return detail::ResultAccess::failure<PresetSummary>(ErrorCode::conflict,
                                                             "PresetEditor is already closed", "/editor");

    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    if (state_->catalog->generation != state_->captured_catalog_generation)
        return detail::ResultAccess::failure<PresetSummary>(
            ErrorCode::conflict, "Preset catalog generation changed", "/expected_revision");
    const auto key = key_for(state_->summary.ref);
    auto found = state_->catalog->records.find(key);
    if (!state_->creating) {
        if (found == state_->catalog->records.end())
            return detail::ResultAccess::failure<PresetSummary>(ErrorCode::not_found,
                                                                 "Preset no longer exists", "/ref");
        if (found->second.summary.revision != state_->summary.revision)
            return detail::ResultAccess::failure<PresetSummary>(ErrorCode::conflict,
                                                                 "Preset revision changed", "/expected_revision");
    } else if (found != state_->catalog->records.end()) {
        return detail::ResultAccess::failure<PresetSummary>(ErrorCode::conflict,
                                                             "Preset id is already in use", "/preset/ref/id");
    }
    if (state_->captured_parent) {
        const auto parent = state_->catalog->records.find(key_for(state_->captured_parent->ref));
        if (parent == state_->catalog->records.end() ||
            parent->second.summary.revision != state_->captured_parent->revision)
            return detail::ResultAccess::failure<PresetSummary>(ErrorCode::conflict,
                                                                 "Parent preset revision changed",
                                                                 "/expected_revision");
    }

    Slic3r::DynamicPrintConfig effective_core;
    if (state_->captured_parent) {
        const auto parent = state_->catalog->records.find(key_for(state_->captured_parent->ref));
        if (parent != state_->catalog->records.end() && parent->second.core)
            effective_core = parent->second.core->config;
    } else {
        effective_core = default_core_config(*state_->catalog, state_->summary.ref.kind());
    }
    auto applied = detail::apply_patch_to_core_config(state_->overrides, effective_core);
    if (!applied.has_value())
        return detail::ResultAccess::failure<PresetSummary>(
            *applied.error_code(), applied.diagnostics().front().message,
            applied.diagnostics().front().field);
    const PresetRevision revision = state_->catalog->next_revision++;
    state_->summary.revision = revision;
    auto core = std::make_shared<Slic3r::Preset>(
        state_->summary.ref.kind() == PresetKind::printer ? Slic3r::Preset::TYPE_PRINTER :
        state_->summary.ref.kind() == PresetKind::filament ? Slic3r::Preset::TYPE_FILAMENT :
                                                            Slic3r::Preset::TYPE_PRINT,
        state_->summary.ref.id());
    core->config = std::move(effective_core);
    detail::PresetRecord record{state_->summary, state_->inherited,
                                merge(state_->inherited, state_->overrides), state_->overrides,
                                std::move(core)};
    auto persisted = detail::persist_user_preset(
        *state_->catalog, record, state_->captured_storage_generation,
        state_->captured_storage_fingerprint);
    if (!persisted.has_value()) {
        --state_->catalog->next_revision;
        state_->summary.revision = state_->creating ? 0 : found->second.summary.revision;
        return detail::ResultAccess::failure<PresetSummary>(
            *persisted.error_code(), persisted.diagnostics().front().message,
            persisted.diagnostics().front().field);
    }
    state_->catalog->records.insert_or_assign(key, std::move(record));
    state_->catalog->storage_generation = persisted.value();
    ++state_->catalog->generation;
    state_->terminal = true;
    return detail::ResultAccess::success(state_->summary, persisted.diagnostics());
}

void PresetEditor::discard() noexcept { state_->terminal = true; }

PresetRepository::PresetRepository(std::shared_ptr<State> state) : state_(std::move(state)) {}
ConfigSchema PresetRepository::schema() const { return state_->catalog->schema; }

Result<std::vector<PresetSummary>> PresetRepository::list(PresetKind kind) const
{
    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    std::vector<PresetSummary> result;
    for (const auto &[key, record] : state_->catalog->records)
        if (key.kind == kind) result.push_back(record.summary);
    return detail::ResultAccess::success(std::move(result));
}

Result<PresetView> PresetRepository::get(SelectedPreset preset) const
{
    if (preset.ref.origin() == PresetOrigin::project_embedded)
        return detail::ResultAccess::failure<PresetView>(ErrorCode::invalid_argument,
                                                         "Project-embedded presets are not repository presets",
                                                         "/ref/origin");
    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    const auto found = state_->catalog->records.find(key_for(preset.ref));
    if (found == state_->catalog->records.end())
        return detail::ResultAccess::failure<PresetView>(ErrorCode::not_found,
                                                         "Preset was not found", "/ref");
    if (found->second.summary.revision != preset.revision)
        return detail::ResultAccess::failure<PresetView>(ErrorCode::conflict,
                                                         "Preset revision changed", "/revision");
    const auto &record = found->second;
    auto view = std::make_shared<const PresetView::State>(
        PresetView::State{record.summary, record.inherited, record.effective, record.overrides});
    return detail::ResultAccess::success(PresetView(std::move(view)));
}

Result<PresetEditor> PresetRepository::edit(PresetRef preset, PresetRevision expected)
{
    if (preset.origin() != PresetOrigin::user)
        return detail::ResultAccess::failure<PresetEditor>(ErrorCode::unsupported,
                                                           "Only user presets are editable",
                                                           "/ref/origin");
    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    const auto found = state_->catalog->records.find(key_for(preset));
    if (found == state_->catalog->records.end())
        return detail::ResultAccess::failure<PresetEditor>(ErrorCode::not_found,
                                                           "Preset was not found", "/ref");
    if (found->second.summary.revision != expected)
        return detail::ResultAccess::failure<PresetEditor>(ErrorCode::conflict,
                                                           "Preset revision changed", "/expected_revision");
    auto editor = std::make_shared<PresetEditor::State>(PresetEditor::State{
        state_->catalog, found->second.summary, found->second.inherited, found->second.overrides,
        found->second.summary.parent, std::this_thread::get_id(),
        state_->catalog->generation, state_->catalog->storage_generation,
        state_->catalog->storage_fingerprint,
        false, false});
    return detail::ResultAccess::success(PresetEditor(std::move(editor)));
}

Result<PresetEditor> PresetRepository::create(PresetKind kind, std::string id,
                                               std::string display_name,
                                               std::optional<SelectedPreset> parent)
{
    if (!valid_utf8_without_nul(id))
        return detail::ResultAccess::failure<PresetEditor>(ErrorCode::invalid_argument,
                                                           "Preset id must be non-empty valid UTF-8 without NUL",
                                                           "/preset/id");
    if (!valid_utf8_without_nul(display_name))
        return detail::ResultAccess::failure<PresetEditor>(ErrorCode::invalid_argument,
                                                           "Preset display name must be non-empty valid UTF-8 without NUL",
                                                           "/preset/display_name");
    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    PresetRef ref = PresetRef::user(kind, std::move(id));
    if (state_->catalog->records.count(key_for(ref)))
        return detail::ResultAccess::failure<PresetEditor>(ErrorCode::conflict,
                                                           "Preset id is already in use", "/preset/ref/id");

    auto default_values = detail::core_config_to_values(default_core_config(*state_->catalog, kind));
    if (!default_values.has_value())
        return detail::ResultAccess::failure<PresetEditor>(
            *default_values.error_code(), default_values.diagnostics().front().message,
            default_values.diagnostics().front().field);
    ConfigValues inherited = std::move(default_values).value();
    if (parent) {
        if (parent->ref.kind() != kind)
            return detail::ResultAccess::failure<PresetEditor>(ErrorCode::invalid_argument,
                                                               "Parent and child preset kinds differ",
                                                               "/parent/ref/kind");
        const auto found = state_->catalog->records.find(key_for(parent->ref));
        if (found == state_->catalog->records.end())
            return detail::ResultAccess::failure<PresetEditor>(ErrorCode::not_found,
                                                               "Parent preset was not found", "/parent/ref");
        if (found->second.summary.revision != parent->revision)
            return detail::ResultAccess::failure<PresetEditor>(ErrorCode::conflict,
                                                               "Parent preset revision changed",
                                                               "/parent/revision");
        inherited = found->second.effective;
    }
    PresetSummary summary{ref, std::move(display_name), {}, parent, 0};
    auto editor = std::make_shared<PresetEditor::State>(PresetEditor::State{
        state_->catalog, std::move(summary), std::move(inherited), ConfigPatch{}, parent,
        std::this_thread::get_id(), state_->catalog->generation,
        state_->catalog->storage_generation,
        state_->catalog->storage_fingerprint, false, true});
    return detail::ResultAccess::success(PresetEditor(std::move(editor)));
}

Result<void> PresetRepository::erase(PresetRef preset, PresetRevision expected)
{
    if (preset.origin() != PresetOrigin::user)
        return detail::ResultAccess::failure(ErrorCode::unsupported,
                                             "Only user presets can be erased", "/ref/origin");
    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    const auto key = key_for(preset);
    const auto found = state_->catalog->records.find(key);
    if (found == state_->catalog->records.end())
        return detail::ResultAccess::failure(ErrorCode::not_found, "Preset was not found", "/ref");
    if (found->second.summary.revision != expected)
        return detail::ResultAccess::failure(ErrorCode::conflict,
                                             "Preset revision changed", "/expected_revision");
    for (const auto &[candidate_key, record] : state_->catalog->records) {
        if (record.summary.parent && record.summary.parent->ref == preset)
            return detail::ResultAccess::failure(ErrorCode::conflict,
                                                 "Preset is still used as a parent", "/ref");
    }
    auto persisted = detail::erase_user_preset(*state_->catalog, key,
                                                state_->catalog->storage_generation,
                                                state_->catalog->storage_fingerprint);
    if (!persisted.has_value())
        return detail::ResultAccess::failure(*persisted.error_code(),
                                             persisted.diagnostics().front().message,
                                             persisted.diagnostics().front().field);
    state_->catalog->records.erase(found);
    state_->catalog->storage_generation = persisted.value();
    ++state_->catalog->generation;
    return detail::ResultAccess::success(persisted.diagnostics());
}

Result<PresetCompatibility> PresetRepository::check_compatibility(
    SelectedPreset candidate, const PresetCompatibilityContext &context) const
{
    std::lock_guard<std::mutex> lock(state_->catalog->mutex);
    const auto candidate_it = state_->catalog->records.find(key_for(candidate.ref));
    if (candidate_it == state_->catalog->records.end())
        return detail::ResultAccess::failure<PresetCompatibility>(ErrorCode::not_found,
                                                                  "Candidate preset was not found",
                                                                  "/candidate/ref");
    if (candidate_it->second.summary.revision != candidate.revision)
        return detail::ResultAccess::failure<PresetCompatibility>(ErrorCode::conflict,
                                                                  "Candidate revision changed",
                                                                  "/candidate/revision");
    if (candidate.ref.kind() != PresetKind::printer && !context.printer)
        return detail::ResultAccess::failure<PresetCompatibility>(ErrorCode::invalid_argument,
                                                                  "Printer context is required",
                                                                  "/context/printer");
    if (candidate.ref.kind() == PresetKind::filament && !context.process)
        return detail::ResultAccess::failure<PresetCompatibility>(ErrorCode::invalid_argument,
                                                                  "Process context is required",
                                                                  "/context/process");

    const auto resolve_context = [&](const SelectedPreset &selected, PresetKind kind,
                                     const char *field) -> Result<const detail::PresetRecord *> {
        if (selected.ref.kind() != kind)
            return detail::ResultAccess::failure<const detail::PresetRecord *>(
                ErrorCode::invalid_argument, "Context preset kind is invalid",
                std::string(field) + "/ref/kind");
        const auto found = state_->catalog->records.find(key_for(selected.ref));
        if (found == state_->catalog->records.end())
            return detail::ResultAccess::failure<const detail::PresetRecord *>(
                ErrorCode::not_found, "Context preset was not found", field);
        if (found->second.summary.revision != selected.revision)
            return detail::ResultAccess::failure<const detail::PresetRecord *>(
                ErrorCode::conflict, "Context preset revision changed",
                std::string(field) + "/revision");
        return detail::ResultAccess::success<const detail::PresetRecord *>(&found->second);
    };

    const detail::PresetRecord *printer = nullptr;
    const detail::PresetRecord *process = nullptr;
    if (context.printer) {
        auto resolved = resolve_context(*context.printer, PresetKind::printer, "/context/printer");
        if (!resolved.has_value())
            return detail::ResultAccess::failure<PresetCompatibility>(
                *resolved.error_code(), resolved.diagnostics().front().message,
                resolved.diagnostics().front().field);
        printer = resolved.value();
    }
    if (context.process) {
        auto resolved = resolve_context(*context.process, PresetKind::process, "/context/process");
        if (!resolved.has_value())
            return detail::ResultAccess::failure<PresetCompatibility>(
                *resolved.error_code(), resolved.diagnostics().front().message,
                resolved.diagnostics().front().field);
        process = resolved.value();
    }

    bool compatible = true;
    const auto &record = candidate_it->second;
    if (record.core && candidate.ref.kind() == PresetKind::process && printer && printer->core) {
        compatible = Slic3r::is_compatible_with_printer(
            Slic3r::PresetWithVendorProfile{*record.core, record.core->vendor},
            Slic3r::PresetWithVendorProfile{*printer->core, printer->core->vendor});
    } else if (record.core && candidate.ref.kind() == PresetKind::filament && printer && process &&
               printer->core && process->core) {
        compatible = Slic3r::is_compatible_with_print(
            Slic3r::PresetWithVendorProfile{*record.core, record.core->vendor},
            Slic3r::PresetWithVendorProfile{*process->core, process->core->vendor},
            Slic3r::PresetWithVendorProfile{*printer->core, printer->core->vendor});
    }
    std::vector<Diagnostic> diagnostics;
    if (!compatible)
        diagnostics.push_back({ErrorCode::invalid_configuration, Severity::warning,
                               "Preset is incompatible with the supplied context", "/candidate"});
    return detail::ResultAccess::success(PresetCompatibility{compatible, std::move(diagnostics)});
}

} // namespace libslicer::v1

namespace libslicer::v1::detail {
namespace {

using Json = nlohmann::json;

std::string kind_token(PresetKind kind)
{
    switch (kind) {
    case PresetKind::printer: return "printer";
    case PresetKind::process: return "process";
    case PresetKind::filament: return "filament";
    }
    throw Slic3r::ConfigurationError("Unknown preset kind");
}

PresetKind parse_kind(const std::string &value)
{
    if (value == "printer") return PresetKind::printer;
    if (value == "process") return PresetKind::process;
    if (value == "filament") return PresetKind::filament;
    throw Slic3r::ConfigurationError("Invalid user preset kind");
}

PresetOrigin parse_origin(const std::string &value)
{
    if (value == "system") return PresetOrigin::system;
    if (value == "vendor") return PresetOrigin::vendor;
    if (value == "user") return PresetOrigin::user;
    throw Slic3r::ConfigurationError("Invalid user preset parent origin");
}

std::string origin_token(PresetOrigin origin)
{
    switch (origin) {
    case PresetOrigin::system: return "system";
    case PresetOrigin::vendor: return "vendor";
    case PresetOrigin::user: return "user";
    case PresetOrigin::project_embedded: break;
    }
    throw Slic3r::ConfigurationError("Project preset cannot be a repository parent");
}

PresetRef make_ref(PresetKind kind, PresetOrigin origin, std::string id)
{
    if (origin == PresetOrigin::system) return PresetRef::system(kind, std::move(id));
    if (origin == PresetOrigin::vendor) return PresetRef::vendor(kind, std::move(id));
    if (origin == PresetOrigin::user) return PresetRef::user(kind, std::move(id));
    throw Slic3r::ConfigurationError("Invalid repository preset origin");
}

Slic3r::Preset::Type core_kind(PresetKind kind)
{
    switch (kind) {
    case PresetKind::printer: return Slic3r::Preset::TYPE_PRINTER;
    case PresetKind::process: return Slic3r::Preset::TYPE_PRINT;
    case PresetKind::filament: return Slic3r::Preset::TYPE_FILAMENT;
    }
    return Slic3r::Preset::TYPE_INVALID;
}

std::filesystem::path manifest_path(const PresetCatalogState &catalog)
{
    return catalog.user_store / "manifest.json";
}

class UserStoreLock {
public:
    explicit UserStoreLock(const std::filesystem::path &store)
    {
        std::filesystem::create_directories(store);
        const auto path = store / ".transaction.lock";
#ifdef _WIN32
        handle_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw Slic3r::RuntimeError("Unable to open user preset transaction lock");
        OVERLAPPED overlapped{};
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw Slic3r::RuntimeError("Unable to acquire user preset transaction lock");
        }
#else
        descriptor_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (descriptor_ < 0)
            throw Slic3r::RuntimeError("Unable to open user preset transaction lock");
        if (::flock(descriptor_, LOCK_EX) != 0) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw Slic3r::RuntimeError("Unable to acquire user preset transaction lock");
        }
#endif
    }

    ~UserStoreLock()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped{};
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

    UserStoreLock(const UserStoreLock &) = delete;
    UserStoreLock &operator=(const UserStoreLock &) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ {INVALID_HANDLE_VALUE};
#else
    int descriptor_ {-1};
#endif
};

std::string read_file_bytes(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Slic3r::ConfigurationError("Unable to read user preset storage content");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path checked_payload_path(const PresetCatalogState &catalog,
                                           const std::string &payload)
{
    const std::filesystem::path relative(payload);
    if (relative.empty() || relative.is_absolute() || relative.has_parent_path() ||
        relative.filename() != relative)
        throw Slic3r::ConfigurationError("Invalid user preset payload path");
    return catalog.user_store / "content" / relative;
}

std::string fingerprint_bytes(const PresetCatalogState &catalog,
                              bool manifest_present, const std::string &manifest_bytes)
{
    std::string bytes;
    if (!manifest_present) {
        bytes = "manifest:absent";
    } else {
        bytes = "manifest:" + std::to_string(manifest_bytes.size()) + ':' + manifest_bytes;
        try {
            const Json manifest = Json::parse(manifest_bytes);
            if (manifest.contains("presets") && manifest.at("presets").is_array()) {
                for (const auto &entry : manifest.at("presets")) {
                    const std::string payload = entry.at("payload").get<std::string>();
                    const auto payload_path = checked_payload_path(catalog, payload);
                    bytes += "\npayload:" + std::to_string(payload.size()) + ':' + payload;
                    if (std::filesystem::exists(payload_path)) {
                        const std::string payload_bytes = read_file_bytes(payload_path);
                        bytes += ":present:" + std::to_string(payload_bytes.size()) + ':' + payload_bytes;
                    } else {
                        bytes += ":absent";
                    }
                }
            }
        } catch (...) {
            // The exact manifest bytes already make a malformed external replacement conflict
            // with every editor opened from a valid catalog snapshot.
        }
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest);
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned char byte : digest) stream << std::setw(2) << static_cast<unsigned>(byte);
    return stream.str();
}

std::string storage_fingerprint(const PresetCatalogState &catalog)
{
    const auto path = manifest_path(catalog);
    return std::filesystem::exists(path)
        ? fingerprint_bytes(catalog, true, read_file_bytes(path))
        : fingerprint_bytes(catalog, false, {});
}

std::string storage_fingerprint_for_manifest(const PresetCatalogState &catalog,
                                             const Json &manifest)
{
    return fingerprint_bytes(catalog, true, manifest.dump(2));
}

Json read_manifest(const PresetCatalogState &catalog)
{
    const auto path = manifest_path(catalog);
    if (!std::filesystem::exists(path))
        return Json{{"format_version", 1}, {"generation", 0}, {"presets", Json::array()}};
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Slic3r::ConfigurationError("Unable to read user preset manifest");
    Json manifest = Json::parse(input);
    if (manifest.value("format_version", 0) != 1 || !manifest.contains("presets") ||
        !manifest.at("presets").is_array())
        throw Slic3r::ConfigurationError("Invalid user preset manifest");
    return manifest;
}

std::string payload_name(PresetKind kind, const std::string &id, std::uint64_t generation)
{
    std::ostringstream stream;
    stream << kind_token(kind) << '-' << generation << '-' << std::hex
           << std::hash<std::string>{}(id) << ".json";
    return stream.str();
}

void replace_file(const std::filesystem::path &source, const std::filesystem::path &target)
{
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (!error) return;
#ifdef _WIN32
    std::filesystem::remove(target, error);
    error.clear();
    std::filesystem::rename(source, target, error);
#endif
    if (error) throw std::filesystem::filesystem_error("Unable to publish preset manifest",
        source, target, error);
}

void flush_file(const std::filesystem::path &path)
{
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE || !FlushFileBuffers(handle)) {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        throw Slic3r::RuntimeError("Unable to flush staged user preset file");
    }
    CloseHandle(handle);
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0 || ::fsync(descriptor) != 0) {
        if (descriptor >= 0) ::close(descriptor);
        throw Slic3r::RuntimeError("Unable to flush staged user preset file");
    }
    ::close(descriptor);
#endif
}

bool flush_directory(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool succeeded = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return succeeded;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor < 0) return false;
    const bool succeeded = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return succeeded;
#endif
}

void write_manifest(PresetCatalogState &catalog, const Json &manifest)
{
    std::filesystem::create_directories(catalog.user_store / "content");
    const auto staged = catalog.user_store / "manifest.json.staging";
    try {
        {
            std::ofstream output(staged, std::ios::binary | std::ios::trunc);
            if (!output) throw Slic3r::RuntimeError("Unable to stage user preset manifest");
            output << manifest.dump(2);
            output.flush();
            if (!output) throw Slic3r::RuntimeError("Unable to flush user preset manifest");
        }
        flush_file(staged);
        replace_file(staged, manifest_path(catalog));
    } catch (...) {
        std::error_code cleanup;
        std::filesystem::remove(staged, cleanup);
        throw;
    }
}

bool reclaim_unreferenced_content(const PresetCatalogState &catalog, const Json &manifest) noexcept
{
    try {
        std::set<std::string> referenced;
        for (const auto &entry : manifest.at("presets"))
            referenced.insert(entry.at("payload").get<std::string>());
        const auto content = catalog.user_store / "content";
        if (!std::filesystem::exists(content)) return true;
        bool succeeded = true;
        for (const auto &entry : std::filesystem::directory_iterator(content)) {
            const std::string name = entry.path().filename().string();
            if (entry.is_regular_file() && referenced.count(name) == 0) {
                std::error_code error;
                if (!std::filesystem::remove(entry.path(), error) || error) succeeded = false;
            }
        }
        return succeeded;
    } catch (...) {
        return false;
    }
}

Json manifest_entry(const PresetRecord &record, const std::string &payload)
{
    Json entry{{"kind", kind_token(record.summary.ref.kind())},
               {"id", record.summary.ref.id()}, {"display_name", record.summary.name},
               {"payload", payload}};
    if (record.summary.parent) {
        entry["parent"] = {{"kind", kind_token(record.summary.parent->ref.kind())},
                           {"origin", origin_token(record.summary.parent->ref.origin())},
                           {"id", record.summary.parent->ref.id()}};
    }
    return entry;
}

void load_user_manifest(PresetCatalogState &catalog)
{
    Json manifest = read_manifest(catalog);
    catalog.storage_generation = manifest.value("generation", std::uint64_t{0});
    std::vector<Json> pending;
    for (const auto &entry : manifest.at("presets")) pending.push_back(entry);
    while (!pending.empty()) {
        bool progressed = false;
        for (auto it = pending.begin(); it != pending.end();) {
            const PresetKind kind = parse_kind(it->at("kind").get<std::string>());
            const std::string id = it->at("id").get<std::string>();
            std::optional<SelectedPreset> parent;
            const PresetRecord *parent_record = nullptr;
            if (it->contains("parent")) {
                const auto &stored = it->at("parent");
                PresetRef parent_ref = make_ref(parse_kind(stored.at("kind").get<std::string>()),
                                                parse_origin(stored.at("origin").get<std::string>()),
                                                stored.at("id").get<std::string>());
                const auto found = catalog.records.find(
                    PresetKey{parent_ref.kind(), parent_ref.origin(), parent_ref.id()});
                if (found == catalog.records.end()) { ++it; continue; }
                parent_record = &found->second;
                parent = SelectedPreset{parent_ref, found->second.summary.revision};
            }

            Slic3r::DynamicPrintConfig overrides_core;
            std::map<std::string, std::string> key_values;
            std::string reason;
            const auto payload = checked_payload_path(
                catalog, it->at("payload").get<std::string>());
            overrides_core.load_from_json(
                payload.string(), Slic3r::ForwardCompatibilitySubstitutionRule::Disable,
                key_values, reason);
            Slic3r::DynamicPrintConfig effective_core = parent_record && parent_record->core
                ? parent_record->core->config : default_core_config(catalog, kind);
            effective_core.apply(overrides_core);
            auto inherited = parent_record ? ResultAccess::success(parent_record->effective)
                                           : core_config_to_values(default_core_config(catalog, kind));
            auto effective = core_config_to_values(effective_core);
            auto overrides = core_config_to_values(overrides_core);
            if (!inherited.has_value() || !effective.has_value() || !overrides.has_value())
                throw Slic3r::ConfigurationError("Unable to decode user preset values");
            ConfigPatch patch;
            for (const auto &value : overrides.value().entries()) patch.set(value.option, value.value);
            PresetRef ref = PresetRef::user(kind, id);
            if (catalog.records.count(PresetKey{kind, PresetOrigin::user, id}))
                throw Slic3r::ConfigurationError("Duplicate user preset identity");
            const PresetRevision revision = catalog.next_revision++;
            PresetSummary summary{ref, it->value("display_name", id), {}, parent, revision};
            auto core = std::make_shared<Slic3r::Preset>(core_kind(kind), id);
            core->config = std::move(effective_core);
            core->file = payload.string();
            catalog.records.emplace(PresetKey{kind, PresetOrigin::user, id}, PresetRecord{
                summary, std::move(inherited).value(), std::move(effective).value(),
                std::move(patch), std::move(core)});
            it = pending.erase(it);
            progressed = true;
        }
        if (!progressed)
            throw Slic3r::ConfigurationError("User preset parent chain is missing or cyclic");
    }
    catalog.storage_fingerprint = storage_fingerprint(catalog);
}

PresetKind public_kind(Slic3r::Preset::Type type)
{
    switch (type) {
    case Slic3r::Preset::TYPE_PRINTER: return PresetKind::printer;
    case Slic3r::Preset::TYPE_PRINT: return PresetKind::process;
    case Slic3r::Preset::TYPE_FILAMENT: return PresetKind::filament;
    default: throw Slic3r::ConfigurationError("Unsupported preset kind");
    }
}

PresetOrigin public_origin(const Slic3r::Preset &preset)
{
    if (preset.is_project_embedded) return PresetOrigin::project_embedded;
    if (preset.is_user()) return PresetOrigin::user;
    if (preset.vendor || preset.is_from_bundle()) return PresetOrigin::vendor;
    return PresetOrigin::system;
}

PresetRef public_ref(const Slic3r::Preset &preset)
{
    const auto kind = public_kind(preset.type);
    switch (public_origin(preset)) {
    case PresetOrigin::system: return PresetRef::system(kind, preset.name);
    case PresetOrigin::vendor: return PresetRef::vendor(kind, preset.name);
    case PresetOrigin::user: return PresetRef::user(kind, preset.name);
    case PresetOrigin::project_embedded: break;
    }
    throw Slic3r::ConfigurationError("Project preset cannot enter repository catalog");
}

enum class RecordFailureStage {
    freeze,
    parent_default,
    effective_conversion,
    identity,
    duplicate,
    dependency_conversion
};

struct RecordFailure {
    RecordFailureStage stage;
    ErrorCode          code;
    std::string        message;
    std::string        field;
};

struct FrozenPresetRecordInput {
    std::size_t ordinal;
    const Slic3r::Preset *source {nullptr};
    const Slic3r::DynamicPrintConfig *effective_core {nullptr};
    const Slic3r::DynamicPrintConfig *baseline_core {nullptr};
    std::optional<std::size_t> parent_ordinal;
    std::optional<std::size_t> default_owner_ordinal;
    PresetRevision revision {0};
    std::optional<PresetRef> ref;
    std::optional<SelectedPreset> parent_summary;
    std::optional<PresetKey> key;
    std::string name;
    std::string vendor;
};

struct EffectiveArtifactSlot {
    std::optional<ConfigValues> effective;
    std::optional<ConfigValues> default_values;
    std::optional<RecordFailure> effective_failure;
    std::optional<RecordFailure> default_failure;
};

struct RecordCandidateStatus {
    std::optional<RecordFailure> failure;
    std::optional<ConfigPatch> overrides;
    std::optional<PresetRecord> candidate;
};

int failure_priority(RecordFailureStage stage)
{
    switch (stage) {
    case RecordFailureStage::freeze: return 0;
    case RecordFailureStage::parent_default: return 1;
    case RecordFailureStage::effective_conversion: return 2;
    case RecordFailureStage::dependency_conversion: return 3;
    case RecordFailureStage::identity: return 4;
    case RecordFailureStage::duplicate: return 5;
    }
    return std::numeric_limits<int>::max();
}

void record_failure(RecordCandidateStatus &status, RecordFailure failure)
{
    if (!status.failure ||
        failure_priority(failure.stage) < failure_priority(status.failure->stage))
        status.failure = std::move(failure);
}

RecordFailure conversion_failure(RecordFailureStage stage,
                                 const std::string &preset_name,
                                 const Result<FusedConfigConversion> &result)
{
    const Diagnostic &diagnostic = result.diagnostics().front();
    return {stage,
            result.error_code().value_or(ErrorCode::invalid_configuration),
            "Unable to convert preset configuration '" + preset_name +
                "': " + diagnostic.message,
            "/resources_dir"};
}

RecordFailure values_conversion_failure(const std::string &preset_name,
                                        const Result<ConfigValues> &result)
{
    const Diagnostic &diagnostic = result.diagnostics().front();
    return {RecordFailureStage::parent_default,
            result.error_code().value_or(ErrorCode::invalid_configuration),
            "Unable to convert default preset configuration for '" +
                preset_name + "': " + diagnostic.message,
            "/resources_dir"};
}

#ifdef LIBSLICER_SDK_TESTING

std::mutex &record_scheduler_test_mutex()
{
    static std::mutex mutex;
    return mutex;
}

RecordSchedulerTestControl &record_scheduler_test_control_storage()
{
    static RecordSchedulerTestControl control;
    return control;
}

RecordSchedulerTestStats &record_scheduler_test_stats_storage()
{
    static RecordSchedulerTestStats stats;
    return stats;
}

RecordFailureStage public_test_stage(RecordFailureInjectionStage stage)
{
    switch (stage) {
    case RecordFailureInjectionStage::freeze: return RecordFailureStage::freeze;
    case RecordFailureInjectionStage::parent_default:
        return RecordFailureStage::parent_default;
    case RecordFailureInjectionStage::identity: return RecordFailureStage::identity;
    case RecordFailureInjectionStage::duplicate: return RecordFailureStage::duplicate;
    case RecordFailureInjectionStage::effective_conversion:
        return RecordFailureStage::effective_conversion;
    }
    return RecordFailureStage::freeze;
}

std::optional<RecordFailureInjectionStage> injected_stage(
    const RecordSchedulerTestControl &control, std::size_t ordinal)
{
    const auto found = std::find_if(
        control.failures.begin(), control.failures.end(),
        [ordinal](const RecordFailureInjection &failure) {
            return failure.ordinal == ordinal;
        });
    return found == control.failures.end()
        ? std::nullopt
        : std::optional<RecordFailureInjectionStage>{found->stage};
}

#endif

struct RecordSchedulerSettings {
    std::size_t worker_limit {4};
#ifdef LIBSLICER_SDK_TESTING
    RecordSchedulerTestControl test;
#endif
};

RecordSchedulerSettings record_scheduler_settings()
{
    RecordSchedulerSettings settings;
#ifdef LIBSLICER_SDK_TESTING
    std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
    settings.test = record_scheduler_test_control_storage();
    if (settings.test.worker_limit == 1 || settings.test.worker_limit == 2 ||
        settings.test.worker_limit == 4)
        settings.worker_limit = settings.test.worker_limit;
    record_scheduler_test_stats_storage().configured_worker_limit =
        settings.worker_limit;
    record_scheduler_test_stats_storage().system_loader_quiescent_before_arena =
        true;
#endif
    return settings;
}

bool inject_failure(const RecordSchedulerSettings &settings,
                    std::size_t ordinal, RecordFailureStage stage)
{
#ifdef LIBSLICER_SDK_TESTING
    const auto injected = injected_stage(settings.test, ordinal);
    return injected && public_test_stage(*injected) == stage;
#else
    (void)settings;
    (void)ordinal;
    (void)stage;
    return false;
#endif
}

void update_peak(std::atomic<std::size_t> &peak, std::size_t value)
{
    std::size_t current = peak.load(std::memory_order_relaxed);
    while (current < value &&
           !peak.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed))
        ;
}

template<class Function>
void run_record_batch(tbb::task_arena &arena, std::size_t count,
                      std::atomic<std::size_t> &active,
                      std::atomic<std::size_t> &peak,
                      Function function)
{
#ifdef LIBSLICER_SDK_TESTING
    {
        std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
        ++record_scheduler_test_stats_storage().dependency_batches_started;
    }
#endif
    arena.execute([&] {
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, count),
            [&](const tbb::blocked_range<std::size_t> &range) {
                for (std::size_t index = range.begin(); index < range.end(); ++index) {
                    const std::size_t now =
                        active.fetch_add(1, std::memory_order_relaxed) + 1;
                    update_peak(peak, now);
                    try {
                        function(index);
                    } catch (...) {
                        active.fetch_sub(1, std::memory_order_relaxed);
                        throw;
                    }
                    active.fetch_sub(1, std::memory_order_relaxed);
                }
            });
    });
#ifdef LIBSLICER_SDK_TESTING
    {
        std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
        ++record_scheduler_test_stats_storage().dependency_batches_joined;
    }
#endif
}

void freeze_collection(
    const Slic3r::PresetCollection &collection,
    PresetRevision &next_revision,
    std::vector<FrozenPresetRecordInput> &inputs,
    std::vector<RecordCandidateStatus> &statuses,
    std::map<const Slic3r::DynamicPrintConfig *, std::size_t> &default_owners,
    std::set<PresetKey> &identities,
    const RecordSchedulerSettings &settings)
{
    const std::size_t first_ordinal = inputs.size();
    std::map<const Slic3r::Preset *, std::size_t> ordinals;
    for (const Slic3r::Preset &preset : collection.get_presets()) {
        if (preset.is_project_embedded || preset.printer_technology() == Slic3r::ptSLA)
            continue;
        const std::size_t ordinal = inputs.size();
        ordinals.emplace(&preset, ordinal);
        inputs.push_back({ordinal, &preset, &preset.config, nullptr,
                          std::nullopt, std::nullopt, next_revision++,
                          std::nullopt, std::nullopt, std::nullopt,
                          preset.name,
                          preset.vendor ? preset.vendor->name : std::string{}});
        statuses.emplace_back();
    }

    for (std::size_t ordinal = first_ordinal; ordinal < inputs.size(); ++ordinal) {
        FrozenPresetRecordInput &input = inputs[ordinal];
        RecordCandidateStatus &status = statuses[ordinal];
        if (inject_failure(settings, ordinal,
                           RecordFailureStage::freeze)) {
            record_failure(status, {RecordFailureStage::freeze,
                                    ErrorCode::invalid_configuration,
                                    "Injected record freeze failure",
                                    "/resources_dir"});
        }

        const Slic3r::Preset *parent = nullptr;
        try {
            parent = collection.get_preset_parent(*input.source);
            if (parent && !ordinals.count(parent)) parent = nullptr;
            if (parent) {
                input.parent_ordinal = ordinals.at(parent);
                input.baseline_core = &parent->config;
                input.parent_summary = SelectedPreset{
                    public_ref(*parent),
                    inputs[*input.parent_ordinal].revision};
#ifdef LIBSLICER_SDK_TESTING
                if (ordinal < *input.parent_ordinal) {
                    std::lock_guard<std::mutex> lock(
                        record_scheduler_test_mutex());
                    RecordSchedulerTestStats &stats =
                        record_scheduler_test_stats_storage();
                    if (!stats.reverse_dependency_child_ordinal) {
                        stats.reverse_dependency_child_ordinal = ordinal;
                        stats.reverse_dependency_parent_ordinal =
                            *input.parent_ordinal;
                    }
                }
#endif
            } else {
                const Slic3r::Preset &authority =
                    collection.default_preset_for(input.source->config);
                input.baseline_core = &authority.config;
                const auto inserted =
                    default_owners.emplace(input.baseline_core, ordinal);
                input.default_owner_ordinal = inserted.first->second;
            }
            if (inject_failure(settings, ordinal,
                               RecordFailureStage::parent_default)) {
                record_failure(status, {RecordFailureStage::parent_default,
                                        ErrorCode::invalid_configuration,
                                        "Injected parent/default freeze failure",
                                        "/resources_dir"});
            }
        } catch (const std::exception &error) {
            record_failure(status, {RecordFailureStage::parent_default,
                                    ErrorCode::invalid_configuration,
                                    error.what(), "/resources_dir"});
        }

        try {
            input.ref = public_ref(*input.source);
            input.key = PresetKey{input.ref->kind(), input.ref->origin(),
                                  input.ref->id()};
            if (inject_failure(settings, ordinal,
                               RecordFailureStage::identity)) {
                record_failure(status, {RecordFailureStage::identity,
                                        ErrorCode::invalid_configuration,
                                        "Injected preset identity failure",
                                        "/resources_dir"});
            }
            if (!identities.insert(*input.key).second ||
                inject_failure(settings, ordinal,
                               RecordFailureStage::duplicate)) {
                record_failure(status, {
                    RecordFailureStage::duplicate,
                    ErrorCode::invalid_configuration,
                    "Duplicate preset identity: " + input.ref->id(),
                    "/resources_dir"});
            }
        } catch (const std::exception &error) {
            record_failure(status, {RecordFailureStage::identity,
                                    ErrorCode::invalid_configuration,
                                    error.what(), "/resources_dir"});
        }
    }
}

Result<void> append_system_collections(PresetCatalogState &catalog)
{
    const RecordSchedulerSettings settings = record_scheduler_settings();
#ifdef LIBSLICER_SDK_TESTING
    const auto freeze_started = std::chrono::steady_clock::now();
#endif
    std::vector<FrozenPresetRecordInput> inputs;
    std::vector<RecordCandidateStatus> statuses;
    std::map<const Slic3r::DynamicPrintConfig *, std::size_t> default_owners;
    std::set<PresetKey> identities;
    PresetRevision next_revision = catalog.next_revision;

    freeze_collection(catalog.bundle->printers, next_revision, inputs, statuses,
                      default_owners, identities, settings);
    freeze_collection(catalog.bundle->prints, next_revision, inputs, statuses,
                      default_owners, identities, settings);
    freeze_collection(catalog.bundle->filaments, next_revision, inputs, statuses,
                      default_owners, identities, settings);
#ifdef LIBSLICER_SDK_TESTING
    {
        std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
        record_scheduler_test_stats_storage().freeze_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - freeze_started).count());
    }
#endif

    if (inputs.empty())
        return ResultAccess::success();

    std::vector<EffectiveArtifactSlot> artifacts(inputs.size());
    std::vector<std::size_t> default_owner_ordinals;
    default_owner_ordinals.reserve(default_owners.size());
    for (const auto &entry : default_owners)
        default_owner_ordinals.push_back(entry.second);
    std::sort(default_owner_ordinals.begin(), default_owner_ordinals.end());

    const std::size_t arena_limit =
        std::min(settings.worker_limit, inputs.size());
    std::atomic<std::size_t> active_workers {0};
    std::atomic<std::size_t> peak_workers {0};
    std::atomic<std::size_t> in_flight_candidates {0};
    std::atomic<std::size_t> peak_in_flight_candidates {0};

#ifdef LIBSLICER_SDK_TESTING
    {
        std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
        record_scheduler_test_stats_storage().arena_max_concurrency = arena_limit;
    }
#endif

    try {
        {
            tbb::task_arena arena(static_cast<int>(arena_limit));
#ifdef LIBSLICER_SDK_TESTING
            const auto default_started = std::chrono::steady_clock::now();
#endif
            run_record_batch(
                arena, default_owner_ordinals.size(), active_workers, peak_workers,
                [&](std::size_t batch_index) {
                    const std::size_t ordinal =
                        default_owner_ordinals[batch_index];
                    const FrozenPresetRecordInput &input = inputs[ordinal];
                    EffectiveArtifactSlot &artifact = artifacts[ordinal];
                    try {
                        if (!input.baseline_core) {
                            artifact.default_failure = RecordFailure{
                                RecordFailureStage::parent_default,
                                ErrorCode::invalid_configuration,
                                "Default preset configuration is unavailable for '" +
                                    input.name + "'",
                                "/resources_dir"};
                            return;
                        }
                        auto converted =
                            core_config_to_values(*input.baseline_core);
                        if (!converted.has_value()) {
                            artifact.default_failure =
                                values_conversion_failure(input.name, converted);
                            return;
                        }
                        artifact.default_values =
                            std::move(converted).value();
#ifdef LIBSLICER_SDK_TESTING
                        std::lock_guard<std::mutex> lock(
                            record_scheduler_test_mutex());
                        ++record_scheduler_test_stats_storage().default_conversions;
#endif
                    } catch (const std::exception &error) {
                        artifact.default_failure = RecordFailure{
                            RecordFailureStage::parent_default,
                            ErrorCode::internal, error.what(), "/resources_dir"};
                    } catch (...) {
                        artifact.default_failure = RecordFailure{
                            RecordFailureStage::parent_default,
                            ErrorCode::internal,
                            "Unknown default conversion worker failure",
                            "/resources_dir"};
                    }
                });
#ifdef LIBSLICER_SDK_TESTING
            {
                std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
                record_scheduler_test_stats_storage().default_batch_ns =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            default_started).count());
            }
            const auto effective_started = std::chrono::steady_clock::now();
#endif

            run_record_batch(
                arena, inputs.size(), active_workers, peak_workers,
                [&](std::size_t ordinal) {
                    const FrozenPresetRecordInput &input = inputs[ordinal];
                    EffectiveArtifactSlot &artifact = artifacts[ordinal];
                    try {
#ifdef LIBSLICER_SDK_TESTING
                        {
                            std::lock_guard<std::mutex> lock(
                                record_scheduler_test_mutex());
                            ++record_scheduler_test_stats_storage()
                                  .effective_artifacts_attempted;
                        }
#endif
                        if (inject_failure(
                                settings, ordinal,
                                RecordFailureStage::effective_conversion)) {
                            artifact.effective_failure = RecordFailure{
                                RecordFailureStage::effective_conversion,
                                ErrorCode::invalid_configuration,
                                "Injected effective conversion failure",
                                "/resources_dir"};
                            return;
                        }
                        if (!input.effective_core || !input.baseline_core) {
                            artifact.effective_failure = RecordFailure{
                                RecordFailureStage::effective_conversion,
                                ErrorCode::invalid_configuration,
                                "Frozen preset conversion input is incomplete",
                                "/resources_dir"};
                            return;
                        }
                        auto converted = core_config_to_values_and_diff(
                            *input.baseline_core, *input.effective_core,
                            catalog.schema);
                        if (!converted.has_value()) {
                            artifact.effective_failure = conversion_failure(
                                RecordFailureStage::effective_conversion,
                                input.name, converted);
                            return;
                        }
                        FusedConfigConversion fused =
                            std::move(converted).value();
                        artifact.effective = std::move(fused.effective);
                        statuses[ordinal].overrides =
                            std::move(fused.overrides);
                    } catch (const std::exception &error) {
                        artifact.effective_failure = RecordFailure{
                            RecordFailureStage::effective_conversion,
                            ErrorCode::internal, error.what(), "/resources_dir"};
                    } catch (...) {
                        artifact.effective_failure = RecordFailure{
                            RecordFailureStage::effective_conversion,
                            ErrorCode::internal,
                            "Unknown effective conversion worker failure",
                            "/resources_dir"};
                    }
                });
#ifdef LIBSLICER_SDK_TESTING
            {
                std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
                record_scheduler_test_stats_storage().effective_batch_ns =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            effective_started).count());
            }
            const auto candidate_started = std::chrono::steady_clock::now();
#endif

            run_record_batch(
                arena, inputs.size(), active_workers, peak_workers,
                [&](std::size_t ordinal) {
                    FrozenPresetRecordInput &input = inputs[ordinal];
                    EffectiveArtifactSlot &artifact = artifacts[ordinal];
                    RecordCandidateStatus &status = statuses[ordinal];
                    bool candidate_in_flight = false;
                    try {
#ifdef LIBSLICER_SDK_TESTING
                        {
                            std::lock_guard<std::mutex> lock(
                                record_scheduler_test_mutex());
                            ++record_scheduler_test_stats_storage()
                                  .candidates_attempted;
                        }
#endif
                        if (artifact.effective_failure)
                            record_failure(status, *artifact.effective_failure);

                        std::optional<ConfigValues> inherited;
                        if (input.parent_ordinal) {
                            const EffectiveArtifactSlot &parent =
                                artifacts[*input.parent_ordinal];
                            if (parent.effective_failure || !parent.effective) {
                                record_failure(status, {
                                    RecordFailureStage::dependency_conversion,
                                    ErrorCode::invalid_configuration,
                                    "Parent effective configuration conversion failed for '" +
                                        input.name + "'",
                                    "/resources_dir"});
#ifdef LIBSLICER_SDK_TESTING
                                std::lock_guard<std::mutex> lock(
                                    record_scheduler_test_mutex());
                                ++record_scheduler_test_stats_storage()
                                      .children_blocked_by_parent_effective_failure;
#endif
                            } else {
                                inherited = *parent.effective;
#ifdef LIBSLICER_SDK_TESTING
                                std::lock_guard<std::mutex> lock(
                                    record_scheduler_test_mutex());
                                ++record_scheduler_test_stats_storage()
                                      .parent_effective_shares;
#endif
                            }
                        } else if (input.default_owner_ordinal) {
                            const EffectiveArtifactSlot &owner =
                                artifacts[*input.default_owner_ordinal];
                            if (owner.default_failure || !owner.default_values) {
                                record_failure(status, owner.default_failure.value_or(
                                    RecordFailure{
                                        RecordFailureStage::parent_default,
                                        ErrorCode::invalid_configuration,
                                        "Default preset configuration conversion failed for '" +
                                            input.name + "'",
                                        "/resources_dir"}));
                            } else {
                                inherited = *owner.default_values;
#ifdef LIBSLICER_SDK_TESTING
                                std::lock_guard<std::mutex> lock(
                                    record_scheduler_test_mutex());
                                ++record_scheduler_test_stats_storage()
                                      .default_value_shares;
#endif
                            }
                        } else {
                            record_failure(status, {
                                RecordFailureStage::parent_default,
                                ErrorCode::invalid_configuration,
                                "Preset inheritance authority is unavailable for '" +
                                    input.name + "'",
                                "/resources_dir"});
                        }

                        if (status.failure || !inherited || !artifact.effective ||
                            !status.overrides || !input.ref || !input.key)
                            return;

                        const std::size_t in_flight =
                            in_flight_candidates.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                        candidate_in_flight = true;
                        update_peak(peak_in_flight_candidates, in_flight);
                        PresetSummary summary{
                            *input.ref, input.name, input.vendor,
                            input.parent_summary, input.revision};
                        status.candidate.emplace(PresetRecord{
                            std::move(summary), *inherited,
                            *artifact.effective,
                            std::move(*status.overrides),
                            std::make_shared<Slic3r::Preset>(*input.source)});
                        status.overrides.reset();
#ifdef LIBSLICER_SDK_TESTING
                        if (input.parent_ordinal &&
                            statuses[*input.parent_ordinal].failure) {
                            std::lock_guard<std::mutex> lock(
                                record_scheduler_test_mutex());
                            ++record_scheduler_test_stats_storage()
                                  .children_completed_from_failed_parent_record;
                        }
#endif
                        in_flight_candidates.fetch_sub(
                            1, std::memory_order_relaxed);
                        candidate_in_flight = false;
#ifdef LIBSLICER_SDK_TESTING
                        if (status.candidate) {
                            std::lock_guard<std::mutex> lock(
                                record_scheduler_test_mutex());
                            ++record_scheduler_test_stats_storage()
                                  .candidates_staged;
                        }
#endif
                    } catch (const std::exception &error) {
                        if (candidate_in_flight)
                            in_flight_candidates.fetch_sub(
                                1, std::memory_order_relaxed);
                        record_failure(status, {
                            RecordFailureStage::effective_conversion,
                            ErrorCode::internal, error.what(), "/resources_dir"});
                    } catch (...) {
                        if (candidate_in_flight)
                            in_flight_candidates.fetch_sub(
                                1, std::memory_order_relaxed);
                        record_failure(status, {
                            RecordFailureStage::effective_conversion,
                            ErrorCode::internal,
                            "Unknown candidate construction worker failure",
                            "/resources_dir"});
                    }
                });
#ifdef LIBSLICER_SDK_TESTING
            {
                std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
                record_scheduler_test_stats_storage().candidate_batch_ns =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            candidate_started).count());
            }
#endif
        }
#ifdef LIBSLICER_SDK_TESTING
        {
            std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
            RecordSchedulerTestStats &stats =
                record_scheduler_test_stats_storage();
            stats.arena_destroyed_before_commit = true;
            stats.maximum_active_workers =
                peak_workers.load(std::memory_order_relaxed);
            stats.in_flight_candidates =
                in_flight_candidates.load(std::memory_order_relaxed);
            stats.peak_in_flight_candidates =
                peak_in_flight_candidates.load(std::memory_order_relaxed);
        }
#endif
    } catch (const std::exception &error) {
        return ResultAccess::failure(
            ErrorCode::internal, error.what(), "/runtime/record_arena");
    } catch (...) {
        return ResultAccess::failure(
            ErrorCode::internal, "Unknown record arena failure",
            "/runtime/record_arena");
    }

#ifdef LIBSLICER_SDK_TESTING
    const auto commit_started = std::chrono::steady_clock::now();
#endif
    for (std::size_t ordinal = 0; ordinal < statuses.size(); ++ordinal) {
        if (!statuses[ordinal].failure)
            continue;
#ifdef LIBSLICER_SDK_TESTING
        {
            std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
            RecordSchedulerTestStats &stats =
                record_scheduler_test_stats_storage();
            stats.primary_failure_ordinal = ordinal;
            switch (statuses[ordinal].failure->stage) {
            case RecordFailureStage::freeze:
                stats.primary_failure_stage =
                    RecordFailureInjectionStage::freeze;
                break;
            case RecordFailureStage::parent_default:
            case RecordFailureStage::dependency_conversion:
                stats.primary_failure_stage =
                    RecordFailureInjectionStage::parent_default;
                break;
            case RecordFailureStage::identity:
                stats.primary_failure_stage =
                    RecordFailureInjectionStage::identity;
                break;
            case RecordFailureStage::duplicate:
                stats.primary_failure_stage =
                    RecordFailureInjectionStage::duplicate;
                break;
            case RecordFailureStage::effective_conversion:
                stats.primary_failure_stage =
                    RecordFailureInjectionStage::effective_conversion;
                break;
            }
        }
#endif
        const RecordFailure &failure = *statuses[ordinal].failure;
        for (RecordCandidateStatus &status : statuses)
            status.overrides.reset();
        for (RecordCandidateStatus &status : statuses)
            status.candidate.reset();
        for (EffectiveArtifactSlot &artifact : artifacts) {
            artifact.effective.reset();
            artifact.default_values.reset();
        }
#ifdef LIBSLICER_SDK_TESTING
        {
            std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
            record_scheduler_test_stats_storage()
                .status_candidate_owners_after_commit = 0;
            record_scheduler_test_stats_storage()
                .artifact_value_owners_after_commit = 0;
        }
#endif
        return ResultAccess::failure(
            failure.code, failure.message, failure.field);
    }

    if (!catalog.records.empty())
        return ResultAccess::failure(
            ErrorCode::internal,
            "System preset catalog was published before record commit",
            "/resources_dir");

    for (std::size_t ordinal = 0; ordinal < statuses.size(); ++ordinal) {
        if (!statuses[ordinal].candidate || !inputs[ordinal].key)
            return ResultAccess::failure(
                ErrorCode::internal,
                "Record candidate is missing at commit",
                "/resources_dir");
        const auto inserted = catalog.records.emplace(
            *inputs[ordinal].key,
            std::move(*statuses[ordinal].candidate));
        if (!inserted.second)
            return ResultAccess::failure(
                ErrorCode::invalid_configuration,
                "Duplicate preset identity at commit: " +
                    inputs[ordinal].key->id,
                "/resources_dir");
        statuses[ordinal].candidate.reset();
    }
    catalog.next_revision = next_revision;
    for (EffectiveArtifactSlot &artifact : artifacts) {
        artifact.effective.reset();
        artifact.default_values.reset();
    }

#ifdef LIBSLICER_SDK_TESTING
    {
        std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
        RecordSchedulerTestStats &stats =
            record_scheduler_test_stats_storage();
        stats.commit_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - commit_started).count());
        stats.committed_records = catalog.records.size();
        stats.status_candidate_owners_after_commit =
            static_cast<std::size_t>(std::count_if(
                statuses.begin(), statuses.end(),
                [](const RecordCandidateStatus &status) {
                    return status.candidate.has_value() ||
                           status.overrides.has_value();
                }));
        stats.artifact_value_owners_after_commit =
            static_cast<std::size_t>(std::count_if(
                artifacts.begin(), artifacts.end(),
                [](const EffectiveArtifactSlot &artifact) {
                    return artifact.effective ||
                           artifact.default_values;
                }));
    }
#endif
    return ResultAccess::success();
}

Result<void> prepare_user_store(const std::filesystem::path &user_store)
{
    std::error_code error;
    std::filesystem::create_directories(user_store / "content", error);
    if (error)
        return ResultAccess::failure(ErrorCode::io,
                                     "Unable to create user preset store: " + error.message(),
                                     "/data_dir");

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto probe = user_store / (".write-probe-" + std::to_string(nonce));
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        if (!output)
            return ResultAccess::failure(ErrorCode::io,
                                         "User preset store is not writable", "/data_dir");
        output.put('\0');
        output.flush();
        if (!output) {
            std::filesystem::remove(probe, error);
            return ResultAccess::failure(ErrorCode::io,
                                         "User preset store is not writable", "/data_dir");
        }
    }
    if (!std::filesystem::remove(probe, error) || error)
        return ResultAccess::failure(ErrorCode::io,
                                     "Unable to clean user preset store write probe", "/data_dir");
    return ResultAccess::success();
}

} // namespace

#ifdef LIBSLICER_SDK_TESTING

void set_record_scheduler_test_control(RecordSchedulerTestControl control)
{
    if (control.worker_limit != 1 && control.worker_limit != 2 &&
        control.worker_limit != 4)
        throw std::invalid_argument(
            "Record scheduler test worker limit must be 1, 2, or 4");
    std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
    record_scheduler_test_control_storage() = std::move(control);
}

void reset_record_scheduler_test_control()
{
    std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
    record_scheduler_test_control_storage() = {};
    record_scheduler_test_stats_storage() = {};
}

RecordSchedulerTestStats record_scheduler_test_stats()
{
    std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
    return record_scheduler_test_stats_storage();
}

std::uintptr_t config_values_identity_for_testing(const ConfigValues &values)
{
    return reinterpret_cast<std::uintptr_t>(
        std::addressof(values.entries()));
}

#endif

Result<std::uint64_t> persist_user_preset(PresetCatalogState &catalog,
                                          const PresetRecord &record,
                                          std::uint64_t expected_storage_generation,
                                          const std::string &expected_storage_fingerprint)
{
    try {
        UserStoreLock transaction_lock(catalog.user_store);
        if (storage_fingerprint(catalog) != expected_storage_fingerprint)
            return ResultAccess::failure<std::uint64_t>(
                ErrorCode::conflict, "User preset storage changed", "/preset/storage_revision");
        Json manifest = read_manifest(catalog);
        const std::uint64_t generation = manifest.value("generation", std::uint64_t{0});
        if (generation != expected_storage_generation)
            return ResultAccess::failure<std::uint64_t>(
                ErrorCode::conflict, "User preset storage changed", "/preset/storage_revision");

        Slic3r::DynamicPrintConfig overrides_core;
        auto applied = apply_patch_to_core_config(record.overrides, overrides_core);
        if (!applied.has_value())
            return ResultAccess::failure<std::uint64_t>(
                *applied.error_code(), applied.diagnostics().front().message,
                applied.diagnostics().front().field);

        const std::uint64_t next_generation = generation + 1;
        std::filesystem::create_directories(catalog.user_store / "content");
        const std::string payload = payload_name(record.summary.ref.kind(),
                                                 record.summary.ref.id(), next_generation);
        const auto staged_payload = catalog.user_store / "content" / (payload + ".staging");
        const auto final_payload = catalog.user_store / "content" / payload;
        overrides_core.save_to_json(staged_payload.string(), record.summary.ref.id(), "User",
                                    SLIC3R_VERSION);
        flush_file(staged_payload);
        replace_file(staged_payload, final_payload);

        Json entries = Json::array();
        for (const auto &entry : manifest.at("presets")) {
            if (entry.value("kind", "") == kind_token(record.summary.ref.kind()) &&
                entry.value("id", "") == record.summary.ref.id())
                continue;
            entries.push_back(entry);
        }
        entries.push_back(manifest_entry(record, payload));
        manifest["generation"] = next_generation;
        manifest["presets"] = std::move(entries);
        const std::string committed_fingerprint =
            storage_fingerprint_for_manifest(catalog, manifest);
        try {
            write_manifest(catalog, manifest);
        } catch (...) {
            std::error_code cleanup;
            std::filesystem::remove(final_payload, cleanup);
            throw;
        }
        catalog.storage_fingerprint = committed_fingerprint;
        const bool directory_flushed = flush_directory(catalog.user_store);
        const bool reclaimed = reclaim_unreferenced_content(catalog, manifest);
        std::vector<Diagnostic> diagnostics;
        if (!directory_flushed || !reclaimed)
            diagnostics.push_back({ErrorCode::io, Severity::warning,
                                   "Preset committed, but post-commit maintenance was incomplete",
                                   "/data_dir"});
        return ResultAccess::success(next_generation, std::move(diagnostics));
    } catch (const std::exception &error) {
        return ResultAccess::failure<std::uint64_t>(ErrorCode::io, error.what(), "/data_dir");
    }
}

Result<std::uint64_t> erase_user_preset(PresetCatalogState &catalog,
                                        const PresetKey &key,
                                        std::uint64_t expected_storage_generation,
                                        const std::string &expected_storage_fingerprint)
{
    try {
        UserStoreLock transaction_lock(catalog.user_store);
        if (storage_fingerprint(catalog) != expected_storage_fingerprint)
            return ResultAccess::failure<std::uint64_t>(
                ErrorCode::conflict, "User preset storage changed", "/preset/storage_revision");
        Json manifest = read_manifest(catalog);
        const std::uint64_t generation = manifest.value("generation", std::uint64_t{0});
        if (generation != expected_storage_generation)
            return ResultAccess::failure<std::uint64_t>(
                ErrorCode::conflict, "User preset storage changed", "/preset/storage_revision");
        Json entries = Json::array();
        bool removed = false;
        for (const auto &entry : manifest.at("presets")) {
            if (entry.value("kind", "") == kind_token(key.kind) &&
                entry.value("id", "") == key.id) {
                removed = true;
                continue;
            }
            entries.push_back(entry);
        }
        if (!removed)
            return ResultAccess::failure<std::uint64_t>(
                ErrorCode::conflict, "User preset storage changed", "/preset/storage_revision");
        const std::uint64_t next_generation = generation + 1;
        manifest["generation"] = next_generation;
        manifest["presets"] = std::move(entries);
        const std::string committed_fingerprint =
            storage_fingerprint_for_manifest(catalog, manifest);
        write_manifest(catalog, manifest);
        catalog.storage_fingerprint = committed_fingerprint;
        const bool directory_flushed = flush_directory(catalog.user_store);
        const bool reclaimed = reclaim_unreferenced_content(catalog, manifest);
        std::vector<Diagnostic> diagnostics;
        if (!directory_flushed || !reclaimed)
            diagnostics.push_back({ErrorCode::io, Severity::warning,
                                   "Preset erased, but post-commit maintenance was incomplete",
                                   "/data_dir"});
        return ResultAccess::success(next_generation, std::move(diagnostics));
    } catch (const std::exception &error) {
        return ResultAccess::failure<std::uint64_t>(ErrorCode::io, error.what(), "/data_dir");
    }
}

ErrorCode public_error_code(Slic3r::SystemPresetIssueKind kind)
{
    switch (kind) {
    case Slic3r::SystemPresetIssueKind::io:
        return ErrorCode::io;
    case Slic3r::SystemPresetIssueKind::parse:
        return ErrorCode::invalid_configuration;
    case Slic3r::SystemPresetIssueKind::duplicate:
        return ErrorCode::conflict;
    case Slic3r::SystemPresetIssueKind::alias_cycle:
        return ErrorCode::invalid_configuration;
    case Slic3r::SystemPresetIssueKind::alias_ambiguous:
        return ErrorCode::conflict;
    case Slic3r::SystemPresetIssueKind::missing_dependency:
        return ErrorCode::not_found;
    case Slic3r::SystemPresetIssueKind::invalid_identity:
        return ErrorCode::invalid_configuration;
    }
    return ErrorCode::internal;
}

Result<std::shared_ptr<PresetCatalogState>> system_load_failure(
    const std::vector<Slic3r::SystemPresetLoadIssue> &issues)
{
    if (issues.empty())
        return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
            ErrorCode::internal,
            "System preset loader failed without a structured issue",
            "/resources_dir");

    std::vector<Diagnostic> additional;
    additional.reserve(issues.size() - 1);
    for (std::size_t index = 1; index < issues.size(); ++index) {
        additional.push_back({
            public_error_code(issues[index].kind),
            Severity::error,
            issues[index].message,
            "/resources_dir"
        });
    }
    return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
        public_error_code(issues.front().kind),
        issues.front().message,
        "/resources_dir",
        std::move(additional));
}

Result<std::shared_ptr<PresetCatalogState>> load_preset_catalog_with_core_locked(
    const ContextOptions &options, ConfigSchema schema)
{
#ifdef LIBSLICER_SDK_TESTING
    {
        std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
        record_scheduler_test_stats_storage() = {};
    }
#endif
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

    try {
        Slic3r::set_resources_dir(options.resources_dir.string());
        Slic3r::set_data_dir(options.data_dir.string());
        Slic3r::set_temporary_dir(options.temporary_dir.string());

        auto catalog = std::make_shared<PresetCatalogState>(std::move(schema));
        try {
            catalog->bundle = std::make_shared<Slic3r::PresetBundle>();
        } catch (const std::exception &error) {
            return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                ErrorCode::invalid_configuration,
                std::string("Failed to construct libslic3r default preset bundle: ") +
                    error.what(),
                "/presets/default_bundle");
        }
        catalog->user_store = options.data_dir / "libslicer_sdk_v1" / "user_store";
        auto prepared_store = prepare_user_store(catalog->user_store);
        if (!prepared_store.has_value())
            return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                *prepared_store.error_code(), prepared_store.diagnostics().front().message,
                prepared_store.diagnostics().front().field);

        const std::filesystem::path profiles = options.resources_dir / "profiles";
#ifdef LIBSLICER_SDK_TESTING
        const auto system_loader_started = std::chrono::steady_clock::now();
#endif
        Slic3r::SystemPresetLoadResult system_load =
            catalog->bundle->load_system_presets_from_json_at(
                profiles,
                Slic3r::ForwardCompatibilitySubstitutionRule::Disable,
                Slic3r::SystemPresetLoadPolicy::SdkStrict);
#ifdef LIBSLICER_SDK_TESTING
        {
            std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
            record_scheduler_test_stats_storage().system_loader_ns =
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() -
                        system_loader_started).count());
        }
#endif
        if (!system_load.issues.empty())
            return system_load_failure(system_load.issues);
        if (catalog->bundle->has_defauls_only())
            return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                ErrorCode::invalid_configuration,
                "System preset loader produced a default-only catalog",
                "/resources_dir");

        Slic3r::PresetsConfigSubstitutions substitutions;
        for (std::size_t index = 0; index < options.preset_dirs.size(); ++index) {
            try {
                const auto &dir = options.preset_dirs[index];
                const auto mark_read_only = [](Slic3r::Preset &preset) {
                    preset.bundle_id = "libslicer-sdk-read-only";
                };
                catalog->bundle->prints.load_presets(
                    dir.string(), catalog->bundle->prints.section_name(), substitutions,
                    Slic3r::ForwardCompatibilitySubstitutionRule::Disable,
                    mark_read_only, Slic3r::PresetOrigin(Slic3r::PresetOrigin::Kind::User));
                catalog->bundle->filaments.load_presets(
                    dir.string(), catalog->bundle->filaments.section_name(), substitutions,
                    Slic3r::ForwardCompatibilitySubstitutionRule::Disable,
                    mark_read_only, Slic3r::PresetOrigin(Slic3r::PresetOrigin::Kind::User));
                catalog->bundle->printers.load_presets(
                    dir.string(), catalog->bundle->printers.section_name(), substitutions,
                    Slic3r::ForwardCompatibilitySubstitutionRule::Disable,
                    mark_read_only, Slic3r::PresetOrigin(Slic3r::PresetOrigin::Kind::User));
            } catch (const std::exception &error) {
                return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                    ErrorCode::invalid_configuration, error.what(),
                    "/preset_dirs/" + std::to_string(index));
            }
        }

        try {
            auto appended = append_system_collections(*catalog);
            if (!appended.has_value())
                return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                    appended.error_code().value_or(ErrorCode::invalid_configuration),
                    appended.diagnostics().front().message,
                    appended.diagnostics().front().field);
        } catch (const std::exception &error) {
            return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                ErrorCode::invalid_configuration, error.what(), "/resources_dir");
        }
        try {
#ifdef LIBSLICER_SDK_TESTING
            const auto user_store_started = std::chrono::steady_clock::now();
#endif
            load_user_manifest(*catalog);
#ifdef LIBSLICER_SDK_TESTING
            {
                std::lock_guard<std::mutex> lock(record_scheduler_test_mutex());
                record_scheduler_test_stats_storage().user_store_ns =
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            user_store_started).count());
            }
#endif
        } catch (const std::exception &error) {
            return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
                ErrorCode::invalid_configuration, error.what(), "/data_dir");
        }
        return ResultAccess::success(std::move(catalog));
    } catch (const std::exception &error) {
        return ResultAccess::failure<std::shared_ptr<PresetCatalogState>>(
            ErrorCode::invalid_configuration, error.what(), "/resources_dir");
    }
}

} // namespace libslicer::v1::detail
