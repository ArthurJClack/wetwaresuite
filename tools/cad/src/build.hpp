// What feature builders share (internal to cad): the build context, booleans
// that stay closed, and the advanced features (sketches, datums, sketch-based
// solids, loft, sweep, draft, rib, thread) in features.cpp.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/cad/cad.hpp"
#include "wetwaresuite/mesh/csg.hpp"

namespace wetwaresuite::cad::detail {

using MeshPtr = std::shared_ptr<const mesh::Mesh>;
using Done = std::map<std::string, CadDoc::Cached>;

struct Ctx {
    const CadDoc& doc;
    const Feature& f;
    const FeatureType& t;

    [[noreturn]] void fail(const std::string& why) const;
    bool has(const std::string& key) const;
    std::string text(const std::string& key) const;
    double number(const std::string& key) const;
    int whole(const std::string& key, int lo, int hi) const;
    bool flag(const std::string& key) const;
    std::vector<mesh::Vec2> pts(const std::string& key, std::size_t minPoints) const;
    std::vector<mesh::Vec3> points3(const std::string& key) const;  // x,y,z triples
    double size(const std::string& diameter, const std::string& radius, bool required = true) const;
    mesh::Mat4 place() const;
    mesh::Mat4 sketchPlane() const;
};

struct Built {
    MeshPtr mesh;
    std::vector<MeshPtr> tools;
    std::shared_ptr<const FeatureExtra> extra;
    std::string signature;  // what later features see, when it isn't the inputs (sketches: the solved geometry)
};

MeshPtr share(mesh::Mesh m);
mesh::Mesh placed(mesh::Mesh m, const mesh::Mat4& t);
// Boolean that retries with tiny nudges until the result is closed.
mesh::Mesh csg(const mesh::Mesh& a, const mesh::Mesh& b, mesh::BooleanOp op);
const CadDoc::Cached* input(const Done& done, const Ctx& c, const std::string& key);

// Types built in features.cpp.
bool isAdvanced(const std::string& type);
Built buildAdvanced(const Ctx& c, const Done& done);
// Extra signature text for an advanced feature (the sketch's own data).
std::string advancedSignature(const Ctx& c);
// Axes a primitive or hole exposes (cylinders, holes), for mates and drawings.
std::vector<Axis> primitiveAxes(const Ctx& c);

// The plane of a planar face: by name (top, bottom, front, back, left, right,
// +x, ...) the outermost face facing that way, or "x,y,z" the face nearest
// that point. Sketch axes follow the world axes the face is closest to.
bool facePlane(const mesh::Mesh& body, const std::string& selector, const CadDoc& doc, Frame& out, std::string& error);
// The standard plane frames xy, xz, yz (as extrude's plane field), shifted by offset along n.
Frame standardFrame(const std::string& plane, double offset);

}  // namespace wetwaresuite::cad::detail
