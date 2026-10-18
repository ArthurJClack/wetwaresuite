// A small table-driven highlighter: one pass over the text per language mode
// (code, markup, Markdown, config files). It aims to be fast and good enough to
// read by, not a full parser.
#include <algorithm>
#include <cctype>

#include "wetwaresuite/code/code.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::code {

namespace {

bool isIdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool isIdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

class Highlighter {
public:
    Highlighter(const std::string& s, const Language& l) : s_(s), L_(l) {
        starts_.push_back(0);
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') starts_.push_back(i + 1);
        }
    }

    std::vector<Span> run() {
        switch (L_.mode) {
            case Mode::Code: code(); break;
            case Mode::Markup: markup(); break;
            case Mode::Markdown: markdown(); break;
            case Mode::Config: config(); break;
            case Mode::Plain: break;
        }
        return std::move(out_);
    }

private:
    const std::string& s_;
    const Language& L_;
    std::vector<std::size_t> starts_;
    std::vector<Span> out_;

    std::size_t n() const { return s_.size(); }
    char at(std::size_t i) const { return i < s_.size() ? s_[i] : '\0'; }
    bool startsWith(std::size_t i, const std::string& w) const { return !w.empty() && s_.compare(i, w.size(), w) == 0; }
    std::size_t lineEnd(std::size_t i) const {
        std::size_t e = s_.find('\n', i);
        return e == std::string::npos ? n() : e;
    }
    std::size_t lineStart(std::size_t i) const {
        auto it = std::upper_bound(starts_.begin(), starts_.end(), i);
        return *(it - 1);
    }
    bool blankBefore(std::size_t i) const {
        for (std::size_t k = lineStart(i); k < i; ++k) {
            if (s_[k] != ' ' && s_[k] != '\t') return false;
        }
        return true;
    }
    std::size_t nextNonSpace(std::size_t i) const {
        while (i < n() && (s_[i] == ' ' || s_[i] == '\t')) ++i;
        return i;
    }

    // one span per line the range covers
    void emit(std::size_t a, std::size_t b, Token k) {
        if (b <= a || k == Token::Plain) return;
        while (a < b) {
            auto it = std::upper_bound(starts_.begin(), starts_.end(), a);
            const std::size_t line = static_cast<std::size_t>(it - starts_.begin()) - 1;
            const std::size_t ls = starts_[line];
            std::size_t le = lineEnd(a);
            std::size_t end = std::min(b, le);
            if (end > a) {
                std::size_t len = end - a;
                if (end == le && end > ls && s_[end - 1] == '\r') --len;  // CRLF files
                if (len) out_.push_back({static_cast<int>(line), static_cast<int>(a - ls), static_cast<int>(len), k});
            }
            a = end + 1;
        }
    }

    bool inSet(const std::set<std::string>& set, const std::string& w) const {
        if (set.count(w)) return true;
        if (!L_.caseInsensitive) return false;
        std::string lo = sysutil::lowerAscii(w), up = w;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return set.count(lo) || set.count(up);
    }

    std::size_t stringEnd(std::size_t i, char q) const {
        std::size_t j = i + 1;
        const bool multi = L_.multilineStrings || q == '`';
        while (j < n()) {
            char c = s_[j];
            if (c == '\\' && L_.id != "batch" && L_.id != "powershell") {
                j += 2;
                continue;
            }
            if (c == q) return j + 1;
            if (c == '\n' && !multi) return j;
            ++j;
        }
        return n();
    }

    bool comment(std::size_t& i) {
        for (const auto& [open, close] : L_.blockComments) {
            if (!startsWith(i, open)) continue;
            if (std::isalpha(static_cast<unsigned char>(open[0])) && !blankBefore(i)) continue;  // Ruby =begin
            std::size_t e = s_.find(close, i + open.size());
            e = e == std::string::npos ? n() : e + close.size();
            emit(i, e, Token::Comment);
            i = e;
            return true;
        }
        for (const auto& lc : L_.lineComments) {
            if (!startsWith(i, lc)) continue;
            if (std::isalpha(static_cast<unsigned char>(lc[0]))) {  // REM in batch files: a word at the line start
                if (!blankBefore(i) || isIdentChar(static_cast<unsigned char>(at(i + lc.size())))) continue;
            }
            std::size_t e = lineEnd(i);
            emit(i, e, Token::Comment);
            i = e;
            return true;
        }
        return false;
    }

    std::size_t numberEnd(std::size_t i) const {
        std::size_t j = i;
        while (j < n()) {
            char c = s_[j];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\'') {
                if (c == '.' && !std::isdigit(static_cast<unsigned char>(at(j + 1)))) break;  // 1..2, x.method
                if (c == '\'' && !std::isxdigit(static_cast<unsigned char>(at(j + 1)))) break;
                ++j;
            } else if ((c == '+' || c == '-') && (s_[j - 1] == 'e' || s_[j - 1] == 'E') && !(at(i + 1) == 'x' || at(i + 1) == 'X')) {
                ++j;
            } else {
                break;
            }
        }
        return j;
    }

    void code() {
        std::size_t i = 0;
        while (i < n()) {
            const char c = s_[i];
            const unsigned char uc = static_cast<unsigned char>(c);
            if (c == '\n' || c == ' ' || c == '\t' || c == '\r') {
                ++i;
                continue;
            }
            if (L_.preprocessor && c == '#' && blankBefore(i)) {
                std::size_t e = lineEnd(i);
                std::size_t cm = s_.find("//", i);
                if (cm != std::string::npos && cm < e) e = cm;
                emit(i, e, Token::Preproc);
                i = e;
                continue;
            }
            if (comment(i)) continue;
            if (L_.tripleQuotes && (startsWith(i, "\"\"\"") || startsWith(i, "'''"))) {
                std::size_t e = s_.find(s_.substr(i, 3), i + 3);
                e = e == std::string::npos ? n() : e + 3;
                emit(i, e, Token::String);
                i = e;
                continue;
            }
            if (L_.quotes.find(c) != std::string::npos) {
                if (L_.id == "rust" && c == '\'' && !(at(i + 2) == '\'' || (at(i + 1) == '\\' && s_.find('\'', i + 2) < lineEnd(i)))) {
                    ++i;  // a lifetime ('a), not a char
                    continue;
                }
                std::size_t e = stringEnd(i, c);
                Token kind = Token::String;
                if (L_.colonKeys && at(nextNonSpace(e)) == ':') kind = Token::Attribute;  // JSON keys
                emit(i, e, kind);
                i = e;
                continue;
            }
            if (std::isdigit(uc) || (c == '.' && std::isdigit(static_cast<unsigned char>(at(i + 1))))) {
                if (i > 0 && isIdentChar(static_cast<unsigned char>(s_[i - 1]))) {
                    ++i;
                    continue;
                }
                std::size_t e = numberEnd(i);
                emit(i, e, Token::Number);
                i = e;
                continue;
            }
            if (L_.colonKeys && c == '#' && L_.id == "css" && std::isxdigit(static_cast<unsigned char>(at(i + 1)))) {
                std::size_t e = i + 1;
                while (e < n() && isIdentChar(static_cast<unsigned char>(s_[e]))) ++e;
                emit(i, e, Token::Number);  // #ff8800
                i = e;
                continue;
            }
            if (!L_.variablePrefix.empty() && c == L_.variablePrefix[0] && i + 1 < n()) {
                std::size_t e = i + 1;
                if (at(e) == '{' || at(e) == '(') {
                    const char close = at(e) == '{' ? '}' : ')';
                    std::size_t f = s_.find(close, e);
                    e = f == std::string::npos || f > lineEnd(i) ? lineEnd(i) : f + 1;
                } else if (c == '%') {
                    std::size_t f = s_.find('%', e);
                    e = f == std::string::npos || f > lineEnd(i) ? e : f + 1;  // %PATH%
                } else {
                    while (e < n() && (isIdentChar(static_cast<unsigned char>(s_[e])) || (e == i + 1 && std::string("?@#*!$0123456789").find(s_[e]) != std::string::npos))) ++e;
                }
                if (e > i + 1) {
                    emit(i, e, Token::Variable);
                    i = e;
                    continue;
                }
            }
            if (isIdentStart(uc) || ((c == '@' || c == '$') && isIdentStart(static_cast<unsigned char>(at(i + 1))))) {
                word(i);
                continue;
            }
            ++i;
        }
    }

    void word(std::size_t& i) {
        std::size_t e = i + 1;
        while (e < n() && (isIdentChar(static_cast<unsigned char>(s_[e])) || (L_.id == "css" && s_[e] == '-'))) ++e;
        if (L_.id == "rust" && at(e) == '!') ++e;  // println!
        if (L_.id == "ruby" && (at(e) == '?' || at(e) == '!')) ++e;
        const std::string w = s_.substr(i, e - i);
        // string prefixes: r"raw", f"{x}", b'bytes'
        if (L_.quotes.find(at(e)) != std::string::npos && at(e) != '\0' && w.size() <= 2 &&
            (L_.id == "python" || L_.id == "rust" || L_.id == "csharp" || L_.id == "cpp")) {
            static const std::set<std::string> prefixes = {"r", "b", "f", "u", "rb", "br", "fr", "rf", "R", "B", "F", "U", "u8", "L"};
            if (prefixes.count(w) || (L_.id == "csharp" && w == "@")) {
                std::size_t se = L_.tripleQuotes && s_.compare(e, 3, std::string(3, at(e))) == 0 ? s_.find(std::string(3, at(e)), e + 3) : stringEnd(e, at(e));
                if (se == std::string::npos) se = n();
                else if (L_.tripleQuotes && s_.compare(e, 3, std::string(3, at(e))) == 0) se += 3;
                emit(i, se, Token::String);
                i = se;
                return;
            }
        }
        Token k = Token::Plain;
        if (w[0] == '@') k = Token::Preproc;  // decorators and annotations
        else if (inSet(L_.keywords, w)) k = Token::Keyword;
        else if (inSet(L_.types, w)) k = Token::Type;
        else if (inSet(L_.constants, w)) k = Token::Constant;
        else if (inSet(L_.builtins, w)) k = Token::Function;
        else {
            std::size_t next = nextNonSpace(e);
            if (L_.colonKeys && at(next) == ':' && at(next + 1) != ':') k = Token::Attribute;
            else if (at(next) == '(') k = Token::Function;
            else if (L_.capitalTypes && std::isupper(static_cast<unsigned char>(w[0])) && w.size() > 1) k = Token::Type;
        }
        emit(i, e, k);
        i = e;
    }

    void markup() {
        std::size_t i = 0;
        while (i < n()) {
            if (startsWith(i, "<!--")) {
                std::size_t e = s_.find("-->", i + 4);
                e = e == std::string::npos ? n() : e + 3;
                emit(i, e, Token::Comment);
                i = e;
                continue;
            }
            if (startsWith(i, "<![CDATA[")) {
                std::size_t e = s_.find("]]>", i);
                e = e == std::string::npos ? n() : e + 3;
                emit(i, e, Token::String);
                i = e;
                continue;
            }
            const char c = s_[i];
            if (c == '<' && (std::isalpha(static_cast<unsigned char>(at(i + 1))) || at(i + 1) == '/' || at(i + 1) == '!' || at(i + 1) == '?')) {
                std::size_t j = i + 1;
                if (at(j) == '/' || at(j) == '!' || at(j) == '?') ++j;
                std::size_t nameStart = j;
                while (j < n() && (isIdentChar(static_cast<unsigned char>(s_[j])) || s_[j] == '-' || s_[j] == ':' || s_[j] == '.')) ++j;
                emit(nameStart, j, Token::Tag);
                // attributes until the closing >
                while (j < n() && s_[j] != '>') {
                    char d = s_[j];
                    if (d == '"' || d == '\'') {
                        std::size_t e = s_.find(d, j + 1);
                        e = e == std::string::npos ? n() : e + 1;
                        emit(j, e, Token::String);
                        j = e;
                    } else if (isIdentStart(static_cast<unsigned char>(d))) {
                        std::size_t e = j;
                        while (e < n() && (isIdentChar(static_cast<unsigned char>(s_[e])) || s_[e] == '-' || s_[e] == ':' || s_[e] == '@' || s_[e] == '.')) ++e;
                        emit(j, e, Token::Attribute);
                        j = e;
                    } else {
                        ++j;
                    }
                }
                i = j + 1;
                continue;
            }
            if (c == '&') {
                std::size_t e = s_.find(';', i);
                if (e != std::string::npos && e - i < 12) {
                    emit(i, e + 1, Token::Constant);
                    i = e + 1;
                    continue;
                }
            }
            ++i;
        }
    }

    void inlineMarkdown(std::size_t a, std::size_t b) {
        std::size_t i = a;
        while (i < b) {
            char c = s_[i];
            if (c == '`') {
                std::size_t e = s_.find('`', i + 1);
                if (e != std::string::npos && e < b) {
                    emit(i, e + 1, Token::String);
                    i = e + 1;
                    continue;
                }
            }
            if ((c == '*' || c == '_') && at(i + 1) == c) {
                std::size_t e = s_.find(std::string(2, c), i + 2);
                if (e != std::string::npos && e < b) {
                    emit(i, e + 2, Token::Keyword);
                    i = e + 2;
                    continue;
                }
            }
            if ((c == '*' || c == '_') && at(i + 1) != ' ' && (i == a || !isIdentChar(static_cast<unsigned char>(s_[i - 1])))) {
                std::size_t e = s_.find(c, i + 1);
                if (e != std::string::npos && e < b && e > i + 1) {
                    emit(i, e + 1, Token::Type);
                    i = e + 1;
                    continue;
                }
            }
            if (c == '[') {
                std::size_t close = s_.find(']', i);
                if (close != std::string::npos && close < b && at(close + 1) == '(') {
                    std::size_t paren = s_.find(')', close);
                    if (paren != std::string::npos && paren < b) {
                        emit(i, close + 1, Token::Link);
                        emit(close + 1, paren + 1, Token::String);
                        i = paren + 1;
                        continue;
                    }
                }
            }
            ++i;
        }
    }

    void markdown() {
        bool fence = false;
        std::string fenceMark;
        for (std::size_t l = 0; l < starts_.size(); ++l) {
            const std::size_t a = starts_[l], b = lineEnd(a);
            std::size_t t = nextNonSpace(a);
            if (startsWith(t, "```") || startsWith(t, "~~~")) {
                std::string mark = s_.substr(t, 3);
                if (!fence) {
                    fence = true;
                    fenceMark = mark;
                } else if (mark == fenceMark) {
                    fence = false;
                }
                emit(a, b, Token::String);
                continue;
            }
            if (fence) {
                emit(a, b, Token::String);
                continue;
            }
            if (at(t) == '#') {
                std::size_t h = t;
                while (at(h) == '#') ++h;
                if (h - t <= 6 && (at(h) == ' ' || h == b)) {
                    emit(a, b, Token::Heading);
                    continue;
                }
            }
            if (at(t) == '>') {
                emit(a, b, Token::Comment);
                continue;
            }
            if ((startsWith(t, "---") || startsWith(t, "***")) && b - t >= 3 && s_.find_first_not_of(at(t), t) >= b) {
                emit(a, b, Token::Comment);
                continue;
            }
            std::size_t body = t;
            if ((at(t) == '-' || at(t) == '*' || at(t) == '+') && at(t + 1) == ' ') {
                emit(t, t + 1, Token::Keyword);
                body = t + 2;
                if (startsWith(body, "[ ]") || startsWith(body, "[x]") || startsWith(body, "[X]")) {
                    emit(body, body + 3, Token::Constant);
                    body += 3;
                }
            } else if (std::isdigit(static_cast<unsigned char>(at(t)))) {
                std::size_t d = t;
                while (std::isdigit(static_cast<unsigned char>(at(d)))) ++d;
                if ((at(d) == '.' || at(d) == ')') && at(d + 1) == ' ') {
                    emit(t, d + 1, Token::Keyword);
                    body = d + 2;
                }
            }
            inlineMarkdown(body, b);
        }
    }

    void configValue(std::size_t i, std::size_t b) {
        while (i < b) {
            char c = s_[i];
            if (c == '#' && (i == 0 || s_[i - 1] == ' ' || s_[i - 1] == '\t')) {
                emit(i, b, Token::Comment);
                return;
            }
            if (c == '"' || c == '\'') {
                std::size_t e = stringEnd(i, c);
                emit(i, std::min(e, b), Token::String);
                i = e;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) || ((c == '-' || c == '+') && std::isdigit(static_cast<unsigned char>(at(i + 1))))) {
                if (i > 0 && isIdentChar(static_cast<unsigned char>(s_[i - 1]))) {
                    ++i;
                    continue;
                }
                std::size_t e = numberEnd(i + 1);
                emit(i, e, Token::Number);
                i = e;
                continue;
            }
            if (isIdentStart(static_cast<unsigned char>(c)) || c == '~') {
                std::size_t e = i + 1;
                while (e < b && isIdentChar(static_cast<unsigned char>(s_[e]))) ++e;
                if (inSet(L_.constants, s_.substr(i, e - i))) emit(i, e, Token::Constant);
                i = e;
                continue;
            }
            if ((c == '&' || c == '*') && L_.id == "yaml" && isIdentStart(static_cast<unsigned char>(at(i + 1)))) {
                std::size_t e = i + 1;
                while (e < b && isIdentChar(static_cast<unsigned char>(s_[e]))) ++e;
                emit(i, e, Token::Variable);  // anchors and aliases
                i = e;
                continue;
            }
            ++i;
        }
    }

    void config() {
        for (std::size_t l = 0; l < starts_.size(); ++l) {
            const std::size_t a = starts_[l], b = lineEnd(a);
            std::size_t t = nextNonSpace(a);
            if (t >= b) continue;
            bool isComment = false;
            for (const auto& lc : L_.lineComments) isComment = isComment || startsWith(t, lc);
            if (isComment) {
                emit(t, b, Token::Comment);
                continue;
            }
            if (at(t) == '[' && L_.id != "yaml") {
                std::size_t e = s_.find(']', t);
                e = e == std::string::npos || e > b ? b : e + 1;
                emit(t, e, Token::Heading);
                configValue(e, b);
                continue;
            }
            if (L_.id == "yaml" && (startsWith(t, "---") || startsWith(t, "..."))) {
                emit(t, b, Token::Comment);
                continue;
            }
            if (L_.id == "yaml" && at(t) == '-' && at(t + 1) == ' ') t = nextNonSpace(t + 2);
            // key: value / key = value
            const char sep = L_.id == "yaml" ? ':' : '=';
            std::size_t k = t;
            if (at(k) == '"' || at(k) == '\'') k = std::min(stringEnd(k, at(k)), b);
            while (k < b && s_[k] != sep && !(sep == ':' && s_[k] == '#') && !(sep == '=' && L_.id == "ini" && s_[k] == ':')) ++k;
            if (k < b && (sep != ':' || at(k + 1) == ' ' || k + 1 == b || at(k + 1) == '\r')) {
                std::size_t ke = k;
                while (ke > t && (s_[ke - 1] == ' ' || s_[ke - 1] == '\t')) --ke;
                emit(t, ke, Token::Attribute);
                configValue(k + 1, b);
            } else {
                configValue(t, b);
            }
        }
    }
};

const char* htmlColour(Token t) {
    switch (t) {
        case Token::Keyword: return "#c678dd";
        case Token::Type: return "#e5c07b";
        case Token::Constant: return "#d19a66";
        case Token::Number: return "#d19a66";
        case Token::String: return "#98c379";
        case Token::Comment: return "#7f848e";
        case Token::Preproc: return "#e06c75";
        case Token::Function: return "#61afef";
        case Token::Tag: return "#e06c75";
        case Token::Attribute: return "#d19a66";
        case Token::Heading: return "#61afef";
        case Token::Variable: return "#e06c75";
        case Token::Link: return "#56b6c2";
        default: return "#abb2bf";
    }
}

int ansiColour(Token t) {
    switch (t) {
        case Token::Keyword: return 35;
        case Token::Type: return 33;
        case Token::Constant:
        case Token::Number: return 33;
        case Token::String: return 32;
        case Token::Comment: return 90;
        case Token::Preproc:
        case Token::Tag:
        case Token::Variable: return 31;
        case Token::Function:
        case Token::Heading: return 34;
        case Token::Attribute: return 33;
        case Token::Link: return 36;
        default: return 0;
    }
}

template <typename Wrap>
std::string render(const std::string& text, const Language& lang, Wrap wrap, std::string (*escape)(const std::string&)) {
    auto spans = highlight(text, lang);
    auto lines = sysutil::splitLines(text);
    std::string out;
    std::size_t si = 0;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const std::string& line = lines[l];
        std::size_t col = 0;
        while (si < spans.size() && spans[si].line == static_cast<int>(l)) {
            const Span& sp = spans[si++];
            if (static_cast<std::size_t>(sp.col) < col) continue;
            out += escape(line.substr(col, static_cast<std::size_t>(sp.col) - col));
            out += wrap(sp.kind, escape(line.substr(static_cast<std::size_t>(sp.col), static_cast<std::size_t>(sp.len))));
            col = static_cast<std::size_t>(sp.col + sp.len);
        }
        if (col < line.size()) out += escape(line.substr(col));
        if (l + 1 < lines.size()) out += "\n";
    }
    return out;
}

std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else o += c;
    }
    return o;
}

std::string noEscape(const std::string& s) { return s; }

}  // namespace

const char* tokenName(Token t) {
    static const char* names[] = {"plain", "keyword", "type", "constant", "number", "string", "comment",
                                  "preproc", "function", "tag", "attribute", "heading", "variable", "link"};
    return names[static_cast<int>(t)];
}

std::vector<Span> highlight(const std::string& text, const Language& lang) { return Highlighter(text, lang).run(); }

std::string highlightHtml(const std::string& text, const Language& lang) {
    return "<pre style=\"background:#282c34;color:#abb2bf;padding:12px;font-family:monospace\">" +
           render(text, lang, [](Token t, const std::string& s) { return "<span style=\"color:" + std::string(htmlColour(t)) + "\">" + s + "</span>"; },
                  htmlEscape) +
           "</pre>\n";
}

std::string highlightAnsi(const std::string& text, const Language& lang) {
    return render(text, lang, [](Token t, const std::string& s) { return "\x1b[" + std::to_string(ansiColour(t)) + "m" + s + "\x1b[0m"; },
                  noEscape);
}

}  // namespace wetwaresuite::code
