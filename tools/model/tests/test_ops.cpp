// Polygon modeling operations: every operator checked for face counts,
// validity and (on closed shapes) a closed, consistently wound surface with
// the right volume.
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "check.hpp"
#include "wetwaresuite/model/ops.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::model;

namespace {

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

double volumeOf(const PolyMesh& m) { return mesh::volume(triangulate(m).mesh); }

bool closedOk(const PolyMesh& m) {
    std::string why;
    if (!valid(m, &why)) {
        std::printf("  invalid: %s\n", why.c_str());
        return false;
    }
    const Stats s = stats(m);
    if (!s.closed) std::printf("  not closed: boundary %zu nonmanifold %zu consistent %d loose %zu\n", s.boundaryEdges, s.nonManifoldEdges, s.consistent, s.looseEdges);
    return s.closed;
}

bool openOk(const PolyMesh& m) {
    std::string why;
    if (!valid(m, &why)) {
        std::printf("  invalid: %s\n", why.c_str());
        return false;
    }
    return stats(m).manifold;
}

void selectFaceWhere(PolyMesh& m, const Vec3& normal) {
    selectAll(m, false);
    for (auto& f : m.faces)
        if (mesh::dot(faceNormal(m, f), normal) > 0.999) f.sel = true;
    flushSelection(m, SelectMode::Face);
}

void selectEdge(PolyMesh& m, std::uint32_t a, std::uint32_t b) {
    selectAll(m, false);
    m.esel.insert(edgeKey(a, b));
    flushSelection(m, SelectMode::Edge);
}

// The cube vertex at the given corner (signs of x, y, z).
std::uint32_t corner(const PolyMesh& m, int sx, int sy, int sz) {
    for (std::uint32_t i = 0; i < m.verts.size(); ++i) {
        const Vec3& p = m.verts[i].p;
        if ((p.x > 0) == (sx > 0) && (p.y > 0) == (sy > 0) && (p.z > 0) == (sz > 0)) return i;
    }
    return 0;
}

}  // namespace

int main() {
    // ---- primitives ----
    {
        PolyMesh c = primitive("cube");
        CHECK(c.verts.size() == 8 && c.faces.size() == 6 && stats(c).quads == 6);
        CHECK(closedOk(c) && stats(c).euler == 2);
        CHECK(near(volumeOf(c), 8));
        PolyMesh s = primitive("uv_sphere");
        CHECK(s.verts.size() == 32 * 15 + 2 && s.faces.size() == 32 * 16 && closedOk(s) && stats(s).euler == 2 && volumeOf(s) > 4);
        PolyMesh ico = primitive("ico_sphere");
        CHECK(ico.faces.size() == 80 && ico.verts.size() == 42 && closedOk(ico) && volumeOf(ico) > 3.5);
        PolyMesh cyl = primitive("cylinder");
        CHECK(cyl.verts.size() == 64 && cyl.faces.size() == 34 && closedOk(cyl) && volumeOf(cyl) > 6);
        PrimitiveOptions fan;
        fan.fill = "fan";
        PolyMesh cylFan = primitive("cylinder", fan);
        CHECK(cylFan.faces.size() == 32 * 3 && closedOk(cylFan));
        PolyMesh cone = primitive("cone");
        CHECK(cone.verts.size() == 33 && cone.faces.size() == 33 && closedOk(cone) && volumeOf(cone) > 2);
        PolyMesh tor = primitive("torus");
        CHECK(tor.faces.size() == 48 * 12 && closedOk(tor) && stats(tor).euler == 0 && volumeOf(tor) > 0);
        PolyMesh plane = primitive("plane");
        CHECK(plane.faces.size() == 1 && stats(plane).boundaryEdges == 4 && plane.hasUvs());
        PolyMesh grid = primitive("grid");
        CHECK(grid.verts.size() == 121 && grid.faces.size() == 100 && openOk(grid));
        PrimitiveOptions none;
        none.fill = "none";
        PolyMesh circ = primitive("circle", none);
        CHECK(circ.faces.empty() && circ.loose.size() == 32 && stats(circ).looseEdges == 32);
        bool threw = false;
        try {
            PrimitiveOptions bad;
            bad.size = -1;
            primitive("cube", bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- extrude ----
    {
        PolyMesh c = primitive("cube");
        selectFaceWhere(c, {0, 0, 1});
        CHECK(extrude(c, ExtrudeMode::Region, 1) == 4);
        CHECK(c.faces.size() == 10 && c.verts.size() == 12 && closedOk(c) && near(volumeOf(c), 12));
        CHECK(countSelection(c).faces == 1 && near(bounds(c).max.z, 2));
        // two neighbouring faces as one region: one shared outline, 6 sides
        PolyMesh r = primitive("cube");
        selectAll(r, false);
        for (auto& f : r.faces) {
            const Vec3 n = faceNormal(r, f);
            if (n.z > 0.9 || n.x > 0.9) f.sel = true;
        }
        flushSelection(r, SelectMode::Face);
        CHECK(extrude(r, ExtrudeMode::Region, 0.5) == 6);
        CHECK(r.faces.size() == 12 && closedOk(r));
        // individual faces
        PolyMesh ind = primitive("cube");
        selectAll(ind, true);
        CHECK(extrude(ind, ExtrudeMode::Individual, 0.5) == 24);
        CHECK(ind.faces.size() == 30 && closedOk(ind));
        // edges: one plane edge pulled out
        PolyMesh p = primitive("plane");
        std::uint32_t a = 0, b = 0;
        for (std::uint32_t i = 0; i < 4; ++i)
            for (std::uint32_t j = 0; j < 4; ++j)
                if (i != j && p.verts[i].p.y < 0 && p.verts[j].p.y < 0) a = i, b = j;
        selectEdge(p, a, b);
        CHECK(extrude(p, ExtrudeMode::Region, 0, Vec3{0, -1, 0}) == 1);
        CHECK(p.faces.size() == 2 && openOk(p) && near(bounds(p).min.y, -2));
        // a lone vertex becomes an edge
        PrimitiveOptions none;
        none.fill = "none";
        PolyMesh circ = primitive("circle", none);
        selectAll(circ, false);
        circ.verts[0].sel = true;
        CHECK(extrude(circ, ExtrudeMode::Region, 0, Vec3{0, 0, 1}) == 1);
        CHECK(circ.loose.size() == 33 && circ.verts.size() == 33 && countSelection(circ).verts == 1);
    }

    // ---- inset ----
    {
        PolyMesh c = primitive("cube");
        selectFaceWhere(c, {0, 0, 1});
        CHECK(inset(c, 0.25, 0, false) == 4);
        CHECK(c.faces.size() == 10 && closedOk(c) && near(volumeOf(c), 8));
        bool inner = false;
        for (const auto& f : c.faces)
            if (f.sel) inner = near(std::fabs(c.verts[f.v[0]].p.x), 0.75) && near(std::fabs(c.verts[f.v[0]].p.y), 0.75);
        CHECK(inner);
        PolyMesh d = primitive("cube");
        selectFaceWhere(d, {0, 0, 1});
        inset(d, 0.25, -0.5, false);
        CHECK(closedOk(d) && near(volumeOf(d), 8 - 0.5 / 3 * (4 + 2.25 + 3), 1e-6));  // a frustum-shaped dent
        PolyMesh e = primitive("cube");
        selectAll(e, true);
        CHECK(inset(e, 0.2, 0, true) == 24 && e.faces.size() == 30 && closedOk(e));
        // a region of two faces insets as one outline
        PolyMesh g = primitive("grid");
        selectAll(g, true);
        CHECK(inset(g, 0.05, 0, false) == 40 && openOk(g) && g.faces.size() == 140);
    }

    // ---- bevel ----
    {
        PolyMesh c = primitive("cube");
        selectEdge(c, corner(c, -1, -1, 1), corner(c, 1, -1, 1));
        CHECK(bevel(c, 0.2, 1, false) == 1);
        CHECK(c.faces.size() == 7 && c.verts.size() == 10 && closedOk(c) && near(volumeOf(c), 8 - 0.04));
        PolyMesh all = primitive("cube");
        selectAll(all, true);
        CHECK(bevel(all, 0.2, 1, false) == 12 + 8);
        CHECK(all.faces.size() == 26 && all.verts.size() == 24 && closedOk(all) && stats(all).euler == 2);
        PolyMesh round = primitive("cube");
        selectAll(round, true);
        bevel(round, 0.3, 3, false);
        CHECK(round.faces.size() == 6 + 36 + 8 && closedOk(round) && volumeOf(round) < 8 && volumeOf(round) > 7.5);
        PolyMesh one = primitive("cube");
        selectEdge(one, corner(one, -1, -1, 1), corner(one, 1, -1, 1));
        bevel(one, 0.3, 4, false);
        CHECK(one.faces.size() == 6 + 4 && closedOk(one));
        // a quarter-round edge: the volume removed is (1 - pi/4) r^2 along the edge
        CHECK(near(volumeOf(one), 8 - 2 * 0.09 * (1 - mesh::kPi / 4), 0.006));
        PolyMesh v = primitive("cube");
        selectAll(v, false);
        v.verts[corner(v, 1, 1, 1)].sel = true;
        flushSelection(v, SelectMode::Vertex);
        CHECK(bevel(v, 0.3, 1, true) == 1);
        CHECK(v.faces.size() == 7 && v.verts.size() == 10 && closedOk(v) && near(volumeOf(v), 8 - 0.3 * 0.3 * 0.3 / 6));
        // a loop of edges on a grid of quads (two bevelled edges through each vertex)
        PolyMesh cyl = primitive("cylinder");
        selectAll(cyl, false);
        for (const auto& f : cyl.faces)
            if (f.v.size() == 32 && faceNormal(cyl, f).z > 0)
                for (std::size_t i = 0; i < 32; ++i) cyl.esel.insert(edgeKey(f.v[i], f.v[(i + 1) % 32]));
        flushSelection(cyl, SelectMode::Edge);
        CHECK(bevel(cyl, 0.1, 2, false) == 64);
        CHECK(closedOk(cyl) && cyl.faces.size() == 34 + 64);
    }

    // ---- loop cut ----
    {
        PolyMesh c = primitive("cube");
        const std::uint32_t a = corner(c, -1, -1, -1), b = corner(c, 1, -1, -1);
        CHECK(loopCut(c, edgeKey(a, b), 1) == 4);
        CHECK(c.faces.size() == 10 && c.verts.size() == 12 && closedOk(c) && near(volumeOf(c), 8));
        CHECK(countSelection(c).edges == 4);
        PolyMesh d = primitive("cube");
        loopCut(d, edgeKey(corner(d, -1, -1, -1), corner(d, 1, -1, -1)), 3);
        CHECK(d.faces.size() == 18 && closedOk(d));
        PolyMesh s = primitive("uv_sphere");
        // a ring of quads around the sphere: cut one, the triangles at the poles stop it
        const std::uint32_t q = 1;  // a vertex on the first ring under the pole
        Topology t(s);
        EdgeKey down = 0;
        for (auto e : t.vertEdges[q]) {
            const EdgeKey k = t.edges[e];
            const std::uint32_t o = keyA(k) == q ? keyB(k) : keyA(k);
            if (s.verts[o].p.z < s.verts[q].p.z - 1e-6 && o != s.verts.size() - 1) down = k;
        }
        CHECK(loopCut(s, down, 1) == 32);
        CHECK(closedOk(s) && s.faces.size() == 512 + 32);
        PolyMesh g = primitive("grid");
        loopCut(g, edgeKey(0, 1), 1);
        CHECK(openOk(g) && g.faces.size() == 110);
    }

    // ---- bisect, knife ----
    {
        PolyMesh c = primitive("cube");
        CHECK(bisect(c, {0, 0, 0.25}, {0, 0, 1}, false, false, false) == 4);
        CHECK(c.faces.size() == 10 && closedOk(c) && near(volumeOf(c), 8));
        PolyMesh h = primitive("cube");
        bisect(h, {0, 0, 0}, {0, 0, 1}, false, true, true);
        CHECK(h.faces.size() == 6 && closedOk(h) && near(volumeOf(h), 4) && near(bounds(h).max.z, 0));
        PolyMesh slant = primitive("cube");
        bisect(slant, {0, 0, 0}, {1, 1, 1}, true, false, true);
        CHECK(closedOk(slant) && near(volumeOf(slant), 4, 1e-6));
        PolyMesh s = primitive("uv_sphere");
        bisect(s, {0, 0, 0.3}, {0, 0.2, 1}, true, false, true);
        CHECK(closedOk(s));
        PolyMesh k = primitive("cube");
        CHECK(knife(k, {-2, -1.5, 0}, {2, -1.5, 0}, {0, 1, 0}, false, false) == 1);
        CHECK(k.faces.size() == 7 && closedOk(k));
        PolyMesh kt = primitive("cube");
        CHECK(knife(kt, {-2, -1.5, 0}, {2, -1.5, 0}, {0, 1, 0}, true, false) == 4);
        CHECK(kt.faces.size() == 10 && closedOk(kt));
    }

    // ---- subdivide ----
    {
        PolyMesh c = primitive("cube");
        selectAll(c, true);
        CHECK(subdivide(c, 1) == 24 && c.faces.size() == 24 && closedOk(c) && near(volumeOf(c), 8));
        PolyMesh d = primitive("cube");
        selectFaceWhere(d, {0, 0, 1});
        subdivide(d, 2);
        CHECK(d.faces.size() == 5 + 9 && closedOk(d));
        PolyMesh ico = primitive("ico_sphere");
        selectAll(ico, true);
        subdivide(ico, 2);
        CHECK(ico.faces.size() == 80 * 9 && closedOk(ico));
        PolyMesh cyl = primitive("cylinder");
        selectAll(cyl, false);
        for (auto& f : cyl.faces) f.sel = f.v.size() == 32;
        flushSelection(cyl, SelectMode::Face);
        subdivide(cyl, 1);
        CHECK(closedOk(cyl) && cyl.faces.size() == 32 + 64);
    }

    // ---- merge, dissolve, delete ----
    {
        PolyMesh twice = primitive("cube");
        append(twice, primitive("cube"));
        selectAll(twice, true);
        CHECK(merge(twice, MergeMode::Distance, 1e-6) == 8);
        CHECK(twice.verts.size() == 8 && twice.faces.size() == 6 && closedOk(twice));
        PolyMesh p = primitive("plane");
        selectAll(p, false);
        p.verts[0].sel = p.verts[1].sel = true;
        CHECK(merge(p, MergeMode::Center, 0) == 1 && p.faces.size() == 1 && p.faces[0].v.size() == 3);
        PolyMesh g = primitive("grid");
        selectAll(g, false);
        for (auto& v : g.verts) v.sel = std::fabs(v.p.x) < 0.5 && std::fabs(v.p.y) < 0.5;
        flushSelection(g, SelectMode::Vertex);
        merge(g, MergeMode::Collapse, 0);
        CHECK(openOk(g) && g.faces.size() < 100);

        PrimitiveOptions two;
        two.xSubdivisions = two.ySubdivisions = 2;
        PolyMesh q = primitive("grid", two);
        selectAll(q, false);
        Topology t(q);
        for (auto k : t.edges)
            if (t.faceCount(k) == 2) q.esel.insert(k);
        flushSelection(q, SelectMode::Edge);
        CHECK(dissolveEdges(q) == 3 && q.faces.size() == 1 && q.verts.size() == 4);
        PolyMesh c = primitive("cube");
        selectAll(c, false);
        for (auto& f : c.faces) {
            const Vec3 n = faceNormal(c, f);
            f.sel = n.z > 0.9 || n.y < -0.9;
        }
        flushSelection(c, SelectMode::Face);
        CHECK(dissolveFaces(c) == 1 && c.faces.size() == 5 && c.verts.size() == 6 && closedOk(c));
        PolyMesh dv = primitive("grid", two);
        selectAll(dv, false);
        for (auto& v : dv.verts) v.sel = std::fabs(v.p.x) < 1e-9 && std::fabs(v.p.y) < 1e-9;
        CHECK(dissolveVerts(dv) == 1 && dv.faces.size() == 1 && dv.faces[0].v.size() == 8);
        PolyMesh ld = primitive("grid");
        CHECK(limitedDissolve(ld, 5, false) > 0 && ld.faces.size() == 1 && ld.faces[0].v.size() == 4);

        PolyMesh del = primitive("cube");
        selectFaceWhere(del, {0, 0, 1});
        CHECK(remove(del, DeleteMode::Faces) == 1 && del.faces.size() == 5 && stats(del).boundaryEdges == 4 && openOk(del));
        PolyMesh dv2 = primitive("cube");
        selectAll(dv2, false);
        dv2.verts[0].sel = true;
        remove(dv2, DeleteMode::Verts);
        CHECK(dv2.faces.size() == 3 && dv2.verts.size() == 7);
        PolyMesh de = primitive("cube");
        selectEdge(de, corner(de, -1, -1, 1), corner(de, 1, -1, 1));
        remove(de, DeleteMode::Edges);
        CHECK(de.faces.size() == 4 && de.verts.size() == 8);
        PolyMesh of = primitive("plane");
        selectAll(of, true);
        remove(of, DeleteMode::OnlyFaces);
        CHECK(of.faces.empty() && of.loose.size() == 4);
    }

    // ---- bridge, fill, spin, mirror ----
    {
        PrimitiveOptions none;
        none.fill = "none";
        PolyMesh tube = primitive("circle", none);
        PolyMesh top = primitive("circle", none);
        append(tube, top, Mat4::translate({0, 0, 2}) * Mat4::rotateZ(5));
        selectAll(tube, true);
        CHECK(bridge(tube) == 32);
        CHECK(tube.faces.size() == 32 && openOk(tube) && stats(tube).boundaryEdges == 64);
        // the pairing follows the twist: quads stay short
        double longest = 0;
        for (const auto& f : tube.faces)
            for (std::size_t i = 0; i < 4; ++i) longest = std::max(longest, mesh::length(tube.verts[f.v[i]].p - tube.verts[f.v[(i + 1) % 4]].p));
        CHECK(longest < 2.05);
        // two cubes: bridging the facing faces makes one solid
        PolyMesh two = primitive("cube");
        append(two, primitive("cube"), Mat4::translate({4, 0, 0}));
        selectAll(two, false);
        for (auto& f : two.faces) {
            const Vec3 n = faceNormal(two, f), c = faceCenter(two, f);
            f.sel = (n.x > 0.9 && c.x < 2) || (n.x < -0.9 && c.x > 2);
        }
        flushSelection(two, SelectMode::Face);
        CHECK(bridge(two) == 4);
        CHECK(two.faces.size() == 14 && closedOk(two) && near(volumeOf(two), 8 + 8 + 8));
        bool threw = false;
        PolyMesh bad = primitive("circle", none);
        selectAll(bad, true);
        try {
            bridge(bad);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);

        PolyMesh circ = primitive("circle", none);
        selectAll(circ, true);
        CHECK(fill(circ) == 1 && circ.faces.size() == 1 && circ.faces[0].v.size() == 32 && faceNormal(circ, circ.faces[0]).z > 0.99);
        PolyMesh open = primitive("cylinder", none);
        CHECK(fillHoles(open, false) == 2 && closedOk(open) && volumeOf(open) > 6);
        PolyMesh three;
        three.addVertex({0, 0, 0});
        three.addVertex({1, 0, 0});
        three.addVertex({0, 1, 0});
        three.addVertex({1, 1, 0});
        selectAll(three, true);
        CHECK(fill(three) == 1 && three.faces.size() == 1 && three.faces[0].v.size() == 4 && openOk(three));

        // spin a profile with both ends on the axis into a closed solid
        PolyMesh prof;
        prof.addVertex({0, 0, 1});
        prof.addVertex({1, 0, 0.5});
        prof.addVertex({1, 0, -0.5});
        prof.addVertex({0, 0, -1});
        prof.loose = {{0, 1}, {1, 2}, {2, 3}};
        selectAll(prof, true);
        spin(prof, {0, 0, 1}, {0, 0, 0}, 360, 24);
        recalcNormals(prof, false, false);
        CHECK(closedOk(prof) && prof.faces.size() == 24 * 3 && volumeOf(prof) > 2);
        // a quarter turn of a face makes a bent solid
        PolyMesh bend = primitive("plane");
        transform(bend, Mat4::translate({2, 0, 0}) * Mat4::rotateX(90));
        selectAll(bend, true);
        spin(bend, {0, 0, 1}, {0, 0, 0}, 90, 6);
        recalcNormals(bend, false, false);
        CHECK(closedOk(bend) && bend.faces.size() == 2 + 6 * 4 && volumeOf(bend) > 0);
        // a full turn of a face is a ring (torus-like): genus 1
        PolyMesh ring = primitive("plane");
        transform(ring, Mat4::translate({2, 0, 0}) * Mat4::rotateX(90));
        selectAll(ring, true);
        spin(ring, {0, 0, 1}, {0, 0, 0}, 360, 8);
        recalcNormals(ring, false, false);
        CHECK(closedOk(ring) && stats(ring).euler == 0 && ring.faces.size() == 32);

        PolyMesh half = primitive("cube");
        bisect(half, {0, 0, 0}, {1, 0, 0}, true, false, false);
        CHECK(stats(half).boundaryEdges == 4);
        selectAll(half, false);
        CHECK(mirror(half, 0, {0, 0, 0}, true, 1e-6) == 5);
        CHECK(closedOk(half) && near(volumeOf(half), 8) && half.verts.size() == 12);
        PolyMesh inPlace = primitive("cube");
        selectAll(inPlace, true);
        mirror(inPlace, 2, {0, 0, 1}, false, 0);
        CHECK(closedOk(inPlace) && near(bounds(inPlace).min.z, 1) && near(volumeOf(inPlace), 8));
        PolyMesh dupe = primitive("cube");
        selectFaceWhere(dupe, {0, 0, 1});
        CHECK(duplicate(dupe) == 1 && dupe.faces.size() == 7 && countSelection(dupe).faces == 1);
    }

    // ---- normals, triangles, quads, smoothing, moving ----
    {
        PolyMesh c = primitive("cube");
        c.faces[0].v = {c.faces[0].v[3], c.faces[0].v[2], c.faces[0].v[1], c.faces[0].v[0]};
        c.faces[3].v = {c.faces[3].v[3], c.faces[3].v[2], c.faces[3].v[1], c.faces[3].v[0]};
        CHECK(!stats(c).consistent);
        CHECK(recalcNormals(c, false, false) == 2 && closedOk(c) && near(volumeOf(c), 8));
        CHECK(recalcNormals(c, true, false) == 6 && near(volumeOf(c), -8));
        flipNormals(c, false);
        CHECK(near(volumeOf(c), 8));
        PolyMesh t = primitive("cube");
        CHECK(triangulateFaces(t, false) == 6 && t.faces.size() == 12 && closedOk(t));
        CHECK(trisToQuads(t, 5, false) == 6 && t.faces.size() == 6 && stats(t).quads == 6 && closedOk(t));
        PolyMesh hex;
        for (int i = 0; i < 6; ++i) hex.addVertex({std::cos(i * mesh::kPi / 3), std::sin(i * mesh::kPi / 3), 0});
        hex.addVertex({0.2, 0.0, 0});
        hex.addFace({0, 1, 2, 3, 4, 5});
        CHECK(triangulateFace(hex, hex.faces[0]).size() == 12);
        PolyMesh notch;  // a concave L shape triangulates inside itself
        for (Vec3 p : {Vec3{0, 0, 0}, Vec3{2, 0, 0}, Vec3{2, 1, 0}, Vec3{1, 1, 0}, Vec3{1, 2, 0}, Vec3{0, 2, 0}}) notch.addVertex(p);
        notch.addFace({0, 1, 2, 3, 4, 5});
        CHECK(near(mesh::surfaceArea(triangulate(notch).mesh), 3));

        PolyMesh g = primitive("grid");
        for (auto& v : g.verts) v.p.z = (v.p.x > 0 ? 0.3 : -0.3);
        selectAll(g, true);
        smoothVertices(g, 0.5, 10, true);
        double zs = 1;
        for (const auto& v : g.verts)
            if (near(v.p.x, 0.2) && near(v.p.y, 0)) zs = std::fabs(v.p.z);
        CHECK(zs < 0.2);
        PolyMesh m = primitive("grid");
        selectAll(m, false);
        for (auto& v : m.verts) v.sel = near(v.p.x, 0) && near(v.p.y, 0);
        CHECK(transformSelected(m, Mat4::translate({0, 0, 1}), 0.5) > 1);
        double far = 0, nearZ = 0;
        for (const auto& v : m.verts) {
            if (near(v.p.x, 0) && near(v.p.y, 0)) nearZ = v.p.z;
            if (std::fabs(v.p.x) > 0.6) far = std::max(far, std::fabs(v.p.z));
        }
        CHECK(near(nearZ, 1) && far == 0);
        PolyMesh fat = primitive("cube");
        selectAll(fat, true);
        shrinkFatten(fat, 0.1);
        CHECK(bounds(fat).max.x > 1.05);
    }

    // ---- selection tools ----
    {
        PolyMesh s = primitive("uv_sphere");
        Topology t(s);
        // a ring of the sphere (around a latitude) is a loop of 32 edges
        const std::uint32_t q = 40;
        EdgeKey around = 0;
        for (auto e : t.vertEdges[q]) {
            const EdgeKey k = t.edges[e];
            const std::uint32_t o = keyA(k) == q ? keyB(k) : keyA(k);
            if (near(s.verts[o].p.z, s.verts[q].p.z)) around = k;
        }
        CHECK(edgeLoop(s, t, around).size() == 32);
        CHECK(edgeRing(s, t, around).size() == 15);
        PolyMesh g = primitive("grid");
        Topology tg(g);
        CHECK(edgeLoop(g, tg, edgeKey(0, 1)).size() == 40);  // the whole boundary
        CHECK(edgeLoop(g, tg, edgeKey(12, 13)).size() == 10);
        PolyMesh two = primitive("cube");
        append(two, primitive("cube"), Mat4::translate({4, 0, 0}));
        selectAll(two, false);
        two.verts[0].sel = true;
        selectLinked(two, SelectMode::Vertex);
        CHECK(countSelection(two).verts == 8 && countSelection(two).faces == 6);
        PolyMesh gg = primitive("grid");
        selectAll(gg, false);
        gg.faces[55].sel = true;
        flushSelection(gg, SelectMode::Face);
        growSelection(gg, SelectMode::Face, 1);
        CHECK(countSelection(gg).faces == 9);
        growSelection(gg, SelectMode::Face, -1);
        CHECK(countSelection(gg).faces == 1);
        PolyMesh c = primitive("cube");
        selectAll(c, false);
        CHECK(selectByNormal(c, {0, 0, 1}, 10) == 1);
        PolyMesh p = primitive("grid");
        selectAll(p, false);
        CHECK(selectNonManifold(p) == 40);
        PolyMesh cy = primitive("cylinder");
        selectAll(cy, false);
        CHECK(selectSharpEdges(cy, 30) == 64);
        PolyMesh r = primitive("grid");
        selectAll(r, false);
        const std::size_t n = selectRandom(r, SelectMode::Face, 0.5, 7);
        CHECK(n > 20 && n < 80);
        r.faces[0].material = 2;
        selectAll(r, false);
        CHECK(selectMaterial(r, 2) == 1);
    }

    // ---- subdivision surfaces ----
    {
        PolyMesh c = catmullClark(primitive("cube"), 1);
        CHECK(c.faces.size() == 24 && c.verts.size() == 26 && closedOk(c) && volumeOf(c) < 8 && volumeOf(c) > 3);
        PolyMesh c2 = catmullClark(primitive("cube"), 2);
        CHECK(c2.faces.size() == 96 && closedOk(c2));
        PolyMesh sharp = primitive("cube");
        for (auto k : allEdges(sharp)) sharp.sharp.insert(k);
        PolyMesh s = catmullClark(sharp, 2);
        CHECK(near(volumeOf(s), 8));  // every edge sharp: the cube keeps its shape
        PolyMesh p = catmullClark(primitive("plane"), 1);
        CHECK(p.faces.size() == 4 && openOk(p) && near(bounds(p).max.x, 1));
        bool threw = false;
        try {
            catmullClark(primitive("cube"), 3, 100);
        } catch (const std::length_error&) {
            threw = true;
        }
        CHECK(threw);
    }

    // ---- UVs ----
    {
        auto inUnit = [](const PolyMesh& m) {
            for (const auto& f : m.faces) {
                if (f.uv.size() != f.v.size()) return false;
                for (const auto& uv : f.uv)
                    if (!(uv.x >= -1e-9 && uv.x <= 1 + 1e-9 && uv.y >= -1e-9 && uv.y <= 1 + 1e-9)) return false;
            }
            return true;
        };
        auto uvArea = [](const PolyMesh& m) {
            double a = 0;
            for (const auto& f : m.faces)
                for (std::size_t i = 0; i < f.uv.size(); ++i) {
                    const Vec2 p = f.uv[i], q = f.uv[(i + 1) % f.uv.size()];
                    a += p.x * q.y - q.x * p.y;
                }
            return a / 2;
        };
        PolyMesh c = primitive("cube");
        UvOptions o;
        CHECK(unwrap(c, o) == 6 && inUnit(c) && uvArea(c) > 0.3);
        o.method = UvMethod::Cube;
        CHECK(unwrap(c, o) == 6 && inUnit(c));
        // seams for the usual cross layout: unwrap is one island with no flipped faces
        PolyMesh u = primitive("cube");
        auto e = [&](int ax, int ay, int az, int bx, int by, int bz) { u.seams.insert(edgeKey(corner(u, ax, ay, az), corner(u, bx, by, bz))); };
        e(-1, -1, 1, 1, -1, 1);
        e(-1, 1, 1, 1, 1, 1);
        e(-1, -1, 1, -1, 1, 1);
        e(-1, -1, -1, -1, -1, 1);
        e(1, -1, -1, 1, -1, 1);
        e(-1, 1, -1, -1, 1, 1);
        e(1, 1, -1, 1, 1, 1);
        o.method = UvMethod::Unwrap;
        CHECK(unwrap(u, o) == 1 && inUnit(u));
        bool flipped = false;
        for (const auto& f : u.faces) {
            double a = 0;
            for (std::size_t i = 0; i < 4; ++i) a += f.uv[i].x * f.uv[(i + 1) % 4].y - f.uv[(i + 1) % 4].x * f.uv[i].y;
            if (a <= 0) flipped = true;
        }
        CHECK(!flipped);
        // a closed sphere with no seams falls back to smart projection
        PolyMesh s = primitive("ico_sphere");
        CHECK(unwrap(s, o) > 1 && inUnit(s));
        PolyMesh cy = primitive("cylinder");
        o.method = UvMethod::Cylinder;
        CHECK(unwrap(cy, o) == 1 && inUnit(cy));
        o.method = UvMethod::Sphere;
        PolyMesh sp = primitive("uv_sphere");
        CHECK(unwrap(sp, o) == 1 && inUnit(sp));
        o.method = UvMethod::Planar;
        PolyMesh pl = primitive("grid");
        CHECK(unwrap(pl, o) == 1 && inUnit(pl) && near(uvArea(pl), 1 - 0, 0.2));
        PolyMesh sh = primitive("cylinder");
        CHECK(seamsFromSharp(sh, 30) == 64);
        PolyMesh packed = primitive("cube");
        for (std::size_t i = 0; i < packed.faces.size(); ++i) {
            const double o2 = 2.0 * static_cast<double>(i);
            packed.faces[i].uv = {{o2, 0}, {o2 + 1, 0}, {o2 + 1, 1}, {o2, 1}};
        }
        CHECK(packIslands(packed, 0.02, false) == 6 && inUnit(packed));
        const raster::Image img = uvLayout(c, 128, false);
        int filled = 0;
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) filled += img.at(x, y).a > 0 ? 1 : 0;
        CHECK(img.width() == 128 && filled > 4000);
    }

    // ---- triangles out, triangles in ----
    {
        PolyMesh c = primitive("cube");
        Triangles t = triangulate(c);
        CHECK(t.mesh.triangleCount() == 12 && t.face.size() == 12 && t.mesh.hasNormals());
        CHECK(t.mesh.vertexCount() == 24);  // flat: each face has its own corners
        for (auto& f : c.faces) f.smooth = true;
        TriangulateOptions so;
        so.autoSmooth = 30;
        CHECK(triangulate(c, so).mesh.vertexCount() == 24);  // auto smooth keeps the 90 degree edges hard
        so.autoSmooth = 180;
        so.uvs = false;
        CHECK(triangulate(c, so).mesh.vertexCount() == 8);
        PolyMesh back = fromTriangles(triangulate(primitive("ico_sphere")).mesh);
        CHECK(back.verts.size() == 42 && back.faces.size() == 80 && closedOk(back));
        auto parts = looseParts([] {
            PolyMesh two = primitive("cube");
            append(two, primitive("cube"), Mat4::translate({5, 0, 0}));
            return two;
        }());
        CHECK(parts.size() == 2 && parts[1].verts.size() == 8 && closedOk(parts[1]));
        PolyMesh sel = primitive("cube");
        selectFaceWhere(sel, {0, 0, 1});
        CHECK(extractSelected(sel).faces.size() == 1 && extractSelected(sel).verts.size() == 4);
    }

    return check::summary("model ops");
}
