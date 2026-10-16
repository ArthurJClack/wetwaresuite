#include "wetwaresuite/mesh/render.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace wetwaresuite::mesh {

const std::vector<std::string>& viewNames() {
    static const std::vector<std::string> v = {"iso", "front", "back", "left", "right", "top", "bottom"};
    return v;
}

std::optional<View> parseView(const std::string& in) {
    std::string s;
    for (char c : in) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "iso" || s == "isometric") return View::Iso;
    if (s == "front") return View::Front;
    if (s == "back") return View::Back;
    if (s == "left") return View::Left;
    if (s == "right" || s == "side") return View::Right;
    if (s == "top") return View::Top;
    if (s == "bottom") return View::Bottom;
    return std::nullopt;
}

namespace {

struct Camera {
    Vec3 forward, right, up;
};

Camera camera(View v) {
    Vec3 f, worldUp{0, 0, 1};
    switch (v) {
        case View::Iso: f = {-1, 1, -1}; break;
        case View::Front: f = {0, 1, 0}; break;
        case View::Back: f = {0, -1, 0}; break;
        case View::Left: f = {1, 0, 0}; break;
        case View::Right: f = {-1, 0, 0}; break;
        case View::Top: f = {0, 0, -1}; worldUp = {0, 1, 0}; break;
        case View::Bottom: f = {0, 0, 1}; worldUp = {0, -1, 0}; break;
    }
    Camera c;
    c.forward = normalize(f);
    c.right = normalize(cross(c.forward, worldUp));
    c.up = cross(c.right, c.forward);
    return c;
}

std::uint8_t clampByte(double v) { return static_cast<std::uint8_t>(std::min(255.0, std::max(0.0, v) + 0.5)); }

}  // namespace

raster::Image render(const Mesh& input, int width, int height, const RenderOptions& o) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("render size must be positive");
    raster::Image out(width, height, o.background);
    if (input.empty()) return out;

    const int ss = o.antialias ? 2 : 1;
    const int W = width * ss, H = height * ss;
    const Camera cam = camera(o.view);

    Mesh smoothMesh;
    const Mesh* mp = &input;
    if (o.smooth && !input.hasNormals()) {
        smoothMesh = input;
        computeNormals(smoothMesh, 30.0);
        mp = &smoothMesh;
    }
    const Mesh& m = *mp;
    const bool useVertexNormals = o.smooth && m.hasNormals();

    // Project and fit.
    std::vector<Vec3> scr(m.positions.size());
    double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300, minD = 1e300, maxD = -1e300;
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const Vec3& p = m.positions[i];
        const double x = dot(p, cam.right), y = dot(p, cam.up), d = dot(p, cam.forward);
        scr[i] = {x, y, d};
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
        minD = std::min(minD, d); maxD = std::max(maxD, d);
    }
    const double ex = std::max(maxX - minX, 1e-12), ey = std::max(maxY - minY, 1e-12);
    const double avail = 1.0 - 2.0 * std::clamp(o.margin, 0.0, 0.45);
    const double scale = std::min(W * avail / ex, H * avail / ey);
    const double cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    for (auto& s : scr) {
        s.x = (s.x - cx) * scale + W / 2.0;
        s.y = H / 2.0 - (s.y - cy) * scale;
    }

    // Lighting in world space: key light from the upper left of the viewer, plus a soft fill.
    const Vec3 toEye = -cam.forward;
    const Vec3 key = normalize(toEye * 1.0 + cam.up * 0.75 - cam.right * 0.45);
    const Vec3 fill = normalize(toEye * 0.6 - cam.up * 0.3 + cam.right * 0.7);
    const Vec3 half = normalize(key + toEye);
    auto shade = [&](Vec3 n) {
        if (dot(n, toEye) < 0) n = -n;  // two-sided
        const double diff = std::max(0.0, dot(n, key)), f = std::max(0.0, dot(n, fill));
        const double spec = std::pow(std::max(0.0, dot(n, half)), 40.0);
        return Vec3{0.34 + 0.58 * diff + 0.22 * f, spec * 0.22, 0};  // (diffuse factor, specular)
    };

    const std::size_t npx = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<float> depth(npx, std::numeric_limits<float>::infinity());
    std::vector<std::int32_t> faceId(npx, -1);
    std::vector<float> lit(npx * 2, 0.f);
    std::vector<Vec3> faceN(m.triangleCount());

    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        const std::uint32_t i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        const Vec3 &a = scr[i0], &b = scr[i1], &c = scr[i2];
        faceN[t] = normalize(m.faceNormal(t));
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-12) continue;
        Vec3 s0, s1, s2;
        if (useVertexNormals) {
            s0 = shade(m.normals[i0]);
            s1 = shade(m.normals[i1]);
            s2 = shade(m.normals[i2]);
            // Smooth normals can face away on silhouettes; fall back to the face for those.
            const Vec3 sf = shade(faceN[t]);
            if (dot(m.normals[i0], faceN[t]) < 0.2) s0 = sf;
            if (dot(m.normals[i1], faceN[t]) < 0.2) s1 = sf;
            if (dot(m.normals[i2], faceN[t]) < 0.2) s2 = sf;
        } else {
            s0 = s1 = s2 = shade(faceN[t]);
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (x0 > x1 || y0 > y1) continue;
        const double inv = 1.0 / area;
        for (int y = y0; y <= y1; ++y) {
            const double py = y + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5;
                double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
                double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
                double w2 = 1.0 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float d = static_cast<float>(w0 * a.z + w1 * b.z + w2 * c.z);
                const std::size_t k = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                if (d >= depth[k]) continue;
                depth[k] = d;
                faceId[k] = static_cast<std::int32_t>(t);
                lit[k * 2] = static_cast<float>(w0 * s0.x + w1 * s1.x + w2 * s2.x);
                lit[k * 2 + 1] = static_cast<float>(w0 * s0.y + w1 * s1.y + w2 * s2.y);
            }
        }
    }

    // Outlines: silhouettes, sharp creases and depth jumps.
    std::vector<std::uint8_t> edge(npx, 0);
    if (o.outline) {
        const double cosCrease = std::cos(35.0 * kPi / 180.0);
        const float jump = static_cast<float>((maxD - minD) * 0.04 + 1e-9);
        auto check = [&](std::size_t k, std::size_t q) {
            const std::int32_t f = faceId[k], g = faceId[q];
            if (f == g) return;
            bool e = (f < 0) != (g < 0);
            if (!e && f >= 0) {
                e = dot(faceN[static_cast<std::size_t>(f)], faceN[static_cast<std::size_t>(g)]) < cosCrease ||
                    std::fabs(depth[k] - depth[q]) > jump;
            }
            if (e) {
                if (f >= 0) edge[k] = 1;
                if (g >= 0) edge[q] = 1;
            }
        };
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const std::size_t k = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                if (x + 1 < W) check(k, k + 1);
                if (y + 1 < H) check(k, k + static_cast<std::size_t>(W));
            }
        }
    }

    const double br = o.color.r, bg = o.color.g, bb = o.color.b;
    const double ink = 0.25;
    const double ss2 = static_cast<double>(ss * ss);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const std::size_t k = static_cast<std::size_t>(y * ss + sy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x * ss + sx);
                    double pr, pg, pb, pa;
                    if (faceId[k] < 0) {
                        pa = o.background.a / 255.0;
                        pr = o.background.r * pa;
                        pg = o.background.g * pa;
                        pb = o.background.b * pa;
                    } else {
                        const double diff = lit[k * 2], spec = lit[k * 2 + 1] * 255.0;
                        pr = br * diff + spec;
                        pg = bg * diff + spec;
                        pb = bb * diff + spec;
                        if (edge[k]) {
                            pr = pr * ink + 30 * (1 - ink);
                            pg = pg * ink + 36 * (1 - ink);
                            pb = pb * ink + 48 * (1 - ink);
                        }
                        pa = 1.0;
                    }
                    r += pr; g += pg; b += pb; a += pa;
                }
            }
            raster::Color c;
            if (a <= 0) {
                c = {o.background.r, o.background.g, o.background.b, 0};
            } else {
                c = {clampByte(r / a), clampByte(g / a), clampByte(b / a), clampByte(a / ss2 * 255.0)};
            }
            out.set(x, y, c);
        }
    }
    return out;
}

}  // namespace wetwaresuite::mesh
