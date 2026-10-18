// Helpers the vector command files share (commands.cpp, cmd_text.cpp).
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>

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
    if (has(p, "fill") && !paintArg(stringParam(p, "fill"), s.fill, error)) return false;
    if (has(p, "stroke") && !paintArg(stringParam(p, "stroke"), s.stroke, error)) return false;
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
            break;
    }
    std::string out = s.id + " " + toString(s.kind) + " \"" + s.name + "\" " + g + " fill=" + paintText(s.fill) +
                      " stroke=" + paintText(s.stroke);
    if (s.gradient) out += std::string(" gradient=") + (s.gradient->radial ? "radial" : "linear") + "(" + std::to_string(s.gradient->stops.size()) + " stops)";
    if (s.stroke) out += " stroke_width=" + num(s.strokeWidth);
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


// Registers the text, gradient, boolean and to-path commands (cmd_text.cpp).
void registerTextCommands(Session& session);

}  // namespace wetwaresuite::vector::detail
