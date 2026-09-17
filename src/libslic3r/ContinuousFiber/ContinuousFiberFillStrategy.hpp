#ifndef slic3r_ContinuousFiberFillStrategy_hpp_
#define slic3r_ContinuousFiberFillStrategy_hpp_

#include "ContinuousFiberConfig.hpp"
#include "../ExPolygon.hpp"

namespace Slic3r {

class ContinuousFiberFillStrategy {
public:
    static ExPolygons build_infill_domain(
        const ExPolygons& original_area,
        const ExPolygons& contour_to_infill_keepout);

    static ExPolygons build_resin_area(
        const ExPolygons& original_area,
        const ExPolygons& accepted_fiber_resin_exclusion);
};

} // namespace Slic3r

#endif
