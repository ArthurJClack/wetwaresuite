// slicer.* commands: print job documents, presets and settings, slicing,
// G-code export and previews.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/mesh/render.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/slicer/slicer.hpp"

namespace wetwaresuite::slicer {

namespace fs = std::filesystem;

namespace {

std::string fmt(double v, int decimals = 1) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string duration(double seconds) {
    const long s = std::lround(seconds);
    const long h = s / 3600, m = (s % 3600) / 60;
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    if (m > 0) return std::to_string(m) + "m " + std::to_string(s % 60) + "s";
    return std::to_string(s) + "s";
}

// Everything undo has to restore.
struct PrintState {
    mesh::Mesh mesh;
    Config config;
};

struct Sliced {
    SliceResult result;
    Gcode gcode;
};

class PrintDoc : public Document {
public:
    PrintState state;
    std::string name;  // model name for messages and the G-code header
    std::shared_ptr<const Sliced> sliced;  // cache, cleared by every edit

    std::string kind() const override { return "print"; }
    std::string summary() const override {
        const mesh::Vec3 sz = mesh::bounds(state.mesh).size();
        std::string s = name + ": " + fmt(sz.x) + " x " + fmt(sz.y) + " x " + fmt(sz.z) + " mm on " + state.config.printer.name + ", " +
                        state.config.filament.name + ", " + state.config.settings.quality + " " + fmt(state.config.settings.layerHeight, 2) +
                        " mm layers";
        if (!checkFit(state.mesh, state.config.printer).fits) s += ", DOESN'T FIT";
        if (sliced) s += ", sliced: " + std::to_string(sliced->result.layers.size()) + " layers, " + duration(sliced->gcode.seconds);
        return s;
    }
    std::unique_ptr<Attachment> preview() const override;
};

raster::Image renderOnBed(const PrintState& st, mesh::View view, int size, bool bed) {
    mesh::Mesh scene = st.mesh;
    if (bed) {
        const Printer& p = st.config.printer;
        mesh::Mesh plate = mesh::box({p.bedX, p.bedY, 0.8}, false);
        mesh::transform(plate, mesh::Mat4::translate({0, 0, -0.8}));
        mesh::append(scene, plate);
    }
    mesh::RenderOptions o;
    o.view = view;
    return mesh::render(scene, size, size, o);
}

std::unique_ptr<Attachment> PrintDoc::preview() const {
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(renderOnBed(state, mesh::View::Iso, 512, true)), "print.png"});
}

class PrintEdit : public Action {
public:
    PrintEdit(std::shared_ptr<PrintDoc> doc, std::function<Result(PrintDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override {
        before_ = doc_->state;
        cached_ = doc_->sliced;
        Result r;
        try {
            r = fn_(*doc_);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) revert();
        else doc_->sliced.reset();
        return r;
    }
    void revert() override {
        doc_->state = before_;
        doc_->sliced = cached_;
    }

private:
    std::shared_ptr<PrintDoc> doc_;
    std::function<Result(PrintDoc&)> fn_;
    PrintState before_;
    std::shared_ptr<const Sliced> cached_;
};

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open print job called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open print job, e.g. print1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG picture of the result", false, {}, false}; }

ParamSpec num(const char* name, const char* description) { return {name, ParamType::Number, description, false}; }
ParamSpec integer(const char* name, const char* description) { return {name, ParamType::Int, description, false}; }

std::vector<ParamSpec> settingsParams() {
    std::vector<std::string> printers = printerNames();
    return {
        {"printer", ParamType::String, "printer preset (sets bed size, nozzle and start/end G-code)", false, printers},
        num("bed_x", "bed width in mm (custom printer)"),
        num("bed_y", "bed depth in mm"),
        num("bed_z", "maximum print height in mm"),
        num("nozzle", "nozzle diameter in mm, e.g. 0.4"),
        {"filament", ParamType::String, "filament preset (temperatures, fan, density)", false, filamentNames()},
        num("nozzle_temp", "nozzle temperature in C (first layer gets +5 unless first_layer_temp is set)"),
        num("first_layer_temp", "first-layer nozzle temperature in C"),
        num("bed_temp", "bed temperature in C"),
        num("fan", "part cooling fan in percent from the second layer"),
        {"quality", ParamType::String, "quality preset: draft 0.28, normal 0.2, fine 0.12 mm layers", false, qualityNames()},
        num("layer_height", "layer height in mm"),
        num("first_layer_height", "first layer height in mm"),
        num("line_width", "extrusion width in mm (default nozzle x 1.125)"),
        integer("perimeters", "number of walls"),
        integer("top_layers", "solid layers on top surfaces"),
        integer("bottom_layers", "solid layers on bottom surfaces"),
        num("infill", "sparse infill density in percent (100 = solid)"),
        {"infill_pattern", ParamType::String, "sparse infill pattern", false, {"lines", "grid", "triangles"}},
        num("speed", "print speed in mm/s"),
        num("first_layer_speed", "first layer speed in mm/s"),
        num("travel_speed", "travel speed in mm/s"),
        num("retraction", "retraction length in mm (0 = off)"),
        num("retraction_speed", "retraction speed in mm/s"),
        integer("skirt", "skirt loops around the first layer (0 = none)"),
        num("skirt_distance", "gap between skirt and part (or brim) in mm"),
        num("brim", "brim width in mm (0 = none)"),
        {"supports", ParamType::String, "support columns under overhangs: everywhere, or only those standing on the bed", false,
         {"none", "everywhere", "buildplate"}},
        num("support_angle", "overhang angle from vertical that needs support, degrees"),
        num("support_density", "support line density in percent"),
        {"seam", ParamType::String, "where wall loops start", false, {"nearest", "rear", "random"}},
        {"start_gcode", ParamType::String, "custom start G-code; '|' separates lines; {bed_temp} {nozzle_temp} {first_layer_temp} are filled in", false},
        {"end_gcode", ParamType::String, "custom end G-code; '|' separates lines; {park_z} {bed_x} {bed_y} are filled in", false},
    };
}

// Applies presets, then overrides. Returns an error message, or empty.
std::string applySettings(Config& c, const Params& p) {
    if (p.count("printer")) {
        auto pr = printerPreset(stringParam(p, "printer"));
        if (!pr) return "unknown printer '" + stringParam(p, "printer") + "'";
        pr->nozzle = c.printer.nozzle;
        c.printer = *pr;
    }
    if (p.count("filament")) {
        auto f = filamentPreset(stringParam(p, "filament"));
        if (!f) return "unknown filament '" + stringParam(p, "filament") + "'";
        c.filament = *f;
    }
    if (p.count("quality") && !applyQuality(c.settings, stringParam(p, "quality"))) return "unknown quality '" + stringParam(p, "quality") + "'";

    auto number = [&](const char* key, double& target, double lo, double hi) -> std::string {
        if (!p.count(key)) return {};
        const double v = numberParam(p, key);
        if (!(v >= lo && v <= hi)) return std::string(key) + " must be between " + fmt(lo, 2) + " and " + fmt(hi, 0);
        target = v;
        return {};
    };
    auto whole = [&](const char* key, int& target, int lo, int hi) -> std::string {
        if (!p.count(key)) return {};
        const auto v = intParam(p, key);
        if (v < lo || v > hi) return std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi);
        target = static_cast<int>(v);
        return {};
    };
    Printer& pr = c.printer;
    Filament& f = c.filament;
    Settings& s = c.settings;
    const bool customBed = p.count("bed_x") || p.count("bed_y") || p.count("bed_z");
    std::string err;
    for (auto e : {number("bed_x", pr.bedX, 20, 2000), number("bed_y", pr.bedY, 20, 2000), number("bed_z", pr.bedZ, 10, 2000),
                   number("nozzle", pr.nozzle, 0.1, 2.0)}) {
        if (err.empty()) err = e;
    }
    if (!err.empty()) return err;
    if (customBed && pr.name.find("custom") == std::string::npos) pr.name = pr.name == "custom" ? "custom" : pr.name + " (custom bed)";
    if (p.count("nozzle_temp") && !p.count("first_layer_temp")) {
        if (auto e = number("nozzle_temp", f.nozzleTemp, 150, 450); !e.empty()) return e;
        f.firstLayerTemp = f.nozzleTemp + 5;
    }
    for (auto e : {number("nozzle_temp", f.nozzleTemp, 150, 450), number("first_layer_temp", f.firstLayerTemp, 150, 450),
                   number("bed_temp", f.bedTemp, 0, 150), number("fan", f.fan, 0, 100),
                   number("layer_height", s.layerHeight, 0.04, 1.2), number("first_layer_height", s.firstLayerHeight, 0.04, 1.2),
                   number("line_width", s.lineWidth, 0.1, 3.0), whole("perimeters", s.perimeters, 0, 20),
                   whole("top_layers", s.topLayers, 0, 100), whole("bottom_layers", s.bottomLayers, 0, 100),
                   number("infill", s.infill, 0, 100), number("speed", s.speed, 1, 1000),
                   number("first_layer_speed", s.firstLayerSpeed, 1, 1000), number("travel_speed", s.travelSpeed, 1, 2000),
                   number("retraction", s.retraction, 0, 20), number("retraction_speed", s.retractionSpeed, 1, 200),
                   whole("skirt", s.skirt, 0, 20), number("skirt_distance", s.skirtDistance, 0, 100), number("brim", s.brim, 0, 50),
                   number("support_angle", s.supportAngle, 0, 89), number("support_density", s.supportDensity, 1, 100)}) {
        if (err.empty()) err = e;
    }
    if (!err.empty()) return err;
    if (p.count("infill_pattern")) s.infillPattern = stringParam(p, "infill_pattern");
    if (p.count("supports")) s.supports = stringParam(p, "supports");
    if (p.count("seam")) s.seam = stringParam(p, "seam");
    if (p.count("start_gcode")) pr.startGcode = stringParam(p, "start_gcode");
    if (p.count("end_gcode")) pr.endGcode = stringParam(p, "end_gcode");
    const double lw = c.lineWidth();
    if (std::max(s.layerHeight, s.firstLayerHeight) > pr.nozzle * 0.8 + 1e-9) {
        return "layer height " + fmt(std::max(s.layerHeight, s.firstLayerHeight), 2) + " mm is too thick for a " + fmt(pr.nozzle, 2) +
               " mm nozzle (keep it under " + fmt(pr.nozzle * 0.8, 2) + ")";
    }
    if (lw < pr.nozzle * 0.6) return "line width must be at least 0.6 x the nozzle";
    return {};
}

Config defaultConfig() {
    Config c;
    c.printer = *printerPreset("ender3");
    c.filament = *filamentPreset("pla");
    applyQuality(c.settings, "normal");
    return c;
}

std::string settingsText(const Config& c) {
    const Settings& s = c.settings;
    return c.printer.name + " (" + fmt(c.printer.bedX, 0) + " x " + fmt(c.printer.bedY, 0) + " x " + fmt(c.printer.bedZ, 0) + " mm, " +
           fmt(c.printer.nozzle, 2) + " mm nozzle), " + c.filament.name + " " + fmt(c.filament.nozzleTemp, 0) + "/" +
           fmt(c.filament.bedTemp, 0) + " C, " + s.quality + ": " + fmt(s.layerHeight, 2) + " mm layers, " +
           std::to_string(s.perimeters) + " walls, " + std::to_string(s.topLayers) + " top / " + std::to_string(s.bottomLayers) +
           " bottom, " + fmt(s.infill, 0) + "% " + s.infillPattern + " infill, " + fmt(s.speed, 0) + " mm/s, supports " + s.supports +
           (s.brim > 0 ? ", brim " + fmt(s.brim, 1) + " mm" : "") + (s.skirt > 0 ? ", skirt " + std::to_string(s.skirt) : "");
}

Result& addSettings(Result& r, const Config& c) {
    const Settings& s = c.settings;
    r.with("printer", c.printer.name)
        .with("bed", NumberList{c.printer.bedX, c.printer.bedY, c.printer.bedZ})
        .with("nozzle", c.printer.nozzle)
        .with("filament", c.filament.name)
        .with("nozzle_temp", c.filament.nozzleTemp)
        .with("bed_temp", c.filament.bedTemp)
        .with("quality", s.quality)
        .with("layer_height", s.layerHeight)
        .with("first_layer_height", s.firstLayerHeight)
        .with("line_width", c.lineWidth())
        .with("perimeters", static_cast<std::int64_t>(s.perimeters))
        .with("top_layers", static_cast<std::int64_t>(s.topLayers))
        .with("bottom_layers", static_cast<std::int64_t>(s.bottomLayers))
        .with("infill", s.infill)
        .with("infill_pattern", s.infillPattern)
        .with("speed", s.speed)
        .with("supports", s.supports)
        .with("brim", s.brim)
        .with("skirt", static_cast<std::int64_t>(s.skirt))
        .with("seam", s.seam);
    return r;
}

Result& addPlacement(Result& r, const PrintState& st) {
    const mesh::Bounds b = mesh::bounds(st.mesh);
    const Fit fit = checkFit(st.mesh, st.config.printer);
    r.with("size", NumberList{b.size().x, b.size().y, b.size().z})
        .with("center", NumberList{b.center().x, b.center().y})
        .with("fits", fit.fits);
    if (!fit.fits) r.with("problem", fit.reason);
    return r;
}

Result placed(const std::string& msg, const PrintDoc& doc, const Params& p) {
    Result r = Result::success(msg);
    r.with("doc", stringParam(p, "doc"));
    addPlacement(r, doc.state);
    if (boolParam(p, "preview", false)) r.attach({"image/png", raster::encodePng(renderOnBed(doc.state, mesh::View::Iso, 512, true)), "print.png"});
    return r;
}

ActionFactory editCommand(Session& session, std::function<Result(PrintDoc&, const Params&)> fn) {
    return [&session, fn](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<PrintDoc>(id);
        if (!doc) return std::make_unique<FunctionAction>([id] { return Result::failure("no open print job called '" + id + "' (open one with slicer.load)"); });
        return std::make_unique<PrintEdit>(doc, [fn, p](PrintDoc& d) { return fn(d, p); });
    };
}

ActionFactory readCommand(Session& session, std::function<Result(PrintDoc&, const Params&)> fn) {
    return [&session, fn](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<PrintDoc>(id);
        if (!doc) return std::make_unique<FunctionAction>([id] { return Result::failure("no open print job called '" + id + "' (open one with slicer.load)"); });
        return std::make_unique<FunctionAction>([doc, fn, p] {
            try {
                return fn(*doc, p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

std::shared_ptr<const Sliced> ensureSliced(PrintDoc& doc) {
    if (doc.sliced) return doc.sliced;
    const Fit fit = checkFit(doc.state.mesh, doc.state.config.printer);
    if (!fit.fits) throw std::runtime_error("can't slice: " + fit.reason);
    auto s = std::make_shared<Sliced>();
    s->result = slice(doc.state.mesh, doc.state.config);
    s->gcode = toGcode(s->result, doc.state.config, doc.name);
    doc.sliced = s;
    return s;
}

Result& addStats(Result& r, const Sliced& s) {
    r.with("layers", static_cast<std::int64_t>(s.result.layers.size()))
        .with("time", duration(s.gcode.seconds))
        .with("seconds", std::round(s.gcode.seconds))
        .with("filament_m", std::round(s.gcode.filamentMm) / 1000.0)
        .with("filament_g", std::round(s.gcode.filamentGrams * 10) / 10.0);
    if (!s.result.warnings.empty()) {
        std::string w;
        for (const auto& x : s.result.warnings) w += (w.empty() ? "" : "; ") + x;
        r.with("warnings", w);
    }
    return r;
}

std::string statsText(const Sliced& s) {
    return std::to_string(s.result.layers.size()) + " layers, about " + duration(s.gcode.seconds) + ", " +
           fmt(s.gcode.filamentMm / 1000.0, 2) + " m / " + fmt(s.gcode.filamentGrams, 1) + " g of filament";
}

std::size_t pickLayer(const Sliced& s, const Params& p, std::size_t fallback) {
    const auto n = static_cast<std::int64_t>(s.result.layers.size());
    if (!p.count("layer")) return fallback;
    std::int64_t l = intParam(p, "layer");
    if (l < 0) l += n;
    if (l < 0 || l >= n) throw std::runtime_error("layer must be 0.." + std::to_string(n - 1) + " (or negative to count from the top)");
    return static_cast<std::size_t>(l);
}

Result layerResult(const Sliced& s, const Config& c, std::size_t layer, const Params& p, const std::string& docId) {
    const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 64, 4096));
    raster::Image img = renderLayer(s.result, layer, c, size, boolParam(p, "travel", true), stringParam(p, "frame", "part") == "bed");
    const std::string png = raster::encodePng(img);
    const Layer& L = s.result.layers[layer];
    std::map<PathType, double> lengths;
    for (const auto& path : L.paths) {
        double len = 0;
        for (std::size_t k = 1; k < path.points.size(); ++k) len += length(path.points[k] - path.points[k - 1]);
        if (path.closed && path.points.size() > 1) len += length(path.points.front() - path.points.back());
        lengths[path.type] += len;
    }
    std::string parts;
    for (const auto& [t, len] : lengths) parts += (parts.empty() ? "" : ", ") + std::string(toString(t)) + " " + fmt(len, 0) + " mm";
    Result r = Result::success("layer " + std::to_string(layer) + " of " + std::to_string(s.result.layers.size()) + " at z=" + fmt(L.z, 2) +
                               " mm: " + (parts.empty() ? "nothing to print" : parts) +
                               " (red outer wall, green inner walls, yellow solid infill, orange infill, cyan support, grey skirt/brim, blue travel)");
    r.with("doc", docId).with("layer", static_cast<std::int64_t>(layer)).with("z", L.z).with("paths", static_cast<std::int64_t>(L.paths.size()));
    const std::string out = stringParam(p, "output");
    if (!out.empty()) {
        std::ofstream f(out, std::ios::binary);
        f << png;
        if (!f) return Result::failure("couldn't write " + out);
        r.with("output", out);
    }
    r.attach({"image/png", png, "layer" + std::to_string(layer) + ".png"});
    return r;
}

bool writeFile(const std::string& path, const std::string& text) {
    std::error_code ec;
    const fs::path p(path);
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << text;
    return static_cast<bool>(f);
}

std::shared_ptr<PrintDoc> newJob(mesh::Mesh m, const std::string& name) {
    if (m.empty()) throw std::runtime_error(name + " has no triangles to print");
    auto doc = std::make_shared<PrintDoc>();
    doc->state.mesh = std::move(m);
    doc->state.config = defaultConfig();
    doc->name = name;
    return doc;
}

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    std::vector<ParamSpec> loadParams = {
        {"path", ParamType::String, "model file: stl, obj, ply, off, 3mf, glb or gltf", false},
        {"mesh", ParamType::String, "or the id of a mesh already open in this session (e.g. mesh1 from cad)", false},
        {"lay_flat", ParamType::Bool, "turn the largest flat face down first", false, {}, false},
        num("scale", "scale factor, e.g. 25.4 for a model drawn in inches"),
        previewParam()};
    for (auto& sp : settingsParams()) loadParams.push_back(sp);

    reg.add({"slicer.load", "Load model",
             "Start a print job from a model file (any mesh format) or an open mesh document, placed in the middle of the bed "
             "and dropped onto it. Returns a print job id like print1. Defaults: ender3 printer, PLA, normal quality; pass any "
             "slicer.settings parameter to change them now.",
             loadParams, false, true},
            [s](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([s, p] {
                    try {
                        mesh::Mesh m;
                        std::string name;
                        if (p.count("mesh")) {
                            const std::string id = stringParam(p, "mesh");
                            auto md = s->workspace().getAs<mesh::MeshDoc>(id);
                            if (!md) return Result::failure("no open mesh called '" + id + "'");
                            m = md->mesh;
                            name = id;
                        } else if (p.count("path")) {
                            const std::string path = stringParam(p, "path");
                            m = mesh::load(path);
                            name = fs::path(path).filename().string();
                        } else {
                            return Result::failure("pass path=model.stl or mesh=<open mesh id>");
                        }
                        auto doc = newJob(std::move(m), name);
                        if (p.count("path")) doc->path = stringParam(p, "path");
                        if (auto err = applySettings(doc->state.config, p); !err.empty()) return Result::failure(err);
                        const double sc = numberParam(p, "scale", 1.0);
                        if (!(sc > 0)) return Result::failure("scale must be positive");
                        if (sc != 1.0) mesh::transform(doc->state.mesh, mesh::Mat4::scale(sc));
                        if (boolParam(p, "lay_flat", false)) layFlat(doc->state.mesh);
                        centerOnBed(doc->state.mesh, doc->state.config.printer);
                        const std::string id = s->workspace().add(doc, "print");
                        Params q = p;
                        q["doc"] = id;
                        Result r = placed("loaded " + name + " as " + id + " (" + std::to_string(doc->state.mesh.triangleCount()) +
                                              " triangles) on the " + doc->state.config.printer.name,
                                          *doc, q);
                        const Fit fit = checkFit(doc->state.mesh, doc->state.config.printer);
                        if (!fit.fits) r.message += "; warning: " + fit.reason;
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"slicer.close", "Close print job", "Close a print job (undo reopens it).", {docParam()}, true},
            [s](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!s->workspace().getAs<PrintDoc>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open print job called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(*s, id);
            });

    reg.add({"slicer.place", "Place on bed",
             "Drop the model onto the bed (lowest point at z=0) and centre it, or put its centre at x, y.",
             {docParam(), num("x", "centre x on the bed in mm (default: bed centre)"), num("y", "centre y in mm"), previewParam()},
             true},
            editCommand(session, [](PrintDoc& d, const Params& p) {
                const Printer& pr = d.state.config.printer;
                moveCenterTo(d.state.mesh, numberParam(p, "x", pr.bedX / 2), numberParam(p, "y", pr.bedY / 2));
                return placed("placed on the bed", d, p);
            }));

    reg.add({"slicer.move", "Move model",
             "Slide the model across the bed by x, y mm (or to centre x, y with absolute=true). It always stays on the bed surface.",
             {docParam(), num("x", "mm along x"), num("y", "mm along y"),
              {"absolute", ParamType::Bool, "x, y are the new centre instead of an offset", false, {}, false}, previewParam()},
             true},
            editCommand(session, [](PrintDoc& d, const Params& p) {
                const mesh::Vec3 c = mesh::bounds(d.state.mesh).center();
                if (boolParam(p, "absolute", false)) moveCenterTo(d.state.mesh, numberParam(p, "x", c.x), numberParam(p, "y", c.y));
                else moveCenterTo(d.state.mesh, c.x + numberParam(p, "x", 0), c.y + numberParam(p, "y", 0));
                return placed("moved", d, p);
            }));

    reg.add({"slicer.rotate", "Rotate model",
             "Rotate the model about its centre by x, then y, then z degrees, then drop it back onto the bed in the same spot.",
             {docParam(), num("x", "degrees about x"), num("y", "degrees about y"), num("z", "degrees about z (spins it on the bed)"),
              previewParam()},
             true},
            editCommand(session, [](PrintDoc& d, const Params& p) {
                const mesh::Vec3 c = mesh::bounds(d.state.mesh).center();
                const mesh::Vec3 deg{numberParam(p, "x", 0), numberParam(p, "y", 0), numberParam(p, "z", 0)};
                mesh::transform(d.state.mesh, mesh::Mat4::translate(c) * mesh::Mat4::rotate(deg) * mesh::Mat4::translate(-c));
                moveCenterTo(d.state.mesh, c.x, c.y);
                return placed("rotated", d, p);
            }));

    reg.add({"slicer.scale", "Scale model",
             "Scale the model about its centre: factor for all axes (e.g. 2, or 0.5), or x, y, z factors. It stays on the bed in the same spot.",
             {docParam(), num("factor", "uniform scale factor"), num("x", "x factor"), num("y", "y factor"), num("z", "z factor"), previewParam()},
             true},
            editCommand(session, [](PrintDoc& d, const Params& p) {
                const double f = numberParam(p, "factor", 1.0);
                const mesh::Vec3 k{numberParam(p, "x", 1.0) * f, numberParam(p, "y", 1.0) * f, numberParam(p, "z", 1.0) * f};
                if (!(k.x > 0 && k.y > 0 && k.z > 0)) return Result::failure("scale factors must be positive");
                const mesh::Vec3 c = mesh::bounds(d.state.mesh).center();
                mesh::transform(d.state.mesh, mesh::Mat4::translate(c) * mesh::Mat4::scale(k) * mesh::Mat4::translate(-c));
                moveCenterTo(d.state.mesh, c.x, c.y);
                return placed("scaled", d, p);
            }));

    reg.add({"slicer.lay_flat", "Lay flat",
             "Turn the model so its largest flat outside face lies on the bed (good for parts exported standing up or tilted).",
             {docParam(), previewParam()}, true},
            editCommand(session, [](PrintDoc& d, const Params& p) {
                const mesh::Vec3 c = mesh::bounds(d.state.mesh).center();
                const bool changed = layFlat(d.state.mesh);
                moveCenterTo(d.state.mesh, c.x, c.y);
                return placed(changed ? "laid flat on its largest face" : "already lying on its largest flat face", d, p);
            }));

    std::vector<ParamSpec> setParams = {docParam()};
    for (auto& sp : settingsParams()) setParams.push_back(sp);
    reg.add({"slicer.settings", "Print settings",
             "Change the printer, filament, quality and any print setting. Presets apply first (printer, filament, quality), then "
             "the individual values, so `quality=fine infill=30` is fine with 30% infill. With no settings, shows the current ones.",
             setParams, true},
            editCommand(session, [](PrintDoc& d, const Params& p) {
                Config c = d.state.config;
                if (auto err = applySettings(c, p); !err.empty()) return Result::failure(err);
                const bool printerChanged = c.printer.bedX != d.state.config.printer.bedX || c.printer.bedY != d.state.config.printer.bedY;
                d.state.config = c;
                if (printerChanged) centerOnBed(d.state.mesh, c.printer);
                Result r = Result::success(settingsText(c));
                r.with("doc", stringParam(p, "doc"));
                addSettings(r, c);
                const Fit fit = checkFit(d.state.mesh, c.printer);
                r.with("fits", fit.fits);
                if (!fit.fits) r.with("problem", fit.reason);
                return r;
            }));

    reg.add({"slicer.info", "Print job info",
             "Describe a print job: model size and position, whether it fits the printer, its volume and the filament it would "
             "take solid, the settings, and the slice estimates once sliced.",
             {docParam()}, false, true},
            readCommand(session, [](PrintDoc& d, const Params& p) {
                const double vol = std::abs(mesh::volume(d.state.mesh));
                const Config& c = d.state.config;
                const Fit fit = checkFit(d.state.mesh, c.printer);
                const mesh::Vec3 sz = mesh::bounds(d.state.mesh).size();
                std::string msg = d.name + ": " + fmt(sz.x) + " x " + fmt(sz.y) + " x " + fmt(sz.z) + " mm, " + fmt(vol / 1000.0, 2) +
                                  " cm3 (" + fmt(vol / 1000.0 * c.filament.density, 1) + " g if printed solid in " + c.filament.name + "); " +
                                  (fit.fits ? "fits the " + c.printer.name : "DOESN'T FIT: " + fit.reason) + "\n" + settingsText(c);
                if (d.sliced) msg += "\nsliced: " + statsText(*d.sliced);
                Result r = Result::success(msg);
                r.with("doc", stringParam(p, "doc")).with("model", d.name);
                addPlacement(r, d.state);
                r.with("volume_cm3", std::round(vol / 10.0) / 100.0)
                    .with("solid_grams", std::round(vol / 1000.0 * c.filament.density * 10) / 10.0)
                    .with("triangles", static_cast<std::int64_t>(d.state.mesh.triangleCount()))
                    .with("watertight", mesh::isWatertight(d.state.mesh));
                addSettings(r, c);
                if (d.sliced) addStats(r, *d.sliced);
                return r;
            }));

    reg.add({"slicer.presets", "List presets",
             "List the printer, filament and quality presets with their values.", {}, false, true},
            [](const Params&) {
                return std::make_unique<FunctionAction>([] {
                    std::string t = "printers:";
                    for (const auto& n : printerNames()) {
                        auto p = *printerPreset(n);
                        t += "\n  " + n + ": " + fmt(p.bedX, 0) + " x " + fmt(p.bedY, 0) + " x " + fmt(p.bedZ, 0) + " mm, " + fmt(p.nozzle, 2) +
                             " mm nozzle" + (n == "custom" ? " (set bed_x, bed_y, bed_z, nozzle)" : "");
                    }
                    t += "\nfilaments:";
                    for (const auto& n : filamentNames()) {
                        auto f = *filamentPreset(n);
                        t += "\n  " + n + ": nozzle " + fmt(f.nozzleTemp, 0) + " C (first layer " + fmt(f.firstLayerTemp, 0) + "), bed " +
                             fmt(f.bedTemp, 0) + " C, fan " + fmt(f.fan, 0) + "%, " + fmt(f.density, 2) + " g/cm3" +
                             (f.maxSpeed > 0 ? ", max " + fmt(f.maxSpeed, 0) + " mm/s" : "");
                    }
                    t += "\nquality:";
                    for (const auto& n : qualityNames()) {
                        Settings st;
                        applyQuality(st, n);
                        t += "\n  " + n + ": " + fmt(st.layerHeight, 2) + " mm layers (first " + fmt(st.firstLayerHeight, 2) + "), " +
                             std::to_string(st.perimeters) + " walls, " + std::to_string(st.topLayers) + " top / " +
                             std::to_string(st.bottomLayers) + " bottom layers, " + fmt(st.speed, 0) + " mm/s";
                    }
                    return Result::success(t);
                });
            });

    reg.add({"slicer.slice", "Slice",
             "Slice the print job into layers and toolpaths and estimate print time and filament. Fails if the model doesn't fit "
             "the printer. preview=true attaches a picture of the middle layer (or the one given by layer).",
             {docParam(), previewParam(), integer("layer", "layer to picture with preview (0 = first, -1 = last)")}, false, true},
            readCommand(session, [](PrintDoc& d, const Params& p) {
                auto sl = ensureSliced(d);
                Result r = Result::success("sliced " + d.name + ": " + statsText(*sl));
                r.with("doc", stringParam(p, "doc"));
                addStats(r, *sl);
                if (boolParam(p, "preview", false)) {
                    const std::size_t l = pickLayer(*sl, p, sl->result.layers.size() / 2);
                    Result img = layerResult(*sl, d.state.config, l, Params{}, stringParam(p, "doc"));
                    for (auto& a : img.attachments) r.attach(std::move(a));
                }
                return r;
            }));

    reg.add({"slicer.export_gcode", "Export G-code",
             "Write the G-code for the print job (slicing first if needed). Marlin flavour with absolute extrusion, "
             ";LAYER:n markers and time and filament estimates in the header.",
             {docParam(), {"path", ParamType::String, "file to write, e.g. part.gcode", true}}, false},
            readCommand(session, [](PrintDoc& d, const Params& p) {
                auto sl = ensureSliced(d);
                const std::string path = stringParam(p, "path");
                if (!writeFile(path, sl->gcode.text)) return Result::failure("couldn't write " + path);
                Result r = Result::success("wrote " + path + ": " + statsText(*sl));
                r.with("doc", stringParam(p, "doc")).with("output", path).with("bytes", static_cast<std::int64_t>(sl->gcode.text.size()));
                addStats(r, *sl);
                return r;
            }));

    reg.add({"slicer.preview_layer", "Preview layer",
             "Draw one sliced layer from above as a PNG: red outer wall, green inner walls, yellow solid infill, orange sparse "
             "infill, cyan support, grey skirt/brim and thin blue travel moves. Slices first if needed.",
             {docParam(), integer("layer", "layer number, 0 = first, -1 = last (default: middle)"),
              {"size", ParamType::Int, "image size in px", false, {}, std::int64_t{512}},
              {"travel", ParamType::Bool, "draw travel moves", false, {}, true},
              {"frame", ParamType::String, "zoom to the part or show the whole bed", false, {"part", "bed"}, std::string("part")},
              {"output", ParamType::String, "also save the PNG to this file", false}},
             false},
            readCommand(session, [](PrintDoc& d, const Params& p) {
                auto sl = ensureSliced(d);
                return layerResult(*sl, d.state.config, pickLayer(*sl, p, sl->result.layers.size() / 2), p, stringParam(p, "doc"));
            }));

    reg.add({"slicer.preview", "Preview on bed",
             "Render the placed model on the printer's bed as a PNG.",
             {docParam(), mesh::viewParam(), {"size", ParamType::Int, "image size in px", false, {}, std::int64_t{512}},
              {"output", ParamType::String, "also save the PNG to this file", false}},
             false},
            readCommand(session, [](PrintDoc& d, const Params& p) {
                auto view = mesh::parseView(stringParam(p, "view", "iso"));
                if (!view) return Result::failure("unknown view");
                const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 512), 64, 4096));
                const std::string png = raster::encodePng(renderOnBed(d.state, *view, size, true));
                Result r = Result::success("preview of " + d.name + " on the " + d.state.config.printer.name);
                r.with("doc", stringParam(p, "doc"));
                const std::string out = stringParam(p, "output");
                if (!out.empty()) {
                    if (!writeFile(out, png)) return Result::failure("couldn't write " + out);
                    r.with("output", out);
                }
                r.attach({"image/png", png, "print.png"});
                return r;
            }));

    std::vector<ParamSpec> oneShot = {
        {"input", ParamType::String, "model file: stl, obj, ply, off, 3mf, glb or gltf", true},
        {"output", ParamType::String, "G-code file to write (default: input with .gcode)", false},
        {"lay_flat", ParamType::Bool, "turn the largest flat face down first", false, {}, false},
        num("scale", "scale factor"),
        num("rotate_z", "spin the model on the bed by this many degrees"),
        previewParam()};
    for (auto& sp : settingsParams()) oneShot.push_back(sp);
    reg.add({"slicer.slice_file", "Slice file",
             "One step from model to G-code: load the model, centre it on the bed, apply the presets and settings, slice and "
             "write the G-code. Fails if the model doesn't fit the printer.",
             oneShot, false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    try {
                        const std::string input = stringParam(p, "input");
                        std::string output = stringParam(p, "output");
                        if (output.empty()) output = fs::path(input).replace_extension(".gcode").string();
                        auto doc = newJob(mesh::load(input), fs::path(input).filename().string());
                        if (auto err = applySettings(doc->state.config, p); !err.empty()) return Result::failure(err);
                        const double sc = numberParam(p, "scale", 1.0);
                        if (!(sc > 0)) return Result::failure("scale must be positive");
                        if (sc != 1.0) mesh::transform(doc->state.mesh, mesh::Mat4::scale(sc));
                        if (boolParam(p, "lay_flat", false)) layFlat(doc->state.mesh);
                        if (p.count("rotate_z")) mesh::transform(doc->state.mesh, mesh::Mat4::rotateZ(numberParam(p, "rotate_z")));
                        centerOnBed(doc->state.mesh, doc->state.config.printer);
                        auto sl = ensureSliced(*doc);
                        if (!writeFile(output, sl->gcode.text)) return Result::failure("couldn't write " + output);
                        Result r = Result::success("sliced " + doc->name + " for the " + doc->state.config.printer.name + " in " +
                                                   doc->state.config.filament.name + ": " + statsText(*sl) + "; wrote " + output);
                        r.with("output", output);
                        addStats(r, *sl);
                        if (boolParam(p, "preview", false)) {
                            Result img = layerResult(*sl, doc->state.config, sl->result.layers.size() / 2, Params{}, "");
                            for (auto& a : img.attachments) r.attach(std::move(a));
                        }
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

}  // namespace wetwaresuite::slicer
