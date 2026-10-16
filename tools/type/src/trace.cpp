// Font from handwriting: marching squares over the ink, curve fitting, grid
// slicing of a scanned sheet, and a printable template to draw on.
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "internal.hpp"

namespace wetwaresuite::typeface {

std::vector<Polygon> traceOutlines(const Image& img, const TraceOptions& o) {
    const int W = img.width(), H = img.height();
    std::vector<Polygon> out;
    if (W == 0 || H == 0) return out;
    const int W2 = W + 2, H2 = H + 2;  // a ring of background so every outline closes
    std::vector<float> f(static_cast<std::size_t>(W2) * H2, 0.0f);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const std::uint8_t* p = img.pixel(x, y);
            const double lum = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0, a = p[3] / 255.0;
            const double ink = o.invert ? lum * a : (1 - lum) * a;
            f[static_cast<std::size_t>(y + 1) * W2 + (x + 1)] = static_cast<float>(ink);
        }
    }
    const double t = std::clamp(o.threshold, 0.01, 0.99);
    auto val = [&](int x, int y) { return static_cast<double>(f[static_cast<std::size_t>(y) * W2 + x]); };
    auto in = [&](int x, int y) { return val(x, y) > t; };
    auto hEdge = [&](int x, int y) { return (static_cast<long long>(y) * W2 + x) * 2; };
    auto vEdge = [&](int x, int y) { return (static_cast<long long>(y) * W2 + x) * 2 + 1; };
    std::unordered_map<long long, Vec> points;
    auto pointOf = [&](long long e) {
        auto it = points.find(e);
        if (it != points.end()) return it->second;
        const long long cell = e / 2;
        const int x = static_cast<int>(cell % W2), y = static_cast<int>(cell / W2);
        const bool horizontal = e % 2 == 0;
        const int x2 = horizontal ? x + 1 : x, y2 = horizontal ? y : y + 1;
        const double a = val(x, y), b = val(x2, y2);
        const double k = std::abs(b - a) > 1e-9 ? std::clamp((t - a) / (b - a), 0.0, 1.0) : 0.5;
        const Vec p{x + (x2 - x) * k - 0.5, y + (y2 - y) * k - 0.5};
        points.emplace(e, p);
        return p;
    };
    std::vector<std::array<long long, 2>> segs;
    std::unordered_map<long long, std::array<int, 2>> at;  // edge -> up to two segments
    auto addSeg = [&](long long a, long long b) {
        const int id = static_cast<int>(segs.size());
        segs.push_back({a, b});
        for (long long e : {a, b}) {
            auto& slot = at.try_emplace(e, std::array<int, 2>{-1, -1}).first->second;
            if (slot[0] < 0) slot[0] = id;
            else slot[1] = id;
        }
    };
    for (int y = 0; y + 1 < H2; ++y) {
        for (int x = 0; x + 1 < W2; ++x) {
            const int c = (in(x, y) ? 1 : 0) | (in(x + 1, y) ? 2 : 0) | (in(x + 1, y + 1) ? 4 : 0) | (in(x, y + 1) ? 8 : 0);
            if (c == 0 || c == 15) continue;
            const long long T = hEdge(x, y), B = hEdge(x, y + 1), L = vEdge(x, y), R = vEdge(x + 1, y);
            const bool centre = (val(x, y) + val(x + 1, y) + val(x + 1, y + 1) + val(x, y + 1)) / 4 > t;
            switch (c) {
            case 1: addSeg(L, T); break;
            case 2: addSeg(T, R); break;
            case 3: addSeg(L, R); break;
            case 4: addSeg(R, B); break;
            case 5:
                if (centre) {
                    addSeg(T, R);
                    addSeg(B, L);
                } else {
                    addSeg(L, T);
                    addSeg(R, B);
                }
                break;
            case 6: addSeg(T, B); break;
            case 7: addSeg(L, B); break;
            case 8: addSeg(B, L); break;
            case 9: addSeg(T, B); break;
            case 10:
                if (centre) {
                    addSeg(L, T);
                    addSeg(R, B);
                } else {
                    addSeg(T, R);
                    addSeg(B, L);
                }
                break;
            case 11: addSeg(R, B); break;
            case 12: addSeg(L, R); break;
            case 13: addSeg(T, R); break;
            case 14: addSeg(L, T); break;
            default: break;
            }
        }
    }
    std::vector<bool> used(segs.size(), false);
    for (std::size_t s = 0; s < segs.size(); ++s) {
        if (used[s]) continue;
        Polygon loop;
        const long long start = segs[s][0];
        long long cur = segs[s][1];
        int seg = static_cast<int>(s);
        used[s] = true;
        loop.push_back(pointOf(start));
        bool closed = false;
        for (std::size_t guard = 0; guard < segs.size() + 2; ++guard) {
            if (cur == start) {
                closed = true;
                break;
            }
            loop.push_back(pointOf(cur));
            const auto& slot = at[cur];
            const int next = slot[0] == seg ? slot[1] : slot[0];
            if (next < 0 || used[static_cast<std::size_t>(next)]) break;
            used[static_cast<std::size_t>(next)] = true;
            seg = next;
            cur = segs[static_cast<std::size_t>(next)][0] == cur ? segs[static_cast<std::size_t>(next)][1] : segs[static_cast<std::size_t>(next)][0];
        }
        if (!closed || loop.size() < 3 || std::abs(signedArea(loop)) < o.minArea) continue;
        // one light smoothing pass takes the stair steps off binary scans
        Polygon smooth(loop.size());
        for (std::size_t i = 0, n = loop.size(); i < n; ++i)
            smooth[i] = loop[(i + n - 1) % n] * 0.25 + loop[i] * 0.5 + loop[(i + 1) % n] * 0.25;
        out.push_back(std::move(smooth));
    }
    return out;
}

std::vector<Contour> traceImage(const Image& img, const TraceOptions& o, const Affine& pixelToFont) {
    std::vector<Contour> out;
    for (const auto& poly : traceOutlines(img, o)) {
        Contour c = fitCurves(poly, o.tolerance, o.corner);
        transform(c, pixelToFont);
        out.push_back(std::move(c));
    }
    fixDirections(out);
    return out;
}

namespace {

Image crop(const Image& src, int x0, int y0, int w, int h) {
    Image out(std::max(1, w), std::max(1, h), {255, 255, 255, 0});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (src.contains(sx, sy)) out.set(x, y, src.at(sx, sy));
        }
    }
    return out;
}

}  // namespace

std::vector<TracedCell> traceGrid(const Image& sheet, const std::vector<std::uint32_t>& chars, const GridLayout& g,
                                  const Metrics& m, const TraceOptions& o) {
    std::vector<TracedCell> out;
    const int rows = std::max(1, g.rows), cols = std::max(1, g.cols);
    const double cw = (sheet.width() - 2 * g.marginX) / cols, ch = (sheet.height() - 2 * g.marginY) / rows;
    if (cw < 4 || ch < 4) return out;
    const double span = m.ascender - m.descender;
    const double k = span / ch;
    const double baseFrac = g.baseline >= 0 ? g.baseline : m.ascender / span;
    const double inset = std::clamp(g.inset, 0.0, 0.45);
    for (std::size_t i = 0; i < chars.size() && i < static_cast<std::size_t>(rows * cols); ++i) {
        if (!chars[i]) continue;
        const int r = static_cast<int>(i) / cols, c = static_cast<int>(i) % cols;
        const double x0 = g.marginX + c * cw, y0 = g.marginY + r * ch;
        const int ix = static_cast<int>(std::lround(x0 + cw * inset)), iy = static_cast<int>(std::lround(y0 + ch * inset));
        const int iw = static_cast<int>(std::lround(cw * (1 - 2 * inset))), ih = static_cast<int>(std::lround(ch * (1 - 2 * inset)));
        const Image cell = crop(sheet, ix, iy, iw, ih);
        // crop pixel -> cell pixel -> font units (y up from the baseline)
        const double ox = ix - x0, oy = iy - y0;
        const Affine toFont{k, 0, 0, -k, ox * k, (baseFrac * ch - oy) * k};
        TracedCell t;
        t.codepoint = chars[i];
        t.contours = traceImage(cell, o, toFont);
        out.push_back(std::move(t));
    }
    return out;
}

Image gridTemplate(const Font& font, const std::vector<std::uint32_t>& chars, int cols, int cell) {
    cols = std::clamp(cols, 1, 64);
    cell = std::clamp(cell, 48, 1024);
    const int n = std::max<int>(1, static_cast<int>(chars.size()));
    const int rows = (n + cols - 1) / cols;
    Image img(cols * cell, rows * cell, {255, 255, 255, 255});
    const auto& m = font.metrics;
    const double span = m.ascender - m.descender;
    const pdf::Font ui{pdf::Family::Sans, false, false};
    for (int i = 0; i < rows * cols; ++i) {
        const double x = (i % cols) * cell, y = (i / cols) * cell;
        raster::strokeRect(img, x + 0.5, y + 0.5, cell - 1, cell - 1, {205, 205, 205, 255}, 1);
        auto guide = [&](double v, Color c) {
            const double gy = y + (m.ascender - v) / span * cell;
            raster::drawLine(img, x + cell * 0.08, gy, x + cell * 0.92, gy, c, 1);
        };
        guide(0, {150, 190, 240, 255});
        guide(m.xHeight, {205, 225, 245, 255});
        guide(m.capHeight, {220, 230, 245, 255});
        if (i < static_cast<int>(chars.size()) && chars[static_cast<std::size_t>(i)]) {
            const std::uint32_t cp = chars[static_cast<std::size_t>(i)];
            const std::string label = cp < 0x100 ? utf8Encode(cp) : unicodeLabel(cp);
            pdf::drawText(img, x + 4, y + std::max(10.0, cell * 0.055), label, ui, std::max(8.0, cell * 0.05), {185, 185, 185, 255});
        }
    }
    return img;
}

}  // namespace wetwaresuite::typeface
