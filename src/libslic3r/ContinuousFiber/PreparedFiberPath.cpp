#include "PreparedFiberPath.hpp"
#include "../ClipperUtils.hpp"
#include "../GCodeWriter.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace Slic3r {

void FiberMachineMotionLimits::validate_move(const Vec3d& from, const Vec3d& to) const
{
    if (tip_xy.empty() || !command_to_tip_offset.allFinite() ||
        !std::isfinite(maximum_z_mm) || maximum_z_mm <= 0)
        throw std::runtime_error("Fiber machine motion limits are unavailable");
    Points xy;
    for (const Vec3d& p : {from, to}) {
        if (!p.allFinite() || p.z() < 0 || p.z() > maximum_z_mm ||
            p.head<2>().cwiseAbs().maxCoeff() > 1000000.0)
            throw std::runtime_error("Fiber motion exceeds machine Z/coordinate limits");
        const Vec2d tip = p.head<2>() + command_to_tip_offset;
        const Point point = Point::new_scale(tip.x(), tip.y());
        if (!std::any_of(tip_xy.begin(), tip_xy.end(), [&](const ExPolygon& area) { return area.contains(point); }))
            throw std::runtime_error("Fiber motion exceeds the configured tool working area");
        xy.push_back(point);
    }
    // Clipper treats an open line coincident with a polygon boundary as outside.
    // Include that boundary using one integer coordinate unit, not a user margin.
    if (xy.front() != xy.back() &&
        total_length(diff_pl(Polylines{Polyline(std::move(xy))}, offset_ex(tip_xy, 1.0f))) > scale_(0.00001))
        throw std::runtime_error("Fiber motion crosses a configured machine exclusion area");
}

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
    if (finish_strategy == FiberFinishStrategy::None)
        require(std::none_of(spans.begin(), spans.end(), [](const auto& span) {
            return span.kind == FiberMotionKind::NonDepositingFinish;
        }), "Fiber path without a finish strategy contains finishing motion");
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
    if (!command_pieces.empty()) {
        // Display cutoff can label a Contour process source as Infill. The
        // validator checks the frozen source labels; here only their internal
        // consistency is available, never derive them from process identity.
        std::map<FiberFragmentId,FiberPathPurpose> source_labels;
        for (const auto& command:command_pieces)
            if (command.origin.kind==FiberExecutionPieceKind::RetainedSource) {
                const auto inserted=source_labels.emplace(command.origin.source,command.origin.display_purpose);
                require(inserted.second || inserted.first->second==command.origin.display_purpose,
                        "Fiber source has inconsistent display labels");
            }
        size_t cursor=0;
        double command_station=0.0, planning_station=0.0;
        bool finishing=false;
        const FiberExecutionPiece* previous=nullptr;
        for (size_t span_index=0;span_index<spans.size();++span_index) {
            const auto& span=spans[span_index];
            if (!span.deposits_fiber() && !finishing) {
                finishing=true;
                planning_station=0.0;
                previous=nullptr;
            }
            size_t edge=0;
            while (edge<span.edges.size()) {
                require(cursor<command_pieces.size(),"Fiber command provenance is incomplete");
                const auto& piece=command_pieces[cursor++];
                const auto& origin=piece.origin;
                require(piece.span_index==span_index && piece.begin_edge==edge &&
                        piece.end_edge>edge && piece.end_edge<=span.edges.size(),
                        "Fiber command provenance does not partition actual edges");
                require(std::isfinite(origin.execution_begin_mm) && std::isfinite(origin.execution_end_mm) &&
                        origin.execution_begin_mm==planning_station &&
                        origin.execution_end_mm>origin.execution_begin_mm,
                        "Fiber command provenance has a discontinuous planning range");
                require(origin.source.parent.domain==id.parent.domain,
                        "Fiber command source belongs to another domain");
                require(origin.display_purpose==FiberPathPurpose::Contour || origin.display_purpose==FiberPathPurpose::Infill,
                        "Fiber command provenance has an invalid display purpose");
                if (origin.kind==FiberExecutionPieceKind::RetainedSource) {
                    require(std::isfinite(origin.source_begin_mm) && std::isfinite(origin.source_end_mm) &&
                            origin.source_begin_mm>=0 && origin.source_end_mm>origin.source_begin_mm,
                            "Fiber command provenance has an invalid positive source range");
                    if (origin.source_station_knots) {
                        const auto& chart=*origin.source_station_knots;
                        require(chart.size()>=2 && chart.front().first<=origin.execution_begin_mm &&
                                chart.back().first>=origin.execution_end_mm &&
                                chart.front().second<=origin.source_begin_mm && chart.back().second>=origin.source_end_mm,
                                "Fiber command source range exceeds its frozen chart");
                        for (size_t i=0;i<chart.size();++i)
                            require(std::isfinite(chart[i].first) && std::isfinite(chart[i].second) &&
                                    chart[i].first>=0 && chart[i].second>=0 &&
                                    (!i || (chart[i].first>chart[i-1].first && chart[i].second>chart[i-1].second)),
                                    "Fiber command source chart is not strictly ordered");
                        const auto canonical_at=[&](double station) {
                            if (station==chart.front().first) return chart.front().second;
                            if (station==chart.back().first) return chart.back().second;
                            const auto next=std::upper_bound(chart.begin(),chart.end(),station,
                                [](double s,const auto& knot){return s<knot.first;});
                            const auto& before=*(next-1);
                            return std::clamp(before.second+(station-before.first)/(next->first-before.first)*
                                (next->second-before.second),chart.front().second,chart.back().second);
                        };
                        require(canonical_at(origin.execution_begin_mm)==origin.source_begin_mm &&
                                canonical_at(origin.execution_end_mm)==origin.source_end_mm,
                                "Fiber command source endpoints differ from the frozen chart");
                    }
                    if (previous && previous->kind==FiberExecutionPieceKind::RetainedSource &&
                        previous->source==origin.source)
                        require(previous->source_end_mm==origin.source_begin_mm || origin.source_begin_mm==0.0,
                                "Fiber consecutive source ranges are discontinuous");
                } else {
                    require(!origin.source_station_knots,"Fiber transition cannot carry a source chart");
                    require(origin.kind==FiberExecutionPieceKind::Transition && !finishing &&
                            origin.other_source.parent.domain==id.parent.domain &&
                            !(origin.other_source==origin.source),"Invalid fiber transition provenance");
                    const auto from=source_labels.find(origin.source), to=source_labels.find(origin.other_source);
                    require(from!=source_labels.end() && to!=source_labels.end(),
                            "Fiber transition has no retained source display labels");
                    const auto display=from->second==FiberPathPurpose::Contour || to->second==FiberPathPurpose::Contour ?
                        FiberPathPurpose::Contour: FiberPathPurpose::Infill;
                    require(origin.display_purpose==display,"Fiber transition has a misleading display purpose");
                }
                require(std::isfinite(piece.command_begin_mm) && std::isfinite(piece.command_end_mm) &&
                        std::abs(piece.command_begin_mm-command_station)<=1e-8 &&
                        piece.command_end_mm>piece.command_begin_mm,
                        "Fiber command provenance has an invalid command range");
                for (;edge<piece.end_edge;++edge)
                    command_station+=unscale<double>((span.geometry.points[edge+1]-span.geometry.points[edge]).cast<double>().norm());
                require(std::abs(piece.command_end_mm-command_station)<=1e-8,
                        "Fiber command provenance length differs from actual edges");
                planning_station=origin.execution_end_mm;
                previous=&origin;
            }
        }
        require(cursor==command_pieces.size(),"Fiber command provenance has unused pieces");
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
    double ideal_e = bound.prepared->start_procedure.prefeed_length_mm * e_units_per_mm;
    bound.prefeed_F = bound.prepared->start_procedure.prefeed_speed_mm_s * e_units_per_mm * 60.0;
    if (!std::isfinite(ideal_e) || !std::isfinite(bound.prefeed_F) || bound.prefeed_F >= 100000.0 ||
        (ideal_e > 0 && ideal_e < 0.00001))
        throw std::invalid_argument("Fiber prefeed conversion overflow");
    bound.prefeed_dE = GCodeFormatter::quantize_e(ideal_e);
    double emitted_e = bound.prefeed_dE;
    // Quantize the cumulative owned feed, rather than independently rounding
    // every relative-E command. Residuals stay before CUT; passive spans never
    // receive an E command, including any remaining fraction of one E grid.
    for (const auto& span : bound.prepared->spans) {
        std::vector<double> deltas;
        for (size_t i = 0; i < span.edges.size(); ++i) {
            const double length = unscale<double>((span.geometry.points[i+1]-span.geometry.points[i]).cast<double>().norm());
            const double delta = length * span.edges[i].feed_mm_per_xy_mm * e_units_per_mm;
            if (!std::isfinite(delta) || (span.actively_feeds_fiber() && delta < 0.00001))
                throw std::invalid_argument("Fiber E command is not representable");
            double command_delta = 0.0;
            if (span.actively_feeds_fiber()) {
                ideal_e += delta;
                const double next_e = GCodeFormatter::quantize_e(ideal_e);
                command_delta = next_e-emitted_e;
                if (!std::isfinite(next_e) || !std::isfinite(command_delta) || command_delta <= 0)
                    throw std::invalid_argument("Quantized fiber E command is not representable");
                emitted_e = next_e;
            }
            deltas.push_back(command_delta);
        }
        bound.edge_dE.push_back(std::move(deltas));
    }
    return bound;
}

} // namespace Slic3r
