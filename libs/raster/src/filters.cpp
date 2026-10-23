#include "wetwaresuite/raster/filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

namespace {

constexpr double kPi = 3.14159265358979323846;

inline std::uint8_t to8(double v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5); }
inline double lumOf(const std::uint8_t* p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }

// Premultiplied bilinear sample at (x, y) in pixel centres, edges repeat. out = r*a, g*a, b*a, a (0..255).
void sample(const Image& img, double x, double y, double out[4]) {
    const int w = img.width(), h = img.height();
    x -= 0.5;
    y -= 0.5;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double tx = x - x0, ty = y - y0;
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int j = 0; j < 2; ++j) {
        const int yy = std::clamp(y0 + j, 0, h - 1);
        const double wy = j ? ty : 1 - ty;
        for (int i = 0; i < 2; ++i) {
            const int xx = std::clamp(x0 + i, 0, w - 1);
            const double wgt = (i ? tx : 1 - tx) * wy;
            if (wgt <= 0) continue;
            const std::uint8_t* p = img.pixel(xx, yy);
            const double a = p[3] / 255.0;
            out[0] += p[0] * a * wgt;
            out[1] += p[1] * a * wgt;
            out[2] += p[2] * a * wgt;
            out[3] += p[3] * wgt;
        }
    }
}

// Writes an accumulated premultiplied sum (divided by n) as a straight pixel.
inline void store(std::uint8_t* d, const double acc[4], double n) {
    const double a = acc[3] / n;
    if (a <= 0.0001) {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    const double inv = 255.0 / (acc[3]);
    d[0] = to8(acc[0] * inv);
    d[1] = to8(acc[1] * inv);
    d[2] = to8(acc[2] * inv);
    d[3] = to8(a);
}

// out(x, y) = average of n samples from src at positions pos(x, y, i, n).
template <typename Pos>
Image sampleAlong(const Image& src, Pos pos) {
    Image out(src.width(), src.height());
    parallelRows(src.height(), static_cast<std::size_t>(src.width()) * 16, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < src.width(); ++x) {
                double acc[4] = {0, 0, 0, 0}, s[4];
                int n = 0;
                pos(x + 0.5, y + 0.5, [&](double sx, double sy) {
                    sample(src, sx, sy, s);
                    for (int c = 0; c < 4; ++c) acc[c] += s[c];
                    ++n;
                });
                store(out.pixel(x, y), acc, std::max(1, n));
            }
        }
    });
    return out;
}

int boxRadiusFor(double sigma) { return std::max(0, static_cast<int>(std::lround(std::sqrt(sigma * sigma + 0.25) - 0.5))); }

std::uint32_t hash(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    std::uint32_t h = seed * 0x9e3779b9u ^ x * 0x85ebca6bu ^ y * 0xc2b2ae35u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

double unit(std::uint32_t h) { return (h & 0xffffff) / 16777216.0; }

// Separable running-sum box filter on a float plane, radius r, edges clamp.
void boxPlane(std::vector<float>& v, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(v.size());
    parallelRows(h, static_cast<std::size_t>(w), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* row = v.data() + static_cast<std::size_t>(y) * w;
            float* out = tmp.data() + static_cast<std::size_t>(y) * w;
            double sum = 0;
            for (int i = -r; i <= r; ++i) sum += row[std::clamp(i, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                out[x] = static_cast<float>(sum / (2 * r + 1));
                sum += row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
            }
        }
    });
    parallelRows(w, static_cast<std::size_t>(h), [&](int x0, int x1) {
        for (int x = x0; x < x1; ++x) {
            double sum = 0;
            for (int i = -r; i <= r; ++i) sum += tmp[static_cast<std::size_t>(std::clamp(i, 0, h - 1)) * w + x];
            for (int y = 0; y < h; ++y) {
                v[static_cast<std::size_t>(y) * w + x] = static_cast<float>(sum / (2 * r + 1));
                sum += tmp[static_cast<std::size_t>(std::min(y + r + 1, h - 1)) * w + x] - tmp[static_cast<std::size_t>(std::max(y - r, 0)) * w + x];
            }
        }
    });
}

}  // namespace

void gaussianBlur(Image& img, double radius) {
    if (radius <= 0) return;
    boxBlur(img, std::max(1, boxRadiusFor(radius)));
}

void motionBlur(Image& img, double angle, double distance) {
    if (distance < 1 || img.empty()) return;
    const double a = angle * kPi / 180, dx = std::cos(a), dy = -std::sin(a);
    const int n = std::clamp(static_cast<int>(std::ceil(distance)), 2, 400);
    img = sampleAlong(img, [&](double x, double y, auto take) {
        for (int i = 0; i < n; ++i) {
            const double t = (i / double(n - 1) - 0.5) * distance;
            take(x + dx * t, y + dy * t);
        }
    });
}

void spinBlur(Image& img, double cx, double cy, double amount) {
    if (amount <= 0 || img.empty()) return;
    const double total = std::min(amount, 360.0) * kPi / 180;
    img = sampleAlong(img, [&](double x, double y, auto take) {
        const double rx = x - cx, ry = y - cy, r = std::hypot(rx, ry);
        const int n = std::clamp(static_cast<int>(r * total / 1.5) + 1, 1, 96);
        for (int i = 0; i < n; ++i) {
            const double t = n == 1 ? 0 : (i / double(n - 1) - 0.5) * total;
            const double c = std::cos(t), s = std::sin(t);
            take(cx + rx * c - ry * s, cy + rx * s + ry * c);
        }
    });
}

void zoomBlur(Image& img, double cx, double cy, double amount) {
    if (amount <= 0 || img.empty()) return;
    const double k = std::clamp(amount, 0.0, 100.0) / 100;
    img = sampleAlong(img, [&](double x, double y, auto take) {
        const double rx = x - cx, ry = y - cy;
        const int n = std::clamp(static_cast<int>(std::hypot(rx, ry) * k / 1.5) + 1, 1, 96);
        for (int i = 0; i < n; ++i) {
            const double t = n == 1 ? 1 : 1 - k * i / double(n - 1);
            take(cx + rx * t, cy + ry * t);
        }
    });
}

void unsharpMaskThreshold(Image& img, double radius, double amount, int threshold) {
    Image blurred = img;
    gaussianBlur(blurred, radius);
    const double k = amount / 100;
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                std::uint8_t* p = img.pixel(x, y);
                const std::uint8_t* b = blurred.pixel(x, y);
                if (std::fabs(lumOf(p) - lumOf(b)) <= threshold) continue;
                for (int c = 0; c < 3; ++c) p[c] = to8(p[c] + (p[c] - b[c]) * k);
            }
        }
    });
}

void highPass(Image& img, double radius) {
    Image blurred = img;
    gaussianBlur(blurred, std::max(0.5, radius));
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                std::uint8_t* p = img.pixel(x, y);
                const std::uint8_t* b = blurred.pixel(x, y);
                for (int c = 0; c < 3; ++c) p[c] = to8(128.0 + p[c] - b[c]);
            }
        }
    });
}

void medianFilter(Image& img, int radius) {
    if (radius <= 0 || img.empty()) return;
    radius = std::min(radius, 50);
    const Image src = img;
    const int w = img.width(), h = img.height();
    const int count = (2 * radius + 1) * (2 * radius + 1), half = count / 2;
    parallelRows(h, static_cast<std::size_t>(w) * radius * 12, [&](int y0, int y1) {
        std::array<std::array<int, 256>, 4> hist{};
        for (int y = y0; y < y1; ++y) {
            for (auto& hc : hist) hc.fill(0);
            auto column = [&](int x, int add) {
                const int xx = std::clamp(x, 0, w - 1);
                for (int j = -radius; j <= radius; ++j) {
                    const std::uint8_t* p = src.pixel(xx, std::clamp(y + j, 0, h - 1));
                    for (int c = 0; c < 4; ++c) hist[static_cast<std::size_t>(c)][p[c]] += add;
                }
            };
            for (int i = -radius; i <= radius; ++i) column(i, 1);
            for (int x = 0; x < w; ++x) {
                std::uint8_t* d = img.pixel(x, y);
                for (int c = 0; c < 4; ++c) {
                    int seen = 0, v = 0;
                    const auto& hc = hist[static_cast<std::size_t>(c)];
                    while (v < 255 && seen + hc[static_cast<std::size_t>(v)] <= half) seen += hc[static_cast<std::size_t>(v++)];
                    d[c] = static_cast<std::uint8_t>(v);
                }
                column(x - radius, -1);
                column(x + radius + 1, 1);
            }
        }
    });
}

void dustAndScratches(Image& img, int radius, int threshold) {
    Image med = img;
    medianFilter(med, std::max(1, radius));
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                std::uint8_t* p = img.pixel(x, y);
                const std::uint8_t* m = med.pixel(x, y);
                const int diff = std::max({std::abs(p[0] - m[0]), std::abs(p[1] - m[1]), std::abs(p[2] - m[2])});
                if (diff > threshold) std::copy_n(m, 4, p);
            }
        }
    });
}

void addNoise(Image& img, double amount, bool gaussian, bool monochrome, std::uint32_t seed) {
    const double s = std::clamp(amount, 0.0, 400.0) / 100 * 128;
    if (s <= 0) return;
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                std::uint8_t* p = img.pixel(x, y);
                double n[3];
                for (int c = 0; c < 3; ++c) {
                    const std::uint32_t h1 = hash(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), seed + static_cast<std::uint32_t>(monochrome ? 0 : c) * 7919u);
                    if (gaussian) {
                        const double u1 = std::max(1e-9, unit(h1)), u2 = unit(hash(h1, 77, seed));
                        n[c] = std::sqrt(-2 * std::log(u1)) * std::cos(2 * kPi * u2) * s / 2;
                    } else {
                        n[c] = (unit(h1) * 2 - 1) * s;
                    }
                }
                for (int c = 0; c < 3; ++c) p[c] = to8(p[c] + n[c]);
            }
        }
    });
}

void emboss(Image& img, double angle, double height, double amount) {
    const Image src = img;
    const double a = angle * kPi / 180, h = std::max(1.0, height);
    const double dx = std::cos(a) * h, dy = -std::sin(a) * h, k = amount / 100;
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                double s1[4], s2[4];
                sample(src, x + 0.5 + dx, y + 0.5 + dy, s1);  // towards the light
                sample(src, x + 0.5 - dx, y + 0.5 - dy, s2);
                const double l1 = 0.299 * s1[0] + 0.587 * s1[1] + 0.114 * s1[2], l2 = 0.299 * s2[0] + 0.587 * s2[1] + 0.114 * s2[2];
                const std::uint8_t v = to8(128 + (l2 - l1) * k);
                std::uint8_t* p = img.pixel(x, y);
                p[0] = p[1] = p[2] = v;
            }
        }
    });
}

void findEdges(Image& img) {
    const Image src = img;
    const int w = img.width(), h = img.height();
    parallelRows(h, static_cast<std::size_t>(w) * 9, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                auto at = [&](int i, int j, int c) { return src.pixel(std::clamp(x + i, 0, w - 1), std::clamp(y + j, 0, h - 1))[c]; };
                std::uint8_t* p = img.pixel(x, y);
                for (int c = 0; c < 3; ++c) {
                    const double gx = at(1, -1, c) + 2 * at(1, 0, c) + at(1, 1, c) - at(-1, -1, c) - 2 * at(-1, 0, c) - at(-1, 1, c);
                    const double gy = at(-1, 1, c) + 2 * at(0, 1, c) + at(1, 1, c) - at(-1, -1, c) - 2 * at(0, -1, c) - at(1, -1, c);
                    p[c] = to8(255 - std::sqrt(gx * gx + gy * gy) / 4);
                }
            }
        }
    });
}

void oilPaint(Image& img, int radius) {
    if (radius <= 0 || img.empty()) return;
    radius = std::min(radius, 40);
    const Image src = img;
    const int w = img.width(), h = img.height(), r = radius;
    constexpr int kBand = 128;
    const int bands = (h + kBand - 1) / kBand;
    parallelRows(bands, static_cast<std::size_t>(w) * kBand * 40, [&](int b0, int b1) {
        for (int band = b0; band < b1; ++band) {
            const int y0 = band * kBand, y1 = std::min(h, y0 + kBand);
            const int iy0 = y0 - r, iy1 = y1 + r;  // rows the integrals cover
            const int iw = w + 2 * r + 1, ih = iy1 - iy0 + 1;
            // integrals of r, g, b, a, lum and lum^2 over the band (with a border of r)
            std::vector<std::array<double, 6>> I(static_cast<std::size_t>(iw) * ih, {0, 0, 0, 0, 0, 0});
            for (int j = 1; j < ih; ++j) {
                std::array<double, 6> row{0, 0, 0, 0, 0, 0};
                const int sy = std::clamp(iy0 + j - 1, 0, h - 1);
                for (int i = 1; i < iw; ++i) {
                    const std::uint8_t* p = src.pixel(std::clamp(i - 1 - r, 0, w - 1), sy);
                    const double l = lumOf(p);
                    row[0] += p[0], row[1] += p[1], row[2] += p[2], row[3] += p[3], row[4] += l, row[5] += l * l;
                    auto& cell = I[static_cast<std::size_t>(j) * iw + i];
                    const auto& up = I[static_cast<std::size_t>(j - 1) * iw + i];
                    for (int c = 0; c < 6; ++c) cell[static_cast<std::size_t>(c)] = up[static_cast<std::size_t>(c)] + row[static_cast<std::size_t>(c)];
                }
            }
            auto box = [&](int xa, int ya, int xb, int yb, int c) {  // inclusive pixel box in band-local integral coords
                auto v = [&](int i, int j) { return I[static_cast<std::size_t>(j) * iw + i][static_cast<std::size_t>(c)]; };
                return v(xb + 1, yb + 1) - v(xa, yb + 1) - v(xb + 1, ya) + v(xa, ya);
            };
            const double n = (r + 1.0) * (r + 1.0);
            for (int y = y0; y < y1; ++y) {
                const int ly = y - iy0;
                for (int x = 0; x < w; ++x) {
                    const int lx = x + r;
                    const int quads[4][4] = {{lx - r, ly - r, lx, ly}, {lx, ly - r, lx + r, ly}, {lx - r, ly, lx, ly + r}, {lx, ly, lx + r, ly + r}};
                    int best = 0;
                    double bestVar = 1e300;
                    for (int q = 0; q < 4; ++q) {
                        const double m = box(quads[q][0], quads[q][1], quads[q][2], quads[q][3], 4) / n;
                        const double var = box(quads[q][0], quads[q][1], quads[q][2], quads[q][3], 5) / n - m * m;
                        if (var < bestVar) bestVar = var, best = q;
                    }
                    std::uint8_t* d = img.pixel(x, y);
                    for (int c = 0; c < 4; ++c) d[c] = to8(box(quads[best][0], quads[best][1], quads[best][2], quads[best][3], c) / n);
                }
            }
        }
    });
}

void renderClouds(Image& img, Color a, Color b, double scale, std::uint32_t seed) {
    const double base = std::max(2.0, scale);
    auto lattice = [&](int x, int y, int octave) { return unit(hash(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), seed + static_cast<std::uint32_t>(octave) * 1013u)); };
    auto noise = [&](double x, double y, int octave) {
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        double tx = x - x0, ty = y - y0;
        tx = tx * tx * (3 - 2 * tx);
        ty = ty * ty * (3 - 2 * ty);
        const double top = lattice(x0, y0, octave) * (1 - tx) + lattice(x0 + 1, y0, octave) * tx;
        const double bot = lattice(x0, y0 + 1, octave) * (1 - tx) + lattice(x0 + 1, y0 + 1, octave) * tx;
        return top * (1 - ty) + bot * ty;
    };
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 30, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                double sum = 0, amp = 1, norm = 0, cell = base;
                for (int o = 0; o < 12 && cell >= 1; ++o, cell /= 2, amp *= 0.5) {
                    sum += noise((x + 0.5) / cell, (y + 0.5) / cell, o) * amp;
                    norm += amp;
                }
                const double t = std::clamp((sum / norm - 0.5) * 1.6 + 0.5, 0.0, 1.0);
                std::uint8_t* p = img.pixel(x, y);
                p[0] = to8(a.r + (b.r - a.r) * t);
                p[1] = to8(a.g + (b.g - a.g) * t);
                p[2] = to8(a.b + (b.b - a.b) * t);
                p[3] = to8(a.a + (b.a - a.a) * t);
            }
        }
    });
}

Image displace(const Image& img, const Image& map, double scaleX, double scaleY, bool wrap) {
    Image out(img.width(), img.height());
    if (img.empty() || map.empty()) return img;
    const int w = img.width(), h = img.height();
    parallelRows(h, static_cast<std::size_t>(w) * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* m = map.pixel(std::min(map.width() - 1, x * map.width() / w), std::min(map.height() - 1, y * map.height() / h));
                double sx = x + 0.5 + (m[0] - 128) / 128.0 * scaleX, sy = y + 0.5 + (m[1] - 128) / 128.0 * scaleY;
                if (wrap) {
                    sx = std::fmod(std::fmod(sx, w) + w, w);
                    sy = std::fmod(std::fmod(sy, h) + h, h);
                }
                double s[4];
                sample(img, sx, sy, s);
                store(out.pixel(x, y), s, 1);
            }
        }
    });
    return out;
}

Image lensCorrect(const Image& img, double distortion, double vignette, double chromatic) {
    Image out(img.width(), img.height());
    if (img.empty()) return img;
    const double cx = img.width() / 2.0, cy = img.height() / 2.0, rmax = std::hypot(cx, cy);
    const double k = std::clamp(distortion, -100.0, 100.0) / 100 * 0.5, v = std::clamp(vignette, -100.0, 100.0) / 100;
    const double ca = std::clamp(chromatic, -10.0, 10.0) / rmax;
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 16, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, rn2 = (dx * dx + dy * dy) / (rmax * rmax);
                const double f = 1 + k * rn2;
                double g[4], r[4], b[4];
                sample(img, cx + dx * f, cy + dy * f, g);
                if (ca != 0) {
                    sample(img, cx + dx * f * (1 + ca), cy + dy * f * (1 + ca), r);
                    sample(img, cx + dx * f * (1 - ca), cy + dy * f * (1 - ca), b);
                    g[0] = r[3] > 0 ? r[0] / r[3] * g[3] : g[0];
                    g[2] = b[3] > 0 ? b[2] / b[3] * g[3] : g[2];
                }
                const double light = 1 + v * rn2;
                for (int c = 0; c < 3; ++c) g[c] *= light;
                store(out.pixel(x, y), g, 1);
            }
        }
    });
    return out;
}

Image warpGrid(const Image& img, int cols, int rows, const std::vector<double>& moves) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (img.empty() || moves.size() != static_cast<std::size_t>((cols + 1) * (rows + 1) * 2)) return img;
    const double cw = img.width() / double(cols), ch = img.height() / double(rows);
    auto move = [&](int i, int j, int c) { return moves[(static_cast<std::size_t>(j) * (cols + 1) + i) * 2 + c]; };
    // Catmull-Rom between control points, so the bend is smooth across cells
    auto cr = [](double p0, double p1, double p2, double p3, double t) {
        return 0.5 * (2 * p1 + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t + (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
    };
    auto at = [&](double gx, double gy, int c) {
        const int i = std::clamp(static_cast<int>(std::floor(gx)), 0, cols - 1), j = std::clamp(static_cast<int>(std::floor(gy)), 0, rows - 1);
        const double tx = gx - i, ty = gy - j;
        double col[4];
        for (int n = 0; n < 4; ++n) {
            const int jj = std::clamp(j - 1 + n, 0, rows);
            col[n] = cr(move(std::clamp(i - 1, 0, cols), jj, c), move(i, jj, c), move(std::min(i + 1, cols), jj, c), move(std::min(i + 2, cols), jj, c), tx);
        }
        return cr(col[0], col[1], col[2], col[3], ty);
    };
    Image out(img.width(), img.height());
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 30, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const double gx = (x + 0.5) / cw, gy = (y + 0.5) / ch;
                double s[4];
                sample(img, x + 0.5 - at(gx, gy, 0), y + 0.5 - at(gx, gy, 1), s);
                store(out.pixel(x, y), s, 1);
            }
        }
    });
    return out;
}

Image liquify(const Image& img, LiquifyMode mode, const std::vector<double>& points, double size, double pressure) {
    if (img.empty() || points.size() < 2) return img;
    const double R = std::max(2.0, size / 2), pr = std::clamp(pressure, 0.0, 1.0);
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        minX = std::min(minX, points[i]), maxX = std::max(maxX, points[i]);
        minY = std::min(minY, points[i + 1]), maxY = std::max(maxY, points[i + 1]);
    }
    const int bx0 = std::max(0, static_cast<int>(minX - R - 1)), by0 = std::max(0, static_cast<int>(minY - R - 1));
    const int bx1 = std::min(img.width(), static_cast<int>(maxX + R + 2)), by1 = std::min(img.height(), static_cast<int>(maxY + R + 2));
    if (bx1 <= bx0 || by1 <= by0) return img;
    const int bw = bx1 - bx0, bh = by1 - by0;
    std::vector<float> fx(static_cast<std::size_t>(bw) * bh, 0.0f), fy(fx.size(), 0.0f);  // where each pixel reads from, as an offset
    auto field = [&](const std::vector<float>& f, double x, double y) {  // bilinear, 0 outside the box
        x -= bx0 + 0.5;
        y -= by0 + 0.5;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const double tx = x - x0, ty = y - y0;
        double v = 0;
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int xx = x0 + i, yy = y0 + j;
                if (xx < 0 || yy < 0 || xx >= bw || yy >= bh) continue;
                v += f[static_cast<std::size_t>(yy) * bw + xx] * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            }
        }
        return v;
    };
    auto dab = [&](double cx, double cy, double mx, double my) {
        const int x0 = std::max(bx0, static_cast<int>(cx - R)), x1 = std::min(bx1, static_cast<int>(cx + R + 1));
        const int y0 = std::max(by0, static_cast<int>(cy - R)), y1 = std::min(by1, static_cast<int>(cy + R + 1));
        if (x1 <= x0 || y1 <= y0) return;
        std::vector<float> nx(static_cast<std::size_t>(x1 - x0) * (y1 - y0)), ny(nx.size());
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const double px = x + 0.5, py = y + 0.5, rx = px - cx, ry = py - cy;
                const double d2 = (rx * rx + ry * ry) / (R * R);
                const std::size_t li = static_cast<std::size_t>(y - by0) * bw + (x - bx0), di = static_cast<std::size_t>(y - y0) * (x1 - x0) + (x - x0);
                if (d2 >= 1) {
                    nx[di] = fx[li], ny[di] = fy[li];
                    continue;
                }
                const double w = (1 - d2) * (1 - d2) * pr;
                double dx = 0, dy = 0;
                switch (mode) {
                    case LiquifyMode::Push: dx = -mx * w, dy = -my * w; break;
                    case LiquifyMode::Bloat: dx = -rx * w * 0.3, dy = -ry * w * 0.3; break;
                    case LiquifyMode::Pucker: dx = rx * w * 0.3, dy = ry * w * 0.3; break;
                    case LiquifyMode::TwirlRight:
                    case LiquifyMode::TwirlLeft: {
                        const double t = (mode == LiquifyMode::TwirlRight ? -1 : 1) * w * 0.4;
                        dx = rx * std::cos(t) - ry * std::sin(t) - rx;
                        dy = rx * std::sin(t) + ry * std::cos(t) - ry;
                        break;
                    }
                }
                // compose: read where the earlier strokes read, from the spot this dab reads
                nx[di] = static_cast<float>(dx + field(fx, px + dx, py + dy));
                ny[di] = static_cast<float>(dy + field(fy, px + dx, py + dy));
            }
        }
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const std::size_t li = static_cast<std::size_t>(y - by0) * bw + (x - bx0), di = static_cast<std::size_t>(y - y0) * (x1 - x0) + (x - x0);
                fx[li] = nx[di], fy[li] = ny[di];
            }
        }
    };
    const double step = std::max(1.0, R * 0.25);
    if (points.size() < 4) {
        for (int i = 0; i < 8; ++i) dab(points[0], points[1], 0, 0);  // holding still: bloat, pucker and twirl keep going
    } else {
        double px = points[0], py = points[1];
        for (std::size_t i = 2; i + 1 < points.size(); i += 2) {
            const double qx = points[i], qy = points[i + 1], len = std::hypot(qx - px, qy - py);
            const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
            for (int k = 1; k <= n; ++k) {
                const double cx = px + (qx - px) * k / n, cy = py + (qy - py) * k / n;
                dab(cx, cy, (qx - px) / n, (qy - py) / n);
            }
            px = qx, py = qy;
        }
    }
    Image out = img;
    for (int y = by0; y < by1; ++y) {
        for (int x = bx0; x < bx1; ++x) {
            const std::size_t li = static_cast<std::size_t>(y - by0) * bw + (x - bx0);
            if (fx[li] == 0 && fy[li] == 0) continue;
            double s[4];
            sample(img, x + 0.5 + fx[li], y + 0.5 + fy[li], s);
            store(out.pixel(x, y), s, 1);
        }
    }
    return out;
}

Mask subjectMask(const Image& img, int tolerance) {
    const int w = img.width(), h = img.height();
    Mask m(w, h, 0);
    if (w < 3 || h < 3) return m;
    // the background's colours: a few clusters of the border pixels
    std::vector<std::array<double, 3>> border;
    auto addBorder = [&](int x, int y) {
        const std::uint8_t* p = img.pixel(x, y);
        border.push_back({double(p[0]), double(p[1]), double(p[2])});
    };
    const int stepX = std::max(1, w / 200), stepY = std::max(1, h / 200);
    for (int x = 0; x < w; x += stepX) addBorder(x, 0), addBorder(x, h - 1);
    for (int y = 0; y < h; y += stepY) addBorder(0, y), addBorder(w - 1, y);
    std::vector<std::array<double, 3>> centres;
    for (int k = 0; k < 6; ++k) centres.push_back(border[border.size() * static_cast<std::size_t>(k) / 6]);
    for (int iter = 0; iter < 8; ++iter) {
        std::vector<std::array<double, 4>> sum(centres.size(), {0, 0, 0, 0});
        for (const auto& b : border) {
            std::size_t best = 0;
            double bd = 1e18;
            for (std::size_t c = 0; c < centres.size(); ++c) {
                const double d = std::pow(b[0] - centres[c][0], 2) + std::pow(b[1] - centres[c][1], 2) + std::pow(b[2] - centres[c][2], 2);
                if (d < bd) bd = d, best = c;
            }
            for (int c = 0; c < 3; ++c) sum[best][static_cast<std::size_t>(c)] += b[static_cast<std::size_t>(c)];
            sum[best][3] += 1;
        }
        for (std::size_t c = 0; c < centres.size(); ++c) {
            if (sum[c][3] > 0) centres[c] = {sum[c][0] / sum[c][3], sum[c][1] / sum[c][3], sum[c][2] / sum[c][3]};
        }
    }
    // foreground: unlike every background colour
    std::vector<std::uint8_t> fg(static_cast<std::size_t>(w) * h, 0);
    const double tol2 = static_cast<double>(tolerance) * tolerance;
    parallelRows(h, static_cast<std::size_t>(w) * 6, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t* p = img.pixel(x, y);
                double bd = 1e18;
                for (const auto& c : centres) bd = std::min(bd, std::pow(p[0] - c[0], 2) + std::pow(p[1] - c[1], 2) + std::pow(p[2] - c[2], 2));
                fg[static_cast<std::size_t>(y) * w + x] = bd > tol2 && p[3] > 0;
            }
        }
    });
    // keep the biggest pieces (specks of noise in the background go)
    std::vector<int> label(fg.size(), -1);
    std::vector<std::size_t> sizes;
    std::vector<std::size_t> stack;
    for (std::size_t i = 0; i < fg.size(); ++i) {
        if (!fg[i] || label[i] >= 0) continue;
        const int id = static_cast<int>(sizes.size());
        std::size_t n = 0;
        stack.push_back(i);
        label[i] = id;
        while (!stack.empty()) {
            const std::size_t q = stack.back();
            stack.pop_back();
            ++n;
            const int x = static_cast<int>(q % w), y = static_cast<int>(q / w);
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& c : nb) {
                if (c[0] < 0 || c[1] < 0 || c[0] >= w || c[1] >= h) continue;
                const std::size_t j = static_cast<std::size_t>(c[1]) * w + c[0];
                if (fg[j] && label[j] < 0) label[j] = id, stack.push_back(j);
            }
        }
        sizes.push_back(n);
    }
    if (sizes.empty()) return m;
    const std::size_t biggest = *std::max_element(sizes.begin(), sizes.end());
    for (std::size_t i = 0; i < fg.size(); ++i) fg[i] = label[i] >= 0 && sizes[static_cast<std::size_t>(label[i])] * 5 >= biggest;
    // fill holes: background is what the border can reach without crossing the subject
    std::vector<std::uint8_t> outside(fg.size(), 0);
    for (int x = 0; x < w; ++x) stack.push_back(static_cast<std::size_t>(x)), stack.push_back(static_cast<std::size_t>(h - 1) * w + x);
    for (int y = 0; y < h; ++y) stack.push_back(static_cast<std::size_t>(y) * w), stack.push_back(static_cast<std::size_t>(y) * w + w - 1);
    while (!stack.empty()) {
        const std::size_t q = stack.back();
        stack.pop_back();
        if (fg[q] || outside[q]) continue;
        outside[q] = 1;
        const int x = static_cast<int>(q % w), y = static_cast<int>(q / w);
        if (x > 0) stack.push_back(q - 1);
        if (x < w - 1) stack.push_back(q + 1);
        if (y > 0) stack.push_back(q - static_cast<std::size_t>(w));
        if (y < h - 1) stack.push_back(q + static_cast<std::size_t>(w));
    }
    std::vector<float> soft(fg.size());
    for (std::size_t i = 0; i < fg.size(); ++i) soft[i] = outside[i] ? 0.0f : 1.0f;
    boxPlane(soft, w, h, 1);  // a one-pixel soft edge
    for (std::size_t i = 0; i < soft.size(); ++i) m.v[i] = to8(soft[i] * 255);
    return m;
}

Mask refineEdge(const Mask& mask, const Image& guide, int radius, double smooth, double contrast, double shift) {
    const int w = mask.width, h = mask.height;
    if (mask.empty() || guide.width() != w || guide.height() != h) return mask;
    const std::size_t n = static_cast<std::size_t>(w) * h;
    std::vector<float> I(n), p(n);
    for (std::size_t i = 0; i < n; ++i) {
        I[i] = static_cast<float>(lumOf(guide.data() + i * 4) / 255);
        p[i] = mask.v[i] / 255.0f;
    }
    std::vector<float> q = p;
    if (radius > 0) {
        // guided filter: the mask follows the picture's edges inside each window
        std::vector<float> mI = I, mp = p, II(n), Ip(n);
        for (std::size_t i = 0; i < n; ++i) II[i] = I[i] * I[i], Ip[i] = I[i] * p[i];
        boxPlane(mI, w, h, radius);
        boxPlane(mp, w, h, radius);
        boxPlane(II, w, h, radius);
        boxPlane(Ip, w, h, radius);
        std::vector<float> a(n), b(n);
        const float eps = 0.0025f;
        for (std::size_t i = 0; i < n; ++i) {
            const float var = II[i] - mI[i] * mI[i], cov = Ip[i] - mI[i] * mp[i];
            a[i] = cov / (var + eps);
            b[i] = mp[i] - a[i] * mI[i];
        }
        boxPlane(a, w, h, radius);
        boxPlane(b, w, h, radius);
        for (std::size_t i = 0; i < n; ++i) q[i] = a[i] * I[i] + b[i];
    }
    if (smooth > 0) {
        const int r = std::max(1, boxRadiusFor(smooth));
        for (int pass = 0; pass < 3; ++pass) boxPlane(q, w, h, r);
    }
    const double gain = 1 + std::clamp(contrast, 0.0, 100.0) / 100 * 8, off = std::clamp(shift, -100.0, 100.0) / 200;
    Mask out(w, h);
    for (std::size_t i = 0; i < n; ++i) out.v[i] = to8(std::clamp(0.5 + (q[i] + off - 0.5) * gain, 0.0, 1.0) * 255);
    return out;
}

}  // namespace wetwaresuite::raster
