#include "achclwss/raster/ops.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>

namespace achclwss::raster {

namespace {

inline std::uint8_t clamp8(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f); }

template <typename Fn>
void forEachPixel(Image& img, Fn fn) {
    auto* p = img.data();
    const std::size_t n = img.bytes();
    for (std::size_t i = 0; i < n; i += 4) fn(p + i);
}

// Applies a per-channel lookup table to RGB.
void applyLut(Image& img, const std::array<std::uint8_t, 256>& lut) {
    forEachPixel(img, [&](std::uint8_t* p) {
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

// One horizontal box-blur pass of the given radius over all four channels.
void boxPass(const Image& src, Image& dst, int r, bool horizontal) {
    const int w = src.width(), h = src.height();
    const int outer = horizontal ? h : w, inner = horizontal ? w : h;
    const float inv = 1.0f / (2 * r + 1);
    std::vector<std::uint8_t> line(static_cast<std::size_t>(inner) * 4);
    for (int o = 0; o < outer; ++o) {
        auto px = [&](int i) { return horizontal ? src.pixel(i, o) : src.pixel(o, i); };
        // premultiply so transparent pixels don't bleed their colour
        float acc[4] = {0, 0, 0, 0};
        auto add = [&](int i, float sign) {
            const auto* p = px(std::clamp(i, 0, inner - 1));
            float a = p[3];
            acc[0] += sign * p[0] * a;
            acc[1] += sign * p[1] * a;
            acc[2] += sign * p[2] * a;
            acc[3] += sign * a;
        };
        for (int i = -r; i <= r; ++i) add(i, 1);
        for (int i = 0; i < inner; ++i) {
            auto* out = horizontal ? dst.pixel(i, o) : dst.pixel(o, i);
            float a = acc[3] * inv;
            if (a > 0) {
                out[0] = clamp8(acc[0] / acc[3]);
                out[1] = clamp8(acc[1] / acc[3]);
                out[2] = clamp8(acc[2] / acc[3]);
            } else {
                out[0] = out[1] = out[2] = 0;
            }
            out[3] = clamp8(a);
            add(i + r + 1, 1);
            add(i - r, -1);
        }
    }
}

}  // namespace

Image resize(const Image& img, int width, int height, Filter filter) {
    width = std::max(1, width);
    height = std::max(1, height);
    Image out(width, height);
    if (img.empty()) return out;
    const double sx = static_cast<double>(img.width()) / width;
    const double sy = static_cast<double>(img.height()) / height;

    if (filter == Filter::Nearest) {
        for (int y = 0; y < height; ++y) {
            int srcY = std::min(img.height() - 1, static_cast<int>(y * sy));
            for (int x = 0; x < width; ++x) {
                int srcX = std::min(img.width() - 1, static_cast<int>(x * sx));
                std::copy_n(img.pixel(srcX, srcY), 4, out.pixel(x, y));
            }
        }
        return out;
    }

    if (sx > 2.0 || sy > 2.0) {
        // area average, premultiplied
        for (int y = 0; y < height; ++y) {
            int y0 = static_cast<int>(y * sy), y1 = std::max(y0 + 1, std::min(img.height(), static_cast<int>((y + 1) * sy)));
            for (int x = 0; x < width; ++x) {
                int x0 = static_cast<int>(x * sx), x1 = std::max(x0 + 1, std::min(img.width(), static_cast<int>((x + 1) * sx)));
                double acc[4] = {0, 0, 0, 0};
                for (int yy = y0; yy < y1; ++yy) {
                    for (int xx = x0; xx < x1; ++xx) {
                        const auto* p = img.pixel(xx, yy);
                        acc[0] += p[0] * p[3];
                        acc[1] += p[1] * p[3];
                        acc[2] += p[2] * p[3];
                        acc[3] += p[3];
                    }
                }
                auto* o = out.pixel(x, y);
                const double n = static_cast<double>(x1 - x0) * (y1 - y0);
                for (int c = 0; c < 3; ++c) o[c] = acc[3] > 0 ? clamp8(static_cast<float>(acc[c] / acc[3])) : 0;
                o[3] = clamp8(static_cast<float>(acc[3] / n));
            }
        }
        return out;
    }

    for (int y = 0; y < height; ++y) {
        double fy = std::max(0.0, (y + 0.5) * sy - 0.5);
        int y0 = std::min(img.height() - 1, static_cast<int>(fy));
        int y1 = std::min(img.height() - 1, y0 + 1);
        float ty = static_cast<float>(fy - y0);
        for (int x = 0; x < width; ++x) {
            double fx = std::max(0.0, (x + 0.5) * sx - 0.5);
            int x0 = std::min(img.width() - 1, static_cast<int>(fx));
            int x1 = std::min(img.width() - 1, x0 + 1);
            float tx = static_cast<float>(fx - x0);
            const auto *a = img.pixel(x0, y0), *b = img.pixel(x1, y0), *c = img.pixel(x0, y1), *d = img.pixel(x1, y1);
            float wa = (1 - tx) * (1 - ty) * a[3], wb = tx * (1 - ty) * b[3], wc = (1 - tx) * ty * c[3], wd = tx * ty * d[3];
            float alpha = wa + wb + wc + wd;
            auto* o = out.pixel(x, y);
            for (int ch = 0; ch < 3; ++ch) {
                o[ch] = alpha > 0 ? clamp8((a[ch] * wa + b[ch] * wb + c[ch] * wc + d[ch] * wd) / alpha) : 0;
            }
            o[3] = clamp8(alpha);
        }
    }
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
            forEachPixel(img, [&](std::uint8_t* p) {
                p[0] = clamp8(p[0] + t);
                p[2] = clamp8(p[2] - t);
            });
            return;
        }
        case Adjust::Saturation:  // -100 (gray) .. 100 (double)
        case Adjust::Vibrance: {
            float s = 1.0f + static_cast<float>(std::clamp(amount, -100.0, 100.0)) / 100.0f;
            bool vibrance = kind == Adjust::Vibrance;
            forEachPixel(img, [&](std::uint8_t* p) {
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
            forEachPixel(img, [&](std::uint8_t* p) {
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
    forEachPixel(img, [&](std::uint8_t* p) {
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
    Image tmp(img.width(), img.height());
    for (int pass = 0; pass < 3; ++pass) {
        boxPass(img, tmp, radius, true);
        boxPass(tmp, img, radius, false);
    }
}

void unsharpMask(Image& img, double radius, double amount) {
    Image blurred = img;
    boxBlur(blurred, std::max(1, static_cast<int>(std::lround(radius / 2))));
    const float k = static_cast<float>(amount);
    auto* p = img.data();
    const auto* b = blurred.data();
    for (std::size_t i = 0; i < img.bytes(); i += 4) {
        for (int c = 0; c < 3; ++c) p[i + c] = clamp8(p[i + c] + (p[i + c] - b[i + c]) * k);
    }
}

void applyEffect(Image& img, Effect kind, double amount) {
    switch (kind) {
        case Effect::Grayscale:
            forEachPixel(img, [](std::uint8_t* p) { p[0] = p[1] = p[2] = clamp8(luma(p)); });
            return;
        case Effect::Invert:
            forEachPixel(img, [](std::uint8_t* p) {
                p[0] = 255 - p[0];
                p[1] = 255 - p[1];
                p[2] = 255 - p[2];
            });
            return;
        case Effect::Sepia:
            forEachPixel(img, [](std::uint8_t* p) {
                float r = p[0], g = p[1], b = p[2];
                p[0] = clamp8(0.393f * r + 0.769f * g + 0.189f * b);
                p[1] = clamp8(0.349f * r + 0.686f * g + 0.168f * b);
                p[2] = clamp8(0.272f * r + 0.534f * g + 0.131f * b);
            });
            return;
        case Effect::Threshold: {
            float level = amount > 0 ? static_cast<float>(amount) : 128.0f;
            forEachPixel(img, [&](std::uint8_t* p) { p[0] = p[1] = p[2] = luma(p) >= level ? 255 : 0; });
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

}  // namespace achclwss::raster
