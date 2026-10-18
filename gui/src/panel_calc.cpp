// The calculator panel: a keypad with history and memory, and tabs for
// graphs, unit conversion, and in Advanced mode statistics, equation solving
// and number bases. Every key that works something out runs a calc.* command.
#include <algorithm>
#include <cmath>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

struct UnitGroup {
    const char* name;
    std::vector<const char*> units;
};

const std::vector<UnitGroup>& unitGroups() {
    static const std::vector<UnitGroup> groups = {
        {"Length", {"km", "m", "cm", "mm", "mi", "yd", "ft", "inch", "nmi"}},
        {"Weight", {"kg", "g", "mg", "t", "lb", "oz", "st"}},
        {"Temperature", {"degC", "degF", "K"}},
        {"Volume", {"L", "mL", "m^3", "gal", "qt", "pt", "cup", "floz"}},
        {"Speed", {"km/h", "mph", "m/s", "knot"}},
        {"Area", {"m^2", "km^2", "ha", "acre", "ft^2"}},
        {"Time", {"s", "min", "h", "day", "week", "year"}},
        {"Data", {"B", "kB", "MB", "GB", "TB", "KiB", "MiB", "GiB"}},
        {"Energy", {"J", "kJ", "cal", "kcal", "kWh", "eV"}},
        {"Pressure", {"Pa", "kPa", "bar", "atm", "psi", "mmHg"}},
    };
    return groups;
}

bool isOperator(const std::string& key) { return key == "+" || key == "-" || key == "*" || key == "/" || key == "^" || key == "%"; }

}  // namespace

void Shell::calcEvaluate() {
    CalcPanel& c = panels_->calc;
    if (c.input.empty()) return;
    // "* 2" right after an answer carries on from it
    if (!c.asked.empty() && std::string("+*/^%").find(c.input[0]) != std::string::npos) c.input = "ans " + c.input;
    Result r = run("calc.eval", {{"expression", c.input}});
    c.error = !r.ok;
    c.historyStale = true;
    if (!r.ok) {
        c.shown = friendlyError(r.message);
        c.detail.clear();
        return;
    }
    c.shown = tp::text(r, "text");
    if (c.shown.empty()) c.shown = r.message.substr(0, r.message.find('\n'));
    c.detail.clear();
    for (const char* k : {"exact", "hex", "bin"}) {
        if (!pro_ && std::string(k) != "exact") continue;  // hex and binary forms are for Advanced mode
        const std::string v = tp::text(r, k);
        if (!v.empty() && v != c.shown) c.detail += (c.detail.empty() ? "" : "    ") + v;
    }
    c.justAnswered = true;
    c.asked = c.input + " =";
    c.input.clear();  // the next calculation starts fresh; an operator first carries on from ans
}

void Shell::calcKeypad() {
    CalcPanel& c = panels_->calc;
    const float unit = ImGui::GetFontSize();
    // the display
    ImGui::BeginChild("display", {0, unit * 7.6f}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushFont(monoFont, ImGui::GetStyle().FontSizeBase * 1.1f);
    ImGui::SetNextItemWidth(-1);
    if (c.focusInput) {
        ImGui::SetKeyboardFocusHere();
        c.focusInput = false;
    }
    if (ImGui::InputTextWithHint("##expr", "type a calculation, e.g. 12% of 80 or 5 km to mi", &c.input, ImGuiInputTextFlags_EnterReturnsTrue)) {
        calcEvaluate();
        c.focusInput = true;
    }
    if (!c.asked.empty() && !c.error) {
        const float aw = ImGui::CalcTextSize(c.asked.c_str()).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionAvail().x - aw));
        ImGui::TextDisabled("%s", c.asked.c_str());
    }
    ImGui::PopFont();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * (c.error ? 1.0f : 2.0f));
    const float w = ImGui::CalcTextSize(c.shown.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionAvail().x - w));
    if (c.error) tp::errorText(c.shown);
    else ImGui::TextUnformatted(c.shown.c_str());
    ImGui::PopFont();
    if (!c.detail.empty()) {
        const float dw = ImGui::CalcTextSize(c.detail.c_str()).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionAvail().x - dw));
        ImGui::TextDisabled("%s", c.detail.c_str());
    }
    ImGui::EndChild();

    if (pro_) {
        const char* angles[] = {"deg", "rad", "grad"};
        ImGui::SetNextItemWidth(unit * 6);
        if (ImGui::BeginCombo("Angles", c.angle.c_str())) {
            for (const char* a : angles) {
                if (ImGui::Selectable(a, c.angle == a)) {
                    c.angle = a;
                    run("calc.settings", {{"angle", std::string(a)}});
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Exact fractions", &c.exact)) run("calc.settings", {{"exact", c.exact}});
        if (!c.mem.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("M = %s", c.mem.c_str());
        }
    }

    static const std::vector<std::vector<const char*>> simpleKeys = {
        {"C", "Back", "(", ")", "/"}, {"7", "8", "9", "*", "sqrt("}, {"4", "5", "6", "-", "^"},
        {"1", "2", "3", "+", "%"},    {"0", ".", "ans", "pi", "="},
    };
    static const std::vector<std::vector<const char*>> advancedKeys = {
        {"sin(", "cos(", "tan(", "ln(", "log("}, {"asin(", "acos(", "atan(", "exp(", "!"},
        {"MC", "MR", "M+", "M-", "MS"},          {"e", "x", "abs(", "mod", ","},
    };
    std::vector<std::vector<const char*>> keys;
    if (pro_) keys = advancedKeys;
    keys.insert(keys.end(), simpleKeys.begin(), simpleKeys.end());
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float rowsH = ImGui::GetContentRegionAvail().y;
    const float bw = (avail - gap * 4) / 5;
    const float bh = std::clamp((rowsH - gap * static_cast<float>(keys.size())) / static_cast<float>(keys.size()), unit * 1.8f, unit * 3.2f);
    for (const auto& row : keys) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) ImGui::SameLine();
            const std::string key = row[i];
            std::string label = key == "/" ? "\xC3\xB7" : key == "*" ? "\xC3\x97" : key == "sqrt(" ? "sqrt" : key;
            if (label.size() > 1 && label.back() == '(') label.pop_back();
            bool pressed = key == "=" ? accentButton(label.c_str(), {bw, bh}) : ImGui::Button(label.c_str(), {bw, bh});
            if (!pressed) continue;
            c.error = false;
            if (key == "=") {
                calcEvaluate();
            } else if (key == "C") {
                c.input.clear();
                c.shown = "0";
                c.detail.clear();
                c.asked.clear();
            } else if (key == "Back") {
                if (!c.input.empty()) c.input.pop_back();
            } else if (key == "MC" || key == "MR" || key == "M+" || key == "M-" || key == "MS") {
                const std::string action = key == "MC" ? "clear" : key == "MR" ? "recall" : key == "M+" ? "add" : key == "M-" ? "subtract" : "store";
                Result r = run("calc.memory", {{"action", action}});
                if (key == "MR" && r.ok) c.input += "mem";
                c.mem = r.ok && key != "MC" ? r.message.substr(r.message.rfind(' ') + 1) : std::string();
                if (key == "MC") c.mem.clear();
            } else {
                if (c.justAnswered && c.input.empty() && (isOperator(key) || key == "!")) c.input = "ans";
                c.input += key == "mod" ? " mod " : key;
            }
            c.justAnswered = key == "=" && !c.error;
            c.focusInput = true;
        }
    }
}

void Shell::calcGraph() {
    CalcPanel& c = panels_->calc;
    const float unit = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(-unit * 5);
    if (ImGui::InputTextWithHint("##graph", "y = ...  (several: separate with ;)", &c.graphExpr, ImGuiInputTextFlags_EnterReturnsTrue)) c.graphStale = true;
    ImGui::SameLine();
    if (accentButton("Draw")) c.graphStale = true;
    ImGui::SetNextItemWidth(unit * 6);
    if (ImGui::InputDouble("from x", &c.xmin, 0, 0, "%g", ImGuiInputTextFlags_EnterReturnsTrue)) c.graphStale = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 6);
    if (ImGui::InputDouble("to x", &c.xmax, 0, 0, "%g", ImGuiInputTextFlags_EnterReturnsTrue)) c.graphStale = true;
    ImGui::SameLine();
    if (ImGui::Button("Zoom in")) {
        const double mid = (c.xmin + c.xmax) / 2, half = (c.xmax - c.xmin) / 4;
        c.xmin = mid - half;
        c.xmax = mid + half;
        c.graphStale = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Zoom out")) {
        const double mid = (c.xmin + c.xmax) / 2, half = (c.xmax - c.xmin);
        c.xmin = mid - half;
        c.xmax = mid + half;
        c.graphStale = true;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gw = std::floor(std::max(200.0f, avail.x) / 50) * 50, gh = std::floor(std::max(150.0f, avail.y - unit) / 50) * 50;
    if (gw != c.graphW || gh != c.graphH) c.graphStale = true;
    if (c.graphStale && !c.graphExpr.empty()) {
        c.graphStale = false;
        c.graphW = gw;
        c.graphH = gh;
        if (c.xmax <= c.xmin) c.xmax = c.xmin + 1;
        Result r = query("calc.graph", {{"expressions", c.graphExpr}, {"xmin", c.xmin}, {"xmax", c.xmax},
                                        {"width", static_cast<std::int64_t>(gw)}, {"height", static_cast<std::int64_t>(gh)}, {"dark", dark_}});
        c.graphError = r.ok ? std::string() : friendlyError(r.message);
        if (r.ok) tp::attachmentTexture(renderer_, c.graph, r);
    }
    tp::errorText(c.graphError);
    if (c.graphError.empty()) tp::fitImage(c.graph, avail.x, avail.y - unit);
}

void Shell::calcSide() {
    CalcPanel& c = panels_->calc;
    const float unit = ImGui::GetFontSize();
    std::vector<const char*> tabs = {"History", "Graph", "Convert"};
    if (pro_) tabs.insert(tabs.end(), {"Stats", "Solve", "Bases"});
    if (ImGui::BeginTabBar("calctabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
        for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
            if (ImGui::BeginTabItem(tabs[static_cast<std::size_t>(i)], nullptr, c.pickTab == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                c.tab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
        c.pickTab = -1;
    }
    if (c.tab >= static_cast<int>(tabs.size())) c.tab = 0;
    if (c.tab == 0) {
        if (c.historyStale) {
            Result h = query("calc.history", {{"limit", std::int64_t{200}}});
            c.history.clear();
            if (h.ok && tp::number(h, "count") > 0) {
                for (const auto& line : tp::splitRows(h.message)) {
                    const std::string& l = line[0];
                    const auto eq = l.rfind("  =  ");
                    if (eq != std::string::npos) c.history.push_back({l.substr(0, eq), l.substr(eq + 5)});
                }
            }
            c.historyStale = false;
        }
        if (c.history.empty()) tp::muted("Your calculations show up here. Click one to use it again.");
        ImGui::BeginChild("hist", {0, -ImGui::GetFrameHeightWithSpacing()});
        ImGui::PushFont(monoFont, 0.0f);
        for (std::size_t i = c.history.size(); i-- > 0;) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable((c.history[i][0] + "##h").c_str())) {
                c.input = c.history[i][0];
                c.justAnswered = false;
                c.focusInput = true;
            }
            ImGui::SetItemTooltip("Click to use it again");
            ImGui::TextDisabled("   = %s", c.history[i][1].c_str());
            ImGui::PopID();
        }
        ImGui::PopFont();
        ImGui::EndChild();
        if (ImGui::Button("Clear history")) {
            run("calc.clear_history", {});
            c.historyStale = true;
        }
        if (pro_) {
            ImGui::SameLine();
            Result v = query("calc.vars", {});
            ImGui::TextDisabled("%s", tp::number(v, "count") > 0 ? ("Variables: " + tp::text(v, "names")).c_str() : "No variables yet (x = 5 makes one)");
        }
    } else if (c.tab == 1) {
        calcGraph();
    } else if (c.tab == 2) {
        bool changed = false;
        static int group = 0;
        ImGui::SetNextItemWidth(unit * 10);
        if (ImGui::BeginCombo("##group", unitGroups()[static_cast<std::size_t>(group)].name)) {
            for (int g = 0; g < static_cast<int>(unitGroups().size()); ++g) {
                if (ImGui::Selectable(unitGroups()[static_cast<std::size_t>(g)].name, g == group)) {
                    group = g;
                    c.convFrom = unitGroups()[static_cast<std::size_t>(g)].units[0];
                    c.convTo = unitGroups()[static_cast<std::size_t>(g)].units[1];
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(unit * 10);
        changed = ImGui::InputText("amount", &c.convValue) || changed;
        auto unitPicker = [&](const char* id, std::string& value) {
            ImGui::SetNextItemWidth(unit * 7);
            bool ch = ImGui::InputText(id, &value);
            ImGui::SameLine();
            ImGui::PushID(id);
            if (ImGui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview)) {
                for (const char* u : unitGroups()[static_cast<std::size_t>(group)].units) {
                    if (ImGui::Selectable(u, value == u)) {
                        value = u;
                        ch = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
            return ch;
        };
        changed = unitPicker("from", c.convFrom) || changed;
        ImGui::SameLine();
        if (ImGui::Button("Swap")) {
            std::swap(c.convFrom, c.convTo);
            changed = true;
        }
        changed = unitPicker("to", c.convTo) || changed;
        if (changed || c.convAnswer.empty()) {
            Result r = query("calc.convert", {{"value", c.convValue}, {"from", c.convFrom}, {"to", c.convTo}});
            c.convAnswer = r.ok ? r.message : friendlyError(r.message);
        }
        ImGui::Spacing();
        tp::title(c.convAnswer, 1.5f);
        tp::muted("Any unit works in the boxes: km/h, kWh, ft^2, GiB... All offline, so there is no currency.");
    } else if (c.tab == 3) {
        tp::muted("Numbers separated by commas, spaces or new lines.");
        if (ImGui::InputTextMultiline("##stats", &c.statsData, {-1, unit * 6})) c.statsAnswer.clear();
        if (accentButton("Describe") || c.statsAnswer.empty()) {
            Result r = query("calc.stats", {{"data", c.statsData}});
            c.statsAnswer = r.ok ? r.message : friendlyError(r.message);
        }
        ImGui::PushFont(monoFont, 0.0f);
        ImGui::TextUnformatted(c.statsAnswer.c_str());
        ImGui::PopFont();
    } else if (c.tab == 4) {
        ImGui::SetNextItemWidth(-unit * 5);
        bool go = ImGui::InputTextWithHint("##solve", "x^2 = 2, or 2x + y = 5; x - y = 1", &c.solveText, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (accentButton("Solve") || go || c.solveAnswer.empty()) {
            Result r = query("calc.solve", {{"equation", c.solveText}});
            c.solveAnswer = r.ok ? r.message : friendlyError(r.message);
        }
        tp::title(c.solveAnswer, 1.3f);
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##base", "a whole number, 0xff, 0b1010 or an expression", &c.baseValue) || c.baseAnswer.empty()) {
            Result r = query("calc.base", {{"value", c.baseValue}});
            c.baseAnswer = r.ok ? r.message : friendlyError(r.message);
        }
        ImGui::PushFont(monoFont, ImGui::GetStyle().FontSizeBase * 1.2f);
        ImGui::TextUnformatted(c.baseAnswer.c_str());
        ImGui::PopFont();
    }
}

void Shell::calcPanel() {
    const float unit = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float padW = std::clamp(avail * 0.45f, unit * 16, unit * 26);
    ImGui::BeginChild("calcpad", {padW, 0}, ImGuiChildFlags_None);
    calcKeypad();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("calcside", {0, 0}, ImGuiChildFlags_Borders);
    calcSide();
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
