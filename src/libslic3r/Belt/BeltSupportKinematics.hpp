#ifndef slic3r_BeltSupportKinematics_hpp_
#define slic3r_BeltSupportKinematics_hpp_

#include "BeltCoordinateSystem.hpp"

namespace Slic3r {

// Exact intersection of a world-vertical branch cone with two consecutive
// belt slicing planes. The displacement is measured from an upper branch
// centre to a predecessor on the lower physical-S plane.
struct BeltWorldMoveKernel
{
    double delta_s{0.0};
    double center_v{0.0};
    double radius_u{0.0};
    double radius_v{0.0};
    bool bounded{false};
};

class BeltSupportKinematics
{
public:
    BeltSupportKinematics(const BeltCoordinateSystem& coordinates,
                          double maximum_world_angle_degrees);

    double maximum_world_angle_degrees() const noexcept
    {
        return m_maximum_world_angle_degrees;
    }
    double maximum_world_horizontal_slope() const noexcept
    {
        return m_maximum_world_horizontal_slope;
    }

    // A world-vertical branch expressed as lower-minus-upper displacement in
    // oriented U/V coordinates per millimetre of physical-S descent.
    double world_vertical_predecessor_v_per_s() const noexcept;
    double world_vertical_u_radius_per_s() const noexcept;

    BeltWorldMoveKernel predecessor_kernel(double delta_s) const noexcept;

    // du/dv are lower-minus-upper displacements. This predicate is the
    // authoritative acceptance contract and is intentionally evaluated after
    // converting the segment back to real-world growth.
    bool predecessor_is_valid(double du, double dv, double delta_s,
                              double slope_tolerance = 0.0) const noexcept;
    double predecessor_world_horizontal_slope(double du, double dv,
                                               double delta_s) const noexcept;

private:
    const BeltCoordinateSystem& m_coordinates;
    double m_maximum_world_angle_degrees{0.0};
    double m_maximum_world_horizontal_slope{0.0};
};

} // namespace Slic3r

#endif
