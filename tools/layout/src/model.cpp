// Documents: presets, styles, colours, markup parsing and the .achlay file format.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::layout {

using nlohmann::json;

// ---- small helpers -------------------------------------------------------------

std::string num(double v, int decimals) {
    if (std::abs(v) < 0.5 * std::pow(10.0, -decimals)) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* toString(FrameType t) {
    switch (t) {
        case FrameType::Text: return "text";
        case FrameType::Image: return "image";
        case FrameType::Rect: return "rect";
        case FrameType::Ellipse: return "ellipse";
        case FrameType::Line: return "line";
        case FrameType::Vector: return "vector";
    }
    return "text";
}
const char* toString(Align a) {
    switch (a) {
        case Align::Left: return "left";
        case Align::Center: return "center";
        case Align::Right: return "right";
        case Align::Justify: return "justify";
        case Align::Inside: return "inside";
        case Align::Outside: return "outside";
    }
    return "left";
}
const char* toString(VAlign a) {
    switch (a) {
        case VAlign::Top: return "top";
        case VAlign::Middle: return "middle";
        case VAlign::Bottom: return "bottom";
    }
    return "top";
}
const char* toString(Fit f) {
    switch (f) {
        case Fit::Fit: return "fit";
        case Fit::Fill: return "fill";
        case Fit::Stretch: return "stretch";
        case Fit::Manual: return "manual";
    }
    return "fit";
}
std::optional<FrameType> parseFrameType(const std::string& s) {
    for (auto t : {FrameType::Text, FrameType::Image, FrameType::Rect, FrameType::Ellipse, FrameType::Line, FrameType::Vector}) {
        if (s == toString(t)) return t;
    }
    if (s == "shape" || s == "rectangle") return FrameType::Rect;
    if (s == "svg") return FrameType::Vector;
    return std::nullopt;
}
std::optional<Align> parseAlign(const std::string& s) {
    for (auto a : {Align::Left, Align::Center, Align::Right, Align::Justify, Align::Inside, Align::Outside}) {
        if (s == toString(a)) return a;
    }
    if (s == "centre") return Align::Center;
    return std::nullopt;
}
std::optional<VAlign> parseVAlign(const std::string& s) {
    for (auto a : {VAlign::Top, VAlign::Middle, VAlign::Bottom}) {
        if (s == toString(a)) return a;
    }
    if (s == "center" || s == "centre") return VAlign::Middle;
    return std::nullopt;
}
std::optional<Fit> parseFit(const std::string& s) {
    for (auto f : {Fit::Fit, Fit::Fill, Fit::Stretch, Fit::Manual}) {
        if (s == toString(f)) return f;
    }
    if (s == "contain") return Fit::Fit;
    if (s == "cover" || s == "crop") return Fit::Fill;
    return std::nullopt;
}

void CharProps::merge(const CharProps& o) {
    if (o.font) font = o.font;
    if (o.size) size = o.size;
    if (o.bold) bold = o.bold;
    if (o.italic) italic = o.italic;
    if (o.underline) underline = o.underline;
    if (o.color) color = o.color;
    if (o.tracking) tracking = o.tracking;
}

void ParaProps::merge(const ParaProps& o) {
    if (o.leading) leading = o.leading;
    if (o.align) align = o.align;
    if (o.spaceBefore) spaceBefore = o.spaceBefore;
    if (o.spaceAfter) spaceAfter = o.spaceAfter;
    if (o.indent) indent = o.indent;
    if (o.leftIndent) leftIndent = o.leftIndent;
    if (o.breakBefore) breakBefore = o.breakBefore;
}

std::string Paragraph::text() const {
    std::string s;
    for (const auto& r : runs) s += r.text;
    return s;
}

std::size_t Story::length() const {
    std::size_t n = 0;
    for (const auto& p : paragraphs) {
        for (const auto& r : p.runs) n += pdfdoc::utf8Decode(r.text).size();
        n += 1;
    }
    return n;
}

// ---- documents -----------------------------------------------------------------

std::string LayoutDoc::summary() const {
    std::size_t frames = 0;
    for (const auto& p : pages) frames += p.frames.size();
    std::string s = std::to_string(pages.size()) + (pages.size() == 1 ? " page " : " pages ") + num(width, 1) + " x " +
                    num(height, 1) + " mm, " + std::to_string(frames) + " frames";
    if (!title.empty()) s = "\"" + title + "\", " + s;
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> LayoutDoc::preview() const {
    if (pages.empty()) return nullptr;
    RenderOptions o;
    o.dpi = std::clamp(512.0 / std::max(pages[0].width, pages[0].height) * 25.4, 10.0, 300.0);
    o.guides = true;
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(renderPage(*this, 0, o)), "page1.png"});
}

Frame* LayoutDoc::findFrame(const std::string& key, int* page, Master** master) {
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t i = 0; i < pages.size(); ++i) {
            for (auto& f : pages[i].frames) {
                if (pass == 0 ? f.id == key : f.name == key) {
                    if (page) *page = static_cast<int>(i);
                    if (master) *master = nullptr;
                    return &f;
                }
            }
        }
        for (auto& m : masters) {
            for (auto& f : m.frames) {
                if (pass == 0 ? f.id == key : f.name == key) {
                    if (page) *page = -1;
                    if (master) *master = &m;
                    return &f;
                }
            }
        }
    }
    return nullptr;
}

const Frame* LayoutDoc::findFrame(const std::string& key, int* page) const {
    return const_cast<LayoutDoc*>(this)->findFrame(key, page, nullptr);
}

const Style* LayoutDoc::findStyle(const std::string& name) const {
    for (const auto& s : styles) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const Swatch* LayoutDoc::findSwatch(const std::string& name) const {
    for (const auto& s : swatches) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

Master* LayoutDoc::findMaster(const std::string& name) {
    for (auto& m : masters) {
        if (m.name == name) return &m;
    }
    return nullptr;
}
const Master* LayoutDoc::findMaster(const std::string& name) const { return const_cast<LayoutDoc*>(this)->findMaster(name); }

const FontFamily* LayoutDoc::findFont(const std::string& name) const {
    for (const auto& f : fonts) {
        if (lower(f.name) == lower(name)) return &f;
    }
    return nullptr;
}

const std::vector<PaperSize>& paperSizes() {
    static const std::vector<PaperSize> sizes = {
        {"A0", 841, 1189}, {"A1", 594, 841}, {"A2", 420, 594}, {"A3", 297, 420}, {"A4", 210, 297}, {"A5", 148, 210},
        {"A6", 105, 148}, {"A7", 74, 105}, {"B4", 250, 353}, {"B5", 176, 250}, {"DL", 99, 210},
        {"letter", 215.9, 279.4}, {"legal", 215.9, 355.6}, {"tabloid", 279.4, 431.8}, {"half_letter", 139.7, 215.9},
        {"card", 85, 55}, {"card_us", 88.9, 50.8}, {"6x9", 152.4, 228.6}, {"5x8", 127, 203.2},
        {"square_210", 210, 210},
    };
    return sizes;
}

std::optional<PaperSize> findPaper(const std::string& name) {
    for (const auto& p : paperSizes()) {
        if (lower(p.name) == lower(name)) return p;
    }
    return std::nullopt;
}

const std::vector<std::string>& presetNames() {
    static const std::vector<std::string> names = {"poster_a2", "poster_a3", "flyer_a5", "flyer_a4", "card", "a4_report",
                                                   "book_6x9", "letter"};
    return names;
}

namespace {

Style paraStyle(const std::string& name, const std::string& basedOn) {
    Style s;
    s.name = name;
    s.basedOn = basedOn;
    return s;
}

// Adds a master with a page-number footer (and optional running header) inside the margins.
void numberedMaster(Layout& doc, const std::string& header, Align align) {
    Master m;
    m.name = "A";
    const Box b{doc.margins.left, doc.margins.top, doc.width - doc.margins.left - doc.margins.right,
                doc.height - doc.margins.top - doc.margins.bottom};
    auto textFrame = [&](double y, const std::string& text) {
        Frame f;
        f.id = "f" + std::to_string(doc.nextFrame++);
        f.type = FrameType::Text;
        f.x = b.x;
        f.y = y;
        f.w = b.w;
        f.h = 6;
        f.mirror = doc.facing;
        f.story = "t" + std::to_string(doc.nextStory++);
        Story st;
        Paragraph p;
        p.style = "folio";
        p.props.align = align;
        p.runs.push_back({text, {}, {}});
        st.paragraphs.push_back(p);
        doc.stories[f.story] = st;
        return f;
    };
    if (!header.empty()) {
        Frame h = textFrame(std::max(2.0, doc.margins.top - 9), header);
        h.name = "header";
        m.frames.push_back(h);
    }
    Frame foot = textFrame(doc.height - doc.margins.bottom + 3, "{page}");
    foot.name = "footer";
    m.frames.push_back(foot);
    doc.masters.push_back(m);
}

}  // namespace

void addDefaultStyles(Layout& doc) {
    Style body = paraStyle("body", "");
    body.chars.font = "helvetica";
    body.chars.size = 10;
    body.chars.bold = false;
    body.chars.italic = false;
    body.chars.underline = false;
    body.chars.color = "black";
    body.chars.tracking = 0;
    body.para.leading = 13;
    body.para.align = Align::Left;
    body.para.spaceBefore = 0;
    body.para.spaceAfter = 4;
    body.para.indent = 0;
    body.para.leftIndent = 0;
    body.para.breakBefore = false;
    auto heading = [](const std::string& name, double size, double leading, double before, double after) {
        Style s = paraStyle(name, "body");
        s.chars.size = size;
        s.chars.bold = true;
        s.para.align = Align::Left;
        s.para.leading = leading;
        s.para.spaceBefore = before;
        s.para.spaceAfter = after;
        return s;
    };
    Style quote = paraStyle("quote", "body");
    quote.chars.italic = true;
    quote.para.leftIndent = 8;
    Style bullet = paraStyle("bullet", "body");
    bullet.para.leftIndent = 5;
    bullet.para.indent = -3.5;
    bullet.para.spaceAfter = 2;
    Style code = paraStyle("code", "body");
    code.chars.font = "courier";
    code.chars.size = 9;
    code.para.leading = 11;
    code.para.spaceAfter = 0;
    Style caption = paraStyle("caption", "body");
    caption.chars.size = 8;
    caption.chars.italic = true;
    caption.para.align = Align::Left;
    caption.para.leading = 10;
    Style folio = paraStyle("folio", "body");
    folio.chars.size = 8;
    folio.para.leading = 10;
    folio.para.spaceAfter = 0;
    Style headline = heading("headline", 60, 62, 0, 6);
    Style subhead = paraStyle("subhead", "body");
    subhead.chars.size = 20;
    subhead.para.align = Align::Left;
    subhead.para.leading = 24;
    subhead.para.spaceAfter = 6;
    Style strong;
    strong.name = "strong";
    strong.character = true;
    strong.chars.bold = true;
    Style emphasis;
    emphasis.name = "emphasis";
    emphasis.character = true;
    emphasis.chars.italic = true;
    for (auto& s : std::vector<Style>{body, heading("h1", 24, 28, 12, 8), heading("h2", 16, 20, 10, 6), heading("h3", 12, 15, 8, 4),
                                      quote, bullet, code, caption, folio, headline, subhead, strong, emphasis}) {
        bool exists = false;
        for (const auto& e : doc.styles) exists = exists || e.name == s.name;
        if (!exists) doc.styles.push_back(s);
    }
}

std::shared_ptr<LayoutDoc> newLayout(const std::string& preset) {
    auto doc = std::make_shared<LayoutDoc>();
    addDefaultStyles(*doc);
    auto size = [&](double w, double h) {
        doc->width = w;
        doc->height = h;
    };
    auto margins = [&](double t, double b, double l, double r) { doc->margins = {t, b, l, r}; };
    const std::string p = lower(preset);
    if (p.empty() || p == "a4") {
        margins(15, 15, 15, 15);
    } else if (p == "poster_a2" || p == "poster_a3") {
        if (p == "poster_a2") size(420, 594);
        else size(297, 420);
        margins(20, 20, 20, 20);
    } else if (p == "flyer_a5") {
        size(148, 210);
        margins(10, 10, 10, 10);
    } else if (p == "flyer_a4") {
        size(297, 210);
        margins(10, 10, 10, 10);
        doc->columns = 3;
        doc->gutter = 20;
    } else if (p == "card") {
        size(85, 55);
        margins(4, 4, 4, 4);
        doc->bleed = 2;
    } else if (p == "a4_report") {
        margins(20, 25, 20, 20);
        numberedMaster(*doc, "", Align::Center);
    } else if (p == "book_6x9") {
        size(152.4, 228.6);
        margins(16, 20, 19, 14);
        doc->facing = true;
        numberedMaster(*doc, "{title}", Align::Outside);
    } else if (p == "letter") {
        size(215.9, 279.4);
        margins(19, 19, 19, 19);
    } else {
        throw std::runtime_error("unknown preset '" + preset + "'");
    }
    Page page;
    page.width = doc->width;
    page.height = doc->height;
    if (!doc->masters.empty()) page.master = "A";
    doc->pages.push_back(page);
    return doc;
}

Box marginBox(const Layout& doc, int pageIndex) {
    const Page* pg = pageIndex >= 0 && pageIndex < static_cast<int>(doc.pages.size()) ? &doc.pages[static_cast<std::size_t>(pageIndex)] : nullptr;
    const double w = pg ? pg->width : doc.width, h = pg ? pg->height : doc.height;
    double left = doc.margins.left, right = doc.margins.right;
    if (doc.facing && pageIndex % 2 == 1) std::swap(left, right);  // even page numbers are left-hand pages
    return {left, doc.margins.top, std::max(1.0, w - left - right), std::max(1.0, h - doc.margins.top - doc.margins.bottom)};
}

Affine frameToPage(const Frame& f) {
    const double t = f.rotation * kPi / 180.0, cs = std::cos(t), sn = std::sin(t);
    const double cx = f.x + f.w / 2, cy = f.y + f.h / 2;
    // p = c + R (u - w/2, v - h/2), R rotating clockwise on a y-down page
    return {cs, sn, -sn, cs, cx - cs * f.w / 2 + sn * f.h / 2, cy - sn * f.w / 2 - cs * f.h / 2};
}

Frame placedMasterFrame(const Layout& doc, const Frame& f, int pageIndex) {
    Frame out = f;
    if (f.mirror && doc.facing && pageIndex % 2 == 1) {
        const double pw = pageIndex < static_cast<int>(doc.pages.size()) ? doc.pages[static_cast<std::size_t>(pageIndex)].width : doc.width;
        out.x = pw - f.x - f.w;
    }
    return out;
}

Box frameBounds(const Frame& f) {
    const Affine m = frameToPage(f);
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (auto [u, v] : {std::pair{0.0, 0.0}, {f.w, 0.0}, {0.0, f.h}, {f.w, f.h}}) {
        const auto p = m.apply(u, v);
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    if (f.type == FrameType::Line || f.stroke.size()) {
        const double s = f.stroke.empty() && f.type != FrameType::Line ? 0 : f.strokeWidth * kPtToMm / 2;
        x0 -= s;
        y0 -= s;
        x1 += s;
        y1 += s;
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

// ---- colours -----------------------------------------------------------------

Color cmykToRgb(double c, double m, double y, double k) {
    auto ch = [&](double v) { return static_cast<std::uint8_t>(std::lround(255 * (1 - std::clamp(v, 0.0, 1.0)) * (1 - std::clamp(k, 0.0, 1.0)))); };
    return {ch(c), ch(m), ch(y), 255};
}

namespace {

std::optional<std::vector<double>> parseCmyk(const std::string& spec) {
    std::string s = lower(spec);
    if (s.rfind("cmyk(", 0) != 0 || s.back() != ')') return std::nullopt;
    std::vector<double> v;
    std::stringstream in(s.substr(5, s.size() - 6));
    std::string item;
    while (std::getline(in, item, ',')) {
        try {
            v.push_back(std::stod(item));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (v.size() != 4) return std::nullopt;
    return v;
}

}  // namespace

std::optional<Paint> resolvePaint(const Layout& doc, const std::string& spec) {
    Paint p;
    if (spec.empty() || lower(spec) == "none") return p;
    for (const auto& sw : doc.swatches) {
        if (sw.name == spec) {
            p.none = false;
            p.cmyk = sw.cmyk;
            p.c = sw.c / 100;
            p.m = sw.m / 100;
            p.y = sw.y / 100;
            p.k = sw.k / 100;
            p.rgb = sw.rgb;
            return p;
        }
    }
    // plain black and white are print colours: 100% K (no rich black on text) and paper
    if (lower(spec) == "black" || lower(spec) == "white") {
        p.none = false;
        p.cmyk = true;
        p.k = lower(spec) == "black" ? 1 : 0;
        p.rgb = cmykToRgb(0, 0, 0, p.k);
        return p;
    }
    if (auto v = parseCmyk(spec)) {
        p.none = false;
        p.cmyk = true;
        p.c = (*v)[0] / 100;
        p.m = (*v)[1] / 100;
        p.y = (*v)[2] / 100;
        p.k = (*v)[3] / 100;
        p.rgb = cmykToRgb(p.c, p.m, p.y, p.k);
        return p;
    }
    if (auto c = vector::parseColor(spec)) {
        p.none = c->a == 0;
        p.rgb = *c;
        return p;
    }
    return std::nullopt;
}

// ---- markup ------------------------------------------------------------------

namespace {

// Inline markup -> runs. code marks `mono` runs with font courier.
std::vector<Run> parseInline(const std::string& text, bool markdown) {
    std::vector<Run> runs;
    bool bold = false, italic = false, underline = false, code = false;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        Run r;
        r.text = cur;
        if (bold) r.props.bold = true;
        if (italic) r.props.italic = true;
        if (underline) r.props.underline = true;
        if (code) r.props.font = "courier";
        runs.push_back(r);
        cur.clear();
    };
    auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        const char prev = i > 0 ? text[i - 1] : ' ';
        if (c == '\\' && (next == '*' || next == '_' || next == '`' || next == '\\' || next == '[')) {
            cur += next;
            ++i;
            continue;
        }
        if (c == '`') {
            flush();
            code = !code;
            continue;
        }
        if (code) {
            cur += c;
            continue;
        }
        if (c == '*' && next == '*') {
            flush();
            bold = !bold;
            ++i;
            continue;
        }
        if (c == '_' && next == '_') {
            flush();
            if (markdown) bold = !bold;
            else underline = !underline;
            ++i;
            continue;
        }
        const bool opens = !isSpace(next) && next != '\0';
        const bool closes = !isSpace(prev);
        if (c == '*' || (markdown && c == '_')) {
            const bool star = c == '*';
            auto boundary = [&](char ch) { return ch == '\0' || isSpace(ch) || std::ispunct(static_cast<unsigned char>(ch)); };
            const bool toggle = italic ? closes && (star || boundary(next)) : opens && (star || boundary(prev));
            if (toggle) {
                flush();
                italic = !italic;
                continue;
            }
        }
        if (markdown && c == '[') {
            const auto close = text.find("](", i);
            const auto end = close == std::string::npos ? std::string::npos : text.find(')', close);
            if (end != std::string::npos) {
                cur += text.substr(i + 1, close - i - 1);
                i = end;
                continue;
            }
        }
        if (markdown && c == '!' && next == '[') {  // images: keep the alt text out
            const auto close = text.find("](", i);
            const auto end = close == std::string::npos ? std::string::npos : text.find(')', close);
            if (end != std::string::npos) {
                i = end;
                continue;
            }
        }
        cur += c == '\t' ? ' ' : c;
    }
    flush();
    if (runs.empty()) runs.push_back({});
    return runs;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

std::vector<Paragraph> parseText(const std::string& input, bool markdown, const std::string& baseStyle) {
    // literal "\n" (two characters) is a line break too, so one-line commands can hold paragraphs
    std::string text;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size() && input[i + 1] == 'n') {
            text += '\n';
            ++i;
        } else if (input[i] != '\r') {
            text += input[i];
        }
    }
    std::vector<std::string> lines;
    {
        std::stringstream in(text);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        if (!text.empty() && text.back() == '\n' && !markdown) lines.push_back("");
    }
    std::vector<Paragraph> out;
    auto para = [&](const std::string& style, const std::string& body) {
        Paragraph p;
        p.style = style;
        p.runs = parseInline(body, markdown);
        out.push_back(p);
    };
    const std::string body = baseStyle.empty() ? "body" : baseStyle;
    if (!markdown) {
        if (lines.empty()) lines.push_back("");
        if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
        for (const auto& l : lines) para(baseStyle, l);
        return out;
    }
    std::string pending;
    auto flush = [&] {
        if (!trim(pending).empty()) para(body, trim(pending));
        pending.clear();
    };
    bool inCode = false;
    for (const auto& raw : lines) {
        const std::string l = trim(raw);
        if (l.rfind("```", 0) == 0) {
            flush();
            inCode = !inCode;
            continue;
        }
        if (inCode) {
            Paragraph p;
            p.style = "code";
            p.runs.push_back({raw, {}, {}});
            out.push_back(p);
            continue;
        }
        if (l.empty()) {
            flush();
            continue;
        }
        if (l == "---" || l == "***" || l == "___") {
            flush();
            continue;
        }
        if (l[0] == '#') {
            flush();
            std::size_t level = 0;
            while (level < l.size() && l[level] == '#') ++level;
            const std::string style = level == 1 ? "h1" : level == 2 ? "h2" : "h3";
            para(style, trim(l.substr(level)));
            continue;
        }
        if (l[0] == '>') {
            flush();
            para("quote", trim(l.substr(1)));
            continue;
        }
        if ((l[0] == '-' || l[0] == '*' || l[0] == '+') && l.size() > 1 && l[1] == ' ') {
            flush();
            para("bullet", "\xE2\x80\xA2 " + trim(l.substr(2)));
            continue;
        }
        std::size_t digits = 0;
        while (digits < l.size() && std::isdigit(static_cast<unsigned char>(l[digits]))) ++digits;
        if (digits > 0 && digits + 1 < l.size() && (l[digits] == '.' || l[digits] == ')') && l[digits + 1] == ' ') {
            flush();
            para("bullet", l.substr(0, digits + 1) + " " + trim(l.substr(digits + 2)));
            continue;
        }
        pending += (pending.empty() ? "" : " ") + l;
    }
    flush();
    if (out.empty()) para(body, "");
    return out;
}

// ---- JSON --------------------------------------------------------------------

namespace {

json charJson(const CharProps& c) {
    json j = json::object();
    if (c.font) j["font"] = *c.font;
    if (c.size) j["size"] = *c.size;
    if (c.bold) j["bold"] = *c.bold;
    if (c.italic) j["italic"] = *c.italic;
    if (c.underline) j["underline"] = *c.underline;
    if (c.color) j["color"] = *c.color;
    if (c.tracking) j["tracking"] = *c.tracking;
    return j;
}

CharProps charFrom(const json& j) {
    CharProps c;
    if (!j.is_object()) return c;
    if (j.contains("font")) c.font = j["font"].get<std::string>();
    if (j.contains("size")) c.size = j["size"].get<double>();
    if (j.contains("bold")) c.bold = j["bold"].get<bool>();
    if (j.contains("italic")) c.italic = j["italic"].get<bool>();
    if (j.contains("underline")) c.underline = j["underline"].get<bool>();
    if (j.contains("color")) c.color = j["color"].get<std::string>();
    if (j.contains("tracking")) c.tracking = j["tracking"].get<double>();
    return c;
}

json paraJson(const ParaProps& p) {
    json j = json::object();
    if (p.leading) j["leading"] = *p.leading;
    if (p.align) j["align"] = toString(*p.align);
    if (p.spaceBefore) j["space_before"] = *p.spaceBefore;
    if (p.spaceAfter) j["space_after"] = *p.spaceAfter;
    if (p.indent) j["indent"] = *p.indent;
    if (p.leftIndent) j["left_indent"] = *p.leftIndent;
    if (p.breakBefore) j["break_before"] = *p.breakBefore;
    return j;
}

ParaProps paraFrom(const json& j) {
    ParaProps p;
    if (!j.is_object()) return p;
    if (j.contains("leading")) p.leading = j["leading"].get<double>();
    if (j.contains("align")) p.align = parseAlign(j["align"].get<std::string>());
    if (j.contains("space_before")) p.spaceBefore = j["space_before"].get<double>();
    if (j.contains("space_after")) p.spaceAfter = j["space_after"].get<double>();
    if (j.contains("indent")) p.indent = j["indent"].get<double>();
    if (j.contains("left_indent")) p.leftIndent = j["left_indent"].get<double>();
    if (j.contains("break_before")) p.breakBefore = j["break_before"].get<bool>();
    return p;
}

json frameJson(const Frame& f) {
    json j = {{"id", f.id}, {"type", toString(f.type)}, {"x", f.x}, {"y", f.y}, {"w", f.w}, {"h", f.h}};
    if (!f.name.empty()) j["name"] = f.name;
    if (f.rotation != 0) j["rotation"] = f.rotation;
    if (!f.fill.empty()) j["fill"] = f.fill;
    if (!f.stroke.empty()) j["stroke"] = f.stroke;
    if (f.strokeWidth != 1) j["stroke_width"] = f.strokeWidth;
    if (f.opacity != 1) j["opacity"] = f.opacity;
    if (f.radius != 0) j["radius"] = f.radius;
    if (f.mirror) j["mirror"] = true;
    if (f.type == FrameType::Text) {
        j["story"] = f.story;
        if (!f.next.empty()) j["next"] = f.next;
        j["columns"] = f.columns;
        j["gutter"] = f.gutter;
        if (f.inset != 0) j["inset"] = f.inset;
        if (f.valign != VAlign::Top) j["valign"] = toString(f.valign);
    }
    if (f.type == FrameType::Image || f.type == FrameType::Vector) {
        j["path"] = f.path;
        j["fit"] = toString(f.fit);
        if (f.fit == Fit::Manual) {
            j["image_scale"] = f.imageScale;
            j["image_x"] = f.imageX;
            j["image_y"] = f.imageY;
        }
    }
    return j;
}

Frame frameFrom(const json& j) {
    Frame f;
    f.id = j.at("id").get<std::string>();
    f.type = parseFrameType(j.value("type", "text")).value_or(FrameType::Text);
    f.x = j.value("x", 0.0);
    f.y = j.value("y", 0.0);
    f.w = j.value("w", 0.0);
    f.h = j.value("h", 0.0);
    f.name = j.value("name", "");
    f.rotation = j.value("rotation", 0.0);
    f.fill = j.value("fill", "");
    f.stroke = j.value("stroke", "");
    f.strokeWidth = j.value("stroke_width", 1.0);
    f.opacity = j.value("opacity", 1.0);
    f.radius = j.value("radius", 0.0);
    f.mirror = j.value("mirror", false);
    f.story = j.value("story", "");
    f.next = j.value("next", "");
    f.columns = j.value("columns", 1);
    f.gutter = j.value("gutter", 5.0);
    f.inset = j.value("inset", 0.0);
    f.valign = parseVAlign(j.value("valign", "top")).value_or(VAlign::Top);
    f.path = j.value("path", "");
    f.fit = parseFit(j.value("fit", "fit")).value_or(Fit::Fit);
    f.imageScale = j.value("image_scale", 100.0);
    f.imageX = j.value("image_x", 0.0);
    f.imageY = j.value("image_y", 0.0);
    return f;
}

}  // namespace

std::string toJson(const LayoutDoc& doc) {
    json j;
    j["format"] = "wetwaresuite-layout";
    j["version"] = 1;
    j["title"] = doc.title;
    j["width"] = doc.width;
    j["height"] = doc.height;
    j["margins"] = {{"top", doc.margins.top}, {"bottom", doc.margins.bottom}, {"left", doc.margins.left}, {"right", doc.margins.right}};
    j["bleed"] = doc.bleed;
    j["columns"] = doc.columns;
    j["gutter"] = doc.gutter;
    j["facing"] = doc.facing;
    j["next_frame"] = doc.nextFrame;
    j["next_story"] = doc.nextStory;
    json pages = json::array();
    for (const auto& p : doc.pages) {
        json pj = {{"width", p.width}, {"height", p.height}, {"master", p.master}, {"frames", json::array()}};
        for (const auto& f : p.frames) pj["frames"].push_back(frameJson(f));
        pages.push_back(pj);
    }
    j["pages"] = pages;
    json masters = json::array();
    for (const auto& m : doc.masters) {
        json mj = {{"name", m.name}, {"frames", json::array()}};
        for (const auto& f : m.frames) mj["frames"].push_back(frameJson(f));
        masters.push_back(mj);
    }
    j["masters"] = masters;
    json stories = json::object();
    for (const auto& [id, s] : doc.stories) {
        json paras = json::array();
        for (const auto& p : s.paragraphs) {
            json pj = {{"style", p.style}, {"runs", json::array()}};
            json props = paraJson(p.props);
            if (!props.empty()) pj["props"] = props;
            for (const auto& r : p.runs) {
                json rj = {{"text", r.text}};
                if (!r.style.empty()) rj["style"] = r.style;
                json cp = charJson(r.props);
                if (!cp.empty()) rj["props"] = cp;
                pj["runs"].push_back(rj);
            }
            paras.push_back(pj);
        }
        stories[id] = paras;
    }
    j["stories"] = stories;
    json styles = json::array();
    for (const auto& s : doc.styles) {
        styles.push_back({{"name", s.name}, {"kind", s.character ? "character" : "paragraph"}, {"based_on", s.basedOn},
                          {"chars", charJson(s.chars)}, {"para", paraJson(s.para)}});
    }
    j["styles"] = styles;
    json swatches = json::array();
    for (const auto& s : doc.swatches) {
        json sj = {{"name", s.name}, {"rgb", s.rgb.hex()}};
        if (s.cmyk) sj["cmyk"] = {s.c, s.m, s.y, s.k};
        swatches.push_back(sj);
    }
    j["swatches"] = swatches;
    json fonts = json::array();
    for (const auto& f : doc.fonts) {
        fonts.push_back({{"name", f.name}, {"regular", f.regular}, {"bold", f.bold}, {"italic", f.italic}, {"bold_italic", f.boldItalic}});
    }
    j["fonts"] = fonts;
    return j.dump(1);
}

std::shared_ptr<LayoutDoc> fromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a layout file: ") + e.what());
    }
    if (!j.is_object() || j.value("format", "") != "wetwaresuite-layout") throw std::runtime_error("not an .achlay layout file");
    auto doc = std::make_shared<LayoutDoc>();
    try {
        doc->title = j.value("title", "");
        doc->width = j.value("width", 210.0);
        doc->height = j.value("height", 297.0);
        if (j.contains("margins")) {
            const auto& m = j["margins"];
            doc->margins = {m.value("top", 12.7), m.value("bottom", 12.7), m.value("left", 12.7), m.value("right", 12.7)};
        }
        doc->bleed = j.value("bleed", 3.0);
        doc->columns = j.value("columns", 1);
        doc->gutter = j.value("gutter", 5.0);
        doc->facing = j.value("facing", false);
        doc->nextFrame = j.value("next_frame", 1);
        doc->nextStory = j.value("next_story", 1);
        for (const auto& pj : j.value("pages", json::array())) {
            Page p;
            p.width = pj.value("width", doc->width);
            p.height = pj.value("height", doc->height);
            p.master = pj.value("master", "");
            for (const auto& fj : pj.value("frames", json::array())) p.frames.push_back(frameFrom(fj));
            doc->pages.push_back(p);
        }
        for (const auto& mj : j.value("masters", json::array())) {
            Master m;
            m.name = mj.value("name", "A");
            for (const auto& fj : mj.value("frames", json::array())) m.frames.push_back(frameFrom(fj));
            doc->masters.push_back(m);
        }
        const json stories = j.value("stories", json::object());
        for (const auto& [id, paras] : stories.items()) {
            Story s;
            for (const auto& pj : paras) {
                Paragraph p;
                p.style = pj.value("style", "");
                if (pj.contains("props")) p.props = paraFrom(pj["props"]);
                for (const auto& rj : pj.value("runs", json::array())) {
                    Run r;
                    r.text = rj.value("text", "");
                    r.style = rj.value("style", "");
                    if (rj.contains("props")) r.props = charFrom(rj["props"]);
                    p.runs.push_back(r);
                }
                s.paragraphs.push_back(p);
            }
            doc->stories[id] = s;
        }
        for (const auto& sj : j.value("styles", json::array())) {
            Style s;
            s.name = sj.value("name", "");
            s.character = sj.value("kind", "paragraph") == "character";
            s.basedOn = sj.value("based_on", "");
            if (sj.contains("chars")) s.chars = charFrom(sj["chars"]);
            if (sj.contains("para")) s.para = paraFrom(sj["para"]);
            doc->styles.push_back(s);
        }
        for (const auto& sj : j.value("swatches", json::array())) {
            Swatch s;
            s.name = sj.value("name", "");
            s.rgb = raster::Color::parse(sj.value("rgb", "#000000")).value_or(Color{});
            if (sj.contains("cmyk") && sj["cmyk"].size() == 4) {
                s.cmyk = true;
                s.c = sj["cmyk"][0].get<double>();
                s.m = sj["cmyk"][1].get<double>();
                s.y = sj["cmyk"][2].get<double>();
                s.k = sj["cmyk"][3].get<double>();
            }
            doc->swatches.push_back(s);
        }
        for (const auto& fj : j.value("fonts", json::array())) {
            doc->fonts.push_back({fj.value("name", ""), fj.value("regular", ""), fj.value("bold", ""), fj.value("italic", ""),
                                  fj.value("bold_italic", "")});
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("broken layout file: ") + e.what());
    }
    addDefaultStyles(*doc);
    return doc;
}

std::string readFile(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::shared_ptr<LayoutDoc> openLayout(const std::filesystem::path& path) {
    auto doc = fromJson(readFile(path.string()));
    doc->path = path.string();
    return doc;
}

void saveLayout(LayoutDoc& doc, const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out << toJson(doc);
    if (!out) throw std::runtime_error("failed writing " + path.string());
    doc.path = path.string();
}

// ---- undo ----------------------------------------------------------------------

Result LayoutEdit::apply() {
    before_ = static_cast<const Layout&>(*doc_);
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void LayoutEdit::revert() { static_cast<Layout&>(*doc_) = before_; }

}  // namespace wetwaresuite::layout
