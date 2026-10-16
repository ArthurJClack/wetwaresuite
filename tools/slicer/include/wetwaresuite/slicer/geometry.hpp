// 2D polygon geometry for slicing: region booleans, offsets and scanline
// infill. A Region is a set of closed loops with outer boundaries
// counter-clockwise and holes clockwise, not overlapping each other (what
// mesh::slice gives for a clean mesh, and what every function here returns).
//
// Booleans work on an exact integer grid (0.1 micron): edges are split where
// they cross, each piece's winding number is found, and the pieces that
// bound the wanted region are linked back into loops. That handles holes,
// touching shapes and self-intersecting input, which is all the offsetter
// needs; it isn't Clipper (crossings are snapped to the grid, which can very
// rarely nick a loop).
#pragma once

#include <utility>
#include <vector>

#include "wetwaresuite/mesh/ops.hpp"

namespace wetwaresuite::slicer {

using mesh::Loop;
using mesh::Vec2;
using Region = std::vector<Loop>;
using Segment = std::pair<Vec2, Vec2>;

// Which winding numbers count as inside.
enum class FillRule { NonZero, Positive, AtLeastTwo };

// Cleans arbitrary loops into a Region under the fill rule.
Region resolve(const std::vector<Loop>& loops, FillRule rule);
// Any winding (inside-out meshes, overlapping shells) becomes a clean Region.
inline Region normalize(const std::vector<Loop>& loops) { return resolve(loops, FillRule::NonZero); }
Region unite(const Region& a, const Region& b);
Region intersect(const Region& a, const Region& b);
Region subtract(const Region& a, const Region& b);
// What every one of the regions covers, in one pass.
Region intersectAll(const std::vector<const Region*>& regions);

// Grows (delta > 0) or shrinks (delta < 0) a Region by exactly |delta| (a
// Minkowski offset): corners on the moving side stay sharp, the others are
// rounded. Parts that vanish are dropped and parts that pinch apart split.
Region offset(const Region& region, double delta);

// Sum of signed loop areas (holes subtract).
double area(const Region& region);
// Even-odd point test.
bool contains(const Region& region, const Vec2& p);
// Drops points closer than `tolerance` to the line through their neighbours.
Loop simplify(const Loop& loop, double tolerance);
// Splits a Region into islands: each outer loop with the holes inside it.
std::vector<Region> islands(const Region& region);

// Parallel lines `spacing` apart at `angleDegrees`, clipped to the region
// (even-odd). Lines sit on a fixed grid through the origin so layers line up.
std::vector<Segment> fillLines(const Region& region, double spacing, double angleDegrees);

}  // namespace wetwaresuite::slicer
