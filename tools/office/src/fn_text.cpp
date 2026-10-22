// Formula functions: logic (IF, IFS, SWITCH, AND, OR, LET, ...), information
// (ISNUMBER, TYPE, ...) and text (LEFT, FIND, SUBSTITUTE, TEXTJOIN, TEXTSPLIT, ...).
#include <algorithm>
#include <cmath>

#include "formula_internal.hpp"
#include "wetwaresuite/pdf/fonts.hpp"
#include "text.hpp"

namespace wetwaresuite::office::fx {

namespace {

bool isErrorArg(const NodeP& n, Ctx& c, CellValue& v) {
    v = evalScalar(n, c);
    return v.isError();
}

// IF that works per element when its condition is an array (=IF(A1:A5>2,"big","small")).
Arg ifFn(const std::vector<NodeP>& a, Ctx& c) {
    need(a, 1, 3);
    Arg cond = evalArg(a[0], c);
    if (!cond.multi()) {
        bool b = toBool(scalar(cond, c));
        if (b) return missing(a, 1) ? (a.size() > 1 ? num(0) : boolean(true)) : evalArg(a[1], c);
        if (a.size() < 3) return boolean(false);
        return missing(a, 2) ? num(0) : evalArg(a[2], c);
    }
    Matrix cm = toMatrix(cond, c);
    Matrix tm = missing(a, 1) ? Matrix(1, 1) : toMatrix(evalArg(a[1], c), c);
    Matrix fm(1, 1);
    if (a.size() < 3) fm.v[0] = CellValue::boolean(false);
    else if (!missing(a, 2)) fm = toMatrix(evalArg(a[2], c), c);
    else fm.v[0] = CellValue::num(0);
    if (missing(a, 1)) tm.v[0] = CellValue::num(0);
    const int rows = std::max({cm.rows, tm.rows, fm.rows}), cols = std::max({cm.cols, tm.cols, fm.cols});
    Matrix out(rows, cols);
    auto pick = [](const Matrix& m, int i, int j, bool& ok) -> const CellValue& {
        const int r = m.rows == 1 ? 0 : i, k = m.cols == 1 ? 0 : j;
        ok = r < m.rows && k < m.cols;
        return ok ? m.at(r, k) : kEmpty;
    };
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            bool ok;
            const CellValue& cv = pick(cm, i, j, ok);
            if (!ok) {
                out.at(i, j) = CellValue::error("#N/A");
                continue;
            }
            try {
                const bool b = toBool(cv);
                const CellValue& v = pick(b ? tm : fm, i, j, ok);
                out.at(i, j) = ok ? v : CellValue::error("#N/A");
            } catch (const EvalError& e) {
                out.at(i, j) = CellValue::error(e.code);
            }
        }
    }
    return Arg::array(std::move(out));
}

void addLogic(FnMap& m) {
    m["IF"] = ifFn;
    m["IFS"] = [](const auto& a, Ctx& c) {
        if (a.size() < 2 || a.size() % 2) throw EvalError{"#VALUE!"};
        for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
            if (evalBool(a[i], c)) return evalArg(a[i + 1], c);
        }
        throw EvalError{"#N/A"};
    };
    m["SWITCH"] = [](const auto& a, Ctx& c) {
        if (a.size() < 3) throw EvalError{"#VALUE!"};
        const CellValue v = evalScalar(a[0], c);
        if (v.isError()) throw EvalError{v.text};
        std::size_t i = 1;
        for (; i + 1 < a.size(); i += 2) {
            const CellValue w = evalScalar(a[i], c);
            if (w.type == v.type && compareValues(v, w) == 0) return evalArg(a[i + 1], c);
        }
        if (i < a.size()) return evalArg(a[i], c);  // the default
        throw EvalError{"#N/A"};
    };
    m["IFERROR"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        CellValue v;
        if (isErrorArg(a[0], c, v)) return evalArg(a[1], c);
        return Arg::val(v);
    };
    m["IFNA"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        CellValue v;
        if (isErrorArg(a[0], c, v) && v.text == "#N/A") return evalArg(a[1], c);
        return Arg::val(v);
    };
    auto logical = [](const std::vector<NodeP>& a, Ctx& c, int kind) {  // 0 and, 1 or, 2 xor
        if (a.empty()) throw EvalError{"#VALUE!"};
        bool acc = kind == 0, any = false;
        int trues = 0;
        for (const auto& x : a) {
            if (x->kind == FormulaNode::Missing) continue;
            Arg v = evalArg(x, c);
            auto take = [&](bool b) {
                acc = kind == 0 ? (acc && b) : (acc || b);
                trues += b ? 1 : 0;
                any = true;
            };
            eachValue(v, c, [&](const CellValue& cv, bool fromRef) {
                if (cv.isError()) throw EvalError{cv.text};
                if (cv.type == CellValue::Number || cv.type == CellValue::Bool) take(cv.number != 0);
                else if (!fromRef) take(toBool(cv));
            });
        }
        if (!any) throw EvalError{"#VALUE!"};
        return boolean(kind == 2 ? trues % 2 == 1 : acc);
    };
    m["AND"] = [logical](const auto& a, Ctx& c) { return logical(a, c, 0); };
    m["OR"] = [logical](const auto& a, Ctx& c) { return logical(a, c, 1); };
    m["XOR"] = [logical](const auto& a, Ctx& c) { return logical(a, c, 2); };
    m["NOT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return boolean(!evalBool(a[0], c));
    };
    m["TRUE"] = [](const auto& a, Ctx&) {
        need(a, 0, 0);
        return boolean(true);
    };
    m["FALSE"] = [](const auto& a, Ctx&) {
        need(a, 0, 0);
        return boolean(false);
    };
    m["LET"] = [](const auto& a, Ctx& c) {
        if (a.size() < 3 || a.size() % 2 == 0) throw EvalError{"#VALUE!"};
        std::vector<std::pair<std::string, Arg>> locals = c.locals ? *c.locals : std::vector<std::pair<std::string, Arg>>{};
        Ctx inner = c;
        inner.locals = &locals;
        for (std::size_t i = 0; i + 1 < a.size(); i += 2) {
            if (a[i]->kind != FormulaNode::Name) throw EvalError{"#NAME?"};
            Arg v = evalArg(a[i + 1], inner);
            locals.emplace_back(upper(a[i]->str), v);
        }
        return evalArg(a.back(), inner);
    };
    m["NA"] = [](const auto& a, Ctx&) -> Arg {
        need(a, 0, 0);
        throw EvalError{"#N/A"};
    };
}

void addInfo(FnMap& m) {
    auto is = [](auto test) {
        return [test](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 1);
            CellValue v;
            try {
                v = evalScalar(a[0], c);
            } catch (const EvalError& e) {
                v = CellValue::error(e.code);
            }
            return boolean(test(v));
        };
    };
    m["ISBLANK"] = is([](const CellValue& v) { return v.empty(); });
    m["ISNUMBER"] = is([](const CellValue& v) { return v.type == CellValue::Number; });
    m["ISTEXT"] = is([](const CellValue& v) { return v.type == CellValue::Text; });
    m["ISNONTEXT"] = is([](const CellValue& v) { return v.type != CellValue::Text; });
    m["ISLOGICAL"] = is([](const CellValue& v) { return v.type == CellValue::Bool; });
    m["ISERROR"] = is([](const CellValue& v) { return v.isError(); });
    m["ISERR"] = is([](const CellValue& v) { return v.isError() && v.text != "#N/A"; });
    m["ISNA"] = is([](const CellValue& v) { return v.isError() && v.text == "#N/A"; });
    m["ISEVEN"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return boolean(std::fmod(std::trunc(std::abs(evalNum(a[0], c))), 2) == 0);
    };
    m["ISODD"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return boolean(std::fmod(std::trunc(std::abs(evalNum(a[0], c))), 2) == 1);
    };
    m["ISREF"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return boolean(evalArg(a[0], c).isRange);
    };
    m["ISFORMULA"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Range r = rangeArg(a[0], c);
        const Cell* cell = cellAt(c, r.sheet, r.r1, r.c1);
        return boolean(cell && cell->hasFormula());
    };
    m["FORMULATEXT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Range r = rangeArg(a[0], c);
        const Cell* cell = cellAt(c, r.sheet, r.r1, r.c1);
        if (!cell || !cell->hasFormula()) throw EvalError{"#N/A"};
        return str("=" + cell->formula);
    };
    m["N"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        CellValue v = evalScalar(a[0], c);
        if (v.isError()) throw EvalError{v.text};
        return num(v.type == CellValue::Number || v.type == CellValue::Bool ? v.number : 0);
    };
    m["T"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        CellValue v = evalScalar(a[0], c);
        if (v.isError()) throw EvalError{v.text};
        return str(v.type == CellValue::Text ? v.text : std::string());
    };
    m["TYPE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Arg v = evalArg(a[0], c);
        if (v.multi()) return num(64);
        CellValue s = scalar(v, c);
        switch (s.type) {
            case CellValue::Text: return num(2);
            case CellValue::Bool: return num(4);
            case CellValue::Error: return num(16);
            default: return num(1);
        }
    };
    m["ERROR.TYPE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        CellValue v;
        try {
            v = evalScalar(a[0], c);
        } catch (const EvalError& e) {
            v = CellValue::error(e.code);
        }
        if (!v.isError()) throw EvalError{"#N/A"};
        static const char* codes[] = {"#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A", "#GETTING_DATA"};
        for (int i = 0; i < 8; ++i) {
            if (v.text == codes[i]) return num(i + 1);
        }
        return num(v.text == "#SPILL!" ? 9 : v.text == "#CALC!" ? 14 : 7);
    };
    m["SHEET"] = [](const auto& a, Ctx& c) {
        need(a, 0, 1);
        if (missing(a, 0)) return num(c.sheet + 1);
        Arg v = evalArg(a[0], c);
        if (v.isRange) return num(v.r.sheet + 1);
        const int s = c.wb.sheetIndex(toStr(scalar(v, c)));
        if (s < 0) throw EvalError{"#N/A"};
        return num(s + 1);
    };
    m["SHEETS"] = [](const auto& a, Ctx& c) {
        need(a, 0, 1);
        return num(static_cast<double>(missing(a, 0) ? c.wb.sheets.size() : 1));
    };
}

// ---- text --------------------------------------------------------------------

std::vector<unsigned> cps(const std::string& s) { return pdf::utf8Decode(s); }
std::string join(const std::vector<unsigned>& v, std::size_t from = 0, std::size_t n = std::string::npos) {
    std::string out;
    for (std::size_t i = from; i < v.size() && (n == std::string::npos || i < from + n); ++i) out += pdf::utf8Encode(v[i]);
    return out;
}

// Position (0-based, in code points) of `needle` in `hay` from `start`, or npos.
std::size_t findCp(const std::vector<unsigned>& hay, const std::vector<unsigned>& needle, std::size_t start, bool caseless, bool wild) {
    auto low = [&](unsigned c) { return caseless && c < 128 ? static_cast<unsigned>(std::tolower(static_cast<int>(c))) : c; };
    if (wild) {
        // SEARCH wildcards: try every start and match the pattern against the rest
        const std::string pat = join(needle);
        for (std::size_t i = start; i <= hay.size(); ++i) {
            for (std::size_t j = hay.size(); j >= i; --j) {
                if (wildcardMatch(pat, join(hay, i, j - i))) return i;
                if (j == 0) break;
            }
        }
        return std::string::npos;
    }
    if (needle.empty()) return start <= hay.size() ? start : std::string::npos;
    for (std::size_t i = start; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t k = 0; k < needle.size() && ok; ++k) ok = low(hay[i + k]) == low(needle[k]);
        if (ok) return i;
    }
    return std::string::npos;
}

Fn finder(bool search) {
    return [search](const std::vector<NodeP>& a, Ctx& c) {
        need(a, 2, 3);
        const auto needle = cps(evalStr(a[0], c)), hay = cps(evalStr(a[1], c));
        const double start = missing(a, 2) ? 1 : evalNum(a[2], c);
        if (start < 1 || start > static_cast<double>(hay.size()) + 1) throw EvalError{"#VALUE!"};
        const bool wild = search && join(needle).find_first_of("*?") != std::string::npos;
        std::size_t p = findCp(hay, needle, static_cast<std::size_t>(start) - 1, search, wild);
        if (p == std::string::npos) throw EvalError{"#VALUE!"};
        return num(static_cast<double>(p + 1));
    };
}

std::string properCase(const std::string& s) {
    std::string out;
    bool startWord = true;
    for (char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalpha(u)) {
            out += static_cast<char>(startWord ? std::toupper(u) : std::tolower(u));
            startWord = false;
        } else {
            out += ch;
            startWord = !(u & 0x80) && !std::isdigit(u) && ch != '\'';
            if (u & 0x80) startWord = false;
        }
    }
    return out;
}

// Occurrence n (1-based; negative counts from the end) of a delimiter; returns its start and length.
bool nthDelimiter(const std::string& text, const std::vector<std::string>& delims, int n, bool caseless, std::size_t& pos, std::size_t& len) {
    std::vector<std::pair<std::size_t, std::size_t>> hits;
    const std::string hay = caseless ? lower(text) : text;
    for (std::size_t i = 0; i < hay.size();) {
        bool found = false;
        for (const auto& d0 : delims) {
            const std::string d = caseless ? lower(d0) : d0;
            if (!d.empty() && hay.compare(i, d.size(), d) == 0) {
                hits.emplace_back(i, d.size());
                i += d.size();
                found = true;
                break;
            }
        }
        if (!found) ++i;
    }
    if (n == 0 || hits.empty()) return false;
    const long idx = n > 0 ? n - 1 : static_cast<long>(hits.size()) + n;
    if (idx < 0 || idx >= static_cast<long>(hits.size())) return false;
    pos = hits[static_cast<std::size_t>(idx)].first;
    len = hits[static_cast<std::size_t>(idx)].second;
    return true;
}

std::vector<std::string> textList(const NodeP& n, Ctx& c) {
    std::vector<std::string> out;
    Matrix mm = toMatrix(evalArg(n, c), c);
    for (const auto& v : mm.v) out.push_back(toStr(v));
    return out;
}

Fn beforeAfter(bool before) {
    return [before](const std::vector<NodeP>& a, Ctx& c) {
        need(a, 2, 6);
        const std::string text = evalStr(a[0], c);
        std::vector<std::string> delims = textList(a[1], c);
        const int n = missing(a, 2) ? 1 : evalInt(a[2], c);
        const bool caseless = !missing(a, 3) && evalNum(a[3], c) != 0;
        const bool matchEnd = !missing(a, 4) && evalNum(a[4], c) != 0;
        std::size_t pos, len;
        if (!nthDelimiter(text, delims, n, caseless, pos, len)) {
            if (matchEnd) {
                if (n > 0) return str(before ? text : std::string());
                return str(before ? std::string() : text);
            }
            if (!missing(a, 5)) return evalArg(a[5], c);
            throw EvalError{"#N/A"};
        }
        return str(before ? text.substr(0, pos) : text.substr(pos + len));
    };
}

void addText(FnMap& m) {
    m["LEN"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return num(static_cast<double>(utf8Len(evalStr(a[0], c))));
    };
    m["LEFT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        double n = missing(a, 1) ? 1 : evalNum(a[1], c);
        if (n < 0) throw EvalError{"#VALUE!"};
        return str(utf8Substr(evalStr(a[0], c), 0, static_cast<std::size_t>(n)));
    };
    m["RIGHT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        double n = missing(a, 1) ? 1 : evalNum(a[1], c);
        if (n < 0) throw EvalError{"#VALUE!"};
        std::string s = evalStr(a[0], c);
        std::size_t len = utf8Len(s), k = static_cast<std::size_t>(n);
        return str(utf8Substr(s, k >= len ? 0 : len - k, k));
    };
    m["MID"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        std::string s = evalStr(a[0], c);
        double start = evalNum(a[1], c), n = evalNum(a[2], c);
        if (start < 1 || n < 0) throw EvalError{"#VALUE!"};
        return str(utf8Substr(s, static_cast<std::size_t>(start) - 1, static_cast<std::size_t>(n)));
    };
    m["UPPER"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return str(upper(evalStr(a[0], c)));
    };
    m["LOWER"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return str(lower(evalStr(a[0], c)));
    };
    m["PROPER"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return str(properCase(evalStr(a[0], c)));
    };
    m["TRIM"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        std::string s = evalStr(a[0], c), out;
        for (char ch : s) {
            if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
            out += ch;
        }
        if (!out.empty() && out.back() == ' ') out.pop_back();
        return str(out);
    };
    m["CLEAN"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        std::string s = evalStr(a[0], c), out;
        for (char ch : s) {
            if (static_cast<unsigned char>(ch) >= 32) out += ch;
        }
        return str(out);
    };
    m["REPT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const std::string s = evalStr(a[0], c);
        const double n = std::trunc(evalNum(a[1], c));
        if (n < 0 || s.size() * n > 32767) throw EvalError{"#VALUE!"};
        std::string out;
        for (int i = 0; i < static_cast<int>(n); ++i) out += s;
        return str(out);
    };
    m["EXACT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        return boolean(evalStr(a[0], c) == evalStr(a[1], c));
    };
    m["FIND"] = finder(false);
    m["SEARCH"] = finder(true);
    m["REPLACE"] = [](const auto& a, Ctx& c) {
        need(a, 4, 4);
        auto s = cps(evalStr(a[0], c));
        const double start = evalNum(a[1], c), n = evalNum(a[2], c);
        if (start < 1 || n < 0) throw EvalError{"#VALUE!"};
        const std::size_t from = std::min(s.size(), static_cast<std::size_t>(start) - 1);
        const std::size_t cnt = std::min(s.size() - from, static_cast<std::size_t>(n));
        return str(join(s, 0, from) + evalStr(a[3], c) + join(s, from + cnt));
    };
    m["SUBSTITUTE"] = [](const auto& a, Ctx& c) {
        need(a, 3, 4);
        std::string s = evalStr(a[0], c), from = evalStr(a[1], c), to = evalStr(a[2], c);
        if (from.empty()) return str(s);
        int which = missing(a, 3) ? 0 : evalInt(a[3], c);
        if (!missing(a, 3) && which < 1) throw EvalError{"#VALUE!"};
        std::string out;
        std::size_t pos = 0;
        int n = 0;
        while (true) {
            std::size_t f = s.find(from, pos);
            if (f == std::string::npos) break;
            ++n;
            out += s.substr(pos, f - pos);
            out += (which == 0 || which == n) ? to : from;
            pos = f + from.size();
        }
        return str(out + s.substr(pos));
    };
    m["CHAR"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const int n = evalInt(a[0], c);
        if (n < 1 || n > 255) throw EvalError{"#VALUE!"};
        // Windows-1252 for the upper half, like Excel on Windows
        static const unsigned cp1252[32] = {0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D,  0x17D, 0x8F,
                                            0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
        const unsigned cp = n >= 128 && n < 160 ? cp1252[n - 128] : static_cast<unsigned>(n);
        return str(pdf::utf8Encode(cp));
    };
    m["UNICHAR"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const int n = evalInt(a[0], c);
        if (n < 1 || n > 0x10FFFF) throw EvalError{"#VALUE!"};
        return str(pdf::utf8Encode(static_cast<unsigned>(n)));
    };
    m["CODE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        auto v = cps(evalStr(a[0], c));
        if (v.empty()) throw EvalError{"#VALUE!"};
        return num(v[0] < 256 ? v[0] : 63);
    };
    m["UNICODE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        auto v = cps(evalStr(a[0], c));
        if (v.empty()) throw EvalError{"#VALUE!"};
        return num(v[0]);
    };
    m["TEXTBEFORE"] = beforeAfter(true);
    m["TEXTAFTER"] = beforeAfter(false);
}

std::string fixedText(double v, int decimals, bool commas) {
    std::string fmt = commas ? "#,##0" : "0";
    if (decimals > 0) fmt += "." + std::string(static_cast<std::size_t>(std::min(decimals, 30)), '0');
    double r = roundTo(v, decimals, 0);
    return formatValue(CellValue::num(r), fmt);
}

void addTextJoin(FnMap& m) {
    auto concatAll = [](const std::vector<NodeP>& a, Ctx& c, const std::vector<std::string>* delims, bool skipEmpty) {
        std::string out;
        std::size_t parts = 0;
        auto add = [&](const std::string& s) {
            if (skipEmpty && s.empty()) return;
            if (parts && delims && !delims->empty()) out += (*delims)[(parts - 1) % delims->size()];
            out += s;
            ++parts;
        };
        for (const auto& x : a) {
            if (x->kind == FormulaNode::Missing) {
                add("");
                continue;
            }
            Arg v = evalArg(x, c);
            if (v.isRange) {
                Range r = clipped(c, v.r);
                for (int row = r.r1; row <= r.r2; ++row) {
                    for (int col = r.c1; col <= r.c2; ++col) add(toStr(cellValue(c, r.sheet, row, col)));
                }
            } else if (v.arr) {
                for (const auto& e : v.arr->v) add(toStr(e));
            } else {
                add(toStr(v.v));
            }
        }
        if (out.size() > 32767) throw EvalError{"#VALUE!"};
        return str(out);
    };
    m["CONCAT"] = [concatAll](const auto& a, Ctx& c) { return concatAll(a, c, nullptr, false); };
    m["CONCATENATE"] = m["CONCAT"];
    m["TEXTJOIN"] = [concatAll](const auto& a, Ctx& c) {
        if (a.size() < 3) throw EvalError{"#VALUE!"};
        std::vector<std::string> d = textList(a[0], c);
        bool skip = evalBool(a[1], c);
        return concatAll(std::vector<NodeP>(a.begin() + 2, a.end()), c, &d, skip);
    };
    m["TEXT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        CellValue v = evalScalar(a[0], c);
        if (v.isError()) throw EvalError{v.text};
        if (v.type == CellValue::Text) {
            double d;
            if (parseNumber(v.text, d)) v = CellValue::num(d);
        }
        return str(formatValue(v, evalStr(a[1], c)));
    };
    m["VALUE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        CellValue v = evalScalar(a[0], c);
        if (v.type == CellValue::Text) {
            // times like 12:30 as fractions of a day
            int h, mi;
            char colon;
            if (std::sscanf(v.text.c_str(), "%d%c%d", &h, &colon, &mi) == 3 && colon == ':') return num((h * 60 + mi) / 1440.0);
        }
        return num(toNum(v));
    };
    m["NUMBERVALUE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 3);
        std::string s = evalStr(a[0], c);
        const std::string dec = missing(a, 1) ? "." : evalStr(a[1], c), grp = missing(a, 2) ? "," : evalStr(a[2], c);
        std::string clean;
        for (char ch : s) {
            if (ch == ' ') continue;
            if (!grp.empty() && ch == grp[0]) continue;
            if (!dec.empty() && ch == dec[0]) {
                clean += '.';
                continue;
            }
            clean += ch;
        }
        double d;
        if (!parseNumber(clean, d)) throw EvalError{"#VALUE!"};
        return num(d);
    };
    m["FIXED"] = [](const auto& a, Ctx& c) {
        need(a, 1, 3);
        const double v = evalNum(a[0], c);
        const int decimals = missing(a, 1) ? 2 : evalInt(a[1], c);
        const bool noCommas = !missing(a, 2) && evalBool(a[2], c);
        return str(fixedText(v, decimals, !noCommas));
    };
    m["DOLLAR"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double v = evalNum(a[0], c);
        const int decimals = missing(a, 1) ? 2 : evalInt(a[1], c);
        std::string t = fixedText(std::abs(v), decimals, true);
        return str(v < 0 ? "-$" + t : "$" + t);
    };
    m["TEXTSPLIT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 6);
        const std::string text = evalStr(a[0], c);
        std::vector<std::string> colDelims = missing(a, 1) ? std::vector<std::string>{} : textList(a[1], c);
        std::vector<std::string> rowDelims = missing(a, 2) ? std::vector<std::string>{} : textList(a[2], c);
        const bool skipEmpty = !missing(a, 3) && evalBool(a[3], c);
        const bool caseless = !missing(a, 4) && evalNum(a[4], c) != 0;
        CellValue pad = missing(a, 5) ? CellValue::error("#N/A") : evalScalar(a[5], c);
        auto split = [&](const std::string& s, const std::vector<std::string>& ds) {
            std::vector<std::string> parts;
            if (ds.empty()) return std::vector<std::string>{s};
            const std::string hay = caseless ? lower(s) : s;
            std::size_t start = 0;
            for (std::size_t i = 0; i < hay.size();) {
                bool hit = false;
                for (const auto& d0 : ds) {
                    const std::string d = caseless ? lower(d0) : d0;
                    if (!d.empty() && hay.compare(i, d.size(), d) == 0) {
                        parts.push_back(s.substr(start, i - start));
                        i += d.size();
                        start = i;
                        hit = true;
                        break;
                    }
                }
                if (!hit) ++i;
            }
            parts.push_back(s.substr(start));
            if (skipEmpty) parts.erase(std::remove(parts.begin(), parts.end(), std::string()), parts.end());
            return parts;
        };
        std::vector<std::vector<std::string>> rows;
        for (const auto& line : split(text, rowDelims)) rows.push_back(split(line, colDelims));
        std::size_t width = 1;
        for (const auto& r : rows) width = std::max(width, r.size());
        Matrix out(static_cast<int>(std::max<std::size_t>(rows.size(), 1)), static_cast<int>(width));
        for (std::size_t i = 0; i < rows.size(); ++i) {
            for (std::size_t j = 0; j < width; ++j) out.at(static_cast<int>(i), static_cast<int>(j)) = j < rows[i].size() ? CellValue::str(rows[i][j]) : pad;
        }
        return Arg::array(std::move(out));
    };
    m["HYPERLINK"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        return missing(a, 1) ? str(evalStr(a[0], c)) : Arg::val(evalScalar(a[1], c));
    };
    m["VALUETOTEXT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        CellValue v = evalScalar(a[0], c);
        const bool strict = !missing(a, 1) && evalNum(a[1], c) == 1;
        if (v.isError()) return str(v.text);
        std::string s = toStr(v);
        return str(strict && v.type == CellValue::Text ? "\"" + s + "\"" : s);
    };
}

}  // namespace

void addTextFunctions(FnMap& m) {
    addLogic(m);
    addInfo(m);
    addText(m);
    addTextJoin(m);
}

}  // namespace wetwaresuite::office::fx
