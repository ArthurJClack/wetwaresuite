// Slide decks: layout presets placed once into frames, which are then drawn
// as PDF/PNG pages or written as PowerPoint shapes, so both outputs agree.
#include "achclwss/office/deck.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/ora.hpp"
#include "text.hpp"
#include "textlayout.hpp"
#include "xml.hpp"

namespace achclwss::office {

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
};

struct Plan {
    Color background;
    std::vector<pdf::RectOp> rects;
    std::vector<Frame> frames;
    bool hasImage = false;
    double ix = 0, iy = 0, iw = 0, ih = 0;
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

Plan plan(const Deck& d, std::size_t index) {
    const Slide& s = d.slides.at(index);
    const Theme& t = d.theme;
    const double sx = d.width / 960.0, sy = d.height / 540.0, fs = std::min(sx, sy);
    Plan P;
    P.background = t.background;
    const Color muted = mix(t.text, t.background, 0.4);
    auto frame = [&](const std::string& name, double x, double y, double w, double h, double size) {
        Frame f;
        f.name = name;
        f.x = x * sx;
        f.y = y * sy;
        f.w = w * sx;
        f.h = h * sy;
        f.size = size * fs;
        f.color = t.text;
        f.marker = t.accent;
        f.link = luminance(t.background) < 0.4 ? mix(t.accent, Color{255, 255, 255, 255}, 0.3) : Color{0x05, 0x63, 0xC1, 255};
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

    std::string layout = s.layout;
    if (std::find(slideLayouts().begin(), slideLayouts().end(), layout) == slideLayouts().end()) layout = "content";
    const bool image = s.pixels && !s.pixels->pixels.empty();
    if (layout == "content" && image) layout = s.bullets.empty() ? "image" : "image_right";

    if (layout == "title" || layout == "section") {
        P.background = t.accent;
        const Color on = luminance(t.accent) > 0.62 ? Color{0x22, 0x22, 0x22, 255} : Color{255, 255, 255, 255};
        const Color soft = mix(on, t.accent, 0.18);
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
        Frame n = frame("Slide Number", 812, 506, 100, 20, 12);
        n.color = muted;
        n.align = 2;
        n.items = textItems(std::to_string(index + 1));
        P.frames.push_back(n);
    }
    for (auto& f : P.frames) fit(f, t.font);
    return P;
}

}  // namespace

pdf::Page layoutSlide(const Deck& deck, std::size_t index) {
    Plan P = plan(deck, index);
    pdf::Page page;
    page.width = deck.width;
    page.height = deck.height;
    page.rect(0, 0, deck.width, deck.height, P.background);
    for (const auto& r : P.rects) page.ops.emplace_back(r);
    if (P.hasImage) page.image(P.ix, P.iy, P.iw, P.ih, deck.slides[index].pixels);
    for (const auto& f : P.frames) flow(f, deck.theme.font, f.scale, &page);
    return page;
}

std::vector<std::string> slideOverflow(const Deck& deck, std::size_t index) {
    std::vector<std::string> out;
    for (const auto& f : plan(deck, index).frames) {
        if (f.overflow) out.push_back(f.name == "Body" ? "bullets" : f.name == "Right" ? "right" : lower(f.name));
    }
    return out;
}

std::vector<pdf::Page> layoutDeck(const Deck& deck) {
    std::vector<pdf::Page> pages;
    for (std::size_t i = 0; i < deck.slides.size(); ++i) pages.push_back(layoutSlide(deck, i));
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

std::string masterXml(const Deck& d) {
    xml::Writer w;
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
            solid(w, r.link.empty() ? f.color : f.link);
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
        w.open("p:txBody");
        static const char* anchors[] = {"t", "ctr", "b"};
        w.open("a:bodyPr").attr("wrap", "square").attr("lIns", "0").attr("tIns", "0").attr("rIns", "0").attr("bIns", "0");
        w.attr("anchor", anchors[std::clamp(f.anchor, 0, 2)]).leaf("a:noAutofit").close();
        w.leaf("a:lstStyle");
        static const char* aligns[] = {"l", "ctr", "r"};
        const double step = f.size * f.scale * 1.1;
        for (std::size_t i = 0; i < f.items.size(); ++i) {
            const auto& it = f.items[i];
            const int level = std::clamp(it.level, 0, 4);
            const double size = f.size * f.scale * (f.bullets ? levelScale(level) : 1.0);
            w.open("a:p").open("a:pPr");
            if (f.bullets) w.attr("marL", emu(step * (level + 1))).attr("indent", emu(-step * (f.numbered && level == 0 ? 1.0 : 0.8))).attr("lvl", std::to_string(level));
            else w.attr("marL", "0").attr("indent", "0");
            w.attr("algn", aligns[std::clamp(f.align, 0, 2)]);
            w.open("a:lnSpc").leaf("a:spcPct", {{"val", "100000"}}).close();
            const double gap = i == 0 ? 0 : (f.bullets ? 0.45 * f.scale * size : 0.15 * size);
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

    void picture(const Plan& P, const std::string& target) {
        const int id = nextId++;
        std::string rid = rel("image", target);
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
};

bool isJpegBytes(const std::string& b) { return b.size() > 2 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8; }

}  // namespace

std::string writePptx(const Deck& deck) {
    const bool anyNotes = std::any_of(deck.slides.begin(), deck.slides.end(), [](const Slide& s) { return !s.notes.empty(); });
    std::vector<raster::ZipEntry> files;

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

    int media = 0;
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
        std::string layout = s.layout;
        w.open("p:cSld").attr("name", layout);
        background(w, P.background);
        w.open("p:spTree");
        groupProps(w);
        for (const auto& r : P.rects) sw.rect(r);
        if (P.hasImage) {
            const bool jpeg = isJpegBytes(s.image);
            std::string name = "image" + std::to_string(++media) + (jpeg ? ".jpeg" : ".png");
            std::string bytes = s.image;
            if (!jpeg && !(bytes.size() > 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0)) bytes = raster::encodePng(s.pixels->pixels);
            files.push_back({"ppt/media/" + name, bytes});
            sw.picture(P, "../media/" + name);
        }
        for (const auto& f : P.frames) sw.frame(f);
        w.close();  // spTree
        w.close();  // cSld
        w.open("p:clrMapOvr").leaf("a:masterClrMapping").close();
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
    app.element("Application", "ACHCLWSS");
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
        {"ppt/theme/theme1.xml", themeXml(deck.theme, "ACHCLWSS " + deck.theme.name)},
        {"ppt/slideMasters/slideMaster1.xml", masterXml(deck)},
        {"ppt/slideMasters/_rels/slideMaster1.xml.rels",
         relsXml({{{"rId1", kRel + "slideLayout", "../slideLayouts/slideLayout1.xml"}, {"rId2", kRel + "theme", "../theme/theme1.xml"}}})},
        {"ppt/slideLayouts/slideLayout1.xml", layoutXml()},
        {"ppt/slideLayouts/_rels/slideLayout1.xml.rels", relsXml({{{"rId1", kRel + "slideMaster", "../slideMasters/slideMaster1.xml"}}})},
    };
    if (anyNotes) {
        Theme plain;
        entries.push_back({"ppt/theme/theme2.xml", themeXml(plain, "ACHCLWSS notes")});
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
        if (const auto* fs = th.find("minorFont")) {
            if (const auto* l = fs->child("latin")) {
                std::string face = lower(l->attr("typeface"));
                if (face.find("times") != std::string::npos || face.find("serif") != std::string::npos || face.find("georgia") != std::string::npos)
                    deck.theme.font = pdf::Family::Serif;
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
    const auto* list = pres.child("sldIdLst");
    if (!list) return deck;
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
        std::vector<const xml::Node*> shapes;
        if (cSld) {
            if (const auto* tree = cSld->child("spTree")) {
                // shapes in document order, including those inside groups
                std::vector<const xml::Node*> ordered;
                std::function<void(const xml::Node*)> walk = [&](const xml::Node* n) {
                    for (const auto& c : n->children) {
                        if (c.is("grpSp")) walk(&c);
                        else if (c.is("sp") || c.is("pic")) ordered.push_back(&c);
                    }
                };
                walk(tree);
                shapes = std::move(ordered);
            }
        }
        bool hasSubtitlePh = false;
        for (const auto* sh : shapes) {
            if (sh->is("pic")) {
                if (!s.image.empty()) continue;
                const auto* blip = sh->find("blip");
                if (!blip) continue;
                auto r = rel.find(blip->attr("r:embed"));
                if (r == rel.end()) continue;
                std::string img = R.get(r->second.second);
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
            std::string phType, name;
            bool isPh = false;
            if (const auto* nv = sh->child("nvSpPr")) {
                if (const auto* c = nv->child("cNvPr")) name = c->attr("name");
                if (const auto* p = nv->child("nvPr")) {
                    if (const auto* ph = p->child("ph")) {
                        isPh = true;
                        phType = ph->attr("type");
                    }
                }
            }
            if (!tx) continue;
            if (phType == "sldNum" || phType == "dt" || phType == "ftr" || startsWith(name, "Slide Number")) continue;
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
            } else if (phType == "subTitle" || startsWith(name, "Subtitle")) {
                s.subtitle = PptxReader::joined(items, "\n");
                if (phType == "subTitle") hasSubtitlePh = true;
            } else if (startsWith(name, "Right")) {
                s.right = items;
            } else if (isPh || startsWith(name, "Body") || !items.empty()) {
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
    return deck;
}

}  // namespace achclwss::office
