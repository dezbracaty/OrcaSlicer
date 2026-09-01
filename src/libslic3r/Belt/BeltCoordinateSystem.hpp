#ifndef slic3r_BeltCoordinateSystem_hpp_
#define slic3r_BeltCoordinateSystem_hpp_

#include "libslic3r/TriangleMeshSlicer.hpp"

namespace Slic3r {

class BeltCoordinateSystem
{
public:
    static BeltCoordinateSystem create(double angle_degrees, double plate_max_world_y);

    double angle_degrees() const noexcept { return m_angle_degrees; }
    double plate_max_world_y() const noexcept { return m_plate_max_world_y; }
    double sin_angle() const noexcept { return m_sin_angle; }
    double cos_angle() const noexcept { return m_cos_angle; }
    double cot_angle() const noexcept { return m_cos_angle / m_sin_angle; }

    const Vec3d& axis_u() const noexcept { return m_axis_u; }
    const Vec3d& axis_v() const noexcept { return m_axis_v; }
    const Vec3d& normal() const noexcept { return m_normal; }
    Vec3d world_origin() const noexcept { return Vec3d(0.0, m_plate_max_world_y, 0.0); }

    OrientedSliceFrame oriented_slice_frame() const noexcept;
    Vec3d world_to_oriented(const Vec3d& world) const noexcept;
    Vec3d oriented_to_world(const Vec3d& oriented) const noexcept;
    Vec3d oriented_to_machine(const Vec3d& oriented) const noexcept;
    Vec3d machine_to_world(const Vec3d& machine) const noexcept;

    double belt_boundary_v(double physical_s) const noexcept;
    double physical_s_to_machine_z(double physical_s) const noexcept;
    double machine_z_to_physical_s(double machine_z) const noexcept;

private:
    double m_angle_degrees{45.0};
    double m_plate_max_world_y{0.0};
    double m_sin_angle{0.0};
    double m_cos_angle{0.0};
    Vec3d m_axis_u{Vec3d::UnitX()};
    Vec3d m_axis_v{Vec3d::UnitY()};
    Vec3d m_normal{Vec3d::UnitZ()};
};

} // namespace Slic3r

#endif
