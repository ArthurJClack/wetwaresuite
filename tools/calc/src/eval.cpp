// Evaluates expression trees: exact arithmetic where it can, units carried
// through every operation, lists element by element.
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "internal.hpp"

namespace wetwaresuite::calc::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

const char* dimName(const Dims& d) {
    struct Known {
        Dims dims;
        const char* name;
    };
    auto mk = [](std::initializer_list<int> v) {
        Dims d;
        int i = 0;
        for (int x : v) d.e[i++] = static_cast<std::int8_t>(x);
        return d;
    };
    static const Known known[] = {
        {mk({1}), "a length"},          {mk({0, 1}), "a mass"},          {mk({0, 0, 1}), "a time"},
        {mk({2}), "an area"},           {mk({3}), "a volume"},           {mk({1, 0, -1}), "a speed"},
        {mk({0, 0, 0, 0, 1}), "a temperature"}, {mk({0, 0, -1}), "a frequency"}, {mk({2, 1, -2}), "an energy"},
        {mk({2, 1, -3}), "a power"},    {mk({1, 1, -2}), "a force"},     {mk({-1, 1, -2}), "a pressure"},
        {mk({0, 0, 0, 0, 0, 0, 0, 1}), "an amount of data"},
    };
    for (const auto& k : known) {
        if (k.dims == d) return k.name;
    }
    return d.none() ? "a plain number" : "that unit";
}

Quantity unitQuantity(const UnitInfo& u) {
    Quantity q;
    q.value = numberFromDouble(u.scale);
    q.dims = u.dims;
    q.unit = u.name;
    q.scale = u.scale;
    q.offset = u.offset;
    q.angle = u.angle;
    return q;
}

bool hasUnit(const Quantity& q) { return !q.unit.empty() || !q.dims.none(); }

Dims addDims(const Dims& a, const Dims& b, int sign) {
    Dims d;
    for (int i = 0; i < kDims; ++i) d.e[i] = static_cast<std::int8_t>(a.e[i] + sign * b.e[i]);
    return d;
}

std::string wrap(const std::string& unit) {
    return unit.find_first_of("*/") == std::string::npos ? unit : "(" + unit + ")";
}

Quantity addQ(const Quantity& a, const Quantity& b, bool subtract) {
    if (a.dims != b.dims) {
        throw std::runtime_error(std::string("can't ") + (subtract ? "subtract " : "add ") + dimName(b.dims) +
                                 (subtract ? " from " : " to ") + dimName(a.dims));
    }
    Quantity r = a.unit.empty() && !b.unit.empty() ? b : a;
    // temperatures: adding to 20 °C keeps the offset once
    Number bv = b.value, av = a.value;
    r.value = subtract ? sub(av, bv) : add(av, bv);
    if (a.offset != 0 || b.offset != 0) {
        r.offset = a.offset != 0 ? a.offset : b.offset;
        r.scale = a.offset != 0 ? a.scale : b.scale;
        r.unit = a.offset != 0 ? a.unit : b.unit;
        // (x + o)s + (y + o)s = (x + y + o)s + o*s: take the extra offset back out
        if (a.offset != 0 && b.offset != 0) {
            double extra = a.offset * a.scale;
            r.value = subtract ? add(r.value, numberFromDouble(extra)) : sub(r.value, numberFromDouble(extra));
        }
    }
    r.angle = a.angle || b.angle;
    return r;
}

Quantity mulQ(const Quantity& a, const Quantity& b, bool divide) {
    Quantity r;
    // 20 degC: a plain number times an offset unit is a reading on that scale
    if (!divide && !hasUnit(a) && b.offset != 0) {
        r = b;
        r.value = numberFromDouble((a.value.toDouble() + b.offset) * b.scale);
        return r;
    }
    if (!divide && !hasUnit(b) && a.offset != 0) {
        r = a;
        r.value = numberFromDouble((b.value.toDouble() + a.offset) * a.scale);
        return r;
    }
    r.value = divide ? div(a.value, b.value) : mul(a.value, b.value);
    r.dims = addDims(a.dims, b.dims, divide ? -1 : 1);
    if (!a.unit.empty() && !b.unit.empty()) {
        r.unit = wrap(a.unit) + (divide ? "/" : "*") + wrap(b.unit);
        r.scale = divide ? a.scale / b.scale : a.scale * b.scale;
    } else if (!a.unit.empty()) {
        r.unit = a.unit;
        r.scale = a.scale;
    } else if (!b.unit.empty()) {
        r.unit = divide ? "1/" + wrap(b.unit) : b.unit;
        r.scale = divide ? 1 / b.scale : b.scale;
    }
    r.angle = (a.angle && !b.angle) || (b.angle && !a.angle && !divide);
    if (r.dims.none() && !r.angle) {  // km/m: just a number
        r.unit.clear();
        r.scale = 1;
    }
    if (!r.dims.none() && r.unit.empty() && (hasUnit(a) || hasUnit(b))) r.scale = 1;
    return r;
}

Number powNumber(const Number& base, const Number& e) {
    if (base.exact && e.isInteger() && e.num.fitsInt64()) {
        std::int64_t n = e.num.toInt64();
        std::uint64_t mag = n < 0 ? static_cast<std::uint64_t>(-(n + 1)) + 1 : static_cast<std::uint64_t>(n);
        const double bitsNeeded = static_cast<double>(std::max(base.num.bits(), base.den.bits())) * static_cast<double>(mag);
        if (bitsNeeded < 100000) {
            if (n < 0 && base.num.isZero()) throw std::runtime_error("can't divide by zero");
            BigInt p = BigInt::pow(base.num, mag), q = BigInt::pow(base.den, mag);
            return n >= 0 ? Number::fraction(p, q) : Number::fraction(q, p);
        }
    }
    // exact roots of exact numbers: 8^(1/3) = 2, (9/4)^(1/2) = 3/2
    if (base.exact && e.exact && !base.isNegative() && e.den.fitsInt64() && e.num.fitsInt64() && e.den.toInt64() <= 64) {
        const std::int64_t k = e.den.toInt64();
        auto iroot = [k](const BigInt& v, BigInt& out) {
            double guess = std::round(std::pow(v.toDouble(), 1.0 / static_cast<double>(k)));
            if (!std::isfinite(guess) || guess > 9e15) return false;
            for (double g = std::max(0.0, guess - 1); g <= guess + 1; ++g) {
                BigInt c(static_cast<std::int64_t>(g));
                if (BigInt::pow(c, static_cast<std::uint64_t>(k)) == v) {
                    out = c;
                    return true;
                }
            }
            return false;
        };
        BigInt rn, rd;
        if (iroot(base.num, rn) && iroot(base.den, rd)) {
            Number root = Number::fraction(rn, rd);
            Number whole;
            whole.num = e.num;
            return powNumber(root, whole);
        }
    }
    double r = std::pow(base.toDouble(), e.toDouble());
    if (std::isnan(r) && base.isNegative()) throw std::runtime_error("a negative number to a fractional power has no real answer");
    return Number::real(r);
}

Quantity powQ(const Quantity& a, const Quantity& b) {
    if (hasUnit(b) && !b.angle) throw std::runtime_error("a power can't have a unit");
    Quantity r = a;
    r.value = powNumber(a.value, b.value);
    if (!a.dims.none() || !a.unit.empty()) {
        if (!b.value.exact) throw std::runtime_error("a unit can only be raised to a whole or simple fractional power");
        for (int i = 0; i < kDims; ++i) {
            BigInt scaled = BigInt(a.dims.e[i]) * b.value.num;
            BigInt q, rem;
            BigInt::divmod(scaled, b.value.den, q, rem);
            if (!rem.isZero()) throw std::runtime_error("that power doesn't give a whole unit");
            r.dims.e[i] = static_cast<std::int8_t>(q.toInt64());
        }
        if (!a.unit.empty()) {
            std::string e = b.value.isInteger() ? b.value.num.toString() : "(" + b.value.num.toString() + "/" + b.value.den.toString() + ")";
            r.unit = wrap(a.unit) + "^" + e;
            r.scale = std::pow(a.scale, b.value.toDouble());
        }
        r.offset = 0;
    }
    return r;
}

BigInt needInteger(const Quantity& q, const char* what) {
    if (hasUnit(q) || !q.value.isInteger()) {
        double v = q.value.toDouble();
        if (!hasUnit(q) && std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9e15) return BigInt(static_cast<std::int64_t>(v));
        throw std::runtime_error(std::string(what) + " works on whole numbers");
    }
    return q.value.num;
}

Quantity bitOp(const std::string& op, const Quantity& a, const Quantity& b) {
    BigInt x = needInteger(a, op == "&" ? "& (and)" : op == "|" ? "| (or)" : op == "xor" ? "xor" : "shifting");
    BigInt y = needInteger(b, op == "&" ? "& (and)" : op == "|" ? "| (or)" : op == "xor" ? "xor" : "shifting");
    BigInt r;
    if (op == "&") r = BigInt::bitAnd(x, y);
    else if (op == "|") r = BigInt::bitOr(x, y);
    else if (op == "xor") r = BigInt::bitXor(x, y);
    else {
        if (!y.fitsInt64() || y.toInt64() < 0 || y.toInt64() > 100000) throw std::runtime_error("shift by 0 to 100000 bits");
        auto n = static_cast<std::size_t>(y.toInt64());
        r = op == "<<" ? x.shiftLeft(n) : x.shiftRight(n);
    }
    return plain(Number::fraction(r, BigInt(1)));
}

Quantity modQ(const Quantity& a, const Quantity& b, bool floorDiv) {
    if (a.dims != b.dims) throw std::runtime_error("% needs both sides in the same kind of unit");
    if (b.value.isZero()) throw std::runtime_error("can't divide by zero");
    Quantity r = a;
    if (a.value.exact && b.value.exact) {
        // floored modulo (the sign of the divisor), like Python: -7 % 3 = 2
        Number q = div(a.value, b.value);
        BigInt fq, rem;
        BigInt::divmod(q.num, q.den, fq, rem);
        if (rem.negative()) fq = fq - BigInt(1);
        Number whole = Number::fraction(fq, BigInt(1));
        if (floorDiv) return plain(whole);
        r.value = sub(a.value, mul(whole, b.value));
        return r;
    }
    double x = a.value.toDouble(), y = b.value.toDouble();
    double q = std::floor(x / y);
    if (floorDiv) return plain(numberFromDouble(q));
    r.value = Number::real(x - q * y);
    return r;
}

Quantity compareQ(const std::string& op, const Quantity& a, const Quantity& b) {
    if (a.dims != b.dims) throw std::runtime_error("can't compare " + std::string(dimName(a.dims)) + " with " + dimName(b.dims));
    int c = compare(a.value, b.value);
    bool r = op == "==" ? c == 0 : op == "!=" ? c != 0 : op == "<" ? c < 0 : op == "<=" ? c <= 0 : op == ">" ? c > 0 : c >= 0;
    return plain(Number(r ? 1 : 0));
}

Quantity binaryQ(const std::string& op, const Quantity& a, const Quantity& b) {
    if (op == "+") return addQ(a, b, false);
    if (op == "-") return addQ(a, b, true);
    if (op == "+%" || op == "-%") {
        // a + b%: b percent of a
        Quantity part = mulQ(a, b, false);
        return addQ(a, part, op == "-%");
    }
    if (op == "*") return mulQ(a, b, false);
    if (op == "/") return mulQ(a, b, true);
    if (op == "%") return modQ(a, b, false);
    if (op == "//") return modQ(a, b, true);
    if (op == "^") return powQ(a, b);
    if (op == "&" || op == "|" || op == "xor" || op == "<<" || op == ">>") return bitOp(op, a, b);
    return compareQ(op, a, b);
}

Val binary(const std::string& op, const Val& a, const Val& b) {
    Val r;
    if (!a.isList && !b.isList) {
        r.q = binaryQ(op, a.q, b.q);
        return r;
    }
    r.isList = true;
    if (a.isList && b.isList) {
        if (a.items.size() != b.items.size()) throw std::runtime_error("the two lists have different lengths");
        for (std::size_t i = 0; i < a.items.size(); ++i) r.items.push_back(binaryQ(op, a.items[i], b.items[i]));
    } else if (a.isList) {
        for (const auto& x : a.items) r.items.push_back(binaryQ(op, x, b.q));
    } else {
        for (const auto& x : b.items) r.items.push_back(binaryQ(op, a.q, x));
    }
    return r;
}

Quantity factorialQ(const Quantity& a) {
    BigInt n = needInteger(a, "! (factorial)");
    if (n.negative()) throw std::runtime_error("factorial needs a number that isn't negative");
    if (!n.fitsInt64() || n.toInt64() > 20000) return plain(Number::real(INFINITY));
    BigInt r(1);
    for (std::int64_t i = 2; i <= n.toInt64(); ++i) r = r * BigInt(i);
    return plain(Number::fraction(r, BigInt(1)));
}

Val lookupName(const Node& n, const Env& env) {
    Val v;
    const std::string& name = n.text;
    if (!n.unitOnly) {
        if (env.locals) {
            auto it = env.locals->find(name);
            if (it != env.locals->end()) {
                v.q = it->second;
                return v;
            }
        }
        auto it = env.state->vars.find(name);
        if (it != env.state->vars.end()) {
            v.q = it->second;
            return v;
        }
        auto lt = env.state->lists.find(name);
        if (lt != env.state->lists.end()) {
            v.isList = true;
            v.items = lt->second;
            return v;
        }
        if (isConstant(name)) {
            std::vector<Val> none;
            return callFunction(name, none, env);
        }
    }
    UnitInfo u;
    if (findUnit(name == "in" ? "inch" : name, u)) {
        v.q = unitQuantity(u);
        return v;
    }
    if (isBuiltinFunction(name)) throw std::runtime_error(name + " is a function: write " + name + "(...)");
    if (n.unitOnly) throw std::runtime_error("I don't know the unit '" + name + "' (calc.units lists them)");
    throw std::runtime_error("I don't know '" + name + "' (give it a value first, like " + name + " = 5)");
}

Val callUser(const UserFunction& f, const std::string& name, std::vector<Val>& args, const Env& env) {
    if (args.size() != f.params.size())
        throw std::runtime_error(name + " takes " + std::to_string(f.params.size()) + " value" + (f.params.size() == 1 ? "" : "s"));
    if (env.depth > 200) throw std::runtime_error(name + " calls itself too deeply");
    std::map<std::string, Quantity> locals;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i].isList) throw std::runtime_error("pass single values to " + name);
        locals[f.params[i]] = args[i].q;
    }
    Env inner = env;
    inner.locals = &locals;
    inner.depth = env.depth + 1;
    NodePtr body = parseExpression(f.body);
    return eval(*body, inner);
}

}  // namespace

Quantity plain(Number n) {
    Quantity q;
    q.value = std::move(n);
    return q;
}

Quantity plainDouble(double v) { return plain(Number::real(v)); }

Number numberFromDouble(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9007199254740992.0) return Number(static_cast<std::int64_t>(v));
    return Number::real(v);
}

double toAngle(const Quantity& q, const Settings& s) {
    double v = q.value.toDouble();
    if (q.angle) return v;  // already radians
    if (s.angle == Angle::Degrees) return v * kPi / 180;
    if (s.angle == Angle::Gradians) return v * kPi / 200;
    return v;
}

double fromRadians(double r, const Settings& s) {
    if (s.angle == Angle::Degrees) return r * 180 / kPi;
    if (s.angle == Angle::Gradians) return r * 200 / kPi;
    return r;
}

std::string siUnitText(const Dims& d) {
    static const char* names[kDims] = {"m", "kg", "s", "A", "K", "mol", "cd", "bit"};
    std::string num, den;
    for (int i = 0; i < kDims; ++i) {
        int e = d.e[i];
        if (!e) continue;
        std::string part = names[i];
        if (std::abs(e) != 1) part += "^" + std::to_string(std::abs(e));
        std::string& side = e > 0 ? num : den;
        side += (side.empty() ? "" : "*") + part;
    }
    if (num.empty() && den.empty()) return "";
    if (den.empty()) return num;
    return (num.empty() ? "1" : num) + "/" + (den.find('*') != std::string::npos ? "(" + den + ")" : den);
}

Val eval(const Node& n, const Env& env) {
    switch (n.kind) {
        case Node::Num: {
            Val v;
            v.q = plain(n.number);
            if (!env.state->settings.exact && v.q.value.exact && !v.q.value.isInteger()) v.q.value = Number::real(v.q.value.toDouble());
            return v;
        }
        case Node::Name: return lookupName(n, env);
        case Node::List: {
            Val v;
            v.isList = true;
            for (const auto& k : n.kids) {
                Val item = eval(*k, env);
                if (item.isList) v.items.insert(v.items.end(), item.items.begin(), item.items.end());
                else v.items.push_back(item.q);
            }
            return v;
        }
        case Node::Unary: {
            Val v = eval(*n.kids[0], env);
            auto apply = [&](Quantity& q) {
                if (n.text == "-") q.value = q.offset != 0 ? numberFromDouble(-(q.value.toDouble() / q.scale - q.offset) * q.scale + q.offset * q.scale) : neg(q.value);
                else if (n.text == "~") q = plain(Number::fraction(needInteger(q, "~ (not)").bitNot(), BigInt(1)));
            };
            if (v.isList) {
                for (auto& q : v.items) apply(q);
            } else {
                apply(v.q);
            }
            return v;
        }
        case Node::Binary: {
            Val a = eval(*n.kids[0], env);
            Val b = eval(*n.kids[1], env);
            if ((n.text == "+%" || n.text == "-%") && !b.isList) {
                // the right side was "x%": it is already x/100
                return binary(n.text, a, b);
            }
            return binary(n.text, a, b);
        }
        case Node::Postfix: {
            Val v = eval(*n.kids[0], env);
            auto apply = [&](Quantity& q) {
                if (n.text == "!") q = factorialQ(q);
                else q.value = div(q.value, Number(100));
            };
            if (v.isList) {
                for (auto& q : v.items) apply(q);
            } else {
                apply(v.q);
            }
            return v;
        }
        case Node::Call: {
            std::vector<Val> args;
            for (const auto& k : n.kids) args.push_back(eval(*k, env));
            auto uf = env.state->functions.find(n.text);
            if (uf != env.state->functions.end()) return callUser(uf->second, n.text, args, env);
            if (!isBuiltinFunction(n.text)) {
                UnitInfo u;
                if (args.size() == 1 && findUnit(n.text, u)) return binary("*", args[0], Val{unitQuantity(u), false, {}});
                throw std::runtime_error("there's no function called " + n.text + " (calc.functions lists them, or define one: " +
                                         n.text + "(x) = ...)");
            }
            return callFunction(n.text, args, env);
        }
        case Node::Convert: {
            Val v = eval(*n.kids[0], env);
            if (n.kids.size() < 2) return v;  // a format word: applied when shown
            Val target = eval(*n.kids[1], env);
            if (target.isList) throw std::runtime_error("convert to a unit, not a list");
            auto convert = [&](Quantity& q) {
                const Quantity& t = target.q;
                if (q.dims != t.dims) {
                    throw std::runtime_error(std::string("can't convert ") + dimName(q.dims) + " to " + dimName(t.dims) +
                                             " (" + n.text + ")");
                }
                q.unit = n.text;
                q.scale = t.offset != 0 ? t.scale : t.value.toDouble();
                q.offset = t.offset;
                q.angle = t.angle || q.angle;
            };
            if (v.isList) {
                for (auto& q : v.items) convert(q);
            } else {
                convert(v.q);
            }
            return v;
        }
    }
    throw std::runtime_error("can't evaluate that");
}

}  // namespace wetwaresuite::calc::detail
