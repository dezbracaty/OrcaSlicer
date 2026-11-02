#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/FlushVolCalc.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include <stdexcept>

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TempPresetDir {
    fs::path path;

    TempPresetDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("orcaslicer-preset-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }

    ~TempPresetDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_print_preset(const DynamicPrintConfig &default_config, const fs::path &file, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Write a preset json carrying a name and an "inherits" value, using the given collection's
// default config so it loads back into that collection. Works for any preset type.
void write_preset_with_inherits(const DynamicPrintConfig &default_config, const fs::path &file,
                                const std::string &name, const std::string &inherits)
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Add an in-memory preset (no file) with the given inherits value (empty => root preset).
Preset &add_inmemory_preset(PresetCollection &coll, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

// Mark an already-loaded preset as renamed from one or more former names.
void set_renamed_from(PresetCollection &coll, const std::string &preset_name, std::vector<std::string> old_names)
{
    for (auto it = coll.begin(); it != coll.end(); ++it)
        if (it->name == preset_name)
            it->renamed_from = std::move(old_names);
}

// A standalone print preset collection that exposes the protected rename-map builder, so a
// renamed_from scenario can be set up without the full system-profile load pipeline.
// (PresetCollection is non-copyable - it holds a mutex - so it is constructed directly with
// the same type/keys/defaults PresetBundle uses for its print collection.)
struct RenameTestCollection : public PresetCollection
{
    RenameTestCollection()
        : PresetCollection(Preset::TYPE_PRINT, Preset::print_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
    using PresetCollection::update_map_system_profile_renamed;
};

} // namespace

TEST_CASE("Preset identity is canonicalized from load path", "[Preset][Identity]")
{
    TempPresetDir              temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_PRINT_NAME / "User.json", "User");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_LOCAL_DIR / "bundle-1" / PRESET_PRINT_NAME / "LocalBundle.json", "LocalBundle");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1" / PRESET_PRINT_NAME / "Subscribed.json", "Subscribed");

    bundle.prints.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_LOCAL_DIR / "bundle-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root_user = bundle.prints.find_preset("User");
    REQUIRE(root_user != nullptr);
    CHECK(root_user->name == "User");
    CHECK_FALSE(root_user->is_from_bundle());

    const Preset *local_bundle = bundle.prints.find_preset("_local/bundle-1/LocalBundle");
    REQUIRE(local_bundle != nullptr);
    CHECK(local_bundle->name == "_local/bundle-1/LocalBundle");
    CHECK(local_bundle->is_from_bundle());

    const Preset *subscribed = bundle.prints.find_preset("_subscribed/remote-1/Subscribed");
    REQUIRE(subscribed != nullptr);
    CHECK(subscribed->name == "_subscribed/remote-1/Subscribed");
    CHECK(subscribed->is_from_bundle());
}

TEST_CASE("Legacy bundle import without bundle metadata stays in the user preset directory", "[Preset][Identity]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    std::string                file      = (temp_dir.path / "legacy-bundle" / "Imported.json").string();
    const fs::path             user_root = temp_dir.path / "user";

    write_print_preset(bundle.prints.default_preset().config, file, "Imported");
    fs::create_directories(user_root);
    bundle.prints.update_user_presets_directory(user_root.string(), PRESET_PRINT_NAME);

    REQUIRE(bundle.import_json_presets(
        substitutions,
        file,
        [](std::string const &) { return 1; },
        ForwardCompatibilitySubstitutionRule::Disable,
        overwrite,
        result));

    const Preset *imported = bundle.prints.find_preset("Imported");
    REQUIRE(imported != nullptr);
    CHECK(imported->name == "Imported");
    CHECK(imported->bundle_id.empty());
    CHECK_FALSE(imported->is_from_bundle());
    // Detached user presets (no inherits) are saved in the "base" subfolder of the user preset root.
    CHECK(fs::equivalent(fs::path(imported->file).parent_path().parent_path(), user_root / PRESET_PRINT_NAME));
}

TEST_CASE("Current vendor type tolerates missing printer model", "[Preset][Bundle]")
{
    PresetBundle bundle;

    VendorProfile orca_vendor("ORCA");
    VendorProfile::PrinterModel model;
    model.name = "Orca Test";
    orca_vendor.models.emplace_back(model);
    bundle.vendors.emplace("ORCA", std::move(orca_vendor));

    bundle.printers.get_edited_preset().config.erase("printer_model");

    CHECK(bundle.get_current_vendor_type() == VendorType::Unknown);
}

TEST_CASE("Printer extruder count tolerates missing nozzle diameter", "[Preset][Bundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;

    config.erase("nozzle_diameter");
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats());
    CHECK(bundle.get_printer_extruder_count() == 1);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    CHECK(bundle.get_printer_extruder_count() == 2);
}

TEST_CASE("find_preset resolves a system preset's renamed_from", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" is the current preset; it was renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // The rename map knows the old name...
    const std::string *renamed = coll.get_preset_name_renamed("Old Process");
    REQUIRE(renamed != nullptr);
    CHECK(*renamed == "New Process");

    // ...and plain find_preset() now follows it (the core of this PR; previously this
    // resolution lived only in find_preset2 and a few call sites).
    const Preset *resolved = coll.find_preset("Old Process");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == "New Process");

    // A genuinely unknown name still returns null (no spurious match).
    CHECK(coll.find_preset("Totally Unknown") == nullptr);

    // A child that still inherits the OLD name resolves through the runtime walker,
    // which uses plain find_preset().
    Preset       &child  = add_inmemory_preset(coll, "Child Process", "Old Process");
    const Preset *parent = coll.get_preset_parent(child);
    REQUIRE(parent != nullptr);
    CHECK(parent->name == "New Process");
}

TEST_CASE("find_preset resolves a preset renamed more than once", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" was renamed twice, so it carries both former names in renamed_from.
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Original Process", "Old Process" });
    coll.update_map_system_profile_renamed();

    // Each historical name resolves to the current preset.
    for (const char *old_name : { "Original Process", "Old Process" }) {
        INFO("resolving old name: " << old_name);
        const std::string *renamed = coll.get_preset_name_renamed(old_name);
        REQUIRE(renamed != nullptr);
        CHECK(*renamed == "New Process");

        const Preset *resolved = coll.find_preset(old_name);
        REQUIRE(resolved != nullptr);
        CHECK(resolved->name == "New Process");
    }

    // A child inheriting either former name resolves through the runtime walker.
    Preset &child = add_inmemory_preset(coll, "Child Process", "Original Process");
    REQUIRE(coll.get_preset_parent(child) != nullptr);
    CHECK(coll.get_preset_parent(child)->name == "New Process");
}

TEST_CASE("find_preset2 auto-matches removed Generic vendor profiles to the library", "[Preset][Rename]")
{
    PresetBundle bundle;

    // The OrcaFilamentLibrary replacement that removed empty "<vendor> Generic" profiles map to.
    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // Plain lookups do NOT fuzzy-match a removed vendor profile.
    CHECK(bundle.filaments.find_preset("Voron Generic PLA") == nullptr);
    CHECK(bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/false) == nullptr);

    // With auto_match, the removed "Voron Generic PLA" resolves to "Generic PLA @System".
    const Preset *matched = bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/true);
    REQUIRE(matched != nullptr);
    CHECK(matched->name == "Generic PLA @System");

    // No library preset exists for an unrelated material => still no match.
    CHECK(bundle.filaments.find_preset2("BrandX Generic PETG", /*auto_match=*/true) == nullptr);
}

TEST_CASE("Renamed parent is normalized into a loaded preset's inherits", "[Preset][Rename]")
{
    TempPresetDir        temp_dir;
    RenameTestCollection coll;

    // Current parent, renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // A user preset on disk that still inherits the OLD name.
    write_preset_with_inherits(coll.default_preset().config,
                               temp_dir.path / PRESET_PRINT_NAME / "Child.json", "Child", "Old Process");

    PresetsConfigSubstitutions substitutions;
    coll.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions,
                      ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = coll.find_preset("Child");
    REQUIRE(child != nullptr);
    // The dangling "Old Process" was rewritten to the resolved parent name at load time,
    // so the runtime walker (plain find_preset) can resolve the chain.
    CHECK(child->inherits() == "New Process");
    REQUIRE(coll.get_preset_parent(*child) != nullptr);
    CHECK(coll.get_preset_parent(*child)->name == "New Process");
}

TEST_CASE("Removed Generic parent is normalized into a loaded filament's inherits", "[Preset][Rename]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // A user filament that still inherits a removed "<vendor> Generic PLA" profile.
    write_preset_with_inherits(bundle.filaments.default_preset().config,
                               temp_dir.path / PRESET_FILAMENT_NAME / "MyPLA.json", "MyPLA", "Voron Generic PLA");

    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(temp_dir.path.string(), PRESET_FILAMENT_NAME, substitutions,
                                  ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = bundle.filaments.find_preset("MyPLA");
    REQUIRE(child != nullptr);
    CHECK(child->inherits() == "Generic PLA @System");
    REQUIRE(bundle.filaments.get_preset_parent(*child) != nullptr);
    CHECK(bundle.filaments.get_preset_parent(*child)->name == "Generic PLA @System");
}

// Reference expectations below come from Orca 3c8c667d's GUI_ObjectList and
// Plater data updates, including their non-symmetric support/custom-code writes.
// See tests/data/filament_operations/sources.json for the extraction contract.
TEST_CASE("Orca object deletion preserves absent automatic and explicit assignments", "[filament-references]")
{
    const int before     = GENERATE(-1, 0, 1, 2, 3);
    const int expected[] = {1, 0, 1, 1, 2};
    CAPTURE(before);
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    auto* volume = object->add_volume(make_cube(10, 10, 10));
    if (before >= 0) {
        object->config.set("extruder", before);
        volume->config.set("extruder", before);
    }
    object->update_filament_references_after_delete(1, -1); // remove material 2
    CHECK(object->config.extruder() == expected[before + 1]);
    if (before < 0)
        CHECK_FALSE(volume->config.has("extruder"));
    else
        CHECK(volume->config.extruder() == expected[before + 1]);
}

TEST_CASE("Orca replacement uses the post-delete target for explicit and layer references", "[filament-references]")
{
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    object->add_volume(make_cube(10, 10, 10));
    object->add_volume(make_cube(10, 10, 10))->config.set("extruder", 2);
    object->config.set("extruder", 2);
    object->layer_config_ranges[{0., 1.}].set("extruder", 2);
    object->layer_config_ranges[{1., 2.}].set("extruder", 3);
    object->layer_config_ranges[{2., 3.}].set("extruder", 0);
    object->update_filament_references_after_delete(1, 1); // target old material 3 -> new 2
    CHECK(object->config.extruder() == 2);
    CHECK(object->volumes[1]->config.extruder() == 2);
    CHECK(object->layer_config_ranges.at({0., 1.}).extruder() == 2);
    CHECK(object->layer_config_ranges.at({1., 2.}).extruder() == 2);
    CHECK(object->layer_config_ranges.at({2., 3.}).extruder() == 0);

    object->update_filament_references_after_delete(1, -1);
    CHECK(object->layer_config_ranges.at({0., 1.}).extruder() == 0);
    CHECK(object->layer_config_ranges.at({1., 2.}).extruder() == 0);
}

TEST_CASE("Orca single-volume aliases depend on instance and layer children", "[filament-references]")
{
    const int tree = GENERATE(0, 1, 2, 3);
    // 0: object alias; 1: explicit volume nodes; 2: instance subtree; 3: layer subtree.
    CAPTURE(tree);
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    auto* volume = object->add_volume(make_cube(10, 10, 10));
    object->config.set("extruder", 1);
    volume->config.set("extruder", 2);
    if (tree == 1)
        object->add_volume(make_cube(10, 10, 10));
    if (tree == 2)
        object->add_instance();
    if (tree == 3)
        object->layer_config_ranges[{0., 1.}].set("extruder", 2);
    object->update_filament_references_after_delete(1, -1);
    CHECK(volume->config.extruder() == (tree < 2 ? 1 : 2));
    if (tree == 3)
        CHECK(object->layer_config_ranges.at({0., 1.}).extruder() == 0);
}

TEST_CASE("Orca support deletion retains its original object-volume asymmetry", "[filament-references]")
{
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    auto* volume = object->add_volume(make_cube(10, 10, 10));
    object->config.set("extruder", 1);
    object->config.set("support_filament", 2);
    object->config.set("support_interface_filament", 3);
    object->update_filament_references_after_delete(1, -1);
    CHECK_FALSE(object->config.has("support_filament"));
    CHECK(object->config.opt_int("support_interface_filament") == 2);

    // The pinned GUI writes this adjusted volume value to the OBJECT, not to
    // the volume. The test intentionally records that upstream behavior.
    object->config.set("support_filament", 1);
    volume->config.set("support_filament", 3);
    volume->config.set("support_interface_filament", 2);
    object->update_filament_references_after_delete(1, -1);
    CHECK(object->config.opt_int("support_filament") == 2);
    CHECK(volume->config.opt_int("support_filament") == 3);
    CHECK_FALSE(volume->config.has("support_interface_filament"));
}

TEST_CASE("Orca append normalizes stale overrides but keeps automatic and inherited values", "[filament-references]")
{
    const int before = GENERATE(-1, 0, 2, 5);
    CAPTURE(before);
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    auto* inherited     = object->add_volume(make_cube(10, 10, 10));
    auto* explicit_part = object->add_volume(make_cube(10, 10, 10));
    if (before >= 0)
        object->config.set("extruder", before);
    explicit_part->config.set("extruder", 5);
    explicit_part->config.set("support_filament", 5);
    object->config.set("support_interface_filament", 5);
    object->update_filament_references_after_append(4);
    CHECK(object->config.extruder() == (before < 0 || before > 4 ? 1 : before));
    CHECK_FALSE(inherited->config.has("extruder"));
    CHECK_FALSE(explicit_part->config.has("extruder"));
    CHECK_FALSE(explicit_part->config.has("support_filament"));
    CHECK_FALSE(object->config.has("support_interface_filament"));
    CHECK(explicit_part->extruder_id() == object->config.extruder());
}

TEST_CASE("Model deletion remaps painted faces through the original triangle selector", "[filament-references]")
{
    const int replacement = GENERATE(-1, 0);
    CAPTURE(replacement);
    Model model;
    auto* object = model.add_object();
    object->add_instance();
    object->config.set("extruder", 1);
    auto* volume = object->add_volume(make_cube(10, 10, 10));
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(2));
    selector.set_facet(1, EnforcerBlockerType(3));
    volume->mmu_segmentation_facets.set(selector);
    REQUIRE(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType(2)).indices.size() == 1);
    REQUIRE(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType(3)).indices.size() == 1);
    model.update_filaments_after_delete(2, 1, replacement);
    CHECK(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType(2)).indices.size() == 1);
    CHECK(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType(3)).indices.empty());
    CHECK(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType(1)).indices.size() == (replacement < 0 ? 0 : 1));
    CHECK(volume->mmu_segmentation_facets.get_facets(*volume, EnforcerBlockerType(0)).indices.size() == (replacement < 0 ? 11 : 10));
}

TEST_CASE("Model deletion preserves non-tool custom code and pinned replacement-tail behavior", "[filament-references]")
{
    const int replacement = GENERATE(-1, 0);
    CAPTURE(replacement);
    Model model;
    auto& codes = model.plates_custom_gcodes[0].gcodes;
    codes.push_back({0.2, CustomGCode::ToolChange, 2, "", ""});
    codes.push_back({0.4, CustomGCode::ToolChange, 3, "", ""});
    codes.push_back({0.6, CustomGCode::PausePrint, 2, "", "pause"});
    model.update_filaments_after_delete(2, 1, replacement);
    REQUIRE(codes.size() == (replacement < 0 ? 2 : 3));
    CHECK(codes[0].print_z == 0.4);
    CHECK(codes[0].type == CustomGCode::ToolChange);
    CHECK(codes[0].extruder == 2);
    CHECK(codes[1].print_z == 0.6);
    CHECK(codes[1].type == CustomGCode::PausePrint);
    CHECK(codes[1].extruder == 2);
    CHECK(codes[1].extra == "pause");
    if (replacement >= 0) {
        // remove_if's tail is retained and its first extruder overwritten by
        // Plater::on_filaments_delete. It is not a newly invented merge rule.
        CHECK(codes[2].type == CustomGCode::PausePrint);
        CHECK(codes[2].print_z == 0.6);
        CHECK(codes[2].extruder == 1);
    }
}

TEST_CASE("Unsupported object trees fail before other model references are changed", "[filament-references]")
{
    Model model;
    auto* first = model.add_object();
    first->add_instance();
    first->add_volume(make_cube(10, 10, 10));
    first->config.set("extruder", 3);
    auto* unsupported = model.add_object();
    unsupported->add_instance();
    unsupported->add_volume(make_cube(10, 10, 10));
    SECTION("cut topology")
    {
        unsupported->cut_id = CutObjectBase(ObjectID(123), 1, 0);
        REQUIRE_THROWS_AS(model.update_filaments_after_append(4), std::invalid_argument);
        REQUIRE_THROWS_AS(model.update_filaments_after_delete(2, 1, -1), std::invalid_argument);
    }
    SECTION("layer lacking the original required extruder")
    {
        unsupported->layer_config_ranges[{0., 1.}].set("layer_height", 0.2);
        REQUIRE_THROWS_AS(model.update_filaments_after_delete(2, 1, -1), std::invalid_argument);
    }
    CHECK(first->config.extruder() == 3);
    CHECK_FALSE(first->volumes[0]->config.has("extruder"));
}

TEST_CASE("Appending a material preserves user flushing entries and computes new colour transitions", "[filament-references]")
{
    const bool new_is_support = GENERATE(false, true);
    CAPTURE(new_is_support);
    PresetBundle bundle;
    add_inmemory_preset(bundle.filaments, "Flush Reference A");
    add_inmemory_preset(bundle.filaments, "Flush Reference B");
    auto& added = add_inmemory_preset(bundle.filaments, "Flush Reference Added");
    added.config.set_key_value("filament_is_support", new ConfigOptionBools({new_is_support}));
    bundle.filament_presets = {"Flush Reference A", "Flush Reference B"};
    bundle.set_num_filaments(2);
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values     = {"#111111", "#EEEEEE"};
    bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values = {0., 123., 456., 0.};

    // The real defaults are single-nozzle and have no cut-retraction reduction:
    // their minimum flush volume is zero, not a fabricated machine contract.
    REQUIRE(bundle.get_printer_extruder_count() == 1);
    const auto before = bundle.full_config();
    REQUIRE(before.option<ConfigOptionFloatsNullable>("nozzle_volume")->get_at(0) == 0.);
    REQUIRE(before.opt_int("enable_long_retraction_when_cut") == 0);
    const int dataset = before.option<ConfigOptionIntsNullable>("nozzle_flush_dataset")->get_at(0);
    FlushVolCalculator reference(0, g_max_flush_volume, dataset);
    const int a_to_added = reference.calc_flush_vol(255, 17, 17, 17, 255, 170, 51, 119);
    const int b_to_added = reference.calc_flush_vol(255, 238, 238, 238, 255, 170, 51, 119);
    const int added_to_a = reference.calc_flush_vol(255, 170, 51, 119, 255, 17, 17, 17);
    const int added_to_b = reference.calc_flush_vol(255, 170, 51, 119, 255, 238, 238, 238);

    Model model;
    std::vector<DynamicPrintConfig*> plates;
    bundle.append_project_filament(model, plates, "#AA3377", std::nullopt, "Flush Reference Added");
    const auto& matrix = bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    REQUIRE(matrix.size() == 9);
    CHECK(matrix[0] == 0.);
    CHECK(matrix[1] == 123.);
    CHECK(matrix[3] == 456.);
    CHECK(matrix[4] == 0.);
    CHECK(matrix[8] == 0.);
    CHECK(matrix[2] == (new_is_support ? g_flush_volume_to_support : a_to_added));
    CHECK(matrix[5] == (new_is_support ? g_flush_volume_to_support : b_to_added));
    CHECK(matrix[6] == (new_is_support ? std::max(g_min_flush_volume_from_support, added_to_a) : added_to_a));
    CHECK(matrix[7] == (new_is_support ? std::max(g_min_flush_volume_from_support, added_to_b) : added_to_b));
    CHECK(bundle.filament_presets.back() == "Flush Reference Added");
}

TEST_CASE("Original plate sequences expand on append and compact on material deletion", "[filament-references]")
{
    PresetBundle bundle;
    add_inmemory_preset(bundle.filaments, "Sequence Reference A");
    add_inmemory_preset(bundle.filaments, "Sequence Reference B");
    bundle.filament_presets = {"Sequence Reference A", "Sequence Reference B"};
    bundle.set_num_filaments(2);
    REQUIRE(bundle.get_printer_extruder_count() == 1);

    DynamicPrintConfig plate;
    plate.set_key_value("filament_map", new ConfigOptionInts({1, 1}));
    plate.set_key_value("first_layer_print_sequence", new ConfigOptionInts({2, 1}));
    const std::vector<LayerPrintSequence> original_layers{{{1, 3}, {2, 1}}, {{4, 8}, {1, 2}}};
    int sequence_count;
    std::vector<int> encoded;
    get_other_layers_print_sequence(original_layers, sequence_count, encoded);
    plate.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(sequence_count));
    plate.set_key_value("other_layers_print_sequence", new ConfigOptionInts(encoded));
    Model model;
    std::vector<DynamicPrintConfig*> plates{&plate};
    bundle.append_project_filament(model, plates, "#AA3377");
    CHECK((plate.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{1, 1, 1}));
    CHECK((plate.option<ConfigOptionInts>("first_layer_print_sequence")->values == std::vector<int>{2, 1, 3}));
    auto layers = get_other_layers_print_sequence(plate.opt_int("other_layers_print_sequence_nums"),
                                                  plate.option<ConfigOptionInts>("other_layers_print_sequence")->values);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].first == original_layers[0].first);
    CHECK(layers[1].first == original_layers[1].first);
    CHECK((layers[0].second == std::vector<int>{2, 1, 3}));
    CHECK((layers[1].second == std::vector<int>{1, 2, 3}));

    bundle.prints.get_edited_preset().config.set("support_filament", 2);
    bundle.prints.get_edited_preset().config.set("support_interface_filament", 3);
    bundle.remove_project_filament(model, plates, 1, -1);
    CHECK((plate.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{1, 1}));
    CHECK((plate.option<ConfigOptionInts>("first_layer_print_sequence")->values == std::vector<int>{1, 2}));
    layers = get_other_layers_print_sequence(plate.opt_int("other_layers_print_sequence_nums"),
                                             plate.option<ConfigOptionInts>("other_layers_print_sequence")->values);
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].first == original_layers[0].first);
    CHECK(layers[1].first == original_layers[1].first);
    CHECK((layers[0].second == std::vector<int>{1, 2}));
    CHECK((layers[1].second == std::vector<int>{1, 2}));
    CHECK_FALSE(bundle.prints.get_edited_preset().config.has("support_filament"));
    CHECK(bundle.prints.get_edited_preset().config.opt_int("support_interface_filament") == 2);
}
