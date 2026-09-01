#include "BeltCoordinateSystem.hpp"

#include <cmath>
#include <stdexcept>

namespace Slic3r {

BeltCoordinateSystem BeltCoordinateSystem::create(double angle_degrees, double plate_max_world_y)
{
    if (!std::isfinite(angle_degrees) || angle_degrees <= 0.0 || angle_degrees >= 90.0)
        throw std::invalid_argument("Belt gantry angle must be finite and between 0 and 90 degrees");
    if (!std::isfinite(plate_max_world_y))
        throw std::invalid_argument("Belt plate maximum world Y must be finite");

    BeltCoordinateSystem result;
    result.m_angle_degrees = angle_degrees;
    result.m_plate_max_world_y = plate_max_world_y;
    const double radians = angle_degrees * std::acos(-1.0) / 180.0;
    result.m_sin_angle = std::sin(radians);
    result.m_cos_angle = std::cos(radians);
    if (std::abs(result.m_sin_angle) < 1e-8)
        throw std::invalid_argument("Belt gantry angle has an unusable sine");
    result.m_axis_u = Vec3d::UnitX();
    result.m_axis_v = Vec3d(0.0, result.m_cos_angle, result.m_sin_angle);
    result.m_normal = Vec3d(0.0, -result.m_sin_angle, result.m_cos_angle);
    return result;
}

OrientedSliceFrame BeltCoordinateSystem::oriented_slice_frame() const noexcept
{
    return {world_origin(), m_axis_u, m_axis_v, m_normal};
}

Vec3d BeltCoordinateSystem::world_to_oriented(const Vec3d& world) const noexcept
{
    const Vec3d relative = world - world_origin();
    return Vec3d(m_axis_u.dot(relative), m_axis_v.dot(relative), m_normal.dot(relative));
}

Vec3d BeltCoordinateSystem::oriented_to_world(const Vec3d& oriented) const noexcept
{
    return world_origin() + m_axis_u * oriented.x() + m_axis_v * oriented.y() + m_normal * oriented.z();
}

Vec3d BeltCoordinateSystem::oriented_to_machine(const Vec3d& oriented) const noexcept
{
    return Vec3d(oriented.x(), oriented.y() + oriented.z() * cot_angle(),
                 physical_s_to_machine_z(oriented.z()));
}

Vec3d BeltCoordinateSystem::machine_to_world(const Vec3d& machine) const noexcept
{
    return Vec3d(machine.x(),
                 m_plate_max_world_y + machine.y() * m_cos_angle - machine.z(),
                 machine.y() * m_sin_angle);
}

double BeltCoordinateSystem::belt_boundary_v(double physical_s) const noexcept
{
    return -physical_s * cot_angle();
}

double BeltCoordinateSystem::physical_s_to_machine_z(double physical_s) const noexcept
{
    return physical_s / m_sin_angle;
}

double BeltCoordinateSystem::machine_z_to_physical_s(double machine_z) const noexcept
{
    return machine_z * m_sin_angle;
}

} // namespace Slic3r
