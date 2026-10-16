// Robust shape fits to noisy scan patches.
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <stdexcept>

#include "wetwaresuite/fit/geom.hpp"

namespace wetwaresuite::fit {

using mesh::cross;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

// Deterministic so a project replays to the same parts.
struct Lcg {
    std::uint64_t s = 0x9e3779b97f4a7c15ull;
    std::size_t below(std::size_t n) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::size_t>((s >> 33) % n);
    }
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

// Least squares plane through the chosen points.
void pcaPlane(const std::vector<Vec3>& pts, const std::vector<std::size_t>& use, Vec3& point, Vec3& normal) {
    Vec3 c;
    for (auto i : use) c += pts[i];
    c = c / static_cast<double>(use.size());
    double cov[9] = {0};
    for (auto i : use) {
        const Vec3 d = pts[i] - c;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) cov[a * 3 + b] += d[a] * d[b];
    }
    point = c;
    normal = eigenSymmetric(cov).vectors[0];
}

// Kasa circle fit in 2D: x^2 + y^2 + D x + E y + F = 0.
bool kasa(const std::vector<Vec2>& p, const std::vector<std::size_t>& use, Vec2& c, double& r) {
    double m[3][4] = {{0}};
    for (auto i : use) {
        const double x = p[i].x, y = p[i].y, z = -(x * x + y * y);
        const double row[3] = {x, y, 1};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) m[a][b] += row[a] * row[b];
            m[a][3] += row[a] * z;
        }
    }
    // Gaussian elimination with partial pivoting
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r2 = col + 1; r2 < 3; ++r2)
            if (std::fabs(m[r2][col]) > std::fabs(m[piv][col])) piv = r2;
        if (std::fabs(m[piv][col]) < 1e-18) return false;
        for (int k = 0; k < 4; ++k) std::swap(m[col][k], m[piv][k]);
        for (int r2 = 0; r2 < 3; ++r2) {
            if (r2 == col) continue;
            const double f = m[r2][col] / m[col][col];
            for (int k = 0; k < 4; ++k) m[r2][k] -= f * m[col][k];
        }
    }
    const double D = m[0][3] / m[0][0], E = m[1][3] / m[1][1], F = m[2][3] / m[2][2];
    c = {-D / 2, -E / 2};
    const double rr = c.x * c.x + c.y * c.y - F;
    if (rr <= 0) return false;
    r = std::sqrt(rr);
    return true;
}

// Repeatedly drops points whose residual is far past the typical one.
template <typename Residual, typename Refit>
std::vector<std::size_t> trimmedFit(std::size_t n, Residual residual, Refit refit) {
    std::vector<std::size_t> use(n);
    std::iota(use.begin(), use.end(), 0);
    refit(use);
    for (int pass = 0; pass < 6; ++pass) {
        std::vector<double> res(n);
        for (std::size_t i = 0; i < n; ++i) res[i] = std::fabs(residual(i));
        std::vector<double> inUse;
        for (auto i : use) inUse.push_back(res[i]);
        const double mad = median(inUse) * 1.4826;
        const double cut = std::max(mad * 2.5, 1e-9);
        std::vector<std::size_t> next;
        for (std::size_t i = 0; i < n; ++i)
            if (res[i] <= cut) next.push_back(i);
        if (next.size() < std::max<std::size_t>(4, n / 5) || next == use) break;
        use = std::move(next);
        refit(use);
    }
    return use;
}

double rmsOf(const std::vector<std::size_t>& use, const std::function<double(std::size_t)>& residual) {
    double s = 0;
    for (auto i : use) s += residual(i) * residual(i);
    return use.empty() ? 0 : std::sqrt(s / static_cast<double>(use.size()));
}

}  // namespace

PlaneFit fitPlane(const Patch& patch) {
    const auto& pts = patch.points;
    if (pts.size() < 3) throw std::runtime_error("need at least 3 scan points to fit a plane; use a bigger radius");
    Bounds b;
    for (const auto& p : pts) b.add(p);
    const double tol = std::max(b.diagonal() * 0.015, 0.05);
    // RANSAC for the dominant plane
    Lcg rng;
    std::size_t bestCount = 0;
    Vec3 bp = pts[0], bn{0, 0, 1};
    const std::size_t iters = pts.size() < 50 ? 30 : 160;
    for (std::size_t it = 0; it < iters; ++it) {
        const Vec3& a = pts[rng.below(pts.size())];
        const Vec3& c = pts[rng.below(pts.size())];
        const Vec3& d = pts[rng.below(pts.size())];
        Vec3 n = cross(c - a, d - a);
        if (length(n) < 1e-12) continue;
        n = normalize(n);
        std::size_t cnt = 0;
        for (const auto& p : pts) cnt += std::fabs(dot(p - a, n)) <= tol;
        if (cnt > bestCount) {
            bestCount = cnt;
            bp = a;
            bn = n;
        }
    }
    PlaneFit f;
    f.total = pts.size();
    std::vector<std::size_t> inl;
    for (std::size_t i = 0; i < pts.size(); ++i)
        if (std::fabs(dot(pts[i] - bp, bn)) <= tol) inl.push_back(i);
    if (inl.size() < 3) {
        inl.resize(pts.size());
        std::iota(inl.begin(), inl.end(), 0);
    }
    pcaPlane(pts, inl, f.point, f.normal);
    // refine with trimming among the RANSAC inliers' neighbourhood
    std::vector<Vec3> sub;
    for (auto i : inl) sub.push_back(pts[i]);
    auto use = trimmedFit(
        sub.size(), [&](std::size_t i) { return dot(sub[i] - f.point, f.normal); },
        [&](const std::vector<std::size_t>& u) { pcaPlane(sub, u, f.point, f.normal); });
    f.used = use.size();
    f.rms = rmsOf(use, [&](std::size_t i) { return dot(sub[i] - f.point, f.normal); });
    for (auto i : use) f.maxDeviation = std::max(f.maxDeviation, std::fabs(dot(sub[i] - f.point, f.normal)));
    // face the side the scan faces
    Vec3 avg;
    for (const auto& n : patch.normals) avg += n;
    if (dot(avg, f.normal) < 0) f.normal = -f.normal;
    return f;
}

SphereFit fitSphere(const Patch& patch) {
    const auto& pts = patch.points;
    if (pts.size() < 4) throw std::runtime_error("need at least 4 scan points to fit a sphere; use a bigger radius");
    Vec3 mean;
    for (const auto& p : pts) mean += p;
    mean = mean / static_cast<double>(pts.size());
    SphereFit f;
    f.total = pts.size();
    auto refit = [&](const std::vector<std::size_t>& use) {
        // x^2+y^2+z^2 + D x + E y + F z + G = 0, centred on the mean for conditioning
        double m[4][5] = {{0}};
        for (auto i : use) {
            const Vec3 q = pts[i] - mean;
            const double row[4] = {q.x, q.y, q.z, 1};
            const double rhs = -dot(q, q);
            for (int a = 0; a < 4; ++a) {
                for (int b = 0; b < 4; ++b) m[a][b] += row[a] * row[b];
                m[a][4] += row[a] * rhs;
            }
        }
        for (int col = 0; col < 4; ++col) {
            int piv = col;
            for (int r = col + 1; r < 4; ++r)
                if (std::fabs(m[r][col]) > std::fabs(m[piv][col])) piv = r;
            if (std::fabs(m[piv][col]) < 1e-18) return;
            for (int k = 0; k < 5; ++k) std::swap(m[col][k], m[piv][k]);
            for (int r = 0; r < 4; ++r) {
                if (r == col) continue;
                const double fct = m[r][col] / m[col][col];
                for (int k = 0; k < 5; ++k) m[r][k] -= fct * m[col][k];
            }
        }
        const Vec3 c{-m[0][4] / m[0][0] / 2, -m[1][4] / m[1][1] / 2, -m[2][4] / m[2][2] / 2};
        const double rr = dot(c, c) - m[3][4] / m[3][3];
        if (rr <= 0) return;
        f.center = mean + c;
        f.radius = std::sqrt(rr);
    };
    auto res = [&](std::size_t i) { return length(pts[i] - f.center) - f.radius; };
    auto use = trimmedFit(pts.size(), res, refit);
    if (f.radius <= 0) throw std::runtime_error("couldn't fit a sphere: the patch is flat; try fit.plane");
    f.used = use.size();
    f.rms = rmsOf(use, res);
    return f;
}

CircleFit fitCircle(const std::vector<Vec3>& pts, const std::optional<Vec3>& normal) {
    if (pts.size() < 3) throw std::runtime_error("need at least 3 points to fit a circle");
    CircleFit f;
    f.total = pts.size();
    Vec3 c0, n;
    std::vector<std::size_t> all(pts.size());
    std::iota(all.begin(), all.end(), 0);
    pcaPlane(pts, all, c0, n);
    if (normal) n = normalize(*normal);
    const Frame fr = frameFor(c0, n);
    std::vector<Vec2> p2(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec3 l = fr.toLocal(pts[i]);
        p2[i] = {l.x, l.y};
    }
    Vec2 c{};
    double r = 0;
    auto refit = [&](const std::vector<std::size_t>& use) { kasa(p2, use, c, r); };
    auto res = [&](std::size_t i) { return mesh::length(p2[i] - c) - r; };
    auto use = trimmedFit(pts.size(), res, refit);
    if (r <= 0) throw std::runtime_error("couldn't fit a circle to those points (they lie on a line)");
    f.center = fr.toWorld(c.x, c.y);
    f.normal = n;
    f.radius = r;
    f.used = use.size();
    f.rms = rmsOf(use, res);
    return f;
}

CylinderFit fitCylinder(const Patch& patch) {
    const auto& pts = patch.points;
    if (pts.size() < 8) throw std::runtime_error("need at least 8 scan points to fit a cylinder; use a bigger radius");
    // first guess: the axis is perpendicular to every surface normal
    double nn[9] = {0};
    for (const auto& n : patch.normals)
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) nn[a * 3 + b] += n[a] * n[b];
    Vec3 axis = eigenSymmetric(nn).vectors[0];
    Vec3 mean;
    for (const auto& p : pts) mean += p;
    mean = mean / static_cast<double>(pts.size());
    CylinderFit f;
    f.total = pts.size();
    std::vector<std::size_t> use;
    Vec2 c{};
    double r = 0;
    Frame fr;
    for (int round = 0; round < 4; ++round) {
        fr = frameFor(mean, axis);
        std::vector<Vec2> p2(pts.size());
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const Vec3 l = fr.toLocal(pts[i]);
            p2[i] = {l.x, l.y};
        }
        auto refit = [&](const std::vector<std::size_t>& u) { kasa(p2, u, c, r); };
        auto res = [&](std::size_t i) { return mesh::length(p2[i] - c) - r; };
        use = trimmedFit(pts.size(), res, refit);
        if (r <= 0) break;
        // better axis: perpendicular to the radial directions of the inliers
        const Vec3 centre3 = fr.toWorld(c.x, c.y);
        double rr[9] = {0};
        for (auto i : use) {
            Vec3 d = pts[i] - centre3;
            d = d - axis * dot(d, axis);
            d = normalize(d);
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) rr[a * 3 + b] += d[a] * d[b];
        }
        Vec3 next = eigenSymmetric(rr).vectors[0];
        if (dot(next, axis) < 0) next = -next;
        // radial directions only span a plane when the patch wraps far enough round; keep the old axis otherwise
        const Eigen3 e = eigenSymmetric(rr);
        if (e.values[1] < 0.05 * static_cast<double>(use.size())) break;
        if (dot(next, axis) > 1 - 1e-10) {
            axis = next;
            break;
        }
        axis = next;
    }
    if (r <= 0) throw std::runtime_error("couldn't fit a cylinder: the patch doesn't curve; try fit.plane");
    // axis direction: point "up" when ambiguous so results are stable
    if (axis.z < -1e-9 || (std::fabs(axis.z) <= 1e-9 && (axis.y < 0 || (axis.y == 0 && axis.x < 0)))) axis = -axis;
    fr = frameFor(mean, axis);
    std::vector<Vec2> p2(pts.size());
    double lo = 1e300, hi = -1e300;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Vec3 l = fr.toLocal(pts[i]);
        p2[i] = {l.x, l.y};
    }
    std::vector<std::size_t> all(pts.size());
    std::iota(all.begin(), all.end(), 0);
    kasa(p2, use.empty() ? all : use, c, r);
    for (auto i : use) {
        const double z = fr.toLocal(pts[i]).z;
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }
    f.axis = axis;
    f.radius = r;
    f.center = fr.toWorld(c.x, c.y, (lo + hi) / 2);
    f.length = std::max(hi - lo, 0.0);
    f.used = use.size();
    f.rms = rmsOf(use, [&](std::size_t i) { return mesh::length(p2[i] - c) - r; });
    return f;
}

std::vector<Vec3> smoothCurve(const std::vector<Vec3>& line, int passes, double spacing) {
    if (line.size() < 3) return line;
    std::vector<Vec3> cur = line;
    for (int p = 0; p < passes; ++p) {
        std::vector<Vec3> next = cur;
        for (std::size_t i = 1; i + 1 < cur.size(); ++i) next[i] = (cur[i - 1] + cur[i] * 2.0 + cur[i + 1]) / 4.0;
        cur.swap(next);
    }
    if (spacing <= 0) return cur;
    const double total = polylineLength(cur, false);
    const auto n = static_cast<std::size_t>(std::max(2.0, std::ceil(total / spacing) + 1));
    std::vector<Vec3> out;
    out.reserve(n);
    std::size_t seg = 0;
    double segStart = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const double target = total * static_cast<double>(k) / static_cast<double>(n - 1);
        while (seg + 2 < cur.size() && segStart + length(cur[seg + 1] - cur[seg]) < target) {
            segStart += length(cur[seg + 1] - cur[seg]);
            ++seg;
        }
        const double l = length(cur[seg + 1] - cur[seg]);
        const double t = l > 0 ? std::clamp((target - segStart) / l, 0.0, 1.0) : 0;
        out.push_back(mesh::lerp(cur[seg], cur[seg + 1], t));
    }
    return out;
}

}  // namespace wetwaresuite::fit
