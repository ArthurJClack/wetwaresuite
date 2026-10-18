// The .typeface.json format, UFO-lite folders, SVG drawings as glyphs, and
// picking a reader by file type.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/vector/vector.hpp"
#include "internal.hpp"

namespace wetwaresuite::typeface {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string readFile(const std::string& path) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeFile(const std::string& path, const std::string& bytes) {
    const fs::path p = fs::path(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("can't write " + path);
}

// ---- XML ----

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

namespace {

std::string decodeEntities(const std::string& s) {
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
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') {
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            const unsigned long v = std::strtoul(e.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            out += utf8Encode(static_cast<std::uint32_t>(std::min<unsigned long>(v, 0x10FFFF)));
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(const std::string& t) : t_(t) {}
    XmlNode root() {
        skipMisc();
        if (i_ >= t_.size() || t_[i_] != '<') throw std::runtime_error("not an XML file");
        return element(0);
    }

private:
    const std::string& t_;
    std::size_t i_ = 0;

    void skipMisc() {
        while (i_ < t_.size()) {
            if (std::isspace(static_cast<unsigned char>(t_[i_]))) {
                ++i_;
            } else if (t_.compare(i_, 4, "<!--") == 0) {
                auto e = t_.find("-->", i_);
                i_ = e == std::string::npos ? t_.size() : e + 3;
            } else if (t_.compare(i_, 2, "<?") == 0 || t_.compare(i_, 2, "<!") == 0) {
                auto e = t_.find('>', i_);
                i_ = e == std::string::npos ? t_.size() : e + 1;
            } else {
                break;
            }
        }
    }

    XmlNode element(int depth) {
        if (depth > 200) throw std::runtime_error("XML nested too deeply");
        XmlNode n;
        ++i_;  // <
        while (i_ < t_.size() && !std::isspace(static_cast<unsigned char>(t_[i_])) && t_[i_] != '>' && t_[i_] != '/') n.name += t_[i_++];
        while (true) {
            while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
            if (i_ >= t_.size()) throw std::runtime_error("XML ends inside <" + n.name + ">");
            if (t_[i_] == '/') {
                i_ += 2;
                return n;
            }
            if (t_[i_] == '>') {
                ++i_;
                break;
            }
            std::string key;
            while (i_ < t_.size() && t_[i_] != '=' && !std::isspace(static_cast<unsigned char>(t_[i_])) && t_[i_] != '>') key += t_[i_++];
            while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
            std::string value;
            if (i_ < t_.size() && t_[i_] == '=') {
                ++i_;
                while (i_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[i_]))) ++i_;
                const char q = i_ < t_.size() ? t_[i_] : '"';
                if (q == '"' || q == '\'') {
                    auto e = t_.find(q, i_ + 1);
                    if (e == std::string::npos) throw std::runtime_error("unterminated XML attribute");
                    value = decodeEntities(t_.substr(i_ + 1, e - i_ - 1));
                    i_ = e + 1;
                }
            }
            n.attrs.push_back({key, value});
        }
        while (i_ < t_.size()) {
            if (t_.compare(i_, 2, "</") == 0) {
                auto e = t_.find('>', i_);
                i_ = e == std::string::npos ? t_.size() : e + 1;
                return n;
            }
            if (t_.compare(i_, 4, "<!--") == 0) {
                auto e = t_.find("-->", i_);
                i_ = e == std::string::npos ? t_.size() : e + 3;
            } else if (t_.compare(i_, 9, "<![CDATA[") == 0) {
                auto e = t_.find("]]>", i_);
                n.text += t_.substr(i_ + 9, (e == std::string::npos ? t_.size() : e) - i_ - 9);
                i_ = e == std::string::npos ? t_.size() : e + 3;
            } else if (t_.compare(i_, 2, "<?") == 0 || t_.compare(i_, 2, "<!") == 0) {
                auto e = t_.find('>', i_);
                i_ = e == std::string::npos ? t_.size() : e + 1;
            } else if (t_[i_] == '<') {
                n.children.push_back(element(depth + 1));
            } else {
                auto e = t_.find('<', i_);
                if (e == std::string::npos) e = t_.size();
                n.text += decodeEntities(t_.substr(i_, e - i_));
                i_ = e;
            }
        }
        return n;
    }
};

}  // namespace

XmlNode parseXml(const std::string& text) { return XmlParser(text).root(); }

// ---- JSON ----

namespace {

json num(double v) {
    if (std::abs(v - std::round(v)) < 1e-9 && std::abs(v) < 1e15) return static_cast<long long>(std::llround(v));
    return std::round(v * 1000) / 1000;
}

}  // namespace

std::string toJson(const Font& font) {
    json j;
    j["format"] = "wetwaresuite.typeface";
    j["version"] = 1;
    const auto& i = font.info;
    j["info"] = {{"family", i.family}, {"style", i.style}, {"version", i.version}, {"copyright", i.copyright},
                 {"license", i.license}, {"license_url", i.licenseUrl}, {"designer", i.designer},
                 {"manufacturer", i.manufacturer}, {"description", i.description}, {"vendor", i.vendor}, {"weight", i.weight}};
    const auto& m = font.metrics;
    j["metrics"] = {{"units_per_em", m.unitsPerEm}, {"ascender", num(m.ascender)}, {"descender", num(m.descender)},
                    {"x_height", num(m.xHeight)}, {"cap_height", num(m.capHeight)}, {"line_gap", num(m.lineGap)},
                    {"italic_angle", num(m.italicAngle)}, {"underline_position", num(m.underlinePosition)},
                    {"underline_thickness", num(m.underlineThickness)}};
    json glyphs = json::array();
    for (const auto& g : font.glyphs) {
        json jg;
        jg["name"] = g.name;
        jg["unicodes"] = g.unicodes;
        jg["advance"] = num(g.advance);
        json contours = json::array();
        for (const auto& c : g.contours) {
            json pts = json::array();
            for (const auto& p : c.points) {
                json pt = {num(p.x), num(p.y)};
                if (p.type == PointType::Quad) pt.push_back("q");
                if (p.type == PointType::Cubic) pt.push_back("c");
                pts.push_back(pt);
            }
            contours.push_back(pts);
        }
        jg["contours"] = contours;
        if (!g.components.empty()) {
            json comps = json::array();
            for (const auto& c : g.components) {
                json jc = {{"base", c.base}, {"offset", {num(c.dx), num(c.dy)}}};
                if (!(c.xx == 1 && c.xy == 0 && c.yx == 0 && c.yy == 1)) jc["matrix"] = {num(c.xx), num(c.xy), num(c.yx), num(c.yy)};
                comps.push_back(jc);
            }
            jg["components"] = comps;
        }
        glyphs.push_back(jg);
    }
    j["glyphs"] = glyphs;
    json kern = json::array();
    for (const auto& [p, v] : font.kerning) kern.push_back({p.first, p.second, num(v)});
    j["kerning"] = kern;
    return j.dump(1) + "\n";
}

Font fromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    if (!j.is_object() || j.value("format", "") != "wetwaresuite.typeface")
        throw std::runtime_error("not a .typeface.json file (no \"format\": \"wetwaresuite.typeface\")");
    Font f;
    f.glyphs.clear();
    if (j.contains("info")) {
        const auto& i = j["info"];
        f.info.family = i.value("family", f.info.family);
        f.info.style = i.value("style", f.info.style);
        f.info.version = i.value("version", f.info.version);
        f.info.copyright = i.value("copyright", "");
        f.info.license = i.value("license", "");
        f.info.licenseUrl = i.value("license_url", "");
        f.info.designer = i.value("designer", "");
        f.info.manufacturer = i.value("manufacturer", "");
        f.info.description = i.value("description", "");
        f.info.vendor = i.value("vendor", f.info.vendor);
        f.info.weight = i.value("weight", 400);
    }
    if (j.contains("metrics")) {
        const auto& m = j["metrics"];
        f.metrics.unitsPerEm = std::clamp(m.value("units_per_em", 1000), 16, 16384);
        f.metrics.ascender = m.value("ascender", f.metrics.ascender);
        f.metrics.descender = m.value("descender", f.metrics.descender);
        f.metrics.xHeight = m.value("x_height", f.metrics.xHeight);
        f.metrics.capHeight = m.value("cap_height", f.metrics.capHeight);
        f.metrics.lineGap = m.value("line_gap", 0.0);
        f.metrics.italicAngle = m.value("italic_angle", 0.0);
        f.metrics.underlinePosition = m.value("underline_position", f.metrics.underlinePosition);
        f.metrics.underlineThickness = m.value("underline_thickness", f.metrics.underlineThickness);
    }
    for (const auto& jg : j.value("glyphs", json::array())) {
        Glyph g;
        g.name = jg.value("name", "");
        if (g.name.empty()) continue;
        for (const auto& u : jg.value("unicodes", json::array())) g.unicodes.push_back(u.get<std::uint32_t>());
        g.advance = jg.value("advance", 500.0);
        for (const auto& jc : jg.value("contours", json::array())) {
            Contour c;
            for (const auto& pt : jc) {
                if (!pt.is_array() || pt.size() < 2) continue;
                Point p{pt[0].get<double>(), pt[1].get<double>(), PointType::On};
                if (pt.size() > 2 && pt[2].is_string()) {
                    const std::string t = pt[2].get<std::string>();
                    p.type = t == "q" ? PointType::Quad : t == "c" ? PointType::Cubic : PointType::On;
                }
                c.points.push_back(p);
            }
            if (c.points.size() >= 2) g.contours.push_back(std::move(c));
        }
        for (const auto& jc : jg.value("components", json::array())) {
            Component c;
            c.base = jc.value("base", "");
            if (jc.contains("offset") && jc["offset"].size() == 2) {
                c.dx = jc["offset"][0].get<double>();
                c.dy = jc["offset"][1].get<double>();
            }
            if (jc.contains("matrix") && jc["matrix"].size() == 4) {
                c.xx = jc["matrix"][0].get<double>();
                c.xy = jc["matrix"][1].get<double>();
                c.yx = jc["matrix"][2].get<double>();
                c.yy = jc["matrix"][3].get<double>();
            }
            if (!c.base.empty()) g.components.push_back(c);
        }
        f.glyphs.push_back(std::move(g));
    }
    for (const auto& k : j.value("kerning", json::array())) {
        if (k.is_array() && k.size() == 3) f.kerning[{k[0].get<std::string>(), k[1].get<std::string>()}] = k[2].get<double>();
    }
    return f;
}

// ---- UFO ----

namespace {

// plist value as JSON (dict, array, string, integer, real, true/false)
json plistValue(const XmlNode& n) {
    if (n.name == "dict") {
        json o = json::object();
        for (std::size_t i = 0; i + 1 < n.children.size(); i += 2) {
            if (n.children[i].name == "key") o[n.children[i].text] = plistValue(n.children[i + 1]);
        }
        return o;
    }
    if (n.name == "array") {
        json a = json::array();
        for (const auto& c : n.children) a.push_back(plistValue(c));
        return a;
    }
    if (n.name == "integer") return std::strtoll(n.text.c_str(), nullptr, 10);
    if (n.name == "real") return std::strtod(n.text.c_str(), nullptr);
    if (n.name == "true") return true;
    if (n.name == "false") return false;
    return n.text;
}

json readPlist(const fs::path& p) {
    if (!fs::exists(p)) return json::object();
    XmlNode root = parseXml(readFile(p.string()));
    if (root.name == "plist" && !root.children.empty()) return plistValue(root.children[0]);
    return plistValue(root);
}

std::string plistXml(const json& v, int indent) {
    const std::string pad(static_cast<std::size_t>(indent), '\t');
    if (v.is_object()) {
        std::string s = pad + "<dict>\n";
        for (auto it = v.begin(); it != v.end(); ++it) {
            s += pad + "\t<key>" + xmlEscape(it.key()) + "</key>\n" + plistXml(it.value(), indent + 1);
        }
        return s + pad + "</dict>\n";
    }
    if (v.is_array()) {
        std::string s = pad + "<array>\n";
        for (const auto& e : v) s += plistXml(e, indent + 1);
        return s + pad + "</array>\n";
    }
    if (v.is_boolean()) return pad + (v.get<bool>() ? "<true/>\n" : "<false/>\n");
    if (v.is_number_integer()) return pad + "<integer>" + std::to_string(v.get<long long>()) + "</integer>\n";
    if (v.is_number()) return pad + "<real>" + fmt(v.get<double>()) + "</real>\n";
    return pad + "<string>" + xmlEscape(v.get<std::string>()) + "</string>\n";
}

std::string plistFile(const json& v) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n" +
           plistXml(v, 0) + "</plist>\n";
}

double numAttr(const XmlNode& n, const char* key, double fallback) {
    const std::string* v = n.attr(key);
    return v ? std::strtod(v->c_str(), nullptr) : fallback;
}

Glyph readGlif(const std::string& text, const std::string& fallbackName) {
    XmlNode g = parseXml(text);
    Glyph out;
    out.name = g.attr("name") ? *g.attr("name") : fallbackName;
    out.advance = 0;
    for (const auto& c : g.children) {
        if (c.name == "advance") out.advance = numAttr(c, "width", 0);
        if (c.name == "unicode" && c.attr("hex")) out.unicodes.push_back(static_cast<std::uint32_t>(std::strtoul(c.attr("hex")->c_str(), nullptr, 16)));
        if (c.name != "outline") continue;
        for (const auto& item : c.children) {
            if (item.name == "component" && item.attr("base")) {
                Component k;
                k.base = *item.attr("base");
                k.xx = numAttr(item, "xScale", 1);
                k.xy = numAttr(item, "xyScale", 0);
                k.yx = numAttr(item, "yxScale", 0);
                k.yy = numAttr(item, "yScale", 1);
                k.dx = numAttr(item, "xOffset", 0);
                k.dy = numAttr(item, "yOffset", 0);
                out.components.push_back(k);
            }
            if (item.name != "contour") continue;
            std::vector<std::pair<Point, std::string>> pts;
            for (const auto& p : item.children) {
                if (p.name != "point") continue;
                const std::string type = p.attr("type") ? *p.attr("type") : "offcurve";
                pts.push_back({{numAttr(p, "x", 0), numAttr(p, "y", 0), PointType::On}, type});
            }
            const std::size_t n = pts.size();
            if (n < 2) continue;
            Contour contour;
            for (std::size_t i = 0; i < n; ++i) {
                Point p = pts[i].first;
                if (pts[i].second == "offcurve") {
                    // the kind of off-curve point depends on the on-curve point that ends its run
                    std::string next = "qcurve";
                    for (std::size_t k = 1; k <= n; ++k) {
                        const std::string& t = pts[(i + k) % n].second;
                        if (t != "offcurve") {
                            next = t;
                            break;
                        }
                    }
                    p.type = next == "curve" ? PointType::Cubic : PointType::Quad;
                }
                contour.points.push_back(p);
            }
            auto it = std::find_if(contour.points.begin(), contour.points.end(), [](const Point& q) { return q.on(); });
            if (it != contour.points.end()) std::rotate(contour.points.begin(), it, contour.points.end());
            out.contours.push_back(std::move(contour));
        }
    }
    return out;
}

std::string glifFileName(const std::string& name, std::set<std::string>& used) {
    std::string base;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isupper(u)) {
            base += c;
            base += '_';
        } else if (u < 0x20 || std::string("\"*+/:<>?[\\]|").find(c) != std::string::npos) {
            base += '_';
        } else {
            base += c;
        }
    }
    if (!base.empty() && base[0] == '.') base[0] = '_';
    std::string file = base + ".glif";
    std::string lower = file;
    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (int k = 1; used.count(lower); ++k) {
        file = base + std::to_string(k) + ".glif";
        lower = file;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    used.insert(lower);
    return file;
}

std::string writeGlif(const Glyph& g) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<glyph name=\"" + xmlEscape(g.name) + "\" format=\"2\">\n";
    s += "\t<advance width=\"" + fmt(g.advance) + "\"/>\n";
    for (auto u : g.unicodes) s += "\t<unicode hex=\"" + unicodeLabel(u).substr(2) + "\"/>\n";
    if (!g.contours.empty() || !g.components.empty()) {
        s += "\t<outline>\n";
        for (const auto& c : g.components) {
            s += "\t\t<component base=\"" + xmlEscape(c.base) + "\"";
            if (c.xx != 1) s += " xScale=\"" + fmt(c.xx) + "\"";
            if (c.xy != 0) s += " xyScale=\"" + fmt(c.xy) + "\"";
            if (c.yx != 0) s += " yxScale=\"" + fmt(c.yx) + "\"";
            if (c.yy != 1) s += " yScale=\"" + fmt(c.yy) + "\"";
            if (c.dx != 0) s += " xOffset=\"" + fmt(c.dx) + "\"";
            if (c.dy != 0) s += " yOffset=\"" + fmt(c.dy) + "\"";
            s += "/>\n";
        }
        for (const auto& c : g.contours) {
            s += "\t\t<contour>\n";
            const auto& pts = c.points;
            for (std::size_t i = 0; i < pts.size(); ++i) {
                const Point& p = pts[i];
                s += "\t\t\t<point x=\"" + fmt(p.x) + "\" y=\"" + fmt(p.y) + "\"";
                if (p.on()) {
                    // segment type from the off-curve points before it (cyclically)
                    const Point& prev = pts[(i + pts.size() - 1) % pts.size()];
                    const char* type = prev.on() ? "line" : prev.type == PointType::Cubic ? "curve" : "qcurve";
                    s += std::string(" type=\"") + type + "\"";
                }
                s += "/>\n";
            }
            s += "\t\t</contour>\n";
        }
        s += "\t</outline>\n";
    }
    return s + "</glyph>\n";
}

}  // namespace

Font loadUfo(const fs::path& dir, std::vector<std::string>* warnings) {
    if (!fs::is_directory(dir)) throw std::runtime_error(dir.string() + " is not a UFO folder");
    Font f;
    f.glyphs.clear();
    const json info = readPlist(dir / "fontinfo.plist");
    auto str = [&](const char* k, const std::string& fb) { return info.contains(k) && info[k].is_string() ? info[k].get<std::string>() : fb; };
    auto numv = [&](const char* k, double fb) { return info.contains(k) && info[k].is_number() ? info[k].get<double>() : fb; };
    f.info.family = str("familyName", f.info.family);
    f.info.style = str("styleName", f.info.style);
    f.info.copyright = str("copyright", "");
    f.info.license = str("openTypeNameLicense", "");
    f.info.licenseUrl = str("openTypeNameLicenseURL", "");
    f.info.designer = str("openTypeNameDesigner", "");
    f.info.manufacturer = str("openTypeNameManufacturer", "");
    f.info.description = str("openTypeNameDescription", "");
    f.info.vendor = str("openTypeOS2VendorID", f.info.vendor);
    f.info.weight = static_cast<int>(numv("openTypeOS2WeightClass", 400));
    if (info.contains("versionMajor")) {
        const int major = static_cast<int>(numv("versionMajor", 1)), minor = static_cast<int>(numv("versionMinor", 0));
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d.%03d", major, minor);
        f.info.version = buf;
    }
    f.metrics.unitsPerEm = std::clamp(static_cast<int>(numv("unitsPerEm", 1000)), 16, 16384);
    f.metrics.ascender = numv("ascender", f.metrics.unitsPerEm * 0.8);
    f.metrics.descender = numv("descender", -f.metrics.unitsPerEm * 0.2);
    f.metrics.xHeight = numv("xHeight", f.metrics.unitsPerEm * 0.5);
    f.metrics.capHeight = numv("capHeight", f.metrics.unitsPerEm * 0.7);
    f.metrics.italicAngle = numv("italicAngle", 0);
    f.metrics.underlinePosition = numv("postscriptUnderlinePosition", -f.metrics.unitsPerEm * 0.1);
    f.metrics.underlineThickness = numv("postscriptUnderlineThickness", f.metrics.unitsPerEm * 0.05);
    f.metrics.lineGap = numv("openTypeHheaLineGap", 0);

    const json contents = readPlist(dir / "glyphs" / "contents.plist");
    if (!contents.is_object() || contents.empty()) throw std::runtime_error("UFO has no glyphs/contents.plist");
    std::vector<std::string> order;
    const json lib = readPlist(dir / "lib.plist");
    if (lib.contains("public.glyphOrder") && lib["public.glyphOrder"].is_array()) {
        for (const auto& n : lib["public.glyphOrder"]) {
            if (n.is_string() && contents.contains(n.get<std::string>())) order.push_back(n.get<std::string>());
        }
    }
    std::set<std::string> listed(order.begin(), order.end());
    for (auto it = contents.begin(); it != contents.end(); ++it) {
        if (!listed.count(it.key())) order.push_back(it.key());
    }
    // .notdef first
    auto nd = std::find(order.begin(), order.end(), ".notdef");
    if (nd != order.end()) std::rotate(order.begin(), nd, nd + 1);
    for (const auto& name : order) {
        const fs::path file = dir / "glyphs" / fs::path(contents[name].get<std::string>());
        try {
            Glyph g = readGlif(readFile(file.string()), name);
            g.name = name;
            f.glyphs.push_back(std::move(g));
        } catch (const std::exception& e) {
            if (warnings) warnings->push_back("glyph " + name + " skipped: " + e.what());
        }
    }
    const json groups = readPlist(dir / "groups.plist");
    auto expand = [&](const std::string& n) {
        std::vector<std::string> out;
        if (n.rfind("public.kern", 0) == 0 && groups.contains(n) && groups[n].is_array()) {
            for (const auto& m : groups[n]) {
                if (m.is_string()) out.push_back(m.get<std::string>());
            }
        } else {
            out.push_back(n);
        }
        return out;
    };
    const json kerning = readPlist(dir / "kerning.plist");
    if (kerning.is_object()) {
        // glyph-glyph pairs override group pairs, as in UFO's kerning lookup
        std::map<KernPair, double> groupPairs;
        for (auto a = kerning.begin(); a != kerning.end(); ++a) {
            if (!a.value().is_object()) continue;
            for (auto b = a.value().begin(); b != a.value().end(); ++b) {
                if (!b.value().is_number()) continue;
                const bool grouped = a.key().rfind("public.kern", 0) == 0 || b.key().rfind("public.kern", 0) == 0;
                for (const auto& l : expand(a.key())) {
                    for (const auto& r : expand(b.key())) {
                        if (groupPairs.size() > 200000) break;
                        (grouped ? groupPairs : f.kerning)[{l, r}] = b.value().get<double>();
                    }
                }
            }
        }
        for (const auto& [p, v] : groupPairs) f.kerning.emplace(p, v);
    }
    return f;
}

void saveUfo(const Font& font, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir / "glyphs", ec);
    if (ec) throw std::runtime_error("can't create " + (dir / "glyphs").string());
    // drop old glyph files so renamed glyphs don't linger
    for (const auto& e : fs::directory_iterator(dir / "glyphs", ec)) {
        if (e.path().extension() == ".glif") fs::remove(e.path(), ec);
    }
    writeFile((dir / "metainfo.plist").string(), plistFile({{"creator", "org.wetwaresuite.type"}, {"formatVersion", 3}}));
    const auto& i = font.info;
    const auto& m = font.metrics;
    json info = {{"familyName", i.family},
                 {"styleName", i.style},
                 {"unitsPerEm", m.unitsPerEm},
                 {"ascender", num(m.ascender)},
                 {"descender", num(m.descender)},
                 {"xHeight", num(m.xHeight)},
                 {"capHeight", num(m.capHeight)},
                 {"italicAngle", num(m.italicAngle)},
                 {"openTypeHheaLineGap", num(m.lineGap)},
                 {"postscriptUnderlinePosition", num(m.underlinePosition)},
                 {"postscriptUnderlineThickness", num(m.underlineThickness)},
                 {"openTypeOS2WeightClass", i.weight},
                 {"openTypeOS2VendorID", i.vendor}};
    const auto dot = i.version.find('.');
    try {
        info["versionMajor"] = std::stoi(i.version.substr(0, dot));
        info["versionMinor"] = dot == std::string::npos ? 0 : std::stoi(i.version.substr(dot + 1));
    } catch (...) {
    }
    auto opt = [&](const char* key, const std::string& v) {
        if (!v.empty()) info[key] = v;
    };
    opt("copyright", i.copyright);
    opt("openTypeNameLicense", i.license);
    opt("openTypeNameLicenseURL", i.licenseUrl);
    opt("openTypeNameDesigner", i.designer);
    opt("openTypeNameManufacturer", i.manufacturer);
    opt("openTypeNameDescription", i.description);
    writeFile((dir / "fontinfo.plist").string(), plistFile(info));
    json contents = json::object();
    json order = json::array();
    std::set<std::string> used;
    for (const auto& g : font.glyphs) {
        const std::string file = glifFileName(g.name, used);
        contents[g.name] = file;
        order.push_back(g.name);
        writeFile((dir / "glyphs" / file).string(), writeGlif(g));
    }
    writeFile((dir / "glyphs" / "contents.plist").string(), plistFile(contents));
    writeFile((dir / "layercontents.plist").string(), plistFile(json::array({json::array({"public.default", "glyphs"})})));
    writeFile((dir / "lib.plist").string(), plistFile({{"public.glyphOrder", order}}));
    json kern = json::object();
    for (const auto& [p, v] : font.kerning) kern[p.first][p.second] = num(v);
    writeFile((dir / "kerning.plist").string(), plistFile(kern));
    if (!fs::exists(dir / "groups.plist")) writeFile((dir / "groups.plist").string(), plistFile(json::object()));
}

// ---- SVG drawings ----

std::vector<Contour> importSvg(const std::string& svgText, const Metrics& m, const std::string& fit, double* advance) {
    auto doc = vector::fromSvg(svgText);
    std::vector<Contour> raw;
    for (const auto& s : doc->shapes) {
        if (!s.visible || !s.fill || s.fill->a == 0) continue;
        std::vector<Contour> cs;
        switch (s.kind) {
        case vector::ShapeKind::Path: cs = parsePathData(s.d); break;
        case vector::ShapeKind::Rect:
            if (s.radius > 0) {
                const double r = std::min({s.radius, s.w / 2, s.h / 2});
                const std::string d = "M" + fmt(s.x + r) + " " + fmt(s.y) + "H" + fmt(s.x + s.w - r) + "A" + fmt(r) + " " + fmt(r) +
                                      " 0 0 1 " + fmt(s.x + s.w) + " " + fmt(s.y + r) + "V" + fmt(s.y + s.h - r) + "A" + fmt(r) + " " +
                                      fmt(r) + " 0 0 1 " + fmt(s.x + s.w - r) + " " + fmt(s.y + s.h) + "H" + fmt(s.x + r) + "A" + fmt(r) +
                                      " " + fmt(r) + " 0 0 1 " + fmt(s.x) + " " + fmt(s.y + s.h - r) + "V" + fmt(s.y + r) + "A" + fmt(r) +
                                      " " + fmt(r) + " 0 0 1 " + fmt(s.x + r) + " " + fmt(s.y) + "Z";
                cs = parsePathData(d);
            } else {
                cs = {rectContour(s.x, s.y, s.w, s.h)};
            }
            break;
        case vector::ShapeKind::Ellipse: cs = {ellipseContour(s.cx, s.cy, s.rx, s.ry)}; break;
        case vector::ShapeKind::Polygon:
        case vector::ShapeKind::Polyline: {
            Polygon p;
            for (const auto& q : s.points) p.push_back({q.x, q.y});
            if (p.size() >= 3) cs = {polygonContour(p)};
            break;
        }
        default: break;
        }
        const vector::Matrix vm = vector::shapeMatrix(s);
        const Affine a{vm.a, vm.b, vm.c, vm.d, vm.e, vm.f};
        for (auto& c : cs) {
            if (!a.identity()) transform(c, a);
            raw.push_back(std::move(c));
        }
    }
    if (raw.empty()) throw std::runtime_error("the SVG has no filled shapes to turn into a glyph (strokes and text are skipped)");
    const double H = doc->height;
    Affine toFont;
    if (advance) *advance = 0;
    if (fit == "none") {
        toFont = {1, 0, 0, -1, 0, H};
        if (advance) *advance = doc->width;
    } else if (fit == "canvas" || fit.empty()) {
        const double k = (m.ascender - m.descender) / std::max(1.0, H);
        toFont = {k, 0, 0, -k, 0, m.ascender};
        if (advance) *advance = std::round(doc->width * k);
    } else {
        Box b = bounds(raw);
        const double target = fit == "cap" ? m.capHeight : fit == "xheight" ? m.xHeight : fit == "ascender" ? m.ascender : 0;
        if (target <= 0) throw std::runtime_error("fit must be canvas, cap, xheight, ascender or none");
        const double k = target / std::max(1e-9, b.height());
        // y' = (b.y1 - y) * k puts the ink's bottom (largest SVG y) on the baseline
        toFont = {k, 0, 0, -k, -b.x0 * k, b.y1 * k};
    }
    transform(raw, toFont);
    fixDirections(raw);
    return raw;
}

// ---- loading by type ----

Font loadFont(const fs::path& path, std::vector<std::string>* warnings) {
    std::string ext = path.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (fs::is_directory(path)) return loadUfo(path, warnings);
    const std::string bytes = readFile(path.string());
    std::size_t i = 0;
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    if (i < bytes.size() && bytes[i] == '{') return fromJson(bytes);
    if (ext == ".svg") throw std::runtime_error("an SVG drawing is a glyph, not a font: use type.import_svg");
    return fromSfnt(bytes, warnings);
}

}  // namespace wetwaresuite::typeface
