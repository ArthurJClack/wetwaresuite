// Turns calculator text into an expression tree. Precedence, lowest first:
//   to/in conversion, comparisons, | and xor, &, << >>, + -, * / % mod and
//   implicit multiplication (2x, 3 km), unary - + ~, ^ and ** (right to left),
//   postfix ! and %, then numbers, names, calls, (groups) and [lists].
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

#include "internal.hpp"

namespace wetwaresuite::calc::detail {

namespace {

struct Tok {
    enum Kind { Num, Ident, Op, End } kind = End;
    std::string text;
    std::size_t pos = 0, end = 0;
    Number number;
};

bool identStart(unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; }
bool identChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// multi-byte characters that are operators or names of their own
struct Special {
    const char* utf8;
    const char* as;
    bool op;
};
const Special kSpecials[] = {
    {"\xC3\x97", "*", true},          // ×
    {"\xC3\xB7", "/", true},          // ÷
    {"\xE2\x88\x92", "-", true},      // −
    {"\xC2\xB7", "*", true},          // ·
    {"\xE2\x8B\x85", "*", true},      // ⋅
    {"\xE2\x89\xA0", "!=", true},     // ≠
    {"\xE2\x89\xA4", "<=", true},     // ≤
    {"\xE2\x89\xA5", ">=", true},     // ≥
    {"\xE2\x88\x9A", "sqrt", false},  // √
    {"\xCF\x80", "pi", false},        // π
    {"\xCF\x84", "tau", false},       // τ
    {"\xCF\x86", "phi", false},       // φ
    {"\xE2\x88\x9E", "inf", false},   // ∞
};

double parseDecimal(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

// "12.5e3" exactly: digits and a power of ten
Number exactDecimal(const std::string& text) {
    std::string digits;
    long long exp = 0;
    bool afterDot = false;
    std::size_t i = 0;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        if (c == '.') {
            afterDot = true;
            continue;
        }
        if (c == 'e' || c == 'E') break;
        digits += c;
        if (afterDot) --exp;
    }
    if (i < text.size()) exp += std::strtoll(text.c_str() + i + 1, nullptr, 10);
    if (digits.empty()) digits = "0";
    if (exp > 400 || exp < -400) return Number::real(parseDecimal(text));
    BigInt n = BigInt::parse(digits);
    if (exp >= 0) return Number::fraction(n * BigInt::pow(BigInt(10), static_cast<std::uint64_t>(exp)), BigInt(1));
    return Number::fraction(n, BigInt::pow(BigInt(10), static_cast<std::uint64_t>(-exp)));
}

std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        Tok t;
        t.pos = i;
        bool special = false;
        for (const auto& sp : kSpecials) {
            std::string u = sp.utf8;
            if (s.compare(i, u.size(), u) == 0) {
                t.kind = sp.op ? Tok::Op : Tok::Ident;
                t.text = sp.as;
                i += u.size();
                special = true;
                break;
            }
        }
        if (special) {
            t.end = i;
            out.push_back(t);
            continue;
        }
        if (std::isdigit(c) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            t.kind = Tok::Num;
            int base = 10;
            if (c == '0' && i + 1 < s.size()) {
                char p = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i + 1])));
                base = p == 'x' ? 16 : p == 'b' ? 2 : p == 'o' ? 8 : 10;
            }
            if (base != 10) {
                std::size_t j = i + 2;
                while (j < s.size() && (std::isxdigit(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
                std::string digits = s.substr(i + 2, j - i - 2);
                if (digits.empty()) throw std::runtime_error("a number like " + s.substr(i, 2) + " needs digits after it");
                t.number = Number::fraction(BigInt::parse(digits, base), BigInt(1));
                t.text = s.substr(i, j - i);
                i = j;
            } else {
                std::size_t j = i;
                while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '_' || s[j] == '.')) ++j;
                if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
                    std::size_t k = j + 1;
                    if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
                    if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
                        while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
                        j = k;
                    }
                }
                t.text = s.substr(i, j - i);
                if (std::count(t.text.begin(), t.text.end(), '.') > 1) throw std::runtime_error("the number " + t.text + " has two decimal points");
                t.number = exactDecimal(t.text);
                i = j;
            }
        } else if (identStart(c)) {
            t.kind = Tok::Ident;
            std::size_t j = i;
            while (j < s.size() && identChar(static_cast<unsigned char>(s[j]))) {
                // stop before a special character glued to a name: 2π, x×y
                bool stop = false;
                for (const auto& sp : kSpecials) {
                    std::string u = sp.utf8;
                    if (j > i && s.compare(j, u.size(), u) == 0) stop = true;
                }
                if (stop) break;
                ++j;
            }
            t.text = s.substr(i, j - i);
            i = j;
        } else {
            t.kind = Tok::Op;
            static const char* two[] = {"**", "<<", ">>", "==", "!=", "<=", ">=", "->", "//"};
            for (const char* op : two) {
                if (s.compare(i, 2, op) == 0) t.text = op;
            }
            if (t.text.empty()) {
                if (std::string("+-*/%^!&|~<>=()[],;").find(static_cast<char>(c)) == std::string::npos)
                    throw std::runtime_error(std::string("I don't know what '") + static_cast<char>(c) + "' means here");
                t.text = std::string(1, static_cast<char>(c));
            }
            i += t.text.size();
        }
        t.end = i;
        out.push_back(t);
    }
    Tok end;
    end.pos = end.end = s.size();
    out.push_back(end);
    return out;
}

NodePtr make(Node::Kind k, std::string text, std::vector<NodePtr> kids = {}) {
    auto n = std::make_shared<Node>();
    n->kind = k;
    n->text = std::move(text);
    n->kids = std::move(kids);
    return n;
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k = {"to", "in", "as", "mod", "xor", "and", "or"};
    return k;
}

class Parser {
public:
    Parser(const std::string& src) : src_(src), toks_(lex(src)) {}

    NodePtr all() {
        NodePtr n = conversion();
        if (peek().kind != Tok::End) {
            const Tok& t = peek();
            throw std::runtime_error("I didn't expect '" + t.text + "' there" + (t.text == ")" ? " (one ')' too many?)" : ""));
        }
        return n;
    }

private:
    const std::string& src_;
    std::vector<Tok> toks_;
    std::size_t at_ = 0;

    const Tok& peek(std::size_t ahead = 0) const { return toks_[std::min(at_ + ahead, toks_.size() - 1)]; }
    const Tok& prev() const { return toks_[at_ ? at_ - 1 : 0]; }
    bool isOp(const char* op, std::size_t ahead = 0) const { return peek(ahead).kind == Tok::Op && peek(ahead).text == op; }
    bool isWord(const char* w) const { return peek().kind == Tok::Ident && peek().text == w; }
    Tok take() { return toks_[at_ < toks_.size() - 1 ? at_++ : at_]; }
    void expect(const char* op) {
        if (!isOp(op)) {
            if (peek().kind == Tok::End) throw std::runtime_error(std::string("missing '") + op + "' at the end");
            throw std::runtime_error(std::string("expected '") + op + "' but found '" + peek().text + "'");
        }
        take();
    }

    NodePtr conversion() {
        NodePtr left = comparison();
        while ((isWord("to") || isWord("as") || (isWord("in") && !afterNumber())) || isOp("->")) {
            take();
            if (peek().kind == Tok::Ident && isFormatWord(peek().text) &&
                (peek(1).kind == Tok::End || (peek(1).kind == Tok::Op && (peek(1).text == ";" || peek(1).text == ")")))) {
                left = make(Node::Convert, take().text, {left});
                continue;
            }
            std::size_t start = peek().pos;
            NodePtr unit = term(true);
            std::string text = src_.substr(start, prev().end - start);
            NodePtr conv = make(Node::Convert, text, {left, unit});
            left = conv;
        }
        return left;
    }

    bool afterNumber() const { return at_ > 0 && prev().kind == Tok::Num; }

    NodePtr comparison() {
        NodePtr left = bitOr();
        static const std::set<std::string> ops = {"==", "!=", "<", "<=", ">", ">="};
        while (peek().kind == Tok::Op && ops.count(peek().text)) {
            std::string op = take().text;
            left = make(Node::Binary, op, {left, bitOr()});
        }
        return left;
    }

    NodePtr bitOr() {
        NodePtr left = bitAnd();
        while (isOp("|") || isWord("xor") || isWord("or")) {
            std::string op = take().text;
            left = make(Node::Binary, op == "|" || op == "or" ? "|" : "xor", {left, bitAnd()});
        }
        return left;
    }

    NodePtr bitAnd() {
        NodePtr left = shift();
        while (isOp("&") || isWord("and")) {
            take();
            left = make(Node::Binary, "&", {left, shift()});
        }
        return left;
    }

    NodePtr shift() {
        NodePtr left = additive();
        while (isOp("<<") || isOp(">>")) {
            std::string op = take().text;
            left = make(Node::Binary, op, {left, additive()});
        }
        return left;
    }

    NodePtr additive() {
        NodePtr left = term(false);
        while (isOp("+") || isOp("-")) {
            std::string op = take().text;
            NodePtr right = term(false);
            // 200 + 10% adds ten percent of 200, like a pocket calculator
            if (right->kind == Node::Postfix && right->text == "%") op += "%";
            left = make(Node::Binary, op, {left, right});
        }
        return left;
    }

    bool startsOperand() const {
        const Tok& t = peek();
        if (t.kind == Tok::Num) return true;
        if (t.kind == Tok::Ident) return !keywords().count(t.text) || (t.text == "in" && afterNumber());
        return t.kind == Tok::Op && (t.text == "(" || t.text == "[");
    }

    NodePtr term(bool units) {
        NodePtr left = unary(units);
        for (;;) {
            if (isOp("*") || isOp("/") || isOp("%") || isOp("//") || isWord("mod")) {
                std::string op = take().text;
                if (op == "mod") op = "%";
                left = make(Node::Binary, op, {left, unary(units)});
            } else if (startsOperand() && peek().kind != Tok::Num) {
                // implicit multiplication: 2x, 3 km, 2(1+1), (a)(b); "2 in" is two inches
                if (isWord("in")) {
                    take();
                    NodePtr inch = make(Node::Name, "inch");
                    inch->unitOnly = true;
                    left = make(Node::Binary, "*", {left, inch});
                    continue;
                }
                left = make(Node::Binary, "*", {left, power(units)});
            } else {
                break;
            }
        }
        return left;
    }

    NodePtr unary(bool units) {
        if (isOp("-") || isOp("+") || isOp("~")) {
            std::string op = take().text;
            return make(Node::Unary, op, {unary(units)});
        }
        return power(units);
    }

    NodePtr power(bool units) {
        NodePtr base = postfix(units);
        if (isOp("^") || isOp("**")) {
            take();
            return make(Node::Binary, "^", {base, unary(units)});  // right to left: 2^3^2 = 2^9
        }
        return base;
    }

    NodePtr postfix(bool units) {
        NodePtr n = primary(units);
        for (;;) {
            if (isOp("!")) {
                take();
                n = make(Node::Postfix, "!", {n});
            } else if (isOp("%") && !(peek(1).kind == Tok::Num || peek(1).kind == Tok::Ident || (peek(1).kind == Tok::Op && (peek(1).text == "(" || peek(1).text == "[")))) {
                take();
                n = make(Node::Postfix, "%", {n});
            } else {
                break;
            }
        }
        return n;
    }

    NodePtr primary(bool units) {
        const Tok& t = peek();
        if (t.kind == Tok::Num) {
            NodePtr n = make(Node::Num, t.text);
            n->number = t.number;
            take();
            return n;
        }
        if (isOp("(")) {
            take();
            NodePtr inner = conversion();
            expect(")");
            return inner;
        }
        if (isOp("[")) {
            take();
            std::vector<NodePtr> items;
            if (!isOp("]")) {
                for (;;) {
                    items.push_back(conversion());
                    if (!isOp(",") && !isOp(";")) break;
                    take();
                }
            }
            expect("]");
            return make(Node::List, "", std::move(items));
        }
        if (t.kind == Tok::Ident) {
            std::string name = take().text;
            if (!units && isOp("(")) {
                take();
                std::vector<NodePtr> args;
                if (!isOp(")")) {
                    for (;;) {
                        args.push_back(conversion());
                        if (!isOp(",") && !isOp(";")) break;
                        take();
                    }
                }
                expect(")");
                return make(Node::Call, name, std::move(args));
            }
            // sin 30, sqrt 2: a built-in function without brackets takes the next operand
            if (!units && isBuiltinFunction(name) && startsOperand()) return make(Node::Call, name, {power(false)});
            NodePtr n = make(Node::Name, name);
            n->unitOnly = units;
            return n;
        }
        if (t.kind == Tok::End) throw std::runtime_error("the expression ends too soon");
        throw std::runtime_error("I didn't expect '" + t.text + "' there");
    }
};

}  // namespace

bool isFormatWord(const std::string& w) {
    static const std::set<std::string> words = {"hex", "bin", "oct", "dec", "binary", "octal", "decimal", "hexadecimal",
                                                "frac", "fraction", "sci", "scientific", "eng", "base"};
    return words.count(w) > 0;
}

NodePtr parseExpression(const std::string& text) {
    Parser p(text);
    return p.all();
}

}  // namespace wetwaresuite::calc::detail
