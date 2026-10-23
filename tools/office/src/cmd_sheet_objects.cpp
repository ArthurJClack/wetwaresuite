// Things that sit on a sheet: charts placed over cells (saved as real .xlsx
// charts) and pivot tables built from live SUMIFS/COUNTIFS formulas.
#include <algorithm>
#include <map>
#include <set>

#include "wetwaresuite/office/chart.hpp"
#include "sheet_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office::sheetcmd {

namespace {

std::string chartText(const SheetChart& c) {
    return c.type + " chart of " + c.range + (c.byRows ? " (series in rows)" : "") + (c.title.empty() ? "" : " \"" + c.title + "\"") + " at " + cellName(c.row, c.col) +
           ", " + std::to_string(c.width) + "x" + std::to_string(c.height) + "px";
}

// Fills chart fields from the params that are present; checks that the data can be charted.
bool chartParams(const Workbook& wb, int sheet, const Params& p, SheetChart& c, std::string& error) {
    if (has(p, "type")) c.type = stringParam(p, "type");
    if (has(p, "title")) c.title = stringParam(p, "title");
    if (has(p, "by_rows")) c.byRows = boolParam(p, "by_rows");
    if (has(p, "range")) {
        Range r;
        if (!parseRange(wb, stringParam(p, "range"), sheet, r, error)) return false;
        const std::string name = rangeName(r);
        c.range = r.sheet == sheet ? name : quoteSheet(wb.sheets[static_cast<std::size_t>(r.sheet)].name) + "!" + name;
    }
    if (has(p, "at")) {
        Range at;
        if (!parseRange(wb, stringParam(p, "at"), sheet, at, error)) return false;
        c.row = at.r1;
        c.col = at.c1;
    }
    if (has(p, "width")) c.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width"), 120, 4000));
    if (has(p, "height")) c.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height"), 90, 4000));
    try {
        sheetChart(wb, sheet, c);
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

Result withChart(Result r, const Workbook& wb, int sheet, const SheetChart& c) {
    try {
        r.attach(png(renderChart(sheetChart(wb, sheet, c)), "chart.png"));
    } catch (const std::exception&) {
    }
    return r;
}

bool chartIndex(const Sheet& sh, const Params& p, std::size_t& i, std::string& error) {
    const auto n = intParam(p, "chart", static_cast<std::int64_t>(sh.charts.size()));
    if (sh.charts.empty()) {
        error = sh.name + " has no charts";
        return false;
    }
    if (n < 1 || n > static_cast<std::int64_t>(sh.charts.size())) {
        error = "no chart " + std::to_string(n) + " (there are " + std::to_string(sh.charts.size()) + ")";
        return false;
    }
    i = static_cast<std::size_t>(n - 1);
    return true;
}

void registerCharts(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_chart_add", "Insert Chart",
             "Put a chart on the sheet over the cells (it follows the data, is drawn in previews and PDFs and is saved as a real "
             "chart in .xlsx). The data range is read like office.sheet_chart: a header row of series names and a first column "
             "of categories. Types: bar, hbar, line, area, pie, scatter (first column = x values).",
             {doc(), str("range", "data including headers, e.g. A1:C13", true), str("type", "chart type", false, std::string("bar"), chartTypes()), str("title", "chart title"),
              flag("by_rows", "series are rows, not columns", false), str("at", "top-left cell the chart sits on (default: two columns right of the data)"),
              integer("width", "pixels", false, std::int64_t{480}), integer("height", "pixels", false, std::int64_t{288}), sheetSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                SheetChart c;
                Range r;
                if (!parseRange(d.data, stringParam(p, "range"), s, r, error)) return Result::failure(error);
                c.row = r.r1;
                c.col = std::min(r.c2 + 2, kMaxCols - 1);
                if (r.sheet != s) c.col = 1;
                if (!chartParams(d.data, s, p, c, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                sh.charts.push_back(c);
                const auto n = static_cast<std::int64_t>(sh.charts.size());
                return withChart(Result::success("chart " + std::to_string(n) + ": " + chartText(c), n).with("chart", n), d.data, s, c);
            }));

    reg.add({"office.sheet_chart_edit", "Edit Chart", "Change a chart on the sheet: its type, title, data range, position or size.",
             {doc(), integer("chart", "chart number (default: the last one)"), str("type", "chart type", false, {}, chartTypes()), str("title", "chart title"),
              str("range", "data range"), flag("by_rows", "series are rows"), str("at", "top-left cell"), integer("width", "pixels"), integer("height", "pixels"), sheetSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                std::size_t i;
                if (!chartIndex(sh, p, i, error)) return Result::failure(error);
                SheetChart c = sh.charts[i];
                if (!chartParams(d.data, s, p, c, error)) return Result::failure(error);
                sh.charts[i] = c;
                return withChart(Result::success("chart " + std::to_string(i + 1) + ": " + chartText(c)), d.data, s, c);
            }));

    reg.add({"office.sheet_chart_delete", "Delete Chart", "Remove a chart from the sheet.",
             {doc(), integer("chart", "chart number (default: the last one)"), sheetSpec()}, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                std::size_t i;
                if (!chartIndex(sh, p, i, error)) return Result::failure(error);
                const std::string what = chartText(sh.charts[i]);
                sh.charts.erase(sh.charts.begin() + static_cast<std::ptrdiff_t>(i));
                return Result::success("deleted chart " + std::to_string(i + 1) + " (" + what + ")");
            }));

    reg.add({"office.sheet_charts", "Charts on Sheet", "List the charts placed on a sheet. chart=N attaches that chart as a PNG.",
             {doc(), fileSpec("spreadsheet"), integer("chart", "attach this chart's picture"), sheetSpec()}, false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                const Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                if (sh.charts.empty()) return Result::success(sh.name + " has no charts (office.sheet_chart_add puts one there)", std::int64_t{0});
                std::string t = sh.name + ": " + std::to_string(sh.charts.size()) + " chart(s)";
                for (std::size_t i = 0; i < sh.charts.size(); ++i) t += "\n  " + std::to_string(i + 1) + ". " + chartText(sh.charts[i]);
                Result r = Result::success(t, static_cast<std::int64_t>(sh.charts.size())).with("count", static_cast<std::int64_t>(sh.charts.size()));
                if (has(p, "chart")) {
                    std::size_t i;
                    if (!chartIndex(sh, p, i, error)) return Result::failure(error);
                    return withChart(r, d.data, s, sh.charts[i]);
                }
                return r;
            }));
}

std::string absRef(const std::string& sheet, int r1, int c1, int r2, int c2) {
    return quoteSheet(sheet) + "!$" + colName(c1) + "$" + std::to_string(r1 + 1) + ":$" + colName(c2) + "$" + std::to_string(r2 + 1);
}

void registerPivot(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_pivot", "Pivot Table",
             "Summarise a table by category, like a pivot table: rows=Region values=Sales sums Sales per region; add columns=Quarter "
             "for a cross-tab; function=count|average|min|max. The cells are live SUMIFS/COUNTIFS/AVERAGEIFS formulas, so the "
             "summary updates when the data changes (static=true writes plain values). Totals are added. Goes to a new sheet "
             "unless to= names a cell.",
             {doc(), str("range", "the table including its header row (default: the used area)"), str("rows", "field for the row labels (header name or column letter)", true),
              str("columns", "field for the column labels (optional)"), str("values", "field to summarise (count needs none)"),
              str("function", "how to summarise", false, std::string("sum"), {"sum", "count", "average", "min", "max"}),
              str("to", "top-left cell for the result, e.g. H1 or Summary!A1 (default: a new sheet)"), flag("static", "write values instead of formulas", false),
              flag("totals", "add grand totals", true), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                if (r.rows() < 2) return Result::failure("the table needs a header row and data");
                const std::string srcName = d.data.sheets[static_cast<std::size_t>(r.sheet)].name;
                int rowField, colField = -1, valField = -1;
                if (!columnArg(d.data, r, stringParam(p, "rows"), true, rowField, error)) return Result::failure(error);
                if (has(p, "columns") && !columnArg(d.data, r, stringParam(p, "columns"), true, colField, error)) return Result::failure(error);
                const std::string fn = stringParam(p, "function", "sum");
                if (has(p, "values") && !columnArg(d.data, r, stringParam(p, "values"), true, valField, error)) return Result::failure(error);
                if (valField < 0 && fn != "count") return Result::failure("give values= (the field to " + fn + ")");
                const Sheet& src = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                auto header = [&](int col) { return display(src.get(r.r1, col)); };
                // distinct labels in order
                auto labels = [&](int col) {
                    std::vector<CellValue> out;
                    std::set<std::string> seen;
                    for (int row = r.r1 + 1; row <= r.r2; ++row) {
                        const Cell* c = src.get(row, col);
                        if (!c || c->value.empty()) continue;
                        const std::string key = std::to_string(c->value.type) + lower(formatValue(c->value, ""));
                        if (seen.insert(key).second) out.push_back(c->value);
                    }
                    std::stable_sort(out.begin(), out.end(), [](const CellValue& a, const CellValue& b) { return compareForSort(a, b) < 0; });
                    return out;
                };
                const auto rowLabels = labels(rowField);
                const auto colLabels = colField >= 0 ? labels(colField) : std::vector<CellValue>{};
                const std::string rowHeader = header(rowField), colHeader = colField >= 0 ? header(colField) : std::string(),
                                  valHeader = valField >= 0 ? header(valField) : rowHeader;
                if (rowLabels.empty()) return Result::failure("no values in the " + rowHeader + " column");
                if (rowLabels.size() > 5000 || colLabels.size() > 500) return Result::failure("too many distinct labels for a pivot table");
                // destination
                int dest = -1, top = 0, left = 0;
                std::string newSheet;
                if (has(p, "to")) {
                    Range to;
                    if (!parseRange(d.data, stringParam(p, "to"), r.sheet, to, error)) return Result::failure(error);
                    dest = to.sheet, top = to.r1, left = to.c1;
                } else {
                    newSheet = freshSheetName(d.data, "Pivot");
                    d.data.sheets.push_back(Sheet{newSheet, {}, {}});
                    dest = static_cast<int>(d.data.sheets.size()) - 1;
                }
                const bool totals = boolParam(p, "totals", true);
                const int width = 1 + (colField >= 0 ? static_cast<int>(colLabels.size()) : 1) + (colField >= 0 && totals ? 1 : 0);
                const int height = 1 + static_cast<int>(rowLabels.size()) + (totals ? 1 : 0);
                const Range area{dest, top, left, top + height - 1, left + width - 1};
                if (dest == r.sheet && area.overlaps(r)) return Result::failure("the pivot table would overwrite its own data; pick another to=");
                Sheet& out = d.data.sheets[static_cast<std::size_t>(dest)];
                const Sheet& srcSheet = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                const int d1 = r.r1 + 1, d2 = r.r2;
                const std::string rowsRef = absRef(srcName, d1, rowField, d2, rowField);
                const std::string colsRef = colField >= 0 ? absRef(srcName, d1, colField, d2, colField) : std::string();
                const std::string valsRef = valField >= 0 ? absRef(srcName, d1, valField, d2, valField) : std::string();
                std::string valueFormat;
                if (valField >= 0) {
                    if (const Cell* c = srcSheet.get(d1, valField)) valueFormat = c->format;
                }
                if (fn == "count") valueFormat.clear();
                auto formula = [&](const std::string& rowCrit, const std::string& colCrit) {
                    std::string crit;
                    if (!rowCrit.empty()) crit += "," + rowsRef + "," + rowCrit;
                    if (!colCrit.empty()) crit += "," + colsRef + "," + colCrit;
                    if (crit.empty()) {
                        if (fn == "count") return "COUNTA(" + rowsRef + ")";
                        static const std::map<std::string, std::string> plain = {{"sum", "SUM"}, {"average", "AVERAGE"}, {"min", "MIN"}, {"max", "MAX"}};
                        return plain.at(fn) + "(" + valsRef + ")";
                    }
                    if (fn == "count") return "COUNTIFS" + ("(" + crit.substr(1)) + ")";
                    if (fn == "sum") return "SUMIFS(" + valsRef + crit + ")";
                    if (fn == "average") return "IFERROR(AVERAGEIFS(" + valsRef + crit + "),\"\")";
                    return std::string(fn == "min" ? "MINIFS(" : "MAXIFS(") + valsRef + crit + ")";
                };
                auto put = [&](int row, int col, const std::string& input, bool bold, const std::string& format) {
                    Cell& c = out.at(row, col);
                    c = Cell{};
                    setInput(c, input);
                    c.style.bold = bold;
                    c.format = format;
                };
                auto putValue = [&](int row, int col, const CellValue& v, bool bold) {
                    Cell& c = out.at(row, col);
                    c = Cell{};
                    c.value = v;
                    c.style.bold = bold;
                };
                static const std::map<std::string, std::string> names = {{"sum", "Sum"}, {"count", "Count"}, {"average", "Average"}, {"min", "Min"}, {"max", "Max"}};
                const std::string what = names.at(fn) + " of " + valHeader;
                // header row
                putValue(top, left, CellValue::str(colField >= 0 ? rowHeader + " / " + colHeader : rowHeader), true);
                if (colField >= 0) {
                    for (std::size_t j = 0; j < colLabels.size(); ++j) putValue(top, left + 1 + static_cast<int>(j), colLabels[j], true);
                    if (totals) put(top, left + width - 1, "'Total", true, "");
                } else {
                    put(top, left + 1, "'" + what, true, "");
                }
                const std::string rowAnchor = "$" + colName(left);
                for (std::size_t i = 0; i < rowLabels.size(); ++i) {
                    const int row = top + 1 + static_cast<int>(i);
                    putValue(row, left, rowLabels[i], false);
                    const std::string rowCrit = rowAnchor + std::to_string(row + 1);
                    if (colField >= 0) {
                        for (std::size_t j = 0; j < colLabels.size(); ++j) {
                            const int col = left + 1 + static_cast<int>(j);
                            put(row, col, "=" + formula(rowCrit, colName(col) + "$" + std::to_string(top + 1)), false, valueFormat);
                        }
                        if (totals) put(row, left + width - 1, "=" + formula(rowCrit, ""), true, valueFormat);
                    } else {
                        put(row, left + 1, "=" + formula(rowCrit, ""), false, valueFormat);
                    }
                }
                if (totals) {
                    const int row = top + height - 1;
                    put(row, left, "'Total", true, "");
                    if (colField >= 0) {
                        for (std::size_t j = 0; j < colLabels.size(); ++j) {
                            const int col = left + 1 + static_cast<int>(j);
                            put(row, col, "=" + formula("", colName(col) + "$" + std::to_string(top + 1)), true, valueFormat);
                        }
                    }
                    put(row, left + width - 1, "=" + formula("", ""), true, valueFormat);
                }
                std::string note = recalcNote(d.data);
                if (boolParam(p, "static", false)) {
                    for (int row = area.r1; row <= area.r2; ++row) {
                        for (int col = area.c1; col <= area.c2; ++col) {
                            if (Cell* c = out.cells.count(Sheet::key(row, col)) ? &out.at(row, col) : nullptr) {
                                c->formula.clear();
                                c->ast.reset();
                            }
                        }
                    }
                }
                if (dest != r.sheet || !newSheet.empty()) d.data.active = dest;
                autofitColumns(out, area.c1, area.c2, true);
                std::string msg = what + " by " + rowHeader + (colField >= 0 ? " and " + colHeader : "") + ": " + std::to_string(rowLabels.size()) + " row(s)" +
                                  (colField >= 0 ? " x " + std::to_string(colLabels.size()) + " column(s)" : "") + " in " +
                                  (newSheet.empty() ? where(d.data, area) : "the new sheet " + newSheet + " (" + rangeName(area) + ")") + note;
                return withPreview(Result::success(msg).with("range", where(d.data, area)).with("sheet", d.data.sheets[static_cast<std::size_t>(dest)].name), d, p);
            }));
}

}  // namespace

void registerSheetObjectCommands(Session& session) {
    registerCharts(session);
    registerPivot(session);
}

}  // namespace wetwaresuite::office::sheetcmd
