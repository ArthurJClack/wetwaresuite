// Vector files: GeoJSON, WKT, KML, GPX and CSV, the import/export dispatch,
// and .achmap project files. Shapefiles live in shapefile.cpp, rasters in raster_io.cpp.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::maps {

namespace fs = std::filesystem;
using nlohmann::json;

// ---- helpers ------------------------------------------------------------------------------

namespace detail {

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeFile(const fs::path& path, const std::string& bytes) {
    if (auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("can't write " + path.string());
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string fmt(double v, int maxDecimals) {
    if (!std::isfinite(v)) return "0";
    char buf[64];
    if (std::abs(v) >= 1e15) {
        std::snprintf(buf, sizeof buf, "%.15g", v);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.*f", std::clamp(maxDecimals, 0, 15), v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string jsonEscape(const std::string& s) { return json(s).dump(); }

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

bool validUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        const int n = (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0) return false;
        for (int k = 1; k <= n; ++k) {
            if (i + static_cast<std::size_t>(k) >= s.size() || (static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) >> 6) != 2) return false;
        }
        i += static_cast<std::size_t>(n) + 1;
    }
    return true;
}

std::string latin1ToUtf8(const std::string& s) {
    std::string out;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out += ch;
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
}

double parseTime(const std::string& iso) {
    const std::string s = trim(iso);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    double sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return kNaN;
    std::size_t p = s.find_first_of("Tt ");
    if (p != std::string::npos) {
        if (std::sscanf(s.c_str() + p + 1, "%d:%d:%lf", &h, &mi, &sec) < 2) return kNaN;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return kNaN;
    double t = static_cast<double>(daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d))) * 86400 + h * 3600 + mi * 60 + sec;
    // offset after the time part
    if (p != std::string::npos) {
        const std::string rest = s.substr(p + 1);
        const auto sign = rest.find_first_of("+-", 5);
        if (sign != std::string::npos) {
            int oh = 0, om = 0;
            if (std::sscanf(rest.c_str() + sign + 1, "%d:%d", &oh, &om) >= 1) {
                if (rest.size() - sign - 1 == 4 && rest.find(':', sign) == std::string::npos) {
                    om = oh % 100;
                    oh /= 100;
                }
                const double off = oh * 3600.0 + om * 60.0;
                t += rest[sign] == '+' ? -off : off;
            }
        }
    }
    return t;
}

std::string formatTime(double t) {
    if (!std::isfinite(t)) return {};
    const auto whole = static_cast<std::int64_t>(std::floor(t));
    std::int64_t days = whole / 86400, rem = whole % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    int y = 0;
    unsigned m = 0, d = 0;
    civilFromDays(days, y, m, d);
    char buf[48];
    const double frac = t - static_cast<double>(whole);
    if (frac > 0.0005) {
        std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%06.3fZ", y, m, d, static_cast<int>(rem / 3600), static_cast<int>(rem / 60 % 60),
                      static_cast<double>(rem % 60) + frac);
    } else {
        std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", y, m, d, static_cast<int>(rem / 3600), static_cast<int>(rem / 60 % 60),
                      static_cast<int>(rem % 60));
    }
    return buf;
}

std::string base64Encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += t[v >> 18];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& in) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        int x = -1;
        if (c >= 'A' && c <= 'Z') x = c - 'A';
        else if (c >= 'a' && c <= 'z') x = c - 'a' + 26;
        else if (c >= '0' && c <= '9') x = c - '0' + 52;
        else if (c == '+' || c == '-') x = 62;
        else if (c == '/' || c == '_') x = 63;
        if (x < 0) continue;
        v = (v << 6) | static_cast<unsigned>(x);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return out;
}

// ---- XML ----

const XmlNode* XmlNode::child(const std::string& n) const {
    for (const auto& c : children) {
        if (c->name == n) return c.get();
    }
    return nullptr;
}

std::vector<const XmlNode*> XmlNode::all(const std::string& n) const {
    std::vector<const XmlNode*> out;
    for (const auto& c : children) {
        if (c->name == n) out.push_back(c.get());
    }
    return out;
}

std::string XmlNode::childText(const std::string& n) const {
    const XmlNode* c = child(n);
    return c ? trim(c->text) : std::string();
}

std::string XmlNode::attr(const std::string& n) const {
    auto it = attrs.find(n);
    return it == attrs.end() ? std::string() : it->second;
}

namespace {

std::string decodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        unsigned cp = 0;
        if (ent == "amp") cp = '&';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "quot") cp = '"';
        else if (ent == "apos") cp = '\'';
        else if (!ent.empty() && ent[0] == '#') cp = static_cast<unsigned>(ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? std::strtoul(ent.c_str() + 2, nullptr, 16) : std::strtoul(ent.c_str() + 1, nullptr, 10));
        if (!cp) {
            out += s[i];
            continue;
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        i = semi;
    }
    return out;
}

std::string localName(const std::string& n) {
    const auto c = n.find(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

}  // namespace

std::unique_ptr<XmlNode> parseXml(const std::string& s) {
    auto root = std::make_unique<XmlNode>();
    root->name = "#document";
    std::vector<XmlNode*> stack{root.get()};
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            const auto e = s.find('<', i);
            stack.back()->text += decodeEntities(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
            if (e == std::string::npos) break;
            i = e;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) {
            const auto e = s.find("-->", i);
            i = e == std::string::npos ? s.size() : e + 3;
            continue;
        }
        if (s.compare(i, 9, "<![CDATA[") == 0) {
            const auto e = s.find("]]>", i);
            stack.back()->text += s.substr(i + 9, (e == std::string::npos ? s.size() : e) - i - 9);
            i = e == std::string::npos ? s.size() : e + 3;
            continue;
        }
        if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
            const auto e = s.find('>', i);
            i = e == std::string::npos ? s.size() : e + 1;
            continue;
        }
        if (s.compare(i, 2, "</") == 0) {
            const auto e = s.find('>', i);
            if (stack.size() > 1) stack.pop_back();
            i = e == std::string::npos ? s.size() : e + 1;
            continue;
        }
        // start tag
        std::size_t j = i + 1;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>' && s[j] != '/') ++j;
        auto node = std::make_unique<XmlNode>();
        node->name = localName(s.substr(i + 1, j - i - 1));
        bool selfClose = false;
        while (j < s.size()) {
            while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
            if (j >= s.size()) break;
            if (s[j] == '>') {
                ++j;
                break;
            }
            if (s[j] == '/') {
                selfClose = true;
                ++j;
                continue;
            }
            std::size_t k = j;
            while (k < s.size() && s[k] != '=' && !std::isspace(static_cast<unsigned char>(s[k])) && s[k] != '>' && s[k] != '/') ++k;
            const std::string an = localName(s.substr(j, k - j));
            while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
            std::string av;
            if (k < s.size() && s[k] == '=') {
                ++k;
                while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
                if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {
                    const char q = s[k];
                    const auto e = s.find(q, k + 1);
                    if (e == std::string::npos) throw std::runtime_error("XML: unterminated attribute value");
                    av = decodeEntities(s.substr(k + 1, e - k - 1));
                    k = e + 1;
                }
            }
            node->attrs[an] = av;
            j = k;
        }
        XmlNode* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClose) stack.push_back(raw);
        i = j;
    }
    return root;
}

std::vector<Polygon> assembleRings(std::vector<Ring> rings) {
    // a ring inside an odd number of others is a hole of the innermost one containing it
    std::vector<int> depth(rings.size(), 0), parent(rings.size(), -1);
    std::vector<double> areaAbs(rings.size());
    for (std::size_t i = 0; i < rings.size(); ++i) areaAbs[i] = std::abs(signedArea(rings[i]));
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (rings[i].empty()) continue;
        for (std::size_t k = 0; k < rings.size(); ++k) {
            if (k == i || areaAbs[k] <= areaAbs[i] || rings[k].size() < 3) continue;
            if (pointInRing(rings[k], rings[i][0].x, rings[i][0].y)) {
                ++depth[i];
                if (parent[i] < 0 || areaAbs[k] < areaAbs[static_cast<std::size_t>(parent[i])]) parent[i] = static_cast<int>(k);
            }
        }
    }
    std::vector<Polygon> out;
    std::map<std::size_t, std::size_t> polyOf;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (depth[i] % 2 == 0 && rings[i].size() >= 3) {
            polyOf[i] = out.size();
            out.push_back({rings[i]});
        }
    }
    for (std::size_t i = 0; i < rings.size(); ++i) {
        if (depth[i] % 2 == 1 && parent[i] >= 0) {
            auto it = polyOf.find(static_cast<std::size_t>(parent[i]));
            if (it != polyOf.end()) out[it->second].push_back(rings[i]);
        }
    }
    return out;
}

}  // namespace detail

using namespace detail;

namespace {

void dropClosing(Ring& r) {
    if (r.size() > 1 && r.front().x == r.back().x && r.front().y == r.back().y) r.pop_back();
}

void normalizeOrientation(Polygon& p, bool outerCcw) {
    for (std::size_t i = 0; i < p.size(); ++i) {
        const bool ccw = signedArea(p[i]) > 0;
        const bool want = i == 0 ? outerCcw : !outerCcw;
        if (ccw != want) std::reverse(p[i].begin(), p[i].end());
    }
}

void toWgs(Geometry& g, const std::optional<Crs>& crs) {
    if (!crs || crs->geographic()) return;
    g.forEach([&](Coord& c) {
        const Coord ll = crs->inverse(c);
        c.x = ll.x;
        c.y = ll.y;
    });
}

Prop fromJson(const json& v) {
    if (v.is_null()) return Prop::null();
    if (v.is_boolean()) return Prop::boolean(v.get<bool>());
    if (v.is_number()) return Prop::number(v.get<double>());
    if (v.is_string()) return Prop::string(v.get<std::string>());
    return Prop::string(v.dump());
}

json toJson(const Prop& p) {
    switch (p.kind) {
        case Prop::Bool: return p.num != 0;
        case Prop::Number:
            if (std::abs(p.num - std::round(p.num)) < 1e-12 && std::abs(p.num) < 9e15) return static_cast<long long>(std::llround(p.num));
            return p.num;
        case Prop::String: return p.str;
        default: return nullptr;
    }
}

Coord coordFromJson(const json& a) {
    if (!a.is_array() || a.size() < 2 || !a[0].is_number() || !a[1].is_number()) throw std::runtime_error("GeoJSON: a position needs [x, y]");
    Coord c{a[0].get<double>(), a[1].get<double>()};
    if (a.size() > 2 && a[2].is_number()) c.z = a[2].get<double>();
    return c;
}

Line lineFromJson(const json& a) {
    Line l;
    if (!a.is_array()) throw std::runtime_error("GeoJSON: expected an array of positions");
    for (const auto& p : a) l.push_back(coordFromJson(p));
    return l;
}

Polygon polygonFromJson(const json& a) {
    Polygon p;
    if (!a.is_array()) throw std::runtime_error("GeoJSON: expected an array of rings");
    for (const auto& r : a) {
        Ring ring = lineFromJson(r);
        dropClosing(ring);
        if (ring.size() >= 3) p.push_back(std::move(ring));
    }
    return p;
}

// One GeoJSON geometry; GeometryCollections give several.
void geometriesFromJson(const json& g, std::vector<Geometry>& out) {
    if (g.is_null()) {
        out.push_back({});
        return;
    }
    const std::string type = g.value("type", "");
    const json& c = g.contains("coordinates") ? g["coordinates"] : json();
    Geometry geom;
    if (type == "Point") {
        geom.type = GeomType::Point;
        if (c.is_array() && !c.empty()) geom.points.push_back(coordFromJson(c));
    } else if (type == "MultiPoint") {
        geom.type = GeomType::Point;
        geom.points = lineFromJson(c);
    } else if (type == "LineString") {
        geom.type = GeomType::Line;
        if (c.is_array() && !c.empty()) geom.lines.push_back(lineFromJson(c));
    } else if (type == "MultiLineString") {
        geom.type = GeomType::Line;
        for (const auto& l : c) geom.lines.push_back(lineFromJson(l));
    } else if (type == "Polygon") {
        geom.type = GeomType::Polygon;
        Polygon p = polygonFromJson(c);
        if (!p.empty()) geom.polygons.push_back(std::move(p));
    } else if (type == "MultiPolygon") {
        geom.type = GeomType::Polygon;
        for (const auto& p : c) {
            Polygon poly = polygonFromJson(p);
            if (!poly.empty()) geom.polygons.push_back(std::move(poly));
        }
    } else if (type == "GeometryCollection") {
        for (const auto& sub : g.value("geometries", json::array())) geometriesFromJson(sub, out);
        return;
    } else {
        throw std::runtime_error("GeoJSON: unknown geometry type '" + type + "'");
    }
    if (geom.empty()) geom.type = GeomType::None;
    out.push_back(std::move(geom));
}

std::optional<Crs> crsMember(const json& j) {
    if (!j.contains("crs") || !j["crs"].is_object()) return std::nullopt;
    const json& c = j["crs"];
    if (c.contains("properties") && c["properties"].is_object() && c["properties"].contains("name") && c["properties"]["name"].is_string())
        return Crs::parse(c["properties"]["name"].get<std::string>());
    return std::nullopt;
}

}  // namespace

std::vector<Feature> parseGeoJson(const std::string& text, const std::optional<Crs>& crsOverride) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    std::optional<Crs> crs = crsOverride ? crsOverride : crsMember(j);
    std::vector<Feature> out;
    auto addFeature = [&](const json& f) {
        Props props;
        if (f.contains("properties") && f["properties"].is_object()) {
            for (const auto& [k, v] : f["properties"].items()) {
                if (k == "coordTimes" || k == "coordinateProperties") continue;
                props[k] = fromJson(v);
            }
        }
        if (f.contains("id") && !props.count("id")) props["id"] = fromJson(f["id"]);
        std::vector<Geometry> gs;
        geometriesFromJson(f.contains("geometry") ? f["geometry"] : json(), gs);
        // per-vertex times (togeojson's coordTimes / coordinateProperties.times)
        json times;
        if (f.contains("properties") && f["properties"].is_object()) {
            const json& p = f["properties"];
            if (p.contains("coordTimes")) times = p["coordTimes"];
            else if (p.contains("coordinateProperties") && p["coordinateProperties"].is_object() && p["coordinateProperties"].contains("times"))
                times = p["coordinateProperties"]["times"];
        }
        for (auto& g : gs) {
            if (times.is_array() && g.type == GeomType::Line) {
                const bool nested = !times.empty() && times[0].is_array();
                for (std::size_t li = 0; li < g.lines.size(); ++li) {
                    const json& ts = nested ? (li < times.size() ? times[li] : json()) : (li == 0 ? times : json());
                    if (!ts.is_array()) continue;
                    for (std::size_t k = 0; k < g.lines[li].size() && k < ts.size(); ++k) {
                        if (ts[k].is_string()) g.lines[li][k].t = parseTime(ts[k].get<std::string>());
                    }
                }
            }
            toWgs(g, crs);
            out.push_back({std::move(g), props});
        }
    };
    const std::string type = j.value("type", "");
    if (type == "FeatureCollection") {
        if (!j.contains("features") || !j["features"].is_array()) throw std::runtime_error("GeoJSON: FeatureCollection without features");
        for (const auto& f : j["features"]) addFeature(f);
    } else if (type == "Feature") {
        addFeature(j);
    } else if (!type.empty()) {
        json f = {{"type", "Feature"}, {"geometry", j}, {"properties", json::object()}};
        addFeature(f);
    } else if (j.is_array()) {
        for (const auto& f : j) addFeature(f);
    } else {
        throw std::runtime_error("not GeoJSON (no \"type\")");
    }
    return out;
}

namespace {

std::string posText(const Coord& c, int prec) {
    std::string s = "[" + fmt(c.x, prec) + "," + fmt(c.y, prec);
    if (!std::isnan(c.z)) s += "," + fmt(c.z, 2);
    return s + "]";
}

std::string lineText(const std::vector<Coord>& l, int prec, bool close) {
    std::string s = "[";
    for (std::size_t i = 0; i < l.size(); ++i) s += (i ? "," : "") + posText(l[i], prec);
    if (close && !l.empty()) s += "," + posText(l.front(), prec);
    return s + "]";
}

std::string polyText(Polygon p, int prec) {
    normalizeOrientation(p, true);
    std::string s = "[";
    for (std::size_t i = 0; i < p.size(); ++i) s += (i ? "," : "") + lineText(p[i], prec, true);
    return s + "]";
}

std::string geometryJson(const Geometry& g0, const ExportOptions& o) {
    Geometry g = g0;
    const int prec = o.crs.geographic() ? o.precision : 3;
    if (!o.crs.geographic()) {
        g.forEach([&](Coord& c) {
            const Coord p = o.crs.forward(c);
            c.x = p.x;
            c.y = p.y;
        });
    }
    switch (g.type) {
        case GeomType::Point:
            if (g.points.size() == 1) return "{\"type\":\"Point\",\"coordinates\":" + posText(g.points[0], prec) + "}";
            return "{\"type\":\"MultiPoint\",\"coordinates\":" + lineText(g.points, prec, false) + "}";
        case GeomType::Line: {
            if (g.lines.size() == 1) return "{\"type\":\"LineString\",\"coordinates\":" + lineText(g.lines[0], prec, false) + "}";
            std::string s = "{\"type\":\"MultiLineString\",\"coordinates\":[";
            for (std::size_t i = 0; i < g.lines.size(); ++i) s += (i ? "," : "") + lineText(g.lines[i], prec, false);
            return s + "]}";
        }
        case GeomType::Polygon: {
            if (g.polygons.size() == 1) return "{\"type\":\"Polygon\",\"coordinates\":" + polyText(g.polygons[0], prec) + "}";
            std::string s = "{\"type\":\"MultiPolygon\",\"coordinates\":[";
            for (std::size_t i = 0; i < g.polygons.size(); ++i) s += (i ? "," : "") + polyText(g.polygons[i], prec);
            return s + "]}";
        }
        default: return "null";
    }
}

}  // namespace

std::string toGeoJson(const std::vector<Feature>& features, const ExportOptions& o) {
    std::string s = "{\"type\":\"FeatureCollection\",";
    if (!o.crs.geographic()) s += "\"crs\":{\"type\":\"name\",\"properties\":{\"name\":\"urn:ogc:def:crs:EPSG::" + std::to_string(o.crs.epsg()) + "\"}},";
    s += "\"features\":[\n";
    for (std::size_t i = 0; i < features.size(); ++i) {
        const Feature& f = features[i];
        json props = json::object();
        for (const auto& [k, v] : f.props) props[k] = toJson(v);
        // keep per-vertex times of tracks
        if (f.geom.type == GeomType::Line) {
            bool any = false;
            json times = json::array();
            for (const auto& l : f.geom.lines) {
                json ts = json::array();
                for (const auto& c : l) {
                    if (!std::isnan(c.t)) any = true;
                    ts.push_back(std::isnan(c.t) ? json() : json(formatTime(c.t)));
                }
                times.push_back(ts);
            }
            if (any) props["coordTimes"] = f.geom.lines.size() == 1 ? times[0] : times;
        }
        s += std::string(i ? ",\n" : "") + "{\"type\":\"Feature\",\"properties\":" + props.dump() + ",\"geometry\":" + geometryJson(f.geom, o) + "}";
    }
    return s + "\n]}\n";
}

// ---- WKT ------------------------------------------------------------------------------------

namespace {

class WktReader {
public:
    explicit WktReader(const std::string& s) : s_(s) {}

    Geometry read() {
        skipSrid();
        std::string type = word();
        std::string dims = peekWord();
        bool hasZ = false, hasM = false;
        if (dims == "z" || dims == "m" || dims == "zm") {
            word();
            hasZ = dims != "m";
            hasM = dims.find('m') != std::string::npos;
        }
        if (type.size() > 2 && type.compare(type.size() - 2, 2, "zm") == 0) {
            type.resize(type.size() - 2);
            hasZ = hasM = true;
        } else if (!type.empty() && type.back() == 'z') {
            type.pop_back();
            hasZ = true;
        } else if (!type.empty() && type.back() == 'm') {
            type.pop_back();
            hasM = true;
        }
        z_ = hasZ;
        m_ = hasM;
        Geometry g;
        if (peekWord() == "empty") {
            word();
            return g;
        }
        if (type == "point") {
            g.type = GeomType::Point;
            expect('(');
            g.points.push_back(coord());
            expect(')');
        } else if (type == "multipoint") {
            g.type = GeomType::Point;
            expect('(');
            do {
                skipWs();
                if (peek() == '(') {
                    expect('(');
                    g.points.push_back(coord());
                    expect(')');
                } else {
                    g.points.push_back(coord());
                }
            } while (accept(','));
            expect(')');
        } else if (type == "linestring") {
            g.type = GeomType::Line;
            g.lines.push_back(coordList());
        } else if (type == "multilinestring") {
            g.type = GeomType::Line;
            expect('(');
            do g.lines.push_back(coordList());
            while (accept(','));
            expect(')');
        } else if (type == "polygon") {
            g.type = GeomType::Polygon;
            g.polygons.push_back(polygon());
        } else if (type == "multipolygon") {
            g.type = GeomType::Polygon;
            expect('(');
            do g.polygons.push_back(polygon());
            while (accept(','));
            expect(')');
        } else {
            throw std::runtime_error("WKT: unsupported geometry '" + type + "'");
        }
        return g;
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;
    bool z_ = false, m_ = false;

    void skipWs() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    char peek() {
        skipWs();
        return i_ < s_.size() ? s_[i_] : '\0';
    }
    bool accept(char c) {
        if (peek() == c) {
            ++i_;
            return true;
        }
        return false;
    }
    void expect(char c) {
        if (!accept(c)) throw std::runtime_error(std::string("WKT: expected '") + c + "' at position " + std::to_string(i_ + 1));
    }
    void skipSrid() {
        skipWs();
        if (lower(s_.substr(i_, 5)) == "srid=") {
            const auto semi = s_.find(';', i_);
            i_ = semi == std::string::npos ? s_.size() : semi + 1;
        }
    }
    std::string word() {
        skipWs();
        std::size_t j = i_;
        while (j < s_.size() && std::isalpha(static_cast<unsigned char>(s_[j]))) ++j;
        std::string w = lower(s_.substr(i_, j - i_));
        i_ = j;
        return w;
    }
    std::string peekWord() {
        const std::size_t save = i_;
        std::string w = word();
        i_ = save;
        return w;
    }
    double number() {
        skipWs();
        char* end = nullptr;
        const double v = std::strtod(s_.c_str() + i_, &end);
        if (end == s_.c_str() + i_) throw std::runtime_error("WKT: expected a number at position " + std::to_string(i_ + 1));
        i_ = static_cast<std::size_t>(end - s_.c_str());
        return v;
    }
    Coord coord() {
        Coord c{number(), number()};
        // count remaining numbers before , or )
        std::vector<double> extra;
        while (true) {
            const char p = peek();
            if (p == ',' || p == ')' || p == '\0') break;
            extra.push_back(number());
        }
        if (!extra.empty() && (z_ || !m_)) c.z = extra[0];
        return c;
    }
    Line coordList() {
        expect('(');
        Line l;
        do l.push_back(coord());
        while (accept(','));
        expect(')');
        return l;
    }
    Polygon polygon() {
        expect('(');
        Polygon p;
        do {
            Ring r = coordList();
            dropClosing(r);
            if (r.size() >= 3) p.push_back(std::move(r));
        } while (accept(','));
        expect(')');
        return p;
    }
};

std::string wktCoord(const Coord& c, int prec) {
    std::string s = fmt(c.x, prec) + " " + fmt(c.y, prec);
    if (!std::isnan(c.z)) s += " " + fmt(c.z, 2);
    return s;
}

std::string wktList(const std::vector<Coord>& l, int prec, bool close) {
    std::string s = "(";
    for (std::size_t i = 0; i < l.size(); ++i) s += (i ? ", " : "") + wktCoord(l[i], prec);
    if (close && !l.empty()) s += ", " + wktCoord(l.front(), prec);
    return s + ")";
}

std::string wktPoly(const Polygon& p, int prec) {
    std::string s = "(";
    for (std::size_t i = 0; i < p.size(); ++i) s += (i ? ", " : "") + wktList(p[i], prec, true);
    return s + ")";
}

}  // namespace

Geometry parseWkt(const std::string& text) {
    WktReader r(text);
    Geometry g = r.read();
    if (g.empty()) g.type = GeomType::None;
    return g;
}

std::string toWkt(const Geometry& g, int prec) {
    bool z = false;
    g.forEach([&](const Coord& c) { z = z || !std::isnan(c.z); });
    const std::string zs = z ? " Z" : "";
    switch (g.type) {
        case GeomType::Point: {
            if (g.points.size() == 1) return "POINT" + zs + " (" + wktCoord(g.points[0], prec) + ")";
            std::string s = "MULTIPOINT" + zs + " (";
            for (std::size_t i = 0; i < g.points.size(); ++i) s += (i ? ", " : "") + std::string("(") + wktCoord(g.points[i], prec) + ")";
            return s + ")";
        }
        case GeomType::Line: {
            if (g.lines.size() == 1) return "LINESTRING" + zs + " " + wktList(g.lines[0], prec, false);
            std::string s = "MULTILINESTRING" + zs + " (";
            for (std::size_t i = 0; i < g.lines.size(); ++i) s += (i ? ", " : "") + wktList(g.lines[i], prec, false);
            return s + ")";
        }
        case GeomType::Polygon: {
            if (g.polygons.size() == 1) return "POLYGON" + zs + " " + wktPoly(g.polygons[0], prec);
            std::string s = "MULTIPOLYGON" + zs + " (";
            for (std::size_t i = 0; i < g.polygons.size(); ++i) s += (i ? ", " : "") + wktPoly(g.polygons[i], prec);
            return s + ")";
        }
        default: return "GEOMETRYCOLLECTION EMPTY";
    }
}

// ---- KML ------------------------------------------------------------------------------------

namespace {

std::vector<Coord> kmlCoords(const std::string& text) {
    std::vector<Coord> out;
    std::istringstream in(text);
    std::string tuple;
    while (in >> tuple) {
        double v[3] = {0, 0, kNaN};
        int n = 0;
        std::size_t start = 0;
        while (n < 3 && start <= tuple.size()) {
            const auto comma = tuple.find(',', start);
            const std::string part = tuple.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!part.empty()) v[n] = std::strtod(part.c_str(), nullptr);
            ++n;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (n >= 2) {
            Coord c{v[0], v[1]};
            if (n >= 3) c.z = v[2];
            out.push_back(c);
        }
    }
    return out;
}

void kmlGeometry(const XmlNode& n, std::vector<Geometry>& out) {
    auto ringOf = [](const XmlNode* boundary) -> Ring {
        if (!boundary) return {};
        const XmlNode* lr = boundary->child("LinearRing");
        Ring r = kmlCoords(lr ? lr->childText("coordinates") : boundary->childText("coordinates"));
        dropClosing(r);
        return r;
    };
    if (n.name == "Point") {
        auto cs = kmlCoords(n.childText("coordinates"));
        if (!cs.empty()) {
            Geometry g;
            g.type = GeomType::Point;
            g.points.push_back(cs[0]);
            out.push_back(g);
        }
    } else if (n.name == "LineString" || n.name == "LinearRing") {
        auto cs = kmlCoords(n.childText("coordinates"));
        if (cs.size() >= 2) out.push_back(Geometry::line(cs));
    } else if (n.name == "Polygon") {
        Polygon p;
        Ring outer = ringOf(n.child("outerBoundaryIs"));
        if (outer.size() < 3) return;
        p.push_back(outer);
        for (const XmlNode* ib : n.all("innerBoundaryIs")) {
            for (const XmlNode* lr : ib->all("LinearRing")) {
                Ring r = kmlCoords(lr->childText("coordinates"));
                dropClosing(r);
                if (r.size() >= 3) p.push_back(r);
            }
        }
        out.push_back(Geometry::polygon(p));
    } else if (n.name == "Track") {
        Line l;
        auto whens = n.all("when");
        auto coords = n.all("coord");
        for (std::size_t i = 0; i < coords.size(); ++i) {
            std::istringstream in(coords[i]->text);
            Coord c;
            double z = kNaN;
            in >> c.x >> c.y;
            if (in >> z) c.z = z;
            if (i < whens.size()) c.t = parseTime(whens[i]->text);
            l.push_back(c);
        }
        if (l.size() >= 2) out.push_back(Geometry::line(l));
    } else if (n.name == "MultiTrack") {
        for (const auto& c : n.children) kmlGeometry(*c, out);
    } else if (n.name == "MultiGeometry") {
        std::vector<Geometry> parts;
        for (const auto& c : n.children) kmlGeometry(*c, parts);
        // merge parts of the same type into one multi-geometry
        std::map<GeomType, Geometry> merged;
        for (auto& g : parts) {
            Geometry& m = merged[g.type];
            m.type = g.type;
            m.points.insert(m.points.end(), g.points.begin(), g.points.end());
            m.lines.insert(m.lines.end(), g.lines.begin(), g.lines.end());
            m.polygons.insert(m.polygons.end(), g.polygons.begin(), g.polygons.end());
        }
        for (auto& [t, g] : merged) out.push_back(std::move(g));
    }
}

void kmlWalk(const XmlNode& n, const std::string& folder, std::vector<Feature>& out, std::size_t& skipped) {
    for (const auto& c : n.children) {
        if (c->name == "Folder" || c->name == "Document" || c->name == "kml") {
            const std::string name = c->childText("name");
            kmlWalk(*c, c->name == "Folder" ? (folder.empty() ? name : folder + "/" + name) : folder, out, skipped);
        } else if (c->name == "Placemark") {
            Props props;
            if (auto nm = c->childText("name"); !nm.empty()) props["name"] = Prop::string(nm);
            if (auto d = c->childText("description"); !d.empty()) props["description"] = Prop::string(d);
            if (!folder.empty()) props["folder"] = Prop::string(folder);
            if (const XmlNode* ext = c->child("ExtendedData")) {
                for (const XmlNode* d : ext->all("Data")) props[d->attr("name")] = Prop::infer(d->childText("value"));
                for (const XmlNode* sd : ext->all("SchemaData")) {
                    for (const XmlNode* s : sd->all("SimpleData")) props[s->attr("name")] = Prop::infer(trim(s->text));
                }
            }
            if (auto when = c->child("TimeStamp")) props["time"] = Prop::string(when->childText("when"));
            std::vector<Geometry> gs;
            for (const auto& g : c->children) kmlGeometry(*g, gs);
            if (gs.empty()) ++skipped;
            for (auto& g : gs) out.push_back({std::move(g), props});
        }
    }
}

std::string kmlColor(Color c, double alpha) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x", static_cast<unsigned>(std::lround(std::clamp(alpha, 0.0, 1.0) * c.a)), c.b, c.g, c.r);
    return buf;
}

std::string kmlCoordText(const std::vector<Coord>& cs, bool close) {
    std::string s;
    auto one = [&](const Coord& c) {
        s += (s.empty() ? "" : " ") + fmt(c.x, 7) + "," + fmt(c.y, 7);
        if (!std::isnan(c.z)) s += "," + fmt(c.z, 2);
    };
    for (const auto& c : cs) one(c);
    if (close && !cs.empty()) one(cs.front());
    return s;
}

}  // namespace

std::vector<Feature> parseKml(const std::string& text, std::vector<std::string>* warnings) {
    auto root = parseXml(text);
    std::vector<Feature> out;
    std::size_t skipped = 0;
    kmlWalk(*root, "", out, skipped);
    if (skipped && warnings) warnings->push_back(std::to_string(skipped) + " placemarks without a supported geometry were skipped");
    return out;
}

std::string toKml(const Layer& layer) {
    const Style& st = layer.style;
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n<name>" + xmlEscape(layer.name) +
                    "</name>\n<Style id=\"s\"><LineStyle><color>" + kmlColor(st.stroke, st.opacity) + "</color><width>" + fmt(st.width, 2) +
                    "</width></LineStyle><PolyStyle><color>" + kmlColor(st.fill, st.fillOpacity * st.opacity) + "</color></PolyStyle><IconStyle><color>" +
                    kmlColor(st.fill, st.opacity) + "</color></IconStyle></Style>\n";
    const std::string nameField = !st.labelField.empty() ? st.labelField : "name";
    for (const auto& f : layer.features) {
        if (f.geom.empty()) continue;
        s += "<Placemark>";
        if (auto it = f.props.find(nameField); it != f.props.end()) s += "<name>" + xmlEscape(it->second.text()) + "</name>";
        if (auto it = f.props.find("description"); it != f.props.end()) s += "<description>" + xmlEscape(it->second.text()) + "</description>";
        s += "<styleUrl>#s</styleUrl>";
        if (!f.props.empty()) {
            s += "<ExtendedData>";
            for (const auto& [k, v] : f.props) s += "<Data name=\"" + xmlEscape(k) + "\"><value>" + xmlEscape(v.text()) + "</value></Data>";
            s += "</ExtendedData>";
        }
        std::vector<std::string> parts;
        for (const auto& p : f.geom.points) parts.push_back("<Point><coordinates>" + kmlCoordText({p}, false) + "</coordinates></Point>");
        for (const auto& l : f.geom.lines) parts.push_back("<LineString><tessellate>1</tessellate><coordinates>" + kmlCoordText(l, false) + "</coordinates></LineString>");
        for (auto p : f.geom.polygons) {
            normalizeOrientation(p, true);
            std::string g = "<Polygon><outerBoundaryIs><LinearRing><coordinates>" + kmlCoordText(p[0], true) + "</coordinates></LinearRing></outerBoundaryIs>";
            for (std::size_t i = 1; i < p.size(); ++i)
                g += "<innerBoundaryIs><LinearRing><coordinates>" + kmlCoordText(p[i], true) + "</coordinates></LinearRing></innerBoundaryIs>";
            parts.push_back(g + "</Polygon>");
        }
        if (parts.size() == 1) {
            s += parts[0];
        } else {
            s += "<MultiGeometry>";
            for (const auto& p : parts) s += p;
            s += "</MultiGeometry>";
        }
        s += "</Placemark>\n";
    }
    return s + "</Document>\n</kml>\n";
}

// ---- GPX ------------------------------------------------------------------------------------

namespace {

Coord gpxPoint(const XmlNode& n) {
    Coord c{std::strtod(n.attr("lon").c_str(), nullptr), std::strtod(n.attr("lat").c_str(), nullptr)};
    if (const XmlNode* e = n.child("ele")) c.z = std::strtod(trim(e->text).c_str(), nullptr);
    if (const XmlNode* t = n.child("time")) c.t = parseTime(t->text);
    return c;
}

Props gpxProps(const XmlNode& n) {
    Props p;
    for (const char* k : {"name", "desc", "cmt", "type", "sym", "src", "number"}) {
        if (auto v = n.childText(k); !v.empty()) p[k] = Prop::infer(v);
    }
    return p;
}

}  // namespace

GpxData parseGpx(const std::string& text) {
    auto root = parseXml(text);
    const XmlNode* gpx = root->child("gpx");
    if (!gpx) throw std::runtime_error("not a GPX file (no <gpx> element)");
    GpxData out;
    for (const XmlNode* w : gpx->all("wpt")) {
        Feature f;
        f.geom.type = GeomType::Point;
        f.geom.points.push_back(gpxPoint(*w));
        f.props = gpxProps(*w);
        if (!std::isnan(f.geom.points[0].z)) f.props["ele"] = Prop::number(f.geom.points[0].z);
        if (auto t = w->childText("time"); !t.empty()) f.props["time"] = Prop::string(t);
        out.waypoints.push_back(std::move(f));
    }
    for (const XmlNode* r : gpx->all("rte")) {
        Line l;
        for (const XmlNode* p : r->all("rtept")) l.push_back(gpxPoint(*p));
        if (l.size() < 2) continue;
        out.routes.push_back({Geometry::line(l), gpxProps(*r)});
    }
    for (const XmlNode* t : gpx->all("trk")) {
        Feature f;
        f.geom.type = GeomType::Line;
        for (const XmlNode* seg : t->all("trkseg")) {
            Line l;
            for (const XmlNode* p : seg->all("trkpt")) l.push_back(gpxPoint(*p));
            if (l.size() >= 2) f.geom.lines.push_back(std::move(l));
        }
        if (f.geom.lines.empty()) continue;
        f.props = gpxProps(*t);
        out.tracks.push_back(std::move(f));
    }
    return out;
}

std::string toGpx(const Layer& layer) {
    std::string s =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"ACHCLWSS maps\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
    s += "<metadata><name>" + xmlEscape(layer.name) + "</name></metadata>\n";
    auto pointBody = [](const Coord& c) {
        std::string b;
        if (!std::isnan(c.z)) b += "<ele>" + fmt(c.z, 2) + "</ele>";
        if (!std::isnan(c.t)) b += "<time>" + formatTime(c.t) + "</time>";
        return b;
    };
    auto attrs = [](const Coord& c) { return " lat=\"" + fmt(c.y, 7) + "\" lon=\"" + fmt(c.x, 7) + "\""; };
    auto meta = [](const Props& p) {
        std::string b;
        for (const char* k : {"name", "cmt", "desc", "sym", "type"}) {
            if (auto it = p.find(k); it != p.end() && !it->second.isNull()) b += std::string("<") + k + ">" + xmlEscape(it->second.text()) + "</" + k + ">";
        }
        return b;
    };
    for (const auto& f : layer.features) {
        for (const auto& c : f.geom.points) {
            Coord p = c;
            if (std::isnan(p.z)) {
                if (auto it = f.props.find("ele"); it != f.props.end() && it->second.asNumber()) p.z = *it->second.asNumber();
            }
            s += "<wpt" + attrs(p) + ">" + pointBody(p) + meta(f.props) + "</wpt>\n";
        }
    }
    for (const auto& f : layer.features) {
        std::vector<const Line*> lines;
        for (const auto& l : f.geom.lines) lines.push_back(&l);
        for (const auto& p : f.geom.polygons) {
            if (!p.empty()) lines.push_back(&p[0]);
        }
        if (lines.empty()) continue;
        s += "<trk>" + meta(f.props) + "\n";
        for (const Line* l : lines) {
            s += "<trkseg>\n";
            for (const auto& c : *l) s += "<trkpt" + attrs(c) + ">" + pointBody(c) + "</trkpt>\n";
            s += "</trkseg>\n";
        }
        s += "</trk>\n";
    }
    return s + "</gpx>\n";
}

// ---- CSV ------------------------------------------------------------------------------------

Table readTable(const std::string& text0, const std::string& delimiter) {
    std::string text = text0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    if (!validUtf8(text)) text = latin1ToUtf8(text);
    char sep = ',';
    if (!delimiter.empty()) {
        sep = delimiter == "tab" || delimiter == "\\t" ? '\t' : delimiter == "semicolon" ? ';' : delimiter == "comma" ? ',' : delimiter == "pipe" ? '|' : delimiter[0];
    } else {
        const std::string head = text.substr(0, text.find('\n'));
        std::size_t best = 0;
        for (char c : {',', ';', '\t', '|'}) {
            const auto n = static_cast<std::size_t>(std::count(head.begin(), head.end(), c));
            if (n > best) {
                best = n;
                sep = c;
            }
        }
    }
    Table t;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false, any = false;
    auto endRow = [&] {
        row.push_back(cell);
        cell.clear();
        if (!(row.size() == 1 && row[0].empty())) {
            if (t.header.empty()) t.header = row;
            else t.rows.push_back(row);
        }
        row.clear();
        any = false;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell += c;
            }
            continue;
        }
        if (c == '"' && cell.empty()) {
            quoted = true;
            any = true;
        } else if (c == sep) {
            row.push_back(cell);
            cell.clear();
            any = true;
        } else if (c == '\n') {
            endRow();
        } else if (c != '\r') {
            cell += c;
            any = true;
        }
    }
    if (any || !cell.empty()) endRow();
    if (t.header.empty()) throw std::runtime_error("the CSV is empty");
    for (auto& h : t.header) h = trim(h);
    return t;
}

namespace {

int findColumn(const std::vector<std::string>& header, const std::string& wanted, const std::vector<std::string>& guesses) {
    if (!wanted.empty()) {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (header[i] == wanted || lower(header[i]) == lower(wanted)) return static_cast<int>(i);
        }
        throw std::runtime_error("no column called '" + wanted + "' (columns: " + [&] {
            std::string s;
            for (const auto& h : header) s += (s.empty() ? "" : ", ") + h;
            return s;
        }() + ")");
    }
    for (const auto& g : guesses) {
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (lower(header[i]) == g) return static_cast<int>(i);
        }
    }
    return -1;
}

std::string csvCell(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    return out + "\"";
}

// "12.5", "12,5", "12°30'N" style values -> degrees
std::optional<double> parseCoordCell(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) return std::nullopt;
    double sign = 1;
    if (!s.empty()) {
        const char h = static_cast<char>(std::toupper(static_cast<unsigned char>(s.back())));
        if (h == 'S' || h == 'W' || h == 'N' || h == 'E') {
            if (h == 'S' || h == 'W') sign = -1;
            s.pop_back();
            s = trim(s);
        }
    }
    if (s.find(',') != std::string::npos && s.find('.') == std::string::npos) std::replace(s.begin(), s.end(), ',', '.');
    // degrees minutes seconds
    if (s.find_first_of("\xC2\xB0'\" ") != std::string::npos && s.find_first_of("\xC2\xB0'\"") != std::string::npos) {
        double parts[3] = {0, 0, 0};
        int n = 0;
        std::string num;
        for (char c : s + " ") {
            if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-') {
                num += c;
            } else if (!num.empty()) {
                if (n < 3) parts[n++] = std::strtod(num.c_str(), nullptr);
                num.clear();
            }
        }
        if (n == 0) return std::nullopt;
        const double deg = std::abs(parts[0]) + parts[1] / 60 + parts[2] / 3600;
        return sign * (parts[0] < 0 ? -deg : deg);
    }
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return std::nullopt;
    return sign * v;
}

}  // namespace

std::vector<Feature> parseCsv(const std::string& text, const ImportOptions& o, std::vector<std::string>* warnings) {
    Table t = readTable(text, o.delimiter);
    const bool projected = o.crs && !o.crs->geographic();
    const int wktCol = findColumn(t.header, o.wktColumn, {"wkt", "geometry", "geom", "the_geom", "shape", "wkt_geom"});
    int latCol = -1, lonCol = -1;
    if (wktCol < 0 || !o.latColumn.empty() || !o.lonColumn.empty()) {
        latCol = findColumn(t.header, o.latColumn,
                            projected ? std::vector<std::string>{"y", "northing", "north", "lat", "latitude"}
                                      : std::vector<std::string>{"lat", "latitude", "lat_dd", "latitude_dd", "y", "ycoord", "y_coord", "breite", "point_y"});
        lonCol = findColumn(t.header, o.lonColumn,
                            projected ? std::vector<std::string>{"x", "easting", "east", "lon", "longitude"}
                                      : std::vector<std::string>{"lon", "lng", "long", "longitude", "lon_dd", "longitude_dd", "x", "xcoord", "x_coord",
                                                                 "laenge", "point_x"});
    }
    if (wktCol < 0 && (latCol < 0 || lonCol < 0))
        throw std::runtime_error("no coordinate columns found: name them lat/lon (or pass lat= and lon=, or a wkt column)");
    std::vector<Feature> out;
    std::size_t bad = 0;
    for (const auto& row : t.rows) {
        Feature f;
        for (std::size_t i = 0; i < t.header.size(); ++i) {
            if (static_cast<int>(i) == latCol || static_cast<int>(i) == lonCol || static_cast<int>(i) == wktCol) continue;
            if (t.header[i].empty()) continue;
            f.props[t.header[i]] = Prop::infer(i < row.size() ? trim(row[i]) : std::string());
        }
        bool ok = false;
        if (latCol >= 0 && lonCol >= 0) {
            auto la = static_cast<std::size_t>(latCol) < row.size() ? parseCoordCell(row[static_cast<std::size_t>(latCol)]) : std::nullopt;
            auto lo = static_cast<std::size_t>(lonCol) < row.size() ? parseCoordCell(row[static_cast<std::size_t>(lonCol)]) : std::nullopt;
            if (la && lo && (projected || (std::abs(*la) <= 90 && std::abs(*lo) <= 360))) {
                f.geom = Geometry::point(*lo, *la);
                ok = true;
            }
        }
        if (!ok && wktCol >= 0 && static_cast<std::size_t>(wktCol) < row.size() && !trim(row[static_cast<std::size_t>(wktCol)]).empty()) {
            try {
                f.geom = parseWkt(row[static_cast<std::size_t>(wktCol)]);
                ok = !f.geom.empty();
            } catch (const std::exception&) {
                ok = false;
            }
        }
        if (!ok) {
            ++bad;
            continue;
        }
        toWgs(f.geom, o.crs);
        out.push_back(std::move(f));
    }
    if (bad && warnings) warnings->push_back(std::to_string(bad) + " rows without readable coordinates were skipped");
    return out;
}

std::string toCsv(const Layer& layer, const ExportOptions& o) {
    const auto fields = layer.fields();
    bool allPoints = !layer.features.empty();
    for (const auto& f : layer.features) allPoints = allPoints && f.geom.type == GeomType::Point && f.geom.points.size() == 1;
    const bool geo = o.crs.geographic();
    const int prec = geo ? o.precision : 3;
    std::string s;
    for (const auto& k : fields) s += csvCell(k) + ",";
    bool hasZ = false;
    for (const auto& f : layer.features) f.geom.forEach([&](const Coord& c) { hasZ = hasZ || !std::isnan(c.z); });
    if (allPoints) s += geo ? "lat,lon" : "x,y";
    else s += "wkt";
    if (allPoints && hasZ) s += ",ele";
    s += "\n";
    for (const auto& f : layer.features) {
        for (const auto& k : fields) {
            auto it = f.props.find(k);
            s += (it == f.props.end() ? std::string() : csvCell(it->second.text())) + ",";
        }
        if (allPoints) {
            const Coord c = o.crs.forward(f.geom.points[0]);
            s += geo ? fmt(c.y, prec) + "," + fmt(c.x, prec) : fmt(c.x, prec) + "," + fmt(c.y, prec);
            if (hasZ) s += "," + (std::isnan(c.z) ? std::string() : fmt(c.z, 2));
        } else {
            Geometry g = f.geom;
            g.forEach([&](Coord& c) {
                const Coord p = o.crs.forward(c);
                c.x = p.x;
                c.y = p.y;
            });
            s += csvCell(toWkt(g, prec));
        }
        s += "\n";
    }
    return s;
}

// ---- dispatch ---------------------------------------------------------------------------------

std::string vectorFormatFor(const fs::path& path) {
    std::string ext = lower(path.extension().string());
    if (!ext.empty()) ext.erase(0, 1);
    if (ext == "geojson" || ext == "json" || ext == "geojsonl") return "geojson";
    if (ext == "kml") return "kml";
    if (ext == "gpx") return "gpx";
    if (ext == "csv" || ext == "tsv" || ext == "txt") return "csv";
    if (ext == "shp" || ext == "dbf" || ext == "shx") return "shp";
    if (ext == "wkt") return "wkt";
    return {};
}

std::vector<Layer> importVector(const fs::path& path, const std::string& format0, const ImportOptions& o, std::vector<std::string>* warnings) {
    std::string format = format0.empty() || format0 == "auto" ? vectorFormatFor(path) : format0;
    if (format.empty()) throw std::runtime_error("can't tell the format of " + path.string() + " (use .geojson, .kml, .gpx, .csv, .shp or .wkt, or pass format=)");
    std::string base = path.stem().string();
    if (format == "shp") {
        fs::path shp = path;
        if (lower(shp.extension().string()) != ".shp") shp.replace_extension(".shp");
        Layer l = readShapefile(shp, o.crs, warnings);
        l.name = base;
        return {l};
    }
    const std::string text = readFile(path);
    std::vector<Layer> out;
    auto make = [&](const std::string& name, std::vector<Feature> fs) {
        Layer l;
        l.name = name;
        l.features = std::move(fs);
        l.source = path.string();
        out.push_back(std::move(l));
    };
    if (format == "geojson") {
        make(base, parseGeoJson(text, o.crs));
    } else if (format == "kml") {
        make(base, parseKml(text, warnings));
    } else if (format == "gpx") {
        GpxData g = parseGpx(text);
        const std::string what = o.what.empty() ? "all" : o.what;
        if ((what == "all" || what == "tracks") && !g.tracks.empty()) make(base + " tracks", std::move(g.tracks));
        if ((what == "all" || what == "routes") && !g.routes.empty()) make(base + " routes", std::move(g.routes));
        if ((what == "all" || what == "waypoints") && !g.waypoints.empty()) make(base + " waypoints", std::move(g.waypoints));
        if (out.empty()) make(base, {});
        if (out.size() == 1) out[0].name = base;
    } else if (format == "csv") {
        make(base, parseCsv(text, o, warnings));
    } else if (format == "wkt") {
        std::vector<Feature> fs;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            Feature f;
            f.geom = parseWkt(line);
            toWgs(f.geom, o.crs);
            f.props["n"] = Prop::number(static_cast<double>(fs.size()));
            fs.push_back(std::move(f));
        }
        make(base, std::move(fs));
    } else {
        throw std::runtime_error("unknown vector format '" + format + "'");
    }
    return out;
}

std::vector<std::string> exportVector(const Layer& layer, const fs::path& path, const std::string& format0, const ExportOptions& o) {
    std::string format = format0.empty() || format0 == "auto" ? vectorFormatFor(path) : format0;
    if (format.empty()) throw std::runtime_error("can't tell the format from " + path.string() + " (use .geojson, .kml, .gpx, .csv, .shp or .wkt)");
    if (layer.isRaster()) throw std::runtime_error("'" + layer.name + "' is a raster layer: use maps.raster_save");
    if (format == "shp") return writeShapefile(layer, path, o);
    std::string text;
    if (format == "geojson") text = toGeoJson(layer.features, o);
    else if (format == "kml") text = toKml(layer);
    else if (format == "gpx") text = toGpx(layer);
    else if (format == "csv") text = toCsv(layer, o);
    else if (format == "wkt") {
        for (const auto& f : layer.features) {
            Geometry g = f.geom;
            g.forEach([&](Coord& c) {
                const Coord p = o.crs.forward(c);
                c.x = p.x;
                c.y = p.y;
            });
            text += toWkt(g, o.crs.geographic() ? o.precision : 3) + "\n";
        }
    } else {
        throw std::runtime_error("unknown vector format '" + format + "'");
    }
    writeFile(path, text);
    return {path.string()};
}

// ---- projects -----------------------------------------------------------------------------------

namespace {

std::string hex(Color c) {
    char buf[12];
    if (c.a == 255) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
    return buf;
}

Color colorOr(const json& j, const char* key, Color fallback) {
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    auto c = Color::parse(j[key].get<std::string>());
    return c ? *c : fallback;
}

json styleJson(const Style& s) {
    json p = json::array();
    for (const auto& c : s.palette) p.push_back(hex(c));
    return {{"stroke", hex(s.stroke)}, {"width", s.width}, {"fill", hex(s.fill)}, {"fill_opacity", s.fillOpacity}, {"opacity", s.opacity},
            {"point_size", s.pointSize}, {"marker", s.marker}, {"dash", s.dash}, {"label_field", s.labelField}, {"label_size", s.labelSize},
            {"label_color", hex(s.labelColor)}, {"halo", s.halo}, {"mode", s.mode}, {"field", s.field}, {"ramp", s.ramp}, {"palette", p},
            {"classes", s.classes}, {"method", s.method}, {"size_by_value", s.sizeByValue}, {"raster_ramp", s.rasterRamp}, {"hillshade", s.hillshade}};
}

Style styleFrom(const json& j) {
    Style s;
    if (!j.is_object()) return s;
    s.stroke = colorOr(j, "stroke", s.stroke);
    s.width = j.value("width", s.width);
    s.fill = colorOr(j, "fill", s.fill);
    s.fillOpacity = j.value("fill_opacity", s.fillOpacity);
    s.opacity = j.value("opacity", s.opacity);
    s.pointSize = j.value("point_size", s.pointSize);
    s.marker = j.value("marker", s.marker);
    s.dash = j.value("dash", s.dash);
    s.labelField = j.value("label_field", s.labelField);
    s.labelSize = j.value("label_size", s.labelSize);
    s.labelColor = colorOr(j, "label_color", s.labelColor);
    s.halo = j.value("halo", s.halo);
    s.mode = j.value("mode", s.mode);
    s.field = j.value("field", s.field);
    s.ramp = j.value("ramp", s.ramp);
    if (j.contains("palette") && j["palette"].is_array()) {
        for (const auto& c : j["palette"]) {
            if (c.is_string()) {
                if (auto col = Color::parse(c.get<std::string>())) s.palette.push_back(*col);
            }
        }
    }
    s.classes = j.value("classes", s.classes);
    s.method = j.value("method", s.method);
    s.sizeByValue = j.value("size_by_value", s.sizeByValue);
    s.rasterRamp = j.value("raster_ramp", s.rasterRamp);
    s.hillshade = j.value("hillshade", s.hillshade);
    return s;
}

}  // namespace

std::string toProject(const Map& map) {
    json j = {{"format", "achclwss-map"}, {"version", 1}, {"title", map.title}};
    json layers = json::array();
    for (const auto& l : map.layers) {
        json lj = {{"name", l.name}, {"visible", l.visible}, {"source", l.source}, {"style", styleJson(l.style)}};
        if (l.grid) {
            const Grid& g = *l.grid;
            json r = {{"width", g.width}, {"height", g.height}, {"crs", g.crs.epsg() ? g.crs.id() : g.crs.wkt()},
                      {"geo", {g.geo.a, g.geo.b, g.geo.c, g.geo.d, g.geo.e, g.geo.f}}, {"source", g.source}};
            if (!std::isnan(g.nodata)) r["nodata"] = g.nodata;
            if (!g.image.empty()) r["png"] = base64Encode(raster::encodePng(g.image));
            if (g.hasValues()) {
                std::string bytes(g.values.size() * 4, '\0');
                for (std::size_t i = 0; i < g.values.size(); ++i) {
                    std::uint32_t u = 0;
                    std::memcpy(&u, &g.values[i], 4);
                    for (int k = 0; k < 4; ++k) bytes[i * 4 + static_cast<std::size_t>(k)] = static_cast<char>((u >> (8 * k)) & 0xFF);
                }
                r["values"] = base64Encode(bytes);
            }
            lj["raster"] = r;
        } else {
            lj["features"] = json::parse(toGeoJson(l.features));
        }
        layers.push_back(lj);
    }
    j["layers"] = layers;
    return j.dump(1) + "\n";
}

Map fromProject(const std::string& text, const fs::path&) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a map project (bad JSON): ") + e.what());
    }
    if (j.value("format", "") != "achclwss-map") throw std::runtime_error("not an ACHCLWSS map project (.achmap)");
    Map m;
    m.title = j.value("title", "");
    for (const auto& lj : j.value("layers", json::array())) {
        Layer l;
        l.name = lj.value("name", "layer");
        l.visible = lj.value("visible", true);
        l.source = lj.value("source", "");
        l.style = styleFrom(lj.value("style", json::object()));
        if (lj.contains("raster")) {
            const json& r = lj["raster"];
            auto g = std::make_shared<Grid>();
            g->width = r.value("width", 0);
            g->height = r.value("height", 0);
            g->crs = Crs::parse(r.value("crs", "EPSG:4326")).value_or(Crs::wgs84());
            const json geo = r.value("geo", json::array());
            if (geo.size() == 6) g->geo = {geo[0].get<double>(), geo[1].get<double>(), geo[2].get<double>(), geo[3].get<double>(), geo[4].get<double>(), geo[5].get<double>()};
            g->nodata = r.contains("nodata") && r["nodata"].is_number() ? r["nodata"].get<double>() : kNaN;
            g->source = r.value("source", "");
            if (r.contains("png")) g->image = raster::decode(base64Decode(r["png"].get<std::string>()));
            if (r.contains("values")) {
                const std::string bytes = base64Decode(r["values"].get<std::string>());
                g->values.resize(bytes.size() / 4);
                for (std::size_t i = 0; i < g->values.size(); ++i) {
                    std::uint32_t u = 0;
                    for (int k = 0; k < 4; ++k) u |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i * 4 + static_cast<std::size_t>(k)])) << (8 * k);
                    std::memcpy(&g->values[i], &u, 4);
                }
                if (g->values.size() != static_cast<std::size_t>(g->width) * g->height) throw std::runtime_error("map project: raster '" + l.name + "' has the wrong number of values");
            }
            g->updateRange();
            l.grid = g;
        } else if (lj.contains("features")) {
            l.features = parseGeoJson(lj["features"].dump());
        }
        m.layers.push_back(std::move(l));
    }
    return m;
}

}  // namespace achclwss::maps
