// Sketch data, constraint equations, the solve and profile extraction.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <stdexcept>

#include "wetwaresuite/cad/cad.hpp"
#include "sketch_solve.hpp"
#include "solver.hpp"

namespace wetwaresuite::cad {

using mesh::Vec2;
using mesh::Vec3;

// ---- data -------------------------------------------------------------------

SketchPoint* Sketch::point(const std::string& id) {
    for (auto& p : points) {
        if (p.id == id) return &p;
    }
    return nullptr;
}
const SketchPoint* Sketch::point(const std::string& id) const { return const_cast<Sketch*>(this)->point(id); }
SketchEntity* Sketch::entity(const std::string& id) {
    for (auto& e : entities) {
        if (e.id == id) return &e;
    }
    return nullptr;
}
const SketchEntity* Sketch::entity(const std::string& id) const { return const_cast<Sketch*>(this)->entity(id); }
SketchConstraint* Sketch::constraint(const std::string& id) {
    for (auto& c : constraints) {
        if (c.id == id) return &c;
    }
    return nullptr;
}
std::string Sketch::newId(const std::string& prefix) {
    for (;;) {
        std::string id = prefix + std::to_string(++counters[prefix]);
        if (!point(id) && !entity(id) && !constraint(id)) return id;
    }
}

mesh::Mat4 Frame::matrix() const {
    mesh::Mat4 m;
    const Vec3 cols[4] = {u, v, n, origin};
    for (int c = 0; c < 4; ++c) {
        m.at(0, c) = cols[c].x;
        m.at(1, c) = cols[c].y;
        m.at(2, c) = cols[c].z;
    }
    return m;
}

std::string SolvedSketch::status() const {
    std::string s = dof == 0 ? "fully defined" : "under defined (" + std::to_string(dof) + (dof == 1 ? " degree" : " degrees") + " of freedom)";
    if (redundant > 0) s += ", " + std::to_string(redundant) + " redundant";
    return s;
}

const std::vector<std::string>& geometricConstraints() {
    static const std::vector<std::string> t = {"coincident", "horizontal", "vertical",  "parallel",  "perpendicular", "tangent",
                                               "equal",      "fixed",      "midpoint",  "concentric", "symmetric",    "collinear"};
    return t;
}
const std::vector<std::string>& dimensionalConstraints() {
    static const std::vector<std::string> t = {"distance", "hdistance", "vdistance", "length", "radius", "diameter", "angle"};
    return t;
}

namespace detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrapAngle(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}

std::string fmt(double v) {
    if (std::fabs(v) < 5e-10) v = 0;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// Variables: every point's x, y (the built-in origin and axis ends locked), every circle's radius.
struct Model {
    const Sketch& sk;
    std::map<std::string, int> pointIndex;
    std::map<std::string, int> radiusVar;
    std::vector<double> x;
    std::vector<bool> locked;

    explicit Model(const Sketch& s) : sk(s) {
        addPoint("origin", 0, 0, true);
        addPoint("#xend", 1, 0, true);
        addPoint("#yend", 0, 1, true);
        for (const auto& p : sk.points) addPoint(p.id, p.x, p.y, false);
        for (const auto& e : sk.entities) {
            if (e.kind == "circle") {
                radiusVar[e.id] = static_cast<int>(x.size());
                x.push_back(e.radius);
                locked.push_back(false);
            }
        }
    }
    void addPoint(const std::string& id, double px, double py, bool lock) {
        pointIndex[id] = static_cast<int>(x.size() / 2);
        x.push_back(px);
        x.push_back(py);
        locked.push_back(lock);
        locked.push_back(lock);
    }

    struct R {
        RefKind kind = RefKind::Point;
        std::string id;
        std::vector<int> pts;  // point: {p}; line: {a, b}; circle: {c}; arc: {c, s, e}; spline: all
        int radius = -1;       // circle radius variable
    };

    int pointOf(const std::string& id) const {
        auto it = pointIndex.find(id);
        if (it == pointIndex.end()) throw std::invalid_argument("the sketch has no point '" + id + "'");
        return it->second;
    }

    R ref(const std::string& text) const {
        R r;
        r.id = text;
        if (text == "xaxis" || text == "yaxis") {
            r.kind = RefKind::Line;
            r.pts = {0, text == "xaxis" ? 1 : 2};
            return r;
        }
        auto dot = text.find('.');
        if (dot != std::string::npos) {
            const std::string eid = text.substr(0, dot), part = text.substr(dot + 1);
            const SketchEntity* e = sk.entity(eid);
            if (!e) throw std::invalid_argument("the sketch has no entity '" + eid + "'");
            std::string pid;
            if ((part == "start" || part == "p1") && (e->kind == "line" || e->kind == "spline")) pid = e->points.front();
            else if ((part == "end" || part == "p2") && e->kind == "line") pid = e->points.back();
            else if (part == "end" && e->kind == "spline") pid = e->points.back();
            else if (part == "center" && (e->kind == "circle" || e->kind == "arc")) pid = e->points[0];
            else if (part == "start" && e->kind == "arc") pid = e->points[1];
            else if (part == "end" && e->kind == "arc") pid = e->points[2];
            else if (part.size() > 1 && part[0] == 'p' && e->kind == "spline") {
                const int k = std::atoi(part.c_str() + 1);
                if (k >= 1 && k <= static_cast<int>(e->points.size())) pid = e->points[static_cast<std::size_t>(k - 1)];
            }
            if (pid.empty()) throw std::invalid_argument("'" + text + "': a " + e->kind + " has " +
                                                         (e->kind == "line" ? "start, end" : e->kind == "circle" ? "center" : e->kind == "arc" ? "center, start, end" : "start, end, p1, p2, ..."));
            r.kind = RefKind::Point;
            r.pts = {pointOf(pid)};
            return r;
        }
        if (pointIndex.count(text) && text[0] != '#') {
            r.kind = RefKind::Point;
            r.pts = {pointOf(text)};
            return r;
        }
        const SketchEntity* e = sk.entity(text);
        if (!e) throw std::invalid_argument("the sketch has no point or entity '" + text + "' (cad.sketch_info lists them)");
        for (const auto& p : e->points) r.pts.push_back(pointOf(p));
        if (e->kind == "line") r.kind = RefKind::Line;
        else if (e->kind == "circle") {
            r.kind = RefKind::Circle;
            r.radius = radiusVar.at(e->id);
        } else if (e->kind == "arc") r.kind = RefKind::Arc;
        else r.kind = RefKind::Spline;
        return r;
    }

    static Vec2 P(const std::vector<double>& x, int p) { return {x[static_cast<std::size_t>(2 * p)], x[static_cast<std::size_t>(2 * p + 1)]}; }
    static Vec2 center(const std::vector<double>& x, const R& r) { return P(x, r.pts[0]); }
    static double radius(const std::vector<double>& x, const R& r) {
        if (r.kind == RefKind::Circle) return x[static_cast<std::size_t>(r.radius)];
        return mesh::length(P(x, r.pts[1]) - P(x, r.pts[0]));
    }
    static std::vector<int> vars(const R& r) {
        std::vector<int> v;
        for (int p : r.pts) v.insert(v.end(), {2 * p, 2 * p + 1});
        if (r.radius >= 0) v.push_back(r.radius);
        return v;
    }
};

using R = Model::R;

bool curve(const R& r) { return r.kind == RefKind::Circle || r.kind == RefKind::Arc; }

std::vector<int> join(std::vector<int> a, const std::vector<int>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

double signedDist(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 u = b - a;
    const double l = mesh::length(u);
    return l > 0 ? mesh::cross(u, p - a) / l : mesh::length(p - a);
}

double sgn(double v) { return v < 0 ? -1.0 : 1.0; }

// The point a distance-type reference measures from: a point, or a circle's centre.
bool pointLike(const R& r) { return r.kind == RefKind::Point || curve(r); }
int pointIndexOf(const R& r) { return r.pts[0]; }

[[noreturn]] void expects(const SketchConstraint& c, const std::string& what) {
    throw std::invalid_argument(c.type + " takes " + what);
}

struct Builder {
    const Model& m;
    std::vector<Equation>& eqs;
    int owner;

    void add(std::vector<int> vars, std::function<double(const std::vector<double>&)> f) {
        eqs.push_back({std::move(vars), std::move(f), owner});
    }
    void samePoint(int p, int q) {
        add({2 * p, 2 * q}, [p, q](const std::vector<double>& x) { return x[static_cast<std::size_t>(2 * p)] - x[static_cast<std::size_t>(2 * q)]; });
        add({2 * p + 1, 2 * q + 1},
            [p, q](const std::vector<double>& x) { return x[static_cast<std::size_t>(2 * p + 1)] - x[static_cast<std::size_t>(2 * q + 1)]; });
    }
    void onLine(int p, const R& l) {
        const int a = l.pts[0], b = l.pts[1];
        add(join({2 * p, 2 * p + 1}, Model::vars(l)), [p, a, b](const std::vector<double>& x) {
            return signedDist(Model::P(x, p), Model::P(x, a), Model::P(x, b));
        });
    }
    void onCurve(int p, const R& c) {
        add(join({2 * p, 2 * p + 1}, Model::vars(c)), [p, c](const std::vector<double>& x) {
            return mesh::length(Model::P(x, p) - Model::center(x, c)) - Model::radius(x, c);
        });
    }
};

// The point two entities share as an end (a line's ends, an arc's start or end), or -1.
int sharedEnd(const R& a, const R& b) {
    auto ends = [](const R& r) -> std::vector<int> {
        if (r.kind == RefKind::Line) return {r.pts[0], r.pts[1]};
        if (r.kind == RefKind::Arc) return {r.pts[1], r.pts[2]};
        return {};
    };
    for (int p : ends(a)) {
        for (int q : ends(b)) {
            if (p == q) return p;
        }
    }
    return -1;
}

void equationsFor(const Model& m, const SketchConstraint& c, int owner, const Evaluator& eval, std::vector<Equation>& eqs) {
    Builder b{m, eqs, owner};
    std::vector<R> refs;
    for (const auto& t : c.refs) refs.push_back(m.ref(t));
    const std::string& t = c.type;
    auto value = [&]() {
        if (c.value.empty()) throw std::invalid_argument(t + " needs a value");
        try {
            return eval(c.value);
        } catch (const std::exception& e) {
            throw std::invalid_argument(t + " " + c.id + ": " + e.what());
        }
    };
    auto count = [&](std::size_t lo, std::size_t hi, const std::string& what) {
        if (refs.size() < lo || refs.size() > hi) expects(c, what);
    };
    const double aux = c.aux;

    if (t == "coincident") {
        count(2, 2, "two refs: two points, or a point and a line, circle or arc");
        const R* p = refs[0].kind == RefKind::Point ? &refs[0] : &refs[1];
        const R* o = p == &refs[0] ? &refs[1] : &refs[0];
        if (p->kind != RefKind::Point) expects(c, "a point and a point, line, circle or arc");
        if (o->kind == RefKind::Point) b.samePoint(p->pts[0], o->pts[0]);
        else if (o->kind == RefKind::Line) b.onLine(p->pts[0], *o);
        else if (curve(*o)) b.onCurve(p->pts[0], *o);
        else expects(c, "a point and a point, line, circle or arc (not a spline)");
    } else if (t == "horizontal" || t == "vertical") {
        const int axis = t == "horizontal" ? 1 : 0;
        std::vector<std::pair<int, int>> pairs;
        if (refs.size() == 2 && refs[0].kind == RefKind::Point && refs[1].kind == RefKind::Point) {
            pairs.emplace_back(refs[0].pts[0], refs[1].pts[0]);
        } else {
            if (refs.empty()) expects(c, "lines, or two points");
            for (const auto& r : refs) {
                if (r.kind != RefKind::Line) expects(c, "lines, or two points");
                pairs.emplace_back(r.pts[0], r.pts[1]);
            }
        }
        for (auto [p, q] : pairs) {
            b.add({2 * p + axis, 2 * q + axis}, [p, q, axis](const std::vector<double>& x) {
                return x[static_cast<std::size_t>(2 * p + axis)] - x[static_cast<std::size_t>(2 * q + axis)];
            });
        }
    } else if (t == "parallel" || t == "perpendicular") {
        count(2, t == "parallel" ? 32 : 2, "two lines");
        for (const auto& r : refs) {
            if (r.kind != RefKind::Line) expects(c, "lines");
        }
        const bool par = t == "parallel";
        for (std::size_t i = 1; i < refs.size(); ++i) {
            const R l1 = refs[0], l2 = refs[i];
            b.add(join(Model::vars(l1), Model::vars(l2)), [l1, l2, par](const std::vector<double>& x) {
                const Vec2 u = Model::P(x, l1.pts[1]) - Model::P(x, l1.pts[0]), v = Model::P(x, l2.pts[1]) - Model::P(x, l2.pts[0]);
                const double d = mesh::length(u) * mesh::length(v);
                if (d == 0) return 0.0;
                return (par ? mesh::cross(u, v) : mesh::dot(u, v)) / d;
            });
        }
    } else if (t == "collinear") {
        count(2, 2, "two lines");
        if (refs[0].kind != RefKind::Line || refs[1].kind != RefKind::Line) expects(c, "two lines");
        b.onLine(refs[1].pts[0], refs[0]);
        b.onLine(refs[1].pts[1], refs[0]);
    } else if (t == "tangent") {
        count(2, 2, "a line and a circle or arc, or two circles/arcs");
        const R* l = refs[0].kind == RefKind::Line ? &refs[0] : refs[1].kind == RefKind::Line ? &refs[1] : nullptr;
        if (l) {
            const R& cv = l == &refs[0] ? refs[1] : refs[0];
            if (!curve(cv)) expects(c, "a line and a circle or arc");
            const R line = *l, circle = cv;
            const double side = aux == 0 ? 1 : aux;
            // an arc ending on the line touches it there: its radius at that point is square to the line. (The
            // distance form is flat at that solution, which reads as a redundant constraint.)
            const int shared = sharedEnd(line, circle);
            if (shared >= 0) {
                b.add(join(Model::vars(line), Model::vars(circle)), [line, circle, shared](const std::vector<double>& x) {
                    const Vec2 r = Model::P(x, shared) - Model::center(x, circle), d = Model::P(x, line.pts[1]) - Model::P(x, line.pts[0]);
                    const double lr = mesh::length(r), ld = mesh::length(d);
                    return lr < 1e-300 || ld < 1e-300 ? 0.0 : mesh::dot(r, d) / ld;
                });
                return;
            }
            b.add(join(Model::vars(line), Model::vars(circle)), [line, circle, side](const std::vector<double>& x) {
                return side * signedDist(Model::center(x, circle), Model::P(x, line.pts[0]), Model::P(x, line.pts[1])) - Model::radius(x, circle);
            });
        } else {
            if (!curve(refs[0]) || !curve(refs[1])) expects(c, "a line and a circle or arc, or two circles/arcs");
            const R c1 = refs[0], c2 = refs[1];
            // two arcs meeting at an end: the centres and that end are in line
            const int shared = sharedEnd(c1, c2);
            if (shared >= 0) {
                b.add(join(Model::vars(c1), Model::vars(c2)), [c1, c2, shared](const std::vector<double>& x) {
                    const Vec2 a = Model::P(x, shared) - Model::center(x, c1), e = Model::P(x, shared) - Model::center(x, c2);
                    const double la = mesh::length(a), le = mesh::length(e);
                    return la < 1e-300 || le < 1e-300 ? 0.0 : mesh::cross(a, e) / le;
                });
                return;
            }
            b.add(join(Model::vars(c1), Model::vars(c2)), [c1, c2, aux](const std::vector<double>& x) {
                const double d = mesh::length(Model::center(x, c1) - Model::center(x, c2));
                const double r1 = Model::radius(x, c1), r2 = Model::radius(x, c2);
                if (aux == 0) return d - (r1 + r2);
                return d - aux / 2 * (r1 - r2);
            });
        }
    } else if (t == "equal") {
        count(2, 32, "two or more lines, or two or more circles/arcs");
        const bool lines = refs[0].kind == RefKind::Line;
        for (const auto& r : refs) {
            if (lines ? r.kind != RefKind::Line : !curve(r)) expects(c, "lines only, or circles/arcs only");
        }
        for (std::size_t i = 1; i < refs.size(); ++i) {
            const R a = refs[0], o = refs[i];
            b.add(join(Model::vars(a), Model::vars(o)), [a, o, lines](const std::vector<double>& x) {
                if (lines) {
                    return mesh::length(Model::P(x, a.pts[1]) - Model::P(x, a.pts[0])) - mesh::length(Model::P(x, o.pts[1]) - Model::P(x, o.pts[0]));
                }
                return Model::radius(x, a) - Model::radius(x, o);
            });
        }
    } else if (t == "fixed") {
        count(1, 1, "one point or entity");
        const auto items = splitList(c.value);
        const R r = refs[0];
        std::vector<double> vals;
        for (const auto& it : items) {
            try {
                vals.push_back(eval(it));
            } catch (const std::exception& e) {
                throw std::invalid_argument("fixed " + c.id + ": " + e.what());
            }
        }
        const std::size_t need = r.pts.size() * 2 + (r.radius >= 0 ? 1 : 0);
        if (vals.size() != need) throw std::invalid_argument("fixed " + c.id + ": value needs " + std::to_string(need) + " numbers");
        for (std::size_t k = 0; k < r.pts.size(); ++k) {
            for (int a = 0; a < 2; ++a) {
                const int var = 2 * r.pts[k] + a;
                const double target = vals[k * 2 + static_cast<std::size_t>(a)];
                b.add({var}, [var, target](const std::vector<double>& x) { return x[static_cast<std::size_t>(var)] - target; });
            }
        }
        if (r.radius >= 0) {
            const int var = r.radius;
            const double target = vals.back();
            b.add({var}, [var, target](const std::vector<double>& x) { return x[static_cast<std::size_t>(var)] - target; });
        }
    } else if (t == "midpoint") {
        count(2, 2, "a point and a line");
        const R* p = refs[0].kind == RefKind::Point ? &refs[0] : &refs[1];
        const R* l = p == &refs[0] ? &refs[1] : &refs[0];
        if (p->kind != RefKind::Point || l->kind != RefKind::Line) expects(c, "a point and a line");
        const int pi = p->pts[0], a = l->pts[0], bb = l->pts[1];
        for (int ax = 0; ax < 2; ++ax) {
            b.add({2 * pi + ax, 2 * a + ax, 2 * bb + ax}, [pi, a, bb, ax](const std::vector<double>& x) {
                return x[static_cast<std::size_t>(2 * pi + ax)] - (x[static_cast<std::size_t>(2 * a + ax)] + x[static_cast<std::size_t>(2 * bb + ax)]) / 2;
            });
        }
    } else if (t == "concentric") {
        count(2, 2, "two circles/arcs, or a point and a circle/arc");
        if (!pointLike(refs[0]) || !pointLike(refs[1]) || (!curve(refs[0]) && !curve(refs[1]))) {
            expects(c, "two circles/arcs, or a point and a circle/arc");
        }
        b.samePoint(pointIndexOf(refs[0]), pointIndexOf(refs[1]));
    } else if (t == "symmetric") {
        count(3, 3, "two points and a line to mirror them about");
        if (refs[0].kind != RefKind::Point || refs[1].kind != RefKind::Point || refs[2].kind != RefKind::Line) {
            expects(c, "two points and a line (p1,p2,l1)");
        }
        const int p = refs[0].pts[0], q = refs[1].pts[0], a = refs[2].pts[0], bb = refs[2].pts[1];
        const std::vector<int> vars = {2 * p, 2 * p + 1, 2 * q, 2 * q + 1, 2 * a, 2 * a + 1, 2 * bb, 2 * bb + 1};
        b.add(vars, [p, q, a, bb](const std::vector<double>& x) {
            return signedDist((Model::P(x, p) + Model::P(x, q)) * 0.5, Model::P(x, a), Model::P(x, bb));
        });
        b.add(vars, [p, q, a, bb](const std::vector<double>& x) {
            const Vec2 u = Model::P(x, bb) - Model::P(x, a);
            const double l = mesh::length(u);
            return l > 0 ? mesh::dot(Model::P(x, q) - Model::P(x, p), u) / l : 0.0;
        });
    } else if (t == "distance") {
        count(2, 2, "two refs: points (or circle centres), a point and a line, or two lines");
        const double d = value();
        if (d < 0) throw std::invalid_argument("distance can't be negative");
        const R r0 = refs[0], r1 = refs[1];
        if (pointLike(r0) && pointLike(r1)) {
            const int p = pointIndexOf(r0), q = pointIndexOf(r1);
            b.add({2 * p, 2 * p + 1, 2 * q, 2 * q + 1}, [p, q, d](const std::vector<double>& x) { return mesh::length(Model::P(x, p) - Model::P(x, q)) - d; });
        } else if (r0.kind == RefKind::Line && r1.kind == RefKind::Line) {
            const double s = aux == 0 ? 1 : aux;
            for (int k = 0; k < 2; ++k) {
                const int p = r1.pts[static_cast<std::size_t>(k)];
                b.add(join({2 * p, 2 * p + 1}, Model::vars(r0)), [p, r0, d, s](const std::vector<double>& x) {
                    return s * signedDist(Model::P(x, p), Model::P(x, r0.pts[0]), Model::P(x, r0.pts[1])) - d;
                });
            }
        } else {
            const R& l = r0.kind == RefKind::Line ? r0 : r1;
            const R& o = r0.kind == RefKind::Line ? r1 : r0;
            if (l.kind != RefKind::Line || !pointLike(o)) expects(c, "two points, a point and a line, or two lines");
            const int p = pointIndexOf(o);
            const double s = aux == 0 ? 1 : aux;
            const R line = l;
            b.add(join({2 * p, 2 * p + 1}, Model::vars(line)), [p, line, d, s](const std::vector<double>& x) {
                return s * signedDist(Model::P(x, p), Model::P(x, line.pts[0]), Model::P(x, line.pts[1])) - d;
            });
        }
    } else if (t == "hdistance" || t == "vdistance") {
        const int axis = t == "hdistance" ? 0 : 1;
        int p = -1, q = -1;
        if (refs.size() == 1 && refs[0].kind == RefKind::Line) {
            p = refs[0].pts[0];
            q = refs[0].pts[1];
        } else if (refs.size() == 2 && pointLike(refs[0]) && pointLike(refs[1])) {
            p = pointIndexOf(refs[0]);
            q = pointIndexOf(refs[1]);
        } else {
            expects(c, "two points (or circle centres), or one line");
        }
        const double d = value(), s = aux == 0 ? 1 : aux;
        b.add({2 * p + axis, 2 * q + axis}, [p, q, axis, d, s](const std::vector<double>& x) {
            return s * (x[static_cast<std::size_t>(2 * q + axis)] - x[static_cast<std::size_t>(2 * p + axis)]) - d;
        });
    } else if (t == "length") {
        count(1, 1, "one line");
        if (refs[0].kind != RefKind::Line) expects(c, "one line");
        const double d = value();
        if (!(d > 0)) throw std::invalid_argument("length must be positive");
        const R l = refs[0];
        b.add(Model::vars(l), [l, d](const std::vector<double>& x) { return mesh::length(Model::P(x, l.pts[1]) - Model::P(x, l.pts[0])) - d; });
    } else if (t == "radius" || t == "diameter") {
        count(1, 1, "one circle or arc");
        if (!curve(refs[0])) expects(c, "one circle or arc");
        const double d = value() / (t == "diameter" ? 2 : 1);
        if (!(d > 0)) throw std::invalid_argument(t + " must be positive");
        const R cv = refs[0];
        b.add(Model::vars(cv), [cv, d](const std::vector<double>& x) { return Model::radius(x, cv) - d; });
    } else if (t == "angle") {
        count(1, 2, "two lines, or one line (measured from the x axis)");
        const R l1 = refs.size() == 2 ? refs[0] : m.ref("xaxis");
        const R l2 = refs.size() == 2 ? refs[1] : refs[0];
        if (l1.kind != RefKind::Line || l2.kind != RefKind::Line) expects(c, "two lines, or one line");
        const double theta = value() * kPi / 180;
        const double side = aux == 0 ? 1 : (aux > 0 ? 1 : -1);
        const bool flip = std::fabs(aux) > 1.5;
        b.add(join(Model::vars(l1), Model::vars(l2)), [l1, l2, theta, side, flip](const std::vector<double>& x) {
            const Vec2 u = Model::P(x, l1.pts[1]) - Model::P(x, l1.pts[0]);
            Vec2 v = Model::P(x, l2.pts[1]) - Model::P(x, l2.pts[0]);
            if (flip) v = v * -1;
            return wrapAngle(std::atan2(mesh::cross(u, v), mesh::dot(u, v)) - side * theta);
        });
    } else {
        throw std::invalid_argument("unknown constraint type '" + t + "'");
    }
}

// The value a dimension measures now.
double measure(const Model& m, const SketchConstraint& c, const std::vector<double>& x) {
    std::vector<R> refs;
    for (const auto& t : c.refs) refs.push_back(m.ref(t));
    const std::string& t = c.type;
    if (t == "distance") {
        if (pointLike(refs[0]) && pointLike(refs[1])) return mesh::length(Model::P(x, pointIndexOf(refs[0])) - Model::P(x, pointIndexOf(refs[1])));
        const R& l = refs[0].kind == RefKind::Line ? refs[0] : refs[1];
        const R& o = refs[0].kind == RefKind::Line ? refs[1] : refs[0];
        const int p = o.kind == RefKind::Line ? o.pts[0] : pointIndexOf(o);
        return std::fabs(signedDist(Model::P(x, p), Model::P(x, l.pts[0]), Model::P(x, l.pts[1])));
    }
    if (t == "hdistance" || t == "vdistance") {
        const int axis = t == "hdistance" ? 0 : 1;
        const int p = refs.size() == 1 ? refs[0].pts[0] : pointIndexOf(refs[0]);
        const int q = refs.size() == 1 ? refs[0].pts[1] : pointIndexOf(refs[1]);
        return std::fabs(x[static_cast<std::size_t>(2 * q + axis)] - x[static_cast<std::size_t>(2 * p + axis)]);
    }
    if (t == "length") return mesh::length(Model::P(x, refs[0].pts[1]) - Model::P(x, refs[0].pts[0]));
    if (t == "radius") return Model::radius(x, refs[0]);
    if (t == "diameter") return 2 * Model::radius(x, refs[0]);
    if (t == "angle") {
        const R l1 = refs.size() == 2 ? refs[0] : m.ref("xaxis");
        const R l2 = refs.size() == 2 ? refs[1] : refs[0];
        Vec2 u = Model::P(x, l1.pts[1]) - Model::P(x, l1.pts[0]), v = Model::P(x, l2.pts[1]) - Model::P(x, l2.pts[0]);
        if (std::fabs(c.aux) > 1.5) v = v * -1;
        return std::fabs(std::atan2(mesh::cross(u, v), mesh::dot(u, v))) * 180 / kPi;
    }
    return 0;
}

// ---- profiles -----------------------------------------------------------------

std::vector<Vec2> arcPoints(Vec2 c, Vec2 s, Vec2 e, int segments) {
    const double r = mesh::length(s - c);
    const double a0 = std::atan2(s.y - c.y, s.x - c.x);
    double sweep = std::atan2(e.y - c.y, e.x - c.x) - a0;
    while (sweep <= 1e-12) sweep += 2 * kPi;
    const int n = std::max(2, static_cast<int>(std::ceil(sweep / (2 * kPi) * segments - 1e-9)));
    std::vector<Vec2> out{s};
    for (int i = 1; i < n; ++i) {
        const double a = a0 + sweep * i / n;
        out.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    out.push_back(e);
    return out;
}

std::vector<Vec2> splinePoints(const std::vector<Vec2>& p, bool closed, int perSpan) {
    std::vector<Vec2> out;
    const int n = static_cast<int>(p.size());
    if (n < 2) return p;
    auto at = [&](int i) {
        if (closed) return p[static_cast<std::size_t>(((i % n) + n) % n)];
        if (i < 0) return p[0] * 2 - p[1];
        if (i >= n) return p[static_cast<std::size_t>(n - 1)] * 2 - p[static_cast<std::size_t>(n - 2)];
        return p[static_cast<std::size_t>(i)];
    };
    const int spans = closed ? n : n - 1;
    for (int s = 0; s < spans; ++s) {
        const Vec2 p0 = at(s - 1), p1 = at(s), p2 = at(s + 1), p3 = at(s + 2);
        for (int k = 0; k < perSpan; ++k) {
            const double t = static_cast<double>(k) / perSpan, t2 = t * t, t3 = t2 * t;
            out.push_back((p1 * 2 + (p2 - p0) * t + (p0 * 2 - p1 * 5 + p2 * 4 - p3) * t2 + (p1 * 3 - p0 - p2 * 3 + p3) * t3) * 0.5);
        }
    }
    if (!closed) out.push_back(p.back());
    return out;
}

bool insideLoop(const std::vector<Vec2>& loop, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        const Vec2 a = loop[i], b = loop[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

double area(const std::vector<Vec2>& loop) {
    double s = 0;
    for (std::size_t i = 0; i < loop.size(); ++i) s += mesh::cross(loop[i], loop[(i + 1) % loop.size()]);
    return s / 2;
}

bool segmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d, double eps) {
    const double d1 = mesh::cross(b - a, c - a), d2 = mesh::cross(b - a, d - a);
    const double d3 = mesh::cross(d - c, a - c), d4 = mesh::cross(d - c, b - c);
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

void extractProfiles(const Sketch& sk, SolvedSketch& out, int segments) {
    struct Edge {
        std::vector<Vec2> pts;
        std::string a, b;
    };
    std::vector<Edge> edges;
    std::vector<std::vector<Vec2>> closedLoops;
    double extent = 1;
    for (const auto& [id, p] : out.points) extent = std::max({extent, std::fabs(p.x), std::fabs(p.y)});
    const double tol = 1e-7 * extent;
    for (const auto& e : sk.entities) {
        if (e.construction) continue;
        if (e.kind == "circle") {
            const Vec2 c = out.points.at(e.points[0]);
            const double r = out.radii.at(e.id);
            std::vector<Vec2> loop;
            for (int i = 0; i < segments; ++i) {
                const double a = 2 * kPi * i / segments;
                loop.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
            }
            closedLoops.push_back(loop);
            out.circleCenters.push_back(c);
            out.circleRadii.push_back(r);
            continue;
        }
        auto pts = entityOutline(e, out, segments);
        if (e.kind == "spline" && e.closed) {
            closedLoops.push_back(pts);
            continue;
        }
        if (pts.size() < 2) continue;
        edges.push_back({pts, e.points[e.kind == "arc" ? 1 : 0], e.points[e.kind == "arc" ? 2 : e.points.size() - 1]});
    }
    // join points that ended up in the same place
    std::vector<std::string> ids;
    for (const auto& [id, p] : out.points) ids.push_back(id);
    std::map<std::string, std::string> rep;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        rep[ids[i]] = ids[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (rep[ids[j]] == ids[j] && mesh::length(out.points[ids[i]] - out.points[ids[j]]) <= tol) {
                rep[ids[i]] = ids[j];
                break;
            }
        }
    }
    std::map<std::string, std::vector<std::size_t>> at;
    for (std::size_t k = 0; k < edges.size(); ++k) {
        edges[k].a = rep[edges[k].a];
        edges[k].b = rep[edges[k].b];
        at[edges[k].a].push_back(k);
        at[edges[k].b].push_back(k);
    }
    auto where = [&](const std::string& id) {
        const Vec2 p = out.points[id];
        return "(" + fmt(p.x) + ", " + fmt(p.y) + ")";
    };
    for (const auto& [node, list] : at) {
        if (list.size() > 2 && out.profileProblem.empty()) out.profileProblem = "the outline branches at " + where(node) + " (" + std::to_string(list.size()) + " edges meet)";
    }
    std::vector<bool> used(edges.size(), false);
    auto walk = [&](std::size_t first, const std::string& from, bool& closed) {
        std::vector<Vec2> pts;
        std::size_t k = first;
        std::string node = from;
        closed = false;
        for (;;) {
            used[k] = true;
            const Edge& e = edges[k];
            std::vector<Vec2> seg = e.pts;
            std::string next = e.b;
            if (e.a != node) {
                std::reverse(seg.begin(), seg.end());
                next = e.a;
            }
            if (!pts.empty()) seg.erase(seg.begin());
            pts.insert(pts.end(), seg.begin(), seg.end());
            node = next;
            if (node == from && at[node].size() == 2) {
                closed = true;
                break;
            }
            std::size_t nk = edges.size();
            for (std::size_t cand : at[node]) {
                if (!used[cand]) {
                    nk = cand;
                    break;
                }
            }
            if (nk == edges.size() || at[node].size() > 2) break;
            k = nk;
        }
        if (closed && pts.size() > 1) pts.pop_back();
        return pts;
    };
    // open chains first, from their free ends
    for (const auto& [node, list] : at) {
        if (list.size() != 1 || used[list[0]]) continue;
        bool closed = false;
        auto chain = walk(list[0], node, closed);
        if (out.profileProblem.empty()) out.profileProblem = "the outline is open: a free end at " + where(node);
        out.chains.push_back(chain);
    }
    for (std::size_t k = 0; k < edges.size(); ++k) {
        if (used[k]) continue;
        bool closed = false;
        auto loop = walk(k, edges[k].a, closed);
        if (closed) closedLoops.push_back(loop);
        else out.chains.push_back(loop);
    }
    // crossings make no sense as a solid profile
    for (std::size_t i = 0; i < closedLoops.size() && out.profileProblem.empty(); ++i) {
        for (std::size_t j = i; j < closedLoops.size() && out.profileProblem.empty(); ++j) {
            const auto& A = closedLoops[i];
            const auto& B = closedLoops[j];
            for (std::size_t s = 0; s < A.size() && out.profileProblem.empty(); ++s) {
                for (std::size_t q = (i == j ? s + 2 : 0); q < B.size(); ++q) {
                    if (i == j && s == 0 && q == B.size() - 1) continue;
                    if (segmentsCross(A[s], A[(s + 1) % A.size()], B[q], B[(q + 1) % B.size()], tol * extent)) {
                        const Vec2 p = A[s];
                        out.profileProblem = "profile lines cross near (" + fmt(p.x) + ", " + fmt(p.y) + ")";
                        break;
                    }
                }
            }
        }
    }
    out.loops = closedLoops;
    // nest: even depth = an outer boundary, odd = a hole in the smallest loop around it
    std::vector<std::size_t> order(closedLoops.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return std::fabs(area(closedLoops[a])) > std::fabs(area(closedLoops[b])); });
    std::vector<int> depth(closedLoops.size(), 0), parent(closedLoops.size(), -1);
    for (std::size_t oi = 0; oi < order.size(); ++oi) {
        const std::size_t i = order[oi];
        const auto& L = closedLoops[i];
        Vec2 probe = L[0];
        // a point just inside the loop's first edge is a safer probe than a vertex
        if (L.size() > 2) {
            const Vec2 mid = (L[0] + L[1]) * 0.5, dir = L[1] - L[0];
            Vec2 nrm{-dir.y, dir.x};
            const double l = mesh::length(nrm);
            if (l > 0) nrm = nrm / l;
            probe = mid + nrm * (area(L) > 0 ? 1e-6 : -1e-6) * extent;
        }
        for (std::size_t oj = 0; oj < oi; ++oj) {
            const std::size_t j = order[oj];
            if (insideLoop(closedLoops[j], probe)) {
                ++depth[i];
                if (parent[i] < 0 || std::fabs(area(closedLoops[j])) < std::fabs(area(closedLoops[static_cast<std::size_t>(parent[i])]))) {
                    parent[i] = static_cast<int>(j);
                }
            }
        }
    }
    std::map<std::size_t, std::size_t> regionOf;
    for (std::size_t i : order) {
        auto loop = closedLoops[i];
        if (depth[i] % 2 == 0) {
            if (area(loop) < 0) std::reverse(loop.begin(), loop.end());
            regionOf[i] = out.regions.size();
            out.regions.push_back({loop, {}});
        } else if (parent[i] >= 0 && regionOf.count(static_cast<std::size_t>(parent[i]))) {
            if (area(loop) > 0) std::reverse(loop.begin(), loop.end());
            out.regions[regionOf[static_cast<std::size_t>(parent[i])]].holes.push_back(loop);
        }
    }
}

}  // namespace

std::vector<Vec2> entityOutline(const SketchEntity& e, const SolvedSketch& s, int segments) {
    std::vector<Vec2> pts;
    for (const auto& id : e.points) pts.push_back(s.points.at(id));
    if (e.kind == "line") return pts;
    if (e.kind == "arc") return arcPoints(pts[0], pts[1], pts[2], segments);
    if (e.kind == "circle") {
        std::vector<Vec2> loop;
        const double r = s.radii.count(e.id) ? s.radii.at(e.id) : e.radius;
        for (int i = 0; i <= segments; ++i) {
            const double a = 2 * kPi * i / segments;
            loop.push_back({pts[0].x + r * std::cos(a), pts[0].y + r * std::sin(a)});
        }
        return loop;
    }
    return splinePoints(pts, e.closed, std::max(4, segments / 8));
}

RefKind refKind(const Sketch& sketch, const std::string& ref) {
    Model m(sketch);
    return m.ref(ref).kind;
}

void prepareConstraint(const Sketch& sketch, SketchConstraint& c, const Evaluator& eval) {
    Model m(sketch);
    const auto& x = m.x;
    std::vector<R> refs;
    for (const auto& t : c.refs) refs.push_back(m.ref(t));
    c.aux = 0;
    const std::string& t = c.type;
    if (t == "tangent" && refs.size() == 2) {
        const R* l = refs[0].kind == RefKind::Line ? &refs[0] : refs[1].kind == RefKind::Line ? &refs[1] : nullptr;
        if (l) {
            const R& cv = l == &refs[0] ? refs[1] : refs[0];
            if (curve(cv)) c.aux = sgn(signedDist(Model::center(x, cv), Model::P(x, l->pts[0]), Model::P(x, l->pts[1])));
        } else if (curve(refs[0]) && curve(refs[1])) {
            const double d = mesh::length(Model::center(x, refs[0]) - Model::center(x, refs[1]));
            const double r1 = Model::radius(x, refs[0]), r2 = Model::radius(x, refs[1]);
            c.aux = std::fabs(d - (r1 + r2)) <= std::fabs(d - std::fabs(r1 - r2)) ? 0 : 2 * sgn(r1 - r2);
        }
    } else if (t == "distance" && refs.size() == 2 && !(pointLike(refs[0]) && pointLike(refs[1]))) {
        const R& l = refs[0].kind == RefKind::Line ? refs[0] : refs[1];
        const R& o = refs[0].kind == RefKind::Line ? refs[1] : refs[0];
        if (l.kind == RefKind::Line) {
            const int p = o.kind == RefKind::Line ? o.pts[0] : pointIndexOf(o);
            c.aux = sgn(signedDist(Model::P(x, p), Model::P(x, l.pts[0]), Model::P(x, l.pts[1])));
        }
    } else if ((t == "hdistance" || t == "vdistance") && !refs.empty()) {
        const int axis = t == "hdistance" ? 0 : 1;
        int p = -1, q = -1;
        if (refs.size() == 1 && refs[0].kind == RefKind::Line) {
            p = refs[0].pts[0];
            q = refs[0].pts[1];
        } else if (refs.size() == 2 && pointLike(refs[0]) && pointLike(refs[1])) {
            p = pointIndexOf(refs[0]);
            q = pointIndexOf(refs[1]);
        }
        if (p >= 0) c.aux = sgn(x[static_cast<std::size_t>(2 * q + axis)] - x[static_cast<std::size_t>(2 * p + axis)]);
    } else if (t == "angle" && !refs.empty() && refs.size() <= 2) {
        const R l1 = refs.size() == 2 ? refs[0] : m.ref("xaxis");
        const R l2 = refs.size() == 2 ? refs[1] : refs[0];
        if (l1.kind == RefKind::Line && l2.kind == RefKind::Line) {
            const Vec2 u = Model::P(x, l1.pts[1]) - Model::P(x, l1.pts[0]), v = Model::P(x, l2.pts[1]) - Model::P(x, l2.pts[0]);
            const double phi = std::atan2(mesh::cross(u, v), mesh::dot(u, v));
            double theta = 0;
            try {
                theta = eval(c.value) * kPi / 180;
            } catch (const std::exception&) {
            }
            // the supplementary branch when the lines are drawn closer to it
            const bool flip = std::fabs(std::fabs(phi) - theta) > std::fabs((kPi - std::fabs(phi)) - theta);
            const double phiUsed = flip ? wrapAngle(phi + kPi) : phi;
            c.aux = sgn(phiUsed) * (flip ? 2 : 1);
        }
    } else if (t == "fixed" && refs.size() == 1 && c.value.empty()) {
        std::string v;
        for (int p : refs[0].pts) {
            const Vec2 q = Model::P(x, p);
            v += (v.empty() ? "" : ",") + fmt(q.x) + "," + fmt(q.y);
        }
        if (refs[0].radius >= 0) v += "," + fmt(x[static_cast<std::size_t>(refs[0].radius)]);
        c.value = v;
    }
    // building the equations checks the refs and the value
    std::vector<Equation> eqs;
    equationsFor(m, c, 0, eval, eqs);
}

SolvedSketch solveSketch(const Sketch& sketch, const Frame& frame, const Evaluator& eval, int segments) {
    Model m(sketch);
    std::vector<Equation> eqs;
    for (const auto& e : sketch.entities) {
        if (e.kind == "arc") {
            const int c = m.pointOf(e.points[0]), s = m.pointOf(e.points[1]), en = m.pointOf(e.points[2]);
            eqs.push_back({{2 * c, 2 * c + 1, 2 * s, 2 * s + 1, 2 * en, 2 * en + 1},
                           [c, s, en](const std::vector<double>& x) {
                               return mesh::length(Model::P(x, s) - Model::P(x, c)) - mesh::length(Model::P(x, en) - Model::P(x, c));
                           },
                           -1});
        }
    }
    for (std::size_t i = 0; i < sketch.constraints.size(); ++i) {
        try {
            equationsFor(m, sketch.constraints[i], static_cast<int>(i), eval, eqs);
        } catch (const std::invalid_argument& e) {
            throw std::runtime_error("constraint " + sketch.constraints[i].id + ": " + e.what());
        }
    }
    double extent = 1;
    for (const auto& p : sketch.points) extent = std::max({extent, std::fabs(p.x), std::fabs(p.y)});
    std::vector<double> x = m.x;
    auto rep = solve(x, m.locked, eqs, 1e-10 * extent, 200);
    if (!rep.converged) {
        std::string names;
        for (int o : rep.failing) {
            if (o >= 0) names += (names.empty() ? "" : ", ") + sketch.constraints[static_cast<std::size_t>(o)].id + " (" + sketch.constraints[static_cast<std::size_t>(o)].type + ")";
        }
        throw std::runtime_error("the sketch can't be solved: constraints conflict" + (names.empty() ? std::string() : ": " + names) +
                                 " (over defined; delete or change one)");
    }
    // the solver stops within its tolerance; snap values that are a hair off a round number so 0 reads as 0
    for (double& v : x) {
        const double q = std::round(v * 1e9) / 1e9;
        if (std::fabs(v - q) < 1e-9 * extent * 1e-2) v = q == 0 ? 0.0 : q;
    }
    SolvedSketch out;
    out.frame = frame;
    out.variables = rep.variables;
    out.equations = static_cast<int>(eqs.size());
    out.dof = rep.dof;
    out.redundant = rep.redundant;
    for (const auto& p : sketch.points) {
        const int k = m.pointOf(p.id);
        out.points[p.id] = Model::P(x, k);
        if (rep.movable[static_cast<std::size_t>(2 * k)] || rep.movable[static_cast<std::size_t>(2 * k + 1)]) out.underDefined.push_back(p.id);
    }
    for (const auto& [id, var] : m.radiusVar) {
        out.radii[id] = x[static_cast<std::size_t>(var)];
        if (!(out.radii[id] > 0)) throw std::runtime_error("circle " + id + " solved to a radius of " + fmt(out.radii[id]));
        if (rep.movable[static_cast<std::size_t>(var)]) out.underDefined.push_back(id + ".radius");
    }
    for (const auto& c : sketch.constraints) {
        if (std::find(dimensionalConstraints().begin(), dimensionalConstraints().end(), c.type) != dimensionalConstraints().end()) {
            out.measured[c.id] = measure(m, c, x);
        }
    }
    extractProfiles(sketch, out, std::max(3, segments));
    return out;
}

}  // namespace detail

}  // namespace wetwaresuite::cad
