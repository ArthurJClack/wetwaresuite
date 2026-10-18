#include "achclwss/forge/texture.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

namespace achclwss::forge {

namespace {

std::uint8_t clamp8(double v) { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

int wrapIndex(int v, int n) {
    v %= n;
    return v < 0 ? v + n : v;
}

double luma(const std::uint8_t* p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }

}  // namespace

std::vector<std::uint8_t> channel(const Image& img, HeightSource which) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height()));
    const std::uint8_t* p = img.data();
    for (std::size_t i = 0; i < out.size(); ++i, p += 4) {
        switch (which) {
            case HeightSource::Luma: out[i] = clamp8(luma(p)); break;
            case HeightSource::Red: out[i] = p[0]; break;
            case HeightSource::Green: out[i] = p[1]; break;
            case HeightSource::Blue: out[i] = p[2]; break;
            case HeightSource::Alpha: out[i] = p[3]; break;
        }
    }
    return out;
}

// ---- normal maps ----

Image normalMap(const Image& height, const NormalMapOptions& o) {
    const int w = height.width(), h = height.height();
    Image out(w, h, {128, 128, 255, 255});
    if (height.empty()) return out;
    const auto ch = channel(height, o.source);
    auto at = [&](int x, int y) {
        if (o.wrap) {
            x = wrapIndex(x, w);
            y = wrapIndex(y, h);
        } else {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
        }
        return ch[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] / 255.0;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double gx = (at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1));
            const double gy = (at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1));
            // gy is the slope going down the image; OpenGL's +Y points up the image.
            double nx = -gx * o.strength, ny = (o.invertY ? -gy : gy) * o.strength, nz = 1.0;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len;
            ny /= len;
            nz /= len;
            out.set(x, y, {clamp8((nx + 1) * 127.5), clamp8((ny + 1) * 127.5), clamp8((nz + 1) * 127.5), 255});
        }
    }
    return out;
}

// ---- tiling ----

Image makeSeamless(const Image& img, double blend) {
    const int w = img.width(), h = img.height();
    if (img.empty()) return img;
    blend = std::clamp(blend, 0.02, 0.5);
    auto ramp = [](int i, int n, double fraction) {
        // 0 on the edge pixels, rising smoothly to 1 at `fraction` of the size in
        const double span = std::max(1.0, fraction * n);
        const double t = std::clamp(std::min(i, n - 1 - i) / span, 0.0, 1.0);
        return t * t * (3 - 2 * t);
    };
    // Two separable passes: blend with a copy shifted half the width (its
    // edges are the original's continuous middle), then the same vertically.
    // The shifted copy's own seam lands where the original has full weight.
    std::vector<double> buf(img.bytes());
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = img.data()[i];
    std::vector<double> tmp(buf.size());
    auto at = [w](int x, int y) { return (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4; };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double m = ramp(x, w, blend);
            const std::size_t a = at(x, y), b = at((x + w / 2) % w, y);
            for (int c = 0; c < 4; ++c) tmp[a + static_cast<std::size_t>(c)] = buf[a + static_cast<std::size_t>(c)] * m + buf[b + static_cast<std::size_t>(c)] * (1 - m);
        }
    }
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        const double m = ramp(y, h, blend);
        for (int x = 0; x < w; ++x) {
            const std::size_t a = at(x, y), b = at(x, (y + h / 2) % h);
            std::uint8_t* o = out.pixel(x, y);
            for (int c = 0; c < 4; ++c) o[c] = clamp8(tmp[a + static_cast<std::size_t>(c)] * m + tmp[b + static_cast<std::size_t>(c)] * (1 - m));
        }
    }
    return out;
}

Image tile(const Image& img, int tilesX, int tilesY) {
    tilesX = std::max(1, tilesX);
    tilesY = std::max(1, tilesY);
    Image out(img.width() * tilesX, img.height() * tilesY);
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) blit(out, img, tx * img.width(), ty * img.height());
    }
    return out;
}

double seamError(const Image& img) {
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2) return 0;
    double sum = 0;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t *a = img.pixel(0, y), *b = img.pixel(w - 1, y);
        for (int c = 0; c < 3; ++c) sum += std::abs(a[c] - b[c]);
    }
    for (int x = 0; x < w; ++x) {
        const std::uint8_t *a = img.pixel(x, 0), *b = img.pixel(x, h - 1);
        for (int c = 0; c < 3; ++c) sum += std::abs(a[c] - b[c]);
    }
    return sum / (3.0 * (w + h));
}

// ---- pixel art ----

namespace {

using Rgb = std::array<std::uint8_t, 3>;

std::vector<Rgb> visiblePixels(const Image& img) {
    std::vector<Rgb> px;
    px.reserve(static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height()));
    const std::uint8_t* p = img.data();
    for (std::size_t i = 0, n = img.bytes() / 4; i < n; ++i, p += 4) {
        if (p[3] > 0) px.push_back({p[0], p[1], p[2]});
    }
    return px;
}

std::uint32_t key(const Rgb& c) { return (static_cast<std::uint32_t>(c[0]) << 16) | (static_cast<std::uint32_t>(c[1]) << 8) | c[2]; }

int nearest(const std::vector<Color>& palette, double r, double g, double b) {
    int best = 0;
    double bestD = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const double dr = r - palette[i].r, dg = g - palette[i].g, db = b - palette[i].b;
        const double d = 0.30 * dr * dr + 0.59 * dg * dg + 0.11 * db * db;  // roughly perceptual
        if (d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::vector<Color> medianCut(std::vector<Rgb> px, int colors) {
    struct Box {
        std::size_t begin, end;
    };
    auto range = [&](const Box& b, int& axis) {
        std::array<int, 3> lo{255, 255, 255}, hi{0, 0, 0};
        for (std::size_t i = b.begin; i < b.end; ++i) {
            for (int c = 0; c < 3; ++c) {
                lo[static_cast<std::size_t>(c)] = std::min<int>(lo[static_cast<std::size_t>(c)], px[i][static_cast<std::size_t>(c)]);
                hi[static_cast<std::size_t>(c)] = std::max<int>(hi[static_cast<std::size_t>(c)], px[i][static_cast<std::size_t>(c)]);
            }
        }
        int best = -1;
        axis = 0;
        for (int c = 0; c < 3; ++c) {
            const int r = hi[static_cast<std::size_t>(c)] - lo[static_cast<std::size_t>(c)];
            if (r > best) {
                best = r;
                axis = c;
            }
        }
        return best;
    };
    std::vector<Box> boxes{{0, px.size()}};
    while (static_cast<int>(boxes.size()) < colors) {
        // split the box with the widest colour range (weighted a little by population)
        int pick = -1, axis = 0;
        double score = 0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            int a = 0;
            const int r = range(boxes[i], a);
            const double s = r * std::sqrt(static_cast<double>(boxes[i].end - boxes[i].begin));
            if (r > 0 && boxes[i].end - boxes[i].begin > 1 && s > score) {
                score = s;
                pick = static_cast<int>(i);
                axis = a;
            }
        }
        if (pick < 0) break;
        Box b = boxes[static_cast<std::size_t>(pick)];
        const auto ax = static_cast<std::size_t>(axis);
        std::sort(px.begin() + static_cast<std::ptrdiff_t>(b.begin), px.begin() + static_cast<std::ptrdiff_t>(b.end),
                  [ax](const Rgb& l, const Rgb& r) { return l[ax] < r[ax]; });
        std::size_t mid = b.begin + (b.end - b.begin) / 2;
        // keep equal values together so a split always separates different colours
        const auto v = px[mid][ax];
        std::size_t m = mid;
        while (m > b.begin && px[m - 1][ax] == v) --m;
        if (m == b.begin) {
            m = mid;
            while (m < b.end && px[m][ax] == v) ++m;
        }
        boxes[static_cast<std::size_t>(pick)] = {b.begin, m};
        boxes.push_back({m, b.end});
    }
    std::vector<Color> out;
    for (const auto& b : boxes) {
        double s[3] = {0, 0, 0};
        for (std::size_t i = b.begin; i < b.end; ++i) {
            for (int c = 0; c < 3; ++c) s[c] += px[i][static_cast<std::size_t>(c)];
        }
        const double n = static_cast<double>(b.end - b.begin);
        out.push_back({clamp8(s[0] / n), clamp8(s[1] / n), clamp8(s[2] / n), 255});
    }
    return out;
}

}  // namespace

std::vector<Color> buildPalette(const Image& img, int colors, QuantizeMethod method) {
    colors = std::clamp(colors, 1, 256);
    std::vector<Rgb> px = visiblePixels(img);
    if (px.empty()) return {};
    std::set<std::uint32_t> distinct;
    for (const auto& c : px) {
        distinct.insert(key(c));
        if (static_cast<int>(distinct.size()) > colors) break;
    }
    if (static_cast<int>(distinct.size()) <= colors) {
        std::vector<Color> out;
        for (auto k : distinct) out.push_back({static_cast<std::uint8_t>(k >> 16), static_cast<std::uint8_t>(k >> 8), static_cast<std::uint8_t>(k), 255});
        return out;
    }
    std::vector<Color> palette = medianCut(px, colors);
    if (method == QuantizeMethod::KMeans) {
        // refine the median-cut palette with a few Lloyd iterations on a sample
        const std::size_t stride = std::max<std::size_t>(1, px.size() / 100000);
        for (int iter = 0; iter < 10; ++iter) {
            std::vector<std::array<double, 4>> sums(palette.size(), {0, 0, 0, 0});
            for (std::size_t i = 0; i < px.size(); i += stride) {
                const auto& c = px[i];
                auto& s = sums[static_cast<std::size_t>(nearest(palette, c[0], c[1], c[2]))];
                s[0] += c[0];
                s[1] += c[1];
                s[2] += c[2];
                s[3] += 1;
            }
            bool moved = false;
            for (std::size_t k = 0; k < palette.size(); ++k) {
                if (sums[k][3] == 0) continue;
                Color n{clamp8(sums[k][0] / sums[k][3]), clamp8(sums[k][1] / sums[k][3]), clamp8(sums[k][2] / sums[k][3]), 255};
                moved = moved || !(n == palette[k]);
                palette[k] = n;
            }
            if (!moved) break;
        }
    }
    return palette;
}

void applyPalette(Image& img, const std::vector<Color>& palette, bool dither) {
    if (palette.empty()) return;
    const int w = img.width(), h = img.height();
    if (!dither) {
        std::uint8_t* p = img.data();
        for (std::size_t i = 0, n = img.bytes() / 4; i < n; ++i, p += 4) {
            if (p[3] == 0) continue;
            const Color& c = palette[static_cast<std::size_t>(nearest(palette, p[0], p[1], p[2]))];
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
        }
        return;
    }
    // Floyd-Steinberg on a float copy; errors only spread between visible pixels
    std::vector<double> buf(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
    for (std::size_t i = 0, n = buf.size() / 3; i < n; ++i) {
        for (int c = 0; c < 3; ++c) buf[i * 3 + static_cast<std::size_t>(c)] = img.data()[i * 4 + static_cast<std::size_t>(c)];
    }
    auto spread = [&](int x, int y, const double* err, double f) {
        if (x < 0 || x >= w || y >= h || img.pixel(x, y)[3] == 0) return;
        double* b = &buf[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
        for (int c = 0; c < 3; ++c) b[c] += err[c] * f;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = img.pixel(x, y);
            if (p[3] == 0) continue;
            double* b = &buf[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 3];
            const Color& c = palette[static_cast<std::size_t>(nearest(palette, b[0], b[1], b[2]))];
            const double err[3] = {b[0] - c.r, b[1] - c.g, b[2] - c.b};
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            spread(x + 1, y, err, 7.0 / 16);
            spread(x - 1, y + 1, err, 3.0 / 16);
            spread(x, y + 1, err, 5.0 / 16);
            spread(x + 1, y + 1, err, 1.0 / 16);
        }
    }
}

std::size_t countColors(const Image& img) {
    std::set<std::uint32_t> distinct;
    for (const auto& c : visiblePixels(img)) distinct.insert(key(c));
    return distinct.size();
}

Image upscale(const Image& img, int factor) {
    factor = std::max(1, factor);
    Image out(img.width() * factor, img.height() * factor);
    for (int y = 0; y < out.height(); ++y) {
        for (int x = 0; x < out.width(); ++x) {
            const std::uint8_t* s = img.pixel(x / factor, y / factor);
            std::copy(s, s + 4, out.pixel(x, y));
        }
    }
    return out;
}

Image outline(const Image& src, Color color, int thickness, bool diagonal, int threshold, bool expand) {
    thickness = std::max(1, thickness);
    Image img = src;
    if (expand) {
        img = Image(src.width() + 2 * thickness, src.height() + 2 * thickness);
        blit(img, src, thickness, thickness);
    }
    const int w = img.width(), h = img.height();
    std::vector<std::uint8_t> solid(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) solid[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = img.pixel(x, y)[3] >= threshold;
    }
    std::vector<std::uint8_t> grown = solid;
    for (int pass = 0; pass < thickness; ++pass) {
        std::vector<std::uint8_t> next = grown;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                auto idx = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
                if (grown[idx]) continue;
                bool hit = false;
                for (int dy = -1; dy <= 1 && !hit; ++dy) {
                    for (int dx = -1; dx <= 1 && !hit; ++dx) {
                        if ((dx == 0 && dy == 0) || (!diagonal && dx != 0 && dy != 0)) continue;
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        hit = grown[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx)] != 0;
                    }
                }
                if (hit) next[idx] = 1;
            }
        }
        grown = std::move(next);
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto idx = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            if (grown[idx] && !solid[idx]) img.set(x, y, color);
        }
    }
    return img;
}

// ---- engine prep ----

std::int64_t nextPowerOfTwo(std::int64_t v) {
    std::int64_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

std::int64_t nearestPowerOfTwo(std::int64_t v) {
    const std::int64_t up = nextPowerOfTwo(v), down = std::max<std::int64_t>(1, up / 2);
    return (v - down < up - v) ? down : up;
}

Rect opaqueBounds(const Image& img, int threshold) {
    int x0 = img.width(), y0 = img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (img.pixel(x, y)[3] > threshold) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// ---- packing ----

namespace {

PackResult shelf(const std::vector<PackItem>& items, const std::vector<std::size_t>& order, int padding, int rowWidth) {
    PackResult r;
    r.rects.resize(items.size());
    int x = padding, y = padding, rowH = 0, used = 0;
    for (auto i : order) {
        const auto& it = items[i];
        if (x > padding && x + it.w + padding > rowWidth) {
            y += rowH + padding;
            x = padding;
            rowH = 0;
        }
        r.rects[i] = {x, y, it.w, it.h};
        x += it.w + padding;
        used = std::max(used, x);
        rowH = std::max(rowH, it.h);
    }
    r.width = std::max(used, 1);
    r.height = std::max(y + rowH + padding, 1);
    return r;
}

}  // namespace

PackResult packShelf(const std::vector<PackItem>& items, int padding, bool powerOfTwo, int maxWidth) {
    if (items.empty()) return {};
    padding = std::max(0, padding);
    std::vector<std::size_t> order(items.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return items[a].h != items[b].h ? items[a].h > items[b].h : items[a].w > items[b].w;
    });
    int widest = 0;
    double area = 0;
    for (const auto& it : items) {
        widest = std::max(widest, it.w);
        area += static_cast<double>(it.w + padding) * (it.h + padding);
    }
    const int minW = widest + 2 * padding;
    std::vector<int> candidates;
    if (maxWidth > 0) {
        candidates.push_back(std::max(minW, maxWidth));
    } else if (powerOfTwo) {
        for (auto w = nextPowerOfTwo(minW); w <= 16384; w *= 2) candidates.push_back(static_cast<int>(w));
    } else {
        const double side = std::sqrt(area);
        for (double f = 0.6; f <= 1.81; f += 0.1) candidates.push_back(std::max(minW, static_cast<int>(std::ceil(side * f)) + padding));
    }
    PackResult best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (int w : candidates) {
        PackResult r = shelf(items, order, padding, w);
        if (powerOfTwo) {
            r.width = static_cast<int>(nextPowerOfTwo(r.width));
            r.height = static_cast<int>(nextPowerOfTwo(r.height));
        }
        // smallest sheet, then wide rather than tall, then the squarest
        const double score = static_cast<double>(r.width) * r.height + (r.height > r.width ? 0.5 : 0.0) + std::abs(r.width - r.height) * 1e-3;
        if (score < bestScore) {
            bestScore = score;
            best = std::move(r);
        }
    }
    return best;
}

PackResult packGrid(const std::vector<PackItem>& items, int columns, int padding, bool powerOfTwo) {
    PackResult r;
    if (items.empty()) return r;
    padding = std::max(0, padding);
    int cw = 1, ch = 1;
    for (const auto& it : items) {
        cw = std::max(cw, it.w);
        ch = std::max(ch, it.h);
    }
    const int n = static_cast<int>(items.size());
    int cols = columns > 0 ? std::min(columns, n) : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
    const int rows = (n + cols - 1) / cols;
    for (int i = 0; i < n; ++i) r.rects.push_back({padding + (i % cols) * (cw + padding), padding + (i / cols) * (ch + padding), cw, ch});
    r.width = padding + cols * (cw + padding);
    r.height = padding + rows * (ch + padding);
    if (powerOfTwo) {
        r.width = static_cast<int>(nextPowerOfTwo(r.width));
        r.height = static_cast<int>(nextPowerOfTwo(r.height));
    }
    return r;
}

void blit(Image& dst, const Image& src, int x, int y) {
    for (int sy = 0; sy < src.height(); ++sy) {
        const int dy = y + sy;
        if (dy < 0 || dy >= dst.height()) continue;
        for (int sx = 0; sx < src.width(); ++sx) {
            const int dx = x + sx;
            if (dx < 0 || dx >= dst.width()) continue;
            const std::uint8_t* s = src.pixel(sx, sy);
            std::copy(s, s + 4, dst.pixel(dx, dy));
        }
    }
}

bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const auto ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb)) {
            std::size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
            std::string na = a.substr(i, ei - i), nb = b.substr(j, ej - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb) return la < lb;
        ++i;
        ++j;
    }
    if (a.size() - i != b.size() - j) return a.size() - i < b.size() - j;
    return a < b;
}

}  // namespace achclwss::forge
