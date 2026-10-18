// SVG export and a small, forgiving SVG importer (no XML library: a minimal
// tag/attribute reader is all the shape subset needs).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string ext(const std::filesystem::path& path) {
    std::string e = lower(path.extension().string());
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return e;
}

// ---- writing ----------------------------------------------------------------

std::string num(double v) {
    if (std::abs(v) < 5e-5) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
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

std::string paintAttrs(const char* name, const std::optional<Color>& c) {
    if (!c || c->a == 0) return std::string(" ") + name + "=\"none\"";
    std::string out = std::string(" ") + name + "=\"" + rgb(*c) + "\"";
    if (c->a < 255) out += std::string(" ") + name + "-opacity=\"" + num(c->a / 255.0) + "\"";
    return out;
}

std::string pointsAttr(const std::vector<Point>& pts) {
    std::string out;
    for (const auto& p : pts) out += (out.empty() ? "" : " ") + num(p.x) + "," + num(p.y);
    return out;
}

}  // namespace

std::optional<Color> parseColor(const std::string& text) {
    if (auto c = Color::parse(text)) return c;
    std::string s = lower(trim(text));
    static const std::map<std::string, Color> extra = {
        {"navy", {0, 0, 128, 255}},       {"teal", {0, 128, 128, 255}},     {"olive", {128, 128, 0, 255}},
        {"maroon", {128, 0, 0, 255}},     {"silver", {192, 192, 192, 255}}, {"gold", {255, 215, 0, 255}},
        {"indigo", {75, 0, 130, 255}},    {"violet", {238, 130, 238, 255}}, {"crimson", {220, 20, 60, 255}},
        {"coral", {255, 127, 80, 255}},   {"salmon", {250, 128, 114, 255}}, {"tomato", {255, 99, 71, 255}},
        {"darkgray", {169, 169, 169, 255}}, {"darkgrey", {169, 169, 169, 255}}, {"lightgray", {211, 211, 211, 255}},
        {"lightgrey", {211, 211, 211, 255}}, {"darkblue", {0, 0, 139, 255}}, {"darkred", {139, 0, 0, 255}},
        {"darkgreen", {0, 100, 0, 255}},  {"skyblue", {135, 206, 235, 255}}, {"steelblue", {70, 130, 180, 255}},
        {"aqua", {0, 255, 255, 255}},     {"fuchsia", {255, 0, 255, 255}},  {"beige", {245, 245, 220, 255}},
        {"khaki", {240, 230, 140, 255}},  {"turquoise", {64, 224, 208, 255}}, {"chocolate", {210, 105, 30, 255}},
        {"darkorange", {255, 140, 0, 255}}, {"hotpink", {255, 105, 180, 255}}, {"slategray", {112, 128, 144, 255}},
        {"midnightblue", {25, 25, 112, 255}}, {"forestgreen", {34, 139, 34, 255}}, {"royalblue", {65, 105, 225, 255}},
        {"limegreen", {50, 205, 50, 255}}, {"lavender", {230, 230, 250, 255}}, {"ivory", {255, 255, 240, 255}},
    };
    if (auto it = extra.find(s); it != extra.end()) return it->second;
    return std::nullopt;
}

namespace {

// ---- reading: a minimal XML tree ----------------------------------------------

struct Element {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::vector<Element> children;
    std::string text;  // character data directly inside, joined
    const std::string* attr(const std::string& key) const {
        auto it = attrs.find(key);
        return it == attrs.end() ? nullptr : &it->second;
    }
};

std::string decodeEntities(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            long code = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtol(ent.c_str() + 2, nullptr, 16)
                                                                            : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (code > 0 && code < 128) out += static_cast<char>(code);
            else out += '?';
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

class XmlReader {
public:
    explicit XmlReader(const std::string& text) : s_(text) {}

    Element parse() {
        Element root;  // synthetic document node
        std::vector<Element*> stack{&root};
        while (i_ < s_.size()) {
            auto lt = s_.find('<', i_);
            if (stack.size() > 1 && lt != i_) stack.back()->text += decodeEntities(s_.substr(i_, lt == std::string::npos ? std::string::npos : lt - i_));
            if (lt == std::string::npos) break;
            i_ = lt;
            if (starts("<?")) skipPast("?>");
            else if (starts("<!--")) skipPast("-->");
            else if (starts("<![CDATA[")) skipPast("]]>");
            else if (starts("<!")) skipDeclaration();
            else if (starts("</")) {
                skipPast(">");
                if (stack.size() > 1) stack.pop_back();
            } else {
                bool selfClosing = false;
                Element el = tag(selfClosing);
                if (el.name.empty()) {
                    ++i_;
                    continue;
                }
                stack.back()->children.push_back(std::move(el));
                if (!selfClosing) stack.push_back(&stack.back()->children.back());
            }
        }
        return root;
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;

    bool starts(const char* p) const { return s_.compare(i_, std::char_traits<char>::length(p), p) == 0; }
    void skipPast(const char* end) {
        auto at = s_.find(end, i_);
        i_ = at == std::string::npos ? s_.size() : at + std::char_traits<char>::length(end);
    }
    void skipDeclaration() {
        int depth = 0;
        for (; i_ < s_.size(); ++i_) {
            if (s_[i_] == '[') ++depth;
            else if (s_[i_] == ']') --depth;
            else if (s_[i_] == '>' && depth <= 0) {
                ++i_;
                return;
            }
        }
    }
    static bool nameChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '-' || c == '_' || c == '.';
    }
    void spaces() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    std::string name() {
        std::size_t b = i_;
        while (i_ < s_.size() && nameChar(s_[i_])) ++i_;
        return s_.substr(b, i_ - b);
    }

    Element tag(bool& selfClosing) {
        Element el;
        ++i_;  // '<'
        el.name = name();
        if (auto colon = el.name.find(':'); colon != std::string::npos) el.name = el.name.substr(colon + 1);  // svg:rect
        while (i_ < s_.size()) {
            spaces();
            if (i_ >= s_.size()) break;
            if (s_[i_] == '>') {
                ++i_;
                return el;
            }
            if (starts("/>")) {
                i_ += 2;
                selfClosing = true;
                return el;
            }
            std::string key = name();
            if (key.empty()) {
                ++i_;
                continue;
            }
            spaces();
            std::string value;
            if (i_ < s_.size() && s_[i_] == '=') {
                ++i_;
                spaces();
                if (i_ < s_.size() && (s_[i_] == '"' || s_[i_] == '\'')) {
                    char q = s_[i_++];
                    auto end = s_.find(q, i_);
                    if (end == std::string::npos) end = s_.size();
                    value = s_.substr(i_, end - i_);
                    i_ = std::min(s_.size(), end + 1);
                } else {
                    std::size_t b = i_;
                    while (i_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i_])) && s_[i_] != '>') ++i_;
                    value = s_.substr(b, i_ - b);
                }
            }
            el.attrs[key] = decodeEntities(value);
        }
        return el;
    }
};

// ---- reading: values ---------------------------------------------------------------

std::vector<double> numbers(const std::string& s) {
    std::vector<double> out;
    const char* p = s.c_str();
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) break;
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(v);
        p = end;
    }
    return out;
}

double length(const std::string* text, double reference, double fallback = 0) {
    if (!text) return fallback;
    std::string s = trim(*text);
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return fallback;
    std::string unit = lower(trim(end));
    if (unit == "%") return v * reference / 100;
    if (unit == "pt") return v * 4.0 / 3.0;
    if (unit == "pc") return v * 16;
    if (unit == "mm") return v * 96 / 25.4;
    if (unit == "cm") return v * 96 / 2.54;
    if (unit == "in") return v * 96;
    if (unit == "em") return v * 16;
    return v;
}

// none -> {nullopt}; a colour -> {colour}; unreadable -> nullopt (keep the inherited value)
std::optional<std::optional<Color>> paint(const std::string& raw) {
    std::string s = lower(trim(raw));
    if (s == "none" || s == "transparent") return std::optional<Color>{};
    if (s == "currentcolor") return std::optional<Color>{Color{0, 0, 0, 255}};
    if (s.rfind("url(", 0) == 0) {
        // gradients and patterns aren't supported: use the fallback colour after the url, else grey
        auto close = s.find(')');
        if (close != std::string::npos && close + 1 < s.size()) return paint(s.substr(close + 1));
        return std::optional<Color>{Color{128, 128, 128, 255}};
    }
    if (auto c = parseColor(s)) return std::optional<Color>{*c};
    return std::nullopt;
}

Matrix parseTransform(const std::string& text) {
    Matrix m;
    std::size_t i = 0;
    while (i < text.size()) {
        auto open = text.find('(', i);
        if (open == std::string::npos) break;
        auto close = text.find(')', open);
        if (close == std::string::npos) break;
        std::string fn = trim(text.substr(i, open - i));
        while (!fn.empty() && (fn.front() == ',' || std::isspace(static_cast<unsigned char>(fn.front())))) fn.erase(0, 1);
        std::vector<double> a = numbers(text.substr(open + 1, close - open - 1));
        i = close + 1;
        auto arg = [&](std::size_t k, double fallback) { return k < a.size() ? a[k] : fallback; };
        Matrix t;
        if (fn == "matrix" && a.size() >= 6) t = {a[0], a[1], a[2], a[3], a[4], a[5]};
        else if (fn == "translate") t = Matrix::translate(arg(0, 0), arg(1, 0));
        else if (fn == "scale") t = Matrix::scale(arg(0, 1), arg(1, arg(0, 1)));
        else if (fn == "rotate") {
            double cx = arg(1, 0), cy = arg(2, 0);
            t = Matrix::translate(cx, cy) * Matrix::rotate(arg(0, 0)) * Matrix::translate(-cx, -cy);
        } else if (fn == "skewX") t = {1, 0, std::tan(arg(0, 0) * kPi / 180), 1, 0, 0};
        else if (fn == "skewY") t = {1, std::tan(arg(0, 0) * kPi / 180), 0, 1, 0, 0};
        m = m * t;
    }
    return m;
}

// Splits a matrix into the shape model: rotate + scale about the local centre, then translate.
// Skew can't be represented and is dropped.
Transform decompose(const Matrix& m, Point centre) {
    Transform t;
    t.sx = std::hypot(m.a, m.b);
    t.rotate = t.sx > 0 ? std::atan2(m.b, m.a) * 180 / kPi : 0;
    const double r = t.rotate * kPi / 180;
    t.sy = -m.c * std::sin(r) + m.d * std::cos(r);
    const Matrix lin = Matrix::rotate(t.rotate) * Matrix::scale(t.sx, t.sy);
    const Point ac = lin.apply(centre);
    t.tx = m.e - centre.x + ac.x;
    t.ty = m.f - centre.y + ac.y;
    auto snap = [](double& v, double target) {
        if (std::abs(v - target) < 1e-9) v = target;
    };
    snap(t.sx, 1);
    snap(t.sy, 1);
    snap(t.rotate, 0);
    snap(t.tx, 0);
    snap(t.ty, 0);
    return t;
}

struct Style {
    std::optional<Color> fill = Color{0, 0, 0, 255};
    std::optional<Color> stroke;
    double strokeWidth = 1;
    double fillOpacity = 1, strokeOpacity = 1;
    double opacity = 1;  // product of group opacities
    bool hidden = false;
    Matrix matrix;
    std::string fillUrl;  // fill="url(#id)": a gradient
};

double clamp01(const std::string& s) {
    auto v = numbers(s);
    if (v.empty()) return 1;
    double x = v[0];
    if (s.find('%') != std::string::npos) x /= 100;
    return std::clamp(x, 0.0, 1.0);
}

class Importer {
public:
    explicit Importer(VectorDoc& doc) : doc_(doc) {}

    void root(const Element& svg) {
        const std::string* vb = svg.attr("viewBox");
        std::vector<double> box = vb ? numbers(*vb) : std::vector<double>{};
        bool hasBox = box.size() == 4 && box[2] > 0 && box[3] > 0;
        double w = length(svg.attr("width"), hasBox ? box[2] : 800, hasBox ? box[2] : 800);
        double h = length(svg.attr("height"), hasBox ? box[3] : 600, hasBox ? box[3] : 600);
        doc_.width = std::max(1, static_cast<int>(std::lround(w)));
        doc_.height = std::max(1, static_cast<int>(std::lround(h)));
        doc_.background = {0, 0, 0, 0};
        Style style;
        if (hasBox) {
            style.matrix = Matrix::scale(w / box[2], h / box[3]) * Matrix::translate(-box[0], -box[1]);
            refW_ = box[2];
            refH_ = box[3];
        } else {
            refW_ = w;
            refH_ = h;
        }
        collectGradients(svg);
        children(svg, style);
    }

private:
    VectorDoc& doc_;
    struct GradientDef {
        Gradient g;
        bool userSpace = false;
        std::string href;
        bool hasStops = false;
    };
    std::map<std::string, GradientDef> gradients_;

    static double fraction(const std::string* v, double fallback) {
        if (!v) return fallback;
        auto n = numbers(*v);
        if (n.empty()) return fallback;
        return v->find('%') != std::string::npos ? n[0] / 100 : n[0];
    }

    void collectGradients(const Element& el) {
        for (const auto& c : el.children) {
            if (c.name == "linearGradient" || c.name == "radialGradient") {
                GradientDef def;
                def.g.radial = c.name == "radialGradient";
                def.userSpace = c.attr("gradientUnits") && *c.attr("gradientUnits") == "userSpaceOnUse";
                def.g.x1 = fraction(c.attr("x1"), 0);
                def.g.y1 = fraction(c.attr("y1"), 0);
                def.g.x2 = fraction(c.attr("x2"), def.userSpace ? 0 : 1);
                def.g.y2 = fraction(c.attr("y2"), 0);
                def.g.cx = fraction(c.attr("cx"), 0.5);
                def.g.cy = fraction(c.attr("cy"), 0.5);
                def.g.r = fraction(c.attr("r"), 0.5);
                if (auto h = c.attr("xlink:href")) def.href = *h;
                else if (auto h2 = c.attr("href")) def.href = *h2;
                if (!def.href.empty() && def.href[0] == '#') def.href.erase(0, 1);
                for (const auto& stop : c.children) {
                    if (stop.name != "stop") continue;
                    std::map<std::string, std::string> props;
                    for (const char* k : {"offset", "stop-color", "stop-opacity"}) {
                        if (auto v = stop.attr(k)) props[k] = *v;
                    }
                    if (auto st = stop.attr("style")) {
                        std::stringstream ss(*st);
                        std::string decl;
                        while (std::getline(ss, decl, ';')) {
                            auto colon = decl.find(':');
                            if (colon != std::string::npos) props[lower(trim(decl.substr(0, colon)))] = trim(decl.substr(colon + 1));
                        }
                    }
                    GradientStop gs;
                    gs.offset = props.count("offset") ? std::clamp(fraction(&props["offset"], 0), 0.0, 1.0) : 0;
                    gs.color = props.count("stop-color") ? parseColor(props["stop-color"]).value_or(Color{0, 0, 0, 255}) : Color{0, 0, 0, 255};
                    if (props.count("stop-opacity")) gs.color.a = static_cast<std::uint8_t>(std::lround(gs.color.a * clamp01(props["stop-opacity"])));
                    if (!def.g.stops.empty()) gs.offset = std::max(gs.offset, def.g.stops.back().offset);
                    def.g.stops.push_back(gs);
                }
                def.hasStops = !def.g.stops.empty();
                if (auto id = c.attr("id")) gradients_[*id] = def;
            }
            collectGradients(c);
        }
        // stops borrowed through href
        for (auto& [id, def] : gradients_) {
            std::string ref = def.href;
            for (int depth = 0; !def.hasStops && !ref.empty() && depth < 8; ++depth) {
                auto it = gradients_.find(ref);
                if (it == gradients_.end()) break;
                if (it->second.hasStops) {
                    def.g.stops = it->second.g.stops;
                    def.hasStops = true;
                }
                ref = it->second.href;
            }
        }
    }

    static std::string allText(const Element& el) {
        std::string t = el.text;
        for (const auto& c : el.children) t += allText(c);
        return t;
    }

    static std::string collapse(const std::string& t) {
        std::string out;
        bool space = false;
        for (char ch : t) {
            if (std::isspace(static_cast<unsigned char>(ch))) {
                space = !out.empty();
                continue;
            }
            if (space) out += ' ';
            space = false;
            out += ch;
        }
        return out;
    }
    double refW_ = 800, refH_ = 600;

    void children(const Element& el, const Style& style) {
        for (const auto& c : el.children) element(c, style);
    }

    Style resolve(const Element& el, const Style& parent) {
        Style s = parent;
        std::map<std::string, std::string> props;
        for (const char* key : {"fill", "stroke", "stroke-width", "fill-opacity", "stroke-opacity", "opacity", "display", "visibility"}) {
            if (auto v = el.attr(key)) props[key] = *v;
        }
        if (auto style = el.attr("style")) {
            std::stringstream ss(*style);
            std::string decl;
            while (std::getline(ss, decl, ';')) {
                auto colon = decl.find(':');
                if (colon == std::string::npos) continue;
                props[lower(trim(decl.substr(0, colon)))] = trim(decl.substr(colon + 1));
            }
        }
        for (const auto& [key, value] : props) {
            if (key == "fill") {
                const std::string v = trim(value);
                if (v.rfind("url(", 0) == 0) {
                    auto hash = v.find('#'), close = v.find(')');
                    s.fillUrl = hash != std::string::npos && close != std::string::npos && close > hash ? v.substr(hash + 1, close - hash - 1) : "";
                } else {
                    s.fillUrl.clear();
                }
                if (auto p = paint(value)) s.fill = *p;
            } else if (key == "stroke") {
                if (auto p = paint(value)) s.stroke = *p;
            } else if (key == "stroke-width") {
                s.strokeWidth = std::max(0.0, length(&value, std::hypot(refW_, refH_) / std::sqrt(2.0), s.strokeWidth));
            } else if (key == "fill-opacity") {
                s.fillOpacity = clamp01(value);
            } else if (key == "stroke-opacity") {
                s.strokeOpacity = clamp01(value);
            } else if (key == "opacity") {
                s.opacity *= clamp01(value);
            } else if (key == "display") {
                if (trim(value) == "none") s.hidden = true;
            } else if (key == "visibility") {
                std::string v = trim(value);
                if (v == "hidden" || v == "collapse") s.hidden = true;
                else if (v == "visible") s.hidden = false;
            }
        }
        if (auto t = el.attr("transform")) s.matrix = s.matrix * parseTransform(*t);
        return s;
    }

    static bool skipped(const std::string& name) {
        static const char* names[] = {"defs", "clipPath", "mask", "pattern", "marker", "symbol", "linearGradient",
                                      "radialGradient", "style", "script", "metadata", "title", "desc", "filter",
                                      "image", "use", "foreignObject", "namedview"};
        for (const char* n : names) {
            if (name == n) return true;
        }
        return false;
    }

    double len(const Element& el, const char* key, double ref) const { return length(el.attr(key), ref, 0); }

    void element(const Element& el, const Style& parent) {
        if (skipped(el.name)) return;
        Style st = resolve(el, parent);
        if (el.name == "g" || el.name == "svg" || el.name == "a" || el.name == "switch") {
            children(el, st);
            return;
        }
        Shape s;
        const double diag = std::hypot(refW_, refH_) / std::sqrt(2.0);
        if (el.name == "rect") {
            s.kind = ShapeKind::Rect;
            s.x = len(el, "x", refW_);
            s.y = len(el, "y", refH_);
            s.w = len(el, "width", refW_);
            s.h = len(el, "height", refH_);
            s.radius = el.attr("rx") ? len(el, "rx", refW_) : len(el, "ry", refH_);
            if (s.w <= 0 || s.h <= 0) return;
            if (auto role = el.attr("data-role"); role && *role == "background") {
                Color c = st.fill.value_or(Color{0, 0, 0, 0});
                c.a = static_cast<std::uint8_t>(std::lround(c.a * st.fillOpacity * st.opacity));
                doc_.background = c;
                return;
            }
        } else if (el.name == "circle") {
            s.kind = ShapeKind::Ellipse;
            s.cx = len(el, "cx", refW_);
            s.cy = len(el, "cy", refH_);
            s.rx = s.ry = len(el, "r", diag);
            if (s.rx <= 0) return;
        } else if (el.name == "ellipse") {
            s.kind = ShapeKind::Ellipse;
            s.cx = len(el, "cx", refW_);
            s.cy = len(el, "cy", refH_);
            s.rx = len(el, "rx", refW_);
            s.ry = len(el, "ry", refH_);
            if (s.rx <= 0 || s.ry <= 0) return;
        } else if (el.name == "line") {
            s.kind = ShapeKind::Line;
            s.points = {{len(el, "x1", refW_), len(el, "y1", refH_)}, {len(el, "x2", refW_), len(el, "y2", refH_)}};
        } else if (el.name == "polyline" || el.name == "polygon") {
            s.kind = el.name == "polygon" ? ShapeKind::Polygon : ShapeKind::Polyline;
            auto v = el.attr("points") ? numbers(*el.attr("points")) : std::vector<double>{};
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) s.points.push_back({v[i], v[i + 1]});
            if (s.points.size() < 2) return;
        } else if (el.name == "text") {
            s.kind = ShapeKind::Text;
            auto xs = el.attr("x") ? numbers(*el.attr("x")) : std::vector<double>{};
            auto ys = el.attr("y") ? numbers(*el.attr("y")) : std::vector<double>{};
            s.x = xs.empty() ? 0 : xs[0];
            s.y = ys.empty() ? 0 : ys[0];
            s.fontSize = el.attr("font-size") ? length(el.attr("font-size"), diag, 16) : 16;
            if (auto style = el.attr("style")) {
                auto at = style->find("font-size:");
                if (at != std::string::npos) {
                    std::string v = style->substr(at + 10);
                    s.fontSize = length(&v, diag, s.fontSize);
                }
            }
            if (auto f = el.attr("data-font")) s.font = *f;
            else if (auto fam = el.attr("font-family")) {
                const std::string f = lower(*fam);
                s.font = f.find("mono") != std::string::npos || f.find("courier") != std::string::npos ? "mono"
                         : (f.find("serif") != std::string::npos && f.find("sans") == std::string::npos) || f.find("times") != std::string::npos ? "serif"
                                                                                                                                         : "sans";
            }
            if (auto w = el.attr("font-weight")) s.bold = *w == "bold" || *w == "bolder" || std::atoi(w->c_str()) >= 600;
            if (auto it = el.attr("font-style")) s.italic = *it == "italic" || *it == "oblique";
            if (auto a = el.attr("text-anchor")) s.anchor = *a == "middle" || *a == "end" ? *a : "start";
            if (auto ls = el.attr("letter-spacing")) s.letterSpacing = length(ls, diag, 0);
            if (auto lh = el.attr("data-line-height")) s.lineHeight = std::max(0.1, std::atof(lh->c_str()));
            std::vector<std::string> lines;
            for (const auto& c : el.children) {
                if (c.name == "tspan") lines.push_back(collapse(allText(c)));
            }
            if (lines.empty()) lines.push_back(collapse(allText(el)));
            for (std::size_t i = 0; i < lines.size(); ++i) s.text += (i ? "\n" : "") + lines[i];
            if (s.text.empty()) return;
        } else if (el.name == "path") {
            s.kind = ShapeKind::Path;
            if (auto d = el.attr("d")) s.d = trim(*d);
            if (s.d.empty()) return;
        } else {
            return;  // unknown element: skip it and whatever is inside
        }

        if (auto n = el.attr("data-name")) s.name = *n;
        else if (auto label = el.attr("inkscape:label")) s.name = *label;
        else if (auto id = el.attr("id")) s.name = *id;

        s.fill = st.fill;
        if (s.fill) s.fill->a = static_cast<std::uint8_t>(std::lround(s.fill->a * st.fillOpacity));
        s.stroke = st.stroke;
        if (s.stroke) s.stroke->a = static_cast<std::uint8_t>(std::lround(s.stroke->a * st.strokeOpacity));
        s.strokeWidth = st.strokeWidth;
        s.opacity = st.opacity;
        s.visible = !st.hidden;
        if (s.kind == ShapeKind::Line) s.fill.reset();
        if (auto g = gradients_.find(st.fillUrl); !st.fillUrl.empty() && g != gradients_.end() && g->second.hasStops) {
            Gradient grad = g->second.g;
            if (g->second.userSpace) {  // to fractions of the shape's own box
                const Bounds b = bounds(localOutline(s, 1.0));
                const double bw = std::max(b.width(), 1e-9), bh = std::max(b.height(), 1e-9);
                grad.x1 = (grad.x1 - b.x0) / bw, grad.x2 = (grad.x2 - b.x0) / bw;
                grad.y1 = (grad.y1 - b.y0) / bh, grad.y2 = (grad.y2 - b.y0) / bh;
                grad.cx = (grad.cx - b.x0) / bw, grad.cy = (grad.cy - b.y0) / bh;
                grad.r = grad.r / std::max(bw, bh);
            }
            s.gradient = grad;
            if (!s.fill) s.fill = grad.stops.front().color;
        }

        const Matrix& m = st.matrix;
        if (!(m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0)) {
            Bounds b = bounds(localOutline(s, 1.0));
            Point centre = b.empty ? Point{0, 0} : Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
            s.transform = decompose(m, centre);
        }
        doc_.add(std::move(s));
    }
};

}  // namespace

std::string toSvg(const VectorDoc& doc) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(doc.width) + "\" height=\"" +
           std::to_string(doc.height) + "\" viewBox=\"0 0 " + std::to_string(doc.width) + " " + std::to_string(doc.height) +
           "\">\n";
    if (doc.background.a > 0) {
        out += "  <rect data-role=\"background\" x=\"0\" y=\"0\" width=\"" + std::to_string(doc.width) + "\" height=\"" +
               std::to_string(doc.height) + "\"" + paintAttrs("fill", doc.background) + "/>\n";
    }
    std::string defs;
    for (const auto& s : doc.shapes) {
        if (!s.gradient || s.gradient->stops.empty() || s.kind == ShapeKind::Line) continue;
        const Gradient& g = *s.gradient;
        defs += g.radial ? "    <radialGradient id=\"g-" + escape(s.id) + "\" cx=\"" + num(g.cx) + "\" cy=\"" + num(g.cy) + "\" r=\"" + num(g.r) + "\">\n"
                         : "    <linearGradient id=\"g-" + escape(s.id) + "\" x1=\"" + num(g.x1) + "\" y1=\"" + num(g.y1) + "\" x2=\"" + num(g.x2) +
                               "\" y2=\"" + num(g.y2) + "\">\n";
        for (const auto& st : g.stops) {
            defs += "      <stop offset=\"" + num(st.offset) + "\" stop-color=\"" + rgb(st.color) + "\"";
            if (st.color.a < 255) defs += " stop-opacity=\"" + num(st.color.a / 255.0) + "\"";
            defs += "/>\n";
        }
        defs += g.radial ? "    </radialGradient>\n" : "    </linearGradient>\n";
    }
    if (!defs.empty()) out += "  <defs>\n" + defs + "  </defs>\n";
    for (const auto& s : doc.shapes) {
        std::string el, body;
        switch (s.kind) {
            case ShapeKind::Rect:
                el = "<rect x=\"" + num(std::min(s.x, s.x + s.w)) + "\" y=\"" + num(std::min(s.y, s.y + s.h)) + "\" width=\"" +
                     num(std::abs(s.w)) + "\" height=\"" + num(std::abs(s.h)) + "\"";
                if (s.radius > 0) el += " rx=\"" + num(s.radius) + "\"";
                break;
            case ShapeKind::Ellipse:
                if (s.rx == s.ry) el = "<circle cx=\"" + num(s.cx) + "\" cy=\"" + num(s.cy) + "\" r=\"" + num(s.rx) + "\"";
                else {
                    el = "<ellipse cx=\"" + num(s.cx) + "\" cy=\"" + num(s.cy) + "\" rx=\"" + num(s.rx) + "\" ry=\"" +
                         num(s.ry) + "\"";
                }
                break;
            case ShapeKind::Line: {
                Point a = s.points.size() > 0 ? s.points[0] : Point{};
                Point b = s.points.size() > 1 ? s.points[1] : a;
                el = "<line x1=\"" + num(a.x) + "\" y1=\"" + num(a.y) + "\" x2=\"" + num(b.x) + "\" y2=\"" + num(b.y) + "\"";
                break;
            }
            case ShapeKind::Polyline: el = "<polyline points=\"" + pointsAttr(s.points) + "\""; break;
            case ShapeKind::Polygon: el = "<polygon points=\"" + pointsAttr(s.points) + "\""; break;
            case ShapeKind::Path: el = "<path d=\"" + escape(s.d) + "\""; break;
            case ShapeKind::Text: {
                const std::string f = lower(s.font);
                const std::string family = f == "serif" ? "Times New Roman, serif" : f == "mono" ? "Courier New, monospace"
                                           : f == "sans" ? "Roboto, Arial, sans-serif" : s.font;
                el = "<text x=\"" + num(s.x) + "\" y=\"" + num(s.y) + "\" font-size=\"" + num(s.fontSize) + "\" font-family=\"" +
                     escape(family) + "\" data-font=\"" + escape(s.font) + "\"";
                if (s.bold) el += " font-weight=\"bold\"";
                if (s.italic) el += " font-style=\"italic\"";
                if (s.anchor != "start") el += " text-anchor=\"" + s.anchor + "\"";
                if (s.letterSpacing != 0) el += " letter-spacing=\"" + num(s.letterSpacing) + "\"";
                if (s.lineHeight != 1.2) el += " data-line-height=\"" + num(s.lineHeight) + "\"";
                std::vector<std::string> lines;
                std::size_t start = 0;
                while (true) {
                    const std::size_t end = s.text.find('\n', start);
                    lines.push_back(s.text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
                if (lines.size() == 1) body = escape(lines[0]);
                else {
                    for (std::size_t i = 0; i < lines.size(); ++i) {
                        body += "<tspan x=\"" + num(s.x) + "\" dy=\"" + (i ? num(s.fontSize * s.lineHeight) : std::string("0")) + "\">" +
                                escape(lines[i]) + "</tspan>";
                    }
                }
                break;
            }
        }
        std::string attrs = " id=\"" + escape(s.id) + "\" data-name=\"" + escape(s.name) + "\"";
        if (s.gradient && !s.gradient->stops.empty() && s.kind != ShapeKind::Line) attrs += " fill=\"url(#g-" + escape(s.id) + ")\"";
        else attrs += paintAttrs("fill", s.kind == ShapeKind::Line ? std::nullopt : s.fill);
        attrs += paintAttrs("stroke", s.stroke);
        if (s.stroke) attrs += " stroke-width=\"" + num(s.strokeWidth) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\"";
        if (s.opacity < 1) attrs += " opacity=\"" + num(s.opacity) + "\"";
        if (!s.visible) attrs += " display=\"none\"";
        if (!s.transform.identity()) {
            const Matrix m = shapeMatrix(s);
            std::string t = "translate(" + num(m.e) + " " + num(m.f) + ")";
            if (s.transform.rotate != 0) t += " rotate(" + num(s.transform.rotate) + ")";
            if (s.transform.sx != 1 || s.transform.sy != 1) t += " scale(" + num(s.transform.sx) + " " + num(s.transform.sy) + ")";
            attrs += " transform=\"" + t + "\"";
        }
        if (s.kind == ShapeKind::Text) out += "  " + el + attrs + ">" + body + "</text>\n";
        else out += "  " + el + attrs + "/>\n";
    }
    out += "</svg>\n";
    return out;
}

std::shared_ptr<VectorDoc> fromSvg(const std::string& text) {
    Element doc = XmlReader(text).parse();
    const Element* svg = nullptr;
    for (const auto& c : doc.children) {
        if (c.name == "svg") svg = &c;
    }
    if (!svg) throw std::runtime_error("not an SVG file (no <svg> element)");
    auto out = std::make_shared<VectorDoc>();
    Importer(*out).root(*svg);
    return out;
}

std::shared_ptr<VectorDoc> openSvg(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    auto doc = fromSvg(ss.str());
    doc->path = path.string();
    return doc;
}

void saveDoc(const VectorDoc& doc, const std::filesystem::path& path, double scale, int quality) {
    const std::string e = ext(path);
    if (e == "svg") {
        std::ofstream out(path, std::ios::binary);
        if (!out) throw std::runtime_error("can't write " + path.string());
        out << toSvg(doc);
        if (!out) throw std::runtime_error("failed writing " + path.string());
        return;
    }
    if (!raster::canWrite(path)) {
        std::string list = "svg";
        for (const auto& x : raster::writableExtensions()) list += ", " + x;
        throw std::runtime_error("can't save ." + e + " files; use one of: " + list);
    }
    raster::save(render(doc, scale), path, quality);
}

}  // namespace wetwaresuite::vector
