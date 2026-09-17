#include "FiberIsland.hpp"

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r {

std::vector<FiberIslandSource> make_fiber_islands(
    ExPolygons components,
    size_t object_id,
    size_t layer_id,
    size_t policy_group_id)
{
    std::sort(components.begin(), components.end(), [](const ExPolygon& lhs, const ExPolygon& rhs) {
        const BoundingBox a = get_extents(lhs);
        const BoundingBox b = get_extents(rhs);
        if (a.min.x() != b.min.x()) return a.min.x() < b.min.x();
        if (a.min.y() != b.min.y()) return a.min.y() < b.min.y();
        if (a.max.x() != b.max.x()) return a.max.x() < b.max.x();
        if (a.max.y() != b.max.y()) return a.max.y() < b.max.y();
        return std::abs(lhs.area()) < std::abs(rhs.area());
    });

    std::vector<FiberIslandSource> result;
    result.reserve(components.size());
    for (size_t component_id = 0; component_id < components.size(); ++component_id) {
        result.push_back({
            FiberDomainId {object_id, layer_id, policy_group_id, component_id},
            std::move(components[component_id])
        });
    }
    return result;
}

std::vector<FiberDomainComponent> merge_connected_fiber_domains(
    const std::vector<FiberContributorDomainSource>& contributors)
{
    Polygons polygons;
    for (const FiberContributorDomainSource& contributor : contributors)
        append(polygons, to_polygons(contributor.effective_domain));

    std::vector<FiberIslandSource> islands = make_fiber_islands(
        union_ex(polygons), 0, 0, 0);
    std::vector<FiberDomainComponent> result;
    result.reserve(islands.size());
    for (FiberIslandSource& island : islands) {
        FiberDomainComponent component;
        component.merged_domain = std::move(island.effective_domain);
        for (const FiberContributorDomainSource& contributor : contributors) {
            if (!intersection_ex(
                    contributor.effective_domain,
                    ExPolygons {component.merged_domain},
                    ApplySafetyOffset::Yes).empty())
                component.contributor_indices.push_back(contributor.contributor_index);
        }
        std::sort(component.contributor_indices.begin(), component.contributor_indices.end());
        result.emplace_back(std::move(component));
    }
    return result;
}

} // namespace Slic3r
