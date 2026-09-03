#include "BeltSupportKinematics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Slic3r {

BeltSupportKinematics::BeltSupportKinematics(
    const BeltCoordinateSystem& coordinates,
    double maximum_world_angle_degrees)
    : m_coordinates(coordinates)
    , m_maximum_world_angle_degrees(maximum_world_angle_degrees)
{
    if (!std::isfinite(maximum_world_angle_degrees) ||
        maximum_world_angle_degrees < 0.0 ||
        maximum_world_angle_degrees >= 90.0) {
        throw std::invalid_argument(
            "Belt support branch angle must be finite and in [0, 90) degrees");
    }
    m_maximum_world_horizontal_slope = std::tan(
        maximum_world_angle_degrees * std::acos(-1.0) / 180.0);
}

double BeltSupportKinematics::world_vertical_predecessor_v_per_s() const noexcept
{
    return -m_coordinates.sin_angle() / m_coordinates.cos_angle();
}

double BeltSupportKinematics::world_vertical_u_radius_per_s() const noexcept
{
    return m_maximum_world_horizontal_slope / m_coordinates.cos_angle();
}

BeltWorldMoveKernel BeltSupportKinematics::predecessor_kernel(
    double delta_s) const noexcept
{
    BeltWorldMoveKernel result;
    result.delta_s = std::max(0.0, delta_s);

    const double sine = m_coordinates.sin_angle();
    const double cosine = m_coordinates.cos_angle();
    const double slope_squared = m_maximum_world_horizontal_slope *
        m_maximum_world_horizontal_slope;
    const double a = cosine * cosine - slope_squared * sine * sine;
    const double b = 2.0 * sine * cosine * (1.0 + slope_squared);
    const double c = sine * sine - slope_squared * cosine * cosine;
    result.bounded = a > 1e-12;
    if (!result.bounded) {
        // At and above the critical angle (90 degrees - gantry angle), the
        // exact conic section is unbounded in V. The caller must add a
        // stability bound instead of silently treating it as an ellipse.
        result.center_v = world_vertical_predecessor_v_per_s() * result.delta_s;
        result.radius_u = world_vertical_u_radius_per_s() * result.delta_s;
        result.radius_v = std::numeric_limits<double>::infinity();
        return result;
    }

    const double center_v_per_s = -b / (2.0 * a);
    const double radius_u_squared = std::max(
        0.0, b * b / (4.0 * a) - c);
    result.center_v = center_v_per_s * result.delta_s;
    result.radius_u = std::sqrt(radius_u_squared) * result.delta_s;
    result.radius_v = std::sqrt(radius_u_squared / a) * result.delta_s;
    return result;
}

double BeltSupportKinematics::predecessor_world_horizontal_slope(
    double du, double dv, double delta_s) const noexcept
{
    if (!(delta_s > 0.0))
        return std::numeric_limits<double>::infinity();

    // World growth points from the lower predecessor to the upper node.
    const double growth_x = -du;
    const double growth_y =
        -m_coordinates.cos_angle() * dv -
        m_coordinates.sin_angle() * delta_s;
    const double growth_z =
        -m_coordinates.sin_angle() * dv +
        m_coordinates.cos_angle() * delta_s;
    if (!(growth_z > 0.0))
        return std::numeric_limits<double>::infinity();
    return std::hypot(growth_x, growth_y) / growth_z;
}

bool BeltSupportKinematics::predecessor_is_valid(
    double du, double dv, double delta_s, double slope_tolerance) const noexcept
{
    return predecessor_world_horizontal_slope(du, dv, delta_s) <=
        m_maximum_world_horizontal_slope + std::max(0.0, slope_tolerance);
}

} // namespace Slic3r
