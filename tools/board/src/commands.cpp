// Every board.* command. Edits run inside a BoardEdit, so each one is a
// single undo step and a failed edit leaves the board as it was.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "wetwaresuite/raster/codec.hpp"
#include "internal.hpp"

namespace wetwaresuite::board {

using namespace detail;

namespace {

namespace fs = std::filesystem;
using Edit = std::function<Result(BoardDoc&, const Params&)>;

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + ",") {
        if (c == ',' || c == ' ' || c == ';') {
            if (!item.empty()) out.push_back(item);
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

std::string joinList(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
    return s;
}

std::string plural(std::size_t n, const std::string& word) { return std::to_string(n) + " " + word + (n == 1 ? "" : "s"); }

std::string readText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path.string());
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// ---- shared params ------------------------------------------------------------------------

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open board, e.g. board1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG of the board to the result", false, {}, false}; }
ParamSpec idsParam(const std::string& what = "element ids or names, comma-separated (\"all\" = every element)") {
    return {"ids", ParamType::String, what, true};
}
const std::vector<std::string>& arrowChoices() {
    static const std::vector<std::string> v = {"none", "arrow", "triangle", "diamond", "circle", "bar", "cross"};
    return v;
}
const std::vector<std::string>& anchorChoices() {
    static const std::vector<std::string> v = {"auto", "top", "right", "bottom", "left", "center"};
    return v;
}

std::vector<ParamSpec> styleParams() {
    return {
        {"fill", ParamType::String, "fill colour (#hex, rgb(), CSS name) or none", false},
        {"stroke", ParamType::String, "outline / line colour or none", false},
        {"stroke_width", ParamType::Number, "outline width in px", false},
        {"dash", ParamType::String, "line pattern", false, {"solid", "dashed", "dotted"}},
        {"opacity", ParamType::Number, "0..1", false},
        {"text_color", ParamType::String, "label colour", false},
        {"font_size", ParamType::Number, "label size in px", false},
        {"font", ParamType::String, "label font", false, {"sans", "serif", "mono"}},
        {"bold", ParamType::Bool, "bold label", false},
        {"italic", ParamType::Bool, "italic label", false},
        {"align", ParamType::String, "label alignment", false, {"left", "center", "right"}},
        {"valign", ParamType::String, "label vertical alignment", false, {"top", "middle", "bottom"}},
        {"fill_style", ParamType::String, "solid fill, hachure lines, cross-hatch or none", false, {"solid", "hachure", "cross", "none"}},
        {"sketch", ParamType::Bool, "hand-drawn strokes (default: the theme's)", false},
        {"roughness", ParamType::Number, "how wobbly sketch strokes are, 0..3 (default 1)", false},
        {"radius", ParamType::Number, "corner radius of rounded shapes", false},
        {"shadow", ParamType::Bool, "drop shadow", false},
    };
}

bool applyStyle(Style& s, const Params& p, std::string& error) {
    auto color = [&](const char* key, std::optional<std::string>& out) {
        if (!has(p, key)) return true;
        const std::string v = stringParam(p, key);
        if (v.empty() || v == "theme" || v == "default") {
            out.reset();
            return true;
        }
        if (!validColor(v)) {
            error = std::string(key) + " isn't a colour: " + v;
            return false;
        }
        out = v == "transparent" ? "none" : v;
        return true;
    };
    if (!color("fill", s.fill) || !color("stroke", s.stroke) || !color("text_color", s.textColor)) return false;
    if (has(p, "stroke_width")) s.strokeWidth = std::clamp(numberParam(p, "stroke_width"), 0.0, 100.0);
    if (has(p, "dash")) s.dash = stringParam(p, "dash");
    if (has(p, "opacity")) s.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
    if (has(p, "font_size")) s.fontSize = std::clamp(numberParam(p, "font_size"), 1.0, 1000.0);
    if (has(p, "font")) s.font = stringParam(p, "font");
    if (has(p, "bold")) s.bold = boolParam(p, "bold");
    if (has(p, "italic")) s.italic = boolParam(p, "italic");
    if (has(p, "align")) s.align = stringParam(p, "align");
    if (has(p, "valign")) s.valign = stringParam(p, "valign");
    if (has(p, "fill_style")) s.fillStyle = stringParam(p, "fill_style");
    if (has(p, "sketch")) s.sketch = boolParam(p, "sketch");
    if (has(p, "roughness")) s.roughness = std::clamp(numberParam(p, "roughness"), 0.0, 4.0);
    if (has(p, "radius")) s.radius = std::max(0.0, numberParam(p, "radius"));
    if (has(p, "shadow")) s.shadow = boolParam(p, "shadow");
    return true;
}

std::vector<ParamSpec> withStyle(std::vector<ParamSpec> specs) {
    for (auto& s : styleParams()) specs.push_back(s);
    specs.push_back(previewParam());
    return specs;
}

// ---- results and lookups -------------------------------------------------------------------

Result& maybePreview(Result& r, const BoardDoc& doc, const Params& p) {
    if (r.ok && boolParam(p, "preview", false)) r.attach(*doc.preview());
    return r;
}

Result edited(const std::string& message, const Params& p) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"));
    return r;
}

ActionFactory boardCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<BoardDoc>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] { return Result::failure("no open board called '" + id + "' (use board.new or board.open first)"); });
        }
        auto run = [edit](BoardDoc& d, const Params& q) {
            Result r = edit(d, q);
            return maybePreview(r, d, q);
        };
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, run, p] {
                try {
                    return run(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<BoardEdit>(doc, [run, p](BoardDoc& d) { return run(d, p); });
    };
}

// Adds a new board document; undo removes it again.
class CreateAction : public Action {
public:
    CreateAction(Session& s, std::function<Result(std::shared_ptr<BoardDoc>&)> make) : session_(s), make_(std::move(make)) {}
    Result apply() override {
        if (doc_) {  // redo
            session_.workspace().put(id_, doc_);
            return Result::success("restored " + id_, id_);
        }
        std::shared_ptr<BoardDoc> doc;
        Result r;
        try {
            r = make_(doc);
        } catch (const std::exception& e) {
            return Result::failure(e.what());
        }
        if (!r.ok || !doc) return r;
        doc_ = doc;
        id_ = session_.workspace().add(doc, "board");
        std::string msg = r.message.empty() ? "created " + id_ : r.message + " " + id_ + ": " + doc->summary();
        if (const auto at = r.message.find("{id}"); at != std::string::npos) msg = r.message.substr(0, at) + id_ + r.message.substr(at + 4);
        Result out = Result::success(msg, id_);
        out.with("doc", id_);
        for (auto& f : r.data) out.data.push_back(f);
        out.attachments = r.attachments;
        return out;
    }
    void revert() override {
        if (!id_.empty()) session_.workspace().take(id_);
    }

private:
    Session& session_;
    std::function<Result(std::shared_ptr<BoardDoc>&)> make_;
    std::shared_ptr<BoardDoc> doc_;
    std::string id_;
};

Result docCreated(Session& session, std::shared_ptr<BoardDoc> doc, const std::string& verb, const Params& p) {
    const std::string id = session.workspace().add(doc, "board");
    Result r = Result::success(verb + " " + id + ": " + doc->summary(), id);
    r.with("doc", id).with("elements", static_cast<std::int64_t>(doc->board.elements.size())).with("theme", doc->board.theme);
    if (boolParam(p, "preview", false)) r.attach(*doc->preview());
    return r;
}

bool resolveIds(const Board& b, const std::string& list, std::vector<std::string>& out, std::string& error) {
    out.clear();
    if (lower(trim(list)) == "all") {
        for (const auto& e : b.elements) out.push_back(e.id);
        return true;
    }
    for (const auto& raw : splitList(list)) {
        const Element* e = b.get(raw);
        if (!e) {
            error = "no element '" + raw + "' (board.find or board.info lists them)";
            return false;
        }
        if (std::find(out.begin(), out.end(), e->id) == out.end()) out.push_back(e->id);
    }
    if (out.empty()) error = "no elements given";
    return !out.empty();
}

bool unlocked(const Board& b, const Element& e, std::string& error) {
    const Layer* l = b.layer(e.layer);
    if (l && l->locked) {
        error = e.id + " is on locked layer '" + l->name + "' (board.layer op=set name=" + l->name + " locked=false)";
        return false;
    }
    return true;
}

std::string describe(const Board& b, const Element& e) {
    std::string s = e.id + " " + toString(e.kind);
    if (!e.name.empty()) s += " '" + e.name + "'";
    if (!e.text.empty()) {
        std::string t = e.text;
        for (auto& c : t) {
            if (c == '\n') c = ' ';
        }
        if (t.size() > 40) t = t.substr(0, 37) + "...";
        s += " \"" + t + "\"";
    }
    if (e.kind == Kind::Connector) {
        auto endName = [&](const std::string& id, Pt p) { return id.empty() ? "(" + fmtNum(p.x) + "," + fmtNum(p.y) + ")" : id; };
        s += " " + endName(e.from, e.start) + " -> " + endName(e.to, e.end) + " " + e.route;
    } else {
        const Box bx = elementBox(b, e);
        s += " at " + fmtNum(std::round(bx.x0)) + "," + fmtNum(std::round(bx.y0)) + " " + fmtNum(std::round(bx.w())) + "x" + fmtNum(std::round(bx.h()));
    }
    if (!e.parent.empty()) s += " in " + e.parent;
    if (e.layer != "default") s += " [" + e.layer + "]";
    return s;
}

// Places a new element: explicit x/y, next to `near`, or right of everything.
void placeNew(const Board& b, Element& e, const Params& p) {
    if (has(p, "near")) {
        if (const Element* n = b.get(stringParam(p, "near"))) {
            const Box nb = elementBox(b, *n);
            const std::string side = stringParam(p, "side", "right");
            const double gap = numberParam(p, "gap", 60);
            if (side == "right") e.x = nb.x1 + gap, e.y = nb.cy() - e.h / 2;
            else if (side == "left") e.x = nb.x0 - gap - e.w, e.y = nb.cy() - e.h / 2;
            else if (side == "below") e.x = nb.cx() - e.w / 2, e.y = nb.y1 + gap;
            else e.x = nb.cx() - e.w / 2, e.y = nb.y0 - gap - e.h;
            e.x = std::round(e.x);
            e.y = std::round(e.y);
        }
    } else if (!has(p, "x") || !has(p, "y")) {
        Box c = contentBounds(b, false);
        const double x = c.empty ? 0 : c.x1 + 40, y = c.empty ? 0 : c.y0;
        if (!has(p, "x")) e.x = x;
        if (!has(p, "y")) e.y = y;
    }
    if (has(p, "x")) e.x = numberParam(p, "x");
    if (has(p, "y")) e.y = numberParam(p, "y");
}

// ---- file commands ---------------------------------------------------------------------------

std::vector<ParamSpec> renderParams() {
    return {
        {"scale", ParamType::Number, "zoom: 2 = twice the pixels (PNG/SVG/PDF)", false, {}, 1.0},
        {"padding", ParamType::Number, "margin around the content in board units", false, {}, 32.0},
        {"region", ParamType::NumberList, "x,y,w,h of the canvas area to draw (default: everything)", false},
        {"only", ParamType::String, "element ids to draw (with their contents), e.g. a frame id", false},
        {"layers", ParamType::String, "layer names to draw, comma-separated (default: visible layers)", false},
        {"transparent", ParamType::Bool, "PNG/SVG/PDF without the background", false, {}, false},
    };
}

RenderOptions renderOptions(const Params& p) {
    RenderOptions o;
    o.scale = std::clamp(numberParam(p, "scale", 1.0), 0.05, 16.0);
    o.padding = std::max(0.0, numberParam(p, "padding", 32.0));
    const auto r = listParam(p, "region");
    if (r.size() == 4 && r[2] > 0 && r[3] > 0) o.region = Box{r[0], r[1], r[0] + r[2], r[1] + r[3], false};
    if (has(p, "only")) o.only = splitList(stringParam(p, "only"));
    if (has(p, "layers")) {
        for (const auto& l : splitList(stringParam(p, "layers"))) o.layers.push_back(l);
    }
    o.transparent = boolParam(p, "transparent", false);
    return o;
}

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"board.new", "New Board",
             "Create an empty whiteboard (an infinite canvas) and return its id, e.g. board1. Then add shapes "
             "(board.add), connect them (board.connect), draw ink (board.ink), or type a whole diagram as Mermaid "
             "(board.from_mermaid) or an indented outline (board.from_outline). Themes: default, sketchy (hand-drawn, "
             "like Excalidraw), dark, blueprint, pastel, mono.",
             {{"title", ParamType::String, "board title", false},
              {"theme", ParamType::String, "look of everything without its own style", false, themeNames(), std::string("default")},
              {"background", ParamType::String, "canvas colour (default: the theme's)", false},
              previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<BoardDoc>();
                    doc->board.title = stringParam(p, "title");
                    doc->board.theme = stringParam(p, "theme", "default");
                    if (has(p, "background")) {
                        if (!validColor(stringParam(p, "background"))) return Result::failure("background isn't a colour");
                        doc->board.background = stringParam(p, "background");
                    }
                    return docCreated(session, doc, "created", p);
                });
            });

    reg.add({"board.open", "Open Board",
             "Open a board file: native .board.json, an Excalidraw scene (.excalidraw) or a draw.io diagram "
             "(.drawio/.xml, first page, compressed or not). Image paths are read relative to the file.",
             {{"path", ParamType::String, "file to open", true}, previewParam()},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        auto doc = std::make_shared<BoardDoc>();
                        doc->board = loadBoard(stringParam(p, "path"));
                        doc->path = stringParam(p, "path");
                        return docCreated(session, doc, "opened " + doc->path + " as", p);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    auto saveSpecs = [](bool exportCmd) {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "output file", true}};
        s.push_back({"format", ParamType::String, "override the extension", false,
                     {"auto", "board", "excalidraw", "drawio", "svg", "pdf", "png", "jpg"}, std::string("auto")});
        if (exportCmd) {
            for (auto& r : renderParams()) s.push_back(r);
        }
        return s;
    };
    reg.add({"board.save", "Save Board",
             "Save the board. The extension picks the format: .board.json (native, keeps everything; reopen with "
             "board.open), .excalidraw (opens in excalidraw.com), .drawio (opens in diagrams.net / draw.io), or "
             ".svg/.pdf/.png/.jpg for a picture of the whole board (board.export has the picture options).",
             saveSpecs(false), false},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                saveBoard(doc.board, path, stringParam(p, "format", "auto"));
                const std::string fmt = stringParam(p, "format", "auto") == "auto" ? formatFor(path) : stringParam(p, "format");
                if (fmt == "board" || fmt.empty()) doc.path = path;
                Result r = edited("saved " + path, p);
                r.with("path", path).with("format", fmt);
                return r;
            }, false));

    reg.add({"board.export", "Export Board",
             "Write a picture of the board or part of it: PNG/JPG (raster, scale = zoom), SVG (vector, text stays "
             "text), PDF (vector, one page sized to the content), or Excalidraw/draw.io files. `only` exports one "
             "frame or a few elements; `region` an exact canvas rectangle; `layers` picks layers.",
             saveSpecs(true), false},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                const RenderOptions o = renderOptions(p);
                saveBoard(doc.board, path, stringParam(p, "format", "auto"), o);
                const Box r = renderRegion(doc.board, o);
                Result res = edited("exported " + path, p);
                res.with("path", path)
                    .with("width", static_cast<std::int64_t>(std::ceil(r.w() * o.scale)))
                    .with("height", static_cast<std::int64_t>(std::ceil(r.h() * o.scale)));
                return res;
            }, false));

    reg.add({"board.import", "Import Into Board",
             "Add everything from another board file (.board.json, .excalidraw, .drawio) or a Mermaid/outline text "
             "file into this board, shifted by dx,dy (default: to the right of what is there). Ids are renumbered.",
             {docParam(), {"path", ParamType::String, "file to import", true},
              {"dx", ParamType::Number, "shift right", false}, {"dy", ParamType::Number, "shift down", false}, previewParam()},
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                const fs::path path = stringParam(p, "path");
                Board other;
                const std::string ext = lower(path.extension().string());
                if (ext == ".mmd" || ext == ".mermaid") {
                    other.theme = doc.board.theme;
                    fromMermaid(other, readText(path), {});
                } else if (ext == ".md" || ext == ".txt") {
                    other.theme = doc.board.theme;
                    fromOutline(other, readText(path), {});
                } else {
                    other = loadBoard(path);
                }
                Board& b = doc.board;
                const Box mine = contentBounds(b, false), theirs = contentBounds(other, false);
                double dx = has(p, "dx") ? numberParam(p, "dx") : (mine.empty || theirs.empty ? 0 : mine.x1 + 80 - theirs.x0);
                double dy = has(p, "dy") ? numberParam(p, "dy") : (mine.empty || theirs.empty ? 0 : mine.y0 - theirs.y0);
                std::map<std::string, std::string> map;
                for (auto& l : other.layers) b.ensureLayer(l.name);
                std::vector<std::string> added;
                for (auto e : other.elements) {
                    const std::string old = e.id;
                    e.parent.clear();
                    const std::string id = b.add(e);
                    map[old] = id;
                    added.push_back(id);
                }
                for (std::size_t i = 0; i < other.elements.size(); ++i) {
                    Element* e = b.get(added[i]);
                    const Element& src = other.elements[i];
                    auto re = [&](const std::string& id) { return id.empty() || !map.count(id) ? std::string() : map[id]; };
                    e->parent = re(src.parent);
                    e->from = re(src.from);
                    e->to = re(src.to);
                }
                for (const auto& id : added) {
                    Element* e = b.get(id);
                    if (e && e->parent.empty()) moveBy(b, *e, dx, dy);
                }
                Result r = edited("imported " + plural(added.size(), "element") + " from " + path.string(), p);
                r.with("added", static_cast<std::int64_t>(added.size())).with("ids", joinList(added));
                return r;
            }));
}

// ---- shapes ------------------------------------------------------------------------------------

void registerShapeCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"board.add", "Add Shape",
             "Add a shape and return its id (e1, e2, ...). Types: rect, rounded, stadium (pill), ellipse, circle, "
             "diamond, hexagon, parallelogram, trapezoid, triangle, cylinder (database), cloud, note (sticky note), "
             "subroutine, text, image (src=file), frame (a titled area; put things in it with parent= or board.group). "
             "Without w/h the shape grows to fit its text. Place it with x,y or next to another element with near= "
             "and side=. name= gives it a name commands can use instead of the id.",
             withStyle({docParam(),
                        {"type", ParamType::String, "shape type", false, shapeKindNames(), std::string("rect")},
                        {"text", ParamType::String, "label (\\n for new lines)", false},
                        {"x", ParamType::Number, "left edge", false},
                        {"y", ParamType::Number, "top edge", false},
                        {"w", ParamType::Number, "width (default: fits the text)", false},
                        {"h", ParamType::Number, "height (default: fits the text)", false},
                        {"near", ParamType::String, "place it next to this element", false},
                        {"side", ParamType::String, "which side of `near`", false, {"right", "left", "below", "above"}, std::string("right")},
                        {"gap", ParamType::Number, "distance from `near`", false, {}, 60.0},
                        {"name", ParamType::String, "a name to refer to it by", false},
                        {"layer", ParamType::String, "layer (created if new; default: the first)", false},
                        {"parent", ParamType::String, "frame or group to put it in", false},
                        {"src", ParamType::String, "image: file path or data: URL", false},
                        {"link", ParamType::String, "URL kept with the element (exported to Excalidraw)", false}}),
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                const std::string type = stringParam(p, "type", "rect");
                Element e;
                e.kind = parseKind(type).value_or(Kind::Rect);
                e.text = stringParam(p, "text");
                e.name = stringParam(p, "name");
                e.layer = has(p, "layer") ? stringParam(p, "layer") : b.layers.front().name;
                e.src = stringParam(p, "src");
                e.link = stringParam(p, "link");
                std::string error;
                if (!applyStyle(e.style, p, error)) return Result::failure(error);
                if (has(p, "parent")) {
                    const Element* parent = b.get(stringParam(p, "parent"));
                    if (!parent || (parent->kind != Kind::Frame && parent->kind != Kind::Group))
                        return Result::failure("parent must be a frame or group: " + stringParam(p, "parent"));
                    e.parent = parent->id;
                }
                if (!e.name.empty() && b.find(e.name) >= 0) return Result::failure("an element called '" + e.name + "' already exists");
                if (const Layer* l = b.layer(e.layer); l && l->locked) return Result::failure("layer '" + e.layer + "' is locked");
                // size
                if (e.kind == Kind::Image) {
                    auto img = loadImageSrc(e.src);
                    if (!img) return Result::failure("can't read the image " + (e.src.empty() ? std::string("(give src=)") : e.src));
                    double w = img->width(), h = img->height();
                    const double k = std::min(1.0, 400.0 / std::max(w, h));
                    e.w = has(p, "w") ? numberParam(p, "w") : (has(p, "h") ? numberParam(p, "h") * w / h : w * k);
                    e.h = has(p, "h") ? numberParam(p, "h") : e.w * h / w;
                } else {
                    const Resolved s = resolve(b, e);
                    double minW = 120, minH = 60;
                    if (e.kind == Kind::Note) minW = minH = 160;
                    if (e.kind == Kind::Diamond) minW = 130, minH = 90;
                    if (e.kind == Kind::Frame) minW = 400, minH = 300;
                    if (e.kind == Kind::Text) minW = minH = 1;
                    if (e.kind == Kind::Cloud) minW = 160, minH = 100;
                    Pt fit = fitText(e.kind, e.text, s, minW, minH);
                    if (e.kind == Kind::Frame) fit = {minW, minH};
                    if (e.kind == Kind::Text && e.text.empty()) fit = {100, s.fontSize * kLineHeight};
                    if (lower(type) == "circle") fit.x = fit.y = std::max(fit.x, fit.y);
                    if (has(p, "w") && e.kind == Kind::Text && !has(p, "h")) fit = fitText(e.kind, e.text, s, numberParam(p, "w"), 1), fit.x = numberParam(p, "w");
                    e.w = has(p, "w") ? numberParam(p, "w") : std::round(fit.x);
                    e.h = has(p, "h") ? numberParam(p, "h") : std::round(fit.y);
                    if (lower(type) == "circle" && has(p, "w") && !has(p, "h")) e.h = e.w;
                }
                e.w = std::max(1.0, e.w);
                e.h = std::max(1.0, e.h);
                placeNew(b, e, p);
                const std::string id = b.add(e);
                if (e.kind == Kind::Frame) {  // frames sit under what's already there
                    std::stable_partition(b.elements.begin(), b.elements.end(), [](const Element& x) { return x.kind == Kind::Frame; });
                }
                if (!e.parent.empty()) refreshGroups(b);
                Result r = edited("added " + describe(b, *b.get(id)), p);
                r.value = id;
                r.with("id", id).with("x", e.x).with("y", e.y).with("w", e.w).with("h", e.h);
                return r;
            }));

    std::vector<ParamSpec> setSpecs = withStyle({docParam(),
                                                 idsParam("element(s) to change: ids or names, comma-separated"),
                                                 {"text", ParamType::String, "new label", false},
                                                 {"name", ParamType::String, "new name", false},
                                                 {"type", ParamType::String, "change the shape type", false, shapeKindNames()},
                                                 {"x", ParamType::Number, "left edge", false},
                                                 {"y", ParamType::Number, "top edge", false},
                                                 {"w", ParamType::Number, "width", false},
                                                 {"h", ParamType::Number, "height", false},
                                                 {"dx", ParamType::Number, "move right by", false},
                                                 {"dy", ParamType::Number, "move down by", false},
                                                 {"fit", ParamType::Bool, "resize to fit the text", false},
                                                 {"layer", ParamType::String, "move to this layer", false},
                                                 {"parent", ParamType::String, "frame/group to move into (\"none\" to take it out)", false},
                                                 {"src", ParamType::String, "image source", false},
                                                 {"link", ParamType::String, "URL", false},
                                                 {"from", ParamType::String, "connector: new start element", false},
                                                 {"to", ParamType::String, "connector: new end element", false},
                                                 {"route", ParamType::String, "connector route", false, {"straight", "elbow", "curved"}},
                                                 {"start_arrow", ParamType::String, "connector start head", false, arrowChoices()},
                                                 {"end_arrow", ParamType::String, "connector end head", false, arrowChoices()},
                                                 {"from_anchor", ParamType::String, "connector: side it leaves from", false, anchorChoices()},
                                                 {"to_anchor", ParamType::String, "connector: side it arrives at", false, anchorChoices()},
                                                 {"ink_width", ParamType::Number, "ink: stroke width", false},
                                                 {"clear_style", ParamType::Bool, "drop the element's own style first (back to the theme)", false}});
    reg.add({"board.set", "Change Elements",
             "Change one or more elements: move (x,y or dx,dy; frames and groups move their contents and attached "
             "connectors follow), resize (w,h or fit=true), relabel, restyle, change type, layer or parent, or re-route "
             "a connector. Only the params you give change.",
             setSpecs, true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIds(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                for (const auto& id : ids) {
                    Element& e = *b.get(id);
                    if (!unlocked(b, e, error)) return Result::failure(error);
                    if (boolParam(p, "clear_style", false)) e.style = Style{};
                    if (!applyStyle(e.style, p, error)) return Result::failure(error);
                    if (has(p, "type")) {
                        const std::string t = stringParam(p, "type");
                        if (e.kind == Kind::Connector || e.kind == Kind::Ink || e.kind == Kind::Group) return Result::failure(id + " is a " + toString(e.kind) + "; its type can't change");
                        e.kind = parseKind(t).value_or(e.kind);
                        if (lower(t) == "circle") e.w = e.h = std::max(e.w, e.h);
                    }
                    if (has(p, "name")) {
                        const std::string n = stringParam(p, "name");
                        const int other = b.find(n);
                        if (!n.empty() && other >= 0 && b.elements[static_cast<std::size_t>(other)].id != e.id) return Result::failure("'" + n + "' is taken");
                        e.name = n;
                    }
                    if (has(p, "text")) e.text = stringParam(p, "text");
                    if (has(p, "src")) e.src = stringParam(p, "src");
                    if (has(p, "link")) e.link = stringParam(p, "link");
                    if (has(p, "layer")) {
                        e.layer = stringParam(p, "layer");
                        b.ensureLayer(e.layer);
                        for (const auto& d : b.descendants(e.id)) b.get(d)->layer = e.layer;
                    }
                    if (has(p, "parent")) {
                        const std::string pid = stringParam(p, "parent");
                        if (pid == "none" || pid.empty()) {
                            e.parent.clear();
                        } else {
                            const Element* par = b.get(pid);
                            if (!par || (par->kind != Kind::Frame && par->kind != Kind::Group)) return Result::failure("parent must be a frame or group: " + pid);
                            const auto desc = b.descendants(e.id);
                            if (par->id == e.id || std::find(desc.begin(), desc.end(), par->id) != desc.end()) return Result::failure("can't put an element inside itself");
                            e.parent = par->id;
                        }
                    }
                    if (e.kind == Kind::Connector) {
                        for (const char* key : {"from", "to"}) {
                            if (!has(p, key)) continue;
                            const std::string v = stringParam(p, key);
                            std::string& slot = std::string(key) == "from" ? e.from : e.to;
                            if (v.empty() || v == "none") {
                                const auto pts = connectorPath(b, e);
                                if (!pts.empty()) (std::string(key) == "from" ? e.start : e.end) = std::string(key) == "from" ? pts.front() : pts.back();
                                slot.clear();
                                continue;
                            }
                            const Element* t = b.get(v);
                            if (!t || !isNode(t->kind)) return Result::failure("connectors attach to shapes: " + v);
                            slot = t->id;
                        }
                        if (has(p, "route")) e.route = stringParam(p, "route");
                        if (has(p, "start_arrow")) e.startArrow = stringParam(p, "start_arrow");
                        if (has(p, "end_arrow")) e.endArrow = stringParam(p, "end_arrow");
                        if (has(p, "from_anchor")) e.fromAnchor = stringParam(p, "from_anchor");
                        if (has(p, "to_anchor")) e.toAnchor = stringParam(p, "to_anchor");
                    }
                    if (has(p, "ink_width") && e.kind == Kind::Ink) e.inkWidth = std::clamp(numberParam(p, "ink_width"), 0.2, 200.0);
                    // geometry
                    if (e.kind != Kind::Connector) {
                        const Box bx = elementBox(b, e);
                        double nx = has(p, "x") ? numberParam(p, "x") : bx.x0, ny = has(p, "y") ? numberParam(p, "y") : bx.y0;
                        nx += numberParam(p, "dx", 0);
                        ny += numberParam(p, "dy", 0);
                        double nw = has(p, "w") ? numberParam(p, "w") : bx.w(), nh = has(p, "h") ? numberParam(p, "h") : bx.h();
                        const bool fit = boolParam(p, "fit", false) || (has(p, "text") && !has(p, "w") && !has(p, "h") && e.kind == Kind::Text);
                        if (fit && e.kind != Kind::Image && e.kind != Kind::Ink && e.kind != Kind::Group && e.kind != Kind::Frame) {
                            const Resolved s = resolve(b, e);
                            const Pt sz = fitText(e.kind, e.text, s, e.kind == Kind::Text ? 1 : 60, e.kind == Kind::Text ? 1 : 30);
                            nw = std::round(sz.x);
                            nh = std::round(sz.y);
                        } else if (has(p, "text") && !has(p, "w") && !has(p, "h") && e.kind != Kind::Image && e.kind != Kind::Ink && e.kind != Kind::Frame) {
                            const Resolved s = resolve(b, e);  // grow (never shrink) so the new text fits
                            const Pt sz = fitText(e.kind, e.text, s, nw, nh);
                            nw = std::max(nw, std::round(sz.x));
                            nh = std::max(nh, std::round(sz.y));
                        }
                        if (nw != bx.w() || nh != bx.h()) {
                            moveBy(b, e, nx - bx.x0, ny - bx.y0);
                            const Box moved = elementBox(b, e);
                            resizeTo(b, e, moved.x0, moved.y0, nw, nh);
                        } else if (nx != bx.x0 || ny != bx.y0) {
                            moveBy(b, e, nx - bx.x0, ny - bx.y0);
                        }
                    } else if (has(p, "dx") || has(p, "dy")) {
                        moveBy(b, e, numberParam(p, "dx", 0), numberParam(p, "dy", 0));
                    }
                }
                refreshGroups(b);
                std::string msg = ids.size() == 1 ? "changed " + describe(b, *b.get(ids[0])) : "changed " + plural(ids.size(), "element");
                Result r = edited(msg, p);
                r.with("ids", joinList(ids));
                return r;
            }));

    reg.add({"board.remove", "Remove Elements",
             "Delete elements. Removing a frame or group removes what is inside it; connectors attached to a removed "
             "shape go too.",
             {docParam(), idsParam(), previewParam()}, true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIds(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                for (const auto& id : ids) {
                    if (!unlocked(b, *b.get(id), error)) return Result::failure(error);
                }
                const int n = b.remove(ids);
                refreshGroups(b);
                Result r = edited("removed " + plural(static_cast<std::size_t>(n), "element"), p);
                r.with("removed", static_cast<std::int64_t>(n));
                return r;
            }));

    reg.add({"board.order", "Reorder Elements",
             "Change the stacking order within the layer: front, back, forward (one step up) or backward.",
             {docParam(), idsParam(), {"to", ParamType::String, "where to move them", true, {"front", "back", "forward", "backward"}}, previewParam()},
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIds(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                const std::set<std::string> sel(ids.begin(), ids.end());
                const std::string to = stringParam(p, "to");
                auto& els = b.elements;
                if (to == "front" || to == "back") {
                    std::vector<Element> picked, rest;
                    for (auto& e : els) (sel.count(e.id) ? picked : rest).push_back(e);
                    els = to == "front" ? rest : picked;
                    auto& tail = to == "front" ? picked : rest;
                    els.insert(els.end(), tail.begin(), tail.end());
                } else if (to == "forward") {
                    for (std::size_t i = els.size(); i-- > 1;) {
                        if (sel.count(els[i - 1].id) && !sel.count(els[i].id)) std::swap(els[i - 1], els[i]);
                    }
                } else {
                    for (std::size_t i = 1; i < els.size(); ++i) {
                        if (sel.count(els[i].id) && !sel.count(els[i - 1].id)) std::swap(els[i - 1], els[i]);
                    }
                }
                return edited("moved " + plural(ids.size(), "element") + " " + to, p);
            }));

    reg.add({"board.duplicate", "Duplicate Elements",
             "Copy elements (with what is inside frames and groups, and connectors between copied shapes), shifted "
             "by dx,dy. Returns the new ids in the same order.",
             {docParam(), idsParam(), {"dx", ParamType::Number, "shift right", false, {}, 40.0},
              {"dy", ParamType::Number, "shift down", false, {}, 40.0}, previewParam()},
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIds(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                std::vector<std::string> all = ids;
                for (const auto& id : ids) {
                    for (const auto& d : b.descendants(id)) {
                        if (std::find(all.begin(), all.end(), d) == all.end()) all.push_back(d);
                    }
                }
                std::set<std::string> sel(all.begin(), all.end());
                for (const auto& e : b.elements) {
                    if (e.kind == Kind::Connector && sel.count(e.from) && sel.count(e.to) && !sel.count(e.id)) all.push_back(e.id);
                }
                std::map<std::string, std::string> map;
                std::vector<Element> copies;
                for (const auto& id : all) copies.push_back(*b.get(id));
                for (auto& c : copies) {
                    const std::string old = c.id;
                    c.name.clear();
                    map[old] = b.add(c);
                }
                const double dx = numberParam(p, "dx", 40), dy = numberParam(p, "dy", 40);
                for (const auto& [old, nid] : map) {
                    Element* e = b.get(nid);
                    if (map.count(e->parent)) e->parent = map[e->parent];
                    if (map.count(e->from)) e->from = map[e->from];
                    if (map.count(e->to)) e->to = map[e->to];
                }
                for (const auto& id : ids) {
                    Element* e = b.get(map[id]);
                    moveBy(b, *e, dx, dy);
                }
                std::vector<std::string> out;
                for (const auto& id : ids) out.push_back(map[id]);
                Result r = edited("duplicated " + plural(all.size(), "element") + " as " + joinList(out), p);
                r.value = joinList(out);
                r.with("ids", joinList(out));
                return r;
            }));
}

// ---- connectors, ink, groups ----------------------------------------------------------------------

void registerConnectCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"board.connect", "Connect",
             "Draw a connector that stays attached: from= and to= are shapes (ids or names; comma lists connect "
             "every pair). Routes: straight, elbow (right angles, routed around other shapes) or curved. Arrowheads: "
             "none, arrow (open V), triangle (filled), diamond, circle, bar, cross. Free ends: start=x,y / end=x,y "
             "instead of from/to. waypoints=x1,y1,x2,y2 bends a straight or curved line through points.",
             withStyle({docParam(),
                        {"from", ParamType::String, "start shape(s)", false},
                        {"to", ParamType::String, "end shape(s)", false},
                        {"start", ParamType::NumberList, "free start point x,y", false},
                        {"end", ParamType::NumberList, "free end point x,y", false},
                        {"label", ParamType::String, "text at the middle", false},
                        {"route", ParamType::String, "path style", false, {"straight", "elbow", "curved"}, std::string("straight")},
                        {"start_arrow", ParamType::String, "head at the start", false, arrowChoices(), std::string("none")},
                        {"end_arrow", ParamType::String, "head at the end", false, arrowChoices(), std::string("arrow")},
                        {"from_anchor", ParamType::String, "side to leave from", false, anchorChoices(), std::string("auto")},
                        {"to_anchor", ParamType::String, "side to arrive at", false, anchorChoices(), std::string("auto")},
                        {"waypoints", ParamType::NumberList, "x,y pairs the line passes through", false},
                        {"name", ParamType::String, "a name for the connector", false},
                        {"layer", ParamType::String, "layer (default: the first)", false}}),
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> froms, tos;
                std::string error;
                if (has(p, "from") && !resolveIds(b, stringParam(p, "from"), froms, error)) return Result::failure(error);
                if (has(p, "to") && !resolveIds(b, stringParam(p, "to"), tos, error)) return Result::failure(error);
                const auto sp = listParam(p, "start"), ep = listParam(p, "end");
                if (froms.empty() && sp.size() < 2) return Result::failure("give from= (a shape) or start=x,y");
                if (tos.empty() && ep.size() < 2) return Result::failure("give to= (a shape) or end=x,y");
                if (froms.empty()) froms.push_back("");
                if (tos.empty()) tos.push_back("");
                for (const auto& id : froms) {
                    if (!id.empty() && !isNode(b.get(id)->kind)) return Result::failure("connectors attach to shapes, not " + id);
                }
                for (const auto& id : tos) {
                    if (!id.empty() && !isNode(b.get(id)->kind)) return Result::failure("connectors attach to shapes, not " + id);
                }
                const auto wp = listParam(p, "waypoints");
                std::vector<std::string> made;
                for (const auto& f : froms) {
                    for (const auto& t : tos) {
                        Element c;
                        c.kind = Kind::Connector;
                        c.from = f;
                        c.to = t;
                        if (sp.size() >= 2) c.start = {sp[0], sp[1]};
                        if (ep.size() >= 2) c.end = {ep[0], ep[1]};
                        for (std::size_t i = 0; i + 1 < wp.size(); i += 2) c.waypoints.push_back({wp[i], wp[i + 1]});
                        c.text = stringParam(p, "label");
                        c.route = stringParam(p, "route", "straight");
                        c.startArrow = stringParam(p, "start_arrow", "none");
                        c.endArrow = stringParam(p, "end_arrow", "arrow");
                        c.fromAnchor = stringParam(p, "from_anchor", "auto");
                        c.toAnchor = stringParam(p, "to_anchor", "auto");
                        c.layer = has(p, "layer") ? stringParam(p, "layer") : b.layers.front().name;
                        if (made.empty()) c.name = stringParam(p, "name");
                        if (!applyStyle(c.style, p, error)) return Result::failure(error);
                        made.push_back(b.add(c));
                    }
                }
                Result r = edited(made.size() == 1 ? "connected " + describe(b, *b.get(made[0])) : "added " + plural(made.size(), "connector"), p);
                r.value = joinList(made);
                r.with("id", joinList(made)).with("count", static_cast<std::int64_t>(made.size()));
                return r;
            }));

    reg.add({"board.ink", "Ink Stroke",
             "Draw a freehand pen stroke through points=x1,y1,x2,y2,... (canvas units). pressures= (0..1, one per "
             "point) makes the line thicker where pressed harder; smoothing (0..0.95) evens out shaky input.",
             {docParam(),
              {"points", ParamType::NumberList, "x,y pairs along the stroke", true},
              {"pressures", ParamType::NumberList, "pen pressure per point, 0..1 (default 1)", false},
              {"width", ParamType::Number, "pen width at full pressure", false, {}, 4.0},
              {"color", ParamType::String, "ink colour (default: the theme's text colour)", false},
              {"opacity", ParamType::Number, "0..1", false},
              {"smoothing", ParamType::Number, "0 = raw, 0.5 = default, 0.9 = very smooth", false, {}, 0.5},
              {"layer", ParamType::String, "layer (default: the first)", false},
              {"name", ParamType::String, "a name for the stroke", false},
              previewParam()},
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                const auto pts = listParam(p, "points");
                if (pts.size() < 2) return Result::failure("points needs at least one x,y pair");
                const auto pr = listParam(p, "pressures");
                std::vector<InkPoint> raw;
                for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
                    const std::size_t k = i / 2;
                    raw.push_back({pts[i], pts[i + 1], k < pr.size() ? std::clamp(pr[k], 0.0, 1.0) : 1.0});
                }
                Element e;
                e.kind = Kind::Ink;
                e.ink = smoothInk(raw, numberParam(p, "smoothing", 0.5));
                e.inkWidth = std::clamp(numberParam(p, "width", 4.0), 0.2, 200.0);
                e.layer = has(p, "layer") ? stringParam(p, "layer") : b.layers.front().name;
                e.name = stringParam(p, "name");
                if (has(p, "color")) {
                    if (!validColor(stringParam(p, "color"))) return Result::failure("color isn't a colour");
                    e.style.stroke = stringParam(p, "color");
                }
                if (has(p, "opacity")) e.style.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
                const Box bx = [&] {
                    Box bb;
                    for (const auto& q : e.ink) bb.add(q.x, q.y);
                    return bb;
                }();
                e.x = bx.x0;
                e.y = bx.y0;
                e.w = bx.w();
                e.h = bx.h();
                const std::string id = b.add(e);
                Result r = edited("drew ink " + id + " with " + plural(e.ink.size(), "point"), p);
                r.value = id;
                r.with("id", id).with("points", static_cast<std::int64_t>(e.ink.size()));
                return r;
            }));

    reg.add({"board.group", "Group",
             "Put elements together: kind=group (invisible; they move, align and export as one) or kind=frame (a "
             "titled box drawn around them, like a Miro frame or Mermaid subgraph). Returns the new group/frame id.",
             {docParam(), idsParam("elements to group"),
              {"kind", ParamType::String, "group or frame", false, {"group", "frame"}, std::string("group")},
              {"label", ParamType::String, "frame title", false},
              {"padding", ParamType::Number, "frame: space around the contents", false, {}, 24.0},
              {"name", ParamType::String, "a name for the group", false},
              previewParam()},
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIds(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                Element g;
                g.kind = stringParam(p, "kind", "group") == "frame" ? Kind::Frame : Kind::Group;
                g.text = stringParam(p, "label");
                g.name = stringParam(p, "name");
                const Element* first = b.get(ids.front());
                g.layer = first->layer;
                g.parent = first->parent;  // the group takes the place of its members
                for (const auto& id : ids) {
                    if (b.get(id)->parent != g.parent) g.parent.clear();
                }
                const std::string gid = b.add(g);
                for (const auto& id : ids) b.get(id)->parent = gid;
                if (g.kind == Kind::Frame) {
                    // fit around the members now, then keep it under them
                    Element* f = b.get(gid);
                    Box bx;
                    for (const auto& id : ids) bx.add(elementBox(b, *b.get(id)));
                    const double pad = numberParam(p, "padding", 24);
                    const double title = g.text.empty() ? 0 : resolve(b, *f).fontSize * kLineHeight + 8;
                    f->x = bx.x0 - pad;
                    f->y = bx.y0 - pad - title;
                    f->w = bx.w() + pad * 2;
                    f->h = bx.h() + pad * 2 + title;
                    std::stable_partition(b.elements.begin(), b.elements.end(), [](const Element& x) { return x.kind == Kind::Frame; });
                }
                refreshGroups(b);
                Result r = edited((g.kind == Kind::Frame ? "framed " : "grouped ") + plural(ids.size(), "element") + " as " + gid, p);
                r.value = gid;
                r.with("id", gid);
                return r;
            }));

    reg.add({"board.ungroup", "Ungroup",
             "Dissolve groups or frames: their members stay where they are (frames themselves are removed too unless "
             "keep_frame=true).",
             {docParam(), idsParam("groups or frames"), {"keep_frame", ParamType::Bool, "keep frames as plain boxes", false, {}, false}, previewParam()},
             true},
            boardCommand(session, [](BoardDoc& doc, const Params& p) {
                Board& b = doc.board;
                std::vector<std::string> ids;
                std::string error;
                if (!resolveIds(b, stringParam(p, "ids"), ids, error)) return Result::failure(error);
                int freed = 0;
                std::vector<std::string> gone;
                for (const auto& id : ids) {
                    Element* g = b.get(id);
                    if (g->kind != Kind::Group && g->kind != Kind::Frame) return Result::failure(id + " isn't a group or frame");
                    for (auto& e : b.elements) {
                        if (e.parent == id) {
                            e.parent = g->parent;
                            ++freed;
                        }
                    }
                    if (g->kind == Kind::Group || !boolParam(p, "keep_frame", false)) gone.push_back(id);
                }
                for (const auto& id : gone) {
                    b.elements.erase(std::remove_if(b.elements.begin(), b.elements.end(), [&](const Element& e) { return e.id == id; }), b.elements.end());
                }
                refreshGroups(b);
                return edited("ungrouped " + plural(static_cast<std::size_t>(freed), "element"), p);
            }));
}

}  // namespace

void registerArrangeCommands(Session& session);
void registerDiagramCommands(Session& session);
void registerQueryCommands(Session& session);

namespace detail {
// shared with commands_more.cpp
ActionFactory boardCommandFactory(Session& session, std::function<Result(BoardDoc&, const Params&)> edit, bool undoable) {
    return boardCommand(session, std::move(edit), undoable);
}
bool resolveIdList(const Board& b, const std::string& list, std::vector<std::string>& out, std::string& error) {
    return resolveIds(b, list, out, error);
}
bool unlockedElement(const Board& b, const Element& e, std::string& error) { return unlocked(b, e, error); }
std::string describeElement(const Board& b, const Element& e) { return describe(b, e); }
std::vector<ParamSpec> renderParamSpecs() { return renderParams(); }
RenderOptions renderOptionsFrom(const Params& p) { return renderOptions(p); }
std::unique_ptr<Action> createBoardAction(Session& s, std::function<Result(std::shared_ptr<BoardDoc>&)> make) {
    return std::make_unique<CreateAction>(s, std::move(make));
}
}  // namespace detail

void registerCommands(Session& session) {
    registerFileCommands(session);
    registerShapeCommands(session);
    registerConnectCommands(session);
    registerArrangeCommands(session);
    registerDiagramCommands(session);
    registerQueryCommands(session);
}

}  // namespace wetwaresuite::board
