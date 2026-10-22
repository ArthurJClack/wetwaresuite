// Formula functions: dates and times (serial numbers in Excel's 1900 system)
// and finance (loans, investments, depreciation).
#include <algorithm>
#include <cmath>
#include <set>

#include "formula_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office::fx {

namespace {

void ymd(double serial, int& y, int& m, int& d) {
    if (serial < 0) throw EvalError{"#NUM!"};
    serialDate(serial, y, m, d);
}

int daysInMonth(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : days[m - 1];
}

// DATE() with Excel's overflow rules: month 13 is January next year, day 0 the last of the month before.
double makeDate(int y, int mo, int d) {
    if (y < 1900) y += 1900;
    y += (mo - 1) >= 0 ? (mo - 1) / 12 : -((12 - mo) / 12);
    mo = ((mo - 1) % 12 + 12) % 12 + 1;
    double s = dateSerial(y, mo, 1) + d - 1;
    if (s < 0) throw EvalError{"#NUM!"};
    return s;
}

double addMonths(double serial, int months, bool endOfMonth) {
    int y, m, d;
    ymd(serial, y, m, d);
    int total = y * 12 + (m - 1) + months;
    int ny = total / 12, nm = total % 12 + 1;
    if (ny < 1900) throw EvalError{"#NUM!"};
    const int dim = daysInMonth(ny, nm);
    return dateSerial(ny, nm, endOfMonth ? dim : std::min(d, dim));
}

int weekdayOf(double serial) {  // 0 Sunday .. 6 Saturday
    long s = static_cast<long>(std::floor(serial));
    return static_cast<int>(((s % 7) + 6) % 7);
}

// Weekend days for the .INTL variants: a code 1..17 or a "0000011" mask (Monday first).
std::set<int> weekendDays(const std::vector<NodeP>& a, std::size_t i, Ctx& c) {
    std::set<int> out;
    if (missing(a, i)) return {0, 6};
    CellValue v = evalScalar(a[i], c);
    if (v.type == CellValue::Text) {
        if (v.text.size() != 7) throw EvalError{"#VALUE!"};
        for (int k = 0; k < 7; ++k) {
            if (v.text[static_cast<std::size_t>(k)] == '1') out.insert((k + 1) % 7);
            else if (v.text[static_cast<std::size_t>(k)] != '0') throw EvalError{"#VALUE!"};
        }
        return out;
    }
    const int code = static_cast<int>(toNum(v));
    if (code >= 1 && code <= 7) return {(code + 5) % 7, (code + 6) % 7};  // 1 = Sat+Sun, 2 = Sun+Mon, ...
    if (code >= 11 && code <= 17) return {code - 11};  // one day: 11 = Sunday ... 17 = Saturday
    throw EvalError{"#NUM!"};
}

std::set<long> holidays(const std::vector<NodeP>& a, std::size_t i, Ctx& c) {
    std::set<long> out;
    if (missing(a, i)) return out;
    eachNumber(std::vector<NodeP>{a[i]}, c, [&](double d) { out.insert(static_cast<long>(std::floor(d))); });
    return out;
}

long networkDays(long start, long end, const std::set<int>& weekend, const std::set<long>& hols) {
    const int sign = start <= end ? 1 : -1;
    if (sign < 0) std::swap(start, end);
    long n = 0;
    for (long d = start; d <= end; ++d) {
        if (weekend.count(weekdayOf(static_cast<double>(d))) || hols.count(d)) continue;
        ++n;
    }
    return n * sign;
}

long workday(long start, long days, const std::set<int>& weekend, const std::set<long>& hols) {
    if (weekend.size() >= 7) throw EvalError{"#VALUE!"};
    long d = start;
    const int step = days >= 0 ? 1 : -1;
    for (long left = std::abs(days); left > 0;) {
        d += step;
        if (weekend.count(weekdayOf(static_cast<double>(d))) || hols.count(d)) continue;
        --left;
    }
    return d;
}

double days360(double a, double b, bool european) {
    int y1, m1, d1, y2, m2, d2;
    ymd(a, y1, m1, d1);
    ymd(b, y2, m2, d2);
    if (european) {
        d1 = std::min(d1, 30);
        d2 = std::min(d2, 30);
    } else {
        const bool lastFeb1 = m1 == 2 && d1 == daysInMonth(y1, 2);
        if (d1 == 31 || lastFeb1) d1 = 30;
        if (d2 == 31 && d1 >= 30) d2 = 30;
    }
    return (y2 - y1) * 360.0 + (m2 - m1) * 30.0 + (d2 - d1);
}

// "2026-03-01", "3/1/2026", "1 Mar 2026", "March 1, 2026" -> serial
bool parseDateText(const std::string& t, double& out) {
    int y, m, d;
    std::string s = trimCopy(t);
    if (parseIsoDate(s, y, m, d)) {
        out = dateSerial(y, m, d);
        return true;
    }
    char sep1, sep2;
    int n = 0;
    if (std::sscanf(s.c_str(), "%d%c%d%c%d%n", &m, &sep1, &d, &sep2, &y, &n) == 5 && sep1 == sep2 && (sep1 == '/' || sep1 == '-' || sep1 == '.') &&
        static_cast<std::size_t>(n) == s.size()) {
        if (y < 100) y += y < 30 ? 2000 : 1900;
        if (m > 12 && d <= 12) std::swap(m, d);  // 25/12/2026
        if (m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m)) return false;
        out = dateSerial(y, m, d);
        return true;
    }
    static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    std::string low = lower(s);
    for (char& ch : low) {
        if (ch == ',' || ch == '-' || ch == '/') ch = ' ';
    }
    char word[32] = {0};
    int a1 = 0, a2 = 0;
    if (std::sscanf(low.c_str(), "%d %31s %d", &a1, word, &a2) == 3 || std::sscanf(low.c_str(), "%31s %d %d", word, &a1, &a2) == 3) {
        for (int k = 0; k < 12; ++k) {
            if (std::string(word).compare(0, 3, months[k]) == 0) {
                y = a2 < 100 ? a2 + 2000 : a2;
                d = a1;
                if (d < 1 || d > daysInMonth(y, k + 1)) return false;
                out = dateSerial(y, k + 1, d);
                return true;
            }
        }
    }
    return false;
}

bool parseTimeText(const std::string& t, double& out) {
    int h = 0, mi = 0, s = 0;
    char ap[3] = {0};
    std::string low = lower(trimCopy(t));
    // a date in front is ignored
    auto sp = low.rfind(' ');
    if (sp != std::string::npos && low.find(':') != std::string::npos && low.find(':') < sp && low.substr(sp + 1) != "am" && low.substr(sp + 1) != "pm") {
        low = low.substr(sp + 1);
    }
    const int n = std::sscanf(low.c_str(), "%d:%d:%d", &h, &mi, &s);
    if (n < 2) return false;
    if (low.find("pm") != std::string::npos && h < 12) h += 12;
    if (low.find("am") != std::string::npos && h == 12) h = 0;
    (void)ap;
    if (h < 0 || mi < 0 || mi > 59 || s < 0 || s > 59) return false;
    out = std::fmod((h * 3600 + mi * 60 + s) / 86400.0, 1.0);
    return true;
}

double dateArg(const NodeP& n, Ctx& c) {
    CellValue v = evalScalar(n, c);
    if (v.type == CellValue::Text) {
        double d;
        if (parseDateText(v.text, d)) return d;
    }
    double d = toNum(v);
    if (d < 0) throw EvalError{"#NUM!"};
    return d;
}

void addDates(FnMap& m) {
    m["TODAY"] = [](const auto& a, Ctx&) {
        need(a, 0, 0);
        return num(std::floor(nowSerial()));
    };
    m["NOW"] = [](const auto& a, Ctx&) {
        need(a, 0, 0);
        return num(nowSerial());
    };
    m["DATE"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        return num(makeDate(evalInt(a[0], c), evalInt(a[1], c), evalInt(a[2], c)));
    };
    m["TIME"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double h = std::trunc(evalNum(a[0], c)), mi = std::trunc(evalNum(a[1], c)), s = std::trunc(evalNum(a[2], c));
        const double t = (h * 3600 + mi * 60 + s) / 86400.0;
        if (t < 0) throw EvalError{"#NUM!"};
        return num(std::fmod(t, 1.0));
    };
    auto part = [](int which) {
        return [which](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 1);
            const double s = dateArg(a[0], c);
            if (which < 3) {
                int y, mo, d;
                ymd(s, y, mo, d);
                return num(which == 0 ? y : which == 1 ? mo : d);
            }
            long secs = std::lround((s - std::floor(s)) * 86400);
            if (secs >= 86400) secs = 0;
            return num(static_cast<double>(which == 3 ? secs / 3600 : which == 4 ? (secs / 60) % 60 : secs % 60));
        };
    };
    m["YEAR"] = part(0);
    m["MONTH"] = part(1);
    m["DAY"] = part(2);
    m["HOUR"] = part(3);
    m["MINUTE"] = part(4);
    m["SECOND"] = part(5);
    m["WEEKDAY"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const int wd = weekdayOf(dateArg(a[0], c));  // 0 = Sunday
        const int type = missing(a, 1) ? 1 : evalInt(a[1], c);
        switch (type) {
            case 1: case 17: return num(wd + 1);
            case 2: case 11: return num((wd + 6) % 7 + 1);
            case 3: return num((wd + 6) % 7);
            case 12: return num((wd + 5) % 7 + 1);
            case 13: return num((wd + 4) % 7 + 1);
            case 14: return num((wd + 3) % 7 + 1);
            case 15: return num((wd + 2) % 7 + 1);
            case 16: return num((wd + 1) % 7 + 1);
            default: throw EvalError{"#NUM!"};
        }
    };
    m["ISOWEEKNUM"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const double s = std::floor(dateArg(a[0], c));
        const int wd = (weekdayOf(s) + 6) % 7;  // Monday 0
        const double thursday = s - wd + 3;
        int y, mo, d;
        ymd(thursday, y, mo, d);
        const double jan1 = dateSerial(y, 1, 1);
        return num(std::floor((thursday - jan1) / 7) + 1);
    };
    m["WEEKNUM"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double s = std::floor(dateArg(a[0], c));
        const int type = missing(a, 1) ? 1 : evalInt(a[1], c);
        if (type == 21) {
            std::vector<NodeP> one{a[0]};
            return callFunction("ISOWEEKNUM", one, c);
        }
        int start;  // weekday the week starts on, 0 = Sunday
        if (type == 1 || type == 17) start = 0;
        else if (type == 2 || type == 11) start = 1;
        else if (type >= 12 && type <= 16) start = type - 10;
        else throw EvalError{"#NUM!"};
        int y, mo, d;
        ymd(s, y, mo, d);
        const double jan1 = dateSerial(y, 1, 1);
        const int offset = (weekdayOf(jan1) - start + 7) % 7;
        return num(std::floor((s - jan1 + offset) / 7) + 1);
    };
    m["EDATE"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        return num(addMonths(dateArg(a[0], c), evalInt(a[1], c), false));
    };
    m["EOMONTH"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        return num(addMonths(dateArg(a[0], c), evalInt(a[1], c), true));
    };
    m["DAYS"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        return num(std::floor(dateArg(a[0], c)) - std::floor(dateArg(a[1], c)));
    };
    m["DAYS360"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        return num(days360(dateArg(a[0], c), dateArg(a[1], c), !missing(a, 2) && evalBool(a[2], c)));
    };
    m["DATEDIF"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double s = std::floor(dateArg(a[0], c)), e = std::floor(dateArg(a[1], c));
        if (s > e) throw EvalError{"#NUM!"};
        const std::string unit = upper(evalStr(a[2], c));
        int y1, m1, d1, y2, m2, d2;
        ymd(s, y1, m1, d1);
        ymd(e, y2, m2, d2);
        int months = (y2 - y1) * 12 + (m2 - m1) - (d2 < d1 ? 1 : 0);
        if (unit == "D") return num(e - s);
        if (unit == "M") return num(months);
        if (unit == "Y") return num(months / 12);
        if (unit == "YM") return num(months % 12);
        if (unit == "MD") {
            int dd = d2 - d1;
            if (dd < 0) {
                const int pm = m2 == 1 ? 12 : m2 - 1, py = m2 == 1 ? y2 - 1 : y2;
                dd += daysInMonth(py, pm);
            }
            return num(dd);
        }
        if (unit == "YD") {
            double anniversary = dateSerial(y2, m1, std::min(d1, daysInMonth(y2, m1)));
            if (anniversary > e) anniversary = dateSerial(y2 - 1, m1, std::min(d1, daysInMonth(y2 - 1, m1)));
            return num(e - anniversary);
        }
        throw EvalError{"#NUM!"};
    };
    m["NETWORKDAYS"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        return num(static_cast<double>(networkDays(static_cast<long>(dateArg(a[0], c)), static_cast<long>(dateArg(a[1], c)), {0, 6}, holidays(a, 2, c))));
    };
    m["NETWORKDAYS.INTL"] = [](const auto& a, Ctx& c) {
        need(a, 2, 4);
        return num(static_cast<double>(
            networkDays(static_cast<long>(dateArg(a[0], c)), static_cast<long>(dateArg(a[1], c)), weekendDays(a, 2, c), holidays(a, 3, c))));
    };
    m["WORKDAY"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        return num(static_cast<double>(workday(static_cast<long>(dateArg(a[0], c)), static_cast<long>(evalNum(a[1], c)), {0, 6}, holidays(a, 2, c))));
    };
    m["WORKDAY.INTL"] = [](const auto& a, Ctx& c) {
        need(a, 2, 4);
        return num(static_cast<double>(workday(static_cast<long>(dateArg(a[0], c)), static_cast<long>(evalNum(a[1], c)), weekendDays(a, 2, c), holidays(a, 3, c))));
    };
    m["YEARFRAC"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        double s = std::floor(dateArg(a[0], c)), e = std::floor(dateArg(a[1], c));
        if (s > e) std::swap(s, e);
        const int basis = missing(a, 2) ? 0 : evalInt(a[2], c);
        switch (basis) {
            case 0: return num(days360(s, e, false) / 360);
            case 1: {
                int y1, m1, d1, y2, m2, d2;
                ymd(s, y1, m1, d1);
                ymd(e, y2, m2, d2);
                double yearDays = 0;
                for (int y = y1; y <= y2; ++y) yearDays += dateSerial(y + 1, 1, 1) - dateSerial(y, 1, 1);
                return num((e - s) / (yearDays / (y2 - y1 + 1)));
            }
            case 2: return num((e - s) / 360);
            case 3: return num((e - s) / 365);
            case 4: return num(days360(s, e, true) / 360);
            default: throw EvalError{"#NUM!"};
        }
    };
    m["DATEVALUE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        double d;
        if (!parseDateText(evalStr(a[0], c), d)) throw EvalError{"#VALUE!"};
        return num(d);
    };
    m["TIMEVALUE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        double t;
        if (!parseTimeText(evalStr(a[0], c), t)) throw EvalError{"#VALUE!"};
        return num(t);
    };
}

// ---- finance -----------------------------------------------------------------

// Future value of a loan/annuity; type 1 pays at the start of each period.
double fvOf(double rate, double nper, double pmt, double pv, int type) {
    if (rate == 0) return -(pv + pmt * nper);
    const double g = std::pow(1 + rate, nper);
    return -(pv * g + pmt * (1 + rate * type) * (g - 1) / rate);
}

double pmtOf(double rate, double nper, double pv, double fv, int type) {
    if (nper == 0) throw EvalError{"#NUM!"};
    if (rate == 0) return -(pv + fv) / nper;
    const double g = std::pow(1 + rate, nper);
    return -(fv + pv * g) * rate / ((1 + rate * type) * (g - 1));
}

double ipmtOf(double rate, double per, double nper, double pv, double fv, int type) {
    if (per < 1 || per > nper) throw EvalError{"#NUM!"};
    const double pmt = pmtOf(rate, nper, pv, fv, type);
    if (type == 1 && per == 1) return 0;
    // balance at the start of the period, times the rate
    const double balance = -fvOf(rate, per - 1, pmt, pv, type);
    double ip = balance * rate;
    if (type == 1) ip /= (1 + rate);
    return -ip;
}

double npvOf(double rate, const std::vector<double>& flows) {
    double s = 0, f = 1;
    for (double v : flows) {
        f *= 1 + rate;
        s += v / f;
    }
    return s;
}

// Newton's method with a bisection fallback.
template <typename F>
double solve(F f, double guess) {
    double x = guess;
    for (int i = 0; i < 100; ++i) {
        const double y = f(x);
        if (std::abs(y) < 1e-10) return x;
        const double h = std::max(1e-7, std::abs(x) * 1e-7);
        const double d = (f(x + h) - y) / h;
        if (d == 0 || !std::isfinite(d)) break;
        double nx = x - y / d;
        if (nx <= -1) nx = (x - 1) / 2;
        if (std::abs(nx - x) < 1e-12) return nx;
        x = nx;
    }
    // bracket search over a wide range
    double lo = -0.9999, hi = 10;
    double flo = f(lo), fhi = f(hi);
    if (!std::isfinite(flo) || !std::isfinite(fhi) || flo * fhi > 0) throw EvalError{"#NUM!"};
    for (int i = 0; i < 300; ++i) {
        const double mid = (lo + hi) / 2, fm = f(mid);
        if (std::abs(fm) < 1e-10) return mid;
        if (flo * fm < 0) {
            hi = mid;
        } else {
            lo = mid;
            flo = fm;
        }
    }
    return (lo + hi) / 2;
}

int typeArg(const std::vector<NodeP>& a, std::size_t i, Ctx& c) { return missing(a, i) ? 0 : (evalNum(a[i], c) != 0 ? 1 : 0); }
double optNum(const std::vector<NodeP>& a, std::size_t i, Ctx& c, double fallback) { return missing(a, i) ? fallback : evalNum(a[i], c); }

void addFinance(FnMap& m) {
    m["PMT"] = [](const auto& a, Ctx& c) {
        need(a, 3, 5);
        return num(pmtOf(evalNum(a[0], c), evalNum(a[1], c), evalNum(a[2], c), optNum(a, 3, c, 0), typeArg(a, 4, c)));
    };
    m["FV"] = [](const auto& a, Ctx& c) {
        need(a, 3, 5);
        return num(fvOf(evalNum(a[0], c), evalNum(a[1], c), evalNum(a[2], c), optNum(a, 3, c, 0), typeArg(a, 4, c)));
    };
    m["PV"] = [](const auto& a, Ctx& c) {
        need(a, 3, 5);
        const double rate = evalNum(a[0], c), nper = evalNum(a[1], c), pmt = evalNum(a[2], c), fv = optNum(a, 3, c, 0);
        const int type = typeArg(a, 4, c);
        if (rate == 0) return num(-(fv + pmt * nper));
        const double g = std::pow(1 + rate, nper);
        return num(-(fv + pmt * (1 + rate * type) * (g - 1) / rate) / g);
    };
    m["NPER"] = [](const auto& a, Ctx& c) {
        need(a, 3, 5);
        const double rate = evalNum(a[0], c), pmt = evalNum(a[1], c), pv = evalNum(a[2], c), fv = optNum(a, 3, c, 0);
        const int type = typeArg(a, 4, c);
        if (rate == 0) {
            if (pmt == 0) throw EvalError{"#NUM!"};
            return num(-(pv + fv) / pmt);
        }
        const double k = pmt * (1 + rate * type) / rate;
        const double x = (k - fv) / (k + pv);
        if (x <= 0) throw EvalError{"#NUM!"};
        return num(std::log(x) / std::log(1 + rate));
    };
    m["RATE"] = [](const auto& a, Ctx& c) {
        need(a, 3, 6);
        const double nper = evalNum(a[0], c), pmt = evalNum(a[1], c), pv = evalNum(a[2], c), fv = optNum(a, 3, c, 0);
        const int type = typeArg(a, 4, c);
        const double guess = optNum(a, 5, c, 0.1);
        return num(solve([&](double r) { return fvOf(r, nper, pmt, pv, type) - fv; }, guess));
    };
    m["IPMT"] = [](const auto& a, Ctx& c) {
        need(a, 4, 6);
        return num(ipmtOf(evalNum(a[0], c), evalNum(a[1], c), evalNum(a[2], c), evalNum(a[3], c), optNum(a, 4, c, 0), typeArg(a, 5, c)));
    };
    m["PPMT"] = [](const auto& a, Ctx& c) {
        need(a, 4, 6);
        const double rate = evalNum(a[0], c), per = evalNum(a[1], c), nper = evalNum(a[2], c), pv = evalNum(a[3], c), fv = optNum(a, 4, c, 0);
        const int type = typeArg(a, 5, c);
        return num(pmtOf(rate, nper, pv, fv, type) - ipmtOf(rate, per, nper, pv, fv, type));
    };
    auto cumulative = [](bool interest) {
        return [interest](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 6, 6);
            const double rate = evalNum(a[0], c), nper = evalNum(a[1], c), pv = evalNum(a[2], c);
            const int start = evalInt(a[3], c), end = evalInt(a[4], c), type = evalInt(a[5], c);
            if (rate <= 0 || nper <= 0 || pv <= 0 || start < 1 || end < start || end > nper || (type != 0 && type != 1)) throw EvalError{"#NUM!"};
            double s = 0;
            for (int p = start; p <= end; ++p) {
                const double ip = ipmtOf(rate, p, nper, pv, 0, type);
                s += interest ? ip : pmtOf(rate, nper, pv, 0, type) - ip;
            }
            return num(s);
        };
    };
    m["CUMIPMT"] = cumulative(true);
    m["CUMPRINC"] = cumulative(false);
    m["NPV"] = [](const auto& a, Ctx& c) {
        if (a.size() < 2) throw EvalError{"#VALUE!"};
        const double rate = evalNum(a[0], c);
        std::vector<double> flows = numbers(std::vector<NodeP>(a.begin() + 1, a.end()), c);
        return num(npvOf(rate, flows));
    };
    m["IRR"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        std::vector<double> flows = numbers(std::vector<NodeP>{a[0]}, c);
        bool pos = false, neg = false;
        for (double v : flows) {
            pos = pos || v > 0;
            neg = neg || v < 0;
        }
        if (!pos || !neg) throw EvalError{"#NUM!"};
        const double guess = optNum(a, 1, c, 0.1);
        return num(solve(
            [&](double r) {
                double s = 0, f = 1;
                for (double v : flows) {
                    s += v / f;
                    f *= 1 + r;
                }
                return s;
            },
            guess));
    };
    m["MIRR"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        std::vector<double> flows = numbers(std::vector<NodeP>{a[0]}, c);
        const double fin = evalNum(a[1], c), re = evalNum(a[2], c);
        const double n = static_cast<double>(flows.size());
        double pvNeg = 0, fvPos = 0;
        for (std::size_t i = 0; i < flows.size(); ++i) {
            if (flows[i] < 0) pvNeg += flows[i] / std::pow(1 + fin, static_cast<double>(i));
            else fvPos += flows[i] * std::pow(1 + re, n - 1 - static_cast<double>(i));
        }
        if (pvNeg == 0 || fvPos == 0) throw EvalError{"#DIV/0!"};
        return num(std::pow(-fvPos / pvNeg, 1 / (n - 1)) - 1);
    };
    auto xflows = [](const std::vector<NodeP>& a, Ctx& c, std::size_t vi, std::vector<double>& vals, std::vector<double>& dates) {
        Matrix v = toMatrix(evalArg(a[vi], c), c), d = toMatrix(evalArg(a[vi + 1], c), c);
        if (v.v.size() != d.v.size() || v.v.empty()) throw EvalError{"#NUM!"};
        for (std::size_t i = 0; i < v.v.size(); ++i) {
            vals.push_back(toNum(v.v[i]));
            dates.push_back(std::floor(toNum(d.v[i])));
        }
    };
    m["XNPV"] = [xflows](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double rate = evalNum(a[0], c);
        std::vector<double> vals, dates;
        xflows(a, c, 1, vals, dates);
        double s = 0;
        for (std::size_t i = 0; i < vals.size(); ++i) s += vals[i] / std::pow(1 + rate, (dates[i] - dates[0]) / 365);
        return num(s);
    };
    m["XIRR"] = [xflows](const auto& a, Ctx& c) {
        need(a, 2, 3);
        std::vector<double> vals, dates;
        xflows(a, c, 0, vals, dates);
        const double guess = optNum(a, 2, c, 0.1);
        return num(solve(
            [&](double r) {
                double s = 0;
                for (std::size_t i = 0; i < vals.size(); ++i) s += vals[i] / std::pow(1 + r, (dates[i] - dates[0]) / 365);
                return s;
            },
            guess));
    };
    m["EFFECT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double r = evalNum(a[0], c), n = std::trunc(evalNum(a[1], c));
        if (r <= 0 || n < 1) throw EvalError{"#NUM!"};
        return num(std::pow(1 + r / n, n) - 1);
    };
    m["NOMINAL"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double r = evalNum(a[0], c), n = std::trunc(evalNum(a[1], c));
        if (r <= 0 || n < 1) throw EvalError{"#NUM!"};
        return num(n * (std::pow(1 + r, 1 / n) - 1));
    };
    m["RRI"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double n = evalNum(a[0], c), pv = evalNum(a[1], c), fv = evalNum(a[2], c);
        if (n <= 0 || pv == 0) throw EvalError{"#NUM!"};
        return num(std::pow(fv / pv, 1 / n) - 1);
    };
    m["PDURATION"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double r = evalNum(a[0], c), pv = evalNum(a[1], c), fv = evalNum(a[2], c);
        if (r <= 0 || pv <= 0 || fv <= 0) throw EvalError{"#NUM!"};
        return num((std::log(fv) - std::log(pv)) / std::log(1 + r));
    };
    // depreciation
    m["SLN"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double life = evalNum(a[2], c);
        if (life == 0) throw EvalError{"#DIV/0!"};
        return num((evalNum(a[0], c) - evalNum(a[1], c)) / life);
    };
    m["SYD"] = [](const auto& a, Ctx& c) {
        need(a, 4, 4);
        const double cost = evalNum(a[0], c), salvage = evalNum(a[1], c), life = evalNum(a[2], c), per = evalNum(a[3], c);
        if (life <= 0 || per < 1 || per > life) throw EvalError{"#NUM!"};
        return num((cost - salvage) * (life - per + 1) * 2 / (life * (life + 1)));
    };
    m["DDB"] = [](const auto& a, Ctx& c) {
        need(a, 4, 5);
        const double cost = evalNum(a[0], c), salvage = evalNum(a[1], c), life = evalNum(a[2], c), per = evalNum(a[3], c);
        const double factor = optNum(a, 4, c, 2);
        if (life <= 0 || per < 1 || per > life || factor <= 0) throw EvalError{"#NUM!"};
        double book = cost, dep = 0;
        for (int p = 1; p <= static_cast<int>(std::ceil(per)); ++p) {
            dep = std::min(book * factor / life, std::max(0.0, book - salvage));
            book -= dep;
        }
        return num(dep);
    };
    m["DB"] = [](const auto& a, Ctx& c) {
        need(a, 4, 5);
        const double cost = evalNum(a[0], c), salvage = evalNum(a[1], c), life = evalNum(a[2], c), per = evalNum(a[3], c);
        const double month = optNum(a, 4, c, 12);
        if (life <= 0 || per < 1 || per > life + 1 || cost <= 0) throw EvalError{"#NUM!"};
        const double rate = std::round((1 - std::pow(salvage / cost, 1 / life)) * 1000) / 1000;
        double book = cost, dep = 0;
        for (int p = 1; p <= static_cast<int>(per); ++p) {
            if (p == 1) dep = cost * rate * month / 12;
            else if (p == static_cast<int>(life) + 1) dep = book * rate * (12 - month) / 12;
            else dep = book * rate;
            book -= dep;
        }
        return num(dep);
    };
}

}  // namespace

void addDateFunctions(FnMap& m) { addDates(m); }
void addFinanceFunctions(FnMap& m) { addFinance(m); }

}  // namespace wetwaresuite::office::fx
