#include <catch2/catch_all.hpp>

#include "libslic3r/ContinuousFiber/FiberIsland.hpp"
#include "libslic3r/ContinuousFiber/FiberPathFinalizer.hpp"
#include "libslic3r/ContinuousFiber/FiberPathValidator.hpp"
#include "libslic3r/ContinuousFiber/FiberPolicyKey.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/FiberGCodeBlockParser.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCode/FanMover.hpp"

#include <set>
#include <limits>
#include <random>
#include <nlopt.hpp>

using namespace Slic3r;

namespace {

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

FiberFragmentId test_id()
{
    return {{{7, 62, 4, 1}, FiberPathPurpose::Infill, 0, 9}, 0};
}

} // namespace

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

    const FiberFinalizationResult result = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
    REQUIRE(result.prepared != nullptr);
    REQUIRE(result.prepared->spans.size() == 2);
    CHECK(result.prepared->spans[0].kind == FiberMotionKind::PoweredDepositing);
    CHECK(result.prepared->spans[1].kind == FiberMotionKind::PassiveDepositingAfterCut);
    CHECK(result.prepared->passive_tail_length_mm() == Catch::Approx(23.0).margin(0.001));
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(50.0).margin(0.001));
    CHECK_FALSE(result.prepared->physical_coverage.empty());
    CHECK_FALSE(result.prepared->resin_exclusion.empty());
    CHECK(result.prepared->outside_domain.empty());

    const auto& actions = result.prepared->actions;
    REQUIRE(actions.size() == 10);
    CHECK(actions[0].type == FiberActionType::Begin);
    CHECK(actions[1].type == FiberActionType::Approach);
    CHECK(actions[2].type == FiberActionType::Prefeed);
    CHECK(actions[3].type == FiberActionType::Start);
    CHECK(actions[3].span_index == 0);
    CHECK(actions[4].type == FiberActionType::MotionSpan);
    CHECK(actions[5].type == FiberActionType::Cut);
    CHECK(actions[6].type == FiberActionType::MotionSpan);
    CHECK(actions[7].type == FiberActionType::FiberDepleted);
    CHECK(actions[8].type == FiberActionType::Finish);
    CHECK(actions[9].type == FiberActionType::End);
    auto invalid = *result.prepared;
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
    config.landing_length_mm = 2.0;
    config.landing_speed_mm_s = 3.0;
    config.adhesion_dwell_ms = 25;
    config.start_stabilization_length_mm = 3.0;
    config.start_speed_mm_s = 10.0;
    config.resin_overlap_mm = 0.05;
    config.outside_tolerance_mm2 = 0.01;

    const FiberFinalizationResult result = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
    REQUIRE(result.prepared != nullptr);
    const PreparedFiberPath& prepared = *result.prepared;
    REQUIRE(prepared.spans.size() == 4);
    CHECK(prepared.spans[0].kind == FiberMotionKind::PrefedLanding);
    CHECK(prepared.spans[1].kind == FiberMotionKind::PoweredStart);
    CHECK(prepared.spans[2].kind == FiberMotionKind::PoweredDepositing);
    CHECK(prepared.spans[3].kind == FiberMotionKind::PassiveDepositingAfterCut);
    CHECK(unscale<double>(prepared.spans[0].geometry.length()) == Catch::Approx(2.0).margin(0.001));
    CHECK(unscale<double>(prepared.spans[1].geometry.length()) == Catch::Approx(3.0).margin(0.001));
    CHECK(prepared.passive_tail_length_mm() == Catch::Approx(10.0).margin(0.001));
    CHECK(prepared.total_depositing_length_mm() == Catch::Approx(60.0).margin(0.001));
    CHECK(prepared.start_procedure.prefeed_length_mm == Catch::Approx(10.5));

    const auto start_event = std::find_if(
        prepared.actions.begin(), prepared.actions.end(),
        [](const FiberProcessAction& action) { return action.type == FiberActionType::Start; });
    REQUIRE(start_event != prepared.actions.end());
    CHECK(start_event->span_index == 1);
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

    const auto a = FiberPathFinalizer::finalize(candidate, domain, without_finish, test_id());
    const auto b = FiberPathFinalizer::finalize(candidate, domain, with_finish, test_id());
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
    config.landing_length_mm = 2;
    config.cut_to_contact_length_mm = 10;
    // The physical strand fits exactly at the lower boundary; a wider
    // infill keepout must be clipped at both sides of this narrow domain.
    const ExPolygons domain{rectangle(0, 9.5, 70, 10.7)};
    const auto result = FiberPathFinalizer::finalize(candidate, domain, config, id);
    REQUIRE(result.prepared);
    const auto area_mm2 = [](const ExPolygons& polygons) {
        return unscaled<double>(unscaled<double>(area(polygons)));
    };
    CHECK(area_mm2(result.prepared->physical_coverage) == Catch::Approx(50.0).margin(.002));
    CHECK(area_mm2(result.prepared->resin_exclusion) ==
        Catch::Approx(50.0).margin(.002));
    CHECK(area_mm2(result.prepared->contour_to_infill_keepout) ==
        Catch::Approx(config.contour_infill_clearance_mm == 0 ? 50.0 : 60.0).margin(.002));
    CHECK(result.prepared->outside_domain.empty());
}

TEST_CASE("fiber path shorter than cut process budget is rejected", "[ContinuousFiber]")
{
    const ExtrusionPath candidate = straight_path(10, 10, 30, 10);
    const ExPolygons domain {rectangle(0, 0, 40, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 23.0;

    const FiberFinalizationResult result = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
    CHECK(result.prepared == nullptr);
    CHECK(result.failure == FiberFinalizationFailure::TooShort);
}

TEST_CASE("fiber path requires a real powered deposition span beyond its passive tail", "[ContinuousFiber]")
{
    const ExPolygons domain {rectangle(0, 0, 70, 20)};
    ContinuousFiberConfig config;
    config.cut_to_contact_length_mm = 23.0;
    config.minimum_effective_length_mm = 25.0;

    const FiberFinalizationResult rejected = FiberPathFinalizer::finalize(
        straight_path(10, 10, 57, 10), domain, config, test_id());
    CHECK(rejected.prepared == nullptr);
    CHECK(rejected.failure == FiberFinalizationFailure::TooShort);

    const FiberFinalizationResult accepted = FiberPathFinalizer::finalize(
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

    const FiberFinalizationResult result = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
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

    const FiberFinalizationResult result = FiberPathFinalizer::finalize(
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
    const FiberFinalizationResult result = FiberPathFinalizer::finalize(
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
    const auto a = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
    REQUIRE(a.prepared);
    candidate.mm3_per_mm *= 3;
    candidate.height *= 2;
    candidate.width *= 2;
    const auto b = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
    REQUIRE(b.prepared);
    const auto bound_a = bind_fiber_execution(a.prepared, 0, 1, 3, 2.0, ";cut");
    const auto bound_b = bind_fiber_execution(b.prepared, 0, 1, 3, 2.0, ";cut");
    double sum_a = 0, sum_b = 0;
    for (const auto& span : bound_a.edge_dE) for (double e : span) sum_a += e;
    for (const auto& span : bound_b.edge_dE) for (double e : span) sum_b += e;
    CHECK(sum_a == Catch::Approx(20*1.02*1.05*2));
    CHECK(sum_b == Catch::Approx(sum_a));
    CHECK_THROWS(bind_fiber_execution(a.prepared, 1, 1, 3, 2.0, ";cut"));
    CHECK_THROWS(bind_fiber_execution(a.prepared, 0, 1, 3, 0.0, ";cut"));
}

TEST_CASE("fiber normal speed is invariant under collinear tessellation", "[ContinuousFiber][speed]")
{
    auto a = straight_path(5, 5, 25, 5);
    auto b = a;
    b.polyline.points.clear();
    for (int i = 5; i <= 25; ++i) b.polyline.points.push_back(Point3::new_scale(i, 5, 0));
    ContinuousFiberConfig config;
    const ExPolygons domain{rectangle(0, 0, 40, 20)};
    const auto pa = FiberPathFinalizer::finalize(a, domain, config, test_id());
    b.polyline = normalize_fiber_geometry(b.polyline);
    const auto pb = FiberPathFinalizer::finalize(b, domain, config, test_id());
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
    const auto result = FiberPathFinalizer::finalize(candidate, {rectangle(0,0,30,30)}, config, test_id());
    REQUIRE(result.prepared);
    const auto& edges = result.prepared->spans.front().edges;
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
    const auto result = FiberPathFinalizer::finalize(straight_path(5,5,55,5),
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

TEST_CASE("fiber finish preserves infill policy and supports open contours", "[ContinuousFiber][finish]")
{
    const auto candidate = path_from_points({{5,5},{25,5},{25,25},{5,25},{5,5}});
    ContinuousFiberConfig config;
    config.finish_extension_length_mm = 2;
    config.finish_overlap_length_mm = 3;
    const ExPolygons domain{rectangle(0,0,30,30)};
    const auto infill = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
    REQUIRE(infill.prepared);
    CHECK(infill.prepared->finish_strategy == FiberFinishStrategy::TangentExtension);
    CHECK(infill.prepared->spans.back().geometry.points.back() == Point3::new_scale(5,3,0));
    auto contour_id = test_id();
    contour_id.parent.purpose = FiberPathPurpose::Contour;
    const auto contour = FiberPathFinalizer::finalize(candidate, domain, config, contour_id);
    REQUIRE(contour.prepared);
    CHECK(contour.prepared->finish_strategy == FiberFinishStrategy::LoopOverlap);
    CHECK(contour.prepared->spans.back().geometry.points.back() == Point3::new_scale(8,5,0));
    const auto open = FiberPathFinalizer::finalize(straight_path(5,5,25,5), domain, config, contour_id);
    REQUIRE(open.prepared);
    CHECK(open.prepared->finish_strategy == FiberFinishStrategy::TangentExtension);
    CHECK(open.prepared->spans.back().geometry.points.back() == Point3::new_scale(27,5,0));
}

TEST_CASE("fiber finish is continuous motion not material clipped by a model hole", "[ContinuousFiber][finish]")
{
    ContinuousFiberConfig config;
    config.finish_extension_length_mm = 15;
    const auto result = FiberPathFinalizer::finalize(straight_path(2,5,10,5),
        {rectangle_with_hole(0,0,30,10,15,2,20,8)}, config, test_id());
    REQUIRE(result.prepared);
    const auto& finish = result.prepared->spans.back();
    CHECK(finish.kind == FiberMotionKind::NonDepositingFinish);
    CHECK(finish.geometry.points.front() == Point3::new_scale(10,5,0));
    CHECK(finish.geometry.points.back() == Point3::new_scale(25,5,0));
    CHECK(finish.edges.front().feed_mm_per_xy_mm == 0);
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(8));
}

TEST_CASE("fiber finish leaves the deposition domain without changing feed tail or coverage", "[ContinuousFiber][finish]")
{
    ContinuousFiberConfig config;
    config.landing_length_mm = 2;
    config.minimum_effective_length_mm = 0.5;
    config.cut_to_contact_length_mm = 23;
    const ExPolygons domain {rectangle(0,0,60,10)};
    const auto source = straight_path(5,5,55,5);
    const auto baseline = FiberPathFinalizer::finalize(source, domain, config, test_id());
    REQUIRE(baseline.prepared);
    config.finish_extension_length_mm = 23;
    const auto result = FiberPathFinalizer::finalize(source, domain, config, test_id());
    REQUIRE(result.prepared);
    CHECK(result.prepared->passive_tail_length_mm() == Catch::Approx(23));
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(50));
    CHECK(area(result.prepared->physical_coverage) == Catch::Approx(area(baseline.prepared->physical_coverage)));
    CHECK(area(result.prepared->resin_exclusion) == Catch::Approx(area(baseline.prepared->resin_exclusion)));
    const auto& finish = result.prepared->spans.back();
    CHECK(finish.geometry.points.back() == Point3::new_scale(78,5,0));
    CHECK(unscale<double>(finish.geometry.length()) == Catch::Approx(23));
    CHECK(finish.edges.front().feed_mm_per_xy_mm == 0);
    for (size_t i = 0; i < baseline.prepared->spans.size(); ++i) {
        CHECK(result.prepared->spans[i].geometry.points == baseline.prepared->spans[i].geometry.points);
        for (size_t e = 0; e < baseline.prepared->spans[i].edges.size(); ++e)
            CHECK(result.prepared->spans[i].edges[e].feed_mm_per_xy_mm ==
                  baseline.prepared->spans[i].edges[e].feed_mm_per_xy_mm);
    }
    auto sampled = source;
    sampled.polyline.points.insert(sampled.polyline.points.end()-1, Point3::new_scale(54,5,0));
    sampled.polyline = normalize_fiber_geometry(sampled.polyline);
    const auto same = FiberPathFinalizer::finalize(sampled, domain, config, test_id());
    REQUIRE(same.prepared);
    CHECK(same.prepared->spans.back().geometry.points == finish.geometry.points);
    CHECK_FALSE(FiberPathFinalizer::finalize(straight_path(5,5,65,5), domain, config, test_id()).prepared);
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

TEST_CASE("fiber numerical knots and speed sampling do not discard a closed contour", "[ContinuousFiber][geometry]")
{
    // A near-duplicate from offsetting plus a real corner very close to the
    // 2 mm sampling grid. Neither may invalidate the complete 40 mm loop.
    auto candidate = path_from_points({{5,5},{5.00001,5.00001},
        {15.0001,5},{15.0001,15},{5,15},{5,5.00001},{5,5}});
    candidate.polyline = normalize_fiber_geometry(candidate.polyline);
    ContinuousFiberConfig config;
    config.landing_length_mm = 2;
    config.cut_to_contact_length_mm = 23;
    config.minimum_effective_length_mm = 0.5;
    config.finish_overlap_length_mm = 23;
    config.tail_min_speed_mm_s = config.tail_max_speed_mm_s = 10;
    auto id = test_id(); id.parent.purpose = FiberPathPurpose::Contour;
    const auto result = FiberPathFinalizer::finalize(candidate, {rectangle(0,0,20,20)}, config, id);
    REQUIRE(result.prepared);
    CHECK(result.prepared->total_depositing_length_mm() == Catch::Approx(40.0002).margin(0.003));
    CHECK(result.prepared->spans.front().geometry.points.front() == candidate.polyline.points.front());
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
    const auto result = FiberPathFinalizer::finalize(candidate, domain, config, id);
    REQUIRE(result.prepared);
    CHECK(result.prepared->finish_strategy == FiberFinishStrategy::LoopOverlap);
    auto unsafe = candidate;
    for (auto& p : unsafe.polyline.points) if (p.x() < scale_(1)) p.x() = scale_(0.3);
    CHECK_FALSE(FiberPathFinalizer::finalize(unsafe, domain, config, id).prepared);
}

TEST_CASE("fiber LayerXY and immutable entity contracts reject mutation", "[ContinuousFiber]")
{
    auto candidate = straight_path(5,5,25,5);
    ContinuousFiberConfig config;
    const ExPolygons domain{rectangle(0,0,30,10)};
    candidate.polyline.points.back().z() = scale_(0.2);
    CHECK_FALSE(FiberPathFinalizer::finalize(candidate, domain, config, test_id()).prepared);
    candidate.polyline.points.back().z() = 0;
    const auto result = FiberPathFinalizer::finalize(candidate, domain, config, test_id());
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
    auto broken = std::make_shared<PreparedFiberPath>(*result.prepared);
    broken->spans.front().edges.pop_back();
    CHECK_THROWS(bind_fiber_execution(broken, 0, 0, 0, 1, ";cut"));
    auto quantized = std::make_shared<PreparedFiberPath>(*result.prepared);
    auto& edge_points = quantized->spans.front().geometry.points;
    edge_points[1] = edge_points[0] + Point3(scale_(0.0008), scale_(0.0008), 0.0);
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
    const auto result = FiberPathFinalizer::finalize(candidate, {rectangle(0,0,30,30)}, config, id);
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
    const auto result = FiberPathFinalizer::finalize(candidate, {rectangle(0,0,30,30)}, config, id);
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
    REQUIRE(FiberPathFinalizer::finalize(path, domain, config, id).prepared);
    const auto invalid = FiberPathFinalizer::finalize(path, domain, config, test_id());
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
            CHECK((arc.start_mm-arc.center_mm).norm()==Catch::Approx(arc.radius_mm).margin(1e-6));
            CHECK((arc.end_mm-arc.center_mm).norm()==Catch::Approx(arc.radius_mm).margin(1e-6));
            end=arc.end_distance_mm;
        }
        CHECK(end<=unscale<double>(result.path->length())+1e-6);
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
        config.landing_length_mm=2;
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
        const auto prepared=FiberPathFinalizer::finalize(candidate,domain,config,id,result.arcs);
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
                CHECK((a.start_mm-a.center_mm).norm()==Catch::Approx(a.radius_mm).margin(1e-6));
                CHECK((a.end_mm-a.center_mm).norm()==Catch::Approx(a.radius_mm).margin(1e-6));
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
            config.landing_length_mm=2;
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
    config.cut_to_contact_length_mm=23; config.landing_length_mm=2;
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
    config.cut_to_contact_length_mm=23; config.landing_length_mm=2;
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
    config.cut_to_contact_length_mm=23; config.landing_length_mm=2;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;
    config.finish_extension_length_mm=23;
    config.contour_include_holes=false;
    const auto result=FiberPathValidator::plan_contours(region,config,{1,11,0,0}).validation;
    const auto physical_domain=fiber_material_offset(region,-.7);
    REQUIRE(result.accepted_count()==1);
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
    config.cut_to_contact_length_mm=23; config.landing_length_mm=2;
    config.minimum_effective_length_mm=.5;
    config.finish_overlap_length_mm=23; config.finish_extension_length_mm=23;
    const auto plan=FiberPathValidator::plan_contours(region,config,{1,106,0,0});
    REQUIRE(plan.audit_lineage());
    REQUIRE(plan.nodes.size()>=2);
    const auto& result=plan.validation;
    CHECK(result.audit_assignments().valid());
    REQUIRE(result.assignments.front().prepared);
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
    config.cut_to_contact_length_mm=23;config.landing_length_mm=2;config.minimum_effective_length_mm=.5;
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
    const auto small_bend=path_from_points({{0,0},{1.9993,0},{1.9993,-.0007}}).polyline;
    CHECK(normalize_fiber_geometry(small_bend).points.size()==2);
    CHECK(normalize_fiber_geometry(small_bend,&planned).points==small_bend.points);
    auto candidate=path_from_points({{0,0},{1.9993,0},{1.9993,-10},{30,-10}},1.0);
    ContinuousFiberConfig config;
    config.landing_length_mm=2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_extension_length_mm=23;
    auto id=test_id(); id.parent.purpose=FiberPathPurpose::Contour;
    const auto result=FiberPathFinalizer::finalize(candidate,offset_ex(planned,float(scale_(1))),config,id,{},&planned);
    REQUIRE(result.prepared);
    CHECK_NOTHROW(result.prepared->validate());
    CHECK(result.prepared->spans.front().geometry.points.back()==candidate.polyline.points[1]);
    CHECK(unscale<double>(result.prepared->spans.front().geometry.length())==Catch::Approx(2).margin(.002));
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
        config.cut_to_contact_length_mm=23;config.landing_length_mm=2;
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
    config.landing_length_mm=2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;

    const auto plan=FiberPathValidator::plan_contours({region},config,{},true);
    REQUIRE(plan.audit_lineage());
    REQUIRE(plan.validation.accepted_count()==3);
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
    config.landing_length_mm=2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;

    const auto plan=FiberPathValidator::plan_contours(region,config,{},true);
    REQUIRE(plan.nodes.size()==1);
    REQUIRE(plan.nodes.front().source);
    Polygon ring(plan.nodes.front().source->points);
    CHECK(ring.contains(Point::new_scale(35,15))==(gap>1.1));
    REQUIRE(plan.validation.accepted_count()==1);
    CHECK(plan.validation.assignments.front().centerline->is_closed());
    CHECK(plan.audit_lineage());
}

TEST_CASE("closed contour audit rejects broken centerlines and depositing sequences", "[ContinuousFiber][closed-outer]")
{
    const ExPolygons region{rectangle(-2,-2,82,52)};
    ContinuousFiberConfig config;
    config.contour_flow=Flow(1.f,.13f,.4f); config.contour_count=1;
    config.landing_length_mm=2; config.cut_to_contact_length_mm=23;
    config.minimum_effective_length_mm=.5; config.finish_overlap_length_mm=23;
    config.finish_extension_length_mm=23;

    const auto plan=FiberPathValidator::plan_contours(region,config,{});
    const auto& result=plan.validation;
    REQUIRE(plan.audit_lineage());
    REQUIRE(result.accepted_count()==1);
    REQUIRE(result.assignments.front().prepared);
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
    auto prepared=std::make_shared<PreparedFiberPath>(*broken.assignments.front().prepared);
    for (auto span=prepared->spans.rbegin();span!=prepared->spans.rend();++span)
        if (span->deposits_fiber()) { span->geometry.points.back().x()+=scale_(.1); break; }
    broken.assignments.front().prepared=std::move(prepared);
    REQUIRE(broken.assignments.front().centerline->is_closed());
    CHECK_FALSE(broken.audit_assignments().valid());
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
    REQUIRE(plan.validation.accepted_count()==4);
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
    config.landing_length_mm = 2;
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
    REQUIRE(plan.validation.accepted_count() == 4);
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
            CHECK(assignment.reason == FiberRejectionReason::ProcessBudgetTooShort);
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
    config.landing_length_mm = 2;
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
            CHECK(assignment.reason == FiberRejectionReason::ProcessBudgetTooShort);
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
        const auto& points = path.polyline.to_polyline().points;
        bool intersects = false;
        for (size_t first = 0; first + 1 < points.size(); ++first)
            for (size_t second = first + 2; second + 1 < points.size(); ++second) {
                if (first == 0 && second + 2 == points.size() && points.front() == points.back()) continue;
                intersects |= Geometry::segments_intersect(points[first], points[first + 1], points[second], points[second + 1]);
            }
        const auto result = FiberPathValidator::validate({&path}, {rectangle(-10, -10, 60, 60)}, {},
            FiberPathPurpose::Infill, erContinuousFiberInfill, {});
        REQUIRE(result.assignments.size() == 1);
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
    config.landing_length_mm=2;config.cut_to_contact_length_mm=23;config.minimum_effective_length_mm=.5;
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
    config.landing_length_mm=2;config.cut_to_contact_length_mm=23;config.minimum_effective_length_mm=.5;
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
    config.landing_length_mm=2;config.cut_to_contact_length_mm=23;config.minimum_effective_length_mm=.5;
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
        const auto result=FiberPathFinalizer::finalize(candidate,material,config,test_id(),{},&centerlines);
        REQUIRE(result.prepared);
        CHECK(result.prepared->outside_domain.empty());
        CHECK(unscale<double>(get_extents(result.prepared->physical_coverage).min.x())>=-.0001);
        // Moving the same bend genuinely outside must still fail validation.
        auto outside=candidate;outside.polyline.points[1].x()=scale_(.3);
        const auto invalid=FiberPathFinalizer::finalize(outside,material,config,test_id(),{},&centerlines);
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
