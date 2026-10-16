// fit.* commands that show, check and save: pictures, fit and print checks,
// exporting parts (with shrink allowance and test slices) and projects.
#include <algorithm>
#include <cmath>
#include <map>

#include "internal.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::fit::detail {

namespace fs = std::filesystem;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

SceneOptions sceneOf(const Params& p) {
    SceneOptions so;
    const std::string show = stringParam(p, "show", "all");
    so.scan = show != "parts";
    so.parts = show != "scan";
    so.refs = show == "all";
    so.heatPart = stringParam(p, "heat");
    return so;
}

std::string dirName(const Vec3& d) {
    const char* names[6] = {"-x", "+x", "-y", "+y", "-z", "+z"};
    const Vec3 axes[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (int i = 0; i < 6; ++i)
        if (dot(d, axes[i]) > 0.999) return names[i];
    return vecText(d);
}

// Shrink allowance in percent for a material name, or a plain number.
double shrinkPercent(const std::string& s) {
    static const std::map<std::string, double> m = {{"none", 0}, {"pla", 0.2}, {"petg", 0.3}, {"asa", 0.5}, {"abs", 0.7}, {"nylon", 1.5}, {"tpu", 0.4}};
    auto it = m.find(s);
    if (it != m.end()) return it->second;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end) throw std::runtime_error("shrink must be pla, petg, asa, abs, nylon, tpu, none or a percent like 0.5");
    return v;
}

}  // namespace

void registerOutput(Session& session) {
    addRead(session,
            {"fit.preview", "Preview",
             "A picture of the scan (grey), reference shapes (orange) and parts (blue and friends). heat=<part> colours that part by its gap "
             "to the scan: blue touching at the set gap, through green and yellow, to red 3 mm away, purple poking into the scan.",
             {docParam(),
              {"view", ParamType::String, "camera", false, mesh::viewNames(), std::string("iso")},
              {"yaw", ParamType::Number, "turn about Z first, degrees", false, {}, 0.0},
              {"pitch", ParamType::Number, "tilt toward the camera, degrees", false, {}, 0.0},
              {"size", ParamType::Int, "picture size in pixels", false, {}, std::int64_t{512}},
              {"show", ParamType::String, "what to draw", false, {"all", "scan", "parts"}, std::string("all")},
              {"heat", ParamType::String, "colour this part by its gap", false}},
             false, true},
            [](FitDoc& doc, const Params& p) {
                const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 32, 4096));
                const SceneOptions so = sceneOf(p);
                if (!so.heatPart.empty()) partOf(doc, so.heatPart);
                Result r = Result::success("preview of " + stringParam(p, "doc"));
                r.attach({"image/png", raster::encodePng(renderScene(doc, p, size, so)), "preview.png"});
                return r;
            });

    addRead(session,
            {"fit.check", "Check Fit",
             "How well a part fits the scan: smallest and biggest gap, how much of it sits at the set gap, and how much pokes into the scan. "
             "Attaches a gap heat map picture.",
             {docParam(),
              {"part", ParamType::String, "part name", true},
              {"range", ParamType::Number, "only count part points this close to the scan, mm", false, {}, 5.0},
              {"view", ParamType::String, "camera for the heat map", false, mesh::viewNames(), std::string("iso")},
              {"yaw", ParamType::Number, "turn about Z, degrees", false, {}, 0.0},
              {"pitch", ParamType::Number, "tilt, degrees", false, {}, 0.0}},
             false, true},
            [](FitDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "part");
                const Part& part = partOf(doc, name);
                const FitReport r = checkFit(doc.bvh(), *part.mesh, numberParam(p, "range", 5), part.gap);
                if (!r.near) return Result::failure(name + " isn't within " + num(numberParam(p, "range", 5)) + " mm of the scan anywhere");
                std::string msg = name + ": gap " + num(r.minGap) + " to " + num(r.maxGap) + " mm (mean " + num(r.meanGap) + ", built with " + num(part.gap) +
                                  "); " + num(r.contact * 100) + "% of the facing side sits within gap + 0.5 mm";
                if (r.inside > 0.001) msg += "; " + num(r.inside * 100) + "% pokes into the scan: raise the gap (fit.rebuild part=" + name + " gap=...)";
                else if (r.contact < 0.5) msg += "; it only touches in places: lower smooth= or check the placement";
                else msg += "; looks like a good fit";
                Result res = Result::success(msg);
                res.with("min_gap", r.minGap).with("max_gap", r.maxGap).with("mean_gap", r.meanGap).with("contact", r.contact).with("inside", r.inside);
                SceneOptions so;
                so.refs = false;
                so.heatPart = name;
                res.attach({"image/png", raster::encodePng(renderScene(doc, p, 512, so)), "fit.png"});
                return res;
            });

    addRead(session,
            {"fit.printability", "Check Printability",
             "Is a part ready to print: closed (watertight), volume, size, thinnest wall, and which side to put on the bed for the fewest overhangs.",
             {docParam(), {"part", ParamType::String, "part name", true}}, false, true},
            [](FitDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "part");
                const Part& part = partOf(doc, name);
                std::vector<Vec3> downs;
                if (length(part.normal) > 0) downs = {-part.normal, part.normal};
                const PrintReport r = checkPrint(*part.mesh, downs);
                std::string msg = name + ": " + (r.watertight ? "closed" : "NOT closed (has holes)") + ", " + num(r.volume / 1000) + " cm3, " + num(r.size.x) +
                                  " x " + num(r.size.y) + " x " + num(r.size.z) + " mm, thinnest wall about " + num(r.thinnest) + " mm";
                if (r.thinnest > 0 && r.thinnest < 1.2) msg += " - thin for FDM, raise thickness";
                msg += "; print with the " + dirName(r.down) + " side down (" + num(r.overhang * 100) + "% overhang)";
                Result res = Result::success(msg);
                return res.with("watertight", r.watertight)
                    .with("volume", r.volume)
                    .with("thinnest", r.thinnest)
                    .with("down", toList(r.down))
                    .with("overhang", r.overhang);
            });

    addRead(session,
            {"fit.export", "Export Part",
             "Save parts for printing: STL, 3MF, OBJ, PLY, GLB (or a PNG picture). part=all (default) puts every part in one file, or "
             "separate=true writes one file each. shrink= scales up for a material's shrinkage; test_slice= mm saves just a thin slice "
             "of the contact surface, to print in minutes and check the fit first.",
             {docParam(),
              {"path", ParamType::String, "file to write (format from the extension)", true},
              {"part", ParamType::String, "part name, or all", false, {}, std::string("all")},
              {"separate", ParamType::Bool, "part=all: one file per part, named <path>-<part>.<ext>", false, {}, false},
              {"shrink", ParamType::String, "pla, petg, asa, abs, nylon, tpu, none, or a percent", false, {}, std::string("none")},
              {"test_slice", ParamType::Number, "save only a thin slice this thick of the fitting surface, mm", false, {}, 0.0},
              {"scan", ParamType::Bool, "include the scan too (for checking in other apps, not printing)", false, {}, false},
              {"view", ParamType::String, "PNG: camera", false, mesh::viewNames(), std::string("iso")},
              {"size", ParamType::Int, "PNG: pixels", false, {}, std::int64_t{1024}}},
             false, false},
            [](FitDoc& doc, const Params& p) {
                const fs::path path = stringParam(p, "path");
                const std::string ext = lowerExt(path);
                std::error_code ec;
                if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
                if (ext == "png" || ext == "jpg" || ext == "jpeg") {
                    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 32, 8192));
                    raster::save(renderScene(doc, p, size, {}), path);
                    return Result::success("wrote a picture to " + path.string()).with("path", path.string());
                }
                if (!mesh::canWrite(path)) return Result::failure("can't save ." + ext + "; use stl, 3mf, obj, ply, glb, gltf or off (or png)");
                if (doc.state.parts.empty()) return Result::failure("no parts yet (fit.plate, fit.clamp, fit.phone_mount, ... make them)");
                const std::string which = stringParam(p, "part", "all");
                std::vector<std::string> names;
                if (which == "all") {
                    for (const auto& [n, pt] : doc.state.parts) names.push_back(n);
                } else {
                    partOf(doc, which);
                    names.push_back(which);
                }
                const double test = numberParam(p, "test_slice", 0);
                std::map<std::string, Mesh> meshes;
                for (const auto& n : names) meshes[n] = *doc.state.parts.at(n).mesh;
                if (test > 0) {
                    // rebuild each part as it was first built, with only a thin slice of it
                    for (const auto& n : names) {
                        const Part& part = doc.state.parts.at(n);
                        if (part.step >= doc.state.log.size()) return Result::failure(n + " has no build step to slice");
                        std::vector<std::pair<std::string, Params>> log(doc.state.log.begin(),
                                                                         doc.state.log.begin() + static_cast<std::ptrdiff_t>(part.step) + 1);
                        log.back().second["test_slice"] = test;
                        const FitState st = replay(doc.state, log);
                        auto it = st.parts.find(n);
                        if (it == st.parts.end()) return Result::failure("couldn't make a test slice of " + n);
                        meshes[n] = *it->second.mesh;
                    }
                }
                const double pct = shrinkPercent(stringParam(p, "shrink", "none"));
                if (pct != 0) {
                    for (auto& [n, m] : meshes) {
                        const Vec3 c = mesh::bounds(m).center();
                        mesh::transform(m, Mat4::translate(c) * Mat4::scale(1 + pct / 100) * Mat4::translate(-c));
                    }
                }
                std::vector<std::string> written;
                if (boolParam(p, "separate", false) && names.size() > 1) {
                    for (const auto& [n, m] : meshes) {
                        fs::path out = path.parent_path() / (path.stem().string() + "-" + n + path.extension().string());
                        mesh::save(out, m);
                        written.push_back(out.string());
                    }
                } else {
                    Mesh all;
                    for (const auto& [n, m] : meshes) mesh::append(all, m);
                    if (boolParam(p, "scan", false) && doc.state.scan) mesh::append(all, *doc.state.scan);
                    mesh::save(path, all);
                    written.push_back(path.string());
                }
                std::string msg = "saved " + joinList(names) + (test > 0 ? " (test slice " + num(test) + " mm)" : "") +
                                  (pct != 0 ? " scaled up " + num(pct) + "% for shrink" : "") + " to " + joinList(written);
                Result r = Result::success(msg);
                return r.with("path", written.front()).with("files", static_cast<std::int64_t>(written.size()));
            });

    addRead(session,
            {"fit.save_project", "Save Project",
             "Save a .wwfit project: the scan file's location and every step since opening (cleanup, alignment, references, parts), so it "
             "reopens with fit.open and any part can be rebuilt with new settings. Keep the scan file next to it.",
             {docParam(), {"path", ParamType::String, "file to write (.wwfit)", true}}, false, false},
            [](FitDoc& doc, const Params& p) {
                fs::path path = stringParam(p, "path");
                if (lowerExt(path) != "wwfit") path += ".wwfit";
                saveProject(doc, path);
                doc.path = path.string();
                return Result::success("saved the project (" + std::to_string(doc.state.log.size()) + " steps) to " + path.string()).with("path", path.string());
            });
}

}  // namespace wetwaresuite::fit::detail
