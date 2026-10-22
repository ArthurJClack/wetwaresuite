// .circuit.json files, KiCad netlist import, the document class and snapshot undo.
#include <map>
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "wetwaresuite/circuit/circuit.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::circuit {

using json = nlohmann::json;

namespace {

json pt(Pt p) { return json::array({p.x, p.y}); }

Pt readPt(const json& j) {
    if (!j.is_array() || j.size() < 2) throw std::runtime_error("a point should be [x, y]");
    return {j[0].get<double>(), j[1].get<double>()};
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- s-expressions ----------------------------------------------------------------------

struct Sx {
    std::string atom;
    std::vector<Sx> list;
    bool isList = false;
    const Sx* child(const std::string& head) const {
        for (const auto& c : list) {
            if (c.isList && !c.list.empty() && !c.list[0].isList && c.list[0].atom == head) return &c;
        }
        return nullptr;
    }
    std::string value(const std::string& head) const {
        const Sx* c = child(head);
        return c && c->list.size() > 1 && !c->list[1].isList ? c->list[1].atom : std::string();
    }
};

Sx parseSx(const std::string& s, std::size_t& i, int depth = 0) {
    if (depth > 200) throw std::runtime_error("netlist nested too deeply");
    auto skip = [&] {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    };
    skip();
    Sx node;
    if (i >= s.size()) throw std::runtime_error("unexpected end of netlist");
    if (s[i] == '(') {
        node.isList = true;
        ++i;
        for (;;) {
            skip();
            if (i >= s.size()) throw std::runtime_error("unbalanced parentheses in netlist");
            if (s[i] == ')') {
                ++i;
                break;
            }
            node.list.push_back(parseSx(s, i, depth + 1));
        }
    } else if (s[i] == '"') {
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            node.atom += s[i++];
        }
        ++i;
    } else {
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' && s[i] != ')') node.atom += s[i++];
    }
    return node;
}

std::string mapKicadPart(const std::string& partIn, int& pins) {
    const std::string p = lower(partIn);
    auto starts = [&](const char* s) { return p.rfind(s, 0) == 0; };
    auto count = [&](const char* prefix) {
        std::string digits;
        for (std::size_t i = std::string(prefix).size(); i < p.size() && std::isdigit(static_cast<unsigned char>(p[i])); ++i) digits += p[i];
        return digits.empty() ? 0 : std::stoi(digits);
    };
    if (p == "r" || p == "r_small" || p == "r_us" || p == "r_small_us") return "resistor";
    if (starts("r_pot") || starts("potentiometer")) return "potentiometer";
    if (p == "c" || p == "c_small") return "capacitor";
    if (starts("c_polarized") || starts("cp") || p == "c_pol") return "capacitor_polarized";
    if (p == "l" || p == "l_small" || starts("l_core")) return "inductor";
    if (starts("led")) return "led";
    if (p == "d" || starts("d_") || starts("1n4")) return "diode";
    if (starts("q_npn")) return "npn";
    if (starts("q_pnp")) return "pnp";
    if (starts("q_nmos")) return "nmos";
    if (starts("q_pmos")) return "pmos";
    if (starts("crystal")) return "crystal";
    if (starts("sw_push")) return "switch";
    if (starts("sw_spdt")) return "switch_spdt";
    if (starts("conn_01x")) {
        pins = count("conn_01x");
        return "header";
    }
    if (starts("conn_02x")) {
        pins = 2 * count("conn_02x");
        return "header2";
    }
    if (starts("battery")) return "battery";
    if (starts("arduino_uno")) return "arduino_uno";
    if (starts("arduino_nano")) return "arduino_nano";
    return canonicalSymbol(p);
}

}  // namespace

std::string toJson(const Circuit& c) {
    json j;
    j["format"] = "wetwaresuite-circuit";
    j["version"] = 1;
    j["title"] = c.title;
    json parts = json::array();
    for (const auto& p : c.parts) {
        json q;
        q["ref"] = p.ref;
        q["symbol"] = p.symbol;
        q["value"] = p.value;
        q["footprint"] = p.footprint;
        if (p.pins) q["pins"] = p.pins;
        if (!p.pinNames.empty()) q["pin_names"] = p.pinNames;
        q["at"] = pt(p.pos);
        q["rot"] = p.rot;
        if (p.mirror) q["mirror"] = true;
        if (!p.fields.empty()) q["fields"] = p.fields;
        if (p.pcb.placed) {
            q["pcb"] = {{"at", pt(p.pcb.pos)}, {"rot", p.pcb.rot}, {"side", p.pcb.bottom ? "bottom" : "top"}};
        }
        parts.push_back(std::move(q));
    }
    j["parts"] = std::move(parts);
    json wires = json::array();
    for (const auto& w : c.wires) {
        json pts = json::array();
        for (const auto& p : w.pts) pts.push_back(pt(p));
        wires.push_back(std::move(pts));
    }
    j["wires"] = std::move(wires);
    json labels = json::array();
    for (const auto& l : c.labels) labels.push_back({{"name", l.name}, {"at", pt(l.pos)}});
    j["labels"] = std::move(labels);
    json nc = json::array();
    for (const auto& p : c.noConnects) nc.push_back(pt(p));
    j["no_connects"] = std::move(nc);
    json b;
    json outline = json::array();
    for (const auto& p : c.board.outline) outline.push_back(pt(p));
    b["layers"] = c.board.layers;
    b["outline"] = std::move(outline);
    json traces = json::array();
    for (const auto& t : c.board.traces) {
        traces.push_back({{"a", pt(t.a)}, {"b", pt(t.b)}, {"width", t.width}, {"layer", layerShort(t.layer)}, {"net", t.net}});
    }
    b["traces"] = std::move(traces);
    json vias = json::array();
    for (const auto& v : c.board.vias) vias.push_back({{"at", pt(v.pos)}, {"drill", v.drill}, {"diameter", v.diameter}, {"net", v.net}});
    b["vias"] = std::move(vias);
    json zones = json::array();
    for (const auto& z : c.board.zones) {
        zones.push_back({{"net", z.net}, {"layer", layerShort(z.layer)}, {"clearance", z.clearance}, {"min_width", z.minWidth}, {"thermal", z.thermal}, {"spoke", z.spoke}});
    }
    b["zones"] = std::move(zones);
    const Rules& r = c.board.rules;
    b["rules"] = {{"trace", r.trace},         {"power_trace", r.powerTrace}, {"clearance", r.clearance}, {"min_trace", r.minTrace},
                  {"via_drill", r.viaDrill},  {"via_diameter", r.viaDiameter}, {"min_drill", r.minDrill}, {"min_annular", r.minAnnular},
                  {"edge_clearance", r.edgeClearance}, {"grid", r.grid}};
    j["board"] = std::move(b);
    return j.dump(1) + "\n";
}

Circuit fromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    if (!j.is_object() || !j.contains("parts")) throw std::runtime_error("not a circuit file (no \"parts\")");
    Circuit c;
    try {
        c.title = j.value("title", "");
        for (const auto& q : j["parts"]) {
            Part p;
            p.ref = q.value("ref", "");
            p.symbol = q.value("symbol", "");
            p.value = q.value("value", "");
            p.footprint = q.value("footprint", "");
            p.pins = q.value("pins", 0);
            if (q.contains("pin_names")) p.pinNames = q["pin_names"].get<std::vector<std::string>>();
            if (q.contains("at")) p.pos = readPt(q["at"]);
            p.rot = q.value("rot", 0);
            p.mirror = q.value("mirror", false);
            if (q.contains("fields")) p.fields = q["fields"].get<std::map<std::string, std::string>>();
            if (q.contains("pcb")) {
                const auto& b = q["pcb"];
                p.pcb.placed = true;
                p.pcb.pos = readPt(b["at"]);
                p.pcb.rot = b.value("rot", 0);
                p.pcb.bottom = b.value("side", "top") == "bottom";
            }
            c.parts.push_back(std::move(p));
        }
        if (j.contains("wires")) {
            for (const auto& w : j["wires"]) {
                Wire wire;
                for (const auto& p : w) wire.pts.push_back(readPt(p));
                if (wire.pts.size() >= 2) c.wires.push_back(std::move(wire));
            }
        }
        if (j.contains("labels")) {
            for (const auto& l : j["labels"]) c.labels.push_back({l.value("name", ""), readPt(l["at"])});
        }
        if (j.contains("no_connects")) {
            for (const auto& p : j["no_connects"]) c.noConnects.push_back(readPt(p));
        }
        if (j.contains("board")) {
            const auto& b = j["board"];
            c.board.layers = b.value("layers", 2) == 4 ? 4 : 2;
            if (b.contains("outline")) {
                for (const auto& p : b["outline"]) c.board.outline.push_back(readPt(p));
            }
            if (b.contains("traces")) {
                for (const auto& t : b["traces"]) {
                    c.board.traces.push_back({readPt(t["a"]), readPt(t["b"]), t.value("width", 0.25), std::max(0, parseLayer(t.value("layer", "F"))), t.value("net", "")});
                }
            }
            if (b.contains("vias")) {
                for (const auto& v : b["vias"]) c.board.vias.push_back({readPt(v["at"]), v.value("drill", 0.4), v.value("diameter", 0.8), v.value("net", "")});
            }
            if (b.contains("zones")) {
                for (const auto& z : b["zones"]) {
                    Zone zone{z.value("net", ""), std::max(0, parseLayer(z.value("layer", "B"))), z.value("clearance", 0.3), z.value("min_width", 0.25)};
                    zone.thermal = z.value("thermal", true);
                    zone.spoke = z.value("spoke", 0.5);
                    c.board.zones.push_back(zone);
                }
            }
            if (b.contains("rules")) {
                const auto& r = b["rules"];
                Rules& o = c.board.rules;
                o.trace = r.value("trace", o.trace);
                o.powerTrace = r.value("power_trace", o.powerTrace);
                o.clearance = r.value("clearance", o.clearance);
                o.minTrace = r.value("min_trace", o.minTrace);
                o.viaDrill = r.value("via_drill", o.viaDrill);
                o.viaDiameter = r.value("via_diameter", o.viaDiameter);
                o.minDrill = r.value("min_drill", o.minDrill);
                o.minAnnular = r.value("min_annular", o.minAnnular);
                o.edgeClearance = r.value("edge_clearance", o.edgeClearance);
                o.grid = r.value("grid", o.grid);
            }
        }
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("bad circuit file: ") + e.what());
    }
    return c;
}

Circuit fromKicadNetlist(const std::string& text, std::vector<std::string>* warnings) {
    std::size_t i = 0;
    Sx root = parseSx(text, i);
    if (!root.isList || root.list.empty() || root.list[0].atom != "export") throw std::runtime_error("not a KiCad netlist (expected (export ...))");
    Circuit c;
    if (const Sx* design = root.child("design")) c.title = design->value("source");
    auto warn = [&](const std::string& w) {
        if (warnings) warnings->push_back(w);
    };
    struct Node {
        std::string ref, pin, function;
    };
    struct NetIn {
        std::string name;
        std::vector<Node> nodes;
    };
    std::vector<NetIn> nets;
    if (const Sx* ns = root.child("nets")) {
        for (const auto& n : ns->list) {
            if (!n.isList || n.list.empty() || n.list[0].atom != "net") continue;
            NetIn net;
            net.name = n.value("name");
            while (!net.name.empty() && net.name[0] == '/') net.name.erase(0, 1);
            for (const auto& node : n.list) {
                if (!node.isList || node.list.empty() || node.list[0].atom != "node") continue;
                net.nodes.push_back({node.value("ref"), node.value("pin"), node.value("pinfunction")});
            }
            nets.push_back(std::move(net));
        }
    }
    // pins each part uses, for checking the mapping and sizing generic ICs
    std::map<std::string, std::map<std::string, std::string>> usedPins;  // ref -> pin -> function
    for (const auto& n : nets) {
        for (const auto& node : n.nodes) usedPins[node.ref][node.pin] = node.function;
    }
    double x = 0, y = 0, rowH = 0;
    const double rowWidth = 70;
    if (const Sx* comps = root.child("components")) {
        for (const auto& comp : comps->list) {
            if (!comp.isList || comp.list.empty() || comp.list[0].atom != "comp") continue;
            Part p;
            p.ref = comp.value("ref");
            p.value = comp.value("value");
            std::string libPart;
            if (const Sx* ls = comp.child("libsource")) libPart = ls->value("part");
            int pins = 0;
            std::string sym = mapKicadPart(libPart, pins);
            if (sym.empty() || sym == "power" || sym == "gnd") sym = "";
            const auto& used = usedPins[p.ref];
            std::shared_ptr<const Symbol> s;
            if (!sym.empty()) {
                s = findSymbol(sym, pins);
                if (s) {
                    for (const auto& [pin, fn] : used) {
                        bool found = false;
                        for (const auto& pd : s->pins) found = found || pd.number == pin;
                        if (!found) {
                            s = nullptr;
                            break;
                        }
                    }
                }
            }
            if (!s) {
                int maxPin = 0;
                for (const auto& [pin, fn] : used) {
                    if (!pin.empty() && std::all_of(pin.begin(), pin.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
                        maxPin = std::max(maxPin, std::stoi(pin));
                    else warn(p.ref + ": pin '" + pin + "' isn't a number; it was skipped");
                }
                maxPin = std::max(maxPin, 2);
                std::vector<std::string> names(static_cast<std::size_t>(maxPin));
                for (const auto& [pin, fn] : used) {
                    if (!pin.empty() && std::all_of(pin.begin(), pin.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
                        const int k = std::stoi(pin);
                        if (k >= 1 && k <= maxPin && !fn.empty() && fn != "~") names[static_cast<std::size_t>(k - 1)] = fn;
                    }
                }
                sym = "ic";
                pins = maxPin;
                p.pinNames = names;
                if (!libPart.empty()) warn(p.ref + ": '" + libPart + "' has no library symbol; using a generic " + std::to_string(maxPin + maxPin % 2) + "-pin IC");
                s = findSymbol(sym, pins, p.pinNames);
            }
            p.symbol = sym;
            p.pins = pins;
            const std::string fp = comp.value("footprint");
            if (!fp.empty() && findFootprint(fp)) {
                p.footprint = fp;
                if (auto colon = fp.find(':'); colon != std::string::npos) p.footprint = fp.substr(colon + 1);
                // keep the short library name when we know it
                p.footprint = findFootprint(fp)->name;
            } else {
                p.footprint = s->footprint;
                if (!fp.empty()) warn(p.ref + ": footprint '" + fp + "' isn't built in; using " + (s->footprint.empty() ? "none" : s->footprint));
            }
            if (p.value.empty()) p.value = s->value;
            // grid layout
            const double w = std::ceil(s->maxX - s->minX) + 6, h = std::ceil(s->maxY - s->minY) + 4;
            if (x > 0 && x + w > rowWidth) {
                x = 0;
                y += rowH;
                rowH = 0;
            }
            p.pos = {std::round(x - s->minX + 3), std::round(y - s->minY + 2)};
            x += w;
            rowH = std::max(rowH, h);
            c.parts.push_back(std::move(p));
        }
    }
    std::set<std::string> refs;
    for (const auto& p : c.parts) refs.insert(p.ref);
    for (const auto& n : nets) {
        // single-pin nets only matter when they carry a real name (a power rail, a test point)
        if (n.nodes.empty() || (n.nodes.size() < 2 && (n.name.empty() || n.name.rfind("Net-(", 0) == 0 || n.name.rfind("unconnected", 0) == 0))) continue;
        for (const auto& node : n.nodes) {
            const Part* part = c.part(node.ref);
            if (!part) {
                warn("net " + n.name + " names an unknown part " + node.ref);
                continue;
            }
            auto s = symbolOf(*part);
            const PinDef* pd = nullptr;
            for (const auto& q : s->pins) {
                if (q.number == node.pin) pd = &q;
            }
            if (pd) labelPin(c, *part, *pd, n.name);
        }
    }
    return c;
}

// ---- document -------------------------------------------------------------------------

std::string CircuitDoc::summary() const {
    int parts = 0;
    for (const auto& p : circuit.parts) {
        auto sym = symbolOf(p);
        if (sym && !sym->power) ++parts;
    }
    const Netlist nl = buildNetlist(circuit);
    std::string s = std::to_string(parts) + " parts, " + std::to_string(nl.nets.size()) + " nets, " + std::to_string(circuit.wires.size()) + " wires";
    if (circuit.board.outline.size() >= 3) {
        char b[96];
        std::snprintf(b, sizeof b, "; board %.1f x %.1f mm, %zu traces, %zu vias", boardWidth(circuit.board), boardHeight(circuit.board),
                      circuit.board.traces.size(), circuit.board.vias.size());
        s += b;
    }
    if (!path.empty()) s += " (" + path + ")";
    return s;
}

std::unique_ptr<Attachment> CircuitDoc::preview() const {
    const pdf::Page page = schematicPage(circuit);
    const double scale = std::clamp(1400.0 / std::max(page.width, page.height), 0.5, 2.5);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(pdf::renderPage(page, scale)), "schematic.png"});
}

Result CircuitEdit::apply() {
    before_ = doc_->circuit;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void CircuitEdit::revert() { doc_->circuit = before_; }

}  // namespace wetwaresuite::circuit
