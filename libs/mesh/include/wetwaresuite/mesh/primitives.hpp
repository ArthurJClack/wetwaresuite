// Primitive solids with exact dimensions. Z is up; round shapes are polygons
// whose vertices lie exactly on the requested radius (so a cylinder's volume
// is slightly below pi r^2 h, by the inscribed-polygon factor). Every closed
// primitive comes out welded and watertight with outward winding.
// Bad dimensions (<= 0, too few segments) throw std::invalid_argument.
#pragma once

#include <vector>

#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::mesh {

// A 2D region: one outer ring and any number of holes. Winding doesn't matter;
// rings are implicitly closed (don't repeat the first point).
struct Polygon2 {
    std::vector<Vec2> outer;
    std::vector<std::vector<Vec2>> holes;
};

// center=true puts the middle at the origin; false puts the min corner (box)
// or the base (cylinder, cone) at the origin.
Mesh box(const Vec3& size, bool center = true);
Mesh cylinder(double radius, double height, int segments = 32, bool center = true);
// Frustum from radius r1 at the bottom to r2 at the top; either may be 0 (a point).
Mesh cone(double r1, double r2, double height, int segments = 32, bool center = true);
// UV sphere: `segments` around Z, `rings` from pole to pole.
Mesh sphere(double radius, int segments = 32, int rings = 16);
// Ring around Z: `major` is the distance to the tube centre, `minor` the tube radius.
Mesh torus(double major, double minor, int segments = 32, int tubeSegments = 16);
// Flat XY rectangle facing +Z, centred, with normals and UVs. Open (one-sided).
Mesh plane(double width, double depth, int divisionsX = 1, int divisionsY = 1);

// Extrudes a 2D region along +Z from z=0 to z=height (or centred).
Mesh extrude(const Polygon2& shape, double height, bool center = false);
// Revolves a closed 2D profile about the Z axis. Profile x is the radius
// (>= 0, points with x = 0 sit on the axis), y is Z. angle in degrees
// (0, 360]; partial revolves get flat end caps.
Mesh revolve(const std::vector<Vec2>& profile, double angleDegrees = 360.0, int segments = 32);

// 2D helpers for extrude.
std::vector<Vec2> circle2(double radius, int segments = 32, const Vec2& center = {});
std::vector<Vec2> rectangle2(double width, double height, bool center = true);

}  // namespace wetwaresuite::mesh
