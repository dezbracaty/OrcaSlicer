#include "BeltModelVolume.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {

BeltModelSlab BeltModelVolume::slice_slab(
    double center_s, double predecessor_thickness,
    double xy_clearance) const
{
    BeltModelSlab result;
    const double thickness = std::max(0.0, predecessor_thickness);
    const Layer* closest = nullptr;
    double closest_distance = std::numeric_limits<double>::max();

    for (const Layer* layer : m_object.layers()) {
        const double distance = std::abs(layer->slice_z - center_s);
        if (distance < closest_distance) {
            closest_distance = distance;
            closest = layer;
        }
        // A support section is connected to its predecessor at smaller S.
        // Sampling the following (not-yet-reached) model layer would erase a
        // legitimate top contact through the configured Z gap.
        const double layer_half_height = 0.5 * std::max(0.0, layer->height);
        const bool intersects_predecessor_slab =
            layer->slice_z <= center_s + EPSILON &&
            layer->slice_z + layer_half_height >=
                center_s - thickness - EPSILON;
        if (!intersects_predecessor_slab ||
            layer->lslices.empty()) {
            continue;
        }
        result.regions.insert(result.regions.end(),
                              layer->lslices.begin(), layer->lslices.end());
        ++result.source_layer_count;
    }

    // A very thin query between numerical layer centres still needs a model
    // section. Keep the old tolerance semantics only as a fallback; normal
    // routing uses the full adjacent-layer slab above.
    if (result.regions.empty() && closest != nullptr &&
        closest->slice_z <= center_s + EPSILON &&
        closest_distance <= thickness + EPSILON && !closest->lslices.empty()) {
        result.regions = closest->lslices;
        result.source_layer_count = 1;
    }

    if (result.regions.empty())
        return result;
    result.regions = union_ex(result.regions);
    if (xy_clearance > EPSILON)
        result.regions = offset_ex(
            result.regions, scale_(xy_clearance), jtRound);
    return result;
}

} // namespace Slic3r
