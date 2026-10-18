// Retouching by painting: the Heal and Remove canvas tools. A stroke is turned
// into photo.heal / photo.remove_object params by the window-free logic
// (healStrokeParams, removeStrokeParams) and run through Session::run like every
// other action, so each one is a single undo step.
//
// The commands run on the UI thread (the panels read the open documents every
// frame, so a worker thread would race them): the command is queued, one frame
// with "Working..." is drawn, and the next frame runs it.
#include <algorithm>
#include <cmath>

#include "shell.hpp"
#include "wetwaresuite/raster/document.hpp"

namespace wetwaresuite::gui {

namespace {

constexpr ImU32 kHealColour = IM_COL32(90, 170, 255, 110);
constexpr ImU32 kRemoveColour = IM_COL32(255, 40, 40, 110);

void drawStroke(ImDrawList* dl, ImVec2 origin, float zoom, const std::vector<double>& points, float size, ImU32 col) {
    std::vector<ImVec2> pts;
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        pts.push_back({origin.x + static_cast<float>(points[i]) * zoom, origin.y + static_cast<float>(points[i + 1]) * zoom});
    }
    if (pts.empty()) return;
    const float thick = std::max(1.0f, size * zoom);
    if (pts.size() == 1) {
        dl->AddCircleFilled(pts[0], thick / 2, col);
        return;
    }
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), col, ImDrawFlags_None, thick);
    dl->AddCircleFilled(pts.front(), thick / 2, col);
    dl->AddCircleFilled(pts.back(), thick / 2, col);
}

}  // namespace

bool Shell::pictureSize(int& width, int& height) const {
    auto img = std::dynamic_pointer_cast<raster::ImageDoc>(session_.workspace().get(currentDoc_));
    if (!img || img->image.width < 1 || img->image.height < 1) return false;
    width = img->image.width;
    height = img->image.height;
    return true;
}

void Shell::retouchSettings() {
    const float unit = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(unit * 7);
    ImGui::SliderFloat("Size", &canvas_.size, 1, 300, "%.0f px", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("How wide the brush is. Make it a little bigger than the thing you're fixing");
    ImGui::SameLine();
    if (canvas_.tool == CanvasTool::Heal) {
        const bool picking = canvas_.pickingSource;
        if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(picking ? "Now click a clean spot" : "Pick source")) canvas_.pickingSource = !picking;
        if (picking) ImGui::PopStyleColor();
        ImGui::SetItemTooltip(
            "Press this, then click a clean spot nearby with the right texture (skin, sky, grass). Healing then copies "
            "from there. Same as Alt+click (Option+click on a Mac)");
        ImGui::SameLine();
        if (canvas_.healSource.picked) {
            ImGui::TextDisabled("Copies texture from the spot you picked");
            ImGui::SameLine();
            if (ImGui::Button("Forget source")) canvas_.healSource.forget();
            ImGui::SetItemTooltip("Go back to filling smoothly from the edges");
        } else {
            ImGui::TextDisabled("Fills smoothly from the edges");
        }
    } else {
        const bool hasMark = !canvas_.removeMark.empty() && canvas_.removeMarkDoc == currentDoc_;
        ImGui::BeginDisabled(!hasMark || canvas_.pending.has_value());
        if (accentButton("Remove")) applyRemove();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(hasMark ? "Fill the red area with picture from around it" : "Paint over the thing to remove first");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasMark);
        if (ImGui::Button("Clear")) canvas_.removeMark.clear();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Throw away the red area and paint again");
        ImGui::SameLine();
        ImGui::Checkbox("Remove when I let go", &canvas_.autoRemove);
        ImGui::SetItemTooltip("Remove straight away when you finish painting, without pressing Remove");
    }
    if (canvas_.pending) {
        ImGui::SameLine();
        ImGui::TextUnformatted("Working...");
    }
}

bool Shell::retouchPress(ImVec2 screen) {
    if (canvas_.tool != CanvasTool::Heal) return false;
    if (!ImGui::GetIO().KeyAlt && !canvas_.pickingSource) return false;
    canvas_.pickingSource = false;
    resultFor_ = "Healing brush";
    const ImVec2 img = toImage(screen);
    int w = 0, h = 0;
    if (!pictureSize(w, h) || img.x < 0 || img.y < 0 || img.x >= static_cast<float>(w) || img.y >= static_cast<float>(h)) {
        result_ = Result::failure("Pick a spot on the picture.");
        return true;
    }
    canvas_.healSource.pick(img.x, img.y);
    result_ = Result::success("Source picked. Now paint over the spot you want to heal.");
    return true;
}

void Shell::retouchRelease() {
    int w = 0, h = 0;
    if (!pictureSize(w, h)) return;
    if (canvas_.tool == CanvasTool::Remove) {
        canvas_.removeMark = canvas_.points;
        canvas_.removeMarkSize = canvas_.size;
        canvas_.removeMarkDoc = currentDoc_;
        if (canvas_.autoRemove) applyRemove();
        return;
    }
    double offset[2] = {0, 0};
    const bool fromSource = canvas_.healSource.offsetFor(canvas_.points[0], canvas_.points[1], offset[0], offset[1]);
    Params p;
    std::string error;
    if (!healStrokeParams(currentDoc_, canvas_.points, canvas_.size, w, h, fromSource ? offset : nullptr, p, error)) {
        resultFor_ = "Healing brush";
        result_ = Result::failure(error);
        return;
    }
    queueRetouch("photo.heal", "Healing brush", std::move(p));
}

void Shell::applyRemove() {
    if (canvas_.removeMark.empty() || canvas_.removeMarkDoc != currentDoc_ || canvas_.pending) return;
    int w = 0, h = 0;
    if (!pictureSize(w, h)) return;
    Params p;
    std::string error;
    if (!removeStrokeParams(currentDoc_, canvas_.removeMark, canvas_.removeMarkSize, w, h, p, error)) {
        resultFor_ = "Remove object";
        result_ = Result::failure(error);
        return;
    }
    queueRetouch("photo.remove_object", "Remove object", std::move(p));
}

void Shell::queueRetouch(const std::string& command, const std::string& title, Params params) {
    if (!find(command)) return;
    canvas_.pending = PendingRetouch{command, title, std::move(params), false};
    SDL_Event wake{};
    wake.type = Shell::wakeEvent;
    SDL_PushEvent(&wake);  // make sure the next frame comes even with no input
}

void Shell::runPendingRetouch() {
    PendingRetouch job = std::move(*canvas_.pending);
    canvas_.pending.reset();
    Result r = run(job.command, job.params);  // one undo step; marks the canvas dirty for the changed area
    if (const CommandInfo* info = find(job.command)) resultFor_ = info->title;
    else resultFor_ = job.title;
    if (r.ok && job.command == "photo.remove_object") canvas_.removeMark.clear();
    if (r.ok && job.command == "photo.heal") canvas_.points.clear();
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::drawRetouch(ImDrawList* dl, ImVec2 origin, bool hovered) {
    const float zoom = canvas_.zoom;
    const bool heal = canvas_.tool == CanvasTool::Heal;
    const bool pendingHeal = canvas_.pending && canvas_.pending->command == "photo.heal";
    if (heal && (canvas_.stroking || pendingHeal)) drawStroke(dl, origin, zoom, canvas_.points, canvas_.size, kHealColour);
    if (canvas_.tool == CanvasTool::Remove) {
        if (canvas_.stroking) drawStroke(dl, origin, zoom, canvas_.points, canvas_.size, kRemoveColour);
        else if (!canvas_.removeMark.empty() && canvas_.removeMarkDoc == currentDoc_) {
            drawStroke(dl, origin, zoom, canvas_.removeMark, canvas_.removeMarkSize, kRemoveColour);
        }
    }
    // where the heal brush copies from: the picked spot, then (once a stroke fixed the offset) following the brush
    const HealSource& src = canvas_.healSource;
    if (heal && src.picked) {
        bool show = true;
        ImVec2 at{static_cast<float>(src.x), static_cast<float>(src.y)};
        if (src.anchored) {
            if (canvas_.stroking && canvas_.points.size() >= 2) {
                at = {static_cast<float>(canvas_.points[canvas_.points.size() - 2] + src.dx),
                      static_cast<float>(canvas_.points.back() + src.dy)};
            } else if (hovered) {
                const ImVec2 m = toImage(ImGui::GetIO().MousePos);
                at = {m.x + static_cast<float>(src.dx), m.y + static_cast<float>(src.dy)};
            } else {
                show = false;
            }
        }
        if (show) {
            const ImVec2 c{origin.x + at.x * zoom, origin.y + at.y * zoom};
            const float r = std::max(4.0f, canvas_.size * zoom / 2);
            dl->AddCircle(c, r + 1, IM_COL32(0, 0, 0, 200), 0, 2.5f);
            dl->AddCircle(c, r, IM_COL32(255, 255, 255, 230), 0, 1.5f);
            dl->AddLine({c.x - 5, c.y}, {c.x + 5, c.y}, IM_COL32(255, 255, 255, 230), 1.5f);
            dl->AddLine({c.x, c.y - 5}, {c.x, c.y + 5}, IM_COL32(255, 255, 255, 230), 1.5f);
        }
    }
    if (canvas_.pending) {
        const char* text = "Working...";
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const ImVec2 mid{canvas_.viewMin.x + canvas_.viewSize.x / 2, canvas_.viewMin.y + canvas_.viewSize.y / 2};
        const float pad = ImGui::GetFontSize();
        dl->AddRectFilled({mid.x - ts.x / 2 - pad, mid.y - ts.y / 2 - pad / 2}, {mid.x + ts.x / 2 + pad, mid.y + ts.y / 2 + pad / 2},
                          IM_COL32(0, 0, 0, 170), pad / 2);
        dl->AddText({mid.x - ts.x / 2, mid.y - ts.y / 2}, IM_COL32(255, 255, 255, 255), text);
        canvas_.pending->shown = true;
    }
}

}  // namespace wetwaresuite::gui
