#include "solver.hpp"

#include <algorithm>
#include <cmath>

namespace wetwaresuite::cad::detail {

namespace {

// Solves the symmetric positive definite system a * out = b (n x n, row-major) by
// Gaussian elimination with partial pivoting. Returns false when singular.
bool solveDense(std::vector<double> a, std::vector<double> b, int n, std::vector<double>& out) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r) {
            if (std::fabs(a[r * n + c]) > std::fabs(a[piv * n + c])) piv = r;
        }
        if (std::fabs(a[piv * n + c]) < 1e-300) return false;
        if (piv != c) {
            for (int k = 0; k < n; ++k) std::swap(a[c * n + k], a[piv * n + k]);
            std::swap(b[c], b[piv]);
        }
        const double d = a[c * n + c];
        for (int r = c + 1; r < n; ++r) {
            const double f = a[r * n + c] / d;
            if (f == 0) continue;
            for (int k = c; k < n; ++k) a[r * n + k] -= f * a[c * n + k];
            b[r] -= f * b[c];
        }
    }
    out.assign(static_cast<std::size_t>(n), 0.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < n; ++k) s -= a[r * n + k] * out[static_cast<std::size_t>(k)];
        out[static_cast<std::size_t>(r)] = s / a[r * n + r];
    }
    return true;
}

struct System {
    const std::vector<Equation>& eqs;
    std::vector<int> col;  // variable -> column, -1 when locked
    int n = 0;

    void residuals(const std::vector<double>& x, std::vector<double>& r) const {
        r.resize(eqs.size());
        for (std::size_t k = 0; k < eqs.size(); ++k) {
            const double v = eqs[k].f(x);
            r[k] = std::isfinite(v) ? v : 1e6;
        }
    }
    // m x n, row-major, central differences over each equation's own variables
    void jacobian(std::vector<double>& x, std::vector<double>& j) const {
        const std::size_t m = eqs.size();
        j.assign(m * static_cast<std::size_t>(n), 0.0);
        for (std::size_t k = 0; k < m; ++k) {
            for (int v : eqs[k].vars) {
                const int c = col[static_cast<std::size_t>(v)];
                if (c < 0) continue;
                double& xv = x[static_cast<std::size_t>(v)];
                const double keep = xv, h = 1e-7 * std::max(1.0, std::fabs(keep));
                xv = keep + h;
                const double fp = eqs[k].f(x);
                xv = keep - h;
                const double fm = eqs[k].f(x);
                xv = keep;
                const double d = (fp - fm) / (2 * h);
                j[k * static_cast<std::size_t>(n) + static_cast<std::size_t>(c)] += std::isfinite(d) ? d : 0.0;
            }
        }
    }
};

double maxAbs(const std::vector<double>& r) {
    double m = 0;
    for (double v : r) m = std::max(m, std::fabs(v));
    return m;
}

double sumSquares(const std::vector<double>& r) {
    double s = 0;
    for (double v : r) s += v * v;
    return s;
}

}  // namespace

SolveReport solve(std::vector<double>& x, const std::vector<bool>& locked, const std::vector<Equation>& equations,
                  double tolerance, int maxIterations) {
    System sys{equations, std::vector<int>(x.size(), -1), 0};
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (locked.empty() || !locked[i]) sys.col[i] = sys.n++;
    }
    const int n = sys.n, m = static_cast<int>(equations.size());
    SolveReport rep;
    rep.variables = n;
    rep.movable.assign(x.size(), false);
    std::vector<double> r, j, rNew, step, xNew;
    sys.residuals(x, r);
    double mu = 1e-12;
    for (rep.iterations = 0; rep.iterations < maxIterations && maxAbs(r) > tolerance && n > 0; ++rep.iterations) {
        sys.jacobian(x, j);
        const double cost = sumSquares(r);
        bool improved = false;
        for (int tries = 0; tries < 30 && !improved; ++tries) {
            std::vector<double> delta(static_cast<std::size_t>(n), 0.0);
            if (m <= n) {
                // least change: delta = -J^T (J J^T + mu I)^-1 r
                std::vector<double> a(static_cast<std::size_t>(m) * m, 0.0), y;
                for (int p = 0; p < m; ++p) {
                    for (int q = p; q < m; ++q) {
                        double s = 0;
                        for (int c = 0; c < n; ++c) s += j[p * n + c] * j[q * n + c];
                        a[p * m + q] = a[q * m + p] = s;
                    }
                }
                double scale = 1;
                for (int p = 0; p < m; ++p) scale = std::max(scale, a[p * m + p]);
                for (int p = 0; p < m; ++p) a[p * m + p] += mu * scale;
                if (!solveDense(a, r, m, y)) {
                    mu *= 10;
                    continue;
                }
                for (int c = 0; c < n; ++c) {
                    double s = 0;
                    for (int p = 0; p < m; ++p) s += j[p * n + c] * y[static_cast<std::size_t>(p)];
                    delta[static_cast<std::size_t>(c)] = -s;
                }
            } else {
                std::vector<double> a(static_cast<std::size_t>(n) * n, 0.0), b(static_cast<std::size_t>(n), 0.0);
                for (int p = 0; p < n; ++p) {
                    for (int q = p; q < n; ++q) {
                        double s = 0;
                        for (int k = 0; k < m; ++k) s += j[k * n + p] * j[k * n + q];
                        a[p * n + q] = a[q * n + p] = s;
                    }
                    double s = 0;
                    for (int k = 0; k < m; ++k) s += j[k * n + p] * r[static_cast<std::size_t>(k)];
                    b[static_cast<std::size_t>(p)] = -s;
                }
                double scale = 1;
                for (int p = 0; p < n; ++p) scale = std::max(scale, a[p * n + p]);
                for (int p = 0; p < n; ++p) a[p * n + p] += mu * scale;
                if (!solveDense(a, b, n, delta)) {
                    mu *= 10;
                    continue;
                }
            }
            xNew = x;
            for (std::size_t i = 0; i < x.size(); ++i) {
                const int c = sys.col[i];
                if (c >= 0) xNew[i] += delta[static_cast<std::size_t>(c)];
            }
            sys.residuals(xNew, rNew);
            if (sumSquares(rNew) < cost) {
                x.swap(xNew);
                r.swap(rNew);
                mu = std::max(mu / 10, 1e-15);
                improved = true;
            } else {
                mu *= 10;
            }
        }
        if (!improved) break;
    }
    rep.residual = maxAbs(r);
    rep.converged = rep.residual <= tolerance;
    if (!rep.converged) {
        for (std::size_t k = 0; k < r.size(); ++k) {
            if (std::fabs(r[k]) > tolerance * 10) {
                const int o = equations[k].owner;
                if (std::find(rep.failing.begin(), rep.failing.end(), o) == rep.failing.end()) rep.failing.push_back(o);
            }
        }
    }

    // rank of the (row-normalised) Jacobian at the solution, by reduced row echelon form
    sys.jacobian(x, j);
    for (int k = 0; k < m; ++k) {
        double big = 0;
        for (int c = 0; c < n; ++c) big = std::max(big, std::fabs(j[k * n + c]));
        if (big > 0) {
            for (int c = 0; c < n; ++c) j[k * n + c] /= big;
        }
    }
    std::vector<int> pivotCol;
    std::vector<int> rowOfCol(static_cast<std::size_t>(n), -1);
    int row = 0;
    const double eps = 1e-6;
    for (int c = 0; c < n && row < m; ++c) {
        int piv = row;
        for (int k = row + 1; k < m; ++k) {
            if (std::fabs(j[k * n + c]) > std::fabs(j[piv * n + c])) piv = k;
        }
        if (std::fabs(j[piv * n + c]) < eps) continue;
        if (piv != row) {
            for (int q = 0; q < n; ++q) std::swap(j[row * n + q], j[piv * n + q]);
        }
        const double d = j[row * n + c];
        for (int q = 0; q < n; ++q) j[row * n + q] /= d;
        for (int k = 0; k < m; ++k) {
            if (k == row) continue;
            const double f = j[k * n + c];
            if (f == 0) continue;
            for (int q = 0; q < n; ++q) j[k * n + q] -= f * j[row * n + q];
        }
        rowOfCol[static_cast<std::size_t>(c)] = row;
        pivotCol.push_back(c);
        ++row;
    }
    rep.rank = row;
    rep.dof = n - row;
    rep.redundant = m - row;
    std::vector<bool> movableCol(static_cast<std::size_t>(n), false);
    for (int c = 0; c < n; ++c) {
        const int pr = rowOfCol[static_cast<std::size_t>(c)];
        if (pr < 0) {
            movableCol[static_cast<std::size_t>(c)] = true;
            continue;
        }
        for (int f = 0; f < n; ++f) {
            if (rowOfCol[static_cast<std::size_t>(f)] < 0 && std::fabs(j[pr * n + f]) > eps) {
                movableCol[static_cast<std::size_t>(c)] = true;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        const int c = sys.col[i];
        rep.movable[i] = c >= 0 && movableCol[static_cast<std::size_t>(c)];
    }
    return rep;
}

}  // namespace wetwaresuite::cad::detail
