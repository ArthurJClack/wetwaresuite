// Excalidraw (.excalidraw JSON) and draw.io (.drawio mxGraph XML) import and
// export: shapes, labels, connectors with their bindings, freehand strokes,
// images, frames and groups.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "achclwss/pdf/page.hpp"
#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::board {

using namespace detail;
using json = nlohmann::json;

namespace {

std::string hex6(const std::optional<Color>& c) {
    if (!c) return "transparent";
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c->r, c->g, c->b);
    return buf;
}

// Ancestor groups, innermost first.
std::vector<std::string> groupChain(const Board& b, const Element& e) {
    std::vector<std::string> out;
    const Element* p = e.parent.empty() ? nullptr : b.get(e.parent);
    for (int d = 0; p && d < 32; ++d) {
        if (p->kind == Kind::Group) out.push_back(p->id);
        p = p->parent.empty() ? nullptr : b.get(p->parent);
    }
    return out;
}

std::string frameOf(const Board& b, const Element& e) {
    const Element* p = e.parent.empty() ? nullptr : b.get(e.parent);
    for (int d = 0; p && d < 32; ++d) {
        if (p->kind == Kind::Frame) return p->id;
        p = p->parent.empty() ? nullptr : b.get(p->parent);
    }
    return {};
}

// ---- Excalidraw export ---------------------------------------------------------------------

json exBase(const Board& b, const Element& e, const Resolved& s, const char* type) {
    json o;
    o["id"] = e.id;
    o["type"] = type;
    o["x"] = e.x;
    o["y"] = e.y;
    o["width"] = e.w;
    o["height"] = e.h;
    o["angle"] = 0;
    o["strokeColor"] = s.stroke ? hex6(s.stroke) : (e.kind == Kind::Note ? "transparent" : "#1e1e1e");
    o["backgroundColor"] = hex6(s.fillStyle == "none" ? std::nullopt : s.fill);
    o["fillStyle"] = s.fillStyle == "hachure" ? "hachure" : (s.fillStyle == "cross" ? "cross-hatch" : "solid");
    o["strokeWidth"] = s.strokeWidth;
    o["strokeStyle"] = s.dash == "dashed" ? "dashed" : (s.dash == "dotted" ? "dotted" : "solid");
    o["roughness"] = s.sketch ? std::min(2.0, std::round(s.roughness)) : 0.0;
    o["opacity"] = std::lround(s.opacity * 100);
    o["groupIds"] = groupChain(b, e);
    const std::string f = frameOf(b, e);
    o["frameId"] = f.empty() ? json(nullptr) : json(f);
    o["roundness"] = nullptr;
    o["seed"] = static_cast<std::int64_t>(hashOf(e.id) % 2000000000ULL);
    o["version"] = 1;
    o["versionNonce"] = static_cast<std::int64_t>(hashOf(e.id + "n") % 2000000000ULL);
    o["isDeleted"] = false;
    o["boundElements"] = nullptr;
    o["updated"] = 1;
    o["link"] = e.link.empty() ? json(nullptr) : json(e.link);
    o["locked"] = false;
    return o;
}

int exFont(const Resolved& s) { return s.sketch ? 1 : (s.font == "mono" ? 3 : 2); }

json exText(const Board& b, const Element& owner, const Resolved& s, const std::string& id, const std::string& container, const Box& area) {
    Element t = owner;
    t.kind = Kind::Text;
    t.id = id;
    json o = exBase(b, t, s, "text");
    const auto lines = container.empty() ? splitLines(owner.text) : wrapText(owner.text, s.font, s.bold, s.italic, s.fontSize, area.w());
    double w = 0;
    for (const auto& l : lines) w = std::max(w, textWidth(l, s.font, s.bold, s.italic, s.fontSize));
    const double h = static_cast<double>(lines.size()) * s.fontSize * kLineHeight;
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) joined += (i ? "\n" : "") + lines[i];
    o["x"] = container.empty() ? owner.x : area.cx() - w / 2;
    o["y"] = container.empty() ? owner.y : area.cy() - h / 2;
    o["width"] = w;
    o["height"] = h;
    o["strokeColor"] = hex6(s.text);
    o["backgroundColor"] = "transparent";
    o["text"] = joined;
    o["originalText"] = owner.text;
    o["fontSize"] = s.fontSize;
    o["fontFamily"] = exFont(s);
    o["textAlign"] = container.empty() ? s.align : "center";
    o["verticalAlign"] = container.empty() ? "top" : "middle";
    o["containerId"] = container.empty() ? json(nullptr) : json(container);
    o["lineHeight"] = kLineHeight;
    o["autoResize"] = true;
    o["baseline"] = std::round(s.fontSize * 0.9);
    return o;
}

std::string exHead(const std::string& h) {
    if (h == "none") return "";
    if (h == "cross") return "bar";
    return h;  // arrow, triangle, diamond, circle, bar
}

}  // namespace

std::string toExcalidraw(const Board& b) {
    json els = json::array();
    json files = json::object();
    std::map<std::string, json> bound;  // shape id -> boundElements
    std::vector<json> pending;
    for (const auto& e : b.elements) {
        const Resolved s = resolve(b, e);
        if (e.kind == Kind::Group) continue;
        json o;
        if (e.kind == Kind::Connector) {
            const auto pts = connectorPath(b, e);
            if (pts.size() < 2) continue;
            o = exBase(b, e, s, e.endArrow == "none" && e.startArrow == "none" ? "line" : "arrow");
            o["x"] = pts.front().x;
            o["y"] = pts.front().y;
            json p = json::array();
            Box bb;
            for (const auto& q : pts) {
                p.push_back({q.x - pts.front().x, q.y - pts.front().y});
                bb.add(q.x, q.y);
            }
            o["width"] = bb.w();
            o["height"] = bb.h();
            o["points"] = p;
            o["lastCommittedPoint"] = nullptr;
            o["startArrowhead"] = exHead(e.startArrow).empty() ? json(nullptr) : json(exHead(e.startArrow));
            o["endArrowhead"] = exHead(e.endArrow).empty() ? json(nullptr) : json(exHead(e.endArrow));
            const Element* a = e.from.empty() ? nullptr : b.get(e.from);
            const Element* z = e.to.empty() ? nullptr : b.get(e.to);
            o["startBinding"] = a ? json{{"elementId", a->id}, {"focus", 0}, {"gap", 1}} : json(nullptr);
            o["endBinding"] = z ? json{{"elementId", z->id}, {"focus", 0}, {"gap", 1}} : json(nullptr);
            if (a) bound[a->id].push_back({{"type", "arrow"}, {"id", e.id}});
            if (z && z != a) bound[z->id].push_back({{"type", "arrow"}, {"id", e.id}});
            if (e.route == "elbow") o["elbowed"] = true;
            if (e.route == "curved") o["roundness"] = {{"type", 2}};
            o["customData"] = {{"achclwss", {{"route", e.route}}}};
            els.push_back(o);
            if (!e.text.empty()) {
                const Pt mid = pointAt(pts, length(pts) / 2);
                Box area{mid.x - 110, mid.y - 20, mid.x + 110, mid.y + 20, false};
                json t = exText(b, e, s, e.id + "-label", e.id, area);
                bound[e.id].push_back({{"type", "text"}, {"id", e.id + "-label"}});
                pending.push_back(t);
            }
            continue;
        }
        if (e.kind == Kind::Ink) {
            o = exBase(b, e, s, "freedraw");
            if (e.ink.empty()) continue;
            Box bb = elementBox(b, e);
            o["x"] = e.ink.front().x;
            o["y"] = e.ink.front().y;
            o["width"] = bb.w();
            o["height"] = bb.h();
            json p = json::array(), pr = json::array();
            for (const auto& q : e.ink) {
                p.push_back({q.x - e.ink.front().x, q.y - e.ink.front().y});
                pr.push_back(q.pressure);
            }
            o["points"] = p;
            o["pressures"] = pr;
            o["simulatePressure"] = false;
            o["strokeWidth"] = std::max(1.0, e.inkWidth / 2.5);
            o["lastCommittedPoint"] = nullptr;
            o["backgroundColor"] = "transparent";
            els.push_back(o);
            continue;
        }
        if (e.kind == Kind::Text) {
            els.push_back(exText(b, e, s, e.id, "", {}));
            continue;
        }
        if (e.kind == Kind::Image) {
            o = exBase(b, e, s, "image");
            std::string uri = e.src;
            if (uri.rfind("data:", 0) != 0) {
                auto img = loadImageSrc(e.src);
                if (!img) continue;
                uri = "data:image/png;base64," + base64Encode(raster::encodePng(*img));
            }
            const std::string fid = "file-" + e.id;
            const auto semi = uri.find(';');
            files[fid] = {{"mimeType", uri.substr(5, semi == std::string::npos ? 9 : semi - 5)}, {"id", fid}, {"dataURL", uri}, {"created", 1}};
            o["fileId"] = fid;
            o["status"] = "saved";
            o["scale"] = {1, 1};
            o["strokeColor"] = "transparent";
            o["backgroundColor"] = "transparent";
            els.push_back(o);
            continue;
        }
        if (e.kind == Kind::Frame) {
            o = exBase(b, e, s, "frame");
            o["name"] = e.text.empty() ? json(nullptr) : json(e.text);
            o["strokeColor"] = "#bbb";
            o["backgroundColor"] = "transparent";
            o["roughness"] = 0;
            els.push_back(o);
            continue;
        }
        const char* type = "rectangle";
        if (e.kind == Kind::Ellipse || e.kind == Kind::Cloud) type = "ellipse";
        else if (e.kind == Kind::Diamond) type = "diamond";
        o = exBase(b, e, s, type);
        if (e.kind == Kind::Rounded || e.kind == Kind::Stadium || e.kind == Kind::Cylinder) o["roundness"] = {{"type", 3}};
        if (e.kind == Kind::Note && !s.stroke) o["strokeColor"] = hex6(s.fill);
        o["customData"] = {{"achclwss", {{"type", toString(e.kind)}, {"name", e.name}}}};
        els.push_back(o);
        if (!e.text.empty()) {
            json t = exText(b, e, s, e.id + "-label", e.id, textArea(e, s));
            bound[e.id].push_back({{"type", "text"}, {"id", e.id + "-label"}});
            pending.push_back(t);
        }
    }
    for (auto& p : pending) els.push_back(p);
    for (auto& o : els) {
        auto it = bound.find(o["id"].get<std::string>());
        if (it != bound.end()) o["boundElements"] = it->second;
    }
    json doc;
    doc["type"] = "excalidraw";
    doc["version"] = 2;
    doc["source"] = "https://github.com/achclwss";
    doc["elements"] = els;
    doc["appState"] = {{"viewBackgroundColor", hex6(boardBackground(b))}, {"gridSize", nullptr}, {"name", b.title}};
    doc["files"] = files;
    return doc.dump(1);
}

namespace {

std::string jstr(const json& j, const char* k, const std::string& f = {}) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : f;
}
double jnum(const json& j, const char* k, double f = 0) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<double>() : f;
}

void exStyle(Element& e, const json& o, bool shape) {
    const std::string stroke = jstr(o, "strokeColor"), bg = jstr(o, "backgroundColor");
    if (!stroke.empty()) e.style.stroke = stroke == "transparent" ? "none" : stroke;
    if (shape && !bg.empty()) e.style.fill = bg == "transparent" ? "none" : bg;
    const std::string fs = jstr(o, "fillStyle");
    if (shape && !fs.empty()) e.style.fillStyle = fs == "hachure" ? "hachure" : (fs == "cross-hatch" ? "cross" : "solid");
    if (o.contains("strokeWidth")) e.style.strokeWidth = jnum(o, "strokeWidth", 2);
    const std::string ss = jstr(o, "strokeStyle");
    if (ss == "dashed" || ss == "dotted") e.style.dash = ss;
    const double r = jnum(o, "roughness", 1);
    e.style.sketch = r > 0;
    if (r > 0) e.style.roughness = r;
    const double op = jnum(o, "opacity", 100);
    if (op < 100) e.style.opacity = op / 100;
    e.link = jstr(o, "link");
}

std::string fromExHead(const json& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) return "none";
    const std::string h = it->get<std::string>();
    if (h == "dot" || h == "circle" || h == "circle_outline") return "circle";
    if (h == "triangle" || h == "triangle_outline") return "triangle";
    if (h == "diamond" || h == "diamond_outline") return "diamond";
    if (h == "bar") return "bar";
    return "arrow";
}

}  // namespace

Board fromExcalidraw(const std::string& text) {
    json doc;
    try {
        doc = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    json elements;
    if (doc.is_object() && doc.contains("elements")) elements = doc["elements"];
    else if (doc.is_array()) elements = doc;
    else throw std::runtime_error("not an Excalidraw scene (no \"elements\")");
    Board b;
    b.theme = "sketchy";
    if (doc.is_object() && doc.contains("appState") && doc["appState"].is_object()) {
        const std::string bg = jstr(doc["appState"], "viewBackgroundColor");
        if (!bg.empty() && validColor(bg)) b.background = bg == "transparent" ? "#ffffff" : bg;
        b.title = jstr(doc["appState"], "name");
    }
    const json files = doc.is_object() && doc.contains("files") ? doc["files"] : json::object();
    std::map<std::string, std::string> idMap;  // excalidraw id -> ours
    std::map<std::string, std::string> groups;
    std::vector<std::pair<std::string, json>> later;  // texts in containers, arrows
    bool anyRough = false;
    auto groupFor = [&](const std::string& gid) {
        auto it = groups.find(gid);
        if (it != groups.end()) return it->second;
        Element g;
        g.kind = Kind::Group;
        const std::string id = b.add(g);
        groups[gid] = id;
        return id;
    };
    auto parentOf = [&](Element& e, const json& o) {
        if (o.contains("groupIds") && o["groupIds"].is_array() && !o["groupIds"].empty()) {
            const auto& gs = o["groupIds"];
            e.parent = groupFor(gs[0].get<std::string>());
            for (std::size_t i = 0; i + 1 < gs.size(); ++i) {
                Element* g = b.get(groupFor(gs[i].get<std::string>()));
                if (g && g->parent.empty()) g->parent = groupFor(gs[i + 1].get<std::string>());
            }
        }
    };
    // pass 1: shapes, frames, images, ink, free text
    for (const auto& o : elements) {
        if (!o.is_object() || o.value("isDeleted", false)) continue;
        const std::string type = jstr(o, "type"), exId = jstr(o, "id");
        Element e;
        e.x = jnum(o, "x");
        e.y = jnum(o, "y");
        e.w = jnum(o, "width");
        e.h = jnum(o, "height");
        if (jnum(o, "roughness", 0) > 0) anyRough = true;
        if (type == "rectangle" || type == "ellipse" || type == "diamond") {
            e.kind = type == "ellipse" ? Kind::Ellipse : type == "diamond" ? Kind::Diamond : (o.contains("roundness") && !o["roundness"].is_null() ? Kind::Rounded : Kind::Rect);
            if (o.contains("customData") && o["customData"].is_object() && o["customData"].contains("achclwss")) {
                if (auto k = parseKind(jstr(o["customData"]["achclwss"], "type"))) e.kind = *k;
                e.name = jstr(o["customData"]["achclwss"], "name");
            }
            exStyle(e, o, true);
        } else if (type == "frame" || type == "magicframe") {
            e.kind = Kind::Frame;
            e.text = jstr(o, "name", "Frame");
        } else if (type == "image") {
            e.kind = Kind::Image;
            const std::string fid = jstr(o, "fileId");
            if (files.contains(fid)) e.src = jstr(files[fid], "dataURL");
        } else if (type == "freedraw") {
            e.kind = Kind::Ink;
            exStyle(e, o, false);
            e.inkWidth = jnum(o, "strokeWidth", 2) * 2.5;
            const bool sim = o.value("simulatePressure", true);
            const json pr = o.contains("pressures") ? o["pressures"] : json::array();
            if (o.contains("points")) {
                std::size_t i = 0;
                for (const auto& p : o["points"]) {
                    if (!p.is_array() || p.size() < 2) continue;
                    const double pressure = !sim && i < pr.size() && pr[i].is_number() ? pr[i].get<double>() : 0.6;
                    e.ink.push_back({e.x + p[0].get<double>(), e.y + p[1].get<double>(), pressure});
                    ++i;
                }
            }
            e.style.sketch.reset();
            e.style.roughness.reset();
        } else if (type == "text") {
            const std::string container = o.contains("containerId") && o["containerId"].is_string() ? o["containerId"].get<std::string>() : "";
            if (!container.empty()) {
                later.push_back({exId, o});
                continue;
            }
            e.kind = Kind::Text;
            e.text = jstr(o, "originalText", jstr(o, "text"));
            const std::string sc = jstr(o, "strokeColor");
            if (!sc.empty() && sc != "transparent") e.style.textColor = sc;
            e.style.fontSize = jnum(o, "fontSize", 20);
            const int ff = static_cast<int>(jnum(o, "fontFamily", 1));
            if (ff == 3) e.style.font = "mono";
            const std::string ta = jstr(o, "textAlign");
            if (!ta.empty()) e.style.align = ta;
        } else if (type == "arrow" || type == "line") {
            later.push_back({exId, o});
            continue;
        } else {
            continue;
        }
        parentOf(e, o);
        idMap[exId] = b.add(e);
    }
    // frames: frameId
    for (const auto& o : elements) {
        if (!o.is_object() || !o.contains("frameId") || !o["frameId"].is_string()) continue;
        auto me = idMap.find(jstr(o, "id"));
        auto fr = idMap.find(o["frameId"].get<std::string>());
        if (me == idMap.end() || fr == idMap.end()) continue;
        Element* e = b.get(me->second);
        if (!e) continue;
        if (e->parent.empty()) e->parent = fr->second;
        else if (Element* g = b.get(e->parent)) {  // the outermost group goes in the frame
            while (!g->parent.empty() && b.get(g->parent)) g = b.get(g->parent);
            if (g->kind == Kind::Group && g->parent.empty()) g->parent = fr->second;
        }
    }
    // pass 2: arrows, then container labels
    for (const auto& [exId, o] : later) {
        if (jstr(o, "type") == "text") continue;
        Element c;
        c.kind = Kind::Connector;
        exStyle(c, o, false);
        const double x = jnum(o, "x"), y = jnum(o, "y");
        std::vector<Pt> pts;
        if (o.contains("points")) {
            for (const auto& p : o["points"]) {
                if (p.is_array() && p.size() >= 2) pts.push_back({x + p[0].get<double>(), y + p[1].get<double>()});
            }
        }
        if (pts.size() < 2) continue;
        c.start = pts.front();
        c.end = pts.back();
        c.startArrow = fromExHead(o, "startArrowhead");
        c.endArrow = jstr(o, "type") == "line" && !o.contains("endArrowhead") ? "none" : fromExHead(o, "endArrowhead");
        auto bindingId = [&](const char* key) -> std::string {
            if (!o.contains(key) || !o[key].is_object()) return {};
            auto it = idMap.find(jstr(o[key], "elementId"));
            return it == idMap.end() ? std::string() : it->second;
        };
        c.from = bindingId("startBinding");
        c.to = bindingId("endBinding");
        std::string route = o.value("elbowed", false) ? "elbow" : (o.contains("roundness") && !o["roundness"].is_null() ? "curved" : "straight");
        if (o.contains("customData") && o["customData"].is_object() && o["customData"].contains("achclwss")) {
            const std::string r = jstr(o["customData"]["achclwss"], "route");
            if (!r.empty()) route = r;
        }
        c.route = route;
        const bool ours = o.contains("customData") && o["customData"].is_object() && o["customData"].contains("achclwss");
        if (pts.size() > 2 && (route == "straight" || (route == "curved" && !ours))) c.waypoints.assign(pts.begin() + 1, pts.end() - 1);
        parentOf(c, o);
        idMap[exId] = b.add(c);
    }
    for (const auto& [exId, o] : later) {
        if (jstr(o, "type") != "text") continue;
        auto it = idMap.find(o["containerId"].get<std::string>());
        Element* host = it == idMap.end() ? nullptr : b.get(it->second);
        const std::string txt = jstr(o, "originalText", jstr(o, "text"));
        if (!host) continue;
        host->text = txt;
        const std::string sc = jstr(o, "strokeColor");
        if (!sc.empty() && sc != "transparent") host->style.textColor = sc;
        if (o.contains("fontSize")) host->style.fontSize = jnum(o, "fontSize", 20);
        if (static_cast<int>(jnum(o, "fontFamily", 1)) == 3) host->style.font = "mono";
    }
    if (!anyRough) b.theme = "default";
    refreshGroups(b);
    return b;
}

// ---- XML (for draw.io) -------------------------------------------------------------------------

namespace {

struct XNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XNode> kids;
    std::string text;
    const std::string* attr(const std::string& k) const {
        for (const auto& [a, v] : attrs) {
            if (a == k) return &v;
        }
        return nullptr;
    }
    std::string get(const std::string& k, const std::string& f = {}) const {
        const std::string* v = attr(k);
        return v ? *v : f;
    }
    const XNode* child(const std::string& n) const {
        for (const auto& k : kids) {
            if (k.name == n) return &k;
        }
        return nullptr;
    }
};

std::string unescapeXml(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            o += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") o += '&';
        else if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (ent == "nbsp") o += pdf::utf8Encode(0xA0);
        else if (!ent.empty() && ent[0] == '#') {
            try {
                const unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? static_cast<unsigned>(std::stoul(ent.substr(2), nullptr, 16))
                                                                                         : static_cast<unsigned>(std::stoul(ent.substr(1)));
                o += pdf::utf8Encode(cp);
            } catch (const std::exception&) {
                o += s.substr(i, semi - i + 1);
            }
        } else {
            o += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return o;
}

struct XmlParser {
    const std::string& s;
    std::size_t i = 0;
    void skipMisc() {
        while (i < s.size()) {
            if (s.compare(i, 4, "<!--") == 0) {
                const auto e = s.find("-->", i);
                i = e == std::string::npos ? s.size() : e + 3;
            } else if (s.compare(i, 2, "<?") == 0) {
                const auto e = s.find("?>", i);
                i = e == std::string::npos ? s.size() : e + 2;
            } else if (s.compare(i, 2, "<!") == 0) {
                const auto e = s.find('>', i);
                i = e == std::string::npos ? s.size() : e + 1;
            } else {
                break;
            }
        }
    }
    XNode element(int depth = 0) {
        if (depth > 200) throw std::runtime_error("XML nested too deeply");
        XNode n;
        if (i >= s.size() || s[i] != '<') throw std::runtime_error("bad XML");
        ++i;
        const std::size_t ns = i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '>' && s[i] != '/') ++i;
        n.name = s.substr(ns, i - ns);
        while (true) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size()) throw std::runtime_error("unexpected end of XML");
            if (s[i] == '/') {
                i = s.find('>', i);
                if (i == std::string::npos) throw std::runtime_error("bad XML");
                ++i;
                return n;
            }
            if (s[i] == '>') {
                ++i;
                break;
            }
            const std::size_t as = i;
            while (i < s.size() && s[i] != '=' && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '>') ++i;
            const std::string key = s.substr(as, i - as);
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i < s.size() && s[i] == '=') {
                ++i;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                const char q = i < s.size() ? s[i] : '"';
                if (q != '"' && q != '\'') throw std::runtime_error("bad XML attribute");
                const auto e = s.find(q, i + 1);
                if (e == std::string::npos) throw std::runtime_error("bad XML attribute");
                n.attrs.push_back({key, unescapeXml(s.substr(i + 1, e - i - 1))});
                i = e + 1;
            }
        }
        while (i < s.size()) {
            if (s.compare(i, 2, "</") == 0) {
                i = s.find('>', i);
                if (i == std::string::npos) throw std::runtime_error("bad XML");
                ++i;
                return n;
            }
            if (s.compare(i, 9, "<![CDATA[") == 0) {
                const auto e = s.find("]]>", i);
                n.text += s.substr(i + 9, e == std::string::npos ? std::string::npos : e - i - 9);
                i = e == std::string::npos ? s.size() : e + 3;
                continue;
            }
            if (s.compare(i, 4, "<!--") == 0 || s.compare(i, 2, "<?") == 0) {
                skipMisc();
                continue;
            }
            if (s[i] == '<') {
                n.kids.push_back(element(depth + 1));
                continue;
            }
            const auto e = s.find('<', i);
            n.text += unescapeXml(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
            i = e == std::string::npos ? s.size() : e;
        }
        return n;
    }
    XNode parse() {
        skipMisc();
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
            skipMisc();
        }
        return element();
    }
};

std::string xmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\n': o += "&#xa;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string urlDecode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            try {
                o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                i += 2;
                continue;
            } catch (const std::exception&) {
            }
        }
        o += s[i];
    }
    return o;
}

std::map<std::string, std::string> parseStyle(const std::string& style, std::string& base) {
    std::map<std::string, std::string> m;
    std::string item;
    bool first = true;
    auto flush = [&] {
        if (item.empty()) return;
        const auto eq = item.find('=');
        if (eq == std::string::npos) {
            if (first) base = item;
            m[item] = "1";
        } else {
            m[item.substr(0, eq)] = item.substr(eq + 1);
        }
        first = false;
        item.clear();
    };
    for (char c : style) {
        if (c == ';') flush();
        else item += c;
    }
    flush();
    return m;
}

// html=1 labels: tags out, <br>/<div> to new lines, entities decoded.
std::string htmlText(const std::string& v) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '<') {
            const auto e = v.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = lower(v.substr(i + 1, e - i - 1));
            if (tag.rfind("br", 0) == 0 || ((tag.rfind("div", 0) == 0 || tag.rfind("p", 0) == 0) && !o.empty() && o.back() != '\n')) o += '\n';
            i = e;
            continue;
        }
        o += v[i];
    }
    while (!o.empty() && o.back() == '\n') o.pop_back();
    return unescapeXml(o);
}

std::string htmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '\n') o += "<br>";
        else o += c;
    }
    return o;
}

std::string dioArrow(const std::string& a, std::string& fill) {
    fill = "1";
    if (a == "none") return "none";
    if (a == "arrow") {
        fill = "0";
        return "open";
    }
    if (a == "triangle") return "classic";
    if (a == "diamond") return "diamond";
    if (a == "circle") return "oval";
    if (a == "bar") return "dash";
    if (a == "cross") return "cross";
    return "classic";
}

std::string fromDioArrow(const std::string& a) {
    if (a.empty()) return "";
    if (a == "none") return "none";
    if (a == "open" || a == "openThin" || a == "openAsync") return "arrow";
    if (a == "diamond" || a == "diamondThin") return "diamond";
    if (a == "oval" || a == "circle" || a == "circlePlus" || a == "dot") return "circle";
    if (a == "dash" || a == "ERmandOne" || a == "baseDash") return "bar";
    if (a == "cross") return "cross";
    return "triangle";
}

void anchorXY(const std::string& anchor, double& x, double& y) {
    x = 0.5;
    y = 0.5;
    if (anchor == "top") y = 0;
    else if (anchor == "bottom") y = 1;
    else if (anchor == "left") x = 0;
    else if (anchor == "right") x = 1;
}

}  // namespace

std::string toDrawio(const Board& b) {
    std::ostringstream s;
    const std::string title = b.title.empty() ? "Board" : b.title;
    s << "<mxfile host=\"achclwss\" agent=\"ACHCLWSS board\" version=\"24.7.0\" type=\"device\">\n";
    s << "  <diagram id=\"achclwss-board\" name=\"" << xmlEsc(title) << "\">\n";
    s << "    <mxGraphModel dx=\"1000\" dy=\"800\" grid=\"1\" gridSize=\"10\" guides=\"1\" tooltips=\"1\" connect=\"1\" arrows=\"1\" fold=\"1\" "
         "page=\"0\" pageScale=\"1\" pageWidth=\"1169\" pageHeight=\"827\" background=\""
      << hex6(boardBackground(b)) << "\" math=\"0\" shadow=\"0\">\n      <root>\n        <mxCell id=\"0\" />\n";
    std::map<std::string, std::string> layerCell;
    for (std::size_t i = 0; i < b.layers.size(); ++i) {
        const std::string id = i == 0 ? "1" : "layer-" + std::to_string(i);
        layerCell[b.layers[i].name] = id;
        s << "        <mxCell id=\"" << id << "\"" << (i == 0 && b.layers[i].name == "default" ? "" : " value=\"" + xmlEsc(b.layers[i].name) + "\"")
          << " parent=\"0\"" << (b.layers[i].visible ? "" : " visible=\"0\"") << (b.layers[i].locked ? " style=\"locked=1;\"" : "") << " />\n";
    }
    auto absBox = [&](const Element& e) { return elementBox(b, e); };
    auto parentCell = [&](const Element& e, double& ox, double& oy) {
        ox = oy = 0;
        const Element* p = e.parent.empty() ? nullptr : b.get(e.parent);
        if (p && (p->kind == Kind::Frame || p->kind == Kind::Group)) {
            const Box pb = absBox(*p);
            ox = pb.x0;
            oy = pb.y0;
            return p->id;
        }
        auto it = layerCell.find(e.layer);
        return it == layerCell.end() ? std::string("1") : it->second;
    };
    // containers must come before their children: frames and groups first, outer first
    std::vector<const Element*> order;
    std::set<std::string> placed;
    std::function<void(const Element&)> visit = [&](const Element& e) {
        if (placed.count(e.id)) return;
        if (!e.parent.empty()) {
            if (const Element* p = b.get(e.parent)) visit(*p);
        }
        placed.insert(e.id);
        order.push_back(&e);
    };
    for (const auto& e : b.elements) {
        if (e.kind != Kind::Connector) visit(e);
    }
    for (const auto& e : b.elements) {
        if (e.kind == Kind::Connector) visit(e);
    }
    for (const Element* ep : order) {
        const Element& e = *ep;
        const Resolved r = resolve(b, e);
        std::string style;
        auto common = [&](bool shape) {
            if (shape) style += "fillColor=" + (r.fill && r.fillStyle != "none" ? hex6(r.fill) : std::string("none")) + ";";
            style += "strokeColor=" + (r.stroke ? hex6(r.stroke) : std::string("none")) + ";";
            style += "fontColor=" + hex6(r.text) + ";strokeWidth=" + fmtNum(r.strokeWidth) + ";fontSize=" + fmtNum(r.fontSize) + ";";
            if (r.font == "serif") style += "fontFamily=Times New Roman;";
            else if (r.font == "mono") style += "fontFamily=Courier New;";
            const int fs = (r.bold ? 1 : 0) | (r.italic ? 2 : 0);
            if (fs) style += "fontStyle=" + std::to_string(fs) + ";";
            if (r.dash == "dashed") style += "dashed=1;";
            if (r.dash == "dotted") style += "dashed=1;dashPattern=1 4;";
            if (r.opacity < 1) style += "opacity=" + std::to_string(std::lround(r.opacity * 100)) + ";";
            if (r.sketch) style += "sketch=1;curveFitting=1;jiggle=2;";
            if (shape && r.fillStyle == "hachure") style += "fillStyle=hachure;";
            if (shape && r.fillStyle == "cross") style += "fillStyle=cross-hatch;";
            if (r.shadow) style += "shadow=1;";
        };
        if (e.kind == Kind::Connector) {
            const auto pts = connectorPath(b, e);
            if (e.route == "elbow") style = "edgeStyle=orthogonalEdgeStyle;rounded=0;orthogonalLoop=1;jettySize=auto;";
            else if (e.route == "curved") style = "edgeStyle=orthogonalEdgeStyle;curved=1;";
            else style = "rounded=0;";
            style += "html=1;";
            std::string ef, sf;
            style += "endArrow=" + dioArrow(e.endArrow, ef) + ";endFill=" + ef + ";startArrow=" + dioArrow(e.startArrow, sf) + ";startFill=" + sf + ";";
            const Element* a = e.from.empty() ? nullptr : b.get(e.from);
            const Element* z = e.to.empty() ? nullptr : b.get(e.to);
            if (a && e.fromAnchor != "auto") {
                double x, y;
                anchorXY(e.fromAnchor, x, y);
                style += "exitX=" + fmtNum(x) + ";exitY=" + fmtNum(y) + ";exitDx=0;exitDy=0;";
            }
            if (z && e.toAnchor != "auto") {
                double x, y;
                anchorXY(e.toAnchor, x, y);
                style += "entryX=" + fmtNum(x) + ";entryY=" + fmtNum(y) + ";entryDx=0;entryDy=0;";
            }
            common(false);
            auto it = layerCell.find(e.layer);
            s << "        <mxCell id=\"" << xmlEsc(e.id) << "\" value=\"" << xmlEsc(htmlEsc(e.text)) << "\" style=\"" << xmlEsc(style) << "\" edge=\"1\" parent=\""
              << (it == layerCell.end() ? "1" : it->second) << "\"" << (a ? " source=\"" + xmlEsc(a->id) + "\"" : "")
              << (z ? " target=\"" + xmlEsc(z->id) + "\"" : "") << ">\n          <mxGeometry relative=\"1\" as=\"geometry\">\n";
            if (!pts.empty()) {
                if (!a) s << "            <mxPoint x=\"" << fmtNum(pts.front().x) << "\" y=\"" << fmtNum(pts.front().y) << "\" as=\"sourcePoint\" />\n";
                if (!z) s << "            <mxPoint x=\"" << fmtNum(pts.back().x) << "\" y=\"" << fmtNum(pts.back().y) << "\" as=\"targetPoint\" />\n";
            }
            if (!e.waypoints.empty()) {
                s << "            <Array as=\"points\">\n";
                for (const auto& w : e.waypoints) s << "              <mxPoint x=\"" << fmtNum(w.x) << "\" y=\"" << fmtNum(w.y) << "\" />\n";
                s << "            </Array>\n";
            }
            s << "          </mxGeometry>\n        </mxCell>\n";
            continue;
        }
        if (e.kind == Kind::Ink) {
            if (e.ink.size() < 2) continue;
            style = "curved=1;endArrow=none;html=1;strokeColor=" + (r.stroke ? hex6(r.stroke) : hex6(r.text)) + ";strokeWidth=" + fmtNum(std::max(1.0, e.inkWidth * 0.7)) + ";";
            auto it = layerCell.find(e.layer);
            s << "        <mxCell id=\"" << xmlEsc(e.id) << "\" value=\"\" style=\"" << xmlEsc(style) << "\" edge=\"1\" parent=\""
              << (it == layerCell.end() ? "1" : it->second) << "\">\n          <mxGeometry relative=\"1\" as=\"geometry\">\n";
            s << "            <mxPoint x=\"" << fmtNum(e.ink.front().x) << "\" y=\"" << fmtNum(e.ink.front().y) << "\" as=\"sourcePoint\" />\n";
            s << "            <mxPoint x=\"" << fmtNum(e.ink.back().x) << "\" y=\"" << fmtNum(e.ink.back().y) << "\" as=\"targetPoint\" />\n";
            s << "            <Array as=\"points\">\n";
            const std::size_t step = std::max<std::size_t>(1, e.ink.size() / 60);
            for (std::size_t i = step; i + 1 < e.ink.size(); i += step)
                s << "              <mxPoint x=\"" << fmtNum(std::round(e.ink[i].x)) << "\" y=\"" << fmtNum(std::round(e.ink[i].y)) << "\" />\n";
            s << "            </Array>\n          </mxGeometry>\n        </mxCell>\n";
            continue;
        }
        switch (e.kind) {
            case Kind::Rect: style = "rounded=0;whiteSpace=wrap;html=1;"; break;
            case Kind::Rounded: style = "rounded=1;whiteSpace=wrap;html=1;"; break;
            case Kind::Stadium: style = "rounded=1;arcSize=50;whiteSpace=wrap;html=1;"; break;
            case Kind::Ellipse: style = "ellipse;whiteSpace=wrap;html=1;"; break;
            case Kind::Diamond: style = "rhombus;whiteSpace=wrap;html=1;"; break;
            case Kind::Hexagon: style = "shape=hexagon;perimeter=hexagonPerimeter2;whiteSpace=wrap;html=1;fixedSize=1;"; break;
            case Kind::Parallelogram: style = "shape=parallelogram;perimeter=parallelogramPerimeter;whiteSpace=wrap;html=1;fixedSize=1;"; break;
            case Kind::Trapezoid: style = "shape=trapezoid;perimeter=trapezoidPerimeter;whiteSpace=wrap;html=1;fixedSize=1;"; break;
            case Kind::Triangle: style = "triangle;whiteSpace=wrap;html=1;direction=north;"; break;
            case Kind::Cylinder: style = "shape=cylinder3;whiteSpace=wrap;html=1;boundedLbl=1;backgroundOutline=1;size=12;"; break;
            case Kind::Cloud: style = "ellipse;shape=cloud;whiteSpace=wrap;html=1;"; break;
            case Kind::Note: style = "shape=note;whiteSpace=wrap;html=1;backgroundOutline=1;size=14;"; break;
            case Kind::Subroutine: style = "shape=process;whiteSpace=wrap;html=1;backgroundOutline=1;"; break;
            case Kind::Text: style = "text;html=1;whiteSpace=wrap;align=" + r.align + ";verticalAlign=top;"; break;
            case Kind::Frame: style = "rounded=0;whiteSpace=wrap;html=1;container=1;collapsible=0;verticalAlign=top;align=left;spacingLeft=8;"; break;
            case Kind::Group: style = "group;"; break;
            case Kind::Image: {
                std::string uri = e.src;
                if (uri.rfind("data:", 0) != 0) {
                    auto img = loadImageSrc(e.src);
                    uri = img ? "data:image/png;base64," + base64Encode(raster::encodePng(*img)) : "";
                }
                // draw.io writes data URIs without ";base64" inside styles (';' separates style keys)
                const auto semi = uri.find(";base64,");
                if (semi != std::string::npos) uri = uri.substr(0, semi) + "," + uri.substr(semi + 8);
                style = "shape=image;verticalLabelPosition=bottom;verticalAlign=top;imageAspect=0;aspect=fixed;image=" + uri + ";";
                break;
            }
            default: style = "whiteSpace=wrap;html=1;"; break;
        }
        if (e.kind != Kind::Group && e.kind != Kind::Image) {
            common(e.kind != Kind::Text);
            if (e.kind != Kind::Text && e.kind != Kind::Frame) style += "align=" + r.align + ";verticalAlign=" + r.valign + ";";
        }
        double ox = 0, oy = 0;
        const std::string parent = parentCell(e, ox, oy);
        const Box eb = absBox(e);
        s << "        <mxCell id=\"" << xmlEsc(e.id) << "\" value=\"" << xmlEsc(htmlEsc(e.text)) << "\" style=\"" << xmlEsc(style) << "\" vertex=\"1\""
          << (e.kind == Kind::Group ? " connectable=\"0\"" : "") << " parent=\"" << xmlEsc(parent) << "\">\n";
        s << "          <mxGeometry x=\"" << fmtNum(eb.x0 - ox) << "\" y=\"" << fmtNum(eb.y0 - oy) << "\" width=\"" << fmtNum(eb.w()) << "\" height=\""
          << fmtNum(eb.h()) << "\" as=\"geometry\" />\n        </mxCell>\n";
    }
    s << "      </root>\n    </mxGraphModel>\n  </diagram>\n</mxfile>\n";
    return s.str();
}

Board fromDrawio(const std::string& text) {
    XmlParser p{text};
    XNode root = p.parse();
    const XNode* model = nullptr;
    XNode inflated;
    std::string pageName;
    if (root.name == "mxGraphModel") {
        model = &root;
    } else if (root.name == "mxfile") {
        const XNode* diagram = root.child("diagram");
        if (!diagram) throw std::runtime_error("the .drawio file has no <diagram>");
        pageName = diagram->get("name");
        model = diagram->child("mxGraphModel");
        if (!model) {
            const std::string data = trim(diagram->text);
            if (data.empty()) throw std::runtime_error("the diagram page is empty");
            std::string xml;
            try {
                xml = urlDecode(pdf::inflate(base64Decode(data)));
            } catch (const std::exception&) {
                throw std::runtime_error("can't decompress the draw.io page (save it uncompressed: File > Properties > Compressed off)");
            }
            XmlParser q{xml};
            inflated = q.parse();
            model = &inflated;
        }
    } else {
        throw std::runtime_error("not a draw.io file (expected <mxfile> or <mxGraphModel>)");
    }
    const XNode* rootNode = model->child("root");
    if (!rootNode) throw std::runtime_error("the draw.io model has no <root>");
    Board b;
    b.title = pageName;
    const std::string bg = model->get("background");
    if (!bg.empty() && bg != "none" && validColor(bg)) b.background = bg;
    b.layers.clear();
    std::map<std::string, const XNode*> cells;
    std::vector<const XNode*> order;
    for (const auto& k : rootNode->kids) {
        const XNode* cell = &k;
        // <object>/<UserObject> wrap a cell and carry the label
        if ((k.name == "object" || k.name == "UserObject") && k.child("mxCell")) cell = &k;
        if (cell->name != "mxCell" && cell->name != "object" && cell->name != "UserObject") continue;
        cells[k.get("id")] = cell;
        order.push_back(cell);
    }
    auto inner = [](const XNode* n) { return n->name == "mxCell" ? n : n->child("mxCell"); };
    auto valueOf = [&](const XNode* n) { return n->name == "mxCell" ? n->get("value") : n->get("label", n->get("value")); };
    std::map<std::string, std::string> layerName;
    bool sketchy = false;
    for (const XNode* n : order) {
        const XNode* c = inner(n);
        const std::string parent = c->get("parent");
        const std::string id = n->get("id");
        if (id == "0" || c->get("vertex") == "1" || c->get("edge") == "1") continue;
        if (parent == "0" || (parent.empty() && id != "0")) {
            std::string name = htmlText(valueOf(n));
            if (name.empty()) name = layerName.empty() ? "default" : "layer " + std::to_string(layerName.size() + 1);
            layerName[id] = name;
            Layer l;
            l.name = name;
            l.visible = c->get("visible", "1") != "0";
            std::string base;
            l.locked = parseStyle(c->get("style"), base).count("locked") > 0;
            b.layers.push_back(l);
        }
    }
    if (b.layers.empty()) b.layers.push_back(Layer{});
    std::map<std::string, std::string> idMap;
    // absolute offset of a cell's parent chain (containers hold relative geometry)
    std::function<Pt(const std::string&, int)> originOf = [&](const std::string& pid, int depth) -> Pt {
        auto it = cells.find(pid);
        if (it == cells.end() || depth > 32) return {};
        const XNode* c = inner(it->second);
        if (c->get("vertex") != "1") return {};
        const XNode* g = c->child("mxGeometry");
        const Pt up = originOf(c->get("parent"), depth + 1);
        if (!g) return up;
        return {up.x + std::atof(g->get("x", "0").c_str()), up.y + std::atof(g->get("y", "0").c_str())};
    };
    auto layerFor = [&](const std::string& pid) {
        std::string cur = pid;
        for (int d = 0; d < 32; ++d) {
            auto l = layerName.find(cur);
            if (l != layerName.end()) return l->second;
            auto it = cells.find(cur);
            if (it == cells.end()) break;
            cur = inner(it->second)->get("parent");
        }
        return b.layers.front().name;
    };
    auto applyStyle = [&](Element& e, const std::map<std::string, std::string>& st, bool shape) {
        auto get = [&](const char* k) {
            auto it = st.find(k);
            return it == st.end() ? std::string() : it->second;
        };
        const std::string fill = get("fillColor"), stroke = get("strokeColor"), font = get("fontColor");
        if (shape && !fill.empty() && fill != "default") e.style.fill = fill == "none" ? "none" : fill;
        if (!stroke.empty() && stroke != "default") e.style.stroke = stroke == "none" ? "none" : stroke;
        if (!font.empty() && font != "default" && validColor(font)) e.style.textColor = font;
        if (!get("strokeWidth").empty()) e.style.strokeWidth = std::atof(get("strokeWidth").c_str());
        if (!get("fontSize").empty()) e.style.fontSize = std::atof(get("fontSize").c_str());
        if (get("dashed") == "1") e.style.dash = get("dashPattern").rfind("1 ", 0) == 0 ? "dotted" : "dashed";
        if (!get("opacity").empty()) e.style.opacity = std::atof(get("opacity").c_str()) / 100;
        const std::string ff = lower(get("fontFamily"));
        if (ff.find("times") != std::string::npos || ff.find("georgia") != std::string::npos || ff.find("serif") == 0) e.style.font = "serif";
        if (ff.find("courier") != std::string::npos || ff.find("mono") != std::string::npos) e.style.font = "mono";
        const int fs = std::atoi(get("fontStyle").c_str());
        if (fs & 1) e.style.bold = true;
        if (fs & 2) e.style.italic = true;
        if (get("sketch") == "1") {
            e.style.sketch = true;
            sketchy = true;
        }
        const std::string fst = get("fillStyle");
        if (shape && fst == "hachure") e.style.fillStyle = "hachure";
        if (shape && (fst == "cross-hatch" || fst == "cross")) e.style.fillStyle = "cross";
        if (get("shadow") == "1") e.style.shadow = true;
        const std::string al = get("align");
        if (al == "left" || al == "center" || al == "right") e.style.align = al;
        const std::string va = get("verticalAlign");
        if (va == "top" || va == "middle" || va == "bottom") e.style.valign = va;
    };
    std::vector<std::pair<const XNode*, std::map<std::string, std::string>>> edges;
    std::vector<std::pair<std::string, std::string>> edgeLabels;  // edge id, text
    // vertices first (parents before children come for free since geometry is resolved by id)
    for (const XNode* n : order) {
        const XNode* c = inner(n);
        if (c->get("vertex") != "1") {
            if (c->get("edge") == "1") {
                std::string base;
                edges.push_back({n, parseStyle(c->get("style"), base)});
            }
            continue;
        }
        std::string base;
        const auto st = parseStyle(c->get("style"), base);
        auto has = [&](const char* k) { return st.count(k) > 0; };
        auto val = [&](const char* k) {
            auto it = st.find(k);
            return it == st.end() ? std::string() : it->second;
        };
        const std::string parentId = c->get("parent");
        auto pit = cells.find(parentId);
        if (has("edgeLabel") && pit != cells.end() && inner(pit->second)->get("edge") == "1") {
            edgeLabels.push_back({parentId, htmlText(valueOf(n))});
            continue;
        }
        Element e;
        const std::string shape = val("shape");
        if (base == "text" || has("text")) e.kind = Kind::Text;
        else if (base == "group") e.kind = Kind::Group;
        else if (shape == "cloud") e.kind = Kind::Cloud;
        else if (base == "ellipse" || has("ellipse")) e.kind = Kind::Ellipse;
        else if (base == "rhombus" || has("rhombus")) e.kind = Kind::Diamond;
        else if (base == "triangle" || has("triangle")) e.kind = Kind::Triangle;
        else if (base == "swimlane" || val("container") == "1") e.kind = Kind::Frame;
        else if (shape == "hexagon") e.kind = Kind::Hexagon;
        else if (shape == "parallelogram") e.kind = Kind::Parallelogram;
        else if (shape == "trapezoid") e.kind = Kind::Trapezoid;
        else if (shape.rfind("cylinder", 0) == 0 || shape == "datastore") e.kind = Kind::Cylinder;
        else if (shape == "note" || shape == "note2") e.kind = Kind::Note;
        else if (shape == "process") e.kind = Kind::Subroutine;
        else if (shape == "image" || base == "image" || has("image")) e.kind = Kind::Image;
        else if (val("rounded") == "1") e.kind = val("arcSize") == "50" ? Kind::Stadium : Kind::Rounded;
        else e.kind = Kind::Rect;
        if (e.kind == Kind::Image) {
            std::string uri = val("image");
            // data:image/png,AAAA -> data:image/png;base64,AAAA
            if (uri.rfind("data:", 0) == 0 && uri.find(";base64,") == std::string::npos) {
                const auto comma = uri.find(',');
                if (comma != std::string::npos) uri = uri.substr(0, comma) + ";base64," + uri.substr(comma + 1);
            }
            e.src = uri;
        }
        e.text = htmlText(valueOf(n));
        if (const XNode* g = c->child("mxGeometry")) {
            const Pt o = originOf(parentId, 0);
            e.x = o.x + std::atof(g->get("x", "0").c_str());
            e.y = o.y + std::atof(g->get("y", "0").c_str());
            e.w = std::atof(g->get("width", "0").c_str());
            e.h = std::atof(g->get("height", "0").c_str());
        }
        applyStyle(e, st, true);
        if (e.kind == Kind::Frame && !e.style.align) e.style.align = "left";
        if (e.kind == Kind::Text && !e.style.valign) e.style.valign = "top";
        e.layer = layerFor(parentId);
        e.parent = parentId;  // resolved to our ids below
        idMap[n->get("id")] = b.add(e);
    }
    for (auto& e : b.elements) {
        if (e.parent.empty()) continue;
        auto it = idMap.find(e.parent);
        e.parent = it == idMap.end() ? std::string() : it->second;
    }
    for (const auto& [n, st] : edges) {
        const XNode* c = inner(n);
        Element e;
        e.kind = Kind::Connector;
        auto val = [&](const char* k) {
            auto it = st.find(k);
            return it == st.end() ? std::string() : it->second;
        };
        auto src = idMap.find(c->get("source")), dst = idMap.find(c->get("target"));
        if (src != idMap.end()) e.from = src->second;
        if (dst != idMap.end()) e.to = dst->second;
        const std::string es = val("edgeStyle");
        e.route = val("curved") == "1" ? "curved" : (!es.empty() && es != "none" ? "elbow" : "straight");
        const std::string ea = fromDioArrow(val("endArrow")), sa = fromDioArrow(val("startArrow"));
        e.endArrow = ea.empty() ? "triangle" : ea;
        if (e.endArrow == "triangle" && val("endFill") == "0") e.endArrow = "arrow";
        e.startArrow = sa.empty() ? "none" : sa;
        if (e.startArrow == "triangle" && val("startFill") == "0") e.startArrow = "arrow";
        auto anchor = [&](const char* kx, const char* ky) -> std::string {
            if (val(kx).empty() || val(ky).empty()) return "auto";
            const double x = std::atof(val(kx).c_str()), y = std::atof(val(ky).c_str());
            if (std::abs(x - 0.5) < 1e-6 && std::abs(y) < 1e-6) return "top";
            if (std::abs(x - 0.5) < 1e-6 && std::abs(y - 1) < 1e-6) return "bottom";
            if (std::abs(x) < 1e-6 && std::abs(y - 0.5) < 1e-6) return "left";
            if (std::abs(x - 1) < 1e-6 && std::abs(y - 0.5) < 1e-6) return "right";
            return fmtNum(x) + "," + fmtNum(y);
        };
        e.fromAnchor = anchor("exitX", "exitY");
        e.toAnchor = anchor("entryX", "entryY");
        e.text = htmlText(valueOf(n));
        if (const XNode* g = c->child("mxGeometry")) {
            const Pt o = originOf(c->get("parent"), 0);
            for (const auto& k : g->kids) {
                if (k.name == "mxPoint") {
                    const Pt pt{o.x + std::atof(k.get("x", "0").c_str()), o.y + std::atof(k.get("y", "0").c_str())};
                    if (k.get("as") == "sourcePoint") e.start = pt;
                    if (k.get("as") == "targetPoint") e.end = pt;
                } else if (k.name == "Array" && k.get("as") == "points" && e.route != "elbow") {
                    for (const auto& q : k.kids) e.waypoints.push_back({o.x + std::atof(q.get("x", "0").c_str()), o.y + std::atof(q.get("y", "0").c_str())});
                }
            }
        }
        applyStyle(e, st, false);
        e.layer = layerFor(c->get("parent"));
        idMap[n->get("id")] = b.add(e);
    }
    for (const auto& [eid, label] : edgeLabels) {
        auto it = idMap.find(eid);
        if (it == idMap.end()) continue;
        if (Element* e = b.get(it->second)) e->text += (e->text.empty() ? "" : "\n") + label;
    }
    if (sketchy) b.theme = "sketchy";
    refreshGroups(b);
    return b;
}

}  // namespace achclwss::board
