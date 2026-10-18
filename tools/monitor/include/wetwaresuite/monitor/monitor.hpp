// monitor: what the computer is doing. Processor use per core, memory, disks,
// processes (and ending them), network traffic, battery and system details, read
// straight from the operating system (/proc on Linux; sysctl, mach and libproc on
// macOS; the Win32 API on Windows) with no helper programs.
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::monitor {

struct CpuTimes {
    std::uint64_t busy = 0, total = 0;  // ticks since boot
};

struct CpuInfo {
    std::string model;
    int logical = 0, physical = 0;
    double mhz = 0;
    double total = 0;              // percent busy, all cores together
    std::vector<double> perCore;   // percent busy
    double load[3] = {-1, -1, -1}; // 1, 5, 15 minute load average (not on Windows)
};

struct MemInfo {
    std::uint64_t total = 0, available = 0, used = 0;
    std::uint64_t swapTotal = 0, swapUsed = 0;
};

struct DiskInfo {
    std::string mount, device, fs;
    std::uint64_t total = 0, free = 0, available = 0;  // available: what a normal user may still write
};

struct ProcInfo {
    std::int64_t pid = 0, ppid = 0;
    std::string name, user, command;
    double cpu = 0;           // percent of one core (can pass 100 with several threads)
    std::uint64_t memory = 0; // resident bytes
    int threads = 0;
    std::string state;
    double cpuSeconds = 0;    // processor time used so far
};

struct NetInfo {
    std::string name;
    std::uint64_t rx = 0, tx = 0;  // bytes received and sent since boot
    double rxRate = 0, txRate = 0; // bytes per second since the last look
    bool up = false;
};

struct BatteryInfo {
    bool present = false;
    double percent = 0;
    bool charging = false, plugged = false;
    double minutesLeft = -1;
};

struct SystemInfo {
    std::string os, version, kernel, arch, hostname, user;
    double uptime = 0;  // seconds
};

// One call per platform; what the commands are built on.
namespace platform {
std::vector<CpuTimes> cpuTimes();  // [0] = all cores, then one per core
std::string cpuModel();
double cpuMhz();
int physicalCores();
bool loadAverage(double out[3]);
MemInfo memory();
std::vector<DiskInfo> disks();
std::vector<ProcInfo> processes();  // cpu left at 0; cpuSeconds filled
bool kill(std::int64_t pid, bool force, std::string& error);
SystemInfo system();
std::vector<NetInfo> network();     // counters only
BatteryInfo battery();
}  // namespace platform

// Remembers the last samples, so processor use and network speed are measured
// between two looks (the first look waits a moment to have something to compare).
class Monitor {
public:
    CpuInfo cpu();
    std::vector<ProcInfo> processes();
    std::vector<NetInfo> network();

private:
    using Clock = std::chrono::steady_clock;
    std::vector<CpuTimes> lastCpu_;
    Clock::time_point lastCpuAt_{};
    std::map<std::int64_t, double> lastProc_;
    Clock::time_point lastProcAt_{};
    std::map<std::string, NetInfo> lastNet_;
    Clock::time_point lastNetAt_{};
};

std::string formatBytes(std::uint64_t bytes);
std::string formatDuration(double seconds);  // "3 days 4 h", "2 h 05 min", "42 s"

void registerCommands(Session& session);

}  // namespace wetwaresuite::monitor
