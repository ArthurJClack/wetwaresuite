// A small s-expression reader for KiCad files (netlists, schematics, boards). Internal.
#pragma once

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace wetwaresuite::circuit {

struct Sx {
    std::string atom;
    std::vector<Sx> list;
    bool isList = false;
    bool quoted = false;

    const std::string& head() const {
        static const std::string none;
        return isList && !list.empty() && !list[0].isList ? list[0].atom : none;
    }
    bool is(const char* h) const { return head() == h; }
    const Sx* child(const std::string& h) const {
        for (const auto& c : list) {
            if (c.head() == h) return &c;
        }
        return nullptr;
    }
    std::string value(const std::string& h) const {
        const Sx* c = child(h);
        return c && c->list.size() > 1 && !c->list[1].isList ? c->list[1].atom : std::string();
    }
    // the i-th element as text ("" past the end or for a list)
    std::string str(std::size_t i) const { return i < list.size() && !list[i].isList ? list[i].atom : std::string(); }
    double num(std::size_t i, double fallback = 0) const {
        const std::string s = str(i);
        if (s.empty()) return fallback;
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        return end && end != s.c_str() ? v : fallback;
    }
    // a bare flag such as `hide` among the elements, or KiCad 8's (hide yes)
    bool flag(const std::string& name) const {
        for (std::size_t i = 1; i < list.size(); ++i) {
            if (!list[i].isList && !list[i].quoted && list[i].atom == name) return true;
        }
        const Sx* c = child(name);
        return c && (c->list.size() < 2 || c->str(1) == "yes");
    }
};

inline Sx parseSx(const std::string& s, std::size_t& i, int depth = 0) {
    if (depth > 200) throw std::runtime_error("file nested too deeply");
    auto skip = [&] {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    };
    skip();
    Sx node;
    if (i >= s.size()) throw std::runtime_error("unexpected end of file");
    if (s[i] == '(') {
        node.isList = true;
        ++i;
        for (;;) {
            skip();
            if (i >= s.size()) throw std::runtime_error("unbalanced parentheses");
            if (s[i] == ')') {
                ++i;
                break;
            }
            node.list.push_back(parseSx(s, i, depth + 1));
        }
    } else if (s[i] == '"') {
        node.quoted = true;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                node.atom += s[i] == 'n' ? '\n' : s[i];
                ++i;
                continue;
            }
            node.atom += s[i++];
        }
        ++i;
    } else {
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(' && s[i] != ')') node.atom += s[i++];
    }
    return node;
}

inline Sx parseSx(const std::string& s) {
    std::size_t i = 0;
    return parseSx(s, i);
}

// A KiCad library part name ("R", "LED", "Conn_01x04") to a built-in symbol name, "" if none.
// `pins` gets the pin count for sized symbols (headers). Lives in io.cpp.
std::string mapKicadPart(const std::string& part, int& pins);

}  // namespace wetwaresuite::circuit
