#include <catch2/catch_all.hpp>

#include "libslic3r/ContinuousFiber/FiberIsland.hpp"
#include "libslic3r/ContinuousFiber/FiberCoverageLedger.hpp"
#include "libslic3r/ContinuousFiber/FiberPathFinalizer.hpp"
#include "libslic3r/ContinuousFiber/FiberPathValidator.hpp"
#include "libslic3r/ContinuousFiber/FiberPolicyKey.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCode/FiberGCodeBlockParser.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/GCode/FanMover.hpp"

#include <set>
#include <map>
#include <limits>
#include <random>
#include <nlopt.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <sstream>
#include <nlohmann/json.hpp>

using namespace Slic3r;

namespace {

std::shared_ptr<FiberExecutionContext> fiber_fixture_context()
{
    auto context=std::make_shared<FiberExecutionContext>();
    context->check_machine=false;context->placements={{0,0,Vec2d::Zero()}};
    auto& c=context->config;
    c.filament_process_type.values.assign(8,"continuous_fiber");
    c.filament_map.values={1,2,3,4,5,6,7,8};c.physical_extruder_map.values={0,1,2,3,4,5,6,7};
    c.toolhead_process_capabilities.values.assign(8,"continuous_fiber");
    c.toolhead_fiber_protocol_id.values.assign(8,"linear-e-v1");c.toolhead_fiber_e_units_per_mm.values.assign(8,1.);
    c.extruder_offset.values.assign(8,Vec2d::Zero());
    return context;
}
FrozenFiberExecution compile_fixture(std::shared_ptr<const PreparedFiberPath> prepared,unsigned filament,
    unsigned extruder,unsigned physical,double units,const std::string& cut,const GCodeWriter* writer=nullptr,
    std::function<Vec2d(const Point&)> unused={},double z=0,bool native=false)
{
    if(!prepared || prepared->logical_filament_id!=filament || cut.empty() || unused)
        throw std::invalid_argument("Invalid explicit fiber compiler fixture");
    auto context=fiber_fixture_context();
    context->config.filament_map.values.at(filament)=int(extruder+1);
    std::swap(context->config.physical_extruder_map.values.at(extruder),context->config.physical_extruder_map.values.at(physical));
    context->config.toolhead_fiber_e_units_per_mm.values.at(physical)=units;
    context->export_arcs=native;
    if(writer)context->transform=writer->coordinate_transform();
    return compile_fiber_execution(prepared->draft(),*context,context->placements.front(),z);
}

ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    ExPolygon result;
    result.contour.points = {
        Point(scale_(x0), scale_(y0)),
        Point(scale_(x1), scale_(y0)),
        Point(scale_(x1), scale_(y1)),
        Point(scale_(x0), scale_(y1))
    };
    return result;
}

ExPolygon rectangle_with_hole(
    double x0, double y0, double x1, double y1,
    double hole_x0, double hole_y0, double hole_x1, double hole_y1)
{
    ExPolygon result = rectangle(x0, y0, x1, y1);
    Polygon hole = rectangle(hole_x0, hole_y0, hole_x1, hole_y1).contour;
    hole.reverse();
    result.holes.emplace_back(std::move(hole));
    return result;
}

ExtrusionPath straight_path(double x0, double y0, double x1, double y1, double width = 0.8)
{
    ExtrusionPath result(erContinuousFiberInfill, 0.1, float(width), 0.2f);
    result.polyline.points = {
        Point3::new_scale(x0, y0, 0.0),
        Point3::new_scale(x1, y1, 0.0)
    };
    return result;
}

ExtrusionPath path_from_points(std::initializer_list<std::pair<double, double>> points, double width = 0.8)
{
    ExtrusionPath result(erContinuousFiberInfill, 0.1, float(width), 0.2f);
    for (const auto& [x, y] : points)
        result.polyline.points.push_back(Point3::new_scale(x, y, 0.0));
    return result;
}

std::string rejection_details(const FiberValidationResult& result)
{
    std::string text;
    for(const auto& item:result.assignments)if(item.kind!=FiberAssignmentKind::AcceptedFiber)
        text+=std::string(fiber_rejection_reason_name(item.reason))+": "+item.detail+"; ";
    return text;
}

FiberFragmentId test_id()
{
    return {{{7, 62, 4, 1}, FiberPathPurpose::Infill, 0, 9}, 0};
}

// Existing independent-source tests keep their original setup. The production
// executor itself accepts only explicit routes and has no policy-inference overload.
FiberFinalizationResult finalize_independent(
    const ExtrusionPath& candidate, const ExPolygons& domain,
    const ContinuousFiberConfig& config, const FiberFragmentId& id,
    const std::vector<ContourArc>& arcs = {}, const ExPolygons* centerline_domain = nullptr)
{
    FiberFinishRoute finish;
    finish.source = id;
    finish.strategy = FiberFinishStrategy::TangentExtension;
    if (candidate.polyline.points.size() < 2 || domain.empty() ||
        std::any_of(candidate.polyline.points.begin(), candidate.polyline.points.end(),
            [](const Point3& p) { return p.z() != 0; }))
        return FiberPathFinalizer::finalize(candidate, domain, config, id, finish, arcs, centerline_domain);
    try {
        finish = make_independent_fiber_finish_route(candidate, config, id);
        return FiberPathFinalizer::finalize(candidate, domain, config, id, finish, arcs, centerline_domain);
    } catch (const std::invalid_argument& error) {
        FiberFinalizationResult result;
        result.failure = FiberFinalizationFailure::InvalidParameter;
        result.length_mm = unscale<double>(candidate.length());
        result.detail = error.what();
        return result;
    }
}

} // namespace

TEST_CASE("explicit fiber finish follows its supplied route on open depositing geometry",
    "[ContinuousFiber][FinishInterface]")
{
    auto candidate=straight_path(5,5,45,5);
    auto id=test_id();id.parent.purpose=FiberPathPurpose::Contour;
    const ExPolygons domain{rectangle(0,0,50,15)};
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;
    config.finish_overlap_length_mm=10;
    config.finish_extension_length_mm=std::numeric_limits<double>::quiet_NaN();
    FiberFinishRoute finish;
    finish.strategy=FiberFinishStrategy::LoopOverlap;
    finish.source=id;finish.source.parent.job_ordinal+=1;
    finish.requested_length_mm=10;
    // An open prefix of a source loop: no straight or closure requirement.
    finish.geometry.points={Point3::new_scale(45,5,0),Point3::new_scale(45,10,0),Point3::new_scale(40,10,0)};
    const auto result=FiberPathFinalizer::finalize(candidate,domain,config,id,finish);
    REQUIRE(result.prepared);
    CHECK(result.prepared->finish_strategy==FiberFinishStrategy::LoopOverlap);
    CHECK(result.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.002));
    const auto& span=result.prepared->spans.back();
    CHECK(span.kind==FiberMotionKind::NonDepositingFinish);
    CHECK(span.geometry.points==finish.geometry.points);
    for(const auto& edge:span.edges)CHECK(edge.feed_mm_per_xy_mm==0);
    CHECK(result.prepared->outside_domain.empty());
    const auto original=finalize_independent(candidate,domain,ContinuousFiberConfig{},id);
    REQUIRE(original.prepared);
    CHECK(diff_ex(result.prepared->physical_coverage,original.prepared->physical_coverage).empty());
    CHECK(diff_ex(original.prepared->physical_coverage,result.prepared->physical_coverage).empty());
}

TEST_CASE("explicit fiber finish rejects incorrect route metadata and geometry",
    "[ContinuousFiber][FinishInterface]")
{
    const auto candidate=straight_path(5,5,45,5);
    auto id=test_id();id.parent.purpose=FiberPathPurpose::Contour;
    const ExPolygons domain{rectangle(0,0,50,15)};
    ContinuousFiberConfig config;config.finish_extension_length_mm=5;
    const auto valid=make_independent_fiber_finish_route(candidate,config,id);
    REQUIRE(FiberPathFinalizer::finalize(candidate,domain,config,id,valid).prepared);
    SECTION("wrong start") {auto route=valid;route.geometry.points.front().x()+=scale_(1);CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("nonzero layer Z") {auto route=valid;route.geometry.points.back().z()=scale_(1);CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("wrong actual length") {auto route=valid;route.geometry.points.back().x()+=scale_(1);CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("wrong request") {auto route=valid;route.requested_length_mm=4;CHECK(FiberPathFinalizer::finalize(candidate,domain,config,id,route).failure==FiberFinalizationFailure::InvalidParameter);}
    SECTION("nonfinite request") {auto route=valid;route.requested_length_mm=std::numeric_limits<double>::quiet_NaN();CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("different source domain") {auto route=valid;route.source.parent.domain.component_id+=1;CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("different source purpose") {auto route=valid;route.source.parent.purpose=FiberPathPurpose::Infill;CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("invalid strategy") {auto route=valid;route.strategy=static_cast<FiberFinishStrategy>(255);CHECK(FiberPathFinalizer::finalize(candidate,domain,config,id,route).failure==FiberFinalizationFailure::InvalidParameter);}
    SECTION("None cannot bypass configured finish") {auto route=valid;route.strategy=FiberFinishStrategy::None;route.requested_length_mm=0;route.geometry.points.clear();CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
    SECTION("missing nonzero finish") {auto route=valid;route.geometry.points.clear();CHECK(FiberPathFinalizer::finalize(candidate,domain,config,id,route).failure==FiberFinalizationFailure::FinishUnavailable);}
    SECTION("zero edge") {auto route=valid;route.geometry.points={route.geometry.points.front(),route.geometry.points.front()};CHECK_FALSE(FiberPathFinalizer::finalize(candidate,domain,config,id,route).prepared);}
}

TEST_CASE("independent finish policy keeps zero requests and original failure ordering",
    "[ContinuousFiber][FinishInterface]")
{
    const auto candidate=straight_path(5,5,45,5);
    auto id=test_id();id.parent.purpose=FiberPathPurpose::Contour;
    const ExPolygons domain{rectangle(0,0,50,15)};
    ContinuousFiberConfig config;config.finish_overlap_length_mm=std::numeric_limits<double>::quiet_NaN();
    const auto zero=make_independent_fiber_finish_route(candidate,config,id);
    CHECK(zero.strategy==FiberFinishStrategy::TangentExtension);CHECK(zero.geometry.empty());
    const auto result=FiberPathFinalizer::finalize(candidate,domain,config,id,zero);
    REQUIRE(result.prepared);CHECK(result.prepared->finish_strategy==FiberFinishStrategy::None);
    config.finish_extension_length_mm=-1;
    CHECK(finalize_independent(candidate,domain,config,id).failure==FiberFinalizationFailure::InvalidParameter);
    config.finish_extension_length_mm=std::numeric_limits<double>::infinity();
    CHECK(finalize_independent(candidate,domain,config,id).failure==FiberFinalizationFailure::InvalidParameter);
    config.finish_extension_length_mm=1e100;
    CHECK(finalize_independent(candidate,domain,config,id).failure==FiberFinalizationFailure::InvalidParameter);
    auto loop=path_from_points({{5,5},{10,5},{10,10},{5,10},{5,5}});
    auto contour=id;contour.parent.purpose=FiberPathPurpose::Contour;
    config=ContinuousFiberConfig{};config.finish_overlap_length_mm=23;
    CHECK(finalize_independent(loop,domain,config,contour).failure==FiberFinalizationFailure::FinishUnavailable);
    config.cut_to_contact_length_mm=23;
    CHECK(finalize_independent(loop,domain,config,contour).failure==FiberFinalizationFailure::TooShort);
    config=ContinuousFiberConfig{};config.finish_extension_length_mm=5;
    const auto degenerate=path_from_points({{5,5},{5,5}});
    const auto unavailable=make_independent_fiber_finish_route(degenerate,config,id);
    CHECK(unavailable.geometry.empty());
    config.cut_to_contact_length_mm=23;
    CHECK(finalize_independent(degenerate,domain,config,id).failure==FiberFinalizationFailure::TooShort);
}

TEST_CASE("rectilinear finish extends motion without moving the cut or depositing endpoint", "[ContinuousFiber][finish]")
{
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;config.z_hop_height_mm = 2;
    const auto source=straight_path(0,5,100,5);
    const ExPolygons domain{rectangle(-5,0,110,25)};
    const auto baseline=finalize_independent(source,domain,config,test_id());
    REQUIRE(baseline.prepared);
    CHECK(baseline.prepared->finish_strategy==FiberFinishStrategy::TangentExtension);
    for (double extension : {0.,12.,23.,500.}) {
        config.finish_extension_length_mm=extension;
        // Closed-loop overlap is irrelevant to this open infill path.
        config.finish_overlap_length_mm=std::numeric_limits<double>::quiet_NaN();
        const auto result=finalize_independent(source,domain,config,test_id());
        REQUIRE(result.prepared);
        const auto& prepared=*result.prepared;
        CHECK(prepared.finish_strategy==FiberFinishStrategy::TangentExtension);
        REQUIRE(prepared.spans.size()==baseline.prepared->spans.size()+(extension>0?1:0));
        CHECK(prepared.passive_tail_length_mm()==Catch::Approx(23).margin(.002));
        const auto& tail=prepared.spans[baseline.prepared->spans.size()-1];
        CHECK(tail.kind==FiberMotionKind::PassiveDepositingAfterCut);
        CHECK(tail.geometry.points.front()==Point3::new_scale(77,5,0));
        CHECK(tail.geometry.points.back()==source.polyline.points.back());
        CHECK(prepared.spans.back().geometry.points.back()==Point3::new_scale(100+extension,5,0));
        CHECK(std::count_if(prepared.actions.begin(),prepared.actions.end(),[](const auto& a) {
            return a.type==FiberActionType::Cut;
        })==1);
        bool cut=false;
        for(const auto& action:prepared.actions) {
            if(action.type==FiberActionType::Cut)cut=true;
            if(cut) {
                CHECK(action.type!=FiberActionType::Prefeed);
                CHECK(action.type!=FiberActionType::Start);
            }
        }
        for(size_t i=0;i<baseline.prepared->spans.size();++i) {
            CHECK(prepared.spans[i].geometry.points==baseline.prepared->spans[i].geometry.points);
            REQUIRE(prepared.spans[i].edges.size()==baseline.prepared->spans[i].edges.size());
            for(size_t e=0;e<prepared.spans[i].edges.size();++e) {
                CHECK(prepared.spans[i].edges[e].feed_mm_per_xy_mm==baseline.prepared->spans[i].edges[e].feed_mm_per_xy_mm);
                CHECK(prepared.spans[i].edges[e].speed_mm_s==baseline.prepared->spans[i].edges[e].speed_mm_s);
            }
        }
        CHECK(prepared.total_depositing_length_mm()==Catch::Approx(100));
        CHECK(diff_ex(prepared.physical_coverage,baseline.prepared->physical_coverage).empty());
        CHECK(diff_ex(baseline.prepared->physical_coverage,prepared.physical_coverage).empty());
        CHECK(diff_ex(prepared.resin_exclusion,baseline.prepared->resin_exclusion).empty());
        CHECK(diff_ex(baseline.prepared->resin_exclusion,prepared.resin_exclusion).empty());
        const auto bound=compile_fixture(result.prepared,0,0,0,1.,";cut");
        for(size_t i=0;i<prepared.spans.size();++i)
            if(!prepared.spans[i].actively_feeds_fiber())
                for(const auto& command:bound.spans()[i].commands)CHECK(command.dE==0);
    }
    config.finish_extension_length_mm=23;
    const auto bent=path_from_points({{0,5},{80,5},{80,15}});
    const auto result=finalize_independent(bent,domain,config,test_id());
    REQUIRE(result.prepared);
    const auto& tail=result.prepared->spans[result.prepared->spans.size()-2];
    CHECK(tail.geometry.points.front()==Point3::new_scale(67,5,0));
    CHECK(tail.geometry.points.back()==Point3::new_scale(80,15,0));
    CHECK(result.prepared->spans.back().geometry.points.back()==Point3::new_scale(80,38,0));
    CHECK(result.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.002));
    CHECK(finalize_independent(straight_path(0,5,23,5),domain,config,test_id()).failure==FiberFinalizationFailure::TooShort);
    const auto reversed=finalize_independent(straight_path(100,5,0,5),domain,config,test_id());
    REQUIRE(reversed.prepared);
    CHECK(reversed.prepared->spans.back().geometry.points.back()==Point3::new_scale(-23,5,0));
    for(double invalid:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        config.finish_extension_length_mm=invalid;
        CHECK(finalize_independent(source,domain,config,test_id()).failure==FiberFinalizationFailure::InvalidParameter);
    }
}

TEST_CASE("rectilinear finalization accepts only its own forward tangent finish", "[ContinuousFiber][FinishInterface]")
{
    const auto source=straight_path(5,5,105,5);const auto id=test_id();
    const ExPolygons domain{rectangle(0,0,120,15)};
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;config.finish_extension_length_mm=23;
    auto route=make_independent_fiber_finish_route(source,config,id);
    REQUIRE(FiberPathFinalizer::finalize(source,domain,config,id,route).prepared);
    SECTION("None") {route.strategy=FiberFinishStrategy::None;}
    SECTION("loop strategy") {route.strategy=FiberFinishStrategy::LoopOverlap;}
    SECTION("wrong request") {route.requested_length_mm=12;}
    SECTION("wrong start") {route.geometry.points.front().x()+=scale_(1);}
    SECTION("wrong length") {route.geometry.points.back().x()+=scale_(1);}
    SECTION("backward") {route.geometry.points.back()=Point3::new_scale(82,5,0);}
    SECTION("sideways") {route.geometry.points.back()=Point3::new_scale(105,28,0);}
    SECTION("wrong fragment") {route.source.fragment_ordinal+=1;}
    SECTION("wrong source") {route.source.parent.job_ordinal+=1;}
    SECTION("missing geometry") {route.geometry.points.clear();}
    CHECK_FALSE(FiberPathFinalizer::finalize(source,domain,config,id,route).prepared);
}

TEST_CASE("rectilinear grouping distinguishes active finishing settings only", "[ContinuousFiber][finish][grouping]")
{
    ContinuousFiberConfig first;
    first.infill_enabled=true;first.infill_pattern=ipRectilinear;first.infill_density=100;
    first.infill_flow=Flow(1.f,.2f,.4f);first.cut_to_contact_length_mm=23;
    auto second=first;second.finish_overlap_length_mm=12;
    std::map<FiberPolicyKey,std::vector<FiberContributorDomainSource>> groups;
    groups[fiber_policy_key(first,0,false)].push_back({0,{rectangle(0,0,20,40)}});
    groups[fiber_policy_key(second,0,false)].push_back({1,{rectangle(20,0,40,40)}});
    REQUIRE(groups.size()==1);
    const auto merged=merge_connected_fiber_domains(groups.begin()->second);
    REQUIRE(merged.size()==1);CHECK(merged.front().contributor_indices==std::vector<size_t>{0,1});
    second.finish_extension_length_mm=23;
    CHECK_FALSE(fiber_policy_key(first,0,false)==fiber_policy_key(second,0,false));
    first.finish_extension_length_mm=23;
    CHECK(fiber_policy_key(first,0,false)==fiber_policy_key(second,0,false));
    second.finish_motion_speed_mm_s=7;
    CHECK_FALSE(fiber_policy_key(first,0,false)==fiber_policy_key(second,0,false));
    second=first;second.finish_overlap_length_mm=12;
    first.contour_enabled=second.contour_enabled=true;
    CHECK_FALSE(fiber_policy_key(first,0,false)==fiber_policy_key(second,0,false));
}

TEST_CASE("rectilinear tangent validation retains enabled ring connection checks", "[ContinuousFiber][OpenRingConnection][FinishInterface]")
{
    ContinuousFiberConfig config;
    config.connect_adjacent_rings=true;
    CHECK_NOTHROW(validate_fiber_process_config(config,FiberPathPurpose::Infill,FiberFinishStrategy::TangentExtension));
    SECTION("negative tail trim") {config.connection_tail_trim_widths=-1;}
    SECTION("nonfinite head trim") {config.connection_head_trim_widths=std::numeric_limits<double>::quiet_NaN();}
    CHECK_THROWS_AS(validate_fiber_process_config(config,FiberPathPurpose::Infill,FiberFinishStrategy::TangentExtension),std::invalid_argument);
    config.connect_adjacent_rings=false;
    CHECK_NOTHROW(validate_fiber_process_config(config,FiberPathPurpose::Infill,FiberFinishStrategy::TangentExtension));
}

TEST_CASE("continuous fiber grouping merges only identical policies", "[ContinuousFiber]")
{
    FiberPolicyKey region_0;
    region_0.contour_enabled = true;
    region_0.contour_count = 2;
    region_0.contour_material = 2;

    const FiberPolicyKey region_1_same_policy = region_0;
    FiberPolicyKey region_2_different_policy = region_0;
    region_2_different_policy.contour_count = 3;

    CHECK(region_0 == region_1_same_policy);
    CHECK_FALSE(region_0 == region_2_different_policy);

    const std::set<FiberPolicyKey> grouped_jobs {
        region_0,
        region_1_same_policy,
        region_2_different_policy
    };
    INFO("region identity is absent; only complete fiber policy controls grouping");
    CHECK(grouped_jobs.size() == 2);
}

TEST_CASE("continuous fiber grouping separates incompatible process parameters", "[ContinuousFiber]")
{
    ContinuousFiberConfig first;
    first.contour_enabled = true;
    first.contour_max_speed_mm_s = 10.0;
    first.contour_acceleration_mm_s2 = 300.0;

    ContinuousFiberConfig second = first;
    second.contour_max_speed_mm_s = 6.0;

    CHECK_FALSE(fiber_policy_key(first, 0.0, true) == fiber_policy_key(second, 0.0, true));

    second = first;
    second.contour_acceleration_mm_s2 = 500.0;
    CHECK_FALSE(fiber_policy_key(first, 0.0, true) == fiber_policy_key(second, 0.0, true));

    second = first;
    second.contour_boundary_clearance_mm = 0.2;
    CHECK_FALSE(fiber_policy_key(first, 0.0, true) == fiber_policy_key(second, 0.0, true));
}

TEST_CASE("fiber G-code block parser protects markers and payload", "[ContinuousFiber]")
{
    FiberGCodeBlockParser parser;
    const auto begin = parser.consume(";FIBER_BEGIN v=2");
    CHECK(begin.protected_line);
    CHECK(begin.begins_block);
    CHECK(parser.consume("G1 X10 Y10 E1").protected_line);
    const auto end = parser.consume(";FIBER_END");
    CHECK(end.protected_line);
    CHECK(end.ends_block);
    CHECK_FALSE(parser.inside_block());
    CHECK_FALSE(parser.consume("G1 X20 Y20").protected_line);

    CHECK_THROWS(parser.consume(";FIBER_END"));
}

TEST_CASE("continuous fiber islands are canonical independent sources", "[ContinuousFiber]")
{
    ExPolygons input {rectangle(40, 0, 60, 20), rectangle(0, 0, 20, 20)};
    const std::vector<FiberIslandSource> islands = make_fiber_islands(std::move(input), 7, 62, 4);

    REQUIRE(islands.size() == 2);
    CHECK((islands[0].id == FiberDomainId {7, 62, 4, 0}));
    CHECK((islands[1].id == FiberDomainId {7, 62, 4, 1}));
    CHECK(get_extents(islands[0].effective_domain).max.x() < get_extents(islands[1].effective_domain).min.x());
}

TEST_CASE("continuous fiber domains merge touching contributors and keep separated islands independent", "[ContinuousFiber]")
{
    const std::vector<FiberContributorDomainSource> contributors {
        {10, ExPolygons {rectangle(0, 0, 10, 10)}},
        {11, ExPolygons {rectangle(10, 0, 20, 10)}},
        {12, ExPolygons {rectangle(30, 0, 40, 10)}}
    };

    const std::vector<FiberDomainComponent> components = merge_connected_fiber_domains(contributors);

    REQUIRE(components.size() == 2);
    CHECK(components[0].contributor_indices == std::vector<size_t> {10, 11});
    CHECK(components[1].contributor_indices == std::vector<size_t> {12});
    CHECK(get_extents(components[0].merged_domain).max.x() < get_extents(components[1].merged_domain).min.x());
}

TEST_CASE("continuous fiber cut tail is finalized before coverage", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(10, 10, 60, 10);
    const ExPolygons domain {rectangle(0, 0, 70, 20)};
    ContinuousFiberConfig config;
    config.minimum_path_length_mm = 1.0;
    config.cut_to_contact_length_mm = 23.0;
    config.resin_overlap_mm = 0.05;
    config.outside_tolerance_mm2 = 0.01;

    const FiberFinalizationResult result = finalize_independent(candidate, domain, config, test_id());
    REQUIRE(result.prepared != nullptr);
    REQUIRE(result.prepared->spans.size() == 2);
    CHECK(result.prepared->spans[0].kind == FiberMotionKind::PoweredEntry);
    CHECK(result.prepared->spans[1].kind == FiberMotionKind::PassiveDepositingAfterCut);
    CHECK(result.prepared->passive_tail_length_mm() == Catch::Approx(23.0).margin(0.001));
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(50.0).margin(0.001));
    CHECK_FALSE(result.prepared->physical_coverage.empty());
    CHECK_FALSE(result.prepared->resin_exclusion.empty());
    CHECK(result.prepared->outside_domain.empty());

    const auto& actions = result.prepared->actions;
    REQUIRE(actions.size() == 11);
    CHECK(actions[0].type == FiberActionType::Begin);
    CHECK(actions[1].type == FiberActionType::Approach);
    CHECK(actions[2].type == FiberActionType::Prefeed);
    CHECK(actions[3].type == FiberActionType::Start);
    CHECK(actions[4].type == FiberActionType::MotionSpan);
    CHECK(actions[5].type == FiberActionType::AdhesionDwell);
    CHECK(actions[6].type == FiberActionType::Cut);
    CHECK(actions[7].type == FiberActionType::MotionSpan);
    CHECK(actions[8].type == FiberActionType::FiberDepleted);
    CHECK(actions[9].type == FiberActionType::Finish);
    CHECK(actions[10].type == FiberActionType::End);
    auto invalid = result.prepared->draft();
    std::swap(invalid.actions[4], invalid.actions[5]);
    CHECK_THROWS(invalid.validate());
}

TEST_CASE("continuous fiber start procedure separates landing from powered deposition", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(10, 10, 70, 10);
    const ExPolygons domain {rectangle(0, 0, 80, 20)};
    ContinuousFiberConfig config;
    config.minimum_path_length_mm = 1.0;
    config.cut_to_contact_length_mm = 10.0;
    config.prefeed_extra_length_mm = 0.5;
    config.prefeed_speed_mm_s = 10.0;
    config.z_hop_height_mm = 2.0;
    config.landing_speed_mm_s = 3.0;
    config.adhesion_dwell_ms = 25;
    config.start_stabilization_length_mm = 3.0;
    config.start_speed_mm_s = 10.0;
    config.resin_overlap_mm = 0.05;
    config.outside_tolerance_mm2 = 0.01;

    const FiberFinalizationResult result = finalize_independent(candidate, domain, config, test_id());
    REQUIRE(result.prepared != nullptr);
    const PreparedFiberPath& prepared = *result.prepared;
    REQUIRE(prepared.spans.size() == 3);
    CHECK(prepared.entry.mode==FiberEntryMode::PathLanding);
    CHECK(prepared.entry.end_station_mm==Catch::Approx(2.0));
    CHECK(prepared.spans[0].kind == FiberMotionKind::PoweredStart);
    CHECK(prepared.spans[1].kind == FiberMotionKind::PoweredDepositing);
    CHECK(prepared.spans[2].kind == FiberMotionKind::PassiveDepositingAfterCut);
    CHECK(unscale<double>(prepared.spans[0].geometry.length()) == Catch::Approx(3.0).margin(0.001));
    CHECK(prepared.passive_tail_length_mm() == Catch::Approx(10.0).margin(0.001));
    CHECK(prepared.total_depositing_length_mm() == Catch::Approx(60.0).margin(0.001));
    CHECK(prepared.start_procedure.prefeed_length_mm == Catch::Approx(10.5));

    const auto start_event = std::find_if(
        prepared.actions.begin(), prepared.actions.end(),
        [](const FiberProcessAction& action) { return action.type == FiberActionType::Start; });
    REQUIRE(start_event != prepared.actions.end());
    CHECK(start_event->span_index == 0);
    CHECK_FALSE(prepared.physical_coverage.empty());
    CHECK_FALSE(prepared.resin_exclusion.empty());
}

TEST_CASE("non depositing finish does not change fiber coverage", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(10, 10, 60, 10);
    const ExPolygons domain {rectangle(0, 0, 70, 20)};
    ContinuousFiberConfig without_finish;
    without_finish.cut_to_contact_length_mm = 23.0;
    without_finish.resin_overlap_mm = 0.05;
    without_finish.outside_tolerance_mm2 = 0.01;
    ContinuousFiberConfig with_finish = without_finish;
    with_finish.finish_extension_length_mm = 5.0;

    auto id = test_id();id.parent.purpose = FiberPathPurpose::Contour;
    const auto a = finalize_independent(candidate, domain, without_finish, id);
    const auto b = finalize_independent(candidate, domain, with_finish, id);
    REQUIRE(a.prepared != nullptr);
    REQUIRE(b.prepared != nullptr);
    REQUIRE(b.prepared->spans.size() == 3);
    CHECK(b.prepared->spans.back().kind == FiberMotionKind::NonDepositingFinish);
    CHECK(std::abs(area(a.prepared->physical_coverage)) == Catch::Approx(std::abs(area(b.prepared->physical_coverage))));
    CHECK(std::abs(area(a.prepared->resin_exclusion)) == Catch::Approx(std::abs(area(b.prepared->resin_exclusion))));
}

TEST_CASE("contour coverage policies retain their widths and domain clipping", "[ContinuousFiber][coverage]")
{
    ExtrusionPath candidate(erContinuousFiberContour, 0.1, 1.0f, 0.2f);
    candidate.polyline = straight_path(10, 10, 60, 10).polyline;
    auto id = test_id();
    id.parent.purpose = FiberPathPurpose::Contour;
    ContinuousFiberConfig config;
    config.resin_overlap_mm = GENERATE(0.0, 0.1);
    config.contour_infill_clearance_mm = GENERATE(0.0, 0.3);
    config.z_hop_height_mm = 2;
    config.cut_to_contact_length_mm = 10;
    // The physical strand fits exactly at the lower boundary; a wider
    // infill keepout must be clipped at both sides of this narrow domain.
    const ExPolygons domain{rectangle(0, 9.5, 70, 10.7)};
    const auto result = finalize_independent(candidate, domain, config, id);
    REQUIRE(result.prepared);
    const auto area_mm2 = [](const ExPolygons& polygons) {
        return unscaled<double>(unscaled<double>(area(polygons)));
    };
    CHECK(area_mm2(result.prepared->physical_coverage) == Catch::Approx(50.0).margin(.002));
    // Material allocation includes the declared numerical envelope, while
    // physical strand width and the model boundary remain unchanged.
    const double envelope_area=104*result.prepared->execution_deviation_mm+.002;
    const double resin_area=area_mm2(result.prepared->resin_exclusion);
    CHECK(resin_area>=50.0);CHECK(resin_area<=50.0+envelope_area);
    const double nominal_keepout=config.contour_infill_clearance_mm==0?50.0:60.0;
    const double keepout_area=area_mm2(result.prepared->contour_to_infill_keepout);
    CHECK(keepout_area>=nominal_keepout);CHECK(keepout_area<=nominal_keepout+envelope_area);
    CHECK(diff_ex(result.prepared->resin_exclusion,domain).empty());
    CHECK(diff_ex(result.prepared->contour_to_infill_keepout,domain,ApplySafetyOffset::Yes).empty());
    CHECK(result.prepared->outside_domain.empty());
}

TEST_CASE("fiber path shorter than cut process budget is rejected", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(10, 10, 30, 10);
    const ExPolygons domain {rectangle(0, 0, 40, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 23.0;

    const FiberFinalizationResult result = finalize_independent(candidate, domain, config, test_id());
    CHECK(result.prepared == nullptr);
    CHECK(result.failure == FiberFinalizationFailure::TooShort);
}

TEST_CASE("fiber path requires a real powered deposition span beyond its passive tail", "[ContinuousFiber]")
{
    const ExPolygons domain {rectangle(0, 0, 70, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 23.0;
    config.minimum_effective_length_mm = 25.0;

    const FiberFinalizationResult rejected = finalize_independent(
        straight_path(10, 10, 57, 10), domain, config, test_id());
    CHECK(rejected.prepared == nullptr);
    CHECK(rejected.failure == FiberFinalizationFailure::TooShort);

    const FiberFinalizationResult accepted = finalize_independent(
        straight_path(10, 10, 58, 10), domain, config, test_id());
    REQUIRE(accepted.prepared != nullptr);
    CHECK(accepted.prepared->passive_tail_length_mm() == Catch::Approx(23.0).margin(0.001));
    CHECK(accepted.prepared->total_depositing_length_mm() - accepted.prepared->passive_tail_length_mm() ==
          Catch::Approx(25.0).margin(0.001));
}

TEST_CASE("fiber physical footprint outside its island is rejected", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(1, 0.1, 49, 0.1);
    const ExPolygons domain {rectangle(0, 0, 50, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 10.0;
    config.outside_tolerance_mm2 = 0.0;

    const FiberFinalizationResult result = finalize_independent(candidate, domain, config, test_id());
    CHECK(result.prepared == nullptr);
    CHECK(result.failure == FiberFinalizationFailure::OutsideDomain);
}

TEST_CASE("fiber infill cannot cross a contour keepout removed from its allowed domain", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(1, 10, 49, 10);
    const ExPolygons infill_allowed_domain {
        rectangle_with_hole(0, 0, 50, 20, 20, 5, 30, 15)
    };
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 5.0;
    config.outside_tolerance_mm2 = 0.0;

    const FiberFinalizationResult result = finalize_independent(
        candidate, infill_allowed_domain, config, test_id());
    CHECK(result.prepared == nullptr);
    CHECK(result.failure == FiberFinalizationFailure::OutsideDomain);
}

TEST_CASE("fiber validator fragments Orca infill connectors at contour keepouts", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(straight_path(1, 10, 49, 10)));
    const ExtrusionEntitiesPtr candidates {&candidate_collection};
    const ExPolygons infill_allowed_domain {
        rectangle_with_hole(0, 0, 50, 20, 20, 5, 30, 15)
    };
    ContinuousFiberConfig config;
    config.minimum_path_length_mm = 1.0;
    config.cut_to_contact_length_mm = 5.0;
    config.outside_tolerance_mm2 = 0.0;

    FiberValidationResult result = FiberPathValidator::validate(
        candidates,
        infill_allowed_domain,
        config,
        FiberPathPurpose::Infill,
        erContinuousFiberInfill,
        test_id().parent.domain);

    CHECK(result.accepted_count() == 2);
    CHECK(result.rejected_count() == 1);
    REQUIRE(result.assignments.size() == 3);
    CHECK(result.assignments[0].kind == FiberAssignmentKind::AcceptedFiber);
    CHECK(result.assignments[1].kind == FiberAssignmentKind::Rejected);
    CHECK(result.assignments[1].reason == FiberRejectionReason::OutsideDomain);
    CHECK(result.assignments[2].kind == FiberAssignmentKind::AcceptedFiber);
    CHECK(result.assignments[0].source_begin_mm == Catch::Approx(0.0));
    CHECK(result.assignments[0].source_end_mm == Catch::Approx(result.assignments[1].source_begin_mm).margin(1e-5));
    CHECK(result.assignments[1].source_end_mm == Catch::Approx(result.assignments[2].source_begin_mm).margin(1e-5));
    CHECK(result.assignments[2].source_end_mm == Catch::Approx(48.0).margin(1e-5));
    CHECK(result.assignments[0].id.parent == result.assignments[1].id.parent);
    CHECK(result.assignments[1].id.parent == result.assignments[2].id.parent);
    CHECK(result.assignments[0].id.fragment_ordinal == 0);
    CHECK(result.assignments[1].id.fragment_ordinal == 1);
    CHECK(result.assignments[2].id.fragment_ordinal == 2);
    CHECK(result.audit_assignments().valid());
    CHECK(result.outside_domain.empty());
}

TEST_CASE("fiber validator rejects a self-intersecting candidate as one conserved interval", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(path_from_points({
        {1, 1}, {11, 11}, {1, 11}, {11, 1}
    })));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().kind == FiberAssignmentKind::Rejected);
    CHECK(result.assignments.front().reason == FiberRejectionReason::SelfIntersection);
    CHECK(result.assignments.front().source_begin_mm == Catch::Approx(0.0));
    CHECK(result.assignments.front().source_end_mm == Catch::Approx(result.candidates.front().length_mm));
    CHECK(result.audit_assignments().valid());
    CHECK(result.resin_exclusion.empty());
}

TEST_CASE("fiber validator rejects repeated and reversed segments", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(path_from_points({
        {1, 1}, {11, 1}, {1, 1}
    })));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().reason == FiberRejectionReason::DuplicateSegment);
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("fiber validator retains a path containing a short geometric edge", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(path_from_points({
        {1, 1}, {1.5, 1}, {1.5, 11}
    })));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().kind == FiberAssignmentKind::AcceptedFiber);
    CHECK(result.assignments.front().prepared->total_depositing_length_mm() == Catch::Approx(10.5));
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("fiber validator does not use a turn threshold to discard a complete path", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(path_from_points({
        {1, 1}, {6, 1}, {6, 6}
    })));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().kind == FiberAssignmentKind::AcceptedFiber);
    CHECK(result.assignments.front().prepared->total_depositing_length_mm() == Catch::Approx(10));
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("fiber validator rejects non-finite process parameters", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(straight_path(1, 1, 11, 1)));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;
    config.minimum_path_length_mm = std::numeric_limits<double>::quiet_NaN();

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().reason == FiberRejectionReason::InvalidParameter);
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("fiber validator rejects non-finite candidate process measures", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    ExtrusionPath candidate = straight_path(1, 1, 11, 1);
    candidate.mm3_per_mm = std::numeric_limits<double>::infinity();
    candidate_collection.entities.push_back(new ExtrusionPath(std::move(candidate)));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().reason == FiberRejectionReason::InvalidGeometry);
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("fiber candidate and fragment IDs are stable across repeated validation", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(straight_path(1, 10, 49, 10)));
    const ExPolygons domain {rectangle_with_hole(0, 0, 50, 20, 20, 5, 30, 15)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 5.0;

    const FiberValidationResult first = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain, 3);
    const FiberValidationResult second = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain, 3);

    REQUIRE(first.assignments.size() == second.assignments.size());
    for (size_t index = 0; index < first.assignments.size(); ++index) {
        CHECK(first.assignments[index].id == second.assignments[index].id);
        CHECK(first.assignments[index].source_begin_mm == Catch::Approx(second.assignments[index].source_begin_mm));
        CHECK(first.assignments[index].source_end_mm == Catch::Approx(second.assignments[index].source_end_mm));
        CHECK(first.assignments[index].kind == second.assignments[index].kind);
        CHECK(first.assignments[index].reason == second.assignments[index].reason);
    }
}

TEST_CASE("fiber assignment audit detects a missing source interval", "[ContinuousFiber]")
{
    FiberValidationResult result;
    const FiberCandidateId candidate_id {{7, 62, 4, 1}, FiberPathPurpose::Infill, 0, 3};
    result.candidates.push_back({candidate_id, 10.0});

    FiberFragmentAssignment first;
    first.id = {candidate_id, 0};
    first.source_begin_mm = 0.0;
    first.source_end_mm = 4.0;
    first.kind = FiberAssignmentKind::Rejected;
    first.reason = FiberRejectionReason::OutsideDomain;
    result.assignments.emplace_back(std::move(first));

    FiberFragmentAssignment second;
    second.id = {candidate_id, 1};
    second.source_begin_mm = 5.0;
    second.source_end_mm = 10.0;
    second.kind = FiberAssignmentKind::Rejected;
    second.reason = FiberRejectionReason::OutsideDomain;
    result.assignments.emplace_back(std::move(second));

    const FiberAssignmentAudit audit = result.audit_assignments();
    CHECK_FALSE(audit.valid());
    CHECK(audit.gap_length_mm == Catch::Approx(1.0));
}

TEST_CASE("fiber finalizer rejects non-finite process parameters", "[ContinuousFiber]")
{
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = std::numeric_limits<double>::infinity();
    const FiberFinalizationResult result = finalize_independent(
        straight_path(1, 1, 11, 1), ExPolygons {rectangle(0, 0, 12, 12)}, config, test_id());
    CHECK(result.prepared == nullptr);
    CHECK(result.failure == FiberFinalizationFailure::InvalidParameter);
}


TEST_CASE("fiber feed is independent of plastic flow geometry and binds hardware units once", "[ContinuousFiber][feed]")
{
    auto candidate = straight_path(5, 5, 25, 5);
    const ExPolygons domain{rectangle(0, 0, 40, 20)};
    ContinuousFiberConfig config;
    config.infill_feed_ratio = 1.02;
    config.infill_feed_correction = 1.05;
    const auto a = finalize_independent(candidate, domain, config, test_id());
    REQUIRE(a.prepared);
    candidate.mm3_per_mm *= 3;
    candidate.height *= 2;
    candidate.width *= 2;
    const auto b = finalize_independent(candidate, domain, config, test_id());
    REQUIRE(b.prepared);
    const auto bound_a = compile_fixture(a.prepared, 0, 1, 3, 2.0, ";cut");
    const auto bound_b = compile_fixture(b.prepared, 0, 1, 3, 2.0, ";cut");
    double sum_a = 0, sum_b = 0;
    for (const auto& span : bound_a.spans()) for (const auto& command : span.commands) sum_a += command.dE;
    for (const auto& span : bound_b.spans()) for (const auto& command : span.commands) sum_b += command.dE;
    CHECK(sum_a == Catch::Approx(20*1.02*1.05*2));
    CHECK(sum_b == Catch::Approx(sum_a));
    CHECK_THROWS(compile_fixture(a.prepared, 1, 1, 3, 2.0, ";cut"));
    CHECK_THROWS(compile_fixture(a.prepared, 0, 1, 3, 0.0, ";cut"));
}

TEST_CASE("fiber E quantization carries residuals before cut only", "[ContinuousFiber][feed]")
{
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;config.z_hop_height_mm = 2;
    config.finish_extension_length_mm=23;
    config.infill_feed_ratio=config.contour_feed_ratio=1.000014;config.speed_sampling_length_mm=.01;
    auto id=test_id();id.parent.purpose=GENERATE(FiberPathPurpose::Infill,FiberPathPurpose::Contour);
    const auto result=finalize_independent(straight_path(0,5,100,5),{rectangle(-5,0,110,15)},config,id);
    REQUIRE(result.prepared);
    const double units=GENERATE(.5,1.,2.);
    const auto bound=compile_fixture(result.prepared,0,0,0,units,";cut");
    double ideal=result.prepared->start_procedure.prefeed_length_mm*units;
    double emitted=bound.prefeed_dE,naive=bound.prefeed_dE;
    size_t fed_edges=0;
    for(size_t s=0;s<result.prepared->spans.size();++s){
        const auto& span=result.prepared->spans[s];
        for(size_t e=0;e<span.edges.size();++e){
            const double length=unscale<double>((span.geometry.points[e+1]-span.geometry.points[e]).cast<double>().norm());
            const double delta=length*span.edges[e].feed_mm_per_xy_mm*units;
            ideal+=delta;naive+=GCodeFormatter::quantize_e(delta);emitted+=bound.spans()[s].commands[e].dE;
            if(span.actively_feeds_fiber()){
                CHECK(bound.spans()[s].commands[e].dE>0);++fed_edges;
                CHECK(emitted==Catch::Approx(ideal).margin(.0000051));
            }else CHECK(bound.spans()[s].commands[e].dE==0);
        }
    }
    CHECK(fed_edges>7000);
    CHECK(std::abs(naive-ideal)>.0005);
    CHECK(emitted==Catch::Approx(ideal).margin(.0000051));
}

TEST_CASE("fiber normal speed is invariant under collinear tessellation", "[ContinuousFiber][speed]")
{
    auto a = straight_path(5, 5, 25, 5);
    auto b = a;
    b.polyline.points.clear();
    for (int i = 5; i <= 25; ++i) b.polyline.points.push_back(Point3::new_scale(i, 5, 0));
    ContinuousFiberConfig config;
    const ExPolygons domain{rectangle(0, 0, 40, 20)};
    const auto pa = finalize_independent(a, domain, config, test_id());
    b.polyline = normalize_fiber_geometry(b.polyline);
    const auto pb = finalize_independent(b, domain, config, test_id());
    REQUIRE(pa.prepared); REQUIRE(pb.prepared);
    const auto& sa = pa.prepared->spans.front();
    const auto& sb = pb.prepared->spans.front();
    CHECK(sa.geometry.points == sb.geometry.points);
    REQUIRE(sa.edges.size() == sb.edges.size());
    for (size_t i = 0; i < sa.edges.size(); ++i)
        CHECK(sa.edges[i].speed_mm_s == Catch::Approx(sb.edges[i].speed_mm_s));
}

TEST_CASE("fiber closed seam uses cyclic corner adjacency", "[ContinuousFiber][speed]")
{
    const auto candidate = path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    ContinuousFiberConfig config;
    const auto result = finalize_independent(candidate, {rectangle(0,0,30,30)}, config, test_id());
    REQUIRE(result.prepared);
    const auto& entry=result.prepared->spans.front();
    REQUIRE(entry.kind==FiberMotionKind::PoweredEntry);
    const auto& edges=result.prepared->spans.back().edges;
    CHECK(edges.front().speed_mm_s == Catch::Approx(6.5));
    CHECK(edges.back().speed_mm_s == Catch::Approx(6.5));
}

TEST_CASE("fiber tail is forcibly sampled by arc length with exact endpoint speeds", "[ContinuousFiber][speed]")
{
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 23;
    config.tail_speed_step_length_mm = 2;
    config.tail_min_speed_mm_s = 3;
    config.tail_max_speed_mm_s = 10;
    const auto result = finalize_independent(straight_path(5,5,55,5),
        {rectangle(0,0,60,10)}, config, test_id());
    REQUIRE(result.prepared);
    const auto& tail = result.prepared->spans.back();
    REQUIRE(tail.edges.size() == 12);
    CHECK(tail.edges.front().speed_mm_s == Catch::Approx(3));
    CHECK(tail.edges.back().speed_mm_s == Catch::Approx(10));
    for (size_t i = 0; i < tail.edges.size(); ++i) {
        CHECK(tail.edges[i].feed_mm_per_xy_mm == 0);
        CHECK(unscale<double>((tail.geometry.points[i+1]-tail.geometry.points[i]).cast<double>().norm()) <= 2.000001);
    }
}

TEST_CASE("rectilinear infill uses its tangent while contours retain their policies", "[ContinuousFiber][finish]")
{
    const auto candidate = path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    ContinuousFiberConfig config;
    config.finish_extension_length_mm = 2;
    config.finish_overlap_length_mm = 3;
    const ExPolygons domain{rectangle(0,0,30,30)};
    const auto infill = finalize_independent(candidate, domain, config, test_id());
    REQUIRE(infill.prepared);
    CHECK(infill.prepared->finish_strategy == FiberFinishStrategy::TangentExtension);
    CHECK(infill.prepared->spans.back().geometry.points.back() == Point3::new_scale(5,3,0));
    auto contour_id = test_id();
    contour_id.parent.purpose = FiberPathPurpose::Contour;
    const auto contour = finalize_independent(candidate, domain, config, contour_id);
    REQUIRE(contour.prepared);
    CHECK(contour.prepared->finish_strategy == FiberFinishStrategy::LoopOverlap);
    CHECK(contour.prepared->spans.back().geometry.points.back() == Point3::new_scale(8,5,0));
    const auto open = finalize_independent(straight_path(5,5,25,5), domain, config, contour_id);
    REQUIRE(open.prepared);
    CHECK(open.prepared->finish_strategy == FiberFinishStrategy::TangentExtension);
    CHECK(open.prepared->spans.back().geometry.points.back() == Point3::new_scale(27,5,0));
}

TEST_CASE("rectilinear non-depositing finish may cross a model hole", "[ContinuousFiber][finish]")
{
    ContinuousFiberConfig config;
    config.finish_extension_length_mm = 15;
    const auto result = finalize_independent(straight_path(2,5,10,5),
        {rectangle_with_hole(0,0,30,10,15,2,20,8)}, config, test_id());
    REQUIRE(result.prepared);
    CHECK(result.prepared->finish_strategy == FiberFinishStrategy::TangentExtension);
    CHECK(result.prepared->spans.back().kind == FiberMotionKind::NonDepositingFinish);
    CHECK(result.prepared->spans.back().geometry.points.back() == Point3::new_scale(25,5,0));
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(8));
}

TEST_CASE("rectilinear infill separates the source endpoint from its finish endpoint", "[ContinuousFiber][finish]")
{
    ContinuousFiberConfig config;
    config.z_hop_height_mm = 2;
    config.minimum_effective_length_mm = 0.5;
    config.cut_to_contact_length_mm = 23;
    const ExPolygons domain {rectangle(0,0,60,10)};
    const auto source = straight_path(5,5,55,5);
    const auto baseline = finalize_independent(source, domain, config, test_id());
    REQUIRE(baseline.prepared);
    config.finish_extension_length_mm = 23;
    const auto result = finalize_independent(source, domain, config, test_id());
    REQUIRE(result.prepared);
    CHECK(result.prepared->passive_tail_length_mm() == Catch::Approx(23));
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(50));
    CHECK(area(result.prepared->physical_coverage) == Catch::Approx(area(baseline.prepared->physical_coverage)));
    CHECK(area(result.prepared->resin_exclusion) == Catch::Approx(area(baseline.prepared->resin_exclusion)));
    const auto& tail = result.prepared->spans[result.prepared->spans.size()-2];
    CHECK(result.prepared->finish_strategy == FiberFinishStrategy::TangentExtension);
    CHECK(tail.kind == FiberMotionKind::PassiveDepositingAfterCut);
    CHECK(tail.geometry.points.back() == source.polyline.points.back());
    CHECK(unscale<double>(tail.geometry.length()) == Catch::Approx(23));
    CHECK(tail.edges.front().feed_mm_per_xy_mm == 0);
    CHECK(result.prepared->spans.size() == baseline.prepared->spans.size()+1);
    for (size_t i = 0; i < baseline.prepared->spans.size(); ++i) {
        CHECK(result.prepared->spans[i].geometry.points == baseline.prepared->spans[i].geometry.points);
        for (size_t e = 0; e < baseline.prepared->spans[i].edges.size(); ++e)
            CHECK(result.prepared->spans[i].edges[e].feed_mm_per_xy_mm ==
                  baseline.prepared->spans[i].edges[e].feed_mm_per_xy_mm);
    }
    auto sampled = source;
    sampled.polyline.points.insert(sampled.polyline.points.end()-1, Point3::new_scale(54,5,0));
    sampled.polyline = normalize_fiber_geometry(sampled.polyline);
    const auto same = finalize_independent(sampled, domain, config, test_id());
    REQUIRE(same.prepared);
    CHECK(same.prepared->spans[same.prepared->spans.size()-2].geometry.points == tail.geometry.points);
    CHECK_FALSE(finalize_independent(straight_path(5,5,65,5), domain, config, test_id()).prepared);
}

TEST_CASE("fiber motion checks real tool coordinates exclusions and height", "[ContinuousFiber][machine-motion]")
{
    FiberMachineMotionLimits limits;
    limits.tip_xy = {rectangle_with_hole(0,0,100,100,40,40,60,60)};
    limits.maximum_z_mm = 100;
    limits.command_to_tip_offset = Vec2d(-19,0);
    CHECK_NOTHROW(limits.validate_move(Vec3d(24,5,1), Vec3d(97,5,1)));
    CHECK_THROWS(limits.validate_move(Vec3d(24,5,1), Vec3d(120,5,1)));
    CHECK_THROWS(limits.validate_move(Vec3d(49,50,1), Vec3d(89,50,1)));
    CHECK_THROWS(limits.validate_move(Vec3d(24,5,99), Vec3d(24,5,101)));
    CHECK_THROWS(limits.validate_move(Vec3d(24,5,1), Vec3d(24,5,-1)));
    CHECK_THROWS(limits.validate_move(Vec3d(24,5,1), Vec3d(std::numeric_limits<double>::infinity(),5,1)));
    // A translated instance must be validated again in its new machine position.
    CHECK_THROWS(limits.validate_move(Vec3d(74,5,1), Vec3d(147,5,1)));
    CHECK_NOTHROW(limits.validate_move(Vec3d(19,0,0), Vec3d(119,0,0)));
}

TEST_CASE("fiber machine line certificates retain the original Clipper decision",
    "[ContinuousFiber][machine-motion][MachineMoveCertificate]")
{
    // The oracle is the pre-certificate implementation, including the actual
    // Clipper operation. It deliberately contains no copy of the fast proof.
    const auto original_validate = [](const FiberMachineMotionLimits& limits, const Vec3d& from, const Vec3d& to) {
        if (limits.tip_xy.empty() || !limits.command_to_tip_offset.allFinite() ||
            !std::isfinite(limits.maximum_z_mm) || limits.maximum_z_mm <= 0)
            throw std::runtime_error("Fiber machine motion limits are unavailable");
        Points xy;
        for (const Vec3d& p : {from, to}) {
            if (!p.allFinite() || p.z() < 0 || p.z() > limits.maximum_z_mm ||
                p.head<2>().cwiseAbs().maxCoeff() > 1000000.0)
                throw std::runtime_error("Fiber motion exceeds machine Z/coordinate limits");
            const Vec2d tip = p.head<2>() + limits.command_to_tip_offset;
            const Point point = Point::new_scale(tip.x(), tip.y());
            if (!std::any_of(limits.tip_xy.begin(), limits.tip_xy.end(),
                [&](const ExPolygon& area) { return area.contains(point); }))
                throw std::runtime_error("Fiber motion exceeds the configured tool working area");
            xy.push_back(point);
        }
        if (xy.front() != xy.back() && total_length(diff_pl(Polylines{Polyline(std::move(xy))},
            limits.line_xy.empty() ? offset_ex(limits.tip_xy, 1.0f) : limits.line_xy)) > scale_(0.00001))
            throw std::runtime_error("Fiber motion crosses a configured machine exclusion area");
    };
    const auto outcome = [](const auto& operation) -> std::string {
        try { operation(); return {}; }
        catch (const std::exception& error) { return error.what(); }
    };
    const auto compare = [&](const FiberMachineMotionLimits& limits, const Vec3d& from, const Vec3d& to,
        int expected_accept = -1) {
        CAPTURE(from.x(), from.y(), from.z(), to.x(), to.y(), to.z());
        const std::string original = outcome([&] { original_validate(limits, from, to); });
        CHECK(outcome([&] { limits.validate_move(from, to); }) == original);
        if (expected_accept >= 0)
            CHECK(original.empty() == bool(expected_accept));
    };
    FiberMachineMotionLimits limits;
    limits.tip_xy = {rectangle(-100,-100,500,500)};
    limits.command_to_tip_offset = Vec2d::Zero();
    limits.maximum_z_mm = 100;
    limits.prepare();

    SECTION("real concave machine domain includes both interior and crossing moves") {
        ExPolygon domain;
        domain.contour = Polygon::new_scale({{392,350},{435,350},{435,355},{0,355},
            {0,350},{27,350},{27,0},{392,0}});
        limits.tip_xy = {domain};
        limits.prepare();
        compare(limits, {200,170,1}, {270,199,1}, 1);
        compare(limits, {10,352,1}, {425,352,1}, 1);
        // Both endpoints are inside the concave polygon, but its notch is not.
        compare(limits, {10,352,1}, {40,340,1}, 0);
        compare(limits, {10,352,1}, {44,348,1}, 1);
        std::reverse(limits.line_xy.front().contour.points.begin(), limits.line_xy.front().contour.points.end());
        compare(limits, {270,199,1}, {200,170,1}, 1);
        limits.command_to_tip_offset = {-19,0};
        compare(limits, {219,170,1}, {289,199,1}, 1);
    }
    SECTION("boundary vertices and one grid offsets retain original tolerances") {
        limits.tip_xy = {rectangle(0,0,100,100)};
        limits.prepare();
        compare(limits, {0,0,1}, {100,0,1}, 1);
        compare(limits, {0,0,1}, {100,100,1}, 1);
        limits.tip_xy = {rectangle(-10,-10,110,110)};
        limits.line_xy = {rectangle(0,0,100,100)};
        for (const int grid : {-1,0,1}) {
            compare(limits, {20,grid*SCALING_FACTOR,1}, {80,grid*SCALING_FACTOR,1});
            compare(limits, {grid*SCALING_FACTOR,grid*SCALING_FACTOR,1}, {50,50,1});
        }
        for (const int outside_grid : {1,9,10,11,20})
            compare(limits, {-outside_grid*SCALING_FACTOR,50,1}, {1,50,1}, outside_grid <= 10);
        // This segment is inside, although its box touches the sloped edge box.
        ExPolygon triangle;
        triangle.contour = Polygon::new_scale({{0,0},{100,0},{0,100}});
        limits.line_xy = {triangle};
        compare(limits, {10,10,1}, {20,20,1}, 1);
    }
    SECTION("holes and multiple components keep full segment checks") {
        limits.tip_xy = {rectangle_with_hole(0,0,100,100,40,40,60,60)};
        limits.prepare();
        compare(limits, {30,50,1}, {70,50,1}, 0);
        compare(limits, {10,10,1}, {30,30,1}, 1);
        limits.tip_xy = {rectangle(0,0,10,10), rectangle(20,0,30,10)};
        limits.prepare();
        compare(limits, {5,5,1}, {25,5,1}, 0);
        compare(limits, {21,1,1}, {29,9,1}, 1);
    }
    SECTION("public degenerate self crossing and repeated contours retain NonZero semantics") {
        const std::vector<Points> contours {
            {}, {{0,0}}, {{0,0},Point::new_scale(100,0)},
            {{0,0},Point::new_scale(50,0),Point::new_scale(100,0)},
            {{0,0},Point::new_scale(100,100),Point::new_scale(0,100),Point::new_scale(100,0)}
        };
        for (const Points& points : contours) {
            limits.line_xy = {ExPolygon(Polygon(points))};
            compare(limits, {25,10,1}, {30,10,1});
            compare(limits, {25,50,1}, {75,50,1});
        }
        const Points loop = rectangle(0,0,100,100).contour.points;
        Points repeated = loop;
        repeated.insert(repeated.end(), loop.begin(), loop.end());
        limits.line_xy = {ExPolygon(Polygon(repeated))};
        compare(limits, {25,50,1}, {75,50,1}, 1); // Even parity, nonzero winding.
        repeated.insert(repeated.end(), loop.begin(), loop.end());
        limits.line_xy = {ExPolygon(Polygon(repeated))};
        compare(limits, {25,50,1}, {75,50,1}, 1);
        Points cancelled = loop;
        cancelled.insert(cancelled.end(), loop.rbegin(), loop.rend());
        limits.line_xy = {ExPolygon(Polygon(cancelled))};
        compare(limits, {25,50,1}, {75,50,1}, 0);
    }
    SECTION("safe arithmetic range limits only the certificate") {
        for (const coord_t extent : {coord_t(ClipperLib::loRange), coord_t(ClipperLib::loRange)+1}) {
            ExPolygon domain;
            domain.contour.points = {{-extent,-extent},{extent,-extent},{extent,extent},{-extent,extent}};
            limits.tip_xy = {domain};
            limits.line_xy = {domain};
            const double near_edge = (extent-10)*SCALING_FACTOR;
            compare(limits, {near_edge,0,1}, {near_edge-SCALING_FACTOR,1,1}, 1);
            compare(limits, {-near_edge,0,1}, {-near_edge+SCALING_FACTOR,-1,1}, 1);
            compare(limits, {-1000,-1000,1}, {1000,1000,1}, 1);
        }
        limits.tip_xy = {rectangle(0,0,100,100)};
        limits.line_xy.front().contour.points = {{0,0},{ClipperLib::hiRange+1,0},
            Point(coord_t(ClipperLib::hiRange+1),coord_t(scale_(100))),Point::new_scale(0,100)};
        compare(limits, {20,20,1}, {80,80,1}, 0); // Preserve Clipper's range exception.
        compare(limits, {20,20,1}, {20,20,1}, 1); // Original zero-XY short circuit.
    }
    SECTION("public line domain changes are observed on every call") {
        limits.tip_xy = {rectangle(0,0,100,100)};
        limits.prepare();
        compare(limits, {20,50,1}, {80,50,1}, 1);
        limits.line_xy = {rectangle_with_hole(0,0,100,100,40,40,60,60)};
        compare(limits, {20,50,1}, {80,50,1}, 0);
        limits.line_xy = {rectangle(0,0,10,100)};
        compare(limits, {20,50,1}, {80,50,1}, 0);
        limits.line_xy.clear();
        compare(limits, {0,0,1}, {100,0,1}, 1);
        limits.prepare();
        compare(limits, {20,50,1}, {80,50,1}, 1);
    }
    SECTION("tool offset finite coordinate and height guards remain mandatory") {
        limits.tip_xy = {rectangle(0,0,100,100)};
        limits.prepare();
        limits.command_to_tip_offset = {-19,0};
        compare(limits, {24,5,1}, {97,5,1}, 1);
        compare(limits, {24,5,1}, {120,5,1}, 0);
        compare(limits, {24,5,99}, {24,5,101}, 0);
        compare(limits, {24,5,-1}, {24,5,1}, 0);
        compare(limits, {24,5,1}, {1000001,5,1}, 0);
        compare(limits, {24,5,1}, {std::numeric_limits<double>::infinity(),5,1}, 0);
        limits.command_to_tip_offset.x() = std::numeric_limits<double>::quiet_NaN();
        compare(limits, {24,5,1}, {97,5,1}, 0);
        limits.command_to_tip_offset = Vec2d::Zero();
        limits.maximum_z_mm = std::numeric_limits<double>::quiet_NaN();
        compare(limits, {24,5,1}, {97,5,1}, 0);
        limits.maximum_z_mm = 100;
        limits.tip_xy.clear();
        compare(limits, {24,5,1}, {97,5,1}, 0);
    }
}

TEST_CASE("fiber arc feedrate limits follow command direction independently of chord samples",
    "[ContinuousFiber][NativeArc][machine-motion]")
{
    const double radius_component=GENERATE(.000007,1.0);
    const Vec3d from(radius_component,radius_component,1),to(radius_component,-radius_component,1);
    FiberMachineMotionLimits limits;
    limits.maximum_axis_speed_mm_s=Vec3d(8,20,20);
    // Endpoint tangents use 10/sqrt(2) mm/s in X. Only the 270-degree
    // route reaches 10 mm/s; the tiny arc may have just one check chord.
    CHECK_NOTHROW(limits.validate_feedrate(from,to,10,EMovePathType::Arc_move_cw));
    CHECK_THROWS(limits.validate_feedrate(from,to,10,EMovePathType::Arc_move_ccw));
    CHECK_NOTHROW(limits.validate_feedrate(to,from,10,EMovePathType::Arc_move_ccw));
    CHECK_THROWS(limits.validate_feedrate(to,from,10,EMovePathType::Arc_move_cw));
    CHECK_THROWS(limits.validate_feedrate(from,from,10,EMovePathType::Arc_move_ccw));
    limits.maximum_axis_speed_mm_s.x()=10;
    CHECK_NOTHROW(limits.validate_feedrate(from,to,10,EMovePathType::Arc_move_ccw));
    CHECK_NOTHROW(limits.validate_feedrate(to,from,10,EMovePathType::Arc_move_cw));
    CHECK_NOTHROW(limits.validate_feedrate(from,from,10,EMovePathType::Arc_move_ccw));
    limits.maximum_axis_speed_mm_s.z()=2;
    CHECK_THROWS(limits.validate_feedrate(from,from+Vec3d(0,0,1),3));
    CHECK_NOTHROW(limits.validate_feedrate(from,from+Vec3d(0,0,1),2));
}

TEST_CASE("fiber numerical knots and speed sampling do not discard a closed contour", "[ContinuousFiber][geometry]")
{
    // A near-duplicate from offsetting plus a real corner very close to the
    // 2 mm sampling grid. Neither may invalidate the complete 40 mm loop.
    auto candidate = path_from_points({{5,5},{5.00001,5.00001},
        {15.0001,5},{15.0001,15},{5,15},{5,5.00001},{5,5}});
    candidate.polyline = normalize_fiber_geometry(candidate.polyline);
    ContinuousFiberConfig config;
    config.z_hop_height_mm = 2;
    config.cut_to_contact_length_mm = 23;
    config.minimum_effective_length_mm = 0.5;
    config.finish_overlap_length_mm = 23;
    config.tail_min_speed_mm_s = config.tail_max_speed_mm_s = 10;
    auto id = test_id(); id.parent.purpose = FiberPathPurpose::Contour;
    const auto result = finalize_independent(candidate, {rectangle(0,0,20,20)}, config, id);
    REQUIRE(result.prepared);
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(40.0002).margin(0.003));
    CHECK(result.prepared->entry.start == candidate.polyline.points.front());
    CHECK(result.prepared->spans.front().geometry.points.front() == result.prepared->entry.contact);
    CHECK_NOTHROW(result.prepared->validate());
    for (const auto& span : result.prepared->spans)
        for (size_t i = 1; i < span.geometry.points.size(); ++i)
            CHECK(unscale<double>((span.geometry.points[i]-span.geometry.points[i-1]).cast<double>().norm()) > 0.0014143);
}

TEST_CASE("fiber loop overlap does not weaken depositing boundary validation", "[ContinuousFiber][finish]")
{
    const auto candidate = path_from_points({{0.49999,1},{20,1},{20,10},{0.49999,10},{0.49999,1}}, 1.0);
    ContinuousFiberConfig config;
    config.finish_overlap_length_mm = 23;
    config.outside_tolerance_mm2 = 0.01;
    auto id = test_id(); id.parent.purpose = FiberPathPurpose::Contour;
    const ExPolygons domain {rectangle(0,0,30,20)};
    const auto result = finalize_independent(candidate, domain, config, id);
    REQUIRE(result.prepared);
    CHECK(result.prepared->finish_strategy == FiberFinishStrategy::LoopOverlap);
    auto unsafe = candidate;
    for (auto& p : unsafe.polyline.points) if (p.x() < scale_(1)) p.x() = scale_(0.3);
    CHECK_FALSE(finalize_independent(unsafe, domain, config, id).prepared);
}

TEST_CASE("fiber LayerXY and immutable entity contracts reject mutation", "[ContinuousFiber]")
{
    auto candidate = straight_path(5,5,25,5);
    ContinuousFiberConfig config;
    const ExPolygons domain{rectangle(0,0,30,10)};
    candidate.polyline.points.back().z() = scale_(0.2);
    CHECK_FALSE(finalize_independent(candidate, domain, config, test_id()).prepared);
    candidate.polyline.points.back().z() = 0;
    const auto result = finalize_independent(candidate, domain, config, test_id());
    REQUIRE(result.prepared);
    ExtrusionFiberPath fiber(candidate, erContinuousFiberInfill, result.prepared);
    ExtrusionPath& base = fiber;
    CHECK_THROWS(base.simplify(10));
    CHECK_THROWS(base.simplify_by_fitting_arc(10));
    CHECK_THROWS(base.clip_end(10));
    CHECK_THROWS(base.reverse());
    CHECK_THROWS(base.intersect_expolygons(domain, nullptr));
    CHECK_NOTHROW(fiber.validate_derived_view());
    base.polyline.points.front().x() += 1;
    CHECK_THROWS(fiber.validate_derived_view());
    auto broken = std::make_unique<FiberPathDraft>(result.prepared->draft());
    broken->spans.front().edges.pop_back();
    const auto context=fiber_fixture_context();
    CHECK_THROWS(compile_fiber_execution(*broken,*context,context->placements.front(),0));
    auto quantized = std::make_unique<FiberPathDraft>(result.prepared->draft());
    auto& edge_points = quantized->spans.front().geometry.points;
    edge_points[1] = edge_points[0] + Point3(scale_(0.0000001), scale_(0.0000001), 0.0);
    CHECK_THROWS(quantized->validate());
}

TEST_CASE("fiber tool resolution keeps material logical extruder and physical tool distinct", "[ContinuousFiber][mapping]")
{
    GCodeConfig config;
    config.filament_process_type.values = {"thermoplastic","continuous_fiber"};
    config.filament_map.values = {2,1};
    config.physical_extruder_map.values = {1,0};
    config.toolhead_process_capabilities.values = {"thermoplastic","continuous_fiber"};
    config.toolhead_fiber_e_units_per_mm.values = {1,2};
    config.toolhead_fiber_protocol_id.values = {"","linear-e-v1"};
    const auto tool = resolve_fiber_tool(config, 1);
    CHECK(tool.logical_filament_id == 1);
    CHECK(tool.logical_extruder_id == 0);
    CHECK(tool.physical_tool_id == 1);
    CHECK(tool.e_units_per_mm == 2);
    config.physical_extruder_map.values = {0,1};
    CHECK_THROWS(resolve_fiber_tool(config, 1));
}

TEST_CASE("CFSYS physical tool ownership is independent of material order", "[ContinuousFiber][mapping][tool-materials]")
{
    GCodeConfig config;
    config.gcode_flavor.value = gcfKlipper;
    config.filament_process_type.values = {"thermoplastic", "thermoplastic", "continuous_fiber"};
    config.filament_diameter.values = {1.75, 1.75, 0.35};
    config.filament_map.values = {2, 2, 1};
    config.physical_extruder_map.values = {1, 0};
    config.toolhead_process_capabilities.values = {"thermoplastic", "continuous_fiber"};
    config.toolhead_filament_capacity.values = {4, 1};
    config.toolhead_fiber_protocol_id.values = {"", "cfsys-v1"};
    config.toolhead_fiber_e_units_per_mm.values = {1, 1};
    REQUIRE_NOTHROW(validate_material_tool_bindings(config));
    const auto tool = resolve_fiber_tool(config, 2);
    CHECK(tool.logical_filament_id == 2);
    CHECK(tool.logical_extruder_id == 0);
    CHECK(tool.physical_tool_id == 1);
    SECTION("base material cannot use T1") {
        config.filament_map.values[0] = 1;
        CHECK_THROWS(validate_material_tool_bindings(config));
    }
    SECTION("fiber cannot use T0") {
        config.filament_map.values[2] = 2;
        CHECK_THROWS(resolve_fiber_tool(config, 2));
    }
    SECTION("fiber capacity is one regardless of slot positions") {
        config.filament_process_type.values[0] = "continuous_fiber";
        config.filament_map.values[0] = 1;
        CHECK_THROWS(validate_material_tool_bindings(config));
    }
}

TEST_CASE("fiber script boundary rejects unknown macros and unowned E", "[ContinuousFiber][scripts]")
{
    CHECK_NOTHROW(require_fiber_safe_script("; only comments\n", "test"));
    for (const std::string script : {"G1 E1", "G10", "G11", "PRINT_END", "M221 S95", "{if 1}G1 E1{endif}"})
        CHECK_THROWS(require_fiber_safe_script(script, "test"));
    CHECK_NOTHROW(validate_fiber_cut_event("M400\nM42 P4 S255\nG4 P100\n"));
    CHECK_THROWS(validate_fiber_cut_event("CF_CUT"));
    CHECK_THROWS(validate_fiber_cut_event("M42 P4 S255"));
    CHECK_THROWS(validate_fiber_cut_event("M42 P4 S255 E10\nG4 P100"));
}

TEST_CASE("fiber writer owns linear feed and rejects generic retract repair", "[ContinuousFiber][writer]")
{
    PrintConfig config;
    config.single_extruder_multi_material.value = false;
    config.filament_process_type.values = {"thermoplastic", "continuous_fiber"};
    config.filament_map.values = {2, 1};
    config.physical_extruder_map.values = {1, 0};
    config.toolhead_process_capabilities.values = {"thermoplastic", "continuous_fiber"};
    config.toolhead_fiber_protocol_id.values = {"", "linear-e-v1"};
    config.toolhead_fiber_e_units_per_mm.values = {1, 2};
    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_extruders({0, 1});
    CHECK(writer.toolchange(1).find("T1") != std::string::npos);
    CHECK(writer.set_temperature(190, false, 1).find("T1") != std::string::npos);
    CHECK(writer.set_temperature(210, false, 0).find("T0") != std::string::npos);
    writer.filament()->extrude(20);
    CHECK(writer.filament()->used_filament() == Catch::Approx(10));
    CHECK(writer.retract().empty());
    CHECK(writer.unretract().empty());
    writer.filament()->set_retracted(1, 0.2);
    CHECK_THROWS(writer.retract());
    CHECK_THROWS(writer.unretract());
    CHECK(writer.filament()->effective_retracted() == 1);
    CHECK(writer.filament()->restart_extra() == Catch::Approx(0.2));
}

TEST_CASE("CFSYS toolchange clearance preserves existing and pending travel lifts", "[ContinuousFiber][cfsys][writer]")
{
    PrintConfig config;
    config.z_hop.values = {0.8};
    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_extruders({0});
    writer.toolchange(0);
    writer.set_position(Vec3d(100, 100, 5));
    const bool already_lifted = GENERATE(false, true);
    if (already_lifted) writer.eager_lift(LiftType::NormalLift);
    else writer.lazy_lift();
    const double original_z = writer.get_position().z();
    const double original_hop = writer.get_zhop();
    CHECK(writer.travel_to_z_for_toolchange(original_z + 10, 508).find(" E") == std::string::npos);
    CHECK(writer.get_position().z() == Catch::Approx(original_z + 10));
    CHECK(writer.get_zhop() == Catch::Approx(original_hop));
    writer.travel_to_z_for_toolchange(original_z, 508);
    CHECK(writer.get_position().z() == Catch::Approx(original_z));
    CHECK(writer.get_zhop() == Catch::Approx(original_hop));
    // A pending ordinary hop must still happen on the next travel.
    writer.travel_to_xyz(Vec3d(110, 100, 5));
    CHECK(writer.get_position().z() == Catch::Approx(5.8));
    writer.unlift();
    CHECK(writer.get_position().z() == Catch::Approx(5));
    writer.set_position(Vec3d(100, 100, 500));
    CHECK_THROWS(writer.travel_to_z_for_toolchange(510, 508));
    CHECK(writer.get_position().z() == 500);
}

TEST_CASE("per-call travel limits cover XY XYZ lifts and belt motion", "[ContinuousFiber][fiber-approach][writer]")
{
    const int branch = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    const auto move = [&](std::optional<double> cap, double baseline = 350.0) {
        PrintConfig config;
        config.travel_speed.value = baseline;
        config.travel_speed_z.value = 120;
        config.z_hop.values = {2};
        config.travel_slope.values = {30};
        config.enable_arc_fitting.value = branch == 6;
        GCodeWriter writer;
        writer.apply_print_config(config);
        writer.set_extruders({0});
        writer.toolchange(0);
        writer.set_is_first_layer(false);
        writer.set_position(Vec3d(100, 100, 5));
        writer.set_current_position_clear(branch != 3);
        if (branch == 0) return writer.travel_to_xy(Vec2d(110, 100), "approach", cap);
        if (branch == 1) return writer.travel_to_xyz(Vec3d(110, 100, 5), "approach", false, cap);
        if (branch == 2 || branch == 3)
            return writer.travel_to_xyz(Vec3d(110, 100, 6), "approach", true, cap);
        if (branch == 8) {
            const auto belt = BeltCoordinateSystem::create(45, 250);
            writer.set_belt_coordinate_system(&belt);
            return writer.travel_to_xyz(Vec3d(110, 105, 6), "approach", true, cap);
        }
        writer.lazy_lift(branch == 4 ? LiftType::NormalLift : branch == 5 ?
            LiftType::SlopeLift : LiftType::SpiralLift);
        return writer.travel_to_xyz(Vec3d(140, 100, 5), "approach", false, cap);
    };
    CHECK(move(std::nullopt) == move(0.0));
    const auto gcode = move(60.0);
    INFO(gcode);
    std::istringstream lines(gcode);
    std::string line;
    double feedrate = 0;
    size_t capped_moves = 0;
    const std::regex f("(?:^| )F([0-9.]+)");
    std::smatch match;
    while (std::getline(lines, line)) {
        line = line.substr(0, line.find(';'));
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G2 ", 0) != 0 && line.rfind("G3 ", 0) != 0)
            continue;
        if (std::regex_search(line, match, f)) feedrate = std::stod(match[1]);
        if (line.find('X') != std::string::npos || line.find('Y') != std::string::npos ||
            line.find("G3") == 0 || line.find("G2") == 0) {
            CHECK(feedrate > 0);
            CHECK(feedrate <= 3600);
            ++capped_moves;
        } else if (line.find('Z') != std::string::npos) {
            CHECK(feedrate == Catch::Approx(7200));
        }
    }
    CHECK(capped_moves > 0);
    if (branch <= 2 || branch == 8) {
        const auto slower = move(60.0, 40.0);
        CHECK(slower.find("F2400") != std::string::npos);
        CHECK(slower.find("F3600") == std::string::npos);
    }
}

TEST_CASE("travel caps preserve pure Z and legacy first-layer speed choices", "[ContinuousFiber][fiber-approach][writer]")
{
    PrintConfig config;
    config.travel_speed.value = 350;
    config.travel_speed_z.value = 120;
    config.initial_layer_travel_speed = ConfigOptionFloatOrPercent(40, false);
    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_position(Vec3d(100, 100, 5));
    writer.set_current_position_clear(true);
    CHECK(writer.travel_to_xy(Vec2d(110, 100), "", 60).find("F2400") != std::string::npos);
    // The old final XYZ branch uses travel_speed rather than first-layer speed.
    CHECK(writer.travel_to_xyz(Vec3d(120, 100, 6), "", true, 60).find("F3600") != std::string::npos);
    CHECK(writer.travel_to_xyz(Vec3d(120, 100, 7), "", true, 60).find("F21000") != std::string::npos);
    CHECK(writer.travel_to_z(8, "", true).find("F7200") != std::string::npos);
    CHECK_THROWS(writer.travel_to_xy(Vec2d(130, 100), "", -1));
    CHECK_THROWS(writer.travel_to_xy(Vec2d(140, 100), "", std::numeric_limits<double>::infinity()));
}

TEST_CASE("CFSYS fiber activation establishes zero PA after tool selection", "[ContinuousFiber][fiber-approach][cfsys]")
{
    PrintConfig config;
    config.gcode_flavor.value = gcfKlipper;
    config.single_extruder_multi_material.value = false;
    config.filament_process_type.values = {"thermoplastic", "continuous_fiber"};
    config.filament_map.values = {1, 2};
    config.physical_extruder_map.values = {0, 1};
    config.toolhead_process_capabilities.values = {"thermoplastic", "continuous_fiber"};
    config.toolhead_fiber_protocol_id.values = {"", "cfsys-v1"};
    config.toolhead_fiber_e_units_per_mm.values = {1, 1};
    config.enable_pressure_advance.values = {false, false};
    config.change_filament_gcode.value.clear();
    config.filament_start_gcode.values = {"", ""};
    config.filament_end_gcode.values = {"", ""};
    GCode generator;
    generator.apply_print_config(config);
    generator.writer().set_extruders({0, 1});
    generator.writer().toolchange(0);
    const auto first = generator.set_extruder(1, 0.2);
    INFO(first);
    const auto t1 = first.find("T1");
    const auto pa = first.find("SET_PRESSURE_ADVANCE ADVANCE=0");
    REQUIRE(t1 != std::string::npos);
    REQUIRE(pa != std::string::npos);
    CHECK(t1 < pa);
    CHECK(generator.set_extruder(1, 0.4).empty());
    CHECK(generator.set_extruder(0, 0.4).find("SET_PRESSURE_ADVANCE") == std::string::npos);
    CHECK(generator.set_extruder(1, 0.4).find("SET_PRESSURE_ADVANCE ADVANCE=0") != std::string::npos);

    config.toolhead_fiber_protocol_id.values[1] = "linear-e-v1";
    GCode other_protocol;
    other_protocol.apply_print_config(config);
    other_protocol.writer().set_extruders({0, 1});
    other_protocol.writer().toolchange(0);
    CHECK(other_protocol.set_extruder(1, 0.2).find("SET_PRESSURE_ADVANCE") == std::string::npos);
}

TEST_CASE("default fan configuration preserves all legacy firmware formats", "[ContinuousFiber][cooling][compatibility]")
{
    for (const auto flavor : {gcfMarlinLegacy, gcfKlipper, gcfRepRapFirmware, gcfRepetier,
             gcfMarlinFirmware, gcfRepRapSprinter, gcfTeacup, gcfMakerWare,
             gcfSailfish, gcfMach3, gcfMachinekit, gcfSmoothie, gcfNoExtrusion}) {
        for (const std::string protocol : {"", "linear-e-v1", "cfsys-v1"}) {

            for (const int floor : {-1, 0, 20, 100}) {
                for (const unsigned speed : {0u, 5u, 60u, 100u}) {
                    CAPTURE(flavor, protocol, floor, speed);
                    PrintConfig config;
                    config.gcode_flavor.value = flavor;
                    config.toolhead_fiber_protocol_id.values = {"", protocol};
                    config.part_cooling_fan_min_pwm.value = floor;
                    // This is the unchanged formatter used before the CFSYS fix.
                    const std::string previous = GCodeWriter::set_fan(flavor, speed, unsigned(std::max(0, floor)));
                    CHECK(GCodeWriter::set_fan(config, speed) == previous);
                    GCodeWriter writer;
                    writer.apply_print_config(config);
                    CHECK(writer.set_fan(speed) == previous);
                }
            }
        }
    }
}

TEST_CASE("CFSYS cooling keeps explicit fan channels and material-specific speeds", "[ContinuousFiber][cfsys][cooling]")
{
    PrintConfig config;
    config.gcode_flavor.value = gcfKlipper;
    CHECK(GCodeWriter::set_fan(config, 60).find("M106 S153") == 0);
    config.toolhead_fiber_protocol_id.values = {"", "cfsys-v1"};
    config.part_cooling_fan_index.value = 1;
    config.part_cooling_fan_min_pwm.value = 20;
    CHECK(GCodeWriter::set_fan(config, 0).find("M106 P1 S0") == 0);
    CHECK(GCodeWriter::set_fan(config, 5).find("M106 P1 S51") == 0);
    CHECK(GCodeWriter::set_fan(config, 60).find("M106 P1 S153") == 0);
    CHECK(GCodeWriter::set_additional_fan(100).find("M106 P2 S255") == 0);
    CHECK(GCodeWriter::set_exhaust_fan(100).find("M106 P3 S255") == 0);

    config.part_cooling_fan_min_pwm.value = 0;
    config.filament_diameter.values = {1.75, 0.35};
    config.filament_map.values = {1, 2};
    config.physical_extruder_map.values = {0, 1};
    config.filament_process_type.values = {"thermoplastic", "continuous_fiber"};
    config.toolhead_process_capabilities.values = {"thermoplastic", "continuous_fiber"};
    config.toolhead_fiber_e_units_per_mm.values = {1, 1};
    config.fan_min_speed.values = {5, 100};
    config.fan_max_speed.values = {100, 100};
    config.reduce_fan_stop_start_freq.values = {false, true};
    config.close_fan_the_first_x_layers.values = {3, 3};
    config.full_fan_speed_layer.values = {0, 0};
    config.fan_cooling_layer_time.values = {5, 5};
    config.slow_down_layer_time.values = {3, 3};
    config.additional_cooling_fan_speed.values = {0, 100};
    config.auxiliary_fan.value = true;
    GCode generator;
    generator.apply_print_config(config);
    generator.writer().set_extruders({0, 1});
    generator.writer().toolchange(0);
    generator.writer().set_position(Vec3d(100, 100, 1));
    CoolingBuffer cooling(generator);
    const std::string first_layer = cooling.process_layer("G4 S10\n", 0, true);
    CHECK(first_layer.find("M106 P1 S0") != std::string::npos);
    const std::string switches = cooling.process_layer(
        "G1 F600 ;_EXTRUDE_SET_SPEED\nG1 X101 E1\n;_EXTRUDE_END\nG4 S10\nT1\nG4 S10\nT0\nG4 S10\n", 4, true);
    INFO(switches);
    const auto fiber_fan = switches.find("M106 P1 S255");
    REQUIRE(fiber_fan != std::string::npos);
    CHECK(switches.find("M106 P2 S255") != std::string::npos);
    CHECK(switches.find("M106 P1 S0", fiber_fan) != std::string::npos);
    CHECK(switches.find("M106 P2 S0", fiber_fan) != std::string::npos);
    CHECK(switches.find("M106 S") == std::string::npos);

    // Kick-start regeneration must not drop P1 or rewrite P2/P3 commands.
    FanMover mover(generator.writer(), 0, false, true, false, 0.2f);
    const std::string moved = mover.process_gcode(
        "M106 P1 S0\nG1 X10 F600\nM106 P1 S128\nG1 X20 F600\nM106 P2 S255\nM106 P3 S0\n", true);
    INFO(moved);
    CHECK(moved.find("M106 P1 S255") != std::string::npos);
    CHECK(moved.find("M106 P2 S255") != std::string::npos);
    CHECK(moved.find("M106 P3 S0") != std::string::npos);
    CHECK(moved.find("M106 S") == std::string::npos);
}

TEST_CASE("configured clearance follows physical tools without a fiber protocol", "[machine-gcode][writer]")
{
    PrintConfig config;
    config.filament_diameter.values = {1.75, 1.75, 1.75};
    config.filament_map.values = {1, 1, 2};
    config.physical_extruder_map.values = {0, 1};
    config.toolchange_z_lift.value = 7.5;
    GCodeWriter writer;
    writer.apply_print_config(config);
    writer.set_extruders({0, 1, 2});
    CHECK_FALSE(writer.toolchange_requires_z_lift(0));
    writer.toolchange(0);
    CHECK_FALSE(writer.toolchange_requires_z_lift(0));
    CHECK_FALSE(writer.toolchange_requires_z_lift(1));
    CHECK(writer.toolchange_requires_z_lift(2));
    writer.toolchange(2);
    CHECK(writer.toolchange_requires_z_lift(0));
    writer.config.toolchange_z_lift.value = 0;
    CHECK_FALSE(writer.toolchange_requires_z_lift(0));
}

TEST_CASE("machine output configuration rejects unsupported combinations", "[machine-gcode][config]")
{
    PrintConfig config;
    config.gcode_flavor.value = gcfKlipper;
    config.part_cooling_fan_index.value = 4;
    config.enable_prime_tower.value = false;
    CHECK(validate_machine_gcode_config(config).empty());
    SECTION("firmware") {
        config.gcode_flavor.value = gcfMach3;
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
        CHECK_THROWS(GCodeWriter::set_fan(config, 50));
    }
    SECTION("channels") {
        config.auxiliary_fan.value = true;
        config.part_cooling_fan_index.value = 2;
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
        config.part_cooling_fan_index.value = 3;
        config.support_air_filtration.value = true;
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
    }
    SECTION("lift and custom scripts") {
        config.toolchange_z_lift.value = 7.5;
        CHECK(validate_machine_gcode_config(config).empty());
        config.change_filament_gcode.value = "; comment only\n";
        CHECK(validate_machine_gcode_config(config).empty());
        config.change_filament_gcode.value += "G1 Z10\n";
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
    }
    SECTION("lift limits") {
        config.toolchange_z_lift.value = std::numeric_limits<double>::infinity();
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
        config.toolchange_z_lift.value = -1;
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
        config.toolchange_z_lift.value = 10;
        config.enable_prime_tower.value = true;
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
        config.enable_prime_tower.value = false;
        config.printer_structure.value = psBelt;
        CHECK_FALSE(validate_machine_gcode_config(config).empty());
    }
}

TEST_CASE("explicit fan channels survive postprocessing and preview", "[machine-gcode][cooling]")
{
    const int channel = GENERATE(0, 1, 4);
    PrintConfig config;
    config.gcode_flavor.value = gcfKlipper;
    config.part_cooling_fan_index.value = channel;
    const std::string prefix = "M106 P" + std::to_string(channel);
    CHECK(GCodeWriter::set_fan(config, 0).find(prefix + " S0") == 0);
    CHECK(GCodeWriter::set_fan(config, 50).find(prefix + " S127") == 0);
    GCodeWriter writer;
    writer.apply_print_config(config);
    FanMover mover(writer, 0, false, true, false, 0.2f);
    const std::string moved = mover.process_gcode(prefix + " S0\nG1 X10 F600\n" + prefix +
        " S128\nG1 X20 F600\nM106 P2 S255\nM107 P2\nM107 P" + std::to_string(channel) + "\n", true);
    CHECK(moved.find(prefix + " S255") != std::string::npos);
    CHECK(moved.find("M106 P2 S255") != std::string::npos);
    CHECK(moved.find("M107 P2") != std::string::npos);
    CHECK(moved.find("M106 S") == std::string::npos);

    GCodeProcessor processor;
    processor.apply_config(config);
    processor.initialize_result_moves();
    const auto move_at_fan = [&](const std::string &commands, int x, double expected) {
        processor.process_buffer(commands + "G1 X" + std::to_string(x) + " E" + std::to_string(x) + " F600\n");
        REQUIRE_FALSE(processor.get_result().moves.empty());
        CHECK(processor.get_result().moves.back().fan_speed == Catch::Approx(expected).margin(0.001));
    };
    move_at_fan(prefix + " S153\n", 10, 60);
    move_at_fan("M106 P2 S255\nM107 P2\n", 20, 60);
    move_at_fan("M107 P" + std::to_string(channel) + "\n", 30, 0);
    move_at_fan(prefix + "\n", 40, 100);
}

TEST_CASE("CFSYS device commands are scoped to their established process stages", "[ContinuousFiber][cfsys][scripts]")
{
    const auto protocol = FiberMachineProtocol::Cfsys;
    CHECK_NOTHROW(validate_fiber_cut_event("M400\nS0\nM400\n", protocol));
    CHECK_THROWS(validate_fiber_cut_event("S0", protocol));
    CHECK_THROWS(validate_fiber_cut_event("M400\nS0\nG1 E2\nM400", protocol));
    CHECK_NOTHROW(require_fiber_safe_script("DF1004\nDF1005 S=60\nG28\nT1\nM109 S220 T1\nT0\n", "machine_start_gcode", protocol, true));
    CHECK_NOTHROW(require_fiber_safe_script("PRINT_START mesh_min=0,0\nt_z_offset_calibrate bed_temperature=80\n", "machine_start_gcode", protocol, true));
    CHECK_NOTHROW(require_fiber_safe_script("PRINT_END", "machine_end_gcode", protocol, true));
    CHECK_NOTHROW(require_fiber_safe_script("G92 E0", "before_layer_change_gcode", protocol, true));
    for (const std::string script : {"G1 E1", "G10", "G11", "M221 S95", "CF9999", "PRINT_END", "M104 S200 E1"})
        CHECK_THROWS(require_fiber_safe_script(script, "machine_start_gcode", protocol, true));
    CHECK_THROWS(require_fiber_safe_script("DF1004", "filament_start_gcode", protocol));
    CHECK_THROWS(require_fiber_safe_script("G92 X0 E0", "before_layer_change_gcode", protocol));
    CHECK_THROWS(require_fiber_safe_script("G92 E5", "before_layer_change_gcode", protocol));
    CHECK_THROWS(require_fiber_safe_script(";FIBER_START", "layer_change_gcode", protocol));
}

TEST_CASE("fiber semantic parser separates approach landing feed tail and finish", "[ContinuousFiber][preview]")
{
    FiberGCodeSemanticParser parser;
    REQUIRE(parser.consume("FIBER_BEGIN v=3 occurrence=1 purpose=contour width=0.8 height=0.2 e_units_per_mm=2"));
    CHECK(parser.deposition() == ToolpathDeposition::None);
    parser.consume("FIBER_PREFEED_BEGIN");
    CHECK(parser.deposition() == ToolpathDeposition::None);
    parser.consume("FIBER_PREFEED_END");
    parser.consume("FIBER_LANDING_BEGIN");
    CHECK(parser.deposition() == ToolpathDeposition::ContinuousFiberPassive);
    parser.consume("FIBER_LANDING_END");
    parser.consume("FIBER_START");
    CHECK(parser.deposition() == ToolpathDeposition::ContinuousFiberPowered);
    parser.consume("FIBER_CUT");
    parser.consume("FIBER_TAIL_BEGIN");
    CHECK(parser.deposition() == ToolpathDeposition::ContinuousFiberPassive);
    parser.consume("FIBER_DEPLETED");
    parser.consume("FIBER_FINISH_BEGIN");
    CHECK(parser.deposition() == ToolpathDeposition::None);
    parser.consume("FIBER_FINISH");
    CHECK_THROWS(parser.finish());
    parser.consume("FIBER_END");
    CHECK_NOTHROW(parser.finish());
    CHECK_THROWS(parser.consume("FIBER_TAIL_BEGIN"));
    CHECK_THROWS(parser.consume("FIBER_BEGIN v=2"));
}

TEST_CASE("fiber tail obeys corner limits without rejecting a closed contour", "[ContinuousFiber][cleanup]")
{
    const auto candidate = path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 23;
    config.tail_min_speed_mm_s = 3;
    config.tail_max_speed_mm_s = 10;
    auto id = test_id(); id.parent.purpose = FiberPathPurpose::Contour;
    const auto result = finalize_independent(candidate, {rectangle(0,0,30,30)}, config, id);
    REQUIRE(result.prepared);
    CHECK(result.prepared->passive_tail_length_mm() == Catch::Approx(23));
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(80));
    const auto& tail = result.prepared->spans.back();
    CHECK(tail.edges.back().speed_mm_s == Catch::Approx(6.5));
    for (const auto& edge : tail.edges) {
        CHECK(edge.speed_mm_s >= 3);
        CHECK(edge.speed_mm_s <= 10);
        CHECK(edge.feed_mm_per_xy_mm == 0);
    }
}

TEST_CASE("contour finish is independent of enabled concentric infill", "[ContinuousFiber][cleanup]")
{
    const auto candidate = path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    ContinuousFiberConfig config;
    config.finish_overlap_length_mm = 3;
    config.finish_extension_length_mm = 17;
    config.infill_enabled = true;
    config.infill_pattern = ipConcentric;
    auto id = test_id(); id.parent.purpose = FiberPathPurpose::Contour;
    const auto result = finalize_independent(candidate, {rectangle(0,0,30,30)}, config, id);
    REQUIRE(result.prepared);
    CHECK(result.prepared->finish_strategy == FiberFinishStrategy::LoopOverlap);
    CHECK(result.prepared->spans.back().geometry.points.back() == Point3::new_scale(8,5,0));
}

TEST_CASE("fiber finalization validates only the requested path family", "[ContinuousFiber][cleanup]")
{
    ContinuousFiberConfig config;
    config.infill_max_speed_mm_s = 0;
    config.infill_feed_ratio = std::numeric_limits<double>::quiet_NaN();
    config.finish_extension_length_mm = -1;
    auto id = test_id(); id.parent.purpose = FiberPathPurpose::Contour;
    const auto path = path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    const ExPolygons domain {rectangle(0,0,30,30)};
    REQUIRE(finalize_independent(path, domain, config, id).prepared);
    const auto invalid = finalize_independent(path, domain, config, test_id());
    CHECK(invalid.failure == FiberFinalizationFailure::InvalidParameter);
    CHECK_FALSE(invalid.detail.empty());
}

TEST_CASE("one unavailable contour finish does not abort other candidates", "[ContinuousFiber][cleanup]")
{
    ExtrusionEntityCollection candidates;
    candidates.entities.push_back(new ExtrusionPath(path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}})));
    candidates.entities.push_back(new ExtrusionPath(path_from_points({{5,5},{7,5},{7,7},{5,7},{5,5}})));
    ContinuousFiberConfig config;
    config.finish_overlap_length_mm = 12;
    const auto result = FiberPathValidator::validate({&candidates}, {rectangle(0,0,30,30)}, config,
        FiberPathPurpose::Contour, erContinuousFiberContour, test_id().parent.domain);
    CHECK(result.accepted_count() == 1);
    CHECK(result.rejected_count() == 1);
    CHECK(result.assignments.back().reason == FiberRejectionReason::FinishUnavailable);
    CHECK_FALSE(result.assignments.back().detail.empty());
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("retired fiber edge filters are ignored when reading old configs", "[ContinuousFiber][cleanup]")
{
    for (std::string key : {"fiber_minimum_segment_length", "fiber_maximum_turn_angle", "fiber_contour_rounding_max_reserve"}) {
        CHECK_FALSE(print_config_def.has(key));
        DynamicPrintConfig restored;
        CHECK_NOTHROW(restored.set_deserialize_strict(key, "1"));
        CHECK_FALSE(restored.has(key));
        std::string value = "1";
        PrintConfigDef::handle_legacy(key, value);
        CHECK(key.empty());
    }
}


#include "libslic3r/ContinuousFiber/ContinuousFiberFillStrategy.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <future>

TEST_CASE("contour tangent rounding retains the frozen layer 54 main loop", "[ContinuousFiber][ContourRounding]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/layer54.json");
    nlohmann::json fixture; stream >> fixture;
    const auto& domain=fixture["domains"][0];
    ExPolygons allowed;
    for (const auto& region:domain["centerline_allowed_region"]) {
        ExPolygon polygon;
        for (const auto& p:region["outer"]) polygon.contour.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
        for (const auto& hole:region["holes"]) {
            Polygon h;
            for (const auto& p:hole) h.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            polygon.holes.push_back(std::move(h));
        }
        allowed.push_back(std::move(polygon));
    }
    Polyline3 source;
    for (const auto& p:domain["candidates"][1]["points"])
        source.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>(),coord_t(0));
    for (double radius:{0.1,0.5}) {
        CAPTURE(radius);
        const auto result=ContinuousFiberFillStrategy::round_hole_contour(source,allowed,{radius});
        for (const auto& issue:result.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(result.path);
        CHECK(result.path->points.front()==result.path->points.back());
        CHECK(result.path->length()>source.length()*.9);
        CHECK(diff_pl(Polylines{result.path->to_polyline()},offset_ex(allowed,float(scale_(.0001)))).empty());
        REQUIRE_FALSE(result.arcs.empty());
        double end=0;
        for (const auto& arc:result.arcs) {
            CHECK(arc.radius_mm==Catch::Approx(radius));
            CHECK(arc.begin_mm>=end-1e-6);
            CHECK(arc.end_distance_mm>arc.begin_mm);
            CHECK((arc.start_mm-arc.center_mm).norm()==Catch::Approx(arc.radius_mm).margin(2*SCALING_FACTOR));
            CHECK((arc.end_mm-arc.center_mm).norm()==Catch::Approx(arc.radius_mm).margin(2*SCALING_FACTOR));
            end=arc.end_distance_mm;
        }
        CHECK(end<=Geometry::length_with_arcs_mm(*result.path)+1e-6);
        for (size_t i=0;i<result.arcs.size();++i) {
            const auto& a=result.arcs[i];
            const auto& b=result.arcs[(i+1)%result.arcs.size()];
            const auto tangent=[](const Vec2d& radial,double sweep) -> Vec2d {
                return std::copysign(1.0,sweep)*Vec2d(-radial.y(),radial.x()).normalized();
            };
            const Vec2d outgoing=tangent(a.end_mm-a.center_mm,a.sweep_radians);
            const Vec2d incoming=tangent(b.start_mm-b.center_mm,b.sweep_radians);
            const Vec2d gap=b.start_mm-a.end_mm;
            if (gap.norm()>1e-6) {
                CHECK(outgoing.dot(gap.normalized())==Catch::Approx(1.0).margin(1e-8));
                CHECK(incoming.dot(gap.normalized())==Catch::Approx(1.0).margin(1e-8));
            } else CHECK(outgoing.dot(incoming)==Catch::Approx(1.0).margin(1e-8));
        }
        Polyline3 reversed=source; reversed.reverse();
        const auto reverse_result=ContinuousFiberFillStrategy::round_hole_contour(reversed,allowed,{radius});
        REQUIRE(reverse_result.path);
        CHECK(reverse_result.path->length()==Catch::Approx(result.path->length()).epsilon(1e-8));
        auto restored=*reverse_result.path;restored.reverse();
        CHECK(restored.points==result.path->points);

        ExPolygons original;
        for (const auto& region:domain["original_region"]) {
            ExPolygon poly;
            for(const auto& p:region["outer"]) poly.contour.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            for(const auto& hole:region["holes"]) {
                Polygon h;
                for(const auto& p:hole) h.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
                poly.holes.push_back(std::move(h));
            }
            original.push_back(std::move(poly));
        }
        ExtrusionPath candidate(erContinuousFiberContour,.13,1.f,.13f);
        candidate.polyline=source;
        ContinuousFiberConfig config;
        config.contour_bend_radius_mm=radius;
        config.contour_boundary_clearance_mm=.2;
        config.cut_to_contact_length_mm=23;
        config.z_hop_height_mm = 2;
        config.minimum_effective_length_mm=.5;
        config.finish_overlap_length_mm=23;
        const auto validation=FiberPathValidator::validate({&candidate},original,config,
            FiberPathPurpose::Contour,erContinuousFiberContour,{16,53,0,0});
        for(const auto& assignment:validation.assignments) {
            INFO(fiber_rejection_reason_name(assignment.reason)); INFO(assignment.detail);
            REQUIRE(assignment.prepared);
            CHECK(assignment.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.003));
            CHECK_NOTHROW(assignment.prepared->validate());
        }
        CHECK(validation.accepted_count()==1);
        CHECK(validation.audit_assignments().valid());
    }
}

TEST_CASE("contour rounding has explicit disabled and infeasible outcomes", "[ContinuousFiber][ContourRounding]")
{
    const auto path=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-1,-1,11,11)};
    ContourRoundingOptions disabled_options;
    const auto disabled=ContinuousFiberFillStrategy::round_hole_contour(path.polyline,domain,disabled_options);
    REQUIRE(disabled.path);
    CHECK(disabled.path->points==path.polyline.points);
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(path.polyline,domain,{.5});
    REQUIRE(rounded.path);
    CHECK(rounded.arcs.size()==4);
    for(const auto& arc:rounded.arcs) CHECK(std::abs(arc.sweep_radians)==Catch::Approx(PI/2));
    const auto impossible=ContinuousFiberFillStrategy::round_hole_contour(path.polyline,domain,{30});
    CHECK_FALSE(impossible.path);
    REQUIRE_FALSE(impossible.issues.empty());
    CHECK(path.polyline.points.size()==5);
    // A nonempty container can still offset to an empty domain. Distance
    // queries must report unavailable geometry rather than produce NaNs.
    const auto empty_domain=ContinuousFiberFillStrategy::round_hole_contour(path.polyline,ExPolygons{ExPolygon{}},{.5});
    CHECK_FALSE(empty_domain.path);
    CHECK_FALSE(empty_domain.issues.empty());
}

TEST_CASE("tangent rounding expands towards insufficient supports", "[ContinuousFiber][ContourRounding]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/short_supports.json");
    nlohmann::json fixtures; stream >> fixtures;
    for (const auto& fixture:fixtures) {
        Polyline3 source;
        for (const auto& p:fixture["points"])
            source.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>(),coord_t(0));
        ExPolygons domain;
        for (const auto& region:fixture["domain"]) {
            ExPolygon area;
            for (const auto& p:region["outer"])
                area.contour.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            for (const auto& hole:region["holes"]) {
                Polygon h;
                for (const auto& p:hole) h.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
                area.holes.push_back(std::move(h));
            }
            domain.push_back(std::move(area));
        }
        const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source,domain,{.1});
        REQUIRE(rounded.path);
        CHECK(rounded.path->length()>source.length()*.9);
        CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
        // Changing the cyclic seam cannot change which neighbouring corner is merged.
        source.points.pop_back();
        std::rotate(source.points.begin(),source.points.begin()+source.points.size()/2,source.points.end());
        source.points.push_back(source.points.front());
        const auto reseamed=ContinuousFiberFillStrategy::round_hole_contour(source,domain,{.1});
        REQUIRE(reseamed.path);
        CHECK(reseamed.path->points==rounded.path->points);
    }
}

TEST_CASE("outer contour rounds sharp direction changes including turns below 90 degrees", "[ContinuousFiber][ContourRounding][outer]")
{
    const auto source=path_from_points({{0,0},{10,0},{14,2},{10,4},{0,4},{0,0}});
    const ExPolygons domain{rectangle(-1,-1,15,5)};
    const auto rounded=ContinuousFiberFillStrategy::round_outer_contour(source.polyline,domain,{.3});
    for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
    REQUIRE(rounded.path);
    REQUIRE(rounded.arcs.size()==5);
    CHECK(rounded.path->points.front()==rounded.path->points.back());
    for (const auto& arc:rounded.arcs) CHECK(arc.radius_mm==Catch::Approx(.3));
    CHECK(std::count_if(rounded.arcs.begin(),rounded.arcs.end(),[](const auto& arc) {
        return std::abs(arc.sweep_radians)>PI/2+1e-4;
    })>=1);
    CHECK(unscale<double>(rounded.path->length())>unscale<double>(source.polyline.length())-1.0);
    for (const Point& original:{Point::new_scale(10,0),Point::new_scale(10,4)})
        CHECK(std::none_of(rounded.path->points.begin(),rounded.path->points.end(),[&](const Point3& p) {
            return p.x()==original.x() && p.y()==original.y();
        }));
    const auto square=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const auto rounded_square=ContinuousFiberFillStrategy::round_outer_contour(square.polyline,
        {rectangle(-1,-1,11,11)},{.3});
    for (const auto& issue:rounded_square.issues) INFO(contour_rounding_failure_name(issue.reason));
    REQUIRE(rounded_square.path);
    REQUIRE(rounded_square.arcs.size()==4);
    for (const auto& arc:rounded_square.arcs) {
        CHECK(arc.radius_mm==Catch::Approx(.3));
        CHECK(std::abs(arc.sweep_radians)==Catch::Approx(PI/2));
    }
    const auto obtuse=path_from_points({{10,0},{5,8},{-5,8},{-10,0},{-5,-8},{5,-8},{10,0}});
    const auto rounded_obtuse=ContinuousFiberFillStrategy::round_outer_contour(obtuse.polyline,
        {rectangle(-11,-9,11,9)},{.3});
    REQUIRE(rounded_obtuse.path);
    CHECK(rounded_obtuse.arcs.size()==6);
    for (const auto& arc:rounded_obtuse.arcs)
        CHECK(std::abs(arc.sweep_radians)<PI/2);
    // The original pointed-candidate occupancy case must remain covered: an
    // all-convex ring cannot move its tip across five millimeters of occupied
    // sibling space just because every vertex is eligible for rounding.
    const auto occupied_tip=ContinuousFiberFillStrategy::round_outer_contour(source.polyline,
        {rectangle(-1,-1,9,5)},{.3});
    REQUIRE_FALSE(occupied_tip.path);
    REQUIRE(occupied_tip.issues.size()==1);
    CHECK(occupied_tip.issues.front().reason==ContourRoundingFailure::SourceOutsideCurrentDomain);
    // The inward notch is a fixed (negative) turn, so sibling occupancy must
    // still reject a candidate whose notch has left the available domain.
    const auto notched=path_from_points({{0,0},{10,0},{10,4},{6,4},{6,2},{4,2},{4,4},{0,4},{0,0}});
    const auto occupied=ContinuousFiberFillStrategy::round_outer_contour(notched.polyline,
        {rectangle(-1,-1,5,5)},{.3});
    REQUIRE_FALSE(occupied.path);
    REQUIRE(occupied.issues.size()==1);
    CHECK(occupied.issues.front().reason==ContourRoundingFailure::SourceOutsideCurrentDomain);
    const auto obtuse_source=path_from_points({{0,0},{10,0},{14,2},{16,4},{14,6},{10,8},{0,8},{-2,4},{0,0}});
    ExPolygon edge_occupied=rectangle(-3,-1,17,9);
    Polygon hole=rectangle(11,.4,13,1.6).contour;
    hole.make_clockwise();
    edge_occupied.holes.push_back(hole);
    const auto crossing=ContinuousFiberFillStrategy::round_outer_contour(obtuse_source.polyline,
        {edge_occupied},{.3});
    REQUIRE_FALSE(crossing.path);
    REQUIRE(crossing.issues.size()==1);
    CHECK(crossing.issues.front().reason==ContourRoundingFailure::OutsideDomain);
}

TEST_CASE("oversized outer radius is classified as insufficient space", "[ContinuousFiber][ContourRounding][outer]")
{
    const auto notched=path_from_points({{0,0},{10,0},{10,4},{6,4},{6,2},{4,2},{4,4},{0,4},{0,0}});
    const auto rounded=ContinuousFiberFillStrategy::round_outer_contour(notched.polyline,
        {rectangle(-1,-1,11,5)},{30});
    REQUIRE_FALSE(rounded.path);
    REQUIRE(rounded.issues.size()==1);
    CHECK(rounded.issues.front().reason==ContourRoundingFailure::InsufficientSpace);
}

TEST_CASE("outer triangle rounds when every corner requires treatment", "[ContinuousFiber][ContourRounding][outer]")
{
    const auto triangle=path_from_points({{0,0},{10,0},{5,8.660254},{0,0}});
    const auto rounded=ContinuousFiberFillStrategy::round_outer_contour(triangle.polyline,
        {rectangle(-1,-1,11,10)},{.3});
    for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
    REQUIRE(rounded.path);
    REQUIRE(rounded.arcs.size()==3);
    CHECK(rounded.path->points.front()==rounded.path->points.back());
    for (const auto& arc:rounded.arcs) CHECK(arc.radius_mm==Catch::Approx(.3));
}

TEST_CASE("7xiao exterior rounding retains a closed path at captured failure regions", "[ContinuousFiber][ContourRounding][7xiao_outer]")
{
    const auto name=GENERATE("7xiao_outer_layer120_candidate0.json",
        "7xiao_outer_layer179_candidate1.json");
    INFO(name);
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/"+name);
    REQUIRE(stream.good());
    nlohmann::json fixture;stream>>fixture;
    Polyline3 source;
    for (const auto& point:fixture.at("source"))
        source.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>(),coord_t(0));
    ExPolygons domain;
    for (const auto& rings:fixture.at("domain")) {
        ExPolygon region;
        for (const auto& point:rings[0])
            region.contour.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
        for (size_t i=1;i<rings.size();++i) {
            Polygon hole;
            for (const auto& point:rings[i])
                hole.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
            region.holes.push_back(std::move(hole));
        }
        domain.push_back(std::move(region));
    }
    const double radius=fixture.at("radius").get<double>();
    const auto rounded=ContinuousFiberFillStrategy::round_outer_contour(source,domain,{radius});
    for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
    REQUIRE(rounded.path);
    CHECK(rounded.path->points.front()==rounded.path->points.back());
    CHECK_FALSE(rounded.arcs.empty());
    for (const auto& arc:rounded.arcs) CHECK(arc.radius_mm==Catch::Approx(radius));
    const auto& points=rounded.path->points;
    const auto tangent_at=[&](const ContourArc& arc,bool start) {
        const Vec2d position=start?arc.start_mm:arc.end_mm;
        const Point sampled=Point::new_scale(position.x(),position.y());
        const auto found=std::find_if(points.begin(),points.end()-1,[&](const Point3& p){return p.x()==sampled.x() && p.y()==sampled.y();});
        REQUIRE(found!=points.end()-1);
        const size_t index=size_t(found-points.begin()),count=points.size()-1;
        const auto mm=[](const Point3& p){return Vec2d(unscale<double>(p.x()),unscale<double>(p.y()));};
        const Vec2d chord=start?mm(points[index])-mm(points[(index+count-1)%count]):
            mm(points[(index+1)%count])-mm(points[index]);
        const Vec2d radial=position-arc.center_mm;
        const Vec2d tangent=std::copysign(1.,arc.sweep_radians)*Vec2d(-radial.y(),radial.x());
        REQUIRE(chord.norm()>0);
        CHECK(std::abs(cross2(chord.normalized(),tangent.normalized()))<.05);
    };
    for (const auto& arc:rounded.arcs) {tangent_at(arc,true);tangent_at(arc,false);}
    CHECK(diff_pl(Polylines{rounded.path->to_polyline()},
        offset_ex(domain,scale_(ContourRoundingOptions{}.geometry_tolerance_mm))).empty());
    auto reversed=source;
    reversed.reverse();
    const auto opposite=ContinuousFiberFillStrategy::round_outer_contour(reversed,domain,{radius});
    REQUIRE(opposite.path);
    CHECK(opposite.path->points.front()==opposite.path->points.back());
    for (const auto& arc:opposite.arcs) CHECK(arc.radius_mm==Catch::Approx(radius));
    auto reseamed=source;
    reseamed.points.pop_back();
    std::rotate(reseamed.points.begin(),reseamed.points.begin()+reseamed.points.size()/3,reseamed.points.end());
    reseamed.points.push_back(reseamed.points.front());
    const auto shifted=ContinuousFiberFillStrategy::round_outer_contour(reseamed,domain,{radius});
    REQUIRE(shifted.path);
    CHECK(shifted.path->points.front()==shifted.path->points.back());
}

TEST_CASE("sampled smooth outer arcs are not treated as hard corners", "[ContinuousFiber][ContourRounding][outer]")
{
    Polyline3 circle;
    for (int i=0;i<90;++i) {
        const double angle=2*PI*i/90;
        circle.points.emplace_back(Point::new_scale(2+.3*std::cos(angle),2+.3*std::sin(angle)),0);
    }
    circle.points.push_back(circle.points.front());
    const auto rounded=ContinuousFiberFillStrategy::round_outer_contour(circle,
        {rectangle(1,1,3,3)},{.3});
    for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
    REQUIRE(rounded.path);
    CHECK(rounded.arcs.empty());
    CHECK(rounded.path->points==circle.points);
}

TEST_CASE("short-edge normalization preserves resolvable geometric deviation", "[ContinuousFiber][ContourRounding]")
{
    auto path=path_from_points({{0,0},{10,0},{10.001,0.003},{10.002,0},{20,0}});
    const auto normalized=normalize_fiber_geometry(path.polyline);
    CHECK(std::any_of(normalized.points.begin(),normalized.points.end(),[](const Point3& p){return unscale<double>(p.y())>.0029;}));
    // Every input edge is shorter than command resolution, but the whole bend
    // cannot be replaced with its chord within that resolution.
    Polyline3 dense;
    for (size_t i=0;i<=20;++i) {
        const double angle=(PI/2)*i/20;
        dense.points.emplace_back(coord_t(scale_(.01*std::cos(angle))),coord_t(scale_(.01*std::sin(angle))),coord_t(0));
    }
    const auto cleaned=normalize_fiber_geometry(dense);
    CHECK(cleaned.points.front()==dense.points.front());
    CHECK(cleaned.points.back()==dense.points.back());
    CHECK(cleaned.points.size()>=3);
}

TEST_CASE("concave boundary turns connect locally without moving the whole loop", "[ContinuousFiber][ContourRounding]")
{
    const auto source=path_from_points({{0,0},{10,0},{10,4},{4,4},{4,10},{0,10},{0,0}});
    Points ring=source.polyline.to_polyline().points;ring.pop_back();
    const ExPolygons domain{ExPolygon(Polygon(ring))};
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,domain,{.1});
    REQUIRE(rounded.path);
    CHECK(rounded.arcs.size()>ring.size());
    CHECK(rounded.path->length()>source.length()*.95);
    CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
    // Unaffected straight portions remain on the original supports.
    CHECK(std::count_if(rounded.path->points.begin(),rounded.path->points.end(),[](const auto& p){return p.y()==0;})>=2);
    for(const auto& arc:rounded.arcs) CHECK(arc.radius_mm>=.1-1e-8);
}

TEST_CASE("rounding settings separate contour policies", "[ContinuousFiber][ContourRounding]")
{
    ContinuousFiberConfig first;
    first.contour_enabled=true;
    auto second=first;second.contour_bend_radius_mm=.1;
    CHECK_FALSE(fiber_policy_key(first,0,true)==fiber_policy_key(second,0,true));
}

TEST_CASE("rounded bend speed is independent of arc tessellation", "[ContinuousFiber][ContourRounding][speed]")
{
    const auto source=path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    const ExPolygons domain{rectangle(0,0,30,30)};
    for (double tolerance:{.00001,.00005}) {
        ContourRoundingOptions options{.5};options.chord_tolerance_mm=tolerance;
        auto result=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,domain,options);
        REQUIRE(result.path);
        ExtrusionPath candidate(*result.path,source);
        ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;
        auto id=test_id();id.parent.purpose=FiberPathPurpose::Contour;
        const auto prepared=finalize_independent(candidate,domain,config,id,result.arcs);
        REQUIRE(prepared.prepared);
        double slow=100,fast=0;
        for (const auto& span:prepared.prepared->spans) if (span.actively_feeds_fiber())
            for (const auto& edge:span.edges) {slow=std::min(slow,edge.speed_mm_s);fast=std::max(fast,edge.speed_mm_s);}
        CHECK(slow==Catch::Approx(6.5));
        CHECK(fast==Catch::Approx(10.0));
        CHECK(prepared.prepared->passive_tail_length_mm()==Catch::Approx(23.0).margin(.003));
    }
}


TEST_CASE("frozen contours retain their geometry with local tangent connections", "[ContinuousFiber][ContourRounding][layer61][layer62][layer69]")
{
    const auto file=GENERATE("layer61.json","layer62.json","layer69.json");
    CAPTURE(file);
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/"+file);
    REQUIRE(stream.good());
    nlohmann::json fixture; stream >> fixture;
    const auto& domain=fixture["domains"][0];
    const auto read_regions=[](const auto& input) {
        ExPolygons regions;
        for (const auto& region:input) {
            ExPolygon area;
            for (const auto& p:region["outer"])
                area.contour.points.emplace_back(p[0].template get<coord_t>(),p[1].template get<coord_t>());
            for (const auto& hole:region["holes"]) {
                Polygon h;
                for (const auto& p:hole) h.points.emplace_back(p[0].template get<coord_t>(),p[1].template get<coord_t>());
                area.holes.push_back(std::move(h));
            }
            regions.push_back(std::move(area));
        }
        return regions;
    };
    const auto allowed=read_regions(domain["centerline_allowed_region"]);
    const auto original=read_regions(domain["original_region"]);
    for (const auto& candidate:domain["candidates"]) {
        Polyline3 source;
        for (const auto& p:candidate["points"])
            source.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>(),coord_t(0));
        for (double radius:{.1,.3,.5}) {
            CAPTURE(radius,candidate["source_length_mm"]);
            const auto result=ContinuousFiberFillStrategy::round_hole_contour(source,allowed,{radius});
            std::string failures;
            for(const auto& issue:result.issues) failures += std::string(contour_rounding_failure_name(issue.reason))+" ";
            INFO(failures);
            REQUIRE(result.path);
            REQUIRE_FALSE(result.arcs.empty());
            CHECK(result.issues.empty());
            CHECK(result.path->points.front()==result.path->points.back());
            CHECK(result.path->length()>source.length()*.9);
            CHECK(diff_pl(Polylines{result.path->to_polyline()},offset_ex(allowed,float(scale_(.0001)))).empty());
            double distance=0;
            const auto tangent=[](const Vec2d& radial,double sweep) -> Vec2d {
                return std::copysign(1.0,sweep)*Vec2d(-radial.y(),radial.x()).normalized();
            };
            for (size_t i=0;i<result.arcs.size();++i) {
                const auto& a=result.arcs[i];const auto& b=result.arcs[(i+1)%result.arcs.size()];
                CHECK(a.radius_mm==Catch::Approx(radius));
                CHECK((a.start_mm-a.center_mm).norm()==Catch::Approx(a.radius_mm).margin(2*SCALING_FACTOR));
                CHECK((a.end_mm-a.center_mm).norm()==Catch::Approx(a.radius_mm).margin(2*SCALING_FACTOR));
                CHECK(a.begin_mm>=distance-1e-6);
                CHECK(a.end_distance_mm>a.begin_mm);
                distance=a.end_distance_mm;
                const Vec2d u=tangent(a.end_mm-a.center_mm,a.sweep_radians);
                const Vec2d v=tangent(b.start_mm-b.center_mm,b.sweep_radians);
                const Vec2d gap=b.start_mm-a.end_mm;
                if (gap.norm()>1e-6) {
                    CHECK(u.dot(gap.normalized())==Catch::Approx(1.0).margin(1e-8));
                    CHECK(v.dot(gap.normalized())==Catch::Approx(1.0).margin(1e-8));
                } else CHECK(u.dot(v)==Catch::Approx(1.0).margin(1e-8));
            }
            auto reseamed=source;reseamed.points.pop_back();
            std::rotate(reseamed.points.begin(),reseamed.points.begin()+reseamed.points.size()/2,reseamed.points.end());
            reseamed.points.push_back(reseamed.points.front());
            const auto rotated=ContinuousFiberFillStrategy::round_hole_contour(reseamed,allowed,{radius});
            REQUIRE(rotated.path);
            CHECK(rotated.path->points==result.path->points);
            auto reversed=source;reversed.reverse();
            auto opposite=ContinuousFiberFillStrategy::round_hole_contour(reversed,allowed,{radius});
            REQUIRE(opposite.path);
            opposite.path->reverse();
            CHECK(opposite.path->points==result.path->points);

            for (const auto& arc:result.arcs)
                CHECK(arc.radius_mm==Catch::Approx(radius).margin(1e-8));
            // This source's three nearby corners used to become a 275-degree
            // return with a 3.11 mm detour. A short, tangent connector exists.
            if (std::string(file)=="layer61.json" && radius==.3 &&
                candidate["source_length_mm"].get<double>()>90) {
                double local_length=0;
                size_t local_arcs=0;
                for (const auto& arc:result.arcs)
                    if ((arc.center_mm-Vec2d(199.0,173.9)).norm()<1.0) {
                        local_length+=arc.radius_mm*std::abs(arc.sweep_radians);
                        ++local_arcs;
                        CHECK(std::abs(arc.sweep_radians)<PI);
                    }
                REQUIRE(local_arcs>0);
                CHECK(local_length<1.5);
            }

            ExtrusionPath extrusion(erContinuousFiberContour,.13,1.f,.13f);
            extrusion.polyline=source;
            ContinuousFiberConfig config;
            config.contour_bend_radius_mm=radius;
            config.contour_boundary_clearance_mm=.2;
            config.cut_to_contact_length_mm=23;
            config.z_hop_height_mm = 2;
            config.minimum_effective_length_mm=.5;
            config.finish_overlap_length_mm=23;
            const auto validation=FiberPathValidator::validate({&extrusion},original,config,
                FiberPathPurpose::Contour,erContinuousFiberContour,{16,fixture["internal_layer"].get<size_t>(),0,0});
            REQUIRE(validation.assignments.size()==1);
            const auto& assignment=validation.assignments.front();
            INFO(fiber_rejection_reason_name(assignment.reason));INFO(assignment.detail);
            REQUIRE(assignment.prepared);
            CHECK_NOTHROW(assignment.prepared->validate());
            CHECK(assignment.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.003));
            CHECK(validation.accepted_count()==1);
            // Recheck a finer, differently phased sampling against the original
            // material domain, not just the rounder's centerline budget.
            const auto fine=Geometry::sample_with_arcs(assignment.centerline->polyline,.000005).to_polyline();
            const auto outside=diff_ex(fiber_contour_coverage(fine,.5*extrusion.width),original,ApplySafetyOffset::Yes);
            CHECK(std::abs(area(outside))*SCALING_FACTOR*SCALING_FACTOR<=config.outside_tolerance_mm2);
            CHECK(validation.audit_assignments().valid());
        }
    }
}


TEST_CASE("rounding preserves the hole enclosed by a boundary contour", "[ContinuousFiber][ContourRounding]")
{
    const auto source=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    ExPolygon area=rectangle(-3,-3,13,13);
    Polygon hole=rectangle(0,0,10,10).contour;
    hole.make_clockwise();area.holes.push_back(hole);
    for (bool reverse:{false,true}) {
        auto input=source.polyline;
        if (reverse) input.reverse();
        const auto result=ContinuousFiberFillStrategy::round_hole_contour(input,{area},{.3});
        for (const auto& issue:result.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(result.path);
        CHECK(diff_pl(Polylines{result.path->to_polyline()},offset_ex(ExPolygons{area},float(scale_(.0001)))).empty());
        Polygon rounded(result.path->to_polyline().points);
        CHECK(rounded.contains(Point::new_scale(5,5)));
        CHECK(rounded.is_clockwise()==reverse);
    }
}

TEST_CASE("rounding rejects invalid rings and cannot cross a narrow permitted corridor", "[ContinuousFiber][ContourRounding]")
{
    const auto crossing=path_from_points({{0,0},{10,10},{0,10},{10,0},{0,0}});
    const auto invalid=ContinuousFiberFillStrategy::round_hole_contour(crossing.polyline,{rectangle(-1,-1,11,11)},{.3});
    REQUIRE_FALSE(invalid.path);
    REQUIRE_FALSE(invalid.issues.empty());
    CHECK(invalid.issues.front().reason==ContourRoundingFailure::InvalidInput);
    const auto source=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    ExPolygon narrow=rectangle(-.001,-.001,10.001,10.001);
    Polygon hole=rectangle(.001,.001,9.999,9.999).contour;
    hole.make_clockwise();narrow.holes.push_back(hole);
    const auto impossible=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,{narrow},{.3});
    CHECK_FALSE(impossible.path);
    CHECK_FALSE(impossible.issues.empty());
}


TEST_CASE("parallel supports may connect with a straight line without zero-length arcs", "[ContinuousFiber][ContourRounding]")
{
    const auto source=path_from_points({{0,0},{4,0},{4,.1},{4.1,.1},{4.1,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-1,-1,11,11)};
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,domain,{.3});
    REQUIRE(rounded.path);
    for (const auto& arc:rounded.arcs) {
        CHECK(arc.end_distance_mm>arc.begin_mm);
        CHECK(std::abs(arc.sweep_radians)>0);
    }
    auto subdivided=source.polyline;subdivided.points.clear();
    for(size_t i=1;i<source.polyline.points.size();++i) {
        const auto& a=source.polyline.points[i-1];const auto& b=source.polyline.points[i];
        subdivided.points.push_back(a);
        subdivided.points.emplace_back((a.x()+b.x())/2,(a.y()+b.y())/2,coord_t(0));
    }
    subdivided.points.push_back(source.polyline.points.back());
    const auto dense=ContinuousFiberFillStrategy::round_hole_contour(subdivided,domain,{.3});
    REQUIRE(dense.path);
    CHECK(dense.path->points==rounded.path->points);
}


TEST_CASE("whole model contour windows preserve neighbouring supports", "[ContinuousFiber][ContourRounding][whole-model]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/whole_model_rounding.json");
    REQUIRE(stream.good());
    nlohmann::json fixture;stream>>fixture;
    for (const auto& input:fixture["domains"]) {
        CAPTURE(input["display_layer"]);
        ExPolygons domain;
        for (const auto& region:input["centerline_allowed_region"]) {
            ExPolygon area;
            for (const auto& point:region["outer"])
                area.contour.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
            for (const auto& hole:region["holes"]) {
                Polygon ring;
                for (const auto& point:hole) ring.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
                area.holes.push_back(std::move(ring));
            }
            domain.push_back(std::move(area));
        }
        Polyline3 source;
        for (const auto& point:input["candidates"][0]["points"])
            source.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>(),coord_t(0));
        ContourRoundingOptions options{.3};
        const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source,domain,options);
        for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(rounded.path);
        CHECK(rounded.issues.empty());
        CHECK(rounded.path->points.front()==rounded.path->points.back());
        CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
        if (unscale<double>(source.length())>25.5) CHECK(rounded.path->length()>source.length()*.9);
        for (const auto& arc:rounded.arcs) {
            CHECK(arc.radius_mm==Catch::Approx(.3));
        }
    }
}

TEST_CASE("contour optimizer terminates on a degenerate active set", "[ContinuousFiber][ContourRounding][optimizer]")
{
    // NLopt issue 370: the internal linear solver could cycle past maxeval.
    // https://github.com/stevengj/nlopt/issues/370
    nlopt::opt optimizer(nlopt::LN_COBYLA,2);
    optimizer.set_min_objective([](const std::vector<double>& x,std::vector<double>&,void*) {
        const double value=2.0-std::cos(x[0])+x[1]*x[1];
        return value*value;
    },nullptr);
    optimizer.set_maxeval(668);
    std::vector<double> x{0,0};
    double score=0;
    try { optimizer.optimize(x,score); }
    catch (const nlopt::roundoff_limited&) { /* Explicit numerical failure is allowed. */ }
    CHECK(std::isfinite(x[0]));
    CHECK(std::isfinite(x[1]));
    CHECK(optimizer.get_numevals()<=668);
}

TEST_CASE("long straight runs simplify without losing a following bend", "[ContinuousFiber][ContourRounding]")
{
    Polyline3 source;
    for (size_t i=0;i<=100000;++i)
        source.points.push_back(Point3::new_scale(double(i)*.001,0,0));
    const auto straight=normalize_fiber_geometry(source);
    REQUIRE(straight.points.size()==2);
    CHECK(straight.points.front()==source.points.front());
    CHECK(straight.points.back()==source.points.back());
    source.points.push_back(Point3::new_scale(100.001,.003,0));
    source.points.push_back(Point3::new_scale(100.002,0,0));
    source.points.push_back(Point3::new_scale(110,0,0));
    const auto bent=normalize_fiber_geometry(source);
    CHECK(bent.points.front()==source.points.front());
    CHECK(bent.points.back()==source.points.back());
    CHECK(std::any_of(bent.points.begin(),bent.points.end(),[](const auto& p){return unscale<double>(p.y())>.0029;}));
}

TEST_CASE("coverage diagnostics distinguish original overlap without dropping a valid contour", "[ContinuousFiber][ContourRounding]")
{
    const auto source=path_from_points({{0,0},{10,0},{10,10},{5.3,10},{5.3,20},
        {10,20},{10,30},{0,30},{0,20},{4.7,20},{4.7,10},{0,10},{0,0}});
    ContourRoundingOptions options{.3};
    const auto result=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,{rectangle(-5,-5,15,35)},options);
    REQUIRE(result.path);
    CHECK(result.issues.empty());
    const auto overlap=ContinuousFiberFillStrategy::contour_coverage_overlap(source.polyline,*result.path,1.0);
    CHECK(overlap.first>3.5);
    // Fillets slightly extend overlap near the neck ends; the long pre-existing
    // 0.4 mm strip intersection must not be counted again as newly introduced.
    CHECK(overlap.second>0);
    for (const auto& arc:result.arcs) CHECK(arc.radius_mm==Catch::Approx(.3));
    const auto square=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const auto isolated=ContinuousFiberFillStrategy::round_hole_contour(square.polyline,{rectangle(-5,-5,15,15)},options);
    REQUIRE(isolated.path);
    CHECK(ContinuousFiberFillStrategy::contour_coverage_overlap(square.polyline,*isolated.path,1.0).second==0);
    const auto geometry=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,{rectangle(-5,-5,15,35)},options);
    REQUIRE(geometry.path);
    CHECK(ContinuousFiberFillStrategy::contour_coverage_overlap(source.polyline,*geometry.path,0)==std::make_pair(0.,0.));
}

TEST_CASE("short contour steps do not turn into winding connectors", "[ContinuousFiber][ContourRounding][no-curl]")
{
    const auto file=GENERATE("layer91.json","layer95.json");
    const int pose=GENERATE(0,1,2);
    CAPTURE(file,pose);
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/"+file);
    REQUIRE(stream.good());
    nlohmann::json fixture;stream>>fixture;
    const auto transform=[&](const auto& input) {
        coord_t x=input[0].template get<coord_t>(),y=input[1].template get<coord_t>();
        if (pose==1) return Point(-y,x); // A different seam and direction in machine XY.
        if (pose==2) return Point(x+scale_(10000.),y-scale_(10000.));
        return Point(x,y);
    };
    const auto& input=fixture["domains"][0];
    ExPolygons domain;
    for (const auto& region:input["centerline_allowed_region"]) {
        ExPolygon area;
        for (const auto& point:region["outer"]) area.contour.points.push_back(transform(point));
        for (const auto& hole:region["holes"]) {
            Polygon ring;
            for (const auto& point:hole) ring.points.push_back(transform(point));
            area.holes.push_back(std::move(ring));
        }
        domain.push_back(std::move(area));
    }
    Polyline3 source;
    for (const auto& point:input["candidates"][0]["points"]) {
        const Point p=transform(point);source.points.emplace_back(p.x(),p.y(),coord_t(0));
    }
    ContourRoundingOptions options{.3};
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source,domain,options);
    for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
    REQUIRE(rounded.path);
    CHECK(rounded.issues.empty());
    CHECK(rounded.path->points.front()==rounded.path->points.back());
    CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
    // The faulty connectors increased these loops to 50.52 / 85.07 mm.
    // These limits leave room for equivalent local solutions, but not the curls.
    CHECK(unscale<double>(rounded.path->length())<(std::string(file)=="layer91.json"?45.:84.));
    CHECK(rounded.path->length()>source.length()*.9);
    for (size_t i=0;i<rounded.arcs.size();++i) {
        const auto& a=rounded.arcs[i];const auto& b=rounded.arcs[(i+1)%rounded.arcs.size()];
        CHECK(a.radius_mm==Catch::Approx(.3));
        // Neither source contains a local return requiring a major arc.
        CHECK(std::abs(a.sweep_radians)<PI+1e-6);
        const Vec2d u=std::copysign(1.,a.sweep_radians)*Vec2d(-(a.end_mm-a.center_mm).y(),(a.end_mm-a.center_mm).x()).normalized();
        const Vec2d v=std::copysign(1.,b.sweep_radians)*Vec2d(-(b.start_mm-b.center_mm).y(),(b.start_mm-b.center_mm).x()).normalized();
        const Vec2d gap=b.start_mm-a.end_mm;
        if (gap.norm()>1e-6) {
            CHECK(u.dot(gap.normalized())==Catch::Approx(1.).margin(1e-7));
            CHECK(v.dot(gap.normalized())==Catch::Approx(1.).margin(1e-7));
        } else CHECK(u.dot(v)==Catch::Approx(1.).margin(1e-7));
    }
}


TEST_CASE("contour cycle selection matches exhaustive constrained search", "[ContinuousFiber][ContourRounding][cycle-search]")
{
    using namespace continuous_fiber_detail;
    CHECK(compatible_cycle({}).indices.empty());
    std::mt19937 random(6172);
    for (size_t trial=0;trial<2000;++trial) {
        CAPTURE(trial);
        const size_t n=1+random()%6;
        std::vector<LocalSolutions> pools(n);
        std::vector<size_t> fixed(n,size_t(-1));
        std::vector<std::vector<size_t>> banned(n);
        for (size_t i=0;i<n;++i) {
            const size_t count=1+random()%5;
            for (size_t j=0;j<count;++j)
                pools[i].values.push_back({{},double(random()%9),double(random()%9),double(random()%100)/10});
            if ((trial%4==1 || trial%4==3) && random()%3==0) fixed[i]=random()%count;
            if ((trial%4==2 || trial%4==3) && random()%2==0) banned[i].push_back(random()%count);
        }
        // Also exercise empty pools, including an empty anchor.
        if (trial%50==0) pools[random()%n].values.clear();
        double expected=std::numeric_limits<double>::infinity();
        std::vector<size_t> selected(n);
        const auto enumerate=[&](const auto& self,size_t i)->void {
            if (i<n) {
                for (size_t j=0;j<pools[i].values.size();++j) {
                    if (fixed[i]!=size_t(-1) && fixed[i]!=j) continue;
                    if (std::find(banned[i].begin(),banned[i].end(),j)!=banned[i].end()) continue;
                    selected[i]=j;self(self,i+1);
                }
                return;
            }
            double score=0;
            for (size_t k=0;k<n;++k) {
                const auto& a=pools[k].values[selected[k]];
                const auto& b=pools[(k+1)%n].values[selected[(k+1)%n]];
                if (a.outgoing_consumed>b.incoming_remaining+1e-8) return;
                score+=a.score;
            }
            expected=std::min(expected,score);
        };
        enumerate(enumerate,0);
        const auto actual=trial%4==0?compatible_cycle(pools):compatible_cycle(pools,fixed,banned);
        REQUIRE(actual.indices.empty()==!std::isfinite(expected));
        if (actual.indices.empty()) continue;
        REQUIRE(actual.indices.size()==n);
        for (size_t i=0;i<n;++i) REQUIRE(actual.indices[i]<pools[i].values.size());
        double score=0;
        for (size_t i=0;i<n;++i) {
            const size_t j=actual.indices[i];
            CHECK((fixed[i]==size_t(-1) || fixed[i]==j));
            CHECK(std::find(banned[i].begin(),banned[i].end(),j)==banned[i].end());
            CHECK(pools[i].values[j].outgoing_consumed<=pools[(i+1)%n].values[actual.indices[(i+1)%n]].incoming_remaining+1e-8);
            score+=pools[i].values[j].score;
        }
        CHECK(score==Catch::Approx(expected).margin(1e-8));
        CHECK(actual.score==Catch::Approx(expected).margin(1e-8));
    }
}

TEST_CASE("coverage diagnostics are explicit and independent of path acceptance", "[ContinuousFiber][ContourRounding]")
{
    auto candidate=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    candidate.set_extrusion_role(erContinuousFiberContour);
    candidate.width=1.;
    const ExPolygons domain{rectangle(-5,-5,15,15)};
    ContinuousFiberConfig config;
    config.contour_bend_radius_mm=.3;
    config.minimum_path_length_mm=100.;
    const auto rejected=FiberPathValidator::validate({&candidate},domain,config,
        FiberPathPurpose::Contour,erContinuousFiberContour,{16,53,0,0});
    REQUIRE(rejected.assignments.size()==1);
    const auto& assignment=rejected.assignments.front();
    CHECK(assignment.reason==FiberRejectionReason::ProcessBudgetTooShort);
    config.minimum_path_length_mm=0;
    const auto accepted=FiberPathValidator::validate({&candidate},domain,config,
        FiberPathPurpose::Contour,erContinuousFiberContour,{16,53,0,0});
    REQUIRE(accepted.accepted_count()==1);
    // Computing an optional diagnostic must not mutate the accepted path.
    const auto& rounded=accepted.assignments.front().centerline->polyline;
    const auto points=rounded.points;
    const auto overlap=ContinuousFiberFillStrategy::contour_coverage_overlap(candidate.polyline,rounded,candidate.width);
    CHECK(overlap.first>0);
    CHECK(rounded.points==points);
    CHECK(accepted.accepted_count()==1);
}

TEST_CASE("locally valid contour arcs must pass complete ring validation", "[ContinuousFiber][ContourRounding][improvement]")
{
using namespace continuous_fiber_detail;
    const auto source=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-2,-2,12,12)};
    const ContourRoundingOptions options{.5};
    const auto baseline=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,domain,options);
    REQUIRE(baseline.path);
    REQUIRE(baseline.arcs.size()==4);
    std::vector<TangentSolution> values;
    for (const auto& arc:baseline.arcs) values.push_back({{arc},0,0,1});
    REQUIRE(validate_cycle(values,source.polyline.to_polyline(),domain,domain,options).path);
    std::swap(values[1],values[2]);
    const auto rejected=validate_cycle(values,source.polyline.to_polyline(),domain,domain,options);
    REQUIRE_FALSE(rejected.path);
    CHECK(std::any_of(rejected.issues.begin(),rejected.issues.end(),[](const auto& issue) {
        return issue.reason==ContourRoundingFailure::SelfIntersection;
    }));
}

TEST_CASE("accepted coverage is the batch union of finalized independent paths", "[ContinuousFiber][coverage]")
{
    auto a=path_from_points({{1,2},{18,2}}),b=path_from_points({{1,8},{18,8}});
    const ExPolygons domain{rectangle(0,0,20,10)};
    ContinuousFiberConfig config;
    const auto result=FiberPathValidator::validate({&a,&b},domain,config,
        FiberPathPurpose::Infill,erContinuousFiberInfill,{16,53,0,0});
    REQUIRE(result.accepted_count()==2);
    ExPolygons physical,exclusion;
    for (const auto& assignment:result.assignments) if (assignment.prepared) {
        append(physical,assignment.prepared->physical_coverage);
        append(exclusion,assignment.prepared->resin_exclusion);
        CHECK(assignment.prepared->contour_to_infill_keepout.empty());
    }
    CHECK(result.contour_to_infill_keepout.empty());
    CHECK(diff_ex(union_ex(physical),result.physical_footprint).empty());
    CHECK(diff_ex(result.physical_footprint,union_ex(physical)).empty());
    CHECK(diff_ex(union_ex(exclusion),result.resin_exclusion).empty());
    CHECK(diff_ex(result.resin_exclusion,union_ex(exclusion)).empty());
}

TEST_CASE("partition alternatives survive a preferred partition failing whole-ring validation", "[ContinuousFiber][ContourRounding][partition-search]")
{
    using namespace continuous_fiber_detail;
    const auto source=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-2,-2,12,12)};
    const ContourRoundingOptions options{.5};
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,domain,options);
    REQUIRE(rounded.path);
    std::vector<TangentSolution> values;
    for (const auto& arc:rounded.arcs) values.push_back({{arc},0,0,1});
    const size_t budget=GENERATE(2,3);
    size_t revisions=budget,searches=budget,attempts=0;
    const std::vector<CornerGroup> initial{{0,1},{1,1},{2,1},{3,1}};
    const auto result=search_contour_partitions(initial,source.polyline.to_polyline(),revisions,searches,
        [&](auto groups,const auto& enqueue,size_t queued) {
            ++attempts;--revisions;--searches;
            if (groups.size()==4) {
                enqueue({{0,2},{2,1},{3,1}},{0,0.}); // Locally preferred, globally crossing.
                enqueue({{0,1},{1,2},{3,1}},{0,1.}); // Worse local score, valid whole ring.
                enqueue({{2,1},{3,1},{0,2}},{0,0.}); // Same partition, different cyclic seam.
                auto rotated=initial;std::rotate(rotated.begin(),rotated.begin()+1,rotated.end());
                enqueue(rotated,{0,-1.}); // Must not retry the original partition.
                return ContourRoundingResult{};
            }
            auto proposed=values;
            if (groups.front().count==2) {
                CHECK(queued==1);
                std::swap(proposed[1],proposed[2]);
            }
            return validate_cycle(proposed,source.polyline.to_polyline(),domain,domain,options);
        });
    CHECK(attempts==budget);
    CHECK(revisions==0);
    CHECK(searches==0);
    if (budget==3) {
        REQUIRE(result.path);
        CHECK(result.path->points==rounded.path->points);
        CHECK(result.issues.empty());
    } else {
        REQUIRE_FALSE(result.path);
        CHECK(std::any_of(result.issues.begin(),result.issues.end(),[](const auto& issue) {
            return issue.reason==ContourRoundingFailure::SearchBudgetExceeded;
        }));
    }
}

TEST_CASE("arc sampling exhaustion is distinct from a geometric radius conflict", "[ContinuousFiber][ContourRounding][budget]")
{
    const auto source=path_from_points({{0,0},{200000,0},{200000,200000},{0,200000},{0,0}});
    ContourRoundingOptions options{10000};options.chord_tolerance_mm=1e-9;
    const auto result=ContinuousFiberFillStrategy::round_hole_contour(source.polyline,
        {rectangle(-1,-1,200001,200001)},options);
    REQUIRE_FALSE(result.path);
    REQUIRE(result.issues.size()==1);
    CHECK(result.issues.front().reason==ContourRoundingFailure::SamplingLimit);
}


TEST_CASE("fiber outer candidates are independent of hole selection at every depth", "[ContinuousFiber][hole-contours][priority]")
{
    const int depth = GENERATE(1, 3);
    ExPolygon region = rectangle(0, 0, 100, 80);
    SECTION("no holes") {}
    SECTION("one hole") { region = rectangle_with_hole(0, 0, 100, 80, 15, 15, 30, 30); }
    SECTION("multiple holes") {
        region = rectangle_with_hole(0, 0, 100, 80, 15, 15, 30, 30);
        auto hole = rectangle(60, 40, 80, 60).contour;
        hole.reverse(); region.holes.push_back(hole);
    }
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = depth;

    const auto all = FiberPathValidator::plan_contours({region}, config, {}, true);
    config.contour_include_holes = false;
    const auto outer = FiberPathValidator::plan_contours({region}, config, {}, true);
    REQUIRE(all.audit_lineage()); REQUIRE(outer.audit_lineage());
    REQUIRE(all.nodes.size() == size_t(depth) * (1 + region.holes.size()));
    REQUIRE(outer.nodes.size() == size_t(depth));
    for (size_t i = 0; i < outer.nodes.size(); ++i) {
        REQUIRE(outer.nodes[i].source); REQUIRE(all.nodes[i].source);
        CHECK(outer.nodes[i].source->points == all.nodes[i].source->points);
        CHECK(outer.nodes[i].side == FiberContourSide::Outer);
        CHECK(outer.nodes[i].depth == i);
        REQUIRE(outer.validation.assignments[i].centerline);
        REQUIRE(all.validation.assignments[i].centerline);
        CHECK(outer.validation.assignments[i].centerline->polyline.points ==
              all.validation.assignments[i].centerline->polyline.points);
    }
}

TEST_CASE("hole strands yield to a printable outer strand without moving it", "[ContinuousFiber][priority]")
{
    const ExPolygons region{rectangle_with_hole(0,0,100,80,20,2.06,60,30)};
    ContinuousFiberConfig config;
    config.contour_bend_radius_mm=GENERATE(0.,.3);
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=1;
    config.contour_boundary_clearance_mm=.2;
    config.cut_to_contact_length_mm=23; config.z_hop_height_mm = 2;
    config.minimum_effective_length_mm=.5;
    config.finish_extension_length_mm=23; config.finish_overlap_length_mm=23;
    config.contour_include_holes=false;
    auto first=FiberPathValidator::plan_contours(region,config,{1,11,0,0}).validation;
    REQUIRE(first.accepted_count()==1);
    const auto first_points=first.assignments.front().centerline->polyline.points;
    config.contour_include_holes=true;
    auto result=FiberPathValidator::plan_contours(region,config,{1,11,0,0}).validation;
    REQUIRE(result.accepted_count()==2);
    CHECK(result.assignments.front().centerline->polyline.points==first_points);
    CHECK(result.assignments.front().prepared->finish_strategy==FiberFinishStrategy::LoopOverlap);
    size_t open=0;
    for (const auto& assignment:result.assignments) if (assignment.prepared && assignment.id.parent.job_ordinal>0) {
        REQUIRE(assignment.centerline);
        CHECK_FALSE(assignment.centerline->is_closed());
        CHECK(assignment.centerline->role()==erContinuousFiberContour);
        CHECK(assignment.prepared->finish_strategy==FiberFinishStrategy::TangentExtension);
        CHECK(assignment.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.001));
        CHECK(area(intersection_ex(first.physical_footprint,assignment.prepared->physical_coverage))*SCALING_FACTOR*SCALING_FACTOR<.001);
        ++open;
    }
    CHECK(open==1);
    // Canonicalize the original boundary before generation, independent of seam.
    auto reseamed=region;
    for (auto& polygon:reseamed) {
        std::rotate(polygon.contour.points.begin(),polygon.contour.points.begin()+1,polygon.contour.points.end());
        for (auto& hole:polygon.holes)
            std::rotate(hole.points.begin(),hole.points.begin()+1,hole.points.end());
    }
    CHECK(result.audit_assignments().valid());
    const auto again=FiberPathValidator::plan_contours(reseamed,config,{1,11,0,0}).validation;
    REQUIRE(again.assignments.size()==result.assignments.size());
    for (size_t i=0;i<result.assignments.size();++i) {
        CHECK(again.assignments[i].kind==result.assignments[i].kind);
        REQUIRE(again.assignments[i].centerline);
        CHECK(again.assignments[i].centerline->polyline.points==result.assignments[i].centerline->polyline.points);
    }
}

TEST_CASE("unprintable priority candidates do not reserve fiber space", "[ContinuousFiber][priority]")
{
ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=2;
    config.cut_to_contact_length_mm=23; config.z_hop_height_mm = 2;
    config.minimum_effective_length_mm=.5; config.contour_include_holes=false;
    const ExPolygons region{rectangle(0,0,4,4),rectangle(10,0,60,40)};
    const auto plan=FiberPathValidator::plan_contours(region,config,{});
    REQUIRE(plan.audit_lineage());
    REQUIRE(plan.validation.assignments.front().reason==FiberRejectionReason::ProcessBudgetTooShort);
    CHECK(intersection_ex(plan.validation.physical_footprint,{region.front()}).empty());
    const auto printable=FiberPathValidator::plan_contours({region.back()},config,{});
    CHECK(plan.validation.accepted_count()==printable.validation.accepted_count());
    CHECK(diff_ex(plan.validation.physical_footprint,printable.validation.physical_footprint).empty());
    CHECK(diff_ex(printable.validation.physical_footprint,plan.validation.physical_footprint).empty());
}

TEST_CASE("layer 12 keeps the outside continuous when hole strands are disabled", "[ContinuousFiber][priority][layer12]")
{
    std::ifstream input(std::string(TEST_DATA_DIR)+"/continuous_fiber/layer12_hole_priority.json");
    REQUIRE(input.good()); nlohmann::json fixture; input>>fixture;
    ExPolygons region;
    for (const auto& value:fixture["original"]) {
        ExPolygon polygon;
        for (const auto& p:value["outer"]) polygon.contour.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
        for (const auto& ring:value["holes"]) {
            Polygon hole;for (const auto& p:ring) hole.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            polygon.holes.push_back(std::move(hole));
        }
        region.push_back(std::move(polygon));
    }
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=1;
    config.contour_boundary_clearance_mm=.2;
    config.cut_to_contact_length_mm=23; config.z_hop_height_mm = 2;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;
    config.finish_extension_length_mm=23;
    config.contour_include_holes=false;
    const auto result=FiberPathValidator::plan_contours(region,config,{1,11,0,0}).validation;
    const auto physical_domain=fiber_material_offset(region,-.7);
    INFO(rejection_details(result));REQUIRE(result.accepted_count()==1);
    const auto accepted=std::find_if(result.assignments.begin(),result.assignments.end(),[](const auto& a){return bool(a.prepared);});
    REQUIRE(accepted!=result.assignments.end());
    // The irregular hole closes its exterior passage after inset. Reconstruct
    // that boundary while retaining the outside route around both round bores.
    CHECK(accepted->centerline->is_closed());
    CHECK(accepted->prepared->total_depositing_length_mm()>240);
    for (const auto& span:accepted->prepared->spans) if (span.deposits_fiber())
        CHECK(diff_pl(Polylines{span.geometry.to_polyline()},offset_ex(physical_domain,float(scale_(.0001)))).empty());
    // Both original arrow locations must retain an outside crossing even when
    // the unavailable passage at the other hole changes the closed route.
    for (const auto& cross:std::vector<Line>{
        Line(Point::new_scale(-37.798726734,2.839246625),Point::new_scale(-37.728978,.780079)),
        Line(Point::new_scale(-36.709527227,-19.685857915),Point::new_scale(-36.779009,-17.626554))}) {
        bool hit=false; Point intersection;
        for (const auto& line:accepted->centerline->polyline.to_polyline().lines())
            hit=hit || line.intersection(cross,&intersection);
        CHECK(hit);
    }
    config.contour_include_holes=true;
    const auto with_holes=FiberPathValidator::plan_contours(region,config,{1,11,0,0}).validation;
    const auto same=std::find_if(with_holes.assignments.begin(),with_holes.assignments.end(),[&](const auto& a){return a.id==accepted->id;});
    REQUIRE(same!=with_holes.assignments.end()); REQUIRE(same->prepared);
    CHECK(same->centerline->polyline.points==accepted->centerline->polyline.points);
}

TEST_CASE("layer 107 plans closed nonoverlapping outer loops", "[ContinuousFiber][priority][layer107]")
{
    std::ifstream input(std::string(TEST_DATA_DIR)+"/continuous_fiber/layer107_outer_conflict.json");
    REQUIRE(input.good()); nlohmann::json fixture; input>>fixture;
    ExPolygons region;
    for (const auto& value:fixture["original"]) {
        ExPolygon polygon;
        for (const auto& p:value["outer"]) polygon.contour.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
        for (const auto& ring:value["holes"]) {
            Polygon hole; for (const auto& p:ring) hole.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            polygon.holes.push_back(std::move(hole));
        }
        region.push_back(std::move(polygon));
    }
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=1;
    config.contour_boundary_clearance_mm=.2; config.contour_include_holes=false;
    config.cut_to_contact_length_mm=23; config.z_hop_height_mm = 2;
    config.minimum_effective_length_mm=.5;
    config.finish_overlap_length_mm=23; config.finish_extension_length_mm=23;
    const auto plan=FiberPathValidator::plan_contours(region,config,{1,106,0,0});
    REQUIRE(plan.audit_lineage());
    REQUIRE(plan.nodes.size()>=2);
    const auto& result=plan.validation;
    CHECK(result.audit_assignments().valid());
    INFO(rejection_details(result));REQUIRE(result.assignments.front().prepared);
    ExPolygons occupied;
    for (const auto& assignment:result.assignments) {
        if (!assignment.prepared) continue;
        CHECK(assignment.centerline->is_closed());
        CHECK(assignment.prepared->finish_strategy==FiberFinishStrategy::LoopOverlap);
        CHECK(assignment.prepared->total_depositing_length_mm()>25.5);
        CHECK(std::abs(area(intersection_ex(occupied,assignment.prepared->physical_coverage))) *
            SCALING_FACTOR*SCALING_FACTOR < .001);
        occupied=union_ex(occupied,assignment.prepared->physical_coverage);
    }

}

TEST_CASE("fragments of one hole reference share their depositing occupancy", "[ContinuousFiber][priority][fragment-occupancy]")
{
ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=3;
    config.minimum_path_length_mm=3;
    const auto plan=FiberPathValidator::plan_contours(
        {rectangle_with_hole(0,0,40,30,4.2,10,11,20)},config,{});
    REQUIRE(plan.audit_lineage());
    ExPolygons occupied;
    size_t hole_fragments=0;
    for (const auto& assignment:plan.validation.assignments) if (assignment.prepared) {
        const auto node=std::find_if(plan.nodes.begin(),plan.nodes.end(),[&](const auto& n) {
            return n.id==assignment.id.parent;
        });
        REQUIRE(node!=plan.nodes.end());
        if (node->side==FiberContourSide::Hole) ++hole_fragments;
        CHECK(area(intersection_ex(occupied,assignment.prepared->physical_coverage))*SCALING_FACTOR*SCALING_FACTOR<.001);
        occupied=union_ex(occupied,assignment.prepared->physical_coverage);
    }
    CHECK(hole_fragments>0);
    CHECK(area(diff_ex(occupied,plan.validation.physical_footprint))*SCALING_FACTOR*SCALING_FACTOR<.0001);
    CHECK(area(diff_ex(plan.validation.physical_footprint,occupied))*SCALING_FACTOR*SCALING_FACTOR<.0001);
}

TEST_CASE("original hole identity survives a closed exterior passage", "[ContinuousFiber][priority][source-boundary]")
{
    std::ifstream input(std::string(TEST_DATA_DIR)+"/continuous_fiber/layer55_hole_source.json");
    REQUIRE(input.good()); nlohmann::json fixture; input>>fixture;
    ExPolygons region;
    for (const auto& value:fixture["original"]) {
        ExPolygon polygon;
        for (const auto& p:value["outer"]) polygon.contour.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
        for (const auto& ring:value["holes"]) {
            Polygon hole;for (const auto& p:ring) hole.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            polygon.holes.push_back(std::move(hole));
        }
        region.push_back(std::move(polygon));
    }
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=1;config.contour_boundary_clearance_mm=.2;
    config.contour_bend_radius_mm=GENERATE(0.,.3);
    config.cut_to_contact_length_mm=23;config.z_hop_height_mm = 2;config.minimum_effective_length_mm=.5;
    config.finish_extension_length_mm=23;config.finish_overlap_length_mm=23;
    config.contour_include_holes=false;
    const auto outer=FiberPathValidator::plan_contours(region,config,{1,54,0,0},true);
    REQUIRE(outer.nodes.size()==1);
    const auto& off=outer.validation;
    REQUIRE(off.accepted_count()>0);
    REQUIRE(off.reference_paths.size()==1);
    for(const auto& a:off.assignments) {
        CHECK(a.kind!=FiberAssignmentKind::IntentionalVoid);
        if(a.prepared) {
            CHECK(a.centerline->is_closed());
            CHECK(a.prepared->finish_strategy==FiberFinishStrategy::LoopOverlap);
        }
    }
    CHECK(off.audit_assignments().valid());
    config.contour_include_holes=true;
    const auto all=FiberPathValidator::plan_contours(region,config,{1,54,0,0},true);
    REQUIRE(all.nodes.front().source); REQUIRE(outer.nodes.front().source);
    CHECK(all.nodes.front().source->points==outer.nodes.front().source->points);
    std::set<size_t> hole_ids;
    for (const auto& node:all.nodes) if(node.side==FiberContourSide::Hole) hole_ids.insert(node.boundary_id);
    CHECK(hole_ids.size()==region.front().holes.size());
    const auto& on=all.validation;
    for(const auto& a:off.assignments) if(a.prepared) {
        const auto same=std::find_if(on.assignments.begin(),on.assignments.end(),[&](const auto& b){return b.id==a.id;});
        REQUIRE(same!=on.assignments.end());REQUIRE(same->prepared);
        CHECK(same->centerline->polyline.points==a.centerline->polyline.points);
    }
}

TEST_CASE("contour normalization cannot manufacture an unavailable interval", "[ContinuousFiber][priority][normalization-domain]")
{
    ExPolygon region;
    for (const auto& p : std::vector<Vec2d>{{-1,-.00001},{1.9992,-.00001},{1.9992,-11},
                                          {80,-11},{80,21},{-1,21}})
        region.contour.points.push_back(Point::new_scale(p.x(), p.y()));
    auto source = path_from_points({{0,0},{1.9993,0},{2,-.0007},{70,-.0007},
                                   {70,20},{0,20},{0,0}}, 1.0).polyline;
    if (GENERATE(false, true)) source.reverse();
    REQUIRE(diff_pl(Polylines{source.to_polyline()}, ExPolygons{region}).empty());


    // Test the actual normalization primitive against its physical domain.
    const ExPolygons domain{region};
    const auto normalized=normalize_fiber_geometry(source,&domain);
    CHECK(normalized.points.front()==normalized.points.back());
    CHECK(unscale<double>(normalized.length())==Catch::Approx(unscale<double>(source.length())).margin(.002));
    CHECK(diff_pl(Polylines{normalized.to_polyline()},domain).empty());
}

TEST_CASE("process splits preserve a bend beside the landing boundary", "[ContinuousFiber][priority][command-domain]")
{
    ExPolygon region;
    for (auto p:std::vector<Vec2d>{{-1,0},{1.9993,0},{1.9993,-11},{35,-11},{35,1},{-1,1}})
        region.contour.points.push_back(Point::new_scale(p.x(),p.y()));
    const ExPolygons planned{region};
    const auto small_bend=path_from_points({{0,0},{1.9993,0},{1.9993,-.00002}}).polyline;
    CHECK(normalize_fiber_geometry(small_bend).points.size()==2);
    CHECK(normalize_fiber_geometry(small_bend,&planned).points==small_bend.points);
    auto candidate=path_from_points({{0,0},{1.9993,0},{1.9993,-10},{30,-10}},1.0);
    ContinuousFiberConfig config;
    config.z_hop_height_mm = 2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_extension_length_mm=23;
    auto id=test_id(); id.parent.purpose=FiberPathPurpose::Contour;
    const auto result=finalize_independent(candidate,offset_ex(planned,float(scale_(1))),config,id,{},&planned);
    REQUIRE(result.prepared);
    CHECK_NOTHROW(result.prepared->validate());
    CHECK(result.prepared->entry.contact==Point3::new_scale(1.9993,-.0007,0));
    CHECK(result.prepared->entry.end_station_mm==2);
    CHECK(result.prepared->spans.front().geometry.points.front()==result.prepared->entry.contact);
    CHECK(result.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.002));
    CHECK(result.prepared->total_depositing_length_mm()==Catch::Approx(unscale<double>(candidate.length())).margin(.00001));
    const auto checked=offset_ex(planned,float(scale_(ContourRoundingOptions{}.geometry_tolerance_mm)));
    for (const auto& span:result.prepared->spans) if(span.deposits_fiber())
        CHECK(diff_pl(Polylines{span.geometry.to_polyline()},checked).empty());
}

TEST_CASE("hole contour settings separate fiber domain policies", "[ContinuousFiber][hole-contours]")
{
    ContinuousFiberConfig enabled;
    enabled.contour_enabled = true;
    ContinuousFiberConfig disabled = enabled;
    disabled.contour_include_holes = false;
    const std::set<FiberPolicyKey> policies{fiber_policy_key(enabled, 0, true), fiber_policy_key(disabled, 0, true)};
    CHECK(policies.size() == 2);
}

TEST_CASE("unopened contours can borrow supports on both sides", "[ContinuousFiber][ContourRounding][bilateral]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/unopened_contours.json");
    REQUIRE(stream.good());
    nlohmann::json fixture;stream>>fixture;
    const auto read_regions=[](const auto& input) {
        ExPolygons regions;
        for (const auto& value:input) {
            ExPolygon region;
            for (const auto& point:value["outer"])
                region.contour.points.emplace_back(point[0].template get<coord_t>(),point[1].template get<coord_t>());
            for (const auto& points:value["holes"]) {
                Polygon hole;
                for (const auto& point:points)
                    hole.points.emplace_back(point[0].template get<coord_t>(),point[1].template get<coord_t>());
                region.holes.push_back(std::move(hole));
            }
            regions.push_back(std::move(region));
        }
        return regions;
    };
    for (const auto& input:fixture["domains"]) {
        CAPTURE(input["display_layer"]);
        const auto original=read_regions(input["original_region"]);
        const auto domain=read_regions(input["centerline_allowed_region"]);
        Polyline3 source;
        for (const auto& point:input["candidates"][0]["points"])
            source.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>(),coord_t(0));
        CAPTURE(unscale<double>(source.length()));
        ContourRoundingOptions options{.3};
        const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source,domain,options);
        for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(rounded.path);
        REQUIRE_FALSE(rounded.arcs.empty());
        CHECK(rounded.issues.empty());
        CHECK(rounded.path->points.front()==rounded.path->points.back());
        CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
        for (const auto& arc:rounded.arcs) CHECK(arc.radius_mm==Catch::Approx(.3));
        auto reseamed=source;reseamed.points.pop_back();
        std::rotate(reseamed.points.begin(),reseamed.points.begin()+reseamed.points.size()/2,reseamed.points.end());
        reseamed.points.push_back(reseamed.points.front());
        const auto rotated=ContinuousFiberFillStrategy::round_hole_contour(reseamed,domain,options);
        REQUIRE(rotated.path);
        CHECK(rotated.path->points==rounded.path->points);
        source.reverse();
        auto reversed=ContinuousFiberFillStrategy::round_hole_contour(source,domain,options);
        REQUIRE(reversed.path);reversed.path->reverse();
        CHECK(reversed.path->points==rounded.path->points);
        source.reverse();

        // Exercise acceptance and process finalization against the ORIGINAL area,
        // not just round_contour's centerline-domain checks.
        ExtrusionPath extrusion(erContinuousFiberContour,.13,1.f,.13f);extrusion.polyline=source;
        ContinuousFiberConfig config;
        config.contour_bend_radius_mm=.3;config.contour_boundary_clearance_mm=.2;
        config.cut_to_contact_length_mm=23;config.z_hop_height_mm = 2;
        config.minimum_effective_length_mm=.5;config.finish_overlap_length_mm=23;
        const auto validation=FiberPathValidator::validate({&extrusion},original,config,
            FiberPathPurpose::Contour,erContinuousFiberContour,{16,input["display_layer"].get<size_t>()-1,0,0});
        REQUIRE(validation.assignments.size()==1);
        const auto& assignment=validation.assignments.front();
        INFO(fiber_rejection_reason_name(assignment.reason));INFO(assignment.detail);
        REQUIRE(assignment.prepared);
        CHECK_NOTHROW(assignment.prepared->validate());
        CHECK(validation.accepted_count()==1);
        CHECK(validation.audit_assignments().valid());
    }
}

TEST_CASE("closed outer routing absorbs connected hole groups at every depth", "[ContinuousFiber][closed-outer]")
{
    ExPolygon region=rectangle_with_hole(0,0,100,80,20,.8,50,30);
    auto chained=rectangle(20,30.8,50,60).contour; chained.reverse();
    auto independent=rectangle(70,50,80,60).contour; independent.reverse();
    region.holes.push_back(chained); region.holes.push_back(independent);
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);
    config.contour_count=3;
    config.contour_boundary_clearance_mm=.05;
    config.contour_bend_radius_mm=GENERATE(0.,.3);
    config.contour_include_holes=false;
    config.z_hop_height_mm = 2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;

    const auto plan=FiberPathValidator::plan_contours({region},config,{},true);
    REQUIRE(plan.audit_lineage());
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==3);
    const auto physical=fiber_material_offset({region},-.55);
    for (const auto& node:plan.nodes) {
        REQUIRE(node.source);
        Polygon ring(node.source->points);
        CHECK_FALSE(ring.contains(Point::new_scale(35,45)));
        CHECK(ring.contains(Point::new_scale(75,55)));
    }
    for (const auto& assignment:plan.validation.assignments) {
        REQUIRE(assignment.prepared);
        CHECK(assignment.centerline->is_closed());
        CHECK(assignment.prepared->finish_strategy==FiberFinishStrategy::LoopOverlap);
        for (const auto& span:assignment.prepared->spans) if (span.deposits_fiber())
            CHECK(diff_pl(Polylines{span.geometry.to_polyline()},offset_ex(physical,float(scale_(.0001)))).empty());
    }
    std::reverse(region.holes.begin(),region.holes.end());
    config.contour_include_holes=true;
    const auto reordered=FiberPathValidator::plan_contours({region},config,{},true);
    REQUIRE(reordered.nodes.size()>=plan.nodes.size());
    for (size_t i=0;i<plan.nodes.size();++i) {
        REQUIRE(reordered.nodes[i].source);
        CHECK(reordered.nodes[i].source->points==plan.nodes[i].source->points);
    }
}

TEST_CASE("outer routing stays closed across a passage width threshold", "[ContinuousFiber][closed-outer]")
{
    const double gap=GENERATE(1.08,1.10,1.12);
    const ExPolygons region{rectangle_with_hole(0,0,100,80,20,gap,50,30)};
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=1;
    config.contour_boundary_clearance_mm=.05; config.contour_include_holes=false;
    config.z_hop_height_mm = 2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;

    const auto plan=FiberPathValidator::plan_contours(region,config,{},true);
    REQUIRE(plan.nodes.size()==1);
    REQUIRE(plan.nodes.front().source);
    Polygon ring(plan.nodes.front().source->points);
    CHECK(ring.contains(Point::new_scale(35,15))==(gap>1.1));
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==1);
    CHECK(plan.validation.assignments.front().centerline->is_closed());
    CHECK(plan.audit_lineage());
}

TEST_CASE("closed contour audit rejects broken centerlines and depositing sequences", "[ContinuousFiber][closed-outer]")
{
    const ExPolygons region{rectangle(-2,-2,82,52)};
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=1;
    config.z_hop_height_mm = 2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;
    config.finish_extension_length_mm=23;

    const auto plan=FiberPathValidator::plan_contours(region,config,{});
    const auto& result=plan.validation;
    REQUIRE(plan.audit_lineage());
    INFO(rejection_details(result));REQUIRE(result.accepted_count()==1);
    INFO(rejection_details(result));REQUIRE(result.assignments.front().prepared);
    const auto& expected=result.assignments.front().prepared->physical_coverage;
    CHECK(diff_ex(result.physical_footprint,expected).empty());
    CHECK(diff_ex(expected,result.physical_footprint).empty());

    auto broken=result;
    broken.assignments.front().centerline->polyline.points.pop_back();
    CHECK_FALSE(broken.audit_assignments().valid());
    ExtrusionEntitiesPtr destination;
    CHECK_THROWS(broken.release_to(destination));
    CHECK(destination.empty());

    // A closed reference or a closing finish move must not hide an open
    // depositing sequence after process preparation.
    broken=result;
    auto prepared=std::make_unique<FiberPathDraft>(broken.assignments.front().prepared->draft());
    for (auto span=prepared->spans.rbegin();span!=prepared->spans.rend();++span)
        if (span->deposits_fiber()) { span->geometry.points.back().x()+=scale_(.1); break; }
    REQUIRE(broken.assignments.front().centerline->is_closed());
    CHECK_THROWS(prepared->validate());
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<PreparedFiberPath>);
}

TEST_CASE("outer generation preserves whole loops inside the physical domain", "[ContinuousFiber][closed-outer]")
{
// A narrow passage is removed by generation, rather than passing an
    // impossible whole outer loop to the allocator and committing fragments.
    const ExPolygons region=union_ex(ExPolygons{rectangle(0,0,40,40),rectangle(40,19.8,60,20.2),rectangle(60,0,100,40)});
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=2;
    config.contour_include_holes=false;
    const auto plan=FiberPathValidator::plan_contours(region,config,{});
    REQUIRE(plan.audit_lineage());
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==4);
    const auto physical=fiber_material_offset(region,-.5);
    for (const auto& assignment:plan.validation.assignments) {
        REQUIRE(assignment.prepared);
        CHECK(assignment.centerline->is_closed());
        CHECK(diff_pl(Polylines{assignment.centerline->polyline.to_polyline()},offset_ex(physical,float(scale_(.0001)))).empty());
    }
}

TEST_CASE("progressive contours consume finalized coverage with one boundary clearance", "[ContinuousFiber][progressive]")
{
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 4;
    config.contour_include_holes = false;
    config.contour_boundary_clearance_mm = .05;
    config.contour_bend_radius_mm = GENERATE(0., .3);
    config.z_hop_height_mm = 2;
    config.cut_to_contact_length_mm = 23;
    config.minimum_effective_length_mm = .5;
    config.finish_overlap_length_mm = 23;
    const ExPolygons region{rectangle(0, 0, 60, 40)};
    const auto plan = FiberPathValidator::plan_contours(region, config, {1, 0, 0, 0}, true);
    REQUIRE(plan.audit_lineage());
    CAPTURE(config.contour_bend_radius_mm);
    for (const auto& a : plan.validation.assignments) {
        INFO(fiber_rejection_reason_name(a.reason));
        for (const auto& issue : a.contour_issues) INFO(contour_rounding_failure_name(issue.reason));
    }
    REQUIRE(plan.nodes.size() == 4);
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count() == 4);
    for (size_t i = 0; i < 4; ++i) {
        const auto& node = plan.nodes[i];
        CHECK(node.depth == i);
        REQUIRE(node.source);
        const BoundingBox box(node.source->points);
        CHECK(unscale<double>(box.min.x()) == Catch::Approx(.55 + i).margin(.0002));
        CHECK(unscale<double>(box.max.x()) == Catch::Approx(59.45 - i).margin(.0002));
        if (i) REQUIRE(node.parent == std::optional<FiberCandidateId>(plan.nodes[i - 1].id));
    }
    // A malformed ancestry cannot be hidden behind an accepted flag.
    auto broken = plan;
    broken.nodes.back().parent = broken.nodes.front().id;
    CHECK_FALSE(broken.audit_lineage());
    broken = plan;
    broken.nodes[1].parent.reset();
    CHECK_FALSE(broken.audit_lineage());
    const auto resin = ContinuousFiberFillStrategy::build_resin_area(region, plan.validation.resin_exclusion);
    CHECK(diff_ex(region, offset_ex(union_ex(resin, plan.validation.resin_exclusion),
        2.f * ClipperSafetyOffset)).empty());
}

TEST_CASE("progressive split contours stop only the rejected branch", "[ContinuousFiber][progressive]")
{
    const ExPolygons region = union_ex(ExPolygons{rectangle(0, 10, 10, 20),
        rectangle(10, 13.5, 20, 16.5), rectangle(20, 0, 50, 30)});
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 4;
    config.contour_include_holes = false;
    config.contour_boundary_clearance_mm = .05;
    config.minimum_path_length_mm = 25.5;
    const auto plan = FiberPathValidator::plan_contours(region, config, {1, 0, 0, 0}, true);
    REQUIRE(plan.audit_lineage());
    size_t split_count = 0, fourth_count = 0, rejected_count = 0;
    for (const auto& node : plan.nodes) {
        if (node.depth == 1) ++split_count;
        if (node.depth == 3) ++fourth_count;
        for (const auto& assignment : plan.validation.assignments) if (assignment.id.parent == node.id &&
            assignment.kind == FiberAssignmentKind::Rejected) {
            ++rejected_count;
            INFO(assignment.detail);CHECK(assignment.reason == FiberRejectionReason::ProcessBudgetTooShort);
            CHECK(std::none_of(plan.nodes.begin(), plan.nodes.end(), [&](const auto& child) {
                return child.parent == std::optional<FiberCandidateId>(node.id);
            }));
        }
    }
    CHECK(split_count == 2);
    CHECK(fourth_count == 1);
    CHECK(rejected_count == 1);
}

TEST_CASE("progressive contours preserve holes and debug does not change deposition", "[ContinuousFiber][progressive]")
{
    ExPolygons region{rectangle_with_hole(0, 0, 60, 60, 20, 20, 40, 40), rectangle(80, 0, 110, 30)};
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 2;
    config.contour_include_holes = GENERATE(false, true);
    config.contour_boundary_clearance_mm = .05;
    const auto plan = FiberPathValidator::plan_contours(region, config, {1, 0, 0, 0}, true);
    REQUIRE(plan.audit_lineage());
    std::string outcomes;
    for (const auto& a : plan.validation.assignments)
        outcomes += std::to_string(a.id.parent.job_ordinal) + ":" + fiber_rejection_reason_name(a.reason) +
            ":" + std::to_string(a.source_end_mm-a.source_begin_mm) + " ";
    INFO(outcomes);
    CHECK(plan.validation.accepted_count() == (config.contour_include_holes ? 6 : 4));
    CHECK(intersection_ex(plan.validation.physical_footprint, ExPolygons{rectangle(20, 20, 40, 40)}).empty());
    std::reverse(region.begin(), region.end());
    for (auto& component : region) {
        component.contour.reverse();
        for (auto& hole : component.holes) hole.reverse();
    }
    const auto reordered = FiberPathValidator::plan_contours(region, config, {1, 0, 0, 0});
    REQUIRE(reordered.audit_lineage());
    REQUIRE(reordered.nodes.size() == plan.nodes.size());
    REQUIRE(reordered.validation.assignments.size() == plan.validation.assignments.size());
    for (size_t i = 0; i < plan.nodes.size(); ++i) {
        CHECK_FALSE(reordered.nodes[i].source);
        CHECK(plan.nodes[i].id == reordered.nodes[i].id);
        CHECK(plan.nodes[i].parent == reordered.nodes[i].parent);
        REQUIRE(plan.validation.assignments[i].centerline);
        REQUIRE(reordered.validation.assignments[i].centerline);
        CHECK(plan.validation.assignments[i].centerline->polyline.points ==
            reordered.validation.assignments[i].centerline->polyline.points);
    }
}

TEST_CASE("progressive process rejection leaves material for subsequent fill", "[ContinuousFiber][progressive]")
{
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 4;
    config.contour_include_holes = false;
    config.minimum_path_length_mm = 1000;
    const ExPolygons region{rectangle(0, 0, 40, 30)};
    const auto plan = FiberPathValidator::plan_contours(region, config, {1, 0, 0, 0});
    REQUIRE(plan.audit_lineage());
    REQUIRE(plan.nodes.size() == 1);
    CHECK(plan.validation.accepted_count() == 0);
    CHECK(plan.validation.physical_footprint.empty());
    CHECK(plan.validation.resin_exclusion.empty());
    CHECK(area(ContinuousFiberFillStrategy::build_resin_area(region, plan.validation.resin_exclusion)) == area(region));
    config.contour_count = 0;
    CHECK(FiberPathValidator::plan_contours(region, config, {}).nodes.empty());
    config.contour_count = -1;
    CHECK_THROWS_AS(FiberPathValidator::plan_contours(region, config, {}), std::invalid_argument);
}

TEST_CASE("clipped hole contours are leaves rather than parents", "[ContinuousFiber][progressive]")
{
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 3;
    config.contour_include_holes = true;
    config.contour_boundary_clearance_mm = .05;
    config.minimum_path_length_mm = 3;
    const auto plan = FiberPathValidator::plan_contours(
        {rectangle_with_hole(0, 0, 40, 30, 4.2, 10, 11, 20)}, config, {});
    REQUIRE(plan.audit_lineage());
    size_t leaves = 0;
    for (const auto& stop : plan.stops) if (stop.side == FiberContourSide::Hole && stop.reason == "open_deposition_leaf") {
        ++leaves;
        REQUIRE(stop.parent);
        CHECK(std::none_of(plan.nodes.begin(), plan.nodes.end(), [&](const auto& node) { return node.parent == stop.parent; }));
    }
    CHECK(leaves > 0);
}

TEST_CASE("4xiao fourth contour has three accepted ancestors", "[ContinuousFiber][progressive][4xiao]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR) + "/continuous_fiber/4xiao/layer7_progressive.json");
    nlohmann::json fixture; stream >> fixture;
    ExPolygon region;
    for (const auto& boundary : fixture["boundaries"]) {
        Polygon polygon;
        for (const auto& point : boundary)
            polygon.points.push_back(Point::new_scale(point[0].get<double>(), point[1].get<double>()));
        if (region.contour.points.empty()) {
            polygon.make_counter_clockwise(); region.contour = std::move(polygon);
        } else {
            polygon.make_clockwise(); region.holes.push_back(std::move(polygon));
        }
    }
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 4;
    config.contour_include_holes = false;
    config.contour_boundary_clearance_mm = .05;
    config.z_hop_height_mm = 2;
    config.cut_to_contact_length_mm = 23;
    config.minimum_effective_length_mm = .5;
    config.finish_overlap_length_mm = 23;
    const auto plan = FiberPathValidator::plan_contours({region}, config, {1, 6, 0, 0}, true);
    REQUIRE(plan.audit_lineage());
    std::set<FiberCandidateId> accepted;
    ExPolygons occupied;
    for (const auto& assignment : plan.validation.assignments) {
        INFO(fiber_rejection_reason_name(assignment.reason));
        if (assignment.prepared) {
            accepted.insert(assignment.id.parent);
            // Integer clipping and arc chords may touch within the existing
            // tolerance, but may not create finite-width overlapping strands.
            CHECK(intersection_ex(fiber_material_offset(occupied, -ContourRoundingOptions{}.geometry_tolerance_mm),
                assignment.prepared->physical_coverage).empty());
            occupied = union_ex(occupied, assignment.prepared->physical_coverage);
        } else {
            INFO(assignment.detail);CHECK(assignment.reason == FiberRejectionReason::ProcessBudgetTooShort);
            CHECK(assignment.source_end_mm - assignment.source_begin_mm < 25.5);
        }
    }
    size_t fourth = 0;
    for (const auto& node : plan.nodes) if (node.depth == 3 && accepted.count(node.id)) {
        ++fourth;
        const auto* ancestor = &node;
        for (size_t depth = 3; depth > 0; --depth) {
            REQUIRE(ancestor->parent);
            REQUIRE(accepted.count(*ancestor->parent));
            const auto found = std::find_if(plan.nodes.begin(), plan.nodes.end(),
                [&](const auto& candidate) { return candidate.id == *ancestor->parent; });
            REQUIRE(found != plan.nodes.end());
            CHECK(found->depth == depth - 1);
            ancestor = &*found;
        }
    }
    CHECK(fourth > 0);
}

TEST_CASE("fiber intersection acceleration preserves exact segment decisions", "[ContinuousFiber][progressive]")
{
    std::mt19937 random(7419);
    std::uniform_real_distribution<double> coordinate(0, 40);
    for (size_t trial = 0; trial < 40; ++trial) {
        ExtrusionPath path(erContinuousFiberInfill, .13, 1.f, .13f);
        for (size_t i = 0; i < 24; ++i)
            path.polyline.points.push_back(Point3::new_scale(trial % 2 ? double(i) : coordinate(random), coordinate(random), 0));
        if (trial % 3 == 0) path.polyline.points.push_back(path.polyline.points.front());
        const Polyline source_polyline = path.polyline.to_polyline();
        const auto& points = source_polyline.points;
        REQUIRE(points.size() == path.polyline.points.size());
        bool intersects = false;
        for (size_t first = 0; first + 1 < points.size(); ++first)
            for (size_t second = first + 2; second + 1 < points.size(); ++second) {
                if (first == 0 && second + 2 == points.size() && points.front() == points.back()) continue;
                intersects |= Geometry::segments_intersect(points[first], points[first + 1], points[second], points[second + 1]);
            }
        const auto result = FiberPathValidator::validate({&path}, {rectangle(-10, -10, 60, 60)}, ContinuousFiberConfig{},
            FiberPathPurpose::Infill, erContinuousFiberInfill, {});
        REQUIRE(result.assignments.size() == 1);
        CAPTURE(trial, intersects, points.size(), path.polyline.points.size());
        INFO(result.assignments.front().detail);
        CHECK(result.assignments.front().reason == (intersects ? FiberRejectionReason::SelfIntersection : FiberRejectionReason::None));
    }
}

TEST_CASE("progressive exterior opening distinguishes a neck from a hole passage", "[ContinuousFiber][progressive]")
{
    ContinuousFiberConfig config;
    config.contour_flow = Flow(1.f, .13f, .4f);
    config.contour_count = 1;
    config.contour_include_holes = false;
    config.contour_boundary_clearance_mm = .05;
    // Both sides of the connecting neck would be emitted by one outer loop.
    // It cannot hold two one-millimetre strands, so retain two closed roots.
    const auto neck = union_ex(ExPolygons{rectangle(0, 0, 10, 10), rectangle(10, 4.05, 30, 5.95),
        rectangle(30, 0, 40, 10)});
    const auto split = FiberPathValidator::plan_contours(neck, config, {});
    REQUIRE(split.audit_lineage());
    REQUIRE(split.validation.accepted_count() == 2);
    for (const auto& node : split.nodes) {
        CHECK(node.depth == 0);
        CHECK_FALSE(node.parent);
    }
    // Only the exterior side of this passage is emitted. The independent hole
    // must not participate in the two-sided neck opening.
    const auto passage = FiberPathValidator::plan_contours(
        {rectangle_with_hole(0, 0, 40, 30, 10, 1.4, 25, 20)}, config, {});
    REQUIRE(passage.audit_lineage());
    CHECK(passage.validation.accepted_count() == 1);
}

TEST_CASE("fiber normalization slab preserves full-domain boundary decisions", "[ContinuousFiber][normalization-domain]")
{
    // Near-collinear bends on the material boundary, with unrelated scanbeam
    // events far away in X. The exact full-region clip is the reference.
    for (coord_t deviation = -12; deviation <= 12; ++deviation) {
        if (deviation == 0) continue;
        for (coord_t translation : {coord_t(-30000000), coord_t(0), coord_t(50000000)}) {
            const Point a(0, 100), b(1000, 110 + deviation), c(20000, 300);
            ExPolygon region;
            region.contour.points = {a, b, c, Point(1000000, 300), Point(1000000, -1000000), Point(-1000000, -1000000)};
            for (coord_t y = 95; y <= 305; ++y)
                region.contour.points.emplace_back(-1000000 + (y % 2) * 10, y);
            region.contour.points.insert(region.contour.points.end(),
                {Point(-1000000, 1300), Point(-1000, 1300), Point(-1000, 100)});
            region.contour.make_counter_clockwise();
            const Point shift(translation, -translation);
            region.translate(shift);
            const ExPolygons domain{region};
            const Polyline chord(Points{a + shift, c + shift});
            const bool removable = diff_pl(Polylines{chord}, domain).empty();
            const Polyline3 source(Polyline(Points{a + shift, b + shift, c + shift}));
            const auto normalized = normalize_fiber_geometry(source, &domain);
            CAPTURE(deviation, translation);
            REQUIRE(normalized.points.size() == (removable ? 2 : 3));
            CHECK(normalized.points.front() == source.points.front());
            CHECK(normalized.points.back() == source.points.back());
        }
    }
}

TEST_CASE("fiber straight fill builds fixed returns without changing the scan grid", "[ContinuousFiber][RoundedInfill]")
{
    for (const double radius : {.2, .5}) for (const float width : {.6f, 1.f}) {
        ContinuousFiberConfig config;
        config.infill_flow=Flow(width,.13f,.4f);
        config.infill_density=double(width)*100; // Exactly 1 mm pitch.
        config.infill_bend_radius_mm=radius;
        const ExPolygons material{rectangle(0,0,10,40)};
        const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point::new_scale(.5,0));
        REQUIRE(generated.paths.size()==1);
        const auto& path=generated.paths.front();
        REQUIRE(path.arcs.size()>=16);
        for (const auto& arc:path.arcs) {
            CHECK(arc.radius_mm==Catch::Approx(radius));
            CHECK((arc.start_mm-arc.center_mm).norm()==Catch::Approx(radius));
            CHECK((arc.end_mm-arc.center_mm).norm()==Catch::Approx(radius));
            CHECK(std::abs(arc.sweep_radians)==Catch::Approx(PI/2));
            CHECK(arc.begin_mm<arc.end_distance_mm);
        }
        const auto points=path.geometry.to_polyline().points;
        for (size_t i=1;i<points.size();++i)
            if (std::abs(unscale<double>(points[i].y()-points[i-1].y()))>2)
                CHECK(points[i].x()==points[i-1].x());
        for (size_t i=0;i+2<path.arcs.size();i+=2)
            CHECK(std::abs(path.arcs[i+2].start_mm.x()-path.arcs[i].start_mm.x())==Catch::Approx(1.).margin(.00001));
        const auto again=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point::new_scale(.5,0));
        CHECK(again.paths.front().geometry.points==path.geometry.points);
    }
}

TEST_CASE("fiber corner stabilization excludes the complete optimized return", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);
    config.infill_density=100;
    config.infill_bend_radius_mm=.2;
    config.corner_stabilization_length_mm=5;
    const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(
        {rectangle(0,0,20,10)},config,0,Point(0,0));
    REQUIRE(generated.paths.size()==1);
    const auto& arcs=generated.paths.front().arcs;
    REQUIRE(arcs.size()>=4);
    REQUIRE(arcs.size()%2==0);
    for (size_t i=0;i<arcs.size();i+=2) {
        const double preceding=i==0?arcs[i].begin_mm:arcs[i].begin_mm-arcs[i-1].end_distance_mm;
        CHECK(preceding>=Catch::Approx(5).margin(.00001));
        // The small bridge between the two quarter-circles belongs to this
        // one return and cannot be misread as a separate stabilization span.
        CHECK(arcs[i+1].begin_mm-arcs[i].end_distance_mm<5);
    }
}

TEST_CASE("fiber infill angles cycle on scheduled fiber layers", "[ContinuousFiber][FiberAngles]")
{
    const auto angles=parse_fiber_infill_angle_sequence(" 0, 45,90,135 ");
    const std::vector<double> expected{0,45,90,135};
    REQUIRE(angles==expected);
    CHECK_THROWS_AS(parse_fiber_infill_angle_sequence(" \t "),std::invalid_argument);
    for (size_t fiber_layer=0;fiber_layer<9;++fiber_layer) {
        const auto selected=fiber_infill_angle_for_layer("0,45,90,135",fiber_layer*2,2);
        REQUIRE(selected);
        CHECK(*selected==Catch::Approx(angles[fiber_layer%angles.size()]*PI/180));
    }
    const auto equivalent=fiber_infill_angle_for_layer("180,225",1,1);
    REQUIRE(equivalent);
    CHECK(*equivalent==Catch::Approx(PI/4));
    const auto standard_first=fiber_infill_angle_for_layer("45,135",0,1);
    const auto standard_second=fiber_infill_angle_for_layer("45,135",1,1);
    REQUIRE(standard_first);
    REQUIRE(standard_second);
    CHECK(*standard_first==Catch::Approx(PI/4));
    CHECK(*standard_second==Catch::Approx(3*PI/4));
    for (const char* invalid:{"0,",",45","0,,90","45deg","nan","inf","-1","360"})
        CHECK_THROWS_AS(parse_fiber_infill_angle_sequence(invalid),std::invalid_argument);
    CHECK_THROWS_AS(fiber_infill_angle_for_layer("0,90",0,0),std::invalid_argument);
}

TEST_CASE("short pre-return spans disconnect but preserve every fiber scan", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);
    config.infill_density=100;
    config.infill_bend_radius_mm=.5;
    const ExPolygons material{rectangle(0,0,20,4)};
    const auto original=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE(original.paths.size()==1);
    REQUIRE(original.paths.front().arcs.size()>=4);
    const size_t scan_count=original.paths.front().arcs.size()/2+1;
    config.corner_stabilization_length_mm=5;
    const auto disconnected=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE(disconnected.paths.size()==scan_count);
    for (const auto& path:disconnected.paths) {
        CHECK(path.arcs.empty());
        CHECK(path.geometry.points.size()==2);
        CHECK(unscale<double>(path.geometry.length())==Catch::Approx(3).margin(.00001));
    }
    config.infill_bend_radius_mm=0;
    const auto no_rounding=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    config.corner_stabilization_length_mm=0;
    const auto no_constraint=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE(no_rounding.paths.size()==no_constraint.paths.size());
    for (size_t i=0;i<no_rounding.paths.size();++i)
        CHECK(no_rounding.paths[i].geometry.points==no_constraint.paths[i].geometry.points);
}

TEST_CASE("stabilized returns respect holes and rotated scan domains", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(.8f,.13f,.4f);
    config.infill_density=80;
    config.infill_bend_radius_mm=.5;
    config.corner_stabilization_length_mm=3;
    const std::vector<ExPolygons> materials{
        {rectangle_with_hole(0,0,12,40,4,10,8,30)},
        diff_ex(ExPolygons{rectangle(0,0,12,40)},ExPolygons{rectangle(5,28,7,41)})};
    for (const auto& material:materials) for (double angle:{0.,.37}) {
        const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(material,config,angle,Point(0,0));
        REQUIRE_FALSE(generated.paths.empty());
        const auto domain=offset_ex(generated.centerline_domain,scale_(.0001));
        for (const auto& path:generated.paths) {
            CHECK(diff_pl(Polylines{path.geometry.to_polyline()},domain).empty());
            REQUIRE(path.arcs.size()%2==0);
            for (size_t i=0;i<path.arcs.size();i+=2) {
                const double gap=i==0?path.arcs[i].begin_mm:
                    path.arcs[i].begin_mm-path.arcs[i-1].end_distance_mm;
                CHECK(gap>=3);
            }
        }
    }
}

TEST_CASE("first optimized return uses the deposited path length threshold", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);
    config.infill_density=100;
    config.infill_bend_radius_mm=.5;
    const ExPolygons material{rectangle(0,0,2.4,12)};
    const auto baseline=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE(baseline.paths.size()==1);
    REQUIRE(baseline.paths.front().arcs.size()==2);
    const double lead_in=baseline.paths.front().arcs.front().begin_mm;
    REQUIRE(lead_in>.01);
    config.corner_stabilization_length_mm=lead_in-.001;
    const auto accepted=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE(accepted.paths.size()==1);
    CHECK(accepted.paths.front().arcs.size()==2);
    config.corner_stabilization_length_mm=lead_in+.001;
    const auto split=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE(split.paths.size()==2);
    for (const auto& path:split.paths) CHECK(path.arcs.empty());
}

TEST_CASE("half circle fiber return fits where its complete disk cannot", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(.2f,.13f,.4f);config.infill_density=20;config.infill_bend_radius_mm=.5;
    const auto generated=ContinuousFiberFillStrategy::generate_rectilinear({rectangle(0,0,2,.8)},config,0,Point::new_scale(.5,0));
    REQUIRE(generated.paths.size()==1);REQUIRE(generated.paths.front().arcs.size()==2);
    CHECK(generated.paths.front().arcs.front().center_mm.y()==Catch::Approx(.2).margin(.00001));
}

TEST_CASE("sloped wall contact determines the furthest tangent support analytically", "[ContinuousFiber][RoundedInfill]")
{
    Polygon polygon;
    for(const auto& p:std::vector<Vec2d>{{0,0},{12,0},{12,28},{0,40}}) polygon.points.push_back(Point::new_scale(p.x(),p.y()));
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=.5;
    const auto generated=ContinuousFiberFillStrategy::generate_rectilinear({ExPolygon(polygon)},config,0,Point(0,0));
    REQUIRE(generated.paths.size()==1);REQUIRE_FALSE(generated.paths.front().arcs.empty());
    const auto& arcs=generated.paths.front().arcs;
    const auto found=std::find_if(arcs.begin(),arcs.end(),[](const auto& arc) { return arc.center_mm.y()>20; });
    REQUIRE(found!=arcs.end());
    const auto& arc=*found;
    const double expected=40-std::sqrt(2.)*.5-arc.center_mm.x()-std::sqrt(2.)*.5;
    CHECK(arc.center_mm.y()==Catch::Approx(expected).margin(.00001));
}

TEST_CASE("rounded fiber routing respects holes concavities rotations and short supports", "[ContinuousFiber][RoundedInfill]")
{
    const std::vector<ExPolygons> materials{
        {rectangle_with_hole(0,0,12,40,4,10,8,30)},
        diff_ex(ExPolygons{rectangle(0,0,12,40)},ExPolygons{rectangle(5,28,7,41)}),
        {rectangle(0,0,12,1.2)},
        {rectangle(0,0,1,1)}};
    ContinuousFiberConfig config;
    config.infill_flow=Flow(.8f,.13f,.4f);config.infill_density=80;config.infill_bend_radius_mm=.5;
    for (size_t shape=0;shape<materials.size();++shape) for (const double angle:{0.,.37}) {
        CAPTURE(shape,angle);
        const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(materials[shape],config,angle,Point(0,0));
        const auto safe=offset_ex(generated.centerline_domain,scale_(.0001));
        for (const auto& path:generated.paths) {
            CHECK(diff_pl(Polylines{path.geometry.to_polyline()},safe).empty());
            for (const auto& arc:path.arcs) CHECK(arc.radius_mm==Catch::Approx(.5));
            const auto lines=path.geometry.to_polyline().lines();
            bool crossing=false;
            for (size_t i=0;i<lines.size() && !crossing;++i) for (size_t j=i+2;j<lines.size();++j) {
                Point intersection;
                if (lines[i].intersection(lines[j],&intersection)) {crossing=true;break;}
            }
            CHECK_FALSE(crossing);
        }
    }
}

TEST_CASE("incompatible rounded fiber spacing is an explicit parameter error", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=.6;
    const ExPolygons material{rectangle(0,0,20,40)};
    CHECK_THROWS_AS(ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0)),std::invalid_argument);
    config.infill_density=50;
    const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    REQUIRE_FALSE(generated.paths.empty());
    for (const auto& path:generated.paths) for (const auto& arc:path.arcs) CHECK(arc.radius_mm==Catch::Approx(.6));
}

TEST_CASE("rounded fiber infill retains its geometry through landing cutting and tail deposition", "[ContinuousFiber][RoundedInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=.5;
    config.z_hop_height_mm = 2;config.cut_to_contact_length_mm=23;config.minimum_effective_length_mm=.5;
    config.finish_extension_length_mm=23;config.contour_boundary_clearance_mm=5;
    const ExPolygons material{rectangle(0,0,10,40)};
    const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    const auto validated=FiberPathValidator::validate_infill(generated,material,config,{1,0,0,0});
    REQUIRE(validated.accepted_count()==1);CHECK(validated.audit_assignments().valid());
    const auto& assignment=validated.assignments.front();REQUIRE(assignment.prepared);
    CHECK(assignment.prepared->total_depositing_length_mm()==Catch::Approx(unscale<double>(generated.paths.front().geometry.length())).margin(.002));
    CHECK(assignment.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.001));
    CHECK(validated.contour_to_infill_keepout.empty());
    auto broken=generated;
    broken.paths.front().geometry.points.back().x()+=scale_(100.);
    CHECK_THROWS(FiberPathValidator::validate_infill(broken,material,config,{1,0,0,0}));
}

TEST_CASE("fiber routing grows both ends through a split scan region", "[ContinuousFiber][RoundedInfill]")
{
    const ExPolygons material=diff_ex(ExPolygons{rectangle(0,0,3,30)},ExPolygons{rectangle(1.4,10,4,20)});
    ContinuousFiberConfig config;
    config.infill_flow=Flow(.2f,.13f,.4f);config.infill_density=20;config.infill_bend_radius_mm=.5;
    const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point::new_scale(.5,0));
    REQUIRE(generated.paths.size()==1);
    CHECK(generated.paths.front().arcs.size()==8);
    CHECK(unscale<double>(generated.paths.front().geometry.length())>60);
    CHECK(diff_pl(Polylines{generated.paths.front().geometry.to_polyline()},
        offset_ex(generated.centerline_domain,scale_(.0001))).empty());
}

TEST_CASE("right-angle exterior corners are rounded when the radius fits", "[ContinuousFiber][ContourRounding][progressive]")
{
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=4;
    config.contour_boundary_clearance_mm=0;
    config.contour_include_holes=false;config.contour_bend_radius_mm=.3;
    const ExPolygons domain{rectangle_with_hole(-.52,-.52,10.52,10.52,.52,.52,9.48,9.48)};
    const auto rounded=FiberPathValidator::plan_contours(domain,config,{1,6,0,0});
    CHECK(rounded.validation.accepted_count()==rounded.nodes.size());
    config.contour_bend_radius_mm=0;
    const auto unrounded=FiberPathValidator::plan_contours(domain,config,{1,6,0,0});
    CHECK(rounded.validation.accepted_count()==unrounded.validation.accepted_count());
}

TEST_CASE("oversized radius rejects a right-angle outer contour", "[ContinuousFiber][ContourRounding][RoundingSpace]")
{
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=4;
    config.contour_include_holes=false;config.contour_bend_radius_mm=30;
    const ExPolygons domain{rectangle(0,0,10,10)};
    const auto square=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const auto impossible=ContinuousFiberFillStrategy::round_outer_contour(square.polyline,domain,{30});
    REQUIRE_FALSE(impossible.path);
    REQUIRE(impossible.issues.size()==1);
    CHECK(impossible.issues.front().reason==ContourRoundingFailure::InsufficientSpace);
    const auto plan=FiberPathValidator::plan_contours(domain,config,{1,6,0,0});
    CHECK(plan.nodes.size()<4);
    CHECK(plan.validation.accepted_count()==0);
    CHECK(plan.validation.rejected_count()>0);
    CHECK(plan.audit_lineage());
    CHECK(plan.validation.physical_footprint.empty());
    CHECK(plan.validation.resin_exclusion.empty());
}

TEST_CASE("contour process width cannot change the configured radius", "[ContinuousFiber][ContourRounding][progressive]")
{
    ExPolygon pointed;
    pointed.contour.points={Point::new_scale(0,0),Point::new_scale(40,0),
        Point::new_scale(65,15),Point::new_scale(40,30),Point::new_scale(0,30)};
    for (float width:{.6f,1.f}) for (double radius:{.1,.3,.7}) {
        const auto candidates=ContinuousFiberFillStrategy::generate_contour_level({pointed},width);
        REQUIRE(candidates.paths.size()==1);
        const auto& candidate=candidates.paths.front();
        const auto result=ContinuousFiberFillStrategy::round_outer_contour(candidate.geometry,
            candidates.geometry_domains.at(candidate.geometry_domain_id),{radius});
        for (const auto& issue:result.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(result.path);
        REQUIRE(result.arcs.size()==5);
        for (const auto& arc:result.arcs) CHECK(arc.radius_mm==Catch::Approx(radius));
    }
}

TEST_CASE("rounded coverage integer spurs do not create artificial half turns", "[ContinuousFiber][ContourRounding][progressive]")
{
    const auto fixture_name=GENERATE("rounded_coverage_spur.json","7xiao_rounding_support.json");
    CAPTURE(fixture_name);
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/"+fixture_name);
    REQUIRE(stream.good());nlohmann::json fixture;stream>>fixture;
    Polyline3 source;
    for(const auto& point:fixture["source"])
        source.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>(),coord_t(0));
    ExPolygons domain;
    for(const auto& rings:fixture["domain"]) {
        ExPolygon region;
        for(const auto& point:rings[0])
            region.contour.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
        for(size_t i=1;i<rings.size();++i) {
            Polygon hole;
            for(const auto& point:rings[i]) hole.points.emplace_back(point[0].get<coord_t>(),point[1].get<coord_t>());
            region.holes.push_back(std::move(hole));
        }
        domain.push_back(std::move(region));
    }
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source,domain,{.3});
    REQUIRE(rounded.path);
    CHECK(rounded.path->points.front()==rounded.path->points.back());
    for(const auto& arc:rounded.arcs) CHECK(arc.radius_mm==Catch::Approx(.3));
    CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,scale_(.0001))).empty());
    source.reverse();
    auto reverse=ContinuousFiberFillStrategy::round_hole_contour(source,domain,{.3});
    REQUIRE(reverse.path);reverse.path->reverse();
    CHECK(reverse.path->points==rounded.path->points);
}


TEST_CASE("zero radius fiber returns reach the material boundary without extra retreat", "[ContinuousFiber][BoundaryInfill]")
{
    for (const float width : {.6f, 1.f}) for (const double density : {40., 100.}) {
        CAPTURE(width, density);
        ContinuousFiberConfig config;
        config.infill_flow=Flow(width,.13f,.4f);config.infill_density=density;config.infill_bend_radius_mm=0;
        const ExPolygons material{rectangle(0,0,12,40)};
        const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
        REQUIRE(generated.paths.size()==1);
        const auto& path=generated.paths.front();
        CHECK(path.arcs.empty());
        const double half=double(width)/2;
        size_t returns=0, scans=0;
        for (const auto& line:path.geometry.to_polyline().lines()) {
            if (line.a.x()==line.b.x()) {
                ++scans;
                CHECK(unscale<double>(std::min(line.a.y(),line.b.y()))==Catch::Approx(half).margin(.000002));
                CHECK(unscale<double>(std::max(line.a.y(),line.b.y()))==Catch::Approx(40-half).margin(.000002));
            } else {
                ++returns;
                CHECK(line.a.y()==line.b.y());
                CHECK(unscale<double>(std::abs(line.a.x()-line.b.x()))==Catch::Approx(width*100/density).margin(.000002));
                const double y=unscale<double>(line.a.y());
                CHECK(std::min(std::abs(y-half),std::abs(y-(40-half)))<.000002);
            }
        }
        CHECK(scans>3);CHECK(returns+1==scans);
        CHECK(ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0)).paths.front().geometry.points==path.geometry.points);
    }
}

TEST_CASE("zero radius fiber preserves all scan intervals around holes and concavities", "[ContinuousFiber][BoundaryInfill]")
{
    Polygon sloped;
    for (const Vec2d& p:std::vector<Vec2d>{{0,0},{12,0},{12,28},{0,40}}) sloped.points.push_back(Point::new_scale(p.x(),p.y()));
    const std::vector<ExPolygons> materials{
        {ExPolygon(sloped)},
        {rectangle_with_hole(0,0,12,40,4,10,8,30)},
        diff_ex(ExPolygons{rectangle(0,0,12,40)},ExPolygons{rectangle(5,28,7,41)}),
        {rectangle(0,0,12,40),rectangle(20,1,30,39)}};
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_bend_radius_mm=0;
    for (size_t shape=0;shape<materials.size();++shape) for (double angle:{0.,.37,PI/2})
        for (double density:{40.,100.}) for(double phase:{0.,-.00005}) {
        CAPTURE(shape,angle,density,phase);config.infill_density=density;
        const auto generated=ContinuousFiberFillStrategy::generate_rectilinear(materials[shape],config,angle,Point::new_scale(phase,0));
        REQUIRE_FALSE(generated.paths.empty());
        // Independently clip a fixed scan grid against the half-width material
        // offset. Compare whole intervals, not just the generated endpoints.
        auto reference=fiber_material_offset(materials[shape],-.5);
        Eigen::Matrix2d frame;frame<<-std::sin(angle),std::cos(angle),std::cos(angle),std::sin(angle);
        for (auto& region:reference) {
            auto transform=[&](Polygon& ring) { for(auto& p:ring.points)p=Point::new_scale((frame*p.cast<double>()*SCALING_FACTOR).x(),(frame*p.cast<double>()*SCALING_FACTOR).y());ring.reverse(); };
            transform(region.contour);for(auto& hole:region.holes)transform(hole);
        }
        const auto bounds=get_extents(reference);const coord_t pitch=scale_(100/density);
        const coord_t origin=scale_(std::cos(angle)*phase);
        coord_t y=origin+coord_t(std::floor(double(bounds.min.y()-origin)/pitch))*pitch;
        if(density==100)y+=(pitch+coord_t(SCALED_EPSILON))/2;
        Polylines grid;
        for(;y<=bounds.max.y();y+=pitch)grid.emplace_back(Points{{bounds.min.x()-1,y},{bounds.max.x()+1,y}});
        const auto scans=intersection_pl(grid,reference);
        REQUIRE_FALSE(scans.empty());
        Lines actual;
        for(const auto& path:generated.paths) {
            CHECK(path.arcs.empty());
            CHECK(diff_pl(Polylines{path.geometry.to_polyline()},offset_ex(fiber_material_offset(materials[shape],-.5),scale_(.00001))).empty());
            Polyline local=path.geometry.to_polyline();
            for(auto& p:local.points) {const Vec2d v=frame*p.cast<double>()*SCALING_FACTOR;p=Point::new_scale(v.x(),v.y());}
            append(actual,local.lines());
            const auto lines=path.geometry.to_polyline().lines();
            bool crossing=false;
            for(size_t i=0;i<lines.size();++i)for(size_t j=i+2;j<lines.size();++j) {
                Point intersection;if(lines[i].intersection(lines[j],&intersection))crossing=true;
            }
            CHECK_FALSE(crossing);
        }
        for(const auto& scan:scans) {
            const double y=scan.points.front().y();
            const double lo=std::min(scan.points.front().x(),scan.points.back().x());
            const double hi=std::max(scan.points.front().x(),scan.points.back().x());
            std::vector<std::pair<double,double>> intervals;
            for(const auto& line:actual)if(std::abs(line.a.y()-y)<=4 && std::abs(line.b.y()-y)<=4) {
                const double a=std::max(lo,double(std::min(line.a.x(),line.b.x())));
                const double b=std::min(hi,double(std::max(line.a.x(),line.b.x())));
                if(b>a)intervals.emplace_back(a,b);
            }
            std::sort(intervals.begin(),intervals.end());
            double end=lo;
            for(const auto& interval:intervals) {
                CHECK(interval.first<=end+4); // No omitted scan interval.
                CHECK(interval.first>=end-4); // No duplicated scan interval.
                end=std::max(end,interval.second);
            }
            CHECK(end>=hi-4);
        }
    }
}

TEST_CASE("zero radius generated paths retain whole candidate validation and normal resin leftovers", "[ContinuousFiber][BoundaryInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=0;
    config.z_hop_height_mm = 2;config.cut_to_contact_length_mm=23;config.minimum_effective_length_mm=.5;
    config.finish_extension_length_mm=23;
    const ExPolygons material{rectangle(0,0,12,40),rectangle(30,0,32,2)};
    auto candidates=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
    const auto validated=FiberPathValidator::validate_infill(candidates,material,config,{1,0,0,0});
    REQUIRE(validated.accepted_count()==1);
    CHECK(validated.audit_assignments().valid());
    REQUIRE(validated.assignments.size()>1);
    bool short_rejected=false;
    for(const auto& assignment:validated.assignments)if(assignment.kind==FiberAssignmentKind::Rejected) {
        CHECK((assignment.reason==FiberRejectionReason::TooShort || assignment.reason==FiberRejectionReason::ProcessBudgetTooShort));
        short_rejected=true;
    }
    CHECK(short_rejected);
    const auto resin=ContinuousFiberFillStrategy::build_resin_area(material,validated.resin_exclusion);
    CHECK_FALSE(intersection_ex(resin,ExPolygons{rectangle(30,0,32,2)}).empty());
    candidates.paths.front().geometry.points.back().x()+=scale_(100.);
    CHECK_THROWS(FiberPathValidator::validate_infill(candidates,material,config,{1,0,0,0}));
}

TEST_CASE("generated infill below extraction resolution retains a short-path assignment",
          "[ContinuousFiber][BoundaryInfill][ExtractionResolution]")
{
    const ExPolygons domain{rectangle(0,0,50,20)};
    ContinuousFiberConfig config;
    config.infill_flow=Flow(.8f,.2f,.4f);
    config.landing_length_mm=2;
    config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5;
    FiberInfillCandidates candidates;
    candidates.centerline_domain=domain;
    const Point3 first=Point3::new_scale(5,5,0);
    Polyline3 geometry;
    // The actual failure measured sqrt(70^2+69^2) scaled units: 0.000098290386 mm.
    geometry.points={first,Point3(first+Point3(70,69,0))};
    candidates.paths.push_back({geometry,{}});
    const FiberDomainId id{7,218,0,2};

    SECTION("below, equal to, and above the extraction threshold") {
        const coord_t length=GENERATE(coord_t(99),coord_t(100),coord_t(101));
        candidates.paths.front().geometry.points.back()=Point3(first+Point3(length,coord_t(0),coord_t(0)));
        const auto result=FiberPathValidator::validate_infill(candidates,domain,config,id);
        REQUIRE(result.assignments.size()==1);
        CHECK(result.assignments.front().reason==(length<=SCALED_EPSILON ?
            FiberRejectionReason::TooShort : FiberRejectionReason::ProcessBudgetTooShort));
        CHECK(result.audit_assignments().valid());
    }
    SECTION("a normal printable path remains accepted") {
        candidates.paths.front().geometry=straight_path(5,5,45,5).polyline;
        const auto result=FiberPathValidator::validate_infill(candidates,domain,config,id);
        REQUIRE(result.accepted_count()==1);
        CHECK(result.audit_assignments().valid());
        CHECK_FALSE(result.resin_exclusion.empty());
    }
    SECTION("invalid process parameters remain fatal") {
        config.minimum_path_length_mm=std::numeric_limits<double>::quiet_NaN();
        REQUIRE_THROWS_WITH(FiberPathValidator::validate_infill(candidates,domain,config,id),
            Catch::Matchers::ContainsSubstring("invalid_parameter"));
    }
    SECTION("an empty material domain remains fatal") {
        REQUIRE_THROWS_WITH(FiberPathValidator::validate_infill(candidates,{},config,id),
            Catch::Matchers::ContainsSubstring("invalid_geometry"));
    }
    SECTION("a nonplanar path with analytic metadata remains fatal") {
        candidates.paths.front().geometry.points.front().z()=1;
        candidates.paths.front().geometry.points.back().z()=1;
        candidates.paths.front().arcs.emplace_back();
        REQUIRE_THROWS_WITH(FiberPathValidator::validate_infill(candidates,domain,config,id),
            Catch::Matchers::ContainsSubstring("invalid_geometry"));
    }
    SECTION("a zero-length candidate remains fatal") {
        candidates.paths.front().geometry.points.back()=first;
        REQUIRE_THROWS_WITH(FiberPathValidator::validate_infill(candidates,domain,config,id),
            Catch::Matchers::ContainsSubstring("invalid_geometry"));
    }
    SECTION("invalid flow remains fatal") {
        config.infill_flow=Flow(.8f,0.f,.4f);
        REQUIRE_THROWS(FiberPathValidator::validate_infill(candidates,domain,config,id));
    }
    SECTION("nonplanar contour validation keeps its original rejection") {
        auto path=straight_path(5,5,5.00007,5.000069);
        path.polyline.points.front().z()=1;
        path.polyline.points.back().z()=1;
        const auto result=FiberPathValidator::validate({&path},domain,config,
            FiberPathPurpose::Contour,erContinuousFiberContour,id);
        REQUIRE(result.assignments.size()==1);
        CHECK(result.assignments.front().reason==FiberRejectionReason::InvalidGeometry);
    }
    SECTION("an original out-of-domain rejection is not reclassified as short") {
        const Point3 outside=Point3::new_scale(60,5,0);
        candidates.paths.front().geometry.points={outside,Point3(outside+Point3(70,69,0))};
        REQUIRE_THROWS(FiberPathValidator::validate_infill(candidates,domain,config,id));
    }
    SECTION("the reproduced short path retains its complete interval and resin area") {
        const auto result=FiberPathValidator::validate_infill(candidates,domain,config,id);
        REQUIRE(result.candidates.size()==1);
        REQUIRE(result.assignments.size()==1);
        const auto& assignment=result.assignments.front();
        CHECK(assignment.kind==FiberAssignmentKind::Rejected);
        CHECK(assignment.reason==FiberRejectionReason::TooShort);
        CHECK(assignment.id.parent.domain==id);
        CHECK(assignment.source_begin_mm==0);
        CHECK(assignment.source_end_mm==Catch::Approx(unscale<double>(geometry.length())));
        CHECK_FALSE(assignment.prepared);
        CHECK(result.audit_assignments().valid());
        CHECK(result.physical_footprint.empty());
        CHECK(result.resin_exclusion.empty());
        CHECK(ContinuousFiberFillStrategy::build_resin_area(domain,result.resin_exclusion)==domain);
    }
}

TEST_CASE("contour clearance and infill half width are applied once", "[ContinuousFiber][BoundaryInfill]")
{
    for(double gap:{0.,.2}) {
        ContinuousFiberConfig config;
        config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=0;
        // A finalized 1 mm contour centered on y=1 reserves material up to 1.5+gap.
        const ExPolygons source{rectangle(0,0,12,40)},keepout{rectangle(0,0,12,1.5+gap)};
        const auto material=ContinuousFiberFillStrategy::build_infill_domain(source,keepout);
        const auto paths=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
        REQUIRE(paths.paths.size()==1);
        const auto bounds=get_extents(paths.paths.front().geometry.to_polyline());
        CHECK(unscale<double>(bounds.min.y())==Catch::Approx(2.+gap).margin(.00002));
    }
}

TEST_CASE("distant fiber islands cannot disconnect boundary returns", "[ContinuousFiber][BoundaryInfill]")
{
    Polygon polygon;
    for(const Vec2d& p:std::vector<Vec2d>{{0,0},{11.13,.17},{13.02,27.11},{7.63,29.83},{1.14,31.17}})
        polygon.points.push_back(Point::new_scale(p.x(),p.y()));
    const ExPolygon first(polygon);
    ExPolygon second=first;second.translate(Point::new_scale(50.127,1.373));
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=0;
    for(double angle:{0.,.37,.785398})for(double phase:{0.,.123,.5}) {
        CAPTURE(angle,phase);
        const auto a=ContinuousFiberFillStrategy::generate_rectilinear({first},config,angle,Point::new_scale(phase,0));
        const auto b=ContinuousFiberFillStrategy::generate_rectilinear({second},config,angle,Point::new_scale(phase,0));
        const auto together=ContinuousFiberFillStrategy::generate_rectilinear({first,second},config,angle,Point::new_scale(phase,0));
        CHECK(together.paths.size()==a.paths.size()+b.paths.size());
        for(const auto* isolated:{&a,&b})for(const auto& path:isolated->paths)
            CHECK(std::any_of(together.paths.begin(),together.paths.end(),[&](const auto& combined) {return combined.geometry.points==path.geometry.points;}));
    }
}


TEST_CASE("arc-free rounded infill remains an intact candidate at the process threshold", "[ContinuousFiber][BoundaryInfill]")
{
    ContinuousFiberConfig config;
    config.infill_flow=Flow(1.f,.13f,.4f);config.infill_density=100;config.infill_bend_radius_mm=.5;
    config.z_hop_height_mm = 2;config.cut_to_contact_length_mm=23;config.minimum_effective_length_mm=.5;
    config.finish_extension_length_mm=23;
    for(double length:{25.49,25.51}) {
        const ExPolygons material{rectangle(0,0,1.8,length+1)};
        const auto candidates=ContinuousFiberFillStrategy::generate_rectilinear(material,config,0,Point(0,0));
        REQUIRE(candidates.paths.size()==1);REQUIRE(candidates.paths.front().arcs.empty());
        const auto validated=FiberPathValidator::validate_infill(candidates,material,config,{1,0,0,0});
        CHECK(validated.accepted_count()==(length>25.5?1:0));
        CHECK(validated.audit_assignments().valid());
    }
}

TEST_CASE("fiber infill at a wall uses a circular footprint through process splits", "[ContinuousFiber][BoundaryInfill]")
{
    const auto candidate=path_from_points({{15,-15},{.5,0},{15,15}},1.);
    const ExPolygons material{rectangle(0,-20,20,20)};
    const ExPolygons centerlines{rectangle(.5,-19.5,19.5,19.5)};
    ContinuousFiberConfig config;
    config.minimum_path_length_mm=1;config.resin_overlap_mm=.05;
    // Put the process split on either side of the bend and exactly on it.
    const double half=unscale<double>(candidate.length())*.5;
    for(double tail:{half-1,half,half+1}) {
        config.cut_to_contact_length_mm=tail;
        const auto result=finalize_independent(candidate,material,config,test_id(),{},&centerlines);
        REQUIRE(result.prepared);
        CHECK(result.prepared->outside_domain.empty());
        CHECK(unscale<double>(get_extents(result.prepared->physical_coverage).min.x())>=-.0001);
        // Moving the same bend genuinely outside must still fail validation.
        auto outside=candidate;outside.polyline.points[1].x()=scale_(.3);
        const auto invalid=finalize_independent(outside,material,config,test_id(),{},&centerlines);
        CHECK(invalid.failure==FiberFinalizationFailure::OutsideDomain);
    }
}

TEST_CASE("closed fiber rounding distinguishes insufficient space from unresolved search", "[ContinuousFiber][ContourRounding][RoundingSpace]")
{
    std::ifstream stream(std::string(TEST_DATA_DIR)+"/continuous_fiber/7xiao_rounding_space.json");
    REQUIRE(stream.good());nlohmann::json fixture;stream>>fixture;
    Polyline3 source;
    for(const auto& p:fixture["source"])
        source.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>(),coord_t(0));
    ExPolygons domain;
    for(const auto& rings:fixture["domain"]) {
        ExPolygon region;
        for(size_t i=0;i<rings.size();++i) {
            Polygon ring;
            for(const auto& p:rings[i])ring.points.emplace_back(p[0].get<coord_t>(),p[1].get<coord_t>());
            if(i==0)region.contour=std::move(ring);else region.holes.push_back(std::move(ring));
        }
        domain.push_back(std::move(region));
    }
    const auto rounded=ContinuousFiberFillStrategy::round_hole_contour(source,domain,{.3});
    REQUIRE_FALSE(rounded.path);
    REQUIRE(rounded.issues.size()==1);
    CHECK(rounded.issues.front().reason==ContourRoundingFailure::InsufficientSpace);

    // A thin annulus has no R disk in its material, but a loop may enclose
    // its hole. An empty material erosion must not be used as a rejection proof.
    const auto square=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const auto annulus=rectangle_with_hole(-.01,-.01,10.01,10.01,.01,.01,9.99,9.99);
    const auto unresolved=ContinuousFiberFillStrategy::round_hole_contour(square.polyline,{annulus},{1.});
    REQUIRE_FALSE(unresolved.path);
    CHECK(std::none_of(unresolved.issues.begin(),unresolved.issues.end(),[](const auto& issue) {
        return issue.reason==ContourRoundingFailure::InsufficientSpace;
    }));
    // Tangent circles at the exact diameter remain feasible within the existing
    // geometry budget; infeasibility checks must reserve their numerical error.
    const auto small=path_from_points({{0,0},{2,0},{2,2},{0,2},{0,0}});
    const auto tangent=ContinuousFiberFillStrategy::round_hole_contour(small.polyline,{rectangle(0,0,2,2)},{1.});
    CHECK(tangent.path.has_value());
}

TEST_CASE("touching deposited fibers leave no internal resin channels", "[ContinuousFiber][FiberResinOccupancy]")
{
    const ExPolygons domain{rectangle(0,0,40,5)};
    ContinuousFiberConfig config;
    // A saved legacy overlap value must not reduce actual material occupancy.
    config.resin_overlap_mm=GENERATE(0.0,.05,.1);
    auto a=straight_path(1,1,39,1),b=straight_path(1,2,39,2),c=straight_path(1,3,39,3);
    a.width=b.width=c.width=1.;
    const auto accepted=FiberPathValidator::validate({&a,&b,&c},domain,config,
        FiberPathPurpose::Infill,erContinuousFiberInfill,{16,4,0,0});
    REQUIRE(accepted.accepted_count()==3);
    const auto resin=ContinuousFiberFillStrategy::build_resin_area(domain,accepted.resin_exclusion);
    CHECK(intersection_ex(resin,ExPolygons{rectangle(2,.6,38,3.4)}).empty());
    CHECK(diff_ex(ExPolygons{rectangle(2,3.6,38,4.9)},resin).empty());
    CHECK(diff_ex(accepted.physical_footprint,accepted.resin_exclusion).empty());
    CHECK(intersection_ex(resin,accepted.physical_footprint).empty());
}

TEST_CASE("concentric policy preserves display depth and ignores inactive infill process", "[ContinuousFiber][ConcentricReference]")
{
    ContinuousFiberConfig config;
    config.contour_enabled=true;config.contour_count=100;
    config.contour_flow=Flow(1.f,.2f,.4f);
    config.contour_material=2;config.contour_bend_radius_mm=.3;
    const auto reference=fiber_policy_key(config,0,false);
    std::set<FiberPolicyKey> display_policies;
    for (int count: {0,1,2,3,4,100}) {
        auto fill=config;fill.contour_enabled=count>0;fill.contour_count=count;
        fill.infill_enabled=true;fill.infill_pattern=ipConcentric;fill.infill_density=100;
        const auto policy=fiber_policy_key(fill,0,false);
        CHECK(display_policies.insert(policy).second);
        if (count==100) CHECK(policy==reference);
        else CHECK_FALSE(policy==reference);
        fill.infill_material=3;fill.infill_flow=Flow(2.f,.3f,.6f);
        fill.infill_bend_radius_mm=4;fill.infill_min_speed_mm_s=7;
        fill.contour_infill_clearance_mm=2;
        CHECK(fiber_policy_key(fill,1.234,true)==policy);
        auto different=fill;different.finish_extension_length_mm=3;
        CHECK_FALSE(fiber_policy_key(different,0,false)==policy);
        const auto physical=fiber_ring_reference_config(fill);
        CHECK(physical.contour_count==100);
        CHECK(physical.contour_flow.width()==config.contour_flow.width());
        CHECK(physical.contour_material==2);
        if (count==0) {
            // A retained count has no effect when displayed contours are off.
            fill.contour_count=4;
            CHECK(fiber_policy_key(fill,0,false)==policy);
        }
    }
}


TEST_CASE("concentric display cutoff cannot change contour traversal or process", "[ContinuousFiber][SharedRing]")
{
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=100;
    config.contour_boundary_clearance_mm=.05;config.contour_bend_radius_mm=.3;
    config.minimum_effective_length_mm=.5;
    config.z_hop_height_mm = 2;config.cut_to_contact_length_mm=23;config.finish_overlap_length_mm=23;
    const auto reference=FiberPathValidator::plan_contours({rectangle(0,0,60,40)},config,{},true);
    REQUIRE(reference.audit_lineage());REQUIRE(reference.validation.accepted_count()>4);
    for(size_t cutoff=0;cutoff<=4;++cutoff) {
        auto display=reference;display.set_display_cutoff(cutoff);
        REQUIRE(display.audit_lineage());
        REQUIRE(display.validation.assignments.size()==reference.validation.assignments.size());
        for(size_t i=0;i<display.validation.assignments.size();++i) {
            const auto& changed=display.validation.assignments[i];const auto& original=reference.validation.assignments[i];
            CHECK(changed.prepared==original.prepared);CHECK(changed.id==original.id);
            CHECK(changed.id.parent.purpose==FiberPathPurpose::Contour);
            const auto node=std::find_if(display.nodes.begin(),display.nodes.end(),[&](const auto& n){return n.id==changed.id.parent;});
            REQUIRE(node!=display.nodes.end());
            CHECK(changed.display_purpose==(node->depth<cutoff?FiberPathPurpose::Contour:FiberPathPurpose::Infill));
        }
    }
}

TEST_CASE("sparse shared rings add spacing only beyond the displayed contour cutoff", "[ContinuousFiber][SharedRing]")
{
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=4;
    config.contour_boundary_clearance_mm=.05;config.contour_bend_radius_mm=0;
    config.minimum_effective_length_mm=.5;config.z_hop_height_mm = 2;
    config.cut_to_contact_length_mm=23;config.finish_overlap_length_mm=23;
    const ExPolygons domain{rectangle(0,0,60,40)};
    const auto full=FiberPathValidator::plan_contours(domain,config,{},true);
    const auto sparse=FiberPathValidator::plan_contours(domain,config,{},true,2,1);
    const auto only_fill=FiberPathValidator::plan_contours(domain,config,{},true,0,1);
    REQUIRE(full.nodes.size()==4);REQUIRE(sparse.nodes.size()==4);REQUIRE(only_fill.nodes.size()==4);
    REQUIRE(sparse.audit_lineage());REQUIRE(only_fill.audit_lineage());
    for(size_t depth=0;depth<=2;++depth) {
        REQUIRE(full.nodes[depth].source);REQUIRE(sparse.nodes[depth].source);
        CHECK(full.nodes[depth].source->points==sparse.nodes[depth].source->points);
    }
    CHECK(full.nodes[3].source->points!=sparse.nodes[3].source->points);
    CHECK(full.nodes[0].source->points==only_fill.nodes[0].source->points);
    CHECK(full.nodes[1].source->points!=only_fill.nodes[1].source->points);
}


TEST_CASE("concentric stability counts only the gaps between marked complete bends", "[ContinuousFiber][ConcentricStability]")
{
    std::vector<ContourBend> bends(3);
    // The first treatment includes two arcs and their 10 mm connector.
    bends[0].begin_mm=0;bends[0].end_mm=12;
    bends[1].begin_mm=16;bends[1].end_mm=17;
    bends[2].begin_mm=23;bends[2].end_mm=24;
    REQUIRE(minimum_closed_loop_stable_length_mm(30,bends));
    CHECK(*minimum_closed_loop_stable_length_mm(30,bends)==Catch::Approx(4));
    bends[1].begin_mm=17;bends[1].end_mm=18;
    CHECK(*minimum_closed_loop_stable_length_mm(30,bends)==Catch::Approx(5));
    CHECK(*minimum_closed_loop_stable_length_mm(27,bends)==Catch::Approx(3));
    CHECK_FALSE(minimum_closed_loop_stable_length_mm(30,{}));
    bends.resize(1);
    CHECK(*minimum_closed_loop_stable_length_mm(30,bends)==Catch::Approx(18));
}

TEST_CASE("rounding records treatment groups without changing contour geometry", "[ContinuousFiber][ConcentricStability]")
{
    auto square=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-1,-1,11,11)};
    const auto forward=ContinuousFiberFillStrategy::round_outer_contour(square.polyline,domain,{.3});
    REQUIRE(forward.path);REQUIRE(forward.arcs.size()==4);
    REQUIRE(forward.bends.size()==4);
    CHECK(*minimum_closed_loop_stable_length_mm(unscale<double>(forward.path->length()),forward.bends)==Catch::Approx(9.4).margin(.00001));
    std::vector<continuous_fiber_detail::TangentSolution> connections;
    for(const auto& arc:forward.arcs) {
        continuous_fiber_detail::TangentSolution connection;connection.arcs={arc};
        connections.push_back(std::move(connection));
    }
    auto zero=forward.arcs[0];zero.start_mm=zero.end_mm;zero.sweep_radians=0;
    connections[1].arcs.insert(connections[1].arcs.begin(),zero);
    const auto linked=continuous_fiber_detail::validate_cycle(connections,square.polyline.to_polyline(),domain,domain,{.3});
    REQUIRE(linked.path);REQUIRE(linked.bends.size()==4);
    CHECK(*minimum_closed_loop_stable_length_mm(unscale<double>(linked.path->length()),linked.bends)==Catch::Approx(0).margin(.00001));
    square.polyline.reverse();
    const auto reverse=ContinuousFiberFillStrategy::round_outer_contour(square.polyline,domain,{.3});
    REQUIRE(reverse.path);REQUIRE(reverse.arcs.size()==4);
    auto reversed=*forward.path;reversed.reverse();
    CHECK(reverse.path->points==reversed.points);
    CHECK(*minimum_closed_loop_stable_length_mm(unscale<double>(reverse.path->length()),reverse.bends)==Catch::Approx(9.4).margin(.00001));
}

TEST_CASE("concentric rejects a whole short-entry ring without changing its outer contour", "[ContinuousFiber][ConcentricStability]")
{
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=100;
    config.contour_bend_radius_mm=.3;
    const ExPolygons domain{rectangle(0,0,8,8)};
    const auto ordinary=FiberPathValidator::plan_contours(domain,config,{},true);
    const auto constrained=FiberPathValidator::plan_contours(domain,config,{},true,1,0,5);
    REQUIRE(constrained.audit_lineage());REQUIRE(ordinary.validation.accepted_count()>1);
    REQUIRE(constrained.validation.accepted_count()==1);
    REQUIRE(constrained.nodes.size()==2);
    REQUIRE(constrained.validation.assignments.size()==2);
    const auto& outer=constrained.validation.assignments[0];const auto& original=ordinary.validation.assignments[0];
    REQUIRE(outer.prepared);REQUIRE(original.prepared);
    REQUIRE(outer.prepared->spans.size()==original.prepared->spans.size());
    for(size_t i=0;i<outer.prepared->spans.size();++i) {
        CHECK(outer.prepared->spans[i].kind==original.prepared->spans[i].kind);
        CHECK(outer.prepared->spans[i].geometry.points==original.prepared->spans[i].geometry.points);
    }
    const auto& failed=constrained.validation.assignments[1];
    CHECK(failed.reason==FiberRejectionReason::BendStabilizationTooShort);
    CHECK(failed.kind==FiberAssignmentKind::Rejected);CHECK_FALSE(failed.prepared);
    CHECK(constrained.validation.physical_footprint==outer.prepared->physical_coverage);
    CHECK(constrained.validation.resin_exclusion==outer.prepared->resin_exclusion);
    CHECK(constrained.stops.back().reason=="candidate_rejected");
    const auto shorter=FiberPathValidator::plan_contours(domain,config,{},true,1,0,3);
    CHECK(shorter.validation.accepted_count()>constrained.validation.accepted_count());
    const auto disabled=FiberPathValidator::plan_contours(domain,config,{},true,1,0,0);
    CHECK(disabled.validation.accepted_count()==ordinary.validation.accepted_count());
}

TEST_CASE("concentric accepted ring starts with a stable run and does not inspect ordinary curves", "[ContinuousFiber][ConcentricStability]")
{
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f);config.contour_count=1;config.contour_bend_radius_mm=.3;
    const auto ring=FiberPathValidator::plan_contours({rectangle(0,0,10,10)},config,{},true,0,0,5);
    REQUIRE(ring.validation.accepted_count()==1);
    const auto& route=ring.validation.assignments.front().centerline->polyline.points;
    REQUIRE(route.size()>2);CHECK(route.front()==route.back());
    // The seam is the previous treatment's exit, not the next bend's entry.
    CHECK(unscale<double>((route[1]-route[0]).cast<double>().norm())>=5);
    Polygon circle;
    for(size_t i=0;i<2048;++i)circle.points.push_back(Point::new_scale(10*std::cos(2*PI*i/2048),10*std::sin(2*PI*i/2048)));
    auto smooth_path=circle.split_at_first_point();
    const auto smooth=ContinuousFiberFillStrategy::round_outer_contour(Polyline3(smooth_path),{rectangle(-11,-11,11,11)},{.3});
    REQUIRE(smooth.path);CHECK(smooth.arcs.empty());
    CHECK_FALSE(minimum_closed_loop_stable_length_mm(unscale<double>(smooth.path->length()),smooth.bends));
    auto fill=config;fill.contour_enabled=true;fill.infill_enabled=true;fill.infill_pattern=ipConcentric;fill.infill_density=100;
    fill.corner_stabilization_length_mm=5;
    fill.concentric_corner_stabilization_length_mm=5;
    CHECK(fiber_ring_reference_config(fill).corner_stabilization_length_mm==0);
    CHECK(fiber_ring_reference_config(fill).concentric_corner_stabilization_length_mm==5);
    const auto key=fiber_policy_key(fill,0,false);
    fill.corner_stabilization_length_mm=100;
    CHECK(fiber_policy_key(fill,0,false)==key);
    fill.concentric_corner_stabilization_length_mm=3;
    CHECK_FALSE(fiber_policy_key(fill,0,false)==key);
    fill.concentric_corner_stabilization_length_mm=5;fill.contour_count=2;
    CHECK_FALSE(fiber_policy_key(fill,0,false)==key);
    fill.concentric_corner_stabilization_length_mm=0;
    config.contour_enabled=true;config.contour_count=fiber_contour_depth_limit;
    CHECK_FALSE(fiber_policy_key(fill,0,false)==fiber_policy_key(config,0,false));
    config.corner_stabilization_length_mm=100;
    config.concentric_corner_stabilization_length_mm=100;
    CHECK(fiber_ring_reference_config(config).corner_stabilization_length_mm==0);
    CHECK(fiber_ring_reference_config(config).concentric_corner_stabilization_length_mm==0);
}


TEST_CASE("strict policy boundary rejects physical protrusion without changing baseline tolerance", "[ContinuousFiber][fiber-mask]")
{
    const ExPolygons domain {rectangle(0, 0, 50, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 2;
    config.outside_tolerance_mm2 = 1.0;
    const auto candidate = straight_path(5, 0.399, 45, 0.399);
    REQUIRE(finalize_independent(candidate, domain, config, test_id()).prepared);
    config.enforce_policy_boundary = true;
    auto context = std::make_shared<FiberPolicyBoundary>();
    context->forbidden_core = {rectangle(0, -10, 50, 0)};
    config.policy_boundary = context;
    const auto rejected = finalize_independent(candidate, domain, config, test_id());
    CHECK_FALSE(rejected.prepared);
    CHECK(rejected.failure == FiberFinalizationFailure::PolicyBoundaryRejected);
    const auto accepted = finalize_independent(straight_path(5, 0.41, 45, 0.41), domain, config, test_id());
    REQUIRE(accepted.prepared);
    CHECK(accepted.prepared->policy_boundary->forbidden_core == context->forbidden_core);
    // With identical policy and planning domains, the accepted footprint is inside both.
    CHECK(accepted.prepared->outside_domain.empty());
}

TEST_CASE("strict policy boundary distinguishes planning reserve from the effective domain", "[ContinuousFiber][fiber-mask]")
{
    const ExPolygons policy{rectangle(0, 0, 50, 20)};
    const auto planning = offset_ex(policy, -float(scale_(0.01)));
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 2;
    config.outside_tolerance_mm2 = 1.0;
    config.enforce_policy_boundary = true;
    auto context = std::make_shared<FiberPolicyBoundary>();
    context->forbidden_core = {rectangle(0, -10, 50, 0)};
    config.policy_boundary = context;
    const auto candidate = straight_path(5, 0.4095, 45, 0.4095);
    const auto accepted = finalize_independent(candidate, planning, config, test_id(), {}, &planning);
    REQUIRE(accepted.prepared);
    CHECK(accepted.prepared->policy_boundary->forbidden_core == context->forbidden_core);
    CHECK_FALSE(diff_ex(accepted.prepared->physical_coverage, offset_ex(planning, 4.f),
                        ApplySafetyOffset::No).empty());
    CHECK(diff_ex(accepted.prepared->physical_coverage, offset_ex(policy, 4.f),
                  ApplySafetyOffset::No).empty());

    SECTION("true policy protrusion remains rejected") {
        const auto result = finalize_independent(straight_path(5, 0.399, 45, 0.399),
                                                       planning, config, test_id(), {}, &planning);
        CHECK_FALSE(result.prepared);
        CHECK(result.failure == FiberFinalizationFailure::PolicyBoundaryRejected);
    }
    SECTION("missing context preserves the original model-edge tolerance") {
        config.policy_boundary.reset();
        const auto result = finalize_independent(candidate, planning, config, test_id());
        CHECK(result.prepared);
    }
    SECTION("empty foreign domain preserves the original model-edge tolerance") {
        context->forbidden_core.clear();
        const auto result = finalize_independent(candidate, planning, config, test_id());
        CHECK(result.prepared);
    }
}

TEST_CASE("strict policy boundary preserves real holes even with a permissive planning domain", "[ContinuousFiber][fiber-mask]")
{
    const ExPolygons planning{rectangle(0, 0, 50, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 2;
    config.outside_tolerance_mm2 = 1.0;
    config.enforce_policy_boundary = true;
    auto context = std::make_shared<FiberPolicyBoundary>();
    context->forbidden_core = {rectangle(20, 5, 30, 15)};
    config.policy_boundary = context;
    REQUIRE(finalize_independent(straight_path(5, 4.59, 45, 4.59),
                                        planning, config, test_id()).prepared);
    const auto rejected = finalize_independent(straight_path(5, 4.601, 45, 4.601),
                                                     planning, config, test_id());
    CHECK_FALSE(rejected.prepared);
    CHECK(rejected.failure == FiberFinalizationFailure::PolicyBoundaryRejected);
}


namespace {
DynamicPrintConfig masked_print_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const std::vector<std::pair<std::string,std::string>> values {
        {"generate_reinforced_infills","1"}, {"generate_reinforced_perimeters","0"},
        {"reinforced_infill_filament","2"}, {"reinforced_infill_density","40%"},
        {"filament_map","1,2"}, {"filament_map_mode","Manual"}, {"single_extruder_multi_material","0"},
        {"flush_volumes_matrix","0,0,0,0"},
        {"filament_colour","#FFFFFF;#000000"},
        {"fiber_cut_gcode","M400\nM42 P4 S255\nG4 P100\n; MASK_TEST_CUT"},
        {"filament_diameter","1.75,1.75"}, {"nozzle_diameter","0.4,0.4"},
        {"filament_process_type","thermoplastic;continuous_fiber"},
        {"toolhead_process_capabilities","thermoplastic;continuous_fiber"},
        {"toolhead_fiber_protocol_id",";linear-e-v1"}, {"physical_extruder_map","0,1"},
        {"toolhead_fiber_e_units_per_mm","1,1"}, {"filament_fiber_feed_correction","1,1"},
        {"fiber_width","0.8"}, {"fiber_minimum_path_length","1"},
        {"fiber_cut_to_contact_length","2"}, {"fiber_minimum_effective_length","0"},
        {"fiber_contour_bend_radius","0"}, {"fiber_infill_bend_radius","0"},
        {"fiber_finish_extension_length","0"}, {"fiber_finish_overlap_length","0"},
        {"enable_prime_tower","0"}, {"sparse_infill_density","15%"},
        {"top_shell_layers","1"}, {"bottom_shell_layers","1"}, {"layer_height","0.2"},
        {"initial_layer_print_height","0.2"}
    };
    for (const auto& [key,value]:values) config.set_deserialize_strict(key,value);
    // An explicit command-only test machine; production profiles retain their scripts.
    for (const auto& key : config.keys())
        if (key.size() >= 6 && key.compare(key.size()-6,6,"_gcode") == 0 &&
            key != "fiber_cut_gcode" && key != "emit_machine_limits_to_gcode")
            config.set_deserialize_strict(key, "");
    return config;
}
}

TEST_CASE("CFSYS final export limits only first approaches and restores material PA", "[ContinuousFiber][fiber-approach][cfsys][OrcaIsolation]")
{
    auto config = masked_print_config();
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"gcode_flavor", "klipper"}, {"toolhead_fiber_protocol_id", ";cfsys-v1"},
        {"fiber_cut_gcode", "M400\nS0\nM400\n"}, {"travel_speed", "350"},
        {"enable_pressure_advance", "1,1"}, {"pressure_advance", "0.05,0"},
        {"fiber_layer_height_ratio", "1"}, {"top_shell_thickness", "0"}, {"bottom_shell_thickness", "0"},
        {"fiber_toolchange_approach_speed", "0"}})
        config.set_deserialize_strict(key, value);
    Model model;
    auto* object = model.add_object();
    object->add_volume(make_cube(60, 40, 2), ModelVolumeType::MODEL_PART, false);
    object->add_instance()->set_offset(Vec3d(100, 100, 0));
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.apply(model, config);
    print.process();
    const auto output = std::filesystem::temp_directory_path() /
        ("fiber-approach-" + std::to_string(std::random_device{}()) + ".gcode");
    GCode generator; // Reuse the same exporter across all effective settings.
    GCodeProcessorResult result;
    size_t baseline_paths = 0;
    const std::regex feed("(?:^| )F([0-9.]+)");
    const std::regex advance("ADVANCE=([0-9.]+)");
    for (const double cap : {0.0, 60.0, 40.0}) {
        config.set_deserialize_strict("fiber_toolchange_approach_speed", std::to_string(cap));
        print.apply(model, config);
        // A speed-only change must leave planning and simplification complete.
        for (const auto* planned : print.objects()) {
            CHECK(planned->is_step_done(posInfill));
            CHECK(planned->is_step_done(posSimplifyInfill));
        }
        print.set_gcode_file_invalidated();
        REQUIRE_NOTHROW(generator.do_export(&print, output.string().c_str(), &result));
        std::ifstream input(output);
        REQUIRE(input.good());
        std::string line;
        std::smatch match;
        int tool = -1;
        double pa = -1, f = 0;
        size_t paths = 0, activations = 0, zeros = 0;
        bool first_pending = false, first_approach = false, in_approach = false;
        bool saw_fast_later = false, saw_restore = false;
        while (std::getline(input, line)) {
            if (line == "T1" || line.find("T1 ;") == 0) {
                if (tool != 1) { ++activations; first_pending = true; pa = -1; }
                tool = 1;
            } else if (line == "T0" || line.find("T0 ;") == 0) {
                tool = 0;
            }
            if (line.find("SET_PRESSURE_ADVANCE ") == 0 && std::regex_search(line, match, advance)) {
                pa = std::stod(match[1]);
                if (tool == 1) { CHECK(pa == 0); ++zeros; }
                if (tool == 0 && pa == Catch::Approx(0.05)) saw_restore = true;
            }
            if (line.find(";FIBER_BEGIN ") == 0) {
                REQUIRE(tool == 1);
                CHECK(pa == 0);
                ++paths;
                first_approach = first_pending;
                first_pending = false;
                in_approach = true;
            }
            if (std::regex_search(line, match, feed)) f = std::stod(match[1]);
            if (in_approach && line.find("G1 ") == 0 &&
                (line.find('X') != std::string::npos || line.find('Y') != std::string::npos)) {
                if (first_approach && cap > 0) CHECK(f <= cap * 60.0);
                if (!first_approach && f > 3600) saw_fast_later = true;
            }
            if (line == ";FIBER_PREFEED_BEGIN") { CHECK(pa == 0); in_approach = false; }
        }
        REQUIRE(paths > 0);
        CHECK(zeros == activations);
        CHECK(saw_restore);
        CHECK(saw_fast_later);
        if (cap == 0) baseline_paths = paths;
        else CHECK(paths == baseline_paths);
    }
    // End a resin path exactly at the first fiber start. The first Approach
    // then has no XY command, but must still consume the activation limit.
    std::unique_ptr<ExtrusionEntity> first_fiber;
    Layer* first_fiber_layer = nullptr;
    std::function<void(const ExtrusionEntity*)> find_first = [&](const ExtrusionEntity* entity) {
        if (first_fiber) return;
        if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
            for (const auto* child : collection->entities) find_first(child);
        } else if (dynamic_cast<const ExtrusionFiberPath*>(entity)) {
            first_fiber.reset(entity->clone());
        }
    };
    for (auto* layer : print.objects().front()->layers()) {
        for (const auto* region : layer->regions()) find_first(&region->fills);
        if (first_fiber) { first_fiber_layer = layer; break; }
    }
    REQUIRE(first_fiber_layer);
    config.set_deserialize_strict("fiber_toolchange_approach_speed", "60");
    print.apply(model, config);
    for (auto* region : first_fiber_layer->regions()) {
        region->perimeters.clear();
        region->fills.clear();
    }
    auto* ordered = new ExtrusionEntityCollection;
    ordered->no_sort = true;
    auto* resin = new ExtrusionPath(erResinInfill, 0.08, 0.4f, 0.2f);
    const Point start = first_fiber->first_point();
    resin->polyline.points = {
        Point3(int64_t(start.x() - scale_(1.0)), int64_t(start.y()), int64_t(0)),
        Point3(int64_t(start.x()), int64_t(start.y()), int64_t(0))};
    ordered->entities = {resin, first_fiber->clone(), first_fiber->clone()};
    first_fiber_layer->regions().front()->fills.entities.push_back(ordered);
    print.set_gcode_file_invalidated();
    REQUIRE_NOTHROW(generator.do_export(&print, output.string().c_str(), &result));
    {
        std::ifstream input(output);
        std::string line;
        std::smatch match;
        size_t paths = 0;
        bool in_approach = false, second_moves = false;
        double f = 0;
        Vec3d previous=Vec3d::Zero();
        const std::regex xy_axis("(?:^| )([XYZ])([-0-9.]+)");
        while (std::getline(input, line)) {
            if (line.find(";FIBER_BEGIN ") == 0) { ++paths; in_approach = true; }
            if (std::regex_search(line, match, feed)) f = std::stod(match[1]);
            Vec3d target=previous;
            for(auto it=std::sregex_iterator(line.begin(),line.end(),xy_axis);it!=std::sregex_iterator();++it)
                target[(*it)[1].str()=="X"?0:(*it)[1].str()=="Y"?1:2]=std::stod((*it)[2]);
            if (in_approach && line.find("G1 ") == 0 &&
                (line.find('X') != std::string::npos || line.find('Y') != std::string::npos)) {
                if(paths==1) {
                    // Resin used three decimal places; align its rounded end
                    // to the fiber's six-decimal source before the first arc.
                    CHECK((target-previous).head<2>().norm()<=std::sqrt(2.)*.001);
                    CHECK(f<=3600);
                }
                if(paths==2) {
                    const Vec3d delta=target-previous;
                    double expected_speed=350;
                    const char* axis_keys[]={"machine_max_speed_x","machine_max_speed_y","machine_max_speed_z"};
                    for(int axis=0;axis<3;++axis) {
                        const double limit=config.option<ConfigOptionFloats>(axis_keys[axis])->get_at(0);
                        if(limit>0 && std::abs(delta[axis])>0)expected_speed=std::min(expected_speed,limit*delta.norm()/std::abs(delta[axis]));
                    }
                    // The second approach is free of the first-tool cap and
                    // still obeys the exact XYZ command's machine axis limits.
                    CHECK(f==Catch::Approx(expected_speed*60).margin(.0011));second_moves=true;
                }
            }
            previous=target;
            if (line == ";FIBER_PREFEED_BEGIN") in_approach = false;
            if (paths == 2 && line == ";FIBER_PREFEED_BEGIN") break;
        }
        REQUIRE(paths == 2);
        CHECK(second_moves);
    }
    config.set_deserialize_strict("pressure_advance", "0.05,0.2");
    print.apply(model, config);
    // Execution changes revoke the old table. The forbidden PA must now fail
    // during process certification as well as the defensive export preflight.
    REQUIRE_THROWS_AS(print.process(),FiberExecutionError);
    CHECK_FALSE(print.fiber_execution_plans());
    print.set_gcode_file_invalidated();
    REQUIRE_THROWS_WITH(generator.do_export(&print, output.string().c_str(), &result),
        "Fiber execution table is missing or invalidated; process must complete before export");
    // A disabled setting with a saved nonzero value must not block export.
    config.set_deserialize_strict("enable_pressure_advance", "1,0");
    print.apply(model, config);
    REQUIRE_NOTHROW(print.process());
    print.set_gcode_file_invalidated();
    REQUIRE_NOTHROW(generator.do_export(&print, output.string().c_str(), &result));
    // Nonzero enabled PA on an inventory slot that is never used for fiber
    // must not be rejected by the whole-plan preflight.
    config.set_deserialize_strict("enable_pressure_advance", "1,1");
    config.set_deserialize_strict("generate_reinforced_infills", "0");
    print.apply(model, config);
    print.process();
    print.set_gcode_file_invalidated();
    REQUIRE_NOTHROW(generator.do_export(&print, output.string().c_str(), &result));
    std::ifstream resin_only(output);
    const std::string resin_gcode((std::istreambuf_iterator<char>(resin_only)), {});
    CHECK(resin_gcode.find(";FIBER_BEGIN ") == std::string::npos);
    std::error_code error;
    std::filesystem::remove(output, error);
}

TEST_CASE("resin-only SDK provenance preserves native bridge surface classification", "[ContinuousFiber][fiber-mask]")
{
    auto config = masked_print_config();
    const int interval = GENERATE(1, 2);
    for (const auto& value : std::vector<std::pair<std::string, std::string>> {
        {"generate_reinforced_perimeters", "1"}, {"reinforced_perimeters_filament", "2"},
        {"fiber_resin_fill_density", "0%"}, {"sparse_infill_density", "0%"},
        {"wall_loops", "2"}, {"top_shell_layers", "1"}, {"top_shell_thickness", "1"},
        {"bottom_shell_layers", "1"}, {"bottom_shell_thickness", "0"},
        {"ensure_vertical_shell_thickness", "ensure_all"}, {"fiber_contour_bend_radius", "0.3"},
        {"fiber_infill_bend_radius", "0.3"}})
        config.set_deserialize_strict(value.first, value.second);
    config.set_deserialize_strict("fiber_layer_height_ratio", std::to_string(interval));

    // Keep every native surface attribute, grouping only geometrically equivalent
    // fragments. Runtime model IDs and polygon vertex starting points may differ.
    using SurfaceKey = std::tuple<int, double, unsigned short, double, unsigned short>;
    using ClassifiedRegion = std::map<SurfaceKey, ExPolygons>;
    struct Snapshot {
        std::vector<std::vector<ClassifiedRegion>> layers;
        std::vector<ExPolygons> eligible_cores;
    };
    const auto run = [&](int source) {
        Model model;
        auto* object = model.add_object();
        object->add_volume(make_cube(60, 40, 2), ModelVolumeType::MODEL_PART, false);
        if (source >= 0) {
            auto mesh = make_cube(28, 24, 2);
            mesh.translate(Vec3f(16, 8, 0));
            auto* modifier = object->add_volume(std::move(mesh), ModelVolumeType::PARAMETER_MODIFIER, false);
            modifier->config.set("fiber_mask_process", source == 1);
            modifier->config.set_key_value("fiber_resin_fill_density", new ConfigOptionPercent(50));
            modifier->config.set("fiber_resin_fill_speed", 17.0);
        }
        object->add_instance()->set_offset(Vec3d(100, 100, 0));
        Print print;
        print.set_plate_origin(Vec3d::Zero());
        print.is_BBL_printer() = false;
        print.apply(model, config);
        print.process();
        Snapshot result;
        size_t eligible_surfaces = 0, prepared_paths = 0, boundaries = 0, resin_first = 0;
        bool bridge_surface = false;
        std::function<void(const ExtrusionEntity*)> inspect = [&](const ExtrusionEntity* entity) {
            if (const auto* collection = dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
                for (const auto* child : collection->entities) inspect(child);
            } else if (const auto* fiber = dynamic_cast<const ExtrusionFiberPath*>(entity)) {
                ++prepared_paths;
                const auto& path = *fiber->prepared_path();
                if (path.policy_boundary) {
                    ++boundaries;
                    // This is the exact predicate that enables policy clipping
                    // in Fill and finalization; an empty foreign core stays tolerant.
                    CHECK(path.policy_boundary->forbidden_core.empty());
                    resin_first += path.policy_boundary->requires_resin_first;
                }
            }
        };
        for (const auto* layer : print.objects().front()->layers()) {
            result.layers.emplace_back();
            ExPolygons core;
            for (const auto* region : layer->regions()) {
                result.layers.back().emplace_back();
                auto& classified = result.layers.back().back();
                for (const auto& surface : region->fill_surfaces.surfaces) {
                    classified[{int(surface.surface_type), surface.thickness, surface.thickness_layers,
                        surface.bridge_angle, surface.extra_perimeters}].push_back(surface.expolygon);
                    if (surface.surface_type == stInternal) {
                        core.push_back(surface.expolygon);
                        ++eligible_surfaces;
                    }
                    bridge_surface |= surface.surface_type == stInternalBridge;
                }
                inspect(&region->fills);
            }
            result.eligible_cores.push_back(union_ex(core));
        }
        REQUIRE(eligible_surfaces > 0);
        REQUIRE(prepared_paths > 0);
        REQUIRE(bridge_surface);
        if (source == 1) {
            REQUIRE(boundaries > 0);
            REQUIRE(resin_first > 0);
        } else CHECK(boundaries == 0);
        return result;
    };
    const auto baseline = run(-1);
    const auto native = run(0);
    const auto sdk = run(1);
    REQUIRE(native.layers.size() == sdk.layers.size());
    REQUIRE(baseline.eligible_cores.size() == native.eligible_cores.size());
    bool native_modifier_changes_core = false;
    for (size_t layer = 0; layer < native.layers.size(); ++layer) {
        INFO("layer=" << layer << " interval=" << interval);
        REQUIRE(native.layers[layer].size() == sdk.layers[layer].size());
        for (size_t region = 0; region < native.layers[layer].size(); ++region) {
            const auto& expected = native.layers[layer][region];
            const auto& actual = sdk.layers[layer][region];
            REQUIRE(actual.size() == expected.size());
            for (const auto& [key, polygons] : expected) {
                const auto found = actual.find(key);
                REQUIRE(found != actual.end());
                CHECK(diff_ex(polygons, found->second, ApplySafetyOffset::No).empty());
                CHECK(diff_ex(found->second, polygons, ApplySafetyOffset::No).empty());
            }
        }
        const auto& before = baseline.eligible_cores[layer];
        const auto& after = native.eligible_cores[layer];
        native_modifier_changes_core |= before.size() != after.size() ||
            std::abs(area(before) - area(after)) > double(scale_(1.0)) * scale_(0.1);
    }
    // Native bridge anchoring really changed the eligible core. The SDK source
    // marker must preserve this classification rather than erase solid supports.
    REQUIRE(native_modifier_changes_core);
}

TEST_CASE("masked material order validates only the effective native rule", "[ContinuousFiber][fiber-mask]")
{
    auto config = masked_print_config();
    config.set_deserialize_strict("extruder_offset", GENERATE("0x0,0x0", "0x0,0.1734x0.2876"));
    config.set_deserialize_strict("other_layers_print_sequence_nums", "2");
    const bool conflict = GENERATE(false,true);
    config.set_deserialize_strict("other_layers_print_sequence",
        conflict ? "1,10,1,2,1,10,2,1" : "1,10,2,1,1,10,1,2");
    Model model;
    auto* object=model.add_object();
    object->add_volume(make_cube(60,40,2),ModelVolumeType::MODEL_PART,false);
    auto mesh=make_cube(28,24,2); mesh.translate(Vec3f(16,8,0));
    auto* modifier=object->add_volume(std::move(mesh),ModelVolumeType::PARAMETER_MODIFIER,false);
    modifier->config.set("fiber_mask_process",true);
    modifier->config.set("fiber_width",1.0);
    object->add_instance()->set_offset(Vec3d(100,100,0));
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.apply(model,config);
    if (conflict) {
        REQUIRE_THROWS_WITH(print.process(),
            "Explicit material order conflicts with masked resin-before-fiber execution");
    } else {
        print.process();
        const auto& tools = print.tool_ordering();
        bool checked=false;
        for (const auto& layer:tools)
            if (layer.extruders.size()>1 && layer.has_extruder(1)) {
                CHECK(layer.extruders.back()==1); checked=true;
            }
        REQUIRE(checked);
        // Independent analytic capsule oracle, using the actual forbidden policy
        // attached to each path. Reconstruct the 0.001-mm command rounding with
        // this instance/nozzle phase; test segment distance, not clipper coverage.
        const auto point_distance=[](const Vec2d& p,const Vec2d& a,const Vec2d& b) {
            const Vec2d d=b-a;
            const double length2=d.squaredNorm();
            const double t=length2==0?0:std::clamp((p-a).dot(d)/length2,0.0,1.0);
            return (p-(a+t*d)).norm();
        };
        const auto capsule_clear=[&](const Vec2d& a,const Vec2d& b,double radius,const ExPolygons& forbidden) {
            for(const auto& polygon:forbidden) {
                if(polygon.contains(Point::new_scale(a.x(),a.y())) ||
                   polygon.contains(Point::new_scale(b.x(),b.y()))) return false;
                const auto clear_loop=[&](const Polygon& loop) {
                    for(size_t i=0;i<loop.points.size();++i) {
                        const Vec2d c=unscaled(loop.points[i]);
                        const Vec2d d=unscaled(loop.points[(i+1)%loop.points.size()]);
                        const auto cross=[](const Vec2d& u,const Vec2d& v) {return u.x()*v.y()-u.y()*v.x();};
                        if(cross(b-a,c-a)*cross(b-a,d-a)<0 &&
                           cross(d-c,a-c)*cross(d-c,b-c)<0) return false;
                        const double distance=std::min({point_distance(a,c,d),point_distance(b,c,d),
                            point_distance(c,a,b),point_distance(d,a,b)});
                        if(distance+1e-9<radius) return false;
                    }
                    return true;
                };
                if(!clear_loop(polygon.contour)) return false;
                for(const auto& hole:polygon.holes) if(!clear_loop(hole)) return false;
            }
            return true;
        };
        const Vec2d origin=unscaled(print.objects().front()->instances().front().shift);
        size_t strict_paths=0,rounded_edges=0;
        bool negative_control=false;
        std::function<void(const ExtrusionEntity*)> check_policy=[&](const ExtrusionEntity* entity) {
            if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(entity)) {
                for(const auto* child:collection->entities) check_policy(child);
            } else if(const auto* fiber=dynamic_cast<const ExtrusionFiberPath*>(entity)) {
                const auto& prepared=*fiber->prepared_path();
                if(!prepared.policy_boundary || prepared.policy_boundary->forbidden_core.empty()) return;
                ++strict_paths;
                const auto forbidden=offset_ex(prepared.policy_boundary->forbidden_core,-4.f);
                const size_t tool=size_t(print.config().filament_map.get_at(prepared.logical_filament_id)-1);
                const Vec2d phase=origin-print.config().extruder_offset.get_at(tool);
                const auto rounded=[&](const Point3& point) {
                    const Vec2d local=unscaled(point.to_point());
                    return Vec2d(std::round((local.x()+phase.x())*1000)/1000-phase.x(),
                                 std::round((local.y()+phase.y())*1000)/1000-phase.y());
                };
                for(const auto& span:prepared.spans) if(span.deposits_fiber())
                    for(size_t i=1;i<span.geometry.points.size();++i) {
                        CHECK(capsule_clear(rounded(span.geometry.points[i-1]),rounded(span.geometry.points[i]),
                                            .5*prepared.width_mm,forbidden));
                        ++rounded_edges;
                    }
                if(!negative_control && !forbidden.empty()) {
                    const auto box=get_extents(forbidden);
                    const double y=unscale<double>(forbidden.front().contour.points.front().y());
                    const Vec2d a(unscale<double>(box.min.x())-1,y);
                    const Vec2d b(unscale<double>(box.max.x())+1,y);
                    for(const auto& polygon:forbidden) {
                        REQUIRE_FALSE(polygon.contains(Point::new_scale(a.x(),a.y())));
                        REQUIRE_FALSE(polygon.contains(Point::new_scale(b.x(),b.y())));
                    }
                    CHECK_FALSE(capsule_clear(a,b,.5*prepared.width_mm,forbidden));
                    negative_control=true;
                }
            }
        };
        for(const auto* layer:print.objects().front()->layers())
            for(const auto* region:layer->regions())
                for(const auto* entity:region->fills.entities) check_policy(entity);
        REQUIRE(strict_paths>0);
        REQUIRE(rounded_edges>0);
        REQUIRE(negative_control);
        SECTION("finalizer rejects a foreign policy before publishing") {
            bool checked=false;
            for(auto* layer:print.objects().front()->layers())for(auto* region:layer->regions()) {
                auto policy=resolve_continuous_fiber_config(*layer,*region);
                auto boundary=std::make_shared<FiberPolicyBoundary>();
                boundary->forbidden_core={rectangle(-1000,-1000,1000,1000)};
                policy.policy_boundary=boundary;policy.enforce_policy_boundary=true;
                std::vector<const ExtrusionFiberPath*> fibers;
                const auto collect=[&](const auto& self,const ExtrusionEntity& entity)->void {
                    if(const auto* fiber=dynamic_cast<const ExtrusionFiberPath*>(&entity))fibers.push_back(fiber);
                    else if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(&entity))
                        for(const auto* child:collection->entities)self(self,*child);
                };
                collect(collect,region->fills);
                for(const auto* fiber:fibers) {
                    ExtrusionPath source(*fiber);source.polyline={};
                    const auto& prepared=*fiber->prepared_path();
                    if(prepared.entry.lowers())source.polyline=prepared.entry.geometry;
                    for(const auto& span:prepared.spans)if(span.deposits_fiber())Geometry::append_with_arcs(source.polyline,span.geometry);
                    const auto rejected=finalize_independent(source,prepared.allowed_domain,policy,prepared.id);
                    CHECK_FALSE(rejected.prepared);checked=true;break;
                }
                if(checked)break;
            }
            REQUIRE(checked);
            STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<PreparedFiberPath>);
        }
    }
}


TEST_CASE("strict infill policy rejection remains a nonfatal resin fallback", "[ContinuousFiber][fiber-mask]")
{
    const ExPolygons domain{rectangle(0,0,50,20)};
    ContinuousFiberConfig config;
    config.infill_flow=Flow(.8f,.2f,.4f);
    config.cut_to_contact_length_mm=2;
    config.outside_tolerance_mm2=1;
    FiberInfillCandidates candidates;
    candidates.centerline_domain=domain;
    candidates.paths.push_back({straight_path(5,.399,45,.399).polyline,{}});
    const FiberDomainId id{7,62,4,1};
    REQUIRE(FiberPathValidator::validate_infill(candidates,domain,config,id).accepted_count()==1);
    config.enforce_policy_boundary=true;
    auto boundary=std::make_shared<FiberPolicyBoundary>();
    boundary->forbidden_core={rectangle(0,-10,50,0)};
    config.policy_boundary=boundary;
    const auto result=FiberPathValidator::validate_infill(candidates,domain,config,id);
    CHECK(result.accepted_count()==0);
    REQUIRE(result.rejected_count()==1);
    CHECK(result.assignments.front().reason==FiberRejectionReason::OutsideDomain);
    CHECK(result.resin_exclusion.empty());
    CHECK(ContinuousFiberFillStrategy::build_resin_area(domain,result.resin_exclusion)==domain);
    SECTION("an unrelated outside-domain failure remains fatal") {
        config.outside_tolerance_mm2=0;
        candidates.paths.push_back({straight_path(5,19.601,45,19.601).polyline,{}});
        REQUIRE_THROWS_WITH(FiberPathValidator::validate_infill(candidates,domain,config,id),
            Catch::Matchers::ContainsSubstring("Generated fiber infill failed preparation: finalized_path_outside_domain;") &&
            Catch::Matchers::ContainsSubstring("layer=62"));
    }
    SECTION("policy failure retains exact-ID fallback even with zero outside tolerance") {
        config.outside_tolerance_mm2=0;
        const auto rejected=FiberPathValidator::validate_infill(candidates,domain,config,id);
        CHECK(rejected.accepted_count()==0);
        CHECK(rejected.rejected_count()==1);
        CHECK(rejected.resin_exclusion.empty());
    }
}


TEST_CASE("nested masked fiber groups emit the prepared materials rather than region defaults", "[ContinuousFiber][fiber-mask]")
{
    auto config = masked_print_config();
    for (const auto& [key,value] : std::vector<std::pair<std::string,std::string>>{
        {"filament_map","1,2,3"}, {"filament_printable","7,7,7"}, {"filament_colour","#FFFFFF;#000000;#FF0000"},
        {"flush_volumes_matrix","0,0,0,0,0,0,0,0,0"},
        {"filament_diameter","1.75,1.75,1.75"}, {"nozzle_diameter","0.4,0.4,0.4"},
        {"filament_process_type","thermoplastic;continuous_fiber;continuous_fiber"},
        {"toolhead_process_capabilities","thermoplastic;continuous_fiber;continuous_fiber"},
        {"toolhead_fiber_protocol_id",";linear-e-v1;linear-e-v1"}, {"physical_extruder_map","0,1,2"},
        {"toolhead_fiber_e_units_per_mm","1,1,1"}, {"filament_fiber_feed_correction","1,1,1"}})
        config.set_deserialize_strict(key,value);
    Model model;
    auto* object=model.add_object();
    object->add_volume(make_cube(60,40,2),ModelVolumeType::MODEL_PART,false);
    auto mesh=make_cube(28,24,2); mesh.translate(Vec3f(16,8,0));
    auto* modifier=object->add_volume(std::move(mesh),ModelVolumeType::PARAMETER_MODIFIER,false);
    modifier->config.set("fiber_mask_process",true);
    modifier->config.set("reinforced_infill_filament",3);
    object->add_instance()->set_offset(Vec3d(100,100,0));
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.apply(model,config);
    print.process();
    bool nested=false;
    size_t expected_fibers=0;
    for(auto* layer:print.objects().front()->layers()) {
        std::set<unsigned> materials;
        std::function<void(const ExtrusionEntity&)> collect=[&](const ExtrusionEntity& entity) {
            if(const auto* group=dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
                for(const auto* child:group->entities) collect(*child);
            } else if(const auto* fiber=dynamic_cast<const ExtrusionFiberPath*>(&entity)) {
                materials.insert(fiber->prepared_path()->logical_filament_id);
                ++expected_fibers;
            }
        };
        for(auto* region:layer->regions()) for(const auto* entity:region->fills.entities) collect(*entity);
        if(nested || materials!=std::set<unsigned>{1,2}) continue;
        // Move the real generated fiber leaves into a deliberately nested mixed
        // collection, preserving their immutable prepared data and ownership.
        auto mixed=std::make_unique<ExtrusionEntityCollection>();
        auto branch=std::make_unique<ExtrusionEntityCollection>();
        branch->no_sort=true;
        std::function<void(ExtrusionEntityCollection&)> extract=[&](ExtrusionEntityCollection& group) {
            for(auto it=group.entities.begin();it!=group.entities.end();) {
                if(dynamic_cast<ExtrusionFiberPath*>(*it)) {
                    branch->entities.push_back(*it);
                    it=group.entities.erase(it);
                } else {
                    if(auto* child=dynamic_cast<ExtrusionEntityCollection*>(*it)) {
                        extract(*child);
                        if(child->entities.empty()) {
                            delete child;
                            it=group.entities.erase(it);
                            continue;
                        }
                    }
                    ++it;
                }
            }
        };
        for(auto* region:layer->regions()) extract(region->fills);
        mixed->entities.push_back(branch.release());
        layer->regions().front()->fills.entities.push_back(mixed.release());
        nested=true;
        const auto& tools=print.tool_ordering().tools_for_layer(layer->print_z);
        CHECK(tools.has_extruder(1)); CHECK(tools.has_extruder(2));
        bool fiber_seen=false;
        for(unsigned material:tools.extruders) {
            if(material==1 || material==2) fiber_seen=true;
            else CHECK_FALSE(fiber_seen);
        }
    }
    REQUIRE(nested);
    const auto output=std::filesystem::temp_directory_path()/"fiber-mask-nested-materials.gcode";
    REQUIRE_NOTHROW(print.export_gcode(output.string(),nullptr));
    std::ifstream file(output); REQUIRE(file.good());
    std::string line;
    std::set<unsigned> emitted;
    size_t actual_fibers=0;
    while(std::getline(file,line)) if(line.rfind(";FIBER_BEGIN ",0)==0) {
        const auto pos=line.find(" filament="); REQUIRE(pos!=std::string::npos);
        emitted.insert(unsigned(std::stoul(line.substr(pos+10)))); ++actual_fibers;
    }
    CHECK(emitted==std::set<unsigned>{1,2});
    CHECK(actual_fibers==expected_fibers);
    std::error_code error; std::filesystem::remove(output,error);
}

TEST_CASE("nested masked groups preserve ordinary role materials and ordered branches", "[ContinuousFiber][fiber-mask]")
{
    auto config = masked_print_config();
    for (const auto& [key, value] : std::vector<std::pair<std::string, std::string>>{
        {"filament_map", "1,2,3,4"}, {"filament_printable", "15,15,15,15"}, {"filament_colour", "#FFFFFF;#00FF00;#FF0000;#000000"},
        {"flush_volumes_matrix", "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"},
        {"filament_diameter", "1.75,1.75,1.75,1.75"}, {"nozzle_diameter", "0.4,0.4,0.4,0.4"},
        {"filament_process_type", "thermoplastic;thermoplastic;thermoplastic;continuous_fiber"},
        {"toolhead_process_capabilities", "thermoplastic;thermoplastic;thermoplastic;continuous_fiber"},
        {"toolhead_fiber_protocol_id", ";;;linear-e-v1"}, {"physical_extruder_map", "0,1,2,3"},
        {"toolhead_fiber_e_units_per_mm", "1,1,1,1"}, {"filament_fiber_feed_correction", "1,1,1,1"},
        {"reinforced_infill_filament", "4"}, {"sparse_infill_filament_id", "1"},
        {"internal_solid_filament_id", "2"}, {"top_surface_filament_id", "3"}})
        config.set_deserialize_strict(key, value);
    Model model;
    auto* object = model.add_object();
    object->add_volume(make_cube(60, 40, 2), ModelVolumeType::MODEL_PART, false);
    auto mesh = make_cube(28, 24, 2);
    mesh.translate(Vec3f(16, 8, 0));
    auto* modifier = object->add_volume(std::move(mesh), ModelVolumeType::PARAMETER_MODIFIER, false);
    modifier->config.set("fiber_mask_process", true);
    object->add_instance()->set_offset(Vec3d(100, 100, 0));
    Print print;
    print.set_plate_origin(Vec3d::Zero());
    print.is_BBL_printer() = false;
    print.apply(model, config);
    print.process();
    Layer* selected = nullptr;
    ExtrusionFiberPath* picked = nullptr;
    std::function<void(ExtrusionEntityCollection&)> extract_one = [&](ExtrusionEntityCollection& group) {
        for (auto it = group.entities.begin(); it != group.entities.end() && !picked;) {
            if (auto* fiber = dynamic_cast<ExtrusionFiberPath*>(*it)) {
                picked = fiber;
                it = group.entities.erase(it);
            } else if (auto* child = dynamic_cast<ExtrusionEntityCollection*>(*it)) {
                extract_one(*child);
                if (child->empty()) {
                    delete child;
                    it = group.entities.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
    };
    for (auto* layer : print.objects().front()->layers()) {
        if (layer->id() != 3) continue;
        for (auto* region : layer->regions()) extract_one(region->fills);
        if (picked) selected = layer;
    }
    REQUIRE(selected);
    auto mixed = std::make_unique<ExtrusionEntityCollection>();
    auto ordered = std::make_unique<ExtrusionEntityCollection>();
    ordered->no_sort = true;
    ordered->set_reverse();
    auto directed = std::make_unique<ExtrusionEntityCollection>();
    directed->set_reverse(); // Independent of no_sort: this nested branch cannot reverse.
    const auto add_path = [](ExtrusionEntityCollection& group, ExtrusionRole role,
                             double x0, double y0, double x1, double y1) {
        ExtrusionPath path(role, 0.08, 0.4f, 0.2f);
        path.polyline = Polyline3(Polyline({Point::new_scale(x0, y0), Point::new_scale(x1, y1)}));
        group.append(std::move(path));
    };
    // This order deliberately differs from geometric chaining order.
    add_path(*ordered, erTopSolidInfill, 10, 12, 20, 12);
    add_path(*ordered, erResinInfill, 10, 10, 20, 10);
    add_path(*directed, erTopSolidInfill, 20, 11, 10, 11);
    ordered->entities.push_back(directed.release());
    ordered->entities.push_back(picked);
    mixed->entities.push_back(ordered.release());
    auto* owner = selected->regions().front();
    owner->fills.entities.push_back(mixed.release());
    const Vec2f emitted_origin = unscale(print.objects().front()->instances().front().shift).cast<float>();
    auto& tools = const_cast<LayerTools&>(print.tool_ordering().tools_for_layer(selected->print_z));
    CHECK_FALSE(tools.wiping_extrusions().is_overriddable(
        *static_cast<const ExtrusionEntityCollection*>(owner->fills.entities.back()),
        print.config(), *print.objects().front(), owner->region()));
    // The old merge incorrectly chose tool 1; cover both its presence and absence.
    const bool internal_tool_present = GENERATE(false, true);
    const bool required_tool_missing = GENERATE(false, true);
    tools.extruders = internal_tool_present ? std::vector<unsigned>{0, 1, 2, 3} : std::vector<unsigned>{0, 2, 3};
    if (required_tool_missing)
        tools.extruders.erase(std::find(tools.extruders.begin(), tools.extruders.end(), 2));
    const auto output = std::filesystem::temp_directory_path() /
        ("fiber-mask-role-materials-" + std::to_string(std::random_device{}()) + ".gcode");
    GCodeProcessorResult result;
    if (required_tool_missing) {
        REQUIRE_THROWS_WITH(print.export_gcode(output.string(), &result),
                            "Selected extrusion material is missing from layer tools");
    } else {
        REQUIRE_NOTHROW(print.export_gcode(output.string(), &result));
        std::vector<Vec2f> top_endpoints;
        std::size_t resin = 0, fibers = 0;
        for (const auto& move : result.moves) {
            if (std::abs(move.print_z - selected->print_z) > 1e-4) continue;
            if (move.extrusion_role == erTopSolidInfill && move.type == EMoveType::Extrude) {
                CHECK(move.extruder_id == 2);
                // Native speed ramps can split a single straight path into
                // several moves. Retain its final endpoint without assuming
                // a particular number of ramp segments.
                if (!top_endpoints.empty() && std::abs(top_endpoints.back().y() - move.position.y()) < 1e-4)
                    top_endpoints.back() = move.position.head<2>();
                else
                    top_endpoints.emplace_back(move.position.head<2>());
            } else if (move.extrusion_role == erResinInfill && move.type == EMoveType::Extrude &&
                       std::abs(move.position.y() - (emitted_origin.y() + 10.0f)) < 1e-4) {
                CHECK(move.extruder_id == 0);
                ++resin;
            } else if (move.deposition == ToolpathDeposition::ContinuousFiberPowered ||
                       move.deposition == ToolpathDeposition::ContinuousFiberPassive) {
                CHECK(move.extruder_id == 3);
                ++fibers;
            }
        }
        REQUIRE(top_endpoints.size() == 2);
        INFO("first=" << top_endpoints[0].transpose() << " second=" << top_endpoints[1].transpose());
        CHECK(top_endpoints[0].isApprox(emitted_origin + Vec2f(20, 12), 1e-5f));
        CHECK(top_endpoints[1].isApprox(emitted_origin + Vec2f(10, 11), 1e-5f));
        CHECK(resin > 0);
        CHECK(fibers > 0);
    }
    std::error_code error;
    std::filesystem::remove(output, error);
    std::filesystem::remove(output.string() + ".tmp", error);
}


TEST_CASE("masked first-layer constraints use the native adopted subset", "[ContinuousFiber][fiber-mask][tool-order]")
{
    auto config = masked_print_config();
    config.set_deserialize_strict("bottom_shell_layers", "0");
    for (const auto& [key,value] : std::vector<std::pair<std::string,std::string>>{
        {"filament_map", "1,2,1"}, {"filament_colour", "#FFFFFF;#000000;#FF0000"},
        {"filament_diameter", "1.75,1.75,1.75"},
        {"filament_process_type", "thermoplastic;continuous_fiber;thermoplastic"},
        {"filament_fiber_feed_correction", "1,1,1"},
        {"flush_volumes_matrix", "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"}})
        config.set_deserialize_strict(key,value);
    Model model;
    auto* object=model.add_object();
    object->add_volume(make_cube(60,40,2),ModelVolumeType::MODEL_PART,false);
    auto mesh=make_cube(28,24,2); mesh.translate(Vec3f(16,8,0));
    auto* modifier=object->add_volume(std::move(mesh),ModelVolumeType::PARAMETER_MODIFIER,false);
    modifier->config.set("fiber_mask_process",true);
    modifier->config.set("fiber_width",1.0);
    object->add_instance()->set_offset(Vec3d(100,100,0));
    Print print;
    print.set_plate_origin(Vec3d::Zero()); print.is_BBL_printer()=false;
    print.apply(model,config); print.process();
    auto& full=const_cast<DynamicPrintConfig&>(print.full_print_config());
    full.set_deserialize_strict("first_layer_print_sequence", "2,1");
    ToolOrdering order(print,unsigned(-1));
    REQUIRE_FALSE(order.empty());
    // Exercise the existing full-layer native entry using one-based material
    // IDs, including an automatically available third thermoplastic material.
    for (auto& layer:order.layer_tools()) layer.extruders={1,2,3};
    SECTION("two configured values do not sort a three-material first layer") {
        order.handle_dontcare_extruder(unsigned(-1));
        REQUIRE_NOTHROW(order.sort_and_build_data(print,unsigned(-1)));
        CHECK(order.front().extruders.back()==1);
    }
    SECTION("an adopted fiber-before-resin subset is an explicit conflict") {
        full.set_deserialize_strict("first_layer_print_sequence", "2,1,3");
        order.handle_dontcare_extruder(unsigned(-1));
        REQUIRE_THROWS_WITH(order.sort_and_build_data(print,unsigned(-1)),
            "Explicit material order conflicts with masked resin-before-fiber execution");
    }
    SECTION("an appended unranked material is not an explicit conflict") {
        full.set_deserialize_strict("first_layer_print_sequence", "2,9,8");
        order.handle_dontcare_extruder(unsigned(-1));
        REQUIRE_NOTHROW(order.sort_and_build_data(print,unsigned(-1)));
        CHECK(order.front().extruders.back()==1);
    }
}

TEST_CASE("masked flush statistics describe the published constrained sequences", "[ContinuousFiber][fiber-mask][tool-order]")
{
    auto config=masked_print_config();
    config.set_deserialize_strict("single_extruder_multi_material","1");
    config.set_deserialize_strict("purge_in_prime_tower","1");
    config.set_deserialize_strict("flush_volumes_matrix","0,800,120,0,0,400,90,0");
    config.set_deserialize_strict("filament_density","1.25,2.0");
    config.set_deserialize_strict("flush_multiplier","1,1");
    Model model;
    auto* object=model.add_object();
    object->add_volume(make_cube(60,40,2),ModelVolumeType::MODEL_PART,false);
    auto mesh=make_cube(28,24,2); mesh.translate(Vec3f(16,8,0));
    auto* modifier=object->add_volume(std::move(mesh),ModelVolumeType::PARAMETER_MODIFIER,false);
    modifier->config.set("fiber_mask_process",true);
    modifier->config.set("fiber_width",1.0);
    object->add_instance()->set_offset(Vec3d(100,100,0));
    Print print;
    print.set_plate_origin(Vec3d::Zero()); print.is_BBL_printer()=false;
    print.apply(model,config); print.process();
    ToolOrdering order(print,unsigned(-1));
    // Construction only collects native tools. Optimization/statistics require
    // the same explicit sort entry that Print's normal pipeline calls.
    order.sort_and_build_data(print,unsigned(-1));
    std::set<coordf_t> resin_first_layers;
    const std::function<bool(const ExtrusionEntity*)> needs_resin_first=[&](const ExtrusionEntity* entity) {
        if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(entity))
            return std::any_of(collection->entities.begin(),collection->entities.end(),needs_resin_first);
        if(const auto* fiber=dynamic_cast<const ExtrusionFiberPath*>(entity)) {
            const auto& boundary=fiber->prepared_path()->policy_boundary;
            return boundary && boundary->requires_resin_first;
        }
        return false;
    };
    for(const auto* layer:print.objects().front()->layers())
        for(const auto* region:layer->regions())
            if(std::any_of(region->fills.entities.begin(),region->fills.entities.end(),needs_resin_first))
                resin_first_layers.insert(layer->print_z);
    REQUIRE_FALSE(resin_first_layers.empty());
    const auto check_stats=[&] {
        int last=-1, changes=0;
        int volume_by_destination[2]={0,0};
        for(const auto& layer:order) {
            if(resin_first_layers.count(layer.print_z) && layer.extruders.size()==2)
                CHECK(layer.extruders.back()==1);
            for(const unsigned material:layer.extruders) {
                REQUIRE(material<2);
                if(last>=0 && unsigned(last)!=material) {
                    ++changes;
                    volume_by_destination[material]+=material==1?800:120;
                }
                last=int(material);
            }
        }
        REQUIRE(changes>0);
        const int weight=int(float(volume_by_destination[0])*.001f*1.25f+
                             float(volume_by_destination[1])*.001f*2.0f);
        const auto single=order.get_filament_change_stats(ToolOrdering::SingleExt);
        CHECK(single.filament_change_count==changes);
        CHECK(single.filament_flush_weight==weight);
        for(const auto mode:{ToolOrdering::MultiExtCurr,ToolOrdering::MultiExtBest}) {
            const auto separate=order.get_filament_change_stats(mode);
            CHECK(separate.filament_change_count==0);
            CHECK(separate.filament_flush_weight==0);
        }
    };
    check_stats();
    // A second optimization uses the same Print and must replace its stats.
    order.sort_and_build_data(print,unsigned(-1));
    check_stats();
    auto& mutable_config=const_cast<PrintConfig&>(print.config());
    // The prime-tower tool is absent from the initial solid first layer.
    // Insertion forces the existing second reorder, which must republish stats.
    REQUIRE_FALSE(order.front().has_extruder(1));
    mutable_config.enable_prime_tower.value=true;
    mutable_config.wipe_tower_filament.value=2;
    order.sort_and_build_data(print,unsigned(-1));
    REQUIRE(order.front().has_extruder(1));
    check_stats();
    // Mapping failure takes the existing early return, after policy constraints.
    mutable_config.filament_map.values.clear();
    for(auto& layer:order.layer_tools())
        if(resin_first_layers.count(layer.print_z) && layer.extruders.size()==2)
            layer.extruders={1,0};
    REQUIRE_NOTHROW(order.sort_and_build_data(print,unsigned(-1)));
    for(const auto& layer:order)
        if(resin_first_layers.count(layer.print_z) && layer.extruders.size()==2)
            CHECK(layer.extruders.back()==1);
    for(const auto mode:{ToolOrdering::SingleExt,ToolOrdering::MultiExtCurr,ToolOrdering::MultiExtBest}) {
        const auto empty=order.get_filament_change_stats(mode);
        CHECK(empty.filament_change_count==0);
        CHECK(empty.filament_flush_weight==0);
    }
    mutable_config.other_layers_print_sequence_nums.value=1;
    mutable_config.other_layers_print_sequence.values={1,10,2,1};
    REQUIRE_THROWS_WITH(order.sort_and_build_data(print,unsigned(-1)),
        "Explicit material order conflicts with masked resin-before-fiber execution");
}

namespace {
ContinuousFiberConfig open_ring_config()
{
    ContinuousFiberConfig config;
    config.contour_enabled=true;config.contour_count=3;config.contour_include_holes=false;
    config.contour_flow=Flow(1.f,.2f,.4f);config.infill_flow=config.contour_flow;
    config.connect_adjacent_rings=true;
    config.connection_tail_trim_widths=1.5;config.connection_head_trim_widths=1;
    return config;
}
FiberContourPlanResult open_ring_plan(const ContinuousFiberConfig& config,
    const ExPolygons& domain={rectangle(0,0,50,40)})
{
    return FiberPathValidator::plan_contours(domain,config,{7,11,2,0},true);
}
size_t open_ring_bridges(const FiberChainExecution& execution)
{
    return std::count_if(execution.pieces.begin(),execution.pieces.end(),[](const auto& piece){
        return piece.kind==FiberExecutionPieceKind::Transition;
    });
}
void check_open_ring_sources(const FiberContourPlanResult& plan)
{
    REQUIRE(plan.validation.audit_open_chains());
    std::set<FiberFragmentId> seen;
    size_t expected=0;for(const auto& source:plan.validation.assignments)
        if(source.kind==FiberAssignmentKind::AcceptedFiber)++expected;
    for(const auto& chain:plan.validation.chain_executions) {
        REQUIRE_FALSE(chain.sources.empty());REQUIRE(chain.prepared);
        CHECK(open_ring_bridges(chain)+1==chain.sources.size());
        if(chain.sources.size()>1)CHECK(chain.centerline.polyline.points.front()!=chain.centerline.polyline.points.back());
        for(size_t index=0;index<chain.sources.size();++index) {
            const auto& source=chain.sources[index];
            CHECK(seen.insert(source.source).second);
            CHECK(source.head_mm==Catch::Approx(index?1.:0.));
            CHECK(source.tail_mm==Catch::Approx(index+1<chain.sources.size()?1.5:0.));
            CHECK(source.retained_mm+source.head_mm+source.tail_mm==Catch::Approx(source.original_mm).margin(1e-5));
            REQUIRE(source.retained_intervals.size()<=2);
            double retained=0;
            const auto frozen=std::find_if(plan.validation.assignments.begin(),plan.validation.assignments.end(),[&](const auto& a){return a.id==source.source;});
            REQUIRE(frozen!=plan.validation.assignments.end());
            for(const auto& interval:source.retained_intervals) {
                CHECK(std::isfinite(interval.first));CHECK(std::isfinite(interval.second));
                CHECK(interval.first>=frozen->source_begin_mm);CHECK(interval.second<=frozen->source_end_mm);
                CHECK(interval.second>interval.first);retained+=interval.second-interval.first;
            }
            CHECK(retained==Catch::Approx(source.retained_mm).margin(1e-5));
        }
        // Every command edge has one provenance owner, including the explicit finish.
        std::vector<std::vector<int>> owners;
        for(const auto& span:chain.prepared->spans)owners.emplace_back(span.edges.size(),0);
        if(chain.sources.size()>1) {
            REQUIRE_FALSE(chain.prepared->command_pieces.empty());
            for(const auto& piece:chain.prepared->command_pieces) {
                REQUIRE(piece.span_index<owners.size());
                REQUIRE(piece.end_edge<=owners[piece.span_index].size());
                CHECK(piece.begin_edge<piece.end_edge);
                CHECK(piece.origin.source_end_mm>=piece.origin.source_begin_mm);
                for(size_t edge=piece.begin_edge;edge<piece.end_edge;++edge)++owners[piece.span_index][edge];
            }
            for(const auto& span:owners)for(int count:span)CHECK(count==1);
        }
    }
    CHECK(seen.size()==expected);
}
}

TEST_CASE("open ring chain retains one forward arc and one local seam per source", "[ContinuousFiber][OpenRingConnection]")
{
    const auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==3);
    const auto frozen=plan.validation.assignments;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE(plan.validation.chain_executions.size()==1);
    const auto& chain=plan.validation.chain_executions.front();
    REQUIRE(chain.sources.size()==3);CHECK(open_ring_bridges(chain)==2);
    check_open_ring_sources(plan);
    for(size_t i=0;i<frozen.size();++i) {
        CHECK(plan.validation.assignments[i].centerline->polyline.points==frozen[i].centerline->polyline.points);
        CHECK(plan.validation.assignments[i].prepared==frozen[i].prepared);
    }
    // Release the chain once; immutable three-source generation records are not three emitters.
    ExtrusionEntitiesPtr output;plan.validation.release_to(output);REQUIRE(output.size()==1);
    const auto* emitted=dynamic_cast<const ExtrusionEntityCollection*>(output.front());
    REQUIRE(emitted);CHECK(emitted->entities.size()==1);
    for(auto* entity:output)delete entity;
}

TEST_CASE("execution connection audit retains independent obstacles whenever a connected route exists",
    "[ContinuousFiber][OpenRingConnection][ConnectionAudit]")
{
    const int scenario=GENERATE(0,1,2,3);
    CAPTURE(scenario);
    auto config=open_ring_config();config.cut_to_contact_length_mm=2;
    auto plan=open_ring_plan(config);FiberPathValidator::plan_open_ring_chains(plan,{rectangle(0,0,50,40)},config,true);
    REQUIRE(plan.validation.chain_executions.size()==1);
    const auto connected=plan.validation.chain_executions.front().prepared;
    REQUIRE(connected);REQUIRE_FALSE(connected->command_pieces.empty());REQUIRE_FALSE(connected->entry.lowers());
    auto context=fiber_fixture_context();
    const auto compile=[&](const std::shared_ptr<const PreparedFiberPath>& geometry) {
        CompiledFiberPath result;result.geometry=geometry;result.context=context;
        result.variants.push_back(std::make_shared<const FrozenFiberExecution>(
            compile_fiber_execution(geometry->draft(),*context,context->placements.front(),.2)));
        return result;
    };
    auto independent_id=test_id();independent_id.parent.domain=connected->id.parent.domain;
    independent_id.parent.purpose=FiberPathPurpose::Infill;
    ContinuousFiberConfig independent_config;independent_config.cut_to_contact_length_mm=2;
    const auto independent=finalize_independent(straight_path(60,10,90,10),
        {rectangle(55,5,95,15)},independent_config,independent_id).prepared;
    REQUIRE(independent);REQUIRE(independent->command_pieces.empty());
    std::map<FiberFragmentId,CompiledFiberPath> paths;
    paths.emplace(independent->id,compile(independent));
    if(scenario==0) {
        CHECK_NOTHROW(audit_fiber_execution_connections({}));
        // The pre-existing contract ignores an entry with no variants, even
        // without a geometry pointer. Do not dereference that unused entry.
        auto empty_id=independent_id;empty_id.parent.domain.policy_group_id+=100;
        paths.emplace(empty_id,CompiledFiberPath{});
        CHECK_NOTHROW(audit_fiber_execution_connections(paths));
        return;
    }
    paths.emplace(connected->id,compile(connected));
    REQUIRE(paths.size()==2);
    CHECK_NOTHROW(audit_fiber_execution_connections(paths));
    if(scenario==1) {
        auto changed=std::make_shared<FrozenFiberExecution>(*paths.at(independent->id).variants.front());
        changed->command_origin.x()-=60; // Source remains outside; actual moves across the ring.
        paths.at(independent->id).variants={changed};
        CHECK_THROWS_AS(audit_fiber_execution_connections(paths),FiberExecutionError);
    }
    if(scenario==2) {
        auto changed=std::make_shared<FrozenFiberExecution>(*paths.at(independent->id).variants.front());
        changed->command_origin.x()-=60;changed->placement.instance_id+=1;
        paths.at(independent->id).variants={changed};
        CHECK_NOTHROW(audit_fiber_execution_connections(paths));
    }
    if(scenario==3) {
        auto changed=std::make_shared<FrozenFiberExecution>(*paths.at(connected->id).variants.front());
        auto commands=std::make_shared<FrozenFiberCommands>(*changed->commands);
        const auto last=std::find_if(commands->spans.rbegin(),commands->spans.rend(),
            [](const auto& span){return span.kind!=FiberMotionKind::NonDepositingFinish && !span.commands.empty();});
        REQUIRE(last!=commands->spans.rend());last->commands.back().target=commands->spans.front().from;
        changed->commands=commands;paths.at(connected->id).variants={changed};
        CHECK_THROWS_AS(audit_fiber_execution_connections(paths),FiberExecutionError);
    }
}

TEST_CASE("open ring connection off and no candidates preserve original prepared paths", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();
    SECTION("disabled") {config.connect_adjacent_rings=false;}
    SECTION("one source") {config.contour_count=1;}
    const ExPolygons domain{rectangle(0,0,50,40)};auto plan=open_ring_plan(config,domain);
    const auto original=plan.validation.assignments;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    CHECK(plan.validation.chain_executions.empty());
    REQUIRE(plan.validation.assignments.size()==original.size());
    for(size_t i=0;i<original.size();++i)CHECK(plan.validation.assignments[i].prepared==original[i].prepared);
    ExtrusionEntitiesPtr output;plan.validation.release_to(output);REQUIRE(output.size()==1);
    const auto* emitted=dynamic_cast<const ExtrusionEntityCollection*>(output.front());
    REQUIRE(emitted);CHECK(emitted->entities.size()==original.size());for(auto* entity:output)delete entity;
}

TEST_CASE("open ring rejected extension restores the terminal role and untouched child", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);REQUIRE(plan.validation.assignments.size()==3);
    // A child with a different material must not borrow the inherited seam or receive a bridge.
    auto& foreign=plan.validation.assignments.back();
    auto foreign_config=config;foreign_config.contour_material=foreign.prepared->logical_filament_id+2;
    const auto foreign_result=finalize_independent(*foreign.centerline,domain,foreign_config,foreign.id);
    REQUIRE(foreign_result.prepared);foreign.prepared=foreign_result.prepared;
    REQUIRE(plan.audit_lineage());
    const auto frozen=foreign.prepared;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE_FALSE(plan.validation.chain_executions.empty());
    size_t bridges=0;for(const auto& chain:plan.validation.chain_executions) {
        bridges+=open_ring_bridges(chain);
        if(chain.sources.size()>1) {CHECK(chain.sources.size()==2);CHECK(chain.sources.back().tail_mm==0);}
        for(const auto& piece:chain.pieces)if(piece.kind==FiberExecutionPieceKind::Transition)
            CHECK(piece.other_source.parent.domain.policy_group_id==piece.source.parent.domain.policy_group_id);
    }
    CHECK(bridges==1);
    CHECK(plan.validation.assignments.back().prepared==frozen);
}

TEST_CASE("open ring excessive clipping never leaves ghost cut windows", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);const auto original=plan.validation.assignments;
    config.connection_tail_trim_widths=1000;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    CHECK(plan.validation.chain_executions.empty());
    for(const auto& decision:plan.chain_decisions)CHECK_FALSE(decision.published);
    for(size_t i=0;i<original.size();++i)CHECK(plan.validation.assignments[i].prepared==original[i].prepared);
}

TEST_CASE("open ring planning tail crosses bridge once and finish uses terminal forward source", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();config.finish_overlap_length_mm=3;
    config.contour_count=2;
    // Terminal perimeter 20mm <23mm: the passive tail must extend back across its incoming bridge.
    const ExPolygons domain{rectangle(0,0,8,8)};auto plan=open_ring_plan(config,domain);
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==2);
    // Synthetic already accepted sources isolate whole-execution budget splitting.
    config.cut_to_contact_length_mm=23;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE(plan.validation.chain_executions.size()==1);const auto& chain=plan.validation.chain_executions.front();
    REQUIRE(chain.sources.size()==2);REQUIRE(chain.prepared);
    const auto& prepared=*chain.prepared;
    CHECK(std::any_of(prepared.command_pieces.begin(),prepared.command_pieces.end(),[](const auto& piece) {
        return piece.end_edge-piece.begin_edge>1;
    }));
    CHECK(prepared.passive_tail_length_mm()==Catch::Approx(23).margin(.002));
    CHECK(std::count_if(prepared.actions.begin(),prepared.actions.end(),[](const auto& a){return a.type==FiberActionType::Cut;})==1);
    bool tail_bridge=false;std::set<FiberFragmentId> tail_sources;
    double finish_length=0;
    for(const auto& piece:prepared.command_pieces) {
        const auto& span=prepared.spans.at(piece.span_index);
        if(span.kind==FiberMotionKind::PassiveDepositingAfterCut) {
            tail_bridge|=piece.origin.kind==FiberExecutionPieceKind::Transition;
            if(piece.origin.kind==FiberExecutionPieceKind::RetainedSource)tail_sources.insert(piece.origin.source);
        }
        if(span.kind==FiberMotionKind::NonDepositingFinish) {
            CHECK(piece.origin.source==chain.sources.back().source);
            CHECK(piece.origin.kind==FiberExecutionPieceKind::RetainedSource);
            finish_length+=piece.command_end_mm-piece.command_begin_mm;
        }
    }
    CHECK(tail_bridge);CHECK(tail_sources.size()==2);CHECK(finish_length==Catch::Approx(3).margin(.002));
    CHECK(prepared.finish_strategy==FiberFinishStrategy::LoopOverlap);
    REQUIRE(prepared.spans.back().kind==FiberMotionKind::NonDepositingFinish);
    CHECK(prepared.spans.back().geometry.points.front()==chain.centerline.polyline.points.back());
    for(const auto& span:prepared.spans)if(span.kind==FiberMotionKind::PassiveDepositingAfterCut || span.kind==FiberMotionKind::NonDepositingFinish)
        for(const auto& edge:span.edges)CHECK(edge.feed_mm_per_xy_mm==0);
    check_open_ring_sources(plan);
}

TEST_CASE("open ring software trim policy is inactive while connections are disabled", "[ContinuousFiber][OpenRingConnection]")
{
    auto a=open_ring_config(),b=a;a.connect_adjacent_rings=false;b.connect_adjacent_rings=false;
    b.connection_tail_trim_widths=4;b.connection_head_trim_widths=0;
    CHECK(fiber_policy_key(a,0,true)==fiber_policy_key(b,0,true));
    b.connect_adjacent_rings=true;CHECK_FALSE(fiber_policy_key(a,0,true)==fiber_policy_key(b,0,true));
    a=b;a.connection_head_trim_widths=1;
    CHECK_FALSE(fiber_policy_key(a,0,true)==fiber_policy_key(b,0,true));
}

TEST_CASE("open ring branch forms separate chains without reopening an occupied parent", "[ContinuousFiber][OpenRingConnection]")
{
    const auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);REQUIRE(plan.nodes.size()==3);
    // Both are direct children, but the open path has capacity for one successor only.
    plan.nodes[2].parent=plan.nodes[0].id;plan.nodes[2].depth=1;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE(plan.validation.chain_executions.size()==2);
    size_t bridges=0;std::set<FiberFragmentId> seen;
    for(const auto& chain:plan.validation.chain_executions) {
        bridges+=open_ring_bridges(chain);
        for(const auto& source:chain.sources)CHECK(seen.insert(source.source).second);
    }
    CHECK(bridges==1);CHECK(seen.size()==3);CHECK(plan.validation.audit_open_chains());
}

TEST_CASE("open ring source station bounds remain strict on noninteger geometry", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();config.contour_bend_radius_mm=.37;
    auto shape=rectangle(0,0,50.123,40.317);
    const ExPolygons domain{shape};auto plan=open_ring_plan(config,domain);
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==3);
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE_FALSE(plan.validation.chain_executions.empty());check_open_ring_sources(plan);
    CHECK(std::count_if(plan.chain_decisions.begin(),plan.chain_decisions.end(),[](const auto& d){return d.published;})==2);
}

TEST_CASE("open ring invalid terminal finish restores the entire domain", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);const auto original=plan.validation.assignments;
    config.finish_overlap_length_mm=1000;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    CHECK(plan.validation.chain_executions.empty());
    for(const auto& decision:plan.chain_decisions)CHECK_FALSE(decision.published);
    for(size_t i=0;i<original.size();++i)CHECK(plan.validation.assignments[i].prepared==original[i].prepared);
}

TEST_CASE("open ring zero finish retains explicit terminal LoopOverlap strategy", "[ContinuousFiber][OpenRingConnection]")
{
    const auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE(plan.validation.chain_executions.size()==1);
    const auto& prepared=*plan.validation.chain_executions.front().prepared;
    CHECK(prepared.finish_strategy==FiberFinishStrategy::LoopOverlap);
    for(const auto& span:prepared.spans)CHECK(span.kind!=FiberMotionKind::NonDepositingFinish);
}

TEST_CASE("open chain bridge corners do not invent radius-treated stability events", "[ContinuousFiber][OpenRingConnection][OpenChainStability]")
{
    auto config=open_ring_config();config.contour_count=2;
    const ExPolygons domain{rectangle(0,0,50,40)};auto plan=open_ring_plan(config,domain);
    plan.set_display_cutoff(0);
    for(const auto& source:plan.validation.assignments)REQUIRE(source.source_bends.empty());
    config.concentric_corner_stabilization_length_mm=10000;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE(plan.validation.chain_executions.size()==1);
    CHECK(plan.validation.chain_executions.front().sources.size()==2);
    CHECK(open_ring_bridges(plan.validation.chain_executions.front())==1);
}

TEST_CASE("open chain stability clips the original marked region at its true retained start", "[ContinuousFiber][OpenRingConnection][OpenChainStability]")
{
    auto config=open_ring_config();config.contour_count=2;
    const ExPolygons domain{rectangle(0,0,50,40)};
    auto frozen=open_ring_plan(config,domain);frozen.set_display_cutoff(0);
    auto reference=frozen;FiberPathValidator::plan_open_ring_chains(reference,domain,config,true);
    REQUIRE(reference.validation.chain_executions.size()==1);
    const auto& head=reference.validation.chain_executions.front().sources.front();
    REQUIRE(head.seam_mm>.5);REQUIRE(head.seam_mm+.5<head.original_mm);
    auto& source=*std::find_if(frozen.validation.assignments.begin(),frozen.validation.assignments.end(),[&](const auto& source){return source.id==head.source;});
    // Controlled metadata-interface fixture: these intervals deliberately mark a
    // region around the chosen seam. They do not claim the square was radius-treated.
    ContourBend bend;bend.begin_mm=head.seam_mm-.5;bend.end_mm=head.seam_mm+.5;
    source.source_bends={bend};config.concentric_corner_stabilization_length_mm=.1;
    SECTION("Infill first retained fragment starts inside bend and has zero prefix") {
        FiberPathValidator::plan_open_ring_chains(frozen,domain,config,true);
        CHECK(frozen.validation.chain_executions.empty());
        REQUIRE_FALSE(frozen.chain_decisions.empty());
        CHECK(std::any_of(frozen.chain_decisions.begin(),frozen.chain_decisions.end(),[](const auto& decision){
            return !decision.published && decision.reason.find("bend_stabilization_too_short")!=std::string::npos;
        }));
    }
    SECTION("the identical Contour mark does not acquire an Infill threshold") {
        source.display_purpose=FiberPathPurpose::Contour;
        FiberPathValidator::plan_open_ring_chains(frozen,domain,config,true);
        REQUIRE(frozen.validation.chain_executions.size()==1);
        CHECK(frozen.validation.chain_executions.front().sources.size()==2);
    }
    SECTION("a completely trimmed marked region is not a predecessor event") {
        source.source_bends.front().begin_mm=head.seam_mm-1;
        source.source_bends.front().end_mm=head.seam_mm-.5;
        config.concentric_corner_stabilization_length_mm=10000;
        FiberPathValidator::plan_open_ring_chains(frozen,domain,config,true);
        REQUIRE(frozen.validation.chain_executions.size()==1);
        CHECK(frozen.validation.chain_executions.front().sources.size()==2);
    }
}

TEST_CASE("open chain first-edge rejection leaves no inherited seam on the next root", "[ContinuousFiber][OpenRingConnection]")
{
    const auto config=open_ring_config();const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);
    REQUIRE(plan.validation.assignments.size()==3);
    // Keep the second edge eligible while blocking the first through its real
    // source identity. No failed parent contact may initialize source1's seam.
    auto& first=plan.validation.assignments.front();
    auto first_config=config;first_config.contour_material=first.prepared->logical_filament_id+2;
    const auto first_result=finalize_independent(*first.centerline,domain,first_config,first.id);
    REQUIRE(first_result.prepared);first.prepared=first_result.prepared;
    REQUIRE(plan.audit_lineage());
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE_FALSE(plan.validation.chain_executions.empty());
    size_t bridges=0;
    for(const auto& execution:plan.validation.chain_executions)if(execution.sources.size()>1) {
        bridges+=open_ring_bridges(execution);CHECK(execution.sources.size()==2);
        CHECK(execution.sources.front().head_mm==0);
        CHECK(execution.sources.back().tail_mm==0);
    }
    CHECK(bridges==1);
    for(const auto& decision:plan.chain_decisions)if(decision.published)CHECK_FALSE(decision.inherited_seam);
}

TEST_CASE("open chain subpicometer positive retained arc safely preserves independent sources", "[ContinuousFiber][OpenRingConnection][TinyRetainedArc]")
{
    auto config=open_ring_config();config.contour_count=2;
    const ExPolygons domain{rectangle(0,0,50,40)};
    auto plan=open_ring_plan(config,domain);INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==2);
    // Establish that this same pair is connectable with ordinary parameters.
    auto ordinary=plan;FiberPathValidator::plan_open_ring_chains(ordinary,domain,config,true);
    REQUIRE(ordinary.validation.chain_executions.size()==1);
    REQUIRE(ordinary.validation.chain_executions.front().sources.size()==2);
    const auto head_id=ordinary.validation.chain_executions.front().sources.front().source;
    const auto head=std::find_if(plan.validation.assignments.begin(),plan.validation.assignments.end(),
        [&](const auto& source){return source.id==head_id;});
    REQUIRE(head!=plan.validation.assignments.end());REQUIRE(head->centerline);
    REQUIRE(head->centerline->width==1.f);
    const double original=head->source_end_mm-head->source_begin_mm;
    config.connection_tail_trim_widths=std::nextafter(original,0.0);
    const double tiny_retained=original-config.connection_tail_trim_widths*head->centerline->width;
    REQUIRE(std::isfinite(config.connection_tail_trim_widths));
    REQUIRE(config.connection_tail_trim_widths>=0);
    REQUIRE(std::isfinite(config.connection_head_trim_widths));
    REQUIRE(config.connection_head_trim_widths>=0);
    REQUIRE(tiny_retained>0);REQUIRE(tiny_retained<1e-12);
    const auto frozen=plan.validation.assignments;
    REQUIRE_NOTHROW(FiberPathValidator::plan_open_ring_chains(plan,domain,config,true));
    CHECK(plan.validation.chain_executions.empty());
    REQUIRE_FALSE(plan.chain_decisions.empty());
    for(const auto& decision:plan.chain_decisions)CHECK_FALSE(decision.published);
    REQUIRE(plan.validation.assignments.size()==frozen.size());
    for(size_t i=0;i<frozen.size();++i) {
        CHECK(plan.validation.assignments[i].prepared==frozen[i].prepared);
        CHECK(plan.validation.assignments[i].centerline->polyline.points==frozen[i].centerline->polyline.points);
        CHECK(plan.validation.assignments[i].source_begin_mm==frozen[i].source_begin_mm);
        CHECK(plan.validation.assignments[i].source_end_mm==frozen[i].source_end_mm);
    }
    ExtrusionEntitiesPtr output;plan.validation.release_to(output);
    REQUIRE(output.size()==1);
    const auto* emitted=dynamic_cast<const ExtrusionEntityCollection*>(output.front());
    REQUIRE(emitted);CHECK(emitted->entities.size()==frozen.size());for(auto* entity:output)delete entity;
}

TEST_CASE("open ring rotated corner gate follows the analytic retained child", "[ContinuousFiber][OpenRingConnection]")
{
    auto config=open_ring_config();config.contour_bend_radius_mm=.37;
    auto shape=rectangle(0,0,50.123,40.317);shape.rotate(.217);
    const ExPolygons domain{shape};auto plan=open_ring_plan(config,domain);
    INFO(rejection_details(plan.validation));REQUIRE(plan.validation.accepted_count()==3);
    const auto original=plan.validation.assignments;
    FiberPathValidator::plan_open_ring_chains(plan,domain,config,true);
    REQUIRE_FALSE(plan.validation.chain_executions.empty());
    CHECK(plan.validation.audit_open_chains());CHECK(plan.validation.audit_assignments().valid());
    REQUIRE(plan.chain_decisions.size()==2);
    CHECK(std::any_of(plan.chain_decisions.begin(),plan.chain_decisions.end(),[](const auto& d){return d.published;}));
    std::vector<Polyline> routes;
    for(const auto& chain:plan.validation.chain_executions)
        routes.push_back(Geometry::sample_with_arcs(chain.centerline.polyline,.000005).to_polyline());
    bool crossing=false;
    for(size_t a=0;a<routes.size();++a)for(size_t b=a;b<routes.size();++b) {
        const auto& p=routes[a].points;const auto& q=routes[b].points;
        for(size_t i=1;i<p.size();++i)for(size_t j=(a==b?i+2:1);j<q.size();++j) {
            if(a==b && i==1 && j==p.size()-1 && p.front()==p.back())continue;
            if(std::max(p[i-1].x(),p[i].x())<std::min(q[j-1].x(),q[j].x()) ||
               std::max(q[j-1].x(),q[j].x())<std::min(p[i-1].x(),p[i].x()) ||
               std::max(p[i-1].y(),p[i].y())<std::min(q[j-1].y(),q[j].y()) ||
               std::max(q[j-1].y(),q[j].y())<std::min(p[i-1].y(),p[i].y()))continue;
            crossing|=Geometry::segments_intersect(p[i-1],p[i],q[j-1],q[j]);
        }
    }
    CHECK_FALSE(crossing);
    for(size_t i=0;i<original.size();++i)
        CHECK(plan.validation.assignments[i].prepared==original[i].prepared);
}

TEST_CASE("connected coordinate precision restores after exceptions without losing emitted Z", "[ContinuousFiber][OpenRingConnection][writer]")
{
    PrintConfig config;GCodeWriter writer,other;
    writer.apply_print_config(config);writer.set_extruders({0});writer.toolchange(0);
    writer.set_position(Vec3d(0,0,.2));
    std::string precise,feed;
    REQUIRE_THROWS_AS(([&] {
        GCodeWriter::ScopedXYZPrecision scope(writer,6);
        CHECK(other.xyz_export_digits()==3);
        precise=writer.extrude_to_xyz(Vec3d(1.123456,2.765432,.200499),1.2345678);
        feed=writer.set_speed(1234.56789);
        throw std::runtime_error("abort connected export");
    })(),std::runtime_error);
    CHECK(precise.find("X1.123456")!=std::string::npos);
    CHECK(precise.find("Y2.765432")!=std::string::npos);
    CHECK(precise.find(" Z.200499")!=std::string::npos);
    CHECK(precise.find("E1.23457")!=std::string::npos);
    CHECK(feed.find("F1234.568")!=std::string::npos);
    CHECK(writer.xyz_export_digits()==3);
    CHECK(writer.get_emitted_position().z()==Catch::Approx(.200499));
    // A format restore sends no move. The next ordinary command must really
    // lower from the last six-place Z, rather than suppressing that correction.
    const auto ordinary=writer.extrude_to_xyz(Vec3d(1.123456,2.765432,.2),0,"",true);
    CHECK(ordinary.find("X1.123 ")!=std::string::npos);
    CHECK(ordinary.find("Y2.765 ")!=std::string::npos);
    CHECK(ordinary.find(" Z.2")!=std::string::npos);
    CHECK(ordinary.find(" E")==std::string::npos);
    CHECK(writer.get_emitted_position().z()==Catch::Approx(.2));
}


// Historical commands remain immutable evidence. Current landing deliberately
// follows an independent route length, while retaining prefeed/dwell/feed state.
TEST_CASE("fiber route landing preserves historical prefeed and dwell procedure", "[ContinuousFiber][Landing]")
{
    std::ifstream input(std::filesystem::path(TEST_DATA_DIR)/"continuous_fiber/legacy_landing_4xiao.json");
    REQUIRE(input.good());nlohmann::json fixture;input>>fixture;REQUIRE(fixture.at("rows").size()==45);
    for(const auto& row:fixture.at("rows")) {
        const auto& a=row.at("source_start_xy");const auto& b=row.at("source_first_end_xy");
        auto candidate=path_from_points({{a[0],a[1]},{b[0],b[1]},{b[0].get<double>()+35,b[1]}});
        ContinuousFiberConfig config;config.z_hop_height_mm=fixture.at("H");config.landing_length_mm=2;
        config.cut_to_contact_length_mm=fixture.at("D");config.prefeed_extra_length_mm=fixture.at("extra");
        config.landing_speed_mm_s=row.at("speed");config.adhesion_dwell_ms=fixture.at("dwell_ms");
        const auto result=finalize_independent(candidate,{rectangle(0,0,500,500)},config,test_id());
        INFO(result.detail);REQUIRE(result.prepared);
        CHECK(result.prepared->entry.end_station_mm==2);
        const auto bound=compile_fixture(result.prepared,0,0,0,1,"S0");
        CHECK(bound.prefeed_dE==Catch::Approx(row.at("prefeed").get<double>()).margin(.000005));
        CHECK(bound.prefeed_F==600);
        const auto& actions=result.prepared->actions;
        const auto landing=std::find_if(actions.begin(),actions.end(),[](const auto& action){return action.type==FiberActionType::LandingSpan;});
        REQUIRE(landing!=actions.end());REQUIRE(std::distance(landing,actions.end())>2);
        CHECK((landing+1)->type==FiberActionType::AdhesionDwell);CHECK((landing+2)->type==FiberActionType::Start);
        CHECK(result.prepared->start_procedure.adhesion_dwell_ms==0);
    }
}

TEST_CASE("landing path length is independent of source edges and velocity sampling", "[ContinuousFiber][LegacyLanding]")
{
    const auto first=GENERATE(.741,2.,5.);
    const auto split=GENERATE(0.,.5);
    const auto hop=GENERATE(0.,2.);
    const auto step=GENERATE(.13,1.7);
    auto candidate=path_from_points({{10,10},{10+first,10},{70,10}});
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;config.z_hop_height_mm=hop;
    config.landing_length_mm=split;config.speed_sampling_length_mm=step;
    config.start_stabilization_length_mm=3;
    const auto result=finalize_independent(candidate,{rectangle(0,0,80,20)},config,test_id());
    REQUIRE(result.prepared);
    const double expected=hop>0?split:0;
    CHECK(result.prepared->entry.end_station_mm==Catch::Approx(expected).margin(.000001));
    CHECK(unscale<double>(result.prepared->entry.contact.x())==Catch::Approx(10+expected).margin(.000001));
    CHECK(result.prepared->total_depositing_length_mm()==Catch::Approx(60).margin(.002));
    CHECK(result.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.002));
    const auto& actions=result.prepared->actions;
    const auto dwell=std::find_if(actions.begin(),actions.end(),[](const auto& e){return e.type==FiberActionType::AdhesionDwell;});
    REQUIRE(dwell!=actions.end());REQUIRE(dwell!=actions.begin());
    if(hop==0) {
        CHECK((dwell-1)->type==FiberActionType::MotionSpan);
        CHECK(result.prepared->spans.front().kind==FiberMotionKind::PoweredEntry);
        for(const auto& edge:result.prepared->spans.front().edges) {
            CHECK(edge.speed_mm_s>0);
            CHECK(edge.speed_mm_s<=config.start_speed_mm_s);
        }
    } else CHECK((dwell-1)->type==(split>0?FiberActionType::LandingSpan:FiberActionType::LowerToLayer));
}

TEST_CASE("landing ignores duplicate and machine-rounded zero first source edges", "[ContinuousFiber][LegacyLanding]")
{
    const auto first=GENERATE(0.,.0001);
    ContinuousFiberConfig config;config.z_hop_height_mm=2;config.cut_to_contact_length_mm=23;
    const auto candidate=path_from_points({{10,10},{10+first,10},{60,10}});
    const auto result=finalize_independent(candidate,{rectangle(0,0,70,20)},config,test_id());
    CAPTURE(first);INFO(result.detail);REQUIRE(result.prepared);CHECK(result.prepared->entry.end_station_mm==2);
    const auto bound=compile_fixture(result.prepared,result.prepared->logical_filament_id,0,0,1,"S0",nullptr,{},.54);
    CHECK(bound.entry_targets.back().x()-bound.entry_targets.front().x()==2);
    CHECK(result.prepared->entry.end_station_mm==2);
    CHECK(bound.entry_targets.back().z()==Catch::Approx(.54).margin(1e-12));
}

TEST_CASE("legacy landing rejects cut overlap but cuts at an existing vertex", "[ContinuousFiber][LegacyLanding]")
{
    ContinuousFiberConfig config;config.z_hop_height_mm=2;config.cut_to_contact_length_mm=23;
    CHECK_FALSE(finalize_independent(straight_path(10,10,34,10),{rectangle(0,0,40,20)},config,test_id()).prepared);
    auto path=path_from_points({{10,10},{10.741,10},{37,10},{60,10}});
    const auto result=finalize_independent(path,{rectangle(0,0,70,20)},config,test_id());
    REQUIRE(result.prepared);
    CHECK(std::count_if(result.prepared->actions.begin(),result.prepared->actions.end(),[](const auto& a){return a.type==FiberActionType::Cut;})==1);
    CHECK(result.prepared->passive_tail_length_mm()==Catch::Approx(23).margin(.002));
}


TEST_CASE("fiber landing binds after plate and belt transforms exactly once", "[ContinuousFiber][LegacyLanding]")
{
    ContinuousFiberConfig config;config.z_hop_height_mm=2;config.cut_to_contact_length_mm=23;
    auto candidate=path_from_points({{10,10},{10.00051,10},{60,10}});
    const auto result=finalize_independent(candidate,{rectangle(0,0,70,20)},config,test_id());
    REQUIRE(result.prepared);
    PrintConfig machine;GCodeWriter writer;writer.apply_print_config(machine);writer.set_extruders({0});writer.toolchange(0);
    writer.set_xy_offset(.00049,0);
    const bool belted=GENERATE(false,true);
    if(belted){const auto belt=BeltCoordinateSystem::create(45,250);writer.set_belt_coordinate_system(&belt);}
    const auto bound=compile_fixture(result.prepared,result.prepared->logical_filament_id,0,0,1,"S0",&writer,{},.54);
    CHECK(bound.entry_targets.back().x()==Catch::Approx(11.99951).margin(1e-12)); // The route follows the configured 2mm, not the tiny first edge.
    writer.set_position(Vec3d(10,10,2.54));
    const GCodeWriter::ScopedXYZPrecision precision(writer,6);
    std::string text;
    for(size_t i=1;i<bound.entry_targets.size();++i)text+=writer.emit_frozen_fiber_motion(bound.entry_targets[i],0,
        result.prepared->start_procedure.landing_speed_mm_s,EMovePathType::Linear_move,Vec2d::Zero(),true);
    INFO(text);
    CHECK(text.find(" E")==std::string::npos);
    CHECK(text.find("X11.99951 Y")!=std::string::npos);
    CHECK((writer.quantized_machine_position(writer.get_position())-bound.entry_targets.back()).norm()<1e-10);
    CHECK((writer.get_emitted_position()-writer.oriented_position(bound.entry_targets.back())).norm()<1e-10);
}


TEST_CASE("legacy zero-hop duplicate entry keeps adhesion before subsequent feed", "[ContinuousFiber][LegacyLanding]")
{
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;config.z_hop_height_mm=0;
    const auto candidate=path_from_points({{10,10},{10,10},{60,10}});
    const auto result=finalize_independent(candidate,{rectangle(0,0,70,20)},config,test_id());
    INFO(result.detail);REQUIRE(result.prepared);
    CHECK(result.prepared->entry.mode==FiberEntryMode::PoweredFirstEdge);
    CHECK(result.prepared->entry.end_station_mm==0);
    CHECK(result.prepared->spans.front().kind!=FiberMotionKind::PoweredEntry);
    const auto& actions=result.prepared->actions;
    const auto dwell=std::find_if(actions.begin(),actions.end(),[](const auto& action){return action.type==FiberActionType::AdhesionDwell;});
    REQUIRE(dwell!=actions.end());REQUIRE(dwell!=actions.begin());REQUIRE(dwell+1!=actions.end());
    CHECK((dwell-1)->type==FiberActionType::Start);
    CHECK((dwell+1)->type==FiberActionType::MotionSpan);
}

TEST_CASE("analytic fiber geometry uses true arc stations and preserves circles", "[ContinuousFiber][NativeArc]")
{
    Polyline3 path;
    path.points={Point3::new_scale(20,0,0),Point3::new_scale(0,20,0),Point3::new_scale(-50,20,0)};
    ArcSegment arc(Point(0,0),scale_(20.),path.points[0].to_point(),path.points[1].to_point(),ArcDirection::Arc_Dir_CCW);
    path.fitting_result={{0,1,EMovePathType::Arc_move_ccw,arc},{1,2,EMovePathType::Linear_move,{}}};
    CHECK(Geometry::length_with_arcs_mm(path)==Catch::Approx(10*PI+50));
    CHECK(unscale<double>(path.length())!=Catch::Approx(10*PI+50));
    auto first=Geometry::slice_with_arcs_mm(path,0,2);
    REQUIRE(first.fitting_result.size()==1);
    CHECK(first.fitting_result.front().arc_data.center==Point(0,0));
    CHECK(Geometry::length_with_arcs_mm(first)==Catch::Approx(2).margin(0.000002));
    const auto tangent=Geometry::tangent_at_length_with_arcs_mm(path,10*PI,true);
    CHECK(tangent.x()==Catch::Approx(-1));CHECK(tangent.y()==Catch::Approx(0).margin(1e-8));
    for(double error:{.01,.00005}) {
        std::vector<double> stations;
        const auto samples=Geometry::sample_with_arcs(path,error,&stations);
        REQUIRE(samples.points.size()==stations.size());
        CHECK(stations.back()==Catch::Approx(10*PI+50));
        CHECK(Geometry::point_at_length_with_arcs_mm(path,2)==first.points.back());
    }
    auto reversed=path;reversed.reverse();
    CHECK(Geometry::length_with_arcs_mm(reversed)==Catch::Approx(10*PI+50));
    auto stale=path;stale.fitting_result.front().end_point_index=2;
    CHECK_THROWS(Geometry::validate_with_arcs(stale));
}

TEST_CASE("fiber landing follows an arc and binding preserves cut tail without E", "[ContinuousFiber][NativeArc]")
{
    auto candidate=path_from_points({{40,0},{0,40},{-50,40}});
    ArcSegment arc(Point(0,0),scale_(40.),candidate.polyline.points[0].to_point(),candidate.polyline.points[1].to_point(),ArcDirection::Arc_Dir_CCW);
    candidate.polyline.fitting_result={{0,1,EMovePathType::Arc_move_ccw,arc},{1,2,EMovePathType::Linear_move,{}}};
    ContinuousFiberConfig config;
    config.z_hop_height_mm=3;config.landing_length_mm=2;config.cut_to_contact_length_mm=23;
    config.finish_extension_length_mm=7;
    const auto result=finalize_independent(candidate,{rectangle(-80,-20,80,80)},config,test_id());
    INFO(result.detail);REQUIRE(result.prepared);
    const auto& prepared=*result.prepared;
    CHECK(prepared.entry.end_station_mm==2);
    REQUIRE(prepared.entry.geometry.fitting_result.size()==1);
    CHECK(prepared.entry.geometry.fitting_result[0].path_type==EMovePathType::Arc_move_ccw);
    CHECK(prepared.passive_tail_length_mm()==Catch::Approx(23).margin(.00001));
    auto no_hop=config;no_hop.z_hop_height_mm=0;no_hop.start_speed_mm_s=10;
    const auto h0=finalize_independent(candidate,{rectangle(-80,-20,80,80)},no_hop,test_id(),
        continuous_fiber_detail::arc_metadata(candidate.polyline));
    INFO(h0.detail);REQUIRE(h0.prepared);
    REQUIRE(h0.prepared->spans.front().kind==FiberMotionKind::PoweredEntry);
    for(const auto& edge:h0.prepared->spans.front().edges)CHECK(edge.speed_mm_s<=6.5+1e-8);
    const auto dwell=std::find_if(h0.prepared->actions.begin(),h0.prepared->actions.end(),[](const auto& action) {
        return action.type==FiberActionType::AdhesionDwell;
    });
    REQUIRE(dwell!=h0.prepared->actions.end());REQUIRE(dwell!=h0.prepared->actions.begin());
    CHECK((dwell-1)->type==FiberActionType::MotionSpan);
    for(bool native:{false,true}) {
        const auto bound=compile_fixture(result.prepared,0,0,0,1,"M400\nS0\nM400\n",nullptr,{},0,native);
        size_t arcs=0;
        for(size_t s=0;s<bound.spans().size();++s) {
            if(native) CHECK(bound.spans()[s].commands.size()==prepared.spans[s].edges.size());
            for(size_t e=0;e<bound.spans()[s].commands.size();++e) {
                const auto& command=bound.spans()[s].commands[e];
                const auto owner=command.source_primitive;
                REQUIRE(owner<prepared.spans[s].edges.size());
                CHECK(command.speed_mm_s==prepared.spans[s].edges[owner].speed_mm_s);
                if(command.arc_index>=0 && bound.arcs()[command.arc_index].ccw)++arcs;
                if(!prepared.spans[s].actively_feeds_fiber())CHECK(command.dE==0);
            }
        }
        CHECK((arcs>0)==native);
    }
}

TEST_CASE("native fiber preview retains small feed after large absolute E", "[ContinuousFiber][NativeArc][preview]")
{
    PrintConfig config;config.gcode_flavor.value=gcfKlipper;
    GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
    const std::string gcode=
        "G90\nM82\nG92 E75000\nG1 X1 Y0 Z0.2 F600\n"
        ";FIBER_BEGIN v=3 occurrence=1 purpose=contour width=0.8 height=0.2 e_units_per_mm=1\n"
        ";FIBER_PREFEED_BEGIN\nG1 E75023.5 F600\n;FIBER_PREFEED_END\n"
        ";FIBER_LANDING_BEGIN\nG1 X1 Y0 Z0.2 F180\n;FIBER_LANDING_END\n;FIBER_START\n"
        "G3 X0.999999 Y0.001 I-1 J0 E75023.501 F600\n"
        ";FIBER_CUT\n;FIBER_TAIL_BEGIN\nG1 X0.9 Y0.1 F600\n"
        ";FIBER_DEPLETED\n;FIBER_FINISH_BEGIN\n;FIBER_FINISH\n;FIBER_END\n";
    REQUIRE_NOTHROW(processor.process_buffer(gcode));
    double powered_feed=0;
    for(const auto& move:processor.get_result().moves)
        if(move.deposition==ToolpathDeposition::ContinuousFiberPowered)powered_feed+=move.fiber_feed_delta_mm;
    CHECK(powered_feed==Catch::Approx(.001).margin(1e-8));
}

TEST_CASE("analytic slices preserve tiny junctions and closed circles", "[ContinuousFiber][NativeArc]")
{
    Polyline3 tiny=path_from_points({{0,0},{100,0},{100.0001,.0001},{100.0001,1}}).polyline;
    ArcSegment a(Point::new_scale(100,.0001),scale_(.0001),tiny.points[1].to_point(),tiny.points[2].to_point(),ArcDirection::Arc_Dir_CCW);
    tiny.fitting_result={{0,1,EMovePathType::Linear_move,{}},{1,2,EMovePathType::Arc_move_ccw,a},{2,3,EMovePathType::Linear_move,{}}};
    const auto copy=Geometry::slice_with_arcs_mm(tiny,0,Geometry::length_with_arcs_mm(tiny));
    CHECK(copy.points==tiny.points);
    Polyline3 circle=path_from_points({{10,0},{-10,0},{10,0}}).polyline;
    for(size_t i=0;i<2;++i)circle.fitting_result.push_back({i,i+1,EMovePathType::Arc_move_ccw,
        ArcSegment(Point(0,0),scale_(10.),circle.points[i].to_point(),circle.points[i+1].to_point(),ArcDirection::Arc_Dir_CCW)});
    CHECK(Geometry::length_with_arcs_mm(circle)==Catch::Approx(20*PI));
    auto reverse=circle;reverse.reverse();
    CHECK(Geometry::length_with_arcs_mm(reverse)==Catch::Approx(20*PI));
    Polyline3 before,after;REQUIRE(Geometry::split_at_length_with_arcs_mm(circle,23,before,after));
    CHECK(before.points.back()==after.points.front());
    CHECK(Geometry::length_with_arcs_mm(after)==Catch::Approx(20*PI-23).margin(.000003));
}

TEST_CASE("preview shares literal E precision across fiber boundaries", "[ContinuousFiber][OrcaCorrections][preview]")
{
    for (bool relative:{false,true}) for (bool streaming:{false,true}) {
        CAPTURE(relative,streaming);
        PrintConfig config;config.gcode_flavor.value=gcfKlipper;
        GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
        const std::string code=std::string("G90\n")+(relative?"M83\n":"M82\n")+
            "G92 E75000.000001\nG1 X0 Y0 Z0.2 F600\nG1 X1 E"+(relative?"0.003":"75000.003001")+"\n"
            ";FIBER_BEGIN v=3 occurrence=1 purpose=contour width=0.8 height=0.2 e_units_per_mm=1\n"
            ";FIBER_PREFEED_BEGIN\nG1 E"+(relative?"23.5":"75023.503001")+" F600\n;FIBER_PREFEED_END\n"
            ";FIBER_LANDING_BEGIN\nG1 X1 Y0 Z0.2 F180\n;FIBER_LANDING_END\n;FIBER_START\n"
            "G1 X2 E"+(relative?"0.1":"75023.603001")+" F600\n"
            ";FIBER_CUT\n;FIBER_TAIL_BEGIN\nG1 X3 F600\n;FIBER_DEPLETED\n"
            ";FIBER_FINISH_BEGIN\nG1 X4 F600\n;FIBER_FINISH\n;FIBER_END\nG0 E"+
            (relative?"0.0001":"75023.603101")+"\n";
        if (streaming) {
            std::istringstream input(code);std::string line;
            while (std::getline(input,line)) processor.process_buffer(line+"\n");
        } else processor.process_buffer(code);
        double prefeed=0,powered=0;
        for (const auto& move:processor.get_result().moves) {
            if (move.fiber_phase==FiberProcessPhase::Prefeed) prefeed+=move.fiber_feed_delta_mm;
            if (move.deposition==ToolpathDeposition::ContinuousFiberPowered) powered+=move.fiber_feed_delta_mm;
            CHECK(move.type!=EMoveType::Retract);
        }
        CHECK(prefeed==Catch::Approx(23.5).margin(1e-7));
        CHECK(powered==Catch::Approx(.1).margin(1e-7));
        CHECK(processor.get_result().moves.back().delta_extruder==Catch::Approx(.0001).margin(1e-8));
    }
}

TEST_CASE("double coordinate reading preserves native word rules", "[ContinuousFiber][OrcaCorrections][preview]")
{
    PrintConfig config;config.gcode_flavor.value=gcfKlipper;
    GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
    processor.process_buffer("G90\nM82\nG92 E75000.000001 Ebad\nG1 Z0.2 F600\n"
        "G1 X1 X2 Xoops Y1 Y+9 E75000.001001 Ebad ; X100 E0\n");
    const auto& move=processor.get_result().moves.back();
    CHECK(move.position.x()==2);
    CHECK(move.position.y()==1);
    CHECK(move.delta_extruder==Catch::Approx(.001).margin(1e-8));
}

TEST_CASE("preview synthetic retracts retain configured precision", "[ContinuousFiber][OrcaCorrections][preview]")
{
    PrintConfig config;config.gcode_flavor.value=gcfKlipper;
    config.retraction_length.values={.123456};config.retract_restart_extra.values={.000001};
    GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
    processor.process_buffer("M83\nG1 Z0.2 F600\nG10\nG11\n");
    const auto& moves=processor.get_result().moves;
    REQUIRE(moves.size()>=3);
    CHECK(moves[moves.size()-2].delta_extruder==Catch::Approx(-.123456).margin(1e-8));
    CHECK(moves.back().delta_extruder==Catch::Approx(.123457).margin(1e-8));
}

TEST_CASE("seam visualization restores precise machine position", "[ContinuousFiber][OrcaCorrections][preview]")
{
    PrintConfig config;config.gcode_flavor.value=gcfKlipper;
    GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
    processor.process_buffer("G90\nM83\nG1 X1000.000001 Y1000.000001 Z0.2 F600\n"
        "; FEATURE: Outer wall\nG1 X1001.000001 E1\nG1 Y1001.000001 E1\n"
        "G1 X1000.000001 E1\nG1 Y1000.000001 E1\n"
        "G1 X1000.000002\nG1 X1000.000003\n");
    const auto& moves=processor.get_result().moves;
    REQUIRE(std::any_of(moves.begin(),moves.end(),[](const auto& m){return m.type==EMoveType::Seam;}));
    CHECK(moves.back().travel_dist==Catch::Approx(.000001).margin(1e-10));
}

TEST_CASE("arc preview preserves G92 origin units and extrusion modes", "[ContinuousFiber][OrcaCorrections][preview]")
{
    for (const auto flavor:{gcfKlipper,gcfMarlinFirmware})
        for (bool inches:{false,true}) for (bool xyz_relative:{false,true})
            for (bool e_relative:{false,true}) for (bool clockwise:{false,true})
                for (const std::string fitting:{"I-1 J0","R1","R+1","R-1"}) {
        CAPTURE(flavor,inches,xyz_relative,e_relative,clockwise,fitting);
        PrintConfig config;config.gcode_flavor.value=flavor;
        GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
        const double unit=inches?25.4:1.;
        const std::string code=std::string(inches?"G20\n":"G21\n")+
            "G90\nM82\nG1 X0 Y0 Z0.2 F600\nG92 X10 Y20 E75000.000001\nG1 X11 Y20\n"+
            (xyz_relative?"G91\n":"G90\n")+(e_relative?"M83\n":"M82\n")+
            (clockwise?"G2 ":"G3 ")+(xyz_relative?"X-1 ":"X10 ")+
            (xyz_relative?(clockwise?"Y-1 ":"Y1 "):(clockwise?"Y19 ":"Y21 "))+fitting+
            (xyz_relative || e_relative?" E0.001\n":" E75000.001001\n");
        processor.process_buffer(code);
        const auto& moves=processor.get_result().moves;
        REQUIRE(moves.size()>2);
        CHECK(moves.back().position.x()==Catch::Approx(0).margin(1e-5));
        CHECK(moves.back().position.y()==Catch::Approx(clockwise?-unit:unit).margin(1e-5));
        double feed=0,travel=0;
        // Only the arc has E, and its exact expected length follows from its
        // quarter/three-quarter unit circle, independently of the parser.
        for (const auto& move:moves) if (move.delta_extruder!=0) {
            CHECK(move.delta_extruder>0);
            feed+=move.delta_extruder;travel+=move.travel_dist;
        }
        CHECK(feed==Catch::Approx(.001*unit).margin(1e-7));
        const double arc_length=(fitting=="R-1"?1.5:.5)*PI*unit;
        CHECK(travel==Catch::Approx(arc_length).margin(.03*arc_length));
    }
}

TEST_CASE("arc preview retains native malformed parameter handling", "[ContinuousFiber][OrcaCorrections][preview]")
{
    for (const std::string invalid:{"Rbad I-1 J0","Ibad J-1","I-1 Jbad","Rbad"}) {
        CAPTURE(invalid);
        PrintConfig config;config.gcode_flavor.value=gcfKlipper;
        GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
        processor.process_buffer("G90\nG1 X1 Y0 Z0.2 F600\nG3 X0 Y1 "+invalid+"\nG1 X1.000001\n");
        CHECK(processor.get_result().moves.back().travel_dist==Catch::Approx(.000001).margin(1e-10));
    }
}

TEST_CASE("preview keeps shared precision through virtual moves tools and cache", "[ContinuousFiber][OrcaCorrections][preview]")
{
    PrintConfig config;config.gcode_flavor.value=gcfKlipper;
    SECTION("virtual G1 updates the same coordinate state") {
        GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
        processor.process_buffer("G90\nM82\nG92 E75000.000001\nG1 Z0.2 F600\n"
            "VG1 X1 E75000.001001\nG1 X2 E75000.001101\n");
        CHECK(processor.get_result().moves.back().delta_extruder==Catch::Approx(.0001).margin(1e-8));
    }
    SECTION("real tool changes preserve existing shared E rules") {
        config.filament_diameter.values={1.75,1.75};config.filament_map.values={1,2};
        config.physical_extruder_map.values={0,1};
        GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
        processor.process_buffer("G90\nM82\nT0\nG92 E75000.000001\nG1 Z0.2 F600\n"
            "G1 X1 E75000.001001\nT1\nG1 X2 E75000.001101\nT0\nG1 X3 E75000.001201\n");
        const auto& moves=processor.get_result().moves;
        REQUIRE(std::any_of(moves.begin(),moves.end(),[](const auto& m){return m.extruder_id==1;}));
        CHECK(moves.back().delta_extruder==Catch::Approx(.0001).margin(1e-8));
        CHECK(std::none_of(moves.begin(),moves.end(),[](const auto& m){return m.type==EMoveType::Retract;}));
    }
    SECTION("Repetier cache commands preserve their existing position semantics") {
        config.gcode_flavor.value=gcfRepetier;
        GCodeProcessor processor;processor.apply_config(config);processor.initialize_result_moves();
        processor.process_buffer("G90\nM82\nG92 E75000.000001\nG1 X1000.000001 Z0.2 F600\nM401\n"
            "G1 X1001.000001 E75000.001001\nM402\nG1 X1001.000002 E75000.001101\n");
        const auto& move=processor.get_result().moves.back();
        CHECK(move.travel_dist==Catch::Approx(.000001).margin(1e-10));
        CHECK(move.delta_extruder==Catch::Approx(.0001).margin(1e-8));
    }
}

TEST_CASE("arc projection searches the permitted interval", "[ContinuousFiber][OrcaCorrections][NativeArc]")
{
    for (bool clockwise:{false,true}) {
        CAPTURE(clockwise);
        Polyline3 arc=path_from_points({{10,0},{0,clockwise?10.:-10.}}).polyline;
        const auto direction=clockwise?ArcDirection::Arc_Dir_CW:ArcDirection::Arc_Dir_CCW;
        arc.fitting_result={{0,1,clockwise?EMovePathType::Arc_move_cw:EMovePathType::Arc_move_ccw,
            ArcSegment(Point(0,0),scale_(10.),arc.points.front().to_point(),arc.points.back().to_point(),direction)}};
        const double angle=(clockwise?-340.:340.)*PI/180;
        const Point3 query=Point3::new_scale(10*std::cos(angle),10*std::sin(angle),0);
        CHECK(Geometry::project_with_arcs_mm(arc,query,5*PI)==Catch::Approx(15*PI).margin(1e-6));
        CHECK(Geometry::project_with_arcs_mm(arc,query)==Catch::Approx(0).margin(1e-6));
        CHECK(Geometry::project_with_arcs_mm(arc,Point3(0,0,0),5*PI)==Catch::Approx(5*PI).margin(1e-6));
        CHECK(Geometry::project_with_arcs_mm(arc,Point3(0,0,0),2.5*PI)==Catch::Approx(2.5*PI).margin(1e-6));
        CHECK(Geometry::project_with_arcs_mm(arc,Point3::new_scale(-10,0,0),5*PI)==Catch::Approx(10*PI).margin(1e-6));
        CHECK(Geometry::project_with_arcs_mm(arc,query,15*PI)==Catch::Approx(15*PI).margin(1e-6));
        CHECK_THROWS_AS(Geometry::project_with_arcs_mm(arc,query,-1),std::invalid_argument);
        CHECK_THROWS_AS(Geometry::project_with_arcs_mm(arc,query,16*PI),std::invalid_argument);
        CHECK_THROWS_AS(Geometry::project_with_arcs_mm(arc,query,std::numeric_limits<double>::quiet_NaN()),std::invalid_argument);
    }
    Polyline3 empty,point;point.points={Point3(0,0,0)};
    CHECK_THROWS_AS(Geometry::project_with_arcs_mm(empty,Point3(0,0,0)),std::invalid_argument);
    CHECK(Geometry::project_with_arcs_mm(point,Point3(1,2,0))==0);
    point.points.push_back(point.points.front());
    CHECK(Geometry::project_with_arcs_mm(point,Point3(1,2,0))==0);
}

TEST_CASE("fiber entity measures deposition separately from finish travel", "[ContinuousFiber][OrcaCorrections][FinishInterface]")
{
    ContinuousFiberConfig config;config.finish_extension_length_mm=23;config.z_hop_height_mm=3;config.cut_to_contact_length_mm=23;
    const auto source=straight_path(0,0,60,0);
    const auto result=finalize_independent(source,{rectangle(-10,-10,90,10)},config,test_id());
    INFO(result.detail);REQUIRE(result.prepared);
    ExtrusionFiberPath fiber(source,erContinuousFiberInfill,result.prepared);
    const ExtrusionEntity* entity=&fiber;
    const ExtrusionPath* path=&fiber;
    CHECK(unscale<double>(entity->length())==Catch::Approx(83).margin(1e-6));
    CHECK(entity->total_volume()==Catch::Approx(6).margin(1e-6));
    CHECK(entity->first_point()==Point::new_scale(0,0));
    CHECK(entity->last_point()==Point::new_scale(83,0));
    for (double epsilon:{-.1,0.,.1}) {
        CAPTURE(epsilon);
        const float scaled_epsilon=float(scale_(epsilon));
        const double width=source.width+2*epsilon;
        Polygons polymorphic;entity->polygons_covered_by_width(polymorphic,scaled_epsilon);
        const auto direct=fiber.polygons_covered_by_width(scaled_epsilon);
        const auto inherited=path->polygons_covered_by_width(scaled_epsilon);
        for (const auto& polygons:{polymorphic,direct,inherited}) {
            CHECK(unscaled<double>(unscaled<double>(area(union_ex(polygons))))==Catch::Approx(60*width).margin(1e-5));
            CHECK(diff_ex(union_ex(polygons),{rectangle(0,-width/2,60,width/2)}).empty());
        }
        const double spacing=Flow(source.width,source.height,0.f).spacing()+2*epsilon;
        CHECK(unscaled<double>(unscaled<double>(area(union_ex(fiber.polygons_covered_by_spacing(scaled_epsilon)))))==
            Catch::Approx(60*spacing).margin(1e-5));
    }
    CHECK(fiber.polygons_covered_by_width(float(scale_(-1.))).empty());
    CHECK(fiber.polygons_covered_by_spacing(float(scale_(-1.))).empty());
}

TEST_CASE("fiber coverage keeps closed seams and touching strands continuous", "[ContinuousFiber][OrcaCorrections][FinishInterface]")
{
    ContinuousFiberConfig config;config.cut_to_contact_length_mm=23;
    config.finish_extension_length_mm=23;config.finish_overlap_length_mm=23;config.z_hop_height_mm=3;
    SECTION("closed square has no caps at landing CUT or seam") {
        const auto source=path_from_points({{0,0},{40,0},{40,40},{0,40},{0,0}});
        auto id=test_id();id.parent.purpose=FiberPathPurpose::Contour;
        const auto result=finalize_independent(source,{rectangle(-5,-5,45,45)},config,id);
        INFO(result.detail);REQUIRE(result.prepared);
        ExtrusionFiberPath fiber(source,erContinuousFiberContour,result.prepared);
        CHECK(unscale<double>(fiber.length())==Catch::Approx(183).margin(1e-6));
        CHECK(fiber.total_volume()==Catch::Approx(16).margin(1e-6));
        for (double epsilon:{-.1,0.,.1}) {
            const double radius=source.width/2.+epsilon;
            const auto coverage=union_ex(fiber.polygons_covered_by_width(float(scale_(epsilon))));
            REQUIRE(coverage.size()==1);REQUIRE(coverage.front().holes.size()==1);
            // Outer corners are round and inner corners square. Their area
            // difference gives an independent closed-stroke area oracle.
            const double expected=320*radius+(PI-4)*radius*radius;
            CHECK(unscaled<double>(unscaled<double>(area(coverage)))==Catch::Approx(expected).margin(.0001));
            CHECK_FALSE(coverage.front().contains(Point::new_scale(20,20)));
        }
    }
    SECTION("narrower spacing reopens the gap between touching parallel strands") {
        const auto source=path_from_points({{0,0},{60,0},{60,.8},{0,.8}});
        const auto result=finalize_independent(source,{rectangle(-5,-5,65,5)},config,test_id());
        INFO(result.detail);REQUIRE(result.prepared);
        ExtrusionFiberPath fiber(source,erContinuousFiberInfill,result.prepared);
        const Point middle=Point::new_scale(30,.4);
        const auto contains=[&](const Polygons& polygons) {
            const auto coverage=union_ex(polygons);
            return std::any_of(coverage.begin(),coverage.end(),[&](const auto& p){return p.contains(middle);});
        };
        CHECK(contains(fiber.polygons_covered_by_width()));
        CHECK_FALSE(contains(fiber.polygons_covered_by_spacing()));
        CHECK_FALSE(contains(fiber.polygons_covered_by_width(float(scale_(-.05)))));
        CHECK(contains(fiber.polygons_covered_by_spacing(float(scale_(.05)))));
    }
}

TEST_CASE("fiber finalization retains multi-support analytic primitives", "[ContinuousFiber][OrcaCorrections][NativeArc]")
{
    for (bool clockwise:{false,true}) for (bool duplicate:{false,true}) {
        CAPTURE(clockwise,duplicate);
        const double sign=clockwise?-1.:1.;
        auto source=path_from_points({{40,-50*sign},{40,-25*sign},{40,0},
            {40/std::sqrt(2.),40/std::sqrt(2.)*sign},{0,40*sign},{-30,40*sign},{-60,40*sign}});
        if (duplicate) source.polyline.points.insert(source.polyline.points.begin()+1,source.polyline.points.front());
        const size_t shift=duplicate?1:0;
        const auto type=clockwise?EMovePathType::Arc_move_cw:EMovePathType::Arc_move_ccw;
        const auto direction=clockwise?ArcDirection::Arc_Dir_CW:ArcDirection::Arc_Dir_CCW;
        source.polyline.fitting_result={{0,2+shift,EMovePathType::Linear_move,{}},
            {2+shift,4+shift,type,ArcSegment(Point(0,0),scale_(40.),Point::new_scale(40,0),Point::new_scale(0,40*sign),direction)},
            {4+shift,6+shift,EMovePathType::Linear_move,{}}};
        ContinuousFiberConfig config;config.z_hop_height_mm=3;config.cut_to_contact_length_mm=23;
        config.finish_extension_length_mm=23;
        const auto arcs=continuous_fiber_detail::arc_metadata(source.polyline);
        const auto result=finalize_independent(source,{rectangle(-90,-90,90,90)},config,test_id(),arcs);
        INFO(result.detail);REQUIRE(result.prepared);
        const auto& prepared=*result.prepared;
        CHECK(prepared.total_depositing_length_mm()==Catch::Approx(110+20*PI).margin(1e-5));
        CHECK(prepared.passive_tail_length_mm()==Catch::Approx(23).margin(1e-6));
        double arc_length=0;
        for (const auto& span:prepared.spans) {
            REQUIRE(span.edges.size()==span.geometry.fitting_result.size());
            for (size_t i=0;i<span.edges.size();++i) {
                const auto& record=span.geometry.fitting_result[i];
                if (record.path_type==type) {
                    CHECK(record.arc_data.center==Point(0,0));
                    CHECK(record.arc_data.direction==direction);
                    CHECK(span.edges[i].speed_mm_s<=6.5+1e-8);
                    arc_length+=unscale<double>(record.arc_data.length);
                }
            }
        }
        CHECK(arc_length==Catch::Approx(20*PI).margin(1e-5));
        CHECK_NOTHROW(prepared.validate());
    }
}

TEST_CASE("fiber finalization preserves a tiny arc after a large station", "[ContinuousFiber][OrcaCorrections][NativeArc]")
{
    auto source=path_from_points({{0,0},{1000,0},{1000.0001,.0001},{1000.0001,100.0001}});
    const auto& points=source.polyline.points;
    source.polyline.fitting_result={{0,1,EMovePathType::Linear_move,{}},
        {1,2,EMovePathType::Arc_move_ccw,ArcSegment(Point::new_scale(1000,.0001),scale_(.0001),
            points[1].to_point(),points[2].to_point(),ArcDirection::Arc_Dir_CCW)},
        {2,3,EMovePathType::Linear_move,{}}};
    ContinuousFiberConfig config;config.z_hop_height_mm=3;config.cut_to_contact_length_mm=23;
    config.finish_extension_length_mm=23;config.speed_sampling_length_mm=10000;
    const auto result=finalize_independent(source,{rectangle(-5,-5,1010,110)},config,test_id(),
        continuous_fiber_detail::arc_metadata(source.polyline));
    INFO(result.detail);REQUIRE(result.prepared);
    size_t arcs=0;
    for (const auto& span:result.prepared->spans) for (const auto& record:span.geometry.fitting_result)
        if (record.path_type==EMovePathType::Arc_move_ccw) {
            ++arcs;
            CHECK(span.geometry.points[record.start_point_index]==points[1]);
            CHECK(span.geometry.points[record.end_point_index]==points[2]);
            CHECK(unscale<double>(record.arc_data.length)==Catch::Approx(.00005*PI).margin(1e-9));
        }
    CHECK(arcs==1);
}

TEST_CASE("frozen geometry is certified for each late placement", "[ContinuousFiber][Finalization]")
{
    auto context=fiber_fixture_context();
    context->placements={{101,7,Vec2d(100.0000004,80.0000003)},{202,8,Vec2d(120.0000008,90.0000007)}};
    context->config.extruder_offset.values[0]=Vec2d(7.1234567,-2.7654321);
    context->transform.xy_offset=Vec2d(20.1234567,10.7654321);
    auto config=ContinuousFiberConfig{};
    config.cut_to_contact_length_mm=23;config.z_hop_height_mm=2;
    const auto source=path_from_points({{10,10},{60,10},{60,40},{10,40}});
    const ExPolygons domain{rectangle(0,0,70,50)};
    const auto finalized=finalize_independent(source,domain,config,test_id());
    REQUIRE(finalized.prepared);const auto& prepared=*finalized.prepared;
    CompiledFiberPath compiled;compiled.geometry=finalized.prepared;compiled.context=context;
    for(const auto& placement:context->placements)
        compiled.variants.push_back(std::make_shared<const FrozenFiberExecution>(
            compile_fiber_execution(prepared.draft(),*context,placement,.54)));
    REQUIRE(compiled.variants.size()==2);
    for(size_t id:{101,202}) {
        const auto& execution=compiled.execution(id);
        CHECK_NOTHROW(certify_fiber_coverage(prepared,execution,*context));
        CHECK(execution.source_deviation_mm<=prepared.execution_deviation_mm);
        const auto footprint=fiber_contour_coverage(frozen_fiber_deposition(execution,*context),.5*source.width);
        CHECK(diff_ex(footprint,offset_ex(prepared.possible_coverage,4.f),ApplySafetyOffset::No).empty());
        CHECK(diff_ex(prepared.guaranteed_components,offset_ex(footprint,4.f),ApplySafetyOffset::No).empty());
        double passive=0;
        for(const auto& span:execution.spans()) {
            Vec2d from=span.from;
            for(const auto& command:span.commands) {
                if(span.kind==FiberMotionKind::PassiveDepositingAfterCut) {
                    CHECK(command.dE==0);passive+=(command.target-from).norm();
                }
                from=command.target;
            }
        }
        CHECK(passive==Catch::Approx(23).margin(prepared.passive_length_error_mm));
    }
    CHECK_THROWS_AS(compiled.execution(303),FiberExecutionError);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<PreparedFiberPath>);
}

TEST_CASE("fiber writer checks actual intermediate commands and restores its hook", "[ContinuousFiber][Finalization]")
{
    PrintConfig config;GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0});writer.toolchange(0);
    const GCodeWriter::ScopedXYZPrecision precision(writer,6);
    writer.set_position(Vec3d(10.1234564,10.2345674,3.1234564));writer.set_current_position_clear(false);
    std::vector<std::pair<Vec3d,Vec3d>> checked;
    {
        const GCodeWriter::ScopedFiberMotionCheck check(writer,[&](const Vec3d& from,const Vec3d& to,
            EMovePathType,const Vec2d&,double,double) {checked.emplace_back(from,to);});
        writer.travel_to_xyz(Vec3d(12.3456784,15.4567894,4.5678914),"unknown position",true);
    }
    REQUIRE(checked.size()==2); // Actual XY followed by actual Z.
    CHECK(checked[0].second==checked[1].first);
    CHECK(checked[0].second.z()==Catch::Approx(GCodeFormatter::quantize(3.1234564,3)).margin(1e-12));
    CHECK(checked[1].second.z()==GCodeFormatter::quantize(4.5678914,6));
    const auto count=checked.size();writer.travel_to_xy(Vec2d(13,16));CHECK(checked.size()==count);
    {
        const GCodeWriter::ScopedFiberMotionCheck check(writer,[](const Vec3d&,const Vec3d&,
            EMovePathType,const Vec2d&,double,double){throw std::runtime_error("rejected intermediate motion");});
        CHECK_THROWS_WITH(writer.travel_to_z(5,"rejected Z",true),"rejected intermediate motion");
    }
    CHECK_NOTHROW(writer.travel_to_z(6,"ordinary travel restored",true));
    Vec2d observed_ij=Vec2d::Zero();
    {
        const GCodeWriter::ScopedFiberMotionCheck check(writer,[&](const Vec3d&,const Vec3d&,
            EMovePathType,const Vec2d& ij,double,double){observed_ij=ij;});
        writer.extrude_arc_to_xy(Vec2d(14,16),Vec2d(.1234567,.7654321),0,true);
    }
    CHECK(observed_ij==Vec2d(GCodeFormatter::quantize(.1234567,6),GCodeFormatter::quantize(.7654321,6)));
}

TEST_CASE("fiber contexts include layer aliases and invalidate moved cached placements", "[ContinuousFiber][Finalization]")
{
    auto config=masked_print_config();config.set_deserialize_strict("filament_printable","3,3");
    config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");
    config.set_deserialize_strict("bottom_shell_thickness","0");
    Model model;auto* first=model.add_object();first->add_volume(make_cube(20,20,2));
    first->add_instance()->set_offset(Vec3d(60.0000004,60.0000003,0));
    first->config.set_key_value("extruder",new ConfigOptionInt(1));
    auto* alias=model.add_object(*first);
    auto shared_mesh=first->volumes.front()->mesh_ptr();alias->volumes.front()->set_mesh(shared_mesh);
    alias->instances.front()->set_offset(Vec3d(100.0000008,60.0000007,0));
    Print print;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);print.process();
    REQUIRE(print.objects().size()==2);
    REQUIRE(print.objects()[1]->get_shared_object()==print.objects()[0]);
    const auto old_context=print.objects().front()->fiber_geometry_context();
    REQUIRE(old_context);CHECK(print.objects()[1]->fiber_geometry_context()==old_context);
    const auto old_table=print.fiber_execution_plans();REQUIRE(old_table);
    alias->instances.front()->set_offset(Vec3d(110.0000008,60.0000007,0));
    print.apply(model,config);
    CHECK_FALSE(print.fiber_execution_plans_current(old_table));
    print.process(nullptr,true);
    const auto moved_table=print.fiber_execution_plans();REQUIRE(moved_table);
    CHECK(moved_table!=old_table);
    REQUIRE_FALSE(moved_table->paths.empty());
    const auto& compiled=moved_table->paths.begin()->second;
    REQUIRE(compiled.variants.size()==2);
    const auto& moved=compiled.execution(alias->instances.front()->id().id);
    const auto& previous=old_table->paths.begin()->second.execution(alias->instances.front()->id().id);
    CHECK(moved.placement.origin.x()>previous.placement.origin.x()+9);
    print.set_plate_origin(Vec3d(1.1234567,2.7654321,0));
    CHECK_FALSE(print.fiber_execution_plans_current(moved_table));
    CHECK(print.objects().front()->is_step_done(posInfill));
    print.process(nullptr,true);
    CHECK(print.fiber_execution_plans()->paths.begin()->second.context->transform.xy_offset==Vec2d(1.1234567,2.7654321));
}

TEST_CASE("fixed fiber Fill results are reused only for their exact run inputs", "[ContinuousFiber][OrcaIsolation][FiberEarlyExecution]")
{
    const int scenario=GENERATE(0,1,2,3); // fixed, auto, changed Z, early height failures
    const bool shared_alias=GENERATE(false,true);
    CAPTURE(scenario,shared_alias);
    auto config=masked_print_config();
    config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");config.set_deserialize_strict("bottom_shell_thickness","0");
    config.set_deserialize_strict("filament_printable","3,3");
    config.set_deserialize_strict("extruder_printable_height","100,100");
    auto* areas=new ConfigOptionPointsGroups;const Vec2ds bed{{0,0},{200,0},{200,200},{0,200}};
    areas->values={bed,bed};config.set_key_value("extruder_printable_area",areas);
    if(scenario==1) {
        config.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(fmmAutoForFlush));
        config.set_key_value("toolhead_filament_capacity",new ConfigOptionInts);
        config.set_deserialize_strict("filament_slots_bound_to_physical_tools","0");
    }
    if(scenario==3) {
        config.set_deserialize_strict("extruder_printable_height","100,0.3");
        config.set_deserialize_strict("fiber_z_hop_height","0");
    }
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,1));
    object->config.set_key_value("extruder",new ConfigOptionInt(1));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    object->add_instance()->set_offset(Vec3d(85,60,0));
    if(shared_alias) {
        auto* alias=model.add_object(*object);auto mesh=object->volumes.front()->mesh_ptr();alias->volumes.front()->set_mesh(mesh);
        alias->instances[0]->set_offset(Vec3d(110,60,0));alias->instances[1]->set_offset(Vec3d(135,60,0));
    }
    Print print;print.is_BBL_printer()=false;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);
    bool changed_z=false;
    print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
        if(scenario>=2 && status.percent==71 && !changed_z) {
            changed_z=true;for(auto* layer:print.get_object(0)->layers()) {
                if(scenario==3)layer->print_z*=.1;
                else layer->print_z+=.01;
            }
        }
    });
    print.process();
    if(scenario>=2)REQUIRE(changed_z);
    if(shared_alias)REQUIRE(print.objects()[1]->get_shared_object()==print.objects()[0]);
    const auto table=print.fiber_execution_plans();REQUIRE(table);REQUIRE_FALSE(table->paths.empty());
    size_t variants=0;
    for(const auto& item:table->paths) {
        const auto& path=item.second;REQUIRE(path.variants.size()==(shared_alias?4:2));variants+=path.variants.size();
        for(const auto& execution:path.variants) {
            FiberExecutionStatistics oracle_stats;
            const auto oracle=compile_certified_fiber_execution(*path.geometry,*path.context,
                execution->placement,execution->layer_z,oracle_stats);
            CHECK(equal_fiber_commands(*execution->commands,*oracle.commands));
            CHECK(execution->entry_targets==oracle.entry_targets);
            CHECK(execution->entry_start==oracle.entry_start);
            CHECK(execution->prefeed_dE==oracle.prefeed_dE);CHECK(execution->prefeed_F==oracle.prefeed_F);
        }
    }
    const auto cold=print.fiber_execution_reuse_statistics();
    CHECK(cold.early_compile_calls==(scenario==1?0:variants));
    CHECK(cold.early_successful_variants+cold.early_failed_compile_calls==cold.early_compile_calls);
    if(scenario==3)CHECK(cold.early_failed_compile_calls>0);
    else CHECK(cold.early_failed_compile_calls==0);
    CHECK(cold.reused_variants==(scenario==0?variants:0));
    CHECK(cold.late_compile_calls==(scenario==0?0:variants));
    CHECK(cold.late_successful_variants==cold.late_compile_calls);CHECK(cold.late_failed_compile_calls==0);
    // Cached Fill is never rerun just to populate an early table, including
    // moved placements and execution-only configuration/frame changes.
    SECTION("cached unchanged") {}
    SECTION("cached instance moved") {
        object->instances.front()->set_offset(Vec3d(65,60,0));print.apply(model,config);
    }
    SECTION("cached plate frame changed") {print.set_plate_origin(Vec3d(1.1234567,2.7654321,0));}
    SECTION("cached machine conversion changed") {
        config.set_deserialize_strict("toolhead_fiber_e_units_per_mm","1,2");print.apply(model,config);
    }
    print.set_status_silent();
    REQUIRE(print.objects().front()->is_step_done(posInfill));
    print.process(nullptr,true);
    const auto cached=print.fiber_execution_reuse_statistics();
    CHECK(cached.early_compile_calls==0);CHECK(cached.early_successful_variants==0);
    CHECK(cached.early_failed_compile_calls==0);CHECK(cached.reused_variants==0);
    CHECK(cached.late_compile_calls==variants);CHECK(cached.late_successful_variants==variants);
}

TEST_CASE("failed early fiber runs cannot publish or carry results into a restart", "[ContinuousFiber][OrcaIsolation][FiberEarlyExecution]")
{
    const int failure=GENERATE(0,1,2); // cancellation, successor exception, revoked epoch
    auto config=masked_print_config();config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");config.set_deserialize_strict("bottom_shell_thickness","0");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,1));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    Print print;print.is_BBL_printer()=false;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);
    bool interrupted=false;
    print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
        if(!interrupted && status.percent==(failure==1?75:71)) {
            interrupted=true;
            REQUIRE(print.objects().front()->is_step_done(posInfill));
            if(failure==0){print.cancel_internal();throw CanceledException();}
            if(failure==1)throw std::runtime_error("successor failed after early fiber compilation");
            print.invalidate_fiber_execution_plans(false);
        }
    });
    CHECK_THROWS(print.process());REQUIRE(interrupted);CHECK_FALSE(print.fiber_execution_plans());
    const auto revoked=print.fiber_execution_reuse_statistics();
    CHECK(revoked.early_compile_calls==0);CHECK(revoked.reused_variants==0);CHECK(revoked.late_compile_calls==0);
    GCode generator;
    const auto filename=(boost::filesystem::temp_directory_path()/
        boost::filesystem::unique_path("fiber-early-revoked-%%%%.gcode")).string();
    CHECK_THROWS_AS(generator.do_export(&print,filename.c_str(),nullptr,nullptr),FiberExecutionError);
    print.set_status_silent();print.restart();print.process(nullptr,true);
    const auto recovered=print.fiber_execution_plans();REQUIRE(recovered);REQUIRE_FALSE(recovered->paths.empty());
    size_t variants=0;for(const auto& item:recovered->paths)variants+=item.second.variants.size();
    const auto stats=print.fiber_execution_reuse_statistics();
    CHECK(stats.early_compile_calls+stats.late_compile_calls==variants);
    CHECK(stats.reused_variants+stats.late_successful_variants==variants);
}

TEST_CASE("machine failures are evaluated after geometry finalization", "[ContinuousFiber][Finalization]")
{
    auto config=ContinuousFiberConfig{};
    auto context=fiber_fixture_context();context->config.toolhead_fiber_e_units_per_mm.values.assign(8,1e-9);
    auto candidate=straight_path(5,5,45,5);
    const ExPolygons domain{rectangle(0,0,50,20)};
    const auto result=finalize_independent(candidate,domain,config,test_id());
    REQUIRE(result.prepared); // Geometry is independent of the machine's E precision.
    CHECK_THROWS_AS(compile_fiber_execution(result.prepared->draft(),*context,context->placements.front(),0),FiberExecutionError);
    CHECK_NOTHROW(FiberPathValidator::validate({&candidate},domain,config,FiberPathPurpose::Infill,
        erContinuousFiberInfill,test_id().parent.domain));
}

TEST_CASE("resin-only cached processing keeps the ordinary stage lifecycle", "[ContinuousFiber][OrcaIsolation]")
{
    // A machine may retain a fiber tool and its settings while this task uses
    // ordinary resin only. Hardware capability must not activate fiber work.
    const bool fiber_hardware=GENERATE(false,true);
    auto config=masked_print_config();
    config.set_deserialize_strict("generate_reinforced_infills","0");
    config.set_deserialize_strict("generate_reinforced_perimeters","0");
    if(!fiber_hardware) {
        config.set_deserialize_strict("filament_process_type","thermoplastic;thermoplastic");
        config.set_deserialize_strict("toolhead_process_capabilities","thermoplastic;thermoplastic");
    }
    config.set_deserialize_strict("ironing_type","top");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(12,12,.8));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    Print print;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);print.process();
    REQUIRE(print.objects().size()==1);
    const auto* sliced=print.objects().front();
    const auto infill=sliced->step_state_with_timestamp(posInfill).timestamp;
    const auto ironing=sliced->step_state_with_timestamp(posIroning).timestamp;
    const auto wipe=print.step_state_with_timestamp(psWipeTower).timestamp;
    REQUIRE(sliced->is_step_done(posInfill));REQUIRE(sliced->is_step_done(posIroning));
    REQUIRE(print.is_step_done(psWipeTower));
    CHECK_FALSE(print.fiber_execution_required());
    CHECK_FALSE(sliced->fiber_geometry_context());CHECK_FALSE(print.fiber_execution_plans());
    for(int retry=0;retry<2;++retry) {
        print.process(nullptr,true);
        CHECK(sliced->step_state_with_timestamp(posInfill).timestamp==infill);
        CHECK(sliced->step_state_with_timestamp(posIroning).timestamp==ironing);
        CHECK(print.step_state_with_timestamp(psWipeTower).timestamp==wipe);
        CHECK_FALSE(sliced->fiber_geometry_context());CHECK_FALSE(print.fiber_execution_plans());
    }
    SECTION("ordinary export-only settings keep cached tool ordering") {
        // These are G-code-only options in HEAD, but also belong to the fiber
        // execution key. A resin task must retain the original invalidation.
        const std::string key=GENERATE(std::string("printable_area"),std::string("pressure_advance"));
        config.set_deserialize_strict(key,key=="printable_area"?"0x0,300x0,300x300,0x300":"0.07,0");
        // Export has not run, so its state is already INVALID. HEAD reports a
        // changed configuration without invalidating any completed stage.
        CHECK(print.apply(model,config)==PrintBase::APPLY_STATUS_CHANGED);
        REQUIRE(print.is_step_done(psWipeTower));
        CHECK(print.step_state_with_timestamp(psWipeTower).timestamp==wipe);
        print.process(nullptr,true);
    }
    SECTION("ordinary coordinate setters retain their original assignment behavior") {
        print.set_plate_origin(Vec3d(1,2,0));
        print.set_belt_coordinate_system(BeltCoordinateSystem::create(45,250));
        REQUIRE(print.is_step_done(psWipeTower));
        CHECK(print.step_state_with_timestamp(psWipeTower).timestamp==wipe);
        print.process(nullptr,true);
    }
    CHECK(sliced->step_state_with_timestamp(posInfill).timestamp==infill);
    CHECK(sliced->step_state_with_timestamp(posIroning).timestamp==ironing);
    CHECK(print.step_state_with_timestamp(psWipeTower).timestamp==wipe);
    CHECK_FALSE(sliced->fiber_geometry_context());CHECK_FALSE(print.fiber_execution_plans());
    CHECK_FALSE(print.fiber_execution_required());
}

TEST_CASE("fiber coordinate invalidation preserves cached ironing and reports invalidation", "[ContinuousFiber][Finalization]")
{
    auto config=masked_print_config();config.set_deserialize_strict("ironing_type","top");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,.8));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    Print print;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);print.process();
    const auto ironing_length=[](const Print& target) {
        double length=0;
        const auto sum=[&](const auto& self,const ExtrusionEntity& entity)->void {
            if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(&entity))
                for(const auto* child:collection->entities)self(self,*child);
            else if(entity.role()==erIroning)length+=entity.length();
        };
        for(const auto* object:target.objects())for(const auto* layer:object->layers())
            for(const auto* region:layer->regions())sum(sum,region->fills);
        return length;
    };
    const auto before=ironing_length(print);REQUIRE(before>0);
    config.set_deserialize_strict("toolhead_fiber_e_units_per_mm","1,2");
    CHECK(print.apply(model,config)==PrintBase::APPLY_STATUS_INVALIDATED);
    CHECK(print.objects().front()->is_step_done(posInfill));
    print.process(nullptr,true);
    CHECK(ironing_length(print)==Catch::Approx(before).margin(1e-6));
    config.set_deserialize_strict("extruder_offset","0x0,0.1234567x0.7654321");
    CHECK(print.apply(model,config)==PrintBase::APPLY_STATUS_INVALIDATED);
    print.process(nullptr,true);
    Print fresh;fresh.set_plate_origin(Vec3d::Zero());fresh.apply(model,config);fresh.process();
    CHECK(ironing_length(print)==Catch::Approx(ironing_length(fresh)).margin(1e-6));
}

TEST_CASE("disabling fiber rebuilds cached fills and ironing like a fresh resin print", "[ContinuousFiber][Finalization][OrcaIsolation]")
{
    auto config=masked_print_config();config.set_deserialize_strict("ironing_type","top");
    config.set_deserialize_strict("generate_reinforced_perimeters","1");
    config.set_deserialize_strict("reinforced_perimeters_filament","2");
    config.set_deserialize_strict("fiber_contour_bend_radius","0.3");
    config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");
    config.set_deserialize_strict("bottom_shell_thickness","0");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,2));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    // Model::add_object(copy) supplies this key on the clone. Give the source
    // the same explicit material so original shared-object equality applies.
    object->config.set_key_value("extruder",new ConfigOptionInt(1));
    auto* alias=model.add_object(*object);
    auto shared_mesh=object->volumes.front()->mesh_ptr();
    alias->volumes.front()->set_mesh(shared_mesh);
    alias->instances.front()->set_offset(Vec3d(100,60,0));
    size_t frontend_cancel_calls=0,retry_cancel_calls=0;
    Print cached;cached.set_plate_origin(Vec3d::Zero());cached.apply(model,config);cached.process();
    REQUIRE(cached.objects().size()==2);
    REQUIRE(cached.objects()[1]->get_shared_object()==cached.objects()[0]);
    REQUIRE(cached.objects()[0]->layers().front()==cached.objects()[1]->layers().front());
    // Preserve entity order and exact points, so equal total length cannot hide stale paths.
    const auto signature=[](const Print& print) {
        std::ostringstream paths;
        const auto visit=[&](const auto& self,const ExtrusionEntity& entity)->void {
            if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(&entity)) {
                paths<<"[";for(const auto* child:collection->entities)self(self,*child);paths<<"]";
            } else {
                paths<<int(entity.role())<<":";
                for(const auto& point:entity.as_polyline().points)paths<<point.x()<<","<<point.y()<<";";
            }
        };
        for(const auto* object:print.objects())for(const auto* layer:object->layers())
            for(const auto* region:layer->regions())visit(visit,region->fills);
        return paths.str();
    };
    const auto count_fiber=[](const Print& print) {
        size_t count=0;
        const auto visit=[&](const auto& self,const ExtrusionEntity& entity)->void {
            if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(&entity))
                for(const auto* child:collection->entities)self(self,*child);
            else if(dynamic_cast<const ExtrusionFiberPath*>(&entity))++count;
        };
        for(const auto* object:print.objects())for(const auto* layer:object->layers())
            for(const auto* region:layer->regions())visit(visit,region->fills);
        return count;
    };
    REQUIRE(count_fiber(cached)>0);
    config.set_deserialize_strict("generate_reinforced_infills","0");
    config.set_deserialize_strict("generate_reinforced_perimeters","0");
    cached.set_cancel_callback([&]{++frontend_cancel_calls;});
    CHECK(cached.apply(model,config)==PrintBase::APPLY_STATUS_INVALIDATED);
    REQUIRE(frontend_cancel_calls>0);
    REQUIRE(cached.fiber_execution_required());
    cached.set_cancel_callback([]{});
    SECTION("resume after cancellation during infill simplification") {
        // The original optimization callback is a real process boundary after
        // Fill and ironing. Exercise the worker entrypoint and pending cleanup,
        // without adding an ordinary callback solely for this regression.
        bool interrupted=false;
        cached.set_status_callback([&](const PrintBase::SlicingStatus& status) {
            const auto* sliced=cached.objects().front();
            if(status.percent==75 && !interrupted && sliced->is_step_done(posInfill) &&
                sliced->is_step_done(posIroning) && !sliced->is_step_done(posSimplifyInfill)) {
                interrupted=true;cached.cancel_internal();throw CanceledException();
            }
        });
        CHECK_THROWS_AS(cached.process(nullptr,true),CanceledException);
        REQUIRE(interrupted);
        REQUIRE(cached.objects().front()->is_step_done(posInfill));
        REQUIRE(cached.objects().front()->is_step_done(posIroning));
        REQUIRE_FALSE(cached.objects().front()->is_step_done(posSimplifyInfill));
        REQUIRE(cached.fiber_execution_required());
        cached.set_status_silent();
        cached.restart();
        REQUIRE(count_fiber(cached)==0);
        REQUIRE_FALSE(cached.fiber_execution_plans());
        // Cancellation is cleared: this must fail because the active cleanup
        // has no published table, even though its current fills are all resin.
        const auto output=std::filesystem::temp_directory_path()/"fiber-pending-disable.gcode";
        {std::ofstream existing(output);existing<<"previous completed output\n";}
        GCode generator;GCodeProcessorResult result;
        CHECK_THROWS_WITH(generator.do_export(&cached,output.string().c_str(),&result),
            "Fiber execution table is missing or invalidated; process must complete before export");
        std::ifstream preserved(output);std::string previous;
        std::getline(preserved,previous);CHECK(previous=="previous completed output");
        preserved.close();std::error_code error;std::filesystem::remove(output,error);
    }
    SECTION("normal cached rebuild") {}
    // A host cancellation callback stops the worker. Local recovery must never
    // invoke this frontend callback from inside the worker being recovered.
    cached.set_cancel_callback([&]{++retry_cancel_calls;cached.cancel_internal();});
    CHECK_NOTHROW(cached.process(nullptr,true));
    CHECK(retry_cancel_calls==0);
    cached.set_cancel_callback([]{});
    CHECK_FALSE(cached.objects().front()->fiber_geometry_context());
    CHECK_FALSE(cached.objects()[1]->fiber_geometry_context());
    CHECK_FALSE(cached.fiber_execution_plans());
    CHECK_FALSE(cached.fiber_execution_required());
    CHECK(count_fiber(cached)==0);
    Print fresh;fresh.set_plate_origin(Vec3d::Zero());fresh.apply(model,config);fresh.process();
    CHECK(signature(cached)==signature(fresh));
    const auto wipe=cached.step_state_with_timestamp(psWipeTower).timestamp;
    const auto fills=signature(cached);
    CHECK_NOTHROW(cached.process(nullptr,true));
    CHECK_FALSE(cached.fiber_execution_plans());
    CHECK(cached.step_state_with_timestamp(psWipeTower).timestamp==wipe);
    CHECK(signature(cached)==fills);
}

TEST_CASE("fiber tasks with no accepted entities still require a completed execution table", "[ContinuousFiber][OrcaIsolation]")
{
    auto config=masked_print_config();
    // Preserve enabled fiber policy while rejecting every short source. A
    // finished empty task differs from an interrupted task missing its table.
    config.set_deserialize_strict("fiber_minimum_path_length","10000");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(12,12,.8));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    Print print;print.is_BBL_printer()=false;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);print.process();
    REQUIRE(print.fiber_execution_required());
    const auto table=print.fiber_execution_plans();REQUIRE(table);REQUIRE(table->paths.empty());
    const auto output=std::filesystem::temp_directory_path()/"fiber-empty-certified-task.gcode";
    GCode generator;GCodeProcessorResult result;
    REQUIRE_NOTHROW(generator.do_export(&print,output.string().c_str(),&result));
    const auto read_output=[&] {
        std::ifstream input(output);
        return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    };
    const auto completed=read_output();REQUIRE_FALSE(completed.empty());
    CHECK(completed.find(";FIBER_BEGIN ")==std::string::npos);
    print.set_plate_origin(Vec3d(1,0,0));
    REQUIRE_FALSE(print.fiber_execution_plans());
    CHECK_THROWS_WITH(generator.do_export(&print,output.string().c_str(),&result),
        "Fiber execution table is missing or invalidated; process must complete before export");
    CHECK(read_output()==completed);
    std::error_code error;std::filesystem::remove(output,error);
}

TEST_CASE("fiber writer formats the checked feedrate for XYZ and every spiral segment", "[ContinuousFiber][Finalization]")
{
    PrintConfig config;config.travel_speed.value=500;config.initial_layer_travel_speed=ConfigOptionFloatOrPercent(100,false);
    config.travel_speed_z.value=12;config.z_hop.values={1.};config.enable_arc_fitting.value=false;
    config.retract_lift_above.values={0.};config.retract_lift_below.values={0.};
    const auto initialize=[&](GCodeWriter& writer) {
        writer.apply_print_config(config);writer.set_extruders({0});writer.toolchange(0);
        writer.set_position(Vec3d(10,10,1));writer.set_current_position_clear(true);
    };
    SECTION("actual XYZ F is shared by check and formatter") {
        GCodeWriter writer;initialize(writer);
        const GCodeWriter::ScopedXYZPrecision precision(writer,6);
        double requested=0;
        const GCodeWriter::ScopedFiberMotionCheck check(writer,[&](const Vec3d&,const Vec3d&,EMovePathType,
            const Vec2d&,double,double& speed){requested=speed;speed=50;});
        const auto text=writer.travel_to_xyz(Vec3d(20,20,2),"checked XYZ",true);
        CHECK(requested==500);CHECK(text.find("F3000")!=std::string::npos);
    }
    SECTION("spiral final segment has its own modal F") {
        GCodeWriter writer;initialize(writer);
        const GCodeWriter::ScopedXYZPrecision precision(writer,6);
        std::vector<double> speeds;
        const GCodeWriter::ScopedFiberMotionCheck check(writer,[&](const Vec3d&,const Vec3d& to,EMovePathType,
            const Vec2d&,double,double& speed){speed=to.z()>1.9999?1.5:10.;speeds.push_back(speed);});
        const auto text=writer.eager_lift(LiftType::SpiralLift);
        REQUIRE(speeds.size()>=4);
        std::istringstream input(text);std::string line;double modal_f=0;size_t motion=0;
        const std::regex feed("(?:^| )F([-0-9.]+)");std::smatch match;
        while(std::getline(input,line)) {
            if(std::regex_search(line,match,feed))modal_f=std::stod(match[1]);
            if(line.rfind("G1 ",0)==0 && line.find(" X")!=std::string::npos) {
                REQUIRE(motion<speeds.size());CHECK(modal_f==Catch::Approx(speeds[motion++]*60).margin(.001));
            }
        }
        CHECK(motion==speeds.size());CHECK(modal_f==90);
    }
    SECTION("ordinary spiral retains its single modal feed command") {
        GCodeWriter writer;initialize(writer);
        const auto text=writer.eager_lift(LiftType::SpiralLift);
        std::istringstream input(text);std::string line;size_t feeds=0;
        while(std::getline(input,line))if(line.rfind("G1 F",0)==0)++feeds;
        CHECK(feeds==1);
    }
}

TEST_CASE("fiber execution tables preserve automatic and manual resin mappings across cached runs", "[ContinuousFiber][R4][OrcaIsolation]")
{
    auto config=masked_print_config();
    // The generic full defaults have zero per-tool printable height. Manual
    // ownership bypasses that geometric filter; automatic mapping requires a
    // real printable workspace for both the resin and fiber candidates.
    config.set_deserialize_strict("filament_printable","3,3");
    config.set_deserialize_strict("extruder_printable_height","100,100");
    auto* printable_areas=new ConfigOptionPointsGroups;
    const Vec2ds bed{{0,0},{200,0},{200,200},{0,200}};
    printable_areas->values={bed,bed};config.set_key_value("extruder_printable_area",printable_areas);
    config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");
    config.set_deserialize_strict("bottom_shell_thickness","0");
    const auto sequence=GENERATE(PrintSequence::ByLayer,PrintSequence::ByObject);
    config.set_key_value("print_sequence",new ConfigOptionEnum<PrintSequence>(sequence));
    const auto mapping_mode=GENERATE(fmmManual,fmmAutoForFlush);
    config.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(mapping_mode));
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,2));
    object->add_instance()->set_offset(Vec3d(60,60,0));
    Print print;print.is_BBL_printer()=false;print.apply(model,config);print.process();
    const auto first=print.fiber_execution_plans();REQUIRE(first);REQUIRE_FALSE(first->paths.empty());
    CHECK(first->filament_map==std::vector<int>{1,2});
    CHECK(print.config().filament_map.values==std::vector<int>{1,2});
    const auto geometry=first->paths.begin()->second.geometry;
    print.process(nullptr,true);
    const auto next=print.fiber_execution_plans();REQUIRE(next);REQUIRE_FALSE(next->paths.empty());
    CHECK(next->paths.begin()->second.geometry==geometry);
    CHECK(next->filament_map.front()==first->filament_map.front());
    CHECK(print.config().filament_map.values.front()==1);
    CHECK_FALSE(print.fiber_execution_plans_current(first));
    CHECK(next->execution_generation>first->execution_generation);
    bool delivered=false;
    CHECK_THROWS_AS(print.with_current_fiber_execution_plans(first,[&]{delivered=true;}),FiberExecutionError);
    CHECK_FALSE(delivered);
    CHECK_NOTHROW(print.with_current_fiber_execution_plans(next,[&]{delivered=true;}));
    CHECK(delivered);
}

TEST_CASE("BBL fiber candidates preserve original solver used material decisions",
    "[ContinuousFiber][OrcaIsolation][BBLFiberMapping]")
{
    // Isolate unused capacity, unused TPU, used TPU, multiple used TPU,
    // incompatible TPU ownership, and ordinary material aliases respectively.
    const int scenario = GENERATE(0,1,2,3,4,5);
    CAPTURE(scenario);
    const bool used_tpu=scenario>=2 && scenario<=4;
    const bool extra_used_material=scenario==3 || scenario==5;
    const bool conflicting_master=scenario==4;
    const std::string types=scenario==1?"PLA;CF;TPU;TPU":
        (used_tpu?(scenario==3?"TPU;CF;TPU;ABS":"TPU;CF;PETG;ABS"):
        (scenario==5?"PLA;CF;PLA;ABS":"PLA;CF;PETG;ABS"));
    auto config=masked_print_config();
    const std::vector<std::pair<std::string,std::string>> options {
        {"filament_map","1,2,1,1"},
        {"filament_colour",scenario==5?"#FFFFFF;#000000;#FFFFFF;#0000FF":"#FFFFFF;#000000;#FF0000;#0000FF"},
        {"filament_ids",scenario==5?"used-resin;used-fiber;used-resin;unused-b":"used-resin;used-fiber;unused-a;unused-b"},
        {"filament_type",types},
        {"filament_process_type","thermoplastic;continuous_fiber;thermoplastic;thermoplastic"},
        {"filament_diameter","1.75,1.75,1.75,1.75"}, {"filament_fiber_feed_correction","1,1,1,1"},
        {"filament_is_support","0,0,0,0"}, {"filament_printable","3,3,3,3"},
        {"filament_slots_bound_to_physical_tools","0"},
        {"extruder_ams_count",scenario==0 || scenario==5?"1#1;1#1":"4#1;4#1"},
        {"master_extruder_id",conflicting_master?"2":"1"},
        {"printable_area","0x0,200x0,200x200,0x200"}, {"extruder_printable_height","100,100"},
        {"fiber_layer_height_ratio","1"}, {"top_shell_thickness","0"}, {"bottom_shell_thickness","0"}
    };
    for(const auto& [key,value]:options)config.set_deserialize_strict(key,value);
    config.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(fmmAutoForFlush));
    config.set_key_value("toolhead_filament_capacity",new ConfigOptionInts);
    auto* matrix=new ConfigOptionFloats;matrix->values.assign(2*4*4,0.);
    config.set_key_value("flush_volumes_matrix",matrix);
    auto* areas=new ConfigOptionPointsGroups;
    const Vec2ds bed{{0,0},{200,0},{200,200},{0,200}};
    areas->values={bed,bed};config.set_key_value("extruder_printable_area",areas);
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,2));
    object->config.set("extruder",1);object->add_instance()->set_offset(Vec3d(60,60,0));
    if(extra_used_material) {
        auto* other=model.add_object();other->add_volume(make_cube(20,20,2));
        other->config.set("extruder",3);other->add_instance()->set_offset(Vec3d(100,60,0));
    }
    Print print;print.is_BBL_printer()=true;print.apply(model,config);
    REQUIRE_FALSE(print.certified_fiber_filament_map().size());
    const std::vector<unsigned> used=extra_used_material?std::vector<unsigned>{0,1,2}:std::vector<unsigned>{0,1};
    const auto physical=print.get_physical_unprintable_filaments(used);
    const std::vector<std::set<int>> geometric(2);
    // No task table exists: this calls the real original mode solver. The
    // declared used set is checked against the actual sliced entities below.
    const auto original=ToolOrdering::get_recommended_filament_maps({used},&print,
        fmmAutoForFlush,physical,geometric);
    REQUIRE(original.size()==4);
    REQUIRE(original[0]==(conflicting_master?1:0));
    REQUIRE(original[1]==(conflicting_master?0:1));
    if(extra_used_material)REQUIRE(original[2]==0);
    std::string process_error;
    try { print.process(); }
    catch(const std::exception& error) { process_error=error.what(); }
    INFO("Print.process result: " << process_error);
    size_t accepted_fibers=0;
    const auto inspect=[&](const auto& self,const ExtrusionEntity& entity)->void {
        if(const auto* fiber=dynamic_cast<const ExtrusionFiberPath*>(&entity)) {
            REQUIRE(fiber->prepared_path());++accepted_fibers;
        } else if(const auto* collection=dynamic_cast<const ExtrusionEntityCollection*>(&entity))
            for(const auto* child:collection->entities)self(self,*child);
    };
    for(const auto* sliced:print.objects())for(const auto* layer:sliced->layers())
        for(const auto* region:layer->regions())inspect(inspect,region->fills);
    REQUIRE(accepted_fibers>0);
    const ToolOrdering actual_usage(print,-1,false);
    REQUIRE(actual_usage.all_extruders()==used);
    if(conflicting_master) {
        CHECK(process_error.find("No permitted tool mapping")!=std::string::npos);
        CHECK_FALSE(print.fiber_execution_plans());
        return;
    }
    REQUIRE(process_error.empty());
    const auto table=print.fiber_execution_plans();REQUIRE(table);REQUIRE_FALSE(table->paths.empty());
    for(unsigned material:used)CHECK(table->filament_map.at(material)==original.at(material)+1);
    CHECK(print.config().filament_map.values==table->filament_map);
}

TEST_CASE("a revoked process run cannot recapture a newer execution generation", "[ContinuousFiber][R4][OrcaIsolation]")
{
    auto config=masked_print_config();config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");config.set_deserialize_strict("bottom_shell_thickness","0");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,2));
    object->add_instance()->set_offset(Vec3d(60,60,0));Print print;print.apply(model,config);
    bool revoked=false;
    print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
        if(status.percent==35 && !revoked){revoked=true;print.invalidate_fiber_execution_plans(false);}
    });
    CHECK_THROWS_AS(print.process(),FiberExecutionError);CHECK(revoked);
    CHECK_FALSE(print.fiber_execution_plans());
    print.set_status_silent();print.process(nullptr,true);
    REQUIRE(print.fiber_execution_plans());CHECK_FALSE(print.fiber_execution_plans()->paths.empty());
    const auto published=print.fiber_execution_plans();
    print.set_belt_coordinate_system(BeltCoordinateSystem::create(45,250));
    CHECK_FALSE(print.fiber_execution_plans_current(published));
}

TEST_CASE("material bitmap expansion is restricted to enabled fiber tasks", "[ContinuousFiber][R4][OrcaIsolation]")
{
    const bool reinforced=GENERATE(false,true);
    auto config=DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict("nozzle_diameter","0.4,0.4,0.4");
    config.set_deserialize_strict("physical_extruder_map","0,1,2");
    config.set_deserialize_strict("filament_printable","1");
    if(reinforced) {
        config.set_deserialize_strict("generate_reinforced_infills","1");
        config.set_deserialize_strict("filament_process_type","continuous_fiber");
        config.set_deserialize_strict("toolhead_process_capabilities","continuous_fiber;continuous_fiber;continuous_fiber");
    }
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,1));object->add_instance();
    Print print;print.apply(model,config);
    const auto masks=print.get_physical_unprintable_filaments({0});REQUIRE(masks.size()==3);
    CHECK(masks[0].count(0)==0);CHECK(masks[1].count(0)==1);
    // HEAD records only the first forbidden tool. Preserve that ordinary
    // behavior here; the stricter complete bitmap belongs to fiber planning.
    CHECK(masks[2].count(0)==size_t(reinforced));
}

TEST_CASE("component coverage preserves short butt caps and rejects tiny distorted edges", "[ContinuousFiber][R4]")
{
    FiberPathDraft draft;draft.width_mm=2;draft.execution_deviation_mm=.000211314;
    draft.reference_deposition=std::make_shared<const Polyline>(
        Polyline(Points{Point::new_scale(0,0),Point::new_scale(.1,0),Point::new_scale(.1,10)}));
    const auto bounds=fiber_geometry_coverage_bounds(draft);
    const Point behind_butt=Point::new_scale(-.5,-.5);
    CHECK(std::none_of(bounds.second.begin(),bounds.second.end(),[&](const ExPolygon& p){return p.contains(behind_butt);}));
    CHECK_FALSE(bounds.first.empty());CHECK(diff_ex(bounds.first,bounds.second).empty());

    auto config=ContinuousFiberConfig{};config.cut_to_contact_length_mm=0;config.z_hop_height_mm=0;
    const auto result=finalize_independent(straight_path(0,5,100,5),{rectangle(-5,0,110,15)},config,test_id());
    REQUIRE(result.prepared);auto context=fiber_fixture_context();
    auto execution=compile_fiber_execution(result.prepared->draft(),*context,context->placements.front(),0);
    REQUIRE(certify_fiber_coverage(*result.prepared,execution,*context));
    auto commands=std::make_shared<FrozenFiberCommands>(*execution.commands);
    auto& first=commands->spans.front();REQUIRE_FALSE(first.commands.empty());
    auto micro=first.commands.front();micro.target=first.from+Vec2d(.000001,.000001);
    first.commands.insert(first.commands.begin(),micro);execution.commands=commands;
    // One grid of centerline displacement is much less than delta, but its
    // short first edge rotates the butt cross-section. Hausdorff is insufficient.
    CHECK_THROWS_AS(certify_fiber_coverage(*result.prepared,execution,*context),FiberExecutionError);
}

TEST_CASE("native coverage rejects an off-circle rounded endpoint", "[ContinuousFiber][R4]")
{
    auto config=ContinuousFiberConfig{};config.cut_to_contact_length_mm=0;config.z_hop_height_mm=0;
    const auto result=finalize_independent(straight_path(0,5,100,5),{rectangle(-5,0,110,15)},config,test_id());
    REQUIRE(result.prepared);auto context=fiber_fixture_context();
    auto execution=compile_fiber_execution(result.prepared->draft(),*context,context->placements.front(),0);
    auto commands=std::make_shared<FrozenFiberCommands>(*execution.commands);
    REQUIRE_FALSE(commands->spans.front().commands.empty());
    auto& first=commands->spans.front();auto& command=first.commands.front();
    command.target=first.from+Vec2d(.0001,.0001);command.arc_index=0;
    commands->arcs.push_back({Vec2d(0,10),true});execution.commands=commands;
    try {certify_fiber_coverage(*result.prepared,execution,*context);FAIL("Off-circle arc was certified");}
    catch(const FiberExecutionError& error){CHECK(error.failure==FiberExecutionFailure::Quantization);}
}

TEST_CASE("component guarantees leave the uncertain seam between touching strands", "[ContinuousFiber][R4]")
{
    FiberPathDraft draft;draft.width_mm=2;draft.execution_deviation_mm=.000211314;
    draft.reference_deposition=std::make_shared<const Polyline>(Polyline(Points{
        Point::new_scale(0,0),Point::new_scale(10,0),Point::new_scale(10,2),Point::new_scale(0,2)}));
    const auto bounds=fiber_geometry_coverage_bounds(draft);const auto seam=Point::new_scale(5,1);
    CHECK(std::none_of(bounds.first.begin(),bounds.first.end(),[&](const ExPolygon& p){return p.contains(seam);}));
    CHECK(std::any_of(bounds.second.begin(),bounds.second.end(),[&](const ExPolygon& p){return p.contains(seam);}));
}

TEST_CASE("arc normal bands retain negative signed radii", "[ContinuousFiber][R4]")
{
    FiberPathDraft draft;draft.width_mm=2;draft.execution_deviation_mm=.000211314;
    auto context=std::make_shared<FiberGeometryContext>();context->export_arcs=true;draft.geometry_context=context;
    FiberMotionSpan span;span.geometry.points={Point3::new_scale(.3,0,0),Point3::new_scale(0,.3,0)};
    span.geometry.fitting_result={{0,1,EMovePathType::Arc_move_ccw,ArcSegment(Point(0,0),scale_(.3),
        span.geometry.points.front().to_point(),span.geometry.points.back().to_point(),ArcDirection::Arc_Dir_CCW)}};
    draft.spans.push_back(span);const auto bounds=fiber_geometry_coverage_bounds(draft);
    const auto negative=Point::new_scale(-.1,-.1);
    CHECK(std::any_of(bounds.first.begin(),bounds.first.end(),[&](const ExPolygon& p){return p.contains(negative);}));
    CHECK(std::any_of(bounds.second.begin(),bounds.second.end(),[&](const ExPolygon& p){return p.contains(negative);}));
    CHECK(diff_ex(bounds.first,bounds.second).empty());
}

TEST_CASE("fiber belt normalization protects shared layers without changing ordinary normalization", "[ContinuousFiber][R4][belt][OrcaIsolation][FiberEarlyExecution]")
{
    const bool reinforced=GENERATE(false,true);
    auto config=reinforced?masked_print_config():DynamicPrintConfig::full_print_config();
    if(reinforced) {
        config.set_deserialize_strict("fiber_layer_height_ratio","1");
        config.set_deserialize_strict("top_shell_thickness","0");config.set_deserialize_strict("bottom_shell_thickness","0");
    }
    config.set_key_value("printer_structure",new ConfigOptionEnum<PrinterStructure>(PrinterStructure::psBelt));
    config.set_deserialize_strict("enable_support","0");config.set_deserialize_strict("enable_prime_tower","0");
    config.set_deserialize_strict("brim_type","no_brim");
    // A thin tilted box can have no internal fiber region after skin removal.
    // Give the reinforced branch a genuine internal band; require real plans.
    Model model;auto* first=model.add_object();first->add_volume(make_cube(12,12,reinforced?10.:.4));
    first->add_instance()->set_offset(Vec3d(60,60,0));
    first->config.set_key_value("extruder",new ConfigOptionInt(1));
    auto* alias=model.add_object(*first);auto mesh=first->volumes.front()->mesh_ptr();alias->volumes.front()->set_mesh(mesh);
    alias->instances.front()->set_offset(Vec3d(60,60,0));
    Print print;print.set_belt_coordinate_system(BeltCoordinateSystem::create(45,250));print.apply(model,config);
    bool raised=false;
    print.set_status_callback([&](const PrintBase::SlicingStatus& status) {
        if(status.percent==71 && !raised) {
            raised=true;
            for(auto* layer:print.get_object(0)->layers()){layer->slice_z+=5;layer->print_z+=5;}
        }
    });
    print.process();REQUIRE(raised);REQUIRE(print.objects().size()==2);
    REQUIRE(print.objects()[1]->get_shared_object()==print.objects()[0]);
    REQUIRE(print.objects()[0]->layers().front()==print.objects()[1]->layers().front());
    CHECK(print.belt_coordinate_system()->print_origin_s()>4.9);
    const auto& layers=print.objects()[0]->layers();
    const auto first_printed=std::find_if(layers.begin(),layers.end(),[](const Layer* layer){return layer->has_extrusions();});
    REQUIRE(first_printed!=layers.end());const auto* printed=*first_printed;
    const double z=printed->print_z;
    const double expected_z=config.opt_float("initial_layer_print_height")-
        (reinforced?0.:print.belt_coordinate_system()->print_origin_s());
    CHECK(z==Catch::Approx(expected_z).margin(1e-6));
    const auto table=print.fiber_execution_plans();
    const auto reuse=print.fiber_execution_reuse_statistics();
    CHECK(reuse.early_compile_calls==0);CHECK(reuse.reused_variants==0);
    if(reinforced)CHECK(reuse.late_compile_calls>0);
    if(reinforced) {
        REQUIRE(table);
        std::ostringstream diagnostics;
        for(const auto* layer:print.objects()[0]->layers())for(const auto& d:layer->fiber_fill_diagnostics)
            if(!d.reason.empty())diagnostics<<layer->id()<<":"<<d.reason<<"; ";
        INFO(diagnostics.str());
        REQUIRE_FALSE(table->paths.empty());CHECK(table->statistics.swept_coverage>0);
        for(const auto& item:table->paths) {
            CHECK(item.second.variants.size()==2);
            REQUIRE(item.second.context->transform.belt);
            CHECK(item.second.context->transform.belt->print_origin_s()==print.belt_coordinate_system()->print_origin_s());
        }
    } else CHECK_FALSE(table);
    print.set_status_silent();print.process(nullptr,true);
    CHECK(printed->print_z==z);
    CHECK_FALSE(print.fiber_execution_plans_current(table));
    if(reinforced)CHECK(print.fiber_execution_plans()->paths.begin()->second.geometry==table->paths.begin()->second.geometry);
    else CHECK_FALSE(print.fiber_execution_plans());
}

TEST_CASE("native circular fiber retains exact targets IJ and quantized feed", "[ContinuousFiber][R4][NativeArc]")
{
    auto config=ContinuousFiberConfig{};config.cut_to_contact_length_mm=0;config.z_hop_height_mm=0;
    config.start_stabilization_length_mm=0;config.speed_sampling_length_mm=10000;
    config.prefeed_extra_length_mm=.1234567;
    config.infill_min_speed_mm_s=config.infill_max_speed_mm_s=10;
    auto geometry=std::make_shared<FiberGeometryContext>();geometry->export_arcs=true;config.geometry_context=geometry;
    auto source=path_from_points({{40,0},{0,40}});
    source.polyline.fitting_result={{0,1,EMovePathType::Arc_move_ccw,ArcSegment(Point(0,0),scale_(40),
        source.polyline.points[0].to_point(),source.polyline.points[1].to_point(),ArcDirection::Arc_Dir_CCW)}};
    const auto result=finalize_independent(source,{rectangle(-60,-10,50,50)},config,test_id(),
        continuous_fiber_detail::arc_metadata(source.polyline));
    INFO(result.detail);REQUIRE(result.prepared);
    auto context=fiber_fixture_context();context->export_arcs=true;
    const auto execution=compile_fiber_execution(result.prepared->draft(),*context,context->placements.front(),0);
    REQUIRE_FALSE(execution.arcs().empty());
    CHECK_NOTHROW(certify_fiber_coverage(*result.prepared,execution,*context));
    double ideal=result.prepared->start_procedure.prefeed_length_mm,emitted=execution.prefeed_dE;
    for(size_t i=0;i<execution.spans().size();++i) {
        const auto& span=execution.spans()[i];Vec2d from=span.from;
        for(const auto& command:span.commands) {
            const auto& source_span=result.prepared->spans[i];const auto owner=command.source_primitive;
            const Vec2d expected=source_span.geometry.points[owner+1].head<2>().cast<double>()*SCALING_FACTOR;
            CHECK(command.target==Vec2d(GCodeFormatter::quantize(expected.x(),6),GCodeFormatter::quantize(expected.y(),6)));
            double length=(command.target-from).norm();
            if(command.arc_index>=0) {
                const auto& arc=execution.arcs()[command.arc_index];
                const auto& fit=source_span.geometry.fitting_result[owner];
                const Vec2d center=fit.arc_data.center.cast<double>()*SCALING_FACTOR;
                const Vec2d expected_ij=center-from;
                CHECK(arc.ij==Vec2d(GCodeFormatter::quantize(expected_ij.x(),6),GCodeFormatter::quantize(expected_ij.y(),6)));
                CHECK(arc.ccw==(fit.path_type==EMovePathType::Arc_move_ccw));
                const Vec2d a=-arc.ij,b=command.target-from-arc.ij;
                double angle=std::atan2(a.x()*b.y()-a.y()*b.x(),a.dot(b));
                if(arc.ccw && angle<=0)angle+=2*PI;if(!arc.ccw && angle>=0)angle-=2*PI;
                length=std::abs(angle)*arc.ij.norm();
            }
            if(source_span.actively_feeds_fiber())ideal+=length*source_span.edges[owner].feed_mm_per_xy_mm;
            else CHECK(command.dE==0);
            emitted+=command.dE;CHECK(emitted==Catch::Approx(GCodeFormatter::quantize_e(ideal)).margin(1e-10));from=command.target;
        }
    }
}

TEST_CASE("fiber compiler propagates cancellation from inside a long command stream", "[ContinuousFiber][R4]")
{
    auto config=ContinuousFiberConfig{};config.cut_to_contact_length_mm=0;config.z_hop_height_mm=0;
    config.speed_sampling_length_mm=.01;
    const auto result=finalize_independent(straight_path(0,5,100,5),{rectangle(-5,0,110,15)},config,test_id());
    REQUIRE(result.prepared);auto context=fiber_fixture_context();size_t checks=0;
    CHECK_THROWS_AS(compile_fiber_execution(result.prepared->draft(),*context,context->placements.front(),0,[&] {
        if(++checks==3)throw CanceledException();
    }),CanceledException);
    CHECK(checks==3);CHECK_NOTHROW(result.prepared->validate());
}

TEST_CASE("coverage keeps an outer bound when a thin rectangle snaps to a line", "[ContinuousFiber][R4]")
{
    FiberPathDraft draft;draft.width_mm=SCALING_FACTOR;draft.execution_deviation_mm=.000211314;
    draft.reference_deposition=std::make_shared<const Polyline>(Polyline(Points{Point::new_scale(0,0),Point::new_scale(10,0)}));
    const auto bounds=fiber_geometry_coverage_bounds(draft);
    REQUIRE_FALSE(bounds.second.empty());
    CHECK(std::any_of(bounds.second.begin(),bounds.second.end(),[](const ExPolygon& p){return p.contains(Point::new_scale(5,0));}));
}

TEST_CASE("reference coverage cache preserves exact geometry and remains bounded", "[ContinuousFiber][R4][cache]")
{
    FiberPathDraft draft;draft.width_mm=1;draft.execution_deviation_mm=.000211314;
    draft.reference_deposition=std::make_shared<const Polyline>(Polyline(Points{
        Point::new_scale(0,0),Point::new_scale(5,0),Point::new_scale(10,0)}));
    const auto uncached=fiber_geometry_coverage_bounds(draft);
    auto context=std::make_shared<FiberGeometryContext>();context->coverage_cache=make_fiber_geometry_coverage_cache();
    draft.geometry_context=context;
    const auto first=fiber_geometry_coverage_bounds(draft),second=fiber_geometry_coverage_bounds(draft);
    for(const auto* item:{&first,&second}) {
        CHECK(diff_ex(uncached.first,item->first).empty());CHECK(diff_ex(item->first,uncached.first).empty());
        CHECK(diff_ex(uncached.second,item->second).empty());CHECK(diff_ex(item->second,uncached.second).empty());
    }
    auto stats=fiber_geometry_coverage_cache_statistics(context->coverage_cache);
    CHECK(stats.hits==1);CHECK(stats.misses==1);
    draft.reference_groups={{1,1},{2,2}};const auto split=fiber_geometry_coverage_bounds(draft);
    CHECK_FALSE(diff_ex(first.first,split.first).empty()); // The semantic join must not be merged.
    draft.width_mm=2;const auto wider=fiber_geometry_coverage_bounds(draft);
    CHECK_FALSE(diff_ex(wider.second,split.second).empty());
    draft.reference_deposition=std::make_shared<const Polyline>(Polyline(Points{
        Point::new_scale(0,0),Point::new_scale(5,1),Point::new_scale(10,0)}));
    const auto moved=fiber_geometry_coverage_bounds(draft);
    CHECK_FALSE(diff_ex(moved.second,wider.second).empty());
    stats=fiber_geometry_coverage_cache_statistics(context->coverage_cache);CHECK(stats.misses==4);
    for(unsigned i=0;i<40;++i){draft.width_mm=1+i*.01;fiber_geometry_coverage_bounds(draft);}
    stats=fiber_geometry_coverage_cache_statistics(context->coverage_cache);
    CHECK(stats.entries==32);CHECK(stats.retained_bytes<=32*1024*1024);
    draft.width_mm=std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(fiber_geometry_coverage_bounds(draft),FiberExecutionError);
}

TEST_CASE("parallel reference cache misses return independent equivalent bounds", "[ContinuousFiber][R4][cache]")
{
    FiberPathDraft draft;draft.width_mm=1;draft.execution_deviation_mm=.000211314;
    draft.reference_deposition=std::make_shared<const Polyline>(Polyline(Points{
        Point::new_scale(0,0),Point::new_scale(10,0),Point::new_scale(10,10)}));
    const auto expected=fiber_geometry_coverage_bounds(draft);
    auto context=std::make_shared<FiberGeometryContext>();context->coverage_cache=make_fiber_geometry_coverage_cache();
    draft.geometry_context=context;
    std::vector<std::future<std::pair<ExPolygons,ExPolygons>>> jobs;
    for(int i=0;i<4;++i)jobs.push_back(std::async(std::launch::async,[&]{return fiber_geometry_coverage_bounds(draft);}));
    for(auto& job:jobs) {
        auto result=job.get();CHECK(diff_ex(expected.first,result.first).empty());CHECK(diff_ex(result.first,expected.first).empty());
        CHECK(diff_ex(expected.second,result.second).empty());CHECK(diff_ex(result.second,expected.second).empty());
        result.second.clear(); // A consumer cannot mutate the cached result.
    }
    const auto after=fiber_geometry_coverage_bounds(draft);CHECK(diff_ex(expected.second,after.second).empty());
    const auto stats=fiber_geometry_coverage_cache_statistics(context->coverage_cache);
    CHECK(stats.entries==1);CHECK(stats.hits+stats.misses==5);CHECK(stats.hits>=1);
}

TEST_CASE("late machine rejection revokes the whole task without changing a manual mapping", "[ContinuousFiber][R4]")
{
    auto config=masked_print_config();config.set_deserialize_strict("fiber_layer_height_ratio","1");
    config.set_deserialize_strict("top_shell_thickness","0");config.set_deserialize_strict("bottom_shell_thickness","0");
    config.set_deserialize_strict("fiber_z_hop_height","0");
    Model model;auto* object=model.add_object();object->add_volume(make_cube(20,20,2));
    object->add_instance()->set_offset(Vec3d(60,60,0));object->add_instance()->set_offset(Vec3d(160,60,0));
    Print print;print.set_plate_origin(Vec3d::Zero());print.apply(model,config);print.process();
    const auto previous=print.fiber_execution_plans();REQUIRE(previous);REQUIRE_FALSE(previous->paths.empty());
    SECTION("one placement outside the fiber workspace rejects all placements") {
        auto* areas=new ConfigOptionPointsGroups;
        areas->values={Vec2ds{{0,0},{250,0},{250,250},{0,250}},Vec2ds{{0,0},{120,0},{120,120},{0,120}}};
        config.set_key_value("extruder_printable_area",areas);
    }
    SECTION("fiber tool height is checked after final layer Z") {
        config.set_deserialize_strict("extruder_printable_height","250,0.3");
    }
    SECTION("a late CFSYS pressure advance violation is rejected before export") {
        config.set_deserialize_strict("gcode_flavor","klipper");
        config.set_deserialize_strict("toolhead_fiber_protocol_id",";cfsys-v1");
        config.set_deserialize_strict("fiber_cut_gcode","M400\nS0\nM400\n");
        config.set_deserialize_strict("enable_pressure_advance","1,1");config.set_deserialize_strict("pressure_advance","0,0.1");
    }
    print.apply(model,config);CHECK_FALSE(print.fiber_execution_plans_current(previous));
    CHECK_THROWS_AS(print.process(),FiberExecutionError);CHECK_FALSE(print.fiber_execution_plans());
    CHECK(print.config().filament_map.values==std::vector<int>{1,2});
}

TEST_CASE("derived envelopes retain the old explicit material partition", "[ContinuousFiber][R5][ledger][CoverageStatistics]")
{
    auto config=ContinuousFiberConfig{};config.z_hop_height_mm=0;
    const auto shape=GENERATE(0,1,2);
    const ExPolygons domain{rectangle(-20,-20,80,80)};
    auto route=shape==0?path_from_points({{0,0},{20,0},{20,20},{0,20}},2):
        shape==1?path_from_points({{0,0},{20,0},{20,2},{0,2}},2):path_from_points({{40,0},{0,40}},2);
    if(shape==2)route.polyline.fitting_result={{0,1,EMovePathType::Arc_move_ccw,
        ArcSegment(Point(0,0),scale_(40),route.polyline.points[0].to_point(),route.polyline.points[1].to_point(),ArcDirection::Arc_Dir_CCW)}};
    const auto first=finalize_independent(route,domain,config,test_id(),
        shape==2?continuous_fiber_detail::arc_metadata(route.polyline):std::vector<ContourArc>{});
    auto other_id=test_id();other_id.fragment_ordinal=1;
    const auto second=finalize_independent(straight_path(10,-5,10,25,1),domain,config,other_id);
    REQUIRE(first.prepared);REQUIRE(second.prepared);
    FiberCoverageRecord record;record.source=test_id().parent.domain;record.source_domain=domain;
    record.accepted_contour_exclusion=first.prepared->resin_exclusion;
    record.accepted_infill_exclusion=second.prepared->resin_exclusion;
    record.execution_envelope=FiberCoverageEnvelope::from_paths({first.prepared,second.prepared});
    const auto fiber=union_ex(record.accepted_contour_exclusion,record.accepted_infill_exclusion);
    record.resin_domain=diff_ex(domain,fiber);
    // Independent regression oracle: the full R4 materialized partition, with
    // its extra clips/unions, must agree with the new derived measurements.
    auto components=first.prepared->guaranteed_components;
    append(components,second.prepared->guaranteed_components);
    const auto legacy_area_mm2=[](const ExPolygons& p){return std::abs(area(p))*SCALING_FACTOR*SCALING_FACTOR;};
    const auto guaranteed=intersection_ex(components,fiber);
    const auto uncertain=diff_ex(fiber,guaranteed);
    const auto old_guaranteed=intersection_ex(guaranteed,fiber);
    const auto old_uncertain=intersection_ex(uncertain,fiber);
    const auto old_classified=union_ex(old_guaranteed,old_uncertain);
    const double old_invalid=legacy_area_mm2(diff_ex(fiber,old_classified))+
        legacy_area_mm2(intersection_ex(old_guaranteed,old_uncertain));
    FiberCoverageLedger ledger;ledger.add(record);const auto audit=ledger.audit(FiberCoverageAuditMode::Detailed);
    REQUIRE(audit.partition_statistics);
    CHECK(audit.partition_statistics->guaranteed_fiber_area_mm2==Catch::Approx(legacy_area_mm2(old_guaranteed)).margin(1e-7));
    CHECK(audit.partition_statistics->uncertain_fiber_area_mm2==Catch::Approx(legacy_area_mm2(old_uncertain)).margin(1e-7));
    CHECK(audit.invalid_execution_envelope_area_mm2==Catch::Approx(old_invalid).margin(1e-7));
    CHECK(audit.partition_statistics->guaranteed_fiber_area_mm2+audit.partition_statistics->uncertain_fiber_area_mm2==Catch::Approx(audit.fiber_exclusion_area_mm2).margin(1e-10));
    CHECK(audit.invalid_execution_envelope_area_mm2<.000001);
    CHECK(audit.unassigned_area_mm2<.01);CHECK(audit.unexpected_overlap_area_mm2<.01);
    const auto normal=ledger.audit();
    CHECK_FALSE(normal.partition_statistics);
    CHECK(normal.original_area_mm2==audit.original_area_mm2);
    CHECK(normal.fiber_exclusion_area_mm2==audit.fiber_exclusion_area_mm2);
    CHECK(normal.resin_area_mm2==audit.resin_area_mm2);
    CHECK(normal.intentional_void_area_mm2==audit.intentional_void_area_mm2);
    CHECK(normal.unassigned_area_mm2==audit.unassigned_area_mm2);
    CHECK(normal.unexpected_overlap_area_mm2==audit.unexpected_overlap_area_mm2);
    CHECK(normal.outside_area_mm2==audit.outside_area_mm2);
    CHECK(normal.invalid_execution_envelope_area_mm2==audit.invalid_execution_envelope_area_mm2);
    if(shape==1) {
        const auto seam=Point::new_scale(5,1);
        CHECK(std::none_of(guaranteed.begin(),guaranteed.end(),[&](const ExPolygon& p){return p.contains(seam);}));
        CHECK(std::any_of(uncertain.begin(),uncertain.end(),[&](const ExPolygon& p){return p.contains(seam);}));
    }
}

TEST_CASE("derived envelopes reject missing or mismatched accepted sources", "[ContinuousFiber][R5][ledger][CoverageStatistics]")
{
    const auto mode=GENERATE(FiberCoverageAuditMode::MaterialIntegrity,FiberCoverageAuditMode::Detailed);
    auto config=ContinuousFiberConfig{};config.z_hop_height_mm=0;
    const ExPolygons domain{rectangle(-5,-5,30,30)};
    const auto first=finalize_independent(straight_path(0,0,20,0,1),domain,config,test_id());
    auto other_id=test_id();other_id.fragment_ordinal=1;
    const auto second=finalize_independent(straight_path(0,10,20,10,1),domain,config,other_id);
    REQUIRE(first.prepared);REQUIRE(second.prepared);
    FiberCoverageRecord record;record.source=test_id().parent.domain;record.source_domain=domain;
    record.accepted_contour_exclusion=first.prepared->resin_exclusion;
    record.accepted_infill_exclusion=second.prepared->resin_exclusion;
    const auto fiber=union_ex(record.accepted_contour_exclusion,record.accepted_infill_exclusion);
    record.resin_domain=diff_ex(domain,fiber);
    SECTION("missing optional cannot turn the whole allocation into uncertainty") {
        CHECK_THROWS(([&]{FiberCoverageLedger ledger;ledger.add(record);return ledger.audit(mode);})());
    }
    SECTION("omitting a real accepted source is an invalid envelope") {
        record.execution_envelope=FiberCoverageEnvelope::from_paths({first.prepared});
        FiberCoverageLedger ledger;ledger.add(record);CHECK(ledger.audit(mode).invalid_execution_envelope_area_mm2>1);
    }
    SECTION("an empty source list cannot conceal real allocated fiber") {
        record.execution_envelope=FiberCoverageEnvelope::from_paths({});
        CHECK_THROWS(([&]{FiberCoverageLedger ledger;ledger.add(record);return ledger.audit(mode);})());
    }
    SECTION("changing F after sealing sources is independently detected") {
        record.execution_envelope=FiberCoverageEnvelope::from_paths({first.prepared,second.prepared});
        record.accepted_infill_exclusion.clear();
        FiberCoverageLedger ledger;ledger.add(record);CHECK(ledger.audit(mode).invalid_execution_envelope_area_mm2>1);
    }
    SECTION("a tiny change to F is still measured independently") {
        record.execution_envelope=FiberCoverageEnvelope::from_paths({first.prepared,second.prepared});
        record.accepted_infill_exclusion.push_back(rectangle(25,25,25.001,25.001));
        FiberCoverageLedger ledger;ledger.add(record);const auto audit=ledger.audit(mode);
        CHECK(audit.invalid_execution_envelope_area_mm2>0);
        CHECK(audit.invalid_execution_envelope_area_mm2<.01);
    }
    SECTION("a sealed source cannot be attributed to another domain") {
        record.execution_envelope=FiberCoverageEnvelope::from_paths({first.prepared,second.prepared});
        ++record.source.policy_group_id;
        CHECK_THROWS(([&]{FiberCoverageLedger ledger;ledger.add(record);return ledger.audit(mode);})());
    }
    SECTION("conflicting sealed identities are invalid") {
        const auto conflict=finalize_independent(straight_path(0,20,20,20,1),domain,config,test_id());
        REQUIRE(conflict.prepared);CHECK_THROWS(FiberCoverageEnvelope::from_paths({first.prepared,conflict.prepared}));
    }
    SECTION("null sources are invalid rather than an empty guarantee") {
        CHECK_THROWS(FiberCoverageEnvelope::from_paths({nullptr}));
    }
}

TEST_CASE("derived envelope optimization retains real material audit failures", "[ContinuousFiber][R5][ledger][CoverageStatistics]")
{
    const auto mode=GENERATE(FiberCoverageAuditMode::MaterialIntegrity,FiberCoverageAuditMode::Detailed);
    auto config=ContinuousFiberConfig{};config.z_hop_height_mm=0;
    const ExPolygons domain{rectangle(-5,-5,30,30)};
    const auto source=finalize_independent(straight_path(0,0,20,0,1),domain,config,test_id());REQUIRE(source.prepared);
    FiberCoverageRecord record;record.source=test_id().parent.domain;record.source_domain=domain;record.accepted_infill_exclusion=source.prepared->resin_exclusion;
    record.execution_envelope=FiberCoverageEnvelope::from_paths({source.prepared});
    SECTION("a real resin overlap remains visible") {
        record.resin_domain=domain;FiberCoverageLedger ledger;ledger.add(record);
        CHECK(ledger.audit(mode).unexpected_overlap_area_mm2>1);
    }
    SECTION("a real unassigned model region remains visible") {
        FiberCoverageLedger ledger;ledger.add(record);CHECK(ledger.audit(mode).unassigned_area_mm2>1);
    }
    SECTION("an intentional void remains valid") {
        record.intentional_void=diff_ex(domain,record.accepted_infill_exclusion);
        FiberCoverageLedger ledger;ledger.add(record);const auto audit=ledger.audit(mode);
        CHECK(audit.unassigned_area_mm2<.01);CHECK(audit.unexpected_overlap_area_mm2<.01);
    }
}

TEST_CASE("derived envelope subtracts grid areas before converting large coordinates", "[ContinuousFiber][R5][ledger][CoverageStatistics]")
{
    auto config=ContinuousFiberConfig{};config.z_hop_height_mm=0;
    const ExPolygons domain{rectangle(99995,99995,100025,100005)};
    const auto source=finalize_independent(straight_path(100000,100000,100020,100000,2),domain,config,test_id());
    REQUIRE(source.prepared);REQUIRE(source.prepared->guaranteed_components.size()==1);
    FiberCoverageRecord record;record.source=test_id().parent.domain;record.source_domain=domain;record.accepted_infill_exclusion=source.prepared->resin_exclusion;
    record.execution_envelope=FiberCoverageEnvelope::from_paths({source.prepared});
    FiberCoverageLedger ledger;ledger.add(record);const auto audit=ledger.audit(FiberCoverageAuditMode::Detailed);
    REQUIRE(audit.partition_statistics);
    // Both bounds are axis-aligned: margin=ceil(.1*delta/q)+64=86 grids.
    const double margin=(std::ceil(source.prepared->execution_deviation_mm*.1/SCALING_FACTOR)+64)*SCALING_FACTOR;
    CHECK(audit.partition_statistics->guaranteed_fiber_area_mm2==Catch::Approx((20-2*margin)*(2-2*margin)).margin(1e-11));
    CHECK(audit.partition_statistics->uncertain_fiber_area_mm2==Catch::Approx(4*margin*(20+2)).margin(1e-11));
}

namespace {
FiberFinalizationResult native_fallback_fixture(bool exact_circle=false,double tail_mm=0,bool native_geometry=true,double rotation=0)
{
    auto config=ContinuousFiberConfig{};config.cut_to_contact_length_mm=tail_mm;config.z_hop_height_mm=0;
    config.start_stabilization_length_mm=0;config.speed_sampling_length_mm=10000;
    config.infill_min_speed_mm_s=config.infill_max_speed_mm_s=10;
    auto geometry=std::make_shared<FiberGeometryContext>();geometry->export_arcs=native_geometry;config.geometry_context=geometry;
    const double end_x=exact_circle?0:std::sqrt(50.),end_y=exact_circle?10:std::sqrt(50.);
    const double c=std::cos(rotation),s=std::sin(rotation);
    auto source=path_from_points({{10*c,10*s},{end_x*c-end_y*s,end_x*s+end_y*c}},1);
    source.polyline.fitting_result={{0,1,EMovePathType::Arc_move_ccw,ArcSegment(Point(0,0),scale_(10),
        source.polyline.points[0].to_point(),source.polyline.points[1].to_point(),ArcDirection::Arc_Dir_CCW)}};
    return finalize_independent(source,{rectangle(-15,-15,15,15)},config,test_id(),
        continuous_fiber_detail::arc_metadata(source.polyline));
}
}

TEST_CASE("rounded native arc uses certified linear commands without reallocating material", "[ContinuousFiber][NativeFallback]")
{
    const auto result=native_fallback_fixture();INFO(result.detail);REQUIRE(result.prepared);
    const auto& prepared=*result.prepared;
    const auto possible=prepared.possible_coverage,guaranteed=prepared.guaranteed_components;
    const auto reference=prepared.reference_deposition;const auto source_points=prepared.spans.front().geometry.points;
    auto context=fiber_fixture_context();context->export_arcs=true;
    const auto raw=compile_fiber_execution(prepared.draft(),*context,context->placements.front(),0);
    REQUIRE_FALSE(raw.arcs().empty());
    try {certify_fiber_coverage(prepared,raw,*context);FAIL("Rounded 45 degree endpoint was certified as a native circle");}
    catch(const FiberExecutionError& error){REQUIRE(error.failure==FiberExecutionFailure::Quantization);}
    FiberExecutionStatistics statistics;
    const auto execution=compile_certified_fiber_execution(prepared,*context,context->placements.front(),0,statistics);
    CHECK(statistics.compiled_candidates>=2);CHECK(statistics.compiled_candidates<=9);
    CHECK(statistics.translated_coverage+statistics.swept_coverage==1);
    REQUIRE(execution.arcs().empty());CHECK(context->export_arcs);
    CHECK(prepared.geometry_context->export_arcs);CHECK(prepared.possible_coverage==possible);
    CHECK(prepared.guaranteed_components==guaranteed);CHECK(prepared.reference_deposition==reference);
    CHECK(prepared.spans.front().geometry.points==source_points);
    CHECK(execution.source_deviation_mm<=prepared.execution_deviation_mm);
    CHECK_NOTHROW(certify_fiber_coverage(prepared,execution,*context));
    PrintConfig config;config.use_relative_e_distances.value=false;
    GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0});writer.toolchange(0);
    const GCodeWriter::ScopedXYZPrecision precision(writer,6);
    writer.set_position(execution.machine_target(execution.spans().front().from));
    std::string text;double ideal_e=0,emitted_e=0;size_t commands=0;
    for(size_t si=0;si<execution.spans().size();++si) {
        const auto& span=execution.spans()[si];Vec3d from=execution.machine_target(span.from);
        for(const auto& command:span.commands) {
            REQUIRE(command.source_primitive<prepared.spans[si].edges.size());
            CHECK(command.arc_index==-1);
            CHECK(command.speed_mm_s==prepared.spans[si].edges[command.source_primitive].speed_mm_s);
            const Vec3d target=execution.machine_target(command.target);
            // The finalizer source remains analytic, and every emitted endpoint
            // must stay within its frozen geometric deviation budget.
            CHECK(std::abs(target.head<2>().norm()-10)<=prepared.execution_deviation_mm);
            ideal_e+=(target-from).norm()*prepared.spans[si].edges[command.source_primitive].feed_mm_per_xy_mm;
            emitted_e+=command.dE;CHECK(emitted_e==Catch::Approx(GCodeFormatter::quantize_e(ideal_e)).margin(1e-10));
            const auto row=writer.emit_frozen_fiber_motion(target,command.dE,command.speed_mm_s);
            const auto coordinate=[&](char axis) {
                const std::regex expression(std::string("(?:^| )")+axis+"([-0-9.]+)");std::smatch match;
                REQUIRE(std::regex_search(row,match,expression));return std::stod(match[1].str());
            };
            CHECK(coordinate('X')==Catch::Approx(target.x()).margin(1e-12));
            CHECK(coordinate('Y')==Catch::Approx(target.y()).margin(1e-12));
            CHECK(coordinate('E')==Catch::Approx(GCodeFormatter::quantize_e(emitted_e)).margin(1e-10));
            text+=row;from=target;++commands;
        }
    }
    CHECK(commands>1);CHECK(text.find("G2 ")==std::string::npos);CHECK(text.find("G3 ")==std::string::npos);
    CHECK(writer.quantized_machine_position(writer.get_position())==execution.machine_target(execution.spans().back().commands.back().target));
}

TEST_CASE("certified compiler preserves exact native arcs and ordinary linear schedules", "[ContinuousFiber][NativeFallback]")
{
    const bool native=GENERATE(false,true);
    const auto result=native_fallback_fixture(true,0,native);INFO(result.detail);REQUIRE(result.prepared);
    auto context=fiber_fixture_context();context->export_arcs=native;
    const auto raw=compile_fiber_execution(result.prepared->draft(),*context,context->placements.front(),0);
    REQUIRE_NOTHROW(certify_fiber_coverage(*result.prepared,raw,*context));
    FiberExecutionStatistics statistics;
    const auto execution=compile_certified_fiber_execution(*result.prepared,*context,context->placements.front(),0,statistics);
    CHECK(statistics.compiled_candidates==1);CHECK(statistics.translated_coverage+statistics.swept_coverage==1);
    CHECK(equal_fiber_commands(*raw.commands,*execution.commands));
    CHECK(raw.command_origin==execution.command_origin);CHECK(raw.layer_z==execution.layer_z);
    CHECK(raw.prefeed_dE==execution.prefeed_dE);CHECK(raw.prefeed_F==execution.prefeed_F);
    CHECK(raw.entry_targets==execution.entry_targets);CHECK(raw.entry_stations_mm==execution.entry_stations_mm);
    CHECK(raw.source_deviation_mm==execution.source_deviation_mm);CHECK(raw.nozzle_offset==execution.nozzle_offset);
    CHECK(execution.arcs().empty()!=native);
}

TEST_CASE("certified native fallback propagates cancellation and machine failures", "[ContinuousFiber][NativeFallback]")
{
    const auto result=native_fallback_fixture();INFO(result.detail);REQUIRE(result.prepared);
    auto context=fiber_fixture_context();context->export_arcs=true;FiberExecutionStatistics statistics;
    SECTION("cancellation inside the fallback never publishes a partial execution") {
        CHECK_THROWS_AS(compile_certified_fiber_execution(*result.prepared,*context,context->placements.front(),0,statistics,[&] {
            if(statistics.compiled_candidates==2)throw CanceledException();
        }),CanceledException);
        CHECK(statistics.compiled_candidates==2);CHECK(statistics.translated_coverage+statistics.swept_coverage==0);
    }
    SECTION("machine workspace rejection is not a representation failure") {
        context->check_machine=true;
        auto limits=std::make_shared<FiberMachineMotionLimits>();limits->tip_xy={rectangle(-2,-2,2,2)};
        limits->maximum_z_mm=100;limits->prepare();
        context->machine_limits.push_back(limits);
        try {
            compile_certified_fiber_execution(*result.prepared,*context,context->placements.front(),0,statistics);
            FAIL("An unreachable native path was accepted");
        } catch(const FiberExecutionError& error){CHECK(error.failure==FiberExecutionFailure::Motion);}
        CHECK(statistics.compiled_candidates==1);CHECK(statistics.translated_coverage+statistics.swept_coverage==0);
    }
}

TEST_CASE("native linear fallback retains rotated placed cut tails and cumulative feed", "[ContinuousFiber][NativeFallback]")
{
    const double rotation=GENERATE(0.,PI/2);
    const auto result=native_fallback_fixture(false,2,true,rotation);INFO(result.detail);REQUIRE(result.prepared);
    const auto& prepared=*result.prepared;const auto possible=prepared.possible_coverage,guaranteed=prepared.guaranteed_components;
    auto context=fiber_fixture_context();context->export_arcs=true;
    context->placements={{42,7,Vec2d(100.1234567,80.7654321)}};
    context->config.extruder_offset.values[0]=Vec2d(7.1234567,-2.7654321);
    context->transform.xy_offset=Vec2d(20.1234567,10.7654321);
    FiberExecutionStatistics statistics;
    const auto execution=compile_certified_fiber_execution(prepared,*context,context->placements.front(),.54,statistics);
    CHECK(execution.arcs().empty());CHECK(statistics.compiled_candidates>=2);CHECK(statistics.compiled_candidates<=9);
    CHECK_NOTHROW(certify_fiber_coverage(prepared,execution,*context));
    CHECK(prepared.possible_coverage==possible);CHECK(prepared.guaranteed_components==guaranteed);CHECK(context->export_arcs);
    double ideal=prepared.start_procedure.prefeed_length_mm,emitted=execution.prefeed_dE,passive_length=0;
    for(size_t si=0;si<execution.spans().size();++si) {
        const auto& span=execution.spans()[si];Vec3d from=execution.machine_target(span.from);
        for(const auto& command:span.commands) {
            const auto to=execution.machine_target(command.target);const double length=(to-from).norm();
            REQUIRE(command.source_primitive<prepared.spans[si].edges.size());
            if(prepared.spans[si].actively_feeds_fiber())
                ideal+=length*prepared.spans[si].edges[command.source_primitive].feed_mm_per_xy_mm;
            else CHECK(command.dE==0);
            if(span.kind==FiberMotionKind::PassiveDepositingAfterCut) {CHECK(command.dE==0);passive_length+=length;}
            emitted+=command.dE;CHECK(emitted==Catch::Approx(GCodeFormatter::quantize_e(ideal)).margin(1e-9));from=to;
        }
    }
    CHECK(std::abs(passive_length-prepared.requested_passive_length_mm)<=prepared.passive_length_error_mm);
    CHECK(execution.source_deviation_mm<=prepared.execution_deviation_mm);
    // The shared low-level compiler must reject a tail target incompatible
    // with this fixed geometry, rather than accepting its approximate length.
    auto incompatible=prepared.draft();incompatible.requested_passive_length_mm+=.01;
    auto linear_context=*context;linear_context.export_arcs=false;
    try {
        compile_fiber_execution(incompatible,linear_context,linear_context.placements.front(),.54);
        FAIL("A mismatched passive length budget was bypassed");
    } catch(const FiberExecutionError& error){CHECK(error.failure==FiberExecutionFailure::TailBudget);}
}

TEST_CASE("shared contour envelopes retain the original independent clipping policies", "[ContinuousFiber][FillPerformance]")
{
    const double clearance=GENERATE(0.,.3);
    const bool short_edge=GENERATE(false,true);
    auto candidate=short_edge?path_from_points({{0,0},{.00005,0},{20,0},{20,20},{0,20},{0,0}},1):
        path_from_points({{0,0},{20,0},{20,20},{0,20},{0,0}},1);
    auto id=test_id();id.parent.purpose=FiberPathPurpose::Contour;
    ContinuousFiberConfig config;config.z_hop_height_mm=0;config.cut_to_contact_length_mm=2;
    config.finish_overlap_length_mm=0;config.contour_infill_clearance_mm=clearance;
    // A hole and a remote island exercise both final domain clips while the
    // strand touches the outer model edge along its first horizontal run.
    const ExPolygons domain{rectangle_with_hole(-1,-.5,21,21,5,5,15,15),rectangle(30,30,35,35)};
    const auto result=finalize_independent(candidate,domain,config,id);INFO(result.detail);REQUIRE(result.prepared);
    const auto& prepared=*result.prepared;
    ExPolygons original_occupancy,original_keepout;
    for(const auto& run:fiber_geometry_contour_runs(prepared.draft())) {
        append(original_occupancy,fiber_contour_coverage(run,.5*prepared.width_mm));
        append(original_keepout,fiber_contour_coverage(run,.5*prepared.width_mm+clearance));
    }
    const auto original_resin=intersection_ex(fiber_material_offset(union_ex(original_occupancy),
        prepared.execution_deviation_mm),prepared.resin_exclusion);
    const auto original_infill=intersection_ex(fiber_material_offset(union_ex(original_keepout),
        prepared.execution_deviation_mm),domain,ApplySafetyOffset::Yes);
    CHECK(prepared.contour_resin_exclusion==original_resin);
    CHECK(prepared.contour_to_infill_keepout==original_infill);
}

TEST_CASE("rounded intermediate masks cannot replace the actual fiber allocation", "[ContinuousFiber][FillPerformance]")
{
    // This exact-grid counterexample prevents substituting G intersect A for
    // G intersect F: clipping P first rounds the slanted boundary of F.
    const auto rectangle_grid=[](coord_t x0,coord_t y0,coord_t x1,coord_t y1) {
        return ExPolygon(Polygon(Points{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}));
    };
    const ExPolygons possible{rectangle_grid(0,0,1000,1000)};
    const ExPolygons guaranteed{rectangle_grid(100,100,900,900)};
    const ExPolygons allowed{ExPolygon(Polygon(Points{{-1000,-333},{2000,667},{2000,3000},{-1000,3000}}))};
    const auto allocated=intersection_ex(possible,allowed);
    const auto original=intersection_ex(guaranteed,allocated);
    const auto substituted=intersection_ex(guaranteed,allowed);
    CHECK_FALSE(xor_ex(original,substituted).empty());
}

TEST_CASE("coverage audits canonicalize public resin inputs before an empty void fast path", "[ContinuousFiber][FillPerformance][CoverageStatistics]")
{
    const bool has_void=GENERATE(false,true);
    const auto mode=GENERATE(FiberCoverageAuditMode::MaterialIntegrity,FiberCoverageAuditMode::Detailed);
    FiberCoverageRecord record;record.source=test_id().parent.domain;
    record.source_domain={rectangle(0,0,20,10)};
    record.resin_domain=has_void?ExPolygons{rectangle(0,0,6,10),rectangle(4,0,10,10)}:
        ExPolygons{rectangle(0,0,12,10),rectangle(8,0,20,10)};
    if(has_void)record.intentional_void={rectangle(10,0,20,10)};
    FiberCoverageLedger ledger;ledger.add(record);const auto audit=ledger.audit(mode);
    CHECK(bool(audit.partition_statistics)==(mode==FiberCoverageAuditMode::Detailed));
    CHECK(audit.unassigned_area_mm2==0);
    CHECK(audit.unexpected_overlap_area_mm2==0);
    CHECK(audit.fiber_exclusion_area_mm2==0);
    CHECK(audit.intentional_void_area_mm2==Catch::Approx(has_void?100.:0.));
}

TEST_CASE("empty coverage statistics distinguish unmeasured from measured zero", "[ContinuousFiber][ledger][CoverageStatistics]")
{
    FiberCoverageLedger ledger;
    CHECK_FALSE(ledger.audit().partition_statistics);
    const auto detailed=ledger.audit(FiberCoverageAuditMode::Detailed);
    REQUIRE(detailed.partition_statistics);
    CHECK(detailed.partition_statistics->guaranteed_fiber_area_mm2==0);
    CHECK(detailed.partition_statistics->uncertain_fiber_area_mm2==0);
}
