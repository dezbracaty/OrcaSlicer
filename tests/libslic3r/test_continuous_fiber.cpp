#include <catch2/catch_all.hpp>
#include "libslic3r/FiberPlanning.hpp"
#include "libslic3r/FiberProcess.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Print.hpp"
#include <fstream>
#include "libslic3r/Layer.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <nlohmann/json.hpp>
using namespace Slic3r;
namespace {
Polyline line(std::initializer_list<Vec2d> points) { Polyline out;for(const auto& p:points)out.points.emplace_back(scale_(p.x()),scale_(p.y()));return out; }
FiberConfig config() {FiberConfig c;c.region.fibercut_length.value=3;c.region.fiber_start_length.value=5;c.region.fiber_z_hop.value=.5;c.region.fiber_tension_length.value=.5;c.width_mm=.8;c.perimeter_width_mm=.8;c.resin_width_mm=.4;c.nozzle_mm=.4;c.height_mm=.2;return c;}
std::shared_ptr<const PreparedFiberPath> prepare(const Polyline& input,FiberConfig c,FiberPurpose purpose=FiberPurpose::Infill,bool source=false,bool arachne=false){std::string rejected;return prepare_fiber_path(input,c,FiberSource{},purpose,source,arachne,rejected);}
void process_invariants(const PreparedFiberPath& path){const auto plan=compile_fiber_process(path);size_t starts=0,cuts=0;Point actual=path.entry;bool tail=false;
 for(const auto& action:plan.actions){if(action.event_only){if(action.event==FiberEventKind::Start)++starts;if(action.event==FiberEventKind::Cut){++cuts;CHECK(actual==path.segments[path.cut_index-1].end);tail=true;}}else {CHECK(std::isfinite(action.e_mm));CHECK(action.speed_mm_s>0);if(tail)CHECK(action.e_mm==0);actual=action.point;}}
 CHECK(starts==1);CHECK(cuts==1);CHECK(actual==plan.exit);CHECK(plan.exit==path.exit);
}
}
TEST_CASE("A4 D1 concentric starts and connections are deterministic", "[fiber][A4][D1]") {
 FiberRegionRecipe recipe;recipe.config=config();recipe.config.region.outer_reinforced_perimeters_counts.value=3;
 recipe.candidate=Surface(stFiberPerimeter,ExPolygon(Polygon(line({{0,0},{80,0},{80,40},{0,40}}).points)));
 recipe.purpose=FiberPurpose::Perimeter;recipe.source_is_fiber_perimeter=true;
 const auto first=plan_fiber_paths(recipe,0,false,0,.1,[]{});const auto second=plan_fiber_paths(recipe,0,false,0,.1,[]{});
 REQUIRE_FALSE(first.candidates.empty());REQUIRE(first.candidates.size()==second.candidates.size());
 for(size_t i=0;i<first.candidates.size();++i)CHECK(first.candidates[i].points==second.candidates[i].points);
 CHECK(first.retained.size()==second.retained.size());
 CHECK(first.resin_requests.empty());
 if(first.retained.size()>1)CHECK(first.retained.front()->source.candidate>first.retained.back()->source.candidate);
}
TEST_CASE("A5 legacy filters trim start and end sequentially", "[fiber][A5]") {
 auto input=line({{0,0},{1,0},{1,1},{40,1},{40,2},{41,2}});const double original_length=input.length();Polylines keep;
 const auto first=filter_fiber_legacy(input,5,0,keep);keep.clear();const auto last=filter_fiber_legacy(input,5,2,keep);
 REQUIRE_FALSE(first.empty());REQUIRE_FALSE(last.empty());CHECK(input.length()<original_length);
 keep.clear();CHECK(filter_fiber_legacy(input,.5,1,keep).empty());REQUIRE(keep.size()==1);CHECK(keep.front().points==input.points);
 keep.clear();const auto rejected=filter_fiber_legacy(input,5,1,keep);CHECK_FALSE(keep.empty());CHECK_FALSE(rejected.empty());
}
TEST_CASE("A6 analytical arcs reverse and split without changing length", "[fiber][A6][D3]") {
 FiberCurveSegment segment;segment.start=Point(scale_(1),0.);segment.end=Point(0.,scale_(1));segment.center_mm=Vec2d::Zero();segment.arc=true;segment.sweep_rad=M_PI/2;segment.length_mm=M_PI/2;
 const Point middle=segment.at(segment.length_mm/2);auto before=segment.portion(0,segment.length_mm/2);auto after=segment.portion(segment.length_mm/2,segment.length_mm);CHECK(before.end==after.start);CHECK(before.length_mm+after.length_mm==Catch::Approx(segment.length_mm));
 segment.reverse();CHECK(segment.sweep_rad<0);CHECK((segment.at(segment.length_mm/2)-middle).cast<double>().norm()<=2);
 auto c=config();c.region.fiber_corner_overshoot.value=0;auto path=prepare(line({{0,0},{40,0},{40,40},{0,40}}),c,FiberPurpose::Perimeter,true,true);REQUIRE(path);CHECK(std::any_of(path->segments.begin(),path->segments.end(),[](const auto& s){return s.arc;}));process_invariants(*path);
}
TEST_CASE("A7 defaults preserve overshoot tension and hop process", "[fiber][A7][D3]") {
 auto c=config();CHECK(c.region.fiber_corner_overshoot.value==1.5);CHECK(c.region.fiber_tension_length.value==.5);
 for(double hop:{0.,.5}){c.region.fiber_z_hop.value=hop;auto path=prepare(line({{0,0},{60,0},{60,40},{90,40}}),c);REQUIRE(path);process_invariants(*path);const auto actions=compile_fiber_process(*path).actions;CHECK(actions[1].e_mm==Catch::Approx(c.region.fibercut_length.value+c.region.fiber_restart_extra_length.value));}
}
TEST_CASE("A8 cut zero vertex and perimeter hop budget boundaries", "[fiber][A8][D2]") {
 auto c=config();c.region.fiber_z_hop.value=1;c.region.fibercut_length.value=3;c.region.fiber_corner_overshoot.value=0;
 for(double length:{3.999,4.,4.001}){auto path=prepare(line({{0,0},{length,0}}),c,FiberPurpose::Perimeter);CHECK(bool(path)==(length>4.));if(path)process_invariants(*path);}
 c.region.fibercut_length.value=0;auto zero=prepare(line({{0,0},{40,0},{40,40}}),c);REQUIRE(zero);CHECK(zero->cut_index==zero->segments.size());process_invariants(*zero);
 c.region.fibercut_length.value=40;auto vertex=prepare(line({{0,0},{60,0},{60,40}}),c);REQUIRE(vertex);CHECK(vertex->segments[vertex->cut_index-1].end==Point(scale_(60),0.));process_invariants(*vertex);
}
TEST_CASE("A9 open infill stops at its planned endpoint", "[fiber][A9][D4]") {
 auto c=config();c.region.fiber_corner_overshoot.value=0;c.region.fiber_slow_length.value=2;
 auto internal=prepare(line({{0,0},{60,0}}),c);REQUIRE(internal);CHECK(internal->exit==internal->segments.back().end);CHECK(internal->exit==Point(scale_(60),0.));
 const auto internal_process=compile_fiber_process(*internal);REQUIRE_FALSE(internal_process.actions.empty());
 CHECK(std::none_of(internal_process.actions.begin(),internal_process.actions.end(),[](const FiberAction& action){return action.motion && (action.point.x()<0 || action.point.x()>scale_(60) || action.point.y()!=0); }));
 process_invariants(*internal);
 c.region.fiber_finish_ironing_distance.value=3;auto perimeter=prepare(line({{0,0},{60,0},{60,40}}),c,FiberPurpose::Perimeter);REQUIRE(perimeter);CHECK(perimeter->exit!=perimeter->segments.back().end);process_invariants(*perimeter);
}
TEST_CASE("D6 finalized entities clone metadata and prohibit reverse", "[fiber][D6]") {
 auto path=prepare(line({{0,0},{80,0}}),config());REQUIRE(path);ExtrusionFiberPath entity(path);std::unique_ptr<ExtrusionEntity> copy(entity.clone());auto* fiber=dynamic_cast<ExtrusionFiberPath*>(copy.get());REQUIRE(fiber);CHECK(fiber->prepared==path);CHECK_FALSE(fiber->can_reverse());CHECK_THROWS(fiber->reverse());
}
TEST_CASE("A7 A8 nonzero overshoot cut at a nonfinal corner", "[fiber][A7][A8][D2][D3]") {
 auto c=config();c.region.fibercut_length.value=40;c.region.fiber_corner_overshoot.value=1.5;
 auto path=prepare(line({{0,0},{60,0},{60,40}}),c);REQUIRE(path);REQUIRE(path->cut_index<path->segments.size());
 CHECK(path->segments[path->cut_index-1].end==Point(scale_(60),0.));process_invariants(*path);
}
TEST_CASE("A6 degenerate and terminal corners preserve finite local geometry", "[fiber][A6][D3]") {
 auto c=config();CHECK_FALSE(prepare(Polyline{},c,FiberPurpose::Perimeter,true,true));
 for(const auto& input:{line({{0,0},{40,0},{40,40}}),line({{0,0},{40,0},{40,0},{40,40},{80,40}}),line({{0,0},{40,0},{80,0}})}){
  auto prepared=prepare(input,c,FiberPurpose::Perimeter,true,true);REQUIRE(prepared);CHECK(std::isfinite(prepared->length_mm));for(const auto& segment:prepared->segments){CHECK(std::isfinite(segment.length_mm));CHECK(segment.length_mm>0);CHECK(std::isfinite(segment.sweep_rad));}
 }
}
#include "libslic3r/GCode/FanMover.hpp"
#include "libslic3r/GCode/PressureEqualizer.hpp"
#include "libslic3r/GCode.hpp"
TEST_CASE("D6 final fiber command boundaries survive fan and pressure consumers", "[fiber][D6]") {
 GCodeWriter writer;const std::string block=";FIBER_BEGIN v=1 path=0:0:0:0:0:0 instance=0 object=0 material=1 width=0.8\n;FIBER_START\nG1 X10 Y0 E1 F240\nM106 S255\nG1 X40 Y0 E4 F600\n;FIBER_CUT s=35\nS0\n;FIBER_TAIL\nG1 X45 Y0 F240\n;FIBER_FINISH\n;FIBER_END\n";
 const std::string input="G90\nM83\nG1 X0 Y0 F600\n"+block+"G1 X60 Y0 E1 F1800\n";
 FanMover fan(writer,1,false,true,false,.5);const auto output=fan.process_gcode(input,true);CHECK(output.find(block)!=std::string::npos);
 GCodeConfig configuration;configuration.use_relative_e_distances.value=true;PressureEqualizer pressure(configuration);pressure.process_layer(LayerResult{input,0,false,true,false});const auto final=pressure.process_layer(LayerResult::make_nop_layer_result());CHECK(final.gcode.find(block)!=std::string::npos);
}
TEST_CASE("A5 strict Lmin and A6 one insertion per original straight segment", "[fiber][A5][A6]") {
 auto c=config();const double budget=c.length_budget_mm();for(double length:{budget-.000001,budget,budget+.000001}){auto prepared=prepare(line({{0,0},{length,0}}),c);CHECK(bool(prepared)==(length>budget));}
 auto long_line=prepare(line({{0,0},{200,0}}),c);REQUIRE(long_line);REQUIRE(long_line->segments.size()==4);CHECK(long_line->segments[0].length_mm==Catch::Approx(.5));CHECK(long_line->segments[1].length_mm==Catch::Approx(5));CHECK(long_line->segments[2].length_mm==Catch::Approx(191.5));CHECK(long_line->segments[3].length_mm==Catch::Approx(3));
 auto closed=line({{0,0},{1,0},{1,1},{0,0}});Polylines retained;CHECK(filter_fiber_legacy(closed,20,1,retained).empty());REQUIRE(retained.size()==1);CHECK(retained.front().points==closed.points);
}

TEST_CASE("A11 planned fiber visits retain two object and two instance identities", "[fiber][A11][visits]") {
 Model model;
 for(size_t object=0;object<2;++object){auto* value=model.add_object();value->add_volume(make_cube(10,10,1),false);value->add_instance()->set_offset({double(object*40),0,0});value->add_instance()->set_offset({double(object*40),20,0});}
 auto settings=DynamicPrintConfig::full_print_config();settings.set_num_extruders(2);settings.set_num_filaments(2);
 std::ifstream fixture(std::filesystem::path(TEST_DATA_DIR)/"continuous_fiber"/"manual_two_tool.json");REQUIRE(fixture.good());nlohmann::json values;fixture>>values;
 for(auto item=values.begin();item!=values.end();++item)settings.set_deserialize_strict(item.key(),item.value().get<std::string>());
 Print print;print.apply(model,settings);REQUIRE(print.objects().size()==2);
 std::set<std::pair<size_t,size_t>> occurrences;
 for(size_t object=0;object<print.objects().size();++object){auto* value=print.get_object(object);REQUIRE(value->instances().size()==2);auto* layer=value->add_layer(0,.2,.2,.1);auto* region=layer->add_region(&value->printing_region(0));
  auto path=prepare(line({{0,0},{80,0}}),config());REQUIRE(path);region->fiber_paths.entities.push_back(new ExtrusionFiberPath(path));
  for(size_t instance=0;instance<value->instances().size();++instance){const auto visits=fiber_visits(*layer,instance);REQUIRE(visits.size()==2);CHECK(visits.front().phase==FiberPhase::ResinBase);CHECK(visits.front().object==object);CHECK(visits.front().instance==instance);const auto& visit=visits.back();CHECK(visit.object==object);CHECK(visit.instance==instance);CHECK(visit.phase==FiberPhase::Fiber);CHECK(visit.root_ids==std::vector<size_t>{0});CHECK(occurrences.emplace(visit.object,visit.instance).second);}
 }
 CHECK(occurrences.size()==4);
}

TEST_CASE("Alpha500 alone resolves the fixed two-tool Auto mapping", "[fiber][config][mapping]")
{
    auto settings = DynamicPrintConfig::full_print_config();
    settings.set_num_extruders(2);
    settings.set_num_filaments(2);
    settings.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmAutoForFlush));
    settings.set_key_value("filament_map", new ConfigOptionInts({2, 1}));

    settings.set_key_value("printer_model", new ConfigOptionString("Unrelated dual-tool printer"));
    CHECK_FALSE(resolve_fixed_filament_map(settings, 2));
    CHECK(settings.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{2, 1});

    settings.set_key_value("printer_model", new ConfigOptionString("CFSYS Alpha500 Printer"));
    CHECK(resolve_fixed_filament_map(settings, 2));
    CHECK(settings.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{1, 2});

    settings.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    settings.set_key_value("filament_map", new ConfigOptionInts({2, 1}));
    CHECK_FALSE(resolve_fixed_filament_map(settings, 2));
    CHECK(settings.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{2, 1});
}

TEST_CASE("D5 coverage permits unprintable boundary remnants and rejects missing resin", "[fiber][D5][coverage]") {
 Model model;auto* object=model.add_object();object->add_volume(make_cube(10,10,1),false);object->add_instance();
 auto settings=DynamicPrintConfig::full_print_config();settings.set_deserialize_strict("generate_reinforced_infills","1");
 Print print;print.apply(model,settings);auto* value=print.get_object(0);
 auto* layer=value->add_layer(0,.2,.2,.1);auto* region=layer->add_region(&value->printing_region(0));
 ResinRequest request;request.phase=FiberPhase::ResinRepair;request.fill_recipe=Flow(.45,.2,.4);request.reason="coverage regression";
 request.area={ExPolygon(Polygon(line({{0,0},{.494,0},{.494,.205},{0,.205}}).points))};
 region->fiber_resin_requests.push_back(request);
 CHECK_NOTHROW(check_fiber_resin_coverage(*layer));
 region->fiber_resin_requests.front().area={ExPolygon(Polygon(line({{0,0},{5,0},{5,5},{0,5}}).points))};
 CHECK_THROWS_WITH(check_fiber_resin_coverage(*layer),Catch::Matchers::ContainsSubstring("no deposited coverage"));
 extrusion_entities_append_paths(region->fiber_repairs.entities,Polylines{line({{0,.225},{5,.225}})},erSolidInfill,request.fill_recipe.mm3_per_mm(),.45,.2);
 CHECK_THROWS_WITH(check_fiber_resin_coverage(*layer),Catch::Matchers::ContainsSubstring("Macroscopic uncovered"));
 // Ordinary group_fills may publish a second region's Base under the first.
 region->fiber_resin_requests.clear();
 auto* second=layer->add_region(&value->printing_region(0));
 request.phase=FiberPhase::ResinBase;request.region=1;
 request.area={ExPolygon(Polygon(line({{0,0},{5,0},{5,.45},{0,.45}}).points))};
 second->fiber_resin_requests.push_back(request);
 CHECK_THROWS(check_fiber_resin_coverage(*layer));
 extrusion_entities_append_paths(region->fills.entities,Polylines{line({{0,.225},{5,.225}})},erSolidInfill,request.fill_recipe.mm3_per_mm(),.45,.2);
 CHECK_NOTHROW(check_fiber_resin_coverage(*layer));
 second->fiber_resin_requests.front().requires_explicit_fill=true;second->fiber_resin_requests.front().request_id="coverage:base";
 CHECK_THROWS_WITH(check_fiber_resin_coverage(*layer),Catch::Matchers::ContainsSubstring("exactly one consumer"));
 second->fiber_resin_requests.front().explicit_fill_count=1;CHECK_NOTHROW(check_fiber_resin_coverage(*layer));
 auto duplicate=second->fiber_resin_requests.front();duplicate.area.clear();region->fiber_resin_requests.push_back(std::move(duplicate));
 CHECK_THROWS_WITH(check_fiber_resin_coverage(*layer),Catch::Matchers::ContainsSubstring("Duplicate or unidentified"));
}

TEST_CASE("CFSYS CCF accepts its real diameter with reinforcement on or off", "[fiber][config][CFSYS]") {
 FullPrintConfig settings;settings.filament_diameter.values={1.75,.35};settings.filament_type.values={"PLA","CCF"};
 for(bool active:{false,true}) {
  settings.generate_reinforced_infills.value=active;
  CHECK(validate(settings).count("filament_diameter")==0);
 }
 settings.filament_type.values[1]="PLA";
 CHECK(validate(settings).count("filament_diameter")==1);
 settings.filament_type.values[1]="CCF";
 for(double invalid:{0.,-.35,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
  settings.filament_diameter.values[1]=invalid;
  CHECK(validate(settings).count("filament_diameter")==1);
 }
}
