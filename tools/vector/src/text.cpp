// Text shapes: lines of text laid out with a TrueType font into path data, so
// they render, convert to paths and take part in booleans like any other shape.
// Point text sits at (x, y); area text wraps inside its box, in columns; text
// on a path follows the path cached in the shape's `d`.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>

#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::vector {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::shared_ptr<pdfdoc::TrueType> fontFor(const Shape& s) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<pdfdoc::TrueType>> cache;
    const std::string name = lower(s.font);
    const std::string key = s.font + (s.bold ? "|b" : "|") + (s.italic ? "i" : "");
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::shared_ptr<pdfdoc::TrueType> font;
    const std::filesystem::path file(s.font);
    const std::string ext = lower(file.extension().string());
    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
        try {
            font = pdfdoc::TrueType::load(file);
        } catch (const std::exception&) {
        }
    } else if (!name.empty() && name != "sans" && name != "roboto" && name != "sans-serif") {
        if (auto family = pdfdoc::standardFamily(name)) font = pdfdoc::systemFont(*family, s.bold, s.italic);
    }
    if (!font) font = pdfdoc::builtinSans();
    cache[key] = font;
    return font;
}

unsigned upperOf(unsigned c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
unsigned lowerOf(unsigned c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}
bool isLowerLetter(unsigned c) { return upperOf(c) != c; }

struct Glyph {
    unsigned cp;
    const pdfdoc::TrueType* font;
    double size;
};

// A run of characters with the fonts and sizes they're drawn in, case options applied.
class Shaper {
public:
    explicit Shaper(const Shape& s) : s_(s), font_(fontFor(s)), fallback_(pdfdoc::builtinSans()) {}

    std::vector<Glyph> glyphs(const std::string& text) const {
        std::vector<unsigned> cps = pdfdoc::utf8Decode(text);
        bool startWord = true;
        std::vector<Glyph> out;
        for (unsigned cp : cps) {
            if (cp == '\r') continue;
            double size = s_.fontSize;
            if (s_.textCase == "upper") cp = upperOf(cp);
            else if (s_.textCase == "lower") cp = lowerOf(cp);
            else if (s_.textCase == "title") cp = startWord ? upperOf(cp) : lowerOf(cp);
            else if (s_.textCase == "small_caps" && isLowerLetter(cp)) {
                cp = upperOf(cp);
                size *= 0.75;
            }
            startWord = cp == ' ' || cp == '\t' || cp == '-';
            const pdfdoc::TrueType* f = font_.get();
            if (!(font_->hasGlyph(cp) || cp == ' ')) f = fallback_->hasGlyph(cp) ? fallback_.get() : font_.get();
            if (f == font_.get() && !font_->hasGlyph(cp) && cp != ' ') cp = '?';
            out.push_back({cp, f, size});
        }
        return out;
    }
    // Advance of glyph i, with kerning towards the next one and letter spacing.
    double advance(const std::vector<Glyph>& g, std::size_t i) const {
        double a = g[i].font->advance(g[i].cp) * g[i].size / 1000;
        if (i + 1 < g.size() && g[i + 1].font == g[i].font) a += g[i].font->kern(g[i].cp, g[i + 1].cp) * g[i].size / 1000;
        return a + (i + 1 < g.size() ? s_.letterSpacing : 0);
    }
    double width(const std::vector<Glyph>& g) const {
        double w = 0;
        for (std::size_t i = 0; i < g.size(); ++i) w += advance(g, i);
        return w;
    }
    double width(const std::string& text) const { return width(glyphs(text)); }
    double ascent() const { return font_->metrics().ascent * s_.fontSize / 1000; }

private:
    const Shape& s_;
    std::shared_ptr<pdfdoc::TrueType> font_, fallback_;
};

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find('\n', start);
        out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Glyph outlines placed one by one: rotated around their origin when `angle` isn't 0.
std::string placeGlyph(const Glyph& g, double x, double y, double angle) {
    if (angle == 0) return g.font->svgPath(g.cp, x, y, g.size);
    BezierPath bp = parsePath(g.font->svgPath(g.cp, 0, 0, g.size));
    transformPath(bp, Matrix::translate(x, y) * Matrix::rotate(angle * 180 / 3.14159265358979323846));
    return pathData(bp);
}

std::string lineGlyphs(const Shaper& shaper, const std::vector<Glyph>& g, double x, double y, double wordGap = 0) {
    std::string d;
    for (std::size_t i = 0; i < g.size(); ++i) {
        d += placeGlyph(g[i], x, y, 0);
        x += shaper.advance(g, i) + (g[i].cp == ' ' ? wordGap : 0);
    }
    return d;
}

}  // namespace

namespace detail {

std::vector<TextLine> layoutArea(const Shape& s, bool* overflow) {
    std::vector<TextLine> out;
    if (overflow) *overflow = false;
    const Shaper shaper(s);
    const int cols = std::max(1, s.columns);
    const double gap = std::max(0.0, s.columnGap);
    const double colW = std::max(1.0, (s.boxWidth - gap * (cols - 1)) / cols);
    const double step = s.fontSize * s.lineHeight, first = shaper.ascent();
    const double space = shaper.width(" ");
    int col = 0;
    double y = s.y + first;
    auto place = [&](const std::string& text, double width, bool lastOfParagraph) {
        if (s.boxHeight > 0 && y > s.y + s.boxHeight + 1e-6) {  // this column is full: on to the next
            ++col;
            y = s.y + first;
        }
        if (col >= cols) {
            if (overflow) *overflow = true;
            return;
        }
        TextLine line;
        line.text = text;
        line.width = width;
        line.y = y;
        const double left = s.x + col * (colW + gap);
        line.x = left;
        if (s.align == "center") line.x = left + (colW - width) / 2;
        else if (s.align == "right") line.x = left + colW - width;
        else if (s.align == "justify" && !lastOfParagraph) {
            int gaps = 0;
            for (char c : text) gaps += c == ' ';
            if (gaps > 0) line.wordGap = (colW - width) / gaps;
            line.width = colW;
        }
        out.push_back(line);
        y += step;
    };
    for (const auto& para : splitLines(s.text)) {
        const auto ws = words(para);
        if (ws.empty()) {
            place("", 0, true);
            continue;
        }
        std::string cur;
        double curW = 0;
        for (const auto& w : ws) {
            double ww = shaper.width(w);
            // a word longer than the column is split by characters
            std::string word = w;
            while (ww > colW && cur.empty()) {
                std::string head;
                double hw = 0;
                std::size_t i = 0;
                for (; i < word.size(); ++i) {
                    const std::size_t len = (static_cast<unsigned char>(word[i]) >= 0xF0) ? 4 : (static_cast<unsigned char>(word[i]) >= 0xE0) ? 3
                                            : (static_cast<unsigned char>(word[i]) >= 0xC0) ? 2 : 1;
                    const std::string ch = word.substr(i, len);
                    const double cw = shaper.width(head + ch);
                    if (cw > colW && !head.empty()) break;
                    head += ch;
                    hw = cw;
                    i += len - 1;
                }
                place(head, hw, false);
                word = word.substr(head.size());
                ww = shaper.width(word);
                if (word.empty()) break;
            }
            if (word.empty()) continue;
            if (cur.empty()) {
                cur = word;
                curW = ww;
            } else if (curW + space + ww <= colW) {
                cur += " " + word;
                curW = shaper.width(cur);
            } else {
                place(cur, curW, false);
                cur = word;
                curW = ww;
            }
        }
        if (!cur.empty()) place(cur, curW, true);
    }
    return out;
}

}  // namespace detail

std::vector<std::string> wrapText(const Shape& s) {
    std::vector<std::string> out;
    if (s.boxWidth > 0) {
        for (const auto& l : detail::layoutArea(s, nullptr)) out.push_back(l.text);
        return out;
    }
    return splitLines(s.text);
}

std::string textPathData(const Shape& s) {
    const Shaper shaper(s);
    std::string d;
    if (!s.onPath.empty() && !s.d.empty()) {
        // along the path: each glyph centred on its spot, turned to the path's direction
        const Outline o = flatten(parsePath(s.d), 2.0);
        std::vector<Point> pts;
        for (const auto& c : o) {
            pts = c.points;
            if (c.closed && !pts.empty()) pts.push_back(pts.front());
            break;  // the first subpath
        }
        if (pts.size() < 2) return d;
        std::vector<double> at{0};
        for (std::size_t i = 1; i < pts.size(); ++i) at.push_back(at.back() + std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y));
        std::string flat = s.text;
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        const auto g = shaper.glyphs(flat);
        const double total = at.back(), width = shaper.width(g);
        double u = s.pathOffset;
        if (s.anchor == "middle") u += (total - width) / 2;
        else if (s.anchor == "end") u += total - width;
        std::size_t seg = 1;
        for (std::size_t i = 0; i < g.size(); ++i) {
            const double adv = shaper.advance(g, i), mid = u + adv / 2;
            u += adv;
            if (mid < 0 || mid > total) continue;
            while (seg + 1 < at.size() && at[seg] < mid) ++seg;
            seg = std::min(seg, at.size() - 1);
            const Point a = pts[seg - 1], b = pts[seg];
            const double segLen = std::max(at[seg] - at[seg - 1], 1e-12), f = (mid - at[seg - 1]) / segLen;
            const double angle = std::atan2(b.y - a.y, b.x - a.x);
            const Point m{a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f};
            d += placeGlyph(g[i], m.x - std::cos(angle) * adv / 2, m.y - std::sin(angle) * adv / 2, angle);
        }
        return d;
    }
    if (s.boxWidth > 0) {
        for (const auto& line : detail::layoutArea(s, nullptr)) d += lineGlyphs(shaper, shaper.glyphs(line.text), line.x, line.y, line.wordGap);
        return d;
    }
    double y = s.y;
    for (const auto& line : splitLines(s.text)) {
        const auto g = shaper.glyphs(line);
        const double width = shaper.width(g);
        double x = s.x;
        if (s.anchor == "middle") x -= width / 2;
        else if (s.anchor == "end") x -= width;
        d += lineGlyphs(shaper, g, x, y);
        y += s.fontSize * s.lineHeight;
    }
    return d;
}

std::string toPathData(const Shape& s, double detail) {
    if (s.kind == ShapeKind::Text) return textPathData(s);
    if (s.kind == ShapeKind::Path) return s.d;
    BezierPath bp = shapePath(s);
    if (!bp.empty()) return pathData(bp);
    return outlinePathData(localOutline(s, detail));
}

}  // namespace wetwaresuite::vector
