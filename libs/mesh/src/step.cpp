// STEP (ISO 10303-21, AP203/AP214/AP242 geometry).
//
// Writing: each closed part becomes a MANIFOLD_SOLID_BREP whose faces are the
// mesh's flat regions (coplanar triangles merged into one PLANE face with its
// outer loop and holes), so CAD programs see real faces instead of triangles.
//
// Reading: B-rep faces on planes, cylinders, cones, spheres, tori and B-spline
// surfaces (rational too) are tessellated. Each face's boundary is sampled from
// its edge curves (lines, circles, ellipses, B-splines, polylines), mapped into
// the surface's (u, v) parameters, triangulated there, and long inner edges are
// split until the triangles follow the surface. Edge samples are shared between
// the faces that meet there, so closed solids come out closed. Assemblies are
// placed by their transforms; lengths are converted to millimetres.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <variant>

#include "internal.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"

namespace wetwaresuite::mesh {

namespace {

constexpr double kTau = 6.28318530717958647692;
constexpr double kHalfPi = 1.57079632679489661923;

// ---- writing ------------------------------------------------------------------

std::string real(double v) {
    if (std::abs(v) < 1e-300) return "0.";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12G", v);
    std::string s = buf;
    const auto e = s.find('E');
    std::string mant = e == std::string::npos ? s : s.substr(0, e), ex = e == std::string::npos ? "" : s.substr(e);
    if (mant.find('.') == std::string::npos) mant += ".";
    return mant + ex;
}

class Writer {
public:
    int add(const std::string& body) {
        lines_.push_back("#" + std::to_string(++next_) + "=" + body + ";");
        return next_;
    }
    static std::string ref(int id) { return "#" + std::to_string(id); }
    static std::string refs(const std::vector<int>& ids) {
        std::string s = "(";
        for (std::size_t i = 0; i < ids.size(); ++i) s += (i ? "," : "") + ref(ids[i]);
        return s + ")";
    }
    int point(const Vec3& p) { return add("CARTESIAN_POINT('',(" + real(p.x) + "," + real(p.y) + "," + real(p.z) + "))"); }
    int direction(const Vec3& d) { return add("DIRECTION('',(" + real(d.x) + "," + real(d.y) + "," + real(d.z) + "))"); }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<std::string> lines_;
    int next_ = 0;
};

// Flat regions of a welded mesh: connected triangles on one plane.
std::vector<std::vector<std::size_t>> flatRegions(const Mesh& m, const std::vector<Vec3>& fn, double tol) {
    const std::size_t nt = m.triangleCount();
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> byEdge;
    for (std::size_t t = 0; t < nt; ++t) {
        for (int k = 0; k < 3; ++k) {
            std::uint32_t a = m.indices[t * 3 + static_cast<std::size_t>(k)], b = m.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)];
            byEdge[{std::min(a, b), std::max(a, b)}].push_back(t);
        }
    }
    std::vector<int> region(nt, -1);
    std::vector<std::vector<std::size_t>> out;
    for (std::size_t s = 0; s < nt; ++s) {
        if (region[s] >= 0) continue;
        const int id = static_cast<int>(out.size());
        out.emplace_back();
        std::vector<std::size_t> stack{s};
        region[s] = id;
        const Vec3 n = fn[s];
        const double w = dot(n, m.corner(s, 0));
        while (!stack.empty()) {
            const std::size_t t = stack.back();
            stack.pop_back();
            out.back().push_back(t);
            for (int k = 0; k < 3; ++k) {
                std::uint32_t a = m.indices[t * 3 + static_cast<std::size_t>(k)], b = m.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)];
                for (std::size_t u : byEdge[{std::min(a, b), std::max(a, b)}]) {
                    if (region[u] >= 0 || dot(fn[u], n) < 1 - 1e-9) continue;
                    bool flat = true;
                    for (int q = 0; q < 3; ++q) flat = flat && std::abs(dot(n, m.corner(u, q)) - w) < tol;
                    if (!flat) continue;
                    region[u] = id;
                    stack.push_back(u);
                }
            }
        }
    }
    return out;
}

std::vector<std::vector<std::size_t>> components(const Mesh& m) {
    const std::size_t nt = m.triangleCount();
    std::vector<std::vector<std::size_t>> byVertex(m.positions.size());
    for (std::size_t t = 0; t < nt; ++t) {
        for (int k = 0; k < 3; ++k) byVertex[m.indices[t * 3 + static_cast<std::size_t>(k)]].push_back(t);
    }
    std::vector<bool> seen(nt, false);
    std::vector<std::vector<std::size_t>> out;
    for (std::size_t s = 0; s < nt; ++s) {
        if (seen[s]) continue;
        out.emplace_back();
        std::vector<std::size_t> stack{s};
        seen[s] = true;
        while (!stack.empty()) {
            const std::size_t t = stack.back();
            stack.pop_back();
            out.back().push_back(t);
            for (int k = 0; k < 3; ++k) {
                for (std::size_t u : byVertex[m.indices[t * 3 + static_cast<std::size_t>(k)]]) {
                    if (!seen[u]) {
                        seen[u] = true;
                        stack.push_back(u);
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace

std::string writeStep(const Mesh& input) {
    Mesh m;
    m.positions = input.positions;
    m.indices = input.indices;
    const double diag = bounds(m).diagonal();
    weld(m, std::max(diag * 1e-9, 1e-12));
    const double tol = std::max(diag * 1e-7, 1e-9);
    const std::size_t nt = m.triangleCount();
    std::vector<Vec3> fn(nt);
    for (std::size_t t = 0; t < nt; ++t) fn[t] = normalize(m.faceNormal(t));

    Writer w;
    const int appContext = w.add("APPLICATION_CONTEXT('core data for automotive mechanical design processes')");
    w.add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000," + Writer::ref(appContext) + ")");
    const int prodContext = w.add("PRODUCT_CONTEXT(''," + Writer::ref(appContext) + ",'mechanical')");
    const int product = w.add("PRODUCT('part','part',''," + Writer::refs({prodContext}) + ")");
    w.add("PRODUCT_RELATED_PRODUCT_CATEGORY('part',''," + Writer::refs({product}) + ")");
    const int formation = w.add("PRODUCT_DEFINITION_FORMATION('',''," + Writer::ref(product) + ")");
    const int defContext = w.add("PRODUCT_DEFINITION_CONTEXT('part definition'," + Writer::ref(appContext) + ",'design')");
    const int definition = w.add("PRODUCT_DEFINITION('design',''," + Writer::ref(formation) + "," + Writer::ref(defContext) + ")");
    const int shapeDef = w.add("PRODUCT_DEFINITION_SHAPE('',''," + Writer::ref(definition) + ")");
    const int mm = w.add("(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.))");
    const int rad = w.add("(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))");
    const int sr = w.add("(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())");
    const int unc = w.add("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(" + real(std::max(tol, 1e-7)) + ")," + Writer::ref(mm) +
                          ",'distance_accuracy_value','confusion accuracy')");
    const int geomContext = w.add("(GEOMETRIC_REPRESENTATION_CONTEXT(3)GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT(" + Writer::refs({unc}) +
                                  ")GLOBAL_UNIT_ASSIGNED_CONTEXT(" + Writer::refs({mm, rad, sr}) + ")REPRESENTATION_CONTEXT('',''))");
    const int origin = w.point({0, 0, 0});
    const int zDir = w.direction({0, 0, 1}), xDir = w.direction({1, 0, 0});
    const int worldAxis = w.add("AXIS2_PLACEMENT_3D(''," + Writer::ref(origin) + "," + Writer::ref(zDir) + "," + Writer::ref(xDir) + ")");

    // one id per used vertex and per undirected edge, shared by the faces that meet there
    std::unordered_map<std::uint32_t, int> vertexIds, pointIds;
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeIds;
    std::map<std::string, int> directions;
    auto pointOf = [&](std::uint32_t v) {
        auto it = pointIds.find(v);
        return it != pointIds.end() ? it->second : pointIds[v] = w.point(m.positions[v]);
    };
    auto dirOf = [&](const Vec3& d) {
        const std::string key = real(d.x) + "," + real(d.y) + "," + real(d.z);
        auto it = directions.find(key);
        return it != directions.end() ? it->second : directions[key] = w.add("DIRECTION('',(" + key + "))");
    };
    auto vertex = [&](std::uint32_t v) {
        auto it = vertexIds.find(v);
        if (it != vertexIds.end()) return it->second;
        return vertexIds[v] = w.add("VERTEX_POINT(''," + Writer::ref(pointOf(v)) + ")");
    };
    auto edge = [&](std::uint32_t a, std::uint32_t b) {
        const auto key = std::make_pair(std::min(a, b), std::max(a, b));
        auto it = edgeIds.find(key);
        if (it == edgeIds.end()) {
            const Vec3 pa = m.positions[key.first], pb = m.positions[key.second];
            const double len = length(pb - pa);
            const int start = pointOf(key.first);
            const int dir = dirOf(len > 0 ? (pb - pa) / len : Vec3{1, 0, 0});
            const int vec = w.add("VECTOR(''," + Writer::ref(dir) + "," + real(len > 0 ? len : 1) + ")");
            const int line = w.add("LINE(''," + Writer::ref(start) + "," + Writer::ref(vec) + ")");
            const int va = vertex(key.first), vb = vertex(key.second);
            it = edgeIds.emplace(key, w.add("EDGE_CURVE(''," + Writer::ref(va) + "," + Writer::ref(vb) + "," + Writer::ref(line) + ",.T.)")).first;
        }
        return w.add("ORIENTED_EDGE('',*,*," + Writer::ref(it->second) + "," + (a == key.first ? ".T." : ".F.") + ")");
    };

    std::vector<int> solids, shells;
    bool allClosed = true;
    const auto parts = components(m);
    std::vector<std::vector<int>> partFaces;
    std::vector<bool> partClosed;
    for (const auto& part : parts) {
        Mesh sub;
        sub.positions = m.positions;
        for (std::size_t t : part) sub.indices.insert(sub.indices.end(), {m.indices[t * 3], m.indices[t * 3 + 1], m.indices[t * 3 + 2]});
        std::vector<Vec3> subN;
        for (std::size_t t : part) subN.push_back(fn[t]);
        std::vector<int> faces;
        for (const auto& region : flatRegions(sub, subN, tol)) {
            // boundary half-edges: those whose twin isn't in the region
            std::set<std::pair<std::uint32_t, std::uint32_t>> halves;
            for (std::size_t t : region) {
                for (int k = 0; k < 3; ++k) {
                    halves.insert({sub.indices[t * 3 + static_cast<std::size_t>(k)], sub.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)]});
                }
            }
            std::multimap<std::uint32_t, std::uint32_t> next;
            for (const auto& [a, b] : halves) {
                if (!halves.count({b, a})) next.insert({a, b});
            }
            if (next.empty()) continue;
            const Vec3 n = subN[region[0]];
            std::vector<std::vector<std::uint32_t>> loops;
            while (!next.empty()) {
                auto it = next.begin();
                const std::uint32_t first = it->first;
                std::vector<std::uint32_t> loop{first};
                std::uint32_t cur = it->second;
                next.erase(it);
                while (cur != first) {
                    loop.push_back(cur);
                    auto nx = next.find(cur);
                    if (nx == next.end()) break;
                    cur = nx->second;
                    next.erase(nx);
                }
                if (loop.size() >= 3) loops.push_back(std::move(loop));
            }
            if (loops.empty()) continue;
            // the outer loop has the largest area around the normal
            auto area = [&](const std::vector<std::uint32_t>& l) {
                Vec3 s;
                for (std::size_t i = 0; i < l.size(); ++i) s = s + cross(sub.positions[l[i]], sub.positions[l[(i + 1) % l.size()]]);
                return dot(s, n) / 2;
            };
            std::size_t outer = 0;
            for (std::size_t i = 1; i < loops.size(); ++i) {
                if (std::abs(area(loops[i])) > std::abs(area(loops[outer]))) outer = i;
            }
            std::vector<int> bounds;
            for (std::size_t i = 0; i < loops.size(); ++i) {
                std::vector<int> oriented;
                const auto& l = loops[i];
                for (std::size_t j = 0; j < l.size(); ++j) oriented.push_back(edge(l[j], l[(j + 1) % l.size()]));
                const int loop = w.add("EDGE_LOOP(''," + Writer::refs(oriented) + ")");
                bounds.push_back(w.add(std::string(i == outer ? "FACE_OUTER_BOUND" : "FACE_BOUND") + "(''," + Writer::ref(loop) + ",.T.)"));
            }
            const Vec3 p0 = sub.positions[loops[outer][0]];
            Vec3 ref = sub.positions[loops[outer][1]] - p0;
            ref = normalize(ref - n * dot(ref, n));
            const int ax = w.add("AXIS2_PLACEMENT_3D(''," + Writer::ref(pointOf(loops[outer][0])) + "," + Writer::ref(dirOf(n)) + "," + Writer::ref(dirOf(ref)) + ")");
            const int plane = w.add("PLANE(''," + Writer::ref(ax) + ")");
            faces.push_back(w.add("ADVANCED_FACE(''," + Writer::refs(bounds) + "," + Writer::ref(plane) + ",.T.)"));
        }
        const bool closed = isWatertight(sub);
        allClosed = allClosed && closed;
        partFaces.push_back(std::move(faces));
        partClosed.push_back(closed);
    }
    int representation = 0;
    if (allClosed) {
        for (std::size_t i = 0; i < partFaces.size(); ++i) {
            const int shell = w.add("CLOSED_SHELL(''," + Writer::refs(partFaces[i]) + ")");
            solids.push_back(w.add("MANIFOLD_SOLID_BREP('part" + std::to_string(i + 1) + "'," + Writer::ref(shell) + ")"));
        }
        std::vector<int> items{worldAxis};
        items.insert(items.end(), solids.begin(), solids.end());
        representation = w.add("ADVANCED_BREP_SHAPE_REPRESENTATION('part'," + Writer::refs(items) + "," + Writer::ref(geomContext) + ")");
    } else {
        for (std::size_t i = 0; i < partFaces.size(); ++i) {
            shells.push_back(w.add(std::string(partClosed[i] ? "CLOSED_SHELL" : "OPEN_SHELL") + "(''," + Writer::refs(partFaces[i]) + ")"));
        }
        const int model = w.add("SHELL_BASED_SURFACE_MODEL(''," + Writer::refs(shells) + ")");
        representation = w.add("MANIFOLD_SURFACE_SHAPE_REPRESENTATION('part'," + Writer::refs({worldAxis, model}) + "," + Writer::ref(geomContext) + ")");
    }
    w.add("SHAPE_DEFINITION_REPRESENTATION(" + Writer::ref(shapeDef) + "," + Writer::ref(representation) + ")");

    std::string out =
        "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('wetwaresuite part'),'2;1');\n"
        "FILE_NAME('part.step','',(''),(''),'wetwaresuite','wetwaresuite','');\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n";
    for (const auto& line : w.lines()) out += line + "\n";
    out += "ENDSEC;\nEND-ISO-10303-21;\n";
    return out;
}

// ---- reading ------------------------------------------------------------------

namespace {

struct Arg;
using ArgList = std::vector<Arg>;
struct Arg {
    enum Kind { None, Number, Text, Ref, Enum, List, Typed } kind = None;
    double num = 0;
    int ref = 0;
    std::string text;  // string, enum name or the type of a typed value
    std::shared_ptr<ArgList> list;
};

struct Record {
    std::string type;  // empty for a complex entity
    ArgList args;
    std::vector<std::pair<std::string, ArgList>> parts;  // complex entity: (TYPE(args) TYPE(args) ...)
    const ArgList* part(const std::string& t) const {
        if (type == t) return &args;
        for (const auto& [name, a] : parts) {
            if (name == t) return &a;
        }
        return nullptr;
    }
    bool is(const std::string& t) const { return part(t) != nullptr; }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    std::unordered_map<int, Record> parse() {
        std::unordered_map<int, Record> out;
        const auto data = s_.find("DATA;");
        if (s_.find("ISO-10303-21") == std::string::npos || data == std::string::npos) {
            throw std::runtime_error("not a STEP file (no ISO-10303-21 header or DATA section)");
        }
        p_ = data + 5;
        while (true) {
            skip();
            if (p_ >= s_.size()) break;
            if (s_.compare(p_, 7, "ENDSEC;") == 0) break;
            if (s_[p_] != '#') {  // something we don't understand: skip the statement
                skipStatement();
                continue;
            }
            ++p_;
            const int id = static_cast<int>(std::strtol(s_.c_str() + p_, nullptr, 10));
            while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
            skip();
            if (p_ >= s_.size() || s_[p_] != '=') {
                skipStatement();
                continue;
            }
            ++p_;
            skip();
            Record r;
            if (p_ < s_.size() && s_[p_] == '(') {  // complex entity
                ++p_;
                while (true) {
                    skip();
                    if (p_ >= s_.size() || s_[p_] == ')') break;
                    std::string name = keyword();
                    if (name.empty()) break;
                    skip();
                    r.parts.push_back({name, list()});
                }
                ++p_;
            } else {
                r.type = keyword();
                skip();
                r.args = list();
            }
            skipStatement();
            out[id] = std::move(r);
        }
        return out;
    }

private:
    void skip() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++p_;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
                const auto e = s_.find("*/", p_ + 2);
                p_ = e == std::string::npos ? s_.size() : e + 2;
            } else {
                break;
            }
        }
    }
    void skipStatement() {
        bool inText = false;
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '\'') inText = !inText;
            else if (c == ';' && !inText) return;
        }
    }
    std::string keyword() {
        const std::size_t b = p_;
        while (p_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[p_])) || s_[p_] == '_' || s_[p_] == '-')) ++p_;
        return s_.substr(b, p_ - b);
    }
    ArgList list() {
        ArgList out;
        skip();
        if (p_ >= s_.size() || s_[p_] != '(') return out;
        ++p_;
        while (true) {
            skip();
            if (p_ >= s_.size()) break;
            if (s_[p_] == ')') {
                ++p_;
                break;
            }
            if (s_[p_] == ',') {
                ++p_;
                continue;
            }
            out.push_back(value());
        }
        return out;
    }
    Arg value() {
        Arg a;
        const char c = s_[p_];
        if (c == '#') {
            ++p_;
            a.kind = Arg::Ref;
            a.ref = static_cast<int>(std::strtol(s_.c_str() + p_, nullptr, 10));
            while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_]))) ++p_;
        } else if (c == '\'') {
            a.kind = Arg::Text;
            ++p_;
            while (p_ < s_.size()) {
                if (s_[p_] == '\'') {
                    if (p_ + 1 < s_.size() && s_[p_ + 1] == '\'') {
                        a.text += '\'';
                        p_ += 2;
                        continue;
                    }
                    ++p_;
                    break;
                }
                a.text += s_[p_++];
            }
        } else if (c == '.') {
            const auto e = s_.find('.', p_ + 1);
            a.kind = Arg::Enum;
            a.text = s_.substr(p_ + 1, e == std::string::npos ? 0 : e - p_ - 1);
            p_ = e == std::string::npos ? s_.size() : e + 1;
        } else if (c == '(') {
            a.kind = Arg::List;
            a.list = std::make_shared<ArgList>(list());
        } else if (c == '$' || c == '*') {
            ++p_;
        } else if (std::isalpha(static_cast<unsigned char>(c))) {
            a.kind = Arg::Typed;
            a.text = keyword();
            skip();
            a.list = std::make_shared<ArgList>(list());
        } else {
            char* end = nullptr;
            a.num = std::strtod(s_.c_str() + p_, &end);
            if (end == s_.c_str() + p_) {
                ++p_;  // stray character
            } else {
                a.kind = Arg::Number;
                p_ = static_cast<std::size_t>(end - s_.c_str());
            }
        }
        return a;
    }

    const std::string& s_;
    std::size_t p_ = 0;
};

double argNum(const ArgList& a, std::size_t i, double fallback = 0) {
    if (i >= a.size()) return fallback;
    if (a[i].kind == Arg::Number) return a[i].num;
    if (a[i].kind == Arg::Typed && a[i].list && !a[i].list->empty() && (*a[i].list)[0].kind == Arg::Number) return (*a[i].list)[0].num;
    return fallback;
}
int argRef(const ArgList& a, std::size_t i) { return i < a.size() && a[i].kind == Arg::Ref ? a[i].ref : 0; }
bool argBool(const ArgList& a, std::size_t i, bool fallback = true) {
    if (i >= a.size() || a[i].kind != Arg::Enum) return fallback;
    return a[i].text == "T";
}
const ArgList* argList(const ArgList& a, std::size_t i) {
    static const ArgList empty;
    return i < a.size() && a[i].kind == Arg::List ? a[i].list.get() : &empty;
}
std::vector<int> refList(const ArgList& a, std::size_t i) {
    std::vector<int> out;
    for (const auto& x : *argList(a, i)) {
        if (x.kind == Arg::Ref) out.push_back(x.ref);
    }
    return out;
}
std::vector<double> numList(const ArgList& a, std::size_t i) {
    std::vector<double> out;
    for (const auto& x : *argList(a, i)) {
        if (x.kind == Arg::Number) out.push_back(x.num);
    }
    return out;
}

struct Frame {
    Vec3 o, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    Vec3 at(double u, double v, double w = 0) const { return o + x * u + y * v + z * w; }
    Vec3 local(const Vec3& p) const { return {dot(p - o, x), dot(p - o, y), dot(p - o, z)}; }
};

// A NURBS curve or surface direction: degree, knots (expanded), weights.
struct Spline {
    int degree = 1;
    std::vector<double> knots;
    double lo() const { return knots[static_cast<std::size_t>(degree)]; }
    double hi() const { return knots[knots.size() - 1 - static_cast<std::size_t>(degree)]; }
    std::size_t span(double t, std::size_t n) const {  // n = control point count
        std::size_t k = static_cast<std::size_t>(degree);
        while (k + 1 < n && t >= knots[k + 1]) ++k;
        return k;
    }
    // non-zero basis functions at t over [span-degree, span]
    void basis(double t, std::size_t sp, std::vector<double>& out) const {
        const std::size_t p = static_cast<std::size_t>(degree);
        out.assign(p + 1, 0.0);
        std::vector<double> left(p + 1), right(p + 1);
        out[0] = 1;
        for (std::size_t j = 1; j <= p; ++j) {
            left[j] = t - knots[sp + 1 - j];
            right[j] = knots[sp + j] - t;
            double saved = 0;
            for (std::size_t r = 0; r < j; ++r) {
                const double den = right[r + 1] + left[j - r];
                const double tmp = den != 0 ? out[r] / den : 0;
                out[r] = saved + right[r + 1] * tmp;
                saved = left[j - r] * tmp;
            }
            out[j] = saved;
        }
    }
};

Spline makeSpline(int degree, const std::vector<double>& mults, const std::vector<double>& knots) {
    Spline s;
    s.degree = std::max(1, degree);
    for (std::size_t i = 0; i < knots.size() && i < mults.size(); ++i) {
        for (int k = 0; k < static_cast<int>(mults[i]); ++k) s.knots.push_back(knots[i]);
    }
    return s;
}

class Reader {
public:
    explicit Reader(const std::string& text) : e_(Parser(text).parse()) {}

    Mesh read(std::string* notes) {
        scale_ = lengthScale();
        {
            Bounds all;
            for (const auto& [id, r] : e_) {
                if (r.is("CARTESIAN_POINT")) all.add(point(id));
            }
            tol_ = all.empty() ? 1e-3 : std::max(all.diagonal() * 5e-4, 1e-9);
        }
        // representations and the solids/shells in each
        std::map<int, std::vector<int>> repItems;
        std::set<int> children;
        std::multimap<int, std::pair<int, Mat4>> links;  // parent -> (child, child-to-parent)
        for (const auto& [id, r] : e_) {
            const ArgList* rep = nullptr;
            for (const char* t : {"ADVANCED_BREP_SHAPE_REPRESENTATION", "MANIFOLD_SURFACE_SHAPE_REPRESENTATION", "FACETED_BREP_SHAPE_REPRESENTATION",
                                  "SHAPE_REPRESENTATION", "GEOMETRICALLY_BOUNDED_SURFACE_SHAPE_REPRESENTATION"}) {
                if (!rep) rep = r.part(t);
            }
            if (rep) repItems[id] = refList(*rep, 1);
        }
        for (const auto& [id, r] : e_) {
            const ArgList* rel = r.part("REPRESENTATION_RELATIONSHIP");
            if (!rel) continue;
            const int r1 = argRef(*rel, 2), r2 = argRef(*rel, 3);
            Mat4 t;
            if (const ArgList* tr = r.part("REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION")) {
                const auto it = e_.find(argRef(*tr, 0));
                if (it != e_.end() && it->second.is("ITEM_DEFINED_TRANSFORMATION")) {
                    const ArgList& a = *it->second.part("ITEM_DEFINED_TRANSFORMATION");
                    t = matrix(frame(argRef(a, 3))) * matrix(frame(argRef(a, 2))).inverse();
                }
            }
            // rep_1 is placed into rep_2
            links.insert({r2, {r1, t}});
            children.insert(r1);
        }
        // mapped items: a representation placed into another
        Mesh out;
        std::size_t faces = 0;
        std::function<void(int, const Mat4&, int)> emit = [&](int rep, const Mat4& m, int depth) {
            if (depth > 32) return;
            for (int item : repItems[rep]) solid(item, m, out, faces, depth);
            auto range = links.equal_range(rep);
            for (auto it = range.first; it != range.second; ++it) emit(it->second.first, m * it->second.second, depth + 1);
        };
        for (const auto& [rep, items] : repItems) {
            if (!children.count(rep)) emit(rep, Mat4{}, 0);
        }
        if (out.empty()) {
            // no shape representation tying things together: take every solid in the file
            for (const auto& [id, r] : e_) {
                if (r.is("MANIFOLD_SOLID_BREP") || r.is("BREP_WITH_VOIDS") || r.is("FACETED_BREP") || r.is("SHELL_BASED_SURFACE_MODEL")) solid(id, Mat4{}, out, faces, 0);
            }
        }
        if (out.empty()) throw std::runtime_error("this STEP file has no solids or surfaces to read (only curves or product data?)");
        weld(out, std::max(bounds(out).diagonal() * 1e-7, 1e-9));
        if (notes) {
            *notes = std::to_string(faces) + " faces";
            for (const auto& [what, n] : skipped_) *notes += ", " + std::to_string(n) + " " + what;
        }
        return out;
    }

private:
    const Record* get(int id) const {
        auto it = e_.find(id);
        return it == e_.end() ? nullptr : &it->second;
    }

    double lengthScale() const {
        for (const auto& [id, r] : e_) {
            if (!r.is("LENGTH_UNIT")) continue;
            if (const ArgList* si = r.part("SI_UNIT")) {
                const std::string prefix = si->size() > 0 && (*si)[0].kind == Arg::Enum ? (*si)[0].text : "";
                if (prefix == "MILLI") return 1;
                if (prefix == "CENTI") return 10;
                if (prefix == "DECI") return 100;
                if (prefix == "KILO") return 1e6;
                if (prefix == "MICRO") return 1e-3;
                return 1000;
            }
            if (const ArgList* cb = r.part("CONVERSION_BASED_UNIT")) {
                std::string name = cb->size() > 0 ? (*cb)[0].text : "";
                for (auto& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (name.find("INCH") != std::string::npos) return 25.4;
                if (name.find("FOOT") != std::string::npos || name.find("FEET") != std::string::npos) return 304.8;
                if (name.find("CENTI") != std::string::npos) return 10;
                if (name == "METRE" || name == "METER") return 1000;
                return 1;
            }
        }
        return 1;
    }

    Vec3 point(int id) const {
        const Record* r = get(id);
        if (!r) return {};
        if (const ArgList* a = r->part("CARTESIAN_POINT")) {
            const auto c = numList(*a, 1);
            return Vec3{c.size() > 0 ? c[0] : 0, c.size() > 1 ? c[1] : 0, c.size() > 2 ? c[2] : 0} * scale_;
        }
        if (const ArgList* a = r->part("VERTEX_POINT")) return point(argRef(*a, 1));
        return {};
    }
    Vec3 direction(int id, Vec3 fallback) const {
        const Record* r = get(id);
        if (!r) return fallback;
        if (const ArgList* a = r->part("DIRECTION")) {
            const auto c = numList(*a, 1);
            const Vec3 d{c.size() > 0 ? c[0] : 0, c.size() > 1 ? c[1] : 0, c.size() > 2 ? c[2] : 0};
            return length(d) > 0 ? normalize(d) : fallback;
        }
        if (const ArgList* a = r->part("VECTOR")) return direction(argRef(*a, 1), fallback);
        return fallback;
    }
    Frame frame(int id) const {
        Frame f;
        const Record* r = get(id);
        if (!r) return f;
        const ArgList* a = r->part("AXIS2_PLACEMENT_3D");
        if (!a) return f;
        f.o = point(argRef(*a, 1));
        f.z = direction(argRef(*a, 2), {0, 0, 1});
        Vec3 x = direction(argRef(*a, 3), std::abs(f.z.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
        x = x - f.z * dot(x, f.z);
        if (length(x) < 1e-12) x = std::abs(f.z.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}, x = x - f.z * dot(x, f.z);
        f.x = normalize(x);
        f.y = cross(f.z, f.x);
        return f;
    }
    static Mat4 matrix(const Frame& f) {
        Mat4 m;
        for (int r = 0; r < 3; ++r) {
            m.at(r, 0) = f.x[r];
            m.at(r, 1) = f.y[r];
            m.at(r, 2) = f.z[r];
            m.at(r, 3) = f.o[r];
        }
        return m;
    }

    // ---- curves ----

    // Control points (scaled), weights, spline of a B-spline curve record; false if it isn't one.
    bool bsplineCurve(const Record& r, std::vector<Vec3>& pts, std::vector<double>& w, Spline& sp) const {
        const ArgList* b = r.part("B_SPLINE_CURVE");
        const ArgList* k = r.part("B_SPLINE_CURVE_WITH_KNOTS");
        if (!b && k && r.type == "B_SPLINE_CURVE_WITH_KNOTS") b = k;  // simple entity: B_SPLINE_CURVE args come first
        if (!b) return false;
        const std::size_t o = r.type.empty() ? 0 : 1;  // a complex entity keeps the name in REPRESENTATION_ITEM
        const int degree = static_cast<int>(argNum(*b, o, 1));
        for (int id : refList(*b, o + 1)) pts.push_back(point(id));
        std::vector<double> mults, knots;
        if (r.type == "B_SPLINE_CURVE_WITH_KNOTS") {
            mults = numList(*k, 6);
            knots = numList(*k, 7);
        } else if (k) {
            mults = numList(*k, 0);
            knots = numList(*k, 1);
        } else {
            // Bezier or uniform: clamped uniform knots
            const int n = static_cast<int>(pts.size());
            for (int i = 0; i <= n - degree; ++i) knots.push_back(i), mults.push_back(i == 0 || i == n - degree ? degree + 1 : 1);
        }
        sp = makeSpline(degree, mults, knots);
        if (const ArgList* rat = r.part("RATIONAL_B_SPLINE_CURVE")) w = numList(*rat, 0);
        if (w.size() != pts.size()) w.assign(pts.size(), 1.0);
        return pts.size() >= 2 && sp.knots.size() == pts.size() + static_cast<std::size_t>(degree) + 1;
    }

    static Vec3 evalCurve(const std::vector<Vec3>& pts, const std::vector<double>& w, const Spline& sp, double t) {
        t = std::clamp(t, sp.lo(), sp.hi());
        const std::size_t s = sp.span(t, pts.size());
        std::vector<double> nb;
        sp.basis(t, s, nb);
        Vec3 sum;
        double ws = 0;
        for (std::size_t j = 0; j < nb.size(); ++j) {
            const std::size_t i = s - static_cast<std::size_t>(sp.degree) + j;
            sum = sum + pts[i] * (nb[j] * w[i]);
            ws += nb[j] * w[i];
        }
        return ws != 0 ? sum / ws : sum;
    }

    // The basis 3D curve under SURFACE_CURVE, SEAM_CURVE, TRIMMED_CURVE...
    int basisCurve(int id) const {
        for (int depth = 0; depth < 8; ++depth) {
            const Record* r = get(id);
            if (!r) return id;
            const ArgList* a = nullptr;
            if ((a = r->part("SURFACE_CURVE")) || (a = r->part("SEAM_CURVE")) || (a = r->part("BOUNDED_SURFACE_CURVE")) || (a = r->part("INTERSECTION_CURVE"))) {
                id = argRef(*a, 1);
            } else if ((a = r->part("TRIMMED_CURVE"))) {
                id = argRef(*a, 1);
            } else {
                return id;
            }
        }
        return id;
    }

    // Points along the curve from p0 to p1 (endpoints exact), in the curve's own direction when sense is true.
    std::vector<Vec3> sampleCurve(int curveId, const Vec3& p0, const Vec3& p1, bool sense) {
        const int id = basisCurve(curveId);
        const Record* r = get(id);
        std::vector<Vec3> out{p0};
        auto finish = [&]() {
            out.push_back(p1);
            return out;
        };
        if (!r) return finish();
        const bool closed = length(p1 - p0) < 1e-9 * (1 + length(p0));
        if (const ArgList* a = r->part("CIRCLE")) {
            const Frame f = frame(argRef(*a, 1));
            const double rad = argNum(*a, 2) * scale_;
            sampleConic(f, rad, rad, p0, p1, sense, closed, out);
            return finish();
        }
        if (const ArgList* a = r->part("ELLIPSE")) {
            const Frame f = frame(argRef(*a, 1));
            sampleConic(f, argNum(*a, 2) * scale_, argNum(*a, 3) * scale_, p0, p1, sense, closed, out);
            return finish();
        }
        if (const ArgList* a = r->part("POLYLINE")) {
            std::vector<Vec3> pts;
            for (int pid : refList(*a, 1)) pts.push_back(point(pid));
            if (!sense) std::reverse(pts.begin(), pts.end());
            for (std::size_t i = 1; i + 1 < pts.size(); ++i) out.push_back(pts[i]);
            return finish();
        }
        std::vector<Vec3> pts;
        std::vector<double> w;
        Spline sp;
        if (bsplineCurve(*r, pts, w, sp)) {
            const double lo = sp.lo(), hi = sp.hi();
            const int n = std::clamp(static_cast<int>(pts.size()) * 8, 16, 4000);
            std::vector<Vec3> dense;
            for (int i = 0; i <= n; ++i) dense.push_back(evalCurve(pts, w, sp, lo + (hi - lo) * i / n));
            auto nearest = [&](const Vec3& p) {
                std::size_t best = 0;
                for (std::size_t i = 1; i < dense.size(); ++i) {
                    if (length(dense[i] - p) < length(dense[best] - p)) best = i;
                }
                return best;
            };
            std::size_t i0 = nearest(p0), i1 = nearest(p1);
            if (closed) {  // the whole curve
                i0 = 0;
                i1 = dense.size() - 1;
                if (!sense) std::swap(i0, i1);
            } else if (sense ? i1 < i0 : i1 > i0) {
                std::swap(i0, i1);  // periodic curves can wrap; take the run between them
            }
            if (i0 < i1) {
                for (std::size_t i = i0 + 1; i < i1; ++i) out.push_back(dense[i]);
            } else {
                for (std::size_t i = i0; i-- > i1 + 1;) out.push_back(dense[i]);
            }
            return finish();
        }
        if (!r->is("LINE")) ++skipped_["unknown curves (drawn straight)"];
        return finish();
    }

    void sampleConic(const Frame& f, double a, double b, const Vec3& p0, const Vec3& p1, bool sense, bool closed, std::vector<Vec3>& out) const {
        auto param = [&](const Vec3& p) {
            const Vec3 l = f.local(p);
            return std::atan2(l.y / b, l.x / a);
        };
        const double t0 = param(p0);
        double t1 = param(p1);
        double sweep = sense ? t1 - t0 : t0 - t1;
        while (sweep <= 1e-12) sweep += kTau;
        if (closed) sweep = kTau;
        while (sweep > kTau + 1e-12) sweep -= kTau;
        const int n = std::max(2, static_cast<int>(std::ceil(64 * sweep / kTau)));
        for (int i = 1; i < n; ++i) {
            const double t = t0 + (sense ? 1 : -1) * sweep * i / n;
            out.push_back(f.at(a * std::cos(t), b * std::sin(t)));
        }
    }

    // An edge's points from its start vertex to its end vertex, cached per edge.
    const std::vector<Vec3>& edgePoints(int edgeId) {
        auto it = edgeCache_.find(edgeId);
        if (it != edgeCache_.end()) return it->second;
        std::vector<Vec3> pts;
        const Record* r = get(edgeId);
        if (r) {
            if (const ArgList* a = r->part("EDGE_CURVE")) {
                const Vec3 p0 = point(argRef(*a, 1)), p1 = point(argRef(*a, 2));
                pts = sampleCurve(argRef(*a, 3), p0, p1, argBool(*a, 4));
            } else if (const ArgList* a2 = r->part("ORIENTED_EDGE")) {
                pts = edgePoints(argRef(*a2, 3));
                if (!argBool(*a2, 4)) std::reverse(pts.begin(), pts.end());
            }
        }
        return edgeCache_[edgeId] = std::move(pts);
    }

    // A face bound as one closed run of points (the first point not repeated at the end).
    std::vector<Vec3> loopPoints(int boundId, bool& degenerate) {
        std::vector<Vec3> out;
        const Record* b = get(boundId);
        if (!b) return out;
        const ArgList* fb = b->part("FACE_OUTER_BOUND");
        if (!fb) fb = b->part("FACE_BOUND");
        if (!fb) return out;
        const Record* loop = get(argRef(*fb, 1));
        const bool orient = argBool(*fb, 2);
        if (!loop) return out;
        if (const ArgList* el = loop->part("EDGE_LOOP")) {
            for (int oe : refList(*el, 1)) {
                const Record* o = get(oe);
                if (!o || !o->is("ORIENTED_EDGE")) continue;
                const ArgList& a = *o->part("ORIENTED_EDGE");
                std::vector<Vec3> pts = edgePoints(argRef(a, 3));
                if (!argBool(a, 4)) std::reverse(pts.begin(), pts.end());
                for (std::size_t i = 0; i + 1 < pts.size(); ++i) out.push_back(pts[i]);  // the next edge starts with this one's end
            }
        } else if (const ArgList* pl = loop->part("POLY_LOOP")) {
            for (int pid : refList(*pl, 1)) out.push_back(point(pid));
        } else if (loop->is("VERTEX_LOOP")) {
            degenerate = true;
        }
        if (!orient) std::reverse(out.begin(), out.end());
        // drop repeats
        std::vector<Vec3> clean;
        for (const auto& p : out) {
            if (clean.empty() || length(clean.back() - p) > 1e-12) clean.push_back(p);
        }
        while (clean.size() > 1 && length(clean.front() - clean.back()) <= 1e-12) clean.pop_back();
        return clean;
    }

    // ---- surfaces ----

    struct Surface {
        enum Kind { Plane, Cylinder, Cone, Sphere, Torus, BSpline, Other } kind = Other;
        Frame f;
        double r = 0, r2 = 0, tanA = 0;
        bool periodicU = false, periodicV = false;
        bool closedU = false, closedV = false;  // B-spline: the two ends of the range meet
        // B-spline
        std::vector<std::vector<Vec3>> cp;
        std::vector<std::vector<double>> w;
        Spline su, sv;

        Vec3 at(double u, double v) const {
            switch (kind) {
                case Plane: return f.at(u, v);
                case Cylinder: return f.at(r * std::cos(u), r * std::sin(u), v);
                case Cone: {
                    const double rr = r + v * tanA;
                    return f.at(rr * std::cos(u), rr * std::sin(u), v);
                }
                case Sphere: return f.at(r * std::cos(v) * std::cos(u), r * std::cos(v) * std::sin(u), r * std::sin(v));
                case Torus: {
                    const double rr = r + r2 * std::cos(v);
                    return f.at(rr * std::cos(u), rr * std::sin(u), r2 * std::sin(v));
                }
                case BSpline: {
                    const std::size_t nu = cp.size(), nv = cp[0].size();
                    u = std::clamp(u, su.lo(), su.hi());
                    v = std::clamp(v, sv.lo(), sv.hi());
                    const std::size_t a = su.span(u, nu), b = sv.span(v, nv);
                    std::vector<double> bu, bv;
                    su.basis(u, a, bu);
                    sv.basis(v, b, bv);
                    Vec3 sum;
                    double ws = 0;
                    for (std::size_t i = 0; i < bu.size(); ++i) {
                        for (std::size_t j = 0; j < bv.size(); ++j) {
                            const std::size_t ii = a - static_cast<std::size_t>(su.degree) + i, jj = b - static_cast<std::size_t>(sv.degree) + j;
                            const double k = bu[i] * bv[j] * w[ii][jj];
                            sum = sum + cp[ii][jj] * k;
                            ws += k;
                        }
                    }
                    return ws != 0 ? sum / ws : sum;
                }
                default: return f.at(u, v);
            }
        }
    };

    bool surface(int id, Surface& s) {
        const Record* r = get(id);
        if (!r) return false;
        const ArgList* a = nullptr;
        if ((a = r->part("PLANE"))) {
            s.kind = Surface::Plane;
            s.f = frame(argRef(*a, 1));
        } else if ((a = r->part("CYLINDRICAL_SURFACE"))) {
            s.kind = Surface::Cylinder;
            s.f = frame(argRef(*a, 1));
            s.r = argNum(*a, 2) * scale_;
            s.periodicU = true;
        } else if ((a = r->part("CONICAL_SURFACE"))) {
            s.kind = Surface::Cone;
            s.f = frame(argRef(*a, 1));
            s.r = argNum(*a, 2) * scale_;
            s.tanA = std::tan(argNum(*a, 3));
            if (std::abs(argNum(*a, 3)) > kTau / 4 + 1e-9) s.tanA = std::tan(argNum(*a, 3) * kTau / 360);  // degrees
            s.periodicU = true;
        } else if ((a = r->part("SPHERICAL_SURFACE"))) {
            s.kind = Surface::Sphere;
            s.f = frame(argRef(*a, 1));
            s.r = argNum(*a, 2) * scale_;
            s.periodicU = true;
        } else if ((a = r->part("TOROIDAL_SURFACE")) || (a = r->part("DEGENERATE_TOROIDAL_SURFACE"))) {
            s.kind = Surface::Torus;
            s.f = frame(argRef(*a, 1));
            s.r = argNum(*a, 2) * scale_;
            s.r2 = argNum(*a, 3) * scale_;
            s.periodicU = s.periodicV = true;
        } else if (r->is("B_SPLINE_SURFACE") || r->is("B_SPLINE_SURFACE_WITH_KNOTS")) {
            const ArgList* b = r->part("B_SPLINE_SURFACE");
            const ArgList* k = r->part("B_SPLINE_SURFACE_WITH_KNOTS");
            const bool simple = r->type == "B_SPLINE_SURFACE_WITH_KNOTS";
            if (simple) b = k;
            if (!b) return false;
            const std::size_t o = simple ? 1 : 0;  // a complex entity keeps the name in REPRESENTATION_ITEM
            const int du = static_cast<int>(argNum(*b, o, 1)), dv = static_cast<int>(argNum(*b, o + 1, 1));
            for (const auto& row : *argList(*b, o + 2)) {
                if (row.kind != Arg::List) continue;
                std::vector<Vec3> line;
                for (const auto& x : *row.list) {
                    if (x.kind == Arg::Ref) line.push_back(point(x.ref));
                }
                s.cp.push_back(std::move(line));
            }
            std::vector<double> mu, mv, ku, kv;
            if (simple) {
                mu = numList(*k, 8), mv = numList(*k, 9), ku = numList(*k, 10), kv = numList(*k, 11);
            } else if (k) {
                mu = numList(*k, 0), mv = numList(*k, 1), ku = numList(*k, 2), kv = numList(*k, 3);
            } else {
                const int nu = static_cast<int>(s.cp.size()), nv = s.cp.empty() ? 0 : static_cast<int>(s.cp[0].size());
                for (int i = 0; i <= nu - du; ++i) ku.push_back(i), mu.push_back(i == 0 || i == nu - du ? du + 1 : 1);
                for (int i = 0; i <= nv - dv; ++i) kv.push_back(i), mv.push_back(i == 0 || i == nv - dv ? dv + 1 : 1);
            }
            s.su = makeSpline(du, mu, ku);
            s.sv = makeSpline(dv, mv, kv);
            if (s.cp.empty() || s.cp[0].empty()) return false;
            for (const auto& row : s.cp) {
                if (row.size() != s.cp[0].size()) return false;
            }
            if (s.su.knots.size() != s.cp.size() + static_cast<std::size_t>(s.su.degree) + 1 ||
                s.sv.knots.size() != s.cp[0].size() + static_cast<std::size_t>(s.sv.degree) + 1) {
                return false;
            }
            s.w.assign(s.cp.size(), std::vector<double>(s.cp[0].size(), 1.0));
            if (const ArgList* rat = r->part("RATIONAL_B_SPLINE_SURFACE")) {
                std::size_t i = 0;
                for (const auto& row : *argList(*rat, 0)) {
                    if (row.kind != Arg::List || i >= s.w.size()) continue;
                    std::size_t j = 0;
                    for (const auto& x : *row.list) {
                        if (x.kind == Arg::Number && j < s.w[i].size()) s.w[i][j++] = x.num;
                    }
                    ++i;
                }
            }
            {
                const double eps = 1e-9 * (1 + length(s.cp[0][0]));
                s.closedU = s.closedV = true;
                for (std::size_t j = 0; j < s.cp[0].size(); ++j) s.closedU = s.closedU && length(s.cp.front()[j] - s.cp.back()[j]) < eps;
                for (std::size_t i = 0; i < s.cp.size(); ++i) s.closedV = s.closedV && length(s.cp[i].front() - s.cp[i].back()) < eps;
            }
            s.kind = Surface::BSpline;
        } else {
            return false;
        }
        return true;
    }

    // (u, v) of a point on the surface; `hint` keeps periodic parameters continuous.
    Vec2 param(const Surface& s, const Vec3& p, const Vec2* hint) const {
        const Vec3 l = s.f.local(p);
        Vec2 uv;
        switch (s.kind) {
            case Surface::Plane: return {l.x, l.y};
            case Surface::Cylinder:
            case Surface::Cone: uv = {std::atan2(l.y, l.x), l.z}; break;
            case Surface::Sphere: uv = {std::atan2(l.y, l.x), std::asin(std::clamp(l.z / std::max(s.r, 1e-300), -1.0, 1.0))}; break;
            case Surface::Torus: {
                const double rho = std::hypot(l.x, l.y);
                uv = {std::atan2(l.y, l.x), std::atan2(l.z, rho - s.r)};
                break;
            }
            case Surface::BSpline: return invert(s, p, hint);
            default: return {l.x, l.y};
        }
        // on the axis (apex, pole) the angle means nothing: keep the neighbour's
        const double rho = std::hypot(l.x, l.y);
        if (rho < 1e-9 * (1 + s.r) && hint) uv.x = hint->x;
        if (hint) {
            if (s.periodicU) uv.x += kTau * std::round((hint->x - uv.x) / kTau);
            if (s.periodicV) uv.y += kTau * std::round((hint->y - uv.y) / kTau);
        }
        return uv;
    }

    // Closest point on a B-spline surface: a grid search, then Newton steps.
    Vec2 invert(const Surface& s, const Vec3& p, const Vec2* hint) const {
        const double u0 = s.su.lo(), u1 = s.su.hi(), v0 = s.sv.lo(), v1 = s.sv.hi();
        const double hu = (u1 - u0) * 1e-6, hv = (v1 - v0) * 1e-6;
        auto newton = [&](double u, double v) {
            for (int it = 0; it < 30; ++it) {
                const Vec3 q = s.at(u, v);
                const Vec3 su = (s.at(std::min(u + hu, u1), v) - s.at(std::max(u - hu, u0), v)) / (std::min(u + hu, u1) - std::max(u - hu, u0));
                const Vec3 sv = (s.at(u, std::min(v + hv, v1)) - s.at(u, std::max(v - hv, v0))) / (std::min(v + hv, v1) - std::max(v - hv, v0));
                const Vec3 d = q - p;
                const double a = dot(su, su), b = dot(su, sv), c = dot(sv, sv), e = dot(su, d), f = dot(sv, d);
                const double det = a * c - b * b;
                if (std::abs(det) < 1e-300) break;
                const double du = (c * e - b * f) / det, dv = (a * f - b * e) / det;
                u = std::clamp(u - du, u0, u1);
                v = std::clamp(v - dv, v0, v1);
                if (std::abs(du) < (u1 - u0) * 1e-12 && std::abs(dv) < (v1 - v0) * 1e-12) break;
            }
            return Vec2{u, v};
        };
        auto miss = [&](const Vec2& uv) { return length(s.at(uv.x, uv.y) - p); };
        Vec2 uv;
        bool found = false;
        if (hint) {  // start from the neighbour: keeps a run of points on one side of a seam
            uv = newton(hint->x, hint->y);
            found = miss(uv) < tol_ * 0.01;
        }
        if (!found) {
            // Newton from the few closest grid points (on a closed surface the nearest can sit across the seam)
            std::vector<std::pair<double, Vec2>> grid;
            const int n = 24;
            for (int i = 0; i <= n; ++i) {
                for (int j = 0; j <= n; ++j) {
                    const Vec2 c{u0 + (u1 - u0) * i / n, v0 + (v1 - v0) * j / n};
                    grid.push_back({miss(c), c});
                }
            }
            std::partial_sort(grid.begin(), grid.begin() + 4, grid.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            double bestD = 1e300;
            for (int k = 0; k < 4; ++k) {
                const Vec2 c = newton(grid[static_cast<std::size_t>(k)].second.x, grid[static_cast<std::size_t>(k)].second.y);
                const double d = miss(c);
                if (d < bestD) bestD = d, uv = c;
            }
        }
        // on the seam of a closed surface both ends of the range are the same point: take the neighbour's side
        if (hint) {
            const double eu = (u1 - u0) * 1e-6, ev = (v1 - v0) * 1e-6;
            if (s.closedU && (uv.x - u0 < eu || u1 - uv.x < eu)) {
                const double alt = uv.x - u0 < eu ? u1 : u0;
                if (std::abs(alt - hint->x) < std::abs(uv.x - hint->x)) uv.x = alt;
            }
            if (s.closedV && (uv.y - v0 < ev || v1 - uv.y < ev)) {
                const double alt = uv.y - v0 < ev ? v1 : v0;
                if (std::abs(alt - hint->y) < std::abs(uv.y - hint->y)) uv.y = alt;
            }
        }
        return uv;
    }

    // ---- faces ----

    void face(int faceId, const Mat4& m, Mesh& out) {
        const Record* r = get(faceId);
        if (!r) return;
        const ArgList* fa = r->part("ADVANCED_FACE");
        if (!fa) fa = r->part("FACE_SURFACE");
        const bool faceted = !fa && (r->is("FACE") || r->is("ORIENTED_FACE"));
        std::vector<int> bounds;
        Surface s;
        bool sameSense = true, haveSurface = false;
        if (fa) {
            bounds = refList(*fa, 1);
            haveSurface = surface(argRef(*fa, 2), s);
            sameSense = argBool(*fa, 3);
            if (!haveSurface) {
                const Record* sr = get(argRef(*fa, 2));
                ++skipped_[std::string(sr && !sr->type.empty() ? sr->type : "unknown surfaces") + " (approximated flat)"];
            }
        } else if (faceted) {
            const ArgList* a = r->part("FACE");
            if (!a) return;
            bounds = refList(*a, 1);
        } else {
            return;
        }
        std::vector<std::vector<Vec3>> loops;
        bool degenerate = false;
        for (int b : bounds) {
            auto l = loopPoints(b, degenerate);
            if (l.size() >= 2) loops.push_back(std::move(l));
        }
        if (!haveSurface) {
            // a plane through the loop
            if (loops.empty() || loops[0].size() < 3) return;
            Vec3 nrm, c;
            for (const auto& p : loops[0]) c = c + p;
            c = c / static_cast<double>(loops[0].size());
            for (std::size_t i = 0; i < loops[0].size(); ++i) nrm = nrm + cross(loops[0][i] - c, loops[0][(i + 1) % loops[0].size()] - c);
            if (length(nrm) < 1e-300) return;
            s.kind = Surface::Plane;
            s.f.o = c;
            s.f.z = normalize(nrm);
            s.f.x = normalize(std::abs(s.f.z.x) < 0.9 ? cross(Vec3{1, 0, 0}, s.f.z) : cross(Vec3{0, 1, 0}, s.f.z));
            s.f.y = cross(s.f.z, s.f.x);
            sameSense = true;
        }
        // loops in the surface's own orientation
        if (!sameSense) {
            for (auto& l : loops) std::reverse(l.begin(), l.end());
        }
        Mesh piece = tessellate(s, loops, degenerate);
        if (piece.empty()) {
            ++skipped_["faces that couldn't be tessellated"];
            return;
        }
        if (!sameSense) {
            for (std::size_t t = 0; t < piece.triangleCount(); ++t) std::swap(piece.indices[t * 3 + 1], piece.indices[t * 3 + 2]);
        }
        transform(piece, m);
        append(out, piece);
    }

    struct PVert {
        Vec2 uv;
        Vec3 p;
    };

    Mesh tessellate(const Surface& s, std::vector<std::vector<Vec3>> loops, bool degenerate) {
        // map each loop into (u, v), continuous along the loop
        std::vector<std::vector<PVert>> pl;
        std::vector<int> windU, windV;
        for (const auto& l : loops) {
            std::vector<PVert> v;
            Vec2 prev;
            for (std::size_t i = 0; i < l.size(); ++i) {
                const Vec2 uv = param(s, l[i], i ? &prev : nullptr);
                v.push_back({uv, l[i]});
                prev = uv;
            }
            // poles and apexes took the previous angle; give the first one the next angle instead
            if (v.size() > 1 && (s.kind == Surface::Cone || s.kind == Surface::Sphere)) {
                for (std::size_t i = 0; i < v.size(); ++i) {
                    const Vec3 lc = s.f.local(v[i].p);
                    if (std::hypot(lc.x, lc.y) < 1e-9 * (1 + s.r)) {
                        const PVert& nb = v[(i + 1) % v.size()];
                        const PVert& pb = v[(i + v.size() - 1) % v.size()];
                        // a pole between two different angles: split it into two points at the same pole
                        Vec2 a = pb.uv, b = nb.uv;
                        if (s.periodicU) b.x += kTau * std::round((a.x - b.x) / kTau);
                        v[i].uv.x = a.x;
                        if (std::abs(b.x - a.x) > 1e-9) {
                            v.insert(v.begin() + static_cast<std::ptrdiff_t>(i) + 1, PVert{{b.x, v[i].uv.y}, v[i].p});
                            ++i;
                        }
                    }
                }
            }
            const Vec2 back = v.size() > 1 ? param(s, l[0], &v.back().uv) : v[0].uv;
            windU.push_back(s.periodicU ? static_cast<int>(std::lround((back.x - v[0].uv.x) / kTau)) : 0);
            windV.push_back(s.periodicV ? static_cast<int>(std::lround((back.y - v[0].uv.y) / kTau)) : 0);
            pl.push_back(std::move(v));
        }
        auto areaOf = [](const std::vector<PVert>& v) {
            double a = 0;
            for (std::size_t i = 0; i < v.size(); ++i) a += cross(v[i].uv, v[(i + 1) % v.size()].uv);
            return a / 2;
        };
        // whole-surface faces (a sphere or torus with no real boundary)
        double total = 0;
        bool anyWinding = false;
        for (std::size_t i = 0; i < pl.size(); ++i) {
            total += std::abs(areaOf(pl[i]));
            anyWinding = anyWinding || windU[i] || windV[i];
        }
        if (pl.empty() || (total < 1e-12 && !anyWinding) || degenerate) {
            if (s.kind == Surface::Sphere) return grid(s, -kTau / 2, kTau / 2, -kHalfPi, kHalfPi, 64, 32);
            if (s.kind == Surface::Torus) return grid(s, 0, kTau, 0, kTau, 64, 24);
            if (pl.empty()) return {};
        }
        // loops that go once around a periodic direction: join two of them into one ring, or close one at a pole
        std::vector<std::vector<PVert>> rings;
        std::vector<std::size_t> winding;
        for (std::size_t i = 0; i < pl.size(); ++i) {
            if (windU[i] && !windV[i]) winding.push_back(i);
            else if (!windU[i] && !windV[i]) rings.push_back(pl[i]);
            else return {};  // goes around both ways (a torus band the other way round): not handled
        }
        // straight runs in (u, v) between two loops, sampled so they follow the surface
        auto seam = [&](std::vector<PVert>& ring, const Vec2& from, const Vec2& to) {
            const int n = 16;
            for (int k = 1; k < n; ++k) {
                const Vec2 uv = from + (to - from) * (static_cast<double>(k) / n);
                ring.push_back({uv, s.at(uv.x, uv.y)});
            }
        };
        if (winding.size() == 2) {
            std::vector<PVert> a = pl[winding[0]], b = pl[winding[1]];
            if (windU[winding[0]] == windU[winding[1]]) return {};
            if (windU[winding[0]] < 0) std::swap(a, b);  // a runs +u, b runs -u
            std::vector<PVert> ring = a;
            ring.push_back({{a[0].uv.x + kTau, a[0].uv.y}, a[0].p});  // a closes a period on
            // b starts where a closed (in u, modulo the period)
            const double endU = ring.back().uv.x;
            std::size_t start = 0;
            double bestD = 1e300;
            for (std::size_t i = 0; i < b.size(); ++i) {
                const double d = std::abs(std::remainder(b[i].uv.x - endU, kTau));
                if (d < bestD) bestD = d, start = i;
            }
            std::rotate(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(start), b.end());
            Vec2 prev{endU, b[0].uv.y};
            std::vector<PVert> bs;
            for (const auto& q : b) {
                Vec2 uv = q.uv;
                uv.x += kTau * std::round((prev.x - uv.x) / kTau);
                prev = uv;
                bs.push_back({uv, q.p});
            }
            Vec2 close = b[0].uv;
            close.x += kTau * std::round((prev.x - close.x) / kTau);
            bs.push_back({close, b[0].p});
            seam(ring, ring.back().uv, bs.front().uv);
            ring.insert(ring.end(), bs.begin(), bs.end());
            seam(ring, ring.back().uv, a[0].uv);
            rings.insert(rings.begin(), ring);
        } else if (winding.size() == 1) {
            std::vector<PVert> a = pl[winding[0]];
            const bool up = windU[winding[0]] > 0;  // the face lies to the left: above when running +u
            double vClose;
            if (s.kind == Surface::Sphere) vClose = up ? kHalfPi : -kHalfPi;
            else if (s.kind == Surface::Cone && std::abs(s.tanA) > 1e-12) vClose = -s.r / s.tanA;
            else return {};
            a.push_back({{a[0].uv.x + kTau * windU[winding[0]], a[0].uv.y}, a[0].p});
            const double uEnd = a.back().uv.x, uStart = a[0].uv.x;
            seam(a, a.back().uv, {uEnd, vClose});
            const int n = 32;
            for (int k = 0; k <= n; ++k) {
                const double u = uEnd + (uStart - uEnd) * k / n;
                a.push_back({{u, vClose}, s.at(u, vClose)});
            }
            seam(a, {uStart, vClose}, a[0].uv);
            rings.insert(rings.begin(), a);
        } else if (!winding.empty()) {
            return {};
        }
        if (rings.empty()) return {};
        // the outer ring has the largest area; it must run counter-clockwise for the triangles to face out
        std::size_t outer = 0;
        for (std::size_t i = 1; i < rings.size(); ++i) {
            if (std::abs(areaOf(rings[i])) > std::abs(areaOf(rings[outer]))) outer = i;
        }
        std::swap(rings[0], rings[outer]);
        // a periodic face may sit a period away from its holes: line the holes up with it
        if (s.periodicU) {
            double uMid = 0;
            for (const auto& p : rings[0]) uMid += p.uv.x;
            uMid /= static_cast<double>(rings[0].size());
            for (std::size_t i = 1; i < rings.size(); ++i) {
                double hm = 0;
                for (const auto& p : rings[i]) hm += p.uv.x;
                hm /= static_cast<double>(rings[i].size());
                const double shift = kTau * std::round((uMid - hm) / kTau);
                for (auto& p : rings[i]) p.uv.x += shift;
            }
        }
        Polygon2 poly;
        std::vector<PVert> verts;
        for (std::size_t i = 0; i < rings.size(); ++i) {
            std::vector<Vec2> ring;
            for (const auto& p : rings[i]) {
                ring.push_back(p.uv);
                verts.push_back(p);
            }
            if (i == 0) poly.outer = ring;
            else poly.holes.push_back(ring);
        }
        std::vector<std::uint32_t> tris;
        try {
            tris = triangulate(poly);
        } catch (const std::exception&) {
            return {};
        }
        if (tris.empty()) return {};
        if (s.kind != Surface::Plane) refine(s, verts, tris);
        Mesh m;
        for (const auto& v : verts) m.addVertex(v.p);
        // ear clipping winds counter-clockwise in (u, v), which faces along the surface normal
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            const Vec3 &a = m.positions[tris[t]], &b = m.positions[tris[t + 1]], &c = m.positions[tris[t + 2]];
            // collapsed at a pole; a sliver along collinear boundary points stays, or the face would leave a gap
            if (a == b || b == c || a == c) continue;
            m.addTriangle(tris[t], tris[t + 1], tris[t + 2]);
        }
        return m;
    }

    // Splits inner edges (not the boundary) whose middle strays from the surface.
    void refine(const Surface& s, std::vector<PVert>& verts, std::vector<std::uint32_t>& tris) const {
        const std::size_t boundaryCount = verts.size();
        const double tol = tol_;
        auto key = [](std::uint32_t a, std::uint32_t b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
        // a rough metric so (u, v) distances look like surface distances
        Vec2 c;
        for (const auto& v : verts) c = c + v.uv;
        c = c / static_cast<double>(verts.size());
        const double h = 1e-4;
        double su = length(s.at(c.x + h, c.y) - s.at(c.x - h, c.y)) / (2 * h), sv = length(s.at(c.x, c.y + h) - s.at(c.x, c.y - h)) / (2 * h);
        if (!(su > 1e-12)) su = 1;
        if (!(sv > 1e-12)) sv = 1;
        auto at = [&](std::uint32_t i) { return Vec2{verts[i].uv.x * su, verts[i].uv.y * sv}; };
        // Lawson flips towards a Delaunay triangulation: keeps the splits from making slivers
        auto flips = [&]() {
            for (int round = 0; round < 50; ++round) {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edges;
                for (std::size_t t = 0; t * 3 < tris.size(); ++t) {
                    for (int k = 0; k < 3; ++k) edges[key(tris[t * 3 + static_cast<std::size_t>(k)], tris[t * 3 + static_cast<std::size_t>((k + 1) % 3)])].push_back(t);
                }
                std::set<std::size_t> touched;
                int flipped = 0;
                for (const auto& [e, ts] : edges) {
                    if (ts.size() != 2 || touched.count(ts[0]) || touched.count(ts[1])) continue;
                    std::uint32_t* t1 = &tris[ts[0] * 3];
                    std::uint32_t* t2 = &tris[ts[1] * 3];
                    {
                        // t1 should be the proper triangle (a collinear ear from ear clipping is the one to get rid of)
                        auto turn = [&](const std::uint32_t* t) { return cross(at(t[1]) - at(t[0]), at(t[2]) - at(t[0])); };
                        if (turn(t1) <= 0) std::swap(t1, t2);
                    }
                    // t1 = (a, b, c) with edge a-b; t2 holds b-a and its third corner d
                    int k1 = 0;
                    while (k1 < 3 && key(t1[k1], t1[(k1 + 1) % 3]) != e) ++k1;
                    const std::uint32_t a = t1[k1], b = t1[(k1 + 1) % 3], cc = t1[(k1 + 2) % 3];
                    std::uint32_t d = t2[0] + t2[1] + t2[2] - a - b;
                    const Vec2 pa = at(a), pb = at(b), pc = at(cc), pd = at(d);
                    if (cross(pb - pa, pc - pa) <= 0) continue;
                    const double adx = pa.x - pd.x, ady = pa.y - pd.y, bdx = pb.x - pd.x, bdy = pb.y - pd.y, cdx = pc.x - pd.x, cdy = pc.y - pd.y;
                    const double det = (adx * adx + ady * ady) * (bdx * cdy - cdx * bdy) - (bdx * bdx + bdy * bdy) * (adx * cdy - cdx * ady) +
                                       (cdx * cdx + cdy * cdy) * (adx * bdy - bdx * ady);
                    const double scale = (adx * adx + ady * ady + bdx * bdx + bdy * bdy + cdx * cdx + cdy * cdy);
                    if (det <= 1e-12 * scale * scale) continue;
                    // the new triangles (c, a, d) and (d, b, c) must both turn the right way
                    if (cross(pa - pc, pd - pc) <= 0 || cross(pb - pd, pc - pd) <= 0) continue;
                    t1[0] = cc, t1[1] = a, t1[2] = d;
                    t2[0] = d, t2[1] = b, t2[2] = cc;
                    touched.insert(ts[0]);
                    touched.insert(ts[1]);
                    ++flipped;
                }
                if (!flipped) break;
            }
        };
        flips();
        for (int pass = 0; pass < 12; ++pass) {
            // edge -> triangles
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>> edges;
            for (std::size_t t = 0; t * 3 < tris.size(); ++t) {
                for (int k = 0; k < 3; ++k) edges[key(tris[t * 3 + static_cast<std::size_t>(k)], tris[t * 3 + static_cast<std::size_t>((k + 1) % 3)])].push_back(t);
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> split;
            for (const auto& [e, ts] : edges) {
                if (ts.size() != 2) continue;  // the boundary (one triangle) stays as sampled, shared with the next face
                const Vec2 mid = (verts[e.first].uv + verts[e.second].uv) * 0.5;
                const Vec3 chord = (verts[e.first].p + verts[e.second].p) * 0.5;
                if (length(s.at(mid.x, mid.y) - chord) > tol) split.push_back(e);
            }
            if (split.empty()) break;
            std::set<std::size_t> touched;
            for (const auto& e : split) {
                auto it = edges.find(e);
                if (it == edges.end()) continue;
                bool fresh = true;
                for (std::size_t t : it->second) fresh = fresh && !touched.count(t);
                if (!fresh) continue;  // one split per triangle per pass
                const Vec2 mid = (verts[e.first].uv + verts[e.second].uv) * 0.5;
                const auto nv = static_cast<std::uint32_t>(verts.size());
                verts.push_back({mid, s.at(mid.x, mid.y)});
                for (std::size_t t : it->second) {
                    touched.insert(t);
                    std::uint32_t* tri = &tris[t * 3];
                    int k = 0;
                    while (k < 3 && !(key(tri[k], tri[(k + 1) % 3]) == e)) ++k;
                    const std::uint32_t a = tri[k], b = tri[(k + 1) % 3], c = tri[(k + 2) % 3];
                    tri[0] = a, tri[1] = nv, tri[2] = c;
                    tris.insert(tris.end(), {nv, b, c});
                }
            }
            flips();
            if (verts.size() > boundaryCount + 200000) break;
        }
    }

    Mesh grid(const Surface& s, double u0, double u1, double v0, double v1, int nu, int nv) const {
        Mesh m;
        for (int j = 0; j <= nv; ++j) {
            for (int i = 0; i <= nu; ++i) m.addVertex(s.at(u0 + (u1 - u0) * i / nu, v0 + (v1 - v0) * j / nv));
        }
        for (int j = 0; j < nv; ++j) {
            for (int i = 0; i < nu; ++i) {
                const auto a = static_cast<std::uint32_t>(j * (nu + 1) + i), b = a + 1, c = a + static_cast<std::uint32_t>(nu + 1), d = c + 1;
                const Vec3 &pa = m.positions[a], &pb = m.positions[b], &pc = m.positions[c], &pd = m.positions[d];
                if (length(cross(pb - pa, pd - pa)) > 1e-18) m.addTriangle(a, b, d);
                if (length(cross(pd - pa, pc - pa)) > 1e-18) m.addTriangle(a, d, c);
            }
        }
        return m;
    }

    void shell(int shellId, const Mat4& m, Mesh& out, std::size_t& faces) {
        const Record* r = get(shellId);
        if (!r) return;
        const ArgList* a = r->part("CLOSED_SHELL");
        if (!a) a = r->part("OPEN_SHELL");
        if (!a) {
            if (const ArgList* o = r->part("ORIENTED_CLOSED_SHELL")) {
                Mesh inner;
                shell(argRef(*o, 2), m, inner, faces);
                if (!argBool(*o, 3)) flip(inner);
                append(out, inner);
            }
            return;
        }
        for (int f : refList(*a, 1)) {
            face(f, m, out);
            ++faces;
        }
    }

    void solid(int id, const Mat4& m, Mesh& out, std::size_t& faces, int depth) {
        const Record* r = get(id);
        if (!r) return;
        if (const ArgList* a = r->part("MANIFOLD_SOLID_BREP")) {
            shell(argRef(*a, 1), m, out, faces);
            if (const ArgList* v = r->part("BREP_WITH_VOIDS")) {
                for (int s : refList(*v, 0)) shell(s, m, out, faces);
            }
        } else if (const ArgList* bv = r->part("BREP_WITH_VOIDS")) {
            shell(argRef(*bv, 1), m, out, faces);
            for (int s : refList(*bv, 2)) shell(s, m, out, faces);
        } else if (const ArgList* fb = r->part("FACETED_BREP")) {
            shell(argRef(*fb, 1), m, out, faces);
        } else if (const ArgList* sm = r->part("SHELL_BASED_SURFACE_MODEL")) {
            for (int s : refList(*sm, 1)) shell(s, m, out, faces);
        } else if (const ArgList* mi = r->part("MAPPED_ITEM")) {
            const Record* map = get(argRef(*mi, 1));
            if (!map || !map->is("REPRESENTATION_MAP") || depth > 32) return;
            const ArgList& ma = *map->part("REPRESENTATION_MAP");
            const Mat4 t = matrix(frame(argRef(*mi, 2))) * matrix(frame(argRef(ma, 0))).inverse();
            const Record* rep = get(argRef(ma, 1));
            if (!rep) return;
            const ArgList* items = nullptr;
            for (const auto& [name, args] : rep->parts) items = &args;
            if (!rep->type.empty()) items = &rep->args;
            if (items) {
                for (int item : refList(*items, 1)) solid(item, m * t, out, faces, depth + 1);
            }
        }
    }

    std::unordered_map<int, Record> e_;
    std::unordered_map<int, std::vector<Vec3>> edgeCache_;
    std::map<std::string, int> skipped_;
    double scale_ = 1, tol_ = 1e-3;
};

}  // namespace

Mesh readStep(const std::string& text, std::string* notes) { return Reader(text).read(notes); }

}  // namespace wetwaresuite::mesh
