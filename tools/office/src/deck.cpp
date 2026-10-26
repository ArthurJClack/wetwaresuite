// Slide decks: layout presets placed once into frames, which are then drawn
// as PDF/PNG pages or written as PowerPoint shapes, so both outputs agree.
#include "wetwaresuite/office/deck.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "wetwaresuite/office/color.hpp"
#include "text.hpp"
#include "textlayout.hpp"
#include "xml.hpp"

namespace wetwaresuite::office {

using pdf::Color;

const std::vector<std::string>& slideLayouts() {
    static const std::vector<std::string> names = {"title", "section", "content", "two_column", "image", "image_right", "title_only", "blank"};
    return names;
}

namespace {

struct Preset {
    const char* name;
    Color accent, background, text;
};

const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"blue", {0x2B, 0x57, 0x9A, 255}, {255, 255, 255, 255}, {0x22, 0x22, 0x22, 255}},
        {"teal", {0x0F, 0x7C, 0x80, 255}, {255, 255, 255, 255}, {0x22, 0x22, 0x22, 255}},
        {"green", {0x2E, 0x7D, 0x32, 255}, {255, 255, 255, 255}, {0x22, 0x22, 0x22, 255}},
        {"orange", {0xC5, 0x5A, 0x11, 255}, {255, 255, 255, 255}, {0x22, 0x22, 0x22, 255}},
        {"red", {0xB3, 0x26, 0x1E, 255}, {255, 255, 255, 255}, {0x22, 0x22, 0x22, 255}},
        {"purple", {0x6A, 0x3D, 0x9A, 255}, {255, 255, 255, 255}, {0x22, 0x22, 0x22, 255}},
        {"slate", {0x3E, 0x4C, 0x59, 255}, {0xF7, 0xF8, 0xFA, 255}, {0x22, 0x2B, 0x33, 255}},
        {"dark", {0x4F, 0xA3, 0xF7, 255}, {0x1E, 0x1F, 0x24, 255}, {0xEE, 0xEE, 0xEE, 255}},
    };
    return p;
}

std::string hex6(Color c) {
    char b[8];
    std::snprintf(b, sizeof b, "%02X%02X%02X", c.r, c.g, c.b);
    return b;
}

Color mix(Color a, Color b, double t) {
    auto m = [t](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

double luminance(Color c) { return (0.299 * c.r + 0.587 * c.g + 0.114 * c.b) / 255.0; }

}  // namespace

const std::vector<std::string>& themeNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& p : presets()) n.push_back(p.name);
        return n;
    }();
    return names;
}

bool parseTheme(const std::string& spec, Theme& out, std::string& error) {
    std::string s = lower(trimCopy(spec));
    for (const auto& p : presets()) {
        if (s == p.name) {
            pdf::Family font = out.font;
            out = Theme{};
            out.name = p.name;
            out.accent = p.accent;
            out.background = p.background;
            out.text = p.text;
            out.font = font;
            return true;
        }
    }
    auto c = Color::parse(s);
    if (!c) {
        error = "unknown theme '" + spec + "': use one of blue, teal, green, orange, red, purple, slate, dark or a colour like #2b579a";
        return false;
    }
    c->a = 255;
    out.accent = *c;
    out.name = "#" + lower(hex6(*c));
    return true;
}

// ---- bullets -----------------------------------------------------------------------

std::vector<ListItem> parseBullets(const std::string& text, bool* numbered) {
    std::vector<ListItem> items;
    std::vector<int> indents;  // indent of each open level
    int topNumbered = 0, top = 0;
    for (const auto& raw : splitLines(text)) {
        int indent = 0;
        std::size_t i = 0;
        for (; i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'); ++i) indent += raw[i] == '\t' ? 4 : 1;
        std::string line = trimCopy(raw.substr(i));
        if (line.empty()) continue;
        // optional marker
        bool digits = false;
        if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ') {
            line = trimCopy(line.substr(2));
        } else if (startsWith(line, "\xE2\x80\xA2")) {
            line = trimCopy(line.substr(3));
        } else {
            std::size_t d = 0;
            while (d < line.size() && std::isdigit(static_cast<unsigned char>(line[d]))) ++d;
            if (d > 0 && d < 4 && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ') {
                line = trimCopy(line.substr(d + 2));
                digits = true;
            }
        }
        while (!indents.empty() && indent < indents.back()) indents.pop_back();
        if (indents.empty() || indent > indents.back()) {
            if (indents.empty() || indents.size() < 5) indents.push_back(indent);
            else indents.back() = indent;
        }
        ListItem it;
        it.level = static_cast<int>(indents.size()) - 1;
        // the first item cannot be indented
        if (items.empty()) {
            it.level = 0;
            indents.assign(1, indent);
        } else {
            it.level = std::min(it.level, items.back().level + 1);
        }
        it.runs = parseInline(line);
        if (it.level == 0) {
            ++top;
            topNumbered += digits ? 1 : 0;
        }
        items.push_back(std::move(it));
    }
    if (numbered) *numbered = top > 0 && topNumbered == top;
    return items;
}

std::string bulletText(const std::vector<ListItem>& items, bool numbered) {
    std::string out;
    int n = 0;
    for (const auto& it : items) {
        out += std::string(static_cast<std::size_t>(std::max(0, it.level)) * 2, ' ');
        out += (numbered && it.level <= 0 ? std::to_string(++n) + ". " : std::string("- ")) + inlineMarkdown(it.runs) + "\n";
    }
    return out;
}

void setSlideImage(Slide& s, const std::string& bytes) {
    const bool png = bytes.size() > 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0;
    const bool jpeg = bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8;
    if (png || jpeg) {
        s.pixels = pdf::loadImageData(bytes);
        s.image = bytes;
    } else {
        raster::Image img = raster::decode(bytes);
        s.image = raster::encodePng(img);
        s.pixels = pdf::loadImageData(s.image);
    }
}

void setObjectImage(SlideObject& o, const std::string& bytes) {
    Slide tmp;
    setSlideImage(tmp, bytes);
    o.image = std::move(tmp.image);
    o.pixels = std::move(tmp.pixels);
}

const std::vector<std::string>& shapeNames() {
    static const std::vector<std::string> n = {"rect",        "rounded",    "ellipse",   "triangle",      "right_triangle", "diamond", "pentagon",
                                               "hexagon",     "octagon",    "star",      "arrow_right",   "arrow_left",     "arrow_up", "arrow_down",
                                               "chevron",     "parallelogram", "trapezoid", "plus",       "callout"};
    return n;
}

const std::vector<std::string>& transitionNames() {
    static const std::vector<std::string> n = {"fade", "push", "wipe", "cover", "zoom"};
    return n;
}

bool parsePaint(const std::string& text, std::string& out) {
    const std::string t = lower(trimCopy(text));
    if (t.empty() || t == "none" || t == "transparent" || t == "no") {
        out.clear();
        return true;
    }
    if (t == "accent" || t == "theme") out = "accent";
    else if (t == "text") out = "text";
    else if (t == "background") out = "background";
    else {
        std::uint32_t c = 0;
        if (!parseColor(t, c) || !hasColor(c)) return false;
        out = colorName(c);
    }
    return true;
}

std::vector<std::pair<double, double>> shapeOutline(const std::string& shape, double x, double y, double w, double h) {
    std::vector<std::pair<double, double>> v;
    const double m = std::min(std::abs(w), std::abs(h));
    const double pi = 3.14159265358979323846;
    auto add = [&](double dx, double dy) { v.push_back({x + dx, y + dy}); };
    auto arc = [&](double cx, double cy, double r, double a0, double a1, int n) {
        for (int i = 0; i <= n; ++i) {
            const double a = a0 + (a1 - a0) * i / n;
            v.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
    };
    // a regular polygon (or star) with a point at the top, stretched to fill the box
    auto regular = [&](int points, double inner) {
        const int n = inner > 0 ? points * 2 : points;
        std::vector<std::pair<double, double>> u;
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (int i = 0; i < n; ++i) {
            const double a = -pi / 2 + 2 * pi * i / n;
            const double r = inner > 0 && i % 2 == 1 ? inner : 1.0;
            u.push_back({r * std::cos(a), r * std::sin(a)});
            minX = std::min(minX, u.back().first), maxX = std::max(maxX, u.back().first);
            minY = std::min(minY, u.back().second), maxY = std::max(maxY, u.back().second);
        }
        for (const auto& [ux, uy] : u) add((ux - minX) / (maxX - minX) * w, (uy - minY) / (maxY - minY) * h);
    };
    if (shape == "ellipse") {
        for (int i = 0; i < 72; ++i) v.push_back({x + w / 2 + w / 2 * std::cos(2 * pi * i / 72), y + h / 2 + h / 2 * std::sin(2 * pi * i / 72)});
    } else if (shape == "rounded") {
        const double r = m * 0.1667;
        arc(x + w - r, y + r, r, -pi / 2, 0, 6);
        arc(x + w - r, y + h - r, r, 0, pi / 2, 6);
        arc(x + r, y + h - r, r, pi / 2, pi, 6);
        arc(x + r, y + r, r, pi, 1.5 * pi, 6);
    } else if (shape == "triangle") {
        add(w / 2, 0), add(w, h), add(0, h);
    } else if (shape == "right_triangle") {
        add(0, 0), add(w, h), add(0, h);
    } else if (shape == "diamond") {
        add(w / 2, 0), add(w, h / 2), add(w / 2, h), add(0, h / 2);
    } else if (shape == "pentagon") {
        regular(5, 0);
    } else if (shape == "star") {
        regular(5, 0.382);
    } else if (shape == "hexagon") {
        const double a = m * 0.25;
        add(a, 0), add(w - a, 0), add(w, h / 2), add(w - a, h), add(a, h), add(0, h / 2);
    } else if (shape == "octagon") {
        const double a = m * 0.2929;
        add(a, 0), add(w - a, 0), add(w, a), add(w, h - a), add(w - a, h), add(a, h), add(0, h - a), add(0, a);
    } else if (shape == "arrow_right" || shape == "arrow_left") {
        const double hd = std::min(m * 0.5, w);
        const bool r = shape == "arrow_right";
        auto X = [&](double dx) { return r ? dx : w - dx; };
        add(X(0), h * 0.25), add(X(w - hd), h * 0.25), add(X(w - hd), 0), add(X(w), h / 2), add(X(w - hd), h), add(X(w - hd), h * 0.75), add(X(0), h * 0.75);
    } else if (shape == "arrow_up" || shape == "arrow_down") {
        const double hd = std::min(m * 0.5, h);
        const bool d = shape == "arrow_down";
        auto Y = [&](double dy) { return d ? dy : h - dy; };
        add(w * 0.25, Y(0)), add(w * 0.75, Y(0)), add(w * 0.75, Y(h - hd)), add(w, Y(h - hd)), add(w / 2, Y(h)), add(0, Y(h - hd)), add(w * 0.25, Y(h - hd));
    } else if (shape == "chevron") {
        const double a = std::min(m * 0.5, w / 2);
        add(0, 0), add(w - a, 0), add(w, h / 2), add(w - a, h), add(0, h), add(a, h / 2);
    } else if (shape == "parallelogram") {
        const double a = m * 0.25;
        add(a, 0), add(w, 0), add(w - a, h), add(0, h);
    } else if (shape == "trapezoid") {
        const double a = m * 0.25;
        add(a, 0), add(w - a, 0), add(w, h), add(0, h);
    } else if (shape == "plus") {
        const double a = m * 0.25;
        add(a, 0), add(w - a, 0), add(w - a, a), add(w, a), add(w, h - a), add(w - a, h - a), add(w - a, h), add(a, h), add(a, h - a), add(0, h - a),
            add(0, a), add(a, a);
    } else if (shape == "callout") {
        add(0, 0), add(w, 0), add(w, h), add(w * 0.4167, h), add(w * 0.2917, h * 1.125), add(w * 0.1667, h), add(0, h);
    } else {
        add(0, 0), add(w, 0), add(w, h), add(0, h);
    }
    return v;
}

// ---- placement ---------------------------------------------------------------------

namespace {

struct Frame {
    std::string name;  // Title, Subtitle, Body, Right, Slide Number
    double x = 0, y = 0, w = 0, h = 0;
    std::vector<ListItem> items;
    bool bullets = false;
    bool numbered = false;  // top-level bullets as 1. 2. 3.
    bool title = false;  // the slide's title placeholder
    double size = 24;
    bool bold = false;
    Color color{0x22, 0x22, 0x22, 255};
    Color marker{0x2B, 0x57, 0x9A, 255};
    Color link{0x05, 0x63, 0xC1, 255};
    int align = 0;   // 0 left, 1 centre, 2 right
    int anchor = 0;  // 0 top, 1 middle, 2 bottom
    double minScale = 0.4;
    double scale = 1;  // fitted
    bool overflow = false;
    std::string scheme;   // PowerPoint: the text colour is this theme colour (accent, text, background)
    bool object = false;  // a free object's text: PowerPoint shrinks it (fontScale) instead of us
};

std::optional<Color> paintColor(const std::string& spec, const Theme& t) {
    if (spec.empty()) return std::nullopt;
    if (spec == "accent") return t.accent;
    if (spec == "text") return t.text;
    if (spec == "background") return t.background;
    auto c = Color::parse(spec);
    if (c) c->a = 255;
    return c;
}

// A free object placed on a slide: its paints resolved and its text laid out.
struct Placed {
    const SlideObject* o = nullptr;
    double x = 0, y = 0, w = 0, h = 0;  // h resolved for auto-height text boxes
    std::optional<Color> fill, line;
    std::vector<std::pair<double, double>> outline;  // shapes
    bool hasText = false;
    Frame text;
};

struct Plan {
    Color background;
    std::vector<pdf::RectOp> rects;
    std::vector<Frame> frames;
    bool hasImage = false;
    double ix = 0, iy = 0, iw = 0, ih = 0;
    std::vector<Placed> back, front;  // master objects, slide objects
    bool showMaster = true;
};

double levelScale(int level) {
    static const double s[] = {1.0, 0.85, 0.75, 0.68, 0.62};
    return s[std::clamp(level, 0, 4)];
}

// Lays out a frame's text at `scale`; returns its height and draws it when `page` is set.
double flow(const Frame& f, pdf::Family family, double scale, pdf::Page* page) {
    struct Item {
        std::vector<textlayout::Line> lines;
        double size, indent, gap;
        int level;
    };
    std::vector<Item> laid;
    double total = 0;
    const double step = f.size * scale * 1.1;
    for (std::size_t i = 0; i < f.items.size(); ++i) {
        const auto& it = f.items[i];
        Item L;
        L.level = std::clamp(it.level, 0, 4);
        L.size = f.size * scale * (f.bullets ? levelScale(L.level) : 1.0);
        L.indent = f.bullets ? step * (L.level + 1) : 0;
        L.gap = i == 0 ? 0 : (f.bullets ? 0.45 * scale * L.size : 0.15 * L.size);
        textlayout::Style st;
        st.font = pdf::Font{family, f.bold, false};
        st.size = L.size;
        st.color = f.color;
        st.link = f.link;
        L.lines = textlayout::wrap(textlayout::pieces(it.runs, st), std::max(10.0, f.w - L.indent), L.size);
        total += L.gap;
        for (const auto& l : L.lines) total += 1.2 * l.size;
        laid.push_back(std::move(L));
    }
    if (!page) return total;
    double y = f.y;
    if (f.anchor == 1) y += (f.h - total) / 2;
    else if (f.anchor == 2) y += f.h - total;
    int number = 0;
    for (const auto& L : laid) {
        y += L.gap;
        bool first = true;
        if (L.level == 0) ++number;
        for (const auto& l : L.lines) {
            double x = f.x + L.indent;
            const double avail = f.w - L.indent;
            if (f.align == 1) x += (avail - l.width) / 2;
            else if (f.align == 2) x += avail - l.width;
            if (first && f.bullets) {
                pdf::Font mf{family, false, false};
                if (f.numbered && L.level == 0) {
                    const std::string m = std::to_string(number) + ".";
                    const double mw = pdf::textWidth(mf, m, L.size);
                    page->text(f.x + L.indent - step * 0.25 - mw, y + 0.1 * l.size + l.size * 0.92, m, mf, L.size, f.marker);
                } else {
                    const char* m = L.level % 2 == 0 ? "\xE2\x80\xA2" : "\xE2\x80\x93";
                    page->text(f.x + L.indent - step * 0.8, y + 0.1 * l.size + l.size * 0.92, m, mf, L.size, f.marker);
                }
            }
            first = false;
            textlayout::drawLine(*page, l, x, y + 0.1 * l.size);
            y += 1.2 * l.size;
        }
    }
    return total;
}

void fit(Frame& f, pdf::Family family) {
    for (double s = 1.0; s >= f.minScale - 1e-9; s -= 0.05) {
        f.scale = s;
        if (flow(f, family, s, nullptr) <= f.h + 0.5) return;
    }
    f.scale = f.minScale;
    f.overflow = true;
}

Placed placeObject(const SlideObject& o, const Theme& t, Color slideBg) {
    Placed P;
    P.o = &o;
    P.x = o.x, P.y = o.y, P.w = o.w, P.h = o.h;
    P.fill = paintColor(o.fill, t);
    P.line = paintColor(o.line, t);
    if (o.kind == "line" || o.kind == "image") return P;
    std::vector<ListItem> items;
    bool numbered = false;
    if (o.bullets) {
        items = parseBullets(o.text, &numbered);
    } else {
        for (const auto& line : splitLines(o.text)) {
            if (!trimCopy(line).empty()) items.push_back(ListItem{0, parseInline(line)});
        }
    }
    if (!items.empty()) {
        Frame f;
        f.name = "Object";
        f.object = true;
        f.x = o.x + 7.2;
        f.y = o.y + 3.6;
        f.w = std::max(10.0, o.w - 14.4);
        f.size = std::clamp(o.size, 4.0, 200.0);
        f.bold = o.bold;
        f.align = o.align;
        f.anchor = o.anchor;
        f.bullets = o.bullets;
        f.numbered = numbered;
        f.minScale = 0.5;
        if (o.italic)
            for (auto& it : items)
                for (auto& r : it.runs) r.italic = true;
        f.items = std::move(items);
        const Color dark{0x22, 0x22, 0x22, 255}, white{255, 255, 255, 255};
        const Color under = P.fill ? *P.fill : slideBg;
        if (auto c = paintColor(o.color, t)) {
            f.color = *c;
            if (o.color[0] != '#') f.scheme = o.color;
        } else if (!P.fill && under == t.background) {
            f.color = t.text;
        } else {
            f.color = luminance(under) > 0.6 ? dark : white;
        }
        f.marker = P.fill ? f.color : t.accent;
        f.link = luminance(under) < 0.4 ? mix(t.accent, white, 0.3) : Color{0x05, 0x63, 0xC1, 255};
        if (o.h <= 0) {
            f.h = flow(f, t.font, 1, nullptr);
            P.h = f.h + 7.2;
        } else {
            f.h = std::max(1.0, o.h - 7.2);
            fit(f, t.font);
        }
        P.text = std::move(f);
        P.hasText = true;
    } else if (o.h <= 0) {
        P.h = o.size * 1.2 + 7.2;
    }
    if (o.kind == "shape") P.outline = shapeOutline(o.shape, P.x, P.y, P.w, P.h);
    return P;
}

void drawPlaced(pdf::Page& page, const Placed& P, pdf::Family family) {
    const SlideObject& o = *P.o;
    if (o.kind == "line") {
        if (!P.line) return;
        double x2 = P.x + P.w, y2 = P.y + P.h;
        const double len = std::hypot(P.w, P.h);
        if (o.arrow && len > 0.1) {
            const double L = std::min(len, std::max(7.0, o.lineWidth * 4)), ux = P.w / len, uy = P.h / len, hw = L * 0.5;
            const double bx = x2 - ux * L, by = y2 - uy * L;
            page.path({{x2, y2}, {bx - uy * hw, by + ux * hw}, {bx + uy * hw, by - ux * hw}}, true, *P.line);
            x2 = bx, y2 = by;
        }
        page.line(P.x, P.y, x2, y2, *P.line, o.lineWidth);
        return;
    }
    if (o.kind == "image") {
        if (o.pixels && !o.pixels->pixels.empty()) page.image(P.x, P.y, P.w, P.h, o.pixels);
        if (P.line) page.rect(P.x, P.y, P.w, P.h, std::nullopt, P.line, o.lineWidth);
        return;
    }
    if (P.fill || P.line) {
        if (o.kind == "shape") page.path(P.outline, true, P.fill, P.line, o.lineWidth);
        else page.rect(P.x, P.y, P.w, P.h, P.fill, P.line, o.lineWidth);
    }
    if (P.hasText) flow(P.text, family, P.text.scale, &page);
}

Plan plan(const Deck& d, std::size_t index) {
    const Slide& s = d.slides.at(index);
    const Theme& t = d.theme;
    const double sx = d.width / 960.0, sy = d.height / 540.0, fs = std::min(sx, sy);
    Plan P;
    std::string layout = s.layout;
    if (std::find(slideLayouts().begin(), slideLayouts().end(), layout) == slideLayouts().end()) layout = "content";
    const bool image = s.pixels && !s.pixels->pixels.empty();
    if (layout == "content" && image) layout = s.bullets.empty() ? "image" : "image_right";
    const bool titleLike = layout == "title" || layout == "section";
    P.background = titleLike ? t.accent : t.background;
    if (auto c = paintColor(s.background, t)) P.background = *c;
    const Color white{255, 255, 255, 255};
    const Color on = luminance(P.background) > 0.62 ? Color{0x22, 0x22, 0x22, 255} : white;
    Color ink = titleLike ? on : t.text;
    if (!titleLike && !s.background.empty() && std::abs(luminance(ink) - luminance(P.background)) < 0.4) ink = on;
    const Color muted = mix(ink, P.background, 0.4);
    auto frame = [&](const std::string& name, double x, double y, double w, double h, double size) {
        Frame f;
        f.name = name;
        f.x = x * sx;
        f.y = y * sy;
        f.w = w * sx;
        f.h = h * sy;
        f.size = size * fs;
        f.color = ink;
        f.marker = t.accent;
        f.link = luminance(P.background) < 0.4 ? mix(t.accent, white, 0.3) : Color{0x05, 0x63, 0xC1, 255};
        return f;
    };
    auto rect = [&](double x, double y, double w, double h, Color c) {
        pdf::RectOp r;
        r.x = x * sx;
        r.y = y * sy;
        r.w = w * sx;
        r.h = h * sy;
        r.fill = c;
        P.rects.push_back(r);
    };
    auto textItems = [](const std::string& text) {
        std::vector<ListItem> v;
        if (!text.empty()) v.push_back(ListItem{0, parseInline(text)});
        return v;
    };
    auto placeImage = [&](double x, double y, double w, double h, bool top = false) {
        if (!s.pixels || s.pixels->pixels.empty()) return;
        x *= sx, y *= sy, w *= sx, h *= sy;
        const double pw = s.pixels->pixels.width(), ph = s.pixels->pixels.height();
        double iw = w, ih = w * ph / pw;
        if (ih > h) {
            ih = h;
            iw = h * pw / ph;
        }
        P.hasImage = true;
        P.ix = x + (w - iw) / 2;
        P.iy = top ? y : y + (h - ih) / 2;
        P.iw = iw;
        P.ih = ih;
    };


    if (titleLike) {
        const Color soft = mix(on, P.background, 0.18);
        const bool title = layout == "title";
        Frame tf = title ? frame("Title", 60, 120, 840, 190, 50) : frame("Title", 80, 130, 800, 180, 44);
        tf.title = true;
        tf.bold = true;
        tf.color = on;
        tf.link = on;
        tf.align = title ? 1 : 0;
        tf.anchor = 2;
        tf.minScale = 0.4;
        tf.items = textItems(s.title);
        Frame sf = title ? frame("Subtitle", 60, 346, 840, 140, 24) : frame("Subtitle", 80, 342, 800, 130, 22);
        sf.color = soft;
        sf.link = on;
        sf.align = tf.align;
        sf.minScale = 0.5;
        for (const auto& line : splitLines(s.subtitle)) sf.items.push_back(ListItem{0, parseInline(line)});
        if (title) rect(420, 328, 120, 3, soft);
        else rect(80, 322, 90, 5, soft);
        if (!tf.items.empty()) P.frames.push_back(tf);
        if (!sf.items.empty()) P.frames.push_back(sf);
    } else if (layout == "blank") {
        placeImage(0, 0, 960, 540);
    } else {
        double top = 40;
        if (!s.title.empty()) {
            Frame tf = frame("Title", 48, 26, 864, 76, 34);
            tf.title = true;
            tf.bold = true;
            tf.color = t.accent;
            tf.anchor = 2;
            tf.minScale = 0.55;
            tf.items = textItems(s.title);
            P.frames.push_back(tf);
            rect(48, 108, 72, 4, t.accent);
            top = 126;
        }
        if (!s.subtitle.empty()) {
            Frame sf = frame("Subtitle", 48, top, 864, 34, 20);
            sf.color = muted;
            sf.items = textItems(s.subtitle);
            sf.minScale = 0.6;
            P.frames.push_back(sf);
            top += 42;
        }
        const double bottom = 492, h = bottom - top;
        auto body = [&](const std::string& name, double x, double w, const std::vector<ListItem>& items) {
            if (items.empty()) return;
            Frame b = frame(name, x, top, w, h, 24);
            b.bullets = true;
            b.numbered = s.numbered;
            b.items = items;
            P.frames.push_back(b);
        };
        if (layout == "content") {
            body("Body", 48, 864, s.bullets);
        } else if (layout == "two_column") {
            body("Body", 48, 420, s.bullets);
            body("Right", 492, 420, s.right);
        } else if (layout == "image") {
            placeImage(48, top, 864, h);
        } else if (layout == "image_right") {
            if (image) {
                body("Body", 48, 500, s.bullets);
                placeImage(572, top, 340, h, true);
            } else {
                body("Body", 48, 864, s.bullets);
            }
        }
    }
    if (!titleLike || d.masterOnTitle) {
        if (d.slideNumbers) {
            Frame n = frame("Slide Number", 812, 506, 100, 20, 12);
            n.color = muted;
            n.align = 2;
            n.items = textItems(std::to_string(index + 1));
            P.frames.push_back(n);
        }
        if (!d.footer.empty()) {
            Frame n = frame("Footer", 48, 506, 700, 20, 12);
            n.color = muted;
            n.items = textItems(d.footer);
            P.frames.push_back(n);
        }
    }
    for (auto& f : P.frames) fit(f, t.font);
    P.showMaster = !titleLike || d.masterOnTitle;
    if (P.showMaster)
        for (const auto& o : d.master) P.back.push_back(placeObject(o, t, P.background));
    for (const auto& o : s.objects) P.front.push_back(placeObject(o, t, P.background));
    return P;
}

}  // namespace

double objectHeight(const Deck& deck, const SlideObject& o) { return placeObject(o, deck.theme, deck.theme.background).h; }

bool objectOverflows(const Deck& deck, const SlideObject& o) {
    const Placed P = placeObject(o, deck.theme, deck.theme.background);
    return P.hasText && P.text.overflow;
}

pdf::Page layoutSlide(const Deck& deck, std::size_t index) {
    Plan P = plan(deck, index);
    pdf::Page page;
    page.width = deck.width;
    page.height = deck.height;
    page.rect(0, 0, deck.width, deck.height, P.background);
    for (const auto& o : P.back) drawPlaced(page, o, deck.theme.font);
    for (const auto& r : P.rects) page.ops.emplace_back(r);
    if (P.hasImage) page.image(P.ix, P.iy, P.iw, P.ih, deck.slides[index].pixels);
    for (const auto& f : P.frames) flow(f, deck.theme.font, f.scale, &page);
    for (const auto& o : P.front) drawPlaced(page, o, deck.theme.font);
    return page;
}

SlideHit slideHit(const Deck& deck, std::size_t index, double x, double y) {
    SlideHit hit;
    if (index >= deck.slides.size()) return hit;
    const Slide& s = deck.slides[index];
    const Plan P = plan(deck, index);
    auto inside = [&](double bx, double by, double bw, double bh, double pad) {
        const double x0 = std::min(bx, bx + bw), x1 = std::max(bx, bx + bw), y0 = std::min(by, by + bh), y1 = std::max(by, by + bh);
        return x >= x0 - pad && x <= x1 + pad && y >= y0 - pad && y <= y1 + pad;
    };
    for (std::size_t i = P.front.size(); i-- > 0;) {
        const Placed& o = P.front[i];
        if (!inside(o.x, o.y, o.w, o.h, o.o->kind == "line" ? 6 : 0)) continue;
        hit.kind = "object";
        hit.object = static_cast<int>(i + 1);
        hit.x = std::min(o.x, o.x + o.w), hit.y = std::min(o.y, o.y + o.h), hit.w = std::abs(o.w), hit.h = std::abs(o.h);
        hit.text = o.o->text;
        return hit;
    }
    auto take = [&](const std::string& kind, double bx, double by, double bw, double bh) {
        hit.kind = kind;
        hit.x = bx, hit.y = by, hit.w = bw, hit.h = bh;
        if (kind == "title") hit.text = s.title;
        else if (kind == "subtitle") hit.text = s.subtitle;
        else if (kind == "bullets") hit.text = bulletText(s.bullets, s.numbered);
        else hit.text = bulletText(s.right);
        return hit;
    };
    for (const auto& f : P.frames) {
        const std::string kind = f.name == "Title" ? "title" : f.name == "Subtitle" ? "subtitle" : f.name == "Body" ? "bullets" : f.name == "Right" ? "right" : "";
        if (!kind.empty() && inside(f.x, f.y, f.w, f.h, 0)) return take(kind, f.x, f.y, f.w, f.h);
    }
    // the empty placeholders, where the layout puts them
    const double sx = deck.width / 960.0, sy = deck.height / 540.0;
    std::string layout = s.layout;
    if (std::find(slideLayouts().begin(), slideLayouts().end(), layout) == slideLayouts().end()) layout = "content";
    const bool titleLike = layout == "title" || layout == "section";
    if (layout == "blank") return hit;
    if (titleLike) {
        const bool title = layout == "title";
        if (s.title.empty() && inside(sx * (title ? 60 : 80), sy * 120, sx * (title ? 840 : 800), sy * 200, 0)) return take("title", sx * (title ? 60 : 80), sy * 120, sx * (title ? 840 : 800), sy * 190);
        if (s.subtitle.empty() && inside(sx * (title ? 60 : 80), sy * 330, sx * (title ? 840 : 800), sy * 160, 0)) return take("subtitle", sx * (title ? 60 : 80), sy * 342, sx * (title ? 840 : 800), sy * 140);
        return hit;
    }
    if (s.title.empty() && inside(48 * sx, 20 * sy, 864 * sx, 90 * sy, 0)) return take("title", 48 * sx, 26 * sy, 864 * sx, 76 * sy);
    const bool bodyLayout = layout == "content" || layout == "two_column" || layout == "image_right";
    if (bodyLayout && inside(48 * sx, 110 * sy, 864 * sx, 382 * sy, 0)) {
        if (layout == "two_column" && x > 480 * sx) return s.right.empty() ? take("right", 492 * sx, 126 * sy, 420 * sx, 366 * sy) : hit;
        if (s.bullets.empty()) return take("bullets", 48 * sx, 126 * sy, (layout == "content" ? 864 : layout == "two_column" ? 420 : 500) * sx, 366 * sy);
    }
    return hit;
}

std::vector<std::string> slideOverflow(const Deck& deck, std::size_t index) {
    std::vector<std::string> out;
    const Plan P = plan(deck, index);
    for (const auto& f : P.frames) {
        if (f.overflow) out.push_back(f.name == "Body" ? "bullets" : f.name == "Right" ? "right" : lower(f.name));
    }
    for (std::size_t i = 0; i < P.front.size(); ++i) {
        if (P.front[i].hasText && P.front[i].text.overflow) {
            const std::string& name = P.front[i].o->name;
            out.push_back("object " + std::to_string(i + 1) + (name.empty() ? "" : " (" + name + ")"));
        }
    }
    return out;
}

std::vector<pdf::Page> layoutDeck(const Deck& deck) {
    std::vector<pdf::Page> pages;
    for (std::size_t i = 0; i < deck.slides.size(); ++i) {
        if (!deck.slides[i].hidden) pages.push_back(layoutSlide(deck, i));
    }
    return pages;
}

// ---- PowerPoint --------------------------------------------------------------------

namespace {

const char* kA = "http://schemas.openxmlformats.org/drawingml/2006/main";
const char* kR = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
const char* kP = "http://schemas.openxmlformats.org/presentationml/2006/main";
const std::string kRel = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
const std::string kCt = "application/vnd.openxmlformats-officedocument.presentationml.";

std::string emu(double pt) { return std::to_string(static_cast<long long>(std::llround(pt * 12700))); }

const char* typeface(pdf::Family f) {
    switch (f) {
        case pdf::Family::Serif: return "Times New Roman";
        case pdf::Family::Mono: return "Courier New";
        default: return "Arial";
    }
}

void ns(xml::Writer& w) { w.attr("xmlns:a", kA).attr("xmlns:r", kR).attr("xmlns:p", kP); }

void groupProps(xml::Writer& w) {
    w.open("p:nvGrpSpPr").leaf("p:cNvPr", {{"id", "1"}, {"name", ""}}).leaf("p:cNvGrpSpPr").leaf("p:nvPr").close();
    w.open("p:grpSpPr").open("a:xfrm");
    w.leaf("a:off", {{"x", "0"}, {"y", "0"}}).leaf("a:ext", {{"cx", "0"}, {"cy", "0"}});
    w.leaf("a:chOff", {{"x", "0"}, {"y", "0"}}).leaf("a:chExt", {{"cx", "0"}, {"cy", "0"}});
    w.close().close();
}

void xfrm(xml::Writer& w, double x, double y, double cx, double cy) {
    w.open("a:xfrm").leaf("a:off", {{"x", emu(x)}, {"y", emu(y)}}).leaf("a:ext", {{"cx", emu(cx)}, {"cy", emu(cy)}}).close();
}

void solid(xml::Writer& w, Color c) { w.open("a:solidFill").leaf("a:srgbClr", {{"val", hex6(c)}}).close(); }

void background(xml::Writer& w, Color c) {
    w.open("p:bg").open("p:bgPr");
    solid(w, c);
    w.leaf("a:effectLst");
    w.close().close();
}

std::string relsXml(const std::vector<std::array<std::string, 3>>& rels) {
    xml::Writer w;
    w.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    for (const auto& r : rels) {
        w.open("Relationship").attr("Id", r[0]).attr("Type", r[1]).attr("Target", r[2]);
        if (startsWith(r[2], "http://") || startsWith(r[2], "https://") || startsWith(r[2], "mailto:")) w.attr("TargetMode", "External");
        w.close();
    }
    return w.str();
}

std::string themeXml(const Theme& t, const std::string& name) {
    const std::string face = typeface(t.font);
    xml::Writer w;
    w.open("a:theme").attr("xmlns:a", kA).attr("name", name);
    w.open("a:themeElements");
    w.open("a:clrScheme").attr("name", name);
    auto clr = [&](const char* tag, const std::string& val) { w.open(tag).leaf("a:srgbClr", {{"val", val}}).close(); };
    clr("a:dk1", hex6(t.text));
    clr("a:lt1", hex6(t.background));
    clr("a:dk2", "44546A");
    clr("a:lt2", "E7E6E6");
    clr("a:accent1", hex6(t.accent));
    clr("a:accent2", "ED7D31");
    clr("a:accent3", "A5A5A5");
    clr("a:accent4", "FFC000");
    clr("a:accent5", "5B9BD5");
    clr("a:accent6", "70AD47");
    clr("a:hlink", "0563C1");
    clr("a:folHlink", "954F72");
    w.close();
    w.open("a:fontScheme").attr("name", name);
    for (const char* f : {"a:majorFont", "a:minorFont"}) {
        w.open(f).leaf("a:latin", {{"typeface", face}}).leaf("a:ea", {{"typeface", ""}}).leaf("a:cs", {{"typeface", ""}}).close();
    }
    w.close();
    w.open("a:fmtScheme").attr("name", name);
    w.open("a:fillStyleLst");
    for (int i = 0; i < 3; ++i) w.open("a:solidFill").leaf("a:schemeClr", {{"val", "phClr"}}).close();
    w.close();
    w.open("a:lnStyleLst");
    for (const char* width : {"6350", "12700", "19050"}) {
        w.open("a:ln").attr("w", width).open("a:solidFill").leaf("a:schemeClr", {{"val", "phClr"}}).close().close();
    }
    w.close();
    w.open("a:effectStyleLst");
    for (int i = 0; i < 3; ++i) w.open("a:effectStyle").leaf("a:effectLst").close();
    w.close();
    w.open("a:bgFillStyleLst");
    for (int i = 0; i < 3; ++i) w.open("a:solidFill").leaf("a:schemeClr", {{"val", "phClr"}}).close();
    w.close();
    w.close();  // fmtScheme
    w.close();  // themeElements
    w.leaf("a:objectDefaults").leaf("a:extraClrSchemeLst");
    return w.str();
}

std::string layoutXml() {
    xml::Writer w;
    w.open("p:sldLayout");
    ns(w);
    w.attr("type", "titleOnly").attr("preserve", "1");
    w.open("p:cSld").attr("name", "Title Only").open("p:spTree");
    groupProps(w);
    w.open("p:sp");
    w.open("p:nvSpPr").leaf("p:cNvPr", {{"id", "2"}, {"name", "Title 1"}});
    w.open("p:cNvSpPr").leaf("a:spLocks", {{"noGrp", "1"}}).close();
    w.open("p:nvPr").leaf("p:ph", {{"type", "title"}}).close();
    w.close();
    w.leaf("p:spPr");
    w.open("p:txBody").leaf("a:bodyPr").leaf("a:lstStyle").open("a:p").leaf("a:endParaRPr", {{"lang", "en-US"}}).close().close();
    w.close();
    w.close().close();
    w.open("p:clrMapOvr").leaf("a:masterClrMapping").close();
    return w.str();
}

std::string notesMasterXml(const Deck& d) {
    (void)d;
    xml::Writer w;
    w.open("p:notesMaster");
    ns(w);
    w.open("p:cSld");
    w.open("p:bg").open("p:bgRef").attr("idx", "1001").leaf("a:schemeClr", {{"val", "bg1"}}).close().close();
    w.open("p:spTree");
    groupProps(w);
    w.open("p:sp");
    w.open("p:nvSpPr").leaf("p:cNvPr", {{"id", "2"}, {"name", "Notes Placeholder 1"}});
    w.open("p:cNvSpPr").leaf("a:spLocks", {{"noGrp", "1"}}).close();
    w.open("p:nvPr").leaf("p:ph", {{"type", "body"}, {"sz", "quarter"}, {"idx", "1"}}).close();
    w.close();
    w.open("p:spPr");
    xfrm(w, 54, 360, 432, 324);
    w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
    w.close();
    w.open("p:txBody").leaf("a:bodyPr").leaf("a:lstStyle").open("a:p").leaf("a:endParaRPr", {{"lang", "en-US"}}).close().close();
    w.close();
    w.close().close();
    w.leaf("p:clrMap", {{"bg1", "lt1"}, {"tx1", "dk1"}, {"bg2", "lt2"}, {"tx2", "dk2"}, {"accent1", "accent1"}, {"accent2", "accent2"},
                        {"accent3", "accent3"}, {"accent4", "accent4"}, {"accent5", "accent5"}, {"accent6", "accent6"}, {"hlink", "hlink"},
                        {"folHlink", "folHlink"}});
    return w.str();
}

std::string notesXml(const std::string& notes) {
    xml::Writer w;
    w.open("p:notes");
    ns(w);
    w.open("p:cSld").open("p:spTree");
    groupProps(w);
    w.open("p:sp");
    w.open("p:nvSpPr").leaf("p:cNvPr", {{"id", "2"}, {"name", "Notes Placeholder 1"}});
    w.open("p:cNvSpPr").leaf("a:spLocks", {{"noGrp", "1"}}).close();
    w.open("p:nvPr").leaf("p:ph", {{"type", "body"}, {"idx", "1"}}).close();
    w.close();
    w.leaf("p:spPr");
    w.open("p:txBody").leaf("a:bodyPr").leaf("a:lstStyle");
    for (const auto& line : splitLines(notes)) {
        w.open("a:p");
        if (!line.empty()) w.open("a:r").leaf("a:rPr", {{"lang", "en-US"}, {"dirty", "0"}}).element("a:t", line).close();
        w.close();
    }
    if (splitLines(notes).empty()) w.open("a:p").close();
    w.close();
    w.close();
    w.close().close();
    w.open("p:clrMapOvr").leaf("a:masterClrMapping").close();
    return w.str();
}

bool isJpegBytes(const std::string& b) { return b.size() > 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8; }

const std::vector<std::pair<std::string, std::string>>& prstNames() {
    static const std::vector<std::pair<std::string, std::string>> m = {
        {"rect", "rect"},           {"rounded", "roundRect"},        {"ellipse", "ellipse"},     {"triangle", "triangle"},
        {"right_triangle", "rtTriangle"}, {"diamond", "diamond"},    {"pentagon", "pentagon"},   {"hexagon", "hexagon"},
        {"octagon", "octagon"},     {"star", "star5"},               {"arrow_right", "rightArrow"}, {"arrow_left", "leftArrow"},
        {"arrow_up", "upArrow"},    {"arrow_down", "downArrow"},     {"chevron", "chevron"},     {"parallelogram", "parallelogram"},
        {"trapezoid", "trapezoid"}, {"plus", "plus"},                {"callout", "wedgeRectCallout"}};
    return m;
}

std::string prstOf(const std::string& shape) {
    for (const auto& [ours, theirs] : prstNames())
        if (ours == shape) return theirs;
    return "rect";
}

std::string shapeOfPrst(const std::string& prst) {
    for (const auto& [ours, theirs] : prstNames())
        if (theirs == prst) return ours;
    if (prst == "wedgeRoundRectCallout" || prst == "wedgeEllipseCallout") return "callout";
    if (prst == "flowChartAlternateProcess" || prst == "round2SameRect" || prst == "snipRoundRect") return "rounded";
    if (prst == "flowChartConnector") return "ellipse";
    if (prst == "flowChartDecision") return "diamond";
    if (prst == "homePlate") return "chevron";
    if (prst == "star4" || prst == "star6" || prst == "star7" || prst == "star8") return "star";
    if (prst == "cross") return "plus";
    return "rect";
}

// A fill for a paint spec: theme colours stay theme colours in PowerPoint.
void paintFill(xml::Writer& w, const std::string& spec, Color c) {
    const char* scheme = spec == "accent" ? "accent1" : spec == "text" ? "tx1" : spec == "background" ? "bg1" : nullptr;
    if (scheme) w.open("a:solidFill").leaf("a:schemeClr", {{"val", scheme}}).close();
    else solid(w, c);
}

const char* transitionSpeed(double seconds) { return seconds <= 0.55 ? "fast" : seconds <= 0.85 ? "med" : "slow"; }

struct SlideWriter {
    const Deck& deck;
    std::size_t index;
    xml::Writer w;
    std::vector<std::array<std::string, 3>> rels;
    std::map<std::string, std::string> links;
    int nextId = 2;

    std::string rel(const std::string& type, const std::string& target) {
        std::string id = "rId" + std::to_string(rels.size() + 1);
        rels.push_back({id, kRel + type, target});
        return id;
    }

    void run(const Run& r, double size, const Frame& f) {
        auto emit = [&](const std::string& text) {
            w.open("a:r").open("a:rPr").attr("lang", "en-US").attr("sz", std::to_string(std::max(100L, std::lround(size * 100))));
            if (f.bold || r.bold) w.attr("b", "1");
            if (r.italic) w.attr("i", "1");
            if (r.underline || !r.link.empty()) w.attr("u", "sng");
            w.attr("dirty", "0");
            if (r.link.empty() && !f.scheme.empty()) paintFill(w, f.scheme, f.color);
            else solid(w, r.link.empty() ? f.color : f.link);
            w.leaf("a:latin", {{"typeface", r.code ? "Courier New" : typeface(deck.theme.font)}});
            if (!r.link.empty()) {
                auto it = links.find(r.link);
                std::string id = it != links.end() ? it->second : (links[r.link] = rel("hyperlink", r.link));
                w.leaf("a:hlinkClick", {{"r:id", id}});
            }
            w.close();
            w.element("a:t", text);
            w.close();
        };
        std::string cur;
        for (char c : r.text) {
            if (c == '\n') {
                if (!cur.empty()) emit(cur);
                cur.clear();
                w.leaf("a:br");
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) emit(cur);
    }

    // p:txBody for a frame. Layout frames are written at their fitted size; a free object's
    // text keeps its size and PowerPoint is told to shrink it (or to grow the box).
    void body(const Frame& f, bool autoHeight) {
        w.open("p:txBody");
        static const char* anchors[] = {"t", "ctr", "b"};
        w.open("a:bodyPr").attr("wrap", "square");
        if (!f.object) w.attr("lIns", "0").attr("tIns", "0").attr("rIns", "0").attr("bIns", "0");
        w.attr("anchor", anchors[std::clamp(f.anchor, 0, 2)]);
        if (f.object && autoHeight) w.leaf("a:spAutoFit");
        else if (f.object && f.scale < 0.999) w.leaf("a:normAutofit", {{"fontScale", std::to_string(std::lround(f.scale * 100) * 1000)}});
        else w.leaf("a:noAutofit");
        w.close();
        w.leaf("a:lstStyle");
        static const char* aligns[] = {"l", "ctr", "r"};
        const double k = f.object ? 1.0 : f.scale;
        const double step = f.size * k * 1.1;
        for (std::size_t i = 0; i < f.items.size(); ++i) {
            const auto& it = f.items[i];
            const int level = std::clamp(it.level, 0, 4);
            const double size = f.size * k * (f.bullets ? levelScale(level) : 1.0);
            w.open("a:p").open("a:pPr");
            if (f.bullets) w.attr("marL", emu(step * (level + 1))).attr("indent", emu(-step * (f.numbered && level == 0 ? 1.0 : 0.8))).attr("lvl", std::to_string(level));
            else w.attr("marL", "0").attr("indent", "0");
            w.attr("algn", aligns[std::clamp(f.align, 0, 2)]);
            w.open("a:lnSpc").leaf("a:spcPct", {{"val", "100000"}}).close();
            const double gap = i == 0 ? 0 : (f.bullets ? 0.45 * k * size : 0.15 * size);
            w.open("a:spcBef").leaf("a:spcPts", {{"val", std::to_string(std::lround(gap * 100))}}).close();
            if (f.bullets) {
                w.open("a:buClr").leaf("a:srgbClr", {{"val", hex6(f.marker)}}).close();
                w.leaf("a:buFont", {{"typeface", "Arial"}});
                if (f.numbered && level == 0) w.leaf("a:buAutoNum", {{"type", "arabicPeriod"}});
                else w.leaf("a:buChar", {{"char", level % 2 == 0 ? "\xE2\x80\xA2" : "\xE2\x80\x93"}});
            } else {
                w.leaf("a:buNone");
            }
            w.close();
            for (const auto& r : it.runs) run(r, size, f);
            w.leaf("a:endParaRPr", {{"lang", "en-US"}, {"sz", std::to_string(std::max(100L, std::lround(size * 100)))}});
            w.close();
        }
        w.close();  // txBody
    }

    void frame(const Frame& f) {
        const int id = nextId++;
        w.open("p:sp");
        w.open("p:nvSpPr").leaf("p:cNvPr", {{"id", std::to_string(id)}, {"name", f.name + " " + std::to_string(id - 1)}});
        if (f.title) {
            w.open("p:cNvSpPr").leaf("a:spLocks", {{"noGrp", "1"}}).close();
            w.open("p:nvPr").leaf("p:ph", {{"type", "title"}}).close();
        } else {
            w.leaf("p:cNvSpPr", {{"txBox", "1"}});
            w.leaf("p:nvPr");
        }
        w.close();
        w.open("p:spPr");
        xfrm(w, f.x, f.y, f.w, f.h);
        w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
        w.leaf("a:noFill");
        w.close();
        body(f, false);
        w.close();  // sp
    }

    void rect(const pdf::RectOp& r) {
        const int id = nextId++;
        w.open("p:sp");
        w.open("p:nvSpPr").leaf("p:cNvPr", {{"id", std::to_string(id)}, {"name", "Accent " + std::to_string(id - 1)}}).leaf("p:cNvSpPr").leaf("p:nvPr").close();
        w.open("p:spPr");
        xfrm(w, r.x, r.y, r.w, r.h);
        w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
        if (r.fill) solid(w, *r.fill);
        w.open("a:ln").leaf("a:noFill").close();
        w.close();
        w.close();
    }

    std::string media(const std::string& image, const raster::Image& pixels, std::vector<raster::ZipEntry>& files, int& counter) {
        const bool jpeg = isJpegBytes(image);
        std::string name = "image" + std::to_string(++counter) + (jpeg ? ".jpeg" : ".png");
        std::string bytes = image;
        if (!jpeg && !(bytes.size() > 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)) bytes = raster::encodePng(pixels);
        files.push_back({"ppt/media/" + name, bytes});
        return rel("image", "../media/" + name);
    }

    void picture(const Plan& P, const std::string& rid) {
        const int id = nextId++;
        w.open("p:pic");
        w.open("p:nvPicPr").leaf("p:cNvPr", {{"id", std::to_string(id)}, {"name", "Picture " + std::to_string(id - 1)}});
        w.open("p:cNvPicPr").leaf("a:picLocks", {{"noChangeAspect", "1"}}).close().leaf("p:nvPr").close();
        w.open("p:blipFill").leaf("a:blip", {{"r:embed", rid}}).open("a:stretch").leaf("a:fillRect").close().close();
        w.open("p:spPr");
        xfrm(w, P.ix, P.iy, P.iw, P.ih);
        w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
        w.close();
        w.close();
    }

    void lineProps(const Placed& P) {
        const SlideObject& o = *P.o;
        if (!P.line) {
            w.open("a:ln").leaf("a:noFill").close();
            return;
        }
        w.open("a:ln").attr("w", emu(o.lineWidth));
        paintFill(w, o.line, *P.line);
        if (o.kind == "line" && o.arrow) w.leaf("a:tailEnd", {{"type", "triangle"}});
        w.close();
    }

    // A free object: p:sp (text box or shape), p:pic or p:cxnSp (line).
    void object(const Placed& P, std::vector<raster::ZipEntry>& files, int& counter) {
        const SlideObject& o = *P.o;
        const int id = nextId++;
        const char* base = o.kind == "text" ? "Text Box " : o.kind == "image" ? "Image " : o.kind == "line" ? "Line " : "Shape ";
        const std::string name = o.name.empty() ? base + std::to_string(id - 1) : o.name;
        const std::vector<std::pair<std::string, std::string>> cNvPr = {{"id", std::to_string(id)}, {"name", name}};
        if (o.kind == "image") {
            if (!o.pixels || o.image.empty()) return;
            const std::string rid = media(o.image, o.pixels->pixels, files, counter);
            w.open("p:pic");
            w.open("p:nvPicPr").leaf("p:cNvPr", cNvPr);
            w.open("p:cNvPicPr").leaf("a:picLocks", {{"noChangeAspect", "1"}}).close().leaf("p:nvPr").close();
            w.open("p:blipFill").leaf("a:blip", {{"r:embed", rid}}).open("a:stretch").leaf("a:fillRect").close().close();
            w.open("p:spPr");
            xfrm(w, P.x, P.y, P.w, P.h);
            w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
            if (P.line) lineProps(P);
            w.close();
            w.close();
            return;
        }
        if (o.kind == "line") {
            w.open("p:cxnSp");
            w.open("p:nvCxnSpPr").leaf("p:cNvPr", cNvPr).leaf("p:cNvCxnSpPr").leaf("p:nvPr").close();
            w.open("p:spPr");
            w.open("a:xfrm");
            if (P.w < 0) w.attr("flipH", "1");
            if (P.h < 0) w.attr("flipV", "1");
            w.leaf("a:off", {{"x", emu(std::min(P.x, P.x + P.w))}, {"y", emu(std::min(P.y, P.y + P.h))}});
            w.leaf("a:ext", {{"cx", emu(std::abs(P.w))}, {"cy", emu(std::abs(P.h))}});
            w.close();
            w.open("a:prstGeom").attr("prst", "line").leaf("a:avLst").close();
            lineProps(P);
            w.close();
            w.close();
            return;
        }
        w.open("p:sp");
        w.open("p:nvSpPr").leaf("p:cNvPr", cNvPr);
        if (o.kind == "text") w.leaf("p:cNvSpPr", {{"txBox", "1"}});
        else w.leaf("p:cNvSpPr");
        w.leaf("p:nvPr").close();
        w.open("p:spPr");
        xfrm(w, P.x, P.y, P.w, P.h);
        w.open("a:prstGeom").attr("prst", o.kind == "shape" ? prstOf(o.shape) : std::string("rect")).leaf("a:avLst").close();
        if (P.fill) paintFill(w, o.fill, *P.fill);
        else w.leaf("a:noFill");
        lineProps(P);
        w.close();
        if (P.hasText) {
            body(P.text, o.kind == "text" && o.h <= 0);
        } else {
            static const char* anchors[] = {"t", "ctr", "b"};
            static const char* aligns[] = {"l", "ctr", "r"};
            w.open("p:txBody").open("a:bodyPr").attr("anchor", anchors[std::clamp(o.anchor, 0, 2)]).close().leaf("a:lstStyle");
            w.open("a:p").leaf("a:pPr", {{"algn", aligns[std::clamp(o.align, 0, 2)]}}).close();
            w.close();
        }
        w.close();
    }
};

std::string masterXml(const Deck& d, SlideWriter& sw, const std::vector<Placed>& objects, std::vector<raster::ZipEntry>& files, int& counter) {
    xml::Writer& w = sw.w;
    w.open("p:sldMaster");
    ns(w);
    w.open("p:cSld");
    w.open("p:bg").open("p:bgRef").attr("idx", "1001").leaf("a:schemeClr", {{"val", "bg1"}}).close().close();
    w.open("p:spTree");
    groupProps(w);
    w.open("p:sp");
    w.open("p:nvSpPr").leaf("p:cNvPr", {{"id", "2"}, {"name", "Title Placeholder 1"}});
    w.open("p:cNvSpPr").leaf("a:spLocks", {{"noGrp", "1"}}).close();
    w.open("p:nvPr").leaf("p:ph", {{"type", "title"}}).close();
    w.close();
    w.open("p:spPr");
    xfrm(w, d.width * 0.05, d.height * 0.05, d.width * 0.9, d.height * 0.14);
    w.open("a:prstGeom").attr("prst", "rect").leaf("a:avLst").close();
    w.close();
    w.open("p:txBody").leaf("a:bodyPr").leaf("a:lstStyle").open("a:p").leaf("a:endParaRPr", {{"lang", "en-US"}}).close().close();
    w.close();  // sp
    sw.nextId = 3;
    for (const auto& o : objects) sw.object(o, files, counter);
    w.close();  // spTree
    w.close();  // cSld
    w.leaf("p:clrMap", {{"bg1", "lt1"}, {"tx1", "dk1"}, {"bg2", "lt2"}, {"tx2", "dk2"}, {"accent1", "accent1"}, {"accent2", "accent2"},
                        {"accent3", "accent3"}, {"accent4", "accent4"}, {"accent5", "accent5"}, {"accent6", "accent6"}, {"hlink", "hlink"},
                        {"folHlink", "folHlink"}});
    w.open("p:sldLayoutIdLst").leaf("p:sldLayoutId", {{"id", "2147483649"}, {"r:id", "rId1"}}).close();
    w.open("p:txStyles");
    for (const char* style : {"p:titleStyle", "p:bodyStyle", "p:otherStyle"}) {
        w.open(style).open("a:lvl1pPr");
        w.open("a:defRPr").attr("sz", std::strcmp(style, "p:titleStyle") == 0 ? "3400" : "2400");
        w.open("a:solidFill").leaf("a:schemeClr", {{"val", "tx1"}}).close();
        w.leaf("a:latin", {{"typeface", "+mn-lt"}});
        w.close().close().close();
    }
    w.close();
    return w.str();
}

}  // namespace

std::string writePptx(const Deck& deck) {
    const bool anyNotes = std::any_of(deck.slides.begin(), deck.slides.end(), [](const Slide& s) { return !s.notes.empty(); });
    std::vector<raster::ZipEntry> files;
    int media = 0;
    SlideWriter mw{deck, 0, {}, {}, {}, 3};
    mw.rel("slideLayout", "../slideLayouts/slideLayout1.xml");
    mw.rel("theme", "../theme/theme1.xml");
    std::vector<Placed> masterObjects;
    for (const auto& o : deck.master) masterObjects.push_back(placeObject(o, deck.theme, deck.theme.background));
    const std::string master = masterXml(deck, mw, masterObjects, files, media);

    xml::Writer ct;
    ct.open("Types").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/content-types");
    ct.leaf("Default", {{"Extension", "rels"}, {"ContentType", "application/vnd.openxmlformats-package.relationships+xml"}});
    ct.leaf("Default", {{"Extension", "xml"}, {"ContentType", "application/xml"}});
    ct.leaf("Default", {{"Extension", "png"}, {"ContentType", "image/png"}});
    ct.leaf("Default", {{"Extension", "jpeg"}, {"ContentType", "image/jpeg"}});
    auto over = [&](const std::string& part, const std::string& type) { ct.leaf("Override", {{"PartName", part}, {"ContentType", type}}); };
    over("/ppt/presentation.xml", kCt + "presentation.main+xml");
    over("/ppt/slideMasters/slideMaster1.xml", kCt + "slideMaster+xml");
    over("/ppt/slideLayouts/slideLayout1.xml", kCt + "slideLayout+xml");
    over("/ppt/theme/theme1.xml", "application/vnd.openxmlformats-officedocument.theme+xml");
    over("/ppt/presProps.xml", kCt + "presProps+xml");
    over("/ppt/viewProps.xml", kCt + "viewProps+xml");
    over("/ppt/tableStyles.xml", kCt + "tableStyles+xml");
    over("/docProps/core.xml", "application/vnd.openxmlformats-package.core-properties+xml");
    over("/docProps/app.xml", "application/vnd.openxmlformats-officedocument.extended-properties+xml");
    if (anyNotes) {
        over("/ppt/notesMasters/notesMaster1.xml", kCt + "notesMaster+xml");
        over("/ppt/theme/theme2.xml", "application/vnd.openxmlformats-officedocument.theme+xml");
    }

    // presentation
    std::vector<std::array<std::string, 3>> presRels = {
        {"rId1", kRel + "slideMaster", "slideMasters/slideMaster1.xml"}, {"rId2", kRel + "theme", "theme/theme1.xml"},
        {"rId3", kRel + "presProps", "presProps.xml"},                   {"rId4", kRel + "viewProps", "viewProps.xml"},
        {"rId5", kRel + "tableStyles", "tableStyles.xml"},
    };
    if (anyNotes) presRels.push_back({"rId6", kRel + "notesMaster", "notesMasters/notesMaster1.xml"});
    xml::Writer pw;
    pw.open("p:presentation");
    ns(pw);
    pw.attr("saveSubsetFonts", "1");
    pw.open("p:sldMasterIdLst").leaf("p:sldMasterId", {{"id", "2147483648"}, {"r:id", "rId1"}}).close();
    if (anyNotes) pw.open("p:notesMasterIdLst").leaf("p:notesMasterId", {{"r:id", "rId6"}}).close();
    if (!deck.slides.empty()) {
        pw.open("p:sldIdLst");
        for (std::size_t i = 0; i < deck.slides.size(); ++i) {
            std::string rid = "rId" + std::to_string(10 + i);
            pw.leaf("p:sldId", {{"id", std::to_string(256 + i)}, {"r:id", rid}});
            presRels.push_back({rid, kRel + "slide", "slides/slide" + std::to_string(i + 1) + ".xml"});
        }
        pw.close();
    }
    pw.leaf("p:sldSz", {{"cx", emu(deck.width)}, {"cy", emu(deck.height)}});
    pw.leaf("p:notesSz", {{"cx", "6858000"}, {"cy", "9144000"}});

    for (std::size_t i = 0; i < deck.slides.size(); ++i) {
        const Slide& s = deck.slides[i];
        const std::string n = std::to_string(i + 1);
        Plan P = plan(deck, i);
        SlideWriter sw{deck, i, {}, {}, {}, 2};
        sw.rel("slideLayout", "../slideLayouts/slideLayout1.xml");
        if (!s.notes.empty()) sw.rel("notesSlide", "../notesSlides/notesSlide" + n + ".xml");
        auto& w = sw.w;
        w.open("p:sld");
        ns(w);
        if (s.hidden) w.attr("show", "0");
        if (!P.showMaster && !deck.master.empty()) w.attr("showMasterSp", "0");
        std::string layout = s.layout;
        w.open("p:cSld").attr("name", layout);
        background(w, P.background);
        w.open("p:spTree");
        groupProps(w);
        for (const auto& r : P.rects) sw.rect(r);
        if (P.hasImage) sw.picture(P, sw.media(s.image, s.pixels->pixels, files, media));
        for (const auto& f : P.frames) sw.frame(f);
        for (const auto& o : P.front) sw.object(o, files, media);
        w.close();  // spTree
        w.close();  // cSld
        w.open("p:clrMapOvr").leaf("a:masterClrMapping").close();
        if (!s.transition.empty() || s.advanceAfter > 0) {
            w.open("p:transition").attr("spd", transitionSpeed(s.transitionSeconds));
            if (s.advanceAfter > 0) w.attr("advTm", std::to_string(std::lround(s.advanceAfter * 1000)));
            if (s.transition == "fade" || s.transition == "zoom") {
                w.leaf("p:" + s.transition);
            } else if (!s.transition.empty()) {
                const std::string dir = s.direction == "right" ? "r" : s.direction == "up" ? "u" : s.direction == "down" ? "d" : "l";
                w.leaf("p:" + s.transition, {{"dir", dir}});
            }
            w.close();
        }
        files.push_back({"ppt/slides/slide" + n + ".xml", w.str()});
        files.push_back({"ppt/slides/_rels/slide" + n + ".xml.rels", relsXml(sw.rels)});
        over("/ppt/slides/slide" + n + ".xml", kCt + "slide+xml");
        if (!s.notes.empty()) {
            files.push_back({"ppt/notesSlides/notesSlide" + n + ".xml", notesXml(s.notes)});
            files.push_back({"ppt/notesSlides/_rels/notesSlide" + n + ".xml.rels",
                             relsXml({{{"rId1", kRel + "notesMaster", "../notesMasters/notesMaster1.xml"}, {"rId2", kRel + "slide", "../slides/slide" + n + ".xml"}}})});
            over("/ppt/notesSlides/notesSlide" + n + ".xml", kCt + "notesSlide+xml");
        }
    }

    xml::Writer root;
    root.open("Relationships").attr("xmlns", "http://schemas.openxmlformats.org/package/2006/relationships");
    root.leaf("Relationship", {{"Id", "rId1"}, {"Type", kRel + "officeDocument"}, {"Target", "ppt/presentation.xml"}});
    root.leaf("Relationship", {{"Id", "rId2"}, {"Type", "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties"}, {"Target", "docProps/core.xml"}});
    root.leaf("Relationship", {{"Id", "rId3"}, {"Type", kRel + "extended-properties"}, {"Target", "docProps/app.xml"}});

    xml::Writer core;
    core.open("cp:coreProperties")
        .attr("xmlns:cp", "http://schemas.openxmlformats.org/package/2006/metadata/core-properties")
        .attr("xmlns:dc", "http://purl.org/dc/elements/1.1/")
        .attr("xmlns:dcterms", "http://purl.org/dc/terms/")
        .attr("xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance");
    if (!deck.title.empty()) core.element("dc:title", deck.title);
    if (!deck.author.empty()) core.element("dc:creator", deck.author);

    xml::Writer app;
    app.open("Properties").attr("xmlns", "http://schemas.openxmlformats.org/officeDocument/2006/extended-properties");
    app.element("Application", "wetwaresuite");
    app.element("Slides", std::to_string(deck.slides.size()));

    xml::Writer props;
    props.open("p:presentationPr");
    ns(props);
    xml::Writer view;
    view.open("p:viewPr");
    ns(view);
    view.open("p:normalViewPr").leaf("p:restoredLeft", {{"sz", "15620"}}).leaf("p:restoredTop", {{"sz", "94660"}}).close();
    view.leaf("p:gridSpacing", {{"cx", "76200"}, {"cy", "76200"}});
    xml::Writer tables;
    tables.open("a:tblStyleLst").attr("xmlns:a", kA).attr("def", "{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}");

    std::vector<raster::ZipEntry> entries = {
        {"[Content_Types].xml", ct.str()},
        {"_rels/.rels", root.str()},
        {"docProps/core.xml", core.str()},
        {"docProps/app.xml", app.str()},
        {"ppt/presentation.xml", pw.str()},
        {"ppt/_rels/presentation.xml.rels", relsXml(presRels)},
        {"ppt/presProps.xml", props.str()},
        {"ppt/viewProps.xml", view.str()},
        {"ppt/tableStyles.xml", tables.str()},
        {"ppt/theme/theme1.xml", themeXml(deck.theme, "wetwaresuite " + deck.theme.name)},
        {"ppt/slideMasters/slideMaster1.xml", master},
        {"ppt/slideMasters/_rels/slideMaster1.xml.rels", relsXml(mw.rels)},
        {"ppt/slideLayouts/slideLayout1.xml", layoutXml()},
        {"ppt/slideLayouts/_rels/slideLayout1.xml.rels", relsXml({{{"rId1", kRel + "slideMaster", "../slideMasters/slideMaster1.xml"}}})},
    };
    if (anyNotes) {
        Theme plain;
        entries.push_back({"ppt/theme/theme2.xml", themeXml(plain, "wetwaresuite notes")});
        entries.push_back({"ppt/notesMasters/notesMaster1.xml", notesMasterXml(deck)});
        entries.push_back({"ppt/notesMasters/_rels/notesMaster1.xml.rels", relsXml({{{"rId1", kRel + "theme", "../theme/theme2.xml"}}})});
    }
    for (auto& f : files) entries.push_back(std::move(f));
    return raster::writeZip(entries);
}

// ---- reading PowerPoint --------------------------------------------------------------

namespace {

// "ppt/slides/" + "../media/a.png" -> "ppt/media/a.png"
std::string resolvePart(const std::string& dir, const std::string& target) {
    if (!target.empty() && target[0] == '/') return target.substr(1);
    std::vector<std::string> parts;
    auto push = [&](const std::string& path) {
        std::string cur;
        for (std::size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == '/') {
                if (cur == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else if (!cur.empty() && cur != ".") {
                    parts.push_back(cur);
                }
                cur.clear();
            } else {
                cur += path[i];
            }
        }
    };
    push(dir);
    push(target);
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::string dirOf(const std::string& part) {
    auto p = part.rfind('/');
    return p == std::string::npos ? std::string() : part.substr(0, p + 1);
}

std::string relsPath(const std::string& part) {
    return dirOf(part) + "_rels/" + part.substr(dirOf(part).size()) + ".rels";
}

struct PptxReader {
    std::map<std::string, std::string> files;

    std::string get(const std::string& name) const {
        auto it = files.find(name);
        return it == files.end() ? std::string() : it->second;
    }

    // id -> {type suffix, resolved target}
    std::map<std::string, std::pair<std::string, std::string>> rels(const std::string& part) const {
        std::map<std::string, std::pair<std::string, std::string>> out;
        std::string text = get(relsPath(part));
        if (text.empty()) return out;
        xml::Node root = xml::parse(text);
        for (const auto* r : root.all("Relationship")) {
            std::string type = r->attr("Type");
            auto slash = type.rfind('/');
            if (slash != std::string::npos) type = type.substr(slash + 1);
            std::string target = r->attr("Target");
            if (r->attr("TargetMode") != "External") target = resolvePart(dirOf(part), target);
            out[r->attr("Id")] = {type, target};
        }
        return out;
    }

    static std::vector<ListItem> paragraphs(const xml::Node& txBody, const std::map<std::string, std::pair<std::string, std::string>>& rel,
                                            bool* numbered = nullptr) {
        std::vector<ListItem> items;
        int top = 0, topNumbered = 0;
        for (const auto* p : txBody.all("p")) {
            ListItem it;
            bool autoNum = false;
            if (const auto* ppr = p->child("pPr")) {
                it.level = std::clamp(std::atoi(ppr->attr("lvl").c_str()), 0, 8);
                autoNum = ppr->child("buAutoNum") != nullptr;
            }
            for (const auto& c : p->children) {
                if (c.is("r") || c.is("fld")) {
                    Run r;
                    if (const auto* t = c.child("t")) r.text = t->allText();
                    if (const auto* rpr = c.child("rPr")) {
                        auto on = [&](const char* k) {
                            std::string v = rpr->attr(k);
                            return !v.empty() && v != "0" && v != "false" && v != "none";
                        };
                        r.bold = on("b");
                        r.italic = on("i");
                        r.underline = on("u");
                        if (const auto* h = rpr->child("hlinkClick")) {
                            auto f = rel.find(h->attr("r:id"));
                            if (f != rel.end()) r.link = f->second.second;
                        }
                        if (const auto* l = rpr->child("latin")) {
                            std::string face = lower(l->attr("typeface"));
                            r.code = face.find("courier") != std::string::npos || face.find("mono") != std::string::npos ||
                                     face.find("consolas") != std::string::npos;
                        }
                    }
                    if (!r.link.empty()) r.underline = false;
                    it.runs.push_back(r);
                } else if (c.is("br")) {
                    Run r;
                    r.text = "\n";
                    it.runs.push_back(r);
                }
            }
            mergeRuns(it.runs);
            if (trimCopy(plainText(it.runs)).empty()) continue;
            if (it.level == 0) {
                ++top;
                topNumbered += autoNum ? 1 : 0;
            }
            items.push_back(std::move(it));
        }
        if (numbered) *numbered = top > 0 && topNumbered == top;
        return items;
    }

    static std::string joined(const std::vector<ListItem>& items, const char* sep) {
        std::string out;
        for (const auto& it : items) out += (out.empty() ? "" : sep) + inlineMarkdown(it.runs);
        return out;
    }
};

using Rels = std::map<std::string, std::pair<std::string, std::string>>;

// Child coordinates (EMU) of a group -> slide coordinates (EMU).
struct Xf {
    double ax = 1, ay = 1, bx = 0, by = 0;
};

double attrNum(const xml::Node* n, const char* key) { return n ? std::atof(n->attr(key).c_str()) : 0.0; }

Xf groupXf(const xml::Node& g, const Xf& T) {
    const auto* gp = g.child("grpSpPr");
    const auto* xf = gp ? gp->child("xfrm") : nullptr;
    if (!xf) return T;
    const double ox = attrNum(xf->child("off"), "x"), oy = attrNum(xf->child("off"), "y");
    const double cx = attrNum(xf->child("ext"), "cx"), cy = attrNum(xf->child("ext"), "cy");
    const double chx = attrNum(xf->child("chOff"), "x"), chy = attrNum(xf->child("chOff"), "y");
    const double chcx = attrNum(xf->child("chExt"), "cx"), chcy = attrNum(xf->child("chExt"), "cy");
    const double kx = chcx > 0 ? cx / chcx : 1, ky = chcy > 0 ? cy / chcy : 1;
    Xf out;
    out.ax = T.ax * kx;
    out.ay = T.ay * ky;
    out.bx = T.ax * (ox - chx * kx) + T.bx;
    out.by = T.ay * (oy - chy * ky) + T.by;
    return out;
}

// Shapes in drawing order with their group transforms (groups flattened, mc:Fallback taken).
void collectShapes(const xml::Node& tree, const Xf& T, std::vector<std::pair<const xml::Node*, Xf>>& out) {
    for (const auto& c : tree.children) {
        if (c.is("grpSp")) collectShapes(c, groupXf(c, T), out);
        else if (c.is("sp") || c.is("pic") || c.is("cxnSp")) out.push_back({&c, T});
        else if (c.is("AlternateContent")) {
            if (const auto* fb = c.child("Fallback")) collectShapes(*fb, T, out);
        }
    }
}

// The colour element directly under `holder` (a solidFill, fillRef, ...) as a paint spec.
std::optional<std::string> colourSpec(const xml::Node* holder) {
    if (!holder) return std::nullopt;
    if (const auto* c = holder->child("srgbClr")) return "#" + lower(c->attr("val"));
    if (const auto* c = holder->child("sysClr")) return "#" + lower(c->attr("lastClr"));
    if (const auto* c = holder->child("schemeClr")) {
        const std::string v = c->attr("val");
        if (v == "tx1" || v == "dk1" || v == "tx2" || v == "dk2") return std::string("text");
        if (v == "bg1" || v == "lt1" || v == "bg2" || v == "lt2") return std::string("background");
        return std::string("accent");
    }
    if (const auto* c = holder->child("prstClr")) {
        std::string out;
        if (parsePaint(c->attr("val"), out)) return out;
    }
    return std::nullopt;
}

// The fill under spPr, ln or rPr: "" for noFill, nullopt when nothing is said.
std::optional<std::string> paintIn(const xml::Node* parent) {
    if (!parent) return std::nullopt;
    if (parent->child("noFill")) return std::string();
    if (const auto* sf = parent->child("solidFill")) return colourSpec(sf);
    if (const auto* g = parent->child("gradFill")) {
        if (const auto* gs = g->find("gs")) return colourSpec(gs);
    }
    return std::nullopt;
}

bool allDigits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }); }

// "Body 3" for prefix "Body": the name our writer gives a layout piece.
bool ourPiece(const std::string& name, const std::string& prefix) {
    return name.size() > prefix.size() + 1 && startsWith(name, prefix + " ") && allDigits(name.substr(prefix.size() + 1));
}

const xml::Node* nvProps(const xml::Node& sh) {
    for (const char* n : {"nvSpPr", "nvPicPr", "nvCxnSpPr"})
        if (const auto* c = sh.child(n)) return c;
    return nullptr;
}

bool readObject(const PptxReader& R, const xml::Node& sh, const Xf& T, const Rels& rel, SlideObject& o) {
    const xml::Node* spPr = sh.child("spPr");
    const xml::Node* xf = spPr ? spPr->child("xfrm") : nullptr;
    if (!xf || !xf->child("off") || !xf->child("ext")) return false;
    o.x = (attrNum(xf->child("off"), "x") * T.ax + T.bx) / 12700.0;
    o.y = (attrNum(xf->child("off"), "y") * T.ay + T.by) / 12700.0;
    o.w = attrNum(xf->child("ext"), "cx") * T.ax / 12700.0;
    o.h = attrNum(xf->child("ext"), "cy") * T.ay / 12700.0;
    if (const auto* nv = nvProps(sh)) {
        if (const auto* c = nv->child("cNvPr")) o.name = c->attr("name");
    }
    for (const char* ours : {"Text Box", "Shape", "Image", "Line"})
        if (ourPiece(o.name, ours)) o.name.clear();
    const xml::Node* style = sh.child("style");
    auto styled = [&](const char* ref) {
        const auto* r = style ? style->child(ref) : nullptr;
        return r && r->attr("idx") != "0" ? colourSpec(r) : std::nullopt;
    };
    const xml::Node* ln = spPr->child("ln");
    if (ln && ln->hasAttr("w")) o.lineWidth = std::max(0.25, std::round(attrNum(ln, "w") / 127.0) / 100.0);
    const std::string prst = spPr->child("prstGeom") ? spPr->child("prstGeom")->attr("prst") : std::string();
    if (sh.is("pic")) {
        const auto* blip = sh.find("blip");
        if (!blip) return false;
        auto r = rel.find(blip->attr("r:embed"));
        if (r == rel.end()) return false;
        try {
            setObjectImage(o, R.get(r->second.second));
        } catch (const std::exception&) {
            return false;
        }
        o.kind = "image";
        o.line = paintIn(ln).value_or("");
        return true;
    }
    if (sh.is("cxnSp") || prst == "line" || startsWith(prst, "straightConnector") || startsWith(prst, "bentConnector") ||
        startsWith(prst, "curvedConnector")) {
        o.kind = "line";
        if (xf->attr("flipH") == "1") o.x += o.w, o.w = -o.w;
        if (xf->attr("flipV") == "1") o.y += o.h, o.h = -o.h;
        auto lp = paintIn(ln);
        o.line = lp ? *lp : styled("lnRef").value_or("text");
        if (ln) {
            const auto* te = ln->child("tailEnd");
            o.arrow = te && !te->attr("type").empty() && te->attr("type") != "none";
        }
        return true;
    }
    const auto* nv = sh.child("nvSpPr");
    const bool txBox = nv && nv->child("cNvSpPr") && nv->child("cNvSpPr")->attr("txBox") == "1";
    o.kind = txBox ? "text" : "shape";
    o.shape = shapeOfPrst(prst);
    auto fp = paintIn(spPr);
    o.fill = fp ? *fp : txBox ? std::string() : styled("fillRef").value_or("");
    auto lp = paintIn(ln);
    o.line = lp ? *lp : txBox ? std::string() : styled("lnRef").value_or("");
    o.anchor = 0;
    if (const auto* tx = sh.child("txBody")) {
        if (const auto* bp = tx->child("bodyPr")) {
            const std::string a = bp->attr("anchor");
            o.anchor = a == "ctr" ? 1 : a == "b" ? 2 : 0;
            if (txBox && bp->child("spAutoFit")) o.h = 0;
        }
        bool numbered = false, bullets = false, first = true;
        for (const auto* p : tx->all("p")) {
            const auto* ppr = p->child("pPr");
            if (ppr && (ppr->child("buChar") || ppr->child("buAutoNum"))) bullets = true;
            if (first && ppr) {
                const std::string al = ppr->attr("algn");
                o.align = al == "ctr" ? 1 : al == "r" ? 2 : 0;
            }
            first = false;
        }
        if (const auto* rpr = tx->find("rPr")) {
            if (rpr->hasAttr("sz")) o.size = attrNum(rpr, "sz") / 100.0;
            if (auto c = paintIn(rpr); c && !c->empty()) o.color = *c;
        }
        auto items = PptxReader::paragraphs(*tx, rel, &numbered);
        bool allBold = !items.empty(), allItalic = !items.empty();
        for (const auto& it : items)
            for (const auto& r : it.runs) {
                if (trimCopy(r.text).empty()) continue;
                allBold = allBold && r.bold;
                allItalic = allItalic && r.italic;
            }
        for (auto& it : items) {
            for (auto& r : it.runs) {
                if (allBold) r.bold = false;
                if (allItalic) r.italic = false;
            }
            mergeRuns(it.runs);
        }
        o.bold = allBold;
        o.italic = allItalic;
        o.bullets = bullets;
        if (bullets) {
            o.text = bulletText(items, numbered);
            while (!o.text.empty() && o.text.back() == '\n') o.text.pop_back();
        } else {
            o.text = PptxReader::joined(items, "\n");
        }
    }
    return true;
}

void readTransition(const xml::Node& sld, Slide& s) {
    std::vector<const xml::Node*> found;
    sld.descendants("transition", found);
    for (const auto* tr : found) {
        const std::string spd = tr->attr("spd");
        if (!spd.empty()) s.transitionSeconds = spd == "fast" ? 0.5 : spd == "slow" ? 1.0 : 0.75;
        if (tr->hasAttr("dur")) s.transitionSeconds = attrNum(tr, "dur") / 1000.0;
        if (tr->hasAttr("advTm")) s.advanceAfter = attrNum(tr, "advTm") / 1000.0;
        for (const auto& c : tr->children) {
            if (c.isText() || !s.transition.empty()) continue;
            const std::string k(c.local());
            std::string t = k == "fade" || k == "dissolve" ? "fade"
                            : k == "push"                  ? "push"
                            : k == "wipe" || k == "split" || k == "strips" || k == "blinds" ? "wipe"
                            : k == "cover" || k == "pull"  ? "cover"
                            : k == "zoom" || k == "newsflash" || k == "circle" ? "zoom"
                                                           : "";
            if (t.empty()) continue;
            s.transition = t;
            const std::string dir = c.attr("dir");
            s.direction = dir == "r" ? "right" : dir == "u" ? "up" : dir == "d" ? "down" : "left";
        }
    }
}

}  // namespace

Deck readPptx(const std::string& bytes) {
    PptxReader R;
    for (auto& e : raster::readZip(bytes)) R.files[e.name] = std::move(e.data);
    std::string presPart = "ppt/presentation.xml";
    {
        // the root relationships name the main part
        std::string text = R.get("_rels/.rels");
        if (!text.empty()) {
            xml::Node root = xml::parse(text);
            for (const auto* r : root.all("Relationship")) {
                if (endsWith(r->attr("Type"), "/officeDocument")) presPart = resolvePart("", r->attr("Target"));
            }
        }
    }
    std::string presText = R.get(presPart);
    if (presText.empty()) throw std::runtime_error("not a PowerPoint file (no presentation part)");
    xml::Node pres = xml::parse(presText);
    Deck deck;
    if (const auto* sz = pres.child("sldSz")) {
        double cx = std::atof(sz->attr("cx").c_str()), cy = std::atof(sz->attr("cy").c_str());
        if (cx > 0 && cy > 0) {
            deck.width = cx / 12700.0;
            deck.height = cy / 12700.0;
        }
    }
    auto presRels = R.rels(presPart);
    // theme accent
    for (const auto& [id, rel] : presRels) {
        if (rel.first != "theme") continue;
        std::string text = R.get(rel.second);
        if (text.empty()) break;
        xml::Node th = xml::parse(text);
        auto colour = [&](const char* name) -> std::optional<Color> {
            const auto* n = th.find(name);
            if (!n) return std::nullopt;
            if (const auto* c = n->child("srgbClr")) return Color::parse("#" + c->attr("val"));
            if (const auto* c = n->child("sysClr")) return Color::parse("#" + c->attr("lastClr"));
            return std::nullopt;
        };
        if (auto a = colour("accent1")) {
            deck.theme.accent = *a;
            deck.theme.name = "#" + lower(hex6(*a));
            for (const auto& p : presets()) {
                if (p.accent == *a) {
                    std::string err;
                    parseTheme(p.name, deck.theme, err);
                }
            }
        }
        if (auto c = colour("lt1")) deck.theme.background = *c;
        if (auto c = colour("dk1")) deck.theme.text = *c;
        if (const auto* fs = th.find("minorFont")) {
            if (const auto* l = fs->child("latin")) {
                std::string face = lower(l->attr("typeface"));
                if (face.find("times") != std::string::npos || face.find("serif") != std::string::npos || face.find("georgia") != std::string::npos)
                    deck.theme.font = pdf::Family::Serif;
                else if (face.find("courier") != std::string::npos || face.find("mono") != std::string::npos)
                    deck.theme.font = pdf::Family::Mono;
            }
        }
        break;
    }
    // core properties
    {
        std::string core = R.get("docProps/core.xml");
        if (!core.empty()) {
            xml::Node c = xml::parse(core);
            if (const auto* t = c.find("title")) deck.title = t->allText();
            if (const auto* a = c.find("creator")) deck.author = a->allText();
        }
    }
    const bool ours = R.get("docProps/app.xml").find("<Application>wetwaresuite</Application>") != std::string::npos;
    // the master's own shapes (logos, bands) become master objects
    for (const auto& [id, rel] : presRels) {
        if (rel.first != "slideMaster") continue;
        std::string text = R.get(rel.second);
        if (text.empty()) break;
        xml::Node m = xml::parse(text);
        const auto* tree = m.find("spTree");
        if (!tree) break;
        auto mrel = R.rels(rel.second);
        std::vector<std::pair<const xml::Node*, Xf>> shapes;
        collectShapes(*tree, Xf{}, shapes);
        for (const auto& [sh, T] : shapes) {
            const auto* nv = nvProps(*sh);
            if (nv && nv->child("nvPr") && nv->child("nvPr")->child("ph")) continue;
            SlideObject o;
            if (readObject(R, *sh, T, mrel, o)) deck.master.push_back(std::move(o));
        }
        break;
    }
    const auto* list = pres.child("sldIdLst");
    if (!list) return deck;
    bool sawNumber = false, anyContent = false, masterOnTitle = false;
    for (const auto* sid : list->all("sldId")) {
        auto it = presRels.find(sid->attr("r:id"));
        if (it == presRels.end()) continue;
        const std::string part = it->second.second;
        std::string text = R.get(part);
        if (text.empty()) continue;
        xml::Node sld = xml::parse(text);
        auto rel = R.rels(part);
        Slide s;
        s.layout.clear();
        std::vector<std::vector<ListItem>> bodies;
        const xml::Node* cSld = sld.child("cSld");
        std::string declared = cSld ? cSld->attr("name") : std::string();
        std::vector<std::pair<const xml::Node*, Xf>> shapes;
        if (cSld) {
            if (const auto* tree = cSld->child("spTree")) collectShapes(*tree, Xf{}, shapes);
        }
        bool hasSubtitlePh = false;
        for (const auto& [sh, T] : shapes) {
            std::string phType, name;
            bool isPh = false;
            if (const auto* nv = nvProps(*sh)) {
                if (const auto* c = nv->child("cNvPr")) name = c->attr("name");
                if (const auto* p = nv->child("nvPr")) {
                    if (const auto* ph = p->child("ph")) {
                        isPh = true;
                        phType = ph->attr("type");
                    }
                }
            }
            if (sh->is("pic") && (isPh || (ours && ourPiece(name, "Picture"))) && s.image.empty()) {
                const auto* blip = sh->find("blip");
                auto r = blip ? rel.find(blip->attr("r:embed")) : rel.end();
                std::string img = r == rel.end() ? std::string() : R.get(r->second.second);
                if (img.empty()) continue;
                try {
                    setSlideImage(s, img);
                } catch (const std::exception&) {
                    s.image.clear();
                    s.pixels.reset();
                }
                continue;
            }
            const xml::Node* tx = sh->child("txBody");
            if (phType == "sldNum" || ourPiece(name, "Slide Number")) {
                sawNumber = true;
                continue;
            }
            if (phType == "dt" || ourPiece(name, "Accent")) continue;
            if (phType == "ftr" || ourPiece(name, "Footer")) {
                if (tx && deck.footer.empty()) deck.footer = PptxReader::joined(PptxReader::paragraphs(*tx, rel), " ");
                continue;
            }
            const bool layoutText = sh->is("sp") && (isPh || ourPiece(name, "Title") || ourPiece(name, "Subtitle") || ourPiece(name, "Body") || ourPiece(name, "Right"));
            if (!layoutText) {
                SlideObject o;
                if (readObject(R, *sh, T, rel, o)) s.objects.push_back(std::move(o));
                continue;
            }
            if (!tx) continue;
            bool numberedItems = false;
            auto items = PptxReader::paragraphs(*tx, rel, &numberedItems);
            if (items.empty()) continue;
            if (phType == "title" || phType == "ctrTitle") {
                // titles are bold by style; keep only emphasis that differs
                bool allBold = true;
                for (const auto& it : items)
                    for (const auto& r : it.runs) allBold = allBold && (r.bold || trimCopy(r.text).empty());
                if (allBold)
                    for (auto& it : items)
                        for (auto& r : it.runs) r.bold = false;
                for (auto& it : items) mergeRuns(it.runs);
                if (s.title.empty()) s.title = PptxReader::joined(items, " ");
                if (phType == "ctrTitle" && s.layout.empty()) s.layout = "title";
            } else if (phType == "subTitle" || ourPiece(name, "Subtitle")) {
                s.subtitle = PptxReader::joined(items, "\n");
                if (phType == "subTitle") hasSubtitlePh = true;
            } else if (ourPiece(name, "Right")) {
                s.right = items;
            } else {
                if (bodies.empty()) s.numbered = numberedItems;
                bodies.push_back(items);
            }
        }
        if (!bodies.empty()) s.bullets = bodies[0];
        if (bodies.size() >= 2 && s.right.empty()) {
            s.right = bodies[1];
            for (std::size_t b = 2; b < bodies.size(); ++b) s.right.insert(s.right.end(), bodies[b].begin(), bodies[b].end());
        }
        if (std::find(slideLayouts().begin(), slideLayouts().end(), declared) != slideLayouts().end()) {
            s.layout = declared;
        } else if (s.layout.empty()) {
            const bool img = !s.image.empty();
            if (img && !s.bullets.empty()) s.layout = "image_right";
            else if (img) s.layout = "image";
            else if (!s.right.empty()) s.layout = "two_column";
            else if (!s.bullets.empty()) s.layout = "content";
            else if (hasSubtitlePh || !s.subtitle.empty()) s.layout = "title";
            else if (!s.title.empty()) s.layout = "title_only";
            else s.layout = "blank";
        }
        const bool titleLike = s.layout == "title" || s.layout == "section";
        if (!titleLike && s.layout != "blank") anyContent = true;
        if (titleLike && sld.attr("showMasterSp") != "0") masterOnTitle = true;
        const std::string show = sld.attr("show");
        s.hidden = show == "0" || show == "false";
        readTransition(sld, s);
        // speaker notes
        for (const auto& [id, r] : rel) {
            if (r.first != "notesSlide") continue;
            std::string nt = R.get(r.second);
            if (nt.empty()) break;
            xml::Node notes = xml::parse(nt);
            std::vector<const xml::Node*> sps;
            notes.descendants("sp", sps);
            for (const auto* sp : sps) {
                const auto* ph = sp->find("ph");
                if (!ph || ph->attr("type") != "body") continue;
                if (const auto* tx = sp->child("txBody")) {
                    std::string out;
                    for (const auto* p : tx->all("p")) {
                        std::string line;
                        for (const auto& c : p->children) {
                            if (c.is("r") || c.is("fld")) line += c.child("t") ? c.child("t")->allText() : "";
                            else if (c.is("br")) line += "\n";
                        }
                        out += (out.empty() ? "" : "\n") + line;
                    }
                    s.notes = trimCopy(out);
                }
            }
            break;
        }
        deck.slides.push_back(std::move(s));
    }
    if (anyContent && !sawNumber) deck.slideNumbers = false;
    deck.masterOnTitle = masterOnTitle && !deck.master.empty();
    return deck;
}

}  // namespace wetwaresuite::office
