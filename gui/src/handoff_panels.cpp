// Handing work to other apps from the window: the "Send to..." menu on an open
// file, the inbox banner on the home screen and the "Open in <app>" suggestion.
// Each button runs an app.* command through Session::run, exactly what
// `wetwaresuite-image app.send to=model doc=img1` would do from the command line.
#include "shell.hpp"

namespace wetwaresuite::gui {

void Shell::refreshInbox() {
    if (!handoff_) return;
    inbox_ = inboxBanner(session_.run("app.inbox", {}));
}

void Shell::suggestOtherApp(const std::string& path, bool openedHere) {
    suggestion_ = {};
    if (!handoff_ || !session_.registry().find("app.open_with")) return;
    suggestion_ = openSuggestion(session_.run("app.open_with", {{"path", path}}), self_, path, openedHere);
}

void Shell::sendTo(const std::string& app) {
    const std::string doc = currentDoc_;
    resultFor_ = "Send to " + appTitle(app);
    Result r = run("app.send", {{"to", app}, {"doc", doc}});
    launchOffer_ = launchAfterSend(r, self_, sendChoices_);
    if (r.ok) {
        // the message names the file and the command for people who use the command line; the window says it simply
        const std::string file = fieldText(r, "file");
        std::string text = fieldText(r, "opened") == "true" ? "Sent to " + appTitle(app) + " and opened here."
                                                            : "Sent to " + appTitle(app) + ". It is waiting in " + appTitle(app) +
                                                                  "'s inbox: open " + appTitle(app) + " and press \"Open it\".";
        r.message = text;
        r.data = {{"File", file}};
    } else {
        r.message = friendlyError(r.message);
    }
    result_ = std::move(r);
    scrollToResult_ = true;
}

void Shell::launchApp(const std::string& app, const std::string& path) {
    Params p{{"app", app}};
    if (!path.empty()) p["path"] = path;
    Result r = session_.run("app.launch", p);
    resultFor_ = "Open in " + appTitle(app);
    r.message = r.ok ? appTitle(app) + " is starting in its own window." : friendlyError(r.message);
    r.data.clear();
    result_ = std::move(r);
    scrollToResult_ = true;
}

void Shell::sendButton() {
    if (!handoff_ || !session_.registry().find("app.targets")) return;
    ImGui::SameLine();
    if (ImGui::Button("Send to...")) {
        sendChoices_ = sendChoices(session_.run("app.targets", {{"doc", currentDoc_}}), self_);
        ImGui::OpenPopup("sendto");
    }
    ImGui::SetItemTooltip("Hand this file to another app, in the format that app wants");
    sendPopup();
}

void Shell::sendPopup() {
    if (!ImGui::BeginPopup("sendto")) return;
    if (sendChoices_.empty()) {
        ImGui::TextUnformatted("No other app takes this kind of file.");
        ImGui::TextDisabled("Save it, then open the saved file in the other app.");
    } else {
        ImGui::TextDisabled("Send this file to:");
    }
    for (const auto& c : sendChoices_) {
        ImGui::PushID(c.app.c_str());
        if (c.installed) {
            if (ImGui::Selectable(c.label.c_str())) sendTo(c.app);
        } else {
            ImGui::BeginDisabled();
            ImGui::Selectable((c.label + "  (not installed)").c_str());
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Add the %s app with the wetwaresuite hub to send files to it.", c.label.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndPopup();
}

namespace {

// A coloured strip across the view with a line of text and one or two buttons.
bool bannerBegin(const char* id, bool dark) {
    const ImVec4 bg = dark ? ImVec4{0.16f, 0.22f, 0.32f, 1} : ImVec4{0.86f, 0.92f, 1.0f, 1};
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    bool open = ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    return open;
}

}  // namespace

void Shell::inboxBannerView() {
    if (!inbox_.show) return;
    bannerBegin("inbox", dark_);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(inbox_.text.c_str());
    ImGui::PopTextWrapPos();
    if (accentButton(inbox_.button.c_str())) {
        Result r = run("app.receive", {});
        resultFor_ = "Inbox";
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
        scrollToResult_ = true;
        refreshInbox();
    }
    ImGui::SameLine();
    if (ImGui::Button("Not now")) inbox_.show = false;
    ImGui::EndChild();
    ImGui::Spacing();
}

void Shell::suggestionBannerView() {
    if (!suggestion_.show) return;
    bannerBegin("suggest", dark_);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(suggestion_.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!suggestion_.installed);
    if (accentButton(suggestion_.button.c_str())) {
        launchApp(suggestion_.app, suggestion_.path);
        suggestion_.show = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Dismiss")) suggestion_.show = false;
    ImGui::EndChild();
    ImGui::Spacing();
}

void Shell::launchOfferView() {
    if (launchOffer_.empty() || resultFor_.rfind("Send to", 0) != 0) return;
    if (accentButton(("Open " + appTitle(launchOffer_) + " now").c_str())) {
        launchApp(launchOffer_, {});
        launchOffer_.clear();
    }
}

}  // namespace wetwaresuite::gui
