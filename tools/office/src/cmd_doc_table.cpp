// office.doc_table_*: Word-style table editing — cell text and shading, merged
// cells, inserting and deleting rows and columns, column widths, borders, banding.
#include <algorithm>
#include <cmath>

#include "doc_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::doccmd {

namespace {

std::size_t columnCount(const Block& b) {
    std::size_t cols = 0;
    for (const auto& r : b.rows) cols = std::max(cols, r.size());
    return cols;
}

bool tableArg(TextDocument& d, const Params& p, Block*& out, std::size_t& index, std::string& error) {
    if (!blockArg(d, p, "block", index, error)) return false;
    Block& b = d.blocks[index];
    if (b.kind != Block::Table) {
        error = "block " + std::to_string(index + 1) + " is not a table (office.doc_info lists the blocks)";
        return false;
    }
    const std::size_t cols = columnCount(b);
    for (auto& r : b.rows) r.resize(cols);
    out = &b;
    return true;
}

void fitFormats(Block& b) {
    const std::size_t cols = columnCount(b);
    b.cellFormats.resize(b.rows.size());
    for (auto& r : b.cellFormats) r.resize(cols);
}

void dropEmptyFormats(Block& b) {
    for (const auto& r : b.cellFormats) {
        for (const auto& f : r) {
            if (f.fill || f.align || f.valign) return;
        }
    }
    b.cellFormats.clear();
}

bool cellArg(const Block& b, const Params& p, const char* key, std::size_t limit, std::size_t& out, std::string& error) {
    const std::int64_t n = intParam(p, key, 0);
    if (n < 1 || n > static_cast<std::int64_t>(limit)) {
        error = std::string(key) + " must be 1.." + std::to_string(limit);
        return false;
    }
    (void)b;
    out = static_cast<std::size_t>(n - 1);
    return true;
}

const CellSpan* mergeAt(const Block& b, int r, int c) {
    for (const auto& m : b.merges) {
        if (r >= m.row && r < m.row + m.rows && c >= m.col && c < m.col + m.cols) return &m;
    }
    return nullptr;
}

// Moves spans after rows (or columns) are inserted at `at` (count > 0) or deleted from `at` (count < 0).
void shiftMerges(Block& b, bool rows, int at, int count) {
    std::vector<CellSpan> out;
    for (CellSpan m : b.merges) {
        int& start = rows ? m.row : m.col;
        int& len = rows ? m.rows : m.cols;
        if (count > 0) {
            if (start >= at) start += count;
            else if (start + len > at) len += count;  // inserted inside the span: it grows
        } else {
            const int k = at, c = -count, e = start + len;
            const int overlap = std::max(0, std::min(e, k + c) - std::max(start, k));
            const int newStart = start < k ? start : (start >= k + c ? start - c : k);
            len -= overlap;
            start = newStart;
            if (len <= 0) continue;
        }
        if (m.rows > 1 || m.cols > 1) out.push_back(m);
    }
    b.merges = std::move(out);
}

Runs cellRuns(const std::string& text) { return inlineLines(text); }

std::string cellName(std::size_t r, std::size_t c) { return "row " + std::to_string(r + 1) + ", column " + std::to_string(c + 1); }

}  // namespace

void registerDocTableCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"office.doc_table_cell", "Table Cell",
             "Set a table cell's text (inline Markdown; a line break makes a new line in the cell) and/or format cells: fill colour, "
             "horizontal align and vertical align. to_row/to_col format a whole range of cells.",
             {doc(), integer("block", "the table's block number", true), integer("row", "row (1-based)", true), integer("col", "column (1-based)", true),
              integer("to_row", "last row of a range to format"), integer("to_col", "last column of a range to format"), str("text", "new cell text"),
              str("fill", "background colour (#rrggbb or a name; none removes it)"), str("align", "text alignment", false, {}, {"left", "center", "right"}),
              str("valign", "vertical alignment", false, {}, {"top", "middle", "bottom"}), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                Block* b;
                std::size_t bi, r1, c1, r2, c2;
                std::string error;
                if (!tableArg(d.data, p, b, bi, error)) return Result::failure(error);
                const std::size_t rows = b->rows.size(), cols = columnCount(*b);
                if (!cellArg(*b, p, "row", rows, r1, error) || !cellArg(*b, p, "col", cols, c1, error)) return Result::failure(error);
                r2 = r1, c2 = c1;
                if (has(p, "to_row") && !cellArg(*b, p, "to_row", rows, r2, error)) return Result::failure(error);
                if (has(p, "to_col") && !cellArg(*b, p, "to_col", cols, c2, error)) return Result::failure(error);
                if (r2 < r1) std::swap(r1, r2);
                if (c2 < c1) std::swap(c1, c2);
                std::vector<std::string> did;
                if (has(p, "text")) {
                    if (r1 != r2 || c1 != c2) return Result::failure("text= sets one cell; leave out to_row/to_col");
                    const CellSpan* m = mergeAt(*b, static_cast<int>(r1), static_cast<int>(c1));
                    if (m && (m->row != static_cast<int>(r1) || m->col != static_cast<int>(c1))) {
                        return Result::failure(cellName(r1, c1) + " is inside a merged cell; its text is in " + cellName(static_cast<std::size_t>(m->row), static_cast<std::size_t>(m->col)));
                    }
                    Runs fresh = cellRuns(textParam(p, "text"));
                    Runs& cell = b->rows[r1][c1];
                    if (d.data.trackChanges) {
                        const int del = newChange(d.data, false), ins = newChange(d.data, true);
                        cell = trackedReplace(d.data, cell, fresh, del, ins);
                    } else {
                        cell = fresh;
                    }
                    did.push_back("text set");
                }
                const bool fmt = has(p, "fill") || has(p, "align") || has(p, "valign");
                if (fmt) {
                    fitFormats(*b);
                    std::uint32_t fill = 0;
                    if (has(p, "fill")) {
                        const std::string t = stringParam(p, "fill");
                        if (lower(t) != "none" && !t.empty() && !parseColor(t, fill)) return Result::failure("fill must be a colour like #dde7f3 or lightblue (none removes it)");
                        did.push_back("fill " + colorName(fill));
                    }
                    const std::string al = lower(stringParam(p, "align")), va = lower(stringParam(p, "valign"));
                    const char a = al == "center" ? 'c' : al == "right" ? 'r' : 0;
                    const char v = va == "middle" ? 'm' : va == "bottom" ? 'b' : 0;
                    if (has(p, "align")) did.push_back("align " + al);
                    if (has(p, "valign")) did.push_back("valign " + va);
                    for (std::size_t r = r1; r <= r2; ++r) {
                        for (std::size_t c = c1; c <= c2; ++c) {
                            CellFormat& f = b->cellFormats[r][c];
                            if (has(p, "fill")) f.fill = fill;
                            if (has(p, "align")) f.align = a;
                            if (has(p, "valign")) f.valign = v;
                        }
                    }
                    dropEmptyFormats(*b);
                }
                if (did.empty()) return Result::failure("nothing to change: give text=, fill=, align= or valign=");
                std::string list;
                for (const auto& x : did) list += (list.empty() ? "" : ", ") + x;
                const std::string where = r1 == r2 && c1 == c2 ? cellName(r1, c1) : "rows " + std::to_string(r1 + 1) + "-" + std::to_string(r2 + 1) + ", columns " +
                                                                                       std::to_string(c1 + 1) + "-" + std::to_string(c2 + 1);
                Result res = Result::success("table " + std::to_string(bi + 1) + " " + where + ": " + list);
                maybePreview(res, d, p);
                return res;
            }));

    reg.add({"office.doc_table_merge", "Merge Cells",
             "Merge a block of table cells into one (rows x cols from row/col; their text joins in the top-left cell), or unmerge=true "
             "to split the merged cell at row/col again.",
             {doc(), integer("block", "the table's block number", true), integer("row", "top row (1-based)", true), integer("col", "left column (1-based)", true),
              integer("rows", "how many rows", false, std::int64_t{1}), integer("cols", "how many columns", false, std::int64_t{1}), flag("unmerge", "split instead"),
              previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                Block* b;
                std::size_t bi, r, c;
                std::string error;
                if (!tableArg(d.data, p, b, bi, error)) return Result::failure(error);
                const std::size_t rows = b->rows.size(), cols = columnCount(*b);
                if (!cellArg(*b, p, "row", rows, r, error) || !cellArg(*b, p, "col", cols, c, error)) return Result::failure(error);
                if (boolParam(p, "unmerge", false)) {
                    for (auto it = b->merges.begin(); it != b->merges.end(); ++it) {
                        const CellSpan m = *it;
                        if (static_cast<int>(r) >= m.row && static_cast<int>(r) < m.row + m.rows && static_cast<int>(c) >= m.col && static_cast<int>(c) < m.col + m.cols) {
                            b->merges.erase(it);
                            Result res = Result::success("split the merged cell at " + cellName(static_cast<std::size_t>(m.row), static_cast<std::size_t>(m.col)) + " (" +
                                                         std::to_string(m.rows) + "x" + std::to_string(m.cols) + ")");
                            maybePreview(res, d, p);
                            return res;
                        }
                    }
                    return Result::failure(cellName(r, c) + " is not merged");
                }
                const auto nr = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "rows", 1)));
                const auto nc = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "cols", 1)));
                if (r + nr > rows || c + nc > cols) return Result::failure("the merge runs past the table (" + std::to_string(rows) + "x" + std::to_string(cols) + ")");
                if (nr == 1 && nc == 1) return Result::failure("give rows= and/or cols= so more than one cell is merged");
                // spans that overlap the new one are replaced by it
                std::vector<CellSpan> kept;
                for (const auto& m : b->merges) {
                    const bool overlap = m.row < static_cast<int>(r + nr) && m.row + m.rows > static_cast<int>(r) && m.col < static_cast<int>(c + nc) && m.col + m.cols > static_cast<int>(c);
                    if (!overlap) kept.push_back(m);
                }
                b->merges = kept;
                Runs& anchor = b->rows[r][c];
                for (std::size_t i = r; i < r + nr; ++i) {
                    for (std::size_t j = c; j < c + nc; ++j) {
                        if (i == r && j == c) continue;
                        Runs& other = b->rows[i][j];
                        if (!trimCopy(plainText(other)).empty() || std::any_of(other.begin(), other.end(), [](const Run& x) { return !x.footnote.empty(); })) {
                            if (!anchor.empty()) {
                                Run nl;
                                nl.text = "\n";
                                anchor.push_back(nl);
                            }
                            anchor.insert(anchor.end(), other.begin(), other.end());
                        }
                        other.clear();
                    }
                }
                mergeRuns(anchor);
                b->merges.push_back({static_cast<int>(r), static_cast<int>(c), static_cast<int>(nr), static_cast<int>(nc)});
                Result res = Result::success("merged " + std::to_string(nr) + "x" + std::to_string(nc) + " cells at " + cellName(r, c) + " in table " + std::to_string(bi + 1));
                maybePreview(res, d, p);
                return res;
            }));

    reg.add({"office.doc_table_insert", "Insert Rows or Columns",
             "Insert rows (row= with where=above|below) or columns (col= with where=left|right) into a table; count= adds several. text= "
             "fills a new row (cells split by |) or column (one cell per line).",
             {doc(), integer("block", "the table's block number", true), integer("row", "insert next to this row"), integer("col", "insert next to this column"),
              str("where", "which side (default below / right)", false, {}, {"above", "below", "left", "right"}), integer("count", "how many", false, std::int64_t{1}),
              str("text", "content of the new row or column"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                Block* b;
                std::size_t bi, n;
                std::string error;
                if (!tableArg(d.data, p, b, bi, error)) return Result::failure(error);
                const bool rowMode = has(p, "row");
                if (rowMode == has(p, "col")) return Result::failure("give row= or col= (not both)");
                const std::size_t rows = b->rows.size(), cols = columnCount(*b);
                if (!cellArg(*b, p, rowMode ? "row" : "col", rowMode ? rows : cols, n, error)) return Result::failure(error);
                const std::string where = lower(stringParam(p, "where", rowMode ? "below" : "right"));
                const bool before = where == "above" || where == "left";
                const auto count = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, 200));
                const std::size_t at = before ? n : n + 1;
                std::vector<std::string> cells;
                if (has(p, "text")) {
                    const std::string t = textParam(p, "text");
                    if (rowMode) {
                        std::string cur;
                        for (std::size_t i = 0; i <= t.size(); ++i) {
                            if (i == t.size() || t[i] == '|') {
                                cells.push_back(trimCopy(cur));
                                cur.clear();
                            } else {
                                cur += t[i];
                            }
                        }
                        const std::string tt = trimCopy(t);
                        if (!tt.empty() && tt.front() == '|' && !cells.empty() && cells.front().empty()) cells.erase(cells.begin());
                        if (!tt.empty() && tt.back() == '|' && !cells.empty() && cells.back().empty()) cells.pop_back();
                    } else {
                        cells = splitLines(t);
                    }
                }
                const bool track = d.data.trackChanges;
                const int ins = track && !cells.empty() ? newChange(d.data, true) : 0;
                auto cellAt = [&](std::size_t k) {
                    Runs r = k < cells.size() ? cellRuns(cells[k]) : Runs{};
                    if (ins) markRuns(d.data, r, ins);
                    return r;
                };
                const bool hadFormats = !b->cellFormats.empty();
                if (hadFormats) fitFormats(*b);
                if (rowMode) {
                    for (std::size_t k = 0; k < count; ++k) {
                        std::vector<Runs> row(cols);
                        for (std::size_t c = 0; c < cols; ++c) row[c] = cellAt(c);
                        b->rows.insert(b->rows.begin() + static_cast<std::ptrdiff_t>(at + k), row);
                        if (hadFormats) b->cellFormats.insert(b->cellFormats.begin() + static_cast<std::ptrdiff_t>(at + k), std::vector<CellFormat>(cols));
                    }
                } else {
                    for (std::size_t r = 0; r < b->rows.size(); ++r) {
                        for (std::size_t k = 0; k < count; ++k) {
                            b->rows[r].insert(b->rows[r].begin() + static_cast<std::ptrdiff_t>(at + k), cellAt(r));
                            if (hadFormats) b->cellFormats[r].insert(b->cellFormats[r].begin() + static_cast<std::ptrdiff_t>(at + k), CellFormat{});
                        }
                    }
                    if (!b->colWidths.empty()) {
                        const double w = b->colWidths[std::min(n, b->colWidths.size() - 1)];
                        b->colWidths.insert(b->colWidths.begin() + static_cast<std::ptrdiff_t>(std::min(at, b->colWidths.size())), count, w);
                    }
                }
                shiftMerges(*b, rowMode, static_cast<int>(at), static_cast<int>(count));
                const std::string what = std::to_string(count) + (rowMode ? " row(s) " : " column(s) ") + where + " " + (rowMode ? "row " : "column ") + std::to_string(n + 1);
                Result res = Result::success("inserted " + what + " in table " + std::to_string(bi + 1) + " (now " + std::to_string(b->rows.size()) + "x" +
                                             std::to_string(columnCount(*b)) + ")");
                maybePreview(res, d, p);
                return res;
            }));

    reg.add({"office.doc_table_delete", "Delete Rows or Columns",
             "Delete table rows (row=) or columns (col=), count= of them. A table keeps at least one row and column (office.doc_delete "
             "removes the whole table).",
             {doc(), integer("block", "the table's block number", true), integer("row", "first row to delete"), integer("col", "first column to delete"),
              integer("count", "how many", false, std::int64_t{1}), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                Block* b;
                std::size_t bi, n;
                std::string error;
                if (!tableArg(d.data, p, b, bi, error)) return Result::failure(error);
                const bool rowMode = has(p, "row");
                if (rowMode == has(p, "col")) return Result::failure("give row= or col= (not both)");
                const std::size_t rows = b->rows.size(), cols = columnCount(*b);
                const std::size_t limit = rowMode ? rows : cols;
                if (!cellArg(*b, p, rowMode ? "row" : "col", limit, n, error)) return Result::failure(error);
                const auto count = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, static_cast<std::int64_t>(limit - n)));
                if (count >= limit) return Result::failure(std::string("a table needs at least one ") + (rowMode ? "row" : "column") + " (office.doc_delete removes the table)");
                const bool hadFormats = !b->cellFormats.empty();
                if (hadFormats) fitFormats(*b);
                const auto from = static_cast<std::ptrdiff_t>(n), to = static_cast<std::ptrdiff_t>(n + count);
                if (rowMode) {
                    b->rows.erase(b->rows.begin() + from, b->rows.begin() + to);
                    if (hadFormats) b->cellFormats.erase(b->cellFormats.begin() + from, b->cellFormats.begin() + to);
                } else {
                    for (auto& r : b->rows) r.erase(r.begin() + from, r.begin() + std::min<std::ptrdiff_t>(to, static_cast<std::ptrdiff_t>(r.size())));
                    if (hadFormats) {
                        for (auto& r : b->cellFormats) r.erase(r.begin() + from, r.begin() + to);
                    }
                    if (b->colWidths.size() >= n + count) b->colWidths.erase(b->colWidths.begin() + from, b->colWidths.begin() + to);
                }
                shiftMerges(*b, rowMode, static_cast<int>(n), -static_cast<int>(count));
                if (hadFormats) dropEmptyFormats(*b);
                Result res = Result::success("deleted " + std::to_string(count) + (rowMode ? " row(s) from row " : " column(s) from column ") + std::to_string(n + 1) +
                                             " of table " + std::to_string(bi + 1) + " (now " + std::to_string(b->rows.size()) + "x" + std::to_string(columnCount(*b)) + ")");
                maybePreview(res, d, p);
                return res;
            }));

    reg.add({"office.doc_table_format", "Table Format",
             "Table-wide settings: header row, grid lines (borders), banded rows, and column widths as relative numbers (widths=2,1,1) or "
             "widths=auto to fit the contents.",
             {doc(), integer("block", "the table's block number", true), flag("header", "the first row is a header"), flag("borders", "grid lines"),
              flag("banded", "shade every other row"), str("widths", "relative column widths, e.g. 3,1,1 (auto = fit the contents)"), previewSpec()},
             true},
            edit(session, [](TextDoc& d, const Params& p, const std::string&) -> Result {
                Block* b;
                std::size_t bi;
                std::string error;
                if (!tableArg(d.data, p, b, bi, error)) return Result::failure(error);
                std::vector<std::string> did;
                if (has(p, "header")) {
                    b->header = boolParam(p, "header");
                    did.push_back(b->header ? "header row" : "no header row");
                }
                if (has(p, "borders")) {
                    b->borders = boolParam(p, "borders");
                    did.push_back(b->borders ? "borders" : "no borders");
                }
                if (has(p, "banded")) {
                    b->banded = boolParam(p, "banded");
                    did.push_back(b->banded ? "banded rows" : "plain rows");
                }
                if (has(p, "widths")) {
                    const std::string w = lower(trimCopy(stringParam(p, "widths")));
                    if (w == "auto" || w.empty()) {
                        b->colWidths.clear();
                        did.push_back("widths fit the contents");
                    } else {
                        std::vector<double> ws;
                        std::string cur;
                        for (std::size_t i = 0; i <= w.size(); ++i) {
                            if (i == w.size() || w[i] == ',' || w[i] == ' ' || w[i] == ';') {
                                if (!trimCopy(cur).empty()) ws.push_back(std::atof(cur.c_str()));
                                cur.clear();
                            } else {
                                cur += w[i];
                            }
                        }
                        const std::size_t cols = columnCount(*b);
                        if (ws.size() != cols || std::any_of(ws.begin(), ws.end(), [](double x) { return !(x > 0); })) {
                            return Result::failure("widths needs " + std::to_string(cols) + " positive numbers (one per column), e.g. widths=" +
                                                   (cols == 3 ? std::string("2,1,1") : std::string("1,...")));
                        }
                        b->colWidths = ws;
                        did.push_back("widths " + w);
                    }
                }
                if (did.empty()) return Result::failure("nothing to change: give header=, borders=, banded= or widths=");
                std::string list;
                for (const auto& x : did) list += (list.empty() ? "" : ", ") + x;
                Result res = Result::success("table " + std::to_string(bi + 1) + ": " + list);
                maybePreview(res, d, p);
                return res;
            }));
}

}  // namespace wetwaresuite::office::doccmd
