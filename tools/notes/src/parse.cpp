// Reading one note: links, tags, headings and tasks, skipping code and comments.
#include <cctype>

#include "wetwaresuite/notes/notes.hpp"

namespace wetwaresuite::notes {

namespace {

struct Line {
    std::size_t start = 0;  // byte offset in the file
    std::string text;       // without the line break
};

std::vector<Line> lineSpans(const std::string& text) {
    std::vector<Line> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        Line l;
        l.start = pos;
        l.text = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!l.text.empty() && l.text.back() == '\r') l.text.pop_back();
        if (nl == std::string::npos) {
            if (pos < text.size()) out.push_back(std::move(l));
            break;
        }
        out.push_back(std::move(l));
        pos = nl + 1;
    }
    return out;
}

// A fence opener/closer: up to 3 spaces, then ``` or ~~~ (3 or more).
bool fence(const std::string& l, char& ch, std::size_t& len, std::string* info = nullptr) {
    std::size_t i = 0;
    while (i < l.size() && i < 3 && l[i] == ' ') ++i;
    if (i >= l.size() || (l[i] != '`' && l[i] != '~')) return false;
    char c = l[i];
    std::size_t n = 0;
    while (i + n < l.size() && l[i + n] == c) ++n;
    if (n < 3) return false;
    std::string rest = l.substr(i + n);
    if (c == '`' && rest.find('`') != std::string::npos) return false;
    ch = c;
    len = n;
    if (info) *info = trim(rest);
    return true;
}

bool tagHasLetter(const std::string& t) {
    for (unsigned char c : t) {
        if (!std::isdigit(c) && c != '/' && c != '-' && c != '_') return true;
    }
    return false;
}

// Marks inline code spans and %% comments %% (which may continue from earlier lines).
std::vector<char> lineMask(const std::string& l, bool& inComment) {
    std::vector<char> mask(l.size(), 0);
    std::size_t i = 0;
    while (i < l.size()) {
        if (inComment) {
            auto end = l.find("%%", i);
            std::size_t stop = end == std::string::npos ? l.size() : end + 2;
            for (std::size_t k = i; k < stop; ++k) mask[k] = 1;
            if (end == std::string::npos) return mask;
            inComment = false;
            i = stop;
            continue;
        }
        if (l[i] == '`') {
            std::size_t n = 0;
            while (i + n < l.size() && l[i + n] == '`') ++n;
            std::size_t search = i + n;
            std::size_t close = std::string::npos;
            while (search < l.size()) {
                auto f = l.find(std::string(n, '`'), search);
                if (f == std::string::npos) break;
                std::size_t m = 0;
                while (f + m < l.size() && l[f + m] == '`') ++m;
                if (m == n) {
                    close = f;
                    break;
                }
                search = f + m;
            }
            if (close == std::string::npos) {
                i += n;
                continue;
            }
            for (std::size_t k = i; k < close + n; ++k) mask[k] = 1;
            i = close + n;
            continue;
        }
        if (l.compare(i, 2, "%%") == 0) {
            inComment = true;
            mask[i] = mask[i + 1] = 1;
            i += 2;
            continue;
        }
        ++i;
    }
    return mask;
}

bool anyMasked(const std::vector<char>& mask, std::size_t a, std::size_t b) {
    for (std::size_t k = a; k < b && k < mask.size(); ++k) {
        if (mask[k]) return true;
    }
    return false;
}

std::string findDue(const std::string& t) {
    auto dateAt = [&](std::size_t p) -> std::string {
        while (p < t.size() && t[p] == ' ') ++p;
        if (p + 10 > t.size()) return {};
        std::string d = t.substr(p, 10);
        for (int k : {0, 1, 2, 3, 5, 6, 8, 9}) {
            if (!std::isdigit(static_cast<unsigned char>(d[static_cast<std::size_t>(k)]))) return {};
        }
        if (d[4] != '-' || d[7] != '-') return {};
        return d;
    };
    for (const char* marker : {"\xF0\x9F\x93\x85", "due::", "due:", "@due(", "\xF0\x9F\x97\x93"}) {
        for (auto p = t.find(marker); p != std::string::npos; p = t.find(marker, p + 1)) {
            std::string d = dateAt(p + std::char_traits<char>::length(marker));
            if (!d.empty()) return d;
        }
    }
    return {};
}

void scanLinks(const std::string& l, const std::vector<char>& mask, std::size_t lineStart, int lineNo, std::vector<Link>& out) {
    std::size_t i = 0;
    while (i < l.size()) {
        if (mask.size() > i && mask[i]) {
            ++i;
            continue;
        }
        bool bang = l[i] == '!' && i + 1 < l.size() && l[i + 1] == '[';
        std::size_t b = bang ? i + 1 : i;
        if (l[b] != '[') {
            ++i;
            continue;
        }
        if (b + 1 < l.size() && l[b + 1] == '[') {
            auto close = l.find("]]", b + 2);
            if (close == std::string::npos) {
                i = b + 2;
                continue;
            }
            std::string inner = l.substr(b + 2, close - b - 2);
            if (inner.find('[') != std::string::npos || anyMasked(mask, i, close + 2) || trim(inner).empty()) {
                i = b + 2;
                continue;
            }
            Link link;
            link.embed = bang;
            link.wiki = true;
            link.line = lineNo;
            link.begin = lineStart + i;
            link.end = lineStart + close + 2;
            std::size_t pipe = inner.find('|');
            if (pipe != std::string::npos) {
                std::size_t cut = pipe;
                if (pipe > 0 && inner[pipe - 1] == '\\') {
                    link.escapedPipe = true;
                    cut = pipe - 1;
                }
                link.alias = inner.substr(pipe + 1);
                inner = inner.substr(0, cut);
            }
            std::size_t hash = inner.find('#');
            if (hash != std::string::npos) {
                link.heading = trim(inner.substr(hash + 1));
                inner = inner.substr(0, hash);
            }
            link.target = trim(inner);
            out.push_back(std::move(link));
            i = close + 2;
            continue;
        }
        // markdown link [text](dest)
        int depth = 0;
        std::size_t j = b;
        std::size_t closeBracket = std::string::npos;
        for (; j < l.size(); ++j) {
            if (l[j] == '\\') {
                ++j;
                continue;
            }
            if (l[j] == '[') ++depth;
            if (l[j] == ']' && --depth == 0) {
                closeBracket = j;
                break;
            }
        }
        if (closeBracket == std::string::npos || closeBracket + 1 >= l.size() || l[closeBracket + 1] != '(') {
            i = b + 1;
            continue;
        }
        std::size_t k = closeBracket + 2;
        int pd = 1;
        std::size_t closeParen = std::string::npos;
        bool angle = k < l.size() && l[k] == '<';
        for (; k < l.size(); ++k) {
            if (angle) {
                if (l[k] == '>') angle = false;
                continue;
            }
            if (l[k] == '(') ++pd;
            if (l[k] == ')' && --pd == 0) {
                closeParen = k;
                break;
            }
        }
        if (closeParen == std::string::npos || anyMasked(mask, i, closeParen + 1)) {
            i = closeBracket + 1;
            continue;
        }
        std::string dest = trim(l.substr(closeBracket + 2, closeParen - closeBracket - 2));
        if (!dest.empty() && dest[0] == '<') {
            auto gt = dest.find('>');
            dest = dest.substr(1, gt == std::string::npos ? std::string::npos : gt - 1);
        } else if (auto sp = dest.find(' '); sp != std::string::npos) {
            dest = dest.substr(0, sp);
        }
        const std::string ld = lower(dest);
        bool external = dest.empty() || dest[0] == '#' || ld.find("://") != std::string::npos || startsWith(ld, "mailto:") ||
                        startsWith(ld, "data:") || startsWith(ld, "obsidian:") || startsWith(ld, "tel:");
        if (!external) {
            Link link;
            link.embed = bang;
            link.wiki = false;
            link.line = lineNo;
            link.begin = lineStart + i;
            link.end = lineStart + closeParen + 1;
            link.alias = l.substr(b + 1, closeBracket - b - 1);
            std::string target = dest;
            if (auto hash = target.find('#'); hash != std::string::npos) {
                link.heading = urlDecode(target.substr(hash + 1));
                target = target.substr(0, hash);
            }
            link.target = urlDecode(target);
            if (!link.target.empty()) out.push_back(std::move(link));
        }
        i = closeParen + 1;
    }
}

void scanTags(const std::string& l, const std::vector<char>& mask, std::size_t lineStart, int lineNo, std::vector<TagSpan>& out) {
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (l[i] != '#' || (mask.size() > i && mask[i])) continue;
        if (i > 0 && !std::isspace(static_cast<unsigned char>(l[i - 1])) && l[i - 1] != '(' && l[i - 1] != ',') continue;
        std::size_t j = i + 1;
        while (j < l.size() && isTagChar(static_cast<unsigned char>(l[j]))) ++j;
        std::string tag = l.substr(i + 1, j - i - 1);
        while (!tag.empty() && tag.back() == '/') tag.pop_back();
        if (tag.empty() || !tagHasLetter(tag)) continue;
        // non-ASCII punctuation like "，" isn't worth handling; the common case is plain text
        out.push_back({tag, lineNo, lineStart + i, lineStart + i + 1 + tag.size()});
        i = j - 1;
    }
}

bool atxHeading(const std::string& l, int& level, std::string& text) {
    std::size_t i = 0;
    while (i < l.size() && i < 3 && l[i] == ' ') ++i;
    std::size_t n = 0;
    while (i + n < l.size() && l[i + n] == '#') ++n;
    if (n == 0 || n > 6) return false;
    if (i + n < l.size() && l[i + n] != ' ' && l[i + n] != '\t') return false;
    std::string t = trim(l.substr(i + n));
    // closing #s
    std::size_t e = t.size();
    while (e > 0 && t[e - 1] == '#') --e;
    if (e < t.size() && (e == 0 || t[e - 1] == ' ')) t = trim(t.substr(0, e));
    level = static_cast<int>(n);
    text = t;
    return true;
}

}  // namespace

bool parseTaskLine(const std::string& l, Task& task) {
    std::size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t' || l[i] == '>')) ++i;
    std::size_t indent = i;
    std::size_t m = i;
    if (m < l.size() && (l[m] == '-' || l[m] == '*' || l[m] == '+')) {
        ++m;
    } else {
        while (m < l.size() && std::isdigit(static_cast<unsigned char>(l[m]))) ++m;
        if (m == i || m >= l.size() || (l[m] != '.' && l[m] != ')')) return false;
        ++m;
    }
    if (m + 4 > l.size() || l[m] != ' ' || l[m + 1] != '[' || l[m + 3] != ']') return false;
    if (m + 4 < l.size() && l[m + 4] != ' ') return false;
    char mark = l[m + 2];
    task.mark = mark;
    task.done = mark != ' ';
    task.text = m + 5 <= l.size() ? trim(l.substr(m + 4)) : "";
    task.indent = static_cast<int>(indent);
    task.due = findDue(task.text);
    return true;
}

Parsed parseNote(const std::string& text) {
    Parsed p;
    std::string yaml;
    std::size_t bodyStart = 0;
    int bodyLine = 1;
    if (splitFrontmatter(text, &yaml, &bodyStart, &bodyLine)) {
        p.hasFrontmatter = true;
        p.frontmatter = parseFrontmatter(yaml);
        p.bodyLine = bodyLine;
    }
    auto lines = lineSpans(text);
    bool inFence = false;
    char fenceChar = 0;
    std::size_t fenceLen = 0;
    bool inComment = false;
    for (std::size_t n = 0; n < lines.size(); ++n) {
        const int lineNo = static_cast<int>(n) + 1;
        const std::string& l = lines[n].text;
        if (lineNo < p.bodyLine) {
            if (lineNo > 1 && lineNo < p.bodyLine - 1) {
                std::vector<char> none(l.size(), 0);
                scanLinks(l, none, lines[n].start, lineNo, p.links);
            }
            continue;
        }
        for (std::size_t k = 0; k < l.size();) {
            while (k < l.size() && std::isspace(static_cast<unsigned char>(l[k]))) ++k;
            if (k >= l.size()) break;
            ++p.words;
            while (k < l.size() && !std::isspace(static_cast<unsigned char>(l[k]))) ++k;
        }
        char c;
        std::size_t len;
        if (inFence) {
            if (fence(l, c, len) && c == fenceChar && len >= fenceLen && trim(l).find_first_not_of(c) == std::string::npos) inFence = false;
            continue;
        }
        if (!inComment && fence(l, c, len)) {
            inFence = true;
            fenceChar = c;
            fenceLen = len;
            continue;
        }
        bool commentBefore = inComment;
        auto mask = lineMask(l, inComment);
        int level;
        std::string htext;
        if (!commentBefore && atxHeading(l, level, htext)) p.headings.push_back({level, htext, lineNo});
        Task task;
        if (!commentBefore && parseTaskLine(l, task)) {
            task.line = lineNo;
            p.tasks.push_back(task);
        }
        scanLinks(l, mask, lines[n].start, lineNo, p.links);
        scanTags(l, mask, lines[n].start, lineNo, p.inlineTags);
    }
    return p;
}

std::optional<std::pair<std::size_t, std::size_t>> sectionRange(const std::string& text, const std::string& heading) {
    Parsed p = parseNote(text);
    std::string want = lower(trim(heading));
    while (!want.empty() && want[0] == '#') want = trim(want.substr(1));
    auto lines = lineSpans(text);
    for (std::size_t h = 0; h < p.headings.size(); ++h) {
        if (lower(p.headings[h].text) != want && slugify(p.headings[h].text) != slugify(want)) continue;
        std::size_t begin = lines[static_cast<std::size_t>(p.headings[h].line - 1)].start;
        std::size_t end = text.size();
        for (std::size_t k = h + 1; k < p.headings.size(); ++k) {
            if (p.headings[k].level <= p.headings[h].level) {
                end = lines[static_cast<std::size_t>(p.headings[k].line - 1)].start;
                break;
            }
        }
        return std::make_pair(begin, end);
    }
    return std::nullopt;
}

}  // namespace wetwaresuite::notes
