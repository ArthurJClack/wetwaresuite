// glTF 2.0: .glb written with one mesh (positions, normals, optional UVs,
// uint32 indices); .glb and .gltf read with every triangle primitive in the
// default scene, node transforms applied.
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

#include "achclwss/mesh/io.hpp"
#include "internal.hpp"

namespace achclwss::mesh {

using json = nlohmann::json;

namespace {

const Mat4 kZupToYup = Mat4::rotateX(-90);  // (x, y, z) -> (x, z, -y)
const Mat4 kYupToZup = Mat4::rotateX(90);

constexpr std::uint32_t kGlbMagic = 0x46546C67, kJsonChunk = 0x4E4F534A, kBinChunk = 0x004E4942;

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const std::uint32_t v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                                static_cast<unsigned char>(in[i + 2]);
        out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63]; out += kB64[(v >> 6) & 63]; out += kB64[v & 63];
    }
    if (i < in.size()) {
        std::uint32_t v = static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16;
        if (i + 1 < in.size()) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    int map[256];
    for (int& m : map) m = -1;
    for (int i = 0; i < 64; ++i) map[static_cast<unsigned char>(kB64[i])] = i;
    map[static_cast<unsigned char>('-')] = 62;  // url-safe variants
    map[static_cast<unsigned char>('_')] = 63;
    std::string out;
    out.reserve(in.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bitsHeld = 0;
    for (unsigned char c : in) {
        const int v = map[c];
        if (v < 0) continue;  // padding, whitespace
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bitsHeld += 6;
        if (bitsHeld >= 8) {
            bitsHeld -= 8;
            out += static_cast<char>((acc >> bitsHeld) & 0xff);
        }
    }
    return out;
}

void pad(std::string& s, char with) {
    while (s.size() % 4) s += with;
}

struct Packed {
    json doc;
    std::string bin;
};

Packed pack(const Mesh& input) {
    Mesh m = input;
    if (!m.hasNormals()) computeNormals(m, 40.0);
    transform(m, kZupToYup);

    Packed p;
    std::string& bin = p.bin;
    json accessors = json::array(), views = json::array();
    auto addView = [&](std::size_t offset, std::size_t length, int target) {
        views.push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", length}, {"target", target}});
        return views.size() - 1;
    };

    const Bounds b = bounds(m);
    std::size_t at = bin.size();
    for (const auto& v : m.positions) { detail::putF32(bin, v.x); detail::putF32(bin, v.y); detail::putF32(bin, v.z); }
    // min/max must match the stored floats.
    auto f = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    accessors.push_back({{"bufferView", addView(at, bin.size() - at, 34962)}, {"componentType", 5126},
                         {"count", m.positions.size()}, {"type", "VEC3"},
                         {"min", {f(b.min.x), f(b.min.y), f(b.min.z)}}, {"max", {f(b.max.x), f(b.max.y), f(b.max.z)}}});
    json attributes = {{"POSITION", 0}};

    at = bin.size();
    for (const auto& v : m.normals) { detail::putF32(bin, v.x); detail::putF32(bin, v.y); detail::putF32(bin, v.z); }
    accessors.push_back({{"bufferView", addView(at, bin.size() - at, 34962)}, {"componentType", 5126},
                         {"count", m.normals.size()}, {"type", "VEC3"}});
    attributes["NORMAL"] = accessors.size() - 1;

    if (m.hasUvs()) {
        at = bin.size();
        for (const auto& v : m.uvs) { detail::putF32(bin, v.x); detail::putF32(bin, 1.0 - v.y); }  // glTF UVs start top-left
        accessors.push_back({{"bufferView", addView(at, bin.size() - at, 34962)}, {"componentType", 5126},
                             {"count", m.uvs.size()}, {"type", "VEC2"}});
        attributes["TEXCOORD_0"] = accessors.size() - 1;
    }

    at = bin.size();
    for (auto i : m.indices) detail::putU32(bin, i);
    accessors.push_back({{"bufferView", addView(at, bin.size() - at, 34963)}, {"componentType", 5125},
                         {"count", m.indices.size()}, {"type", "SCALAR"}});
    const std::size_t indexAccessor = accessors.size() - 1;
    pad(bin, '\0');

    p.doc = {{"asset", {{"version", "2.0"}, {"generator", "achclwss"}}},
             {"scene", 0},
             {"scenes", {{{"nodes", {0}}}}},
             {"nodes", {{{"mesh", 0}, {"name", "mesh"}}}},
             {"meshes", {{{"name", "mesh"}, {"primitives", {{{"attributes", attributes}, {"indices", indexAccessor}, {"mode", 4}}}}}}},
             {"accessors", accessors},
             {"bufferViews", views},
             {"buffers", {{{"byteLength", bin.size()}}}}};
    return p;
}

// ---- reading ----

std::size_t components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT4") return 16;
    throw std::runtime_error("glTF accessor type " + type + " isn't supported here");
}

std::size_t componentSize(int t) {
    switch (t) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: throw std::runtime_error("glTF component type " + std::to_string(t) + " isn't supported");
    }
}

class Reader {
public:
    Reader(const json& doc, std::vector<std::string> buffers) : doc_(doc), buffers_(std::move(buffers)) {}

    // Accessor values as doubles, `components` per element.
    std::vector<double> accessor(std::size_t index, std::size_t& comps) const {
        const json& a = doc_.at("accessors").at(index);
        comps = components(a.at("type").get<std::string>());
        const std::size_t count = a.at("count").get<std::size_t>();
        const int type = a.at("componentType").get<int>();
        const bool normalized = a.value("normalized", false);
        std::vector<double> out(count * comps, 0.0);
        if (!a.contains("bufferView")) return out;  // all zeros per spec
        if (a.contains("sparse")) throw std::runtime_error("sparse glTF accessors aren't supported");
        const json& view = doc_.at("bufferViews").at(a.at("bufferView").get<std::size_t>());
        const std::string& buf = buffers_.at(view.at("buffer").get<std::size_t>());
        const std::size_t cs = componentSize(type);
        std::size_t stride = view.value("byteStride", std::size_t{0});
        if (stride == 0) stride = cs * comps;
        const std::size_t base = view.value("byteOffset", std::size_t{0}) + a.value("byteOffset", std::size_t{0});
        if (count && base + stride * (count - 1) + cs * comps > buf.size()) throw std::runtime_error("glTF accessor runs past its buffer");
        const auto* u = reinterpret_cast<const unsigned char*>(buf.data());
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t c = 0; c < comps; ++c) {
                const unsigned char* p = u + base + stride * i + cs * c;
                double v = 0;
                switch (type) {
                    case 5120: v = static_cast<std::int8_t>(p[0]); if (normalized) v = std::fmax(v / 127.0, -1.0); break;
                    case 5121: v = p[0]; if (normalized) v /= 255.0; break;
                    case 5122: v = static_cast<std::int16_t>(detail::getU16(p)); if (normalized) v = std::fmax(v / 32767.0, -1.0); break;
                    case 5123: v = detail::getU16(p); if (normalized) v /= 65535.0; break;
                    case 5125: v = detail::getU32(p); break;
                    case 5126: v = detail::getF32(p); break;
                    default: break;
                }
                out[i * comps + c] = v;
            }
        }
        return out;
    }

    Mesh primitive(const json& prim) const {
        Mesh m;
        if (prim.value("mode", 4) != 4) return m;  // points, lines and strips are skipped
        const json& attrs = prim.at("attributes");
        if (!attrs.contains("POSITION")) return m;
        std::size_t comps = 0;
        auto pos = accessor(attrs.at("POSITION").get<std::size_t>(), comps);
        if (comps != 3) throw std::runtime_error("glTF POSITION must be VEC3");
        for (std::size_t i = 0; i + 2 < pos.size(); i += 3) m.positions.push_back({pos[i], pos[i + 1], pos[i + 2]});
        if (attrs.contains("NORMAL")) {
            auto n = accessor(attrs.at("NORMAL").get<std::size_t>(), comps);
            if (comps == 3 && n.size() == pos.size()) {
                for (std::size_t i = 0; i + 2 < n.size(); i += 3) m.normals.push_back({n[i], n[i + 1], n[i + 2]});
            }
        }
        if (attrs.contains("TEXCOORD_0")) {
            auto t = accessor(attrs.at("TEXCOORD_0").get<std::size_t>(), comps);
            if (comps == 2 && t.size() / 2 == m.positions.size()) {
                for (std::size_t i = 0; i + 1 < t.size(); i += 2) m.uvs.push_back({t[i], 1.0 - t[i + 1]});
            }
        }
        if (prim.contains("indices")) {
            auto idx = accessor(prim.at("indices").get<std::size_t>(), comps);
            for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                m.addTriangle(static_cast<std::uint32_t>(idx[i]), static_cast<std::uint32_t>(idx[i + 1]), static_cast<std::uint32_t>(idx[i + 2]));
            }
        } else {
            for (std::uint32_t i = 0; i + 2 < m.positions.size(); i += 3) m.addTriangle(i, i + 1, i + 2);
        }
        for (auto i : m.indices) {
            if (i >= m.positions.size()) throw std::runtime_error("glTF indices refer to a vertex that doesn't exist");
        }
        return m;
    }

    Mesh meshAt(std::size_t index) const {
        Mesh out;
        for (const auto& prim : doc_.at("meshes").at(index).at("primitives")) append(out, primitive(prim));
        return out;
    }

    static Mat4 nodeMatrix(const json& node) {
        Mat4 m;
        if (node.contains("matrix")) {
            const auto& a = node.at("matrix");  // column-major
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 4; ++r) m.at(r, c) = a.at(static_cast<std::size_t>(c * 4 + r)).get<double>();
            }
            return m;
        }
        Mat4 t, r, s;
        if (node.contains("translation")) {
            const auto& v = node.at("translation");
            t = Mat4::translate({v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()});
        }
        if (node.contains("rotation")) {
            const auto& q = node.at("rotation");
            const double x = q.at(0).get<double>(), y = q.at(1).get<double>(), z = q.at(2).get<double>(), w = q.at(3).get<double>();
            r.at(0, 0) = 1 - 2 * (y * y + z * z); r.at(0, 1) = 2 * (x * y - z * w);     r.at(0, 2) = 2 * (x * z + y * w);
            r.at(1, 0) = 2 * (x * y + z * w);     r.at(1, 1) = 1 - 2 * (x * x + z * z); r.at(1, 2) = 2 * (y * z - x * w);
            r.at(2, 0) = 2 * (x * z - y * w);     r.at(2, 1) = 2 * (y * z + x * w);     r.at(2, 2) = 1 - 2 * (x * x + y * y);
        }
        if (node.contains("scale")) {
            const auto& v = node.at("scale");
            s = Mat4::scale({v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()});
        }
        return t * r * s;
    }

    Mesh scene() const {
        Mesh out;
        const bool hasNodes = doc_.contains("nodes") && doc_.contains("scenes") && !doc_.at("scenes").empty();
        if (!hasNodes) {
            if (doc_.contains("meshes")) {
                for (std::size_t i = 0; i < doc_.at("meshes").size(); ++i) append(out, meshAt(i));
            }
            return out;
        }
        const std::size_t sceneIndex = doc_.value("scene", std::size_t{0});
        const json& sc = doc_.at("scenes").at(sceneIndex);
        std::vector<std::pair<std::size_t, Mat4>> stack;
        if (sc.contains("nodes")) {
            for (const auto& n : sc.at("nodes")) stack.emplace_back(n.get<std::size_t>(), Mat4{});
        }
        std::size_t guard = 0;
        while (!stack.empty()) {
            auto [idx, parent] = stack.back();
            stack.pop_back();
            if (++guard > 1000000) throw std::runtime_error("glTF node hierarchy loops");
            const json& node = doc_.at("nodes").at(idx);
            const Mat4 world = parent * nodeMatrix(node);
            if (node.contains("mesh")) {
                Mesh m = meshAt(node.at("mesh").get<std::size_t>());
                transform(m, world);
                append(out, m);
            }
            if (node.contains("children")) {
                for (const auto& c : node.at("children")) stack.emplace_back(c.get<std::size_t>(), world);
            }
        }
        return out;
    }

private:
    const json& doc_;
    std::vector<std::string> buffers_;
};

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("glTF buffer " + p.string() + " not found");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string uriDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

std::string writeGlb(const Mesh& mesh) {
    Packed p = pack(mesh);
    std::string js = p.doc.dump();
    pad(js, ' ');
    std::string out;
    detail::putU32(out, kGlbMagic);
    detail::putU32(out, 2);
    detail::putU32(out, static_cast<std::uint32_t>(12 + 8 + js.size() + 8 + p.bin.size()));
    detail::putU32(out, static_cast<std::uint32_t>(js.size()));
    detail::putU32(out, kJsonChunk);
    out += js;
    detail::putU32(out, static_cast<std::uint32_t>(p.bin.size()));
    detail::putU32(out, kBinChunk);
    out += p.bin;
    return out;
}

std::string writeGltf(const Mesh& mesh) {
    Packed p = pack(mesh);
    p.doc["buffers"][0]["uri"] = "data:application/octet-stream;base64," + base64Encode(p.bin);
    return p.doc.dump(1);
}

Mesh readGltf(const std::string& bytes, const std::filesystem::path& baseDir) {
    json doc;
    std::string glbBin;
    bool haveGlbBin = false;
    const auto* u = reinterpret_cast<const unsigned char*>(bytes.data());
    if (bytes.size() >= 12 && detail::getU32(u) == kGlbMagic) {
        if (detail::getU32(u + 4) != 2) throw std::runtime_error("only glTF 2.0 .glb files are supported");
        std::size_t at = 12;
        bool haveJson = false;
        while (at + 8 <= bytes.size()) {
            const std::size_t len = detail::getU32(u + at), type = detail::getU32(u + at + 4);
            if (at + 8 + len > bytes.size()) throw std::runtime_error(".glb chunk is truncated");
            if (type == kJsonChunk && !haveJson) {
                doc = json::parse(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + len));
                haveJson = true;
            } else if (type == kBinChunk && !haveGlbBin) {
                glbBin = bytes.substr(at + 8, len);
                haveGlbBin = true;
            }
            at += 8 + len;
        }
        if (!haveJson) throw std::runtime_error(".glb has no JSON chunk");
    } else {
        try {
            doc = json::parse(bytes);
        } catch (const json::exception& e) {
            throw std::runtime_error(std::string("not a glTF file: ") + e.what());
        }
    }
    try {
        std::vector<std::string> buffers;
        if (doc.contains("buffers")) {
            for (std::size_t i = 0; i < doc.at("buffers").size(); ++i) {
                const json& b = doc.at("buffers").at(i);
                if (!b.contains("uri")) {
                    if (i != 0 || !haveGlbBin) throw std::runtime_error("glTF buffer " + std::to_string(i) + " has no data");
                    buffers.push_back(glbBin);
                    continue;
                }
                const std::string uri = b.at("uri").get<std::string>();
                if (uri.rfind("data:", 0) == 0) {
                    const std::size_t comma = uri.find(',');
                    if (comma == std::string::npos || uri.find(";base64") > comma) throw std::runtime_error("glTF data URIs must be base64");
                    buffers.push_back(base64Decode(uri.substr(comma + 1)));
                } else {
                    if (baseDir.empty() && !std::filesystem::path(uriDecode(uri)).is_absolute()) {
                        throw std::runtime_error("glTF refers to " + uri + "; open it from a file so I can find that buffer");
                    }
                    buffers.push_back(readFile(baseDir / std::filesystem::path(uriDecode(uri))));
                }
            }
        }
        Reader r(doc, std::move(buffers));
        Mesh m = r.scene();
        transform(m, kYupToZup);
        cleanup(m);
        return m;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("malformed glTF: ") + e.what());
    }
}

}  // namespace achclwss::mesh
