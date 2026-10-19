// The retouch brushes on the canvas toolbar: Clone, Dodge, Burn, Sponge, Smudge
// and Blur (or sharpen). A stroke becomes paint.clone / paint.dodge /
// paint.smudge / paint.blur_brush params through the window-free
// brushToolParams() and runs through Session::run, one undo step per stroke.
#include <algorithm>

#include "shell.hpp"

namespace wetwaresuite::gui {

bool Shell::brushTool(CanvasTool tool) {
    switch (tool) {
        case CanvasTool::Clone:
        case CanvasTool::Dodge:
        case CanvasTool::Burn:
        case CanvasTool::Sponge:
        case CanvasTool::Smudge:
        case CanvasTool::Blur: return true;
        default: return false;
    }
}

namespace {

const char* toolName(CanvasTool t, bool sharpen) {
    switch (t) {
        case CanvasTool::Clone: return "clone";
        case CanvasTool::Dodge: return "dodge";
        case CanvasTool::Burn: return "burn";
        case CanvasTool::Sponge: return "sponge";
        case CanvasTool::Smudge: return "smudge";
        default: return sharpen ? "sharpen" : "blur";
    }
}

}  // namespace

void Shell::brushToolSettings() {
    const float unit = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(unit * 7);
    ImGui::SliderFloat("Size", &canvas_.size, 1, 300, "%.0f px", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    const CanvasTool t = canvas_.tool;
    const bool tone = t == CanvasTool::Dodge || t == CanvasTool::Burn || t == CanvasTool::Sponge;
    ImGui::SetNextItemWidth(unit * 6);
    ImGui::SliderFloat(tone ? "Exposure" : t == CanvasTool::Clone ? "Opacity" : "Strength", t == CanvasTool::Clone ? &canvas_.opacity : &canvas_.strength, 1,
                       100, "%.0f%%");
    if (t == CanvasTool::Dodge || t == CanvasTool::Burn) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(unit * 6.5f);
        if (ImGui::BeginCombo("Range", canvas_.toneRange.c_str())) {
            for (const char* r : {"shadows", "midtones", "highlights"}) {
                if (ImGui::Selectable(r, canvas_.toneRange == r)) canvas_.toneRange = r;
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Which tones change most");
    }
    if (t == CanvasTool::Blur) {
        ImGui::SameLine();
        ImGui::Checkbox("Sharpen", &canvas_.sharpen);
        ImGui::SetItemTooltip("Crisp up instead of softening");
    }
    if (t == CanvasTool::Clone) {
        ImGui::SameLine();
        const bool picking = canvas_.pickingSource;
        if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(picking ? "Now click the spot to copy" : "Pick source")) canvas_.pickingSource = !picking;
        if (picking) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Same as Alt+click (Option+click on a Mac) on the spot to copy from");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", canvas_.cloneSource.picked ? "Copies from the spot you picked" : "Alt+click a spot to copy from");
    }
}

bool Shell::brushToolPress(ImVec2 screen) {
    if (canvas_.tool != CanvasTool::Clone) return false;
    if (!ImGui::GetIO().KeyAlt && !canvas_.pickingSource) return false;
    canvas_.pickingSource = false;
    resultFor_ = "Clone stamp";
    const ImVec2 img = toImage(screen);
    int w = 0, h = 0;
    if (!pictureSize(w, h) || img.x < 0 || img.y < 0 || img.x >= static_cast<float>(w) || img.y >= static_cast<float>(h)) {
        result_ = Result::failure("Pick a spot on the picture.");
        return true;
    }
    canvas_.cloneSource.pick(img.x, img.y);
    result_ = Result::success("Source picked. Now paint where the copy should go.");
    return true;
}

void Shell::runBrushTool() {
    BrushToolSettings set;
    set.size = canvas_.size;
    set.opacity = canvas_.tool == CanvasTool::Clone ? canvas_.opacity : 100;
    set.strength = canvas_.strength;
    set.range = canvas_.toneRange;
    double offset[2];
    const bool haveOffset = canvas_.tool == CanvasTool::Clone && canvas_.points.size() >= 2 &&
                            canvas_.cloneSource.offsetFor(canvas_.points[0], canvas_.points[1], offset[0], offset[1]);
    std::vector<double> pressures;
    if (canvas_.usedPen) pressures.assign(canvas_.pressures.begin(), canvas_.pressures.end());
    std::string command, error;
    Params p;
    const std::string tool = toolName(canvas_.tool, canvas_.sharpen);
    if (!brushToolParams(tool, currentDoc_, canvas_.points, pressures, set, haveOffset ? offset : nullptr, command, p, error)) {
        resultFor_ = "Brush";
        result_ = Result::failure(error);
        return;
    }
    const CommandInfo* info = find(command);
    if (!info) return;
    Result r = run(command, p);
    resultFor_ = info->title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::drawBrushTool(ImDrawList* dl, ImVec2 origin, bool hovered) {
    if (canvas_.tool != CanvasTool::Clone || !canvas_.cloneSource.picked) return;
    const HealSource& src = canvas_.cloneSource;
    ImVec2 at{static_cast<float>(src.x), static_cast<float>(src.y)};
    if (src.anchored) {
        if (canvas_.stroking && canvas_.points.size() >= 2) {
            at = {static_cast<float>(canvas_.points[canvas_.points.size() - 2] + src.dx), static_cast<float>(canvas_.points.back() + src.dy)};
        } else if (hovered) {
            const ImVec2 m = toImage(ImGui::GetIO().MousePos);
            at = {m.x + static_cast<float>(src.dx), m.y + static_cast<float>(src.dy)};
        } else {
            return;
        }
    }
    const ImVec2 c{origin.x + at.x * canvas_.zoom, origin.y + at.y * canvas_.zoom};
    const float r = std::max(4.0f, canvas_.size * canvas_.zoom / 2);
    dl->AddCircle(c, r + 1, IM_COL32(0, 0, 0, 200), 0, 2.5f);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 230), 0, 1.5f);
    dl->AddLine({c.x - 5, c.y}, {c.x + 5, c.y}, IM_COL32(255, 255, 255, 230), 1.5f);
    dl->AddLine({c.x, c.y - 5}, {c.x, c.y + 5}, IM_COL32(255, 255, 255, 230), 1.5f);
}

}  // namespace wetwaresuite::gui
