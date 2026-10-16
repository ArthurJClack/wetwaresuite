#include "achclwss/paint/paint.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "achclwss/raster/document.hpp"
#include "achclwss/raster/paint.hpp"

namespace achclwss::paint {

using namespace raster;

namespace {

struct Preset {
    double size, hardness, flow, spacing;
    bool pressureSize, pressureOpacity;
};

// Starting points that feel like the tools people know; explicit params override them.
const std::map<std::string, Preset>& presets() {
    static const std::map<std::string, Preset> p = {
        {"pen", {6, 95, 100, 10, true, false}},       {"pencil", {3, 100, 80, 8, false, true}},
        {"marker", {18, 70, 60, 10, false, false}},   {"airbrush", {60, 0, 8, 8, false, true}},
        {"ink", {8, 98, 100, 5, true, true}},         {"round", {20, 80, 100, 15, true, false}},
        {"soft", {40, 20, 40, 10, true, true}},
    };
    return p;
}

std::vector<std::string> presetNames() {
    std::vector<std::string> names;
    for (const auto& [n, _] : presets()) names.push_back(n);
    return names;
}

std::string idOf(const Params& p) { return stringParam(p, "doc"); }

Result done(const std::string& message, ImageDoc& doc, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", idOf(p)).with("layer", static_cast<std::int64_t>(doc.active))
        .with("layers", static_cast<std::int64_t>(doc.image.layers.size()));
    return maybePreview(r, doc, p);
}

double percent(const Params& p, const std::string& key, double fallback) {
    return std::clamp(numberParam(p, key, fallback), 0.0, 100.0) / 100.0;
}

// Reads x,y pairs plus optional per-point pressures (0..1).
bool strokePoints(const Params& p, std::vector<StrokePoint>& out, std::string& error) {
    NumberList xy = listParam(p, "points");
    if (xy.size() < 2 || xy.size() % 2) {
        error = "points must be x,y pairs, e.g. [10,10, 50,40, 90,10]";
        return false;
    }
    NumberList pressures = listParam(p, "pressures");
    if (!pressures.empty() && pressures.size() != xy.size() / 2) {
        error = "pressures needs one value (0..1) per point: got " + std::to_string(pressures.size()) + " for " +
                std::to_string(xy.size() / 2) + " points";
        return false;
    }
    for (std::size_t i = 0; i < xy.size(); i += 2) {
        double pr = pressures.empty() ? 1.0 : std::clamp(pressures[i / 2], 0.0, 1.0);
        out.push_back({xy[i], xy[i + 1], pr});
    }
    return true;
}

std::vector<ParamSpec> brushParams(bool erase) {
    std::vector<ParamSpec> v = {
        docParam(),
        {"points", ParamType::NumberList, "stroke path as x,y pairs in px, e.g. [10,10, 50,40, 90,10]", true},
        {"pressures", ParamType::NumberList, "optional pen pressure per point, 0..1 (from a tablet)", false},
        {"brush", ParamType::String, "brush preset", false, presetNames(), std::string(erase ? "round" : "round")},
        {"size", ParamType::Number, "brush diameter in px (overrides the preset)", false},
        {"hardness", ParamType::Number, "edge hardness 0..100 (overrides the preset)", false},
        {"opacity", ParamType::Number, "stroke opacity 0..100", false, {}, 100.0},
        {"flow", ParamType::Number, "paint per dab 0..100 (overrides the preset)", false},
        {"spacing", ParamType::Number, "dab spacing as % of size (overrides the preset)", false},
        {"smooth", ParamType::Number, "stabilizer for shaky mouse input, 0..90", false, {}, 0.0},
    };
    if (!erase) v.insert(v.begin() + 3, colorParam("color", "paint colour", false, "#000000"));
    v.push_back(layerParam());
    v.push_back(previewParam());
    return v;
}

Result paintStroke(ImageDoc& doc, const Params& p, bool erase) {
    std::string error;
    int idx = layerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    std::vector<StrokePoint> pts;
    if (!strokePoints(p, pts, error)) return Result::failure(error);

    const Preset& pre = presets().at(stringParam(p, "brush", "round"));
    Brush b;
    if (!erase && !colorArg(p, "color", "#000000", b.color, error)) return Result::failure(error);
    b.size = std::max(0.5, numberParam(p, "size", pre.size));
    b.hardness = percent(p, "hardness", pre.hardness);
    b.flow = percent(p, "flow", pre.flow);
    b.spacing = std::max(0.02, numberParam(p, "spacing", pre.spacing) / 100.0);
    b.opacity = percent(p, "opacity", 100);
    b.pressureSize = pre.pressureSize;
    b.pressureOpacity = pre.pressureOpacity;
    b.erase = erase;
    pts = smooth(pts, std::clamp(numberParam(p, "smooth", 0), 0.0, 90.0) / 100.0);
    doc.active = idx;
    stroke(doc.image.layers[static_cast<std::size_t>(idx)].image, b, pts);
    return done(std::string(erase ? "erased" : "painted") + " a stroke of " + std::to_string(pts.size()) + " points", doc, p);
}

Image& targetLayer(ImageDoc& doc, int idx) { return doc.image.layers[static_cast<std::size_t>(idx)].image; }

void linearOrRadialGradient(Image& img, double x1, double y1, double x2, double y2, Color a, Color b, bool radial) {
    const double dx = x2 - x1, dy = y2 - y1;
    const double len2 = dx * dx + dy * dy;
    const double radius = std::sqrt(len2);
    Image layer(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            double t;
            if (radial) t = radius > 0 ? std::hypot(x + 0.5 - x1, y + 0.5 - y1) / radius : 1;
            else t = len2 > 0 ? ((x + 0.5 - x1) * dx + (y + 0.5 - y1) * dy) / len2 : 1;
            t = std::clamp(t, 0.0, 1.0);
            auto lerp = [&](std::uint8_t u, std::uint8_t v) { return static_cast<std::uint8_t>(u + (v - u) * t + 0.5); };
            layer.set(x, y, {lerp(a.r, b.r), lerp(a.g, b.g), lerp(a.b, b.b), lerp(a.a, b.a)});
        }
    }
    blendOnto(img, layer, 0, 0, Blend::Normal, 1.0);
}

}  // namespace

void registerCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"paint.new",
             "New canvas",
             "Create a blank painting canvas with one background layer and an empty paint layer on top, ready to draw.",
             {{"width", ParamType::Int, "width in px", true},
              {"height", ParamType::Int, "height in px", true},
              colorParam("background", "background colour (use transparent for none)", false, "#ffffff")},
             false},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    auto w = intParam(p, "width"), h = intParam(p, "height");
                    if (w < 1 || h < 1 || w > 30000 || h > 30000) return Result::failure("width and height must be 1..30000");
                    Color bg;
                    std::string error;
                    if (!colorArg(p, "background", "#ffffff", bg, error)) return Result::failure(error);
                    auto doc = newImage(static_cast<int>(w), static_cast<int>(h), bg);
                    doc->image.layers.push_back({"Layer 1", Image(static_cast<int>(w), static_cast<int>(h))});
                    doc->active = 1;
                    std::string id = s->workspace().add(doc, "img");
                    return Result::success("created " + id + " with layers Background and Layer 1")
                        .with("doc", id).with("width", w).with("height", h).with("layer", std::int64_t{1});
                });
            });

    reg.add({"paint.stroke",
             "Brush stroke",
             "Paint a brush stroke through a list of points. Use pressures for tablet-style strokes and smooth to "
             "steady mouse input. Presets: pen, pencil, ink, marker, airbrush, soft, round.",
             brushParams(false), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) { return paintStroke(doc, p, false); }));

    reg.add({"paint.erase",
             "Eraser stroke",
             "Erase along a list of points (makes the layer transparent there).",
             brushParams(true), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) { return paintStroke(doc, p, true); }));

    reg.add({"paint.fill",
             "Bucket fill",
             "Flood fill the area around a point with a colour, like a paint bucket.",
             {docParam(),
              {"x", ParamType::Int, "x in px", true},
              {"y", ParamType::Int, "y in px", true},
              colorParam("color", "fill colour", true, ""),
              {"tolerance", ParamType::Int, "how different a colour can be and still fill, 0..255", false, {}, std::int64_t{32}},
              {"contiguous", ParamType::Bool, "only fill connected pixels (false = every matching pixel)", false, {}, true},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "", c, error)) return Result::failure(error);
                int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                if (x < 0 || y < 0 || x >= doc.image.width || y >= doc.image.height) return Result::failure("point is outside the image");
                auto n = floodFill(targetLayer(doc, idx), x, y, c, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "tolerance", 32), 0, 255)),
                                   boolParam(p, "contiguous", true));
                Result r = done("filled " + std::to_string(n) + " pixels", doc, p);
                return r.with("pixels", static_cast<std::int64_t>(n));
            }));

    reg.add({"paint.shape",
             "Draw shape",
             "Draw a rectangle, ellipse or line inside the box x,y,width,height (for a line: from x,y to x+width,y+height). "
             "filled=false draws just the outline.",
             {docParam(),
              {"kind", ParamType::String, "shape", true, {"rect", "ellipse", "line"}},
              {"x", ParamType::Number, "left (or line start x)", true},
              {"y", ParamType::Number, "top (or line start y)", true},
              {"width", ParamType::Number, "box width (or line dx)", true},
              {"height", ParamType::Number, "box height (or line dy)", true},
              colorParam("color", "colour", false, "#000000"),
              {"filled", ParamType::Bool, "fill the shape (ignored for lines)", false, {}, true},
              {"line_width", ParamType::Number, "outline or line width in px", false, {}, 2.0},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "#000000", c, error)) return Result::failure(error);
                const std::string kind = stringParam(p, "kind");
                double x = numberParam(p, "x"), y = numberParam(p, "y"), w = numberParam(p, "width"), h = numberParam(p, "height");
                double lw = std::max(0.5, numberParam(p, "line_width", 2));
                bool filled = boolParam(p, "filled", true);
                Image& img = targetLayer(doc, idx);
                if (kind == "line") {
                    drawLine(img, x, y, x + w, y + h, c, lw);
                } else {
                    if (w < 0) {
                        x += w;
                        w = -w;
                    }
                    if (h < 0) {
                        y += h;
                        h = -h;
                    }
                    if (kind == "rect") {
                        filled ? fillRect(img, x, y, w, h, c) : strokeRect(img, x, y, w, h, c, lw);
                    } else {
                        filled ? fillEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, c)
                               : strokeEllipse(img, x + w / 2, y + h / 2, w / 2, h / 2, c, lw);
                    }
                }
                doc.active = idx;
                return done("drew a " + kind, doc, p);
            }));

    reg.add({"paint.polygon",
             "Draw polygon",
             "Fill a polygon through the given points (x,y pairs).",
             {docParam(), {"points", ParamType::NumberList, "corners as x,y pairs, at least 3", true},
              colorParam("color", "fill colour", false, "#000000"), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "#000000", c, error)) return Result::failure(error);
                NumberList xy = listParam(p, "points");
                if (xy.size() < 6 || xy.size() % 2) return Result::failure("points must be at least 3 x,y pairs");
                std::vector<Point> pts;
                for (std::size_t i = 0; i < xy.size(); i += 2) pts.push_back({xy[i], xy[i + 1]});
                fillPolygon(targetLayer(doc, idx), pts, c);
                return done("drew a polygon with " + std::to_string(pts.size()) + " corners", doc, p);
            }));

    reg.add({"paint.gradient",
             "Gradient",
             "Paint a linear or radial gradient from one colour to another across the layer.",
             {docParam(),
              {"x1", ParamType::Number, "start x (radial: centre x)", true},
              {"y1", ParamType::Number, "start y (radial: centre y)", true},
              {"x2", ParamType::Number, "end x (radial: edge x)", true},
              {"y2", ParamType::Number, "end y (radial: edge y)", true},
              colorParam("from", "start colour", false, "#000000"),
              colorParam("to", "end colour", false, "#ffffff"),
              {"kind", ParamType::String, "gradient shape", false, {"linear", "radial"}, std::string("linear")},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color a, b;
                if (!colorArg(p, "from", "#000000", a, error) || !colorArg(p, "to", "#ffffff", b, error)) return Result::failure(error);
                linearOrRadialGradient(targetLayer(doc, idx), numberParam(p, "x1"), numberParam(p, "y1"), numberParam(p, "x2"),
                                       numberParam(p, "y2"), a, b, stringParam(p, "kind", "linear") == "radial");
                return done("painted a gradient", doc, p);
            }));

    reg.add({"paint.clear",
             "Clear layer",
             "Wipe a layer to transparent, or fill it entirely with a colour.",
             {docParam(), colorParam("color", "colour to fill with", false, "transparent"), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Color c;
                if (!colorArg(p, "color", "transparent", c, error)) return Result::failure(error);
                targetLayer(doc, idx).fill(c);
                return done("cleared layer " + std::to_string(idx), doc, p);
            }));

    // ---- layers ----

    reg.add({"paint.layer_add",
             "Add layer",
             "Add a new transparent layer just above the active one and make it active.",
             {docParam(), {"name", ParamType::String, "layer name", false}, colorParam("fill", "start filled with this colour", false, "")},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string name = stringParam(p, "name", "Layer " + std::to_string(doc.image.layers.size()));
                Color c{0, 0, 0, 0};
                std::string error;
                if (p.count("fill") && !colorArg(p, "fill", "", c, error)) return Result::failure(error);
                auto pos = doc.image.layers.begin() + doc.active + 1;
                doc.image.layers.insert(pos, {name, Image(doc.image.width, doc.image.height, c)});
                doc.active += 1;
                return done("added layer \"" + name + "\" at index " + std::to_string(doc.active), doc, p);
            }));

    reg.add({"paint.layer_delete", "Delete layer", "Delete a layer (an image always keeps at least one).",
             {docParam(), layerParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                if (doc.image.layers.size() == 1) return Result::failure("can't delete the only layer");
                std::string name = doc.image.layers[static_cast<std::size_t>(idx)].name;
                doc.image.layers.erase(doc.image.layers.begin() + idx);
                doc.active = std::clamp(doc.active > idx ? doc.active - 1 : doc.active, 0, static_cast<int>(doc.image.layers.size()) - 1);
                return done("deleted layer \"" + name + "\"", doc, p);
            }));

    reg.add({"paint.layer_set",
             "Layer properties",
             "Rename a layer or change its opacity (0..100), blend mode or visibility.",
             {docParam(), layerParam(),
              {"name", ParamType::String, "new name", false},
              {"opacity", ParamType::Number, "opacity 0..100", false},
              {"blend", ParamType::String, "blend mode", false, blendNames()},
              {"visible", ParamType::Bool, "show or hide the layer", false},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                auto& l = doc.image.layers[static_cast<std::size_t>(idx)];
                if (p.count("name")) l.name = stringParam(p, "name");
                if (p.count("opacity")) l.opacity = percent(p, "opacity", 100);
                if (p.count("blend")) l.blend = *parseBlend(stringParam(p, "blend"));
                if (p.count("visible")) l.visible = boolParam(p, "visible", true);
                return done("updated layer " + std::to_string(idx) + " \"" + l.name + "\"", doc, p);
            }));

    reg.add({"paint.layer_select", "Select layer", "Make a layer the active one; brush and fill commands go there by default.",
             {docParam(), {"layer", ParamType::Int, "layer index, 0 = bottom", true}}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                doc.active = idx;
                return done("layer " + std::to_string(idx) + " \"" + doc.activeLayer().name + "\" is active", doc, p);
            }));

    reg.add({"paint.layer_move", "Move layer", "Move a layer to a new position in the stack (0 = bottom).",
             {docParam(), layerParam(), {"to", ParamType::Int, "new index", true}, previewParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                int to = static_cast<int>(intParam(p, "to"));
                if (to < 0 || to >= static_cast<int>(doc.image.layers.size())) return Result::failure("'to' is outside the layer stack");
                Layer moving = std::move(doc.image.layers[static_cast<std::size_t>(idx)]);
                doc.image.layers.erase(doc.image.layers.begin() + idx);
                doc.image.layers.insert(doc.image.layers.begin() + to, std::move(moving));
                doc.active = to;
                return done("moved layer to index " + std::to_string(to), doc, p);
            }));

    reg.add({"paint.layer_duplicate", "Duplicate layer", "Copy a layer and put the copy just above it.",
             {docParam(), layerParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layer copy = doc.image.layers[static_cast<std::size_t>(idx)];
                copy.name += " copy";
                doc.image.layers.insert(doc.image.layers.begin() + idx + 1, std::move(copy));
                doc.active = idx + 1;
                return done("duplicated layer " + std::to_string(idx), doc, p);
            }));

    reg.add({"paint.layer_merge_down", "Merge down", "Merge a layer into the one below it.",
             {docParam(), layerParam(), previewParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                if (idx == 0) return Result::failure("the bottom layer has nothing below it");
                auto& top = doc.image.layers[static_cast<std::size_t>(idx)];
                auto& below = doc.image.layers[static_cast<std::size_t>(idx - 1)];
                if (top.visible) blendOnto(below.image, top.image, 0, 0, top.blend, top.opacity);
                doc.image.layers.erase(doc.image.layers.begin() + idx);
                doc.active = idx - 1;
                return done("merged into layer " + std::to_string(idx - 1), doc, p);
            }));

    reg.add({"paint.flatten", "Flatten", "Merge every visible layer into a single layer.", {docParam(), previewParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                Image flat = doc.flatten();
                doc.image.layers.clear();
                doc.image.layers.push_back({"Background", std::move(flat)});
                doc.active = 0;
                return done("flattened to one layer", doc, p);
            }));
}

}  // namespace achclwss::paint
