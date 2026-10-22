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
#include "internal.hpp"

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
            if (code > 0 && code < 0x80) out += static_cast<char>(code);
            else if (code < 0x800) out += static_cast<char>(0xC0 | (code >> 6)), out += static_cast<char>(0x80 | (code & 63));
            else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 63));
                out += static_cast<char>(0x80 | (code & 63));
            } else if (code < 0x110000) {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 63));
                out += static_cast<char>(0x80 | ((code >> 6) & 63));
                out += static_cast<char>(0x80 | (code & 63));
            } else {
                out += '?';
            }
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

double clamp01(const std::string& s) {
    auto v = numbers(s);
    if (v.empty()) return 1;
    double x = v[0];
    if (s.find('%') != std::string::npos) x /= 100;
    return std::clamp(x, 0.0, 1.0);
}

std::string urlId(const std::string& v) {
    const auto hash = v.find('#'), close = v.find(')');
    if (v.rfind("url(", 0) != 0 || hash == std::string::npos || close == std::string::npos || close < hash) return "";
    return trim(v.substr(hash + 1, close - hash - 1));
}

// Inherited presentation properties (SVG's defaults where the file says nothing).
struct Style {
    std::optional<Color> fill = Color{0, 0, 0, 255};
    std::optional<Color> stroke;
    double strokeWidth = 1;
    double fillOpacity = 1, strokeOpacity = 1;
    bool hidden = false;
    Matrix matrix;
    std::string fillUrl, strokeUrl;
    std::string fillRule = "nonzero";
    StrokeStyle line;
    const Element* wrap = nullptr;  // an appearance wrapper whose settings go to the shape inside
};

struct Defs {
    struct GradientDef {
        Gradient g;
        bool userSpace = false;
        std::string href;
        bool hasStops = false;
    };
    std::map<std::string, GradientDef> gradients;
    std::map<std::string, const Element*> byId;
};

double fraction(const std::string* v, double fallback) {
    if (!v) return fallback;
    auto n = numbers(*v);
    if (n.empty()) return fallback;
    return v->find('%') != std::string::npos ? n[0] / 100 : n[0];
}

void collect(const Element& el, Defs& defs) {
    for (const auto& c : el.children) {
        if (auto id = c.attr("id")) defs.byId[*id] = &c;
        if (c.name == "linearGradient" || c.name == "radialGradient") {
            Defs::GradientDef def;
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
            if (auto id = c.attr("id")) defs.gradients[*id] = def;
        }
        collect(c, defs);
    }
    for (auto& [id, def] : defs.gradients) {  // stops borrowed through href
        std::string ref = def.href;
        for (int depth = 0; !def.hasStops && !ref.empty() && depth < 8; ++depth) {
            auto it = defs.gradients.find(ref);
            if (it == defs.gradients.end()) break;
            if (it->second.hasStops) {
                def.g.stops = it->second.g.stops;
                def.hasStops = true;
            }
            ref = it->second.href;
        }
    }
}

std::string allText(const Element& el) {
    std::string t = el.text;
    for (const auto& c : el.children) t += allText(c);
    return t;
}

std::string collapse(const std::string& t) {
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

bool skipped(const std::string& name) {
    static const char* names[] = {"defs", "clipPath", "mask", "pattern", "marker", "symbol", "linearGradient", "radialGradient", "style",
                                  "script", "metadata", "title", "desc", "filter", "image", "foreignObject", "namedview"};
    for (const char* n : names) {
        if (name == n) return true;
    }
    return false;
}

class Importer {
public:
    Importer(VectorDoc& doc, Defs& defs, std::string idPrefix = "") : doc_(doc), defs_(defs), prefix_(std::move(idPrefix)) {}

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
        ours_ = svg.attr("data-next-id") != nullptr;
        if (!ours_) style.line.cap = "butt", style.line.join = "miter";
        if (hasBox) {
            style.matrix = Matrix::scale(w / box[2], h / box[3]) * Matrix::translate(-box[0], -box[1]);
            refW_ = box[2];
            refH_ = box[3];
            // one artboard saved on its own: the shapes keep their canvas coordinates
            if (auto canvas = svg.attr("data-canvas")) {
                const auto xpos = canvas->find('x');
                if (xpos != std::string::npos) {
                    doc_.width = std::max(1, std::atoi(canvas->c_str()));
                    doc_.height = std::max(1, std::atoi(canvas->c_str() + xpos + 1));
                    style.matrix = Matrix{};
                }
            }
        } else {
            refW_ = w;
            refH_ = h;
        }
        collect(svg, defs_);
        symbols(svg);
        rootData(svg);
        children(svg, style, "");
        finish();
    }

    // The shapes of one symbol, read into a scratch document and moved over by the caller.
    void symbolBody(const Element& el) {
        Style style;
        ours_ = true;
        children(el, style, "");
        finish();
    }

private:
    VectorDoc& doc_;
    Defs& defs_;
    std::string prefix_;  // ids written inside a symbol carry "sym-<name>-"
    bool ours_ = false;
    double refW_ = 800, refH_ = 600;
    std::map<std::string, std::string> idMap_;  // id in the file -> shape id
    std::vector<std::pair<std::string, std::string>> onPath_;  // text shape id -> id of its path in the file
    std::string currentLayer_;
    int depth_ = 0;

    void symbols(const Element& el) {
        for (const auto& c : el.children) {
            if (c.name == "symbol") {
                Symbol sym;
                const std::string id = c.attr("id") ? *c.attr("id") : "";
                sym.name = c.attr("data-name") ? *c.attr("data-name") : (id.rfind("sym-", 0) == 0 ? id.substr(4) : id);
                if (sym.name.empty()) sym.name = "symbol" + std::to_string(doc_.symbols.size() + 1);
                VectorDoc scratch;
                scratch.symbols = doc_.symbols;
                Importer sub(scratch, defs_, "sym-" + sym.name + "-");
                sub.symbolBody(c);
                sym.shapes = std::move(scratch.shapes);
                if (!doc_.findSymbol(sym.name)) doc_.symbols.push_back(std::move(sym));
            } else {
                symbols(c);
            }
        }
    }

    void rootData(const Element& svg) {
        if (auto a = svg.attr("data-artboards")) {
            for (const auto& part : detail::splitOn(*a, ';')) {
                const auto colon = part.rfind(':');
                if (colon == std::string::npos) continue;
                auto v = numbers(part.substr(colon + 1));
                if (v.size() == 4 && v[2] > 0 && v[3] > 0) doc_.artboards.push_back({part.substr(0, colon), v[0], v[1], v[2], v[3]});
            }
        }
        if (auto g = svg.attr("data-guides-x")) doc_.guidesX = numbers(*g);
        if (auto g = svg.attr("data-guides-y")) doc_.guidesY = numbers(*g);
        if (auto g = svg.attr("data-grid")) doc_.gridSize = std::max(0.0, std::atof(g->c_str()));
        if (auto sn = svg.attr("data-snap")) {
            doc_.snapGrid = sn->find("grid") != std::string::npos;
            doc_.snapGuides = sn->find("guides") != std::string::npos;
            doc_.snapPoints = sn->find("points") != std::string::npos;
        }
        if (auto c = svg.attr("data-current-layer")) currentLayer_ = *c;
        if (auto w = svg.attr("data-swatches")) {
            for (const auto& part : detail::splitOn(*w, ';')) {
                const auto eq = part.find('=');
                if (eq == std::string::npos) continue;
                if (auto c = parseColor(part.substr(eq + 1))) doc_.swatches.push_back({part.substr(0, eq), *c});
            }
        }
        if (auto t = svg.attr("data-text-styles")) {
            for (const auto& part : detail::splitOn(*t, ';')) {
                TextStyle st;
                if (detail::parseTextStyle(part, st)) doc_.textStyles.push_back(st);
            }
        }
        if (auto n = svg.attr("data-next-id")) doc_.nextId = std::max(doc_.nextId, std::atoi(n->c_str()));
    }

    void finish() {
        for (const auto& [textId, ref] : onPath_) {
            const int i = doc_.find(textId);
            auto it = idMap_.find(ref);
            if (i >= 0) doc_.shapes[static_cast<std::size_t>(i)].onPath = it != idMap_.end() ? it->second : "";
        }
        if (!currentLayer_.empty()) {
            auto it = idMap_.find(currentLayer_);
            if (it != idMap_.end()) doc_.currentLayer = it->second;
        }
    }

    // Adds a shape, keeping the file's id when it's one of ours (s12) and still free.
    std::string add(Shape s, const Element* el) {
        const std::string* fileId = el ? el->attr("id") : nullptr;
        std::string want = fileId ? *fileId : "";
        if (!prefix_.empty() && want.rfind(prefix_, 0) == 0) want = want.substr(prefix_.size());
        bool reuse = want.size() > 1 && want[0] == 's' &&
                     std::all_of(want.begin() + 1, want.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        for (const auto& x : doc_.shapes) reuse = reuse && x.id != want;
        std::string id;
        if (reuse) {
            s.id = want;
            if (s.name.empty()) s.name = std::string(toString(s.kind)) + " " + want.substr(1);
            doc_.nextId = std::max(doc_.nextId, std::atoi(want.c_str() + 1) + 1);
            doc_.shapes.push_back(std::move(s));
            id = want;
        } else {
            const std::string parent = s.parent;
            id = doc_.add(std::move(s));
            doc_.shapes.back().parent = parent;  // add() would put it in the current layer
        }
        if (fileId) idMap_[*fileId] = id;
        return id;
    }

    void children(const Element& el, const Style& style, const std::string& parent) {
        for (const auto& c : el.children) element(c, style, parent);
    }

    Style resolve(const Element& el, const Style& parent) {
        Style s = parent;
        s.wrap = nullptr;
        std::map<std::string, std::string> props;
        for (const char* key : {"fill", "stroke", "stroke-width", "fill-opacity", "stroke-opacity", "display", "visibility", "fill-rule", "stroke-linecap",
                                "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "marker-start", "marker-end"}) {
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
            const std::string v = trim(value);
            if (key == "fill") {
                s.fillUrl = urlId(v);
                if (auto p = paint(value)) s.fill = *p;
            } else if (key == "stroke") {
                s.strokeUrl = urlId(v);
                if (auto p = paint(value)) s.stroke = *p;
            } else if (key == "stroke-width") {
                s.strokeWidth = std::max(0.0, length(&value, std::hypot(refW_, refH_) / std::sqrt(2.0), s.strokeWidth));
            } else if (key == "fill-opacity") {
                s.fillOpacity = clamp01(value);
            } else if (key == "stroke-opacity") {
                s.strokeOpacity = clamp01(value);
            } else if (key == "display") {
                if (v == "none") s.hidden = true;
            } else if (key == "visibility") {
                if (v == "hidden" || v == "collapse") s.hidden = true;
                else if (v == "visible") s.hidden = false;
            } else if (key == "fill-rule") {
                s.fillRule = v == "evenodd" ? "evenodd" : "nonzero";
            } else if (key == "stroke-linecap") {
                if (v == "butt" || v == "round" || v == "square") s.line.cap = v;
            } else if (key == "stroke-linejoin") {
                if (v == "miter" || v == "round" || v == "bevel") s.line.join = v;
            } else if (key == "stroke-miterlimit") {
                s.line.miterLimit = std::max(1.0, std::atof(v.c_str()));
            } else if (key == "stroke-dasharray") {
                s.line.dash.clear();
                if (v != "none") {
                    for (double d : numbers(v)) s.line.dash.push_back(std::max(0.0, d));
                }
            } else if (key == "stroke-dashoffset") {
                s.line.dashOffset = std::atof(v.c_str());
            } else if (key == "marker-start" || key == "marker-end") {
                std::string kind = "none";
                if (const std::string id = urlId(v); !id.empty()) {
                    auto it = defs_.byId.find(id);
                    kind = it != defs_.byId.end() && it->second->attr("data-arrow") ? *it->second->attr("data-arrow") : "triangle";
                }
                (key == "marker-start" ? s.line.startArrow : s.line.endArrow) = kind;
            }
        }
        if (auto t = el.attr("transform")) s.matrix = s.matrix * parseTransform(*t);
        return s;
    }

    // Opacity, blend, effects and lock of one element (these aren't inherited).
    void own(const Element& el, Shape& s) {
        if (auto o = el.attr("opacity")) s.opacity *= clamp01(*o);
        if (auto st = el.attr("style")) {
            std::stringstream ss(*st);
            std::string decl;
            while (std::getline(ss, decl, ';')) {
                auto colon = decl.find(':');
                if (colon == std::string::npos) continue;
                const std::string key = lower(trim(decl.substr(0, colon))), value = trim(decl.substr(colon + 1));
                if (key == "opacity") s.opacity *= clamp01(value);
                if (key == "mix-blend-mode") {
                    if (auto b = raster::parseBlend(value)) s.blend = raster::toString(*b);
                }
            }
        }
        if (auto b = el.attr("data-blend")) {
            if (auto m = raster::parseBlend(*b)) s.blend = raster::toString(*m);
        }
        if (auto fx = el.attr("data-effects")) {
            std::string error;
            detail::parseEffects(*fx, s.effects, error);
        } else if (auto f = el.attr("filter")) {
            filterEffects(urlId(*f), s.effects);
        }
        if (auto l = el.attr("data-locked")) s.locked = *l == "1";
    }

    void filterEffects(const std::string& id, std::vector<Effect>& out) {
        auto it = defs_.byId.find(id);
        if (it == defs_.byId.end()) return;
        for (const auto& fe : it->second->children) {
            Effect e;
            auto attrNum = [&](const char* k, double fallback) { return fe.attr(k) ? std::atof(fe.attr(k)->c_str()) : fallback; };
            if (fe.name == "feGaussianBlur" && it->second->children.size() == 1) {
                e.type = "blur";
                e.opacity = 1;
                e.radius = attrNum("stdDeviation", 2);
            } else if (fe.name == "feDropShadow") {
                e.type = "drop_shadow";
                e.dx = attrNum("dx", 2);
                e.dy = attrNum("dy", 2);
                e.radius = attrNum("stdDeviation", 2);
                if (auto c = fe.attr("flood-color")) e.color = parseColor(*c).value_or(Color{0, 0, 0, 255});
                e.opacity = fe.attr("flood-opacity") ? clamp01(*fe.attr("flood-opacity")) : 1;
            } else {
                continue;
            }
            out.push_back(e);
        }
    }

    double len(const Element& el, const char* key, double ref) const { return length(el.attr(key), ref, 0); }

    static std::string nameOf(const Element& el) {
        if (auto n = el.attr("data-name")) return *n;
        if (auto label = el.attr("inkscape:label")) return *label;
        if (auto id = el.attr("id")) return *id;
        return "";
    }

    const Element* def(const Element& el, const char* attr) {
        const std::string id = el.attr(attr) ? urlId(*el.attr(attr)) : "";
        auto it = defs_.byId.find(id);
        return id.empty() || it == defs_.byId.end() ? nullptr : it->second;
    }

    // Imports a clipPath's or mask's shapes into group `gid` and links them as its clip or mask.
    void attachDefs(const std::string& gid, const Element* clip, const Element* mask, const Style& st) {
        for (int pass = 0; pass < 2; ++pass) {
            const Element* d = pass == 0 ? clip : mask;
            if (!d) continue;
            Style cs = resolve(*d, st);
            cs.hidden = false;
            const std::size_t before = doc_.shapes.size();
            ++depth_;
            for (const auto& c : d->children) element(c, cs, gid);
            --depth_;
            std::vector<std::string> made;
            for (std::size_t i = before; i < doc_.shapes.size(); ++i) {
                if (doc_.shapes[i].parent == gid) made.push_back(doc_.shapes[i].id);
            }
            if (made.empty()) continue;
            std::string link = made.front();
            if (made.size() > 1) {  // several shapes clip (or mask) together, as one group
                Shape holder;
                holder.kind = ShapeKind::Group;
                holder.name = pass == 0 ? "clip" : "mask";
                holder.parent = gid;
                link = add(std::move(holder), nullptr);
                for (const auto& id : made) doc_.shapes[static_cast<std::size_t>(doc_.find(id))].parent = link;
            }
            const int gi = doc_.find(gid);
            if (gi < 0) continue;
            if (pass == 0) doc_.shapes[static_cast<std::size_t>(gi)].clip = link;
            else doc_.shapes[static_cast<std::size_t>(gi)].mask = link;
        }
    }

    void element(const Element& el, const Style& parent, const std::string& parentId) {
        if (skipped(el.name) || depth_ > 32) return;
        if (auto role = el.attr("data-role"); role && *role == "extra") return;
        Style st = resolve(el, parent);
        if (el.name == "g" && el.attr("data-role") && *el.attr("data-role") == "appearance") {
            st.wrap = &el;
            ++depth_;
            for (const auto& c : el.children) {
                if (c.attr("data-role")) continue;
                Style inner = st;
                element(c, inner, parentId);
            }
            --depth_;
            return;
        }
        if (el.name == "a" || el.name == "switch") {
            children(el, st, parentId);
            return;
        }
        if (el.name == "g" || el.name == "svg") {
            Shape g;
            g.kind = ShapeKind::Group;
            g.name = nameOf(el);
            g.parent = parentId;
            g.layer = (el.attr("data-kind") && *el.attr("data-kind") == "layer") ||
                      (el.attr("inkscape:groupmode") && *el.attr("inkscape:groupmode") == "layer");
            g.visible = !(st.hidden && !parent.hidden);
            own(el, g);
            st.hidden = parent.hidden;  // the group carries its own hidden flag; its shapes keep theirs
            const std::string gid = add(std::move(g), &el);
            attachDefs(gid, def(el, "clip-path"), def(el, "mask"), st);
            ++depth_;
            children(el, st, gid);
            --depth_;
            return;
        }
        if (el.name == "use") {
            use(el, st, parentId);
            return;
        }
        Shape s;
        if (!geometry(el, st, s)) return;
        style(el, st, s);
        const Element* clip = def(el, "clip-path");
        const Element* mask = def(el, "mask");
        std::string parentOfShape = parentId;
        std::string group;
        if (clip || mask) {  // a clip or mask on a single shape: a group holds the shape and its clip
            Shape g;
            g.kind = ShapeKind::Group;
            g.name = clip ? "clip group" : "mask group";
            g.parent = parentId;
            group = add(std::move(g), nullptr);
            parentOfShape = group;
        }
        s.parent = parentOfShape;
        const bool textOnPath = s.kind == ShapeKind::Text && !s.onPath.empty();
        const std::string ref = s.onPath;
        const std::string id = add(std::move(s), &el);
        if (textOnPath) onPath_.push_back({id, ref});
        if (!group.empty()) attachDefs(group, clip, mask, st);
    }

    void use(const Element& el, const Style& st, const std::string& parentId) {
        std::string ref;
        if (auto h = el.attr("href")) ref = *h;
        else if (auto h2 = el.attr("xlink:href")) ref = *h2;
        if (ref.empty() || ref[0] != '#') return;
        ref.erase(0, 1);
        auto it = defs_.byId.find(ref);
        if (it == defs_.byId.end()) return;
        Style inner = st;
        inner.matrix = st.matrix * Matrix::translate(len(el, "x", refW_), len(el, "y", refH_));
        if (it->second->name == "symbol") {
            const std::string name = it->second->attr("data-name") ? *it->second->attr("data-name") : (ref.rfind("sym-", 0) == 0 ? ref.substr(4) : ref);
            if (!doc_.findSymbol(name)) return;
            Shape s;
            s.kind = ShapeKind::Instance;
            s.symbol = name;
            s.name = nameOf(el);
            s.parent = parentId;
            s.visible = !st.hidden;
            own(el, s);
            VectorDoc probe;  // its box, so the transform turns around the right centre
            probe.symbols = doc_.symbols;
            probe.shapes.push_back(s);
            probe.refresh();
            s = probe.shapes.back();
            s.transform = detail::decomposeMatrix(inner.matrix, Point{s.x + s.w / 2, s.y + s.h / 2});
            add(std::move(s), &el);
            return;
        }
        if (depth_ > 8) return;
        ++depth_;
        element(*it->second, inner, parentId);
        --depth_;
    }

    bool geometry(const Element& el, const Style& st, Shape& s) {
        const double diag = std::hypot(refW_, refH_) / std::sqrt(2.0);
        if (el.name == "rect") {
            s.kind = ShapeKind::Rect;
            s.x = len(el, "x", refW_);
            s.y = len(el, "y", refH_);
            s.w = len(el, "width", refW_);
            s.h = len(el, "height", refH_);
            s.radius = el.attr("rx") ? len(el, "rx", refW_) : len(el, "ry", refH_);
            if (s.w <= 0 || s.h <= 0) return false;
            if (auto role = el.attr("data-role"); role && *role == "background") {
                Color c = st.fill.value_or(Color{0, 0, 0, 0});
                c.a = static_cast<std::uint8_t>(std::lround(c.a * st.fillOpacity));
                doc_.background = c;
                return false;
            }
        } else if (el.name == "circle") {
            s.kind = ShapeKind::Ellipse;
            s.cx = len(el, "cx", refW_);
            s.cy = len(el, "cy", refH_);
            s.rx = s.ry = len(el, "r", diag);
            if (s.rx <= 0) return false;
        } else if (el.name == "ellipse") {
            s.kind = ShapeKind::Ellipse;
            s.cx = len(el, "cx", refW_);
            s.cy = len(el, "cy", refH_);
            s.rx = len(el, "rx", refW_);
            s.ry = len(el, "ry", refH_);
            if (s.rx <= 0 || s.ry <= 0) return false;
        } else if (el.name == "line") {
            s.kind = ShapeKind::Line;
            s.points = {{len(el, "x1", refW_), len(el, "y1", refH_)}, {len(el, "x2", refW_), len(el, "y2", refH_)}};
        } else if (el.name == "polyline" || el.name == "polygon") {
            s.kind = el.name == "polygon" ? ShapeKind::Polygon : ShapeKind::Polyline;
            auto v = el.attr("points") ? numbers(*el.attr("points")) : std::vector<double>{};
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) s.points.push_back({v[i], v[i + 1]});
            if (s.points.size() < 2) return false;
        } else if (el.name == "text") {
            return text(el, s, diag);
        } else if (el.name == "path") {
            s.kind = ShapeKind::Path;
            if (auto d = el.attr("d")) s.d = trim(*d);
            if (s.d.empty()) return false;
        } else {
            return false;  // unknown element: skip it and whatever is inside
        }
        return true;
    }

    bool text(const Element& el, Shape& s, double diag) {
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
        if (auto c = el.attr("data-case")) s.textCase = *c;
        else if (auto fv = el.attr("font-variant"); fv && *fv == "small-caps") s.textCase = "small_caps";
        if (auto ts = el.attr("data-style")) s.textStyle = *ts;
        if (auto bw = el.attr("data-box-width")) {
            s.boxWidth = std::max(0.0, std::atof(bw->c_str()));
            if (auto bh = el.attr("data-box-height")) s.boxHeight = std::max(0.0, std::atof(bh->c_str()));
            if (auto c = el.attr("data-columns")) s.columns = std::max(1, std::atoi(c->c_str()));
            if (auto g = el.attr("data-column-gap")) s.columnGap = std::max(0.0, std::atof(g->c_str()));
            if (auto a = el.attr("data-align")) s.align = *a;
            if (auto t = el.attr("data-text")) s.text = *t;
            return !s.text.empty();
        }
        for (const auto& c : el.children) {
            if (c.name != "textPath") continue;
            std::string ref = c.attr("href") ? *c.attr("href") : c.attr("xlink:href") ? *c.attr("xlink:href") : "";
            if (!ref.empty() && ref[0] == '#') ref.erase(0, 1);
            s.text = collapse(allText(c));
            s.pathOffset = c.attr("startOffset") ? std::atof(c.attr("startOffset")->c_str()) : 0;
            if (auto po = el.attr("data-path-offset")) s.pathOffset = std::atof(po->c_str());
            if (!ref.empty()) {
                s.onPath = ref;  // the file's id for now; finish() turns it into the shape's id
                if (auto it = defs_.byId.find(ref); it != defs_.byId.end() && it->second->attr("d")) s.d = *it->second->attr("d");
            }
            return !s.text.empty();
        }
        std::vector<std::string> lines;
        for (const auto& c : el.children) {
            if (c.name == "tspan") lines.push_back(collapse(allText(c)));
        }
        if (lines.empty()) lines.push_back(collapse(allText(el)));
        for (std::size_t i = 0; i < lines.size(); ++i) s.text += (i ? "\n" : "") + lines[i];
        return !s.text.empty();
    }

    // Paint, stroke style, effects and transform from the resolved style and our data-* attributes.
    void style(const Element& el, const Style& st, Shape& s) {
        s.name = nameOf(el);
        s.fill = st.fill;
        if (s.fill) s.fill->a = static_cast<std::uint8_t>(std::lround(s.fill->a * st.fillOpacity));
        s.stroke = st.stroke;
        if (s.stroke) s.stroke->a = static_cast<std::uint8_t>(std::lround(s.stroke->a * st.strokeOpacity));
        s.strokeWidth = st.strokeWidth;
        s.fillRule = st.fillRule;
        s.line = st.line;
        s.visible = !st.hidden;
        if (st.wrap) {
            own(*st.wrap, s);
            if (st.wrap->attr("display") && *st.wrap->attr("display") == "none") s.visible = false;
        }
        own(el, s);
        if (s.kind == ShapeKind::Line) s.fill.reset();
        auto url = [&](const std::string& id, bool forStroke) {
            if (id.empty()) return;
            if (auto g = defs_.gradients.find(id); g != defs_.gradients.end() && g->second.hasStops) {
                Gradient grad = g->second.g;
                if (g->second.userSpace) {  // to fractions of the shape's own box
                    const Bounds b = bounds(localOutline(s, 1.0));
                    const double bw = std::max(b.width(), 1e-9), bh = std::max(b.height(), 1e-9);
                    grad.x1 = (grad.x1 - b.x0) / bw, grad.x2 = (grad.x2 - b.x0) / bw;
                    grad.y1 = (grad.y1 - b.y0) / bh, grad.y2 = (grad.y2 - b.y0) / bh;
                    grad.cx = (grad.cx - b.x0) / bw, grad.cy = (grad.cy - b.y0) / bh;
                    grad.r = grad.r / std::max(bw, bh);
                }
                if (forStroke) {
                    s.strokeGradient = grad;
                    if (!s.stroke) s.stroke = grad.stops.front().color;
                } else {
                    s.gradient = grad;
                    if (!s.fill) s.fill = grad.stops.front().color;
                }
                return;
            }
            auto p = defs_.byId.find(id);
            if (p == defs_.byId.end() || forStroke) return;
            if (auto ff = p->second->attr("data-freeform")) {
                Gradient g;
                g.freeform = true;
                std::string error;
                if (detail::parseFreeform(*ff, g.points, error)) {
                    s.gradient = g;
                    if (!s.fill) s.fill = g.points.front().color;
                }
            }
        };
        url(st.fillUrl, false);
        url(st.strokeUrl, true);
        if (auto f = el.attr("data-fill")) s.fill = parseColor(*f);
        if (auto p = el.attr("data-pattern")) {
            Pattern pat;
            std::string error;
            if (detail::parsePattern(*p, pat, error)) s.pattern = pat;
            if (!el.attr("data-fill") && !s.gradient) s.fill.reset();
        }
        if (auto ds = el.attr("data-stroke")) s.stroke = parseColor(*ds);
        if (auto dw = el.attr("data-stroke-width")) s.strokeWidth = std::max(0.0, std::atof(dw->c_str()));
        if (auto so = el.attr("data-stroke-style")) {
            std::istringstream in(*so);
            std::string tok, error;
            while (in >> tok) {
                const auto eq = tok.find('=');
                if (eq != std::string::npos) detail::readStrokeOption(s.line, tok.substr(0, eq), tok.substr(eq + 1), error);
            }
        }
        if (auto ap = el.attr("data-appearance")) {
            std::string error;
            detail::parseAppearance(*ap, s.appearance, error);
        }
        if (auto sw = el.attr("data-swatch-fill")) s.fillSwatch = *sw;
        if (auto sw = el.attr("data-swatch-stroke")) s.strokeSwatch = *sw;
        const Matrix& m = st.matrix;
        if (!m.identity()) {
            const Bounds b = bounds(localOutline(s, 1.0));
            const Point centre = b.empty ? Point{0, 0} : Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
            s.transform = detail::decomposeMatrix(m, centre);
            const Matrix rebuilt = shapeMatrix(s);
            const bool exact = std::abs(rebuilt.a - m.a) < 1e-6 && std::abs(rebuilt.b - m.b) < 1e-6 && std::abs(rebuilt.c - m.c) < 1e-6 &&
                               std::abs(rebuilt.d - m.d) < 1e-6;
            if (!exact && s.kind != ShapeKind::Text) {  // skewed: the outline takes the matrix itself
                BezierPath bp = shapePath(s);
                transformPath(bp, m);
                s.strokeWidth *= m.scaleFactor();
                s.kind = ShapeKind::Path;
                s.d = pathData(bp);
                s.points.clear();
                s.transform = {};
            }
        }
    }
};

}  // namespace

std::shared_ptr<VectorDoc> fromSvg(const std::string& text) {
    Element doc = XmlReader(text).parse();
    const Element* svg = nullptr;
    for (const auto& c : doc.children) {
        if (c.name == "svg") svg = &c;
    }
    if (!svg) throw std::runtime_error("not an SVG file (no <svg> element)");
    auto out = std::make_shared<VectorDoc>();
    Defs defs;
    Importer(*out, defs).root(*svg);
    out->refresh();
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

}  // namespace wetwaresuite::vector
