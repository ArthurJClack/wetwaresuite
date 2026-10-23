// Organising commands: layers, groups, locking, moving between groups,
// stacking order, transforms of several shapes, align and distribute,
// repeats, and hit testing for selection.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {


ParamSpec shapesParam() { return {"shapes", ParamType::String, "several shapes, comma separated (instead of shape)", false}; }
ParamSpec oneShapeParam() { return {"shape", ParamType::String, "shape id or name", false}; }

Bounds unionBounds(const VectorDoc& doc, const std::vector<int>& idx) {
    Bounds b;
    for (int i : idx) b.add(shapeBounds(doc, i));
    return b;
}

void layerTree(const VectorDoc& doc, const std::string& parent, int depth, std::string& text, std::string& ids) {
    for (int i : doc.children(parent)) {
        const Shape& s = doc.shapes[static_cast<std::size_t>(i)];
        if (s.kind != ShapeKind::Group || !s.layer) continue;
        int count = 0;
        for (int k : doc.children(s.id)) count += doc.shapes[static_cast<std::size_t>(k)].layer ? 0 : 1;
        text += "\n" + std::string(static_cast<std::size_t>(depth) * 2, ' ') + s.id + " \"" + s.name + "\" " + std::to_string(count) +
                (count == 1 ? " item" : " items") + (s.visible ? "" : " hidden") + (s.locked ? " locked" : "") +
                (doc.currentLayer == s.id ? " (current)" : "");
        ids += (ids.empty() ? "" : ",") + s.id;
        layerTree(doc, s.id, depth + 1, text, ids);
    }
}

Matrix about(const Matrix& m, Point c) { return Matrix::translate(c.x, c.y) * m * Matrix::translate(-c.x, -c.y); }

// Moves the shape's subtree right after the last sibling's subtree (front) or before the first's (back), or one step.
void restack(VectorDoc& doc, int idx, const std::string& to) {
    const Shape s = doc.shapes[static_cast<std::size_t>(idx)];
    std::vector<int> sibs = doc.children(s.parent);
    const auto me = std::find(sibs.begin(), sibs.end(), idx);
    const std::size_t pos = static_cast<std::size_t>(me - sibs.begin());
    auto first = [&](int k) {
        const auto t = doc.subtree(k);
        return *std::min_element(t.begin(), t.end());
    };
    auto last = [&](int k) {
        const auto t = doc.subtree(k);
        return *std::max_element(t.begin(), t.end());
    };
    if (to == "front" && pos + 1 < sibs.size()) moveBlock(doc, {idx}, last(sibs.back()) + 1);
    else if (to == "back" && pos > 0) moveBlock(doc, {idx}, first(sibs.front()));
    else if (to == "up" && pos + 1 < sibs.size()) moveBlock(doc, {idx}, last(sibs[pos + 1]) + 1);
    else if (to == "down" && pos > 0) moveBlock(doc, {idx}, first(sibs[pos - 1]));
}

}  // namespace

void registerOrganizeCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.add_layer", "New Layer",
             "Add a layer (a named group listed in the layers panel) on top, or inside another layer as a sublayer. "
             "New shapes go into the current layer; the new layer becomes current. Returns its id.",
             {docParam(), {"name", ParamType::String, "layer name", false},
              {"parent", ParamType::String, "make it a sublayer of this layer", false},
              {"current", ParamType::Bool, "make it the current layer", false, {}, true}},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                Shape g;
                g.kind = ShapeKind::Group;
                g.layer = true;
                if (has(p, "parent")) {
                    const int pi = doc.find(stringParam(p, "parent"));
                    if (pi < 0 || !doc.shapes[static_cast<std::size_t>(pi)].layer) return Result::failure("parent= must name a layer");
                    g.parent = doc.shapes[static_cast<std::size_t>(pi)].id;
                }
                int layers = 1;
                for (const auto& s : doc.shapes) layers += s.layer ? 1 : 0;
                g.name = stringParam(p, "name", "Layer " + std::to_string(layers));
                const std::string id = insertShape(doc, std::move(g), static_cast<int>(doc.shapes.size()));
                if (boolParam(p, "current", true)) doc.currentLayer = id;
                return Result::success("added layer " + id + " \"" + stringParam(p, "name", "Layer " + std::to_string(layers)) + "\"", id).with("shape", id);
            }));

    reg.add({"vector.current_layer", "Current Layer", "Pick the layer new shapes go into (layer=none: the top level).",
             {docParam(), {"layer", ParamType::String, "layer id or name, or none", true}}, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                const std::string name = stringParam(p, "layer");
                if (name == "none" || name.empty()) {
                    doc.currentLayer.clear();
                    return Result::success("new shapes go to the top level");
                }
                const int i = doc.find(name);
                if (i < 0 || doc.shapes[static_cast<std::size_t>(i)].kind != ShapeKind::Group) return Result::failure("no layer or group '" + name + "'");
                doc.currentLayer = doc.shapes[static_cast<std::size_t>(i)].id;
                return Result::success("current layer " + doc.currentLayer, doc.currentLayer).with("shape", doc.currentLayer);
            }));

    reg.add({"vector.layers", "Layers", "List the layers and sublayers, top-level first, with how many items each holds.",
             {docParam()}, false, true},
            vectorCommand(session, [](VectorDoc& doc, const Params&) {
                std::string text, ids;
                layerTree(doc, "", 0, text, ids);
                if (ids.empty()) text = "\n(no layers: everything is at the top level; vector.add_layer makes one)";
                return Result::success(std::to_string(ids.empty() ? 0 : std::count(ids.begin(), ids.end(), ',') + 1) + " layers" + text)
                    .with("layers", ids)
                    .with("current", doc.currentLayer);
            }, false));

    reg.add({"vector.group", "Group",
             "Put shapes into a new group (it stacks where the topmost of them was). Groups move, scale, fade and "
             "take effects as one. Returns the group id.",
             {docParam(), {"shapes", ParamType::String, "the shapes, comma separated", true}, {"name", ParamType::String, "group name", false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!shapeList(doc, stringParam(p, "shapes"), idx, error)) return Result::failure(error);
                const std::string gid = groupShapes(doc, idx, stringParam(p, "name", ""));
                Shape& g = doc.shapes[static_cast<std::size_t>(doc.find(gid))];
                if (g.name.empty()) g.name = "group " + gid.substr(1);
                Result r = Result::success("grouped " + std::to_string(doc.children(gid).size()) + " shapes as " + gid, gid);
                r.with("shape", gid);
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.ungroup", "Ungroup",
             "Dissolve a group (or layer): its shapes move up into the group's parent, in the same place in the "
             "stack. A clipping shape stays as an unpainted shape.",
             {docParam(), shapeParam(), previewParam()}, true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params&) {
                const Shape g = doc.shapes[static_cast<std::size_t>(idx)];
                if (g.kind != ShapeKind::Group) return Result::failure(g.id + " isn't a group");
                const auto kids = doc.children(g.id);
                for (int k : kids) {
                    Shape& c = doc.shapes[static_cast<std::size_t>(k)];
                    c.parent = g.parent;
                    if (c.id == g.clip) c.fill.reset(), c.stroke.reset();
                    // the group's own fading and effects can't stay behind with nobody to hold them: pass opacity down
                    c.opacity *= g.opacity;
                }
                doc.shapes.erase(doc.shapes.begin() + idx);
                if (doc.currentLayer == g.id) doc.currentLayer = g.parent;
                return Result::success("ungrouped " + g.id + " (" + std::to_string(kids.size()) + " shapes)", g.id);
            })));

    reg.add({"vector.lock", "Lock",
             "Lock shapes (or layers) so clicks and edits pass them by; locked=false unlocks. Hide with vector.style visible=false.",
             {docParam(), oneShapeParam(), shapesParam(), {"locked", ParamType::Bool, "lock (true) or unlock (false)", false, {}, true}}, true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!selection(doc, p, idx, error)) return Result::failure(error);
                const bool lock = boolParam(p, "locked", true);
                for (int i : idx) doc.shapes[static_cast<std::size_t>(i)].locked = lock;
                return Result::success(std::string(lock ? "locked " : "unlocked ") + idText(doc, idx), idText(doc, idx));
            }));

    reg.add({"vector.move_to", "Move to Layer",
             "Move shapes into a group or layer (on top of what's there), or to the top level with into=none.",
             {docParam(), oneShapeParam(), shapesParam(), {"into", ParamType::String, "group or layer id/name, or none", true}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!selection(doc, p, idx, error)) return Result::failure(error);
                std::string target;
                if (stringParam(p, "into") != "none") {
                    const int t = doc.find(stringParam(p, "into"));
                    if (t < 0 || doc.shapes[static_cast<std::size_t>(t)].kind != ShapeKind::Group) return Result::failure("into= must name a group or layer");
                    target = doc.shapes[static_cast<std::size_t>(t)].id;
                    for (int i : idx) {
                        const auto tree = doc.subtree(i);
                        if (std::find(tree.begin(), tree.end(), t) != tree.end()) return Result::failure("can't move a group into itself");
                    }
                }
                std::vector<std::string> ids;
                for (int i : topmost(doc, idx)) ids.push_back(doc.shapes[static_cast<std::size_t>(i)].id);
                for (const auto& id : ids) {
                    const int i = doc.find(id);
                    doc.shapes[static_cast<std::size_t>(i)].parent = target;
                    moveBlock(doc, {i}, static_cast<int>(doc.shapes.size()));  // on top of everything, so on top inside the target
                }
                Result r = Result::success("moved " + std::to_string(ids.size()) + " shape(s) into " + (target.empty() ? "the top level" : target));
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.reorder", "Arrange",
             "Move a shape in the stacking order among the shapes beside it (in its group or layer): front, back, up or down one step.",
             {docParam(), shapeParam(), {"to", ParamType::String, "where to move it", true, {"front", "back", "up", "down"}}, previewParam()},
             true},
            vectorCommand(session, onShape([](VectorDoc& doc, int idx, const Params& p) {
                const std::string to = stringParam(p, "to");
                const std::string id = doc.shapes[static_cast<std::size_t>(idx)].id;
                restack(doc, idx, to);
                const int now = doc.find(id);
                const auto sibs = doc.children(doc.shapes[static_cast<std::size_t>(now)].parent);
                const auto pos = static_cast<std::size_t>(std::find(sibs.begin(), sibs.end(), now) - sibs.begin());
                return Result::success("moved " + id + " to position " + std::to_string(pos + 1) + " of " + std::to_string(sibs.size()) + " (1 = bottom)", id)
                    .with("shape", id)
                    .with("index", static_cast<std::int64_t>(now));
            })));

    reg.add({"vector.transform", "Transform",
             "Move, rotate, scale, skew or flip shapes and groups. dx/dy move; rotate turns clockwise by degrees; "
             "scale (or scale_x/scale_y) resizes; skew_x/skew_y slant by degrees; flip=horizontal|vertical mirrors. "
             "It all happens around the centre of the selection, or each shape's own centre with each=true, or "
             "origin_x/origin_y. A single plain shape keeps an editable transform (reset=true clears it first); "
             "otherwise the change is built into the shapes.",
             {docParam(), oneShapeParam(), shapesParam(), numParam("dx", "move right by px", false, 0.0), numParam("dy", "move down by px", false, 0.0),
              numParam("rotate", "degrees clockwise", false, 0.0), numParam("scale", "uniform factor", false, 1.0),
              numParam("scale_x", "horizontal factor", false, 1.0), numParam("scale_y", "vertical factor", false, 1.0),
              numParam("skew_x", "horizontal slant, degrees", false, 0.0), numParam("skew_y", "vertical slant, degrees", false, 0.0),
              {"flip", ParamType::String, "mirror", false, {"none", "horizontal", "vertical"}, std::string("none")},
              {"each", ParamType::Bool, "transform each shape around its own centre", false, {}, false},
              numParam("origin_x", "pivot x (default: the centre)", false), numParam("origin_y", "pivot y", false),
              {"reset", ParamType::Bool, "clear a single shape's transform first", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!selection(doc, p, idx, error)) return Result::failure(error);
                idx = topmost(doc, idx);
                for (int i : idx) {
                    if (!editable(doc, i, error)) return Result::failure(error);
                }
                const double k = numberParam(p, "scale", 1), kx = k * numberParam(p, "scale_x", 1), ky = k * numberParam(p, "scale_y", 1);
                if (kx == 0 || ky == 0) return Result::failure("scale factors can't be 0");
                const double rot = numberParam(p, "rotate", 0), skx = numberParam(p, "skew_x", 0), sky = numberParam(p, "skew_y", 0);
                const std::string flip = stringParam(p, "flip", "none");
                const bool fancy = skx != 0 || sky != 0 || flip != "none" || boolParam(p, "each", false) || has(p, "origin_x") || has(p, "origin_y");
                Shape& first = doc.shapes[static_cast<std::size_t>(idx[0])];
                if (idx.size() == 1 && !fancy && first.kind != ShapeKind::Group && !(first.kind == ShapeKind::Text && !first.onPath.empty())) {
                    if (boolParam(p, "reset", false)) first.transform = {};
                    Transform& t = first.transform;
                    t.tx += numberParam(p, "dx", 0);
                    t.ty += numberParam(p, "dy", 0);
                    t.rotate = std::fmod(t.rotate + rot, 360.0);
                    t.sx *= kx;
                    t.sy *= ky;
                    Result r = Result::success("transformed " + first.id + ": " + describe(first), first.id);
                    r.with("shape", first.id);
                    return maybePreview(r, doc, p);
                }
                Matrix lin = Matrix::rotate(rot) * Matrix::skew(skx, sky) * Matrix::scale(kx, ky);
                if (flip == "horizontal") lin = lin * Matrix::scale(-1, 1);
                if (flip == "vertical") lin = lin * Matrix::scale(1, -1);
                const Matrix move = Matrix::translate(numberParam(p, "dx", 0), numberParam(p, "dy", 0));
                const Bounds all = unionBounds(doc, idx);
                if (all.empty) return Result::failure("nothing there to transform");
                Point pivot{(all.x0 + all.x1) / 2, (all.y0 + all.y1) / 2};
                if (has(p, "origin_x")) pivot.x = numberParam(p, "origin_x");
                if (has(p, "origin_y")) pivot.y = numberParam(p, "origin_y");
                std::vector<std::string> ids;
                for (int i : idx) ids.push_back(doc.shapes[static_cast<std::size_t>(i)].id);
                for (const auto& id : ids) {
                    const int i = doc.find(id);
                    Point c = pivot;
                    if (boolParam(p, "each", false)) {
                        const Bounds b = shapeBounds(doc, i);
                        if (!b.empty) c = {(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
                    }
                    applyMatrix(doc, i, move * about(lin, c));
                }
                Result r = Result::success("transformed " + std::to_string(ids.size()) + " shape(s): " + idText(doc, idx), idText(doc, idx));
                r.with("shapes", idText(doc, idx));
                if (ids.size() == 1) r.with("shape", ids[0]);
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.align", "Align",
             "Line shapes up: horizontal left/center/right and vertical top/middle/bottom, relative to the canvas, "
             "an artboard, the selection's box or a key shape (to=canvas|artboard|selection|key). One shape aligns "
             "to the canvas by default, several to their selection.",
             {docParam(), oneShapeParam(), shapesParam(),
              {"horizontal", ParamType::String, "where to put them across", false, {"none", "left", "center", "right"}, std::string("none")},
              {"vertical", ParamType::String, "where to put them up/down", false, {"none", "top", "middle", "bottom"}, std::string("none")},
              {"to", ParamType::String, "what to align to", false, {"auto", "canvas", "artboard", "selection", "key"}, std::string("auto")},
              {"key", ParamType::String, "the key shape (to=key); it stays put", false},
              {"artboard", ParamType::String, "artboard name or number (to=artboard)", false},
              numParam("margin", "gap from the canvas or artboard edge in px", false, 0.0), previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!selection(doc, p, idx, error)) return Result::failure(error);
                idx = topmost(doc, idx);
                std::string to = stringParam(p, "to", "auto");
                if (to == "auto") to = has(p, "key") ? "key" : idx.size() == 1 ? "canvas" : "selection";
                Bounds ref;
                double m = numberParam(p, "margin", 0);
                std::string keyId;
                if (to == "canvas") {
                    ref = {0, 0, static_cast<double>(doc.width), static_cast<double>(doc.height), false};
                } else if (to == "artboard") {
                    int a = 0;
                    if (has(p, "artboard")) {
                        const std::string name = stringParam(p, "artboard");
                        a = -1;
                        for (int k = 0; k < doc.artboardCount(); ++k) {
                            if (doc.artboard(k).name == name || std::to_string(k + 1) == name) a = k;
                        }
                        if (a < 0) return Result::failure("no artboard '" + name + "'");
                    }
                    const Artboard ab = doc.artboard(a);
                    ref = {ab.x, ab.y, ab.x + ab.w, ab.y + ab.h, false};
                } else if (to == "key") {
                    const int k = doc.find(stringParam(p, "key", ""));
                    if (k < 0) return Result::failure("to=key needs key= naming a shape");
                    keyId = doc.shapes[static_cast<std::size_t>(k)].id;
                    ref = shapeBounds(doc, k);
                    m = 0;
                } else {
                    ref = unionBounds(doc, idx);
                    m = 0;
                }
                if (ref.empty) return Result::failure("nothing to align to");
                const std::string hz = stringParam(p, "horizontal", "none"), vt = stringParam(p, "vertical", "none");
                std::vector<std::string> ids;
                for (int i : idx) ids.push_back(doc.shapes[static_cast<std::size_t>(i)].id);
                for (const auto& id : ids) {
                    if (id == keyId) continue;
                    const int i = doc.find(id);
                    if (!editable(doc, i, error)) return Result::failure(error);
                    const Bounds b = shapeBounds(doc, i);
                    if (b.empty) continue;
                    double dx = 0, dy = 0;
                    if (hz == "left") dx = ref.x0 + m - b.x0;
                    else if (hz == "center") dx = (ref.x0 + ref.x1 - b.width()) / 2 - b.x0;
                    else if (hz == "right") dx = ref.x1 - m - b.x1;
                    if (vt == "top") dy = ref.y0 + m - b.y0;
                    else if (vt == "middle") dy = (ref.y0 + ref.y1 - b.height()) / 2 - b.y0;
                    else if (vt == "bottom") dy = ref.y1 - m - b.y1;
                    if (dx != 0 || dy != 0) applyMatrix(doc, i, Matrix::translate(dx, dy));
                }
                const std::string list = idText(doc, idx);
                Result r = Result::success("aligned " + list + " to the " + to + (idx.size() == 1 ? ": " + describe(doc.shapes[static_cast<std::size_t>(idx[0])]) : ""), list);
                r.with("shapes", list);
                if (idx.size() == 1) r.with("shape", list);
                return maybePreview(r, doc, p);
            }));

    reg.add({"vector.distribute", "Distribute",
             "Space three or more shapes evenly across (axis=horizontal) or down (vertical): by=spacing makes the "
             "gaps equal (or exactly gap= px), by=centers / left / right / top / bottom spaces those edges evenly. "
             "The outermost shapes stay put (unless gap= is given).",
             {docParam(), {"shapes", ParamType::String, "the shapes, comma separated", true},
              {"axis", ParamType::String, "direction", false, {"horizontal", "vertical"}, std::string("horizontal")},
              {"by", ParamType::String, "what to space evenly", false, {"spacing", "centers", "left", "right", "top", "bottom"}, std::string("spacing")},
              numParam("gap", "exact gap in px (by=spacing)", false), previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                std::vector<int> idx;
                std::string error;
                if (!shapeList(doc, stringParam(p, "shapes"), idx, error)) return Result::failure(error);
                idx = topmost(doc, idx);
                const bool gapGiven = has(p, "gap");
                if (idx.size() < (gapGiven ? 2u : 3u)) return Result::failure("give at least three shapes (or two with gap=)");
                const bool hz = stringParam(p, "axis", "horizontal") == "horizontal";
                const std::string by = stringParam(p, "by", "spacing");
                struct Item {
                    std::string id;
                    double lo, hi;
                };
                std::vector<Item> items;
                for (int i : idx) {
                    const Bounds b = shapeBounds(doc, i);
                    if (b.empty) continue;
                    items.push_back({doc.shapes[static_cast<std::size_t>(i)].id, hz ? b.x0 : b.y0, hz ? b.x1 : b.y1});
                }
                std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.lo + a.hi < b.lo + b.hi; });
                const std::size_t n = items.size();
                std::vector<double> target(n);
                if (by == "spacing") {
                    double total = 0;
                    for (const auto& it : items) total += it.hi - it.lo;
                    const double gap = gapGiven ? numberParam(p, "gap") : (items.back().hi - items.front().lo - total) / static_cast<double>(n - 1);
                    double at = items.front().lo;
                    for (std::size_t k = 0; k < n; ++k) {
                        target[k] = at;
                        at += items[k].hi - items[k].lo + gap;
                    }
                } else {
                    auto key = [&](const Item& it) {
                        if (by == "left" || by == "top") return it.lo;
                        if (by == "right" || by == "bottom") return it.hi;
                        return (it.lo + it.hi) / 2;
                    };
                    const double a = key(items.front()), b = key(items.back());
                    for (std::size_t k = 0; k < n; ++k) target[k] = items[k].lo + (a + (b - a) * static_cast<double>(k) / static_cast<double>(n - 1) - key(items[k]));
                }
                for (std::size_t k = 0; k < n; ++k) {
                    const double d = target[k] - items[k].lo;
                    if (std::abs(d) > 1e-12) applyMatrix(doc, doc.find(items[k].id), Matrix::translate(hz ? d : 0, hz ? 0 : d));
                }
                Result r = Result::success("distributed " + std::to_string(n) + " shapes " + (hz ? "across" : "down") + " by " + by);
                return maybePreview(r, doc, p);
            }));
}

}  // namespace wetwaresuite::vector::detail
