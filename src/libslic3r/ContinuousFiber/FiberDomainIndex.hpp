#ifndef slic3r_FiberDomainIndex_hpp_
#define slic3r_FiberDomainIndex_hpp_

// Internal continuous-fiber integer geometry. No SDK or process state.
#include "../AABBTreeLines.hpp"
#include "../ExPolygon.hpp"
#include "../Int128.hpp"
#include "../clipper.hpp"
#include <algorithm>
#include <tuple>

namespace Slic3r { namespace continuous_fiber_detail {
enum class DomainSide { Outside, Boundary, Inside, Uncertain };
class FiberDomainIndex {
public:
    Lines boundaries;
    Linesf edges;
    AABBTreeIndirect::Tree<2,double> tree;
    BoundingBox bounds;
    bool empty_domain, supported=true;
    explicit FiberDomainIndex(const ExPolygons& domain) : bounds(get_extents(domain)),empty_domain(domain.empty()) {
        for(const auto* ring:to_polygon_ptrs(domain)) for(const Point& p:ring->points)
            if(!supports_point(p)) { supported=false;return; }
        for(const auto* ring:to_polygon_ptrs(domain)) for(const auto& line:ring->lines()) {
            boundaries.push_back(line);edges.emplace_back(line.a.cast<double>(),line.b.cast<double>());
        }
        tree=AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    }
    static bool supports_point(const Point& p) {
        return std::abs(double(p.x()))<=ClipperLib::loRange && std::abs(double(p.y()))<=ClipperLib::loRange;
    }
    bool supports(const Polyline& path) const {
        return supported && std::all_of(path.points.begin(),path.points.end(),supports_point);
    }
    // All tree geometry is in integer grid units, exactly representable here.
    // A one-grid outward box guard is broad phase only; predicates use full edges.
    template<class Callback> void query(double min_x,double min_y,double max_x,double max_y,Callback callback) const {
        if(min_x>max_x || min_y>max_y) return;
        const Eigen::AlignedBox<double,2> box(Vec2d(min_x,min_y)-Vec2d::Constant(1),Vec2d(max_x,max_y)+Vec2d::Constant(1));
        AABBTreeIndirect::traverse(tree,AABBTreeIndirect::intersecting(box),[&](const auto& node){return callback(boundaries[node.idx]);});
    }
    DomainSide side_doubled(const Point& p) const {
        if (empty_domain) return DomainSide::Outside;
        if(p.x()<2*bounds.min.x() || p.x()>2*bounds.max.x() || p.y()<2*bounds.min.y() || p.y()>2*bounds.max.y()) return DomainSide::Outside;
        int winding=0;bool boundary=false;
        const double x=double(p.x())*.5, y=double(p.y())*.5;
        query(x,y,double(bounds.max.x()),y,[&](const Line& edge) {
            const Point a=2*edge.a,b=2*edge.b;
            const int sign=Int128::sign_determinant_2x2(b.x()-a.x(),b.y()-a.y(),p.x()-a.x(),p.y()-a.y());
            if (!sign && p.x()>=std::min(a.x(),b.x()) && p.x()<=std::max(a.x(),b.x()) &&
                p.y()>=std::min(a.y(),b.y()) && p.y()<=std::max(a.y(),b.y())) { boundary=true;return false; }
            if(a.y()<=p.y() && b.y()>p.y() && sign>0) ++winding;
            if(b.y()<=p.y() && a.y()>p.y() && sign<0) --winding;
            return true;
        });
        return boundary?DomainSide::Boundary:winding?DomainSide::Inside:DomainSide::Outside;
    }
    // Sufficient containment certificate only. Any boundary contact, unsupported
    // input, or degenerate route is unproved and retains the caller's clipping check.
    bool proves_strict_containment(const Polyline& path) const {
        if (empty_domain || !supports(path) || path.points.size()<2 ||
            side_doubled(2*path.points.front())!=DomainSide::Inside) return false;
        bool nonzero=false;
        const auto orient=[](const Point& a,const Point& b,const Point& p) {
            return Int128::sign_determinant_2x2(b.x()-a.x(),b.y()-a.y(),p.x()-a.x(),p.y()-a.y());
        };
        const auto on=[](const Point& a,const Point& b,const Point& p) {
            return p.x()>=std::min(a.x(),b.x()) && p.x()<=std::max(a.x(),b.x()) &&
                   p.y()>=std::min(a.y(),b.y()) && p.y()<=std::max(a.y(),b.y());
        };
        for(size_t i=1;i<path.points.size();++i) {
            const Point& a=path.points[i-1];const Point& b=path.points[i];
            if(a==b) continue;
            nonzero=true;bool contact=false;
            query(double(std::min(a.x(),b.x())),double(std::min(a.y(),b.y())),
                  double(std::max(a.x(),b.x())),double(std::max(a.y(),b.y())),[&](const Line& edge) {
                const int p=orient(a,b,edge.a),q=orient(a,b,edge.b),
                          r=orient(edge.a,edge.b,a),t=orient(edge.a,edge.b,b);
                contact=(p*q<0 && r*t<0) || (!p && on(a,b,edge.a)) ||
                        (!q && on(a,b,edge.b)) || (!r && on(edge.a,edge.b,a)) ||
                        (!t && on(edge.a,edge.b,b));
                return !contact;
            });
            if(contact) return false;
        }
        return nonzero;
    }
    // Sufficient query certificate over native canonical planned C only.
    bool proves_sample_band(const Polyline& path,double radius_grid) const;
    bool permits(const Polyline& path,bool occupied) const {
        const auto allowed=[&](DomainSide side) {
            return side==DomainSide::Boundary || side==(occupied?DomainSide::Outside:DomainSide::Inside);
        };
        if(path.points.size()<2 || std::all_of(path.points.begin()+1,path.points.end(),[&](const Point& p){return p==path.points.front();})) return false;
        if(empty_domain) return occupied;
        for(size_t i=1;i<path.points.size();++i) {
            const Point a=path.points[i-1],b=path.points[i];
            if(a==b) continue;
            Points events{a,b};bool crossing=false;
            const auto orient=[](const Point& p,const Point& q,const Point& r) {
                return Int128::sign_determinant_2x2(q.x()-p.x(),q.y()-p.y(),r.x()-p.x(),r.y()-p.y());
            };
            const auto on=[&](const Point& p) {
                return orient(a,b,p)==0 && p.x()>=std::min(a.x(),b.x()) && p.x()<=std::max(a.x(),b.x()) &&
                    p.y()>=std::min(a.y(),b.y()) && p.y()<=std::max(a.y(),b.y());
            };
            const Vec2d am=a.cast<double>(),bm=b.cast<double>();
            query(std::min(am.x(),bm.x()),std::min(am.y(),bm.y()),std::max(am.x(),bm.x()),std::max(am.y(),bm.y()),[&](const Line& edge) {
                const int p=orient(a,b,edge.a),q=orient(a,b,edge.b),r=orient(edge.a,edge.b,a),s=orient(edge.a,edge.b,b);
                if(p*q<0 && r*s<0) { crossing=true;return false; }
                if(on(edge.a)) events.push_back(edge.a);
                if(on(edge.b)) events.push_back(edge.b);
                return true;
            });
            if(crossing) return false;
            const bool use_x=std::abs(b.x()-a.x())>=std::abs(b.y()-a.y());
            std::sort(events.begin(),events.end(),[&](const Point& p,const Point& q) {
                return use_x?std::tie(p.x(),p.y())<std::tie(q.x(),q.y()):std::tie(p.y(),p.x())<std::tie(q.y(),q.x());
            });
            events.erase(std::unique(events.begin(),events.end()),events.end());
            for(size_t j=0;j<events.size();++j) {
                if(!allowed(side_doubled(2*events[j]))) return false;
                if(j && !allowed(side_doubled(events[j-1]+events[j]))) return false;
            }
        }
        return true;
    }
};
} } // namespace Slic3r::continuous_fiber_detail
#endif
