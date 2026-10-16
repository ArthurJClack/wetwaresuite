// Map files: Tiled TMX (XML) and TMJ (JSON), a plain JSON format, LDtk-lite
// JSON, CSV per layer and flattened images.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

#include "wetwaresuite/levels/levels.hpp"
#include "wetwaresuite/raster/codec.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// Defined (and exported) by stb_image_write inside libs/raster.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace wetwaresuite::levels {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace {

// ---- small helpers ----------------------------------------------------------------

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeFile(const fs::path& path, const std::string& data) {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out << data;
    if (!out) throw std::runtime_error("can't write " + path.string());
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string num(double v) {
    if (std::abs(v - std::round(v)) < 1e-9) return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

// Path of a tileset image as a file next to `dir` should refer to it.
std::string relativeTo(const std::string& image, const fs::path& dir) {
    if (image.empty()) return {};
    std::error_code ec;
    fs::path base = fs::absolute(dir.empty() ? fs::path(".") : dir, ec);
    fs::path rel = fs::path(image).lexically_relative(base.lexically_normal());
    if (rel.empty()) return fs::path(image).generic_string();
    return rel.generic_string();
}

std::string resolveFrom(const fs::path& dir, const std::string& source) {
    if (source.empty()) return {};
    fs::path p(source);
    if (p.is_relative()) p = dir / p;
    std::error_code ec;
    auto abs = fs::absolute(p, ec);
    return (ec ? p : abs).lexically_normal().string();
}

// Loads a tileset's pixels when the image exists; otherwise tiles render as colours.
void attachImage(Tileset& t) {
    if (t.image.empty()) return;
    try {
        auto img = std::make_shared<Image>(raster::load(t.image));
        if (t.imageWidth <= 0) t.imageWidth = img->width();
        if (t.imageHeight <= 0) t.imageHeight = img->height();
        t.pixels = std::move(img);
    } catch (const std::exception&) {
        t.pixels.reset();
    }
    if (t.columns <= 0 && t.imageWidth > 0) t.columns = std::max(1, (t.imageWidth - 2 * t.margin + t.spacing) / (t.tileWidth + t.spacing));
    if (t.tileCount <= 0 && t.imageHeight > 0) {
        const int rows = std::max(1, (t.imageHeight - 2 * t.margin + t.spacing) / (t.tileHeight + t.spacing));
        t.tileCount = t.columns * rows;
    }
}

// ---- base64 and zlib ---------------------------------------------------------------

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16 | static_cast<unsigned char>(in[i + 1]) << 8 | static_cast<unsigned char>(in[i + 2]);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += kB64[v >> 6 & 63];
        out += kB64[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += i + 1 < in.size() ? kB64[v >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        const char* p = std::strchr(kB64, c);
        if (c == '=' || !p || c == '\0') continue;
        v = v << 6 | static_cast<unsigned>(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>(v >> bits & 0xFF);
        }
    }
    return out;
}

std::string zlibCompress(const std::string& data) {
    int len = 0;
    unsigned char* z = stbi_zlib_compress(reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())), static_cast<int>(data.size()), &len, 8);
    if (!z) throw std::runtime_error("zlib compression failed");
    std::string out(reinterpret_cast<char*>(z), static_cast<std::size_t>(len));
    std::free(z);
    return out;
}

std::string inflate(const std::string& data, const std::string& compression) {
    if (compression.empty()) return data;
    int len = 0;
    char* raw = nullptr;
    if (compression == "zlib") {
        raw = stbi_zlib_decode_malloc_guesssize_headerflag(data.data(), static_cast<int>(data.size()),
                                                         static_cast<int>(data.size() * 4 + 256), &len, 1);
    } else if (compression == "gzip") {
        // header: magic, method, flags, mtime, xfl, os; then optional fields
        if (data.size() < 18 || static_cast<unsigned char>(data[0]) != 0x1f || static_cast<unsigned char>(data[1]) != 0x8b) {
            throw std::runtime_error("bad gzip data");
        }
        const unsigned flags = static_cast<unsigned char>(data[3]);
        std::size_t at = 10;
        if (flags & 4) at += 2 + (static_cast<unsigned char>(data[at]) | static_cast<unsigned char>(data[at + 1]) << 8);
        if (flags & 8) at = data.find('\0', at) + 1;
        if (flags & 16) at = data.find('\0', at) + 1;
        if (flags & 2) at += 2;
        if (at == 0 || at >= data.size()) throw std::runtime_error("bad gzip data");
        raw = stbi_zlib_decode_noheader_malloc(data.data() + at, static_cast<int>(data.size() - at), &len);
    } else {
        throw std::runtime_error("tile data compression '" + compression + "' isn't supported (use csv, base64, zlib or gzip)");
    }
    if (!raw) throw std::runtime_error("corrupt " + compression + " tile data");
    std::string out(raw, static_cast<std::size_t>(len));
    std::free(raw);
    return out;
}

std::string packGids(const std::vector<std::uint32_t>& cells) {
    std::string out(cells.size() * 4, '\0');
    for (std::size_t i = 0; i < cells.size(); ++i) {
        for (int b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<char>(cells[i] >> (8 * b) & 0xFF);
    }
    return out;
}

std::vector<std::uint32_t> unpackGids(const std::string& bytes, std::size_t count) {
    if (bytes.size() < count * 4) throw std::runtime_error("tile data is shorter than the layer");
    std::vector<std::uint32_t> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t v = 0;
        for (int b = 0; b < 4; ++b) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i * 4 + b])) << (8 * b);
        out[i] = v;
    }
    return out;
}

std::vector<std::uint32_t> parseNumbers(const std::string& text, std::size_t count) {
    std::vector<std::uint32_t> out;
    out.reserve(count);
    const char* p = text.c_str();
    while (*p) {
        while (*p && !(std::isdigit(static_cast<unsigned char>(*p)) || *p == '-')) ++p;
        if (!*p) break;
        char* end = nullptr;
        const long long v = std::strtoll(p, &end, 10);
        if (end == p) {
            ++p;
            continue;
        }
        out.push_back(v < 0 ? 0u : static_cast<std::uint32_t>(v));
        p = end;
    }
    out.resize(count, 0);
    return out;
}

std::string encodeCells(const std::vector<std::uint32_t>& cells, const std::string& encoding) {
    if (encoding == "zlib") return base64Encode(zlibCompress(packGids(cells)));
    return base64Encode(packGids(cells));
}

// ---- a minimal XML reader (enough for TMX/TSX) ------------------------------------

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;

    const std::string* attr(const std::string& key) const {
        for (const auto& [k, v] : attrs) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        const std::string* v = attr(key);
        return v ? *v : fallback;
    }
    int getInt(const std::string& key, int fallback = 0) const {
        const std::string* v = attr(key);
        if (!v || v->empty()) return fallback;
        return static_cast<int>(std::strtol(v->c_str(), nullptr, 10));
    }
    double getNum(const std::string& key, double fallback = 0) const {
        const std::string* v = attr(key);
        if (!v || v->empty()) return fallback;
        return std::strtod(v->c_str(), nullptr);
    }
    const XmlNode* child(const std::string& n) const {
        for (const auto& c : children) {
            if (c.name == n) return &c;
        }
        return nullptr;
    }
};

class XmlParser {
public:
    // XML treats \r\n and a lone \r as \n (files saved on Windows have them)
    explicit XmlParser(const std::string& s) {
        s_.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\r') {
                s_ += s[i];
            } else if (i + 1 >= s.size() || s[i + 1] != '\n') {
                s_ += '\n';
            }
        }
    }

    XmlNode parse() {
        skipMisc();
        if (peek() != '<') fail("expected an element");
        XmlNode root = element();
        return root;
    }

private:
    std::string s_;
    std::size_t i_ = 0;

    [[noreturn]] void fail(const std::string& what) const { throw std::runtime_error("XML error at byte " + std::to_string(i_) + ": " + what); }
    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    bool starts(const char* t) const { return s_.compare(i_, std::strlen(t), t) == 0; }
    void skipSpace() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    void skipTo(const char* end) {
        auto at = s_.find(end, i_);
        if (at == std::string::npos) fail(std::string("missing ") + end);
        i_ = at + std::strlen(end);
    }
    void skipMisc() {
        while (true) {
            skipSpace();
            if (starts("<?")) skipTo("?>");
            else if (starts("<!--")) skipTo("-->");
            else if (starts("<!")) skipTo(">");
            else if (starts("\xEF\xBB\xBF")) i_ += 3;
            else return;
        }
    }
    std::string nameToken() {
        std::size_t b = i_;
        while (i_ < s_.size() && s_[i_] != '\0' && (std::isalnum(static_cast<unsigned char>(s_[i_])) || std::strchr("_:-.", s_[i_]))) ++i_;
        if (b == i_) fail("expected a name");
        return s_.substr(b, i_ - b);
    }
    static std::string decode(const std::string& t) {
        std::string out;
        for (std::size_t k = 0; k < t.size(); ++k) {
            if (t[k] != '&') {
                out += t[k];
                continue;
            }
            auto semi = t.find(';', k);
            if (semi == std::string::npos) {
                out += t[k];
                continue;
            }
            const std::string ent = t.substr(k + 1, semi - k - 1);
            if (ent == "amp") out += '&';
            else if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (!ent.empty() && ent[0] == '#') {
                unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16)
                                                                                     : std::strtoul(ent.c_str() + 1, nullptr, 10);
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | cp >> 6);
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | cp >> 12);
                    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | cp >> 18);
                    out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
                    out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
            } else {
                out += '&' + ent + ';';
            }
            k = semi;
        }
        return out;
    }
    XmlNode element(int depth = 0) {
        if (depth > 64) fail("elements nested too deeply");
        ++i_;  // '<'
        XmlNode n;
        n.name = nameToken();
        while (true) {
            skipSpace();
            if (starts("/>")) {
                i_ += 2;
                return n;
            }
            if (peek() == '>') {
                ++i_;
                break;
            }
            std::string key = nameToken();
            skipSpace();
            if (peek() != '=') fail("expected = after " + key);
            ++i_;
            skipSpace();
            const char q = peek();
            if (q != '"' && q != '\'') fail("expected a quoted value for " + key);
            auto end = s_.find(q, i_ + 1);
            if (end == std::string::npos) fail("unterminated attribute " + key);
            n.attrs.emplace_back(key, decode(s_.substr(i_ + 1, end - i_ - 1)));
            i_ = end + 1;
        }
        while (true) {
            if (i_ >= s_.size()) fail("unclosed <" + n.name + ">");
            if (starts("</")) {
                i_ += 2;
                const std::string close = nameToken();
                if (close != n.name) fail("</" + close + "> closes <" + n.name + ">");
                skipSpace();
                if (peek() != '>') fail("expected >");
                ++i_;
                return n;
            }
            if (starts("<!--")) {
                skipTo("-->");
            } else if (starts("<![CDATA[")) {
                i_ += 9;
                auto end = s_.find("]]>", i_);
                if (end == std::string::npos) fail("unterminated CDATA");
                n.text += s_.substr(i_, end - i_);
                i_ = end + 3;
            } else if (starts("<?")) {
                skipTo("?>");
            } else if (peek() == '<') {
                n.children.push_back(element(depth + 1));
            } else {
                auto end = s_.find('<', i_);
                if (end == std::string::npos) end = s_.size();
                n.text += decode(s_.substr(i_, end - i_));
                i_ = end;
            }
        }
    }
};

std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "&#10;"; break;
            default: out += c;
        }
    }
    return out;
}

// ---- properties -------------------------------------------------------------------

void propsXml(std::ostringstream& o, const Properties& props, const std::string& indent) {
    if (props.empty()) return;
    o << indent << "<properties>\n";
    for (const auto& [k, v] : props) {
        o << indent << " <property name=\"" << esc(k) << "\"";
        if (v.type != "string") o << " type=\"" << esc(v.type) << "\"";
        o << " value=\"" << esc(v.value) << "\"/>\n";
    }
    o << indent << "</properties>\n";
}

Properties propsFromXml(const XmlNode& parent) {
    Properties out;
    const XmlNode* ps = parent.child("properties");
    if (!ps) return out;
    for (const auto& p : ps->children) {
        if (p.name != "property") continue;
        std::string type = p.get("type", "string");
        if (type == "class" || type == "object") type = "string";
        out[p.get("name")] = {type, p.attr("value") ? p.get("value") : p.text};
    }
    return out;
}

json propValue(const Property& p) {
    if (p.type == "bool") return p.value == "true";
    if (p.type == "int") return std::strtoll(p.value.c_str(), nullptr, 10);
    if (p.type == "float") return std::strtod(p.value.c_str(), nullptr);
    return p.value;
}

std::string jsonText(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) return num(v.get<double>());
    if (v.is_null()) return {};
    return v.dump();
}

Property propFromJson(const json& v) {
    if (v.is_boolean()) return {"bool", jsonText(v)};
    if (v.is_number_integer()) return {"int", jsonText(v)};
    if (v.is_number()) return {"float", jsonText(v)};
    return {"string", jsonText(v)};
}

json propsTiledJson(const Properties& props) {
    json a = json::array();
    for (const auto& [k, v] : props) a.push_back({{"name", k}, {"type", v.type}, {"value", propValue(v)}});
    return a;
}

Properties propsFromTiledJson(const json& j) {
    Properties out;
    if (!j.is_array()) return out;
    for (const auto& p : j) {
        if (!p.is_object()) continue;
        std::string type = p.value("type", std::string("string"));
        if (type == "class" || type == "object") type = "string";
        out[p.value("name", std::string())] = {type, jsonText(p.value("value", json()))};
    }
    return out;
}

json propsPlain(const Properties& props) {
    json o = json::object();
    for (const auto& [k, v] : props) o[k] = propValue(v);
    return o;
}

Properties propsFromPlain(const json& j) {
    Properties out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it) out[it.key()] = propFromJson(it.value());
    return out;
}

void finish(Map& m) {
    int maxLayer = 0, maxObject = 0;
    for (auto& l : m.layers) {
        if (l.id <= 0) l.id = ++maxLayer;
        maxLayer = std::max(maxLayer, l.id);
        if (l.kind == LayerKind::Tiles) l.cells.resize(static_cast<std::size_t>(m.width) * m.height, 0);
        for (auto& o : l.objects) {
            if (o.id <= 0) o.id = ++maxObject;
            maxObject = std::max(maxObject, o.id);
        }
    }
    m.nextLayerId = std::max(m.nextLayerId, maxLayer + 1);
    m.nextObjectId = std::max(m.nextObjectId, maxObject + 1);
    std::sort(m.tilesets.begin(), m.tilesets.end(), [](const Tileset& a, const Tileset& b) { return a.firstGid < b.firstGid; });
    for (auto& l : m.layers) {
        if (auto it = l.props.find("collision"); it != l.props.end() && it->second.value == "true") {
            l.collision = true;
            l.props.erase(it);
        }
    }
}

void checkSize(const Map& m) {
    if (m.width <= 0 || m.height <= 0 || m.tileWidth <= 0 || m.tileHeight <= 0) throw std::runtime_error("the map has no size");
    if (static_cast<long long>(m.width) * m.height > 16LL * 1024 * 1024) throw std::runtime_error("the map is too big (over 16M cells)");
}

// ---- TMX tilesets -----------------------------------------------------------------

Tileset tilesetFromXml(const XmlNode& n, const fs::path& dir) {
    Tileset t;
    t.name = n.get("name");
    t.tileWidth = n.getInt("tilewidth", 16);
    t.tileHeight = n.getInt("tileheight", 16);
    t.margin = n.getInt("margin");
    t.spacing = n.getInt("spacing");
    t.columns = n.getInt("columns");
    t.tileCount = n.getInt("tilecount");
    if (const XmlNode* img = n.child("image")) {
        t.image = resolveFrom(dir, img->get("source"));
        t.imageWidth = img->getInt("width");
        t.imageHeight = img->getInt("height");
    }
    for (const auto& c : n.children) {
        if (c.name != "tile") continue;
        Properties p = propsFromXml(c);
        if (!c.get("type").empty()) p["type"] = {"string", c.get("type")};
        if (!c.get("class").empty()) p["type"] = {"string", c.get("class")};
        if (!p.empty()) t.tileProps[c.getInt("id")] = std::move(p);
    }
    attachImage(t);
    return t;
}

Tileset tilesetFromTiledJson(const json& j, const fs::path& dir) {
    Tileset t;
    t.name = j.value("name", std::string());
    t.tileWidth = j.value("tilewidth", 16);
    t.tileHeight = j.value("tileheight", 16);
    t.margin = j.value("margin", 0);
    t.spacing = j.value("spacing", 0);
    t.columns = j.value("columns", 0);
    t.tileCount = j.value("tilecount", 0);
    t.image = resolveFrom(dir, j.value("image", std::string()));
    t.imageWidth = j.value("imagewidth", 0);
    t.imageHeight = j.value("imageheight", 0);
    if (j.contains("tiles") && j["tiles"].is_array()) {
        for (const auto& tile : j["tiles"]) {
            Properties p = propsFromTiledJson(tile.value("properties", json::array()));
            const std::string type = tile.value("type", tile.value("class", std::string()));
            if (!type.empty()) p["type"] = {"string", type};
            if (!p.empty()) t.tileProps[tile.value("id", 0)] = std::move(p);
        }
    }
    attachImage(t);
    return t;
}

Tileset externalTileset(const fs::path& file) {
    const std::string text = readFile(file);
    const std::string ext = lower(file.extension().string());
    if (ext == ".tsx" || (!text.empty() && text.find('<') < text.find('{'))) {
        XmlNode n = XmlParser(text).parse();
        if (n.name != "tileset") throw std::runtime_error(file.string() + " isn't a TSX tileset");
        return tilesetFromXml(n, file.parent_path());
    }
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error(file.string() + " isn't valid JSON");
    return tilesetFromTiledJson(j, file.parent_path());
}

void tmxLayers(Map& m, const XmlNode& parent, const fs::path& dir, bool parentVisible, double parentOpacity) {
    for (const auto& c : parent.children) {
        const bool visible = parentVisible && c.get("visible", "1") != "0";
        const double opacity = parentOpacity * c.getNum("opacity", 1.0);
        if (c.name == "group") {
            tmxLayers(m, c, dir, visible, opacity);
        } else if (c.name == "layer") {
            Layer l;
            l.id = c.getInt("id");
            l.name = c.get("name", "Layer " + std::to_string(m.layers.size() + 1));
            l.visible = visible;
            l.opacity = opacity;
            l.props = propsFromXml(c);
            const std::size_t count = static_cast<std::size_t>(m.width) * m.height;
            const XmlNode* d = c.child("data");
            if (!d) throw std::runtime_error("layer " + l.name + " has no <data>");
            if (d->child("chunk")) throw std::runtime_error("infinite maps (chunked layers) aren't supported; turn off Map > Infinite in Tiled");
            const std::string enc = d->get("encoding");
            if (enc == "csv") {
                l.cells = parseNumbers(d->text, count);
            } else if (enc == "base64") {
                l.cells = unpackGids(inflate(base64Decode(d->text), d->get("compression")), count);
            } else if (enc.empty()) {
                for (const auto& t : d->children) {
                    if (t.name == "tile") l.cells.push_back(static_cast<std::uint32_t>(std::strtoul(t.get("gid", "0").c_str(), nullptr, 10)));
                }
                l.cells.resize(count, 0);
            } else {
                throw std::runtime_error("unknown tile encoding '" + enc + "'");
            }
            m.layers.push_back(std::move(l));
        } else if (c.name == "objectgroup") {
            Layer l;
            l.id = c.getInt("id");
            l.kind = LayerKind::Objects;
            l.name = c.get("name", "Objects " + std::to_string(m.layers.size() + 1));
            l.visible = visible;
            l.opacity = opacity;
            l.props = propsFromXml(c);
            for (const auto& o : c.children) {
                if (o.name != "object") continue;
                MapObject ob;
                ob.id = o.getInt("id");
                ob.name = o.get("name");
                ob.type = o.get("type", o.get("class"));
                ob.x = o.getNum("x");
                ob.y = o.getNum("y");
                ob.width = o.getNum("width");
                ob.height = o.getNum("height");
                if (o.child("point")) ob.shape = "point";
                else if (o.child("ellipse")) ob.shape = "ellipse";
                ob.props = propsFromXml(o);
                const std::string gid = o.get("gid");
                if (!gid.empty()) {
                    ob.props["gid"] = {"int", gid};
                    ob.y -= ob.height;  // tile objects are anchored bottom-left in Tiled
                }
                l.objects.push_back(std::move(ob));
            }
            m.layers.push_back(std::move(l));
        }
    }
}

void tiledJsonLayers(Map& m, const json& layers, const fs::path& dir, bool parentVisible, double parentOpacity) {
    if (!layers.is_array()) return;
    for (const auto& j : layers) {
        const std::string type = j.value("type", std::string());
        const bool visible = parentVisible && j.value("visible", true);
        const double opacity = parentOpacity * j.value("opacity", 1.0);
        if (type == "group") {
            tiledJsonLayers(m, j.value("layers", json::array()), dir, visible, opacity);
            continue;
        }
        Layer l;
        l.id = j.value("id", 0);
        l.name = j.value("name", std::string("Layer ") + std::to_string(m.layers.size() + 1));
        l.visible = visible;
        l.opacity = opacity;
        l.props = propsFromTiledJson(j.value("properties", json::array()));
        if (type == "tilelayer") {
            if (j.contains("chunks")) throw std::runtime_error("infinite maps (chunked layers) aren't supported; turn off Map > Infinite in Tiled");
            const std::size_t count = static_cast<std::size_t>(m.width) * m.height;
            const json& d = j.value("data", json::array());
            if (d.is_string()) {
                l.cells = unpackGids(inflate(base64Decode(d.get<std::string>()), j.value("compression", std::string())), count);
            } else {
                for (const auto& v : d) l.cells.push_back(v.is_number() ? static_cast<std::uint32_t>(v.get<double>()) : 0u);
                l.cells.resize(count, 0);
            }
        } else if (type == "objectgroup") {
            l.kind = LayerKind::Objects;
            for (const auto& o : j.value("objects", json::array())) {
                MapObject ob;
                ob.id = o.value("id", 0);
                ob.name = o.value("name", std::string());
                ob.type = o.value("type", o.value("class", std::string()));
                ob.x = o.value("x", 0.0);
                ob.y = o.value("y", 0.0);
                ob.width = o.value("width", 0.0);
                ob.height = o.value("height", 0.0);
                if (o.value("point", false)) ob.shape = "point";
                else if (o.value("ellipse", false)) ob.shape = "ellipse";
                ob.props = propsFromTiledJson(o.value("properties", json::array()));
                if (o.contains("gid")) {
                    ob.props["gid"] = {"int", jsonText(o["gid"])};
                    ob.y -= ob.height;
                }
                l.objects.push_back(std::move(ob));
            }
        } else {
            continue;  // image layers are skipped
        }
        m.layers.push_back(std::move(l));
    }
}

Properties layerPropsOut(const Layer& l) {
    Properties p = l.props;
    if (l.collision) p["collision"] = {"bool", "true"};
    return p;
}

}  // namespace

// ---- TMX ----------------------------------------------------------------------------

std::string toTmx(const Map& m, const fs::path& dir, const std::string& encoding) {
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    o << "<map version=\"1.10\" tiledversion=\"1.10.2\" orientation=\"orthogonal\" renderorder=\"right-down\" width=\"" << m.width
      << "\" height=\"" << m.height << "\" tilewidth=\"" << m.tileWidth << "\" tileheight=\"" << m.tileHeight
      << "\" infinite=\"0\"";
    if (!m.background.empty()) o << " backgroundcolor=\"" << esc(m.background) << "\"";
    o << " nextlayerid=\"" << m.nextLayerId << "\" nextobjectid=\"" << m.nextObjectId << "\">\n";
    propsXml(o, m.props, " ");
    for (const auto& t : m.tilesets) {
        o << " <tileset firstgid=\"" << t.firstGid << "\" name=\"" << esc(t.name) << "\" tilewidth=\"" << t.tileWidth << "\" tileheight=\""
          << t.tileHeight << "\"";
        if (t.spacing) o << " spacing=\"" << t.spacing << "\"";
        if (t.margin) o << " margin=\"" << t.margin << "\"";
        o << " tilecount=\"" << t.tileCount << "\" columns=\"" << t.columns << "\">\n";
        if (!t.image.empty()) {
            o << "  <image source=\"" << esc(relativeTo(t.image, dir)) << "\" width=\"" << t.imageWidth << "\" height=\"" << t.imageHeight << "\"/>\n";
        }
        for (const auto& [id, props] : t.tileProps) {
            o << "  <tile id=\"" << id << "\">\n";
            propsXml(o, props, "   ");
            o << "  </tile>\n";
        }
        o << " </tileset>\n";
    }
    for (const auto& l : m.layers) {
        const std::string common = " id=\"" + std::to_string(l.id) + "\" name=\"" + esc(l.name) + "\"";
        std::string extra;
        if (!l.visible) extra += " visible=\"0\"";
        if (l.opacity < 1.0) extra += " opacity=\"" + num(l.opacity) + "\"";
        if (l.kind == LayerKind::Tiles) {
            o << " <layer" << common << " width=\"" << m.width << "\" height=\"" << m.height << "\"" << extra << ">\n";
            propsXml(o, layerPropsOut(l), "  ");
            if (encoding == "base64" || encoding == "zlib") {
                o << "  <data encoding=\"base64\"" << (encoding == "zlib" ? " compression=\"zlib\"" : "") << ">\n   "
                  << encodeCells(l.cells, encoding) << "\n  </data>\n";
            } else {
                o << "  <data encoding=\"csv\">\n";
                for (int y = 0; y < m.height; ++y) {
                    for (int x = 0; x < m.width; ++x) {
                        o << m.at(l, x, y);
                        if (x + 1 < m.width || y + 1 < m.height) o << ',';
                    }
                    o << '\n';
                }
                o << "</data>\n";
            }
            o << " </layer>\n";
        } else {
            o << " <objectgroup" << common << extra << ">\n";
            propsXml(o, layerPropsOut(l), "  ");
            for (const auto& ob : l.objects) {
                o << "  <object id=\"" << ob.id << "\"";
                if (!ob.name.empty()) o << " name=\"" << esc(ob.name) << "\"";
                if (!ob.type.empty()) o << " type=\"" << esc(ob.type) << "\"";
                o << " x=\"" << num(ob.x) << "\" y=\"" << num(ob.y) << "\"";
                if (ob.shape != "point" && (ob.width > 0 || ob.height > 0)) o << " width=\"" << num(ob.width) << "\" height=\"" << num(ob.height) << "\"";
                if (ob.props.empty() && ob.shape == "rect") {
                    o << "/>\n";
                    continue;
                }
                o << ">\n";
                propsXml(o, ob.props, "   ");
                if (ob.shape == "point") o << "   <point/>\n";
                if (ob.shape == "ellipse") o << "   <ellipse/>\n";
                o << "  </object>\n";
            }
            o << " </objectgroup>\n";
        }
    }
    o << "</map>\n";
    return o.str();
}

Map fromTmx(const std::string& text, const fs::path& dir) {
    XmlNode root = XmlParser(text).parse();
    if (root.name != "map") throw std::runtime_error("not a TMX map (root element is <" + root.name + ">)");
    const std::string orient = root.get("orientation", "orthogonal");
    if (orient != "orthogonal") throw std::runtime_error(orient + " maps aren't supported (only orthogonal)");
    if (root.get("infinite") == "1") throw std::runtime_error("infinite maps aren't supported; turn off Map > Infinite in Tiled");
    Map m = newMap(root.getInt("width"), root.getInt("height"), root.getInt("tilewidth", 16), root.getInt("tileheight", 16));
    checkSize(m);
    m.background = root.get("backgroundcolor");
    m.nextLayerId = root.getInt("nextlayerid", 1);
    m.nextObjectId = root.getInt("nextobjectid", 1);
    m.props = propsFromXml(root);
    for (const auto& c : root.children) {
        if (c.name != "tileset") continue;
        Tileset t;
        if (const std::string* src = c.attr("source")) {
            t = externalTileset(dir / *src);
        } else {
            t = tilesetFromXml(c, dir);
        }
        t.firstGid = static_cast<std::uint32_t>(c.getInt("firstgid", 1));
        m.tilesets.push_back(std::move(t));
    }
    tmxLayers(m, root, dir, true, 1.0);
    finish(m);
    return m;
}

// ---- TMJ ----------------------------------------------------------------------------

std::string toTmj(const Map& m, const fs::path& dir, const std::string& encoding) {
    json j;
    j["compressionlevel"] = -1;
    j["type"] = "map";
    j["version"] = "1.10";
    j["tiledversion"] = "1.10.2";
    j["orientation"] = "orthogonal";
    j["renderorder"] = "right-down";
    j["width"] = m.width;
    j["height"] = m.height;
    j["tilewidth"] = m.tileWidth;
    j["tileheight"] = m.tileHeight;
    j["infinite"] = false;
    if (!m.background.empty()) j["backgroundcolor"] = m.background;
    j["nextlayerid"] = m.nextLayerId;
    j["nextobjectid"] = m.nextObjectId;
    if (!m.props.empty()) j["properties"] = propsTiledJson(m.props);
    json ts = json::array();
    for (const auto& t : m.tilesets) {
        json tj = {{"firstgid", t.firstGid}, {"name", t.name}, {"tilewidth", t.tileWidth}, {"tileheight", t.tileHeight},
                   {"spacing", t.spacing},   {"margin", t.margin}, {"columns", t.columns}, {"tilecount", t.tileCount}};
        if (!t.image.empty()) {
            tj["image"] = relativeTo(t.image, dir);
            tj["imagewidth"] = t.imageWidth;
            tj["imageheight"] = t.imageHeight;
        }
        if (!t.tileProps.empty()) {
            json tiles = json::array();
            for (const auto& [id, props] : t.tileProps) tiles.push_back({{"id", id}, {"properties", propsTiledJson(props)}});
            tj["tiles"] = tiles;
        }
        ts.push_back(tj);
    }
    j["tilesets"] = ts;
    json layers = json::array();
    for (const auto& l : m.layers) {
        json lj = {{"id", l.id}, {"name", l.name}};
        if (l.kind == LayerKind::Tiles) {
            lj["type"] = "tilelayer";
            lj["width"] = m.width;
            lj["height"] = m.height;
            if (encoding == "base64" || encoding == "zlib") {
                lj["encoding"] = "base64";
                if (encoding == "zlib") lj["compression"] = "zlib";
                lj["data"] = encodeCells(l.cells, encoding);
            } else {
                lj["data"] = l.cells;
            }
        } else {
            lj["type"] = "objectgroup";
            lj["draworder"] = "topdown";
            json objs = json::array();
            for (const auto& ob : l.objects) {
                json oj = {{"id", ob.id},         {"name", ob.name}, {"type", ob.type},      {"x", ob.x},       {"y", ob.y},
                           {"width", ob.width}, {"height", ob.height}, {"rotation", 0}, {"visible", true}};
                if (ob.shape == "point") oj["point"] = true;
                if (ob.shape == "ellipse") oj["ellipse"] = true;
                if (!ob.props.empty()) oj["properties"] = propsTiledJson(ob.props);
                objs.push_back(oj);
            }
            lj["objects"] = objs;
        }
        lj["x"] = 0;
        lj["y"] = 0;
        lj["opacity"] = l.opacity;
        lj["visible"] = l.visible;
        if (Properties p = layerPropsOut(l); !p.empty()) lj["properties"] = propsTiledJson(p);
        layers.push_back(lj);
    }
    j["layers"] = layers;
    return j.dump(1) + "\n";
}

Map fromTmj(const std::string& text, const fs::path& dir) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("not valid JSON");
    const std::string orient = j.value("orientation", std::string("orthogonal"));
    if (orient != "orthogonal") throw std::runtime_error(orient + " maps aren't supported (only orthogonal)");
    if (j.value("infinite", false)) throw std::runtime_error("infinite maps aren't supported; turn off Map > Infinite in Tiled");
    Map m = newMap(j.value("width", 0), j.value("height", 0), j.value("tilewidth", 16), j.value("tileheight", 16));
    checkSize(m);
    m.background = j.value("backgroundcolor", std::string());
    m.nextLayerId = j.value("nextlayerid", 1);
    m.nextObjectId = j.value("nextobjectid", 1);
    m.props = propsFromTiledJson(j.value("properties", json::array()));
    for (const auto& tj : j.value("tilesets", json::array())) {
        Tileset t = tj.contains("source") ? externalTileset(dir / tj.value("source", std::string())) : tilesetFromTiledJson(tj, dir);
        t.firstGid = tj.value("firstgid", 1u);
        m.tilesets.push_back(std::move(t));
    }
    tiledJsonLayers(m, j.value("layers", json::array()), dir, true, 1.0);
    finish(m);
    return m;
}

// ---- plain JSON ---------------------------------------------------------------------

std::string toPlainJson(const Map& m, const fs::path& dir) {
    json j;
    j["format"] = "wetwaresuite-level";
    j["version"] = 1;
    j["width"] = m.width;
    j["height"] = m.height;
    j["tileWidth"] = m.tileWidth;
    j["tileHeight"] = m.tileHeight;
    if (!m.background.empty()) j["background"] = m.background;
    j["properties"] = propsPlain(m.props);
    json ts = json::array();
    for (const auto& t : m.tilesets) {
        json tj = {{"name", t.name},         {"firstGid", t.firstGid}, {"image", relativeTo(t.image, dir)}, {"tileWidth", t.tileWidth},
                   {"tileHeight", t.tileHeight}, {"margin", t.margin}, {"spacing", t.spacing},       {"columns", t.columns},
                   {"tileCount", t.tileCount}};
        json tiles = json::object();
        for (const auto& [id, props] : t.tileProps) tiles[std::to_string(id)] = propsPlain(props);
        tj["tiles"] = tiles;
        ts.push_back(tj);
    }
    j["tilesets"] = ts;
    json layers = json::array();
    for (const auto& l : m.layers) {
        json lj = {{"name", l.name}, {"type", l.kind == LayerKind::Tiles ? "tiles" : "objects"}, {"visible", l.visible}, {"opacity", l.opacity}};
        if (l.collision) lj["collision"] = true;
        lj["properties"] = propsPlain(l.props);
        if (l.kind == LayerKind::Tiles) {
            json rows = json::array();
            for (int y = 0; y < m.height; ++y) {
                json row = json::array();
                for (int x = 0; x < m.width; ++x) row.push_back(m.at(l, x, y));
                rows.push_back(row);
            }
            lj["data"] = rows;
        } else {
            json objs = json::array();
            for (const auto& ob : l.objects) {
                objs.push_back({{"id", ob.id},         {"name", ob.name},     {"type", ob.type},   {"x", ob.x},
                                {"y", ob.y},           {"width", ob.width}, {"height", ob.height}, {"shape", ob.shape},
                                {"properties", propsPlain(ob.props)}});
            }
            lj["objects"] = objs;
        }
        layers.push_back(lj);
    }
    j["layers"] = layers;
    // one row per line keeps the file readable and diffable
    std::string out = j.dump(1);
    std::string compact;
    compact.reserve(out.size());
    int depthInRow = 0;
    bool inString = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (inString) {
            compact += out[i];
            if (out[i] == '\\' && i + 1 < out.size()) compact += out[++i];
            else if (out[i] == '"') inString = false;
            continue;
        }
        if (out[i] == '"') {
            inString = true;
            compact += out[i];
            continue;
        }
        // collapse whitespace inside innermost numeric arrays
        if (out[i] == '[') {
            std::size_t k = i + 1;
            while (k < out.size() && (std::isspace(static_cast<unsigned char>(out[k])))) ++k;
            if (k < out.size() && (std::isdigit(static_cast<unsigned char>(out[k])) || out[k] == ']')) depthInRow = 1;
            compact += out[i];
            continue;
        }
        if (depthInRow) {
            if (out[i] == ']') depthInRow = 0;
            if (std::isspace(static_cast<unsigned char>(out[i]))) continue;
        }
        compact += out[i];
    }
    return compact + "\n";
}

Map fromPlainJson(const std::string& text, const fs::path& dir) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("not valid JSON");
    Map m = newMap(j.value("width", 0), j.value("height", 0), j.value("tileWidth", 16), j.value("tileHeight", 16));
    checkSize(m);
    m.background = j.value("background", std::string());
    m.props = propsFromPlain(j.value("properties", json::object()));
    for (const auto& tj : j.value("tilesets", json::array())) {
        Tileset t;
        t.name = tj.value("name", std::string());
        t.firstGid = tj.value("firstGid", m.nextGid());
        t.image = resolveFrom(dir, tj.value("image", std::string()));
        t.tileWidth = tj.value("tileWidth", m.tileWidth);
        t.tileHeight = tj.value("tileHeight", m.tileHeight);
        t.margin = tj.value("margin", 0);
        t.spacing = tj.value("spacing", 0);
        t.columns = tj.value("columns", 0);
        t.tileCount = tj.value("tileCount", 0);
        const json tiles = tj.value("tiles", json::object());
        for (auto it = tiles.begin(); it != tiles.end(); ++it) t.tileProps[std::atoi(it.key().c_str())] = propsFromPlain(it.value());
        attachImage(t);
        m.tilesets.push_back(std::move(t));
    }
    for (const auto& lj : j.value("layers", json::array())) {
        Layer l;
        l.name = lj.value("name", std::string("Layer ") + std::to_string(m.layers.size() + 1));
        l.kind = lj.value("type", std::string("tiles")) == "objects" ? LayerKind::Objects : LayerKind::Tiles;
        l.visible = lj.value("visible", true);
        l.opacity = lj.value("opacity", 1.0);
        l.collision = lj.value("collision", false);
        l.props = propsFromPlain(lj.value("properties", json::object()));
        if (l.kind == LayerKind::Tiles) {
            l.cells.assign(static_cast<std::size_t>(m.width) * m.height, 0);
            const json data = lj.value("data", json::array());
            for (int y = 0; y < m.height && y < static_cast<int>(data.size()); ++y) {
                const json& row = data[static_cast<std::size_t>(y)];
                if (row.is_number()) {  // a flat array is fine too
                    for (std::size_t i = 0; i < data.size() && i < l.cells.size(); ++i) l.cells[i] = data[i].get<std::uint32_t>();
                    break;
                }
                for (int x = 0; x < m.width && x < static_cast<int>(row.size()); ++x) m.set(l, x, y, row[static_cast<std::size_t>(x)].get<std::uint32_t>());
            }
        } else {
            for (const auto& o : lj.value("objects", json::array())) {
                MapObject ob;
                ob.id = o.value("id", 0);
                ob.name = o.value("name", std::string());
                ob.type = o.value("type", std::string());
                ob.x = o.value("x", 0.0);
                ob.y = o.value("y", 0.0);
                ob.width = o.value("width", 0.0);
                ob.height = o.value("height", 0.0);
                ob.shape = o.value("shape", std::string("rect"));
                ob.props = propsFromPlain(o.value("properties", json::object()));
                l.objects.push_back(std::move(ob));
            }
        }
        m.layers.push_back(std::move(l));
    }
    finish(m);
    return m;
}

// ---- LDtk-lite ----------------------------------------------------------------------

namespace {

std::string iid(int n) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "a0c1e5d0-0000-4000-8000-%012d", n);
    return buf;
}

std::string ldtkFieldType(const Property& p) {
    if (p.type == "int") return "Int";
    if (p.type == "float") return "Float";
    if (p.type == "bool") return "Bool";
    if (p.type == "color") return "Color";
    if (p.type == "file") return "FilePath";
    return "String";
}

}  // namespace

std::string toLdtk(const Map& m, const fs::path& dir, std::vector<std::string>* warnings) {
    const int grid = m.tileWidth;
    if (warnings && m.tileWidth != m.tileHeight) warnings->push_back("LDtk needs square cells; using the tile width " + std::to_string(grid));
    int uid = 100;
    json tilesets = json::array();
    std::map<const Tileset*, int> tsUid;
    for (const auto& t : m.tilesets) {
        tsUid[&t] = uid;
        tilesets.push_back({{"__cWid", t.columns},
                            {"__cHei", t.columns ? (t.tileCount + t.columns - 1) / t.columns : 0},
                            {"identifier", t.name.empty() ? "Tileset" + std::to_string(uid) : t.name},
                            {"uid", uid++},
                            {"relPath", t.image.empty() ? json(nullptr) : json(relativeTo(t.image, dir))},
                            {"pxWid", t.imageWidth},
                            {"pxHei", t.imageHeight},
                            {"tileGridSize", t.tileWidth},
                            {"spacing", t.spacing},
                            {"padding", t.margin},
                            {"tags", json::array()},
                            {"customData", json::array()},
                            {"enumTags", json::array()}});
    }
    json layerDefs = json::array(), instances = json::array(), entityDefs = json::array();
    std::map<std::string, int> entityUid;
    int iidN = 1;
    const json levelUidStub = 0;
    // LDtk lists layers top first
    for (std::size_t li = m.layers.size(); li-- > 0;) {
        const Layer& l = m.layers[li];
        const int layerUid = uid++;
        json inst = {{"__identifier", l.name}, {"__cWid", m.width}, {"__cHei", m.height}, {"__gridSize", grid}, {"__opacity", l.opacity},
                     {"__pxTotalOffsetX", 0}, {"__pxTotalOffsetY", 0}, {"__tilesetDefUid", nullptr}, {"__tilesetRelPath", nullptr},
                     {"iid", iid(iidN++)}, {"levelId", levelUidStub}, {"layerDefUid", layerUid}, {"pxOffsetX", 0}, {"pxOffsetY", 0},
                     {"visible", l.visible}, {"optionalRules", json::array()}, {"intGridCsv", json::array()},
                     {"autoLayerTiles", json::array()}, {"seed", 0}, {"overrideTilesetUid", nullptr}, {"gridTiles", json::array()},
                     {"entityInstances", json::array()}};
        json def = {{"identifier", l.name}, {"uid", layerUid}, {"gridSize", grid}, {"displayOpacity", l.opacity}};
        if (l.kind == LayerKind::Tiles && l.collision) {
            inst["__type"] = "IntGrid";
            def["__type"] = def["type"] = "IntGrid";
            def["intGridValues"] = json::array({{{"value", 1}, {"identifier", "solid"}, {"color", "#E62828"}}});
            json csv = json::array();
            for (auto c : l.cells) csv.push_back((c & kGidMask) ? 1 : 0);
            inst["intGridCsv"] = csv;
        } else if (l.kind == LayerKind::Tiles) {
            inst["__type"] = "Tiles";
            def["__type"] = def["type"] = "Tiles";
            // LDtk layers use one tileset: the one most cells use
            std::map<const Tileset*, int> uses;
            for (auto c : l.cells) {
                if (const Tileset* t = m.tilesetFor(c)) uses[t]++;
            }
            const Tileset* t = nullptr;
            for (auto& [k, n] : uses) {
                if (!t || n > uses[t]) t = k;
            }
            json tiles = json::array();
            int skipped = 0;
            if (t) {
                inst["__tilesetDefUid"] = tsUid[t];
                inst["__tilesetRelPath"] = t->image.empty() ? json(nullptr) : json(relativeTo(t->image, dir));
                def["tilesetDefUid"] = tsUid[t];
                for (int y = 0; y < m.height; ++y) {
                    for (int x = 0; x < m.width; ++x) {
                        const std::uint32_t raw = m.at(l, x, y), g = raw & kGidMask;
                        if (!g) continue;
                        if (!t->contains(g)) {
                            ++skipped;
                            continue;
                        }
                        const int id = static_cast<int>(g - t->firstGid);
                        const int f = (raw & kFlipH ? 1 : 0) | (raw & kFlipV ? 2 : 0);
                        tiles.push_back({{"px", {x * grid, y * grid}}, {"src", {t->tileX(id), t->tileY(id)}}, {"f", f}, {"t", id},
                                         {"d", {y * m.width + x}}, {"a", 1}});
                    }
                }
            } else {
                for (auto c : l.cells) skipped += (c & kGidMask) ? 1 : 0;
            }
            if (skipped && warnings) warnings->push_back("layer " + l.name + ": " + std::to_string(skipped) + " tiles from a second tileset (or none) were left out");
            inst["gridTiles"] = tiles;
        } else {
            inst["__type"] = "Entities";
            def["__type"] = def["type"] = "Entities";
            json ents = json::array();
            for (const auto& ob : l.objects) {
                const std::string ident = ob.type.empty() ? (ob.name.empty() ? "Entity" : ob.name) : ob.type;
                if (!entityUid.count(ident)) {
                    entityUid[ident] = uid;
                    entityDefs.push_back({{"identifier", ident}, {"uid", uid++}, {"width", std::max(1, static_cast<int>(ob.width))},
                                          {"height", std::max(1, static_cast<int>(ob.height))}, {"color", "#94D9B3"}, {"pivotX", 0}, {"pivotY", 0},
                                          {"fieldDefs", json::array()}, {"tags", json::array()}});
                }
                json fields = json::array();
                if (!ob.name.empty() && ob.name != ident) fields.push_back({{"__identifier", "name"}, {"__type", "String"}, {"__value", ob.name}, {"__tile", nullptr}, {"defUid", 0}, {"realEditorValues", json::array()}});
                if (ob.shape != "rect") {
                    fields.push_back({{"__identifier", "shape"}, {"__type", "String"}, {"__value", ob.shape}, {"__tile", nullptr}, {"defUid", 0},
                                      {"realEditorValues", json::array()}});
                }
                for (const auto& [k, v] : ob.props) {
                    fields.push_back({{"__identifier", k}, {"__type", ldtkFieldType(v)}, {"__value", propValue(v)}, {"__tile", nullptr}, {"defUid", 0},
                                      {"realEditorValues", json::array()}});
                }
                ents.push_back({{"__identifier", ident},
                                {"__grid", {static_cast<int>(ob.x / grid), static_cast<int>(ob.y / grid)}},
                                {"__pivot", {0, 0}},
                                {"__tags", json::array()},
                                {"__tile", nullptr},
                                {"__smartColor", "#94D9B3"},
                                {"__worldX", static_cast<int>(ob.x)},
                                {"__worldY", static_cast<int>(ob.y)},
                                {"iid", iid(iidN++)},
                                {"width", static_cast<int>(ob.width)},
                                {"height", static_cast<int>(ob.height)},
                                {"defUid", entityUid[ident]},
                                {"px", {static_cast<int>(ob.x), static_cast<int>(ob.y)}},
                                {"fieldInstances", fields}});
            }
            inst["entityInstances"] = ents;
        }
        layerDefs.push_back(def);
        instances.push_back(inst);
    }
    json fields = json::array();
    for (const auto& [k, v] : m.props) fields.push_back({{"__identifier", k}, {"__type", ldtkFieldType(v)}, {"__value", propValue(v)}, {"__tile", nullptr}, {"defUid", 0}, {"realEditorValues", json::array()}});
    json level = {{"identifier", "Level_0"}, {"iid", iid(0)}, {"uid", 0}, {"worldX", 0}, {"worldY", 0}, {"worldDepth", 0},
                  {"pxWid", m.width * grid}, {"pxHei", m.height * grid},
                  {"__bgColor", m.background.empty() ? "#696A79" : m.background}, {"bgColor", nullptr}, {"useAutoIdentifier", false},
                  {"bgRelPath", nullptr}, {"bgPos", nullptr}, {"bgPivotX", 0.5}, {"bgPivotY", 0.5}, {"__smartColor", "#ADADB5"},
                  {"__bgPos", nullptr}, {"externalRelPath", nullptr}, {"fieldInstances", fields}, {"layerInstances", instances},
                  {"__neighbours", json::array()}};
    json j = {{"__header__", {{"fileType", "LDtk Project JSON"}, {"app", "LDtk"}, {"doc", "https://ldtk.io/json"},
                              {"schema", "https://ldtk.io/files/JSON_SCHEMA.json"}, {"appAuthor", "wetwaresuite levels (LDtk-lite)"},
                              {"appVersion", "1.5.3"}, {"url", "https://ldtk.io"}}},
              {"iid", iid(999999)},
              {"jsonVersion", "1.5.3"},
              {"appBuildId", 0},
              {"nextUid", uid},
              {"identifierStyle", "Capitalize"},
              {"worldLayout", "Free"},
              {"worldGridWidth", m.width * grid},
              {"worldGridHeight", m.height * grid},
              {"defaultLevelWidth", m.width * grid},
              {"defaultLevelHeight", m.height * grid},
              {"defaultPivotX", 0},
              {"defaultPivotY", 0},
              {"defaultGridSize", grid},
              {"defaultEntityWidth", grid},
              {"defaultEntityHeight", grid},
              {"bgColor", "#40465B"},
              {"defaultLevelBgColor", "#696A79"},
              {"minifyJson", true},
              {"externalLevels", false},
              {"exportTiled", false},
              {"simplifiedExport", false},
              {"imageExportMode", "None"},
              {"exportLevelBg", true},
              {"pngFilePattern", nullptr},
              {"backupOnSave", false},
              {"backupLimit", 10},
              {"backupRelPath", nullptr},
              {"levelNamePattern", "Level_%idx"},
              {"tutorialDesc", nullptr},
              {"customCommands", json::array()},
              {"flags", json::array()},
              {"defs", {{"layers", layerDefs}, {"entities", entityDefs}, {"tilesets", tilesets}, {"enums", json::array()},
                        {"externalEnums", json::array()}, {"levelFields", json::array()}}},
              {"levels", json::array({level})},
              {"worlds", json::array()},
              {"toc", json::array()},
              {"dummyWorldIid", iid(999998)}};
    return j.dump() + "\n";
}

Map fromLdtk(const std::string& text, const fs::path& dir, const std::string& levelName) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw std::runtime_error("not valid JSON");
    json levels = j.value("levels", json::array());
    if (levels.empty() && j.contains("worlds") && !j["worlds"].empty()) levels = j["worlds"][0].value("levels", json::array());
    if (levels.empty()) throw std::runtime_error("the LDtk file has no levels");
    const json* level = &levels[0];
    if (!levelName.empty()) {
        level = nullptr;
        for (const auto& l : levels) {
            if (l.value("identifier", std::string()) == levelName) level = &l;
        }
        if (!level) throw std::runtime_error("no LDtk level called '" + levelName + "'");
    }
    if (level->contains("externalRelPath") && (*level)["externalRelPath"].is_string()) {
        throw std::runtime_error("levels saved in separate files (externalLevels) aren't supported");
    }
    const json instances = level->value("layerInstances", json::array());
    int grid = j.value("defaultGridSize", 16);
    for (const auto& inst : instances) {
        if (inst.value("__type", std::string()) != "Entities") {
            grid = inst.value("__gridSize", grid);
            break;
        }
    }
    const int pxW = level->value("pxWid", 0), pxH = level->value("pxHei", 0);
    Map m = newMap((pxW + grid - 1) / grid, (pxH + grid - 1) / grid, grid, grid);
    checkSize(m);
    if (level->contains("__bgColor") && (*level)["__bgColor"].is_string()) m.background = (*level)["__bgColor"].get<std::string>();
    for (const auto& f : level->value("fieldInstances", json::array())) m.props[f.value("__identifier", std::string())] = propFromJson(f.value("__value", json()));
    std::map<int, std::size_t> tsByUid;
    if (j.contains("defs")) {
        for (const auto& tj : j["defs"].value("tilesets", json::array())) {
            Tileset t;
            t.name = tj.value("identifier", std::string());
            t.tileWidth = t.tileHeight = tj.value("tileGridSize", grid);
            t.spacing = tj.value("spacing", 0);
            t.margin = tj.value("padding", 0);
            t.imageWidth = tj.value("pxWid", 0);
            t.imageHeight = tj.value("pxHei", 0);
            t.columns = tj.value("__cWid", 0);
            t.tileCount = t.columns * tj.value("__cHei", 0);
            if (tj.contains("relPath") && tj["relPath"].is_string()) t.image = resolveFrom(dir, tj["relPath"].get<std::string>());
            attachImage(t);
            if (t.tileCount <= 0) continue;  // embedded atlases and the like
            t.firstGid = m.nextGid();
            tsByUid[tj.value("uid", 0)] = m.tilesets.size();
            m.tilesets.push_back(std::move(t));
        }
    }
    // layerInstances are top first
    for (std::size_t k = instances.size(); k-- > 0;) {
        const json& inst = instances[k];
        const std::string type = inst.value("__type", std::string());
        const std::string name = inst.value("__identifier", std::string("Layer"));
        const int g = inst.value("__gridSize", grid);
        auto tilesInto = [&](Layer& l, const json& tiles) {
            const Tileset* t = nullptr;
            if (inst.contains("__tilesetDefUid") && inst["__tilesetDefUid"].is_number()) {
                auto it = tsByUid.find(inst["__tilesetDefUid"].get<int>());
                if (it != tsByUid.end()) t = &m.tilesets[it->second];
            }
            if (!t) return;
            for (const auto& tile : tiles) {
                const json px = tile.value("px", json::array());
                if (px.size() < 2) continue;
                const int x = px[0].get<int>() / g, y = px[1].get<int>() / g, f = tile.value("f", 0);
                if (!m.inside(x, y)) continue;
                std::uint32_t gid = t->firstGid + static_cast<std::uint32_t>(tile.value("t", 0));
                if (f & 1) gid |= kFlipH;
                if (f & 2) gid |= kFlipV;
                m.set(l, x, y, gid);
            }
        };
        if (type == "Entities") {
            Layer& l = m.addLayer(name, LayerKind::Objects);
            l.visible = inst.value("visible", true);
            for (const auto& e : inst.value("entityInstances", json::array())) {
                MapObject ob;
                ob.id = m.nextObjectId++;
                ob.type = e.value("__identifier", std::string());
                ob.name = ob.type;
                const json px = e.value("px", json::array());
                if (px.size() >= 2) {
                    ob.x = px[0].get<double>();
                    ob.y = px[1].get<double>();
                }
                ob.width = e.value("width", 0.0);
                ob.height = e.value("height", 0.0);
                const json pivot = e.value("__pivot", json::array());
                if (pivot.size() >= 2) {  // our objects are anchored top-left
                    ob.x -= pivot[0].get<double>() * ob.width;
                    ob.y -= pivot[1].get<double>() * ob.height;
                }
                for (const auto& f : e.value("fieldInstances", json::array())) {
                    const std::string key = f.value("__identifier", std::string());
                    if (key == "name" && f.value("__value", json()).is_string()) ob.name = f["__value"].get<std::string>();
                    else if (key == "shape" && f.value("__value", json()).is_string()) ob.shape = f["__value"].get<std::string>();
                    else if (!key.empty()) ob.props[key] = propFromJson(f.value("__value", json()));
                }
                l.objects.push_back(std::move(ob));
            }
            continue;
        }
        if (type == "IntGrid") {
            Layer& l = m.addLayer(name, LayerKind::Tiles);
            l.collision = true;
            l.visible = inst.value("visible", true);
            const json csv = inst.value("intGridCsv", json::array());
            for (std::size_t i = 0; i < csv.size() && i < l.cells.size(); ++i) l.cells[i] = csv[i].get<std::uint32_t>();
            const json autoTiles = inst.value("autoLayerTiles", json::array());
            if (!autoTiles.empty()) {
                Layer& tl = m.addLayer(name + "_tiles", LayerKind::Tiles, static_cast<int>(m.layers.size()) - 1);
                tilesInto(tl, autoTiles);
            }
            continue;
        }
        Layer& l = m.addLayer(name, LayerKind::Tiles);
        l.visible = inst.value("visible", true);
        l.opacity = inst.value("__opacity", 1.0);
        tilesInto(l, inst.value("gridTiles", json::array()));
        tilesInto(l, inst.value("autoLayerTiles", json::array()));
    }
    finish(m);
    return m;
}

// ---- CSV ----------------------------------------------------------------------------

std::string layerCsv(const Map& m, const Layer& l, bool local) {
    std::string out;
    for (int y = 0; y < m.height; ++y) {
        for (int x = 0; x < m.width; ++x) {
            const std::uint32_t g = m.at(l, x, y) & kGidMask;
            if (x) out += ',';
            if (!local || l.collision) out += std::to_string(g);
            else if (g == 0) out += "-1";
            else if (const Tileset* t = m.tilesetFor(g)) out += std::to_string(g - t->firstGid);
            else out += std::to_string(m.tilesets.empty() ? g - 1 : g);  // no tileset: tile n was stored as n+1
        }
        out += '\n';
    }
    return out;
}

std::vector<std::uint32_t> parseCsv(const Map& m, const std::string& text, bool local, int& rows, int& cols) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(m.width) * m.height, 0);
    const std::uint32_t first = m.tilesets.empty() ? 1u : m.tilesets.front().firstGid;
    rows = cols = 0;
    std::istringstream in(text);
    std::string line;
    int y = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t,") == std::string::npos) continue;
        int x = 0;
        std::size_t at = 0;
        while (at <= line.size()) {
            std::size_t comma = line.find(',', at);
            if (comma == std::string::npos) comma = line.size();
            std::string cell = line.substr(at, comma - at);
            const auto b = cell.find_first_not_of(" \t\""), e = cell.find_last_not_of(" \t\"");
            cell = b == std::string::npos ? std::string() : cell.substr(b, e - b + 1);
            long long v = cell.empty() ? (local ? -1 : 0) : std::strtoll(cell.c_str(), nullptr, 10);
            std::uint32_t g = local ? (v < 0 ? 0u : static_cast<std::uint32_t>(v) + first) : (v < 0 ? 0u : static_cast<std::uint32_t>(v));
            if (x < m.width && y < m.height) out[static_cast<std::size_t>(y) * m.width + x] = g;
            ++x;
            at = comma + 1;
        }
        cols = std::max(cols, x);
        ++y;
    }
    rows = y;
    return out;
}

// ---- files --------------------------------------------------------------------------

std::string formatFor(const fs::path& path) {
    const std::string ext = lower(path.extension().string());
    if (ext == ".tmx" || ext == ".xml") return "tmx";
    if (ext == ".tmj") return "tmj";
    if (ext == ".ldtk") return "ldtk";
    if (ext == ".csv") return "csv";
    if (ext == ".json") {
        std::error_code ec;
        if (fs::exists(path, ec)) {
            std::ifstream in(path, std::ios::binary);
            std::string head(4096, '\0');
            in.read(head.data(), static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<std::size_t>(in.gcount()));
            if (head.find("\"__header__\"") != std::string::npos || head.find("\"jsonVersion\"") != std::string::npos) return "ldtk";
            if (head.find("\"tiledversion\"") != std::string::npos || head.find("\"orientation\"") != std::string::npos) return "tmj";
        }
        return "json";
    }
    if (raster::canWrite(path)) return "png";
    return {};
}

Map loadMap(const fs::path& path) {
    const std::string format = formatFor(path);
    const std::string text = readFile(path);
    const fs::path dir = path.parent_path();
    if (format == "tmx") return fromTmx(text, dir);
    if (format == "tmj") return fromTmj(text, dir);
    if (format == "ldtk") return fromLdtk(text, dir);
    if (format == "json") {
        json j = json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("tiledversion")) return fromTmj(text, dir);
        if (j.is_object() && j.contains("__header__")) return fromLdtk(text, dir);
        return fromPlainJson(text, dir);
    }
    throw std::runtime_error("can't open " + path.string() + " as a map (use .tmx, .tmj, .json or .ldtk; CSV goes into a layer with levels.import_csv)");
}

void saveMap(const Map& map, const fs::path& path, const std::string& format, const std::string& encoding, double scale) {
    const fs::path dir = path.parent_path();
    if (format == "tmx") writeFile(path, toTmx(map, dir, encoding));
    else if (format == "tmj") writeFile(path, toTmj(map, dir, encoding));
    else if (format == "json") writeFile(path, toPlainJson(map, dir));
    else if (format == "ldtk") writeFile(path, toLdtk(map, dir));
    else if (format == "png") {
        RenderOptions o;
        o.scale = scale;
        o.objects = false;
        o.collision = false;
        if (auto parent = path.parent_path(); !parent.empty()) {
            std::error_code ec;
            fs::create_directories(parent, ec);
        }
        raster::save(render(map, o), path);
    } else {
        throw std::runtime_error("unknown map format '" + format + "'");
    }
}

}  // namespace wetwaresuite::levels
