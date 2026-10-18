#include "wetwaresuite/raster/selection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>

#include "wetwaresuite/raster/parallel.hpp"

namespace wetwaresuite::raster {

const std::vector<std::string>& maskOpNames() {
    static const std::vector<std::string> names = {"replace", "add", "subtract", "intersect"};
    return names;
}

std::optional<MaskOp> parseMaskOp(const std::string& name) {
    if (name == "replace" || name == "new") return MaskOp::Replace;
    if (name == "add" || name == "union") return MaskOp::Add;
    if (name == "subtract" || name == "remove") return MaskOp::Subtract;
    if (name == "intersect") return MaskOp::Intersect;
    return std::nullopt;
}

Mask rectMask(int width, int height, int x, int y, int w, int h) {
    Mask m(width, height);
    const int x0 = std::clamp(x, 0, width), x1 = std::clamp(x + w, 0, width);
    const int y0 = std::clamp(y, 0, height), y1 = std::clamp(y + h, 0, height);
    for (int row = y0; row < y1; ++row) std::fill(m.row(row) + x0, m.row(row) + x1, std::uint8_t{255});
    return m;
}

Mask ellipseMask(int width, int height, double x, double y, double w, double h, bool antialias) {
    Mask m(width, height);
    const double a = w / 2, b = h / 2, cx = x + a, cy = y + b;
    if (a <= 0 || b <= 0) return m;
    const int y0 = std::max(0, static_cast<int>(std::floor(y)) - 1), y1 = std::min(height, static_cast<int>(std::ceil(y + h)) + 1);
    const int x0 = std::max(0, static_cast<int>(std::floor(x)) - 1), x1 = std::min(width, static_cast<int>(std::ceil(x + w)) + 1);
    const double k = std::min(a, b);
    for (int row = y0; row < y1; ++row) {
        const double dy = (row + 0.5 - cy) / b;
        for (int col = x0; col < x1; ++col) {
            const double dx = (col + 0.5 - cx) / a;
            const double r = std::sqrt(dx * dx + dy * dy);
            double cov;
            if (antialias) cov = std::clamp(0.5 - (r - 1) * k, 0.0, 1.0);
            else cov = r <= 1 ? 1 : 0;
            m.row(row)[col] = static_cast<std::uint8_t>(std::lround(cov * 255));
        }
    }
    return m;
}

Mask polygonMask(int width, int height, const std::vector<double>& pts, bool antialias) {
    Mask m(width, height);
    const std::size_t n = pts.size() / 2;
    if (n < 3) return m;
    double minY = pts[1], maxY = pts[1];
    for (std::size_t i = 0; i < n; ++i) minY = std::min(minY, pts[i * 2 + 1]), maxY = std::max(maxY, pts[i * 2 + 1]);
    const int y0 = std::max(0, static_cast<int>(std::floor(minY))), y1 = std::min(height, static_cast<int>(std::ceil(maxY)) + 1);
    const int subs = antialias ? 4 : 1;
    std::vector<float> cov(static_cast<std::size_t>(width));
    std::vector<double> xs;
    for (int row = y0; row < y1; ++row) {
        std::fill(cov.begin(), cov.end(), 0.0f);
        bool any = false;
        for (int s = 0; s < subs; ++s) {
            const double sy = row + (antialias ? (s + 0.5) / subs : 0.5);
            xs.clear();
            for (std::size_t i = 0; i < n; ++i) {
                const double ax = pts[i * 2], ay = pts[i * 2 + 1];
                const double bx = pts[((i + 1) % n) * 2], by = pts[((i + 1) % n) * 2 + 1];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) xs.push_back(ax + (sy - ay) / (by - ay) * (bx - ax));
            }
            std::sort(xs.begin(), xs.end());
            for (std::size_t i = 0; i + 1 < xs.size(); i += 2) {
                double xa = std::clamp(xs[i], 0.0, static_cast<double>(width)), xb = std::clamp(xs[i + 1], 0.0, static_cast<double>(width));
                if (xb <= xa) continue;
                any = true;
                const float wgt = 1.0f / static_cast<float>(subs);
                if (!antialias) {
                    const int c0 = static_cast<int>(std::ceil(xa - 0.5)), c1 = static_cast<int>(std::ceil(xb - 0.5));
                    for (int c = std::max(0, c0); c < std::min(width, c1); ++c) cov[static_cast<std::size_t>(c)] = 1;
                    continue;
                }
                const int ia = static_cast<int>(xa), ib = std::min(width - 1, static_cast<int>(xb));
                if (ia == ib) {
                    cov[static_cast<std::size_t>(ia)] += static_cast<float>(xb - xa) * wgt;
                } else {
                    cov[static_cast<std::size_t>(ia)] += static_cast<float>(ia + 1 - xa) * wgt;
                    for (int c = ia + 1; c < ib; ++c) cov[static_cast<std::size_t>(c)] += wgt;
                    cov[static_cast<std::size_t>(ib)] += static_cast<float>(xb - ib) * wgt;
                }
            }
        }
        if (!any) continue;
        std::uint8_t* out = m.row(row);
        for (int c = 0; c < width; ++c) out[c] = static_cast<std::uint8_t>(std::lround(std::clamp(cov[static_cast<std::size_t>(c)], 0.0f, 1.0f) * 255));
    }
    return m;
}

namespace {

bool near(const std::uint8_t* a, const std::uint8_t* b, int tol) {
    for (int c = 0; c < 4; ++c) {
        if (std::abs(a[c] - b[c]) > tol) return false;
    }
    return true;
}

}  // namespace

Mask wandMask(const Image& img, int x, int y, int tolerance, bool contiguous) {
    Mask m(img.width(), img.height());
    if (!img.contains(x, y)) return m;
    const std::uint8_t seed[4] = {img.pixel(x, y)[0], img.pixel(x, y)[1], img.pixel(x, y)[2], img.pixel(x, y)[3]};
    const int w = img.width(), h = img.height();
    if (!contiguous) {
        parallelRows(h, static_cast<std::size_t>(w) * 4, [&](int b, int e) {
            for (int row = b; row < e; ++row) {
                for (int col = 0; col < w; ++col) {
                    if (near(img.pixel(col, row), seed, tolerance)) m.row(row)[col] = 255;
                }
            }
        });
        return m;
    }
    // scanline flood fill
    std::vector<std::pair<int, int>> stack{{x, y}};
    while (!stack.empty()) {
        auto [sx, sy] = stack.back();
        stack.pop_back();
        if (m.row(sy)[sx] || !near(img.pixel(sx, sy), seed, tolerance)) continue;
        int l = sx, r = sx;
        while (l > 0 && !m.row(sy)[l - 1] && near(img.pixel(l - 1, sy), seed, tolerance)) --l;
        while (r < w - 1 && !m.row(sy)[r + 1] && near(img.pixel(r + 1, sy), seed, tolerance)) ++r;
        std::fill(m.row(sy) + l, m.row(sy) + r + 1, std::uint8_t{255});
        for (int ny : {sy - 1, sy + 1}) {
            if (ny < 0 || ny >= h) continue;
            bool inRun = false;
            for (int c = l; c <= r; ++c) {
                const bool ok = !m.row(ny)[c] && near(img.pixel(c, ny), seed, tolerance);
                if (ok && !inRun) stack.emplace_back(c, ny);
                inRun = ok;
            }
        }
    }
    return m;
}

Mask colorRangeMask(const Image& img, Color color, int tolerance, int fuzz) {
    Mask m(img.width(), img.height());
    const int want[3] = {color.r, color.g, color.b};
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 4, [&](int b, int e) {
        for (int row = b; row < e; ++row) {
            for (int col = 0; col < img.width(); ++col) {
                const std::uint8_t* p = img.pixel(col, row);
                int d = 0;
                for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(p[c] - want[c]));
                int v = 0;
                if (d <= tolerance) v = 255;
                else if (fuzz > 0 && d < tolerance + fuzz) v = 255 - (d - tolerance) * 255 / fuzz;
                m.row(row)[col] = static_cast<std::uint8_t>(v * p[3] / 255);
            }
        }
    });
    return m;
}

Mask alphaMask(const Image& img) {
    Mask m(img.width(), img.height());
    for (int row = 0; row < img.height(); ++row) {
        for (int col = 0; col < img.width(); ++col) m.row(row)[col] = img.pixel(col, row)[3];
    }
    return m;
}

void combine(Mask& base, const Mask& shape, MaskOp op) {
    if (op == MaskOp::Replace || base.empty()) {
        if (op == MaskOp::Replace || op == MaskOp::Add) base = shape;
        else base = Mask(shape.width, shape.height);  // subtract/intersect from nothing leaves nothing
        return;
    }
    const std::size_t n = std::min(base.v.size(), shape.v.size());
    for (std::size_t i = 0; i < n; ++i) {
        const int a = base.v[i], b = shape.v[i];
        int v = a;
        switch (op) {
            case MaskOp::Add: v = std::max(a, b); break;
            case MaskOp::Subtract: v = a * (255 - b) / 255; break;
            case MaskOp::Intersect: v = std::min(a, b); break;
            case MaskOp::Replace: v = b; break;
        }
        base.v[i] = static_cast<std::uint8_t>(v);
    }
}

void invert(Mask& m) {
    for (auto& v : m.v) v = static_cast<std::uint8_t>(255 - v);
}

namespace {

// Sliding max (or min) over a window of 2r+1 along one line.
void slideLine(const std::uint8_t* in, std::uint8_t* out, int n, std::ptrdiff_t stride, int r, bool takeMax) {
    std::deque<int> q;
    auto better = [&](int a, int b) { return takeMax ? in[a * stride] >= in[b * stride] : in[a * stride] <= in[b * stride]; };
    int next = 0;
    for (int i = 0; i < n; ++i) {
        while (next < n && next <= i + r) {
            while (!q.empty() && better(next, q.back())) q.pop_back();
            q.push_back(next++);
        }
        while (q.front() < i - r) q.pop_front();
        out[i * stride] = in[q.front() * stride];
    }
}

void boxLine(const std::uint8_t* in, std::uint8_t* out, int n, std::ptrdiff_t stride, int r) {
    long sum = 0;
    const int span = 2 * r + 1;
    auto at = [&](int i) { return static_cast<long>(in[std::clamp(i, 0, n - 1) * stride]); };
    for (int i = -r; i <= r; ++i) sum += at(i);
    for (int i = 0; i < n; ++i) {
        out[i * stride] = static_cast<std::uint8_t>((sum + span / 2) / span);
        sum += at(i + r + 1) - at(i - r);
    }
}

template <typename LineFn>
void separable(Mask& m, LineFn fn) {
    if (m.empty()) return;
    const int w = m.width, h = m.height;
    std::vector<std::uint8_t> tmp(m.v.size());
    parallelRows(h, static_cast<std::size_t>(w), [&](int b, int e) {
        for (int y = b; y < e; ++y) fn(m.row(y), tmp.data() + static_cast<std::size_t>(y) * w, w, 1);
    });
    parallelRows(w, static_cast<std::size_t>(h), [&](int b, int e) {
        for (int x = b; x < e; ++x) fn(tmp.data() + x, m.v.data() + x, h, w);
    });
}

}  // namespace

void grow(Mask& m, int radius) {
    if (radius == 0) return;
    const int r = std::abs(radius);
    const bool takeMax = radius > 0;
    separable(m, [&](const std::uint8_t* in, std::uint8_t* out, int n, std::ptrdiff_t stride) { slideLine(in, out, n, stride, r, takeMax); });
}

void feather(Mask& m, double radius) {
    if (radius <= 0) return;
    const int r = std::max(1, static_cast<int>(std::lround(radius / 2)));
    for (int pass = 0; pass < 3; ++pass) {
        separable(m, [&](const std::uint8_t* in, std::uint8_t* out, int n, std::ptrdiff_t stride) { boxLine(in, out, n, stride, r); });
    }
}

Mask border(const Mask& m, int width) {
    Mask outer = m, inner = m;
    grow(outer, std::max(1, (width + 1) / 2));
    grow(inner, -std::max(1, width / 2));
    combine(outer, inner, MaskOp::Subtract);
    return outer;
}

bool anySelected(const Mask& m) {
    return std::any_of(m.v.begin(), m.v.end(), [](std::uint8_t v) { return v != 0; });
}

PixelRect bounds(const Mask& m) {
    PixelRect r{m.width, m.height, 0, 0};
    for (int y = 0; y < m.height; ++y) {
        const std::uint8_t* row = m.row(y);
        for (int x = 0; x < m.width; ++x) {
            if (!row[x]) continue;
            r.x0 = std::min(r.x0, x), r.x1 = std::max(r.x1, x + 1);
            r.y0 = std::min(r.y0, y), r.y1 = std::max(r.y1, y + 1);
        }
    }
    if (r.empty()) return {};
    return r;
}

double coverage(const Mask& m) {
    if (m.empty()) return 0;
    double sum = 0;
    for (auto v : m.v) sum += v;
    return sum / (255.0 * static_cast<double>(m.v.size()));
}

void keepInside(Image& edited, const Image& before, const Mask& m, PixelRect rect) {
    const int w = std::min({edited.width(), before.width(), m.width}), h = std::min({edited.height(), before.height(), m.height});
    if (rect.empty()) rect = {0, 0, w, h};
    rect = {std::max(0, rect.x0), std::max(0, rect.y0), std::min(w, rect.x1), std::min(h, rect.y1)};
    if (rect.empty()) return;
    parallelRows(rect.height(), static_cast<std::size_t>(rect.width()) * 4, [&](int b, int e) {
        for (int y = rect.y0 + b; y < rect.y0 + e; ++y) {
            const std::uint8_t* mr = m.row(y);
            for (int x = rect.x0; x < rect.x1; ++x) {
                const int k = mr[x];
                if (k == 255) continue;
                std::uint8_t* d = edited.pixel(x, y);
                const std::uint8_t* o = before.pixel(x, y);
                for (int c = 0; c < 4; ++c) d[c] = static_cast<std::uint8_t>(o[c] + ((d[c] - o[c]) * k + (d[c] >= o[c] ? 127 : -127)) / 255);
            }
        }
    });
}

void applyMask(Image& img, const Mask& m) {
    const int w = std::min(img.width(), m.width), h = std::min(img.height(), m.height);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            std::uint8_t* p = img.pixel(x, y);
            p[3] = static_cast<std::uint8_t>(p[3] * m.at(x, y) / 255);
        }
    }
}

Image maskToImage(const Mask& m) {
    Image img(m.width, m.height);
    for (int y = 0; y < m.height; ++y) {
        for (int x = 0; x < m.width; ++x) {
            std::uint8_t* p = img.pixel(x, y);
            p[0] = p[1] = p[2] = 255;
            p[3] = m.at(x, y);
        }
    }
    return img;
}

Mask imageToMask(const Image& img) { return alphaMask(img); }

std::array<std::uint8_t, 256> curveTable(const std::vector<std::pair<double, double>>& input) {
    std::vector<std::pair<double, double>> pts;
    for (auto [x, y] : input) pts.emplace_back(std::clamp(x, 0.0, 255.0), std::clamp(y, 0.0, 255.0));
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end(), [](auto& a, auto& b) { return a.first == b.first; }), pts.end());
    if (pts.empty()) pts = {{0, 0}, {255, 255}};
    if (pts.size() == 1) pts.insert(pts.begin(), {0, 0}), pts.emplace_back(255, 255);
    const std::size_t n = pts.size();
    // Fritsch-Carlson monotone cubic: slopes that keep each piece within its end points
    std::vector<double> d(n - 1), m(n);
    for (std::size_t i = 0; i + 1 < n; ++i) d[i] = (pts[i + 1].second - pts[i].second) / (pts[i + 1].first - pts[i].first);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        const double a = m[i] / d[i], b = m[i + 1] / d[i], s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    std::array<std::uint8_t, 256> lut{};
    for (int x = 0; x < 256; ++x) {
        double y;
        if (x <= pts.front().first) y = pts.front().second;
        else if (x >= pts.back().first) y = pts.back().second;
        else {
            std::size_t i = 0;
            while (i + 2 < n && x > pts[i + 1].first) ++i;
            const double h = pts[i + 1].first - pts[i].first, t = (x - pts[i].first) / h;
            const double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * pts[i].second + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * pts[i + 1].second +
                (t3 - t2) * h * m[i + 1];
        }
        lut[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(std::lround(std::clamp(y, 0.0, 255.0)));
    }
    return lut;
}

void applyCurves(Image& img, const std::array<std::uint8_t, 256>& r, const std::array<std::uint8_t, 256>& g,
                 const std::array<std::uint8_t, 256>& b) {
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* p = img.pixel(0, y);
            for (int x = 0; x < img.width(); ++x, p += 4) p[0] = r[p[0]], p[1] = g[p[1]], p[2] = b[p[2]];
        }
    });
}

void colorBalance(Image& img, ColorShift sh, ColorShift mid, ColorShift hi, bool preserveLuminosity) {
    parallelRows(img.height(), static_cast<std::size_t>(img.width()) * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::uint8_t* p = img.pixel(0, y);
            for (int x = 0; x < img.width(); ++x, p += 4) {
                const double r = p[0], g = p[1], b = p[2];
                const double l = (std::max({r, g, b}) + std::min({r, g, b})) / 510.0;
                const double ws = (1 - l) * (1 - l), wh = l * l, wm = 1 - ws - wh;
                const double k = 0.8;  // 100 moves a channel by up to ~0.8 * 255 * weight
                double nr = r + (sh.cyanRed * ws + mid.cyanRed * wm + hi.cyanRed * wh) * k * 2.55;
                double ng = g + (sh.magentaGreen * ws + mid.magentaGreen * wm + hi.magentaGreen * wh) * k * 2.55;
                double nb = b + (sh.yellowBlue * ws + mid.yellowBlue * wm + hi.yellowBlue * wh) * k * 2.55;
                nr = std::clamp(nr, 0.0, 255.0), ng = std::clamp(ng, 0.0, 255.0), nb = std::clamp(nb, 0.0, 255.0);
                if (preserveLuminosity) {
                    const double before = 0.299 * r + 0.587 * g + 0.114 * b, after = 0.299 * nr + 0.587 * ng + 0.114 * nb;
                    const double shift = before - after;
                    nr = std::clamp(nr + shift, 0.0, 255.0), ng = std::clamp(ng + shift, 0.0, 255.0), nb = std::clamp(nb + shift, 0.0, 255.0);
                }
                p[0] = static_cast<std::uint8_t>(std::lround(nr));
                p[1] = static_cast<std::uint8_t>(std::lround(ng));
                p[2] = static_cast<std::uint8_t>(std::lround(nb));
            }
        }
    });
}

}  // namespace wetwaresuite::raster
