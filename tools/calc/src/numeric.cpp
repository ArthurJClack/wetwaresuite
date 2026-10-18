// Evaluating whole lines, showing answers, statistics, solving, calculus and primes.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

#include "internal.hpp"

namespace wetwaresuite::calc {

using namespace detail;

namespace {

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// splits at ';' outside brackets
std::vector<std::string> statements(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : text) {
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if ((c == ';' || c == '\n') && depth <= 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

// the position of a lone '=' (not ==, !=, <=, >=) outside brackets, or npos
std::size_t assignAt(const std::string& s) {
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '(' || c == '[') ++depth;
        if (c == ')' || c == ']') --depth;
        if (c != '=' || depth) continue;
        if (i + 1 < s.size() && s[i + 1] == '=') {
            ++i;
            continue;
        }
        if (i > 0 && std::string("=!<>").find(s[i - 1]) != std::string::npos) continue;
        return i;
    }
    return std::string::npos;
}

bool isName(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_' || static_cast<unsigned char>(s[0]) >= 0x80)) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80)) return false;
    }
    return true;
}

std::string normaliseFormat(const std::string& f) {
    if (f == "binary") return "bin";
    if (f == "octal") return "oct";
    if (f == "decimal") return "dec";
    if (f == "hexadecimal") return "hex";
    if (f == "fraction") return "frac";
    if (f == "scientific") return "sci";
    return f;
}

std::string continuedFraction(double v) {
    if (!std::isfinite(v)) return formatNumber(v, 12);
    // best fraction with a denominator up to a million
    long long h1 = 1, h0 = 0, k1 = 0, k0 = 1;
    double x = v;
    for (int i = 0; i < 64; ++i) {
        double a = std::floor(x);
        long long ai = static_cast<long long>(a);
        long long h2 = ai * h1 + h0, k2 = ai * k1 + k0;
        if (k2 > 1000000 || std::fabs(static_cast<double>(h2)) > 9e15) break;
        h0 = h1;
        h1 = h2;
        k0 = k1;
        k1 = k2;
        if (std::fabs(static_cast<double>(h1) / static_cast<double>(k1) - v) < 1e-12 * std::max(1.0, std::fabs(v))) break;
        double f = x - a;
        if (f < 1e-15) break;
        x = 1 / f;
    }
    if (k1 == 1) return std::to_string(h1);
    return std::to_string(h1) + "/" + std::to_string(k1);
}

std::string baseText(const BigInt& n, const Settings& s, const std::string& base) {
    const int radix = base == "hex" ? 16 : base == "bin" ? 2 : 8;
    const char* prefix = base == "hex" ? "0x" : base == "bin" ? "0b" : "0o";
    if (n.negative() && s.bits > 0) return prefix + n.lowBits(static_cast<std::size_t>(s.bits)).toString(radix);
    std::string digits = n.abs().toString(radix);
    return (n.negative() ? "-" : "") + std::string(prefix) + digits;
}

std::string formatPlain(const Number& n, const Settings& s, std::string format) {
    format = normaliseFormat(format);
    const bool integer = n.isInteger() || (!n.exact && std::isfinite(n.approx) && n.approx == std::floor(n.approx) && std::fabs(n.approx) < 9e15);
    std::string base = format.empty() ? s.base : format;
    if ((base == "hex" || base == "bin" || base == "oct") && integer) {
        BigInt v = n.exact ? n.num : BigInt(static_cast<std::int64_t>(n.approx));
        return baseText(v, s, base);
    }
    if (format == "frac") return n.exact ? (n.isInteger() ? n.num.toString() : n.num.toString() + "/" + n.den.toString()) : continuedFraction(n.toDouble());
    if (format == "sci" || format == "eng") {
        double v = n.toDouble();
        if (v == 0 || !std::isfinite(v)) return formatNumber(v, s.precision);
        int e = static_cast<int>(std::floor(std::log10(std::fabs(v))));
        if (format == "eng") e = static_cast<int>(std::floor(e / 3.0)) * 3;
        double m = v / std::pow(10.0, e);
        return formatNumber(m, s.precision) + "e" + std::to_string(e);
    }
    if (n.isInteger()) return n.num.toString();
    return formatNumber(n.toDouble(), s.precision);
}

}  // namespace

std::string formatNumber(double v, int precision) {
    if (std::isnan(v)) return "not a number";
    if (std::isinf(v)) return v > 0 ? "infinity" : "-infinity";
    if (v == 0) return "0";
    precision = std::clamp(precision, 1, 17);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    std::string s = buf;
    auto e = s.find('e');
    if (e != std::string::npos) {
        std::string mant = s.substr(0, e), ex = s.substr(e + 1);
        if (mant.find('.') != std::string::npos) {
            while (!mant.empty() && mant.back() == '0') mant.pop_back();
            if (!mant.empty() && mant.back() == '.') mant.pop_back();
        }
        bool negExp = !ex.empty() && ex[0] == '-';
        if (!ex.empty() && (ex[0] == '+' || ex[0] == '-')) ex.erase(0, 1);
        while (ex.size() > 1 && ex[0] == '0') ex.erase(0, 1);
        s = mant + "e" + (negExp ? "-" : "") + ex;
    }
    return s;
}

std::string formatQuantity(const Quantity& q, const Settings& s, const std::string& format) {
    if (q.unit.empty() && q.dims.none()) return formatPlain(q.value, s, format);
    if (q.unit.empty()) {
        Number v = q.value;
        return formatPlain(v, s, format) + " " + siUnitText(q.dims);
    }
    Number shown;
    if (q.offset == 0 && q.value.exact && q.scale == std::floor(q.scale) && q.scale >= 1 && q.scale < 9e15) {
        shown = div(q.value, Number(static_cast<std::int64_t>(q.scale)));
    } else {
        double v = q.value.toDouble() / q.scale - q.offset;
        // tidy float noise: 0.30000000000000004 km
        shown = Number::real(std::fabs(v - std::round(v)) < 1e-9 * std::max(1.0, std::fabs(v)) ? std::round(v) : v);
    }
    return formatPlain(shown, s, format) + " " + q.unit;
}

Answer evaluate(State& state, const std::string& input) {
    Answer last;
    bool any = false;
    for (const auto& raw : statements(input)) {
        std::string stmt = trim(raw);
        if (stmt.empty()) continue;
        Answer a;
        std::size_t eq = assignAt(stmt);
        Env env{&state, nullptr, 0};
        if (eq != std::string::npos) {
            std::string lhs = trim(stmt.substr(0, eq)), rhs = trim(stmt.substr(eq + 1));
            if (rhs.empty()) throw std::runtime_error("put something after '='");
            auto open = lhs.find('(');
            if (open != std::string::npos && lhs.back() == ')') {
                std::string name = trim(lhs.substr(0, open));
                if (!isName(name)) throw std::runtime_error("a function name is letters and digits, like f or area");
                if (isBuiltinFunction(name)) throw std::runtime_error(name + " is built in; pick another name");
                UserFunction f;
                std::string inner = lhs.substr(open + 1, lhs.size() - open - 2) + ",";
                std::string cur;
                for (char c : inner) {
                    if (c == ',') {
                        std::string p = trim(cur);
                        if (!p.empty()) {
                            if (!isName(p)) throw std::runtime_error("'" + p + "' can't be a parameter name");
                            f.params.push_back(p);
                        }
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                f.body = rhs;
                parseExpression(rhs);  // a syntax error shows now, not on first use
                state.functions[name] = f;
                a.defined = name;
                std::string params;
                for (const auto& p : f.params) params += (params.empty() ? "" : ", ") + p;
                a.text = name + "(" + params + ") = " + rhs;
            } else {
                if (!isName(lhs)) throw std::runtime_error("the left of '=' must be a name, like x = 5 (use == to compare)");
                if (isBuiltinFunction(lhs) || lhs == "ans" || lhs == "pi" || lhs == "e") throw std::runtime_error(lhs + " is built in; pick another name");
                NodePtr node = parseExpression(rhs);
                Val v = eval(*node, env);
                if (v.isList) {
                    state.lists[lhs] = v.items;
                    state.vars.erase(lhs);
                    a.isList = true;
                    a.list = v.items;
                } else {
                    state.vars[lhs] = v.q;
                    state.lists.erase(lhs);
                    a.value = v.q;
                }
                if (node->kind == Node::Convert && node->kids.size() == 1) a.format = normaliseFormat(node->text);
                a.assigned = lhs;
            }
        } else {
            NodePtr node = parseExpression(stmt);
            Val v = eval(*node, env);
            if (node->kind == Node::Convert && node->kids.size() == 1) a.format = normaliseFormat(node->text);
            a.isList = v.isList;
            a.list = v.items;
            a.value = v.q;
        }
        if (a.defined.empty()) {
            if (a.isList) {
                std::string t = "[";
                for (std::size_t i = 0; i < a.list.size(); ++i) t += (i ? ", " : "") + formatQuantity(a.list[i], state.settings, a.format);
                a.text = t + "]";
            } else {
                a.text = formatQuantity(a.value, state.settings, a.format);
                state.vars["ans"] = a.value;
            }
            if (!a.assigned.empty()) a.text = a.assigned + " = " + a.text;
        }
        state.history.push_back({stmt, a.text});
        if (state.history.size() > 1000) state.history.erase(state.history.begin());
        last = a;
        any = true;
    }
    if (!any) throw std::runtime_error("type something to calculate, like 2 + 2");
    return last;
}

// ---- compiled expressions ---------------------------------------------------------------------

struct Compiled::Impl {
    State state;
    NodePtr node;
};

Compiled::Compiled(const State& state, const std::string& expression) : impl_(std::make_unique<Impl>()) {
    impl_->state.settings = state.settings;
    impl_->state.vars = state.vars;
    impl_->state.lists = state.lists;
    impl_->state.functions = state.functions;
    impl_->state.memory = state.memory;
    impl_->state.hasMemory = state.hasMemory;
    impl_->node = parseExpression(expression);
}
Compiled::~Compiled() = default;
Compiled::Compiled(Compiled&&) noexcept = default;
Compiled& Compiled::operator=(Compiled&&) noexcept = default;

double Compiled::at(const std::map<std::string, double>& vars) const {
    std::map<std::string, Quantity> locals;
    for (const auto& [k, v] : vars) locals[k] = plainDouble(v);
    Env env{&impl_->state, &locals, 0};
    try {
        Val v = eval(*impl_->node, env);
        if (v.isList) return NAN;
        return v.q.value.toDouble();
    } catch (const std::exception&) {
        return NAN;
    }
}

double Compiled::at(const std::string& var, double x) const { return at(std::map<std::string, double>{{var, x}}); }

std::vector<std::string> Compiled::names() const {
    std::set<std::string> out;
    std::function<void(const Node&)> walk = [&](const Node& n) {
        if (n.kind == Node::Name && !n.unitOnly) {
            const std::string& name = n.text;
            bool known = impl_->state.vars.count(name) || impl_->state.lists.count(name) || isConstant(name);
            UnitInfo u;
            if (!known && (name.size() == 1 || !findUnit(name, u))) out.insert(name);
        }
        for (const auto& k : n.kids) walk(*k);
    };
    walk(*impl_->node);
    return {out.begin(), out.end()};
}

double convertUnits(double value, const std::string& from, const std::string& to) {
    State st;
    st.settings.exact = false;
    Answer a = evaluate(st, "(" + formatNumber(value, 17) + ") " + from + " to " + to);
    return a.value.value.toDouble() / a.value.scale - a.value.offset;
}

// ---- statistics ---------------------------------------------------------------------------------

std::vector<double> parseNumbers(const std::string& text) {
    std::vector<double> out;
    std::string cur;
    auto flush = [&] {
        std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        char* end = nullptr;
        double v = std::strtod(t.c_str(), &end);
        if (end == t.c_str() || *end) throw std::runtime_error("'" + t + "' isn't a number");
        out.push_back(v);
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '[' || c == ']') flush();
        else cur += c;
    }
    flush();
    return out;
}

namespace {

double quantile(const std::vector<double>& sorted, double p) {
    // linear between closest ranks (the spreadsheet QUARTILE.INC / numpy default)
    double pos = p * static_cast<double>(sorted.size() - 1);
    std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - static_cast<double>(lo));
}

}  // namespace

Stats describe(std::vector<double> xs) {
    if (xs.empty()) throw std::runtime_error("give some numbers");
    Stats s;
    s.count = xs.size();
    std::sort(xs.begin(), xs.end());
    double logSum = 0;
    bool positive = true;
    for (double x : xs) {
        s.sum += x;
        if (x <= 0) positive = false;
        else logSum += std::log(x);
    }
    const double n = static_cast<double>(xs.size());
    s.mean = s.sum / n;
    s.min = xs.front();
    s.max = xs.back();
    s.range = s.max - s.min;
    s.median = quantile(xs, 0.5);
    s.q1 = quantile(xs, 0.25);
    s.q3 = quantile(xs, 0.75);
    s.iqr = s.q3 - s.q1;
    double ss = 0, cube = 0;
    for (double x : xs) {
        ss += (x - s.mean) * (x - s.mean);
        cube += (x - s.mean) * (x - s.mean) * (x - s.mean);
    }
    s.pvariance = ss / n;
    s.pstdev = std::sqrt(s.pvariance);
    s.variance = xs.size() > 1 ? ss / (n - 1) : 0;
    s.stdev = std::sqrt(s.variance);
    s.geomean = positive ? std::exp(logSum / n) : NAN;
    s.skewness = s.pstdev > 0 ? (cube / n) / (s.pstdev * s.pstdev * s.pstdev) : 0;
    std::size_t best = 0;
    for (std::size_t i = 0; i < xs.size();) {
        std::size_t j = i;
        while (j < xs.size() && xs[j] == xs[i]) ++j;
        if (j - i > best) {
            best = j - i;
            s.modes = {xs[i]};
        } else if (j - i == best) {
            s.modes.push_back(xs[i]);
        }
        i = j;
    }
    if (best == 1) s.modes.clear();  // no value repeats: no mode
    return s;
}

Regression linearRegression(const std::vector<double>& xs, const std::vector<double>& ys) {
    if (xs.size() != ys.size() || xs.size() < 2) throw std::runtime_error("give the same number of x and y values (at least two)");
    const double n = static_cast<double>(xs.size());
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        sx += xs[i];
        sy += ys[i];
        sxx += xs[i] * xs[i];
        syy += ys[i] * ys[i];
        sxy += xs[i] * ys[i];
    }
    Regression r;
    const double dx = n * sxx - sx * sx, dy = n * syy - sy * sy;
    if (dx == 0) throw std::runtime_error("all the x values are the same");
    r.slope = (n * sxy - sx * sy) / dx;
    r.intercept = (sy - r.slope * sx) / n;
    r.r = dy == 0 ? 1 : (n * sxy - sx * sy) / std::sqrt(dx * dy);
    r.r2 = r.r * r.r;
    return r;
}

// ---- solving ----------------------------------------------------------------------------------

namespace {

std::string asZero(const std::string& eq) {
    std::size_t at = assignAt(eq);
    std::size_t dbl = eq.find("==");
    if (at == std::string::npos && dbl != std::string::npos) return "(" + eq.substr(0, dbl) + ") - (" + eq.substr(dbl + 2) + ")";
    if (at == std::string::npos) return eq;
    return "(" + eq.substr(0, at) + ") - (" + eq.substr(at + 1) + ")";
}

double tidy(double x) {
    double r = std::round(x);
    if (std::fabs(x - r) < 1e-9 * std::max(1.0, std::fabs(x))) return r == 0 ? 0 : r;
    return x;
}

std::vector<double> solveOne(const Compiled& f, const std::string& var, double from, double to) {
    std::vector<double> roots;
    auto F = [&](double x) { return f.at(var, x); };
    auto addRoot = [&](double r) {
        double fr = F(r);
        if (!std::isfinite(fr)) return;
        double scale = std::max({1.0, std::fabs(F(r + 1e-3)), std::fabs(F(r - 1e-3))});
        if (std::fabs(fr) > 1e-7 * scale) return;  // a pole or jump (tan x), not a root
        for (double q : roots) {
            if (std::fabs(q - r) < 1e-7 * std::max(1.0, std::fabs(r))) return;
        }
        roots.push_back(tidy(r));
    };
    const int n = 20000;
    double prevX = from, prevF = F(from);
    for (int i = 1; i <= n; ++i) {
        double x = from + (to - from) * i / n;
        double fx = F(x);
        if (std::isfinite(prevF) && prevF == 0) addRoot(prevX);
        if (std::isfinite(prevF) && std::isfinite(fx) && (prevF < 0) != (fx < 0) && fx != 0) {
            double a = prevX, b = x, fa = prevF;
            for (int k = 0; k < 200 && b - a > 1e-15 * std::max(1.0, std::fabs(a)); ++k) {
                double m = (a + b) / 2, fm = F(m);
                if (!std::isfinite(fm)) break;
                if ((fm < 0) == (fa < 0)) {
                    a = m;
                    fa = fm;
                } else {
                    b = m;
                }
            }
            addRoot((a + b) / 2);
        }
        prevX = x;
        prevF = fx;
    }
    if (std::isfinite(prevF) && prevF == 0) addRoot(prevX);
    // roots that only touch zero (x^2 = 0): look for small minima of |f|
    for (int i = 1; i < n; ++i) {
        double x0 = from + (to - from) * (i - 1) / n, x1 = from + (to - from) * i / n, x2 = from + (to - from) * (i + 1) / n;
        double f0 = std::fabs(F(x0)), f1 = std::fabs(F(x1)), f2 = std::fabs(F(x2));
        if (!(f1 <= f0 && f1 <= f2) || !std::isfinite(f1)) continue;
        double a = x0, b = x2;
        for (int k = 0; k < 100; ++k) {  // golden section on |f|
            double c = b - (b - a) * 0.618033988749895, d = a + (b - a) * 0.618033988749895;
            if (std::fabs(F(c)) < std::fabs(F(d))) b = d;
            else a = c;
        }
        double m = (a + b) / 2;
        if (std::fabs(F(m)) < 1e-10) addRoot(m);
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

bool gaussSolve(std::vector<std::vector<double>> a, std::vector<double> b, std::vector<double>& x) {
    const std::size_t n = b.size();
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        }
        if (std::fabs(a[piv][c]) < 1e-300) return false;
        std::swap(a[c], a[piv]);
        std::swap(b[c], b[piv]);
        for (std::size_t r = c + 1; r < n; ++r) {
            double f = a[r][c] / a[c][c];
            for (std::size_t k = c; k < n; ++k) a[r][k] -= f * a[c][k];
            b[r] -= f * b[c];
        }
    }
    x.assign(n, 0);
    for (std::size_t c = n; c-- > 0;) {
        double s = b[c];
        for (std::size_t k = c + 1; k < n; ++k) s -= a[c][k] * x[k];
        x[c] = s / a[c][c];
    }
    return true;
}

// Gauss-Newton from one start; true when it lands on a solution
bool newton(const std::vector<Compiled>& fs, const std::vector<std::string>& vars, std::vector<double>& x) {
    const std::size_t n = vars.size(), m = fs.size();
    auto values = [&](const std::vector<double>& at) {
        std::map<std::string, double> env;
        for (std::size_t i = 0; i < n; ++i) env[vars[i]] = at[i];
        std::vector<double> out(m);
        for (std::size_t j = 0; j < m; ++j) out[j] = fs[j].at(env);
        return out;
    };
    for (int iter = 0; iter < 200; ++iter) {
        std::vector<double> f = values(x);
        double norm = 0;
        for (double v : f) {
            if (!std::isfinite(v)) return false;
            norm += v * v;
        }
        if (norm < 1e-24) return true;
        std::vector<std::vector<double>> J(m, std::vector<double>(n));
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<double> xp = x;
            double h = 1e-7 * std::max(1.0, std::fabs(x[i]));
            xp[i] += h;
            std::vector<double> fp = values(xp);
            for (std::size_t j = 0; j < m; ++j) J[j][i] = (fp[j] - f[j]) / h;
        }
        // normal equations: (J^T J) dx = -J^T f
        std::vector<std::vector<double>> A(n, std::vector<double>(n, 0));
        std::vector<double> g(n, 0);
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t k = 0; k < n; ++k) {
                for (std::size_t j = 0; j < m; ++j) A[i][k] += J[j][i] * J[j][k];
            }
            for (std::size_t j = 0; j < m; ++j) g[i] -= J[j][i] * f[j];
        }
        std::vector<double> dx;
        if (!gaussSolve(A, g, dx)) return false;
        for (std::size_t i = 0; i < n; ++i) x[i] += dx[i];
    }
    std::vector<double> f = values(x);
    double norm = 0;
    for (double v : f) norm += v * v;
    return norm < 1e-16;
}

}  // namespace

Solution solve(const State& state, const std::string& equations, const std::vector<std::string>& wanted, double from, double to) {
    std::vector<Compiled> fs;
    std::set<std::string> seen;
    for (const auto& raw : statements(equations)) {
        std::string eq = trim(raw);
        if (eq.empty()) continue;
        fs.emplace_back(state, asZero(eq));
        for (const auto& n : fs.back().names()) seen.insert(n);
    }
    if (fs.empty()) throw std::runtime_error("give an equation, like x^2 = 4");
    Solution sol;
    sol.vars = wanted.empty() ? std::vector<std::string>(seen.begin(), seen.end()) : wanted;
    // x, y, z first, the way people write them
    std::stable_sort(sol.vars.begin(), sol.vars.end(), [](const std::string& a, const std::string& b) {
        auto rank = [](const std::string& s) { return s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : 3; };
        return rank(a) < rank(b);
    });
    if (sol.vars.empty()) throw std::runtime_error("there's nothing to solve for: use a name like x");
    if (from >= to) throw std::runtime_error("from must be less than to");
    if (sol.vars.size() == 1 && fs.size() == 1) {
        for (double r : solveOne(fs[0], sol.vars[0], from, to)) sol.roots.push_back({r});
        return sol;
    }
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> pick(std::max(from, -100.0), std::min(to, 100.0));
    std::vector<std::vector<double>> starts = {std::vector<double>(sol.vars.size(), 0.0), std::vector<double>(sol.vars.size(), 1.0),
                                               std::vector<double>(sol.vars.size(), -1.0)};
    for (int i = 0; i < 40; ++i) {
        std::vector<double> s(sol.vars.size());
        for (auto& v : s) v = pick(rng);
        starts.push_back(s);
    }
    for (auto x : starts) {
        if (!newton(fs, sol.vars, x)) continue;
        for (auto& v : x) v = tidy(v);
        bool dup = false;
        for (const auto& r : sol.roots) {
            double d = 0;
            for (std::size_t i = 0; i < x.size(); ++i) d = std::max(d, std::fabs(r[i] - x[i]) / std::max(1.0, std::fabs(x[i])));
            dup = dup || d < 1e-6;
        }
        if (!dup) sol.roots.push_back(x);
        if (sol.roots.size() >= 16) break;
    }
    return sol;
}

double integrate(const State& state, const std::string& expression, const std::string& var, double a, double b) {
    Compiled f(state, expression);
    if (!std::isfinite(a) || !std::isfinite(b)) throw std::runtime_error("the limits must be finite numbers");
    std::function<double(double, double, double, double, double, double, int)> simpson =
        [&](double lo, double hi, double flo, double fmid, double fhi, double whole, int depth) -> double {
        double mid = (lo + hi) / 2, lm = (lo + mid) / 2, rm = (mid + hi) / 2;
        double flm = f.at(var, lm), frm = f.at(var, rm);
        double left = (mid - lo) / 6 * (flo + 4 * flm + fmid), right = (hi - mid) / 6 * (fmid + 4 * frm + fhi);
        double diff = left + right - whole;
        if (depth <= 0 || std::fabs(diff) <= 1e-12 * std::max(1.0, std::fabs(left + right))) return left + right + diff / 15;
        return simpson(lo, mid, flo, flm, fmid, left, depth - 1) + simpson(mid, hi, fmid, frm, fhi, right, depth - 1);
    };
    // split into pieces first so narrow features aren't missed
    const int pieces = 64;
    double total = 0;
    for (int i = 0; i < pieces; ++i) {
        double lo = a + (b - a) * i / pieces, hi = a + (b - a) * (i + 1) / pieces;
        double flo = f.at(var, lo), fhi = f.at(var, hi), fmid = f.at(var, (lo + hi) / 2);
        total += simpson(lo, hi, flo, fmid, fhi, (hi - lo) / 6 * (flo + 4 * fmid + fhi), 30);
    }
    return total;
}

double derivative(const State& state, const std::string& expression, const std::string& var, double x, int order) {
    Compiled f(state, expression);
    auto d1 = [&](double h) { return (f.at(var, x + h) - f.at(var, x - h)) / (2 * h); };
    auto d2 = [&](double h) { return (f.at(var, x + h) - 2 * f.at(var, x) + f.at(var, x - h)) / (h * h); };
    double h = 1e-3 * std::max(1.0, std::fabs(x));
    // Richardson extrapolation of the central difference
    if (order == 2) return (16 * d2(h / 2) - d2(h)) / 15;
    return (4 * d1(h / 2) - d1(h)) / 3;
}

// ---- primes ------------------------------------------------------------------------------------

namespace {

BigInt mulMod(const BigInt& a, const BigInt& b, const BigInt& m) { return (a * b) % m; }

BigInt powMod(BigInt base, BigInt exp, const BigInt& m) {
    BigInt result(1);
    base = base % m;
    while (!exp.isZero()) {
        if (exp.isOdd()) result = mulMod(result, base, m);
        exp = exp.shiftRight(1);
        if (!exp.isZero()) base = mulMod(base, base, m);
    }
    return result;
}

BigInt pollard(const BigInt& n) {
    if (!n.isOdd()) return BigInt(2);
    for (std::int64_t c = 1; c < 30; ++c) {
        BigInt x(2), y(2), d(1);
        auto f = [&](const BigInt& v) { return (mulMod(v, v, n) + BigInt(c)) % n; };
        for (int steps = 0; d == BigInt(1) && steps < 200000; ++steps) {
            x = f(x);
            y = f(f(y));
            d = BigInt::gcd((x - y).abs(), n);
        }
        if (d != n && d != BigInt(1)) return d;
    }
    return n;
}

}  // namespace

bool isProbablePrime(const BigInt& n) {
    if (n < BigInt(2)) return false;
    static const int small[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (int p : small) {
        if (n == BigInt(p)) return true;
        if ((n % BigInt(p)).isZero()) return false;
    }
    BigInt d = n - BigInt(1);
    std::size_t r = 0;
    while (!d.isOdd()) {
        d = d.shiftRight(1);
        ++r;
    }
    const BigInt nm1 = n - BigInt(1);
    for (int a : small) {
        BigInt x = powMod(BigInt(a), d, n);
        if (x == BigInt(1) || x == nm1) continue;
        bool composite = true;
        for (std::size_t i = 1; i < r; ++i) {
            x = mulMod(x, x, n);
            if (x == nm1) {
                composite = false;
                break;
            }
        }
        if (composite) return false;
    }
    return true;
}

std::vector<std::pair<BigInt, int>> factorize(const BigInt& input) {
    std::map<std::string, std::pair<BigInt, int>> found;  // keyed by padded decimal so they sort
    auto addFactor = [&](const BigInt& p) {
        std::string key = p.toString();
        key = std::string(400 > key.size() ? 400 - key.size() : 0, '0') + key;
        auto& e = found[key];
        e.first = p;
        ++e.second;
    };
    BigInt n = input.abs();
    if (n < BigInt(2)) return {};
    for (std::int64_t p = 2; p < 10000; ++p) {
        BigInt bp(p);
        if (bp * bp > n) break;
        while ((n % bp).isZero()) {
            addFactor(bp);
            n = n / bp;
        }
    }
    std::vector<BigInt> stack;
    if (n > BigInt(1)) stack.push_back(n);
    while (!stack.empty()) {
        BigInt m = stack.back();
        stack.pop_back();
        if (m == BigInt(1)) continue;
        if (isProbablePrime(m)) {
            addFactor(m);
            continue;
        }
        BigInt d = pollard(m);
        if (d == m) {
            addFactor(m);  // couldn't split it in time: shown as is
            continue;
        }
        stack.push_back(d);
        stack.push_back(m / d);
    }
    std::vector<std::pair<BigInt, int>> out;
    for (auto& [k, v] : found) out.push_back(v);
    return out;
}

std::vector<std::string> splitExpressions(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& raw : statements(text)) {
        std::string s = trim(raw);
        if (s.empty()) continue;
        // "y = x^2" and "f(x) = x^2" plot the right-hand side
        std::size_t eq = assignAt(s);
        if (eq != std::string::npos) {
            std::string lhs = trim(s.substr(0, eq));
            if (lhs == "y" || (lhs.size() > 3 && lhs.substr(lhs.size() - 3) == "(x)")) s = trim(s.substr(eq + 1));
        }
        out.push_back(s);
    }
    return out;
}

}  // namespace wetwaresuite::calc
