// Formula functions: lookups and references (VLOOKUP, XLOOKUP, INDEX, MATCH,
// OFFSET, INDIRECT, ...) and dynamic arrays (SORT, FILTER, UNIQUE, SEQUENCE,
// TAKE, VSTACK, ...).
#include <algorithm>
#include <cmath>
#include <random>
#include <set>

#include "formula_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office::fx {

namespace {

// Picks element (row, col) of an Arg (1-based; 0 means the whole row/column), as INDEX does.
Arg indexInto(const Arg& src, int row, int col, Ctx& c) {
    int rows, cols;
    dims(src, c, rows, cols);
    if (row < 0 || col < 0) throw EvalError{"#VALUE!"};
    if (row > rows || col > cols) throw EvalError{"#REF!"};
    if (src.isRange) {
        Range out = src.r;
        if (row > 0) out.r1 = out.r2 = src.r.r1 + row - 1;
        if (col > 0) out.c1 = out.c2 = src.r.c1 + col - 1;
        return Arg::range(out);
    }
    Matrix m = toMatrix(src, c);
    if (row > 0 && col > 0) return Arg::val(m.at(row - 1, col - 1));
    if (row > 0) {
        Matrix out(1, m.cols);
        for (int j = 0; j < m.cols; ++j) out.at(0, j) = m.at(row - 1, j);
        return Arg::array(std::move(out));
    }
    if (col > 0) {
        Matrix out(m.rows, 1);
        for (int i = 0; i < m.rows; ++i) out.at(i, 0) = m.at(i, col - 1);
        return Arg::array(std::move(out));
    }
    return src;
}

// XLOOKUP / XMATCH search: index of look in list, or -1.
int xfind(const std::vector<CellValue>& list, const CellValue& look, int mode, int search) {
    auto order = [&](std::size_t k) { return search < 0 ? list.size() - 1 - k : k; };
    if (search == 2 || search == -2) {
        // binary search on sorted data (ascending for 2, descending for -2)
        int lo = 0, hi = static_cast<int>(list.size()) - 1, found = -1, below = -1, above = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            int cmp = compareValues(list[static_cast<std::size_t>(mid)], look);
            if (search == -2) cmp = -cmp;
            if (cmp == 0) {
                found = mid;
                break;
            }
            if (cmp < 0) {
                below = mid;
                lo = mid + 1;
            } else {
                above = mid;
                hi = mid - 1;
            }
        }
        if (found >= 0) return found;
        if (mode == -1) return search == 2 ? below : above;
        if (mode == 1) return search == 2 ? above : below;
        return -1;
    }
    if (mode == 0 || mode == 2) {
        for (std::size_t k = 0; k < list.size(); ++k) {
            if (lookupEqual(look, list[order(k)], mode == 2)) return static_cast<int>(order(k));
        }
        return -1;
    }
    // exact, else the next smaller (-1) or next larger (1) value; data needn't be sorted
    int best = -1;
    for (std::size_t k = 0; k < list.size(); ++k) {
        const std::size_t i = order(k);
        const CellValue& v = list[i];
        if (v.empty() || v.isError() || (v.type == CellValue::Text) != (look.type == CellValue::Text)) continue;
        int cmp = compareValues(v, look);
        if (cmp == 0) return static_cast<int>(i);
        if ((mode < 0 && cmp < 0) || (mode > 0 && cmp > 0)) {
            if (best < 0 || (mode < 0 ? compareValues(v, list[static_cast<std::size_t>(best)]) > 0
                                      : compareValues(v, list[static_cast<std::size_t>(best)]) < 0)) {
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

bool vertical(const Arg& a, Ctx& c) {
    int rows, cols;
    dims(a, c, rows, cols);
    return !(rows == 1 && cols > 1);
}

// The address text of a cell: $A$1, A$1, R1C1 styles.
std::string address(int row, int col, int absNum, bool a1, const std::string& sheet) {
    std::string s;
    if (a1) {
        const bool absR = absNum == 1 || absNum == 2, absC = absNum == 1 || absNum == 3;
        s = (absC ? "$" : "") + colName(col) + (absR ? "$" : "") + std::to_string(row + 1);
    } else {
        const bool absR = absNum == 1 || absNum == 2, absC = absNum == 1 || absNum == 3;
        s = absR ? "R" + std::to_string(row + 1) : "R[" + std::to_string(row + 1) + "]";
        s += absC ? "C" + std::to_string(col + 1) : "C[" + std::to_string(col + 1) + "]";
    }
    return sheet.empty() ? s : quoteSheet(sheet) + "!" + s;
}

void addLookups(FnMap& m) {
    m["VLOOKUP"] = [](const auto& a, Ctx& c) {
        need(a, 3, 4);
        CellValue look = evalScalar(a[0], c);
        if (look.isError()) throw EvalError{look.text};
        Arg table = evalArg(a[1], c);
        int col = evalInt(a[2], c);
        bool approx = missing(a, 3) ? true : evalBool(a[3], c);
        int rows, cols;
        dims(table, c, rows, cols);
        if (col < 1) throw EvalError{"#VALUE!"};
        if (col > cols) throw EvalError{"#REF!"};
        if (table.isRange) {
            Range first = table.r;
            first.c2 = first.c1;
            int idx = findIn(lineOf(Arg::range(first), c), look, approx ? 1 : 0);
            if (idx < 0) throw EvalError{"#N/A"};
            const Range& t = table.r;
            return Arg::range(Range{t.sheet, t.r1 + idx, t.c1 + col - 1, t.r1 + idx, t.c1 + col - 1});
        }
        Matrix mm = toMatrix(table, c);
        std::vector<CellValue> first;
        for (int i = 0; i < mm.rows; ++i) first.push_back(mm.at(i, 0));
        int idx = findIn(first, look, approx ? 1 : 0);
        if (idx < 0) throw EvalError{"#N/A"};
        return Arg::val(mm.at(idx, col - 1));
    };
    m["HLOOKUP"] = [](const auto& a, Ctx& c) {
        need(a, 3, 4);
        CellValue look = evalScalar(a[0], c);
        if (look.isError()) throw EvalError{look.text};
        Arg table = evalArg(a[1], c);
        int row = evalInt(a[2], c);
        bool approx = missing(a, 3) ? true : evalBool(a[3], c);
        int rows, cols;
        dims(table, c, rows, cols);
        if (row < 1) throw EvalError{"#VALUE!"};
        if (row > rows) throw EvalError{"#REF!"};
        if (table.isRange) {
            Range first = table.r;
            first.r2 = first.r1;
            int idx = findIn(lineOf(Arg::range(first), c), look, approx ? 1 : 0);
            if (idx < 0) throw EvalError{"#N/A"};
            const Range& t = table.r;
            return Arg::range(Range{t.sheet, t.r1 + row - 1, t.c1 + idx, t.r1 + row - 1, t.c1 + idx});
        }
        Matrix mm = toMatrix(table, c);
        std::vector<CellValue> first(mm.v.begin(), mm.v.begin() + mm.cols);
        int idx = findIn(first, look, approx ? 1 : 0);
        if (idx < 0) throw EvalError{"#N/A"};
        return Arg::val(mm.at(row - 1, idx));
    };
    m["LOOKUP"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        CellValue look = evalScalar(a[0], c);
        if (look.isError()) throw EvalError{look.text};
        Arg lr = evalArg(a[1], c);
        Arg rr = missing(a, 2) ? lr : evalArg(a[2], c);
        if (missing(a, 2)) {
            // the array form: search the first row/column, return from the last
            int rows, cols;
            dims(lr, c, rows, cols);
            Matrix mm = toMatrix(lr, c);
            const bool byCols = cols > rows;
            std::vector<CellValue> first;
            for (int i = 0; i < (byCols ? cols : rows); ++i) first.push_back(byCols ? mm.at(0, i) : mm.at(i, 0));
            int idx = findIn(first, look, 1);
            if (idx < 0) throw EvalError{"#N/A"};
            return Arg::val(byCols ? mm.at(rows - 1, idx) : mm.at(idx, cols - 1));
        }
        int idx = findIn(lineOf(lr, c), look, 1);
        if (idx < 0) throw EvalError{"#N/A"};
        std::vector<CellValue> res = lineOf(rr, c);
        if (rr.isRange) {
            Range out = rr.r;
            if (vertical(rr, c)) out.r1 = out.r2 = rr.r.r1 + idx;
            else out.c1 = out.c2 = rr.r.c1 + idx;
            return Arg::range(out);
        }
        if (idx >= static_cast<int>(res.size())) throw EvalError{"#N/A"};
        return Arg::val(res[static_cast<std::size_t>(idx)]);
    };
    m["MATCH"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        CellValue look = evalScalar(a[0], c);
        if (look.isError()) throw EvalError{look.text};
        Arg r = evalArg(a[1], c);
        int type = missing(a, 2) ? 1 : evalInt(a[2], c);
        int rows, cols;
        dims(r, c, rows, cols);
        if (rows > 1 && cols > 1) throw EvalError{"#N/A"};
        int idx = findIn(lineOf(r, c), look, type == 0 ? 0 : type > 0 ? 1 : -1);
        if (idx < 0) throw EvalError{"#N/A"};
        return num(idx + 1);
    };
    m["XMATCH"] = [](const auto& a, Ctx& c) {
        need(a, 2, 4);
        CellValue look = evalScalar(a[0], c);
        if (look.isError()) throw EvalError{look.text};
        Arg r = evalArg(a[1], c);
        const int mode = missing(a, 2) ? 0 : evalInt(a[2], c), search = missing(a, 3) ? 1 : evalInt(a[3], c);
        int idx = xfind(lineOf(r, c), look, mode, search);
        if (idx < 0) throw EvalError{"#N/A"};
        return num(idx + 1);
    };
    m["INDEX"] = [](const auto& a, Ctx& c) {
        need(a, 2, 4);
        Arg src = evalArg(a[0], c);
        int row = missing(a, 1) ? 0 : evalInt(a[1], c);
        int col = missing(a, 2) ? 0 : evalInt(a[2], c);
        int rows, cols;
        dims(src, c, rows, cols);
        if (missing(a, 2) && rows == 1 && cols > 1) {  // one index into a single row
            col = row;
            row = 1;
        } else if (missing(a, 2) && cols == 1) {
            col = 1;
        }
        return indexInto(src, row, col, c);
    };
    m["XLOOKUP"] = [](const auto& a, Ctx& c) {
        need(a, 3, 6);
        CellValue look = evalScalar(a[0], c);
        if (look.isError()) throw EvalError{look.text};
        Arg lr = evalArg(a[1], c);
        Arg rr = evalArg(a[2], c);
        int mode = missing(a, 4) ? 0 : evalInt(a[4], c);
        int search = missing(a, 5) ? 1 : evalInt(a[5], c);
        std::vector<CellValue> list = lineOf(lr, c);
        const bool vert = vertical(lr, c);
        int idx = xfind(list, look, mode, search);
        if (idx < 0) {
            if (!missing(a, 3)) return evalArg(a[3], c);
            throw EvalError{"#N/A"};
        }
        // the matching row (or column) of the return array, which may be several wide
        if (rr.isRange) {
            Range out = rr.r;
            if (vert) out.r1 = out.r2 = rr.r.r1 + idx;
            else out.c1 = out.c2 = rr.r.c1 + idx;
            if (out.r1 >= kMaxRows || out.c1 >= kMaxCols) throw EvalError{"#REF!"};
            return Arg::range(out);
        }
        Matrix mm = toMatrix(rr, c);
        if (vert) {
            if (idx >= mm.rows) throw EvalError{"#REF!"};
            Matrix out(1, mm.cols);
            for (int j = 0; j < mm.cols; ++j) out.at(0, j) = mm.at(idx, j);
            return out.cols == 1 ? Arg::val(out.v[0]) : Arg::array(std::move(out));
        }
        if (idx >= mm.cols) throw EvalError{"#REF!"};
        Matrix out(mm.rows, 1);
        for (int i = 0; i < mm.rows; ++i) out.at(i, 0) = mm.at(i, idx);
        return out.rows == 1 ? Arg::val(out.v[0]) : Arg::array(std::move(out));
    };
    m["CHOOSE"] = [](const auto& a, Ctx& c) {
        if (a.size() < 2) throw EvalError{"#VALUE!"};
        Arg which = evalArg(a[0], c);
        if (which.multi()) {
            Matrix w = toMatrix(which, c);
            for (auto& v : w.v) {
                const int k = static_cast<int>(toNum(v));
                if (k < 1 || k >= static_cast<int>(a.size())) v = CellValue::error("#VALUE!");
                else v = evalScalar(a[static_cast<std::size_t>(k)], c);
            }
            return Arg::array(std::move(w));
        }
        const int k = static_cast<int>(std::trunc(toNum(scalar(which, c))));
        if (k < 1 || k >= static_cast<int>(a.size())) throw EvalError{"#VALUE!"};
        return evalArg(a[static_cast<std::size_t>(k)], c);
    };
}

void addReferences(FnMap& m) {
    m["ROW"] = [](const auto& a, Ctx& c) {
        need(a, 0, 1);
        if (a.empty() || missing(a, 0)) return num(c.row + 1);
        Range r = rangeArg(a[0], c);
        if (r.rows() > 1 && r.r2 < kMaxRows - 1) {
            Matrix out(r.rows(), 1);
            for (int i = 0; i < r.rows(); ++i) out.at(i, 0) = CellValue::num(r.r1 + i + 1);
            return Arg::array(std::move(out));
        }
        return num(r.r1 + 1);
    };
    m["COLUMN"] = [](const auto& a, Ctx& c) {
        need(a, 0, 1);
        if (a.empty() || missing(a, 0)) return num(c.col + 1);
        Range r = rangeArg(a[0], c);
        if (r.cols() > 1 && r.c2 < kMaxCols - 1) {
            Matrix out(1, r.cols());
            for (int j = 0; j < r.cols(); ++j) out.at(0, j) = CellValue::num(r.c1 + j + 1);
            return Arg::array(std::move(out));
        }
        return num(r.c1 + 1);
    };
    m["ROWS"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Arg v = evalArg(a[0], c);
        if (v.isRange) return num(v.r.rows());
        int rows, cols;
        dims(v, c, rows, cols);
        return num(rows);
    };
    m["COLUMNS"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Arg v = evalArg(a[0], c);
        if (v.isRange) return num(v.r.cols());
        int rows, cols;
        dims(v, c, rows, cols);
        return num(cols);
    };
    m["OFFSET"] = [](const auto& a, Ctx& c) {
        need(a, 3, 5);
        Range base = rangeArg(a[0], c);
        const int dr = evalInt(a[1], c), dc = evalInt(a[2], c);
        const int h = missing(a, 3) ? base.rows() : evalInt(a[3], c);
        const int w = missing(a, 4) ? base.cols() : evalInt(a[4], c);
        if (h < 1 || w < 1) throw EvalError{"#REF!"};
        Range out{base.sheet, base.r1 + dr, base.c1 + dc, base.r1 + dr + h - 1, base.c1 + dc + w - 1};
        if (out.r1 < 0 || out.c1 < 0 || out.r2 >= kMaxRows || out.c2 >= kMaxCols) throw EvalError{"#REF!"};
        return Arg::range(out);
    };
    m["INDIRECT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 2);
        std::string text = trimCopy(evalStr(a[0], c));
        const bool a1 = missing(a, 1) || evalBool(a[1], c);
        Range r;
        std::string err;
        if (!a1) {
            // R1C1: R2C3 or R2C3:R4C5
            int r1, c1, r2, c2;
            char x;
            if (std::sscanf(text.c_str(), "R%dC%d:R%dC%d%c", &r1, &c1, &r2, &c2, &x) == 4) {
                r = Range{c.sheet, r1 - 1, c1 - 1, r2 - 1, c2 - 1};
            } else if (std::sscanf(text.c_str(), "R%dC%d%c", &r1, &c1, &x) == 2) {
                r = Range{c.sheet, r1 - 1, c1 - 1, r1 - 1, c1 - 1};
            } else {
                throw EvalError{"#REF!"};
            }
            if (r.r1 < 0 || r.c1 < 0) throw EvalError{"#REF!"};
            return Arg::range(r);
        }
        if (!parseRange(c.wb, text, c.sheet, r, err)) throw EvalError{"#REF!"};
        return Arg::range(r);
    };
    m["ADDRESS"] = [](const auto& a, Ctx& c) {
        need(a, 2, 5);
        const int row = evalInt(a[0], c), col = evalInt(a[1], c);
        const int absNum = missing(a, 2) ? 1 : evalInt(a[2], c);
        const bool a1 = missing(a, 3) || evalBool(a[3], c);
        const std::string sheet = missing(a, 4) ? std::string() : evalStr(a[4], c);
        if (row < 1 || col < 1 || row > kMaxRows || col > kMaxCols || absNum < 1 || absNum > 4) throw EvalError{"#VALUE!"};
        return str(address(row - 1, col - 1, absNum, a1, sheet));
    };
    m["ANCHORARRAY"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        Range r = rangeArg(a[0], c);
        cellValue(c, r.sheet, r.r1, r.c1);  // make sure it has run
        const Cell* cell = cellAt(c, r.sheet, r.r1, r.c1);
        if (!cell || !cell->hasFormula()) throw EvalError{"#REF!"};
        if (cell->value.isError() && cell->value.text == "#SPILL!") throw EvalError{"#REF!"};
        const int rows = std::max(1, cell->spillRows), cols = std::max(1, cell->spillCols);
        return Arg::range(Range{r.sheet, r.r1, r.c1, r.r1 + rows - 1, r.c1 + cols - 1});
    };
    m["SINGLE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return Arg::val(evalScalar(a[0], c));
    };
}

// ---- dynamic arrays ------------------------------------------------------------

Matrix matrixArg(const NodeP& n, Ctx& c) { return toMatrix(evalArg(n, c), c); }

int sortCompare(const CellValue& a, const CellValue& b) {
    auto cls = [](const CellValue& v) {
        switch (v.type) {
            case CellValue::Number: return 0;
            case CellValue::Text: return 1;
            case CellValue::Bool: return 2;
            case CellValue::Error: return 3;
            default: return 4;
        }
    };
    const int ca = cls(a), cb = cls(b);
    if (ca != cb) return ca < cb ? -1 : 1;
    return compareValues(a, b);
}

Matrix pickRows(const Matrix& m, const std::vector<int>& rows) {
    Matrix out(static_cast<int>(rows.size()), m.cols);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        for (int j = 0; j < m.cols; ++j) out.at(static_cast<int>(i), j) = m.at(rows[i], j);
    }
    return out;
}

Matrix transpose(const Matrix& m) {
    Matrix out(m.cols, m.rows);
    for (int i = 0; i < m.rows; ++i) {
        for (int j = 0; j < m.cols; ++j) out.at(j, i) = m.at(i, j);
    }
    return out;
}

std::string rowKey(const Matrix& m, int i) {
    std::string k;
    for (int j = 0; j < m.cols; ++j) {
        const CellValue& v = m.at(i, j);
        k += std::to_string(static_cast<int>(v.type)) + ":" + (v.type == CellValue::Text ? lower(v.text) : textNumber(v.number)) + "\x1f";
    }
    return k;
}

void addArrays1(FnMap& m) {
    m["SORT"] = [](const auto& a, Ctx& c) {
        need(a, 1, 4);
        Matrix mm = matrixArg(a[0], c);
        const int index = missing(a, 1) ? 1 : evalInt(a[1], c);
        const int order = missing(a, 2) ? 1 : evalInt(a[2], c);
        const bool byCol = !missing(a, 3) && evalBool(a[3], c);
        if (byCol) mm = transpose(mm);
        if (index < 1 || index > mm.cols || (order != 1 && order != -1)) throw EvalError{"#VALUE!"};
        std::vector<int> rows(static_cast<std::size_t>(mm.rows));
        for (int i = 0; i < mm.rows; ++i) rows[static_cast<std::size_t>(i)] = i;
        std::stable_sort(rows.begin(), rows.end(), [&](int x, int y) {
            const int cmp = sortCompare(mm.at(x, index - 1), mm.at(y, index - 1));
            return order > 0 ? cmp < 0 : cmp > 0;
        });
        Matrix out = pickRows(mm, rows);
        return Arg::array(byCol ? transpose(out) : out);
    };
    m["SORTBY"] = [](const auto& a, Ctx& c) {
        if (a.size() < 2) throw EvalError{"#VALUE!"};
        Matrix mm = matrixArg(a[0], c);
        std::vector<std::pair<std::vector<CellValue>, int>> keys;
        for (std::size_t i = 1; i < a.size(); i += 2) {
            std::vector<CellValue> k = lineOf(evalArg(a[i], c), c);
            if (static_cast<int>(k.size()) != mm.rows) throw EvalError{"#VALUE!"};
            const int order = i + 1 < a.size() && !missing(a, i + 1) ? evalInt(a[i + 1], c) : 1;
            keys.emplace_back(std::move(k), order);
        }
        std::vector<int> rows(static_cast<std::size_t>(mm.rows));
        for (int i = 0; i < mm.rows; ++i) rows[static_cast<std::size_t>(i)] = i;
        std::stable_sort(rows.begin(), rows.end(), [&](int x, int y) {
            for (const auto& [k, order] : keys) {
                const int cmp = sortCompare(k[static_cast<std::size_t>(x)], k[static_cast<std::size_t>(y)]);
                if (cmp != 0) return order > 0 ? cmp < 0 : cmp > 0;
            }
            return false;
        });
        return Arg::array(pickRows(mm, rows));
    };
    m["FILTER"] = [](const auto& a, Ctx& c) {
        need(a, 2, 3);
        Matrix mm = matrixArg(a[0], c);
        Matrix inc = matrixArg(a[1], c);
        const bool byRows = inc.cols == 1 && inc.rows == mm.rows;
        const bool byCols = !byRows && inc.rows == 1 && inc.cols == mm.cols;
        if (!byRows && !byCols) throw EvalError{"#VALUE!"};
        std::vector<int> keep;
        for (std::size_t i = 0; i < inc.v.size(); ++i) {
            const CellValue& v = inc.v[i];
            if (v.isError()) throw EvalError{v.text};
            if (toBool(v.type == CellValue::Text ? CellValue::boolean(false) : v)) keep.push_back(static_cast<int>(i));
        }
        if (keep.empty()) {
            if (!missing(a, 2)) return evalArg(a[2], c);
            throw EvalError{"#CALC!"};
        }
        if (byRows) return Arg::array(pickRows(mm, keep));
        return Arg::array(transpose(pickRows(transpose(mm), keep)));
    };
    m["UNIQUE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 3);
        Matrix mm = matrixArg(a[0], c);
        const bool byCol = !missing(a, 1) && evalBool(a[1], c);
        const bool once = !missing(a, 2) && evalBool(a[2], c);
        if (byCol) mm = transpose(mm);
        std::vector<std::string> keys;
        std::map<std::string, int> count;
        for (int i = 0; i < mm.rows; ++i) {
            keys.push_back(rowKey(mm, i));
            ++count[keys.back()];
        }
        std::vector<int> rows;
        std::set<std::string> seen;
        for (int i = 0; i < mm.rows; ++i) {
            const std::string& k = keys[static_cast<std::size_t>(i)];
            if (once ? count[k] == 1 : seen.insert(k).second) rows.push_back(i);
        }
        if (rows.empty()) throw EvalError{"#CALC!"};
        Matrix out = pickRows(mm, rows);
        return Arg::array(byCol ? transpose(out) : out);
    };
    m["SEQUENCE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 4);
        const int rows = evalInt(a[0], c), cols = missing(a, 1) ? 1 : evalInt(a[1], c);
        const double start = missing(a, 2) ? 1 : evalNum(a[2], c), step = missing(a, 3) ? 1 : evalNum(a[3], c);
        if (rows < 1 || cols < 1) throw EvalError{"#CALC!"};
        if (static_cast<long long>(rows) * cols > 1000000) throw EvalError{"#NUM!"};
        Matrix out(rows, cols);
        for (int i = 0; i < rows * cols; ++i) out.v[static_cast<std::size_t>(i)] = CellValue::num(start + step * i);
        return Arg::array(std::move(out));
    };
    m["RANDARRAY"] = [](const auto& a, Ctx& c) {
        need(a, 0, 5);
        const int rows = missing(a, 0) ? 1 : evalInt(a[0], c), cols = missing(a, 1) ? 1 : evalInt(a[1], c);
        const double lo = missing(a, 2) ? 0 : evalNum(a[2], c), hi = missing(a, 3) ? 1 : evalNum(a[3], c);
        const bool whole = !missing(a, 4) && evalBool(a[4], c);
        if (rows < 1 || cols < 1 || lo > hi) throw EvalError{"#VALUE!"};
        if (static_cast<long long>(rows) * cols > 1000000) throw EvalError{"#NUM!"};
        static std::mt19937_64 g{std::random_device{}()};
        Matrix out(rows, cols);
        for (auto& v : out.v) {
            v = whole ? CellValue::num(static_cast<double>(std::uniform_int_distribution<long long>(static_cast<long long>(std::ceil(lo)), static_cast<long long>(std::floor(hi)))(g)))
                      : CellValue::num(std::uniform_real_distribution<double>(lo, hi)(g));
        }
        return Arg::array(std::move(out));
    };
    m["TRANSPOSE"] = [](const auto& a, Ctx& c) {
        need(a, 1, 1);
        return Arg::array(transpose(matrixArg(a[0], c)));
    };
    m["MMULT"] = [](const auto& a, Ctx& c) {
        need(a, 2, 2);
        Matrix x = matrixArg(a[0], c), y = matrixArg(a[1], c);
        if (x.cols != y.rows) throw EvalError{"#VALUE!"};
        Matrix out(x.rows, y.cols);
        for (int i = 0; i < x.rows; ++i) {
            for (int j = 0; j < y.cols; ++j) {
                double s = 0;
                for (int k = 0; k < x.cols; ++k) {
                    const CellValue& p = x.at(i, k);
                    const CellValue& q = y.at(k, j);
                    if (p.type != CellValue::Number || q.type != CellValue::Number) throw EvalError{"#VALUE!"};
                    s += p.number * q.number;
                }
                out.at(i, j) = CellValue::num(s);
            }
        }
        return Arg::array(std::move(out));
    };
}

void addArrays2(FnMap& m) {
    auto takeDrop = [](bool take) {
        return [take](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 2, 3);
            Matrix mm = matrixArg(a[0], c);
            int rows = missing(a, 1) ? (take ? mm.rows : 0) : evalInt(a[1], c);
            int cols = missing(a, 2) ? (take ? mm.cols : 0) : evalInt(a[2], c);
            auto span = [take](int n, int size, int& from, int& count) {
                if (take) {
                    count = std::min(std::abs(n), size);
                    from = n >= 0 ? 0 : size - count;
                } else {
                    const int d = std::min(std::abs(n), size);
                    count = size - d;
                    from = n >= 0 ? d : 0;
                }
            };
            int r0, rn, c0, cn;
            span(rows, mm.rows, r0, rn);
            span(cols, mm.cols, c0, cn);
            if (rn <= 0 || cn <= 0) throw EvalError{"#CALC!"};
            Matrix out(rn, cn);
            for (int i = 0; i < rn; ++i) {
                for (int j = 0; j < cn; ++j) out.at(i, j) = mm.at(r0 + i, c0 + j);
            }
            return Arg::array(std::move(out));
        };
    };
    m["TAKE"] = takeDrop(true);
    m["DROP"] = takeDrop(false);
    auto stack = [](bool vertical) {
        return [vertical](const std::vector<NodeP>& a, Ctx& c) {
            if (a.empty()) throw EvalError{"#VALUE!"};
            std::vector<Matrix> parts;
            int rows = 0, cols = 0;
            for (const auto& x : a) {
                parts.push_back(matrixArg(x, c));
                if (vertical) {
                    rows += parts.back().rows;
                    cols = std::max(cols, parts.back().cols);
                } else {
                    cols += parts.back().cols;
                    rows = std::max(rows, parts.back().rows);
                }
            }
            Matrix out(rows, cols);
            for (auto& v : out.v) v = CellValue::error("#N/A");
            int at = 0;
            for (const auto& p : parts) {
                for (int i = 0; i < p.rows; ++i) {
                    for (int j = 0; j < p.cols; ++j) out.at(vertical ? at + i : i, vertical ? j : at + j) = p.at(i, j);
                }
                at += vertical ? p.rows : p.cols;
            }
            return Arg::array(std::move(out));
        };
    };
    m["VSTACK"] = stack(true);
    m["HSTACK"] = stack(false);
    auto choose = [](bool rowsWanted) {
        return [rowsWanted](const std::vector<NodeP>& a, Ctx& c) {
            if (a.size() < 2) throw EvalError{"#VALUE!"};
            Matrix mm = matrixArg(a[0], c);
            if (!rowsWanted) mm = transpose(mm);
            std::vector<int> pick;
            for (std::size_t i = 1; i < a.size(); ++i) {
                for (const auto& v : matrixArg(a[i], c).v) {
                    int k = static_cast<int>(toNum(v));
                    if (k < 0) k = mm.rows + k + 1;
                    if (k < 1 || k > mm.rows) throw EvalError{"#VALUE!"};
                    pick.push_back(k - 1);
                }
            }
            Matrix out = pickRows(mm, pick);
            return Arg::array(rowsWanted ? out : transpose(out));
        };
    };
    m["CHOOSEROWS"] = choose(true);
    m["CHOOSECOLS"] = choose(false);
    auto flatten = [](bool toCol) {
        return [toCol](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 1, 3);
            Matrix mm = matrixArg(a[0], c);
            const int ignore = missing(a, 1) ? 0 : evalInt(a[1], c);
            const bool byCol = !missing(a, 2) && evalBool(a[2], c);
            if (byCol) mm = transpose(mm);
            std::vector<CellValue> vals;
            for (const auto& v : mm.v) {
                if ((ignore == 1 || ignore == 3) && v.empty()) continue;
                if ((ignore == 2 || ignore == 3) && v.isError()) continue;
                vals.push_back(v);
            }
            if (vals.empty()) throw EvalError{"#CALC!"};
            Matrix out(toCol ? static_cast<int>(vals.size()) : 1, toCol ? 1 : static_cast<int>(vals.size()));
            out.v = std::move(vals);
            return Arg::array(std::move(out));
        };
    };
    m["TOCOL"] = flatten(true);
    m["TOROW"] = flatten(false);
    auto wrap = [](bool wrapRows) {
        return [wrapRows](const std::vector<NodeP>& a, Ctx& c) {
            need(a, 2, 3);
            Matrix mm = matrixArg(a[0], c);
            if (mm.rows != 1 && mm.cols != 1) throw EvalError{"#VALUE!"};
            const int n = evalInt(a[1], c);
            if (n < 1) throw EvalError{"#NUM!"};
            CellValue pad = missing(a, 2) ? CellValue::error("#N/A") : evalScalar(a[2], c);
            const int count = static_cast<int>(mm.v.size()), other = (count + n - 1) / n;
            Matrix out(wrapRows ? other : n, wrapRows ? n : other);
            for (int k = 0; k < static_cast<int>(out.v.size()); ++k) {
                const int major = k / n, minor = k % n;
                CellValue& dst = wrapRows ? out.at(major, minor) : out.at(minor, major);
                dst = k < count ? mm.v[static_cast<std::size_t>(k)] : pad;
            }
            return Arg::array(std::move(out));
        };
    };
    m["WRAPROWS"] = wrap(true);
    m["WRAPCOLS"] = wrap(false);
    m["EXPAND"] = [](const auto& a, Ctx& c) {
        need(a, 2, 4);
        Matrix mm = matrixArg(a[0], c);
        const int rows = missing(a, 1) ? mm.rows : evalInt(a[1], c), cols = missing(a, 2) ? mm.cols : evalInt(a[2], c);
        if (rows < mm.rows || cols < mm.cols) throw EvalError{"#VALUE!"};
        CellValue pad = missing(a, 3) ? CellValue::error("#N/A") : evalScalar(a[3], c);
        Matrix out(rows, cols);
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j) out.at(i, j) = i < mm.rows && j < mm.cols ? mm.at(i, j) : pad;
        }
        return Arg::array(std::move(out));
    };
}

}  // namespace

void addLookupFunctions(FnMap& m) {
    addLookups(m);
    addReferences(m);
}

void addArrayFunctions(FnMap& m) {
    addArrays1(m);
    addArrays2(m);
}

}  // namespace wetwaresuite::office::fx
