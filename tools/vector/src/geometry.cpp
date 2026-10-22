// Shape outlines, SVG path parsing and curve flattening.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

constexpr double kPi = 3.14159265358979323846;

double dist(Point a, Point b) { return std::hypot(b.x - a.x, b.y - a.y); }

int segmentsFor(double length, double detail) {
    double n = std::ceil(std::sqrt(std::max(0.0, length * detail)) * 1.5);
    return static_cast<int>(std::clamp(n, 4.0, 256.0));
}

class PathParser {
public:
    PathParser(const std::string& s, double detail) : s_(s), detail_(detail) {}

    Outline run(std::string* error) {
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
                    closePath();
                    continue;
                }
            } else if (cmd == 0 || cmd == 'Z' || cmd == 'z') {
                return fail(error, "expected a command letter");
            }
            if (!segment(cmd)) return fail(error, "bad or missing number for '" + std::string(1, cmd) + "'");
            if (cmd == 'M') cmd = 'L';  // extra coordinate pairs after a moveto are linetos
            if (cmd == 'm') cmd = 'l';
        }
        flush();
        return std::move(out_);
    }

private:
    const std::string& s_;
    double detail_;
    std::size_t i_ = 0;
    Outline out_;
    Contour cur_;
    Point pos_{0, 0}, start_{0, 0}, ctrl_{0, 0};
    char prev_ = 0;  // uppercase letter of the previous segment, for S and T reflection

    Outline fail(std::string* error, const std::string& why) {
        if (error) *error = why + " at position " + std::to_string(i_);
        flush();
        return std::move(out_);
    }

    void skip() {
        while (i_ < s_.size() && (std::isspace(static_cast<unsigned char>(s_[i_])) || s_[i_] == ',')) ++i_;
    }

    bool number(double& v) {
        skip();
        const std::size_t begin = i_;
        auto digit = [&](std::size_t k) { return k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k])); };
        if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
        bool any = false;
        while (digit(i_)) {
            ++i_;
            any = true;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (digit(i_)) {
                ++i_;
                any = true;
            }
        }
        if (!any) {
            i_ = begin;
            return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            std::size_t k = i_ + 1;
            if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
            if (digit(k)) {
                i_ = k;
                while (digit(i_)) ++i_;
            }
        }
        v = std::strtod(s_.substr(begin, i_ - begin).c_str(), nullptr);
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
        if (rel) p = {p.x + pos_.x, p.y + pos_.y};
        return true;
    }

    void flush() {
        if (cur_.points.size() >= 2) out_.push_back(std::move(cur_));
        cur_ = {};
    }

    void lineTo(Point p) {
        if (cur_.points.empty()) cur_.points.push_back(pos_);
        cur_.points.push_back(p);
        pos_ = p;
    }

    void closePath() {
        if (!cur_.points.empty()) {
            cur_.closed = true;
            flush();
        }
        pos_ = start_;
        prev_ = 'Z';
    }

    void cubic(Point c1, Point c2, Point p) {
        const int n = segmentsFor(dist(pos_, c1) + dist(c1, c2) + dist(c2, p), detail_);
        const Point p0 = pos_;
        for (int k = 1; k <= n; ++k) {
            double t = static_cast<double>(k) / n, u = 1 - t;
            double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, d = t * t * t;
            lineTo(k == n ? p : Point{a * p0.x + b * c1.x + c * c2.x + d * p.x, a * p0.y + b * c1.y + c * c2.y + d * p.y});
        }
    }

    void quad(Point c1, Point p) {
        const int n = segmentsFor(dist(pos_, c1) + dist(c1, p), detail_);
        const Point p0 = pos_;
        for (int k = 1; k <= n; ++k) {
            double t = static_cast<double>(k) / n, u = 1 - t;
            lineTo(k == n ? p : Point{u * u * p0.x + 2 * u * t * c1.x + t * t * p.x, u * u * p0.y + 2 * u * t * c1.y + t * t * p.y});
        }
    }

    // SVG arc, endpoint to centre parameterisation (SVG 1.1 F.6.5).
    void arc(double rx, double ry, double angle, bool large, bool sweep, Point p) {
        const Point p0 = pos_;
        if (p0.x == p.x && p0.y == p.y) return;
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
        auto angleBetween = [](double ux, double uy, double vx, double vy) {
            return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
        const double ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
        const double vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
        const double theta1 = angleBetween(1, 0, ux, uy);
        double dtheta = angleBetween(ux, uy, vx, vy);
        if (!sweep && dtheta > 0) dtheta -= 2 * kPi;
        if (sweep && dtheta < 0) dtheta += 2 * kPi;
        const int n = segmentsFor(std::abs(dtheta) * std::max(rx, ry) * 2, detail_);
        for (int k = 1; k <= n; ++k) {
            double t = theta1 + dtheta * k / n;
            double ex = rx * std::cos(t), ey = ry * std::sin(t);
            lineTo(k == n ? p : Point{cx + ex * cs - ey * sn, cy + ex * sn + ey * cs});
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
                cubic(c1, c2, p);
                ctrl_ = c2;
                break;
            case 'S':
                if (!point(c2, rel) || !point(p, rel)) return false;
                c1 = (prev_ == 'C' || prev_ == 'S') ? Point{2 * pos_.x - ctrl_.x, 2 * pos_.y - ctrl_.y} : pos_;
                cubic(c1, c2, p);
                ctrl_ = c2;
                break;
            case 'Q':
                if (!point(c1, rel) || !point(p, rel)) return false;
                quad(c1, p);
                ctrl_ = c1;
                break;
            case 'T':
                if (!point(p, rel)) return false;
                c1 = (prev_ == 'Q' || prev_ == 'T') ? Point{2 * pos_.x - ctrl_.x, 2 * pos_.y - ctrl_.y} : pos_;
                quad(c1, p);
                ctrl_ = c1;
                break;
            case 'A': {
                double rx = 0, ry = 0, angle = 0;
                bool large = false, sweep = false;
                if (!number(rx) || !number(ry) || !number(angle) || !flag(large) || !flag(sweep) || !point(p, rel)) {
                    return false;
                }
                arc(rx, ry, angle, large, sweep, p);
                break;
            }
            default: return false;
        }
        prev_ = up;
        return true;
    }
};

void roundedCorner(std::vector<Point>& pts, double cx, double cy, double r, double fromDeg, int n) {
    for (int k = 0; k <= n; ++k) {
        double t = (fromDeg + 90.0 * k / n) * kPi / 180;
        pts.push_back({cx + r * std::cos(t), cy + r * std::sin(t)});
    }
}

}  // namespace

const char* toString(ShapeKind kind) {
    switch (kind) {
        case ShapeKind::Rect: return "rect";
        case ShapeKind::Ellipse: return "ellipse";
        case ShapeKind::Line: return "line";
        case ShapeKind::Polyline: return "polyline";
        case ShapeKind::Polygon: return "polygon";
        case ShapeKind::Path: return "path";
        case ShapeKind::Text: return "text";
        case ShapeKind::Group: return "group";
        case ShapeKind::Instance: return "instance";
    }
    return "shape";
}

Matrix Matrix::operator*(const Matrix& m) const {
    return {a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
}

Matrix Matrix::rotate(double degrees) {
    const double t = degrees * kPi / 180, cs = std::cos(t), sn = std::sin(t);
    return {cs, sn, -sn, cs, 0, 0};
}

Matrix Matrix::skew(double xDegrees, double yDegrees) {
    return {1, std::tan(yDegrees * kPi / 180), std::tan(xDegrees * kPi / 180), 1, 0, 0};
}

double Matrix::scaleFactor() const { return std::sqrt(std::abs(a * d - b * c)); }

bool Matrix::invert(Matrix& out) const {
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-14) return false;
    out = {d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det};
    return true;
}

void Bounds::add(Point p) {
    if (empty) {
        *this = {p.x, p.y, p.x, p.y, false};
        return;
    }
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
}

void Bounds::add(const Bounds& b) {
    if (b.empty) return;
    add(Point{b.x0, b.y0});
    add(Point{b.x1, b.y1});
}

Bounds bounds(const Outline& outline) {
    Bounds b;
    for (const auto& c : outline) {
        for (const auto& p : c.points) {
            if (b.empty) {
                b = {p.x, p.y, p.x, p.y, false};
                continue;
            }
            b.x0 = std::min(b.x0, p.x);
            b.y0 = std::min(b.y0, p.y);
            b.x1 = std::max(b.x1, p.x);
            b.y1 = std::max(b.y1, p.y);
        }
    }
    return b;
}

Outline flattenPath(const std::string& d, double detail, std::string* error) {
    if (error) error->clear();
    return PathParser(d, detail).run(error);
}

Outline localOutline(const Shape& s, double detail) {
    Outline out;
    switch (s.kind) {
        case ShapeKind::Rect: {
            double x = std::min(s.x, s.x + s.w), y = std::min(s.y, s.y + s.h);
            double w = std::abs(s.w), h = std::abs(s.h);
            double r = std::clamp(s.radius, 0.0, std::min(w, h) / 2);
            Contour c{{}, true};
            if (r <= 0) {
                c.points = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
            } else {
                int n = std::clamp(static_cast<int>(std::ceil(r * detail / 2)), 3, 64);
                roundedCorner(c.points, x + w - r, y + r, r, 270, n);
                roundedCorner(c.points, x + w - r, y + h - r, r, 0, n);
                roundedCorner(c.points, x + r, y + h - r, r, 90, n);
                roundedCorner(c.points, x + r, y + r, r, 180, n);
            }
            out.push_back(std::move(c));
            break;
        }
        case ShapeKind::Ellipse: {
            double rx = std::abs(s.rx), ry = std::abs(s.ry);
            // a multiple of 4 so the extreme points (and the bounding box) are exact
            int n = std::clamp(static_cast<int>(std::max(rx, ry) * detail * 0.75) + 12, 16, 720) / 4 * 4;
            Contour c{{}, true};
            for (int k = 0; k < n; ++k) {
                double t = 2 * kPi * k / n;
                c.points.push_back({s.cx + rx * std::cos(t), s.cy + ry * std::sin(t)});
            }
            out.push_back(std::move(c));
            break;
        }
        case ShapeKind::Line:
        case ShapeKind::Polyline:
            if (!s.points.empty()) out.push_back({s.points, false});
            break;
        case ShapeKind::Polygon:
            if (!s.points.empty()) out.push_back({s.points, true});
            break;
        case ShapeKind::Path: out = flattenPath(s.d, detail); break;
        case ShapeKind::Text:
            out = flattenPath(textPathData(s), detail);
            for (auto& c : out) c.closed = true;
            break;
        case ShapeKind::Instance:  // the symbol's box (outlineIn draws the symbol itself)
            if (s.w > 0 && s.h > 0) out.push_back({{{s.x, s.y}, {s.x + s.w, s.y}, {s.x + s.w, s.y + s.h}, {s.x, s.y + s.h}}, true});
            break;
        case ShapeKind::Group: break;
    }
    return out;
}

Matrix shapeMatrix(const Shape& s) {
    const Transform& t = s.transform;
    if (t.identity()) return {};
    Bounds b = bounds(localOutline(s, 1.0));
    double cx = b.empty ? 0 : (b.x0 + b.x1) / 2, cy = b.empty ? 0 : (b.y0 + b.y1) / 2;
    return Matrix::translate(t.tx + cx, t.ty + cy) * Matrix::rotate(t.rotate) * Matrix::scale(t.sx, t.sy) *
           Matrix::translate(-cx, -cy);
}

Outline outline(const Shape& s, double detail) {
    const Transform& t = s.transform;
    Outline out = localOutline(s, detail * std::max({std::abs(t.sx), std::abs(t.sy), 1e-6}));
    if (t.identity()) return out;
    const Matrix m = shapeMatrix(s);
    for (auto& c : out) {
        for (auto& p : c.points) p = m.apply(p);
    }
    return out;
}

}  // namespace wetwaresuite::vector
