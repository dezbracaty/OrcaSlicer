#include <catch2/catch_all.hpp>

#include "libslic3r/ContinuousFiber/FiberIsland.hpp"
#include "libslic3r/ContinuousFiber/FiberPathFinalizer.hpp"
#include "libslic3r/ContinuousFiber/FiberPathValidator.hpp"
#include "libslic3r/ContinuousFiber/FiberPolicyKey.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/FiberGCodeBlockParser.hpp"
#include "libslic3r/GCodeWriter.hpp"

#include <set>
#include <limits>

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
    second.minimum_segment_length_mm = 0.5;
    CHECK_FALSE(fiber_policy_key(first, 0.0, true) == fiber_policy_key(second, 0.0, true));

    second = first;
    second.maximum_turn_angle_degrees = 90.0;
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

TEST_CASE("fiber validator enforces minimum segment length", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(path_from_points({
        {1, 1}, {1.5, 1}, {1.5, 11}
    })));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;
    config.minimum_segment_length_mm = 1.0;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().reason == FiberRejectionReason::SegmentTooShort);
    CHECK(result.audit_assignments().valid());
}

TEST_CASE("fiber validator enforces the maximum turn angle", "[ContinuousFiber]")
{
    ExtrusionEntityCollection candidate_collection;
    candidate_collection.entities.push_back(new ExtrusionPath(path_from_points({
        {1, 1}, {6, 1}, {6, 6}
    })));
    const ExPolygons domain {rectangle(0, 0, 12, 12)};
    ContinuousFiberConfig config;
    config.maximum_turn_angle_degrees = 45.0;

    FiberValidationResult result = FiberPathValidator::validate(
        {&candidate_collection}, domain, config, FiberPathPurpose::Infill,
        erContinuousFiberInfill, test_id().parent.domain);

    REQUIRE(result.assignments.size() == 1);
    CHECK(result.assignments.front().reason == FiberRejectionReason::TurnLimitExceeded);
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
    CHECK_THROWS(FiberPathFinalizer::finalize(straight_path(5,5,25,5), domain, config, contour_id));
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
    const auto candidate = path_from_points({{5,5},{5.00001,5.00001},
        {15.0001,5},{15.0001,15},{5,15},{5,5.00001},{5,5}});
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
