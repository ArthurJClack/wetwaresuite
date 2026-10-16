// Workbook model: cells, A1 names, typed input, number formats, dates and
// structural edits (insert/delete rows and columns, sheets) that keep every
// formula pointing at the same cells.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "achclwss/office/sheet.hpp"
#include "text.hpp"

namespace achclwss::office {

// ---- values --------------------------------------------------------------------

CellValue CellValue::num(double v) {
    CellValue c;
    c.type = Number;
    c.number = v;
    return c;
}
CellValue CellValue::str(std::string s) {
    CellValue c;
    c.type = Text;
    c.text = std::move(s);
    return c;
}
CellValue CellValue::boolean(bool b) {
    CellValue c;
    c.type = Bool;
    c.number = b ? 1 : 0;
    return c;
}
CellValue CellValue::error(std::string code) {
    CellValue c;
    c.type = Error;
    c.text = std::move(code);
    return c;
}
bool CellValue::operator==(const CellValue& o) const {
    if (type != o.type) return false;
    if (type == Number || type == Bool) return number == o.number;
    return text == o.text;
}

// ---- sheets ----------------------------------------------------------------------

const Cell* Sheet::get(int row, int col) const {
    auto it = cells.find(key(row, col));
    return it == cells.end() ? nullptr : &it->second;
}

Cell& Sheet::at(int row, int col) { return cells[key(row, col)]; }

void Sheet::erase(int row, int col) { cells.erase(key(row, col)); }

bool Sheet::bounds(int& r1, int& c1, int& r2, int& c2) const {
    bool any = false;
    for (const auto& [k, c] : cells) {
        if (c.value.empty() && !c.hasFormula()) continue;
        const int r = rowOf(k), col = colOf(k);
        if (!any) {
            r1 = r2 = r;
            c1 = c2 = col;
            any = true;
        } else {
            r1 = std::min(r1, r);
            r2 = std::max(r2, r);
            c1 = std::min(c1, col);
            c2 = std::max(c2, col);
        }
    }
    return any;
}

int Workbook::sheetIndex(const std::string& name) const {
    const std::string want = lower(name);
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        if (lower(sheets[i].name) == want) return static_cast<int>(i);
    }
    return -1;
}

// ---- A1 ----------------------------------------------------------------------------

std::string colName(int col) {
    std::string s;
    int c = col + 1;
    while (c > 0) {
        int r = (c - 1) % 26;
        s.insert(s.begin(), static_cast<char>('A' + r));
        c = (c - 1) / 26;
    }
    return s;
}

int colIndex(const std::string& letters) {
    if (letters.empty() || letters.size() > 3) return -1;
    int v = 0;
    for (char ch : letters) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (c < 'A' || c > 'Z') return -1;
        v = v * 26 + (c - 'A' + 1);
    }
    return v - 1 < kMaxCols ? v - 1 : -1;
}

std::string cellName(int row, int col) { return colName(col) + std::to_string(row + 1); }

std::string rangeName(const Range& r) {
    if (r.r1 == r.r2 && r.c1 == r.c2) return cellName(r.r1, r.c1);
    if (r.r1 == 0 && r.r2 == kMaxRows - 1) return colName(r.c1) + ":" + colName(r.c2);
    return cellName(r.r1, r.c1) + ":" + cellName(r.r2, r.c2);
}

std::string quoteSheet(const std::string& name) {
    bool plain = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_');
    for (char c : name) plain = plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.');
    // names that look like a cell (e.g. "AB12") also need quotes
    if (plain) {
        std::size_t i = 0;
        while (i < name.size() && std::isalpha(static_cast<unsigned char>(name[i]))) ++i;
        bool digits = i < name.size() && i <= 3;
        for (std::size_t j = i; j < name.size(); ++j) digits = digits && std::isdigit(static_cast<unsigned char>(name[j]));
        if (digits) plain = false;
        if (upper(name) == "TRUE" || upper(name) == "FALSE") plain = false;
    }
    if (plain) return name;
    std::string out = "'";
    for (char c : name) {
        out += c;
        if (c == '\'') out += '\'';
    }
    return out + "'";
}

// ---- dates -------------------------------------------------------------------------

namespace {

long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civilFromDays(long z, int& y, int& m, int& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yoe + era * 400 + (m <= 2));
}

const long kEpoch = daysFromCivil(1899, 12, 30);

}  // namespace

double dateSerial(int year, int month, int day) {
    double s = static_cast<double>(daysFromCivil(year, month, day) - kEpoch);
    if (s < 61) s -= 1;  // Excel counts a 29 Feb 1900 that never was
    return s;
}

void serialDate(double serial, int& y, int& m, int& d) {
    long s = static_cast<long>(std::floor(serial));
    if (s == 60) {
        y = 1900;
        m = 2;
        d = 29;
        return;
    }
    if (s < 60) s += 1;
    civilFromDays(s + kEpoch, y, m, d);
}

// ---- number formats ------------------------------------------------------------------

std::string generalNumber(double v) {
    if (!std::isfinite(v)) return "#NUM!";
    if (v == 0) return "0";
    const double a = std::abs(v);
    char buf[48];
    if (a >= 1e11 || a < 1e-9) {
        std::snprintf(buf, sizeof buf, "%.5E", v);
        std::string s = buf;  // 1.23457E+11 -> trim mantissa zeros
        auto e = s.find('E');
        std::string mant = s.substr(0, e), ex = s.substr(e);
        while (mant.find('.') != std::string::npos && (mant.back() == '0' || mant.back() == '.')) {
            bool dot = mant.back() == '.';
            mant.pop_back();
            if (dot) break;
        }
        return mant + ex;
    }
    if (a == std::floor(a)) {
        std::snprintf(buf, sizeof buf, "%.0f", v);
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

namespace {

// Splits a format code into its ';' sections, ignoring ';' inside quotes.
std::vector<std::string> sections(const std::string& fmt) {
    std::vector<std::string> out(1);
    bool q = false;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c == '"') q = !q;
        if (c == '\\' && i + 1 < fmt.size()) {
            out.back() += c;
            out.back() += fmt[++i];
            continue;
        }
        if (c == ';' && !q) {
            out.emplace_back();
            continue;
        }
        out.back() += c;
    }
    return out;
}

// Format code with quoted text, escapes and [..] blocks removed: what's left decides the kind.
std::string bare(const std::string& fmt) {
    std::string out;
    bool q = false;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (c == '"') {
            q = !q;
            continue;
        }
        if (q) continue;
        if (c == '\\' || c == '_' || c == '*') {
            ++i;
            continue;
        }
        if (c == '[') {
            auto e = fmt.find(']', i);
            if (e == std::string::npos) break;
            i = e;
            continue;
        }
        out += c;
    }
    return out;
}

// Literal text of a format fragment: quotes, escapes, [$€-407] currency, _x padding.
std::string literal(const std::string& frag) {
    std::string out;
    bool q = false;
    for (std::size_t i = 0; i < frag.size(); ++i) {
        char c = frag[i];
        if (c == '"') {
            q = !q;
            continue;
        }
        if (q) {
            out += c;
            continue;
        }
        if (c == '\\' && i + 1 < frag.size()) {
            out += frag[++i];
        } else if (c == '_' && i + 1 < frag.size()) {
            ++i;
            out += ' ';
        } else if (c == '*' && i + 1 < frag.size()) {
            ++i;
        } else if (c == '[') {
            auto e = frag.find(']', i);
            if (e == std::string::npos) break;
            std::string inner = frag.substr(i + 1, e - i - 1);
            if (!inner.empty() && inner[0] == '$') {
                auto dash = inner.find('-');
                out += inner.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
            }
            i = e;
        } else if (c == '%') {
            out += '%';
        } else {
            out += c;
        }
    }
    return out;
}

std::string groupThousands(const std::string& digits) {
    std::string out;
    int n = 0;
    for (std::size_t i = digits.size(); i-- > 0;) {
        out.insert(out.begin(), digits[i]);
        if (++n % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return out;
}

const char* kMonths[] = {"January", "February", "March",     "April",   "May",      "June",
                         "July",    "August",   "September", "October", "November", "December"};
const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

std::string formatDate(double serial, const std::string& fmt) {
    int y, m, d;
    serialDate(serial, y, m, d);
    double frac = serial - std::floor(serial);
    long secs = std::lround(frac * 86400);
    if (secs >= 86400) secs = 86399;
    const int hh = static_cast<int>(secs / 3600), mi = static_cast<int>((secs / 60) % 60), ss = static_cast<int>(secs % 60);
    const int weekday = static_cast<int>(((static_cast<long>(std::floor(serial)) % 7) + 6) % 7);  // serial 1 = Sunday
    std::string lowerFmt = lower(fmt);
    const bool ampm = lowerFmt.find("am/pm") != std::string::npos || lowerFmt.find("a/p") != std::string::npos;
    std::string out;
    bool q = false;
    bool lastWasHour = false;
    for (std::size_t i = 0; i < fmt.size();) {
        char c = fmt[i];
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '"') {
            q = !q;
            ++i;
            continue;
        }
        if (q) {
            out += c;
            ++i;
            continue;
        }
        if (c == '\\' && i + 1 < fmt.size()) {
            out += fmt[i + 1];
            i += 2;
            continue;
        }
        if (c == '[') {
            auto e = fmt.find(']', i);
            i = e == std::string::npos ? fmt.size() : e + 1;
            continue;
        }
        std::size_t run = 1;
        while (i + run < fmt.size() && std::tolower(static_cast<unsigned char>(fmt[i + run])) == lc) ++run;
        char buf[16];
        if (lc == 'y') {
            if (run >= 3) std::snprintf(buf, sizeof buf, "%04d", y);
            else std::snprintf(buf, sizeof buf, "%02d", y % 100);
            out += buf;
            lastWasHour = false;
        } else if (lc == 'm') {
            // minutes when right after hours or right before seconds
            std::size_t k = i + run;
            while (k < fmt.size() && (fmt[k] == ':' || fmt[k] == ' ')) ++k;
            const bool minutes = lastWasHour || (k < fmt.size() && std::tolower(static_cast<unsigned char>(fmt[k])) == 's');
            if (minutes) {
                std::snprintf(buf, sizeof buf, run >= 2 ? "%02d" : "%d", mi);
                out += buf;
            } else if (run >= 4) {
                out += kMonths[m - 1];
            } else if (run == 3) {
                out += std::string(kMonths[m - 1]).substr(0, 3);
            } else {
                std::snprintf(buf, sizeof buf, run == 2 ? "%02d" : "%d", m);
                out += buf;
            }
            lastWasHour = false;
        } else if (lc == 'd') {
            if (run >= 4) out += kDays[weekday];
            else if (run == 3) out += std::string(kDays[weekday]).substr(0, 3);
            else {
                std::snprintf(buf, sizeof buf, run == 2 ? "%02d" : "%d", d);
                out += buf;
            }
            lastWasHour = false;
        } else if (lc == 'h') {
            int h = hh;
            if (ampm) {
                h = hh % 12;
                if (h == 0) h = 12;
            }
            std::snprintf(buf, sizeof buf, run >= 2 ? "%02d" : "%d", h);
            out += buf;
            lastWasHour = true;
        } else if (lc == 's') {
            std::snprintf(buf, sizeof buf, run >= 2 ? "%02d" : "%d", ss);
            out += buf;
            lastWasHour = false;
        } else if (lc == 'a' && lower(fmt.substr(i, 5)) == "am/pm") {
            out += hh < 12 ? "AM" : "PM";
            i += 5;
            continue;
        } else if (lc == 'a' && lower(fmt.substr(i, 3)) == "a/p") {
            out += hh < 12 ? "A" : "P";
            i += 3;
            continue;
        } else {
            out.append(run, c);
            if (c != ':' && c != ' ') lastWasHour = false;
        }
        i += run;
    }
    return out;
}

std::string formatNumber(double v, const std::string& sec, bool negativeSign) {
    // locate the digit placeholders
    std::size_t first = std::string::npos, last = std::string::npos;
    bool q = false;
    for (std::size_t i = 0; i < sec.size(); ++i) {
        char c = sec[i];
        if (c == '"') q = !q;
        if (q) continue;
        if (c == '\\' || c == '_' || c == '*') {
            ++i;
            continue;
        }
        if (c == '[') {
            auto e = sec.find(']', i);
            if (e == std::string::npos) break;
            i = e;
            continue;
        }
        if (c == '0' || c == '#' || c == '?') {
            if (first == std::string::npos) first = i;
            last = i;
        } else if ((c == '.' || c == ',') && first != std::string::npos) {
            last = i;
        } else if ((c == 'E' || c == 'e') && first != std::string::npos && i + 1 < sec.size() && (sec[i + 1] == '+' || sec[i + 1] == '-')) {
            std::size_t k = i + 2;
            while (k < sec.size() && (sec[k] == '0' || sec[k] == '#')) ++k;
            last = k - 1;
            i = last;
        }
    }
    if (first == std::string::npos) {
        std::string lit = literal(sec);
        if (lower(lit).find("general") != std::string::npos) {
            std::string g = generalNumber(negativeSign ? -std::abs(v) : std::abs(v));
            auto p = lower(lit).find("general");
            return lit.substr(0, p) + g + lit.substr(p + 7);
        }
        return (negativeSign ? "-" : "") + lit;
    }
    std::string prefix = literal(sec.substr(0, first)), suffix = literal(sec.substr(last + 1));
    std::string core = sec.substr(first, last - first + 1);
    if (sec.find('%') != std::string::npos && bare(sec).find('%') != std::string::npos) v *= 100;
    // scientific
    auto epos = core.find_first_of("eE");
    if (epos != std::string::npos) {
        std::string mant = core.substr(0, epos);
        auto dot = mant.find('.');
        int decimals = dot == std::string::npos ? 0 : static_cast<int>(mant.size() - dot - 1);
        int expDigits = static_cast<int>(core.size() - epos - 2);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*E", decimals, std::abs(v));
        std::string s = buf;
        auto e = s.find('E');
        int ex = std::atoi(s.c_str() + e + 1);
        char eb[16];
        std::snprintf(eb, sizeof eb, "E%c%0*d", ex < 0 ? '-' : '+', std::max(1, expDigits), ex < 0 ? -ex : ex);
        return (negativeSign ? "-" : "") + prefix + s.substr(0, e) + eb + suffix;
    }
    // trailing commas scale by thousands
    while (!core.empty() && core.back() == ',') {
        core.pop_back();
        v /= 1000;
    }
    auto dot = core.find('.');
    std::string intPart = dot == std::string::npos ? core : core.substr(0, dot);
    std::string fracPart = dot == std::string::npos ? "" : core.substr(dot + 1);
    const bool thousands = intPart.find(',') != std::string::npos;
    int minInt = 0;
    for (char c : intPart) minInt += c == '0' ? 1 : 0;
    int maxDec = 0, minDec = 0;
    for (char c : fracPart) {
        if (c == '0' || c == '#' || c == '?') ++maxDec;
        if (c == '0') minDec = maxDec;
    }
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.*f", maxDec, std::abs(v));
    std::string s = buf;
    std::string ip = s, fp;
    auto sd = s.find('.');
    if (sd != std::string::npos) {
        ip = s.substr(0, sd);
        fp = s.substr(sd + 1);
    }
    while (static_cast<int>(fp.size()) > minDec && !fp.empty() && fp.back() == '0') fp.pop_back();
    if (ip == "0" && minInt == 0) ip.clear();
    while (static_cast<int>(ip.size()) < minInt) ip.insert(ip.begin(), '0');
    if (thousands) ip = groupThousands(ip);
    std::string number = fp.empty() ? ip : ip + "." + fp;
    if (number.empty()) number = "0";
    // a value that rounds to zero isn't shown as negative
    bool allZero = true;
    for (char c : number) allZero = allZero && (c == '0' || c == '.' || c == ',');
    return (negativeSign && !allZero ? "-" : "") + prefix + number + suffix;
}

}  // namespace

bool isDateFormat(const std::string& format) {
    std::string b = lower(bare(sections(format)[0]));
    if (b.find("general") != std::string::npos) return false;
    if (b.find_first_of("0#?") != std::string::npos) return false;
    return b.find_first_of("ydhs") != std::string::npos || b.find('m') != std::string::npos;
}

std::string formatValue(const CellValue& v, const std::string& format) {
    switch (v.type) {
        case CellValue::Empty: return "";
        case CellValue::Error: return v.text;
        case CellValue::Bool: return v.number != 0 ? "TRUE" : "FALSE";
        case CellValue::Text: {
            auto secs = sections(format);
            if (secs.size() >= 4 && secs[3].find('@') != std::string::npos) {
                std::string lit = literal(secs[3]);
                auto p = lit.find('@');
                return lit.substr(0, p) + v.text + lit.substr(p + 1);
            }
            return v.text;
        }
        case CellValue::Number: break;
    }
    const double n = v.number;
    if (format.empty() || lower(format) == "general") return generalNumber(n);
    if (format == "@") return generalNumber(n);
    auto secs = sections(format);
    std::string sec = secs[0];
    bool sign = n < 0;
    if (n < 0 && secs.size() >= 2) {
        sec = secs[1];
        sign = false;
    } else if (n == 0 && secs.size() >= 3) {
        sec = secs[2];
    }
    if (isDateFormat(sec)) {
        if (n < 0) return "#########";
        return formatDate(n, sec);
    }
    return formatNumber(n, sec, sign);
}

// ---- typed input -----------------------------------------------------------------------

void setInput(Cell& cell, const std::string& input) {
    cell.formula.clear();
    cell.ast.reset();
    if (input.empty()) {
        cell.value = CellValue{};
        return;
    }
    if (input[0] == '=' && input.size() > 1) {
        cell.formula = trimCopy(input.substr(1));
        cell.value = CellValue{};
        return;
    }
    if (input[0] == '\'') {
        cell.value = CellValue::str(input.substr(1));
        return;
    }
    const std::string t = trimCopy(input);
    const std::string up = upper(t);
    if (up == "TRUE" || up == "FALSE") {
        cell.value = CellValue::boolean(up == "TRUE");
        return;
    }
    int y, m, d;
    if (parseIsoDate(t, y, m, d)) {
        cell.value = CellValue::num(dateSerial(y, m, d));
        if (cell.format.empty()) cell.format = "yyyy-mm-dd";
        return;
    }
    // numbers, allowing a currency sign, thousands separators and a percent sign
    std::string s = t;
    bool pct = false, currency = false, grouped = false, neg = false;
    if (!s.empty() && s.back() == '%') {
        pct = true;
        s.pop_back();
    }
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.erase(0, 1);
    }
    if (!s.empty() && s[0] == '$') {
        currency = true;
        s.erase(0, 1);
    }
    std::string digits;
    bool ok = !s.empty();
    for (std::size_t i = 0; i < s.size() && ok; ++i) {
        char c = s[i];
        if (c == ',') {
            // only as a thousands separator: three digits must follow before the end or the point
            std::size_t j = i + 1, n = 0;
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
                ++j;
                ++n;
            }
            ok = n == 3 && i > 0;
            grouped = true;
            continue;
        }
        digits += c;
    }
    if (ok) {
        char* end = nullptr;
        double v = std::strtod(digits.c_str(), &end);
        ok = end && *end == '\0' && !digits.empty() && std::isfinite(v) &&
             (std::isdigit(static_cast<unsigned char>(digits[0])) || digits[0] == '.');
        if (ok) {
            if (neg) v = -v;
            if (pct) v /= 100;
            cell.value = CellValue::num(v);
            if (cell.format.empty()) {
                auto dot = digits.find('.');
                int decimals = dot == std::string::npos ? 0 : static_cast<int>(digits.size() - dot - 1);
                if (digits.find_first_of("eE") != std::string::npos) decimals = 0;
                std::string dec = decimals > 0 ? "." + std::string(static_cast<std::size_t>(std::min(decimals, 10)), '0') : "";
                if (pct) cell.format = "0" + dec + "%";
                else if (currency) cell.format = "$#,##0" + (decimals > 0 ? dec : std::string());
                else if (grouped) cell.format = "#,##0" + dec;
            }
            return;
        }
    }
    cell.value = CellValue::str(input);
}

std::string inputText(const Cell& cell) {
    if (cell.hasFormula()) return "=" + cell.formula;
    const CellValue& v = cell.value;
    switch (v.type) {
        case CellValue::Empty: return "";
        case CellValue::Number: return isDateFormat(cell.format) ? formatValue(v, cell.format) : textNumber(v.number);
        case CellValue::Bool: return v.number != 0 ? "TRUE" : "FALSE";
        case CellValue::Text: {
            std::string s = v.text;
            Cell probe;
            setInput(probe, s);
            // text that would be read back as something else keeps its apostrophe
            if (probe.value.type != CellValue::Text || probe.hasFormula() || (!s.empty() && s[0] == '\'')) return "'" + s;
            return s;
        }
        case CellValue::Error: return v.text;
    }
    return "";
}

// ---- structural edits ---------------------------------------------------------------------

namespace {

// Shifts an inclusive span [a, b] for an insert (count > 0) or delete (count < 0) at `at`.
bool shiftSpan(int& a, int& b, int at, int count, int limit) {
    if (count > 0) {
        if (a >= at) a += count;
        if (b >= at) b += count;
        return a < limit && b < limit;
    }
    const int n = -count, end = at + n;  // deleted: [at, end)
    if (a >= at && b < end) return false;
    int na = a < at ? a : (a < end ? at : a - n);
    int nb = b < at ? b : (b < end ? at - 1 : b - n);
    if (nb < na) return false;
    a = na;
    b = nb;
    return true;
}

void rewriteAll(Workbook& wb, int target, const std::function<bool(FormulaRef&)>& fn) {
    for (std::size_t s = 0; s < wb.sheets.size(); ++s) {
        for (auto& [k, c] : wb.sheets[s].cells) {
            if (!c.hasFormula()) continue;
            std::string f = rewriteFormula(c.formula, [&](FormulaRef& r) {
                const int sheet = r.sheet.empty() ? static_cast<int>(s) : wb.sheetIndex(r.sheet);
                if (sheet != target) return true;
                return fn(r);
            });
            if (f != c.formula) {
                c.formula = f;
                c.ast.reset();
            }
        }
    }
}

void moveCells(Sheet& sh, bool rows, int at, int count) {
    std::map<std::int64_t, Cell> moved;
    for (auto& [k, c] : sh.cells) {
        int r = Sheet::rowOf(k), col = Sheet::colOf(k);
        int& v = rows ? r : col;
        if (count < 0 && v >= at && v < at - count) continue;
        if (v >= at) v += count;
        if (r < 0 || col < 0 || r >= kMaxRows || col >= kMaxCols) continue;
        moved.emplace(Sheet::key(r, col), std::move(c));
    }
    sh.cells = std::move(moved);
    if (!rows) {
        std::map<int, double> widths;
        for (auto [col, w] : sh.colWidths) {
            if (count < 0 && col >= at && col < at - count) continue;
            widths[col >= at ? col + count : col] = w;
        }
        sh.colWidths = std::move(widths);
    }
}

void structural(Workbook& wb, int sheet, bool rows, int at, int count) {
    if (sheet < 0 || sheet >= static_cast<int>(wb.sheets.size()) || count == 0) return;
    // formulas first, while sheet names and positions still match
    rewriteAll(wb, sheet, [&](FormulaRef& r) {
        if (rows) {
            if (r.kind == FormulaRef::ColsRef) return true;
            return shiftSpan(r.r1, r.r2, at, count, kMaxRows);
        }
        if (r.kind == FormulaRef::RowsRef) return true;
        return shiftSpan(r.c1, r.c2, at, count, kMaxCols);
    });
    moveCells(wb.sheets[static_cast<std::size_t>(sheet)], rows, at, count);
}

}  // namespace

void Workbook::insertRows(int sheet, int at, int count) { structural(*this, sheet, true, at, count); }
void Workbook::deleteRows(int sheet, int at, int count) { structural(*this, sheet, true, at, -count); }
void Workbook::insertCols(int sheet, int at, int count) { structural(*this, sheet, false, at, count); }
void Workbook::deleteCols(int sheet, int at, int count) { structural(*this, sheet, false, at, -count); }

void Workbook::renameSheet(int sheet, const std::string& name) {
    if (sheet < 0 || sheet >= static_cast<int>(sheets.size())) return;
    rewriteAll(*this, sheet, [&](FormulaRef& r) {
        if (!r.sheet.empty()) r.sheet = name;
        return true;
    });
    sheets[static_cast<std::size_t>(sheet)].name = name;
}

void Workbook::deleteSheet(int sheet) {
    if (sheet < 0 || sheet >= static_cast<int>(sheets.size())) return;
    // references from other sheets become #REF!; the sheet's own formulas go with it
    rewriteAll(*this, sheet, [&](FormulaRef& r) { return r.sheet.empty(); });
    sheets.erase(sheets.begin() + sheet);
    if (active >= static_cast<int>(sheets.size())) active = static_cast<int>(sheets.size()) - 1;
    if (active < 0) active = 0;
}

}  // namespace achclwss::office
