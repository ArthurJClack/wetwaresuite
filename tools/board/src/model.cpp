// The board model: elements, layers, themes, style resolution, undo
// snapshots and the native .board.json format.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "achclwss/board/board.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/vector/vector.hpp"

namespace achclwss::board {

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---- boxes ------------------------------------------------------------------------------

void Box::add(double x, double y) {
    if (empty) {
        x0 = x1 = x;
        y0 = y1 = y;
        empty = false;
        return;
    }
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
}

void Box::add(const Box& b) {
    if (b.empty) return;
    add(b.x0, b.y0);
    add(b.x1, b.y1);
}

Box Box::grown(double by) const {
    Box b = *this;
    if (b.empty) return b;
    b.x0 -= by;
    b.y0 -= by;
    b.x1 += by;
    b.y1 += by;
    return b;
}

bool Box::intersects(const Box& b) const { return !empty && !b.empty && x0 < b.x1 && b.x0 < x1 && y0 < b.y1 && b.y0 < y1; }
bool Box::contains(const Box& b) const { return !empty && !b.empty && b.x0 >= x0 && b.x1 <= x1 && b.y0 >= y0 && b.y1 <= y1; }

// ---- kinds ------------------------------------------------------------------------------

namespace {

struct KindName {
    Kind kind;
    const char* name;
};
constexpr KindName kKinds[] = {
    {Kind::Rect, "rect"},           {Kind::Rounded, "rounded"},     {Kind::Stadium, "stadium"},
    {Kind::Ellipse, "ellipse"},     {Kind::Diamond, "diamond"},     {Kind::Hexagon, "hexagon"},
    {Kind::Parallelogram, "parallelogram"}, {Kind::Trapezoid, "trapezoid"}, {Kind::Triangle, "triangle"},
    {Kind::Cylinder, "cylinder"},   {Kind::Cloud, "cloud"},         {Kind::Note, "note"},
    {Kind::Subroutine, "subroutine"}, {Kind::Text, "text"},         {Kind::Image, "image"},
    {Kind::Frame, "frame"},         {Kind::Group, "group"},         {Kind::Connector, "connector"},
    {Kind::Ink, "ink"},
};

}  // namespace

const char* toString(Kind kind) {
    for (const auto& k : kKinds) {
        if (k.kind == kind) return k.name;
    }
    return "rect";
}

std::optional<Kind> parseKind(const std::string& raw) {
    std::string name;
    for (char c : raw) name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& k : kKinds) {
        if (name == k.name) return k.kind;
    }
    if (name == "rectangle" || name == "box" || name == "square" || name == "process") return Kind::Rect;
    if (name == "round" || name == "roundrect" || name == "rounded_rect") return Kind::Rounded;
    if (name == "circle" || name == "oval") return Kind::Ellipse;
    if (name == "rhombus" || name == "decision") return Kind::Diamond;
    if (name == "database" || name == "db" || name == "drum") return Kind::Cylinder;
    if (name == "sticky" || name == "sticky_note" || name == "postit") return Kind::Note;
    if (name == "pill" || name == "terminal") return Kind::Stadium;
    if (name == "label") return Kind::Text;
    if (name == "picture" || name == "img") return Kind::Image;
    if (name == "arrow" || name == "line" || name == "edge") return Kind::Connector;
    if (name == "freehand" || name == "pen" || name == "freedraw") return Kind::Ink;
    if (name == "io" || name == "data") return Kind::Parallelogram;
    return std::nullopt;
}

const std::vector<std::string>& shapeKindNames() {
    static const std::vector<std::string> names = {"rect", "rounded", "stadium", "ellipse", "circle", "diamond", "hexagon", "parallelogram",
                                                   "trapezoid", "triangle", "cylinder", "cloud", "note", "subroutine", "text", "image", "frame"};
    return names;
}

bool isNode(Kind kind) { return kind != Kind::Connector && kind != Kind::Ink; }

// ---- board ------------------------------------------------------------------------------

int Board::find(const std::string& idOrName) const {
    if (idOrName.empty()) return -1;
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (elements[i].id == idOrName) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (elements[i].name == idOrName) return static_cast<int>(i);
    }
    return -1;
}

Element* Board::get(const std::string& idOrName) {
    const int i = find(idOrName);
    return i < 0 ? nullptr : &elements[static_cast<std::size_t>(i)];
}

const Element* Board::get(const std::string& idOrName) const {
    const int i = find(idOrName);
    return i < 0 ? nullptr : &elements[static_cast<std::size_t>(i)];
}

std::string Board::add(Element e) {
    std::string id;
    do {
        id = "e" + std::to_string(nextId++);
    } while (find(id) >= 0);
    e.id = id;
    if (e.layer.empty()) e.layer = layers.empty() ? "default" : layers.front().name;
    ensureLayer(e.layer);
    elements.push_back(std::move(e));
    return id;
}

Layer* Board::layer(const std::string& name) {
    for (auto& l : layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

const Layer* Board::layer(const std::string& name) const {
    for (const auto& l : layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

Layer& Board::ensureLayer(const std::string& name) {
    if (Layer* l = layer(name)) return *l;
    Layer l;
    l.name = name;
    layers.push_back(l);
    return layers.back();
}

std::vector<std::string> Board::children(const std::string& id) const {
    std::vector<std::string> out;
    for (const auto& e : elements) {
        if (e.parent == id && !id.empty()) out.push_back(e.id);
    }
    return out;
}

std::vector<std::string> Board::descendants(const std::string& id) const {
    std::vector<std::string> out;
    std::vector<std::string> todo{id};
    std::set<std::string> seen{id};
    while (!todo.empty()) {
        const std::string cur = todo.back();
        todo.pop_back();
        for (const auto& c : children(cur)) {
            if (seen.insert(c).second) {
                out.push_back(c);
                todo.push_back(c);
            }
        }
    }
    return out;
}

int Board::remove(const std::vector<std::string>& ids) {
    std::set<std::string> gone;
    for (const auto& raw : ids) {
        const Element* e = get(raw);
        if (!e) continue;
        gone.insert(e->id);
        for (const auto& d : descendants(e->id)) gone.insert(d);
    }
    for (const auto& e : elements) {
        if (e.kind == Kind::Connector && (gone.count(e.from) || gone.count(e.to))) gone.insert(e.id);
    }
    const std::size_t before = elements.size();
    elements.erase(std::remove_if(elements.begin(), elements.end(), [&](const Element& e) { return gone.count(e.id) > 0; }),
                   elements.end());
    return static_cast<int>(before - elements.size());
}

std::string BoardDoc::summary() const {
    int shapes = 0, connectors = 0, ink = 0;
    for (const auto& e : board.elements) {
        if (e.kind == Kind::Connector) ++connectors;
        else if (e.kind == Kind::Ink) ++ink;
        else ++shapes;
    }
    std::string s = std::to_string(shapes) + (shapes == 1 ? " shape, " : " shapes, ") + std::to_string(connectors) +
                    (connectors == 1 ? " connector, " : " connectors, ") + std::to_string(ink) + (ink == 1 ? " ink stroke" : " ink strokes") +
                    ", " + std::to_string(board.layers.size()) + (board.layers.size() == 1 ? " layer" : " layers") + ", theme " +
                    board.theme;
    if (!board.title.empty()) s = "\"" + board.title + "\": " + s;
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> BoardDoc::preview() const {
    RenderOptions o;
    const Box r = renderRegion(board, o);
    o.scale = std::clamp(768.0 / std::max({r.w(), r.h(), 1.0}), 0.05, 2.0);
    return std::make_unique<Attachment>(Attachment{"image/png", raster::encodePng(renderImage(board, o)), "board.png"});
}

Result BoardEdit::apply() {
    before_ = doc_->board;
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void BoardEdit::revert() { doc_->board = before_; }

// ---- themes -----------------------------------------------------------------------------

namespace {

std::vector<Theme> makeThemes() {
    std::vector<Theme> t;
    Theme d;
    d.name = "default";
    d.background = "#ffffff";
    d.stroke = "#364fc7";
    d.fill = "#edf2ff";
    d.text = "#1e1e1e";
    d.line = "#495057";
    d.noteFill = "#fff3bf";
    d.noteText = "#1e1e1e";
    d.frameStroke = "#adb5bd";
    d.frameFill = "#f8f9fa";
    d.palette = {"#e03131", "#1971c2", "#2f9e44", "#f08c00", "#9c36b5", "#0c8599", "#e8590c", "#5c940d"};
    t.push_back(d);

    Theme s = d;
    s.name = "sketchy";
    s.stroke = "#1e1e1e";
    s.fill = "#a5d8ff";
    s.line = "#1e1e1e";
    s.noteFill = "#ffec99";
    s.frameStroke = "#868e96";
    s.frameFill = "none";
    s.sketch = true;
    s.roughness = 1;
    s.fillStyle = "hachure";
    s.fontSize = 18;
    t.push_back(s);

    Theme k = d;
    k.name = "dark";
    k.background = "#1e1e2e";
    k.stroke = "#89b4fa";
    k.fill = "#313244";
    k.text = "#cdd6f4";
    k.line = "#a6adc8";
    k.noteFill = "#f9e2af";
    k.noteText = "#1e1e2e";
    k.frameStroke = "#6c7086";
    k.frameFill = "#181825";
    k.palette = {"#f38ba8", "#89b4fa", "#a6e3a1", "#fab387", "#cba6f7", "#94e2d5", "#f9e2af", "#74c7ec"};
    t.push_back(k);

    Theme b = d;
    b.name = "blueprint";
    b.background = "#0b3d91";
    b.stroke = "#ffffff";
    b.fill = "none";
    b.text = "#ffffff";
    b.line = "#dbe4ff";
    b.noteFill = "#1c56b8";
    b.noteText = "#ffffff";
    b.frameStroke = "#74a3ff";
    b.frameFill = "none";
    b.font = "mono";
    b.strokeWidth = 1.5;
    b.palette = {"#ffffff", "#a5d8ff", "#ffe066", "#b2f2bb", "#ffc9c9", "#d0bfff"};
    t.push_back(b);

    Theme p = d;
    p.name = "pastel";
    p.background = "#fffdf7";
    p.stroke = "#5f3dc4";
    p.fill = "#f3f0ff";
    p.text = "#2b2b2b";
    p.line = "#7950f2";
    p.noteFill = "#ffdeeb";
    p.frameStroke = "#d0bfff";
    p.frameFill = "#fbf9ff";
    p.palette = {"#f783ac", "#74c0fc", "#8ce99a", "#ffc078", "#b197fc", "#66d9e8"};
    t.push_back(p);

    Theme m = d;
    m.name = "mono";
    m.stroke = "#000000";
    m.fill = "#ffffff";
    m.text = "#000000";
    m.line = "#000000";
    m.noteFill = "#f1f3f5";
    m.frameStroke = "#000000";
    m.frameFill = "none";
    m.strokeWidth = 1.5;
    m.font = "serif";
    m.palette = {"#000000", "#495057", "#868e96"};
    t.push_back(m);
    return t;
}

const std::vector<Theme>& themes() {
    static const std::vector<Theme> t = makeThemes();
    return t;
}

}  // namespace

const Theme& theme(const std::string& name) {
    for (const auto& t : themes()) {
        if (t.name == name) return t;
    }
    return themes().front();
}

const std::vector<std::string>& themeNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& t : themes()) n.push_back(t.name);
        return n;
    }();
    return names;
}

std::optional<Color> parseColor(const std::string& text) {
    if (text.empty() || text == "none" || text == "transparent") return std::nullopt;
    return vector::parseColor(text);
}

bool validColor(const std::string& text) { return text == "none" || text == "transparent" || vector::parseColor(text).has_value(); }

Color boardBackground(const Board& board) {
    const std::string spec = board.background.empty() ? theme(board.theme).background : board.background;
    return parseColor(spec).value_or(Color{255, 255, 255, 0});
}

Resolved resolve(const Board& board, const Element& e) {
    const Theme& t = theme(board.theme);
    const Style& s = e.style;
    Resolved r;
    std::string fill = t.fill, stroke = t.stroke, text = t.text;
    if (e.kind == Kind::Note) {
        fill = t.noteFill;
        stroke = "none";
        text = t.noteText;
        r.shadow = !t.sketch;
    } else if (e.kind == Kind::Frame) {
        fill = t.frameFill;
        stroke = t.frameStroke;
    } else if (e.kind == Kind::Text || e.kind == Kind::Image || e.kind == Kind::Group) {
        fill = "none";
        stroke = "none";
    } else if (e.kind == Kind::Connector || e.kind == Kind::Ink) {
        fill = "none";
        stroke = e.kind == Kind::Connector ? t.line : t.text;
    }
    if (s.fill) fill = *s.fill;
    if (s.stroke) stroke = *s.stroke;
    if (s.textColor) text = *s.textColor;
    r.fill = parseColor(fill);
    r.stroke = parseColor(stroke);
    r.text = parseColor(text).value_or(Color{30, 30, 30, 255});
    r.strokeWidth = s.strokeWidth.value_or(e.kind == Kind::Frame ? 1.5 : t.strokeWidth);
    r.dash = s.dash.value_or(e.kind == Kind::Frame && !t.sketch ? "dashed" : "solid");
    r.opacity = std::clamp(s.opacity.value_or(1.0), 0.0, 1.0);
    r.fontSize = std::max(1.0, s.fontSize.value_or(e.kind == Kind::Connector ? t.fontSize * 0.85 : t.fontSize));
    r.font = s.font.value_or(t.font);
    r.bold = s.bold.value_or(e.kind == Kind::Frame);
    r.italic = s.italic.value_or(false);
    r.align = s.align.value_or(e.kind == Kind::Text || e.kind == Kind::Frame ? "left" : "center");
    r.valign = s.valign.value_or(e.kind == Kind::Frame ? "top" : (e.kind == Kind::Text ? "top" : "middle"));
    r.fillStyle = s.fillStyle.value_or(e.kind == Kind::Note || e.kind == Kind::Frame ? "solid" : t.fillStyle);
    r.sketch = s.sketch.value_or(t.sketch);
    r.roughness = std::clamp(s.roughness.value_or(t.roughness), 0.0, 4.0);
    r.radius = std::max(0.0, s.radius.value_or(12.0));
    if (s.shadow) r.shadow = *s.shadow;
    return r;
}

// ---- base64 and images ------------------------------------------------------------------

std::string base64Encode(const std::string& bytes) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                           static_cast<unsigned char>(bytes[i + 2]);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& text) {
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        else break;
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xff);
        }
    }
    return out;
}

std::shared_ptr<raster::Image> loadImageSrc(const std::string& src) {
    try {
        if (src.rfind("data:", 0) == 0) {
            const auto comma = src.find(',');
            if (comma == std::string::npos) return nullptr;
            const std::string head = src.substr(0, comma);
            if (head.find(";base64") == std::string::npos) return nullptr;
            return std::make_shared<raster::Image>(raster::decode(base64Decode(src.substr(comma + 1))));
        }
        if (src.empty() || !fs::exists(src)) return nullptr;
        return std::make_shared<raster::Image>(raster::load(src));
    } catch (const std::exception&) {
        return nullptr;
    }
}

// ---- native JSON ------------------------------------------------------------------------

namespace {

json styleJson(const Style& s) {
    json j = json::object();
    if (s.fill) j["fill"] = *s.fill;
    if (s.stroke) j["stroke"] = *s.stroke;
    if (s.textColor) j["text_color"] = *s.textColor;
    if (s.strokeWidth) j["stroke_width"] = *s.strokeWidth;
    if (s.dash) j["dash"] = *s.dash;
    if (s.opacity) j["opacity"] = *s.opacity;
    if (s.fontSize) j["font_size"] = *s.fontSize;
    if (s.font) j["font"] = *s.font;
    if (s.bold) j["bold"] = *s.bold;
    if (s.italic) j["italic"] = *s.italic;
    if (s.align) j["align"] = *s.align;
    if (s.valign) j["valign"] = *s.valign;
    if (s.fillStyle) j["fill_style"] = *s.fillStyle;
    if (s.sketch) j["sketch"] = *s.sketch;
    if (s.roughness) j["roughness"] = *s.roughness;
    if (s.radius) j["radius"] = *s.radius;
    if (s.shadow) j["shadow"] = *s.shadow;
    return j;
}

template <typename T>
void read(const json& j, const char* key, std::optional<T>& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) out = it->get<T>();
}

Style styleFrom(const json& j) {
    Style s;
    if (!j.is_object()) return s;
    read(j, "fill", s.fill);
    read(j, "stroke", s.stroke);
    read(j, "text_color", s.textColor);
    read(j, "stroke_width", s.strokeWidth);
    read(j, "dash", s.dash);
    read(j, "opacity", s.opacity);
    read(j, "font_size", s.fontSize);
    read(j, "font", s.font);
    read(j, "bold", s.bold);
    read(j, "italic", s.italic);
    read(j, "align", s.align);
    read(j, "valign", s.valign);
    read(j, "fill_style", s.fillStyle);
    read(j, "sketch", s.sketch);
    read(j, "roughness", s.roughness);
    read(j, "radius", s.radius);
    read(j, "shadow", s.shadow);
    return s;
}

std::string str(const json& j, const char* key, const std::string& fallback = {}) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

double number(const json& j, const char* key, double fallback = 0) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

}  // namespace

std::string toJson(const Board& b) {
    json j;
    j["format"] = "achclwss-board";
    j["version"] = 1;
    j["title"] = b.title;
    j["theme"] = b.theme;
    if (!b.background.empty()) j["background"] = b.background;
    j["next_id"] = b.nextId;
    json layers = json::array();
    for (const auto& l : b.layers) layers.push_back({{"name", l.name}, {"visible", l.visible}, {"locked", l.locked}, {"opacity", l.opacity}});
    j["layers"] = layers;
    json els = json::array();
    for (const auto& e : b.elements) {
        json o;
        o["id"] = e.id;
        o["type"] = toString(e.kind);
        if (!e.name.empty()) o["name"] = e.name;
        o["x"] = e.x;
        o["y"] = e.y;
        o["w"] = e.w;
        o["h"] = e.h;
        if (!e.text.empty()) o["text"] = e.text;
        o["layer"] = e.layer;
        if (!e.parent.empty()) o["parent"] = e.parent;
        json st = styleJson(e.style);
        if (!st.empty()) o["style"] = st;
        if (!e.src.empty()) o["src"] = e.src;
        if (!e.link.empty()) o["link"] = e.link;
        if (e.kind == Kind::Connector) {
            if (!e.from.empty()) o["from"] = e.from;
            if (!e.to.empty()) o["to"] = e.to;
            o["from_anchor"] = e.fromAnchor;
            o["to_anchor"] = e.toAnchor;
            o["route"] = e.route;
            o["start_arrow"] = e.startArrow;
            o["end_arrow"] = e.endArrow;
            o["start"] = {e.start.x, e.start.y};
            o["end"] = {e.end.x, e.end.y};
            if (!e.waypoints.empty()) {
                json w = json::array();
                for (const auto& p : e.waypoints) w.push_back({p.x, p.y});
                o["waypoints"] = w;
            }
        }
        if (e.kind == Kind::Ink) {
            json pts = json::array();
            for (const auto& p : e.ink) pts.push_back({std::round(p.x * 100) / 100, std::round(p.y * 100) / 100, std::round(p.pressure * 1000) / 1000});
            o["points"] = pts;
            o["width"] = e.inkWidth;
        }
        els.push_back(o);
    }
    j["elements"] = els;
    return j.dump(1);
}

Board fromJson(const std::string& text) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    if (!j.is_object() || !j.contains("elements")) throw std::runtime_error("not a .board.json file (no \"elements\")");
    Board b;
    b.title = str(j, "title");
    b.theme = str(j, "theme", "default");
    b.background = str(j, "background");
    b.nextId = static_cast<int>(number(j, "next_id", 1));
    if (j.contains("layers") && j["layers"].is_array() && !j["layers"].empty()) {
        b.layers.clear();
        for (const auto& l : j["layers"]) {
            Layer layer;
            layer.name = str(l, "name", "default");
            layer.visible = l.value("visible", true);
            layer.locked = l.value("locked", false);
            layer.opacity = number(l, "opacity", 1);
            b.layers.push_back(layer);
        }
    }
    int maxId = 0;
    for (const auto& o : j["elements"]) {
        Element e;
        e.id = str(o, "id");
        auto kind = parseKind(str(o, "type", "rect"));
        e.kind = kind.value_or(Kind::Rect);
        e.name = str(o, "name");
        e.x = number(o, "x");
        e.y = number(o, "y");
        e.w = number(o, "w");
        e.h = number(o, "h");
        e.text = str(o, "text");
        e.layer = str(o, "layer", b.layers.front().name);
        e.parent = str(o, "parent");
        if (o.contains("style")) e.style = styleFrom(o["style"]);
        e.src = str(o, "src");
        e.link = str(o, "link");
        e.from = str(o, "from");
        e.to = str(o, "to");
        e.fromAnchor = str(o, "from_anchor", "auto");
        e.toAnchor = str(o, "to_anchor", "auto");
        e.route = str(o, "route", "straight");
        e.startArrow = str(o, "start_arrow", "none");
        e.endArrow = str(o, "end_arrow", "arrow");
        auto pt = [&](const char* key) {
            Pt p;
            if (o.contains(key) && o[key].is_array() && o[key].size() >= 2) p = {o[key][0].get<double>(), o[key][1].get<double>()};
            return p;
        };
        e.start = pt("start");
        e.end = pt("end");
        if (o.contains("waypoints")) {
            for (const auto& w : o["waypoints"]) {
                if (w.is_array() && w.size() >= 2) e.waypoints.push_back({w[0].get<double>(), w[1].get<double>()});
            }
        }
        if (o.contains("points")) {
            for (const auto& w : o["points"]) {
                if (w.is_array() && w.size() >= 2) e.ink.push_back({w[0].get<double>(), w[1].get<double>(), w.size() > 2 ? w[2].get<double>() : 1.0});
            }
        }
        e.inkWidth = number(o, "width", 4);
        if (e.id.empty() || b.find(e.id) >= 0) e.id = "e" + std::to_string(b.nextId++);
        if (e.id.size() > 1 && e.id[0] == 'e') {
            try {
                maxId = std::max(maxId, std::stoi(e.id.substr(1)));
            } catch (const std::exception&) {
            }
        }
        b.ensureLayer(e.layer);
        b.elements.push_back(std::move(e));
    }
    b.nextId = std::max(b.nextId, maxId + 1);
    return b;
}

// ---- files ------------------------------------------------------------------------------

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void writeFile(const fs::path& path, const std::string& bytes) {
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path.string());
    out << bytes;
    if (!out) throw std::runtime_error("can't write " + path.string());
}

}  // namespace

std::string formatFor(const fs::path& path) {
    const std::string name = lower(path.filename().string());
    auto ends = [&](const std::string& s) { return name.size() >= s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0; };
    if (ends(".board.json") || ends(".board") || ends(".json")) return "board";
    if (ends(".excalidraw") || ends(".excalidraw.json")) return "excalidraw";
    if (ends(".drawio") || ends(".dio") || ends(".xml")) return "drawio";
    if (ends(".svg")) return "svg";
    if (ends(".pdf")) return "pdf";
    for (const char* ext : {"png", "jpg", "jpeg", "bmp", "tga", "ppm"}) {
        if (ends(std::string(".") + ext)) return ext;
    }
    return "";
}

Board loadBoard(const fs::path& path) {
    const std::string bytes = readFile(path);
    std::string fmt = formatFor(path);
    if (fmt == "board") {
        // a .json may be an Excalidraw scene
        if (bytes.find("\"excalidraw\"") != std::string::npos && bytes.find("achclwss-board") == std::string::npos) fmt = "excalidraw";
    }
    if (fmt.empty()) {
        if (bytes.find("<mxfile") != std::string::npos || bytes.find("<mxGraphModel") != std::string::npos) fmt = "drawio";
        else if (bytes.find("\"excalidraw\"") != std::string::npos) fmt = "excalidraw";
        else fmt = "board";
    }
    Board b;
    if (fmt == "board") b = fromJson(bytes);
    else if (fmt == "excalidraw") b = fromExcalidraw(bytes);
    else if (fmt == "drawio") b = fromDrawio(bytes);
    else throw std::runtime_error("boards open from .board.json, .excalidraw or .drawio, not " + path.filename().string());
    // image paths relative to the file
    for (auto& e : b.elements) {
        if (e.kind == Kind::Image && !e.src.empty() && e.src.rfind("data:", 0) != 0) {
            fs::path p = e.src;
            if (p.is_relative() && path.has_parent_path()) e.src = (path.parent_path() / p).lexically_normal().string();
        }
    }
    if (b.title.empty()) {
        std::string stem = path.filename().string();
        const auto dot = stem.find('.');
        b.title = dot == std::string::npos ? stem : stem.substr(0, dot);
    }
    return b;
}

void saveBoard(const Board& board, const fs::path& path, const std::string& format, const RenderOptions& options) {
    std::string fmt = format.empty() || format == "auto" ? formatFor(path) : format;
    if (fmt == "json") fmt = "board";
    if (fmt == "jpeg") fmt = "jpg";
    if (fmt.empty()) throw std::runtime_error("can't tell the format of " + path.filename().string() +
                                              " (use .board.json, .excalidraw, .drawio, .svg, .pdf or .png)");
    if (fmt == "board") return writeFile(path, toJson(board));
    if (fmt == "excalidraw") return writeFile(path, toExcalidraw(board));
    if (fmt == "drawio") return writeFile(path, toDrawio(board));
    if (fmt == "svg") return writeFile(path, toSvg(board, options));
    if (fmt == "pdf") return writeFile(path, toPdf(board, options));
    RenderOptions o = options;
    if (fmt == "jpg" || fmt == "bmp" || fmt == "ppm") o.transparent = false;
    const raster::Image img = renderImage(board, o);
    if (fmt == "png") return writeFile(path, raster::encodePng(img));
    if (fmt == "jpg") return writeFile(path, raster::encodeJpeg(img, 90));
    if (fmt == "bmp" || fmt == "tga" || fmt == "ppm") {
        if (path.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
        }
        return raster::save(img, path);
    }
    throw std::runtime_error("unknown format: " + fmt);
}

}  // namespace achclwss::board
