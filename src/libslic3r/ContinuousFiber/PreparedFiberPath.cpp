#include "PreparedFiberPath.hpp"
#include <cmath>
#include <stdexcept>

namespace Slic3r {

double PreparedFiberPath::total_depositing_length_mm() const
{
    double result = 0.0;
    for (const FiberMotionSpan& span : spans)
        if (span.deposits_fiber())
            result += unscale<double>(span.geometry.length());
    return result;
}

double PreparedFiberPath::passive_tail_length_mm() const
{
    double result = 0.0;
    for (const FiberMotionSpan& span : spans)
        if (span.kind == FiberMotionKind::PassiveDepositingAfterCut)
            result += unscale<double>(span.geometry.length());
    return result;
}


namespace {
std::vector<FiberProcessAction> planned_actions(const PreparedFiberPath& path)
{
    std::vector<FiberProcessAction> result;
    const auto add = [&](FiberActionType type, size_t span = 0) { result.push_back({type, span}); };
    add(FiberActionType::Begin);
    add(FiberActionType::Approach);
    if (path.start_procedure.z_hop_height_mm > 0) add(FiberActionType::ZHop);
    if (path.start_procedure.prefeed_length_mm > 0) add(FiberActionType::Prefeed);
    size_t i = 0;
    if (i < path.spans.size() && path.spans[i].kind == FiberMotionKind::PrefedLanding)
        add(FiberActionType::LandingSpan, i++);
    else if (path.start_procedure.z_hop_height_mm > 0)
        add(FiberActionType::LowerToLayer);
    if (path.start_procedure.adhesion_dwell_ms > 0) add(FiberActionType::AdhesionDwell);
    add(FiberActionType::Start, i);
    const size_t first_powered = i;
    while (i < path.spans.size() && path.spans[i].actively_feeds_fiber())
        add(FiberActionType::MotionSpan, i++);
    if (i == first_powered) throw std::invalid_argument("Fiber plan needs active deposition before cut");
    add(FiberActionType::Cut, i);
    while (i < path.spans.size() && path.spans[i].kind == FiberMotionKind::PassiveDepositingAfterCut)
        add(FiberActionType::MotionSpan, i++);
    add(FiberActionType::FiberDepleted, i);
    while (i < path.spans.size() && path.spans[i].kind == FiberMotionKind::NonDepositingFinish)
        add(FiberActionType::MotionSpan, i++);
    if (i != path.spans.size()) throw std::invalid_argument("Invalid fiber motion phase order");
    add(FiberActionType::Finish, i);
    add(FiberActionType::End, i);
    return result;
}
} // namespace

void PreparedFiberPath::finalize_actions() { actions = planned_actions(*this); }

void PreparedFiberPath::validate() const
{
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::invalid_argument(message);
    };
    require(!spans.empty(), "Fiber plan has no spans");
    require(std::isfinite(acceleration_mm_s2) && acceleration_mm_s2 > 0, "Invalid fiber acceleration");
    const auto expected = planned_actions(*this);
    require(actions.size() == expected.size(), "Invalid fiber action count");
    for (size_t i = 0; i < actions.size(); ++i)
        require(actions[i].type == expected[i].type && actions[i].span_index == expected[i].span_index,
                "Invalid fiber action order");
    for (size_t i = 0; i < spans.size(); ++i) {
        const auto& span = spans[i];
        require(span.geometry.points.size() >= 2 && span.edges.size()+1 == span.geometry.points.size(),
                "Fiber edge process count differs from geometry");
        if (i) require(spans[i-1].geometry.points.back() == span.geometry.points.front(), "Discontinuous fiber spans");
        for (const auto& point : span.geometry.points)
            require(point.z() == 0, "Fiber LayerXY Z must be zero");
        for (size_t edge = 0; edge < span.edges.size(); ++edge) {
            const auto& process = span.edges[edge];
            require(std::isfinite(process.speed_mm_s) && process.speed_mm_s > 0 && process.speed_mm_s * 60.0 < 100000.0, "Invalid fiber speed");
            require(std::isfinite(process.feed_mm_per_xy_mm) &&
                    (span.actively_feeds_fiber() ? process.feed_mm_per_xy_mm > 0 :
                     process.feed_mm_per_xy_mm == 0), "Invalid fiber feed ratio");
            // A 0.001 mm diagonal may round both axes to the same coordinate.
            // The cell diagonal bound is translation-independent, so reject before
            // coverage is committed (the emitter still checks instance coordinates).
            require(unscale<double>((span.geometry.points[edge+1]-span.geometry.points[edge]).cast<double>().norm()) > std::sqrt(2.0) * 0.001,
                    "Fiber command edge is below coordinate resolution");
        }
    }
    const double values[] = {start_procedure.prefeed_length_mm, start_procedure.prefeed_speed_mm_s,
        start_procedure.z_hop_height_mm, start_procedure.landing_speed_mm_s, start_procedure.start_speed_mm_s};
    for (double v : values) require(std::isfinite(v) && v >= 0, "Invalid fiber start procedure");
    require(start_procedure.adhesion_dwell_ms >= 0, "Negative fiber dwell");
    require(start_procedure.prefeed_length_mm == 0 || start_procedure.prefeed_speed_mm_s > 0, "Invalid prefeed speed");
    require(start_procedure.z_hop_height_mm == 0 || start_procedure.landing_speed_mm_s > 0, "Invalid lowering speed");
}

BoundFiberExecutionPlan bind_fiber_execution(
    std::shared_ptr<const PreparedFiberPath> prepared,
    unsigned filament_id, unsigned extruder_id, unsigned physical_tool_id,
    double e_units_per_mm, std::string cut_gcode)
{
    if (!prepared) throw std::invalid_argument("Missing prepared fiber plan");
    prepared->validate();
    if (prepared->logical_filament_id != filament_id)
        throw std::invalid_argument("Final tool ordering changed fiber material");
    if (!std::isfinite(e_units_per_mm) || e_units_per_mm <= 0)
        throw std::invalid_argument("Invalid fiber E units/mm");
    if (cut_gcode.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("Missing fiber cut event");
    BoundFiberExecutionPlan bound;
    bound.prepared = std::move(prepared);
    bound.logical_filament_id = filament_id;
    bound.logical_extruder_id = extruder_id;
    bound.physical_tool_id = physical_tool_id;
    bound.e_units_per_mm = e_units_per_mm;
    bound.cut_gcode = std::move(cut_gcode);
    for (const auto& span : bound.prepared->spans) {
        std::vector<double> deltas;
        for (size_t i = 0; i < span.edges.size(); ++i) {
            const double length = unscale<double>((span.geometry.points[i+1]-span.geometry.points[i]).cast<double>().norm());
            const double delta = length * span.edges[i].feed_mm_per_xy_mm * e_units_per_mm;
            if (!std::isfinite(delta) || (span.actively_feeds_fiber() && delta < 0.00001))
                throw std::invalid_argument("Fiber E command is not representable");
            deltas.push_back(delta);
        }
        bound.edge_dE.push_back(std::move(deltas));
    }
    bound.prefeed_dE = bound.prepared->start_procedure.prefeed_length_mm * e_units_per_mm;
    bound.prefeed_F = bound.prepared->start_procedure.prefeed_speed_mm_s * e_units_per_mm * 60.0;
    if (!std::isfinite(bound.prefeed_dE) || !std::isfinite(bound.prefeed_F) || bound.prefeed_F >= 100000.0 ||
        (bound.prefeed_dE > 0 && bound.prefeed_dE < 0.00001))
        throw std::invalid_argument("Fiber prefeed conversion overflow");
    return bound;
}

} // namespace Slic3r
