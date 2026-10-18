// fit.* commands for opening, cleaning, scaling and aligning the scan.
#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::fit::detail {

namespace fs = std::filesystem;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

std::string health(const FitDoc& doc) {
    const Mesh& m = *doc.state.scan;
    const auto loops = boundaryLoops(m);
    const auto ps = pieces(m);
    const double noise = noiseLevel(m);
    std::size_t small = 0;
    for (const auto& l : loops) {
        double per = 0;
        for (std::size_t i = 0; i < l.size(); ++i) per += length(m.positions[l[(i + 1) % l.size()]] - m.positions[l[i]]);
        small += per / mesh::kPi <= 25 ? 1 : 0;
    }
    std::string s = mesh::describe(m);
    s += "\n" + std::to_string(ps.size()) + " piece" + (ps.size() == 1 ? "" : "s") + ", " + std::to_string(loops.size()) + " open edge loop" +
         (loops.size() == 1 ? "" : "s") + " (" + std::to_string(small) + " small holes), noise about " + num(noise) + " mm, edges about " +
         num(meanEdgeLength(m)) + " mm";
    std::vector<std::string> tips;
    if (ps.size() > 1) tips.push_back("fit.keep_largest drops the " + std::to_string(ps.size() - 1) + " loose bits");
    if (small) tips.push_back("fit.fill_holes patches the small holes");
    if (noise > 0.3) tips.push_back("fit.smooth calms the noise");
    if (!tips.empty()) s += "\nnext: " + joinList(tips);
    return s;
}

Result scanResult(const std::string& message, FitDoc& doc, const Params& p) {
    Result r = done(message, doc, p);
    const Mesh& m = *doc.state.scan;
    const Vec3 sz = mesh::bounds(m).size();
    r.with("triangles", static_cast<std::int64_t>(m.triangleCount())).with("size", NumberList{sz.x, sz.y, sz.z});
    return r;
}

void requireScan(const FitDoc& doc) {
    if (!doc.state.scan || doc.state.scan->empty()) throw std::runtime_error("the scan is empty (every triangle was removed); undo the last step");
}

}  // namespace

void registerScan(Session& session) {
    auto& reg = session.registry();
    reg.add({"fit.open", "Open Scan",
             "Open a phone 3D scan (OBJ, PLY, STL, GLB/glTF, OFF, 3MF, or XYZ/PTS points) or a .wwfit project. Scans are turned to "
             "millimetres (apps save metres) and Z-up (apps save Y-up), and big ones are simplified so everything stays fast. Returns the "
             "document id (fit1) and a health report.",
             withPreview({{"path", ParamType::String, "scan file or .wwfit project", true},
                          {"units", ParamType::String, "what the file's numbers are; auto takes anything under 10 units across as metres", false,
                           {"auto", "mm", "cm", "m", "in"}, std::string("auto")},
                          {"up", ParamType::String, "which way is up in the file; auto: Y for OBJ/PLY/points (phone apps), Z for STL/3MF, glTF handled", false,
                           {"auto", "y", "z"}, std::string("auto")},
                          {"budget", ParamType::Int, "most triangles to keep (bigger scans are simplified; 0 = keep all)", false, {}, std::int64_t{300000}},
                          {"cell", ParamType::Number, "point clouds: voxel size in mm (0 = automatic)", false, {}, 0.0}}),
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const std::string path = stringParam(p, "path");
                        std::shared_ptr<FitDoc> doc =
                            lowerExt(path) == "wwfit" ? loadProject(path) : openScan(path, p);
                        const std::string id = session.workspace().add(doc, "fit");
                        Params q = p;
                        q["doc"] = id;
                        const ScanInfo& in = doc->state.info;
                        std::string msg = "opened " + id + ": " + health(*doc);
                        if (lowerExt(path) == "wwfit") {
                            msg += "\nreplayed " + std::to_string(doc->state.log.size()) + " steps: " + std::to_string(doc->state.refs.size()) +
                                   " references, " + std::to_string(doc->state.parts.size()) + " parts";
                        } else {
                            if (in.scale != 1) msg += "\nfile was in " + in.units + ", scaled x" + num(in.scale) + " to mm";
                            if (in.yUp) msg += "\nturned from Y-up to Z-up";
                            if (in.pointCloud) msg += "\nbuilt a surface from " + std::to_string(in.points) + " points";
                            if (in.simplified) msg += "\nsimplified from " + std::to_string(in.fileTriangles) + " triangles";
                        }
                        Result r = Result::success(msg, id);
                        const Vec3 sz = mesh::bounds(*doc->state.scan).size();
                        r.with("doc", id)
                            .with("triangles", static_cast<std::int64_t>(doc->state.scan->triangleCount()))
                            .with("size", NumberList{sz.x, sz.y, sz.z})
                            .with("scale", in.scale);
                        return addPreview(r, *doc, q);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"fit.close", "Close Scan", "Close a scan document (save a project first with fit.save_project to keep it).", {docParam()}, false, false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                return std::make_unique<FunctionAction>([&session, id] {
                    if (!session.workspace().getAs<FitDoc>(id)) return Result::failure("no open scan called '" + id + "'");
                    session.workspace().take(id);
                    return Result::success("closed " + id);
                });
            });

    addRead(session,
            {"fit.info", "Scan Info", "Health report (size in mm, loose pieces, holes, noise, what to fix next) plus every reference and part.",
             withPreview({docParam()}), false, true},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                std::string s = health(doc);
                if (!doc.state.refs.empty()) {
                    s += "\nreferences:";
                    for (const auto& [n, r] : doc.state.refs) s += "\n  " + describeRef(n, r);
                }
                if (!doc.state.parts.empty()) {
                    s += "\nparts:";
                    for (const auto& [n, pt] : doc.state.parts) s += "\n  " + describePart(n, pt);
                }
                Result r = scanResult(s, doc, p);
                r.with("references", static_cast<std::int64_t>(doc.state.refs.size())).with("parts", static_cast<std::int64_t>(doc.state.parts.size()));
                r.with("noise", noiseLevel(*doc.state.scan));
                return r;
            });

    addEdit(session,
            {"fit.keep_largest", "Keep Largest Piece",
             "Drop floating junk (bits of seat, your hand, the floor) by keeping only the biggest connected pieces of the scan.",
             withPreview({docParam(),
                          {"pieces", ParamType::Int, "how many of the biggest pieces to keep", false, {}, std::int64_t{1}},
                          {"min_fraction", ParamType::Number, "also keep pieces at least this share of the biggest's area (0..1)", false, {}, 0.0}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                Mesh m = *doc.state.scan;
                const std::size_t before = pieces(m).size();
                const std::size_t removed = keepLargest(m, static_cast<int>(intParam(p, "pieces", 1)), numberParam(p, "min_fraction", 0));
                setScan(doc, std::move(m));
                requireScan(doc);
                return scanResult("kept " + std::to_string(pieces(*doc.state.scan).size()) + " of " + std::to_string(before) + " pieces, removed " +
                                      std::to_string(removed) + " triangles",
                                  doc, p);
            });

    addEdit(session,
            {"fit.crop", "Crop Scan",
             "Keep only part of the scan: inside a box (min=, max=) or on one side of a plane (point=, normal=: keeps the side the normal points to).",
             withPreview({docParam(),
                          {"min", ParamType::NumberList, "box corner x,y,z (mm)", false},
                          {"max", ParamType::NumberList, "opposite box corner x,y,z (mm)", false},
                          {"inside", ParamType::Bool, "box: keep inside (false keeps outside)", false, {}, true},
                          {"point", ParamType::NumberList, "plane: a point on it", false},
                          {"normal", ParamType::NumberList, "plane: direction of the side to keep", false}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                Mesh m = *doc.state.scan;
                std::size_t removed = 0;
                if (auto lo = vec3Param(p, "min")) {
                    Bounds b;
                    b.add(*lo);
                    b.add(requireVec3(p, "max"));
                    removed = cropBox(m, b, boolParam(p, "inside", true));
                } else if (auto pt = vec3Param(p, "point")) {
                    removed = cropPlane(m, *pt, requireVec3(p, "normal"));
                } else {
                    return Result::failure("give min= and max= for a box, or point= and normal= for a plane");
                }
                setScan(doc, std::move(m));
                requireScan(doc);
                return scanResult("cropped away " + std::to_string(removed) + " triangles", doc, p);
            });

    addEdit(session,
            {"fit.remove_floor", "Remove Floor",
             "Find the lowest big flat level area (the floor or a table under the scan) and delete it and everything below it.",
             withPreview({docParam(), {"tolerance", ParamType::Number, "how far from flat still counts as floor, mm (0 = from the scan size)", false, {}, 0.0}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                Mesh m = *doc.state.scan;
                const FloorResult f = removeFloor(m, numberParam(p, "tolerance", 0));
                if (!f.found) return Result::failure("no big flat level area found under the scan; try fit.crop with a plane");
                setScan(doc, std::move(m));
                requireScan(doc);
                Result r = scanResult("removed the floor at z = " + num(f.point.z) + " (" + std::to_string(f.removed) + " triangles)", doc, p);
                return r.with("floor_z", f.point.z);
            });

    addEdit(session,
            {"fit.fill_holes", "Fill Holes", "Patch holes in the scan up to max_size mm across, so builders and fits don't fall through them.",
             withPreview({docParam(), {"max_size", ParamType::Number, "biggest hole to fill, mm across (the scan's outer edge is usually far bigger)", false, {}, 25.0}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                Mesh m = *doc.state.scan;
                const HoleStats h = fillHoles(m, numberParam(p, "max_size", 25));
                setScan(doc, std::move(m));
                Result r = scanResult("filled " + std::to_string(h.filled) + " holes (largest " + num(h.largestFilled) + " mm), left " +
                                          std::to_string(h.skipped) + " bigger ones",
                                      doc, p);
                return r.with("filled", static_cast<std::int64_t>(h.filled)).with("skipped", static_cast<std::int64_t>(h.skipped));
            });

    addEdit(session,
            {"fit.smooth", "Smooth Noise", "Remove scanner noise (the orange-peel look) without shrinking the shape (Taubin smoothing). Open edges stay put.",
             withPreview({docParam(),
                          {"iterations", ParamType::Int, "passes", false, {}, std::int64_t{3}},
                          {"strength", ParamType::Number, "0..1 per pass", false, {}, 0.5}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                Mesh m = *doc.state.scan;
                const double before = noiseLevel(m);
                const double moved = sculpt::smooth(m, {}, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "iterations", 3), 1, 100)),
                                                    std::clamp(numberParam(p, "strength", 0.5), 0.0, 1.0), true);
                setScan(doc, std::move(m));
                return scanResult("smoothed: noise " + num(before) + " -> " + num(noiseLevel(*doc.state.scan)) + " mm (moved up to " + num(moved) + " mm)",
                                  doc, p);
            });

    addEdit(session,
            {"fit.simplify", "Simplify Scan", "Fewer triangles with the same shape (quadric edge collapse), to keep a weak pc fast.",
             withPreview({docParam(), {"triangles", ParamType::Int, "about how many triangles to keep", true}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const auto target = static_cast<std::size_t>(std::max<std::int64_t>(100, intParam(p, "triangles", 50000)));
                const std::size_t before = doc.state.scan->triangleCount();
                setScan(doc, sculpt::decimateQuadric(*doc.state.scan, target));
                return scanResult("simplified " + std::to_string(before) + " -> " + std::to_string(doc.state.scan->triangleCount()) + " triangles", doc, p);
            });

    addEdit(session,
            {"fit.scale", "Scale Scan",
             "Fix the scan's size from a real measurement: from= and to= are two points on the scan and length= what a ruler says between them on "
             "the car; or axis= and size= (the scan's extent along that axis); or a plain factor=. References and parts scale too.",
             withPreview({docParam(),
                          {"from", ParamType::NumberList, "first measured point x,y,z", false},
                          {"to", ParamType::NumberList, "second measured point x,y,z", false},
                          {"length", ParamType::Number, "real distance between from and to, mm", false},
                          {"axis", ParamType::String, "with size: x, y or z", false, {"x", "y", "z"}},
                          {"size", ParamType::Number, "with axis: real extent along it, mm", false},
                          {"factor", ParamType::Number, "plain scale factor", false}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                double f = 0;
                if (p.count("factor")) {
                    f = numberParam(p, "factor");
                } else if (p.count("length")) {
                    const double now = length(requireVec3(p, "to") - requireVec3(p, "from"));
                    if (now <= 0) return Result::failure("from and to are the same point");
                    f = numberParam(p, "length") / now;
                } else if (p.count("size")) {
                    const std::string ax = stringParam(p, "axis", "x");
                    const int a = ax == "y" ? 1 : ax == "z" ? 2 : 0;
                    const double now = mesh::bounds(*doc.state.scan).size()[a];
                    if (now <= 0) return Result::failure("the scan is flat along " + ax);
                    f = numberParam(p, "size") / now;
                } else {
                    return Result::failure("give from=, to= and length= (best), or axis= and size=, or factor=");
                }
                if (!(f > 0) || !std::isfinite(f)) return Result::failure("the scale factor must be above 0");
                transformAll(doc, Mat4::scale(f));
                Result r = scanResult("scaled x" + num(f), doc, p);
                return r.with("factor", f);
            });

    addEdit(session,
            {"fit.align", "Align Scan",
             "Turn the scan square to the axes. mode=auto lines its main directions up with X, Y, Z. mode=plane turns a fitted plane (plane=) to "
             "face the given direction (face=up makes it the floor) and, with origin=true, moves it to z = 0 / the origin. Everything turns together.",
             withPreview({docParam(),
                          {"mode", ParamType::String, "auto or plane", false, {"auto", "plane"}, std::string("auto")},
                          {"plane", ParamType::String, "mode=plane: a plane reference from fit.plane", false},
                          {"face", ParamType::String, "mode=plane: which way the plane should face", false, {"up", "down", "front", "back", "left", "right"}, std::string("up")},
                          {"origin", ParamType::Bool, "mode=plane: also move the plane's point to the origin", false, {}, true}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                if (stringParam(p, "mode", "auto") == "auto") {
                    transformAll(doc, principalAlignment(*doc.state.scan));
                    const Bounds b = mesh::bounds(*doc.state.scan);
                    transformAll(doc, Mat4::translate({-b.center().x, -b.center().y, -b.min.z}));
                    return scanResult("lined the scan up with X, Y and Z (centred, resting on z = 0)", doc, p);
                }
                const Ref& pl = refParam(doc, p, "plane", RefKind::Plane);
                const std::string face = stringParam(p, "face", "up");
                const Vec3 target = face == "up" ? Vec3{0, 0, 1} : face == "down" ? Vec3{0, 0, -1} : face == "front" ? Vec3{0, -1, 0}
                                  : face == "back" ? Vec3{0, 1, 0} : face == "left" ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
                const Vec3 pt = pl.point;
                Mat4 t = Mat4::translate(pt) * rotationBetween(pl.dir, target) * Mat4::translate(-pt);
                if (boolParam(p, "origin", true)) t = Mat4::translate(-pt) * t;
                transformAll(doc, t);
                return scanResult("turned " + stringParam(p, "plane") + " to face " + face, doc, p);
            });

    addEdit(session,
            {"fit.move", "Move Scan", "Move the scan with its references and parts: by= an offset, or origin= a point that becomes 0,0,0 (a bolt hole, a corner).",
             withPreview({docParam(),
                          {"by", ParamType::NumberList, "offset x,y,z (mm)", false},
                          {"origin", ParamType::NumberList, "this point moves to 0,0,0", false}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                Vec3 d;
                if (auto o = vec3Param(p, "origin")) d = -*o;
                else if (auto b = vec3Param(p, "by")) d = *b;
                else return Result::failure("give by=x,y,z or origin=x,y,z");
                transformAll(doc, Mat4::translate(d));
                return scanResult("moved by " + vecText(d), doc, p);
            });

    addEdit(session,
            {"fit.rotate", "Rotate Scan", "Rotate the scan with its references and parts, degrees about X, then Y, then Z, around about= (default the scan centre).",
             withPreview({docParam(),
                          {"degrees", ParamType::NumberList, "x,y,z degrees", true},
                          {"about", ParamType::NumberList, "pivot x,y,z (default: centre of the scan)", false}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const Vec3 deg = requireVec3(p, "degrees");
                const Vec3 c = vec3Param(p, "about").value_or(mesh::bounds(*doc.state.scan).center());
                transformAll(doc, Mat4::translate(c) * Mat4::rotate(deg) * Mat4::translate(-c));
                return scanResult("rotated by " + vecText(deg) + " degrees", doc, p);
            });

    addEdit(session,
            {"fit.mirror", "Mirror Scan",
             "Mirror the scan (with references and parts) across a plane through `at` perpendicular to an axis: scanned the left side, build the right.",
             withPreview({docParam(),
                          {"axis", ParamType::String, "mirror across this axis", false, {"x", "y", "z"}, std::string("x")},
                          {"at", ParamType::Number, "where the mirror plane crosses the axis, mm", false, {}, 0.0}})},
            [](FitDoc& doc, const Params& p) {
                requireScan(doc);
                const std::string ax = stringParam(p, "axis", "x");
                Vec3 s{1, 1, 1}, o;
                const int a = ax == "y" ? 1 : ax == "z" ? 2 : 0;
                s[a] = -1;
                o[a] = numberParam(p, "at", 0);
                transformAll(doc, Mat4::translate(o) * Mat4::scale(s) * Mat4::translate(-o));
                return scanResult("mirrored across " + ax + " = " + num(o[a]), doc, p);
            });
}

}  // namespace wetwaresuite::fit::detail
