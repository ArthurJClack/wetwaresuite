// Titles, lower thirds and captions drawn with the suite's own font code
// (TrueType outlines filled with anti-aliasing): several lines, alignment, an
// outline, a soft shadow and a box, the same on every machine.
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "render_internal.hpp"
#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/raster/filters.hpp"
#include "wetwaresuite/raster/paint.hpp"

namespace wetwaresuite::cut {

using raster::Color;
using raster::Image;

namespace {

std::shared_ptr<pdfdoc::TrueType> fontFor(const std::string& name, bool bold, bool italic) {
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<pdfdoc::TrueType>> cache;
    const std::string key = name + (bold ? "|b" : "") + (italic ? "|i" : "");
    std::lock_guard<std::mutex> guard(lock);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::shared_ptr<pdfdoc::TrueType> font;
    if (!name.empty() && name != "sans" && name != "default") {
        std::error_code ec;
        if (std::filesystem::is_regular_file(name, ec)) {
            try {
                font = pdfdoc::TrueType::load(name);
            } catch (const std::exception&) {
            }
        } else {
            font = pdfdoc::systemFont(name, bold, italic);
        }
    } else if (bold || italic) {
        font = pdfdoc::systemFont("sans", bold, italic);
    }
    if (!font) font = pdfdoc::builtinSans();
    cache[key] = font;
    return font;
}

std::vector<std::vector<unsigned>> splitLines(const std::string& raw) {
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
    return lines;
}

double lineWidth(const pdfdoc::TrueType& f, const std::vector<unsigned>& line, double size) {
    double w = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        w += f.advance(line[i]) * size / 1000;
        if (i + 1 < line.size()) w += f.kern(line[i], line[i + 1]) * size / 1000;
    }
    return w;
}

struct Line {
    std::vector<unsigned> chars;
    double size = 0;
    Color color;
    double width = 0;
};

// Felzenszwalb's 1D squared distance transform.
void dt1d(const std::vector<float>& f, std::vector<float>& d, std::vector<int>& v, std::vector<float>& z, int n) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;
    for (int q = 1; q < n; ++q) {
        float s;
        for (;;) {
            const int p = v[static_cast<std::size_t>(k)];
            s = ((f[static_cast<std::size_t>(q)] + static_cast<float>(q * q)) - (f[static_cast<std::size_t>(p)] + static_cast<float>(p * p))) /
                static_cast<float>(2 * q - 2 * p);
            if (s <= z[static_cast<std::size_t>(k)] && k > 0) {
                --k;
                continue;
            }
            break;
        }
        ++k;
        v[static_cast<std::size_t>(k)] = q;
        z[static_cast<std::size_t>(k)] = s;
        z[static_cast<std::size_t>(k) + 1] = 1e20f;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[static_cast<std::size_t>(k) + 1] < static_cast<float>(q)) ++k;
        const int p = v[static_cast<std::size_t>(k)];
        d[static_cast<std::size_t>(q)] = static_cast<float>((q - p) * (q - p)) + f[static_cast<std::size_t>(p)];
    }
}

// Distance (px) from each pixel to the nearest pixel with coverage >= 0.5.
std::vector<float> distanceToInk(const std::vector<float>& cover, int w, int h) {
    const float inf = 1e10f;
    std::vector<float> g(cover.size());
    for (std::size_t i = 0; i < cover.size(); ++i) g[i] = cover[i] >= 0.5f ? 0.0f : inf;
    const int n = std::max(w, h);
    std::vector<float> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[static_cast<std::size_t>(y)] = g[static_cast<std::size_t>(y) * w + x];
        dt1d(f, d, v, z, h);
        for (int y = 0; y < h; ++y) g[static_cast<std::size_t>(y) * w + x] = d[static_cast<std::size_t>(y)];
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) f[static_cast<std::size_t>(x)] = g[static_cast<std::size_t>(y) * w + x];
        dt1d(f, d, v, z, w);
        for (int x = 0; x < w; ++x) g[static_cast<std::size_t>(y) * w + x] = std::sqrt(d[static_cast<std::size_t>(x)]);
    }
    return g;
}

// Paints `color` with per-pixel coverage onto img (straight alpha, "over").
void paintCoverage(Image& img, const std::vector<float>& cover, Color color, int ox = 0, int oy = 0) {
    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; ++y) {
        const int sy = y - oy;
        if (sy < 0 || sy >= h) continue;
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox;
            if (sx < 0 || sx >= w) continue;
            const float a = cover[static_cast<std::size_t>(sy) * w + sx] * (color.a / 255.0f);
            if (a <= 0) continue;
            std::uint8_t* p = img.pixel(x, y);
            const float da = p[3] / 255.0f;
            const float oa = a + da * (1 - a);
            for (int k = 0; k < 3; ++k) {
                const float s = (k == 0 ? color.r : k == 1 ? color.g : color.b) / 255.0f;
                const float dv = p[k] / 255.0f;
                p[k] = static_cast<std::uint8_t>(std::lround(std::clamp((s * a + dv * da * (1 - a)) / std::max(oa, 1e-6f), 0.0f, 1.0f) * 255));
            }
            p[3] = static_cast<std::uint8_t>(std::lround(std::clamp(oa, 0.0f, 1.0f) * 255));
        }
    }
}

}  // namespace

std::size_t titleLength(const Clip& c) {
    std::size_t n = 0;
    for (const auto& l : splitLines(c.text)) n += l.size();
    if (c.style == "lower_third") {
        for (const auto& l : splitLines(c.subtitle)) n += l.size();
    }
    return n;
}

TitleImage drawTitle(const Clip& c, const Settings& settings, double scale, int chars) {
    TitleImage out;
    const auto font = fontFor(c.font, c.bold, c.italic);
    const pdfdoc::VMetrics vm = font->metrics();
    const bool lower = c.style == "lower_third";
    const Color textColour = colourOr(c.color, {255, 255, 255, 255});

    // lines in timeline px
    std::vector<Line> lines;
    int left = chars < 0 ? 1 << 30 : chars;
    auto addLines = [&](const std::string& text, double size, Color colour) {
        for (auto& l : splitLines(text)) {
            Line line;
            const int take = std::clamp(left, 0, static_cast<int>(l.size()));
            line.chars.assign(l.begin(), l.begin() + take);
            left -= static_cast<int>(l.size());
            line.size = size;
            line.color = colour;
            line.width = lineWidth(*font, l, size);  // full width, so the typewriter doesn't shift the text
            lines.push_back(line);
        }
    };
    addLines(c.text, c.fontSize, textColour);
    if (lower && !c.subtitle.empty()) {
        Color sub = textColour;
        sub.a = static_cast<std::uint8_t>(sub.a * 0.85);
        addLines(c.subtitle, c.fontSize * 0.6, sub);
    }
    double blockW = 0, blockH = 0;
    for (const auto& l : lines) {
        blockW = std::max(blockW, l.width);
        blockH += l.size * 1.2;
    }
    const double W = settings.width, H = settings.height;
    // where the block sits
    double bx = (W - blockW) / 2, by = (H - blockH) / 2;
    const std::string& p = c.position;
    if (p == "top") by = H * 0.08;
    else if (p == "bottom") by = H * 0.92 - blockH;
    else if (p == "lower-third") bx = W * 0.06, by = H * 0.72;
    else if (p == "top-left") bx = W * 0.05, by = H * 0.06;
    else if (p == "top-right") bx = W * 0.95 - blockW, by = H * 0.06;
    else if (p == "bottom-left") bx = W * 0.05, by = H * 0.94 - blockH;
    else if (p == "bottom-right") bx = W * 0.95 - blockW, by = H * 0.94 - blockH;
    else if (p == "custom") bx = c.x, by = c.y;

    const double pad = c.box || lower ? c.fontSize * 0.3 : 0;
    const double accent = lower ? c.fontSize * 0.14 : 0;
    const double reach = c.outline + c.shadow * 1.6 + 2;
    out.x = bx - pad - accent - reach;
    out.y = by - pad - reach;
    out.w = blockW + 2 * pad + accent + 2 * reach;
    out.h = blockH + 2 * pad + 2 * reach;
    const int iw = std::max(1, static_cast<int>(std::ceil(out.w * scale)));
    const int ih = std::max(1, static_cast<int>(std::ceil(out.h * scale)));
    out.image = Image(iw, ih);
    auto X = [&](double tx) { return (tx - out.x) * scale; };
    auto Y = [&](double ty) { return (ty - out.y) * scale; };

    if (c.box || lower) {
        const Color boxColour = colourOr(c.boxColor, {0, 0, 0, 128});
        raster::fillRect(out.image, X(bx - pad), Y(by - pad), (blockW + 2 * pad) * scale, (blockH + 2 * pad) * scale, boxColour);
    }
    if (lower) {
        raster::fillRect(out.image, X(bx - pad - accent), Y(by - pad), accent * scale, (blockH + 2 * pad) * scale,
                         colourOr(c.accentColor, {232, 163, 61, 255}));
    }

    // glyph outlines per colour (titles have one colour, lower thirds two)
    std::vector<std::pair<Color, std::vector<std::vector<raster::Point>>>> groups;
    double top = by;
    for (const auto& l : lines) {
        const double baseline = top + l.size * 0.1 + vm.ascent * l.size / 1000;
        double pen = bx;
        if (c.align == "center") pen = bx + (blockW - l.width) / 2;
        else if (c.align == "right") pen = bx + blockW - l.width;
        if (groups.empty() || !(groups.back().first == l.color)) groups.push_back({l.color, {}});
        for (std::size_t i = 0; i < l.chars.size(); ++i) {
            for (const auto& contour : font->outline(l.chars[i])) {
                std::vector<raster::Point> pts;
                pts.reserve(contour.size());
                for (const auto& q : contour) pts.push_back({X(pen + q.x * l.size / 1000), Y(baseline - q.y * l.size / 1000)});
                groups.back().second.push_back(std::move(pts));
            }
            pen += font->advance(l.chars[i]) * l.size / 1000;
            if (i + 1 < l.chars.size()) pen += font->kern(l.chars[i], l.chars[i + 1]) * l.size / 1000;
        }
        top += l.size * 1.2;
    }
    std::vector<float> all(static_cast<std::size_t>(iw) * ih, 0.0f);
    std::vector<std::vector<float>> covers;
    for (const auto& g : groups) {
        covers.push_back(raster::polygonCoverage(iw, ih, g.second, true));
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = std::max(all[i], covers.back()[i]);
    }
    std::vector<float> ink = all;
    if (c.outline > 0) {
        const auto dist = distanceToInk(all, iw, ih);
        const float r = static_cast<float>(c.outline * scale);
        for (std::size_t i = 0; i < ink.size(); ++i) ink[i] = std::max(all[i], std::clamp(r + 0.5f - dist[i], 0.0f, 1.0f));
    }
    if (c.shadow > 0) {
        // the shadow of the text and its outline, softened and offset down-right
        Image sh(iw, ih);
        for (int y = 0; y < ih; ++y) {
            for (int x = 0; x < iw; ++x) sh.pixel(x, y)[3] = static_cast<std::uint8_t>(std::lround(ink[static_cast<std::size_t>(y) * iw + x] * 255));
        }
        raster::gaussianBlur(sh, std::max(0.5, c.shadow * scale * 0.5));
        std::vector<float> cover(ink.size());
        for (int y = 0; y < ih; ++y) {
            for (int x = 0; x < iw; ++x) cover[static_cast<std::size_t>(y) * iw + x] = sh.pixel(x, y)[3] / 255.0f;
        }
        const int off = static_cast<int>(std::lround(c.shadow * scale));
        paintCoverage(out.image, cover, colourOr(c.shadowColor, {0, 0, 0, 153}), off, off);
    }
    if (c.outline > 0) {
        std::vector<float> ring(ink.size());
        for (std::size_t i = 0; i < ink.size(); ++i) ring[i] = ink[i];
        paintCoverage(out.image, ring, colourOr(c.outlineColor, {0, 0, 0, 255}));
    }
    for (std::size_t g = 0; g < groups.size(); ++g) paintCoverage(out.image, covers[g], groups[g].first);
    return out;
}

}  // namespace wetwaresuite::cut
