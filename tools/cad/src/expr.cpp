// A small, safe expression evaluator for CAD fields: no assignment, no loops,
// nothing but arithmetic over numbers and named parameters.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>

#include "wetwaresuite/cad/cad.hpp"

namespace wetwaresuite::cad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

const std::set<std::string>& functionNames() {
    static const std::set<std::string> names = {"sin",  "cos", "tan",   "asin",  "acos", "atan", "atan2", "sqrt",
                                                "abs",  "min", "max",   "floor", "ceil", "round", "pow"};
    return names;
}

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Parser {
public:
    Parser(const std::string& text, const Resolver* lookup) : s_(text), lookup_(lookup) {}

    double parse() {
        double v = expr();
        skip();
        if (i_ < s_.size()) fail(std::string("unexpected '") + s_[i_] + "'");
        return v;
    }

    std::vector<std::string> names;  // filled when collecting

private:
    const std::string& s_;
    const Resolver* lookup_;
    std::size_t i_ = 0;
    int depth_ = 0;

    [[noreturn]] void fail(const std::string& why) const {
        throw ExprError("bad expression '" + s_ + "': " + why);
    }
    void skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool eat(char c) {
        skip();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    double expr() {
        if (++depth_ > 200) fail("nested too deeply");
        double v = term();
        for (;;) {
            if (eat('+')) v += term();
            else if (eat('-')) v -= term();
            else break;
        }
        --depth_;
        return v;
    }
    double term() {
        double v = unary();
        for (;;) {
            if (eat('*')) {
                v *= unary();
            } else if (eat('/')) {
                double d = unary();
                if (d == 0 && !collecting()) fail("division by zero");
                v = d == 0 ? 0 : v / d;
            } else if (eat('%')) {
                double d = unary();
                if (d == 0 && !collecting()) fail("modulo by zero");
                v = d == 0 ? 0 : std::fmod(v, d);
            } else {
                break;
            }
        }
        return v;
    }
    double unary() {
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return power();
    }
    double power() {
        double base = primary();
        if (eat('^')) return std::pow(base, unary());
        return base;
    }
    bool collecting() const { return lookup_ == nullptr; }

    double primary() {
        skip();
        if (i_ >= s_.size()) fail("ends too early");
        char c = s_[i_];
        if (c == '(') {
            ++i_;
            double v = expr();
            if (!eat(')')) fail("missing ')'");
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return number();
        if (identStart(c)) {
            std::size_t start = i_;
            while (i_ < s_.size() && identChar(s_[i_])) ++i_;
            std::string name = s_.substr(start, i_ - start);
            skip();
            if (i_ < s_.size() && s_[i_] == '(') {
                ++i_;
                return call(name);
            }
            return variable(name);
        }
        fail(std::string("unexpected '") + c + "'");
    }

    double number() {
        std::size_t start = i_;
        while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            std::size_t j = i_ + 1;
            if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) ++j;
            if (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) {
                i_ = j;
                while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) ++i_;
            }
        }
        std::string text = s_.substr(start, i_ - start);
        if (text == ".") fail("'.' is not a number");
        return std::strtod(text.c_str(), nullptr);
    }

    double variable(const std::string& name) {
        if (functionNames().count(name)) fail(name + " is a function; call it like " + name + "(x)");
        if (collecting()) {
            if (name != "pi" && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            return 1;
        }
        if (*lookup_) {
            if (auto v = (*lookup_)(name)) return *v;
        }
        if (name == "pi") return kPi;
        fail("unknown name '" + name + "' (define it with cad.param_set)");
    }

    double call(const std::string& name) {
        std::vector<double> args;
        skip();
        if (!eat(')')) {
            do {
                args.push_back(expr());
            } while (eat(','));
            if (!eat(')')) fail("missing ')' after the arguments of " + name);
        }
        if (!functionNames().count(name)) fail("unknown function '" + name + "'");
        auto need = [&](std::size_t n) {
            if (args.size() != n) fail(name + " takes " + std::to_string(n) + (n == 1 ? " argument" : " arguments"));
        };
        if (name == "min" || name == "max") {
            if (args.empty()) fail(name + " needs at least one argument");
            return name == "min" ? *std::min_element(args.begin(), args.end()) : *std::max_element(args.begin(), args.end());
        }
        if (name == "atan2" || name == "pow") {
            need(2);
            return name == "pow" ? std::pow(args[0], args[1]) : std::atan2(args[0], args[1]) / kDeg;
        }
        need(1);
        const double x = args[0];
        if (name == "sin") return std::sin(x * kDeg);
        if (name == "cos") return std::cos(x * kDeg);
        if (name == "tan") return std::tan(x * kDeg);
        if (name == "asin") return std::asin(x) / kDeg;
        if (name == "acos") return std::acos(x) / kDeg;
        if (name == "atan") return std::atan(x) / kDeg;
        if (name == "sqrt") {
            if (x < 0 && !collecting()) fail("sqrt of a negative number");
            return std::sqrt(std::fabs(x));
        }
        if (name == "abs") return std::fabs(x);
        if (name == "floor") return std::floor(x);
        if (name == "ceil") return std::ceil(x);
        return std::round(x);
    }
};

}  // namespace

double evaluate(const std::string& expr, const Resolver& lookup) {
    Parser parser(expr, &lookup);
    double v = parser.parse();
    if (!std::isfinite(v)) throw ExprError("expression '" + expr + "' is not a finite number");
    return v;
}

std::vector<std::string> identifiers(const std::string& expr) {
    Parser parser(expr, nullptr);
    try {
        parser.parse();
    } catch (const ExprError&) {
        // a broken expression still reports the names found before the error
    }
    return parser.names;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    int depth = 0;
    auto flush = [&] {
        std::size_t a = 0, b = item.size();
        while (a < b && std::isspace(static_cast<unsigned char>(item[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(item[b - 1]))) --b;
        if (b > a) out.push_back(item.substr(a, b - a));
        item.clear();
    };
    for (char c : text) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (depth <= 0 && (c == ',' || c == ';')) flush();
        else item += c;
    }
    flush();
    return out;
}

bool validParamName(const std::string& name) {
    if (name.empty() || !identStart(name[0]) || name.size() > 64) return false;
    for (char c : name) {
        if (!identChar(c)) return false;
    }
    return name != "pi" && !functionNames().count(name);
}

}  // namespace wetwaresuite::cad
