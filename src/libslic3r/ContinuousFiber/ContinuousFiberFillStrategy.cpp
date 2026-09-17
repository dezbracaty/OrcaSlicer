#include "ContinuousFiberFillStrategy.hpp"

#include "../ClipperUtils.hpp"

namespace Slic3r {

ExPolygons ContinuousFiberFillStrategy::build_infill_domain(
    const ExPolygons& original_area,
    const ExPolygons& contour_to_infill_keepout)
{
    if (contour_to_infill_keepout.empty())
        return original_area;
    return diff_ex(original_area, contour_to_infill_keepout, ApplySafetyOffset::Yes);
}

ExPolygons ContinuousFiberFillStrategy::build_resin_area(
    const ExPolygons& original_area,
    const ExPolygons& accepted_fiber_resin_exclusion)
{
    if (accepted_fiber_resin_exclusion.empty())
        return original_area;
    return diff_ex(original_area, accepted_fiber_resin_exclusion, ApplySafetyOffset::Yes);
}

} // namespace Slic3r
