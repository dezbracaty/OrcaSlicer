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

void FiberMachineMotionLimits::validate_feedrate(const Vec3d& from, const Vec3d& to,
    double speed_mm_s, EMovePathType type, const Vec2d& center) const
{
    const auto check=[&](const Vec3d& velocity) {
        for (int axis=0;axis<3;++axis)
            if (maximum_axis_speed_mm_s[axis]>0 &&
                std::abs(velocity[axis])>maximum_axis_speed_mm_s[axis]+EPSILON)
                throw std::runtime_error("Bound fiber motion exceeds machine axis feedrate limit");
    };
    if (type==EMovePathType::Arc_move_cw || type==EMovePathType::Arc_move_ccw) {
        const Vec2d first=from.head<2>()-center,last=to.head<2>()-center;
        const double start=std::atan2(first.y(),first.x());
        const bool ccw=type==EMovePathType::Arc_move_ccw;
        const auto progress=[&](double angle) {
            return std::fmod((ccw?angle-start:start-angle)+4*PI,2*PI);
        };
        const double end=std::atan2(last.y(),last.x());
        double sweep=progress(end);
        if (sweep==0) sweep=2*PI;
        const auto check_angle=[&](double angle) {
            check(Vec3d(std::sin(angle)*speed_mm_s,std::cos(angle)*speed_mm_s,0));
        };
        check_angle(start);
        check_angle(end);
        for (int quadrant=0;quadrant<4;++quadrant) {
            const double angle=quadrant*PI/2;
            if (progress(angle)<=sweep) check_angle(angle);
        }
    } else {
        const Vec3d delta=to-from;
        check(delta.norm()>0?Vec3d(delta.normalized()*speed_mm_s):Vec3d::Zero());
    }
}

double PreparedFiberPath::total_depositing_length_mm() const
{
    double result = entry.lowers() ? entry.end_station_mm : 0.0;
    for (const FiberMotionSpan& span : spans)
        if (span.deposits_fiber())
            result += Geometry::length_with_arcs_mm(span.geometry);
    return result;
}

double PreparedFiberPath::passive_tail_length_mm() const
{
    double result = 0.0;
    for (const FiberMotionSpan& span : spans)
        if (span.kind == FiberMotionKind::PassiveDepositingAfterCut)
            result += Geometry::length_with_arcs_mm(span.geometry);
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
    require(!allowed_domain.empty() && std::isfinite(outside_tolerance_mm2) && outside_tolerance_mm2>=0,
            "Fiber plan has no valid model boundary budget");
    require(std::isfinite(entry.end_station_mm) && entry.end_station_mm>=0, "Invalid entry station");
    require(entry.start.z()==0 && entry.contact.z()==0, "Invalid entry LayerXY");
    require(entry.lowers()==(start_procedure.z_hop_height_mm>0), "Entry mode differs from Z-hop");
    Geometry::validate_with_arcs(entry.geometry);
    require(!entry.geometry.points.empty() && entry.geometry.points.front()==entry.start &&
            entry.geometry.points.back()==entry.contact,"Invalid continuous landing route");
    require(std::abs(Geometry::length_with_arcs_mm(entry.geometry)-entry.end_station_mm)<=
            4*std::sqrt(2.)*SCALING_FACTOR*std::max(size_t(1),entry.geometry.fitting_result.size()),
            "Landing route length differs from its process interval");
    require(spans.front().geometry.points.front()==(entry.lowers()?entry.contact:entry.start),
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
        Geometry::validate_with_arcs(span.geometry);
        require(span.geometry.fitting_result.size()==span.edges.size(),"Fiber primitives do not match process intervals");
        for(size_t j=0;j<span.edges.size();++j)
            require(span.geometry.fitting_result[j].start_point_index==j && span.geometry.fitting_result[j].end_point_index==j+1,
                "Fiber fitting record crosses a process boundary");
        if (i) require(spans[i-1].geometry.points.back() == span.geometry.points.front(), "Discontinuous fiber spans");
        for (const auto& point : span.geometry.points)
            require(point.z() == 0, "Fiber LayerXY Z must be zero");
        for (size_t edge = 0; edge < span.edges.size(); ++edge) {
            const auto& process = span.edges[edge];
            require(std::isfinite(process.speed_mm_s) && process.speed_mm_s > 0 && process.speed_mm_s * 60.0 < 100000.0, "Invalid fiber speed");
            require(std::isfinite(process.feed_mm_per_xy_mm) &&
                    (span.actively_feeds_fiber() ? process.feed_mm_per_xy_mm > 0 :
                     process.feed_mm_per_xy_mm == 0), "Invalid fiber feed ratio");
            // A source-grid diagonal may round both axes to the same coordinate.
            // The cell diagonal bound is translation-independent, so reject before
            // coverage is committed (the emitter still checks instance coordinates).
            const double command_length=unscale<double>((span.geometry.points[edge+1]-span.geometry.points[edge]).cast<double>().norm());
            if(command_length<=std::sqrt(2.)*SCALING_FACTOR)
                throw std::invalid_argument("Fiber command edge is below coordinate resolution: length="+
                    std::to_string(command_length)+" type="+std::to_string(unsigned(span.geometry.fitting_result[edge].path_type)));
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
            const auto stations=Geometry::stations_with_arcs_mm(span.geometry);
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
                    command_station+=stations[edge+1]-stations[edge];
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
    double e_units_per_mm, std::string cut_gcode, const GCodeWriter* writer,
    std::function<Vec2d(const Point&)> point_to_writer, double layer_z, bool export_arcs)
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
    if(!point_to_writer) point_to_writer=[](const Point& p){return p.cast<double>()*SCALING_FACTOR;};
    bound.point_to_writer=point_to_writer;
    const auto input=[&](const Point3& p) {const Vec2d xy=point_to_writer(p.to_point());return Vec3d(xy.x(),xy.y(),layer_z);};
    const auto machine=[&](const Vec3d& p) {
        if(writer) return writer->quantized_machine_position(p);
        Vec3d result=p;for(int axis=0;axis<3;++axis)result[axis]=GCodeFormatter::quantize(result[axis],6);return result;
    };
    // Geometry is already planned in millimetres. A late non-isometry would
    // change the physical cutter distance, even if the output were all G1.
    const Vec3d origin=machine(input(Point3(0,0,0)));
    const Vec3d x=machine(input(Point3::new_scale(1.,0.,0.)))-origin;
    const Vec3d y=machine(input(Point3::new_scale(0.,1.,0.)))-origin;
    if(std::abs(x.norm()-1)>1e-5 || std::abs(y.norm()-1)>1e-5 || std::abs(x.dot(y))>1e-5 ||
        std::abs(x.z())>1e-5 || std::abs(y.z())>1e-5)
        throw std::invalid_argument("Fiber execution requires a planar millimetre-preserving transform");
    const bool reflected=x.x()*y.y()-x.y()*y.x()<0;
    double sampling_error=0.00005; // bounded output approximation
    for(unsigned attempt=0;;++attempt) {
        bound.spans.clear();bound.writer_targets.clear();bound.machine_targets.clear();
        bound.arc_offsets.clear();bound.command_types.clear();bound.command_stations_mm.clear();bound.source_primitives.clear();bound.edge_dE.clear();
        double passive_length=0,command_station=0;
        for(const auto& source:bound.prepared->spans) {
            FiberMotionSpan span;span.kind=source.kind;
            std::vector<size_t> owners;
            if (export_arcs) {
                span.geometry=source.geometry;
                span.edges=source.edges;
                for (size_t edge=0;edge<source.edges.size();++edge) owners.push_back(edge);
            } else {
                span.geometry.points.push_back(source.geometry.points.front());
                for (size_t edge=0;edge<source.edges.size();++edge) {
                    // validate() guarantees one fitting record per process edge.
                    // Sample that primitive directly, without rescanning the span.
                    Polyline3 primitive;
                    primitive.points={source.geometry.points[edge],source.geometry.points[edge+1]};
                    auto record=source.geometry.fitting_result[edge];
                    record.start_point_index=0;record.end_point_index=1;
                    primitive.fitting_result.push_back(std::move(record));
                    const auto sampled=Geometry::sample_with_arcs(primitive,
                        source.kind==FiberMotionKind::PassiveDepositingAfterCut?sampling_error:0.00005);
                    for (size_t i=1;i<sampled.points.size();++i) {
                        const size_t index=span.geometry.points.size()-1;
                        span.geometry.points.push_back(sampled.points[i]);
                        span.geometry.fitting_result.push_back({index,index+1,EMovePathType::Linear_move,{}});
                        span.edges.push_back(source.edges[edge]);
                        owners.push_back(edge);
                    }
                }
            }
            std::vector<Vec3d> inputs,actual;
            for(const auto& point:span.geometry.points) {inputs.push_back(input(point));actual.push_back(machine(inputs.back()));}
            std::vector<Vec2d> offsets;std::vector<double> deltas;std::vector<EMovePathType> types;
            std::vector<double> command_stations{command_station};
            for(size_t edge=0;edge<span.edges.size();++edge) {
                const auto& record=span.geometry.fitting_result[edge];
                EMovePathType command_type=record.path_type;
                const bool arc=record.path_type==EMovePathType::Arc_move_cw || record.path_type==EMovePathType::Arc_move_ccw;
                Vec2d ij=Vec2d::Zero();double length=(actual[edge+1]-actual[edge]).norm();
                if(length==0) throw std::invalid_argument("Fiber process primitive disappears after coordinate quantization");
                if(arc) {
                    const Vec3d center=machine(input(Point3(record.arc_data.center,0)));
                    ij=center.head<2>()-actual[edge].head<2>();
                    if(writer) {ij.x()=writer->quantize_xyz(ij.x());ij.y()=writer->quantize_xyz(ij.y());}
                    const Vec2d end=actual[edge+1].head<2>()-(actual[edge].head<2>()+ij);
                    if(ij.norm()==0 || std::abs(ij.norm()-end.norm())>bound.prepared->execution_deviation_mm)
                        throw std::invalid_argument("Quantized fiber arc has inconsistent radii");
                    bool ccw=record.path_type==EMovePathType::Arc_move_ccw;
                    if(reflected) ccw=!ccw;
                    double angle=std::atan2((-ij).x()*end.y()-(-ij).y()*end.x(),(-ij).dot(end));
                    if(ccw && angle<=0)angle+=2*PI;
                    if(!ccw && angle>=0)angle-=2*PI;
                    length=std::abs(angle)*ij.norm();
                    // Record direction in emitted machine axes; source geometry remains unchanged.
                    command_type=ccw?EMovePathType::Arc_move_ccw:EMovePathType::Arc_move_cw;
                }
                if(span.kind==FiberMotionKind::PassiveDepositingAfterCut)passive_length+=length;
                const double delta=length*span.edges[edge].feed_mm_per_xy_mm*e_units_per_mm;
                if(!std::isfinite(delta))throw std::invalid_argument("Fiber E conversion overflow");
                double command_delta=0;
                if(span.actively_feeds_fiber()) {
                    ideal_e+=delta;const double next=GCodeFormatter::quantize_e(ideal_e);
                    command_delta=next-emitted_e;
                    if(command_delta<=0)throw std::invalid_argument("Quantized fiber E command is not representable");
                    emitted_e=next;
                }
                offsets.push_back(ij);deltas.push_back(command_delta);types.push_back(command_type);
                command_station+=length;command_stations.push_back(command_station);
            }
            bound.spans.push_back(std::move(span));bound.source_primitives.push_back(std::move(owners));
            bound.writer_targets.push_back(std::move(inputs));bound.machine_targets.push_back(std::move(actual));
            bound.command_stations_mm.push_back(std::move(command_stations));bound.command_types.push_back(std::move(types));bound.arc_offsets.push_back(std::move(offsets));bound.edge_dE.push_back(std::move(deltas));
        }
        const double error=std::abs(passive_length-bound.prepared->requested_passive_length_mm);
        if(error<=bound.prepared->passive_length_error_mm || bound.prepared->requested_passive_length_mm==0)break;
        if(export_arcs || attempt>=16)throw std::invalid_argument("Actual fiber passive tail exceeds its length budget");
        sampling_error*=0.5;ideal_e=bound.prefeed_dE;emitted_e=bound.prefeed_dE;
    }
    return bound;
}

void bind_fiber_entry_motion(BoundFiberExecutionPlan& plan, const GCodeWriter& writer, const Vec2d& start_xy,
    const Vec2d& contact_xy, double layer_z)
{
    if (!plan.prepared || !start_xy.allFinite() || !contact_xy.allFinite() || !std::isfinite(layer_z))
        throw std::invalid_argument("Invalid fiber entry placement");
    const auto& start=plan.prepared->start_procedure;
    std::vector<double> stations;
    const auto route=Geometry::sample_with_arcs(plan.prepared->entry.geometry,0.00005,&stations);
    const double length=Geometry::length_with_arcs_mm(plan.prepared->entry.geometry);
    plan.entry_motion.writer_targets.clear();plan.entry_motion.machine_targets.clear();
    const Vec2d offset=start_xy-plan.prepared->entry.start.head<2>().cast<double>()*SCALING_FACTOR;
    for(size_t i=0;i<route.points.size();++i) {
        const Vec2d xy=plan.point_to_writer?plan.point_to_writer(route.points[i].to_point()):
            Vec2d(route.points[i].head<2>().cast<double>()*SCALING_FACTOR+offset);
        const double z=layer_z+start.z_hop_height_mm*(length>0?1-stations[i]/length:1);
        const Vec3d input(xy.x(),xy.y(),z);
        plan.entry_motion.writer_targets.push_back(input);
        plan.entry_motion.machine_targets.push_back(writer.quantized_machine_position(input));
    }
    if(length==0) {
        plan.entry_motion.writer_targets.emplace_back(contact_xy.x(),contact_xy.y(),layer_z);
        plan.entry_motion.machine_targets.push_back(writer.quantized_machine_position(plan.entry_motion.writer_targets.back()));
    }
    plan.entry_motion.from=plan.entry_motion.machine_targets.front();
    plan.entry_motion.to=plan.entry_motion.machine_targets.back();
    plan.entry_motion.speed_mm_s=start.landing_speed_mm_s;
    plan.entry_motion.bound=true;
}

} // namespace Slic3r
