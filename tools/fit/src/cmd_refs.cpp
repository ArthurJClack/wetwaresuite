// fit.* commands that fit clean reference shapes to patches of the scan.
#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "wetwaresuite/mesh/document.hpp"

namespace wetwaresuite::fit::detail {

using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

void requireScan(const FitDoc& doc) {
    if (!doc.state.scan || doc.state.scan->empty()) throw std::runtime_error("the scan is empty");
}

Patch patchFor(const FitDoc& doc, const Params& p, double defRadius, Vec3* centre = nullptr) {
    requireScan(doc);
    const SurfacePoint sp = snap(doc, requireVec3(p, "at"));
    if (centre) *centre = sp.point;
    const double r = numberParam(p, "radius", defRadius);
    if (r <= 0) throw std::runtime_error("radius must be above 0");
    Patch patch = patchAround(*doc.state.scan, scanVertexNormals(doc), sp.point, r);
    return patch;
}

std::string quality(double rms, std::size_t used, std::size_t total) {
    const double share = total ? 100.0 * static_cast<double>(used) / static_cast<double>(total) : 0;
    std::string s = "fit error " + num(rms) + " mm using " + num(share) + "% of " + std::to_string(total) + " points";
    if (share < 50) s += " (the patch has a lot that isn't this shape; try a smaller radius)";
    return s;
}

Result refResult(const std::string& name, const Ref& r, FitDoc& doc, const Params& p, const std::string& extra) {
    Result res = done(describeRef(name, r) + (extra.empty() ? "" : "\n" + extra), doc, p);
    res.value = name;
    res.with("name", name).with("point", toList(r.point));
    if (length(r.dir) > 0) res.with("direction", toList(r.dir));
    if (r.radius > 0) res.with("radius", r.radius);
    if (r.length > 0) res.with("length", r.length);
    res.with("error", r.rms);
    return res;
}

std::vector<ParamSpec> patchParams(double defRadius, const std::string& what) {
    return withPreview({docParam(),
                        {"at", ParamType::NumberList, "a point on the " + what + " x,y,z (snapped onto the scan)", true},
                        {"radius", ParamType::Number, "use the scan within this distance of the point, mm", false, {}, defRadius},
                        nameParam(what)});
}

}  // namespace

void registerRefs(Session& session) {
    addEdit(session,
            {"fit.plane", "Fit Plane",
             "Fit a flat reference to the scan around a point (robust to bumps, noise and holes). Reports how flat the area really is. The plane "
             "faces away from the scan surface, which is the side parts get built on.",
             patchParams(15, "plane")},
            [](FitDoc& doc, const Params& p) {
                Vec3 c;
                const Patch patch = patchFor(doc, p, 15, &c);
                const PlaneFit f = fitPlane(patch);
                Ref r;
                r.kind = RefKind::Plane;
                r.point = c - f.normal * dot(c - f.point, f.normal);  // the snapped point, moved onto the plane
                r.dir = f.normal;
                r.radius = numberParam(p, "radius", 15);
                r.rms = f.rms;
                const std::string name = newName(doc, p, "plane");
                doc.state.refs[name] = r;
                return refResult(name, r, doc, p, quality(f.rms, f.used, f.total) + ", worst bump " + num(f.maxDeviation) + " mm");
            });

    addEdit(session,
            {"fit.cylinder", "Fit Cylinder",
             "Fit a cylinder (pipe, bar, headrest post, roll cage, mirror stem, cup holder) to the scan around a point. Works from one side too. "
             "Gives the axis, radius and length clamps and inserts are built from.",
             patchParams(20, "cylinder")},
            [](FitDoc& doc, const Params& p) {
                const Patch patch = patchFor(doc, p, 20);
                const CylinderFit f = fitCylinder(patch);
                Ref r;
                r.kind = RefKind::Cylinder;
                r.point = f.center;
                r.dir = f.axis;
                r.radius = f.radius;
                r.length = f.length;
                r.rms = f.rms;
                const std::string name = newName(doc, p, "cylinder");
                doc.state.refs[name] = r;
                return refResult(name, r, doc, p, quality(f.rms, f.used, f.total));
            });

    addEdit(session,
            {"fit.sphere", "Fit Sphere", "Fit a sphere (knob, gauge bezel, gear knob) to the scan around a point.", patchParams(15, "sphere")},
            [](FitDoc& doc, const Params& p) {
                const Patch patch = patchFor(doc, p, 15);
                const SphereFit f = fitSphere(patch);
                Ref r;
                r.kind = RefKind::Sphere;
                r.point = f.center;
                r.radius = f.radius;
                r.rms = f.rms;
                const std::string name = newName(doc, p, "sphere");
                doc.state.refs[name] = r;
                return refResult(name, r, doc, p, quality(f.rms, f.used, f.total));
            });

    addEdit(session,
            {"fit.circle", "Fit Circle",
             "Fit a circle for round openings (speaker holes, gauge holes, vents). source=hole uses the open edge of a hole in the scan nearest "
             "the point; source=section cuts the scan with a plane through the point (normal=) and fits the curve within radius=.",
             withPreview({docParam(),
                          {"at", ParamType::NumberList, "a point on or near the circle x,y,z", true},
                          {"source", ParamType::String, "where the circle comes from", false, {"hole", "section"}, std::string("hole")},
                          {"normal", ParamType::NumberList, "section: the cutting plane's normal (default: the surface direction at the point)", false},
                          {"radius", ParamType::Number, "section: use the curve within this distance of the point, mm", false, {}, 60.0},
                          nameParam("circle")})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const Vec3 at = requireVec3(p, "at");
                const Mesh& m = *doc.state.scan;
                std::vector<Vec3> pts;
                std::optional<Vec3> normal;
                if (stringParam(p, "source", "hole") == "hole") {
                    double best = 1e300;
                    for (const auto& loop : boundaryLoops(m)) {
                        Vec3 c;
                        for (auto v : loop) c += m.positions[v];
                        c = c / static_cast<double>(loop.size());
                        double d = 1e300;
                        for (auto v : loop) d = std::min(d, length(m.positions[v] - at));
                        d = std::min(d, length(c - at));
                        if (d < best) {
                            best = d;
                            pts.clear();
                            for (auto v : loop) pts.push_back(m.positions[v]);
                        }
                    }
                    if (pts.size() < 3) return Result::failure("no hole in the scan near that point; try source=section");
                } else {
                    const SurfacePoint sp = snap(doc, at);
                    const Vec3 n = vec3Param(p, "normal").value_or(sp.normal);
                    normal = normalize(n);
                    const Section sec = section(m, sp.point, *normal);
                    const double rad = numberParam(p, "radius", 60);
                    for (const auto& line : sec.lines)
                        for (const auto& q : line)
                            if (length(q - sp.point) <= rad) pts.push_back(q);
                    if (pts.size() < 3) return Result::failure("the section has too few points near there; use a bigger radius");
                }
                const CircleFit f = fitCircle(pts, normal);
                Ref r;
                r.kind = RefKind::Circle;
                r.point = f.center;
                r.dir = f.normal;
                // face the same way as the scan around it
                const SurfacePoint sp = snap(doc, f.center + f.normal * 0.0);
                if (dot(r.dir, sp.normal) < 0) r.dir = -r.dir;
                r.radius = f.radius;
                r.rms = f.rms;
                const std::string name = newName(doc, p, "circle");
                doc.state.refs[name] = r;
                return refResult(name, r, doc, p, quality(f.rms, f.used, f.total));
            });

    addEdit(session,
            {"fit.curve", "Fit Curve",
             "A smooth curve along the scan, from a cross-section through the point (a door edge, a dash lip, a panel gap) for trim strips. "
             "normal= is the cutting plane's normal (or axis=). length= keeps that much of the curve centred on the point.",
             withPreview({docParam(),
                          {"at", ParamType::NumberList, "a point on the curve x,y,z", true},
                          {"normal", ParamType::NumberList, "the cutting plane's normal x,y,z", false},
                          {"axis", ParamType::String, "instead of normal: cut across this axis", false, {"x", "y", "z"}},
                          {"length", ParamType::Number, "keep this many mm of curve around the point (0 = the whole line)", false, {}, 0.0},
                          {"smooth", ParamType::Int, "smoothing passes", false, {}, std::int64_t{4}},
                          {"spacing", ParamType::Number, "point spacing of the result, mm", false, {}, 1.0},
                          nameParam("curve")})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const SurfacePoint sp = snap(doc, requireVec3(p, "at"));
                Vec3 n;
                if (auto nn = vec3Param(p, "normal")) n = normalize(*nn);
                else if (p.count("axis")) {
                    const std::string ax = stringParam(p, "axis");
                    n[ax == "y" ? 1 : ax == "z" ? 2 : 0] = 1;
                } else {
                    return Result::failure("give normal=x,y,z or axis= for the cutting plane");
                }
                const Section sec = section(*doc.state.scan, sp.point, n);
                std::size_t pick = 0, idx = 0;
                double best = 1e300;
                for (std::size_t i = 0; i < sec.lines.size(); ++i)
                    for (std::size_t j = 0; j < sec.lines[i].size(); ++j)
                        if (length(sec.lines[i][j] - sp.point) < best) {
                            best = length(sec.lines[i][j] - sp.point);
                            pick = i;
                            idx = j;
                        }
                if (sec.lines.empty()) return Result::failure("the plane doesn't cut the scan there");
                std::vector<Vec3> line = sec.lines[pick];
                const double want = numberParam(p, "length", 0);
                if (want > 0) {
                    // walk out both ways from the nearest point
                    std::size_t a = idx, b = idx;
                    double la = 0, lb = 0;
                    while (a > 0 && la < want / 2) {
                        la += length(line[a] - line[a - 1]);
                        --a;
                    }
                    while (b + 1 < line.size() && lb < want / 2) {
                        lb += length(line[b + 1] - line[b]);
                        ++b;
                    }
                    line = std::vector<Vec3>(line.begin() + static_cast<std::ptrdiff_t>(a), line.begin() + static_cast<std::ptrdiff_t>(b) + 1);
                }
                if (line.size() < 2) return Result::failure("the curve there is too short");
                Ref r;
                r.kind = RefKind::Curve;
                r.dir = n;
                r.curve = smoothCurve(line, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "smooth", 4), 0, 100)),
                                      std::max(numberParam(p, "spacing", 1), 0.1));
                r.point = r.curve[r.curve.size() / 2];
                r.length = polylineLength(r.curve, false);
                const std::string name = newName(doc, p, "curve");
                doc.state.refs[name] = r;
                return refResult(name, r, doc, p, "");
            });

    addEdit(session,
            {"fit.point", "Mark Point", "Name a point on the scan (snapped onto it) to come back to: a bolt hole, the middle of a vent.",
             withPreview({docParam(), {"at", ParamType::NumberList, "x,y,z", true}, nameParam("point")})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const SurfacePoint sp = snap(doc, requireVec3(p, "at"));
                Ref r;
                r.kind = RefKind::Point;
                r.point = sp.point;
                r.dir = sp.normal;
                const std::string name = newName(doc, p, "point");
                doc.state.refs[name] = r;
                return refResult(name, r, doc, p, "");
            });

    addRead(session,
            {"fit.refs", "List References", "Every reference shape and part, with its numbers.", {docParam()}, false, true},
            [](FitDoc& doc, const Params&) {
                std::string s;
                for (const auto& [n, r] : doc.state.refs) s += (s.empty() ? "" : "\n") + describeRef(n, r);
                for (const auto& [n, pt] : doc.state.parts) s += (s.empty() ? "" : "\n") + describePart(n, pt);
                if (s.empty()) s = "no references or parts yet (fit.plane, fit.cylinder, ... make references)";
                Result r = Result::success(s);
                return r.with("references", static_cast<std::int64_t>(doc.state.refs.size())).with("parts", static_cast<std::int64_t>(doc.state.parts.size()));
            });

    addEdit(session,
            {"fit.delete", "Delete", "Delete a reference or a part by name.", withPreview({docParam(), {"name", ParamType::String, "reference or part", true}})},
            [](FitDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (doc.state.refs.erase(name) == 0 && doc.state.parts.erase(name) == 0) return Result::failure("nothing called '" + name + "'");
                return done("deleted " + name, doc, p);
            });
}

}  // namespace wetwaresuite::fit::detail
