// Rates between two looks, and formatting.
#include <algorithm>
#include <cstdio>
#include <thread>

#include "wetwaresuite/monitor/monitor.hpp"

namespace wetwaresuite::monitor {

namespace {

double pct(std::uint64_t busy0, std::uint64_t total0, std::uint64_t busy1, std::uint64_t total1) {
    if (total1 <= total0 || busy1 < busy0) return 0;
    return std::clamp(100.0 * static_cast<double>(busy1 - busy0) / static_cast<double>(total1 - total0), 0.0, 100.0);
}

}  // namespace

CpuInfo Monitor::cpu() {
    auto nowT = Clock::now();
    if (lastCpu_.empty() || nowT - lastCpuAt_ < std::chrono::milliseconds(100)) {
        if (lastCpu_.empty() || nowT - lastCpuAt_ > std::chrono::seconds(30)) {
            lastCpu_ = platform::cpuTimes();
            lastCpuAt_ = nowT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        nowT = Clock::now();
    }
    auto cur = platform::cpuTimes();
    CpuInfo c;
    static const std::string model = platform::cpuModel();
    static const int physical = platform::physicalCores();
    c.model = model;
    c.physical = physical;
    c.mhz = platform::cpuMhz();
    c.logical = static_cast<int>(cur.size() > 1 ? cur.size() - 1 : std::thread::hardware_concurrency());
    if (!cur.empty() && cur.size() == lastCpu_.size()) {
        c.total = pct(lastCpu_[0].busy, lastCpu_[0].total, cur[0].busy, cur[0].total);
        for (std::size_t i = 1; i < cur.size(); ++i) c.perCore.push_back(pct(lastCpu_[i].busy, lastCpu_[i].total, cur[i].busy, cur[i].total));
    }
    if (!platform::loadAverage(c.load)) c.load[0] = c.load[1] = c.load[2] = -1;
    lastCpu_ = cur;
    lastCpuAt_ = nowT;
    return c;
}

std::vector<ProcInfo> Monitor::processes() {
    auto nowT = Clock::now();
    if (lastProc_.empty() || nowT - lastProcAt_ > std::chrono::seconds(30)) {
        for (const auto& p : platform::processes()) lastProc_[p.pid] = p.cpuSeconds;
        lastProcAt_ = nowT;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        nowT = Clock::now();
    }
    auto list = platform::processes();
    const double wall = std::chrono::duration<double>(nowT - lastProcAt_).count();
    std::map<std::int64_t, double> next;
    for (auto& p : list) {
        auto it = lastProc_.find(p.pid);
        if (it != lastProc_.end() && wall > 0.05) p.cpu = std::max(0.0, (p.cpuSeconds - it->second) / wall * 100.0);
        next[p.pid] = p.cpuSeconds;
    }
    if (wall > 0.05) {
        lastProc_ = std::move(next);
        lastProcAt_ = nowT;
    }
    return list;
}

std::vector<NetInfo> Monitor::network() {
    auto nowT = Clock::now();
    auto list = platform::network();
    const double wall = std::chrono::duration<double>(nowT - lastNetAt_).count();
    for (auto& n : list) {
        auto it = lastNet_.find(n.name);
        if (it != lastNet_.end() && wall > 0.05 && wall < 600) {
            n.rxRate = n.rx >= it->second.rx ? static_cast<double>(n.rx - it->second.rx) / wall : 0;
            n.txRate = n.tx >= it->second.tx ? static_cast<double>(n.tx - it->second.tx) / wall : 0;
        }
    }
    if (wall > 0.05 || lastNet_.empty()) {
        lastNet_.clear();
        for (const auto& n : list) lastNet_[n.name] = n;
        lastNetAt_ = nowT;
    }
    return list;
}

std::string formatBytes(std::uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024 && u < 5) {
        v /= 1024;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
    return buf;
}

std::string formatDuration(double s) {
    char buf[48];
    const long long t = static_cast<long long>(s);
    if (t >= 86400) std::snprintf(buf, sizeof buf, "%lld day%s %lld h", t / 86400, t / 86400 == 1 ? "" : "s", (t % 86400) / 3600);
    else if (t >= 3600) std::snprintf(buf, sizeof buf, "%lld h %02lld min", t / 3600, (t % 3600) / 60);
    else if (t >= 60) std::snprintf(buf, sizeof buf, "%lld min %02lld s", t / 60, t % 60);
    else std::snprintf(buf, sizeof buf, "%lld s", t);
    return buf;
}

}  // namespace wetwaresuite::monitor
