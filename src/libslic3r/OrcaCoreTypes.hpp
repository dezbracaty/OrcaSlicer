#ifndef slic3r_OrcaCoreTypes_hpp_
#define slic3r_OrcaCoreTypes_hpp_

#include <cstdint>

#if 0
// Saves around 32% RAM after slicing step, 6.7% after G-code export (tested on PrusaSlicer 2.2.0 final).
using coord_t = int32_t;
#else
// FIXME At least FillRectilinear2 and std::boost Voronoi require coord_t to be 32bit.
using coord_t = int64_t;
#endif

using coordf_t = double;

#endif
