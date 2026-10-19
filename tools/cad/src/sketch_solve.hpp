// Solving sketches and turning them into profiles (internal to cad).
#pragma once

#include <functional>
#include <string>

#include "wetwaresuite/cad/sketch.hpp"

namespace wetwaresuite::cad::detail {

using Evaluator = std::function<double(const std::string&)>;

// What a reference names: a point (p3, origin, l1.start, c1.center, ...) or an
// entity (l1, c2, a1, s1, and the built-in xaxis / yaxis lines).
enum class RefKind { Point, Line, Circle, Arc, Spline };
RefKind refKind(const Sketch& sketch, const std::string& ref);  // throws std::invalid_argument

// Checks a constraint's refs and picks its branch (aux) from the current
// positions, so solving keeps the geometry on the side it was drawn. Throws
// std::invalid_argument with what the type expects.
void prepareConstraint(const Sketch& sketch, SketchConstraint& c, const Evaluator& eval);

// Solves a copy of the sketch and extracts its profiles. Throws
// std::runtime_error when the constraints conflict (naming them).
SolvedSketch solveSketch(const Sketch& sketch, const Frame& frame, const Evaluator& eval, int segments);

// Tessellated outline of one entity in sketch coordinates (for drawing it).
std::vector<mesh::Vec2> entityOutline(const SketchEntity& e, const SolvedSketch& solved, int segments);

}  // namespace wetwaresuite::cad::detail
