// A small numeric constraint solver shared by sketches and assemblies. Each
// equation is a residual over a few variables; the solver moves the free
// variables as little as possible to drive every residual to zero (damped
// minimum-norm Gauss-Newton, so an under-constrained sketch keeps its shape
// where nothing pins it), then reports the rank of the system: degrees of
// freedom left, redundant equations and which variables can still move.
#pragma once

#include <functional>
#include <vector>

namespace wetwaresuite::cad::detail {

struct Equation {
    std::vector<int> vars;  // the variables it reads (others are ignored for its derivatives)
    std::function<double(const std::vector<double>&)> f;
    int owner = -1;  // which constraint it came from, for reporting
};

struct SolveReport {
    bool converged = false;
    double residual = 0;  // largest |residual| at the end
    int iterations = 0;
    int variables = 0;    // free (unlocked) variables
    int rank = 0;
    int dof = 0;          // variables - rank
    int redundant = 0;    // equations - rank
    std::vector<bool> movable;   // per variable: can still move without breaking an equation
    std::vector<int> failing;    // owners of equations left unsatisfied
};

// Solves in place. `locked` variables never move (may be empty = none locked).
// `tolerance` is on each residual.
SolveReport solve(std::vector<double>& x, const std::vector<bool>& locked, const std::vector<Equation>& equations,
                  double tolerance = 1e-9, int maxIterations = 200);

}  // namespace wetwaresuite::cad::detail
