// Edit-mode selection: model.select, select_loop, select_grow,
// select_linked, and model.pick (click in the viewport).
#include <algorithm>
#include <cmath>
#include <limits>

#include "cmd.hpp"

namespace wetwaresuite::model::cmd {

namespace {

struct Snapshot {
    std::vector<char> v, f;
    std::set<EdgeKey> e;
};

Snapshot snap(const PolyMesh& m) {
    Snapshot s;
    for (const auto& v : m.verts) s.v.push_back(v.sel);
    for (const auto& f : m.faces) s.f.push_back(f.sel);
    s.e = m.esel;
    return s;
}

// Combines the new selection (in m) with the old one: replace, add or remove.
void combine(PolyMesh& m, const Snapshot& old, const std::string& how, SelectMode mode) {
    if (how == "set") {
        flushSelection(m, mode);
        return;
    }
    const bool add = how == "add";
    for (std::size_t i = 0; i < m.verts.size(); ++i) m.verts[i].sel = add ? (old.v[i] || m.verts[i].sel) : (old.v[i] && !m.verts[i].sel);
    for (std::size_t i = 0; i < m.faces.size(); ++i) m.faces[i].sel = add ? (old.f[i] || m.faces[i].sel) : (old.f[i] && !m.faces[i].sel);
    std::set<EdgeKey> e;
    for (auto k : old.e)
        if (add || !m.esel.count(k)) e.insert(k);
    if (add) e.insert(m.esel.begin(), m.esel.end());
    m.esel = std::move(e);
    flushSelection(m, mode);
}

std::string how(const Params& p) {
    if (boolParam(p, "deselect", false)) return "remove";
    if (boolParam(p, "extend", false)) return "add";
    return "set";
}

EdgeKey edgeParam(const PolyMesh& m, const Params& p) {
    const NumberList e = listParam(p, "edge");
    if (e.size() != 2) throw std::runtime_error("edge is two vertex indices, e.g. edge=0,1");
    const auto a = static_cast<std::uint32_t>(e[0]), b = static_cast<std::uint32_t>(e[1]);
    if (a >= m.verts.size() || b >= m.verts.size()) throw std::runtime_error("edge vertex out of range");
    const EdgeKey k = edgeKey(a, b);
    const auto all = allEdges(m);
    if (!std::binary_search(all.begin(), all.end(), k)) throw std::runtime_error("vertices " + std::to_string(a) + " and " + std::to_string(b) + " aren't joined by an edge");
    return k;
}

std::vector<ParamSpec> combineParams() {
    return {{"extend", ParamType::Bool, "add to the selection (shift)", false, {}, false},
            {"deselect", ParamType::Bool, "remove from the selection", false, {}, false}};
}

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<ParamSpec> screenParams() {
    return {{"width", ParamType::Int, "viewport width the u,v refer to (its aspect matters)", false, {}, std::int64_t{512}},
            {"height", ParamType::Int, "viewport height", false, {}, std::int64_t{512}},
            {"pan_x", ParamType::Number, "viewport pan (model radii)", false, {}, 0.0},
            {"pan_y", ParamType::Number, "viewport pan", false, {}, 0.0}};
}

OrbitView screenView(const Params& p) {
    OrbitView v = viewFrom(p);
    v.panX = numberParam(p, "pan_x", 0);
    v.panY = numberParam(p, "pan_y", 0);
    return v;
}

}  // namespace

void registerSelectCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"model.select",
             "Select mesh elements",
             "Edit-mode selection on the active mesh in the current select mode (or mode=). type: all, none, invert; "
             "index (indices= vertex or face numbers; for edges pairs of vertex numbers); box (box=x0,y0,z0,x1,y1,z1 in "
             "world space: vertices inside, faces whose middle is inside); rect (rect=u0,v0,u1,v1 on the viewport from "
             "view/yaw/pitch, 0..1 from the top left; only visible elements unless xray=true); normal (faces facing "
             "normal= within angle); material (slot=); non_manifold (holes and odd edges); sharp (edges sharper than "
             "angle); random (ratio=, seed=). extend=true adds, deselect=true removes. model.elements lists indices.",
             withDoc(join({objectParam(),
                           {"type", ParamType::String, "how to select", false,
                            {"all", "none", "invert", "index", "box", "rect", "normal", "material", "non_manifold", "sharp", "random"},
                            std::string("all")},
                           {"mode", ParamType::String, "select mode to switch to", false, {"vert", "edge", "face"}},
                           {"indices", ParamType::NumberList, "type=index: vertex/face indices, or vertex pairs for edges", false},
                           {"box", ParamType::NumberList, "type=box: x0,y0,z0,x1,y1,z1 (world)", false},
                           {"rect", ParamType::NumberList, "type=rect: u0,v0,u1,v1 (0..1 of the viewport)", false},
                           {"xray", ParamType::Bool, "type=rect: also hidden elements", false, {}, false},
                           {"normal", ParamType::NumberList, "type=normal: direction x,y,z (world)", false},
                           {"angle", ParamType::Number, "type=normal/sharp: degrees", false, {}, 30.0},
                           {"slot", ParamType::Int, "type=material: material slot", false, {}, std::int64_t{0}},
                           {"ratio", ParamType::Number, "type=random: share selected", false, {}, 0.5},
                           {"seed", ParamType::Int, "type=random: seed", false, {}, std::int64_t{1}}},
                          join(combineParams(), screenParams()))),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object& o, PolyMesh& m, const Params& p) {
                if (has(p, "mode")) d.scene.selectMode = *parseSelectMode(stringParam(p, "mode"));
                const SelectMode mode = d.scene.selectMode;
                const std::string type = stringParam(p, "type", "all");
                const Snapshot old = snap(m);
                const Mat4 w = d.world(o);
                if (type == "invert") {
                    invertSelection(m, mode);
                    return done("inverted the selection", d, p);
                }
                selectAll(m, false);
                if (type == "all") {
                    selectAll(m, true);
                } else if (type == "none") {
                } else if (type == "index") {
                    const NumberList idx = listParam(p, "indices");
                    if (idx.empty()) return Result::failure("give indices=");
                    if (mode == SelectMode::Edge) {
                        if (idx.size() % 2) return Result::failure("edges are pairs of vertex indices");
                        Params q;
                        for (std::size_t i = 0; i < idx.size(); i += 2) {
                            q["edge"] = NumberList{idx[i], idx[i + 1]};
                            m.esel.insert(edgeParam(m, q));
                        }
                    } else {
                        const std::size_t n = mode == SelectMode::Vertex ? m.verts.size() : m.faces.size();
                        for (double x : idx) {
                            if (x < 0 || x >= static_cast<double>(n))
                                return Result::failure("index " + num(x) + " out of range (0.." + std::to_string(n) + ")");
                            if (mode == SelectMode::Vertex) m.verts[static_cast<std::size_t>(x)].sel = true;
                            else m.faces[static_cast<std::size_t>(x)].sel = true;
                        }
                    }
                } else if (type == "box" || type == "rect") {
                    std::function<bool(const Vec3&)> inside;
                    ViewBuffer vb;
                    if (type == "box") {
                        const NumberList b = listParam(p, "box");
                        if (b.size() != 6) return Result::failure("box is x0,y0,z0,x1,y1,z1");
                        const Vec3 lo{std::min(b[0], b[3]), std::min(b[1], b[4]), std::min(b[2], b[5])};
                        const Vec3 hi{std::max(b[0], b[3]), std::max(b[1], b[4]), std::max(b[2], b[5])};
                        inside = [lo, hi](const Vec3& q) {
                            return q.x >= lo.x && q.y >= lo.y && q.z >= lo.z && q.x <= hi.x && q.y <= hi.y && q.z <= hi.z;
                        };
                    } else {
                        const NumberList r = listParam(p, "rect");
                        if (r.size() != 4) return Result::failure("rect is u0,v0,u1,v1");
                        const int W = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 512), 16, 4096));
                        const int H = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 512), 16, 4096));
                        d.refresh();
                        vb = viewBuffer(d, screenView(p), W, H, true);
                        const double x0 = std::min(r[0], r[2]) * W, x1 = std::max(r[0], r[2]) * W;
                        const double y0 = std::min(r[1], r[3]) * H, y1 = std::max(r[1], r[3]) * H;
                        const bool xray = boolParam(p, "xray", false);
                        inside = [&vb, x0, x1, y0, y1, xray](const Vec3& q) {
                            const Vec2 s = vb.proj.toScreen(q);
                            return s.x >= x0 && s.x <= x1 && s.y >= y0 && s.y <= y1 && (xray || vb.visible(q));
                        };
                    }
                    if (mode == SelectMode::Face) {
                        for (auto& f : m.faces) f.sel = inside(w.point(faceCenter(m, f)));
                    } else {
                        for (auto& v : m.verts) v.sel = inside(w.point(v.p));
                        if (mode == SelectMode::Edge) {
                            for (auto k : allEdges(m))
                                if (m.verts[keyA(k)].sel && m.verts[keyB(k)].sel) m.esel.insert(k);
                        }
                    }
                } else if (type == "normal") {
                    const Vec3 dir = w.inverse().normal(vec3(p, "normal", {0, 0, 1}));
                    selectByNormal(m, dir, numberParam(p, "angle", 30));
                } else if (type == "material") {
                    selectMaterial(m, static_cast<int>(intParam(p, "slot", 0)));
                } else if (type == "non_manifold") {
                    selectNonManifold(m);
                } else if (type == "sharp") {
                    selectSharpEdges(m, numberParam(p, "angle", 30));
                } else if (type == "random") {
                    selectRandom(m, mode, numberParam(p, "ratio", 0.5), static_cast<unsigned>(intParam(p, "seed", 1)));
                }
                if (type == "index" || type == "box" || type == "rect" || type == "random" || type == "all") flushSelection(m, mode);
                combine(m, old, how(p), mode);
                const auto c = countSelection(m);
                return done("selected " + (c.verts == 1 ? std::string("1 vertex") : std::to_string(c.verts) + " vertices") + ", " +
                                many(c.edges, "edge") + ", " + many(c.faces, "face"),
                            d, p);
            })));

    reg.add({"model.select_loop",
             "Select loop or ring",
             "Select the edge loop through edge= (two vertex indices; Alt+click in Blender), or the edge ring across it "
             "(ring=true; Ctrl+Alt+click). In face mode it selects the loop of faces along the ring.",
             withDoc(join({objectParam(),
                           {"edge", ParamType::NumberList, "two vertex indices joined by an edge", true},
                           {"ring", ParamType::Bool, "the ring across instead of the loop along", false, {}, false}},
                          combineParams())),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                const EdgeKey e = edgeParam(m, p);
                const Snapshot old = snap(m);
                const Topology t(m);
                const bool faceMode = d.scene.selectMode == SelectMode::Face;
                const bool ring = boolParam(p, "ring", false) || faceMode;
                const auto edges = ring ? edgeRing(m, t, e) : edgeLoop(m, t, e);
                selectAll(m, false);
                if (faceMode) {
                    const std::set<EdgeKey> rs(edges.begin(), edges.end());
                    for (auto& f : m.faces) {
                        int hits = 0;
                        for (std::size_t i = 0; i < f.v.size(); ++i) hits += rs.count(edgeKey(f.v[i], f.v[(i + 1) % f.v.size()])) ? 1 : 0;
                        f.sel = hits >= 2;
                    }
                    combine(m, old, how(p), SelectMode::Face);
                } else {
                    m.esel.insert(edges.begin(), edges.end());
                    combine(m, old, how(p), SelectMode::Edge);
                    if (d.scene.selectMode == SelectMode::Vertex) flushSelection(m, SelectMode::Edge);
                }
                return done("selected a " + std::string(ring ? "ring" : "loop") + " of " + std::to_string(edges.size()) + " edges", d, p);
            })));

    reg.add({"model.select_grow",
             "Grow or shrink selection",
             "Select more (steps > 0, Ctrl+Numpad+) or less (steps < 0) around the current selection.",
             withDoc({objectParam(), {"steps", ParamType::Int, "rings of neighbours to add (negative removes)", false, {}, std::int64_t{1}}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                growSelection(m, d.scene.selectMode, static_cast<int>(intParam(p, "steps", 1)));
                return done(intParam(p, "steps", 1) >= 0 ? "grew the selection" : "shrank the selection", d, p);
            })));

    reg.add({"model.select_linked",
             "Select linked",
             "Select everything connected to the selection (Ctrl+L); seams=true stops at UV seams.",
             withDoc({objectParam(), {"seams", ParamType::Bool, "stop at seams", false, {}, false}}),
             true},
            edit(session, meshEdit([](ModelDoc& d, Object&, PolyMesh& m, const Params& p) {
                selectLinked(m, d.scene.selectMode, boolParam(p, "seams", false));
                return done("selected linked", d, p);
            })));

    reg.add({"model.pick",
             "Click to select",
             "Select what's under a point of the viewport (u,v from 0..1, top left; with view/yaw/pitch/zoom, width and "
             "height of the view): in object mode the object, in edit mode the nearest visible vertex, edge or face in "
             "the select mode. extend=true toggles it into the selection (shift+click). Nothing under the point clears "
             "the selection unless extend.",
             withDoc(join({{"u", ParamType::Number, "0..1 across", true},
                           {"v", ParamType::Number, "0..1 down", true},
                           {"radius", ParamType::Number, "how close a vertex or edge must be, in px", false, {}, 12.0},
                           {"extend", ParamType::Bool, "toggle into the selection", false, {}, false}},
                          screenParams())),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const int W = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 512), 16, 4096));
                const int H = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 512), 16, 4096));
                const double x = numberParam(p, "u") * W, y = numberParam(p, "v") * H;
                const bool extend = boolParam(p, "extend", false);
                d.refresh();
                Object* a = d.activeObject();
                const bool editing = d.scene.editMode && a && d.meshOf(*a);
                const ViewBuffer vb = viewBuffer(d, screenView(p), W, H, editing);
                const int pix = vb.at(static_cast<int>(x), static_cast<int>(y));
                if (!editing) {
                    const int oi = pix < 0 ? -1 : vb.object[static_cast<std::size_t>(pix)];
                    if (oi < 0) {
                        if (!extend)
                            for (auto& o : d.scene.objects) o.selected = false;
                        return done("nothing there", d, p);
                    }
                    Object& hit = d.scene.objects[static_cast<std::size_t>(oi)];
                    if (!extend)
                        for (auto& o : d.scene.objects) o.selected = false;
                    hit.selected = extend ? !(hit.selected && hit.name == d.scene.active) : true;
                    d.scene.active = hit.name;
                    return done("picked " + hit.name, d, p).with("picked", hit.name);
                }
                PolyMesh& m = d.editMesh(*a);
                const Mat4 w = d.world(*a);
                const double radius = numberParam(p, "radius", 12);
                const int ai = static_cast<int>(a - d.scene.objects.data());
                auto clearIfNeeded = [&] {
                    if (!extend) selectAll(m, false);
                };
                std::string what;
                if (d.scene.selectMode == SelectMode::Face) {
                    const int f = pix >= 0 && vb.object[static_cast<std::size_t>(pix)] == ai ? vb.face[static_cast<std::size_t>(pix)] : -1;
                    clearIfNeeded();
                    if (f >= 0) {
                        auto& face = m.faces[static_cast<std::size_t>(f)];
                        face.sel = extend ? !face.sel : true;
                        what = "face " + std::to_string(f);
                    }
                    flushSelection(m, SelectMode::Face);
                } else if (d.scene.selectMode == SelectMode::Vertex) {
                    int best = -1;
                    double bd = radius * radius;
                    for (std::size_t i = 0; i < m.verts.size(); ++i) {
                        const Vec3 q = w.point(m.verts[i].p);
                        const Vec2 s = vb.proj.toScreen(q);
                        const double dd = (s.x - x) * (s.x - x) + (s.y - y) * (s.y - y);
                        if (dd < bd && vb.visible(q)) {
                            bd = dd;
                            best = static_cast<int>(i);
                        }
                    }
                    clearIfNeeded();
                    if (best >= 0) {
                        auto& v = m.verts[static_cast<std::size_t>(best)];
                        v.sel = extend ? !v.sel : true;
                        what = "vertex " + std::to_string(best);
                    }
                    flushSelection(m, SelectMode::Vertex);
                } else {
                    EdgeKey best = 0;
                    bool found = false;
                    double bd = radius;
                    for (auto k : allEdges(m)) {
                        const Vec3 qa = w.point(m.verts[keyA(k)].p), qb = w.point(m.verts[keyB(k)].p);
                        const Vec2 sa = vb.proj.toScreen(qa), sb = vb.proj.toScreen(qb);
                        const double dx = sb.x - sa.x, dy = sb.y - sa.y, len2 = dx * dx + dy * dy;
                        double t = len2 > 0 ? ((x - sa.x) * dx + (y - sa.y) * dy) / len2 : 0;
                        t = std::clamp(t, 0.0, 1.0);
                        const double ex = sa.x + dx * t - x, ey = sa.y + dy * t - y, dist = std::sqrt(ex * ex + ey * ey);
                        if (dist < bd && vb.visible(qa + (qb - qa) * t)) {
                            bd = dist;
                            best = k;
                            found = true;
                        }
                    }
                    clearIfNeeded();
                    if (found) {
                        if (extend && m.esel.count(best)) m.esel.erase(best);
                        else m.esel.insert(best);
                        what = "edge " + std::to_string(keyA(best)) + "-" + std::to_string(keyB(best));
                    }
                    flushSelection(m, SelectMode::Edge);
                }
                Result r = done(what.empty() ? "nothing there" : "picked " + what, d, p);
                if (!what.empty()) r.with("picked", what);
                return r;
            }));
}

}  // namespace wetwaresuite::model::cmd
