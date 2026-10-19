// Edit-mode operators, part two: bridge, fill, spin, mirror, symmetrize,
// duplicate, normals, triangulate, tris to quads, smooth, shrink/fatten,
// vertex colours.
#include <algorithm>

#include "cmd.hpp"

namespace wetwaresuite::model::cmd {

namespace {

int axisOf(const std::string& s) {
    const char c = s.empty() ? 'x' : s.back();
    return c == 'y' ? 1 : c == 'z' ? 2 : 0;
}

bool anyFaces(const PolyMesh& m) { return !selectedFaces(m).empty(); }
bool anyVerts(const PolyMesh& m) { return !selectedVerts(m).empty(); }

}  // namespace

void registerEditCommands2(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.bridge",
             "Bridge edge loops",
             "Join two (or more) selected edge loops or open edge chains with a band of quads (Bridge Edge Loops). "
             "With selected faces, their areas are removed and the outlines bridged (a tunnel through a box). The "
             "loops need the same number of edges; twist rotates the pairing.",
             withDoc({objectParam(), {"twist", ParamType::Int, "rotate the pairing by this many edges", false, {}, std::int64_t{0}}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::size_t n = bridge(m, static_cast<int>(intParam(p, "twist", 0)));
                return done("bridged with " + std::to_string(n) + " faces", d, p);
            })));

    reg.add({"model.fill",
             "Fill / make face",
             "Make a face from the selection (F): a closed loop of selected edges, or the selected vertices in order "
             "around their middle (two vertices make an edge). type=holes caps every hole (boundary loop) instead, or "
             "only fully selected ones with selected=true.",
             withDoc({objectParam(),
                      {"type", ParamType::String, "what to fill", false, {"face", "holes"}, std::string("face")},
                      {"selected", ParamType::Bool, "holes: only selected loops", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                std::size_t n = 0;
                if (stringParam(p, "type", "face") == "holes") n = fillHoles(m, boolParam(p, "selected", false));
                else n = fill(m);
                if (!n) return Result::failure("nothing to fill: select a closed loop of edges or three or more vertices");
                return done("filled " + std::to_string(n), d, p);
            })));

    reg.add({"model.spin",
             "Spin",
             "Spin the selection around an axis (world, through center=, default the 3D cursor): steps copies over angle "
             "degrees joined by faces. A profile of edges becomes a lathed surface (a vase, a wheel); 360 closes it up.",
             withDoc({objectParam(),
                      {"angle", ParamType::Number, "degrees", false, {}, 360.0},
                      {"steps", ParamType::Int, "copies", false, {}, std::int64_t{12}},
                      {"axis", ParamType::NumberList, "axis direction x,y,z", false},
                      {"center", ParamType::NumberList, "a point on the axis (default: the 3D cursor)", false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const int steps = static_cast<int>(intParam(p, "steps", 12));
                if (steps < 1 || steps > 512) return Result::failure("steps is 1 to 512");
                if (!anyVerts(m)) return Result::failure("select the profile to spin");
                const Mat4 inv = d.world(o).inverse();
                const Vec3 axis = mesh::normalize(inv.direction(vec3(p, "axis", {0, 0, 1})));
                const Vec3 center = inv.point(vec3(p, "center", d.scene.cursor));
                const bool profile = selectedFaces(m).empty();
                const std::size_t n = spin(m, axis, center, numberParam(p, "angle", 360), steps);
                if (profile) recalcNormals(m, false, false);  // a lathed profile: closed parts face out
                return done("spun: " + std::to_string(n) + " new faces", d, p);
            })));

    reg.add({"model.mirror",
             "Mirror",
             "Mirror the selection (everything when nothing is selected) across the object's x, y or z plane through "
             "its origin (or center=, world). duplicate=true keeps the original and adds the mirrored copy, sharing "
             "vertices within merge of the plane; otherwise it flips in place.",
             withDoc({objectParam(),
                      {"axis", ParamType::String, "plane normal", false, {"x", "y", "z"}, std::string("x")},
                      {"duplicate", ParamType::Bool, "keep the original", false, {}, false},
                      {"center", ParamType::NumberList, "a point on the plane (world; default: the origin)", false},
                      {"merge", ParamType::Number, "join vertices this close to the plane", false, {}, 0.001}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const Mat4 w = d.world(o);
                const Vec3 center = w.inverse().point(vec3(p, "center", w.point({0, 0, 0})));
                const std::size_t n = mirror(m, axisOf(stringParam(p, "axis", "x")), center, boolParam(p, "duplicate", false),
                                             numberParam(p, "merge", 0.001));
                return done("mirrored " + std::to_string(n) + " vertices", d, p);
            })));

    reg.add({"model.symmetrize",
             "Symmetrize",
             "Make the mesh symmetric: keep one side of the object's plane (side=+x keeps positive x) and replace the "
             "other with its mirror image, welded on the plane.",
             withDoc({objectParam(),
                      {"side", ParamType::String, "the side to keep", false, {"+x", "-x", "+y", "-y", "+z", "-z"}, std::string("+x")},
                      {"merge", ParamType::Number, "join vertices this close to the plane", false, {}, 0.001}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::string side = stringParam(p, "side", "+x");
                const int axis = axisOf(side);
                Vec3 n{0, 0, 0};
                (axis == 0 ? n.x : axis == 1 ? n.y : n.z) = side[0] == '-' ? -1 : 1;
                selectAll(m, false);
                bisect(m, {0, 0, 0}, n, true, false, false);
                selectAll(m, false);
                const std::size_t count = mirror(m, axis, {0, 0, 0}, true, numberParam(p, "merge", 0.001));
                selectAll(m, false);
                return done("symmetrized: mirrored " + std::to_string(count) + " vertices", d, p);
            })));

    reg.add({"model.duplicate",
             "Duplicate",
             "Copy the selected faces and loose edges (Shift+D) inside the mesh, moved by offset (world); the copy ends "
             "up selected.",
             withDoc({objectParam(), {"offset", ParamType::NumberList, "move the copy by x,y,z", false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const std::size_t n = duplicate(m);
                if (!n) return Result::failure("select faces or edges to duplicate");
                const Vec3 off = d.world(o).inverse().direction(vec3(p, "offset", {0, 0, 0}));
                transformSelected(m, Mat4::translate(off));
                return done("duplicated " + std::to_string(n), d, p);
            })));

    reg.add({"model.normals",
             "Normals",
             "Fix face directions: recalc makes neighbours agree and closed parts face outward (Shift+N; inside=true "
             "faces inward), flip reverses them. Works on the selected faces, or all when none are selected.",
             withDoc({objectParam(),
                      {"action", ParamType::String, "what to do", false, {"recalc", "flip"}, std::string("recalc")},
                      {"inside", ParamType::Bool, "recalc: point inward", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const bool sel = anyFaces(m);
                const std::size_t n = stringParam(p, "action", "recalc") == "flip" ? flipNormals(m, sel)
                                                                                    : recalcNormals(m, boolParam(p, "inside", false), sel);
                return done("flipped " + std::to_string(n) + " faces", d, p);
            })));

    reg.add({"model.triangulate",
             "Triangulate faces",
             "Split the selected faces (all when none are) into triangles (Ctrl+T).",
             withDoc({objectParam()}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::size_t n = triangulateFaces(m, anyFaces(m));
                return done("triangulated into " + std::to_string(n) + " triangles", d, p);
            })));

    reg.add({"model.tris_to_quads",
             "Triangles to quads",
             "Join pairs of triangles into quads (Alt+J) where they're flatter than angle; selected faces, or all.",
             withDoc({objectParam(), {"angle", ParamType::Number, "max angle between the triangles", false, {}, 40.0}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::size_t n = trisToQuads(m, numberParam(p, "angle", 40), anyFaces(m));
                return done("made " + std::to_string(n) + " quads", d, p);
            })));

    reg.add({"model.smooth_vertices",
             "Smooth vertices",
             "Relax the selected vertices (all when none are) toward their neighbours; boundaries slide along themselves.",
             withDoc({objectParam(),
                      {"factor", ParamType::Number, "0..1 per step", false, {}, 0.5},
                      {"repeat", ParamType::Int, "steps", false, {}, std::int64_t{1}}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const int rep = static_cast<int>(intParam(p, "repeat", 1));
                if (rep < 1 || rep > 1000) return Result::failure("repeat is 1 to 1000");
                const std::size_t n = smoothVertices(m, numberParam(p, "factor", 0.5), rep, anyVerts(m));
                return done("smoothed " + std::to_string(n) + " vertices", d, p);
            })));

    reg.add({"model.shrink_fatten",
             "Shrink/fatten",
             "Move the selected vertices along their normals (Alt+S): positive fattens, negative shrinks.",
             withDoc({objectParam(), {"offset", ParamType::Number, "distance", true}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::size_t n = shrinkFatten(m, numberParam(p, "offset"));
                if (!n) return Result::failure("nothing selected");
                return done("moved " + std::to_string(n) + " vertices", d, p);
            })));

    reg.add({"model.vertex_color",
             "Paint vertex colours",
             "Set the colour of the selected vertices (all when none are) and turn vertex colours on; they tint the "
             "viewport and go out in OBJ files.",
             withDoc({objectParam(),
                      {"color", ParamType::String, "colour like #ff8800 or red", true},
                      {"off", ParamType::Bool, "turn vertex colours off instead", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                if (boolParam(p, "off", false)) {
                    m.colors = false;
                    return done("vertex colours off", d, p);
                }
                const Vec3 c = color(p, "color", {1, 1, 1});
                const bool sel = anyVerts(m);
                std::size_t n = 0;
                for (auto& v : m.verts)
                    if (!sel || v.sel) {
                        v.color = c;
                        ++n;
                    }
                m.colors = true;
                return done("painted " + std::to_string(n) + " vertices " + hex(c), d, p);
            })));
}

}  // namespace wetwaresuite::model::cmd
