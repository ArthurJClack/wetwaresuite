// Glyph names, unicode parsing and UTF-8, plus the Font lookups that use them.
#include <algorithm>
#include <cctype>
#include <cstdio>

#include "wetwaresuite/type/type.hpp"

namespace wetwaresuite::typeface {

const std::vector<std::string>& macGlyphNames() {
    static const std::vector<std::string> names = {
        ".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
        "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash",
        "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less",
        "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright",
        "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
        "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde",
        "Adieresis", "Aring", "Ccedilla", "Eacute", "Ntilde", "Odieresis", "Udieresis", "aacute", "agrave",
        "acircumflex", "adieresis", "atilde", "aring", "ccedilla", "eacute", "egrave", "ecircumflex", "edieresis",
        "iacute", "igrave", "icircumflex", "idieresis", "ntilde", "oacute", "ograve", "ocircumflex", "odieresis",
        "otilde", "uacute", "ugrave", "ucircumflex", "udieresis", "dagger", "degree", "cent", "sterling", "section",
        "bullet", "paragraph", "germandbls", "registered", "copyright", "trademark", "acute", "dieresis", "notequal",
        "AE", "Oslash", "infinity", "plusminus", "lessequal", "greaterequal", "yen", "mu", "partialdiff", "summation",
        "product", "pi", "integral", "ordfeminine", "ordmasculine", "Omega", "ae", "oslash", "questiondown",
        "exclamdown", "logicalnot", "radical", "florin", "approxequal", "Delta", "guillemotleft", "guillemotright",
        "ellipsis", "nonbreakingspace", "Agrave", "Atilde", "Otilde", "OE", "oe", "endash", "emdash", "quotedblleft",
        "quotedblright", "quoteleft", "quoteright", "divide", "lozenge", "ydieresis", "Ydieresis", "fraction",
        "currency", "guilsinglleft", "guilsinglright", "fi", "fl", "daggerdbl", "periodcentered", "quotesinglbase",
        "quotedblbase", "perthousand", "Acircumflex", "Ecircumflex", "Aacute", "Edieresis", "Egrave", "Iacute",
        "Icircumflex", "Idieresis", "Igrave", "Oacute", "Ocircumflex", "apple", "Ograve", "Uacute", "Ucircumflex",
        "Ugrave", "dotlessi", "circumflex", "tilde", "macron", "breve", "dotaccent", "ring", "cedilla", "hungarumlaut",
        "ogonek", "caron", "Lslash", "lslash", "Scaron", "scaron", "Zcaron", "zcaron", "brokenbar", "Eth", "eth",
        "Yacute", "yacute", "Thorn", "thorn", "minus", "multiply", "onesuperior", "twosuperior", "threesuperior",
        "onehalf", "onequarter", "threequarters", "franc", "Gbreve", "gbreve", "Idotaccent", "Scedilla", "scedilla",
        "Cacute", "cacute", "Ccaron", "ccaron", "dcroat"};
    return names;
}

std::vector<std::uint32_t> utf8Decode(const std::string& text) {
    std::vector<std::uint32_t> out;
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n;) {
        const unsigned c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > n) {
            out.push_back(c);  // stray byte: read it as Latin-1
            ++i;
            continue;
        }
        std::uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            if ((s[i + k] & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if (!ok) {
            out.push_back(c);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(len);
    }
    return out;
}

std::string utf8Encode(std::uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

namespace {

std::optional<std::uint32_t> hexValue(const std::string& s) {
    if (s.empty() || s.size() > 6) return std::nullopt;
    std::uint32_t v = 0;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        v = v * 16 + static_cast<std::uint32_t>(std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(c) - 'a' + 10);
    }
    if (v > 0x10FFFF) return std::nullopt;
    return v;
}

}  // namespace

std::optional<std::uint32_t> parseUnicode(const std::string& text) {
    if (text.empty()) return std::nullopt;
    auto cps = utf8Decode(text);
    if (cps.size() == 1) return cps[0];
    if (text.size() > 2 && (text[0] == 'U' || text[0] == 'u') && text[1] == '+') return hexValue(text.substr(2));
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return hexValue(text.substr(2));
    if (text.size() == 7 && text.rfind("uni", 0) == 0) return hexValue(text.substr(3));
    if ((text.size() == 5 || text.size() == 6 || text.size() == 7) && text[0] == 'u' && text.rfind("uni", 0) != 0)
        return hexValue(text.substr(1));
    return std::nullopt;
}

std::string unicodeLabel(std::uint32_t cp) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(cp));
    return buf;
}

std::string glyphNameFor(std::uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) return macGlyphNames()[cp - 29];
    char buf[16];
    if (cp <= 0xFFFF) std::snprintf(buf, sizeof buf, "uni%04X", static_cast<unsigned>(cp));
    else std::snprintf(buf, sizeof buf, "u%05X", static_cast<unsigned>(cp));
    return buf;
}

std::optional<std::uint32_t> unicodeForName(const std::string& name) {
    const auto& mac = macGlyphNames();
    for (std::uint32_t cp = 0x20; cp <= 0x7E; ++cp) {
        if (mac[cp - 29] == name) return cp;
    }
    if (name.size() == 7 && name.rfind("uni", 0) == 0) return hexValue(name.substr(3));
    if (name.size() >= 5 && name.size() <= 7 && name[0] == 'u' && name.rfind("uni", 0) != 0) return hexValue(name.substr(1));
    return std::nullopt;
}

bool validGlyphName(const std::string& name) {
    if (name.empty() || name.size() > 63) return false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E) return false;
        if (std::string("\"<>&/\\[](){}%").find(c) != std::string::npos) return false;
    }
    return true;
}

// ---- Font lookups ----

int Font::index(const std::string& name) const {
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        if (glyphs[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Font::indexForUnicode(std::uint32_t cp) const {
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        for (auto u : glyphs[i].unicodes) {
            if (u == cp) return static_cast<int>(i);
        }
    }
    return -1;
}

int Font::resolve(const std::string& ref) const {
    if (int i = index(ref); i >= 0) return i;
    if (auto cp = parseUnicode(ref)) {
        if (int i = indexForUnicode(*cp); i >= 0) return i;
    }
    return -1;
}

Glyph* Font::glyph(const std::string& ref) {
    int i = resolve(ref);
    return i < 0 ? nullptr : &glyphs[static_cast<std::size_t>(i)];
}

const Glyph* Font::glyph(const std::string& ref) const {
    int i = resolve(ref);
    return i < 0 ? nullptr : &glyphs[static_cast<std::size_t>(i)];
}

double Font::kern(const std::string& left, const std::string& right) const {
    auto it = kerning.find({left, right});
    return it == kerning.end() ? 0.0 : it->second;
}

}  // namespace wetwaresuite::typeface
