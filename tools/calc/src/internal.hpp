// Shared pieces of the calculator: the parsed expression tree and its evaluator.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/calc/calc.hpp"

namespace wetwaresuite::calc::detail {

struct Node;
using NodePtr = std::shared_ptr<Node>;

struct Node {
    enum Kind { Num, Name, Unary, Binary, Call, Postfix, List, Convert } kind = Num;
    std::string text;  // name, operator, or the "to" target text
    Number number;
    bool unitOnly = false;  // a name on the right of "to": a unit, never a variable
    std::vector<NodePtr> kids;
};

// A value while evaluating: one quantity or a list of them.
struct Val {
    Quantity q;
    bool isList = false;
    std::vector<Quantity> items;
};

struct Env {
    const State* state = nullptr;
    const std::map<std::string, Quantity>* locals = nullptr;
    int depth = 0;
};

// Parses one expression (no assignment). Throws std::runtime_error.
NodePtr parseExpression(const std::string& text);
Val eval(const Node& n, const Env& env);

Quantity plain(Number n);
Quantity plainDouble(double v);
Number numberFromDouble(double v);  // exact when it is a whole number that fits
double toAngle(const Quantity& q, const Settings& s);  // radians, from the angle mode or its unit
double fromRadians(double r, const Settings& s);
bool isBuiltinFunction(const std::string& name);
bool isConstant(const std::string& name);
Val callFunction(const std::string& name, std::vector<Val>& args, const Env& env);
std::vector<std::string> functionNames();
std::string siUnitText(const Dims& d);

// "to hex" style directives
bool isFormatWord(const std::string& w);

}  // namespace wetwaresuite::calc::detail
