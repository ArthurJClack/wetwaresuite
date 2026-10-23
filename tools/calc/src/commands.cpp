// The calc.* commands. Changes to what the calculator remembers (variables,
// functions, memory, history, settings) are undoable: each keeps a copy of the
// state from before it ran.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include "internal.hpp"
#include "wetwaresuite/sysutil/sysutil.hpp"

namespace wetwaresuite::calc {

namespace {

using StatePtr = std::shared_ptr<State>;
using ReadFn = std::function<Result(State&, const Params&)>;

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

Result guarded(const ReadFn& fn, State& st, const Params& p) {
    try {
        return fn(st, p);
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
}

ActionFactory reader(StatePtr st, ReadFn fn) {
    return [st, fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([st, fn, p] { return guarded(fn, *st, p); });
    };
}

// keeps the state from before, so undo puts every variable, the memory and the history back
class Change : public Action {
public:
    Change(StatePtr st, Params p, ReadFn fn) : st_(std::move(st)), p_(std::move(p)), fn_(std::move(fn)) {}
    Result apply() override {
        before_ = *st_;
        Result r = guarded(fn_, *st_, p_);
        if (!r.ok) *st_ = before_;
        return r;
    }
    void revert() override { *st_ = before_; }

private:
    StatePtr st_;
    Params p_;
    ReadFn fn_;
    State before_;
};

ActionFactory changer(StatePtr st, ReadFn fn) {
    return [st, fn](const Params& p) -> std::unique_ptr<Action> { return std::make_unique<Change>(st, p, fn); };
}

ParamSpec opt(const std::string& name, ParamType type, const std::string& what, Value def = {}) {
    return {name, type, what, false, {}, std::move(def)};
}

std::string angleName(Angle a) { return a == Angle::Degrees ? "deg" : a == Angle::Radians ? "rad" : "grad"; }

// the answer's other forms: exact fraction, hex/bin/oct of whole numbers
Result answerResult(const Answer& a, const State& st) {
    // a quantity with a unit is reported in that unit (5 km to mi -> 3.1), not in SI
    const Quantity& shownQ = a.value;
    const double shown = shownQ.unit.empty() ? shownQ.value.toDouble() : shownQ.value.toDouble() / shownQ.scale - shownQ.offset;
    Result r = Result::success(a.text, a.isList ? Value(a.text) : Value(shown));
    r.with("text", a.text);
    if (a.isList) {
        NumberList xs;
        for (const auto& q : a.list) xs.push_back(q.value.toDouble());
        r.with("values", xs);
        return r;
    }
    if (!a.defined.empty()) return r.with("defined", a.defined);
    const Quantity& q = a.value;
    r.with("value", shown);
    if (q.value.exact && !q.value.isInteger() && q.unit.empty() && q.dims.none()) {
        // a fraction is only worth showing while it is short enough to read
        const std::string frac = q.value.num.toString() + "/" + q.value.den.toString();
        if (frac.size() <= 40) r.with("exact", frac);
    }
    if (!q.unit.empty() || !q.dims.none()) r.with("unit", q.unit.empty() ? detail::siUnitText(q.dims) : q.unit);
    if (q.unit.empty() && q.dims.none() && q.value.isInteger() && q.value.num.bits() <= 4096) {
        Settings s = st.settings;
        r.with("dec", q.value.num.toString());
        for (const char* b : {"hex", "bin", "oct"}) r.with(b, formatQuantity(q, s, b));
    }
    if (!a.assigned.empty()) r.with("assigned", a.assigned);
    return r;
}

std::vector<double> numbersFrom(const Params& p, const std::string& listKey, const std::string& textKey) {
    std::vector<double> xs;
    if (p.count(listKey)) xs = listParam(p, listKey);
    if (p.count(textKey)) {
        auto more = parseNumbers(stringParam(p, textKey));
        xs.insert(xs.end(), more.begin(), more.end());
    }
    return xs;
}

// one column of numbers from a CSV/TSV file (1-based column; 0 = the first column with numbers)
std::vector<double> numbersFromFile(const std::string& path, int column) {
    std::ifstream in(sysutil::toPath(path), std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + path);
    std::vector<double> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        char sep = line.find('\t') != std::string::npos ? '\t' : line.find(';') != std::string::npos && line.find(',') == std::string::npos ? ';' : ',';
        std::vector<std::string> cells;
        std::string cur;
        for (char c : line + sep) {
            if (c == sep) {
                cells.push_back(trim(cur));
                cur.clear();
            } else if (c != '"') {
                cur += c;
            }
        }
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (column > 0 && static_cast<int>(i) != column - 1) continue;
            char* end = nullptr;
            double v = std::strtod(cells[i].c_str(), &end);
            if (cells[i].empty() || *end) continue;
            out.push_back(v);
            if (column <= 0) {
                column = static_cast<int>(i) + 1;  // stick to the first column that had a number
                break;
            }
        }
    }
    if (out.empty()) throw std::runtime_error("no numbers found in " + path);
    return out;
}

std::string listText(const std::vector<double>& xs, int precision) {
    std::string out;
    for (double x : xs) out += (out.empty() ? "" : ", ") + formatNumber(x, precision);
    return out;
}

void writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream out(sysutil::toPath(path), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("can't write " + path);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void registerEval(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"calc.eval", "Calculate",
             "Work out an expression: 2+2, 3/4 + 1/6 (stays an exact fraction), 2^100, 17!, sqrt(2), sin(30) (degrees unless "
             "calc.settings angle=rad), 15% of things (200 + 10% = 220), 0xff & 0b1010, 255 to hex, 5 km to mi, 60 mph to km/h, "
             "20 degC to degF, x = 5 (remembered), f(x) = x^2 + 1 (your own function), mean([4, 8, 15]). ans is the last "
             "answer, mem the memory. Several lines or ';' run in order. Returns the answer with its exact, hex and binary forms.",
             {{"expression", ParamType::String, "what to work out, e.g. (3 + 4) * 2 or 12 km/h to mph", true}},
             true, false},
            changer(st, [](State& s, const Params& p) {
                Answer a = evaluate(s, stringParam(p, "expression"));
                return answerResult(a, s);
            }));

    reg.add({"calc.let", "Set a variable",
             "Give a name a value, worked out from an expression (x = 2 * pi). The name can then be used in any calculation.",
             {{"name", ParamType::String, "the variable's name, like x or rate", true},
              {"value", ParamType::String, "an expression for its value, e.g. 3.5 or 2 km", true}},
             true, false},
            changer(st, [](State& s, const Params& p) {
                Answer a = evaluate(s, trim(stringParam(p, "name")) + " = " + stringParam(p, "value"));
                return answerResult(a, s);
            }));

    reg.add({"calc.define", "Define a function",
             "Make your own function, e.g. function=\"area(w, h)\" body=\"w * h\", then use area(3, 4) anywhere.",
             {{"function", ParamType::String, "the name and parameters, e.g. f(x) or bmi(kg, m)", true},
              {"body", ParamType::String, "the expression, using the parameters", true}},
             true, false},
            changer(st, [](State& s, const Params& p) {
                Answer a = evaluate(s, trim(stringParam(p, "function")) + " = " + stringParam(p, "body"));
                return Result::success("defined " + a.text, a.text).with("defined", a.defined);
            }));

    reg.add({"calc.forget", "Forget a variable",
             "Forget a variable, list or function by name; name=all forgets every one of them.",
             {{"name", ParamType::String, "the name, or all", true}},
             true, false},
            changer(st, [](State& s, const Params& p) {
                std::string n = trim(stringParam(p, "name"));
                if (n == "all") {
                    std::size_t count = s.vars.size() + s.lists.size() + s.functions.size();
                    s.vars.clear();
                    s.lists.clear();
                    s.functions.clear();
                    return Result::success("forgot " + std::to_string(count) + " names");
                }
                std::size_t gone = s.vars.erase(n) + s.lists.erase(n) + s.functions.erase(n);
                if (!gone) return Result::failure("nothing is called " + n);
                return Result::success("forgot " + n);
            }));

    reg.add({"calc.vars", "Variables", "List the variables, lists and functions the calculator remembers.", {}, false, true},
            reader(st, [](State& s, const Params&) {
                std::string text;
                std::string names;
                for (const auto& [k, v] : s.vars) {
                    text += k + " = " + formatQuantity(v, s.settings) + "\n";
                    names += (names.empty() ? "" : ",") + k;
                }
                for (const auto& [k, v] : s.lists) {
                    text += k + " = [";
                    for (std::size_t i = 0; i < v.size(); ++i) text += (i ? ", " : "") + formatQuantity(v[i], s.settings);
                    text += "]\n";
                    names += (names.empty() ? "" : ",") + k;
                }
                for (const auto& [k, f] : s.functions) {
                    std::string ps;
                    for (const auto& x : f.params) ps += (ps.empty() ? "" : ", ") + x;
                    text += k + "(" + ps + ") = " + f.body + "\n";
                    names += (names.empty() ? "" : ",") + k + "()";
                }
                if (text.empty()) text = "no variables yet (try x = 5)";
                return Result::success(trim(text)).with("names", names).with("count", static_cast<std::int64_t>(s.vars.size() + s.lists.size() + s.functions.size()));
            }));
}

void registerMemory(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"calc.history", "History", "The calculations so far, newest last.",
             {opt("limit", ParamType::Int, "how many of the newest to show (0 = all)", std::int64_t{50})}, false, true},
            reader(st, [](State& s, const Params& p) {
                std::size_t limit = static_cast<std::size_t>(std::max<std::int64_t>(0, intParam(p, "limit", 50)));
                std::size_t start = limit && s.history.size() > limit ? s.history.size() - limit : 0;
                std::string text, inputs, outputs;
                for (std::size_t i = start; i < s.history.size(); ++i) {
                    text += s.history[i].input + "  =  " + s.history[i].output + "\n";
                    inputs += (inputs.empty() ? "" : "\n") + s.history[i].input;
                    outputs += (outputs.empty() ? "" : "\n") + s.history[i].output;
                }
                if (text.empty()) text = "nothing calculated yet";
                return Result::success(trim(text)).with("count", static_cast<std::int64_t>(s.history.size())).with("inputs", inputs).with("outputs", outputs);
            }));

    reg.add({"calc.clear_history", "Clear history", "Forget the list of past calculations (variables stay).", {}, true, false},
            changer(st, [](State& s, const Params&) {
                std::size_t n = s.history.size();
                s.history.clear();
                return Result::success("cleared " + std::to_string(n) + " calculations");
            }));

    reg.add({"calc.memory", "Memory",
             "The memory key: store (MS), add (M+), subtract (M-), recall (MR) or clear (MC). value= is an expression (default: "
             "the last answer). The memory is also called mem in calculations.",
             {{"action", ParamType::String, "what to do", false, {"store", "add", "subtract", "recall", "clear"}, std::string("recall")},
              opt("value", ParamType::String, "an expression (default: ans, the last answer)")},
             true, false},
            changer(st, [](State& s, const Params& p) {
                std::string action = stringParam(p, "action", "recall");
                if (action == "clear") {
                    s.hasMemory = false;
                    s.memory = Quantity{};
                    return Result::success("memory cleared");
                }
                if (action == "recall") {
                    if (!s.hasMemory) return Result::success("memory is empty", 0.0).with("empty", true);
                    std::string t = formatQuantity(s.memory, s.settings);
                    return Result::success(t, s.memory.value.toDouble()).with("text", t).with("value", s.memory.value.toDouble());
                }
                std::string expr = p.count("value") && !trim(stringParam(p, "value")).empty() ? stringParam(p, "value") : "ans";
                State scratch = s;  // working out the value isn't a calculation in the history
                Answer a = evaluate(scratch, expr);
                if (a.isList) return Result::failure("the memory holds one value, not a list");
                if (action == "store" || !s.hasMemory) {
                    if (action == "subtract") a.value.value = neg(a.value.value);
                    s.memory = a.value;
                } else {
                    Answer sum = evaluate(scratch, std::string("mem ") + (action == "add" ? "+" : "-") + " (" + expr + ")");
                    s.memory = sum.value;
                }
                s.hasMemory = true;
                std::string t = formatQuantity(s.memory, s.settings);
                return Result::success("memory = " + t, s.memory.value.toDouble()).with("text", t).with("value", s.memory.value.toDouble());
            }));

    reg.add({"calc.settings", "Calculator settings",
             "How answers are worked out and shown: angle (deg, rad or grad for sin/cos/tan), precision (digits shown), base "
             "(dec, hex, bin, oct for whole numbers: programmer mode), bits (word size for negative hex and ~: 8, 16, 32, 64 or "
             "0 for unlimited), exact (keep fractions exact). Without any it shows the current ones.",
             {{"angle", ParamType::String, "angle unit for trig", false, {"deg", "rad", "grad"}},
              opt("precision", ParamType::Int, "significant digits shown, 1..17"),
              {"base", ParamType::String, "how whole numbers are shown", false, {"dec", "hex", "bin", "oct"}},
              opt("bits", ParamType::Int, "word size: 8, 16, 32, 64, or 0 for unlimited"),
              opt("exact", ParamType::Bool, "keep fractions like 1/3 exact")},
             true, false},
            changer(st, [](State& s, const Params& p) {
                Settings& t = s.settings;
                if (p.count("angle")) {
                    std::string a = stringParam(p, "angle");
                    t.angle = a == "rad" ? Angle::Radians : a == "grad" ? Angle::Gradians : Angle::Degrees;
                }
                if (p.count("precision")) t.precision = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "precision"), 1, 17));
                if (p.count("base")) t.base = stringParam(p, "base");
                if (p.count("bits")) {
                    std::int64_t b = intParam(p, "bits");
                    if (b != 0 && b != 8 && b != 16 && b != 32 && b != 64 && b != 128) return Result::failure("bits is 8, 16, 32, 64, 128 or 0");
                    t.bits = static_cast<int>(b);
                }
                if (p.count("exact")) t.exact = boolParam(p, "exact");
                std::string text = "angle " + angleName(t.angle) + ", " + std::to_string(t.precision) + " digits, base " + t.base +
                                   ", " + (t.bits ? std::to_string(t.bits) + " bits" : std::string("unlimited bits")) + ", " +
                                   (t.exact ? "exact fractions" : "decimals only");
                return Result::success(text)
                    .with("angle", angleName(t.angle))
                    .with("precision", static_cast<std::int64_t>(t.precision))
                    .with("base", t.base)
                    .with("bits", static_cast<std::int64_t>(t.bits))
                    .with("exact", t.exact);
            }));
}

void registerUnits(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"calc.convert", "Convert units",
             "Convert a value between units: 5 km to mi, 100 degF to degC, 1 GiB to MB, 60 mph to m/s, 1 acre to m^2. Units can "
             "be combined with * / ^ (kg*m/s^2). All offline; there is no currency.",
             {{"value", ParamType::String, "the amount, a number or an expression", true},
              {"from", ParamType::String, "the unit it is in, e.g. km, degF, lb, km/h", true},
              {"to", ParamType::String, "the unit you want, e.g. mi, degC, kg, mph", true}},
             false, true},
            reader(st, [](State& s, const Params& p) {
                State scratch = s;
                Answer a = evaluate(scratch, "(" + stringParam(p, "value") + ") " + stringParam(p, "from") + " to " + stringParam(p, "to"));
                double shown = a.value.value.toDouble() / a.value.scale - a.value.offset;
                return Result::success(stringParam(p, "value") + " " + stringParam(p, "from") + " = " + a.text, shown)
                    .with("value", shown)
                    .with("text", a.text)
                    .with("unit", stringParam(p, "to"));
            }));

    reg.add({"calc.units", "List units",
             "The units the calculator knows, by category (length, mass, time, temperature, data, ...). SI prefixes (k, M, m, u, "
             "n ...) and binary prefixes (Ki, Mi, Gi) work on the units that take them, and plurals work (metres, hours).",
             {opt("category", ParamType::String, "only this category, e.g. length")}, false, true},
            reader(st, [](State&, const Params& p) {
                std::string cat = stringParam(p, "category");
                std::string text;
                std::int64_t count = 0;
                for (const auto& c : unitCategories()) {
                    if (!cat.empty() && c != cat) continue;
                    text += c + ":";
                    for (const auto& u : unitList(c)) {
                        text += " " + u.name + " (" + u.title + ")";
                        ++count;
                    }
                    text += "\n";
                }
                if (!count) return Result::failure("no category called " + cat + "; categories: length, mass, time, ...");
                std::string cats;
                for (const auto& c : unitCategories()) cats += (cats.empty() ? "" : ",") + c;
                return Result::success(trim(text)).with("count", count).with("categories", cats);
            }));

    reg.add({"calc.base", "Number bases",
             "Show a whole number in decimal, hex, binary and octal (programmer mode). value= is an expression like 0xff, "
             "0b1010 << 2 or 255; negative numbers use the bits= word size (two's complement).",
             {{"value", ParamType::String, "a whole number or expression", true},
              opt("bits", ParamType::Int, "word size for negative numbers (default: calc.settings bits)")},
             false, true},
            reader(st, [](State& s, const Params& p) {
                State scratch = s;
                if (p.count("bits")) scratch.settings.bits = static_cast<int>(intParam(p, "bits"));
                Answer a = evaluate(scratch, stringParam(p, "value"));
                if (!a.value.value.isInteger()) return Result::failure("that isn't a whole number: " + a.text);
                const Quantity& q = a.value;
                std::string dec = q.value.num.toString(), hex = formatQuantity(q, scratch.settings, "hex"),
                            bin = formatQuantity(q, scratch.settings, "bin"), oct = formatQuantity(q, scratch.settings, "oct");
                // counted from the binary as shown, so negatives count their two's complement bits
                std::int64_t digits = 0, ones = 0;
                for (std::size_t i = bin.find('b') == std::string::npos ? 0 : bin.find('b') + 1; i < bin.size(); ++i) {
                    if (bin[i] == '0' || bin[i] == '1') ++digits;
                    if (bin[i] == '1') ++ones;
                }
                return Result::success("dec " + dec + "\nhex " + hex + "\nbin " + bin + "\noct " + oct)
                    .with("dec", dec).with("hex", hex).with("bin", bin).with("oct", oct)
                    .with("bits_used", digits)
                    .with("ones", ones);
            }));

    reg.add({"calc.factor", "Prime factors", "Split a whole number into prime factors (360 = 2^3 * 3^2 * 5) and say if it is prime.",
             {{"value", ParamType::String, "a whole number or expression, e.g. 2^64 + 1", true}}, false, true},
            reader(st, [](State& s, const Params& p) {
                State scratch = s;
                Answer a = evaluate(scratch, stringParam(p, "value"));
                if (!a.value.value.isInteger()) return Result::failure("that isn't a whole number: " + a.text);
                const BigInt& n = a.value.value.num;
                auto f = factorize(n);
                std::string text;
                for (const auto& [prime, e] : f) text += (text.empty() ? "" : " * ") + prime.toString() + (e > 1 ? "^" + std::to_string(e) : "");
                if (n.negative()) text = "-1" + std::string(text.empty() ? "" : " * ") + text;
                if (text.empty()) text = n.toString();
                bool prime = isProbablePrime(n);
                return Result::success(n.toString() + " = " + text + (prime ? " (prime)" : ""), text).with("factors", text).with("prime", prime);
            }));
}

void registerStats(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"calc.stats", "Statistics",
             "Describe a list of numbers: count, sum, mean, median, mode, min, max, range, sample and population variance and "
             "standard deviation, quartiles, IQR, geometric mean, skewness. Give values=[...], data=\"1, 2, 3\" or a CSV file "
             "(file= and column=).",
             {opt("values", ParamType::NumberList, "the numbers"), opt("data", ParamType::String, "numbers as text: 1, 2, 3 or one per line"),
              opt("file", ParamType::String, "a CSV or TSV file"), opt("column", ParamType::Int, "which column of the file (1 = first; default: the first with numbers)")},
             false, true},
            reader(st, [](State& s, const Params& p) {
                auto xs = numbersFrom(p, "values", "data");
                if (p.count("file")) {
                    auto more = numbersFromFile(stringParam(p, "file"), static_cast<int>(intParam(p, "column", 0)));
                    xs.insert(xs.end(), more.begin(), more.end());
                }
                Stats d = describe(xs);
                const int pr = s.settings.precision;
                auto f = [pr](double v) { return formatNumber(v, pr); };
                std::string modes = d.modes.empty() ? "none" : listText(d.modes, pr);
                std::string text = "count " + std::to_string(d.count) + "\nsum " + f(d.sum) + "\nmean " + f(d.mean) + "\nmedian " + f(d.median) +
                                   "\nmode " + modes + "\nmin " + f(d.min) + "\nmax " + f(d.max) + "\nrange " + f(d.range) +
                                   "\nstdev " + f(d.stdev) + " (sample)\npstdev " + f(d.pstdev) + " (population)\nvariance " + f(d.variance) +
                                   "\nq1 " + f(d.q1) + "\nq3 " + f(d.q3) + "\niqr " + f(d.iqr) +
                                   (std::isnan(d.geomean) ? std::string() : "\ngeomean " + f(d.geomean)) + "\nskewness " + f(d.skewness);
                return Result::success(text)
                    .with("count", static_cast<std::int64_t>(d.count)).with("sum", d.sum).with("mean", d.mean).with("median", d.median)
                    .with("mode", modes).with("min", d.min).with("max", d.max).with("range", d.range).with("stdev", d.stdev)
                    .with("pstdev", d.pstdev).with("variance", d.variance).with("pvariance", d.pvariance).with("q1", d.q1)
                    .with("q3", d.q3).with("iqr", d.iqr).with("skewness", d.skewness);
            }));

    reg.add({"calc.regression", "Line of best fit",
             "Fit a straight line y = slope*x + intercept through points (least squares) and say how well it fits (r, r^2).",
             {opt("xs", ParamType::NumberList, "x values"), opt("ys", ParamType::NumberList, "y values"),
              opt("x_data", ParamType::String, "x values as text"), opt("y_data", ParamType::String, "y values as text"),
              opt("predict", ParamType::Number, "also give y at this x")},
             false, true},
            reader(st, [](State& s, const Params& p) {
                auto xs = numbersFrom(p, "xs", "x_data"), ys = numbersFrom(p, "ys", "y_data");
                Regression r = linearRegression(xs, ys);
                const int pr = s.settings.precision;
                std::string text = "y = " + formatNumber(r.slope, pr) + "x " + (r.intercept < 0 ? "- " : "+ ") + formatNumber(std::fabs(r.intercept), pr) +
                                   "\nr = " + formatNumber(r.r, pr) + ", r^2 = " + formatNumber(r.r2, pr);
                Result res = Result::success(text).with("slope", r.slope).with("intercept", r.intercept).with("r", r.r).with("r2", r.r2);
                if (p.count("predict")) {
                    double x = numberParam(p, "predict"), y = r.slope * x + r.intercept;
                    res.message += "\nat x = " + formatNumber(x, pr) + ", y = " + formatNumber(y, pr);
                    res.with("predicted", y);
                }
                return res;
            }));
}

void registerSolve(CommandRegistry& reg, const StatePtr& st) {
    reg.add({"calc.solve", "Solve an equation",
             "Find the values that make an equation true: x^2 = 2 (both roots), 2x + 3 = 11, sin(x) = 0.5, or a system with ';' "
             "like \"2x + y = 5; x - y = 1\". One unknown: every real root between from and to. Systems: Newton's method from "
             "several starts. Unknowns are found automatically (or give vars=x,y).",
             {{"equation", ParamType::String, "the equation(s); without '=' it means = 0", true},
              opt("vars", ParamType::String, "the unknowns, comma separated (default: the names the equation uses)"),
              opt("from", ParamType::Number, "lowest value to look at", -1000.0), opt("to", ParamType::Number, "highest value to look at", 1000.0)},
             false, true},
            reader(st, [](State& s, const Params& p) {
                std::vector<std::string> vars;
                std::string cur;
                for (char c : stringParam(p, "vars") + ",") {
                    if (c == ',' || c == ' ') {
                        if (!cur.empty()) vars.push_back(cur);
                        cur.clear();
                    } else {
                        cur += c;
                    }
                }
                Solution sol = solve(s, stringParam(p, "equation"), vars, numberParam(p, "from", -1000), numberParam(p, "to", 1000));
                std::string text, names;
                for (const auto& v : sol.vars) names += (names.empty() ? "" : ",") + v;
                NumberList flat;
                for (const auto& r : sol.roots) {
                    std::string line;
                    for (std::size_t i = 0; i < r.size(); ++i) {
                        line += (i ? ", " : "") + sol.vars[i] + " = " + formatNumber(r[i], s.settings.precision);
                        flat.push_back(r[i]);
                    }
                    text += line + "\n";
                }
                if (sol.roots.empty()) text = "no real solution found between " + formatNumber(numberParam(p, "from", -1000), 6) + " and " + formatNumber(numberParam(p, "to", 1000), 6);
                return Result::success(trim(text)).with("vars", names).with("solutions", static_cast<std::int64_t>(sol.roots.size())).with("values", flat);
            }));

    reg.add({"calc.integrate", "Integral", "The definite integral of an expression from a to b (adaptive Simpson), e.g. x^2 from 0 to 3 = 9.",
             {{"expression", ParamType::String, "the expression, e.g. sin(x) or exp(-x^2)", true},
              opt("var", ParamType::String, "the variable", std::string("x")),
              {"from", ParamType::Number, "lower limit", true}, {"to", ParamType::Number, "upper limit", true}},
             false, true},
            reader(st, [](State& s, const Params& p) {
                State scratch = s;
                if (scratch.settings.angle == Angle::Degrees) scratch.settings.angle = Angle::Radians;  // calculus is in radians
                double v = integrate(scratch, stringParam(p, "expression"), stringParam(p, "var", "x"), numberParam(p, "from"), numberParam(p, "to"));
                std::string t = formatNumber(std::fabs(v - std::round(v)) < 1e-9 ? std::round(v) : v, s.settings.precision);
                return Result::success("integral = " + t, v).with("value", v).with("text", t);
            }));

    reg.add({"calc.derivative", "Derivative", "The slope of an expression at a point (numerical, first or second derivative), e.g. x^3 at 2 = 12.",
             {{"expression", ParamType::String, "the expression", true}, opt("var", ParamType::String, "the variable", std::string("x")),
              {"at", ParamType::Number, "where", true}, opt("order", ParamType::Int, "1 or 2", std::int64_t{1})},
             false, true},
            reader(st, [](State& s, const Params& p) {
                State scratch = s;
                if (scratch.settings.angle == Angle::Degrees) scratch.settings.angle = Angle::Radians;
                double v = derivative(scratch, stringParam(p, "expression"), stringParam(p, "var", "x"), numberParam(p, "at"),
                                      intParam(p, "order", 1) == 2 ? 2 : 1);
                double shown = std::fabs(v - std::round(v)) < 1e-6 ? std::round(v) : v;
                std::string t = formatNumber(shown, std::min(s.settings.precision, 10));
                return Result::success("derivative = " + t, shown).with("value", shown).with("text", t);
            }));

    reg.add({"calc.table", "Table of values", "y for x from a to b in steps, like a graphing calculator's table.",
             {{"expression", ParamType::String, "the expression in x", true}, opt("var", ParamType::String, "the variable", std::string("x")),
              opt("from", ParamType::Number, "first x", 0.0), opt("to", ParamType::Number, "last x", 10.0), opt("step", ParamType::Number, "step", 1.0)},
             false, true},
            reader(st, [](State& s, const Params& p) {
                Compiled c(s, stringParam(p, "expression"));
                double a = numberParam(p, "from", 0), b = numberParam(p, "to", 10), step = numberParam(p, "step", 1);
                if (step <= 0 || (b - a) / step > 10000) return Result::failure("step must be positive and give at most 10000 rows");
                std::string var = stringParam(p, "var", "x"), text;
                NumberList xs, ys;
                for (int i = 0; a + i * step <= b + step * 1e-9; ++i) {
                    double x = a + i * step, y = c.at(var, x);
                    xs.push_back(x);
                    ys.push_back(y);
                    text += var + " = " + formatNumber(x, 10) + "  ->  " + formatNumber(y, s.settings.precision) + "\n";
                }
                return Result::success(trim(text)).with("xs", xs).with("ys", ys);
            }));
}

void registerGraph(CommandRegistry& reg, const StatePtr& st) {
    auto graphParams = [] {
        return std::vector<ParamSpec>{
            {"expressions", ParamType::String, "what to draw, in x: sin(x); x^2/10 (';' or new lines between curves; y = ... works too)", true},
            opt("xmin", ParamType::Number, "left edge", -10.0), opt("xmax", ParamType::Number, "right edge", 10.0),
            opt("ymin", ParamType::Number, "bottom edge (with ymax; default: fit the curves)"), opt("ymax", ParamType::Number, "top edge"),
        };
    };
    auto options = [](const Params& p) {
        GraphOptions o;
        o.xmin = numberParam(p, "xmin", -10);
        o.xmax = numberParam(p, "xmax", 10);
        if (p.count("ymin") && p.count("ymax")) {
            o.ymin = numberParam(p, "ymin");
            o.ymax = numberParam(p, "ymax");
        }
        if (!(o.xmax > o.xmin)) throw std::runtime_error("xmax must be bigger than xmin");
        return o;
    };
    std::vector<ParamSpec> g = graphParams();
    g.push_back(opt("width", ParamType::Int, "picture width in px", std::int64_t{800}));
    g.push_back(opt("height", ParamType::Int, "picture height in px", std::int64_t{500}));
    g.push_back(opt("dark", ParamType::Bool, "dark background", false));
    g.push_back(opt("output", ParamType::String, "save the PNG here (without it, the picture comes back in the result)"));
    reg.add({"calc.graph", "Draw a graph",
             "Draw y = f(x) for one or more expressions as a PNG with axes, grid, numbers and a legend. Trig uses radians here "
             "unless the angle setting is grad. Save it with output=, or get the picture back.",
             g, false, false},
            reader(st, [options](State& s, const Params& p) {
                GraphOptions o = options(p);
                o.width = static_cast<int>(intParam(p, "width", 800));
                o.height = static_cast<int>(intParam(p, "height", 500));
                o.dark = boolParam(p, "dark");
                State scratch = s;
                if (scratch.settings.angle == Angle::Degrees) scratch.settings.angle = Angle::Radians;
                auto exprs = splitExpressions(stringParam(p, "expressions"));
                if (exprs.empty()) return Result::failure("give something to draw, like sin(x)");
                auto curves = sampleCurves(scratch, exprs, o, std::max(400, o.width * 2));
                std::string png = graphPng(curves, o);
                std::string out = stringParam(p, "output");
                Result r = Result::success(out.empty() ? "drew " + std::to_string(curves.size()) + " curve(s)" : "saved the graph to " + out);
                if (out.empty()) r.attach({"image/png", png, "graph.png"});
                else writeFile(out, png);
                return r.with("curves", static_cast<std::int64_t>(curves.size())).with("bytes", static_cast<std::int64_t>(png.size()));
            }));

    std::vector<ParamSpec> sp = graphParams();
    sp.push_back(opt("samples", ParamType::Int, "points per curve", std::int64_t{400}));
    reg.add({"calc.sample", "Sample a graph",
             "The points of a graph as numbers (xs, ys, one ys list per curve joined in order) for drawing elsewhere.",
             sp, false, true},
            reader(st, [options](State& s, const Params& p) {
                GraphOptions o = options(p);
                State scratch = s;
                if (scratch.settings.angle == Angle::Degrees) scratch.settings.angle = Angle::Radians;
                auto exprs = splitExpressions(stringParam(p, "expressions"));
                if (exprs.empty()) return Result::failure("give something to draw, like sin(x)");
                auto curves = sampleCurves(scratch, exprs, o, static_cast<int>(intParam(p, "samples", 400)));
                NumberList ys;
                for (const auto& c : curves) ys.insert(ys.end(), c.ys.begin(), c.ys.end());
                return Result::success("sampled " + std::to_string(curves.size()) + " curve(s)")
                    .with("curves", static_cast<std::int64_t>(curves.size()))
                    .with("xs", NumberList(curves[0].xs))
                    .with("ys", ys);
            }));

    reg.add({"calc.functions", "Functions and constants", "Every built-in function and constant, for writing expressions.", {}, false, true},
            reader(st, [](State&, const Params&) {
                std::string fns;
                for (const auto& f : detail::functionNames()) fns += (fns.empty() ? "" : ", ") + f;
                std::string text =
                    "functions: " + fns +
                    "\nconstants: pi, e, tau, phi, inf, c (speed of light), G, g0 (gravity), planck, hbar, NA, kB, R, qe, me, mp, eps0, mu0"
                    "\noperators: + - * / // (floor divide) % or mod ^ ** ! (factorial) % (percent) & | xor ~ << >> == != < <= > >="
                    "\nnumbers: 1_000_000, 1.5e3, 0xff, 0b1010, 0o17; lists [1, 2, 3]; units 5 km, 3 ft + 2 in"
                    "\nanswers: ans (last), mem (memory); 'to': to hex, to bin, to oct, to frac, to sci, to <unit>";
                return Result::success(text).with("functions", fns);
            }));
}

}  // namespace

void registerCommands(Session& session) {
    auto st = std::make_shared<State>();
    auto& reg = session.registry();
    registerEval(reg, st);
    registerMemory(reg, st);
    registerUnits(reg, st);
    registerStats(reg, st);
    registerSolve(reg, st);
    registerGraph(reg, st);
}

}  // namespace wetwaresuite::calc
