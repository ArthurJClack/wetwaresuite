// The formula evaluator's insides, shared by formula.cpp (parser, operators,
// recalculation) and the function library files (fn_*.cpp). Not public.
#pragma once

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "wetwaresuite/office/sheet.hpp"

namespace wetwaresuite::office {

struct FormulaNode {
    enum Kind { Num, Str, Bool, Err, Ref, Unary, Binary, Percent, Call, Missing, Name, Array, Value };
    Kind kind = Missing;
    double num = 0;
    std::string str;  // string value, error code, operator, upper-case function name, or a name as written
    FormulaRef ref;
    std::vector<std::shared_ptr<const FormulaNode>> args;  // Array: rows*cols constants, row by row
    int rows = 0, cols = 0;                                 // Array
    CellValue value;                                        // Value: an already evaluated argument
};

namespace fx {

using NodeP = std::shared_ptr<const FormulaNode>;

struct EvalError {
    std::string code;
};

// An array value: rows x cols cells, row by row.
struct Matrix {
    int rows = 0, cols = 0;
    std::vector<CellValue> v;
    Matrix() = default;
    Matrix(int r, int c) : rows(r), cols(c), v(static_cast<std::size_t>(r) * static_cast<std::size_t>(c)) {}
    CellValue& at(int r, int c) { return v[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)]; }
    const CellValue& at(int r, int c) const { return v[static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c)]; }
};
using MatrixP = std::shared_ptr<const Matrix>;

// What an expression evaluates to: a value, a reference to cells, or an array.
struct Arg {
    bool isRange = false;
    CellValue v;
    Range r;
    MatrixP arr;
    bool isArray() const { return arr != nullptr; }
    bool multi() const { return arr ? arr->rows * arr->cols > 1 : isRange && (r.r1 != r.r2 || r.c1 != r.c2); }
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
    static Arg array(Matrix m) {
        Arg a;
        a.arr = std::make_shared<const Matrix>(std::move(m));
        return a;
    }
};

struct Recalc;

struct Ctx {
    const Workbook& wb;
    int sheet = 0, row = 0, col = 0;
    Recalc* rc = nullptr;
    int depth = 0;
    std::vector<std::pair<std::string, Arg>>* locals = nullptr;  // LET names (upper case), innermost last
};

using Fn = std::function<Arg(const std::vector<NodeP>&, Ctx&)>;
using FnMap = std::map<std::string, Fn>;

extern const CellValue kEmpty;

Arg evalNode(const FormulaNode& n, Ctx& ctx);
const CellValue& cellValue(Ctx& ctx, int sheet, int row, int col);
Range clipped(const Ctx& ctx, Range r);  // whole rows/columns trimmed to the used area
CellValue scalar(const Arg& a, Ctx& ctx);  // implicit intersection for ranges, top-left for arrays
bool parseNumber(const std::string& text, double& out);
double toNum(const CellValue& v);
std::string toStr(const CellValue& v);
bool toBool(const CellValue& v);
CellValue checkNum(double v);
int compareValues(const CellValue& a, const CellValue& b);
bool wildcardMatch(const std::string& pat, const std::string& text);
bool lookupEqual(const CellValue& a, const CellValue& b, bool wildcard);
int findIn(const std::vector<CellValue>& list, const CellValue& look, int mode);
double nowSerial();
double roundTo(double v, double digits, int mode);
std::string utf8Substr(const std::string& s, std::size_t start, std::size_t count);
std::size_t utf8Len(const std::string& s);

// The cells of a range or the elements of an array (a value is 1x1). Whole rows/columns are clipped.
Matrix toMatrix(const Arg& a, Ctx& ctx);
// A 1-D list from a single row or column (range or array).
std::vector<CellValue> lineOf(const Arg& a, Ctx& ctx);
// The size an Arg has (whole rows/columns clipped).
void dims(const Arg& a, Ctx& ctx, int& rows, int& cols);

inline Arg evalArg(const NodeP& n, Ctx& ctx) { return evalNode(*n, ctx); }
inline CellValue evalScalar(const NodeP& n, Ctx& ctx) { return scalar(evalNode(*n, ctx), ctx); }
inline double evalNum(const NodeP& n, Ctx& ctx) { return toNum(evalScalar(n, ctx)); }
inline std::string evalStr(const NodeP& n, Ctx& ctx) { return toStr(evalScalar(n, ctx)); }
inline bool evalBool(const NodeP& n, Ctx& ctx) { return toBool(evalScalar(n, ctx)); }
inline bool missing(const std::vector<NodeP>& a, std::size_t i) { return i >= a.size() || a[i]->kind == FormulaNode::Missing; }
inline void need(const std::vector<NodeP>& a, std::size_t lo, std::size_t hi) {
    if (a.size() < lo || a.size() > hi) throw EvalError{"#VALUE!"};
}
inline Arg num(double v) { return Arg::val(checkNum(v)); }
inline Arg str(std::string s) { return Arg::val(CellValue::str(std::move(s))); }
inline Arg boolean(bool b) { return Arg::val(CellValue::boolean(b)); }
Range rangeArg(const NodeP& n, Ctx& ctx);  // the argument must be a reference

// Calls fn(row, col, value) for every non-empty cell in the range, in row-major order.
template <typename F>
void eachCell(Ctx& ctx, const Range& r, F fn) {
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
        const CellValue& v = cellValue(ctx, r.sheet, row, col);  // may compute it; existing map nodes never move
        if (!v.empty()) fn(row, col, v);
        ++it;
    }
}

// Every non-empty value of an Arg: range cells, array elements or the value itself.
// fromRef tells functions like SUM to skip text and booleans that came from cells.
template <typename F>
void eachValue(const Arg& v, Ctx& ctx, F fn) {
    if (v.isRange) {
        eachCell(ctx, v.r, [&](int, int, const CellValue& cv) { fn(cv, true); });
    } else if (v.arr) {
        for (const auto& cv : v.arr->v) {
            if (!cv.empty()) fn(cv, true);
        }
    } else {
        fn(v.v, false);
    }
}

// Number-like values for SUM/AVERAGE/...: ranges and arrays skip text and booleans, direct arguments coerce.
template <typename F>
void eachNumber(const std::vector<NodeP>& args, Ctx& ctx, F fn) {
    for (const auto& a : args) {
        if (a->kind == FormulaNode::Missing) continue;
        Arg v = evalArg(a, ctx);
        eachValue(v, ctx, [&](const CellValue& cv, bool fromRef) {
            if (fromRef) {
                if (cv.type == CellValue::Error) throw EvalError{cv.text};
                if (cv.type == CellValue::Number) fn(cv.number);
            } else {
                fn(toNum(cv));
            }
        });
    }
}

// All numbers of the arguments, in order.
std::vector<double> numbers(const std::vector<NodeP>& args, Ctx& ctx);

// Nodes holding values that are already known (to call one function from another).
NodeP valueNode(CellValue v);
NodeP matrixNode(const Matrix& m);
NodeP missingNode();
// Calls a library function by name.
Arg callFunction(const std::string& name, const std::vector<NodeP>& args, Ctx& ctx);
// The cell a formula is in, when evaluating during a recalculation (for ISFORMULA and friends).
const Cell* cellAt(const Ctx& ctx, int sheet, int row, int col);
// A whole-number argument (truncated toward zero like Excel).
inline int evalInt(const NodeP& n, Ctx& ctx) {
    double d = evalNum(n, ctx);
    if (d > 2e9 || d < -2e9) throw EvalError{"#NUM!"};
    return static_cast<int>(std::trunc(d));
}

// COUNTIF-style criterion.
struct Criterion {
    std::string op = "=";
    CellValue value;
    bool wildcard = false;
    explicit Criterion(const CellValue& c);
    bool matches(const CellValue& v) const;
};

// The function library, one file per family.
void addMathFunctions(FnMap& m);
void addStatFunctions(FnMap& m);
void addTextFunctions(FnMap& m);
void addLookupFunctions(FnMap& m);
void addArrayFunctions(FnMap& m);
void addDateFunctions(FnMap& m);
void addFinanceFunctions(FnMap& m);
const FnMap& functions();

// Functions whose arguments are all single values: given an array or multi-cell range they run once
// per element and return an array (=ROUND(A1:A5, 1) spills five results).
bool liftable(const std::string& name);

}  // namespace fx
}  // namespace wetwaresuite::office
