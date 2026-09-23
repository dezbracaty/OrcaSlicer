// Fixed-model acceptance, independent of planner pass/fail decisions.
// Run via CTest -L fiber_acceptance; reports and G-code remain in the build tree.
#include <libslicer/Library.hpp>
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
struct Block { int layer=0, extruder=0; size_t line=0; Path points; };
struct Candidate { int layer=0; bool outer=false; Path points; };

// Read actual depositing G0/G1 moves. Finishing travel MUST NOT close a gap.
// This fixed case uses linear moves; unsupported motions fail instead of being ignored.
std::vector<Block> read_blocks(std::istream& input, const std::vector<Point>& offsets)
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
        } else if (line==";FIBER_LANDING_BEGIN" || line==";FIBER_START" || line==";FIBER_TAIL_BEGIN") {
            require(active,"Fiber phase outside a block");
            depositing=true;
            if (contour && block.points.empty()) block.points.push_back({xy.x+offsets[block.extruder].x,xy.y+offsets[block.extruder].y});
        } else if (line==";FIBER_DEPLETED" || line==";FIBER_FINISH_BEGIN") {
            depositing=false;
        } else if (line==";FIBER_END") {
            require(active,"Fiber end outside a block");
            if (contour) { require(block.points.size()>1,"Empty contour block");blocks.push_back(block); }
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
        if (opcode!="G92" && active && contour && depositing && distance(next,xy)>0)
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
} // namespace

int main(int argc,char** argv)
{
    const fs::path assets=fs::path(LIBSLICER_TEST_DATA_DIR)/"continuous_fiber/4xiao";
    const fs::path output=FIBER_ACCEPTANCE_OUTPUT_DIR;
    const bool checking=argc==2 && std::string(argv[1])=="--self-test";
    try {
        require(argc==1 || checking,"Usage: fiber_4xiao_acceptance [--self-test]");
        const Json rule=read_json(assets/"expectations.json");
        require(rule.at("schema_version")==1 && rule.at("first_required_layer")==4 && rule.at("last_required_layer")==24 && rule.at("outer_contours_per_layer")==1,"Unexpected acceptance contract");
        if (checking) {self_test(rule);return 0;}
        fs::create_directories(output);
        write_json(output/"report.json",{{"passed",false},{"status","running"}});
        const fs::path model=assets/rule.at("model").get<std::string>();
        require(fingerprint(model)==rule.at("model_fnv1a64").get<std::string>(),"Model fingerprint does not match the fixed fixture");
        const fs::path config_file=assets/rule.at("config").get<std::string>();
        const Json config=read_json(config_file);const auto& selection=config.at("selection");
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
        request.objects.push_back(read_model(model));request.output_gcode_path=(output/"4xiao.gcode").string();
        const auto start=std::chrono::steady_clock::now();const auto result=library->slice(request);
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        Json diagnostics=Json::array();for(const auto& d:result.diagnostics)diagnostics.push_back({{"code",d.code},{"message",d.message}});
        write_json(output/"slice_diagnostics.json",diagnostics);
        require(result.success && result.preview,"Full-model slice failed");
        require(result.preview->layers.size()==rule.at("layer_count").get<size_t>(),"Unexpected layer count");
        std::vector<Candidate> candidates;Json trace=Json::array();
        for (const auto& d:result.preview->fiber_fill_diagnostics) {
            Json record={{"layer",d.layer_index+1},{"kind",int(d.kind)},{"reason",d.reason},{"source_length_mm",d.source_length_mm},{"points",Json::array()}};
            for(auto p:d.points)record["points"].push_back({p.x,p.y});
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
        const auto blocks=read_blocks(gcode,offsets);Json report=evaluate(rule,blocks,candidates);
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
