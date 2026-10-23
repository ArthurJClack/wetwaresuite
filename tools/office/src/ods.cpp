// OpenDocument spreadsheets (.ods), read and written natively: values,
// formulas (OpenFormula syntax), number formats as data styles, cell styles,
// column widths, row heights, hidden rows/columns, merges, array formulas,
// named ranges, the auto-filter range and frozen panes.
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

#include "wetwaresuite/raster/ora.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"
#include "xlsx_internal.hpp"
#include "xml.hpp"

namespace wetwaresuite::office {

// ---- formulas -------------------------------------------------------------------------

std::string odfFormula(const std::string& in) {
    std::string f = in;
    for (const char* pre : {"of:", "oooc:", "msoxl:"}) {
        if (startsWith(f, pre)) f = f.substr(std::char_traits<char>::length(pre));
    }
    if (!f.empty() && f[0] == '=') f.erase(0, 1);
    std::string out;
    bool str = false;
    int braces = 0;
    for (std::size_t i = 0; i < f.size(); ++i) {
        char c = f[i];
        if (c == '"') str = !str;
        if (str) {
            out += c;
            continue;
        }
        if (c == '{') ++braces;
        if (c == '}') --braces;
        if (c == ';') {
            out += braces > 0 ? ',' : ',';
            continue;
        }
        if (c == '|' && braces > 0) {
            out += ';';
            continue;
        }
        if (c == '[') {
            auto e = f.find(']', i);
            if (e == std::string::npos) {
                out += c;
                continue;
            }
            std::string ref = f.substr(i + 1, e - i - 1);  // ".A1:.B2" or "Sheet2.A1" or "$'My sheet'.A1"
            std::string res;
            std::size_t p = 0;
            bool firstPart = true;
            while (p <= ref.size()) {
                auto colon = ref.find(':', p);
                std::string part = ref.substr(p, colon == std::string::npos ? std::string::npos : colon - p);
                // split sheet and cell at the last '.' outside quotes
                std::string sheet, cell = part;
                bool q = false;
                std::size_t dot = std::string::npos;
                for (std::size_t k = 0; k < part.size(); ++k) {
                    if (part[k] == '\'') q = !q;
                    if (part[k] == '.' && !q) dot = k;
                }
                if (dot != std::string::npos) {
                    sheet = part.substr(0, dot);
                    cell = part.substr(dot + 1);
                }
                if (!sheet.empty() && sheet[0] == '$') sheet.erase(0, 1);
                if (sheet.size() >= 2 && sheet.front() == '\'' && sheet.back() == '\'') {
                    std::string s;
                    for (std::size_t k = 1; k + 1 < sheet.size(); ++k) {
                        s += sheet[k];
                        if (sheet[k] == '\'' && sheet[k + 1] == '\'') ++k;
                    }
                    sheet = s;
                }
                if (!res.empty()) res += ":";
                if (firstPart && !sheet.empty()) res += quoteSheet(sheet) + "!";
                res += cell;
                firstPart = false;
                if (colon == std::string::npos) break;
                p = colon + 1;
            }
            out += res;
            i = e;
            continue;
        }
        out += c;
    }
    return out;
}

namespace {

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || static_cast<unsigned char>(c) >= 0x80; }

// $?COL$?ROW at i; returns the length (0 when there is none).
std::size_t cellAt(const std::string& f, std::size_t i, bool allowColumnOnly) {
    std::size_t k = i;
    if (k < f.size() && f[k] == '$') ++k;
    const std::size_t l0 = k;
    while (k < f.size() && std::isalpha(static_cast<unsigned char>(f[k])) && k - l0 < 3) ++k;
    if (k == l0) return 0;
    if (k < f.size() && std::isalpha(static_cast<unsigned char>(f[k]))) return 0;
    std::size_t m = k;
    if (m < f.size() && f[m] == '$') ++m;
    const std::size_t d0 = m;
    while (m < f.size() && std::isdigit(static_cast<unsigned char>(f[m]))) ++m;
    if (m == d0) return allowColumnOnly && m == k ? k - i : 0;
    return m - i;
}

std::string odfSheet(const std::string& name) {
    bool plain = !name.empty();
    for (char c : name) plain = plain && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
    if (plain) return name;
    std::string q = "'";
    for (char c : name) q += c == '\'' ? std::string("''") : std::string(1, c);
    return q + "'";
}

// A1 syntax ("SUM(Data!A1:B2)") to OpenFormula ("SUM([Data.A1:.B2])").
std::string toOdf(const std::string& f) {
    std::string out;
    int braces = 0;
    for (std::size_t i = 0; i < f.size();) {
        const char c = f[i];
        if (c == '"') {
            std::size_t e = i + 1;
            while (e < f.size()) {
                if (f[e] == '"' && e + 1 < f.size() && f[e + 1] == '"') e += 2;
                else if (f[e] == '"') break;
                else ++e;
            }
            out += f.substr(i, e - i + 1);
            i = e + 1;
            continue;
        }
        if (c == '{') ++braces;
        if (c == '}') --braces;
        if (c == ',') {
            out += ';';
            ++i;
            continue;
        }
        if (c == ';' && braces > 0) {
            out += '|';
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t e = i;
            while (e < f.size() && (std::isdigit(static_cast<unsigned char>(f[e])) || f[e] == '.')) ++e;
            if (e < f.size() && (f[e] == 'E' || f[e] == 'e') && e + 1 < f.size() && (std::isdigit(static_cast<unsigned char>(f[e + 1])) || f[e + 1] == '+' || f[e + 1] == '-')) {
                e += 2;
                while (e < f.size() && std::isdigit(static_cast<unsigned char>(f[e]))) ++e;
            }
            out += f.substr(i, e - i);
            i = e;
            continue;
        }
        if (c == '\'' || c == '$' || std::isalpha(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80) {
            // an optional sheet prefix, then a cell or range
            std::size_t k = i;
            std::string sheet;
            if (c == '\'') {
                std::size_t e = i + 1;
                while (e < f.size()) {
                    if (f[e] == '\'' && e + 1 < f.size() && f[e + 1] == '\'') {
                        sheet += '\'';
                        e += 2;
                    } else if (f[e] == '\'') {
                        break;
                    } else {
                        sheet += f[e++];
                    }
                }
                if (e + 1 < f.size() && f[e + 1] == '!') k = e + 2;
                else sheet.clear();
            } else {
                std::size_t e = i;
                while (e < f.size() && isIdent(f[e])) ++e;
                if (e < f.size() && f[e] == '!' && e > i) {
                    sheet = f.substr(i, e - i);
                    k = e + 1;
                }
            }
            const std::size_t a = cellAt(f, k, true);
            bool ref = a > 0;
            std::size_t end = k + a;
            std::size_t b = 0;
            if (ref && end < f.size() && f[end] == ':') {
                b = cellAt(f, end + 1, true);
                if (b) end = end + 1 + b;
            }
            const bool columnOnly = ref && f.substr(k, a).find_first_of("0123456789") == std::string::npos;
            if (columnOnly && !b) ref = false;
            if (ref && end < f.size() && (f[end] == '(' || isIdent(f[end]))) ref = false;
            if (ref) {
                std::string first = f.substr(k, a);
                out += "[" + (sheet.empty() ? std::string() : "$" + odfSheet(sheet)) + "." + first;
                if (b) out += ":." + f.substr(end - b, b);
                out += "]";
                i = end;
                continue;
            }
            // a function name, a defined name or TRUE/FALSE
            std::size_t e = i;
            while (e < f.size() && (isIdent(f[e]) || f[e] == '$' || f[e] == '\'' || f[e] == '!')) {
                if (f[e] == '\'' && e != i) break;
                ++e;
            }
            if (e == i) e = i + 1;
            std::string word = f.substr(i, e - i);
            const std::string up = upper(word);
            if ((up == "TRUE" || up == "FALSE") && (e >= f.size() || f[e] != '(')) word += "()";
            out += word;
            i = e;
            continue;
        }
        out += c;
        ++i;
    }
    return "of:=" + out;
}

// ---- number formats as data styles ----------------------------------------------------

std::string attrEsc(const std::string& s) { return xml::escape(s); }

std::string textEl(const std::string& s) { return s.empty() ? std::string() : "<number:text>" + xml::escape(s) + "</number:text>"; }

// Literal text of a format piece ("$", " kg", "\"x\""), quotes and escapes removed.
std::string literalOf(const std::string& s) {
    std::string out;
    bool q = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            q = !q;
            continue;
        }
        if (q) {
            out += c;
            continue;
        }
        if (c == '\\' && i + 1 < s.size()) {
            out += s[++i];
            continue;
        }
        if (c == '_' || c == '*') {
            ++i;
            continue;
        }
        if (c == '[') {
            auto e = s.find(']', i);
            if (e == std::string::npos) break;
            const std::string in = s.substr(i + 1, e - i - 1);
            if (!in.empty() && in[0] == '$') {
                auto dash = in.find('-');
                out += in.substr(1, dash == std::string::npos ? std::string::npos : dash - 1);
            }
            i = e;
            continue;
        }
        out += c;
    }
    return out;
}

std::string dateStyle(const std::string& name, const std::string& code) {
    std::string body;
    const std::string sec = code.substr(0, code.find(';'));
    const std::string low = lower(sec);
    bool timeOnly = low.find_first_of("yd") == std::string::npos;
    bool lastHour = false;
    std::string lit;
    auto flushLit = [&] {
        body += textEl(lit);
        lit.clear();
    };
    for (std::size_t i = 0; i < sec.size();) {
        const char c = sec[i];
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '"' || c == '\\') {
            std::size_t e = i + 1;
            if (c == '"') {
                e = sec.find('"', i + 1);
                if (e == std::string::npos) e = sec.size();
                lit += sec.substr(i + 1, e - i - 1);
                i = e + 1;
            } else {
                if (i + 1 < sec.size()) lit += sec[i + 1];
                i += 2;
            }
            continue;
        }
        if (c == '[') {
            auto e = sec.find(']', i);
            i = e == std::string::npos ? sec.size() : e + 1;
            continue;
        }
        if (lc == 'a' && lower(sec.substr(i, 5)) == "am/pm") {
            flushLit();
            body += "<number:am-pm/>";
            i += 5;
            continue;
        }
        std::size_t run = 1;
        while (i + run < sec.size() && std::tolower(static_cast<unsigned char>(sec[i + run])) == lc) ++run;
        const char* longStyle = " number:style=\"long\"";
        if (lc == 'y') {
            flushLit();
            body += std::string("<number:year") + (run >= 3 ? longStyle : "") + "/>";
            lastHour = false;
        } else if (lc == 'm') {
            flushLit();
            std::size_t k = i + run;
            while (k < sec.size() && (sec[k] == ':' || sec[k] == ' ')) ++k;
            const bool minutes = lastHour || (k < sec.size() && std::tolower(static_cast<unsigned char>(sec[k])) == 's');
            if (minutes) body += std::string("<number:minutes") + (run >= 2 ? longStyle : "") + "/>";
            else if (run >= 3) body += std::string("<number:month number:textual=\"true\"") + (run >= 4 ? longStyle : "") + "/>";
            else body += std::string("<number:month") + (run >= 2 ? longStyle : "") + "/>";
            lastHour = false;
        } else if (lc == 'd') {
            flushLit();
            if (run >= 3) body += std::string("<number:day-of-week") + (run >= 4 ? longStyle : "") + "/>";
            else body += std::string("<number:day") + (run >= 2 ? longStyle : "") + "/>";
            lastHour = false;
        } else if (lc == 'h') {
            flushLit();
            body += std::string("<number:hours") + (run >= 2 ? longStyle : "") + "/>";
            lastHour = true;
        } else if (lc == 's') {
            flushLit();
            body += std::string("<number:seconds") + (run >= 2 ? longStyle : "") + "/>";
            lastHour = false;
        } else {
            lit.append(run, c);
        }
        i += run;
    }
    flushLit();
    const char* tag = timeOnly ? "number:time-style" : "number:date-style";
    return std::string("<") + tag + " style:name=\"" + name + "\">" + body + "</" + tag + ">";
}

std::string numberStyle(const std::string& name, const std::string& code) {
    const std::string sec = code.substr(0, code.find(';'));
    std::size_t first = std::string::npos, last = std::string::npos;
    bool q = false;
    for (std::size_t i = 0; i < sec.size(); ++i) {
        const char c = sec[i];
        if (c == '"') q = !q;
        if (q) continue;
        if (c == '\\') {
            ++i;
            continue;
        }
        if (c == '[') {
            auto e = sec.find(']', i);
            if (e == std::string::npos) break;
            i = e;
            continue;
        }
        if (c == '0' || c == '#' || c == '?' || ((c == '.' || c == ',') && first != std::string::npos)) {
            if (first == std::string::npos) first = i;
            last = i;
        }
    }
    if (first == std::string::npos) return {};
    const std::string core = sec.substr(first, last - first + 1);
    const std::string prefix = literalOf(sec.substr(0, first)), suffix = literalOf(sec.substr(last + 1));
    const auto dot = core.find('.');
    int decimals = 0, minInt = 0;
    if (dot != std::string::npos) {
        for (char c : core.substr(dot + 1)) decimals += c == '0' || c == '#' ? 1 : 0;
    }
    for (char c : core.substr(0, dot)) minInt += c == '0' ? 1 : 0;
    const bool grouping = core.substr(0, dot).find(',') != std::string::npos;
    const bool percent = suffix.find('%') != std::string::npos;
    const bool sci = lower(sec).find("e+") != std::string::npos || lower(sec).find("e-") != std::string::npos;
    std::string num;
    if (sci) {
        num = "<number:scientific-number number:decimal-places=\"" + std::to_string(decimals) + "\" number:min-integer-digits=\"" +
              std::to_string(std::max(1, minInt)) + "\" number:min-exponent-digits=\"2\"/>";
    } else {
        num = "<number:number number:decimal-places=\"" + std::to_string(decimals) + "\" number:min-decimal-places=\"" + std::to_string(decimals) +
              "\" number:min-integer-digits=\"" + std::to_string(minInt) + "\"" + (grouping ? " number:grouping=\"true\"" : "") + "/>";
    }
    const char* tag = percent ? "number:percentage-style" : "number:number-style";
    std::string suf = suffix;
    if (percent) suf.erase(suf.find('%'), 1);
    return std::string("<") + tag + " style:name=\"" + name + "\">" + textEl(prefix) + num + textEl(suf) + (percent ? "<number:text>%</number:text>" : "") +
           "</" + tag + ">";
}

// The data style element for a number format code, or "" when it has none (General, text).
std::string dataStyle(const std::string& name, const std::string& code) {
    if (code.empty() || lower(code) == "general" || code == "@") return {};
    if (isDateFormat(code)) return dateStyle(name, code);
    return numberStyle(name, code);
}

// And back: a data style element as a format code.
std::string formatOf(const xml::Node& ds) {
    std::string out;
    auto isLong = [](const xml::Node& n) { return n.attr("number:style") == "long"; };
    for (const auto& c : ds.children) {
        if (c.isText()) continue;
        const std::string_view n = c.local();
        if (n == "text") {
            const std::string t = c.allText();
            if (t == "%") {
                out += "%";
                continue;
            }
            bool plain = true;
            for (char ch : t) plain = plain && (std::string(" -/:.,()$").find(ch) != std::string::npos || static_cast<unsigned char>(ch) >= 0x80);
            out += plain ? t : "\"" + t + "\"";
        } else if (n == "currency-symbol") {
            out += c.allText();
        } else if (n == "number" || n == "scientific-number") {
            const int dec = std::atoi(c.attr("number:decimal-places").c_str());
            const int minInt = c.hasAttr("number:min-integer-digits") ? std::atoi(c.attr("number:min-integer-digits").c_str()) : 1;
            std::string core;
            if (c.attr("number:grouping") == "true") core = minInt >= 1 ? "#,##0" : "#,###";
            else core = minInt >= 1 ? std::string(static_cast<std::size_t>(minInt), '0') : "#";
            if (dec > 0) core += "." + std::string(static_cast<std::size_t>(dec), '0');
            if (n == "scientific-number") core += "E+00";
            out += core;
        } else if (n == "year") {
            out += isLong(c) ? "yyyy" : "yy";
        } else if (n == "month") {
            if (c.attr("number:textual") == "true") out += isLong(c) ? "mmmm" : "mmm";
            else out += isLong(c) ? "mm" : "m";
        } else if (n == "day") {
            out += isLong(c) ? "dd" : "d";
        } else if (n == "day-of-week") {
            out += isLong(c) ? "dddd" : "ddd";
        } else if (n == "hours") {
            out += isLong(c) ? "hh" : "h";
        } else if (n == "minutes") {
            out += isLong(c) ? "mm" : "m";
        } else if (n == "seconds") {
            out += isLong(c) ? "ss" : "s";
        } else if (n == "am-pm") {
            out += "AM/PM";
        } else if (n == "text-content") {
            out += "@";
        }
    }
    return out;
}

// ---- cell styles ----------------------------------------------------------------------

std::string hexColor(std::uint32_t c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%06x", static_cast<unsigned>(rgbOf(c)));
    return b;
}

const char* borderOdf(int code) {
    switch (code) {
        case 2: return "1.76pt solid";
        case 3: return "2.49pt solid";
        case 4: return "2.01pt double";
        case 5: return "0.74pt dashed";
        case 6: return "0.74pt dotted";
        default: return "0.74pt solid";
    }
}

int borderFromOdf(const std::string& v, std::uint32_t& color) {
    if (v.empty() || v == "none") return 0;
    const double w = std::atof(v.c_str()) * (v.find("mm") != std::string::npos ? 72 / 25.4 : v.find("cm") != std::string::npos ? 72 / 2.54
                                                                                       : v.find("in") != std::string::npos ? 72
                                                                                                                             : 1);
    if (auto h = v.find('#'); h != std::string::npos) parseColor(v.substr(h, 7), color);
    if (v.find("double") != std::string::npos) return 4;
    if (v.find("dashed") != std::string::npos || v.find("dash") != std::string::npos) return 5;
    if (v.find("dotted") != std::string::npos) return 6;
    if (w >= 2.2) return 3;
    if (w >= 1.2) return 2;
    return 1;
}

std::string cellStyleXml(const std::string& name, const CellStyle& s, const std::string& dataStyleName) {
    std::string x = "<style:style style:name=\"" + name + "\" style:family=\"table-cell\" style:parent-style-name=\"Default\"";
    if (!dataStyleName.empty()) x += " style:data-style-name=\"" + dataStyleName + "\"";
    x += ">";
    std::string cell;
    if (hasColor(s.fill)) cell += " fo:background-color=\"" + hexColor(s.fill) + "\"";
    static const char* sides[] = {"left", "right", "top", "bottom"};
    for (int i = 0; i < 4; ++i) {
        if (!s.border[i]) continue;
        cell += std::string(" fo:border-") + sides[i] + "=\"" + borderOdf(s.border[i]) + " " +
                (hasColor(s.borderColor) ? hexColor(s.borderColor) : std::string("#000000")) + "\"";
    }
    if (s.wrap) cell += " fo:wrap-option=\"wrap\"";
    if (s.valign) cell += std::string(" style:vertical-align=\"") + (s.valign == 't' ? "top" : "middle") + "\"";
    if (s.align) cell += " style:text-align-source=\"fix\"";
    if (!cell.empty()) x += "<style:table-cell-properties" + cell + "/>";
    std::string para;
    if (s.align) para += std::string(" fo:text-align=\"") + (s.align == 'l' ? "start" : s.align == 'c' ? "center" : "end") + "\"";
    if (s.indent) para += " fo:margin-left=\"" + textNumber(s.indent * 0.125) + "in\"";
    if (!para.empty()) x += "<style:paragraph-properties" + para + "/>";
    std::string text;
    if (s.bold) text += " fo:font-weight=\"bold\" style:font-weight-asian=\"bold\" style:font-weight-complex=\"bold\"";
    if (s.italic) text += " fo:font-style=\"italic\" style:font-style-asian=\"italic\" style:font-style-complex=\"italic\"";
    if (s.underline) text += " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"";
    if (s.strike) text += " style:text-line-through-style=\"solid\"";
    if (hasColor(s.color)) text += " fo:color=\"" + hexColor(s.color) + "\"";
    if (s.size > 0) text += " fo:font-size=\"" + textNumber(s.size) + "pt\"";
    if (!text.empty()) x += "<style:text-properties" + text + "/>";
    return x + "</style:style>";
}

double odfLength(const std::string& v) {
    double x = std::atof(v.c_str());
    if (endsWith(v, "cm")) return x / 2.54 * 72;
    if (endsWith(v, "mm")) return x / 25.4 * 72;
    if (endsWith(v, "in")) return x * 72;
    if (endsWith(v, "pc")) return x * 12;
    return x;
}

// Text of a <text:p>, honouring <text:s text:c="n"/>, tabs and line breaks.
std::string odfText(const xml::Node& n) {
    std::string out;
    for (const auto& c : n.children) {
        if (c.isText()) out += c.text;
        else if (c.is("s")) out.append(static_cast<std::size_t>(std::max(1, std::atoi(c.attr("text:c").c_str()))), ' ');
        else if (c.is("tab")) out += '\t';
        else if (c.is("line-break")) out += '\n';
        else if (c.is("note") || c.is("annotation")) continue;
        else out += odfText(c);
    }
    return out;
}

// Paragraph markup for text: one <text:p> per line, runs of spaces as <text:s/>.
std::string paragraphs(const std::string& s) {
    std::string out;
    std::size_t start = 0;
    while (true) {
        const auto nl = s.find('\n', start);
        const std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        out += "<text:p>";
        for (std::size_t i = 0; i < line.size();) {
            if (line[i] == ' ' && (i == 0 || (i + 1 < line.size() && line[i + 1] == ' '))) {
                std::size_t e = i;
                while (e < line.size() && line[e] == ' ') ++e;
                const std::size_t n = e - i;
                out += n == 1 ? "<text:s/>" : "<text:s text:c=\"" + std::to_string(n) + "\"/>";
                i = e;
            } else {
                std::size_t e = i;
                while (e < line.size() && !(line[e] == ' ' && (e + 1 < line.size() && line[e + 1] == ' '))) ++e;
                out += xml::escape(line.substr(i, e - i));
                i = e;
            }
        }
        out += "</text:p>";
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

}  // namespace

// ---- writing --------------------------------------------------------------------------

namespace {

const char* kOdsNs =
    " xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\""
    " xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\""
    " xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\""
    " xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\""
    " xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\""
    " xmlns:number=\"urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0\""
    " xmlns:of=\"urn:oasis:names:tc:opendocument:xmlns:of:1.2\""
    " xmlns:config=\"urn:oasis:names:tc:opendocument:xmlns:config:1.0\""
    " office:version=\"1.3\"";

struct OdsStyles {
    std::map<std::string, std::string> dataNames;  // format code -> N1
    std::map<std::string, std::string> cellNames;  // style xml key -> ce1
    std::map<std::string, std::string> colNames, rowNames;
    std::string xml;

    std::string dataStyleFor(const std::string& code) {
        if (code.empty()) return {};
        auto it = dataNames.find(code);
        if (it != dataNames.end()) return it->second;
        const std::string name = "N" + std::to_string(dataNames.size() + 1);
        const std::string el = dataStyle(name, code);
        dataNames[code] = el.empty() ? std::string() : name;
        xml += el;
        return el.empty() ? std::string() : name;
    }
    std::string cellStyleFor(const Cell& c) {
        if (c.style.plain() && c.format.empty()) return {};
        const std::string data = dataStyleFor(c.format);
        const std::string key = cellStyleXml("", c.style, data);
        auto it = cellNames.find(key);
        if (it != cellNames.end()) return it->second;
        const std::string name = "ce" + std::to_string(cellNames.size() + 1);
        cellNames[key] = name;
        xml += cellStyleXml(name, c.style, data);
        return name;
    }
    std::string columnStyle(double widthChars) {
        const std::string w = textNumber(std::round((widthChars * 7 + 5) * 0.75 * 100) / 100) + "pt";
        auto it = colNames.find(w);
        if (it != colNames.end()) return it->second;
        const std::string name = "co" + std::to_string(colNames.size() + 1);
        colNames[w] = name;
        xml += "<style:style style:name=\"" + name + "\" style:family=\"table-column\"><style:table-column-properties fo:break-before=\"auto\" style:column-width=\"" + w +
               "\"/></style:style>";
        return name;
    }
    std::string rowStyle(double heightPt) {
        const std::string h = textNumber(heightPt) + "pt";
        auto it = rowNames.find(h);
        if (it != rowNames.end()) return it->second;
        const std::string name = "ro" + std::to_string(rowNames.size() + 1);
        rowNames[h] = name;
        xml += "<style:style style:name=\"" + name + "\" style:family=\"table-row\"><style:table-row-properties style:row-height=\"" + h +
               "\" fo:break-before=\"auto\" style:use-optimal-row-height=\"" + (heightPt == 15 ? "true" : "false") + "\"/></style:style>";
        return name;
    }
};

std::string isoDate(double serial) {
    int y, m, d;
    serialDate(serial, y, m, d);
    char b[40];
    const double frac = serial - std::floor(serial);
    long secs = std::lround(frac * 86400);
    if (secs >= 86400) secs = 86399;
    if (secs) std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02ld:%02ld:%02ld", y, m, d, secs / 3600, (secs / 60) % 60, secs % 60);
    else std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    return b;
}

std::string cellXml(const Cell& c, const std::string& style, const Range* merge, OdsStyles&) {
    std::string x = "<table:table-cell";
    if (!style.empty()) x += " table:style-name=\"" + style + "\"";
    if (merge) x += " table:number-columns-spanned=\"" + std::to_string(merge->cols()) + "\" table:number-rows-spanned=\"" + std::to_string(merge->rows()) + "\"";
    if (c.hasFormula() && !c.spilled) {
        x += " table:formula=\"" + attrEsc(toOdf(c.formula)) + "\"";
        if (c.spillRows > 0 && c.spillCols > 0 && !c.value.isError()) {
            x += " table:number-matrix-columns-spanned=\"" + std::to_string(c.spillCols) + "\" table:number-matrix-rows-spanned=\"" +
                 std::to_string(c.spillRows) + "\"";
        }
    }
    const CellValue& v = c.value;
    const std::string shown = formatValue(v, c.format);
    switch (v.type) {
        case CellValue::Number: {
            const std::string low = lower(c.format);
            if (isDateFormat(c.format) && low.find_first_of("yd") == std::string::npos && v.number < 1 && v.number >= 0) {
                long secs = std::lround(v.number * 86400);
                char b[40];
                std::snprintf(b, sizeof b, "PT%02ldH%02ldM%02ldS", secs / 3600, (secs / 60) % 60, secs % 60);
                x += std::string(" office:value-type=\"time\" office:time-value=\"") + b + "\"";
            } else if (isDateFormat(c.format) && v.number >= 1) {
                x += " office:value-type=\"date\" office:date-value=\"" + isoDate(v.number) + "\"";
            } else if (!isDateFormat(c.format) && c.format.find('%') != std::string::npos) {
                x += " office:value-type=\"percentage\" office:value=\"" + textNumber(v.number) + "\"";
            } else {
                x += " office:value-type=\"float\" office:value=\"" + textNumber(v.number) + "\"";
            }
            break;
        }
        case CellValue::Bool: x += std::string(" office:value-type=\"boolean\" office:boolean-value=\"") + (v.number != 0 ? "true" : "false") + "\""; break;
        case CellValue::Text: x += " office:value-type=\"string\""; break;
        case CellValue::Error: x += " office:value-type=\"string\""; break;
        case CellValue::Empty: break;
    }
    if (v.empty()) return x + "/>";
    return x + ">" + paragraphs(shown) + "</table:table-cell>";
}

std::string tableXml(const Sheet& sh, OdsStyles& styles) {
    std::string x = "<table:table table:name=\"" + attrEsc(sh.name) + "\">";
    int r1, c1, r2 = -1, c2 = -1;
    const bool any = sh.bounds(r1, c1, r2, c2);
    for (const auto& [c, w] : sh.colWidths) c2 = std::max(c2, c);
    for (int c : sh.hiddenCols) c2 = std::max(c2, c);
    for (const auto& [r, h] : sh.rowHeights) r2 = std::max(r2, r);
    for (int r : sh.hiddenRows) r2 = std::max(r2, r);
    for (const auto& m : sh.merges) r2 = std::max(r2, m.r2), c2 = std::max(c2, m.c2);
    (void)any;
    // columns, runs of equal ones merged
    const int ncols = std::max(1, c2 + 1);
    for (int c = 0; c < ncols;) {
        auto w = sh.colWidths.find(c);
        const std::string style = styles.columnStyle(w == sh.colWidths.end() ? 8.43 : w->second);
        const bool hidden = sh.hiddenCols.count(c) > 0;
        int run = 1;
        while (c + run < ncols) {
            auto w2 = sh.colWidths.find(c + run);
            if (styles.columnStyle(w2 == sh.colWidths.end() ? 8.43 : w2->second) != style || (sh.hiddenCols.count(c + run) > 0) != hidden) break;
            ++run;
        }
        x += "<table:table-column table:style-name=\"" + style + "\"";
        if (run > 1) x += " table:number-columns-repeated=\"" + std::to_string(run) + "\"";
        if (hidden) x += " table:visibility=\"collapse\"";
        x += " table:default-cell-style-name=\"Default\"/>";
        c += run;
    }
    if (r2 < 0) {
        x += "<table:table-row><table:table-cell/></table:table-row>";
        return x + "</table:table>";
    }
    auto it = sh.cells.begin();
    for (int r = 0; r <= r2; ++r) {
        auto h = sh.rowHeights.find(r);
        const bool hidden = sh.hiddenRows.count(r) > 0;
        const bool hasCells = it != sh.cells.end() && Sheet::rowOf(it->first) == r;
        bool inMerge = false;
        for (const auto& m : sh.merges) inMerge = inMerge || (r >= m.r1 && r <= m.r2);
        const std::string rowStyle = styles.rowStyle(h == sh.rowHeights.end() ? 15 : h->second);
        if (!hasCells && !inMerge && h == sh.rowHeights.end() && !hidden) {
            // a run of empty default rows
            int run = 1;
            while (r + run <= r2) {
                const int rr = r + run;
                bool m2 = false;
                for (const auto& m : sh.merges) m2 = m2 || (rr >= m.r1 && rr <= m.r2);
                if ((it != sh.cells.end() && Sheet::rowOf(it->first) == rr) || m2 || sh.rowHeights.count(rr) || sh.hiddenRows.count(rr)) break;
                ++run;
            }
            x += "<table:table-row table:style-name=\"" + rowStyle + "\"" + (run > 1 ? " table:number-rows-repeated=\"" + std::to_string(run) + "\"" : "") +
                 "><table:table-cell table:number-columns-repeated=\"" + std::to_string(ncols) + "\"/></table:table-row>";
            r += run - 1;
            continue;
        }
        x += "<table:table-row table:style-name=\"" + rowStyle + "\"" + (hidden ? " table:visibility=\"collapse\"" : "") + ">";
        int col = 0;
        auto emitEmpty = [&](int upto) {
            if (upto > col) {
                x += upto - col > 1 ? "<table:table-cell table:number-columns-repeated=\"" + std::to_string(upto - col) + "\"/>" : "<table:table-cell/>";
                col = upto;
            }
        };
        // cells of this row in order, with covered cells of merges where they fall
        std::map<int, const Cell*> rowCells;
        for (; it != sh.cells.end() && Sheet::rowOf(it->first) == r; ++it) {
            if (!it->second.blank()) rowCells[Sheet::colOf(it->first)] = &it->second;
        }
        int lastCol = rowCells.empty() ? -1 : rowCells.rbegin()->first;
        for (const auto& m : sh.merges) {
            if (r >= m.r1 && r <= m.r2) lastCol = std::max(lastCol, m.c2);
        }
        for (int c = 0; c <= lastCol; ++c) {
            const Range* m = sh.mergeAt(r, c);
            const bool anchor = m && m->r1 == r && m->c1 == c;
            auto rc = rowCells.find(c);
            if (m && !anchor) {
                emitEmpty(c);
                x += "<table:covered-table-cell/>";
                col = c + 1;
                continue;
            }
            if (rc == rowCells.end() && !anchor) continue;
            emitEmpty(c);
            static const Cell kEmpty;
            const Cell& cell = rc == rowCells.end() ? kEmpty : *rc->second;
            x += cellXml(cell, styles.cellStyleFor(cell), anchor ? m : nullptr, styles);
            col = c + 1;
        }
        emitEmpty(ncols);
        x += "</table:table-row>";
    }
    return x + "</table:table>";
}

std::string odsAddress(const std::string& sheet, const Range& r) {
    std::string a = "$" + odfSheet(sheet) + ".$" + colName(r.c1) + "$" + std::to_string(r.r1 + 1);
    if (r.r2 != r.r1 || r.c2 != r.c1) a += ":.$" + colName(r.c2) + "$" + std::to_string(r.r2 + 1);
    return a;
}

std::string settingsXml(const Workbook& wb) {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-settings" + std::string(kOdsNs) + "><office:settings>";
    x += "<config:config-item-set config:name=\"ooo:view-settings\"><config:config-item-map-indexed config:name=\"Views\"><config:config-item-map-entry>";
    x += "<config:config-item config:name=\"ViewId\" config:type=\"string\">view1</config:config-item>";
    x += "<config:config-item-map-named config:name=\"Tables\">";
    auto item = [](const char* name, const char* type, int v) {
        return std::string("<config:config-item config:name=\"") + name + "\" config:type=\"" + type + "\">" + std::to_string(v) + "</config:config-item>";
    };
    for (const auto& sh : wb.sheets) {
        x += "<config:config-item-map-entry config:name=\"" + attrEsc(sh.name) + "\">";
        x += item("HorizontalSplitMode", "short", sh.freezeCols > 0 ? 2 : 0);
        x += item("VerticalSplitMode", "short", sh.freezeRows > 0 ? 2 : 0);
        x += item("HorizontalSplitPosition", "int", sh.freezeCols);
        x += item("VerticalSplitPosition", "int", sh.freezeRows);
        x += item("ActiveSplitRange", "short", sh.freezeRows > 0 ? 2 : 3);
        x += item("PositionLeft", "int", 0) + item("PositionRight", "int", sh.freezeCols);
        x += item("PositionTop", "int", 0) + item("PositionBottom", "int", sh.freezeRows);
        x += "</config:config-item-map-entry>";
    }
    x += "</config:config-item-map-named>";
    const int active = std::clamp(wb.active, 0, std::max(0, static_cast<int>(wb.sheets.size()) - 1));
    if (!wb.sheets.empty()) x += "<config:config-item config:name=\"ActiveTable\" config:type=\"string\">" + attrEsc(wb.sheets[static_cast<std::size_t>(active)].name) + "</config:config-item>";
    x += "</config:config-item-map-entry></config:config-item-map-indexed></config:config-item-set></office:settings></office:document-settings>";
    return x;
}

}  // namespace

std::string writeOds(const Workbook& wb) {
    OdsStyles styles;
    std::string tables;
    for (const auto& sh : wb.sheets) tables += tableXml(sh, styles);
    std::string extra;
    if (!wb.names.empty()) {
        extra += "<table:named-expressions>";
        const std::string base = wb.sheets.empty() ? std::string("Sheet1") : wb.sheets[0].name;
        for (const auto& [key, n] : wb.names) {
            extra += "<table:named-expression table:name=\"" + attrEsc(n.name) + "\" table:base-cell-address=\"$" + attrEsc(odfSheet(base)) +
                     ".$A$1\" table:expression=\"" + attrEsc(toOdf(n.formula)) + "\"/>";
        }
        extra += "</table:named-expressions>";
    }
    bool filters = false;
    for (const auto& sh : wb.sheets) filters = filters || sh.hasAutoFilter;
    if (filters) {
        extra += "<table:database-ranges>";
        int k = 0;
        for (const auto& sh : wb.sheets) {
            if (!sh.hasAutoFilter) continue;
            extra += "<table:database-range table:name=\"__Anonymous_Sheet_DB__" + std::to_string(k++) + "\" table:target-range-address=\"" +
                     attrEsc(odsAddress(sh.name, sh.autoFilter)) + "\" table:display-filter-buttons=\"true\"/>";
        }
        extra += "</table:database-ranges>";
    }
    std::string content = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content" + std::string(kOdsNs) + ">";
    content += "<office:automatic-styles>" + styles.xml + "</office:automatic-styles>";
    content += "<office:body><office:spreadsheet>" + tables + extra + "</office:spreadsheet></office:body></office:document-content>";
    const std::string stylesXml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles" + std::string(kOdsNs) +
                                  "><office:styles><style:default-style style:family=\"table-cell\"><style:text-properties fo:font-size=\"11pt\"/></style:default-style>"
                                  "<style:style style:name=\"Default\" style:family=\"table-cell\"/></office:styles></office:document-styles>";
    const std::string manifest =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
        "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.spreadsheet\"/>"
        "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
        "<manifest:file-entry manifest:full-path=\"settings.xml\" manifest:media-type=\"text/xml\"/>"
        "</manifest:manifest>";
    std::vector<raster::ZipEntry> entries;
    entries.push_back({"mimetype", "application/vnd.oasis.opendocument.spreadsheet"});
    entries.push_back({"content.xml", content});
    entries.push_back({"styles.xml", stylesXml});
    entries.push_back({"settings.xml", settingsXml(wb)});
    entries.push_back({"META-INF/manifest.xml", manifest});
    return raster::writeZip(entries);
}

// ---- reading --------------------------------------------------------------------------

namespace {

struct OdsCellStyle {
    std::string parent, dataStyle;
    CellStyle style;
    bool hasAlign = false;
};

void readCellStyleProps(const xml::Node& s, OdsCellStyle& out) {
    CellStyle& st = out.style;
    if (const auto* tp = s.child("text-properties")) {
        if (tp->attr("fo:font-weight") == "bold" || std::atoi(tp->attr("fo:font-weight").c_str()) >= 600) st.bold = true;
        if (tp->attr("fo:font-style") == "italic" || tp->attr("fo:font-style") == "oblique") st.italic = true;
        const std::string u = tp->attr("style:text-underline-style");
        if (!u.empty() && u != "none") st.underline = true;
        const std::string lt = tp->attr("style:text-line-through-style");
        if (!lt.empty() && lt != "none") st.strike = true;
        std::uint32_t c;
        if (tp->hasAttr("fo:color") && parseColor(tp->attr("fo:color"), c) && rgbOf(c) != 0) st.color = c;
        if (tp->hasAttr("fo:font-size")) {
            const double sz = odfLength(tp->attr("fo:font-size"));
            if (sz > 0 && std::fabs(sz - 11) > 0.01 && std::fabs(sz - 10) > 0.01) st.size = static_cast<float>(sz);
        }
    }
    if (const auto* cp = s.child("table-cell-properties")) {
        std::uint32_t c;
        if (cp->hasAttr("fo:background-color") && cp->attr("fo:background-color") != "transparent" && parseColor(cp->attr("fo:background-color"), c)) st.fill = c;
        static const char* sides[] = {"fo:border-left", "fo:border-right", "fo:border-top", "fo:border-bottom"};
        const std::string all = cp->attr("fo:border");
        for (int i = 0; i < 4; ++i) {
            std::uint32_t bc = 0;
            const std::string v = cp->hasAttr(sides[i]) ? cp->attr(sides[i]) : all;
            st.border[i] = static_cast<std::uint8_t>(borderFromOdf(v, bc));
            if (st.border[i] && hasColor(bc) && rgbOf(bc) != 0) st.borderColor = bc;
        }
        if (cp->attr("fo:wrap-option") == "wrap") st.wrap = true;
        const std::string va = cp->attr("style:vertical-align");
        if (va == "top") st.valign = 't';
        else if (va == "middle") st.valign = 'm';
    }
    if (const auto* pp = s.child("paragraph-properties")) {
        const std::string a = pp->attr("fo:text-align");
        if (a == "start" || a == "left") st.align = 'l';
        else if (a == "center" || a == "justify") st.align = 'c';
        else if (a == "end" || a == "right") st.align = 'r';
        if (pp->hasAttr("fo:margin-left")) st.indent = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(odfLength(pp->attr("fo:margin-left")) / 9)), 0, 15));
    }
}

struct OdsStyleSet {
    std::map<std::string, OdsCellStyle> cells;
    std::map<std::string, std::string> formats;  // data style name -> format code
    std::map<std::string, double> colWidthPt, rowHeightPt;

    void read(const xml::Node* container) {
        if (!container) return;
        for (const auto& s : container->children) {
            if (s.isText()) continue;
            const std::string name = s.attr("style:name");
            if (s.is("style")) {
                const std::string family = s.attr("style:family");
                if (family == "table-cell") {
                    OdsCellStyle cs;
                    cs.parent = s.attr("style:parent-style-name");
                    cs.dataStyle = s.attr("style:data-style-name");
                    readCellStyleProps(s, cs);
                    cells[name] = cs;
                } else if (family == "table-column") {
                    if (const auto* cp = s.child("table-column-properties")) colWidthPt[name] = odfLength(cp->attr("style:column-width"));
                } else if (family == "table-row") {
                    if (const auto* rp = s.child("table-row-properties")) {
                        if (rp->attr("style:use-optimal-row-height") == "false" && rp->hasAttr("style:row-height")) rowHeightPt[name] = odfLength(rp->attr("style:row-height"));
                    }
                }
            } else if (s.is("number-style") || s.is("percentage-style") || s.is("currency-style") || s.is("date-style") || s.is("time-style") ||
                       s.is("text-style")) {
                formats[name] = formatOf(s);
            }
        }
    }

    // The style and format of a named cell style, parents included.
    void resolve(const std::string& name, CellStyle& style, std::string& format, int depth = 0) const {
        auto it = cells.find(name);
        if (it == cells.end() || depth > 8) return;
        if (!it->second.parent.empty()) resolve(it->second.parent, style, format, depth + 1);
        const CellStyle& s = it->second.style;
        CellStyle base;
        // a child's set properties override its parent's
        if (s.bold) style.bold = true;
        if (s.italic) style.italic = true;
        if (s.underline) style.underline = true;
        if (s.strike) style.strike = true;
        if (s.wrap) style.wrap = true;
        if (s.align) style.align = s.align;
        if (s.valign) style.valign = s.valign;
        if (s.indent) style.indent = s.indent;
        if (s.size > 0) style.size = s.size;
        if (hasColor(s.color)) style.color = s.color;
        if (hasColor(s.fill)) style.fill = s.fill;
        for (int i = 0; i < 4; ++i) {
            if (s.border[i]) style.border[i] = s.border[i];
        }
        if (hasColor(s.borderColor)) style.borderColor = s.borderColor;
        (void)base;
        if (!it->second.dataStyle.empty()) {
            auto f = formats.find(it->second.dataStyle);
            if (f != formats.end()) format = f->second;
        }
    }
};

Cell odsCell(const xml::Node& c, const OdsStyleSet& styles, const std::string& columnDefault) {
    Cell cell;
    const std::string type = c.attr("office:value-type");
    if (type == "float" || type == "percentage" || type == "currency") {
        cell.value = CellValue::num(std::atof(c.attr("office:value").c_str()));
    } else if (type == "date") {
        const std::string d = c.attr("office:date-value");
        int y, m, dd;
        if (parseIsoDate(d.substr(0, 10), y, m, dd)) {
            double s = dateSerial(y, m, dd);
            if (d.size() >= 19) {
                s += (std::atoi(d.substr(11, 2).c_str()) * 3600 + std::atoi(d.substr(14, 2).c_str()) * 60 + std::atoi(d.substr(17, 2).c_str())) / 86400.0;
            }
            cell.value = CellValue::num(s);
            cell.format = d.size() > 10 ? "yyyy-mm-dd hh:mm" : "yyyy-mm-dd";
        }
    } else if (type == "boolean") {
        cell.value = CellValue::boolean(c.attr("office:boolean-value") == "true");
    } else if (type == "time") {
        const std::string t = c.attr("office:time-value");  // PT12H30M00S
        double h = 0, mi = 0, s = 0;
        std::sscanf(t.c_str(), "PT%lfH%lfM%lfS", &h, &mi, &s);
        cell.value = CellValue::num((h * 3600 + mi * 60 + s) / 86400.0);
        cell.format = "hh:mm:ss";
    } else {
        std::string text;
        bool first = true;
        for (const auto* p : c.all("p")) {
            text += (first ? "" : "\n") + odfText(*p);
            first = false;
        }
        if (!text.empty() || type == "string") cell.value = CellValue::str(text);
    }
    if (c.hasAttr("table:formula")) cell.formula = fileFormula(odfFormula(c.attr("table:formula")), false);
    std::string styleName = c.attr("table:style-name");
    if (styleName.empty()) styleName = columnDefault;
    std::string format;
    styles.resolve(styleName, cell.style, format);
    if (!format.empty()) cell.format = format;
    else if (type == "percentage") cell.format = "0%";
    else if (type == "currency") cell.format = "#,##0.00";
    return cell;
}

struct OdsSheetReader {
    Sheet& sh;
    const OdsStyleSet& styles;
    int row = 0, colCursor = 0;
    std::vector<std::string> colDefaults;
    std::vector<std::pair<int, int>> matrices;  // anchors whose spill area holds stored values
    std::vector<Range> matrixAreas;

    void column(const xml::Node& child) {
        const int rep = std::max(1, std::atoi(child.attr("table:number-columns-repeated").c_str()));
        auto w = styles.colWidthPt.find(child.attr("table:style-name"));
        const bool hidden = child.attr("table:visibility") == "collapse" || child.attr("table:visibility") == "filter";
        const std::string def = child.attr("table:default-cell-style-name");
        for (int k = 0; k < rep && colCursor < 1024; ++k, ++colCursor) {
            if (w != styles.colWidthPt.end() && w->second > 0) {
                const double chars = std::round((w->second / 0.75 - 5) / 7 * 100) / 100;
                if (std::fabs(chars - 8.43) > 0.02) sh.colWidths[colCursor] = chars;
            }
            if (hidden) sh.hiddenCols.insert(colCursor);
            colDefaults.push_back(def == "Default" ? std::string() : def);
        }
        if (rep > 1024) colCursor += rep;
    }

    void tableRow(const xml::Node& child) {
        const int rep = std::max(1, std::atoi(child.attr("table:number-rows-repeated").c_str()));
        bool hasContent = false;
        for (const auto& c : child.children) {
            if (c.is("table-cell") && (c.hasAttr("office:value-type") || c.hasAttr("table:formula") || !c.children.empty() ||
                                       c.hasAttr("table:number-columns-spanned") || c.hasAttr("table:style-name"))) {
                hasContent = true;
            }
        }
        const bool hidden = child.attr("table:visibility") == "collapse" || child.attr("table:visibility") == "filter";
        auto h = styles.rowHeightPt.find(child.attr("table:style-name"));
        if (!hasContent && rep > 64) {
            row += rep;  // the "rest of the sheet" filler
            return;
        }
        for (int k = 0; k < rep && row < kMaxRows; ++k, ++row) {
            if (h != styles.rowHeightPt.end() && std::fabs(h->second - 15) > 0.05 && std::fabs(h->second - 12.8) > 0.05) sh.rowHeights[row] = std::round(h->second * 100) / 100;
            if (hidden) sh.hiddenRows.insert(row);
            if (!hasContent) continue;
            int col = 0;
            for (const auto& c : child.children) {
                if (!c.is("table-cell") && !c.is("covered-table-cell")) continue;
                const int crep = std::max(1, std::atoi(c.attr("table:number-columns-repeated").c_str()));
                if (c.is("covered-table-cell")) {
                    col += crep;
                    continue;
                }
                const std::string def = col < static_cast<int>(colDefaults.size()) ? colDefaults[static_cast<std::size_t>(col)] : std::string();
                Cell cell = odsCell(c, styles, def);
                const int cs = std::atoi(c.attr("table:number-columns-spanned").c_str()), rs = std::atoi(c.attr("table:number-rows-spanned").c_str());
                if (k == 0 && (cs > 1 || rs > 1)) sh.merges.push_back(Range{0, row, col, row + std::max(1, rs) - 1, col + std::max(1, cs) - 1});
                const int mc = std::atoi(c.attr("table:number-matrix-columns-spanned").c_str()), mr = std::atoi(c.attr("table:number-matrix-rows-spanned").c_str());
                if (cell.hasFormula() && (mc > 1 || mr > 1)) matrixAreas.push_back(Range{0, row, col, row + std::max(1, mr) - 1, col + std::max(1, mc) - 1});
                if (!cell.blank()) {
                    for (int m = 0; m < crep && col + m < kMaxCols; ++m) {
                        if (crep > 256 && cell.value.empty() && !cell.hasFormula()) break;  // styled filler to the sheet edge
                        sh.cells[Sheet::key(row, col + m)] = cell;
                    }
                }
                col += crep;
            }
        }
    }

    void walk(const xml::Node& parent) {
        for (const auto& child : parent.children) {
            if (child.is("table-column")) column(child);
            else if (child.is("table-columns") || child.is("table-header-columns") || child.is("table-column-group")) walk(child);
            else if (child.is("table-row")) tableRow(child);
            else if (child.is("table-rows") || child.is("table-header-rows") || child.is("table-row-group")) walk(child);
        }
    }

    void finish() {
        for (const auto& a : matrixAreas) {
            auto it = sh.cells.lower_bound(Sheet::key(a.r1, a.c1));
            const auto end = sh.cells.upper_bound(Sheet::key(a.r2, a.c2));
            for (; it != end; ++it) {
                const int r = Sheet::rowOf(it->first), c = Sheet::colOf(it->first);
                if (!a.contains(r, c) || (r == a.r1 && c == a.c1)) continue;
                if (!it->second.hasFormula()) it->second.spilled = true;
            }
        }
    }
};

// "$Sheet1.$A$1:.$D$6" -> range
bool odsRange(const Workbook& wb, const std::string& address, Range& out) {
    std::string err;
    return parseRange(wb, odfFormula("[" + address + "]"), 0, out, err);
}

void readSettings(const std::string& text, Workbook& wb) {
    if (text.empty()) return;
    xml::Node root = xml::parse(text);
    std::vector<const xml::Node*> maps;
    root.descendants("config-item-map-named", maps);
    for (const auto* m : maps) {
        if (m->attr("config:name") != "Tables") continue;
        for (const auto* e : m->all("config-item-map-entry")) {
            const int s = wb.sheetIndex(e->attr("config:name"));
            if (s < 0) continue;
            std::map<std::string, int> items;
            for (const auto* it : e->all("config-item")) items[it->attr("config:name")] = std::atoi(it->allText().c_str());
            Sheet& sh = wb.sheets[static_cast<std::size_t>(s)];
            if (items["HorizontalSplitMode"] == 2) sh.freezeCols = std::max(0, items["HorizontalSplitPosition"]);
            if (items["VerticalSplitMode"] == 2) sh.freezeRows = std::max(0, items["VerticalSplitPosition"]);
        }
    }
}

}  // namespace

Workbook readOds(const std::string& bytes) {
    xlsx::Files files;
    for (auto& e : raster::readZip(bytes)) files[e.name] = std::move(e.data);
    const std::string content = xlsx::zipEntry(files, "content.xml");
    if (content.empty()) throw std::runtime_error("not an OpenDocument file (no content.xml)");
    xml::Node root = xml::parse(content);
    OdsStyleSet styles;
    xml::Node stylesRoot;
    if (std::string st = xlsx::zipEntry(files, "styles.xml"); !st.empty()) {
        stylesRoot = xml::parse(st);
        styles.read(stylesRoot.child("styles"));
        styles.read(stylesRoot.child("automatic-styles"));
    }
    styles.read(root.child("automatic-styles"));
    const xml::Node* body = root.child("body");
    const xml::Node* ss = body ? body->child("spreadsheet") : nullptr;
    if (!ss) throw std::runtime_error("not a spreadsheet (no office:spreadsheet)");
    Workbook wb;
    for (const auto* table : ss->all("table")) {
        Sheet sh;
        sh.name = table->attr("table:name");
        if (sh.name.empty()) sh.name = "Sheet" + std::to_string(wb.sheets.size() + 1);
        OdsSheetReader reader{sh, styles};
        reader.walk(*table);
        reader.finish();
        wb.sheets.push_back(std::move(sh));
    }
    if (wb.sheets.empty()) throw std::runtime_error("spreadsheet has no tables");
    if (const auto* ne = ss->child("named-expressions")) {
        for (const auto& n : ne->children) {
            if (n.isText()) continue;
            DefinedName d;
            d.name = n.attr("table:name");
            if (!validName(d.name)) continue;
            if (n.is("named-range")) {
                Range r;
                if (!odsRange(wb, n.attr("table:cell-range-address"), r)) continue;
                d.formula = quoteSheet(wb.sheets[static_cast<std::size_t>(r.sheet)].name) + "!$" + colName(r.c1) + "$" + std::to_string(r.r1 + 1);
                if (r.r2 != r.r1 || r.c2 != r.c1) d.formula += ":$" + colName(r.c2) + "$" + std::to_string(r.r2 + 1);
            } else if (n.is("named-expression")) {
                d.formula = odfFormula(n.attr("table:expression"));
            } else {
                continue;
            }
            wb.names[upper(d.name)] = d;
        }
    }
    if (const auto* dbs = ss->child("database-ranges")) {
        for (const auto* db : dbs->all("database-range")) {
            Range r;
            if (db->attr("table:display-filter-buttons") != "true" || !odsRange(wb, db->attr("table:target-range-address"), r)) continue;
            Sheet& sh = wb.sheets[static_cast<std::size_t>(r.sheet)];
            sh.hasAutoFilter = true;
            sh.autoFilter = r;
            sh.autoFilter.sheet = 0;
        }
    }
    readSettings(xlsx::zipEntry(files, "settings.xml"), wb);
    return wb;
}

}  // namespace wetwaresuite::office
