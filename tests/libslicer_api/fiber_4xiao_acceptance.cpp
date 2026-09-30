// Shared 4xiao model regressions, independent of planner pass/fail decisions.
// Reports and G-code remain in the build tree.
#include <libslicer/Library.hpp>
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/ContinuousFiber/ContinuousFiberFillStrategy.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
Json read_json(const fs::path& file)
{
    std::ifstream input(file);
    require(bool(input), "Missing test asset: " + file.string());
    Json value; input >> value; return value;
}
void write_json(const fs::path& file, const Json& value)
{
    std::ofstream output(file);
    require(bool(output), "Cannot write report: " + file.string());
    output << value.dump(2) << '\n';
    require(bool(output), "Failed to write report: " + file.string());
}
std::string fingerprint(const fs::path& file)
{
    std::ifstream input(file, std::ios::binary);
    require(bool(input), "Missing file: " + file.string());
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (char ch; input.get(ch);) hash = (hash ^ static_cast<unsigned char>(ch)) * UINT64_C(1099511628211);
    require(input.eof(), "Cannot read file: " + file.string());
    std::ostringstream text; text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}
struct Point { double x=0, y=0; };
using Path = std::vector<Point>;
double distance(Point a, Point b) { return std::hypot(a.x-b.x,a.y-b.y); }
double length(const Path& p)
{
    double total=0; for (size_t i=1;i<p.size();++i) total+=distance(p[i-1],p[i]); return total;
}
double x_span(const Path& p)
{
    if (p.empty()) return 0;
    const auto extremes=std::minmax_element(p.begin(),p.end(),[](Point a,Point b){return a.x<b.x;});
    return extremes.second->x-extremes.first->x;
}
double point_segment_distance(Point p, Point a, Point b)
{
    const double dx=b.x-a.x,dy=b.y-a.y,n=dx*dx+dy*dy;
    const double t=n==0?0:std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/n,0.,1.);
    return distance(p,{a.x+t*dx,a.y+t*dy});
}
bool covers_vertices(const Path& a, const Path& b, double tolerance)
{
    if (a.empty() || b.size()<2) return false;
    for (Point p:a) {
        bool found=false;
        for (size_t i=1;i<b.size();++i) {
            if (point_segment_distance(p,b[i-1],b[i])<=tolerance) { found=true;break; }
        }
        if (!found) return false;
    }
    return true;
}
bool inside_ring(Point p, const Path& ring)
{
    bool inside=false;
    for(size_t i=1;i<ring.size();++i) {
        const Point a=ring[i-1],b=ring[i];
        if((a.y>p.y)!=(b.y>p.y) && p.x<a.x+(p.y-a.y)*(b.x-a.x)/(b.y-a.y)) inside=!inside;
    }
    return inside;
}
double cross(Point a,Point b,Point c) {return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}
bool simple_ring(const Path& ring)
{
    const size_t edges=ring.size()-1;
    for(size_t i=0;i<edges;++i) {
        const Point a=ring[i],b=ring[i+1],next=ring[(i+2)%edges];
        // An adjacent reversal also overlaps, even though the edges share a vertex.
        if(std::abs(cross(a,b,next))<1e-9 && (b.x-a.x)*(next.x-b.x)+(b.y-a.y)*(next.y-b.y)<0) return false;
        for(size_t j=i+2;j<edges;++j) {
            if(i==0 && j+1==edges) continue;
            const Point c=ring[j],d=ring[j+1];
            if(std::max(a.x,b.x)<std::min(c.x,d.x) || std::max(c.x,d.x)<std::min(a.x,b.x) ||
               std::max(a.y,b.y)<std::min(c.y,d.y) || std::max(c.y,d.y)<std::min(a.y,b.y)) continue;
            const double ab_c=cross(a,b,c),ab_d=cross(a,b,d),cd_a=cross(c,d,a),cd_b=cross(c,d,b);
            if(((ab_c>0 && ab_d<0)||(ab_c<0 && ab_d>0)) && ((cd_a>0 && cd_b<0)||(cd_a<0 && cd_b>0))) return false;
            if(point_segment_distance(a,c,d)<1e-9 || point_segment_distance(b,c,d)<1e-9 ||
               point_segment_distance(c,a,b)<1e-9 || point_segment_distance(d,a,b)<1e-9) return false;
        }
    }
    return true;
}
// Exact segment/rectangle clipping, with consecutive visits kept separate.
// Walk twice so a passage spanning the chosen G-code seam remains continuous.
double continuous_passage_span(const Path& ring,const std::array<double,4>& box)
{
    double best=0,lo=0,hi=0;bool continuing=false;
    for(size_t k=0;k<2*(ring.size()-1);++k) {
        const size_t i=k%(ring.size()-1);
        const Point a=ring[i],b=ring[i+1];
        double enter=0,leave=1;
        const auto clip=[&](double origin,double delta,double lower,double upper) {
            if(delta==0) return origin>=lower && origin<=upper;
            double t0=(lower-origin)/delta,t1=(upper-origin)/delta;
            if(t0>t1)std::swap(t0,t1);
            enter=std::max(enter,t0);leave=std::min(leave,t1);return enter<=leave;
        };
        if(!clip(a.x,b.x-a.x,box[0],box[2]) || !clip(a.y,b.y-a.y,box[1],box[3])) {continuing=false;continue;}
        const double from=a.x+enter*(b.x-a.x),to=a.x+leave*(b.x-a.x);
        if(!continuing || enter>1e-12) {lo=std::min(from,to);hi=std::max(from,to);}
        else {lo=std::min({lo,from,to});hi=std::max({hi,from,to});}
        best=std::max(best,hi-lo);continuing=leave>=1.-1e-12;
    }
    return best;
}
Json check_spatial_contract(const Path& points,const Json& contract)
{
    // Called only on closed paths. Collapse repeated vertices and the accepted
    // closure difference before topology tests; acceptance closure tolerance is separate.
    Path ring;
    for(Point p:points) if(ring.empty() || distance(ring.back(),p)>1e-9)ring.push_back(p);
    ring.back()=ring.front();
    Json result={{"passed",true},{"failures",Json::array()},{"bores",Json::array()},{"passages",Json::array()}};
    const bool simple=ring.size()>=4 && simple_ring(ring);
    result["simple"]=simple;
    if(!simple)result["failures"].push_back("main_outer_self_intersects");
    for(const auto& disk:contract.at("enclosed_disks")) {
        const Point center{disk.at("center")[0],disk.at("center")[1]};
        double clearance=std::numeric_limits<double>::max();
        for(size_t i=1;i<ring.size();++i)clearance=std::min(clearance,point_segment_distance(center,ring[i-1],ring[i]));
        const bool enclosed=simple && inside_ring(center,ring) && clearance>=disk.at("radius_mm").get<double>();
        result["bores"].push_back({{"name",disk.at("name")},{"enclosed",enclosed},{"boundary_distance_mm",clearance}});
        if(!enclosed)result["failures"].push_back(disk.at("name").get<std::string>()+"_not_enclosed");
    }
    for(const auto& passage:contract.at("passages")) {
        const auto box=passage.at("bounds_xyxy").get<std::array<double,4>>();
        const double span=continuous_passage_span(ring,box),required=box[2]-box[0];
        const bool traversed=span>=required-1e-9;
        result["passages"].push_back({{"name",passage.at("name")},{"traversed",traversed},
            {"required_x_span_mm",required},{"continuous_x_span_mm",span},{"missing_x_span_mm",std::max(0.,required-span)}});
        if(!traversed)result["failures"].push_back(passage.at("name").get<std::string>()+"_not_continuous");
    }
    result["passed"]=result["failures"].empty();return result;
}
struct Block { int layer=0, extruder=0; size_t line=0; Path points; bool contour=true; size_t component=0; };
struct Candidate { int layer=0; bool outer=false; Path points; };

// Read actual depositing G0/G1 moves. Finishing travel MUST NOT close a gap.
// This fixed case uses linear moves; unsupported motions fail instead of being ignored.
std::vector<Block> read_blocks(std::istream& input, const std::vector<Point>& offsets, bool include_infill=false)
{
    std::vector<Block> blocks;
    Block block;
    Point xy;
    bool absolute=true, active=false, contour=false, depositing=false;
    const std::regex field(R"((\w+)=([^ ]+))");
    size_t number=0;
    for (std::string line;std::getline(input,line);) {
        ++number;
        if (line.rfind(";FIBER_BEGIN ",0)==0) {
            require(!active,"Nested fiber block at line " + std::to_string(number));
            std::map<std::string,std::string> fields;
            for (std::sregex_iterator i(line.begin(),line.end(),field),end;i!=end;++i)
                fields[(*i)[1]]=(*i)[2];
            require(fields.count("layer") && fields.count("extruder") && fields.count("purpose"),"Incomplete fiber metadata");
            block={std::stoi(fields.at("layer"))+1,std::stoi(fields.at("extruder")),number,{}};
            require(block.extruder>=0 && size_t(block.extruder)<offsets.size(),"Invalid fiber extruder");
            active=true;contour=fields.at("purpose")=="contour";depositing=false;
            if(include_infill)require(fields.count("component"),"Missing fiber component metadata");
            block.contour=contour;block.component=fields.count("component")?std::stoul(fields.at("component")):0;
        } else if (line==";FIBER_LANDING_BEGIN" || line==";FIBER_START" || line==";FIBER_TAIL_BEGIN") {
            require(active,"Fiber phase outside a block");
            depositing=true;
            if ((contour || include_infill) && block.points.empty()) block.points.push_back({xy.x+offsets[block.extruder].x,xy.y+offsets[block.extruder].y});
        } else if (line==";FIBER_DEPLETED" || line==";FIBER_FINISH_BEGIN") {
            depositing=false;
        } else if (line==";FIBER_END") {
            require(active,"Fiber end outside a block");
            if (contour || include_infill) { require(block.points.size()>1,"Empty depositing block");blocks.push_back(block); }
            active=false;depositing=false;
        }
        std::istringstream command(line.substr(0,line.find(';')));
        std::string opcode; command>>opcode;
        if (opcode=="G90") absolute=true;
        if (opcode=="G91") absolute=false;
        require(!(depositing && (opcode=="G2" || opcode=="G3")),"Unexpected arc in fixed linear G-code case");
        if (opcode!="G0" && opcode!="G1" && opcode!="G92") continue;
        Point next=xy;
        for (std::string word;command>>word;) {
            if (word.size()<2 || (word[0]!='X' && word[0]!='Y')) continue;
            const double value=std::stod(word.substr(1));require(std::isfinite(value),"Nonfinite G-code coordinate");
            double& axis=word[0]=='X' ? next.x : next.y;
            axis=absolute || opcode=="G92" ? value : axis+value;
        }
        if (opcode!="G92" && active && (contour || include_infill) && depositing && distance(next,xy)>0)
            block.points.push_back({next.x+offsets[block.extruder].x,next.y+offsets[block.extruder].y});
        xy=next;
    }
    require(!active,"Unterminated fiber block");
    return blocks;
}
Json evaluate(const Json& rule, const std::vector<Block>& blocks, const std::vector<Candidate>& candidates)
{
    Json report={{"passed",true},{"layers",Json::array()}};
    const int first=rule.at("first_required_layer"),last=rule.at("last_required_layer");
    const double tolerance=rule.at("candidate_match_tolerance_mm");
    for (int layer=1;layer<=rule.at("layer_count").get<int>();++layer) {
        Json row={{"layer",layer},{"required",layer>=first && layer<=last},{"outer_count",0},
                  {"main_outer_count",0},{"paths",Json::array()},{"failures",Json::array()}};
        int outer=0,main=0;
        for (const auto& block:blocks) if (block.layer==layer) {
            const double perimeter=length(block.points),span=x_span(block.points);
            const bool closed=block.points.size()>=4 && distance(block.points.front(),block.points.back())<=rule.at("closure_tolerance_mm").get<double>();
            int matches=0;bool is_outer=false;
            for (const auto& candidate:candidates) if (candidate.layer==layer &&
                std::abs(x_span(candidate.points)-span)<=2*tolerance &&
                covers_vertices(block.points,candidate.points,tolerance) && covers_vertices(candidate.points,block.points,tolerance)) {
                ++matches;is_outer=candidate.outer;
            }
            const bool large_outer=matches==1 && is_outer && closed &&
                perimeter>=rule.at("minimum_outer_length_mm").get<double>() && span>=rule.at("minimum_outer_x_span_mm").get<double>();
            Json spatial=nullptr;
            if(large_outer)spatial=check_spatial_contract(block.points,rule.at("spatial_contract"));
            const bool is_main=large_outer && spatial.at("passed").get<bool>();
            if (matches==1 && is_outer) ++outer;
            if (is_main) ++main;
            row["paths"].push_back({{"gcode_line",block.line},{"source",matches==1?(is_outer?"outer":"hole"):"unmatched_or_ambiguous"},
                {"closed",closed},{"length_mm",perimeter},{"x_span_mm",span},{"main_outer",is_main},{"spatial_check",spatial}});
            if (row["required"].get<bool>()) {
                if(large_outer)for(const auto& failure:spatial.at("failures"))row["failures"].push_back(failure);
                if (matches!=1) row["failures"].push_back("unmatched_or_ambiguous_contour");
                if (!closed) row["failures"].push_back("open_depositing_path");
            }
        }
        row["outer_count"]=outer;row["main_outer_count"]=main;
        if (row["required"].get<bool>()) {
            if (outer!=rule.at("outer_contours_per_layer").get<int>()) row["failures"].push_back("outer_count_must_equal_one");
            if (main!=1) row["failures"].push_back("missing_complete_main_outer");
        }
        row["passed"]=row["failures"].empty();
        if (!row["passed"].get<bool>()) report["passed"]=false;
        report["layers"].push_back(row);
    }
    for (const auto& block:blocks) require(block.layer>=1 && block.layer<=rule.at("layer_count").get<int>(),"Fiber block outside expected layer range");
    return report;
}

libslicer::SliceObjectInput read_model(const fs::path& file)
{
    // Match the application input: indexed in-memory mesh, centered on the bed,
    // bottom at Z=0. File-only compatibility import has different placement rules.
    std::ifstream in(file,std::ios::binary);
    char header[80];std::uint32_t count=0;
    in.read(header,80);in.read(reinterpret_cast<char*>(&count),4);
    require(in.good() && count>0 && fs::file_size(file)==84ull+50ull*count,"Invalid binary STL fixture");
    libslicer::SliceObjectInput object;object.name="4xiao";
    libslicer::SliceVolumeInput volume;
    std::map<std::array<float,3>,std::uint32_t> index;
    double bottom=std::numeric_limits<double>::max();
    for (std::uint32_t i=0;i<count;++i) {
        char facet[50];in.read(facet,50);require(in.good(),"Truncated STL fixture");
        std::array<std::uint32_t,3> triangle;
        for (size_t j=0;j<3;++j) {
            std::array<float,3> p;std::memcpy(p.data(),facet+12+j*12,12);
            for (float v:p) require(std::isfinite(v),"Nonfinite STL coordinate");
            bottom=std::min(bottom,double(p[2]));
            const auto entry=index.emplace(p,static_cast<std::uint32_t>(volume.vertices.size()));
            if (entry.second) volume.vertices.push_back({p[0],p[1],p[2]});
            triangle[j]=entry.first->second;
        }
        volume.triangles.push_back({triangle[0],triangle[1],triangle[2]});
    }
    double minx=1e9,miny=1e9,maxx=-1e9,maxy=-1e9;
    for (auto p:volume.vertices) {minx=std::min(minx,double(p.x));maxx=std::max(maxx,double(p.x));miny=std::min(miny,double(p.y));maxy=std::max(maxy,double(p.y));}
    object.transform[3]=217.5-(minx+maxx)*.5;object.transform[7]=177.5-(miny+maxy)*.5;object.transform[11]=-bottom;
    object.volumes.push_back(std::move(volume));return object;
}
void self_test(const Json& rule)
{
    Path large{{170,158.8},{265,158.8},{265,179.4},{170,179.4},{170,158.8}},small{{0,0},{5,0},{5,5},{0,5},{0,0}};
    std::vector<Block> blocks;std::vector<Candidate> candidates;
    for (int layer=rule.at("first_required_layer");layer<=rule.at("last_required_layer");++layer) {
        blocks.push_back({layer,0,1,large});candidates.push_back({layer,true,large});
    }
    require(evaluate(rule,blocks,candidates).at("passed"),"Checker rejects a complete valid fixture");
    const auto must_fail=[&](const std::vector<Block>& b,const std::vector<Candidate>& c){require(!evaluate(rule,b,c).at("passed").get<bool>(),"Checker incorrectly accepted a broken fixture");};
    // Every required layer must be checked, including the last one.
    for (size_t i=0;i<blocks.size();++i) {auto missing=blocks;missing.erase(missing.begin()+i);must_fail(missing,candidates);}
    auto holes=candidates;for (auto& c:holes)c.outer=false;must_fail(blocks,holes);
    auto local=blocks;auto local_candidates=candidates;
    for (auto& b:local)b.points=small;for(auto& c:local_candidates)c.points=small;
    must_fail(local,local_candidates);
    auto split=blocks;split.push_back(blocks.back());must_fail(split,candidates);
    auto open=blocks;open.front().points.pop_back();must_fail(open,candidates);
    // Long, closed, correctly labelled outer rings must still fail if either
    // arrow passage is replaced by a detour along a bore, even with both bores enclosed.
    const auto bad_route=[&](const Path& path,const std::string& failure) {
        auto b=blocks;auto c=candidates;b.front().points=path;c.front().points=path;
        const auto r=evaluate(rule,b,c);require(!r.at("passed").get<bool>(),"Wrong spatial route passed");
        const auto& failures=r.at("layers")[rule.at("first_required_layer").get<int>()-1].at("failures");
        require(std::find(failures.begin(),failures.end(),failure)!=failures.end(),"Wrong route failed for an unrelated reason: "+failure);
    };
    bad_route({{170,158.8},{177,158.8},{177,160},{184,160},{184,158.8},{265,158.8},{265,179.4},{170,179.4},{170,158.8}},"upper_outer_passage_not_continuous");
    bad_route({{170,158.8},{265,158.8},{265,179.4},{184,179.4},{184,178},{177,178},{177,179.4},{170,179.4},{170,158.8}},"lower_outer_passage_not_continuous");
    bad_route({{170,158.8},{177,158.8},{177,168},{184,168},{184,158.8},{265,158.8},{265,179.4},{170,179.4},{170,158.8}},"left_upper_bore_not_enclosed");
    bad_route({{170,158.8},{265,179.4},{265,158.8},{170,179.4},{170,158.8}},"main_outer_self_intersects");
    // Same path can visit both halves but a detour between them is not continuous.
    bad_route({{170,158.8},{180,158.8},{180,160},{181,160},{181,158.8},{265,158.8},{265,179.4},{170,179.4},{170,158.8}},"upper_outer_passage_not_continuous");
    // A separate hole contour crossing the missing passage cannot repair the main exterior.
    auto supplemented=blocks;auto supplemented_candidates=candidates;
    supplemented.front().points={{170,158.8},{177,158.8},{177,168},{184,168},{184,158.8},{265,158.8},{265,179.4},{170,179.4},{170,158.8}};
    supplemented_candidates.front().points=supplemented.front().points;
    const Path extra_hole{{176,158.8},{186,158.8},{186,169},{176,169},{176,158.8}};
    supplemented.push_back({blocks.front().layer,0,2,extra_hole});
    supplemented_candidates.push_back({blocks.front().layer,false,extra_hole});
    const auto supplemented_report=evaluate(rule,supplemented,supplemented_candidates);
    const auto& supplemented_layer=supplemented_report.at("layers")[blocks.front().layer-1];
    require(supplemented_layer.at("outer_count")==1 && supplemented_layer.at("main_outer_count")==0 &&
        !supplemented_layer.at("passed").get<bool>(),"Separate hole ring incorrectly supplied missing main-outer coverage");
    const Path across_seam{{180,158.8},{265,158.8},{265,179.4},{170,179.4},{170,158.8},{180,158.8}};
    require(check_spatial_contract(across_seam,rule.at("spatial_contract")).at("passed"),"Passage split by seam incorrectly rejected");
    auto reversed=across_seam;std::reverse(reversed.begin(),reversed.end());
    require(check_spatial_contract(reversed,rule.at("spatial_contract")).at("passed"),"Reversed closed ring incorrectly rejected");
    auto unknown=candidates;unknown.clear();must_fail(blocks,unknown);
    std::istringstream commands("G90\nG1 X0 Y0\n;FIBER_BEGIN layer=3 extruder=0 purpose=contour\n;FIBER_LANDING_BEGIN\nG1 X90 Y0\nG1 X90 Y20\nG1 X0 Y20\n;FIBER_DEPLETED\n;FIBER_FINISH_BEGIN\nG1 X0 Y0\n;FIBER_END\n");
    const auto parsed=read_blocks(commands,{{0,0}});
    require(parsed.size()==1 && parsed.front().points.size()==4 && distance(parsed.front().points.front(),parsed.front().points.back())==20,"Finish motion incorrectly counted as depositing closure");
    std::cout<<"PASS: complete case; missing each required layer; holes; small rings; split rings; open paths; missing provenance; nondepositing finish; bore detours; discontinuous passages; self-intersection; seam/reversal invariance\n";
}
// Test-only reference geometry. No production generator or path validator is
// called here: the input is the source region, configuration and emitted moves.
Slic3r::Polyline integer_path(const Path& path)
{
    Slic3r::Polyline out;
    for(Point p:path)out.points.push_back(Slic3r::Point::new_scale(p.x,p.y));
    return out;
}
Slic3r::ExPolygons reference_offset(const Slic3r::ExPolygons& source,double delta)
{
    Slic3r::ClipperLib::ClipperOffset operation;operation.ArcTolerance=12.5;operation.ShortestEdgeLength=0;
    for(const auto& polygon:Slic3r::to_polygons(source))
        operation.AddPath(polygon.points,Slic3r::ClipperLib::jtRound,Slic3r::ClipperLib::etClosedPolygon);
    Slic3r::ClipperLib::Paths paths;operation.Execute(paths,scale_(delta));
    return Slic3r::ClipperPaths_to_Slic3rExPolygons(paths,true);
}
Slic3r::ExPolygons reference_sweep(const Path& path,double radius)
{
    auto line=integer_path(path);
    const bool closed=distance(path.front(),path.back())<=std::sqrt(2.)*.001;
    if(closed)line.points.back()=line.points.front();
    Slic3r::ClipperLib::ClipperOffset operation;operation.ArcTolerance=12.5;operation.ShortestEdgeLength=0;
    operation.AddPath(line.points,Slic3r::ClipperLib::jtRound,closed?Slic3r::ClipperLib::etClosedLine:Slic3r::ClipperLib::etOpenButt);
    Slic3r::ClipperLib::Paths paths;operation.Execute(paths,scale_(radius));
    return Slic3r::ClipperPaths_to_Slic3rExPolygons(paths,true);
}
// Frozen operation budget, before observing results: two 0.002 mm path
// normalizations; two XY G-code roundings; one float source/rejected conversion
// within +/-512 mm; eight world/local integer conversions; four safety offsets
// (reference and production keepout clipping + material difference); four round
// offset boundaries (two sides, keepout and half-width). Test offsets below use
// the same 12.5-coordinate-unit arc error. No post-hoc threshold fitting.
static_assert(Slic3r::ClipperSafetyOffset==10.f,"Update the frozen safety error budget");
static_assert(Slic3r::GCodeFormatter::XYZF_EXPORT_DIGITS==3,"Update the frozen G-code error budget");
constexpr double gap_integer_error=8*1.4142135623730951e-6+4*10e-6+4*.0000125;
const double gap_preview_error=std::sqrt(2.)*.5*(std::nextafter(512.f,INFINITY)-512.f);
const double gap_reference_error=std::sqrt(2.)*.0005+gap_preview_error+gap_integer_error;
const double gap_rejected_error=.002+gap_reference_error;
const double gap_final_error=.004+std::sqrt(2.)*.001+gap_preview_error+gap_integer_error;

struct GapPath { Path points; bool accepted=true; };

void check_components(const std::vector<size_t>& components,size_t expected)
{
    require(components.size()==expected,"Missing/unexpected source regions");
    std::set<size_t> unique(components.begin(),components.end());
    require(unique.size()==expected && *unique.begin()==0 && *unique.rbegin()==expected-1,"Missing or repeated source component");
}

// Project the two supports to the independent reference ring. Either direction
// is admissible only when its whole arc stays within the adjacent-row strip and
// the emitted return covers that arc in both directions (with bounded error).
bool matches_boundary_arc(const Path& run,const Slic3r::Polygons& rings,double tolerance)
{
    if(run.size()<2)return false;
    const double lo=std::min(run.front().y,run.back().y)-tolerance;
    const double hi=std::max(run.front().y,run.back().y)+tolerance;
    for(Point p:run)if(p.y<lo || p.y>hi)return false;
    struct Support {size_t ring=0,edge=0;double t=0,distance=std::numeric_limits<double>::max();Point point;};
    const auto supports=[&](Point p) {
        std::vector<Support> found;
        for(size_t r=0;r<rings.size();++r)for(size_t e=0;e<rings[r].points.size();++e) {
            const auto& a=rings[r].points[e];const auto& b=rings[r].points[(e+1)%rings[r].points.size()];
            const Point from{Slic3r::unscale<double>(a.x()),Slic3r::unscale<double>(a.y())},to{Slic3r::unscale<double>(b.x()),Slic3r::unscale<double>(b.y())};
            const double dx=to.x-from.x,dy=to.y-from.y,n=dx*dx+dy*dy;
            const double t=n==0?0:std::clamp(((p.x-from.x)*dx+(p.y-from.y)*dy)/n,0.,1.);
            const Point projection{from.x+t*dx,from.y+t*dy};
            const double d=distance(p,projection);
            if(d<=tolerance)found.push_back({r,e,t,d,projection});
        }
        std::sort(found.begin(),found.end(),[](const Support& a,const Support& b){return a.distance<b.distance;});
        return found;
    };
    // Quantized reference boundaries may put a support within the error band
    // of two nearby branches. Test the admissible arcs; nearest alone cannot
    // resolve that ambiguity without the rest of the connecting geometry.
    const auto starts=supports(run.front()),ends=supports(run.back());
    const auto near=[&](const Path& subject,const Path& reference,const Path& reference_arc) {
        Slic3r::ClipperLib::ClipperOffset operation;operation.ArcTolerance=12.5;operation.ShortestEdgeLength=0;
        operation.AddPath(integer_path(reference).points,Slic3r::ClipperLib::jtRound,Slic3r::ClipperLib::etOpenRound);
        Slic3r::ClipperLib::Paths band;operation.Execute(band,scale_(tolerance));
        // A normal perturbation e of two lines moves their acute intersection
        // by up to e/sin(theta/2). Propagate only the reference reconstruction
        // error here; do not increase the normal or normalization tolerance.
        for(size_t i=1;i+1<reference_arc.size();++i) {
            const Point p=reference_arc[i],a=reference_arc[i-1],b=reference_arc[i+1];
            const double ax=a.x-p.x,ay=a.y-p.y,bx=b.x-p.x,by=b.y-p.y;
            const double product=std::hypot(ax,ay)*std::hypot(bx,by);if(product==0)continue;
            const double cosine=std::clamp((ax*bx+ay*by)/product,-1.,1.);
            const double sine=std::sqrt((1-cosine)/2);
            if(sine==0)return false;
            const double radius=tolerance+gap_reference_error*(1/sine-1);
            if(radius<=tolerance+SCALING_FACTOR)continue;
            if(!std::isfinite(radius) || radius>=hi-lo)return false;
            Slic3r::ClipperLib::ClipperOffset corner;corner.ArcTolerance=12.5;corner.ShortestEdgeLength=0;
            corner.AddPath(integer_path({p}).points,Slic3r::ClipperLib::jtRound,Slic3r::ClipperLib::etOpenRound);
            Slic3r::ClipperLib::Paths disk;corner.Execute(disk,scale_(radius));
            band.insert(band.end(),std::make_move_iterator(disk.begin()),std::make_move_iterator(disk.end()));
        }
        return Slic3r::diff_pl(Slic3r::Polylines{integer_path(subject)},Slic3r::ClipperPaths_to_Slic3rExPolygons(band,true)).empty();
    };
    for(const auto& a:starts)for(const auto& b:ends)if(a.ring==b.ring)for(int direction:{1,-1}) {
        const auto& ring=rings[a.ring].points;
        const Support& from=direction>0?a:b;const Support& to=direction>0?b:a;
        const double begin=from.edge+from.t;
        double end=to.edge+to.t;if(end<begin)end+=ring.size();
        Path reference{from.point};bool in_strip=true;
        for(size_t i=size_t(std::floor(begin))+1;double(i)<end;++i) {
            const auto& v=ring[i%ring.size()];Point p{Slic3r::unscale<double>(v.x()),Slic3r::unscale<double>(v.y())};
            if(p.y<lo || p.y>hi){in_strip=false;break;}
            reference.push_back(p);
        }
        if(!in_strip)continue;
        reference.push_back(to.point);
        const bool outward=near(run,reference,reference),inward=near(reference,run,reference);
        if(outward && inward)return true;
    }
    return false;
}


Json check_gap_domain(const Slic3r::ExPolygons& centerlines,const std::vector<GapPath>& paths,
    double angle,Point origin,double pitch,double tolerance,bool full=true)
{
    Json report={{"passed",true},{"failures",Json::array()},{"scan_intervals",0},{"accepted_paths",0},{"rejected_paths",0}};
    const auto fail=[&](const std::string& reason){report["passed"]=false;if(report["failures"].size()<12)report["failures"].push_back(reason);};
    const auto local_point=[&](Point p){return Point{-std::sin(angle)*p.x+std::cos(angle)*p.y,std::cos(angle)*p.x+std::sin(angle)*p.y};};
    auto local=centerlines;
    for(auto& region:local) {
        auto transform=[&](Slic3r::Polygon& ring){for(auto& p:ring.points){Point q=local_point({Slic3r::unscale<double>(p.x()),Slic3r::unscale<double>(p.y())});p=Slic3r::Point::new_scale(q.x,q.y);}ring.reverse();};
        transform(region.contour);for(auto& hole:region.holes)transform(hole);
    }
    if(local.empty()) {if(!paths.empty())fail("path_in_empty_centerline_domain");return report;}
    const auto bounds=Slic3r::get_extents(local);
    const coord_t grid_step=scale_(pitch), grid_origin=scale_(local_point(origin).y);
    auto first=grid_origin+coord_t(std::floor(double(bounds.min.y()-grid_origin)/grid_step))*grid_step;
    if(full)first+=(grid_step+coord_t(SCALED_EPSILON))/2;
    Slic3r::Polylines grid;
    for(auto y=first;y<=bounds.max.y();y+=grid_step)grid.emplace_back(Slic3r::Points{{bounds.min.x()-1,y},{bounds.max.x()+1,y}});
    // Offsets propagate normal uncertainty through steep walls without an
    // arbitrary larger tolerance along the scan direction.
    const auto rings=Slic3r::to_polygons(local);
    const auto definite=reference_offset(local,-tolerance);
    const auto possible=reference_offset(local,tolerance);
    const auto expected=Slic3r::intersection_pl(grid,definite);
    std::map<coord_t,std::vector<std::pair<double,int>>> supports;
    for(const auto& scan:Slic3r::intersection_pl(grid,local)) {
        supports[scan.points.front().y()].emplace_back(Slic3r::unscale<double>(std::min(scan.points.front().x(),scan.points.back().x())),-1);
        supports[scan.points.front().y()].emplace_back(Slic3r::unscale<double>(std::max(scan.points.front().x(),scan.points.back().x())),1);
    }
    const auto support=[&](Point p,double error) {
        const coord_t row=first+coord_t(std::llround((scale_(p.y)-first)/double(grid_step)))*grid_step;
        const auto found=supports.find(row);
        int side=0;double nearest=std::numeric_limits<double>::max();
        if(found!=supports.end())for(const auto& endpoint:found->second)
            if(std::abs(endpoint.first-p.x)<nearest){nearest=std::abs(endpoint.first-p.x);side=endpoint.second;}
        unsigned sides=side<0?1u:side>0?2u:0u;
        if(found!=supports.end())for(const auto& endpoint:found->second)
            if(std::abs(endpoint.first-p.x)<=error)sides|=endpoint.second<0?1u:2u;
        return std::make_pair(row,sides);
    };
    report["scan_intervals"]=expected.size();
    std::map<coord_t,std::vector<std::pair<double,double>>> actual;
    double missing_mm=0,duplicate_mm=0;
    for(const auto& path:paths) {
        if(path.accepted)report["accepted_paths"]=report["accepted_paths"].get<int>()+1;
        else report["rejected_paths"]=report["rejected_paths"].get<int>()+1;
        Path points;for(Point p:path.points)points.push_back(local_point(p));
        std::vector<std::optional<coord_t>> scan_rows(points.size());
        const double path_tolerance=path.accepted?tolerance:std::min(tolerance,gap_rejected_error);
        if(!Slic3r::diff_pl(Slic3r::Polylines{integer_path(points)},possible).empty())fail("path_outside_reference_domain");
        for(size_t i=1;i<points.size();++i) {
            const Point a=points[i-1],b=points[i];
            const auto row=first+coord_t(std::llround((scale_((a.y+b.y)*.5)-first)/double(grid_step)))*grid_step;
            const double y=Slic3r::unscale<double>(row);
            if(std::abs(a.y-y)<=path_tolerance && std::abs(b.y-y)<=path_tolerance) {
                scan_rows[i]=row;
                actual[row].emplace_back(std::min(a.x,b.x),std::max(a.x,b.x));
            } else if(!Slic3r::intersection_pl(Slic3r::Polylines{integer_path({a,b})},definite).empty()) {
                fail("boundary_return_retreats_inside_reference_domain");
            }
        }
        for(size_t first=1;first<points.size();) {
            if(scan_rows[first]){++first;continue;}
            size_t end=first+1;while(end<points.size() && !scan_rows[end])++end;
            const size_t failures_before=report["failures"].size();
            const auto from=support(points[first-1],path_tolerance),to=support(points[end-1],path_tolerance);
            if(std::abs(points[first-1].y-Slic3r::unscale<double>(from.first))>path_tolerance ||
               std::abs(points[end-1].y-Slic3r::unscale<double>(to.first))>path_tolerance ||
               std::abs(from.first-to.first)!=grid_step || (from.second & to.second)==0)
                fail("return_does_not_join_adjacent_same_side_supports");
            const Path run(points.begin()+first-1,points.begin()+end);
            if(!matches_boundary_arc(run,rings,path_tolerance)) {
                fail("return_does_not_cover_reference_boundary_arc");
            }
            if(report["failures"].size()>failures_before) {
                Json detail={{"accepted",path.accepted},{"from_side",from.second},{"to_side",to.second},{"row_delta",from.first-to.first},{"points",Json::array()}};
                for(Point p:run)detail["points"].push_back({p.x,p.y});
                report["invalid_returns"].push_back(std::move(detail));
            }
            first=end;
        }
    }
    for(auto& row:actual)std::sort(row.second.begin(),row.second.end());
    for(const auto& scan:expected) {
        const double lo=Slic3r::unscale<double>(std::min(scan.points.front().x(),scan.points.back().x()));
        const double hi=Slic3r::unscale<double>(std::max(scan.points.front().x(),scan.points.back().x()));
        double end=lo;
        for(const auto& interval:actual[scan.points.front().y()]) {
            const double a=std::max(lo,interval.first),b=std::min(hi,interval.second);
            if(b<=a)continue;
            if(a>end+tolerance)missing_mm+=a-end;
            if(a<end-tolerance)duplicate_mm+=std::min(end,b)-a;
            end=std::max(end,b);
        }
        if(end<hi-tolerance)missing_mm+=hi-end;
    }
    if(missing_mm>0)fail("missing_reference_scan_interval");
    if(duplicate_mm>0)fail("duplicated_reference_scan_interval");
    report["missing_mm"]=missing_mm;report["duplicate_mm"]=duplicate_mm;
    return report;
}

void check_short_rejection(const Path& points,const std::string& reason,double minimum,double budget)
{
    const bool too_short=reason.rfind("too_short",0)==0;
    require(too_short || reason.rfind("process_budget_too_short",0)==0,"Unexpected infill rejection: "+reason);
    require(points.size()>1 && length(points)<(too_short?minimum:std::max(minimum,budget))+.002+gap_preview_error,"Invalid short-path rejection");
}

void gap_checker_self_test()
{
    Slic3r::Polygon ring;
    for(Point p:Path{{0,0},{10,0},{10,5},{0,5}})ring.points.push_back(Slic3r::Point::new_scale(p.x,p.y));
    const Slic3r::ExPolygons domain{Slic3r::ExPolygon(ring)};
    // At angle pi/2 scans are horizontal. Sparse phase zero, fixed origin .5.
    const std::vector<GapPath> valid{{{{0,.5},{10,.5},{10,1.5},{0,1.5},{0,2.5},{10,2.5},{10,3.5},{0,3.5},{0,4.5},{10,4.5}},true}};
    auto check=[&](const std::vector<GapPath>& paths){return check_gap_domain(domain,paths,PI/2,{0,.5},1,gap_final_error,false);};
    require(check(valid).at("passed"),"Gap checker rejects valid boundary returns");
    auto retreat=valid;
    for(size_t i=1;i+1<retreat.front().points.size();++i)retreat.front().points[i].x+=retreat.front().points[i].x<5?.45:-.45;
    require(!check(retreat).at("passed").get<bool>(),"Gap checker accepted recessed internal returns");
    require(!check({}).at("passed").get<bool>(),"Gap checker accepted a missing region");
    Slic3r::Polygon acute;
    for(Point p:Path{{0,0},{10,1},{0,2}})acute.points.push_back(Slic3r::Point::new_scale(p.x,p.y));
    require(matches_boundary_arc({{9,.9},{9.996,1},{9,1.1}},{acute},gap_rejected_error),"Bounded edge rounding incorrectly rejects an acute intersection");
    require(!matches_boundary_arc({{9,.9},{9.9,1},{9,1.1}},{acute},gap_rejected_error),"Acute-corner error propagation hides excessive retreat");
    auto winding=valid;
    const Path detour{{10,5},{0,5},{0,0},{10,0},{10,.5}};
    winding.front().points.insert(winding.front().points.begin()+2,detour.begin(),detour.end());
    require(!check(winding).at("passed").get<bool>(),"Gap checker accepted a complete extra boundary circuit");
    bool bad_components=false;
    try {check_components({1,1},2);}catch(const std::exception&){bad_components=true;}
    require(bad_components,"Gap checker accepted a missing main component with unchanged record count");
    check_components({0,1},2);
    auto duplicate=valid;duplicate.push_back(valid.front());
    require(!check(duplicate).at("passed").get<bool>(),"Gap checker accepted duplicate scans");
    // Classification cannot legitimize an unprintably-short claim for this strand.
    bool rejected=false;
    try {check_short_rejection(valid.front().points,"process_budget_too_short",0,25.5);}catch(const std::exception&){rejected=true;}
    require(rejected,"Gap checker accepted a fabricated short-path rejection");
    check_short_rejection({{0,0},{1,0}},"process_budget_too_short",0,25.5);
    std::cout<<"PASS: independent gap checker accepts boundary routing and rejects retreat, missing region, duplicate scans\n";
}

Json evaluate_gap(const libslicer::ToolpathPreview& preview,const std::vector<Block>& blocks,const Json& effective)
{
    require(Slic3r::ContourRoundingOptions{}.chord_tolerance_mm==.00005,"Update the frozen offset arc error budget");
    require(effective.at("generate_reinforced_infills")=="1" && effective.at("reinforced_infill_density")=="100%","Gap acceptance requires enabled 100% infill");
    require(effective.at("fiber_width")=="1" && effective.at("reinforced_infill_pattern")=="rectilinear" &&
        effective.at("fiber_infill_bend_radius")=="0" && effective.at("fiber_contour_bend_radius")=="0" && effective.at("fiber_contour_infill_clearance")=="0","Unexpected gap acceptance geometry");
    const double budget=std::stod(effective.at("fiber_landing_length").get<std::string>())+
        std::stod(effective.at("fiber_start_stabilization_length").get<std::string>())+
        std::stod(effective.at("fiber_minimum_effective_length").get<std::string>())+
        std::stod(effective.at("fiber_cut_to_contact_length").get<std::string>());
    require(std::abs(budget-25.5)<1e-9,"Unexpected fiber process budget");
    Json result={{"passed",true},{"layers",Json::array()},{"geometry_tolerance_mm",gap_final_error},{"reference_tolerance_mm",gap_reference_error}};
    size_t resin_segments=0;for(const auto& segment:preview.segments)if(segment.extrusion_role==libslicer::ToolpathExtrusionRole::ResinInfill && segment.deposition==libslicer::ToolpathDepositionKind::Thermoplastic)++resin_segments;
    require(resin_segments>0,"Fixture must retain ordinary resin leftover deposition");result["resin_segments"]=resin_segments;
    for(int layer=4;layer<=24;++layer) {
        Json row={{"layer",layer},{"passed",true},{"regions",Json::array()}};
        std::vector<const libslicer::FiberFillDiagnosticPath*> sources;
        for(const auto& diagnostic:preview.fiber_fill_diagnostics)
            if(int(diagnostic.layer_index)+1==layer && diagnostic.kind==libslicer::FiberDiagnosticKind::OriginalContourRegion)sources.push_back(&diagnostic);
        // STL topology: a single main island until the last fiber layer, then
        // 64 tiny disconnected remnants in addition to the main island.
        const size_t expected_components=layer==24?65:1;
        std::vector<size_t> components;for(const auto* source:sources)components.push_back(source->component_id);
        check_components(components,expected_components);
        for(const auto& block:blocks)if(block.layer==layer)require(block.component<expected_components,"Unmatched depositing path");
        for(const auto& diagnostic:preview.fiber_fill_diagnostics)if(int(diagnostic.layer_index)+1==layer && diagnostic.kind==libslicer::FiberDiagnosticKind::RejectedPath)
            require(diagnostic.policy_group_id==0 && diagnostic.component_id<expected_components,"Unmatched rejected path");
        float angle=float(45*PI/180);if((layer-1)&1)angle+=float(PI/2);angle+=float(PI/2);
        for(const auto* source:sources) {
            require(source->policy_group_id==0 && !source->boundaries.empty(),"Unexpected fixture policy or empty source");
            Slic3r::ExPolygon area;
            for(size_t i=0;i<source->boundaries.size();++i) {
                Slic3r::Polygon ring;
                for(const auto& p:source->boundaries[i]) {
                    require(std::abs(p.x)<512 && std::abs(p.y)<512,"Preview coordinate exceeds frozen error budget bounds");
                    ring.points.push_back(Slic3r::Point::new_scale(p.x,p.y));
                }
                if(ring.points.front()==ring.points.back())ring.points.pop_back();
                if(i==0){ring.make_counter_clockwise();area.contour=std::move(ring);}
                else {ring.make_clockwise();area.holes.push_back(std::move(ring));}
            }
            Slic3r::ExPolygons occupied;
            std::vector<GapPath> paths;
            size_t contour_count=0;
            for(const auto& block:blocks)if(block.layer==layer && block.component==source->component_id) {
                if(block.contour){Slic3r::append(occupied,reference_sweep(block.points,.5));++contour_count;}
                else paths.push_back({block.points,true});
            }
            const auto keepout=Slic3r::intersection_ex(Slic3r::union_ex(occupied),Slic3r::ExPolygons{area},Slic3r::ApplySafetyOffset::Yes);
            const auto material=Slic3r::diff_ex(Slic3r::ExPolygons{area},keepout,Slic3r::ApplySafetyOffset::Yes);
            const auto centerlines=reference_offset(material,-.5);
            for(const auto& diagnostic:preview.fiber_fill_diagnostics)if(int(diagnostic.layer_index)+1==layer &&
                diagnostic.component_id==source->component_id && !diagnostic.contour && diagnostic.kind==libslicer::FiberDiagnosticKind::RejectedPath) {
                Path points;for(const auto& p:diagnostic.points)points.push_back({p.x,p.y});
                check_short_rejection(points,diagnostic.reason,std::stod(effective.at("fiber_minimum_path_length").get<std::string>()),budget);
                paths.push_back({std::move(points),false});
            }
            auto checked=check_gap_domain(centerlines,paths,angle,{217.5,177.5},1,gap_final_error);
            checked["component"]=source->component_id;checked["contours"]=contour_count;
            // The main island has long printable passages at every required
            // layer; a missing/fully rejected infill cannot pass this fixture.
            if(source->component_id==0 && (checked["accepted_paths"].get<int>()==0 || contour_count==0)) {
                checked["passed"]=false;checked["failures"].push_back("main_region_requires_actual_contour_and_infill");
            }
            if(source->component_id==0) {
                // Fixed interior passage between the two left bores and the
                // right-hand body. Its location is specified in model/bed XY,
                // independently of the generated path or its selected seam.
                Slic3r::Polygon window;
                for(Point p:Path{{183,165},{199,165},{199,176},{183,176}})window.points.push_back(Slic3r::Point::new_scale(p.x,p.y));
                double central_length=0;
                for(const auto& path:paths)if(path.accepted)
                    for(const auto& part:Slic3r::intersection_pl(Slic3r::Polylines{integer_path(path.points)},Slic3r::ExPolygons{Slic3r::ExPolygon(window)}))
                        central_length+=Slic3r::unscale<double>(part.length());
                checked["central_accepted_length_mm"]=central_length;
                if(central_length<=0){checked["passed"]=false;checked["failures"].push_back("central_passage_requires_actual_infill");}
            }
            if(!checked["passed"].get<bool>())row["passed"]=false;
            row["regions"].push_back(std::move(checked));
        }
        if(!row["passed"].get<bool>())result["passed"]=false;
        result["layers"].push_back(std::move(row));
    }
    return result;
}

// Positive-radius acceptance uses the emitted fixed-radius return template,
// not the zero-radius requirement to cover each scan interval up to its end.
Json check_rounded_domain(const Slic3r::ExPolygons& centerlines,const std::vector<GapPath>& paths,double angle)
{
    constexpr double radius=.3,pitch=1.;
    Json report={{"passed",true},{"failures",Json::array()},{"accepted_paths",0},{"returns",0},{"max_shape_error_mm",0.}};
    const auto fail=[&](const char* reason){report["passed"]=false;if(report["failures"].size()<12)report["failures"].push_back(reason);};
    Slic3r::Linesf edges;
    for(const auto& ring:Slic3r::to_polygons(centerlines))for(const auto& line:ring.lines())
        edges.emplace_back(line.a.cast<double>()*SCALING_FACTOR,line.b.cast<double>()*SCALING_FACTOR);
    const auto tree=Slic3r::AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    const auto containing=reference_offset(centerlines,gap_final_error);
    const auto local=[&](Point p){return Point{-std::sin(angle)*p.x+std::cos(angle)*p.y,std::cos(angle)*p.x+std::sin(angle)*p.y};};
    for(const auto& path:paths)if(path.accepted) {
        report["accepted_paths"]=report["accepted_paths"].get<int>()+1;
        if(!Slic3r::diff_pl(Slic3r::Polylines{integer_path(path.points)},containing).empty())fail("outside_centerline_domain");
        Path points;for(Point p:path.points)points.push_back(local(p));
        std::vector<std::pair<size_t,size_t>> scans;
        for(size_t i=1;i<points.size();++i)
            // R=.3 arc chords are <.012 mm. Process boundaries can subdivide
            // a straight support, so extend the shape check to those fragments.
            if(std::abs(points[i].y-points[i-1].y)<.0015 && std::abs(points[i].x-points[i-1].x)>.02) {
                if(!scans.empty() && scans.back().second==i-1)scans.back().second=i;
                else scans.emplace_back(i-1,i);
            }
        for(size_t k=1;k<scans.size();++k) {
            const size_t scan_end=scans[k-1].second,next_scan=scans[k].first;
            if(next_scan<=scan_end+1)continue;
            const double dy=points[next_scan].y-points[scan_end].y;
            if(std::abs(dy)<gap_final_error)continue;
            const size_t turns=size_t(std::llround(std::abs(dy)/pitch));
            if(turns==0 || std::abs(std::abs(dy)-turns*pitch)>gap_final_error){fail("unclassified_return");continue;}
            std::vector<size_t> stations{scan_end};
            const double step=std::copysign(1.,dy);
            // A missing intermediate scan may leave consecutive U-turns in
            // one G-code path. Split at each intermediate scan level and run
            // the same radius/contact oracle on every individual turn.
            for(size_t turn_index=1;turn_index<turns;++turn_index) {
                const double target=points[scan_end].y+step*turn_index*pitch;
                size_t begin=next_scan,end=0;
                for(size_t i=stations.back()+2;i+1<next_scan;++i)if(std::abs(points[i].y-target)<=gap_final_error) {
                    begin=std::min(begin,i);end=i;
                }
                if(begin==next_scan) {fail("unclassified_return");stations.clear();break;}
                stations.push_back((begin+end)/2);
            }
            if(stations.empty())continue;
            stations.push_back(next_scan);
            const double incoming_direction=std::copysign(1.,points[scan_end].x-points[scans[k-1].first].x);
            for(size_t turn_index=0;turn_index<turns;++turn_index) {
                const size_t first=stations[turn_index],last=stations[turn_index+1];
                const double direction=turn_index%2?-incoming_direction:incoming_direction;
                Path shape;std::vector<double> contacts;
                for(size_t i=first;i<=last;++i) {
                    const Point q{direction*(points[i].x-points[first].x),step*(points[i].y-points[first].y)};shape.push_back(q);
                    if(q.y>.003 && q.y<radius)contacts.push_back(q.x-std::sqrt(std::max(0.,radius*radius-std::pow(q.y-radius,2))));
                    else if(q.y>pitch-radius && q.y<pitch-.003)contacts.push_back(q.x-std::sqrt(std::max(0.,radius*radius-std::pow(q.y-(pitch-radius),2))));
                    else if(q.y>=radius && q.y<=pitch-radius)contacts.push_back(q.x-radius);
                }
                if(contacts.empty()){fail("missing_return_arc");continue;}
                std::sort(contacts.begin(),contacts.end());const double contact=contacts[contacts.size()/2];double error=0;
                for(auto q:shape) {
                    q.x-=contact;double deviation;
                    if(q.x<=0 && (std::abs(q.y)<.003 || std::abs(q.y-pitch)<.003))deviation=std::min(std::abs(q.y),std::abs(q.y-pitch));
                    else if(q.y<radius)deviation=std::abs(std::hypot(q.x,q.y-radius)-radius);
                    else if(q.y>pitch-radius)deviation=std::abs(std::hypot(q.x,q.y-(pitch-radius))-radius);
                    else deviation=std::abs(q.x-radius);
                    error=std::max(error,deviation);
                }
                report["max_shape_error_mm"]=std::max(report["max_shape_error_mm"].get<double>(),error);
                if(error>gap_final_error)fail("wrong_return_radius_or_shape");
                double nearest=HUGE_VAL;
                for(size_t i=first;i<=last;++i) {
                    size_t edge=0;Slic3r::Vec2d point;
                    const auto p=path.points[i];
                    const double squared=Slic3r::AABBTreeLines::squared_distance_to_indexed_lines(edges,tree,Slic3r::Vec2d(p.x,p.y),edge,point);
                    if(squared>=0)nearest=std::min(nearest,std::sqrt(squared));
                }
                // Contact may be in the interior of the straight connector, not
                // at a G-code vertex. Check segment-to-boundary distance as well.
                if(nearest>gap_final_error)for(size_t i=first+1;i<=last;++i) {
                    const auto a=path.points[i-1],b=path.points[i];
                    const Slic3r::Vec2d lo(std::min(a.x,b.x)-nearest,std::min(a.y,b.y)-nearest),hi(std::max(a.x,b.x)+nearest,std::max(a.y,b.y)+nearest);
                    Slic3r::AABBTreeIndirect::traverse(tree,Slic3r::AABBTreeIndirect::intersecting(Eigen::AlignedBox<double,2>(lo,hi)),[&](const auto& node) {
                        const auto& edge=edges[node.idx];const Point c{edge.a.x(),edge.a.y()},d{edge.b.x(),edge.b.y()};
                        if(Slic3r::Geometry::segments_intersect(Slic3r::Point::new_scale(a.x,a.y),Slic3r::Point::new_scale(b.x,b.y),Slic3r::Point::new_scale(c.x,c.y),Slic3r::Point::new_scale(d.x,d.y)))nearest=0;
                        else nearest=std::min({nearest,point_segment_distance(a,c,d),point_segment_distance(b,c,d),point_segment_distance(c,a,b),point_segment_distance(d,a,b)});
                        return nearest>gap_final_error;
                    });
                }
                if(nearest>gap_final_error)fail("return_has_unexplained_boundary_retreat");
                report["returns"]=report["returns"].get<int>()+1;
            }
        }
    }
    return report;
}

void rounded_checker_self_test()
{
    const Slic3r::ExPolygons domain{Slic3r::ExPolygon(Slic3r::Polygon(integer_path({{0,0},{10,0},{10,3},{0,3}}).points))};
    const auto path=[](double radius,double contact) {
        Path points{{contact-2,1}};
        for(int k=0;k<2;++k)for(int i=0;i<=80;++i) {
            const double a=(-1+k)*PI/2+(PI/2)*i/80;
            points.push_back({contact+radius*std::cos(a),(k?2-radius:1+radius)+radius*std::sin(a)});
        }
        points.push_back({contact-2,2});return points;
    };
    // angle=-pi/2 keeps X and reverses Y, preserving the same return contract.
    require(check_rounded_domain(domain,{{path(.3,9.7),true}},-PI/2)["passed"].get<bool>(),"Valid rounded return rejected");
    require(!check_rounded_domain(domain,{{path(.3,9.25),true}},-PI/2)["passed"].get<bool>(),"Extra .45 mm retreat passed");
    require(!check_rounded_domain(domain,{{path(.5,9.5),true}},-PI/2)["passed"].get<bool>(),"Wrong .5 mm radius passed");

    // A narrow region can connect two returns back to back without an
    // intervening scanline. Each 1 mm turn still has to pass independently.
    const Slic3r::ExPolygons strip{Slic3r::ExPolygon(Slic3r::Polygon(
        integer_path({{-.3,0},{.3,0},{.3,4},{-.3,4}}).points))};
    Path chained{{-.2,1},{0,1}};
    const auto append_arc=[&](Point center,double begin,double end) {
        for(int i=1;i<=40;++i) {
            const double angle=begin+(end-begin)*i/40;
            chained.push_back({center.x+.3*std::cos(angle),center.y+.3*std::sin(angle)});
        }
    };
    append_arc({0,1.3},-PI/2,0);
    chained.push_back({.3,1.7});
    append_arc({0,1.7},0,PI/2);
    const size_t second_start=chained.size();
    append_arc({0,2.3},-PI/2,-PI);
    chained.push_back({-.3,2.7});
    append_arc({0,2.7},PI,PI/2);
    chained.push_back({.2,3});
    const auto world=[](const Path& local) {
        Path result;for(const Point& point:local)result.push_back({-point.x,point.y});return result;
    };
    const auto two_returns=check_rounded_domain(strip,{{world(chained),true}},PI/2);
    require(two_returns["passed"].get<bool>() && two_returns["returns"]==2,
        "Two consecutive fixed-radius returns were not checked separately");
    chained[second_start+20].x+=.04;
    require(!check_rounded_domain(strip,{{world(chained),true}},PI/2)["passed"].get<bool>(),
        "Malformed second return passed the chained-return check");
}

int rounded_model_acceptance(const fs::path& model,size_t expected_layers,int contours,const fs::path& output)
{
    fs::create_directories(output);
    const auto pinned=read_json(fs::path(LIBSLICER_TEST_DATA_DIR)/"continuous_fiber/4xiao/config.json");const auto& selection=pinned.at("selection");
    libslicer::LibraryOptions options;options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);libslicer::ConfigSelection selected;
    selected.machine_model_id=selection.at("machine_model_id");selected.machine_variant_id=selection.at("machine_variant_id");selected.process_preset_id=selection.at("process_preset_id");
    selected.filament_preset_ids=selection.at("filament_preset_ids").get<std::vector<std::string>>();selected.filament_physical_tools=selection.at("filament_physical_tools").get<std::vector<unsigned>>();
    require(library->activate_config(selected).success,"Cannot activate model acceptance presets");
    const std::vector<std::pair<std::string,std::string>> patch{{"fiber_contour_bend_radius","0.3"},{"fiber_infill_bend_radius","0.3"},
        {"outer_reinforced_perimeters_counts",std::to_string(contours)},{"generate_reinforced_infills","1"},{"reinforced_infill_pattern","rectilinear"},
        {"reinforced_infill_density","100%"},{"fiber_resin_fill_density","100%"},{"fiber_fill_debug","1"}};
    require(library->apply_active_config_patch(patch).success,"Rounded acceptance parameters rejected");
    libslicer::SliceRequest request;request.config=*library->active_config_snapshot();request.center_on_build_plate=false;
    Json effective=Json::object();for(const auto& setting:request.config.values())effective[setting.first]=setting.second;
    for(const auto& setting:patch)require(effective.at(setting.first)==setting.second,"Effective acceptance parameter differs: "+setting.first);
    require(effective.at("fiber_width")=="1" && effective.at("fiber_contour_infill_clearance")=="0","Unexpected rounded acceptance geometry");
    write_json(output/"effective_config.json",effective);request.objects.push_back(read_model(model));request.output_gcode_path=(output/"model.gcode").string();
    const auto started=std::chrono::steady_clock::now();const auto result=library->slice(request);
    Json report={{"passed",false},{"model_fingerprint",fingerprint(model)},{"slice_success",result.success},{"slice_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()},
        {"layers",Json::array()},{"failures",Json::array()},{"diagnostics",Json::array()}};
    for(const auto& d:result.diagnostics)report["diagnostics"].push_back({{"code",d.code},{"message",d.message}});
    write_json(output/"report.json",report);
    if (!result.success || !result.preview) {
        for (const auto& diagnostic:result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 1;
    }
    require(result.preview->layers.size()==expected_layers,"Unexpected rounded model layer count");
    std::vector<Point> offsets;std::istringstream stream(effective.at("extruder_offset").get<std::string>());for(std::string item;std::getline(stream,item,',');){const auto x=item.find('x');offsets.push_back({std::stod(item.substr(0,x)),std::stod(item.substr(x+1))});}
    std::ifstream gcode(result.output.path);require(bool(gcode),"Missing rounded G-code");const auto blocks=read_blocks(gcode,offsets,true);
    report["passed"]=true;report["layer_count"]=expected_layers;report["gcode_fingerprint"]=fingerprint(result.output.path);size_t sources=0,returns=0,resin=0,max_contours=0;
    for (const auto& diagnostic:result.preview->fiber_fill_diagnostics)
        if (diagnostic.contour && diagnostic.kind==libslicer::FiberDiagnosticKind::RejectedPath &&
            diagnostic.reason.rfind("contour_rounding_unresolved",0)==0) {
            report["passed"]=false;
            report["failures"].push_back({{"reason","unresolved_outer_contour"},
                {"layer",diagnostic.layer_index+1},{"component",diagnostic.component_id},
                {"detail",diagnostic.reason}});
        }
    for(const auto& segment:result.preview->segments)if(segment.extrusion_role==libslicer::ToolpathExtrusionRole::ResinInfill && segment.deposition==libslicer::ToolpathDepositionKind::Thermoplastic)++resin;
    const double budget=std::stod(effective.at("fiber_landing_length").get<std::string>())+std::stod(effective.at("fiber_start_stabilization_length").get<std::string>())+
        std::stod(effective.at("fiber_minimum_effective_length").get<std::string>())+std::stod(effective.at("fiber_cut_to_contact_length").get<std::string>());
    std::set<std::pair<size_t,size_t>> seen;
    for(const auto& source:result.preview->fiber_fill_diagnostics)if(source.kind==libslicer::FiberDiagnosticKind::OriginalContourRegion) {
        const size_t layer=source.layer_index+1;++sources;require(seen.emplace(layer,source.component_id).second,"Duplicate source region");require(!source.boundaries.empty(),"Missing source region");
        Slic3r::ExPolygon area;
        for(size_t i=0;i<source.boundaries.size();++i){Slic3r::Polygon ring;for(const auto& p:source.boundaries[i])ring.points.push_back(Slic3r::Point::new_scale(p.x,p.y));if(i==0)area.contour=std::move(ring);else area.holes.push_back(std::move(ring));}
        Slic3r::ExPolygons occupied;std::vector<GapPath> paths;double central=0;size_t contour_count=0;
        bool central_short_rejection=false;
        const auto window=Slic3r::ExPolygons{Slic3r::ExPolygon(Slic3r::Polygon(integer_path({{183,165},{199,165},{199,176},{183,176}}).points))};
        for(const auto& block:blocks)if(block.layer==layer && block.component==source.component_id) {
            if(block.contour){Slic3r::append(occupied,reference_sweep(block.points,.5));++contour_count;}
            else {paths.push_back({block.points,true});for(const auto& part:Slic3r::intersection_pl(Slic3r::Polylines{integer_path(block.points)},window))central+=Slic3r::unscale<double>(part.length());}
        }
        for(const auto& d:result.preview->fiber_fill_diagnostics)if(d.layer_index+1==layer && d.component_id==source.component_id && !d.contour && d.kind==libslicer::FiberDiagnosticKind::RejectedPath) {
            Path points;for(auto p:d.points)points.push_back({p.x,p.y});check_short_rejection(points,d.reason,std::stod(effective.at("fiber_minimum_path_length").get<std::string>()),budget);
            if(points.size()>1 && !Slic3r::intersection_pl(Slic3r::Polylines{integer_path(points)},window).empty())central_short_rejection=true;
        }
        const auto keepout=Slic3r::intersection_ex(Slic3r::union_ex(occupied),Slic3r::ExPolygons{area},Slic3r::ApplySafetyOffset::Yes);
        const auto domain=reference_offset(Slic3r::diff_ex(Slic3r::ExPolygons{area},keepout,Slic3r::ApplySafetyOffset::Yes),-.5);
        float angle=float(45*PI/180);if((layer-1)&1)angle+=float(PI/2);angle+=float(PI/2);
        max_contours=std::max(max_contours,contour_count);
        auto row=check_rounded_domain(domain,paths,angle);row["layer"]=layer;row["component"]=source.component_id;row["contours"]=contour_count;row["central_accepted_length_mm"]=central;row["central_short_rejection"]=central_short_rejection;returns+=row["returns"].get<size_t>();
        if(expected_layers==32 && source.component_id==0 && (paths.empty() || !contour_count || (central<=0 && !(layer==24 && central_short_rejection)))) {row["passed"]=false;row["failures"].push_back("main_and_central_region_require_actual_fiber");}
        if(!row["passed"].get<bool>())report["passed"]=false;report["layers"].push_back(std::move(row));
    }
    for(const auto& block:blocks)require(seen.count({size_t(block.layer),block.component}),"Depositing path without source region");
    if(expected_layers==32)for(size_t layer=4;layer<=24;++layer){std::vector<size_t> components;for(auto key:seen)if(key.first==layer)components.push_back(key.second);check_components(components,layer==24?65:1);}
    require(sources>0 && returns>0 && resin>0,"Rounded acceptance requires source regions, real fiber returns and resin leftovers");
    report["max_contours_per_source"]=max_contours;
    if (max_contours<size_t(contours)) {
        report["passed"]=false;
        report["failures"].push_back("requested_contour_count_not_deposited");
    }
    report["returns"]=returns;report["resin_segments"]=resin;write_json(output/"report.json",report);
    std::cout<<(report["passed"].get<bool>()?"PASS":"FAIL")<<" rounded model layers="<<expected_layers<<" returns="<<returns<<" report="<<(output/"report.json")<<'\n';return report["passed"].get<bool>()?0:1;
}


// Frozen application input. Diagnostics are exported for the independent
// G-code material-envelope checker (which does not call the production planner).
int resin_model(const fs::path& fixture, const fs::path& output, const fs::path& overrides)
{
    fs::create_directories(output);
    auto config=read_json(fixture/"config.json");
    const fs::path model=fixture/config.at("model").get<std::string>();
    if(!overrides.empty()){const auto changes=read_json(overrides);for(auto it=changes.begin();it!=changes.end();++it)config["settings"][it.key()]=it.value();}
    const auto& selection=config.at("selection");
    libslicer::LibraryOptions options;options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"CFSYS"};
    auto library=libslicer::Library::open(options);libslicer::ConfigSelection selected;
    selected.machine_model_id=selection.at("machine_model_id");selected.machine_variant_id=selection.at("machine_variant_id");selected.process_preset_id=selection.at("process_preset_id");
    selected.filament_preset_ids=selection.at("filament_preset_ids").get<std::vector<std::string>>();selected.filament_physical_tools=selection.at("filament_physical_tools").get<std::vector<unsigned>>();
    require(library->activate_config(selected).success,"Cannot activate resin regression presets");
    const auto before=*library->active_config_snapshot();const auto items=library->active_config()->settings;
    std::vector<std::pair<std::string,std::string>> patch;
    for(auto it=config["settings"].begin();it!=config["settings"].end();++it) {
        const auto old=before.value(it.key());
        if(old && *old!=it.value().get<std::string>() && std::any_of(items.begin(),items.end(),[&](const auto& item){return item.key==it.key();}))patch.emplace_back(it.key(),it.value().get<std::string>());
    }
    const auto applied=library->apply_active_config_patch(patch);
    for(const auto& d:applied.diagnostics)std::cerr<<d.key<<": "<<d.message<<'\n';
    require(applied.success,"Resin regression config rejected");
    libslicer::SliceRequest request;request.config=*library->active_config_snapshot();request.center_on_build_plate=false;
    Json effective=Json::object();for(const auto& item:request.config.values())effective[item.first]=item.second;
    write_json(output/"effective_config.json",effective);
    for(const auto& item:patch)require(effective[item.first]==item.second,"Effective parameter mismatch: "+item.first);
    request.objects.push_back(read_model(model));request.output_gcode_path=(output/"model.gcode").string();
    const auto started=std::chrono::steady_clock::now();const auto result=library->slice(request);
    Json report={{"slice_success",result.success},{"slice_seconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()},{"diagnostics",Json::array()}};
    for(const auto& d:result.diagnostics)report["diagnostics"].push_back({{"code",d.code},{"message",d.message}});
    write_json(output/"report.json",report);require(result.success && result.preview,"Resin regression slice failed");
    report["layer_count"]=result.preview->layers.size();report["model_fingerprint"]=fingerprint(model);
    Json domains=Json::array();
    for(const auto& d:result.preview->fiber_fill_diagnostics) {
        Json row={{"layer",d.layer_index+1},{"kind",int(d.kind)},{"reason",d.reason},{"component",d.component_id},{"contour",d.contour},{"source_length_mm",d.source_length_mm},{"boundaries",Json::array()},{"points",Json::array()}};
        for(auto p:d.points)row["points"].push_back({p.x,p.y});
        for(const auto& boundary:d.boundaries){Json ring=Json::array();for(auto p:boundary)ring.push_back({p.x,p.y});row["boundaries"].push_back(ring);}
        domains.push_back(std::move(row));
    }
    write_json(output/"domains.json",domains);write_json(output/"report.json",report);
    std::cout<<"Slice complete: "<<output<<" ("<<report["slice_seconds"]<<" s)\n";return 0;
}

} // namespace

int main(int argc,char** argv)
{
    const fs::path assets=fs::path(LIBSLICER_TEST_DATA_DIR)/"continuous_fiber/4xiao";
    const bool gap_current=argc==2 && std::string(argv[1])=="--infill-gap-current";
    const bool gap_checking=argc==2 && std::string(argv[1])=="--infill-gap-self-test";
    const bool gap=gap_current || (argc==2 && std::string(argv[1])=="--infill-gap");
    const bool closed_outer_regression=argc==2 && std::string(argv[1])=="--closed-outer-regression";
    const fs::path output=argc==6 && std::string(argv[1])=="--rounded-model" ? fs::path(argv[5]) : gap ? fs::path(FIBER_ACCEPTANCE_OUTPUT_DIR)/(gap_current?"infill-gap-current":"infill-gap") : closed_outer_regression ? fs::path(FIBER_ACCEPTANCE_OUTPUT_DIR)/"closed-outer-regression" :
        fs::path(FIBER_ACCEPTANCE_OUTPUT_DIR);
    const bool checking=argc==2 && std::string(argv[1])=="--self-test";
    try {
        if((argc==4 || argc==5) && std::string(argv[1])=="--resin-model")return resin_model(argv[2],argv[3],argc==5?fs::path(argv[4]):fs::path{});
        if(argc==2 && std::string(argv[1])=="--rounded-self-test") {rounded_checker_self_test();return 0;}
        if(argc==6 && std::string(argv[1])=="--rounded-model")
            return rounded_model_acceptance(argv[2],std::stoul(argv[3]),std::stoi(argv[4]),argv[5]);
        require(checking || closed_outer_regression || gap || gap_checking,"Usage: fiber_4xiao_acceptance [--self-test|--closed-outer-regression|--infill-gap|--infill-gap-current|--infill-gap-self-test]");
        require(SCALING_FACTOR==1e-6,"Update the frozen integer coordinate budget");
        Json rule=read_json(assets/"expectations.json");
        require(rule.at("schema_version")==1 && rule.at("first_required_layer")==4 && rule.at("last_required_layer")==24 && rule.at("outer_contours_per_layer")==1,"Unexpected acceptance contract");
        if (checking) {self_test(rule);return 0;}
        if(gap_checking){gap_checker_self_test();return 0;}
        // Check the user's current settings and the transition from a blocked
        // exterior passage to an open concavity.
        if (closed_outer_regression) {
            rule["first_required_layer"]=14;
            rule["last_required_layer"]=17;
            rule["notes"]=Json::array({"Closed-outer regression: 0.05 mm clearance, hole loops disabled; display layers 14..17 must retain one complete main outer around both left bores."});
        }
        fs::create_directories(output);
        write_json(output/"report.json",{{"passed",false},{"status","running"}});
        const fs::path model=assets/rule.at("model").get<std::string>();
        require(fingerprint(model)==rule.at("model_fnv1a64").get<std::string>(),"Model fingerprint does not match the fixed fixture");
        const fs::path config_file=assets/rule.at("config").get<std::string>();
        Json config=read_json(config_file);
        if (closed_outer_regression) {
            config["settings"]["fiber_contour_boundary_clearance"]="0.05";
            config["settings"]["fiber_contour_include_holes"]="0";
        }
        if(gap) {
            config["settings"]["outer_reinforced_perimeters_counts"]="4";
            config["settings"]["generate_reinforced_infills"]="1";
            config["settings"]["reinforced_infill_density"]="100%";
            if(gap_current) {
                config["settings"]["fiber_contour_boundary_clearance"]="0.05";
                config["settings"]["fiber_contour_include_holes"]="0";
            }
        }
        const auto& selection=config.at("selection");
        libslicer::LibraryOptions options;options.resource_directory=LIBSLICER_TEST_RESOURCE_DIR;options.vendors={"CFSYS"};
        auto library=libslicer::Library::open(options);require(bool(library),"Cannot open slicer library");
        libslicer::ConfigSelection selected;
        selected.machine_model_id=selection.at("machine_model_id");selected.machine_variant_id=selection.at("machine_variant_id");selected.process_preset_id=selection.at("process_preset_id");
        selected.filament_preset_ids=selection.at("filament_preset_ids").get<std::vector<std::string>>();selected.filament_physical_tools=selection.at("filament_physical_tools").get<std::vector<unsigned>>();
        require(library->activate_config(selected).success,"Cannot activate pinned machine and material selection");
        const auto before=*library->active_config_snapshot();const auto items=library->active_config()->settings;
        std::vector<std::pair<std::string,std::string>> patch;
        for (auto it=config.at("settings").begin();it!=config.at("settings").end();++it) {
            const auto old=before.value(it.key());require(bool(old),"Pinned setting no longer exists: "+it.key());
            if (*old!=it.value().get<std::string>() && std::any_of(items.begin(),items.end(),[&](const auto& item){return item.key==it.key();})) patch.emplace_back(it.key(),it.value().get<std::string>());
        }
        const auto applied=library->apply_active_config_patch(patch);
        for (const auto& d:applied.diagnostics) std::cerr<<d.key<<": "<<d.message<<'\n';
        require(applied.success,"Pinned configuration was rejected");
        libslicer::SliceRequest request;request.config=*library->active_config_snapshot();
        Json effective=Json::object();for(const auto& item:request.config.values())effective[item.first]=item.second;
        write_json(output/"effective_config.json",effective);
        require(effective==config.at("settings"),"Effective configuration differs from pinned full snapshot; inspect effective_config.json");
        if(gap)request.center_on_build_plate=false;
        request.objects.push_back(read_model(model));request.output_gcode_path=(output/"4xiao.gcode").string();
        const auto start=std::chrono::steady_clock::now();const auto result=library->slice(request);
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        Json diagnostics=Json::array();for(const auto& d:result.diagnostics)diagnostics.push_back({{"code",d.code},{"message",d.message}});
        write_json(output/"slice_diagnostics.json",diagnostics);
        require(result.success && result.preview,"Full-model slice failed");
        require(result.preview->layers.size()==rule.at("layer_count").get<size_t>(),"Unexpected layer count");
        std::vector<Candidate> candidates;Json trace=Json::array();
        for (const auto& d:result.preview->fiber_fill_diagnostics) {
            Json record={{"layer",d.layer_index+1},{"kind",int(d.kind)},{"reason",d.reason},{"source_length_mm",d.source_length_mm},{"contour",d.contour},{"component",d.component_id},{"policy",d.policy_group_id},{"boundaries",Json::array()},{"points",Json::array()}};
            for(auto p:d.points)record["points"].push_back({p.x,p.y});
            for(const auto& boundary:d.boundaries) {Json ring=Json::array();for(auto p:boundary)ring.push_back({p.x,p.y});record["boundaries"].push_back(std::move(ring));}
            trace.push_back(record);
            if (d.kind==libslicer::FiberDiagnosticKind::ContourCandidate) {
                require(d.reason.rfind("outer;",0)==0 || d.reason.rfind("hole;",0)==0,"Unknown contour origin in diagnostics");
                Candidate candidate{int(d.layer_index)+1,d.reason.rfind("outer;",0)==0,{}};
                for(auto p:d.points)candidate.points.push_back({p.x,p.y});candidates.push_back(std::move(candidate));
            }
        }
        write_json(output/"contour_diagnostics.json",trace);
        std::vector<Point> offsets;std::istringstream offset_text(effective.at("extruder_offset").get<std::string>());
        for(std::string pair;std::getline(offset_text,pair,',');) {const auto x=pair.find('x');require(x!=std::string::npos,"Invalid tool offset");offsets.push_back({std::stod(pair.substr(0,x)),std::stod(pair.substr(x+1))});}
        std::ifstream gcode(result.output.path);require(bool(gcode),"Missing output G-code");
        const auto blocks=read_blocks(gcode,offsets,gap);
        if(gap) {
            Json report=evaluate_gap(*result.preview,blocks,effective);
            report["slice_seconds"]=seconds;report["model_fingerprint"]=fingerprint(model);report["gcode_fingerprint"]=fingerprint(result.output.path);
            report["effective_config"]=effective;report["case"]=gap_current?"infill_gap_current":"infill_gap";
            write_json(output/"report.json",report);
            for(const auto& row:report["layers"])std::cout<<(row["passed"].get<bool>()?"PASS":"FAIL")<<" layer "<<row["layer"]<<" regions="<<row["regions"].size()<<'\n';
            std::cout<<"Report: "<<(output/"report.json")<<'\n';
            return report["passed"].get<bool>()?0:1;
        }
        if (closed_outer_regression)
            for (const auto& block : blocks)
                require(block.points.size()>=4 && distance(block.points.front(),block.points.back())<=rule.at("closure_tolerance_mm").get<double>(),
                    "Open outer deposition on display layer " + std::to_string(block.layer));
        Json report=evaluate(rule,blocks,candidates);
        report["case"]="closed_outer_regression";
        report["config_overrides"]=closed_outer_regression ? Json{{"fiber_contour_boundary_clearance","0.05"},{"fiber_contour_include_holes","0"}} : Json::object();
        report["contract"]=rule;report["config_fingerprint"]=fingerprint(config_file);report["gcode_fingerprint"]=fingerprint(result.output.path);
        report["binary_fingerprint"]=fingerprint(fs::absolute(argv[0]));report["slice_seconds"]=seconds;report["gcode"]=result.output.path;
        report["status"]=report.at("passed").get<bool>()?"passed":"failed";
        write_json(output/"report.json",report);
        for(const auto& row:report.at("layers")) if(row.at("required").get<bool>())
            std::cout<<(row.at("passed").get<bool>()?"PASS":"FAIL")<<" layer "<<row.at("layer")<<" outer="<<row.at("outer_count")<<" main="<<row.at("main_outer_count")<<" "<<row.at("failures").dump()<<'\n';
        std::cout<<"Report: "<<(output/"report.json")<<'\n';
        return report.at("passed").get<bool>()?0:1;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<'\n';
        if(!checking) {fs::create_directories(output);write_json(output/"report.json",{{"passed",false},{"status","error"},{"error",error.what()}});}
        return 2;
    }
}
