// The Updates part of the Aa menu (and the launcher's Updates button): on/off,
// check now and what the last check found, all through the app.update_*
// commands so every app shares one setting (and scripts and Claude can do the same).
#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

bool fieldOn(const Result& r, const std::string& name) {
    const Value* v = r.field(name);
    const bool* b = v ? std::get_if<bool>(v) : nullptr;
    return b && *b;
}

}  // namespace

void Shell::readUpdates(bool now) {
    if (!session_.registry().find("app.update_status")) return;
    const double t = ImGui::GetTime();
    if (!now && t - updatesRead_ < 1.0) return;  // another app (or the background check) may change it
    updatesRead_ = t;
    const Result r = session_.run("app.update_status", {});
    updates_.version = fieldText(r, "current");
    updates_.status = fieldText(r, "status");
    updates_.note = fieldText(r, "note");
    updates_.why = fieldText(r, "why");
    updates_.on = fieldOn(r, "auto");
    updates_.canAuto = fieldOn(r, "can_auto");
}

void Shell::runUpdate(const std::string& command, const Params& params) {
    const Result r = session_.run(command, params);
    readUpdates(true);
    if (!r.ok) updates_.status = "failed", updates_.note = friendlyError(r.message);
}

void Shell::updatesSettings() {
    if (!session_.registry().find("app.update_status")) return;
    readUpdates(false);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Updates");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22);
    ImGui::TextDisabled("This is wetwaresuite %s.", updates_.version.c_str());
    bool on = updates_.on;
    if (ImGui::Checkbox("Update by itself", &on)) runUpdate("app.update_settings", {{"auto", on}});
    ImGui::SetItemTooltip("Once a day each app quietly checks for a new version and installs it. Offline it just tries "
                          "again later. Applies to every wetwaresuite app.");
    if (!updates_.canAuto && !updates_.why.empty()) ImGui::TextDisabled("Not here: %s.", updates_.why.c_str());

    const bool busy = updates_.status == "checking" || updates_.status == "downloading" || updates_.status == "installing";
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(busy ? "Checking..." : "Check now"))
        runUpdate(on && updates_.canAuto ? "app.update_install" : "app.update_check", {{"background", true}});
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(on && updates_.canAuto ? "Check now, and install a new version if there is one."
                                                  : "Check now. Nothing is downloaded until you click Install.");
    if (updates_.status == "available" || updates_.status == "ready") {
        ImGui::SameLine();
        if (accentButton("Install update", {0, 0})) runUpdate("app.update_install", {{"background", true}});
        ImGui::SetItemTooltip("Download it (if it isn't yet), check it and put it in place. Open apps keep running.");
    }
    if (!updates_.note.empty()) {
        if (updates_.status != "failed") ImGui::TextDisabled("%s", updates_.note.c_str());
        else ImGui::TextColored(darkLook() ? ImVec4{1.0f, 0.55f, 0.5f, 1} : ImVec4{0.7f, 0.0f, 0.0f, 1}, "%s", updates_.note.c_str());
    }
    ImGui::PopTextWrapPos();
}

}  // namespace wetwaresuite::gui
