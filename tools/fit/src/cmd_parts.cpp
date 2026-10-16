// fit.* commands that build parts on the scan, add holes and mounts, and rebuild.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "internal.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "parts.hpp"
#include "wetwaresuite/mesh/primitives.hpp"

namespace wetwaresuite::fit::detail {

using mesh::dot;
using mesh::length;
using mesh::normalize;

// ---- shared by parts and presets ----

Frame placeOn(FitDoc& doc, const Params& p, double size, std::string* note) {
    if (!doc.state.scan || doc.state.scan->empty()) throw std::runtime_error("the scan is empty");
    Vec3 origin, n;
    if (p.count("plane") && !stringParam(p, "plane").empty()) {
        const Ref& r = refParam(doc, p, "plane", RefKind::Plane);
        n = r.dir;
        origin = vec3Param(p, "at") ? snap(doc, *vec3Param(p, "at")).point : r.point;
    } else {
        const SurfacePoint sp = snap(doc, requireVec3(p, "at"));
        origin = sp.point;
        const double rad = std::max(8.0, size * 0.6);
        const Patch patch = patchAround(*doc.state.scan, scanVertexNormals(doc), origin, rad);
        if (patch.points.size() >= 3) {
            const PlaneFit f = fitPlane(patch);
            n = f.normal;
            if (dot(n, sp.normal) < 0) n = -n;
            if (note) *note = "surface fitted within " + num(rad) + " mm, flatness " + num(f.rms) + " mm";
        } else {
            n = sp.normal;
        }
    }
    if (boolParam(p, "flip", false)) n = -n;
    return frameFor(origin, n, numberParam(p, "angle", 0));
}

Footprint footprintOf(const Params& p) {
    const std::string shape = stringParam(p, "shape", "rounded");
    Footprint f;
    if (shape == "outline") {
        const NumberList l = listParam(p, "outline");
        if (l.size() < 6 || l.size() % 2) throw std::runtime_error("outline= needs u,v pairs (at least 3 points), mm from the placement point");
        for (std::size_t i = 0; i + 1 < l.size(); i += 2) f.outer.push_back({l[i], l[i + 1]});
    } else if (shape == "circle") {
        f = circleFootprint(numberParam(p, "diameter", 40));
    } else if (shape == "oval") {
        f = ovalFootprint(numberParam(p, "width", 50), numberParam(p, "height", 40));
    } else {
        const double w = numberParam(p, "width", 50), h = numberParam(p, "height", 40);
        f = rectFootprint(w, h, shape == "rect" ? 0 : numberParam(p, "corner", std::min(5.0, std::min(w, h) / 4)));
    }
    const double hole = numberParam(p, "hole_diameter", 0);
    if (hole > 0) f.holes.push_back(circleFootprint(hole).outer);
    return f;
}

double footprintSize(const Footprint& f) {
    Bounds b;
    for (const auto& q : f.outer) b.add({q.x, q.y, 0});
    return std::max(b.size().x, b.size().y);
}

std::string fitSummary(const FitDoc& doc, const Mesh& part, double gap) {
    const FitReport r = checkFit(doc.bvh(), part, 5, gap);
    if (!r.near) return "not near the scan";
    std::string s = "gap to scan " + num(r.minGap) + " to " + num(r.maxGap) + " mm, " + num(r.contact * 100) + "% within gap + 0.5 mm";
    if (r.inside > 0.001) s += ", " + num(r.inside * 100) + "% poking into the scan (raise gap)";
    return s;
}

Result partResult(const std::string& name, FitDoc& doc, const Params& p, const std::string& extra) {
    const Part& part = doc.state.parts.at(name);
    const Vec3 sz = mesh::bounds(*part.mesh).size();
    std::string msg = describePart(name, part);
    if (!extra.empty()) msg += "\n" + extra;
    Result r = done(msg, doc, p);
    r.value = name;
    return r.with("part", name)
        .with("triangles", static_cast<std::int64_t>(part.mesh->triangleCount()))
        .with("size", NumberList{sz.x, sz.y, sz.z})
        .with("watertight", isClosed(*part.mesh));
}

std::string buildConform(FitDoc& doc, const Params& p, bool block, const std::string& prefix, const Footprint* outline, const Frame* frame) {
    ConformSpec s;
    s.footprint = outline ? *outline : footprintOf(p);
    std::string note;
    s.frame = frame ? *frame : placeOn(doc, p, footprintSize(s.footprint), &note);
    s.gap = numberParam(p, "gap", 0.4);
    s.thickness = numberParam(p, "thickness", 3);
    s.block = block;
    if (block) {
        const NumberList tilt = listParam(p, "tilt");
        if (tilt.size() >= 1) s.tiltU = tilt[0];
        if (tilt.size() >= 2) s.tiltV = tilt[1];
    }
    // a test slice is just the contact skin: a thin plate however the part was built
    const double test = numberParam(p, "test_slice", 0);
    if (test > 0) {
        s.block = false;
        s.thickness = test;
    }
    s.resolution = numberParam(p, "resolution", 0);
    s.smoothing = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "smooth", 2), 0, 50));
    ConformStats st;
    Mesh m = conformPart(doc.bvh(), s, &st);
    const std::string name = newName(doc, p, prefix);
    putPart(doc, name, std::move(m), block ? "block" : "plate", s.frame.n, s.gap);
    std::string info = "sampled the scan every " + num(st.resolution) + " mm";
    if (st.missed) info += " (" + std::to_string(st.missed) + " of " + std::to_string(st.samples) + " points fell in holes and were filled in)";
    info += "; surface varies " + num(st.maxHeight - st.minHeight) + " mm under it";
    if (s.block) info += "; " + num(s.thickness) + " to " + num(st.maxThickness) + " mm thick";
    if (!note.empty()) info += "\n" + note;
    return name + "\n" + info + "\n" + fitSummary(doc, *doc.state.parts.at(name).mesh, s.gap);
}

std::vector<ParamSpec> placementParams() {
    return {{"at", ParamType::NumberList, "where on the scan x,y,z (snapped onto it); the part centres here", false},
            {"plane", ParamType::String, "use this plane reference's direction (and its point when at= is not given)", false},
            {"angle", ParamType::Number, "turn the outline about the surface direction, degrees", false, {}, 0.0},
            {"flip", ParamType::Bool, "build on the other side of the surface", false, {}, false}};
}

std::vector<ParamSpec> outlineParams() {
    return {{"shape", ParamType::String, "outline", false, {"rounded", "rect", "circle", "oval", "outline"}, std::string("rounded")},
            {"width", ParamType::Number, "rounded/rect/oval: size along the outline's u, mm", false, {}, 50.0},
            {"height", ParamType::Number, "rounded/rect/oval: size along v, mm", false, {}, 40.0},
            {"diameter", ParamType::Number, "circle: diameter, mm", false, {}, 40.0},
            {"corner", ParamType::Number, "rounded: corner radius, mm", false, {}, 5.0},
            {"outline", ParamType::NumberList, "shape=outline: u,v pairs in mm around the placement point", false},
            {"hole_diameter", ParamType::Number, "cut a round hole this wide in the middle (rings), mm", false, {}, 0.0}};
}

std::vector<ParamSpec> buildParams(double thickness) {
    return {{"gap", ParamType::Number, "clearance between the scan and the part, mm", false, {}, 0.4},
            {"thickness", ParamType::Number, "thickness, mm", false, {}, thickness},
            {"smooth", ParamType::Int, "blur passes over the sampled scan (more = calmer, less exact)", false, {}, std::int64_t{2}},
            {"resolution", ParamType::Number, "sample spacing, mm (0 = automatic)", false, {}, 0.0}};
}

namespace {

std::vector<ParamSpec> concat(std::initializer_list<std::vector<ParamSpec>> lists) {
    std::vector<ParamSpec> out;
    for (const auto& l : lists) out.insert(out.end(), l.begin(), l.end());
    return out;
}

// Where a mount or hole goes on a part: the point on the part and its outward direction.
std::pair<Vec3, Vec3> spotOnPart(const Part& part, const Params& p) {
    const Bvh pb(part.mesh);
    Vec3 at;
    if (auto a = vec3Param(p, "at")) {
        at = *a;
    } else {
        // the middle of the face pointing away from the scan
        const Bounds b = mesh::bounds(*part.mesh);
        Vec3 n = length(part.normal) > 0 ? part.normal : Vec3{0, 0, 1};
        auto hit = pb.ray(b.center() + n * b.diagonal(), -n, 2 * b.diagonal());
        at = hit ? hit->point : b.center();
    }
    const auto near = pb.nearest(at);
    Vec3 n = normalize(part.mesh->faceNormal(near.tri));
    if (auto d = vec3Param(p, "direction")) n = normalize(*d);
    return {near.point, n};
}

Mesh placed(Mesh m, const Vec3& at, const Vec3& n, double angle) {
    mesh::transform(m, frameFor(at, n, angle).matrix());
    return m;
}

void setPartMesh(Part& part, Mesh m) {
    m.normals.clear();
    part.mesh = std::make_shared<const Mesh>(std::move(m));
}

}  // namespace

Mesh drillHole(const Mesh& part, const Vec3& at, const Vec3& n, double d, double depth, double head, bool* ok) {
    const double diag = mesh::bounds(part).diagonal();
    const double len = depth > 0 ? depth : diag * 2;
    Mesh cut = mesh::cylinder(d / 2, len + 1, 24, false);
    mesh::transform(cut, Mat4::translate({0, 0, -len}));
    if (head > d) {
        const double h = (head - d) / 2;  // 90 degree countersink
        Mesh cone = mesh::cone(d / 2, head / 2 + 0.5, h + 0.5, 24, false);
        mesh::transform(cone, Mat4::translate({0, 0, -h}));
        cut = unite(cut, cone);
    }
    return subtract(part, placed(cut, at, n, 0), ok);
}

void registerParts(Session& session) {
    addEdit(session,
            {"fit.plate", "Conform Plate",
             "A plate whose underside hugs the scan at the gap you set, with any outline: the base of a dash mount, a cover, a panel. Place it "
             "with at= (it fits the surface direction there) or plane=. Noisy or holey scan areas are smoothed and filled.",
             withPreview(concat({{docParam()}, placementParams(), outlineParams(), buildParams(3), {nameParam("plate")}}))},
            [](FitDoc& doc, const Params& p) {
                const std::string info = buildConform(doc, p, false, "plate");
                const std::string name = info.substr(0, info.find('\n'));
                return partResult(name, doc, p, info.substr(info.find('\n') + 1));
            });

    addEdit(session,
            {"fit.block", "Conform Block",
             "A block flat on top and matching the scan underneath: spacers, wedges, level pads on a curved dash. tilt=u,v tips the flat top "
             "(degrees) to aim whatever sits on it; thickness is at the thinnest point.",
             withPreview(concat({{docParam()}, placementParams(), outlineParams(), buildParams(3),
                                 {{"tilt", ParamType::NumberList, "tilt of the flat top: degrees about the outline's u, and about v", false}, nameParam("block")}}))},
            [](FitDoc& doc, const Params& p) {
                const std::string info = buildConform(doc, p, true, "block");
                const std::string name = info.substr(0, info.find('\n'));
                return partResult(name, doc, p, info.substr(info.find('\n') + 1));
            });

    addEdit(session,
            {"fit.clamp", "Clamp",
             "A clamp round a fitted cylinder (pipe, bar, headrest post, roll cage, mirror stem): pieces=1 is a ring with a slot and a bolt, "
             "pieces=2 is two halves bolted together.",
             withPreview({docParam(),
                          {"cylinder", ParamType::String, "cylinder reference from fit.cylinder", true},
                          {"gap", ParamType::Number, "clearance round the cylinder, mm (small: the bolt pulls it tight)", false, {}, 0.3},
                          {"thickness", ParamType::Number, "ring wall, mm", false, {}, 4.0},
                          {"width", ParamType::Number, "along the axis, mm", false, {}, 15.0},
                          {"pieces", ParamType::Int, "1 or 2", false, {}, std::int64_t{1}},
                          {"bolt", ParamType::String, "screw size for the tabs (M3..M8), or none", false, {}, std::string("M4")},
                          {"slot", ParamType::Number, "width of the split, mm", false, {}, 2.0},
                          {"angle", ParamType::Number, "which way the tabs point, degrees about the axis", false, {}, 0.0},
                          {"offset", ParamType::Number, "slide along the axis from the middle of the fitted length, mm", false, {}, 0.0},
                          nameParam("clamp")})},
            [](FitDoc& doc, const Params& p) {
                const Ref& cyl = refParam(doc, p, "cylinder", RefKind::Cylinder);
                ClampSpec s;
                s.axis = cyl.dir;
                s.center = cyl.point + cyl.dir * numberParam(p, "offset", 0);
                s.radius = cyl.radius;
                s.gap = numberParam(p, "gap", 0.3);
                s.thickness = numberParam(p, "thickness", 4);
                s.width = numberParam(p, "width", 15);
                if (numberParam(p, "test_slice", 0) > 0) s.width = numberParam(p, "test_slice", 0);
                s.pieces = intParam(p, "pieces", 1) == 2 ? 2 : 1;
                const std::string bolt = stringParam(p, "bolt", "M4");
                s.boltDiameter = bolt == "none" ? 0 : screwClearance(bolt);
                if (bolt != "none" && s.boltDiameter <= 0) return Result::failure("bolt must be M2, M2.5, M3, M4, M5, M6, M8, M10 or none");
                s.slot = numberParam(p, "slot", 2);
                s.angle = numberParam(p, "angle", 0);
                auto meshes = clampParts(s);
                const std::string name = newName(doc, p, "clamp");
                std::string names;
                for (std::size_t i = 0; i < meshes.size(); ++i) {
                    const std::string n = meshes.size() == 1 ? name : name + (i == 0 ? "a" : "b");
                    putPart(doc, n, std::move(meshes[i]), "clamp", {}, s.gap);
                    names += (names.empty() ? "" : ", ") + n;
                }
                const std::string first = meshes.size() == 1 ? name : name + "a";
                return partResult(first, doc, p,
                                  "inner diameter " + num(2 * (s.radius + s.gap)) + " mm for a " + num(2 * s.radius) + " mm cylinder; parts: " + names);
            });

    addEdit(session,
            {"fit.insert", "Insert Sleeve",
             "A sleeve that sits inside a fitted cylinder (cup holder insert, gauge cup, a liner for a round hole). The opening is at the "
             "upper end of the fitted length; flip=true turns it round.",
             withPreview({docParam(),
                          {"cylinder", ParamType::String, "cylinder reference (the inside of the hole)", true},
                          {"gap", ParamType::Number, "clearance to the hole wall, mm", false, {}, 0.5},
                          {"wall", ParamType::Number, "sleeve wall, mm", false, {}, 2.5},
                          {"depth", ParamType::Number, "how deep, mm (0 = the fitted length)", false, {}, 0.0},
                          {"floor", ParamType::Number, "bottom thickness, mm (0 = open tube)", false, {}, 2.0},
                          {"flip", ParamType::Bool, "opening at the other end", false, {}, false},
                          nameParam("insert")})},
            [](FitDoc& doc, const Params& p) {
                const Ref& cyl = refParam(doc, p, "cylinder", RefKind::Cylinder);
                const double gap = numberParam(p, "gap", 0.5);
                const double outer = cyl.radius - gap;
                double depth = numberParam(p, "depth", 0) > 0 ? numberParam(p, "depth", 0) : std::max(cyl.length, 10.0);
                const double test = numberParam(p, "test_slice", 0);
                if (test > 0) depth = test;
                Vec3 a = cyl.dir;
                if (boolParam(p, "flip", false)) a = -a;
                Mesh cup = cupSolid(outer, numberParam(p, "wall", 2.5), depth, test > 0 ? 0.0 : numberParam(p, "floor", 2), 96);
                const Vec3 base = cyl.point + a * (cyl.length / 2) - a * depth;
                mesh::transform(cup, frameFor(base, a).matrix());
                const std::string name = newName(doc, p, "insert");
                putPart(doc, name, std::move(cup), "insert", {}, gap);
                return partResult(name, doc, p, "outer diameter " + num(2 * outer) + " mm in a " + num(2 * cyl.radius) + " mm hole, " + num(depth) + " mm deep");
            });

    addEdit(session,
            {"fit.strip", "Trim Strip",
             "A strip that follows a fitted curve (door edge, dash lip, panel gap): the curve offset by the gap away from the scan, then "
             "thickness thick and width wide across the cutting plane.",
             withPreview({docParam(),
                          {"curve", ParamType::String, "curve reference from fit.curve", true},
                          {"gap", ParamType::Number, "clearance to the scan, mm", false, {}, 0.4},
                          {"thickness", ParamType::Number, "mm", false, {}, 2.0},
                          {"width", ParamType::Number, "mm", false, {}, 10.0},
                          {"flip", ParamType::Bool, "put it on the other side of the curve", false, {}, false},
                          nameParam("strip")})},
            [](FitDoc& doc, const Params& p) {
                const Ref& c = refParam(doc, p, "curve", RefKind::Curve);
                Vec3 side;
                const std::size_t step = std::max<std::size_t>(1, c.curve.size() / 10);
                for (std::size_t i = 0; i < c.curve.size(); i += step) side += snap(doc, c.curve[i]).normal;
                if (boolParam(p, "flip", false)) side = -side;
                const double gap = numberParam(p, "gap", 0.4);
                const double width = numberParam(p, "test_slice", 0) > 0 ? numberParam(p, "test_slice", 0) : numberParam(p, "width", 10);
                Mesh m = stripPart(c.curve, c.dir, side, gap, numberParam(p, "thickness", 2), width);
                const std::string name = newName(doc, p, "strip");
                putPart(doc, name, std::move(m), "strip", normalize(side), gap);
                return partResult(name, doc, p, num(c.length) + " mm along the curve");
            });

    addEdit(session,
            {"fit.hole", "Screw Hole",
             "Drill a screw hole into a part at a point (snapped onto the part), straight in from its surface. screw=M4 picks the clearance "
             "size; countersink=true adds a 90 degree seat for a flat head.",
             withPreview({docParam(),
                          {"part", ParamType::String, "part name", true},
                          {"at", ParamType::NumberList, "where x,y,z (default: the middle of the part's top)", false},
                          {"screw", ParamType::String, "M2, M2.5, M3, M4, M5, M6, M8, M10", false, {}, std::string("M4")},
                          {"diameter", ParamType::Number, "instead of screw: hole diameter, mm", false},
                          {"countersink", ParamType::Bool, "flat-head seat", false, {}, false},
                          {"depth", ParamType::Number, "blind hole depth, mm (0 = all the way through)", false, {}, 0.0},
                          {"direction", ParamType::NumberList, "drill along the reverse of this x,y,z instead of the surface direction", false}})},
            [](FitDoc& doc, const Params& p) {
                Part& part = partParam(doc, p, "part");
                const std::string screw = stringParam(p, "screw", "M4");
                const double d = p.count("diameter") ? numberParam(p, "diameter") : screwClearance(screw);
                if (d <= 0) return Result::failure("unknown screw size " + screw + " (M2, M2.5, M3, M4, M5, M6, M8, M10) - or give diameter=");
                const double head = boolParam(p, "countersink", false) ? (p.count("diameter") ? d * 1.9 : countersinkDiameter(screw)) : 0;
                const auto [at, n] = spotOnPart(part, p);
                bool ok = false;
                Mesh m = drillHole(*part.mesh, at, n, d, numberParam(p, "depth", 0), head, &ok);
                if (!ok) return Result::failure("the hole couldn't be cut cleanly there; move it away from edges");
                setPartMesh(part, std::move(m));
                return partResult(stringParam(p, "part"), doc, p, num(d) + " mm hole at " + vecText(at) + (head > 0 ? ", countersunk " + num(head) + " mm" : ""));
            });

    addEdit(session,
            {"fit.mount", "Add Mount",
             "Add a standard mount to a part, standing on its surface: ball (17 mm phone holder ball), camera (1/4\"-20 nut trap), gopro "
             "(3-prong), magnet (pocket), ziptie (pair of slots), amps (4-hole 30 x 38 mm pattern).",
             withPreview({docParam(),
                          {"part", ParamType::String, "part name", true},
                          {"type", ParamType::String, "mount", true, {"ball", "camera", "gopro", "magnet", "ziptie", "amps"}},
                          {"at", ParamType::NumberList, "where x,y,z (default: the middle of the part's top)", false},
                          {"angle", ParamType::Number, "turn it about the surface direction, degrees", false, {}, 0.0},
                          {"diameter", ParamType::Number, "ball: ball diameter; magnet: magnet diameter, mm", false},
                          {"depth", ParamType::Number, "magnet: pocket depth, mm", false, {}, 3.0},
                          {"direction", ParamType::NumberList, "stand it along this x,y,z instead of the surface direction", false}})},
            [](FitDoc& doc, const Params& p) {
                Part& part = partParam(doc, p, "part");
                const std::string type = stringParam(p, "type");
                const auto [at, n] = spotOnPart(part, p);
                const double angle = numberParam(p, "angle", 0);
                bool ok = true;
                Mesh m = *part.mesh;
                std::string what;
                if (type == "ball") {
                    const double d = numberParam(p, "diameter", 17);
                    m = unite(m, placed(ballMount(d, d * 0.53, 8), at, n, angle), &ok);
                    what = num(d) + " mm ball";
                } else if (type == "camera") {
                    m = unite(m, placed(cameraMount(), at, n, angle), &ok);
                    what = "1/4\"-20 nut trap boss";
                } else if (type == "gopro") {
                    m = unite(m, placed(goproMount(), at, n, angle), &ok);
                    what = "GoPro 3-prong mount";
                } else if (type == "magnet") {
                    const double d = numberParam(p, "diameter", 10) + 0.3;
                    m = drillHole(m, at, n, d, numberParam(p, "depth", 3), 0, &ok);
                    what = num(d) + " mm magnet pocket";
                } else if (type == "ziptie") {
                    const Frame fr = frameFor(at, n, angle);
                    for (double s : {-6.0, 6.0}) {
                        Mesh slot = mesh::box({5, 2.6, 200}, true);
                        mesh::transform(slot, fr.matrix() * Mat4::translate({s, 0, 0}));
                        bool k = false;
                        m = subtract(m, slot, &k);
                        ok = ok && k;
                    }
                    what = "two 5 x 2.6 mm zip-tie slots 12 mm apart";
                } else if (type == "amps") {
                    const Frame fr = frameFor(at, n, angle);
                    for (double u : {-15.0, 15.0})
                        for (double v : {-19.0, 19.0}) {
                            bool k = false;
                            m = drillHole(m, fr.toWorld(u, v), n, 4.5, 0, 0, &k);
                            ok = ok && k;
                        }
                    what = "AMPS 30 x 38 mm hole pattern";
                }
                if (!ok) {
                    if (type == "ball" || type == "camera" || type == "gopro") {
                        what += " (kept as a separate body overlapping the part; slicers merge them)";
                    } else {
                        return Result::failure("the " + type + " couldn't be cut cleanly there; move it");
                    }
                }
                setPartMesh(part, std::move(m));
                return partResult(stringParam(p, "part"), doc, p, "added a " + what + " at " + vecText(at));
            });

    addEdit(session,
            {"fit.combine", "Combine Parts", "Join parts into one printable body (the originals are removed unless keep=true).",
             withPreview({docParam(),
                          {"parts", ParamType::String, "part names separated by commas", true},
                          {"keep", ParamType::Bool, "keep the originals too", false, {}, false},
                          nameParam("combined")})},
            [](FitDoc& doc, const Params& p) {
                std::vector<std::string> names;
                std::stringstream ss(stringParam(p, "parts"));
                for (std::string s; std::getline(ss, s, ',');) {
                    s.erase(0, s.find_first_not_of(' '));
                    s.erase(s.find_last_not_of(' ') + 1);
                    if (!s.empty()) names.push_back(s);
                }
                if (names.size() < 2) return Result::failure("give at least two parts, like parts=plate1,clamp1");
                Mesh m = *partOf(doc, names[0]).mesh;
                double gap = partOf(doc, names[0]).gap;
                Vec3 n = partOf(doc, names[0]).normal;
                bool allOk = true;
                for (std::size_t i = 1; i < names.size(); ++i) {
                    bool ok = false;
                    m = unite(m, *partOf(doc, names[i]).mesh, &ok);
                    allOk = allOk && ok;
                }
                if (!boolParam(p, "keep", false))
                    for (const auto& nm : names) doc.state.parts.erase(nm);
                const std::string name = newName(doc, p, "combined");
                putPart(doc, name, std::move(m), "combined", n, gap);
                return partResult(name, doc, p, allOk ? "joined " + joinList(names) : "joined " + joinList(names) + " (some overlap as separate bodies; slicers merge them)");
            });

    addEdit(session,
            {"fit.rebuild", "Rebuild Part",
             "Change how a part was built (its gap, thickness, size...) and rebuild it and everything done after it, holes and mounts "
             "included. Give the part and only the settings to change.",
             withPreview({docParam(),
                          {"part", ParamType::String, "part name", true},
                          {"gap", ParamType::Number, "new clearance, mm", false},
                          {"thickness", ParamType::Number, "new thickness, mm", false},
                          {"width", ParamType::Number, "new width, mm", false},
                          {"height", ParamType::Number, "new height, mm", false},
                          {"diameter", ParamType::Number, "new diameter, mm", false},
                          {"corner", ParamType::Number, "new corner radius, mm", false},
                          {"angle", ParamType::Number, "new angle, degrees", false},
                          {"tilt", ParamType::NumberList, "new tilt u,v, degrees", false},
                          {"smooth", ParamType::Int, "new smoothing passes", false},
                          {"wall", ParamType::Number, "new wall, mm", false},
                          {"depth", ParamType::Number, "new depth, mm", false},
                          {"floor", ParamType::Number, "new floor, mm", false},
                          {"at", ParamType::NumberList, "new place x,y,z", false}})},
            [](FitDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "part");
                const Part& part = partOf(doc, name);
                auto log = doc.state.log;
                if (part.step >= log.size()) return Result::failure(name + " can't be rebuilt (it wasn't built by a logged step)");
                Params& step = log[part.step].second;
                std::string changed;
                for (const auto& [k, v] : p) {
                    if (k == "doc" || k == "part" || k == "preview" || k == "view" || k == "yaw" || k == "pitch") continue;
                    step[k] = v;
                    changed += (changed.empty() ? "" : ", ") + k + "=" + toText(v);
                }
                if (changed.empty()) return Result::failure("give at least one setting to change, like gap=0.6");
                doc.state = replay(doc.state, log);
                if (!doc.state.parts.count(name)) return done("rebuilt with " + changed, doc, p);
                return partResult(name, doc, p, "rebuilt with " + changed + "\n" + fitSummary(doc, *doc.state.parts.at(name).mesh, doc.state.parts.at(name).gap));
            });
}

}  // namespace wetwaresuite::fit::detail
