// Rules that decide how cells look and what may go in them: conditional
// formatting (highlight rules, top/bottom, averages, duplicates, colour scales,
// data bars) and data validation (lists, number/date/length limits, custom).
#include <algorithm>
#include <cmath>
#include <map>

#include "wetwaresuite/office/sheet.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

namespace {

// A number for a rule's bound: a date, a time, a number or a formula evaluated at the cell.
bool boundValue(const Workbook& wb, int sheet, int row, int col, const Range& base, const std::string& text, CellValue& out) {
    std::string t = trimCopy(text);
    if (!t.empty() && t[0] == '=') t = trimCopy(t.substr(1));
    if (t.empty()) return false;
    int y, m, d;
    if (parseIsoDate(t, y, m, d)) {
        out = CellValue::num(dateSerial(y, m, d));
        return true;
    }
    int h, mi;
    char colon;
    if (std::sscanf(t.c_str(), "%d%c%d", &h, &colon, &mi) == 3 && colon == ':' && t.find_first_not_of("0123456789:") == std::string::npos) {
        out = CellValue::num((h * 60 + mi) / 1440.0);
        return true;
    }
    if (!formulaError(t).empty()) {
        out = CellValue::str(t);  // plain text such as North
        return true;
    }
    out = wb.evaluate(offsetFormula(t, row - base.r1, col - base.c1), sheet, row, col);
    if (out.isError() && out.text == "#NAME?" && t.find('(') == std::string::npos) out = CellValue::str(t);  // a word, not a name
    return true;
}

int cmp(const CellValue& a, const CellValue& b) {
    if (a.type == CellValue::Text || b.type == CellValue::Text) {
        const std::string x = lower(formatValue(a, "")), y = lower(formatValue(b, ""));
        return x < y ? -1 : x > y ? 1 : 0;
    }
    return a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
}

bool compareOp(const std::string& op, const CellValue& v, const CellValue& a, const CellValue& b) {
    if (op == "between" || op == "not_between") {
        CellValue lo = a, hi = b;
        if (cmp(lo, hi) > 0) std::swap(lo, hi);
        const bool in = cmp(v, lo) >= 0 && cmp(v, hi) <= 0;
        return op == "between" ? in : !in;
    }
    const int c = cmp(v, a);
    if (op == ">") return c > 0;
    if (op == "<") return c < 0;
    if (op == ">=") return c >= 0;
    if (op == "<=") return c <= 0;
    if (op == "<>" || op == "!=") return c != 0;
    return c == 0;
}

std::uint32_t lerp(std::uint32_t a, std::uint32_t b, double t) {
    auto ch = [&](int shift) {
        const double x = ((a >> shift) & 0xFF), y = ((b >> shift) & 0xFF);
        return static_cast<std::uint32_t>(std::lround(x + (y - x) * t)) & 0xFF;
    };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0) | kColorSet;
}

std::string key(const CellValue& v) { return std::to_string(static_cast<int>(v.type)) + (v.type == CellValue::Text ? lower(v.text) : textNumber(v.number)); }

}  // namespace

struct CellPainter::Impl {
    const Workbook& wb;
    int sheet;
    struct Stats {
        bool ready = false;
        std::vector<double> sorted;  // numbers in the range, ascending
        double mean = 0;
        std::map<std::string, int> counts;  // value -> how often (duplicate / unique)
    };
    mutable std::vector<Stats> stats;

    const Sheet& sh() const { return wb.sheets[static_cast<std::size_t>(sheet)]; }

    const Stats& statsFor(std::size_t i) const {
        Stats& s = stats[i];
        if (s.ready) return s;
        s.ready = true;
        const Range& r = sh().condFormats[i].range;
        for (const auto& [k, c] : sh().cells) {
            const int row = Sheet::rowOf(k), col = Sheet::colOf(k);
            if (!r.contains(row, col) || c.value.empty()) continue;
            if (c.value.type == CellValue::Number) s.sorted.push_back(c.value.number);
            ++s.counts[key(c.value)];
        }
        std::sort(s.sorted.begin(), s.sorted.end());
        double sum = 0;
        for (double x : s.sorted) sum += x;
        s.mean = s.sorted.empty() ? 0 : sum / static_cast<double>(s.sorted.size());
        return s;
    }

    bool match(std::size_t i, int row, int col, const CellValue& v) const {
        const CondFormat& cf = sh().condFormats[i];
        const std::string& t = cf.type;
        if (t == "formula") {
            CellValue res;
            if (!boundValue(wb, sheet, row, col, cf.range, cf.value1, res)) return false;
            if (res.type == CellValue::Number || res.type == CellValue::Bool) return res.number != 0;
            return false;
        }
        if (t == "blank") return v.empty() || (v.type == CellValue::Text && trimCopy(v.text).empty());
        if (t == "not_blank") return !(v.empty() || (v.type == CellValue::Text && trimCopy(v.text).empty()));
        if (t == "errors") return v.isError();
        if (v.empty() || v.isError()) return false;
        if (t == "cell") {
            CellValue a, b;
            if (!boundValue(wb, sheet, row, col, cf.range, cf.value1, a)) return false;
            if (cf.op == "between" || cf.op == "not_between") {
                if (!boundValue(wb, sheet, row, col, cf.range, cf.value2, b)) return false;
            }
            if (a.isError() || b.isError()) return false;
            if ((v.type == CellValue::Text) != (a.type == CellValue::Text) && cf.op != "=" && cf.op != "<>") return false;
            return compareOp(cf.op, v, a, b);
        }
        if (t == "text" || t == "begins" || t == "ends") {
            const std::string hay = lower(formatValue(v, "")), needle = lower(cf.value1);
            if (t == "text") return hay.find(needle) != std::string::npos;
            if (t == "begins") return startsWith(hay, needle);
            return endsWith(hay, needle);
        }
        if (t == "duplicate" || t == "unique") {
            const auto& s = statsFor(i);
            auto it = s.counts.find(key(v));
            const int n = it == s.counts.end() ? 0 : it->second;
            return t == "duplicate" ? n > 1 : n == 1;
        }
        if (v.type != CellValue::Number) return false;
        const auto& s = statsFor(i);
        if (s.sorted.empty()) return false;
        if (t == "above_average") return v.number > s.mean;
        if (t == "below_average") return v.number < s.mean;
        if (t == "top" || t == "bottom") {
            double n = cf.value1.empty() ? 10 : std::atof(cf.value1.c_str());
            if (cf.percent) n = std::floor(static_cast<double>(s.sorted.size()) * n / 100);
            const std::size_t k = static_cast<std::size_t>(std::clamp(n, 1.0, static_cast<double>(s.sorted.size())));
            if (t == "top") return v.number >= s.sorted[s.sorted.size() - k];
            return v.number <= s.sorted[k - 1];
        }
        return false;
    }
};

CellPainter::CellPainter(const Workbook& wb, int sheet) : impl_(std::make_unique<Impl>(Impl{wb, sheet, {}})) {
    if (sheet >= 0 && sheet < static_cast<int>(wb.sheets.size())) impl_->stats.resize(wb.sheets[static_cast<std::size_t>(sheet)].condFormats.size());
}
CellPainter::~CellPainter() = default;

bool CellPainter::matches(std::size_t rule, int row, int col) const {
    if (impl_->sheet < 0 || impl_->sheet >= static_cast<int>(impl_->wb.sheets.size())) return false;
    const Sheet& sh = impl_->sh();
    if (rule >= sh.condFormats.size() || !sh.condFormats[rule].range.contains(row, col)) return false;
    const Cell* c = sh.get(row, col);
    return impl_->match(rule, row, col, c ? c->value : CellValue{});
}

CellStyle CellPainter::style(int row, int col, double* bar, std::uint32_t* barColor) const {
    if (bar) *bar = -1;
    if (impl_->sheet < 0 || impl_->sheet >= static_cast<int>(impl_->wb.sheets.size())) return {};
    const Sheet& sh = impl_->sh();
    const Cell* c = sh.get(row, col);
    CellStyle st = c ? c->style : CellStyle{};
    if (sh.condFormats.empty()) return st;
    const CellValue v = c ? c->value : CellValue{};
    for (std::size_t i = 0; i < sh.condFormats.size(); ++i) {
        const CondFormat& cf = sh.condFormats[i];
        if (!cf.range.contains(row, col)) continue;
        if (cf.type == "color_scale" || cf.type == "data_bar") {
            if (v.type != CellValue::Number) continue;
            const auto& s = impl_->statsFor(i);
            if (s.sorted.empty()) continue;
            const double lo = s.sorted.front(), hi = s.sorted.back();
            const double f = hi > lo ? (v.number - lo) / (hi - lo) : 1;
            if (cf.type == "data_bar") {
                if (bar) *bar = std::clamp(f, 0.0, 1.0);
                if (barColor) *barColor = hasColor(cf.maxColor) ? cf.maxColor : (0x638EC6 | kColorSet);
                continue;
            }
            const std::uint32_t a = hasColor(cf.minColor) ? cf.minColor : (0xF8696B | kColorSet);
            const std::uint32_t b = hasColor(cf.maxColor) ? cf.maxColor : (0x63BE7B | kColorSet);
            if (hasColor(cf.midColor)) st.fill = f < 0.5 ? lerp(a, cf.midColor, f * 2) : lerp(cf.midColor, b, (f - 0.5) * 2);
            else st.fill = lerp(a, b, f);
            continue;
        }
        if (!impl_->match(i, row, col, v)) continue;
        const CellStyle& o = cf.style;
        if (hasColor(o.fill)) st.fill = o.fill;
        if (hasColor(o.color)) st.color = o.color;
        if (o.bold) st.bold = true;
        if (o.italic) st.italic = true;
        if (o.underline) st.underline = true;
        if (o.strike) st.strike = true;
        if (hasColor(o.borderColor)) {
            st.borderColor = o.borderColor;
            for (int k = 0; k < 4; ++k) st.border[k] = std::max<std::uint8_t>(st.border[k], 1);
        }
    }
    return st;
}

CellStyle effectiveStyle(const Workbook& wb, int sheet, int row, int col, double* bar, std::uint32_t* barColor) {
    CellPainter p(wb, sheet);
    return p.style(row, col, bar, barColor);
}

// ---- validation ----------------------------------------------------------------

const Validation* validationAt(const Workbook& wb, int sheet, int row, int col) {
    if (sheet < 0 || sheet >= static_cast<int>(wb.sheets.size())) return nullptr;
    const auto& vs = wb.sheets[static_cast<std::size_t>(sheet)].validations;
    for (auto it = vs.rbegin(); it != vs.rend(); ++it) {
        if (it->range.contains(row, col)) return &*it;
    }
    return nullptr;
}

std::vector<std::string> validationList(const Workbook& wb, int sheet, const Validation& v) {
    std::vector<std::string> out;
    std::string src = trimCopy(v.value1);
    if (!src.empty() && src[0] == '=') src = trimCopy(src.substr(1));
    Range r;
    std::string err;
    if (!src.empty() && src.find(',') == std::string::npos && parseRange(wb, src, sheet, r, err)) {
        const Sheet& sh = wb.sheets[static_cast<std::size_t>(r.sheet)];
        const int r2 = std::min(r.r2, r.r1 + 9999), c2 = std::min(r.c2, r.c1 + 255);
        for (int row = r.r1; row <= r2; ++row) {
            for (int col = r.c1; col <= c2; ++col) {
                const Cell* c = sh.get(row, col);
                if (c && !c->value.empty()) out.push_back(formatValue(c->value, c->format));
            }
        }
        return out;
    }
    if (src.size() >= 2 && src.front() == '"' && src.back() == '"') src = src.substr(1, src.size() - 2);
    std::string item;
    for (char ch : src + ",") {
        if (ch == ',') {
            std::string t = trimCopy(item);
            if (!t.empty()) out.push_back(t);
            item.clear();
        } else {
            item += ch;
        }
    }
    return out;
}

bool validateCell(const Workbook& wb, int sheet, int row, int col, std::string& message, bool& reject) {
    reject = false;
    const Validation* v = validationAt(wb, sheet, row, col);
    if (!v) return true;
    const Cell* cell = wb.sheets[static_cast<std::size_t>(sheet)].get(row, col);
    const CellValue val = cell ? cell->value : CellValue{};
    const bool blank = val.empty() || (val.type == CellValue::Text && val.text.empty());
    if (blank) return v->allowBlank || v->type == "custom";
    bool ok = true;
    std::string why;
    const std::string& t = v->type;
    auto bounds = [&](std::string& describe) {
        CellValue a, b;
        bool haveA = boundValue(wb, sheet, row, col, v->range, v->value1, a);
        bool haveB = boundValue(wb, sheet, row, col, v->range, v->value2, b);
        auto show = [&](const CellValue& x) {
            if (t == "date" && x.type == CellValue::Number) return formatValue(x, "yyyy-mm-dd");
            if (t == "time" && x.type == CellValue::Number) return formatValue(x, "hh:mm");
            return formatValue(x, "");
        };
        if (v->op == "between" || v->op == "not_between") describe = (v->op == "between" ? "between " : "not between ") + show(a) + " and " + show(b);
        else describe = v->op + " " + show(a);
        return std::make_pair(haveA ? a : CellValue{}, haveB ? b : CellValue{});
    };
    if (t == "list") {
        const auto items = validationList(wb, sheet, *v);
        const std::string shown = lower(cell ? formatValue(val, cell->format) : std::string()), raw = lower(formatValue(val, ""));
        ok = false;
        for (const auto& it : items) ok = ok || lower(it) == shown || lower(it) == raw;
        std::string list;
        for (std::size_t i = 0; i < items.size() && i < 12; ++i) list += (i ? ", " : "") + items[i];
        if (items.size() > 12) list += ", ...";
        why = "must be one of: " + list;
    } else if (t == "custom") {
        CellValue res;
        ok = boundValue(wb, sheet, row, col, v->range, v->value1, res) && (res.type == CellValue::Number || res.type == CellValue::Bool) && res.number != 0;
        why = "doesn't meet the rule =" + v->value1;
    } else if (t == "length") {
        std::string d;
        auto [a, b] = bounds(d);
        const double n = static_cast<double>(formatValue(val, cell ? cell->format : "").size());
        ok = compareOp(v->op, CellValue::num(n), a, b);
        why = "length must be " + d;
    } else {
        std::string d;
        auto [a, b] = bounds(d);
        if (val.type != CellValue::Number) {
            ok = false;
        } else {
            ok = compareOp(v->op, val, a, b);
            if (t == "whole" && val.number != std::floor(val.number)) ok = false;
        }
        const char* kind = t == "whole" ? "a whole number " : t == "decimal" ? "a number " : t == "date" ? "a date " : "a time ";
        why = std::string("must be ") + kind + d;
    }
    if (ok) return true;
    message = v->error.empty() ? cellName(row, col) + " " + why : v->error;
    if (!v->errorTitle.empty()) message = v->errorTitle + ": " + message;
    reject = v->errorStyle == "stop" || v->errorStyle.empty();
    return false;
}

}  // namespace wetwaresuite::office
