// The 3D view for cad parts, meshes, sculpts, scan fits and prints: drag to
// turn the model, right- or middle-drag to move it, scroll to zoom, double-click
// to fit. The document draws itself (Document::renderView, the same software
// renderer app.view uses headless), so it runs without a GPU. Frames drawn
// mid-drag skip antialiasing; letting go redraws it sharp.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

struct Angle {
    const char* name;
    double yaw, pitch;
    const char* tip;
};
constexpr Angle kAngles[] = {{"Iso", 45, 35.26, "From the front-right, above"},
                             {"Front", 0, 0, "Straight on from the front"},
                             {"Right", 90, 0, "From the right side"},
                             {"Top", 0, 89.9, "Looking down from above"}};

}  // namespace

bool Shell::probe3d(const Document& doc) const {
    std::vector<std::uint8_t> rgba;
    return doc.renderView(OrbitView{}, 0, 0, rgba);  // 0 x 0 only asks
}

void Shell::orbitToolbar() {
    for (const auto& a : kAngles) {
        if (ImGui::Button(a.name)) {
            canvas_.orbit.yaw = a.yaw;
            canvas_.orbit.pitch = a.pitch;
            canvas_.orbitDirty = true;
        }
        ImGui::SetItemTooltip("%s", a.tip);
        ImGui::SameLine();
    }
    if (ImGui::Button("Fit")) {
        canvas_.orbit.zoom = 1;
        canvas_.orbit.panX = canvas_.orbit.panY = 0;
        canvas_.orbitDirty = true;
    }
    ImGui::SetItemTooltip("Show the whole model (or double-click it)");
    ImGui::SameLine();
    ImGui::TextDisabled("drag to turn, right-drag to move, scroll to zoom");
}

void Shell::orbitCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active) {
    ImGuiIO& io = ImGui::GetIO();
    OrbitView& o = canvas_.orbit;
    const bool left = active && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool move = active && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                 (left && io.KeyShift));
    const float minSide = std::max(1.0f, std::min(size.x, size.y));
    if ((io.MouseDelta.x != 0 || io.MouseDelta.y != 0) && (left || move)) {
        if (move) {
            // a model radius is about half the shorter side at zoom 1
            const double perPx = 2.0 / (minSide * 0.88 * o.zoom);
            o.panX += io.MouseDelta.x * perPx;
            o.panY -= io.MouseDelta.y * perPx;
        } else {
            o.yaw -= io.MouseDelta.x * 0.4;
            o.pitch = std::clamp(o.pitch + io.MouseDelta.y * 0.4, -89.9, 89.9);
        }
        canvas_.orbitDirty = true;
        canvas_.orbitDragging = true;
    }
    if (hovered && io.MouseWheel != 0) {
        o.zoom = std::clamp(o.zoom * std::pow(1.15, static_cast<double>(io.MouseWheel)), 0.05, 200.0);
        canvas_.orbitDirty = true;
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        o.zoom = 1;
        o.panX = o.panY = 0;
        canvas_.orbitDirty = true;
    }
    if (canvas_.orbitDragging && !active) {
        canvas_.orbitDragging = false;
        canvas_.orbitDirty = true;  // redraw sharp
    }
    const int w = static_cast<int>(size.x), h = static_cast<int>(size.y);
    if (w != canvas_.orbitW || h != canvas_.orbitH) canvas_.orbitDirty = true;
    if (canvas_.orbitDirty && w >= 8 && h >= 8) {
        canvas_.orbitDirty = false;
        canvas_.orbitW = w;
        canvas_.orbitH = h;
        if (auto doc = session_.workspace().get(currentDoc_)) {
            std::vector<std::uint8_t> rgba;
            o.antialias = !canvas_.orbitDragging && io.MouseWheel == 0;
            if (doc->renderView(o, w, h, rgba)) canvas_.texture.upload(renderer_, rgba.data(), w, h);
            if (!o.antialias) canvas_.orbitSharpPending = true;
        }
    } else if (canvas_.orbitSharpPending && !canvas_.orbitDragging && io.MouseWheel == 0) {
        canvas_.orbitSharpPending = false;
        canvas_.orbitDirty = true;  // the wheel stopped: draw it sharp next frame
    }
    if (canvas_.texture.tex) dl->AddImage(canvas_.texture.id(), v0, {v0.x + size.x, v0.y + size.y});
    char angle[64];
    std::snprintf(angle, sizeof angle, "yaw %.0f  pitch %.0f  zoom %.2g", std::fmod(o.yaw + 3600.0, 360.0), o.pitch, o.zoom);
    dl->AddText({v0.x + 8, v0.y + size.y - ImGui::GetFontSize() - 6}, IM_COL32(110, 110, 120, 255), angle);
}

}  // namespace wetwaresuite::gui
