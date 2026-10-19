// The Layers panel for image documents, like the one in Photoshop, GIMP or
// Krita: the stack top first with groups folded in, eye and selection per
// layer, the active layer's blend mode, opacity, fill, locks and clipping,
// buttons to add, group, copy, delete, merge and reorder, adjustment layers
// with their settings as sliders, text and shape layer editing and layer
// styles. Every change runs a paint.* or photo.* command through
// Session::run, so it undoes and scripts like any other action.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "shell.hpp"
#include "tool_panels.hpp"
#include "wetwaresuite/raster/adjustments.hpp"
#include "wetwaresuite/raster/document.hpp"

namespace wetwaresuite::gui {

namespace {

// Slider values while they're being dragged (the command runs when the slider is let go).
std::map<std::string, float>& dragging() {
    static std::map<std::string, float> d;
    return d;
}

// A slider that runs `apply` once, when the user lets go after changing it.
template <typename Fn>
void liveSlider(const std::string& key, const char* label, float value, float lo, float hi, const char* format, Fn apply) {
    auto& d = dragging();
    auto it = d.find(key);
    float v = it != d.end() ? it->second : value;
    if (ImGui::SliderFloat(label, &v, lo, hi, format)) d[key] = v;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        d.erase(key);
        apply(v);
    } else if (!ImGui::IsItemActive()) {
        d.erase(key);
    }
}

std::string kindTag(const raster::Layer& l) {
    switch (l.kind) {
        case raster::LayerKind::Group: return l.collapsed ? "[+] " : "[-] ";
        case raster::LayerKind::Adjustment: return "(adj) ";
        case raster::LayerKind::Text: return "(T) ";
        case raster::LayerKind::Shape: return "(shape) ";
        default: return "";
    }
}

// "drop_shadow:color=#000000ff,opacity=0.75;stroke:size=3" -> effect -> key -> value
std::map<std::string, std::map<std::string, std::string>> styleValues(const raster::LayerStyle& st) {
    std::map<std::string, std::map<std::string, std::string>> out;
    const std::string text = raster::styleToText(st);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string part = text.substr(pos, end - pos);
        const std::size_t colon = part.find(':');
        const std::string effect = part.substr(0, colon);
        auto& kv = out[effect];
        if (colon != std::string::npos) {
            std::size_t p = colon + 1;
            while (p < part.size()) {
                std::size_t e = part.find(',', p);
                if (e == std::string::npos) e = part.size();
                const std::string item = part.substr(p, e - p);
                const std::size_t eq = item.find('=');
                if (eq != std::string::npos) kv[item.substr(0, eq)] = item.substr(eq + 1);
                p = e + 1;
            }
        }
        pos = end + 1;
    }
    return out;
}

double toNumber(const std::string& s, double fallback) {
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

ImVec4 toColour(const std::string& hex, ImVec4 fallback) {
    auto c = raster::Color::parse(hex);
    if (!c) return fallback;
    return {c->r / 255.0f, c->g / 255.0f, c->b / 255.0f, c->a / 255.0f};
}

}  // namespace

void Shell::layersPanel() {
    auto doc = std::dynamic_pointer_cast<raster::ImageDoc>(session_.workspace().get(currentDoc_));
    if (!doc || doc->image.layers.empty() || !find("paint.layer_set")) return;
    ImGui::Spacing();
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushID("layers");
    const std::string id = currentDoc_;
    // runs a layer command; only failures are shown, so clicking around stays quiet
    auto act = [&](const std::string& command, Params p, const char* title) {
        if (!find(command)) return;
        p["doc"] = id;
        Result r = run(command, p);
        if (!r.ok) {
            resultFor_ = title;
            r.message = friendlyError(r.message);
            result_ = std::move(r);
        }
        canvas_.dirty = true;
    };
    const auto& L = doc->image.layers;
    const int n = static_cast<int>(L.size());
    const int active = std::clamp(doc->active, 0, n - 1);
    const float unit = ImGui::GetFontSize();

    // ---- the stack, top first ----
    ImGui::BeginChild("stack", {0, std::min(unit * 1.6f * static_cast<float>(n) + unit, unit * 16)}, ImGuiChildFlags_Borders);
    int hideBelowDepth = -1;  // inside a folded group: skip its members
    for (int i = n - 1; i >= 0; --i) {
        const raster::Layer& l = L[static_cast<std::size_t>(i)];
        if (hideBelowDepth >= 0) {
            if (l.depth > hideBelowDepth) continue;
            hideBelowDepth = -1;
        }
        if (l.kind == raster::LayerKind::Group && l.collapsed) hideBelowDepth = l.depth;
        ImGui::PushID(i);
        bool visible = l.visible;
        if (ImGui::Checkbox("##eye", &visible)) act("paint.layer_set", {{"layer", std::int64_t{i}}, {"visible", visible}}, "Show layer");
        ImGui::SetItemTooltip("Show or hide");
        ImGui::SameLine();
        if (l.depth > 0) {
            ImGui::Dummy({unit * static_cast<float>(l.depth), 1});
            ImGui::SameLine();
        }
        if (l.kind == raster::LayerKind::Group) {
            if (ImGui::SmallButton(l.collapsed ? "+" : "-")) act("paint.layer_set", {{"layer", std::int64_t{i}}, {"collapsed", !l.collapsed}}, "Fold group");
            ImGui::SameLine();
        }
        std::string label = std::string(l.clip ? "-> " : "") + kindTag(l) + l.name;
        if (!l.mask.empty()) label += l.maskEnabled ? "  [mask]" : "  [mask off]";
        if (l.style.any()) label += "  fx";
        if (l.lockAlpha) label += "  [lock]";
        if (ImGui::Selectable(label.c_str(), i == active)) act("paint.layer_select", {{"layer", std::int64_t{i}}}, "Select layer");
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- buttons ----
    auto button = [&](const char* text, const char* tip) {
        tp::flow(text);
        const bool pressed = ImGui::SmallButton(text);
        ImGui::SetItemTooltip("%s", tip);
        return pressed;
    };
    if (button("New", "Add an empty layer above this one")) act("paint.layer_add", {}, "Add layer");
    if (button("Group", "Put this layer in a new group")) act("paint.group", {{"from", std::int64_t{active}}, {"to", std::int64_t{active}}}, "Group");
    if (L[static_cast<std::size_t>(active)].kind == raster::LayerKind::Group && button("Ungroup", "Remove the group, keep its layers")) {
        act("paint.ungroup", {{"layer", std::int64_t{active}}}, "Ungroup");
    }
    if (button("Copy", "Duplicate the layer (a group with everything in it)")) act("paint.layer_duplicate", {{"layer", std::int64_t{active}}}, "Duplicate");
    if (button("Delete", "Delete the layer")) act("paint.layer_delete", {{"layer", std::int64_t{active}}}, "Delete layer");
    if (button("Up", "Move up the stack")) act("paint.layer_move", {{"layer", std::int64_t{active}}, {"to", std::int64_t{std::min(active + 1, n - 1)}}}, "Move layer");
    if (button("Down", "Move down the stack")) {
        const int s = raster::groupStart(L, active);
        act("paint.layer_move", {{"layer", std::int64_t{active}}, {"to", std::int64_t{s > 0 ? active - 1 : active}}}, "Move layer");
    }
    if (button("Merge down", "Merge into the layer below (applies an adjustment layer)")) act("paint.layer_merge_down", {{"layer", std::int64_t{active}}}, "Merge down");
    if (button("Merge visible", "Merge every visible layer into one")) act("paint.merge_visible", {}, "Merge visible");
    if (button("Flatten", "Merge everything into one layer")) act("paint.flatten", {}, "Flatten");
    if (button("Rasterize", "Turn text, shapes, styles or a group into plain pixels")) act("paint.rasterize", {{"layer", std::int64_t{active}}}, "Rasterize");
    if (find("photo.mask") && L[static_cast<std::size_t>(active)].mask.empty() && button("Add mask", "Hide parts without erasing (from the selection when there is one)")) {
        act("photo.mask", {{"layer", std::int64_t{active}}, {"action", std::string("add")}, {"from", std::string(doc->hasSelection() ? "selection" : "reveal_all")}}, "Mask");
    }

    // adjustment layers
    if (find("photo.adjustment_layer")) {
        ImGui::SetNextItemWidth(unit * 11);
        if (ImGui::BeginCombo("##adj", "Add adjustment...")) {
            for (const auto& t : raster::adjustmentLayerTypes()) {
                if (!ImGui::Selectable(t.c_str())) continue;
                Params p{{"type", t}};
                if (t == "curves") p["preset"] = std::string("contrast");  // curves needs a starting curve
                act("photo.adjustment_layer", p, "Adjustment layer");
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Levels, curves, hue/saturation... that change everything below without touching it");
    }

    // ---- the active layer ----
    const raster::Layer& l = L[static_cast<std::size_t>(active)];
    const std::string k = id + "/" + std::to_string(active) + "/";
    ImGui::SeparatorText(l.name.c_str());
    ImGui::SetNextItemWidth(unit * 9);
    const std::string blendName = raster::toString(l.blend);
    if (ImGui::BeginCombo("Blend", blendName.c_str())) {
        for (const auto& b : raster::blendNames()) {
            if (ImGui::Selectable(b.c_str(), b == blendName)) act("paint.layer_set", {{"layer", std::int64_t{active}}, {"blend", b}}, "Blend mode");
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(unit * 9);
    liveSlider(k + "opacity", "Opacity", static_cast<float>(l.opacity * 100), 0, 100, "%.0f%%",
               [&](float v) { act("paint.layer_set", {{"layer", std::int64_t{active}}, {"opacity", static_cast<double>(v)}}, "Opacity"); });
    if (l.kind != raster::LayerKind::Group && l.kind != raster::LayerKind::Adjustment) {
        ImGui::SetNextItemWidth(unit * 9);
        liveSlider(k + "fill", "Fill", static_cast<float>(l.fill * 100), 0, 100, "%.0f%%",
                   [&](float v) { act("paint.layer_set", {{"layer", std::int64_t{active}}, {"fill", static_cast<double>(v)}}, "Fill"); });
        ImGui::SetItemTooltip("Fades the layer's pixels but not its style (shadows, strokes...)");
        bool lock = l.lockAlpha;
        if (ImGui::Checkbox("Lock transparency", &lock)) act("paint.layer_set", {{"layer", std::int64_t{active}}, {"lock_alpha", lock}}, "Lock");
        ImGui::SameLine();
    }
    bool clip = l.clip;
    if (active > 0 && ImGui::Checkbox("Clip to below", &clip)) act("paint.layer_set", {{"layer", std::int64_t{active}}, {"clip", clip}}, "Clipping mask");
    if (l.kind == raster::LayerKind::Group) {
        bool pass = l.passThrough;
        if (ImGui::Checkbox("Pass through", &pass)) act("paint.layer_set", {{"layer", std::int64_t{active}}, {"pass_through", pass}}, "Group mode");
        ImGui::SetItemTooltip("Members blend straight into what's below; off blends the group as one picture");
    }

    // adjustment settings as sliders
    if (l.kind == raster::LayerKind::Adjustment && l.settings.count("type")) {
        const std::string type = l.settings.at("type");
        const std::string range = l.settings.count("range") ? l.settings.at("range") : "reds";
        ImGui::TextDisabled("%s", type.c_str());
        for (const auto& f : raster::adjustmentFields(type)) {
            const bool ink = type == "selective_color" && (f.key == "cyan" || f.key == "magenta" || f.key == "yellow" || f.key == "black");
            const std::string stored = ink ? range + "." + f.key : f.key;
            const std::string cur = l.settings.count(stored) ? l.settings.at(stored) : f.fallback;
            ImGui::SetNextItemWidth(unit * 9);
            using K = raster::AdjustmentField::Kind;
            if (f.kind == K::Number) {
                liveSlider(k + f.key, f.key.c_str(), static_cast<float>(toNumber(cur, 0)), static_cast<float>(f.min), static_cast<float>(f.max), "%.2f",
                           [&](float v) { act("photo.adjustment_edit", {{"layer", std::int64_t{active}}, {f.key, static_cast<double>(v)}}, "Adjustment"); });
            } else if (f.kind == K::Bool) {
                bool b = cur == "true";
                if (ImGui::Checkbox(f.key.c_str(), &b)) act("photo.adjustment_edit", {{"layer", std::int64_t{active}}, {f.key, b}}, "Adjustment");
            } else if (f.kind == K::Choice) {
                if (ImGui::BeginCombo(f.key.c_str(), cur.empty() ? "(none)" : cur.c_str())) {
                    for (const auto& c : f.choices) {
                        if (!c.empty() && ImGui::Selectable(c.c_str(), c == cur)) act("photo.adjustment_edit", {{"layer", std::int64_t{active}}, {f.key, c}}, "Adjustment");
                    }
                    ImGui::EndCombo();
                }
            } else if (f.kind == K::Color) {
                ImVec4 c = toColour(cur, {1, 1, 1, 1});
                float rgba[4] = {c.x, c.y, c.z, c.w};
                ImGui::ColorEdit4(f.key.c_str(), rgba, ImGuiColorEditFlags_NoInputs);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    act("photo.adjustment_edit", {{"layer", std::int64_t{active}}, {f.key, toHex({rgba[0], rgba[1], rgba[2], rgba[3]})}}, "Adjustment");
                }
            } else {  // lists and colour lists: typed
                static std::map<std::string, std::string> typed;
                auto& buf = typed[k + f.key];
                if (buf.empty()) buf = cur;
                char text[256];
                std::snprintf(text, sizeof text, "%s", buf.c_str());
                if (ImGui::InputText(f.key.c_str(), text, sizeof text, ImGuiInputTextFlags_EnterReturnsTrue)) {
                    Params p{{"layer", std::int64_t{active}}};
                    if (f.kind == K::List) {
                        NumberList nums;
                        for (const char* c = text; *c;) {
                            char* end = nullptr;
                            const double v = std::strtod(c, &end);
                            if (end == c) {
                                ++c;
                                continue;
                            }
                            nums.push_back(v);
                            c = end;
                        }
                        p[f.key] = nums;
                    } else {
                        p[f.key] = std::string(text);
                    }
                    act("photo.adjustment_edit", p, "Adjustment");
                    buf.clear();
                } else {
                    buf = text;
                }
                ImGui::SetItemTooltip("%s (Enter applies)", f.description.c_str());
            }
        }
    }

    // text layers
    if (l.kind == raster::LayerKind::Text && find("paint.text_edit")) {
        static std::map<std::string, std::string> typed;
        auto& buf = typed[k + "text"];
        const std::string cur = l.settings.count("text") ? l.settings.at("text") : "";
        if (buf.empty()) buf = cur;
        char text[1024];
        std::snprintf(text, sizeof text, "%s", buf.c_str());
        ImGui::SetNextItemWidth(unit * 12);
        if (ImGui::InputText("Text", text, sizeof text, ImGuiInputTextFlags_EnterReturnsTrue)) {
            act("paint.text_edit", {{"layer", std::int64_t{active}}, {"text", std::string(text)}}, "Edit text");
            buf.clear();
        } else {
            buf = text;
        }
        ImGui::SetNextItemWidth(unit * 9);
        liveSlider(k + "size", "Font size", static_cast<float>(toNumber(l.settings.count("size") ? l.settings.at("size") : "32", 32)), 4, 400, "%.0f px",
                   [&](float v) { act("paint.text_edit", {{"layer", std::int64_t{active}}, {"size", static_cast<double>(v)}}, "Edit text"); });
        ImVec4 c = toColour(l.settings.count("color") ? l.settings.at("color") : "#000000", {0, 0, 0, 1});
        float rgba[4] = {c.x, c.y, c.z, c.w};
        ImGui::ColorEdit4("Text colour", rgba, ImGuiColorEditFlags_NoInputs);
        if (ImGui::IsItemDeactivatedAfterEdit()) act("paint.text_edit", {{"layer", std::int64_t{active}}, {"color", toHex({rgba[0], rgba[1], rgba[2], rgba[3]})}}, "Edit text");
    } else if (find("paint.text")) {
        static char newText[256] = "";
        ImGui::SetNextItemWidth(unit * 9);
        if (ImGui::InputTextWithHint("##newtext", "Type, Enter adds text", newText, sizeof newText, ImGuiInputTextFlags_EnterReturnsTrue) && newText[0]) {
            act("paint.text", {{"text", std::string(newText)}, {"size", static_cast<double>(std::max(12, doc->image.height / 12))},
                               {"color", toHex({canvas_.color[0], canvas_.color[1], canvas_.color[2], canvas_.color[3]})}}, "Text layer");
            newText[0] = 0;
        }
        ImGui::SetItemTooltip("Adds an editable text layer in the brush colour");
    }

    // layer styles
    if (l.kind != raster::LayerKind::Group && l.kind != raster::LayerKind::Adjustment && find("paint.layer_style") &&
        ImGui::TreeNode("Layer style")) {
        const auto values = styleValues(l.style);
        for (const auto& effect : raster::styleEffectNames()) {
            ImGui::PushID(effect.c_str());
            const bool onNow = values.count(effect) > 0;
            bool on = onNow;
            if (ImGui::Checkbox(effect.c_str(), &on)) act("paint.layer_style", {{"layer", std::int64_t{active}}, {"effect", effect}, {"on", on}}, "Layer style");
            if (onNow) {
                const auto& kv = values.at(effect);
                ImGui::Indent();
                for (const auto& key : raster::styleEffectKeys(effect)) {
                    const std::string cur = kv.count(key) ? kv.at(key) : "";
                    ImGui::SetNextItemWidth(unit * 8);
                    auto set = [&](Value v) { act("paint.layer_style", {{"layer", std::int64_t{active}}, {"effect", effect}, {key, v}}, "Layer style"); };
                    if (key == "color" || key == "to") {
                        ImVec4 c = toColour(cur, {0, 0, 0, 1});
                        float rgba[4] = {c.x, c.y, c.z, c.w};
                        ImGui::ColorEdit4(key.c_str(), rgba, ImGuiColorEditFlags_NoInputs);
                        if (ImGui::IsItemDeactivatedAfterEdit()) set(toHex({rgba[0], rgba[1], rgba[2], rgba[3]}));
                    } else if (key == "opacity" || key == "spread" || key == "highlight" || key == "shadow") {
                        liveSlider(k + effect + key, key.c_str(), static_cast<float>(toNumber(cur, 0) * 100), 0, 100, "%.0f%%",
                                   [&](float v) { set(static_cast<double>(v)); });
                    } else if (key == "size" || key == "distance") {
                        liveSlider(k + effect + key, key.c_str(), static_cast<float>(toNumber(cur, 0)), 0, 100, "%.0f px", [&](float v) { set(static_cast<double>(v)); });
                    } else if (key == "angle" || key == "altitude") {
                        liveSlider(k + effect + key, key.c_str(), static_cast<float>(toNumber(cur, 0)), key == "angle" ? -180.0f : 0.0f, key == "angle" ? 180.0f : 90.0f,
                                   "%.0f deg", [&](float v) { set(static_cast<double>(v)); });
                    } else if (key == "depth") {
                        liveSlider(k + effect + key, key.c_str(), static_cast<float>(toNumber(cur, 1)), 0.1f, 10, "%.1f", [&](float v) { set(static_cast<double>(v)); });
                    } else if (key == "radial" || key == "down") {
                        bool b = cur == "true";
                        if (ImGui::Checkbox(key.c_str(), &b)) set(b);
                    } else if (key == "position" || key == "blend") {
                        const std::vector<std::string> choices = key == "position" ? std::vector<std::string>{"outside", "inside", "center"} : raster::blendNames();
                        if (ImGui::BeginCombo(key.c_str(), cur.c_str())) {
                            for (const auto& c : choices) {
                                if (ImGui::Selectable(c.c_str(), c == cur)) set(c);
                            }
                            ImGui::EndCombo();
                        }
                    }
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

}  // namespace wetwaresuite::gui
