// Text helpers, dates and frontmatter.
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "wetwaresuite/notes/notes.hpp"

namespace wetwaresuite::notes {

fs::path toPath(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string fromPath(const fs::path& p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool startsWith(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }
bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string urlEncodePath(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == '#') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            cur += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::string slugify(const std::string& text) {
    std::string out;
    bool dash = false;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            if (dash && !out.empty()) out += '-';
            dash = false;
            out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else if (c == ' ' || c == '-' || c == '_' || c == '\t') {
            dash = true;
        }
    }
    return out;
}

bool isTagChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '/' || c >= 0x80; }

// ---- dates ------------------------------------------------------------------------

namespace {

// Howard Hinnant's civil-date algorithms.
std::int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civilFromDays(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(static_cast<std::int64_t>(yoe) + era * 400 + (m <= 2));
}

const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
const char* kDays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

std::string pad(int v, int width) {
    std::string s = std::to_string(v);
    while (static_cast<int>(s.size()) < width) s = "0" + s;
    return s;
}

// ISO 8601 week number and week-based year.
void isoWeek(const Date& d, int& week, int& year) {
    std::int64_t n = dayNumber(d);
    int wd = weekday(d);  // 0 = Monday
    std::int64_t thursday = n - wd + 3;
    int ty, tm, td;
    civilFromDays(thursday, ty, tm, td);
    std::int64_t jan1 = daysFromCivil(ty, 1, 1);
    week = static_cast<int>((thursday - jan1) / 7 + 1);
    year = ty;
}

}  // namespace

Date now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec};
}

std::int64_t dayNumber(const Date& d) { return daysFromCivil(d.year, d.month, d.day); }

Date addDays(const Date& d, int days) {
    Date out = d;
    civilFromDays(dayNumber(d) + days, out.year, out.month, out.day);
    return out;
}

int weekday(const Date& d) {
    std::int64_t n = dayNumber(d);  // 1970-01-01 was a Thursday (3)
    return static_cast<int>(((n % 7) + 7 + 3) % 7);
}

std::string isoDate(const Date& d) { return pad(d.year, 4) + "-" + pad(d.month, 2) + "-" + pad(d.day, 2); }

std::optional<Date> parseDate(const std::string& raw, const Date& base) {
    std::string t = lower(trim(raw));
    Date day = base;
    day.hour = day.minute = day.second = 0;
    if (t.empty() || t == "today") return day;
    if (t == "yesterday") return addDays(day, -1);
    if (t == "tomorrow") return addDays(day, 1);
    if ((t[0] == '+' || t[0] == '-') && t.size() > 1) {
        std::string n = t.substr(1);
        if (!n.empty() && (n.back() == 'd')) n.pop_back();
        bool digits = !n.empty();
        for (char c : n) digits = digits && std::isdigit(static_cast<unsigned char>(c));
        if (digits) return addDays(day, (t[0] == '-' ? -1 : 1) * std::stoi(n));
    }
    int y = 0, m = 0, d = 0;
    if (t.size() >= 10 && std::sscanf(t.c_str(), "%4d-%2d-%2d", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        Date out{y, m, d, 0, 0, 0};
        if (t.size() >= 16 && (t[10] == ' ' || t[10] == 't')) {
            int hh = 0, mm = 0;
            if (std::sscanf(t.c_str() + 11, "%2d:%2d", &hh, &mm) == 2) {
                out.hour = hh;
                out.minute = mm;
            }
        }
        // normalise days past the month's end
        Date norm = addDays(Date{y, m, 1, 0, 0, 0}, d - 1);
        norm.hour = out.hour;
        norm.minute = out.minute;
        return norm;
    }
    return std::nullopt;
}

std::string formatDate(const Date& d, const std::string& f) {
    std::string out;
    std::size_t i = 0;
    auto take = [&](const char* tok) {
        std::size_t n = std::char_traits<char>::length(tok);
        if (f.compare(i, n, tok) == 0) {
            i += n;
            return true;
        }
        return false;
    };
    const int wd = weekday(d);
    while (i < f.size()) {
        if (f[i] == '[') {
            auto close = f.find(']', i);
            if (close == std::string::npos) {
                out += f.substr(i + 1);
                break;
            }
            out += f.substr(i + 1, close - i - 1);
            i = close + 1;
        } else if (take("YYYY") || take("gggg") || take("GGGG")) {
            if (f[i - 1] == 'Y') {
                out += pad(d.year, 4);
            } else {
                int w, y;
                isoWeek(d, w, y);
                out += pad(y, 4);
            }
        } else if (take("YY")) {
            out += pad(d.year % 100, 2);
        } else if (take("MMMM")) {
            out += kMonths[d.month - 1];
        } else if (take("MMM")) {
            out += std::string(kMonths[d.month - 1]).substr(0, 3);
        } else if (take("MM")) {
            out += pad(d.month, 2);
        } else if (take("M")) {
            out += std::to_string(d.month);
        } else if (take("Do")) {
            int n = d.day;
            const char* suf = (n % 100 >= 11 && n % 100 <= 13) ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
            out += std::to_string(n) + suf;
        } else if (take("DDDD")) {
            out += pad(static_cast<int>(dayNumber(d) - daysFromCivil(d.year, 1, 1) + 1), 3);
        } else if (take("DD")) {
            out += pad(d.day, 2);
        } else if (take("D")) {
            out += std::to_string(d.day);
        } else if (take("dddd")) {
            out += kDays[wd];
        } else if (take("ddd")) {
            out += std::string(kDays[wd]).substr(0, 3);
        } else if (take("dd")) {
            out += std::string(kDays[wd]).substr(0, 2);
        } else if (take("WW") || take("ww")) {
            int w, y;
            isoWeek(d, w, y);
            out += pad(w, 2);
        } else if (take("W") || take("w")) {
            int w, y;
            isoWeek(d, w, y);
            out += std::to_string(w);
        } else if (take("HH")) {
            out += pad(d.hour, 2);
        } else if (take("H")) {
            out += std::to_string(d.hour);
        } else if (take("hh")) {
            out += pad(d.hour % 12 == 0 ? 12 : d.hour % 12, 2);
        } else if (take("h")) {
            out += std::to_string(d.hour % 12 == 0 ? 12 : d.hour % 12);
        } else if (take("mm")) {
            out += pad(d.minute, 2);
        } else if (take("m")) {
            out += std::to_string(d.minute);
        } else if (take("ss")) {
            out += pad(d.second, 2);
        } else if (take("s")) {
            out += std::to_string(d.second);
        } else if (take("A")) {
            out += d.hour < 12 ? "AM" : "PM";
        } else if (take("a")) {
            out += d.hour < 12 ? "am" : "pm";
        } else {
            out += f[i++];
        }
    }
    return out;
}

// ---- frontmatter -----------------------------------------------------------------------

std::string FmValue::text() const {
    if (!isList) return scalar;
    std::string out;
    for (const auto& v : list) out += (out.empty() ? "" : ", ") + v;
    return out;
}

namespace {

std::string unquote(const std::string& raw) {
    std::string s = trim(raw);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string out;
        for (std::size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size()) {
                char n = s[++i];
                out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
            } else {
                out += s[i];
            }
        }
        return out;
    }
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        std::string out;
        for (std::size_t i = 1; i + 1 < s.size(); ++i) {
            out += s[i];
            if (s[i] == '\'' && s[i + 1] == '\'') ++i;
        }
        return out;
    }
    // trailing comment
    if (auto hash = s.find(" #"); hash != std::string::npos) s = trim(s.substr(0, hash));
    return s;
}

std::vector<std::string> flowList(const std::string& inner) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    int depth = 0;
    for (char c : inner) {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
            cur += c;
        } else if (c == '[') {
            ++depth;
            cur += c;
        } else if (c == ']') {
            --depth;
            cur += c;
        } else if (c == ',' && depth == 0) {
            if (!trim(cur).empty()) out.push_back(unquote(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) out.push_back(unquote(cur));
    return out;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        std::string l = text.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        start = nl + 1;
    }
    return lines;
}

bool isKeyLine(const std::string& l, std::string* key, std::string* rest) {
    if (l.empty() || l[0] == ' ' || l[0] == '\t' || l[0] == '#' || l[0] == '-') return false;
    auto colon = l.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    if (colon + 1 < l.size() && l[colon + 1] != ' ' && l[colon + 1] != '\t') return false;
    std::string k = trim(l.substr(0, colon));
    if (k.size() >= 2 && (k.front() == '"' || k.front() == '\'')) k = k.substr(1, k.size() - 2);
    if (key) *key = k;
    if (rest) *rest = colon + 1 < l.size() ? l.substr(colon + 1) : "";
    return true;
}

}  // namespace

bool splitFrontmatter(const std::string& text, std::string* yaml, std::size_t* bodyStart, int* bodyLine) {
    std::size_t start = 0;
    if (startsWith(text, "\xEF\xBB\xBF")) start = 3;  // BOM
    auto firstEnd = text.find('\n', start);
    if (firstEnd == std::string::npos) return false;
    if (trim(text.substr(start, firstEnd - start)) != "---") return false;
    std::size_t pos = firstEnd + 1;
    int line = 2;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l == "---" || l == "...") {
            if (yaml) *yaml = text.substr(firstEnd + 1, pos - firstEnd - 1);
            if (bodyStart) *bodyStart = nl == std::string::npos ? text.size() : nl + 1;
            if (bodyLine) *bodyLine = line + 1;
            return true;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++line;
    }
    return false;
}

std::string bodyOf(const std::string& text) {
    std::size_t start = 0;
    if (splitFrontmatter(text, nullptr, &start)) return text.substr(start);
    return text;
}

Frontmatter parseFrontmatter(const std::string& yaml) {
    Frontmatter fm;
    auto lines = splitLines(yaml);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string key, rest;
        if (!isKeyLine(lines[i], &key, &rest)) continue;
        FmValue v;
        std::string r = trim(rest);
        // following indented lines belong to this key
        std::vector<std::string> block;
        std::size_t j = i + 1;
        while (j < lines.size() && (lines[j].empty() || lines[j][0] == ' ' || lines[j][0] == '\t' || startsWith(lines[j], "- "))) {
            block.push_back(lines[j]);
            ++j;
        }
        while (!block.empty() && trim(block.back()).empty()) block.pop_back();
        if (r.empty() && !block.empty()) {
            bool allItems = true;
            for (const auto& b : block) allItems = allItems && (trim(b).empty() || startsWith(trim(b), "- ") || trim(b) == "-");
            if (allItems) {
                v.isList = true;
                for (const auto& b : block) {
                    std::string t = trim(b);
                    if (t.empty()) continue;
                    t = t == "-" ? "" : t.substr(2);
                    if (!trim(t).empty()) v.list.push_back(unquote(t));
                }
            } else {
                for (const auto& b : block) v.scalar += (v.scalar.empty() ? "" : "\n") + b;
            }
        } else if (r == "|" || r == ">" || r == "|-" || r == ">-" || r == "|+" || r == ">+") {
            std::string joined;
            for (const auto& b : block) joined += (joined.empty() ? "" : (r[0] == '|' ? "\n" : " ")) + trim(b);
            v.scalar = joined;
        } else if (r.size() >= 2 && r.front() == '[' && r.back() == ']') {
            v.isList = true;
            v.list = flowList(r.substr(1, r.size() - 2));
        } else {
            v.scalar = unquote(r);
        }
        i = j - 1;
        fm.emplace_back(key, std::move(v));
    }
    return fm;
}

const FmValue* fmFind(const Frontmatter& fm, const std::string& key) {
    for (const auto& [k, v] : fm) {
        if (lower(k) == lower(key)) return &v;
    }
    return nullptr;
}

std::string yamlScalar(const std::string& s) {
    bool quote = s.empty() || s != trim(s) || s.find(": ") != std::string::npos || s.find(" #") != std::string::npos ||
                 s.find('\n') != std::string::npos || endsWith(s, ":");
    if (!quote && std::string("-?:,[]{}#&*!|>'\"%@`").find(s[0]) != std::string::npos) quote = true;
    if (!quote) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string setFrontmatter(const std::string& text, const std::string& key, const FmValue* value) {
    std::string entry;
    if (value) {
        if (value->isList) {
            entry = key + ":\n";
            for (const auto& item : value->list) entry += "  - " + yamlScalar(item) + "\n";
            if (value->list.empty()) entry = key + ": []\n";
        } else {
            entry = key + ": " + yamlScalar(value->scalar) + "\n";
        }
    }
    std::string yaml;
    std::size_t bodyStart = 0;
    if (!splitFrontmatter(text, &yaml, &bodyStart)) {
        if (!value) return text;
        return "---\n" + entry + "---\n" + text;
    }
    std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    auto lines = splitLines(yaml);
    // a list written [a, b] stays that way
    if (value && value->isList && !value->list.empty()) {
        for (const auto& l : lines) {
            std::string k, rest;
            if (isKeyLine(l, &k, &rest) && lower(k) == lower(key) && startsWith(trim(rest), "[")) {
                entry = key + ": [";
                for (std::size_t i = 0; i < value->list.size(); ++i) {
                    std::string item = yamlScalar(value->list[i]);
                    if (item == value->list[i] && item.find_first_of(",[]") != std::string::npos) item = "\"" + item + "\"";
                    entry += (i ? ", " : "") + item;
                }
                entry += "]\n";
                break;
            }
        }
    }
    std::vector<std::string> out;
    bool replaced = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string k;
        if (isKeyLine(lines[i], &k, nullptr) && lower(k) == lower(key)) {
            std::size_t j = i + 1;
            while (j < lines.size() && (lines[j].empty() || lines[j][0] == ' ' || lines[j][0] == '\t' || startsWith(lines[j], "- "))) ++j;
            // keep trailing blank lines of the block
            while (j > i + 1 && trim(lines[j - 1]).empty()) --j;
            if (value && !replaced) {
                for (const auto& l : splitLines(entry)) out.push_back(l);
            }
            replaced = true;
            i = j - 1;
            continue;
        }
        out.push_back(lines[i]);
    }
    if (!replaced && value) {
        while (!out.empty() && trim(out.back()).empty()) out.pop_back();
        for (const auto& l : splitLines(entry)) out.push_back(l);
    }
    std::size_t prefix = startsWith(text, "\xEF\xBB\xBF") ? 3 : 0;
    std::string result = text.substr(0, prefix) + "---" + nl;
    for (const auto& l : out) result += l + nl;
    result += "---" + nl;
    return result + text.substr(bodyStart);
}

std::vector<std::string> frontmatterTags(const Frontmatter& fm) {
    std::vector<std::string> out;
    for (const char* key : {"tags", "tag"}) {
        const FmValue* v = fmFind(fm, key);
        if (!v) continue;
        std::vector<std::string> items = v->list;
        if (!v->isList) {
            std::string cur;
            for (char c : v->scalar) {
                if (c == ',' || c == ' ') {
                    if (!cur.empty()) items.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) items.push_back(cur);
        }
        for (auto t : items) {
            t = trim(t);
            while (!t.empty() && t[0] == '#') t.erase(0, 1);
            if (!t.empty()) out.push_back(t);
        }
    }
    return out;
}

std::vector<std::string> frontmatterAliases(const Frontmatter& fm) {
    std::vector<std::string> out;
    for (const char* key : {"aliases", "alias"}) {
        const FmValue* v = fmFind(fm, key);
        if (!v) continue;
        if (v->isList) {
            for (const auto& a : v->list) {
                if (!trim(a).empty()) out.push_back(trim(a));
            }
        } else {
            for (const auto& a : flowList(v->scalar)) out.push_back(trim(a));
        }
    }
    return out;
}

}  // namespace wetwaresuite::notes
