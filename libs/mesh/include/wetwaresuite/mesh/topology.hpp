// Connectivity and per-vertex helpers for tools that walk a mesh's surface
// (sculpting, smoothing, retopology): who neighbours whom, vertex normals
// without splitting, and closest points on triangles. Vertices are matched by
// index, so weld() a mesh first if its file split vertices for UVs or normals.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::mesh {

// Compressed lists, one per vertex: items[offsets[v] .. offsets[v + 1]).
struct Adjacency {
    std::vector<std::uint32_t> offsets;  // vertexCount + 1 entries
    std::vector<std::uint32_t> items;

    std::size_t count(std::size_t v) const { return offsets[v + 1] - offsets[v]; }
    const std::uint32_t* begin(std::size_t v) const { return items.data() + offsets[v]; }
    const std::uint32_t* end(std::size_t v) const { return items.data() + offsets[v + 1]; }
};

// Vertices sharing an edge with each vertex (each listed once, sorted).
Adjacency vertexNeighbors(const Mesh& mesh);
// Triangles using each vertex (triangle indices, ascending).
Adjacency vertexTriangles(const Mesh& mesh);

// Unit face normals, one per triangle (zero for degenerate triangles).
std::vector<Vec3> faceNormals(const Mesh& mesh);
// Area-weighted unit vertex normals, one per position, never split.
std::vector<Vec3> vertexNormals(const Mesh& mesh);

// For each vertex, whether it lies on an open edge (an edge used by one triangle).
std::vector<bool> boundaryVertices(const Mesh& mesh);

// Closest point to p on triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

// Ray (origin, unit direction) against triangle abc, both sides. Returns the
// distance along the ray, or a negative number when it misses.
double rayTriangle(const Vec3& origin, const Vec3& dir, const Vec3& a, const Vec3& b, const Vec3& c);

}  // namespace wetwaresuite::mesh
