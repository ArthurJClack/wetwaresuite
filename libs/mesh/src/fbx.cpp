// Autodesk FBX: binary (any version, 32- or 64-bit offsets, zlib arrays) and ASCII files are
// read; binary 7.4 is written. Only static geometry crosses: every mesh model, placed by its
// node transforms, with normals and the first UV set. FBX files are usually Y-up and in
// centimetres; meshes come in Z-up with the file's numbers kept (written files say millimetres,
// the unit the suite's CAD tools use).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::mesh {

namespace {

[[noreturn]] void bad(const std::string& why) { throw std::runtime_error("FBX: " + why); }

// ---- the node tree -----------------------------------------------------------------

using Prop = std::variant<std::int64_t, double, std::string, std::vector<double>, std::vector<std::int64_t>>;

struct Node {
    std::string name;
    std::vector<Prop> props;
    std::vector<Node> children;

    const Node* child(const std::string& n) const {
        for (const auto& c : children) {
            if (c.name == n) return &c;
        }
        return nullptr;
    }
    std::int64_t integer(std::size_t i, std::int64_t fallback = 0) const {
        if (i >= props.size()) return fallback;
        if (auto v = std::get_if<std::int64_t>(&props[i])) return *v;
        if (auto d = std::get_if<double>(&props[i])) return static_cast<std::int64_t>(*d);
        return fallback;
    }
    double number(std::size_t i, double fallback = 0) const {
        if (i >= props.size()) return fallback;
        if (auto d = std::get_if<double>(&props[i])) return *d;
        if (auto v = std::get_if<std::int64_t>(&props[i])) return static_cast<double>(*v);
        return fallback;
    }
    std::string text(std::size_t i) const {
        if (i < props.size()) {
            if (auto s = std::get_if<std::string>(&props[i])) return *s;
        }
        return {};
    }
    std::vector<double> numbers(std::size_t i) const {
        if (i >= props.size()) return {};
        if (auto d = std::get_if<std::vector<double>>(&props[i])) return *d;
        if (auto v = std::get_if<std::vector<std::int64_t>>(&props[i])) return {v->begin(), v->end()};
        return {};
    }
    std::vector<std::int64_t> integers(std::size_t i) const {
        if (i >= props.size()) return {};
        if (auto v = std::get_if<std::vector<std::int64_t>>(&props[i])) return *v;
        if (auto d = std::get_if<std::vector<double>>(&props[i])) {
            std::vector<std::int64_t> out;
            out.reserve(d->size());
            for (double x : *d) out.push_back(static_cast<std::int64_t>(std::llround(x)));
            return out;
        }
        return {};
    }
};

// ---- binary reader -------------------------------------------------------------------

constexpr char kBinaryMagic[] = "Kaydara FBX Binary  ";  // + \0 \x1a \0, then the version

class BinaryReader {
public:
    explicit BinaryReader(const std::string& b) : b_(b) {}

    Node read() {
        if (b_.size() < 27) bad("file is cut short");
        version_ = u32(23);
        std::size_t at = 27;
        Node root;
        while (at < b_.size()) {
            Node n;
            const std::size_t next = node(at, n);
            if (next == 0) break;  // the null record that ends the top level
            root.children.push_back(std::move(n));
            at = next;
        }
        return root;
    }

private:
    const std::string& b_;
    std::uint32_t version_ = 0;

    void need(std::size_t at, std::size_t n) const {
        if (at > b_.size() || n > b_.size() - at) bad("file is cut short");
    }
    std::uint32_t u32(std::size_t at) const {
        need(at, 4);
        std::uint32_t v;
        std::memcpy(&v, b_.data() + at, 4);
        return v;  // FBX is little-endian, as is every machine this builds for
    }
    std::uint64_t u64(std::size_t at) const {
        need(at, 8);
        std::uint64_t v;
        std::memcpy(&v, b_.data() + at, 8);
        return v;
    }
    template <typename T>
    T get(std::size_t at) const {
        need(at, sizeof(T));
        T v;
        std::memcpy(&v, b_.data() + at, sizeof(T));
        return v;
    }

    // Reads the node at `at` into n; returns the offset after it, or 0 for a null record.
    std::size_t node(std::size_t at, Node& n, int depth = 0) {
        if (depth > 64) bad("nodes nested too deeply");
        const bool wide = version_ >= 7500;
        const std::uint64_t end = wide ? u64(at) : u32(at);
        const std::uint64_t count = wide ? u64(at + 8) : u32(at + 4);
        const std::size_t head = wide ? 25 : 13;
        need(at, head);
        const std::size_t nameLen = static_cast<unsigned char>(b_[at + head - 1]);
        if (end == 0) return 0;
        if (end > b_.size() || end <= at) bad("corrupt node");
        need(at + head, nameLen);
        n.name = b_.substr(at + head, nameLen);
        std::size_t p = at + head + nameLen;
        for (std::uint64_t i = 0; i < count; ++i) p = prop(p, n.props);
        while (p < end) {
            Node c;
            const std::size_t next = node(p, c, depth + 1);
            if (next == 0) break;
            n.children.push_back(std::move(c));
            p = next;
        }
        return static_cast<std::size_t>(end);
    }

    std::size_t prop(std::size_t p, std::vector<Prop>& out) {
        need(p, 1);
        const char type = b_[p++];
        switch (type) {
            case 'Y': out.emplace_back(std::int64_t{get<std::int16_t>(p)}); return p + 2;
            case 'C': out.emplace_back(std::int64_t{get<std::uint8_t>(p)}); return p + 1;
            case 'I': out.emplace_back(std::int64_t{get<std::int32_t>(p)}); return p + 4;
            case 'L': out.emplace_back(get<std::int64_t>(p)); return p + 8;
            case 'F': out.emplace_back(double{get<float>(p)}); return p + 4;
            case 'D': out.emplace_back(get<double>(p)); return p + 8;
            case 'S':
            case 'R': {
                const std::uint32_t len = u32(p);
                need(p + 4, len);
                out.emplace_back(b_.substr(p + 4, len));
                return p + 4 + len;
            }
            case 'f': case 'd': case 'l': case 'i': case 'b': {
                const std::uint32_t len = u32(p), encoding = u32(p + 4), size = u32(p + 8);
                need(p + 12, size);
                const std::size_t elem = type == 'd' || type == 'l' ? 8 : type == 'b' ? 1 : 4;
                if (len > (1u << 28)) bad("array too large");
                std::string data = b_.substr(p + 12, size);
                if (encoding == 1) data = raster::inflateZlib(data);
                if (data.size() < static_cast<std::size_t>(len) * elem) bad("array is cut short");
                if (type == 'f' || type == 'd') {
                    std::vector<double> v(len);
                    for (std::uint32_t i = 0; i < len; ++i) {
                        if (type == 'd') std::memcpy(&v[i], data.data() + i * 8, 8);
                        else {
                            float f;
                            std::memcpy(&f, data.data() + i * 4, 4);
                            v[i] = f;
                        }
                    }
                    out.emplace_back(std::move(v));
                } else {
                    std::vector<std::int64_t> v(len);
                    for (std::uint32_t i = 0; i < len; ++i) {
                        if (type == 'l') std::memcpy(&v[i], data.data() + i * 8, 8);
                        else if (type == 'i') {
                            std::int32_t x;
                            std::memcpy(&x, data.data() + i * 4, 4);
                            v[i] = x;
                        } else {
                            v[i] = static_cast<unsigned char>(data[i]);
                        }
                    }
                    out.emplace_back(std::move(v));
                }
                return p + 12 + size;
            }
            default: bad(std::string("unknown property type '") + type + "'");
        }
    }
};

// ---- ASCII reader --------------------------------------------------------------------

class AsciiReader {
public:
    explicit AsciiReader(const std::string& t) : t_(t) {}

    Node read() {
        Node root;
        while (true) {
            skip();
            if (p_ >= t_.size()) break;
            if (t_[p_] == '}') bad("unexpected '}'");
            root.children.push_back(node(0));
        }
        return root;
    }

private:
    const std::string& t_;
    std::size_t p_ = 0;

    void skip() {
        while (p_ < t_.size()) {
            const char c = t_[p_];
            if (c == ';') {
                while (p_ < t_.size() && t_[p_] != '\n') ++p_;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                ++p_;
            } else {
                break;
            }
        }
    }
    // spaces only (a node's properties end at the line break)
    void skipInline() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\r')) ++p_;
    }

    Node node(int depth) {
        if (depth > 64) bad("nodes nested too deeply");
        Node n;
        const std::size_t start = p_;
        while (p_ < t_.size() && t_[p_] != ':') {
            if (t_[p_] == '\n' || t_[p_] == '{' || t_[p_] == '}') bad("expected 'Name:' near offset " + std::to_string(start));
            ++p_;
        }
        if (p_ >= t_.size()) bad("file is cut short");
        n.name = t_.substr(start, p_ - start);
        while (!n.name.empty() && std::isspace(static_cast<unsigned char>(n.name.back()))) n.name.pop_back();
        ++p_;  // ':'
        values(n);
        skipInline();
        if (p_ < t_.size() && t_[p_] == '{') {
            ++p_;
            for (;;) {
                skip();
                if (p_ >= t_.size()) bad("missing '}'");
                if (t_[p_] == '}') {
                    ++p_;
                    break;
                }
                n.children.push_back(node(depth + 1));
            }
        }
        return n;
    }

    // Comma-separated values up to the end of the line (a trailing comma continues it).
    void values(Node& n) {
        for (;;) {
            skipInline();
            if (p_ >= t_.size() || t_[p_] == '\n' || t_[p_] == '{' || t_[p_] == '}' || t_[p_] == ';') return;
            const char c = t_[p_];
            if (c == '"') {
                const std::size_t s = ++p_;
                while (p_ < t_.size() && t_[p_] != '"') ++p_;
                std::string v = t_.substr(s, p_ - s);
                ++p_;
                // "Name::Model" in ASCII is "Model\0\x01Name" in binary
                const auto sep = v.find("::");
                if (sep != std::string::npos) v = v.substr(sep + 2) + std::string("\0\x01", 2) + v.substr(0, sep);
                n.props.emplace_back(std::move(v));
            } else if (c == '*') {
                // *N { a: 1,2,3 }
                ++p_;
                while (p_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[p_]))) ++p_;
                skip();
                if (p_ >= t_.size() || t_[p_] != '{') bad("expected '{' after array size");
                ++p_;
                skip();
                while (p_ < t_.size() && t_[p_] != ':') ++p_;
                ++p_;
                std::vector<double> v;
                bool integral = true;
                for (;;) {
                    skip();
                    if (p_ >= t_.size()) bad("missing '}' after array");
                    if (t_[p_] == '}') {
                        ++p_;
                        break;
                    }
                    if (t_[p_] == ',') {
                        ++p_;
                        continue;
                    }
                    const std::size_t s = p_;
                    char* e = nullptr;
                    const double x = std::strtod(t_.c_str() + p_, &e);
                    p_ = static_cast<std::size_t>(e - t_.c_str());
                    if (p_ == s) bad("bad number in array");
                    integral = integral && std::memchr(t_.data() + s, '.', p_ - s) == nullptr &&
                               std::memchr(t_.data() + s, 'e', p_ - s) == nullptr;
                    v.push_back(x);
                }
                if (integral) {
                    std::vector<std::int64_t> iv;
                    iv.reserve(v.size());
                    for (double x : v) iv.push_back(static_cast<std::int64_t>(x));
                    n.props.emplace_back(std::move(iv));
                } else {
                    n.props.emplace_back(std::move(v));
                }
            } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
                const std::size_t s = p_;
                char* e = nullptr;
                const double x = std::strtod(t_.c_str() + p_, &e);
                p_ = static_cast<std::size_t>(e - t_.c_str());
                if (p_ == s) bad("bad number");
                const std::string tok = t_.substr(s, p_ - s);
                if (tok.find_first_of(".eE") == std::string::npos) n.props.emplace_back(static_cast<std::int64_t>(std::llround(x)));
                else n.props.emplace_back(x);
            } else {
                // bare word (T, Y, W, ...): keep as text
                const std::size_t s = p_;
                while (p_ < t_.size() && !std::isspace(static_cast<unsigned char>(t_[p_])) && t_[p_] != ',' && t_[p_] != '{') ++p_;
                n.props.emplace_back(t_.substr(s, p_ - s));
            }
            skipInline();
            if (p_ < t_.size() && t_[p_] == ',') {
                ++p_;
                skip();  // values can continue on the next line
                continue;
            }
            return;
        }
    }
};

// ---- scene -> mesh -------------------------------------------------------------------

// Properties70 { P: "Name", "type", "label", "flags", values... }
std::vector<double> property(const Node* obj, const std::string& name) {
    const Node* props = obj ? obj->child("Properties70") : nullptr;
    if (!props) props = obj ? obj->child("Properties60") : nullptr;
    if (!props) return {};
    for (const auto& p : props->children) {
        if (p.text(0) != name) continue;
        std::vector<double> out;
        for (std::size_t i = 4; i < p.props.size(); ++i) {
            if (std::holds_alternative<double>(p.props[i]) || std::holds_alternative<std::int64_t>(p.props[i])) out.push_back(p.number(i));
        }
        if (out.empty() && p.props.size() > 3) {  // Properties60 has one fewer column
            for (std::size_t i = 3; i < p.props.size(); ++i) {
                if (std::holds_alternative<double>(p.props[i]) || std::holds_alternative<std::int64_t>(p.props[i])) out.push_back(p.number(i));
            }
        }
        return out;
    }
    return {};
}

Vec3 vec(const Node* obj, const std::string& name, Vec3 fallback) {
    auto v = property(obj, name);
    return v.size() >= 3 ? Vec3{v[0], v[1], v[2]} : fallback;
}

// Euler angles in degrees, applied in the given FBX order (0 = XYZ: X first).
Mat4 euler(const Vec3& r, int order) {
    const Mat4 X = Mat4::rotateX(r.x), Y = Mat4::rotateY(r.y), Z = Mat4::rotateZ(r.z);
    switch (order) {
        case 1: return Y * Z * X;  // XZY
        case 2: return X * Z * Y;  // YZX
        case 3: return Z * X * Y;  // YXZ
        case 4: return Y * X * Z;  // ZXY
        case 5: return X * Y * Z;  // ZYX
        default: return Z * Y * X; // XYZ
    }
}

Mat4 localMatrix(const Node* model) {
    const int order = static_cast<int>(property(model, "RotationOrder").empty() ? 0 : property(model, "RotationOrder")[0]);
    const Vec3 t = vec(model, "Lcl Translation", {});
    const Vec3 r = vec(model, "Lcl Rotation", {});
    const Vec3 s = vec(model, "Lcl Scaling", {1, 1, 1});
    const Vec3 pre = vec(model, "PreRotation", {});
    const Vec3 post = vec(model, "PostRotation", {});
    const Vec3 roff = vec(model, "RotationOffset", {}), rpiv = vec(model, "RotationPivot", {});
    const Vec3 soff = vec(model, "ScalingOffset", {}), spiv = vec(model, "ScalingPivot", {});
    return Mat4::translate(t) * Mat4::translate(roff) * Mat4::translate(rpiv) * euler(pre, 0) * euler(r, order) *
           euler(post, 0).inverse() * Mat4::translate(-rpiv) * Mat4::translate(soff) * Mat4::translate(spiv) * Mat4::scale(s) *
           Mat4::translate(-spiv);
}

Mat4 geometricMatrix(const Node* model) {
    return Mat4::translate(vec(model, "GeometricTranslation", {})) * euler(vec(model, "GeometricRotation", {}), 0) *
           Mat4::scale(vec(model, "GeometricScaling", {1, 1, 1}));
}

// One layer element (normals or UVs), read per polygon corner.
struct Layer {
    std::vector<double> data;
    std::vector<std::int64_t> index;
    std::string mapping, reference;
    int width = 3;
    bool present() const { return !data.empty(); }
    // value number for polygon-vertex k (corner), control point v, polygon f
    std::int64_t pick(std::size_t corner, std::int64_t point, std::size_t polygon) const {
        std::int64_t i = 0;
        if (mapping == "ByPolygonVertex") i = static_cast<std::int64_t>(corner);
        else if (mapping == "ByVertice" || mapping == "ByVertex" || mapping == "ByControlPoint") i = point;
        else if (mapping == "ByPolygon") i = static_cast<std::int64_t>(polygon);
        else if (mapping == "AllSame") i = 0;
        else i = static_cast<std::int64_t>(corner);
        if (reference == "IndexToDirect" || reference == "Index") {
            if (i < 0 || static_cast<std::size_t>(i) >= index.size()) return -1;
            i = index[static_cast<std::size_t>(i)];
        }
        if (i < 0 || static_cast<std::size_t>(i + 1) * static_cast<std::size_t>(width) > data.size()) return -1;
        return i;
    }
};

Layer layer(const Node* geom, const char* element, const char* values, const char* indexName, int width) {
    Layer l;
    l.width = width;
    const Node* e = geom->child(element);
    if (!e) return l;
    if (const Node* v = e->child(values)) l.data = v->numbers(0);
    if (const Node* i = e->child(indexName)) l.index = i->integers(0);
    if (const Node* m = e->child("MappingInformationType")) l.mapping = m->text(0);
    if (const Node* r = e->child("ReferenceInformationType")) l.reference = r->text(0);
    return l;
}

void addGeometry(Mesh& out, const Node& geom, const Mat4& m) {
    const Node* vn = geom.child("Vertices");
    const Node* pn = geom.child("PolygonVertexIndex");
    if (!vn || !pn) return;
    const std::vector<double> v = vn->numbers(0);
    const std::vector<std::int64_t> poly = pn->integers(0);
    const std::size_t points = v.size() / 3;
    const Layer normals = layer(&geom, "LayerElementNormal", "Normals", "NormalsIndex", 3);
    const Layer uvs = layer(&geom, "LayerElementUV", "UV", "UVIndex", 2);
    const bool mirror = m.determinant3() < 0;

    // corners become vertices; identical (point, normal, uv) corners are shared
    std::map<std::tuple<std::int64_t, std::int64_t, std::int64_t>, std::uint32_t> seen;
    const bool firstMesh = out.positions.empty();
    const bool withNormals = normals.present() && (firstMesh || out.hasNormals());
    const bool withUvs = uvs.present() && (firstMesh || out.hasUvs());
    if (!firstMesh && out.hasNormals() && !withNormals) out.normals.clear();
    if (!firstMesh && out.hasUvs() && !withUvs) out.uvs.clear();

    std::vector<std::uint32_t> face;
    std::size_t polygon = 0;
    for (std::size_t c = 0; c < poly.size(); ++c) {
        std::int64_t pi = poly[c];
        const bool last = pi < 0;
        if (last) pi = ~pi;
        if (pi < 0 || static_cast<std::size_t>(pi) >= points) bad("polygon uses a missing vertex");
        const std::int64_t ni = withNormals ? normals.pick(c, pi, polygon) : -1;
        const std::int64_t ti = withUvs ? uvs.pick(c, pi, polygon) : -1;
        auto key = std::make_tuple(pi, ni, ti);
        auto it = seen.find(key);
        std::uint32_t id;
        if (it != seen.end()) {
            id = it->second;
        } else {
            const std::size_t p = static_cast<std::size_t>(pi) * 3;
            id = out.addVertex(m.point({v[p], v[p + 1], v[p + 2]}));
            if (withNormals) {
                Vec3 n{0, 0, 1};
                if (ni >= 0) n = {normals.data[static_cast<std::size_t>(ni) * 3], normals.data[static_cast<std::size_t>(ni) * 3 + 1],
                                  normals.data[static_cast<std::size_t>(ni) * 3 + 2]};
                out.normals.push_back(m.normal(n));
            }
            if (withUvs) {
                Vec2 t{};
                if (ti >= 0) t = {uvs.data[static_cast<std::size_t>(ti) * 2], uvs.data[static_cast<std::size_t>(ti) * 2 + 1]};
                out.uvs.push_back(t);
            }
            seen.emplace(key, id);
        }
        face.push_back(id);
        if (last) {
            for (std::size_t k = 1; k + 1 < face.size(); ++k) {
                if (mirror) out.addTriangle(face[0], face[k + 1], face[k]);
                else out.addTriangle(face[0], face[k], face[k + 1]);
            }
            face.clear();
            ++polygon;
        }
    }
}

Mesh sceneToMesh(const Node& root) {
    const Node* objects = root.child("Objects");
    if (!objects) bad("no Objects section");
    std::map<std::int64_t, const Node*> models, geometries;
    for (const auto& o : objects->children) {
        if (o.name == "Model") models[o.integer(0)] = &o;
        else if (o.name == "Geometry" && (o.text(2) == "Mesh" || o.child("Vertices"))) geometries[o.integer(0)] = &o;
    }
    std::map<std::int64_t, std::int64_t> parentOf;           // model -> parent model
    std::multimap<std::int64_t, std::int64_t> geometryOf;    // model -> geometry
    if (const Node* conns = root.child("Connections")) {
        for (const auto& c : conns->children) {
            if (c.name != "C" && c.name != "Connect") continue;
            if (c.text(0) != "OO") continue;
            const std::int64_t child = c.integer(1), parent = c.integer(2);
            if (geometries.count(child) && models.count(parent)) geometryOf.emplace(parent, child);
            else if (models.count(child) && models.count(parent)) parentOf[child] = parent;
        }
    }

    // axes: the file's up/front axes onto Z-up
    Mat4 axes = Mat4::rotateX(90);  // the usual Y-up
    const Node* settings = root.child("GlobalSettings");
    if (settings && !property(settings, "UpAxis").empty()) {
        const int up = static_cast<int>(property(settings, "UpAxis")[0]);
        const double upSign = property(settings, "UpAxisSign").empty() ? 1 : property(settings, "UpAxisSign")[0];
        const int front = property(settings, "FrontAxis").empty() ? 2 : static_cast<int>(property(settings, "FrontAxis")[0]);
        const double frontSign = property(settings, "FrontAxisSign").empty() ? 1 : property(settings, "FrontAxisSign")[0];
        const int coord = property(settings, "CoordAxis").empty() ? 0 : static_cast<int>(property(settings, "CoordAxis")[0]);
        const double coordSign = property(settings, "CoordAxisSign").empty() ? 1 : property(settings, "CoordAxisSign")[0];
        if (up >= 0 && up < 3 && front >= 0 && front < 3 && coord >= 0 && coord < 3 && up != front && up != coord && front != coord) {
            // file axes -> ours: coord -> +X, up -> +Z, front -> -Y (FBX's front points at the viewer)
            Mat4 a;
            for (int i = 0; i < 16; ++i) a.m[i] = 0;
            a.at(0, coord) = coordSign;
            a.at(2, up) = upSign;
            a.at(1, front) = -frontSign;
            a.at(3, 3) = 1;
            axes = a;
        }
    }

    std::function<Mat4(std::int64_t, int)> world = [&](std::int64_t id, int depth) -> Mat4 {
        const Mat4 local = localMatrix(models[id]);
        auto p = parentOf.find(id);
        if (p == parentOf.end() || depth > 64) return local;
        return world(p->second, depth + 1) * local;
    };

    Mesh out;
    bool any = false;
    for (const auto& [modelId, geomId] : geometryOf) {
        const Node* model = models[modelId];
        if (property(model, "Visibility").size() == 1 && property(model, "Visibility")[0] == 0) continue;
        addGeometry(out, *geometries[geomId], axes * world(modelId, 0) * geometricMatrix(model));
        any = true;
    }
    if (!any) {
        // FBX 6 keeps the mesh inside its Model node; otherwise geometry with no model: take it as it is
        for (const auto& o : objects->children) {
            if (o.name == "Model" && o.child("Vertices")) {
                addGeometry(out, o, axes * localMatrix(&o) * geometricMatrix(&o));
                any = true;
            }
        }
        if (!any) {
            for (const auto& [id, g] : geometries) addGeometry(out, *g, axes);
        }
    }
    if (out.empty()) bad("no mesh geometry found");
    return out;
}

// ---- binary writer -------------------------------------------------------------------

class Writer {
public:
    std::string out;

    void begin(const std::string& name) {
        stack_.push_back(out.size());
        out.append(12, '\0');  // end offset, property count, property bytes (filled in by props/end)
        out += static_cast<char>(name.size());
        out += name;
        propStart_ = out.size();
        props_ = 0;
    }
    void i32(std::int32_t v) { put('I', &v, 4); }
    void i64(std::int64_t v) { put('L', &v, 8); }
    void f64(double v) { put('D', &v, 8); }
    void i16(std::int16_t v) { put('Y', &v, 2); }
    void str(const std::string& s) {
        out += 'S';
        u32(static_cast<std::uint32_t>(s.size()));
        out += s;
        ++props_;
    }
    void raw(const std::string& s) {
        out += 'R';
        u32(static_cast<std::uint32_t>(s.size()));
        out += s;
        ++props_;
    }
    void doubles(const std::vector<double>& v) { array('d', v.data(), v.size(), 8); }
    void ints(const std::vector<std::int32_t>& v) { array('i', v.data(), v.size(), 4); }
    // Call after the properties, before children.
    void endProps() {
        const std::size_t at = stack_.back();
        setU32(at + 4, props_);
        setU32(at + 8, static_cast<std::uint32_t>(out.size() - propStart_));
        hasChildren_.push_back(false);
    }
    void end() {
        const std::size_t at = stack_.back();
        stack_.pop_back();
        const bool children = hasChildren_.back();
        hasChildren_.pop_back();
        if (children) out.append(13, '\0');
        setU32(at, static_cast<std::uint32_t>(out.size()));
        if (!hasChildren_.empty()) hasChildren_.back() = true;
    }
    // A leaf node with the properties written by `fill`.
    void leaf(const std::string& name, const std::function<void()>& fill) {
        begin(name);
        fill();
        endProps();
        end();
    }
    // A node whose children are written by `fill`.
    void group(const std::string& name, const std::function<void()>& props, const std::function<void()>& children) {
        begin(name);
        if (props) props();
        endProps();
        children();
        hasChildren_.back() = true;
        end();
    }
    // Properties70 entry
    void p(const std::string& name, const std::string& type, const std::string& label, const std::string& flags,
           const std::vector<double>& values = {}, bool integer = false) {
        leaf("P", [&] {
            str(name);
            str(type);
            str(label);
            str(flags);
            for (double v : values) {
                if (integer) i32(static_cast<std::int32_t>(v));
                else f64(v);
            }
        });
    }

private:
    std::vector<std::size_t> stack_;
    std::vector<bool> hasChildren_;
    std::size_t propStart_ = 0;
    std::uint32_t props_ = 0;

    void u32(std::uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); }
    void setU32(std::size_t at, std::uint32_t v) { std::memcpy(&out[at], &v, 4); }
    void put(char type, const void* v, std::size_t n) {
        out += type;
        out.append(static_cast<const char*>(v), n);
        ++props_;
    }
    void array(char type, const void* data, std::size_t count, std::size_t elem) {
        const std::string rawBytes(static_cast<const char*>(data), count * elem);
        const std::string z = count > 16 ? raster::deflateZlib(rawBytes) : std::string{};
        const bool zipped = !z.empty() && z.size() < rawBytes.size();
        out += type;
        u32(static_cast<std::uint32_t>(count));
        u32(zipped ? 1u : 0u);
        u32(static_cast<std::uint32_t>(zipped ? z.size() : rawBytes.size()));
        out += zipped ? z : rawBytes;
        ++props_;
    }
};

}  // namespace

Mesh readFbx(const std::string& bytes) {
    Node root;
    if (bytes.size() >= 23 && bytes.compare(0, 20, kBinaryMagic) == 0) {
        root = BinaryReader(bytes).read();
    } else {
        root = AsciiReader(bytes).read();
    }
    return sceneToMesh(root);
}

std::string writeFbx(const Mesh& input) {
    if (input.empty()) throw std::runtime_error("FBX: the mesh is empty");
    Writer w;
    w.out = std::string(kBinaryMagic, 20) + std::string("\0\x1a\0", 3);
    const std::uint32_t version = 7400;
    w.out.append(reinterpret_cast<const char*>(&version), 4);

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    w.group("FBXHeaderExtension", nullptr, [&] {
        w.leaf("FBXHeaderVersion", [&] { w.i32(1003); });
        w.leaf("FBXVersion", [&] { w.i32(7400); });
        w.group("CreationTimeStamp", nullptr, [&] {
            w.leaf("Version", [&] { w.i32(1000); });
            w.leaf("Year", [&] { w.i32(tm.tm_year + 1900); });
            w.leaf("Month", [&] { w.i32(tm.tm_mon + 1); });
            w.leaf("Day", [&] { w.i32(tm.tm_mday); });
            w.leaf("Hour", [&] { w.i32(tm.tm_hour); });
            w.leaf("Minute", [&] { w.i32(tm.tm_min); });
            w.leaf("Second", [&] { w.i32(tm.tm_sec); });
            w.leaf("Millisecond", [&] { w.i32(0); });
        });
        w.leaf("Creator", [&] { w.str("wetwaresuite"); });
    });
    w.leaf("Creator", [&] { w.str("wetwaresuite"); });
    w.group("GlobalSettings", nullptr, [&] {
        w.leaf("Version", [&] { w.i32(1000); });
        w.group("Properties70", nullptr, [&] {
            w.p("UpAxis", "int", "Integer", "", {1}, true);
            w.p("UpAxisSign", "int", "Integer", "", {1}, true);
            w.p("FrontAxis", "int", "Integer", "", {2}, true);
            w.p("FrontAxisSign", "int", "Integer", "", {1}, true);
            w.p("CoordAxis", "int", "Integer", "", {0}, true);
            w.p("CoordAxisSign", "int", "Integer", "", {1}, true);
            w.p("OriginalUpAxis", "int", "Integer", "", {2}, true);
            w.p("OriginalUpAxisSign", "int", "Integer", "", {1}, true);
            w.p("UnitScaleFactor", "double", "Number", "", {0.1});
            w.p("OriginalUnitScaleFactor", "double", "Number", "", {0.1});
        });
    });
    w.group("Documents", nullptr, [&] {
        w.leaf("Count", [&] { w.i32(1); });
        w.group("Document", [&] { w.i64(1000000); w.str(""); w.str("Scene"); }, [&] {
            w.leaf("RootNode", [&] { w.i64(0); });
        });
    });
    w.group("References", nullptr, [] {});
    w.group("Definitions", nullptr, [&] {
        w.leaf("Version", [&] { w.i32(100); });
        w.leaf("Count", [&] { w.i32(3); });
        w.group("ObjectType", [&] { w.str("GlobalSettings"); }, [&] { w.leaf("Count", [&] { w.i32(1); }); });
        w.group("ObjectType", [&] { w.str("Model"); }, [&] { w.leaf("Count", [&] { w.i32(1); }); });
        w.group("ObjectType", [&] { w.str("Geometry"); }, [&] { w.leaf("Count", [&] { w.i32(1); }); });
    });

    // Z-up -> Y-up, one polygon per triangle, normals per corner
    const Mat4 toYup = Mat4::rotateX(-90);
    std::vector<double> verts;
    verts.reserve(input.positions.size() * 3);
    for (const auto& p : input.positions) {
        const Vec3 q = toYup.point(p);
        verts.insert(verts.end(), {q.x, q.y, q.z});
    }
    std::vector<std::int32_t> polys;
    polys.reserve(input.indices.size());
    for (std::size_t i = 0; i < input.indices.size(); i += 3) {
        polys.push_back(static_cast<std::int32_t>(input.indices[i]));
        polys.push_back(static_cast<std::int32_t>(input.indices[i + 1]));
        polys.push_back(~static_cast<std::int32_t>(input.indices[i + 2]));
    }
    std::vector<double> normals;
    normals.reserve(input.indices.size() * 3);
    for (std::size_t i = 0; i < input.indices.size(); ++i) {
        Vec3 n;
        if (input.hasNormals()) n = input.normals[input.indices[i]];
        else n = normalize(input.faceNormal(i / 3));
        n = toYup.direction(n);
        normals.insert(normals.end(), {n.x, n.y, n.z});
    }
    const std::int64_t geomId = 2000000, modelId = 3000000;
    w.group("Objects", nullptr, [&] {
        w.group("Geometry", [&] { w.i64(geomId); w.str(std::string("Mesh\0\x01Geometry", 14)); w.str("Mesh"); }, [&] {
            w.leaf("Vertices", [&] { w.doubles(verts); });
            w.leaf("PolygonVertexIndex", [&] { w.ints(polys); });
            w.leaf("GeometryVersion", [&] { w.i32(124); });
            w.group("LayerElementNormal", [&] { w.i32(0); }, [&] {
                w.leaf("Version", [&] { w.i32(101); });
                w.leaf("Name", [&] { w.str(""); });
                w.leaf("MappingInformationType", [&] { w.str("ByPolygonVertex"); });
                w.leaf("ReferenceInformationType", [&] { w.str("Direct"); });
                w.leaf("Normals", [&] { w.doubles(normals); });
            });
            if (input.hasUvs()) {
                std::vector<double> uv;
                uv.reserve(input.uvs.size() * 2);
                for (const auto& t : input.uvs) uv.insert(uv.end(), {t.x, t.y});
                std::vector<std::int32_t> uvIndex(input.indices.begin(), input.indices.end());
                w.group("LayerElementUV", [&] { w.i32(0); }, [&] {
                    w.leaf("Version", [&] { w.i32(101); });
                    w.leaf("Name", [&] { w.str("UVMap"); });
                    w.leaf("MappingInformationType", [&] { w.str("ByPolygonVertex"); });
                    w.leaf("ReferenceInformationType", [&] { w.str("IndexToDirect"); });
                    w.leaf("UV", [&] { w.doubles(uv); });
                    w.leaf("UVIndex", [&] { w.ints(uvIndex); });
                });
            }
            w.group("Layer", [&] { w.i32(0); }, [&] {
                w.leaf("Version", [&] { w.i32(100); });
                w.group("LayerElement", nullptr, [&] {
                    w.leaf("Type", [&] { w.str("LayerElementNormal"); });
                    w.leaf("TypedIndex", [&] { w.i32(0); });
                });
                if (input.hasUvs()) {
                    w.group("LayerElement", nullptr, [&] {
                        w.leaf("Type", [&] { w.str("LayerElementUV"); });
                        w.leaf("TypedIndex", [&] { w.i32(0); });
                    });
                }
            });
        });
        w.group("Model", [&] { w.i64(modelId); w.str(std::string("Mesh\0\x01Model", 11)); w.str("Mesh"); }, [&] {
            w.leaf("Version", [&] { w.i32(232); });
            w.group("Properties70", nullptr, [&] {
                w.p("Lcl Translation", "Lcl Translation", "", "A", {0, 0, 0});
                w.p("Lcl Rotation", "Lcl Rotation", "", "A", {0, 0, 0});
                w.p("Lcl Scaling", "Lcl Scaling", "", "A", {1, 1, 1});
            });
            w.leaf("Shading", [&] { w.str("Y"); });
            w.leaf("Culling", [&] { w.str("CullingOff"); });
        });
    });
    w.group("Connections", nullptr, [&] {
        w.leaf("C", [&] { w.str("OO"); w.i64(modelId); w.i64(0); });
        w.leaf("C", [&] { w.str("OO"); w.i64(geomId); w.i64(modelId); });
    });
    w.out.append(13, '\0');  // end of the top level

    // footer
    static const unsigned char id[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66, 0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    static const unsigned char magic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e, 0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    w.out.append(reinterpret_cast<const char*>(id), 16);
    w.out.append(4, '\0');
    w.out.append((16 - w.out.size() % 16) % 16, '\0');
    w.out.append(reinterpret_cast<const char*>(&version), 4);
    w.out.append(120, '\0');
    w.out.append(reinterpret_cast<const char*>(magic), 16);
    return w.out;
}

}  // namespace wetwaresuite::mesh
