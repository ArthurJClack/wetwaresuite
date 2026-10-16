// Formula engine: tokenizer, precedence parser, evaluator with Excel's
// coercion rules, the function library, and dependency-ordered recalculation
// (cells are computed on demand, so dependencies always come first; a cell
// that is reached again while it is being computed is a cycle).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "achclwss/office/sheet.hpp"
#include "achclwss/pdf/fonts.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"

namespace achclwss::office {

struct FormulaNode {
    enum Kind { Num, Str, Bool, Err, Ref, Unary, Binary, Percent, Call, Missing };
    Kind kind = Missing;
    double num = 0;
    std::string str;  // string value, error code, operator, or upper-case function name
    FormulaRef ref;
    std::vector<std::shared_ptr<const FormulaNode>> args;
};

namespace {

using NodeP = std::shared_ptr<const FormulaNode>;

// ---- tokens --------------------------------------------------------------------

struct Tok {
    enum T { Num, Str, Bool, Err, Ref, Func, Name, Op, LP, RP, Sep, End };
    T t = End;
    std::string text;
    double num = 0;
    FormulaRef ref;
    std::size_t begin = 0, end = 0;
};

const char* kErrors[] = {"#DIV/0!", "#N/A", "#NAME?", "#NULL!", "#NUM!", "#REF!", "#VALUE!", "#CYCLE!", "#SPILL!", "#CALC!"};

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

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
            out.kind = FormulaRef::CellRef;
            out.r1 = out.r2 = r1;
            out.c1 = out.c2 = c1;
            out.absR1 = out.absR2 = ar1;
            out.absC1 = out.absC2 = ac1;
        }
    } else {
        if (k1 != 0) return false;
        out.kind = FormulaRef::CellRef;
        out.r1 = out.r2 = r1;
        out.c1 = out.c2 = c1;
        out.absR1 = out.absR2 = ar1;
        out.absC1 = out.absC2 = ac1;
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
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
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
        if (c == '(' || c == ')' || c == ',' || c == ';') {
            t.t = c == '(' ? Tok::LP : c == ')' ? Tok::RP : Tok::Sep;
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
        if (std::string("+-*/^&=<>%").find(c) != std::string::npos) {
            t.t = Tok::Op;
            t.text = std::string(1, c);
            t.end = p = p + 1;
            out.push_back(t);
            continue;
        }
        if (c == '{') return fail("array constants {...} aren't supported");
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
        return postfix();
    }
    NodeP postfix() {
        NodeP p = primary();
        while (isOp("%")) {
            ++i;
            FormulaNode n;
            n.kind = FormulaNode::Percent;
            n.args = {p};
            p = make(std::move(n));
        }
        return p;
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
                n.kind = FormulaNode::Err;  // unknown names evaluate to #NAME?
                n.str = "#NAME?";
                return make(std::move(n));
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

namespace {

struct EvalError {
    std::string code;
};

struct Arg {
    bool isRange = false;
    CellValue v;
    Range r;
    static Arg val(CellValue v) {
        Arg a;
        a.v = std::move(v);
        return a;
    }
    static Arg range(Range r) {
        Arg a;
        a.isRange = true;
        a.r = r;
        return a;
    }
};

struct Recalc;

struct Ctx {
    const Workbook& wb;
    int sheet = 0, row = 0, col = 0;
    Recalc* rc = nullptr;
    int depth = 0;
};

struct Deferred {
    Cell* cell;
    int sheet, row, col;
};

struct Recalc {
    Workbook& wb;
    std::unordered_map<const Cell*, int> state;  // 0 dirty, 1 computing, 2 done
    int cycles = 0;
    void compute(int sheet, std::int64_t key, Cell& cell, int depth);
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

Arg evalNode(const FormulaNode& n, Ctx& ctx);

const CellValue kEmpty;

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

// Calls fn(row, col, value) for every non-empty cell in the range, in row-major order.
template <typename Fn>
void eachCell(Ctx& ctx, const Range& r, Fn fn) {
    const Sheet& sh = ctx.wb.sheets[static_cast<std::size_t>(r.sheet)];
    auto it = sh.cells.lower_bound(Sheet::key(r.r1, r.c1));
    const auto stop = Sheet::key(r.r2, r.c2);
    while (it != sh.cells.end() && it->first <= stop) {
        const int row = Sheet::rowOf(it->first), col = Sheet::colOf(it->first);
        if (col < r.c1) {
            it = sh.cells.lower_bound(Sheet::key(row, r.c1));
            continue;
        }
        if (col > r.c2) {
            it = sh.cells.lower_bound(Sheet::key(row + 1, r.c1));
            continue;
        }
        const CellValue& v = cellValue(ctx, r.sheet, row, col);  // may compute it; the map itself never changes
        if (!v.empty()) fn(row, col, v);
        ++it;
    }
}

// Range clipped to the used area, so whole-column ranges stay cheap.
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

// A single value from an operand: ranges use implicit intersection with the formula's row/column.
CellValue scalar(const Arg& a, Ctx& ctx) {
    if (!a.isRange) return a.v;
    const Range& r = a.r;
    if (r.r1 == r.r2 && r.c1 == r.c2) return cellValue(ctx, r.sheet, r.r1, r.c1);
    if (r.c1 == r.c2 && ctx.row >= r.r1 && ctx.row <= r.r2) return cellValue(ctx, r.sheet, ctx.row, r.c1);
    if (r.r1 == r.r2 && ctx.col >= r.c1 && ctx.col <= r.c2) return cellValue(ctx, r.sheet, r.r1, ctx.col);
    return CellValue::error("#VALUE!");
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

Arg evalArg(const NodeP& n, Ctx& ctx) { return evalNode(*n, ctx); }
CellValue evalScalar(const NodeP& n, Ctx& ctx) { return scalar(evalNode(*n, ctx), ctx); }
double evalNum(const NodeP& n, Ctx& ctx) { return toNum(evalScalar(n, ctx)); }
std::string evalStr(const NodeP& n, Ctx& ctx) { return toStr(evalScalar(n, ctx)); }
bool evalBool(const NodeP& n, Ctx& ctx) { return toBool(evalScalar(n, ctx)); }
bool missing(const std::vector<NodeP>& a, std::size_t i) { return i >= a.size() || a[i]->kind == FormulaNode::Missing; }

// Number-like values for SUM/AVERAGE/...: ranges skip text and booleans, direct arguments coerce.
template <typename Fn>
void eachNumber(const std::vector<NodeP>& args, Ctx& ctx, Fn fn) {
    for (const auto& a : args) {
        if (a->kind == FormulaNode::Missing) continue;
        Arg v = evalArg(a, ctx);
        if (v.isRange) {
            eachCell(ctx, v.r, [&](int, int, const CellValue& cv) {
                if (cv.type == CellValue::Error) throw EvalError{cv.text};
                if (cv.type == CellValue::Number) fn(cv.number);
            });
        } else {
            fn(toNum(v.v));
        }
    }
}

// ---- criteria (SUMIF, COUNTIF, AVERAGEIF) -----------------------------------------

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

struct Criterion {
    std::string op = "=";
    CellValue value;
    bool wildcard = false;

    explicit Criterion(const CellValue& c) {
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
        if (parseNumber(s, d)) value = CellValue::num(d);
        else if (upper(s) == "TRUE" || upper(s) == "FALSE") value = CellValue::boolean(upper(s) == "TRUE");
        else {
            value = s.empty() ? CellValue{} : CellValue::str(s);
            wildcard = s.find_first_of("*?") != std::string::npos;
        }
    }

    bool matches(const CellValue& v) const {
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
};

// Cells of `r` at the same offset as (row, col) in `base`.
const CellValue& sameOffset(Ctx& ctx, const Range& base, const Range& other, int row, int col) {
    const int rr = other.r1 + (row - base.r1), cc = other.c1 + (col - base.c1);
    if (rr >= kMaxRows || cc >= kMaxCols) return kEmpty;
    return cellValue(ctx, other.sheet, rr, cc);
}

Arg conditional(const std::vector<NodeP>& a, Ctx& ctx, int mode) {  // 0 sum, 1 count, 2 average
    if (a.size() < 2 || (mode == 1 && a.size() != 2) || a.size() > 3) throw EvalError{"#VALUE!"};
    Arg rangeArg = evalArg(a[0], ctx);
    if (!rangeArg.isRange) throw EvalError{"#VALUE!"};
    Criterion crit(evalScalar(a[1], ctx));
    Range base = rangeArg.r;
    Range target = base;
    if (mode != 1 && !missing(a, 2)) {
        Arg t = evalArg(a[2], ctx);
        if (!t.isRange) throw EvalError{"#VALUE!"};
        target = t.r;
    }
    double sum = 0;
    int count = 0;
    auto consider = [&](int row, int col, const CellValue& v) {
        if (!crit.matches(v)) return;
        if (mode == 1) {
            ++count;
            return;
        }
        const CellValue& tv = sameOffset(ctx, base, target, row, col);
        if (tv.type == CellValue::Number) {
            sum += tv.number;
            ++count;
        }
    };
    if (crit.value.empty() || (crit.op == "<>" && !crit.value.empty())) {
        // blanks can match: walk every cell of the (clipped) range
        Range c = clipped(ctx, base);
        for (int r = c.r1; r <= c.r2; ++r) {
            for (int col = c.c1; col <= c.c2; ++col) consider(r, col, cellValue(ctx, c.sheet, r, col));
        }
    } else {
        eachCell(ctx, base, consider);
    }
    if (mode == 0) return Arg::val(CellValue::num(sum));
    if (mode == 1) return Arg::val(CellValue::num(count));
    if (count == 0) throw EvalError{"#DIV/0!"};
    return Arg::val(CellValue::num(sum / count));
}

// SUMIFS/AVERAGEIFS/MAXIFS/MINIFS(target, range1, criteria1, ...) and COUNTIFS(range1, criteria1, ...).
Arg multiConditional(const std::vector<NodeP>& a, Ctx& ctx, int mode) {  // 0 sum, 1 count, 2 average, 3 max, 4 min
    const std::size_t first = mode == 1 ? 0 : 1;
    if (a.size() < first + 2 || (a.size() - first) % 2 != 0) throw EvalError{"#VALUE!"};
    std::vector<Range> ranges;
    std::vector<Criterion> crits;
    for (std::size_t i = first; i < a.size(); i += 2) {
        Arg r = evalArg(a[i], ctx);
        if (!r.isRange) throw EvalError{"#VALUE!"};
        ranges.push_back(r.r);
        crits.emplace_back(evalScalar(a[i + 1], ctx));
    }
    Range target = ranges[0];
    if (mode != 1) {
        Arg t = evalArg(a[0], ctx);
        if (!t.isRange) throw EvalError{"#VALUE!"};
        target = t.r;
    }
    for (const auto& r : ranges) {
        if (r.rows() != target.rows() || r.cols() != target.cols()) throw EvalError{"#VALUE!"};
    }
    // walk only as far as any of the ranges has data
    int rows = 1, cols = 1;
    auto extend = [&](const Range& r) {
        Range c = clipped(ctx, r);
        rows = std::max(rows, c.rows());
        cols = std::max(cols, c.cols());
    };
    extend(target);
    for (const auto& r : ranges) extend(r);
    const Range& base = ranges[0];
    double acc = 0;
    int count = 0;
    for (int dr = 0; dr < rows; ++dr) {
        for (int dc = 0; dc < cols; ++dc) {
            const int row = base.r1 + dr, col = base.c1 + dc;
            bool all = true;
            for (std::size_t k = 0; k < ranges.size() && all; ++k) all = crits[k].matches(sameOffset(ctx, base, ranges[k], row, col));
            if (!all) continue;
            if (mode == 1) {
                ++count;
                continue;
            }
            const CellValue& tv = sameOffset(ctx, base, target, row, col);
            if (tv.type != CellValue::Number) continue;
            if (mode == 3) acc = count ? std::max(acc, tv.number) : tv.number;
            else if (mode == 4) acc = count ? std::min(acc, tv.number) : tv.number;
            else acc += tv.number;
            ++count;
        }
    }
    if (mode == 1) return Arg::val(CellValue::num(count));
    if (mode == 2) {
        if (count == 0) throw EvalError{"#DIV/0!"};
        return Arg::val(CellValue::num(acc / count));
    }
    return Arg::val(CellValue::num(acc));
}

// ---- lookups ------------------------------------------------------------------------

bool lookupEqual(const CellValue& a, const CellValue& b, bool wildcard) {
    if (a.type == CellValue::Text && b.type == CellValue::Text) {
        return wildcard ? wildcardMatch(a.text, b.text) : lower(a.text) == lower(b.text);
    }
    if (a.type != b.type) return false;
    return a.number == b.number;
}

// Index (0-based) of `look` in a 1-D list of values. mode: 0 exact, 1 largest <= (sorted ascending),
// -1 smallest >= (sorted descending), 2 exact with wildcards; for XLOOKUP's next-smaller/larger see below.
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

std::vector<CellValue> line(Ctx& ctx, const Range& r) {
    std::vector<CellValue> out;
    Range c = clipped(ctx, r);
    if (r.r1 == r.r2 || (r.c1 != r.c2 && r.rows() < r.cols())) {
        for (int col = r.c1; col <= c.c2; ++col) out.push_back(cellValue(ctx, r.sheet, r.r1, col));
    } else {
        for (int row = r.r1; row <= c.r2; ++row) out.push_back(cellValue(ctx, r.sheet, row, r.c1));
    }
    return out;
}

Range rangeArg(const NodeP& n, Ctx& ctx) {
    Arg a = evalArg(n, ctx);
    if (!a.isRange) {
        if (a.v.isError()) throw EvalError{a.v.text};
        throw EvalError{"#VALUE!"};
    }
    return a.r;
}

// ---- text helpers ---------------------------------------------------------------------

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

using Fn = std::function<Arg(const std::vector<NodeP>&, Ctx&)>;

void need(const std::vector<NodeP>& a, std::size_t lo, std::size_t hi) {
    if (a.size() < lo || a.size() > hi) throw EvalError{"#VALUE!"};
}

Arg num(double v) { return Arg::val(checkNum(v)); }
Arg str(std::string s) { return Arg::val(CellValue::str(std::move(s))); }
Arg boolean(bool b) { return Arg::val(CellValue::boolean(b)); }

const std::map<std::string, Fn>& functions() {
    static const std::map<std::string, Fn> f = [] {
        std::map<std::string, Fn> m;
        // -- math and statistics
        m["SUM"] = [](const auto& a, Ctx& c) {
            double s = 0;
            eachNumber(a, c, [&](double v) { s += v; });
            return num(s);
        };
        m["PRODUCT"] = [](const auto& a, Ctx& c) {
            double s = 1;
            eachNumber(a, c, [&](double v) { s *= v; });
            return num(s);
        };
        m["AVERAGE"] = [](const auto& a, Ctx& c) {
            double s = 0;
            int n = 0;
            eachNumber(a, c, [&](double v) {
                s += v;
                ++n;
            });
            if (!n) throw EvalError{"#DIV/0!"};
            return num(s / n);
        };
        m["MIN"] = [](const auto& a, Ctx& c) {
            double s = 0;
            bool any = false;
            eachNumber(a, c, [&](double v) {
                s = any ? std::min(s, v) : v;
                any = true;
            });
            return num(s);
        };
        m["MAX"] = [](const auto& a, Ctx& c) {
            double s = 0;
            bool any = false;
            eachNumber(a, c, [&](double v) {
                s = any ? std::max(s, v) : v;
                any = true;
            });
            return num(s);
        };
        m["MEDIAN"] = [](const auto& a, Ctx& c) {
            std::vector<double> v;
            eachNumber(a, c, [&](double x) { v.push_back(x); });
            if (v.empty()) throw EvalError{"#NUM!"};
            std::sort(v.begin(), v.end());
            const std::size_t n = v.size();
            return num(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
        };
        m["COUNT"] = [](const auto& a, Ctx& c) {
            int n = 0;
            for (const auto& x : a) {
                if (x->kind == FormulaNode::Missing) continue;
                Arg v = evalArg(x, c);
                if (v.isRange) {
                    eachCell(c, v.r, [&](int, int, const CellValue& cv) { n += cv.type == CellValue::Number ? 1 : 0; });
                } else if (v.v.type == CellValue::Number || v.v.type == CellValue::Bool) {
                    ++n;
                } else if (v.v.type == CellValue::Text) {
                    double d;
                    n += parseNumber(v.v.text, d) ? 1 : 0;
                }
            }
            return num(n);
        };
        m["COUNTA"] = [](const auto& a, Ctx& c) {
            int n = 0;
            for (const auto& x : a) {
                if (x->kind == FormulaNode::Missing) continue;
                Arg v = evalArg(x, c);
                if (v.isRange) eachCell(c, v.r, [&](int, int, const CellValue&) { ++n; });
                else n += v.v.empty() ? 0 : 1;
            }
            return num(n);
        };
        m["COUNTBLANK"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            Range r = rangeArg(a[0], c);
            double total = static_cast<double>(r.rows()) * r.cols();
            int filled = 0;
            eachCell(c, r, [&](int, int, const CellValue& v) { filled += (v.type == CellValue::Text && v.text.empty()) ? 0 : 1; });
            return num(total - filled);
        };
        m["ROUND"] = [](const auto& a, Ctx& c) {
            need(a, 1, 2);
            return num(roundTo(evalNum(a[0], c), missing(a, 1) ? 0 : evalNum(a[1], c), 0));
        };
        m["ROUNDUP"] = [](const auto& a, Ctx& c) {
            need(a, 1, 2);
            return num(roundTo(evalNum(a[0], c), missing(a, 1) ? 0 : evalNum(a[1], c), 1));
        };
        m["ROUNDDOWN"] = [](const auto& a, Ctx& c) {
            need(a, 1, 2);
            return num(roundTo(evalNum(a[0], c), missing(a, 1) ? 0 : evalNum(a[1], c), -1));
        };
        m["ABS"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return num(std::abs(evalNum(a[0], c)));
        };
        m["SQRT"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            double v = evalNum(a[0], c);
            if (v < 0) throw EvalError{"#NUM!"};
            return num(std::sqrt(v));
        };
        m["POWER"] = [](const auto& a, Ctx& c) {
            need(a, 2, 2);
            double b = evalNum(a[0], c), e = evalNum(a[1], c);
            if (b == 0 && e < 0) throw EvalError{"#DIV/0!"};
            return num(std::pow(b, e));
        };
        m["MOD"] = [](const auto& a, Ctx& c) {
            need(a, 2, 2);
            double n = evalNum(a[0], c), d = evalNum(a[1], c);
            if (d == 0) throw EvalError{"#DIV/0!"};
            return num(n - d * std::floor(n / d));
        };
        m["INT"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return num(std::floor(evalNum(a[0], c)));
        };
        m["PI"] = [](const auto& a, Ctx&) {
            need(a, 0, 0);
            return num(3.14159265358979323846);
        };
        // -- logic
        m["IF"] = [](const auto& a, Ctx& c) {
            need(a, 1, 3);
            bool cond = evalBool(a[0], c);
            if (cond) return missing(a, 1) ? (a.size() > 1 ? num(0) : boolean(true)) : evalArg(a[1], c);
            if (a.size() < 3) return boolean(false);
            return missing(a, 2) ? num(0) : evalArg(a[2], c);
        };
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
            try {
                v = evalScalar(a[0], c);
            } catch (const EvalError&) {
                return evalArg(a[1], c);
            }
            if (v.isError()) return evalArg(a[1], c);
            return Arg::val(v);
        };
        auto logical = [](const std::vector<NodeP>& a, Ctx& c, bool isAnd) {
            if (a.empty()) throw EvalError{"#VALUE!"};
            bool acc = isAnd, any = false;
            for (const auto& x : a) {
                Arg v = evalArg(x, c);
                auto take = [&](bool b) {
                    acc = isAnd ? (acc && b) : (acc || b);
                    any = true;
                };
                if (v.isRange) {
                    eachCell(c, v.r, [&](int, int, const CellValue& cv) {
                        if (cv.isError()) throw EvalError{cv.text};
                        if (cv.type == CellValue::Number || cv.type == CellValue::Bool) take(cv.number != 0);
                    });
                } else {
                    take(toBool(v.v));
                }
            }
            if (!any) throw EvalError{"#VALUE!"};
            return boolean(acc);
        };
        m["AND"] = [logical](const auto& a, Ctx& c) { return logical(a, c, true); };
        m["OR"] = [logical](const auto& a, Ctx& c) { return logical(a, c, false); };
        m["NOT"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return boolean(!evalBool(a[0], c));
        };
        m["ISBLANK"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return boolean(evalScalar(a[0], c).empty());
        };
        m["ISNUMBER"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return boolean(evalScalar(a[0], c).type == CellValue::Number);
        };
        m["ISTEXT"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return boolean(evalScalar(a[0], c).type == CellValue::Text);
        };
        m["ISERROR"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            try {
                return boolean(evalScalar(a[0], c).isError());
            } catch (const EvalError&) {
                return boolean(true);
            }
        };
        // -- text
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
        auto concatAll = [](const std::vector<NodeP>& a, Ctx& c, const std::string* delim, bool skipEmpty) {
            std::string out;
            bool first = true;
            auto add = [&](const std::string& s) {
                if (skipEmpty && s.empty()) return;
                if (!first && delim) out += *delim;
                out += s;
                first = false;
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
                } else {
                    add(toStr(v.v));
                }
            }
            return str(out);
        };
        m["CONCAT"] = [concatAll](const auto& a, Ctx& c) { return concatAll(a, c, nullptr, false); };
        m["CONCATENATE"] = m["CONCAT"];
        m["TEXTJOIN"] = [concatAll](const auto& a, Ctx& c) {
            if (a.size() < 3) throw EvalError{"#VALUE!"};
            std::string d = evalStr(a[0], c);
            bool skip = evalBool(a[1], c);
            return concatAll(std::vector<NodeP>(a.begin() + 2, a.end()), c, &d, skip);
        };
        m["SUBSTITUTE"] = [](const auto& a, Ctx& c) {
            need(a, 3, 4);
            std::string s = evalStr(a[0], c), from = evalStr(a[1], c), to = evalStr(a[2], c);
            if (from.empty()) return str(s);
            int which = missing(a, 3) ? 0 : static_cast<int>(evalNum(a[3], c));
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
        m["TEXT"] = [](const auto& a, Ctx& c) {
            need(a, 2, 2);
            return str(formatValue(evalScalar(a[0], c), evalStr(a[1], c)));
        };
        m["VALUE"] = [](const auto& a, Ctx& c) {
            need(a, 1, 1);
            return num(toNum(evalScalar(a[0], c)));
        };
        // -- lookups
        m["VLOOKUP"] = [](const auto& a, Ctx& c) {
            need(a, 3, 4);
            CellValue look = evalScalar(a[0], c);
            if (look.isError()) throw EvalError{look.text};
            Range t = rangeArg(a[1], c);
            int col = static_cast<int>(evalNum(a[2], c));
            bool approx = missing(a, 3) ? true : evalBool(a[3], c);
            if (col < 1) throw EvalError{"#VALUE!"};
            if (col > t.cols()) throw EvalError{"#REF!"};
            Range first = t;
            first.c2 = first.c1;
            int idx = findIn(line(c, first), look, approx ? 1 : 0);
            if (idx < 0) throw EvalError{"#N/A"};
            Range hit{t.sheet, t.r1 + idx, t.c1 + col - 1, t.r1 + idx, t.c1 + col - 1};
            return Arg::range(hit);
        };
        m["HLOOKUP"] = [](const auto& a, Ctx& c) {
            need(a, 3, 4);
            CellValue look = evalScalar(a[0], c);
            Range t = rangeArg(a[1], c);
            int row = static_cast<int>(evalNum(a[2], c));
            bool approx = missing(a, 3) ? true : evalBool(a[3], c);
            if (row < 1) throw EvalError{"#VALUE!"};
            if (row > t.rows()) throw EvalError{"#REF!"};
            Range first = t;
            first.r2 = first.r1;
            int idx = findIn(line(c, first), look, approx ? 1 : 0);
            if (idx < 0) throw EvalError{"#N/A"};
            return Arg::range(Range{t.sheet, t.r1 + row - 1, t.c1 + idx, t.r1 + row - 1, t.c1 + idx});
        };
        m["MATCH"] = [](const auto& a, Ctx& c) {
            need(a, 2, 3);
            CellValue look = evalScalar(a[0], c);
            if (look.isError()) throw EvalError{look.text};
            Range r = rangeArg(a[1], c);
            int type = missing(a, 2) ? 1 : static_cast<int>(evalNum(a[2], c));
            if (r.rows() > 1 && r.cols() > 1) throw EvalError{"#N/A"};
            int idx = findIn(line(c, r), look, type == 0 ? 0 : type > 0 ? 1 : -1);
            if (idx < 0) throw EvalError{"#N/A"};
            return num(idx + 1);
        };
        m["INDEX"] = [](const auto& a, Ctx& c) {
            need(a, 2, 3);
            Range r = rangeArg(a[0], c);
            int row = static_cast<int>(evalNum(a[1], c));
            int col = missing(a, 2) ? 0 : static_cast<int>(evalNum(a[2], c));
            if (row < 0 || col < 0) throw EvalError{"#VALUE!"};
            if (missing(a, 2) && r.rows() == 1 && r.cols() > 1) {  // one index into a single row
                col = row;
                row = 1;
            }
            if (row > r.rows() || col > r.cols()) throw EvalError{"#REF!"};
            Range out = r;
            if (row > 0) out.r1 = out.r2 = r.r1 + row - 1;
            if (col > 0) out.c1 = out.c2 = r.c1 + col - 1;
            else if (r.cols() == 1) out.c2 = out.c1;
            return Arg::range(out);
        };
        m["XLOOKUP"] = [](const auto& a, Ctx& c) {
            need(a, 3, 6);
            CellValue look = evalScalar(a[0], c);
            if (look.isError()) throw EvalError{look.text};
            Range lr = rangeArg(a[1], c);
            Range rr = rangeArg(a[2], c);
            int mode = missing(a, 4) ? 0 : static_cast<int>(evalNum(a[4], c));
            int search = missing(a, 5) ? 1 : static_cast<int>(evalNum(a[5], c));
            std::vector<CellValue> list = line(c, lr);
            const bool vertical = !(lr.r1 == lr.r2 && lr.c1 != lr.c2);
            int idx = -1;
            auto order = [&](std::size_t k) { return search < 0 ? list.size() - 1 - k : k; };
            if (mode == 0 || mode == 2) {
                const bool wild = mode == 2;
                for (std::size_t k = 0; k < list.size() && idx < 0; ++k) {
                    if (lookupEqual(look, list[order(k)], wild)) idx = static_cast<int>(order(k));
                }
            } else {
                // exact, else the next smaller (-1) or next larger (1) value; data needn't be sorted
                int best = -1;
                for (std::size_t k = 0; k < list.size(); ++k) {
                    const std::size_t i = order(k);
                    const CellValue& v = list[i];
                    if (v.empty() || v.isError() || (v.type == CellValue::Text) != (look.type == CellValue::Text)) continue;
                    int cmp = compareValues(v, look);
                    if (cmp == 0) {
                        best = static_cast<int>(i);
                        break;
                    }
                    if ((mode < 0 && cmp < 0) || (mode > 0 && cmp > 0)) {
                        if (best < 0 || (mode < 0 ? compareValues(v, list[static_cast<std::size_t>(best)]) > 0
                                                  : compareValues(v, list[static_cast<std::size_t>(best)]) < 0)) {
                            best = static_cast<int>(i);
                        }
                    }
                }
                idx = best;
            }
            if (idx < 0) {
                if (!missing(a, 3)) return evalArg(a[3], c);
                throw EvalError{"#N/A"};
            }
            Range out = rr;
            if (vertical) out.r1 = out.r2 = rr.r1 + idx;
            else out.c1 = out.c2 = rr.c1 + idx;
            if (out.r1 >= kMaxRows || out.c1 >= kMaxCols) throw EvalError{"#REF!"};
            return Arg::range(out);
        };
        m["SUMIF"] = [](const auto& a, Ctx& c) { return conditional(a, c, 0); };
        m["COUNTIF"] = [](const auto& a, Ctx& c) { return conditional(a, c, 1); };
        m["AVERAGEIF"] = [](const auto& a, Ctx& c) { return conditional(a, c, 2); };
        m["SUMIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 0); };
        m["COUNTIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 1); };
        m["AVERAGEIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 2); };
        m["MAXIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 3); };
        m["MINIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 4); };
        // -- dates
        m["TODAY"] = [](const auto& a, Ctx&) {
            need(a, 0, 0);
            return num(std::floor(nowSerial()));
        };
        m["NOW"] = [](const auto& a, Ctx&) {
            need(a, 0, 0);
            return num(nowSerial());
        };
        m["DATE"] = [](const auto& a, Ctx& c) {
            need(a, 3, 3);
            int y = static_cast<int>(evalNum(a[0], c)), mo = static_cast<int>(evalNum(a[1], c)), d = static_cast<int>(evalNum(a[2], c));
            if (y < 1900) y += 1900;
            // normalise month overflow, then let the day count roll over
            y += (mo - 1) >= 0 ? (mo - 1) / 12 : -((12 - mo) / 12);
            mo = ((mo - 1) % 12 + 12) % 12 + 1;
            double s = dateSerial(y, mo, 1) + d - 1;
            if (s < 0) throw EvalError{"#NUM!"};
            return num(s);
        };
        auto datePart = [](int which) {
            return [which](const std::vector<NodeP>& a, Ctx& c) {
                need(a, 1, 1);
                double s = evalNum(a[0], c);
                if (s < 0) throw EvalError{"#NUM!"};
                int y, m, d;
                serialDate(s, y, m, d);
                return num(which == 0 ? y : which == 1 ? m : d);
            };
        };
        m["YEAR"] = datePart(0);
        m["MONTH"] = datePart(1);
        m["DAY"] = datePart(2);
        m["ROW"] = [](const auto& a, Ctx& c) {
            need(a, 0, 1);
            if (a.empty() || missing(a, 0)) return num(c.row + 1);
            return num(rangeArg(a[0], c).r1 + 1);
        };
        m["COLUMN"] = [](const auto& a, Ctx& c) {
            need(a, 0, 1);
            if (a.empty() || missing(a, 0)) return num(c.col + 1);
            return num(rangeArg(a[0], c).c1 + 1);
        };
        return m;
    }();
    return f;
}

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

Arg evalNode(const FormulaNode& n, Ctx& ctx) {
    try {
        switch (n.kind) {
            case FormulaNode::Num: return Arg::val(CellValue::num(n.num));
            case FormulaNode::Str: return Arg::val(CellValue::str(n.str));
            case FormulaNode::Bool: return Arg::val(CellValue::boolean(n.num != 0));
            case FormulaNode::Err: return Arg::val(CellValue::error(n.str));
            case FormulaNode::Missing: return Arg::val(CellValue{});
            case FormulaNode::Ref: {
                int s = resolveSheet(ctx, n.ref);
                if (s < 0) return Arg::val(CellValue::error("#REF!"));
                return Arg::range(toRange(n.ref, s));
            }
            case FormulaNode::Unary: {
                double v = toNum(scalar(evalNode(*n.args[0], ctx), ctx));
                return num(n.str == "-" ? -v : v);
            }
            case FormulaNode::Percent: return num(toNum(scalar(evalNode(*n.args[0], ctx), ctx)) / 100);
            case FormulaNode::Binary: {
                CellValue l = scalar(evalNode(*n.args[0], ctx), ctx);
                CellValue r = scalar(evalNode(*n.args[1], ctx), ctx);
                return binaryOp(n.str, l, r);
            }
            case FormulaNode::Call: {
                const auto& fns = functions();
                auto it = fns.find(n.str);
                if (it == fns.end()) return Arg::val(CellValue::error("#NAME?"));
                return it->second(n.args, ctx);
            }
        }
    } catch (const EvalError& e) {
        return Arg::val(CellValue::error(e.code));
    }
    return Arg::val(CellValue::error("#VALUE!"));
}

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

constexpr int kMaxDepth = 150;  // deeper chains are finished from a work stack, not the call stack

void Recalc::compute(int sheet, std::int64_t key, Cell& cell, int depth) {
    if (depth > kMaxDepth) throw Deferred{&cell, sheet, Sheet::rowOf(key), Sheet::colOf(key)};
    if (!cell.ast) cell.ast = parseFormula(cell.formula);
    state[&cell] = 1;
    Ctx ctx{wb, sheet, Sheet::rowOf(key), Sheet::colOf(key), this, depth};
    try {
        Arg a = evalNode(*cell.ast, ctx);
        CellValue v = finalValue(a, ctx);
        // a function this engine doesn't know: keep the value the file was saved with
        if (!(v.isError() && v.text == "#NAME?" && !cell.value.empty() && !cell.value.isError() && hasUnknownFunction(*cell.ast))) {
            cell.value = std::move(v);
        }
    } catch (const Deferred&) {
        state[&cell] = 0;  // retried once the deeper cell is done
        throw;
    }
    state[&cell] = 2;
}

}  // namespace

bool matchesCriteria(const CellValue& v, const std::string& criteria) { return Criterion(CellValue::str(criteria)).matches(v); }

Workbook::Stats Workbook::recalc() {
    Recalc rc{*this, {}, 0};
    std::vector<std::tuple<int, std::int64_t, Cell*>> formulas;
    for (std::size_t s = 0; s < sheets.size(); ++s) {
        for (auto& [k, c] : sheets[s].cells) {
            if (c.hasFormula()) {
                formulas.emplace_back(static_cast<int>(s), k, &c);
                rc.state[&c] = 0;
            }
        }
    }
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
    Stats st;
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
    Ctx ctx{*this, sheet, row, col, nullptr, 0};
    auto ast = parseFormula(formula);
    return finalValue(evalNode(*ast, ctx), ctx);
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
        error = "'" + text + "' isn't a cell or range (use e.g. B2, A1:C10, A:A, 2:5 or Sheet2!A1:B3)";
        return false;
    }
    out = toRange(ref, sheet);
    return true;
}

std::string fileFormula(const std::string& formula, bool toFile) {
    // Newer Excel functions are stored with an _xlfn. prefix inside .xlsx files.
    static const std::set<std::string> future = {"IFS", "CONCAT", "TEXTJOIN", "XLOOKUP", "MAXIFS", "MINIFS", "SWITCH"};
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
    for (const auto& t : toks) {
        if (t.t != Tok::Func || !future.count(t.text)) continue;
        out += formula.substr(last, t.begin - last) + "_xlfn.";
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
        out.push_back(toRange(t.ref, s));
    }
    return out;
}

}  // namespace achclwss::office
