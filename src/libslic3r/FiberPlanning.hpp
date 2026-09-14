#ifndef slic3r_FiberPlanning_hpp_
#define slic3r_FiberPlanning_hpp_
#include "FiberProcess.hpp"
#include "Surface.hpp"
#include "Flow.hpp"
#include "BoundingBox.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <functional>
namespace Slic3r {
class Layer; class LayerRegion; class Print; class PrintObject;
struct ResinRequest {
    std::vector<FiberSource> source_ids;
    size_t region = 0;
    unsigned material = 0;
    FiberPhase phase = FiberPhase::ResinBase;
    ExPolygons area;
    // Region-stage request before the legacy thick-pattern reservation.
    // area is the final resin obligation; retain this source for diagnostics.
    ExPolygons original_area;
    Flow fill_recipe;
    double angle_rad = 0, density = 1;
    std::string reason;
    // Requests created by the fiber planner have one explicit producer.  The
    // counter is retained with the request so layer validation can reject a
    // missing or duplicate producer instead of only checking final coverage.
    std::string request_id;
    bool requires_explicit_fill = false;
    size_t explicit_fill_count = 0;
    // A repair request owns the rejected centerline until all compatible
    // rejects in the same layer island have been unioned. Eroding once after
    // that union reproduces the legacy layer_t0_path batch boundary.
    Polylines repair_paths;
    coord_t repair_radius = 0;
    coord_t repair_gap = 0;
};
struct FiberRegionRecipe {
    FiberSource source;
    Surface original;
    Surface candidate;
    FiberConfig config;
    FiberPurpose purpose = FiberPurpose::Infill;
    bool source_is_fiber_perimeter = false;
    // Fill context captured before fiber surfaces leave Orca's normal
    // group_fills path.  Rectilinear generation must use the same reference
    // frame, clipping domain, and connection controls as an ordinary fill.
    BoundingBox fill_bounding_box;
    ExPolygons no_overlap_expolygons;
    const PrintConfig* print_config = nullptr;
    const PrintObjectConfig* print_object_config = nullptr;
    double print_z = 0;
    double resolution = 0.0125;
    double align_angle_rad = 0;
    float anchor_length = 1000;
    float anchor_length_max = 1000;
    bool dont_alternate_fill_direction = false;
    std::vector<ResinRequest> resin_requests;
};
struct FiberPathResult {
    Polylines candidates, rejected;
    std::vector<std::string> rejection_reasons;
    std::vector<std::shared_ptr<const PreparedFiberPath>> retained;
    std::vector<ResinRequest> resin_requests;
};
struct FiberVisit {
    size_t object = 0, instance = 0, visit_index = 0;
    FiberPhase phase = FiberPhase::ResinBase;
    unsigned material = 0, tool = 0;
    std::vector<size_t> root_ids;
};
bool fiber_active(const PrintRegionConfig&);
bool fiber_active(const Print&);
bool fiber_config_key(const std::string&);
FiberConfig resolve_fiber_config(const LayerRegion&, bool perimeter);
void validate_fiber_configuration(const Print&);
void prepare_fiber_regions(PrintObject&);
void prepare_fiber_fill_sources(Layer&);
void make_fiber_fills(Layer&);
void check_fiber_resin_coverage(const Layer&);
void audit_fiber_final_file(const Print&, const std::string&);
FiberPathResult plan_fiber_paths(const FiberRegionRecipe&, double angle_rad, bool fixed_angle,
    size_t layer_id, double seam_gap_mm, const std::function<void()>& cancel);
std::vector<FiberVisit> fiber_visits(const Layer&, size_t instance);
}
#endif
