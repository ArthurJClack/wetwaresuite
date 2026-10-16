// Text to diagram: Mermaid flowcharts (graph/flowchart), sequence diagrams
// and mind maps, plus plain indented outlines, turned into board elements
// and laid out.
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "internal.hpp"

namespace achclwss::board {

using namespace detail;

namespace {

bool isIdChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

std::string stripComments(const std::string& line) {
    const auto p = line.find("%%");
    return p == std::string::npos ? line : line.substr(0, p);
}

// Mermaid labels: quotes, markdown backticks, <br> line breaks, &quot; style entities, #35; codes.
std::string cleanLabel(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    if (s.size() >= 2 && s.front() == '`' && s.back() == '`') s = s.substr(1, s.size() - 2);
    for (const char* br : {"<br/>", "<br />", "<br>", "<BR>", "\\n"}) {
        std::size_t p;
        while ((p = s.find(br)) != std::string::npos) s.replace(p, std::string(br).size(), "\n");
    }
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '<') {  // drop other tags
            const auto close = s.find('>', i);
            if (close != std::string::npos) {
                i = close;
                continue;
            }
        }
        if (s[i] == '&') {
            const auto semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 6) {
                const std::string ent = s.substr(i, semi - i + 1);
                const char* rep = ent == "&quot;" ? "\"" : ent == "&amp;" ? "&" : ent == "&lt;" ? "<" : ent == "&gt;" ? ">" : ent == "&nbsp;" ? " " : nullptr;
                if (rep) {
                    out += rep;
                    i = semi;
                    continue;
                }
            }
        }
        if (s[i] == '#' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            const auto semi = s.find(';', i);
            if (semi != std::string::npos) {
                try {
                    out += pdf::utf8Encode(static_cast<unsigned>(std::stoul(s.substr(i + 1, semi - i - 1))));
                    i = semi;
                    continue;
                } catch (const std::exception&) {
                }
            }
        }
        out += s[i];
    }
    return out;
}

// Splits on ';' outside quotes and brackets.
std::vector<std::string> statements(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool quote = false;
    for (char c : line) {
        if (c == '"') quote = !quote;
        if (!quote) {
            if (c == '[' || c == '(' || c == '{') ++depth;
            if ((c == ']' || c == ')' || c == '}') && depth > 0) --depth;
            if (c == ';' && depth == 0) {
                out.push_back(cur);
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

std::string lightOf(const std::string& color, double amount) {
    auto c = parseColor(color);
    if (!c) return "#ffffff";
    auto mix = [&](std::uint8_t v) { return static_cast<int>(std::lround(v + (255 - v) * amount)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", mix(c->r), mix(c->g), mix(c->b));
    return buf;
}

// CSS-ish "fill:#f9f,stroke:#333,stroke-width:4px,color:#fff" onto a style.
void applyCss(Style& s, const std::string& css) {
    std::string item;
    auto flush = [&] {
        const auto colon = item.find(':');
        if (colon != std::string::npos) {
            const std::string k = lower(trim(item.substr(0, colon)));
            std::string v = trim(item.substr(colon + 1));
            if (!v.empty() && v.back() == ';') v.pop_back();
            auto num = [&] {
                try {
                    return std::stod(v);
                } catch (const std::exception&) {
                    return 0.0;
                }
            };
            if (k == "fill" && validColor(v)) s.fill = v;
            else if (k == "stroke" && validColor(v)) s.stroke = v;
            else if (k == "color" && validColor(v)) s.textColor = v;
            else if (k == "stroke-width" && num() > 0) s.strokeWidth = num();
            else if (k == "stroke-dasharray") s.dash = "dashed";
            else if (k == "font-weight") s.bold = v == "bold" || num() >= 600;
            else if (k == "font-style") s.italic = v == "italic";
            else if (k == "font-size" && num() > 0) s.fontSize = num();
            else if (k == "opacity" && num() > 0) s.opacity = num();
        }
        item.clear();
    };
    for (char c : css) {
        if (c == ',') flush();
        else item += c;
    }
    flush();
}

void merge(Style& into, const Style& from) {
    if (from.fill) into.fill = from.fill;
    if (from.stroke) into.stroke = from.stroke;
    if (from.textColor) into.textColor = from.textColor;
    if (from.strokeWidth) into.strokeWidth = from.strokeWidth;
    if (from.dash) into.dash = from.dash;
    if (from.bold) into.bold = from.bold;
    if (from.italic) into.italic = from.italic;
    if (from.fontSize) into.fontSize = from.fontSize;
    if (from.opacity) into.opacity = from.opacity;
}

void sizeNode(const Board& board, Element& e, double minW, double minH) {
    const Resolved s = resolve(board, e);
    Pt sz = fitText(e.kind, e.text, s, minW, minH);
    if (e.kind == Kind::Ellipse && e.style.radius.value_or(0) < 0) sz.x = sz.y = std::max(sz.x, sz.y);
    e.w = std::round(sz.x);
    e.h = std::round(sz.y);
}

// ---- flowcharts -------------------------------------------------------------------------------

struct Shape {
    const char* open;
    const char* close;
    Kind kind;
    bool circle;
};
constexpr Shape kShapes[] = {
    {"(((", ")))", Kind::Ellipse, true}, {"((", "))", Kind::Ellipse, true}, {"([", "])", Kind::Stadium, false},
    {"[[", "]]", Kind::Subroutine, false}, {"[(", ")]", Kind::Cylinder, false}, {"[/", "/]", Kind::Parallelogram, false},
    {"[\\", "\\]", Kind::Parallelogram, false}, {"[/", "\\]", Kind::Trapezoid, false}, {"[\\", "/]", Kind::Trapezoid, false},
    {"{{", "}}", Kind::Hexagon, false}, {"(", ")", Kind::Rounded, false}, {"[", "]", Kind::Rect, false},
    {"{", "}", Kind::Diamond, false}, {">", "]", Kind::Parallelogram, false},
};

struct NodeRef {
    std::string id;
    bool hasShape = false;
    Kind kind = Kind::Rect;
    bool circle = false;
    std::string text;
    std::string cls;
};

struct Link {
    std::string startHead, endHead;  // "", "<"/">" arrow, "o", "x"
    std::string line = "solid";      // solid, thick, dotted
    std::string text;
    int length = 1;
};

struct Cursor {
    const std::string& s;
    std::size_t i = 0;
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
    bool done() {
        ws();
        return i >= s.size();
    }
    bool starts(const char* t) const { return s.compare(i, std::char_traits<char>::length(t), t) == 0; }
};

bool parseNode(Cursor& c, NodeRef& n) {
    c.ws();
    const std::size_t start = c.i;
    while (c.i < c.s.size()) {
        const unsigned char ch = static_cast<unsigned char>(c.s[c.i]);
        if (isIdChar(ch)) {
            ++c.i;
            continue;
        }
        // a '-' inside an id ("node-1") but not the start of a link
        if (ch == '-' && c.i > start && c.i + 1 < c.s.size() && isIdChar(static_cast<unsigned char>(c.s[c.i + 1])) &&
            c.s[c.i + 1] != 'x' && c.s[c.i + 1] != 'o') {
            ++c.i;
            continue;
        }
        break;
    }
    if (c.i == start) return false;
    n.id = c.s.substr(start, c.i - start);
    // a trailing 'o'/'x' glued to a link ("A--oB") isn't possible here since ids stop before '-'
    for (const auto& sh : kShapes) {
        if (!c.starts(sh.open)) continue;
        const std::size_t textStart = c.i + std::char_traits<char>::length(sh.open);
        std::size_t p = textStart;
        // quoted text may hold closing characters
        while (p < c.s.size() && c.s[p] == ' ') ++p;
        std::size_t searchFrom = textStart;
        if (p < c.s.size() && c.s[p] == '"') {
            const auto q = c.s.find('"', p + 1);
            if (q != std::string::npos) searchFrom = q + 1;
        }
        const auto close = c.s.find(sh.close, searchFrom);
        if (close == std::string::npos) continue;
        // the inner text must not itself start like a longer opener's tail ("((" vs "(")
        n.hasShape = true;
        n.kind = sh.kind;
        n.circle = sh.circle;
        n.text = cleanLabel(c.s.substr(textStart, close - textStart));
        c.i = close + std::char_traits<char>::length(sh.close);
        break;
    }
    if (c.starts(":::")) {
        c.i += 3;
        const std::size_t cs = c.i;
        while (c.i < c.s.size() && (isIdChar(static_cast<unsigned char>(c.s[c.i])) || c.s[c.i] == '-')) ++c.i;
        n.cls = c.s.substr(cs, c.i - cs);
    }
    return true;
}

bool lineChar(char ch) { return ch == '-' || ch == '=' || ch == '.'; }

// Reads one link body like "-->", "-.->", "==>", "---", "--o", "<-->" (no text).
bool parseLinkBody(Cursor& c, Link& l, bool allowOpen, bool& open) {
    const std::size_t save = c.i;
    open = false;
    if (c.i < c.s.size() && (c.s[c.i] == '<' || ((c.s[c.i] == 'o' || c.s[c.i] == 'x') && c.i + 1 < c.s.size() && lineChar(c.s[c.i + 1])))) {
        l.startHead = c.s[c.i] == '<' ? ">" : std::string(1, c.s[c.i]);
        ++c.i;
    }
    const std::size_t bs = c.i;
    while (c.i < c.s.size() && lineChar(c.s[c.i])) ++c.i;
    const std::string body = c.s.substr(bs, c.i - bs);
    if (body.size() < 2) {
        c.i = save;
        return false;
    }
    if (body.find('=') != std::string::npos) l.line = "thick";
    else if (body.find('.') != std::string::npos) l.line = "dotted";
    else l.line = "solid";
    if (c.i < c.s.size() && c.s[c.i] == '>') {
        l.endHead = ">";
        ++c.i;
    } else if (c.i < c.s.size() && (c.s[c.i] == 'o' || c.s[c.i] == 'x') &&
               (c.i + 1 >= c.s.size() || c.s[c.i + 1] == ' ' || c.s[c.i + 1] == '|' || c.s[c.i + 1] == '\t')) {
        l.endHead = std::string(1, c.s[c.i]);
        ++c.i;
    } else if (allowOpen && (body == "--" || body == "==" || body == "-.") && l.startHead.empty()) {
        open = true;  // "-- text -->"
    } else if (body.size() < 3 && body != "-.") {
        c.i = save;
        return false;
    }
    int dashes = 0;
    for (char ch : body) dashes += ch == '-' || ch == '=' ? 1 : 0;
    l.length = std::max(1, dashes - (l.endHead.empty() ? 2 : 1));
    return true;
}

bool parseLink(Cursor& c, Link& l) {
    c.ws();
    bool open = false;
    if (!parseLinkBody(c, l, true, open)) return false;
    if (open) {
        // text up to the closing body: the first "--", "==" or ".-" after a space
        const std::size_t ts = c.i;
        std::size_t p = ts;
        for (; p + 1 < c.s.size(); ++p) {
            const bool closer = (c.s[p] == '-' && c.s[p + 1] == '-') || (c.s[p] == '=' && c.s[p + 1] == '=') || (c.s[p] == '.' && c.s[p + 1] == '-');
            if (closer && p > ts && (c.s[p - 1] == ' ' || c.s[p - 1] == '\t')) break;
        }
        if (p + 1 >= c.s.size()) return false;
        l.text = cleanLabel(c.s.substr(ts, p - ts));
        c.i = p;
        Link close;
        bool again = false;
        if (!parseLinkBody(c, close, false, again)) return false;
        l.endHead = close.endHead;
        if (close.line != "solid") l.line = close.line;
    }
    c.ws();
    if (c.i < c.s.size() && c.s[c.i] == '|') {
        const auto end = c.s.find('|', c.i + 1);
        if (end != std::string::npos) {
            l.text = cleanLabel(c.s.substr(c.i + 1, end - c.i - 1));
            c.i = end + 1;
        }
    }
    return true;
}

std::string headName(const std::string& h) {
    if (h == ">") return "triangle";
    if (h == "o") return "circle";
    if (h == "x") return "cross";
    return "none";
}

struct Flow {
    Board& board;
    const ImportOptions& opt;
    ImportStats& stats;
    std::map<std::string, std::string> nodes;  // mermaid id -> element id
    std::map<std::string, Style> classes;
    std::map<std::string, std::vector<std::string>> classOf;  // mermaid id -> classes
    std::vector<std::string> frameStack;
    std::vector<std::string> edgeIds;
    std::vector<std::string> nodeOrder;

    std::string layerName() const { return opt.layer.empty() ? board.layers.front().name : opt.layer; }

    std::string node(const NodeRef& r) {
        auto it = nodes.find(r.id);
        if (it == nodes.end()) {
            // a subgraph id can be linked to like a node
            Element e;
            e.kind = r.hasShape ? r.kind : Kind::Rect;
            e.name = r.id;
            e.text = r.hasShape ? r.text : r.id;
            e.layer = layerName();
            if (!frameStack.empty()) e.parent = frameStack.back();
            if (r.circle) e.style.radius = -1;  // marker for sizeNode: keep it round
            const std::string id = board.add(e);
            nodes[r.id] = id;
            nodeOrder.push_back(r.id);
            stats.ids.push_back(id);
            ++stats.nodes;
            it = nodes.find(r.id);
        } else if (r.hasShape) {
            Element* e = board.get(it->second);
            if (e && e->kind != Kind::Frame) {
                e->kind = r.kind;
                e->text = r.text;
                if (r.circle) e->style.radius = -1;
            }
        }
        // a node mentioned inside a subgraph belongs to it (the innermost one wins)
        if (!frameStack.empty() && it->second != frameStack.back()) {
            Element* e = board.get(it->second);
            if (e && e->kind != Kind::Frame && (e->parent.empty() || std::find(frameStack.begin(), frameStack.end(), e->parent) != frameStack.end()))
                e->parent = frameStack.back();
        }
        if (!r.cls.empty()) classOf[r.id].push_back(r.cls);
        return it->second;
    }

    void edge(const std::string& a, const std::string& b, const Link& l) {
        Element c;
        c.kind = Kind::Connector;
        c.from = a;
        c.to = b;
        c.text = l.text;
        c.layer = layerName();
        c.route = opt.route.empty() ? "elbow" : opt.route;
        c.endArrow = headName(l.endHead);
        c.startArrow = headName(l.startHead);
        if (l.line == "thick") c.style.strokeWidth = 3.5;
        if (l.line == "dotted") c.style.dash = "dashed";
        const std::string id = board.add(c);
        edgeIds.push_back(id);
        stats.ids.push_back(id);
        ++stats.edges;
    }

    void chain(const std::string& stmt) {
        Cursor c{stmt};
        std::vector<std::string> left;
        auto group = [&](std::vector<std::string>& out) {
            out.clear();
            do {
                NodeRef r;
                if (!parseNode(c, r)) return false;
                out.push_back(node(r));
                c.ws();
                if (c.i < c.s.size() && c.s[c.i] == '&') {
                    ++c.i;
                    continue;
                }
                break;
            } while (true);
            return true;
        };
        if (!group(left)) {
            stats.warnings.push_back("skipped: " + trim(stmt));
            return;
        }
        while (!c.done()) {
            Link l;
            if (!parseLink(c, l)) {
                stats.warnings.push_back("couldn't read the link in: " + trim(stmt));
                return;
            }
            std::vector<std::string> right;
            if (!group(right)) {
                stats.warnings.push_back("missing node after a link in: " + trim(stmt));
                return;
            }
            for (const auto& a : left) {
                for (const auto& b : right) edge(a, b, l);
            }
            left = right;
        }
    }

    void subgraph(const std::string& rest) {
        std::string id = trim(rest), title;
        const auto br = id.find('[');
        if (br != std::string::npos && id.back() == ']') {
            title = cleanLabel(id.substr(br + 1, id.size() - br - 2));
            id = trim(id.substr(0, br));
        } else if (!id.empty() && id.front() == '"') {
            title = cleanLabel(id);
            id = title;
        } else {
            title = id;
        }
        Element f;
        f.kind = Kind::Frame;
        f.name = id;
        f.text = title;
        f.layer = layerName();
        if (!frameStack.empty()) f.parent = frameStack.back();
        f.w = 200;
        f.h = 120;
        const std::string fid = board.add(f);
        nodes[id] = fid;
        frameStack.push_back(fid);
        stats.ids.push_back(fid);
        ++stats.frames;
    }
};

ImportStats flowchart(Board& board, const std::vector<std::string>& lines, std::size_t first, const std::string& header,
                      const ImportOptions& opt) {
    ImportStats stats;
    stats.kind = "flowchart";
    std::string direction = "TB";
    {
        std::string h = trim(header);
        const auto sp = h.find_first_of(" \t");
        if (sp != std::string::npos) {
            std::string d = trim(h.substr(sp));
            for (auto& ch : d) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (d == "TD" || d == "TB" || d == "BT" || d == "LR" || d == "RL") direction = d == "TD" ? "TB" : d;
        }
    }
    if (!opt.direction.empty()) direction = opt.direction;
    Flow flow{board, opt, stats, {}, {}, {}, {}, {}, {}};
    std::vector<std::pair<std::string, Style>> styles;
    for (std::size_t li = first; li < lines.size(); ++li) {
        for (const auto& raw : statements(stripComments(lines[li]))) {
            const std::string st = trim(raw);
            if (st.empty()) continue;
            const std::string low = lower(st);
            auto word = [&](const char* w) {
                const std::size_t n = std::char_traits<char>::length(w);
                return low.compare(0, n, w) == 0 && (low.size() == n || low[n] == ' ' || low[n] == '\t');
            };
            if (word("subgraph")) {
                flow.subgraph(st.substr(8));
            } else if (low == "end") {
                if (!flow.frameStack.empty()) flow.frameStack.pop_back();
            } else if (word("direction") || word("linkstyle") || word("click") || word("accTitle") || word("acctitle") || word("accdescr")) {
                continue;
            } else if (word("classdef")) {
                std::string rest = trim(st.substr(8));
                const auto sp = rest.find_first_of(" \t");
                if (sp == std::string::npos) continue;
                for (const auto& name : [&] {
                         std::vector<std::string> v;
                         std::string cur;
                         for (char ch : rest.substr(0, sp) + ",") {
                             if (ch == ',') {
                                 if (!cur.empty()) v.push_back(cur);
                                 cur.clear();
                             } else {
                                 cur += ch;
                             }
                         }
                         return v;
                     }()) {
                    applyCss(flow.classes[name], trim(rest.substr(sp)));
                }
            } else if (word("class")) {
                std::string rest = trim(st.substr(5));
                const auto sp = rest.find_last_of(" \t");
                if (sp == std::string::npos) continue;
                const std::string cls = trim(rest.substr(sp));
                std::string cur;
                for (char ch : rest.substr(0, sp) + ",") {
                    if (ch == ',') {
                        if (!trim(cur).empty()) flow.classOf[trim(cur)].push_back(cls);
                        cur.clear();
                    } else {
                        cur += ch;
                    }
                }
            } else if (word("style")) {
                std::string rest = trim(st.substr(5));
                const auto sp = rest.find_first_of(" \t");
                if (sp == std::string::npos) continue;
                Style s;
                applyCss(s, trim(rest.substr(sp)));
                styles.push_back({trim(rest.substr(0, sp)), s});
            } else {
                flow.chain(st);
            }
        }
    }
    // styles: classDef default, then classes, then style lines
    for (const auto& id : flow.nodeOrder) {
        Element* e = board.get(flow.nodes[id]);
        if (!e) continue;
        auto def = flow.classes.find("default");
        if (def != flow.classes.end() && !flow.classOf.count(id)) merge(e->style, def->second);
        for (const auto& cls : flow.classOf[id]) {
            auto it = flow.classes.find(cls);
            if (it != flow.classes.end()) merge(e->style, it->second);
        }
    }
    for (const auto& [id, s] : styles) {
        auto it = flow.nodes.find(id);
        if (it == flow.nodes.end()) continue;
        if (Element* e = board.get(it->second)) merge(e->style, s);
    }
    // sizes
    for (const auto& id : flow.nodeOrder) {
        Element* e = board.get(flow.nodes[id]);
        if (!e || e->kind == Kind::Frame) continue;
        const bool round = e->style.radius.value_or(0) < 0;
        const double minW = e->kind == Kind::Diamond ? 130 : 120, minH = e->kind == Kind::Diamond ? 80 : 50;
        sizeNode(board, *e, minW, minH);
        if (round) {
            e->w = e->h = std::max(e->w, e->h);
            e->style.radius.reset();
        }
    }
    if (stats.nodes == 0 && stats.frames == 0) throw std::runtime_error("no nodes found in the flowchart");
    // layout
    const std::string alg = opt.layout.empty() ? "layered" : opt.layout;
    std::vector<std::string> layoutIds;
    for (const auto& id : flow.nodeOrder) {
        const Element* e = board.get(flow.nodes[id]);
        if (e && e->kind != Kind::Frame) layoutIds.push_back(e->id);
    }
    if (alg != "none" && !layoutIds.empty()) {
        LayoutOptions lo;
        lo.algorithm = alg;
        lo.direction = direction;
        lo.ids = layoutIds;
        lo.keepOrigin = false;
        lo.originX = opt.x;
        lo.originY = opt.y + (stats.frames ? 40 : 0);
        lo.gapX = direction == "LR" || direction == "RL" ? 40 : 50;
        lo.gapY = direction == "LR" || direction == "RL" ? 90 : 70;
        std::string err;
        layout(board, lo, err);
        if (!err.empty()) stats.warnings.push_back(err);
        // nodes outside a subgraph that the fitted frame now covers move out of its way (with their rank-mates beyond them)
        const bool vertical = direction == "TB" || direction == "BT";
        for (int pass = 0; pass < 4 && stats.frames; ++pass) {
            bool moved = false;
            for (const auto& fe : board.elements) {
                if (fe.kind != Kind::Frame || std::find(stats.ids.begin(), stats.ids.end(), fe.id) == stats.ids.end()) continue;
                const auto inside = board.descendants(fe.id);
                const Box fb{fe.x, fe.y, fe.x + fe.w, fe.y + fe.h, false};
                for (const auto& nid : layoutIds) {
                    if (std::find(inside.begin(), inside.end(), nid) != inside.end()) continue;
                    Element* n = board.get(nid);
                    if (!n || !n->parent.empty()) continue;
                    const Box nb = elementBox(board, *n);
                    if (!nb.intersects(fb.grown(12))) continue;
                    const bool after = vertical ? nb.cx() >= fb.cx() : nb.cy() >= fb.cy();
                    const double d = vertical ? (after ? fb.x1 + 24 - nb.x0 : fb.x0 - 24 - nb.x1) : (after ? fb.y1 + 24 - nb.y0 : fb.y0 - 24 - nb.y1);
                    for (const auto& oid : layoutIds) {
                        Element* o = board.get(oid);
                        if (!o || std::find(inside.begin(), inside.end(), oid) != inside.end()) continue;
                        const Box ob = elementBox(board, *o);
                        const bool band = vertical ? (ob.y1 > nb.y0 && ob.y0 < nb.y1) : (ob.x1 > nb.x0 && ob.x0 < nb.x1);
                        const bool beyond = vertical ? (after ? ob.cx() >= nb.cx() - 1 : ob.cx() <= nb.cx() + 1) : (after ? ob.cy() >= nb.cy() - 1 : ob.cy() <= nb.cy() + 1);
                        if (band && beyond) moveBy(board, *o, vertical ? d : 0, vertical ? 0 : d);
                    }
                    moved = true;
                    break;
                }
            }
            refreshGroups(board, true);
            if (!moved) break;
        }
        // forward edges leave and enter along the flow; the rest pick their sides
        for (const auto& cid : flow.edgeIds) {
            Element* c = board.get(cid);
            const Element* a = c ? board.get(c->from) : nullptr;
            const Element* b = c ? board.get(c->to) : nullptr;
            if (!a || !b || a == b || c->route == "straight") continue;
            const Box ba = elementBox(board, *a), bb = elementBox(board, *b);
            std::string from, to;
            if (direction == "TB" && bb.y0 >= ba.y1 + 10) from = "bottom", to = "top";
            else if (direction == "BT" && bb.y1 <= ba.y0 - 10) from = "top", to = "bottom";
            else if (direction == "LR" && bb.x0 >= ba.x1 + 10) from = "right", to = "left";
            else if (direction == "RL" && bb.x1 <= ba.x0 - 10) from = "left", to = "right";
            else if ((direction == "TB" || direction == "BT") && std::abs(bb.cy() - ba.cy()) > 10) {
                // back edges go round the side so they don't run over the forward ones
                const bool right = ba.cx() >= bb.cx() - 1;
                from = to = right ? "right" : "left";
            } else if ((direction == "LR" || direction == "RL") && std::abs(bb.cx() - ba.cx()) > 10) {
                const bool below = ba.cy() >= bb.cy() - 1;
                from = to = below ? "bottom" : "top";
            }
            if (!from.empty()) {
                c->fromAnchor = from;
                c->toAnchor = to;
            }
        }
    } else {
        refreshGroups(board, true);
    }
    // frames go under their nodes
    std::stable_partition(board.elements.begin(), board.elements.end(), [](const Element& e) { return e.kind == Kind::Frame; });
    return stats;
}


// ---- sequence diagrams ------------------------------------------------------------------------

struct Participant {
    std::string id, label;
    bool actor = false;
    double cx = 0, w = 0, h = 0;
};

struct SeqItem {
    enum Type { Message, Note, BlockStart, BlockElse, BlockEnd } type = Message;
    std::string from, to, text, arrow, kindWord, notePos;
    std::vector<std::string> over;
};

ImportStats sequence(Board& board, const std::vector<std::string>& lines, std::size_t first, const ImportOptions& opt) {
    ImportStats stats;
    stats.kind = "sequence";
    std::vector<Participant> parts;
    auto partIndex = [&](const std::string& id) -> int {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].id == id) return static_cast<int>(i);
        }
        parts.push_back({id, id, false, 0, 0, 0});
        return static_cast<int>(parts.size()) - 1;
    };
    std::vector<SeqItem> items;
    bool autonumber = false;
    static const char* kArrows[] = {"<<-->>", "<<->>", "-->>", "->>", "-->", "->", "--x", "-x", "--)", "-)"};
    for (std::size_t li = first; li < lines.size(); ++li) {
        const std::string st = trim(stripComments(lines[li]));
        if (st.empty()) continue;
        const std::string low = lower(st);
        auto word = [&](const char* w) {
            const std::size_t n = std::char_traits<char>::length(w);
            return low.compare(0, n, w) == 0 && (low.size() == n || low[n] == ' ' || low[n] == '\t');
        };
        if (word("participant") || word("actor")) {
            const bool actor = word("actor");
            std::string rest = trim(st.substr(actor ? 5 : 11));
            std::string id = rest, label = rest;
            const auto as = lower(rest).find(" as ");
            if (as != std::string::npos) {
                id = trim(rest.substr(0, as));
                label = cleanLabel(rest.substr(as + 4));
            }
            const int i = partIndex(id);
            parts[static_cast<std::size_t>(i)].label = cleanLabel(label);
            parts[static_cast<std::size_t>(i)].actor = actor;
            continue;
        }
        if (word("autonumber")) {
            autonumber = true;
            continue;
        }
        if (word("title")) {
            board.title = cleanLabel(st.substr(5));
            continue;
        }
        if (word("activate") || word("deactivate") || word("create") || word("destroy") || word("box") || word("link") || word("links")) continue;
        if (word("note")) {
            SeqItem it;
            it.type = SeqItem::Note;
            const auto colon = st.find(':');
            it.text = colon == std::string::npos ? "" : cleanLabel(st.substr(colon + 1));
            const std::string where = trim(st.substr(4, colon == std::string::npos ? std::string::npos : colon - 4));
            const std::string lw = lower(where);
            std::string who;
            if (lw.rfind("left of", 0) == 0) it.notePos = "left", who = where.substr(7);
            else if (lw.rfind("right of", 0) == 0) it.notePos = "right", who = where.substr(8);
            else if (lw.rfind("over", 0) == 0) it.notePos = "over", who = where.substr(4);
            std::string cur;
            for (char ch : who + ",") {
                if (ch == ',') {
                    if (!trim(cur).empty()) {
                        it.over.push_back(trim(cur));
                        partIndex(trim(cur));
                    }
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
            if (it.over.empty()) {
                stats.warnings.push_back("note without a participant: " + st);
                continue;
            }
            items.push_back(it);
            continue;
        }
        if (word("loop") || word("alt") || word("opt") || word("par") || word("critical") || word("break") || word("rect")) {
            SeqItem it;
            it.type = SeqItem::BlockStart;
            const auto sp = st.find_first_of(" \t");
            it.kindWord = lower(sp == std::string::npos ? st : st.substr(0, sp));
            it.text = sp == std::string::npos ? "" : cleanLabel(st.substr(sp));
            items.push_back(it);
            continue;
        }
        if (word("else") || word("and") || word("option")) {
            SeqItem it;
            it.type = SeqItem::BlockElse;
            const auto sp = st.find_first_of(" \t");
            it.kindWord = lower(sp == std::string::npos ? st : st.substr(0, sp));
            it.text = sp == std::string::npos ? "" : cleanLabel(st.substr(sp));
            items.push_back(it);
            continue;
        }
        if (low == "end") {
            SeqItem it;
            it.type = SeqItem::BlockEnd;
            items.push_back(it);
            continue;
        }
        // a message: A->>B: text
        const auto colon = st.find(':');
        const std::string head = colon == std::string::npos ? st : st.substr(0, colon);
        bool found = false;
        for (const char* a : kArrows) {
            const auto p = head.find(a);
            if (p == std::string::npos) continue;
            SeqItem it;
            it.arrow = a;
            it.from = trim(head.substr(0, p));
            std::string to = trim(head.substr(p + std::char_traits<char>::length(a)));
            if (!to.empty() && (to[0] == '+' || to[0] == '-')) to = trim(to.substr(1));
            it.to = to;
            it.text = colon == std::string::npos ? "" : cleanLabel(st.substr(colon + 1));
            if (it.from.empty() || it.to.empty()) break;
            partIndex(it.from);
            partIndex(it.to);
            items.push_back(it);
            found = true;
            break;
        }
        if (!found) stats.warnings.push_back("skipped: " + st);
    }
    if (parts.empty()) throw std::runtime_error("no participants or messages found in the sequence diagram");

    const std::string layer = opt.layer.empty() ? board.layers.front().name : opt.layer;
    const Theme& t = theme(board.theme);
    const double fs = t.fontSize, lh = fs * kLineHeight;
    // participant sizes and columns
    for (auto& p : parts) {
        Element probe;
        probe.kind = Kind::Rect;
        probe.text = p.label;
        Resolved s = resolve(board, probe);
        const Pt sz = fitText(Kind::Rect, p.label, s, 110, 46);
        p.w = std::round(sz.x);
        p.h = std::round(sz.y);
    }
    std::vector<double> needGap(parts.size(), 0);  // space wanted between column i and i+1
    auto idx = [&](const std::string& id) {
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i].id == id) return static_cast<int>(i);
        }
        return 0;
    };
    for (const auto& it : items) {
        if (it.type != SeqItem::Message) continue;
        const int a = idx(it.from), b = idx(it.to);
        double wtext = 0;
        for (const auto& l : splitLines(it.text)) wtext = std::max(wtext, textWidth(l, t.font, false, false, fs * 0.9));
        const int lo = std::min(a, b), hi = std::max(a, b);
        if (hi == lo) {
            if (lo < static_cast<int>(parts.size())) needGap[static_cast<std::size_t>(lo)] = std::max(needGap[static_cast<std::size_t>(lo)], wtext + 70);
        } else {
            const double per = (wtext + 40) / (hi - lo);
            for (int k = lo; k < hi; ++k) needGap[static_cast<std::size_t>(k)] = std::max(needGap[static_cast<std::size_t>(k)], per);
        }
    }
    double x = opt.x + parts.front().w / 2;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) x += std::max({parts[i - 1].w / 2 + parts[i].w / 2 + 40, needGap[i - 1], 160.0});
        parts[i].cx = std::round(x);
    }
    double maxH = 0;
    for (const auto& p : parts) maxH = std::max(maxH, p.h);
    const double top = opt.y;
    std::vector<std::string> topIds;
    for (const auto& p : parts) {
        Element e;
        e.kind = p.actor ? Kind::Stadium : Kind::Rect;
        e.name = p.id;
        e.text = p.label;
        e.layer = layer;
        e.x = p.cx - p.w / 2;
        e.y = top + (maxH - p.h);
        e.w = p.w;
        e.h = p.h;
        const std::string id = board.add(e);
        topIds.push_back(id);
        stats.ids.push_back(id);
        ++stats.nodes;
    }
    double y = top + maxH + 36;
    int number = 0;
    struct Open {
        std::string kind, text;
        double y0;
        std::vector<std::pair<double, std::string>> elses;
        int lo = 1 << 30, hi = -1;
    };
    std::vector<Open> open;
    auto touch = [&](int a, int b) {
        for (auto& o : open) {
            o.lo = std::min({o.lo, a, b});
            o.hi = std::max({o.hi, a, b});
        }
    };
    std::vector<Element> frames;
    for (const auto& it : items) {
        if (it.type == SeqItem::Message) {
            const int a = idx(it.from), b = idx(it.to);
            touch(a, b);
            std::string text = it.text;
            if (autonumber) text = std::to_string(++number) + ". " + text;
            const auto tl = splitLines(text);
            const double labelH = text.empty() ? 0 : static_cast<double>(tl.size()) * fs * 0.9 * kLineHeight;
            y += labelH + 8;
            Element c;
            c.kind = Kind::Connector;
            c.layer = layer;
            c.route = "straight";
            const double ax = parts[static_cast<std::size_t>(a)].cx, bx = parts[static_cast<std::size_t>(b)].cx;
            c.start = {ax, y};
            const bool dashed = it.arrow.rfind("--", 0) == 0 || it.arrow.rfind("<<--", 0) == 0;
            if (dashed) c.style.dash = "dashed";
            const std::string tail = it.arrow.substr(it.arrow.find_last_of("-") + 1);
            c.endArrow = tail == ">>" ? "triangle" : tail == ">" ? "none" : tail == "x" ? "cross" : tail == ")" ? "arrow" : "triangle";
            if (it.arrow.rfind("<<", 0) == 0) c.startArrow = "triangle";
            if (a == b) {
                c.waypoints = {{ax + 44, y}, {ax + 44, y + 26}};
                c.end = {ax + 4, y + 26};
            } else {
                c.end = {bx, y};
            }
            const std::string cid = board.add(c);
            stats.ids.push_back(cid);
            ++stats.edges;
            if (!text.empty()) {
                Element label;
                label.kind = Kind::Text;
                label.layer = layer;
                label.text = text;
                label.style.fontSize = fs * 0.9;
                label.style.align = "center";
                Resolved s = resolve(board, label);
                const Pt sz = fitText(Kind::Text, text, s, 10, 10);
                label.w = std::ceil(sz.x);
                label.h = std::ceil(sz.y);
                label.x = a == b ? ax + 52 : (ax + bx) / 2 - label.w / 2;
                label.y = a == b ? y - 4 : y - labelH - 6;
                stats.ids.push_back(board.add(label));
            }
            y += a == b ? 48 : 22;
        } else if (it.type == SeqItem::Note) {
            std::vector<int> who;
            for (const auto& w : it.over) who.push_back(idx(w));
            const int lo = *std::min_element(who.begin(), who.end()), hi = *std::max_element(who.begin(), who.end());
            touch(lo, hi);
            Element n;
            n.kind = Kind::Note;
            n.layer = layer;
            n.text = it.text;
            n.style.fontSize = fs * 0.9;
            Resolved s = resolve(board, n);
            Pt sz = fitText(Kind::Note, it.text, s, 100, 40);
            const double l = parts[static_cast<std::size_t>(lo)].cx, r = parts[static_cast<std::size_t>(hi)].cx;
            if (it.notePos == "over") {
                const double w = std::max(sz.x, r - l + 60);
                n.x = (l + r) / 2 - w / 2;
                n.w = w;
            } else if (it.notePos == "left") {
                n.w = sz.x;
                n.x = l - 16 - sz.x;
            } else {
                n.w = sz.x;
                n.x = l + 16;
            }
            n.y = y + 8;
            n.h = sz.y;
            stats.ids.push_back(board.add(n));
            ++stats.nodes;
            y += sz.y + 24;
        } else if (it.type == SeqItem::BlockStart) {
            open.push_back({it.kindWord, it.text, y, {}, 1 << 30, -1});
            y += lh + 18;
        } else if (it.type == SeqItem::BlockElse) {
            if (!open.empty()) open.back().elses.push_back({y + 4, "[" + it.text + "]"});
            y += lh + 16;
        } else if (!open.empty()) {
            Open o = open.back();
            open.pop_back();
            if (o.hi < 0) o.lo = 0, o.hi = static_cast<int>(parts.size()) - 1;
            for (auto& outer : open) {
                outer.lo = std::min(outer.lo, o.lo);
                outer.hi = std::max(outer.hi, o.hi);
            }
            const double inset = 12.0 * static_cast<double>(open.size());
            Element f;
            f.kind = Kind::Frame;
            f.layer = layer;
            f.text = o.kind + (o.text.empty() ? "" : "  [" + o.text + "]");
            f.style.fill = o.kind == "rect" ? std::optional<std::string>(lightOf(t.stroke, 0.9)) : std::optional<std::string>("none");
            f.style.fontSize = fs * 0.85;
            f.x = parts[static_cast<std::size_t>(o.lo)].cx - parts[static_cast<std::size_t>(o.lo)].w / 2 - 14 + inset;
            f.w = parts[static_cast<std::size_t>(o.hi)].cx + parts[static_cast<std::size_t>(o.hi)].w / 2 + 14 - inset - f.x;
            f.y = o.y0;
            f.h = y - o.y0 + 6;
            frames.push_back(f);
            for (const auto& [ey, et] : o.elses) {
                Element line;
                line.kind = Kind::Connector;
                line.layer = layer;
                line.route = "straight";
                line.start = {f.x, ey};
                line.end = {f.x + f.w, ey};
                line.endArrow = "none";
                line.style.dash = "dashed";
                line.style.strokeWidth = 1;
                line.style.stroke = t.frameStroke;
                stats.ids.push_back(board.add(line));
                Element label;
                label.kind = Kind::Text;
                label.layer = layer;
                label.text = et;
                label.style.fontSize = fs * 0.8;
                Resolved s = resolve(board, label);
                const Pt sz = fitText(Kind::Text, et, s, 10, 10);
                label.w = std::ceil(sz.x);
                label.h = std::ceil(sz.y);
                label.x = f.x + 10;
                label.y = ey + 3;
                stats.ids.push_back(board.add(label));
            }
            y += 16;
        }
    }
    if (!open.empty()) stats.warnings.push_back("a block was never closed with 'end'");
    y += 20;
    // bottom participant boxes and lifelines
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto& p = parts[i];
        Element e;
        e.kind = p.actor ? Kind::Stadium : Kind::Rect;
        e.name = p.id + "_end";
        e.text = p.label;
        e.layer = layer;
        e.x = p.cx - p.w / 2;
        e.y = y;
        e.w = p.w;
        e.h = p.h;
        const std::string bottom = board.add(e);
        stats.ids.push_back(bottom);
        Element life;
        life.kind = Kind::Connector;
        life.layer = layer;
        life.from = topIds[i];
        life.to = bottom;
        life.fromAnchor = "bottom";
        life.toAnchor = "top";
        life.route = "straight";
        life.endArrow = "none";
        life.style.dash = "dashed";
        life.style.strokeWidth = 1.2;
        life.name = p.id + "_lifeline";
        const std::string lid = board.add(life);
        stats.ids.push_back(lid);
        // lifelines go to the back
        auto pos = std::find_if(board.elements.begin(), board.elements.end(), [&](const Element& el) { return el.id == lid; });
        Element moved = *pos;
        board.elements.erase(pos);
        board.elements.insert(board.elements.begin(), moved);
    }
    for (auto& f : frames) {
        const std::string fid = board.add(f);
        stats.ids.push_back(fid);
        ++stats.frames;
        auto pos = std::find_if(board.elements.begin(), board.elements.end(), [&](const Element& el) { return el.id == fid; });
        Element moved = *pos;
        board.elements.erase(pos);
        board.elements.insert(board.elements.begin(), moved);
    }
    return stats;
}

// ---- mind maps and outlines -------------------------------------------------------------------

struct TreeItem {
    int depth = 0;
    std::string text;
    Kind kind = Kind::Rounded;
    bool hasShape = false;
};

ImportStats buildMindmap(Board& board, std::vector<TreeItem> items, const ImportOptions& opt, const std::string& title, const std::string& kindName) {
    ImportStats stats;
    stats.kind = kindName;
    if (items.empty()) throw std::runtime_error("the outline has no items");
    // normalise depths: the first item's depth is 0, children at most one deeper than their parent
    int roots = 0;
    const int base = std::min_element(items.begin(), items.end(), [](const TreeItem& a, const TreeItem& b) { return a.depth < b.depth; })->depth;
    for (auto& it : items) {
        it.depth -= base;
        roots += it.depth == 0 ? 1 : 0;
    }
    if (roots > 1 || !title.empty()) {
        if (roots == 1 && !title.empty()) {
            // a single root already: the title replaces nothing, it becomes the board title
            board.title = title;
        } else {
            for (auto& it : items) ++it.depth;
            TreeItem root;
            root.depth = 0;
            root.text = title.empty() ? "Mind map" : title;
            items.insert(items.begin(), root);
        }
    }
    const Theme& t = theme(board.theme);
    const std::string layer = opt.layer.empty() ? board.layers.front().name : opt.layer;
    std::vector<std::string> stack;  // element id per depth
    std::vector<int> branchOf;       // palette index per depth-1 ancestor
    std::vector<std::string> nodeIds;
    int branch = -1;
    std::vector<int> stackBranch;
    for (const auto& it : items) {
        int d = std::max(0, it.depth);
        if (d > static_cast<int>(stack.size())) d = static_cast<int>(stack.size());
        if (stack.empty()) d = 0;
        stack.resize(static_cast<std::size_t>(d));
        stackBranch.resize(static_cast<std::size_t>(d));
        if (d == 1) ++branch;
        const int myBranch = d == 0 ? -1 : (d == 1 ? branch : stackBranch.back());
        const std::string color = t.palette.empty() || myBranch < 0 ? t.stroke : t.palette[static_cast<std::size_t>(myBranch) % t.palette.size()];
        Element e;
        e.layer = layer;
        e.text = it.text;
        if (d == 0) {
            e.kind = it.hasShape ? it.kind : Kind::Ellipse;
            e.style.fontSize = t.fontSize * 1.25;
            e.style.bold = true;
        } else {
            e.kind = it.hasShape ? it.kind : Kind::Rounded;
            if (d >= 2) e.style.fontSize = t.fontSize * 0.875;
            if (opt.colorBranches) {
                e.style.stroke = color;
                e.style.fill = d == 1 ? lightOf(color, t.name == "dark" ? 0.25 : 0.82) : (t.name == "dark" ? t.fill : t.background);
                if (t.name == "dark" && d == 1) e.style.textColor = "#1e1e2e";
            }
            if (d >= 2) e.style.radius = 18;
        }
        const std::string id = board.add(e);
        Element* el = board.get(id);
        sizeNode(board, *el, d == 0 ? 150 : 90, d == 0 ? 70 : (d == 1 ? 44 : 36));
        stats.ids.push_back(id);
        nodeIds.push_back(id);
        ++stats.nodes;
        if (d > 0) {
            Element c;
            c.kind = Kind::Connector;
            c.layer = layer;
            c.from = stack.back();
            c.to = id;
            c.route = opt.route.empty() ? "curved" : opt.route;
            c.endArrow = "none";
            if (opt.colorBranches) c.style.stroke = color;
            c.style.strokeWidth = d == 1 ? 3 : 2;
            stats.ids.push_back(board.add(c));
            ++stats.edges;
        }
        stack.push_back(id);
        stackBranch.push_back(myBranch);
    }
    (void)branchOf;
    const std::string alg = opt.layout.empty() ? "mindmap" : opt.layout;
    if (alg != "none") {
        LayoutOptions lo;
        lo.algorithm = alg;
        lo.direction = opt.direction.empty() ? (alg == "tree" ? "TB" : "LR") : opt.direction;
        lo.ids = nodeIds;
        lo.keepOrigin = false;
        lo.originX = opt.x;
        lo.originY = opt.y;
        lo.gapX = alg == "mindmap" ? 56 : 40;
        lo.gapY = alg == "mindmap" ? 60 : 60;
        std::string err;
        layout(board, lo, err);
        if (!err.empty()) stats.warnings.push_back(err);
    }
    return stats;
}

// Mermaid mindmap node text: "id[text]", "((text))", "))bang((", ")cloud(", "{{hex}}", or plain.
TreeItem mindmapNode(const std::string& raw, int depth) {
    TreeItem it;
    it.depth = depth;
    std::string s = raw;
    const auto icon = s.find("::icon(");
    if (icon != std::string::npos) s = s.substr(0, icon);
    const auto cls = s.find(":::");
    if (cls != std::string::npos) s = s.substr(0, cls);
    s = trim(s);
    struct Sh {
        const char* open;
        const char* close;
        Kind kind;
    };
    static const Sh shapes[] = {{"((", "))", Kind::Ellipse}, {"))", "((", Kind::Cloud}, {"{{", "}}", Kind::Hexagon},
                                {"(", ")", Kind::Rounded},   {"[", "]", Kind::Rect},    {")", "(", Kind::Cloud}};
    for (const auto& sh : shapes) {
        const auto p = s.find(sh.open);
        const std::size_t ol = std::char_traits<char>::length(sh.open), cl = std::char_traits<char>::length(sh.close);
        if (p == std::string::npos || s.size() < p + ol + cl) continue;
        if (s.compare(s.size() - cl, cl, sh.close) != 0) continue;
        it.text = cleanLabel(s.substr(p + ol, s.size() - cl - p - ol));
        it.kind = sh.kind;
        it.hasShape = true;
        return it;
    }
    it.text = cleanLabel(s);
    return it;
}

int indentOf(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

// Turns raw indents into depths with a stack, so 2-space and 4-space outlines both work.
std::vector<int> depthsOf(const std::vector<int>& indents) {
    std::vector<int> out;
    std::vector<int> stack;
    for (int ind : indents) {
        while (!stack.empty() && ind < stack.back()) stack.pop_back();
        if (stack.empty() || ind > stack.back()) stack.push_back(ind);
        out.push_back(static_cast<int>(stack.size()) - 1);
    }
    return out;
}

}  // namespace

ImportStats fromMermaid(Board& board, const std::string& text, const ImportOptions& opt) {
    std::vector<std::string> lines = splitLines(text);
    // strip a ```mermaid fence and front matter
    std::vector<std::string> body;
    bool inFront = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        if (t.rfind("```", 0) == 0) continue;
        if (t == "---" && (i == 0 || inFront)) {
            inFront = !inFront;
            continue;
        }
        if (inFront) continue;
        body.push_back(lines[i]);
    }
    std::size_t first = 0;
    while (first < body.size() && (trim(stripComments(body[first])).empty())) ++first;
    if (first >= body.size()) throw std::runtime_error("the Mermaid text is empty");
    const std::string header = trim(stripComments(body[first]));
    const std::string low = lower(header);
    if (low.rfind("sequencediagram", 0) == 0) return sequence(board, body, first + 1, opt);
    if (low.rfind("mindmap", 0) == 0) {
        std::vector<int> indents;
        std::vector<std::string> texts;
        for (std::size_t i = first + 1; i < body.size(); ++i) {
            const std::string line = stripComments(body[i]);
            if (trim(line).empty()) continue;
            indents.push_back(indentOf(line));
            texts.push_back(line);
        }
        const auto depths = depthsOf(indents);
        std::vector<TreeItem> items;
        for (std::size_t i = 0; i < texts.size(); ++i) items.push_back(mindmapNode(texts[i], depths[i]));
        return buildMindmap(board, items, opt, {}, "mindmap");
    }
    if (low.rfind("flowchart", 0) == 0 || low.rfind("graph", 0) == 0) {
        auto stats = flowchart(board, body, first + 1, header, opt);
        return stats;
    }
    // no header: read it as a flowchart body
    auto stats = flowchart(board, body, first, "flowchart TB", opt);
    stats.warnings.insert(stats.warnings.begin(), "no diagram header (flowchart, sequenceDiagram, mindmap): read as a flowchart");
    return stats;
}

ImportStats fromOutline(Board& board, const std::string& text, const ImportOptions& opt, const std::string& title) {
    std::vector<int> indents;
    std::vector<std::string> texts;
    std::vector<int> headingLevel;
    for (const auto& raw : splitLines(text)) {
        std::string t = trim(raw);
        if (t.empty() || t.rfind("```", 0) == 0) continue;
        int heading = 0;
        if (t[0] == '#') {
            while (heading < static_cast<int>(t.size()) && t[static_cast<std::size_t>(heading)] == '#') ++heading;
            t = trim(t.substr(static_cast<std::size_t>(heading)));
        } else {
            // bullets: -, *, +, 1. 1) and task boxes
            if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && (t.size() == 1 || t[1] == ' ' || t[1] == '\t')) t = trim(t.substr(1));
            else {
                std::size_t k = 0;
                while (k < t.size() && std::isdigit(static_cast<unsigned char>(t[k]))) ++k;
                if (k > 0 && k + 1 < t.size() && (t[k] == '.' || t[k] == ')') && t[k + 1] == ' ') t = trim(t.substr(k + 1));
            }
            if (t.rfind("[ ] ", 0) == 0 || t.rfind("[x] ", 0) == 0 || t.rfind("[X] ", 0) == 0) t = trim(t.substr(3));
        }
        // **bold** markers
        std::string clean;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if ((t[i] == '*' || t[i] == '_') && i + 1 < t.size() && t[i + 1] == t[i]) {
                ++i;
                continue;
            }
            clean += t[i];
        }
        if (clean.empty()) continue;
        texts.push_back(clean);
        indents.push_back(indentOf(raw));
        headingLevel.push_back(heading);
    }
    if (texts.empty()) throw std::runtime_error("the outline has no items");
    // headings give depth (# = 0, ## = 1, ...); bullets nest under the last heading
    std::vector<TreeItem> items;
    int headingDepth = -1;
    std::vector<int> bulletIndents;
    std::vector<std::size_t> bulletAt;
    auto flushBullets = [&] {
        const auto d = depthsOf(bulletIndents);
        for (std::size_t k = 0; k < bulletAt.size(); ++k) items[bulletAt[k]].depth = headingDepth + 1 + d[k];
        bulletIndents.clear();
        bulletAt.clear();
    };
    for (std::size_t i = 0; i < texts.size(); ++i) {
        TreeItem it;
        it.text = texts[i];
        if (headingLevel[i] > 0) {
            flushBullets();
            headingDepth = headingLevel[i] - 1;
            it.depth = headingDepth;
            items.push_back(it);
        } else {
            bulletIndents.push_back(indents[i]);
            bulletAt.push_back(items.size());
            items.push_back(it);
        }
    }
    flushBullets();
    return buildMindmap(board, items, opt, title, "outline");
}

}  // namespace achclwss::board
