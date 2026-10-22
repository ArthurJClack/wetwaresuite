// Brush commands: paint and erase strokes, the retouching brushes (clone stamp,
// dodge/burn/sponge, smudge, blur/sharpen, colour replacement), pattern fill and
// gradients. Brushes share one set of settings (preset, size, hardness, flow,
// spacing, scatter, tip, texture, symmetry) and record only the area they touch.
#include <algorithm>
#include <cmath>
#include <map>

#include "common.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "wetwaresuite/raster/retouch.hpp"
#include "wetwaresuite/raster/selection.hpp"

namespace wetwaresuite::paint {

using namespace raster;

namespace {

struct Preset {
    double size, hardness, flow, spacing;
    bool pressureSize, pressureOpacity;
    double scatter = 0;
    const char* texture = "";
};

// Starting points that feel like the tools people know; explicit params override them.
const std::map<std::string, Preset>& presets() {
    static const std::map<std::string, Preset> p = {
        {"pen", {6, 95, 100, 10, true, false}},        {"pencil", {3, 100, 80, 8, false, true}},
        {"marker", {18, 70, 60, 10, false, false}},    {"airbrush", {60, 0, 8, 8, false, true}},
        {"ink", {8, 98, 100, 5, true, true}},          {"round", {20, 80, 100, 15, true, false}},
        {"soft", {40, 20, 40, 10, true, true}},        {"chalk", {24, 90, 70, 12, false, true, 0, "noise"}},
        {"spray", {12, 60, 50, 25, false, false, 1.5}}, {"square", {16, 100, 100, 10, false, false}},
    };
    return p;
}

std::vector<std::string> presetNames() {
    std::vector<std::string> names;
    for (const auto& [n, _] : presets()) names.push_back(n);
    return names;
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

// A tip or texture: "layer:N" (that layer's pixels), a built-in pattern name, or an image file.
std::shared_ptr<const Mask> loadShape(const ImageDoc& doc, const std::string& what, bool pattern, double scale, std::string& error) {
    if (what.rfind("layer:", 0) == 0) {
        const int n = std::atoi(what.c_str() + 6);
        if (n < 0 || n >= static_cast<int>(doc.image.layers.size()) || doc.image.layers[static_cast<std::size_t>(n)].image.empty()) {
            error = "'" + what + "': no pixel layer " + std::to_string(n);
            return nullptr;
        }
        const Image& img = doc.image.layers[static_cast<std::size_t>(n)].image;
        const PixelRect b = bounds(alphaMask(img));
        if (b.empty()) {
            error = "layer " + std::to_string(n) + " is empty";
            return nullptr;
        }
        // a tip is the layer's painted area; its alpha (and brightness) is the shape
        Image part = crop(img, b.x0, b.y0, b.width(), b.height());
        auto m = std::make_shared<Mask>(part.width(), part.height());
        for (std::size_t i = 0; i < m->v.size(); ++i) m->v[i] = part.data()[i * 4 + 3];
        return m;
    }
    if (pattern) {
        const auto& names = patternNames();
        if (std::find(names.begin(), names.end(), what) != names.end()) {
            const int size = std::clamp(static_cast<int>(std::lround(16 * scale)), 2, 512);
            return maskFromImage(builtinPattern(what, size, {255, 255, 255, 255}, {90, 90, 90, 255}));
        }
    }
    if (what == "square") return std::make_shared<Mask>(16, 16, 255);
    if (what == "star" || what == "leaf" || what == "splatter") {
        Image img(64, 64);
        std::vector<Point> pts;
        if (what == "star") {
            for (int i = 0; i < 10; ++i) {
                const double a = -1.5708 + i * 3.14159265 / 5, r = i % 2 ? 13 : 31;
                pts.push_back({32 + r * std::cos(a), 32 + r * std::sin(a)});
            }
            fillPolygon(img, pts, {255, 255, 255, 255});
        } else if (what == "leaf") {
            fillEllipse(img, 32, 32, 14, 30, {255, 255, 255, 255});
        } else {
            std::uint32_t s = 7;
            for (int i = 0; i < 24; ++i) {
                s = s * 1664525u + 1013904223u;
                const double x = 8 + (s >> 8) % 48;
                s = s * 1664525u + 1013904223u;
                const double y = 8 + (s >> 8) % 48;
                fillEllipse(img, x, y, 2 + (s >> 4) % 5, 2 + (s >> 4) % 5, {255, 255, 255, 255});
            }
        }
        auto m = std::make_shared<Mask>(64, 64);
        for (std::size_t i = 0; i < m->v.size(); ++i) m->v[i] = img.data()[i * 4 + 3];
        return m;
    }
    try {
        auto opened = openImage(what);
        Image img = opened->flatten();
        if (std::fabs(scale - 1) > 1e-6 && pattern) {
            img = resize(img, std::max(1, static_cast<int>(img.width() * scale)), std::max(1, static_cast<int>(img.height() * scale)));
        }
        return maskFromImage(img);
    } catch (const std::exception& e) {
        error = "can't use '" + what + "' as a " + (pattern ? "texture" : "tip") + ": " + e.what();
        return nullptr;
    }
}

}  // namespace

std::vector<ParamSpec> brushParams(bool withColor, const std::string& preset) {
    std::vector<ParamSpec> v = {
        docParam(),
        {"points", ParamType::NumberList, "stroke path as x,y pairs in px, e.g. [10,10, 50,40, 90,10]", true},
        {"pressures", ParamType::NumberList, "optional pen pressure per point, 0..1 (from a tablet)", false},
        {"brush", ParamType::String, "brush preset", false, presetNames(), preset},
        {"size", ParamType::Number, "brush diameter in px (overrides the preset)", false},
        {"hardness", ParamType::Number, "edge hardness 0..100 (overrides the preset)", false},
        {"opacity", ParamType::Number, "stroke opacity 0..100", false, {}, 100.0},
        {"flow", ParamType::Number, "paint per dab 0..100 (overrides the preset)", false},
        {"spacing", ParamType::Number, "dab spacing as % of size (overrides the preset)", false},
        {"smooth", ParamType::Number, "stabilizer for shaky mouse input, 0..90", false, {}, 0.0},
        {"scatter", ParamType::Number, "throw dabs off the path, % of size (0..400)", false},
        {"tip", ParamType::String, "tip shape: square, star, leaf, splatter, layer:N (a layer's pixels) or an image file", false},
        {"texture", ParamType::String, "paper texture: a pattern (noise, checker, dots...), layer:N or an image file", false},
        {"texture_scale", ParamType::Number, "texture size, %", false, {}, 100.0},
        {"texture_depth", ParamType::Number, "how much the texture shows, 0..100", false, {}, 100.0},
        {"symmetry", ParamType::String, "paint mirrored copies too", false, {"none", "vertical", "horizontal", "both", "radial"}, std::string("none")},
        {"symmetry_count", ParamType::Int, "radial: copies around the centre", false, {}, std::int64_t{6}},
        {"symmetry_x", ParamType::Number, "symmetry centre x (default: middle)", false},
        {"symmetry_y", ParamType::Number, "symmetry centre y (default: middle)", false},
    };
    if (withColor) v.insert(v.begin() + 3, colorParam("color", "paint colour", false, "#000000"));
    v.push_back(layerParam());
    v.push_back(previewParam());
    return v;
}

bool readBrush(const ImageDoc& doc, const Params& p, Brush& b, std::vector<StrokePoint>& pts, std::string& error) {
    if (!strokePoints(p, pts, error)) return false;
    const Preset& pre = presets().at(stringParam(p, "brush", "round"));
    if (p.count("color") && !colorArg(p, "color", "#000000", b.color, error)) return false;
    b.size = std::max(0.5, numberParam(p, "size", pre.size));
    b.hardness = percent(p, "hardness", pre.hardness);
    b.flow = percent(p, "flow", pre.flow);
    b.spacing = std::max(0.02, numberParam(p, "spacing", pre.spacing) / 100.0);
    b.opacity = percent(p, "opacity", 100);
    b.pressureSize = pre.pressureSize;
    b.pressureOpacity = pre.pressureOpacity;
    b.scatter = std::clamp(numberParam(p, "scatter", pre.scatter * 100), 0.0, 400.0) / 100;
    const std::string tip = stringParam(p, "tip", stringParam(p, "brush", "round") == "square" ? "square" : "");
    if (!tip.empty() && !(b.tip = loadShape(doc, tip, false, 1, error))) return false;
    const std::string texture = stringParam(p, "texture", pre.texture);
    if (!texture.empty() && texture != "none") {
        if (!(b.texture = loadShape(doc, texture, true, std::clamp(numberParam(p, "texture_scale", 100), 10.0, 1000.0) / 100, error))) return false;
        b.textureDepth = percent(p, "texture_depth", 100);
    }
    const std::string sym = stringParam(p, "symmetry", "none");
    b.symmetry.mirrorX = sym == "vertical" || sym == "both";
    b.symmetry.mirrorY = sym == "horizontal" || sym == "both";
    b.symmetry.radial = sym == "radial" ? static_cast<int>(std::clamp<std::int64_t>(intParam(p, "symmetry_count", 6), 1, 64)) : 1;
    b.symmetry.cx = numberParam(p, "symmetry_x", -1);
    b.symmetry.cy = numberParam(p, "symmetry_y", -1);
    // the same stroke always scatters the same way, so redo paints the same picture
    std::uint32_t h = 2166136261u;
    for (const auto& q : pts) h = (h ^ static_cast<std::uint32_t>(std::lround(q.x * 7 + q.y * 131))) * 16777619u;
    b.seed = h;
    pts = smooth(pts, std::clamp(numberParam(p, "smooth", 0), 0.0, 90.0) / 100.0);
    return true;
}

namespace {

// The shared shape of every brush command: find the layer, read the brush, record the area, run `paint`.
template <typename Fn>
Result brushEdit(ImageDoc& doc, const Params& p, const std::string& what, Fn paint) {
    std::string error;
    const int idx = pixelLayerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    Brush b;
    std::vector<StrokePoint> pts;
    if (!readBrush(doc, p, b, pts, error)) return Result::failure(error);
    doc.active = idx;
    Image& target = doc.image.layers[static_cast<std::size_t>(idx)].image;
    if (!paint(target, b, pts, error)) return Result::failure(error);  // checks first, then touch + paint
    return done(what + " along " + std::to_string(pts.size()) + " points", doc, p);
}

// Records the area a brush will change (undo keeps just that), then paints.
template <typename Fn>
bool recordAnd(ImageDoc& doc, Image& target, const Brush& b, const std::vector<StrokePoint>& pts, Fn fn) {
    const PixelRect area = strokeBounds(target, b, pts);
    doc.touch(doc.active, area.x0, area.y0, area.width(), area.height());
    fn();
    return true;
}

std::vector<ParamSpec> plus(std::vector<ParamSpec> v, std::initializer_list<ParamSpec> extra) {
    v.insert(v.begin() + 2, extra.begin(), extra.end());
    return v;
}

}  // namespace

void registerRetouchBrushes(Session& session) {
    auto& reg = session.registry();
    const auto declared = ImageEdit::Mode::Declared;

    reg.add({"paint.stroke",
             "Brush stroke",
             "Paint a brush stroke through a list of points. Use pressures for tablet-style strokes and smooth to steady "
             "mouse input. Presets: pen, pencil, ink, marker, airbrush, soft, round, chalk, spray, square. scatter, tip, "
             "texture and symmetry (mirror or radial copies) work like other painting apps' brush settings.",
             brushParams(true, "round"), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                return brushEdit(doc, p, "painted a stroke", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    return recordAnd(doc, t, b, pts, [&] { stroke(t, b, pts); });
                });
            }, true, declared));

    reg.add({"paint.erase",
             "Eraser stroke",
             "Erase along a list of points (makes the layer transparent there). Takes the same brush settings as paint.stroke.",
             brushParams(false, "round"), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                return brushEdit(doc, p, "erased a stroke", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    b.erase = true;
                    return recordAnd(doc, t, b, pts, [&] { stroke(t, b, pts); });
                });
            }, true, declared));

    reg.add({"paint.clone",
             "Clone stamp",
             "Paint with pixels copied from elsewhere in the picture. source_x/source_y is where the copying starts: it "
             "lines up with the first point of the stroke and moves with it (like alt-clicking a source in other apps). "
             "sample=image copies what you see (all layers), layer copies just this layer.",
             plus(brushParams(false, "soft"),
                  {{"source_x", ParamType::Number, "x of the spot to copy from (matches the first point)", true},
                   {"source_y", ParamType::Number, "y of the spot to copy from", true},
                   {"sample", ParamType::String, "copy from this layer or from the whole picture", false, {"layer", "image"}, std::string("layer")}}),
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                return brushEdit(doc, p, "cloned", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    const int dx = static_cast<int>(std::lround(numberParam(p, "source_x") - listParam(p, "points")[0]));
                    const int dy = static_cast<int>(std::lround(numberParam(p, "source_y") - listParam(p, "points")[1]));
                    const Image source = stringParam(p, "sample", "layer") == "image" ? doc.flatten() : t;
                    return recordAnd(doc, t, b, pts, [&] { cloneStroke(t, source, dx, dy, b, pts); });
                });
            }, true, declared));

    reg.add({"paint.dodge",
             "Dodge, burn, sponge",
             "Brush that lightens (dodge), darkens (burn) or changes the colourfulness (sponge) of what it passes over. "
             "range picks the tones dodge and burn work on most; exposure is the strength.",
             plus(brushParams(false, "soft"),
                  {{"tool", ParamType::String, "what the brush does", false, {"dodge", "burn", "sponge"}, std::string("dodge")},
                   {"range", ParamType::String, "dodge/burn: tones to work on", false, {"shadows", "midtones", "highlights"}, std::string("midtones")},
                   {"exposure", ParamType::Number, "strength 0..100", false, {}, 50.0},
                   {"sponge_mode", ParamType::String, "sponge: more or less colour", false, {"saturate", "desaturate"}, std::string("desaturate")}}),
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                const std::string tool = stringParam(p, "tool", "dodge");
                return brushEdit(doc, p, tool == "dodge" ? "dodged" : tool == "burn" ? "burned" : "sponged", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    const ToneTool tt = tool == "burn" ? ToneTool::Burn : tool == "sponge" ? ToneTool::Sponge : ToneTool::Dodge;
                    const std::string r = stringParam(p, "range", "midtones");
                    const ToneRange tr = r == "shadows" ? ToneRange::Shadows : r == "highlights" ? ToneRange::Highlights : ToneRange::Midtones;
                    return recordAnd(doc, t, b, pts, [&] {
                        toneStroke(t, tt, tr, percent(p, "exposure", 50), stringParam(p, "sponge_mode", "desaturate") == "saturate", b, pts);
                    });
                });
            }, true, declared));

    reg.add({"paint.smudge",
             "Smudge",
             "Drag the colours under the brush along the stroke, like a finger in wet paint. finger_paint starts with the "
             "colour param instead.",
             plus(brushParams(true, "soft"),
                  {{"strength", ParamType::Number, "how far colours are carried, 0..100", false, {}, 50.0},
                   {"finger_paint", ParamType::Bool, "start the smear with `color`", false, {}, false}}),
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                return brushEdit(doc, p, "smudged", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    return recordAnd(doc, t, b, pts, [&] { smudgeStroke(t, percent(p, "strength", 50), boolParam(p, "finger_paint", false), b, pts); });
                });
            }, true, declared));

    reg.add({"paint.blur_brush",
             "Blur or sharpen brush",
             "Soften (blur) or crisp up (sharpen) just where the brush goes.",
             plus(brushParams(false, "soft"),
                  {{"mode", ParamType::String, "what the brush does", false, {"blur", "sharpen"}, std::string("blur")},
                   {"strength", ParamType::Number, "0..100", false, {}, 50.0}}),
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                const bool sharpen = stringParam(p, "mode", "blur") == "sharpen";
                return brushEdit(doc, p, sharpen ? "sharpened" : "blurred", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    return recordAnd(doc, t, b, pts, [&] { blurStroke(t, sharpen, percent(p, "strength", 50), b, pts); });
                });
            }, true, declared));

    reg.add({"paint.color_replace",
             "Colour replacement",
             "Brush that recolours: pixels like the one under the first point (within tolerance) take the hue and "
             "saturation of `color` and keep their own light and shade, e.g. to change the colour of eyes or a shirt.",
             plus(brushParams(true, "round"), {{"tolerance", ParamType::Int, "how different a colour can be and still change, 0..255", false, {}, std::int64_t{40}}}),
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                return brushEdit(doc, p, "replaced the colour", [&](Image& t, Brush& b, auto& pts, std::string&) {
                    return recordAnd(doc, t, b, pts, [&] { replaceColorStroke(t, static_cast<int>(intParam(p, "tolerance", 40)), b, pts); });
                });
            }, true, declared));
}

void registerFillCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"paint.pattern_fill",
             "Pattern fill",
             "Fill a layer (the selection when there is one, or the area around x,y like a paint bucket) with a repeating "
             "pattern: a built-in one in two colours, another layer's pixels, or an image file.",
             {docParam(),
              {"pattern", ParamType::String, "built-in pattern", false, patternNames(), std::string("checker")},
              {"pattern_layer", ParamType::Int, "use this layer's pixels as the tile instead", false},
              {"pattern_path", ParamType::String, "use this image file as the tile instead", false},
              {"size", ParamType::Int, "built-in pattern cell size in px", false, {}, std::int64_t{16}},
              colorParam("color", "built-in pattern: main colour", false, "#000000"),
              colorParam("background", "built-in pattern: second colour", false, "#ffffff"),
              {"scale", ParamType::Number, "tile size, %", false, {}, 100.0},
              {"offset_x", ParamType::Int, "shift the tiles right", false, {}, std::int64_t{0}},
              {"offset_y", ParamType::Int, "shift the tiles down", false, {}, std::int64_t{0}},
              {"x", ParamType::Int, "fill only the area around this point (with y)", false},
              {"y", ParamType::Int, "fill only the area around this point (with x)", false},
              {"tolerance", ParamType::Int, "with x,y: how different a colour can be and still fill, 0..255", false, {}, std::int64_t{32}},
              {"opacity", ParamType::Number, "0..100", false, {}, 100.0},
              {"blend", ParamType::String, "blend mode", false, blendNames(), std::string("normal")},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Image tile;
                if (p.count("pattern_layer")) {
                    const auto n = intParam(p, "pattern_layer");
                    if (n < 0 || n >= static_cast<std::int64_t>(doc.image.layers.size()) || doc.image.layers[static_cast<std::size_t>(n)].image.empty()) {
                        return Result::failure("pattern_layer " + std::to_string(n) + " isn't a pixel layer");
                    }
                    const Image& src = doc.image.layers[static_cast<std::size_t>(n)].image;
                    const PixelRect b = bounds(alphaMask(src));
                    if (b.empty()) return Result::failure("pattern_layer " + std::to_string(n) + " is empty");
                    tile = crop(src, b.x0, b.y0, b.width(), b.height());
                } else if (p.count("pattern_path")) {
                    try {
                        tile = openImage(stringParam(p, "pattern_path"))->flatten();
                    } catch (const std::exception& e) {
                        return Result::failure(std::string("can't open the pattern: ") + e.what());
                    }
                } else {
                    Color a, b;
                    if (!colorArg(p, "color", "#000000", a, error) || !colorArg(p, "background", "#ffffff", b, error)) return Result::failure(error);
                    tile = builtinPattern(stringParam(p, "pattern", "checker"), static_cast<int>(intParam(p, "size", 16)), a, b);
                }
                Image& img = doc.image.layers[static_cast<std::size_t>(idx)].image;
                Image fill = tilePattern(tile, img.width(), img.height(), static_cast<int>(intParam(p, "offset_x", 0)),
                                         static_cast<int>(intParam(p, "offset_y", 0)), std::clamp(numberParam(p, "scale", 100), 1.0, 10000.0) / 100);
                if (p.count("x") != p.count("y")) return Result::failure("give both x and y (or neither, to fill the whole layer)");
                if (p.count("x")) {
                    const int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
                    if (!img.contains(x, y)) return Result::failure("point is outside the image");
                    applyMask(fill, wandMask(img, x, y, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "tolerance", 32), 0, 255)), true));
                }
                blendOnto(img, fill, 0, 0, *parseBlend(stringParam(p, "blend", "normal")), percent(p, "opacity", 100));
                doc.active = idx;
                return done("filled layer " + std::to_string(idx) + " with a pattern", doc, p);
            }));

    std::vector<std::string> presetChoices = gradientPresetNames();
    presetChoices.insert(presetChoices.begin(), "none");
    reg.add({"paint.gradient",
             "Gradient",
             "Paint a gradient across the layer from (x1,y1) to (x2,y2): from one colour to another, through a list of "
             "colours, or a preset. Shapes: linear, radial, angle (sweeps around), reflected (mirrored both ways), diamond.",
             {docParam(),
              {"x1", ParamType::Number, "start x (radial: centre x)", true},
              {"y1", ParamType::Number, "start y (radial: centre y)", true},
              {"x2", ParamType::Number, "end x (radial: edge x)", true},
              {"y2", ParamType::Number, "end y (radial: edge y)", true},
              colorParam("from", "start colour", false, "#000000"),
              colorParam("to", "end colour", false, "#ffffff"),
              {"colors", ParamType::String, "several colours instead, e.g. #000000,#ff8800,#ffffff (add @percent for positions)", false},
              {"preset", ParamType::String, "a built-in gradient instead", false, presetChoices, std::string("none")},
              {"kind", ParamType::String, "gradient shape", false, gradientShapeNames(), std::string("linear")},
              {"reverse", ParamType::Bool, "run it the other way", false, {}, false},
              {"opacity", ParamType::Number, "0..100", false, {}, 100.0},
              {"blend", ParamType::String, "blend mode", false, blendNames(), std::string("normal")},
              layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = pixelLayerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                std::vector<GradientStop> stops;
                const std::string preset = stringParam(p, "preset", "none");
                if (preset != "none") {
                    stops = gradientPreset(preset);
                } else if (p.count("colors")) {
                    if (!parseGradientStops(stringParam(p, "colors"), stops, error)) return Result::failure(error);
                } else {
                    Color a, b;
                    if (!colorArg(p, "from", "#000000", a, error) || !colorArg(p, "to", "#ffffff", b, error)) return Result::failure(error);
                    stops = {{0, a}, {1, b}};
                }
                drawGradient(doc.image.layers[static_cast<std::size_t>(idx)].image, stops, *parseGradientShape(stringParam(p, "kind", "linear")),
                             numberParam(p, "x1"), numberParam(p, "y1"), numberParam(p, "x2"), numberParam(p, "y2"), percent(p, "opacity", 100),
                             *parseBlend(stringParam(p, "blend", "normal")), boolParam(p, "reverse", false));
                doc.active = idx;
                return done("painted a gradient", doc, p);
            }));
}

}  // namespace wetwaresuite::paint
