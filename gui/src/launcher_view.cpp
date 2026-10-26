// The launcher: what the main wetwaresuite shortcut opens. It carries no tools,
// only a grid of the apps; each one starts as its own program in its own window
// (app.launch), and a file opened or dropped here goes to the app made for it
// (app.open_with, then app.launch). Closing the launcher closes nothing else.
#include <algorithm>
#include <cctype>
#include <cfloat>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

bool listed(const std::string& list, const std::string& id) {
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        if (list.compare(start, end - start, id) == 0 && end - start == id.size()) return true;
        start = end + 1;
    }
    return false;
}

ImU32 accentColour(std::uint32_t rgb, float alpha = 1.0f) {
    return ImGui::GetColorU32(ImVec4{((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha});
}

}  // namespace

void Shell::launcherStart(const std::string& app, const std::string& path) {
    Params p{{"app", app}};
    if (!path.empty()) p["path"] = path;
    Result r = session_.run("app.launch", p);
    launcherNoteOk_ = r.ok;
    launcherNote_ = r.ok ? appTitle(app) + " is opening in its own window." : friendlyError(r.message);
}

void Shell::launcherOpen(const std::string& path) {
    Result where = session_.run("app.open_with", {{"path", path}});
    const std::string app = where.ok ? fieldText(where, "app") : std::string();
    if (app.empty()) {
        launcherNoteOk_ = false;
        launcherNote_ = friendlyError(where.message);
        return;
    }
    launcherStart(app, path);
}

void Shell::launcherFrame() {
    if (installedStale_ && handoff_) {
        installed_ = fieldText(session_.run("app.installed", {}), "installed");
        installedStale_ = false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float unit = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Text, title_);
    ImGui::PushFont(nullptr, unit * 1.6f);
    ImGui::TextUnformatted(app_.name.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Pick an app. Each one opens in its own window, so you can use several at once.");
    ImGui::Spacing();
    if (accentButton("Open a file...", {unit * 10, unit * 2.2f})) showOpenDialog();
    ImGui::SetItemTooltip("Opens it in the app made for it (Ctrl+O). You can also drop files on this window.");
    ImGui::SameLine();
    if (ImGui::Button("Updates", {0, unit * 2.2f})) ImGui::OpenPopup("updates");
    ImGui::SetItemTooltip("Whether wetwaresuite updates by itself, and checking now");
    if (ImGui::BeginPopup("updates")) {
        updatesSettings();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (!launcherNote_.empty()) {
        if (launcherNoteOk_) ImGui::TextUnformatted(launcherNote_.c_str());
        else ImGui::TextColored({1.0f, 0.45f, 0.4f, 1}, "%s", launcherNote_.c_str());
    }
    ImGui::Spacing();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float tileW = unit * 14.5f, tileH = unit * 7.3f;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / (tileW + style.ItemSpacing.x)));
    for (std::size_t i = 0; i < app_.launcher.size(); ++i) {
        const LauncherApp& a = app_.launcher[i];
        const bool have = !handoff_ || listed(installed_, a.id);
        if (i % static_cast<std::size_t>(columns)) ImGui::SameLine();
        ImGui::PushID(a.id.c_str());
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("tile", {tileW, tileH}) && have;
        const bool hovered = ImGui::IsItemHovered();
        if (have) ImGui::SetItemTooltip("Open %s in its own window", a.name.c_str());
        else ImGui::SetItemTooltip("Not installed yet: add it with the wetwaresuite hub (wetwaresuite-hub install %s).", a.id.c_str());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 end{at.x + tileW, at.y + tileH};
        dl->AddRectFilled(at, end, ImGui::GetColorU32(hovered && have ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), unit * 0.4f);
        // the app's badge: its colour and initial, the same colour as its window icon
        const float badge = unit * 2.6f;
        const ImVec2 b0{at.x + unit * 0.7f, at.y + unit * 0.7f};
        dl->AddRectFilled(b0, {b0.x + badge, b0.y + badge}, accentColour(a.accent, have ? 1.0f : 0.35f), unit * 0.5f);
        const std::string initial(1, static_cast<char>(std::toupper(static_cast<unsigned char>(a.name.empty() ? '?' : a.name[0]))));
        ImFont* font = ImGui::GetFont();
        const float big = unit * 1.5f;
        const ImVec2 is = font->CalcTextSizeA(big, FLT_MAX, 0.0f, initial.c_str());
        dl->AddText(font, big, {b0.x + (badge - is.x) / 2, b0.y + (badge - is.y) / 2}, IM_COL32(255, 255, 255, 255), initial.c_str());
        const float textX = b0.x + badge + unit * 0.7f;
        dl->AddText(font, unit * 1.15f, {textX, b0.y}, ImGui::GetColorU32(have ? ImGuiCol_Text : ImGuiCol_TextDisabled), a.name.c_str());
        const std::string state = have ? "" : "not installed";
        if (!state.empty()) dl->AddText({textX, b0.y + unit * 1.35f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), state.c_str());
        // the summary wraps under the badge
        dl->AddText(font, unit, {at.x + unit * 0.7f, b0.y + badge + unit * 0.35f}, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    a.summary.c_str(), nullptr, tileW - unit * 1.4f);
        if (clicked) launcherStart(a.id, {});
        ImGui::PopID();
    }
    ImGui::End();
}

}  // namespace wetwaresuite::gui
