// Formula functions: arithmetic, rounding, trigonometry, SUMPRODUCT, SUBTOTAL
// and AGGREGATE.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <set>

#include "formula_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office::fx {

bool liftable(const std::string& name) {
    static const std::set<std::string> s = {
        // math
        "ROUND", "ROUNDUP", "ROUNDDOWN", "ABS", "SQRT", "POWER", "MOD", "INT", "MROUND", "CEILING", "CEILING.MATH", "CEILING.PRECISE",
        "FLOOR", "FLOOR.MATH", "FLOOR.PRECISE", "TRUNC", "SIGN", "EXP", "LN", "LOG", "LOG10", "FACT", "FACTDOUBLE", "COMBIN", "COMBINA",
        "PERMUT", "EVEN", "ODD", "QUOTIENT", "SIN", "COS", "TAN", "ASIN", "ACOS", "ATAN", "ATAN2", "SINH", "COSH", "TANH", "DEGREES",
        "RADIANS", "SQRTPI", "ROMAN", "ARABIC", "BASE", "DECIMAL", "DEC2BIN", "BIN2DEC", "DEC2HEX", "HEX2DEC",
        // text
        "LEN", "LEFT", "RIGHT", "MID", "UPPER", "LOWER", "PROPER", "TRIM", "CLEAN", "SUBSTITUTE", "REPLACE", "REPT", "FIND", "SEARCH",
        "EXACT", "CHAR", "CODE", "UNICHAR", "UNICODE", "TEXT", "VALUE", "NUMBERVALUE", "FIXED", "DOLLAR", "TEXTBEFORE", "TEXTAFTER",
        // information and logic
        "ISBLANK", "ISNUMBER", "ISTEXT", "ISERROR", "ISERR", "ISNA", "ISLOGICAL", "ISNONTEXT", "ISEVEN", "ISODD", "N", "T", "NOT",
        "IFERROR", "IFNA", "ERROR.TYPE",
        // dates
        "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND", "WEEKDAY", "WEEKNUM", "ISOWEEKNUM", "DATE", "TIME", "EDATE", "EOMONTH",
        "DATEVALUE", "TIMEVALUE", "DAYS", "DATEDIF", "YEARFRAC", "DAYS360",
        // statistics on single values
        "NORM.DIST", "NORMDIST", "NORM.INV", "NORMINV", "NORM.S.DIST", "NORMSDIST", "NORM.S.INV", "NORMSINV", "STANDARDIZE"};
    return s.count(name) > 0;
}

namespace {

double factorial(double n) {
    if (n < 0) throw EvalError{"#NUM!"};
    n = std::floor(n);
    if (n > 170) throw EvalError{"#NUM!"};
    double r = 1;
    for (int i = 2; i <= static_cast<int>(n); ++i) r *= i;
    return r;
}

double combin(double n, double k) {
    n = std::floor(n);
    k = std::floor(k);
    if (n < 0 || k < 0 || k > n) throw EvalError{"#NUM!"};
    double r = 1;
    k = std::min(k, n - k);
    for (int i = 1; i <= static_cast<int>(k); ++i) r = r * (n - k + i) / i;
    return std::round(r);
}

// CEILING/FLOOR to a multiple, Excel style: the sign of the multiple must suit the number.
double toMultiple(double x, double sig, bool up) {
    if (sig == 0) return 0;
    if (x > 0 && sig < 0) throw EvalError{"#NUM!"};
    const double q = x / sig;
    const double nudged = std::round(q * 1e9) / 1e9;
    const double qq = std::abs(nudged - q) < 1e-9 ? nudged : q;
    return (up ? std::ceil(qq) : std::floor(qq)) * sig;
}

std::mt19937_64& rng() {
    static std::mt19937_64 g{std::random_device{}()};
    return g;
}

using Unary = double (*)(double);

Fn unaryMath(Unary f, bool positive = false, bool unit = false) {
    return [f, positive, unit](const std::vector<NodeP>& a, Ctx& c) {
        need(a, 1, 1);
        const double x = evalNum(a[0], c);
        if (positive && x <= 0) throw EvalError{"#NUM!"};
        if (unit && (x < -1 || x > 1)) throw EvalError{"#NUM!"};
        return num(f(x));
    };
}

// Values a SUBTOTAL/AGGREGATE call looks at: hidden rows and nested SUBTOTAL/AGGREGATE cells left out.
NodeP visibleOnly(const NodeP& n, Ctx& c, bool skipHidden, bool skipErrors) {
    Arg v = evalArg(n, c);
    if (!v.isRange) {
        if (!skipErrors) return n;
        Matrix m = toMatrix(v, c);
        for (auto& x : m.v) {
            if (x.isError()) x = CellValue{};
        }
        return matrixNode(m);
    }
    Range r = clipped(c, v.r);
    Matrix m(r.rows(), r.cols());
    const Sheet& sh = c.wb.sheets[static_cast<std::size_t>(r.sheet)];
    eachCell(c, r, [&](int row, int col, const CellValue& cv) {
        if (skipHidden && sh.hiddenRows.count(row)) return;
        const Cell* cell = sh.get(row, col);
        if (cell && cell->hasFormula()) {
            const std::string f = upper(cell->formula);
            if (f.find("SUBTOTAL(") != std::string::npos || f.find("AGGREGATE(") != std::string::npos) return;
        }
        if (skipErrors && cv.isError()) return;
        m.at(row - r.r1, col - r.c1) = cv;
    });
    return matrixNode(m);
}

const char* subtotalName(int code) {
    static const char* names[] = {"AVERAGE", "COUNT", "COUNTA", "MAX", "MIN", "PRODUCT", "STDEV.S", "STDEV.P", "SUM", "VAR.S", "VAR.P",
                                  "MEDIAN", "MODE.SNGL", "LARGE", "SMALL", "PERCENTILE.INC", "QUARTILE.INC", "PERCENTILE.EXC", "QUARTILE.EXC"};
    if (code < 1 || code > 19) return nullptr;
    return names[code - 1];
}

void addBasics(FnMap& m) {
    m["SUM"] = [](const auto& a, Ctx& c) {
        double s = 0;
        eachNumber(a, c, [&](double v) { s += v; });
        return num(s);
    };
    m["PRODUCT"] = [](const auto& a, Ctx& c) {
        double s = 1;
        bool any = false;
        eachNumber(a, c, [&](double v) {
            s *= v;
            any = true;
        });
        return num(any ? s : 0);
    };
    m["SUMSQ"] = [](const auto& a, Ctx& c) {
        double s = 0;
        eachNumber(a, c, [&](double v) { s += v * v; });
        return num(s);
    };
    m["SUMPRODUCT"] = [](const auto& a, Ctx& c) {
        if (a.empty()) throw EvalError{"#VALUE!"};
        std::vector<Matrix> ms;
        for (const auto& x : a) ms.push_back(toMatrix(evalArg(x, c), c));
        for (const auto& mm : ms) {
            if (mm.rows != ms[0].rows || mm.cols != ms[0].cols) throw EvalError{"#VALUE!"};
        }
        double s = 0;
        for (std::size_t i = 0; i < ms[0].v.size(); ++i) {
            double p = 1;
            for (const auto& mm : ms) {
                const CellValue& v = mm.v[i];
                if (v.isError()) throw EvalError{v.text};
                p *= (v.type == CellValue::Number || v.type == CellValue::Bool) ? v.number : 0;
            }
            s += p;
        }
        return num(s);
    };
    auto rounding = [](int mode) {
        return [mode](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 2);
            return num(roundTo(evalNum(a[0], c), missing(a, 1) ? 0 : evalNum(a[1], c), mode));
        };
    };
    m["ROUND"] = rounding(0);
    m["ROUNDUP"] = rounding(1);
    m["ROUNDDOWN"] = rounding(-1);
    m["TRUNC"] = rounding(-1);
    m["MROUND"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double x = evalNum(a[0], c), mult = evalNum(a[1], c);
        if (mult == 0) return num(0);
        if ((x > 0 && mult < 0) || (x < 0 && mult > 0)) throw EvalError{"#NUM!"};
        return num(roundTo(x / mult, 0, 0) * mult);
    };
    m["INT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return num(std::floor(evalNum(a[0], c)));
    };
    m["ABS"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return num(std::abs(evalNum(a[0], c)));
    };
    m["SIGN"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const double x = evalNum(a[0], c);
        return num(x > 0 ? 1 : x < 0 ? -1 : 0);
    };
    m["SQRT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        double v = evalNum(a[0], c);
        if (v < 0) throw EvalError{"#NUM!"};
        return num(std::sqrt(v));
    };
    m["SQRTPI"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        double v = evalNum(a[0], c);
        if (v < 0) throw EvalError{"#NUM!"};
        return num(std::sqrt(v * 3.14159265358979323846));
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
    m["QUOTIENT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        double n = evalNum(a[0], c), d = evalNum(a[1], c);
        if (d == 0) throw EvalError{"#DIV/0!"};
        return num(std::trunc(n / d));
    };
    m["PI"] = [](const auto& a, Ctx&) {
        need(a, 0, 0);
        return num(3.14159265358979323846);
    };
    m["EXP"] = unaryMath([](double x) { return std::exp(x); });
    m["LN"] = unaryMath([](double x) { return std::log(x); }, true);
    m["LOG10"] = unaryMath([](double x) { return std::log10(x); }, true);
    m["LOG"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double x = evalNum(a[0], c), b = missing(a, 1) ? 10 : evalNum(a[1], c);
        if (x <= 0 || b <= 0 || b == 1) throw EvalError{b == 1 ? "#DIV/0!" : "#NUM!"};
        return num(std::log(x) / std::log(b));
    };
    m["FACT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return num(factorial(evalNum(a[0], c)));
    };
    m["FACTDOUBLE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        double n = std::floor(evalNum(a[0], c)), r = 1;
        if (n < -1 || n > 300) throw EvalError{"#NUM!"};
        for (double k = n; k > 1; k -= 2) r *= k;
        return num(r);
    };
    m["COMBIN"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        return num(combin(evalNum(a[0], c), evalNum(a[1], c)));
    };
    m["COMBINA"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double n = std::floor(evalNum(a[0], c)), k = std::floor(evalNum(a[1], c));
        if (n < 0 || k < 0) throw EvalError{"#NUM!"};
        if (n == 0 && k == 0) return num(1);
        return num(combin(n + k - 1, k));
    };
    m["PERMUT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double n = std::floor(evalNum(a[0], c)), k = std::floor(evalNum(a[1], c));
        if (n < 0 || k < 0 || k > n) throw EvalError{"#NUM!"};
        double r = 1;
        for (int i = 0; i < static_cast<int>(k); ++i) r *= n - i;
        return num(r);
    };
    m["GCD"] = [](const auto& a, Ctx& c) {
        long long g = 0;
        eachNumber(a, c, [&](double v) {
            if (v < 0) throw EvalError{"#NUM!"};
            g = std::gcd(g, static_cast<long long>(std::floor(v)));
        });
        return num(static_cast<double>(g));
    };
    m["LCM"] = [](const auto& a, Ctx& c) {
        long long l = 1;
        bool any = false;
        eachNumber(a, c, [&](double v) {
            if (v < 0) throw EvalError{"#NUM!"};
            const long long x = static_cast<long long>(std::floor(v));
            l = x == 0 ? 0 : (l == 0 ? 0 : std::lcm(l, x));
            any = true;
        });
        return num(any ? static_cast<double>(l) : 0);
    };
    m["EVEN"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const double x = evalNum(a[0], c);
        const double r = std::ceil(std::abs(x) / 2) * 2;
        return num(x < 0 ? -r : r);
    };
    m["ODD"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const double x = evalNum(a[0], c);
        double r = std::ceil(std::abs(x));
        if (std::fmod(r, 2) == 0) r += 1;
        return num(x < 0 ? -r : r);
    };
}

void addRounding(FnMap& m) {
    m["CEILING"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double x = evalNum(a[0], c);
        const double s = missing(a, 1) ? (x < 0 ? -1 : 1) : evalNum(a[1], c);
        if (x < 0 && s > 0) return num(-toMultiple(-x, s, false));  // toward zero, like Excel 2010+
        return num(x < 0 ? -toMultiple(-x, -s, true) : toMultiple(x, s, true));
    };
    m["FLOOR"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double x = evalNum(a[0], c);
        const double s = missing(a, 1) ? (x < 0 ? -1 : 1) : evalNum(a[1], c);
        if (s == 0) throw EvalError{"#DIV/0!"};
        if (x < 0 && s > 0) return num(-toMultiple(-x, s, true));
        return num(x < 0 ? -toMultiple(-x, -s, false) : toMultiple(x, s, false));
    };
    auto mathRound = [](bool up) {
        return [up](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 3);
            const double x = evalNum(a[0], c);
            const double s = std::abs(missing(a, 1) ? 1 : evalNum(a[1], c));
            const bool awayFromZero = !missing(a, 2) && evalNum(a[2], c) != 0;
            if (s == 0) return num(0);
            if (x >= 0) return num(toMultiple(x, s, up));
            // negative numbers: toward zero by default; mode turns it away from zero
            const bool towardUp = awayFromZero ? !up : up;
            return num(-toMultiple(-x, s, !towardUp));
        };
    };
    m["CEILING.MATH"] = mathRound(true);
    m["FLOOR.MATH"] = mathRound(false);
    m["CEILING.PRECISE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double x = evalNum(a[0], c), s = std::abs(missing(a, 1) ? 1 : evalNum(a[1], c));
        if (s == 0) return num(0);
        return num(std::ceil(x / s) * s);
    };
    m["ISO.CEILING"] = m["CEILING.PRECISE"];
    m["FLOOR.PRECISE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double x = evalNum(a[0], c), s = std::abs(missing(a, 1) ? 1 : evalNum(a[1], c));
        if (s == 0) return num(0);
        return num(std::floor(x / s) * s);
    };
    // trigonometry
    m["SIN"] = unaryMath([](double x) { return std::sin(x); });
    m["COS"] = unaryMath([](double x) { return std::cos(x); });
    m["TAN"] = unaryMath([](double x) { return std::tan(x); });
    m["ASIN"] = unaryMath([](double x) { return std::asin(x); }, false, true);
    m["ACOS"] = unaryMath([](double x) { return std::acos(x); }, false, true);
    m["ATAN"] = unaryMath([](double x) { return std::atan(x); });
    m["SINH"] = unaryMath([](double x) { return std::sinh(x); });
    m["COSH"] = unaryMath([](double x) { return std::cosh(x); });
    m["TANH"] = unaryMath([](double x) { return std::tanh(x); });
    m["DEGREES"] = unaryMath([](double x) { return x * 180 / 3.14159265358979323846; });
    m["RADIANS"] = unaryMath([](double x) { return x * 3.14159265358979323846 / 180; });
    m["ATAN2"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double x = evalNum(a[0], c), y = evalNum(a[1], c);
        if (x == 0 && y == 0) throw EvalError{"#DIV/0!"};
        return num(std::atan2(y, x));
    };
    // random numbers (recalculated every time)
    m["RAND"] = [](const auto& a, Ctx&) {
        need(a, 0, 0);
        return num(std::uniform_real_distribution<double>(0, 1)(rng()));
    };
    m["RANDBETWEEN"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const double lo = std::ceil(evalNum(a[0], c)), hi = std::floor(evalNum(a[1], c));
        if (lo > hi) throw EvalError{"#NUM!"};
        return num(static_cast<double>(std::uniform_int_distribution<long long>(static_cast<long long>(lo), static_cast<long long>(hi))(rng())));
    };
    m["SUBTOTAL"] = [](const auto& a, Ctx& c) {
        if (a.size() < 2) throw EvalError{"#VALUE!"};
        const int code = evalInt(a[0], c);
        const char* name = subtotalName(code > 100 ? code - 100 : code);
        if (!name || (code % 100) > 11) throw EvalError{"#VALUE!"};
        std::vector<NodeP> args;
        for (std::size_t i = 1; i < a.size(); ++i) args.push_back(visibleOnly(a[i], c, true, false));
        return callFunction(name, args, c);
    };
    m["AGGREGATE"] = [](const auto& a, Ctx& c) {
        if (a.size() < 3) throw EvalError{"#VALUE!"};
        const int code = evalInt(a[0], c), opt = evalInt(a[1], c);
        const char* name = subtotalName(code);
        if (!name || opt < 0 || opt > 7) throw EvalError{"#VALUE!"};
        const bool skipHidden = opt == 1 || opt == 3 || opt == 5 || opt == 7, skipErrors = opt == 2 || opt == 3 || opt == 6 || opt == 7;
        std::vector<NodeP> args;
        if (code >= 14) {
            args.push_back(visibleOnly(a[2], c, skipHidden, skipErrors));
            if (a.size() > 3) args.push_back(a[3]);
        } else {
            for (std::size_t i = 2; i < a.size(); ++i) args.push_back(visibleOnly(a[i], c, skipHidden, skipErrors));
        }
        return callFunction(name, args, c);
    };
}

std::string toRoman(int n) {
    static const std::pair<int, const char*> t[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
                                                    {40, "XL"},  {10, "X"},   {9, "IX"},   {5, "V"},   {4, "IV"},   {1, "I"}};
    std::string out;
    for (const auto& [v, s] : t) {
        while (n >= v) {
            out += s;
            n -= v;
        }
    }
    return out;
}

void addConversions(FnMap& m) {
    m["ROMAN"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double x = std::trunc(evalNum(a[0], c));
        if (x < 0 || x > 3999) throw EvalError{"#VALUE!"};
        return str(toRoman(static_cast<int>(x)));
    };
    m["ARABIC"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        const std::string s = upper(trimCopy(evalStr(a[0], c)));
        auto val = [](char ch) {
            switch (ch) {
                case 'I': return 1;
                case 'V': return 5;
                case 'X': return 10;
                case 'L': return 50;
                case 'C': return 100;
                case 'D': return 500;
                case 'M': return 1000;
                default: return -1;
            }
        };
        int total = 0;
        for (std::size_t i = 0; i < s.size(); ++i) {
            const int v = val(s[i]);
            if (v < 0) throw EvalError{"#VALUE!"};
            const int next = i + 1 < s.size() ? val(s[i + 1]) : 0;
            total += v < next ? -v : v;
        }
        return num(total);
    };
    auto toBase = [](int base) {
        return [base](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 2);
            double x = std::trunc(evalNum(a[0], c));
            if (x < 0 || x > 9.007e15) throw EvalError{"#NUM!"};
            const int places = missing(a, 1) ? 0 : evalInt(a[1], c);
            std::string out;
            long long v = static_cast<long long>(x);
            do {
                out.insert(out.begin(), "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[v % base]);
                v /= base;
            } while (v > 0);
            if (places > 0 && static_cast<int>(out.size()) < places) out.insert(0, static_cast<std::size_t>(places) - out.size(), '0');
            return str(out);
        };
    };
    auto fromBase = [](int base) {
        return [base](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 1);
            const std::string s = upper(trimCopy(evalStr(a[0], c)));
            if (s.empty()) return num(0);
            long long v = 0;
            for (char ch : s) {
                int d = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 10 : 99;
                if (d >= base) throw EvalError{"#NUM!"};
                v = v * base + d;
            }
            return num(static_cast<double>(v));
        };
    };
    m["DEC2BIN"] = toBase(2);
    m["DEC2HEX"] = toBase(16);
    m["DEC2OCT"] = toBase(8);
    m["BIN2DEC"] = fromBase(2);
    m["HEX2DEC"] = fromBase(16);
    m["OCT2DEC"] = fromBase(8);
    m["BASE"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        const int base = evalInt(a[1], c);
        if (base < 2 || base > 36) throw EvalError{"#NUM!"};
        double x = std::trunc(evalNum(a[0], c));
        if (x < 0) throw EvalError{"#NUM!"};
        const int places = missing(a, 2) ? 0 : evalInt(a[2], c);
        std::string out;
        long long v = static_cast<long long>(x);
        do {
            out.insert(out.begin(), "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[v % base]);
            v /= base;
        } while (v > 0);
        if (places > 0 && static_cast<int>(out.size()) < places) out.insert(0, static_cast<std::size_t>(places) - out.size(), '0');
        return str(out);
    };
    m["DECIMAL"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        const int base = evalInt(a[1], c);
        if (base < 2 || base > 36) throw EvalError{"#NUM!"};
        const std::string s = upper(trimCopy(evalStr(a[0], c)));
        double v = 0;
        for (char ch : s) {
            int d = std::isdigit(static_cast<unsigned char>(ch)) ? ch - '0' : (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 10 : 99;
            if (d >= base) throw EvalError{"#NUM!"};
            v = v * base + d;
        }
        return num(v);
    };
}

}  // namespace

void addMathFunctions(FnMap& m) {
    addBasics(m);
    addRounding(m);
    addConversions(m);
}

}  // namespace wetwaresuite::office::fx
