#include "wetwaresuite/raster/ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

namespace {

inline std::uint8_t clamp8(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

// Visits pixels in order on one thread: for loops that carry state (histograms, random noise).
template <typename Fn>
void forEachPixel(Image& img, Fn fn) {
    auto* p = img.data();
    const std::size_t n = img.bytes();
    for (std::size_t i = 0; i < n; i += 4) fn(p + i);
}

// Visits pixels across threads: only for per-pixel maps that depend on nothing but the pixel.
template <typename Fn>
void mapPixels(Image& img, Fn fn) {
    const int w = img.width();
    parallelRows(img.height(), static_cast<std::size_t>(w), [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            std::uint8_t* p = img.pixel(0, y);
            for (int x = 0; x < w; ++x) fn(p + static_cast<std::size_t>(x) * 4);
        }
    });
}

// Applies a per-channel lookup table to RGB.
void applyLut(Image& img, const std::array<std::uint8_t, 256>& lut) {
    mapPixels(img, [&](std::uint8_t* p) {
        p[0] = lut[p[0]];
        p[1] = lut[p[1]];
        p[2] = lut[p[2]];
    });
}

void rgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    if (mx == mn) {
        h = s = 0;
        return;
    }
    float d = mx - mn;
    s = l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
}

float hueToRgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    if (s == 0) {
        r = g = b = l;
        return;
    }
    float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
    float p = 2 * l - q;
    r = hueToRgb(p, q, h + 1.0f / 3);
    g = hueToRgb(p, q, h);
    b = hueToRgb(p, q, h - 1.0f / 3);
}

float luma(const std::uint8_t* p) { return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]; }

// Box-blur passes. Colour is weighted by alpha (premultiplied) so transparent pixels don't bleed
// their colour; sums are exact integers, so the result never depends on thread count or order.
inline void writeBlurred(std::uint8_t* __restrict out, const std::uint32_t* __restrict acc, float inv) {
    const float a = static_cast<float>(acc[3]);
    const float div = a > 0 ? a : 1;
    for (int c = 0; c < 3; ++c) out[c] = a > 0 ? clamp8(static_cast<float>(acc[c]) / div) : 0;
    out[3] = clamp8(a * inv);
}

// Rows: a sliding window along each row; rows are split across threads.
void boxPassH(const Image& src, Image& dst, int r) {
    const int w = src.width();
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    parallelRows(src.height(), static_cast<std::size_t>(w) * 3, [&](int b, int e) {
        std::vector<std::uint32_t> pm(static_cast<std::size_t>(w + 2 * r + 1) * 4);
        for (int y = b; y < e; ++y) {
            const std::uint8_t* row = src.pixel(0, y);
            for (int i = 0; i < w + 2 * r + 1; ++i) {  // premultiplied row, edges repeated
                const std::uint8_t* p = row + static_cast<std::size_t>(std::clamp(i - r, 0, w - 1)) * 4;
                std::uint32_t* q = pm.data() + static_cast<std::size_t>(i) * 4;
                q[0] = p[0] * std::uint32_t{p[3]};
                q[1] = p[1] * std::uint32_t{p[3]};
                q[2] = p[2] * std::uint32_t{p[3]};
                q[3] = p[3];
            }
            std::uint32_t acc[4] = {0, 0, 0, 0};
            for (int i = 0; i <= 2 * r; ++i) {
                for (int c = 0; c < 4; ++c) acc[c] += pm[static_cast<std::size_t>(i) * 4 + c];
            }
            std::uint8_t* out = dst.pixel(0, y);
            for (int x = 0; x < w; ++x) {
                writeBlurred(out + static_cast<std::size_t>(x) * 4, acc, inv);
                const std::uint32_t* add = pm.data() + static_cast<std::size_t>(x + 2 * r + 1) * 4;
                const std::uint32_t* sub = pm.data() + static_cast<std::size_t>(x) * 4;
                for (int c = 0; c < 4; ++c) acc[c] += add[c] - sub[c];
            }
        }
    });
}

// Columns: one running sum per column, sliding down the rows, so every inner loop walks a
// contiguous row segment (cache friendly, vectorizable). Column bands are split across threads.
void boxPassV(const Image& src, Image& dst, int r) {
    const int w = src.width(), h = src.height();
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const int bands = std::max(1, (w + 63) / 64);
    parallelRows(bands, static_cast<std::size_t>(h) * 64 * 3, [&](int b, int e) {
        const int x0 = b * 64, x1 = std::min(w, e * 64), n = (x1 - x0) * 4;
        std::vector<std::uint32_t> acc(static_cast<std::size_t>(n), 0);
        auto addRow = [&](int y, bool subtract) {
            const std::uint8_t* __restrict p = src.pixel(x0, std::clamp(y, 0, h - 1));
            std::uint32_t* __restrict a = acc.data();
            for (int i = 0; i < n; i += 4) {
                const std::uint32_t al = p[i + 3];
                const std::uint32_t v0 = p[i] * al, v1 = p[i + 1] * al, v2 = p[i + 2] * al;
                if (subtract) {
                    a[i] -= v0, a[i + 1] -= v1, a[i + 2] -= v2, a[i + 3] -= al;
                } else {
                    a[i] += v0, a[i + 1] += v1, a[i + 2] += v2, a[i + 3] += al;
                }
            }
        };
        for (int y = -r; y <= r; ++y) addRow(y, false);
        for (int y = 0; y < h; ++y) {
            std::uint8_t* out = dst.pixel(x0, y);
            for (int i = 0; i < n; i += 4) writeBlurred(out + i, acc.data() + i, inv);
            addRow(y + r + 1, false);
            addRow(y - r, true);
        }
    });
}

constexpr double kPi = 3.14159265358979323846;

double lanczos3(double x) {
    x = std::fabs(x);
    if (x < 1e-8) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = kPi * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

// Lanczos-3 taps for one output coordinate: first source index plus normalised weights.
struct Taps {
    int first = 0;
    std::vector<float> w;
};

std::vector<Taps> lanczosTaps(int srcSize, int dstSize) {
    const double scale = static_cast<double>(srcSize) / dstSize;
    const double stretch = std::max(1.0, scale);  // widen the kernel when shrinking so it doesn't alias
    const double support = 3.0 * stretch;
    std::vector<Taps> taps(static_cast<std::size_t>(dstSize));
    for (int i = 0; i < dstSize; ++i) {
        const double centre = (i + 0.5) * scale;
        const int lo = std::max(0, static_cast<int>(std::floor(centre - support)));
        const int hi = std::min(srcSize - 1, static_cast<int>(std::ceil(centre + support)));
        Taps& t = taps[static_cast<std::size_t>(i)];
        t.first = lo;
        double sum = 0;
        for (int j = lo; j <= hi; ++j) {
            const double wt = lanczos3((j + 0.5 - centre) / stretch);
            t.w.push_back(static_cast<float>(wt));
            sum += wt;
        }
        if (sum != 0) {
            for (auto& v : t.w) v = static_cast<float>(v / sum);
        }
    }
    return taps;
}

Image resizeLanczos(const Image& img, int width, int height) {
    const int sw = img.width(), sh = img.height();
    const auto tx = lanczosTaps(sw, width), ty = lanczosTaps(sh, height);
    // horizontal pass into premultiplied floats, then vertical pass back to 8-bit
    std::vector<float> mid(static_cast<std::size_t>(width) * sh * 4);
    parallelRows(sh, static_cast<std::size_t>(width) * 24, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            const std::uint8_t* row = img.pixel(0, y);
            float* out = mid.data() + static_cast<std::size_t>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                const Taps& t = tx[static_cast<std::size_t>(x)];
                float acc[4] = {0, 0, 0, 0};
                for (std::size_t k = 0; k < t.w.size(); ++k) {
                    const std::uint8_t* p = row + (static_cast<std::size_t>(t.first) + k) * 4;
                    const float wa = t.w[k] * p[3];
                    acc[0] += p[0] * wa;
                    acc[1] += p[1] * wa;
                    acc[2] += p[2] * wa;
                    acc[3] += wa;
                }
                for (int c = 0; c < 4; ++c) out[x * 4 + c] = acc[c];
            }
        }
    });
    Image out(width, height);
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    parallelRows(height, static_cast<std::size_t>(width) * 24, [&](int b, int e) {
        std::vector<float> acc(stride);
        for (int y = b; y < e; ++y) {
            const Taps& t = ty[static_cast<std::size_t>(y)];
            std::fill(acc.begin(), acc.end(), 0.0f);
            for (std::size_t k = 0; k < t.w.size(); ++k) {
                const float* __restrict src = mid.data() + (static_cast<std::size_t>(t.first) + k) * stride;
                float* __restrict a = acc.data();
                const float wt = t.w[k];
                for (std::size_t i = 0; i < stride; ++i) a[i] += src[i] * wt;
            }
            std::uint8_t* o = out.pixel(0, y);
            for (int x = 0; x < width; ++x) {
                const float* a = acc.data() + static_cast<std::size_t>(x) * 4;
                const float alpha = a[3];
                const float div = alpha > 0 ? alpha : 1;
                for (int c = 0; c < 3; ++c) o[x * 4 + c] = alpha > 0 ? clamp8(a[c] / div) : 0;
                o[x * 4 + 3] = clamp8(alpha);
            }
        }
    });
    return out;
}

}  // namespace

const std::vector<std::string>& filterNames() {
    static const std::vector<std::string> names = {"nearest", "bilinear", "area", "lanczos"};
    return names;
}

std::optional<Filter> parseFilter(const std::string& name) {
    const auto& n = filterNames();
    for (std::size_t i = 0; i < n.size(); ++i) {
        if (n[i] == name) return static_cast<Filter>(i);
    }
    return std::nullopt;
}

Image resize(const Image& img, int width, int height, Filter filter) {
    width = std::max(1, width);
    height = std::max(1, height);
    if (img.empty()) return Image(width, height);
    const double sx = static_cast<double>(img.width()) / width;
    const double sy = static_cast<double>(img.height()) / height;
    if (filter == Filter::Lanczos) return resizeLanczos(img, width, height);
    Image out(width, height);
    const std::size_t rowCost = static_cast<std::size_t>(width) * 4;

    if (filter == Filter::Nearest) {
        std::vector<int> srcX(static_cast<std::size_t>(width));
        for (int x = 0; x < width; ++x) srcX[static_cast<std::size_t>(x)] = std::min(img.width() - 1, static_cast<int>(x * sx));
        parallelRows(height, rowCost, [&](int b, int e) {
            for (int y = b; y < e; ++y) {
                const int srcY = std::min(img.height() - 1, static_cast<int>(y * sy));
                const std::uint8_t* row = img.pixel(0, srcY);
                std::uint8_t* o = out.pixel(0, y);
                for (int x = 0; x < width; ++x) std::copy_n(row + static_cast<std::size_t>(srcX[static_cast<std::size_t>(x)]) * 4, 4, o + x * 4);
            }
        });
        return out;
    }

    if (sx > 2.0 || sy > 2.0 || (filter == Filter::Area && (sx > 1.0 || sy > 1.0))) {
        // area average, premultiplied
        parallelRows(height, static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(std::ceil(sy)), [&](int b, int e) {
            for (int y = b; y < e; ++y) {
                int y0 = static_cast<int>(y * sy), y1 = std::max(y0 + 1, std::min(img.height(), static_cast<int>((y + 1) * sy)));
                for (int x = 0; x < width; ++x) {
                    int x0 = static_cast<int>(x * sx), x1 = std::max(x0 + 1, std::min(img.width(), static_cast<int>((x + 1) * sx)));
                    double acc[4] = {0, 0, 0, 0};
                    for (int yy = y0; yy < y1; ++yy) {
                        const std::uint8_t* p = img.pixel(x0, yy);
                        std::uint64_t row[4] = {0, 0, 0, 0};  // exact per row, so the sum order never matters
                        for (int xx = 0; xx < x1 - x0; ++xx, p += 4) {
                            row[0] += p[0] * std::uint32_t{p[3]};
                            row[1] += p[1] * std::uint32_t{p[3]};
                            row[2] += p[2] * std::uint32_t{p[3]};
                            row[3] += p[3];
                        }
                        for (int c = 0; c < 4; ++c) acc[c] += static_cast<double>(row[c]);
                    }
                    auto* o = out.pixel(x, y);
                    const double n = static_cast<double>(x1 - x0) * (y1 - y0);
                    for (int c = 0; c < 3; ++c) o[c] = acc[3] > 0 ? clamp8(static_cast<float>(acc[c] / acc[3])) : 0;
                    o[3] = clamp8(static_cast<float>(acc[3] / n));
                }
            }
        });
        return out;
    }

    struct Col {
        int x0, x1;
        float t;
    };
    std::vector<Col> cols(static_cast<std::size_t>(width));
    for (int x = 0; x < width; ++x) {
        double fx = std::max(0.0, (x + 0.5) * sx - 0.5);
        int x0 = std::min(img.width() - 1, static_cast<int>(fx));
        cols[static_cast<std::size_t>(x)] = {x0, std::min(img.width() - 1, x0 + 1), static_cast<float>(fx - x0)};
    }
    parallelRows(height, rowCost * 4, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            double fy = std::max(0.0, (y + 0.5) * sy - 0.5);
            int y0 = std::min(img.height() - 1, static_cast<int>(fy));
            int y1 = std::min(img.height() - 1, y0 + 1);
            float ty = static_cast<float>(fy - y0);
            const std::uint8_t *r0 = img.pixel(0, y0), *r1 = img.pixel(0, y1);
            std::uint8_t* o = out.pixel(0, y);
            for (int x = 0; x < width; ++x, o += 4) {
                const Col& c = cols[static_cast<std::size_t>(x)];
                const float tx = c.t;
                const auto *pa = r0 + c.x0 * 4, *pb = r0 + c.x1 * 4, *pc = r1 + c.x0 * 4, *pd = r1 + c.x1 * 4;
                float wa = (1 - tx) * (1 - ty) * pa[3], wb = tx * (1 - ty) * pb[3], wc = (1 - tx) * ty * pc[3], wd = tx * ty * pd[3];
                float alpha = wa + wb + wc + wd;
                for (int ch = 0; ch < 3; ++ch) {
                    o[ch] = alpha > 0 ? clamp8((pa[ch] * wa + pb[ch] * wb + pc[ch] * wc + pd[ch] * wd) / alpha) : 0;
                }
                o[3] = clamp8(alpha);
            }
        }
    });
    return out;
}

Image crop(const Image& img, int x, int y, int width, int height) {
    int x0 = std::clamp(x, 0, img.width()), y0 = std::clamp(y, 0, img.height());
    int x1 = std::clamp(x + width, 0, img.width()), y1 = std::clamp(y + height, 0, img.height());
    Image out(x1 - x0, y1 - y0);
    for (int yy = y0; yy < y1; ++yy) std::copy_n(img.pixel(x0, yy), (x1 - x0) * 4, out.pixel(0, yy - y0));
    return out;
}

Image rotate90(const Image& img, int turns) {
    turns = ((turns % 4) + 4) % 4;
    if (turns == 0) return img;
    const int w = img.width(), h = img.height();
    Image out(turns == 2 ? w : h, turns == 2 ? h : w);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int nx = 0, ny = 0;
            if (turns == 1) {
                nx = h - 1 - y;
                ny = x;
            } else if (turns == 2) {
                nx = w - 1 - x;
                ny = h - 1 - y;
            } else {
                nx = y;
                ny = w - 1 - x;
            }
            std::copy_n(img.pixel(x, y), 4, out.pixel(nx, ny));
        }
    }
    return out;
}

Image flipHorizontal(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) std::copy_n(img.pixel(x, y), 4, out.pixel(img.width() - 1 - x, y));
    }
    return out;
}

Image flipVertical(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y) std::copy_n(img.pixel(0, y), img.width() * 4, out.pixel(0, img.height() - 1 - y));
    return out;
}

Image resizeCanvas(const Image& img, int width, int height, double ax, double ay) {
    Image out(width, height);
    int ox = static_cast<int>(std::lround((width - img.width()) * std::clamp(ax, 0.0, 1.0)));
    int oy = static_cast<int>(std::lround((height - img.height()) * std::clamp(ay, 0.0, 1.0)));
    for (int y = 0; y < img.height(); ++y) {
        int ny = y + oy;
        if (ny < 0 || ny >= height) continue;
        for (int x = 0; x < img.width(); ++x) {
            int nx = x + ox;
            if (nx >= 0 && nx < width) std::copy_n(img.pixel(x, y), 4, out.pixel(nx, ny));
        }
    }
    return out;
}

const std::vector<std::string>& adjustmentKinds() {
    static const std::vector<std::string> kinds = {"brightness", "contrast", "saturation", "hue",
                                                   "exposure",   "gamma",    "temperature", "vibrance"};
    return kinds;
}

std::optional<Adjust> parseAdjust(const std::string& name) {
    const auto& k = adjustmentKinds();
    for (std::size_t i = 0; i < k.size(); ++i) {
        if (k[i] == name) return static_cast<Adjust>(i);
    }
    return std::nullopt;
}

void adjust(Image& img, Adjust kind, double amount) {
    std::array<std::uint8_t, 256> lut{};
    switch (kind) {
        case Adjust::Brightness: {  // -100..100
            float shift = static_cast<float>(amount) * 2.55f;
            for (int i = 0; i < 256; ++i) lut[i] = clamp8(i + shift);
            applyLut(img, lut);
            return;
        }
        case Adjust::Contrast: {  // -100..100
            float c = static_cast<float>(std::clamp(amount, -100.0, 100.0)) * 2.55f;
            float f = (259 * (c + 255)) / (255 * (259 - c));
            for (int i = 0; i < 256; ++i) lut[i] = clamp8(f * (i - 128) + 128);
            applyLut(img, lut);
            return;
        }
        case Adjust::Exposure: {  // stops, e.g. -2..2
            float f = static_cast<float>(std::pow(2.0, amount));
            for (int i = 0; i < 256; ++i) lut[i] = clamp8(i * f);
            applyLut(img, lut);
            return;
        }
        case Adjust::Gamma: {  // 0.1..10, >1 brightens midtones
            double g = std::max(0.01, amount);
            for (int i = 0; i < 256; ++i) lut[i] = clamp8(static_cast<float>(255.0 * std::pow(i / 255.0, 1.0 / g)));
            applyLut(img, lut);
            return;
        }
        case Adjust::Temperature: {  // -100 (cool) .. 100 (warm)
            float t = static_cast<float>(amount) * 0.3f;
            mapPixels(img, [&](std::uint8_t* p) {
                p[0] = clamp8(p[0] + t);
                p[2] = clamp8(p[2] - t);
            });
            return;
        }
        case Adjust::Saturation:  // -100 (gray) .. 100 (double)
        case Adjust::Vibrance: {
            float s = 1.0f + static_cast<float>(std::clamp(amount, -100.0, 100.0)) / 100.0f;
            bool vibrance = kind == Adjust::Vibrance;
            mapPixels(img, [&](std::uint8_t* p) {
                float l = luma(p);
                float f = s;
                if (vibrance) {  // boost dull colours more than saturated ones
                    float mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
                    float sat = mx > 0 ? (mx - mn) / mx : 0;
                    f = 1 + (s - 1) * (1 - sat);
                }
                for (int c = 0; c < 3; ++c) p[c] = clamp8(l + (p[c] - l) * f);
            });
            return;
        }
        case Adjust::Hue: {  // degrees
            float shift = static_cast<float>(std::fmod(amount / 360.0, 1.0));
            mapPixels(img, [&](std::uint8_t* p) {
                float h, s, l, r, g, b;
                rgbToHsl(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, h, s, l);
                h = std::fmod(h + shift + 1.0f, 1.0f);
                hslToRgb(h, s, l, r, g, b);
                p[0] = clamp8(r * 255);
                p[1] = clamp8(g * 255);
                p[2] = clamp8(b * 255);
            });
            return;
        }
    }
}

void levels(Image& img, double black, double white, double gamma) {
    if (white <= black) white = black + 1;
    double g = std::max(0.01, gamma);
    std::array<std::uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        double t = std::clamp((i - black) / (white - black), 0.0, 1.0);
        lut[i] = clamp8(static_cast<float>(255.0 * std::pow(t, 1.0 / g)));
    }
    applyLut(img, lut);
}

void autoContrast(Image& img) {
    std::array<std::array<std::size_t, 256>, 3> hist{};
    std::size_t count = 0;
    forEachPixel(img, [&](std::uint8_t* p) {
        if (p[3] == 0) return;
        for (int c = 0; c < 3; ++c) ++hist[c][p[c]];
        ++count;
    });
    if (count == 0) return;
    const std::size_t clip = count / 200;  // 0.5% each end
    std::array<std::array<std::uint8_t, 256>, 3> luts{};
    for (int c = 0; c < 3; ++c) {
        int lo = 0, hi = 255;
        for (std::size_t acc = 0; lo < 255 && (acc += hist[c][lo]) <= clip; ++lo) {
        }
        for (std::size_t acc = 0; hi > 0 && (acc += hist[c][hi]) <= clip; --hi) {
        }
        if (hi <= lo) {
            for (int i = 0; i < 256; ++i) luts[c][i] = static_cast<std::uint8_t>(i);
            continue;
        }
        for (int i = 0; i < 256; ++i) luts[c][i] = clamp8((i - lo) * 255.0f / (hi - lo));
    }
    mapPixels(img, [&](std::uint8_t* p) {
        for (int c = 0; c < 3; ++c) p[c] = luts[c][p[c]];
    });
}

const std::vector<std::string>& effectKinds() {
    static const std::vector<std::string> kinds = {"grayscale", "invert", "sepia",    "threshold", "posterize",
                                                   "blur",      "sharpen", "pixelate", "noise"};
    return kinds;
}

std::optional<Effect> parseEffect(const std::string& name) {
    const auto& k = effectKinds();
    for (std::size_t i = 0; i < k.size(); ++i) {
        if (k[i] == name) return static_cast<Effect>(i);
    }
    return std::nullopt;
}

void boxBlur(Image& img, int radius) {
    if (radius <= 0 || img.empty()) return;
    radius = std::min(radius, 30000);  // keeps the integer sums from overflowing
    Image tmp(img.width(), img.height());
    for (int pass = 0; pass < 3; ++pass) {
        boxPassH(img, tmp, radius);
        boxPassV(tmp, img, radius);
    }
}

void unsharpMask(Image& img, double radius, double amount) {
    Image blurred = img;
    boxBlur(blurred, std::max(1, static_cast<int>(std::lround(radius / 2))));
    const float k = static_cast<float>(amount);
    const std::size_t stride = static_cast<std::size_t>(img.width()) * 4;
    parallelRows(img.height(), stride, [&](int rb, int re) {
        std::uint8_t* __restrict p = img.data();
        const std::uint8_t* __restrict b = blurred.data();
        for (std::size_t i = static_cast<std::size_t>(rb) * stride; i < static_cast<std::size_t>(re) * stride; i += 4) {
            for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] + (p[i + c] - b[i + c]) * k);
        }
    });
}

void applyEffect(Image& img, Effect kind, double amount) {
    switch (kind) {
        case Effect::Grayscale:
            mapPixels(img, [](std::uint8_t* p) { p[0] = p[1] = p[2] = clamp8(luma(p)); });
            return;
        case Effect::Invert:
            mapPixels(img, [](std::uint8_t* p) {
                p[0] = 255 - p[0];
                p[1] = 255 - p[1];
                p[2] = 255 - p[2];
            });
            return;
        case Effect::Sepia:
            mapPixels(img, [](std::uint8_t* p) {
                float r = p[0], g = p[1], b = p[2];
                p[0] = clamp8(0.393f * r + 0.769f * g + 0.189f * b);
                p[1] = clamp8(0.349f * r + 0.686f * g + 0.168f * b);
                p[2] = clamp8(0.272f * r + 0.534f * g + 0.131f * b);
            });
            return;
        case Effect::Threshold: {
            float level = amount > 0 ? static_cast<float>(amount) : 128.0f;
            mapPixels(img, [&](std::uint8_t* p) { p[0] = p[1] = p[2] = luma(p) >= level ? 255 : 0; });
            return;
        }
        case Effect::Posterize: {
            int n = std::clamp(static_cast<int>(amount > 0 ? amount : 4), 2, 255);
            std::array<std::uint8_t, 256> lut{};
            for (int i = 0; i < 256; ++i) lut[i] = clamp8(std::round(i * (n - 1) / 255.0f) * 255.0f / (n - 1));
            applyLut(img, lut);
            return;
        }
        case Effect::Blur:
            boxBlur(img, std::max(1, static_cast<int>(std::lround(amount > 0 ? amount / 2 : 1))));
            return;
        case Effect::Sharpen:
            unsharpMask(img, amount > 0 ? amount : 2, 1.0);
            return;
        case Effect::Pixelate: {
            int s = std::max(2, static_cast<int>(amount > 0 ? amount : 8));
            for (int by = 0; by < img.height(); by += s) {
                for (int bx = 0; bx < img.width(); bx += s) {
                    int x1 = std::min(img.width(), bx + s), y1 = std::min(img.height(), by + s);
                    unsigned acc[4] = {0, 0, 0, 0};
                    for (int y = by; y < y1; ++y) {
                        for (int x = bx; x < x1; ++x) {
                            for (int c = 0; c < 4; ++c) acc[c] += img.pixel(x, y)[c];
                        }
                    }
                    unsigned n = static_cast<unsigned>((x1 - bx) * (y1 - by));
                    for (int y = by; y < y1; ++y) {
                        for (int x = bx; x < x1; ++x) {
                            for (int c = 0; c < 4; ++c) img.pixel(x, y)[c] = static_cast<std::uint8_t>(acc[c] / n);
                        }
                    }
                }
            }
            return;
        }
        case Effect::Noise: {
            std::mt19937 rng(12345);  // fixed seed: same result every time, so redo matches
            std::normal_distribution<float> dist(0.0f, static_cast<float>(std::clamp(amount, 0.0, 100.0)) * 0.6f);
            forEachPixel(img, [&](std::uint8_t* p) {
                float n = dist(rng);
                for (int c = 0; c < 3; ++c) p[c] = clamp8(p[c] + n);
            });
            return;
        }
    }
}

}  // namespace wetwaresuite::raster
