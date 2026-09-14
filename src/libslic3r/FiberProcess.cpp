// Legacy formulas originate in bulber 643d319bd140ea54347a23d1b6520e5474b49ed6.
#include "FiberProcess.hpp"
#include "Line.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>
namespace Slic3r {
static Point fiber_projection(const Point& point, const Line& line)
{
    const Vec2d delta = (line.b-line.a).cast<double>();
    if (delta.squaredNorm() == 0) return line.a;
    return Point((line.a.cast<double>() + delta * ((point-line.a).cast<double>().dot(delta)/delta.squaredNorm())).cast<coord_t>());
}
Polylines filter_fiber_legacy(Polyline& input, double minlength, int type, Polylines& otherpath)
{
    std::vector<Polyline> short_result;
    if (input.points.size() < 2) return short_result;

    if(input.is_closed() || minlength <= 0.5)
    {
        otherpath.emplace_back(input);
        return std::move(short_result);
    }

    switch(type)
    {
    case 0://start
        {
            double cutlength=0;
            int num =0;
            for (const auto &line: input.lines())
            {
                if(line.length() >scale_(minlength) )
                    break;
                cutlength += line.length();
                num++;
            }
            if(num>0)
            {
                Polyline firstline;
                for (const auto &line: input.lines())
                {
                    firstline.append(line.a);
                    firstline.append(line.b);
                    num--;
                    if(num<=0)
                        break;
                }
                short_result.emplace_back(firstline);
            }
            if(cutlength>0)
                input.clip_start(cutlength);
            break;
        }
    case 1://middle
        {
            Lines lines = input.lines();
            Line maxline;
            bool firstone= true;
            for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line)
            {
                if(firstone)
                {
                    maxline = (*line);
                    firstone = false;
                }
                else
                {
                    if((*line).length()>maxline.length())
                        maxline = (*line);
                }
            }

            int minlinenum = 0;
            int maxlinenum = 0;
            int totalnum = lines.size();
            for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line)
            {
                if((*line).parallel_to(maxline))
                {
                    if((*line).length()<scale_(minlength))
                    {
                        minlinenum++;
                    }
                    else
                    {
                        maxlinenum++;
                    }
                }
            }

            if(maxlinenum + maxlinenum < totalnum*0.25)
            {
                short_result.emplace_back(input);
                return std::move(short_result);
            }

            int preisshort = -1;
            Polyline onefiberline;
            Polyline oneshortline;

            for (Lines::const_iterator line = lines.begin(); line != lines.end(); ++line)
            {
                if((*line).parallel_to(maxline)|| (*line).length()>=scale_(minlength))
                {
                    if((*line).length()< scale_(minlength))
                    {
                        if(preisshort==0)
                        {
                            if(onefiberline.size()>=2)
                                otherpath.emplace_back(onefiberline);
                            onefiberline.clear();
                        }
                        oneshortline.append((*line).a);
                        oneshortline.append((*line).b);

                        preisshort = 1;
                    }
                    else
                    {
                        if(preisshort==1)
                        {
                            if(oneshortline.size()>=2)
                                short_result.emplace_back(oneshortline);
                            oneshortline.clear();
                        }

                        if(onefiberline.size()>1)
                        {
                            Point preEndP = onefiberline.points[onefiberline.size()-1];
                            Point preEndP1 = onefiberline.points[onefiberline.size()-2];
                            Point MidP = (preEndP+(*line).a)*0.5f;

                            double R = (*line).distance_to_infinite(MidP,(*line).a,(*line).b);
                            if(R<SCALED_EPSILON)
                            {
                                otherpath.emplace_back(onefiberline);
                                onefiberline.clear();

                                onefiberline.append((*line).a);
                                onefiberline.append((*line).b);
                            }
                            else
                            {
                            #if 1 //ENABLE_ANGLE_EXTEND
                                Line preLine = Line(preEndP1,preEndP);
                                double pretheta,nexttheta;
                                Point pre_pp = fiber_projection(MidP, preLine);
                                Point next_pp = fiber_projection(MidP, *line);

                                onefiberline.points[onefiberline.size()-1] = pre_pp;
                                onefiberline.append(next_pp);
                                onefiberline.append((*line).b);
                            #else
                                Point centerP = MidP + (*line).vector()*(R/(*line).length());
                                Line preLine = Line(preEndP1,preEndP);
                                double pretheta,nexttheta;
                                Point pre_pp = centerP.projection_onto(preLine,pretheta);
                                Point next_pp = centerP.projection_onto((*line),nexttheta);

                                onefiberline.points[onefiberline.size()-1] = pre_pp;

                                Point dir = (pre_pp - centerP);
                                auto pdir = preLine.pointLeftLine(MidP);
                                for(int i=1;i<9;i++)
                                {
                                    Point tempP = centerP + (dir*arcratio).rotated(M_PI*(pdir?1.0f:-1.0f)*i/9.0f);
                                    onefiberline.append(tempP);
                                }

                                onefiberline.append(next_pp);
                                onefiberline.append((*line).b);
                            #endif
                            }
                        }
                        else
                        {
                            onefiberline.append((*line).a);
                            onefiberline.append((*line).b);
                        }

                        preisshort = 0;
                    }
                }
                else
                {
                    if(preisshort==1)
                    {
                        oneshortline.append((*line).a);
                        oneshortline.append((*line).b);
                    }
                    else if(preisshort==0)
                    {
                        #if 0
                        onefiberline.append((*line).a);
                        onefiberline.append((*line).b);
                        #else
                        #endif
                    }
                }
            }
            if(onefiberline.size()>=2)
            {
                 otherpath.emplace_back(onefiberline);
                 onefiberline.clear();
            }
            if(oneshortline.size()>=2)
            {
                 short_result.emplace_back(oneshortline);
                 oneshortline.clear();
            }

            //BOOST_LOG_TRIVIAL(error)<<"otherpath.size() "<<otherpath.size();

            break;
        }
    case 2://end
        {
            input.reverse();

            double cutlength=0;
            int num =0;
            for (const auto &line: input.lines())
            {
                if(line.length() >scale_(minlength) )
                    break;
                cutlength += line.length();
                num++;
            }
            if(num>0)
            {
                Polyline firstline;
                for (const auto &line: input.lines())
                {
                    firstline.append(line.a);
                    firstline.append(line.b);
                    num--;
                    if(num<=0)
                        break;
                }
                short_result.emplace_back(firstline);
            }
            if(cutlength>0)
                input.clip_start(cutlength);

            if (input.points.size() >= 2)
                input.reverse();
            break;
        }
    }
    return std::move(short_result);
}
std::string FiberSource::id() const
{
    return std::to_string(object)+":"+std::to_string(region)+":"+std::to_string(layer)+":"+
        std::to_string(surface)+":"+std::to_string(island)+":"+std::to_string(recipe_ordinal)+":"+
        std::to_string(candidate);
}
double FiberConfig::length_budget_mm() const
{
    return region.fibercut_length.value + region.fiber_start_length.value + region.fiber_z_hop.value + region.fiber_tension_length.value;
}
static Vec2d fiber_mm(const Point& p) { return p.cast<double>() * SCALING_FACTOR; }
static Point fiber_scaled(const Vec2d& p) { return Point((p / SCALING_FACTOR).cast<coord_t>()); }
Point FiberCurveSegment::at(double distance_mm) const
{
    if (distance_mm <= 0) return start;
    if (distance_mm >= length_mm) return end;
    const double t = distance_mm / length_mm;
    if (!arc) return fiber_scaled(fiber_mm(start)+(fiber_mm(end)-fiber_mm(start))*t);
    const Vec2d v = fiber_mm(start)-center_mm;
    const double angle = sweep_rad*t;
    return fiber_scaled(center_mm+Vec2d(v.x()*cos(angle)-v.y()*sin(angle),v.x()*sin(angle)+v.y()*cos(angle)));
}
FiberCurveSegment FiberCurveSegment::portion(double begin_mm, double end_mm) const
{
    FiberCurveSegment result=*this;
    result.start=at(begin_mm); result.end=at(end_mm);
    result.length_mm=end_mm-begin_mm;
    result.sweep_rad=sweep_rad*result.length_mm/length_mm;
    return result;
}
void FiberCurveSegment::reverse() { std::swap(start,end); sweep_rad=-sweep_rad; }
static FiberCurveSegment fiber_line(const Point& a,const Point& b)
{
    FiberCurveSegment segment; segment.start=a; segment.end=b;
    segment.length_mm=(fiber_mm(b)-fiber_mm(a)).norm(); return segment;
}
static double fiber_angle(const std::vector<FiberCurveSegment>& s,size_t point_index)
{
    if(point_index==0 || point_index>=s.size()) return M_PI;
    const Vec2d a=fiber_mm(s[point_index-1].end)-fiber_mm(s[point_index-1].start);
    const Vec2d b=fiber_mm(s[point_index].end)-fiber_mm(s[point_index].start);
    if(a.squaredNorm()==0 || b.squaredNorm()==0) return 0;
    return std::abs(std::atan2(a.x()*b.y()-a.y()*b.x(),a.dot(b)));
}
static size_t fiber_clip_end(Polyline& path,double distance)
{
    size_t removed=0;
    while(distance>0 && !path.points.empty()) {
        const Vec2d last=path.last_point().cast<double>();
        path.points.pop_back();++removed;
        if(path.points.empty())return removed;
        const Vec2d delta=path.last_point().cast<double>()-last;
        const double length2=delta.squaredNorm();
        if(length2>distance*distance) {
            path.points.emplace_back((last+delta*(distance/std::sqrt(length2))).cast<coord_t>());
            break;
        }
        distance-=std::sqrt(length2);
    }
    return removed;
}
static size_t fiber_clip_start(Polyline& path,double distance)
{
    path.reverse();
    const size_t removed=fiber_clip_end(path,distance);
    if(path.points.size()>=2)path.reverse();
    return removed;
}
// Atomic corner construction preserves the old three-point circle and cross-short-edge trim.
static std::vector<FiberCurveSegment> fiber_round(const Polyline& input,double width_mm,double trim_mm)
{
    std::vector<FiberCurveSegment> result;
    if(input.points.size()<2) return result;
    Polyline remaining=input, output;
    output.points.push_back(input.points.front());
    for(size_t i=1;i+1<remaining.points.size();++i) {
        const Point prev=remaining.points[i-1], vertex=remaining.points[i];
        Point corner=vertex; const Point next=remaining.points[i+1];
        Vec2d incoming=(prev-vertex).cast<double>(), outgoing=(next-vertex).cast<double>();
        const double denominator=incoming.norm()*outgoing.norm();
        const double inner=denominator>0 ? std::acos(std::clamp(incoming.dot(outgoing)/denominator,-1.,1.)) : M_PI;
        if(inner>M_PI/2 || incoming.norm()<scale_(width_mm) || trim_mm<=0) {
            output.points.push_back(vertex); continue;
        }
        Polyline before=output; before.points.push_back(vertex); fiber_clip_end(before,scale_(trim_mm));
        size_t source=i;
        if(i+2<remaining.points.size()) {
            const Line in(prev,vertex), following(next,remaining.points[i+2]);
            if(in.parallel_to(following) && following.length()<=scale_(width_mm)) {
                source=i+1; corner=Line(vertex,next).midpoint();
            }
        }
        Polyline after; after.points.assign(remaining.points.begin()+source,remaining.points.end());
        const size_t removed=fiber_clip_start(after,scale_(trim_mm));
        if(before.points.empty() || after.points.empty()) { output.points.push_back(vertex); continue; }
        const Point a=before.last_point(), b=after.first_point();
        Vec2d va=(a-corner).cast<double>(), vb=(b-corner).cast<double>();
        if(va.norm()==0 || vb.norm()==0) {output.points.push_back(vertex);continue;}
        Vec2d bisector=va.normalized()+vb.normalized();
        if(bisector.squaredNorm()<EPSILON) {output.points.push_back(vertex);continue;}
        const Point middle=Point((corner.cast<double>()+bisector.normalized()*scale_(trim_mm)*1.3).cast<coord_t>());
        const Vec2d pa=fiber_mm(a), pb=fiber_mm(b), pm=fiber_mm(middle);
        const Vec2d ab=pb-pa, am=pm-pa;
        const double det=2*(ab.x()*am.y()-ab.y()*am.x());
        if(std::abs(det)<1e-12) {output.points.push_back(vertex);continue;}
        const Vec2d center=pa+Vec2d((ab.squaredNorm()*am.y()-am.squaredNorm()*ab.y())/det,
            (ab.x()*am.squaredNorm()-am.x()*ab.squaredNorm())/det);
        const double radius=(pa-center).norm();
        if(!center.allFinite() || !std::isfinite(radius) || radius<=0) {output.points.push_back(vertex);continue;}
        double sweep=std::atan2((pb-center).y(),(pb-center).x())-std::atan2((pa-center).y(),(pa-center).x());
        // A corner fillet must use the local arc between its two trimmed ends.
        // The legacy direction helper was named and assigned in opposite senses;
        // following it literally produces a nearly complete circle at some turns.
        while(sweep>M_PI)sweep-=2*M_PI;
        while(sweep<-M_PI)sweep+=2*M_PI;
        if(std::abs(sweep)<1e-9 || radius>2000.) {output.points.push_back(vertex);continue;}
        // Commit only a complete, finite corner.
        for(size_t n=1;n<before.points.size();++n) if(before.points[n]!=before.points[n-1]) result.push_back(fiber_line(before.points[n-1],before.points[n]));
        FiberCurveSegment arc; arc.start=a;arc.end=b;arc.center_mm=center;arc.arc=true;arc.sweep_rad=sweep;arc.length_mm=std::abs(sweep)*radius;
        result.push_back(arc); output.points={b};
        i=source+removed-1;
    }
    output.points.push_back(input.points.back());
    for(size_t n=1;n<output.points.size();++n) if(output.points[n]!=output.points[n-1])result.push_back(fiber_line(output.points[n-1],output.points[n]));
    return result;
}
static size_t fiber_split(std::vector<FiberCurveSegment>& segments,double at_mm)
{
    double walked=0;
    if(at_mm<=0) return 0;
    for(size_t i=0;i<segments.size();++i) {
        const double end=walked+segments[i].length_mm;
        if(std::abs(at_mm-end)<1e-9) return i+1;
        if(at_mm<end) {
            const auto original=segments[i];const double within=at_mm-walked;
            segments[i]=original.portion(0,within);
            segments.insert(segments.begin()+i+1,original.portion(within,original.length_mm));return i+1;
        }
        walked=end;
    }
    return segments.size();
}
std::shared_ptr<const PreparedFiberPath> prepare_fiber_path(const Polyline& input,const FiberConfig& config,const FiberSource& source,
    FiberPurpose purpose,bool source_is_fiber_perimeter,bool use_arachne,std::string& rejection)
{
    if(input.points.size()<2) {rejection="empty candidate";return {};}
    auto path=std::make_shared<PreparedFiberPath>();path->source=source;path->purpose=purpose;
    path->source_is_fiber_perimeter=source_is_fiber_perimeter;path->perimeter_process=purpose!=FiberPurpose::Infill;path->config=config;
    if(use_arachne && source_is_fiber_perimeter)
        path->segments=fiber_round(input,config.width_mm,config.perimeter_width_mm*config.region.fiber_corner_trim_length.value);
    else for(const Line& line:input.lines()) if(line.a!=line.b)path->segments.push_back(fiber_line(line.a,line.b));
    if(path->perimeter_process) {std::reverse(path->segments.begin(),path->segments.end());for(auto& segment:path->segments)segment.reverse();}
    for(const auto& segment:path->segments)path->length_mm+=segment.length_mm;
    const double hop=config.region.fiber_z_hop.value,cut=config.region.fibercut_length.value,start=config.region.fiber_start_length.value;
    if(!std::isfinite(path->length_mm) || path->length_mm<=hop+cut) {rejection="D2: L <= hop + cut";return {};}
    if(!path->perimeter_process && path->length_mm<=config.length_budget_mm()) {rejection="internal L <= cut + start + hop + tension";return {};}
    fiber_split(path->segments,hop);
    if(start>0) for(size_t i=0;i<path->segments.size();++i)if(path->segments[i].length_mm>start) {
        const auto segment=path->segments[i];path->segments[i]=segment.portion(0,start);
        path->segments.insert(path->segments.begin()+i+1,segment.portion(start,segment.length_mm));++i;
    }
    path->cut_distance_mm=path->length_mm-cut;
    path->cut_index=fiber_split(path->segments,path->cut_distance_mm);
    path->events={{FiberEventKind::Start,0,0},{FiberEventKind::Cut,path->cut_distance_mm,path->cut_index}};
    if(cut>0)path->events.push_back({FiberEventKind::Tail,path->cut_distance_mm,path->cut_index});
    path->entry=path->segments.front().start;path->display.points.push_back(path->entry);
    for(const auto& segment:path->segments) {
        size_t count=1;
        if(segment.arc) {const double radius=segment.length_mm/std::abs(segment.sweep_rad);const double angle=2*std::acos(std::clamp(1-0.01/radius,-1.,1.));count=std::max<size_t>(1,size_t(std::ceil(std::abs(segment.sweep_rad)/angle)));}
        for(size_t n=1;n<=count;++n)path->display.points.push_back(segment.at(segment.length_mm*n/count));
    }
    path->exit=compile_fiber_process(*path).exit;
    return path;
}
FiberProcessPlan compile_fiber_process(const PreparedFiberPath& path)
{
    FiberProcessPlan plan;const auto& cfg=path.config.region;
    const double ratio=path.config.flow_ratio*(path.perimeter_process?cfg.fiber_perimeters_length_ratio.value:cfg.fiber_infill_length_ratio.value);
    auto event=[&](FiberEventKind kind,const Point& point,double distance,size_t index){FiberAction action;action.point=point;action.event=kind;action.event_only=true;action.motion=false;action.distance_mm=distance;action.segment_index=index;plan.actions.push_back(action);};
    auto move=[&](const Point& point,double e,double speed,double z,bool tail,double distance,size_t index){if(!std::isfinite(e)||!std::isfinite(speed)||speed<=0)throw std::runtime_error("Invalid fiber process action");FiberAction action;action.point=point;action.e_mm=e;action.speed_mm_s=speed;action.z_offset_mm=z;action.tail=tail;action.distance_mm=distance;action.segment_index=index;plan.actions.push_back(action);plan.total_e_mm+=e;plan.exit=point;};
    event(FiberEventKind::Start,path.entry,0,0);
    move(path.entry,cfg.fibercut_length.value+cfg.fiber_restart_extra_length.value,cfg.fiber_restart_speed.value,cfg.fiber_z_hop.value,false,0,0);
    double walked=0,pre_extended=0;
    for(size_t i=0;i<path.segments.size();++i) {
        const auto& segment=path.segments[i];const size_t pindex=i+1;
        const bool tail=pindex>path.cut_index;
        double speed=cfg.fiber_normal_max_speed.value-(cfg.fiber_normal_max_speed.value-cfg.fiber_normal_min_speed.value)*fiber_angle(path.segments,i)/M_PI*
            (cfg.fiber_start_length.value>0?std::min(1.,segment.length_mm/cfg.fiber_start_length.value):1.);
        if(pindex==1 || pindex==2)speed=cfg.fiber_start_min_speed.value;
        speed=std::max(speed,cfg.fiber_start_min_limit_speed.value);
        if(tail)speed=cfg.fiber_finish_min_speed.value+double(pindex-path.cut_index-1)/double(path.segments.size()+1-path.cut_index)*(cfg.fiber_finish_max_speed.value-cfg.fiber_finish_min_speed.value);
        const double e=tail || (pindex==1 && cfg.fiber_z_hop.value>0)?0:segment.length_mm*ratio-pre_extended;
        pre_extended=0;
        size_t count=1;
        if(segment.arc){double radius=segment.length_mm/std::abs(segment.sweep_rad);count=std::max<size_t>(1,size_t(std::ceil(std::abs(segment.sweep_rad)/(2*std::acos(std::clamp(1-0.01/radius,-1.,1.))))));}
        for(size_t n=1;n<=count;++n)move(segment.at(segment.length_mm*n/count),e/count,speed,0,tail,walked+segment.length_mm*n/count,pindex);
        walked+=segment.length_mm;
        if(pindex==path.cut_index) {event(FiberEventKind::Cut,segment.end,walked,pindex);if(pindex<path.segments.size())event(FiberEventKind::Tail,segment.end,walked,pindex);}
        const bool cut_complete = pindex >= path.cut_index;
        const double r=path.perimeter_process?0:cfg.fiber_corner_overshoot.value;
        if(r>0 && i+1<path.segments.size() && pindex>1) {
            const double distance=std::min(r*std::min(fiber_angle(path.segments,pindex)/M_PI,1.),segment.length_mm);
            const Vec2d delta=fiber_mm(segment.end)-fiber_mm(segment.start);
            if(delta.norm()>0 && distance>0) {
                const Point extended=fiber_scaled(fiber_mm(segment.end)+delta.normalized()*distance);
                const double extra=distance*cfg.fiber_angle_extend_ratio.value;
                move(extended,cut_complete?0:extra,speed,0,cut_complete,walked,pindex);
                const auto& next=path.segments[i+1];const Vec2d direction=fiber_mm(next.end)-fiber_mm(next.start);
                // Negative signed offset moves the start towards the next endpoint; no positive clamp.
                const double signed_offset=-r*std::min(fiber_angle(path.segments,pindex)/M_PI,1.);
                if(direction.norm()>0)move(fiber_scaled(fiber_mm(next.start)-direction.normalized()*signed_offset),0,speed,0,cut_complete,walked,pindex);
                if(!cut_complete)pre_extended=extra;
            }
        }
    }
    if(path.perimeter_process && cfg.fiber_finish_ironing_distance.value>0) {
        double distance=cfg.fiber_finish_ironing_distance.value;
        // The legacy implementation first returned to the path entry and then
        // followed every point covered by the ironing distance. Follow rounded
        // segments as curves instead of moving directly across an arc chord.
        move(path.entry,0,cfg.fiber_finish_min_speed.value,0,true,walked,path.segments.size());
        for(const auto& segment:path.segments) {
            const double used=std::min(distance,segment.length_mm);
            size_t count=1;
            if(segment.arc && used>0) {
                const double radius=segment.length_mm/std::abs(segment.sweep_rad);
                const double used_sweep=std::abs(segment.sweep_rad)*used/segment.length_mm;
                const double angle=2*std::acos(std::clamp(1-0.01/radius,-1.,1.));
                count=std::max<size_t>(1,size_t(std::ceil(used_sweep/angle)));
            }
            for(size_t n=1;n<=count;++n)
                move(segment.at(used*n/count),0,cfg.fiber_finish_min_speed.value,0,true,walked,path.segments.size());
            distance-=used;if(distance<=0)break;
        }
    }
    event(FiberEventKind::Finish,plan.exit,walked,path.segments.size());return plan;
}
}
