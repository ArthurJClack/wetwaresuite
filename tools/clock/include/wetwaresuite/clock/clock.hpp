// clock: the time here and around the world, timers, a stopwatch with laps,
// alarms that are remembered between runs, date arithmetic and calendars.
// Time zones come from a built-in table of about a hundred cities with their
// daylight-saving rules (US, EU, Australia, New Zealand, Israel, Chile), so it
// works the same on every system and needs no time-zone database.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::clock {

struct Civil {
    int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};

std::int64_t daysFromCivil(int y, int m, int d);  // days since 1970-01-01
Civil civilFromDays(std::int64_t days);
Civil civilFromUnix(std::int64_t seconds);
std::int64_t unixFromCivil(const Civil& c);  // as if UTC
int weekdayOf(std::int64_t days);            // 0 = Sunday
int isoWeek(int y, int m, int d);            // ISO 8601 week number
int daysInMonth(int y, int m);

struct Zone {
    std::string city;     // "New York"
    std::string id;       // "America/New_York"
    int offset = 0;       // standard time, minutes east of UTC
    std::string rule;     // daylight saving: "", "us", "eu", "au", "nz", "il", "cl"
    std::string country;
};
const std::vector<Zone>& zones();

// A place to show time for: the computer's own zone, a city, or a fixed offset.
struct Place {
    bool local = false;
    Zone zone;
};
// "local", a city ("tokyo"), an id ("Europe/Paris"), "UTC", "GMT", "UTC+5:30", "+02:00", "EST", ...
Place findPlace(const std::string& text);  // throws std::runtime_error with suggestions
std::string placeName(const Place& p);
int offsetAt(const Place& p, std::int64_t utc, bool* dst = nullptr);  // minutes east of UTC
// Local wall time to UTC; a time skipped by a clock change moves forward.
std::int64_t toUtc(const Civil& local, const Place& p);
std::string offsetText(int minutes);  // "+05:30", "-08:00"

// "2026-10-16", "2026-10-16 14:30", "2026-10-16T14:30:05", "14:30", "7:30pm",
// "today", "tomorrow", "yesterday", "now", "16 Oct 2026", "Oct 16, 2026"
bool parseDateTime(const std::string& text, Civil& out, bool& hasDate, bool& hasTime);
// "90", "90s", "5m", "1h30m", "1:30:00", "2 hours 15 minutes", "1.5h" -> seconds
double parseDuration(const std::string& text);
std::string formatDuration(double seconds, bool tenths = false);  // "1:05:09", "4:03.2"

// One month as text, Monday first, ISO week numbers on the left; `mark` (a day) is bracketed.
std::string calendarText(int year, int month, int mark = 0);

void registerCommands(Session& session);

}  // namespace wetwaresuite::clock
