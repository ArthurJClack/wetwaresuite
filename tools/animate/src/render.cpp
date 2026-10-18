// Drawing a frame. 2D layers: shapes are transformed polygons filled with
// libs/raster's antialiased fill; text, pictures and cels are sprites drawn
// through their affine transform with bilinear sampling. 3D layers: each mesh
// is put in camera space, perspective-projected and clipped to the frame,
// then drawn by libs/mesh's preview renderer (front view, pinned to the frame
// so it doesn't auto-fit) and stacked far to near.
#include <algorithm>
#include <cmath>
#include <list>
#include <tuple>

#include "achclwss/animate/animate.hpp"
#include "achclwss/mesh/render.hpp"
#include "achclwss/raster/ops.hpp"
#include "achclwss/raster/paint.hpp"
#include "internal.hpp"

namespace achclwss::animate {

namespace {

using mesh::Vec3;
using raster::Point;

void over(std::uint8_t* d, double r, double g, double b, double a) {
    if (a <= 0) return;
    a = std::min(a, 1.0);
    const double da = d[3] / 255.0;
    const double oa = a + da * (1 - a);
    if (oa <= 0) return;
    auto mix = [&](std::uint8_t dv, double sv) {
        return static_cast<std::uint8_t>(std::clamp((sv * a + dv * da * (1 - a)) / oa, 0.0, 255.0) + 0.5);
    };
    d[0] = mix(d[0], r);
    d[1] = mix(d[1], g);
    d[2] = mix(d[2], b);
    d[3] = static_cast<std::uint8_t>(oa * 255.0 + 0.5);
}

struct Tint {
    Color multiply{255, 255, 255, 255};
    std::optional<Color> colorize;  // onion skins: pull every pixel towards this colour
    double opacity = 1;
};

// Draws `src` with `m` mapping its pixel coords to dst pixel coords.
void blit(Image& dst, const Image& src, const Affine& m, const Tint& tint) {
    if (src.empty() || tint.opacity <= 0) return;
    const double sw = src.width(), sh = src.height();
    double xs[4], ys[4];
    m.apply(0, 0, xs[0], ys[0]);
    m.apply(sw, 0, xs[1], ys[1]);
    m.apply(sw, sh, xs[2], ys[2]);
    m.apply(0, sh, xs[3], ys[3]);
    const int x0 = std::max(0, static_cast<int>(std::floor(*std::min_element(xs, xs + 4))));
    const int x1 = std::min(dst.width(), static_cast<int>(std::ceil(*std::max_element(xs, xs + 4))) + 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(*std::min_element(ys, ys + 4))));
    const int y1 = std::min(dst.height(), static_cast<int>(std::ceil(*std::max_element(ys, ys + 4))) + 1);
    if (x0 >= x1 || y0 >= y1) return;
    const Affine inv = m.inverse();
    const int W = src.width(), H = src.height();
    const double mr = tint.multiply.r / 255.0, mg = tint.multiply.g / 255.0, mb = tint.multiply.b / 255.0;
    const double op = tint.opacity * tint.multiply.a / 255.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            double u, v;
            inv.apply(x + 0.5, y + 0.5, u, v);
            u -= 0.5;
            v -= 0.5;
            if (u < -1 || v < -1 || u > W || v > H) continue;
            const int iu = static_cast<int>(std::floor(u)), iv = static_cast<int>(std::floor(v));
            const double fu = u - iu, fv = v - iv;
            double acc[4] = {0, 0, 0, 0};  // premultiplied
            for (int k = 0; k < 4; ++k) {
                const int sx = iu + (k & 1), sy = iv + (k >> 1);
                if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
                const double wgt = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
                const std::uint8_t* p = src.pixel(sx, sy);
                const double a = p[3] / 255.0 * wgt;
                acc[0] += p[0] * a;
                acc[1] += p[1] * a;
                acc[2] += p[2] * a;
                acc[3] += a;
            }
            if (acc[3] <= 1e-6) continue;
            double r = acc[0] / acc[3] * mr, g = acc[1] / acc[3] * mg, b = acc[2] / acc[3] * mb;
            if (tint.colorize) {
                r = r * 0.3 + tint.colorize->r * 0.7;
                g = g * 0.3 + tint.colorize->g * 0.7;
                b = b * 0.3 + tint.colorize->b * 0.7;
            }
            over(dst.pixel(x, y), r, g, b, acc[3] * op);
        }
    }
}

// Pictures shrunk a lot are pre-scaled (area-averaged) so they don't sparkle; a few are cached.
std::shared_ptr<const Image> fitted(const std::shared_ptr<const Image>& img, double pxW, double pxH) {
    if (!img || img->empty()) return img;
    const int tw = std::max(1, static_cast<int>(std::lround(pxW))), th = std::max(1, static_cast<int>(std::lround(pxH)));
    if (tw * 2 > img->width() && th * 2 > img->height()) return img;
    thread_local std::list<std::tuple<const Image*, int, int, std::shared_ptr<const Image>, std::shared_ptr<const Image>>> cache;
    for (auto it = cache.begin(); it != cache.end(); ++it) {
        if (std::get<0>(*it) == img.get() && std::get<1>(*it) == tw && std::get<2>(*it) == th) {
            auto out = std::get<4>(*it);
            cache.splice(cache.begin(), cache, it);
            return out;
        }
    }
    auto out = std::make_shared<const Image>(raster::resize(*img, tw, th));
    cache.emplace_front(img.get(), tw, th, img, out);  // keeping `img` alive keeps the pointer key unique
    if (cache.size() > 32) cache.pop_back();
    return out;
}

Color withOpacity(Color c, double opacity) {
    c.a = static_cast<std::uint8_t>(std::clamp(c.a * opacity, 0.0, 255.0) + 0.5);
    return c;
}

std::vector<Point> transformed(const Affine& m, const std::vector<Point>& pts) {
    std::vector<Point> out;
    out.reserve(pts.size());
    for (const auto& p : pts) {
        Point q;
        m.apply(p.x, p.y, q.x, q.y);
        out.push_back(q);
    }
    return out;
}

void strokePath(Image& img, const std::vector<Point>& pts, bool closed, Color c, double width) {
    if (pts.size() < 2 || width <= 0 || c.a == 0) return;
    raster::Brush b;
    b.color = c;
    b.size = std::max(0.5, width);
    b.hardness = 1.0;
    b.spacing = 0.05;
    b.pressureSize = false;
    std::vector<raster::StrokePoint> sp;
    for (const auto& p : pts) sp.push_back({p.x, p.y, 1.0});
    if (closed) sp.push_back({pts[0].x, pts[0].y, 1.0});
    raster::stroke(img, b, sp);
}

void drawCel(Image& out, const Affine& m, double w, double h, const std::shared_ptr<const Image>& drawing, const Tint& tint) {
    if (!drawing || drawing->empty()) return;
    const double k = m.scale();
    auto pic = fitted(drawing, w * k, h * k);
    const Affine toBox{w / pic->width(), 0, 0, h / pic->height(), 0, 0};
    blit(out, *pic, m * toBox, tint);
}

void drawElement2d(Image& out, const AnimDoc& doc, const Element& e, int frame, const FrameOptions& o) {
    const double f = frame;
    const double opacity = std::clamp(e.numbers.count("opacity") ? numberAt(e, "opacity", f) : 1.0, 0.0, 1.0);
    if (opacity <= 0 || e.kind == "bone") return;
    const Affine m = Affine{o.scale, 0, 0, o.scale, 0, 0} * worldTransform2d(doc, e, f);
    const double k = m.scale();
    double w = 0, h = 0;
    elementSize(e, f, w, h);
    if (e.kind == "rect" || e.kind == "ellipse" || e.kind == "polygon") {
        std::vector<Point> pts;
        if (e.kind == "rect") {
            pts = {{0, 0}, {w, 0}, {w, h}, {0, h}};
        } else if (e.kind == "ellipse") {
            const int n = std::clamp(static_cast<int>(std::max(w, h) * k * 0.8), 24, 720);
            for (int i = 0; i < n; ++i) {
                const double t = 2 * mesh::kPi * i / n;
                pts.push_back({w / 2 + w / 2 * std::cos(t), h / 2 + h / 2 * std::sin(t)});
            }
        } else {
            for (std::size_t i = 0; i + 1 < e.points.size(); i += 2) pts.push_back({e.points[i], e.points[i + 1]});
        }
        if (pts.size() < 3) return;
        const auto world = transformed(m, pts);
        raster::fillPolygon(out, world, withOpacity(colorAt(e, "color", f), opacity));
        const double sw = numberAt(e, "stroke_width", f);
        if (sw > 0) strokePath(out, world, true, withOpacity(colorAt(e, "stroke", f), opacity), sw * k);
    } else if (e.kind == "line") {
        std::vector<Point> pts;
        for (std::size_t i = 0; i + 1 < e.points.size(); i += 2) pts.push_back({e.points[i], e.points[i + 1]});
        strokePath(out, transformed(m, pts), false, withOpacity(colorAt(e, "color", f), opacity), numberAt(e, "stroke_width", f) * k);
    } else if (e.kind == "text") {
        if (e.text.empty()) return;
        const double ss = std::clamp(k, 0.25, 8.0);
        Image sprite = detail::renderText(e, numberAt(e, "font_size", f), colorAt(e, "color", f), ss);
        Tint t;
        t.opacity = opacity;
        blit(out, sprite, m * Affine{1 / ss, 0, 0, 1 / ss, 0, 0}, t);
    } else if (e.kind == "image") {
        if (!e.picture || e.picture->empty() || w <= 0 || h <= 0) return;
        auto pic = fitted(e.picture, w * k, h * k);
        Tint t;
        t.multiply = colorAt(e, "color", f);
        t.opacity = opacity;
        blit(out, *pic, m * Affine{w / pic->width(), 0, 0, h / pic->height(), 0, 0}, t);
    } else if (e.kind == "cel") {
        if (w <= 0 || h <= 0) return;
        int start = 0;
        auto current = celAt(e, frame, &start);
        if (o.onion > 0) {
            const bool has = current != nullptr;
            int n = 0;
            for (auto it = e.cels.lower_bound(has ? start : frame); it != e.cels.begin() && n < o.onion;) {
                --it;
                ++n;
                Tint t;
                t.colorize = Color{230, 60, 60, 255};
                t.opacity = o.onionOpacity * (1.0 - 0.5 * (n - 1) / std::max(1, o.onion));
                drawCel(out, m, w, h, it->second, t);
            }
            n = 0;
            for (auto it = e.cels.upper_bound(frame); it != e.cels.end() && n < o.onion; ++it) {
                ++n;
                Tint t;
                t.colorize = Color{40, 170, 90, 255};
                t.opacity = o.onionOpacity * (1.0 - 0.5 * (n - 1) / std::max(1, o.onion));
                drawCel(out, m, w, h, it->second, t);
            }
        }
        if (current) {
            Tint t;
            t.multiply = colorAt(e, "color", f);
            t.opacity = opacity;
            drawCel(out, m, w, h, current, t);
        }
    }
}

void drawBones(Image& out, const AnimDoc& doc, int frame, const FrameOptions& o) {
    for (const auto& l : doc.layers) {
        if (!l.visible || l.kind != "2d") continue;
        for (const auto& e : l.elements) {
            if (e.kind != "bone") continue;
            const Affine m = Affine{o.scale, 0, 0, o.scale, 0, 0} * worldTransform2d(doc, e, frame);
            double x0, y0, x1, y1;
            m.apply(0, 0, x0, y0);
            m.apply(numberAt(e, "length", frame), 0, x1, y1);
            const Color c = colorAt(e, "color", frame);
            const double len = std::hypot(x1 - x0, y1 - y0);
            if (len > 1) {
                // a long thin diamond from the joint to the tip, like most rigging tools draw them
                const double ux = (x1 - x0) / len, uy = (y1 - y0) / len;
                const double wdt = std::clamp(len * 0.12, 2.0, 10.0);
                const double mx = x0 + ux * len * 0.2, my = y0 + uy * len * 0.2;
                std::vector<Point> diamond = {{x0, y0}, {mx - uy * wdt, my + ux * wdt}, {x1, y1}, {mx + uy * wdt, my - ux * wdt}};
                raster::fillPolygon(out, diamond, withOpacity(c, 0.55));
                strokePath(out, diamond, true, c, 1.5);
            }
            raster::fillEllipse(out, x0, y0, 4, 4, c);
            raster::fillEllipse(out, x0, y0, 2, 2, Color{255, 255, 255, 255});
        }
    }
}

// ---- 3D ----

struct Cam {
    Vec3 eye, f, r, u;
    double tanHalf = 0.36;
};

Cam makeCam(const Vec3& eye, const Vec3& target, double fov) {
    Cam c;
    c.eye = eye;
    c.f = mesh::normalize(target - eye);
    if (mesh::length(c.f) < 1e-9) c.f = {0, 1, 0};
    Vec3 up{0, 0, 1};
    if (mesh::length(mesh::cross(c.f, up)) < 1e-6) up = {0, 1, 0};
    c.r = mesh::normalize(mesh::cross(c.f, up));
    c.u = mesh::cross(c.r, c.f);
    c.tanHalf = std::tan(std::clamp(fov, 1.0, 170.0) * mesh::kPi / 360.0);
    return c;
}

struct CV {  // a clip-space vertex: camera coords and the camera-space normal
    Vec3 p;
    Vec3 n;
};

CV lerpCV(const CV& a, const CV& b, double t) { return {mesh::lerp(a.p, b.p, t), mesh::lerp(a.n, b.n, t)}; }

// Sutherland-Hodgman against one plane: keep where dist(v) >= 0.
template <typename D>
std::vector<CV> clipPlane(const std::vector<CV>& poly, D dist) {
    std::vector<CV> out;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const CV& a = poly[i];
        const CV& b = poly[(i + 1) % poly.size()];
        const double da = dist(a), db = dist(b);
        if (da >= 0) out.push_back(a);
        if ((da >= 0) != (db >= 0)) out.push_back(lerpCV(a, b, da / (da - db)));
    }
    return out;
}

void renderLayer3d(Image& out, const AnimDoc& doc, const Layer& layer, int frame) {
    const double f = frame;
    const int W = out.width(), H = out.height();
    const double aspect = static_cast<double>(W) / H;
    // camera: the layer's first visible camera, or one framing every mesh at frame 0
    Cam cam;
    const Element* camEl = nullptr;
    for (const auto& e : layer.elements) {
        if (e.kind == "camera" && e.visible) {
            camEl = &e;
            break;
        }
    }
    if (camEl) {
        const Vec3 eye = worldTransform3d(doc, *camEl, f).point({0, 0, 0});
        cam = makeCam(eye, {numberAt(*camEl, "target_x", f), numberAt(*camEl, "target_y", f), numberAt(*camEl, "target_z", f)},
                      numberAt(*camEl, "fov", f));
    } else {
        mesh::Bounds b;
        for (const auto& e : layer.elements) {
            if (e.kind != "mesh" || !e.geometry) continue;
            const mesh::Mat4 m = worldTransform3d(doc, e, 0);
            const mesh::Bounds mb = mesh::bounds(*e.geometry);
            for (int i = 0; i < 8; ++i) {
                b.add(m.point({(i & 1) ? mb.max.x : mb.min.x, (i & 2) ? mb.max.y : mb.min.y, (i & 4) ? mb.max.z : mb.min.z}));
            }
        }
        if (b.empty()) return;
        const double radius = std::max(b.diagonal() / 2, 1e-3);
        const double fov = 40;
        const double dist = radius / std::sin(fov * mesh::kPi / 360.0) * 1.05;
        cam = makeCam(b.center() + mesh::normalize(Vec3{1, -1.6, 0.9}) * dist, b.center(), fov);
    }
    const double near = 1e-3;
    struct Drawn {
        double depth;
        double opacity;
        Image image;
    };
    std::vector<Drawn> drawn;
    for (const auto& e : layer.elements) {
        if (e.kind != "mesh" || !e.visible || !e.geometry) continue;
        const double opacity = std::clamp(numberAt(e, "opacity", f), 0.0, 1.0);
        if (opacity <= 0) continue;
        const mesh::Mat4 m = worldTransform3d(doc, e, f);
        const mesh::Mesh& g = *e.geometry;
        auto toCam = [&](const Vec3& world) {
            const Vec3 c = world - cam.eye;
            return Vec3{mesh::dot(c, cam.r), mesh::dot(c, cam.u), mesh::dot(c, cam.f)};
        };
        const double zref = std::max(near * 10, toCam(m.point(mesh::bounds(g).center())).z);
        mesh::Mesh outMesh;
        for (std::size_t t = 0; t < g.triangleCount(); ++t) {
            std::vector<CV> poly;
            for (int k = 0; k < 3; ++k) {
                const std::uint32_t i = g.indices[t * 3 + static_cast<std::size_t>(k)];
                const Vec3 n = g.hasNormals() ? m.normal(g.normals[i]) : Vec3{0, 0, 1};
                poly.push_back({toCam(m.point(g.positions[i])), {mesh::dot(n, cam.r), mesh::dot(n, cam.u), mesh::dot(n, cam.f)}});
            }
            poly = clipPlane(poly, [&](const CV& v) { return v.p.z - near; });
            if (poly.size() < 3) continue;
            // project to the frame: x in [-aspect, aspect], y in [-1, 1]; depth scaled to match near the object
            for (auto& v : poly) {
                const double z = v.p.z;
                v.p = {v.p.x / (z * cam.tanHalf), v.p.y / (z * cam.tanHalf), z / (zref * cam.tanHalf)};
            }
            poly = clipPlane(poly, [&](const CV& v) { return v.p.x + aspect; });
            if (poly.size() >= 3) poly = clipPlane(poly, [&](const CV& v) { return aspect - v.p.x; });
            if (poly.size() >= 3) poly = clipPlane(poly, [&](const CV& v) { return v.p.y + 1; });
            if (poly.size() >= 3) poly = clipPlane(poly, [&](const CV& v) { return 1 - v.p.y; });
            if (poly.size() < 3) continue;
            const auto base = static_cast<std::uint32_t>(outMesh.positions.size());
            for (const auto& v : poly) {
                // renderer's front view: x right, z up, y away from the eye
                outMesh.positions.push_back({v.p.x, v.p.z, v.p.y});
                outMesh.normals.push_back(mesh::normalize(Vec3{v.n.x, v.n.z, v.n.y}));
            }
            for (std::uint32_t k = 1; k + 1 < poly.size(); ++k) outMesh.addTriangle(base, base + k, base + k + 1);
        }
        if (outMesh.empty()) continue;
        // two unused vertices pin the renderer's auto-fit to exactly the frame
        const double d0 = outMesh.positions[0].y;
        outMesh.positions.push_back({-aspect, d0, -1});
        outMesh.positions.push_back({aspect, d0, 1});
        outMesh.normals.push_back({0, -1, 0});
        outMesh.normals.push_back({0, -1, 0});
        mesh::RenderOptions ro;
        ro.view = mesh::View::Front;
        ro.background = {0, 0, 0, 0};
        ro.color = colorAt(e, "color", f);
        ro.margin = 0;
        ro.smooth = true;
        drawn.push_back({zref, opacity * ro.color.a / 255.0, mesh::render(outMesh, W, H, ro)});
    }
    std::stable_sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.depth > b.depth; });
    for (const auto& d : drawn) raster::blendOnto(out, d.image, 0, 0, raster::Blend::Normal, d.opacity);
}

}  // namespace

Image renderFrame(const AnimDoc& doc, int frame, const FrameOptions& o) {
    const int W = std::max(1, static_cast<int>(std::lround(doc.settings.width * o.scale)));
    const int H = std::max(1, static_cast<int>(std::lround(doc.settings.height * o.scale)));
    Image out(W, H, doc.settings.background);
    FrameOptions eff = o;
    eff.scale = static_cast<double>(W) / doc.settings.width;
    for (const auto& l : doc.layers) {
        if (!l.visible || l.opacity <= 0) continue;
        Image li(W, H, Color{0, 0, 0, 0});
        if (l.kind == "3d") {
            renderLayer3d(li, doc, l, frame);
        } else {
            for (const auto& e : l.elements) {
                if (e.visible) drawElement2d(li, doc, e, frame, eff);
            }
        }
        raster::blendOnto(out, li, 0, 0, raster::parseBlend(l.blend).value_or(raster::Blend::Normal), std::clamp(l.opacity, 0.0, 1.0));
    }
    if (o.bones) drawBones(out, doc, frame, eff);
    return out;
}

}  // namespace achclwss::animate
