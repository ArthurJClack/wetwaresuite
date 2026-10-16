// Indexed triangle meshes: the one geometry type cad, slicer and asset forge
// share. Z is up, units are whatever the file used (millimetres for STL/3MF).
// Triangles wind counter-clockwise seen from outside, so normals point out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "achclwss/mesh/math.hpp"

namespace achclwss::mesh {

struct Mesh {
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices;  // three per triangle
    std::vector<Vec3> normals;           // empty, or one per position
    std::vector<Vec2> uvs;               // empty, or one per position

    std::size_t vertexCount() const { return positions.size(); }
    std::size_t triangleCount() const { return indices.size() / 3; }
    bool empty() const { return indices.empty(); }
    bool hasNormals() const { return !normals.empty() && normals.size() == positions.size(); }
    bool hasUvs() const { return !uvs.empty() && uvs.size() == positions.size(); }

    // Corner `k` (0..2) of triangle `t`.
    const Vec3& corner(std::size_t t, int k) const { return positions[indices[t * 3 + static_cast<std::size_t>(k)]]; }
    // Unnormalized face normal (length = 2 x area).
    Vec3 faceNormal(std::size_t t) const;

    void addTriangle(std::uint32_t a, std::uint32_t b, std::uint32_t c) { indices.insert(indices.end(), {a, b, c}); }
    std::uint32_t addVertex(const Vec3& p) {
        positions.push_back(p);
        return static_cast<std::uint32_t>(positions.size() - 1);
    }
};

Bounds bounds(const Mesh& mesh);
double surfaceArea(const Mesh& mesh);
// Signed volume of the enclosed space (positive when the winding faces out).
// Meaningful for closed meshes only.
double volume(const Mesh& mesh);
// Every edge is shared by exactly two triangles that walk it in opposite
// directions. Vertices are matched by exact position, so meshes split for
// normals or UVs still count as closed.
bool isWatertight(const Mesh& mesh);
// Number of edges used by only one triangle (holes), after exact-position matching.
std::size_t boundaryEdgeCount(const Mesh& mesh);

// Merges vertices closer than `tolerance` (0 = identical positions only),
// drops triangles that collapse, then removes unused vertices. Normals and
// UVs of the first merged vertex are kept.
void weld(Mesh& mesh, double tolerance = 0.0);
// Drops degenerate (repeated index or zero-area) triangles and unused vertices.
void cleanup(Mesh& mesh);
// Area-weighted vertex normals. Below 180 degrees, vertices are split where
// faces meet at a sharper angle than `creaseDegrees`, so boxes stay crisp
// while spheres stay smooth.
void computeNormals(Mesh& mesh, double creaseDegrees = 180.0);
// Reverses every triangle's winding and negates normals.
void flip(Mesh& mesh);
// Applies m to positions and normals; mirroring transforms also flip winding.
void transform(Mesh& mesh, const Mat4& m);
// Adds src's triangles to dst. Normals/UVs survive only when both sides have them
// (or dst was empty).
void append(Mesh& dst, const Mesh& src);
// Each triangle gets its own three vertices (for flat shading or exporters).
Mesh unweld(const Mesh& mesh);

}  // namespace achclwss::mesh
