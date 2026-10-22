// Adjustment layers (non-destructive levels, curves, hue/saturation... that change
// everything below them while they're there) and the same adjustments applied
// once to a layer's pixels.
#include <algorithm>
#include <cstdio>

#include "commands.hpp"
#include "wetwaresuite/raster/adjustments.hpp"

namespace wetwaresuite::photo {

using namespace raster;

namespace {

std::string numText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

ParamType typeOf(AdjustmentField::Kind k) {
    switch (k) {
        case AdjustmentField::Kind::Number: return ParamType::Number;
        case AdjustmentField::Kind::List: return ParamType::NumberList;
        case AdjustmentField::Kind::Bool: return ParamType::Bool;
        default: return ParamType::String;
    }
}

// The settings params every adjustment command takes: the union of what all types read.
void addFieldParams(std::vector<ParamSpec>& v) {
    for (const auto& f : allAdjustmentFields()) {
        ParamSpec spec{f.key, typeOf(f.kind), f.description, false, {}, {}};
        if (f.kind == AdjustmentField::Kind::Choice) {
            for (const auto& c : f.choices) {
                if (!c.empty()) spec.choices.push_back(c);
            }
        }
        v.push_back(spec);
    }
}

std::string valueText(const Params& p, const AdjustmentField& f) {
    switch (f.kind) {
        case AdjustmentField::Kind::Number: return numText(numberParam(p, f.key));
        case AdjustmentField::Kind::Bool: return boolParam(p, f.key) ? "true" : "false";
        case AdjustmentField::Kind::List: {
            std::string out;
            for (double d : listParam(p, f.key)) out += (out.empty() ? "" : ",") + numText(d);
            return out;
        }
        default: return stringParam(p, f.key);
    }
}

// Copies the params `type` reads into settings. Selective colour keeps one set of inks per colour
// range, so its cyan/magenta/yellow/black go under "<range>.".
bool readSettings(const std::string& type, const Params& p, Settings& s, std::string& error) {
    s["type"] = type;
    const std::string range = type == "selective_color" ? stringParam(p, "range", s.count("range") ? s.at("range") : "reds") : "";
    for (const auto& f : adjustmentFields(type)) {
        if (!p.count(f.key)) continue;
        const bool ink = type == "selective_color" && (f.key == "cyan" || f.key == "magenta" || f.key == "yellow" || f.key == "black");
        s[ink ? range + "." + f.key : f.key] = valueText(p, f);
    }
    // params that belong to other types are a mistake worth pointing out
    for (const auto& f : allAdjustmentFields()) {
        if (!p.count(f.key)) continue;
        const auto& mine = adjustmentFields(type);
        if (std::none_of(mine.begin(), mine.end(), [&](const AdjustmentField& m) { return m.key == f.key; })) {
            std::string keys;
            for (const auto& m : mine) keys += (keys.empty() ? "" : ", ") + m.key;
            error = type + " has no '" + f.key + "' setting" + (keys.empty() ? " (it has none)" : " (it has: " + keys + ")");
            return false;
        }
    }
    error = checkAdjustment(type, s);
    return error.empty();
}

std::string describe(const Settings& s) {
    std::string out;
    for (const auto& [k, v] : s) {
        if (k != "type") out += (out.empty() ? "" : ", ") + k + "=" + v;
    }
    return out.empty() ? "default settings" : out;
}

std::vector<ParamSpec> layerSpecs() {
    std::vector<ParamSpec> v = {docParam(),
                                {"type", ParamType::String, "which adjustment", true, adjustmentLayerTypes()},
                                {"layer", ParamType::Int, "put it just above this layer (default: the active one)", false},
                                {"name", ParamType::String, "layer name", false},
                                {"clip", ParamType::Bool, "change only the layer below (clipping mask) instead of everything below", false, {}, false},
                                {"opacity", ParamType::Number, "0..100, how strongly it applies", false, {}, 100.0}};
    addFieldParams(v);
    v.push_back(previewParam());
    return v;
}

Result addAdjustmentLayer(ImageDoc& doc, const Params& p) {
    const std::string type = stringParam(p, "type");
    Settings s;
    std::string error;
    if (!readSettings(type, p, s, error)) return Result::failure(error);
    if (p.count("layer")) {
        const int at = layerIndex(doc, p, error);
        if (at < 0) return Result::failure(error);
        doc.active = at;
    }
    Layer l(stringParam(p, "name", type));
    l.kind = LayerKind::Adjustment;
    l.settings = std::move(s);
    l.opacity = std::clamp(numberParam(p, "opacity", 100), 0.0, 100.0) / 100;
    l.clip = boolParam(p, "clip", false);
    if (doc.hasSelection()) l.mask = doc.selection;  // like other editors: the selection becomes its mask
    const int idx = insertLayer(doc, std::move(l));
    if (idx == 0) doc.image.layers[0].clip = false;
    const Layer& made = doc.image.layers[static_cast<std::size_t>(idx)];
    Result r = Result::success("added " + type + " adjustment layer " + std::to_string(idx) + " (" + describe(made.settings) + ")" +
                               (made.mask.empty() ? "" : ", masked to the selection"));
    r.with("doc", stringParam(p, "doc")).with("layer", static_cast<std::int64_t>(idx));
    return maybePreview(r, doc, p);
}

Result editAdjustmentLayer(ImageDoc& doc, const Params& p) {
    std::string error;
    const int idx = layerIndex(doc, p, error);
    if (idx < 0) return Result::failure(error);
    Layer& l = doc.image.layers[static_cast<std::size_t>(idx)];
    if (l.kind != LayerKind::Adjustment) return Result::failure("layer " + std::to_string(idx) + " isn't an adjustment layer");
    const std::string type = stringParam(p, "type", l.settings.count("type") ? l.settings.at("type") : "");
    Settings s = type == (l.settings.count("type") ? l.settings.at("type") : "") ? l.settings : Settings{};
    if (boolParam(p, "reset", false)) s.clear();
    if (!readSettings(type, p, s, error)) return Result::failure(error);
    l.settings = std::move(s);
    if (p.count("opacity")) l.opacity = std::clamp(numberParam(p, "opacity", 100), 0.0, 100.0) / 100;
    doc.active = idx;
    doc.markAllChanged();
    Result r = Result::success("adjustment layer " + std::to_string(idx) + ": " + type + " (" + describe(l.settings) + ")");
    r.with("doc", stringParam(p, "doc")).with("layer", static_cast<std::int64_t>(idx));
    return maybePreview(r, doc, p);
}

Result applyToPixels(ImageDoc& doc, const Params& p) {
    const std::string type = stringParam(p, "type");
    Settings s;
    std::string error;
    if (!readSettings(type, p, s, error)) return Result::failure(error);
    std::vector<int> targets;
    if (boolParam(p, "all_layers", false)) {
        for (int i = 0; i < static_cast<int>(doc.image.layers.size()); ++i) {
            if (doc.image.layers[static_cast<std::size_t>(i)].hasPixels() && pixelLayer(doc, i, error)) targets.push_back(i);
        }
    } else {
        const int idx = pixelLayerIndex(doc, p, error);
        if (idx < 0) return Result::failure(error);
        targets.push_back(idx);
    }
    for (int i : targets) applyAdjustment(doc.image.layers[static_cast<std::size_t>(i)].image, type, s);
    Result r = Result::success(type + " applied (" + describe(s) + ")" + (doc.hasSelection() ? " inside the selection" : ""));
    r.with("doc", stringParam(p, "doc"));
    return maybePreview(r, doc, p);
}

}  // namespace

void registerAdjustmentCommands(Session& session) {
    auto& reg = session.registry();
    std::string types;
    for (const auto& t : adjustmentLayerTypes()) types += (types.empty() ? "" : ", ") + t;

    reg.add({"photo.adjustment_layer",
             "Adjustment layer",
             "Add an adjustment layer: it changes the look of every layer below it (or only the one below with clip=true) "
             "without touching their pixels, so it can be edited, hidden, masked or deleted later. Types: " + types +
                 ". Each type reads its own settings (the descriptions say which); with a selection, the layer is masked to it.",
             layerSpecs(), true},
            imageCommand(session, addAdjustmentLayer));

    std::vector<ParamSpec> edit = {docParam(), layerParam(),
                                   {"type", ParamType::String, "change it to another adjustment", false, adjustmentLayerTypes()},
                                   {"reset", ParamType::Bool, "start from the default settings", false, {}, false},
                                   {"opacity", ParamType::Number, "0..100", false}};
    addFieldParams(edit);
    edit.push_back(previewParam());
    reg.add({"photo.adjustment_edit", "Edit adjustment layer",
             "Change an adjustment layer's settings; only the ones you give change (reset=true starts over).", edit, true},
            imageCommand(session, editAdjustmentLayer));

    std::vector<ParamSpec> apply = {docParam(), {"type", ParamType::String, "which adjustment", true, adjustmentLayerTypes()}, layerParam(),
                                    {"all_layers", ParamType::Bool, "apply to every layer instead of one", false, {}, false}};
    addFieldParams(apply);
    apply.push_back(previewParam());
    reg.add({"photo.apply_adjustment", "Apply adjustment",
             "Apply one of the adjustment-layer adjustments straight to a layer's pixels (inside the selection when there is one).",
             apply, true},
            imageCommand(session, applyToPixels));
}

}  // namespace wetwaresuite::photo
