#include "PreparedFiberPath.hpp"
#include "../ClipperUtils.hpp"
#include "../GCodeWriter.hpp"
#include "../PrintBase.hpp"
#include "ContinuousFiberConfig.hpp"
#include <algorithm>
#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include <cstring>
#include <list>
#include <iomanip>
#include <mutex>
#include <map>
#include <sstream>
#include <stdexcept>

namespace Slic3r {

// A positive certificate only: a connected box containing no polygon boundary
// stays in the same interior region as its first point. Uncertain cases retain
// the original Clipper check, including its boundary and length tolerances.
static bool fiber_line_inside_boundary_disjoint_box(const Point& from, const Point& to,
    const ExPolygons& areas)
{
    if (areas.size() != 1 || !areas.front().holes.empty())
        return false;
    const Points& contour = areas.front().contour.points;
    if (contour.size() < 3)
        return false;
    const auto in_range = [](const Point& point) {
        return point.x() >= -ClipperLib::loRange && point.x() <= ClipperLib::loRange &&
               point.y() >= -ClipperLib::loRange && point.y() <= ClipperLib::loRange;
    };
    if (!in_range(from) || !in_range(to) ||
        !std::all_of(contour.begin(), contour.end(), in_range))
        return false;
    const coord_t min_x = std::min(from.x(), to.x()), max_x = std::max(from.x(), to.x());
    const coord_t min_y = std::min(from.y(), to.y()), max_y = std::max(from.y(), to.y());
    bool inside = false;
    for (size_t i = 0; i < contour.size(); ++i) {
        const Point& a = contour[i];
        const Point& b = contour[(i + 1) % contour.size()];
        // Closed boxes: even a possible vertex touch must use Clipper.
        if (!(max_x < std::min(a.x(), b.x()) || min_x > std::max(a.x(), b.x()) ||
              max_y < std::min(a.y(), b.y()) || min_y > std::max(a.y(), b.y())))
            return false;
        if ((a.y() > from.y()) != (b.y() > from.y())) {
            // With |coordinate| <= L = 2^30-1, each difference is <= 2L,
            // and even 2*(2L)^2 < INT64_MAX. No filtered predicate or epsilon.
            const int64_t cross = (b.x() - a.x()) * (from.y() - a.y()) -
                                  (b.y() - a.y()) * (from.x() - a.x());
            if (cross == 0)
                return false;
            if ((cross > 0) == (b.y() > a.y()))
                inside = !inside;
        }
    }
    // Odd winding also satisfies Clipper's NonZero fill rule, even for an
    // unusual public contour. This is never used as a negative certificate.
    return inside;
}

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
        !fiber_line_inside_boundary_disjoint_box(xy.front(), xy.back(), line_xy) &&
        total_length(diff_pl(Polylines{Polyline(std::move(xy))}, (line_xy.empty()?offset_ex(tip_xy, 1.0f):line_xy))) > scale_(0.00001))
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

double FiberPathDraft::total_depositing_length_mm() const
{
    double result = entry.lowers() ? entry.end_station_mm : 0.0;
    for (const FiberMotionSpan& span : spans)
        if (span.deposits_fiber())
            result += Geometry::length_with_arcs_mm(span.geometry);
    return result;
}

double FiberPathDraft::passive_tail_length_mm() const
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

std::vector<FiberProcessAction> planned_actions(const FiberPathDraft& path)
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

void FiberPathDraft::finalize_actions() { actions = planned_actions(*this); }

void FiberPathDraft::validate() const
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

namespace {
bool is_arc(EMovePathType type) { return type==EMovePathType::Arc_move_cw || type==EMovePathType::Arc_move_ccw; }
std::vector<Vec3d> command_points(const Vec3d& from,const Vec3d& to,EMovePathType type,const Vec2d& ij)
{
    std::vector<Vec3d> points{from};
    if(is_arc(type)) {
        const Vec2d center=from.head<2>()+ij,start=-ij,end=to.head<2>()-center;
        if(start.norm()==0 || std::abs(start.norm()-end.norm())>0.00022)
            throw FiberExecutionError("Invalid quantized fiber circle");
        double angle=std::atan2(start.x()*end.y()-start.y()*end.x(),start.dot(end));
        if(type==EMovePathType::Arc_move_ccw && angle<=0)angle+=2*PI;
        if(type==EMovePathType::Arc_move_cw && angle>=0)angle-=2*PI;
        const double step=2*std::acos(std::clamp(1-0.00005/start.norm(),-1.,1.));
        if(step<=0 || std::abs(angle)/step>1000000)throw FiberExecutionError("Fiber arc checking budget exceeded");
        const size_t count=std::max(size_t(1),size_t(std::ceil(std::abs(angle)/step)));
        const double theta=std::atan2(start.y(),start.x());
        for(size_t i=1;i<count;++i) {
            const double t=double(i)/count,a=theta+t*angle;
            const Vec2d p=center+start.norm()*Vec2d(std::cos(a),std::sin(a));
            points.emplace_back(p.x(),p.y(),from.z()+t*(to.z()-from.z()));
        }
    }
    points.push_back(to);return points;
}
}

void FiberMachineMotionLimits::prepare()
{
    line_xy=offset_ex(tip_xy,1.f);
    arc_xy=offset_ex(tip_xy,-float(scale_(0.00005)));
    arc_line_xy=offset_ex(arc_xy,1.f);
}

void FiberMachineMotionLimits::validate_motion(const Vec3d& from,const Vec3d& to,double speed,
    EMovePathType type,const Vec2d& ij) const
{
    validate_feedrate(from,to,speed,type,from.head<2>()+ij);
    if(!is_arc(type)) { validate_move(from,to);return; }
    const auto points=command_points(from,to,type,ij);
    // Arc chords lie inside the circle. Reserve its chord approximation error
    // when checking the available machine area.
    FiberMachineMotionLimits arc_limits=*this;
    arc_limits.tip_xy=arc_xy;
    arc_limits.line_xy=arc_line_xy;
    for(size_t i=1;i<points.size();++i)arc_limits.validate_move(points[i-1],points[i]);
}

std::shared_ptr<const FiberMachineMotionLimits> fiber_machine_limits(const PrintConfig& config,unsigned extruder)
{
    auto limits=std::make_shared<FiberMachineMotionLimits>();
    Polygon bed=Polygon::new_scale(config.printable_area.values);bed.make_counter_clockwise();
    limits->tip_xy={ExPolygon(std::move(bed))};
    if(config.bed_exclude_area.values.size()>=3) {
        Polygon excluded=Polygon::new_scale(config.bed_exclude_area.values);excluded.make_counter_clockwise();
        limits->tip_xy=diff_ex(limits->tip_xy,ExPolygons{ExPolygon(std::move(excluded))});
    }
    const auto& areas=config.extruder_printable_area.values;
    if(extruder<areas.size() && areas[extruder].size()>=3) {
        Polygon area=Polygon::new_scale(areas[extruder]);area.make_counter_clockwise();
        limits->tip_xy=intersection_ex(limits->tip_xy,ExPolygons{ExPolygon(std::move(area))});
    }
    limits->command_to_tip_offset=config.extruder_offset.get_at(extruder);
    limits->maximum_z_mm=config.printable_height.value;
    const auto& heights=config.extruder_printable_height.values;
    if(extruder<heights.size() && heights[extruder]>0)limits->maximum_z_mm=std::min(limits->maximum_z_mm,heights[extruder]);
    const ConfigOptionFloats* axes[]={&config.machine_max_speed_x,&config.machine_max_speed_y,&config.machine_max_speed_z};
    for(int i=0;i<3;++i)if(!axes[i]->values.empty())limits->maximum_axis_speed_mm_s[i]=axes[i]->values.front();
    limits->prepare();return limits;
}

double fiber_reference_chord_tolerance(const FiberPathDraft& draft,const FiberMotionSpan& span)
{
    if(span.kind!=FiberMotionKind::PassiveDepositingAfterCut || draft.requested_passive_length_mm<=0)return 0.00005;
    // Select the one G1 representation in local geometry, before material
    // allocation. Stop at the coordinate grid: oversampling a digital circle
    // further can increase its length and creates no new geometric precision.
    double tolerance=0.00005,best=tolerance,best_error=std::numeric_limits<double>::max();
    for(unsigned attempt=0;attempt<16;++attempt) {
        const auto sampled=Geometry::sample_with_arcs(span.geometry,tolerance);
        double length=0;
        for(size_t i=1;i<sampled.points.size();++i)
            length+=(sampled.points[i]-sampled.points[i-1]).cast<double>().norm()*SCALING_FACTOR;
        const double error=std::abs(length-draft.requested_passive_length_mm);
        if(error<best_error){best=tolerance;best_error=error;}
        if(error<=draft.passive_length_error_mm*.75)return tolerance;
        if(tolerance<=.5*SCALING_FACTOR)break;
        tolerance=std::max(tolerance*.5,.5*SCALING_FACTOR);
    }
    return best;
}

static FrozenFiberExecution compile_fiber_execution_impl(const FiberPathDraft& prepared,
    const FiberExecutionContext& context,const FiberPlacement& placement,double layer_z,const std::function<void()>& cancel,
    double chord_tolerance_cap)
{
    try {prepared.validate();}
    catch(const std::invalid_argument& error){throw FiberExecutionError(error.what(),FiberExecutionFailure::InvalidPlan);}
    unsigned physical_tool_id=std::numeric_limits<unsigned>::max();
    try {
    ResolvedFiberTool tool;
    try {tool=resolve_fiber_tool(context.config,prepared.logical_filament_id);}
    catch(const std::invalid_argument& error){throw FiberExecutionError(error.what(),FiberExecutionFailure::Context);}
    physical_tool_id=tool.physical_tool_id;
    FrozenFiberExecution result;
    FrozenFiberCommands commands;
    result.placement=placement;result.layer_z=layer_z;
    result.logical_filament_id=tool.logical_filament_id;result.logical_extruder_id=tool.logical_extruder_id;
    result.physical_tool_id=tool.physical_tool_id;result.e_units_per_mm=tool.e_units_per_mm;
    result.nozzle_offset=context.config.extruder_offset.get_at(tool.logical_extruder_id);
    if(context.check_machine)result.limits=context.machine_limits.at(tool.logical_extruder_id);
    const auto input=[&](const Point3& p,double z) {
        const Vec2d xy=p.head<2>().cast<double>()*SCALING_FACTOR+placement.origin-result.nozzle_offset;
        return Vec3d(xy.x(),xy.y(),z);
    };
    const auto machine=[&](const Point3& p) {return context.transform.quantized(input(p,layer_z),6);};
    const Vec3d origin=machine(Point3(0,0,0));
    const Vec3d x=machine(Point3::new_scale(1.,0.,0.))-origin;
    const Vec3d y=machine(Point3::new_scale(0.,1.,0.))-origin;
    if(std::abs(x.norm()-1)>1e-5 || std::abs(y.norm()-1)>1e-5 || std::abs(x.dot(y))>1e-5 ||
        std::abs(x.z())>1e-5 || std::abs(y.z())>1e-5)
        throw FiberExecutionError("Fiber execution requires a planar millimetre-preserving transform",FiberExecutionFailure::Transform);
    result.layer_z=origin.z();
    const bool reflected=x.x()*y.y()-x.y()*y.x()<0;
    double ideal_e=prepared.start_procedure.prefeed_length_mm*tool.e_units_per_mm;
    result.prefeed_F=prepared.start_procedure.prefeed_speed_mm_s*tool.e_units_per_mm*60;
    if(!std::isfinite(ideal_e) || !std::isfinite(result.prefeed_F) || result.prefeed_F>=100000 ||
        (ideal_e>0 && ideal_e<0.00001))throw FiberExecutionError("Fiber prefeed conversion overflow",FiberExecutionFailure::Feed);
    result.prefeed_dE=GCodeFormatter::quantize_e(ideal_e);
    double emitted_e=result.prefeed_dE,station=0;
    bool has_powered_e=false;
    const auto check_limit=[](double value,const ConfigOptionFloats& option,const char* name) {
        if(!option.values.empty() && option.values.front()>0 && value>option.values.front()+EPSILON)
            throw FiberExecutionError(std::string("Prepared fiber exceeds ")+name,FiberExecutionFailure::Motion);
    };
    if(context.check_machine) {
        const auto& c=context.config;
        check_limit(prepared.acceleration_mm_s2,c.machine_max_acceleration_extruding,"print acceleration");
        check_limit(prepared.acceleration_mm_s2,c.machine_max_acceleration_travel,"travel acceleration");
        check_limit(prepared.acceleration_mm_s2,c.machine_max_acceleration_x,"X acceleration");
        check_limit(prepared.acceleration_mm_s2,c.machine_max_acceleration_y,"Y acceleration");
        check_limit(result.prefeed_F/60,c.machine_max_speed_e,"E prefeed speed");
    }
    // Compile the frozen G1 sampling schedule. Candidate placement never
    // changes it after the geometry/material contract was published.
    size_t command_count=0;
    for(const auto& source:prepared.spans) {
        if(cancel)cancel();
        const double sampling_error=std::min(fiber_reference_chord_tolerance(prepared,source),chord_tolerance_cap);
        for(unsigned attempt=0;;++attempt) {
            FrozenFiberSpan span;span.kind=source.kind;
            span.from=machine(source.geometry.points.front()).head<2>();
            Vec3d from=machine(source.geometry.points.front());
            double length_sum=0;
            for(size_t edge=0;edge<source.edges.size();++edge) {
                Polyline3 primitive;primitive.points={source.geometry.points[edge],source.geometry.points[edge+1]};
                auto record=source.geometry.fitting_result[edge];record.start_point_index=0;record.end_point_index=1;
                primitive.fitting_result.push_back(record);
                auto sampled=context.export_arcs?primitive:Geometry::sample_with_arcs(primitive,sampling_error);
                const bool linear_fallback=std::isfinite(chord_tolerance_cap) && !context.export_arcs && is_arc(record.path_type);
                if(linear_fallback && sampled.points.size()>=4) {
                    // Native butt caps follow the endpoint tangents. Uniform
                    // first/last chords can rotate those caps outside P even
                    // when their centerline sagitta is tiny. Try short tangent
                    // chords on the same source grid; certification below still
                    // rejects every candidate that cannot preserve frozen P/G.
                    const Vec2d center=record.arc_data.center.cast<double>()*SCALING_FACTOR;
                    const double radius=record.arc_data.radius*SCALING_FACTOR;
                    const double limit=std::min(record.arc_data.length*SCALING_FACTOR*.25,
                        std::sqrt(radius*SCALING_FACTOR*.25));
                    const auto tangent_point=[&](size_t endpoint,size_t neighbor,bool at_start) {
                        const Vec2d end=sampled.points[endpoint].head<2>().cast<double>()*SCALING_FACTOR;
                        const Vec2d adjacent=sampled.points[neighbor].head<2>().cast<double>()*SCALING_FACTOR;
                        const Vec2d radial=end-center;
                        Vec2d tangent(-radial.y(),radial.x());
                        if(record.path_type==EMovePathType::Arc_move_cw)tangent=-tangent;
                        const double length=std::min((adjacent-end).norm(),limit);
                        const Vec2d delta=(at_start?1.:-1.)*length*tangent.normalized();
                        // Quantize the displacement before integer addition.
                        // Rounding endpoint +/- delta independently can turn
                        // a symmetric tangent into unequal grid components.
                        Point3 candidate=sampled.points[endpoint];candidate+=Point3::new_scale(delta.x(),delta.y(),0);
                        if(candidate!=sampled.points[endpoint])sampled.points[neighbor]=candidate;
                    };
                    tangent_point(0,1,true);
                    tangent_point(sampled.points.size()-1,sampled.points.size()-2,false);
                }
                const Vec3d ideal_center=linear_fallback?
                    context.transform.machine(input(Point3(record.arc_data.center,0),layer_z)):Vec3d::Zero();
                const Vec2d ideal_start=linear_fallback?
                    (context.transform.machine(input(primitive.points.front(),layer_z))-ideal_center).head<2>().eval():Vec2d::Zero();
                const double arc_direction=(record.path_type==EMovePathType::Arc_move_ccw)!=reflected?1.:-1.;
                const Vec2d first_radial=from.head<2>()-ideal_center.head<2>();
                double arc_phase=linear_fallback?arc_direction*std::atan2(
                    ideal_start.x()*first_radial.y()-ideal_start.y()*first_radial.x(),ideal_start.dot(first_radial)):0;
                for(size_t i=1;i<sampled.points.size();++i) {
                    if((++command_count%1024)==0 && cancel)cancel();
                    const Vec3d to=machine(sampled.points[i]);
                    FrozenFiberCommand command;command.target=to.head<2>();command.source_primitive=uint32_t(edge);
                    command.speed_mm_s=source.edges[edge].speed_mm_s;
                    // Exact endpoint quantization error in canonical LayerXY.
                    // Sampling preserves source endpoints and bounds each chord
                    // by sampling_error; an isometry preserves these distances.
                    // Tangent fallback segments additionally use the analytic
                    // radial/finite-phase bound below, including rounded targets.
                    const Vec3d ideal=context.transform.machine(input(sampled.points[i],layer_z));
                    const double endpoint_error=(to-ideal).norm();
                    result.source_deviation_mm=std::max(result.source_deviation_mm,
                        endpoint_error+sampling_error+2*std::sqrt(2.)*SCALING_FACTOR);
                    if(linear_fallback) {
                        // Bound every point of the actual rounded G1 segment,
                        // not just its endpoints or the old uniform sagitta.
                        // Radial extrema are analytic on a line; angular phase
                        // is monotone on a short chord that misses the center.
                        const Vec2d a=from.head<2>()-ideal_center.head<2>(),b=to.head<2>()-ideal_center.head<2>();
                        const Vec2d delta=b-a;
                        const double denominator=delta.squaredNorm();
                        if(denominator==0)throw FiberExecutionError("Linear arc fallback vanishes on the command grid",FiberExecutionFailure::Quantization);
                        const double minimum_radius=(a+std::clamp(-a.dot(delta)/denominator,0.,1.)*delta).norm();
                        const double maximum_radius=std::max(a.norm(),b.norm());
                        const double advance=arc_direction*std::atan2(a.x()*b.y()-a.y()*b.x(),a.dot(b));
                        if(minimum_radius<=0 || advance<0 || advance>=PI)
                            throw FiberExecutionError("Linear arc fallback leaves its directed source interval",FiberExecutionFailure::Coverage);
                        const double next_phase=arc_phase+advance;
                        const double excursion=std::max({0.,-arc_phase,next_phase-std::abs(record.arc_data.angle_radians)});
                        const double radius=record.arc_data.radius*SCALING_FACTOR;
                        const double radial_error=std::max(std::abs(minimum_radius-radius),std::abs(maximum_radius-radius));
                        const double numeric_guard=4*std::sqrt(2.)*SCALING_FACTOR+
                            64*std::numeric_limits<double>::epsilon()*std::max({1.,radius,ideal_center.cwiseAbs().maxCoeff()});
                        const Vec3d ideal_end=context.transform.machine(input(primitive.points.back(),layer_z));
                        const double source_endpoint_error=std::max(std::abs(ideal_start.norm()-radius),
                            std::abs((ideal_end-ideal_center).head<2>().norm()-radius));
                        result.source_deviation_mm=std::max(result.source_deviation_mm,
                            radial_error+source_endpoint_error+radius*excursion+numeric_guard);
                        arc_phase=next_phase;
                    }
                    Vec3d actual_delta=to-from;
                    for(int axis=0;axis<3;++axis)actual_delta[axis]=GCodeFormatter::quantize(actual_delta[axis],6);
                    double length=actual_delta.norm();
                    if(length==0)throw FiberExecutionError("Fiber process primitive disappears after coordinate quantization",FiberExecutionFailure::Quantization);
                    EMovePathType type=EMovePathType::Linear_move;Vec2d ij=Vec2d::Zero();
                    if(context.export_arcs && is_arc(record.path_type)) {
                        const Vec3d center=machine(Point3(record.arc_data.center,0));
                        ij=center.head<2>()-from.head<2>();
                        for(int axis=0;axis<2;++axis)ij[axis]=GCodeFormatter::quantize(ij[axis],6);
                        const Vec2d end=actual_delta.head<2>()-ij;
                        if(ij.norm()==0 || std::abs(ij.norm()-end.norm())>prepared.execution_deviation_mm)
                            throw FiberExecutionError("Quantized fiber arc has inconsistent radii",FiberExecutionFailure::Quantization);
                        const Vec3d ideal_center=context.transform.machine(input(Point3(record.arc_data.center,0),layer_z));
                        const double center_error=(center-ideal_center).norm();
                        const double radius_error=std::abs(ij.norm()-double(record.arc_data.radius)*SCALING_FACTOR);
                        result.source_deviation_mm=std::max(result.source_deviation_mm,
                            2*(center_error+radius_error+endpoint_error)+8*std::sqrt(2.)*SCALING_FACTOR);
                        bool ccw=record.path_type==EMovePathType::Arc_move_ccw;if(reflected)ccw=!ccw;
                        double angle=std::atan2((-ij).x()*end.y()-(-ij).y()*end.x(),(-ij).dot(end));
                        if(ccw && angle<=0)angle+=2*PI;if(!ccw && angle>=0)angle-=2*PI;
                        length=std::abs(angle)*ij.norm();type=ccw?EMovePathType::Arc_move_ccw:EMovePathType::Arc_move_cw;
                        command.arc_index=int32_t(commands.arcs.size());commands.arcs.push_back({ij,ccw});
                    }
                    length_sum+=length;
                    if(source.actively_feeds_fiber()) {
                        ideal_e+=length*source.edges[edge].feed_mm_per_xy_mm*tool.e_units_per_mm;
                        if(!std::isfinite(ideal_e))throw FiberExecutionError("Fiber E conversion overflow",FiberExecutionFailure::Feed);
                        const double next=GCodeFormatter::quantize_e(ideal_e);command.dE=next-emitted_e;
                        if(!std::isfinite(next) || !std::isfinite(command.dE) || command.dE<0)
                            throw FiberExecutionError("Quantized fiber E command is not representable",FiberExecutionFailure::Feed);
                        has_powered_e |= command.dE>0;
                        emitted_e=next;
                    }
                    command.station_mm=station+length_sum;
                    if(result.limits) {
                        try {result.limits->validate_motion(from,to,command.speed_mm_s,type,ij);}
                        catch(const std::runtime_error& error){throw FiberExecutionError(error.what(),FiberExecutionFailure::Motion);}
                        check_limit(command.speed_mm_s*source.edges[edge].feed_mm_per_xy_mm*tool.e_units_per_mm,
                            context.config.machine_max_speed_e,"E speed");
                    }
                    span.commands.push_back(command);from=to;
                }
            }
            if(source.kind==FiberMotionKind::PassiveDepositingAfterCut && prepared.requested_passive_length_mm>0 &&
                std::abs(length_sum-prepared.requested_passive_length_mm)>prepared.passive_length_error_mm) {
                throw FiberExecutionError("Actual fiber passive tail exceeds its length budget",FiberExecutionFailure::TailBudget);
            }
            station+=length_sum;commands.spans.push_back(std::move(span));break;
        }
    }
    if(!has_powered_e)
        throw FiberExecutionError("Fiber powered feed is not representable",FiberExecutionFailure::Feed);
    result.entry_start=context.transform.quantized(input(prepared.entry.start,layer_z+prepared.start_procedure.z_hop_height_mm),6);
    if(prepared.entry.lowers()) {
        std::vector<double> stations;
        const auto route=Geometry::sample_with_arcs(prepared.entry.geometry,0.00005,&stations);
        const double length=Geometry::length_with_arcs_mm(prepared.entry.geometry);
        for(size_t i=0;i<route.points.size();++i) {
            const double z=layer_z+prepared.start_procedure.z_hop_height_mm*(length>0?1-stations[i]/length:1);
            const Vec3d ideal=context.transform.machine(input(route.points[i],z));
            result.entry_targets.push_back(context.transform.quantized(input(route.points[i],z),6));
            result.entry_stations_mm.push_back(stations[i]);
            result.source_deviation_mm=std::max(result.source_deviation_mm,
                (result.entry_targets.back()-ideal).norm()+0.00005+2*std::sqrt(2.)*SCALING_FACTOR);
        }
        if(length==0) {
            result.entry_targets.push_back(context.transform.quantized(input(prepared.entry.contact,layer_z),6));
            result.entry_stations_mm.push_back(0.);
        }
        if(result.limits)for(size_t i=1;i<result.entry_targets.size();++i) {
            try {result.limits->validate_motion(result.entry_targets[i-1],result.entry_targets[i],prepared.start_procedure.landing_speed_mm_s,
                EMovePathType::Linear_move,Vec2d::Zero());}
            catch(const std::runtime_error& error){throw FiberExecutionError(error.what(),FiberExecutionFailure::Motion);}
        }
    }
    if(result.source_deviation_mm>prepared.execution_deviation_mm)
        throw FiberExecutionError("Quantized fiber geometry exceeds its source approximation budget",FiberExecutionFailure::Coverage);
    result.command_origin=origin.head<2>();
    const auto relative=[&](const Vec2d& absolute) {
        Vec2d point=absolute-result.command_origin;
        for(int axis=0;axis<2;++axis)point[axis]=GCodeFormatter::quantize(point[axis],6);
        if(result.machine_target(point).head<2>()!=absolute)
            throw FiberExecutionError("Relative fiber command does not reproduce its certified machine target",FiberExecutionFailure::Quantization);
        return point;
    };
    for(auto& span:commands.spans) {
        span.from=relative(span.from);
        for(auto& command:span.commands)command.target=relative(command.target);
    }
    result.commands=std::make_shared<const FrozenFiberCommands>(std::move(commands));
    return result;
    } catch(const FiberExecutionError& error) {
        throw FiberExecutionError(error,placement.instance_id,prepared.id,physical_tool_id);
    }
      catch(const CanceledException&) { throw; }
      catch(const std::exception& error) {
        throw FiberExecutionError(FiberExecutionError(error.what(),FiberExecutionFailure::InvalidPlan),placement.instance_id,prepared.id,physical_tool_id);
    }
}

FrozenFiberExecution compile_fiber_execution(const FiberPathDraft& prepared,
    const FiberExecutionContext& context,const FiberPlacement& placement,double layer_z,const std::function<void()>& cancel)
{
    return compile_fiber_execution_impl(prepared,context,placement,layer_z,cancel,std::numeric_limits<double>::infinity());
}

namespace {
Point layer_point(const Vec3d& machine,const FrozenFiberExecution& execution,const FiberExecutionContext& context)
{
    const Vec2d local=context.transform.oriented(machine).head<2>()+execution.nozzle_offset-execution.placement.origin;
    return Point::new_scale(local.x(),local.y());
}
void append_points(Polyline& line,const std::vector<Vec3d>& points,const FrozenFiberExecution& execution,const FiberExecutionContext& context)
{
    for(const auto& p:points) {
        const Point local=layer_point(p,execution,context);
        if(line.points.empty() || line.points.back()!=local)line.points.push_back(local);
    }
}
}

Polyline frozen_fiber_deposition(const FrozenFiberExecution& execution,const FiberExecutionContext& context)
{
    Polyline line;append_points(line,execution.entry_targets,execution,context);
    for(const auto& span:execution.spans()) {
        if(span.kind==FiberMotionKind::NonDepositingFinish)continue;
        Vec3d from=execution.machine_target(span.from);
        for(const auto& command:span.commands) {
            const Vec3d to=execution.machine_target(command.target);
            const auto* arc=command.arc_index<0?nullptr:&execution.arcs().at(command.arc_index);
            append_points(line,command_points(from,to,arc?(arc->ccw?EMovePathType::Arc_move_ccw:EMovePathType::Arc_move_cw):
                EMovePathType::Linear_move,arc?arc->ij:Vec2d::Zero()),execution,context);
            from=to;
        }
    }
    return line;
}

Polylines frozen_fiber_contour_runs(const FiberPathDraft& prepared,const FrozenFiberExecution& execution,
    const FiberExecutionContext& context)
{
    Polylines runs;Polyline current;
    const auto flush=[&] {if(current.points.size()>1)runs.push_back(std::move(current));current.points.clear();};
    const auto add=[&](const std::vector<Vec3d>& points,FiberPathPurpose purpose) {
        if(purpose!=FiberPathPurpose::Contour){flush();return;}
        append_points(current,points,execution,context);
    };
    if(prepared.command_pieces.empty()) {
        if(prepared.id.parent.purpose==FiberPathPurpose::Contour)runs.push_back(frozen_fiber_deposition(execution,context));
        return runs;
    }
    // Entry stations refer to the source route. Its process knots are included
    // by sample_with_arcs, so a piece seam never replaces a curved command.
    if(!execution.entry_targets.empty()) {
        for(size_t i=1;i<execution.entry_targets.size();++i) {
            const double a=execution.entry_stations_mm[i-1],b=execution.entry_stations_mm[i];
            for(const auto& piece:prepared.entry.pieces) {
                const double begin=std::max(a,piece.execution_begin_mm),end=std::min(b,piece.execution_end_mm);
                if(end<=begin || b<=a)continue;
                const Vec3d delta=execution.entry_targets[i]-execution.entry_targets[i-1];
                add({execution.entry_targets[i-1]+delta*((begin-a)/(b-a)),
                    execution.entry_targets[i-1]+delta*((end-a)/(b-a))},piece.display_purpose);
            }
        }
    }
    size_t cursor=0;
    for(size_t si=0;si<execution.spans().size();++si) {
        const auto& span=execution.spans()[si];
        if(span.kind==FiberMotionKind::NonDepositingFinish) {flush();continue;}
        Vec3d from=execution.machine_target(span.from);
        for(const auto& command:span.commands) {
            while(cursor<prepared.command_pieces.size() &&
                (prepared.command_pieces[cursor].span_index<si ||
                    (prepared.command_pieces[cursor].span_index==si && prepared.command_pieces[cursor].end_edge<=command.source_primitive)))++cursor;
            if(cursor==prepared.command_pieces.size() || prepared.command_pieces[cursor].span_index!=si)
                throw FiberExecutionError("Frozen fiber contour command has no source piece");
            const auto& piece=prepared.command_pieces[cursor];
            const Vec3d to=execution.machine_target(command.target);
            const auto* arc=command.arc_index<0?nullptr:&execution.arcs().at(command.arc_index);
            add(command_points(from,to,arc?(arc->ccw?EMovePathType::Arc_move_ccw:EMovePathType::Arc_move_cw):
                EMovePathType::Linear_move,arc?arc->ij:Vec2d::Zero()),piece.origin.display_purpose);
            from=to;
        }
    }
    flush();return runs;
}

Polylines fiber_geometry_contour_runs(const FiberPathDraft& prepared)
{
    Polylines runs;Polyline current;
    const auto flush=[&] {if(current.points.size()>1)runs.push_back(std::move(current));current.points.clear();};
    const auto add=[&](const Polyline3& route,FiberPathPurpose purpose) {
        if(purpose!=FiberPathPurpose::Contour){flush();return;}
        const auto points=Geometry::sample_with_arcs(route,0.00005).to_polyline().points;
        if(points.empty())return;
        if(!current.points.empty() && current.points.back()!=points.front())flush();
        current.points.insert(current.points.end(),points.begin()+(current.points.empty()?0:1),points.end());
    };
    if(prepared.command_pieces.empty()) {
        if(prepared.id.parent.purpose!=FiberPathPurpose::Contour)return {};
        if(prepared.entry.lowers())add(prepared.entry.geometry,FiberPathPurpose::Contour);
        for(const auto& span:prepared.spans)if(span.deposits_fiber())add(span.geometry,FiberPathPurpose::Contour);
    } else {
        if(prepared.entry.lowers())for(const auto& piece:prepared.entry.pieces)
            add(Geometry::slice_with_arcs_mm(prepared.entry.geometry,piece.execution_begin_mm,piece.execution_end_mm),piece.display_purpose);
        for(const auto& piece:prepared.command_pieces) {
            const auto& span=prepared.spans.at(piece.span_index);
            if(!span.deposits_fiber()){flush();continue;}
            const auto stations=Geometry::stations_with_arcs_mm(span.geometry);
            add(Geometry::slice_with_arcs_mm(span.geometry,stations.at(piece.begin_edge),stations.at(piece.end_edge)),piece.origin.display_purpose);
        }
    }
    flush();return runs;
}

Vec3d FrozenFiberExecution::machine_target(const Vec2d& relative) const
{
    const Vec2d absolute=relative+command_origin;
    return Vec3d(GCodeFormatter::quantize(absolute.x(),6),GCodeFormatter::quantize(absolute.y(),6),layer_z);
}

bool equal_fiber_commands(const FrozenFiberCommands& a,const FrozenFiberCommands& b)
{
    if(a.spans.size()!=b.spans.size() || a.arcs.size()!=b.arcs.size())return false;
    for(size_t i=0;i<a.arcs.size();++i)if(a.arcs[i].ij!=b.arcs[i].ij || a.arcs[i].ccw!=b.arcs[i].ccw)return false;
    for(size_t i=0;i<a.spans.size();++i) {
        const auto& x=a.spans[i];const auto& y=b.spans[i];
        if(x.from!=y.from || x.kind!=y.kind || x.commands.size()!=y.commands.size())return false;
        for(size_t j=0;j<x.commands.size();++j) {
            const auto& p=x.commands[j];const auto& q=y.commands[j];
            if(p.target!=q.target || p.dE!=q.dE || p.speed_mm_s!=q.speed_mm_s || p.station_mm!=q.station_mm ||
                p.source_primitive!=q.source_primitive || p.arc_index!=q.arc_index)return false;
        }
    }
    return true;
}


const FrozenFiberExecution& CompiledFiberPath::execution(size_t instance_id) const
{
    for(const auto& variant:variants)if(variant->placement.instance_id==instance_id)return *variant;
    throw FiberExecutionError("Certified fiber placement is missing");
}

namespace {
// A stroke is the union of finite butt rectangles, analytic annular sectors
// and the OUTER round-join sectors. Full vertex discs would cross short butt
// caps. Bounds are taken per component before union; erosion of the union is
// not an inner bound for independently perturbed components.
struct SweepPrimitive {
    Vec2d a,b,center;
    double angle{0};
    bool arc{false};
    Point grid_a,grid_b;
    double grid_z{0};
    bool grid_known{false};
    size_t group{0};
    double calculation_scale{1};
};
using CoverageBounds=std::pair<ExPolygons,ExPolygons>; // inner, outer
const double coverage_grid_mm=SCALING_FACTOR;

Point rounded_machine_grid(const Vec2d& p)
{
    return Point(coord_t(std::llround(p.x()/SCALING_FACTOR)),coord_t(std::llround(p.y()/SCALING_FACTOR)));
}

Points coverage_convex_hull(Points points)
{
    using Wide=boost::multiprecision::int128_t;
    std::sort(points.begin(),points.end(),[](const Point& a,const Point& b){return a.x()<b.x() || (a.x()==b.x() && a.y()<b.y());});
    points.erase(std::unique(points.begin(),points.end()),points.end());
    if(points.size()<3)return points;
    const auto turn=[](const Point& a,const Point& b,const Point& c)->Wide {
        return Wide(b.x()-a.x())*(c.y()-a.y())-Wide(b.y()-a.y())*(c.x()-a.x());
    };
    Points hull;hull.reserve(points.size()*2);
    for(const auto& point:points) {
        while(hull.size()>1 && turn(hull[hull.size()-2],hull.back(),point)<=0)hull.pop_back();
        hull.push_back(point);
    }
    const size_t lower=hull.size();
    for(size_t i=points.size()-1;i-->0;) {
        while(hull.size()>lower && turn(hull[hull.size()-2],hull.back(),points[i])<=0)hull.pop_back();
        hull.push_back(points[i]);
    }
    hull.pop_back();return hull;
}

CoverageBounds convex_component(const std::vector<Vec2d>& vertices,double inset)
{
    Points snapped;snapped.reserve(vertices.size());
    for(const auto& p:vertices)snapped.push_back(Point::new_scale(p.x(),p.y()));
    const auto hull=coverage_convex_hull(std::move(snapped));
    if(hull.empty())return {};
    const coord_t margin=coord_t(std::ceil(scale_(inset)))+64;
    // Exact integer Minkowski sum with an axis-aligned square. Its support
    // contains the continuous convex component after snapping, without any
    // per-component Clipper offset/cleanup or acute-miter amplification.
    Points expanded;expanded.reserve(hull.size()*4);
    for(const auto& p:hull)for(coord_t x:{-margin,margin})for(coord_t y:{-margin,margin})
        expanded.emplace_back(p.x()+x,p.y()+y);
    CoverageBounds result;
    result.second.emplace_back(Polygon(coverage_convex_hull(std::move(expanded))));

    // Intersect the inward supporting half-planes directly. Unlike a global
    // homothetic shrink, this preserves the width of long, short rectangles.
    struct LPoint {long double x,y;};
    if(hull.size()<3)return result;
    std::vector<LPoint> inside;inside.reserve(hull.size());
    for(const auto& p:hull)inside.push_back({static_cast<long double>(p.x()),static_cast<long double>(p.y())});
    for(size_t edge=0;edge<hull.size() && !inside.empty();++edge) {
        const auto& a=hull[edge];const auto& b=hull[(edge+1)%hull.size()];
        const long double dx=b.x()-a.x(),dy=b.y()-a.y(),length=std::hypot(dx,dy);
        const auto distance=[&](const LPoint& p){return (-dy*(p.x-a.x())+dx*(p.y-a.y()))/length-margin;};
        std::vector<LPoint> clipped;clipped.reserve(inside.size()+1);
        for(size_t i=0;i<inside.size();++i) {
            const auto& p=inside[i];const auto& q=inside[(i+1)%inside.size()];
            const long double dp=distance(p),dq=distance(q);
            if(dp>=0)clipped.push_back(p);
            if((dp>=0)!=(dq>=0)) {
                const long double t=dp/(dp-dq);clipped.push_back({p.x+t*(q.x-p.x),p.y+t*(q.y-p.y)});
            }
        }
        inside=std::move(clipped);
    }
    Points inner;inner.reserve(inside.size());
    for(const auto& p:inside)inner.emplace_back(coord_t(std::llround(p.x)),coord_t(std::llround(p.y)));
    inner=coverage_convex_hull(std::move(inner));
    if(inner.size()>2) {
        // Verify the rounded result against every inward half-plane using
        // integer squared distances. Approximate clipping cannot silently
        // publish an outward-rounded inner vertex.
        using Check=boost::multiprecision::int256_t;
        const Check retained=margin-2;
        for(const auto& point:inner)for(size_t edge=0;edge<hull.size();++edge) {
            const auto& a=hull[edge];const auto& b=hull[(edge+1)%hull.size()];
            const Check dx=b.x()-a.x(),dy=b.y()-a.y();
            const Check cross=dx*(point.y()-a.y())-dy*(point.x()-a.x());
            if(cross<0 || cross*cross<retained*retained*(dx*dx+dy*dy))
                throw FiberExecutionError("Rounded fiber inner bound failed its supporting half-plane check",FiberExecutionFailure::Coverage);
        }
        result.first.emplace_back(Polygon(std::move(inner)));
    }
    return result;
}

CoverageBounds circular_sector(const Vec2d& center,double radius,double start,double sweep,double error)
{
    if(radius<=0 || sweep==0)return {};
    if(std::abs(sweep)>PI/2+1e-10)
        throw FiberExecutionError("Coverage sector was not subdivided",FiberExecutionFailure::InvalidPlan);
    const double step=2*std::acos(std::max(-1.,1-std::min(error/8,radius)/radius));
    const size_t count=std::max(size_t(1),size_t(std::ceil(std::abs(sweep)/step)));
    if(count>1000000)throw FiberExecutionError("Fiber coverage sampling budget exceeded",FiberExecutionFailure::Coverage);
    const double h=sweep/count;
    std::vector<Vec2d> inner{center},outer{center};
    // Chords of inner radius lie inside the circular sector. Circumscribed
    // outer chords have support >= radius; radial bias pays for grid error.
    for(size_t i=0;i<=count;++i) {
        const double angle=start+h*i;const Vec2d direction(std::cos(angle),std::sin(angle));
        inner.push_back(center+direction*std::max(0.,radius-error/4));
        outer.push_back(center+direction*((radius+error/4)/std::cos(std::abs(h)/2)));
    }
    return {convex_component(inner,error/8).first,convex_component(outer,error/8).second};
}

std::vector<SweepPrimitive> merge_collinear(const std::vector<SweepPrimitive>& input)
{
    using Wide=boost::multiprecision::int128_t;
    std::vector<SweepPrimitive> result;result.reserve(input.size());
    for(const auto& p:input) {
        if(!result.empty()) {
            auto& before=result.back();
            if(!before.arc && !p.arc && before.grid_known && p.grid_known && before.grid_z==p.grid_z &&
                before.grid_b==p.grid_a && before.group==p.group) {
                const Point a=before.grid_b-before.grid_a,b=p.grid_b-p.grid_a;
                if(Wide(a.x())*b.y()==Wide(a.y())*b.x() && Wide(a.x())*b.x()+Wide(a.y())*b.y()>0) {
                    before.b=p.b;before.grid_b=p.grid_b;
                    before.calculation_scale=std::max(before.calculation_scale,p.calculation_scale);continue;
                }
            }
        }
        result.push_back(p);
    }
    return result;
}

ExPolygons union_coverage_components(ExPolygons components)
{
    // Intermediate levels need only oriented paths. union_ex would run a
    // second union solely to rebuild a PolyTree which the next level flattens.
    // Preserve NonZero winding (including holes) and build that hierarchy once
    // at the root. At <=19 levels the longest chain is <=20 Clipper executions.
    std::vector<Polygons> level;
    constexpr size_t batch=32;
    for(size_t i=0;i<components.size();i+=batch) {
        ExPolygons part;
        for(size_t j=i;j<std::min(i+batch,components.size());++j)part.push_back(std::move(components[j]));
        level.push_back(union_(part));
    }
    if(level.empty())return {};
    unsigned depth=1;
    while(level.size()>2) {
        if(++depth>=19)throw FiberExecutionError("Fiber coverage union budget exceeded",FiberExecutionFailure::Coverage);
        std::vector<Polygons> next;
        for(size_t i=0;i<level.size();i+=2) {
            if(i+1==level.size())next.push_back(std::move(level[i]));
            else {append(level[i],std::move(level[i+1]));next.push_back(union_(level[i]));}
        }
        level=std::move(next);
    }
    if(level.size()==2)append(level[0],std::move(level[1]));
    return union_ex(level.front());
}

CoverageBounds sweep_bounds(const std::vector<SweepPrimitive>& input,double radius,double error,bool deterministic_linear_joins=false)
{
    const auto primitives=merge_collinear(input);
    double magnitude=std::max(1.,radius),minimum_length=std::numeric_limits<double>::max();
    for(const auto& p:primitives) {
        magnitude=std::max({magnitude,p.calculation_scale,p.a.cwiseAbs().maxCoeff(),p.b.cwiseAbs().maxCoeff()});
        if(p.arc)magnitude=std::max(magnitude,p.center.cwiseAbs().maxCoeff());
        minimum_length=std::min(minimum_length,(p.b-p.a).norm());
    }
    if(minimum_length<=0 || 32*std::numeric_limits<double>::epsilon()*magnitude*(1+radius/minimum_length)>4*SCALING_FACTOR)
        throw FiberExecutionError("Fiber coverage floating arithmetic exceeds its grid budget",FiberExecutionFailure::Coverage);
    CoverageBounds result;
    result.first.reserve(primitives.size()*2);result.second.reserve(primitives.size()*2);
    const auto add=[&](CoverageBounds component) {
        // Slic3r::append reserves exact size; repeated use here reallocates and
        // copies every earlier ExPolygon. Keep amortized vector growth instead.
        for(auto& polygon:component.first)result.first.push_back(std::move(polygon));
        for(auto& polygon:component.second)result.second.push_back(std::move(polygon));
    };
    for(size_t i=0;i<primitives.size();++i) {
        const auto& p=primitives[i];
        if(!p.arc) {
            const Vec2d d=p.b-p.a;if(d.norm()==0)continue;
            const Vec2d n(-d.y()/d.norm(),d.x()/d.norm());
            add(convex_component({p.a+n*radius,p.b+n*radius,p.b-n*radius,p.a-n*radius},error/4));
        } else {
            const double ra=(p.a-p.center).norm(),rb=(p.b-p.center).norm();
            // The protocol accepts a rounded endpoint radius. Bound its entire
            // radial interval rather than pretending it is an exact circle.
            const double lo=std::min(ra,rb),hi=std::max(ra,rb);

            const size_t count=std::max(size_t(1),size_t(std::ceil(std::abs(p.angle)/(PI/2))));
            const double start=std::atan2(p.a.y()-p.center.y(),p.a.x()-p.center.x());
            for(size_t k=0;k<count;++k) {
                const double angle=start+p.angle*k/count,sweep=p.angle/count;
                const auto outer_lo=circular_sector(p.center,lo+radius,angle,sweep,error);
                const auto outer_hi=hi==lo?outer_lo:circular_sector(p.center,hi+radius,angle,sweep,error);
                const auto inner_lo=circular_sector(p.center,std::max(0.,lo-radius),angle,sweep,error);
                const auto inner_hi=hi==lo?inner_lo:circular_sector(p.center,std::max(0.,hi-radius),angle,sweep,error);
                add({diff_ex(outer_lo.first,inner_hi.second),diff_ex(outer_hi.second,inner_lo.first)});
                // Signed normal radius crosses zero: negative radii map to
                // a second sector rotated by pi, not an empty inner hole.
                if(lo<radius) {
                    const auto negative_lo=circular_sector(p.center,std::max(0.,radius-hi),angle+PI,sweep,error);
                    const auto negative_hi=hi==lo?negative_lo:circular_sector(p.center,radius-lo,angle+PI,sweep,error);
                    add({negative_lo.first,negative_hi.second});
                }
            }
        }
        const size_t next=i+1<primitives.size()?i+1:0;
        if(next==0 && (primitives.empty() || (p.b-primitives.front().a).norm()>1e-10))continue;
        const auto& q=primitives[next];
        if((p.b-q.a).norm()>1e-10)continue;
        const auto tangent=[](const SweepPrimitive& v,bool end)->Vec2d {
            if(!v.arc)return (v.b-v.a).normalized();
            const Vec2d r=(end?v.b:v.a)-v.center;
            const double sign=v.angle>0?1:-1;return Vec2d(-r.y()*sign,r.x()*sign).normalized();
        };
        const Vec2d before=tangent(p,true),after=tangent(q,false);
        const double turn=std::atan2(before.x()*after.y()-before.y()*after.x(),before.dot(after));
        if(std::abs(turn)>PI-1e-8)throw FiberExecutionError("Reversing fiber join is uncertifiable",FiberExecutionFailure::Coverage);
        if(deterministic_linear_joins && !p.arc && !q.arc && p.b==q.a) {
            // The actual commands are fixed, so their shared join has no
            // independent component perturbation. A disk clipped by both far
            // butt planes is inside the two finite rectangles + outer round
            // join: each disk point lies before the incoming endpoint, after
            // the outgoing start, or in exactly that outer join wedge. Unlike
            // a full vertex disk, the clipped patch cannot cross a short cap.
            const double step=2*std::acos(std::max(-1.,1-std::min(error/8,radius)/radius));
            const size_t count=std::max(size_t(8),size_t(std::ceil(2*PI/step)));
            if(count>1000000)throw FiberExecutionError("Fiber join certification budget exceeded",FiberExecutionFailure::Coverage);
            std::vector<Vec2d> patch;patch.reserve(count);
            for(size_t k=0;k<count;++k) {
                const double angle=2*PI*k/count;
                patch.emplace_back(std::cos(angle)*std::max(0.,radius-error/4),std::sin(angle)*std::max(0.,radius-error/4));
            }
            const auto clip=[&](const Vec2d& normal,double bound) {
                std::vector<Vec2d> clipped;clipped.reserve(patch.size());
                for(size_t k=0;k<patch.size();++k) {
                    const Vec2d& a=patch[k];const Vec2d& b=patch[(k+1)%patch.size()];
                    const double da=normal.dot(a)-bound,db=normal.dot(b)-bound;
                    if(da>=0)clipped.push_back(a);
                    if((da>=0)!=(db>=0))clipped.push_back(a+(b-a)*(da/(da-db)));
                }
                patch=std::move(clipped);
            };
            clip(before,-(p.b-p.a).norm());clip(-after,-(q.b-q.a).norm());
            for(auto& point:patch)point+=p.b;
            if(patch.size()>=3) {
                auto inner=convex_component(patch,error/8).first;
                for(auto& polygon:inner)result.first.push_back(std::move(polygon));
            }
        }
        if(turn==0)continue;
        const Vec2d normal=turn>0?Vec2d(before.y(),-before.x()):Vec2d(-before.y(),before.x());
        const double start=std::atan2(normal.y(),normal.x());
        const size_t count=std::max(size_t(1),size_t(std::ceil(std::abs(turn)/(PI/2))));
        for(size_t k=0;k<count;++k)add(circular_sector(p.b,radius,start+turn*k/count,turn/count,error));
    }
    // Keep G as a set of components. Its consumers already perform a NonZero
    // intersection/difference, which materializes the union in the same pass.
    result.second=union_coverage_components(std::move(result.second));return result;
}

std::vector<SweepPrimitive> geometry_primitives(const FiberPathDraft& draft)
{
    std::vector<SweepPrimitive> result;
    const bool native=draft.geometry_context && draft.geometry_context->export_arcs;
    if(!native && draft.reference_deposition) {
        const auto& points=draft.reference_deposition->points;
        size_t group_index=0;
        for(size_t i=1;i<points.size();++i) {
            while(group_index<draft.reference_groups.size() && i>draft.reference_groups[group_index].first)++group_index;
            SweepPrimitive p;
            if(group_index<draft.reference_groups.size())p.group=draft.reference_groups[group_index].second;p.a=points[i-1].cast<double>()*SCALING_FACTOR;p.b=points[i].cast<double>()*SCALING_FACTOR;
            p.grid_a=points[i-1];p.grid_b=points[i];p.grid_known=true;result.push_back(p);
        }
        return result;
    }
    const auto add=[&](const Polyline3& line,size_t group) {
        for(size_t i=1;i<line.points.size();++i) {
            SweepPrimitive p;p.group=group;p.a=line.points[i-1].head<2>().cast<double>()*SCALING_FACTOR;
            p.b=line.points[i].head<2>().cast<double>()*SCALING_FACTOR;
            if(p.a==p.b)continue;
            p.grid_a=line.points[i-1].to_point();p.grid_b=line.points[i].to_point();p.grid_known=true;
            if(!line.fitting_result.empty()) {
                const auto& fit=line.fitting_result.at(i-1);p.arc=is_arc(fit.path_type);
                if(p.arc) {
                    p.center=fit.arc_data.center.cast<double>()*SCALING_FACTOR;
                    const Vec2d a=p.a-p.center,b=p.b-p.center;
                    p.angle=std::atan2(a.x()*b.y()-a.y()*b.x(),a.dot(b));
                    if(fit.path_type==EMovePathType::Arc_move_ccw && p.angle<=0)p.angle+=2*PI;
                    if(fit.path_type==EMovePathType::Arc_move_cw && p.angle>=0)p.angle-=2*PI;
                }
            }
            result.push_back(p);
        }
    };
    if(draft.entry.lowers())add(Geometry::sample_with_arcs(draft.entry.geometry,.00005),0);
    for(size_t i=0;i<draft.spans.size();++i)if(draft.spans[i].deposits_fiber())add(draft.spans[i].geometry,i+1);
    return result;
}

std::vector<SweepPrimitive> execution_primitives(const FrozenFiberExecution& execution,const FiberExecutionContext& context)
{
    std::vector<SweepPrimitive> result;
    const auto local=[&](const Vec3d& p)->Vec2d {
        return context.transform.oriented(p).head<2>()+execution.nozzle_offset-execution.placement.origin;
    };
    size_t group=0;
    const auto add=[&](const Vec3d& a,const Vec3d& b,const FrozenFiberArc* arc) {
        SweepPrimitive p;p.group=group;p.a=local(a);p.b=local(b);if(p.a==p.b)return;
        p.calculation_scale=std::max({1.,a.cwiseAbs().maxCoeff(),b.cwiseAbs().maxCoeff(),
            execution.nozzle_offset.cwiseAbs().maxCoeff(),execution.placement.origin.cwiseAbs().maxCoeff(),
            context.transform.xy_offset.cwiseAbs().maxCoeff()});
        if(context.transform.belt)p.calculation_scale=std::max(p.calculation_scale,
            std::abs(context.transform.belt->print_origin_s()*context.transform.belt->cot_angle()));
        p.grid_a=rounded_machine_grid(a.head<2>());p.grid_b=rounded_machine_grid(b.head<2>());
        p.grid_known=!context.transform.belt || a.z()==b.z();p.grid_z=context.transform.belt?a.z():0;
        if(arc) {
            p.arc=true;p.center=local(Vec3d(a.x()+arc->ij.x(),a.y()+arc->ij.y(),a.z()));
            const Vec2d u=p.a-p.center,v=p.b-p.center;
            p.angle=std::atan2(u.x()*v.y()-u.y()*v.x(),u.dot(v));
            const Vec2d x=local(a+Vec3d(1,0,0))-local(a),y=local(a+Vec3d(0,1,0))-local(a);
            const bool ccw=arc->ccw!=(x.x()*y.y()-x.y()*y.x()<0);
            if(ccw && p.angle<=0)p.angle+=2*PI;if(!ccw && p.angle>=0)p.angle-=2*PI;
        }
        result.push_back(p);
    };
    for(size_t i=1;i<execution.entry_targets.size();++i)add(execution.entry_targets[i-1],execution.entry_targets[i],nullptr);
    for(size_t i=0;i<execution.spans().size();++i) {
        const auto& span=execution.spans()[i];group=i+1;
        if(span.kind==FiberMotionKind::NonDepositingFinish)continue;
        Vec3d from=execution.machine_target(span.from);
        for(const auto& command:span.commands) {
            const Vec3d to=execution.machine_target(command.target);
            add(from,to,command.arc_index<0?nullptr:&execution.arcs().at(command.arc_index));from=to;
        }
    }
    return result;
}

double coverage_area(const ExPolygons& polygons)
{
    double area=0;for(const auto& p:polygons)area+=p.area();return area*SCALING_FACTOR*SCALING_FACTOR;
}
bool inside_material_domain(const PreparedFiberPath& path,const ExPolygons& outer)
{
    if(coverage_area(diff_ex(outer,path.allowed_domain,ApplySafetyOffset::Yes))>path.outside_tolerance_mm2)return false;
    if(path.policy_boundary && !path.policy_boundary->forbidden_core.empty() &&
        !intersection_ex(outer,offset_ex(path.policy_boundary->forbidden_core,-4.f),ApplySafetyOffset::No).empty())return false;
    return true;
}
}

// Memoization is scoped to a Print, shared across its canonical objects. The
// cache owns only successful pure reference results, never a material domain,
// execution, candidate tool, placement or publication permission.
class FiberGeometryCoverageCache {
    struct Entry {
        uint64_t hash;
        std::vector<uint64_t> key;
        std::shared_ptr<const CoverageBounds> bounds;
        size_t bytes;
    };
    std::mutex mutex;
    std::list<Entry> entries;
    size_t retained_bytes{0},hits{0},misses{0};
    static constexpr size_t maximum_bytes=32*1024*1024,maximum_item_bytes=4*1024*1024,maximum_entries=32;
    static size_t polygon_bytes(const ExPolygons& polygons) {
        size_t bytes=polygons.capacity()*sizeof(ExPolygon);
        for(const auto& polygon:polygons) {
            bytes+=polygon.contour.points.capacity()*sizeof(Point)+polygon.holes.capacity()*sizeof(Polygon);
            for(const auto& hole:polygon.holes)bytes+=hole.points.capacity()*sizeof(Point);
        }
        return bytes;
    }
public:
    std::shared_ptr<const CoverageBounds> find(uint64_t hash,const std::vector<uint64_t>& key) {
        std::lock_guard<std::mutex> lock(mutex);
        for(auto it=entries.begin();it!=entries.end();++it)if(it->hash==hash && it->key==key) {
            ++hits;auto result=it->bounds;entries.splice(entries.begin(),entries,it);return result;
        }
        ++misses;return {};
    }
    void insert(uint64_t hash,std::vector<uint64_t> key,const std::shared_ptr<const CoverageBounds>& bounds) {
        const size_t bytes=sizeof(Entry)+2*sizeof(void*)+64+sizeof(CoverageBounds)+key.capacity()*sizeof(uint64_t)+
            polygon_bytes(bounds->first)+polygon_bytes(bounds->second);
        if(bytes>maximum_item_bytes)return;
        std::lock_guard<std::mutex> lock(mutex);
        // Concurrent misses may calculate the same key outside this lock.
        for(auto it=entries.begin();it!=entries.end();++it)if(it->hash==hash && it->key==key)return;
        while(!entries.empty() && (entries.size()>=maximum_entries || retained_bytes+bytes>maximum_bytes)) {
            retained_bytes-=entries.back().bytes;entries.pop_back();
        }
        entries.push_front({hash,std::move(key),bounds,bytes});retained_bytes+=bytes;
    }
    FiberGeometryCoverageCacheStatistics statistics() {
        std::lock_guard<std::mutex> lock(mutex);return {hits,misses,entries.size(),retained_bytes};
    }
};

std::shared_ptr<FiberGeometryCoverageCache> make_fiber_geometry_coverage_cache()
{
    return std::make_shared<FiberGeometryCoverageCache>();
}
FiberGeometryCoverageCacheStatistics fiber_geometry_coverage_cache_statistics(const std::shared_ptr<FiberGeometryCoverageCache>& cache)
{
    return cache?cache->statistics():FiberGeometryCoverageCacheStatistics{};
}

std::pair<ExPolygons,ExPolygons> fiber_geometry_coverage_bounds(const FiberPathDraft& draft)
{
    const auto primitives=geometry_primitives(draft);
    const double radius=.5*draft.width_mm,error=draft.execution_deviation_mm*.4;
    const auto cache=draft.geometry_context?draft.geometry_context->coverage_cache:nullptr;
    if(!cache)return sweep_bounds(primitives,radius,error);
    // Explicit initialized words, rather than object bytes: line centers and
    // padding are not initialized. Equality includes every effective input;
    // hash collisions cannot return another geometry. +/-zero are identical.
    std::vector<uint64_t> key;key.reserve(2+16*primitives.size());
    const auto number=[&](double value) {
        if(!std::isfinite(value))throw FiberExecutionError("Nonfinite reference coverage input",FiberExecutionFailure::Coverage);
        uint64_t bits=0;if(value!=0)std::memcpy(&bits,&value,sizeof(bits));key.push_back(bits);
    };
    number(radius);number(error);
    for(const auto& p:primitives) {
        number(p.a.x());number(p.a.y());number(p.b.x());number(p.b.y());key.push_back(p.arc);
        if(p.arc){number(p.center.x());number(p.center.y());number(p.angle);}
        else key.insert(key.end(),3,0);
        key.push_back(p.grid_known);
        if(p.grid_known) {
            key.push_back(uint64_t(p.grid_a.x()));key.push_back(uint64_t(p.grid_a.y()));
            key.push_back(uint64_t(p.grid_b.x()));key.push_back(uint64_t(p.grid_b.y()));
        } else key.insert(key.end(),4,0);
        number(p.grid_z);key.push_back(p.group);number(p.calculation_scale);
    }
    uint64_t hash=UINT64_C(14695981039346656037);
    for(uint64_t word:key){hash^=word;hash*=UINT64_C(1099511628211);}
    if(const auto cached=cache->find(hash,key))return *cached;
    // No locks or mutable Print state are involved in this heavy operation.
    auto bounds=std::make_shared<const CoverageBounds>(sweep_bounds(primitives,radius,error));
    cache->insert(hash,std::move(key),bounds);return *bounds;
}

bool certify_fiber_coverage(const PreparedFiberPath& path,const FrozenFiberExecution& execution,const FiberExecutionContext& context)
{
    // A six-place IJ command whose endpoint is off its circle has no
    // firmware-independent butt tangent. Do not certify an invented spiral.
    // Exact integer squared radii also cover very small sweeps safely.
    using Wide=boost::multiprecision::int128_t;
    for(const auto& span:execution.spans()) {
        Vec3d from=execution.machine_target(span.from);
        for(const auto& command:span.commands) {
            const Vec3d to=execution.machine_target(command.target);
            if(command.arc_index>=0) {
                const auto& arc=execution.arcs().at(command.arc_index);
                const Point ij=rounded_machine_grid(arc.ij);
                const Point delta=rounded_machine_grid(to.head<2>()-from.head<2>());
                const Point end=delta-ij;
                if(Wide(ij.x())*ij.x()+Wide(ij.y())*ij.y()!=Wide(end.x())*end.x()+Wide(end.y())*end.y())
                    throw FiberExecutionError("Rounded native arc has no certified circular endpoint/tangent",FiberExecutionFailure::Quantization);
            }
            from=to;
        }
    }
    const auto actual=execution_primitives(execution,context);
    const auto reference=geometry_primitives(path.draft());
    // Fixed machine-grid differences prove one translation of the entire G1
    // route. This does not infer a normal/cap bound from endpoint proximity.
    bool translated=!context.transform.belt && actual.size()==reference.size();
    std::optional<Point> grid_delta;
    const auto check=[&](const Point& machine,const Point& source) {
        const Point delta=machine-source;if(!grid_delta)grid_delta=delta;return delta==*grid_delta;
    };
    for(size_t i=0;translated && i<actual.size();++i)
        translated=!actual[i].arc && !reference[i].arc && actual[i].grid_known && reference[i].grid_known &&
            check(actual[i].grid_a,reference[i].grid_a) && check(actual[i].grid_b,reference[i].grid_b);
    if(translated && grid_delta) {
        const Vec2d shift=grid_delta->cast<double>()*SCALING_FACTOR+context.transform.xy_offset+
            execution.nozzle_offset-execution.placement.origin;
        // A <= one-grid translation is paid by the directed numerical bounds
        // at every convex component, including the angular caps of joins.
        if(shift.norm()<=SCALING_FACTOR && inside_material_domain(path,path.possible_coverage))return true;
    }
    const bool native_linear_fallback=context.export_arcs && execution.arcs().empty();
    // A native material contract can require a tighter actual stroke proof.
    // Refine its sector approximation/inset, retaining convex_component's
    // fixed 64-grid arithmetic guard and exact supporting-plane verification.
    const auto bounds=sweep_bounds(actual,0.5*path.width_mm,(native_linear_fallback?8:16)*coverage_grid_mm,
        native_linear_fallback);
    const auto outside=diff_ex(bounds.second,path.possible_coverage);
    const auto missing=diff_ex(path.guaranteed_components,bounds.first);
    if(!outside.empty() || !missing.empty() || !inside_material_domain(path,bounds.second)) {
        std::ostringstream message;message<<std::setprecision(17)
            <<"Actual fiber stroke cannot satisfy the frozen material contract: outside_P_mm2="
            <<std::abs(area(outside))*SCALING_FACTOR*SCALING_FACTOR
            <<" missing_G_mm2="<<std::abs(area(missing))*SCALING_FACTOR*SCALING_FACTOR;
        if(!outside.empty()) {
            const auto box=get_extents(outside);message<<" outside_P_bbox_mm="
                <<unscale<double>(box.min.x())<<","<<unscale<double>(box.min.y())<<":"
                <<unscale<double>(box.max.x())<<","<<unscale<double>(box.max.y());
            const auto& point=outside.front().contour.points.front();message<<" outside_P_witness_mm="
                <<unscale<double>(point.x())<<","<<unscale<double>(point.y());
        }
        if(!missing.empty()) {
            const auto& point=missing.front().contour.points.front();message<<" missing_G_witness_mm="
                <<unscale<double>(point.x())<<","<<unscale<double>(point.y());
        }
        throw FiberExecutionError(message.str(),FiberExecutionFailure::Coverage);
    }
    return false;
}

FrozenFiberExecution compile_certified_fiber_execution(const PreparedFiberPath& prepared,
    const FiberExecutionContext& context,const FiberPlacement& placement,double layer_z,
    FiberExecutionStatistics& statistics,const std::function<void()>& cancel)
{
    const auto compile_and_certify=[&](const FiberExecutionContext& compilation_context,double chord_cap) {
        if(cancel)cancel();
        ++statistics.compiled_candidates;
        auto execution=compile_fiber_execution_impl(prepared.draft(),compilation_context,placement,layer_z,cancel,chord_cap);
        if(cancel)cancel();
        // Certification always uses the published context/reference, including
        // its analytic P/G bounds. A fallback cannot change material allocation.
        const bool translated=certify_fiber_coverage(prepared,execution,context);
        if(translated)++statistics.translated_coverage;else ++statistics.swept_coverage;
        return execution;
    };
    try {
        return compile_and_certify(context,std::numeric_limits<double>::infinity());
    } catch(const FiberExecutionError& error) {
        const auto has_arc=[](const Polyline3& geometry) {
            return std::any_of(geometry.fitting_result.begin(),geometry.fitting_result.end(),
                [](const auto& record){return is_arc(record.path_type);});
        };
        const bool analytic=has_arc(prepared.entry.geometry) ||
            std::any_of(prepared.spans.begin(),prepared.spans.end(),[&](const auto& span){return has_arc(span.geometry);});
        if(!context.export_arcs || !analytic || error.failure!=FiberExecutionFailure::Quantization)throw;
    }
    FiberExecutionContext linear_context=context;
    linear_context.export_arcs=false;
    double chord_cap=0.00005;
    for(unsigned attempt=0;attempt<8;++attempt) {
        try {
            return compile_and_certify(linear_context,chord_cap);
        } catch(const FiberExecutionError& error) {
            // Only representation/coverage errors can improve by refining the
            // G1 chords. Every attempt must still satisfy E, machine and tail
            // constraints; exhaustion fails the whole task.
            const bool refinable=error.failure==FiberExecutionFailure::Coverage ||
                error.failure==FiberExecutionFailure::Quantization || error.failure==FiberExecutionFailure::TailBudget;
            if(!refinable || attempt==7)throw;
        }
        chord_cap=std::max(chord_cap*.5,.5*SCALING_FACTOR);
    }
    throw FiberExecutionError("Fiber linear fallback exhausted its certification budget",FiberExecutionFailure::Coverage);
}

} // namespace Slic3r
