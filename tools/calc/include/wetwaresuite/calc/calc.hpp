// calc: a calculator that is easy to start with and goes deep. Expressions with
// variables and your own functions, exact whole numbers and fractions (big ones
// too), scientific functions, programmer mode (hex/bin/oct, bitwise, word
// sizes), units in expressions and conversions (offline tables, no currency),
// statistics, equation solving, integrals, derivatives and graphs as PNG.
// Everything is a calc.* command; settings, variables and memory are undoable.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::calc {

// ---- whole numbers of any size ------------------------------------------------------

class BigInt {
public:
    BigInt() = default;
    BigInt(std::int64_t v);  // NOLINT: a plain number is a BigInt
    static BigInt parse(const std::string& digits, int base = 10);  // throws std::runtime_error on bad digits

    bool isZero() const { return mag_.empty(); }
    bool negative() const { return neg_; }
    std::size_t bits() const;  // size of the magnitude in bits
    bool fitsInt64() const;
    std::int64_t toInt64() const;  // only when fitsInt64()
    double toDouble() const;
    std::string toString(int base = 10) const;  // "-ff" style for base 16 (no prefix)
    bool isOdd() const { return !mag_.empty() && (mag_[0] & 1u); }

    BigInt operator-() const;
    friend BigInt operator+(const BigInt& a, const BigInt& b);
    friend BigInt operator-(const BigInt& a, const BigInt& b);
    friend BigInt operator*(const BigInt& a, const BigInt& b);
    // truncating division (like C++): q = a / b rounded toward zero, r has a's sign
    static void divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r);  // throws on b == 0
    friend BigInt operator/(const BigInt& a, const BigInt& b);
    friend BigInt operator%(const BigInt& a, const BigInt& b);
    friend bool operator==(const BigInt& a, const BigInt& b) { return a.neg_ == b.neg_ && a.mag_ == b.mag_; }
    friend bool operator!=(const BigInt& a, const BigInt& b) { return !(a == b); }
    friend bool operator<(const BigInt& a, const BigInt& b);
    friend bool operator>(const BigInt& a, const BigInt& b) { return b < a; }
    friend bool operator<=(const BigInt& a, const BigInt& b) { return !(b < a); }
    friend bool operator>=(const BigInt& a, const BigInt& b) { return !(a < b); }
    static int compareAbs(const BigInt& a, const BigInt& b);

    BigInt abs() const;
    static BigInt gcd(BigInt a, BigInt b);
    static BigInt pow(const BigInt& base, std::uint64_t exp);
    BigInt shiftLeft(std::size_t n) const;
    BigInt shiftRight(std::size_t n) const;  // floor division by 2^n (two's complement for negatives)
    // two's complement bitwise operators, as if both had infinitely many sign bits
    static BigInt bitAnd(const BigInt& a, const BigInt& b);
    static BigInt bitOr(const BigInt& a, const BigInt& b);
    static BigInt bitXor(const BigInt& a, const BigInt& b);
    BigInt bitNot() const;  // -x - 1
    // the low `bits` bits as an unsigned number (two's complement for negatives)
    BigInt lowBits(std::size_t bits) const;
    std::size_t popcount() const;

private:
    bool neg_ = false;
    std::vector<std::uint32_t> mag_;  // little-endian limbs, no leading zeros
    void trim();
    friend struct BigIntAccess;
};

// ---- numbers: exact fractions when cheap, floating point otherwise ----------------------

struct Number {
    bool exact = true;
    BigInt num{0};
    BigInt den{1};  // > 0, gcd(num, den) == 1
    double approx = 0;  // the value when !exact

    Number() = default;
    Number(std::int64_t v) : num(v) {}  // NOLINT
    static Number real(double v);
    static Number fraction(BigInt n, BigInt d);  // normalised; throws on d == 0
    double toDouble() const;
    bool isInteger() const { return exact && den == BigInt(1); }
    bool isZero() const { return exact ? num.isZero() : approx == 0; }
    bool isNegative() const { return exact ? num.negative() : approx < 0; }
};

Number add(const Number& a, const Number& b);
Number sub(const Number& a, const Number& b);
Number mul(const Number& a, const Number& b);
Number div(const Number& a, const Number& b);  // throws "can't divide by zero"
Number neg(const Number& a);
int compare(const Number& a, const Number& b);  // -1, 0, 1

// ---- quantities: a number with an optional unit ---------------------------------------

constexpr int kDims = 8;  // m kg s A K mol cd bit
struct Dims {
    std::int8_t e[kDims] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool none() const;
    bool operator==(const Dims& o) const;
    bool operator!=(const Dims& o) const { return !(*this == o); }
};

struct Quantity {
    Number value;          // in SI base units (or plain)
    Dims dims;
    std::string unit;      // what to show it in ("km/h"), "" = SI or plain
    double scale = 1;      // SI per one `unit`
    double offset = 0;     // for °C and °F: SI = (shown + offset) * scale
    bool angle = false;    // carries an angle unit (deg, rad, ...): trig uses it as is
};

// One unit from the offline tables.
struct UnitInfo {
    std::string name;      // "km"
    std::string category;  // "length"
    std::string title;     // "kilometre"
    double scale = 1;
    double offset = 0;
    Dims dims;
    bool angle = false;
};
// Looks a unit name up, with SI prefixes and plurals ("kilometres", "mA", "GiB"). False if unknown.
bool findUnit(const std::string& name, UnitInfo& out);
std::vector<UnitInfo> unitList(const std::string& category = {});  // the main units (no prefixed forms)
std::vector<std::string> unitCategories();

// ---- settings and state -----------------------------------------------------------------

enum class Angle { Degrees, Radians, Gradians };

struct Settings {
    Angle angle = Angle::Degrees;
    int precision = 12;   // significant digits shown
    std::string base = "dec";  // dec, hex, bin, oct: how whole-number answers are shown
    int bits = 64;        // word size for ~ and negative hex/bin/oct (8, 16, 32, 64, 0 = unlimited)
    bool exact = true;    // keep fractions exact
};

struct UserFunction {
    std::vector<std::string> params;
    std::string body;  // the expression text
};

struct HistoryEntry {
    std::string input;
    std::string output;
};

// Variables, functions, memory, history and settings: what a calculator remembers.
struct State {
    Settings settings;
    std::map<std::string, Quantity> vars;
    std::map<std::string, std::vector<Quantity>> lists;  // data = [1, 2, 3]
    std::map<std::string, UserFunction> functions;
    Quantity memory;
    bool hasMemory = false;
    std::vector<HistoryEntry> history;
};

// ---- evaluating ----------------------------------------------------------------------------

struct Answer {
    Quantity value;
    bool isList = false;
    std::vector<Quantity> list;
    std::string text;      // how it is shown, e.g. "3/4", "0xff", "12.5 km"
    std::string assigned;  // "x" when the input was x = ...
    std::string defined;   // "f" when the input defined a function
    std::string format;    // a "to hex"/"to frac" style request, if any
};

// Evaluates one line (or several separated by ';'; the answer is the last one's).
// Throws std::runtime_error with a plain-word message on mistakes.
Answer evaluate(State& state, const std::string& input);

// A parsed expression you can evaluate many times (graphs, solving, integrals).
class Compiled {
public:
    Compiled(const State& state, const std::string& expression);  // throws on syntax errors
    ~Compiled();
    Compiled(Compiled&&) noexcept;
    Compiled& operator=(Compiled&&) noexcept;
    double at(const std::map<std::string, double>& vars) const;  // NaN where undefined
    double at(const std::string& var, double x) const;
    std::vector<std::string> names() const;  // the free names it uses that the state doesn't know

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Shows a quantity the way the settings ask ("1/3", "0.333333333333", "0x1f", "5 km").
std::string formatQuantity(const Quantity& q, const Settings& s, const std::string& format = {});
std::string formatNumber(double v, int precision);

// Converts `value` from one unit to another ("km/h" -> "mph"); throws when they don't match.
double convertUnits(double value, const std::string& from, const std::string& to);

// ---- statistics, solving, calculus ------------------------------------------------------

struct Stats {
    std::size_t count = 0;
    double sum = 0, mean = 0, median = 0, min = 0, max = 0, range = 0;
    double variance = 0, stdev = 0;    // sample (n - 1)
    double pvariance = 0, pstdev = 0;  // population (n)
    double q1 = 0, q3 = 0, iqr = 0;
    double geomean = 0;  // NaN when a value is <= 0
    double skewness = 0;
    std::vector<double> modes;
};
Stats describe(std::vector<double> values);  // throws on an empty list

struct Regression {
    double slope = 0, intercept = 0, r = 0, r2 = 0;
};
Regression linearRegression(const std::vector<double>& xs, const std::vector<double>& ys);

// Parses "1, 2, 3", "1 2 3" or one number per line.
std::vector<double> parseNumbers(const std::string& text);

struct Solution {
    std::vector<std::string> vars;
    std::vector<std::vector<double>> roots;  // one vector of values (one per var) per solution
};
// Solves "x^2 = 4", "x^2 - 4" (= 0), or a system "2x + y = 5; x - y = 1". For one unknown it
// finds every real root it can between `from` and `to`; for systems it uses Newton's method.
Solution solve(const State& state, const std::string& equations, const std::vector<std::string>& vars = {},
               double from = -1000, double to = 1000);

double integrate(const State& state, const std::string& expression, const std::string& var, double a, double b);
double derivative(const State& state, const std::string& expression, const std::string& var, double at, int order = 1);

// Prime factors of a whole number, smallest first ("2^3 * 3 * 5"); empty for 0 and ±1.
std::vector<std::pair<BigInt, int>> factorize(const BigInt& n);
bool isProbablePrime(const BigInt& n);

// ---- graphs ------------------------------------------------------------------------------

struct GraphOptions {
    double xmin = -10, xmax = 10;
    double ymin = 0, ymax = 0;  // both 0 = fit the curves
    int width = 800, height = 500;
    bool dark = false;
    bool grid = true;
};
struct Curve {
    std::string expression;
    std::vector<double> xs, ys;  // NaN y = a gap
};
// Samples y = f(x) for each expression at `samples` points between xmin and xmax.
std::vector<Curve> sampleCurves(const State& state, const std::vector<std::string>& expressions, const GraphOptions& o, int samples);
// Draws the curves with axes, grid, labels and a legend; returns PNG bytes.
std::string graphPng(const std::vector<Curve>& curves, const GraphOptions& o);
// "sin(x); x^2" or "y = x^2" lines -> expressions in x
std::vector<std::string> splitExpressions(const std::string& text);

void registerCommands(Session& session);

}  // namespace wetwaresuite::calc
