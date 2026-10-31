#ifndef slic3r_FillConcentricInternal_hpp_
#define slic3r_FillConcentricInternal_hpp_

#include "FillBase.hpp"
#include <utility>
#include <vector>

namespace Slic3r {

namespace MicroFillDetail {
// Internal core-only geometry seam shared with native acceptance checks.
size_t filter_redundant_paths(ExtrusionEntitiesPtr &entities, size_t begin,
    const std::vector<std::pair<const ExtrusionPath *, unsigned int>> &neighbors,
    unsigned int tool, float height, double max_length_scaled);
}

class FillConcentricInternal : public Fill
{
public:
    ~FillConcentricInternal() override = default;
    void fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) override;
    bool is_self_crossing() override { return false; }

protected:
    Fill* clone() const override { return new FillConcentricInternal(*this); };
    bool no_sort() const override { return true; }

    friend class Layer;
};

} // namespace Slic3r

#endif // slic3r_FillConcentricInternal_hpp_
