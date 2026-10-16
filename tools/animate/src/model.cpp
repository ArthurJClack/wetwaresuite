// The animation document: element kinds and their properties, easing,
// evaluating keyframes, transforms through parent chains, JSON files and undo.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "wetwaresuite/animate/animate.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/primitives.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::animate {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

// ---- easing ----

const std::vector<std::string>& easeNames() {
    static const std::vector<std::string> v = {"linear", "ease-in", "ease-out", "ease-in-out", "step", "bezier"};
    return v;
}

std::optional<Ease> parseEase(const std::string& name) {
    const std::string s = detail::lower(name);
    if (s == "linear") return Ease::Linear;
    if (s == "ease-in" || s == "in") return Ease::EaseIn;
    if (s == "ease-out" || s == "out") return Ease::EaseOut;
    if (s == "ease-in-out" || s == "ease" || s == "in-out") return Ease::EaseInOut;
    if (s == "step" || s == "hold" || s == "constant") return Ease::Step;
    if (s == "bezier") return Ease::Bezier;
    return std::nullopt;
}

std::string toString(Ease e) {
    switch (e) {
        case Ease::Linear: return "linear";
        case Ease::EaseIn: return "ease-in";
        case Ease::EaseOut: return "ease-out";
        case Ease::EaseInOut: return "ease-in-out";
        case Ease::Step: return "step";
        case Ease::Bezier: return "bezier";
    }
    return "linear";
}

double cubicBezier(double x1, double y1, double x2, double y2, double x) {
    x = std::clamp(x, 0.0, 1.0);
    x1 = std::clamp(x1, 0.0, 1.0);
    x2 = std::clamp(x2, 0.0, 1.0);
    auto bx = [&](double s) { return 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s; };
    auto by = [&](double s) { return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s; };
    auto dx = [&](double s) { return 3 * (1 - s) * (1 - s) * x1 + 6 * (1 - s) * s * (x2 - x1) + 3 * s * s * (1 - x2); };
    double s = x;
    for (int i = 0; i < 8; ++i) {
        const double err = bx(s) - x;
        if (std::fabs(err) < 1e-7) return by(s);
        const double d = dx(s);
        if (std::fabs(d) < 1e-6) break;
        s = std::clamp(s - err / d, 0.0, 1.0);
    }
    double lo = 0, hi = 1;
    s = x;
    for (int i = 0; i < 50; ++i) {
        const double v = bx(s);
        if (std::fabs(v - x) < 1e-7) break;
        if (v < x) lo = s;
        else hi = s;
        s = (lo + hi) / 2;
    }
    return by(s);
}

double ease(Ease e, double t, const std::array<double, 4>& bz) {
    t = std::clamp(t, 0.0, 1.0);
    switch (e) {
        case Ease::Linear: return t;
        case Ease::EaseIn: return cubicBezier(0.42, 0, 1, 1, t);
        case Ease::EaseOut: return cubicBezier(0, 0, 0.58, 1, t);
        case Ease::EaseInOut: return cubicBezier(0.42, 0, 0.58, 1, t);
        case Ease::Step: return t >= 1 ? 1 : 0;
        case Ease::Bezier: return cubicBezier(bz[0], bz[1], bz[2], bz[3], t);
    }
    return t;
}

// ---- kinds and properties ----

const std::vector<std::string>& kinds2d() {
    static const std::vector<std::string> v = {"rect", "ellipse", "polygon", "line", "text", "image", "cel", "bone"};
    return v;
}

const std::vector<std::string>& kinds3d() {
    static const std::vector<std::string> v = {"mesh", "camera"};
    return v;
}

bool is3dKind(const std::string& kind) { return kind == "mesh" || kind == "camera"; }

const std::vector<std::string>& numberProps(const std::string& kind) {
    static const std::vector<std::string> box = {"x", "y", "rotation", "scale", "scale_x", "scale_y", "opacity",
                                                 "anchor_x", "anchor_y", "width", "height", "stroke_width"};
    static const std::vector<std::string> path = {"x", "y", "rotation", "scale", "scale_x", "scale_y", "opacity", "stroke_width"};
    static const std::vector<std::string> text = {"x", "y", "rotation", "scale", "scale_x", "scale_y", "opacity",
                                                  "anchor_x", "anchor_y", "font_size"};
    static const std::vector<std::string> picture = {"x", "y", "rotation", "scale", "scale_x", "scale_y", "opacity",
                                                     "anchor_x", "anchor_y", "width", "height"};
    static const std::vector<std::string> bone = {"x", "y", "rotation", "scale", "scale_x", "scale_y", "length"};
    static const std::vector<std::string> mesh = {"x", "y", "z", "rot_x", "rot_y", "rotation", "scale",
                                                  "scale_x", "scale_y", "scale_z", "opacity"};
    static const std::vector<std::string> camera = {"x", "y", "z", "target_x", "target_y", "target_z", "fov"};
    static const std::vector<std::string> none;
    if (kind == "rect" || kind == "ellipse") return box;
    if (kind == "polygon" || kind == "line") return path;
    if (kind == "text") return text;
    if (kind == "image" || kind == "cel") return picture;
    if (kind == "bone") return bone;
    if (kind == "mesh") return mesh;
    if (kind == "camera") return camera;
    return none;
}

const std::vector<std::string>& colorProps(const std::string& kind) {
    static const std::vector<std::string> both = {"color", "stroke"};
    static const std::vector<std::string> one = {"color"};
    static const std::vector<std::string> none;
    if (kind == "rect" || kind == "ellipse" || kind == "polygon") return both;
    if (kind == "camera" || kind.empty()) return none;
    return one;
}

bool isColorProp(const std::string& prop) { return prop == "color" || prop == "stroke"; }

const std::vector<std::string>& primitiveNames() {
    static const std::vector<std::string> v = {"box", "sphere", "cylinder", "cone", "torus", "plane"};
    return v;
}

Element makeElement(const std::string& kind, const Settings& s) {
    Element e;
    e.kind = kind;
    for (const auto& p : numberProps(kind)) e.numbers[p] = 0;
    auto set = [&](const char* k, double v) {
        if (e.numbers.count(k)) e.numbers[k] = v;
    };
    for (const char* k : {"scale", "scale_x", "scale_y", "scale_z", "opacity"}) set(k, 1);
    set("anchor_x", 0.5);
    set("anchor_y", 0.5);
    const Color ink{31, 42, 55, 255};
    if (kind == "rect" || kind == "ellipse") {
        set("width", 100);
        set("height", 100);
    }
    if (kind == "line") set("stroke_width", 4);
    if (kind == "text") set("font_size", 32);
    if (kind == "cel") {
        set("width", s.width);
        set("height", s.height);
        set("anchor_x", 0);
        set("anchor_y", 0);
    }
    if (kind == "bone") set("length", 100);
    if (kind == "camera") {
        set("y", -8);
        set("z", 4);
        set("fov", 40);
    }
    if (kind == "rect" || kind == "ellipse" || kind == "polygon") {
        e.colors["color"] = {74, 144, 217, 255};
        e.colors["stroke"] = ink;
    } else if (kind == "line" || kind == "text") {
        e.colors["color"] = ink;
    } else if (kind == "image" || kind == "cel") {
        e.colors["color"] = {255, 255, 255, 255};
    } else if (kind == "bone") {
        e.colors["color"] = {224, 69, 123, 255};
    } else if (kind == "mesh") {
        e.colors["color"] = {122, 158, 204, 255};
    }
    if (kind == "mesh") e.source = "box";
    return e;
}

void loadSource(Element& e) {
    if (e.kind == "image") {
        if (e.source.empty()) throw std::runtime_error("an image element needs source=<image file>");
        std::error_code ec;
        if (!fs::is_regular_file(e.source, ec)) throw std::runtime_error("image file not found: " + e.source);
        e.picture = std::make_shared<const Image>(raster::load(e.source));
    } else if (e.kind == "mesh") {
        mesh::Mesh m;
        const std::string src = e.source.empty() ? "box" : e.source;
        const auto& sz = e.size;
        auto at = [&](std::size_t i, double fallback) { return i < sz.size() && sz[i] > 0 ? sz[i] : fallback; };
        if (src == "box") m = mesh::box({at(0, 1), at(1, at(0, 1)), at(2, at(0, 1))});
        else if (src == "sphere") m = mesh::sphere(at(0, 0.5), 40, 20);
        else if (src == "cylinder") m = mesh::cylinder(at(0, 0.5), at(1, 1), 40);
        else if (src == "cone") m = mesh::cone(at(0, 0.5), sz.size() > 1 ? std::max(0.0, sz[1]) : 0.0, at(2, 1), 40);
        else if (src == "torus") m = mesh::torus(at(0, 0.4), at(1, 0.15), 40, 20);
        else if (src == "plane") m = mesh::plane(at(0, 2), at(1, at(0, 2)));
        else {
            std::error_code ec;
            if (!fs::is_regular_file(src, ec)) {
                throw std::runtime_error("mesh file not found: " + src + " (or use a primitive: box, sphere, cylinder, cone, torus, plane)");
            }
            m = mesh::load(src);
        }
        if (m.empty()) throw std::runtime_error("the mesh " + src + " has no triangles");
        if (!m.hasNormals()) mesh::computeNormals(m, 30.0);
        e.geometry = std::make_shared<const mesh::Mesh>(std::move(m));
    }
}

// ---- document ----

std::string AnimDoc::summary() const {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%dx%d %gfps %d frames (%.2gs), %zu layers, %zu elements", settings.width, settings.height,
                  settings.fps, settings.frames, settings.frames / settings.fps, layers.size(), elementCount());
    std::string s = buf;
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> AnimDoc::preview() const {
    FrameOptions o;
    o.scale = std::min(1.0, 512.0 / std::max(settings.width, settings.height));
    auto a = std::make_unique<Attachment>();
    a->mime = "image/png";
    a->bytes = raster::encodePng(renderFrame(*this, 0, o));
    a->name = "frame0.png";
    return a;
}

Layer* AnimDoc::layer(const std::string& id) {
    for (auto& l : layers) {
        if (l.id == id || (!l.name.empty() && l.name == id)) return &l;
    }
    return nullptr;
}

const Layer* AnimDoc::layer(const std::string& id) const { return const_cast<AnimDoc*>(this)->layer(id); }

Element* AnimDoc::element(const std::string& id, Layer** owner) {
    for (auto& l : layers) {
        for (auto& e : l.elements) {
            if (e.id == id) {
                if (owner) *owner = &l;
                return &e;
            }
        }
    }
    // names work too when they're unique enough to find
    for (auto& l : layers) {
        for (auto& e : l.elements) {
            if (!e.name.empty() && e.name == id) {
                if (owner) *owner = &l;
                return &e;
            }
        }
    }
    return nullptr;
}

const Element* AnimDoc::element(const std::string& id, const Layer** owner) const {
    Layer* l = nullptr;
    const Element* e = const_cast<AnimDoc*>(this)->element(id, &l);
    if (owner) *owner = l;
    return e;
}

std::size_t AnimDoc::elementCount() const {
    std::size_t n = 0;
    for (const auto& l : layers) n += l.elements.size();
    return n;
}

// ---- evaluation ----

namespace {

// The keys around `frame`: index of the key at or before it (or -1), and the progress to the next.
template <typename F>
auto interpolate(const std::vector<Key>& keys, double frame, F mix) {
    if (frame <= keys.front().frame) return mix(keys.front(), keys.front(), 0.0);
    if (frame >= keys.back().frame) return mix(keys.back(), keys.back(), 0.0);
    std::size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].frame <= frame) ++i;
    const Key& a = keys[i];
    const Key& b = keys[i + 1];
    const double t = (frame - a.frame) / static_cast<double>(b.frame - a.frame);
    return mix(a, b, ease(b.ease, t, b.bezier));
}

}  // namespace

double numberAt(const Element& e, const std::string& prop, double frame) {
    auto k = e.keys.find(prop);
    if (k != e.keys.end() && !k->second.empty()) {
        return interpolate(k->second, frame, [](const Key& a, const Key& b, double t) { return a.number + (b.number - a.number) * t; });
    }
    auto it = e.numbers.find(prop);
    return it == e.numbers.end() ? 0.0 : it->second;
}

Color colorAt(const Element& e, const std::string& prop, double frame) {
    auto k = e.keys.find(prop);
    if (k != e.keys.end() && !k->second.empty()) {
        return interpolate(k->second, frame, [](const Key& a, const Key& b, double t) {
            auto ch = [t](std::uint8_t x, std::uint8_t y) {
                return static_cast<std::uint8_t>(std::clamp(std::lround(x + (y - x) * t), 0L, 255L));
            };
            return Color{ch(a.color.r, b.color.r), ch(a.color.g, b.color.g), ch(a.color.b, b.color.b), ch(a.color.a, b.color.a)};
        });
    }
    auto it = e.colors.find(prop);
    return it == e.colors.end() ? Color{0, 0, 0, 255} : it->second;
}

void setKey(Element& e, const std::string& prop, const Key& key) {
    auto& keys = e.keys[prop];
    auto it = std::lower_bound(keys.begin(), keys.end(), key.frame, [](const Key& k, int f) { return k.frame < f; });
    if (it != keys.end() && it->frame == key.frame) *it = key;
    else keys.insert(it, key);
}

std::shared_ptr<const Image> celAt(const Element& e, int frame, int* startFrame) {
    auto it = e.cels.upper_bound(frame);
    if (it == e.cels.begin()) return nullptr;
    --it;
    if (startFrame) *startFrame = it->first;
    return it->second;
}

// ---- transforms ----

Affine Affine::operator*(const Affine& o) const {
    return {a * o.a + c * o.b, b * o.a + d * o.b, a * o.c + c * o.d, b * o.c + d * o.d, a * o.e + c * o.f + e, b * o.e + d * o.f + f};
}

void Affine::apply(double x, double y, double& ox, double& oy) const {
    ox = a * x + c * y + e;
    oy = b * x + d * y + f;
}

Affine Affine::inverse() const {
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-12) return {0, 0, 0, 0, 0, 0};
    const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
    return {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
}

double Affine::scale() const { return std::sqrt(std::fabs(a * d - b * c)); }

void elementSize(const Element& e, double frame, double& w, double& h) {
    w = h = 0;
    if (e.kind == "rect" || e.kind == "ellipse" || e.kind == "cel") {
        w = numberAt(e, "width", frame);
        h = numberAt(e, "height", frame);
    } else if (e.kind == "image") {
        w = numberAt(e, "width", frame);
        h = numberAt(e, "height", frame);
        const double pw = e.picture ? e.picture->width() : 0, ph = e.picture ? e.picture->height() : 0;
        if (w <= 0 && h <= 0) {
            w = pw;
            h = ph;
        } else if (w <= 0) {
            w = ph > 0 ? h * pw / ph : h;
        } else if (h <= 0) {
            h = pw > 0 ? w * ph / pw : w;
        }
    } else if (e.kind == "text") {
        detail::textBox(e, numberAt(e, "font_size", frame), w, h);
    }
}

namespace {

Affine local2d(const Element& e, double frame) {
    const double x = numberAt(e, "x", frame), y = numberAt(e, "y", frame);
    const double r = numberAt(e, "rotation", frame) * mesh::kPi / 180.0;
    const double s = numberAt(e, "scale", frame);
    const double sx = s * numberAt(e, "scale_x", frame), sy = s * numberAt(e, "scale_y", frame);
    double w = 0, h = 0;
    elementSize(e, frame, w, h);
    const double ax = e.numbers.count("anchor_x") ? numberAt(e, "anchor_x", frame) * w : 0;
    const double ay = e.numbers.count("anchor_y") ? numberAt(e, "anchor_y", frame) * h : 0;
    const double cs = std::cos(r), sn = std::sin(r);
    Affine m{cs * sx, sn * sx, -sn * sy, cs * sy, x, y};
    return m * Affine{1, 0, 0, 1, -ax, -ay};
}

}  // namespace

Affine worldTransform2d(const AnimDoc& doc, const Element& e, double frame) {
    Affine m = local2d(e, frame);
    const Element* cur = &e;
    for (int depth = 0; depth < 64 && !cur->parent.empty(); ++depth) {
        const Element* p = doc.element(cur->parent);
        if (!p || is3dKind(p->kind)) break;
        m = local2d(*p, frame) * m;
        cur = p;
    }
    return m;
}

mesh::Mat4 worldTransform3d(const AnimDoc& doc, const Element& e, double frame) {
    auto local = [frame](const Element& el) {
        using mesh::Mat4;
        if (el.kind == "camera") return Mat4::translate({numberAt(el, "x", frame), numberAt(el, "y", frame), numberAt(el, "z", frame)});
        const double s = numberAt(el, "scale", frame);
        return Mat4::translate({numberAt(el, "x", frame), numberAt(el, "y", frame), numberAt(el, "z", frame)}) *
               Mat4::rotate({numberAt(el, "rot_x", frame), numberAt(el, "rot_y", frame), numberAt(el, "rotation", frame)}) *
               Mat4::scale({s * numberAt(el, "scale_x", frame), s * numberAt(el, "scale_y", frame), s * numberAt(el, "scale_z", frame)});
    };
    mesh::Mat4 m = local(e);
    const Element* cur = &e;
    for (int depth = 0; depth < 64 && !cur->parent.empty(); ++depth) {
        const Element* p = doc.element(cur->parent);
        if (!p || !is3dKind(p->kind)) break;
        m = local(*p) * m;
        cur = p;
    }
    return m;
}

// ---- JSON ----

namespace {

json keyJson(const Key& k, bool color) {
    json j = {{"frame", k.frame}};
    if (color) j["value"] = k.color.hex();
    else j["value"] = k.number;
    j["ease"] = toString(k.ease);
    if (k.ease == Ease::Bezier) j["bezier"] = {k.bezier[0], k.bezier[1], k.bezier[2], k.bezier[3]};
    return j;
}

Color parseColorJson(const json& j, const std::string& what) {
    if (!j.is_string()) throw std::runtime_error(what + " must be a colour string");
    auto c = Color::parse(j.get<std::string>());
    if (!c) throw std::runtime_error(what + ": unknown colour " + j.get<std::string>());
    return *c;
}

}  // namespace

std::string toJson(const AnimDoc& doc) {
    json j;
    j["format"] = "wetwaresuite-animate";
    j["version"] = 1;
    j["width"] = doc.settings.width;
    j["height"] = doc.settings.height;
    j["fps"] = doc.settings.fps;
    j["frames"] = doc.settings.frames;
    j["background"] = doc.settings.background.hex();
    j["next_element"] = doc.nextElement;
    j["next_layer"] = doc.nextLayer;
    json layers = json::array();
    for (const auto& l : doc.layers) {
        json lj = {{"id", l.id}, {"name", l.name}, {"kind", l.kind}, {"visible", l.visible}, {"opacity", l.opacity}, {"blend", l.blend}};
        json els = json::array();
        for (const auto& e : l.elements) {
            json ej = {{"id", e.id}, {"kind", e.kind}};
            if (!e.name.empty()) ej["name"] = e.name;
            if (!e.parent.empty()) ej["parent"] = e.parent;
            if (!e.visible) ej["visible"] = false;
            json props = json::object();
            for (const auto& [k, v] : e.numbers) props[k] = v;
            for (const auto& [k, v] : e.colors) props[k] = v.hex();
            ej["props"] = props;
            if (!e.keys.empty()) {
                json keys = json::object();
                for (const auto& [prop, list] : e.keys) {
                    if (list.empty()) continue;
                    json arr = json::array();
                    for (const auto& k : list) arr.push_back(keyJson(k, isColorProp(prop)));
                    keys[prop] = arr;
                }
                ej["keys"] = keys;
            }
            if (!e.points.empty()) ej["points"] = e.points;
            if (e.kind == "text") {
                ej["text"] = e.text;
                ej["font"] = e.font;
                ej["bold"] = e.bold;
                ej["italic"] = e.italic;
            }
            if (!e.source.empty()) ej["source"] = e.source;
            if (!e.size.empty()) ej["size"] = e.size;
            if (!e.cels.empty()) {
                json cels = json::object();
                for (const auto& [f, img] : e.cels) cels[std::to_string(f)] = img ? detail::base64Encode(raster::encodePng(*img)) : "";
                ej["cels"] = cels;
            }
            els.push_back(ej);
        }
        lj["elements"] = els;
        layers.push_back(lj);
    }
    j["layers"] = layers;
    return j.dump(1);
}

std::shared_ptr<AnimDoc> fromJson(const std::string& text, const fs::path& baseDir) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not valid JSON: ") + e.what());
    }
    if (!j.is_object() || j.value("format", std::string()) != "wetwaresuite-animate") {
        throw std::runtime_error("not an wetwaresuite animation (.anim.json) file");
    }
    try {
        auto doc = std::make_shared<AnimDoc>();
        auto& s = doc->settings;
        s.width = j.value("width", 640);
        s.height = j.value("height", 360);
        s.fps = j.value("fps", 24.0);
        s.frames = j.value("frames", 48);
        if (j.contains("background")) s.background = parseColorJson(j["background"], "background");
        if (s.width < 1 || s.height < 1 || s.width > 8192 || s.height > 8192 || s.fps <= 0 || s.frames < 1) {
            throw std::runtime_error("bad canvas settings");
        }
        doc->nextElement = j.value("next_element", 1);
        doc->nextLayer = j.value("next_layer", 1);
        for (const auto& lj : j.value("layers", json::array())) {
            Layer l;
            l.id = lj.at("id").get<std::string>();
            l.name = lj.value("name", std::string());
            l.kind = lj.value("kind", std::string("2d"));
            if (l.kind != "2d" && l.kind != "3d") throw std::runtime_error("layer " + l.id + " has an unknown kind " + l.kind);
            l.visible = lj.value("visible", true);
            l.opacity = lj.value("opacity", 1.0);
            l.blend = lj.value("blend", std::string("normal"));
            for (const auto& ej : lj.value("elements", json::array())) {
                const std::string kind = ej.at("kind").get<std::string>();
                const auto& allowed = l.kind == "3d" ? kinds3d() : kinds2d();
                if (std::find(allowed.begin(), allowed.end(), kind) == allowed.end()) {
                    throw std::runtime_error("a " + l.kind + " layer can't hold a " + kind + " element");
                }
                Element e = makeElement(kind, s);
                e.id = ej.at("id").get<std::string>();
                e.name = ej.value("name", std::string());
                e.parent = ej.value("parent", std::string());
                e.visible = ej.value("visible", true);
                const json props = ej.value("props", json::object());
                for (const auto& [k, v] : props.items()) {
                    if (isColorProp(k)) e.colors[k] = parseColorJson(v, e.id + " " + k);
                    else if (v.is_number() && e.numbers.count(k)) e.numbers[k] = v.get<double>();
                }
                const json keyMap = ej.value("keys", json::object());
                for (const auto& [prop, arr] : keyMap.items()) {
                    for (const auto& kj : arr) {
                        Key k;
                        k.frame = kj.at("frame").get<int>();
                        if (isColorProp(prop)) k.color = parseColorJson(kj.at("value"), e.id + " " + prop + " key");
                        else k.number = kj.at("value").get<double>();
                        auto ez = parseEase(kj.value("ease", std::string("linear")));
                        if (!ez) throw std::runtime_error(e.id + " has a key with an unknown ease");
                        k.ease = *ez;
                        if (kj.contains("bezier") && kj["bezier"].size() == 4) {
                            for (int i = 0; i < 4; ++i) k.bezier[static_cast<std::size_t>(i)] = kj["bezier"][static_cast<std::size_t>(i)].get<double>();
                        }
                        setKey(e, prop, k);
                    }
                }
                if (ej.contains("points")) e.points = ej["points"].get<std::vector<double>>();
                e.text = ej.value("text", std::string());
                e.font = ej.value("font", std::string("sans"));
                e.bold = ej.value("bold", false);
                e.italic = ej.value("italic", false);
                e.source = ej.value("source", e.source);
                if (ej.contains("size")) e.size = ej["size"].get<std::vector<double>>();
                if (!e.source.empty() && (kind == "image" || kind == "mesh")) {
                    const auto& prims = primitiveNames();
                    const bool primitive = kind == "mesh" && std::find(prims.begin(), prims.end(), e.source) != prims.end();
                    if (!primitive && fs::path(e.source).is_relative() && !baseDir.empty()) {
                        e.source = (baseDir / e.source).lexically_normal().string();
                    }
                    loadSource(e);
                }
                const json cels = ej.value("cels", json::object());
                for (const auto& [f, data] : cels.items()) {
                    const int frame = std::stoi(f);
                    const std::string png = detail::base64Decode(data.get<std::string>());
                    e.cels[frame] = std::make_shared<const Image>(png.empty() ? Image(1, 1) : raster::decode(png));
                }
                l.elements.push_back(std::move(e));
            }
            doc->layers.push_back(std::move(l));
        }
        return doc;
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("bad animation file: ") + e.what());
    }
}

// ---- undo ----

Result AnimEdit::apply() {
    before_ = {doc_->settings, doc_->layers, doc_->nextElement, doc_->nextLayer};
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) revert();
    return r;
}

void AnimEdit::revert() {
    doc_->settings = before_.settings;
    doc_->layers = before_.layers;
    doc_->nextElement = before_.nextElement;
    doc_->nextLayer = before_.nextLayer;
}

}  // namespace wetwaresuite::animate
