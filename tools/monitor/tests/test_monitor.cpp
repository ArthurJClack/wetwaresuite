// monitor: the numbers the operating system gives are sane, our own process is
// in the list, a child can be ended, the chart renders, and MCP lists the tools.
#include <atomic>
#include <thread>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/monitor/monitor.hpp"

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
static long long selfPid() { return static_cast<long long>(GetCurrentProcessId()); }
#else
#include <unistd.h>
static long long selfPid() { return static_cast<long long>(getpid()); }
#endif

using namespace wetwaresuite;
using nlohmann::json;

namespace {

std::string field(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

}  // namespace

int main() {
    CHECK(monitor::formatBytes(512) == "512 B");
    CHECK(monitor::formatBytes(1536) == "1.5 KB");
    CHECK(monitor::formatBytes(5ull << 30) == "5.0 GB");
    CHECK(monitor::formatDuration(42) == "42 s");
    CHECK(monitor::formatDuration(3 * 3600 + 5 * 60) == "3 h 05 min");
    CHECK(monitor::formatDuration(2 * 86400 + 7200) == "2 days 2 h");

    {
        auto times = monitor::platform::cpuTimes();
        CHECK(!times.empty() && times[0].total > 0 && times[0].busy <= times[0].total);
        monitor::MemInfo m = monitor::platform::memory();
        CHECK(m.total > (64ull << 20) && m.used <= m.total && m.available <= m.total);
        auto disks = monitor::platform::disks();
        CHECK(!disks.empty());
        for (const auto& d : disks) CHECK(d.total > 0 && d.free <= d.total);
        monitor::SystemInfo si = monitor::platform::system();
        CHECK(!si.os.empty() && !si.arch.empty() && si.uptime > 0);
        monitor::platform::battery();  // may have none; must not crash
    }

    Session s;
    monitor::registerCommands(s);
    Result cpu = s.run("monitor.cpu", {});
    CHECK(cpu.ok && std::get<double>(cpu.value) >= 0 && std::get<double>(cpu.value) <= 100);
    CHECK(!std::get<NumberList>(*cpu.field("per_core")).empty());
    Result mem = s.run("monitor.memory", {});
    CHECK(mem.ok && field(mem, "total") != "0");
    CHECK(s.run("monitor.disks", {}).ok);
    CHECK(s.run("monitor.system", {}).ok);
    CHECK(s.run("monitor.network", {}).ok);
    CHECK(s.run("monitor.battery", {}).ok);
    Result ov = s.run("monitor.overview", {});
    CHECK(ov.ok && ov.message.find("memory") != std::string::npos && ov.message.find("busiest programs") != std::string::npos);

    // our own process, found by pid
    {
        std::atomic<bool> stop{false};
        std::thread busy([&] {
            volatile double x = 0;
            while (!stop) x = x + 1;
        });
        Result procs = s.run("monitor.processes", {{"filter", std::to_string(selfPid())}});
        stop = true;
        busy.join();
        CHECK(procs.ok && field(procs, "count") == "1" && field(procs, "rows").find("test_monitor") != std::string::npos);
        Result me = s.run("monitor.process", {{"pid", static_cast<std::int64_t>(selfPid())}});
        CHECK(me.ok && field(me, "name").find("test_monitor") != std::string::npos && std::get<std::int64_t>(*me.field("memory")) > 0);
        CHECK(!s.run("monitor.process", {{"pid", std::int64_t{999999999}}}).ok);
        Result byMem = s.run("monitor.processes", {{"sort", std::string("memory")}, {"limit", std::int64_t{3}}});
        CHECK(byMem.ok && std::get<std::int64_t>(*byMem.field("count")) > 3);
    }

    // end a program we started
    {
        ChildProcess child;
        std::string error;
#ifdef _WIN32
        bool started = child.start({"ping", "-n", "60", "127.0.0.1"}, ".", error);
#else
        bool started = child.start({"sleep", "60"}, ".", error);
#endif
        CHECK(started);
        Result k = s.run("monitor.kill", {{"pid", static_cast<std::int64_t>(child.pid())}});
        CHECK(k.ok);
        std::string out;
        CHECK(child.wait(5, out));
        CHECK(!s.run("monitor.kill", {}).ok);
        CHECK(!s.run("monitor.kill", {{"name", std::string("no-such-program-here")}}).ok);
    }

    Result w = s.run("monitor.watch", {{"seconds", 0.6}, {"interval", 0.2}});
    CHECK(w.ok && std::get<NumberList>(*w.field("cpu")).size() >= 3 && w.attachments.size() == 1 && w.attachments[0].bytes.size() > 1000);

    {
        mcp::Server server(s, {"wetwaresuite", "test", "monitor"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            if (std::string(t["name"]).rfind("monitor_", 0) == 0) ++seen;
        }
        CHECK(seen == 11);
        json call = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", {{"name", "monitor_memory"}, {"arguments", json::object()}}}};
        json res = json::parse(*server.handle(call.dump()))["result"];
        CHECK(res["isError"] == false && res["structuredContent"]["total"].get<long long>() > 0);
    }
    return check::summary("monitor");
}
