// Every animate.* command. Edits run inside an AnimEdit, so each is one undo
// step and a failed edit leaves the document as it was. Exports render every
// frame in-process; only export_video needs ffmpeg.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <set>

#include "achclwss/animate/animate.hpp"
#include "achclwss/core/process.hpp"
#include "achclwss/mesh/primitives.hpp"
#include "achclwss/pdf/page.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/ops.hpp"
#include "achclwss/raster/paint.hpp"
#include "internal.hpp"

namespace achclwss::animate {

namespace fs = std::filesystem;
using detail::lower;
using detail::num;
using json = nlohmann::ordered_json;

namespace {

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::int64_t i64(int v) { return static_cast<std::int64_t>(v); }
std::int64_t i64(std::size_t v) { return static_cast<std::int64_t>(v); }

std::string join(const std::vector<std::string>& v, const std::string& sep = ", ") {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

bool contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open animation, e.g. anim1", true}; }
ParamSpec elementParam() { return {"element", ParamType::String, "element id (e.g. e3) or its name; animate.info lists them", true}; }
ParamSpec previewParam() {
    return {"preview", ParamType::Bool, "attach a PNG of the frame (the `frame` param, else frame 0) so you can see the result", false, {}, false};
}
ParamSpec overwriteParam() { return {"overwrite", ParamType::Bool, "replace outputs that already exist", false, {}, true}; }
ParamSpec easeParam() {
    return {"ease", ParamType::String, "how the value travels into this key from the key before: linear, ease-in (slow start), "
                                       "ease-out (slow finish), ease-in-out, step (hold the old value, then jump) or bezier (with the bezier param)", false, easeNames(), std::string("linear")};
}
ParamSpec bezierParam() {
    return {"bezier", ParamType::NumberList, "custom curve x1,y1,x2,y2 like CSS cubic-bezier (implies ease=bezier), e.g. 0.34,1.56,0.64,1 overshoots", false};
}

const std::vector<std::string> kBlends = {"normal", "multiply", "screen", "overlay", "add", "darken", "lighten", "difference"};
const std::vector<std::string> kFonts = {"sans", "serif", "mono"};

// The element properties a command can take, picked by name.
std::vector<ParamSpec> propSpecs() {
    return {
        {"name", ParamType::String, "a name to refer to the element by (instead of its id)", false},
        {"parent", ParamType::String, "element (id or name) this one moves, turns and scales with: bones and cutout rigs; \"none\" to detach", false},
        {"visible", ParamType::Bool, "show or hide the element", false},
        {"x", ParamType::Number, "2D: where the anchor sits, px from the left (in the parent's space when parented). 3D: world x", false},
        {"y", ParamType::Number, "2D: px from the top (y grows down). 3D: world y (depth; z is up)", false},
        {"z", ParamType::Number, "3D: world z (up)", false},
        {"rotation", ParamType::Number, "degrees; 2D turns clockwise about the anchor, 3D turns about z", false},
        {"rot_x", ParamType::Number, "3D: degrees about x (applied first)", false},
        {"rot_y", ParamType::Number, "3D: degrees about y", false},
        {"scale", ParamType::Number, "uniform scale, 1 = natural size", false},
        {"scale_x", ParamType::Number, "extra horizontal scale (multiplies scale; -1 mirrors)", false},
        {"scale_y", ParamType::Number, "extra vertical scale (multiplies scale)", false},
        {"scale_z", ParamType::Number, "3D: extra scale along z", false},
        {"opacity", ParamType::Number, "0 (invisible) .. 1 (solid)", false},
        {"anchor_x", ParamType::Number, "the point x/y refer to and rotation turns about, as a fraction of the width: 0 left, 0.5 centre, 1 right", false},
        {"anchor_y", ParamType::Number, "the same as a fraction of the height: 0 top, 0.5 centre, 1 bottom", false},
        {"width", ParamType::Number, "rect/ellipse/cel box width in px; image: display width (0 = natural, or from height keeping aspect)", false},
        {"height", ParamType::Number, "rect/ellipse/cel box height in px; image: display height", false},
        {"stroke_width", ParamType::Number, "outline width in px for rect/ellipse/polygon (0 = none); line width for line", false},
        {"font_size", ParamType::Number, "text size in px", false},
        {"length", ParamType::Number, "bone length in px (children attach at x=length to sit on the tip)", false},
        {"target_x", ParamType::Number, "camera: the point it looks at, x", false},
        {"target_y", ParamType::Number, "camera: look-at y", false},
        {"target_z", ParamType::Number, "camera: look-at z", false},
        {"fov", ParamType::Number, "camera: vertical field of view in degrees", false},
        {"color", ParamType::String, "fill colour (shapes, text, line, mesh) or tint (image, cel; white = unchanged): a name, #rrggbb, #rrggbbaa", false},
        {"stroke", ParamType::String, "outline colour for rect/ellipse/polygon (needs stroke_width)", false},
        {"points", ParamType::NumberList, "polygon/line: x1,y1,x2,y2,... in px relative to the element's x,y", false},
        {"text", ParamType::String, "text element: the words (\\n for a new line)", false},
        {"font", ParamType::String, "text font", false, kFonts},
        {"bold", ParamType::Bool, "bold text", false},
        {"italic", ParamType::Bool, "italic text", false},
        {"source", ParamType::String, "image: the picture file. mesh: a primitive (box, sphere, cylinder, cone, torus, plane) or a mesh file (STL, OBJ, PLY, OFF, glTF, 3MF)", false},
        {"size", ParamType::NumberList, "mesh primitive size: box x,y,z; sphere r; cylinder r,h; cone r1,r2,h; torus major,minor; plane w,d", false},
    };
}

const std::vector<std::string>& propNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const auto& s : propSpecs()) v.push_back(s.name);
        return v;
    }();
    return names;
}

bool isNumberParam(const std::string& name) {
    for (const auto& s : propSpecs()) {
        if (s.name == name) return s.type == ParamType::Number;
    }
    return false;
}

Color parseColor(const std::string& text, const std::string& what) {
    auto c = Color::parse(text);
    if (!c) throw std::runtime_error("'" + text + "' isn't a colour for " + what + "; use a name like red, #rrggbb or #rrggbbaa");
    return *c;
}

Key keyFrom(const Params& p, int frame) {
    Key k;
    k.frame = frame;
    if (has(p, "ease")) k.ease = parseEase(stringParam(p, "ease")).value_or(Ease::Linear);
    if (has(p, "bezier")) {
        NumberList b = listParam(p, "bezier");
        if (b.size() != 4) throw std::runtime_error("bezier takes four numbers: x1,y1,x2,y2");
        for (std::size_t i = 0; i < 4; ++i) k.bezier[i] = b[i];
        k.ease = Ease::Bezier;
    }
    return k;
}

void checkFrame(const AnimDoc& doc, std::int64_t frame) {
    if (frame < 0 || frame >= doc.settings.frames) {
        throw std::runtime_error("frame " + std::to_string(frame) + " is outside the animation (frames 0.." +
                                 std::to_string(doc.settings.frames - 1) + "; animate.settings frames=N makes it longer)");
    }
}

// Sets a key. A property's first key, when it isn't at frame 0, also gets a key at frame 0 holding the
// current base value, so "x=300 at frame 24" means "move there by frame 24". Returns true when it did.
bool addKey(Element& e, const std::string& prop, const Key& k) {
    bool base = false;
    auto it = e.keys.find(prop);
    if ((it == e.keys.end() || it->second.empty()) && k.frame != 0) {
        Key b;
        b.frame = 0;
        if (isColorProp(prop)) b.color = colorAt(e, prop, 0);
        else b.number = numberAt(e, prop, 0);
        setKey(e, prop, b);
        base = true;
    }
    setKey(e, prop, k);
    return base;
}

bool wouldCycle(const AnimDoc& doc, const Element& child, const Element* parent) {
    for (int depth = 0; parent && depth < 64; ++depth) {
        if (parent->id == child.id) return true;
        if (parent->parent.empty()) return false;
        parent = doc.element(parent->parent);
    }
    return parent != nullptr;
}

std::string propList(const std::string& kind) {
    std::vector<std::string> all = numberProps(kind);
    for (const auto& c : colorProps(kind)) all.push_back(c);
    return join(all);
}

// Applies whichever element properties are present. With `keyFrame`, number and colour
// properties become keys at that frame instead of base values.
void applyProps(AnimDoc& doc, Element& e, const Params& p, std::optional<int> keyFrame) {
    const auto& nums = numberProps(e.kind);
    const auto& cols = colorProps(e.kind);
    bool reload = false;
    for (const auto& name : propNames()) {
        if (!has(p, name)) continue;
        if (isNumberParam(name) || isColorProp(name)) {
            const bool color = isColorProp(name);
            if ((color && !contains(cols, name)) || (!color && !contains(nums, name))) {
                throw std::runtime_error(name + " isn't a property of " + e.kind + " elements (they have: " + propList(e.kind) + ")");
            }
            if (keyFrame) {
                Key k = keyFrom(p, *keyFrame);
                if (color) k.color = parseColor(stringParam(p, name), name);
                else k.number = numberParam(p, name);
                addKey(e, name, k);
                continue;
            }
            auto keyed = e.keys.find(name);
            if (keyed != e.keys.end() && !keyed->second.empty()) {
                throw std::runtime_error(name + " is keyframed (" + std::to_string(keyed->second.size()) +
                                         " keys), so a base value wouldn't show; pass frame=N to set a key there, or "
                                         "animate.key_delete element=" + e.id + " property=" + name + " all=true first");
            }
            if (color) e.colors[name] = parseColor(stringParam(p, name), name);
            else e.numbers[name] = numberParam(p, name);
            continue;
        }
        if (name == "name") {
            e.name = stringParam(p, name);
        } else if (name == "visible") {
            e.visible = boolParam(p, name);
        } else if (name == "parent") {
            const std::string id = stringParam(p, name);
            if (id.empty() || lower(id) == "none") {
                e.parent.clear();
                continue;
            }
            const Element* par = doc.element(id);
            if (!par) throw std::runtime_error("no element called '" + id + "' to parent to");
            if (is3dKind(par->kind) != is3dKind(e.kind)) throw std::runtime_error("2D elements parent to 2D elements and 3D to 3D");
            if (par->kind == "camera") throw std::runtime_error("elements can't be parented to a camera");
            if (wouldCycle(doc, e, par)) throw std::runtime_error("that would make a parent loop (" + par->id + " already follows " + e.id + ")");
            e.parent = par->id;
        } else if (name == "points") {
            if (e.kind != "polygon" && e.kind != "line") throw std::runtime_error("points only apply to polygon and line elements");
            NumberList pts = listParam(p, name);
            if (pts.size() % 2) throw std::runtime_error("points are x,y pairs, so they need an even count of numbers");
            if (e.kind == "polygon" && pts.size() < 6) throw std::runtime_error("a polygon needs at least 3 points (6 numbers)");
            if (e.kind == "line" && pts.size() < 4) throw std::runtime_error("a line needs at least 2 points (4 numbers)");
            e.points = pts;
        } else if (name == "text" || name == "font" || name == "bold" || name == "italic") {
            if (e.kind != "text") throw std::runtime_error(name + " only applies to text elements");
            if (name == "text") e.text = stringParam(p, name);
            if (name == "font") e.font = stringParam(p, name);
            if (name == "bold") e.bold = boolParam(p, name);
            if (name == "italic") e.italic = boolParam(p, name);
        } else if (name == "source") {
            if (e.kind != "image" && e.kind != "mesh") throw std::runtime_error("source only applies to image and mesh elements");
            std::string src = stringParam(p, name);
            if (!(e.kind == "mesh" && contains(primitiveNames(), lower(src)))) {
                std::error_code ec;
                fs::path a = fs::absolute(src, ec);
                if (!ec) src = a.lexically_normal().string();
            } else {
                src = lower(src);
            }
            e.source = src;
            reload = true;
        } else if (name == "size") {
            if (e.kind != "mesh") throw std::runtime_error("size only applies to mesh primitives (use scale or width/height for 2D)");
            e.size = listParam(p, name);
            reload = true;
        }
    }
    if (reload) loadSource(e);
}

std::string keySummary(const Element& e) {
    std::string s;
    for (const auto& [prop, keys] : e.keys) {
        if (keys.empty()) continue;
        std::string frames;
        for (const auto& k : keys) frames += (frames.empty() ? "" : ",") + std::to_string(k.frame);
        s += (s.empty() ? "" : " ") + prop + "@" + frames;
    }
    return s;
}

std::string describe(const AnimDoc& doc, const Element& e) {
    const Element d = makeElement(e.kind, doc.settings);
    std::string s = e.id + " " + e.kind;
    if (!e.name.empty()) s += " \"" + e.name + "\"";
    if (!e.visible) s += " (hidden)";
    if (!e.parent.empty()) s += " parent=" + e.parent;
    if (e.kind == "text") s += " text=\"" + (e.text.size() > 40 ? e.text.substr(0, 37) + "..." : e.text) + "\"";
    if (e.kind == "image" || e.kind == "mesh") s += " source=" + (contains(primitiveNames(), e.source) ? e.source : fs::path(e.source).filename().string());
    if (e.kind == "image" && e.picture) s += " (" + std::to_string(e.picture->width()) + "x" + std::to_string(e.picture->height()) + ")";
    if (!e.size.empty()) {
        std::string sz;
        for (double v : e.size) sz += (sz.empty() ? "" : ",") + num(v);
        s += " size=" + sz;
    }
    for (const auto& prop : numberProps(e.kind)) {
        if (e.keys.count(prop) && !e.keys.at(prop).empty()) continue;
        const double v = e.numbers.count(prop) ? e.numbers.at(prop) : 0;
        const double dv = d.numbers.count(prop) ? d.numbers.at(prop) : 0;
        if (std::fabs(v - dv) > 1e-9 || prop == "x" || prop == "y") s += " " + prop + "=" + num(v);
    }
    for (const auto& prop : colorProps(e.kind)) {
        if (e.keys.count(prop) && !e.keys.at(prop).empty()) continue;
        if (prop == "stroke" && !(e.numbers.count("stroke_width") && e.numbers.at("stroke_width") > 0) && !e.keys.count("stroke_width")) continue;
        if (e.colors.count(prop)) s += " " + prop + "=" + e.colors.at(prop).hex();
    }
    if (!e.points.empty()) s += " points=" + std::to_string(e.points.size() / 2);
    if (e.kind == "cel") {
        std::string fr;
        for (const auto& [f, img] : e.cels) fr += (fr.empty() ? "" : ",") + std::to_string(f);
        s += " drawings=" + (fr.empty() ? std::string("none") : fr);
    }
    const std::string keys = keySummary(e);
    if (!keys.empty()) s += " keys: " + keys;
    return s;
}

// ---- previews ----

std::string framePng(const AnimDoc& doc, int frame, int maxSize, int onion, bool bones) {
    FrameOptions o;
    o.scale = std::min(1.0, static_cast<double>(maxSize) / std::max(doc.settings.width, doc.settings.height));
    o.onion = onion;
    o.bones = bones;
    return raster::encodePng(renderFrame(doc, frame, o));
}

// Several frames side by side with their numbers, so motion shows in one picture.
Image contactStrip(const AnimDoc& doc, const std::vector<int>& frames, int cell, int onion, bool bones) {
    const int cols = std::min<int>(static_cast<int>(frames.size()), frames.size() <= 4 ? 4 : frames.size() <= 9 ? 3 : 4);
    const int rows = (static_cast<int>(frames.size()) + cols - 1) / cols;
    FrameOptions o;
    o.scale = std::min(1.0, static_cast<double>(cell) / std::max(doc.settings.width, doc.settings.height));
    o.onion = onion;
    o.bones = bones;
    const int fw = std::max(1, static_cast<int>(std::lround(doc.settings.width * o.scale)));
    const int fh = std::max(1, static_cast<int>(std::lround(doc.settings.height * o.scale)));
    const int gap = 6, label = 18;
    Image sheet(cols * fw + (cols + 1) * gap, rows * (fh + label) + (rows + 1) * gap, Color{40, 44, 52, 255});
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const int cx = gap + static_cast<int>(i % static_cast<std::size_t>(cols)) * (fw + gap);
        const int cy = gap + static_cast<int>(i / static_cast<std::size_t>(cols)) * (fh + label + gap);
        Image f = renderFrame(doc, frames[i], o);
        raster::blendOnto(sheet, f, cx, cy + label, raster::Blend::Normal, 1.0);
        pdf::drawText(sheet, cx + 2, cy + 13, "frame " + std::to_string(frames[i]), pdf::Font{}, 12, Color{230, 232, 236, 255});
    }
    return sheet;
}

Result& maybePreview(Result& r, const AnimDoc& doc, const Params& p, int frame = -1) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    if (frame < 0) frame = static_cast<int>(intParam(p, "frame", 0));
    frame = std::clamp(frame, 0, doc.settings.frames - 1);
    r.attach({"image/png", framePng(doc, frame, 640, static_cast<int>(intParam(p, "onion", 0)), boolParam(p, "bones", false)),
              "frame" + std::to_string(frame) + ".png"});
    return r;
}

using Edit = std::function<Result(AnimDoc&, const Params&)>;

ActionFactory animCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<AnimDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open animation called '" + id + "' (use animate.new or animate.open first)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<AnimEdit>(doc, [edit, p](AnimDoc& d) { return edit(d, p); });
    };
}

Element& requireElement(AnimDoc& doc, const Params& p, Layer** owner = nullptr) {
    const std::string id = stringParam(p, "element");
    Element* e = doc.element(id, owner);
    if (!e) throw std::runtime_error("no element called '" + id + "' (animate.info lists them)");
    return *e;
}

Layer& requireLayer(AnimDoc& doc, const std::string& id) {
    Layer* l = doc.layer(id);
    if (!l) throw std::runtime_error("no layer called '" + id + "' (animate.info lists them)");
    return *l;
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

Result docCreated(Session& session, std::shared_ptr<AnimDoc> doc, const std::string& verb) {
    std::string id = session.workspace().add(doc, "anim");
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")", id);
    r.with("doc", id)
        .with("width", i64(doc->settings.width))
        .with("height", i64(doc->settings.height))
        .with("fps", doc->settings.fps)
        .with("frames", i64(doc->settings.frames))
        .with("layers", i64(doc->layers.size()));
    if (!doc->layers.empty()) r.with("layer", doc->layers.back().id);
    return r;
}

std::string settingsError(const Settings& s) {
    if (s.width < 1 || s.height < 1 || s.width > 8192 || s.height > 8192) return "width and height must be 1..8192";
    if (!(s.fps > 0 && s.fps <= 240)) return "fps must be more than 0 and at most 240";
    if (s.frames < 1 || s.frames > 100000) return "frames must be 1..100000";
    return {};
}

Layer& addLayer(AnimDoc& doc, const std::string& kind, const std::string& name) {
    Layer l;
    l.id = "L" + std::to_string(doc.nextLayer++);
    l.kind = kind;
    l.name = name.empty() ? (kind == "3d" ? "3D " : "Layer ") + std::to_string(doc.nextLayer - 1) : name;
    doc.layers.push_back(std::move(l));
    return doc.layers.back();
}

void unparentChildren(AnimDoc& doc, const std::set<std::string>& gone) {
    for (auto& l : doc.layers) {
        for (auto& e : l.elements) {
            if (gone.count(e.parent)) e.parent.clear();
        }
    }
}

// ---- export helpers ----

struct Range {
    int start = 0, end = 0, step = 1;
    std::vector<int> frames() const {
        std::vector<int> v;
        for (int f = start; f <= end; f += step) v.push_back(f);
        return v;
    }
};

std::vector<ParamSpec> rangeParams() {
    return {{"start", ParamType::Int, "first frame (0-based)", false, {}, std::int64_t{0}},
            {"end", ParamType::Int, "last frame, included (default: the last frame)", false},
            {"step", ParamType::Int, "use every Nth frame (2 halves the frame count)", false, {}, std::int64_t{1}},
            {"scale", ParamType::Number, "output size as a fraction of the canvas (0.5 = half size)", false, {}, 1.0}};
}

Range rangeOf(const AnimDoc& doc, const Params& p) {
    Range r;
    r.start = static_cast<int>(intParam(p, "start", 0));
    r.end = static_cast<int>(intParam(p, "end", doc.settings.frames - 1));
    r.step = static_cast<int>(intParam(p, "step", 1));
    if (r.step < 1) throw std::runtime_error("step must be 1 or more");
    checkFrame(doc, r.start);
    checkFrame(doc, r.end);
    if (r.end < r.start) throw std::runtime_error("end comes before start");
    return r;
}

FrameOptions exportOptions(const Params& p) {
    FrameOptions o;
    o.scale = numberParam(p, "scale", 1.0);
    if (!(o.scale > 0 && o.scale <= 8)) throw std::runtime_error("scale must be more than 0 and at most 8");
    return o;
}

std::string checkOutput(const fs::path& output, const Params& p) {
    std::error_code ec;
    if (fs::exists(output, ec) && !boolParam(p, "overwrite", true)) {
        return output.string() + " already exists (pass overwrite=true to replace it)";
    }
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), ec);
    return {};
}

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

std::int64_t fileBytes(const fs::path& p) {
    std::error_code ec;
    auto n = fs::file_size(p, ec);
    return ec ? 0 : static_cast<std::int64_t>(n);
}

std::string frameName(const std::string& prefix, int frame) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d", frame);
    return prefix + buf + ".png";
}

std::vector<std::string> imagesIn(const fs::path& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && raster::canRead(entry.path())) files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

void registerDocuments(Session& session) {
    auto& reg = session.registry();

    reg.add({"animate.new", "New Animation",
             "Create an animation and return its id (e.g. anim1), with one empty layer (L1). Add things with "
             "animate.element_add, move them over time with animate.key_set (or element_set with frame=N), look with "
             "animate.preview, then export (PNG sequence, sprite sheet, GIF, mp4/webm). Frames count from 0.",
             {{"width", ParamType::Int, "canvas width in px", false, {}, std::int64_t{640}},
              {"height", ParamType::Int, "canvas height in px", false, {}, std::int64_t{360}},
              {"fps", ParamType::Number, "frames per second", false, {}, 24.0},
              {"frames", ParamType::Int, "length in frames (fps x seconds)", false, {}, std::int64_t{48}},
              {"background", ParamType::String, "canvas colour; transparent for sprites", false, {}, std::string("white")},
              {"layer", ParamType::String, "the first layer: 2d (shapes, text, images, cels, bones), 3d (meshes and a camera) or none",
               false, {"2d", "3d", "none"}, std::string("2d")}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<AnimDoc>();
                    auto& s = doc->settings;
                    s.width = static_cast<int>(intParam(p, "width", 640));
                    s.height = static_cast<int>(intParam(p, "height", 360));
                    s.fps = numberParam(p, "fps", 24);
                    s.frames = static_cast<int>(intParam(p, "frames", 48));
                    auto bg = Color::parse(stringParam(p, "background", "white"));
                    if (!bg) return Result::failure("'" + stringParam(p, "background") + "' isn't a colour");
                    s.background = *bg;
                    if (std::string err = settingsError(s); !err.empty()) return Result::failure(err);
                    const std::string layer = stringParam(p, "layer", "2d");
                    if (layer != "none") addLayer(*doc, layer, "");
                    return docCreated(session, doc, "created");
                });
            });

    reg.add({"animate.open", "Open Animation", "Open an .anim.json animation saved by animate.save. Returns the document id.",
             {{"path", ParamType::String, "path to an .anim.json file", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const fs::path path = stringParam(p, "path");
                    std::error_code ec;
                    if (!fs::is_regular_file(path, ec)) return Result::failure("file not found: " + path.string());
                    try {
                        auto doc = fromJson(detail::readFile(path), fs::absolute(path, ec).parent_path());
                        doc->path = path.string();
                        return docCreated(session, doc, "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"animate.save", "Save Animation",
             "Save as an .anim.json file (JSON: canvas, layers, elements, keys; cel drawings embedded as PNG). Images and "
             "mesh files are referred to by path, not copied.",
             {docParam(), {"path", ParamType::String, "where to save, e.g. walk.anim.json", true}}, false},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                fs::path path = stringParam(p, "path");
                if (extOf(path) != "json") return Result::failure("animations save as .anim.json; to make a video or GIF, use animate.export_*");
                std::error_code ec;
                if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
                if (!detail::writeFile(path, toJson(doc))) return Result::failure("can't write " + path.string());
                doc.path = path.string();
                Result r = Result::success("saved " + path.string(), path.string());
                return r.with("path", path.string()).with("bytes", fileBytes(path));
            }, false));

    reg.add({"animate.close", "Close Animation", "Close an animation (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<AnimDoc>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open animation called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"animate.info", "Animation Info",
             "Describe an animation: canvas, fps, length, and every layer with its elements (id, kind, name, parent, "
             "changed properties, which properties are keyframed at which frames, cel drawings).",
             {docParam()}, false, true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                const auto& s = doc.settings;
                std::string text = stringParam(p, "doc") + ": " + std::to_string(s.width) + "x" + std::to_string(s.height) + " " +
                                   num(s.fps) + "fps, frames 0.." + std::to_string(s.frames - 1) + " (" + num(s.frames / s.fps) +
                                   "s), background " + s.background.hex();
                if (doc.layers.empty()) text += "\nno layers yet (animate.layer_add)";
                std::int64_t keys = 0;
                for (auto it = doc.layers.rbegin(); it != doc.layers.rend(); ++it) {
                    const auto& l = *it;
                    text += "\n" + l.id + " \"" + l.name + "\" (" + l.kind + (l.visible ? "" : ", hidden") +
                            (l.opacity < 1 ? ", opacity " + num(l.opacity) : "") + (l.blend != "normal" ? ", " + l.blend : "") + ")";
                    if (l.elements.empty()) text += ": empty";
                    for (auto e = l.elements.rbegin(); e != l.elements.rend(); ++e) {
                        text += "\n  " + describe(doc, *e);
                        for (const auto& [prop, list] : e->keys) keys += i64(list.size());
                    }
                }
                text += "\n(layers and elements are listed top first)";
                Result r = Result::success(text);
                return r.with("doc", stringParam(p, "doc"))
                    .with("width", i64(s.width))
                    .with("height", i64(s.height))
                    .with("fps", s.fps)
                    .with("frames", i64(s.frames))
                    .with("duration", s.frames / s.fps)
                    .with("layers", i64(doc.layers.size()))
                    .with("elements", i64(doc.elementCount()))
                    .with("keys", keys);
            }, false));

    reg.add({"animate.settings", "Animation Settings",
             "Change the canvas size, frame rate, length in frames or background. Keys keep their frame numbers.",
             {docParam(),
              {"width", ParamType::Int, "canvas width in px", false},
              {"height", ParamType::Int, "canvas height in px", false},
              {"fps", ParamType::Number, "frames per second", false},
              {"frames", ParamType::Int, "length in frames", false},
              {"background", ParamType::String, "canvas colour (transparent for sprites)", false},
              previewParam()},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                auto& s = doc.settings;
                if (has(p, "width")) s.width = static_cast<int>(intParam(p, "width"));
                if (has(p, "height")) s.height = static_cast<int>(intParam(p, "height"));
                if (has(p, "fps")) s.fps = numberParam(p, "fps");
                if (has(p, "frames")) s.frames = static_cast<int>(intParam(p, "frames"));
                if (has(p, "background")) s.background = parseColor(stringParam(p, "background"), "background");
                if (std::string err = settingsError(s); !err.empty()) return Result::failure(err);
                Result r = Result::success("animation is now " + doc.summary());
                r.with("frames", i64(s.frames)).with("fps", s.fps);
                return maybePreview(r, doc, p, 0);
            }));

}

void registerLayers(Session& session) {
    auto& reg = session.registry();

    reg.add({"animate.layer_add", "Add Layer",
             "Add a layer on top. 2d layers hold rect, ellipse, polygon, line, text, image, cel and bone elements; 3d "
             "layers hold meshes and a camera (without a camera the view frames every mesh).",
             {docParam(),
              {"kind", ParamType::String, "2d or 3d", false, {"2d", "3d"}, std::string("2d")},
              {"name", ParamType::String, "layer name (usable instead of its id)", false},
              {"opacity", ParamType::Number, "0..1", false, {}, 1.0},
              {"blend", ParamType::String, "how the layer mixes with the layers under it", false, kBlends, std::string("normal")}},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Layer& l = addLayer(doc, stringParam(p, "kind", "2d"), stringParam(p, "name"));
                l.opacity = numberParam(p, "opacity", 1);
                l.blend = stringParam(p, "blend", "normal");
                Result r = Result::success("added " + l.kind + " layer " + l.id + " \"" + l.name + "\"", l.id);
                return r.with("layer", l.id);
            }));

    reg.add({"animate.layer_remove", "Remove Layer", "Delete a layer and everything on it (elements parented to them are detached).",
             {docParam(), {"layer", ParamType::String, "layer id (L1) or name", true}}, true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Layer& l = requireLayer(doc, stringParam(p, "layer"));
                const std::string id = l.id;
                std::set<std::string> gone;
                for (const auto& e : l.elements) gone.insert(e.id);
                doc.layers.erase(std::find_if(doc.layers.begin(), doc.layers.end(), [&](const Layer& x) { return x.id == id; }));
                unparentChildren(doc, gone);
                Result r = Result::success("removed layer " + id + " (" + std::to_string(gone.size()) + " elements)");
                return r.with("layer", id);
            }));

    reg.add({"animate.layer_set", "Set Layer", "Rename, hide/show, fade, change the blend mode of, or restack a layer.",
             {docParam(),
              {"layer", ParamType::String, "layer id (L1) or name", true},
              {"name", ParamType::String, "new name", false},
              {"visible", ParamType::Bool, "show or hide", false},
              {"opacity", ParamType::Number, "0..1", false},
              {"blend", ParamType::String, "blend mode", false, kBlends},
              {"index", ParamType::Int, "new place in the stack: 0 = bottom, -1 = top", false},
              {"frame", ParamType::Int, "frame for the preview", false},
              previewParam()},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Layer& l = requireLayer(doc, stringParam(p, "layer"));
                if (has(p, "name")) l.name = stringParam(p, "name");
                if (has(p, "visible")) l.visible = boolParam(p, "visible");
                if (has(p, "opacity")) l.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
                if (has(p, "blend")) l.blend = stringParam(p, "blend");
                const std::string id = l.id;
                if (has(p, "index")) {
                    auto n = static_cast<std::int64_t>(doc.layers.size());
                    std::int64_t to = intParam(p, "index");
                    if (to < 0) to += n;
                    to = std::clamp<std::int64_t>(to, 0, n - 1);
                    auto it = std::find_if(doc.layers.begin(), doc.layers.end(), [&](const Layer& x) { return x.id == id; });
                    Layer moved = std::move(*it);
                    doc.layers.erase(it);
                    doc.layers.insert(doc.layers.begin() + to, std::move(moved));
                }
                const Layer& now = *doc.layer(id);
                Result r = Result::success("layer " + id + " \"" + now.name + "\"" + (now.visible ? "" : " hidden") + ", opacity " +
                                           num(now.opacity) + ", " + now.blend);
                r.with("layer", id);
                return maybePreview(r, doc, p);
            }));

}

void registerElements(Session& session) {
    auto& reg = session.registry();

    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"kind", ParamType::String,
                                          "rect, ellipse, polygon (points), line (points), text, image (source), cel (frame-by-frame "
                                          "drawing), bone (for rigs) on 2d layers; mesh (source) or camera on 3d layers", true,
                                          {"rect", "ellipse", "polygon", "line", "text", "image", "cel", "bone", "mesh", "camera"}},
                                         {"layer", ParamType::String, "layer id or name (default: the top layer of the right kind, made if missing)", false}};
        for (const auto& s : propSpecs()) params.push_back(s);
        params.push_back({"frame", ParamType::Int, "frame for the preview", false});
        params.push_back(previewParam());
        reg.add({"animate.element_add", "Add Element",
                 "Add an element on top of a layer and return its id (e1, e2, ...). Properties not given take defaults: "
                 "100x100 boxes, anchor in the centre, x=y=0 (top-left of the canvas; cels fill the canvas). Every number "
                 "and colour property can be keyframed afterwards.",
                 params, true},
                animCommand(session, [](AnimDoc& doc, const Params& p) {
                    const std::string kind = stringParam(p, "kind");
                    const std::string lk = is3dKind(kind) ? "3d" : "2d";
                    Layer* layer = nullptr;
                    if (has(p, "layer")) {
                        layer = &requireLayer(doc, stringParam(p, "layer"));
                        if (layer->kind != lk) {
                            throw std::runtime_error("layer " + layer->id + " is a " + layer->kind + " layer; a " + kind + " goes on a " + lk + " layer");
                        }
                    } else {
                        for (auto it = doc.layers.rbegin(); it != doc.layers.rend() && !layer; ++it) {
                            if (it->kind == lk) layer = &*it;
                        }
                        if (!layer) layer = &addLayer(doc, lk, "");
                    }
                    const std::string layerId = layer->id;
                    Element e = makeElement(kind, doc.settings);
                    e.id = "e" + std::to_string(doc.nextElement++);
                    if (kind == "text" && !has(p, "text")) throw std::runtime_error("a text element needs text=...");
                    if (kind == "image" && !has(p, "source")) throw std::runtime_error("an image element needs source=<image file>");
                    if ((kind == "polygon" || kind == "line") && !has(p, "points")) throw std::runtime_error("a " + kind + " needs points=x1,y1,x2,y2,...");
                    applyProps(doc, e, p, std::nullopt);
                    if (kind == "mesh" && !e.geometry) loadSource(e);
                    const std::string id = e.id;
                    doc.layer(layerId)->elements.push_back(std::move(e));
                    Result r = Result::success("added " + kind + " " + id + " on " + layerId, id);
                    r.with("element", id).with("layer", layerId);
                    return maybePreview(r, doc, p);
                }));
    }

    reg.add({"animate.element_remove", "Remove Element", "Delete an element (its children are detached and stay where their own values put them).",
             {docParam(), elementParam()}, true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Layer* owner = nullptr;
                Element& e = requireElement(doc, p, &owner);
                const std::string id = e.id;
                owner->elements.erase(std::find_if(owner->elements.begin(), owner->elements.end(), [&](const Element& x) { return x.id == id; }));
                unparentChildren(doc, {id});
                Result r = Result::success("removed " + id);
                return r.with("element", id);
            }));

    {
        std::vector<ParamSpec> params = {docParam(), elementParam(),
                                         {"frame", ParamType::Int, "set keys at this frame instead of base values (and preview it)", false},
                                         easeParam(), bezierParam(),
                                         {"order", ParamType::String, "restack within the layer", false, {"front", "back", "forward", "backward"}},
                                         {"layer", ParamType::String, "move to another layer of the same kind", false}};
        for (const auto& s : propSpecs()) params.push_back(s);
        params.push_back(previewParam());
        reg.add({"animate.element_set", "Set Element",
                 "Change an element. Without frame, number/colour properties set the base value (for properties with no keys). "
                 "With frame=N they become keys at frame N, so `x=300 frame=24` moves it there by frame 24 (a first key "
                 "also keys frame 0 with the old value); ease shapes the motion into that key. Also renames, re-parents (rigs), restacks and moves layers.",
                 params, true},
                animCommand(session, [](AnimDoc& doc, const Params& p) {
                    Layer* owner = nullptr;
                    Element& e = requireElement(doc, p, &owner);
                    std::optional<int> frame;
                    if (has(p, "frame")) {
                        checkFrame(doc, intParam(p, "frame"));
                        frame = static_cast<int>(intParam(p, "frame"));
                    }
                    applyProps(doc, e, p, frame);
                    const std::string id = e.id;
                    if (has(p, "layer")) {
                        Layer& to = requireLayer(doc, stringParam(p, "layer"));
                        if (to.kind != owner->kind) throw std::runtime_error("an element can only move to another " + owner->kind + " layer");
                        if (to.id != owner->id) {
                            Element moved = e;
                            owner->elements.erase(std::find_if(owner->elements.begin(), owner->elements.end(), [&](const Element& x) { return x.id == id; }));
                            to.elements.push_back(std::move(moved));
                            owner = &to;
                        }
                    }
                    if (has(p, "order")) {
                        auto& els = owner->elements;
                        auto it = std::find_if(els.begin(), els.end(), [&](const Element& x) { return x.id == id; });
                        const std::string o = stringParam(p, "order");
                        auto idx = it - els.begin();
                        auto to = o == "front" ? static_cast<std::ptrdiff_t>(els.size()) - 1 : o == "back" ? 0 : o == "forward" ? idx + 1 : idx - 1;
                        to = std::clamp<std::ptrdiff_t>(to, 0, static_cast<std::ptrdiff_t>(els.size()) - 1);
                        Element moved = std::move(*it);
                        els.erase(it);
                        els.insert(els.begin() + to, std::move(moved));
                    }
                    const Element* now = doc.element(id);
                    Result r = Result::success((frame ? "keyed at frame " + std::to_string(*frame) + ": " : std::string("set: ")) + describe(doc, *now));
                    r.with("element", id).with("layer", owner->id);
                    return maybePreview(r, doc, p);
                }));
    }

}

void registerKeys(Session& session) {
    auto& reg = session.registry();

    reg.add({"animate.key_set", "Set Key",
             "Set one keyframe: property `property` of `element` is `value` at `frame`. Between keys the value moves "
             "along the ease of the later key; before the first key and after the last it holds. A property's first key "
             "at frame N > 0 also keys frame 0 with the value it had, so one call makes a motion.",
             {docParam(), elementParam(),
              {"property", ParamType::String, "x, y, rotation, scale, opacity, color, ... (animate.key_list shows an element's)", true},
              {"frame", ParamType::Int, "frame number, from 0", true},
              {"value", ParamType::String, "a number, or a colour for color/stroke", true},
              easeParam(), bezierParam(), previewParam()},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Element& e = requireElement(doc, p);
                const std::string prop = stringParam(p, "property");
                const int frame = static_cast<int>(intParam(p, "frame"));
                checkFrame(doc, frame);
                const bool color = isColorProp(prop);
                if ((color && !contains(colorProps(e.kind), prop)) || (!color && !contains(numberProps(e.kind), prop))) {
                    throw std::runtime_error(prop + " isn't a property of " + e.kind + " elements (they have: " + propList(e.kind) + ")");
                }
                Key k = keyFrom(p, frame);
                const std::string value = stringParam(p, "value");
                if (color) {
                    k.color = parseColor(value, prop);
                } else {
                    char* end = nullptr;
                    k.number = std::strtod(value.c_str(), &end);
                    if (value.empty() || (end && *end != '\0')) throw std::runtime_error(prop + " takes a number, not '" + value + "'");
                }
                const bool base = addKey(e, prop, k);
                Result r = Result::success(e.id + " " + prop + " = " + (color ? k.color.hex() : num(k.number)) + " at frame " +
                                           std::to_string(frame) + " (" + toString(k.ease) + "); " + prop + " now has " +
                                           std::to_string(e.keys[prop].size()) + " keys" +
                                           (base ? " (its first key, so frame 0 got a key with the value it had)" : ""));
                r.with("element", e.id).with("property", prop).with("frame", i64(frame))
                    .with("keys", i64(e.keys[prop].size()));
                return maybePreview(r, doc, p, frame);
            }));

    reg.add({"animate.key_delete", "Delete Key",
             "Remove the key at `frame` (from one property, or every property of the element), or all of a property's "
             "keys with all=true. With no keys left a property goes back to its base value.",
             {docParam(), elementParam(),
              {"property", ParamType::String, "property to delete from (default: every property)", false},
              {"frame", ParamType::Int, "frame of the key to remove", false},
              {"all", ParamType::Bool, "remove every key (of the property, or of the whole element)", false, {}, false}},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Element& e = requireElement(doc, p);
                const bool all = boolParam(p, "all", false);
                if (!all && !has(p, "frame")) throw std::runtime_error("say which key: frame=N, or all=true");
                const std::string prop = stringParam(p, "property");
                if (!prop.empty() && !e.keys.count(prop)) throw std::runtime_error(e.id + " has no keys on " + prop);
                std::int64_t removed = 0;
                for (auto it = e.keys.begin(); it != e.keys.end();) {
                    if (!prop.empty() && it->first != prop) {
                        ++it;
                        continue;
                    }
                    auto& keys = it->second;
                    const auto before = keys.size();
                    if (all) keys.clear();
                    else keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Key& k) { return k.frame == intParam(p, "frame"); }), keys.end());
                    removed += static_cast<std::int64_t>(before - keys.size());
                    if (keys.empty()) {
                        // keep the value the property last had as its base, so deleting keys doesn't make things jump
                        it = e.keys.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (removed == 0) throw std::runtime_error("no key at frame " + std::to_string(intParam(p, "frame")) + (prop.empty() ? "" : " on " + prop));
                Result r = Result::success("removed " + std::to_string(removed) + " key" + (removed == 1 ? "" : "s") + " from " + e.id);
                return r.with("element", e.id).with("removed", removed);
            }));

    reg.add({"animate.key_list", "List Keys",
             "List keyframes: frame, value and ease for every keyed property of one element (or every element).",
             {docParam(), {"element", ParamType::String, "element id or name (default: all)", false},
              {"property", ParamType::String, "just this property", false}},
             false, true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                std::string text;
                std::int64_t count = 0;
                const std::string only = stringParam(p, "property");
                auto list = [&](const Element& e) {
                    for (const auto& [prop, keys] : e.keys) {
                        if (keys.empty() || (!only.empty() && prop != only)) continue;
                        text += (text.empty() ? "" : "\n") + e.id + (e.name.empty() ? "" : " (" + e.name + ")") + " " + prop + ":";
                        for (const auto& k : keys) {
                            text += " " + std::to_string(k.frame) + "=" + (isColorProp(prop) ? k.color.hex() : num(k.number));
                            if (k.ease != Ease::Linear) {
                                text += " " + toString(k.ease);
                                if (k.ease == Ease::Bezier) {
                                    text += "(" + num(k.bezier[0]) + "," + num(k.bezier[1]) + "," + num(k.bezier[2]) + "," + num(k.bezier[3]) + ")";
                                }
                            }
                            ++count;
                        }
                        text += "";
                    }
                };
                if (has(p, "element")) {
                    const Element& e = requireElement(doc, p);
                    list(e);
                    if (text.empty()) text = e.id + " has no keys" + (only.empty() ? "" : " on " + only) + "; its properties: " + propList(e.kind);
                } else {
                    for (const auto& l : doc.layers) {
                        for (const auto& e : l.elements) list(e);
                    }
                    if (text.empty()) text = "no keys yet";
                }
                Result r = Result::success(text);
                return r.with("keys", count);
            }, false));

    reg.add({"animate.values", "Values At Frame",
             "Evaluate an element (or every element) at a frame: each property's value after easing, and where its "
             "anchor lands on the canvas (2D) or in the world (3D) after parenting. Use it to check a motion without "
             "looking.",
             {docParam(), {"element", ParamType::String, "element id or name (default: all)", false},
              {"frame", ParamType::Int, "frame number", true}},
             false, true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                const int frame = static_cast<int>(intParam(p, "frame"));
                checkFrame(doc, frame);
                std::string text = "frame " + std::to_string(frame) + " (" + num(frame / doc.settings.fps) + "s)";
                Result r;
                auto one = [&](const Element& e, bool fields) {
                    text += "\n" + e.id + (e.name.empty() ? "" : " (" + e.name + ")") + ":";
                    for (const auto& prop : numberProps(e.kind)) {
                        const double v = numberAt(e, prop, frame);
                        text += " " + prop + "=" + num(v);
                        if (fields) r.with(prop, v);
                    }
                    for (const auto& prop : colorProps(e.kind)) {
                        const std::string v = colorAt(e, prop, frame).hex();
                        text += " " + prop + "=" + v;
                        if (fields) r.with(prop, v);
                    }
                    if (is3dKind(e.kind)) {
                        const mesh::Vec3 w = worldTransform3d(doc, e, frame).point({0, 0, 0});
                        text += " world=(" + num(w.x) + ", " + num(w.y) + ", " + num(w.z) + ")";
                        if (fields) r.with("world_x", w.x).with("world_y", w.y).with("world_z", w.z);
                    } else {
                        double w = 0, h = 0;
                        elementSize(e, frame, w, h);
                        const double ax = e.numbers.count("anchor_x") ? numberAt(e, "anchor_x", frame) * w : 0;
                        const double ay = e.numbers.count("anchor_y") ? numberAt(e, "anchor_y", frame) * h : 0;
                        double cx = 0, cy = 0;
                        worldTransform2d(doc, e, frame).apply(ax, ay, cx, cy);
                        text += " canvas=(" + num(cx) + ", " + num(cy) + ")";
                        if (fields) r.with("canvas_x", cx).with("canvas_y", cy);
                        if (e.kind == "bone") {
                            double tx = 0, ty = 0;
                            worldTransform2d(doc, e, frame).apply(numberAt(e, "length", frame), 0, tx, ty);
                            text += " tip=(" + num(tx) + ", " + num(ty) + ")";
                            if (fields) r.with("tip_x", tx).with("tip_y", ty);
                        }
                    }
                };
                if (has(p, "element")) {
                    one(requireElement(doc, p), true);
                } else {
                    for (const auto& l : doc.layers) {
                        for (const auto& e : l.elements) one(e, false);
                    }
                }
                r.ok = true;
                r.message = text;
                return r.with("frame", i64(frame));
            }, false));

}

void registerCels(Session& session) {
    auto& reg = session.registry();

    reg.add({"animate.draw", "Draw On Cel",
             "Draw on a cel element's drawing at a frame: a brush stroke through points, a filled shape, or an eraser "
             "stroke. If the frame has no drawing yet a new blank one starts there (or a copy of the one showing, with "
             "from_previous=true). A drawing holds until the next drawing. Points are in the cel's own px (the canvas "
             "px for a default cel). Use preview=true onion=1 to see the neighbouring drawings.",
             {docParam(), elementParam(),
              {"frame", ParamType::Int, "frame to draw on", true},
              {"points", ParamType::NumberList, "x1,y1,x2,y2,...", true},
              {"pressure", ParamType::NumberList, "optional pressure 0..1 per point (thins the stroke)", false},
              {"mode", ParamType::String, "stroke (brush), fill (closed filled shape) or erase", false, {"stroke", "fill", "erase"}, std::string("stroke")},
              {"color", ParamType::String, "ink colour", false, {}, std::string("black")},
              {"size", ParamType::Number, "brush diameter in px", false, {}, 6.0},
              {"hardness", ParamType::Number, "0 soft .. 1 hard edge", false, {}, 0.9},
              {"opacity", ParamType::Number, "0..1", false, {}, 1.0},
              {"smooth", ParamType::Number, "smooth shaky points 0..0.9", false, {}, 0.0},
              {"from_previous", ParamType::Bool, "start a new drawing as a copy of the one showing (for small changes)", false, {}, false},
              {"onion", ParamType::Int, "preview: show this many drawings before (red) and after (green)", false, {}, std::int64_t{0}},
              previewParam()},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Element& e = requireElement(doc, p);
                if (e.kind != "cel") throw std::runtime_error(e.id + " is a " + e.kind + "; draw on a cel (animate.element_add kind=cel)");
                const int frame = static_cast<int>(intParam(p, "frame"));
                checkFrame(doc, frame);
                const int w = std::max(1, static_cast<int>(std::lround(e.numbers["width"])));
                const int h = std::max(1, static_cast<int>(std::lround(e.numbers["height"])));
                Image img(w, h, Color{0, 0, 0, 0});
                bool fresh = true;
                if (auto it = e.cels.find(frame); it != e.cels.end() && it->second) {
                    img = *it->second;
                    fresh = false;
                } else if (boolParam(p, "from_previous", false)) {
                    if (auto prev = celAt(e, frame)) img = *prev;
                }
                if (img.width() != w || img.height() != h) img = raster::resize(img, w, h);
                NumberList pts = listParam(p, "points");
                NumberList pressure = listParam(p, "pressure");
                if (pts.size() < 2 || pts.size() % 2) throw std::runtime_error("points are x,y pairs");
                const Color c = parseColor(stringParam(p, "color", "black"), "color");
                const std::string mode = stringParam(p, "mode", "stroke");
                if (mode == "fill") {
                    std::vector<raster::Point> poly;
                    for (std::size_t i = 0; i + 1 < pts.size(); i += 2) poly.push_back({pts[i], pts[i + 1]});
                    if (poly.size() < 3) throw std::runtime_error("a fill needs at least 3 points");
                    Color fc = c;
                    fc.a = static_cast<std::uint8_t>(std::clamp(fc.a * numberParam(p, "opacity", 1.0), 0.0, 255.0));
                    raster::fillPolygon(img, poly, fc);
                } else {
                    std::vector<raster::StrokePoint> sp;
                    for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
                        const std::size_t k = i / 2;
                        sp.push_back({pts[i], pts[i + 1], k < pressure.size() ? std::clamp(pressure[k], 0.0, 1.0) : 1.0});
                    }
                    sp = raster::smooth(sp, std::clamp(numberParam(p, "smooth", 0.0), 0.0, 0.95));
                    raster::Brush b;
                    b.color = c;
                    b.size = numberParam(p, "size", 6);
                    b.hardness = std::clamp(numberParam(p, "hardness", 0.9), 0.0, 1.0);
                    b.opacity = std::clamp(numberParam(p, "opacity", 1.0), 0.0, 1.0);
                    b.erase = mode == "erase";
                    if (b.size <= 0) throw std::runtime_error("size must be more than 0");
                    raster::stroke(img, b, sp);
                }
                e.cels[frame] = std::make_shared<const Image>(std::move(img));
                Result r = Result::success(std::string(mode == "fill" ? "filled" : mode == "erase" ? "erased" : "drew") + " on " + e.id +
                                           " at frame " + std::to_string(frame) + (fresh ? " (new drawing)" : "") + "; drawings at " +
                                           [&] {
                                               std::string s;
                                               for (const auto& [f, i] : e.cels) s += (s.empty() ? "" : ",") + std::to_string(f);
                                               return s;
                                           }());
                r.with("element", e.id).with("frame", i64(frame)).with("drawings", i64(e.cels.size()));
                return maybePreview(r, doc, p, frame);
            }));

    reg.add({"animate.cel", "Cel Drawings",
             "Manage a cel's drawings: blank (an empty drawing at frame, ending the one held before), delete (remove the "
             "drawing at frame so the previous one holds), copy (the drawing showing at `from` onto frame), import (an "
             "image file stretched to the cel; a folder imports its images in name order onto frame, frame+hold, ...).",
             {docParam(), elementParam(),
              {"action", ParamType::String, "what to do", true, {"blank", "delete", "copy", "import"}},
              {"frame", ParamType::Int, "frame to act on", true},
              {"from", ParamType::Int, "copy: frame whose drawing to copy", false},
              {"path", ParamType::String, "import: an image file or a folder of images", false},
              {"hold", ParamType::Int, "import a folder: frames each image holds (2 = animating on twos)", false, {}, std::int64_t{1}},
              {"onion", ParamType::Int, "preview: onion skin drawings each side", false, {}, std::int64_t{0}},
              previewParam()},
             true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                Element& e = requireElement(doc, p);
                if (e.kind != "cel") throw std::runtime_error(e.id + " is a " + e.kind + ", not a cel");
                const int frame = static_cast<int>(intParam(p, "frame"));
                checkFrame(doc, frame);
                const std::string action = stringParam(p, "action");
                const int w = std::max(1, static_cast<int>(std::lround(e.numbers["width"])));
                const int h = std::max(1, static_cast<int>(std::lround(e.numbers["height"])));
                std::string msg;
                if (action == "blank") {
                    e.cels[frame] = std::make_shared<const Image>(w, h, Color{0, 0, 0, 0});
                    msg = "blank drawing at frame " + std::to_string(frame);
                } else if (action == "delete") {
                    if (!e.cels.erase(frame)) throw std::runtime_error(e.id + " has no drawing starting at frame " + std::to_string(frame));
                    msg = "deleted the drawing at frame " + std::to_string(frame);
                } else if (action == "copy") {
                    if (!has(p, "from")) throw std::runtime_error("copy needs from=<frame>");
                    auto src = celAt(e, static_cast<int>(intParam(p, "from")));
                    if (!src) throw std::runtime_error("nothing is drawn at frame " + std::to_string(intParam(p, "from")));
                    e.cels[frame] = src;
                    msg = "copied the drawing at frame " + std::to_string(intParam(p, "from")) + " to frame " + std::to_string(frame);
                } else {
                    const fs::path path = stringParam(p, "path");
                    if (path.empty()) throw std::runtime_error("import needs path=<image file or folder>");
                    std::error_code ec;
                    std::vector<std::string> files;
                    if (fs::is_directory(path, ec)) files = imagesIn(path);
                    else if (fs::is_regular_file(path, ec)) files.push_back(path.string());
                    else throw std::runtime_error("not found: " + path.string());
                    if (files.empty()) throw std::runtime_error("no images in " + path.string());
                    const int hold = std::max<int>(1, static_cast<int>(intParam(p, "hold", 1)));
                    int f = frame, n = 0;
                    for (const auto& file : files) {
                        if (f >= doc.settings.frames) break;
                        Image img = raster::load(file);
                        if (img.width() != w || img.height() != h) img = raster::resize(img, w, h);
                        e.cels[f] = std::make_shared<const Image>(std::move(img));
                        f += hold;
                        ++n;
                    }
                    msg = "imported " + std::to_string(n) + " drawing" + (n == 1 ? "" : "s") + " from frame " + std::to_string(frame);
                    if (n < static_cast<int>(files.size())) msg += " (" + std::to_string(files.size() - static_cast<std::size_t>(n)) + " didn't fit before the last frame)";
                }
                Result r = Result::success(e.id + ": " + msg);
                r.with("element", e.id).with("drawings", i64(e.cels.size()));
                return maybePreview(r, doc, p, frame);
            }));

}

void registerLooking(Session& session) {
    auto& reg = session.registry();

    reg.add({"animate.preview", "Preview Frames",
             "Look at the animation: one frame as a PNG, or several frames side by side (frames=0,12,24,36) to see the "
             "motion in one picture. onion shows neighbouring cel drawings; bones draws the rig.",
             {docParam(),
              {"frame", ParamType::Int, "the frame to show", false, {}, std::int64_t{0}},
              {"frames", ParamType::NumberList, "several frames for a contact strip, e.g. 0,6,12,18", false},
              {"every", ParamType::Int, "strip of every Nth frame across the whole animation (instead of frames)", false},
              {"onion", ParamType::Int, "cels: show this many drawings before (red) and after (green)", false, {}, std::int64_t{0}},
              {"bones", ParamType::Bool, "draw bones and joints on top", false, {}, false},
              {"size", ParamType::Int, "longest side of the image in px (per frame in a strip)", false, {}, std::int64_t{640}}},
             false, true},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                const int onion = static_cast<int>(intParam(p, "onion", 0));
                const bool bones = boolParam(p, "bones", false);
                std::vector<int> frames;
                for (double f : listParam(p, "frames")) frames.push_back(static_cast<int>(f));
                if (has(p, "every")) {
                    const int every = std::max<int>(1, static_cast<int>(intParam(p, "every")));
                    for (int f = 0; f < doc.settings.frames; f += every) frames.push_back(f);
                }
                for (int f : frames) checkFrame(doc, f);
                if (frames.size() > 48) throw std::runtime_error("at most 48 frames in a strip (use a bigger every=)");
                const int size = std::clamp<int>(static_cast<int>(intParam(p, "size", 640)), 16, 4096);
                Result r;
                if (frames.empty()) {
                    const int frame = static_cast<int>(intParam(p, "frame", 0));
                    checkFrame(doc, frame);
                    r = Result::success("frame " + std::to_string(frame) + " of " + stringParam(p, "doc"));
                    r.attach({"image/png", framePng(doc, frame, size, onion, bones), "frame" + std::to_string(frame) + ".png"});
                    r.with("frame", i64(frame));
                } else {
                    Image sheet = contactStrip(doc, frames, std::min(size, frames.size() > 4 ? 320 : 480), onion, bones);
                    std::string list;
                    for (int f : frames) list += (list.empty() ? "" : ",") + std::to_string(f);
                    r = Result::success("frames " + list + " of " + stringParam(p, "doc"));
                    r.attach({"image/png", raster::encodePng(sheet), "strip.png"});
                    r.with("frames", i64(frames.size()));
                }
                return r.with("doc", stringParam(p, "doc"));
            }, false));

    reg.add({"animate.render", "Render Frame",
             "Render one frame to a PNG file at full (or scaled) size. preview=true also attaches it.",
             {docParam(),
              {"frame", ParamType::Int, "frame number", true},
              {"output", ParamType::String, "PNG (or JPG/BMP/TGA) file to write", true},
              {"scale", ParamType::Number, "output size as a fraction of the canvas", false, {}, 1.0},
              {"onion", ParamType::Int, "include onion skins", false, {}, std::int64_t{0}},
              {"bones", ParamType::Bool, "include the rig", false, {}, false},
              overwriteParam(), previewParam()},
             false},
            animCommand(session, [](AnimDoc& doc, const Params& p) {
                const int frame = static_cast<int>(intParam(p, "frame"));
                checkFrame(doc, frame);
                const fs::path out = stringParam(p, "output");
                if (!raster::canWrite(out)) throw std::runtime_error("write frames as .png, .jpg, .bmp or .tga");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                FrameOptions o = exportOptions(p);
                o.onion = static_cast<int>(intParam(p, "onion", 0));
                o.bones = boolParam(p, "bones", false);
                Image img = renderFrame(doc, frame, o);
                raster::save(img, out);
                Result r = Result::success("rendered frame " + std::to_string(frame) + " to " + out.string() + " (" +
                                           std::to_string(img.width()) + "x" + std::to_string(img.height()) + ")", out.string());
                r.with("output", out.string()).with("width", i64(img.width())).with("height", i64(img.height())).with("bytes", fileBytes(out));
                if (boolParam(p, "preview", false)) r.attach({"image/png", raster::previewPng(img, 640), "frame.png"});
                return r;
            }, false));

}

void registerExports(Session& session) {
    auto& reg = session.registry();

    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"dir", ParamType::String, "folder to write the frames into (made if missing)", true},
                                         {"prefix", ParamType::String, "file name before the 4-digit frame number", false, {}, std::string("frame_")}};
        for (const auto& s : rangeParams()) params.push_back(s);
        params.push_back(overwriteParam());
        reg.add({"animate.export_png", "Export PNG Sequence",
                 "Write frames as numbered PNGs (frame_0000.png, ...) with transparency kept: for compositing, video "
                 "editors and game engines.",
                 params, false},
                animCommand(session, [](AnimDoc& doc, const Params& p) {
                    const Range range = rangeOf(doc, p);
                    const FrameOptions o = exportOptions(p);
                    const fs::path dir = stringParam(p, "dir");
                    std::error_code ec;
                    fs::create_directories(dir, ec);
                    if (!fs::is_directory(dir, ec)) throw std::runtime_error("can't make the folder " + dir.string());
                    const std::string prefix = stringParam(p, "prefix", "frame_");
                    std::int64_t n = 0, bytes = 0;
                    std::string first;
                    for (int f : range.frames()) {
                        const fs::path out = dir / frameName(prefix, f);
                        if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                        raster::save(renderFrame(doc, f, o), out);
                        if (first.empty()) first = out.string();
                        bytes += fileBytes(out);
                        ++n;
                    }
                    Result r = Result::success("wrote " + std::to_string(n) + " frames to " + dir.string() + " (" + frameName(prefix, range.start) +
                                               " ...)", dir.string());
                    return r.with("dir", dir.string()).with("frames", n).with("first", first).with("bytes", bytes);
                }, false));
    }

    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"output", ParamType::String, "sprite sheet PNG to write", true},
                                         {"atlas", ParamType::String, "JSON atlas to write (default: output with .json)", false},
                                         {"columns", ParamType::Int, "frames per row (default: a near-square grid)", false},
                                         {"padding", ParamType::Int, "empty px between frames", false, {}, std::int64_t{0}},
                                         {"name", ParamType::String, "animation name in the atlas and frame-name prefix (default: the output's name)", false}};
        for (const auto& s : rangeParams()) params.push_back(s);
        params.push_back(overwriteParam());
        params.push_back(previewParam());
        reg.add({"animate.export_sheet", "Export Sprite Sheet",
                 "Pack frames into one sprite sheet PNG in a grid plus a JSON atlas in the TexturePacker hash format "
                 "(frames with x, y, w, h and duration in ms; an animations list; meta with size and frame rate) that "
                 "Phaser, PixiJS, Godot importers, Unity importers and forge.sprite_split read.",
                 params, false},
                animCommand(session, [](AnimDoc& doc, const Params& p) {
                    const Range range = rangeOf(doc, p);
                    const FrameOptions o = exportOptions(p);
                    const fs::path out = stringParam(p, "output");
                    if (extOf(out) != "png") throw std::runtime_error("sprite sheets are written as .png");
                    const fs::path atlas = has(p, "atlas") ? fs::path(stringParam(p, "atlas")) : fs::path(out).replace_extension(".json");
                    if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                    if (std::string err = checkOutput(atlas, p); !err.empty()) return Result::failure(err);
                    const auto frames = range.frames();
                    const int n = static_cast<int>(frames.size());
                    const int fw = std::max(1, static_cast<int>(std::lround(doc.settings.width * o.scale)));
                    const int fh = std::max(1, static_cast<int>(std::lround(doc.settings.height * o.scale)));
                    const int pad = std::max<int>(0, static_cast<int>(intParam(p, "padding", 0)));
                    int cols = has(p, "columns") ? static_cast<int>(intParam(p, "columns"))
                                                 : static_cast<int>(std::ceil(std::sqrt(n * static_cast<double>(fh) / fw)));
                    cols = std::clamp(cols, 1, n);
                    const int rows = (n + cols - 1) / cols;
                    const long long sw = static_cast<long long>(cols) * fw + static_cast<long long>(cols - 1) * pad;
                    const long long sh = static_cast<long long>(rows) * fh + static_cast<long long>(rows - 1) * pad;
                    if (sw > 16384 || sh > 16384) {
                        throw std::runtime_error("the sheet would be " + std::to_string(sw) + "x" + std::to_string(sh) +
                                                 " px (over 16384); use scale=, step= or a frame range");
                    }
                    std::string name = stringParam(p, "name");
                    if (name.empty()) {
                        name = out.stem().string();
                    }
                    Image sheet(static_cast<int>(sw), static_cast<int>(sh), Color{0, 0, 0, 0});
                    json jf = json::object();
                    json anim = json::array();
                    const int ms = static_cast<int>(std::lround(1000.0 * range.step / doc.settings.fps));
                    for (int i = 0; i < n; ++i) {
                        const int x = (i % cols) * (fw + pad), y = (i / cols) * (fh + pad);
                        raster::blendOnto(sheet, renderFrame(doc, frames[static_cast<std::size_t>(i)], o), x, y, raster::Blend::Normal, 1.0);
                        char key[32];
                        std::snprintf(key, sizeof key, "_%04d", frames[static_cast<std::size_t>(i)]);
                        const std::string fname = name + key;
                        jf[fname] = {{"frame", {{"x", x}, {"y", y}, {"w", fw}, {"h", fh}}},
                                     {"rotated", false},
                                     {"trimmed", false},
                                     {"spriteSourceSize", {{"x", 0}, {"y", 0}, {"w", fw}, {"h", fh}}},
                                     {"sourceSize", {{"w", fw}, {"h", fh}}},
                                     {"duration", ms}};
                        anim.push_back(fname);
                    }
                    raster::save(sheet, out);
                    json j = {{"frames", jf},
                              {"animations", {{name, anim}}},
                              {"meta", {{"app", "achclwss animate"},
                                        {"version", "1"},
                                        {"image", out.filename().string()},
                                        {"format", "RGBA8888"},
                                        {"size", {{"w", sw}, {"h", sh}}},
                                        {"scale", "1"},
                                        {"frameRate", doc.settings.fps / range.step},
                                        {"frameTags", json::array({{{"name", name}, {"from", 0}, {"to", n - 1}, {"direction", "forward"}}})}}}};
                    if (!detail::writeFile(atlas, j.dump(2))) throw std::runtime_error("can't write " + atlas.string());
                    Result r = Result::success("packed " + std::to_string(n) + " frames into " + out.string() + " (" + std::to_string(sw) + "x" +
                                               std::to_string(sh) + ", " + std::to_string(cols) + " columns) + " + atlas.string(), out.string());
                    r.with("output", out.string()).with("atlas", atlas.string()).with("frames", i64(n)).with("columns", i64(cols))
                        .with("rows", i64(rows)).with("width", static_cast<std::int64_t>(sw)).with("height", static_cast<std::int64_t>(sh))
                        .with("frame_width", i64(fw)).with("frame_height", i64(fh));
                    if (boolParam(p, "preview", false)) r.attach({"image/png", raster::previewPng(sheet, 800), "sheet.png"});
                    return r;
                }, false));
    }

    {
        std::vector<ParamSpec> params = {docParam(), {"output", ParamType::String, "GIF file to write", true}};
        for (const auto& s : rangeParams()) params.push_back(s);
        params.push_back({"loop", ParamType::Int, "times to play: 0 = forever", false, {}, std::int64_t{0}});
        params.push_back({"dither", ParamType::Bool, "ordered dithering for smoother gradients (bigger file)", false, {}, false});
        params.push_back(overwriteParam());
        reg.add({"animate.export_gif", "Export GIF",
                 "Write an animated GIF (built-in encoder, no ffmpeg needed). Each frame gets its own 256-colour palette; a "
                 "transparent background stays transparent (pixels under half alpha). Use scale=0.5 or step=2 for smaller files.",
                 params, false},
                animCommand(session, [](AnimDoc& doc, const Params& p) {
                    const Range range = rangeOf(doc, p);
                    const FrameOptions o = exportOptions(p);
                    const fs::path out = stringParam(p, "output");
                    if (extOf(out) != "gif") throw std::runtime_error("the output must end in .gif");
                    if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                    std::vector<Image> frames;
                    std::vector<int> delays;
                    double t = 0;
                    int last = 0;
                    for (int f : range.frames()) {
                        frames.push_back(renderFrame(doc, f, o));
                        // GIF delays are whole 1/100 s, so carry the remainder to keep the overall speed right
                        t += 100.0 * range.step / doc.settings.fps;
                        const int now = static_cast<int>(std::lround(t));
                        delays.push_back(std::max(2, now - last));
                        last = now;
                    }
                    const std::string bytes = encodeGif(frames, delays, static_cast<int>(intParam(p, "loop", 0)), boolParam(p, "dither", false));
                    if (!detail::writeFile(out, bytes)) throw std::runtime_error("can't write " + out.string());
                    Result r = Result::success("wrote " + out.string() + " (" + std::to_string(frames.size()) + " frames, " +
                                               std::to_string(frames[0].width()) + "x" + std::to_string(frames[0].height()) + ", " +
                                               std::to_string(bytes.size() / 1024) + " KB)", out.string());
                    return r.with("output", out.string()).with("frames", i64(frames.size())).with("width", i64(frames[0].width()))
                        .with("height", i64(frames[0].height())).with("bytes", static_cast<std::int64_t>(bytes.size()));
                }, false));
    }

    {
        std::vector<ParamSpec> params = {docParam(),
                                         {"output", ParamType::String, ".mp4 (H.264), .webm (VP9, keeps transparency), .mov (ProRes 4444, keeps transparency) or .mkv", true},
                                         {"quality", ParamType::String, "draft (fast, small), good, best", false, {"draft", "good", "best"}, std::string("good")}};
        for (const auto& s : rangeParams()) {
            if (s.name != "step") params.push_back(s);
        }
        params.push_back(overwriteParam());
        reg.add({"animate.export_video", "Export Video",
                 "Encode the animation as a video with ffmpeg (needs ffmpeg on PATH). mp4/mkv put a transparent background "
                 "on white; webm and mov keep the alpha channel.",
                 params, false},
                animCommand(session, [](AnimDoc& doc, const Params& p) {
                    auto ffmpeg = findProgram("ffmpeg");
                    if (!ffmpeg) {
                        return Result::failure("video export needs ffmpeg, which isn't installed (or isn't on PATH). Install it, it's free: "
                                               "Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`. "
                                               "animate.export_gif and export_png work without it.");
                    }
                    const Range range = rangeOf(doc, p);
                    const FrameOptions o = exportOptions(p);
                    const fs::path out = stringParam(p, "output");
                    const std::string ext = extOf(out);
                    if (ext != "mp4" && ext != "webm" && ext != "mov" && ext != "mkv") throw std::runtime_error("export video as .mp4, .webm, .mov or .mkv");
                    if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                    const bool alphaOut = (ext == "webm" || ext == "mov") && doc.settings.background.a < 255;
                    detail::TempDir tmp;
                    int i = 0;
                    for (int f : range.frames()) {
                        Image img = renderFrame(doc, f, o);
                        if (!alphaOut && doc.settings.background.a < 255) {
                            Image flat(img.width(), img.height(), Color{255, 255, 255, 255});
                            raster::blendOnto(flat, img, 0, 0, raster::Blend::Normal, 1.0);
                            img = std::move(flat);
                        }
                        char name[32];
                        std::snprintf(name, sizeof name, "f%06d.png", i++);
                        raster::save(img, tmp.path() / name);
                    }
                    const std::string q = stringParam(p, "quality", "good");
                    const std::string fps = num(doc.settings.fps);
                    std::vector<std::string> args = {ffmpeg->string(), "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-y",
                                                     "-framerate", fps, "-i", (tmp.path() / "f%06d.png").string()};
                    if (ext == "mp4" || ext == "mkv") {
                        args.insert(args.end(), {"-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf",
                                                 q == "draft" ? "28" : q == "best" ? "16" : "20", "-preset",
                                                 q == "draft" ? "veryfast" : q == "best" ? "slow" : "medium"});
                        if (ext == "mp4") args.insert(args.end(), {"-movflags", "+faststart"});
                    } else if (ext == "webm") {
                        args.insert(args.end(), {"-c:v", "libvpx-vp9", "-b:v", "0", "-crf", q == "draft" ? "40" : q == "best" ? "24" : "32",
                                                 "-deadline", q == "draft" ? "realtime" : "good", "-cpu-used", q == "draft" ? "8" : q == "best" ? "1" : "4",
                                                 "-row-mt", "1", "-pix_fmt", alphaOut ? "yuva420p" : "yuv420p"});
                        if (!alphaOut) args.insert(args.end(), {"-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2"});
                    } else {
                        args.insert(args.end(), {"-c:v", "prores_ks", "-profile:v", alphaOut ? "4444" : "3", "-pix_fmt",
                                                 alphaOut ? "yuva444p10le" : "yuv422p10le", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2"});
                    }
                    args.insert(args.end(), {"-r", fps, fs::absolute(out).string()});
                    ProcessResult pr = runProgram(args);
                    if (pr.exitCode != 0) {
                        std::string msg = pr.output;
                        if (msg.size() > 1500) msg = "..." + msg.substr(msg.size() - 1500);
                        return Result::failure("ffmpeg failed (exit " + std::to_string(pr.exitCode) + "): " + msg);
                    }
                    Result r = Result::success("encoded " + out.string() + " (" + std::to_string(i) + " frames at " + fps + "fps" +
                                               (alphaOut ? ", with transparency" : "") + ")", out.string());
                    return r.with("output", out.string()).with("frames", i64(i)).with("duration", i / doc.settings.fps).with("bytes", fileBytes(out))
                        .with("alpha", alphaOut);
                }, false));
    }
}

}  // namespace

void registerCommands(Session& session) {
    registerDocuments(session);
    registerLayers(session);
    registerElements(session);
    registerKeys(session);
    registerCels(session);
    registerLooking(session);
    registerExports(session);
}

}  // namespace achclwss::animate
