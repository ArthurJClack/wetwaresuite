// The pieces scripts and pipelines are made of: variables, substitution,
// wildcards, the YAML subset and conditions.
#include <algorithm>
#include <cctype>
#include <cstdlib>

#include <nlohmann/json.hpp>

#include "wetwaresuite/automate/automate.hpp"

namespace wetwaresuite::automate {

using nlohmann::json;

namespace {

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string escapeWord(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == ' ' || c == '\t' || c == '"' || c == '\\' || c == '#') out += '\\';
        out += c;
    }
    return out;
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) return s.substr(1, s.size() - 2);
    return s;
}

// Natural order: walk_2 before walk_10.
bool naturalLess(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
            while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
            std::string na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size() - 1));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size() - 1));
            if (na.size() != nb.size()) return na.size() < nb.size();
            if (na != nb) return na < nb;
            i = i2;
            j = j2;
            continue;
        }
        char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
        char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(b[j])));
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

bool matchSegment(const char* p, const char* pe, const char* s, const char* se) {
    const char *star = nullptr, *resume = nullptr;
    while (s < se) {
        if (p < pe && (*p == '?' || std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*s)))) {
            ++p;
            ++s;
        } else if (p < pe && *p == '*') {
            star = p++;
            resume = s;
        } else if (star) {
            p = star + 1;
            s = ++resume;
        } else {
            return false;
        }
    }
    while (p < pe && *p == '*') ++p;
    return p == pe;
}

std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool matchSegments(const std::vector<std::string>& pat, std::size_t pi, const std::vector<std::string>& str, std::size_t si) {
    if (pi == pat.size()) return si == str.size();
    if (pat[pi] == "**") {
        for (std::size_t k = si; k <= str.size(); ++k) {
            if (matchSegments(pat, pi + 1, str, k)) return true;
        }
        return false;
    }
    if (si == str.size()) return false;
    const auto& p = pat[pi];
    const auto& s = str[si];
    if (!matchSegment(p.data(), p.data() + p.size(), s.data(), s.data() + s.size())) return false;
    return matchSegments(pat, pi + 1, str, si + 1);
}

// ---- YAML subset ----------------------------------------------------------------

struct Line {
    int indent = 0;
    std::string text;
    int number = 0;
};

std::string stripComment(const std::string& s) {
    char quote = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            if (i == 0 || s[i - 1] == ' ' || s[i - 1] == ':' || s[i - 1] == '[' || s[i - 1] == ',' || s[i - 1] == '-') quote = c;
        } else if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            return s.substr(0, i);
        }
    }
    return s;
}

json scalar(const std::string& raw, int lineNo);

json flowList(const std::string& text, int lineNo) {
    // [a, "b, c", 3]
    json arr = json::array();
    std::string inner = text.substr(1, text.size() - 2);
    std::string cur;
    char quote = 0;
    for (char c : inner + ",") {
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
            cur += c;
        } else if (c == ',') {
            std::string t = trim(cur);
            if (!t.empty()) arr.push_back(scalar(t, lineNo));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return arr;
}

json scalar(const std::string& raw, int lineNo) {
    std::string t = trim(raw);
    if (t.empty() || t == "~" || t == "null") return nullptr;
    if (t.front() == '"' && t.back() == '"' && t.size() >= 2) {
        json j = json::parse(t, nullptr, false);  // JSON string escapes
        return j.is_discarded() ? json(t.substr(1, t.size() - 2)) : j;
    }
    if (t.front() == '\'' && t.back() == '\'' && t.size() >= 2) {
        std::string out;
        std::string body = t.substr(1, t.size() - 2);
        for (std::size_t i = 0; i < body.size(); ++i) {
            out += body[i];
            if (body[i] == '\'' && i + 1 < body.size() && body[i + 1] == '\'') ++i;
        }
        return out;
    }
    if (t.front() == '[' && t.back() == ']') return flowList(t, lineNo);
    if (t.front() == '{' && t.back() == '}') {
        json j = json::parse(t, nullptr, false);
        if (!j.is_discarded()) return j;
    }
    if (t == "true" || t == "yes") return true;
    if (t == "false" || t == "no") return false;
    char* end = nullptr;
    long long iv = std::strtoll(t.c_str(), &end, 10);
    if (end && *end == 0) return iv;
    double dv = std::strtod(t.c_str(), &end);
    if (end && *end == 0 && t.find_first_of("0123456789") != std::string::npos) return dv;
    return t;
}

// Finds "key: value" (or "key:") outside quotes; npos when the line isn't a mapping entry.
std::size_t keyColon(const std::string& text) {
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if ((c == '"' || c == '\'') && i == 0) {
            quote = c;
        } else if (c == ':' && (i + 1 == text.size() || text[i + 1] == ' ' || text[i + 1] == '\t')) {
            return i;
        } else if (c == ' ' && i == 0) {
            return std::string::npos;
        }
    }
    return std::string::npos;
}

bool isItem(const Line& l) { return l.text == "-" || l.text.rfind("- ", 0) == 0; }

json parseBlock(std::vector<Line>& lines, std::size_t& i, int indent);

json parseSequence(std::vector<Line>& lines, std::size_t& i, int indent) {
    json arr = json::array();
    while (i < lines.size() && lines[i].indent == indent && isItem(lines[i])) {
        std::string rest = lines[i].text.size() > 1 ? lines[i].text.substr(2) : "";
        std::size_t lead = rest.find_first_not_of(' ');
        if (lead == std::string::npos) {
            ++i;
            if (i < lines.size() && lines[i].indent > indent) arr.push_back(parseBlock(lines, i, lines[i].indent));
            else arr.push_back(nullptr);
            continue;
        }
        rest = rest.substr(lead);
        const int childIndent = indent + 2 + static_cast<int>(lead);
        if (keyColon(rest) != std::string::npos || isItem({0, rest, 0})) {
            // "- key: value" starts a mapping whose keys line up with "key"
            lines[i].indent = childIndent;
            lines[i].text = rest;
            arr.push_back(parseBlock(lines, i, childIndent));
        } else {
            arr.push_back(scalar(rest, lines[i].number));
            ++i;
        }
    }
    return arr;
}

json parseMapping(std::vector<Line>& lines, std::size_t& i, int indent) {
    json obj = json::object();
    while (i < lines.size() && lines[i].indent == indent && !isItem(lines[i])) {
        const Line& l = lines[i];
        std::size_t colon = keyColon(l.text);
        if (colon == std::string::npos) throw ParamError("line " + std::to_string(l.number) + ": expected 'key: value', got '" + l.text + "'");
        std::string key = unquote(trim(l.text.substr(0, colon)));
        std::string value = trim(l.text.substr(colon + 1));
        const int number = l.number;
        ++i;
        if (!value.empty() && value != "|" && value != ">") {
            obj[key] = scalar(value, number);
            continue;
        }
        if (value == "|" || value == ">") {
            // block text: every more-indented line, joined with newlines (|) or spaces (>)
            std::string text;
            while (i < lines.size() && lines[i].indent > indent) {
                text += (text.empty() ? "" : (value == "|" ? "\n" : " ")) + std::string(static_cast<std::size_t>(lines[i].indent - indent - 2 > 0 ? lines[i].indent - indent - 2 : 0), ' ') + lines[i].text;
                ++i;
            }
            obj[key] = text;
            continue;
        }
        if (i < lines.size() && lines[i].indent > indent) obj[key] = parseBlock(lines, i, lines[i].indent);
        else if (i < lines.size() && lines[i].indent == indent && isItem(lines[i])) obj[key] = parseSequence(lines, i, indent);
        else obj[key] = nullptr;
    }
    return obj;
}

json parseBlock(std::vector<Line>& lines, std::size_t& i, int indent) {
    if (i >= lines.size()) return nullptr;
    return isItem(lines[i]) ? parseSequence(lines, i, indent) : parseMapping(lines, i, indent);
}

}  // namespace

Vars parseVars(const std::string& text) {
    Vars vars;
    std::string t = trim(text);
    if (t.empty()) return vars;
    if (t.front() == '{') {
        json j = json::parse(t, nullptr, false);
        if (j.is_discarded() || !j.is_object()) throw ParamError("vars looks like JSON but isn't a valid object");
        for (const auto& [k, v] : j.items()) vars[k] = v.is_string() ? v.get<std::string>() : v.dump();
        return vars;
    }
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '\\' && i + 1 < t.size() && (t[i + 1] == ',' || t[i + 1] == '\\')) {
            cur += t[++i];
        } else if (c == ',' || c == '\n' || c == ';') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    for (const auto& raw : parts) {
        std::string part = trim(raw);
        if (part.empty()) continue;
        auto eq = part.find('=');
        if (eq == std::string::npos || eq == 0) throw ParamError("vars must be name=value pairs, got '" + part + "'");
        std::string name = trim(part.substr(0, eq));
        if (!name.empty() && name[0] == '$') name.erase(0, 1);
        for (char c : name) {
            if (!identChar(c) && c != '.') throw ParamError("variable names use letters, digits and _: '" + name + "'");
        }
        vars[name] = unquote(trim(part.substr(eq + 1)));
    }
    return vars;
}

std::string substitute(const std::string& text, const Vars& vars, bool escape) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '$') {  // \$ is a literal dollar
            out += escape ? "\\$" : "$";  // command lines keep the escape for the batch parser
            ++i;
            continue;
        }
        if (c != '$' || i + 1 >= text.size()) {
            out += c;
            continue;
        }
        if (text[i + 1] == '{') {
            auto close = text.find('}', i + 2);
            if (close != std::string::npos) {
                std::string name = text.substr(i + 2, close - i - 2);
                auto it = vars.find(name);
                if (it != vars.end()) {
                    out += escape ? escapeWord(it->second) : it->second;
                    i = close;
                    continue;
                }
            }
            out += c;
            continue;
        }
        std::size_t j = i + 1;
        while (j < text.size() && (identChar(text[j]) || (text[j] == '.' && j + 1 < text.size() && identChar(text[j + 1])))) ++j;
        std::string name = text.substr(i + 1, j - i - 1);
        // longest defined dotted prefix: $img.width, else $img followed by ".width"
        while (!name.empty()) {
            auto it = vars.find(name);
            if (it != vars.end()) break;
            auto dot = name.find_last_of('.');
            if (dot == std::string::npos) {
                name.clear();
                break;
            }
            name.resize(dot);
        }
        if (name.empty()) {
            out += c;
            continue;
        }
        const std::string& v = vars.at(name);
        out += escape ? escapeWord(v) : v;
        i += name.size();
    }
    return out;
}

bool globMatch(const std::string& pattern, const std::string& path) {
    return matchSegments(segments(pattern), 0, segments(path), 0);
}

std::vector<fs::path> expandGlob(const std::string& pattern, const fs::path& base) {
    std::vector<fs::path> out;
    auto segs = segments(pattern);
    // the fixed folder part comes first
    fs::path root;
    const bool absolute = !pattern.empty() && (pattern[0] == '/' || pattern[0] == '\\' || (pattern.size() > 1 && pattern[1] == ':'));
    if (absolute) {
        if (pattern[0] == '/' || pattern[0] == '\\') root = "/";
    } else {
        root = base;
    }
    std::size_t k = 0;
    if (absolute && pattern.size() > 1 && pattern[1] == ':') {
        root = segs[0] + "/";
        k = 1;
    }
    for (; k < segs.size() && segs[k].find_first_of("*?") == std::string::npos; ++k) root /= segs[k];
    std::error_code ec;
    if (k == segs.size()) {  // no wildcards: the file itself
        if (fs::is_regular_file(root, ec)) out.push_back(root);
        return out;
    }
    std::vector<std::string> rest(segs.begin() + static_cast<std::ptrdiff_t>(k), segs.end());
    const fs::path start = root.empty() ? fs::path(".") : root;
    const bool deep = rest.size() > 1 || rest[0] == "**";
    auto consider = [&](const fs::directory_entry& e) {
        std::error_code fec;
        if (!e.is_regular_file(fec)) return;
        auto rel = e.path().lexically_relative(start).generic_string();
        if (matchSegments(rest, 0, segments(rel), 0)) out.push_back(root.empty() ? e.path().lexically_relative(".") : e.path());
    };
    if (deep) {
        for (fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            consider(*it);
        }
    } else {
        for (const auto& e : fs::directory_iterator(start, fs::directory_options::skip_permission_denied, ec)) consider(e);
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return naturalLess(a.generic_string(), b.generic_string()); });
    return out;
}

std::string pipelineToJson(const std::string& text) {
    std::string t = trim(text);
    if (!t.empty() && (t.front() == '{' || t.front() == '[')) {
        try {
            return json::parse(t).dump();
        } catch (const json::parse_error& e) {
            throw ParamError(std::string("pipeline JSON: ") + e.what());
        }
    }
    std::vector<Line> lines;
    int number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        std::string raw = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        ++number;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (raw.find('\t') != std::string::npos && raw.find_first_not_of(" \t") != std::string::npos &&
            raw.find('\t') < raw.find_first_not_of(" \t")) {
            throw ParamError("line " + std::to_string(number) + ": indent with spaces, not tabs");
        }
        std::string s = stripComment(raw);
        std::size_t ind = s.find_first_not_of(' ');
        if (ind == std::string::npos) continue;
        std::string body = trim(s);
        if (body == "---") continue;
        lines.push_back({static_cast<int>(ind), body, number});
    }
    if (lines.empty()) throw ParamError("the pipeline is empty");
    std::size_t i = 0;
    json root = parseBlock(lines, i, lines[0].indent);
    if (i < lines.size()) throw ParamError("line " + std::to_string(lines[i].number) + ": unexpected indentation");
    return root.dump();
}

bool evalCondition(const std::string& condition, const Vars& vars) {
    std::string c = trim(substitute(condition, vars));
    if (c.empty()) return false;
    std::string lc = lower(c);
    if (lc.rfind("not ", 0) == 0 && lc.rfind("not in ", 0) != 0) return !evalCondition(c.substr(4), vars);
    if (lc == "true" || lc == "yes" || lc == "1") return true;
    if (lc == "false" || lc == "no" || lc == "0") return false;
    if (lc.rfind("exists ", 0) == 0) {
        std::error_code ec;
        return fs::exists(unquote(trim(c.substr(7))), ec);
    }
    if (lc.rfind("empty ", 0) == 0) return trim(c.substr(6)).empty();
    static const std::vector<std::string> ops = {" not in ", " in ", " contains ", " matches ", " == ", " != ", " >= ", " <= ", " > ", " < ", " = "};
    for (const auto& op : ops) {
        auto pos = lc.find(op);
        if (pos == std::string::npos) continue;
        std::string a = unquote(trim(c.substr(0, pos))), b = unquote(trim(c.substr(pos + op.size())));
        if (lower(a) == "ext") {  // bare ext: the current file's extension
            auto it = vars.find("file.ext");
            a = it != vars.end() ? it->second : "";
        }
        const std::string la = lower(a), lb = lower(b);
        if (op == " in " || op == " not in ") {
            bool found = false;
            std::string item;
            for (char ch : lb + ",") {
                if (ch == ',') {
                    std::string x = trim(item);
                    if (!x.empty() && x[0] == '.') x.erase(0, 1);
                    std::string y = la;
                    if (!y.empty() && y[0] == '.') y.erase(0, 1);
                    found = found || x == y;
                    item.clear();
                } else {
                    item += ch;
                }
            }
            return op == " in " ? found : !found;
        }
        if (op == " contains ") return la.find(lb) != std::string::npos;
        if (op == " matches ") return globMatch(b, a);
        if (op == " == " || op == " = ") return la == lb;
        if (op == " != ") return la != lb;
        char* ea = nullptr;
        char* eb = nullptr;
        double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
        if (a.empty() || b.empty() || *ea || *eb) throw ParamError("'" + c + "' compares something that isn't a number");
        if (op == " >= ") return x >= y;
        if (op == " <= ") return x <= y;
        if (op == " > ") return x > y;
        return x < y;
    }
    // a lone value: true unless empty, 0 or false
    return true;
}

}  // namespace wetwaresuite::automate
