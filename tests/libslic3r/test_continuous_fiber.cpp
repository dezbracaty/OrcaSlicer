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

TEST_CASE("fiber finish is selected by purpose not closed topology", "[ContinuousFiber][finish]")
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
    const auto unavailable = FiberPathFinalizer::finalize(straight_path(5,5,25,5), domain, config, contour_id);
    CHECK(unavailable.failure == FiberFinalizationFailure::FinishUnavailable);
    CHECK_FALSE(unavailable.detail.empty());
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
        const auto result=ContinuousFiberFillStrategy::round_contour(source,allowed,{radius});
        for (const auto& issue:result.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(result.path);
        CHECK(result.path->points.front()==result.path->points.back());
        CHECK(result.path->length()>source.length()*.9);
        CHECK(diff_pl(Polylines{result.path->to_polyline()},offset_ex(allowed,float(scale_(.0001)))).empty());
        REQUIRE_FALSE(result.arcs.empty());
        double end=0;
        for (const auto& arc:result.arcs) {
            CHECK(arc.radius_mm>=radius-1e-8);
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
        const auto reverse_result=ContinuousFiberFillStrategy::round_contour(reversed,allowed,{radius});
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
    disabled_options.fiber_width_mm=1.0;
    const auto disabled=ContinuousFiberFillStrategy::round_contour(path.polyline,domain,disabled_options);
    REQUIRE(disabled.path);
    CHECK(disabled.path->points==path.polyline.points);
    const auto rounded=ContinuousFiberFillStrategy::round_contour(path.polyline,domain,{.5});
    REQUIRE(rounded.path);
    CHECK(rounded.arcs.size()==4);
    for(const auto& arc:rounded.arcs) CHECK(std::abs(arc.sweep_radians)==Catch::Approx(PI/2));
    const auto impossible=ContinuousFiberFillStrategy::round_contour(path.polyline,domain,{30});
    CHECK_FALSE(impossible.path);
    REQUIRE_FALSE(impossible.issues.empty());
    CHECK(path.polyline.points.size()==5);
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
        const auto rounded=ContinuousFiberFillStrategy::round_contour(source,domain,{.1});
        REQUIRE(rounded.path);
        CHECK(rounded.path->length()>source.length()*.9);
        CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
        // Changing the cyclic seam cannot change which neighbouring corner is merged.
        source.points.pop_back();
        std::rotate(source.points.begin(),source.points.begin()+source.points.size()/2,source.points.end());
        source.points.push_back(source.points.front());
        const auto reseamed=ContinuousFiberFillStrategy::round_contour(source,domain,{.1});
        REQUIRE(reseamed.path);
        CHECK(reseamed.path->points==rounded.path->points);
    }
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
    const auto rounded=ContinuousFiberFillStrategy::round_contour(source.polyline,domain,{.1});
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
        auto result=ContinuousFiberFillStrategy::round_contour(source.polyline,domain,options);
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
            const auto result=ContinuousFiberFillStrategy::round_contour(source,allowed,{radius});
            for(const auto& issue:result.issues) INFO(contour_rounding_failure_name(issue.reason));
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
                CHECK(a.radius_mm>=radius-1e-8);
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
            const auto rotated=ContinuousFiberFillStrategy::round_contour(reseamed,allowed,{radius});
            REQUIRE(rotated.path);
            CHECK(rotated.path->points==result.path->points);
            auto reversed=source;reversed.reverse();
            auto opposite=ContinuousFiberFillStrategy::round_contour(reversed,allowed,{radius});
            REQUIRE(opposite.path);
            opposite.path->reverse();
            CHECK(opposite.path->points==result.path->points);

            // The process strip must have a positive inner bend radius, even
            // when the requested lower bound is smaller than its half-width.
            ContourRoundingOptions strip_options{radius};
            strip_options.fiber_width_mm=1.0;
            const auto strip=ContinuousFiberFillStrategy::round_contour(source,allowed,strip_options);
            REQUIRE(strip.path);
            if (std::string(file)=="layer62.json" && radius==.3)
                CHECK(ContinuousFiberFillStrategy::contour_coverage_overlap(source,*strip.path,1.0).second>0); // Report the local width conflict; do not relabel it as radius failure.
            CHECK(strip.path->length()>source.length()*.9);
            for (const auto& arc:strip.arcs) {
                CHECK(arc.radius_mm>=radius-1e-8);
                CHECK(arc.radius_mm-.5>=strip_options.geometry_tolerance_mm-1e-8);
            }
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
        const auto result=ContinuousFiberFillStrategy::round_contour(input,{area},{.3});
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
    const auto invalid=ContinuousFiberFillStrategy::round_contour(crossing.polyline,{rectangle(-1,-1,11,11)},{.3});
    REQUIRE_FALSE(invalid.path);
    REQUIRE_FALSE(invalid.issues.empty());
    CHECK(invalid.issues.front().reason==ContourRoundingFailure::InvalidInput);
    const auto source=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    ExPolygon narrow=rectangle(-.001,-.001,10.001,10.001);
    Polygon hole=rectangle(.001,.001,9.999,9.999).contour;
    hole.make_clockwise();narrow.holes.push_back(hole);
    const auto impossible=ContinuousFiberFillStrategy::round_contour(source.polyline,{narrow},{.3});
    CHECK_FALSE(impossible.path);
    CHECK_FALSE(impossible.issues.empty());
}


TEST_CASE("parallel supports may connect with a straight line without zero-length arcs", "[ContinuousFiber][ContourRounding]")
{
    const auto source=path_from_points({{0,0},{4,0},{4,.1},{4.1,.1},{4.1,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-1,-1,11,11)};
    const auto rounded=ContinuousFiberFillStrategy::round_contour(source.polyline,domain,{.3});
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
    const auto dense=ContinuousFiberFillStrategy::round_contour(subdivided,domain,{.3});
    REQUIRE(dense.path);
    CHECK(dense.path->points==rounded.path->points);
}


TEST_CASE("whole model contour windows preserve neighbouring supports", "[ContinuousFiber][ContourRounding][whole-model]")
{
    const double width=GENERATE(0.0,1.0);
    CAPTURE(width);
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
        options.fiber_width_mm=width;
        const auto rounded=ContinuousFiberFillStrategy::round_contour(source,domain,options);
        for (const auto& issue:rounded.issues) INFO(contour_rounding_failure_name(issue.reason));
        REQUIRE(rounded.path);
        CHECK(rounded.issues.empty());
        CHECK(rounded.path->points.front()==rounded.path->points.back());
        CHECK(diff_pl(Polylines{rounded.path->to_polyline()},offset_ex(domain,float(scale_(.0001)))).empty());
        if (unscale<double>(source.length())>25.5) CHECK(rounded.path->length()>source.length()*.9);
        for (const auto& arc:rounded.arcs) {
            CHECK(arc.radius_mm>=.3-1e-8);
            if (width>0) CHECK(arc.radius_mm-.5*width>=options.geometry_tolerance_mm-1e-8);
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
    ContourRoundingOptions options{.3};options.fiber_width_mm=1;
    const auto result=ContinuousFiberFillStrategy::round_contour(source.polyline,{rectangle(-5,-5,15,35)},options);
    REQUIRE(result.path);
    CHECK(result.issues.empty());
    const auto overlap=ContinuousFiberFillStrategy::contour_coverage_overlap(source.polyline,*result.path,1.0);
    CHECK(overlap.first>3.5);
    // Fillets slightly extend overlap near the neck ends; the long pre-existing
    // 0.4 mm strip intersection must not be counted again as newly introduced.
    CHECK(overlap.second>0);
    CHECK(overlap.second<.02);
    const auto square=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const auto isolated=ContinuousFiberFillStrategy::round_contour(square.polyline,{rectangle(-5,-5,15,15)},options);
    REQUIRE(isolated.path);
    CHECK(ContinuousFiberFillStrategy::contour_coverage_overlap(square.polyline,*isolated.path,1.0).second==0);
    options.fiber_width_mm=0;
    const auto geometry=ContinuousFiberFillStrategy::round_contour(source.polyline,{rectangle(-5,-5,15,35)},options);
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
    ContourRoundingOptions options{.3};options.fiber_width_mm=1.;
    const auto rounded=ContinuousFiberFillStrategy::round_contour(source,domain,options);
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
        CHECK(a.radius_mm>=.5001-1e-8);
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

TEST_CASE("rejected short rounded contours do not compute coverage diagnostics", "[ContinuousFiber][ContourRounding]")
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
    CHECK(assignment.contour_source_overlap_mm2==0);
    CHECK(assignment.contour_added_overlap_mm2==0);
    config.minimum_path_length_mm=0;
    const auto accepted=FiberPathValidator::validate({&candidate},domain,config,
        FiberPathPurpose::Contour,erContinuousFiberContour,{16,53,0,0});
    REQUIRE(accepted.accepted_count()==1);
    // A sharp source square has inside-corner overlap. Its accepted assignment
    // must still report it, so moving diagnostics does not silently disable them.
    CHECK(accepted.assignments.front().contour_source_overlap_mm2>0);
}

TEST_CASE("optional contour improvement preserves a validated ring on global conflicts", "[ContinuousFiber][ContourRounding][improvement]")
{
    using namespace continuous_fiber_detail;
    const auto source=path_from_points({{0,0},{10,0},{10,10},{0,10},{0,0}});
    const ExPolygons domain{rectangle(-2,-2,12,12)};
    const ContourRoundingOptions options{.5};
    const auto baseline=ContinuousFiberFillStrategy::round_contour(source.polyline,domain,options);
    REQUIRE(baseline.path);
    REQUIRE(baseline.arcs.size()==4);
    std::vector<TangentSolution> values;
    for (const auto& arc:baseline.arcs) values.push_back({{arc},0,0,1});
    size_t validations=0;
    const auto validate=[&](const auto& proposal) {
        ++validations;
        return validate_cycle(proposal,source.polyline.to_polyline(),domain,domain,options);
    };
    SECTION("individually valid arcs make a crossing complete ring") {
        auto crossing=values;
        std::swap(crossing[1],crossing[2]);
        const auto rejected=validate(crossing);
        REQUIRE_FALSE(rejected.path);
        REQUIRE(std::any_of(rejected.issues.begin(),rejected.issues.end(),[](const auto& issue) {
            return issue.reason==ContourRoundingFailure::SelfIntersection;
        }));
        const auto retained=refine_validated_cycle(baseline,values,[&](auto& proposal) {
            proposal=crossing;return true;
        },validate);
        REQUIRE(retained.path);
        CHECK(retained.path->points==baseline.path->points);
        CHECK(retained.issues.empty());
        CHECK(values[1].arcs.front().start_mm==baseline.arcs[1].start_mm);
    }
    SECTION("unchanged proposal does not repeat whole-ring validation") {
        const auto retained=refine_validated_cycle(baseline,values,[](auto&){return false;},validate);
        REQUIRE(retained.path);
        CHECK(validations==0);
    }
    SECTION("optional sampling exhaustion cannot reject an accepted ring") {
        const auto retained=refine_validated_cycle(baseline,values,[](auto&)->bool {
            throw std::length_error("sampling budget");
        },validate);
        REQUIRE(retained.path);
        CHECK(retained.path->points==baseline.path->points);
        CHECK(validations==0);
    }
    SECTION("unexpected errors are not hidden as unsuccessful improvement") {
        CHECK_THROWS_AS(refine_validated_cycle(baseline,values,[](auto&)->bool {
            throw std::logic_error("broken invariant");
        },validate),std::logic_error);
    }
    SECTION("a globally valid proposal replaces the baseline") {
        const auto larger=ContinuousFiberFillStrategy::round_contour(source.polyline,domain,{.7});
        REQUIRE(larger.path);
        const auto improved=refine_validated_cycle(baseline,values,[&](auto& proposal) {
            proposal.clear();
            for (const auto& arc:larger.arcs) proposal.push_back({{arc},0,0,0});
            return true;
        },validate);
        REQUIRE(improved.path);
        CHECK(improved.path->points==larger.path->points);
        CHECK(validations==1);
    }
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
    const auto rounded=ContinuousFiberFillStrategy::round_contour(source.polyline,domain,options);
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
                enqueue({{0,2},{2,1},{3,1}},0.); // Locally preferred, globally crossing.
                enqueue({{0,1},{1,2},{3,1}},1.); // Worse local score, valid whole ring.
                enqueue({{2,1},{3,1},{0,2}},0.); // Same partition, different cyclic seam.
                auto rotated=initial;std::rotate(rotated.begin(),rotated.begin()+1,rotated.end());
                enqueue(rotated,-1.); // Must not retry the original partition.
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
    const auto result=ContinuousFiberFillStrategy::round_contour(source.polyline,
        {rectangle(-1,-1,200001,200001)},options);
    REQUIRE_FALSE(result.path);
    REQUIRE(result.issues.size()==1);
    CHECK(result.issues.front().reason==ContourRoundingFailure::SamplingLimit);
}
