// The terminal panel: tabs of shells, a scrollback, a prompt with history
// (Up/Down) and name completion (Tab), Stop for a running command and typing
// answers into it. Each line runs through terminal.run, terminal.send and
// terminal.read, so a script or Claude can drive the same terminals.
#include <algorithm>
#include <functional>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

struct PromptHooks {
    std::function<std::string(const std::string&)> complete;
    TerminalTab* tab = nullptr;
};

int promptCallback(ImGuiInputTextCallbackData* d) {
    auto* h = static_cast<PromptHooks*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        const std::string line(d->Buf, static_cast<std::size_t>(d->BufTextLen));
        const std::string done = h->complete(line);
        if (done != line) {
            d->DeleteChars(0, d->BufTextLen);
            d->InsertChars(0, done.c_str());
        }
    } else if (d->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
        TerminalTab& t = *h->tab;
        if (t.history.empty()) return 0;
        const int n = static_cast<int>(t.history.size());
        if (d->EventKey == ImGuiKey_UpArrow) t.historyPos = t.historyPos < 0 ? n - 1 : std::max(0, t.historyPos - 1);
        else if (d->EventKey == ImGuiKey_DownArrow) t.historyPos = t.historyPos < 0 ? -1 : (t.historyPos + 1 >= n ? -1 : t.historyPos + 1);
        const std::string& pick = t.historyPos < 0 ? std::string() : t.history[static_cast<std::size_t>(t.historyPos)];
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, pick.c_str());
    }
    return 0;
}

}  // namespace

void Shell::terminalNew(const std::string& folder) {
    TerminalPanel& tp_ = panels_->terminal;
    Params p;
    if (!folder.empty()) p["folder"] = folder;
    if (!tp_.shell.empty()) p["shell"] = tp_.shell;
    Result r = run("terminal.open", p);
    if (!r.ok) {
        tp_.error = friendlyError(r.message);
        return;
    }
    TerminalTab t;
    t.doc = tp::text(r, "doc");
    t.cwd = tp::text(r, "cwd");
    tp_.tabs.push_back(t);
    tp_.current = t.doc;
    tp_.focusInput = true;
    tp_.error.clear();
}

void Shell::terminalPoll(bool force) {
    TerminalPanel& tp_ = panels_->terminal;
    const double now = tp::seconds();
    if (!force && now - tp_.lastPoll < 0.1) return;
    tp_.lastPoll = now;
    for (auto it = tp_.tabs.begin(); it != tp_.tabs.end();) {
        TerminalTab& t = *it;
        if (!t.running && !force) {
            ++it;
            continue;
        }
        if (!session_.workspace().get(t.doc)) {  // closed elsewhere
            it = tp_.tabs.erase(it);
            continue;
        }
        Result r = query("terminal.read", {{"doc", t.doc}});
        if (r.ok) {
            const std::string out = tp::text(r, "output");
            if (!out.empty()) {
                t.scroll += out;
                t.scrollDown = true;
            }
            const bool running = tp::number(r, "running") != 0;
            if (t.running && !running) {
                const int code = static_cast<int>(tp::number(r, "exit_code"));
                if (code != 0) t.scroll += "(exit code " + std::to_string(code) + ")\n";
                t.scrollDown = true;
            }
            t.running = running;
            t.cwd = tp::text(r, "cwd");
        }
        ++it;
    }
    // keep the scrollback from growing without end
    for (auto& t : tp_.tabs) {
        if (t.scroll.size() > 4'000'000) t.scroll.erase(0, t.scroll.size() - 3'000'000);
    }
}

void Shell::terminalPanel() {
    TerminalPanel& tp_ = panels_->terminal;
    // terminals opened by a script or a form show up too
    for (const auto& id : docIds()) {
        if (panelForDoc(id) != "terminal") continue;
        bool known = false;
        for (const auto& t : tp_.tabs) known = known || t.doc == id;
        if (!known) {
            TerminalTab t;
            t.doc = id;
            Result r = query("terminal.read", {{"doc", id}, {"all", true}});
            t.scroll = tp::text(r, "output");
            t.cwd = tp::text(r, "cwd");
            t.running = tp::number(r, "running") != 0;
            tp_.tabs.push_back(t);
        }
    }
    if (panelForDoc(currentDoc_) == "terminal") tp_.current = currentDoc_;

    const float unit = ImGui::GetFontSize();
    if (ImGui::Button("New terminal")) terminalNew("");
    if (pro_) {
        ImGui::SameLine();
        static tp::Rows shells;
        if (shells.empty()) shells = tp::rows(query("terminal.shells", {}));
        ImGui::SetNextItemWidth(unit * 8);
        if (ImGui::BeginCombo("Shell", tp_.shell.empty() ? "default" : tp_.shell.c_str())) {
            if (ImGui::Selectable("default", tp_.shell.empty())) tp_.shell.clear();
            for (const auto& s : shells) {
                if (ImGui::Selectable(s[0].c_str(), s[0] == tp_.shell)) tp_.shell = s[0];
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Which shell new terminals use");
    }
    tp::errorText(tp_.error);
    if (tp_.tabs.empty()) {
        tp::muted("No terminal open.");
        return;
    }
    TerminalTab* cur = &tp_.tabs.back();
    std::string close;
    if (ImGui::BeginTabBar("terms", ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (auto& t : tp_.tabs) {
            bool open = true;
            const std::string label = (t.cwd.empty() ? t.doc : tp::baseName(t.cwd)) + (t.running ? " (busy)" : "") + "###" + t.doc;
            if (ImGui::BeginTabItem(label.c_str(), &open)) {
                cur = &t;
                if (tp_.current != t.doc) {
                    tp_.current = t.doc;
                    tp_.focusInput = true;
                }
                ImGui::EndTabItem();
            }
            if (!open) close = t.doc;
        }
        ImGui::EndTabBar();
    }
    TerminalTab& t = *cur;

    const float promptH = ImGui::GetFrameHeightWithSpacing() * 2;
    ImGui::BeginChild("scroll", {0, -promptH}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(monoFont, 0.0f);
    if (t.scroll.empty()) tp::muted("Type a command below and press Enter. Up and Down bring back earlier ones; Tab finishes names.");
    else ImGui::TextUnformatted(t.scroll.c_str(), t.scroll.c_str() + t.scroll.size());
    ImGui::PopFont();
    if (t.scrollDown) {
        ImGui::SetScrollHereY(1.0f);
        t.scrollDown = false;
    }
    ImGui::EndChild();

    ImGui::PushFont(monoFont, 0.0f);
    const std::string prompt = t.running ? "input >" : (t.cwd.empty() ? std::string("$") : tp::baseName(t.cwd) + " $");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(prompt.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (tp_.focusInput) {
        ImGui::SetKeyboardFocusHere();
        tp_.focusInput = false;
    }
    PromptHooks hooks;
    hooks.tab = &t;
    hooks.complete = [this, &t](const std::string& line) {
        Result r = query("terminal.complete", {{"doc", t.doc}, {"line", line}});
        const std::string common = tp::text(r, "common");
        const auto rows = tp::rows(r);
        if (rows.size() > 1) {
            std::string list;
            for (const auto& row : rows) list += row[0] + "  ";
            t.scroll += list + "\n";
            t.scrollDown = true;
        }
        if (common.empty()) return line;
        const auto space = line.find_last_of(" \t");
        return (space == std::string::npos ? std::string() : line.substr(0, space + 1)) + common;
    };
    const bool enter = ImGui::InputText("##prompt", &t.input,
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion,
                                        promptCallback, &hooks);
    const bool typing = ImGui::IsItemActive();
    ImGui::PopFont();
    if (typing && t.running && t.input.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) run("terminal.interrupt", {{"doc", t.doc}});
    if (enter) {
        const std::string line = t.input;
        t.input.clear();
        t.historyPos = -1;
        if (t.running) {
            Result r = run("terminal.send", {{"doc", t.doc}, {"text", line}, {"wait", 0.0}});
            if (!r.ok) t.scroll += friendlyError(r.message) + "\n";
        } else if (!line.empty()) {
            if (t.history.empty() || t.history.back() != line) t.history.push_back(line);
            Result r = run("terminal.run", {{"doc", t.doc}, {"command", line}, {"wait", false}});
            if (!r.ok) t.scroll += friendlyError(r.message) + "\n";
            t.running = r.ok && tp::number(r, "running") != 0;
            terminalPoll(true);
        }
        t.scrollDown = true;
        tp_.focusInput = true;
    }
    ImGui::BeginDisabled(!t.running);
    if (ImGui::Button("Stop")) run("terminal.interrupt", {{"doc", t.doc}});
    ImGui::SetItemTooltip("Stop the running command (Ctrl+C with an empty prompt)");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        run("terminal.clear", {{"doc", t.doc}});
        t.scroll.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", t.cwd.c_str());
    if (!close.empty()) {
        run("terminal.close", {{"doc", close}});
        tp_.tabs.erase(std::remove_if(tp_.tabs.begin(), tp_.tabs.end(), [&](const TerminalTab& x) { return x.doc == close; }), tp_.tabs.end());
    }
}

}  // namespace wetwaresuite::gui
