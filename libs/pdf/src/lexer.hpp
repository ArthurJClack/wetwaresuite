// The PDF lexer, shared by the file reader, page drawing and editing (internal).
#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>

#include "wetwaresuite/pdf/reader.hpp"

namespace wetwaresuite::pdf::detail {

inline bool isWs(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
inline bool isDelim(char c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}
inline bool isRegular(char c) { return !isWs(c) && !isDelim(c); }
inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct Lexer {
    const std::string& s;
    std::size_t pos = 0;
    int depth = 0;

    Lexer(const std::string& src, std::size_t p) : s(src), pos(p) {}

    bool eof() const { return pos >= s.size(); }

    void skipWs() {
        while (pos < s.size()) {
            if (isWs(s[pos])) {
                ++pos;
            } else if (s[pos] == '%') {
                while (pos < s.size() && s[pos] != '\n' && s[pos] != '\r') ++pos;
            } else {
                break;
            }
        }
    }

    std::string regularRun() {
        std::size_t start = pos;
        while (pos < s.size() && isRegular(s[pos])) ++pos;
        return s.substr(start, pos - start);
    }

    bool peekKeyword(const char* kw) {
        skipWs();
        std::size_t n = std::char_traits<char>::length(kw);
        if (s.compare(pos, n, kw) != 0) return false;
        return pos + n >= s.size() || !isRegular(s[pos + n]);
    }

    Object name() {
        ++pos;  // '/'
        std::string out;
        while (pos < s.size() && isRegular(s[pos])) {
            if (s[pos] == '#' && pos + 2 < s.size() && hexVal(s[pos + 1]) >= 0 && hexVal(s[pos + 2]) >= 0) {
                out += static_cast<char>(hexVal(s[pos + 1]) * 16 + hexVal(s[pos + 2]));
                pos += 3;
            } else {
                out += s[pos++];
            }
        }
        return Object::makeName(out);
    }

    Object literal() {
        ++pos;  // '('
        Object o;
        o.kind = Object::String;
        int nest = 1;
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '\\') {
                if (pos >= s.size()) break;
                char e = s[pos++];
                switch (e) {
                    case 'n': o.str += '\n'; break;
                    case 'r': o.str += '\r'; break;
                    case 't': o.str += '\t'; break;
                    case 'b': o.str += '\b'; break;
                    case 'f': o.str += '\f'; break;
                    case '\r':
                        if (pos < s.size() && s[pos] == '\n') ++pos;
                        break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0';
                            for (int k = 0; k < 2 && pos < s.size() && s[pos] >= '0' && s[pos] <= '7'; ++k) v = v * 8 + (s[pos++] - '0');
                            o.str += static_cast<char>(v & 0xFF);
                        } else {
                            o.str += e;
                        }
                }
            } else if (c == '(') {
                ++nest;
                o.str += c;
            } else if (c == ')') {
                if (--nest == 0) break;
                o.str += c;
            } else {
                o.str += c;
            }
        }
        return o;
    }

    Object hexString() {
        ++pos;  // '<'
        Object o;
        o.kind = Object::String;
        int hi = -1;
        while (pos < s.size() && s[pos] != '>') {
            int v = hexVal(s[pos++]);
            if (v < 0) continue;
            if (hi < 0) {
                hi = v;
            } else {
                o.str += static_cast<char>(hi * 16 + v);
                hi = -1;
            }
        }
        if (hi >= 0) o.str += static_cast<char>(hi * 16);
        if (pos < s.size()) ++pos;
        return o;
    }

    // Reads one object. Keywords other than true/false/null come back as an empty Name with
    // the keyword in `op` (content-stream operators); `op` stays empty otherwise.
    Object parse(std::string* op = nullptr) {
        skipWs();
        Object o;
        if (eof()) return o;
        if (++depth > 200) {
            --depth;
            throw std::runtime_error("PDF objects nested too deeply");
        }
        struct Guard {
            int& d;
            ~Guard() { --d; }
        } guard{depth};
        const char c = s[pos];
        if (c == '/') return name();
        if (c == '(') return literal();
        if (c == '<') {
            if (pos + 1 < s.size() && s[pos + 1] == '<') {
                pos += 2;
                o.kind = Object::Dict;
                while (true) {
                    skipWs();
                    if (eof()) break;
                    if (s.compare(pos, 2, ">>") == 0) {
                        pos += 2;
                        break;
                    }
                    if (s[pos] != '/') {  // malformed: skip a token
                        Object junk = parse();
                        if (junk.kind == Object::Null && !eof() && !isRegular(s[pos]) && s[pos] != '/') ++pos;
                        continue;
                    }
                    std::string key = name().str;
                    skipWs();
                    if (s.compare(pos, 2, ">>") == 0) {
                        o.keys.push_back(key);
                        o.items.emplace_back();
                        continue;
                    }
                    o.keys.push_back(key);
                    o.items.push_back(parse());
                }
                return o;
            }
            return hexString();
        }
        if (c == '[') {
            ++pos;
            o.kind = Object::Array;
            while (true) {
                skipWs();
                if (eof()) break;
                if (s[pos] == ']') {
                    ++pos;
                    break;
                }
                std::string inner;
                Object item = parse(&inner);
                if (!inner.empty() && inner != "R") continue;  // stray keyword
                if (inner.empty() && item.kind == Object::Null && !eof() && (s[pos] == ')' || s[pos] == '>' || s[pos] == '}' || s[pos] == '{')) ++pos;
                o.items.push_back(std::move(item));
            }
            return o;
        }
        if (c == ']' || c == ')' || c == '>' || c == '{' || c == '}') {
            ++pos;
            return o;
        }
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            std::string tok = regularRun();
            o.kind = Object::Number;
            o.number = std::strtod(tok.c_str(), nullptr);
            const bool integer = tok.find('.') == std::string::npos && tok[0] != '-' && tok[0] != '+';
            if (integer) {
                // "12 0 R" is a reference
                std::size_t save = pos;
                skipWs();
                if (!eof() && s[pos] >= '0' && s[pos] <= '9') {
                    std::string gen = regularRun();
                    skipWs();
                    if (gen.find('.') == std::string::npos && !eof() && s[pos] == 'R' && (pos + 1 >= s.size() || !isRegular(s[pos + 1]))) {
                        ++pos;
                        return Object::makeRef(static_cast<int>(o.number), std::atoi(gen.c_str()));
                    }
                }
                pos = save;
            }
            return o;
        }
        std::string kw = regularRun();
        if (kw.empty()) {
            ++pos;
            return o;
        }
        if (kw == "true" || kw == "false") {
            o.kind = Object::Bool;
            o.boolean = kw == "true";
            return o;
        }
        if (kw == "null") return o;
        if (op) *op = kw;
        return o;
    }
};

}  // namespace wetwaresuite::pdf::detail
