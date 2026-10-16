// The system monitor panel: live processor and memory graphs, each core, the
// disks, network speed, battery, and a table of running programs that can be
// sorted, filtered and ended. It samples monitor.* commands about once a second.
#include <algorithm>
#include <cstdio>

#include "tool_panels.hpp"
// after imgui.h
#include "imgui_stdlib.h"

namespace wetwaresuite::gui {

namespace {

void meter(const char* label, double fraction, const std::string& text) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7);
    ImGui::ProgressBar(static_cast<float>(std::clamp(fraction, 0.0, 1.0)), {-1, 0}, text.c_str());
}

std::vector<float> asVector(const std::deque<float>& d) { return {d.begin(), d.end()}; }

}  // namespace

void Shell::monitorSample() {
    MonitorPanel& m = panels_->monitor;
    const double now = tp::seconds();
    if (now - m.lastSample >= 1.0) {
        m.lastSample = now;
        Result cpu = query("monitor.cpu", {});
        m.cpuPercent = tp::number(cpu, "percent");
        m.cpuModel = tp::text(cpu, "model") + "  (" + tp::text(cpu, "cores") + " cores, " + tp::text(cpu, "threads") + " threads)";
        m.perCore.clear();
        for (double v : tp::numbers(cpu, "per_core")) m.perCore.push_back(static_cast<float>(v));
        Result mem = query("monitor.memory", {});
        m.memTotal = tp::number(mem, "total");
        m.memUsed = tp::number(mem, "used");
        m.swapTotal = tp::number(mem, "swap_total");
        m.swapUsed = tp::number(mem, "swap_used");
        m.cpu.push_back(static_cast<float>(m.cpuPercent));
        m.mem.push_back(static_cast<float>(m.memTotal > 0 ? 100 * m.memUsed / m.memTotal : 0));
        while (m.cpu.size() > 120) m.cpu.pop_front();
        while (m.mem.size() > 120) m.mem.pop_front();
        m.network = tp::rows(query("monitor.network", {}));
    }
    if (now - m.lastProcs >= 2.5) {
        m.lastProcs = now;
        Params p{{"sort", m.sort}, {"limit", std::int64_t{80}}};
        if (!m.filter.empty()) p["filter"] = m.filter;
        m.procs = tp::rows(query("monitor.processes", p));
    }
    if (now - m.lastSlow >= 10.0) {
        m.lastSlow = now;
        m.disks = tp::rows(query("monitor.disks", {}));
        Result sys = query("monitor.system", {});
        m.system = tp::text(sys, "version") + " (" + tp::text(sys, "kernel") + ", " + tp::text(sys, "arch") + ") on " + tp::text(sys, "hostname");
        const double up = tp::number(sys, "uptime");
        char buf[64];
        std::snprintf(buf, sizeof buf, "up for %d days %d h %02d min", static_cast<int>(up / 86400), static_cast<int>(up / 3600) % 24,
                      static_cast<int>(up / 60) % 60);
        m.uptime = buf;
        Result bat = query("monitor.battery", {});
        m.battery = tp::number(bat, "present") != 0 ? bat.message.substr(0, bat.message.find('\n')) : std::string();
    }
}

void Shell::monitorPanel() {
    MonitorPanel& m = panels_->monitor;
    monitorSample();
    const float unit = ImGui::GetFontSize();
    if (ImGui::BeginTabBar("montabs")) {
        if (ImGui::BeginTabItem("Overview", nullptr, m.pickTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            m.tab = 0;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(pro_ ? "Processes###procs" : "Programs###procs", nullptr, m.pickTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            m.tab = 1;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        m.pickTab = -1;
    }
    if (m.tab == 0) {
        ImGui::BeginChild("overview");
        tp::muted(m.system + ", " + m.uptime);
        const float graphH = unit * 5;
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "Processor %.0f%%", m.cpuPercent);
        const auto cpu = asVector(m.cpu), mem = asVector(m.mem);
        ImGui::PlotLines("##cpu", cpu.data(), static_cast<int>(cpu.size()), 0, overlay, 0, 100, {half, graphH});
        ImGui::SameLine();
        std::snprintf(overlay, sizeof overlay, "Memory %.0f%%", mem.empty() ? 0.0 : static_cast<double>(mem.back()));
        ImGui::PlotLines("##mem", mem.data(), static_cast<int>(mem.size()), 0, overlay, 0, 100, {half, graphH});
        ImGui::Spacing();
        tp::title("Processor", 1.1f);
        tp::muted(m.cpuModel);
        if (pro_) {
            const int cols = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (unit * 12)));
            for (std::size_t i = 0; i < m.perCore.size(); ++i) {
                if (i % static_cast<std::size_t>(cols)) ImGui::SameLine();
                char label[32];
                std::snprintf(label, sizeof label, "core %zu  %.0f%%", i + 1, static_cast<double>(m.perCore[i]));
                ImGui::ProgressBar(m.perCore[i] / 100.0f, {unit * 11.5f, 0}, label);
            }
        } else {
            meter("In use", m.cpuPercent / 100, std::to_string(static_cast<int>(m.cpuPercent)) + "%");
        }
        ImGui::Spacing();
        tp::title("Memory", 1.1f);
        meter("In use", m.memTotal > 0 ? m.memUsed / m.memTotal : 0, tp::humanSize(m.memUsed) + " of " + tp::humanSize(m.memTotal));
        if (m.swapTotal > 0) meter("Swap", m.swapUsed / m.swapTotal, tp::humanSize(m.swapUsed) + " of " + tp::humanSize(m.swapTotal));
        ImGui::Spacing();
        tp::title("Disks", 1.1f);
        for (const auto& d : m.disks) {
            if (d.size() < 4) continue;
            const double total = std::stod(d[1]), used = std::stod(d[2]), free = std::stod(d[3]);
            ImGui::PushID(d[0].c_str());
            meter(d[0].size() > 14 ? ("..." + d[0].substr(d[0].size() - 11)).c_str() : d[0].c_str(), total > 0 ? used / total : 0,
                  tp::humanSize(free) + " free of " + tp::humanSize(total) + (pro_ && d.size() > 4 ? "  " + d[4] : ""));
            ImGui::SetItemTooltip("%s", d[0].c_str());
            ImGui::PopID();
        }
        ImGui::Spacing();
        tp::title("Network", 1.1f);
        if (ImGui::BeginTable("net", pro_ ? 5 : 3, ImGuiTableFlags_SizingStretchProp)) {
            for (const auto& n : m.network) {
                if (n.size() < 6 || (!pro_ && (n[1] != "up" || n[0] == "lo"))) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n[0].c_str());
                ImGui::TableNextColumn();
                ImGui::Text("down %s/s", tp::humanSize(std::stod(n[4])).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("up %s/s", tp::humanSize(std::stod(n[5])).c_str());
                if (pro_) {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", n[1].c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s received, %s sent", tp::humanSize(std::stod(n[2])).c_str(), tp::humanSize(std::stod(n[3])).c_str());
                }
            }
            ImGui::EndTable();
        }
        if (!m.battery.empty()) {
            ImGui::Spacing();
            tp::title("Battery", 1.1f);
            ImGui::TextUnformatted(m.battery.c_str());
        }
        ImGui::EndChild();
        return;
    }

    // programs
    ImGui::SetNextItemWidth(unit * 12);
    if (ImGui::InputTextWithHint("##filter", "find a program", &m.filter)) m.lastProcs = -100;
    ImGui::SameLine();
    const char* sorts[] = {"cpu", "memory", "name", "pid"};
    const char* sortNames[] = {"busiest first", "most memory first", "by name", "by id"};
    ImGui::SetNextItemWidth(unit * 11);
    int si = 0;
    for (int i = 0; i < 4; ++i) {
        if (m.sort == sorts[i]) si = i;
    }
    if (ImGui::Combo("##sort", &si, sortNames, 4)) {
        m.sort = sorts[si];
        m.lastProcs = -100;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m.selectedPid < 0);
    if (ImGui::Button("End program")) ImGui::OpenPopup("endprog");
    if (pro_) {
        ImGui::SameLine();
        if (ImGui::Button("Force end")) {
            Result r = run("monitor.kill", {{"pid", static_cast<std::int64_t>(m.selectedPid)}, {"force", true}});
            m.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            m.lastProcs = -100;
        }
        ImGui::SameLine();
        if (ImGui::Button("Details")) {
            Result r = query("monitor.process", {{"pid", static_cast<std::int64_t>(m.selectedPid)}});
            m.status = r.ok ? r.message : friendlyError(r.message);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("endprog")) {
        std::string name;
        for (const auto& p : m.procs) {
            if (p.size() > 2 && p[0] == std::to_string(m.selectedPid)) name = p[2];
        }
        ImGui::Text("End %s? Unsaved work in it may be lost.", name.c_str());
        if (accentButton("End it")) {
            Result r = run("monitor.kill", {{"pid", static_cast<std::int64_t>(m.selectedPid)}});
            m.status = r.ok ? sentence(r.message) : friendlyError(r.message);
            m.lastProcs = -100;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it running")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    tp::muted(m.status);
    const int cols = pro_ ? 7 : 3;
    if (!ImGui::BeginTable("procs", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthStretch, 3);
    ImGui::TableSetupColumn("Processor", ImGuiTableColumnFlags_WidthStretch, 1);
    ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthStretch, 1);
    if (pro_) {
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("User", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 4);
    }
    ImGui::TableHeadersRow();
    for (const auto& p : m.procs) {
        if (p.size() < 9) continue;
        const long long pid = std::stoll(p[0]);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(p[0].c_str());
        if (ImGui::Selectable(p[2].c_str(), pid == m.selectedPid, ImGuiSelectableFlags_SpanAllColumns)) m.selectedPid = pid;
        ImGui::SetItemTooltip("%s", p[8].c_str());
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::Text("%.1f%%", std::stod(p[3]));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tp::humanSize(std::stod(p[4])).c_str());
        if (pro_) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p[0].c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p[5].c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p[6].c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", p[8].c_str());
        }
    }
    ImGui::EndTable();
}

}  // namespace wetwaresuite::gui
