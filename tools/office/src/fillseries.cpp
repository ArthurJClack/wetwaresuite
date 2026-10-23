// Fill series (Excel's fill handle / Fill > Series): what the cells after a
// few source cells become. Numbers and dates continue as a linear trend (or a
// growth trend), dates can step by weekdays, months or years, "Item 7" and
// "Q1" count up, month and day names cycle, anything else repeats.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "sheet_internal.hpp"

namespace wetwaresuite::office {

namespace {

const char* const kMonths[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
const char* const kDays[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// A month or day name: which list, its index, whether short ("Jan") and its case (0 Title, 1 UPPER, 2 lower).
struct Name {
    int list = -1, index = 0;
    bool shortForm = false;
    int letterCase = 0;
};

bool nameOf(const std::string& text, Name& n) {
    if (text.size() < 3) return false;
    const std::string l = lower(text);
    for (int list = 0; list < 2; ++list) {
        const int count = list == 0 ? 12 : 7;
        for (int i = 0; i < count; ++i) {
            const std::string full = list == 0 ? kMonths[i] : kDays[i];
            if (l == full || l == full.substr(0, 3)) {
                n.list = list;
                n.index = i;
                n.shortForm = l.size() == 3 && full.size() > 3;
                bool up = true, low = true;
                for (char ch : text) {
                    if (std::islower(static_cast<unsigned char>(ch))) up = false;
                    if (std::isupper(static_cast<unsigned char>(ch))) low = false;
                }
                n.letterCase = up ? 1 : low ? 2 : 0;
                return true;
            }
        }
    }
    return false;
}

std::string nameText(const Name& n, int index) {
    const int count = n.list == 0 ? 12 : 7;
    index = ((index % count) + count) % count;
    std::string s = n.list == 0 ? kMonths[index] : kDays[index];
    if (n.shortForm) s = s.substr(0, 3);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool up = n.letterCase == 1 || (n.letterCase == 0 && i == 0);
        s[i] = static_cast<char>(up ? std::toupper(static_cast<unsigned char>(s[i])) : s[i]);
    }
    return s;
}

// "Item 7", "Q1", "file-003.txt": the text around the last run of digits and the number.
struct Numbered {
    std::string prefix, suffix;
    long long number = 0;
    int width = 0;  // digits written, for zero padding
};

bool numberedOf(const std::string& text, Numbered& n) {
    std::size_t end = text.size();
    while (end > 0 && !std::isdigit(static_cast<unsigned char>(text[end - 1]))) --end;
    if (end == 0) return false;
    std::size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(text[start - 1]))) --start;
    if (end - start > 15) return false;
    n.prefix = text.substr(0, start);
    n.suffix = text.substr(end);
    n.number = std::stoll(text.substr(start, end - start));
    n.width = text[start] == '0' ? static_cast<int>(end - start) : 0;
    return true;
}

std::string numberedText(const Numbered& n, long long value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    if (static_cast<int>(digits.size()) < n.width) digits.insert(0, static_cast<std::size_t>(n.width) - digits.size(), '0');
    return n.prefix + (value < 0 ? "-" : "") + digits + n.suffix;
}

int daysInMonth(int y, int m) {
    static const int kDim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : kDim[m - 1];
}

double addMonths(double serial, long long months) {
    int y, m, d;
    serialDate(serial, y, m, d);
    long long total = static_cast<long long>(y) * 12 + (m - 1) + months;
    const int ny = static_cast<int>(total / 12), nm = static_cast<int>(total % 12) + 1;
    const double frac = serial - std::floor(serial);
    return dateSerial(ny, nm, std::min(d, daysInMonth(ny, nm))) + frac;
}

int weekday(double serial) {  // 0 = Sunday
    return static_cast<int>(((static_cast<long long>(std::floor(serial)) - 1) % 7 + 7) % 7);
}

double addWeekdays(double serial, long long days) {
    double s = serial;
    const int dir = days < 0 ? -1 : 1;
    for (long long k = 0; k < (days < 0 ? -days : days); ++k) {
        do {
            s += dir;
        } while (weekday(s) == 0 || weekday(s) == 6);
    }
    return s;
}

// Months between two dates when they fall on the same day of the month (or both on the month's last day).
bool monthGap(double a, double b, long long& months) {
    int y1, m1, d1, y2, m2, d2;
    serialDate(a, y1, m1, d1);
    serialDate(b, y2, m2, d2);
    const bool sameDay = d1 == d2 || (d1 == daysInMonth(y1, m1) && d2 == daysInMonth(y2, m2));
    months = (static_cast<long long>(y2) * 12 + m2) - (static_cast<long long>(y1) * 12 + m1);
    return sameDay && months != 0 && std::floor(a) == a && std::floor(b) == b;
}

}  // namespace

bool fillSeries(const std::vector<Cell>& src, int count, const FillSpec& spec, std::vector<Cell>& out, std::vector<int>& copiedFrom, std::string& error) {
    out.assign(static_cast<std::size_t>(std::max(0, count)), Cell{});
    copiedFrom.assign(out.size(), -1);
    const int n = static_cast<int>(src.size());
    if (n == 0 || count <= 0) return true;
    auto repeat = [&] {
        for (int k = 0; k < count; ++k) {
            out[static_cast<std::size_t>(k)] = src[static_cast<std::size_t>(k % n)];
            copiedFrom[static_cast<std::size_t>(k)] = k % n;
        }
        return true;
    };
    if (spec.series == "copy") return repeat();
    auto styled = [&](int k, CellValue v) {
        Cell c = src[static_cast<std::size_t>((k + n) % n)];
        c.formula.clear();
        c.ast.reset();
        c.value = std::move(v);
        return c;
    };

    bool numbers = true, texts = true;
    for (const auto& c : src) {
        if (c.hasFormula() || c.value.type != CellValue::Number) numbers = false;
        if (c.hasFormula() || c.value.type != CellValue::Text) texts = false;
    }
    if (numbers) {
        const bool date = isDateFormat(src[0].format);
        const std::string unit = spec.unit == "day" ? std::string() : spec.unit;
        if (spec.series == "growth") {
            double ratio = spec.step;
            if (!spec.hasStep) {
                if (n < 2 || src[0].value.number == 0) {
                    error = "a growth series needs two starting numbers (or step=<ratio>)";
                    return false;
                }
                ratio = std::pow(src[static_cast<std::size_t>(n - 1)].value.number / src[0].value.number, 1.0 / (n - 1));
                if (!std::isfinite(ratio)) {
                    error = "can't find a growth rate from those numbers";
                    return false;
                }
            }
            double v = src[static_cast<std::size_t>(n - 1)].value.number;
            for (int k = 0; k < count; ++k) out[static_cast<std::size_t>(k)] = styled(k, CellValue::num(v *= ratio));
            return true;
        }
        const double last = src[static_cast<std::size_t>(n - 1)].value.number;
        if (date && unit.empty() && !spec.hasStep && n >= 2) {
            long long months = 0, gap = 0;
            bool monthly = monthGap(src[0].value.number, src[1].value.number, months);
            for (int i = 1; monthly && i < n; ++i) monthly = monthGap(src[static_cast<std::size_t>(i - 1)].value.number, src[static_cast<std::size_t>(i)].value.number, gap) && gap == months;
            if (monthly) {
                for (int k = 0; k < count; ++k) out[static_cast<std::size_t>(k)] = styled(k, CellValue::num(addMonths(src[0].value.number, months * (n + k))));
                return true;
            }
        }
        if (unit == "month" || unit == "year" || unit == "weekday") {
            const long long step = static_cast<long long>(std::llround(spec.hasStep ? spec.step : 1.0)) * (unit == "year" ? 12 : 1);
            for (int k = 0; k < count; ++k) {
                const double v = unit == "weekday" ? addWeekdays(last, step * (k + 1)) : addMonths(last, step * (k + 1));
                out[static_cast<std::size_t>(k)] = styled(k, CellValue::num(v));
            }
            return true;
        }
        if (spec.hasStep) {
            for (int k = 0; k < count; ++k) out[static_cast<std::size_t>(k)] = styled(k, CellValue::num(last + spec.step * (k + 1)));
            return true;
        }
        if (n == 1) {
            if (!date && spec.series != "linear") return repeat();  // like Excel: one number copies, one date counts up
            for (int k = 0; k < count; ++k) out[static_cast<std::size_t>(k)] = styled(k, CellValue::num(last + (k + 1)));
            return true;
        }
        // least-squares line through the source cells (exact for two)
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < n; ++i) {
            const double y = src[static_cast<std::size_t>(i)].value.number;
            sx += i;
            sy += y;
            sxx += static_cast<double>(i) * i;
            sxy += i * y;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        const double icpt = (sy - slope * sx) / n;
        for (int k = 0; k < count; ++k) {
            double v = icpt + slope * (n + k);
            if (std::fabs(v - std::round(v)) < 1e-9) v = std::round(v);
            out[static_cast<std::size_t>(k)] = styled(k, CellValue::num(v));
        }
        return true;
    }
    if (texts) {
        std::vector<Name> names(static_cast<std::size_t>(n));
        bool allNames = true;
        for (int i = 0; i < n && allNames; ++i) {
            allNames = nameOf(src[static_cast<std::size_t>(i)].value.text, names[static_cast<std::size_t>(i)]) && names[static_cast<std::size_t>(i)].list == names[0].list;
        }
        if (allNames) {
            const int size = names[0].list == 0 ? 12 : 7;
            int step = spec.hasStep ? static_cast<int>(std::lround(spec.step)) : n >= 2 ? ((names[1].index - names[0].index) % size + size) % size : 1;
            if (step == 0 && !spec.hasStep) step = 1;
            const Name& lastName = names[static_cast<std::size_t>(n - 1)];
            for (int k = 0; k < count; ++k) out[static_cast<std::size_t>(k)] = styled(k, CellValue::str(nameText(lastName, lastName.index + step * (k + 1))));
            return true;
        }
        std::vector<Numbered> nums(static_cast<std::size_t>(n));
        bool allNumbered = true;
        for (int i = 0; i < n && allNumbered; ++i) {
            auto& x = nums[static_cast<std::size_t>(i)];
            allNumbered = numberedOf(src[static_cast<std::size_t>(i)].value.text, x) && x.prefix == nums[0].prefix && x.suffix == nums[0].suffix;
        }
        if (allNumbered) {
            long long step = spec.hasStep ? std::llround(spec.step) : n >= 2 ? (nums[static_cast<std::size_t>(n - 1)].number - nums[0].number) / (n - 1) : 1;
            const Numbered& lastNum = nums[static_cast<std::size_t>(n - 1)];
            for (int k = 0; k < count; ++k) out[static_cast<std::size_t>(k)] = styled(k, CellValue::str(numberedText(lastNum, lastNum.number + step * (k + 1))));
            return true;
        }
    }
    if (spec.hasStep || spec.series == "linear") {
        error = "a series needs numbers, dates, month or day names, or text ending in a number";
        return false;
    }
    return repeat();
}

}  // namespace wetwaresuite::office
