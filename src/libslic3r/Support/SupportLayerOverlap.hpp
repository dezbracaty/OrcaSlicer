#ifndef slic3r_SupportLayerOverlap_hpp_
#define slic3r_SupportLayerOverlap_hpp_

#include "../libslic3r.h"
#include <cmath>

namespace Slic3r {

namespace SupportMaterialInternal {
// Shared by the actual clipping operation and its narrow boundary check. A
// support plane at (or within EPSILON above) the object's bottom is contact,
// not penetration into the object layer. Preserve the other overlap branches.
inline bool is_layers_overlap(coordf_t support_print_z, coordf_t support_bottom_z,
    coordf_t object_print_z, coordf_t object_height, coordf_t bridging_height = 0.f) noexcept
{
    if (std::abs(support_print_z - object_print_z) < EPSILON)
        return true;

    const coordf_t object_lh = bridging_height > EPSILON ? bridging_height : object_height;
    if (support_print_z < object_print_z && support_print_z > object_print_z - object_lh + EPSILON)
        return true;

    if (support_print_z > object_print_z && support_bottom_z < object_print_z - EPSILON)
        return true;

    return false;
}
} // namespace SupportMaterialInternal

} // namespace Slic3r

#endif /* slic3r_SupportLayerOverlap_hpp_ */
