// Mesh tidying for boolean results (internal to cad).
#pragma once

#include <optional>

#include "achclwss/mesh/mesh.hpp"

namespace achclwss::cad {

// Merges every flat region of a closed mesh (triangles lying in one plane,
// connected by edges) and re-triangulates it from its outline, dropping
// vertices that only sat on straight edges. BSP booleans leave flat faces cut
// into many thin slivers; this gives them back as a few clean triangles, which
// keeps files small and the next boolean robust. Returns nullopt when the
// result wouldn't be a clean closed mesh with the same volume.
std::optional<mesh::Mesh> mergeCoplanar(const mesh::Mesh& m);

}  // namespace achclwss::cad
