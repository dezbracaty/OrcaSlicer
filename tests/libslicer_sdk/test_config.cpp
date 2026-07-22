#include <catch2/catch_test_macros.hpp>

#include <libslicer/v1/Config.hpp>
#include <libslicer/v1/ConfigSchema.hpp>
#include <libslicer/v1/Context.hpp>
#include <libslicer/v1/Preset.hpp>
#include <libslicer/v1/Result.hpp>
#include <libslicer/v1/Version.hpp>
#include <libslicer/v1/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
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

void check_primary_error(const Diagnostic &diagnostic, ErrorCode code,
                         const std::string &field)
{
    CHECK(diagnostic.code == code);
    CHECK(diagnostic.severity == Severity::error);
    CHECK(diagnostic.field == field);
    CHECK_FALSE(diagnostic.message.empty());
}

} // namespace

TEST_CASE("Public SDK exposes its v1 semantic version", "[libslicer_sdk][public][version]")
{
    CHECK(sdk_version_major == 1);
    CHECK(sdk_version_minor == 0);
    CHECK(sdk_version_patch == 0);
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
