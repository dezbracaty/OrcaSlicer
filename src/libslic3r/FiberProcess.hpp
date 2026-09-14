#ifndef slic3r_FiberProcess_hpp_
#define slic3r_FiberProcess_hpp_
#include "PrintConfig.hpp"
#include "Polyline.hpp"
#include <memory>
#include <string>
namespace Slic3r {
enum class FiberPhase : unsigned char { ResinBase, Fiber, ResinRepair };
enum class FiberPurpose : unsigned char { Perimeter, Infill, ConcentricAll };
enum class FiberEventKind : unsigned char { Start, Cut, Tail, Finish };
struct FiberSource {
    size_t object = 0, region = 0, layer = 0, surface = 0;
    size_t island = 0, recipe_ordinal = 0, candidate = 0;
    std::string id() const;
};
struct FiberConfig {
    PrintRegionConfig region;
    unsigned material = 0, resin_material = 0, tool = 0, resin_tool = 0;
    double width_mm = 0.8, perimeter_width_mm = 0.8, resin_width_mm = 0.4;
    double nozzle_mm = 0.4, resin_nozzle_mm = 0.4, height_mm = 0.2, flow_ratio = 1;
    double length_budget_mm() const;
};
struct FiberCurveSegment {
    Point start, end;
    Vec2d center_mm = Vec2d::Zero();
    double sweep_rad = 0, length_mm = 0;
    bool arc = false;
    Point at(double distance_mm) const;
    FiberCurveSegment portion(double begin_mm, double end_mm) const;
    void reverse();
};
struct FiberEvent { FiberEventKind kind; double distance_mm = 0; size_t segment_index = 0; };
struct PreparedFiberPath {
    FiberSource source;
    FiberPurpose purpose = FiberPurpose::Infill;
    bool source_is_fiber_perimeter = false, perimeter_process = false;
    FiberConfig config;
    std::vector<FiberCurveSegment> segments;
    std::vector<FiberEvent> events;
    Polyline display;
    Point entry, exit;
    double length_mm = 0, cut_distance_mm = 0;
    size_t cut_index = 0;
};
struct FiberAction {
    Point point;
    double z_offset_mm = 0, e_mm = 0, speed_mm_s = 0, distance_mm = 0;
    size_t segment_index = 0;
    FiberEventKind event = FiberEventKind::Start;
    bool motion = true, event_only = false, tail = false;
};
struct FiberProcessPlan {
    std::vector<FiberAction> actions;
    Point exit;
    double total_e_mm = 0;
};
// Input curves and their legacy process indices are immutable after this call.
std::shared_ptr<const PreparedFiberPath> prepare_fiber_path(const Polyline&, const FiberConfig&, const FiberSource&,
    FiberPurpose, bool source_is_fiber_perimeter, bool use_arachne, std::string& rejection);
FiberProcessPlan compile_fiber_process(const PreparedFiberPath&);
Polylines filter_fiber_legacy(Polyline& input, double threshold_mm, int type, Polylines& retained);
}
#endif
