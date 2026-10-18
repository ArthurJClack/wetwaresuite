// Bounding volume hierarchy, 3x3 eigen solver and frames.
#include <algorithm>
#include <cmath>
#include <numeric>

#include "wetwaresuite/fit/geom.hpp"
#include "wetwaresuite/mesh/topology.hpp"

namespace wetwaresuite::fit {

using mesh::cross;
using mesh::dot;
using mesh::length;
using mesh::normalize;

namespace {

constexpr std::uint32_t kLeafSize = 4;

// Slab test; returns the entry distance or a negative number on a miss.
double rayBox(const Bounds& b, const Vec3& o, const Vec3& inv, double maxT) {
    double t0 = 0, t1 = maxT;
    for (int a = 0; a < 3; ++a) {
        double tn = (b.min[a] - o[a]) * inv[a];
        double tf = (b.max[a] - o[a]) * inv[a];
        if (tn > tf) std::swap(tn, tf);
        if (std::isnan(tn) || std::isnan(tf)) {
            if (o[a] < b.min[a] || o[a] > b.max[a]) return -1;
            continue;
        }
        t0 = std::max(t0, tn);
        t1 = std::min(t1, tf);
        if (t0 > t1) return -1;
    }
    return t0;
}

double boxDistance2(const Bounds& b, const Vec3& p) {
    double d = 0;
    for (int a = 0; a < 3; ++a) {
        const double v = p[a] < b.min[a] ? b.min[a] - p[a] : p[a] > b.max[a] ? p[a] - b.max[a] : 0;
        d += v * v;
    }
    return d;
}

}  // namespace

Bvh::Bvh(std::shared_ptr<const Mesh> mesh) : mesh_(std::move(mesh)) {
    if (!mesh_ || mesh_->empty()) return;
    const Mesh& m = *mesh_;
    const std::size_t n = m.triangleCount();
    tris_.resize(n);
    std::iota(tris_.begin(), tris_.end(), 0u);
    std::vector<Vec3> centre(n);
    std::vector<Bounds> box(n);
    for (std::size_t t = 0; t < n; ++t) {
        for (int k = 0; k < 3; ++k) box[t].add(m.corner(t, k));
        centre[t] = box[t].center();
    }
    nodes_.reserve(2 * n / kLeafSize + 2);
    struct Todo {
        std::uint32_t node, first, count;
    };
    nodes_.push_back({});
    std::vector<Todo> stack{{0, 0, static_cast<std::uint32_t>(n)}};
    while (!stack.empty()) {
        const Todo job = stack.back();
        stack.pop_back();
        Bounds b, cb;
        for (std::uint32_t i = job.first; i < job.first + job.count; ++i) {
            b.merge(box[tris_[i]]);
            cb.add(centre[tris_[i]]);
        }
        nodes_[job.node].box = b;
        const Vec3 ext = cb.size();
        const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        if (job.count <= kLeafSize || ext[axis] <= 0) {
            nodes_[job.node].first = job.first;
            nodes_[job.node].count = job.count;
            continue;
        }
        const std::uint32_t half = job.count / 2;
        auto begin = tris_.begin() + job.first;
        std::nth_element(begin, begin + half, begin + job.count,
                         [&](std::uint32_t a, std::uint32_t c) { return centre[a][axis] < centre[c][axis]; });
        const auto left = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back({});
        nodes_.push_back({});
        nodes_[job.node].left = left;
        nodes_[job.node].right = left + 1;
        nodes_[job.node].count = 0;
        stack.push_back({left, job.first, half});
        stack.push_back({left + 1, job.first + half, job.count - half});
    }
}

std::optional<Bvh::Hit> Bvh::ray(const Vec3& o, const Vec3& dir, double maxT) const {
    if (empty()) return std::nullopt;
    const Mesh& m = *mesh_;
    const Vec3 inv{1.0 / dir.x, 1.0 / dir.y, 1.0 / dir.z};
    std::optional<Hit> best;
    double bestT = maxT;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& nd = nodes_[stack.back()];
        stack.pop_back();
        if (rayBox(nd.box, o, inv, bestT) < 0) continue;
        if (nd.count) {
            for (std::uint32_t i = nd.first; i < nd.first + nd.count; ++i) {
                const std::uint32_t t = tris_[i];
                const double d = mesh::rayTriangle(o, dir, m.corner(t, 0), m.corner(t, 1), m.corner(t, 2));
                if (d >= 0 && d <= bestT) {
                    bestT = d;
                    best = Hit{d, o + dir * d, t};
                }
            }
        } else {
            stack.push_back(nd.left);
            stack.push_back(nd.right);
        }
    }
    return best;
}

std::vector<Bvh::Hit> Bvh::rayAll(const Vec3& o, const Vec3& dir, double maxT) const {
    std::vector<Hit> hits;
    if (empty()) return hits;
    const Mesh& m = *mesh_;
    const Vec3 inv{1.0 / dir.x, 1.0 / dir.y, 1.0 / dir.z};
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& nd = nodes_[stack.back()];
        stack.pop_back();
        if (rayBox(nd.box, o, inv, maxT) < 0) continue;
        if (nd.count) {
            for (std::uint32_t i = nd.first; i < nd.first + nd.count; ++i) {
                const std::uint32_t t = tris_[i];
                const double d = mesh::rayTriangle(o, dir, m.corner(t, 0), m.corner(t, 1), m.corner(t, 2));
                if (d >= 0 && d <= maxT) hits.push_back({d, o + dir * d, t});
            }
        } else {
            stack.push_back(nd.left);
            stack.push_back(nd.right);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.t < b.t; });
    return hits;
}

Bvh::Nearest Bvh::nearest(const Vec3& p) const {
    Nearest best;
    if (empty()) return best;
    const Mesh& m = *mesh_;
    double best2 = std::numeric_limits<double>::infinity();
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& nd = nodes_[stack.back()];
        stack.pop_back();
        if (boxDistance2(nd.box, p) >= best2) continue;
        if (nd.count) {
            for (std::uint32_t i = nd.first; i < nd.first + nd.count; ++i) {
                const std::uint32_t t = tris_[i];
                const Vec3 q = mesh::closestPointOnTriangle(p, m.corner(t, 0), m.corner(t, 1), m.corner(t, 2));
                const Vec3 d = q - p;
                const double d2 = dot(d, d);
                if (d2 < best2) {
                    best2 = d2;
                    best.point = q;
                    best.tri = t;
                }
            }
        } else {
            // nearer child first, so the far one is usually pruned
            const double dl = boxDistance2(nodes_[nd.left].box, p), dr = boxDistance2(nodes_[nd.right].box, p);
            if (dl < dr) {
                stack.push_back(nd.right);
                stack.push_back(nd.left);
            } else {
                stack.push_back(nd.left);
                stack.push_back(nd.right);
            }
        }
    }
    best.distance = std::sqrt(best2);
    return best;
}

// ---- linear algebra ----

Eigen3 eigenSymmetric(const double in[9]) {
    double a[3][3], v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) a[i][j] = in[i * 3 + j];
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-300) continue;
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&](int x, int y) { return a[x][x] < a[y][y]; });
    Eigen3 e;
    for (int i = 0; i < 3; ++i) {
        const int k = order[i];
        e.values[i] = a[k][k];
        e.vectors[i] = normalize(Vec3{v[0][k], v[1][k], v[2][k]});
    }
    return e;
}

Mat4 rotationBetween(const Vec3& from, const Vec3& to) {
    const Vec3 a = normalize(from), b = normalize(to);
    const double c = std::clamp(dot(a, b), -1.0, 1.0);
    if (c > 1 - 1e-12) return Mat4::identity();
    Vec3 axis = cross(a, b);
    if (length(axis) < 1e-9) {
        // opposite: turn half way round any axis perpendicular to a
        axis = std::fabs(a.x) < 0.9 ? cross(a, Vec3{1, 0, 0}) : cross(a, Vec3{0, 1, 0});
    }
    return Mat4::rotateAxis(normalize(axis), std::acos(c) * 180.0 / mesh::kPi);
}

Mat4 Frame::matrix() const {
    Mat4 m;
    m.at(0, 0) = u.x; m.at(0, 1) = v.x; m.at(0, 2) = n.x; m.at(0, 3) = origin.x;
    m.at(1, 0) = u.y; m.at(1, 1) = v.y; m.at(1, 2) = n.y; m.at(1, 3) = origin.y;
    m.at(2, 0) = u.z; m.at(2, 1) = v.z; m.at(2, 2) = n.z; m.at(2, 3) = origin.z;
    return m;
}

Frame frameFor(const Vec3& origin, const Vec3& nIn, double angleDegrees) {
    Frame f;
    f.origin = origin;
    f.n = normalize(nIn);
    if (length(f.n) < 0.5) f.n = {0, 0, 1};
    Vec3 ref{1, 0, 0};
    if (std::fabs(dot(ref, f.n)) > 0.9) ref = {0, 1, 0};
    f.u = normalize(ref - f.n * dot(ref, f.n));
    f.v = cross(f.n, f.u);
    if (angleDegrees != 0) {
        const double a = angleDegrees * mesh::kPi / 180.0;
        const Vec3 u2 = f.u * std::cos(a) + f.v * std::sin(a);
        f.v = cross(f.n, u2);
        f.u = u2;
    }
    return f;
}

}  // namespace wetwaresuite::fit
