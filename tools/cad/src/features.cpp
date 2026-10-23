// Sketch-based and reference features: sketch, datum plane and axis, extrude
// and revolve of sketch profiles, loft, sweep, draft, rib and thread.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/topology.hpp"
#include "blend.hpp"
#include "build.hpp"
#include "sketch_solve.hpp"

namespace wetwaresuite::cad::detail {

using mesh::Mat4;
using mesh::Mesh;
using mesh::Polygon2;
using mesh::Vec2;
using mesh::Vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string g17(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12g", std::fabs(v) < 1e-13 ? 0.0 : v);
    return buf;
}

std::string vecSig(const Vec3& v) { return g17(v.x) + "," + g17(v.y) + "," + g17(v.z); }

double ringArea(const std::vector<Vec2>& r) {
    double s = 0;
    for (std::size_t i = 0; i < r.size(); ++i) s += mesh::cross(r[i], r[(i + 1) % r.size()]);
    return s / 2;
}

Frame frameFromNormal(const Vec3& nIn, double offset) {
    const Vec3 n = mesh::normalize(nIn);
    Vec3 ur{1, 0, 0}, vr{0, 1, 0};
    const double ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    if (ax >= ay && ax >= az) {
        ur = {0, 1, 0};
        vr = {0, 0, 1};
    } else if (ay >= az) {
        ur = {1, 0, 0};
        vr = {0, 0, 1};
    }
    Frame f;
    f.n = n;
    f.u = mesh::normalize(ur - n * mesh::dot(ur, n));
    Vec3 v = vr - n * mesh::dot(vr, n);
    v = v - f.u * mesh::dot(v, f.u);
    f.v = mesh::normalize(v);
    f.origin = n * offset;
    return f;
}

Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = mesh::dot(ab, ap), d2 = mesh::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const Vec3 bp = p - b;
    const double d3 = mesh::dot(ab, bp), d4 = mesh::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const Vec3 cp = p - c;
    const double d5 = mesh::dot(ab, cp), d6 = mesh::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double den = 1 / (va + vb + vc);
    return a + ab * (vb * den) + ac * (vc * den);
}

// ---- solids from rings ------------------------------------------------------

// Station k, ring j (outer first, then holes), point i. Side walls join each
// station to the next; caps close the first and last stations (their 2D
// shapes, with the same ring point order). closed joins the last station back
// to the first (no caps). The result faces outwards.
Mesh stackRings(const std::vector<std::vector<std::vector<Vec3>>>& st, const Polygon2* firstCap, const Polygon2* lastCap, bool closed) {
    Mesh m;
    if (st.size() < 2) return m;
    std::vector<std::uint32_t> ringStart;
    std::uint32_t total = 0;
    for (const auto& r : st[0]) {
        ringStart.push_back(total);
        total += static_cast<std::uint32_t>(r.size());
    }
    for (const auto& s : st) {
        for (const auto& r : s) {
            for (const auto& p : r) m.addVertex(p);
        }
    }
    const std::size_t ns = st.size();
    const std::size_t walls = closed ? ns : ns - 1;
    for (std::size_t k = 0; k < walls; ++k) {
        const std::uint32_t s0 = static_cast<std::uint32_t>(k) * total, s1 = static_cast<std::uint32_t>((k + 1) % ns) * total;
        for (std::size_t j = 0; j < st[0].size(); ++j) {
            const auto n = static_cast<std::uint32_t>(st[0][j].size());
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint32_t a = s0 + ringStart[j] + i, b = s0 + ringStart[j] + (i + 1) % n;
                const std::uint32_t a1 = s1 + ringStart[j] + i, b1 = s1 + ringStart[j] + (i + 1) % n;
                m.addTriangle(a, b, b1);
                m.addTriangle(a, b1, a1);
            }
        }
    }
    auto cap = [&](const Polygon2& shape, std::uint32_t base, bool top) {
        const auto tris = mesh::triangulate(shape);
        std::vector<Vec2> flat = shape.outer;
        for (const auto& h : shape.holes) flat.insert(flat.end(), h.begin(), h.end());
        // CCW triangles walk a CCW outer ring forwards; the bottom cap must walk it backwards
        const bool forward = ringArea(shape.outer) > 0;
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            std::uint32_t a = tris[t], b = tris[t + 1], c = tris[t + 2];
            if (mesh::cross(flat[b] - flat[a], flat[c] - flat[a]) < 0) std::swap(b, c);
            const bool keep = top == forward;
            if (keep) m.addTriangle(base + a, base + b, base + c);
            else m.addTriangle(base + a, base + c, base + b);
        }
    };
    if (!closed) {
        if (firstCap) cap(*firstCap, 0, false);
        if (lastCap) cap(*lastCap, static_cast<std::uint32_t>(ns - 1) * total, true);
    }
    mesh::cleanup(m);
    if (mesh::volume(m) < 0) mesh::flip(m);
    return m;
}

// Offsets a ring sideways by d along its right-hand normals (outwards for a
// counter-clockwise outer ring and for a clockwise hole), with mitred corners.
std::vector<Vec2> offsetRing(const std::vector<Vec2>& r, double d) {
    const std::size_t n = r.size();
    std::vector<Vec2> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Vec2 p = r[(i + n - 1) % n], c = r[i], q = r[(i + 1) % n];
        Vec2 t1 = c - p, t2 = q - c;
        t1 = t1 / std::max(mesh::length(t1), 1e-300);
        t2 = t2 / std::max(mesh::length(t2), 1e-300);
        const Vec2 n1{t1.y, -t1.x}, n2{t2.y, -t2.x};
        Vec2 mdir = n1 + n2;
        const double ml = mesh::length(mdir);
        if (ml < 1e-9) {
            out[i] = c + n1 * d;
            continue;
        }
        mdir = mdir / ml;
        out[i] = c + mdir * (d / std::max(mesh::dot(mdir, n1), 0.05));
    }
    return out;
}

Polygon2 oriented(Polygon2 p) {
    if (ringArea(p.outer) < 0) std::reverse(p.outer.begin(), p.outer.end());
    for (auto& h : p.holes) {
        if (ringArea(h) > 0) std::reverse(h.begin(), h.end());
    }
    return p;
}

Polygon2 cleaned(const Polygon2& p) {
    auto clean = [](const std::vector<Vec2>& r) {
        std::vector<Vec2> out;
        for (const auto& q : r) {
            if (out.empty() || mesh::length(out.back() - q) > 1e-12) out.push_back(q);
        }
        while (out.size() > 1 && mesh::length(out.front() - out.back()) <= 1e-12) out.pop_back();
        return out;
    };
    Polygon2 o{clean(p.outer), {}};
    for (const auto& h : p.holes) o.holes.push_back(clean(h));
    return o;
}

// A region swept along the frame's normal from z0 to z1, its rings offset by
// -tan(draft) * |z| (draft > 0 narrows the solid away from the plane).
Mesh prism(const Polygon2& regionIn, const Frame& f, double dirSign, double z0, double z1, double draftDeg) {
    const Polygon2 region = oriented(cleaned(regionIn));
    Mat4 toWorld = f.matrix();
    if (dirSign < 0) toWorld = toWorld * Mat4::scale({1, 1, -1});
    if (std::fabs(draftDeg) < 1e-12) {
        Mesh m = mesh::extrude(region, z1 - z0);
        mesh::transform(m, toWorld * Mat4::translate({0, 0, z0}));
        return m;
    }
    const double k = std::tan(draftDeg * kPi / 180);
    std::vector<double> zs{z0};
    if (z0 < 0 && z1 > 0) zs.push_back(0);
    zs.push_back(z1);
    std::vector<std::vector<std::vector<Vec3>>> st;
    std::vector<Polygon2> shapes;
    for (double z : zs) {
        Polygon2 s;
        s.outer = offsetRing(region.outer, -k * std::fabs(z));
        for (const auto& h : region.holes) s.holes.push_back(offsetRing(h, -k * std::fabs(z)));
        if (ringArea(s.outer) <= 0) throw std::runtime_error("the draft angle is too steep for this depth (the profile closes up)");
        for (const auto& h : s.holes) {
            if (ringArea(h) >= 0) throw std::runtime_error("the draft angle is too steep for this depth (a hole closes up)");
        }
        std::vector<std::vector<Vec3>> rings;
        auto lift = [&](const std::vector<Vec2>& r) {
            std::vector<Vec3> out;
            for (const auto& p : r) out.push_back(toWorld.point({p.x, p.y, z}));
            return out;
        };
        rings.push_back(lift(s.outer));
        for (const auto& h : s.holes) rings.push_back(lift(h));
        st.push_back(std::move(rings));
        shapes.push_back(std::move(s));
    }
    return stackRings(st, &shapes.front(), &shapes.back(), false);
}

// ---- references -------------------------------------------------------------

const SolvedSketch& sketchOf(const Ctx& c, const Done& done, const std::string& key) {
    const auto* in = input(done, c, key);
    if (!in->extra || !in->extra->sketch) c.fail(key + " must name a sketch ('" + c.text(key) + "' isn't one)");
    return *in->extra->sketch;
}

const CadDoc::Cached* targetOf(const Ctx& c, const Done& done) {
    if (!c.has("target") || c.text("operation") == "new") return nullptr;
    const auto* t = input(done, c, "target");
    return t->mesh ? t : nullptr;
}

// The finished solid of a sketch feature: new, or combined with the target.
Built combine(const Ctx& c, const Done& done, Mesh tool) {
    if (tool.empty()) c.fail("produced no solid");
    const auto* target = targetOf(c, done);
    const std::string op = c.text("operation");
    Built out;
    if (!target || op == "new") {
        if (op == "cut" || op == "intersect") {
            if (!c.has("target")) c.fail(op + " needs a target body");
            return out;  // the target is suppressed or empty
        }
        out.mesh = share(std::move(tool));
        return out;
    }
    const auto kind = op == "cut" ? mesh::BooleanOp::Difference : op == "intersect" ? mesh::BooleanOp::Intersection : mesh::BooleanOp::Union;
    out.mesh = share(csg(*target->mesh, tool, kind));
    if (op == "cut") out.tools.push_back(std::make_shared<const Mesh>(std::move(tool)));
    return out;
}

double modelExtent(const CadDoc& doc, const Done& done, const Vec3& from) {
    mesh::Bounds b;
    for (const auto& [name, r] : done) {
        if (r.mesh) b.merge(mesh::bounds(*r.mesh));
    }
    if (b.empty()) return 1000;
    (void)doc;
    return 2 * (b.diagonal() + mesh::length(b.center() - from)) + 1;
}

std::vector<std::size_t> pickRegions(const Ctx& c, const SolvedSketch& s) {
    std::vector<std::size_t> out;
    const auto items = splitList(c.text("regions"));
    if (items.empty()) {
        for (std::size_t i = 0; i < s.regions.size(); ++i) out.push_back(i);
        return out;
    }
    for (const auto& it : items) {
        const long long k = std::llround(c.doc.eval(it));
        if (k < 1 || k > static_cast<long long>(s.regions.size())) {
            c.fail("regions: the sketch has " + std::to_string(s.regions.size()) + " regions, there's no " + it);
        }
        out.push_back(static_cast<std::size_t>(k - 1));
    }
    return out;
}

void needProfiles(const Ctx& c, const SolvedSketch& s, const std::string& name) {
    if (!s.profileProblem.empty()) c.fail("sketch " + name + ": " + s.profileProblem);
    if (s.regions.empty()) c.fail("sketch " + name + " has no closed profile");
}

// ---- features -----------------------------------------------------------------

Built buildSketch(const Ctx& c, const Done& done) {
    Frame frame;
    bool onFace = false;
    if (c.has("on")) {
        const auto* on = input(done, c, "on");
        if (on->extra && on->extra->plane && !on->mesh) {
            frame = *on->extra->plane;
        } else if (on->mesh) {
            std::string error;
            if (!facePlane(*on->mesh, c.text("face"), c.doc, frame, error)) c.fail(error);
            onFace = true;
        } else {
            c.fail("on: '" + c.text("on") + "' is neither a body nor a datum plane");
        }
        frame.origin = frame.origin + frame.n * c.number("offset");
    } else {
        frame = standardFrame(c.text("plane"), c.number("offset"));
    }
    SolvedSketch solved;
    try {
        solved = solveSketch(c.f.sketch, frame, [&](const std::string& e) { return c.doc.eval(e); }, c.whole("segments", 3, 1024));
    } catch (const std::exception& e) {
        c.fail(e.what());
    }
    solved.onFace = onFace;
    std::string sig = "sketch|" + vecSig(frame.origin) + vecSig(frame.u) + vecSig(frame.v) + vecSig(frame.n) + (onFace ? "f" : "") + "|" +
                      c.text("segments");
    for (const auto& e : c.f.sketch.entities) {
        sig += "|" + e.kind + (e.construction ? "*" : "") + (e.closed ? "o" : "");
        for (const auto& p : e.points) {
            const Vec2 q = solved.points.at(p);
            sig += ":" + g17(q.x) + "," + g17(q.y);
        }
        if (solved.radii.count(e.id)) sig += "r" + g17(solved.radii.at(e.id));
    }
    auto extra = std::make_shared<FeatureExtra>();
    extra->plane = frame;
    extra->sketch = std::make_shared<const SolvedSketch>(std::move(solved));
    Built b;
    b.extra = extra;
    b.signature = sig;
    return b;
}

Built buildDatumPlane(const Ctx& c, const Done& done) {
    Frame f;
    const auto pts = c.points3("points");
    if (!pts.empty()) {
        if (pts.size() != 3) c.fail("points needs exactly three x,y,z points");
        const Vec3 n = mesh::cross(pts[1] - pts[0], pts[2] - pts[0]);
        if (mesh::length(n) < 1e-12) c.fail("the three points are in a line");
        f = frameFromNormal(n, mesh::dot(mesh::normalize(n), pts[0]));
        f.origin = pts[0];
    } else if (c.has("on")) {
        const auto* on = input(done, c, "on");
        if (on->extra && on->extra->plane) {
            f = *on->extra->plane;
        } else if (on->mesh) {
            std::string error;
            if (!facePlane(*on->mesh, c.text("face"), c.doc, f, error)) c.fail(error);
        } else {
            c.fail("on: '" + c.text("on") + "' has no face or plane");
        }
    } else {
        f = standardFrame(c.text("plane"), 0);
    }
    f.origin = f.origin + f.n * c.number("offset");
    const double angle = c.number("angle");
    if (angle != 0) {
        const Vec3 axis = c.text("axis") == "v" ? f.v : f.u;
        const Mat4 r = Mat4::rotateAxis(axis, angle);
        f.u = r.direction(f.u);
        f.v = r.direction(f.v);
        f.n = r.direction(f.n);
    }
    auto extra = std::make_shared<FeatureExtra>();
    extra->plane = f;
    Built b;
    b.extra = extra;
    return b;
}

Built buildDatumAxis(const Ctx& c, const Done& done) {
    Axis a;
    const auto pts = c.points3("points");
    if (!pts.empty()) {
        if (pts.size() != 2) c.fail("points needs exactly two x,y,z points");
        if (mesh::length(pts[1] - pts[0]) < 1e-12) c.fail("the two points are the same");
        a.point = pts[0];
        a.dir = mesh::normalize(pts[1] - pts[0]);
    } else if (c.has("on")) {
        const auto* on = input(done, c, "on");
        const int index = c.whole("index", 1, 100000);
        if (!on->extra || on->extra->axes.empty()) c.fail("on: '" + c.text("on") + "' has no axis (holes, cylinders, revolves and circles do)");
        if (index > static_cast<int>(on->extra->axes.size())) c.fail("index: " + c.text("on") + " has " + std::to_string(on->extra->axes.size()) + " axes");
        a = on->extra->axes[static_cast<std::size_t>(index - 1)];
        a.radius = 0;
    } else {
        const std::string ax = c.text("axis");
        a.point = {c.number("cx"), c.number("cy"), c.number("cz")};
        a.dir = ax == "x" ? Vec3{1, 0, 0} : ax == "y" ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    }
    auto extra = std::make_shared<FeatureExtra>();
    extra->axes.push_back(a);
    Built b;
    b.extra = extra;
    return b;
}

Built buildSketchExtrude(const Ctx& c, const Done& done) {
    const SolvedSketch& s = sketchOf(c, done, "sketch");
    needProfiles(c, s, c.text("sketch"));
    const Frame& f = s.frame;
    const std::string end = c.text("end"), op = c.text("operation");
    double dir = (op == "cut" && s.onFace) ? -1 : 1;
    if (c.flag("reverse")) dir = -dir;
    double z0 = 0, z1 = 0;
    const double depth = c.number("depth");
    const double draft = c.number("draft");
    if (!(draft > -89 && draft < 89)) c.fail("draft must be between -89 and 89 degrees");
    bool halfSpace = false;
    Frame stop;
    if (end == "blind" || end == "symmetric") {
        if (!(depth > 0)) c.fail("depth must be positive");
        if (end == "symmetric") {
            z0 = -depth / 2;
            z1 = depth / 2;
        } else {
            const double d2 = c.number("depth2");
            if (d2 < 0) c.fail("depth2 can't be negative");
            z0 = -d2;
            z1 = depth;
        }
    } else if (end == "through_all") {
        z1 = modelExtent(c.doc, done, f.origin);
    } else {  // up_to_face
        const auto* target = c.has("target") ? input(done, c, "target") : nullptr;
        if (!target || !target->mesh) c.fail("up_to_face needs a target body whose face to stop at");
        std::string error;
        if (!facePlane(*target->mesh, c.text("up_to"), c.doc, stop, error)) c.fail("up_to: " + error);
        const Vec3 along = f.n * dir;
        const double cosA = mesh::dot(along, stop.n);
        if (std::fabs(std::fabs(cosA) - 1) < 1e-9) {
            const double d = mesh::dot(stop.origin - f.origin, stop.n) / cosA;
            if (std::fabs(d) < 1e-9) c.fail("up_to: that face is in the sketch plane");
            if (d < 0) dir = -dir;
            z1 = std::fabs(d);
        } else {
            if (std::fabs(cosA) < 1e-6) c.fail("up_to: that face is parallel to the extrusion direction");
            z1 = modelExtent(c.doc, done, f.origin);
            halfSpace = true;
        }
    }
    Mesh tool;
    try {
        for (std::size_t i : pickRegions(c, s)) mesh::append(tool, prism(s.regions[i], f, dir, z0, z1, draft));
        if (halfSpace) {
            // keep the side of the stop plane the sketch is on
            const double L = z1 * 4;
            Mesh box = mesh::box({L, L, L}, true);
            const double side = mesh::dot(f.origin - stop.origin, stop.n) >= 0 ? 1 : -1;
            const Vec3 centre = stop.origin + stop.n * (side * L / 2);
            Frame bf = stop;
            bf.origin = centre;
            mesh::transform(box, bf.matrix());
            tool = csg(tool, box, mesh::BooleanOp::Intersection);
        }
    } catch (const std::invalid_argument& e) {
        c.fail(e.what());
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        if (what.rfind("feature ", 0) == 0) throw;
        c.fail(what);
    }
    Built b = combine(c, done, std::move(tool));
    auto extra = std::make_shared<FeatureExtra>();
    for (std::size_t i = 0; i < s.circleCenters.size(); ++i) {
        extra->axes.push_back({f.at(s.circleCenters[i].x, s.circleCenters[i].y), f.n * dir, s.circleRadii[i]});
    }
    b.extra = extra;
    return b;
}

Built buildSketchRevolve(const Ctx& c, const Done& done) {
    const SolvedSketch& s = sketchOf(c, done, "sketch");
    needProfiles(c, s, c.text("sketch"));
    const Frame& f = s.frame;
    const std::string axis = c.text("axis");
    Vec2 a{0, 0}, d{0, 1};
    if (axis == "x") d = {1, 0};
    else if (axis != "y") {
        const Feature& sf = *c.doc.findFeature(c.text("sketch"));
        const SketchEntity* e = sf.sketch.entity(axis);
        if (!e || e->kind != "line") c.fail("axis must be x, y or a line of the sketch (got '" + axis + "')");
        a = s.points.at(e->points[0]);
        d = s.points.at(e->points[1]) - a;
        if (mesh::length(d) < 1e-12) c.fail("the axis line has no length");
        d = d / mesh::length(d);
    }
    const Vec2 perp{-d.y, d.x};
    double side = 0;
    for (const auto& r : s.regions) {
        for (const auto& p : r.outer) {
            const double rr = mesh::dot(p - a, perp);
            if (std::fabs(rr) > std::fabs(side)) side = rr;
        }
    }
    side = side < 0 ? -1 : 1;
    const double angle = c.number("angle");
    if (!(angle > 0 && angle <= 360)) c.fail("angle must be in (0, 360]");
    const int segments = c.whole("segments", 3, 1024);
    const Vec3 A = f.at(a.x, a.y), D = mesh::normalize(f.u * d.x + f.v * d.y);
    const Vec3 E1 = mesh::normalize((f.u * perp.x + f.v * perp.y) * side);
    Vec3 E2 = mesh::cross(D, E1);
    if (c.flag("reverse")) E2 = E2 * -1;
    Mat4 local;
    const Vec3 cols[4] = {E1, E2, D, A};
    for (int k = 0; k < 4; ++k) {
        local.at(0, k) = cols[k].x;
        local.at(1, k) = cols[k].y;
        local.at(2, k) = cols[k].z;
    }
    if (c.flag("symmetric")) local = local * Mat4::rotateZ(-angle / 2);
    auto toRZ = [&](const std::vector<Vec2>& ring) {
        std::vector<Vec2> out;
        for (const auto& p : ring) {
            double r = mesh::dot(p - a, perp) * side;
            if (r < -1e-9 * (1 + std::fabs(r))) c.fail("the profile crosses the revolve axis");
            out.push_back({std::max(r, 0.0), mesh::dot(p - a, d)});
        }
        return out;
    };
    Mesh tool;
    try {
        for (std::size_t i : pickRegions(c, s)) {
            Mesh m = mesh::revolve(toRZ(s.regions[i].outer), angle, segments);
            for (const auto& h : s.regions[i].holes) m = csg(m, mesh::revolve(toRZ(h), angle, segments), mesh::BooleanOp::Difference);
            mesh::append(tool, m);
        }
    } catch (const std::invalid_argument& e) {
        c.fail(e.what());
    }
    mesh::transform(tool, local);
    Built b = combine(c, done, std::move(tool));
    auto extra = std::make_shared<FeatureExtra>();
    extra->axes.push_back({A, D, 0});
    b.extra = extra;
    return b;
}

// Points around a ring, keeping its corners, spread by length to n in all.
std::vector<Vec3> resample(const std::vector<Vec3>& ring, std::size_t n) {
    const std::size_t m = ring.size();
    if (n <= m) return ring;
    std::vector<double> len(m);
    double total = 0;
    for (std::size_t i = 0; i < m; ++i) total += len[i] = mesh::length(ring[(i + 1) % m] - ring[i]);
    std::vector<std::size_t> extra(m, 0);
    std::size_t given = 0;
    std::vector<std::pair<double, std::size_t>> rest;
    for (std::size_t i = 0; i < m; ++i) {
        const double want = static_cast<double>(n - m) * len[i] / total;
        extra[i] = static_cast<std::size_t>(std::floor(want));
        given += extra[i];
        rest.emplace_back(want - static_cast<double>(extra[i]), i);
    }
    std::sort(rest.rbegin(), rest.rend());
    for (std::size_t k = 0; given < n - m; ++k, ++given) ++extra[rest[k % m].second];
    std::vector<Vec3> out;
    for (std::size_t i = 0; i < m; ++i) {
        const Vec3 p = ring[i], q = ring[(i + 1) % m];
        for (std::size_t k = 0; k <= extra[i]; ++k) out.push_back(mesh::lerp(p, q, static_cast<double>(k) / static_cast<double>(extra[i] + 1)));
    }
    return out;
}

Vec3 newell(const std::vector<Vec3>& r) {
    Vec3 n;
    for (std::size_t i = 0; i < r.size(); ++i) n += mesh::cross(r[i], r[(i + 1) % r.size()]);
    return n;
}

Vec3 centroid(const std::vector<Vec3>& r) {
    Vec3 c;
    for (const auto& p : r) c += p;
    return c / static_cast<double>(r.size());
}

Built buildLoft(const Ctx& c, const Done& done) {
    const auto names = splitList(c.text("sections"));
    if (names.size() < 2) c.fail("sections needs at least two sketches");
    struct Section {
        std::vector<Vec3> ring;
        Frame frame;
    };
    std::vector<Section> secs;
    std::size_t most = 0;
    for (const auto& name : names) {
        auto it = done.find(name);
        if (it == done.end()) c.fail("sections: '" + name + "' " + (c.doc.featureIndex(name) < 0 ? "doesn't exist" : "comes later in the tree"));
        if (!it->second.extra || !it->second.extra->sketch) c.fail("sections: '" + name + "' isn't a sketch");
        const SolvedSketch& s = *it->second.extra->sketch;
        needProfiles(c, s, name);
        if (s.regions.size() != 1 || !s.regions[0].holes.empty()) c.fail("sketch " + name + ": a loft section must be one closed outline without holes");
        Section sec{{}, s.frame};
        for (const auto& p : s.regions[0].outer) sec.ring.push_back(s.frame.at(p.x, p.y));
        most = std::max(most, sec.ring.size());
        secs.push_back(std::move(sec));
    }
    bool same = true;
    for (const auto& s : secs) same = same && s.ring.size() == secs[0].ring.size();
    const int samples = c.whole("samples", 0, 4096);
    const std::size_t n = samples > 0 ? std::max<std::size_t>(static_cast<std::size_t>(samples), most) : same ? most : std::max<std::size_t>(64, most);
    const Vec3 n0 = newell(secs[0].ring);
    for (auto& s : secs) {
        if (mesh::dot(newell(s.ring), n0) < 0) std::reverse(s.ring.begin(), s.ring.end());
        s.ring = resample(s.ring, n);
        if (s.ring.size() != n) c.fail("samples must be at least the number of points of every section (" + std::to_string(most) + ")");
    }
    // start each ring where it lines up best with the previous one
    for (std::size_t k = 1; k < secs.size(); ++k) {
        const auto& prev = secs[k - 1].ring;
        auto& ring = secs[k].ring;
        const Vec3 cp = centroid(prev), cr = centroid(ring);
        std::size_t best = 0;
        double bestCost = 1e300;
        for (std::size_t s = 0; s < n; ++s) {
            double cost = 0;
            for (std::size_t i = 0; i < n && cost < bestCost; ++i) {
                const Vec3 dd = (ring[(i + s) % n] - cr) - (prev[i] - cp);
                cost += mesh::dot(dd, dd);
            }
            if (cost < bestCost) {
                bestCost = cost;
                best = s;
            }
        }
        std::rotate(ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(best), ring.end());
    }
    std::vector<std::vector<std::vector<Vec3>>> st;
    if (c.flag("ruled") || secs.size() == 2) {
        for (const auto& s : secs) st.push_back({s.ring});
    } else {
        const int steps = 8;
        const std::size_t ns = secs.size();
        for (std::size_t k = 0; k + 1 < ns; ++k) {
            for (int q = 0; q < steps; ++q) {
                const double t = static_cast<double>(q) / steps, t2 = t * t, t3 = t2 * t;
                std::vector<Vec3> ring(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const Vec3 p1 = secs[k].ring[i], p2 = secs[k + 1].ring[i];
                    const Vec3 p0 = k > 0 ? secs[k - 1].ring[i] : p1 * 2 - p2;
                    const Vec3 p3 = k + 2 < ns ? secs[k + 2].ring[i] : p2 * 2 - p1;
                    ring[i] = (p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5;
                }
                st.push_back({ring});
            }
        }
        st.push_back({secs.back().ring});
    }
    auto capOf = [](const Section& s) {
        Polygon2 p;
        for (const auto& q : s.ring) p.outer.push_back(s.frame.project(q));
        return p;
    };
    const Polygon2 first = capOf(secs.front()), last = capOf(secs.back());
    Mesh tool;
    try {
        tool = stackRings(st, &first, &last, false);
    } catch (const std::invalid_argument& e) {
        c.fail(e.what());
    }
    return combine(c, done, std::move(tool));
}

Built buildSweep(const Ctx& c, const Done& done) {
    const SolvedSketch& prof = sketchOf(c, done, "profile");
    const SolvedSketch& path = sketchOf(c, done, "path");
    needProfiles(c, prof, c.text("profile"));
    if (prof.regions.size() != 1) c.fail("the profile sketch must have exactly one closed region");
    std::vector<Vec2> p2;
    bool closed = false;
    if (path.chains.size() == 1 && path.loops.empty()) {
        p2 = path.chains[0];
    } else if (path.chains.empty() && path.loops.size() == 1) {
        p2 = path.loops[0];
        closed = true;
    } else {
        c.fail("the path sketch must hold one chain of lines, arcs or splines (open or closed)");
    }
    std::vector<Vec3> P;
    for (const auto& q : p2) {
        const Vec3 w = path.frame.at(q.x, q.y);
        if (P.empty() || mesh::length(w - P.back()) > 1e-9) P.push_back(w);
    }
    if (closed && P.size() > 1 && mesh::length(P.front() - P.back()) <= 1e-9) P.pop_back();
    if (P.size() < 2) c.fail("the path is too short");
    const std::size_t nv = P.size(), nseg = closed ? nv : nv - 1;
    std::vector<Vec3> dirs(nseg);
    std::vector<double> along(nv + 1, 0);
    for (std::size_t j = 0; j < nseg; ++j) {
        const Vec3 d = P[(j + 1) % nv] - P[j];
        dirs[j] = mesh::normalize(d);
        along[j + 1] = along[j] + mesh::length(d);
    }
    // rotation-minimising frames per segment
    std::vector<Vec3> rs(nseg + 1);
    Vec3 ref = std::fabs(dirs[0].z) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    rs[0] = mesh::normalize(ref - dirs[0] * mesh::dot(ref, dirs[0]));
    auto turn = [](const Vec3& r, const Vec3& from, const Vec3& to) {
        const Vec3 ax = mesh::cross(from, to);
        const double s = mesh::length(ax), co = mesh::dot(from, to);
        if (s < 1e-12) return co > 0 ? r : r * -1;
        return Mat4::rotateAxis(ax, std::atan2(s, co) * 180 / kPi).direction(r);
    };
    for (std::size_t j = 1; j < nseg; ++j) rs[j] = turn(rs[j - 1], dirs[j - 1], dirs[j]);
    double twist = c.number("twist");
    if (closed) {
        // the frame comes back rotated: spread the difference along the path
        const Vec3 back = turn(rs[nseg - 1], dirs[nseg - 1], dirs[0]);
        const double defect = std::atan2(mesh::dot(mesh::cross(back, rs[0]), dirs[0]), mesh::dot(back, rs[0])) * 180 / kPi;
        twist += defect;
    }
    const double total = closed ? along[nseg] : along[nv - 1];
    // the profile, in the first segment's frame at the start of the path
    const Vec3 s0 = mesh::cross(dirs[0], rs[0]);
    const Polygon2 shape = oriented(cleaned(prof.regions[0]));
    auto local = [&](const std::vector<Vec2>& ring) {
        std::vector<Vec3> out;
        for (const auto& q : ring) {
            const Vec3 w = prof.frame.at(q.x, q.y) - P[0];
            out.push_back({mesh::dot(w, rs[0]), mesh::dot(w, s0), mesh::dot(w, dirs[0])});
        }
        return out;
    };
    std::vector<std::vector<Vec3>> rings{local(shape.outer)};
    for (const auto& h : shape.holes) rings.push_back(local(h));
    std::vector<std::vector<std::vector<Vec3>>> st;
    for (std::size_t i = 0; i < nv; ++i) {
        // the outgoing segment's frame (the last point uses the incoming one), mitred where the path bends
        const std::size_t j = (!closed && i == nv - 1) ? nseg - 1 : i;
        const Vec3 d = dirs[j];
        const double tw = twist * along[i] / total * kPi / 180;
        const Vec3 r = rs[j] * std::cos(tw) + mesh::cross(d, rs[j]) * std::sin(tw);
        const Vec3 sv = mesh::cross(d, r);
        Vec3 t = d;
        if (closed || (i > 0 && i < nv - 1)) t = mesh::normalize(dirs[(i + nseg - 1) % nseg] + d);
        std::vector<std::vector<Vec3>> station;
        for (const auto& ring : rings) {
            std::vector<Vec3> out;
            for (const auto& q : ring) {
                Vec3 w = P[i] + r * q.x + sv * q.y + d * q.z;
                const double den = mesh::dot(d, t);
                if (std::fabs(den) > 1e-9) w = w + d * (mesh::dot(P[i] - w, t) / den);
                out.push_back(w);
            }
            station.push_back(std::move(out));
        }
        st.push_back(std::move(station));
    }
    Mesh tool;
    try {
        tool = stackRings(st, &shape, &shape, closed);
    } catch (const std::invalid_argument& e) {
        c.fail(e.what());
    }
    if (!(mesh::volume(tool) > 0)) c.fail("the sweep folds over itself (the profile is too big for a bend in the path)");
    return combine(c, done, std::move(tool));
}

Built buildDraft(const Ctx& c, const Done& done) {
    const auto* target = input(done, c, "target");
    if (!target->mesh) return {};
    Vec3 pull;
    directionByName(c.text("pull"), pull);
    const double angle = c.number("angle");
    if (!(std::fabs(angle) < 60)) c.fail("angle must be between -60 and 60 degrees");
    Mesh m;
    m.positions = target->mesh->positions;
    m.indices = target->mesh->indices;
    mesh::weld(m, std::max(mesh::bounds(m).diagonal() * 1e-9, 1e-12));
    const std::size_t nt = m.triangleCount();
    std::vector<Vec3> fn(nt);
    for (std::size_t t = 0; t < nt; ++t) fn[t] = mesh::normalize(m.faceNormal(t));
    std::vector<std::vector<Vec3>> walls(m.positions.size());
    for (std::size_t t = 0; t < nt; ++t) {
        if (std::fabs(mesh::dot(fn[t], pull)) > 1e-6 || mesh::length(fn[t]) == 0) continue;
        for (int k = 0; k < 3; ++k) {
            auto& list = walls[m.indices[t * 3 + static_cast<std::size_t>(k)]];
            bool seen = false;
            for (const auto& n : list) seen = seen || mesh::dot(n, fn[t]) > 1 - 1e-9;
            if (!seen) list.push_back(fn[t]);
        }
    }
    double neutral = 1e300;
    for (const auto& p : m.positions) neutral = std::min(neutral, mesh::dot(p, pull));
    if (c.has("neutral")) neutral = c.number("neutral");
    const double k = std::tan(angle * kPi / 180);
    const Vec3 e1 = mesh::normalize(std::fabs(pull.x) > 0.5 ? mesh::cross(pull, Vec3{0, 1, 0}) : mesh::cross(pull, Vec3{1, 0, 0}));
    const Vec3 e2 = mesh::cross(pull, e1);
    int moved = 0;
    for (std::size_t v = 0; v < m.positions.size(); ++v) {
        const auto& ns = walls[v];
        if (ns.empty()) continue;
        const double h = mesh::dot(m.positions[v], pull) - neutral, target = -h * k;
        // least squares: n_i . w = target for every wall through this vertex, w across pull
        double a11 = 0, a12 = 0, a22 = 0, b1 = 0, b2 = 0;
        for (const auto& n : ns) {
            const double x = mesh::dot(n, e1), y = mesh::dot(n, e2);
            a11 += x * x;
            a12 += x * y;
            a22 += y * y;
            b1 += x * target;
            b2 += y * target;
        }
        const double det = a11 * a22 - a12 * a12;
        Vec3 w;
        if (std::fabs(det) > 1e-9) {
            w = e1 * ((b1 * a22 - b2 * a12) / det) + e2 * ((a11 * b2 - a12 * b1) / det);
        } else {
            w = ns[0] * target;
        }
        m.positions[v] = m.positions[v] + w;
        ++moved;
    }
    if (moved == 0) c.fail("the body has no walls parallel to the pull direction to draft");
    for (std::size_t t = 0; t < nt; ++t) {
        const Vec3 n = m.faceNormal(t);
        if (mesh::length(fn[t]) > 0 && mesh::dot(n, fn[t]) <= 0) c.fail("the draft angle is too large for this body (walls cross)");
    }
    if (!(mesh::volume(m) > 0)) c.fail("the draft angle is too large for this body");
    return {share(std::move(m)), {}, nullptr, {}};
}

// Ray parity: is p inside the closed mesh?
bool insideMesh(const Mesh& m, const Vec3& p) {
    auto parity = [&](Vec3 dir) {
        dir = mesh::normalize(dir);
        int hits = 0;
        for (std::size_t t = 0; t < m.triangleCount(); ++t) {
            if (mesh::rayTriangle(p, dir, m.corner(t, 0), m.corner(t, 1), m.corner(t, 2)) > 0) ++hits;
        }
        return hits % 2 == 1;
    };
    const bool a = parity({0.5773, 0.5774, 0.5775}), b = parity({-0.4123, 0.8167, -0.4036});
    return a == b ? a : parity({0.7071, -0.3013, 0.6398});
}

// Splits a mesh into its connected pieces (triangles sharing vertices).
std::vector<Mesh> pieces(const Mesh& in) {
    Mesh m = in;
    mesh::weld(m, std::max(mesh::bounds(m).diagonal() * 1e-9, 1e-12));
    std::vector<std::size_t> parent(m.positions.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<std::size_t(std::size_t)> root = [&](std::size_t a) { return parent[a] == a ? a : parent[a] = root(parent[a]); };
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const std::size_t a = root(m.indices[t * 3]);
        for (int k = 1; k < 3; ++k) parent[root(m.indices[t * 3 + static_cast<std::size_t>(k)])] = a;
    }
    std::map<std::size_t, Mesh> by;
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        Mesh& out = by[root(m.indices[t * 3])];
        for (int k = 0; k < 3; ++k) out.addVertex(m.corner(t, k));
        const auto base = static_cast<std::uint32_t>(out.positions.size() - 3);
        out.addTriangle(base, base + 1, base + 2);
    }
    std::vector<Mesh> list;
    for (auto& [r, piece] : by) {
        mesh::weld(piece, 1e-12);
        list.push_back(std::move(piece));
    }
    return list;
}

Built buildRib(const Ctx& c, const Done& done) {
    const SolvedSketch& s = sketchOf(c, done, "sketch");
    const auto* target = input(done, c, "target");
    if (!target->mesh) return {};
    if (s.chains.size() != 1 || !s.loops.empty()) c.fail("a rib sketch must hold exactly one open chain of lines");
    const auto& chain = s.chains[0];
    const double t = c.number("thickness");
    if (!(t > 0)) c.fail("thickness must be positive");
    const Mesh& body = *target->mesh;
    const mesh::Bounds bb = mesh::bounds(body);
    const double L = 4 * (bb.diagonal() + mesh::length(bb.center() - s.frame.origin)) + 1;
    Vec2 d = chain.back() - chain.front();
    if (mesh::length(d) < 1e-12) c.fail("the rib line has no length");
    d = d / mesh::length(d);
    const Vec2 nrm{-d.y, d.x};
    // the half of the rib's slab on each side of the line, cut back to the body's box
    Mesh box = mesh::box(bb.size() + Vec3{1e-6, 1e-6, 1e-6} * bb.diagonal(), true);
    mesh::transform(box, Mat4::translate(bb.center()));
    double halfLen = 0;
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) halfLen += mesh::length(chain[i + 1] - chain[i]);
    halfLen /= 2;
    Vec2 mid = chain.front();
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const double l = mesh::length(chain[i + 1] - chain[i]);
        if (halfLen <= l) {
            mid = chain[i] + (chain[i + 1] - chain[i]) * (halfLen / l);
            break;
        }
        halfLen -= l;
    }
    struct Choice {
        Mesh fill;
        double volume = 1e300;
    };
    Choice best[2];
    for (int k = 0; k < 2; ++k) {
        const double side = k == 0 ? 1 : -1;
        Polygon2 region;
        region.outer = chain;
        region.outer.push_back(chain.back() + nrm * (side * L));
        region.outer.push_back(chain.front() + nrm * (side * L));
        Mesh slab;
        try {
            slab = prism(region, s.frame, 1, -t / 2, t / 2, 0);
        } catch (const std::exception& e) {
            c.fail(e.what());
        }
        Mesh gap = csg(csg(slab, box, mesh::BooleanOp::Intersection), body, mesh::BooleanOp::Difference);
        const Vec2 probe2 = mid + nrm * (side * 1e-4 * (bb.diagonal() + 1));
        const Vec3 probe = s.frame.at(probe2.x, probe2.y);
        for (auto& piece : pieces(gap)) {
            if (insideMesh(piece, probe)) {
                best[k].volume = std::fabs(mesh::volume(piece));
                best[k].fill = std::move(piece);
                break;
            }
        }
    }
    int pick = best[0].volume <= best[1].volume ? 0 : 1;
    if (c.flag("flip")) pick = 1 - pick;
    if (best[pick].fill.empty()) c.fail("the rib doesn't reach the body on that side (try flip)");
    return {share(csg(body, best[pick].fill, mesh::BooleanOp::Union)), {}, nullptr, {}};
}

// ISO metric basic profile: radius at fraction f (0..1) of the pitch.
double threadRadius(double f, double rMajor, double depth) {
    f -= std::floor(f);
    const double crest = 0.125, flank = 0.3125, root = 0.25;
    if (f < crest) return rMajor;
    if (f < crest + flank) return rMajor - depth * (f - crest) / flank;
    if (f < crest + flank + root) return rMajor - depth;
    return rMajor - depth * (1 - (f - crest - flank - root) / flank);
}

Built buildThread(const Ctx& c, const Done& done) {
    const auto* target = input(done, c, "target");
    if (!target->mesh) return {};
    const double dMajor = c.number("diameter"), pitch = c.number("pitch"), length = c.number("length");
    if (!(dMajor > 0) || !(pitch > 0) || !(length > 0)) c.fail("diameter, pitch and length must be positive");
    if (pitch > dMajor / 2) c.fail("the pitch is too coarse for that diameter");
    const double turnsD = length / pitch;
    if (turnsD > 400) c.fail("that's more than 400 turns; shorten the thread");
    const int n = c.whole("segments", 8, 128);
    const bool internal = c.flag("internal"), left = c.flag("left");
    if (c.flag("cosmetic")) {
        Vec3 axisDir;
        directionByName(c.text("axis") == "z" ? "+z" : c.text("axis") == "x" ? "+x" : c.text("axis") == "y" ? "+y" : c.text("axis"), axisDir);
        auto extra = std::make_shared<FeatureExtra>();
        extra->axes.push_back({{c.number("x"), c.number("y"), c.number("z")}, axisDir, internal ? dMajor / 2 - 0.5413 * pitch : dMajor / 2});
        return {target->mesh, {}, extra, {}};
    }
    const double rMaj = dMajor / 2, depth = 0.5413 * pitch;
    const double rOther = internal ? (rMaj - depth) * 0.9 : rMaj + pitch;  // the tool's plain side
    Vec3 dir;
    directionByName(c.text("axis") == "z" ? "+z" : c.text("axis") == "x" ? "+x" : c.text("axis") == "y" ? "+y" : c.text("axis"), dir);
    const Vec3 start{c.number("x"), c.number("y"), c.number("z")};
    // a thread that reaches an end of the body runs out past it, so the cut doesn't end on that face
    double lo = 1e300, hi = -1e300;
    for (const auto& v : target->mesh->positions) {
        const double h = mesh::dot(v - start, dir);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    const double tol = 1e-6 * (1 + hi - lo);
    // (by an odd fraction of the pitch, so no row of the tool lies on the end face)
    const double run = 0.6180339887 * pitch;
    const double z0 = lo >= -tol ? -run : 0.0, z1 = length >= hi - tol ? length + run : length;
    const int rows = std::max(2, static_cast<int>(std::ceil((z1 - z0) / pitch * n)));
    Mesh tool;
    // thread surface grid (rows+1) x n, then the plain cylinder's two rings
    for (int k = 0; k <= rows; ++k) {
        const double z = z0 + (z1 - z0) * k / rows;
        for (int j = 0; j < n; ++j) {
            const double th = 2 * kPi * j / n;
            const double f = z / pitch + (left ? 1 : -1) * static_cast<double>(j) / n;
            // the tool overshoots the stock surface a little (past the crest outside, the root inside), so
            // its surface never lies on the stock's and the crest stays the stock's own flat
            const double over = 0.1 * depth;
            const double r = internal ? threadRadius(f, rMaj, depth + over) : threadRadius(f, rMaj + over, depth + over);
            tool.addVertex({r * std::cos(th), r * std::sin(th), z});
        }
    }
    const auto plain0 = static_cast<std::uint32_t>(tool.positions.size());
    for (double z : {z0, z1}) {
        for (int j = 0; j < n; ++j) {
            const double th = 2 * kPi * j / n;
            tool.addVertex({rOther * std::cos(th), rOther * std::sin(th), z});
        }
    }
    auto id = [n](int k, int j) { return static_cast<std::uint32_t>(k * n + ((j % n) + n) % n); };
    const auto un = static_cast<std::uint32_t>(n);
    for (int k = 0; k < rows; ++k) {
        for (int j = 0; j < n; ++j) {
            tool.addTriangle(id(k, j), id(k, j + 1), id(k + 1, j + 1));
            tool.addTriangle(id(k, j), id(k + 1, j + 1), id(k + 1, j));
        }
    }
    for (std::uint32_t j = 0; j < un; ++j) {
        const std::uint32_t a = plain0 + j, b = plain0 + (j + 1) % un, a1 = plain0 + un + j, b1 = plain0 + un + (j + 1) % un;
        tool.addTriangle(a, b1, b);  // facing the other way from the thread surface
        tool.addTriangle(a, a1, b1);
        // bottom and top annuli join the thread surface to the plain ring
        const std::uint32_t t0 = id(0, static_cast<int>(j)), t1 = id(0, static_cast<int>(j) + 1);
        tool.addTriangle(t0, a, b);
        tool.addTriangle(t0, b, t1);
        const std::uint32_t u0 = id(rows, static_cast<int>(j)), u1 = id(rows, static_cast<int>(j) + 1);
        tool.addTriangle(u0, b1, a1);
        tool.addTriangle(u0, u1, b1);
    }
    mesh::cleanup(tool);
    if (mesh::volume(tool) < 0) mesh::flip(tool);
    Mat4 align;
    const Vec3 zAxis{0, 0, 1};
    const Vec3 ax = mesh::cross(zAxis, dir);
    if (mesh::length(ax) > 1e-12) align = Mat4::rotateAxis(ax, std::acos(std::clamp(mesh::dot(zAxis, dir), -1.0, 1.0)) * 180 / kPi);
    else if (dir.z < 0) align = Mat4::rotateX(180);
    mesh::transform(tool, Mat4::translate(start) * align);
    Built b{share(csg(*target->mesh, tool, mesh::BooleanOp::Difference)), {}, nullptr, {}};
    auto extra = std::make_shared<FeatureExtra>();
    extra->axes.push_back({start, dir, internal ? rMaj - depth : rMaj});
    b.extra = extra;
    return b;
}

}  // namespace

Frame standardFrame(const std::string& plane, double offset) {
    Frame f;
    if (plane == "xz") {
        f.u = {1, 0, 0};
        f.v = {0, 0, 1};
        f.n = {0, 1, 0};
    } else if (plane == "yz") {
        f.u = {0, 1, 0};
        f.v = {0, 0, 1};
        f.n = {1, 0, 0};
    }
    f.origin = f.n * offset;
    return f;
}

bool facePlane(const Mesh& body, const std::string& selector, const CadDoc& doc, Frame& out, std::string& error) {
    Vec3 dir;
    if (directionByName(selector, dir)) {
        double best = -1e300;
        bool found = false;
        for (std::size_t t = 0; t < body.triangleCount(); ++t) {
            const Vec3 n = body.faceNormal(t);
            const double l = mesh::length(n);
            if (l == 0 || mesh::dot(n / l, dir) < 1 - 1e-6) continue;
            const double h = mesh::dot(body.corner(t, 0), dir);
            if (h > best) {
                best = h;
                found = true;
            }
        }
        if (!found) {
            error = "the body has no flat face facing " + selector;
            return false;
        }
        out = frameFromNormal(dir, best);
        return true;
    }
    const auto items = splitList(selector);
    if (items.size() != 3) {
        error = "face must be top, bottom, front, back, left, right (+x -x +y -y +z -z) or a point x,y,z on it, not '" + selector + "'";
        return false;
    }
    Vec3 p;
    try {
        p = {doc.eval(items[0]), doc.eval(items[1]), doc.eval(items[2])};
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    double best = 1e300;
    Vec3 n;
    Vec3 on;
    for (std::size_t t = 0; t < body.triangleCount(); ++t) {
        const Vec3 fnrm = body.faceNormal(t);
        if (mesh::length(fnrm) == 0) continue;
        const Vec3 q = closestOnTriangle(p, body.corner(t, 0), body.corner(t, 1), body.corner(t, 2));
        const double d = mesh::length(q - p);
        if (d < best) {
            best = d;
            n = mesh::normalize(fnrm);
            on = body.corner(t, 0);
        }
    }
    if (best == 1e300) {
        error = "the body has no faces";
        return false;
    }
    // snap nearly axis-aligned normals so sketch axes come out exact
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(n[i]) > 1 - 1e-9) {
            Vec3 a;
            a[i] = n[i] > 0 ? 1 : -1;
            n = a;
        }
    }
    out = frameFromNormal(n, mesh::dot(n, on));
    return true;
}

bool isAdvanced(const std::string& type) {
    static const std::vector<std::string> types = {"sketch", "datum_plane", "datum_axis", "sketch_extrude", "sketch_revolve",
                                                   "loft",   "sweep",       "draft",      "rib",            "thread"};
    return std::find(types.begin(), types.end(), type) != types.end();
}

std::string advancedSignature(const Ctx& c) {
    if (c.f.type != "sketch") return {};
    std::string sig = "|sk";
    for (const auto& p : c.f.sketch.points) sig += "|" + p.id + ":" + g17(p.x) + "," + g17(p.y);
    for (const auto& e : c.f.sketch.entities) {
        sig += "|" + e.id + ":" + e.kind + (e.construction ? "*" : "") + (e.closed ? "o" : "") + g17(e.radius);
        for (const auto& p : e.points) sig += "," + p;
    }
    for (const auto& k : c.f.sketch.constraints) {
        sig += "|" + k.id + ":" + k.type + "/" + g17(k.aux);
        for (const auto& r : k.refs) sig += "," + r;
        sig += "=";
        for (const auto& item : splitList(k.value)) {
            try {
                sig += g17(c.doc.eval(item)) + ";";
            } catch (const std::exception&) {
                sig += "!" + item;
            }
        }
    }
    return sig;
}

Built buildAdvanced(const Ctx& c, const Done& done) {
    const std::string& t = c.f.type;
    if (t == "sketch") return buildSketch(c, done);
    if (t == "datum_plane") return buildDatumPlane(c, done);
    if (t == "datum_axis") return buildDatumAxis(c, done);
    if (t == "sketch_extrude") return buildSketchExtrude(c, done);
    if (t == "sketch_revolve") return buildSketchRevolve(c, done);
    if (t == "loft") return buildLoft(c, done);
    if (t == "sweep") return buildSweep(c, done);
    if (t == "draft") return buildDraft(c, done);
    if (t == "rib") return buildRib(c, done);
    if (t == "thread") return buildThread(c, done);
    c.fail("unknown feature type " + t);
}

std::vector<Axis> primitiveAxes(const Ctx& c) {
    const std::string& t = c.f.type;
    std::vector<Axis> out;
    try {
        if (t == "cylinder" || t == "cone" || t == "torus" || t == "revolve") {
            const Mat4 m = c.place();
            double r = 0;
            if (t == "cylinder") r = c.size("diameter", "radius", false);
            out.push_back({m.point({0, 0, 0}), mesh::normalize(m.direction({0, 0, 1})), r});
        } else if (t == "extrude_circle") {
            const Mat4 m = c.place() * c.sketchPlane();
            out.push_back({m.point({0, 0, 0}), mesh::normalize(m.direction({0, 0, 1})), c.size("diameter", "radius", false)});
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

}  // namespace wetwaresuite::cad::detail
