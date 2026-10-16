// fit.* commands that measure the scan: distances, sections, local radius,
// and turning a spot on a picture or a rough point into a point on the surface.
#include <algorithm>
#include <cmath>
#include <fstream>

#include "internal.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::fit::detail {

namespace fs = std::filesystem;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

void requireScan(const FitDoc& doc) {
    if (!doc.state.scan || doc.state.scan->empty()) throw std::runtime_error("the scan is empty");
}

// The section plane: point + normal, or axis + at.
std::pair<Vec3, Vec3> planeOf(const FitDoc& doc, const Params& p) {
    if (p.count("plane")) {
        const Ref& r = refParam(doc, p, "plane", RefKind::Plane);
        return {r.point, r.dir};
    }
    if (auto pt = vec3Param(p, "point")) return {*pt, requireVec3(p, "normal")};
    if (p.count("axis")) {
        const std::string ax = stringParam(p, "axis");
        Vec3 n, pt;
        const int a = ax == "y" ? 1 : ax == "z" ? 2 : 0;
        n[a] = 1;
        pt[a] = numberParam(p, "at", 0);
        return {pt, n};
    }
    throw std::runtime_error("give the cutting plane: point= and normal=, or axis= and at=, or plane=<reference>");
}

std::vector<ParamSpec> planeParams() {
    return {{"point", ParamType::NumberList, "a point on the cutting plane x,y,z", false},
            {"normal", ParamType::NumberList, "the cutting plane's normal x,y,z", false},
            {"axis", ParamType::String, "instead: cut across this axis", false, {"x", "y", "z"}},
            {"at", ParamType::Number, "with axis: where along it, mm", false, {}, 0.0},
            {"plane", ParamType::String, "instead: cut along a plane reference", false}};
}

void drawLine(raster::Image& img, double x0, double y0, double x1, double y1, raster::Color c) {
    const int steps = static_cast<int>(std::ceil(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)))) + 1;
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        const int x = static_cast<int>(std::lround(x0 + (x1 - x0) * t)), y = static_cast<int>(std::lround(y0 + (y1 - y0) * t));
        for (int dy = 0; dy <= 1; ++dy)
            for (int dx = 0; dx <= 1; ++dx)
                if (x + dx >= 0 && y + dy >= 0 && x + dx < img.width() && y + dy < img.height()) img.set(x + dx, y + dy, c);
    }
}

}  // namespace

void registerMeasure(Session& session) {
    addRead(session,
            {"fit.distance", "Measure Distance", "Straight distance between two points (mm). Points are snapped onto the scan first unless snap=false.",
             {docParam(),
              {"from", ParamType::NumberList, "first point x,y,z", true},
              {"to", ParamType::NumberList, "second point x,y,z", true},
              {"snap", ParamType::Bool, "move both points onto the scan surface first", false, {}, true}},
             false, true},
            [](FitDoc& doc, const Params& p) {
                Vec3 a = requireVec3(p, "from"), b = requireVec3(p, "to");
                if (boolParam(p, "snap", true)) {
                    requireScan(doc);
                    a = snap(doc, a).point;
                    b = snap(doc, b).point;
                }
                const Vec3 d = b - a;
                Result r = Result::success("distance " + num(length(d)) + " mm (dx " + num(d.x) + ", dy " + num(d.y) + ", dz " + num(d.z) + ")", length(d));
                return r.with("distance", length(d)).with("from", toList(a)).with("to", toList(b)).with("delta", toList(d));
            });

    {
        auto ps = planeParams();
        ps.insert(ps.begin(), docParam());
        ps.push_back({"near", ParamType::NumberList, "report the line nearest this point (default: the longest)", false});
        ps.push_back({"path", ParamType::String, "also write the section as .svg (1:1 mm, for printing a paper template) or .png", false});
        addRead(session,
                {"fit.section", "Cross Section",
                 "Cut the scan with a plane and measure the curve: its length, and width and height in the plane. Writes an SVG template at 1:1 "
                 "or a PNG with path=. points= in the result is the chosen curve (up to 200 points).",
                 ps, false, false},
                [](FitDoc& doc, const Params& p) {
                    requireScan(doc);
                    const auto [pt, nIn] = planeOf(doc, p);
                    const Vec3 n = normalize(nIn);
                    const Section sec = section(*doc.state.scan, pt, n);
                    if (sec.lines.empty()) return Result::failure("the plane doesn't cut the scan there");
                    std::size_t pick = 0;
                    if (auto near = vec3Param(p, "near")) {
                        double best = 1e300;
                        for (std::size_t i = 0; i < sec.lines.size(); ++i)
                            for (const auto& q : sec.lines[i])
                                if (length(q - *near) < best) {
                                    best = length(q - *near);
                                    pick = i;
                                }
                    } else {
                        double best = -1;
                        for (std::size_t i = 0; i < sec.lines.size(); ++i) {
                            const double l = polylineLength(sec.lines[i], sec.closed[i]);
                            if (l > best) {
                                best = l;
                                pick = i;
                            }
                        }
                    }
                    const Frame fr = frameFor(pt, n);
                    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
                    for (const auto& q : sec.lines[pick]) {
                        const Vec3 l = fr.toLocal(q);
                        lo[0] = std::min(lo[0], l.x); hi[0] = std::max(hi[0], l.x);
                        lo[1] = std::min(lo[1], l.y); hi[1] = std::max(hi[1], l.y);
                    }
                    const double len = polylineLength(sec.lines[pick], sec.closed[pick]);
                    std::string msg = std::to_string(sec.lines.size()) + " line" + (sec.lines.size() == 1 ? "" : "s") + "; line " + std::to_string(pick + 1) +
                                      ": " + num(len) + " mm long, " + (sec.closed[pick] ? "closed" : "open") + ", " + num(hi[0] - lo[0]) + " x " +
                                      num(hi[1] - lo[1]) + " mm in the plane";
                    NumberList pts;
                    const std::size_t step = std::max<std::size_t>(1, sec.lines[pick].size() / 200);
                    for (std::size_t i = 0; i < sec.lines[pick].size(); i += step) {
                        const auto& q = sec.lines[pick][i];
                        pts.insert(pts.end(), {q.x, q.y, q.z});
                    }
                    Result r = Result::success(msg, len);
                    r.with("lines", static_cast<std::int64_t>(sec.lines.size()))
                        .with("length", len)
                        .with("closed", static_cast<bool>(sec.closed[pick]))
                        .with("width", hi[0] - lo[0])
                        .with("height", hi[1] - lo[1])
                        .with("points", pts);
                    if (p.count("path")) {
                        const fs::path out = stringParam(p, "path");
                        std::error_code ec;
                        if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
                        // every line, in plane coordinates
                        double a0 = 1e300, b0 = 1e300, a1 = -1e300, b1 = -1e300;
                        for (const auto& line : sec.lines)
                            for (const auto& q : line) {
                                const Vec3 l = fr.toLocal(q);
                                a0 = std::min(a0, l.x); a1 = std::max(a1, l.x);
                                b0 = std::min(b0, l.y); b1 = std::max(b1, l.y);
                            }
                        const double margin = 5, W = a1 - a0 + 2 * margin, H = b1 - b0 + 2 * margin;
                        if (lowerExt(out) == "svg") {
                            std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(W) + "mm\" height=\"" + num(H) +
                                              "mm\" viewBox=\"0 0 " + num(W) + " " + num(H) + "\">\n";
                            for (std::size_t i = 0; i < sec.lines.size(); ++i) {
                                svg += std::string("<") + (sec.closed[i] ? "polygon" : "polyline") + " fill=\"none\" stroke=\"black\" stroke-width=\"0.3\" points=\"";
                                for (const auto& q : sec.lines[i]) {
                                    const Vec3 l = fr.toLocal(q);
                                    svg += num(l.x - a0 + margin) + "," + num(b1 - l.y + margin) + " ";
                                }
                                svg += "\"/>\n";
                            }
                            svg += "</svg>\n";
                            std::ofstream f(out, std::ios::binary);
                            f << svg;
                            if (!f) return Result::failure("couldn't write " + out.string());
                        } else {
                            const int size = 1024;
                            const double k = (size - 20) / std::max(W, H);
                            raster::Image img(size, size, {255, 255, 255, 255});
                            for (std::size_t i = 0; i < sec.lines.size(); ++i) {
                                const auto& line = sec.lines[i];
                                const raster::Color c = i == pick ? raster::Color{214, 60, 40, 255} : raster::Color{40, 44, 52, 255};
                                for (std::size_t j = 0; j + 1 < line.size() + (sec.closed[i] ? 1 : 0); ++j) {
                                    const Vec3 l0 = fr.toLocal(line[j]), l1 = fr.toLocal(line[(j + 1) % line.size()]);
                                    drawLine(img, 10 + (l0.x - a0 + margin) * k, 10 + (b1 - l0.y + margin) * k, 10 + (l1.x - a0 + margin) * k,
                                             10 + (b1 - l1.y + margin) * k, c);
                                }
                            }
                            raster::save(img, out);
                        }
                        r.message += "\nwrote " + out.string();
                        r.with("path", out.string());
                    }
                    return r;
                });
    }

    addRead(session,
            {"fit.radius", "Local Radius",
             "How curved the scan is at a spot: fits a sphere to the patch within radius= of the point. Big numbers mean nearly flat.",
             {docParam(),
              {"at", ParamType::NumberList, "the spot x,y,z (snapped onto the scan)", true},
              {"radius", ParamType::Number, "patch size around it, mm", false, {}, 15.0}},
             false, true},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const SurfacePoint sp = snap(doc, requireVec3(p, "at"));
                const double rad = numberParam(p, "radius", 15);
                const Patch patch = patchAround(*doc.state.scan, scanVertexNormals(doc), sp.point, rad);
                const PlaneFit pf = fitPlane(patch);
                double curv = 1e9;
                std::string msg;
                try {
                    const SphereFit sf = fitSphere(patch);
                    curv = sf.radius;
                } catch (const std::exception&) {
                }
                if (curv > rad * 40 || pf.rms < 0.02) msg = "nearly flat here (radius over " + num(rad * 40) + " mm, flatness " + num(pf.rms) + " mm)";
                else msg = "curve radius about " + num(curv) + " mm here (flatness " + num(pf.rms) + " mm)";
                Result r = Result::success(msg, curv);
                return r.with("radius", curv).with("flatness", pf.rms).with("point", toList(sp.point)).with("normal", toList(sp.normal));
            });

    addRead(session,
            {"fit.snap", "Snap to Scan", "The nearest point on the scan to a rough point, the way the surface faces there, and how far away it was.",
             {docParam(), {"point", ParamType::NumberList, "rough point x,y,z", true}}, false, true},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const SurfacePoint sp = snap(doc, requireVec3(p, "point"));
                Result r = Result::success("on the scan at " + vecText(sp.point) + " facing " + vecText(sp.normal) + " (" + num(sp.distance) + " mm away)");
                return r.with("point", toList(sp.point)).with("normal", toList(sp.normal)).with("distance", sp.distance);
            });

    addRead(session,
            {"fit.pick", "Pick Point",
             "Turn a spot on a picture (from fit.preview or any preview=true, same view/yaw/pitch/show) into a 3D point on the scan: u from 0 at "
             "the left to 1 at the right, v from 0 at the top to 1 at the bottom. This is how to aim at what you see.",
             {docParam(),
              {"u", ParamType::Number, "0..1 across the picture", true},
              {"v", ParamType::Number, "0..1 down the picture", true},
              {"view", ParamType::String, "camera of the picture", false, mesh::viewNames(), std::string("iso")},
              {"yaw", ParamType::Number, "yaw of the picture", false, {}, 0.0},
              {"pitch", ParamType::Number, "pitch of the picture", false, {}, 0.0},
              {"show", ParamType::String, "what the picture showed", false, {"all", "scan", "parts"}, std::string("all")}},
             false, true},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                SceneOptions so;
                const std::string show = stringParam(p, "show", "all");
                so.scan = show != "parts";
                so.parts = show != "scan";
                so.refs = show == "all";
                const Mesh scene = sceneMesh(doc, so, nullptr);
                sculpt::ViewRay v;
                if (auto parsed = mesh::parseView(stringParam(p, "view", "iso"))) v.view = *parsed;
                v.yaw = numberParam(p, "yaw", 0);
                v.pitch = numberParam(p, "pitch", 0);
                auto hit = sculpt::screenToSurface(scene, v, numberParam(p, "u"), numberParam(p, "v"));
                if (!hit) return Result::failure("nothing there: that spot of the picture is background");
                const SurfacePoint sp = snap(doc, *hit);
                std::string on = "scan";
                for (const auto& [name, part] : doc.state.parts) {
                    if (!part.mesh || !so.parts) continue;
                    Bvh pb(part.mesh);
                    if (pb.nearest(*hit).distance + 1e-6 < sp.distance) on = name;
                }
                Result r = Result::success("picked " + vecText(*hit) + " on " + on);
                return r.with("point", toList(*hit)).with("normal", toList(sp.normal)).with("on", on);
            });
}

}  // namespace wetwaresuite::fit::detail
