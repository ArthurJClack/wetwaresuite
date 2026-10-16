// Every blueprint.* command. Edits run inside a DrawingEdit so each one is a
// single undo step; reading commands (info, list, measure, preview, save,
// export) stay out of the history.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>

#include "achclwss/blueprint/blueprint.hpp"
#include "achclwss/raster/codec.hpp"
#include "internal.hpp"

namespace achclwss::blueprint {

using detail::num;

Result DrawingEdit::apply() {
    before_ = std::make_shared<Drawing>(*doc_);
    Result r = fn_(*doc_);
    if (!r.ok) revert();  // failed edits leave the drawing untouched
    return r;
}

void DrawingEdit::revert() {
    if (!before_) return;
    const std::string path = doc_->path;
    *doc_ = *before_;
    doc_->path = path;
}

namespace {

using Edit = std::function<Result(Drawing&, const Params&)>;

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string pt(Vec2 p) { return num(p.x) + "," + num(p.y); }

// A length as the dimension style formats it, with the exact value when that differs ("12'-6\" (12.5)").
std::string both(const Drawing& doc, double v) {
    const std::string f = formatLength(doc, v), raw = num(v);
    return f == raw ? f : f + " (" + raw + ")";
}

// An area as raw drawing units with the friendly form when it adds something ("24868750 (24.87 m²)").
std::string areaBoth(const Drawing& doc, double area) {
    const std::string f = formatArea(doc, area), raw = num(area);
    return f.rfind(raw + " ", 0) == 0 ? f : raw + " (" + f + ")";
}

// Real-world millimetres in the drawing's units (e.g. a 200 mm wall in metres is 0.2).
double realMm(const Drawing& doc, double mm) { return mm / unitToMm(doc.units); }

// ---- param specs ----------------------------------------------------------------

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open blueprint, e.g. bp1", true}; }
ParamSpec previewParam() { return {"preview", ParamType::Bool, "attach a PNG preview of the drawing", false, {}, false}; }
ParamSpec layerParam() {
    return {"layer", ParamType::String, "layer to put it on (default: the current layer; created if missing)", false};
}
ParamSpec numParam(const std::string& name, const std::string& description, bool required = true, Value fallback = {}) {
    return {name, ParamType::Number, description, required, {}, std::move(fallback)};
}
ParamSpec strParam(const std::string& name, const std::string& description, bool required = false, Value fallback = {},
                   std::vector<std::string> choices = {}) {
    return {name, ParamType::String, description, required, std::move(choices), std::move(fallback)};
}
ParamSpec boolParamSpec(const std::string& name, const std::string& description, Value fallback = {}) {
    return {name, ParamType::Bool, description, false, {}, std::move(fallback)};
}
ParamSpec entitiesParam(const std::string& what) {
    return {"entities", ParamType::String,
            what + ": ids like e1,e4, or all, last, layer:NAME, type:wall (combine with commas)", true};
}

std::vector<std::string> paperNames() {
    std::vector<std::string> out;
    for (const auto& p : paperSizes()) out.push_back(p.name);
    return out;
}

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// ---- command plumbing ---------------------------------------------------------

Result& maybePreview(Result& r, const Drawing& doc, const Params& p, bool sheet = false) {
    if (r.ok && boolParam(p, "preview", false)) {
        raster::Image img = render(doc, sheet ? sheetView(doc) : extentsView(doc), sheet ? 1024 : 800, 0, doc.grid.show);
        r.attach({"image/png", raster::encodePng(img), "preview.png"});
    }
    return r;
}

ActionFactory drawingCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<Drawing>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] {
                return Result::failure("no open blueprint called '" + id + "' (use blueprint.new or blueprint.open first)");
            });
        }
        if (!undoable) return std::make_unique<FunctionAction>([doc, edit, p] { return edit(*doc, p); });
        return std::make_unique<DrawingEdit>(doc, [edit, p](Drawing& d) { return edit(d, p); });
    };
}

// Read-only commands that take either an open document (doc=bp1) or a DXF file (file=plan.dxf).
ActionFactory readCommand(Session& session, std::function<Result(Drawing&, const Params&, const std::string&)> fn) {
    return [&session, fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([&session, fn, p] {
            if (has(p, "doc")) {
                const std::string id = stringParam(p, "doc");
                auto doc = session.workspace().getAs<Drawing>(id);
                if (!doc) return Result::failure("no open blueprint called '" + id + "' (use blueprint.new or blueprint.open first)");
                return fn(*doc, p, id);
            }
            if (has(p, "file")) {
                try {
                    auto doc = openDxf(stringParam(p, "file"));
                    return fn(*doc, p, stringParam(p, "file"));
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            }
            return Result::failure("give doc=<id> of an open blueprint or file=<path.dxf>");
        });
    };
}

std::vector<ParamSpec> docOrFile() {
    return {{"doc", ParamType::String, "id of an open blueprint, e.g. bp1 (or use file)", false},
            {"file", ParamType::String, "a .dxf file to read instead of an open document", false}};
}

Vec2 point(const Drawing& doc, const Params& p, const char* x, const char* y) {
    return doc.snap({numberParam(p, x), numberParam(p, y)});
}

bool pointsArg(const Drawing& doc, const Params& p, const char* key, std::size_t minPoints, std::vector<Vec2>& out, std::string& error) {
    NumberList v = listParam(p, key);
    if (v.size() % 2 != 0 || v.size() / 2 < minPoints) {
        error = std::string(key) + " must be x,y pairs (at least " + std::to_string(minPoints) + " points, e.g. 0,0,4000,0,4000,3000)";
        return false;
    }
    for (std::size_t i = 0; i + 1 < v.size(); i += 2) out.push_back(doc.snap({v[i], v[i + 1]}));
    return true;
}

std::string targetLayer(Drawing& doc, const Params& p, std::string& note, std::string& error) {
    std::string name = has(p, "layer") ? stringParam(p, "layer") : doc.current;
    if (name.empty()) name = "0";
    const Layer* l = doc.layer(name);
    if (!l) {
        doc.ensureLayer(name);
        note = " (new layer " + name + ")";
        return name;
    }
    if (l->locked) error = "layer " + l->name + " is locked (blueprint.layer_set name=" + l->name + " locked=false)";
    return l->name;
}

std::string describe(const Drawing& doc, const Entity& e) {
    std::string s = e.id + " " + toString(e.type) + " [" + e.layer + "]";
    switch (e.type) {
        case EntityType::Line:
            if (e.pts.size() >= 2) {
                s += " " + pt(e.pts[0]) + " -> " + pt(e.pts[1]) + " length " + num(distance(e.pts[0], e.pts[1])) +
                     " angle " + formatAngle(normalizeAngle(angleOf(e.pts[1] - e.pts[0])));
            }
            break;
        case EntityType::Polyline:
        case EntityType::Hatch:
        case EntityType::Room:
        case EntityType::Wall: {
            if (e.type == EntityType::Room && !e.name.empty()) s += " \"" + e.name + "\"";
            const bool closed = e.closed || e.type == EntityType::Hatch || e.type == EntityType::Room;
            s += std::string(closed ? " closed" : " open") + " points=";
            for (std::size_t i = 0; i < e.pts.size(); ++i) {
                if (i == 10) {
                    s += " ... (" + std::to_string(e.pts.size()) + " points)";
                    break;
                }
                s += (i ? " " : "") + pt(e.pts[i]);
            }
            if (e.type == EntityType::Wall) {
                s += " thickness " + num(e.thickness) + " length " + num(polylineLength(e.pts, closed));
                if (e.fill != "none") s += " fill " + e.fill;
            } else if (closed) {
                s += " area " + formatArea(doc, polygonArea(e.pts)) + " perimeter " + num(polylineLength(e.pts, true));
            } else {
                s += " length " + num(polylineLength(e.pts, false));
            }
            if (e.type == EntityType::Hatch) s += " pattern " + e.pattern + (e.pattern == "solid" ? "" : " " + formatAngle(e.angle) + " " + num(e.spacing) + "mm");
            break;
        }
        case EntityType::Circle: s += " centre " + pt(e.c) + " radius " + num(e.r); break;
        case EntityType::Arc:
            s += " centre " + pt(e.c) + " radius " + num(e.r) + " from " + formatAngle(e.a0) + " to " + formatAngle(e.a1);
            break;
        case EntityType::Point: s += " at " + pt(e.c); break;
        case EntityType::Text:
            s += " \"" + e.text + "\" at " + pt(e.c) + " height " + num(e.height);
            if (e.rotation != 0) s += " rotation " + formatAngle(e.rotation);
            break;
        case EntityType::Dimension:
            s += std::string(" ") + toString(e.dim);
            if (e.dim == DimKind::Radius || e.dim == DimKind::Diameter) s += " centre " + pt(e.c);
            else if (e.dim == DimKind::Angular && e.pts.size() >= 4) s += " vertex " + pt(e.pts[0]) + " rays to " + pt(e.pts[1]) + " and " + pt(e.pts[2]);
            else if (e.pts.size() >= 2) s += " " + pt(e.pts[0]) + " -> " + pt(e.pts[1]);
            s += " = " + dimensionText(doc, e);
            break;
        case EntityType::Door:
        case EntityType::Window:
            s += " in " + e.host + " at " + num(e.at) + " width " + num(e.width);
            if (e.type == EntityType::Door) s += std::string(" hinge ") + (e.hingeEnd ? "end" : "start") + " swing " + (e.swingRight ? "right" : "left");
            break;
    }
    if (e.color) s += " color " + e.color->hex().substr(0, 7);
    return s;
}

Result added(Drawing& doc, Entity e, const Params& p, const std::string& extra = "") {
    std::string note, error;
    e.layer = targetLayer(doc, p, note, error);
    if (!error.empty()) return Result::failure(error);
    const std::string id = doc.add(std::move(e));
    const Entity& made = doc.entities.back();
    Result r = Result::success("added " + describe(doc, made) + note + extra, id);
    r.with("entity", id).with("layer", made.layer);
    return r;
}

// Resolves an entity selection. Locked entities named by id are an error; in groups they're skipped.
bool select(const Drawing& doc, const std::string& spec, std::vector<int>& out, std::string& error, bool editing, int* skippedLocked = nullptr) {
    std::vector<std::string> tokens;
    std::string item;
    for (char ch : spec + ",") {
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch))) {
            if (!item.empty()) tokens.push_back(item);
            item.clear();
        } else {
            item += ch;
        }
    }
    if (tokens.empty()) {
        error = "no entities given (use ids like e1,e2 or all)";
        return false;
    }
    std::set<int> seen;
    int locked = 0;
    auto take = [&](int i, bool explicitId) {
        const Entity& e = doc.entities[static_cast<std::size_t>(i)];
        const Layer* l = doc.layer(e.layer);
        if (editing && l && l->locked) {
            if (explicitId) {
                error = e.id + " is on locked layer " + l->name;
                return false;
            }
            ++locked;
            return true;
        }
        if (seen.insert(i).second) out.push_back(i);
        return true;
    };
    for (const auto& t : tokens) {
        const std::string lt = lower(t);
        if (lt == "all" || lt == "*") {
            for (std::size_t i = 0; i < doc.entities.size(); ++i) {
                if (!take(static_cast<int>(i), false)) return false;
            }
        } else if (lt == "last") {
            if (doc.entities.empty()) {
                error = "the drawing is empty";
                return false;
            }
            if (!take(static_cast<int>(doc.entities.size()) - 1, true)) return false;
        } else if (lt.rfind("layer:", 0) == 0) {
            const Layer* l = doc.layer(t.substr(6));
            if (!l) {
                error = "no layer '" + t.substr(6) + "'";
                return false;
            }
            for (std::size_t i = 0; i < doc.entities.size(); ++i) {
                if (doc.entities[i].layer == l->name && !take(static_cast<int>(i), false)) return false;
            }
        } else if (lt.rfind("type:", 0) == 0) {
            auto type = parseEntityType(t.substr(5));
            if (!type) {
                error = "unknown entity type '" + t.substr(5) + "'";
                return false;
            }
            for (std::size_t i = 0; i < doc.entities.size(); ++i) {
                if (doc.entities[i].type == *type && !take(static_cast<int>(i), false)) return false;
            }
        } else {
            int i = doc.find(t);
            if (i < 0) {
                error = "no entity '" + t + "' (blueprint.list shows them)";
                return false;
            }
            if (!take(i, true)) return false;
        }
    }
    if (skippedLocked) *skippedLocked = locked;
    if (out.empty()) {
        error = locked ? "every selected entity is on a locked layer" : "the selection is empty";
        return false;
    }
    return true;
}

Box selectionBox(const Drawing& doc, const std::vector<int>& sel) {
    Drawing tmp;
    tmp.units = doc.units;
    tmp.scaleNum = doc.scaleNum;
    tmp.scaleDen = doc.scaleDen;
    tmp.dims = doc.dims;
    tmp.layers = doc.layers;
    for (auto& l : tmp.layers) l.visible = true;
    for (int i : sel) tmp.entities.push_back(doc.entities[static_cast<std::size_t>(i)]);
    // keep hosts so doors explode
    for (const auto& e : doc.entities) {
        if (tmp.find(e.id) < 0 && e.type == EntityType::Wall) {
            for (const auto& s : tmp.entities) {
                if (s.host == e.id) {
                    Entity hidden = e;
                    hidden.layer = "__hidden__";
                    tmp.entities.push_back(hidden);
                    break;
                }
            }
        }
    }
    tmp.layers.push_back(Layer{"__hidden__"});
    tmp.layers.back().visible = false;
    return extents(tmp);
}

// Transforms a selection in place. Doors and windows ride along with their wall.
std::string transformSelection(Drawing& doc, const std::vector<int>& sel, const Xform& xf) {
    std::set<std::string> walls;
    int riders = 0, alone = 0;
    std::set<std::string> selected;
    for (int i : sel) selected.insert(doc.entities[static_cast<std::size_t>(i)].id);
    for (int i : sel) {
        Entity& e = doc.entities[static_cast<std::size_t>(i)];
        if (e.type == EntityType::Door || e.type == EntityType::Window) {
            if (!selected.count(e.host)) ++alone;
            continue;
        }
        transformEntity(e, xf);
        if (e.type == EntityType::Wall) walls.insert(e.id);
    }
    for (auto& e : doc.entities) {
        if ((e.type != EntityType::Door && e.type != EntityType::Window) || !walls.count(e.host)) continue;
        e.at *= xf.scale();
        e.width *= xf.scale();
        if (xf.mirrors()) e.swingRight = !e.swingRight;
        ++riders;
    }
    std::string note;
    if (riders) note += ", " + std::to_string(riders) + " door/window(s) followed their walls";
    if (alone) note += ", " + std::to_string(alone) + " door/window(s) skipped (they move with their wall; use blueprint.set at=...)";
    return note;
}

// Copies a selection (transformed), returning the new ids. Openings in copied walls are copied too.
std::vector<std::string> copySelection(Drawing& doc, const std::vector<int>& sel, const Xform& xf) {
    std::vector<std::string> ids;
    std::vector<Entity> originals;
    for (int i : sel) originals.push_back(doc.entities[static_cast<std::size_t>(i)]);
    const std::vector<Entity> all = doc.entities;
    for (const auto& o : originals) {
        if (o.type == EntityType::Door || o.type == EntityType::Window) continue;
        Entity e = o;
        transformEntity(e, xf);
        const std::string id = doc.add(e);
        ids.push_back(id);
        if (o.type != EntityType::Wall) continue;
        for (const auto& d : all) {
            if ((d.type != EntityType::Door && d.type != EntityType::Window) || d.host != o.id) continue;
            Entity c = d;
            c.host = id;
            c.at *= xf.scale();
            c.width *= xf.scale();
            if (xf.mirrors()) c.swingRight = !c.swingRight;
            ids.push_back(doc.add(c));
        }
    }
    return ids;
}

std::string idList(const std::vector<std::string>& ids) {
    std::string s;
    for (const auto& id : ids) s += (s.empty() ? "" : ",") + id;
    return s;
}

std::string idList(const Drawing& doc, const std::vector<int>& sel) {
    std::vector<std::string> ids;
    for (int i : sel) ids.push_back(doc.entities[static_cast<std::size_t>(i)].id);
    return idList(ids);
}

Vec2 baseOr(const Drawing& doc, const std::vector<int>& sel, const Params& p) {
    if (has(p, "cx") || has(p, "cy")) return {numberParam(p, "cx"), numberParam(p, "cy")};
    Box b = selectionBox(doc, sel);
    if (b.empty) return {};
    return {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
}

// ---- sheet fitting ------------------------------------------------------------

struct Area {
    double x0, y0, x1, y1;
};

Area drawingArea(const Drawing& doc) {
    const double W = doc.paperWidth(), H = doc.paperHeight();
    if (!doc.title.show) return {10, 10, W - 10, H - 10};
    return {20, 50, W - 20, H - 20};
}

void centerOnSheet(Drawing& doc) {
    Box b = extents(doc);
    if (b.empty) return;
    Area a = drawingArea(doc);
    Vec2 mid{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
    Vec2 paperMid{(a.x0 + a.x1) / 2, (a.y0 + a.y1) / 2};
    doc.origin = mid - paperMid / doc.k();
}

// Largest standard scale at which the drawing fits the sheet's drawing area.
void fitScale(Drawing& doc) {
    Box b = extents(doc);
    if (b.empty) return;
    Area a = drawingArea(doc);
    const double needW = b.width() * unitToMm(doc.units), needH = b.height() * unitToMm(doc.units);
    static const double ratios[][2] = {{10, 1}, {5, 1}, {2, 1}, {1, 1}, {1, 2}, {1, 5}, {1, 10}, {1, 20}, {1, 25}, {1, 50}, {1, 75},
                                       {1, 100}, {1, 200}, {1, 250}, {1, 500}, {1, 1000}, {1, 1250}, {1, 2000}, {1, 2500}, {1, 5000},
                                       {1, 10000}, {1, 20000}, {1, 50000}, {1, 100000}};
    for (const auto& r : ratios) {
        const double k = r[0] / r[1];
        if (needW * k <= (a.x1 - a.x0) && needH * k <= (a.y1 - a.y0)) {
            doc.scaleNum = r[0];
            doc.scaleDen = r[1];
            return;
        }
    }
    doc.scaleNum = 1;
    doc.scaleDen = 100000;
}

// Applies paper/orientation/scale/centre params shared by new, open, page and convert.
bool applyPage(Drawing& doc, const Params& p, std::string& error, bool autoCenter) {
    if (has(p, "paper")) {
        auto paper = findPaper(stringParam(p, "paper"));
        if (!paper) {
            error = "unknown paper '" + stringParam(p, "paper") + "' (A4, A3, A2, A1, A0, letter, legal, tabloid)";
            return false;
        }
        doc.paper = paper->name;
    }
    if (has(p, "orientation")) doc.landscape = stringParam(p, "orientation") == "landscape";
    bool fit = false;
    if (has(p, "scale")) {
        const std::string s = lower(stringParam(p, "scale"));
        if (s == "fit" || s == "auto") {
            fit = true;
        } else if (!parseScale(s, doc.scaleNum, doc.scaleDen)) {
            error = "scale must look like 1:50, 1:100, 2:1 or fit";
            return false;
        }
    }
    if (fit) fitScale(doc);
    if (has(p, "origin_x")) doc.origin.x = numberParam(p, "origin_x");
    if (has(p, "origin_y")) doc.origin.y = numberParam(p, "origin_y");
    if (boolParam(p, "center", autoCenter || fit)) centerOnSheet(doc);
    return true;
}

std::vector<ParamSpec> pageParams(bool withDefaults) {
    return {strParam("paper", "paper size", false, withDefaults ? Value{std::string("A3")} : Value{}, paperNames()),
            strParam("orientation", "paper orientation", false, withDefaults ? Value{std::string("landscape")} : Value{}, {"landscape", "portrait"}),
            strParam("scale", "drawing scale paper:real, e.g. 1:50, 1:100, 2:1, or fit to pick the largest standard scale that fits",
                     false, withDefaults ? Value{std::string("1:50")} : Value{})};
}

Result docCreated(Session& session, std::shared_ptr<Drawing> doc, const std::string& verb, const std::string& extra = "") {
    std::string id = session.workspace().add(doc, "bp");
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")" + extra, id);
    r.with("doc", id)
        .with("units", std::string(toString(doc->units)))
        .with("scale", doc->scaleText())
        .with("paper", doc->paper)
        .with("orientation", std::string(doc->landscape ? "landscape" : "portrait"))
        .with("entities", static_cast<std::int64_t>(doc->entities.size()));
    return r;
}

// Closing is undoable: undo puts the drawing back under the same id.
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

bool colorArg(const std::string& text, std::optional<Color>& out, std::string& error) {
    const std::string t = lower(text);
    if (t == "bylayer" || t.empty()) {
        out.reset();
        return true;
    }
    auto c = parseColor(text);
    if (!c) {
        error = "'" + text + "' isn't a colour; use a name (red, blue, gray), #rrggbb, rgb(r,g,b) or an AutoCAD colour number 1..255";
        return false;
    }
    out = *c;
    return true;
}

int segmentAt(const Entity& e, Vec2 p) {
    int best = 0;
    double bestD = -1;
    const std::size_t n = e.pts.size();
    const std::size_t segs = e.closed ? n : n - 1;
    for (std::size_t i = 0; i < segs; ++i) {
        Vec2 a = e.pts[i], b = e.pts[(i + 1) % n];
        double len2 = dot(b - a, b - a);
        double t = len2 > 0 ? std::clamp(dot(p - a, b - a) / len2, 0.0, 1.0) : 0;
        double d = distance(p, a + (b - a) * t);
        if (bestD < 0 || d < bestD) {
            bestD = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

bool inside(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if ((poly[i].y > p.y) != (poly[j].y > p.y) &&
            p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) {
            in = !in;
        }
    }
    return in;
}

bool lineIntersection(Vec2 a1, Vec2 a2, Vec2 b1, Vec2 b2, Vec2& out) {
    Vec2 da = a2 - a1, db = b2 - b1;
    double den = cross(da, db);
    if (std::abs(den) < 1e-12) return false;
    out = a1 + da * (cross(b1 - a1, db) / den);
    return true;
}

// The entity a single id names, for commands that need exactly one.
const Entity* one(const Drawing& doc, const std::string& id, std::string& error) {
    int i = doc.find(id);
    if (i < 0) {
        error = "no entity '" + id + "' (blueprint.list shows them)";
        return nullptr;
    }
    return &doc.entities[static_cast<std::size_t>(i)];
}

// Checks an opening fits in its wall and doesn't overlap the others.
bool placeOpening(const Drawing& doc, const Entity& wall, Entity& o, std::string& error) {
    if (!(o.width > 0)) {
        error = "width must be positive";
        return false;
    }
    if (openingSegment(wall, o.at, o.width) < 0) {
        error = "a " + formatLength(doc, o.width) + " opening centred " + formatLength(doc, o.at) + " along " + wall.id +
                " doesn't fit inside one straight run of the wall (keep it clear of corners by half the wall thickness; the wall is " +
                formatLength(doc, polylineLength(wall.pts, wall.closed)) + " long)";
        return false;
    }
    for (const auto& other : doc.entities) {
        if (other.id == o.id || other.host != wall.id) continue;
        if (std::abs(other.at - o.at) < (other.width + o.width) / 2 - 1e-9) {
            error = "that overlaps " + other.id + " (" + toString(other.type) + " at " + formatLength(doc, other.at) + ")";
            return false;
        }
    }
    return true;
}

// Whole numbers in results become integers so the CLI prints 24868750, not 2.48688e+07.
Value tidyNumber(const Value& v) {
    if (auto d = std::get_if<double>(&v)) {
        const double r = std::round(*d);
        if (std::isfinite(*d) && std::abs(r) < 1e15 && std::abs(*d - r) <= 1e-9 * std::max(1.0, std::abs(*d)))
            return static_cast<std::int64_t>(r);
    }
    return v;
}

class TidyAction : public Action {
public:
    explicit TidyAction(std::unique_ptr<Action> inner) : inner_(std::move(inner)) {}
    Result apply() override {
        Result r = inner_->apply();
        r.value = tidyNumber(r.value);
        for (auto& field : r.data) field.second = tidyNumber(field.second);
        return r;
    }
    void revert() override { inner_->revert(); }

private:
    std::unique_ptr<Action> inner_;
};

class TidyRegistry {
public:
    explicit TidyRegistry(CommandRegistry& reg) : reg_(reg) {}
    void add(CommandInfo info, ActionFactory factory) {
        reg_.add(std::move(info), [factory = std::move(factory)](const Params& p) -> std::unique_ptr<Action> {
            auto action = factory(p);
            if (!action) return action;
            return std::make_unique<TidyAction>(std::move(action));
        });
    }

private:
    CommandRegistry& reg_;
};

}  // namespace

void registerCommands(Session& session) {
    TidyRegistry reg(session.registry());

    // ---- documents ---------------------------------------------------------------

    reg.add({"blueprint.new", "New Blueprint",
             "Create a new technical drawing / floor plan and return its id (e.g. bp1). Coordinates are real-world drawing "
             "units (x right, y up, like CAD); the scale (1:50) maps them onto the paper for export and printing.",
             join({strParam("units", "drawing units", false, std::string("mm"), {"mm", "cm", "m", "in", "ft"})}, join(pageParams(true),
                  {strParam("title", "title block: drawing title"), strParam("author", "title block: drawn by"),
                   strParam("date", "title block: date, e.g. 2026-10-16")})),
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto doc = std::make_shared<Drawing>();
                    doc->units = parseUnit(stringParam(p, "units", "mm")).value_or(Unit::Mm);
                    std::string error;
                    if (!applyPage(*doc, p, error, false)) return Result::failure(error);
                    doc->origin = Vec2{-25, -52} / doc->k();
                    doc->title.title = stringParam(p, "title");
                    doc->title.author = stringParam(p, "author");
                    doc->title.date = stringParam(p, "date");
                    return docCreated(session, doc, "created");
                });
            });

    reg.add({"blueprint.open", "Open DXF",
             "Open a DXF drawing (LINE, LWPOLYLINE, POLYLINE, CIRCLE, ARC, TEXT, MTEXT, POINT, DIMENSION, HATCH, SOLID; layers "
             "with colours and line types). Blocks/INSERT, splines and ellipses are skipped and reported. A DXF saved by this "
             "tool comes back with its walls, doors, rooms, sheet and title block; any other DXF is fitted to the paper.",
             join({{"path", ParamType::String, "path to a .dxf file", true},
                    strParam("units", "override the drawing units (default: from the file's $INSUNITS, else mm)", false, {},
                             {"mm", "cm", "m", "in", "ft"})},
                  join(pageParams(false), {boolParamSpec("center", "centre the drawing on the paper")})),
             false},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        std::string notes;
                        bool sheetInfo = false;
                        auto doc = openDxf(stringParam(p, "path"), &notes, &sheetInfo);
                        if (has(p, "units")) doc->units = parseUnit(stringParam(p, "units")).value_or(doc->units);
                        Params page = p;
                        if (!sheetInfo && !has(p, "scale")) page["scale"] = std::string("fit");
                        std::string error;
                        if (!applyPage(*doc, page, error, !sheetInfo)) return Result::failure(error);
                        return docCreated(session, doc, "opened", notes.empty() ? "" : "; " + notes);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"blueprint.save", "Save DXF",
             "Save the drawing as DXF (AutoCAD 2000 ASCII, opens in LibreCAD, FreeCAD, AutoCAD, QCAD...). Dimensions are real "
             "DIMENSION entities; walls, doors, windows and rooms are drawn as lines/arcs/text and stay editable when reopened "
             "here. sheet=true also writes the border and title block on layer BP_SHEET.",
             {docParam(), {"path", ParamType::String, "output .dxf file", true},
              boolParamSpec("sheet", "include the border frame and title block", false)},
             false},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                std::filesystem::path fp(path);
                if (lower(fp.extension().string()) != ".dxf") {
                    return Result::failure("blueprint.save writes .dxf; use blueprint.export for .svg, .pdf or .png");
                }
                try {
                    std::ofstream out(fp, std::ios::binary);
                    if (!out) return Result::failure("cannot write " + path);
                    out << toDxf(doc, boolParam(p, "sheet", false));
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
                doc.path = path;
                return Result::success("saved " + path + " (" + std::to_string(doc.entities.size()) + " entities)", path).with("path", path);
            }, false));

    reg.add({"blueprint.export", "Export Sheet",
             "Export the whole sheet (drawing at scale, border, title block) to .pdf (vector, prints to scale at 100%), .svg "
             "(real mm size), or .png/.jpg/.bmp (rendered at dpi).",
             {docParam(), {"path", ParamType::String, "output file; the extension picks the format", true},
              numParam("dpi", "resolution for raster formats", false, 150.0),
              {"quality", ParamType::Int, "JPEG quality 1..100", false, {}, std::int64_t{90}}},
             false},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                const std::string path = stringParam(p, "path");
                const double dpi = numberParam(p, "dpi", 150);
                if (!(dpi >= 10 && dpi <= 1200)) return Result::failure("dpi must be 10..1200");
                try {
                    exportFile(doc, path, dpi, static_cast<int>(std::clamp<std::int64_t>(intParam(p, "quality", 90), 1, 100)));
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
                return Result::success("exported " + path + " (" + doc.paper + (doc.landscape ? " landscape" : " portrait") + " at " +
                                           doc.scaleText() + ")",
                                       path)
                    .with("path", path);
            }, false));

    reg.add({"blueprint.convert", "Convert DXF",
             "One step: read a .dxf and write .pdf, .svg, .png or .dxf, e.g. input=plan.dxf output=plan.pdf paper=A3 scale=1:100. "
             "Without scale a foreign DXF is fitted to the paper; a DXF saved here keeps its own sheet settings.",
             join({{"input", ParamType::String, "a .dxf file", true}, {"output", ParamType::String, "output file; the extension picks the format", true}},
                  join(pageParams(false), {boolParamSpec("center", "centre the drawing on the paper"), numParam("dpi", "resolution for raster formats", false, 150.0),
                                           strParam("title", "title block title")})),
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    try {
                        std::string notes;
                        bool sheetInfo = false;
                        auto doc = openDxf(stringParam(p, "input"), &notes, &sheetInfo);
                        Params page = p;
                        if (!sheetInfo && !has(p, "scale")) page["scale"] = std::string("fit");
                        std::string error;
                        if (!applyPage(*doc, page, error, !sheetInfo)) return Result::failure(error);
                        if (has(p, "title")) doc->title.title = stringParam(p, "title");
                        const std::string output = stringParam(p, "output");
                        exportFile(*doc, output, numberParam(p, "dpi", 150));
                        return Result::success("converted " + stringParam(p, "input") + " to " + output + " (" + doc->paper + " at " +
                                                   doc->scaleText() + ", " + std::to_string(doc->entities.size()) + " entities" +
                                                   (notes.empty() ? "" : "; " + notes) + ")",
                                               output)
                            .with("path", output)
                            .with("scale", doc->scaleText());
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"blueprint.close", "Close Blueprint", "Close a blueprint (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<Drawing>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open blueprint called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"blueprint.info", "Blueprint Info",
             "Describe a drawing: units, scale, paper, layers (with colour, weight, line type, visible/locked), entity counts, "
             "extents, grid/snap and title block. Works on an open doc or straight from a .dxf file.",
             docOrFile(), false},
            readCommand(session, [](Drawing& doc, const Params&, const std::string& name) {
                std::string text = name + ": " + doc.paper + (doc.landscape ? " landscape" : " portrait") + " (" + num(doc.paperWidth()) +
                                   " x " + num(doc.paperHeight()) + " mm), scale " + doc.scaleText() + ", units " + toString(doc.units);
                text += "\nthe paper shows " + formatLength(doc, doc.paperWidth() / doc.k()) + " x " + formatLength(doc, doc.paperHeight() / doc.k()) +
                        " " + toString(doc.units) + "; drawing origin at paper bottom-left is " + pt(doc.origin);
                text += "\nlayers:";
                for (const auto& l : doc.layers) {
                    text += "\n  " + l.name + (l.name == doc.current ? " (current)" : "") + " color " + l.color.hex().substr(0, 7) + " weight " +
                            num(l.weight) + "mm " + toString(l.linetype) + (l.visible ? "" : " hidden") + (l.locked ? " locked" : "");
                }
                std::map<std::string, int> counts;
                for (const auto& e : doc.entities) ++counts[toString(e.type)];
                text += "\nentities: " + std::to_string(doc.entities.size());
                std::string countText;
                for (const auto& [type, n] : counts) countText += (countText.empty() ? "" : ", ") + std::to_string(n) + " " + type;
                if (!countText.empty()) text += " (" + countText + ")";
                Box b = extents(doc);
                NumberList ext;
                if (!b.empty) {
                    text += "\nextents: " + num(b.x0) + "," + num(b.y0) + " to " + num(b.x1) + "," + num(b.y1) + " (" + num(b.width()) + " x " +
                            num(b.height()) + ")";
                    ext = {b.x0, b.y0, b.x1, b.y1};
                }
                text += "\ngrid " + (doc.grid.spacing > 0 ? num(doc.grid.spacing) : std::string("off")) + ", snap " +
                        (doc.grid.snap > 0 ? num(doc.grid.snap) : std::string("off"));
                text += "\ntitle block " + std::string(doc.title.show ? "on" : "off") + ": \"" + doc.title.title + "\" by " +
                        (doc.title.author.empty() ? "-" : doc.title.author) + ", " + (doc.title.date.empty() ? "-" : doc.title.date) +
                        ", sheet " + doc.title.sheet;
                Result r = Result::success(text);
                r.with("doc", name)
                    .with("units", std::string(toString(doc.units)))
                    .with("scale", doc.scaleText())
                    .with("paper", doc.paper)
                    .with("orientation", std::string(doc.landscape ? "landscape" : "portrait"))
                    .with("entities", static_cast<std::int64_t>(doc.entities.size()))
                    .with("layers", static_cast<std::int64_t>(doc.layers.size()))
                    .with("current_layer", doc.current);
                if (!ext.empty()) r.with("extents", ext);
                return r;
            }));

    reg.add({"blueprint.preview", "Preview Blueprint",
             "Render the drawing to a PNG so you can see it. view=extents zooms to the drawing; view=sheet shows the whole "
             "paper with border and title block.",
             {docParam(), {"size", ParamType::Int, "longest side in px", false, {}, std::int64_t{1024}},
              strParam("view", "what to show", false, std::string("extents"), {"extents", "sheet"}),
              boolParamSpec("grid", "draw the grid (default: the drawing's grid setting)")},
             false},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                const int size = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "size", 1024), 64, 4096));
                const bool sheet = stringParam(p, "view", "extents") == "sheet";
                raster::Image img = render(doc, sheet ? sheetView(doc) : extentsView(doc), size, 0, boolParam(p, "grid", doc.grid.show));
                Result r = Result::success("preview " + std::to_string(img.width()) + "x" + std::to_string(img.height()));
                r.attach({"image/png", raster::encodePng(img), "preview.png"});
                return r;
            }, false));

    reg.add({"blueprint.page", "Page Setup",
             "Change the sheet: paper size, orientation, drawing scale (or scale=fit), where the drawing sits on the paper "
             "(origin_x/origin_y = drawing coordinates at the paper's bottom-left corner, or center=true), and the border.",
             join(pageParams(false), {docParam(), numParam("origin_x", "drawing x at the paper's left edge", false),
                                      numParam("origin_y", "drawing y at the paper's bottom edge", false),
                                      boolParamSpec("center", "centre the drawing in the space above the title block"), previewParam()}),
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::string error;
                if (!applyPage(doc, p, error, false)) return Result::failure(error);
                Box b = extents(doc);
                std::string fits;
                if (!b.empty) {
                    Vec2 lo = doc.toPaper({b.x0, b.y0}), hi = doc.toPaper({b.x1, b.y1});
                    const bool ok = lo.x >= 0 && lo.y >= 0 && hi.x <= doc.paperWidth() && hi.y <= doc.paperHeight();
                    fits = ok ? ", drawing fits on the paper" : ", warning: the drawing runs off the paper (try scale=fit or center=true)";
                }
                Result r = Result::success("page " + doc.paper + (doc.landscape ? " landscape" : " portrait") + " at " + doc.scaleText() +
                                           ", origin " + pt(doc.origin) + fits);
                r.with("scale", doc.scaleText()).with("paper", doc.paper);
                return maybePreview(r, doc, p, true);
            }));

    reg.add({"blueprint.titleblock", "Title Block",
             "Fill in the title block printed in the sheet's bottom-right corner (and the border frame). Only given fields change; "
             "show=false removes the border and title block.",
             {docParam(), strParam("title", "drawing title"), strParam("author", "drawn by"), strParam("date", "date"),
              strParam("sheet", "sheet number, e.g. 1/3 or A-101"), strParam("scale", "text for the scale cell (default: the drawing scale)"),
              boolParamSpec("show", "show the border and title block"), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                TitleBlock& t = doc.title;
                if (has(p, "title")) t.title = stringParam(p, "title");
                if (has(p, "author")) t.author = stringParam(p, "author");
                if (has(p, "date")) t.date = stringParam(p, "date");
                if (has(p, "sheet")) t.sheet = stringParam(p, "sheet");
                if (has(p, "scale")) t.scale = stringParam(p, "scale");
                if (has(p, "show")) t.show = boolParam(p, "show", true);
                Result r = Result::success("title block " + std::string(t.show ? "on" : "off") + ": \"" + t.title + "\", " + t.author + ", " +
                                           t.date + ", sheet " + t.sheet + ", scale " + (t.scale.empty() ? doc.scaleText() : t.scale));
                return maybePreview(r, doc, p, true);
            }));

    reg.add({"blueprint.dimstyle", "Dimension Style",
             "How dimensions look: text_height and arrow_size in paper mm (they stay the same size on paper at any scale), "
             "arrows (arrow, tick for architectural, dot), precision (decimals, -1 = by unit), suffix (append mm/m/\").",
             {docParam(), numParam("text_height", "text height on paper, mm", false), numParam("arrow_size", "arrowhead size on paper, mm", false),
              strParam("arrows", "arrowhead style", false, {}, {"arrow", "tick", "dot"}),
              {"precision", ParamType::Int, "decimal places (-1 = mm 0, cm 1, m 2, in 2, ft as feet-inches)", false},
              boolParamSpec("suffix", "append the unit to values"), numParam("ext_offset", "gap at the measured point, paper mm", false),
              numParam("ext_beyond", "extension past the dimension line, paper mm", false), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                DimStyle& d = doc.dims;
                if (has(p, "text_height")) d.textHeight = numberParam(p, "text_height");
                if (has(p, "arrow_size")) d.arrowSize = numberParam(p, "arrow_size");
                if (has(p, "arrows")) d.arrows = stringParam(p, "arrows");
                if (has(p, "precision")) d.precision = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "precision"), -1, 8));
                if (has(p, "suffix")) d.suffix = boolParam(p, "suffix");
                if (has(p, "ext_offset")) d.extOffset = numberParam(p, "ext_offset");
                if (has(p, "ext_beyond")) d.extBeyond = numberParam(p, "ext_beyond");
                if (!(d.textHeight > 0) || !(d.arrowSize > 0)) return Result::failure("text_height and arrow_size must be positive");
                Result r = Result::success("dimension style: text " + num(d.textHeight) + "mm, " + d.arrows + " " + num(d.arrowSize) +
                                           "mm, precision " + std::to_string(d.precision) + (d.suffix ? ", with units" : ""));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.grid", "Grid and Snap",
             "Set the grid (shown in previews) and snap: with snap > 0 every coordinate you give to drawing commands is rounded "
             "to the nearest multiple of it.",
             {docParam(), numParam("spacing", "grid spacing in drawing units (0 = off)", false), numParam("snap", "snap step in drawing units (0 = off)", false),
              boolParamSpec("show", "show the grid in previews"), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                if (has(p, "spacing")) doc.grid.spacing = std::max(0.0, numberParam(p, "spacing"));
                if (has(p, "snap")) doc.grid.snap = std::max(0.0, numberParam(p, "snap"));
                if (has(p, "show")) doc.grid.show = boolParam(p, "show");
                else if (has(p, "spacing")) doc.grid.show = doc.grid.spacing > 0;
                Result r = Result::success("grid " + (doc.grid.spacing > 0 ? num(doc.grid.spacing) : std::string("off")) +
                                           (doc.grid.show ? " (shown)" : "") + ", snap " + (doc.grid.snap > 0 ? num(doc.grid.snap) : std::string("off")));
                return maybePreview(r, doc, p);
            }));

    // ---- layers ---------------------------------------------------------------------

    auto layerStyle = [](Layer& l, const Params& p, std::string& error) {
        if (has(p, "color")) {
            std::optional<Color> c;
            if (!colorArg(stringParam(p, "color"), c, error)) return false;
            l.color = c.value_or(Color{0, 0, 0, 255});
        }
        if (has(p, "weight")) {
            double w = numberParam(p, "weight");
            if (!(w >= 0 && w <= 5)) {
                error = "weight is the pen width on paper in mm, 0..5 (0.25 is typical, 0.5 for walls)";
                return false;
            }
            l.weight = w;
        }
        if (has(p, "linetype")) l.linetype = parseLineType(stringParam(p, "linetype")).value_or(LineType::Continuous);
        if (has(p, "visible")) l.visible = boolParam(p, "visible", true);
        if (has(p, "locked")) l.locked = boolParam(p, "locked", false);
        return true;
    };
    auto layerText = [](const Layer& l) {
        return l.name + ": color " + l.color.hex().substr(0, 7) + ", weight " + num(l.weight) + "mm, " + toString(l.linetype) +
               (l.visible ? "" : ", hidden") + (l.locked ? ", locked" : "");
    };
    const std::vector<std::string> linetypes = {"continuous", "dashed", "center", "hidden"};

    reg.add({"blueprint.layer_add", "Add Layer",
             "Add a layer with a colour, pen weight (mm on paper) and line type. current=true makes new entities go on it.",
             {docParam(), {"name", ParamType::String, "layer name, e.g. WALLS", true},
              strParam("color", "colour: name, #rrggbb or AutoCAD colour number", false, std::string("black")),
              numParam("weight", "pen width on paper, mm", false, 0.25),
              strParam("linetype", "line type", false, std::string("continuous"), linetypes),
              boolParamSpec("current", "make it the current layer", false)},
             true},
            drawingCommand(session, [layerStyle, layerText](Drawing& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (name.empty()) return Result::failure("the layer needs a name");
                if (doc.layer(name)) return Result::failure("layer " + name + " already exists (use blueprint.layer_set)");
                Layer l{name};
                std::string error;
                if (!layerStyle(l, p, error)) return Result::failure(error);
                doc.layers.push_back(l);
                if (boolParam(p, "current", false)) doc.current = name;
                return Result::success("added layer " + layerText(l) + (doc.current == name ? " (current)" : ""), name).with("layer", name);
            }));

    reg.add({"blueprint.layer_set", "Layer Settings",
             "Change a layer's colour, weight, line type, visibility or lock, or rename it. Hidden layers don't print; "
             "entities on locked layers can't be edited.",
             {docParam(), {"name", ParamType::String, "layer name", true}, strParam("color", "colour"), numParam("weight", "pen width on paper, mm", false),
              strParam("linetype", "line type", false, {}, linetypes), boolParamSpec("visible", "show the layer"),
              boolParamSpec("locked", "protect its entities from edits"), strParam("rename", "new name"), previewParam()},
             true},
            drawingCommand(session, [layerStyle, layerText](Drawing& doc, const Params& p) {
                Layer* l = doc.layer(stringParam(p, "name"));
                if (!l) return Result::failure("no layer '" + stringParam(p, "name") + "' (blueprint.layers lists them)");
                std::string error;
                if (!layerStyle(*l, p, error)) return Result::failure(error);
                if (has(p, "rename")) {
                    const std::string to = stringParam(p, "rename");
                    if (to.empty() || (doc.layer(to) && doc.layer(to) != l)) return Result::failure("can't rename to '" + to + "'");
                    for (auto& e : doc.entities) {
                        if (e.layer == l->name) e.layer = to;
                    }
                    if (doc.current == l->name) doc.current = to;
                    l->name = to;
                }
                Result r = Result::success("layer " + layerText(*l), l->name);
                r.with("layer", l->name);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.layer_current", "Current Layer", "Choose the layer new entities go on (created if missing).",
             {docParam(), {"name", ParamType::String, "layer name", true}}, true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                const std::string name = stringParam(p, "name");
                if (name.empty()) return Result::failure("the layer needs a name");
                const bool existed = doc.layer(name) != nullptr;
                doc.current = doc.ensureLayer(name).name;
                return Result::success("current layer " + doc.current + (existed ? "" : " (new)"), doc.current).with("layer", doc.current);
            }));

    reg.add({"blueprint.layers", "List Layers", "List the layers with their colour, weight, line type and state.", {docParam()}, false},
            drawingCommand(session, [layerText](Drawing& doc, const Params&) {
                std::string text = std::to_string(doc.layers.size()) + " layers (current " + doc.current + ")";
                std::string names;
                for (const auto& l : doc.layers) {
                    int n = 0;
                    for (const auto& e : doc.entities) n += e.layer == l.name;
                    text += "\n" + layerText(l) + ", " + std::to_string(n) + " entities";
                    names += (names.empty() ? "" : ",") + l.name;
                }
                return Result::success(text).with("layers", names).with("current", doc.current);
            }, false));

    // ---- drawing entities ---------------------------------------------------------

    reg.add({"blueprint.line", "Line",
             "Draw a straight line from x1,y1 to x2,y2, or from x1,y1 with a length and angle (degrees counter-clockwise from +x).",
             {docParam(), numParam("x1", "start x"), numParam("y1", "start y"), numParam("x2", "end x", false), numParam("y2", "end y", false),
              numParam("length", "length (instead of x2,y2)", false), numParam("angle", "direction in degrees, with length", false, 0.0), layerParam(),
              previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity e;
                e.type = EntityType::Line;
                Vec2 a = point(doc, p, "x1", "y1"), b;
                if (has(p, "x2") || has(p, "y2")) {
                    if (!has(p, "x2") || !has(p, "y2")) return Result::failure("give both x2 and y2 (or length and angle)");
                    b = point(doc, p, "x2", "y2");
                } else if (has(p, "length")) {
                    b = a + direction(numberParam(p, "angle", 0)) * numberParam(p, "length");
                } else {
                    return Result::failure("give x2,y2 or length (and angle)");
                }
                if (distance(a, b) <= 0) return Result::failure("the line has zero length");
                e.pts = {a, b};
                Result r = added(doc, e, p);
                if (r.ok) r.with("length", distance(a, b));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.polyline", "Polyline", "Draw a connected line through points (x,y,x,y,...); closed=true joins the last point to the first.",
             {docParam(), {"points", ParamType::NumberList, "vertices as x,y,x,y,...", true}, boolParamSpec("closed", "close the shape", false),
              layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity e;
                e.type = EntityType::Polyline;
                std::string error;
                if (!pointsArg(doc, p, "points", 2, e.pts, error)) return Result::failure(error);
                e.closed = boolParam(p, "closed", false) && e.pts.size() > 2;
                Result r = added(doc, e, p);
                if (r.ok) {
                    r.with("length", polylineLength(e.pts, e.closed));
                    if (e.closed) r.with("area", std::abs(polygonArea(e.pts)));
                }
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.rect", "Rectangle", "Draw a rectangle (a closed polyline) with its corner at x,y.",
             {docParam(), numParam("x", "corner x"), numParam("y", "corner y"), numParam("width", "size along x"), numParam("height", "size along y"),
              layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Vec2 a = point(doc, p, "x", "y");
                double w = numberParam(p, "width"), h = numberParam(p, "height");
                if (w == 0 || h == 0) return Result::failure("width and height can't be 0");
                Vec2 b = doc.snap(a + Vec2{w, h});
                Entity e;
                e.type = EntityType::Polyline;
                e.closed = true;
                double x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                e.pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
                Result r = added(doc, e, p);
                if (r.ok) r.with("area", (x1 - x0) * (y1 - y0)).with("perimeter", 2 * ((x1 - x0) + (y1 - y0)));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.circle", "Circle", "Draw a circle by centre and radius (or diameter).",
             {docParam(), numParam("cx", "centre x"), numParam("cy", "centre y"), numParam("radius", "radius", false),
              numParam("diameter", "diameter (instead of radius)", false), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity e;
                e.type = EntityType::Circle;
                e.c = point(doc, p, "cx", "cy");
                e.r = has(p, "radius") ? numberParam(p, "radius") : numberParam(p, "diameter") / 2;
                if (!(e.r > 0)) return Result::failure("give a positive radius or diameter");
                Result r = added(doc, e, p);
                if (r.ok) r.with("area", kPi * e.r * e.r).with("circumference", 2 * kPi * e.r);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.arc", "Arc", "Draw an arc by centre and radius, counter-clockwise from start to end angle (degrees from +x).",
             {docParam(), numParam("cx", "centre x"), numParam("cy", "centre y"), numParam("radius", "radius"), numParam("start", "start angle, degrees"),
              numParam("end", "end angle, degrees"), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity e;
                e.type = EntityType::Arc;
                e.c = point(doc, p, "cx", "cy");
                e.r = numberParam(p, "radius");
                e.a0 = normalizeAngle(numberParam(p, "start"));
                e.a1 = normalizeAngle(numberParam(p, "end"));
                if (!(e.r > 0)) return Result::failure("radius must be positive");
                Result r = added(doc, e, p);
                double sweep = normalizeAngle(e.a1 - e.a0);
                if (sweep == 0) sweep = 360;
                if (r.ok) r.with("length", radians(sweep) * e.r);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.text", "Text",
             "Place a line of text. height is the capital-letter height in drawing units (default: 3.5 mm on paper at the "
             "current scale); align/valign say which point of the text sits at x,y.",
             {docParam(), numParam("x", "x of the alignment point"), numParam("y", "y of the alignment point"),
              {"text", ParamType::String, "the text", true}, numParam("height", "text height in drawing units", false),
              numParam("rotation", "degrees counter-clockwise", false, 0.0),
              strParam("align", "horizontal alignment", false, std::string("left"), {"left", "center", "right"}),
              strParam("valign", "vertical alignment", false, std::string("baseline"), {"baseline", "middle", "top"}), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity e;
                e.type = EntityType::Text;
                e.c = point(doc, p, "x", "y");
                e.text = stringParam(p, "text");
                if (e.text.empty()) return Result::failure("text can't be empty");
                e.height = has(p, "height") ? numberParam(p, "height") : 3.5 / doc.k();
                if (!(e.height > 0)) return Result::failure("height must be positive");
                e.rotation = numberParam(p, "rotation", 0);
                const std::string a = stringParam(p, "align", "left"), v = stringParam(p, "valign", "baseline");
                e.halign = a == "center" ? HAlign::Center : a == "right" ? HAlign::Right : HAlign::Left;
                e.valign = v == "middle" ? VAlign::Middle : v == "top" ? VAlign::Top : VAlign::Baseline;
                Result r = added(doc, e, p);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.point", "Point", "Place a point marker (a reference or survey point).",
             {docParam(), numParam("x", "x"), numParam("y", "y"), layerParam(), previewParam()}, true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity e;
                e.type = EntityType::Point;
                e.c = point(doc, p, "x", "y");
                Result r = added(doc, e, p);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.dimension", "Dimension",
             "Add a dimension whose value is measured from the geometry and formatted in the drawing units. "
             "aligned/horizontal/vertical: measure x1,y1 to x2,y2 (or a line: entity=e3), dimension line through x,y or "
             "offset away (default 10 mm on paper, positive = left of the direction / above / right). radius/diameter: "
             "entity=<circle or arc> or cx,cy,radius, leader at angle. angular: vertex cx,cy and points x1,y1 and x2,y2 on "
             "the two rays (or entity=e1,e2 for two lines), arc through x,y or at offset radius. text overrides the label "
             "(<> stands for the value).",
             {docParam(), strParam("kind", "what to measure", false, std::string("aligned"), {"aligned", "horizontal", "vertical", "radius", "diameter", "angular"}),
              numParam("x1", "first point x", false), numParam("y1", "first point y", false), numParam("x2", "second point x", false),
              numParam("y2", "second point y", false), numParam("x", "a point the dimension line (or arc) passes through, x", false),
              numParam("y", "that point's y", false), numParam("offset", "distance of the dimension line from the points (drawing units)", false),
              strParam("entity", "entity to measure: a line, a circle/arc, or two lines e1,e2 for angular"),
              numParam("cx", "centre (radius/diameter) or vertex (angular) x", false), numParam("cy", "centre or vertex y", false),
              numParam("radius", "radius when there's no entity", false), numParam("angle", "leader angle for radius/diameter, degrees", false, 45.0),
              strParam("text", "label override; <> is replaced by the measured value"), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity d;
                d.type = EntityType::Dimension;
                d.dim = parseDimKind(stringParam(p, "kind", "aligned")).value_or(DimKind::Aligned);
                d.text = stringParam(p, "text");
                std::string error;
                const double defaultOffset = 10 / doc.k();
                std::vector<const Entity*> refs;
                if (has(p, "entity")) {
                    std::vector<int> sel;
                    if (!select(doc, stringParam(p, "entity"), sel, error, false)) return Result::failure(error);
                    for (int i : sel) refs.push_back(&doc.entities[static_cast<std::size_t>(i)]);
                }
                if (d.dim == DimKind::Aligned || d.dim == DimKind::Horizontal || d.dim == DimKind::Vertical) {
                    Vec2 p1, p2;
                    if (!refs.empty()) {
                        if (refs[0]->type != EntityType::Line) return Result::failure("entity must be a line for a linear dimension");
                        p1 = refs[0]->pts[0];
                        p2 = refs[0]->pts[1];
                    } else {
                        if (!has(p, "x1") || !has(p, "y1") || !has(p, "x2") || !has(p, "y2")) return Result::failure("give x1,y1,x2,y2 (or entity=<line>)");
                        p1 = point(doc, p, "x1", "y1");
                        p2 = point(doc, p, "x2", "y2");
                    }
                    Vec2 loc;
                    if (has(p, "x") && has(p, "y")) {
                        loc = point(doc, p, "x", "y");
                    } else {
                        const double off = numberParam(p, "offset", defaultOffset);
                        if (d.dim == DimKind::Horizontal) loc = {(p1.x + p2.x) / 2, (off >= 0 ? std::max(p1.y, p2.y) : std::min(p1.y, p2.y)) + off};
                        else if (d.dim == DimKind::Vertical) loc = {(off >= 0 ? std::max(p1.x, p2.x) : std::min(p1.x, p2.x)) + off, (p1.y + p2.y) / 2};
                        else loc = (p1 + p2) / 2 + perp(normalized(p2 - p1)) * off;
                    }
                    d.pts = {p1, p2, loc};
                    if (dimensionValue(d) <= 0) return Result::failure("the two points don't differ in the measured direction");
                } else if (d.dim == DimKind::Radius || d.dim == DimKind::Diameter) {
                    if (!refs.empty()) {
                        if (refs[0]->type != EntityType::Circle && refs[0]->type != EntityType::Arc) return Result::failure("entity must be a circle or arc");
                        d.c = refs[0]->c;
                        d.r = refs[0]->r;
                    } else {
                        if (!has(p, "cx") || !has(p, "cy") || !has(p, "radius")) return Result::failure("give entity=<circle> or cx, cy and radius");
                        d.c = point(doc, p, "cx", "cy");
                        d.r = numberParam(p, "radius");
                    }
                    if (!(d.r > 0)) return Result::failure("radius must be positive");
                    d.rotation = normalizeAngle(numberParam(p, "angle", 45));
                    if (!refs.empty() && refs[0]->type == EntityType::Arc && !has(p, "angle")) {
                        const Entity& a = *refs[0];
                        d.rotation = normalizeAngle(a.a0 + normalizeAngle(a.a1 - a.a0) / 2);
                    }
                } else {
                    Vec2 v, p1, p2;
                    if (refs.size() >= 2) {
                        const Entity &l1 = *refs[0], &l2 = *refs[1];
                        if (l1.type != EntityType::Line || l2.type != EntityType::Line) return Result::failure("angular entity= needs two lines");
                        if (!lineIntersection(l1.pts[0], l1.pts[1], l2.pts[0], l2.pts[1], v)) return Result::failure("those lines are parallel");
                        p1 = distance(v, l1.pts[0]) > distance(v, l1.pts[1]) ? l1.pts[0] : l1.pts[1];
                        p2 = distance(v, l2.pts[0]) > distance(v, l2.pts[1]) ? l2.pts[0] : l2.pts[1];
                    } else {
                        if (!has(p, "cx") || !has(p, "cy") || !has(p, "x1") || !has(p, "y1") || !has(p, "x2") || !has(p, "y2")) {
                            return Result::failure("give the vertex cx,cy and a point on each ray x1,y1 and x2,y2 (or entity=e1,e2)");
                        }
                        v = point(doc, p, "cx", "cy");
                        p1 = point(doc, p, "x1", "y1");
                        p2 = point(doc, p, "x2", "y2");
                    }
                    if (distance(v, p1) <= 0 || distance(v, p2) <= 0) return Result::failure("the ray points must differ from the vertex");
                    Vec2 loc;
                    if (has(p, "x") && has(p, "y")) {
                        loc = point(doc, p, "x", "y");
                    } else {
                        // without a location, measure the angle under 180 degrees between the rays
                        const double a1 = angleOf(p1 - v), sweep = normalizeAngle(angleOf(p2 - v) - a1);
                        const double mid = sweep <= 180 ? a1 + sweep / 2 : a1 - (360 - sweep) / 2;
                        const double rad = numberParam(p, "offset", 0.6 * std::min(distance(v, p1), distance(v, p2)));
                        loc = v + direction(mid) * std::abs(rad);
                    }
                    d.pts = {v, p1, p2, loc};
                }
                const double value = dimensionValue(d);
                const std::string label = dimensionText(doc, d);
                Result r = added(doc, d, p);
                if (r.ok) r.with("value", value).with("text", label);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.hatch", "Hatch",
             "Fill a closed area: points=x,y,... or entity=<closed polyline, rectangle, room or circle>. pattern solid, lines "
             "(at angle, spacing in paper mm) or cross.",
             {docParam(), {"points", ParamType::NumberList, "boundary as x,y,x,y,...", false}, strParam("entity", "a closed shape to fill"),
              strParam("pattern", "fill pattern", false, std::string("lines"), {"solid", "lines", "cross"}),
              numParam("angle", "line angle, degrees", false, 45.0), numParam("spacing", "line spacing on paper, mm", false, 2.0), layerParam(),
              previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity h;
                h.type = EntityType::Hatch;
                h.closed = true;
                std::string error;
                if (has(p, "entity")) {
                    const Entity* src = one(doc, stringParam(p, "entity"), error);
                    if (!src) return Result::failure(error);
                    if (src->type == EntityType::Circle) {
                        for (int i = 0; i < 72; ++i) h.pts.push_back(src->c + direction(i * 5.0) * src->r);
                    } else if ((src->type == EntityType::Polyline && src->closed) || src->type == EntityType::Room || src->type == EntityType::Hatch) {
                        h.pts = src->pts;
                    } else {
                        return Result::failure(src->id + " isn't a closed shape (use a closed polyline, rect, room or circle)");
                    }
                } else if (!pointsArg(doc, p, "points", 3, h.pts, error)) {
                    return Result::failure("give points=x,y,... (3 or more) or entity=<closed shape>");
                }
                h.pattern = stringParam(p, "pattern", "lines");
                h.angle = numberParam(p, "angle", 45);
                h.spacing = numberParam(p, "spacing", 2);
                if (!(h.spacing > 0.1)) return Result::failure("spacing must be more than 0.1 mm");
                Result r = added(doc, h, p);
                if (r.ok) r.with("area", std::abs(polygonArea(h.pts)));
                return maybePreview(r, doc, p);
            }));

    // ---- floor-plan helpers -------------------------------------------------------

    reg.add({"blueprint.wall", "Wall",
             "Draw a wall along points (x,y,...) with a thickness (default 200 mm), as two parallel faces with mitred corners. "
             "closed=true runs it all the way round (an outer wall). justify says whether the points are the wall's centre "
             "line, its left face or its right face (left/right as you walk from point to point). Doors and windows cut "
             "openings into it.",
             {docParam(), {"points", ParamType::NumberList, "path as x,y,x,y,...", true}, numParam("thickness", "wall thickness (default 200 mm)", false),
              boolParamSpec("closed", "join the end back to the start", false),
              strParam("justify", "what the points trace", false, std::string("center"), {"center", "left", "right"}),
              strParam("fill", "fill between the faces", false, std::string("none"), {"none", "solid", "lines"}), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity w;
                w.type = EntityType::Wall;
                std::string error;
                if (!pointsArg(doc, p, "points", 2, w.pts, error)) return Result::failure(error);
                w.closed = boolParam(p, "closed", false) && w.pts.size() > 2;
                if (w.closed && distance(w.pts.front(), w.pts.back()) < 1e-9) w.pts.pop_back();
                w.thickness = has(p, "thickness") ? numberParam(p, "thickness") : realMm(doc, 200);
                if (!(w.thickness > 0)) return Result::failure("thickness must be positive");
                w.fill = stringParam(p, "fill", "none");
                const std::string just = stringParam(p, "justify", "center");
                if (just != "center") w.pts = offsetPolyline(w.pts, w.closed, (just == "left" ? -1 : 1) * w.thickness / 2);
                Result r = added(doc, w, p);
                if (r.ok) r.with("length", polylineLength(w.pts, w.closed));
                return maybePreview(r, doc, p);
            }));

    auto openingCommand = [&session](EntityType type) {
        return drawingCommand(session, [type](Drawing& doc, const Params& p) {
            std::string error;
            const Entity* wall = one(doc, stringParam(p, "wall"), error);
            if (!wall) return Result::failure(error);
            if (wall->type != EntityType::Wall) return Result::failure(wall->id + " is a " + toString(wall->type) + ", not a wall");
            Entity o;
            o.type = type;
            o.host = wall->id;
            o.width = has(p, "width") ? numberParam(p, "width") : realMm(doc, type == EntityType::Door ? 900 : 1200);
            if (has(p, "at")) o.at = numberParam(p, "at");
            else if (has(p, "x") && has(p, "y")) o.at = wallDistanceAt(*wall, point(doc, p, "x", "y"));
            else return Result::failure("say where: at=<distance along the wall to the opening's centre> or x,y near the wall");
            if (type == EntityType::Door) {
                o.hingeEnd = stringParam(p, "hinge", "start") == "end";
                o.swingRight = stringParam(p, "side", "left") == "right";
            }
            if (!placeOpening(doc, *wall, o, error)) return Result::failure(error);
            Result r = added(doc, o, p);
            return maybePreview(r, doc, p);
        });
    };

    reg.add({"blueprint.door", "Door",
             "Put a door in a wall: an opening of width (default 900 mm) centred at distance `at` along the wall's centre line "
             "from its first point (or nearest to x,y), with the leaf and its 90-degree swing arc. hinge start/end picks the "
             "jamb nearer the wall's start or end; side left/right picks which side of the wall it swings into.",
             {docParam(), {"wall", ParamType::String, "id of the wall", true}, numParam("at", "distance along the wall to the opening's centre", false),
              numParam("x", "or: a point near the wall, x", false), numParam("y", "and y", false), numParam("width", "opening width (default 900 mm)", false),
              strParam("hinge", "hinge jamb", false, std::string("start"), {"start", "end"}),
              strParam("side", "swing side, looking along the wall from its start", false, std::string("left"), {"left", "right"}), layerParam(),
              previewParam()},
             true},
            openingCommand(EntityType::Door));

    reg.add({"blueprint.window", "Window",
             "Put a window in a wall: an opening of width (default 1200 mm) centred at distance `at` along the wall (or nearest "
             "to x,y), drawn with sill and glazing lines.",
             {docParam(), {"wall", ParamType::String, "id of the wall", true}, numParam("at", "distance along the wall to the opening's centre", false),
              numParam("x", "or: a point near the wall, x", false), numParam("y", "and y", false), numParam("width", "opening width (default 1200 mm)", false),
              layerParam(), previewParam()},
             true},
            openingCommand(EntityType::Window));

    reg.add({"blueprint.room", "Room",
             "Mark a room: a closed polygon (points=x,y,... or wall=<closed wall> to use its inside face) labelled with its name "
             "and its floor area (m² for metric, ft² for imperial), which updates when the room changes.",
             {docParam(), {"points", ParamType::NumberList, "corners as x,y,x,y,...", false}, strParam("wall", "a closed wall whose inside face is the room"),
              strParam("name", "room name, e.g. Kitchen"), boolParamSpec("area", "show the area in the label", true),
              boolParamSpec("outline", "draw the room's outline", true), numParam("label_x", "label position x (default: the centre)", false),
              numParam("label_y", "label position y", false), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Entity room;
                room.type = EntityType::Room;
                room.closed = true;
                std::string error;
                if (has(p, "wall")) {
                    const Entity* wall = one(doc, stringParam(p, "wall"), error);
                    if (!wall) return Result::failure(error);
                    if (wall->type != EntityType::Wall || !wall->closed) return Result::failure(wall->id + " isn't a closed wall");
                    auto a = wallFace(*wall, 1), b = wallFace(*wall, -1);
                    room.pts = std::abs(polygonArea(a)) < std::abs(polygonArea(b)) ? a : b;
                } else if (!pointsArg(doc, p, "points", 3, room.pts, error)) {
                    return Result::failure("give points=x,y,... (3 or more corners) or wall=<closed wall>");
                }
                room.name = stringParam(p, "name");
                room.showArea = boolParam(p, "area", true);
                room.outline = boolParam(p, "outline", true);
                room.c = has(p, "label_x") && has(p, "label_y") ? Vec2{numberParam(p, "label_x"), numberParam(p, "label_y")} : polygonCentroid(room.pts);
                const double area = std::abs(polygonArea(room.pts));
                Result r = added(doc, room, p);
                if (r.ok) {
                    r.with("area", area).with("area_text", formatArea(doc, area)).with("perimeter", polylineLength(room.pts, true));
                }
                return maybePreview(r, doc, p);
            }));

    // ---- editing --------------------------------------------------------------------

    reg.add({"blueprint.move", "Move", "Move entities by dx, dy (drawing units). Doors and windows move with their walls.",
             {docParam(), entitiesParam("what to move"), numParam("dx", "distance along x", false, 0.0), numParam("dy", "distance along y", false, 0.0),
              previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::vector<int> sel;
                std::string error;
                int locked = 0;
                if (!select(doc, stringParam(p, "entities"), sel, error, true, &locked)) return Result::failure(error);
                const double dx = numberParam(p, "dx", 0), dy = numberParam(p, "dy", 0);
                std::string note = transformSelection(doc, sel, Xform::translate(dx, dy));
                if (locked) note += ", " + std::to_string(locked) + " on locked layers left alone";
                Result r = Result::success("moved " + idList(doc, sel) + " by " + num(dx) + "," + num(dy) + note, idList(doc, sel));
                r.with("entities", idList(doc, sel)).with("count", static_cast<std::int64_t>(sel.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.copy", "Copy",
             "Copy entities, offset by dx, dy; count > 1 makes a row of copies (an array) at dx, 2dx, ... Returns the new ids. "
             "Copying a wall copies its doors and windows.",
             {docParam(), entitiesParam("what to copy"), numParam("dx", "offset along x", false, 0.0), numParam("dy", "offset along y", false, 0.0),
              {"count", ParamType::Int, "number of copies", false, {}, std::int64_t{1}}, previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities"), sel, error, false)) return Result::failure(error);
                const auto count = std::clamp<std::int64_t>(intParam(p, "count", 1), 1, 1000);
                const double dx = numberParam(p, "dx", 0), dy = numberParam(p, "dy", 0);
                std::vector<std::string> ids;
                for (std::int64_t i = 1; i <= count; ++i) {
                    auto more = copySelection(doc, sel, Xform::translate(dx * static_cast<double>(i), dy * static_cast<double>(i)));
                    ids.insert(ids.end(), more.begin(), more.end());
                }
                if (ids.empty()) return Result::failure("nothing to copy (doors and windows are copied with their wall)");
                Result r = Result::success("copied as " + idList(ids), idList(ids));
                r.with("entities", idList(ids)).with("count", static_cast<std::int64_t>(ids.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.rotate", "Rotate",
             "Rotate entities counter-clockwise by angle degrees around cx,cy (default: the centre of the selection).",
             {docParam(), entitiesParam("what to rotate"), numParam("angle", "degrees counter-clockwise"), numParam("cx", "pivot x", false),
              numParam("cy", "pivot y", false), boolParamSpec("copy", "rotate a copy and keep the original", false), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities"), sel, error, !boolParam(p, "copy", false))) return Result::failure(error);
                Vec2 c = baseOr(doc, sel, p);
                Xform xf = Xform::rotate(c, numberParam(p, "angle"));
                std::string ids = idList(doc, sel), note;
                if (boolParam(p, "copy", false)) ids = idList(copySelection(doc, sel, xf));
                else note = transformSelection(doc, sel, xf);
                Result r = Result::success("rotated " + ids + " by " + formatAngle(numberParam(p, "angle")) + " around " + pt(c) + note, ids);
                r.with("entities", ids);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.scale", "Scale",
             "Scale entities by factor around cx,cy (default: the centre of the selection). Wall thickness, text height and "
             "door widths scale too.",
             {docParam(), entitiesParam("what to scale"), numParam("factor", "scale factor, e.g. 2 or 0.5"), numParam("cx", "base point x", false),
              numParam("cy", "base point y", false), boolParamSpec("copy", "scale a copy and keep the original", false), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                const double f = numberParam(p, "factor");
                if (!(f > 0)) return Result::failure("factor must be positive");
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities"), sel, error, !boolParam(p, "copy", false))) return Result::failure(error);
                Vec2 c = baseOr(doc, sel, p);
                Xform xf = Xform::scaleAbout(c, f);
                std::string ids = idList(doc, sel), note;
                if (boolParam(p, "copy", false)) ids = idList(copySelection(doc, sel, xf));
                else note = transformSelection(doc, sel, xf);
                Result r = Result::success("scaled " + ids + " by " + num(f) + " around " + pt(c) + note, ids);
                r.with("entities", ids);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.mirror", "Mirror",
             "Mirror entities across the line through x1,y1 and x2,y2. copy=true keeps the originals (CAD's default for "
             "symmetric plans). Text stays readable.",
             {docParam(), entitiesParam("what to mirror"), numParam("x1", "mirror line point 1 x"), numParam("y1", "point 1 y"),
              numParam("x2", "mirror line point 2 x"), numParam("y2", "point 2 y"), boolParamSpec("copy", "keep the originals", false), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                Vec2 a = point(doc, p, "x1", "y1"), b = point(doc, p, "x2", "y2");
                if (distance(a, b) <= 0) return Result::failure("the mirror line needs two different points");
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities"), sel, error, !boolParam(p, "copy", false))) return Result::failure(error);
                Xform xf = Xform::mirror(a, b);
                std::string ids = idList(doc, sel), note;
                if (boolParam(p, "copy", false)) ids = idList(copySelection(doc, sel, xf));
                else note = transformSelection(doc, sel, xf);
                Result r = Result::success("mirrored " + ids + note, ids);
                r.with("entities", ids);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.offset", "Offset",
             "Make a parallel copy of a line, polyline, circle or arc at a distance. Pick the side with a point x,y on that "
             "side, or by sign: positive = left of a line's direction, outside a closed shape or circle.",
             {docParam(), {"entity", ParamType::String, "id of a line, polyline, circle or arc", true}, numParam("distance", "offset distance"),
              numParam("x", "a point on the side to offset to, x", false), numParam("y", "and y", false), layerParam(), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::string error;
                const Entity* src = one(doc, stringParam(p, "entity"), error);
                if (!src) return Result::failure(error);
                double d = numberParam(p, "distance");
                if (d == 0) return Result::failure("distance can't be 0");
                const bool side = has(p, "x") && has(p, "y");
                Vec2 sp{numberParam(p, "x"), numberParam(p, "y")};
                Entity e = *src;
                e.id.clear();
                switch (src->type) {
                    case EntityType::Line: {
                        Vec2 u = normalized(src->pts[1] - src->pts[0]);
                        if (side) d = std::abs(d) * (cross(u, sp - src->pts[0]) >= 0 ? 1 : -1);
                        for (auto& q : e.pts) q = q + perp(u) * d;
                        break;
                    }
                    case EntityType::Polyline: {
                        if (src->closed) {
                            const double orient = polygonArea(src->pts) > 0 ? -1 : 1;  // left of a CCW outline is inside
                            if (side) d = std::abs(d) * (inside(src->pts, sp) ? -1 : 1);
                            e.pts = offsetPolyline(src->pts, true, d * orient);
                        } else {
                            if (side) {
                                int seg = segmentAt(*src, sp);
                                Vec2 a = src->pts[static_cast<std::size_t>(seg)], b = src->pts[static_cast<std::size_t>(seg) + 1];
                                d = std::abs(d) * (cross(b - a, sp - a) >= 0 ? 1 : -1);
                            }
                            e.pts = offsetPolyline(src->pts, false, d);
                        }
                        break;
                    }
                    case EntityType::Circle:
                    case EntityType::Arc:
                        if (side) d = std::abs(d) * (distance(src->c, sp) < src->r ? -1 : 1);
                        e.r = src->r + d;
                        if (!(e.r > 0)) return Result::failure("that offset would make the radius zero or negative");
                        break;
                    default: return Result::failure("offset works on lines, polylines, circles and arcs (" + src->id + " is a " + toString(src->type) + ")");
                }
                Params q = p;
                if (!has(p, "layer")) q["layer"] = src->layer;
                Result r = added(doc, e, q);
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.delete", "Delete", "Delete entities. Deleting a wall deletes its doors and windows.",
             {docParam(), entitiesParam("what to delete"), previewParam()}, true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities"), sel, error, true)) return Result::failure(error);
                std::set<std::string> gone;
                for (int i : sel) gone.insert(doc.entities[static_cast<std::size_t>(i)].id);
                for (const auto& e : doc.entities) {
                    if (gone.count(e.host)) gone.insert(e.id);
                }
                std::vector<std::string> ids;
                for (const auto& e : doc.entities) {
                    if (gone.count(e.id)) ids.push_back(e.id);
                }
                doc.entities.erase(std::remove_if(doc.entities.begin(), doc.entities.end(), [&](const Entity& e) { return gone.count(e.id) > 0; }),
                                   doc.entities.end());
                Result r = Result::success("deleted " + idList(ids), idList(ids));
                r.with("entities", idList(ids)).with("count", static_cast<std::int64_t>(ids.size()));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.set", "Change Entity",
             "Change properties of entities: layer, color (or bylayer), text, height, rotation (text), name/area/outline "
             "(rooms), at/width/hinge/side (doors and windows), thickness/fill (walls), pattern/angle/spacing (hatches), "
             "closed (polylines). Only the given properties change.",
             {docParam(), entitiesParam("what to change"), strParam("layer", "move to this layer (created if missing)"),
              strParam("color", "colour, or bylayer"), strParam("text", "new text (text, or a dimension's label override; empty = measured)"),
              numParam("height", "text height", false), numParam("rotation", "text rotation, degrees", false), strParam("name", "room name"),
              boolParamSpec("area", "rooms: show the area"), boolParamSpec("outline", "rooms: draw the outline"),
              numParam("at", "door/window: distance along the wall", false), numParam("width", "door/window: opening width", false),
              strParam("hinge", "door hinge", false, {}, {"start", "end"}), strParam("side", "door swing side", false, {}, {"left", "right"}),
              numParam("thickness", "wall thickness", false), strParam("fill", "wall fill", false, {}, {"none", "solid", "lines"}),
              strParam("pattern", "hatch pattern", false, {}, {"solid", "lines", "cross"}), numParam("angle", "hatch angle", false),
              numParam("spacing", "hatch spacing, paper mm", false), boolParamSpec("closed", "polyline: closed"), previewParam()},
             true},
            drawingCommand(session, [](Drawing& doc, const Params& p) {
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities"), sel, error, true)) return Result::failure(error);
                std::string note;
                if (has(p, "layer")) {
                    const std::string name = targetLayer(doc, p, note, error);
                    if (!error.empty()) return Result::failure(error);
                    for (int i : sel) doc.entities[static_cast<std::size_t>(i)].layer = name;
                }
                for (int i : sel) {
                    Entity& e = doc.entities[static_cast<std::size_t>(i)];
                    if (has(p, "color") && !colorArg(stringParam(p, "color"), e.color, error)) return Result::failure(error);
                    if (has(p, "text") && (e.type == EntityType::Text || e.type == EntityType::Dimension)) e.text = stringParam(p, "text");
                    if (has(p, "height") && e.type == EntityType::Text) e.height = numberParam(p, "height");
                    if (has(p, "rotation") && e.type == EntityType::Text) e.rotation = numberParam(p, "rotation");
                    if (e.type == EntityType::Room) {
                        if (has(p, "name")) e.name = stringParam(p, "name");
                        if (has(p, "area")) e.showArea = boolParam(p, "area");
                        if (has(p, "outline")) e.outline = boolParam(p, "outline");
                    }
                    if (e.type == EntityType::Door || e.type == EntityType::Window) {
                        Entity o = e;
                        if (has(p, "at")) o.at = numberParam(p, "at");
                        if (has(p, "width")) o.width = numberParam(p, "width");
                        if (has(p, "hinge")) o.hingeEnd = stringParam(p, "hinge") == "end";
                        if (has(p, "side")) o.swingRight = stringParam(p, "side") == "right";
                        const Entity* wall = one(doc, e.host, error);
                        if (!wall || !placeOpening(doc, *wall, o, error)) return Result::failure(e.id + ": " + error);
                        e = o;
                    }
                    if (e.type == EntityType::Wall) {
                        if (has(p, "thickness")) e.thickness = numberParam(p, "thickness");
                        if (has(p, "fill")) e.fill = stringParam(p, "fill");
                        if (!(e.thickness > 0)) return Result::failure("thickness must be positive");
                    }
                    if (e.type == EntityType::Hatch) {
                        if (has(p, "pattern")) e.pattern = stringParam(p, "pattern");
                        if (has(p, "angle")) e.angle = numberParam(p, "angle");
                        if (has(p, "spacing")) e.spacing = std::max(0.1, numberParam(p, "spacing"));
                    }
                    if (has(p, "closed") && e.type == EntityType::Polyline) e.closed = boolParam(p, "closed") && e.pts.size() > 2;
                    if (e.type == EntityType::Text && !(e.height > 0)) return Result::failure("height must be positive");
                }
                std::string text = "changed " + idList(doc, sel) + note;
                for (int i : sel) text += "\n" + describe(doc, doc.entities[static_cast<std::size_t>(i)]);
                Result r = Result::success(text, idList(doc, sel));
                r.with("entities", idList(doc, sel));
                return maybePreview(r, doc, p);
            }));

    reg.add({"blueprint.list", "List Entities",
             "List entities with their exact geometry (ids, layer, coordinates, lengths, areas, measured dimension values). "
             "Filter with entities=layer:WALLS, type:door, ids... Works on an open doc or straight from a .dxf file.",
             join(docOrFile(), {strParam("entities", "which ones (default all): ids, all, layer:NAME, type:TYPE", false, std::string("all"))}),
             false},
            readCommand(session, [](Drawing& doc, const Params& p, const std::string& name) {
                if (doc.entities.empty()) return Result::success(name + ": no entities").with("count", std::int64_t{0}).with("ids", std::string());
                std::vector<int> sel;
                std::string error;
                if (!select(doc, stringParam(p, "entities", "all"), sel, error, false)) return Result::failure(error);
                std::string text = std::to_string(sel.size()) + " of " + std::to_string(doc.entities.size()) + " entities in " + name + ":";
                for (int i : sel) text += "\n" + describe(doc, doc.entities[static_cast<std::size_t>(i)]);
                Result r = Result::success(text);
                r.with("count", static_cast<std::int64_t>(sel.size())).with("ids", idList(doc, sel));
                return r;
            }));

    reg.add({"blueprint.measure", "Measure",
             "Measure without drawing anything. entity=<id>: its length, or area and perimeter of a closed shape/room, radius "
             "of a circle, value of a dimension. entity=e1,e2 (two lines): the angle between them. points=x1,y1,x2,y2: distance "
             "and direction; three or more points: area and perimeter of that polygon, or kind=angle for the angle at the "
             "middle point. Works on an open doc or straight from a .dxf file.",
             join(docOrFile(), {strParam("entity", "entity id, or two line ids for an angle"),
                                {"points", ParamType::NumberList, "x,y,x,y,... to measure between", false},
                                strParam("kind", "what to measure from points", false, std::string("auto"), {"auto", "distance", "area", "angle"})}),
             false},
            readCommand(session, [](Drawing& doc, const Params& p, const std::string&) {
                std::string error;
                if (has(p, "entity")) {
                    std::vector<int> sel;
                    if (!select(doc, stringParam(p, "entity"), sel, error, false)) return Result::failure(error);
                    if (sel.size() >= 2) {
                        const Entity &a = doc.entities[static_cast<std::size_t>(sel[0])], &b = doc.entities[static_cast<std::size_t>(sel[1])];
                        if (a.type != EntityType::Line || b.type != EntityType::Line) return Result::failure("an angle needs two lines");
                        Vec2 u = normalized(a.pts[1] - a.pts[0]), v = normalized(b.pts[1] - b.pts[0]);
                        double ang = degrees(std::acos(std::clamp(dot(u, v), -1.0, 1.0)));
                        return Result::success("angle between " + a.id + " and " + b.id + ": " + formatAngle(ang) + " (" + formatAngle(180 - ang) + " the other way)", ang)
                            .with("angle", ang);
                    }
                    const Entity& e = doc.entities[static_cast<std::size_t>(sel[0])];
                    Result r = Result::success(describe(doc, e));
                    switch (e.type) {
                        case EntityType::Line: {
                            const double len = distance(e.pts[0], e.pts[1]);
                            r.message = e.id + " length " + both(doc, len) + ", angle " +
                                        formatAngle(normalizeAngle(angleOf(e.pts[1] - e.pts[0])));
                            r.value = len;
                            r.with("length", len).with("angle", normalizeAngle(angleOf(e.pts[1] - e.pts[0])));
                            break;
                        }
                        case EntityType::Polyline:
                        case EntityType::Hatch:
                        case EntityType::Room:
                        case EntityType::Wall: {
                            const bool closed = e.closed || e.type == EntityType::Hatch || e.type == EntityType::Room;
                            const double len = polylineLength(e.pts, closed);
                            if (closed && e.type != EntityType::Wall) {
                                const double area = std::abs(polygonArea(e.pts));
                                r.message = e.id + " area " + areaBoth(doc, area) + ", perimeter " + both(doc, len);
                                r.value = area;
                                r.with("area", area).with("area_text", formatArea(doc, area)).with("perimeter", len);
                            } else {
                                r.message = e.id + (e.type == EntityType::Wall ? " centre-line length " : " length ") + both(doc, len);
                                r.value = len;
                                r.with("length", len);
                                if (e.type == EntityType::Wall) r.with("thickness", e.thickness);
                            }
                            break;
                        }
                        case EntityType::Circle: {
                            const double area = kPi * e.r * e.r;
                            r.message = e.id + " radius " + num(e.r) + ", diameter " + num(2 * e.r) + ", circumference " + num(2 * kPi * e.r) +
                                        ", area " + areaBoth(doc, area);
                            r.value = e.r;
                            r.with("radius", e.r).with("diameter", 2 * e.r).with("circumference", 2 * kPi * e.r).with("area", area);
                            break;
                        }
                        case EntityType::Arc: {
                            double sweep = normalizeAngle(e.a1 - e.a0);
                            if (sweep == 0) sweep = 360;
                            const double len = radians(sweep) * e.r;
                            r.message = e.id + " radius " + num(e.r) + ", sweep " + formatAngle(sweep) + ", arc length " + num(len);
                            r.value = len;
                            r.with("radius", e.r).with("angle", sweep).with("length", len);
                            break;
                        }
                        case EntityType::Dimension: {
                            const double v = dimensionValue(e);
                            r.message = e.id + " measures " + dimensionText(doc, e) + " (" + num(v) + ")";
                            r.value = v;
                            r.with("value", v).with("text", dimensionText(doc, e));
                            break;
                        }
                        case EntityType::Door:
                        case EntityType::Window:
                            r.value = e.width;
                            r.with("width", e.width).with("at", e.at);
                            break;
                        default: break;
                    }
                    return r;
                }
                std::vector<Vec2> pts;
                if (!pointsArg(doc, p, "points", 2, pts, error)) return Result::failure("give entity=<id> or points=x1,y1,x2,y2,...");
                std::string kind = stringParam(p, "kind", "auto");
                if (kind == "auto") kind = pts.size() == 2 ? "distance" : "area";
                if (kind == "distance") {
                    const double len = polylineLength(pts, false);
                    Vec2 d = pts.back() - pts.front();
                    Result r = Result::success("distance " + both(doc, len) + ", dx " + num(d.x) + ", dy " + num(d.y) +
                                                   ", angle " + formatAngle(normalizeAngle(angleOf(d))),
                                               len);
                    r.with("distance", len).with("dx", d.x).with("dy", d.y).with("angle", normalizeAngle(angleOf(d)));
                    return r;
                }
                if (pts.size() < 3) return Result::failure(kind + " needs at least 3 points");
                if (kind == "angle") {
                    Vec2 u = normalized(pts[0] - pts[1]), v = normalized(pts[2] - pts[1]);
                    double ang = degrees(std::acos(std::clamp(dot(u, v), -1.0, 1.0)));
                    return Result::success("angle at " + pt(pts[1]) + ": " + formatAngle(ang), ang).with("angle", ang);
                }
                const double area = std::abs(polygonArea(pts)), per = polylineLength(pts, true);
                Result r = Result::success("area " + areaBoth(doc, area) + ", perimeter " + both(doc, per), area);
                r.with("area", area).with("area_text", formatArea(doc, area)).with("perimeter", per);
                return r;
            }));
}

}  // namespace achclwss::blueprint
