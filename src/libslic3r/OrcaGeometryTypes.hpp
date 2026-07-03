#ifndef slic3r_OrcaGeometryTypes_hpp_
#define slic3r_OrcaGeometryTypes_hpp_

#include "OrcaCoreTypes.hpp"

#include <cstdint>

#include <Eigen/Geometry>

namespace Slic3r {

template<int N, int M, class T>
using Mat = Eigen::Matrix<T, N, M, Eigen::DontAlign, N, M>;

template<int N, class T>
using Vec = Mat<N, 1, T>;

using Vec2crd = Eigen::Matrix<coord_t,  2, 1, Eigen::DontAlign>;
using Vec3crd = Eigen::Matrix<coord_t,  3, 1, Eigen::DontAlign>;
using Vec2i32 = Eigen::Matrix<int32_t,  2, 1, Eigen::DontAlign>;
using Vec2i64 = Eigen::Matrix<int64_t,  2, 1, Eigen::DontAlign>;
using Vec3i32 = Eigen::Matrix<int32_t,  3, 1, Eigen::DontAlign>;
using Vec3i64 = Eigen::Matrix<int64_t,  3, 1, Eigen::DontAlign>;
using Vec4i32 = Eigen::Matrix<int32_t,  4, 1, Eigen::DontAlign>;

using Vec2f   = Eigen::Matrix<float,    2, 1, Eigen::DontAlign>;
using Vec3f   = Eigen::Matrix<float,    3, 1, Eigen::DontAlign>;
using Vec4f   = Eigen::Matrix<float,    4, 1, Eigen::DontAlign>;
using Vec2d   = Eigen::Matrix<double,   2, 1, Eigen::DontAlign>;
using Vec3d   = Eigen::Matrix<double,   3, 1, Eigen::DontAlign>;
using Vec4d   = Eigen::Matrix<double,   4, 1, Eigen::DontAlign>;

using Matrix2f = Eigen::Matrix<float,  2, 2, Eigen::DontAlign>;
using Matrix2d = Eigen::Matrix<double, 2, 2, Eigen::DontAlign>;
using Matrix3f = Eigen::Matrix<float,  3, 3, Eigen::DontAlign>;
using Matrix3d = Eigen::Matrix<double, 3, 3, Eigen::DontAlign>;
using Matrix4f = Eigen::Matrix<float,  4, 4, Eigen::DontAlign>;
using Matrix4d = Eigen::Matrix<double, 4, 4, Eigen::DontAlign>;

template<int N, class T>
using Transform = Eigen::Transform<float, N, Eigen::Affine, Eigen::DontAlign>;

using Transform2f = Eigen::Transform<float,  2, Eigen::Affine, Eigen::DontAlign>;
using Transform2d = Eigen::Transform<double, 2, Eigen::Affine, Eigen::DontAlign>;
using Transform3f = Eigen::Transform<float,  3, Eigen::Affine, Eigen::DontAlign>;
using Transform3d = Eigen::Transform<double, 3, Eigen::Affine, Eigen::DontAlign>;

} // namespace Slic3r

#endif
