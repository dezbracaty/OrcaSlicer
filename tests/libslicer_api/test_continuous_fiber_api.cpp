#include <catch2/catch_all.hpp>
#include <libslicer/Library.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
namespace {
std::filesystem::path assets(){return std::filesystem::path(LIBSLICER_TEST_DATA_DIR)/"continuous_fiber";}
nlohmann::json read(const std::filesystem::path& path){std::ifstream stream(path);nlohmann::json out;stream>>out;return out;}
libslicer::Config fixture_config(const nlohmann::json& extra){auto config=libslicer::Config::defaults();auto values=read(assets()/"manual_two_tool.json");if(!extra.is_null())values.update(extra);for(auto it=values.begin();it!=values.end();++it){CAPTURE(it.key());REQUIRE(config.set(it.key(),it.value().get<std::string>()).success);}return config;}
auto library(){libslicer::LibraryOptions options;options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;return libslicer::Library::open(options);}
void assert_fiber(const libslicer::ToolpathPreview& preview){using namespace libslicer;size_t starts=0,cuts=0,tails=0,active=0;
 for(const auto& event:preview.events){starts+=event.kind==ToolpathEventKind::FiberStart;cuts+=event.kind==ToolpathEventKind::FiberCut;}
 for(const auto& segment:preview.segments)if(segment.deposition_process==ToolpathDepositionProcess::Fiber){++active;CHECK(segment.filament_id==1);if(segment.fiber_tail){++tails;CHECK(segment.extrusion_delta_mm==0);CHECK(segment.nominal_deposition_length_mm>0);}}
 CHECK(active>0);CHECK(starts>0);CHECK(cuts==starts);CHECK(tails>0);CHECK(preview.statistics.total_fiber_deposition_length_mm>0);
}
}
TEST_CASE("A1 A2 A3 A4 A7 A9 A10 SDK frozen representative matrix", "[libslicer_api][fiber][matrix]") {
 auto sdk=library();REQUIRE(sdk);const auto manifest=read(assets()/"matrix.json");
 for(const auto& item:manifest["cases"]){const std::string id=item["id"];if(id=="A0"||id.rfind("A11",0)==0||id.rfind("A12",0)==0||id.rfind("A13",0)==0)continue;
  DYNAMIC_SECTION(id){auto config=fixture_config(item["patch"]);libslicer::SliceRequest request;request.config=config.snapshot();request.objects.push_back({(assets()/item["model"].get<std::string>()).string(),{}});const auto result=sdk->slice(request);
   for(const auto& diagnostic:result.diagnostics)UNSCOPED_INFO(diagnostic.message);REQUIRE(result.success);REQUIRE(result.preview);assert_fiber(*result.preview);
   if(id=="A10_swap")for(const auto& segment:result.preview->segments)if(segment.deposition_process==libslicer::ToolpathDepositionProcess::Fiber)CHECK(segment.tool_id==0);
   if(id.rfind("A3_",0)==0){const int interval=std::stoi(id.substr(3));for(const auto& segment:result.preview->segments)if(segment.deposition_process==libslicer::ToolpathDepositionProcess::Fiber)CHECK(segment.layer_index%interval==0);}
  }
 }
}
TEST_CASE("A11 two objects and same-material regions preserve ownership", "[libslicer_api][fiber][A11]") {
 auto sdk=library();REQUIRE(sdk);auto config=fixture_config({{"skirt_loops","1"},{"brim_width","2"},{"brim_type","outer_only"}});libslicer::SliceRequest request;request.config=config.snapshot();request.objects.push_back({(assets()/"rectangle.obj").string(),{}});auto second=request.objects.front();second.transform[3]=90;request.objects.push_back(second);
 auto result=sdk->slice(request);for(const auto& d:result.diagnostics)UNSCOPED_INFO(d.message);REQUIRE(result.success);REQUIRE(result.preview);assert_fiber(*result.preview);std::set<std::uint32_t> objects;for(const auto& s:result.preview->segments)if(s.deposition_process==libslicer::ToolpathDepositionProcess::Fiber)objects.insert(s.object_id);CHECK(objects.size()==2);
}
TEST_CASE("A13 unsupported combinations diagnose only active fiber", "[libslicer_api][fiber][A13]") {
 auto sdk=library();REQUIRE(sdk);
 auto early=fixture_config({});const auto before=early.snapshot();const auto rejected=early.set("fiber_layer_height_ratio","0");CHECK_FALSE(rejected.success);REQUIRE_FALSE(rejected.diagnostics.empty());CHECK(rejected.diagnostics.front().key=="fiber_layer_height_ratio");CHECK(early.snapshot().values()==before.values());
 for(const auto& invalid:nlohmann::json::array({{{"filament_map","1,1"}},{{"reinforced_infill_filament","99"}}})){CAPTURE(invalid.dump());auto config=fixture_config(invalid);libslicer::SliceRequest request;request.config=config.snapshot();request.objects.push_back({(assets()/"rectangle.obj").string(),{}});auto result=sdk->slice(request);CHECK_FALSE(result.success);CHECK_FALSE(result.diagnostics.empty());}
 auto normal=fixture_config({{"generate_reinforced_perimeters","0"},{"generate_reinforced_infills","0"},{"filament_map","1,1"}});libslicer::SliceRequest request;request.config=normal.snapshot();request.objects.push_back({(assets()/"rectangle.obj").string(),{}});CHECK(sdk->slice(request).success);
}
namespace {
nlohmann::json fiber_semantics(const libslicer::ToolpathPreview& preview){nlohmann::json result;result["segments"]=nlohmann::json::array();result["events"]=nlohmann::json::array();
 for(const auto& s:preview.segments)result["segments"].push_back({int(s.motion),int(s.deposition_process),int(s.phase),s.fiber_tail,s.fiber_path_id,s.object_id,s.instance_id,s.tool_id,s.filament_id,s.start_mm.x,s.start_mm.y,s.start_mm.z,s.end_mm.x,s.end_mm.y,s.end_mm.z,s.extrusion_delta_mm,s.nominal_speed_mm_s,s.nominal_deposition_length_mm});
 for(const auto& e:preview.events)if(e.kind==libslicer::ToolpathEventKind::FiberStart||e.kind==libslicer::ToolpathEventKind::FiberCut)result["events"].push_back({int(e.kind),e.fiber_path_id,e.object_id,e.instance_id,e.tool_id,e.filament_id,e.position_mm.x,e.position_mm.y,e.position_mm.z});return result;
}
}
TEST_CASE("A12 reslice cancellation preserves saved output and reimport semantics", "[libslicer_api][fiber][A12]") {
 auto sdk=library();REQUIRE(sdk);
 auto config=fixture_config({});
 libslicer::SliceRequest request;request.config=config.snapshot();
 request.objects.push_back({(assets()/"rectangle.obj").string(),{}});
 request.output_gcode_path=(std::filesystem::current_path()/"fiber-A12-semantics.gcode").string();
 request.output_gcode_3mf_path=(std::filesystem::current_path()/"fiber-A12-semantics.gcode.3mf").string();
 auto initial=sdk->slice(request);REQUIRE(initial.success);REQUIRE(initial.preview);assert_fiber(*initial.preview);
 REQUIRE(config.set("fiber_layer_height_ratio","2").success);request.config=config.snapshot();
 auto changed=sdk->slice(request);REQUIRE(changed.success);REQUIRE(changed.preview);
 const auto saved_text=[&request](){std::ifstream input(request.output_gcode_path);REQUIRE(input.is_open());return std::string((std::istreambuf_iterator<char>(input)),{});};
 const auto saved=saved_text();REQUIRE_FALSE(saved.empty());
 const auto package_path=request.output_gcode_3mf_path;
 const auto saved_package=[&package_path](){std::ifstream input(package_path,std::ios::binary);REQUIRE(input.is_open());return std::string((std::istreambuf_iterator<char>(input)),{});};
 const auto packaged=saved_package();REQUIRE_FALSE(packaged.empty());
 libslicer::SliceCallbacks cancel;cancel.is_cancelled=[](){return true;};
 const auto cancelled=sdk->slice(request,cancel);CHECK(cancelled.cancelled);CHECK_FALSE(cancelled.success);
 CHECK(saved_text()==saved);
 CHECK(saved_package()==packaged);
 auto retried=sdk->slice(request);REQUIRE(retried.success);REQUIRE(retried.preview);
 CHECK(fiber_semantics(*changed.preview)==fiber_semantics(*retried.preview));
 auto fresh_library=library();REQUIRE(fresh_library);
 auto fresh=fresh_library->slice(request);REQUIRE(fresh.success);REQUIRE(fresh.preview);
 CHECK(fiber_semantics(*fresh.preview)==fiber_semantics(*changed.preview));
 libslicer::GCodePreviewRequest imported;imported.gcode_path=request.output_gcode_path;
 auto reloaded=sdk->load_gcode_preview(imported);REQUIRE(reloaded.success);REQUIRE(reloaded.preview);assert_fiber(*reloaded.preview);
 CHECK(fiber_semantics(*reloaded.preview)==fiber_semantics(*fresh.preview));
 CHECK(reloaded.preview->statistics.total_fiber_deposition_length_mm==Catch::Approx(fresh.preview->statistics.total_fiber_deposition_length_mm));
 std::filesystem::remove(request.output_gcode_path);
 std::filesystem::remove(request.output_gcode_3mf_path);
}
TEST_CASE("A13 belt object-order and dedicated support conflict diagnostics identify keys", "[libslicer_api][fiber][A13]") {
 auto sdk=library();REQUIRE(sdk);
 for(const auto& patch:nlohmann::json::array({{{"printer_structure","belt"}},{{"print_sequence","by object"}},{{"enable_support","1"},{"support_filament","2"}},{{"infill_combination","1"}}})){
  auto config=fixture_config(patch);libslicer::SliceRequest request;request.config=config.snapshot();request.objects.push_back({(assets()/"rectangle.obj").string(),{}});auto result=sdk->slice(request);CHECK_FALSE(result.success);REQUIRE_FALSE(result.diagnostics.empty());std::string diagnostics;for(const auto& d:result.diagnostics)diagnostics+=d.message;CHECK(diagnostics.find("Continuous fiber configuration conflict")!=std::string::npos);
 }
}
TEST_CASE("Legacy ineffective controls remain parseable and persist without invented behavior", "[libslicer_api][fiber][config]") {
 auto configuration=libslicer::Config::defaults();
 const auto inactive_values=nlohmann::json::object({{"fiber_travel_max_length","5"},{"fiber_infill_arc_ratio","1"},{"fiber_end_min_length","5"},{"fiber_middle_min_length","0"},{"fiber_slow_length","2"},{"fiber_start_max_speed","8"},{"fiber_normal_min_limit_speed","2"},{"fiber_finish_min_limit_speed","3"}});
 for(const auto& item:inactive_values.items()){
  CAPTURE(item.key());CHECK(configuration.set(item.key(),item.value().get<std::string>()).success);
  REQUIRE(configuration.set(item.key(),"7").success);
  const auto saved=configuration.snapshot();CHECK(saved.value(item.key())=="7");
  const auto definitions=configuration.settings();const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto& definition){return definition.key==item.key();});
  REQUIRE(found!=definitions.end());CHECK_FALSE(found->visible);CHECK(found->value=="7");
  REQUIRE(configuration.reset(item.key()).success);CHECK(saved.value(item.key())=="7");
 }
}

TEST_CASE("Changing the fiber pattern does not mutate the perimeter enable flag", "[libslicer_api][fiber][config]") {
 auto configuration=libslicer::Config::defaults();
 REQUIRE(configuration.set("generate_reinforced_perimeters","0").success);
 REQUIRE(configuration.set("reinforced_infill_pattern","rectilinear").success);
 CHECK(configuration.snapshot().value("generate_reinforced_perimeters")=="0");
 REQUIRE(configuration.set("generate_reinforced_perimeters","1").success);
 REQUIRE(configuration.set("reinforced_infill_pattern","concentric").success);
 CHECK(configuration.snapshot().value("generate_reinforced_perimeters")=="1");
}

TEST_CASE("Malformed external fiber tags degrade to ordinary preview motion", "[libslicer_api][fiber][gcode]") {
 const auto path=std::filesystem::current_path()/"malformed-fiber-tags.gcode";
 {
  std::ofstream out(path);
  out<<"G21\nG90\nM83\n;LAYER:0\nG1 X0 Y0 Z0.2 F1200\n"
       ";FIBER_PHASE invalid\n;FIBER_BEGIN v=2 path=bad object=x instance=0 width=-1\n"
       ";FIBER_START\nG1 X20 Y0 E1 F600\n;FIBER_END\n";
  REQUIRE(out.good());
 }
 auto sdk=library();REQUIRE(sdk);libslicer::GCodePreviewRequest request;request.gcode_path=path.string();
 const auto imported=sdk->load_gcode_preview(request);std::filesystem::remove(path);
 REQUIRE(imported.success);REQUIRE(imported.preview);CHECK_FALSE(imported.diagnostics.empty());
 CHECK(std::all_of(imported.preview->segments.begin(),imported.preview->segments.end(),[](const auto& segment){return segment.deposition_process==libslicer::ToolpathDepositionProcess::Plastic;}));
}

TEST_CASE("Manual mapping project keys are writable without changing the Auto default", "[libslicer_api][fiber][config]") {
 auto configuration=libslicer::Config::defaults();CHECK(configuration.snapshot().value("filament_map_mode")=="Auto For Flush");
 const auto settings=configuration.settings();
 for(const std::string key:{"filament_map","filament_map_mode"}){const auto found=std::find_if(settings.begin(),settings.end(),[&](const auto& item){return item.key==key;});REQUIRE(found!=settings.end());CHECK(found->group==libslicer::SettingGroup::Printer);CHECK(found->visible);CHECK_FALSE(found->read_only);}
 REQUIRE(configuration.apply_patch({{"filament_map_mode","Manual"},{"filament_map","1,2"}}).success);
 const auto saved=configuration.snapshot();CHECK(saved.value("filament_map_mode")=="Manual");CHECK(saved.value("filament_map")=="1,2");
 REQUIRE(configuration.reset("filament_map_mode").success);CHECK(configuration.snapshot().value("filament_map_mode")=="Auto For Flush");CHECK(saved.value("filament_map_mode")=="Manual");
 auto restored=libslicer::Config::defaults();REQUIRE(restored.apply_patch({{"filament_map_mode",*saved.value("filament_map_mode")},{"filament_map",*saved.value("filament_map")}}).success);CHECK(restored.snapshot().value("filament_map_mode")==saved.value("filament_map_mode"));CHECK(restored.snapshot().value("filament_map")==saved.value("filament_map"));
}

TEST_CASE("Two tool project purge dimensions persist and invalid sizes remain rejected", "[libslicer_api][fiber][config]") {
 auto sdk=library();REQUIRE(sdk);auto configuration=fixture_config({});
 const auto saved=configuration.snapshot();CHECK(saved.value("flush_multiplier")=="0.3,0.3");CHECK(saved.value("flush_volumes_matrix")=="0,280,280,0,0,280,280,0");
 auto restored=libslicer::Config::defaults();REQUIRE(restored.apply_patch({{"flush_multiplier",*saved.value("flush_multiplier")},{"flush_volumes_matrix",*saved.value("flush_volumes_matrix")}}).success);CHECK(restored.snapshot().value("flush_multiplier")==saved.value("flush_multiplier"));CHECK(restored.snapshot().value("flush_volumes_matrix")==saved.value("flush_volumes_matrix"));
 for(const std::string key:{"flush_multiplier","flush_volumes_matrix"}){auto invalid=configuration;REQUIRE(invalid.set(key,"0.3").success);libslicer::SliceRequest request;request.config=invalid.snapshot();request.objects.push_back({(assets()/"rectangle.obj").string(),{}});const auto result=sdk->slice(request);CHECK_FALSE(result.success);CHECK(std::any_of(result.diagnostics.begin(),result.diagnostics.end(),[&](const auto& diagnostic){return diagnostic.option_key==key;}));}
}


TEST_CASE("A12 original fixed dual tools preserve independent material edits and process state", "[libslicer_api][fiber][A12][config]") {
    auto sdk = library(); REQUIRE(sdk);
    const auto models = sdk->machine_models();
    const auto cfsys = std::find_if(models.begin(), models.end(), [](const auto& model) {
        return model.id == "CFSYS Alpha500 Printer";
    });
    REQUIRE(cfsys != models.end());
    CHECK(std::count_if(models.begin(), models.end(), [](const auto& model) {
        return model.id.rfind("CFSYS", 0) == 0;
    }) == 1);
    REQUIRE(cfsys->variants.size() == 1);
    CHECK(std::filesystem::is_regular_file(cfsys->bed_model_path));
    CHECK(cfsys->bed_texture_path.empty());
    CHECK(cfsys->variants.front().id == "0.4");
    CHECK(cfsys->variants.front().physical_tool_count == 2);
    CHECK(cfsys->variants.front().filament_slots_bound_to_physical_tools);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    REQUIRE(sdk->activate_config(selection));
    REQUIRE(sdk->active_config());
    CHECK(sdk->active_config()->selection.process_preset_id == "CCF&CIRON @CFSYS");
    CHECK(sdk->active_config()->selection.filament_preset_ids ==
          std::vector<std::string>{"CFSYS CIRON", "CFSYS CCF"});
    REQUIRE(sdk->active_config()->filament_slots.size() == 2);
    CHECK(sdk->active_config()->filament_slots[0].physical_tool_index == 0);
    CHECK(sdk->active_config()->filament_slots[0].physical_tool_role == "substrate");
    CHECK(sdk->active_config()->filament_slots[0].physical_tool_side == "right");
    CHECK(sdk->active_config()->filament_slots[1].physical_tool_index == 1);
    CHECK(sdk->active_config()->filament_slots[1].physical_tool_role == "continuous_fiber");
    CHECK(sdk->active_config()->filament_slots[1].physical_tool_side == "left");
    CHECK(sdk->active_config()->filament_slots[0].color.red == 0x80);
    CHECK(sdk->active_config()->filament_slots[0].color.green == 0x80);
    CHECK(sdk->active_config()->filament_slots[0].color.blue == 0x80);
    CHECK(sdk->active_config()->filament_slots[1].color.red == 0xff);
    CHECK(sdk->active_config()->filament_slots[1].color.green == 0x00);
    CHECK(sdk->active_config()->filament_slots[1].color.blue == 0x80);
    auto snapshot = *sdk->active_config_snapshot();
    CHECK(snapshot.value("filament_map") == "1,2");
    CHECK(snapshot.value("filament_map_mode") == "Default");
    CHECK(snapshot.value("initial_layer_print_height") == "0.15");
    CHECK(snapshot.value("layer_height") == "0.13");
    CHECK(snapshot.value("sparse_infill_density") == "40%");
    CHECK(snapshot.value("generate_reinforced_infills") == "0");
    CHECK(snapshot.value("travel_acceleration") == "5000");
    CHECK(snapshot.value("fiber_travel_speed") == "60");
    CHECK(snapshot.value("fiber_z_down_speed") == "3");
    auto setting = [](const std::vector<libslicer::SettingItem>& settings, const std::string& key) {
        const auto found = std::find_if(settings.begin(), settings.end(), [&](const auto& item) { return item.key == key; });
        return found == settings.end() ? nullptr : &*found;
    };
    const auto initial_view = *sdk->active_config();
    const auto* initial_pattern = setting(initial_view.settings, "reinforced_infill_pattern");
    REQUIRE(initial_pattern != nullptr);
    CHECK_FALSE(initial_pattern->enabled);
    const auto enabled_infill = sdk->set_active_config_value("generate_reinforced_infills", "1");
    REQUIRE(enabled_infill);
    const auto* enabled_pattern = setting(enabled_infill.changed_items, "reinforced_infill_pattern");
    const auto* enabled_density = setting(enabled_infill.changed_items, "reinforced_infill_density");
    REQUIRE(enabled_pattern != nullptr);
    REQUIRE(enabled_density != nullptr);
    CHECK(enabled_pattern->enabled);
    CHECK(enabled_density->enabled);
    const auto disabled_infill = sdk->set_active_config_value("generate_reinforced_infills", "0");
    REQUIRE(disabled_infill);
    const auto* disabled_pattern = setting(disabled_infill.changed_items, "reinforced_infill_pattern");
    const auto* disabled_density = setting(disabled_infill.changed_items, "reinforced_infill_density");
    REQUIRE(disabled_pattern != nullptr);
    REQUIRE(disabled_density != nullptr);
    CHECK_FALSE(disabled_pattern->enabled);
    CHECK_FALSE(disabled_density->enabled);
    REQUIRE(sdk->set_active_filament_preset(0, "CFSYS ABS"));
    CHECK(sdk->active_config_snapshot()->value("filament_map") == "1,2");
    const auto valid_base_material = *sdk->active_config_snapshot();
    CHECK_FALSE(sdk->set_active_filament_preset(1, "CFSYS CIRON"));
    CHECK(sdk->active_config_snapshot()->values() == valid_base_material.values());
    CHECK_FALSE(sdk->apply_active_config_patch({{"filament_map_mode", "Manual"}, {"filament_map", "2,1"}}));
    REQUIRE(sdk->apply_active_config_patch({
        {"fiber_layer_height_ratio", "3"}, {"nozzle_temperature", "271,279"}}));
    REQUIRE(sdk->set_active_filament_color(0, {18, 52, 86, 255}));
    snapshot = *sdk->active_config_snapshot();
    CHECK(snapshot.value("filament_map_mode") == "Default");
    CHECK(snapshot.value("filament_map") == "1,2");
    CHECK(snapshot.value("fiber_layer_height_ratio") == "3");
    REQUIRE(snapshot.value("nozzle_temperature"));
    CHECK(snapshot.value("nozzle_temperature")->rfind("271,", 0) == 0);
    CHECK(snapshot.value("nozzle_temperature") != "271,279");
    CHECK(snapshot.value("filament_is_ccf") == "0,1");
    CHECK(sdk->active_config()->filament_slots[0].color.red == 18);
    CHECK(sdk->active_config()->filament_slots[0].physical_tool_index == 0);
    CHECK(sdk->active_config()->filament_slots[1].color.red == 0xff);
    CHECK(sdk->active_config()->filament_slots[1].color.green == 0x00);
    CHECK(sdk->active_config()->filament_slots[1].color.blue == 0x80);
    CHECK(sdk->active_config()->filament_slots[1].physical_tool_index == 1);
    const auto revision = sdk->active_config()->revision;
    CHECK_FALSE(sdk->set_active_filament_preset(1, "missing material"));
    CHECK(sdk->active_config()->revision == revision);
    CHECK(sdk->active_config_snapshot()->values() == snapshot.values());
    CHECK_FALSE(sdk->set_active_filament_preset(0, "CFSYS CCF"));
    CHECK(sdk->active_config_snapshot()->value("filament_map") == "1,2");
    CHECK(sdk->active_config_snapshot()->value("fiber_layer_height_ratio") == "3");
}

TEST_CASE("A11 original Alpha500 settings slice with tower and flush flags intact", "[libslicer_api][fiber][A11][original]") {
    auto sdk = library(); REQUIRE(sdk);
    libslicer::ConfigSelection selection;
    selection.machine_model_id = "CFSYS Alpha500 Printer";
    selection.machine_variant_id = "0.4";
    selection.process_preset_id = "CCF&CIRON @CFSYS";
    selection.filament_preset_ids = {"CFSYS CIRON", "CFSYS CCF"};
    REQUIRE(sdk->activate_config(selection));
    {
        libslicer::SliceRequest request;
        request.config = *sdk->active_config_snapshot();
        CHECK(request.config.value("enable_prime_tower") == "1");
        CHECK(request.config.value("flush_into_support") == "1");
        request.objects.push_back({(assets()/"rectangle.obj").string(), {}});
        auto second = request.objects.front(); second.transform[3] = 90;
        request.objects.push_back(second);
        const auto result = sdk->slice(request);
        for (const auto& diagnostic : result.diagnostics) UNSCOPED_INFO(diagnostic.message);
        REQUIRE(result.success); REQUIRE(result.preview); assert_fiber(*result.preview);
        size_t tower_segments = 0;
        for (const auto& segment : result.preview->segments) {
            if (segment.deposition_process == libslicer::ToolpathDepositionProcess::Fiber)
                CHECK(segment.tool_id == 1);
            if (segment.extrusion_role == libslicer::ToolpathExtrusionRole::WipeTower) ++tower_segments;
        }
        CHECK(tower_segments > 0);
        // Inspect emitted feedrates, including XYZ's same-Z fallback to XY.
        std::ifstream gcode(result.output.path);
        REQUIRE(gcode.good());
        std::string line;
        double feedrate = 0, entry_feedrate = 0;
        bool first_fiber = false, checked_descent = false, block_entry_descent = false;
        size_t fiber_begin = 0, fiber_start = 0, fiber_cut = 0, fiber_tail = 0,
               fiber_finish = 0, fiber_end = 0;
        int fiber_stage = 0;
        while (std::getline(gcode, line)) {
            if (line.rfind(";FIBER_BEGIN", 0) == 0) {
                CHECK(fiber_stage == 0); fiber_stage = 1; ++fiber_begin;
                block_entry_descent = false;
                if (!first_fiber) CHECK(entry_feedrate == Catch::Approx(3600));
                first_fiber = true;
            }
            else if (line.rfind(";FIBER_START", 0) == 0) {
                CHECK(fiber_stage == 1);
                CHECK(block_entry_descent);
                fiber_stage = 2; ++fiber_start;
            }
            else if (line.rfind(";FIBER_CUT", 0) == 0) { CHECK(fiber_stage == 2); fiber_stage = 3; ++fiber_cut; }
            else if (line.rfind(";FIBER_TAIL", 0) == 0) { CHECK(fiber_stage == 3); fiber_stage = 4; ++fiber_tail; }
            else if (line.rfind(";FIBER_FINISH", 0) == 0) { CHECK(fiber_stage == 4); fiber_stage = 5; ++fiber_finish; }
            else if (line.rfind(";FIBER_END", 0) == 0) { CHECK(fiber_stage == 5); fiber_stage = 0; ++fiber_end; }
            if (line.rfind("G1 ", 0) != 0) continue;
            const auto feed = line.find('F');
            if (feed != std::string::npos) feedrate = std::stod(line.substr(feed + 1));
            if (line.find('X') != std::string::npos || line.find('Y') != std::string::npos) {
                if (!first_fiber) entry_feedrate = feedrate;
                else if (!checked_descent && line.find('Z') != std::string::npos && line.find('E') == std::string::npos) {
                    CHECK(feedrate == Catch::Approx(180)); checked_descent = true;
                }
                if (fiber_stage == 1 && line.find('Z') != std::string::npos && line.find('E') == std::string::npos)
                    block_entry_descent = true;
            }
        }
        CHECK(first_fiber); CHECK(checked_descent);
        CHECK(fiber_stage == 0);
        CHECK(fiber_begin > 0);
        CHECK(fiber_start == fiber_begin); CHECK(fiber_cut == fiber_begin);
        CHECK(fiber_tail == fiber_begin); CHECK(fiber_finish == fiber_begin);
        CHECK(fiber_end == fiber_begin);
    }
}
