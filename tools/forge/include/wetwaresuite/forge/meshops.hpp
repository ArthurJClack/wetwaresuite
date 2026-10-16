// Mesh helpers for game assets that libs/mesh doesn't have: UV projection,
// up-axis conversion, ratio-based decimation.
#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::forge {

enum class Projection { Planar, Box, Cylindrical, Spherical };

// Projects simple UVs. `axis` (0 = X, 1 = Y, 2 = Z) is the projection direction
// for planar and the pole axis for cylindrical and spherical; box picks each
// triangle's dominant axis. UVs fill 0..1 on the longest side (box keeps one
// texel density on every side) and are then multiplied by `scale`. Vertices
// are split where triangles need different UVs (box sides, the wrap seam).
void generateUvs(mesh::Mesh& m, Projection projection, int axis = 2, double scale = 1.0);

enum class AxisConversion { None, YUpToZUp, ZUpToYUp };

// Rotates the mesh between Y-up (most game engines, OBJ/FBX exports) and
// Z-up (this library, CAD, Blender). Y-up -> Z-up turns +Y into +Z.
void convertAxes(mesh::Mesh& m, AxisConversion conversion);
std::optional<AxisConversion> parseAxes(const std::string& name);  // none, y_up_to_z_up, z_up_to_y_up

// Smooth vertex normals where faces meet at less than `creaseDegrees`, split
// (hard) edges where they meet more sharply. Vertices are matched by position,
// so it smooths across UV seams and unwelded files without merging vertices
// (UVs survive).
void recomputeNormals(mesh::Mesh& m, double creaseDegrees = 60.0);

// Decimates to about `ratio` of the triangles (0..1], then recomputes normals
// with `creaseDegrees` when `normals` is set (decimation drops normals and UVs).
mesh::Mesh decimateRatio(const mesh::Mesh& m, double ratio, bool normals = true, double creaseDegrees = 60.0);
mesh::Mesh decimateTo(const mesh::Mesh& m, std::size_t triangles, bool normals = true, double creaseDegrees = 60.0);

}  // namespace wetwaresuite::forge
