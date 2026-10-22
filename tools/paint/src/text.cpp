// Text and shape layers: layers that keep what they were made from (the text,
// font, size... or the shape and its colours) in their settings, so they stay
// editable; their pixels are drawn from those settings. Painting on one, or
// any geometry change, turns it into a plain pixel layer.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>

#include "common.hpp"
#include "wetwaresuite/pdfdoc/fonts.hpp"

namespace wetwaresuite::paint {

using namespace raster;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string numText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

double num(const std::map<std::string, std::string>& s, const std::string& key, double fallback) {
    auto it = s.find(key);
    if (it == s.end() || it->second.empty()) return fallback;
    char* end = nullptr;
    const double v = std::strtod(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : v;
}

std::string text(const std::map<std::string, std::string>& s, const std::string& key, const std::string& fallback) {
    auto it = s.find(key);
    return it == s.end() ? fallback : it->second;
}

Color colour(const std::map<std::string, std::string>& s, const std::string& key, const std::string& fallback) {
    auto c = Color::parse(text(s, key, fallback));
    return c ? *c : *Color::parse(fallback);
}

// A setting the create/edit commands take: its param and how it's stored.
struct Field {
    const char* name;
    ParamType type;
    const char* description;
    const char* fallback;
    std::vector<std::string> choices = {};
};

const std::vector<Field>& textFields() {
    static const std::vector<Field> f = {
        {"text", ParamType::String, "the words; \\n starts a new line", ""},
        {"x", ParamType::Number, "left edge (align=center: middle, right: right edge) in px", "10"},
        {"y", ParamType::Number, "top of the first line in px", "10"},
        {"size", ParamType::Number, "font size in px", "32"},
        {"font", ParamType::String, "font: sans (built in), a family like serif, mono, Arial, DejaVu Sans, or a .ttf file", "sans"},
        {"bold", ParamType::Bool, "bold face (system fonts)", "false"},
        {"italic", ParamType::Bool, "italic face (system fonts)", "false"},
        {"color", ParamType::String, "text colour", "#000000"},
        {"align", ParamType::String, "line alignment", "left", {"left", "center", "right"}},
        {"line_height", ParamType::Number, "line spacing, % of the size", "120"},
        {"tracking", ParamType::Number, "extra space between letters, 1/1000 em", "0"},
        {"width", ParamType::Number, "wrap lines at this width in px (0 = only at \\n)", "0"},
    };
    return f;
}

const std::vector<Field>& shapeFields() {
    static const std::vector<Field> f = {
        {"shape", ParamType::String, "which shape", "rect", {"rect", "rounded_rect", "ellipse", "polygon", "star", "line"}},
        {"x", ParamType::Number, "left of the box (line: start x)", "10"},
        {"y", ParamType::Number, "top of the box (line: start y)", "10"},
        {"width", ParamType::Number, "box width (line: dx)", "100"},
        {"height", ParamType::Number, "box height (line: dy)", "100"},
        {"fill", ParamType::String, "fill colour (transparent = none)", "#3b82f6"},
        {"stroke", ParamType::String, "outline colour (transparent = none)", "#00000000"},
        {"stroke_width", ParamType::Number, "outline width in px", "0"},
        {"radius", ParamType::Number, "rounded_rect: corner radius in px", "12"},
        {"sides", ParamType::Int, "polygon: corners; star: points", "5"},
        {"inner", ParamType::Number, "star: inner radius, % of the outer", "45"},
        {"angle", ParamType::Number, "turn the shape, degrees clockwise", "0"},
    };
    return f;
}

std::vector<ParamSpec> fieldSpecs(const std::vector<Field>& fields, bool creating) {
    std::vector<ParamSpec> v = {docParam()};
    if (!creating) v.push_back(layerParam());
    for (const auto& f : fields) {
        const bool required = creating && std::string(f.name) == "text";
        v.push_back({f.name, f.type, f.description, required, f.choices, {}});
    }
    if (creating) v.push_back({"name", ParamType::String, "layer name", false});
    v.push_back(previewParam());
    return v;
}

// Copies the given params into a layer's settings.
void storeFields(const std::vector<Field>& fields, const Params& p, std::map<std::string, std::string>& s, bool creating) {
    for (const auto& f : fields) {
        if (p.count(f.name)) {
            if (f.type == ParamType::Number) s[f.name] = numText(numberParam(p, f.name));
            else if (f.type == ParamType::Int) s[f.name] = std::to_string(intParam(p, f.name));
            else if (f.type == ParamType::Bool) s[f.name] = boolParam(p, f.name) ? "true" : "false";
            else s[f.name] = stringParam(p, f.name);
        } else if (creating && !s.count(f.name)) {
            s[f.name] = f.fallback;
        }
    }
}

std::string checkColours(const std::map<std::string, std::string>& s, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (s.count(k) && !Color::parse(s.at(k))) return std::string(k) + ": '" + s.at(k) + "' isn't a colour";
    }
    return "";
}

std::shared_ptr<pdfdoc::TrueType> findFont(const std::string& name, bool bold, bool italic, std::string& note) {
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<pdfdoc::TrueType>> cache;
    const std::string key = name + (bold ? "|b" : "") + (italic ? "|i" : "");
    std::lock_guard<std::mutex> guard(lock);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::shared_ptr<pdfdoc::TrueType> font;
    if (name.empty() || name == "sans" || name == "default" || name == "roboto") {
        font = bold || italic ? pdfdoc::systemFont("sans", bold, italic) : nullptr;
    } else {
        std::error_code ec;
        if (std::filesystem::exists(name, ec)) {
            try {
                font = pdfdoc::TrueType::load(name);
            } catch (const std::exception& e) {
                note = std::string("couldn't read ") + name + ": " + e.what() + "; using the built-in font";
            }
        } else {
            font = pdfdoc::systemFont(name, bold, italic);
            if (!font) note = "no font '" + name + "' on this computer; using the built-in font";
        }
    }
    if (!font) font = pdfdoc::builtinSans();
    cache[key] = font;
    return font;
}

// Splits into lines at \n (typed as a real newline or the two characters \ n) and wraps at `width`.
std::vector<std::vector<unsigned>> layoutLines(const pdfdoc::TrueType& font, const std::string& raw, double size, double tracking, double width) {
    std::string s;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'n') {
            s += '\n';
            ++i;
        } else {
            s += raw[i];
        }
    }
    std::vector<std::vector<unsigned>> lines(1);
    for (unsigned c : pdfdoc::utf8Decode(s)) {
        if (c == '\n') lines.emplace_back();
        else if (c != '\r') lines.back().push_back(c);
    }
    if (width <= 0) return lines;
    auto lineWidth = [&](const std::vector<unsigned>& l) {
        double w = 0;
        for (unsigned c : l) w += (font.advance(c) + tracking) * size / 1000;
        return w;
    };
    std::vector<std::vector<unsigned>> out;
    for (const auto& line : lines) {
        std::vector<unsigned> cur;
        std::size_t lastSpace = std::string::npos;
        for (unsigned c : line) {
            cur.push_back(c);
            if (c == ' ') lastSpace = cur.size() - 1;
            if (lineWidth(cur) > width && cur.size() > 1) {
                if (lastSpace != std::string::npos && lastSpace > 0) {
                    out.emplace_back(cur.begin(), cur.begin() + static_cast<std::ptrdiff_t>(lastSpace));
                    cur.erase(cur.begin(), cur.begin() + static_cast<std::ptrdiff_t>(lastSpace) + 1);
                } else {
                    out.emplace_back(cur.begin(), cur.end() - 1);
                    cur.erase(cur.begin(), cur.end() - 1);
                }
                lastSpace = std::string::npos;
                for (std::size_t i = 0; i < cur.size(); ++i) {
                    if (cur[i] == ' ') lastSpace = i;
                }
            }
        }
        out.push_back(cur);
    }
    return out;
}

// Draws a text layer's settings onto a transparent canvas-sized image. `note` explains font fallbacks.
Image renderText(const std::map<std::string, std::string>& s, int width, int height, std::string& note) {
    Image img(width, height);
    const double size = std::max(1.0, num(s, "size", 32));
    auto font = findFont(text(s, "font", "sans"), text(s, "bold", "false") == "true", text(s, "italic", "false") == "true", note);
    const double tracking = num(s, "tracking", 0), boxW = num(s, "width", 0);
    const auto lines = layoutLines(*font, text(s, "text", ""), size, tracking, boxW);
    const pdfdoc::VMetrics m = font->metrics();
    const double lineH = size * num(s, "line_height", 120) / 100;
    const double x0 = num(s, "x", 10), y0 = num(s, "y", 10);
    const std::string align = text(s, "align", "left");
    std::vector<std::vector<Point>> contours;
    double baseline = y0 + m.ascent * size / 1000;
    for (const auto& line : lines) {
        double w = 0;
        for (std::size_t i = 0; i < line.size(); ++i) {
            w += (font->advance(line[i]) + tracking) * size / 1000;
            if (i + 1 < line.size()) w += font->kern(line[i], line[i + 1]) * size / 1000;
        }
        double pen = x0;
        if (align == "center") pen = boxW > 0 ? x0 + (boxW - w) / 2 : x0 - w / 2;
        else if (align == "right") pen = boxW > 0 ? x0 + boxW - w : x0 - w;
        for (std::size_t i = 0; i < line.size(); ++i) {
            for (const auto& c : font->outline(line[i])) {
                std::vector<Point> pts;
                pts.reserve(c.size());
                for (const auto& q : c) pts.push_back({pen + q.x * size / 1000, baseline - q.y * size / 1000});
                contours.push_back(std::move(pts));
            }
            pen += (font->advance(line[i]) + tracking) * size / 1000;
            if (i + 1 < line.size()) pen += font->kern(line[i], line[i + 1]) * size / 1000;
        }
        baseline += lineH;
    }
    fillPolygons(img, contours, colour(s, "color", "#000000"), true);
    return img;
}

// The outline of a shape layer's shape, as one closed contour (or an open line).
std::vector<Point> shapeOutline(const std::map<std::string, std::string>& s) {
    const std::string kind = text(s, "shape", "rect");
    const double x = num(s, "x", 10), y = num(s, "y", 10), w = num(s, "width", 100), h = num(s, "height", 100);
    std::vector<Point> pts;
    if (kind == "line") return {{x, y}, {x + w, y + h}};
    const double cx = x + w / 2, cy = y + h / 2, rx = std::fabs(w) / 2, ry = std::fabs(h) / 2;
    if (kind == "ellipse") {
        pts = ellipsePoints(cx, cy, rx, ry);
    } else if (kind == "rounded_rect") {
        const double r = std::clamp(num(s, "radius", 12), 0.0, std::min(rx, ry));
        const double l = cx - rx, t = cy - ry, rr = cx + rx, b = cy + ry;
        const double corners[4][3] = {{rr - r, t + r, -kPi / 2}, {rr - r, b - r, 0}, {l + r, b - r, kPi / 2}, {l + r, t + r, kPi}};
        const int steps = std::clamp(static_cast<int>(r), 2, 32);
        for (const auto& c : corners) {
            for (int i = 0; i <= steps; ++i) {
                const double a = c[2] + (kPi / 2) * i / steps;
                pts.push_back({c[0] + r * std::cos(a), c[1] + r * std::sin(a)});
            }
        }
    } else if (kind == "polygon" || kind == "star") {
        const int n = std::clamp(static_cast<int>(num(s, "sides", 5)), 3, 200);
        const bool star = kind == "star";
        const double inner = std::clamp(num(s, "inner", 45), 1.0, 100.0) / 100;
        const int count = star ? 2 * n : n;
        for (int i = 0; i < count; ++i) {
            const double a = -kPi / 2 + 2 * kPi * i / count;
            const double f = star && i % 2 ? inner : 1.0;
            pts.push_back({cx + rx * f * std::cos(a), cy + ry * f * std::sin(a)});
        }
    } else {
        pts = {{cx - rx, cy - ry}, {cx + rx, cy - ry}, {cx + rx, cy + ry}, {cx - rx, cy + ry}};
    }
    const double angle = num(s, "angle", 0) * kPi / 180;
    if (angle != 0) {
        const double c = std::cos(angle), sn = std::sin(angle);
        for (auto& q : pts) {
            const double dx = q.x - cx, dy = q.y - cy;
            q = {cx + dx * c - dy * sn, cy + dx * sn + dy * c};
        }
    }
    return pts;
}

Image renderShape(const std::map<std::string, std::string>& s, int width, int height) {
    Image img(width, height);
    const std::vector<Point> pts = shapeOutline(s);
    const bool line = text(s, "shape", "rect") == "line";
    const Color fill = colour(s, "fill", "#3b82f6"), edge = colour(s, "stroke", "#00000000");
    const double sw = num(s, "stroke_width", 0);
    if (!line && fill.a > 0) fillPolygons(img, {pts}, fill, false);
    if ((sw > 0 && edge.a > 0) || line) {
        Brush b;
        b.color = line && (sw <= 0 || edge.a == 0) ? fill : edge;
        b.size = std::max(1.0, line && sw <= 0 ? 2.0 : sw);
        b.hardness = 1.0;
        b.spacing = 0.05;
        b.pressureSize = false;
        std::vector<StrokePoint> sp;
        for (const auto& q : pts) sp.push_back({q.x, q.y, 1.0});
        if (!line && !pts.empty()) sp.push_back({pts[0].x, pts[0].y, 1.0});
        stroke(img, b, sp);
    }
    return img;
}

Result textLayer(ImageDoc& doc, const Params& p, bool creating) {
    std::string error, note;
    int idx = -1;
    std::map<std::string, std::string> settings;
    if (!creating) {
        idx = layerIndex(doc, p, error);
        if (idx < 0) return Result::failure(error);
        const Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
        if (l.kind != LayerKind::Text) {
            return Result::failure("layer " + std::to_string(idx) + " isn't a text layer" +
                                   (l.kind == LayerKind::Pixel ? " (it may have been rasterized)" : ""));
        }
        settings = l.settings;
    }
    storeFields(textFields(), p, settings, creating);
    settings["type"] = "text";
    if (std::string e = checkColours(settings, {"color"}); !e.empty()) return Result::failure(e);
    if (text(settings, "text", "").empty()) return Result::failure("text is empty");
    Image pixels = renderText(settings, doc.image.width, doc.image.height, note);
    if (creating) {
        std::string label = text(settings, "text", "");
        if (label.size() > 24) label = label.substr(0, 24) + "...";
        Layer l(stringParam(p, "name", label));
        l.kind = LayerKind::Text;
        l.settings = std::move(settings);
        l.image = std::move(pixels);
        idx = insertLayer(doc, std::move(l));
    } else {
        Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
        l.settings = std::move(settings);
        l.image = std::move(pixels);
        doc.active = idx;
    }
    Result r = done(std::string(creating ? "added text layer " : "updated text layer ") + std::to_string(idx) + (note.empty() ? "" : " (" + note + ")"), doc, p);
    return r;
}

Result shapeLayer(ImageDoc& doc, const Params& p, bool creating) {
    std::string error;
    int idx = -1;
    std::map<std::string, std::string> settings;
    if (!creating) {
        idx = layerIndex(doc, p, error);
        if (idx < 0) return Result::failure(error);
        const Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
        if (l.kind != LayerKind::Shape) return Result::failure("layer " + std::to_string(idx) + " isn't a shape layer");
        settings = l.settings;
    }
    storeFields(shapeFields(), p, settings, creating);
    settings["type"] = "shape";
    if (std::string e = checkColours(settings, {"fill", "stroke"}); !e.empty()) return Result::failure(e);
    Image pixels = renderShape(settings, doc.image.width, doc.image.height);
    if (creating) {
        Layer l(stringParam(p, "name", "Shape " + text(settings, "shape", "rect")));
        l.kind = LayerKind::Shape;
        l.settings = std::move(settings);
        l.image = std::move(pixels);
        idx = insertLayer(doc, std::move(l));
    } else {
        Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
        l.settings = std::move(settings);
        l.image = std::move(pixels);
        doc.active = idx;
    }
    return done(std::string(creating ? "added shape layer " : "updated shape layer ") + std::to_string(idx), doc, p);
}

}  // namespace

void registerTextCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"paint.text",
             "Text layer",
             "Add a text layer above the active layer. It stays editable with paint.text_edit (words, font, size, colour, "
             "alignment, spacing); painting on it or transforming the image turns it into pixels. Fonts: sans is built in "
             "(the same everywhere); other names are looked up on this computer.",
             fieldSpecs(textFields(), true), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) { return textLayer(doc, p, true); }));
    reg.add({"paint.text_edit", "Edit text", "Change a text layer: only the settings you give change.", fieldSpecs(textFields(), false), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) { return textLayer(doc, p, false); }));
    reg.add({"paint.shape_layer",
             "Shape layer",
             "Add an editable shape layer (rect, rounded_rect, ellipse, polygon, star, line) with a fill and an outline. "
             "Change it later with paint.shape_edit; it stays sharp because it's redrawn from its settings.",
             fieldSpecs(shapeFields(), true), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) { return shapeLayer(doc, p, true); }));
    reg.add({"paint.shape_edit", "Edit shape", "Change a shape layer: only the settings you give change.", fieldSpecs(shapeFields(), false), true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) { return shapeLayer(doc, p, false); }));
}

}  // namespace wetwaresuite::paint
