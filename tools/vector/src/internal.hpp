// Helpers the vector command files share (commands.cpp, cmd_text.cpp).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/raster/document.hpp"
#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector::detail {

using Edit = std::function<Result(VectorDoc&, const Params&)>;

inline std::string num(double v) {
    if (std::abs(v) < 5e-4) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

inline std::string paintText(const std::optional<Color>& c) {
    if (!c || c->a == 0) return "none";
    std::string hex = c->hex();
    return c->a == 255 ? hex.substr(0, 7) : hex;
}

inline bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

// "none" -> no paint; anything else must be a colour.
inline bool paintArg(const std::string& text, std::optional<Color>& out, std::string& error) {
    std::string t;
    for (char c : text) t += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t == "none" || t.empty()) {
        out.reset();
        return true;
    }
    auto c = parseColor(text);
    if (!c) {
        error = "'" + text + "' isn't a colour; use #rrggbb, #rrggbbaa, rgb(r,g,b), a name like red, or none";
        return false;
    }
    out = *c;
    return true;
}

inline ParamSpec docParam() { return {"doc", ParamType::String, "id of an open vector document, e.g. vec1", true}; }
inline ParamSpec shapeParam() { return {"shape", ParamType::String, "shape id (e.g. s3, from vector.info) or shape name", true}; }
inline ParamSpec previewParam() { return raster::previewParam(); }

inline ParamSpec numParam(const std::string& name, const std::string& description, bool required = true, Value fallback = {}) {
    return {name, ParamType::Number, description, required, {}, std::move(fallback)};
}

inline ParamSpec paintParam(const std::string& name, const std::string& what, const std::string& fallback) {
    return {name, ParamType::String, what + ": #rrggbb, #rrggbbaa, rgb(r,g,b), a name like red, or none", false, {},
            fallback.empty() ? Value{} : Value{fallback}};
}

// Style params shared by the add_* commands, with per-kind defaults.
inline std::vector<ParamSpec> styleParams(const std::string& fill, const std::string& stroke, double strokeWidth) {
    std::vector<ParamSpec> out;
    if (!fill.empty()) out.push_back(paintParam("fill", "fill colour", fill));
    out.push_back(paintParam("stroke", "outline colour", stroke));
    out.push_back(numParam("stroke_width", "outline width in px", false, strokeWidth));
    out.push_back(numParam("opacity", "opacity of the whole shape, 0..1", false, 1.0));
    out.push_back({"name", ParamType::String, "a name to find the shape by later (default: kind + number)", false});
    out.push_back(previewParam());
    return out;
}

inline std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Applies fill/stroke/stroke_width/opacity/name/visible params that are present.
inline bool applyStyle(Shape& s, const Params& p, std::string& error) {
    if (has(p, "fill")) {
        if (!paintArg(stringParam(p, "fill"), s.fill, error)) return false;
        s.fillSwatch.clear();
    }
    if (has(p, "stroke")) {
        if (!paintArg(stringParam(p, "stroke"), s.stroke, error)) return false;
        s.strokeSwatch.clear();
    }
    if (has(p, "blend")) {
        auto b = raster::parseBlend(stringParam(p, "blend"));
        if (!b) {
            error = "'" + stringParam(p, "blend") + "' isn't a blend mode (normal, multiply, screen, overlay, darken, lighten, ...)";
            return false;
        }
        s.blend = raster::toString(*b);
    }
    if (has(p, "fill_rule")) {
        const std::string r = stringParam(p, "fill_rule");
        if (r != "evenodd" && r != "nonzero") {
            error = "fill_rule must be evenodd or nonzero";
            return false;
        }
        s.fillRule = r;
    }
    if (has(p, "stroke_width")) {
        double w = numberParam(p, "stroke_width");
        if (w < 0) {
            error = "stroke_width can't be negative";
            return false;
        }
        s.strokeWidth = w;
    }
    if (has(p, "opacity")) s.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
    if (has(p, "name")) s.name = stringParam(p, "name");
    if (has(p, "visible")) s.visible = boolParam(p, "visible", true);
    return true;
}

inline Result& maybePreview(Result& r, const VectorDoc& doc, const Params& p) {
    if (r.ok && boolParam(p, "preview", false)) r.attach(*doc.preview());
    return r;
}

inline ActionFactory vectorCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<VectorDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] {
                return Result::failure("no open vector document called '" + id + "' (use vector.new or vector.open first)");
            });
        }
        if (!undoable) return std::make_unique<FunctionAction>([doc, edit, p] { return edit(*doc, p); });
        return std::make_unique<VectorEdit>(doc, [edit, p](VectorDoc& d) { return edit(d, p); });
    };
}

// Runs `fn` on the shape named by the "shape" param.
inline Edit onShape(std::function<Result(VectorDoc&, int, const Params&)> fn) {
    return [fn](VectorDoc& doc, const Params& p) {
        const std::string name = stringParam(p, "shape");
        int idx = doc.find(name);
        if (idx < 0) return Result::failure("no shape '" + name + "' in this document (vector.info lists them)");
        Result r = fn(doc, idx, p);
        return maybePreview(r, doc, p);
    };
}

// Shared tail of every add_* command.
inline Result addShape(VectorDoc& doc, Shape s, const Params& p) {
    std::string error;
    if (!applyStyle(s, p, error)) return Result::failure(error);
    const std::string kind = toString(s.kind);
    std::string id = doc.add(std::move(s));
    Result r = Result::success("added " + kind + " " + id, id);
    r.with("shape", id);
    return maybePreview(r, doc, p);
}

inline bool pointsArg(const Params& p, std::size_t minPoints, std::vector<Point>& out, std::string& error) {
    NumberList v = listParam(p, "points");
    if (v.size() % 2 != 0 || v.size() / 2 < minPoints) {
        error = "points must be x,y pairs (at least " + std::to_string(minPoints) + " points, e.g. 10,10,90,10,50,80)";
        return false;
    }
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) out.push_back({v[i], v[i + 1]});
    return true;
}

inline std::string describe(const Shape& s) {
    std::string g;
    switch (s.kind) {
        case ShapeKind::Rect:
            g = "x=" + num(s.x) + " y=" + num(s.y) + " w=" + num(s.w) + " h=" + num(s.h);
            if (s.radius > 0) g += " radius=" + num(s.radius);
            break;
        case ShapeKind::Ellipse: g = "cx=" + num(s.cx) + " cy=" + num(s.cy) + " rx=" + num(s.rx) + " ry=" + num(s.ry); break;
        case ShapeKind::Line:
        case ShapeKind::Polyline:
        case ShapeKind::Polygon:
            g = "points=";
            for (std::size_t i = 0; i < s.points.size(); ++i) {
                if (i == 8) {
                    g += " ... (" + std::to_string(s.points.size()) + " points)";
                    break;
                }
                g += (i ? " " : "") + num(s.points[i].x) + "," + num(s.points[i].y);
            }
            break;
        case ShapeKind::Path: g = "d=\"" + (s.d.size() > 80 ? s.d.substr(0, 77) + "..." : s.d) + "\""; break;
        case ShapeKind::Text:
            g = "text=\"" + (s.text.size() > 60 ? s.text.substr(0, 57) + "..." : s.text) + "\" x=" + num(s.x) + " y=" + num(s.y) +
                " size=" + num(s.fontSize) + " font=" + s.font + (s.anchor != "start" ? " anchor=" + s.anchor : "");
            if (s.boxWidth > 0) g += " box=" + num(s.boxWidth) + "x" + num(s.boxHeight) + (s.columns > 1 ? " columns=" + std::to_string(s.columns) : "") + " align=" + s.align;
            if (!s.onPath.empty()) g += " on_path=" + s.onPath + (s.pathOffset != 0 ? " offset=" + num(s.pathOffset) : "");
            if (s.textCase != "none") g += " case=" + s.textCase;
            if (!s.textStyle.empty()) g += " style=" + s.textStyle;
            break;
        case ShapeKind::Group:
            g = s.layer ? "(layer)" : "(group)";
            if (!s.clip.empty()) g += " clip=" + s.clip;
            if (!s.mask.empty()) g += " mask=" + s.mask;
            break;
        case ShapeKind::Instance: g = "symbol=" + s.symbol; break;
    }
    std::string out = s.id + " " + toString(s.kind) + " \"" + s.name + "\" " + g;
    if (!s.parent.empty()) out += " in=" + s.parent;
    if (s.kind == ShapeKind::Group || s.kind == ShapeKind::Instance) {
        if (s.opacity < 1) out += " opacity=" + num(s.opacity);
        if (s.blend != "normal") out += " blend=" + s.blend;
        if (!s.visible) out += " hidden";
        if (s.locked) out += " locked";
        return out;
    }
    out += " fill=" + paintText(s.fill) + " stroke=" + paintText(s.stroke);
    if (s.gradient) out += std::string(" gradient=") + (s.gradient->radial ? "radial" : "linear") + "(" + std::to_string(s.gradient->stops.size()) + " stops)";
    if (s.gradient && s.gradient->freeform) out += "(freeform)";
    if (s.strokeGradient) out += " stroke_gradient";
    if (s.pattern) out += " pattern=" + s.pattern->type;
    if (s.stroke) out += " stroke_width=" + num(s.strokeWidth);
    if (s.stroke) {
        std::string opts;
        const StrokeStyle def;
        if (s.line.cap != def.cap) opts += " cap=" + s.line.cap;
        if (s.line.join != def.join) opts += " join=" + s.line.join;
        if (!s.line.dash.empty()) opts += " dashed";
        if (s.line.align != "center") opts += " align=" + s.line.align;
        if (s.line.startArrow != "none" || s.line.endArrow != "none") opts += " arrows=" + s.line.startArrow + "," + s.line.endArrow;
        if (s.line.profile != "uniform") opts += " profile=" + s.line.profile;
        out += opts;
    }
    if (s.fillRule == "nonzero" && (s.kind == ShapeKind::Path || s.kind == ShapeKind::Polygon)) out += " fill_rule=nonzero";
    if (!s.appearance.empty()) out += " appearance=" + std::to_string(s.appearance.size());
    if (!s.effects.empty()) {
        out += " effects=";
        for (std::size_t i = 0; i < s.effects.size(); ++i) out += (i ? "," : "") + s.effects[i].type;
    }
    if (s.blend != "normal") out += " blend=" + s.blend;
    if (s.locked) out += " locked";
    if (s.opacity < 1) out += " opacity=" + num(s.opacity);
    const Transform& t = s.transform;
    if (!t.identity()) {
        out += " transform=(";
        if (t.tx != 0 || t.ty != 0) out += "move " + num(t.tx) + "," + num(t.ty) + " ";
        if (t.rotate != 0) out += "rotate " + num(t.rotate) + " ";
        if (t.sx != 1 || t.sy != 1) out += "scale " + num(t.sx) + "," + num(t.sy) + " ";
        out.back() = ')';
    }
    Bounds b = bounds(outline(s));
    if (!b.empty) out += " bounds=" + num(b.x0) + "," + num(b.y0) + " " + num(b.width()) + "x" + num(b.height());
    if (!s.visible) out += " hidden";
    return out;
}


// Coverage of an area on a W x H picture (0..1 per pixel), kept to the area's box.
struct Cover {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<float> a;
    bool empty() const { return w <= 0 || h <= 0; }
    float at(int x, int y) const {
        if (x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) return 0;
        return a[static_cast<std::size_t>(y - y0) * w + (x - x0)];
    }
};
Cover rasterize(const Outline& area, bool nonzero, int W, int H);

// One laid-out line of area text: its words, where its baseline starts, extra px per space (justify).
struct TextLine {
    std::string text;
    double x = 0, y = 0, width = 0, wordGap = 0;
};
std::vector<TextLine> layoutArea(const Shape& s, bool* overflow);

// The nearest translate/rotate/scale about `centre` (skew is dropped).
Transform decomposeMatrix(const Matrix& m, Point centre);
// Copies a shape and everything inside it (fresh ids) just above it; returns the copy's id.
std::string duplicateSubtree(VectorDoc& doc, int index, const std::string& newParent);
// Removes a shape and everything inside it, and any clip/mask links to them.
void eraseSubtree(VectorDoc& doc, int index);

// Text forms (serial.cpp), shared by commands, vector.info and the SVG data-* attributes.
std::string colorText(const Color& c);
std::vector<std::string> splitOn(const std::string& text, char sep);  // trimmed, empties dropped, (..) kept whole
bool readStrokeOption(StrokeStyle& st, const std::string& key, const std::string& value, std::string& error);
std::string strokeOptionsText(const StrokeStyle& st);  // only what differs from the defaults
std::string appearanceText(const std::vector<AppearanceItem>& items);
bool parseAppearanceItem(const std::string& text, AppearanceItem& a, std::string& error);
bool parseAppearance(const std::string& text, std::vector<AppearanceItem>& out, std::string& error);
std::string effectsText(const std::vector<Effect>& effects);
bool parseEffect(const std::string& text, Effect& e, std::string& error);
bool parseEffects(const std::string& text, std::vector<Effect>& out, std::string& error);
std::string patternText(const Pattern& p);
bool parsePattern(const std::string& text, Pattern& p, std::string& error);
std::string freeformText(const std::vector<ColorPoint>& pts);
bool parseFreeform(const std::string& text, std::vector<ColorPoint>& out, std::string& error);
std::string textStyleText(const TextStyle& t);
bool parseTextStyle(const std::string& text, TextStyle& t);
bool validName(const std::string& name);  // letters, digits, _ - . (swatch, symbol, style and artboard names)
void applyTextStyle(Shape& s, const TextStyle& t);

// Shared by the command files (cmd_util.cpp).
// Shapes named by a comma-separated list param (ids or names); fails on unknown names or an empty list.
bool shapeList(const VectorDoc& doc, const std::string& list, std::vector<int>& out, std::string& error);
// The "shapes" param, or the single "shape" param when shapes isn't given.
bool selection(const VectorDoc& doc, const Params& p, std::vector<int>& out, std::string& error);
// Drops shapes that sit inside another listed shape (moving the group moves them already).
std::vector<int> topmost(const VectorDoc& doc, std::vector<int> idx);
bool editable(const VectorDoc& doc, int idx, std::string& error);  // not locked (itself or a group around it)
// A shape's filled area on the canvas, closed contours (text and groups included).
Outline areaOf(const VectorDoc& doc, int idx, double detail = 4.0);
// Outer contours each with the holes directly inside them (even-odd nesting).
std::vector<Outline> pieces(const Outline& area);
// Moves the listed shapes (with what's inside them) so they sit just before list index `before`
// (an index into the list as it was); returns where the block starts now.
int moveBlock(VectorDoc& doc, std::vector<int> idx, int before);
// Puts shapes into a new group at the stacking place of the topmost one; returns the group id.
std::string groupShapes(VectorDoc& doc, std::vector<int> idx, const std::string& name);
// A fresh shape id (the shape isn't added).
std::string freshId(VectorDoc& doc);
// Inserts `s` (given a fresh id when it has none) at list index `at`; returns its id.
std::string insertShape(VectorDoc& doc, Shape s, int at);
// The leaves of the drawing in paint order (bottom first), hidden, clip and mask shapes left out.
std::vector<int> paintOrder(const VectorDoc& doc);
// Topmost visible, unlocked leaf under p (within `tolerance` px of its outline counts), or -1.
int hitTest(const VectorDoc& doc, Point p, double tolerance);
// The outermost group around a shape that isn't a layer (what a click selects), or the shape itself.
int selectable(const VectorDoc& doc, int idx);
// Snaps p to the grid, guides and anchors as the document's snap settings say, within `radius`.
Point snapPoint(const VectorDoc& doc, Point p, double radius, std::string* what = nullptr);
// A path-like shape's anchors on the canvas (its transform applied). Fails for text, groups and instances.
bool canvasPath(const Shape& s, BezierPath& out, std::string& error);
// Stores canvas anchors back: the shape becomes a path with no transform (outline width kept as drawn).
void storePath(Shape& s, const BezierPath& bp);
// Everything a list param result needs: "s1,s2".
std::string idText(const VectorDoc& doc, const std::vector<int>& idx);

// Registers the text, gradient, boolean and to-path commands (cmd_text.cpp).
void registerTextCommands(Session& session);
void registerPathCommands(Session& session);       // cmd_paths.cpp: anchors, pen, join, split, simplify, offset ...
void registerRegionCommands(Session& session);     // cmd_regions.cpp: compound, divide, shape builder, paint, clip, mask
void registerAppearanceCommands(Session& session); // cmd_appearance.cpp: stroke options, appearance, effects, swatches
void registerOrganizeCommands(Session& session);   // cmd_organize.cpp: layers, groups, align, transform, repeat, hit
void registerBoardCommands(Session& session);      // cmd_boards.cpp: artboards, guides, symbols, export, trace

}  // namespace wetwaresuite::vector::detail
