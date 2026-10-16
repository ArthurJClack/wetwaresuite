// A small CommonMark-ish Markdown to HTML renderer with Obsidian's extensions.
#include <cctype>
#include <map>

#include "wetwaresuite/notes/notes.hpp"

namespace wetwaresuite::notes {

namespace {

bool blank(const std::string& l) { return l.find_first_not_of(" \t") == std::string::npos; }

std::size_t indentOf(const std::string& l) {
    std::size_t i = 0;
    while (i < l.size() && l[i] == ' ') ++i;
    return i;
}

std::string dedent(const std::string& l, std::size_t n) {
    std::size_t i = 0;
    while (i < l.size() && i < n && l[i] == ' ') ++i;
    return l.substr(i);
}

bool isPunct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool isWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n'; }

bool fenceLine(const std::string& l, char& ch, std::size_t& len, std::string& info) {
    std::size_t i = indentOf(l);
    if (i > 3 || i >= l.size() || (l[i] != '`' && l[i] != '~')) return false;
    char c = l[i];
    std::size_t n = 0;
    while (i + n < l.size() && l[i + n] == c) ++n;
    if (n < 3) return false;
    info = trim(l.substr(i + n));
    if (c == '`' && info.find('`') != std::string::npos) return false;
    ch = c;
    len = n;
    return true;
}

bool hrLine(const std::string& l) {
    std::size_t i = indentOf(l);
    if (i > 3 || i >= l.size()) return false;
    char c = l[i];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (std::size_t k = i; k < l.size(); ++k) {
        if (l[k] == c) ++count;
        else if (l[k] != ' ' && l[k] != '\t') return false;
    }
    return count >= 3;
}

bool atx(const std::string& l, int& level, std::string& text) {
    std::size_t i = indentOf(l);
    if (i > 3) return false;
    std::size_t n = 0;
    while (i + n < l.size() && l[i + n] == '#') ++n;
    if (n == 0 || n > 6 || (i + n < l.size() && l[i + n] != ' ' && l[i + n] != '\t')) return false;
    std::string t = trim(l.substr(i + n));
    std::size_t e = t.size();
    while (e > 0 && t[e - 1] == '#') --e;
    if (e < t.size() && (e == 0 || t[e - 1] == ' ')) t = trim(t.substr(0, e));
    level = static_cast<int>(n);
    text = t;
    return true;
}

struct Marker {
    bool ordered = false;
    char bullet = '-';
    int start = 1;
    std::size_t indent = 0;
    std::size_t content = 0;  // column where the item's text starts
};

std::optional<Marker> listMarker(const std::string& l) {
    Marker m;
    std::size_t i = indentOf(l);
    m.indent = i;
    if (i >= l.size()) return std::nullopt;
    std::size_t after;
    if (l[i] == '-' || l[i] == '*' || l[i] == '+') {
        m.bullet = l[i];
        after = i + 1;
    } else {
        std::size_t d = i;
        while (d < l.size() && d - i < 9 && std::isdigit(static_cast<unsigned char>(l[d]))) ++d;
        if (d == i || d >= l.size() || (l[d] != '.' && l[d] != ')')) return std::nullopt;
        m.ordered = true;
        m.bullet = l[d];
        m.start = std::stoi(l.substr(i, d - i));
        after = d + 1;
    }
    if (after < l.size() && l[after] != ' ' && l[after] != '\t') return std::nullopt;
    std::size_t sp = 0;
    while (after + sp < l.size() && l[after + sp] == ' ') ++sp;
    if (after + sp >= l.size()) sp = 1;     // empty item
    else if (sp > 4 || sp == 0) sp = 1;
    m.content = after + sp;
    return m;
}

bool delimiterRow(const std::string& l) {
    std::string t = trim(l);
    if (t.find('-') == std::string::npos) return false;
    bool sawPipe = t.find('|') != std::string::npos;
    for (char c : t) {
        if (c != '|' && c != '-' && c != ':' && c != ' ' && c != '\t') return false;
    }
    return sawPipe || t.find("--") != std::string::npos;
}

std::vector<std::string> splitCells(const std::string& row) {
    std::string t = trim(row);
    if (!t.empty() && t.front() == '|') t.erase(0, 1);
    if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\')) t.pop_back();
    std::vector<std::string> cells;
    std::string cur;
    bool inCode = false;
    int wiki = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += wiki ? "\\|" : "|";
            ++i;
            continue;
        }
        if (c == '`') inCode = !inCode;
        if (t.compare(i, 2, "[[") == 0) ++wiki;
        if (t.compare(i, 2, "]]") == 0 && wiki) --wiki;
        if (c == '|' && !inCode && !wiki) {
            cells.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    cells.push_back(trim(cur));
    return cells;
}

const std::set<std::string>& imageExts() {
    static const std::set<std::string> e = {"png", "jpg", "jpeg", "gif", "svg", "webp", "bmp", "avif", "ico"};
    return e;
}

std::string extOf(const std::string& path) {
    auto dot = path.rfind('.');
    auto slash = path.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(path.substr(dot + 1));
}

class Renderer {
public:
    explicit Renderer(const RenderHooks& h) : hooks_(h) {}

    std::string blocks(const std::vector<std::string>& lines, bool tight) {
        std::string out;
        std::vector<std::string> para;
        auto flush = [&] {
            if (para.empty()) return;
            std::string text;
            for (std::size_t k = 0; k < para.size(); ++k) text += (k ? "\n" : "") + trim(para[k]);
            para.clear();
            std::string html = inlines(stripBlockIds(text));
            // a paragraph that is only an embedded note shouldn't wrap a <div> in a <p>
            bool block = startsWith(html, "<div") && endsWith(html, "</div>");
            out += tight || block ? html + "\n" : "<p>" + html + "</p>\n";
        };
        std::size_t i = 0;
        const std::size_t n = lines.size();
        while (i < n) {
            const std::string& l = lines[i];
            if (blank(l)) {
                flush();
                ++i;
                continue;
            }
            char fch;
            std::size_t flen;
            std::string info;
            int level;
            std::string htext;
            if (para.empty() && indentOf(l) >= 4) {
                std::vector<std::string> code;
                while (i < n && (blank(lines[i]) || indentOf(lines[i]) >= 4)) code.push_back(dedent(lines[i++], 4));
                while (!code.empty() && blank(code.back())) code.pop_back();
                std::string body;
                for (const auto& c : code) body += c + "\n";
                out += "<pre><code>" + htmlEscape(body) + "</code></pre>\n";
                continue;
            }
            if (fenceLine(l, fch, flen, info)) {
                flush();
                std::size_t ind = indentOf(l);
                std::string body;
                ++i;
                while (i < n) {
                    char c2;
                    std::size_t l2;
                    std::string i2;
                    if (fenceLine(lines[i], c2, l2, i2) && c2 == fch && l2 >= flen && i2.empty()) {
                        ++i;
                        break;
                    }
                    body += dedent(lines[i], ind) + "\n";
                    ++i;
                }
                std::string lang = info.substr(0, info.find(' '));
                if (lang == "mermaid") {
                    out += "<pre class=\"mermaid\">" + htmlEscape(body) + "</pre>\n";
                } else {
                    out += "<pre><code" + (lang.empty() ? std::string() : " class=\"language-" + htmlEscape(lang) + "\"") + ">" +
                           htmlEscape(body) + "</code></pre>\n";
                }
                continue;
            }
            if (atx(l, level, htext)) {
                flush();
                out += heading(level, htext);
                ++i;
                continue;
            }
            if (!para.empty() && indentOf(l) < 4) {
                std::string t = trim(l);
                bool eq = !t.empty() && t.find_first_not_of('=') == std::string::npos;
                bool dash = !t.empty() && t.find_first_not_of('-') == std::string::npos;
                if (eq || dash) {
                    std::string text;
                    for (std::size_t k = 0; k < para.size(); ++k) text += (k ? " " : "") + trim(para[k]);
                    para.clear();
                    out += heading(eq ? 1 : 2, text);
                    ++i;
                    continue;
                }
            }
            if (hrLine(l)) {
                flush();
                out += "<hr>\n";
                ++i;
                continue;
            }
            std::size_t ind = indentOf(l);
            if (ind <= 3 && l[ind] == '>') {
                flush();
                std::vector<std::string> inner;
                while (i < n && !blank(lines[i])) {
                    const std::string& q = lines[i];
                    std::size_t qi = indentOf(q);
                    if (qi <= 3 && qi < q.size() && q[qi] == '>') {
                        std::string rest = q.substr(qi + 1);
                        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
                        inner.push_back(rest);
                    } else if (!inner.empty() && !blank(inner.back())) {
                        inner.push_back(q);  // lazy continuation
                    } else {
                        break;
                    }
                    ++i;
                }
                out += quote(inner);
                continue;
            }
            if (l.find('|') != std::string::npos && i + 1 < n && delimiterRow(lines[i + 1])) {
                auto head = splitCells(l);
                auto delim = splitCells(lines[i + 1]);
                flush();
                std::vector<std::string> align;
                for (const auto& d : delim) {
                    bool left = !d.empty() && d.front() == ':', right = !d.empty() && d.back() == ':';
                    align.push_back(left && right ? "center" : right ? "right" : left ? "left" : "");
                }
                auto cellTag = [&](const char* tag, const std::string& text, std::size_t col) {
                    std::string a = col < align.size() && !align[col].empty() ? " style=\"text-align:" + align[col] + "\"" : "";
                    return std::string("<") + tag + a + ">" + inlines(text) + "</" + tag + ">";
                };
                out += "<table>\n<thead><tr>";
                for (std::size_t c = 0; c < head.size(); ++c) out += cellTag("th", head[c], c);
                out += "</tr></thead>\n<tbody>\n";
                i += 2;
                while (i < n && !blank(lines[i]) && lines[i].find('|') != std::string::npos) {
                    auto cells = splitCells(lines[i]);
                    out += "<tr>";
                    for (std::size_t c = 0; c < head.size(); ++c) out += cellTag("td", c < cells.size() ? cells[c] : "", c);
                    out += "</tr>\n";
                    ++i;
                }
                out += "</tbody>\n</table>\n";
                continue;
            }
            if (auto m = listMarker(l); m && (para.empty() || !m->ordered || m->start == 1) && !(para.empty() == false && blank(l.substr(m->content < l.size() ? m->content : l.size())))) {
                flush();
                out += list(lines, i);
                continue;
            }
            if (ind <= 3 && l[ind] == '<' && ind + 1 < l.size() &&
                (std::isalpha(static_cast<unsigned char>(l[ind + 1])) || l[ind + 1] == '/' || l[ind + 1] == '!')) {
                flush();
                while (i < n && !blank(lines[i])) out += lines[i++] + "\n";
                continue;
            }
            if (startsWith(trim(l), "$$")) {
                flush();
                std::string body = trim(l).substr(2);
                bool closed = endsWith(trim(body), "$$");
                if (closed) body = trim(body).substr(0, trim(body).size() - 2);
                ++i;
                while (!closed && i < n) {
                    std::string t = trim(lines[i++]);
                    if (endsWith(t, "$$")) {
                        body += "\n" + t.substr(0, t.size() - 2);
                        break;
                    }
                    body += "\n" + t;
                }
                out += "<div class=\"math math-display\">$$" + htmlEscape(body) + "$$</div>\n";
                continue;
            }
            para.push_back(l);
            ++i;
        }
        flush();
        return out;
    }

    std::string inlines(const std::string& s) {
        std::string out;
        std::size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.size()) {
                if (s[i + 1] == '\n') {
                    out += "<br>\n";
                    i += 2;
                    continue;
                }
                if (isPunct(s[i + 1])) {
                    out += htmlEscape(std::string(1, s[i + 1]));
                    i += 2;
                    continue;
                }
            }
            if (c == '`') {
                std::size_t n = 0;
                while (i + n < s.size() && s[i + n] == '`') ++n;
                std::size_t close = findRun(s, i + n, '`', n);
                if (close != std::string::npos) {
                    std::string code = s.substr(i + n, close - i - n);
                    for (auto& ch : code) {
                        if (ch == '\n') ch = ' ';
                    }
                    if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && !blank(code)) code = code.substr(1, code.size() - 2);
                    out += "<code>" + htmlEscape(code) + "</code>";
                    i = close + n;
                } else {
                    out += std::string(n, '`');
                    i += n;
                }
                continue;
            }
            if ((c == '!' && s.compare(i + 1, 2, "[[") == 0) || s.compare(i, 2, "[[") == 0) {
                bool embed = c == '!';
                std::size_t open = embed ? i + 3 : i + 2;
                auto close = s.find("]]", open);
                if (close != std::string::npos && s.substr(open, close - open).find('\n') == std::string::npos) {
                    out += wikilink(s.substr(open, close - open), embed);
                    i = close + 2;
                    continue;
                }
            }
            if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {
                std::size_t end;
                std::string text, dest, title;
                if (bracketLink(s, i + 1, end, text, dest, title)) {
                    out += image(text, dest, title);
                    i = end;
                    continue;
                }
            }
            if (c == '[') {
                std::size_t end;
                std::string text, dest, title;
                if (bracketLink(s, i, end, text, dest, title)) {
                    out += anchor(text, dest, title);
                    i = end;
                    continue;
                }
            }
            if (c == '<') {
                auto close = s.find('>', i + 1);
                if (close != std::string::npos) {
                    std::string inner = s.substr(i + 1, close - i - 1);
                    if (inner.find(' ') == std::string::npos && (inner.find("://") != std::string::npos || startsWith(lower(inner), "mailto:"))) {
                        out += "<a href=\"" + htmlEscape(inner) + "\">" + htmlEscape(inner) + "</a>";
                        i = close + 1;
                        continue;
                    }
                    if (inner.find('@') != std::string::npos && inner.find(' ') == std::string::npos && inner.find('/') == std::string::npos) {
                        out += "<a href=\"mailto:" + htmlEscape(inner) + "\">" + htmlEscape(inner) + "</a>";
                        i = close + 1;
                        continue;
                    }
                    if (!inner.empty() && (std::isalpha(static_cast<unsigned char>(inner[0])) || inner[0] == '/' || inner[0] == '!')) {
                        out += "<" + inner + ">";  // raw inline HTML
                        i = close + 1;
                        continue;
                    }
                }
                out += "&lt;";
                ++i;
                continue;
            }
            if (c == '*' || c == '_') {
                std::size_t k = 0;
                while (i + k < s.size() && s[i + k] == c) ++k;
                char prev = i > 0 ? s[i - 1] : ' ';
                char next = i + k < s.size() ? s[i + k] : ' ';
                bool canOpen = !isSpace(next) && !(c == '_' && isWord(prev));
                if (canOpen && k <= 3) {
                    std::size_t close = findEmphasisClose(s, i + k, c, k);
                    if (close != std::string::npos) {
                        std::string inner = inlines(s.substr(i + k, close - i - k));
                        if (k == 1) out += "<em>" + inner + "</em>";
                        else if (k == 2) out += "<strong>" + inner + "</strong>";
                        else out += "<em><strong>" + inner + "</strong></em>";
                        i = close + k;
                        continue;
                    }
                }
                out += std::string(k, c);
                i += k;
                continue;
            }
            if ((c == '~' || c == '=') && i + 1 < s.size() && s[i + 1] == c && i + 2 < s.size() && !isSpace(s[i + 2])) {
                auto close = s.find(std::string(2, c), i + 2);
                if (close != std::string::npos && !isSpace(s[close - 1])) {
                    std::string inner = inlines(s.substr(i + 2, close - i - 2));
                    out += c == '~' ? "<del>" + inner + "</del>" : "<mark>" + inner + "</mark>";
                    i = close + 2;
                    continue;
                }
            }
            if (c == '#' && (i == 0 || isSpace(s[i - 1]) || s[i - 1] == '(')) {
                std::size_t j = i + 1;
                while (j < s.size() && isTagChar(static_cast<unsigned char>(s[j]))) ++j;
                std::string tag = s.substr(i + 1, j - i - 1);
                while (!tag.empty() && tag.back() == '/') tag.pop_back();
                bool letter = false;
                for (char ch : tag) letter = letter || !std::isdigit(static_cast<unsigned char>(ch));
                if (!tag.empty() && letter) {
                    std::string href = hooks_.tag ? hooks_.tag(tag) : "";
                    out += href.empty() ? "<span class=\"tag\">#" + htmlEscape(tag) + "</span>"
                                        : "<a class=\"tag\" href=\"" + htmlEscape(href) + "\">#" + htmlEscape(tag) + "</a>";
                    i = i + 1 + tag.size();
                    continue;
                }
            }
            if ((c == 'h' || c == 'w') && (i == 0 || !isWord(s[i - 1])) &&
                (s.compare(i, 7, "http://") == 0 || s.compare(i, 8, "https://") == 0 || s.compare(i, 4, "www.") == 0)) {
                std::size_t j = i;
                while (j < s.size() && !isSpace(s[j]) && s[j] != '<' && s[j] != '>') ++j;
                while (j > i && std::string(".,:;!?*_~'\")").find(s[j - 1]) != std::string::npos) {
                    if (s[j - 1] == ')' && s.substr(i, j - i).find('(') != std::string::npos) break;
                    --j;
                }
                std::string url = s.substr(i, j - i);
                std::string href = startsWith(url, "www.") ? "http://" + url : url;
                out += "<a href=\"" + htmlEscape(href) + "\">" + htmlEscape(url) + "</a>";
                i = j;
                continue;
            }
            if (c == '$' && i + 1 < s.size() && s[i + 1] != '$' && !isSpace(s[i + 1])) {
                auto close = s.find('$', i + 1);
                if (close != std::string::npos && !isSpace(s[close - 1]) &&
                    (close + 1 >= s.size() || !std::isdigit(static_cast<unsigned char>(s[close + 1])))) {
                    out += "<span class=\"math\">$" + htmlEscape(s.substr(i + 1, close - i - 1)) + "$</span>";
                    i = close + 1;
                    continue;
                }
            }
            if (c == '&') {
                std::size_t j = i + 1;
                while (j < s.size() && j - i < 10 && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '#')) ++j;
                if (j < s.size() && s[j] == ';' && j > i + 1) {
                    out += s.substr(i, j - i + 1);
                    i = j + 1;
                    continue;
                }
                out += "&amp;";
                ++i;
                continue;
            }
            if (c == '\n') {
                if (endsWith(out, " ")) {
                    while (endsWith(out, " ")) out.pop_back();
                }
                out += "<br>\n";
                ++i;
                continue;
            }
            out += htmlEscape(std::string(1, c));
            ++i;
        }
        return out;
    }

private:
    const RenderHooks& hooks_;
    std::map<std::string, int> ids_;

    static std::string stripBlockIds(const std::string& text) {
        // " ^block-id" at the end of a line becomes an anchor
        std::string out;
        std::size_t start = 0;
        while (start <= text.size()) {
            auto nl = text.find('\n', start);
            std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            auto caret = line.rfind(" ^");
            if (caret != std::string::npos) {
                std::string id = line.substr(caret + 2);
                bool ok = !id.empty();
                for (char c : id) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '-');
                if (ok) line = "<a id=\"^" + id + "\"></a>" + line.substr(0, caret);
            }
            out += line;
            if (nl == std::string::npos) break;
            out += "\n";
            start = nl + 1;
        }
        return out;
    }

    static std::size_t findRun(const std::string& s, std::size_t from, char c, std::size_t n) {
        std::size_t j = from;
        while (j < s.size()) {
            if (s[j] == c) {
                std::size_t r = 0;
                while (j + r < s.size() && s[j + r] == c) ++r;
                if (r == n) return j;
                j += r;
            } else {
                ++j;
            }
        }
        return std::string::npos;
    }

    static std::size_t findEmphasisClose(const std::string& s, std::size_t from, char c, std::size_t k) {
        std::size_t j = from;
        while (j < s.size()) {
            char ch = s[j];
            if (ch == '\\') {
                j += 2;
                continue;
            }
            if (ch == '`') {
                std::size_t n = 0;
                while (j + n < s.size() && s[j + n] == '`') ++n;
                std::size_t close = findRun(s, j + n, '`', n);
                j = close == std::string::npos ? j + n : close + n;
                continue;
            }
            if (s.compare(j, 2, "[[") == 0) {
                auto close = s.find("]]", j + 2);
                if (close != std::string::npos) {
                    j = close + 2;
                    continue;
                }
            }
            if (ch == c) {
                std::size_t r = 0;
                while (j + r < s.size() && s[j + r] == c) ++r;
                char prev = s[j - 1];
                char next = j + r < s.size() ? s[j + r] : ' ';
                bool rightFlank = !isSpace(prev) && j > from;
                bool leftFlank = !isSpace(next);
                if (c == '_' && isWord(next)) rightFlank = false;
                if (rightFlank && r == k) return j;
                if (rightFlank && r > k && !leftFlank) return j + (r - k);
                j += r;
                continue;
            }
            ++j;
        }
        return std::string::npos;
    }

    static bool bracketLink(const std::string& s, std::size_t at, std::size_t& end, std::string& text, std::string& dest, std::string& title) {
        int depth = 0;
        std::size_t j = at;
        std::size_t close = std::string::npos;
        for (; j < s.size(); ++j) {
            if (s[j] == '\\') {
                ++j;
                continue;
            }
            if (s[j] == '[') ++depth;
            if (s[j] == ']' && --depth == 0) {
                close = j;
                break;
            }
        }
        if (close == std::string::npos || close + 1 >= s.size() || s[close + 1] != '(') return false;
        std::size_t k = close + 2;
        while (k < s.size() && isSpace(s[k])) ++k;
        std::string d;
        if (k < s.size() && s[k] == '<') {
            auto gt = s.find('>', k);
            if (gt == std::string::npos) return false;
            d = s.substr(k + 1, gt - k - 1);
            k = gt + 1;
        } else {
            int pd = 0;
            while (k < s.size() && !isSpace(s[k])) {
                if (s[k] == '(') ++pd;
                if (s[k] == ')') {
                    if (pd == 0) break;
                    --pd;
                }
                d += s[k++];
            }
        }
        while (k < s.size() && isSpace(s[k])) ++k;
        std::string t;
        if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {
            char q = s[k];
            auto qe = s.find(q, k + 1);
            if (qe == std::string::npos) return false;
            t = s.substr(k + 1, qe - k - 1);
            k = qe + 1;
            while (k < s.size() && isSpace(s[k])) ++k;
        }
        if (k >= s.size() || s[k] != ')') return false;
        text = s.substr(at + 1, close - at - 1);
        dest = d;
        title = t;
        end = k + 1;
        return true;
    }

    static bool external(const std::string& dest) {
        std::string l = lower(dest);
        return l.empty() || l[0] == '#' || l.find("://") != std::string::npos || startsWith(l, "mailto:") || startsWith(l, "data:") ||
               startsWith(l, "tel:") || startsWith(l, "obsidian:");
    }

    std::string anchor(const std::string& text, const std::string& dest, const std::string& title) {
        std::string href = dest;
        std::string cls;
        if (!external(dest) && hooks_.link) {
            Link link;
            link.wiki = false;
            std::string target = dest;
            if (auto h = target.find('#'); h != std::string::npos) {
                link.heading = urlDecode(target.substr(h + 1));
                target = target.substr(0, h);
            }
            link.target = urlDecode(target);
            link.alias = text;
            if (auto r = hooks_.link(link)) {
                href = *r;
                cls = " class=\"internal-link\"";
            }
        }
        std::string t = title.empty() ? "" : " title=\"" + htmlEscape(title) + "\"";
        return "<a" + cls + " href=\"" + htmlEscape(href) + "\"" + t + ">" + inlines(text) + "</a>";
    }

    std::string image(const std::string& rawAlt, const std::string& dest, const std::string& title) {
        std::string alt = rawAlt;
        std::string size;
        if (auto bar = alt.rfind('|'); bar != std::string::npos) {
            std::string w = trim(alt.substr(bar + 1));
            if (!w.empty() && w.find_first_not_of("0123456789x") == std::string::npos) {
                auto x = w.find('x');
                size = " width=\"" + w.substr(0, x) + "\"" + (x != std::string::npos ? " height=\"" + w.substr(x + 1) + "\"" : "");
                alt = alt.substr(0, bar);
            }
        }
        std::string src = dest;
        if (!external(dest) && hooks_.image) src = hooks_.image(urlDecode(dest));
        std::string t = title.empty() ? "" : " title=\"" + htmlEscape(title) + "\"";
        return "<img src=\"" + htmlEscape(src) + "\" alt=\"" + htmlEscape(alt) + "\"" + size + t + ">";
    }

    std::string wikilink(const std::string& rawInner, bool embed) {
        Link link;
        link.embed = embed;
        std::string inner = rawInner;
        if (auto bar = inner.find('|'); bar != std::string::npos) {
            std::size_t cut = bar > 0 && inner[bar - 1] == '\\' ? bar - 1 : bar;
            link.alias = inner.substr(bar + 1);
            inner = inner.substr(0, cut);
        }
        if (auto h = inner.find('#'); h != std::string::npos) {
            link.heading = trim(inner.substr(h + 1));
            inner = inner.substr(0, h);
        }
        link.target = trim(inner);
        if (embed) {
            if (hooks_.embed) {
                if (auto html = hooks_.embed(link, hooks_.depth)) return *html;
            }
            const std::string ext = extOf(link.target);
            if (imageExts().count(ext)) {
                std::string src = hooks_.image ? hooks_.image(link.target) : urlEncodePath(link.target);
                std::string size;
                std::string w = trim(link.alias);
                if (!w.empty() && w.find_first_not_of("0123456789x") == std::string::npos) {
                    auto x = w.find('x');
                    size = " width=\"" + w.substr(0, x) + "\"" + (x != std::string::npos ? " height=\"" + w.substr(x + 1) + "\"" : "");
                }
                return "<img src=\"" + htmlEscape(src) + "\" alt=\"" + htmlEscape(link.target) + "\"" + size + ">";
            }
            return "<span class=\"embed missing\">" + htmlEscape(rawInner) + "</span>";
        }
        std::string display = trim(link.alias);
        if (display.empty()) display = link.target.empty() ? link.heading : (link.heading.empty() ? link.target : link.target + " > " + link.heading);
        std::optional<std::string> href;
        if (hooks_.link) href = hooks_.link(link);
        if (!href) {
            if (!hooks_.link) {
                std::string h = link.target.empty() ? "" : urlEncodePath(link.target) + ".html";
                if (!link.heading.empty()) h += "#" + (link.heading[0] == '^' ? link.heading : slugify(link.heading));
                href = h;
            } else {
                return "<a class=\"internal-link unresolved\" title=\"no note called " + htmlEscape(link.target) + " yet\">" + htmlEscape(display) + "</a>";
            }
        }
        return "<a class=\"internal-link\" href=\"" + htmlEscape(*href) + "\">" + htmlEscape(display) + "</a>";
    }

    std::string heading(int level, const std::string& text) {
        std::string id = slugify(text);
        if (id.empty()) id = "section";
        int& count = ids_[id];
        std::string unique = count ? id + "-" + std::to_string(count) : id;
        ++count;
        std::string tag = "h" + std::to_string(level);
        return "<" + tag + " id=\"" + htmlEscape(unique) + "\">" + inlines(stripBlockIds(text)) + "</" + tag + ">\n";
    }

    std::string quote(const std::vector<std::string>& inner) {
        if (!inner.empty()) {
            const std::string first = trim(inner[0]);
            if (startsWith(first, "[!")) {
                auto close = first.find(']');
                if (close != std::string::npos) {
                    std::string type = lower(first.substr(2, close - 2));
                    std::string rest = first.substr(close + 1);
                    std::string fold;
                    if (!rest.empty() && (rest[0] == '+' || rest[0] == '-')) {
                        fold = rest.substr(0, 1);
                        rest = rest.substr(1);
                    }
                    std::string title = trim(rest);
                    if (title.empty()) {
                        title = type;
                        if (!title.empty()) title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
                    }
                    std::vector<std::string> body(inner.begin() + 1, inner.end());
                    std::string content = blocks(body, false);
                    std::string cls = "callout callout-" + htmlEscape(slugify(type));
                    if (!fold.empty()) {
                        return "<details class=\"" + cls + "\"" + (fold == "+" ? " open" : "") + "><summary class=\"callout-title\">" +
                               inlines(title) + "</summary><div class=\"callout-content\">\n" + content + "</div></details>\n";
                    }
                    return "<div class=\"" + cls + "\"><div class=\"callout-title\">" + inlines(title) +
                           "</div><div class=\"callout-content\">\n" + content + "</div></div>\n";
                }
            }
        }
        return "<blockquote>\n" + blocks(inner, false) + "</blockquote>\n";
    }

    std::string list(const std::vector<std::string>& lines, std::size_t& i) {
        auto first = *listMarker(lines[i]);
        std::vector<std::vector<std::string>> items;
        std::vector<int> numbers;
        bool loose = false;
        std::size_t content = first.content;
        const std::size_t n = lines.size();
        while (i < n) {
            const std::string& l = lines[i];
            auto m = listMarker(l);
            if (m && m->indent < content && !hrLine(l)) {
                if (m->ordered != first.ordered || m->indent > first.indent + 3) break;
                if (!items.empty() && m->indent < first.indent) break;
                items.push_back({l.size() > m->content ? l.substr(m->content) : ""});
                numbers.push_back(m->start);
                content = m->content;
                ++i;
                continue;
            }
            if (items.empty()) break;
            if (blank(l)) {
                std::size_t j = i + 1;
                while (j < n && blank(lines[j])) ++j;
                if (j >= n) break;
                auto nm = listMarker(lines[j]);
                bool sibling = nm && nm->indent < content && nm->ordered == first.ordered && nm->indent <= first.indent + 3;
                if (indentOf(lines[j]) >= content || sibling) {
                    if (sibling) loose = true;
                    for (std::size_t k = i; k < j; ++k) items.back().push_back("");
                    i = j;
                    continue;
                }
                break;
            }
            if (indentOf(l) >= content) {
                items.back().push_back(dedent(l, content));
                ++i;
                continue;
            }
            // lazy continuation of the item's paragraph
            int lv;
            std::string ht;
            char fc;
            std::size_t fl;
            std::string fi;
            if (!items.back().empty() && !blank(items.back().back()) && !m && !atx(l, lv, ht) && !hrLine(l) &&
                !fenceLine(l, fc, fl, fi) && l[indentOf(l)] != '>') {
                items.back().push_back(trim(l));
                ++i;
                continue;
            }
            break;
        }
        std::string tag = first.ordered ? "ol" : "ul";
        bool tasks = false;
        std::string body;
        for (auto& item : items) {
            std::string cls, box;
            if (!item.empty()) {
                const std::string& f = item[0];
                if (f.size() >= 3 && f[0] == '[' && f[2] == ']' && (f.size() == 3 || f[3] == ' ')) {
                    char mark = f[1];
                    tasks = true;
                    cls = mark == ' ' ? " class=\"task\"" : " class=\"task done\" data-task=\"" + htmlEscape(std::string(1, mark)) + "\"";
                    box = std::string("<input type=\"checkbox\" disabled") + (mark != ' ' ? " checked" : "") + "> ";
                    item[0] = f.size() > 4 ? f.substr(4) : "";
                }
            }
            std::string inner = blocks(item, !loose);
            while (!inner.empty() && inner.back() == '\n') inner.pop_back();
            body += "<li" + cls + ">" + box + inner + "</li>\n";
        }
        std::string open = "<" + tag;
        if (tasks) open += " class=\"contains-task-list\"";
        if (first.ordered && first.start != 1) open += " start=\"" + std::to_string(first.start) + "\"";
        return open + ">\n" + body + "</" + tag + ">\n";
    }
};

// Removes %% comments %% outside code fences.
std::string stripComments(const std::string& md) {
    if (md.find("%%") == std::string::npos) return md;
    std::string out;
    bool inFence = false, inComment = false;
    char fch = 0;
    std::size_t flen = 0;
    std::size_t start = 0;
    while (start <= md.size()) {
        auto nl = md.find('\n', start);
        std::string line = md.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        char c;
        std::size_t len;
        std::string info;
        if (!inComment && fenceLine(line, c, len, info)) {
            if (!inFence) {
                inFence = true;
                fch = c;
                flen = len;
            } else if (c == fch && len >= flen && info.empty()) {
                inFence = false;
            }
            out += line;
        } else if (inFence) {
            out += line;
        } else {
            std::string kept;
            std::size_t i = 0;
            while (i < line.size()) {
                auto p = line.find("%%", i);
                if (p == std::string::npos) {
                    if (!inComment) kept += line.substr(i);
                    break;
                }
                if (!inComment) kept += line.substr(i, p - i);
                inComment = !inComment;
                i = p + 2;
            }
            out += kept;
        }
        if (nl == std::string::npos) break;
        out += "\n";
        start = nl + 1;
    }
    return out;
}

}  // namespace

std::string markdownToHtml(const std::string& markdown, const RenderHooks& hooks) {
    std::string md = stripComments(markdown);
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= md.size()) {
        auto nl = md.find('\n', start);
        std::string l = md.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        // leading tabs count as 4 spaces
        std::string expanded;
        std::size_t k = 0;
        for (; k < l.size() && (l[k] == '\t' || l[k] == ' '); ++k) expanded += l[k] == '\t' ? "    " : " ";
        lines.push_back(expanded + l.substr(k));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    Renderer r(hooks);
    return r.blocks(lines, false);
}

std::string pageCss() {
    return R"(:root{--bg:#fdfdfc;--fg:#1d1d1f;--muted:#6b6b70;--line:#e3e3e6;--accent:#4f5bd5;--code:#f3f3f5;--mark:#fff3a3;--callout:#f2f4ff}
@media (prefers-color-scheme:dark){:root{--bg:#18181b;--fg:#e8e8ea;--muted:#a0a0a8;--line:#33333a;--accent:#9aa3ff;--code:#26262c;--mark:#6b5d00;--callout:#22243a}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.6 system-ui,-apple-system,"Segoe UI",sans-serif}
.site-nav{display:flex;gap:16px;flex-wrap:wrap;padding:12px 16px;border-bottom:1px solid var(--line);font-size:14px}
.site-nav a{color:var(--muted);text-decoration:none}.site-nav a:hover{color:var(--accent)}
main{max-width:760px;margin:0 auto;padding:24px 16px 64px}
h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.6em 0 .5em}h1{font-size:2em;margin-top:.4em}
a{color:var(--accent)}a.internal-link{text-decoration:none;border-bottom:1px solid currentColor}
a.unresolved{color:var(--muted);border-bottom:1px dashed var(--muted)}
.tag{display:inline-block;padding:0 8px;border-radius:10px;background:var(--callout);color:var(--accent);text-decoration:none;font-size:.9em}
code{background:var(--code);padding:.1em .35em;border-radius:4px;font:.9em ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
pre{background:var(--code);padding:12px 14px;border-radius:8px;overflow-x:auto}pre code{background:none;padding:0}
blockquote{margin:1em 0;padding:.2em 1em;border-left:3px solid var(--line);color:var(--muted)}
mark{background:var(--mark);color:inherit}
table{border-collapse:collapse;display:block;overflow-x:auto;margin:1em 0}th,td{border:1px solid var(--line);padding:6px 10px}th{background:var(--code)}
img,video{max-width:100%;height:auto}
ul.contains-task-list{list-style:none;padding-left:1.2em}li.task.done{color:var(--muted)}li.task input{margin:0 .4em 0 -1.2em}
.callout{border-left:4px solid var(--accent);background:var(--callout);border-radius:6px;padding:8px 14px;margin:1em 0}
.callout-title{font-weight:600}.callout-warning,.callout-caution,.callout-attention{border-color:#e0a100}.callout-danger,.callout-error,.callout-bug,.callout-failure{border-color:#e5484d}.callout-tip,.callout-success,.callout-check,.callout-done{border-color:#2f9e5b}
.embed{border-left:3px solid var(--accent);padding:2px 0 2px 14px;margin:1em 0}.embed-title{font-size:.85em;color:var(--muted)}
.embed.missing{color:var(--muted);font-style:italic}
.properties{font-size:.9em;color:var(--muted);border-bottom:1px solid var(--line);margin-bottom:1em;padding-bottom:.5em}.properties td,.properties th{border:none;padding:2px 12px 2px 0;background:none;text-align:left}
.backlinks{margin-top:3em;padding-top:1em;border-top:1px solid var(--line);font-size:.95em}.backlinks h2{font-size:1em;color:var(--muted);margin:0 0 .5em}
.note-list{columns:2 240px}.note-list li{break-inside:avoid}.folder{color:var(--muted);font-size:.9em}
.math{font-family:ui-serif,Georgia,serif}
@media print{.site-nav,.backlinks{display:none}main{max-width:none}}
)";
}

std::string htmlPage(const std::string& title, const std::string& body, const std::string& cssHref, const std::string& nav) {
    std::string css = cssHref.empty() ? "<style>\n" + pageCss() + "</style>" : "<link rel=\"stylesheet\" href=\"" + htmlEscape(cssHref) + "\">";
    return "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" +
           htmlEscape(title) + "</title>\n" + css + "\n</head>\n<body>\n" + (nav.empty() ? "" : "<nav class=\"site-nav\">" + nav + "</nav>\n") +
           "<main>\n" + body + "</main>\n</body>\n</html>\n";
}

}  // namespace wetwaresuite::notes
