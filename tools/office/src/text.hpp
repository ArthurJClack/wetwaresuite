// Small string helpers shared by the office sources.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace wetwaresuite::office {

// UTF-8 text to a path (std::filesystem::u8path is deprecated in C++20).
inline std::filesystem::path pathFromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

inline std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

inline std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trimCopy(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline bool startsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
inline bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

inline std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            out.push_back(cur);
            cur.clear();
        } else if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() || (!text.empty() && text.back() != '\n' && text.back() != '\r')) out.push_back(cur);
    return out;
}

// "2024-03-01" (also 2024/3/1); false otherwise.
inline bool parseIsoDate(const std::string& t, int& y, int& m, int& d) {
    std::string s = trimCopy(t);
    if (s.size() < 8 || s.size() > 10) return false;
    int n = 0;
    char sep1 = 0, sep2 = 0;
    if (std::sscanf(s.c_str(), "%4d%c%2d%c%2d%n", &y, &sep1, &m, &sep2, &d, &n) != 5) return false;
    if (static_cast<std::size_t>(n) != s.size() || sep1 != sep2 || (sep1 != '-' && sep1 != '/')) return false;
    return y >= 1900 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

// A number as text the way formulas join it ("0.3", "1E+20"): up to 15 significant digits.
inline std::string textNumber(double v) {
    if (v == 0) return "0";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    std::string s = buf;
    auto e = s.find('e');
    if (e != std::string::npos) {
        std::string mant = s.substr(0, e), ex = s.substr(e + 1);
        int x = std::atoi(ex.c_str());
        char eb[16];
        std::snprintf(eb, sizeof eb, "E%c%02d", x < 0 ? '-' : '+', x < 0 ? -x : x);
        s = mant + eb;
    }
    return s;
}

inline std::string base64Encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) | static_cast<unsigned char>(in[i + 2]);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += t[(v >> 6) & 63];
        out += t[v & 63];
    }
    if (i < in.size()) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

inline std::string base64Decode(const std::string& in) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (char c : in) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else continue;
        v = (v << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return out;
}

}  // namespace wetwaresuite::office
