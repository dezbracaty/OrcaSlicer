#ifndef slic3r_FiberSource_hpp_
#define slic3r_FiberSource_hpp_

#include <cstddef>
#include <cstdint>

namespace Slic3r {

enum class FiberPathPurpose : uint8_t {
    Contour,
    Infill
};

struct FiberDomainId {
    size_t object_id { 0 };
    size_t layer_id { 0 };
    size_t policy_group_id { 0 };
    size_t component_id { 0 };

    bool operator==(const FiberDomainId& rhs) const
    {
        return object_id == rhs.object_id && layer_id == rhs.layer_id &&
               policy_group_id == rhs.policy_group_id && component_id == rhs.component_id;
    }

    bool operator<(const FiberDomainId& rhs) const
    {
        if (object_id != rhs.object_id) return object_id < rhs.object_id;
        if (layer_id != rhs.layer_id) return layer_id < rhs.layer_id;
        if (policy_group_id != rhs.policy_group_id) return policy_group_id < rhs.policy_group_id;
        return component_id < rhs.component_id;
    }
};

struct FiberCandidateId {
    FiberDomainId domain;
    FiberPathPurpose purpose { FiberPathPurpose::Infill };
    size_t job_ordinal { 0 };
    size_t path_ordinal { 0 };

    bool operator==(const FiberCandidateId& rhs) const
    {
        return domain == rhs.domain && purpose == rhs.purpose &&
               job_ordinal == rhs.job_ordinal && path_ordinal == rhs.path_ordinal;
    }

    bool operator<(const FiberCandidateId& rhs) const
    {
        if (domain < rhs.domain) return true;
        if (rhs.domain < domain) return false;
        if (purpose != rhs.purpose) return purpose < rhs.purpose;
        if (job_ordinal != rhs.job_ordinal) return job_ordinal < rhs.job_ordinal;
        return path_ordinal < rhs.path_ordinal;
    }
};

struct FiberFragmentId {
    FiberCandidateId parent;
    size_t fragment_ordinal { 0 };

    bool operator==(const FiberFragmentId& rhs) const
    {
        return parent == rhs.parent && fragment_ordinal == rhs.fragment_ordinal;
    }

    bool operator<(const FiberFragmentId& rhs) const
    {
        if (parent < rhs.parent) return true;
        if (rhs.parent < parent) return false;
        return fragment_ordinal < rhs.fragment_ordinal;
    }
};

} // namespace Slic3r

#endif
