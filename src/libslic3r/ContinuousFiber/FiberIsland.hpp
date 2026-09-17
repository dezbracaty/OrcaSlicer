#ifndef slic3r_FiberIsland_hpp_
#define slic3r_FiberIsland_hpp_

#include "FiberSource.hpp"
#include "../ExPolygon.hpp"

#include <vector>

namespace Slic3r {

struct FiberIslandSource {
    FiberDomainId id;
    ExPolygon effective_domain;
};

struct FiberContributorDomainSource {
    size_t contributor_index { 0 };
    ExPolygons effective_domain;
};

struct FiberDomainComponent {
    ExPolygon merged_domain;
    std::vector<size_t> contributor_indices;
};

std::vector<FiberIslandSource> make_fiber_islands(
    ExPolygons components,
    size_t object_id,
    size_t layer_id,
    size_t policy_group_id);

std::vector<FiberDomainComponent> merge_connected_fiber_domains(
    const std::vector<FiberContributorDomainSource>& contributors);

} // namespace Slic3r

#endif
