// The Colours part of the Aa menu: three colour pickers, ready-made sets and
// a reset, all going through the app.palette_* commands so the choice is
// shared by every app (and scripts and Claude can do the same).
#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

bool fieldBool(const Result& r, const std::string& name) {
    const Value* v = r.field(name);
    const bool* b = v ? std::get_if<bool>(v) : nullptr;
    return b && *b;
}

std::string firstLine(const std::string& text) { return text.substr(0, text.find('\n')); }

}  // namespace

bool Shell::loadPalette() {
    if (palettePending_ || !session_.registry().find("app.palette_get")) return false;
    const Result r = session_.run("app.palette_get", {});
    if (!r.ok) return false;
    const bool custom = fieldBool(r, "custom");
    Palette p = defaultPalette(app_.accent);
    if (!dark_) p.secondary = fromRgb24(0xebebed), p.tertiary = fromRgb24(0x737379);  // the light look's own greys
    if (custom) {
        auto a = parseHex(fieldText(r, "primary")), b = parseHex(fieldText(r, "secondary")), c = parseHex(fieldText(r, "tertiary"));
        if (a && b && c) p = {*a, *b, *c};
    }
    const bool changed = custom != custom_ || (custom && !(p == palette_));
    custom_ = custom;
    palette_ = p;
    return changed;
}

void Shell::runPalette(const std::string& command, const Params& params) {
    if (!session_.registry().find(command)) {
        paletteNote_ = "This build can't save colours.";
        paletteNoteOk_ = false;
        return;
    }
    const Result r = session_.run(command, params);
    paletteNoteOk_ = r.ok;
    paletteNote_ = r.ok ? firstLine(r.message) : friendlyError(r.message);
    palettePending_ = false;
    loadPalette();
    applyStyle(dpi_);
}

void Shell::savePaletteNow() {
    palettePending_ = false;
    runPalette("app.palette_set",
               {{"primary", hex6(palette_.primary)}, {"secondary", hex6(palette_.secondary)}, {"tertiary", hex6(palette_.tertiary)}});
}

void Shell::coloursSettings() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Colours");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
    ImGui::TextDisabled("%s", custom_ ? "Your colours. Every wetwaresuite app uses them."
                                      : "This app's own colours. Pick your own and every app uses them.");
    if (!custom_) {
        if (ImGui::RadioButton("Dark", dark_)) {
            dark_ = true;
            loadPalette();
            applyStyle(dpi_);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Light", !dark_)) {
            dark_ = false;
            loadPalette();
            applyStyle(dpi_);
        }
    } else {
        ImGui::TextDisabled("Dark or light follows the Panels colour.");
    }

    struct Pick {
        const char* label;
        const char* help;
        Rgba* colour;
    };
    const Pick picks[] = {
        {"Main colour", "Primary: buttons, headings and highlights.", &palette_.primary},
        {"Panels", "Secondary: the boxes and panels. A dark one gives a dark look, a pale one a light look.", &palette_.secondary},
        {"Details", "Tertiary: links, borders, selected text and the keyboard focus.", &palette_.tertiary},
    };
    for (const Pick& p : picks) {
        float v[3] = {p.colour->r, p.colour->g, p.colour->b};
        ImGui::PushID(p.label);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
        if (ImGui::ColorEdit3("##c", v, ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoOptions)) {
            *p.colour = {v[0], v[1], v[2], 1};
            custom_ = true;  // shown right away, saved once the mouse is let go
            palettePending_ = true;
            applyStyle(dpi_);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(p.label);
        ImGui::SetItemTooltip("%s", p.help);
        ImGui::PopID();
    }
    ImGui::TextDisabled("Text and button shades adjust so everything stays readable.");

    ImGui::TextUnformatted("Ready-made");
    const float maxX = ImGui::GetCursorScreenPos().x + ImGui::GetFontSize() * 22;
    bool first = true;
    for (const auto& preset : palettePresets()) {
        const float w = ImGui::CalcTextSize(preset.title.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        if (!first && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + w <= maxX) ImGui::SameLine();
        first = false;
        if (ImGui::Button(preset.title.c_str())) runPalette("app.palette_preset", {{"name", preset.name}});
        ImGui::SetItemTooltip("%s", preset.description.c_str());
    }
    ImGui::BeginDisabled(!custom_);
    if (ImGui::Button("Reset to this app's own colours")) runPalette("app.palette_reset", {});
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Forget the picked colours: every app goes back to its own.");
    if (!paletteNote_.empty()) {
        if (paletteNoteOk_) ImGui::TextDisabled("%s", paletteNote_.c_str());
        else ImGui::TextColored(darkLook() ? ImVec4{1.0f, 0.55f, 0.5f, 1} : ImVec4{0.7f, 0.0f, 0.0f, 1}, "%s", paletteNote_.c_str());
    }
    ImGui::PopTextWrapPos();
}

}  // namespace wetwaresuite::gui
