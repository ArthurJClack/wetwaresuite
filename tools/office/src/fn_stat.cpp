// Formula functions: counting, averages, spread, ranks and percentiles,
// regression, the normal distribution and the *IF/*IFS family.
#include <algorithm>
#include <cmath>
#include <map>

#include "formula_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office::fx {

namespace {

constexpr double kPi = 3.14159265358979323846;

double mean(const std::vector<double>& v) {
    if (v.empty()) throw EvalError{"#DIV/0!"};
    double s = 0;
    for (double x : v) s += x;
    return s / static_cast<double>(v.size());
}

double variance(const std::vector<double>& v, bool sample) {
    if (v.size() < (sample ? 2u : 1u)) throw EvalError{"#DIV/0!"};
    const double m = mean(v);
    double s = 0;
    for (double x : v) s += (x - m) * (x - m);
    return s / static_cast<double>(v.size() - (sample ? 1 : 0));
}

// The "A" variants count text as 0 and TRUE as 1 when it comes from cells.
std::vector<double> numbersA(const std::vector<NodeP>& args, Ctx& c) {
    std::vector<double> out;
    for (const auto& a : args) {
        if (a->kind == FormulaNode::Missing) continue;
        eachValue(evalArg(a, c), c, [&](const CellValue& v, bool fromRef) {
            if (v.isError()) throw EvalError{v.text};
            if (v.type == CellValue::Number || v.type == CellValue::Bool) out.push_back(v.number);
            else if (v.type == CellValue::Text) {
                if (fromRef) out.push_back(0);
                else out.push_back(toNum(v));
            }
        });
    }
    return out;
}

double percentileInc(std::vector<double> v, double k) {
    if (v.empty() || k < 0 || k > 1) throw EvalError{"#NUM!"};
    std::sort(v.begin(), v.end());
    const double pos = k * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= v.size()) return v.back();
    return v[lo] + frac * (v[lo + 1] - v[lo]);
}

double percentileExc(std::vector<double> v, double k) {
    const double n = static_cast<double>(v.size());
    if (v.empty() || k <= 0 || k >= 1) throw EvalError{"#NUM!"};
    const double pos = k * (n + 1) - 1;
    if (pos < 0 || pos > n - 1) throw EvalError{"#NUM!"};
    std::sort(v.begin(), v.end());
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(lo);
    if (lo + 1 >= v.size()) return v.back();
    return v[lo] + frac * (v[lo + 1] - v[lo]);
}

// Two equally long lists of numbers, pairs with a non-number on either side dropped.
void pairs(const std::vector<NodeP>& a, Ctx& c, std::vector<double>& ys, std::vector<double>& xs) {
    Matrix my = toMatrix(evalArg(a[0], c), c), mx = toMatrix(evalArg(a[1], c), c);
    if (my.v.size() != mx.v.size()) throw EvalError{"#N/A"};
    for (std::size_t i = 0; i < my.v.size(); ++i) {
        if (my.v[i].isError()) throw EvalError{my.v[i].text};
        if (mx.v[i].isError()) throw EvalError{mx.v[i].text};
        if (my.v[i].type == CellValue::Number && mx.v[i].type == CellValue::Number) {
            ys.push_back(my.v[i].number);
            xs.push_back(mx.v[i].number);
        }
    }
    if (ys.empty()) throw EvalError{"#DIV/0!"};
}

struct Fit {
    double slope, intercept, r2, sxx, syy, sxy;
    std::size_t n;
};

Fit fit(const std::vector<double>& ys, const std::vector<double>& xs) {
    const double mx = mean(xs), my = mean(ys);
    double sxx = 0, syy = 0, sxy = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        sxx += (xs[i] - mx) * (xs[i] - mx);
        syy += (ys[i] - my) * (ys[i] - my);
        sxy += (xs[i] - mx) * (ys[i] - my);
    }
    if (sxx == 0) throw EvalError{"#DIV/0!"};
    Fit f;
    f.slope = sxy / sxx;
    f.intercept = my - f.slope * mx;
    f.r2 = syy == 0 ? 1 : sxy * sxy / (sxx * syy);
    f.sxx = sxx;
    f.syy = syy;
    f.sxy = sxy;
    f.n = xs.size();
    return f;
}

double normCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// Acklam's inverse normal, refined by one Halley step.
double normInv(double p) {
    if (p <= 0 || p >= 1) throw EvalError{"#NUM!"};
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02,
                               -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01,
                               -1.328068155288572e+01};
    static const double cc[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00,
                                4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
    double x;
    if (p < 0.02425) {
        const double q = std::sqrt(-2 * std::log(p));
        x = (((((cc[0] * q + cc[1]) * q + cc[2]) * q + cc[3]) * q + cc[4]) * q + cc[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p > 1 - 0.02425) {
        const double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((cc[0] * q + cc[1]) * q + cc[2]) * q + cc[3]) * q + cc[4]) * q + cc[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else {
        const double q = p - 0.5, r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    }
    const double e = normCdf(x) - p;
    const double u = e * std::sqrt(2 * kPi) * std::exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

Fn spread(bool sample, bool sd, bool countText) {
    return [sample, sd, countText](const std::vector<NodeP>& a, Ctx& c) {
        std::vector<double> v = countText ? numbersA(a, c) : numbers(a, c);
        const double var = variance(v, sample);
        return num(sd ? std::sqrt(var) : var);
    };
}

// Which rank a value has in a list: 1 for the largest (or smallest when ascending).
Fn rankFn(bool average) {
    return [average](const std::vector<NodeP>& a, Ctx& c) {
        need(a, 2, 3);
        const double x = evalNum(a[0], c);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[1]}, c, [&](double d) { v.push_back(d); });
        const bool asc = !missing(a, 2) && evalNum(a[2], c) != 0;
        int before = 0, equal = 0;
        for (double d : v) {
            if (d == x) ++equal;
            else if (asc ? d < x : d > x) ++before;
        }
        if (!equal) throw EvalError{"#N/A"};
        return num(average ? before + (equal + 1) / 2.0 : before + 1);
    };
}

void addAverages(FnMap& m) {
    m["AVERAGE"] = [](const auto& a, Ctx& c) { return num(mean(numbers(a, c))); };
    m["AVERAGEA"] = [](const auto& a, Ctx& c) { return num(mean(numbersA(a, c))); };
    auto extreme = [](bool max, bool countText) {
        return [max, countText](const std::vector<NodeP>& a, Ctx& c) {
            std::vector<double> v = countText ? numbersA(a, c) : numbers(a, c);
            if (v.empty()) return num(0);
            return num(max ? *std::max_element(v.begin(), v.end()) : *std::min_element(v.begin(), v.end()));
        };
    };
    m["MIN"] = extreme(false, false);
    m["MAX"] = extreme(true, false);
    m["MINA"] = extreme(false, true);
    m["MAXA"] = extreme(true, true);
    m["MEDIAN"] = [](const auto& a, Ctx& c) {
        std::vector<double> v = numbers(a, c);
        if (v.empty()) throw EvalError{"#NUM!"};
        std::sort(v.begin(), v.end());
        const std::size_t n = v.size();
        return num(n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2);
    };
    m["MODE.SNGL"] = [](const auto& a, Ctx& c) {
        std::vector<double> v = numbers(a, c);
        std::map<double, int> count;
        double best = 0;
        int bestN = 1;
        for (double x : v) {
            const int n = ++count[x];
            if (n > bestN) {
                bestN = n;
                best = x;
            }
        }
        if (bestN < 2) throw EvalError{"#N/A"};
        // first value (in order) reaching the top count
        for (double x : v) {
            if (count[x] == bestN) return num(x);
        }
        return num(best);
    };
    m["MODE"] = m["MODE.SNGL"];
    m["GEOMEAN"] = [](const auto& a, Ctx& c) {
        std::vector<double> v = numbers(a, c);
        if (v.empty()) throw EvalError{"#NUM!"};
        double s = 0;
        for (double x : v) {
            if (x <= 0) throw EvalError{"#NUM!"};
            s += std::log(x);
        }
        return num(std::exp(s / static_cast<double>(v.size())));
    };
    m["HARMEAN"] = [](const auto& a, Ctx& c) {
        std::vector<double> v = numbers(a, c);
        if (v.empty()) throw EvalError{"#NUM!"};
        double s = 0;
        for (double x : v) {
            if (x <= 0) throw EvalError{"#NUM!"};
            s += 1 / x;
        }
        return num(static_cast<double>(v.size()) / s);
    };
    m["AVEDEV"] = [](const auto& a, Ctx& c) {
        std::vector<double> v = numbers(a, c);
        const double mu = mean(v);
        double s = 0;
        for (double x : v) s += std::abs(x - mu);
        return num(s / static_cast<double>(v.size()));
    };
    m["DEVSQ"] = [](const auto& a, Ctx& c) {
        std::vector<double> v = numbers(a, c);
        if (v.empty()) return num(0);
        const double mu = mean(v);
        double s = 0;
        for (double x : v) s += (x - mu) * (x - mu);
        return num(s);
    };
    m["TRIMMEAN"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
        const double p = evalNum(a[1], c);
        if (p < 0 || p >= 1 || v.empty()) throw EvalError{"#NUM!"};
        std::sort(v.begin(), v.end());
        std::size_t cut = static_cast<std::size_t>(std::floor(static_cast<double>(v.size()) * p / 2));
        std::vector<double> mid(v.begin() + static_cast<long>(cut), v.end() - static_cast<long>(cut));
        return num(mean(mid));
    };
    m["STDEV.S"] = spread(true, true, false);
    m["STDEV"] = m["STDEV.S"];
    m["STDEV.P"] = spread(false, true, false);
    m["STDEVP"] = m["STDEV.P"];
    m["STDEVA"] = spread(true, true, true);
    m["STDEVPA"] = spread(false, true, true);
    m["VAR.S"] = spread(true, false, false);
    m["VAR"] = m["VAR.S"];
    m["VAR.P"] = spread(false, false, false);
    m["VARP"] = m["VAR.P"];
    m["VARA"] = spread(true, false, true);
    m["VARPA"] = spread(false, false, true);
}

void addOrder(FnMap& m) {
    auto kth = [](bool largest) {
        return [largest](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 2, 2);
            std::vector<double> v;
            eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
            Arg ks = evalArg(a[1], c);
            auto pick = [&](double kd) {
                const long k = static_cast<long>(std::ceil(kd - 1e-9));
                if (k < 1 || k > static_cast<long>(v.size())) throw EvalError{"#NUM!"};
                std::vector<double> s = v;
                if (largest) std::sort(s.begin(), s.end(), std::greater<double>());
                else std::sort(s.begin(), s.end());
                return s[static_cast<std::size_t>(k - 1)];
            };
            if (ks.multi()) {  // LARGE(A:A, {1,2,3})
                Matrix km = toMatrix(ks, c);
                for (auto& x : km.v) {
                    const CellValue in = x;
                    try {
                        x = CellValue::num(pick(toNum(in)));
                    } catch (const EvalError& e) {
                        x = CellValue::error(e.code);
                    }
                }
                return Arg::array(std::move(km));
            }
            return num(pick(toNum(scalar(ks, c))));
        };
    };
    m["LARGE"] = kth(true);
    m["SMALL"] = kth(false);
    m["RANK.EQ"] = rankFn(false);
    m["RANK"] = m["RANK.EQ"];
    m["RANK.AVG"] = rankFn(true);
    m["PERCENTILE.INC"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
        return num(percentileInc(v, evalNum(a[1], c)));
    };
    m["PERCENTILE"] = m["PERCENTILE.INC"];
    m["PERCENTILE.EXC"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
        return num(percentileExc(v, evalNum(a[1], c)));
    };
    m["QUARTILE.INC"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
        const int q = evalInt(a[1], c);
        if (q < 0 || q > 4) throw EvalError{"#NUM!"};
        return num(percentileInc(v, q / 4.0));
    };
    m["QUARTILE"] = m["QUARTILE.INC"];
    m["QUARTILE.EXC"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
        const int q = evalInt(a[1], c);
        if (q < 1 || q > 3) throw EvalError{"#NUM!"};
        return num(percentileExc(v, q / 4.0));
    };
    m["PERCENTRANK.INC"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        std::vector<double> v;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { v.push_back(d); });
        const double x = evalNum(a[1], c);
        const int digits = missing(a, 2) ? 3 : evalInt(a[2], c);
        if (v.empty()) throw EvalError{"#NUM!"};
        std::sort(v.begin(), v.end());
        if (x < v.front() || x > v.back()) throw EvalError{"#N/A"};
        if (v.size() == 1) return num(1);
        // position by linear interpolation between neighbours
        double rank = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i] == x) {
                rank = static_cast<double>(i);
                break;
            }
            if (i + 1 < v.size() && v[i] < x && x < v[i + 1]) {
                rank = static_cast<double>(i) + (x - v[i]) / (v[i + 1] - v[i]);
                break;
            }
        }
        const double r = rank / static_cast<double>(v.size() - 1);
        return num(roundTo(r, digits, -1));
    };
    m["PERCENTRANK"] = m["PERCENTRANK.INC"];
}

void addRegression(FnMap& m) {
    m["CORREL"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> ys, xs;
        pairs(a, c, ys, xs);
        Fit f = fit(ys, xs);
        if (f.syy == 0) throw EvalError{"#DIV/0!"};
        return num(f.sxy / std::sqrt(f.sxx * f.syy));
    };
    m["PEARSON"] = m["CORREL"];
    m["RSQ"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> ys, xs;
        pairs(a, c, ys, xs);
        return num(fit(ys, xs).r2);
    };
    m["SLOPE"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> ys, xs;
        pairs(a, c, ys, xs);
        return num(fit(ys, xs).slope);
    };
    m["INTERCEPT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> ys, xs;
        pairs(a, c, ys, xs);
        return num(fit(ys, xs).intercept);
    };
    m["STEYX"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> ys, xs;
        pairs(a, c, ys, xs);
        Fit f = fit(ys, xs);
        if (f.n < 3) throw EvalError{"#DIV/0!"};
        return num(std::sqrt((f.syy - f.sxy * f.sxy / f.sxx) / static_cast<double>(f.n - 2)));
    };
    m["FORECAST.LINEAR"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double x = evalNum(a[0], c);
        std::vector<double> ys, xs;
        std::vector<NodeP> rest{a[1], a[2]};
        pairs(rest, c, ys, xs);
        Fit f = fit(ys, xs);
        return num(f.intercept + f.slope * x);
    };
    m["FORECAST"] = m["FORECAST.LINEAR"];
    m["TREND"] = [](const auto& a, Ctx& c) {
        need(a, 1, 3);
        std::vector<double> ys, xs;
        Matrix ym = toMatrix(evalArg(a[0], c), c);
        if (missing(a, 1)) {
            for (std::size_t i = 0; i < ym.v.size(); ++i) {
                ys.push_back(toNum(ym.v[i]));
                xs.push_back(static_cast<double>(i + 1));
            }
        } else {
            std::vector<NodeP> two{a[0], a[1]};
            pairs(two, c, ys, xs);
        }
        Fit f = fit(ys, xs);
        Matrix nx = missing(a, 2) ? (missing(a, 1) ? Matrix() : toMatrix(evalArg(a[1], c), c)) : toMatrix(evalArg(a[2], c), c);
        if (nx.v.empty()) {
            nx = Matrix(ym.rows, ym.cols);
            for (std::size_t i = 0; i < nx.v.size(); ++i) nx.v[i] = CellValue::num(static_cast<double>(i + 1));
        }
        for (auto& v : nx.v) v = CellValue::num(f.intercept + f.slope * toNum(v));
        return Arg::array(std::move(nx));
    };
    auto covariance = [](bool sample) {
        return [sample](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 2, 2);
            std::vector<double> ys, xs;
            pairs(a, c, ys, xs);
            if (sample && ys.size() < 2) throw EvalError{"#DIV/0!"};
            const double my = mean(ys), mx = mean(xs);
            double s = 0;
            for (std::size_t i = 0; i < ys.size(); ++i) s += (ys[i] - my) * (xs[i] - mx);
            return num(s / static_cast<double>(ys.size() - (sample ? 1 : 0)));
        };
    };
    m["COVARIANCE.S"] = covariance(true);
    m["COVARIANCE.P"] = covariance(false);
    m["COVAR"] = covariance(false);
    m["FREQUENCY"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        std::vector<double> data, bins;
        eachNumber(std::vector<NodeP>{a[0]}, c, [&](double d) { data.push_back(d); });
        eachNumber(std::vector<NodeP>{a[1]}, c, [&](double d) { bins.push_back(d); });
        std::vector<double> sorted = bins;
        std::sort(sorted.begin(), sorted.end());
        Matrix out(static_cast<int>(bins.size()) + 1, 1);
        std::vector<double> counts(bins.size() + 1, 0);
        for (double x : data) {
            std::size_t k = static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), x) - sorted.begin());
            counts[k] += 1;
        }
        // report in the bins' own order
        for (std::size_t i = 0; i < bins.size(); ++i) {
            std::size_t k = static_cast<std::size_t>(std::lower_bound(sorted.begin(), sorted.end(), bins[i]) - sorted.begin());
            out.at(static_cast<int>(i), 0) = CellValue::num(counts[k]);
        }
        out.at(static_cast<int>(bins.size()), 0) = CellValue::num(counts.back());
        return Arg::array(std::move(out));
    };
    // normal distribution
    m["NORM.DIST"] = [](const auto& a, Ctx& c) {
        need(a, 4, 4);
        const double x = evalNum(a[0], c), mu = evalNum(a[1], c), sd = evalNum(a[2], c);
        const bool cumulative = evalBool(a[3], c);
        if (sd <= 0) throw EvalError{"#NUM!"};
        const double z = (x - mu) / sd;
        return num(cumulative ? normCdf(z) : std::exp(-z * z / 2) / (sd * std::sqrt(2 * kPi)));
    };
    m["NORMDIST"] = m["NORM.DIST"];
    m["NORM.S.DIST"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        const double z = evalNum(a[0], c);
        const bool cumulative = missing(a, 1) ? true : evalBool(a[1], c);
        return num(cumulative ? normCdf(z) : std::exp(-z * z / 2) / std::sqrt(2 * kPi));
    };
    m["NORMSDIST"] = m["NORM.S.DIST"];
    m["NORM.INV"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double p = evalNum(a[0], c), mu = evalNum(a[1], c), sd = evalNum(a[2], c);
        if (sd <= 0) throw EvalError{"#NUM!"};
        return num(mu + sd * normInv(p));
    };
    m["NORMINV"] = m["NORM.INV"];
    m["NORM.S.INV"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return num(normInv(evalNum(a[0], c)));
    };
    m["NORMSINV"] = m["NORM.S.INV"];
    m["STANDARDIZE"] = [](const auto& a, Ctx& c) {
        need(a, 3, 3);
        const double x = evalNum(a[0], c), mu = evalNum(a[1], c), sd = evalNum(a[2], c);
        if (sd <= 0) throw EvalError{"#NUM!"};
        return num((x - mu) / sd);
    };
}

// ---- the *IF and *IFS family -------------------------------------------------------

// Cells of `other` at the same offset as (row, col) in `base`.
const CellValue& sameOffset(Ctx& ctx, const Range& base, const Range& other, int row, int col) {
    const int rr = other.r1 + (row - base.r1), cc = other.c1 + (col - base.c1);
    if (rr >= kMaxRows || cc >= kMaxCols) return kEmpty;
    return cellValue(ctx, other.sheet, rr, cc);
}

Arg conditional(const std::vector<NodeP>& a, Ctx& ctx, int mode) {  // 0 sum, 1 count, 2 average
    if (a.size() < 2 || (mode == 1 && a.size() != 2) || a.size() > 3) throw EvalError{"#VALUE!"};
    Range base = rangeArg(a[0], ctx);
    Arg critArg = evalArg(a[1], ctx);
    Range target = base;
    if (mode != 1 && !missing(a, 2)) target = rangeArg(a[2], ctx);
    auto run = [&](const CellValue& critValue) {
        Criterion crit(critValue);
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
        if (crit.value.empty() || crit.op == "<>") {
            // blanks can match: walk every cell of the (clipped) range
            Range cr = clipped(ctx, base);
            for (int r = cr.r1; r <= cr.r2; ++r) {
                for (int col = cr.c1; col <= cr.c2; ++col) consider(r, col, cellValue(ctx, cr.sheet, r, col));
            }
        } else {
            eachCell(ctx, base, consider);
        }
        if (mode == 0) return CellValue::num(sum);
        if (mode == 1) return CellValue::num(count);
        if (count == 0) return CellValue::error("#DIV/0!");
        return CellValue::num(sum / count);
    };
    if (critArg.multi()) {  // COUNTIF(A:A, {"a","b"}) gives one result per criterion
        Matrix m = toMatrix(critArg, ctx);
        for (auto& v : m.v) v = run(v);
        return Arg::array(std::move(m));
    }
    CellValue v = run(scalar(critArg, ctx));
    if (v.isError()) throw EvalError{v.text};
    return Arg::val(v);
}

// SUMIFS/AVERAGEIFS/MAXIFS/MINIFS(target, range1, criteria1, ...) and COUNTIFS(range1, criteria1, ...).
Arg multiConditional(const std::vector<NodeP>& a, Ctx& ctx, int mode) {  // 0 sum, 1 count, 2 average, 3 max, 4 min
    const std::size_t first = mode == 1 ? 0 : 1;
    if (a.size() < first + 2 || (a.size() - first) % 2 != 0) throw EvalError{"#VALUE!"};
    std::vector<Range> ranges;
    std::vector<Criterion> crits;
    for (std::size_t i = first; i < a.size(); i += 2) {
        ranges.push_back(rangeArg(a[i], ctx));
        crits.emplace_back(evalScalar(a[i + 1], ctx));
    }
    Range target = ranges[0];
    if (mode != 1) target = rangeArg(a[0], ctx);
    for (const auto& r : ranges) {
        if (r.rows() != target.rows() || r.cols() != target.cols()) throw EvalError{"#VALUE!"};
    }
    // walk only as far as any of the ranges has data
    int rows = 1, cols = 1;
    auto extend = [&](const Range& r) {
        Range cr = clipped(ctx, r);
        rows = std::max(rows, cr.rows());
        cols = std::max(cols, cr.cols());
    };
    extend(target);
    for (const auto& r : ranges) extend(r);
    rows = std::min(rows, target.rows());
    cols = std::min(cols, target.cols());
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

void addCounting(FnMap& m) {
    m["COUNT"] = [](const auto& a, Ctx& c) {
        int n = 0;
        for (const auto& x : a) {
            if (x->kind == FormulaNode::Missing) continue;
            eachValue(evalArg(x, c), c, [&](const CellValue& cv, bool fromRef) {
                if (cv.type == CellValue::Number) ++n;
                else if (!fromRef && cv.type == CellValue::Bool) ++n;
                else if (!fromRef && cv.type == CellValue::Text) {
                    double d;
                    n += parseNumber(cv.text, d) ? 1 : 0;
                }
            });
        }
        return num(n);
    };
    m["COUNTA"] = [](const auto& a, Ctx& c) {
        int n = 0;
        for (const auto& x : a) {
            if (x->kind == FormulaNode::Missing) continue;
            eachValue(evalArg(x, c), c, [&](const CellValue& cv, bool) { n += cv.empty() ? 0 : 1; });
        }
        return num(n);
    };
    m["COUNTBLANK"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Arg v = evalArg(a[0], c);
        if (!v.isRange) {
            Matrix mm = toMatrix(v, c);
            int n = 0;
            for (const auto& x : mm.v) n += (x.empty() || (x.type == CellValue::Text && x.text.empty())) ? 1 : 0;
            return num(n);
        }
        const Range& r = v.r;
        double total = static_cast<double>(r.rows()) * r.cols();
        int filled = 0;
        eachCell(c, r, [&](int, int, const CellValue& cv) { filled += (cv.type == CellValue::Text && cv.text.empty()) ? 0 : 1; });
        return num(total - filled);
    };
    m["SUMIF"] = [](const auto& a, Ctx& c) { return conditional(a, c, 0); };
    m["COUNTIF"] = [](const auto& a, Ctx& c) { return conditional(a, c, 1); };
    m["AVERAGEIF"] = [](const auto& a, Ctx& c) { return conditional(a, c, 2); };
    m["SUMIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 0); };
    m["COUNTIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 1); };
    m["AVERAGEIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 2); };
    m["MAXIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 3); };
    m["MINIFS"] = [](const auto& a, Ctx& c) { return multiConditional(a, c, 4); };
}

}  // namespace

void addStatFunctions(FnMap& m) {
    addAverages(m);
    addOrder(m);
    addRegression(m);
    addCounting(m);
}

}  // namespace wetwaresuite::office::fx
