// 2D sketches with constraints, as in SolidWorks or Onshape. A sketch is a
// feature of the part: points, entities (lines, circles, arcs, splines) and
// constraints, geometric (coincident, horizontal, parallel, tangent, ...) and
// dimensional (distance, length, radius, angle, ...; the value is an
// expression over the part's parameters). On every rebuild the sketch is
// solved numerically (damped least-change Gauss-Newton), its closed loops
// become profiles, and features such as sketch_extrude, sketch_revolve, loft
// and sweep consume them.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/mesh/math.hpp"
#include "wetwaresuite/mesh/primitives.hpp"

namespace wetwaresuite::cad {

struct SketchPoint {
    std::string id;  // p1, p2, ...
    double x = 0, y = 0;
};

// kind: line (points start, end), circle (center; radius), arc (center, start,
// end; counter-clockwise from start to end), spline (points it passes through;
// closed joins the last to the first).
struct SketchEntity {
    std::string id;  // l1, c1, a1, s1
    std::string kind;
    std::vector<std::string> points;
    double radius = 0;
    bool construction = false;  // drawn and constrained, but not part of a profile
    bool closed = false;
};

struct SketchConstraint {
    std::string id;    // k1, k2, ...
    std::string type;  // coincident, horizontal, ..., distance, length, radius, angle
    std::vector<std::string> refs;
    std::string value;  // dimensions: an expression; fixed: "x,y"
    double aux = 0;     // side / branch picked when it was added
};

struct Sketch {
    std::vector<SketchPoint> points;
    std::vector<SketchEntity> entities;
    std::vector<SketchConstraint> constraints;
    std::map<std::string, int> counters;  // per id prefix

    SketchPoint* point(const std::string& id);
    const SketchPoint* point(const std::string& id) const;
    SketchEntity* entity(const std::string& id);
    const SketchEntity* entity(const std::string& id) const;
    SketchConstraint* constraint(const std::string& id);
    std::string newId(const std::string& prefix);
};

// A placed plane: sketch (x, y) lands on origin + x*u + y*v; n is the
// direction features extrude along (u, v, n may be left-handed).
struct Frame {
    mesh::Vec3 origin, u{1, 0, 0}, v{0, 1, 0}, n{0, 0, 1};
    mesh::Vec3 at(double x, double y, double z = 0) const { return origin + u * x + v * y + n * z; }
    mesh::Vec2 project(const mesh::Vec3& p) const {
        const mesh::Vec3 d = p - origin;
        return {mesh::dot(d, u), mesh::dot(d, v)};
    }
    mesh::Mat4 matrix() const;  // sketch (x, y, z) -> world
};

struct Axis {
    mesh::Vec3 point, dir{0, 0, 1};
    double radius = 0;  // holes, cylinders and circles: their radius, 0 for datum axes
};

// A sketch after solving.
struct SolvedSketch {
    Frame frame;
    bool onFace = false;
    std::map<std::string, mesh::Vec2> points;  // every point, solved
    std::map<std::string, double> radii;       // circles
    int variables = 0, equations = 0, dof = 0, redundant = 0;
    std::vector<std::string> underDefined;      // points (and circles) that can still move
    std::map<std::string, double> measured;     // constraint id -> its value now (lengths, degrees)
    // closed regions (outer + holes, counter-clockwise outer), every closed loop, open chains
    std::vector<mesh::Polygon2> regions;
    std::vector<std::vector<mesh::Vec2>> loops;
    std::vector<std::vector<mesh::Vec2>> chains;
    std::string profileProblem;  // why it isn't a clean set of closed profiles, empty when it is
    std::vector<mesh::Vec2> circleCenters;      // profile circles, for axes and drawings
    std::vector<double> circleRadii;
    std::string status() const;  // "fully defined", "under defined (3 degrees of freedom)", ...
};

// What a feature exposes besides a solid: a solved sketch, a plane, axes.
struct FeatureExtra {
    std::shared_ptr<const SolvedSketch> sketch;
    std::optional<Frame> plane;
    std::vector<Axis> axes;
};

// Geometric and dimensional constraint types (cad.sketch_constrain / cad.sketch_dimension).
const std::vector<std::string>& geometricConstraints();
const std::vector<std::string>& dimensionalConstraints();

}  // namespace wetwaresuite::cad
