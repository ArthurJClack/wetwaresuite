// Mesh processing: simplification and planar slicing (what the slicer builds on).
#pragma once

#include <cstddef>
#include <vector>

#include "achclwss/mesh/mesh.hpp"

namespace achclwss::mesh {

// Vertex clustering: snaps vertices to a grid of `cellSize`, merges each cell
// to the average of its vertices and drops collapsed triangles. Fast, but
// doesn't guarantee a manifold result.
Mesh clusterVertices(const Mesh& mesh, double cellSize);
// Picks the grid size so the result has at most `targetTriangles` (as close as
// a few bisection steps get). Returns a copy when already small enough.
Mesh decimate(const Mesh& mesh, std::size_t targetTriangles);

// A closed 2D loop (the last point connects back to the first).
using Loop = std::vector<Vec2>;

// Cross-section of the mesh with the plane Z = z, as closed loops in XY.
// Outer boundaries wind counter-clockwise, holes clockwise (for outward-wound
// meshes). Vertices exactly on the plane count as above it, so results are
// consistent and never contain doubled or zero-length pieces. Faces lying in
// the plane contribute nothing. Chains that can't close (open meshes) are
// returned closed anyway.
std::vector<Loop> slice(const Mesh& mesh, double z);
// Slices at many heights in one pass over the triangles; result[i] is for zs[i].
std::vector<std::vector<Loop>> sliceLayers(const Mesh& mesh, const std::vector<double>& zs);

// Signed area of a loop (positive = counter-clockwise).
double loopArea(const Loop& loop);

}  // namespace achclwss::mesh
