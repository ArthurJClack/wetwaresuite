// Rendering from the font's own outlines: an antialiased nonzero rasterizer,
// text lines, glyph views with points and metrics, overview grids, and proof
// sheets as PNG or PDF (the same layout drawn on two kinds of canvas).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/pdfdoc/writer.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "internal.hpp"

namespace wetwaresuite::typeface {

namespace {

void blendPixel(Image& img, int x, int y, Color c, double coverage) {
    const double a = std::clamp(coverage, 0.0, 1.0) * c.a / 255.0;
    if (a <= 0) return;
    std::uint8_t* p = img.pixel(x, y);
    const double da = p[3] / 255.0, oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto mix = [&](std::uint8_t d, std::uint8_t s) { return static_cast<std::uint8_t>(std::lround((s * a + d * da * (1 - a)) / oa)); };
    p[0] = mix(p[0], c.r);
    p[1] = mix(p[1], c.g);
    p[2] = mix(p[2], c.b);
    p[3] = static_cast<std::uint8_t>(std::lround(oa * 255));
}

// Polygons in pixel coordinates, nonzero winding, 4 sub-scanlines with exact horizontal coverage.
void fillPolygons(Image& img, const std::vector<Polygon>& polys, Color color) {
    struct E {
        double x0, y0, x1, y1;
        int dir;
    };
    std::vector<E> edges;
    double minY = 1e18, maxY = -1e18, minX = 1e18, maxX = -1e18;
    for (const auto& p : polys) {
        for (std::size_t i = 0, n = p.size(); i < n && n >= 3; ++i) {
            const Vec a = p[i], b = p[(i + 1) % n];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(b.x) || !std::isfinite(b.y)) continue;
            minX = std::min({minX, a.x, b.x});
            maxX = std::max({maxX, a.x, b.x});
            if (a.y == b.y) continue;
            if (a.y < b.y) edges.push_back({a.x, a.y, b.x, b.y, 1});
            else edges.push_back({b.x, b.y, a.x, a.y, -1});
            minY = std::min({minY, a.y, b.y});
            maxY = std::max({maxY, a.y, b.y});
        }
    }
    if (edges.empty()) return;
    const int W = img.width(), H = img.height();
    const int r0 = std::max(0, static_cast<int>(std::floor(minY))), r1 = std::min(H - 1, static_cast<int>(std::ceil(maxY)));
    const int c0 = std::max(0, static_cast<int>(std::floor(minX))), c1 = std::min(W - 1, static_cast<int>(std::ceil(maxX)));
    if (r0 > r1 || c0 > c1) return;
    std::sort(edges.begin(), edges.end(), [](const E& a, const E& b) { return a.y0 < b.y0; });
    const int S = 4;
    std::vector<double> acc(static_cast<std::size_t>(c1 - c0 + 2), 0.0);
    std::vector<std::pair<double, int>> xs;
    for (int row = r0; row <= r1; ++row) {
        std::fill(acc.begin(), acc.end(), 0.0);
        bool any = false;
        for (int s = 0; s < S; ++s) {
            const double sy = row + (s + 0.5) / S;
            xs.clear();
            for (std::size_t i = 0; i < edges.size(); ++i) {
                const E& e = edges[i];
                if (e.y0 > sy) break;
                if (sy >= e.y1) continue;
                xs.push_back({e.x0 + (sy - e.y0) / (e.y1 - e.y0) * (e.x1 - e.x0), e.dir});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            double start = 0;
            for (const auto& [x, d] : xs) {
                const int before = wind;
                wind += d;
                if (before == 0 && wind != 0) {
                    start = x;
                } else if (before != 0 && wind == 0) {
                    double a = std::max(start, static_cast<double>(c0)), b = std::min(x, static_cast<double>(c1 + 1));
                    if (b <= a) continue;
                    any = true;
                    const double w = 1.0 / S;
                    const int ia = static_cast<int>(std::floor(a)), ib = static_cast<int>(std::floor(b));
                    if (ia == ib) {
                        acc[static_cast<std::size_t>(ia - c0)] += (b - a) * w;
                    } else {
                        acc[static_cast<std::size_t>(ia - c0)] += (ia + 1 - a) * w;
                        for (int k = ia + 1; k < ib; ++k) acc[static_cast<std::size_t>(k - c0)] += w;
                        if (ib <= c1) acc[static_cast<std::size_t>(ib - c0)] += (b - ib) * w;
                    }
                }
            }
        }
        if (!any) continue;
        for (int x = c0; x <= c1; ++x) {
            const double c = acc[static_cast<std::size_t>(x - c0)];
            if (c > 0.002) blendPixel(img, x, row, color, c);
        }
    }
}

std::unordered_map<std::uint32_t, int> cmapOf(const Font& font) {
    std::unordered_map<std::uint32_t, int> m;
    for (std::size_t i = 0; i < font.glyphs.size(); ++i) {
        for (auto u : font.glyphs[i].unicodes) m.emplace(u, static_cast<int>(i));
    }
    return m;
}


// ---- canvases ----

class Canvas {
public:
    virtual ~Canvas() = default;
    // UI text (not the font being designed), baseline at y.
    virtual void label(double x, double y, const std::string& s, double size, Color c, bool bold) = 0;
    virtual void shapes(const std::vector<Contour>& contours, const Affine& m, Color c) = 0;
    virtual void line(double x1, double y1, double x2, double y2, Color c, double w) = 0;
    double labelWidth(const std::string& s, double size, bool bold) const {
        return pdf::textWidthPx(s, pdf::Font{pdf::Family::Sans, bold, false}, size);
    }
};

class ImageCanvas : public Canvas {
public:
    ImageCanvas(Image& img, double dy) : img_(img), dy_(dy) {}
    void label(double x, double y, const std::string& s, double size, Color c, bool bold) override {
        pdf::drawText(img_, x, y + dy_, s, pdf::Font{pdf::Family::Sans, bold, false}, size, c);
    }
    void shapes(const std::vector<Contour>& contours, const Affine& m, Color c) override {
        fillContours(img_, contours, Affine::translate(0, dy_) * m, c);
    }
    void line(double x1, double y1, double x2, double y2, Color c, double w) override {
        raster::drawLine(img_, x1, y1 + dy_, x2, y2 + dy_, c, w);
    }
    void setOffset(double dy) { dy_ = dy; }

private:
    Image& img_;
    double dy_;
};

class PdfCanvas : public Canvas {
public:
    explicit PdfCanvas(double pageHeight) : h_(pageHeight) {}
    std::string content;
    void label(double x, double y, const std::string& s, double size, Color c, bool bold) override {
        content += rgb(c, "rg") + "BT /" + std::string(bold ? "F2 " : "F1 ") + pdfdoc::num(size) + " Tf " + pdfdoc::num(x) + " " +
                   pdfdoc::num(h_ - y) + " Td " + pdfdoc::text(s) + " Tj ET\n";
    }
    void shapes(const std::vector<Contour>& contours, const Affine& m, Color c) override {
        std::string path;
        auto pt = [&](Vec v) {
            v = m.apply(v);
            return pdfdoc::num(v.x) + " " + pdfdoc::num(h_ - v.y);
        };
        for (const auto& contour : contours) {
            const auto segs = segments(contour);
            if (segs.empty()) continue;
            path += pt(segs[0].p[0]) + " m\n";
            for (const auto& s : segs) {
                if (s.order == 1) {
                    path += pt(s.p[1]) + " l\n";
                } else if (s.order == 2) {
                    const Vec c1 = s.p[0] + (s.p[1] - s.p[0]) * (2.0 / 3), c2 = s.p[2] + (s.p[1] - s.p[2]) * (2.0 / 3);
                    path += pt(c1) + " " + pt(c2) + " " + pt(s.p[2]) + " c\n";
                } else {
                    path += pt(s.p[1]) + " " + pt(s.p[2]) + " " + pt(s.p[3]) + " c\n";
                }
            }
            path += "h\n";
        }
        if (!path.empty()) content += rgb(c, "rg") + path + "f\n";
    }
    void line(double x1, double y1, double x2, double y2, Color c, double w) override {
        content += rgb(c, "RG") + pdfdoc::num(w) + " w " + pdfdoc::num(x1) + " " + pdfdoc::num(h_ - y1) + " m " + pdfdoc::num(x2) + " " +
                   pdfdoc::num(h_ - y2) + " l S\n";
    }

private:
    double h_;
    static std::string rgb(Color c, const char* op) {
        return pdfdoc::num(c.r / 255.0) + " " + pdfdoc::num(c.g / 255.0) + " " + pdfdoc::num(c.b / 255.0) + " " + op + "\n";
    }
};

// Draws a run of the font's glyphs on a canvas; returns the width in px. Stops before maxWidth (0 = no limit).
class Typesetter {
public:
    explicit Typesetter(const Font& f) : font_(f), cmap_(cmapOf(f)), notdef_(f.index(".notdef")) {}

    int glyphFor(std::uint32_t cp) const {
        auto it = cmap_.find(cp);
        return it == cmap_.end() ? notdef_ : it->second;
    }

    const std::vector<Contour>& outlineOf(int i) {
        auto it = cache_.find(i);
        if (it != cache_.end()) return it->second;
        return cache_[i] = outline(font_, font_.glyphs[static_cast<std::size_t>(i)]);
    }

    double advanceUnits(const std::vector<int>& run, const TextStyle& st) const {
        double x = 0;
        for (std::size_t k = 0; k < run.size(); ++k) {
            x += step(run, k, st);
        }
        return x;
    }

    double step(const std::vector<int>& run, std::size_t k, const TextStyle& st) const {
        const int g = run[k];
        double adv = g < 0 ? font_.metrics.unitsPerEm * 0.5 : font_.glyphs[static_cast<std::size_t>(g)].advance;
        adv += st.tracking;
        if (st.kerning && k + 1 < run.size() && g >= 0 && run[k + 1] >= 0)
            adv += font_.kern(font_.glyphs[static_cast<std::size_t>(g)].name, font_.glyphs[static_cast<std::size_t>(run[k + 1])].name);
        return adv;
    }

    std::vector<int> run(const std::string& text) const {
        std::vector<int> out;
        for (auto cp : utf8Decode(text)) {
            if (cp == '\n' || cp == '\r') continue;
            out.push_back(glyphFor(cp));
        }
        return out;
    }

    double draw(Canvas& c, const std::string& text, double x, double baseline, double sizePx, Color color, const TextStyle& st,
                double maxWidth = 0) {
        const auto r = run(text);
        const double k = sizePx / font_.metrics.unitsPerEm;
        double pen = 0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            const double s = step(r, i, st) * k;
            if (maxWidth > 0 && pen + s > maxWidth) break;
            const Affine m{k, 0, 0, -k, x + pen, baseline};
            if (r[i] >= 0) {
                c.shapes(outlineOf(r[i]), m, color);
            } else {
                // no .notdef: a hollow box
                const double w = font_.metrics.unitsPerEm * 0.4, h = font_.metrics.capHeight;
                Contour outer = rectContour(w * 0.1, 0, w, h), inner = rectContour(w * 0.2, h * 0.05, w * 0.8, h * 0.9);
                reverse(inner);
                c.shapes({outer, inner}, m, color);
            }
            pen += s;
        }
        return pen;
    }

private:
    const Font& font_;
    std::unordered_map<std::uint32_t, int> cmap_;
    int notdef_;
    std::map<int, std::vector<Contour>> cache_;
};

std::vector<std::string> wrapWords(Typesetter& ts, const std::string& text, double sizePx, double maxWidth, const TextStyle& st,
                                   double unitsPerEm) {
    std::vector<std::string> lines;
    std::string para;
    auto flushPara = [&](const std::string& p) {
        std::string line, word;
        auto width = [&](const std::string& s) { return ts.advanceUnits(ts.run(s), st) * sizePx / unitsPerEm; };
        std::vector<std::string> words;
        for (char ch : p + " ") {
            if (ch == ' ') {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word += ch;
            }
        }
        for (const auto& w : words) {
            const std::string candidate = line.empty() ? w : line + " " + w;
            if (!line.empty() && maxWidth > 0 && width(candidate) > maxWidth) {
                lines.push_back(line);
                line = w;
            } else {
                line = candidate;
            }
        }
        lines.push_back(line);
    };
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') {
            flushPara(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    flushPara(cur);
    return lines;
}

}  // namespace

void fillContours(Image& image, const std::vector<Contour>& contours, const Affine& toPixels, Color color) {
    std::vector<Contour> cs = contours;
    for (auto& c : cs) {
        for (auto& p : c.points) {
            const Vec q = toPixels.apply({p.x, p.y});
            p.x = q.x;
            p.y = q.y;
        }
    }
    fillPolygons(image, flatten(cs, 0.2), color);
}

std::vector<int> glyphRun(const Font& font, const std::string& text) { return Typesetter(font).run(text); }

double textAdvance(const Font& font, const std::string& text, const TextStyle& style) {
    Typesetter ts(font);
    return ts.advanceUnits(ts.run(text), style);
}

double drawText(Image& image, const Font& font, const std::string& text, double x, double y, const TextStyle& style) {
    Typesetter ts(font);
    ImageCanvas c(image, 0);
    return ts.draw(c, text, x, y, style.size, style.color, style);
}

Image renderText(const Font& font, const std::string& text, const TextStyle& style, Color background, int padding, double maxWidth) {
    Typesetter ts(font);
    const double k = style.size / font.metrics.unitsPerEm;
    const auto lines = wrapWords(ts, text, style.size, maxWidth, style, font.metrics.unitsPerEm);
    const double lh = style.lineHeight > 0 ? style.lineHeight : (font.metrics.ascender - font.metrics.descender + font.metrics.lineGap) * k;
    double w = 1;
    for (const auto& l : lines) w = std::max(w, ts.advanceUnits(ts.run(l), style) * k);
    if (maxWidth > 0) w = std::min(w, maxWidth);
    const int W = std::clamp(static_cast<int>(std::ceil(w)) + 2 * padding, 1, 16384);
    const int H = std::clamp(static_cast<int>(std::ceil(lh * static_cast<double>(lines.size()))) + 2 * padding, 1, 16384);
    Image img(W, H, background);
    ImageCanvas c(img, 0);
    double base = padding + font.metrics.ascender * k + (lh - (font.metrics.ascender - font.metrics.descender + font.metrics.lineGap) * k) / 2;
    for (const auto& l : lines) {
        ts.draw(c, l, padding, base, style.size, style.color, style);
        base += lh;
    }
    return img;
}

Image glyphView(const Font& font, const Glyph& glyph, const GlyphViewOptions& o) {
    const int S = std::clamp(o.size, 64, 4096);
    const auto& m = font.metrics;
    const auto contours = outline(font, glyph);
    Box b = bounds(contours);
    double x0 = std::min(0.0, b.empty ? 0.0 : b.x0), x1 = std::max(glyph.advance, b.empty ? 0.0 : b.x1);
    double y0 = std::min(m.descender, b.empty ? 0.0 : b.y0), y1 = std::max(m.ascender, b.empty ? 0.0 : b.y1);
    if (x1 - x0 < 1) x1 = x0 + m.unitsPerEm * 0.5;
    const double margin = S * 0.08, footer = 28;
    const double k = std::min((S - 2 * margin) / (x1 - x0), (S - 2 * margin - footer) / (y1 - y0));
    const double ox = (S - (x1 - x0) * k) / 2 - x0 * k, oy = margin + y1 * k;
    const Affine toPx{k, 0, 0, -k, ox, oy};
    Image img(S, S, {255, 255, 255, 255});
    auto px = [&](double x, double y) { return toPx.apply({x, y}); };
    const pdf::Font ui{pdf::Family::Sans, false, false};
    if (o.metrics) {
        struct L {
            double y;
            const char* name;
            Color c;
        };
        const L lines[] = {{m.ascender, "ascender", {170, 120, 200, 255}},
                           {m.capHeight, "cap height", {90, 140, 220, 255}},
                           {m.xHeight, "x-height", {60, 170, 150, 255}},
                           {0, "baseline", {60, 60, 60, 255}},
                           {m.descender, "descender", {200, 120, 90, 255}}};
        for (const auto& l : lines) {
            const double y = px(0, l.y).y;
            raster::drawLine(img, 0, y, S, y, {l.c.r, l.c.g, l.c.b, 150}, l.y == 0 ? 1.5 : 1);
            pdf::drawText(img, 4, y - 3, std::string(l.name) + " " + fmt(l.y), ui, 11, l.c);
        }
        const double a = px(0, 0).x, adv = px(glyph.advance, 0).x;
        raster::drawLine(img, a, px(0, y1).y, a, px(0, y0).y, {120, 120, 120, 160}, 1);
        raster::drawLine(img, adv, px(0, y1).y, adv, px(0, y0).y, {120, 120, 120, 160}, 1);
    }
    if (o.fill) fillContours(img, contours, toPx, o.points ? Color{40, 40, 60, 70} : Color{20, 20, 30, 255});
    if (o.points) {
        // outline
        for (const auto& poly : flatten(contours, 0.5 / std::max(k, 1e-6))) {
            for (std::size_t i = 0; i < poly.size(); ++i) {
                const Vec a = toPx.apply(poly[i]), c = toPx.apply(poly[(i + 1) % poly.size()]);
                raster::drawLine(img, a.x, a.y, c.x, c.y, {30, 30, 40, 255}, 1.5);
            }
        }
        for (const auto& c : glyph.contours) {
            const auto& pts = c.points;
            const std::size_t n = pts.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (pts[i].on()) continue;
                // handles to the neighbouring points
                const Point& prev = pts[(i + n - 1) % n];
                const Point& next = pts[(i + 1) % n];
                const Vec p = px(pts[i].x, pts[i].y);
                if (prev.on() || pts[i].type == PointType::Quad) {
                    const Vec q = px(prev.x, prev.y);
                    raster::drawLine(img, p.x, p.y, q.x, q.y, {140, 140, 160, 255}, 1);
                }
                if (next.on() || pts[i].type == PointType::Quad) {
                    const Vec q = px(next.x, next.y);
                    raster::drawLine(img, p.x, p.y, q.x, q.y, {140, 140, 160, 255}, 1);
                }
            }
            for (std::size_t i = 0; i < n; ++i) {
                const Vec p = px(pts[i].x, pts[i].y);
                if (pts[i].on()) {
                    const double r = i == 0 ? 5 : 3.5;
                    raster::fillRect(img, p.x - r, p.y - r, 2 * r, 2 * r, i == 0 ? Color{220, 50, 50, 255} : Color{40, 150, 70, 255});
                } else {
                    raster::fillEllipse(img, p.x, p.y, 3.5, 3.5,
                                        pts[i].type == PointType::Cubic ? Color{50, 100, 220, 255} : Color{230, 140, 30, 255});
                }
            }
        }
        // component origins
        for (const auto& comp : glyph.components) {
            const Vec p = px(comp.dx, comp.dy);
            raster::strokeRect(img, p.x - 5, p.y - 5, 10, 10, {160, 60, 200, 255}, 1.5);
        }
    }
    std::string foot = glyph.name;
    if (!glyph.unicodes.empty()) foot += "  " + unicodeLabel(glyph.unicodes[0]);
    foot += "  advance " + fmt(glyph.advance);
    if (!b.empty) foot += "  lsb " + fmt(b.x0) + "  rsb " + fmt(glyph.advance - b.x1);
    if (!glyph.components.empty()) foot += "  " + std::to_string(glyph.components.size()) + " component(s)";
    pdf::drawText(img, 8, S - 10, foot, ui, 13, {40, 40, 40, 255});
    return img;
}

Image overview(const Font& font, int cell, int columns, int maxGlyphs) {
    cell = std::clamp(cell, 32, 512);
    columns = std::clamp(columns, 1, 64);
    const int n = std::min(static_cast<int>(font.glyphs.size()), std::max(1, maxGlyphs));
    const int rows = std::max(1, (n + columns - 1) / columns);
    const int header = 40, footer = static_cast<int>(font.glyphs.size()) > n ? 24 : 0;
    Image img(columns * cell + 1, header + rows * cell + footer + 1, {255, 255, 255, 255});
    const pdf::Font ui{pdf::Family::Sans, false, false}, bold{pdf::Family::Sans, true, false};
    pdf::drawText(img, 8, 26, font.familyStyle() + "  -  " + std::to_string(font.glyphs.size()) + " glyphs, " +
                                  std::to_string(font.kerning.size()) + " kerning pairs",
                  bold, 16, {30, 30, 30, 255});
    const auto& m = font.metrics;
    const double label = 16, span = m.ascender - std::min(m.descender, 0.0);
    const double k = (cell - label - 10) / std::max(span, 1.0);
    for (int i = 0; i < n; ++i) {
        const Glyph& g = font.glyphs[static_cast<std::size_t>(i)];
        const int cx = (i % columns) * cell, cy = header + (i / columns) * cell;
        raster::strokeRect(img, cx + 0.5, cy + 0.5, cell, cell, {215, 215, 220, 255}, 1);
        const double base = cy + 5 + m.ascender * k;
        const double ox = cx + (cell - g.advance * k) / 2;
        raster::drawLine(img, cx + 2, base, cx + cell - 2, base, {225, 225, 235, 255}, 1);
        fillContours(img, outline(font, g), {k, 0, 0, -k, ox, base}, {20, 20, 30, 255});
        std::string name = g.name;
        while (name.size() > 3 && pdf::textWidthPx(name, ui, 10) > cell - 6) name.pop_back();
        pdf::drawText(img, cx + 4, cy + cell - 4, name, ui, 10, {110, 110, 120, 255});
    }
    if (footer) {
        pdf::drawText(img, 8, header + rows * cell + 17, "... and " + std::to_string(font.glyphs.size() - static_cast<std::size_t>(n)) + " more",
                      ui, 12, {110, 110, 120, 255});
    }
    return img;
}

// ---- proof sheets ----

namespace {

struct Block {
    double height;
    std::function<void(Canvas&, double)> draw;
};

const char* kParagraph =
    "Sphinx of black quartz, judge my vow. Pack my box with five dozen liquor jugs. How vexingly quick daft zebras jump! "
    "The five boxing wizards jump quickly. Jackdaws love my big sphinx of quartz. Waltz, bad nymph, for quick jigs vex.";

// Text for a proof that the font can set: the text itself, else upper- or lower-cased (a caps-only
// font), else words made from the font's own characters (so a font of a few glyphs still proofs).
std::string coveredText(const Font& font, const std::string& text) {
    auto coverage = [&](const std::string& t) {
        std::size_t have = 0, total = 0;
        for (auto cp : utf8Decode(t)) {
            if (cp <= 0x20) continue;
            ++total;
            if (font.indexForUnicode(cp) >= 0) ++have;
        }
        return total ? static_cast<double>(have) / static_cast<double>(total) : 1.0;
    };
    std::string upper = text, lower = text;
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string best = text;
    double score = coverage(text);
    for (const std::string* t : {&upper, &lower}) {
        const double c = coverage(*t);
        if (c > score + 1e-9) {
            score = c;
            best = *t;
        }
    }
    if (score >= 0.75) return best;
    std::vector<std::uint32_t> cps;
    for (const auto& g : font.glyphs) {
        for (auto u : g.unicodes) {
            if (u > 0x20 && std::find(cps.begin(), cps.end(), u) == cps.end()) cps.push_back(u);
        }
    }
    if (cps.empty()) return best;
    // about as long as the original, in words of 3 to 6 characters taken in a stepping order
    const std::size_t want = std::max<std::size_t>(utf8Decode(text).size(), 12);
    std::string out;
    std::size_t k = 0, count = 0, word = 0, wordLen = 4;
    while (count < want) {
        out += utf8Encode(cps[(k * 7 + k / cps.size()) % cps.size()]);
        ++k;
        ++count;
        if (++word == wordLen) {
            out += ' ';
            ++count;
            word = 0;
            wordLen = 3 + (k % 4);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::vector<Block> proofBlocks(const Font& font, const ProofOptions& o, Typesetter& ts, double width) {
    std::vector<Block> blocks;
    const auto& m = font.metrics;
    const double upm = m.unitsPerEm;
    const Color ink{20, 20, 28, 255}, muted{110, 110, 120, 255}, rule{210, 210, 218, 255};
    TextStyle st;
    auto section = [&](const std::string& title) {
        blocks.push_back({34, [=](Canvas& c, double y) {
                              c.label(0, y + 22, title, 12, muted, true);
                              c.line(0, y + 28, width, y + 28, rule, 0.75);
                          }});
    };
    const std::string title = o.title.empty() ? font.familyStyle() : o.title;
    std::string sub = "version " + font.info.version + "  |  " + std::to_string(font.glyphs.size()) + " glyphs  |  " +
                      std::to_string(m.unitsPerEm) + " units per em  |  " + std::to_string(font.kerning.size()) + " kerning pairs";
    if (!font.info.license.empty()) sub += "  |  " + font.info.license;
    if (!font.info.designer.empty()) sub += "  |  " + font.info.designer;
    blocks.push_back({64, [=](Canvas& c, double y) {
                          c.label(0, y + 28, title, 26, ink, true);
                          c.label(0, y + 50, sub, 11, muted, false);
                      }});
    // a large sample of the font's own name
    const std::string name = coveredText(font, font.info.family);
    blocks.push_back({110, [&ts, name, width, ink, st](Canvas& c, double y) {
                          ts.draw(c, name, 0, y + 84, 84, ink, st, width);
                      }});
    if (o.charset) {
        std::vector<std::uint32_t> cps;
        for (const auto& g : font.glyphs) {
            for (auto u : g.unicodes) {
                if (u > 0x20) cps.push_back(u);
            }
        }
        std::sort(cps.begin(), cps.end());
        cps.erase(std::unique(cps.begin(), cps.end()), cps.end());
        if (!cps.empty()) {
            section("CHARACTER SET (" + std::to_string(cps.size()) + ")");
            const double size = 32, k = size / upm, gapPx = size * 0.35;
            std::vector<std::string> line;
            double w = 0;
            std::vector<std::vector<std::string>> lines;
            for (auto cp : cps) {
                const std::string s = utf8Encode(cp);
                const double a = ts.advanceUnits(ts.run(s), st) * k + gapPx;
                if (w + a > width && !line.empty()) {
                    lines.push_back(line);
                    line.clear();
                    w = 0;
                }
                line.push_back(s);
                w += a;
            }
            if (!line.empty()) lines.push_back(line);
            for (const auto& l : lines) {
                blocks.push_back({size * 1.45, [&ts, l, size, gapPx, ink, st](Canvas& c, double y) {
                                      double x = 0;
                                      for (const auto& s : l) x += ts.draw(c, s, x, y + size * 1.05, size, ink, st) + gapPx;
                                  }});
            }
        }
    }
    if (!o.sizes.empty() && !o.sample.empty()) {
        section("SIZES");
        const std::string sample = o.sample == ProofOptions{}.sample ? coveredText(font, o.sample) : o.sample;
        for (double size : o.sizes) {
            if (size <= 0) continue;
            const double lh = std::max(size * 1.3, 18.0);
            blocks.push_back({lh, [&ts, size, sample, width, ink, muted, st, lh](Canvas& c, double y) {
                                  c.label(0, y + lh * 0.75, fmt(size), 10, muted, false);
                                  ts.draw(c, sample, 34, y + lh * 0.77, size, ink, st, width - 34);
                              }});
        }
    }
    const std::string para = o.paragraph.empty() ? coveredText(font, kParagraph) : o.paragraph;
    if (!para.empty()) {
        for (double size : {18.0, 12.0}) {
            section("TEXT " + fmt(size));
            for (const auto& l : wrapWords(ts, para, size, width, st, upm)) {
                blocks.push_back({size * 1.45, [&ts, l, size, ink, st](Canvas& c, double y) { ts.draw(c, l, 0, y + size * 1.1, size, ink, st); }});
            }
        }
    }
    if (o.kerning && !font.kerning.empty()) {
        std::vector<std::pair<double, KernPair>> strongest;
        for (const auto& [p, v] : font.kerning) {
            const Glyph* a = font.glyph(p.first);
            const Glyph* b = font.glyph(p.second);
            if (a && b && !a->unicodes.empty() && !b->unicodes.empty()) strongest.push_back({-std::abs(v), p});
        }
        std::sort(strongest.begin(), strongest.end());
        if (!strongest.empty()) {
            section("KERNING: STRONGEST PAIRS, KERNED (TOP) AND UNKERNED (BOTTOM)");
            std::vector<std::string> pairs;
            for (std::size_t i = 0; i < strongest.size() && i < 24; ++i) {
                const auto& p = strongest[i].second;
                pairs.push_back(utf8Encode(font.glyph(p.first)->unicodes[0]) + utf8Encode(font.glyph(p.second)->unicodes[0]));
            }
            const double size = 44;
            for (bool kerned : {true, false}) {
                TextStyle s2 = st;
                s2.kerning = kerned;
                blocks.push_back({size * 1.35, [&ts, pairs, size, ink, s2, width](Canvas& c, double y) {
                                      double x = 0;
                                      for (const auto& p : pairs) {
                                          const double w = ts.draw(c, p, x, y + size, size, ink, s2, width - x);
                                          x += w + size * 0.45;
                                          if (x > width) break;
                                      }
                                  }});
            }
        }
    }
    if (o.grid) {
        section("GLYPHS (" + std::to_string(font.glyphs.size()) + ")");
        const int cols = std::max(1, static_cast<int>(width / 86));
        const double cell = width / cols;
        const double span = m.ascender - std::min(m.descender, 0.0);
        const double k = (cell - 26) / std::max(span, 1.0);
        for (std::size_t r = 0; r * static_cast<std::size_t>(cols) < font.glyphs.size(); ++r) {
            blocks.push_back({cell, [&ts, &font, r, cols, cell, k, rule, ink, muted](Canvas& c, double y) {
                                  const auto& mm = font.metrics;
                                  for (int i = 0; i < cols; ++i) {
                                      const std::size_t gi = r * static_cast<std::size_t>(cols) + static_cast<std::size_t>(i);
                                      if (gi >= font.glyphs.size()) break;
                                      const Glyph& g = font.glyphs[gi];
                                      const double x = i * cell;
                                      c.line(x, y, x + cell, y, rule, 0.5);
                                      c.line(x, y, x, y + cell, rule, 0.5);
                                      c.line(x + cell, y, x + cell, y + cell, rule, 0.5);
                                      c.line(x, y + cell, x + cell, y + cell, rule, 0.5);
                                      const double base = y + 4 + mm.ascender * k;
                                      c.line(x + 3, base, x + cell - 3, base, {230, 200, 200, 255}, 0.5);
                                      const double ox = x + (cell - g.advance * k) / 2;
                                      c.shapes(ts.outlineOf(static_cast<int>(gi)), {k, 0, 0, -k, ox, base}, ink);
                                      std::string lab = g.name;
                                      while (lab.size() > 3 && c.labelWidth(lab, 7.5, false) > cell - 6) lab.pop_back();
                                      c.label(x + 3, y + cell - 12, lab, 7.5, muted, false);
                                      if (!g.unicodes.empty()) c.label(x + 3, y + cell - 3, unicodeLabel(g.unicodes[0]), 7, muted, false);
                                  }
                              }});
        }
    }
    return blocks;
}

}  // namespace

Image proofImage(const Font& font, const ProofOptions& o) {
    Typesetter ts(font);
    const double margin = 40, width = std::clamp(o.width, 300.0, 6000.0);
    const auto blocks = proofBlocks(font, o, ts, width - 2 * margin);
    double h = 2 * margin;
    for (const auto& b : blocks) h += b.height;
    Image img(static_cast<int>(width), std::min(30000, static_cast<int>(std::ceil(h))), {255, 255, 255, 255});
    // blocks draw at x = 0: shift the whole canvas by the margin
    class Shifted : public Canvas {
    public:
        Shifted(Canvas& c, double dx) : c_(c), dx_(dx) {}
        void label(double x, double y, const std::string& s, double size, Color c, bool bold) override { c_.label(x + dx_, y, s, size, c, bold); }
        void shapes(const std::vector<Contour>& cs, const Affine& m, Color c) override { c_.shapes(cs, Affine::translate(dx_, 0) * m, c); }
        void line(double x1, double y1, double x2, double y2, Color c, double w) override { c_.line(x1 + dx_, y1, x2 + dx_, y2, c, w); }

    private:
        Canvas& c_;
        double dx_;
    };
    ImageCanvas base(img, 0);
    Shifted canvas(base, margin);
    double y = margin;
    for (const auto& b : blocks) {
        if (y > img.height()) break;
        b.draw(canvas, y);
        y += b.height;
    }
    return img;
}

std::string proofPdf(const Font& font, const ProofOptions& o) {
    Typesetter ts(font);
    const double W = 595.28, H = 841.89, margin = 40;
    const auto blocks = proofBlocks(font, o, ts, W - 2 * margin);
    class Shifted : public Canvas {
    public:
        Shifted(PdfCanvas& c, double dx) : c_(c), dx_(dx) {}
        void label(double x, double y, const std::string& s, double size, Color c, bool bold) override { c_.label(x + dx_, y, s, size, c, bold); }
        void shapes(const std::vector<Contour>& cs, const Affine& m, Color c) override { c_.shapes(cs, Affine::translate(dx_, 0) * m, c); }
        void line(double x1, double y1, double x2, double y2, Color c, double w) override { c_.line(x1 + dx_, y1, x2 + dx_, y2, c, w); }

    private:
        PdfCanvas& c_;
        double dx_;
    };
    std::vector<std::string> pages;
    PdfCanvas page(H);
    double y = margin;
    for (const auto& b : blocks) {
        if (y + b.height > H - margin && y > margin) {
            pages.push_back(page.content);
            page.content.clear();
            y = margin;
        }
        Shifted s(page, margin);
        b.draw(s, y);
        y += b.height;
    }
    pages.push_back(page.content);
    pdfdoc::Writer w;
    const int f1 = pdfdoc::addStandardFont(w, pdfdoc::Standard::Helvetica);
    const int f2 = pdfdoc::addStandardFont(w, pdfdoc::Standard::HelveticaBold);
    const int pagesId = w.reserve();
    std::string kids;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        // page numbers in the footer
        std::string content = pages[i];
        PdfCanvas foot(H);
        foot.label(margin, H - 20, font.familyStyle() + " - page " + std::to_string(i + 1) + " of " + std::to_string(pages.size()), 8,
                   {140, 140, 150, 255}, false);
        content += foot.content;
        const int c = w.addStream("", content);
        const int p = w.add("<< /Type /Page /Parent " + std::to_string(pagesId) + " 0 R /MediaBox [0 0 " + pdfdoc::num(W) + " " + pdfdoc::num(H) +
                            "] /Resources << /Font << /F1 " + std::to_string(f1) + " 0 R /F2 " + std::to_string(f2) + " 0 R >> >> /Contents " +
                            std::to_string(c) + " 0 R >>");
        kids += std::to_string(p) + " 0 R ";
    }
    w.set(pagesId, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages.size()) + " >>");
    const int catalog = w.add("<< /Type /Catalog /Pages " + std::to_string(pagesId) + " 0 R >>");
    const int info = w.add("<< /Title " + pdfdoc::text(font.familyStyle() + " proof") + " /Producer (wetwaresuite type) >>");
    return w.finish(catalog, info);
}

}  // namespace wetwaresuite::typeface
