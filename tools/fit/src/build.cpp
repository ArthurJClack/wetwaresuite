// Part builders: conforming plates and blocks, clamps, cups, strips, standard
// mounts, plus fit and printability checks.
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <unordered_map>

#include "wetwaresuite/fit/geom.hpp"
#include "wetwaresuite/mesh/csg.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::fit {

using mesh::cross;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

double segmentDistance(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a, ap = p - a;
    const double l2 = dot(ab, ab);
    const double t = l2 > 0 ? std::clamp(dot(ap, ab) / l2, 0.0, 1.0) : 0;
    return mesh::length(p - (a + ab * t));
}

bool insideRing(const std::vector<Vec2>& r, const Vec2& p) {
    bool in = false;
    for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
        if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x) in = !in;
    }
    return in;
}

void transformInPlace(Mesh& m, const Mat4& t) { mesh::transform(m, t); }

}  // namespace

// ---- footprints ----

Footprint rectFootprint(double w, double h, double corner, int segs) {
    if (w <= 0 || h <= 0) throw std::invalid_argument("width and height must be above 0");
    Footprint f;
    corner = std::clamp(corner, 0.0, std::min(w, h) / 2 - 1e-6);
    if (corner <= 0) {
        f.outer = {{-w / 2, -h / 2}, {w / 2, -h / 2}, {w / 2, h / 2}, {-w / 2, h / 2}};
        return f;
    }
    const Vec2 centres[4] = {{w / 2 - corner, -h / 2 + corner}, {w / 2 - corner, h / 2 - corner}, {-w / 2 + corner, h / 2 - corner}, {-w / 2 + corner, -h / 2 + corner}};
    for (int c = 0; c < 4; ++c) {
        const double a0 = -mesh::kPi / 2 + c * mesh::kPi / 2;
        for (int s = 0; s <= segs; ++s) {
            const double a = a0 + (mesh::kPi / 2) * s / segs;
            f.outer.push_back(centres[c] + Vec2{std::cos(a), std::sin(a)} * corner);
        }
    }
    return f;
}

Footprint circleFootprint(double d, int segments) { return ovalFootprint(d, d, segments); }

Footprint ovalFootprint(double w, double h, int segments) {
    if (w <= 0 || h <= 0) throw std::invalid_argument("diameter must be above 0");
    Footprint f;
    for (int i = 0; i < segments; ++i) {
        const double a = 2 * mesh::kPi * i / segments;
        f.outer.push_back({std::cos(a) * w / 2, std::sin(a) * h / 2});
    }
    return f;
}

double footprintDistance(const Footprint& f, const Vec2& p) {
    double d = 1e300;
    int inside = 0;
    auto ring = [&](const std::vector<Vec2>& r) {
        if (r.size() < 3) return;
        for (std::size_t i = 0; i < r.size(); ++i) d = std::min(d, segmentDistance(p, r[i], r[(i + 1) % r.size()]));
        inside += insideRing(r, p) ? 1 : 0;
    };
    ring(f.outer);
    for (const auto& h : f.holes) ring(h);
    return inside % 2 ? -d : d;
}

// ---- conforming parts ----

Mesh conformPart(const Bvh& scan, const ConformSpec& s, ConformStats* stats) {
    if (scan.empty()) throw std::runtime_error("the scan is empty");
    if (s.footprint.outer.size() < 3) throw std::runtime_error("the outline needs at least 3 points");
    if (s.thickness <= 0) throw std::runtime_error("thickness must be above 0");
    Vec2 lo{1e300, 1e300}, hi{-1e300, -1e300};
    for (const auto& p : s.footprint.outer) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    const double w = hi.x - lo.x, h = hi.y - lo.y;
    double res = s.resolution > 0 ? s.resolution : std::max(0.4, std::max(w, h) / 90.0);
    auto counts = [&](double r) {
        return std::pair<int, int>{static_cast<int>(std::ceil(w / r)) + 4, static_cast<int>(std::ceil(h / r)) + 4};
    };
    while (static_cast<std::size_t>(counts(res).first) * static_cast<std::size_t>(counts(res).second) > kMaxGridCells) res *= 1.25;
    const auto [nu, nv] = counts(res);
    const Vec2 base = lo - Vec2{2 * res, 2 * res};
    const double reach = s.reach > 0 ? s.reach : std::max(25.0, 0.5 * std::hypot(w, h) + 10.0);
    const Frame& fr = s.frame;
    auto at = [&](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(nu) + static_cast<std::size_t>(i); };

    // sample the scan: rays come down along -n from `reach` above the frame
    const std::size_t N = static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv);
    std::vector<double> sdf(N), height(N, std::nan(""));
    std::size_t inside = 0, missedInside = 0;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const Vec2 uv = base + Vec2{i * res, j * res};
            sdf[at(i, j)] = footprintDistance(s.footprint, uv);
            auto hit = scan.ray(fr.toWorld(uv.x, uv.y, reach), -fr.n, 2 * reach);
            if (hit) height[at(i, j)] = reach - hit->t;
            if (sdf[at(i, j)] <= res) {
                ++inside;
                missedInside += hit ? 0 : 1;
            }
        }
    if (inside == 0 || missedInside > inside * 6 / 10) {
        throw std::runtime_error("the outline is mostly off the scan (" + std::to_string(inside - missedInside) + " of " +
                                 std::to_string(inside) + " sample points hit it); move it, make it smaller, or fill holes first");
    }
    // fill misses (holes in the scan) smoothly from their neighbours
    double mean = 0;
    std::size_t valid = 0;
    for (double v : height)
        if (!std::isnan(v)) {
            mean += v;
            ++valid;
        }
    mean /= static_cast<double>(std::max<std::size_t>(valid, 1));
    std::vector<bool> missing(N);
    for (std::size_t k = 0; k < N; ++k) {
        missing[k] = std::isnan(height[k]);
        if (missing[k]) height[k] = mean;
    }
    if (valid < N) {
        for (int it = 0; it < 600; ++it) {
            double change = 0;
            for (int j = 0; j < nv; ++j)
                for (int i = 0; i < nu; ++i) {
                    if (!missing[at(i, j)]) continue;
                    double sum = 0;
                    int c = 0;
                    if (i > 0) { sum += height[at(i - 1, j)]; ++c; }
                    if (i + 1 < nu) { sum += height[at(i + 1, j)]; ++c; }
                    if (j > 0) { sum += height[at(i, j - 1)]; ++c; }
                    if (j + 1 < nv) { sum += height[at(i, j + 1)]; ++c; }
                    const double nvv = sum / c;
                    change = std::max(change, std::fabs(nvv - height[at(i, j)]));
                    height[at(i, j)] = nvv;
                }
            if (change < 1e-4) break;
        }
    }
    // blur away scanner noise (1-2-1 in each direction)
    for (int pass = 0; pass < s.smoothing; ++pass) {
        std::vector<double> tmp(N);
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
                tmp[at(i, j)] = (height[at(std::max(i - 1, 0), j)] + 2 * height[at(i, j)] + height[at(std::min(i + 1, nu - 1), j)]) / 4;
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
                height[at(i, j)] = (tmp[at(i, std::max(j - 1, 0))] + 2 * tmp[at(i, j)] + tmp[at(i, std::min(j + 1, nv - 1))]) / 4;
    }

    // marching squares over the outline's distance field
    std::vector<Vec2> uv;
    std::vector<double> under;  // underside height along n
    std::unordered_map<std::size_t, std::uint32_t> cornerId, edgeId;
    auto cornerVert = [&](int i, int j) {
        const std::size_t k = at(i, j);
        auto it = cornerId.find(k);
        if (it != cornerId.end()) return it->second;
        const auto id = static_cast<std::uint32_t>(uv.size());
        uv.push_back(base + Vec2{i * res, j * res});
        under.push_back(height[k] + s.gap);
        cornerId.emplace(k, id);
        return id;
    };
    auto edgeVert = [&](int i0, int j0, int i1, int j1) {
        const std::size_t a = at(i0, j0), b = at(i1, j1);
        const std::size_t key = std::min(a, b) * 2 + (j0 == j1 ? 0 : 1);
        auto it = edgeId.find(key);
        if (it != edgeId.end()) return it->second;
        const double t = std::clamp(sdf[a] / (sdf[a] - sdf[b]), 0.02, 0.98);
        const auto id = static_cast<std::uint32_t>(uv.size());
        uv.push_back(base + Vec2{(i0 + (i1 - i0) * t) * res, (j0 + (j1 - j0) * t) * res});
        under.push_back(height[a] + (height[b] - height[a]) * t + s.gap);
        edgeId.emplace(key, id);
        return id;
    };
    std::vector<std::uint32_t> tri2;
    for (int j = 0; j + 1 < nv; ++j)
        for (int i = 0; i + 1 < nu; ++i) {
            const int ci[4] = {i, i + 1, i + 1, i}, cj[4] = {j, j, j + 1, j + 1};
            std::vector<std::uint32_t> poly;
            for (int k = 0; k < 4; ++k) {
                const int k2 = (k + 1) % 4;
                const bool in0 = sdf[at(ci[k], cj[k])] < 0, in1 = sdf[at(ci[k2], cj[k2])] < 0;
                if (in0) poly.push_back(cornerVert(ci[k], cj[k]));
                if (in0 != in1) poly.push_back(edgeVert(ci[k], cj[k], ci[k2], cj[k2]));
            }
            for (std::size_t m = 1; m + 1 < poly.size(); ++m) tri2.insert(tri2.end(), {poly[0], poly[m], poly[m + 1]});
        }
    if (tri2.empty()) throw std::runtime_error("the outline is smaller than the grid; make it bigger or lower resolution");

    // top heights
    const std::size_t nvtx = uv.size();
    std::vector<double> top(nvtx);
    double minH = 1e300, maxH = -1e300, maxThick = s.thickness;
    for (std::size_t k = 0; k < nvtx; ++k) {
        minH = std::min(minH, under[k] - s.gap);
        maxH = std::max(maxH, under[k] - s.gap);
    }
    if (s.block) {
        const double tu = std::tan(s.tiltU * mesh::kPi / 180.0), tv = std::tan(s.tiltV * mesh::kPi / 180.0);
        auto slope = [&](std::size_t k) { return uv[k].y * tu - uv[k].x * tv; };
        double h0 = -1e300;
        for (std::size_t k = 0; k < nvtx; ++k) h0 = std::max(h0, under[k] - slope(k));
        h0 += s.thickness;
        for (std::size_t k = 0; k < nvtx; ++k) {
            top[k] = h0 + slope(k);
            maxThick = std::max(maxThick, top[k] - under[k]);
        }
    } else {
        for (std::size_t k = 0; k < nvtx; ++k) top[k] = under[k] + s.thickness;
    }

    Mesh m;
    for (std::size_t k = 0; k < nvtx; ++k) m.addVertex(fr.toWorld(uv[k].x, uv[k].y, under[k]));
    for (std::size_t k = 0; k < nvtx; ++k) m.addVertex(fr.toWorld(uv[k].x, uv[k].y, top[k]));
    const auto off = static_cast<std::uint32_t>(nvtx);
    std::unordered_map<std::uint64_t, int> edgeUse;
    auto ek = [](std::uint32_t a, std::uint32_t b) { return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | std::max(a, b); };
    for (std::size_t t = 0; t < tri2.size(); t += 3) {
        const std::uint32_t a = tri2[t], b = tri2[t + 1], c = tri2[t + 2];
        m.addTriangle(a, c, b);                    // underside faces -n
        m.addTriangle(a + off, b + off, c + off);  // top faces +n
        ++edgeUse[ek(a, b)];
        ++edgeUse[ek(b, c)];
        ++edgeUse[ek(c, a)];
    }
    for (std::size_t t = 0; t < tri2.size(); t += 3) {
        for (int e = 0; e < 3; ++e) {
            const std::uint32_t a = tri2[t + static_cast<std::size_t>(e)], b = tri2[t + static_cast<std::size_t>((e + 1) % 3)];
            if (edgeUse[ek(a, b)] != 1) continue;
            m.addTriangle(a, b, b + off);
            m.addTriangle(a, b + off, a + off);
        }
    }
    mesh::cleanup(m);
    if (s.targetTriangles > 0 && m.triangleCount() > s.targetTriangles) {
        Mesh d = sculpt::decimateQuadric(m, s.targetTriangles);
        if (isClosed(d)) m = std::move(d);
    }
    if (stats) {
        stats->samples = inside;
        stats->missed = missedInside;
        stats->minHeight = minH;
        stats->maxHeight = maxH;
        stats->resolution = res;
        stats->maxThickness = maxThick;
    }
    return m;
}

// ---- solids of revolution ----

Mesh ringSolid(double inner, double outer, double width, int segments) {
    if (inner <= 0 || outer <= inner || width <= 0) throw std::invalid_argument("ring needs 0 < inner < outer and width > 0");
    return mesh::revolve({{inner, -width / 2}, {outer, -width / 2}, {outer, width / 2}, {inner, width / 2}}, 360, segments);
}

Mesh cupSolid(double r, double wall, double depth, double floor, int segments) {
    if (r <= 0 || wall <= 0 || wall >= r || depth <= 0) throw std::invalid_argument("cup needs radius > wall > 0 and depth > 0");
    if (floor <= 0) return mesh::revolve({{r - wall, 0}, {r, 0}, {r, depth}, {r - wall, depth}}, 360, segments);
    if (floor >= depth) throw std::invalid_argument("cup floor must be thinner than its depth");
    return mesh::revolve({{0, 0}, {r, 0}, {r, depth}, {r - wall, depth}, {r - wall, floor}, {0, floor}}, 360, segments);
}

// ---- booleans ----

bool isClosed(const Mesh& m) { return !m.empty() && mesh::boundaryEdgeCount(m) == 0; }

namespace {
// BSP booleans sometimes drop a sliver triangle where many cuts meet, leaving
// a hole a fraction of a millimetre across; patch those.
bool closeSlivers(Mesh& r) {
    if (r.empty()) return false;
    if (isClosed(r)) return true;
    fillHoles(r, 1.0);
    return isClosed(r);
}
}  // namespace

Mesh unite(const Mesh& a, const Mesh& b, bool* ok) {
    if (a.empty() || b.empty()) {
        if (ok) *ok = true;
        Mesh m = a;
        mesh::append(m, b);
        return m;
    }
    Mesh r = mesh::booleanUnion(a, b);
    const bool good = closeSlivers(r) && mesh::volume(r) > 0;
    if (ok) *ok = good;
    if (good) return r;
    Mesh m = a;
    mesh::append(m, b);
    return m;
}

Mesh subtract(const Mesh& a, const Mesh& b, bool* ok) {
    Mesh r = mesh::booleanDifference(a, b);
    const bool good = closeSlivers(r) && mesh::volume(r) > 0;
    if (ok) *ok = good;
    return good ? r : a;
}

// ---- clamps and strips ----

std::vector<Mesh> clampParts(const ClampSpec& s) {
    if (s.radius <= 0) throw std::invalid_argument("clamp radius must be above 0");
    if (s.thickness <= 0 || s.width <= 0) throw std::invalid_argument("clamp thickness and width must be above 0");
    const double ri = s.radius + s.gap, ro = ri + s.thickness;
    const int segs = std::clamp(static_cast<int>(ro * 3), 48, 128);
    Mesh body = ringSolid(ri, ro, s.width, segs);
    const bool tabs = s.boltDiameter > 0;
    const double tabT = std::max(s.thickness, 4.0);
    const double tabLen = tabs ? s.boltDiameter * 2 + 4 : 0;
    const double slot = std::max(s.slot, 0.5);
    auto boxAt = [](double u0, double u1, double v0, double v1, double z0, double z1) {
        Mesh b = mesh::box({u1 - u0, v1 - v0, z1 - z0}, false);
        mesh::transform(b, Mat4::translate({u0, v0, z0}));
        return b;
    };
    const int sides = s.pieces == 2 ? 2 : 1;
    for (int side = 0; side < sides && tabs; ++side) {
        const double sg = side == 0 ? 1 : -1;
        const double u0 = ro - s.thickness * 0.5, u1 = ro + tabLen;
        Mesh tab = boxAt(sg > 0 ? u0 : -u1, sg > 0 ? u1 : -u0, -(slot / 2 + tabT), slot / 2 + tabT, -s.width / 2, s.width / 2);
        body = unite(body, tab);
    }
    // the split
    const double reachU = ro + tabLen + 2;
    Mesh cut = s.pieces == 2 ? boxAt(-reachU, reachU, -slot / 2, slot / 2, -s.width, s.width)
                             : boxAt(ri - s.thickness, reachU, -slot / 2, slot / 2, -s.width, s.width);
    body = subtract(body, cut);
    for (int side = 0; side < sides && tabs; ++side) {
        const double sg = side == 0 ? 1 : -1;
        Mesh bolt = mesh::cylinder(s.boltDiameter / 2, 2 * (slot / 2 + tabT) + 4, 24, true);
        mesh::transform(bolt, Mat4::translate({sg * (ro + tabLen * 0.55), 0, 0}) * Mat4::rotateX(90));
        body = subtract(body, bolt);
    }
    const Frame fr = frameFor(s.center, s.axis, s.angle);
    transformInPlace(body, fr.matrix());
    std::vector<Mesh> out;
    if (s.pieces == 2) {
        for (const auto& tris : pieces(body)) {
            Mesh p;
            std::unordered_map<std::uint32_t, std::uint32_t> remap;
            for (auto t : tris) {
                std::uint32_t ids[3];
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t v = body.indices[t * 3 + static_cast<std::size_t>(k)];
                    auto it = remap.find(v);
                    if (it == remap.end()) it = remap.emplace(v, p.addVertex(body.positions[v])).first;
                    ids[k] = it->second;
                }
                p.addTriangle(ids[0], ids[1], ids[2]);
            }
            if (mesh::volume(p) > 1) out.push_back(std::move(p));
        }
    }
    if (out.empty()) out.push_back(std::move(body));
    return out;
}

Mesh stripPart(const std::vector<Vec3>& curve, const Vec3& planeNormal, const Vec3& side, double gap, double thickness, double width) {
    if (curve.size() < 2) throw std::invalid_argument("the curve needs at least 2 points");
    if (thickness <= 0 || width <= 0) throw std::invalid_argument("strip thickness and width must be above 0");
    const Vec3 pn = normalize(planeNormal);
    const std::size_t n = curve.size();
    std::vector<Vec3> off(n);
    double vote = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 t = normalize(curve[std::min(i + 1, n - 1)] - curve[i == 0 ? 0 : i - 1]);
        off[i] = normalize(cross(pn, t));
        vote += dot(off[i], side);
    }
    if (vote < 0)
        for (auto& o : off) o = -o;
    Mesh m;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec3 a = curve[i] + off[i] * gap, b = curve[i] + off[i] * (gap + thickness);
        m.addVertex(a - pn * (width / 2));
        m.addVertex(b - pn * (width / 2));
        m.addVertex(b + pn * (width / 2));
        m.addVertex(a + pn * (width / 2));
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const auto q = static_cast<std::uint32_t>(i * 4), r = q + 4;
        for (std::uint32_t k = 0; k < 4; ++k) {
            const std::uint32_t k2 = (k + 1) % 4;
            m.addTriangle(q + k, q + k2, r + k2);
            m.addTriangle(q + k, r + k2, r + k);
        }
    }
    const auto last = static_cast<std::uint32_t>((n - 1) * 4);
    m.addTriangle(0, 2, 1);
    m.addTriangle(0, 3, 2);
    m.addTriangle(last, last + 1, last + 2);
    m.addTriangle(last, last + 2, last + 3);
    if (mesh::volume(m) < 0) mesh::flip(m);
    return m;
}

// ---- mounts ----

Mesh ballMount(double ball, double neck, double neckLength) {
    Mesh n = mesh::cylinder(neck / 2, neckLength + 1 + ball * 0.25, 32, false);
    mesh::transform(n, Mat4::translate({0, 0, -1}));
    Mesh b = mesh::sphere(ball / 2, 40, 20);
    mesh::transform(b, Mat4::translate({0, 0, neckLength + ball / 2}));
    return unite(n, b);
}

Mesh cameraMount(double boss, double height) {
    Mesh m = mesh::cylinder(boss / 2, height + 1, 48, false);
    mesh::transform(m, Mat4::translate({0, 0, -1}));
    // 1/4"-20 nut: 11.11 mm across flats, 5.56 mm thick
    const double hexR = (11.11 + 0.3) / std::sqrt(3.0);
    Mesh hex = mesh::cylinder(hexR, 6.2, 6, false);
    mesh::transform(hex, Mat4::translate({0, 0, height - 5.8}));
    m = subtract(m, hex);
    Mesh hole = mesh::cylinder(3.4, height + 4, 24, false);
    mesh::transform(hole, Mat4::translate({0, 0, -2}));
    return subtract(m, hole);
}

Mesh goproMount() {
    // the 3-prong half of a GoPro mount: prongs 3 mm thick, 3.2 mm apart, 5 mm bolt
    Mesh m = mesh::box({20, 17, 4}, false);
    mesh::transform(m, Mat4::translate({-10, -8.5, -1}));
    const double t = 3.0, g = 3.2;
    for (int k = -1; k <= 1; ++k) {
        const double y = k * (t + g);
        Mesh prong = mesh::box({15, t, 9}, false);
        mesh::transform(prong, Mat4::translate({-7.5, y - t / 2, 2}));
        Mesh top = mesh::cylinder(7.5, t, 40, true);
        mesh::transform(top, Mat4::translate({0, y, 11}) * Mat4::rotateX(90));
        m = unite(m, unite(prong, top));
    }
    Mesh bolt = mesh::cylinder(2.6, 3 * t + 2 * g + 4, 24, true);
    mesh::transform(bolt, Mat4::translate({0, 0, 11}) * Mat4::rotateX(90));
    return subtract(m, bolt);
}

double screwClearance(const std::string& size) {
    static const std::map<std::string, double> d = {{"M2", 2.4}, {"M2.5", 2.9}, {"M3", 3.4}, {"M4", 4.5}, {"M5", 5.5},
                                                    {"M6", 6.6}, {"M8", 9.0}, {"M10", 11.0}};
    auto it = d.find(size);
    return it == d.end() ? 0 : it->second;
}

double countersinkDiameter(const std::string& size) {
    static const std::map<std::string, double> d = {{"M2", 4.4}, {"M2.5", 5.5}, {"M3", 6.3}, {"M4", 8.4}, {"M5", 10.4},
                                                    {"M6", 12.6}, {"M8", 16.6}, {"M10", 20.0}};
    auto it = d.find(size);
    return it == d.end() ? 0 : it->second;
}

// ---- checks ----

FitReport checkFit(const Bvh& scan, const Mesh& part, double range, double expectedGap) {
    FitReport r;
    r.total = part.vertexCount();
    r.vertexGap.assign(part.vertexCount(), std::nan(""));
    if (scan.empty()) return r;
    const Mesh& sm = scan.mesh();
    std::size_t behind = 0;
    for (std::size_t i = 0; i < part.vertexCount(); ++i) {
        const auto q = scan.nearest(part.positions[i]);
        if (q.distance > range) continue;
        const Vec3 n = normalize(sm.faceNormal(q.tri));
        // only straight behind counts: points off past the scan's open edge are just beside it
        const Vec3 d = part.positions[i] - q.point;
        const bool back = q.distance > 1e-9 && dot(d, n) < -0.5 * q.distance;
        r.vertexGap[i] = back ? -q.distance : q.distance;
        behind += back && q.distance > 0.05 ? 1 : 0;
        ++r.near;
    }
    if (!r.near) return r;
    // a part mostly "behind" the surface means the scan's triangles face the
    // other way (apps don't agree on winding), not that the part is buried
    if (behind * 2 > r.near) {
        for (double& g : r.vertexGap)
            if (!std::isnan(g)) g = -g;
    }
    double sum = 0, mn = 1e300, mx = -1e300;
    std::size_t contact = 0, in = 0;
    for (double g : r.vertexGap) {
        if (std::isnan(g)) continue;
        sum += g;
        mn = std::min(mn, g);
        mx = std::max(mx, g);
        if (g < -0.05) ++in;
        else if (g <= expectedGap + 0.5) ++contact;
    }
    r.minGap = mn;
    r.maxGap = mx;
    r.meanGap = sum / static_cast<double>(r.near);
    r.contact = static_cast<double>(contact) / static_cast<double>(r.near);
    r.inside = static_cast<double>(in) / static_cast<double>(r.near);
    return r;
}

PrintReport checkPrint(const Mesh& part, const std::vector<Vec3>& extraDowns) {
    PrintReport r;
    if (part.empty()) return r;
    r.watertight = isClosed(part);
    r.volume = mesh::volume(part);
    r.area = mesh::surfaceArea(part);
    r.size = mesh::bounds(part).size();
    // wall thickness: rays inward from face centres
    auto shared = std::make_shared<const Mesh>(part);
    const Bvh bvh(shared);
    // (thickness, area) per sampled face; the 2nd percentile by area ignores the
    // slivers booleans leave at hole rims, whose rays graze straight out again
    std::vector<std::pair<double, double>> th;
    const std::size_t n = part.triangleCount();
    const std::size_t step = std::max<std::size_t>(1, n / 4000);
    for (std::size_t t = 0; t < n; t += step) {
        const Vec3 f = part.faceNormal(t);
        if (length(f) < 1e-12) continue;
        const Vec3 nn = normalize(f);
        const Vec3 c = (part.corner(t, 0) + part.corner(t, 1) + part.corner(t, 2)) / 3.0;
        const auto hits = bvh.rayAll(c - nn * 1e-4, -nn);
        for (const auto& h : hits) {
            if (h.tri == t || h.t < 1e-3) continue;
            th.emplace_back(h.t, length(f) * 0.5);
            break;
        }
    }
    if (!th.empty()) {
        std::sort(th.begin(), th.end());
        double total = 0, acc = 0;
        for (const auto& [t, a] : th) total += a;
        r.thinnest = th.back().first;
        for (const auto& [t, a] : th) {
            acc += a;
            if (acc >= total * 0.02) {
                r.thinnest = t;
                break;
            }
        }
    }
    std::vector<Vec3> downs = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    for (const auto& d : extraDowns)
        if (length(d) > 0.5) downs.push_back(normalize(d));
    const double c45 = std::cos(45.0 * mesh::kPi / 180.0);
    double bestScore = 1e300;
    for (const auto& d : downs) {
        double maxH = -1e300;
        for (const auto& p : part.positions) maxH = std::max(maxH, dot(p, d));
        double over = 0, bed = 0;
        for (std::size_t t = 0; t < n; ++t) {
            const Vec3 f = part.faceNormal(t);
            const double a = length(f) * 0.5;
            if (a <= 0) continue;
            const Vec3 nn = f / (2 * a);
            const Vec3 c = (part.corner(t, 0) + part.corner(t, 1) + part.corner(t, 2)) / 3.0;
            const bool onBed = dot(c, d) >= maxH - 0.3;
            if (onBed && dot(nn, d) > 0.98) bed += a;
            else if (dot(nn, d) > c45) over += a;
        }
        // overhangs cost most; a bigger flat base breaks ties
        const double score = over - bed * 0.05;
        if (score < bestScore) {
            bestScore = score;
            r.down = d;
            r.overhang = r.area > 0 ? over / r.area : 0;
        }
    }
    return r;
}

Rgb gapColor(double gap, double expected) {
    if (std::isnan(gap)) return {200, 204, 212};
    if (gap < -0.05) return {168, 64, 200};
    // blue at the expected gap, through green and yellow, to red 3 mm further out
    const double t = std::clamp((gap - expected) / 3.0, 0.0, 1.0);
    const double hue = 220.0 * (1 - t);
    const double hh = hue / 60.0;
    const double x = 1 - std::fabs(std::fmod(hh, 2.0) - 1);
    double r = 0, g = 0, b = 0;
    if (hh < 1) { r = 1; g = x; }
    else if (hh < 2) { r = x; g = 1; }
    else if (hh < 3) { g = 1; b = x; }
    else { g = x; b = 1; }
    auto byte = [](double v) { return static_cast<std::uint8_t>(std::clamp(v * 0.85 * 255 + 25, 0.0, 255.0)); };
    return {byte(r), byte(g), byte(b)};
}

}  // namespace wetwaresuite::fit
