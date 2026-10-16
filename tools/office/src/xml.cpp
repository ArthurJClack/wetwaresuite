#include "xml.hpp"

#include <cstdlib>
#include <stdexcept>

#include "achclwss/pdf/fonts.hpp"

namespace achclwss::office::xml {

std::string_view Node::local() const {
    std::string_view n = name;
    auto c = n.find(':');
    return c == std::string_view::npos ? n : n.substr(c + 1);
}

std::string Node::attr(std::string_view key) const {
    for (const auto& [k, v] : attrs) {
        if (k == key) return v;
    }
    if (key.find(':') == std::string_view::npos) {
        for (const auto& [k, v] : attrs) {
            auto c = k.find(':');
            if (c != std::string::npos && std::string_view(k).substr(c + 1) == key) return v;
        }
    }
    return {};
}

bool Node::hasAttr(std::string_view key) const {
    for (const auto& [k, v] : attrs) {
        if (k == key) return true;
        auto c = k.find(':');
        if (key.find(':') == std::string_view::npos && c != std::string::npos && std::string_view(k).substr(c + 1) == key) return true;
    }
    return false;
}

const Node* Node::child(std::string_view localName) const {
    for (const auto& c : children) {
        if (c.is(localName)) return &c;
    }
    return nullptr;
}

std::vector<const Node*> Node::all(std::string_view localName) const {
    std::vector<const Node*> out;
    for (const auto& c : children) {
        if (c.is(localName)) out.push_back(&c);
    }
    return out;
}

void Node::descendants(std::string_view localName, std::vector<const Node*>& out) const {
    for (const auto& c : children) {
        if (c.is(localName)) out.push_back(&c);
        c.descendants(localName, out);
    }
}

const Node* Node::find(std::string_view localName) const {
    for (const auto& c : children) {
        if (c.is(localName)) return &c;
        if (const Node* f = c.find(localName)) return f;
    }
    return nullptr;
}

std::string Node::allText() const {
    if (isText()) return text;
    std::string out;
    for (const auto& c : children) out += c.allText();
    return out;
}

namespace {

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        auto semi = s.find(';', i);
        if (semi == std::string_view::npos || semi - i > 12) {
            out += '&';
            continue;
        }
        std::string_view ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                   ? std::strtoul(std::string(ent.substr(2)).c_str(), nullptr, 16)
                                   : std::strtoul(std::string(ent.substr(1)).c_str(), nullptr, 10);
            out += pdf::utf8Encode(static_cast<unsigned>(cp));
        } else {
            out += '&';
            continue;
        }
        i = semi;
    }
    return out;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

struct Parser {
    std::string_view s;
    std::size_t pos = 0;

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("broken XML: ") + what + " at byte " + std::to_string(pos));
    }

    void skipMisc() {
        // declarations, comments, processing instructions and DOCTYPE before/after the root
        while (pos < s.size()) {
            while (pos < s.size() && isSpace(s[pos])) ++pos;
            if (s.compare(pos, 4, "<!--") == 0) {
                auto e = s.find("-->", pos);
                pos = e == std::string_view::npos ? s.size() : e + 3;
            } else if (s.compare(pos, 2, "<?") == 0) {
                auto e = s.find("?>", pos);
                pos = e == std::string_view::npos ? s.size() : e + 2;
            } else if (s.compare(pos, 2, "<!") == 0) {
                int depth = 0;
                while (pos < s.size()) {
                    char c = s[pos++];
                    if (c == '[') ++depth;
                    else if (c == ']') --depth;
                    else if (c == '>' && depth <= 0) break;
                }
            } else {
                break;
            }
        }
    }

    std::string name() {
        std::size_t start = pos;
        while (pos < s.size() && !isSpace(s[pos]) && s[pos] != '>' && s[pos] != '/' && s[pos] != '=') ++pos;
        if (pos == start) fail("expected a name");
        return std::string(s.substr(start, pos - start));
    }

    Node element(int depth) {
        if (depth > 512) fail("nested too deeply");
        if (pos >= s.size() || s[pos] != '<') fail("expected '<'");
        ++pos;
        Node n;
        n.name = name();
        while (true) {
            while (pos < s.size() && isSpace(s[pos])) ++pos;
            if (pos >= s.size()) fail("unterminated tag");
            if (s[pos] == '/') {
                if (pos + 1 < s.size() && s[pos + 1] == '>') {
                    pos += 2;
                    return n;
                }
                fail("stray '/'");
            }
            if (s[pos] == '>') {
                ++pos;
                break;
            }
            std::string key = name();
            while (pos < s.size() && isSpace(s[pos])) ++pos;
            if (pos >= s.size() || s[pos] != '=') fail("expected '='");
            ++pos;
            while (pos < s.size() && isSpace(s[pos])) ++pos;
            if (pos >= s.size() || (s[pos] != '"' && s[pos] != '\'')) fail("expected a quoted value");
            char q = s[pos++];
            auto end = s.find(q, pos);
            if (end == std::string_view::npos) fail("unterminated attribute");
            n.attrs.emplace_back(std::move(key), decodeEntities(s.substr(pos, end - pos)));
            pos = end + 1;
        }
        // content
        while (true) {
            if (pos >= s.size()) fail(("unclosed <" + n.name + ">").c_str());
            if (s[pos] == '<') {
                if (s.compare(pos, 2, "</") == 0) {
                    pos += 2;
                    std::string closing = name();
                    while (pos < s.size() && s[pos] != '>') ++pos;
                    ++pos;
                    if (closing != n.name) fail(("mismatched </" + closing + ">").c_str());
                    return n;
                }
                if (s.compare(pos, 4, "<!--") == 0) {
                    auto e = s.find("-->", pos);
                    pos = e == std::string_view::npos ? s.size() : e + 3;
                    continue;
                }
                if (s.compare(pos, 9, "<![CDATA[") == 0) {
                    auto e = s.find("]]>", pos);
                    if (e == std::string_view::npos) fail("unterminated CDATA");
                    Node t;
                    t.text = std::string(s.substr(pos + 9, e - pos - 9));
                    n.children.push_back(std::move(t));
                    pos = e + 3;
                    continue;
                }
                if (s.compare(pos, 2, "<?") == 0) {
                    auto e = s.find("?>", pos);
                    pos = e == std::string_view::npos ? s.size() : e + 2;
                    continue;
                }
                n.children.push_back(element(depth + 1));
                continue;
            }
            auto next = s.find('<', pos);
            if (next == std::string_view::npos) next = s.size();
            Node t;
            t.text = decodeEntities(s.substr(pos, next - pos));
            if (!n.children.empty() && n.children.back().isText()) n.children.back().text += t.text;
            else n.children.push_back(std::move(t));
            pos = next;
        }
    }
};

}  // namespace

Node parse(std::string_view text) {
    Parser p{text};
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) p.pos = 3;
    p.skipMisc();
    if (p.pos >= text.size()) throw std::runtime_error("broken XML: no root element");
    return p.element(0);
}

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                // control characters are not allowed in XML 1.0
                if (static_cast<unsigned char>(c) < 32 && c != '\n' && c != '\t' && c != '\r') break;
                out += c;
        }
    }
    return out;
}

Writer::Writer() { out_ = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"; }

void Writer::finishTag() {
    if (tagOpen_) {
        out_ += '>';
        tagOpen_ = false;
    }
}

Writer& Writer::open(std::string_view name) {
    finishTag();
    out_ += '<';
    out_ += name;
    stack_.emplace_back(name);
    tagOpen_ = true;
    return *this;
}

Writer& Writer::attr(std::string_view key, std::string_view value) {
    out_ += ' ';
    out_ += key;
    out_ += "=\"";
    out_ += escape(value);
    out_ += '"';
    return *this;
}

Writer& Writer::text(std::string_view content) {
    finishTag();
    out_ += escape(content);
    return *this;
}

Writer& Writer::raw(std::string_view xml) {
    finishTag();
    out_ += xml;
    return *this;
}

Writer& Writer::close() {
    if (stack_.empty()) return *this;
    if (tagOpen_) {
        out_ += "/>";
        tagOpen_ = false;
    } else {
        out_ += "</" + stack_.back() + ">";
    }
    stack_.pop_back();
    return *this;
}

Writer& Writer::leaf(std::string_view name, std::vector<std::pair<std::string, std::string>> attrs) {
    open(name);
    for (const auto& [k, v] : attrs) attr(k, v);
    return close();
}

Writer& Writer::element(std::string_view name, std::string_view content) {
    open(name);
    text(content);
    return close();
}

std::string Writer::str() {
    while (!stack_.empty()) close();
    return out_;
}

}  // namespace achclwss::office::xml
