// Colour correction (exposure, white balance, lift/gamma/gain wheels,
// brightness/contrast/gamma, curves, saturation, .cube LUTs) and the effects
// that work on a clip's own pixels (chroma key with spill suppression, blur,
// sharpen, vignette, flips). The separable steps fold into one 256-entry table
// per channel, so a full grade costs about as much as one pass.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "render_internal.hpp"
#include "wetwaresuite/raster/filters.hpp"
#include "wetwaresuite/raster/ops.hpp"
#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::cut {

using raster::Color;
using raster::Image;

std::optional<Color> parseColour(const std::string& raw) {
    std::string text = raw;
    double alpha = 1;
    if (auto at = text.find('@'); at != std::string::npos) {
        try {
            alpha = std::clamp(std::stod(text.substr(at + 1)), 0.0, 1.0);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        text = text.substr(0, at);
    }
    if (text.size() == 8 && (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)) text = "#" + text.substr(2);
    auto c = Color::parse(text);
    if (!c) return std::nullopt;
    c->a = static_cast<std::uint8_t>(std::lround(c->a * alpha));
    return c;
}

Color colourOr(const std::string& text, Color fallback) {
    auto c = parseColour(text);
    return c ? *c : fallback;
}

// ---- curves and LUTs ----

std::vector<float> curveTable(const std::vector<double>& pts) {
    std::vector<float> table(256);
    std::vector<std::pair<double, double>> p;
    for (std::size_t i = 0; i + 1 < pts.size(); i += 2) p.push_back({std::clamp(pts[i], 0.0, 1.0), std::clamp(pts[i + 1], 0.0, 1.0)});
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end(), [](const auto& a, const auto& b) { return std::abs(a.first - b.first) < 1e-6; }), p.end());
    if (p.empty()) {
        for (int i = 0; i < 256; ++i) table[static_cast<std::size_t>(i)] = static_cast<float>(i / 255.0);
        return table;
    }
    if (p.front().first > 0) p.insert(p.begin(), {0.0, p.front().second});
    if (p.back().first < 1) p.push_back({1.0, p.back().second});
    // monotone cubic (Fritsch-Carlson): no overshoot between points
    const std::size_t n = p.size();
    std::vector<double> d(n - 1), m(n);
    for (std::size_t i = 0; i + 1 < n; ++i) d[i] = (p[i + 1].second - p[i].second) / (p[i + 1].first - p[i].first);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (std::abs(d[i]) < 1e-12) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        const double a = m[i] / d[i], b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    std::size_t seg = 0;
    for (int i = 0; i < 256; ++i) {
        const double x = i / 255.0;
        while (seg + 2 < n && x > p[seg + 1].first) ++seg;
        const double h = p[seg + 1].first - p[seg].first;
        const double t = std::clamp((x - p[seg].first) / h, 0.0, 1.0);
        const double t2 = t * t, t3 = t2 * t;
        const double y = (2 * t3 - 3 * t2 + 1) * p[seg].second + (t3 - 2 * t2 + t) * h * m[seg] + (-2 * t3 + 3 * t2) * p[seg + 1].second +
                         (t3 - t2) * h * m[seg + 1];
        table[static_cast<std::size_t>(i)] = static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
    return table;
}

Lut3d parseCube(const std::string& text) {
    Lut3d lut;
    std::istringstream in(text);
    std::string line;
    double dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream ls(line.substr(first));
        if (std::isalpha(static_cast<unsigned char>(line[first]))) {
            std::string key;
            ls >> key;
            if (key == "LUT_3D_SIZE") ls >> lut.size;
            else if (key == "LUT_1D_SIZE") throw std::runtime_error("1D .cube LUTs aren't supported; use a 3D LUT");
            else if (key == "DOMAIN_MIN") ls >> dmin[0] >> dmin[1] >> dmin[2];
            else if (key == "DOMAIN_MAX") ls >> dmax[0] >> dmax[1] >> dmax[2];
            continue;
        }
        float r = 0, g = 0, b = 0;
        if (!(ls >> r >> g >> b)) throw std::runtime_error("bad line in the .cube file: " + line);
        lut.table.insert(lut.table.end(), {r, g, b});
    }
    if (lut.size < 2 || lut.size > 256) throw std::runtime_error("the .cube file has no LUT_3D_SIZE (2..256)");
    const std::size_t want = static_cast<std::size_t>(lut.size) * lut.size * lut.size * 3;
    if (lut.table.size() != want) {
        throw std::runtime_error("the .cube file has " + std::to_string(lut.table.size() / 3) + " entries; LUT_3D_SIZE " +
                                 std::to_string(lut.size) + " needs " + std::to_string(want / 3));
    }
    // a non-default domain: rescale the inputs by remapping the outputs isn't possible, so note it in the values
    (void)dmin;
    (void)dmax;
    return lut;
}

Lut3d loadCube(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("can't read the LUT " + path);
    std::ostringstream s;
    s << f.rdbuf();
    try {
        return parseCube(s.str());
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

namespace {

constexpr double kEps = 1e-9;

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

void applyLut3d(const Lut3d& lut, float rgb[3]) {
    const int n = lut.size;
    const float s = static_cast<float>(n - 1);
    float f[3];
    int i0[3], i1[3];
    for (int k = 0; k < 3; ++k) {
        const float v = std::clamp(rgb[k], 0.0f, 1.0f) * s;
        i0[k] = std::min(n - 2, static_cast<int>(v));
        i1[k] = i0[k] + 1;
        f[k] = v - static_cast<float>(i0[k]);
    }
    auto at = [&](int r, int g, int b, int ch) {
        return lut.table[(static_cast<std::size_t>(b) * n * n + static_cast<std::size_t>(g) * n + static_cast<std::size_t>(r)) * 3 + ch];
    };
    for (int ch = 0; ch < 3; ++ch) {
        const float c00 = at(i0[0], i0[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i0[2], ch) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i0[2], ch) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i1[2], ch) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i1[2], ch) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1];
        const float c1 = c01 * (1 - f[1]) + c11 * f[1];
        rgb[ch] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

bool nonZero(const std::vector<double>& w) {
    return std::any_of(w.begin(), w.end(), [](double v) { return std::abs(v) > kEps; });
}

double wheelAt(const std::vector<double>& w, int ch) {
    const double c = ch < static_cast<int>(w.size()) ? w[static_cast<std::size_t>(ch)] : 0;
    const double m = w.size() > 3 ? w[3] : 0;
    return c + m;
}

}  // namespace

void chromaKey(Image& img, Color key, double similarity, double softness, double spill) {
    const double kr = key.r / 255.0, kg = key.g / 255.0, kb = key.b / 255.0;
    const double ky = 0.2126 * kr + 0.7152 * kg + 0.0722 * kb;
    const double kcb = (kb - ky) / 1.8556, kcr = (kr - ky) / 1.5748;
    const double threshold = std::clamp(similarity, 0.0, 1.0) * 0.5;
    const double soft = std::max(1e-3, std::clamp(softness, 0.0, 1.0) * 0.5);
    // the key's strongest channel is the one spill suppression holds down
    const int dominant = (kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0);
    const int w = img.width();
    raster::parallelRows(img.height(), static_cast<std::size_t>(w), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* p = img.pixel(0, y);
            for (int x = 0; x < w; ++x, p += 4) {
                const double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
                const double yy = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                const double cb = (b - yy) / 1.8556, cr = (r - yy) / 1.5748;
                const double d = std::hypot(cb - kcb, cr - kcr);
                const double a = clamp01((d - threshold) / soft);
                double c[3] = {r, g, b};
                if (spill > 0) {
                    const double o1 = c[(dominant + 1) % 3], o2 = c[(dominant + 2) % 3];
                    const double limit = std::max(o1, o2);
                    if (c[dominant] > limit) c[dominant] -= (c[dominant] - limit) * std::clamp(spill, 0.0, 1.0);
                }
                for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>(std::lround(clamp01(c[k]) * 255));
                p[3] = static_cast<std::uint8_t>(std::lround(p[3] * a));
            }
        }
    });
}

bool applyLook(Image& img, const Clip& c, double local, double pixelScale, const Lut3d* lut) {
    if (img.empty()) return false;
    bool changed = false;
    if (!c.keyColor.empty()) {
        if (auto key = parseColour(c.keyColor)) {
            chromaKey(img, *key, c.keySimilarity, c.keySoftness, c.keySpill);
            changed = true;
        }
    }
    const double exposure = c.value("exposure", local), temperature = c.value("temperature", local), tint = c.value("tint", local);
    const double brightness = c.value("brightness", local), contrast = c.value("contrast", local), gamma = c.value("gamma", local);
    const double saturation = c.grayscale ? 0.0 : c.value("saturation", local);
    const bool wheels = nonZero(c.wheelLift) || nonZero(c.wheelGamma) || nonZero(c.wheelGain);
    const bool curves = !c.curve.empty() || !c.curveR.empty() || !c.curveG.empty() || !c.curveB.empty();
    const bool separable = std::abs(exposure) > kEps || std::abs(temperature) > kEps || std::abs(tint) > kEps || wheels ||
                           std::abs(brightness) > kEps || std::abs(contrast - 1) > kEps || std::abs(gamma - 1) > kEps || curves;
    const bool sat = std::abs(saturation - 1) > kEps;
    if (separable || sat || lut) {
        std::array<std::array<float, 256>, 3> table{};
        const std::vector<float> master = c.curve.empty() ? std::vector<float>() : curveTable(c.curve);
        const std::vector<float> per[3] = {c.curveR.empty() ? std::vector<float>() : curveTable(c.curveR),
                                           c.curveG.empty() ? std::vector<float>() : curveTable(c.curveG),
                                           c.curveB.empty() ? std::vector<float>() : curveTable(c.curveB)};
        const double expo = std::pow(2.0, exposure);
        const double wb[3] = {1 + 0.25 * temperature / 100, 1 - 0.25 * tint / 100, 1 - 0.25 * temperature / 100};
        for (int ch = 0; ch < 3; ++ch) {
            const double gain = 1 + wheelAt(c.wheelGain, ch), lift = wheelAt(c.wheelLift, ch);
            const double gexp = std::pow(2.0, -wheelAt(c.wheelGamma, ch));
            for (int i = 0; i < 256; ++i) {
                double v = i / 255.0;
                v *= expo * wb[ch];
                v *= gain;
                v = v + lift * (1 - v);
                v = std::pow(std::max(0.0, v), gexp);
                v = (v - 0.5) * contrast + 0.5 + brightness;
                v = std::pow(std::max(0.0, v), 1.0 / std::max(0.1, gamma));
                v = clamp01(v);
                auto sample = [](const std::vector<float>& t, double x) {
                    const double f = clamp01(x) * 255;
                    const int a = std::min(254, static_cast<int>(f));
                    return t[static_cast<std::size_t>(a)] * (1 - (f - a)) + t[static_cast<std::size_t>(a) + 1] * (f - a);
                };
                if (!master.empty()) v = sample(master, v);
                if (!per[ch].empty()) v = sample(per[ch], v);
                table[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] = static_cast<float>(v);
            }
        }
        const int w = img.width();
        raster::parallelRows(img.height(), static_cast<std::size_t>(w) * (lut ? 4 : 1), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                std::uint8_t* p = img.pixel(0, y);
                for (int x = 0; x < w; ++x, p += 4) {
                    float rgb[3] = {table[0][p[0]], table[1][p[1]], table[2][p[2]]};
                    if (sat) {
                        const float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
                        for (float& v : rgb) v = l + (v - l) * static_cast<float>(saturation);
                    }
                    if (lut) applyLut3d(*lut, rgb);
                    for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>(std::lround(std::clamp(rgb[k], 0.0f, 1.0f) * 255));
                }
            }
        });
        changed = true;
    }
    const double blur = c.value("blur", local) * pixelScale;
    if (blur > 0.25) {
        raster::gaussianBlur(img, blur);
        changed = true;
    }
    const double sharpen = c.value("sharpen", local);
    if (sharpen > kEps) {
        raster::unsharpMask(img, std::max(1.0, 1.5 * pixelScale), sharpen);
        changed = true;
    }
    const double vignette = c.value("vignette", local);
    if (vignette > kEps) {
        const double cx = img.width() / 2.0, cy = img.height() / 2.0, rmax = std::hypot(cx, cy);
        const int w = img.width();
        raster::parallelRows(img.height(), static_cast<std::size_t>(w), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                std::uint8_t* p = img.pixel(0, y);
                for (int x = 0; x < w; ++x, p += 4) {
                    const double r = std::hypot(x + 0.5 - cx, y + 0.5 - cy) / rmax;
                    const double u = clamp01((r - 0.45) / 0.55);
                    const double k = 1 - std::clamp(vignette, 0.0, 1.0) * u * u * (3 - 2 * u);
                    for (int ch = 0; ch < 3; ++ch) p[ch] = static_cast<std::uint8_t>(std::lround(p[ch] * k));
                }
            }
        });
        changed = true;
    }
    if (c.flipH) {
        img = raster::flipHorizontal(img);
        changed = true;
    }
    if (c.flipV) {
        img = raster::flipVertical(img);
        changed = true;
    }
    return changed;
}

}  // namespace wetwaresuite::cut
