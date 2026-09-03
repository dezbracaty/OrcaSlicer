#ifndef slic3r_BeltModelVolume_hpp_
#define slic3r_BeltModelVolume_hpp_

#include "libslic3r/ExPolygon.hpp"

#include <cstddef>

namespace Slic3r {

class PrintObject;

struct BeltModelSlab
{
    ExPolygons regions;
    size_t source_layer_count{0};
};

// Conservative model occupancy in a physical-S slab. Unlike a closest-layer
// lookup, this preserves thin walls and fast-changing silhouettes that a belt
// branch can cross between two adjacent oriented layers.
class BeltModelVolume
{
public:
    explicit BeltModelVolume(const PrintObject& object) : m_object(object) {}

    BeltModelSlab slice_slab(double center_s, double predecessor_thickness,
                             double xy_clearance) const;

private:
    const PrintObject& m_object;
};

} // namespace Slic3r

#endif
