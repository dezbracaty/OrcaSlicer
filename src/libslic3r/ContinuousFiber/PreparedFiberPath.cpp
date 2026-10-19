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
    double result = entry.lowers() ? entry.end_station_mm : 0.0;
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
void validate_origin(const FiberExecutionPiece& origin,const FiberFragmentId& id)
{
    const auto require=[](bool ok,const char* message){if(!ok)throw std::invalid_argument(message);};
    require(origin.source.parent.domain==id.parent.domain,"Fiber source belongs to another domain");
    require(origin.display_purpose==FiberPathPurpose::Contour || origin.display_purpose==FiberPathPurpose::Infill,
            "Fiber source has an invalid display purpose");
    require(std::isfinite(origin.execution_begin_mm) && std::isfinite(origin.execution_end_mm) &&
            origin.execution_begin_mm>=0 && origin.execution_end_mm>origin.execution_begin_mm,
            "Fiber source has an invalid execution range");
    if(origin.kind==FiberExecutionPieceKind::RetainedSource) {
        require(std::isfinite(origin.source_begin_mm) && std::isfinite(origin.source_end_mm) &&
                origin.source_begin_mm>=0 && origin.source_end_mm>origin.source_begin_mm,
                "Fiber source has an invalid positive source range");
        if(origin.source_station_knots) {
            const auto& chart=*origin.source_station_knots;
            require(chart.size()>=2 && chart.front().first<=origin.execution_begin_mm &&
                    chart.back().first>=origin.execution_end_mm && chart.front().second<=origin.source_begin_mm &&
                    chart.back().second>=origin.source_end_mm,"Fiber source exceeds its frozen chart");
            for(size_t i=0;i<chart.size();++i)
                require(std::isfinite(chart[i].first) && std::isfinite(chart[i].second) && chart[i].first>=0 && chart[i].second>=0 &&
                        (!i || (chart[i].first>chart[i-1].first && chart[i].second>chart[i-1].second)),
                        "Fiber source chart is not strictly ordered");
            const auto at=[&](double station) {
                if(station==chart.front().first)return chart.front().second;
                if(station==chart.back().first)return chart.back().second;
                const auto next=std::upper_bound(chart.begin(),chart.end(),station,[](double value,const auto& knot){return value<knot.first;});
                const auto& prev=*(next-1);
                return std::clamp(prev.second+(station-prev.first)/(next->first-prev.first)*(next->second-prev.second),chart.front().second,chart.back().second);
            };
            require(at(origin.execution_begin_mm)==origin.source_begin_mm && at(origin.execution_end_mm)==origin.source_end_mm,
                    "Fiber source endpoints differ from its frozen chart");
        }
    } else {
        require(origin.kind==FiberExecutionPieceKind::Transition && !origin.source_station_knots &&
                origin.other_source.parent.domain==id.parent.domain && !(origin.other_source==origin.source),
                "Invalid fiber transition source");
    }
}

std::vector<FiberProcessAction> planned_actions(const PreparedFiberPath& path)
{
    std::vector<FiberProcessAction> result;
    const auto add = [&](FiberActionType type, size_t span = 0) { result.push_back({type, span}); };
    add(FiberActionType::Begin);
    add(FiberActionType::Approach);
    if (path.start_procedure.z_hop_height_mm > 0) add(FiberActionType::ZHop);
    if (path.start_procedure.prefeed_length_mm > 0) add(FiberActionType::Prefeed);
    size_t i = 0;
    if (path.entry.lowers()) {
        add(path.entry.end_station_mm>0 ? FiberActionType::LandingSpan : FiberActionType::LowerToLayer);
        add(FiberActionType::AdhesionDwell);
        add(FiberActionType::Start, i);
    } else {
        add(FiberActionType::Start, i);
        if (i<path.spans.size() && path.spans[i].kind==FiberMotionKind::PoweredEntry)
            add(FiberActionType::MotionSpan,i++);
        // Keep even a zero-length first source edge and G4 P0 in old order.
        add(FiberActionType::AdhesionDwell);
    }
    const size_t first_powered = i;
    while (i < path.spans.size() && path.spans[i].actively_feeds_fiber())
        add(FiberActionType::MotionSpan, i++);
    if (i == first_powered && (i==0 || path.spans[i-1].kind!=FiberMotionKind::PoweredEntry))
        throw std::invalid_argument("Fiber plan needs active deposition before cut");
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
    require(std::isfinite(entry.end_station_mm) && entry.end_station_mm>=0, "Invalid entry station");
    require(entry.source.start.z()==0 && entry.source.end.z()==0 && entry.contact.z()==0, "Invalid entry LayerXY");
    require(entry.lowers()==(start_procedure.z_hop_height_mm>0), "Entry mode differs from Z-hop");
    const Vec3d source_delta=(entry.source.end-entry.source.start).cast<double>();
    const Vec3d contact_delta=(entry.contact-entry.source.start).cast<double>();
    const double first_length=unscale<double>(source_delta.norm());
    require(entry.end_station_mm<=first_length+0.000002, "Entry exceeds its first source edge");
    if (entry.end_station_mm>0) {
        const Vec3d expected=source_delta*(entry.end_station_mm/first_length);
        require(unscale<double>((contact_delta-expected).norm())<=0.000002, "Entry is not on its first source edge");
    } else require(entry.contact==entry.source.start,"Zero entry must retain its source start");
    require(spans.front().geometry.points.front()==(entry.lowers()?entry.contact:entry.source.start),
            "Entry does not join the motion spans");
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
    double entry_station=0.0;
    for (const auto& piece:entry.pieces) {
        validate_origin(piece,id);
        require(piece.execution_begin_mm==entry_station && piece.execution_end_mm>entry_station,
            "Entry provenance has a discontinuous source range");
        entry_station=piece.execution_end_mm;
    }
    require((entry.pieces.empty() && command_pieces.empty()) ||
            entry_station==(entry.lowers()?entry.end_station_mm:0.0), "Entry provenance is incomplete");
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
        double command_station=0.0, planning_station=entry.lowers()?entry.end_station_mm:0.0;
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
                validate_origin(origin,id);
                if (origin.kind==FiberExecutionPieceKind::RetainedSource) {
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

void bind_fiber_entry_motion(BoundFiberExecutionPlan& plan, const GCodeWriter& writer, const Vec2d& start_xy,
    const Vec2d& contact_xy, double layer_z)
{
    if (!plan.prepared || !start_xy.allFinite() || !contact_xy.allFinite() || !std::isfinite(layer_z))
        throw std::invalid_argument("Invalid fiber entry placement");
    const auto& start=plan.prepared->start_procedure;
    plan.entry_motion.from=writer.quantized_machine_position(Vec3d(start_xy.x(),start_xy.y(),layer_z+start.z_hop_height_mm));
    plan.entry_motion.writer_target=Vec3d(contact_xy.x(),contact_xy.y(),layer_z);
    plan.entry_motion.to=writer.quantized_machine_position(plan.entry_motion.writer_target);
    plan.entry_motion.speed_mm_s=start.landing_speed_mm_s;
    plan.entry_motion.bound=true;
}

std::string emit_fiber_landing(GCodeWriter& writer, const BoundFiberExecutionPlan& plan)
{
    if (!plan.prepared || !plan.prepared->entry.lowers() || !plan.entry_motion.bound)
        throw std::invalid_argument("Unbound fiber landing motion");
    // Keep the legacy single G1 XYZ F command, rather than a separate G1 F.
    writer.set_speed(plan.entry_motion.speed_mm_s*60.0);
    return ";FIBER_LANDING_BEGIN\n" +
        writer.travel_to_bound_xyz(plan.entry_motion.writer_target,plan.entry_motion.to) +
        ";FIBER_LANDING_END\n";
}

} // namespace Slic3r
