#pragma once
#include "ContinuousFiberFillStrategy.hpp"
#include <cstring>
namespace Slic3r { namespace continuous_fiber_detail {
enum class FinalSourceOccupiedKind : uint8_t { Clear, Collision };
struct FinalSourceOccupiedResult
{
    FinalSourceOccupiedKind kind;
    size_t collider_id;
};
// Construction is reserved to the successfully certified generator. A public
// mode flag is insufficient: all geometry, material and numerical context match.
class FiberInfillMaterialCertificate
{
    friend class Slic3r::ContinuousFiberFillStrategy;
    ExPolygons m_material;
    double m_width, m_scale;
    std::vector<FiberInfillPath> m_paths;
    std::vector<FinalSourceOccupiedResult> m_occupied;
    FiberInfillMaterialCertificate(const ExPolygons& m,
                                   double w,
                                   const std::vector<FiberInfillPath>& p,
                                   const std::vector<FinalSourceOccupiedResult>& occupied)
        : m_material(m), m_width(w), m_scale(SCALING_FACTOR), m_paths(p), m_occupied(occupied)
    {
        if (m_occupied.size() != m_paths.size())
            throw std::logic_error("Missing final source occupied result");
    }
    static bool scalar(double a, double b)
    {
        uint64_t x, y;
        std::memcpy(&x, &a, 8);
        std::memcpy(&y, &b, 8);
        return x == y;
    }
    static bool point(const Vec2d& a, const Vec2d& b) { return scalar(a.x(), b.x()) && scalar(a.y(), b.y()); }
    static bool fitting(const Polyline3& a, const Polyline3& b)
    {
        if (a.points != b.points || a.fitting_result.size() != b.fitting_result.size())
            return false;
        for (size_t i = 0; i < a.fitting_result.size(); ++i) {
            const auto& x = a.fitting_result[i];
            const auto& y = b.fitting_result[i];
            if (x.start_point_index != y.start_point_index || x.end_point_index != y.end_point_index || x.path_type != y.path_type)
                return false;
            if (x.path_type == EMovePathType::Arc_move_cw || x.path_type == EMovePathType::Arc_move_ccw) {
                const auto& A = x.arc_data;
                const auto& B = y.arc_data;
                if (A.center != B.center || A.start_point != B.start_point || A.end_point != B.end_point || A.is_arc != B.is_arc ||
                    A.direction != B.direction || !scalar(A.radius, B.radius) || !scalar(A.length, B.length) ||
                    !scalar(A.angle_radians, B.angle_radians) || !scalar(A.polar_start_theta, B.polar_start_theta) ||
                    !scalar(A.polar_end_theta, B.polar_end_theta))
                    return false;
            }
        }
        return true;
    }

public:
    const ExPolygons& material() const { return m_material; }
    void require_source_occupied_clear(size_t index) const
    {
        if (index >= m_paths.size() || index >= m_occupied.size())
            throw std::runtime_error("Missing final source occupied result");
        if (m_occupied[index].kind != FinalSourceOccupiedKind::Clear)
            throw std::runtime_error("Generated fiber infill overlaps reserved return; source=" + std::to_string(index) +
                                     "; collider=" + std::to_string(m_occupied[index].collider_id));
    }
    bool matches_material(const ExPolygons& m, double w) const
    {
        if (!scalar(w, m_width) || !scalar(SCALING_FACTOR, m_scale) || m.size() != m_material.size())
            return false;
        for (size_t i = 0; i < m.size(); ++i) {
            if (m[i].contour.points != m_material[i].contour.points || m[i].holes.size() != m_material[i].holes.size())
                return false;
            for (size_t j = 0; j < m[i].holes.size(); ++j)
                if (m[i].holes[j].points != m_material[i].holes[j].points)
                    return false;
        }
        return true;
    }
    bool matches_source(size_t index, const Polyline3& g, const std::vector<ContourArc>& arcs) const
    {
        if (index >= m_paths.size())
            return false;
        const auto& p = m_paths[index];
        if (!fitting(p.geometry, g) || p.arcs.size() != arcs.size())
            return false;
        for (size_t i = 0; i < arcs.size(); ++i) {
            const auto& a = p.arcs[i];
            const auto& b = arcs[i];
            if (!point(a.center_mm, b.center_mm) || !point(a.start_mm, b.start_mm) || !point(a.end_mm, b.end_mm) ||
                !scalar(a.radius_mm, b.radius_mm) || !scalar(a.sweep_radians, b.sweep_radians) ||
                !scalar(a.source_sweep_radians, b.source_sweep_radians) || !scalar(a.begin_mm, b.begin_mm) ||
                !scalar(a.end_distance_mm, b.end_distance_mm))
                return false;
        }
        return g.points.size() == 2 || (!arcs.empty() && !g.fitting_result.empty());
    }
    bool matches_batch(const FiberInfillCandidates& batch) const
    {
        if (m_occupied.size() != m_paths.size() || batch.paths.size() != m_paths.size())
            return false;
        for (size_t i = 0; i < m_paths.size(); ++i)
            if (!matches_source(i, batch.paths[i].geometry, batch.paths[i].arcs))
                return false;
        return true;
    }
};
}}
