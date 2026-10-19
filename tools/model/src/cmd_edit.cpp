// Edit-mode operators, part one: transform, extrude, inset, bevel, loop cut,
// bisect, knife, subdivide, merge, dissolve, delete.
#include <algorithm>
#include <cmath>

#include "cmd.hpp"

namespace wetwaresuite::model::cmd {

namespace {

std::string count(std::size_t n, const std::string& what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); }

double worldScale(const Mat4& w) {
    const double sx = mesh::length(w.direction({1, 0, 0})), sy = mesh::length(w.direction({0, 1, 0})), sz = mesh::length(w.direction({0, 0, 1}));
    return std::max((sx + sy + sz) / 3, 1e-12);
}

std::string modeDefault(const ModelDoc& d) {
    return d.scene.selectMode == SelectMode::Vertex ? "verts" : d.scene.selectMode == SelectMode::Edge ? "edges" : "faces";
}

}  // namespace

void registerEditCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.transform",
             "Move, rotate, scale",
             "Move (G), rotate (R) and scale (S) the selected vertices of the edited mesh, in world space: scale first, "
             "then rotate, then move, about the pivot (median of the selection, its bounds centre, the 3D cursor or the "
             "object origin). rotate is degrees about x,y,z, or angle= about axis=. proportional= (a radius) drags "
             "nearby vertices along with a smooth falloff.",
             withDoc({objectParam(),
                      {"translate", ParamType::NumberList, "move by x,y,z", false},
                      {"rotate", ParamType::NumberList, "degrees about x,y,z", false},
                      {"angle", ParamType::Number, "degrees about axis=", false},
                      {"axis", ParamType::NumberList, "rotation axis for angle= (default 0,0,1)", false},
                      {"scale", ParamType::NumberList, "x,y,z factors or one number", false},
                      {"pivot", ParamType::String, "where rotate and scale centre", false, {"median", "bounds", "cursor", "origin"}, std::string("median")},
                      {"proportional", ParamType::Number, "proportional editing radius (0 = off)", false, {}, 0.0}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const auto sel = selectedVerts(m);
                if (sel.empty()) return Result::failure("nothing selected (model.select)");
                const Mat4 w = d.world(o), inv = w.inverse();
                const std::string pivotMode = stringParam(p, "pivot", "median");
                Vec3 pivot{0, 0, 0};
                if (pivotMode == "cursor") {
                    pivot = d.scene.cursor;
                } else if (pivotMode == "origin") {
                    pivot = w.point({0, 0, 0});
                } else if (pivotMode == "bounds") {
                    mesh::Bounds b;
                    for (auto v : sel) b.add(w.point(m.verts[v].p));
                    pivot = (b.min + b.max) * 0.5;
                } else {
                    for (auto v : sel) pivot = pivot + w.point(m.verts[v].p);
                    pivot = pivot / static_cast<double>(sel.size());
                }
                Mat4 r = Mat4::rotate(vec3(p, "rotate", {0, 0, 0}));
                if (has(p, "angle")) r = Mat4::rotateAxis(vec3(p, "axis", {0, 0, 1}), numberParam(p, "angle")) * r;
                const Mat4 t = Mat4::translate(vec3(p, "translate", {0, 0, 0})) * Mat4::translate(pivot) * r *
                               Mat4::scale(vec3(p, "scale", {1, 1, 1})) * Mat4::translate(-pivot);
                const std::size_t n = transformSelected(m, inv * t * w, numberParam(p, "proportional", 0) / worldScale(w));
                return done("transformed " + count(n, "vertex"), d, p);
            })));

    reg.add({"model.extrude",
             "Extrude",
             "Extrude the selection (E): selected faces as one region per connected area moved along its normal by "
             "offset (individual=true: each face along its own normal), or by direction= (world x,y,z). Without "
             "selected faces, selected edges extrude into faces and lone vertices into edges, moved by direction. The "
             "new geometry ends up selected.",
             withDoc({objectParam(),
                      {"offset", ParamType::Number, "distance along the normal", false, {}, 1.0},
                      {"direction", ParamType::NumberList, "move the extrusion by this world vector instead", false},
                      {"individual", ParamType::Bool, "each face separately", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                std::optional<Vec3> dir;
                if (has(p, "direction")) dir = d.world(o).inverse().direction(vec3(p, "direction", {0, 0, 1}));
                const auto mode = boolParam(p, "individual", false) ? ExtrudeMode::Individual : ExtrudeMode::Region;
                const std::size_t n = extrude(m, mode, numberParam(p, "offset", 1), dir);
                if (!n) return Result::failure("nothing selected to extrude");
                return done("extruded: " + count(n, "new element"), d, p);
            })));

    reg.add({"model.inset",
             "Inset faces",
             "Inset the selected faces (I): a smaller copy inside each region (or each face with individual=true), "
             "thickness in from the edge, then moved depth along the normal (negative pushes in).",
             withDoc({objectParam(),
                      {"thickness", ParamType::Number, "how far in", false, {}, 0.1},
                      {"depth", ParamType::Number, "how far out along the normal", false, {}, 0.0},
                      {"individual", ParamType::Bool, "each face separately", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::size_t n = inset(m, numberParam(p, "thickness", 0.1), numberParam(p, "depth", 0), boolParam(p, "individual", false));
                if (!n) return Result::failure("select faces to inset");
                return done("inset: " + count(n, "new face"), d, p);
            })));

    reg.add({"model.bevel",
             "Bevel",
             "Bevel the selected edges (Ctrl+B) with width offset along each face and segments rounded steps, or the "
             "selected vertices with vertices=true (Ctrl+Shift+B).",
             withDoc({objectParam(),
                      {"width", ParamType::Number, "offset", false, {}, 0.1},
                      {"segments", ParamType::Int, "steps in the rounding", false, {}, std::int64_t{1}},
                      {"vertices", ParamType::Bool, "bevel vertices instead of edges", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const int seg = static_cast<int>(intParam(p, "segments", 1));
                if (seg < 1 || seg > 64) return Result::failure("segments is 1 to 64");
                const std::size_t n = bevel(m, numberParam(p, "width", 0.1), seg, boolParam(p, "vertices", false));
                if (!n) return Result::failure(boolParam(p, "vertices", false) ? "select vertices to bevel" : "select edges between two faces to bevel");
                return done("bevelled: " + count(n, "new face"), d, p);
            })));

    reg.add({"model.loop_cut",
             "Loop cut",
             "Cut new edge loops (Ctrl+R) across the ring of quads through edge= (two vertex indices). cuts loops "
             "evenly spaced; with one cut, factor slides it along the edge (0..1). The new loops end up selected.",
             withDoc({objectParam(),
                      {"edge", ParamType::NumberList, "two vertex indices of an edge the loop crosses", true},
                      {"cuts", ParamType::Int, "number of loops", false, {}, std::int64_t{1}},
                      {"factor", ParamType::Number, "where a single cut sits along the edge", false, {}, 0.5}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const NumberList e = listParam(p, "edge");
                if (e.size() != 2) return Result::failure("edge is two vertex indices");
                const auto a = static_cast<std::uint32_t>(e[0]), b = static_cast<std::uint32_t>(e[1]);
                if (a >= m.verts.size() || b >= m.verts.size()) return Result::failure("edge vertex out of range");
                const int cuts = static_cast<int>(intParam(p, "cuts", 1));
                if (cuts < 1 || cuts > 100) return Result::failure("cuts is 1 to 100");
                const std::size_t n = loopCut(m, edgeKey(a, b), cuts, numberParam(p, "factor", 0.5));
                if (!n) return Result::failure("no quads to cut across that edge");
                d.scene.selectMode = SelectMode::Edge;
                return done("loop cut: " + count(n, "new edge"), d, p);
            })));

    reg.add({"model.bisect",
             "Bisect",
             "Cut the mesh with a plane through point= facing normal= (world space). clear_inner deletes what's behind "
             "the plane, clear_outer what's in front; fill caps the cut. Only selected faces are cut when some are "
             "selected (selected=false cuts everything).",
             withDoc({objectParam(),
                      {"point", ParamType::NumberList, "a point on the plane (default: the object origin)", false},
                      {"normal", ParamType::NumberList, "the plane's normal", false},
                      {"clear_inner", ParamType::Bool, "delete behind the plane", false, {}, false},
                      {"clear_outer", ParamType::Bool, "delete in front of the plane", false, {}, false},
                      {"fill", ParamType::Bool, "cap the cut", false, {}, false},
                      {"selected", ParamType::Bool, "only cut selected faces (default: when any are)", false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const Mat4 w = d.world(o), inv = w.inverse();
                const Vec3 point = inv.point(vec3(p, "point", w.point({0, 0, 0})));
                const Vec3 normal = inv.normal(vec3(p, "normal", {0, 0, 1}));
                std::vector<char> filter;
                const bool onlySel = has(p, "selected") ? boolParam(p, "selected") : !selectedFaces(m).empty();
                if (onlySel)
                    for (const auto& f : m.faces) filter.push_back(f.sel);
                const std::size_t n = bisect(m, point, normal, boolParam(p, "clear_inner", false), boolParam(p, "clear_outer", false),
                                             boolParam(p, "fill", false), onlySel ? &filter : nullptr);
                return done("bisect: " + count(n, "edge") + " along the cut", d, p);
            })));

    reg.add({"model.knife",
             "Knife cut",
             "Cut along a line as seen from the view (K): points=x0,y0,z0,x1,y1,z1 in world space, or line=u0,v0,u1,v1 "
             "on the viewport (0..1 from the top left, with view/yaw/pitch). Cuts the faces facing you; through=true "
             "cuts the back too. selected=true limits it to the selection.",
             withDoc({objectParam(),
                      {"points", ParamType::NumberList, "start and end in world space", false},
                      {"line", ParamType::NumberList, "start and end on the viewport (u0,v0,u1,v1)", false},
                      {"through", ParamType::Bool, "cut through to the back", false, {}, false},
                      {"selected", ParamType::Bool, "only selected faces", false, {}, false},
                      {"width", ParamType::Int, "viewport width for line=", false, {}, std::int64_t{512}},
                      {"height", ParamType::Int, "viewport height for line=", false, {}, std::int64_t{512}}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                d.refresh();
                const int W = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 512), 16, 4096));
                const int H = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 512), 16, 4096));
                const ViewBuffer vb = viewBuffer(d, viewFrom(p), W, H, true);
                Vec3 a, b;
                if (has(p, "points")) {
                    const NumberList l = listParam(p, "points");
                    if (l.size() != 6) return Result::failure("points is x0,y0,z0,x1,y1,z1");
                    a = {l[0], l[1], l[2]};
                    b = {l[3], l[4], l[5]};
                } else if (has(p, "line")) {
                    const NumberList l = listParam(p, "line");
                    if (l.size() != 4) return Result::failure("line is u0,v0,u1,v1");
                    a = vb.proj.ray(l[0] * W, l[1] * H);
                    b = vb.proj.ray(l[2] * W, l[3] * H);
                } else {
                    return Result::failure("give points= or line=");
                }
                const Mat4 inv = d.world(o).inverse();
                const std::size_t n = knife(m, inv.point(a), inv.point(b), inv.direction(vb.proj.forward), boolParam(p, "through", false),
                                            boolParam(p, "selected", false));
                if (!n) return Result::failure("the line doesn't cross any faces");
                return done("knife: " + count(n, "new edge"), d, p);
            })));

    reg.add({"model.subdivide",
             "Subdivide",
             "Split the selected faces: quads into a grid of (cuts+1)^2, triangles likewise, n-gons into quads around "
             "a middle point. (For a smooth result use the subsurf modifier.)",
             withDoc({objectParam(), {"cuts", ParamType::Int, "cuts per edge", false, {}, std::int64_t{1}}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const int cuts = static_cast<int>(intParam(p, "cuts", 1));
                if (cuts < 1 || cuts > 32) return Result::failure("cuts is 1 to 32");
                const std::size_t n = subdivide(m, cuts);
                if (!n) return Result::failure("select faces to subdivide");
                return done("subdivided into " + count(n, "face"), d, p);
            })));

    reg.add({"model.merge",
             "Merge vertices",
             "Merge (M): distance welds selected vertices closer than distance (all=true: every vertex; Blender's "
             "merge by distance); center, first, last, cursor collapse the selection into one vertex there; collapse "
             "makes one vertex per connected piece.",
             withDoc({objectParam(),
                      {"mode", ParamType::String, "how", false, {"distance", "center", "first", "last", "cursor", "collapse"}, std::string("distance")},
                      {"distance", ParamType::Number, "merge distance", false, {}, 0.0001},
                      {"all", ParamType::Bool, "distance: every vertex, not just the selection", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                const auto mode = parseMergeMode(stringParam(p, "mode", "distance"));
                const Vec3 cursor = d.world(o).inverse().point(d.scene.cursor);
                const std::size_t n = merge(m, *mode, numberParam(p, "distance", 0.0001), cursor, boolParam(p, "all", false));
                return done("merged away " + count(n, "vertex"), d, p).with("removed", static_cast<std::int64_t>(n));
            })));

    reg.add({"model.dissolve",
             "Dissolve",
             "Remove elements but keep the surface closed (X > Dissolve): verts joins the faces around each selected "
             "vertex, edges joins the faces on both sides, faces merges each selected area into one n-gon, limited "
             "joins faces flatter than angle (on the selection, or everything when nothing is selected). Default: the "
             "select mode. clean drops leftover vertices in the middle of straight edges.",
             withDoc({objectParam(),
                      {"type", ParamType::String, "what to dissolve", false, {"verts", "edges", "faces", "limited"}},
                      {"angle", ParamType::Number, "limited: max angle between faces", false, {}, 5.0},
                      {"clean", ParamType::Bool, "remove leftover two-edge vertices", false, {}, true}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::string type = stringParam(p, "type", modeDefault(d));
                const bool clean = boolParam(p, "clean", true);
                std::size_t n = 0;
                if (type == "verts") n = dissolveVerts(m);
                else if (type == "edges") n = dissolveEdges(m, clean);
                else if (type == "faces") n = dissolveFaces(m, clean);
                else n = limitedDissolve(m, numberParam(p, "angle", 5), countSelection(m).verts > 0);
                return done("dissolved " + std::to_string(n) + " " + type, d, p);
            })));

    reg.add({"model.delete",
             "Delete",
             "Delete (X) the selected verts (and everything using them), edges (and their faces), faces (and edges and "
             "vertices only they use), or only_faces (keeping the edges). Default: the select mode.",
             withDoc({objectParam(), {"type", ParamType::String, "what to delete", false, {"verts", "edges", "faces", "only_faces"}}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const std::string type = stringParam(p, "type", modeDefault(d));
                const std::size_t n = remove(m, *parseDeleteMode(type));
                if (!n) return Result::failure("nothing selected to delete");
                return done("deleted " + std::to_string(n) + " " + type, d, p);
            })));
}

}  // namespace wetwaresuite::model::cmd
