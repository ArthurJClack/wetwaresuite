// One-command car parts: phone mount, gauge pod, switch panel, mirror
// (dashcam) mount, speaker adapter ring and cup holder insert. Each is built
// from the same pieces as the fit.plate/block/clamp/insert commands.
#include <algorithm>
#include <cmath>
#include <map>

#include "parts.hpp"
#include "wetwaresuite/mesh/document.hpp"

namespace wetwaresuite::fit::detail {

using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

std::vector<ParamSpec> concat(std::initializer_list<std::vector<ParamSpec>> lists) {
    std::vector<ParamSpec> out;
    for (const auto& l : lists) out.insert(out.end(), l.begin(), l.end());
    return out;
}

// The top face of a part: middle point and outward direction.
std::pair<Vec3, Vec3> topOf(const Part& part) {
    const Bvh pb(part.mesh);
    const Bounds b = mesh::bounds(*part.mesh);
    const Vec3 n = length(part.normal) > 0 ? part.normal : Vec3{0, 0, 1};
    auto hit = pb.ray(b.center() + n * b.diagonal(), -n, 2 * b.diagonal());
    const Vec3 at = hit ? hit->point : b.center();
    return {at, normalize(part.mesh->faceNormal(pb.nearest(at).tri))};
}

void setMesh(Part& part, Mesh m) {
    m.normals.clear();
    part.mesh = std::make_shared<const Mesh>(std::move(m));
}

Mesh placedAt(Mesh m, const Vec3& at, const Vec3& n, double angle) {
    mesh::transform(m, frameFor(at, n, angle).matrix());
    return m;
}

std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }
std::string rest(const std::string& s) { return s.find('\n') == std::string::npos ? "" : s.substr(s.find('\n') + 1); }

// Typical cutout diameters for round car speakers, mm (check the speaker's own sheet).
double speakerCutout(const std::string& size) {
    static const std::map<std::string, double> d = {{"3.5", 77}, {"4", 93}, {"5.25", 118}, {"6.5", 142}, {"6.75", 147}, {"8", 182}};
    auto it = d.find(size);
    return it == d.end() ? 0 : it->second;
}

}  // namespace

void registerPresets(Session& session) {
    addEdit(session,
            {"fit.phone_mount", "Phone Mount",
             "A phone holder base for the dash: a pad that hugs the scan (stick it on with VHB tape, or screws=true for two countersunk M4 "
             "holes) with a 17 mm ball for standard phone holder clamps. tilt=u,v makes the base a wedge to angle the phone.",
             withPreview(concat({{docParam()}, placementParams(),
                                 {{"width", ParamType::Number, "pad width, mm", false, {}, 60.0},
                                  {"height", ParamType::Number, "pad height, mm", false, {}, 45.0},
                                  {"corner", ParamType::Number, "corner radius, mm", false, {}, 8.0},
                                  {"gap", ParamType::Number, "clearance to the dash, mm (tape is about 1 mm)", false, {}, 0.4},
                                  {"thickness", ParamType::Number, "pad thickness, mm", false, {}, 4.0},
                                  {"tilt", ParamType::NumberList, "wedge the top: degrees about u and v", false},
                                  {"ball", ParamType::Number, "ball diameter, mm (17 fits most holders)", false, {}, 17.0},
                                  {"screws", ParamType::Bool, "add two countersunk M4 holes", false, {}, false},
                                  {"smooth", ParamType::Int, "blur passes over the scan", false, {}, std::int64_t{2}},
                                  {"test_slice", ParamType::Number, "only a thin contact skin this thick, to test the fit, mm", false, {}, 0.0},
                                  nameParam("phone")}}))},
            [](FitDoc& doc, const Params& p) {
                Params q = p;
                q["shape"] = std::string("rounded");
                if (!q.count("width")) q["width"] = 60.0;
                if (!q.count("height")) q["height"] = 45.0;
                if (!q.count("corner")) q["corner"] = 8.0;
                if (!q.count("thickness")) q["thickness"] = 4.0;
                const bool tilted = !listParam(p, "tilt").empty();
                const std::string info = buildConform(doc, q, tilted, "phone");
                const std::string name = firstLine(info);
                Part& part = doc.state.parts.at(name);
                part.kind = "phone mount";
                if (numberParam(p, "test_slice", 0) > 0) return partResult(name, doc, p, "test slice: just the contact skin\n" + rest(info));
                const auto [at, n] = topOf(part);
                bool ok = true;
                Mesh m = unite(*part.mesh, placedAt(ballMount(numberParam(p, "ball", 17), numberParam(p, "ball", 17) * 0.53, 8), at, n, 0), &ok);
                std::string extra = rest(info);
                if (!ok) extra += "\nthe ball is a separate body overlapping the pad (slicers merge them)";
                if (boolParam(p, "screws", false)) {
                    const Frame fr = frameFor(at, n, numberParam(p, "angle", 0));
                    const double off = numberParam(q, "width", 60) / 2 - 9;
                    for (double s : {-off, off}) {
                        bool k = false;
                        Mesh h = drillHole(m, fr.toWorld(s, 0), n, screwClearance("M4"), 0, countersinkDiameter("M4"), &k);
                        if (k) m = std::move(h);
                    }
                    extra += "\ntwo countersunk M4 holes " + num(2 * off) + " mm apart";
                }
                setMesh(part, std::move(m));
                return partResult(name, doc, p, extra);
            });

    addEdit(session,
            {"fit.gauge_pod", "Gauge Pod",
             "A pod for a round gauge (52 or 60 mm, or any diameter) on the dash or pillar: a base that hugs the scan with a flat top tilted "
             "toward you (tilt=u,v), a cup for the gauge, and a hole through the bottom for the wires.",
             withPreview(concat({{docParam()}, placementParams(),
                                 {{"gauge", ParamType::Number, "gauge diameter, mm (52 and 60 are standard)", false, {}, 52.0},
                                  {"tilt", ParamType::NumberList, "aim the gauge: degrees about u and v", false},
                                  {"depth", ParamType::Number, "cup depth, mm", false, {}, 30.0},
                                  {"wall", ParamType::Number, "cup wall, mm", false, {}, 2.5},
                                  {"gap", ParamType::Number, "clearance to the dash, mm", false, {}, 0.4},
                                  {"thickness", ParamType::Number, "base thickness at the thinnest point, mm", false, {}, 3.0},
                                  {"smooth", ParamType::Int, "blur passes over the scan", false, {}, std::int64_t{2}},
                                  {"test_slice", ParamType::Number, "only a thin contact skin this thick, to test the fit, mm", false, {}, 0.0},
                                  nameParam("gauge")}}))},
            [](FitDoc& doc, const Params& p) {
                const double gauge = numberParam(p, "gauge", 52), wall = numberParam(p, "wall", 2.5);
                if (gauge <= 10) return Result::failure("gauge must be over 10 mm");
                const double outerR = gauge / 2 + 0.3 + wall;
                Params q = p;
                q["shape"] = std::string("circle");
                q["diameter"] = 2 * outerR + 10;
                if (!q.count("tilt")) q["tilt"] = NumberList{15, 0};
                const std::string info = buildConform(doc, q, true, "gauge");
                const std::string name = firstLine(info);
                Part& part = doc.state.parts.at(name);
                part.kind = "gauge pod";
                if (numberParam(p, "test_slice", 0) > 0) return partResult(name, doc, p, "test slice: just the contact skin\n" + rest(info));
                const auto [at, n] = topOf(part);
                Mesh cup = cupSolid(outerR, wall, numberParam(p, "depth", 30) + 1, 3, 96);
                mesh::transform(cup, Mat4::translate({0, 0, -1}));
                bool ok = true;
                Mesh m = unite(*part.mesh, placedAt(cup, at, n, 0), &ok);
                bool holeOk = false;
                Mesh h = drillHole(m, at + n * 2, n, gauge * 0.4, 0, 0, &holeOk);
                if (holeOk) m = std::move(h);
                setMesh(part, std::move(m));
                return partResult(name, doc, p,
                                  "cup " + num(gauge + 0.6) + " mm inside for a " + num(gauge) + " mm gauge, wire hole " + num(gauge * 0.4) + " mm" +
                                      (ok ? "" : " (cup kept as a separate body; slicers merge it)") + "\n" + rest(info));
            });

    addEdit(session,
            {"fit.switch_panel", "Switch Panel",
             "A panel that hugs the scan with cutouts for standard rocker switches (21 x 37 mm, Contura/Carling style) in a row.",
             withPreview(concat({{docParam()}, placementParams(),
                                 {{"switches", ParamType::Int, "how many switches", false, {}, std::int64_t{3}},
                                  {"cutout", ParamType::NumberList, "switch cutout w,h, mm", false},
                                  {"pitch", ParamType::Number, "centre to centre spacing, mm", false, {}, 27.0},
                                  {"margin", ParamType::Number, "border round the cutouts, mm", false, {}, 9.0},
                                  {"gap", ParamType::Number, "clearance to the scan, mm", false, {}, 0.4},
                                  {"thickness", ParamType::Number, "panel thickness, mm (rocker clips like 2-3)", false, {}, 2.5},
                                  {"smooth", ParamType::Int, "blur passes over the scan", false, {}, std::int64_t{2}},
                                  {"test_slice", ParamType::Number, "only a thin contact skin this thick, to test the fit, mm", false, {}, 0.0},
                                  nameParam("switches")}}))},
            [](FitDoc& doc, const Params& p) {
                const int count = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "switches", 3), 1, 20));
                NumberList cut = listParam(p, "cutout");
                if (cut.size() != 2) cut = {21.1, 36.8};
                const double pitch = std::max(numberParam(p, "pitch", 27), cut[0] + 2), margin = numberParam(p, "margin", 9);
                const double w = (count - 1) * pitch + cut[0] + 2 * margin, h = cut[1] + 2 * margin;
                Footprint f = rectFootprint(w, h, 4);
                for (int i = 0; i < count; ++i) {
                    const double cx = -((count - 1) * pitch) / 2 + i * pitch;
                    Footprint c = rectFootprint(cut[0], cut[1], 0.8, 3);
                    for (auto& q : c.outer) q.x += cx;
                    f.holes.push_back(c.outer);
                }
                Params q = p;
                if (!q.count("thickness")) q["thickness"] = 2.5;
                const Frame fr = placeOn(doc, q, std::max(w, h));
                const std::string info = buildConform(doc, q, false, "switches", &f, &fr);
                const std::string name = firstLine(info);
                doc.state.parts.at(name).kind = "switch panel";
                return partResult(name, doc, p, std::to_string(count) + " cutouts " + num(cut[0]) + " x " + num(cut[1]) + " mm, panel " + num(w) + " x " + num(h) + " mm\n" + rest(info));
            });

    addEdit(session,
            {"fit.mirror_mount", "Mirror Mount",
             "A dashcam or camera mount that clamps round a fitted cylinder (the rear-view mirror stem, a grab handle, a roll bar): two halves "
             "bolted together, with a GoPro or 1/4\" camera mount on one half.",
             withPreview({docParam(),
                          {"cylinder", ParamType::String, "cylinder reference from fit.cylinder", true},
                          {"mount", ParamType::String, "what the camera attaches to", false, {"gopro", "camera", "ball"}, std::string("gopro")},
                          {"gap", ParamType::Number, "clearance round the stem, mm", false, {}, 0.3},
                          {"thickness", ParamType::Number, "clamp wall, mm", false, {}, 4.0},
                          {"width", ParamType::Number, "along the stem, mm", false, {}, 18.0},
                          {"bolt", ParamType::String, "screw size", false, {}, std::string("M3")},
                          {"angle", ParamType::Number, "turn the clamp about the stem, degrees", false, {}, 0.0},
                          {"offset", ParamType::Number, "slide along the stem, mm", false, {}, 0.0},
                          {"test_slice", ParamType::Number, "only a thin ring this wide, to test the fit, mm", false, {}, 0.0},
                          nameParam("mirror")})},
            [](FitDoc& doc, const Params& p) {
                const Ref& cyl = refParam(doc, p, "cylinder", RefKind::Cylinder);
                ClampSpec s;
                s.axis = cyl.dir;
                s.center = cyl.point + cyl.dir * numberParam(p, "offset", 0);
                s.radius = cyl.radius;
                s.gap = numberParam(p, "gap", 0.3);
                s.thickness = numberParam(p, "thickness", 4);
                s.width = numberParam(p, "width", 18);
                const double test = numberParam(p, "test_slice", 0);
                if (test > 0) s.width = test;
                s.pieces = 2;
                s.boltDiameter = screwClearance(stringParam(p, "bolt", "M3"));
                if (s.boltDiameter <= 0) return Result::failure("bolt must be M2 .. M10");
                s.angle = numberParam(p, "angle", 0);
                auto meshes = clampParts(s);
                const Frame fr = frameFor(s.center, s.axis, s.angle);
                // the half on the +v side carries the mount
                std::size_t carrier = 0;
                double best = -1e300;
                for (std::size_t i = 0; i < meshes.size(); ++i) {
                    const double side = dot(mesh::bounds(meshes[i]).center() - s.center, fr.v);
                    if (side > best) {
                        best = side;
                        carrier = i;
                    }
                }
                std::string extra = "clamp for a " + num(2 * cyl.radius) + " mm stem";
                if (test <= 0) {
                    const double ro = s.radius + s.gap + s.thickness;
                    const Vec3 at = s.center + fr.v * (ro - 0.3);
                    const std::string mount = stringParam(p, "mount", "gopro");
                    Mesh mm = mount == "camera" ? cameraMount() : mount == "ball" ? ballMount() : goproMount();
                    // stand the mount on the outside of the ring, its prongs along the stem
                    bool ok = true;
                    meshes[carrier] = unite(meshes[carrier], placedAt(mm, at, fr.v, 90), &ok);
                    extra += " with a " + mount + " mount" + (ok ? "" : " (as a separate overlapping body)");
                }
                const std::string name = newName(doc, p, "mirror");
                std::string first;
                for (std::size_t i = 0; i < meshes.size(); ++i) {
                    const std::string n = meshes.size() == 1 ? name : name + (i == carrier ? "a" : "b");
                    if (first.empty() || i == carrier) first = n;
                    putPart(doc, n, std::move(meshes[i]), "mirror mount", {}, s.gap);
                }
                return partResult(first, doc, p, extra);
            });

    addEdit(session,
            {"fit.speaker_ring", "Speaker Ring",
             "An adapter ring from a car's round speaker opening (a circle fitted with fit.circle) to a standard speaker: flat on top for the "
             "speaker, matching the door or dash underneath, with four M4 holes to fix it.",
             withPreview({docParam(),
                          {"circle", ParamType::String, "circle reference of the opening", true},
                          {"speaker", ParamType::String, "speaker size in inches: 3.5, 4, 5.25, 6.5, 6.75, 8", false, {}, std::string("6.5")},
                          {"cutout", ParamType::Number, "instead of speaker: the speaker's cutout diameter, mm", false},
                          {"flange", ParamType::Number, "ring width outside the opening, mm", false, {}, 12.0},
                          {"gap", ParamType::Number, "clearance to the panel, mm", false, {}, 0.4},
                          {"thickness", ParamType::Number, "thinnest point, mm", false, {}, 8.0},
                          {"flip", ParamType::Bool, "build on the other side", false, {}, false},
                          {"test_slice", ParamType::Number, "only a thin contact skin this thick, to test the fit, mm", false, {}, 0.0},
                          nameParam("speaker")})},
            [](FitDoc& doc, const Params& p) {
                const Ref& c = refParam(doc, p, "circle", RefKind::Circle);
                const double cut = p.count("cutout") ? numberParam(p, "cutout") : speakerCutout(stringParam(p, "speaker", "6.5"));
                if (cut <= 0) return Result::failure("unknown speaker size; give cutout= in mm");
                const double outer = std::max(2 * c.radius, cut) + 2 * numberParam(p, "flange", 12);
                Footprint f = circleFootprint(outer);
                f.holes.push_back(circleFootprint(cut).outer);
                Vec3 n = c.dir;
                if (boolParam(p, "flip", false)) n = -n;
                const Frame fr = frameFor(c.point, n, 0);
                Params q = p;
                if (!q.count("thickness")) q["thickness"] = 8.0;
                const std::string info = buildConform(doc, q, true, "speaker", &f, &fr);
                const std::string name = firstLine(info);
                Part& part = doc.state.parts.at(name);
                part.kind = "speaker ring";
                if (numberParam(p, "test_slice", 0) <= 0) {
                    const double rr = (std::max(2 * c.radius, cut) / 2 + outer / 2) / 2;
                    Mesh m = *part.mesh;
                    for (int k = 0; k < 4; ++k) {
                        const double a = mesh::kPi / 4 + k * mesh::kPi / 2;
                        bool ok = false;
                        // drill from above the top, straight down the ring's axis
                        Mesh h = drillHole(m, fr.toWorld(rr * std::cos(a), rr * std::sin(a), 200), n, screwClearance("M4"), 0, 0, &ok);
                        if (ok) m = std::move(h);
                    }
                    setMesh(part, std::move(m));
                }
                return partResult(name, doc, p,
                                  "ring " + num(outer) + " mm across with a " + num(cut) + " mm cutout over a " + num(2 * c.radius) + " mm opening\n" + rest(info));
            });

    addEdit(session,
            {"fit.cupholder", "Cup Holder Insert",
             "An insert for a round cup holder (a cylinder fitted inside it): a sleeve with a floor, sized from the scan.",
             withPreview({docParam(),
                          {"cylinder", ParamType::String, "cylinder reference of the holder's inside", true},
                          {"gap", ParamType::Number, "clearance to the holder wall, mm", false, {}, 0.6},
                          {"wall", ParamType::Number, "mm", false, {}, 2.0},
                          {"depth", ParamType::Number, "mm (0 = the fitted length)", false, {}, 0.0},
                          {"floor", ParamType::Number, "mm", false, {}, 2.0},
                          {"flip", ParamType::Bool, "opening at the other end", false, {}, false},
                          {"test_slice", ParamType::Number, "only a thin ring this tall, to test the fit, mm", false, {}, 0.0},
                          nameParam("cupholder")})},
            [](FitDoc& doc, const Params& p) {
                Params q = p;
                if (!q.count("gap")) q["gap"] = 0.6;
                if (!q.count("wall")) q["wall"] = 2.0;
                if (!q.count("name")) q["name"] = newName(doc, p, "cupholder");
                Result r = handlers().at("fit.insert")(doc, q);
                if (r.ok) doc.state.parts.at(stringParam(q, "name")).kind = "cup holder insert";
                return r;
            });
}

}  // namespace wetwaresuite::fit::detail
