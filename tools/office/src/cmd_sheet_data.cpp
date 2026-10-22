// Moving data around: office.sheet_copy (with paste special), sheet_move (cut
// and paste that keeps references), sheet_autofilter (hide rows in place) and
// sheet_import (CSV with import options).
#include <algorithm>

#include "sheet_cmd.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office::sheetcmd {

std::vector<ParamSpec> csvSpecs() {
    return {str("delimiter", "CSV delimiter", false, std::string("auto"), {"auto", "comma", "semicolon", "tab", "pipe", "space"}),
            integer("skip_rows", "lines to skip at the top (titles above the table)"),
            flag("decimal_comma", "numbers use a decimal comma: 1.234,5 = 1234.5"),
            str("date_order", "how 03/04/2026 reads", false, std::string("auto"), {"auto", "dmy", "mdy"}),
            str("text_columns", "columns kept as text (ids with leading zeros), e.g. A,C"),
            flag("detect_types", "turn numbers, dates and TRUE/FALSE into values (false keeps every field as text)"),
            str("encoding", "text encoding", false, std::string("auto"), {"auto", "utf-8", "latin1"})};
}

bool csvOptions(const Params& p, CsvOptions& o, std::string& error) {
    const std::string dl = stringParam(p, "delimiter", "auto");
    o.delimiter = dl == "comma" ? ',' : dl == "semicolon" ? ';' : dl == "tab" ? '\t' : dl == "pipe" ? '|' : dl == "space" ? ' ' : 0;
    if (has(p, "skip_rows")) o.skipRows = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "skip_rows"), 0, 1000000));
    o.decimalComma = boolParam(p, "decimal_comma", false);
    const std::string order = stringParam(p, "date_order", "auto");
    o.dateOrder = order == "auto" ? std::string() : order;
    o.detectTypes = boolParam(p, "detect_types", true);
    const std::string enc = stringParam(p, "encoding", "auto");
    o.encoding = enc == "auto" ? std::string() : enc;
    if (has(p, "text_columns")) {
        std::string item;
        for (char ch : stringParam(p, "text_columns") + ",") {
            if (ch != ',') {
                item += ch;
                continue;
            }
            const std::string t = upper(trimCopy(item));
            item.clear();
            if (t.empty()) continue;
            int c = std::all_of(t.begin(), t.end(), [](char x) { return std::isdigit(static_cast<unsigned char>(x)); }) ? std::atoi(t.c_str()) - 1 : colIndex(t);
            if (c < 0) {
                error = "bad column '" + t + "' in text_columns";
                return false;
            }
            o.textColumns.insert(c);
        }
    }
    return true;
}

namespace {

// Copies one cell the way paste special asks: everything, the value only, the formula/value without formats, or formats only.
void pasteCell(Cell& dst, const Cell& src, const std::string& what, int dr, int dc) {
    if (what == "formats") {
        dst.format = src.format;
        dst.style = src.style;
        return;
    }
    const std::string keepFormat = dst.format;
    const CellStyle keepStyle = dst.style;
    if (what == "values") {
        dst.formula.clear();
        dst.ast.reset();
        dst.value = src.value;
    } else {
        dst.value = src.value;
        dst.formula = src.hasFormula() ? offsetFormula(src.formula, dr, dc) : std::string();
        dst.ast.reset();
    }
    dst.spilled = false;
    if (what == "all" || (what == "values" && false)) {
        dst.format = src.format;
        dst.style = src.style;
    } else {
        dst.format = keepFormat.empty() && what == "values" ? src.format : keepFormat;
        dst.style = keepStyle;
    }
}

void registerCopyMove(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_copy", "Copy Cells",
             "Copy a range to another place (same or another sheet) like copy and paste: relative references in formulas move "
             "with the cells, absolute ($A$1) ones stay. what=values pastes the computed values only, formulas pastes without "
             "formats, formats pastes only the look (format painter for a block). transpose=true turns rows into columns; "
             "skip_blanks=true leaves cells under empty source cells alone.",
             {doc(), str("from", "cells to copy, e.g. A1:D10", true), str("to", "top-left cell of the destination, e.g. F1 or Sheet2!A1", true),
              str("what", "what to paste", false, std::string("all"), {"all", "values", "formulas", "formats"}), flag("transpose", "swap rows and columns", false),
              flag("skip_blanks", "don't overwrite with empty cells", false), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range from, to;
                std::string error;
                if (!rangeArg(d.data, p, from, error, true, "from")) return Result::failure(error);
                if (!rangeArg(d.data, p, to, error, false, "to")) return Result::failure(error);
                if (from.rows() * static_cast<long long>(from.cols()) > 1000000) return Result::failure("that range is too large to copy");
                const bool transpose = boolParam(p, "transpose", false), skip = boolParam(p, "skip_blanks", false);
                const std::string what = stringParam(p, "what", "all");
                const int h = transpose ? from.cols() : from.rows(), w = transpose ? from.rows() : from.cols();
                if (to.r1 + h > kMaxRows || to.c1 + w > kMaxCols) return Result::failure("the copy would run off the sheet");
                // take a snapshot first so overlapping areas copy cleanly
                std::vector<std::pair<std::pair<int, int>, Cell>> cells;
                const Sheet& src = d.data.sheets[static_cast<std::size_t>(from.sheet)];
                for (int r = from.r1; r <= from.r2; ++r) {
                    for (int c = from.c1; c <= from.c2; ++c) {
                        const Cell* cell = src.get(r, c);
                        cells.push_back({{r, c}, cell ? *cell : Cell{}});
                    }
                }
                std::vector<Range> merges;
                for (const Range& m : src.merges) {
                    if (!transpose && from.contains(m.r1, m.c1) && from.contains(m.r2, m.c2)) merges.push_back(m);
                }
                Sheet& dst = d.data.sheets[static_cast<std::size_t>(to.sheet)];
                for (auto& [pos, cell] : cells) {
                    const int orow = pos.first - from.r1, ocol = pos.second - from.c1;
                    const int row = to.r1 + (transpose ? ocol : orow), col = to.c1 + (transpose ? orow : ocol);
                    if (cell.blank() || (!cell.hasContent() && what != "formats" && what != "all")) {
                        if (skip) continue;
                        if (what == "formats") {
                            if (Cell* t = dst.cells.count(Sheet::key(row, col)) ? &dst.at(row, col) : nullptr) {
                                t->format.clear();
                                t->style = CellStyle{};
                                if (t->blank()) dst.erase(row, col);
                            }
                        } else if (what == "all") {
                            dst.erase(row, col);
                        } else if (Cell* t = dst.cells.count(Sheet::key(row, col)) ? &dst.at(row, col) : nullptr) {
                            t->value = {};
                            t->formula.clear();
                            t->ast.reset();
                            if (t->blank()) dst.erase(row, col);
                        }
                        continue;
                    }
                    if (skip && !cell.hasContent() && what != "formats") continue;
                    Cell& t = dst.at(row, col);
                    pasteCell(t, cell, what, row - pos.first, col - pos.second);
                    if (t.blank()) dst.erase(row, col);
                }
                if (what == "all" || what == "formats") {
                    for (Range m : merges) {
                        m.r1 += to.r1 - from.r1, m.r2 += to.r1 - from.r1, m.c1 += to.c1 - from.c1, m.c2 += to.c1 - from.c1;
                        dst.merges.erase(std::remove_if(dst.merges.begin(), dst.merges.end(), [&](const Range& o) { return o.overlaps(m); }), dst.merges.end());
                        dst.merges.push_back(m);
                    }
                }
                std::string note = recalcNote(d.data);
                const Range out{to.sheet, to.r1, to.c1, to.r1 + h - 1, to.c1 + w - 1};
                std::string notes;
                if (!checkValidation(d.data, to.sheet, out, error, notes)) return Result::failure(error);
                return withPreview(Result::success("copied " + where(d.data, from) + " to " + where(d.data, out) + (what != "all" ? " (" + what + ")" : "") +
                                                   (transpose ? ", transposed" : "") + note + notes)
                                       .with("range", where(d.data, out)),
                                   d, p);
            }));

    reg.add({"office.sheet_move", "Move Cells",
             "Cut and paste a range: the cells (with formats and merges) move to `to`, and every formula that referred to them "
             "follows, as in Excel. Whatever was at the destination is replaced.",
             {doc(), str("from", "cells to move, e.g. A1:D10", true), str("to", "new top-left cell, e.g. F1 or Sheet2!A1", true), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range from, to;
                std::string error;
                if (!rangeArg(d.data, p, from, error, true, "from")) return Result::failure(error);
                if (!rangeArg(d.data, p, to, error, false, "to")) return Result::failure(error);
                if (to.r1 + from.rows() > kMaxRows || to.c1 + from.cols() > kMaxCols) return Result::failure("the block would run off the sheet");
                d.data.moveRange(from, to.sheet, to.r1, to.c1);
                std::string note = recalcNote(d.data);
                const Range out{to.sheet, to.r1, to.c1, to.r1 + from.rows() - 1, to.c1 + from.cols() - 1};
                return withPreview(Result::success("moved " + where(d.data, from) + " to " + where(d.data, out) + "; references follow" + note).with("range", where(d.data, out)), d, p);
            }));
}

void registerAutoFilter(Session& session) {
    auto& reg = session.registry();
    reg.add({"office.sheet_autofilter", "AutoFilter",
             "Filter a table in place, like Excel's AutoFilter: rows whose column doesn't match the criteria are hidden (the "
             "data stays; SUBTOTAL and AGGREGATE skip hidden rows). Each call adds a condition on one column; clear=true "
             "shows every row again and removes the filter. Criteria work like COUNTIF: \">100\", \"North\", \"<>done\", \"a*\".",
             {doc(), str("range", "the table including its header row (default: the used area, or the current filter)"), str("column", "column letter or header name"),
              str("criteria", "condition the kept rows meet"), flag("clear", "show all rows and remove the filter", false), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(s)];
                Range r;
                if (has(p, "range")) {
                    if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                } else if (sh.hasAutoFilter) {
                    r = sh.autoFilter;
                    r.sheet = s;
                } else {
                    r = usedRange(d.data, s);
                }
                if (boolParam(p, "clear", false)) {
                    const Range f = sh.hasAutoFilter ? sh.autoFilter : r;
                    int shown = 0;
                    for (int row = f.r1 + 1; row <= f.r2; ++row) shown += static_cast<int>(sh.hiddenRows.erase(row));
                    sh.hasAutoFilter = false;
                    std::string note = recalcNote(d.data);
                    return withPreview(Result::success("removed the filter; " + std::to_string(shown) + " hidden row(s) shown again" + note), d, p);
                }
                if (!has(p, "column") || !has(p, "criteria")) return Result::failure("give column= and criteria= (or clear=true)");
                if (r.rows() < 2) return Result::failure("the table needs a header row and data");
                int col;
                if (!columnArg(d.data, r, stringParam(p, "column"), true, col, error)) return Result::failure(error);
                const std::string criteria = stringParam(p, "criteria");
                sh.hasAutoFilter = true;
                sh.autoFilter = Range{0, r.r1, r.c1, r.r2, r.c2};
                int visible = 0, total = 0;
                for (int row = r.r1 + 1; row <= r.r2; ++row) {
                    ++total;
                    const Cell* c = sh.get(row, col);
                    if (!matchesCriteria(c ? c->value : CellValue{}, criteria)) sh.hiddenRows.insert(row);
                    else if (!sh.hiddenRows.count(row)) ++visible;
                }
                std::string note = recalcNote(d.data);
                return withPreview(Result::success(std::to_string(visible) + " of " + std::to_string(total) + " rows shown (" + display(sh.get(r.r1, col)) + " " + criteria + ")" + note,
                                                   static_cast<std::int64_t>(visible))
                                       .with("visible", static_cast<std::int64_t>(visible)),
                                   d, p);
            }));
}

void registerImport(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> params = {doc(), str("path", "CSV/TSV/text file to import"), str("csv", "or the CSV text itself"),
                                     str("name", "import into a new sheet with this name (default: the file name)"),
                                     str("at", "or paste into an existing sheet starting at this cell, e.g. Sheet1!A20")};
    for (auto& s : csvSpecs()) params.push_back(s);
    params.push_back(previewSpec());
    reg.add({"office.sheet_import", "Import Text",
             "Import a CSV/TSV file (or text) into the open workbook with import options, like Excel's text import: delimiter, "
             "rows to skip, decimal comma, day-first or month-first dates, columns kept as text, encoding. Goes to a new sheet "
             "unless at= names a cell.",
             params, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                CsvOptions o;
                std::string error;
                if (!csvOptions(p, o, error)) return Result::failure(error);
                std::string text, label;
                if (has(p, "path")) {
                    text = readBytes(stringParam(p, "path"));
                    label = pathFromUtf8(stringParam(p, "path")).stem().string();
                    const std::string ext = extOf(stringParam(p, "path"));
                    if (!o.delimiter && (ext == "tsv" || ext == "tab")) o.delimiter = '\t';
                } else if (hasText(p, "csv")) {
                    text = textParam(p, "csv");
                    label = "Imported";
                } else {
                    return Result::failure("give path= or csv=");
                }
                Workbook parsed = readCsv(text, o);
                const Sheet& in = parsed.sheets[0];
                int r1, c1, r2, c2;
                if (!in.bounds(r1, c1, r2, c2)) return Result::failure("nothing to import");
                if (has(p, "at")) {
                    Range at;
                    if (!parseRange(d.data, stringParam(p, "at"), d.data.active, at, error)) return Result::failure(error);
                    Sheet& dst = d.data.sheets[static_cast<std::size_t>(at.sheet)];
                    for (const auto& [k, c] : in.cells) dst.cells[Sheet::key(at.r1 + Sheet::rowOf(k), at.c1 + Sheet::colOf(k))] = c;
                    std::string note = recalcNote(d.data);
                    const Range out{at.sheet, at.r1, at.c1, at.r1 + r2, at.c1 + c2};
                    return withPreview(Result::success("imported " + std::to_string(r2 + 1) + " rows x " + std::to_string(c2 + 1) + " columns into " + where(d.data, out) + note)
                                           .with("range", where(d.data, out)),
                                       d, p);
                }
                std::string name = has(p, "name") ? trimCopy(stringParam(p, "name")) : freshSheetName(d.data, label.empty() ? "Imported" : label);
                if (!validSheetName(d.data, name, -1, error)) return Result::failure(error);
                Sheet sh = in;
                sh.name = name;
                d.data.sheets.push_back(std::move(sh));
                d.data.active = static_cast<int>(d.data.sheets.size()) - 1;
                std::string note = recalcNote(d.data);
                return withPreview(Result::success("imported " + std::to_string(r2 + 1) + " rows x " + std::to_string(c2 + 1) + " columns into the new sheet " + name + note, name)
                                       .with("sheet", name),
                                   d, p);
            }));
}

}  // namespace

void registerSheetDataCommands(Session& session) {
    registerCopyMove(session);
    registerAutoFilter(session);
    registerImport(session);
}

}  // namespace wetwaresuite::office::sheetcmd
