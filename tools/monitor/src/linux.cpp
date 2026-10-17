// Linux (and other /proc systems): everything comes from /proc and /sys.
#if defined(__linux__)

#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "wetwaresuite/monitor/monitor.hpp"

namespace wetwaresuite::monitor::platform {

namespace {

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t a = s.find_first_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a);
}

// "Key:   123 kB" -> 123 * 1024
std::uint64_t kbField(const std::string& text, const std::string& key) {
    std::size_t at = text.find("\n" + key + ":");
    if (at == std::string::npos) {
        if (text.rfind(key + ":", 0) != 0) return 0;
        at = 0;
    } else {
        ++at;
    }
    return std::strtoull(text.c_str() + at + key.size() + 1, nullptr, 10) * 1024;
}

std::string userName(unsigned uid) {
    static std::map<unsigned, std::string> cache;
    auto it = cache.find(uid);
    if (it != cache.end()) return it->second;
    struct passwd pw {};
    struct passwd* res = nullptr;
    char buf[1024];
    std::string name = getpwuid_r(uid, &pw, buf, sizeof buf, &res) == 0 && res ? res->pw_name : std::to_string(uid);
    cache[uid] = name;
    return name;
}

}  // namespace

std::vector<CpuTimes> cpuTimes() {
    std::vector<CpuTimes> out;
    std::istringstream in(slurp("/proc/stat"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        std::istringstream ls(line);
        std::string name;
        ls >> name;
        std::uint64_t v[10] = {}, total = 0;
        for (int i = 0; i < 10 && (ls >> v[i]); ++i) {
            if (i < 8) total += v[i];  // guest time is already in user
        }
        out.push_back({total - v[3] - v[4], total});
    }
    return out;
}

std::string cpuModel() {
    std::istringstream in(slurp("/proc/cpuinfo"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0 || line.rfind("Hardware", 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos) return trim(line.substr(c + 1));
        }
    }
    return {};
}

double cpuMhz() {
    std::string cur = slurp("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (!cur.empty()) return std::atof(cur.c_str()) / 1000.0;
    std::istringstream in(slurp("/proc/cpuinfo"));
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu MHz", 0) == 0) return std::atof(line.substr(line.find(':') + 1).c_str());
    }
    return 0;
}

int physicalCores() {
    std::istringstream in(slurp("/proc/cpuinfo"));
    std::string line, phys;
    std::set<std::string> cores;
    while (std::getline(in, line)) {
        if (line.rfind("physical id", 0) == 0) phys = trim(line.substr(line.find(':') + 1));
        if (line.rfind("core id", 0) == 0) cores.insert(phys + "/" + trim(line.substr(line.find(':') + 1)));
    }
    if (!cores.empty()) return static_cast<int>(cores.size());
    return static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
}

bool loadAverage(double out[3]) {
    std::istringstream in(slurp("/proc/loadavg"));
    return static_cast<bool>(in >> out[0] >> out[1] >> out[2]);
}

MemInfo memory() {
    const std::string m = "\n" + slurp("/proc/meminfo");
    MemInfo r;
    r.total = kbField(m, "MemTotal");
    r.available = kbField(m, "MemAvailable");
    if (!r.available) r.available = kbField(m, "MemFree") + kbField(m, "Buffers") + kbField(m, "Cached");
    r.used = r.total > r.available ? r.total - r.available : 0;
    r.swapTotal = kbField(m, "SwapTotal");
    const std::uint64_t swapFree = kbField(m, "SwapFree");
    r.swapUsed = r.swapTotal > swapFree ? r.swapTotal - swapFree : 0;
    return r;
}

std::vector<DiskInfo> disks() {
    static const std::set<std::string> real = {"ext2", "ext3", "ext4", "xfs", "btrfs", "vfat", "exfat", "ntfs", "ntfs3", "fuseblk", "zfs",
                                               "f2fs", "jfs", "reiserfs", "hfsplus", "apfs", "overlay", "9p", "virtiofs", "nfs", "nfs4", "cifs", "smb3"};
    std::vector<DiskInfo> out;
    std::set<std::string> seen;
    std::istringstream in(slurp("/proc/mounts"));
    std::string dev, mnt, fs, rest;
    while (in >> dev >> mnt >> fs && std::getline(in, rest)) {
        if (!real.count(fs) && mnt != "/") continue;
        if (mnt.rfind("/snap/", 0) == 0 || mnt.rfind("/proc", 0) == 0 || mnt.rfind("/sys", 0) == 0) continue;
        // octal escapes for spaces in mount points
        std::string m;
        for (std::size_t i = 0; i < mnt.size(); ++i) {
            if (mnt[i] == '\\' && i + 3 < mnt.size()) {
                m += static_cast<char>(std::strtol(mnt.substr(i + 1, 3).c_str(), nullptr, 8));
                i += 3;
            } else {
                m += mnt[i];
            }
        }
        struct statvfs st {};
        if (statvfs(m.c_str(), &st) != 0 || st.f_blocks == 0) continue;
        const std::string key = dev + "|" + std::to_string(st.f_blocks);
        if (dev != "overlay" && !seen.insert(key).second) continue;  // bind mounts of the same disk
        DiskInfo d;
        d.mount = m;
        d.device = dev;
        d.fs = fs;
        d.total = static_cast<std::uint64_t>(st.f_blocks) * st.f_frsize;
        d.free = static_cast<std::uint64_t>(st.f_bfree) * st.f_frsize;
        d.available = static_cast<std::uint64_t>(st.f_bavail) * st.f_frsize;
        out.push_back(d);
    }
    return out;
}

std::vector<ProcInfo> processes() {
    std::vector<ProcInfo> out;
    const double tick = static_cast<double>(sysconf(_SC_CLK_TCK));
    const std::uint64_t page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    DIR* d = opendir("/proc");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const std::string base = std::string("/proc/") + e->d_name;
        const std::string stat = slurp(base + "/stat");
        const std::size_t open = stat.find('('), close = stat.rfind(')');
        if (open == std::string::npos || close == std::string::npos) continue;
        ProcInfo p;
        p.pid = std::atoll(e->d_name);
        p.name = stat.substr(open + 1, close - open - 1);
        std::istringstream fields(stat.substr(close + 2));
        std::vector<std::string> f;
        std::string x;
        while (fields >> x) f.push_back(x);
        if (f.size() < 22) continue;
        p.state = f[0];
        p.ppid = std::atoll(f[1].c_str());
        p.cpuSeconds = (std::atof(f[11].c_str()) + std::atof(f[12].c_str())) / tick;
        p.threads = std::atoi(f[17].c_str());
        p.memory = std::strtoull(f[21].c_str(), nullptr, 10) * page;
        const std::string status = slurp(base + "/status");
        const std::size_t uid = status.find("\nUid:");
        if (uid != std::string::npos) p.user = userName(static_cast<unsigned>(std::strtoul(status.c_str() + uid + 5, nullptr, 10)));
        std::string cmd = slurp(base + "/cmdline");
        for (auto& c : cmd) {
            if (c == '\0') c = ' ';
        }
        p.command = trim(cmd);
        if (p.command.empty()) p.command = "[" + p.name + "]";
        out.push_back(std::move(p));
    }
    closedir(d);
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
    s.os = "Linux";
    std::istringstream os(slurp("/etc/os-release"));
    std::string line;
    while (std::getline(os, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            s.version = line.substr(12);
            if (s.version.size() >= 2 && s.version.front() == '"') s.version = s.version.substr(1, s.version.size() - 2);
        }
    }
    s.uptime = std::atof(slurp("/proc/uptime").c_str());
    s.user = userName(getuid());
    return s;
}

std::vector<NetInfo> network() {
    std::vector<NetInfo> out;
    std::istringstream in(slurp("/proc/net/dev"));
    std::string line;
    while (std::getline(in, line)) {
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        NetInfo n;
        n.name = trim(line.substr(0, c));
        std::istringstream v(line.substr(c + 1));
        std::uint64_t f[16] = {};
        for (auto& x : f) v >> x;
        n.rx = f[0];
        n.tx = f[8];
        const std::string state = trim(slurp("/sys/class/net/" + n.name + "/operstate"));
        n.up = state == "up" || state == "unknown";
        out.push_back(n);
    }
    return out;
}

BatteryInfo battery() {
    BatteryInfo b;
    DIR* d = opendir("/sys/class/power_supply");
    if (!d) return b;
    double sum = 0;
    int n = 0;
    while (dirent* e = readdir(d)) {
        const std::string base = std::string("/sys/class/power_supply/") + e->d_name;
        const std::string type = trim(slurp(base + "/type"));
        if (type == "Mains" && trim(slurp(base + "/online")) == "1") b.plugged = true;
        if (type != "Battery") continue;
        const std::string cap = trim(slurp(base + "/capacity"));
        if (cap.empty()) continue;
        b.present = true;
        sum += std::atof(cap.c_str());
        ++n;
        const std::string status = trim(slurp(base + "/status"));
        if (status == "Charging") b.charging = true;
        if (status == "Charging" || status == "Full") b.plugged = true;
        const double power = std::atof(slurp(base + "/power_now").c_str()), energy = std::atof(slurp(base + "/energy_now").c_str());
        if (status == "Discharging" && power > 0) b.minutesLeft = energy / power * 60.0;
    }
    closedir(d);
    if (n) b.percent = sum / n;
    return b;
}

}  // namespace wetwaresuite::monitor::platform

#endif
