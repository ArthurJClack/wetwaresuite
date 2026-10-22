// Layer commands: add, delete, properties, order, duplicate, merge, groups,
// rasterize and layer styles. Groups are flat (see LayerInfo::depth), so a
// "block" [groupStart(i), i] is layer i with everything inside it, and every
// structural command moves or copies whole blocks.
#include <algorithm>
#include <cstdio>

#include "common.hpp"

namespace wetwaresuite::paint {

using namespace raster;

namespace {

using Layers = std::vector<Layer>;

std::string quoted(const Layer& l) { return "\"" + l.name + "\""; }

// Renders layers [lo, hi] on their own, depths taken relative to the top one (the block's outermost).
Image renderPart(Layers part, int width, int height) {
    if (part.empty()) return Image(width, height);
    const int base = part.back().depth;
    for (auto& l : part) l.depth -= base;
    return composite(part, width, height);
}

// What each layer looks like counting its groups: hidden when it or any group around it is hidden.
std::vector<bool> shownLayers(const Layers& L) {
    std::vector<bool> shown(L.size()), atDepth;
    for (std::size_t i = L.size(); i-- > 0;) {
        const int d = L[i].depth;
        const bool v = L[i].visible && (d == 0 || (d - 1 < static_cast<int>(atDepth.size()) && atDepth[static_cast<std::size_t>(d - 1)]));
        atDepth.resize(static_cast<std::size_t>(d) + 1);
        atDepth[static_cast<std::size_t>(d)] = v;
        shown[i] = v;
    }
    return shown;
}

// A pixel layer with the look of `src` drawn in (style, fill, text...), keeping its other settings.
Layer bakedCopy(const Layer& src, Image pixels) {
    Layer out = src;
    out.image = std::move(pixels);
    out.style = {};
    out.fill = 1;
    out.kind = LayerKind::Pixel;
    out.settings.clear();
    return out;
}

Result mergeDown(ImageDoc& doc, const Params& p) {
    std::string error;
    const int idx = layerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    Layers& L = doc.image.layers;
    const Layer& top = L[static_cast<std::size_t>(idx)];
    if (!top.visible) return Result::failure("layer " + std::to_string(idx) + " is hidden; show it first (merging would drop it)");
    const int topStart = groupStart(L, idx);
    const int below = topStart - 1;
    if (below < 0) return Result::failure("the bottom layer has nothing below it");
    if (L[static_cast<std::size_t>(below)].depth < top.depth) {
        return Result::failure("layer " + std::to_string(idx) + " is the bottom of its group; nothing below it in the group to merge into");
    }
    const Layer& under = L[static_cast<std::size_t>(below)];
    if (under.kind == LayerKind::Adjustment) return Result::failure("can't merge into an adjustment layer; move it or merge it down instead");
    const int lo = groupStart(L, below);
    Layers part(L.begin() + lo, L.begin() + idx + 1);
    Layer& base = part[static_cast<std::size_t>(below - lo)];
    const bool keepBase = under.kind != LayerKind::Group;
    if (keepBase) {
        // the merged layer keeps the lower layer's opacity, blend and place; its look is drawn in
        base.opacity = 1.0;
        base.blend = Blend::Normal;
        base.clip = false;
        base.maskEnabled = base.maskEnabled && !base.mask.empty();
    }
    base.visible = true;
    Image merged = renderPart(std::move(part), doc.image.width, doc.image.height);
    Layer result;
    if (keepBase) {
        result = bakedCopy(under, std::move(merged));
        result.mask = Mask();
        result.visible = true;
    } else {
        result = Layer(under.name, std::move(merged));
        result.depth = under.depth;
    }
    const std::string what = (top.kind == LayerKind::Adjustment ? "applied adjustment " : "merged ") + quoted(top) + " into " + quoted(under);
    L.erase(L.begin() + lo, L.begin() + idx + 1);
    L.insert(L.begin() + lo, std::move(result));
    doc.active = lo;
    return done(what, doc, p);
}

Result mergeVisible(ImageDoc& doc, const Params& p) {
    Layers& L = doc.image.layers;
    const std::vector<bool> shown = shownLayers(L);
    Image flat = doc.flatten();
    Layers kept;
    int at = -1;
    for (std::size_t i = 0; i < L.size(); ++i) {
        if (shown[i]) {
            if (at < 0) at = static_cast<int>(kept.size());
            continue;
        }
        kept.push_back(std::move(L[i]));
    }
    if (at < 0) return Result::failure("no layer is visible");
    kept.insert(kept.begin() + at, Layer("Merged", std::move(flat)));
    L = std::move(kept);
    normalizeDepths(L);
    doc.active = at;
    return done("merged the visible layers into layer " + std::to_string(at), doc, p);
}

// Moves block [s, idx] so its top lands at index `to` of the final stack. Its depth comes from
// where it lands: inside a group when it's put right under the group's header or among its members.
Result moveLayer(ImageDoc& doc, const Params& p) {
    std::string error;
    const int idx = layerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    Layers& L = doc.image.layers;
    const int s = groupStart(L, idx), n = idx - s + 1;
    const int to = static_cast<int>(intParam(p, "to"));
    const int t = to - n + 1;  // where the block's bottom goes after it's taken out
    if (to < 0 || to >= static_cast<int>(L.size()) || t < 0) {
        return Result::failure("'to' is outside the layer stack (0.." + std::to_string(L.size() - 1) +
                               (n > 1 ? ", and a group of " + std::to_string(n) + " layers needs room below it)" : ")"));
    }
    Layers block(std::make_move_iterator(L.begin() + s), std::make_move_iterator(L.begin() + idx + 1));
    L.erase(L.begin() + s, L.begin() + idx + 1);
    int depth = 0;
    if (t < static_cast<int>(L.size())) {
        const Layer& above = L[static_cast<std::size_t>(t)];
        depth = above.kind == LayerKind::Group ? above.depth + 1 : above.depth;
    }
    const int shift = depth - block.back().depth;
    for (auto& l : block) l.depth += shift;
    L.insert(L.begin() + t, std::make_move_iterator(block.begin()), std::make_move_iterator(block.end()));
    normalizeDepths(L);
    doc.active = to;
    std::string where = depth > 0 ? " (inside a group)" : "";
    return done("moved " + quoted(L[static_cast<std::size_t>(to)]) + " to index " + std::to_string(to) + where, doc, p);
}

}  // namespace

void registerLayerCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"paint.layer_add",
             "Add layer",
             "Add a new transparent layer just above the active one (inside the group when a group is active) and make it active.",
             {docParam(), {"name", ParamType::String, "layer name", false}, colorParam("fill", "start filled with this colour", false, "")},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string name = stringParam(p, "name", "Layer " + std::to_string(doc.image.layers.size()));
                Color c{0, 0, 0, 0};
                std::string error;
                if (p.count("fill") && !colorArg(p, "fill", "", c, error)) return Result::failure(error);
                const int at = insertLayer(doc, Layer(name, Image(doc.image.width, doc.image.height, c)));
                return done("added layer \"" + name + "\" at index " + std::to_string(at), doc, p);
            }));

    reg.add({"paint.layer_delete", "Delete layer",
             "Delete a layer; deleting a group deletes everything in it (keep_contents=true ungroups instead). "
             "An image always keeps at least one layer.",
             {docParam(), layerParam(), {"keep_contents", ParamType::Bool, "groups: keep the layers inside", false, {}, false}}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layers& L = doc.image.layers;
                const bool keep = boolParam(p, "keep_contents", false);
                const int s = keep ? idx : groupStart(L, idx);
                if (static_cast<int>(L.size()) - (idx - s + 1) < 1) return Result::failure("can't delete the only layer");
                const std::string name = L[static_cast<std::size_t>(idx)].name;
                if (keep) {
                    for (int i = groupStart(L, idx); i < idx; ++i) L[static_cast<std::size_t>(i)].depth -= 1;
                }
                L.erase(L.begin() + s, L.begin() + idx + 1);
                normalizeDepths(L);
                doc.active = std::clamp(doc.active > idx ? doc.active - (idx - s + 1) : std::min(doc.active, s), 0, static_cast<int>(L.size()) - 1);
                return done("deleted layer \"" + name + "\"" + (idx - s > 0 ? " and the " + std::to_string(idx - s) + " layers in it" : ""), doc, p);
            }));

    reg.add({"paint.layer_set",
             "Layer properties",
             "Change a layer's name, opacity (0..100), fill opacity (0..100: fades the pixels but not the layer style), "
             "blend mode, visibility, clipping, alpha lock (paint only where there's paint already), and for groups "
             "pass-through (members blend straight through) and folding in the layers panel.",
             {docParam(), layerParam(),
              {"name", ParamType::String, "new name", false},
              {"opacity", ParamType::Number, "opacity 0..100", false},
              {"fill", ParamType::Number, "fill opacity 0..100", false},
              {"blend", ParamType::String, "blend mode", false, blendNames()},
              {"visible", ParamType::Bool, "show or hide the layer", false},
              {"clip", ParamType::Bool, "clip to the layer below (clipping mask)", false},
              {"lock_alpha", ParamType::Bool, "lock transparent pixels", false},
              {"pass_through", ParamType::Bool, "groups: blend members straight through (false = isolated, then blended as one)", false},
              {"collapsed", ParamType::Bool, "groups: folded in the layers panel", false},
              previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                auto& l = doc.image.layers[static_cast<std::size_t>(idx)];
                if (p.count("clip") && boolParam(p, "clip", false) && idx == 0) return Result::failure("the bottom layer has nothing below it to clip to");
                if ((p.count("pass_through") || p.count("collapsed")) && l.kind != LayerKind::Group) {
                    return Result::failure("pass_through and collapsed are for groups; layer " + std::to_string(idx) + " isn't one");
                }
                if (p.count("name")) l.name = stringParam(p, "name");
                if (p.count("opacity")) l.opacity = percent(p, "opacity", 100);
                if (p.count("fill")) l.fill = percent(p, "fill", 100);
                if (p.count("blend")) l.blend = *parseBlend(stringParam(p, "blend"));
                if (p.count("visible")) l.visible = boolParam(p, "visible", true);
                if (p.count("clip")) l.clip = boolParam(p, "clip", false);
                if (p.count("lock_alpha")) l.lockAlpha = boolParam(p, "lock_alpha", false);
                if (p.count("pass_through")) l.passThrough = boolParam(p, "pass_through", true);
                if (p.count("collapsed")) l.collapsed = boolParam(p, "collapsed", false);
                return done("updated layer " + std::to_string(idx) + " " + quoted(l), doc, p);
            }));

    reg.add({"paint.layer_select", "Select layer", "Make a layer the active one; brush and fill commands go there by default.",
             {docParam(), {"layer", ParamType::Int, "layer index, 0 = bottom", true}}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                doc.active = idx;
                return done("layer " + std::to_string(idx) + " " + quoted(doc.activeLayer()) + " is active", doc, p);
            }));

    reg.add({"paint.layer_move", "Move layer",
             "Move a layer (a group moves with everything in it) so it ends up at index `to` (0 = bottom). Landing just "
             "under a group's header or between its members puts it into that group; moving it past the header takes it out.",
             {docParam(), layerParam(), {"to", ParamType::Int, "new index of the layer", true}, previewParam()}, true},
            imageCommand(session, moveLayer));

    reg.add({"paint.layer_duplicate", "Duplicate layer", "Copy a layer (a group with everything in it) and put the copy just above it.",
             {docParam(), layerParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layers& L = doc.image.layers;
                const int s = groupStart(L, idx);
                Layers copy(L.begin() + s, L.begin() + idx + 1);
                copy.back().name += " copy";
                L.insert(L.begin() + idx + 1, copy.begin(), copy.end());
                doc.active = idx + static_cast<int>(copy.size());
                return done("duplicated layer " + std::to_string(idx), doc, p);
            }));

    reg.add({"paint.layer_merge_down", "Merge down",
             "Merge a layer into the one below it (in the same group). Its blend mode, opacity, mask, clipping and style "
             "are drawn in; an adjustment layer gets applied to the layer below; a group merges with everything in it.",
             {docParam(), layerParam(), previewParam()}, true},
            imageCommand(session, mergeDown));

    reg.add({"paint.merge_visible", "Merge visible", "Merge every visible layer into one; hidden layers stay as they are.",
             {docParam(), previewParam()}, true},
            imageCommand(session, mergeVisible));

    reg.add({"paint.flatten", "Flatten", "Merge every visible layer into a single layer (hidden layers are dropped).", {docParam(), previewParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                Image flat = doc.flatten();
                doc.image.layers.clear();
                doc.image.layers.push_back({"Background", std::move(flat)});
                doc.active = 0;
                return done("flattened to one layer", doc, p);
            }));
}

void registerGroupCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"paint.group",
             "Group layers",
             "Put layers from..to (indices, inclusive; default the active layer) into a new group, which becomes active. "
             "They must sit side by side in the same group. Groups fold, hide, fade and blend together; pass_through=false "
             "blends the group as one picture.",
             {docParam(), {"from", ParamType::Int, "lowest layer to group", false}, {"to", ParamType::Int, "highest layer to group", false},
              {"name", ParamType::String, "group name", false}, {"pass_through", ParamType::Bool, "members blend straight through", false, {}, true}},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                Layers& L = doc.image.layers;
                const int n = static_cast<int>(L.size());
                int hi = static_cast<int>(intParam(p, "to", p.count("from") ? intParam(p, "from") : doc.active));
                int lo = static_cast<int>(intParam(p, "from", hi));
                if (lo > hi) std::swap(lo, hi);
                if (lo < 0 || hi >= n) return Result::failure("from/to must be layer indices 0.." + std::to_string(n - 1));
                const int d = L[static_cast<std::size_t>(hi)].depth;
                for (int i = lo; i <= hi; ++i) {
                    lo = std::min(lo, groupStart(L, i));  // a group comes with everything in it
                }
                for (int i = lo; i <= hi; ++i) {
                    if (L[static_cast<std::size_t>(i)].depth < d) return Result::failure("those layers aren't in the same group; move them together first");
                }
                for (int i = lo; i <= hi; ++i) L[static_cast<std::size_t>(i)].depth += 1;
                Layer g(stringParam(p, "name", "Group " + std::to_string(std::count_if(L.begin(), L.end(), [](const Layer& l) {
                                                                return l.kind == LayerKind::Group;
                                                            }) + 1)));
                g.kind = LayerKind::Group;
                g.depth = d;
                g.passThrough = boolParam(p, "pass_through", true);
                L.insert(L.begin() + hi + 1, std::move(g));
                doc.active = hi + 1;
                return done("grouped layers " + std::to_string(lo) + ".." + std::to_string(hi) + " into group " + std::to_string(hi + 1), doc, p);
            }));

    reg.add({"paint.ungroup", "Ungroup", "Remove a group but keep the layers that were in it.", {docParam(), layerParam(), previewParam()}, true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layers& L = doc.image.layers;
                if (L[static_cast<std::size_t>(idx)].kind != LayerKind::Group) return Result::failure("layer " + std::to_string(idx) + " isn't a group");
                const int s = groupStart(L, idx);
                for (int i = s; i < idx; ++i) L[static_cast<std::size_t>(i)].depth -= 1;
                L.erase(L.begin() + idx);
                normalizeDepths(L);
                doc.active = std::max(0, idx - 1);
                return done("ungrouped " + std::to_string(idx - s) + " layers", doc, p);
            }));

    reg.add({"paint.rasterize",
             "Rasterize layer",
             "Turn a layer into plain pixels: text and shape layers stop being editable, a layer style and fill opacity are "
             "drawn into the pixels, and a group becomes one layer.",
             {docParam(), layerParam(), previewParam()},
             true},
            imageCommand(session, [](ImageDoc& doc, const Params& p) {
                std::string error;
                const int idx = layerIndex(doc, p, error);
                if (idx < 0) return Result::failure(error);
                Layers& L = doc.image.layers;
                Layer& l = L[static_cast<std::size_t>(idx)];
                if (l.kind == LayerKind::Adjustment) return Result::failure("an adjustment layer has no pixels; use paint.layer_merge_down to apply it");
                const int s = groupStart(L, idx);
                Layers part(L.begin() + s, L.begin() + idx + 1);
                Layer& top = part.back();
                top.opacity = 1;
                top.blend = Blend::Normal;
                top.visible = true;
                top.clip = false;
                top.maskEnabled = false;
                top.passThrough = false;
                Image pixels = renderPart(std::move(part), doc.image.width, doc.image.height);
                Layer out = bakedCopy(l, std::move(pixels));
                if (l.kind == LayerKind::Group && l.passThrough) out.blend = Blend::Normal;
                out.passThrough = true;
                out.collapsed = false;
                L.erase(L.begin() + s, L.begin() + idx + 1);
                L.insert(L.begin() + s, std::move(out));
                doc.active = s;
                return done("rasterized layer " + std::to_string(s), doc, p);
            }));
}

namespace {

std::string numText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// Style params: (name, how a value turns into the text setStyleValue reads)
struct StyleParam {
    const char* name;
    ParamType type;
    double scale;  // numbers are divided by this (percent params)
    const char* description;
};

const std::vector<StyleParam>& styleParams() {
    static const std::vector<StyleParam> v = {
        {"color", ParamType::String, 1, "colour (overlays: the start colour)"},
        {"to", ParamType::String, 1, "gradient overlay: end colour"},
        {"opacity", ParamType::Number, 100, "0..100"},
        {"angle", ParamType::Number, 1, "light angle or gradient angle, degrees"},
        {"distance", ParamType::Number, 1, "shadows: offset in px"},
        {"size", ParamType::Number, 1, "blur size, stroke width or bevel size in px"},
        {"spread", ParamType::Number, 100, "shadows and glows: 0..100, how solid the edge is"},
        {"blend", ParamType::String, 1, "blend mode of the effect"},
        {"radial", ParamType::Bool, 1, "gradient overlay: radial instead of linear"},
        {"position", ParamType::String, 1, "stroke: outside, inside or center"},
        {"depth", ParamType::Number, 1, "bevel: strength, 1 = normal"},
        {"altitude", ParamType::Number, 1, "bevel: light height, degrees"},
        {"highlight", ParamType::Number, 100, "bevel: highlight opacity 0..100"},
        {"shadow", ParamType::Number, 100, "bevel: shadow opacity 0..100"},
        {"down", ParamType::Bool, 1, "bevel: pressed in instead of raised"},
    };
    return v;
}

std::vector<ParamSpec> styleSpecs() {
    std::vector<std::string> effects = styleEffectNames();
    effects.push_back("all");
    std::vector<ParamSpec> v = {docParam(), layerParam(),
                                {"effect", ParamType::String, "which effect (all with on=false clears the style)", false, effects},
                                {"on", ParamType::Bool, "turn the effect on (false = off)", false, {}, true},
                                {"style", ParamType::String, "a whole style as text (what this command returns), to copy one layer's style to another", false}};
    for (const auto& sp : styleParams()) {
        ParamSpec spec{sp.name, sp.type, sp.description, false, {}, {}};
        if (std::string(sp.name) == "blend") spec.choices = blendNames();
        if (std::string(sp.name) == "position") spec.choices = {"outside", "inside", "center"};
        v.push_back(spec);
    }
    v.push_back(previewParam());
    return v;
}

Result layerStyle(ImageDoc& doc, const Params& p) {
    std::string error;
    const int idx = layerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
    if (l.kind == LayerKind::Group || l.kind == LayerKind::Adjustment) {
        return Result::failure("layer styles go on pixel, text and shape layers; layer " + std::to_string(idx) + " is a " + toString(l.kind));
    }
    LayerStyle style = l.style;
    if (p.count("style") && !styleFromText(stringParam(p, "style"), style, error)) return Result::failure(error);
    const std::string effect = stringParam(p, "effect", p.count("style") ? "" : "drop_shadow");
    const bool on = boolParam(p, "on", true);
    if (effect == "all") {
        if (on) return Result::failure("effect=all only works with on=false (clear the style)");
        style = {};
    } else if (!effect.empty()) {
        const auto& keys = styleEffectKeys(effect);
        for (const auto& sp : styleParams()) {
            if (!p.count(sp.name)) continue;
            if (std::find(keys.begin(), keys.end(), sp.name) == keys.end()) {
                std::string list;
                for (const auto& k : keys) list += (list.empty() ? "" : ", ") + k;
                return Result::failure(effect + " has no '" + sp.name + "' setting (it has: " + list + ")");
            }
            std::string text;
            if (sp.type == ParamType::Number) text = numText(numberParam(p, sp.name) / sp.scale);
            else if (sp.type == ParamType::Bool) text = boolParam(p, sp.name) ? "true" : "false";
            else text = stringParam(p, sp.name);
            if (!setStyleValue(style, effect, sp.name, text, error)) return Result::failure(error);
        }
        setStyleEffect(style, effect, on);
    }
    l.style = style;
    doc.markAllChanged();
    const std::string text = styleToText(style);
    Result r = done(text.empty() ? "layer " + std::to_string(idx) + " has no style now" : "layer " + std::to_string(idx) + " style: " + text, doc, p);
    return r.with("style", text);
}

}  // namespace

void registerStyleCommands(Session& session) {
    session.registry().add({"paint.layer_style",
                            "Layer style",
                            "Add or change a layer effect, drawn live as the layer changes: drop_shadow, inner_shadow, outer_glow, "
                            "inner_glow, bevel (bevel and emboss), color_overlay, gradient_overlay, stroke. Only the settings you "
                            "give change. on=false turns an effect off; effect=all on=false clears the style. The result's `style` "
                            "text can be passed as style= to copy it to another layer.",
                            styleSpecs(), true},
                           imageCommand(session, layerStyle));
}

}  // namespace wetwaresuite::paint
