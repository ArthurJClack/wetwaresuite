// Windows: the Win32 API (no WMI, no PowerShell).
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  // GetIfTable2 needs Vista or later
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <map>
#include <string>

#include "wetwaresuite/monitor/monitor.hpp"

namespace wetwaresuite::monitor::platform {

namespace {

std::string utf8(const wchar_t* w, int len = -1) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, s.data(), n, nullptr, nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::uint64_t ft(const FILETIME& f) { return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

std::string regString(const wchar_t* key, const wchar_t* value) {
    wchar_t buf[256];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return utf8(buf);
}

// per-core times come from ntdll, looked up at run time
struct CorePerf {
    LARGE_INTEGER idle, kernel, user, dpc, interrupt;
    ULONG interruptCount;
};
using QuerySystemInformation = LONG(WINAPI*)(int, void*, ULONG, ULONG*);

}  // namespace

std::vector<CpuTimes> cpuTimes() {
    std::vector<CpuTimes> out(1);
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        out[0].total = ft(kernel) + ft(user);  // kernel time includes idle time
        out[0].busy = out[0].total - ft(idle);
    }
    static auto query = reinterpret_cast<QuerySystemInformation>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    if (!query) return out;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    std::vector<CorePerf> perf(si.dwNumberOfProcessors);
    ULONG got = 0;
    if (query(8 /* SystemProcessorPerformanceInformation */, perf.data(), static_cast<ULONG>(perf.size() * sizeof(CorePerf)), &got) != 0) return out;
    for (std::size_t i = 0; i < got / sizeof(CorePerf); ++i) {
        CpuTimes c;
        c.total = static_cast<std::uint64_t>(perf[i].kernel.QuadPart + perf[i].user.QuadPart);
        c.busy = c.total - static_cast<std::uint64_t>(perf[i].idle.QuadPart);
        out.push_back(c);
    }
    return out;
}

std::string cpuModel() { return regString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString"); }

double cpuMhz() {
    DWORD mhz = 0, size = sizeof mhz;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size) != ERROR_SUCCESS) return 0;
    return mhz;
}

int physicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    if (!GetLogicalProcessorInformation(info.data(), &len)) return 0;
    int n = 0;
    for (std::size_t i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i) {
        if (info[i].Relationship == RelationProcessorCore) ++n;
    }
    return n;
}

bool loadAverage(double[3]) { return false; }

MemInfo memory() {
    MemInfo r;
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return r;
    r.total = m.ullTotalPhys;
    r.available = m.ullAvailPhys;
    r.used = r.total - r.available;
    if (m.ullTotalPageFile > m.ullTotalPhys) {
        r.swapTotal = m.ullTotalPageFile - m.ullTotalPhys;
        const std::uint64_t commitUsed = m.ullTotalPageFile - m.ullAvailPageFile;
        r.swapUsed = commitUsed > r.used ? std::min(commitUsed - r.used, r.swapTotal) : 0;
    }
    return r;
}

std::vector<DiskInfo> disks() {
    std::vector<DiskInfo> out;
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(511, buf);
    for (const wchar_t* d = buf; n && *d; d += wcslen(d) + 1) {
        UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
        ULARGE_INTEGER avail, total, freeBytes;
        if (!GetDiskFreeSpaceExW(d, &avail, &total, &freeBytes)) continue;
        wchar_t fsName[64] = {}, label[256] = {};
        GetVolumeInformationW(d, label, 256, nullptr, nullptr, nullptr, fsName, 64);
        DiskInfo di;
        di.mount = utf8(d);
        di.device = label[0] ? utf8(label) : (type == DRIVE_REMOVABLE ? "removable" : type == DRIVE_REMOTE ? "network" : "local disk");
        di.fs = utf8(fsName);
        di.total = total.QuadPart;
        di.free = freeBytes.QuadPart;
        di.available = avail.QuadPart;
        out.push_back(di);
    }
    return out;
}

std::vector<ProcInfo> processes() {
    std::vector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        ProcInfo p;
        p.pid = e.th32ProcessID;
        p.ppid = e.th32ParentProcessID;
        p.name = utf8(e.szExeFile);
        p.threads = static_cast<int>(e.cntThreads);
        p.state = "R";
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
        if (h) {
            FILETIME c, x, k, u;
            if (GetProcessTimes(h, &c, &x, &k, &u)) p.cpuSeconds = static_cast<double>(ft(k) + ft(u)) / 1e7;
            PROCESS_MEMORY_COUNTERS pmc;
            if (K32GetProcessMemoryInfo(h, &pmc, sizeof pmc)) p.memory = pmc.WorkingSetSize;
            wchar_t path[MAX_PATH * 2];
            DWORD len = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(h, 0, path, &len)) p.command = utf8(path, static_cast<int>(len));
            CloseHandle(h);
        }
        if (p.command.empty()) p.command = p.name;
        out.push_back(std::move(p));
    }
    CloseHandle(snap);
    return out;
}

bool kill(std::int64_t pid, bool, std::string& error) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        error = GetLastError() == ERROR_ACCESS_DENIED ? "not allowed to end that process (try running as administrator)" : "no process " + std::to_string(pid);
        return false;
    }
    const bool ok = TerminateProcess(h, 1) != 0;
    if (!ok) error = "Windows refused to end process " + std::to_string(pid);
    CloseHandle(h);
    return ok;
}

SystemInfo system() {
    SystemInfo s;
    s.os = "Windows";
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtl = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (rtl && rtl(&v) == 0) {
        s.kernel = "NT " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
        s.version = (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) ? "Windows 11" : v.dwMajorVersion == 10 ? "Windows 10" : "Windows " + std::to_string(v.dwMajorVersion);
    }
    std::string product = regString(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    if (!product.empty()) s.version += " " + product;
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    s.arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x86_64" : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86";
    wchar_t name[256];
    DWORD len = 256;
    if (GetComputerNameW(name, &len)) s.hostname = utf8(name);
    len = 256;
    if (GetUserNameW(name, &len)) s.user = utf8(name);
    s.uptime = static_cast<double>(GetTickCount64()) / 1000.0;
    return s;
}

std::vector<NetInfo> network() {
    std::vector<NetInfo> out;
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return out;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& r = table->Table[i];
        if (!r.InterfaceAndOperStatusFlags.HardwareInterface || r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        NetInfo n;
        n.name = utf8(r.Alias);
        n.rx = r.InOctets;
        n.tx = r.OutOctets;
        n.up = r.OperStatus == IfOperStatusUp;
        out.push_back(n);
    }
    FreeMibTable(table);
    return out;
}

BatteryInfo battery() {
    BatteryInfo b;
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps)) return b;
    b.plugged = ps.ACLineStatus == 1;
    if (ps.BatteryFlag == 128 || ps.BatteryLifePercent == 255) return b;  // no battery
    b.present = true;
    b.percent = ps.BatteryLifePercent;
    b.charging = (ps.BatteryFlag & 8) != 0;
    if (ps.BatteryLifeTime != static_cast<DWORD>(-1)) b.minutesLeft = ps.BatteryLifeTime / 60.0;
    return b;
}

}  // namespace wetwaresuite::monitor::platform

#endif
