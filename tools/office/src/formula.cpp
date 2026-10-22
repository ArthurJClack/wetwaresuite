// Formula engine: tokenizer, precedence parser, evaluator with Excel's
// coercion rules, arrays (constants, element-wise operators, functions that
// run per element), defined names and LET, and dependency-ordered
// recalculation (cells are computed on demand, so dependencies always come
// first; a cell that is reached again while it is being computed is a cycle).
// Array results spill into the cells below and to the right.
// The function library lives in fn_*.cpp.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "formula_internal.hpp"
#include "wetwaresuite/office/sheet.hpp"
#include "wetwaresuite/pdf/fonts.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

using fx::NodeP;

namespace {

// ---- tokens --------------------------------------------------------------------

struct Tok {
    enum T { Num, Str, Bool, Err, Ref, Func, Name, Op, LP, RP, Sep, LB, RB, End };
    T t = End;
    std::string text;
    double num = 0;
    FormulaRef ref;
    std::size_t begin = 0, end = 0;
};

const char* kErrors[] = {"#DIV/0!", "#N/A", "#NAME?", "#NULL!", "#NUM!", "#REF!", "#VALUE!", "#CYCLE!", "#SPILL!", "#CALC!", "#GETTING_DATA"};

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '\\' || (c & 0x80); }

// One "A1" / "$A$1" / "A" / "1" piece. kind: 0 = cell, 1 = column only, 2 = row only.
bool refPart(const std::string& s, std::size_t& p, int& row, int& col, bool& absR, bool& absC, int& kind) {
    std::size_t q = p;
    absC = absR = false;
    row = col = -1;
    if (q < s.size() && s[q] == '$') {
        absC = true;
        ++q;
    }
    std::size_t ls = q;
    while (q < s.size() && std::isalpha(static_cast<unsigned char>(s[q])) && q - ls < 4) ++q;
    const std::size_t letters = q - ls;
    if (letters > 3) return false;
    if (letters) {
        std::string up;
        for (std::size_t i = ls; i < q; ++i) up += static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
        col = colIndex(up);
        if (col < 0 || col >= kMaxCols) return false;
    }
    std::size_t rs = q;
    if (q < s.size() && s[q] == '$') {
        absR = true;
        ++q;
    }
    std::size_t ds = q;
    while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q])) && q - ds < 8) ++q;
    if (q > ds) {
        long r = std::stol(s.substr(ds, q - ds));
        if (r < 1 || r > kMaxRows) return false;
        row = static_cast<int>(r - 1);
    } else {
        q = rs;  // no digits: the '$' wasn't ours
        absR = false;
    }
    if (letters && row >= 0) kind = 0;
    else if (letters) kind = 1;
    else if (row >= 0) {
        kind = 2;
        if (absC) {  // "$5" style: the leading '$' belonged to the row
            absR = true;
            absC = false;
        }
    } else return false;
    p = q;
    return true;
}

// Parses a reference (no sheet prefix) at p. Fails unless it ends at a token boundary.
bool tryRef(const std::string& s, std::size_t p, FormulaRef& ref, std::size_t& endPos) {
    int r1, c1, k1;
    bool ar1, ac1;
    std::size_t q = p;
    if (!refPart(s, q, r1, c1, ar1, ac1, k1)) return false;
    FormulaRef out;
    auto single = [&] {
        out.kind = FormulaRef::CellRef;
        out.r1 = out.r2 = r1;
        out.c1 = out.c2 = c1;
        out.absR1 = out.absR2 = ar1;
        out.absC1 = out.absC2 = ac1;
    };
    if (q < s.size() && s[q] == ':') {
        std::size_t q2 = q + 1;
        int r2, c2, k2;
        bool ar2, ac2;
        if (refPart(s, q2, r2, c2, ar2, ac2, k2) && k2 == k1) {
            q = q2;
            out.kind = k1 == 0 ? FormulaRef::RangeRef : k1 == 1 ? FormulaRef::ColsRef : FormulaRef::RowsRef;
            out.r1 = r1;
            out.c1 = c1;
            out.r2 = r2;
            out.c2 = c2;
            out.absR1 = ar1;
            out.absC1 = ac1;
            out.absR2 = ar2;
            out.absC2 = ac2;
        } else if (k1 != 0) {
            return false;
        } else {
            single();
        }
    } else {
        if (k1 != 0) return false;
        single();
    }
    if (q < s.size() && (identChar(s[q]) || s[q] == '(' || s[q] == '!' || s[q] == '$')) return false;
    ref = out;
    endPos = q;
    return true;
}

std::vector<Tok> tokenize(const std::string& s, std::string& error) {
    std::vector<Tok> out;
    std::size_t p = 0;
    auto fail = [&](const std::string& why) {
        error = why;
        return std::vector<Tok>{};
    };
    while (p < s.size()) {
        const char c = s[p];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++p;
            continue;
        }
        Tok t;
        t.begin = p;
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && p + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[p + 1])))) {
            FormulaRef rows;
            std::size_t e;
            if (tryRef(s, p, rows, e) && rows.kind == FormulaRef::RowsRef) {
                t.t = Tok::Ref;
                t.ref = rows;
                t.end = p = e;
                out.push_back(t);
                continue;
            }
            std::size_t q = p;
            while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) ++q;
            if (q < s.size() && s[q] == '.') {
                ++q;
                while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q]))) ++q;
            }
            if (q < s.size() && (s[q] == 'e' || s[q] == 'E')) {
                std::size_t r = q + 1;
                if (r < s.size() && (s[r] == '+' || s[r] == '-')) ++r;
                if (r < s.size() && std::isdigit(static_cast<unsigned char>(s[r]))) {
                    while (r < s.size() && std::isdigit(static_cast<unsigned char>(s[r]))) ++r;
                    q = r;
                }
            }
            t.t = Tok::Num;
            t.num = std::strtod(s.substr(p, q - p).c_str(), nullptr);
            t.end = p = q;
            out.push_back(t);
            continue;
        }
        if (c == '"') {
            std::string v;
            std::size_t q = p + 1;
            bool closed = false;
            while (q < s.size()) {
                if (s[q] == '"') {
                    if (q + 1 < s.size() && s[q + 1] == '"') {
                        v += '"';
                        q += 2;
                        continue;
                    }
                    closed = true;
                    ++q;
                    break;
                }
                v += s[q++];
            }
            if (!closed) return fail("unterminated text (missing \")");
            t.t = Tok::Str;
            t.text = v;
            t.end = p = q;
            out.push_back(t);
            continue;
        }
        if (c == '#') {
            // A2# right after a reference: the spill range anchored there
            if (!out.empty() && out.back().t == Tok::Ref && out.back().end == p && out.back().ref.kind == FormulaRef::CellRef) {
                t.t = Tok::Op;
                t.text = "#";
                t.end = p = p + 1;
                out.push_back(t);
                continue;
            }
            bool found = false;
            for (const char* e : kErrors) {
                std::size_t n = std::char_traits<char>::length(e);
                std::string up = upper(s.substr(p, n));
                if (up == e) {
                    t.t = Tok::Err;
                    t.text = e;
                    t.end = p = p + n;
                    found = true;
                    break;
                }
            }
            if (!found) return fail("unknown error value at '" + s.substr(p, 8) + "'");
            out.push_back(t);
            continue;
        }
        if (c == '\'') {
            std::string name;
            std::size_t q = p + 1;
            bool closed = false;
            while (q < s.size()) {
                if (s[q] == '\'') {
                    if (q + 1 < s.size() && s[q + 1] == '\'') {
                        name += '\'';
                        q += 2;
                        continue;
                    }
                    closed = true;
                    ++q;
                    break;
                }
                name += s[q++];
            }
            if (!closed || q >= s.size() || s[q] != '!') return fail("a quoted sheet name must be followed by '!'");
            std::size_t e;
            if (!tryRef(s, q + 1, t.ref, e)) return fail("bad reference after sheet '" + name + "'");
            t.ref.sheet = name;
            t.t = Tok::Ref;
            t.end = p = e;
            out.push_back(t);
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '\\' || (c & 0x80)) {
            std::size_t e;
            if (tryRef(s, p, t.ref, e)) {
                t.t = Tok::Ref;
                t.end = p = e;
                out.push_back(t);
                continue;
            }
            std::size_t q = p;
            if (s[q] == '$') return fail("bad reference at '" + s.substr(p, 8) + "'");
            while (q < s.size() && identChar(s[q])) ++q;
            std::string word = s.substr(p, q - p);
            if (q < s.size() && s[q] == '!') {
                if (!tryRef(s, q + 1, t.ref, e)) return fail("bad reference after sheet '" + word + "'");
                t.ref.sheet = word;
                t.t = Tok::Ref;
                t.end = p = e;
                out.push_back(t);
                continue;
            }
            std::size_t r = q;
            while (r < s.size() && s[r] == ' ') ++r;
            std::string up = upper(word);
            if (r < s.size() && s[r] == '(') {
                t.t = Tok::Func;
                t.text = up;
                for (const char* pre : {"_XLFN._XLWS.", "_XLFN.", "_XLWS."}) {
                    if (startsWith(t.text, pre)) t.text = t.text.substr(std::char_traits<char>::length(pre));
                }
                t.end = p = r + 1;
            } else if (up == "TRUE" || up == "FALSE") {
                t.t = Tok::Bool;
                t.num = up == "TRUE" ? 1 : 0;
                t.end = p = q;
            } else {
                t.t = Tok::Name;
                t.text = word;
                t.end = p = q;
            }
            out.push_back(t);
            continue;
        }
        if (c == '(' || c == ')' || c == ',' || c == ';' || c == '{' || c == '}') {
            t.t = c == '(' ? Tok::LP : c == ')' ? Tok::RP : c == '{' ? Tok::LB : c == '}' ? Tok::RB : Tok::Sep;
            t.text = std::string(1, c);
            t.end = p = p + 1;
            out.push_back(t);
            continue;
        }
        std::string two = s.substr(p, 2);
        if (two == "<>" || two == "<=" || two == ">=") {
            t.t = Tok::Op;
            t.text = two;
            t.end = p = p + 2;
            out.push_back(t);
            continue;
        }
        if (std::string("+-*/^&=<>%@").find(c) != std::string::npos) {
            t.t = Tok::Op;
            t.text = std::string(1, c);
            t.end = p = p + 1;
            out.push_back(t);
            continue;
        }
        return fail(std::string("unexpected '") + c + "'");
    }
    Tok end;
    end.t = Tok::End;
    end.begin = end.end = s.size();
    out.push_back(end);
    return out;
}

// ---- parser ----------------------------------------------------------------------

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Parser {
    const std::vector<Tok>& toks;
    std::size_t i = 0;

    const Tok& peek() const { return toks[i]; }
    bool isOp(const char* op) const { return peek().t == Tok::Op && peek().text == op; }

    NodeP make(FormulaNode n) { return std::make_shared<const FormulaNode>(std::move(n)); }
    NodeP binary(const std::string& op, NodeP a, NodeP b) {
        FormulaNode n;
        n.kind = FormulaNode::Binary;
        n.str = op;
        n.args = {std::move(a), std::move(b)};
        return make(std::move(n));
    }
    NodeP call(const std::string& name, std::vector<NodeP> args) {
        FormulaNode n;
        n.kind = FormulaNode::Call;
        n.str = name;
        n.args = std::move(args);
        return make(std::move(n));
    }

    NodeP expr() { return comparison(); }
    NodeP comparison() {
        NodeP l = concat();
        while (peek().t == Tok::Op && (peek().text == "=" || peek().text == "<>" || peek().text == "<" || peek().text == ">" ||
                                       peek().text == "<=" || peek().text == ">=")) {
            std::string op = toks[i++].text;
            l = binary(op, l, concat());
        }
        return l;
    }
    NodeP concat() {
        NodeP l = additive();
        while (isOp("&")) {
            ++i;
            l = binary("&", l, additive());
        }
        return l;
    }
    NodeP additive() {
        NodeP l = multiplicative();
        while (isOp("+") || isOp("-")) {
            std::string op = toks[i++].text;
            l = binary(op, l, multiplicative());
        }
        return l;
    }
    NodeP multiplicative() {
        NodeP l = power();
        while (isOp("*") || isOp("/")) {
            std::string op = toks[i++].text;
            l = binary(op, l, power());
        }
        return l;
    }
    NodeP power() {
        NodeP l = unary();
        while (isOp("^")) {
            ++i;
            l = binary("^", l, unary());
        }
        return l;
    }
    NodeP unary() {
        if (isOp("-") || isOp("+")) {
            std::string op = toks[i++].text;
            FormulaNode n;
            n.kind = FormulaNode::Unary;
            n.str = op;
            n.args = {unary()};
            return make(std::move(n));
        }
        if (isOp("@")) {  // implicit intersection
            ++i;
            return call("SINGLE", {unary()});
        }
        return postfix();
    }
    NodeP postfix() {
        NodeP p = primary();
        while (isOp("%") || isOp("#")) {
            if (isOp("#")) {
                ++i;
                p = call("ANCHORARRAY", {p});
                continue;
            }
            ++i;
            FormulaNode n;
            n.kind = FormulaNode::Percent;
            n.args = {p};
            p = make(std::move(n));
        }
        return p;
    }
    NodeP constant() {
        const Tok& t = toks[i];
        FormulaNode n;
        bool neg = false;
        if (t.t == Tok::Op && (t.text == "-" || t.text == "+")) {
            neg = t.text == "-";
            ++i;
        }
        const Tok& v = toks[i];
        switch (v.t) {
            case Tok::Num:
                n.kind = FormulaNode::Num;
                n.num = neg ? -v.num : v.num;
                break;
            case Tok::Str: n.kind = FormulaNode::Str, n.str = v.text; break;
            case Tok::Bool: n.kind = FormulaNode::Bool, n.num = v.num; break;
            case Tok::Err: n.kind = FormulaNode::Err, n.str = v.text; break;
            default: throw ParseError("an array {...} holds numbers, text, TRUE/FALSE or errors only");
        }
        ++i;
        return make(std::move(n));
    }
    NodeP array() {
        // {1,2,3;4,5,6}: ',' between columns, ';' between rows
        FormulaNode n;
        n.kind = FormulaNode::Array;
        int cols = -1, cur = 0;
        while (true) {
            n.args.push_back(constant());
            ++cur;
            if (peek().t == Tok::Sep) {
                if (peek().text == ";") {
                    if (cols >= 0 && cur != cols) throw ParseError("every row of an array {...} needs the same number of values");
                    cols = cur;
                    cur = 0;
                }
                ++i;
                continue;
            }
            if (peek().t == Tok::RB) {
                ++i;
                break;
            }
            throw ParseError("expected ',', ';' or '}' in an array");
        }
        if (cols >= 0 && cur != cols) throw ParseError("every row of an array {...} needs the same number of values");
        if (cols < 0) cols = cur;
        n.cols = cols;
        n.rows = static_cast<int>(n.args.size()) / cols;
        return make(std::move(n));
    }
    NodeP primary() {
        const Tok& t = toks[i];
        FormulaNode n;
        switch (t.t) {
            case Tok::Num:
                ++i;
                n.kind = FormulaNode::Num;
                n.num = t.num;
                return make(std::move(n));
            case Tok::Str:
                ++i;
                n.kind = FormulaNode::Str;
                n.str = t.text;
                return make(std::move(n));
            case Tok::Bool:
                ++i;
                n.kind = FormulaNode::Bool;
                n.num = t.num;
                return make(std::move(n));
            case Tok::Err:
                ++i;
                n.kind = FormulaNode::Err;
                n.str = t.text;
                return make(std::move(n));
            case Tok::Ref:
                ++i;
                n.kind = FormulaNode::Ref;
                n.ref = t.ref;
                return make(std::move(n));
            case Tok::Name:
                ++i;
                n.kind = FormulaNode::Name;  // a defined name or a LET variable
                n.str = t.text;
                return make(std::move(n));
            case Tok::LB:
                ++i;
                return array();
            case Tok::LP: {
                ++i;
                NodeP e = expr();
                if (peek().t != Tok::RP) throw ParseError("missing ')'");
                ++i;
                return e;
            }
            case Tok::Func: {
                ++i;
                n.kind = FormulaNode::Call;
                n.str = t.text;
                if (peek().t == Tok::RP) {
                    ++i;
                    return make(std::move(n));
                }
                while (true) {
                    if (peek().t == Tok::Sep || peek().t == Tok::RP) {
                        FormulaNode missing;
                        missing.kind = FormulaNode::Missing;
                        n.args.push_back(make(std::move(missing)));
                    } else {
                        n.args.push_back(expr());
                    }
                    if (peek().t == Tok::Sep) {
                        ++i;
                        continue;
                    }
                    if (peek().t == Tok::RP) {
                        ++i;
                        break;
                    }
                    throw ParseError("expected ',' or ')' in " + n.str + "()");
                }
                return make(std::move(n));
            }
            case Tok::End: throw ParseError("the formula ends too early");
            default: throw ParseError("unexpected '" + (t.text.empty() ? std::string(t.t == Tok::RP ? ")" : ",") : t.text) + "'");
        }
    }
};

NodeP parseOrError(const std::string& formula, std::string& error) {
    std::vector<Tok> toks = tokenize(formula, error);
    if (!error.empty()) return nullptr;
    Parser p{toks};
    try {
        NodeP n = p.expr();
        if (p.peek().t != Tok::End) throw ParseError("unexpected text after the formula");
        return n;
    } catch (const ParseError& e) {
        error = e.what();
        return nullptr;
    }
}

}  // namespace

std::shared_ptr<const FormulaNode> parseFormula(const std::string& formula) {
    std::string error;
    NodeP n = parseOrError(formula, error);
    if (n) return n;
    FormulaNode bad;
    bad.kind = FormulaNode::Err;
    bad.str = "#NAME?";
    return std::make_shared<const FormulaNode>(std::move(bad));
}

std::string formulaError(const std::string& formula) {
    std::string error;
    parseOrError(formula, error);
    return error;
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 255) return false;
    const unsigned char c0 = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(c0) || name[0] == '_' || name[0] == '\\' || (c0 & 0x80))) return false;
    for (char c : name) {
        if (!identChar(c)) return false;
    }
    const std::string up = upper(name);
    if (up == "TRUE" || up == "FALSE" || up == "R" || up == "C") return false;
    FormulaRef r;
    std::size_t e;
    if (tryRef(name, 0, r, e) && e == name.size()) return false;  // looks like a cell
    return true;
}

// ---- rewriting -------------------------------------------------------------------

namespace {

std::string refPartText(int row, int col, bool absR, bool absC, int kind) {
    std::string s;
    if (kind != 2) s += (absC ? "$" : "") + colName(col);
    if (kind != 1) s += (absR ? "$" : "") + std::to_string(row + 1);
    return s;
}

std::string refText(const FormulaRef& r) {
    std::string s = r.sheet.empty() ? "" : quoteSheet(r.sheet) + "!";
    switch (r.kind) {
        case FormulaRef::CellRef: return s + refPartText(r.r1, r.c1, r.absR1, r.absC1, 0);
        case FormulaRef::RangeRef:
            return s + refPartText(r.r1, r.c1, r.absR1, r.absC1, 0) + ":" + refPartText(r.r2, r.c2, r.absR2, r.absC2, 0);
        case FormulaRef::ColsRef: return s + refPartText(0, r.c1, false, r.absC1, 1) + ":" + refPartText(0, r.c2, false, r.absC2, 1);
        case FormulaRef::RowsRef: return s + refPartText(r.r1, 0, r.absR1, false, 2) + ":" + refPartText(r.r2, 0, r.absR2, false, 2);
    }
    return s;
}

}  // namespace

std::string rewriteFormula(const std::string& formula, const std::function<bool(FormulaRef&)>& fn) {
    std::string error;
    std::vector<Tok> toks = tokenize(formula, error);
    if (!error.empty()) return formula;
    std::string out;
    std::size_t last = 0;
    for (const auto& t : toks) {
        if (t.t != Tok::Ref) continue;
        FormulaRef r = t.ref;
        out += formula.substr(last, t.begin - last);
        if (fn(r)) {
            if (r.kind == FormulaRef::CellRef) {
                r.r2 = r.r1;
                r.c2 = r.c1;
            }
            out += refText(r);
        } else {
            out += "#REF!";
        }
        last = t.end;
    }
    out += formula.substr(last);
    return out;
}

std::string offsetFormula(const std::string& formula, int dr, int dc) {
    return rewriteFormula(formula, [&](FormulaRef& r) {
        auto mv = [](int& v, bool abs, int d, int limit) {
            if (abs) return true;
            v += d;
            return v >= 0 && v < limit;
        };
        bool ok = true;
        if (r.kind != FormulaRef::ColsRef) ok = mv(r.r1, r.absR1, dr, kMaxRows) && mv(r.r2, r.absR2, dr, kMaxRows) && ok;
        if (r.kind != FormulaRef::RowsRef) ok = mv(r.c1, r.absC1, dc, kMaxCols) && mv(r.c2, r.absC2, dc, kMaxCols) && ok;
        return ok;
    });
}

// ---- evaluation ------------------------------------------------------------------

namespace fx {

const CellValue kEmpty;

struct Deferred {
    Cell* cell;
    int sheet, row, col;
};

struct Recalc {
    Workbook& wb;
    std::unordered_map<const Cell*, int> state;  // 0 dirty, 1 computing, 2 done
    std::map<std::pair<int, std::int64_t>, const Cell*> owner;  // spilled cell -> the formula that spilled it this pass
    bool spillChanged = false;
    int spills = 0;
    void compute(int sheet, std::int64_t key, Cell& cell, int depth);
    void spill(int sheet, std::int64_t key, Cell& cell, const Matrix& m);
};

double nowSerial() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return dateSerial(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) + (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) / 86400.0;
}

const CellValue& cellValue(Ctx& ctx, int sheet, int row, int col) {
    const Sheet& sh = ctx.wb.sheets[static_cast<std::size_t>(sheet)];
    auto it = sh.cells.find(Sheet::key(row, col));
    if (it == sh.cells.end()) return kEmpty;
    const Cell& c = it->second;
    if (c.hasFormula() && ctx.rc) {
        auto st = ctx.rc->state.find(&c);
        if (st != ctx.rc->state.end() && st->second != 2) {
            if (st->second == 1) throw EvalError{"#CYCLE!"};
            Cell& mc = const_cast<Cell&>(c);  // the workbook is mutable while recalculating
            ctx.rc->compute(sheet, it->first, mc, ctx.depth + 1);
        }
    }
    return c.value;
}

namespace {

int resolveSheet(const Ctx& ctx, const FormulaRef& r) {
    if (r.sheet.empty()) return ctx.sheet;
    return ctx.wb.sheetIndex(r.sheet);
}

Range toRange(const FormulaRef& r, int sheet) {
    Range out;
    out.sheet = sheet;
    switch (r.kind) {
        case FormulaRef::CellRef:
            out.r1 = out.r2 = r.r1;
            out.c1 = out.c2 = r.c1;
            break;
        case FormulaRef::RangeRef:
            out.r1 = std::min(r.r1, r.r2);
            out.r2 = std::max(r.r1, r.r2);
            out.c1 = std::min(r.c1, r.c2);
            out.c2 = std::max(r.c1, r.c2);
            break;
        case FormulaRef::ColsRef:
            out.r1 = 0;
            out.r2 = kMaxRows - 1;
            out.c1 = std::min(r.c1, r.c2);
            out.c2 = std::max(r.c1, r.c2);
            break;
        case FormulaRef::RowsRef:
            out.c1 = 0;
            out.c2 = kMaxCols - 1;
            out.r1 = std::min(r.r1, r.r2);
            out.r2 = std::max(r.r1, r.r2);
            break;
    }
    return out;
}

// Whole columns / rows trimmed to the used area; explicit ranges keep their size.
Range trimWhole(const Ctx& ctx, Range r) {
    const bool wholeCols = r.r1 == 0 && r.r2 == kMaxRows - 1, wholeRows = r.c1 == 0 && r.c2 == kMaxCols - 1;
    if (!wholeCols && !wholeRows) return r;
    int br1, bc1, br2, bc2;
    if (!ctx.wb.sheets[static_cast<std::size_t>(r.sheet)].bounds(br1, bc1, br2, bc2)) br2 = bc2 = 0;
    if (wholeCols) r.r2 = std::max(r.r1, std::min(r.r2, br2));
    if (wholeRows) r.c2 = std::max(r.c1, std::min(r.c2, bc2));
    return r;
}

}  // namespace

Range clipped(const Ctx& ctx, Range r) {
    int br1, bc1, br2, bc2;
    if (!ctx.wb.sheets[static_cast<std::size_t>(r.sheet)].bounds(br1, bc1, br2, bc2)) {
        r.r2 = r.r1;
        r.c2 = r.c1;
        return r;
    }
    r.r2 = std::max(r.r1, std::min(r.r2, br2));
    r.c2 = std::max(r.c1, std::min(r.c2, bc2));
    return r;
}

CellValue scalar(const Arg& a, Ctx& ctx) {
    if (a.arr) return a.arr->v.empty() ? CellValue{} : a.arr->v[0];
    if (!a.isRange) return a.v;
    const Range& r = a.r;
    if (r.r1 == r.r2 && r.c1 == r.c2) return cellValue(ctx, r.sheet, r.r1, r.c1);
    if (r.c1 == r.c2 && ctx.row >= r.r1 && ctx.row <= r.r2) return cellValue(ctx, r.sheet, ctx.row, r.c1);
    if (r.r1 == r.r2 && ctx.col >= r.c1 && ctx.col <= r.c2) return cellValue(ctx, r.sheet, r.r1, ctx.col);
    return CellValue::error("#VALUE!");
}

void dims(const Arg& a, Ctx& ctx, int& rows, int& cols) {
    if (a.arr) {
        rows = a.arr->rows;
        cols = a.arr->cols;
    } else if (a.isRange) {
        Range r = trimWhole(ctx, a.r);
        rows = r.rows();
        cols = r.cols();
    } else {
        rows = cols = 1;
    }
}

Matrix toMatrix(const Arg& a, Ctx& ctx) {
    if (a.arr) return *a.arr;
    if (!a.isRange) {
        Matrix m(1, 1);
        m.v[0] = a.v;
        return m;
    }
    Range r = trimWhole(ctx, a.r);
    if (static_cast<long long>(r.rows()) * r.cols() > 4000000) throw EvalError{"#NUM!"};
    Matrix m(r.rows(), r.cols());
    eachCell(ctx, r, [&](int row, int col, const CellValue& v) { m.at(row - r.r1, col - r.c1) = v; });
    return m;
}

std::vector<CellValue> lineOf(const Arg& a, Ctx& ctx) {
    if (a.isRange) {
        // a lookup line: rows of one column, or columns of one row; whole ones stop at the used area
        const Range& r = a.r;
        Range c = clipped(ctx, r);
        std::vector<CellValue> out;
        if (r.r1 == r.r2 || (r.c1 != r.c2 && r.rows() < r.cols())) {
            for (int col = r.c1; col <= c.c2; ++col) out.push_back(cellValue(ctx, r.sheet, r.r1, col));
        } else {
            for (int row = r.r1; row <= c.r2; ++row) out.push_back(cellValue(ctx, r.sheet, row, r.c1));
        }
        return out;
    }
    Matrix m = toMatrix(a, ctx);
    std::vector<CellValue> out;
    if (m.rows == 1) return m.v;
    for (int i = 0; i < m.rows; ++i) out.push_back(m.at(i, 0));
    return out;
}

Range rangeArg(const NodeP& n, Ctx& ctx) {
    Arg a = evalArg(n, ctx);
    if (!a.isRange) {
        if (!a.arr && a.v.isError()) throw EvalError{a.v.text};
        throw EvalError{"#VALUE!"};
    }
    return a.r;
}

std::vector<double> numbers(const std::vector<NodeP>& args, Ctx& ctx) {
    std::vector<double> v;
    eachNumber(args, ctx, [&](double x) { v.push_back(x); });
    return v;
}

NodeP valueNode(CellValue v) {
    FormulaNode n;
    n.kind = FormulaNode::Value;
    n.value = std::move(v);
    return std::make_shared<const FormulaNode>(std::move(n));
}

NodeP matrixNode(const Matrix& m) {
    FormulaNode n;
    n.kind = FormulaNode::Array;
    n.rows = m.rows;
    n.cols = m.cols;
    n.args.reserve(m.v.size());
    for (const auto& v : m.v) n.args.push_back(valueNode(v));
    return std::make_shared<const FormulaNode>(std::move(n));
}

NodeP missingNode() {
    static const NodeP m = std::make_shared<const FormulaNode>();
    return m;
}

Arg callFunction(const std::string& name, const std::vector<NodeP>& args, Ctx& ctx) {
    const auto& fns = functions();
    auto it = fns.find(name);
    if (it == fns.end()) throw EvalError{"#NAME?"};
    return it->second(args, ctx);
}

const Cell* cellAt(const Ctx& ctx, int sheet, int row, int col) {
    if (sheet < 0 || sheet >= static_cast<int>(ctx.wb.sheets.size())) return nullptr;
    return ctx.wb.sheets[static_cast<std::size_t>(sheet)].get(row, col);
}

bool parseNumber(const std::string& text, double& out) {
    std::string t = trimCopy(text);
    if (t.empty()) return false;
    bool pct = false;
    if (t.back() == '%') {
        pct = true;
        t.pop_back();
    }
    std::string clean;
    for (char c : t) {
        if (c != ',' && c != '$') clean += c;
    }
    if (clean.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(clean.c_str(), &end);
    if (!end || *end != '\0') return false;
    if (!std::isfinite(v)) return false;
    out = pct ? v / 100 : v;
    return true;
}

double toNum(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number:
        case CellValue::Bool: return v.number;
        case CellValue::Empty: return 0;
        case CellValue::Error: throw EvalError{v.text};
        case CellValue::Text: {
            double d;
            if (parseNumber(v.text, d)) return d;
            int y, m, dd;
            if (parseIsoDate(v.text, y, m, dd)) return dateSerial(y, m, dd);
            throw EvalError{"#VALUE!"};
        }
    }
    return 0;
}

std::string toStr(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number: return textNumber(v.number);
        case CellValue::Bool: return v.number != 0 ? "TRUE" : "FALSE";
        case CellValue::Text: return v.text;
        case CellValue::Error: throw EvalError{v.text};
        case CellValue::Empty: return "";
    }
    return "";
}

bool toBool(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number:
        case CellValue::Bool: return v.number != 0;
        case CellValue::Empty: return false;
        case CellValue::Error: throw EvalError{v.text};
        case CellValue::Text: {
            std::string u = upper(v.text);
            if (u == "TRUE") return true;
            if (u == "FALSE") return false;
            throw EvalError{"#VALUE!"};
        }
    }
    return false;
}

CellValue checkNum(double v) {
    if (!std::isfinite(v)) throw EvalError{"#NUM!"};
    return CellValue::num(v);
}

// Excel ordering: numbers < text < booleans; text compares case-insensitively.
int compareValues(const CellValue& a, const CellValue& b) {
    auto rank = [](const CellValue& v) {
        switch (v.type) {
            case CellValue::Number: return 0;
            case CellValue::Text: return 1;
            case CellValue::Bool: return 2;
            default: return 0;
        }
    };
    CellValue x = a, y = b;
    if (x.empty()) x = y.type == CellValue::Text ? CellValue::str("") : y.type == CellValue::Bool ? CellValue::boolean(false) : CellValue::num(0);
    if (y.empty()) y = x.type == CellValue::Text ? CellValue::str("") : x.type == CellValue::Bool ? CellValue::boolean(false) : CellValue::num(0);
    if (rank(x) != rank(y)) return rank(x) < rank(y) ? -1 : 1;
    if (x.type == CellValue::Text) {
        std::string l = lower(x.text), r = lower(y.text);
        return l < r ? -1 : l > r ? 1 : 0;
    }
    return x.number < y.number ? -1 : x.number > y.number ? 1 : 0;
}

bool wildcardMatch(const std::string& pat, const std::string& text) {
    // '*' any run, '?' one character, '~' escapes; case-insensitive
    std::string p = lower(pat), t = lower(text);
    std::size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && p[pi] == '~' && pi + 1 < p.size() && p[pi + 1] == t[ti]) {
            pi += 2;
            ++ti;
        } else if (pi < p.size() && (p[pi] == '?' || (p[pi] == t[ti] && p[pi] != '*'))) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

Criterion::Criterion(const CellValue& c) {
    if (c.type != CellValue::Text) {
        value = c;
        return;
    }
    std::string s = c.text;
    for (const char* o : {"<=", ">=", "<>", "<", ">", "="}) {
        std::size_t n = std::char_traits<char>::length(o);
        if (s.compare(0, n, o) == 0) {
            op = o;
            s = s.substr(n);
            break;
        }
    }
    double d;
    int y, m, dd;
    if (parseNumber(s, d)) value = CellValue::num(d);
    else if (parseIsoDate(s, y, m, dd)) value = CellValue::num(dateSerial(y, m, dd));
    else if (upper(s) == "TRUE" || upper(s) == "FALSE") value = CellValue::boolean(upper(s) == "TRUE");
    else {
        value = s.empty() ? CellValue{} : CellValue::str(s);
        wildcard = s.find_first_of("*?") != std::string::npos;
    }
}

bool Criterion::matches(const CellValue& v) const {
    if (v.isError()) return false;
    if (value.empty()) {
        const bool blank = v.empty() || (v.type == CellValue::Text && v.text.empty());
        return op == "<>" ? !blank : op == "=" ? blank : false;
    }
    if (value.type == CellValue::Text && (op == "=" || op == "<>")) {
        bool eq = v.type == CellValue::Text && (wildcard ? wildcardMatch(value.text, v.text) : lower(v.text) == lower(value.text));
        return op == "=" ? eq : !eq;
    }
    if (op == "<>") {
        if (v.type != value.type) return true;
        return compareValues(v, value) != 0;
    }
    if (v.type != value.type || v.empty()) return false;  // numbers only compare with numbers
    int c = compareValues(v, value);
    if (op == "=") return c == 0;
    if (op == "<") return c < 0;
    if (op == ">") return c > 0;
    if (op == "<=") return c <= 0;
    return c >= 0;
}

bool lookupEqual(const CellValue& a, const CellValue& b, bool wildcard) {
    if (a.type == CellValue::Text && b.type == CellValue::Text) {
        return wildcard ? wildcardMatch(a.text, b.text) : lower(a.text) == lower(b.text);
    }
    if (a.type != b.type) return false;
    return a.number == b.number;
}

int findIn(const std::vector<CellValue>& list, const CellValue& look, int mode) {
    if (mode == 0 || mode == 2) {
        const bool wild = mode == 2 || (look.type == CellValue::Text && look.text.find_first_of("*?") != std::string::npos);
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (lookupEqual(look, list[i], wild)) return static_cast<int>(i);
        }
        return -1;
    }
    int best = -1;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const CellValue& v = list[i];
        if (v.empty() || v.isError()) continue;
        if ((v.type == CellValue::Text) != (look.type == CellValue::Text)) continue;
        int c = compareValues(v, look);
        if (mode == 1) {
            if (c <= 0) best = static_cast<int>(i);
            else break;
        } else {
            if (c >= 0) best = static_cast<int>(i);
            else break;
        }
    }
    return best;
}

std::string utf8Substr(const std::string& s, std::size_t start, std::size_t count) {
    auto cps = pdf::utf8Decode(s);
    std::string out;
    for (std::size_t i = start; i < cps.size() && i < start + count; ++i) out += pdf::utf8Encode(cps[i]);
    return out;
}
std::size_t utf8Len(const std::string& s) { return pdf::utf8Decode(s).size(); }

double roundTo(double v, double digits, int mode) {  // 0 half away from zero, 1 up (away), -1 down (toward zero)
    const double f = std::pow(10.0, std::trunc(digits));
    double x = v * f;
    // undo binary noise such as 2.675 * 100 = 267.49999999999997
    const double nudged = std::round(x * 1e9) / 1e9;
    if (std::abs(nudged - x) < 1e-7) x = nudged;
    double r;
    if (mode == 0) r = std::round(x);
    else if (mode == 1) r = x < 0 ? -std::ceil(-x) : std::ceil(x);
    else r = std::trunc(x);
    return r / f;
}

namespace {

Arg binaryOp(const std::string& op, const CellValue& l, const CellValue& r) {
    if (op == "&") return str(toStr(l) + toStr(r));
    if (op == "=" || op == "<>" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        if (l.isError()) throw EvalError{l.text};
        if (r.isError()) throw EvalError{r.text};
        int c = compareValues(l, r);
        bool res = op == "=" ? c == 0 : op == "<>" ? c != 0 : op == "<" ? c < 0 : op == ">" ? c > 0 : op == "<=" ? c <= 0 : c >= 0;
        return boolean(res);
    }
    double a = toNum(l), b = toNum(r);
    if (op == "+") return num(a + b);
    if (op == "-") return num(a - b);
    if (op == "*") return num(a * b);
    if (op == "/") {
        if (b == 0) throw EvalError{"#DIV/0!"};
        return num(a / b);
    }
    if (op == "^") {
        if (a == 0 && b < 0) throw EvalError{"#DIV/0!"};
        double p = std::pow(a, b);
        if (std::isnan(p)) throw EvalError{"#NUM!"};
        return num(p);
    }
    throw EvalError{"#VALUE!"};
}

CellValue guarded(const std::function<CellValue()>& f) {
    try {
        return f();
    } catch (const EvalError& e) {
        return CellValue::error(e.code);
    }
}

// Excel's broadcasting: a single row/column/value stretches; past the end of a shorter array is #N/A.
Matrix broadcast2(const Matrix& a, const Matrix& b, const std::function<CellValue(const CellValue&, const CellValue&)>& f) {
    const int rows = std::max(a.rows, b.rows), cols = std::max(a.cols, b.cols);
    if (static_cast<long long>(rows) * cols > 4000000) throw EvalError{"#NUM!"};
    Matrix out(rows, cols);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            const int ai = a.rows == 1 ? 0 : i, aj = a.cols == 1 ? 0 : j, bi = b.rows == 1 ? 0 : i, bj = b.cols == 1 ? 0 : j;
            if (ai >= a.rows || aj >= a.cols || bi >= b.rows || bj >= b.cols) {
                out.at(i, j) = CellValue::error("#N/A");
                continue;
            }
            const CellValue& x = a.at(ai, aj);
            const CellValue& y = b.at(bi, bj);
            out.at(i, j) = guarded([&] { return f(x, y); });
        }
    }
    return out;
}

const DefinedName* lookupName(const Workbook& wb, const std::string& n) { return wb.name(n); }

Arg evalName(const FormulaNode& n, Ctx& ctx) {
    const std::string up = upper(n.str);
    if (ctx.locals) {
        for (auto it = ctx.locals->rbegin(); it != ctx.locals->rend(); ++it) {
            if (it->first == up) return it->second;
        }
    }
    const DefinedName* dn = lookupName(ctx.wb, up);
    if (!dn) throw EvalError{"#NAME?"};
    if (ctx.depth > 60) throw EvalError{"#CYCLE!"};
    if (!dn->ast) const_cast<DefinedName*>(dn)->ast = parseFormula(dn->formula);  // cached on first use
    Ctx inner = ctx;
    inner.locals = nullptr;
    inner.depth = ctx.depth + 4;
    return evalNode(*dn->ast, inner);
}

// A per-element call of a function whose arguments are single values.
Arg liftedCall(const fx::Fn& fn, const FormulaNode& n, Ctx& ctx) {
    std::vector<Arg> vals(n.args.size());
    bool multi = false;
    for (std::size_t i = 0; i < n.args.size(); ++i) {
        if (n.args[i]->kind == FormulaNode::Missing) continue;
        vals[i] = evalNode(*n.args[i], ctx);
        multi = multi || vals[i].multi();
    }
    if (!multi) {
        std::vector<NodeP> nodes(n.args.size());
        for (std::size_t i = 0; i < n.args.size(); ++i) {
            nodes[i] = n.args[i]->kind == FormulaNode::Missing ? n.args[i] : valueNode(scalar(vals[i], ctx));
        }
        return fn(nodes, ctx);
    }
    std::vector<Matrix> ms(n.args.size());
    int rows = 1, cols = 1;
    for (std::size_t i = 0; i < n.args.size(); ++i) {
        if (n.args[i]->kind == FormulaNode::Missing) continue;
        ms[i] = toMatrix(vals[i], ctx);
        rows = std::max(rows, ms[i].rows);
        cols = std::max(cols, ms[i].cols);
    }
    if (static_cast<long long>(rows) * cols > 1000000) throw EvalError{"#NUM!"};
    Matrix out(rows, cols);
    std::vector<NodeP> nodes(n.args.size());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            bool outside = false;
            for (std::size_t i = 0; i < n.args.size(); ++i) {
                if (n.args[i]->kind == FormulaNode::Missing) {
                    nodes[i] = n.args[i];
                    continue;
                }
                const Matrix& m = ms[i];
                const int mi = m.rows == 1 ? 0 : r, mj = m.cols == 1 ? 0 : c;
                if (mi >= m.rows || mj >= m.cols) {
                    outside = true;
                    break;
                }
                nodes[i] = valueNode(m.at(mi, mj));
            }
            if (outside) {
                out.at(r, c) = CellValue::error("#N/A");
                continue;
            }
            out.at(r, c) = guarded([&] {
                Arg a = fn(nodes, ctx);
                CellValue v = scalar(a, ctx);
                return v;
            });
        }
    }
    return Arg::array(std::move(out));
}

}  // namespace

Arg evalNode(const FormulaNode& n, Ctx& ctx) {
    try {
        switch (n.kind) {
            case FormulaNode::Num: return Arg::val(CellValue::num(n.num));
            case FormulaNode::Str: return Arg::val(CellValue::str(n.str));
            case FormulaNode::Bool: return Arg::val(CellValue::boolean(n.num != 0));
            case FormulaNode::Err: return Arg::val(CellValue::error(n.str));
            case FormulaNode::Missing: return Arg::val(CellValue{});
            case FormulaNode::Value: return Arg::val(n.value);
            case FormulaNode::Name: return evalName(n, ctx);
            case FormulaNode::Array: {
                Matrix m(n.rows, n.cols);
                for (std::size_t i = 0; i < n.args.size(); ++i) m.v[i] = scalar(evalNode(*n.args[i], ctx), ctx);
                return Arg::array(std::move(m));
            }
            case FormulaNode::Ref: {
                int s = resolveSheet(ctx, n.ref);
                if (s < 0) return Arg::val(CellValue::error("#REF!"));
                return Arg::range(toRange(n.ref, s));
            }
            case FormulaNode::Unary:
            case FormulaNode::Percent: {
                Arg v = evalNode(*n.args[0], ctx);
                const bool percent = n.kind == FormulaNode::Percent, neg = n.str == "-";
                auto one = [&](const CellValue& x) {
                    double d = toNum(x);
                    return checkNum(percent ? d / 100 : neg ? -d : d);
                };
                if (v.multi()) {
                    Matrix m = toMatrix(v, ctx);
                    for (auto& x : m.v) {
                        const CellValue in = x;
                        x = guarded([&] { return one(in); });
                    }
                    return Arg::array(std::move(m));
                }
                return Arg::val(one(scalar(v, ctx)));
            }
            case FormulaNode::Binary: {
                Arg l = evalNode(*n.args[0], ctx);
                Arg r = evalNode(*n.args[1], ctx);
                if (l.multi() || r.multi()) {
                    const std::string op = n.str;
                    Matrix out = broadcast2(toMatrix(l, ctx), toMatrix(r, ctx), [&](const CellValue& x, const CellValue& y) { return binaryOp(op, x, y).v; });
                    return Arg::array(std::move(out));
                }
                return binaryOp(n.str, scalar(l, ctx), scalar(r, ctx));
            }
            case FormulaNode::Call: {
                const auto& fns = functions();
                auto it = fns.find(n.str);
                if (it == fns.end()) return Arg::val(CellValue::error("#NAME?"));
                if (liftable(n.str)) return liftedCall(it->second, n, ctx);
                return it->second(n.args, ctx);
            }
        }
    } catch (const EvalError& e) {
        return Arg::val(CellValue::error(e.code));
    }
    return Arg::val(CellValue::error("#VALUE!"));
}

const FnMap& functions() {
    static const FnMap f = [] {
        FnMap m;
        addMathFunctions(m);
        addStatFunctions(m);
        addTextFunctions(m);
        addLookupFunctions(m);
        addArrayFunctions(m);
        addDateFunctions(m);
        addFinanceFunctions(m);
        return m;
    }();
    return f;
}

namespace {

CellValue finalValue(const Arg& a, Ctx& ctx) {
    CellValue v;
    try {
        v = scalar(a, ctx);
    } catch (const EvalError& e) {
        v = CellValue::error(e.code);
    }
    if (v.empty()) return CellValue::num(0);
    return v;
}

bool hasUnknownFunction(const FormulaNode& n) {
    if (n.kind == FormulaNode::Call && !functions().count(n.str)) return true;
    for (const auto& a : n.args) {
        if (hasUnknownFunction(*a)) return true;
    }
    return false;
}

// deeper chains are finished from a work stack, not the call stack. Kept low because MSVC's evaluator
// frames are large and Windows threads get 1 MB of stack (150 crashed the Windows tests).
constexpr int kMaxDepth = 40;

}  // namespace

void Recalc::spill(int sheet, std::int64_t key, Cell& cell, const Matrix& m) {
    Sheet& sh = wb.sheets[static_cast<std::size_t>(sheet)];
    const int row = Sheet::rowOf(key), col = Sheet::colOf(key);
    // the previous spill of this formula goes first (unless another formula took the cells this pass)
    for (int i = 0; i < cell.spillRows; ++i) {
        for (int j = 0; j < cell.spillCols; ++j) {
            if (!i && !j) continue;
            auto it = sh.cells.find(Sheet::key(row + i, col + j));
            if (it == sh.cells.end() || !it->second.spilled) continue;
            auto o = owner.find({sheet, it->first});
            if (o != owner.end() && o->second != &cell) continue;
            if (!it->second.value.empty()) spillChanged = true;
            it->second.value = CellValue{};
        }
    }
    const int rows = std::min(m.rows, kMaxRows - row), cols = std::min(m.cols, kMaxCols - col);
    bool blocked = rows < m.rows || cols < m.cols;
    for (int i = 0; i < rows && !blocked; ++i) {
        for (int j = 0; j < cols && !blocked; ++j) {
            if (!i && !j) continue;
            auto it = sh.cells.find(Sheet::key(row + i, col + j));
            if (it == sh.cells.end() || !it->second.hasContent()) continue;
            if (!it->second.spilled) {
                blocked = true;
                break;
            }
            auto o = owner.find({sheet, it->first});
            if (o != owner.end() && o->second != &cell) blocked = true;
        }
    }
    if (blocked) {
        if (cell.spillRows) spillChanged = true;
        cell.value = CellValue::error("#SPILL!");
        cell.spillRows = cell.spillCols = 0;
        return;
    }
    auto fix = [](CellValue v) { return v.empty() ? CellValue::num(0) : v; };
    if (cell.spillRows != rows || cell.spillCols != cols) spillChanged = true;
    cell.value = fix(m.at(0, 0));
    cell.spillRows = rows;
    cell.spillCols = cols;
    ++spills;
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            if (!i && !j) continue;
            const std::int64_t k = Sheet::key(row + i, col + j);
            Cell& t = sh.cells[k];  // inserting never moves the other cells
            CellValue v = fix(m.at(i, j));
            if (!(t.value == v) || !t.spilled) spillChanged = true;
            t.value = std::move(v);
            t.spilled = true;
            owner[{sheet, k}] = &cell;
        }
    }
}

void Recalc::compute(int sheet, std::int64_t key, Cell& cell, int depth) {
    if (depth > kMaxDepth) throw Deferred{&cell, sheet, Sheet::rowOf(key), Sheet::colOf(key)};
    if (!cell.ast) cell.ast = parseFormula(cell.formula);
    state[&cell] = 1;
    // if a Deferred unwinds through here, mark the cell for a retry once the deeper cell is done.
    // A guard instead of catch-and-rethrow: MSVC keeps every rethrowing catch frame on the stack.
    struct RetryOnUnwind {
        int& slot;
        int exceptions = std::uncaught_exceptions();
        ~RetryOnUnwind() {
            if (std::uncaught_exceptions() > exceptions) slot = 0;
        }
    } retry{state[&cell]};
    Ctx ctx{wb, sheet, Sheet::rowOf(key), Sheet::colOf(key), this, depth, nullptr};
    Arg a = evalNode(*cell.ast, ctx);
    if (a.multi()) {
        Matrix m;
        bool ok = true;
        try {
            m = toMatrix(a, ctx);
        } catch (const EvalError& e) {
            cell.value = CellValue::error(e.code);
            ok = false;
        }
        if (ok) spill(sheet, key, cell, m);
    } else {
        if (cell.spillRows) {
            Matrix none(1, 1);
            none.v[0] = finalValue(a, ctx);
            spill(sheet, key, cell, none);  // clears the old spill
            cell.spillRows = cell.spillCols = 0;
            --spills;
        }
        CellValue v = finalValue(a, ctx);
        // a function this engine doesn't know: keep the value the file was saved with
        if (!(v.isError() && v.text == "#NAME?" && !cell.value.empty() && !cell.value.isError() && hasUnknownFunction(*cell.ast))) {
            cell.value = std::move(v);
        }
    }
    state[&cell] = 2;
}

}  // namespace fx

const DefinedName* Workbook::name(const std::string& n) const {
    auto it = names.find(upper(n));
    return it == names.end() ? nullptr : &it->second;
}

bool matchesCriteria(const CellValue& v, const std::string& criteria) { return fx::Criterion(CellValue::str(criteria)).matches(v); }

Workbook::Stats Workbook::recalc() {
    using namespace fx;
    // spilled values whose formula is gone (or no longer covers them) are dropped first
    for (auto& sh : sheets) {
        std::set<std::int64_t> covered;
        bool anySpilled = false;
        for (const auto& [k, c] : sh.cells) {
            anySpilled = anySpilled || c.spilled;
            if (!c.hasFormula() || !c.spillRows) continue;
            for (int i = 0; i < c.spillRows; ++i) {
                for (int j = 0; j < c.spillCols; ++j) covered.insert(Sheet::key(Sheet::rowOf(k) + i, Sheet::colOf(k) + j));
            }
        }
        if (!anySpilled) continue;
        for (auto it = sh.cells.begin(); it != sh.cells.end();) {
            Cell& c = it->second;
            if (c.spilled && (c.hasFormula() || !covered.count(it->first))) {
                c.spilled = false;
                if (!c.hasFormula()) c.value = CellValue{};
                if (c.blank()) {
                    it = sh.cells.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    Stats st;
    std::vector<std::tuple<int, std::int64_t, Cell*>> formulas;
    for (std::size_t s = 0; s < sheets.size(); ++s) {
        for (auto& [k, c] : sheets[s].cells) {
            if (c.hasFormula()) formulas.emplace_back(static_cast<int>(s), k, &c);
        }
    }
    for (int pass = 0; pass < 4; ++pass) {
        Recalc rc{*this, {}, {}, false, 0};
        for (auto& [s, k, cell] : formulas) rc.state[cell] = 0;
        for (auto& [s, k, cell] : formulas) {
            if (rc.state[cell] == 2) continue;
            // a work stack: when a dependency chain gets too deep, compute the deep cell first and retry
            std::vector<Deferred> stack{{cell, s, Sheet::rowOf(k), Sheet::colOf(k)}};
            std::set<Cell*> pending{cell};
            while (!stack.empty()) {
                Deferred d = stack.back();
                if (rc.state[d.cell] == 2) {
                    stack.pop_back();
                    continue;
                }
                try {
                    rc.compute(d.sheet, Sheet::key(d.row, d.col), *d.cell, 0);
                    stack.pop_back();
                } catch (const Deferred& deeper) {
                    if (!pending.insert(deeper.cell).second) {
                        // reached again through a long chain: a cycle
                        deeper.cell->value = CellValue::error("#CYCLE!");
                        rc.state[deeper.cell] = 2;
                        continue;
                    }
                    stack.push_back(deeper);
                }
            }
        }
        st.spills = rc.spills;
        // cells that read spilled values before their formula ran need another pass
        if (!rc.spillChanged) break;
    }
    // spilled cells left empty
    for (auto& sh : sheets) {
        for (auto it = sh.cells.begin(); it != sh.cells.end();) {
            if (it->second.spilled && it->second.value.empty() && it->second.format.empty() && it->second.style.plain()) it = sh.cells.erase(it);
            else ++it;
        }
    }
    st.formulas = static_cast<int>(formulas.size());
    for (auto& [s, k, cell] : formulas) {
        if (cell->value.isError()) {
            ++st.errors;
            if (cell->value.text == "#CYCLE!") ++st.cycles;
        }
    }
    return st;
}

CellValue Workbook::evaluate(const std::string& formula, int sheet, int row, int col) const {
    if (sheet < 0 || sheet >= static_cast<int>(sheets.size())) return CellValue::error("#REF!");
    fx::Ctx ctx{*this, sheet, row, col, nullptr, 0, nullptr};
    auto ast = parseFormula(formula);
    return fx::finalValue(fx::evalNode(*ast, ctx), ctx);
}

std::vector<std::vector<CellValue>> Workbook::evaluateArray(const std::string& formula, int sheet, int row, int col) const {
    if (sheet < 0 || sheet >= static_cast<int>(sheets.size())) return {{CellValue::error("#REF!")}};
    fx::Ctx ctx{*this, sheet, row, col, nullptr, 0, nullptr};
    auto ast = parseFormula(formula);
    fx::Arg a = fx::evalNode(*ast, ctx);
    if (!a.multi()) return {{fx::finalValue(a, ctx)}};
    std::vector<std::vector<CellValue>> out;
    try {
        fx::Matrix m = fx::toMatrix(a, ctx);
        for (int i = 0; i < m.rows; ++i) {
            out.emplace_back();
            for (int j = 0; j < m.cols; ++j) out.back().push_back(m.at(i, j).empty() ? CellValue::num(0) : m.at(i, j));
        }
    } catch (const fx::EvalError& e) {
        return {{CellValue::error(e.code)}};
    }
    return out;
}

std::vector<std::string> functionNames() {
    std::vector<std::string> out;
    for (const auto& [k, v] : fx::functions()) {
        if (k != "ANCHORARRAY" && k != "SINGLE") out.push_back(k);
    }
    return out;
}

bool parseRange(const Workbook& wb, const std::string& text, int defaultSheet, Range& out, std::string& error) {
    std::string t = trimCopy(text);
    if (!t.empty() && t[0] == '=') t = trimCopy(t.substr(1));
    if (t.empty()) {
        error = "empty range";
        return false;
    }
    int sheet = defaultSheet;
    std::string refPart = t;
    auto bang = t.rfind('!');
    if (bang != std::string::npos) {
        std::string name = t.substr(0, bang);
        if (name.size() >= 2 && name.front() == '\'' && name.back() == '\'') {
            std::string unq;
            for (std::size_t i = 1; i + 1 < name.size(); ++i) {
                unq += name[i];
                if (name[i] == '\'' && i + 2 < name.size() && name[i + 1] == '\'') ++i;
            }
            name = unq;
        }
        sheet = wb.sheetIndex(name);
        if (sheet < 0) {
            error = "no sheet called '" + name + "'";
            return false;
        }
        refPart = t.substr(bang + 1);
    }
    if (sheet < 0 || sheet >= static_cast<int>(wb.sheets.size())) {
        error = "no such sheet";
        return false;
    }
    FormulaRef ref;
    std::size_t end = 0;
    if (!tryRef(refPart, 0, ref, end) || end != refPart.size()) {
        // a defined name that points at cells
        if (bang == std::string::npos) {
            if (const DefinedName* dn = wb.name(t)) {
                std::string f = trimCopy(dn->formula);
                if (f != t && f.find('!') != std::string::npos) {
                    std::string inner;
                    if (parseRange(wb, f, defaultSheet, out, inner)) return true;
                }
            }
        }
        error = "'" + text + "' isn't a cell or range (use e.g. B2, A1:C10, A:A, 2:5, Sheet2!A1:B3 or a defined name)";
        return false;
    }
    out = fx::toRange(ref, sheet);
    return true;
}

namespace {

// Functions added to Excel in 2010 or later are stored with an _xlfn. prefix inside .xlsx files.
const std::set<std::string>& futureFunctions() {
    static const std::set<std::string> s = {
        "AGGREGATE", "ANCHORARRAY", "ARABIC", "BITAND", "BITOR", "BITXOR", "CEILING.MATH", "CEILING.PRECISE", "CHOOSECOLS", "CHOOSEROWS",
        "CONCAT", "COT", "COVARIANCE.P", "COVARIANCE.S", "CSC", "DAYS", "DECIMAL", "DROP", "EXPAND", "EXPON.DIST", "FILTER", "FLOOR.MATH",
        "FLOOR.PRECISE", "FORECAST.LINEAR", "FORMULATEXT", "GAMMA", "HSTACK", "IFNA", "IFS", "ISFORMULA", "ISOWEEKNUM", "LET", "MAXIFS",
        "MINIFS", "MODE.MULT", "MODE.SNGL", "NETWORKDAYS.INTL", "NORM.DIST", "NORM.INV", "NORM.S.DIST", "NORM.S.INV", "NUMBERVALUE",
        "PERCENTILE.EXC", "PERCENTILE.INC", "PERCENTRANK.EXC", "PERCENTRANK.INC", "QUARTILE.EXC", "QUARTILE.INC", "RANDARRAY", "RANK.AVG",
        "RANK.EQ", "SEC", "SEQUENCE", "SHEET", "SHEETS", "SINGLE", "SORT", "SORTBY", "STDEV.P", "STDEV.S", "SWITCH", "TAKE", "TEXTAFTER",
        "TEXTBEFORE", "TEXTJOIN", "TEXTSPLIT", "TOCOL", "TOROW", "UNICHAR", "UNICODE", "UNIQUE", "VAR.P", "VAR.S", "VSTACK", "WORKDAY.INTL",
        "WRAPCOLS", "WRAPROWS", "XLOOKUP", "XMATCH", "XOR", "VALUETOTEXT", "ARRAYTOTEXT"};
    return s;
}

}  // namespace

std::string fileFormula(const std::string& formula, bool toFile) {
    if (!toFile) {
        std::string out = formula;
        for (const char* pre : {"_xlfn._xlws.", "_xlfn.", "_xlws."}) {
            std::size_t p;
            while ((p = out.find(pre)) != std::string::npos) out.erase(p, std::char_traits<char>::length(pre));
        }
        return out;
    }
    std::string error;
    std::vector<Tok> toks = tokenize(formula, error);
    if (!error.empty()) return formula;
    std::string out;
    std::size_t last = 0;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        const Tok& t = toks[i];
        if (t.t == Tok::Ref && i + 1 < toks.size() && toks[i + 1].t == Tok::Op && toks[i + 1].text == "#") {
            out += formula.substr(last, t.begin - last) + "_xlfn.ANCHORARRAY(" + formula.substr(t.begin, t.end - t.begin) + ")";
            last = toks[i + 1].end;
            ++i;
            continue;
        }
        if (t.t == Tok::Op && t.text == "@") {
            out += formula.substr(last, t.begin - last);  // implicit intersection is the file's default
            last = t.end;
            continue;
        }
        if (t.t != Tok::Func || !futureFunctions().count(t.text)) continue;
        out += formula.substr(last, t.begin - last) + (t.text == "SORT" || t.text == "FILTER" ? "_xlfn._xlws." : "_xlfn.");
        last = t.begin;
    }
    return out + formula.substr(last);
}

std::vector<Range> formulaReferences(const Workbook& wb, const std::string& formula, int sheet) {
    std::vector<Range> out;
    std::string error;
    for (const auto& t : tokenize(formula, error)) {
        if (t.t != Tok::Ref) continue;
        int s = t.ref.sheet.empty() ? sheet : wb.sheetIndex(t.ref.sheet);
        if (s < 0) continue;
        out.push_back(fx::toRange(t.ref, s));
    }
    return out;
}

}  // namespace wetwaresuite::office
