#include <catch2/catch_test_macros.hpp>

#include <libslicer/v1/Config.hpp>
#include <libslicer/v1/ConfigSchema.hpp>
#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Preset.hpp>
#include <libslicer/v1/Project.hpp>
#include <libslicer/v1/Result.hpp>
#include <libslicer/v1/Slice.hpp>
#include <libslicer/v1/Version.hpp>
#include <libslicer/v1/json.hpp>

#include "ConfigSchemaInternal.hpp"
#include "OrcaConfigAdapter.hpp"
#include "PresetInternal.hpp"
#include "RuntimeCoordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace libslicer::v1;

namespace {

struct TemporaryRoot {
    std::filesystem::path path;
    ~TemporaryRoot() { std::error_code error; std::filesystem::remove_all(path, error); }
};

TemporaryRoot make_temporary_root(const std::string &purpose)
{
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    TemporaryRoot result{std::filesystem::temp_directory_path() /
                         ("libslicer-public-api-" + purpose + "-" + unique)};
    REQUIRE(std::filesystem::create_directories(result.path));
    return result;
}

ContextOptions context_options(const std::filesystem::path &root)
{
    return {std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR),
            root / "data",
            root / "temporary",
            {},
            {64ull * 1024 * 1024,
             256ull * 1024 * 1024,
             2'000'000,
             128ull * 1024 * 1024,
             128ull * 1024 * 1024,
             5'000'000,
             128ull * 1024 * 1024}};
}

void write_process_preset(const std::filesystem::path &root,
                          const std::string &name,
                          const std::string &inherits)
{
    Slic3r::PresetBundle defaults;
    Slic3r::DynamicPrintConfig config(
        defaults.prints.default_preset().config);
    config.option<Slic3r::ConfigOptionString>(
        "print_settings_id", true)->value = name;
    config.option<Slic3r::ConfigOptionString>(
        BBL_JSON_KEY_INHERITS, true)->value = inherits;
    const auto directory = root / PRESET_PRINT_NAME;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    REQUIRE_FALSE(error);
    config.save_to_json(
        (directory / (name + ".json")).string(),
        name, "User", "1.0.0");
}

void check_primary_error(const Diagnostic &diagnostic, ErrorCode code,
                         const std::string &field)
{
    CHECK(diagnostic.code == code);
    CHECK(diagnostic.severity == Severity::error);
    CHECK(diagnostic.field == field);
    CHECK_FALSE(diagnostic.message.empty());
}

bool same_diagnostics(const std::vector<Diagnostic> &lhs,
                      const std::vector<Diagnostic> &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index].code != rhs[index].code ||
            lhs[index].severity != rhs[index].severity ||
            lhs[index].message != rhs[index].message ||
            lhs[index].field != rhs[index].field)
            return false;
    }
    return true;
}

struct ContextCreateOutcome {
    bool success {false};
    std::optional<ErrorCode> error;
    std::vector<Diagnostic> diagnostics;
};

ContextCreateOutcome create_context_outcome(ContextOptions options)
{
    auto result = SdkContext::create(std::move(options));
    return {result.has_value(), result.error_code(), result.diagnostics()};
}

bool same_value_descriptor(const ValueDescriptor &lhs,
                           const ValueDescriptor &rhs)
{
    if (lhs.shape() != rhs.shape() ||
        lhs.value_nullable() != rhs.value_nullable() ||
        lhs.enum_values() != rhs.enum_values() ||
        lhs.canonical_unit() != rhs.canonical_unit())
        return false;

    const auto lhs_numeric = lhs.numeric();
    const auto rhs_numeric = rhs.numeric();
    if (lhs_numeric.has_value() != rhs_numeric.has_value())
        return false;
    if (lhs_numeric &&
        (lhs_numeric->minimum != rhs_numeric->minimum ||
         lhs_numeric->maximum != rhs_numeric->maximum ||
         lhs_numeric->canonical_unit != rhs_numeric->canonical_unit))
        return false;

    const auto lhs_float_or_percent = lhs.float_or_percent();
    const auto rhs_float_or_percent = rhs.float_or_percent();
    if (lhs_float_or_percent.has_value() != rhs_float_or_percent.has_value())
        return false;
    if (lhs_float_or_percent) {
        if (lhs_float_or_percent->numeric.minimum !=
                rhs_float_or_percent->numeric.minimum ||
            lhs_float_or_percent->numeric.maximum !=
                rhs_float_or_percent->numeric.maximum ||
            lhs_float_or_percent->numeric.canonical_unit !=
                rhs_float_or_percent->numeric.canonical_unit ||
            lhs_float_or_percent->percent_base.has_value() !=
                rhs_float_or_percent->percent_base.has_value())
            return false;
        if (lhs_float_or_percent->percent_base &&
            *lhs_float_or_percent->percent_base !=
                *rhs_float_or_percent->percent_base)
            return false;
    }

    const auto lhs_list = lhs.list();
    const auto rhs_list = rhs.list();
    if (lhs_list.has_value() != rhs_list.has_value())
        return false;
    return !lhs_list ||
        (lhs_list->min_items() == rhs_list->min_items() &&
         lhs_list->max_items() == rhs_list->max_items() &&
         lhs_list->fixed_length() == rhs_list->fixed_length() &&
         same_value_descriptor(lhs_list->item(), rhs_list->item()));
}

bool same_option_descriptor(const OptionDescriptor &lhs,
                            const OptionDescriptor &rhs)
{
    return lhs.id == rhs.id &&
        same_value_descriptor(lhs.value, rhs.value) &&
        lhs.label == rhs.label &&
        lhs.allowed_scopes == rhs.allowed_scopes &&
        lhs.applicable_preset_kinds == rhs.applicable_preset_kinds &&
        lhs.filament_reference == rhs.filament_reference &&
        lhs.editable == rhs.editable;
}

const OptionDescriptor *linear_find(
    const std::vector<OptionDescriptor> &options, const OptionId &option)
{
    const auto found = std::find_if(
        options.begin(), options.end(),
        [&option](const OptionDescriptor &descriptor) {
            return descriptor.id == option;
        });
    return found == options.end() ? nullptr : &*found;
}

bool same_config_values(const ConfigValues &lhs, const ConfigValues &rhs)
{
    const auto &lhs_entries = lhs.entries();
    const auto &rhs_entries = rhs.entries();
    if (lhs_entries.size() != rhs_entries.size())
        return false;
    for (std::size_t index = 0; index < lhs_entries.size(); ++index) {
        if (lhs_entries[index].option != rhs_entries[index].option ||
            lhs_entries[index].value != rhs_entries[index].value)
            return false;
    }
    return true;
}

bool same_config_patch(const ConfigPatch &lhs, const ConfigPatch &rhs)
{
    const auto &lhs_entries = lhs.entries();
    const auto &rhs_entries = rhs.entries();
    if (lhs_entries.size() != rhs_entries.size())
        return false;
    for (std::size_t index = 0; index < lhs_entries.size(); ++index) {
        if (lhs_entries[index].option != rhs_entries[index].option ||
            lhs_entries[index].value != rhs_entries[index].value)
            return false;
    }
    return true;
}

Result<ConfigPatch> linear_core_config_diff_to_patch(
    const Slic3r::ConfigBase &inherited,
    const Slic3r::ConfigBase &effective,
    const ConfigSchema &schema)
{
    const std::vector<OptionDescriptor> options = schema.options();
    ConfigPatch patch;
    for (const std::string &key : effective.keys()) {
        const OptionDescriptor *descriptor =
            linear_find(options, OptionId(key));
        if (!descriptor || !descriptor->editable)
            continue;
        const auto *current = effective.optptr(key);
        const auto *base = inherited.optptr(key);
        if (!current || (base && *current == *base))
            continue;
        const auto *definition = Slic3r::print_config_def.get(key);
        if (!definition) {
            return libslicer::v1::detail::ResultAccess::failure<ConfigPatch>(
                ErrorCode::invalid_configuration,
                "Core configuration contains an unknown option", key);
        }
        auto converted =
            libslicer::v1::detail::core_option_to_value(*definition, *current);
        if (!converted.has_value()) {
            return libslicer::v1::detail::ResultAccess::failure<ConfigPatch>(
                *converted.error_code(),
                converted.diagnostics().front().message,
                converted.diagnostics().front().field);
        }
        patch.set(OptionId(key), std::move(converted).value());
    }
    return libslicer::v1::detail::ResultAccess::success(std::move(patch));
}

struct CatalogRecordSnapshot {
    PresetSummary metadata;
    ConfigValues  inherited;
    ConfigValues  effective;
    ConfigPatch   overrides;
};

bool same_selected_preset(const std::optional<SelectedPreset> &lhs,
                          const std::optional<SelectedPreset> &rhs)
{
    return lhs.has_value() == rhs.has_value() &&
        (!lhs || (lhs->ref == rhs->ref && lhs->revision == rhs->revision));
}

bool same_preset_summary(const PresetSummary &lhs,
                         const PresetSummary &rhs)
{
    return lhs.ref == rhs.ref &&
        lhs.name == rhs.name &&
        lhs.vendor == rhs.vendor &&
        same_selected_preset(lhs.parent, rhs.parent) &&
        lhs.revision == rhs.revision;
}

bool same_catalog_snapshot(const std::vector<CatalogRecordSnapshot> &lhs,
                           const std::vector<CatalogRecordSnapshot> &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!same_preset_summary(lhs[index].metadata, rhs[index].metadata) ||
            !same_config_values(lhs[index].inherited, rhs[index].inherited) ||
            !same_config_values(lhs[index].effective, rhs[index].effective) ||
            !same_config_patch(lhs[index].overrides, rhs[index].overrides))
            return false;
    }
    return true;
}

std::vector<CatalogRecordSnapshot> capture_catalog(
    const PresetRepository &repository)
{
    std::vector<CatalogRecordSnapshot> records;
    for (PresetKind kind : {PresetKind::printer, PresetKind::process,
                            PresetKind::filament}) {
        auto listed = repository.list(kind);
        REQUIRE(listed.has_value());
        for (const PresetSummary &summary : listed.value()) {
            auto view = repository.get({summary.ref, summary.revision});
            REQUIRE(view.has_value());
            records.push_back({
                view.value().metadata(),
                view.value().inherited_values(),
                view.value().effective_values(),
                view.value().overrides()});
        }
    }
    return records;
}

void check_catalog_value_sharing(
    const std::vector<CatalogRecordSnapshot> &records,
    std::size_t expected_default_authorities)
{
    using libslicer::v1::detail::config_values_identity_for_testing;

    std::map<PresetKind, std::set<std::uintptr_t>> root_identities;
    for (const CatalogRecordSnapshot &record : records) {
        if (!record.metadata.parent) {
            root_identities[record.metadata.ref.kind()].insert(
                config_values_identity_for_testing(record.inherited));
            continue;
        }

        const SelectedPreset &parent = *record.metadata.parent;
        const auto found = std::find_if(
            records.begin(), records.end(),
            [&parent](const CatalogRecordSnapshot &candidate) {
                return candidate.metadata.ref == parent.ref &&
                    candidate.metadata.revision == parent.revision;
            });
        REQUIRE(found != records.end());
        CHECK(config_values_identity_for_testing(record.inherited) ==
              config_values_identity_for_testing(found->effective));
    }

    std::set<std::uintptr_t> distinct_root_identities;
    std::size_t root_count = 0;
    for (const auto &entry : root_identities) {
        const PresetKind kind = entry.first;
        const auto &identities = entry.second;
        CAPTURE(kind);
        REQUIRE_FALSE(identities.empty());
        root_count += static_cast<std::size_t>(std::count_if(
            records.begin(), records.end(),
            [kind](const CatalogRecordSnapshot &record) {
                return !record.metadata.parent &&
                    record.metadata.ref.kind() == kind;
            }));
        distinct_root_identities.insert(identities.begin(), identities.end());
    }
    CHECK(root_identities.size() == 3);
    CHECK(distinct_root_identities.size() == expected_default_authorities);
    CHECK(root_count > distinct_root_identities.size());
}

template<class T, class = void>
struct has_legacy_config_resolver_facade : std::false_type {};

template<class T>
struct has_legacy_config_resolver_facade<
    T, std::void_t<decltype(std::declval<T &>().create_config_resolver())>>
    : std::true_type {};

template<class T, class = void>
struct has_project_embedded_factory : std::false_type {};

template<class T>
struct has_project_embedded_factory<
    T, std::void_t<decltype(T::project_embedded(
           PresetKind::filament, std::declval<std::string>()))>>
    : std::true_type {};

template<class T, class = void>
struct has_snapshot_inspect : std::false_type {};

template<class T>
struct has_snapshot_inspect<
    T, std::void_t<decltype(std::declval<T &>().inspect())>>
    : std::true_type {};

template<class T, class = void>
struct has_snapshot_effective_configuration : std::false_type {};

template<class T>
struct has_snapshot_effective_configuration<
    T, std::void_t<decltype(std::declval<T &>().effective_configuration())>>
    : std::true_type {};

} // namespace

TEST_CASE("Public SDK exposes its v1 semantic version", "[libslicer_sdk][public][version]")
{
    CHECK(sdk_version_major == 1);
    CHECK(sdk_version_minor == 0);
    CHECK(sdk_version_patch == 0);
}

TEST_CASE("Stable public SDK v1 headers expose only the frozen facade",
          "[libslicer_sdk][public][surface]")
{
    static_assert(!has_legacy_config_resolver_facade<SdkContext>::value,
                  "ConfigResolver must not be exposed by stable SdkContext");
    static_assert(!has_project_embedded_factory<PresetRef>::value,
                  "Project-embedded PresetRef construction is loader authority only");
    static_assert(!has_snapshot_inspect<ProjectSnapshot>::value,
                  "ProjectSnapshot must not expose inspect");
    static_assert(!has_snapshot_effective_configuration<ProjectSnapshot>::value,
                  "ProjectSnapshot must not expose effective configuration");

    static_assert(std::is_same_v<decltype(SliceRequest::project), ProjectSnapshot>);
    static_assert(std::is_same_v<decltype(SliceRequest::plate), PlateId>);
    static_assert(std::is_same_v<decltype(SliceRequest::temporary_selection),
                  std::optional<TemporarySliceSelection>>);
    static_assert(std::is_same_v<
        decltype(std::declval<SliceJob &>().wait_for(std::chrono::milliseconds{0})),
        Result<std::optional<std::shared_ptr<const SliceResult>>>>);
}

TEST_CASE("ConfigValue public factories preserve type shape and value",
          "[libslicer_sdk][public][config]")
{
    const ConfigValue boolean = ConfigValue::boolean(true);
    const ConfigValue integer = ConfigValue::integer(42);
    const ConfigValue decimal = ConfigValue::decimal(2.5);
    const ConfigValue percent = ConfigValue::percent(35.0);
    const ConfigValue mixed = ConfigValue::float_or_percent({12.5, true});
    const ConfigValue string = ConfigValue::string("grid");
    const ConfigValue enumeration = ConfigValue::enumeration("grid");
    const ConfigValue point2 = ConfigValue::point2({1.0, 2.0});
    const ConfigValue point3 = ConfigValue::point3({1.0, 2.0, 3.0});

    CHECK(boolean.as_boolean() == true);
    CHECK(integer.as_integer() == 42);
    CHECK(decimal.as_decimal() == 2.5);
    CHECK(percent.as_percent() == 35.0);
    CHECK(mixed.as_float_or_percent() == FloatOrPercent{12.5, true});
    CHECK(string.as_string() == "grid");
    CHECK_FALSE(string.as_enumeration().has_value());
    CHECK(enumeration.as_enumeration() == "grid");
    CHECK_FALSE(enumeration.as_string().has_value());
    CHECK(point2.as_point2() == Point2{1.0, 2.0});
    CHECK(point3.as_point3() == Point3{1.0, 2.0, 3.0});
    CHECK_FALSE(integer.as_decimal().has_value());

    const auto integer_shape = ConfigValueShape::scalar(ConfigValueType::integer);
    const auto group_shape = ConfigValueShape::list(ConfigValueShape::list(integer_shape));
    REQUIRE(group_shape.item_shape().has_value());
    REQUIRE(group_shape.item_shape()->item_shape().has_value());
    CHECK(group_shape.item_shape()->item_shape()->type() == ConfigValueType::integer);

    const ConfigValue null_integer = ConfigValue::null(integer_shape);
    CHECK(null_integer.is_null());
    CHECK(null_integer.shape() == integer_shape);
    CHECK_FALSE(null_integer.as_integer().has_value());

    auto list = ConfigValue::list(integer_shape,
                                  {ConfigValue::integer(4), null_integer,
                                   ConfigValue::integer(8)});
    REQUIRE(list.has_value());
    REQUIRE(list.value().as_list().has_value());
    CHECK(list.value().as_list()->size() == 3);
    CHECK(list.value().as_list()->at(1).is_null());

    auto invalid = ConfigValue::list(integer_shape, {ConfigValue::decimal(1.0)});
    REQUIRE_FALSE(invalid.has_value());
    REQUIRE(invalid.error_code().has_value());
    CHECK(invalid.error_code() == ErrorCode::invalid_argument);
    REQUIRE_FALSE(invalid.diagnostics().empty());
    check_primary_error(invalid.diagnostics().front(), ErrorCode::invalid_argument, "/values/0");
    CHECK_THROWS_AS(invalid.value(), std::logic_error);
}

TEST_CASE("ConfigPatch public operations are ordered and copy independent",
          "[libslicer_sdk][public][config]")
{
    ConfigPatch patch;
    patch.set(OptionId("first"), ConfigValue::integer(1));
    patch.set(OptionId("second"), ConfigValue::integer(2));
    patch.set(OptionId("first"), ConfigValue::integer(3));

    REQUIRE(patch.entries().size() == 2);
    CHECK(patch.entries()[0].option == OptionId("first"));
    CHECK(patch.entries()[0].value.as_integer() == 3);
    CHECK(patch.entries()[1].option == OptionId("second"));
    CHECK(patch.find(OptionId("first")) == ConfigValue::integer(3));

    ConfigPatch copy = patch;
    copy.set(OptionId("first"), ConfigValue::integer(9));
    CHECK(patch.find(OptionId("first")) == ConfigValue::integer(3));
    CHECK(copy.find(OptionId("first")) == ConfigValue::integer(9));
    CHECK(copy.erase(OptionId("second")));
    CHECK_FALSE(copy.erase(OptionId("missing")));
    CHECK(patch.find(OptionId("second")) == ConfigValue::integer(2));
}

TEST_CASE("SdkContext public validation reports each zero resource limit",
          "[libslicer_sdk][public][context][errors]")
{
    TemporaryRoot root = make_temporary_root("limits");
    const std::vector<std::string> fields{
        "/limits/project_input_bytes",
        "/limits/project_uncompressed_bytes",
        "/limits/model_triangles",
        "/limits/gcode_bytes",
        "/limits/preview_bytes",
        "/limits/preview_moves",
        "/limits/temporary_disk_bytes"};

    for (std::size_t index = 0; index < fields.size(); ++index) {
        ContextOptions options = context_options(root.path);
        switch (index) {
        case 0: options.limits.project_input_bytes = 0; break;
        case 1: options.limits.project_uncompressed_bytes = 0; break;
        case 2: options.limits.model_triangles = 0; break;
        case 3: options.limits.gcode_bytes = 0; break;
        case 4: options.limits.preview_bytes = 0; break;
        case 5: options.limits.preview_moves = 0; break;
        case 6: options.limits.temporary_disk_bytes = 0; break;
        default: FAIL("unexpected resource limit index");
        }

        auto result = SdkContext::create(std::move(options));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error_code() == ErrorCode::invalid_argument);
        REQUIRE_FALSE(result.diagnostics().empty());
        check_primary_error(result.diagnostics().front(), ErrorCode::invalid_argument,
                            fields[index]);
        CHECK_THROWS_AS(result.value(), std::logic_error);
    }
}

TEST_CASE("SdkContext refuses fallback preset catalogs",
          "[libslicer_sdk][public][context][catalog]")
{
    TemporaryRoot root = make_temporary_root("catalog");

    ContextOptions bad_resources = context_options(root.path);
    bad_resources.resources_dir = root.path / "missing-resources";
    auto missing_resources = SdkContext::create(std::move(bad_resources));
    REQUIRE_FALSE(missing_resources.has_value());
    CHECK(missing_resources.error_code() == ErrorCode::io);
    REQUIRE_FALSE(missing_resources.diagnostics().empty());
    CHECK(missing_resources.diagnostics().front().field == "/resources_dir");

    const auto no_profiles_dir = root.path / "resources-no-profiles";
    REQUIRE(std::filesystem::create_directories(no_profiles_dir));
    ContextOptions no_profiles = context_options(root.path);
    no_profiles.resources_dir = no_profiles_dir;
    auto missing_profiles = SdkContext::create(std::move(no_profiles));
    REQUIRE_FALSE(missing_profiles.has_value());
    CHECK(missing_profiles.error_code() == ErrorCode::invalid_configuration);
    REQUIRE_FALSE(missing_profiles.diagnostics().empty());
    CHECK(missing_profiles.diagnostics().front().field == "/resources_dir/profiles");

    const auto no_vendor_dir = root.path / "resources-no-vendor";
    REQUIRE(std::filesystem::create_directories(no_vendor_dir / "profiles"));
    ContextOptions no_vendor = context_options(root.path);
    no_vendor.resources_dir = no_vendor_dir;
    auto missing_vendor = SdkContext::create(std::move(no_vendor));
    REQUIRE_FALSE(missing_vendor.has_value());
    CHECK(missing_vendor.error_code() == ErrorCode::not_found);
    REQUIRE_FALSE(missing_vendor.diagnostics().empty());
    CHECK(missing_vendor.diagnostics().front().field == "/resources_dir");

    const auto no_orca_dir = root.path / "resources-no-orca";
    REQUIRE(std::filesystem::create_directories(no_orca_dir / "profiles"));
    {
        std::ofstream dummy(no_orca_dir / "profiles" / "OtherVendor.json");
        dummy << "{}";
    }
    ContextOptions no_orca = context_options(root.path);
    no_orca.resources_dir = no_orca_dir;
    auto missing_orca = SdkContext::create(std::move(no_orca));
    REQUIRE_FALSE(missing_orca.has_value());
    CHECK(missing_orca.error_code() == ErrorCode::not_found);
    REQUIRE_FALSE(missing_orca.diagnostics().empty());
    CHECK(missing_orca.diagnostics().front().field == "/resources_dir");

    auto valid = SdkContext::create(context_options(root.path));
    REQUIRE(valid.has_value());
    auto repository = valid.value().presets();
    REQUIRE(repository.has_value());
    auto filaments = repository.value().list(PresetKind::filament);
    REQUIRE(filaments.has_value());
    CHECK_FALSE(filaments.value().empty());
}

TEST_CASE("SdkContext reuses one canonical process runtime and keeps local policy local",
          "[libslicer_sdk][context][runtime][reuse]")
{
    using namespace libslicer::v1::detail;

    REQUIRE(reset_runtime_coordinator_for_testing());
    set_runtime_initialization_hook_for_testing({});

    TemporaryRoot root = make_temporary_root("runtime-reuse");
    const auto first_presets = root.path / "presets-a";
    const auto second_presets = root.path / "presets-b";
    const auto first_presets_link = root.path / "presets-a-link";
    REQUIRE(std::filesystem::create_directories(first_presets));
    REQUIRE(std::filesystem::create_directories(second_presets));
    std::filesystem::create_directory_symlink(first_presets, first_presets_link);

    ContextOptions first_options = context_options(root.path);
    first_options.preset_dirs = {first_presets, second_presets};
    auto first = SdkContext::create(first_options);
    REQUIRE(first.has_value());

    const RuntimeCoordinatorTestStats initialized =
        runtime_coordinator_test_stats();
    CHECK(initialized.attempts == 1);
    CHECK(initialized.loader_invocations == 1);
    CHECK(initialized.publishes == 1);
    CHECK(initialized.generation == 1);
    CHECK(initialized.ready);
    REQUIRE(initialized.live_runtime_identity != 0);

    for (std::size_t index = 0; index < 20; ++index) {
        ContextOptions reused = context_options(root.path);
        reused.resources_dir =
            std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR) / ".";
        reused.temporary_dir = root.path / ("temporary-" + std::to_string(index));
        reused.preset_dirs = {first_presets_link, second_presets};
        const std::uint64_t delta = index + 1;
        switch (index % 7) {
        case 0: reused.limits.project_input_bytes += delta; break;
        case 1: reused.limits.project_uncompressed_bytes += delta; break;
        case 2: reused.limits.model_triangles += delta; break;
        case 3: reused.limits.gcode_bytes += delta; break;
        case 4: reused.limits.preview_bytes += delta; break;
        case 5: reused.limits.preview_moves += delta; break;
        case 6: reused.limits.temporary_disk_bytes += delta; break;
        }
        auto context = SdkContext::create(std::move(reused));
        REQUIRE(context.has_value());
    }

    const RuntimeCoordinatorTestStats reused = runtime_coordinator_test_stats();
    CHECK(reused.loader_invocations == 1);
    CHECK(reused.publishes == 1);
    CHECK(reused.reuses == 20);
    CHECK(reused.generation == initialized.generation);
    CHECK(reused.live_runtime_identity == initialized.live_runtime_identity);

    ContextOptions reordered = context_options(root.path);
    reordered.preset_dirs = {second_presets, first_presets};
    auto conflict = SdkContext::create(std::move(reordered));
    REQUIRE_FALSE(conflict.has_value());
    CHECK(conflict.error_code() == ErrorCode::conflict);
    REQUIRE_FALSE(conflict.diagnostics().empty());
    CHECK(conflict.diagnostics().front().field == "/preset_dirs/0");
    CHECK(runtime_coordinator_test_stats().loader_invocations == 1);
}

TEST_CASE("SdkContext concurrent first create is single flight",
          "[libslicer_sdk][context][runtime][concurrent]")
{
    using namespace libslicer::v1::detail;

    REQUIRE(reset_runtime_coordinator_for_testing());
    TemporaryRoot root = make_temporary_root("runtime-concurrent");

    std::mutex barrier_mutex;
    std::condition_variable barrier_condition;
    bool loader_started = false;
    bool release_loader = false;
    set_runtime_initialization_hook_for_testing(
        [&](RuntimeInitializationTestStage stage) {
            if (stage != RuntimeInitializationTestStage::loader_start)
                return;
            std::unique_lock<std::mutex> lock(barrier_mutex);
            loader_started = true;
            barrier_condition.notify_all();
            barrier_condition.wait(lock, [&] { return release_loader; });
        });

    constexpr std::size_t thread_count = 16;
    std::vector<ContextCreateOutcome> outcomes(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            outcomes[index] = create_context_outcome(context_options(root.path));
        });
    }

    bool observed_loader = false;
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        observed_loader = barrier_condition.wait_for(
            lock, std::chrono::seconds(10), [&] { return loader_started; });
    }

    ContextCreateOutcome conflict;
    std::chrono::steady_clock::duration conflict_duration {};
    if (observed_loader) {
        ContextOptions different_key = context_options(root.path / "different");
        different_key.resources_dir =
            std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR);
        const auto conflict_started = std::chrono::steady_clock::now();
        conflict = create_context_outcome(std::move(different_key));
        conflict_duration = std::chrono::steady_clock::now() - conflict_started;
    }

    const auto wait_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (observed_loader &&
           runtime_coordinator_test_stats().waiters < thread_count - 1 &&
           std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_loader = true;
    }
    barrier_condition.notify_all();
    for (std::thread &thread : threads)
        thread.join();
    set_runtime_initialization_hook_for_testing({});

    REQUIRE(observed_loader);
    REQUIRE_FALSE(conflict.success);
    CHECK(conflict.error == ErrorCode::conflict);
    REQUIRE_FALSE(conflict.diagnostics.empty());
    CHECK(conflict.diagnostics.front().field == "/data_dir");
    CHECK(conflict_duration < std::chrono::milliseconds(500));

    for (const ContextCreateOutcome &outcome : outcomes) {
        CHECK(outcome.success);
        CHECK_FALSE(outcome.error.has_value());
        CHECK(outcome.diagnostics.empty());
    }

    const RuntimeCoordinatorTestStats stats = runtime_coordinator_test_stats();
    CHECK(stats.attempts == 1);
    CHECK(stats.loader_invocations == 1);
    CHECK(stats.publishes == 1);
    CHECK(stats.failures == 0);
    CHECK(stats.waiters == thread_count - 1);
    CHECK(stats.generation == 1);
}

TEST_CASE("SdkContext failed single flight is atomic and explicit create retries",
          "[libslicer_sdk][context][runtime][retry]")
{
    using namespace libslicer::v1::detail;

    REQUIRE(reset_runtime_coordinator_for_testing());
    TemporaryRoot root = make_temporary_root("runtime-retry");
    const auto resources = root.path / "resources";
    const auto profiles = resources / "profiles";
    REQUIRE(std::filesystem::create_directories(profiles));

    ContextOptions failed_options = context_options(root.path);
    failed_options.resources_dir = resources;

    std::mutex barrier_mutex;
    std::condition_variable barrier_condition;
    bool loader_started = false;
    bool release_loader = false;
    set_runtime_initialization_hook_for_testing(
        [&](RuntimeInitializationTestStage stage) {
            if (stage != RuntimeInitializationTestStage::loader_start)
                return;
            std::unique_lock<std::mutex> lock(barrier_mutex);
            loader_started = true;
            barrier_condition.notify_all();
            barrier_condition.wait(lock, [&] { return release_loader; });
        });

    constexpr std::size_t thread_count = 16;
    std::vector<ContextCreateOutcome> outcomes(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([&, index] {
            outcomes[index] = create_context_outcome(failed_options);
        });
    }

    bool observed_loader = false;
    {
        std::unique_lock<std::mutex> lock(barrier_mutex);
        observed_loader = barrier_condition.wait_for(
            lock, std::chrono::seconds(10), [&] { return loader_started; });
    }
    const auto wait_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (observed_loader &&
           runtime_coordinator_test_stats().waiters < thread_count - 1 &&
           std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> lock(barrier_mutex);
        release_loader = true;
    }
    barrier_condition.notify_all();
    for (std::thread &thread : threads)
        thread.join();
    set_runtime_initialization_hook_for_testing({});

    REQUIRE(observed_loader);
    REQUIRE_FALSE(outcomes.front().success);
    REQUIRE(outcomes.front().error == ErrorCode::not_found);
    REQUIRE_FALSE(outcomes.front().diagnostics.empty());
    for (const ContextCreateOutcome &outcome : outcomes) {
        CHECK_FALSE(outcome.success);
        CHECK(outcome.error == outcomes.front().error);
        CHECK(same_diagnostics(outcome.diagnostics,
                               outcomes.front().diagnostics));
    }

    RuntimeCoordinatorTestStats failed = runtime_coordinator_test_stats();
    CHECK(failed.attempts == 1);
    CHECK(failed.loader_invocations == 1);
    CHECK(failed.publishes == 0);
    CHECK(failed.failures == 1);
    CHECK(failed.generation == 0);
    CHECK_FALSE(failed.initializing);
    CHECK_FALSE(failed.ready);

    REQUIRE(std::filesystem::remove(profiles));
    std::filesystem::create_directory_symlink(
        std::filesystem::path(LIBSLICER_TEST_RESOURCES_DIR) / "profiles",
        profiles);

    auto retried = SdkContext::create(std::move(failed_options));
    REQUIRE(retried.has_value());
    const RuntimeCoordinatorTestStats succeeded =
        runtime_coordinator_test_stats();
    CHECK(succeeded.attempts == 2);
    CHECK(succeeded.loader_invocations == 2);
    CHECK(succeeded.failures == 1);
    CHECK(succeeded.publishes == 1);
    CHECK(succeeded.generation == 1);
    CHECK(succeeded.ready);
}

TEST_CASE("RuntimeCoordinator strongly retains the shared runtime and keeps reuse within the gate",
          "[libslicer_sdk][context][runtime][lifetime][performance]")
{
    using namespace libslicer::v1::detail;

    REQUIRE(reset_runtime_coordinator_for_testing());
    set_runtime_initialization_hook_for_testing({});
    TemporaryRoot root = make_temporary_root("runtime-lifetime");

    const auto cold_started = std::chrono::steady_clock::now();
    std::uintptr_t runtime_identity = 0;
    {
        auto first = SdkContext::create(context_options(root.path));
        REQUIRE(first.has_value());
        runtime_identity =
            runtime_coordinator_test_stats().live_runtime_identity;
        REQUIRE(runtime_identity != 0);
    }
    const auto cold_duration =
        std::chrono::steady_clock::now() - cold_started;

    const RuntimeCoordinatorTestStats retained =
        runtime_coordinator_test_stats();
    CHECK(retained.ready);
    CHECK(retained.live_runtime_identity == runtime_identity);
    CHECK(retained.loader_invocations == 1);

    const auto reuse_started = std::chrono::steady_clock::now();
    auto reused = SdkContext::create(context_options(root.path));
    const auto reuse_duration =
        std::chrono::steady_clock::now() - reuse_started;
    REQUIRE(reused.has_value());

    const auto cold_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cold_duration).count();
    const auto reuse_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            reuse_duration).count();
    CAPTURE(cold_ms, reuse_us);
    CHECK(cold_duration <= std::chrono::seconds(15));
    CHECK(reuse_duration <= std::chrono::milliseconds(100));
    CHECK(runtime_coordinator_test_stats().loader_invocations == 1);
    CHECK(runtime_coordinator_test_stats().live_runtime_identity ==
          runtime_identity);
}

TEST_CASE("RuntimeCoordinator discards injected failure stages and retries explicitly",
          "[libslicer_sdk][context][runtime][failure-stage][retry]")
{
    using namespace libslicer::v1::detail;

    const std::vector<RuntimeInitializationTestStage> failure_stages{
        RuntimeInitializationTestStage::loader_start,
        RuntimeInitializationTestStage::before_publish
    };

    for (RuntimeInitializationTestStage failure_stage : failure_stages) {
        DYNAMIC_SECTION("stage " << static_cast<int>(failure_stage)) {
            REQUIRE(reset_runtime_coordinator_for_testing());
            TemporaryRoot root = make_temporary_root(
                "runtime-failure-stage-" +
                std::to_string(static_cast<int>(failure_stage)));
            std::atomic<std::size_t> injected {0};
            set_runtime_initialization_hook_for_testing(
                [&](RuntimeInitializationTestStage observed) {
                    if (observed == failure_stage &&
                        injected.fetch_add(1) == 0)
                        throw std::runtime_error(
                            "Injected runtime initialization failure");
                });

            auto failed = SdkContext::create(context_options(root.path));
            REQUIRE_FALSE(failed.has_value());
            CHECK(failed.error_code() == ErrorCode::internal);
            REQUIRE_FALSE(failed.diagnostics().empty());
            CHECK(failed.diagnostics().front().field == "/runtime");

            const RuntimeCoordinatorTestStats discarded =
                runtime_coordinator_test_stats();
            CHECK(discarded.publishes == 0);
            CHECK(discarded.failures == 1);
            CHECK(discarded.generation == 0);
            CHECK_FALSE(discarded.ready);
            CHECK_FALSE(discarded.initializing);

            set_runtime_initialization_hook_for_testing({});
            auto retried = SdkContext::create(context_options(root.path));
            REQUIRE(retried.has_value());
            const RuntimeCoordinatorTestStats succeeded =
                runtime_coordinator_test_stats();
            CHECK(succeeded.attempts == 2);
            CHECK(succeeded.publishes == 1);
            CHECK(succeeded.failures == 1);
            CHECK(succeeded.generation == 1);
            CHECK(succeeded.ready);
        }
    }
}

TEST_CASE("ConfigSchema immutable index matches linear lookup and rejects duplicates",
          "[libslicer_sdk][schema][index][cfg-07]")
{
    using namespace libslicer::v1::detail;

    auto built = build_orca_fff_schema();
    REQUIRE(built.has_value());
    const ConfigSchema schema = built.value();
    const ConfigSchema schema_copy = schema;
    const std::vector<OptionDescriptor> options = schema.options();
    REQUIRE(options.size() >= 3);

    const std::vector<OptionId> present{
        options.front().id,
        options[options.size() / 2].id,
        options.back().id,
        OptionId("outer_wall_speed"),
        OptionId("wall_loops")
    };
    for (const OptionId &id : present) {
        const OptionDescriptor *linear = linear_find(options, id);
        const OptionDescriptor *indexed =
            ConfigSchemaAccess::find_descriptor(schema, id);
        const OptionDescriptor *indexed_copy =
            ConfigSchemaAccess::find_descriptor(schema_copy, id);
        REQUIRE(linear != nullptr);
        REQUIRE(indexed != nullptr);
        CHECK(indexed == indexed_copy);
        CHECK(same_option_descriptor(*linear, *indexed));
        const auto public_value = schema.find(id);
        REQUIRE(public_value.has_value());
        CHECK(same_option_descriptor(*linear, *public_value));
        const auto repeated = schema.find(id);
        REQUIRE(repeated.has_value());
        CHECK(same_option_descriptor(*public_value, *repeated));
    }

    const std::vector<OptionId> missing{
        OptionId(""),
        OptionId("definitely_unknown_option"),
        OptionId("Outer_Wall_Speed")
    };
    for (const OptionId &id : missing) {
        CHECK(linear_find(options, id) == nullptr);
        CHECK(ConfigSchemaAccess::find_descriptor(schema, id) == nullptr);
        CHECK_FALSE(schema.find(id).has_value());
    }
    CHECK(ConfigSchemaAccess::option_index_build_count(schema) == 1);
    CHECK(ConfigSchemaAccess::option_index_build_count(schema_copy) == 1);
    CHECK(ConfigSchemaAccess::option_index_mutation_count(schema) == 0);

    const ValueDescriptor scalar = ConfigSchemaAccess::scalar(
        ConfigValueShape::scalar(ConfigValueType::integer));
    std::vector<OptionDescriptor> duplicates{
        {OptionId("duplicate/~option"), scalar, "first",
         {OptionScope::project}, {}, FilamentReferenceKind::none, true},
        {OptionId("duplicate/~option"), scalar, "second",
         {OptionScope::project}, {}, FilamentReferenceKind::none, true}
    };
    REQUIRE(linear_find(duplicates, OptionId("duplicate/~option")) ==
            &duplicates.front());
    auto duplicate = ConfigSchemaAccess::make(
        "test.duplicate", 1, std::move(duplicates), {}, {});
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error_code() == ErrorCode::internal);
    REQUIRE_FALSE(duplicate.diagnostics().empty());
    check_primary_error(duplicate.diagnostics().front(), ErrorCode::internal,
                        "/schema/options/duplicate~1~0option");
}

TEST_CASE("ConfigSchema immutable index supports concurrent read-only operations",
          "[libslicer_sdk][schema][index][concurrent][cfg-07]")
{
    using namespace libslicer::v1::detail;

    const ValueDescriptor scalar = ConfigSchemaAccess::scalar(
        ConfigValueShape::scalar(ConfigValueType::integer));
    auto built = ConfigSchemaAccess::make(
        "test.concurrent", 1,
        {{OptionId("value"), scalar, "value", {OptionScope::project}, {},
          FilamentReferenceKind::none, true}},
        {}, [](const OptionId &, const ConfigValues &,
               const ConfigValidationContext &) {
            return ResultAccess::success(
                OptionEvaluation{true, true, {}});
        });
    REQUIRE(built.has_value());
    const ConfigSchema schema = built.value();
    const ConfigValues base = ConfigValuesAccess::make({});
    const ConfigPatch patch;
    const ConfigValidationContext context{
        OptionScope::project, std::nullopt, std::nullopt,
        std::nullopt, std::nullopt};

    constexpr std::size_t thread_count = 16;
    constexpr std::size_t iteration_count = 64;
    std::mutex start_mutex;
    std::condition_variable start_condition;
    std::size_t ready = 0;
    bool start = false;
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t thread_index = 0;
         thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&, schema_copy = schema] {
            {
                std::unique_lock<std::mutex> lock(start_mutex);
                ++ready;
                start_condition.notify_all();
                start_condition.wait(lock, [&] { return start; });
            }
            for (std::size_t iteration = 0;
                 iteration < iteration_count; ++iteration) {
                const auto found = schema_copy.find(OptionId("value"));
                const auto missing = schema_copy.find(OptionId("Value"));
                const auto options_copy = schema_copy.options();
                const auto normalized = schema_copy.validate_and_normalize(
                    base, patch, context);
                const auto evaluated = schema_copy.evaluate_option(
                    OptionId("value"), base, patch, context);
                if (!found || missing || options_copy.size() != 1 ||
                    !same_option_descriptor(*found, options_copy.front()) ||
                    !normalized.has_value() || !evaluated.has_value() ||
                    !evaluated.value().visible ||
                    !evaluated.value().compatible ||
                    ConfigSchemaAccess::find_descriptor(
                        schema_copy, OptionId("value")) !=
                        ConfigSchemaAccess::find_descriptor(
                            schema, OptionId("value")))
                    ++failures;
            }
        });
    }
    {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_condition.wait(lock, [&] { return ready == thread_count; });
        start = true;
    }
    start_condition.notify_all();
    for (std::thread &thread : threads)
        thread.join();

    CHECK(failures.load() == 0);
    CHECK(ConfigSchemaAccess::option_index_build_count(schema) == 1);
    CHECK(ConfigSchemaAccess::option_index_mutation_count(schema) == 0);
}

TEST_CASE("Indexed config diff matches linear oracle and round-trips core values",
          "[libslicer_sdk][schema][index][diff][cfg-08]")
{
    auto built = libslicer::v1::detail::build_orca_fff_schema();
    REQUIRE(built.has_value());
    const ConfigSchema schema = built.value();

    Slic3r::DynamicPrintConfig inherited =
        Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::DynamicPrintConfig effective(inherited);
    effective.set_key_value(
        "outer_wall_speed", new Slic3r::ConfigOptionFloat(123.25));
    effective.set_key_value(
        "wall_loops", new Slic3r::ConfigOptionInt(5));

    auto indexed = libslicer::v1::detail::core_config_diff_to_patch(
        inherited, effective, schema);
    auto linear = linear_core_config_diff_to_patch(
        inherited, effective, schema);
    REQUIRE(indexed.has_value());
    REQUIRE(linear.has_value());
    CHECK(same_config_patch(indexed.value(), linear.value()));
    REQUIRE(indexed.value().entries().size() == 2);
    CHECK(indexed.value().entries()[0].option ==
          linear.value().entries()[0].option);
    CHECK(indexed.value().entries()[1].option ==
          linear.value().entries()[1].option);

    Slic3r::DynamicPrintConfig round_tripped(inherited);
    auto applied = libslicer::v1::detail::apply_patch_to_core_config(
        indexed.value(), round_tripped);
    REQUIRE(applied.has_value());
    REQUIRE(round_tripped.optptr("outer_wall_speed") != nullptr);
    REQUIRE(effective.optptr("outer_wall_speed") != nullptr);
    REQUIRE(round_tripped.optptr("wall_loops") != nullptr);
    REQUIRE(effective.optptr("wall_loops") != nullptr);
    CHECK(*round_tripped.optptr("outer_wall_speed") ==
          *effective.optptr("outer_wall_speed"));
    CHECK(*round_tripped.optptr("wall_loops") ==
          *effective.optptr("wall_loops"));
}

TEST_CASE("Indexed schema preserves complete production catalog records and order",
          "[libslicer_sdk][schema][index][catalog][cfg-08]")
{
    using namespace libslicer::v1::detail;

    REQUIRE(reset_runtime_coordinator_for_testing());
    set_runtime_initialization_hook_for_testing({});
    TemporaryRoot root = make_temporary_root("schema-index-catalog");
    auto context = SdkContext::create(context_options(root.path));
    REQUIRE(context.has_value());
    auto repository_result = context.value().presets();
    REQUIRE(repository_result.has_value());
    const PresetRepository repository = repository_result.value();

    const auto capture = [&](const PresetRepository &source) {
        std::vector<CatalogRecordSnapshot> records;
        for (PresetKind kind : {PresetKind::printer, PresetKind::process,
                                PresetKind::filament}) {
            auto listed = source.list(kind);
            REQUIRE(listed.has_value());
            for (const PresetSummary &summary : listed.value()) {
                auto view = source.get({summary.ref, summary.revision});
                REQUIRE(view.has_value());
                records.push_back({
                    view.value().metadata(),
                    view.value().inherited_values(),
                    view.value().effective_values(),
                    view.value().overrides()});
            }
        }
        return records;
    };

    const std::vector<CatalogRecordSnapshot> first = capture(repository);
    REQUIRE_FALSE(first.empty());
    auto reused_context = SdkContext::create(context_options(root.path));
    REQUIRE(reused_context.has_value());
    auto reused_repository = reused_context.value().presets();
    REQUIRE(reused_repository.has_value());
    const std::vector<CatalogRecordSnapshot> second =
        capture(reused_repository.value());
    CHECK(same_catalog_snapshot(first, second));
    CHECK(runtime_coordinator_test_stats().loader_invocations == 1);
}

TEST_CASE("Fused config conversion matches the two-pass oracle and shares changed values",
          "[libslicer_sdk][config][fused][sharing][cfg-09]")
{
    auto built = libslicer::v1::detail::build_orca_fff_schema();
    REQUIRE(built.has_value());
    const ConfigSchema schema = built.value();

    Slic3r::DynamicPrintConfig inherited =
        Slic3r::DynamicPrintConfig::full_print_config();
    Slic3r::DynamicPrintConfig effective(inherited);
    effective.set_key_value(
        "outer_wall_speed", new Slic3r::ConfigOptionFloat(117.5));
    effective.set_key_value(
        "wall_loops", new Slic3r::ConfigOptionInt(7));
    effective.set_key_value(
        "filament_colour",
        new Slic3r::ConfigOptionStrings({"#112233", "#445566"}));
    effective.set_key_value(
        "bed_exclude_area",
        new Slic3r::ConfigOptionPoints(
            {Slic3r::Vec2d{1.0, 2.0}, Slic3r::Vec2d{3.0, 4.0}}));

    auto expected_values =
        libslicer::v1::detail::core_config_to_values(effective);
    auto expected_patch =
        libslicer::v1::detail::core_config_diff_to_patch(
        inherited, effective, schema);
    auto fused = libslicer::v1::detail::core_config_to_values_and_diff(
        inherited, effective, schema);
    REQUIRE(expected_values.has_value());
    REQUIRE(expected_patch.has_value());
    REQUIRE(fused.has_value());
    CHECK(same_config_values(fused.value().effective,
                             expected_values.value()));
    CHECK(same_config_patch(fused.value().overrides,
                            expected_patch.value()));

    for (const ConfigEntry &changed : fused.value().overrides.entries()) {
        const auto effective_value =
            fused.value().effective.find(changed.option);
        REQUIRE(effective_value.has_value());
        CHECK(libslicer::v1::detail::config_value_identity_for_testing(
                  changed.value) ==
              libslicer::v1::detail::config_value_identity_for_testing(
                  *effective_value));
    }

    Slic3r::DynamicPrintConfig round_tripped(inherited);
    REQUIRE(libslicer::v1::detail::apply_patch_to_core_config(
                fused.value().overrides, round_tripped).has_value());
    for (const ConfigEntry &changed : fused.value().overrides.entries()) {
        const std::string key = changed.option.value();
        REQUIRE(round_tripped.optptr(key) != nullptr);
        REQUIRE(effective.optptr(key) != nullptr);
        CHECK(*round_tripped.optptr(key) == *effective.optptr(key));
    }

    Slic3r::DynamicPrintConfig invalid(effective);
    invalid.set_key_value(
        "sdk_unknown_option",
        new Slic3r::ConfigOptionString("invalid"));
    auto invalid_oracle =
        libslicer::v1::detail::core_config_to_values(invalid);
    auto invalid_fused =
        libslicer::v1::detail::core_config_to_values_and_diff(
        inherited, invalid, schema);
    REQUIRE_FALSE(invalid_oracle.has_value());
    REQUIRE_FALSE(invalid_fused.has_value());
    CHECK(invalid_fused.error_code() == invalid_oracle.error_code());
    CHECK(same_diagnostics(invalid_fused.diagnostics(),
                           invalid_oracle.diagnostics()));

    std::atomic<std::size_t> failures {0};
    std::vector<std::thread> readers;
    for (std::size_t thread_index = 0; thread_index < 8; ++thread_index) {
        readers.emplace_back([&] {
            for (std::size_t iteration = 0; iteration < 200; ++iteration) {
                if (!same_config_values(
                        fused.value().effective, expected_values.value()) ||
                    !same_config_patch(
                        fused.value().overrides, expected_patch.value()))
                    failures.fetch_add(1);
            }
        });
    }
    for (auto &reader : readers)
        reader.join();
    CHECK(failures.load() == 0);
}

TEST_CASE("Record scheduler preserves catalogs across bounded worker limits",
          "[libslicer_sdk][preset][scheduler][determinism][cfg-10]")
{
    using namespace libslicer::v1::detail;

    const auto build_catalog =
        [&](std::size_t worker_limit) {
            REQUIRE(reset_runtime_coordinator_for_testing());
            reset_record_scheduler_test_control();
            set_record_scheduler_test_control({worker_limit, {}});
            TemporaryRoot root = make_temporary_root(
                "record-scheduler-" + std::to_string(worker_limit));
            auto context = SdkContext::create(context_options(root.path));
            REQUIRE(context.has_value());
            auto repository = context.value().presets();
            REQUIRE(repository.has_value());
            const auto records = capture_catalog(repository.value());
            const auto stats = record_scheduler_test_stats();
            CHECK(stats.configured_worker_limit == worker_limit);
            CHECK(stats.arena_max_concurrency ==
                  std::min(worker_limit, records.size()));
            CHECK(stats.maximum_active_workers <= worker_limit);
            CHECK(stats.dependency_batches_started == 3);
            CHECK(stats.dependency_batches_joined == 3);
            CHECK(stats.system_loader_quiescent_before_arena);
            CHECK(stats.arena_destroyed_before_commit);
            CHECK(stats.worker_lazy_helper_calls == 0);
            CHECK(stats.worker_catalog_mutations == 0);
            CHECK(stats.worker_core_mutations == 0);
            CHECK(stats.worker_next_revision_writes == 0);
            CHECK(stats.peak_in_flight_candidates <= worker_limit);
            CHECK(stats.in_flight_candidates == 0);
            CHECK(stats.committed_records == records.size());
            CHECK(stats.status_candidate_owners_after_commit == 0);
            CHECK(stats.artifact_value_owners_after_commit == 0);
            check_catalog_value_sharing(records, stats.default_conversions);
            return std::make_pair(records, stats);
        };

    auto one = build_catalog(1);
    auto two = build_catalog(2);
    CHECK(same_catalog_snapshot(one.first, two.first));
    two.first.clear();
    two.first.shrink_to_fit();

    auto four = build_catalog(4);
    CHECK(same_catalog_snapshot(one.first, four.first));
    CHECK(four.second.default_conversions > 0);
    CHECK(four.second.default_value_shares >
          four.second.default_conversions);

    auto repeated_four = build_catalog(4);
    CHECK(same_catalog_snapshot(one.first, repeated_four.first));
    CHECK(repeated_four.second.maximum_active_workers <= 4);
}

TEST_CASE("Record scheduler selects the minimum ordinal and separates parent artifacts",
          "[libslicer_sdk][preset][scheduler][failure][dag][cfg-10]")
{
    using namespace libslicer::v1::detail;

    const auto run_failure =
        [&](std::vector<RecordFailureInjection> failures,
            std::vector<std::filesystem::path> preset_roots = {}) {
            REQUIRE(reset_runtime_coordinator_for_testing());
            reset_record_scheduler_test_control();
            set_record_scheduler_test_control({4, std::move(failures)});
            TemporaryRoot root = make_temporary_root(
                "record-scheduler-failure");
            ContextOptions options = context_options(root.path);
            options.preset_dirs = std::move(preset_roots);
            auto context = SdkContext::create(std::move(options));
            CHECK_FALSE(context.has_value());
            return record_scheduler_test_stats();
        };

    const RecordSchedulerTestStats interleaved = run_failure({
        {30, RecordFailureInjectionStage::effective_conversion},
        {20, RecordFailureInjectionStage::duplicate},
        {10, RecordFailureInjectionStage::freeze}});
    REQUIRE(interleaved.primary_failure_ordinal.has_value());
    CHECK(*interleaved.primary_failure_ordinal == 10);
    CHECK(interleaved.primary_failure_stage ==
          RecordFailureInjectionStage::freeze);
    CHECK(interleaved.effective_artifacts_attempted > 30);
    CHECK(interleaved.committed_records == 0);
    CHECK(interleaved.dependency_batches_started == 3);
    CHECK(interleaved.dependency_batches_joined == 3);
    CHECK(interleaved.arena_destroyed_before_commit);
    CHECK(interleaved.status_candidate_owners_after_commit == 0);
    CHECK(interleaved.artifact_value_owners_after_commit == 0);

    const std::vector<RecordFailureInjectionStage> individual_stages{
        RecordFailureInjectionStage::freeze,
        RecordFailureInjectionStage::parent_default,
        RecordFailureInjectionStage::identity,
        RecordFailureInjectionStage::duplicate,
        RecordFailureInjectionStage::effective_conversion};
    for (const RecordFailureInjectionStage stage : individual_stages) {
        CAPTURE(stage);
        const RecordSchedulerTestStats individual =
            run_failure({{12, stage}});
        REQUIRE(individual.primary_failure_ordinal.has_value());
        CHECK(*individual.primary_failure_ordinal == 12);
        REQUIRE(individual.primary_failure_stage.has_value());
        CHECK(*individual.primary_failure_stage == stage);
        CHECK(individual.effective_artifacts_attempted > 12);
        CHECK(individual.committed_records == 0);
        CHECK(individual.dependency_batches_started == 3);
        CHECK(individual.dependency_batches_joined == 3);
        CHECK(individual.status_candidate_owners_after_commit == 0);
        CHECK(individual.artifact_value_owners_after_commit == 0);
    }

    REQUIRE(reset_runtime_coordinator_for_testing());
    reset_record_scheduler_test_control();
    set_record_scheduler_test_control({4, {}});
    TemporaryRoot discovery_root =
        make_temporary_root("record-scheduler-reverse-discovery");
    const auto parent_preset_root =
        discovery_root.path / "parent-presets";
    const auto child_preset_root =
        discovery_root.path / "child-presets";
    write_process_preset(parent_preset_root, "Z SDK Grandparent", {});
    write_process_preset(
        parent_preset_root, "M SDK Parent", "Z SDK Grandparent");
    write_process_preset(
        child_preset_root, "A SDK Child", "M SDK Parent");
    const std::vector<std::filesystem::path> preset_roots{
        parent_preset_root, child_preset_root};
    {
        ContextOptions options = context_options(discovery_root.path);
        options.preset_dirs = preset_roots;
        auto context = SdkContext::create(std::move(options));
        REQUIRE(context.has_value());
        auto repository = context.value().presets();
        REQUIRE(repository.has_value());
        const std::vector<CatalogRecordSnapshot> records =
            capture_catalog(repository.value());
        const auto find_named =
            [&records](const std::string &name) {
                return std::find_if(
                    records.begin(), records.end(),
                    [&name](const CatalogRecordSnapshot &record) {
                        return record.metadata.name == name;
                    });
            };
        const auto child_record = find_named("A SDK Child");
        const auto parent_record = find_named("M SDK Parent");
        const auto grandparent_record = find_named("Z SDK Grandparent");
        REQUIRE(child_record != records.end());
        REQUIRE(parent_record != records.end());
        REQUIRE(grandparent_record != records.end());
        REQUIRE(child_record->metadata.parent.has_value());
        REQUIRE(parent_record->metadata.parent.has_value());
        CHECK(config_values_identity_for_testing(child_record->inherited) ==
              config_values_identity_for_testing(parent_record->effective));
        CHECK(config_values_identity_for_testing(parent_record->inherited) ==
              config_values_identity_for_testing(grandparent_record->effective));
    }
    const RecordSchedulerTestStats discovered =
        record_scheduler_test_stats();
    REQUIRE(discovered.reverse_dependency_child_ordinal.has_value());
    REQUIRE(discovered.reverse_dependency_parent_ordinal.has_value());
    const std::size_t child =
        *discovered.reverse_dependency_child_ordinal;
    const std::size_t parent =
        *discovered.reverse_dependency_parent_ordinal;
    REQUIRE(child < parent);

    const RecordSchedulerTestStats parent_duplicate = run_failure({
        {parent, RecordFailureInjectionStage::duplicate}}, preset_roots);
    REQUIRE(parent_duplicate.primary_failure_ordinal.has_value());
    CHECK(*parent_duplicate.primary_failure_ordinal == parent);
    CHECK(parent_duplicate.primary_failure_stage ==
          RecordFailureInjectionStage::duplicate);
    CHECK(parent_duplicate.children_completed_from_failed_parent_record > 0);
    CHECK(parent_duplicate.committed_records == 0);

    for (const RecordFailureInjectionStage stage :
         {RecordFailureInjectionStage::freeze,
          RecordFailureInjectionStage::identity}) {
        CAPTURE(stage);
        const RecordSchedulerTestStats parent_metadata =
            run_failure({{parent, stage}}, preset_roots);
        REQUIRE(parent_metadata.primary_failure_ordinal.has_value());
        CHECK(*parent_metadata.primary_failure_ordinal == parent);
        CHECK(parent_metadata.primary_failure_stage == stage);
        CHECK(parent_metadata.children_completed_from_failed_parent_record > 0);
        CHECK(parent_metadata.children_blocked_by_parent_effective_failure == 0);
        CHECK(parent_metadata.committed_records == 0);
    }

    const RecordSchedulerTestStats parent_conversion = run_failure({
        {parent, RecordFailureInjectionStage::effective_conversion}},
        preset_roots);
    REQUIRE(parent_conversion.primary_failure_ordinal.has_value());
    CHECK(*parent_conversion.primary_failure_ordinal == child);
    CHECK(parent_conversion.primary_failure_stage ==
          RecordFailureInjectionStage::parent_default);
    CHECK(parent_conversion.children_blocked_by_parent_effective_failure > 0);
    CHECK(parent_conversion.committed_records == 0);
}

TEST_CASE("ConfigSchema is discoverable and validates patches through public API",
          "[libslicer_sdk][public][schema]")
{
    TemporaryRoot root = make_temporary_root("schema");
    auto context = SdkContext::create(context_options(root.path));
    REQUIRE(context.has_value());
    auto repository = context.value().presets();
    REQUIRE(repository.has_value());

    const ConfigSchema schema = repository.value().schema();
    CHECK(schema.schema_id() == "orca.fff.config");
    CHECK(schema.schema_version() == 2);
    CHECK_FALSE(schema.options().empty());

    const auto speed = schema.find(OptionId("outer_wall_speed"));
    REQUIRE(speed.has_value());
    CHECK(speed->id == OptionId("outer_wall_speed"));
    CHECK(speed->editable);
    CHECK_FALSE(schema.find(OptionId("definitely_unknown_option")).has_value());

    auto process_presets = repository.value().list(PresetKind::process);
    REQUIRE(process_presets.has_value());
    REQUIRE_FALSE(process_presets.value().empty());
    const PresetSummary process = process_presets.value().front();
    auto process_view = repository.value().get({process.ref, process.revision});
    REQUIRE(process_view.has_value());

    ConfigValidationContext validation{OptionScope::project, std::nullopt,
                                       std::nullopt, std::nullopt, std::nullopt};
    ConfigPatch unknown;
    unknown.set(OptionId("definitely_unknown_option"), ConfigValue::integer(1));
    auto unknown_result = schema.validate_and_normalize(
        process_view.value().effective_values(), unknown, validation);
    REQUIRE_FALSE(unknown_result.has_value());
    CHECK(unknown_result.error_code() == ErrorCode::invalid_configuration);
    REQUIRE_FALSE(unknown_result.diagnostics().empty());
    check_primary_error(unknown_result.diagnostics().front(),
                        ErrorCode::invalid_configuration,
                        "/configuration/definitely_unknown_option");

    const auto filament_map = schema.find(OptionId("filament_map"));
    REQUIRE(filament_map.has_value());
    CHECK_FALSE(filament_map->editable);
    CHECK(filament_map->filament_reference == FilamentReferenceKind::none);

    ConfigPatch empty;
    auto encoded = export_json(empty, schema);
    REQUIRE(encoded.has_value());
    auto decoded = import_json(encoded.value(), schema);
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().entries().empty());
}

TEST_CASE("Preset repository public transactions persist and reject stale revisions",
          "[libslicer_sdk][public][preset]")
{
    TemporaryRoot root = make_temporary_root("presets");
    auto context = SdkContext::create(context_options(root.path));
    REQUIRE(context.has_value());
    auto repository_result = context.value().presets();
    REQUIRE(repository_result.has_value());
    PresetRepository repository = std::move(repository_result).value();

    auto process_presets = repository.list(PresetKind::process);
    REQUIRE(process_presets.has_value());
    REQUIRE_FALSE(process_presets.value().empty());
    const PresetSummary parent_summary = process_presets.value().front();
    const SelectedPreset parent{parent_summary.ref, parent_summary.revision};

    auto create = repository.create(PresetKind::process,
                                    "sdk.public.test.process",
                                    "SDK public test process",
                                    parent);
    REQUIRE(create.has_value());
    const PresetView draft = create.value().snapshot();
    CHECK(draft.metadata().ref.origin() == PresetOrigin::user);
    CHECK(draft.metadata().ref.id() == "sdk.public.test.process");
    CHECK(draft.overrides().entries().empty());
    auto commit = create.value().commit();
    REQUIRE(commit.has_value());
    CHECK(commit.value().ref.origin() == PresetOrigin::user);
    CHECK(commit.value().ref.id() == "sdk.public.test.process");
    CHECK(commit.value().name == "SDK public test process");

    const SelectedPreset selected{commit.value().ref, commit.value().revision};
    auto view = repository.get(selected);
    REQUIRE(view.has_value());
    CHECK(view.value().metadata().ref == selected.ref);
    CHECK(view.value().overrides().entries().empty());
    CHECK(view.value().effective_values().entries().size() >=
          view.value().inherited_values().entries().size());

    auto first = repository.edit(selected.ref, selected.revision);
    auto stale = repository.edit(selected.ref, selected.revision);
    REQUIRE(first.has_value());
    REQUIRE(stale.has_value());
    auto first_commit = first.value().commit();
    REQUIRE(first_commit.has_value());
    auto stale_commit = stale.value().commit();
    REQUIRE_FALSE(stale_commit.has_value());
    CHECK(stale_commit.error_code() == ErrorCode::conflict);
    REQUIRE_FALSE(stale_commit.diagnostics().empty());
    CHECK(stale_commit.diagnostics().front().severity == Severity::error);

    auto erase_read_only = repository.erase(parent.ref, parent.revision);
    REQUIRE_FALSE(erase_read_only.has_value());
    CHECK(erase_read_only.error_code() == ErrorCode::unsupported);

    auto reloaded_context = SdkContext::create(context_options(root.path));
    REQUIRE(reloaded_context.has_value());
    auto reloaded_repository = reloaded_context.value().presets();
    REQUIRE(reloaded_repository.has_value());
    auto reloaded_processes = reloaded_repository.value().list(PresetKind::process);
    REQUIRE(reloaded_processes.has_value());
    const auto reloaded = std::find_if(
        reloaded_processes.value().begin(), reloaded_processes.value().end(),
        [](const PresetSummary &summary) {
            return summary.ref.origin() == PresetOrigin::user &&
                   summary.ref.id() == "sdk.public.test.process";
        });
    REQUIRE(reloaded != reloaded_processes.value().end());
    CHECK(reloaded->name == "SDK public test process");

    REQUIRE(repository.erase(first_commit.value().ref,
                             first_commit.value().revision).has_value());
}
