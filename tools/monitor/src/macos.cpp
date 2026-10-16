// macOS: sysctl, mach host statistics, libproc and getmntinfo.
#if defined(__APPLE__)

#include <ifaddrs.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_var.h>
#include <pwd.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <cstring>
#include <map>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/monitor/monitor.hpp"

namespace wetwaresuite::monitor::platform {

namespace {

std::string sysctlString(const char* name) {
    std::size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string s(len, '\0');
    if (sysctlbyname(name, s.data(), &len, nullptr, 0) != 0) return {};
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

template <typename T>
T sysctlValue(const char* name, T fallback = {}) {
    T v{};
    std::size_t len = sizeof v;
    return sysctlbyname(name, &v, &len, nullptr, 0) == 0 ? v : fallback;
}

std::string userName(uid_t uid) {
    static std::map<uid_t, std::string> cache;
    auto it = cache.find(uid);
    if (it != cache.end()) return it->second;
    struct passwd* pw = getpwuid(uid);
    std::string name = pw ? pw->pw_name : std::to_string(uid);
    cache[uid] = name;
    return name;
}

}  // namespace

std::vector<CpuTimes> cpuTimes() {
    std::vector<CpuTimes> out(1);
    natural_t count = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t infoCount = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &infoCount) != KERN_SUCCESS) return out;
    for (natural_t i = 0; i < count; ++i) {
        const integer_t* t = info + CPU_STATE_MAX * i;
        CpuTimes c;
        c.busy = static_cast<std::uint64_t>(t[CPU_STATE_USER]) + static_cast<std::uint64_t>(t[CPU_STATE_SYSTEM]) + static_cast<std::uint64_t>(t[CPU_STATE_NICE]);
        c.total = c.busy + static_cast<std::uint64_t>(t[CPU_STATE_IDLE]);
        out[0].busy += c.busy;
        out[0].total += c.total;
        out.push_back(c);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(info), infoCount * sizeof(integer_t));
    return out;
}

std::string cpuModel() {
    std::string m = sysctlString("machdep.cpu.brand_string");
    return m.empty() ? sysctlString("hw.model") : m;
}

double cpuMhz() { return static_cast<double>(sysctlValue<std::uint64_t>("hw.cpufrequency", 0)) / 1e6; }

int physicalCores() { return sysctlValue<int>("hw.physicalcpu", 0); }

bool loadAverage(double out[3]) { return getloadavg(out, 3) == 3; }

MemInfo memory() {
    MemInfo r;
    r.total = sysctlValue<std::uint64_t>("hw.memsize", 0);
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
        // what Activity Monitor calls "used" is app + wired + compressed memory
        const std::uint64_t used = (static_cast<std::uint64_t>(vm.internal_page_count) - vm.purgeable_count + vm.wire_count + vm.compressor_page_count) * page;
        r.used = std::min(used, r.total);
        r.available = r.total - r.used;
    }
    struct xsw_usage swap {};
    std::size_t len = sizeof swap;
    if (sysctlbyname("vm.swapusage", &swap, &len, nullptr, 0) == 0) {
        r.swapTotal = swap.xsu_total;
        r.swapUsed = swap.xsu_used;
    }
    return r;
}

std::vector<DiskInfo> disks() {
    std::vector<DiskInfo> out;
    struct statfs* mnts = nullptr;
    int n = getmntinfo(&mnts, MNT_NOWAIT);
    for (int i = 0; i < n; ++i) {
        const struct statfs& m = mnts[i];
        const std::string mount = m.f_mntonname, fs = m.f_fstypename;
        if (fs == "devfs" || fs == "autofs" || fs == "nullfs") continue;
        if (mount != "/" && (m.f_flags & MNT_DONTBROWSE) && mount != "/System/Volumes/Data") continue;
        if (m.f_blocks == 0) continue;
        DiskInfo d;
        d.mount = mount;
        d.device = m.f_mntfromname;
        d.fs = fs;
        d.total = static_cast<std::uint64_t>(m.f_blocks) * m.f_bsize;
        d.free = static_cast<std::uint64_t>(m.f_bfree) * m.f_bsize;
        d.available = static_cast<std::uint64_t>(m.f_bavail) * m.f_bsize;
        out.push_back(d);
    }
    return out;
}

std::vector<ProcInfo> processes() {
    std::vector<ProcInfo> out;
    int n = proc_listallpids(nullptr, 0);
    if (n <= 0) return out;
    std::vector<pid_t> pids(static_cast<std::size_t>(n) + 64);
    n = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    const double toSeconds = tb.denom ? static_cast<double>(tb.numer) / tb.denom / 1e9 : 1e-9;
    for (int i = 0; i < n; ++i) {
        const pid_t pid = pids[static_cast<std::size_t>(i)];
        if (pid <= 0) continue;
        ProcInfo p;
        p.pid = pid;
        struct proc_taskallinfo info {};
        const int got = proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &info, sizeof info);
        if (got == static_cast<int>(sizeof info)) {
            p.ppid = info.pbsd.pbi_ppid;
            p.name = info.pbsd.pbi_name[0] ? info.pbsd.pbi_name : info.pbsd.pbi_comm;
            p.user = userName(info.pbsd.pbi_uid);
            p.cpuSeconds = static_cast<double>(info.ptinfo.pti_total_user + info.ptinfo.pti_total_system) * toSeconds;
            p.memory = info.ptinfo.pti_resident_size;
            p.threads = info.ptinfo.pti_threadnum;
            p.state = info.pbsd.pbi_status == 5 /* SZOMB */ ? "Z" : info.ptinfo.pti_numrunning > 0 ? "R" : "S";
        } else {
            struct proc_bsdshortinfo sh {};
            if (proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &sh, sizeof sh) != static_cast<int>(sizeof sh)) continue;
            p.ppid = sh.pbsi_ppid;
            p.name = sh.pbsi_comm;
            p.user = userName(sh.pbsi_uid);
        }
        char path[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_pidpath(pid, path, sizeof path) > 0) p.command = path;
        if (p.command.empty()) p.command = p.name;
        out.push_back(std::move(p));
    }
    return out;
}

bool kill(std::int64_t pid, bool force, std::string& error) {
    if (::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) == 0) return true;
    error = errno == EPERM ? "not allowed to end that process (it belongs to another user)" : errno == ESRCH ? "no process " + std::to_string(pid) : std::strerror(errno);
    return false;
}

SystemInfo system() {
    SystemInfo s;
    struct utsname u {};
    if (uname(&u) == 0) {
        s.kernel = std::string(u.sysname) + " " + u.release;
        s.arch = u.machine;
        s.hostname = u.nodename;
    }
    s.os = "macOS";
    s.version = sysctlString("kern.osproductversion");
    struct timeval boot {};
    std::size_t len = sizeof boot;
    if (sysctlbyname("kern.boottime", &boot, &len, nullptr, 0) == 0) s.uptime = static_cast<double>(time(nullptr) - boot.tv_sec);
    s.user = userName(getuid());
    return s;
}

std::vector<NetInfo> network() {
    std::vector<NetInfo> out;
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (struct ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || !a->ifa_data) continue;
        const auto* d = static_cast<const struct if_data*>(a->ifa_data);
        NetInfo n;
        n.name = a->ifa_name;
        n.rx = d->ifi_ibytes;
        n.tx = d->ifi_obytes;
        n.up = (a->ifa_flags & IFF_UP) && (a->ifa_flags & IFF_RUNNING);
        out.push_back(n);
    }
    freeifaddrs(list);
    return out;
}

BatteryInfo battery() {
    BatteryInfo b;
    ProcessResult r = runProgram({"pmset", "-g", "batt"});
    if (r.exitCode != 0) return b;
    b.plugged = r.output.find("AC Power") != std::string::npos;
    const std::size_t pct = r.output.find('%');
    if (pct == std::string::npos) return b;
    std::size_t start = pct;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(r.output[start - 1]))) --start;
    b.present = true;
    b.percent = std::atof(r.output.substr(start, pct - start).c_str());
    b.charging = r.output.find("; charging") != std::string::npos;
    const std::size_t rem = r.output.find(" remaining");
    if (rem != std::string::npos) {
        std::size_t c = r.output.rfind(':', rem);
        std::size_t h = c;
        while (h > 0 && std::isdigit(static_cast<unsigned char>(r.output[h - 1]))) --h;
        if (c != std::string::npos && c > h) b.minutesLeft = std::atof(r.output.substr(h, c - h).c_str()) * 60 + std::atof(r.output.substr(c + 1, 2).c_str());
    }
    return b;
}

}  // namespace wetwaresuite::monitor::platform

#endif
