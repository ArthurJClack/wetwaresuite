// Dates, time zones with daylight saving, parsing and formatting.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <stdexcept>

#include "wetwaresuite/clock/clock.hpp"

namespace wetwaresuite::clock {

std::int64_t daysFromCivil(int y, int m, int d) {  // Howard Hinnant's algorithm
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

Civil civilFromDays(std::int64_t z) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    Civil c;
    c.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    c.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    c.year = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (c.month <= 2));
    return c;
}

Civil civilFromUnix(std::int64_t s) {
    std::int64_t days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
    std::int64_t rest = s - days * 86400;
    Civil c = civilFromDays(days);
    c.hour = static_cast<int>(rest / 3600);
    c.minute = static_cast<int>(rest % 3600 / 60);
    c.second = static_cast<int>(rest % 60);
    return c;
}

std::int64_t unixFromCivil(const Civil& c) { return daysFromCivil(c.year, c.month, c.day) * 86400 + c.hour * 3600 + c.minute * 60 + c.second; }

int weekdayOf(std::int64_t days) {
    const int r = static_cast<int>((days + 4) % 7);  // 1970-01-01 was a Thursday
    return r < 0 ? r + 7 : r;
}

int daysInMonth(int y, int m) {
    static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return n[(m - 1 + 12) % 12];
}

int isoWeek(int y, int m, int d) {
    const std::int64_t days = daysFromCivil(y, m, d);
    const int wd = (weekdayOf(days) + 6) % 7;  // Monday = 0
    const std::int64_t thursday = days - wd + 3;
    const int ty = civilFromDays(thursday).year;
    return static_cast<int>((thursday - daysFromCivil(ty, 1, 1)) / 7 + 1);
}

namespace {

// the day of the n-th (1..5) or last (n = -1) `weekday` (0 = Sunday) of a month
std::int64_t nthWeekday(int y, int m, int weekday, int n) {
    if (n > 0) {
        const std::int64_t first = daysFromCivil(y, m, 1);
        return first + (weekday - weekdayOf(first) + 7) % 7 + 7 * (n - 1);
    }
    const std::int64_t last = daysFromCivil(y, m, daysInMonth(y, m));
    return last - (weekdayOf(last) - weekday + 7) % 7;
}

// is daylight saving on at `utc` for a zone with standard offset `std` (minutes)?
bool dstOn(const std::string& rule, int stdOff, std::int64_t utc) {
    if (rule.empty()) return false;
    const int y = civilFromUnix(utc + stdOff * 60).year;
    auto at = [&](std::int64_t day, int hour) { return day * 86400 + hour * 3600; };
    const std::int64_t so = stdOff * 60, dso = (stdOff + 60) * 60;
    if (rule == "us") return utc >= at(nthWeekday(y, 3, 0, 2), 2) - so && utc < at(nthWeekday(y, 11, 0, 1), 2) - dso;
    if (rule == "eu") return utc >= at(nthWeekday(y, 3, 0, -1), 1) && utc < at(nthWeekday(y, 10, 0, -1), 1);
    if (rule == "il") return utc >= at(nthWeekday(y, 3, 0, -1) - 2, 2) - so && utc < at(nthWeekday(y, 10, 0, -1), 2) - dso;
    // southern hemisphere: on from spring (this year) until autumn (next year)
    if (rule == "au") return utc >= at(nthWeekday(y, 10, 0, 1), 2) - so || utc < at(nthWeekday(y, 4, 0, 1), 3) - dso;
    if (rule == "nz") return utc >= at(nthWeekday(y, 9, 0, -1), 2) - so || utc < at(nthWeekday(y, 4, 0, 1), 3) - dso;
    if (rule == "cl") return utc >= at(nthWeekday(y, 9, 0, 1), 4) || utc < at(nthWeekday(y, 4, 0, 1), 3);
    return false;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string squash(const std::string& s) {  // "New_York" / "new york" -> "newyork"
    std::string out;
    for (char c : lower(s)) {
        if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    }
    return out;
}

int localOffset(std::int64_t utc, bool* dst) {
    std::time_t t = static_cast<std::time_t>(utc);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    if (dst) *dst = tm.tm_isdst > 0;
    Civil c{tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec};
    return static_cast<int>((unixFromCivil(c) - utc) / 60);
}

}  // namespace

const std::vector<Zone>& zones() {
    static const std::vector<Zone> list = {
        {"UTC", "UTC", 0, "", ""},
        {"London", "Europe/London", 0, "eu", "United Kingdom"}, {"Dublin", "Europe/Dublin", 0, "eu", "Ireland"},
        {"Lisbon", "Europe/Lisbon", 0, "eu", "Portugal"}, {"Reykjavik", "Atlantic/Reykjavik", 0, "", "Iceland"},
        {"Azores", "Atlantic/Azores", -60, "eu", "Portugal"}, {"Paris", "Europe/Paris", 60, "eu", "France"},
        {"Berlin", "Europe/Berlin", 60, "eu", "Germany"}, {"Madrid", "Europe/Madrid", 60, "eu", "Spain"},
        {"Rome", "Europe/Rome", 60, "eu", "Italy"}, {"Amsterdam", "Europe/Amsterdam", 60, "eu", "Netherlands"},
        {"Brussels", "Europe/Brussels", 60, "eu", "Belgium"}, {"Zurich", "Europe/Zurich", 60, "eu", "Switzerland"},
        {"Vienna", "Europe/Vienna", 60, "eu", "Austria"}, {"Prague", "Europe/Prague", 60, "eu", "Czechia"},
        {"Warsaw", "Europe/Warsaw", 60, "eu", "Poland"}, {"Stockholm", "Europe/Stockholm", 60, "eu", "Sweden"},
        {"Oslo", "Europe/Oslo", 60, "eu", "Norway"}, {"Copenhagen", "Europe/Copenhagen", 60, "eu", "Denmark"},
        {"Budapest", "Europe/Budapest", 60, "eu", "Hungary"}, {"Belgrade", "Europe/Belgrade", 60, "eu", "Serbia"},
        {"Lagos", "Africa/Lagos", 60, "", "Nigeria"}, {"Casablanca", "Africa/Casablanca", 60, "", "Morocco"},
        {"Athens", "Europe/Athens", 120, "eu", "Greece"}, {"Helsinki", "Europe/Helsinki", 120, "eu", "Finland"},
        {"Kyiv", "Europe/Kyiv", 120, "eu", "Ukraine"}, {"Bucharest", "Europe/Bucharest", 120, "eu", "Romania"},
        {"Sofia", "Europe/Sofia", 120, "eu", "Bulgaria"}, {"Riga", "Europe/Riga", 120, "eu", "Latvia"},
        {"Jerusalem", "Asia/Jerusalem", 120, "il", "Israel"}, {"Tel Aviv", "Asia/Tel_Aviv", 120, "il", "Israel"},
        {"Cairo", "Africa/Cairo", 120, "", "Egypt"}, {"Johannesburg", "Africa/Johannesburg", 120, "", "South Africa"},
        {"Istanbul", "Europe/Istanbul", 180, "", "Turkey"}, {"Moscow", "Europe/Moscow", 180, "", "Russia"},
        {"Riyadh", "Asia/Riyadh", 180, "", "Saudi Arabia"}, {"Baghdad", "Asia/Baghdad", 180, "", "Iraq"},
        {"Nairobi", "Africa/Nairobi", 180, "", "Kenya"}, {"Doha", "Asia/Qatar", 180, "", "Qatar"},
        {"Tehran", "Asia/Tehran", 210, "", "Iran"}, {"Dubai", "Asia/Dubai", 240, "", "United Arab Emirates"},
        {"Baku", "Asia/Baku", 240, "", "Azerbaijan"}, {"Kabul", "Asia/Kabul", 270, "", "Afghanistan"},
        {"Karachi", "Asia/Karachi", 300, "", "Pakistan"}, {"Tashkent", "Asia/Tashkent", 300, "", "Uzbekistan"},
        {"Almaty", "Asia/Almaty", 300, "", "Kazakhstan"}, {"Delhi", "Asia/Kolkata", 330, "", "India"},
        {"Mumbai", "Asia/Kolkata", 330, "", "India"}, {"Kolkata", "Asia/Kolkata", 330, "", "India"},
        {"Bangalore", "Asia/Kolkata", 330, "", "India"}, {"Colombo", "Asia/Colombo", 330, "", "Sri Lanka"},
        {"Kathmandu", "Asia/Kathmandu", 345, "", "Nepal"}, {"Dhaka", "Asia/Dhaka", 360, "", "Bangladesh"},
        {"Yangon", "Asia/Yangon", 390, "", "Myanmar"}, {"Bangkok", "Asia/Bangkok", 420, "", "Thailand"},
        {"Jakarta", "Asia/Jakarta", 420, "", "Indonesia"}, {"Ho Chi Minh City", "Asia/Ho_Chi_Minh", 420, "", "Vietnam"},
        {"Hanoi", "Asia/Bangkok", 420, "", "Vietnam"}, {"Singapore", "Asia/Singapore", 480, "", "Singapore"},
        {"Kuala Lumpur", "Asia/Kuala_Lumpur", 480, "", "Malaysia"}, {"Hong Kong", "Asia/Hong_Kong", 480, "", "China"},
        {"Shanghai", "Asia/Shanghai", 480, "", "China"}, {"Beijing", "Asia/Shanghai", 480, "", "China"},
        {"Taipei", "Asia/Taipei", 480, "", "Taiwan"}, {"Manila", "Asia/Manila", 480, "", "Philippines"},
        {"Perth", "Australia/Perth", 480, "", "Australia"}, {"Seoul", "Asia/Seoul", 540, "", "South Korea"},
        {"Tokyo", "Asia/Tokyo", 540, "", "Japan"}, {"Osaka", "Asia/Tokyo", 540, "", "Japan"},
        {"Darwin", "Australia/Darwin", 570, "", "Australia"}, {"Adelaide", "Australia/Adelaide", 570, "au", "Australia"},
        {"Brisbane", "Australia/Brisbane", 600, "", "Australia"}, {"Sydney", "Australia/Sydney", 600, "au", "Australia"},
        {"Melbourne", "Australia/Melbourne", 600, "au", "Australia"}, {"Canberra", "Australia/Sydney", 600, "au", "Australia"},
        {"Hobart", "Australia/Hobart", 600, "au", "Australia"}, {"Vladivostok", "Asia/Vladivostok", 600, "", "Russia"},
        {"Guam", "Pacific/Guam", 600, "", "United States"}, {"Noumea", "Pacific/Noumea", 660, "", "New Caledonia"},
        {"Auckland", "Pacific/Auckland", 720, "nz", "New Zealand"}, {"Wellington", "Pacific/Auckland", 720, "nz", "New Zealand"},
        {"Fiji", "Pacific/Fiji", 720, "", "Fiji"}, {"Tonga", "Pacific/Tongatapu", 780, "", "Tonga"},
        {"Kiritimati", "Pacific/Kiritimati", 840, "", "Kiribati"}, {"Honolulu", "Pacific/Honolulu", -600, "", "United States"},
        {"Anchorage", "America/Anchorage", -540, "us", "United States"}, {"Los Angeles", "America/Los_Angeles", -480, "us", "United States"},
        {"San Francisco", "America/Los_Angeles", -480, "us", "United States"}, {"Seattle", "America/Los_Angeles", -480, "us", "United States"},
        {"Vancouver", "America/Vancouver", -480, "us", "Canada"}, {"Tijuana", "America/Tijuana", -480, "us", "Mexico"},
        {"Denver", "America/Denver", -420, "us", "United States"}, {"Phoenix", "America/Phoenix", -420, "", "United States"},
        {"Calgary", "America/Edmonton", -420, "us", "Canada"}, {"Chicago", "America/Chicago", -360, "us", "United States"},
        {"Dallas", "America/Chicago", -360, "us", "United States"}, {"Houston", "America/Chicago", -360, "us", "United States"},
        {"Winnipeg", "America/Winnipeg", -360, "us", "Canada"}, {"Mexico City", "America/Mexico_City", -360, "", "Mexico"},
        {"Guatemala", "America/Guatemala", -360, "", "Guatemala"}, {"Costa Rica", "America/Costa_Rica", -360, "", "Costa Rica"},
        {"New York", "America/New_York", -300, "us", "United States"}, {"Washington", "America/New_York", -300, "us", "United States"},
        {"Boston", "America/New_York", -300, "us", "United States"}, {"Miami", "America/New_York", -300, "us", "United States"},
        {"Atlanta", "America/New_York", -300, "us", "United States"}, {"Toronto", "America/Toronto", -300, "us", "Canada"},
        {"Montreal", "America/Toronto", -300, "us", "Canada"}, {"Havana", "America/Havana", -300, "us", "Cuba"},
        {"Bogota", "America/Bogota", -300, "", "Colombia"}, {"Lima", "America/Lima", -300, "", "Peru"},
        {"Panama", "America/Panama", -300, "", "Panama"}, {"Caracas", "America/Caracas", -240, "", "Venezuela"},
        {"Halifax", "America/Halifax", -240, "us", "Canada"}, {"Santiago", "America/Santiago", -240, "cl", "Chile"},
        {"La Paz", "America/La_Paz", -240, "", "Bolivia"}, {"Puerto Rico", "America/Puerto_Rico", -240, "", "United States"},
        {"St. John's", "America/St_Johns", -210, "us", "Canada"}, {"Buenos Aires", "America/Argentina/Buenos_Aires", -180, "", "Argentina"},
        {"Sao Paulo", "America/Sao_Paulo", -180, "", "Brazil"}, {"Rio de Janeiro", "America/Sao_Paulo", -180, "", "Brazil"},
        {"Montevideo", "America/Montevideo", -180, "", "Uruguay"}, {"Nuuk", "America/Nuuk", -120, "eu", "Greenland"},
        {"Cape Verde", "Atlantic/Cape_Verde", -60, "", "Cabo Verde"},
    };
    return list;
}

std::string offsetText(int minutes) {
    char buf[16];
    const int a = std::abs(minutes);
    std::snprintf(buf, sizeof buf, "%c%02d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
    return buf;
}

Place findPlace(const std::string& text) {
    Place p;
    const std::string t = lower(text);
    const std::string key = squash(text);
    if (key.empty() || key == "local" || key == "here" || key == "me") {
        p.local = true;
        p.zone.city = "here";
        return p;
    }
    // fixed offsets: UTC+5:30, GMT-3, +02:00, -0800
    {
        std::string s = t;
        for (const char* pre : {"utc", "gmt"}) {
            if (s.rfind(pre, 0) == 0) s = s.substr(3);
        }
        while (!s.empty() && s[0] == ' ') s.erase(0, 1);
        if (s.empty() && (t == "utc" || t == "gmt" || t == "z")) {
            p.zone = zones().front();
            return p;
        }
        if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
            int sign = s[0] == '-' ? -1 : 1;
            std::string digits = s.substr(1);
            digits.erase(std::remove(digits.begin(), digits.end(), ':'), digits.end());
            if (!digits.empty() && digits.size() <= 4 && std::all_of(digits.begin(), digits.end(), ::isdigit)) {
                int h = 0, m = 0;
                if (digits.size() <= 2) h = std::stoi(digits);
                else h = std::stoi(digits.substr(0, digits.size() - 2)), m = std::stoi(digits.substr(digits.size() - 2));
                if (h > 14 || m > 59) throw std::runtime_error("offset out of range: " + text);
                p.zone = {"UTC" + offsetText(sign * (h * 60 + m)), "", sign * (h * 60 + m), "", ""};
                return p;
            }
        }
    }
    struct Abbr {
        const char* name;
        int offset;
        const char* rule;
        const char* city;
    };
    static const Abbr abbrs[] = {{"est", -300, "", "New York"}, {"edt", -240, "", "New York"}, {"et", -300, "us", "New York"},
                                 {"cst", -360, "", "Chicago"}, {"cdt", -300, "", "Chicago"}, {"ct", -360, "us", "Chicago"},
                                 {"mst", -420, "", "Denver"}, {"mdt", -360, "", "Denver"}, {"mt", -420, "us", "Denver"},
                                 {"pst", -480, "", "Los Angeles"}, {"pdt", -420, "", "Los Angeles"}, {"pt", -480, "us", "Los Angeles"},
                                 {"bst", 60, "", "London"}, {"cet", 60, "", "Paris"}, {"cest", 120, "", "Paris"}, {"eet", 120, "", "Athens"},
                                 {"ist", 330, "", "Delhi"}, {"jst", 540, "", "Tokyo"}, {"kst", 540, "", "Seoul"}, {"aest", 600, "", "Sydney"},
                                 {"aedt", 660, "", "Sydney"}, {"nzst", 720, "", "Auckland"}, {"hst", -600, "", "Honolulu"}};
    for (const auto& a : abbrs) {
        if (key == a.name) {
            p.zone = {std::string(a.name), "", a.offset, a.rule, ""};
            for (auto& c : p.zone.city) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return p;
        }
    }
    for (const auto& z : zones()) {
        if (squash(z.city) == key || squash(z.id) == key || squash(z.id.substr(z.id.find('/') + 1)) == key) {
            p.zone = z;
            return p;
        }
    }
    std::string near;
    for (const auto& z : zones()) {
        if (squash(z.city).find(key) != std::string::npos || squash(z.country).find(key) != std::string::npos) {
            if (near.empty()) p.zone = z;
            near += (near.empty() ? "" : ", ") + z.city;
        }
    }
    if (!near.empty() && near.find(',') == std::string::npos) return p;  // one match on part of a name or a country
    throw std::runtime_error("don't know the place '" + text + "'" + (near.empty() ? " (clock.zones lists them, or use an offset like UTC+2)" : "; did you mean " + near + "?"));
}

std::string placeName(const Place& p) { return p.local ? "here" : p.zone.city; }

int offsetAt(const Place& p, std::int64_t utc, bool* dst) {
    if (p.local) return localOffset(utc, dst);
    const bool on = dstOn(p.zone.rule, p.zone.offset, utc);
    if (dst) *dst = on;
    return p.zone.offset + (on ? 60 : 0);
}

std::int64_t toUtc(const Civil& local, const Place& p) {
    const std::int64_t wall = unixFromCivil(local);
    std::int64_t guess = wall - offsetAt(p, wall) * 60;
    // settle on the offset in force at the result (twice handles both sides of a change)
    for (int i = 0; i < 2; ++i) guess = wall - offsetAt(p, guess) * 60;
    return guess;
}

namespace {

int monthFromName(const std::string& w) {
    static const char* names[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    const std::string l = lower(w);
    for (int i = 0; i < 12; ++i) {
        if (l.size() >= 3 && l.compare(0, 3, names[i]) == 0) return i + 1;
    }
    return 0;
}

bool parseTimeOfDay(std::string s, Civil& c) {
    s = lower(s);
    bool pm = false, am = false;
    if (s.size() > 2 && (s.substr(s.size() - 2) == "pm" || s.substr(s.size() - 2) == "am")) {
        pm = s.substr(s.size() - 2) == "pm";
        am = !pm;
        s = s.substr(0, s.size() - 2);
        while (!s.empty() && s.back() == ' ') s.pop_back();
    }
    int h = 0, m = 0, sec = 0;
    char extra = 0;
    int n = std::sscanf(s.c_str(), "%d:%d:%d%c", &h, &m, &sec, &extra);
    if (n < 2) {
        if ((am || pm) && std::sscanf(s.c_str(), "%d%c", &h, &extra) == 1) n = 1;
        else return false;
    }
    if (n == 4) return false;
    if (am || pm) {
        if (h < 1 || h > 12) return false;
        h = h % 12 + (pm ? 12 : 0);
    }
    if (h > 23 || m > 59 || sec > 60) return false;
    c.hour = h;
    c.minute = m;
    c.second = sec;
    return true;
}

}  // namespace

bool parseDateTime(const std::string& textIn, Civil& out, bool& hasDate, bool& hasTime) {
    hasDate = hasTime = false;
    std::string text = lower(textIn);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text[0] == ' ') text.erase(0, 1);
    if (text.empty()) return false;
    if (text.back() == 'z') text.pop_back();
    // the date part
    std::string rest = text;
    int y = 0, mo = 0, d = 0;
    int used = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d%n", &y, &mo, &d, &used) == 3 && y > 31) {
        hasDate = true;
        rest = text.substr(static_cast<std::size_t>(used));
    } else if (std::sscanf(text.c_str(), "%d/%d/%d%n", &y, &mo, &d, &used) == 3 && y > 31) {
        hasDate = true;
        rest = text.substr(static_cast<std::size_t>(used));
    } else {
        // words: today, tomorrow, "16 oct 2026", "oct 16, 2026", "oct 16"
        std::vector<std::string> words;
        std::string cur;
        for (char c : text + " ") {
            if (c == ' ' || c == ',') {
                if (!cur.empty()) words.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        std::size_t usedWords = 0;
        if (!words.empty() && (words[0] == "today" || words[0] == "now" || words[0] == "tomorrow" || words[0] == "yesterday")) {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            std::int64_t days = daysFromCivil(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) + (words[0] == "tomorrow" ? 1 : words[0] == "yesterday" ? -1 : 0);
            Civil c = civilFromDays(days);
            y = c.year, mo = c.month, d = c.day;
            hasDate = true;
            usedWords = 1;
            if (words[0] == "now") {
                out = {y, mo, d, tm.tm_hour, tm.tm_min, tm.tm_sec};
                hasTime = true;
                return true;
            }
        } else if (words.size() >= 2 && monthFromName(words[0]) && std::isdigit(static_cast<unsigned char>(words[1][0]))) {
            mo = monthFromName(words[0]);
            d = std::atoi(words[1].c_str());
            usedWords = 2;
            if (words.size() >= 3 && words[2].size() == 4 && std::isdigit(static_cast<unsigned char>(words[2][0]))) y = std::atoi(words[2].c_str()), usedWords = 3;
            hasDate = true;
        } else if (words.size() >= 2 && std::isdigit(static_cast<unsigned char>(words[0][0])) && monthFromName(words[1]) && words[0].find(':') == std::string::npos) {
            d = std::atoi(words[0].c_str());
            mo = monthFromName(words[1]);
            usedWords = 2;
            if (words.size() >= 3 && words[2].size() == 4 && std::isdigit(static_cast<unsigned char>(words[2][0]))) y = std::atoi(words[2].c_str()), usedWords = 3;
            hasDate = true;
        }
        if (hasDate && y == 0) {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            y = tm.tm_year + 1900;
        }
        rest.clear();
        for (std::size_t i = usedWords; i < words.size(); ++i) rest += (rest.empty() ? "" : " ") + words[i];
    }
    if (hasDate) {
        if (mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo)) return false;
        out = {y, mo, d, 0, 0, 0};
    }
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == 't' || rest[0] == ',')) rest.erase(0, 1);
    if (rest.rfind("at ", 0) == 0) rest = rest.substr(3);
    if (!rest.empty()) {
        Civil tc = out;
        if (!parseTimeOfDay(rest, tc)) return false;
        out.hour = tc.hour, out.minute = tc.minute, out.second = tc.second;
        hasTime = true;
    }
    return hasDate || hasTime;
}

double parseDuration(const std::string& textIn) {
    std::string t = lower(textIn);
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    if (t.empty()) throw std::runtime_error("no duration given");
    if (t.find(':') != std::string::npos) {  // h:mm:ss or mm:ss
        double parts[3] = {0, 0, 0};
        int n = 0;
        std::string cur;
        for (char c : t + ":") {
            if (c == ':') {
                if (n == 3) throw std::runtime_error("bad duration '" + textIn + "'");
                parts[n++] = std::atof(cur.c_str());
                cur.clear();
            } else {
                cur += c;
            }
        }
        return n == 3 ? parts[0] * 3600 + parts[1] * 60 + parts[2] : parts[0] * 60 + parts[1];
    }
    double total = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < t.size()) {
        std::size_t start = i;
        while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.')) ++i;
        if (start == i) throw std::runtime_error("bad duration '" + textIn + "' (try 90s, 5m, 1h30m or 1:30:00)");
        const double v = std::atof(t.substr(start, i - start).c_str());
        std::size_t u = i;
        while (i < t.size() && std::isalpha(static_cast<unsigned char>(t[i]))) ++i;
        const std::string unit = t.substr(u, i - u);
        double mult = 0;
        if (unit.empty() || unit == "s" || unit.rfind("sec", 0) == 0) mult = 1;
        else if (unit == "m" || unit.rfind("min", 0) == 0) mult = 60;
        else if (unit == "h" || unit.rfind("hour", 0) == 0 || unit == "hr" || unit == "hrs") mult = 3600;
        else if (unit == "d" || unit.rfind("day", 0) == 0) mult = 86400;
        else if (unit == "ms") mult = 0.001;
        else throw std::runtime_error("unknown time unit '" + unit + "'");
        total += v * mult;
        any = true;
    }
    if (!any) throw std::runtime_error("bad duration '" + textIn + "'");
    return total;
}

std::string formatDuration(double seconds, bool tenths) {
    const bool neg = seconds < 0;
    double s = std::fabs(seconds);
    if (!tenths) s = std::ceil(s - 1e-9);  // a countdown shows 0:01 until it is really over
    const long long whole = static_cast<long long>(std::floor(s + 1e-9));
    const int frac = static_cast<int>(std::floor((s - static_cast<double>(whole)) * 10 + 1e-6));
    char buf[48];
    const long long h = whole / 3600, m = whole % 3600 / 60, sec = whole % 60;
    if (h) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", h, m, sec);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", m, sec);
    std::string out = std::string(neg ? "-" : "") + buf;
    if (tenths) out += "." + std::to_string(std::clamp(frac, 0, 9));
    return out;
}

std::string calendarText(int year, int month, int mark) {
    static const char* names[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    std::string title = std::string(names[month - 1]) + " " + std::to_string(year);
    std::string out = std::string((34 - title.size()) / 2, ' ') + title + "\n  wk  Mo  Tu  We  Th  Fr  Sa  Su\n";
    const std::int64_t first = daysFromCivil(year, month, 1);
    const int lead = (weekdayOf(first) + 6) % 7;
    const int n = daysInMonth(year, month);
    int day = 1 - lead;
    while (day <= n) {
        const int anchor = std::max(day, 1);
        char wk[16];
        std::snprintf(wk, sizeof wk, "%4d", isoWeek(year, month, anchor));
        out += wk;
        for (int i = 0; i < 7; ++i, ++day) {
            char cell[16];
            if (day < 1 || day > n) std::snprintf(cell, sizeof cell, "    ");
            else if (day == mark) std::snprintf(cell, sizeof cell, "[%2d]", day);
            else std::snprintf(cell, sizeof cell, "%4d", day);
            out += cell;
        }
        out += "\n";
    }
    return out;
}

}  // namespace wetwaresuite::clock
