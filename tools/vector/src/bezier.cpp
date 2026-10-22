// Paths as anchors with handles (the pen tool's model): SVG path data in and
// out, flattening, splitting, nearest point, smoothing and curve fitting.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKappa = 0.5522847498307936;  // handle length of a quarter circle, times the radius

Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
Point sub(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
Point mul(Point a, double k) { return {a.x * k, a.y * k}; }
double dot(Point a, Point b) { return a.x * b.x + a.y * b.y; }
double cross(Point a, Point b) { return a.x * b.y - a.y * b.x; }
double len(Point a) { return std::hypot(a.x, a.y); }
double dist(Point a, Point b) { return len(sub(a, b)); }
bool same(Point a, Point b, double eps = 1e-9) { return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps; }
Point lerp(Point a, Point b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
Point unit(Point a) {
    const double l = len(a);
    return l > 1e-12 ? mul(a, 1 / l) : Point{0, 0};
}

int segmentsFor(double length, double detail) {
    double n = std::ceil(std::sqrt(std::max(0.0, length * detail)) * 1.5);
    return static_cast<int>(std::clamp(n, 4.0, 256.0));
}

Node corner(Point p) { return {p, p, p}; }

// Reads SVG path data into cubic subpaths.
class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}

    BezierPath run(std::string* error) {
        char cmd = 0;
        while (true) {
            skip();
            if (i_ >= s_.size()) break;
            const char ch = s_[i_];
            if (std::isalpha(static_cast<unsigned char>(ch))) {
                if (!std::strchr("MmLlHhVvCcSsQqTtAaZz", ch)) return fail(error, "unknown path command '" + std::string(1, ch) + "'");
                cmd = ch;
                ++i_;
                if (cmd == 'Z' || cmd == 'z') {
                    close();
                    continue;
                }
            } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
                return fail(error, "expected a command letter");
            }
            if (!segment(cmd)) return fail(error, "bad or missing number for '" + std::string(1, cmd) + "'");
            if (cmd == 'M') cmd = 'L';
            if (cmd == 'm') cmd = 'l';
        }
        flush();
        return std::move(out_);
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;
    BezierPath out_;
    SubPath cur_;
    Point pos_{0, 0}, start_{0, 0}, ctrl_{0, 0};
    char prev_ = 0;

    BezierPath fail(std::string* error, const std::string& why) {
        if (error) *error = why + " at position " + std::to_string(i_);
        flush();
        return std::move(out_);
    }
    void skip() {
        while (i_ < s_.size() && (std::isspace(static_cast<unsigned char>(s_[i_])) || s_[i_] == ',')) ++i_;
    }
    bool number(double& v) {
        skip();
        const char* begin = s_.c_str() + i_;
        // strtod reads "1e5" and hex; SVG numbers are plain decimals, so check the first char
        if (i_ >= s_.size() || !(std::isdigit(static_cast<unsigned char>(*begin)) || *begin == '-' || *begin == '+' || *begin == '.')) return false;
        std::size_t k = i_;
        if (s_[k] == '+' || s_[k] == '-') ++k;
        bool any = false, dotSeen = false;
        while (k < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[k])) || (s_[k] == '.' && !dotSeen))) {
            if (s_[k] == '.') dotSeen = true;
            else any = true;
            ++k;
        }
        if (!any) return false;
        if (k < s_.size() && (s_[k] == 'e' || s_[k] == 'E')) {
            std::size_t e = k + 1;
            if (e < s_.size() && (s_[e] == '+' || s_[e] == '-')) ++e;
            if (e < s_.size() && std::isdigit(static_cast<unsigned char>(s_[e]))) {
                k = e;
                while (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) ++k;
            }
        }
        v = std::strtod(s_.substr(i_, k - i_).c_str(), nullptr);
        i_ = k;
        return true;
    }
    bool flag(bool& v) {
        skip();
        if (i_ < s_.size() && (s_[i_] == '0' || s_[i_] == '1')) {
            v = s_[i_++] == '1';
            return true;
        }
        return false;
    }
    bool point(Point& p, bool rel) {
        if (!number(p.x) || !number(p.y)) return false;
        if (rel) p = add(p, pos_);
        return true;
    }
    void flush() {
        if (!cur_.nodes.empty() && (cur_.nodes.size() >= 2 || cur_.closed)) out_.push_back(std::move(cur_));
        cur_ = {};
    }
    void ensureStart() {
        if (cur_.nodes.empty()) cur_.nodes.push_back(corner(pos_));
    }
    void lineTo(Point p) {
        ensureStart();
        cur_.nodes.push_back(corner(p));
        pos_ = p;
    }
    void cubicTo(Point c1, Point c2, Point p) {
        ensureStart();
        cur_.nodes.back().out = c1;
        cur_.nodes.push_back({p, c2, p});
        pos_ = p;
    }
    void close() {
        if (!cur_.nodes.empty()) {
            // a last anchor on top of the first one is the same anchor
            if (cur_.nodes.size() > 1 && same(cur_.nodes.back().p, cur_.nodes.front().p, 1e-6)) {
                cur_.nodes.front().in = cur_.nodes.back().in;
                cur_.nodes.pop_back();
            }
            cur_.closed = true;
            flush();
        }
        pos_ = start_;
        prev_ = 'Z';
    }
    void arc(double rx, double ry, double angle, bool large, bool sweep, Point p) {
        const Point p0 = pos_;
        if (same(p0, p)) return;
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx == 0 || ry == 0) {
            lineTo(p);
            return;
        }
        const double phi = angle * kPi / 180, cs = std::cos(phi), sn = std::sin(phi);
        const double dx2 = (p0.x - p.x) / 2, dy2 = (p0.y - p.y) / 2;
        const double x1p = cs * dx2 + sn * dy2, y1p = -sn * dx2 + cs * dy2;
        const double lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry);
        if (lambda > 1) {
            rx *= std::sqrt(lambda);
            ry *= std::sqrt(lambda);
        }
        const double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
        const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
        double coef = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
        if (large == sweep) coef = -coef;
        const double cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
        const double cx = cs * cxp - sn * cyp + (p0.x + p.x) / 2, cy = sn * cxp + cs * cyp + (p0.y + p.y) / 2;
        auto angleBetween = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
        const double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
        const double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
        const double theta1 = angleBetween(1, 0, ux, uy);
        double dtheta = angleBetween(ux, uy, vx, vy);
        if (!sweep && dtheta > 0) dtheta -= 2 * kPi;
        if (sweep && dtheta < 0) dtheta += 2 * kPi;
        const int n = std::max(1, static_cast<int>(std::ceil(std::abs(dtheta) / (kPi / 2) - 1e-9)));
        const double step = dtheta / n, k = 4.0 / 3.0 * std::tan(step / 4);
        auto at = [&](double t) { return Point{cx + rx * std::cos(t) * cs - ry * std::sin(t) * sn, cy + rx * std::cos(t) * sn + ry * std::sin(t) * cs}; };
        auto deriv = [&](double t) { return Point{-rx * std::sin(t) * cs - ry * std::cos(t) * sn, -rx * std::sin(t) * sn + ry * std::cos(t) * cs}; };
        for (int i = 0; i < n; ++i) {
            const double t0 = theta1 + step * i, t1 = t0 + step;
            const Point a = at(t0), b = i == n - 1 ? p : at(t1);
            cubicTo(add(a, mul(deriv(t0), k)), sub(b, mul(deriv(t1), k)), b);
        }
    }
    bool segment(char cmd) {
        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
        Point p, c1, c2;
        double v = 0;
        switch (up) {
            case 'M':
                if (!point(p, rel)) return false;
                flush();
                pos_ = start_ = p;
                cur_.nodes.push_back(corner(p));
                break;
            case 'L':
                if (!point(p, rel)) return false;
                lineTo(p);
                break;
            case 'H':
                if (!number(v)) return false;
                lineTo({rel ? pos_.x + v : v, pos_.y});
                break;
            case 'V':
                if (!number(v)) return false;
                lineTo({pos_.x, rel ? pos_.y + v : v});
                break;
            case 'C':
                if (!point(c1, rel) || !point(c2, rel) || !point(p, rel)) return false;
                cubicTo(c1, c2, p);
                ctrl_ = c2;
                break;
            case 'S':
                if (!point(c2, rel) || !point(p, rel)) return false;
                c1 = (prev_ == 'C' || prev_ == 'S') ? sub(mul(pos_, 2), ctrl_) : pos_;
                cubicTo(c1, c2, p);
                ctrl_ = c2;
                break;
            case 'Q':
            case 'T': {
                Point q;
                if (up == 'Q') {
                    if (!point(q, rel) || !point(p, rel)) return false;
                } else {
                    if (!point(p, rel)) return false;
                    q = (prev_ == 'Q' || prev_ == 'T') ? sub(mul(pos_, 2), ctrl_) : pos_;
                }
                const Point p0 = pos_;
                cubicTo(add(p0, mul(sub(q, p0), 2.0 / 3)), add(p, mul(sub(q, p), 2.0 / 3)), p);
                ctrl_ = q;
                break;
            }
            case 'A': {
                double rx = 0, ry = 0, angle = 0;
                bool large = false, sweep = false;
                if (!number(rx) || !number(ry) || !number(angle) || !flag(large) || !flag(sweep) || !point(p, rel)) return false;
                arc(rx, ry, angle, large, sweep, p);
                break;
            }
            default: return false;
        }
        prev_ = up;
        return true;
    }
};

std::string fmt(double v) {
    if (std::abs(v) < 5e-4) return "0";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? std::string("0") : s;
}

bool isLine(const Node& a, const Node& b) { return same(a.out, a.p) && same(b.in, b.p); }

Point bez(Point p0, Point p1, Point p2, Point p3, double t) {
    const double u = 1 - t;
    return {u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
            u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y};
}

// ---- curve fitting (Philip Schneider, Graphics Gems 1990) ------------------------

struct Cubic {
    Point p0, p1, p2, p3;
};

Point bezDeriv(const Cubic& c, double t) {
    const double u = 1 - t;
    return add(add(mul(sub(c.p1, c.p0), 3 * u * u), mul(sub(c.p2, c.p1), 6 * u * t)), mul(sub(c.p3, c.p2), 3 * t * t));
}
Point bezDeriv2(const Cubic& c, double t) {
    return add(mul(add(sub(c.p2, mul(c.p1, 2)), c.p0), 6 * (1 - t)), mul(add(sub(c.p3, mul(c.p2, 2)), c.p1), 6 * t));
}

Cubic fitOne(const std::vector<Point>& d, std::size_t first, std::size_t last, const std::vector<double>& u, Point t1, Point t2) {
    const Point p0 = d[first], p3 = d[last];
    double c00 = 0, c01 = 0, c11 = 0, x0 = 0, x1 = 0;
    for (std::size_t i = first; i <= last; ++i) {
        const double t = u[i - first], s = 1 - t;
        const Point a1 = mul(t1, 3 * s * s * t), a2 = mul(t2, 3 * s * t * t);
        c00 += dot(a1, a1);
        c01 += dot(a1, a2);
        c11 += dot(a2, a2);
        const Point tmp = sub(d[i], bez(p0, p0, p3, p3, t));
        x0 += dot(a1, tmp);
        x1 += dot(a2, tmp);
    }
    const double det = c00 * c11 - c01 * c01;
    double alpha1 = 0, alpha2 = 0;
    if (std::abs(det) > 1e-12) {
        alpha1 = (x0 * c11 - c01 * x1) / det;
        alpha2 = (c00 * x1 - x0 * c01) / det;
    }
    const double segLen = dist(p0, p3), eps = 1e-6 * segLen;
    if (alpha1 < eps || alpha2 < eps) alpha1 = alpha2 = segLen / 3;
    return {p0, add(p0, mul(t1, alpha1)), add(p3, mul(t2, alpha2)), p3};
}

double maxError(const std::vector<Point>& d, std::size_t first, std::size_t last, const Cubic& c, const std::vector<double>& u, std::size_t& split) {
    double best = 0;
    split = (first + last) / 2;
    for (std::size_t i = first + 1; i < last; ++i) {
        const double e = dist(bez(c.p0, c.p1, c.p2, c.p3, u[i - first]), d[i]);
        if (e > best) best = e, split = i;
    }
    return best;
}

void fitCurve(const std::vector<Point>& d, std::size_t first, std::size_t last, Point t1, Point t2, double tol, std::vector<Cubic>& out, int depth) {
    const std::size_t n = last - first + 1;
    if (n == 2) {
        const double l = dist(d[first], d[last]) / 3;
        out.push_back({d[first], add(d[first], mul(t1, l)), add(d[last], mul(t2, l)), d[last]});
        return;
    }
    std::vector<double> u(n, 0);
    for (std::size_t i = 1; i < n; ++i) u[i] = u[i - 1] + dist(d[first + i], d[first + i - 1]);
    const double total = u.back();
    if (total <= 0) {
        out.push_back({d[first], d[first], d[last], d[last]});
        return;
    }
    for (auto& v : u) v /= total;
    Cubic c = fitOne(d, first, last, u, t1, t2);
    std::size_t split = 0;
    double err = maxError(d, first, last, c, u, split);
    if (err < tol) {
        out.push_back(c);
        return;
    }
    if (err < tol * 4) {
        for (int iter = 0; iter < 6; ++iter) {
            for (std::size_t i = 0; i < n; ++i) {  // Newton step towards the closest point
                const Point q = bez(c.p0, c.p1, c.p2, c.p3, u[i]);
                const Point q1 = bezDeriv(c, u[i]), q2 = bezDeriv2(c, u[i]);
                const Point diff = sub(q, d[first + i]);
                const double den = dot(q1, q1) + dot(diff, q2);
                if (std::abs(den) > 1e-12) u[i] = std::clamp(u[i] - dot(diff, q1) / den, 0.0, 1.0);
            }
            c = fitOne(d, first, last, u, t1, t2);
            err = maxError(d, first, last, c, u, split);
            if (err < tol) {
                out.push_back(c);
                return;
            }
        }
    }
    if (depth > 40 || split <= first || split >= last) {
        out.push_back(c);
        return;
    }
    const Point centre = unit(sub(d[split - 1], d[split + 1]));
    fitCurve(d, first, split, t1, centre, tol, out, depth + 1);
    fitCurve(d, split, last, mul(centre, -1), t2, tol, out, depth + 1);
}

}  // namespace

const char* toString(NodeType type) {
    switch (type) {
        case NodeType::Corner: return "corner";
        case NodeType::Smooth: return "smooth";
        case NodeType::Symmetric: return "symmetric";
    }
    return "corner";
}

NodeType nodeType(const Node& n) {
    const Point a = sub(n.p, n.in), b = sub(n.out, n.p);
    const double la = len(a), lb = len(b);
    if (la < 1e-9 || lb < 1e-9) return NodeType::Corner;
    if (std::abs(cross(a, b)) > 1e-3 * la * lb || dot(a, b) <= 0) return NodeType::Corner;
    return std::abs(la - lb) <= 1e-3 * std::max(la, lb) ? NodeType::Symmetric : NodeType::Smooth;
}

BezierPath parsePath(const std::string& d, std::string* error) {
    if (error) error->clear();
    return Reader(d).run(error);
}

std::string pathData(const BezierPath& path) {
    std::string out;
    for (const auto& sp : path) {
        if (sp.nodes.empty()) continue;
        out += (out.empty() ? "M" : " M") + fmt(sp.nodes[0].p.x) + " " + fmt(sp.nodes[0].p.y);
        const std::size_t n = sp.nodes.size(), segs = sp.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const Node& a = sp.nodes[i];
            const Node& b = sp.nodes[(i + 1) % n];
            if (isLine(a, b)) {
                if (sp.closed && i + 1 == n) break;  // Z draws the closing line
                out += " L" + fmt(b.p.x) + " " + fmt(b.p.y);
            } else {
                out += " C" + fmt(a.out.x) + " " + fmt(a.out.y) + " " + fmt(b.in.x) + " " + fmt(b.in.y) + " " + fmt(b.p.x) + " " + fmt(b.p.y);
            }
        }
        if (sp.closed) out += " Z";
    }
    return out;
}

Outline flatten(const BezierPath& path, double detail) {
    Outline out;
    for (const auto& sp : path) {
        if (sp.nodes.empty()) continue;
        Contour c;
        c.closed = sp.closed;
        c.points.push_back(sp.nodes[0].p);
        const std::size_t n = sp.nodes.size(), segs = sp.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const Node& a = sp.nodes[i];
            const Node& b = sp.nodes[(i + 1) % n];
            const bool last = sp.closed && i + 1 == n;
            if (isLine(a, b)) {
                if (!last) c.points.push_back(b.p);
                continue;
            }
            const int k = segmentsFor(dist(a.p, a.out) + dist(a.out, b.in) + dist(b.in, b.p), detail);
            for (int j = 1; j <= k; ++j) {
                if (last && j == k) break;  // the closing point is the first one
                c.points.push_back(j == k ? b.p : bez(a.p, a.out, b.in, b.p, static_cast<double>(j) / k));
            }
        }
        if (c.points.size() >= 2 || (c.closed && !c.points.empty())) out.push_back(std::move(c));
    }
    return out;
}

void transformPath(BezierPath& path, const Matrix& m) {
    for (auto& sp : path) {
        for (auto& n : sp.nodes) {
            n.p = m.apply(n.p);
            n.in = m.apply(n.in);
            n.out = m.apply(n.out);
        }
    }
}

Bounds pathBounds(const BezierPath& path) { return bounds(flatten(path, 4.0)); }

BezierPath shapePath(const Shape& s) {
    BezierPath out;
    switch (s.kind) {
        case ShapeKind::Rect: {
            const double x = std::min(s.x, s.x + s.w), y = std::min(s.y, s.y + s.h);
            const double w = std::abs(s.w), h = std::abs(s.h);
            const double r = std::clamp(s.radius, 0.0, std::min(w, h) / 2);
            SubPath sp;
            sp.closed = true;
            if (r <= 0) {
                sp.nodes = {corner({x, y}), corner({x + w, y}), corner({x + w, y + h}), corner({x, y + h})};
            } else {
                const double k = r * kKappa;
                sp.nodes = {
                    {{x + r, y}, {x + r - k, y}, {x + r, y}},           {{x + w - r, y}, {x + w - r, y}, {x + w - r + k, y}},
                    {{x + w, y + r}, {x + w, y + r - k}, {x + w, y + r}}, {{x + w, y + h - r}, {x + w, y + h - r}, {x + w, y + h - r + k}},
                    {{x + w - r, y + h}, {x + w - r + k, y + h}, {x + w - r, y + h}}, {{x + r, y + h}, {x + r, y + h}, {x + r - k, y + h}},
                    {{x, y + h - r}, {x, y + h - r + k}, {x, y + h - r}}, {{x, y + r}, {x, y + r}, {x, y + r - k}},
                };
            }
            out.push_back(std::move(sp));
            break;
        }
        case ShapeKind::Ellipse: {
            const double rx = std::abs(s.rx), ry = std::abs(s.ry), kx = rx * kKappa, ky = ry * kKappa;
            SubPath sp;
            sp.closed = true;
            sp.nodes = {
                {{s.cx + rx, s.cy}, {s.cx + rx, s.cy - ky}, {s.cx + rx, s.cy + ky}},
                {{s.cx, s.cy + ry}, {s.cx + kx, s.cy + ry}, {s.cx - kx, s.cy + ry}},
                {{s.cx - rx, s.cy}, {s.cx - rx, s.cy + ky}, {s.cx - rx, s.cy - ky}},
                {{s.cx, s.cy - ry}, {s.cx - kx, s.cy - ry}, {s.cx + kx, s.cy - ry}},
            };
            out.push_back(std::move(sp));
            break;
        }
        case ShapeKind::Line:
        case ShapeKind::Polyline:
        case ShapeKind::Polygon: {
            SubPath sp;
            sp.closed = s.kind == ShapeKind::Polygon;
            for (const auto& p : s.points) sp.nodes.push_back(corner(p));
            if (!sp.nodes.empty()) out.push_back(std::move(sp));
            break;
        }
        case ShapeKind::Path: out = parsePath(s.d); break;
        case ShapeKind::Text: out = parsePath(textPathData(s)); break;
        case ShapeKind::Group:
        case ShapeKind::Instance: break;
    }
    return out;
}

Point cubicPoint(const Node& a, const Node& b, double t) { return bez(a.p, a.out, b.in, b.p, t); }

std::size_t splitSegment(SubPath& sp, std::size_t i, double t) {
    const std::size_t n = sp.nodes.size();
    Node& a = sp.nodes[i];
    Node& b = sp.nodes[(i + 1) % n];
    Node mid;
    if (isLine(a, b)) {
        mid = corner(lerp(a.p, b.p, t));
    } else {
        const Point p01 = lerp(a.p, a.out, t), p12 = lerp(a.out, b.in, t), p23 = lerp(b.in, b.p, t);
        const Point p012 = lerp(p01, p12, t), p123 = lerp(p12, p23, t);
        mid = {lerp(p012, p123, t), p012, p123};
        a.out = p01;
        b.in = p23;
    }
    sp.nodes.insert(sp.nodes.begin() + static_cast<std::ptrdiff_t>(i + 1), mid);
    return i + 1;
}

PathHit nearestOnPath(const BezierPath& path, Point p) {
    PathHit best;
    for (std::size_t si = 0; si < path.size(); ++si) {
        const auto& sp = path[si];
        const std::size_t n = sp.nodes.size();
        if (n == 0) continue;
        if (n == 1) {
            const double dd = dist(sp.nodes[0].p, p);
            if (dd < best.distance) best = {si, 0, 0, dd, sp.nodes[0].p};
            continue;
        }
        const std::size_t segs = sp.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const Node& a = sp.nodes[i];
            const Node& b = sp.nodes[(i + 1) % n];
            constexpr int kSteps = 48;
            double bt = 0, bd = 1e300;
            for (int k = 0; k <= kSteps; ++k) {
                const double t = static_cast<double>(k) / kSteps;
                const double dd = dist(cubicPoint(a, b, t), p);
                if (dd < bd) bd = dd, bt = t;
            }
            double lo = std::max(0.0, bt - 1.0 / kSteps), hi = std::min(1.0, bt + 1.0 / kSteps);
            for (int k = 0; k < 40; ++k) {  // golden-section refinement
                const double m1 = lo + (hi - lo) * 0.382, m2 = lo + (hi - lo) * 0.618;
                if (dist(cubicPoint(a, b, m1), p) < dist(cubicPoint(a, b, m2), p)) hi = m2;
                else lo = m1;
            }
            const double t = (lo + hi) / 2;
            const Point at = cubicPoint(a, b, t);
            const double dd = dist(at, p);
            if (dd < best.distance) best = {si, i, t, dd, at};
        }
    }
    return best;
}

void reverse(SubPath& sp) {
    std::reverse(sp.nodes.begin(), sp.nodes.end());
    for (auto& n : sp.nodes) std::swap(n.in, n.out);
    if (sp.closed && !sp.nodes.empty()) std::rotate(sp.nodes.rbegin(), sp.nodes.rbegin() + 1, sp.nodes.rend());  // keep the start
}

SubPath smoothThrough(const std::vector<Point>& pts, bool closed, double tension) {
    SubPath sp;
    sp.closed = closed;
    const std::size_t n = pts.size();
    const double k = std::clamp(tension, 0.0, 1.0) / 3;
    for (std::size_t i = 0; i < n; ++i) {
        Node node = corner(pts[i]);
        const bool hasPrev = closed || i > 0, hasNext = closed || i + 1 < n;
        if (n > 2 && hasPrev && hasNext) {
            const Point prev = pts[(i + n - 1) % n], next = pts[(i + 1) % n];
            const Point dir = unit(sub(next, prev));
            node.in = sub(pts[i], mul(dir, dist(prev, pts[i]) * 2 * k));
            node.out = add(pts[i], mul(dir, dist(next, pts[i]) * 2 * k));
        } else if (n == 2 || !hasPrev || !hasNext) {
            // open ends: aim the handle a third of the way along towards the neighbour
            if (hasNext && n > 1) node.out = lerp(pts[i], pts[i + 1], k);
            if (hasPrev && i > 0) node.in = lerp(pts[i], pts[i - 1], k);
        }
        sp.nodes.push_back(node);
    }
    return sp;
}

SubPath simplify(const std::vector<Point>& input, bool closed, double tolerance, double cornerAngle) {
    // drop repeated points
    std::vector<Point> pts;
    for (const auto& p : input) {
        if (pts.empty() || dist(pts.back(), p) > 1e-9) pts.push_back(p);
    }
    if (closed && pts.size() > 1 && dist(pts.front(), pts.back()) <= 1e-9) pts.pop_back();
    SubPath sp;
    sp.closed = closed;
    const std::size_t n = pts.size();
    if (n < 3) {
        for (const auto& p : pts) sp.nodes.push_back(corner(p));
        return sp;
    }
    tolerance = std::max(tolerance, 1e-3);
    // corners: where the direction turns sharply, looking a little way to each side
    const double cosLimit = std::cos(std::clamp(cornerAngle, 1.0, 179.0) * kPi / 180);
    std::vector<bool> isCorner(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        if (!closed && (i == 0 || i + 1 == n)) {
            isCorner[i] = true;
            continue;
        }
        const Point a = pts[(i + n - 1) % n], b = pts[i], c = pts[(i + 1) % n];
        const Point u = unit(sub(b, a)), v = unit(sub(c, b));
        if (dot(u, v) < cosLimit) isCorner[i] = true;
    }
    std::vector<std::size_t> corners;
    for (std::size_t i = 0; i < n; ++i) {
        if (isCorner[i]) corners.push_back(i);
    }
    // the runs between corners, each fitted on its own (a closed path with no corners is one run)
    std::vector<Point> ring = pts;
    std::size_t start = 0;
    if (closed) {
        start = corners.empty() ? 0 : corners[0];
        std::rotate(ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(start), ring.end());
        ring.push_back(ring.front());
    }
    std::vector<std::size_t> cuts;  // indices into ring
    for (std::size_t c : corners) cuts.push_back((c + n - start) % n);
    if (closed) cuts.push_back(n);
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    if (cuts.empty() || cuts.front() != 0) cuts.insert(cuts.begin(), 0);
    if (!closed && cuts.back() != n - 1) cuts.push_back(n - 1);
    const bool smoothLoop = closed && corners.empty();

    std::vector<Cubic> curves;
    for (std::size_t k = 0; k + 1 < cuts.size(); ++k) {
        const std::size_t a = cuts[k], b = cuts[k + 1];
        if (b <= a) continue;
        Point t1 = unit(sub(ring[a + 1], ring[a])), t2 = unit(sub(ring[b - 1], ring[b]));
        if (smoothLoop) {  // the seam is smooth too: tangents from both sides
            t1 = unit(sub(ring[1], ring[ring.size() - 2]));
            t2 = mul(t1, -1);
        }
        fitCurve(ring, a, b, t1, t2, tolerance, curves, 0);
    }
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const Cubic& c = curves[i];
        if (sp.nodes.empty()) sp.nodes.push_back({c.p0, c.p0, c.p1});
        else sp.nodes.back().out = c.p1;
        sp.nodes.push_back({c.p3, c.p2, c.p3});
    }
    if (closed && sp.nodes.size() > 1) {  // the last node is the first one again
        sp.nodes.front().in = sp.nodes.back().in;
        sp.nodes.pop_back();
    }
    // nearly straight curves become lines
    const std::size_t m = sp.nodes.size();
    for (std::size_t i = 0; i < (closed ? m : m - 1); ++i) {
        Node& a = sp.nodes[i];
        Node& b = sp.nodes[(i + 1) % m];
        const Point ab = sub(b.p, a.p);
        const double l = len(ab);
        if (l < 1e-9) continue;
        const double d1 = std::abs(cross(ab, sub(a.out, a.p))) / l, d2 = std::abs(cross(ab, sub(b.in, a.p))) / l;
        if (d1 < tolerance * 0.05 && d2 < tolerance * 0.05) {
            a.out = a.p;
            b.in = b.p;
        }
    }
    return sp;
}

}  // namespace wetwaresuite::vector
