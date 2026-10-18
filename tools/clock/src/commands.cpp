// The clock.* commands. Timers and the stopwatch live in the session; alarms
// are saved in the suite's data folder (clock/alarms.tsv) so they outlast it.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <thread>

#include "wetwaresuite/clock/clock.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::clock {

namespace {

using std::int64_t;
using Steady = std::chrono::steady_clock;

struct Timer {
    std::string id, label;
    double duration = 0;
    Steady::time_point endsAt{};
    double left = 0;  // while paused
    bool paused = false, notified = false;
    double remaining() const {
        if (paused) return left;
        return std::max(0.0, std::chrono::duration<double>(endsAt - Steady::now()).count());
    }
    bool done() const { return !paused && remaining() <= 0; }
};

struct Alarm {
    std::string id;
    int hour = 7, minute = 0;
    std::string days = "daily";  // daily, weekdays, weekends, mon,wed,... or a date 2026-12-24
    std::string label;
    bool on = true;
    int64_t created = 0, lastRung = 0;
};

struct State {
    std::vector<Timer> timers;
    int timerCounter = 0;
    bool swRunning = false;
    Steady::time_point swStart{};
    double swBanked = 0;
    std::vector<double> laps;  // time of each lap mark since the start
    std::vector<Alarm> alarms;
    int alarmCounter = 0;
    bool loaded = false;
    double stopwatch() const { return swBanked + (swRunning ? std::chrono::duration<double>(Steady::now() - swStart).count() : 0.0); }
};
using StatePtr = std::shared_ptr<State>;

ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def = {}) { return {name, type, what, false, {}, std::move(def)}; }
ParamSpec req(const std::string& name, ParamType type, const std::string& what) { return {name, type, what, true, {}, {}}; }

int64_t nowUnix() { return static_cast<int64_t>(std::time(nullptr)); }

const char* kWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

std::string two(int v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02d", v);
    return b;
}

std::string isoText(const Civil& c, int offset) {
    return std::to_string(c.year) + "-" + two(c.month) + "-" + two(c.day) + "T" + two(c.hour) + ":" + two(c.minute) + ":" + two(c.second) + offsetText(offset);
}

Civil localCivil(int64_t utc, const Place& p, int* off = nullptr, bool* dst = nullptr) {
    const int o = offsetAt(p, utc, dst);
    if (off) *off = o;
    return civilFromUnix(utc + o * 60);
}

Civil todayHere() { return localCivil(nowUnix(), findPlace("local")); }

std::vector<Place> placesParam(const std::string& text) {
    std::vector<Place> out;
    std::string cur;
    for (char c : text + "|") {
        if (c == '|' || c == ';' || c == '\n') {
            if (!sysutil::trim(cur).empty()) out.push_back(findPlace(sysutil::trim(cur)));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// the instant a "time" parameter means in `place`; no date = today there
int64_t instantFrom(const std::string& text, const Place& place) {
    if (sysutil::trim(text).empty() || sysutil::lowerAscii(sysutil::trim(text)) == "now") return nowUnix();
    Civil c;
    bool hasDate = false, hasTime = false;
    if (!parseDateTime(text, c, hasDate, hasTime)) throw std::runtime_error("can't read the time '" + text + "' (try 2026-10-16 14:30, 9:30am or tomorrow 8:00)");
    if (!hasDate) {
        Civil here = localCivil(nowUnix(), place);
        c.year = here.year, c.month = here.month, c.day = here.day;
    }
    return toUtc(c, place);
}

Civil dateFrom(const std::string& text) {
    if (sysutil::trim(text).empty()) return todayHere();
    Civil c;
    bool hasDate = false, hasTime = false;
    if (!parseDateTime(text, c, hasDate, hasTime) || !hasDate) throw std::runtime_error("can't read the date '" + text + "' (try 2026-10-16, 16 Oct 2026 or tomorrow)");
    if (!hasTime) c.hour = c.minute = c.second = 0;
    return c;
}

std::string dateText(const Civil& c) {
    return std::string(kWeekdays[weekdayOf(daysFromCivil(c.year, c.month, c.day))]) + " " + std::to_string(c.day) + " " + kMonths[c.month - 1] + " " + std::to_string(c.year);
}

std::string clockText(const Civil& c, bool seconds = true) { return two(c.hour) + ":" + two(c.minute) + (seconds ? ":" + two(c.second) : ""); }

// ---- alarms on disk ----
sysutil::fs::path alarmFile() { return sysutil::dataDir() / "clock" / "alarms.tsv"; }

void loadAlarms(State& st) {
    if (st.loaded) return;
    st.loaded = true;
    std::string text;
    try {
        text = sysutil::readFile(alarmFile());
    } catch (const std::exception&) {
        return;
    }
    for (const auto& line : sysutil::splitLines(text)) {
        std::vector<std::string> f;
        std::string cur;
        for (char c : line + "\t") {
            if (c == '\t') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (f.size() < 8 || f[0].empty() || f[0][0] == '#') continue;
        Alarm a;
        a.id = f[0];
        a.hour = std::atoi(f[1].c_str());
        a.minute = std::atoi(f[2].c_str());
        a.days = f[3];
        a.on = f[4] == "on";
        a.created = std::atoll(f[5].c_str());
        a.lastRung = std::atoll(f[6].c_str());
        a.label = f[7];
        st.alarms.push_back(a);
        st.alarmCounter = std::max(st.alarmCounter, std::atoi(a.id.c_str() + 1));
    }
}

void saveAlarms(const State& st) {
    std::string out = "# id\thour\tminute\tdays\ton\tcreated\tlast rung\tlabel\n";
    for (const auto& a : st.alarms)
        out += a.id + "\t" + std::to_string(a.hour) + "\t" + std::to_string(a.minute) + "\t" + a.days + "\t" + (a.on ? "on" : "off") + "\t" + std::to_string(a.created) + "\t" +
               std::to_string(a.lastRung) + "\t" + a.label + "\n";
    std::error_code ec;
    sysutil::fs::create_directories(alarmFile().parent_path(), ec);
    sysutil::writeFile(alarmFile(), out);
}

bool alarmOnDay(const Alarm& a, const Civil& day) {
    const int wd = weekdayOf(daysFromCivil(day.year, day.month, day.day));
    const std::string d = sysutil::lowerAscii(a.days);
    if (d == "daily" || d == "every day") return true;
    if (d == "weekdays") return wd >= 1 && wd <= 5;
    if (d == "weekends") return wd == 0 || wd == 6;
    if (d.size() == 10 && d[4] == '-') return d == std::to_string(day.year) + "-" + two(day.month) + "-" + two(day.day);
    static const char* shortNames[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    return d.find(shortNames[wd]) != std::string::npos;
}

// the ring times around now, in UTC, from `fromDay` days ago to `toDay` days ahead
std::vector<int64_t> ringTimes(const Alarm& a, int fromDay, int toDay) {
    const Place here = findPlace("local");
    const Civil today = todayHere();
    const int64_t base = daysFromCivil(today.year, today.month, today.day);
    std::vector<int64_t> out;
    for (int i = fromDay; i <= toDay; ++i) {
        Civil day = civilFromDays(base + i);
        if (!alarmOnDay(a, day)) continue;
        day.hour = a.hour;
        day.minute = a.minute;
        out.push_back(toUtc(day, here));
    }
    return out;
}

int64_t nextRing(const Alarm& a) {
    const int64_t now = nowUnix();
    for (int64_t t : ringTimes(a, 0, 370)) {
        if (t > now) return t;
    }
    return 0;
}

std::string normaliseDays(const std::string& in) {
    std::string d = sysutil::lowerAscii(sysutil::trim(in));
    if (d.empty() || d == "every day" || d == "everyday") return "daily";
    if (d == "daily" || d == "weekdays" || d == "weekends") return d;
    if (d == "once") {  // the next time it comes round: today if still ahead, else tomorrow (fixed below)
        return "once";
    }
    Civil c;
    bool hasDate = false, hasTime = false;
    if (parseDateTime(d, c, hasDate, hasTime) && hasDate) return std::to_string(c.year) + "-" + two(c.month) + "-" + two(c.day);
    static const char* shortNames[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    std::string out;
    for (int i = 0; i < 7; ++i) {
        if (d.find(shortNames[i]) != std::string::npos) out += (out.empty() ? "" : ",") + std::string(shortNames[i]);
    }
    if (d.find("mon-fri") != std::string::npos) return "weekdays";
    if (out.empty()) throw std::runtime_error("can't read the days '" + in + "' (daily, weekdays, weekends, once, mon,wed,fri or a date)");
    return out;
}

std::string alarmLine(const Alarm& a) {
    std::string next;
    if (a.on) {
        int64_t t = nextRing(a);
        if (t) {
            Civil c = localCivil(t, findPlace("local"));
            next = ", next " + dateText(c).substr(0, 3) + " " + std::to_string(c.day) + " " + std::string(kMonths[c.month - 1]).substr(0, 3);
        }
    }
    return a.id + "  " + two(a.hour) + ":" + two(a.minute) + "  " + a.days + (a.label.empty() ? "" : "  \"" + a.label + "\"") + (a.on ? "" : "  (off)") + next;
}

// keeps everything from before, so undo puts timers, the stopwatch and alarms back
class Change : public Action {
public:
    using Fn = std::function<Result(State&, const Params&)>;
    Change(StatePtr st, Params p, Fn fn, bool alarms) : st_(std::move(st)), p_(std::move(p)), fn_(std::move(fn)), alarms_(alarms) {}
    Result apply() override {
        loadAlarms(*st_);
        before_ = *st_;
        try {
            Result r = fn_(*st_, p_);
            if (!r.ok) *st_ = before_;
            else if (alarms_) saveAlarms(*st_);
            return r;
        } catch (const std::exception& e) {
            *st_ = before_;
            return Result::failure(e.what());
        }
    }
    void revert() override {
        *st_ = before_;
        if (alarms_) {
            try {
                saveAlarms(*st_);
            } catch (const std::exception&) {
            }
        }
    }

private:
    StatePtr st_;
    Params p_;
    Fn fn_;
    bool alarms_;
    State before_;
};

ActionFactory changer(StatePtr st, Change::Fn fn, bool alarms = false) {
    return [st, fn, alarms](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<Change>(st, p, fn, alarms); };
}

ActionFactory reader(StatePtr st, std::function<Result(State&, const Params&)> fn) {
    return [st, fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([st, fn, p] {
            try {
                loadAlarms(*st);
                return fn(*st, p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

Timer& findTimer(State& st, const std::string& id) {
    if (st.timers.empty()) throw std::runtime_error("no timers (clock.timer_start sets one)");
    if (id.empty()) return st.timers.back();
    for (auto& t : st.timers) {
        if (t.id == id || t.label == id) return t;
    }
    throw std::runtime_error("no timer called '" + id + "' (clock.timers lists them)");
}

std::string timerLine(const Timer& t) {
    return t.id + "  " + (t.done() ? "done" : formatDuration(t.remaining()) + " left") + (t.paused ? " (paused)" : "") + " of " + formatDuration(t.duration) +
           (t.label.empty() ? "" : "  \"" + t.label + "\"");
}

Result swResult(const State& st, const std::string& what) {
    std::string text = what + (what.empty() ? "" : "\n") + formatDuration(st.stopwatch(), true) + (st.swRunning ? " (running)" : "") + "\n";
    NumberList laps;
    double prev = 0;
    for (std::size_t i = 0; i < st.laps.size(); ++i) {
        text += "  lap " + std::to_string(i + 1) + "  " + formatDuration(st.laps[i] - prev, true) + "  (" + formatDuration(st.laps[i], true) + ")\n";
        laps.push_back(st.laps[i] - prev);
        prev = st.laps[i];
    }
    return Result::success(text, st.stopwatch()).with("seconds", st.stopwatch()).with("running", st.swRunning).with("laps", laps);
}

}  // namespace

void registerTimeCommands(Session& s, StatePtr st);
void registerTimerCommands(Session& s, StatePtr st);
void registerAlarmCommands(Session& s, StatePtr st);
void registerDateCommands(Session& s, StatePtr st);

void registerCommands(Session& session) {
    auto st = std::make_shared<State>();
    registerTimeCommands(session, st);
    registerTimerCommands(session, st);
    registerAlarmCommands(session, st);
    registerDateCommands(session, st);
}

void registerTimeCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"clock.now", "Time now",
             "The date and time now, here or in any city or offset: weekday, ISO week, day of the year, UTC offset, whether "
             "summer time is on, and Unix time.",
             {opt("place", ParamType::String, "a city, zone id or offset (default: here)", std::string("local"))}, false, true},
            reader(st, [](State&, const Params& p) {
                const Place place = findPlace(stringParam(p, "place", "local"));
                const int64_t now = nowUnix();
                int off = 0;
                bool dst = false;
                Civil c = localCivil(now, place, &off, &dst);
                const int doy = static_cast<int>(daysFromCivil(c.year, c.month, c.day) - daysFromCivil(c.year, 1, 1)) + 1;
                const int yearDays = daysInMonth(c.year, 2) == 29 ? 366 : 365;
                std::string text = (place.local ? std::string() : placeName(place) + ": ") + dateText(c) + ", " + clockText(c) + "\nUTC" + offsetText(off) +
                                   (dst ? ", summer time" : "") + "; week " + std::to_string(isoWeek(c.year, c.month, c.day)) + ", day " + std::to_string(doy) + " of " +
                                   std::to_string(yearDays) + "\nUnix time " + std::to_string(now);
                return Result::success(text, isoText(c, off))
                    .with("iso", isoText(c, off))
                    .with("date", std::to_string(c.year) + "-" + two(c.month) + "-" + two(c.day))
                    .with("time", clockText(c))
                    .with("weekday", std::string(kWeekdays[weekdayOf(daysFromCivil(c.year, c.month, c.day))]))
                    .with("week", static_cast<int64_t>(isoWeek(c.year, c.month, c.day)))
                    .with("day_of_year", static_cast<int64_t>(doy))
                    .with("offset_minutes", static_cast<int64_t>(off))
                    .with("dst", dst)
                    .with("unix", static_cast<int64_t>(now));
            }));

    reg.add({"clock.world", "World clock",
             "The time in several places at once (now, or at=a time here), with how many hours each is ahead of here.",
             {opt("places", ParamType::String, "cities separated by |", std::string("London|New York|Los Angeles|Tokyo|Sydney|Delhi|Dubai|Paris|Shanghai")),
              opt("at", ParamType::String, "a time here instead of now, e.g. 15:00 or 2026-12-31 23:00")},
             false, true},
            reader(st, [](State&, const Params& p) {
                const Place here = findPlace("local");
                const int64_t t = instantFrom(stringParam(p, "at"), here);
                const Civil hereC = localCivil(t, here);
                const int hereOff = offsetAt(here, t);
                const int64_t hereDay = daysFromCivil(hereC.year, hereC.month, hereC.day);
                std::string text = "here: " + dateText(hereC) + ", " + clockText(hereC, false) + " (UTC" + offsetText(hereOff) + ")\n", rows;
                for (const auto& pl : placesParam(stringParam(p, "places", "London|New York|Los Angeles|Tokyo|Sydney|Delhi|Dubai|Paris|Shanghai"))) {
                    int off = 0;
                    bool dst = false;
                    Civil c = localCivil(t, pl, &off, &dst);
                    const int64_t dd = daysFromCivil(c.year, c.month, c.day) - hereDay;
                    const double ahead = (off - hereOff) / 60.0;
                    char diff[32];
                    std::snprintf(diff, sizeof diff, ahead == std::floor(ahead) ? "%+.0f h" : "%+.1f h", ahead);
                    char line[160];
                    std::snprintf(line, sizeof line, "%-16s %s %s  %-9s %s%s\n", placeName(pl).c_str(), clockText(c, false).c_str(),
                                  dd == 0 ? "         " : dd > 0 ? "next day " : "day before", diff, ("UTC" + offsetText(off)).c_str(), dst ? " summer" : "");
                    text += line;
                    rows += placeName(pl) + "\t" + isoText(c, off) + "\t" + std::to_string(off) + "\t" + (dst ? "dst" : "") + "\t" + std::to_string(dd) + "\n";
                }
                return Result::success(text).with("rows", rows);
            }));

    reg.add({"clock.zones", "Places", "The cities and zones the clock knows (about a hundred), optionally only those matching search=.",
             {opt("search", ParamType::String, "part of a city, zone or country name")}, false, true},
            reader(st, [](State&, const Params& p) {
                const std::string q = sysutil::lowerAscii(stringParam(p, "search"));
                const int64_t now = nowUnix();
                std::string text, rows;
                std::size_t n = 0;
                for (const auto& z : zones()) {
                    if (!q.empty() && sysutil::lowerAscii(z.city + " " + z.id + " " + z.country).find(q) == std::string::npos) continue;
                    ++n;
                    Place pl;
                    pl.zone = z;
                    bool dst = false;
                    const int off = offsetAt(pl, now, &dst);
                    text += z.city + (z.country.empty() ? "" : ", " + z.country) + "  UTC" + offsetText(off) + (dst ? " (summer time)" : "") + "  " + z.id + "\n";
                    rows += z.city + "\t" + z.id + "\t" + std::to_string(off) + "\t" + z.country + "\n";
                }
                return Result::success(n ? text : "no places match").with("count", static_cast<int64_t>(n)).with("rows", rows);
            }));

    reg.add({"clock.convert", "Convert time", "What a time in one place is in others, e.g. 9:00 in New York in London and Tokyo.",
             {req("time", ParamType::String, "the time, e.g. 9:00, 2026-03-29 01:30, tomorrow 3pm"),
              opt("from", ParamType::String, "where that time is (default: here)", std::string("local")),
              opt("to", ParamType::String, "places separated by | (default: UTC and here)")},
             false, true},
            reader(st, [](State&, const Params& p) {
                const Place from = findPlace(stringParam(p, "from", "local"));
                const int64_t t = instantFrom(stringParam(p, "time"), from);
                int fromOff = 0;
                Civil fc = localCivil(t, from, &fromOff);
                std::string text = dateText(fc) + ", " + clockText(fc, false) + " " + placeName(from) + " (UTC" + offsetText(fromOff) + ") is\n", rows;
                std::string to = stringParam(p, "to");
                std::vector<Place> targets = to.empty() ? placesParam("UTC|local") : placesParam(to);
                std::string first;
                for (const auto& pl : targets) {
                    int off = 0;
                    bool dst = false;
                    Civil c = localCivil(t, pl, &off, &dst);
                    text += "  " + clockText(c, false) + "  " + dateText(c) + "  " + placeName(pl) + " (UTC" + offsetText(off) + (dst ? ", summer time" : "") + ")\n";
                    rows += placeName(pl) + "\t" + isoText(c, off) + "\n";
                    if (first.empty()) first = isoText(c, off);
                }
                return Result::success(text, first).with("unix", static_cast<int64_t>(t)).with("rows", rows).with("iso", first);
            }));
}

void registerTimerCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"clock.timer_start", "Start timer", "Start a countdown: 90s, 5m, 1h30m or 1:30:00. clock.due tells when it is done.",
             {req("duration", ParamType::String, "how long"), opt("label", ParamType::String, "what it is for, e.g. tea")}, true},
            changer(st, [](State& state, const Params& p) {
                const double secs = parseDuration(stringParam(p, "duration"));
                if (secs <= 0 || secs > 366 * 86400.0) throw std::runtime_error("a timer must be between a second and a year");
                Timer t;
                t.id = "t" + std::to_string(++state.timerCounter);
                t.label = stringParam(p, "label");
                t.duration = secs;
                t.endsAt = Steady::now() + std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(secs));
                state.timers.push_back(t);
                return Result::success("timer " + t.id + " set for " + formatDuration(secs) + (t.label.empty() ? "" : " (" + t.label + ")"), t.id).with("timer", t.id).with("seconds", secs);
            }));

    reg.add({"clock.timers", "Timers", "Every timer: time left, paused or done.", {}, false, true},
            reader(st, [](State& state, const Params&) {
                std::string text, rows;
                for (const auto& t : state.timers) {
                    text += timerLine(t) + "\n";
                    rows += t.id + "\t" + std::to_string(t.remaining()) + "\t" + std::to_string(t.duration) + "\t" + (t.done() ? "done" : t.paused ? "paused" : "running") + "\t" + t.label + "\n";
                }
                return Result::success(text.empty() ? "no timers" : text).with("count", static_cast<int64_t>(state.timers.size())).with("rows", rows);
            }));

    reg.add({"clock.timer_pause", "Pause timer", "Pause a timer (default: the newest).", {opt("timer", ParamType::String, "timer id or label")}, true},
            changer(st, [](State& state, const Params& p) {
                Timer& t = findTimer(state, stringParam(p, "timer"));
                if (t.paused || t.done()) return Result::success(t.id + " is " + (t.paused ? "already paused" : "done"));
                t.left = t.remaining();
                t.paused = true;
                return Result::success("paused " + t.id + " with " + formatDuration(t.left) + " left");
            }));

    reg.add({"clock.timer_resume", "Resume timer", "Carry on a paused timer.", {opt("timer", ParamType::String, "timer id or label")}, true},
            changer(st, [](State& state, const Params& p) {
                Timer& t = findTimer(state, stringParam(p, "timer"));
                if (!t.paused) return Result::success(t.id + " isn't paused");
                t.paused = false;
                t.endsAt = Steady::now() + std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(t.left));
                return Result::success("resumed " + t.id);
            }));

    reg.add({"clock.timer_cancel", "Cancel timer", "Remove a timer (all=true removes every one).",
             {opt("timer", ParamType::String, "timer id or label"), opt("all", ParamType::Bool, "remove every timer", false)}, true},
            changer(st, [](State& state, const Params& p) {
                if (boolParam(p, "all", false)) {
                    std::size_t n = state.timers.size();
                    state.timers.clear();
                    return Result::success("removed " + sysutil::plural(n, "timer"));
                }
                const std::string id = findTimer(state, stringParam(p, "timer")).id;
                state.timers.erase(std::remove_if(state.timers.begin(), state.timers.end(), [&](const Timer& t) { return t.id == id; }), state.timers.end());
                return Result::success("removed " + id);
            }));

    reg.add({"clock.stopwatch", "Stopwatch", "The stopwatch: start, stop, lap, reset, or just show it (with each lap).",
             {{"action", ParamType::String, "what to do", false, {"show", "start", "stop", "lap", "reset"}, std::string("show")}}, false},
            changer(st, [](State& state, const Params& p) {
                const std::string a = stringParam(p, "action", "show");
                if (a == "start") {
                    if (state.swRunning) return swResult(state, "already running");
                    state.swRunning = true;
                    state.swStart = Steady::now();
                    return swResult(state, "started");
                }
                if (a == "stop") {
                    if (!state.swRunning) return swResult(state, "not running");
                    state.swBanked = state.stopwatch();
                    state.swRunning = false;
                    return swResult(state, "stopped");
                }
                if (a == "lap") {
                    if (!state.swRunning) throw std::runtime_error("start the stopwatch first");
                    state.laps.push_back(state.stopwatch());
                    return swResult(state, "lap " + std::to_string(state.laps.size()));
                }
                if (a == "reset") {
                    state.swRunning = false;
                    state.swBanked = 0;
                    state.laps.clear();
                    return swResult(state, "reset");
                }
                return swResult(state, "");
            }));

    reg.add({"clock.due", "What's due",
             "Timers that have finished and alarms that have gone off since the last look (each is told once). The GUI asks "
             "this every second; scripts can ask it in a loop or use clock.wait.",
             {}, false},
            reader(st, [](State& state, const Params&) {
                std::string text, rows;
                std::size_t n = 0;
                for (auto& t : state.timers) {
                    if (t.done() && !t.notified) {
                        t.notified = true;
                        ++n;
                        text += "timer " + t.id + " is done" + (t.label.empty() ? "" : ": " + t.label) + "\n";
                        rows += "timer\t" + t.id + "\t" + t.label + "\n";
                    }
                }
                const int64_t now = nowUnix();
                bool changed = false;
                for (auto& a : state.alarms) {
                    if (!a.on) continue;
                    const int64_t since = std::max(a.lastRung, a.created);
                    int64_t latest = 0;  // rings once even if it was missed several times
                    for (int64_t ring : ringTimes(a, -2, 0)) {
                        if (ring > since && ring <= now) latest = ring;
                    }
                    if (!latest) continue;
                    a.lastRung = latest;
                    changed = true;
                    ++n;
                    text += "alarm " + a.id + " (" + two(a.hour) + ":" + two(a.minute) + ")" + (a.label.empty() ? "" : ": " + a.label) + "\n";
                    rows += "alarm\t" + a.id + "\t" + a.label + "\n";
                    if (a.days.size() == 10 && a.days[4] == '-') a.on = false;  // a one-off has rung
                }
                if (changed) saveAlarms(state);
                return Result::success(n ? text : "nothing due", static_cast<int64_t>(n)).with("count", static_cast<int64_t>(n)).with("rows", rows);
            }));

    reg.add({"clock.wait", "Wait", "Wait: for some seconds, or until a timer is done (at most 10 minutes).",
             {opt("seconds", ParamType::Number, "how long to wait"), opt("timer", ParamType::String, "or wait for this timer")}, false, true},
            reader(st, [](State& state, const Params& p) {
                double secs = numberParam(p, "seconds", 0);
                std::string label;
                if (!stringParam(p, "timer").empty()) {
                    Timer& t = findTimer(state, stringParam(p, "timer"));
                    if (t.paused) throw std::runtime_error(t.id + " is paused");
                    secs = t.remaining();
                    label = t.id;
                }
                if (secs > 600) throw std::runtime_error("that is longer than 10 minutes; use clock.due to check later");
                std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0, secs)));
                return Result::success(label.empty() ? "waited " + formatDuration(secs, true) : "timer " + label + " is done");
            }));
}

void registerAlarmCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"clock.alarm_add", "Add alarm",
             "Set an alarm at a time of day (7:30, 6pm), daily, on weekdays, weekends, some days (mon,wed,fri), once, or on a date. "
             "Alarms are saved and come back next time.",
             {req("time", ParamType::String, "time of day, e.g. 07:30 or 6:15pm"), opt("days", ParamType::String, "daily, weekdays, weekends, once, mon,wed,fri or a date", std::string("daily")),
              opt("label", ParamType::String, "what it is for")},
             true},
            changer(
                st,
                [](State& state, const Params& p) {
                    Civil c;
                    bool hasDate = false, hasTime = false;
                    if (!parseDateTime(stringParam(p, "time"), c, hasDate, hasTime) || !hasTime) throw std::runtime_error("can't read the time '" + stringParam(p, "time") + "' (try 07:30 or 6:15pm)");
                    Alarm a;
                    a.hour = c.hour;
                    a.minute = c.minute;
                    a.days = hasDate ? std::to_string(c.year) + "-" + two(c.month) + "-" + two(c.day) : normaliseDays(stringParam(p, "days", "daily"));
                    if (a.days == "once") {
                        Civil today = todayHere();
                        Civil when = today;
                        when.hour = a.hour;
                        when.minute = a.minute;
                        if (toUtc(when, findPlace("local")) <= nowUnix()) when = civilFromDays(daysFromCivil(today.year, today.month, today.day) + 1);
                        a.days = std::to_string(when.year) + "-" + two(when.month) + "-" + two(when.day);
                    }
                    a.label = stringParam(p, "label");
                    a.created = nowUnix();
                    a.id = "a" + std::to_string(++state.alarmCounter);
                    state.alarms.push_back(a);
                    return Result::success("alarm " + alarmLine(a), a.id).with("alarm", a.id).with("next", static_cast<int64_t>(nextRing(a)));
                },
                true));

    reg.add({"clock.alarms", "Alarms", "Every alarm: time, days, label, on or off, and when it next goes off.", {}, false, true},
            reader(st, [](State& state, const Params&) {
                std::string text, rows;
                for (const auto& a : state.alarms) {
                    text += alarmLine(a) + "\n";
                    rows += a.id + "\t" + two(a.hour) + ":" + two(a.minute) + "\t" + a.days + "\t" + (a.on ? "on" : "off") + "\t" + std::to_string(a.on ? nextRing(a) : 0) + "\t" + a.label + "\n";
                }
                return Result::success(text.empty() ? "no alarms" : text).with("count", static_cast<int64_t>(state.alarms.size())).with("rows", rows);
            }));

    reg.add({"clock.alarm_remove", "Remove alarm", "Delete an alarm (undo brings it back).", {req("alarm", ParamType::String, "alarm id, e.g. a1")}, true},
            changer(
                st,
                [](State& state, const Params& p) {
                    const std::string id = stringParam(p, "alarm");
                    auto it = std::find_if(state.alarms.begin(), state.alarms.end(), [&](const Alarm& a) { return a.id == id; });
                    if (it == state.alarms.end()) throw std::runtime_error("no alarm called '" + id + "' (clock.alarms lists them)");
                    state.alarms.erase(it);
                    return Result::success("removed alarm " + id);
                },
                true));

    reg.add({"clock.alarm_toggle", "Turn alarm on or off", "Turn an alarm off without deleting it, or back on.",
             {req("alarm", ParamType::String, "alarm id"), opt("on", ParamType::Bool, "on (true) or off (false); default flips it")}, true},
            changer(
                st,
                [](State& state, const Params& p) {
                    const std::string id = stringParam(p, "alarm");
                    for (auto& a : state.alarms) {
                        if (a.id != id) continue;
                        a.on = p.count("on") ? boolParam(p, "on", true) : !a.on;
                        if (a.on) a.created = std::max(a.created, nowUnix());  // don't ring for times that passed while it was off
                        return Result::success("alarm " + alarmLine(a)).with("on", a.on);
                    }
                    throw std::runtime_error("no alarm called '" + id + "'");
                },
                true));
}

void registerDateCommands(Session& s, StatePtr st) {
    auto& reg = s.registry();
    reg.add({"clock.date_diff", "Days between", "How long between two dates (or times): days, weeks, months and years, and working days (Monday to Friday).",
             {req("from", ParamType::String, "first date, e.g. 2026-01-01"), opt("to", ParamType::String, "second date (default: today)")}, false, true},
            reader(st, [](State&, const Params& p) {
                Civil a = dateFrom(stringParam(p, "from")), b = stringParam(p, "to").empty() ? todayHere() : dateFrom(stringParam(p, "to"));
                const int64_t da = daysFromCivil(a.year, a.month, a.day), db = daysFromCivil(b.year, b.month, b.day);
                const int64_t days = db - da;
                const int64_t secs = (unixFromCivil(b) - unixFromCivil(a));
                // years, months, days
                Civil lo = days >= 0 ? a : b, hi = days >= 0 ? b : a;
                int y = hi.year - lo.year, m = hi.month - lo.month, d = hi.day - lo.day;
                if (d < 0) {
                    --m;
                    d += daysInMonth(hi.month == 1 ? hi.year - 1 : hi.year, hi.month == 1 ? 12 : hi.month - 1);
                }
                if (m < 0) {
                    --y;
                    m += 12;
                }
                // working days in [lo, hi)
                int64_t work = 0;
                const int64_t from = std::min(da, db), to = std::max(da, db);
                const int64_t fullWeeks = (to - from) / 7;
                work = fullWeeks * 5;
                for (int64_t x = from + fullWeeks * 7; x < to; ++x) {
                    int wd = weekdayOf(x);
                    if (wd >= 1 && wd <= 5) ++work;
                }
                std::string text = sysutil::plural(static_cast<std::size_t>(std::llabs(days)), "day") + (days < 0 ? " before" : "") + "\n" +
                                   std::to_string(std::llabs(days) / 7) + " weeks and " + std::to_string(std::llabs(days) % 7) + " days\n" + std::to_string(y) + " years, " +
                                   std::to_string(m) + " months, " + std::to_string(d) + " days\n" + std::to_string(work) + " working days (Mon-Fri)";
                if (secs % 86400) text += "\n" + formatDuration(static_cast<double>(std::llabs(secs))) + " (h:mm:ss)";
                return Result::success(text, static_cast<int64_t>(days))
                    .with("days", static_cast<int64_t>(days))
                    .with("weeks", static_cast<double>(days) / 7.0)
                    .with("working_days", static_cast<int64_t>(days < 0 ? -work : work))
                    .with("years", static_cast<int64_t>(y))
                    .with("months", static_cast<int64_t>(m))
                    .with("seconds", static_cast<int64_t>(secs));
            }));

    reg.add({"clock.date_add", "Add to a date",
             "A date plus or minus some time: \"3 weeks\", \"-10 days\", \"2 months\", \"1 year 2 months\", \"5 working days\", \"36 hours\".",
             {opt("date", ParamType::String, "the start date (default: today)"), req("add", ParamType::String, "how much to add (negative to go back)")}, false, true},
            reader(st, [](State&, const Params& p) {
                Civil c = dateFrom(stringParam(p, "date"));
                std::string t = sysutil::lowerAscii(stringParam(p, "add"));
                std::size_t i = 0;
                int64_t addDays = 0, addSecs = 0;
                int addMonths = 0;
                bool any = false;
                while (i < t.size()) {
                    while (i < t.size() && (t[i] == ' ' || t[i] == ',' || t[i] == '+' || (t.compare(i, 4, "and ") == 0 && (i += 3)))) ++i;
                    if (i >= t.size()) break;
                    std::size_t start = i;
                    if (t[i] == '-') ++i;
                    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) ++i;
                    if (i == start || (i == start + 1 && t[start] == '-')) throw std::runtime_error("can't read '" + stringParam(p, "add") + "' (try 3 weeks, -10 days, 2 months)");
                    const double v = std::atof(t.substr(start, i - start).c_str());
                    while (i < t.size() && t[i] == ' ') ++i;
                    std::size_t u = i;
                    while (i < t.size() && std::isalpha(static_cast<unsigned char>(t[i]))) ++i;
                    std::string unit = t.substr(u, i - u);
                    if (unit == "working" || unit == "business" || unit == "work") {
                        while (i < t.size() && t[i] == ' ') ++i;
                        while (i < t.size() && std::isalpha(static_cast<unsigned char>(t[i]))) ++i;
                        unit = "workday";
                    }
                    any = true;
                    if (unit.rfind("y", 0) == 0) addMonths += static_cast<int>(v * 12);
                    else if (unit.rfind("mo", 0) == 0) addMonths += static_cast<int>(v);
                    else if (unit.rfind("w", 0) == 0 && unit != "workday") addDays += static_cast<int64_t>(v * 7);
                    else if (unit.rfind("d", 0) == 0 || unit.empty()) addDays += static_cast<int64_t>(v);
                    else if (unit.rfind("h", 0) == 0) addSecs += static_cast<int64_t>(v * 3600);
                    else if (unit.rfind("mi", 0) == 0 || unit == "m") addSecs += static_cast<int64_t>(v * 60);
                    else if (unit.rfind("s", 0) == 0) addSecs += static_cast<int64_t>(v);
                    else if (unit == "workday") {
                        int64_t day = daysFromCivil(c.year, c.month, c.day);
                        int64_t n = static_cast<int64_t>(v), step = n >= 0 ? 1 : -1;
                        for (int64_t k = 0; k != n;) {
                            day += step;
                            int wd = weekdayOf(day);
                            if (wd >= 1 && wd <= 5) k += step;
                        }
                        Civil nd = civilFromDays(day);
                        c.year = nd.year, c.month = nd.month, c.day = nd.day;
                    } else {
                        throw std::runtime_error("unknown unit '" + unit + "' (days, weeks, months, years, hours, minutes, working days)");
                    }
                }
                if (!any) throw std::runtime_error("nothing to add");
                if (addMonths) {
                    int total = c.year * 12 + (c.month - 1) + addMonths;
                    c.year = total / 12;
                    c.month = total % 12 + 1;
                    c.day = std::min(c.day, daysInMonth(c.year, c.month));
                }
                Civil r = civilFromUnix(unixFromCivil(c) + addDays * 86400 + addSecs);
                const bool timePart = addSecs != 0 || r.hour || r.minute;
                std::string iso = std::to_string(r.year) + "-" + two(r.month) + "-" + two(r.day) + (timePart ? " " + clockText(r, false) : "");
                return Result::success(dateText(r) + (timePart ? ", " + clockText(r, false) : "") + "  (" + iso + ")", iso)
                    .with("date", iso)
                    .with("weekday", std::string(kWeekdays[weekdayOf(daysFromCivil(r.year, r.month, r.day))]));
            }));

    reg.add({"clock.calendar", "Calendar", "A month as a calendar (Monday first, week numbers on the left, today marked), or a whole year with month=0.",
             {opt("year", ParamType::Int, "year (default: this year)"), opt("month", ParamType::Int, "1-12, or 0 for the whole year (default: this month)")}, false, true},
            reader(st, [](State&, const Params& p) {
                const Civil today = todayHere();
                const int y = static_cast<int>(intParam(p, "year", today.year));
                const int m = static_cast<int>(intParam(p, "month", p.count("year") && !p.count("month") ? 0 : today.month));
                if (m < 0 || m > 12 || y < 1 || y > 9999) throw std::runtime_error("month is 1-12 (0 = the whole year), year 1-9999");
                std::string text;
                for (int mm = (m ? m : 1); mm <= (m ? m : 12); ++mm) text += calendarText(y, mm, (y == today.year && mm == today.month) ? today.day : 0) + (m ? "" : "\n");
                return Result::success(text).with("days", static_cast<int64_t>(m ? daysInMonth(y, m) : (daysInMonth(y, 2) == 29 ? 366 : 365)));
            }));
}

}  // namespace wetwaresuite::clock
