// Boolean solids (CSG) with BSP trees, after Evan Wallace's csg.js. Inputs
// should be closed and outward-wound; results are welded, T-junctions are
// stitched, and closed inputs give closed (watertight) outputs.
#pragma once

#include "achclwss/mesh/mesh.hpp"

namespace achclwss::mesh {

enum class BooleanOp { Union, Difference, Intersection };

Mesh booleanOp(const Mesh& a, const Mesh& b, BooleanOp op);
inline Mesh booleanUnion(const Mesh& a, const Mesh& b) { return booleanOp(a, b, BooleanOp::Union); }
inline Mesh booleanDifference(const Mesh& a, const Mesh& b) { return booleanOp(a, b, BooleanOp::Difference); }
inline Mesh booleanIntersection(const Mesh& a, const Mesh& b) { return booleanOp(a, b, BooleanOp::Intersection); }

}  // namespace achclwss::mesh
