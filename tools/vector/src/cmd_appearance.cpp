// Appearance commands: stroke options (caps, joins, dashes, arrowheads, width
// profiles, alignment), extra fills and strokes, effects, patterns and global
// swatches.
#include <algorithm>

#include "internal.hpp"

namespace wetwaresuite::vector::detail {

namespace {

const char* kStrokeKeys[] = {"cap", "join", "miter_limit", "dash", "dash_offset", "align", "start_arrow", "end_arrow", "arrow_scale", "profile"};

std::string valueText(const Params& p, const std::string& key) {
    auto it = p.find(key);
    if (it == p.end()) return {};
    if (auto* s = std::get_if<std::string>(&it->second)) return *s;
    if (auto* d = std::get_if<double>(&it->second)) return num(*d);
    if (auto* i = std::get_if<std::int64_t>(&it->second)) return std::to_string(*i);
    if (auto* l = std::get_if<NumberList>(&it->second)) {
        std::string out;
        for (double v : *l) out += (out.empty() ? "" : ",") + num(v);
        return out;
    }
    return {};
}

// The leaves a command on a shape reaches: the shape, or everything drawable inside a group.
std::vector<int> leaves(const VectorDoc& doc, const std::vector<int>& idx) {
    std::vector<int> out;
    for (int i : idx) {
        for (int k : doc.subtree(i)) {
            const ShapeKind kind = doc.shapes[static_cast<std::size_t>(k)].kind;
            if (kind != ShapeKind::Group && kind != ShapeKind::Instance && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        }
    }
    return out;
}

Edit onEach(std::function<bool(VectorDoc&, Shape&, const Params&, std::string&)> fn, const std::string& verb, bool groupsToo = false) {
    return [fn, verb, groupsToo](VectorDoc& doc, const Params& p) {
        std::vector<int> idx;
        std::string error;
        if (!selection(doc, p, idx, error)) return Result::failure(error);
        std::vector<int> targets = groupsToo ? idx : leaves(doc, idx);
        if (targets.empty()) return Result::failure("nothing to change there");
        for (int i : targets) {
            if (!editable(doc, i, error)) return Result::failure(error);
            if (!fn(doc, doc.shapes[static_cast<std::size_t>(i)], p, error)) return Result::failure(error);
        }
        const std::string ids = idText(doc, targets);
        Result r = Result::success(targets.size() == 1 ? verb + " " + describe(doc.shapes[static_cast<std::size_t>(targets[0])])
                                                       : verb + " " + std::to_string(targets.size()) + " shapes: " + ids,
                                   ids);
        r.with("shapes", ids);
        if (targets.size() == 1) r.with("shape", ids);
        return maybePreview(r, doc, p);
    };
}

ParamSpec shapesParam() { return {"shapes", ParamType::String, "several shapes, comma separated (instead of shape)", false}; }
ParamSpec oneShapeParam() { return {"shape", ParamType::String, "shape id or name", false}; }

}  // namespace

void registerAppearanceCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"vector.stroke_style", "Stroke Options",
             "Set how outlines are drawn: cap (butt/round/square), join (miter/round/bevel), miter_limit, dash "
             "(e.g. 6,3), dash_offset, align (center/inside/outside), start_arrow/end_arrow (none, triangle, open, "
             "circle, square, bar), arrow_scale and a width profile (uniform, taper, taper_start, taper_end, bulge, "
             "pinch). Only what you give changes; reset=true goes back to plain round lines.",
             {docParam(), oneShapeParam(), shapesParam(),
              {"cap", ParamType::String, "line ends", false, {"butt", "round", "square"}},
              {"join", ParamType::String, "corners", false, {"miter", "round", "bevel"}},
              numParam("miter_limit", "longest miter, times the width", false),
              {"dash", ParamType::String, "dash and gap lengths in px, e.g. 6,3 or 10,4,2,4; none = solid", false},
              numParam("dash_offset", "start the dashes this far along", false),
              {"align", ParamType::String, "where the outline sits on a closed shape's edge", false, {"center", "inside", "outside"}},
              {"start_arrow", ParamType::String, "arrowhead at the start", false, {"none", "triangle", "open", "circle", "square", "bar"}},
              {"end_arrow", ParamType::String, "arrowhead at the end", false, {"none", "triangle", "open", "circle", "square", "bar"}},
              numParam("arrow_scale", "arrowhead size, times the default", false),
              {"profile", ParamType::String, "width along the line", false, {"uniform", "taper", "taper_start", "taper_end", "bulge", "pinch"}},
              numParam("width", "outline width in px", false), paintParam("color", "outline colour", ""),
              {"reset", ParamType::Bool, "back to plain round lines first", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, onEach([](VectorDoc&, Shape& s, const Params& p, std::string& error) {
                if (boolParam(p, "reset", false)) s.line = {};
                for (const char* key : kStrokeKeys) {
                    if (has(p, key) && !readStrokeOption(s.line, key, valueText(p, key), error)) return false;
                }
                if (has(p, "width")) {
                    if (numberParam(p, "width") < 0) return error = "width can't be negative", false;
                    s.strokeWidth = numberParam(p, "width");
                }
                if (has(p, "color")) {
                    if (!paintArg(stringParam(p, "color"), s.stroke, error)) return false;
                    s.strokeSwatch.clear();
                }
                return true;
            }, "stroke options for")));

    reg.add({"vector.appearance", "Appearance",
             "Stack extra fills and outlines on a shape (the Appearance panel), e.g. add=\"stroke color=#ffcc00 "
             "width=12 under\" for a wide halo behind the main outline, or add=\"fill color=#00000040 dx=4 dy=4 "
             "under\" for a hard shadow. Items: fill|stroke, color=, width=, opacity=, blend=, under, dx=, dy= and "
             "stroke options (cap=, dash=, ...); separate several with ;. set= replaces the stack, remove= drops "
             "one item (0 = first), clear=true empties it.",
             {docParam(), oneShapeParam(), shapesParam(), {"add", ParamType::String, "items to add", false},
              {"set", ParamType::String, "the whole stack (replaces it)", false},
              {"remove", ParamType::Int, "index of an item to drop", false},
              {"clear", ParamType::Bool, "remove every extra item", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, onEach([](VectorDoc&, Shape& s, const Params& p, std::string& error) {
                if (boolParam(p, "clear", false)) s.appearance.clear();
                if (has(p, "set") && !parseAppearance(stringParam(p, "set"), s.appearance, error)) return false;
                if (has(p, "add")) {
                    std::vector<AppearanceItem> more;
                    if (!parseAppearance(stringParam(p, "add"), more, error)) return false;
                    s.appearance.insert(s.appearance.end(), more.begin(), more.end());
                }
                if (has(p, "remove")) {
                    const auto k = intParam(p, "remove", 0);
                    if (k < 0 || k >= static_cast<std::int64_t>(s.appearance.size())) return error = s.id + " has no appearance item " + std::to_string(k), false;
                    s.appearance.erase(s.appearance.begin() + k);
                }
                return true;
            }, "appearance of")));

    reg.add({"vector.effect", "Effects",
             "Add live effects drawn with the shape (and written as SVG filters, rasterised in PDF): drop_shadow, "
             "inner_shadow, outer_glow, inner_glow or blur, e.g. type=drop_shadow dx=6 dy=6 radius=5 "
             "color=#00000080, or add=\"outer_glow color=gold radius=10; blur radius=2\". remove= drops one "
             "(index or type), clear=true removes all. Works on groups too.",
             {docParam(), oneShapeParam(), shapesParam(),
              {"type", ParamType::String, "effect to add", false, {"drop_shadow", "inner_shadow", "outer_glow", "inner_glow", "blur"}},
              paintParam("color", "effect colour", ""), numParam("opacity", "0..1", false), numParam("radius", "blur radius in px", false),
              numParam("dx", "shadow offset right", false), numParam("dy", "shadow offset down", false),
              {"add", ParamType::String, "effects in text form, separated by ;", false},
              {"remove", ParamType::String, "index (0 = first) or type of an effect to drop", false},
              {"clear", ParamType::Bool, "remove every effect", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, onEach([](VectorDoc&, Shape& s, const Params& p, std::string& error) {
                if (boolParam(p, "clear", false)) s.effects.clear();
                if (has(p, "remove")) {
                    const std::string what = stringParam(p, "remove");
                    const auto before = s.effects.size();
                    if (!what.empty() && std::isdigit(static_cast<unsigned char>(what[0]))) {
                        const std::size_t k = static_cast<std::size_t>(std::atoi(what.c_str()));
                        if (k < s.effects.size()) s.effects.erase(s.effects.begin() + static_cast<std::ptrdiff_t>(k));
                    } else {
                        s.effects.erase(std::remove_if(s.effects.begin(), s.effects.end(), [&](const Effect& e) { return e.type == what; }), s.effects.end());
                    }
                    if (s.effects.size() == before) return error = s.id + " has no effect '" + what + "'", false;
                }
                if (has(p, "type")) {
                    std::string text = stringParam(p, "type");
                    for (const char* key : {"color", "opacity", "radius", "dx", "dy"}) {
                        if (has(p, key)) text += std::string(" ") + key + "=" + valueText(p, key);
                    }
                    Effect e;
                    if (!parseEffect(text, e, error)) return false;
                    s.effects.push_back(e);
                }
                if (has(p, "add")) {
                    std::vector<Effect> more;
                    if (!parseEffects(stringParam(p, "add"), more, error)) return false;
                    s.effects.insert(s.effects.end(), more.begin(), more.end());
                }
                return true;
            }, "effects on", true)));

    reg.add({"vector.pattern", "Pattern Fill",
             "Fill shapes with a repeating pattern: type stripes, dots, grid, checker or hatch, in color= over "
             "background= (none = see-through), tile size= px, angle= degrees, weight= (how much of the tile the "
             "mark covers, 0..1). remove=true takes it off.",
             {docParam(), oneShapeParam(), shapesParam(),
              {"type", ParamType::String, "pattern", false, {"stripes", "dots", "grid", "checker", "hatch"}, std::string("stripes")},
              paintParam("color", "mark colour", "black"), paintParam("background", "colour between the marks", "none"),
              numParam("size", "tile size in px", false, 10.0), numParam("angle", "degrees", false, 0.0),
              numParam("weight", "mark coverage 0..1", false, 0.5), {"remove", ParamType::Bool, "remove the pattern", false, {}, false},
              previewParam()},
             true},
            vectorCommand(session, onEach([](VectorDoc&, Shape& s, const Params& p, std::string& error) {
                if (boolParam(p, "remove", false)) {
                    s.pattern.reset();
                    return true;
                }
                std::string text = stringParam(p, "type", "stripes");
                for (const char* key : {"color", "background", "size", "angle", "weight"}) {
                    if (has(p, key)) text += std::string(" ") + key + "=" + valueText(p, key);
                }
                Pattern pat;
                if (!parsePattern(text, pat, error)) return false;
                if (has(p, "color") && !parseColor(stringParam(p, "color"))) return error = "'" + stringParam(p, "color") + "' isn't a colour", false;
                s.pattern = pat;
                return true;
            }, "pattern on")));

    reg.add({"vector.swatch", "Swatch",
             "Global colour swatches: name= with color= defines or changes one, and every shape using it changes "
             "too. With shapes= (and target=fill or stroke) it paints them with the swatch. remove=true deletes "
             "it (shapes keep the colour). With no name, lists the swatches.",
             {docParam(), {"name", ParamType::String, "swatch name (letters, digits, _ - .)", false}, paintParam("color", "the colour", ""),
              oneShapeParam(), shapesParam(), {"target", ParamType::String, "what the swatch paints", false, {"fill", "stroke"}, std::string("fill")},
              {"remove", ParamType::Bool, "delete the swatch", false, {}, false}, previewParam()},
             true},
            vectorCommand(session, [](VectorDoc& doc, const Params& p) {
                if (!has(p, "name")) {
                    std::string text = std::to_string(doc.swatches.size()) + " swatches", list;
                    for (const auto& [n, c] : doc.swatches) {
                        text += "\n" + n + " " + colorText(c);
                        list += (list.empty() ? "" : ",") + n;
                    }
                    return Result::success(text).with("swatches", list);
                }
                const std::string name = stringParam(p, "name");
                if (!validName(name)) return Result::failure("swatch names use letters, digits, _ - and . only");
                auto it = std::find_if(doc.swatches.begin(), doc.swatches.end(), [&](const auto& s) { return s.first == name; });
                if (boolParam(p, "remove", false)) {
                    if (it == doc.swatches.end()) return Result::failure("no swatch '" + name + "'");
                    doc.swatches.erase(it);
                    for (auto& s : doc.shapes) {
                        if (s.fillSwatch == name) s.fillSwatch.clear();
                        if (s.strokeSwatch == name) s.strokeSwatch.clear();
                    }
                    return Result::success("deleted swatch " + name);
                }
                std::string error;
                std::string text;
                if (has(p, "color")) {
                    auto c = parseColor(stringParam(p, "color"));
                    if (!c) return Result::failure("'" + stringParam(p, "color") + "' isn't a colour");
                    if (it == doc.swatches.end()) doc.swatches.push_back({name, *c});
                    else it->second = *c;
                    int changed = 0;
                    for (auto& s : doc.shapes) {
                        if (s.fillSwatch == name) s.fill = *c, ++changed;
                        if (s.strokeSwatch == name) s.stroke = *c, ++changed;
                    }
                    text = "swatch " + name + " = " + colorText(*c) + (changed ? " (" + std::to_string(changed) + " uses updated)" : "");
                }
                const auto sw = doc.swatch(name);
                if (!sw) return Result::failure("no swatch '" + name + "' yet; give color= to define it");
                if (has(p, "shapes") || has(p, "shape")) {
                    std::vector<int> idx;
                    if (!selection(doc, p, idx, error)) return Result::failure(error);
                    const bool stroke = stringParam(p, "target", "fill") == "stroke";
                    for (int i : leaves(doc, idx)) {
                        Shape& s = doc.shapes[static_cast<std::size_t>(i)];
                        (stroke ? s.stroke : s.fill) = *sw;
                        (stroke ? s.strokeSwatch : s.fillSwatch) = name;
                        if (!stroke) s.gradient.reset();
                    }
                    text += std::string(text.empty() ? "" : "; ") + "painted " + idText(doc, idx) + " " + (stroke ? "outline" : "fill") + " with " + name;
                }
                Result r = Result::success(text.empty() ? "swatch " + name + " = " + colorText(*sw) : text, name);
                r.with("name", name).with("color", colorText(*sw));
                return maybePreview(r, doc, p);
            }));
}

}  // namespace wetwaresuite::vector::detail
