// The clock panel: the time here and around the world, timers, a stopwatch
// with laps, alarms that are remembered, a calendar and date maths. Finished
// timers and alarms pop up from any panel (Shell::backgroundTicks asks clock.due).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

std::string clockText(double seconds, bool tenths) {
    if (seconds < 0) seconds = 0;
    const auto whole = static_cast<long long>(tenths ? seconds : std::ceil(seconds - 1e-6));
    const int h = static_cast<int>(whole / 3600), m = static_cast<int>(whole / 60 % 60), s = static_cast<int>(whole % 60);
    char buf[48];
    if (tenths) {
        const int t = static_cast<int>((seconds - std::floor(seconds)) * 10);
        if (h) std::snprintf(buf, sizeof buf, "%d:%02d:%02d.%d", h, m, s, t);
        else std::snprintf(buf, sizeof buf, "%d:%02d.%d", m, s, t);
    } else if (h) {
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", h, m, s);
    } else {
        std::snprintf(buf, sizeof buf, "%d:%02d", m, s);
    }
    return buf;
}

std::string localWhen(const std::string& unixText) {
    std::time_t t = 0;
    try {
        t = static_cast<std::time_t>(std::stoll(unixText));
    } catch (...) {
        return {};
    }
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[48];
    std::strftime(buf, sizeof buf, "%a %d %b, %H:%M", &tm);
    return buf;
}

void bigText(const std::string& text, float scale) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
    const float w = ImGui::CalcTextSize(text.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetContentRegionAvail().x - w) * 0.5f));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopFont();
}

void centred(const std::string& text) {
    const float w = ImGui::CalcTextSize(text.c_str()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetContentRegionAvail().x - w) * 0.5f));
    ImGui::TextDisabled("%s", text.c_str());
}

}  // namespace

void Shell::clockTick() {
    ClockPanel& c = panels_->clock;
    const double now = tp::seconds();
    const bool fast = c.tab == 3 && c.swRunning;
    if (now - c.lastTick < (fast ? 0.09 : 0.95)) return;
    c.lastTick = now;
    Result n = query("clock.now", {});
    c.time = tp::text(n, "time");
    c.date = tp::text(n, "weekday") + " " + tp::text(n, "date");
    c.weekInfo = "week " + tp::text(n, "week") + ", day " + tp::text(n, "day_of_year");
    c.zone = n.message.substr(n.message.find('\n') + 1);
    c.zone = c.zone.substr(0, c.zone.find(';'));
    c.timers = tp::rows(query("clock.timers", {}));
    if (c.tab == 3 || c.swRunning || c.swSeconds == 0) {
        Result sw = query("clock.stopwatch", {});
        c.swSeconds = tp::number(sw, "seconds");
        c.swRunning = tp::number(sw, "running") != 0;
        c.laps = tp::numbers(sw, "laps");
    }
    if (c.tab == 1) {
        Result w = query("clock.world", {{"places", c.worldPlaces}});
        c.world = tp::rows(w);
        c.error = w.ok ? std::string() : friendlyError(w.message);
    }
}

void Shell::clockTimers() {
    ClockPanel& c = panels_->clock;
    const float unit = ImGui::GetFontSize();
    tp::title("Start a timer", 1.1f);
    const char* quick[] = {"1 min", "3 min", "5 min", "10 min", "15 min", "25 min", "1 hour"};
    const char* quickDur[] = {"1m", "3m", "5m", "10m", "15m", "25m", "1h"};
    for (int i = 0; i < 7; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::Button(quick[i])) {
            run("clock.timer_start", {{"duration", std::string(quickDur[i])}});
            c.lastTick = -100;
        }
    }
    ImGui::SetNextItemWidth(unit * 7);
    ImGui::InputTextWithHint("##dur", "1h30m, 90s", &c.timerText);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 10);
    ImGui::InputTextWithHint("##label", "what for (optional)", &c.timerLabel);
    ImGui::SameLine();
    if (accentButton("Start")) {
        Params p{{"duration", c.timerText}};
        if (!c.timerLabel.empty()) p["label"] = c.timerLabel;
        Result r = run("clock.timer_start", p);
        c.error = r.ok ? std::string() : friendlyError(r.message);
        c.timerLabel.clear();
        c.lastTick = -100;
    }
    tp::errorText(c.error);
    ImGui::Spacing();
    if (c.timers.empty()) tp::muted("No timers. When one finishes, a message pops up wherever you are in the app.");
    for (const auto& t : c.timers) {
        if (t.size() < 4) continue;
        ImGui::PushID(t[0].c_str());
        const double left = std::stod(t[1]), total = std::stod(t[2]);
        const std::string label = t.size() > 4 && !t[4].empty() ? t[4] : "Timer " + t[0].substr(1);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(unit * 9);
        const std::string state = t[3] == "done" ? "done" : clockText(left, false) + (t[3] == "paused" ? " (paused)" : "");
        ImGui::ProgressBar(total > 0 ? static_cast<float>(1 - left / total) : 1.0f, {unit * 14, 0}, state.c_str());
        ImGui::SameLine();
        if (t[3] == "running" && ImGui::Button("Pause")) run("clock.timer_pause", {{"timer", t[0]}});
        if (t[3] == "paused" && ImGui::Button("Resume")) run("clock.timer_resume", {{"timer", t[0]}});
        ImGui::SameLine();
        if (ImGui::Button(t[3] == "done" ? "Remove" : "Cancel")) run("clock.timer_cancel", {{"timer", t[0]}});
        ImGui::PopID();
    }
}

void Shell::clockStopwatch() {
    ClockPanel& c = panels_->clock;
    const float unit = ImGui::GetFontSize();
    ImGui::Spacing();
    ImGui::PushFont(monoFont, 0.0f);
    bigText(clockText(c.swSeconds, true), 4.0f);
    ImGui::PopFont();
    const float bw = unit * 7;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - bw * 3 - ImGui::GetStyle().ItemSpacing.x * 2) * 0.5f));
    auto act = [&](const char* action) {
        Result r = run("clock.stopwatch", {{"action", std::string(action)}});
        c.swSeconds = tp::number(r, "seconds");
        c.swRunning = tp::number(r, "running") != 0;
        c.laps = tp::numbers(r, "laps");
    };
    if (accentButton(c.swRunning ? "Stop" : "Start", {bw, unit * 2.2f})) act(c.swRunning ? "stop" : "start");
    ImGui::SameLine();
    ImGui::BeginDisabled(!c.swRunning);
    if (ImGui::Button("Lap", {bw, unit * 2.2f})) act("lap");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset", {bw, unit * 2.2f})) act("reset");
    ImGui::Spacing();
    if (!c.laps.empty() && ImGui::BeginTable("laps", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Lap");
        ImGui::TableSetupColumn("Lap time");
        ImGui::TableSetupColumn("Total");
        ImGui::TableHeadersRow();
        double prev = 0;
        std::vector<std::pair<double, double>> rows;
        for (double total : c.laps) {
            rows.emplace_back(total - prev, total);
            prev = total;
        }
        for (std::size_t i = rows.size(); i-- > 0;) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%zu", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(clockText(rows[i].first, true).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(clockText(rows[i].second, true).c_str());
        }
        ImGui::EndTable();
    }
}

void Shell::clockAlarms() {
    ClockPanel& c = panels_->clock;
    const float unit = ImGui::GetFontSize();
    tp::title("New alarm", 1.1f);
    ImGui::SetNextItemWidth(unit * 6);
    ImGui::InputTextWithHint("##atime", "07:30", &c.alarmTime);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 8);
    const char* days[] = {"daily", "weekdays", "weekends", "once"};
    if (ImGui::BeginCombo("##days", c.alarmDays.c_str())) {
        for (const char* d : days) {
            if (ImGui::Selectable(d, c.alarmDays == d)) c.alarmDays = d;
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("You can also type days like mon,wed,fri or a date");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 10);
    ImGui::InputTextWithHint("##alabel", "label (optional)", &c.alarmLabel);
    ImGui::SameLine();
    if (accentButton("Add alarm")) {
        Params p{{"time", c.alarmTime}, {"days", c.alarmDays}};
        if (!c.alarmLabel.empty()) p["label"] = c.alarmLabel;
        Result r = run("clock.alarm_add", p);
        c.error = r.ok ? sentence(r.message) : friendlyError(r.message);
        c.alarms = tp::rows(query("clock.alarms", {}));
        c.alarmLabel.clear();
    }
    tp::muted(c.error);
    ImGui::Spacing();
    if (c.alarms.empty()) tp::muted("No alarms. They are saved, so they come back next time you open the app.");
    for (const auto& a : c.alarms) {
        if (a.size() < 5) continue;
        ImGui::PushID(a[0].c_str());
        bool on = a[3] == "on";
        if (ImGui::Checkbox("##on", &on)) {
            run("clock.alarm_toggle", {{"alarm", a[0]}, {"on", on}});
            c.alarms = tp::rows(query("clock.alarms", {}));
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.5f);
        ImGui::TextUnformatted(a[1].c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  %s", a[2].c_str(), a.size() > 5 ? a[5].c_str() : "");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", on && a[4] != "0" ? ("next " + localWhen(a[4])).c_str() : "off");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            run("clock.alarm_remove", {{"alarm", a[0]}});
            c.alarms = tp::rows(query("clock.alarms", {}));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
}

void Shell::clockDates() {
    ClockPanel& c = panels_->clock;
    const float unit = ImGui::GetFontSize();
    tp::title("Days between two dates", 1.1f);
    ImGui::SetNextItemWidth(unit * 9);
    bool diff = ImGui::InputTextWithHint("##from", "2026-01-01", &c.diffFrom);
    ImGui::SameLine();
    ImGui::TextUnformatted("and");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 9);
    diff = ImGui::InputTextWithHint("##to", "today", &c.diffTo) || diff;
    if (diff && !c.diffFrom.empty()) {
        Params p{{"from", c.diffFrom}};
        if (!c.diffTo.empty()) p["to"] = c.diffTo;
        Result r = query("clock.date_diff", p);
        c.diffAnswer = r.ok ? r.message : friendlyError(r.message);
    }
    tp::muted(c.diffAnswer);
    ImGui::Spacing();
    tp::title("Add to a date", 1.1f);
    ImGui::SetNextItemWidth(unit * 9);
    bool add = ImGui::InputTextWithHint("##adate", "today", &c.addDate);
    ImGui::SameLine();
    ImGui::TextUnformatted("plus");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 10);
    add = ImGui::InputTextWithHint("##amount", "3 weeks, -10 days, 5 working days", &c.addAmount) || add;
    if ((add || c.addAnswer.empty()) && !c.addAmount.empty()) {
        Params p{{"add", c.addAmount}};
        if (!c.addDate.empty()) p["date"] = c.addDate;
        Result r = query("clock.date_add", p);
        c.addAnswer = r.ok ? r.message : friendlyError(r.message);
    }
    tp::muted(c.addAnswer);
    if (!pro_) return;
    ImGui::Spacing();
    tp::title("Convert a time between places", 1.1f);
    ImGui::SetNextItemWidth(unit * 7);
    bool conv = ImGui::InputText("##ctime", &c.convTime);
    ImGui::SameLine();
    ImGui::TextUnformatted("in");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 9);
    conv = ImGui::InputText("##cfrom", &c.convFrom) || conv;
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 12);
    conv = ImGui::InputText("##cto", &c.convTo) || conv;
    if (conv || c.convAnswer.empty()) {
        Result r = query("clock.convert", {{"time", c.convTime}, {"from", c.convFrom}, {"to", c.convTo}});
        c.convAnswer = r.ok ? r.message : friendlyError(r.message);
    }
    ImGui::PushFont(monoFont, 0.0f);
    ImGui::TextUnformatted(c.convAnswer.c_str());
    ImGui::PopFont();
}

void Shell::clockPanel() {
    ClockPanel& c = panels_->clock;
    clockTick();
    const float unit = ImGui::GetFontSize();
    const char* tabs[] = {"Clock", "World", "Timers", "Stopwatch", "Alarms", "Calendar", "Dates"};
    if (ImGui::BeginTabBar("clocktabs")) {
        for (int i = 0; i < 7; ++i) {
            std::string label = tabs[i];
            if (i == 2 && !c.timers.empty()) label += " (" + std::to_string(c.timers.size()) + ")";
            if (ImGui::BeginTabItem((label + "###" + tabs[i]).c_str(), nullptr, c.pickTab == i ? ImGuiTabItemFlags_SetSelected : 0)) {
                if (c.tab != i) c.lastTick = -100;
                c.tab = i;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
        c.pickTab = -1;
    }
    ImGui::BeginChild("clockbody");
    switch (c.tab) {
        case 0: {
            ImGui::Dummy({0, unit * 2});
            ImGui::PushFont(monoFont, 0.0f);
            bigText(c.time, 5.0f);
            ImGui::PopFont();
            bigText(c.date, 1.6f);
            centred(c.zone + ", " + c.weekInfo);
            ImGui::Spacing();
            if (!c.timers.empty()) {
                for (const auto& t : c.timers) {
                    if (t.size() > 3) centred((t.size() > 4 && !t[4].empty() ? t[4] : t[0]) + ": " + (t[3] == "done" ? "done" : clockText(std::stod(t[1]), false) + " left"));
                }
            }
            int on = 0;
            for (const auto& a : c.alarms) on += a.size() > 3 && a[3] == "on";
            if (on) centred(std::to_string(on) + " alarm(s) set");
            break;
        }
        case 1: {
            ImGui::SetNextItemWidth(unit * 12);
            bool add = ImGui::InputTextWithHint("##addcity", "add a city, e.g. Berlin", &c.addPlace, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if ((ImGui::Button("Add") || add) && !c.addPlace.empty()) {
                c.worldPlaces += "|" + c.addPlace;
                c.addPlace.clear();
                c.lastTick = -100;
            }
            tp::errorText(c.error);
            if (ImGui::BeginTable("world", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("City", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, unit * 4);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, unit * 4.5f);
                ImGui::TableHeadersRow();
                std::string remove;
                for (const auto& w : c.world) {
                    if (w.size() < 5) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(w[0].c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
                    ImGui::TextUnformatted(w[1].size() >= 16 ? w[1].substr(11, 5).c_str() : w[1].c_str());
                    ImGui::PopFont();
                    ImGui::TableNextColumn();
                    const std::string day = w[4] == "1" ? "tomorrow" : w[4] == "-1" ? "yesterday" : "today";
                    const int off = std::stoi(w[2]);
                    char offText[32];
                    std::snprintf(offText, sizeof offText, "UTC%+d:%02d", off / 60, std::abs(off % 60));
                    ImGui::TextDisabled("%s, %s%s", day.c_str(), offText, w[3] == "dst" ? " summer" : "");
                    ImGui::TableNextColumn();
                    ImGui::PushID(w[0].c_str());
                    if (ImGui::SmallButton("Remove")) remove = w[0];
                    ImGui::PopID();
                }
                ImGui::EndTable();
                if (!remove.empty()) {
                    std::string kept;
                    std::size_t start = 0;
                    const std::string& all = c.worldPlaces;
                    while (start <= all.size()) {
                        std::size_t bar = all.find('|', start);
                        const std::string name = all.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
                        if (tp::lowerAscii(name) != tp::lowerAscii(remove) && !name.empty()) kept += (kept.empty() ? "" : "|") + name;
                        if (bar == std::string::npos) break;
                        start = bar + 1;
                    }
                    c.worldPlaces = kept;
                    c.lastTick = -100;
                }
            }
            break;
        }
        case 2:
            clockTimers();
            break;
        case 3:
            clockStopwatch();
            break;
        case 4:
            clockAlarms();
            break;
        case 5: {
            if (c.calYear == 0) {
                const std::string d = c.date.substr(c.date.find(' ') + 1);
                c.calYear = d.size() >= 7 ? std::stoi(d.substr(0, 4)) : 2026;
                c.calMonth = d.size() >= 7 ? std::stoi(d.substr(5, 2)) : 1;
                c.calendar.clear();
            }
            bool move = false;
            if (ImGui::Button("<")) {
                if (--c.calMonth < 1) {
                    c.calMonth = 12;
                    --c.calYear;
                }
                move = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Today")) {
                c.calYear = 0;
                move = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(">")) {
                if (++c.calMonth > 12) {
                    c.calMonth = 1;
                    ++c.calYear;
                }
                move = true;
            }
            if (move) c.calendar.clear();
            if (c.calYear == 0) break;
            if (c.calendar.empty()) {
                Result r = query("clock.calendar", {{"year", static_cast<std::int64_t>(c.calYear)}, {"month", static_cast<std::int64_t>(c.calMonth)}});
                c.calendar = r.ok ? r.message.substr(0, r.message.rfind("\n\n") == std::string::npos ? std::string::npos : r.message.rfind("\n\n")) : friendlyError(r.message);
            }
            ImGui::PushFont(monoFont, ImGui::GetStyle().FontSizeBase * 1.4f);
            ImGui::TextUnformatted(c.calendar.c_str());
            ImGui::PopFont();
            tp::muted("Today is in [brackets]; the first column is the week number.");
            break;
        }
        default:
            clockDates();
            break;
    }
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
