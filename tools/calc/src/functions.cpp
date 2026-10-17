// Built-in functions and constants.
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <stdexcept>

#include "internal.hpp"

namespace wetwaresuite::calc::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

using Args = std::vector<Val>;
using Fn = std::function<Val(Args&, const Env&)>;

struct FnInfo {
    int minArgs, maxArgs;  // -1 = any
    Fn fn;
};

Val one(Quantity q) {
    Val v;
    v.q = std::move(q);
    return v;
}
Val num(double v) { return one(plain(numberFromDouble(v))); }
Val real(double v) { return one(plainDouble(v)); }

const Quantity& scalar(const Args& a, std::size_t i, const char* fn) {
    if (a[i].isList) throw std::runtime_error(std::string(fn) + " takes single values, not a list");
    return a[i].q;
}

double plainArg(const Args& a, std::size_t i, const char* fn) {
    const Quantity& q = scalar(a, i, fn);
    if (!q.dims.none()) throw std::runtime_error(std::string(fn) + " needs a plain number (no unit)");
    return q.value.toDouble();
}

BigInt intArg(const Args& a, std::size_t i, const char* fn) {
    const Quantity& q = scalar(a, i, fn);
    if (q.value.isInteger() && q.dims.none()) return q.value.num;
    double v = q.value.toDouble();
    if (q.dims.none() && v == std::floor(v) && std::fabs(v) < 9e15) return BigInt(static_cast<std::int64_t>(v));
    throw std::runtime_error(std::string(fn) + " works on whole numbers");
}

// every value of the arguments, lists flattened
std::vector<Quantity> flat(const Args& a) {
    std::vector<Quantity> out;
    for (const auto& v : a) {
        if (v.isList) out.insert(out.end(), v.items.begin(), v.items.end());
        else out.push_back(v.q);
    }
    return out;
}

std::vector<double> flatDoubles(const Args& a, const char* fn) {
    std::vector<double> out;
    for (const auto& q : flat(a)) {
        (void)fn;
        out.push_back(q.value.toDouble());
    }
    return out;
}

Val unary(Args& a, const char* fn, double (*f)(double)) {
    if (a[0].isList) {
        Val v;
        v.isList = true;
        for (const auto& q : a[0].items) v.items.push_back(plainDouble(f(q.value.toDouble())));
        return v;
    }
    return real(f(plainArg(a, 0, fn)));
}

Val trig(Args& a, const Env& env, double (*f)(double)) {
    auto one1 = [&](const Quantity& q) { return plainDouble(f(toAngle(q, env.state->settings))); };
    if (a[0].isList) {
        Val v;
        v.isList = true;
        for (const auto& q : a[0].items) v.items.push_back(one1(q));
        return v;
    }
    double r = f(toAngle(a[0].q, env.state->settings));
    if (std::fabs(r) < 1e-15) r = 0;  // sin(180°) is 0, not 1.2e-16
    return real(r);
}

Val inverseTrig(Args& a, const Env& env, double (*f)(double)) {
    return real(fromRadians(f(plainArg(a, 0, "inverse trig")), env.state->settings));
}

// sum of exact values stays exact
Number sumOf(const std::vector<Quantity>& qs) {
    Number s(0);
    for (const auto& q : qs) s = add(s, q.value);
    return s;
}

Quantity withUnitOf(const std::vector<Quantity>& qs, Number v) {
    Quantity q = qs.empty() ? Quantity{} : qs.front();
    q.value = std::move(v);
    return q;
}

Val median(Args& a) {
    auto qs = flat(a);
    if (qs.empty()) throw std::runtime_error("median of nothing");
    std::sort(qs.begin(), qs.end(), [](const Quantity& x, const Quantity& y) { return compare(x.value, y.value) < 0; });
    const std::size_t n = qs.size();
    if (n % 2) return one(qs[n / 2]);
    return one(withUnitOf(qs, div(add(qs[n / 2 - 1].value, qs[n / 2].value), Number(2))));
}

Val variance(Args& a, bool sample, bool root) {
    auto xs = flatDoubles(a, "var");
    if (xs.size() < (sample ? 2u : 1u)) throw std::runtime_error(sample ? "need at least two values" : "need at least one value");
    double mean = 0;
    for (double x : xs) mean += x;
    mean /= static_cast<double>(xs.size());
    double ss = 0;
    for (double x : xs) ss += (x - mean) * (x - mean);
    double v = ss / static_cast<double>(xs.size() - (sample ? 1 : 0));
    return real(root ? std::sqrt(v) : v);
}

BigInt binomial(std::int64_t n, std::int64_t k) {
    if (k < 0 || k > n) return BigInt(0);
    k = std::min(k, n - k);
    BigInt r(1);
    for (std::int64_t i = 1; i <= k; ++i) r = r * BigInt(n - k + i) / BigInt(i);
    return r;
}

std::mt19937_64& rng() {
    static std::mt19937_64 g{std::random_device{}()};
    return g;
}

Val roundTo(Args& a, double (*f)(double)) {
    const Quantity& q = scalar(a, 0, "round");
    double digits = a.size() > 1 ? plainArg(a, 1, "round") : 0;
    if (q.value.exact && digits == 0) {
        // exact: floor/ceil/round of a fraction
        BigInt fl, rem;
        BigInt::divmod(q.value.num, q.value.den, fl, rem);
        if (rem.negative()) fl = fl - BigInt(1);
        Number floorN = Number::fraction(fl, BigInt(1));
        Number frac = sub(q.value, floorN);
        Number out = floorN;
        if (f == static_cast<double (*)(double)>(std::ceil)) {
            if (!frac.isZero()) out = add(floorN, Number(1));
        } else if (f == static_cast<double (*)(double)>(std::round)) {
            int c = compare(frac, Number::fraction(BigInt(1), BigInt(2)));
            if (c > 0 || (c == 0 && !q.value.isNegative())) out = add(floorN, Number(1));
        } else if (f == static_cast<double (*)(double)>(std::trunc)) {
            if (q.value.isNegative() && !frac.isZero()) out = add(floorN, Number(1));
        }
        Quantity r = q;
        r.value = out;
        return one(r);
    }
    double scale = std::pow(10.0, digits);
    double shown = q.value.toDouble() / q.scale;
    Quantity r = q;
    r.value = numberFromDouble(f(shown * scale) / scale * q.scale);
    return one(r);
}

std::map<std::string, FnInfo> buildFunctions() {
    std::map<std::string, FnInfo> m;
    auto u = [&m](const char* name, double (*f)(double)) {
        m[name] = {1, 1, [name, f](Args& a, const Env&) { return unary(a, name, f); }};
    };
    u("exp", [](double x) { return std::exp(x); });
    u("ln", [](double x) { return std::log(x); });
    u("log10", [](double x) { return std::log10(x); });
    u("log2", [](double x) { return std::log2(x); });
    u("cbrt", [](double x) { return std::cbrt(x); });
    u("sinh", [](double x) { return std::sinh(x); });
    u("cosh", [](double x) { return std::cosh(x); });
    u("tanh", [](double x) { return std::tanh(x); });
    u("asinh", [](double x) { return std::asinh(x); });
    u("acosh", [](double x) { return std::acosh(x); });
    u("atanh", [](double x) { return std::atanh(x); });
    u("gamma", [](double x) { return std::tgamma(x); });
    u("lgamma", [](double x) { return std::lgamma(x); });
    u("erf", [](double x) { return std::erf(x); });
    u("erfc", [](double x) { return std::erfc(x); });
    auto t = [&m](const char* name, double (*f)(double)) {
        m[name] = {1, 1, [f](Args& a, const Env& e) { return trig(a, e, f); }};
    };
    t("sin", [](double x) { return std::sin(x); });
    t("cos", [](double x) { return std::cos(x); });
    t("tan", [](double x) { return std::tan(x); });
    t("sec", [](double x) { return 1 / std::cos(x); });
    t("csc", [](double x) { return 1 / std::sin(x); });
    t("cot", [](double x) { return 1 / std::tan(x); });
    auto it = [&m](const char* name, double (*f)(double)) {
        m[name] = {1, 1, [f](Args& a, const Env& e) { return inverseTrig(a, e, f); }};
    };
    it("asin", [](double x) { return std::asin(x); });
    it("acos", [](double x) { return std::acos(x); });
    it("atan", [](double x) { return std::atan(x); });
    m["atan2"] = {2, 2, [](Args& a, const Env& e) {
                      return real(fromRadians(std::atan2(plainArg(a, 0, "atan2"), plainArg(a, 1, "atan2")), e.state->settings));
                  }};
    m["sqrt"] = {1, 1, [](Args&, const Env&) -> Val { throw std::runtime_error("sqrt is evaluated through powers"); }};
    m["log"] = {1, 2, [](Args& a, const Env&) {
                    double x = plainArg(a, 0, "log");
                    if (a.size() == 1) return real(std::log10(x));
                    return real(std::log(x) / std::log(plainArg(a, 1, "log")));
                }};
    m["root"] = {2, 2, [](Args& a, const Env&) {
                     double x = plainArg(a, 0, "root"), n = plainArg(a, 1, "root");
                     if (x < 0 && std::fmod(n, 2) == 1) return real(-std::pow(-x, 1 / n));
                     return real(std::pow(x, 1 / n));
                 }};
    m["hypot"] = {2, -1, [](Args& a, const Env&) {
                      double s = 0;
                      for (double x : flatDoubles(a, "hypot")) s += x * x;
                      return real(std::sqrt(s));
                  }};
    m["abs"] = {1, 1, [](Args& a, const Env&) {
                    Quantity q = scalar(a, 0, "abs");
                    if (q.value.isNegative()) q.value = neg(q.value);
                    return one(q);
                }};
    m["sign"] = {1, 1, [](Args& a, const Env&) {
                     const Quantity& q = scalar(a, 0, "sign");
                     return num(q.value.isZero() ? 0 : q.value.isNegative() ? -1 : 1);
                 }};
    m["floor"] = {1, 2, [](Args& a, const Env&) { return roundTo(a, static_cast<double (*)(double)>(std::floor)); }};
    m["ceil"] = {1, 2, [](Args& a, const Env&) { return roundTo(a, static_cast<double (*)(double)>(std::ceil)); }};
    m["round"] = {1, 2, [](Args& a, const Env&) { return roundTo(a, static_cast<double (*)(double)>(std::round)); }};
    m["trunc"] = {1, 2, [](Args& a, const Env&) { return roundTo(a, static_cast<double (*)(double)>(std::trunc)); }};
    m["frac"] = {1, 1, [](Args& a, const Env& e) {
                     Args b = {a[0]};
                     Val whole = roundTo(b, static_cast<double (*)(double)>(std::trunc));
                     (void)e;
                     Quantity q = a[0].q;
                     q.value = sub(a[0].q.value, whole.q.value);
                     return one(q);
                 }};
    return m;
}

void addMore(std::map<std::string, FnInfo>& m) {
    m["min"] = {1, -1, [](Args& a, const Env&) {
                    auto qs = flat(a);
                    if (qs.empty()) throw std::runtime_error("min of nothing");
                    return one(*std::min_element(qs.begin(), qs.end(), [](const Quantity& x, const Quantity& y) { return compare(x.value, y.value) < 0; }));
                }};
    m["max"] = {1, -1, [](Args& a, const Env&) {
                    auto qs = flat(a);
                    if (qs.empty()) throw std::runtime_error("max of nothing");
                    return one(*std::max_element(qs.begin(), qs.end(), [](const Quantity& x, const Quantity& y) { return compare(x.value, y.value) < 0; }));
                }};
    m["sum"] = {0, -1, [](Args& a, const Env&) {
                    auto qs = flat(a);
                    return one(withUnitOf(qs, sumOf(qs)));
                }};
    m["prod"] = {0, -1, [](Args& a, const Env&) {
                     Number p(1);
                     for (const auto& q : flat(a)) p = mul(p, q.value);
                     return one(plain(p));
                 }};
    m["product"] = m["prod"];
    m["mean"] = {1, -1, [](Args& a, const Env&) {
                     auto qs = flat(a);
                     if (qs.empty()) throw std::runtime_error("mean of nothing");
                     return one(withUnitOf(qs, div(sumOf(qs), Number(static_cast<std::int64_t>(qs.size())))));
                 }};
    m["avg"] = m["average"] = m["mean"];
    m["median"] = {1, -1, [](Args& a, const Env&) { return median(a); }};
    m["var"] = {1, -1, [](Args& a, const Env&) { return variance(a, true, false); }};
    m["variance"] = m["var"];
    m["stdev"] = {1, -1, [](Args& a, const Env&) { return variance(a, true, true); }};
    m["std"] = m["stddev"] = m["stdev"];
    m["pvar"] = {1, -1, [](Args& a, const Env&) { return variance(a, false, false); }};
    m["pstdev"] = {1, -1, [](Args& a, const Env&) { return variance(a, false, true); }};
    m["count"] = {0, -1, [](Args& a, const Env&) { return num(static_cast<double>(flat(a).size())); }};
    m["range"] = {1, -1, [](Args& a, const Env&) {
                      auto xs = flatDoubles(a, "range");
                      auto mm = std::minmax_element(xs.begin(), xs.end());
                      return real(*mm.second - *mm.first);
                  }};
    m["mode"] = {1, -1, [](Args& a, const Env&) {
                     auto xs = flatDoubles(a, "mode");
                     std::map<double, int> counts;
                     for (double x : xs) ++counts[x];
                     double best = xs.front();
                     int n = 0;
                     for (auto& [x, c] : counts) {
                         if (c > n) {
                             n = c;
                             best = x;
                         }
                     }
                     return num(best);
                 }};
    m["gcd"] = {1, -1, [](Args& a, const Env&) {
                    BigInt g(0);
                    for (const auto& q : flat(a)) {
                        Args one1 = {Val{q, false, {}}};
                        g = BigInt::gcd(g, intArg(one1, 0, "gcd"));
                    }
                    return one(plain(Number::fraction(g, BigInt(1))));
                }};
    m["lcm"] = {1, -1, [](Args& a, const Env&) {
                    BigInt l(1);
                    for (const auto& q : flat(a)) {
                        Args one1 = {Val{q, false, {}}};
                        BigInt v = intArg(one1, 0, "lcm").abs();
                        if (v.isZero()) return num(0);
                        l = l / BigInt::gcd(l, v) * v;
                    }
                    return one(plain(Number::fraction(l, BigInt(1))));
                }};
    m["fact"] = {1, 1, [](Args& a, const Env&) {
                     BigInt n = intArg(a, 0, "fact");
                     if (n.negative()) throw std::runtime_error("factorial needs a number that isn't negative");
                     if (!n.fitsInt64() || n.toInt64() > 20000) return real(INFINITY);
                     BigInt r(1);
                     for (std::int64_t i = 2; i <= n.toInt64(); ++i) r = r * BigInt(i);
                     return one(plain(Number::fraction(r, BigInt(1))));
                 }};
    m["factorial"] = m["fact"];
    m["ncr"] = {2, 2, [](Args& a, const Env&) {
                    BigInt n = intArg(a, 0, "ncr"), k = intArg(a, 1, "ncr");
                    if (!n.fitsInt64() || !k.fitsInt64() || n.toInt64() > 100000) throw std::runtime_error("ncr: numbers too big");
                    return one(plain(Number::fraction(binomial(n.toInt64(), k.toInt64()), BigInt(1))));
                }};
    m["comb"] = m["choose"] = m["ncr"];
    m["npr"] = {2, 2, [](Args& a, const Env&) {
                    BigInt n = intArg(a, 0, "npr"), k = intArg(a, 1, "npr");
                    if (!n.fitsInt64() || !k.fitsInt64() || n.toInt64() > 100000) throw std::runtime_error("npr: numbers too big");
                    BigInt r(1);
                    for (std::int64_t i = 0; i < k.toInt64(); ++i) r = r * BigInt(n.toInt64() - i);
                    return one(plain(Number::fraction(r, BigInt(1))));
                }};
    m["perm"] = m["npr"];
    m["mod"] = {2, 2, [](Args& a, const Env&) {
                    BigInt x = intArg(a, 0, "mod"), y = intArg(a, 1, "mod");
                    BigInt r = x % y;
                    if (!r.isZero() && r.negative() != y.negative()) r = r + y;
                    return one(plain(Number::fraction(r, BigInt(1))));
                }};
    m["isprime"] = {1, 1, [](Args& a, const Env&) { return num(isProbablePrime(intArg(a, 0, "isprime")) ? 1 : 0); }};
    m["nextprime"] = {1, 1, [](Args& a, const Env&) {
                          BigInt n = intArg(a, 0, "nextprime") + BigInt(1);
                          while (!isProbablePrime(n)) n = n + BigInt(1);
                          return one(plain(Number::fraction(n, BigInt(1))));
                      }};
    m["fib"] = {1, 1, [](Args& a, const Env&) {
                    BigInt n = intArg(a, 0, "fib");
                    if (n.negative() || !n.fitsInt64() || n.toInt64() > 100000) throw std::runtime_error("fib takes 0 to 100000");
                    BigInt x(0), y(1);
                    for (std::int64_t i = 0; i < n.toInt64(); ++i) {
                        BigInt z = x + y;
                        x = std::move(y);
                        y = std::move(z);
                    }
                    return one(plain(Number::fraction(x, BigInt(1))));
                }};
    m["popcount"] = {1, 1, [](Args& a, const Env&) { return num(static_cast<double>(intArg(a, 0, "popcount").abs().popcount())); }};
    m["bits"] = {1, 1, [](Args& a, const Env&) { return num(static_cast<double>(intArg(a, 0, "bits").abs().bits())); }};
    m["rand"] = {0, 0, [](Args&, const Env&) { return real(std::uniform_real_distribution<double>(0, 1)(rng())); }};
    m["randint"] = {2, 2, [](Args& a, const Env&) {
                        auto lo = static_cast<long long>(plainArg(a, 0, "randint")), hi = static_cast<long long>(plainArg(a, 1, "randint"));
                        if (hi < lo) std::swap(lo, hi);
                        return num(static_cast<double>(std::uniform_int_distribution<long long>(lo, hi)(rng())));
                    }};
    m["if"] = {3, 3, [](Args& a, const Env&) { return scalar(a, 0, "if").value.isZero() ? a[2] : a[1]; }};
    m["clamp"] = {3, 3, [](Args& a, const Env&) {
                      const Quantity &x = scalar(a, 0, "clamp"), &lo = scalar(a, 1, "clamp"), &hi = scalar(a, 2, "clamp");
                      return one(compare(x.value, lo.value) < 0 ? lo : compare(x.value, hi.value) > 0 ? hi : x);
                  }};
    m["pow"] = {2, 2, [](Args& a, const Env&) { return real(std::pow(plainArg(a, 0, "pow"), plainArg(a, 1, "pow"))); }};
    m["percent"] = {2, 2, [](Args& a, const Env&) { return one(plain(div(mul(scalar(a, 0, "percent").value, Number(100)), scalar(a, 1, "percent").value))); }};
}

const std::map<std::string, FnInfo>& functions() {
    static const std::map<std::string, FnInfo> m = [] {
        auto f = buildFunctions();
        addMore(f);
        return f;
    }();
    return m;
}

struct Constant {
    double value;
    Dims dims;
    const char* unit;
};

Dims dims(std::initializer_list<int> v) {
    Dims d;
    int i = 0;
    for (int x : v) d.e[i++] = static_cast<std::int8_t>(x);
    return d;
}

const std::map<std::string, Constant>& constants() {
    static const std::map<std::string, Constant> c = {
        {"pi", {kPi, {}, ""}},
        {"e", {2.718281828459045235, {}, ""}},
        {"tau", {2 * kPi, {}, ""}},
        {"phi", {1.6180339887498948482, {}, ""}},
        {"inf", {INFINITY, {}, ""}},
        {"infinity", {INFINITY, {}, ""}},
        {"nan", {NAN, {}, ""}},
        {"c", {299792458.0, dims({1, 0, -1}), "m/s"}},
        {"G", {6.67430e-11, dims({3, -1, -2}), "m^3/(kg*s^2)"}},
        {"g0", {9.80665, dims({1, 0, -2}), "m/s^2"}},
        {"planck", {6.62607015e-34, dims({2, 1, -1}), "J*s"}},
        {"hbar", {1.054571817e-34, dims({2, 1, -1}), "J*s"}},
        {"NA", {6.02214076e23, dims({0, 0, 0, 0, 0, -1}), "1/mol"}},
        {"kB", {1.380649e-23, dims({2, 1, -2, 0, -1}), "J/K"}},
        {"R", {8.314462618, dims({2, 1, -2, 0, -1, -1}), "J/(mol*K)"}},
        {"qe", {1.602176634e-19, dims({0, 0, 1, 1}), "A*s"}},
        {"me", {9.1093837015e-31, dims({0, 1}), "kg"}},
        {"mp", {1.67262192369e-27, dims({0, 1}), "kg"}},
        {"eps0", {8.8541878128e-12, dims({-3, -1, 4, 2}), "F/m"}},
        {"mu0", {1.25663706212e-6, dims({1, 1, -2, -2}), "N/A^2"}},
    };
    return c;
}

}  // namespace

bool isBuiltinFunction(const std::string& name) { return functions().count(name) > 0; }

bool isConstant(const std::string& name) { return constants().count(name) > 0 || name == "ans" || name == "mem"; }

std::vector<std::string> functionNames() {
    std::vector<std::string> out;
    for (const auto& [k, v] : functions()) out.push_back(k);
    return out;
}

Val callFunction(const std::string& name, std::vector<Val>& args, const Env& env) {
    if (name == "ans" || name == "mem") {
        if (name == "mem") {
            if (!env.state->hasMemory) throw std::runtime_error("the memory is empty (store a value first)");
            return one(env.state->memory);
        }
        auto it = env.state->vars.find("ans");
        if (it == env.state->vars.end()) throw std::runtime_error("there's no previous answer yet");
        return one(it->second);
    }
    auto c = constants().find(name);
    if (c != constants().end()) {
        Quantity q = plainDouble(c->second.value);
        q.dims = c->second.dims;
        return one(q);
    }
    auto f = functions().find(name);
    if (f == functions().end()) throw std::runtime_error("there's no function called " + name);
    const int n = static_cast<int>(args.size());
    if (n < f->second.minArgs || (f->second.maxArgs >= 0 && n > f->second.maxArgs)) {
        std::string want = f->second.minArgs == f->second.maxArgs ? std::to_string(f->second.minArgs)
                           : f->second.maxArgs < 0                ? "at least " + std::to_string(f->second.minArgs)
                                                                  : std::to_string(f->second.minArgs) + " to " + std::to_string(f->second.maxArgs);
        throw std::runtime_error(name + " takes " + want + " value" + (want == "1" ? "" : "s"));
    }
    if (name == "sqrt") {
        // through the power rules: exact for perfect squares (sqrt(16/9) = 4/3), units are halved
        const Quantity& q = scalar(args, 0, "sqrt");
        if (q.value.isNegative()) throw std::runtime_error("the square root of a negative number isn't real");
        auto expr = parseExpression("x^(1/2)");
        std::map<std::string, Quantity> locals{{"x", q}};
        Env inner = env;
        inner.locals = &locals;
        return eval(*expr, inner);
    }
    return f->second.fn(args, env);
}

}  // namespace wetwaresuite::calc::detail
