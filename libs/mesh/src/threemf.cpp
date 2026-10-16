// 3MF: a zip (the stored-zip writer from libs/raster) holding an XML model.
// Written as one object in millimetres; read with every object placed by the
// build items (components resolved), converted to millimetres.
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>

#include "achclwss/mesh/io.hpp"
#include "achclwss/raster/ora.hpp"

namespace achclwss::mesh {

namespace {

void num(std::string& out, double v) {
    char buf[32];
    int n = std::snprintf(buf, sizeof buf, "%.9g", v);
    out.append(buf, static_cast<std::size_t>(n));
}

// Finds the next <name ...> tag (not <names...>) from `at`; returns npos when none.
std::size_t findTag(const std::string& xml, const std::string& name, std::size_t at, std::size_t limit, std::size_t& end) {
    const std::string open = "<" + name;
    for (;;) {
        std::size_t p = xml.find(open, at);
        if (p == std::string::npos || p >= limit) return std::string::npos;
        const std::size_t after = p + open.size();
        if (after < xml.size() && (std::isspace(static_cast<unsigned char>(xml[after])) || xml[after] == '/' || xml[after] == '>')) {
            end = xml.find('>', after);
            if (end == std::string::npos) return std::string::npos;
            return p;
        }
        at = after;
    }
}

const char* attrStart(const std::string& xml, std::size_t tag, std::size_t end, const char* name) {
    const std::size_t len = std::strlen(name);
    for (std::size_t p = tag; p + len + 2 < end; ++p) {
        if (std::isspace(static_cast<unsigned char>(xml[p])) && xml.compare(p + 1, len, name) == 0 && xml[p + 1 + len] == '=' &&
            (xml[p + 2 + len] == '"' || xml[p + 2 + len] == '\'')) {
            return xml.c_str() + p + 3 + len;
        }
    }
    return nullptr;
}

double attrNum(const std::string& xml, std::size_t tag, std::size_t end, const char* name, double fallback) {
    const char* s = attrStart(xml, tag, end, name);
    if (!s) return fallback;
    return std::strtod(s, nullptr);
}

std::string attrStr(const std::string& xml, std::size_t tag, std::size_t end, const char* name) {
    const char* s = attrStart(xml, tag, end, name);
    if (!s) return {};
    const char quote = s[-1];
    const char* e = std::strchr(s, quote);
    return e ? std::string(s, e) : std::string();
}

Mat4 parseTransform(const std::string& text) {
    Mat4 m;
    if (text.empty()) return m;
    double v[12];
    const char* p = text.c_str();
    for (double& x : v) {
        char* e = nullptr;
        x = std::strtod(p, &e);
        if (e == p) return m;
        p = e;
    }
    // 3MF matrices act on row vectors: [x y z 1] * M (4 rows x 3 columns).
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) m.at(c, r) = v[r * 3 + c];
        m.at(r, 3) = v[9 + r];
    }
    return m;
}

struct Object {
    Mesh mesh;
    std::vector<std::pair<std::string, Mat4>> components;
};

Mesh resolve(const std::map<std::string, Object>& objects, const std::string& id, int depth) {
    auto it = objects.find(id);
    if (it == objects.end() || depth > 16) return {};
    Mesh out = it->second.mesh;
    for (const auto& [child, m] : it->second.components) {
        Mesh c = resolve(objects, child, depth + 1);
        transform(c, m);
        append(out, c);
    }
    return out;
}

}  // namespace

std::string write3mf(const Mesh& m) {
    std::string model =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n"
        " <metadata name=\"Application\">achclwss</metadata>\n"
        " <resources>\n  <object id=\"1\" type=\"model\">\n   <mesh>\n    <vertices>\n";
    for (const auto& p : m.positions) {
        model += "     <vertex x=\"";
        num(model, p.x);
        model += "\" y=\"";
        num(model, p.y);
        model += "\" z=\"";
        num(model, p.z);
        model += "\"/>\n";
    }
    model += "    </vertices>\n    <triangles>\n";
    for (std::size_t t = 0; t < m.triangleCount(); ++t) {
        model += "     <triangle v1=\"" + std::to_string(m.indices[t * 3]) + "\" v2=\"" + std::to_string(m.indices[t * 3 + 1]) +
                 "\" v3=\"" + std::to_string(m.indices[t * 3 + 2]) + "\"/>\n";
    }
    model += "    </triangles>\n   </mesh>\n  </object>\n </resources>\n <build>\n  <item objectid=\"1\"/>\n </build>\n</model>\n";
    const std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/></Types>\n";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
        "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>\n";
    return raster::writeZip({{"[Content_Types].xml", types}, {"_rels/.rels", rels}, {"3D/3dmodel.model", model}});
}

Mesh read3mf(const std::string& bytes) {
    std::string xml;
    for (auto& e : raster::readZip(bytes)) {
        const bool isModel = e.name.size() > 6 && e.name.compare(e.name.size() - 6, 6, ".model") == 0;
        if (isModel && (xml.empty() || e.name == "3D/3dmodel.model")) xml = std::move(e.data);
    }
    if (xml.empty()) throw std::runtime_error("3MF package has no 3D model");

    double unit = 1.0;  // to millimetres
    std::size_t end = 0;
    if (std::size_t t = findTag(xml, "model", 0, xml.size(), end); t != std::string::npos) {
        const std::string u = attrStr(xml, t, end, "unit");
        if (u == "micron") unit = 0.001;
        else if (u == "centimeter") unit = 10;
        else if (u == "meter") unit = 1000;
        else if (u == "inch") unit = 25.4;
        else if (u == "foot") unit = 304.8;
    }

    std::map<std::string, Object> objects;
    std::vector<std::string> order;
    for (std::size_t at = 0;;) {
        std::size_t oEnd = 0;
        const std::size_t o = findTag(xml, "object", at, xml.size(), oEnd);
        if (o == std::string::npos) break;
        const std::string id = attrStr(xml, o, oEnd, "id");
        std::size_t close = xml.find("</object>", oEnd);
        if (xml[oEnd - 1] == '/') close = oEnd;  // <object .../>
        if (close == std::string::npos) close = xml.size();
        Object obj;
        for (std::size_t v = oEnd, ve = 0; (v = findTag(xml, "vertex", v, close, ve)) != std::string::npos; v = ve) {
            obj.mesh.positions.push_back({attrNum(xml, v, ve, "x", 0), attrNum(xml, v, ve, "y", 0), attrNum(xml, v, ve, "z", 0)});
        }
        for (std::size_t t = oEnd, te = 0; (t = findTag(xml, "triangle", t, close, te)) != std::string::npos; t = te) {
            const double a = attrNum(xml, t, te, "v1", -1), b = attrNum(xml, t, te, "v2", -1), c = attrNum(xml, t, te, "v3", -1);
            const double n = static_cast<double>(obj.mesh.positions.size());
            if (a < 0 || b < 0 || c < 0 || a >= n || b >= n || c >= n) throw std::runtime_error("3MF triangle refers to a vertex that doesn't exist");
            obj.mesh.addTriangle(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(c));
        }
        for (std::size_t c = oEnd, ce = 0; (c = findTag(xml, "component", c, close, ce)) != std::string::npos; c = ce) {
            obj.components.emplace_back(attrStr(xml, c, ce, "objectid"), parseTransform(attrStr(xml, c, ce, "transform")));
        }
        order.push_back(id);
        objects[id] = std::move(obj);
        at = close;
    }

    Mesh out;
    bool anyItem = false;
    if (std::size_t b = xml.find("<build"); b != std::string::npos) {
        for (std::size_t i = b, ie = 0; (i = findTag(xml, "item", i, xml.size(), ie)) != std::string::npos; i = ie) {
            anyItem = true;
            Mesh m = resolve(objects, attrStr(xml, i, ie, "objectid"), 0);
            transform(m, parseTransform(attrStr(xml, i, ie, "transform")));
            append(out, m);
        }
    }
    if (!anyItem) {
        for (const auto& id : order) append(out, resolve(objects, id, 0));
    }
    if (unit != 1.0) transform(out, Mat4::scale(unit));
    cleanup(out);
    return out;
}

}  // namespace achclwss::mesh
