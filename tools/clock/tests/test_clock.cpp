// clock: calendar arithmetic, time zones and daylight saving, parsing, timers,
// the stopwatch, saved alarms, date maths and MCP.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "wetwaresuite/clock/clock.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

using namespace wetwaresuite;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string field(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}

std::int64_t utc(int y, int mo, int d, int h, int mi) { return clock::unixFromCivil({y, mo, d, h, mi, 0}); }

int offsetIn(const std::string& place, std::int64_t t) { return clock::offsetAt(clock::findPlace(place), t); }

double dur(const std::string& s) { return clock::parseDuration(s); }

}  // namespace

int main() {
    // ---- calendar arithmetic ----
    {
        CHECK(clock::daysFromCivil(1970, 1, 1) == 0);
        CHECK(clock::daysFromCivil(2000, 3, 1) == 11017);
        CHECK(clock::daysFromCivil(1969, 12, 31) == -1);
        bool roundTrip = true;
        for (std::int64_t d = -800000; d < 800000; d += 997) {
            auto c = clock::civilFromDays(d);
            if (clock::daysFromCivil(c.year, c.month, c.day) != d) roundTrip = false;
        }
        CHECK(roundTrip);
        CHECK(clock::weekdayOf(clock::daysFromCivil(2026, 10, 16)) == 5);  // a Friday
        CHECK(clock::weekdayOf(-1) == 3);                                   // 1969-12-31 was a Wednesday
        CHECK(clock::isoWeek(2026, 1, 1) == 1);
        CHECK(clock::isoWeek(2021, 1, 1) == 53);
        CHECK(clock::daysInMonth(2024, 2) == 29 && clock::daysInMonth(2100, 2) == 28 && clock::daysInMonth(2000, 2) == 29);
        auto c = clock::civilFromUnix(1792000000);
        CHECK(c.year == 2026 && c.month == 10 && c.day == 14 && c.hour == 17 && c.minute == 46 && c.second == 40);
        CHECK(clock::unixFromCivil(c) == 1792000000);
    }

    // ---- time zones and summer time ----
    {
        CHECK(offsetIn("New York", utc(2026, 1, 15, 12, 0)) == -300);
        CHECK(offsetIn("New York", utc(2026, 7, 1, 12, 0)) == -240);
        CHECK(offsetIn("New York", utc(2026, 3, 8, 6, 59)) == -300);  // clocks go forward at 2:00 local = 7:00 UTC
        CHECK(offsetIn("New York", utc(2026, 3, 8, 7, 0)) == -240);
        CHECK(offsetIn("New York", utc(2026, 11, 1, 5, 59)) == -240);  // back at 2:00 summer time = 6:00 UTC
        CHECK(offsetIn("New York", utc(2026, 11, 1, 6, 0)) == -300);
        CHECK(offsetIn("London", utc(2026, 3, 29, 0, 59)) == 0);
        CHECK(offsetIn("London", utc(2026, 3, 29, 1, 0)) == 60);
        CHECK(offsetIn("Paris", utc(2026, 10, 25, 0, 59)) == 120 && offsetIn("Paris", utc(2026, 10, 25, 1, 0)) == 60);
        CHECK(offsetIn("Sydney", utc(2026, 1, 1, 0, 0)) == 660 && offsetIn("Sydney", utc(2026, 7, 1, 0, 0)) == 600);
        CHECK(offsetIn("Auckland", utc(2026, 1, 1, 0, 0)) == 780 && offsetIn("Auckland", utc(2026, 6, 1, 0, 0)) == 720);
        CHECK(offsetIn("Tokyo", utc(2026, 7, 1, 0, 0)) == 540);
        CHECK(offsetIn("Kathmandu", utc(2026, 7, 1, 0, 0)) == 345);
        CHECK(offsetIn("Europe/Berlin", utc(2026, 7, 1, 0, 0)) == 120);
        CHECK(offsetIn("UTC+5:30", 0) == 330 && offsetIn("GMT-3", 0) == -180 && offsetIn("+0800", 0) == 480);
        CHECK(offsetIn("EST", utc(2026, 7, 1, 0, 0)) == -300);
        CHECK(offsetIn("germany", 0) == 60);  // a country with one city in the table
        bool threw = false;
        try {
            clock::findPlace("Atlantis");
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        threw = false;
        try {
            clock::findPlace("japan");  // Tokyo or Osaka
        } catch (const std::exception& e) {
            threw = std::string(e.what()).find("Tokyo") != std::string::npos;
        }
        CHECK(threw);
        clock::Place ny = clock::findPlace("new_york");
        CHECK(clock::toUtc({2026, 7, 4, 12, 0, 0}, ny) == utc(2026, 7, 4, 16, 0));
        CHECK(clock::toUtc({2026, 1, 4, 12, 0, 0}, ny) == utc(2026, 1, 4, 17, 0));
        CHECK(clock::offsetText(-210) == "-03:30");
        CHECK(clock::zones().size() > 100);
    }

    // ---- parsing ----
    {
        clock::Civil c;
        bool d = false, t = false;
        CHECK(clock::parseDateTime("2026-10-16 14:30", c, d, t) && d && t && c.month == 10 && c.hour == 14 && c.minute == 30);
        CHECK(clock::parseDateTime("2026-10-16T08:05:09Z", c, d, t) && c.second == 9);
        CHECK(clock::parseDateTime("7:30pm", c, d, t) && !d && t && c.hour == 19 && c.minute == 30);
        CHECK(clock::parseDateTime("12am", c, d, t) && c.hour == 0);
        CHECK(clock::parseDateTime("16 Oct 2026", c, d, t) && d && !t && c.day == 16 && c.month == 10 && c.year == 2026);
        CHECK(clock::parseDateTime("Oct 16, 2026 9:15", c, d, t) && d && t && c.hour == 9);
        CHECK(clock::parseDateTime("tomorrow 8:00", c, d, t) && d && t && c.hour == 8);
        CHECK(!clock::parseDateTime("2026-02-30", c, d, t));
        CHECK(!clock::parseDateTime("25:00", c, d, t));
        CHECK(!clock::parseDateTime("banana", c, d, t));
        CHECK(dur("90") == 90 && dur("5m") == 300 && dur("1h30m") == 5400 && dur("1:30:00") == 5400 && dur("4:30") == 270);
        CHECK(dur("2 hours 15 minutes") == 8100 && dur("1.5h") == 5400 && dur("250ms") == 0.25);
        CHECK(clock::formatDuration(3909) == "1:05:09" && clock::formatDuration(243.25, true) == "4:03.2" && clock::formatDuration(59.2) == "1:00");
        std::string cal = clock::calendarText(2026, 10, 16);
        CHECK(cal.find("October 2026") != std::string::npos && cal.find("[16]") != std::string::npos && cal.find("  40") != std::string::npos);
    }

    const fs::path home = fs::temp_directory_path() / ("ww-clock-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
#ifdef _WIN32
    _putenv_s("WETWARESUITE_HOME", sysutil::fromPath(home).c_str());
#else
    setenv("WETWARESUITE_HOME", sysutil::fromPath(home).c_str(), 1);
#endif

    Session s;
    clock::registerCommands(s);

    // ---- now, world, convert ----
    {
        Result now = s.run("clock.now", {{"place", std::string("UTC")}});
        CHECK(now.ok && field(now, "iso").find("+00:00") != std::string::npos && field(now, "offset_minutes") == "0");
        CHECK(s.run("clock.now", {}).ok);
        Result w = s.run("clock.world", {{"places", std::string("Tokyo|London")}, {"at", std::string("2026-07-01 12:00")}});
        CHECK(w.ok && w.message.find("Tokyo") != std::string::npos);
        Result cv = s.run("clock.convert", {{"time", std::string("2026-07-01 09:00")}, {"from", std::string("New York")}, {"to", std::string("London|Tokyo")}});
        CHECK(cv.ok && field(cv, "rows").find("2026-07-01T14:00:00+01:00") != std::string::npos && field(cv, "rows").find("2026-07-01T22:00:00+09:00") != std::string::npos);
        Result z = s.run("clock.zones", {{"search", std::string("australia")}});
        CHECK(z.ok && std::get<std::int64_t>(*z.field("count")) >= 6);
        CHECK(!s.run("clock.convert", {{"time", std::string("9:00")}, {"from", std::string("Nowhere")}}).ok);
    }

    // ---- timers and stopwatch ----
    {
        Result t = s.run("clock.timer_start", {{"duration", std::string("0.3s")}, {"label", std::string("tea")}});
        CHECK(t.ok && field(t, "timer") == "t1");
        CHECK(field(s.run("clock.due", {}), "count") == "0");
        CHECK(s.run("clock.timer_start", {{"duration", std::string("10m")}}).ok);
        CHECK(s.run("clock.timer_pause", {{"timer", std::string("t2")}}).ok);
        CHECK(s.run("clock.timers", {}).message.find("paused") != std::string::npos);
        CHECK(s.run("clock.timer_resume", {{"timer", std::string("t2")}}).ok);
        CHECK(s.run("clock.wait", {{"timer", std::string("tea")}}).ok);
        Result due = s.run("clock.due", {});
        CHECK(due.ok && field(due, "count") == "1" && due.message.find("tea") != std::string::npos);
        CHECK(field(s.run("clock.due", {}), "count") == "0");  // told once
        CHECK(s.run("clock.timer_cancel", {{"timer", std::string("t2")}}).ok);
        CHECK(field(s.run("clock.timers", {}), "count") == "1");
        CHECK(s.history().undo());
        CHECK(field(s.run("clock.timers", {}), "count") == "2");
        CHECK(!s.run("clock.timer_start", {{"duration", std::string("soon")}}).ok);

        CHECK(s.run("clock.stopwatch", {{"action", std::string("start")}}).ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK(s.run("clock.stopwatch", {{"action", std::string("lap")}}).ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Result sw = s.run("clock.stopwatch", {{"action", std::string("stop")}});
        CHECK(sw.ok && std::get<double>(*sw.field("seconds")) >= 0.17 && std::get<NumberList>(*sw.field("laps")).size() == 1);
        CHECK(field(s.run("clock.stopwatch", {{"action", std::string("reset")}}), "seconds") == "0");
    }

    // ---- alarms, saved between sessions ----
    {
        Result a = s.run("clock.alarm_add", {{"time", std::string("07:30")}, {"days", std::string("weekdays")}, {"label", std::string("work")}});
        CHECK(a.ok && field(a, "alarm") == "a1" && std::get<std::int64_t>(*a.field("next")) > 0);
        CHECK(s.run("clock.alarm_add", {{"time", std::string("6:15pm")}, {"days", std::string("mon,wed")}}).ok);
        CHECK(s.run("clock.alarm_add", {{"time", std::string("9:00")}, {"days", std::string("once")}}).ok);
        CHECK(!s.run("clock.alarm_add", {{"time", std::string("9:00")}, {"days", std::string("someday")}}).ok);
        CHECK(fs::exists(home / "clock" / "alarms.tsv"));
        Result list = s.run("clock.alarms", {});
        CHECK(list.ok && field(list, "count") == "3" && list.message.find("18:15") != std::string::npos);
        CHECK(s.run("clock.alarm_remove", {{"alarm", std::string("a2")}}).ok);
        CHECK(field(s.run("clock.alarms", {}), "count") == "2");
        CHECK(s.history().undo());
        CHECK(field(s.run("clock.alarms", {}), "count") == "3");
        CHECK(s.run("clock.alarm_toggle", {{"alarm", std::string("a1")}, {"on", false}}).ok);

        Session again;
        clock::registerCommands(again);
        Result l2 = again.run("clock.alarms", {});
        CHECK(field(l2, "count") == "3" && l2.message.find("(off)") != std::string::npos);

        // an alarm set long ago rings once, then not again
        sysutil::writeFile(home / "clock" / "alarms.tsv", "a9\t3\t0\tdaily\ton\t0\t0\tlong ago\n");
        Session third;
        clock::registerCommands(third);
        Result due = third.run("clock.due", {});
        CHECK(due.ok && field(due, "count") == "1" && due.message.find("long ago") != std::string::npos);
        CHECK(field(third.run("clock.due", {}), "count") == "0");
    }

    // ---- date maths ----
    {
        Result d = s.run("clock.date_diff", {{"from", std::string("2026-01-01")}, {"to", std::string("2026-12-25")}});
        CHECK(d.ok && field(d, "days") == "358" && field(d, "working_days") == "256" && field(d, "months") == "11");
        CHECK(field(s.run("clock.date_diff", {{"from", std::string("2026-03-10")}, {"to", std::string("2026-03-01")}}), "days") == "-9");
        CHECK(field(s.run("clock.date_add", {{"date", std::string("2026-01-31")}, {"add", std::string("1 month")}}), "date") == "2026-02-28");
        CHECK(field(s.run("clock.date_add", {{"date", std::string("2026-10-16")}, {"add", std::string("5 working days")}}), "date") == "2026-10-23");
        CHECK(field(s.run("clock.date_add", {{"date", std::string("2026-10-16")}, {"add", std::string("-10 days")}}), "date") == "2026-10-06");
        CHECK(field(s.run("clock.date_add", {{"date", std::string("2024-02-29")}, {"add", std::string("1 year")}}), "date") == "2025-02-28");
        CHECK(field(s.run("clock.date_add", {{"date", std::string("2026-10-16")}, {"add", std::string("2 weeks and 3 days")}}), "date") == "2026-11-02");
        CHECK(field(s.run("clock.date_add", {{"date", std::string("2026-10-16")}, {"add", std::string("36 hours")}}), "date") == "2026-10-17 12:00");
        CHECK(!s.run("clock.date_add", {{"add", std::string("3 fortnights")}}).ok);
        Result cal = s.run("clock.calendar", {{"year", std::int64_t{2026}}, {"month", std::int64_t{2}}});
        CHECK(cal.ok && cal.message.find("February 2026") != std::string::npos && field(cal, "days") == "28");
        CHECK(field(s.run("clock.calendar", {{"year", std::int64_t{2024}}}), "days") == "366");
    }

    {
        mcp::Server server(s, {"wetwaresuite", "test", "clock"});
        json list = json::parse(*server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"))["result"]["tools"];
        int seen = 0;
        for (const auto& t : list) {
            if (std::string(t["name"]).rfind("clock_", 0) == 0) ++seen;
        }
        CHECK(seen == 19);
        json call = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                     {"params", {{"name", "clock_date_diff"}, {"arguments", {{"from", "2026-01-01"}, {"to", "2026-01-31"}}}}}};
        json res = json::parse(*server.handle(call.dump()))["result"];
        CHECK(res["isError"] == false && res["structuredContent"]["days"] == 30);
    }
    std::error_code ec;
    fs::remove_all(home, ec);
    return check::summary("clock");
}
