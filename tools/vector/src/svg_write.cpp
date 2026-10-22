// SVG export. Plain SVG wherever SVG can say it (gradients, patterns, dashes,
// arrow markers, clip paths, masks, filters, symbols and <use>, blend modes);
// what it can't (stroke alignment, width profiles, extra fills and strokes)
// is drawn with extra elements, and data-* attributes keep the settings so
// vector.open reads the file back exactly.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

namespace {

std::string num4(double v) {
    if (std::abs(v) < 5e-5) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? std::string("0") : s;
}

std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "&#10;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string rgb(Color c) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += t[(v >> 18) & 63], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += t[(v >> 18) & 63], out += t[(v >> 12) & 63], out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += t[(v >> 18) & 63], out += t[(v >> 12) & 63], out += t[(v >> 6) & 63], out += '=';
    }
    return out;
}

std::string paint(const char* name, const std::optional<Color>& c) {
    if (!c || c->a == 0) return std::string(" ") + name + "=\"none\"";
    std::string out = std::string(" ") + name + "=\"" + rgb(*c) + "\"";
    if (c->a < 255) out += std::string(" ") + name + "-opacity=\"" + num4(c->a / 255.0) + "\"";
    return out;
}

std::string pointsAttr(const std::vector<Point>& pts) {
    std::string out;
    for (const auto& p : pts) out += (out.empty() ? "" : " ") + num4(p.x) + "," + num4(p.y);
    return out;
}

std::string matrixAttr(const Matrix& m) {
    return "matrix(" + num4(m.a) + " " + num4(m.b) + " " + num4(m.c) + " " + num4(m.d) + " " + num4(m.e) + " " + num4(m.f) + ")";
}

std::string transformAttr(const Shape& s) {
    if (s.transform.identity()) return "";
    const Matrix m = shapeMatrix(s);
    std::string t = "translate(" + num4(m.e) + " " + num4(m.f) + ")";
    if (s.transform.rotate != 0) t += " rotate(" + num4(s.transform.rotate) + ")";
    if (s.transform.sx != 1 || s.transform.sy != 1) t += " scale(" + num4(s.transform.sx) + " " + num4(s.transform.sy) + ")";
    return " transform=\"" + t + "\"";
}

std::string fontFamily(const Shape& s) {
    std::string f = s.font;
    for (auto& c : f) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return f == "serif" ? "Times New Roman, serif" : f == "mono" ? "Courier New, monospace" : f == "sans" ? "Roboto, Arial, sans-serif" : s.font;
}

std::string gradientDef(const Gradient& g, const std::string& id) {
    std::string out = g.radial ? "    <radialGradient id=\"" + id + "\" cx=\"" + num4(g.cx) + "\" cy=\"" + num4(g.cy) + "\" r=\"" + num4(g.r) + "\">\n"
                               : "    <linearGradient id=\"" + id + "\" x1=\"" + num4(g.x1) + "\" y1=\"" + num4(g.y1) + "\" x2=\"" + num4(g.x2) +
                                     "\" y2=\"" + num4(g.y2) + "\">\n";
    for (const auto& st : g.stops) {
        out += "      <stop offset=\"" + num4(st.offset) + "\" stop-color=\"" + rgb(st.color) + "\"";
        if (st.color.a < 255) out += " stop-opacity=\"" + num4(st.color.a / 255.0) + "\"";
        out += "/>\n";
    }
    return out + (g.radial ? "    </radialGradient>\n" : "    </linearGradient>\n");
}

// A freeform gradient has no SVG form: a small picture of it stretched over the shape's box.
std::string freeformDef(const Gradient& g, const std::string& id) {
    constexpr int kSide = 48;
    raster::Image img(kSide, kSide);
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) img.set(x, y, g.atPoint((x + 0.5) / kSide, (y + 0.5) / kSide));
    }
    return "    <pattern id=\"" + id + "\" patternContentUnits=\"objectBoundingBox\" width=\"1\" height=\"1\" data-freeform=\"" +
           esc(detail::freeformText(g.points)) + "\"><image width=\"1\" height=\"1\" preserveAspectRatio=\"none\" href=\"data:image/png;base64," +
           base64(raster::encodePng(img)) + "\"/></pattern>\n";
}

std::string patternDef(const Pattern& p, const Bounds& box, const std::optional<Color>& base, const std::string& id) {
    const double s = std::max(p.size, 0.5), w = std::clamp(p.weight, 0.02, 0.98);
    std::string body;
    const std::optional<Color> bg = p.background ? p.background : base;
    if (bg && bg->a > 0) body += "<rect width=\"" + num4(s) + "\" height=\"" + num4(s) + "\"" + paint("fill", bg) + "/>";
    const std::string c = paint("fill", p.color);
    if (p.type == "stripes") body += "<rect width=\"" + num4(s) + "\" height=\"" + num4(s * w) + "\"" + c + "/>";
    else if (p.type == "dots") body += "<circle cx=\"" + num4(s / 2) + "\" cy=\"" + num4(s / 2) + "\" r=\"" + num4(s * w / 2) + "\"" + c + "/>";
    else if (p.type == "grid") {
        body += "<rect width=\"" + num4(s) + "\" height=\"" + num4(s * w * 0.4) + "\"" + c + "/>";
        body += "<rect width=\"" + num4(s * w * 0.4) + "\" height=\"" + num4(s) + "\"" + c + "/>";
    } else if (p.type == "checker") {
        body += "<rect width=\"" + num4(s) + "\" height=\"" + num4(s) + "\"" + c + "/>";  // tile 2s, marks on the diagonal
        body += "<rect x=\"" + num4(s) + "\" y=\"" + num4(s) + "\" width=\"" + num4(s) + "\" height=\"" + num4(s) + "\"" + c + "/>";
    } else if (p.type == "hatch") {
        const double t = s * w * 0.4;
        body += "<path d=\"M0 0 L" + num4(t) + " 0 L" + num4(s) + " " + num4(s - t) + " L" + num4(s) + " " + num4(s) + " L" + num4(s - t) + " " + num4(s) + " L0 " +
                num4(t) + " Z M" + num4(s) + " 0 L" + num4(s) + " " + num4(t) + " L" + num4(t) + " " + num4(s) + " L0 " + num4(s) + " L0 " + num4(s - t) + " L" + num4(s - t) +
                " 0 Z\"" + c + "/>";
    }
    const double tile = p.type == "checker" ? s * 2 : s;
    if (p.type == "checker" && bg && bg->a > 0) body = "<rect width=\"" + num4(tile) + "\" height=\"" + num4(tile) + "\"" + paint("fill", bg) + "/>" + body.substr(body.find("/>") + 2);
    return "    <pattern id=\"" + id + "\" patternUnits=\"userSpaceOnUse\" x=\"" + num4(box.x0) + "\" y=\"" + num4(box.y0) + "\" width=\"" + num4(tile) +
           "\" height=\"" + num4(tile) + "\" patternTransform=\"rotate(" + num4(p.angle) + " " + num4(box.x0) + " " + num4(box.y0) + ")\" data-pattern=\"" +
           esc(detail::patternText(p)) + "\">" + body + "</pattern>\n";
}

std::string markerDef(const std::string& kind, const std::string& id, double scale) {
    // in stroke widths: the tip sits half a width past the end, like the renderer
    const double L = 4 * scale;
    std::string shape;
    if (kind == "triangle") shape = "<path d=\"M" + num4(L) + " 0 L0 " + num4(-L / 2) + " L0 " + num4(L / 2) + " Z\"/>";
    else if (kind == "open") shape = "<path d=\"M0 " + num4(-L / 2) + " L" + num4(L) + " 0 L0 " + num4(L / 2) + "\" fill=\"none\" stroke=\"context-stroke\" stroke-width=\"1\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>";
    else if (kind == "circle") shape = "<circle cx=\"" + num4(L - 0.5) + "\" cy=\"0\" r=\"" + num4(L * 0.4) + "\"/>";
    else if (kind == "square") shape = "<rect x=\"" + num4(L - 0.5 - L * 0.35) + "\" y=\"" + num4(-L * 0.35) + "\" width=\"" + num4(L * 0.7) + "\" height=\"" + num4(L * 0.7) + "\"/>";
    else if (kind == "bar") shape = "<rect x=\"" + num4(L - 1) + "\" y=\"" + num4(-L / 2) + "\" width=\"1\" height=\"" + num4(L) + "\"/>";
    return "    <marker id=\"" + id + "\" data-arrow=\"" + kind + "\" markerUnits=\"strokeWidth\" orient=\"auto-start-reverse\" overflow=\"visible\" refX=\"" +
           num4(L - 0.5) + "\" refY=\"0\" fill=\"context-stroke\">" + shape + "</marker>\n";
}

std::string filterDef(const std::vector<Effect>& fx, const std::string& id) {
    std::string out = "    <filter id=\"" + id + "\" x=\"-50%\" y=\"-50%\" width=\"200%\" height=\"200%\" color-interpolation-filters=\"sRGB\">\n";
    std::string prev = "SourceGraphic";
    int n = 0;
    for (const auto& e : fx) {
        const std::string r = "r" + std::to_string(n++);
        const std::string flood = " flood-color=\"" + rgb(e.color) + "\" flood-opacity=\"" + num4(e.opacity * e.color.a / 255) + "\"";
        if (e.type == "blur") {
            out += "      <feGaussianBlur in=\"" + prev + "\" stdDeviation=\"" + num4(e.radius) + "\" result=\"" + r + "\"/>\n";
        } else if (e.type == "drop_shadow" || e.type == "outer_glow") {
            const double dx = e.type == "drop_shadow" ? e.dx : 0, dy = e.type == "drop_shadow" ? e.dy : 0;
            out += "      <feDropShadow in=\"" + prev + "\" dx=\"" + num4(dx) + "\" dy=\"" + num4(dy) + "\" stdDeviation=\"" + num4(e.radius) + "\"" + flood +
                   " result=\"" + r + "\"/>\n";
        } else {
            const double dx = e.type == "inner_shadow" ? e.dx : 0, dy = e.type == "inner_shadow" ? e.dy : 0;
            out += "      <feFlood" + flood + " result=\"" + r + "f\"/>\n";
            out += "      <feComposite in=\"" + r + "f\" in2=\"SourceAlpha\" operator=\"out\" result=\"" + r + "o\"/>\n";
            out += "      <feOffset in=\"" + r + "o\" dx=\"" + num4(dx) + "\" dy=\"" + num4(dy) + "\" result=\"" + r + "s\"/>\n";
            out += "      <feGaussianBlur in=\"" + r + "s\" stdDeviation=\"" + num4(e.radius) + "\" result=\"" + r + "b\"/>\n";
            out += "      <feComposite in=\"" + r + "b\" in2=\"SourceAlpha\" operator=\"in\" result=\"" + r + "i\"/>\n";
            out += "      <feMerge result=\"" + r + "\"><feMergeNode in=\"" + prev + "\"/><feMergeNode in=\"" + r + "i\"/></feMerge>\n";
        }
        prev = r;
    }
    return out + "    </filter>\n";
}

class Writer {
public:
    explicit Writer(const VectorDoc& doc) : doc_(doc) {}
    std::string defs;

    std::string list(const std::vector<Shape>& shapes, const std::string& parent, int indent, const std::string& prefix) {
        std::string out;
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            if (shapes[i].parent == parent) out += item(shapes, i, indent, prefix);
        }
        return out;
    }

private:
    const VectorDoc& doc_;

    static std::string pad(int n) { return std::string(static_cast<std::size_t>(n) * 2, ' '); }

    // opacity, blend, filter, visibility and lock: on the outermost element of the shape
    std::string common(const Shape& s, const std::string& id) {
        std::string a;
        if (s.opacity < 1) a += " opacity=\"" + num4(s.opacity) + "\"";
        if (s.blend != "normal") a += " style=\"mix-blend-mode:" + s.blend + "\" data-blend=\"" + s.blend + "\"";
        if (!s.effects.empty()) {
            defs += filterDef(s.effects, "fx-" + id);
            a += " filter=\"url(#fx-" + id + ")\" data-effects=\"" + esc(detail::effectsText(s.effects)) + "\"";
        }
        if (!s.visible) a += " display=\"none\"";
        if (s.locked) a += " data-locked=\"1\"";
        return a;
    }

    std::string item(const std::vector<Shape>& shapes, std::size_t i, int indent, const std::string& prefix) {
        const Shape& s = shapes[i];
        const std::string id = prefix + s.id;
        if (s.kind == ShapeKind::Group) {
            std::string a = "<g id=\"" + esc(id) + "\" data-name=\"" + esc(s.name) + "\"";
            if (s.layer) a += " data-kind=\"layer\" inkscape:groupmode=\"layer\" inkscape:label=\"" + esc(s.name) + "\"";
            a += common(s, id);
            std::string clipBody, maskBody;
            for (std::size_t k = 0; k < shapes.size(); ++k) {
                if (shapes[k].parent != s.id) continue;
                if (shapes[k].id == s.clip) clipBody = clipShapes(shapes, k, prefix);
                if (shapes[k].id == s.mask) maskBody = item(shapes, k, 3, prefix);
            }
            if (!s.clip.empty() && !clipBody.empty()) {
                defs += "    <clipPath id=\"clip-" + esc(id) + "\" data-of=\"" + esc(prefix + s.clip) + "\">\n" + clipBody + "    </clipPath>\n";
                a += " clip-path=\"url(#clip-" + esc(id) + ")\"";
            }
            if (!s.mask.empty() && !maskBody.empty()) {
                defs += "    <mask id=\"mask-" + esc(id) + "\" maskUnits=\"userSpaceOnUse\" x=\"-100000\" y=\"-100000\" width=\"200000\" height=\"200000\">\n" + maskBody +
                        "    </mask>\n";
                a += " mask=\"url(#mask-" + esc(id) + ")\"";
            }
            std::string body;
            for (std::size_t k = 0; k < shapes.size(); ++k) {
                if (shapes[k].parent == s.id && shapes[k].id != s.clip && shapes[k].id != s.mask) body += item(shapes, k, indent + 1, prefix);
            }
            return pad(indent) + a + ">\n" + body + pad(indent) + "</g>\n";
        }
        if (s.kind == ShapeKind::Instance) {
            const Matrix m = shapeMatrix(s) * Matrix::translate(s.cx, s.cy);
            return pad(indent) + "<use id=\"" + esc(id) + "\" data-name=\"" + esc(s.name) + "\" href=\"#sym-" + esc(s.symbol) + "\" data-symbol=\"" + esc(s.symbol) +
                   "\" transform=\"" + matrixAttr(m) + "\"" + common(s, id) + "/>\n";
        }
        return leaf(s, id, indent);
    }

    // geometry only, for <clipPath> (which holds shapes, not groups)
    std::string clipShapes(const std::vector<Shape>& shapes, std::size_t i, const std::string& prefix) {
        const Shape& s = shapes[i];
        if (s.kind == ShapeKind::Group) {
            std::string out;
            for (std::size_t k = 0; k < shapes.size(); ++k) {
                if (shapes[k].parent == s.id) out += clipShapes(shapes, k, prefix);
            }
            return out;
        }
        if (s.kind == ShapeKind::Instance) return item(shapes, i, 3, prefix);
        Shape plain = s;
        plain.appearance.clear();
        plain.effects.clear();
        plain.opacity = 1;
        plain.blend = "normal";
        plain.visible = true;
        return leaf(plain, prefix + s.id, 3);
    }

    std::string geometry(const Shape& s, std::string& body) {
        switch (s.kind) {
            case ShapeKind::Rect: {
                std::string el = "<rect x=\"" + num4(std::min(s.x, s.x + s.w)) + "\" y=\"" + num4(std::min(s.y, s.y + s.h)) + "\" width=\"" + num4(std::abs(s.w)) +
                                 "\" height=\"" + num4(std::abs(s.h)) + "\"";
                if (s.radius > 0) el += " rx=\"" + num4(s.radius) + "\"";
                return el;
            }
            case ShapeKind::Ellipse:
                if (s.rx == s.ry) return "<circle cx=\"" + num4(s.cx) + "\" cy=\"" + num4(s.cy) + "\" r=\"" + num4(s.rx) + "\"";
                return "<ellipse cx=\"" + num4(s.cx) + "\" cy=\"" + num4(s.cy) + "\" rx=\"" + num4(s.rx) + "\" ry=\"" + num4(s.ry) + "\"";
            case ShapeKind::Line: {
                const Point a = s.points.size() > 0 ? s.points[0] : Point{};
                const Point b = s.points.size() > 1 ? s.points[1] : a;
                return "<line x1=\"" + num4(a.x) + "\" y1=\"" + num4(a.y) + "\" x2=\"" + num4(b.x) + "\" y2=\"" + num4(b.y) + "\"";
            }
            case ShapeKind::Polyline: return "<polyline points=\"" + pointsAttr(s.points) + "\"";
            case ShapeKind::Polygon: return "<polygon points=\"" + pointsAttr(s.points) + "\"";
            case ShapeKind::Path: return "<path d=\"" + esc(s.d) + "\"";
            case ShapeKind::Text: return text(s, body);
            default: return "<g";
        }
    }

    std::string text(const Shape& s, std::string& body) {
        std::string el = "<text x=\"" + num4(s.x) + "\" y=\"" + num4(s.y) + "\" font-size=\"" + num4(s.fontSize) + "\" font-family=\"" + esc(fontFamily(s)) +
                         "\" data-font=\"" + esc(s.font) + "\"";
        if (s.bold) el += " font-weight=\"bold\"";
        if (s.italic) el += " font-style=\"italic\"";
        if (s.anchor != "start" && s.boxWidth <= 0) el += " text-anchor=\"" + s.anchor + "\"";
        if (s.letterSpacing != 0) el += " letter-spacing=\"" + num4(s.letterSpacing) + "\"";
        if (s.lineHeight != 1.2) el += " data-line-height=\"" + num4(s.lineHeight) + "\"";
        if (s.textCase != "none") el += " data-case=\"" + s.textCase + "\"";
        if (s.textCase == "upper") el += " text-transform=\"uppercase\" style=\"text-transform:uppercase\"";
        if (s.textCase == "small_caps") el += " font-variant=\"small-caps\"";
        if (!s.textStyle.empty()) el += " data-style=\"" + esc(s.textStyle) + "\"";
        if (!s.onPath.empty()) {
            el += " data-on-path=\"" + esc(s.onPath) + "\" data-path-offset=\"" + num4(s.pathOffset) + "\"";
            std::string flat = s.text;
            std::replace(flat.begin(), flat.end(), '\n', ' ');
            body = "<textPath href=\"#" + esc(s.onPath) + "\" startOffset=\"" + num4(s.pathOffset) + "\">" + esc(flat) + "</textPath>";
            return el;
        }
        if (s.boxWidth > 0) {
            el += " data-box-width=\"" + num4(s.boxWidth) + "\" data-box-height=\"" + num4(s.boxHeight) + "\" data-columns=\"" + std::to_string(s.columns) +
                  "\" data-column-gap=\"" + num4(s.columnGap) + "\" data-align=\"" + s.align + "\" data-text=\"" + esc(s.text) + "\"";
            for (const auto& line : detail::layoutArea(s, nullptr)) {
                body += "<tspan x=\"" + num4(line.x) + "\" y=\"" + num4(line.y) + "\"";
                if (line.wordGap > 0) body += " word-spacing=\"" + num4(line.wordGap) + "\"";
                body += ">" + esc(line.text) + "</tspan>";
            }
            return el;
        }
        std::vector<std::string> lines = wrapText(s);
        if (lines.size() == 1) body = esc(lines[0]);
        else {
            for (std::size_t i = 0; i < lines.size(); ++i) {
                body += "<tspan x=\"" + num4(s.x) + "\" dy=\"" + (i ? num4(s.fontSize * s.lineHeight) : std::string("0")) + "\">" + esc(lines[i]) + "</tspan>";
            }
        }
        return el;
    }

    std::string leaf(const Shape& s, const std::string& id, int indent) {
        std::string body;
        const std::string el = geometry(s, body);
        const bool canFill = s.kind != ShapeKind::Line;
        const Bounds box = bounds(localOutline(s, 1.0));
        std::string attrs = " id=\"" + esc(id) + "\" data-name=\"" + esc(s.name) + "\"";
        std::vector<std::string> extrasUnder, extrasOver;
        const std::string tf = transformAttr(s);
        auto extraPath = [&](const std::string& d, const std::string& fillAttr) {
            return "<path data-role=\"extra\" d=\"" + d + "\"" + fillAttr + tf + "/>";
        };
        // fill
        const bool grad = canFill && s.gradient && (!s.gradient->stops.empty() || !s.gradient->points.empty());
        if (grad) {
            if (s.gradient->freeform) defs += freeformDef(*s.gradient, "g-" + id);
            else defs += gradientDef(*s.gradient, "g-" + id);
            attrs += " fill=\"url(#g-" + esc(id) + ")\"";
            if (s.fill) attrs += " data-fill=\"" + detail::colorText(*s.fill) + "\"";
        } else if (canFill && s.pattern) {
            defs += patternDef(*s.pattern, box, s.fill, "p-" + id);
            attrs += " fill=\"url(#p-" + esc(id) + ")\"";
            if (s.fill) attrs += " data-fill=\"" + detail::colorText(*s.fill) + "\"";
        } else {
            attrs += paint("fill", canFill ? s.fill : std::nullopt);
        }
        if (grad && s.pattern) {  // a pattern over a gradient: the marks as a second element
            defs += patternDef(*s.pattern, box, std::nullopt, "p-" + id);
            extrasOver.push_back(extraPath(toPathData(s), " fill=\"url(#p-" + esc(id) + ")\""));
        }
        if (s.pattern) attrs += " data-pattern=\"" + esc(detail::patternText(*s.pattern)) + "\"";
        if (canFill && s.fillRule == "evenodd" && (s.kind == ShapeKind::Path || s.kind == ShapeKind::Polygon || s.kind == ShapeKind::Polyline)) {
            attrs += " fill-rule=\"evenodd\"";
        }
        // stroke
        const bool stroked = ((s.stroke && s.stroke->a > 0) || s.strokeGradient) && s.strokeWidth > 0;
        const bool outlined = stroked && (s.line.profile != "uniform" || (s.line.align != "center" && s.kind != ShapeKind::Line));
        std::string strokePaint;
        if (stroked && s.strokeGradient) {
            defs += gradientDef(*s.strokeGradient, "sg-" + id);
            strokePaint = " fill=\"url(#sg-" + esc(id) + ")\"";
        } else if (stroked) {
            strokePaint = paint("fill", s.stroke);
        }
        if (outlined) {
            attrs += " stroke=\"none\" data-stroke=\"" + (s.stroke ? detail::colorText(*s.stroke) : std::string("none")) + "\" data-stroke-width=\"" +
                     num4(s.strokeWidth) + "\"";
            extrasOver.push_back(extraPath(outlinePathData(outlineStroke(localOutline(s, 4.0), s.strokeWidth, s.line)), strokePaint + " fill-rule=\"nonzero\""));
        } else if (stroked) {
            if (s.strokeGradient) attrs += " stroke=\"url(#sg-" + esc(id) + ")\"";
            else attrs += paint("stroke", s.stroke);
            attrs += " stroke-width=\"" + num4(s.strokeWidth) + "\" stroke-linecap=\"" + s.line.cap + "\" stroke-linejoin=\"" + s.line.join + "\"";
            if (s.line.join == "miter" && s.line.miterLimit != 4) attrs += " stroke-miterlimit=\"" + num4(s.line.miterLimit) + "\"";
            if (!s.line.dash.empty()) {
                attrs += " stroke-dasharray=\"";
                for (std::size_t k = 0; k < s.line.dash.size(); ++k) attrs += (k ? "," : "") + num4(s.line.dash[k]);
                attrs += "\"";
                if (s.line.dashOffset != 0) attrs += " stroke-dashoffset=\"" + num4(s.line.dashOffset) + "\"";
            }
            if (s.line.startArrow != "none") {
                defs += markerDef(s.line.startArrow, "ma-" + id + "-start", s.line.arrowScale);
                attrs += " marker-start=\"url(#ma-" + esc(id) + "-start)\"";
            }
            if (s.line.endArrow != "none") {
                defs += markerDef(s.line.endArrow, "ma-" + id + "-end", s.line.arrowScale);
                attrs += " marker-end=\"url(#ma-" + esc(id) + "-end)\"";
            }
        } else {
            attrs += " stroke=\"none\"";
            if (s.strokeWidth != 1) attrs += " data-stroke-width=\"" + num4(s.strokeWidth) + "\"";
        }
        const std::string options = detail::strokeOptionsText(s.line);
        if (!options.empty()) attrs += " data-stroke-style=\"" + esc(options) + "\"";
        if (!s.fillSwatch.empty()) attrs += " data-swatch-fill=\"" + esc(s.fillSwatch) + "\"";
        if (!s.strokeSwatch.empty()) attrs += " data-swatch-stroke=\"" + esc(s.strokeSwatch) + "\"";
        // the appearance stack
        for (const auto& a : s.appearance) {
            std::string d = toPathData(s);
            std::string look;
            if (a.stroke) look = " fill=\"none\"" + paint("stroke", a.color) + " stroke-width=\"" + num4(a.width) + "\" stroke-linecap=\"" + a.style.cap +
                                 "\" stroke-linejoin=\"" + a.style.join + "\"";
            else look = paint("fill", canFill ? std::optional<Color>(a.color) : std::nullopt);
            if (a.opacity < 1) look += " opacity=\"" + num4(a.opacity) + "\"";
            if (a.blend != "normal") look += " style=\"mix-blend-mode:" + a.blend + "\"";
            std::string t = tf;
            if (a.dx != 0 || a.dy != 0) t = " transform=\"translate(" + num4(a.dx) + " " + num4(a.dy) + ")" + (tf.empty() ? "" : " " + tf.substr(12, tf.size() - 13)) + "\"";
            (a.under ? extrasUnder : extrasOver).push_back("<path data-role=\"extra\" d=\"" + d + "\"" + look + t + "/>");
        }
        if (!s.appearance.empty()) attrs += " data-appearance=\"" + esc(detail::appearanceText(s.appearance)) + "\"";
        const std::string self = el + attrs + tf;
        const std::string closeTag = s.kind == ShapeKind::Text ? ">" + body + "</text>" : "/>";
        if (extrasUnder.empty() && extrasOver.empty()) return pad(indent) + self + common(s, id) + closeTag + "\n";
        std::string out = pad(indent) + "<g data-role=\"appearance\"" + common(s, id) + ">\n";
        for (const auto& e : extrasUnder) out += pad(indent + 1) + e + "\n";
        out += pad(indent + 1) + self + closeTag + "\n";
        for (const auto& e : extrasOver) out += pad(indent + 1) + e + "\n";
        return out + pad(indent) + "</g>\n";
    }
};

}  // namespace

std::string toSvg(const VectorDoc& doc, int artboard) {
    double vx = 0, vy = 0, vw = doc.width, vh = doc.height;
    if (artboard >= 0) {
        const Artboard a = doc.artboard(artboard);
        vx = a.x, vy = a.y, vw = a.w, vh = a.h;
    }
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
           "xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" width=\"" +
           num4(vw) + "\" height=\"" + num4(vh) + "\" viewBox=\"" + num4(vx) + " " + num4(vy) + " " + num4(vw) + " " + num4(vh) + "\"";
    if (artboard >= 0) out += " data-canvas=\"" + std::to_string(doc.width) + "x" + std::to_string(doc.height) + "\"";
    if (!doc.artboards.empty()) {
        std::string a;
        for (const auto& b : doc.artboards) a += (a.empty() ? "" : ";") + b.name + ":" + num4(b.x) + "," + num4(b.y) + "," + num4(b.w) + "," + num4(b.h);
        out += " data-artboards=\"" + esc(a) + "\"";
    }
    auto joinNums = [](const std::vector<double>& v) {
        std::string s;
        for (double x : v) s += (s.empty() ? "" : ",") + num4(x);
        return s;
    };
    if (!doc.guidesX.empty()) out += " data-guides-x=\"" + joinNums(doc.guidesX) + "\"";
    if (!doc.guidesY.empty()) out += " data-guides-y=\"" + joinNums(doc.guidesY) + "\"";
    if (doc.gridSize > 0) out += " data-grid=\"" + num4(doc.gridSize) + "\"";
    if (doc.snapGrid || doc.snapGuides || doc.snapPoints) {
        out += std::string(" data-snap=\"") + (doc.snapGrid ? "grid " : "") + (doc.snapGuides ? "guides " : "") + (doc.snapPoints ? "points" : "") + "\"";
    }
    if (!doc.currentLayer.empty()) out += " data-current-layer=\"" + esc(doc.currentLayer) + "\"";
    if (!doc.swatches.empty()) {
        std::string w;
        for (const auto& [n, c] : doc.swatches) w += (w.empty() ? "" : ";") + n + "=" + detail::colorText(c);
        out += " data-swatches=\"" + esc(w) + "\"";
    }
    if (!doc.textStyles.empty()) {
        std::string t;
        for (const auto& st : doc.textStyles) t += (t.empty() ? "" : ";") + detail::textStyleText(st);
        out += " data-text-styles=\"" + esc(t) + "\"";
    }
    out += " data-next-id=\"" + std::to_string(doc.nextId) + "\">\n";
    if (doc.background.a > 0) {
        out += "  <rect data-role=\"background\" x=\"" + num4(vx) + "\" y=\"" + num4(vy) + "\" width=\"" + num4(vw) + "\" height=\"" + num4(vh) + "\"" +
               paint("fill", doc.background) + "/>\n";
    }
    Writer w(doc);
    std::string symbols;
    for (const auto& sym : doc.symbols) {
        symbols += "    <symbol id=\"sym-" + esc(sym.name) + "\" data-name=\"" + esc(sym.name) + "\" overflow=\"visible\">\n" +
                   w.list(sym.shapes, "", 3, "sym-" + sym.name + "-") + "    </symbol>\n";
    }
    const std::string body = w.list(doc.shapes, "", 1, "");
    const std::string defs = symbols + w.defs;
    if (!defs.empty()) out += "  <defs>\n" + defs + "  </defs>\n";
    out += body + "</svg>\n";
    return out;
}

}  // namespace wetwaresuite::vector
