// The monitor.* commands.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <thread>

#include "wetwaresuite/monitor/monitor.hpp"
#include "wetwaresuite/pdf/page.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/paint.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::monitor {

namespace {

using std::int64_t;
using MonitorPtr = std::shared_ptr<Monitor>;

ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def = {}) { return {name, type, what, false, {}, std::move(def)}; }

ActionFactory reader(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([fn, p] {
            try {
                return fn(p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

std::string bar(double percent, int width = 20) {
    const int n = static_cast<int>(percent / 100.0 * width + 0.5);
    return "[" + std::string(static_cast<std::size_t>(std::clamp(n, 0, width)), '#') + std::string(static_cast<std::size_t>(width - std::clamp(n, 0, width)), '.') + "]";
}

Result cpuResult(Monitor& m) {
    CpuInfo c = m.cpu();
    std::string text = c.model + "\n" + std::to_string(c.logical) + " threads" + (c.physical ? " on " + std::to_string(c.physical) + " cores" : "") +
                       (c.mhz > 0 ? fmt(", %.0f MHz", c.mhz) : "") + "\nin use: " + bar(c.total) + fmt(" %.0f%%", c.total) + "\n";
    for (std::size_t i = 0; i < c.perCore.size(); ++i) text += fmt("  core %-3.0f ", static_cast<double>(i)) + bar(c.perCore[i], 10) + fmt(" %3.0f%%", c.perCore[i]) + "\n";
    if (c.load[0] >= 0) text += fmt("load average %.2f", c.load[0]) + fmt(", %.2f", c.load[1]) + fmt(", %.2f", c.load[2]) + "\n";
    Result r = Result::success(text, c.total);
    r.with("percent", c.total).with("per_core", NumberList(c.perCore.begin(), c.perCore.end())).with("model", c.model);
    r.with("threads", static_cast<int64_t>(c.logical)).with("cores", static_cast<int64_t>(c.physical)).with("mhz", c.mhz);
    if (c.load[0] >= 0) r.with("load", NumberList{c.load[0], c.load[1], c.load[2]});
    return r;
}

Result memoryResult() {
    MemInfo mem = platform::memory();
    const double pct = mem.total ? 100.0 * static_cast<double>(mem.used) / static_cast<double>(mem.total) : 0;
    std::string text = "memory: " + bar(pct) + fmt(" %.0f%%", pct) + "  " + formatBytes(mem.used) + " used of " + formatBytes(mem.total) + ", " +
                       formatBytes(mem.available) + " free\n";
    if (mem.swapTotal) text += "swap: " + formatBytes(mem.swapUsed) + " used of " + formatBytes(mem.swapTotal) + "\n";
    return Result::success(text, pct)
        .with("percent", pct)
        .with("total", static_cast<int64_t>(mem.total))
        .with("used", static_cast<int64_t>(mem.used))
        .with("available", static_cast<int64_t>(mem.available))
        .with("swap_total", static_cast<int64_t>(mem.swapTotal))
        .with("swap_used", static_cast<int64_t>(mem.swapUsed));
}

std::string diskRows(std::string& text) {
    std::string rows;
    for (const auto& d : platform::disks()) {
        const double pct = d.total ? 100.0 * static_cast<double>(d.total - d.free) / static_cast<double>(d.total) : 0;
        text += d.mount + "  " + bar(pct, 12) + fmt(" %.0f%%", pct) + "  " + formatBytes(d.available) + " free of " + formatBytes(d.total) + " (" + d.fs + ")\n";
        rows += d.mount + "\t" + std::to_string(d.total) + "\t" + std::to_string(d.free) + "\t" + std::to_string(d.available) + "\t" + d.fs + "\t" + d.device + "\n";
    }
    return rows;
}

std::vector<ProcInfo> sortedProcesses(Monitor& m, const Params& p) {
    auto list = m.processes();
    const std::string filter = stringParam(p, "filter"), user = stringParam(p, "user"), sort = stringParam(p, "sort", "cpu");
    auto lower = [](std::string s) {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string f = lower(filter);
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const ProcInfo& x) {
                                  if (!user.empty() && x.user != user) return true;
                                  return !f.empty() && lower(x.name).find(f) == std::string::npos && lower(x.command).find(f) == std::string::npos &&
                                         std::to_string(x.pid) != f;
                              }),
               list.end());
    std::sort(list.begin(), list.end(), [&](const ProcInfo& a, const ProcInfo& b) {
        if (sort == "memory") return a.memory > b.memory;
        if (sort == "name") return lower(a.name) < lower(b.name);
        if (sort == "pid") return a.pid < b.pid;
        if (a.cpu != b.cpu) return a.cpu > b.cpu;
        return a.memory > b.memory;
    });
    return list;
}

std::string procRow(const ProcInfo& x) {
    return std::to_string(x.pid) + "\t" + std::to_string(x.ppid) + "\t" + x.name + "\t" + fmt("%.1f", x.cpu) + "\t" + std::to_string(x.memory) + "\t" + std::to_string(x.threads) +
           "\t" + x.user + "\t" + x.state + "\t" + x.command + "\n";
}

Result systemResult() {
    SystemInfo s = platform::system();
    std::string text = s.os + (s.version.empty() ? "" : " (" + s.version + ")") + "\n" + s.kernel + ", " + s.arch + "\ncomputer: " + s.hostname + "\nuser: " + s.user +
                       "\nup for " + formatDuration(s.uptime) + "\n";
    return Result::success(text)
        .with("os", s.os)
        .with("version", s.version)
        .with("kernel", s.kernel)
        .with("arch", s.arch)
        .with("hostname", s.hostname)
        .with("user", s.user)
        .with("uptime", s.uptime);
}

Result batteryResult() {
    BatteryInfo b = platform::battery();
    if (!b.present) return Result::success(b.plugged ? "no battery (plugged in)" : "no battery").with("present", false).with("plugged", b.plugged);
    std::string text = "battery " + fmt("%.0f%%", b.percent) + (b.charging ? ", charging" : b.plugged ? ", plugged in" : ", on battery") +
                       (b.minutesLeft >= 0 ? ", " + formatDuration(b.minutesLeft * 60) + " left" : "");
    return Result::success(text, b.percent).with("present", true).with("percent", b.percent).with("charging", b.charging).with("plugged", b.plugged).with("minutes_left", b.minutesLeft);
}

// a chart of samples over time: processor and memory lines
std::string chartPng(const std::vector<double>& cpu, const std::vector<double>& mem, double interval) {
    const int W = 800, H = 300, L = 48, R = 16, T = 28, B = 32;
    raster::Image img(W, H, {255, 255, 255, 255});
    const pdf::Font font{pdf::Family::Sans, false, false};
    const raster::Color grid{225, 228, 232, 255}, ink{60, 64, 72, 255}, blue{56, 120, 220, 255}, green{40, 160, 90, 255};
    for (int i = 0; i <= 4; ++i) {
        double y = T + (H - T - B) * (1.0 - i / 4.0);
        raster::drawLine(img, L, y, W - R, y, grid, 1);
        std::string label = std::to_string(i * 25) + "%";
        pdf::drawText(img, L - 8 - pdf::textWidthPx(label, font, 12), y + 4, label, font, 12, ink);
    }
    const std::size_t n = std::max(cpu.size(), mem.size());
    auto xAt = [&](std::size_t i) { return L + (n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0) * (W - L - R); };
    auto yAt = [&](double v) { return T + (H - T - B) * (1.0 - std::clamp(v, 0.0, 100.0) / 100.0); };
    auto line = [&](const std::vector<double>& v, raster::Color c) {
        for (std::size_t i = 1; i < v.size(); ++i) raster::drawLine(img, xAt(i - 1), yAt(v[i - 1]), xAt(i), yAt(v[i]), c, 2);
    };
    line(mem, green);
    line(cpu, blue);
    pdf::drawText(img, L, 18, "processor", font, 13, blue);
    pdf::drawText(img, L + 80, 18, "memory", font, 13, green);
    std::string span = "last " + formatDuration(interval * static_cast<double>(n > 0 ? n - 1 : 0));
    pdf::drawText(img, W - R - pdf::textWidthPx(span, font, 12), H - 10, span, font, 12, ink);
    return raster::encodePng(img);
}

void registerReadCommands(Session& s, MonitorPtr mon) {
    auto& reg = s.registry();
    reg.add({"monitor.overview", "Overview",
             "Everything at a glance: processor and memory use, disks, the busiest programs, network speed, battery and uptime.", {}, false, true},
            reader([mon](const Params&) {
                Result cpu = cpuResult(*mon), mem = memoryResult();
                std::string text = cpu.message + mem.message + "\n";
                std::string drows = diskRows(text);
                auto procs = sortedProcesses(*mon, {});
                text += "\nbusiest programs:\n";
                for (std::size_t i = 0; i < procs.size() && i < 5; ++i)
                    text += fmt("  %5.1f%%  ", procs[i].cpu) + formatBytes(procs[i].memory) + "  " + procs[i].name + " (" + std::to_string(procs[i].pid) + ")\n";
                double rx = 0, tx = 0;
                for (const auto& n : mon->network()) {
                    rx += n.rxRate;
                    tx += n.txRate;
                }
                text += "network: down " + formatBytes(static_cast<std::uint64_t>(rx)) + "/s, up " + formatBytes(static_cast<std::uint64_t>(tx)) + "/s\n";
                Result bat = batteryResult();
                if (toText(*bat.field("present")) == "true") text += bat.message + "\n";
                SystemInfo si = platform::system();
                text += si.hostname + ", up for " + formatDuration(si.uptime) + ", " + std::to_string(procs.size()) + (procs.size() == 1 ? " program running\n" : " programs running\n");
                Result r = Result::success(text);
                r.data = cpu.data;
                r.with("memory_percent", *mem.field("percent")).with("memory_used", *mem.field("used")).with("memory_total", *mem.field("total"));
                r.with("disks", drows).with("processes", static_cast<int64_t>(procs.size())).with("net_rx", rx).with("net_tx", tx).with("uptime", si.uptime);
                return r;
            }));

    reg.add({"monitor.cpu", "Processor", "Processor model, cores, and how busy it is in total and per core (and the load average on macOS and Linux).", {}, false, true},
            reader([mon](const Params&) { return cpuResult(*mon); }));

    reg.add({"monitor.memory", "Memory", "Memory in use, free, and swap.", {}, false, true}, reader([](const Params&) { return memoryResult(); }));

    reg.add({"monitor.disks", "Disks", "Every disk and drive: where it is, size, free space and file system.", {}, false, true},
            reader([](const Params&) {
                std::string text;
                std::string rows = diskRows(text);
                return Result::success(text.empty() ? "no disks found" : text).with("rows", rows);
            }));

    reg.add({"monitor.processes", "Programs running",
             "The programs running now: processor use, memory, threads, user and command. Sort by cpu, memory, name or pid; "
             "filter by part of the name.",
             {{"sort", ParamType::String, "order", false, {"cpu", "memory", "name", "pid"}, std::string("cpu")},
              opt("filter", ParamType::String, "only names or commands containing this (or a pid)"), opt("user", ParamType::String, "only this user's"),
              opt("limit", ParamType::Int, "how many to list", int64_t{25})},
             false, true},
            reader([mon](const Params& p) {
                auto list = sortedProcesses(*mon, p);
                const std::size_t limit = static_cast<std::size_t>(std::max<int64_t>(1, intParam(p, "limit", 25)));
                char head[128];
                std::snprintf(head, sizeof head, "%7s %6s %9s %4s  %-12s %s\n", "pid", "cpu%", "memory", "thr", "user", "name");
                std::string text = head, rows;
                for (std::size_t i = 0; i < list.size() && i < limit; ++i) {
                    const auto& x = list[i];
                    char line[160];
                    std::snprintf(line, sizeof line, "%7lld %6.1f %9s %4d  %-12.12s ", static_cast<long long>(x.pid), x.cpu, formatBytes(x.memory).c_str(), x.threads, x.user.c_str());
                    text += line + x.name + "\n";
                    rows += procRow(x);
                }
                return Result::success(text + std::to_string(list.size()) + " in all").with("count", static_cast<int64_t>(list.size())).with("rows", rows);
            }));

    reg.add({"monitor.process", "Program details", "One program by pid: name, command, user, processor and memory use, threads, its parent and the programs it started.",
             {{"pid", ParamType::Int, "process id", true}}, false, true},
            reader([mon](const Params& p) {
                const int64_t pid = intParam(p, "pid", 0);
                auto list = mon->processes();
                auto it = std::find_if(list.begin(), list.end(), [&](const ProcInfo& x) { return x.pid == pid; });
                if (it == list.end()) throw std::runtime_error("no process " + std::to_string(pid));
                std::string kids;
                std::size_t nk = 0;
                for (const auto& x : list) {
                    if (x.ppid == pid && x.pid != pid) {
                        ++nk;
                        kids += "  " + std::to_string(x.pid) + " " + x.name + "\n";
                    }
                }
                std::string parent;
                for (const auto& x : list) {
                    if (x.pid == it->ppid) parent = x.name;
                }
                std::string text = it->name + " (pid " + std::to_string(pid) + ")\ncommand: " + it->command + "\nuser: " + it->user + fmt("\nprocessor: %.1f%%", it->cpu) +
                                   " (" + formatDuration(it->cpuSeconds) + " used so far)\nmemory: " + formatBytes(it->memory) + "\nthreads: " + std::to_string(it->threads) +
                                   "\nparent: " + std::to_string(it->ppid) + (parent.empty() ? "" : " " + parent) + "\n" + (nk ? "started:\n" + kids : "");
                return Result::success(text)
                    .with("name", it->name)
                    .with("command", it->command)
                    .with("cpu", it->cpu)
                    .with("memory", static_cast<int64_t>(it->memory))
                    .with("ppid", it->ppid)
                    .with("children", static_cast<int64_t>(nk));
            }));

    reg.add({"monitor.system", "About this computer", "Operating system and version, kernel, processor architecture, computer name, user and uptime.", {}, false, true},
            reader([](const Params&) { return systemResult(); }));

    reg.add({"monitor.network", "Network", "Each network connection: whether it is up, bytes received and sent, and the speed right now.", {}, false, true},
            reader([mon](const Params&) {
                mon->network();
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::string text, rows;
                for (const auto& n : mon->network()) {
                    text += n.name + (n.up ? "" : " (down)") + ": down " + formatBytes(static_cast<std::uint64_t>(n.rxRate)) + "/s, up " + formatBytes(static_cast<std::uint64_t>(n.txRate)) +
                            "/s; " + formatBytes(n.rx) + " received, " + formatBytes(n.tx) + " sent\n";
                    rows += n.name + "\t" + (n.up ? "up" : "down") + "\t" + std::to_string(n.rx) + "\t" + std::to_string(n.tx) + "\t" + fmt("%.0f", n.rxRate) + "\t" + fmt("%.0f", n.txRate) + "\n";
                }
                return Result::success(text.empty() ? "no network connections" : text).with("rows", rows);
            }));

    reg.add({"monitor.battery", "Battery", "Battery charge, whether it is charging, and time left.", {}, false, true}, reader([](const Params&) { return batteryResult(); }));
}

}  // namespace

void registerCommands(Session& session) {
    auto mon = std::make_shared<Monitor>();
    registerReadCommands(session, mon);
    auto& reg = session.registry();
    reg.add({"monitor.kill", "End program",
             "End a running program by pid (or every program whose name is exactly name=). Asks it to quit; force=true ends "
             "it at once. Cannot be undone.",
             {opt("pid", ParamType::Int, "process id"), opt("name", ParamType::String, "or every process with this exact name"),
              opt("force", ParamType::Bool, "end it at once instead of asking", false)},
             false},
            reader([mon](const Params& p) {
                std::vector<std::int64_t> pids;
                if (p.count("pid")) pids.push_back(intParam(p, "pid", 0));
                const std::string name = stringParam(p, "name");
                if (!name.empty()) {
                    for (const auto& x : platform::processes()) {
                        if (x.name == name) pids.push_back(x.pid);
                    }
                    if (pids.empty()) throw std::runtime_error("no program called " + name + " is running");
                }
                if (pids.empty()) throw std::runtime_error("give pid= or name=");
                std::string errors;
                std::size_t n = 0;
                for (auto pid : pids) {
                    std::string error;
                    if (platform::kill(pid, boolParam(p, "force", false), error)) ++n;
                    else errors += error + "\n";
                }
                if (!n) throw std::runtime_error(errors);
                return Result::success("ended " + std::to_string(n) + " of " + std::to_string(pids.size()) + (errors.empty() ? "" : "\n" + errors)).with("count", static_cast<int64_t>(n));
            }));

    reg.add({"monitor.watch", "Watch",
             "Measure processor and memory use for some seconds and return the samples with a chart (and the average and peak).",
             {opt("seconds", ParamType::Number, "how long to watch", 5.0), opt("interval", ParamType::Number, "seconds between samples", 0.5),
              opt("output", ParamType::String, "also save the chart as this PNG")},
             false},
            reader([mon](const Params& p) {
                const double seconds = std::clamp(numberParam(p, "seconds", 5.0), 0.2, 600.0);
                const double interval = std::clamp(numberParam(p, "interval", 0.5), 0.1, 60.0);
                std::vector<double> cpu, mem;
                mon->cpu();
                // a fixed number of samples: counting by the clock gave fewer on slow machines,
                // where reading the processor takes a good part of each interval
                const int samples = std::max(1, static_cast<int>(std::lround(seconds / interval)));
                for (int i = 0; i < samples; ++i) {
                    std::this_thread::sleep_for(std::chrono::duration<double>(interval));
                    cpu.push_back(mon->cpu().total);
                    MemInfo m = platform::memory();
                    mem.push_back(m.total ? 100.0 * static_cast<double>(m.used) / static_cast<double>(m.total) : 0);
                }
                double avg = 0, peak = 0;
                for (double c : cpu) {
                    avg += c;
                    peak = std::max(peak, c);
                }
                avg /= static_cast<double>(cpu.size());
                std::string png = chartPng(cpu, mem, interval);
                Result r = Result::success(fmt("processor: average %.0f%%", avg) + fmt(", peak %.0f%%", peak) + fmt("; memory %.0f%%", mem.back()) + " (" + std::to_string(cpu.size()) + " samples)");
                r.with("cpu", NumberList(cpu.begin(), cpu.end())).with("memory", NumberList(mem.begin(), mem.end())).with("average", avg).with("peak", peak);
                const std::string out = stringParam(p, "output");
                if (!out.empty()) {
                    sysutil::writeFile(sysutil::toPath(out), png);
                    r.with("path", out);
                }
                r.attach({"image/png", png, "watch.png"});
                return r;
            }));
}

}  // namespace wetwaresuite::monitor
