// office.sheet_* commands: workbooks in the workspace, edited cell by cell or
// range by range, recalculated after every change.
#include <algorithm>
#include <cmath>
#include <set>

#include "wetwaresuite/office/chart.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "commands.hpp"
#include "sheet_internal.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

namespace {

using Edit = std::function<Result(SheetDoc&, const Params&, const std::string&)>;

ActionFactory edit(Session& s, Edit fn) { return docCommand<SheetDoc>(s, std::move(fn), true); }
ActionFactory read(Session& s, Edit fn) { return docCommand<SheetDoc>(s, std::move(fn), false); }

ParamSpec doc() { return docSpec("spreadsheet", "sheet1"); }
ParamSpec sheetSpec() { return str("sheet", "sheet name or 1-based number (default: the active sheet)"); }

Value valueOf(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number: return numValue(v.number);
        case CellValue::Bool: return v.number != 0;
        case CellValue::Text:
        case CellValue::Error: return v.text;
        default: return std::string();
    }
}

std::string typeName(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number: return "number";
        case CellValue::Text: return "text";
        case CellValue::Bool: return "boolean";
        case CellValue::Error: return "error";
        default: return "empty";
    }
}

bool sheetArg(const Workbook& wb, const Params& p, int& out, std::string& error, const char* key = "sheet") {
    if (!has(p, key)) {
        out = std::clamp(wb.active, 0, static_cast<int>(wb.sheets.size()) - 1);
        return true;
    }
    const std::string s = trimCopy(stringParam(p, key));
    int i = wb.sheetIndex(s);
    if (i < 0 && !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        i = std::atoi(s.c_str()) - 1;
        if (i >= static_cast<int>(wb.sheets.size())) i = -1;
    }
    if (i < 0) {
        std::string names;
        for (const auto& sh : wb.sheets) names += (names.empty() ? "" : ", ") + sh.name;
        error = "no sheet '" + s + "' (sheets: " + names + ")";
        return false;
    }
    out = i;
    return true;
}

// The used area of a sheet as a range (A1 when empty).
Range usedRange(const Workbook& wb, int sheet) {
    Range r;
    r.sheet = sheet;
    int r1, c1, r2, c2;
    if (wb.sheets[static_cast<std::size_t>(sheet)].bounds(r1, c1, r2, c2)) {
        r.r2 = r2;
        r.c2 = c2;
    }
    return r;
}

// range=... (with an optional sheet= default); missing means the used range. clip trims whole rows/columns to the used area.
bool rangeArg(const Workbook& wb, const Params& p, Range& r, std::string& error, bool clip, const char* key = "range") {
    int sheet;
    if (!sheetArg(wb, p, sheet, error)) return false;
    if (!has(p, key)) {
        r = usedRange(wb, sheet);
        return true;
    }
    if (!parseRange(wb, stringParam(p, key), sheet, r, error)) return false;
    if (clip) {
        Range u = usedRange(wb, r.sheet);
        r.r2 = std::max(r.r1, std::min(r.r2, std::max(u.r2, r.r1)));
        r.c2 = std::max(r.c1, std::min(r.c2, std::max(u.c2, r.c1)));
    }
    return true;
}

std::string where(const Workbook& wb, const Range& r) {
    return (wb.sheets.size() > 1 ? quoteSheet(wb.sheets[static_cast<std::size_t>(r.sheet)].name) + "!" : std::string()) + rangeName(r);
}

std::string recalcNote(Workbook& wb) {
    auto st = wb.recalc();
    std::string note;
    if (st.cycles) note += "; " + std::to_string(st.cycles) + " cell(s) in a circular reference (#CYCLE!)";
    if (st.errors) note += "; " + std::to_string(st.errors) + " formula(s) give errors";
    return note;
}

std::string display(const Cell* c) { return c ? formatValue(c->value, c->format) : std::string(); }

// A column given as a letter ("C"), a 1-based number within the range ("3") or a header name ("Sales").
bool columnArg(const Workbook& wb, const Range& r, const std::string& text, bool header, int& col, std::string& error) {
    std::string t = trimCopy(text);
    if (t.empty()) {
        error = "no column given";
        return false;
    }
    if (header) {
        const Sheet& sh = wb.sheets[static_cast<std::size_t>(r.sheet)];
        for (int c = r.c1; c <= r.c2; ++c) {
            const Cell* h = sh.get(r.r1, c);
            if (h && lower(display(h)) == lower(t)) {
                col = c;
                return true;
            }
        }
    }
    int letters = colIndex(upper(t));
    if (letters >= 0 && t.size() <= 3) {
        col = letters;
    } else if (std::all_of(t.begin(), t.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); })) {
        col = r.c1 + std::atoi(t.c_str()) - 1;
    } else {
        error = "no column '" + t + "' (use a letter like C" + std::string(header ? ", a header name" : "") + " or a number)";
        return false;
    }
    if (col < r.c1 || col > r.c2) {
        error = "column " + colName(col) + " is outside " + rangeName(r);
        return false;
    }
    return true;
}

// Does the first row look like a header (text over numbers)?
bool looksLikeHeader(const Sheet& sh, const Range& r) {
    if (r.rows() < 2) return false;
    bool allText = true, anyBelow = false;
    for (int c = r.c1; c <= r.c2; ++c) {
        const Cell* h = sh.get(r.r1, c);
        if (h && !h->value.empty() && h->value.type != CellValue::Text) allText = false;
        const Cell* b = sh.get(r.r1 + 1, c);
        if (b && b->value.type != CellValue::Text && !b->value.empty()) anyBelow = true;
        if (h && h->style.bold) return true;
    }
    return allText && anyBelow;
}

const std::map<std::string, std::string>& formatPresets() {
    static const std::map<std::string, std::string> m = {
        {"general", ""},           {"number", "#,##0.00"},  {"integer", "#,##0"},       {"currency", "$#,##0.00"},
        {"euro", "#,##0.00 \xE2\x82\xAC"}, {"pound", "\xC2\xA3#,##0.00"}, {"percent", "0%"}, {"percent2", "0.00%"},
        {"date", "yyyy-mm-dd"},    {"datetime", "yyyy-mm-dd hh:mm"}, {"time", "hh:mm"}, {"scientific", "0.00E+00"},
        {"text", "@"},             {"accounting", "#,##0.00;(#,##0.00)"},
    };
    return m;
}

// Writes CSV/TSV text into the sheet starting at (row, col); returns the area written.
Range pasteBlock(Workbook& wb, int sheet, int row, int col, const std::string& text) {
    char d = text.find('\t') != std::string::npos ? '\t' : sniffDelimiter(text);
    // parse as raw text so every cell goes through setInput (formulas, %, dates)
    Workbook parsed = readCsv(text, d);
    Range out{sheet, row, col, row, col};
    if (parsed.sheets.empty()) return out;
    Sheet& dst = wb.sheets[static_cast<std::size_t>(sheet)];
    const Sheet& src = parsed.sheets[0];
    int r1, c1, r2, c2;
    if (!src.bounds(r1, c1, r2, c2)) return out;
    for (int r = 0; r <= r2; ++r) {
        for (int c = 0; c <= c2; ++c) {
            const Cell* s = src.get(r, c);
            if (!s) {
                dst.erase(row + r, col + c);
                continue;
            }
            Cell& cell = dst.at(row + r, col + c);
            cell = *s;
        }
    }
    out.r2 = row + r2;
    out.c2 = col + c2;
    return out;
}

int sortClass(const CellValue& v) {
    switch (v.type) {
        case CellValue::Number: return 0;
        case CellValue::Text: return 1;
        case CellValue::Bool: return 2;
        case CellValue::Error: return 3;
        default: return 4;
    }
}

// < 0 when a sorts before b (ascending); blanks always last.
int compareForSort(const CellValue& a, const CellValue& b) {
    const int ca = sortClass(a), cb = sortClass(b);
    if (ca != cb) return ca < cb ? -1 : 1;
    if (a.type == CellValue::Number || a.type == CellValue::Bool) return a.number < b.number ? -1 : a.number > b.number ? 1 : 0;
    if (a.type == CellValue::Text) {
        const std::string x = lower(a.text), y = lower(b.text);
        return x < y ? -1 : x > y ? 1 : 0;
    }
    return 0;
}

std::string gridText(const Workbook& wb, const Range& r, const std::string& show, int maxRows, int maxCols, bool& cut) {
    const Sheet& sh = wb.sheets[static_cast<std::size_t>(r.sheet)];
    const int r2 = std::min(r.r2, r.r1 + maxRows - 1), c2 = std::min(r.c2, r.c1 + maxCols - 1);
    cut = r2 < r.r2 || c2 < r.c2;
    std::string out = "\t";
    for (int c = r.c1; c <= c2; ++c) out += colName(c) + (c < c2 ? "\t" : "");
    for (int row = r.r1; row <= r2; ++row) {
        out += "\n" + std::to_string(row + 1);
        for (int c = r.c1; c <= c2; ++c) {
            const Cell* cell = sh.get(row, c);
            std::string v;
            if (cell) {
                if (show == "formulas") v = inputText(*cell);
                else if (show == "both" && cell->hasFormula()) v = display(cell) + " [=" + cell->formula + "]";
                else v = display(cell);
            }
            for (auto& ch : v) {
                if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
            }
            out += "\t" + v;
        }
    }
    return out;
}

std::vector<std::string> dependentsOf(const Workbook& wb, int sheet, int row, int col) {
    std::vector<std::string> out;
    for (std::size_t s = 0; s < wb.sheets.size(); ++s) {
        for (const auto& [k, c] : wb.sheets[s].cells) {
            if (!c.hasFormula()) continue;
            for (const auto& ref : formulaReferences(wb, c.formula, static_cast<int>(s))) {
                if (ref.sheet == sheet && row >= ref.r1 && row <= ref.r2 && col >= ref.c1 && col <= ref.c2) {
                    std::string name = cellName(Sheet::rowOf(k), Sheet::colOf(k));
                    if (static_cast<int>(s) != sheet) name = quoteSheet(wb.sheets[s].name) + "!" + name;
                    out.push_back(name);
                    break;
                }
            }
            if (out.size() >= 50) return out;
        }
    }
    return out;
}

bool validSheetName(const Workbook& wb, const std::string& name, int except, std::string& error) {
    if (name.empty() || name.size() > 31) {
        error = "sheet names are 1 to 31 characters";
        return false;
    }
    if (name.find_first_of("[]:*?/\\") != std::string::npos) {
        error = "sheet names can't contain [ ] : * ? / \\";
        return false;
    }
    int i = wb.sheetIndex(name);
    if (i >= 0 && i != except) {
        error = "there is already a sheet called " + wb.sheets[static_cast<std::size_t>(i)].name;
        return false;
    }
    return true;
}

Result withPreview(Result r, const SheetDoc& d, const Params& p) {
    maybePreview(r, d, p);
    return r;
}

}  // namespace

void registerSheetCommands(Session& session) {
    auto& reg = session.registry();

    // ---- documents -------------------------------------------------------------------

    reg.add({"office.sheet_new", "New Spreadsheet",
             "Create an empty workbook and return its id (sheet1, sheet2, ...). Optionally fill the first sheet from CSV/TSV text "
             "(cells are typed: numbers, =formulas, 15%, $1,200, 2026-03-01 dates, TRUE/FALSE).",
             {str("name", "name of the first sheet", false, std::string("Sheet1")), integer("sheets", "how many sheets to start with", false, std::int64_t{1}),
              str("csv", "initial cells as CSV or tab-separated text, one row per line"), str("csv_file", "read the CSV/TSV text from this file instead")},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p]() -> Result {
                    auto d = std::make_shared<SheetDoc>();
                    const int n = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "sheets", 1), 1, 100));
                    std::string first = stringParam(p, "name", "Sheet1");
                    std::string error;
                    if (!validSheetName(d->data, first, -1, error)) return Result::failure(error);
                    d->data.sheets.push_back(Sheet{first, {}, {}});
                    for (int i = 2; i <= n; ++i) {
                        std::string name = "Sheet" + std::to_string(i);
                        while (d->data.sheetIndex(name) >= 0) name += "_";
                        d->data.sheets.push_back(Sheet{name, {}, {}});
                    }
                    if (hasText(p, "csv")) pasteBlock(d->data, 0, 0, 0, textParam(p, "csv"));
                    d->data.recalc();
                    return created(session, d, "created");
                });
            });

    reg.add({"office.sheet_open", "Open Spreadsheet",
             "Open a spreadsheet: .xlsx/.xlsm (values, formulas, number formats, bold, column widths, all sheets), .csv/.tsv "
             "(delimiter detected), .ods; .xls and other formats through LibreOffice. Formulas are recalculated on open.",
             {str("path", "file to open", true),
              str("delimiter", "CSV delimiter", false, std::string("auto"), {"auto", "comma", "semicolon", "tab", "pipe"})},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p]() -> Result {
                    try {
                        const std::string path = stringParam(p, "path");
                        const std::string dl = stringParam(p, "delimiter", "auto");
                        char delim = dl == "comma" ? ',' : dl == "semicolon" ? ';' : dl == "tab" ? '\t' : dl == "pipe" ? '|' : 0;
                        auto d = std::make_shared<SheetDoc>();
                        d->data = loadWorkbook(path, delim);
                        d->path = path;
                        auto st = d->data.recalc();
                        std::string extra;
                        if (st.formulas) extra = "; " + std::to_string(st.formulas) + " formulas";
                        if (st.errors) extra += " (" + std::to_string(st.errors) + " with errors)";
                        return created(session, d, "opened", extra);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"office.sheet_save", "Save Spreadsheet",
             "Save the workbook. The extension picks the format: .xlsx (all sheets, formulas with cached values, formats, bold, "
             "widths), .csv/.tsv (one sheet's computed values, no formulas), .pdf (printable grid), .html (tables); .ods/.xls through "
             "LibreOffice. Without path it saves back to where it was opened or last saved.",
             {doc(), str("path", "output file (default: the document's own path)"), sheetSpec()}, false, false},
            read(session, [](SheetDoc& d, const Params& p, const std::string& id) -> Result {
                std::string path = has(p, "path") ? stringParam(p, "path") : d.path;
                if (path.empty()) return Result::failure(id + " has never been saved; give path=<file.xlsx>");
                int sheet = -1;
                std::string error;
                if (has(p, "sheet") && !sheetArg(d.data, p, sheet, error)) return Result::failure(error);
                const std::string ext = extOf(path);
                saveWorkbook(d.data, path, sheet);
                std::string note;
                if (ext == "csv" || ext == "tsv" || ext == "txt") {
                    const int s = sheet >= 0 ? sheet : d.data.active;
                    if (d.data.sheets.size() > 1) note = " (sheet " + d.data.sheets[static_cast<std::size_t>(s)].name + " only; CSV holds one sheet)";
                    bool formulas = false;
                    for (const auto& [k, c] : d.data.sheets[static_cast<std::size_t>(s)].cells) formulas = formulas || c.hasFormula();
                    if (formulas) note += " (formulas saved as their values)";
                }
                if (ext != "pdf" && ext != "html" && ext != "htm") d.path = path;
                return Result::success("saved " + path + note, path).with("path", path);
            }));

    reg.add({"office.sheet_close", "Close Spreadsheet", "Close a workbook without saving (undo reopens it).", {doc()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                std::string id, error;
                if (!findDoc<SheetDoc>(session, p, id, error)) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"office.sheet_info", "Spreadsheet Info",
             "Describe a workbook: its sheets with used ranges, cell and formula counts, errors, the active sheet and file path.",
             {doc(), fileSpec("spreadsheet")}, false, true},
            read(session, [](SheetDoc& d, const Params&, const std::string& id) -> Result {
                std::string t = id + ": " + std::to_string(d.data.sheets.size()) + " sheet(s)" + (d.path.empty() || d.path == id ? "" : ", " + d.path);
                int totalFormulas = 0, totalErrors = 0;
                for (std::size_t s = 0; s < d.data.sheets.size(); ++s) {
                    const Sheet& sh = d.data.sheets[s];
                    int r1, c1, r2, c2, formulas = 0, errors = 0;
                    for (const auto& [k, c] : sh.cells) {
                        formulas += c.hasFormula() ? 1 : 0;
                        errors += c.value.isError() ? 1 : 0;
                    }
                    totalFormulas += formulas;
                    totalErrors += errors;
                    t += "\n  " + std::to_string(s + 1) + ". " + sh.name + (static_cast<int>(s) == d.data.active ? " (active)" : "") + ": ";
                    if (sh.bounds(r1, c1, r2, c2)) {
                        t += "A1:" + cellName(r2, c2) + " (" + std::to_string(r2 + 1) + " rows x " + std::to_string(c2 + 1) + " cols), " +
                             std::to_string(sh.cells.size()) + " cells, " + std::to_string(formulas) + " formulas";
                        if (errors) t += ", " + std::to_string(errors) + " errors";
                        // header preview
                        std::string head;
                        for (int c = 0; c <= std::min(c2, 9); ++c) head += (c ? " | " : "") + display(sh.get(0, c));
                        t += "\n     row 1: " + head + (c2 > 9 ? " | ..." : "");
                    } else {
                        t += "empty";
                    }
                }
                return Result::success(t)
                    .with("doc", id)
                    .with("sheets", static_cast<std::int64_t>(d.data.sheets.size()))
                    .with("active", d.data.sheets.empty() ? std::string() : d.data.sheets[static_cast<std::size_t>(d.data.active)].name)
                    .with("formulas", static_cast<std::int64_t>(totalFormulas))
                    .with("errors", static_cast<std::int64_t>(totalErrors))
                    .with("path", d.path);
            }));

    // ---- reading cells -----------------------------------------------------------------

    reg.add({"office.sheet_get", "Get Cells",
             "Read cells. One cell gives its value, type, formula, format and what it reads from / what reads it. A range gives a "
             "tab-separated grid with column letters and row numbers (show=formulas lists what to type to recreate each cell; "
             "show=both adds formulas next to values). Without range: the used area of the sheet. format=csv returns plain CSV.",
             {doc(), fileSpec("spreadsheet"), str("range", "cell or range: B2, A1:D20, C:C, 'My sheet'!A1:B5"), sheetSpec(),
              str("show", "what to show for each cell", false, std::string("values"), {"values", "formulas", "both"}),
              str("format", "grid (row numbers + letters, tab-separated) or csv", false, std::string("grid"), {"grid", "csv"}),
              integer("max_rows", "stop after this many rows", false, std::int64_t{200})},
             false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                const Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                if (r.rows() == 1 && r.cols() == 1 && has(p, "range")) {
                    const Cell* c = sh.get(r.r1, r.c1);
                    std::string name = where(d.data, r);
                    if (!c) return Result::success(name + " is empty", std::string()).with("cell", name).with("type", std::string("empty"));
                    std::string t = name + " = " + display(c);
                    if (c->hasFormula()) t += "  (formula =" + c->formula + ")";
                    t += "\ntype " + typeName(c->value) + (c->format.empty() ? "" : ", format " + c->format) + (c->style.bold ? ", bold" : "");
                    if (c->hasFormula()) {
                        std::string refs;
                        for (const auto& ref : formulaReferences(d.data, c->formula, r.sheet)) refs += (refs.empty() ? "" : ", ") + where(d.data, ref);
                        if (!refs.empty()) t += "\nreads " + refs;
                    }
                    auto deps = dependentsOf(d.data, r.sheet, r.r1, r.c1);
                    if (!deps.empty()) {
                        std::string list;
                        for (const auto& x : deps) list += (list.empty() ? "" : ", ") + x;
                        t += "\nused by " + list;
                    }
                    Result res = Result::success(t, valueOf(c->value));
                    res.with("cell", name).with("value", valueOf(c->value)).with("display", display(c)).with("type", typeName(c->value));
                    if (c->hasFormula()) res.with("formula", "=" + c->formula);
                    if (!c->format.empty()) res.with("format", c->format);
                    return res;
                }
                const int maxRows = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "max_rows", 200), 1, 100000));
                if (stringParam(p, "format", "grid") == "csv") {
                    Sheet part;
                    const int r2 = std::min(r.r2, r.r1 + maxRows - 1);
                    const std::string show = stringParam(p, "show", "values");
                    for (int row = r.r1; row <= r2; ++row) {
                        for (int col = r.c1; col <= r.c2; ++col) {
                            const Cell* c = sh.get(row, col);
                            if (!c) continue;
                            Cell& out = part.at(row - r.r1, col - r.c1);
                            if (show == "formulas") out.value = CellValue::str(inputText(*c));
                            else out = *c;
                        }
                    }
                    std::string csv;
                    for (char ch : writeCsv(part)) {
                        if (ch != '\r') csv += ch;
                    }
                    return Result::success(csv.empty() ? "(empty)" : csv, csv).with("range", where(d.data, r));
                }
                bool cut = false;
                std::string grid = gridText(d.data, r, stringParam(p, "show", "values"), maxRows, 60, cut);
                std::string head = where(d.data, r) + " (" + std::to_string(r.rows()) + " rows x " + std::to_string(r.cols()) + " cols)" +
                                   (cut ? ", showing the first " + std::to_string(std::min(r.rows(), maxRows)) + " rows / 60 columns" : "");
                return Result::success(head + "\n" + grid).with("range", where(d.data, r)).with("rows", static_cast<std::int64_t>(r.rows())).with(
                    "cols", static_cast<std::int64_t>(r.cols()));
            }));

    reg.add({"office.sheet_find", "Find in Spreadsheet",
             "Find cells whose displayed value (and optionally formula) contains the text. Lists up to 200 matches.",
             {doc(), fileSpec("spreadsheet"), str("query", "text to look for", true), str("sheet", "only this sheet (default: all sheets)"),
              flag("match_case", "case-sensitive", false), flag("whole_cell", "the whole cell must equal the query", false),
              flag("formulas", "also search formula text", false)},
             false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                const std::string q = stringParam(p, "query");
                const bool mc = boolParam(p, "match_case", false), whole = boolParam(p, "whole_cell", false), inFormulas = boolParam(p, "formulas", false);
                int only = -1;
                std::string error;
                if (has(p, "sheet") && !sheetArg(d.data, p, only, error)) return Result::failure(error);
                auto hit = [&](const std::string& text) {
                    std::string a = mc ? text : lower(text), b = mc ? q : lower(q);
                    return whole ? a == b : a.find(b) != std::string::npos;
                };
                std::string list;
                int count = 0;
                std::string first;
                for (std::size_t s = 0; s < d.data.sheets.size(); ++s) {
                    if (only >= 0 && static_cast<int>(s) != only) continue;
                    for (const auto& [k, c] : d.data.sheets[s].cells) {
                        const std::string shown = formatValue(c.value, c.format);
                        if (!(hit(shown) || (inFormulas && c.hasFormula() && hit("=" + c.formula)))) continue;
                        ++count;
                        std::string name = quoteSheet(d.data.sheets[s].name) + "!" + cellName(Sheet::rowOf(k), Sheet::colOf(k));
                        if (first.empty()) first = name;
                        if (count <= 200) list += "\n  " + name + ": " + shown + (c.hasFormula() ? "  (=" + c.formula + ")" : "");
                    }
                }
                if (!count) return Result::success("no cells match \"" + q + "\"", std::int64_t{0}).with("count", std::int64_t{0});
                Result r = Result::success(std::to_string(count) + " cell(s) match \"" + q + "\":" + list + (count > 200 ? "\n  ..." : ""),
                                           static_cast<std::int64_t>(count));
                return r.with("count", static_cast<std::int64_t>(count)).with("first", first);
            }));

    reg.add({"office.sheet_calc", "Calculate",
             "Recalculate every formula, or evaluate a formula without storing it, as if it sat in cell `at` (e.g. "
             "formula=\"SUMIF(A:A,\\\"North\\\",C:C)\"). Handy to check a result before writing it into the sheet.",
             {doc(), fileSpec("spreadsheet"), str("formula", "formula to evaluate, with or without the leading ="), sheetSpec(), str("at", "cell the formula is evaluated in (for relative refs)", false, std::string("A1"))},
             false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                if (!has(p, "formula")) {
                    auto st = d.data.recalc();
                    return Result::success("recalculated " + std::to_string(st.formulas) + " formulas" +
                                           (st.errors ? ", " + std::to_string(st.errors) + " give errors" : "") +
                                           (st.cycles ? ", " + std::to_string(st.cycles) + " in circular references" : ""))
                        .with("formulas", static_cast<std::int64_t>(st.formulas))
                        .with("errors", static_cast<std::int64_t>(st.errors))
                        .with("cycles", static_cast<std::int64_t>(st.cycles));
                }
                std::string f = trimCopy(stringParam(p, "formula"));
                if (!f.empty() && f[0] == '=') f = f.substr(1);
                std::string err = formulaError(f);
                if (!err.empty()) return Result::failure("formula error: " + err);
                int sheet;
                std::string error;
                if (!sheetArg(d.data, p, sheet, error)) return Result::failure(error);
                Range at;
                if (!parseRange(d.data, stringParam(p, "at", "A1"), sheet, at, error)) return Result::failure(error);
                CellValue v = d.data.evaluate(f, at.sheet, at.r1, at.c1);
                return Result::success("=" + f + " -> " + formatValue(v, ""), valueOf(v)).with("value", valueOf(v)).with("type", typeName(v));
            }));

    reg.add({"office.sheet_preview", "Preview Spreadsheet",
             "Render cells as a PNG grid with column letters and row numbers, the way a spreadsheet app shows them. Without range: "
             "the used area (up to 50 rows x 20 columns).",
             {doc(), fileSpec("spreadsheet"), str("range", "cells to show, e.g. A1:H30"), sheetSpec(), number("scale", "pixels per point", false, 1.5)}, false, true},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                if (!has(p, "range")) {
                    r.r2 = std::min(r.r2, r.r1 + 49);
                    r.c2 = std::min(r.c2, r.c1 + 19);
                }
                r.r2 = std::min(r.r2, r.r1 + 499);
                r.c2 = std::min(r.c2, r.c1 + 99);
                const double scale = std::clamp(numberParam(p, "scale", 1.5), 0.25, 4.0);
                raster::Image img = pdf::renderPage(sheetGridPage(d.data, r, true), scale);
                Result res = Result::success("preview of " + where(d.data, r) + " (" + std::to_string(img.width()) + "x" + std::to_string(img.height()) + ")");
                res.attach(png(img, "sheet.png"));
                return res;
            }));

    reg.add({"office.sheet_chart", "Chart",
             "Draw a bar, hbar (horizontal bars), line or pie chart of a range as a PNG. The usual layout is detected: a header row "
             "of series names and a first column of category labels (e.g. A1:C13 with months in A and two series in B and C). "
             "by_rows=true reads series from rows instead. With path the PNG is written to that file; it is always attached.",
             {doc(), fileSpec("spreadsheet"), str("range", "data range including headers/labels, e.g. A1:C13 (default: the used area)"), sheetSpec(),
              str("type", "chart type", false, std::string("bar"), chartTypes()), str("title", "chart title"),
              flag("by_rows", "series are rows, not columns", false), str("series", "only these series: header names or numbers, e.g. Total or 1,3"),
              str("path", "write the PNG here (e.g. chart.png)"),
              integer("width", "pixels", false, std::int64_t{800}), integer("height", "pixels", false, std::int64_t{500})},
             false, false},
            read(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                Chart c = chartFromRange(d.data, r, boolParam(p, "by_rows", false));
                if (has(p, "series")) {
                    std::vector<ChartSeries> keep;
                    std::string item, spec = stringParam(p, "series") + ",";
                    for (char ch : spec) {
                        if (ch != ',') {
                            item += ch;
                            continue;
                        }
                        const std::string want = lower(trimCopy(item));
                        item.clear();
                        if (want.empty()) continue;
                        bool found = false;
                        for (std::size_t i = 0; i < c.series.size(); ++i) {
                            const bool byNumber = std::all_of(want.begin(), want.end(), [](char x) { return std::isdigit(static_cast<unsigned char>(x)); }) &&
                                                  static_cast<std::size_t>(std::atoi(want.c_str())) == i + 1;
                            if (lower(c.series[i].name) == want || byNumber) {
                                keep.push_back(c.series[i]);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            std::string names;
                            for (const auto& s : c.series) names += (names.empty() ? "" : ", ") + s.name;
                            return Result::failure("no series '" + want + "' (series: " + names + ")");
                        }
                    }
                    c.series = keep;
                }
                c.type = stringParam(p, "type", "bar");
                c.title = stringParam(p, "title");
                c.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 800), 120, 4000));
                c.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 500), 90, 4000));
                raster::Image img = renderChart(c);
                std::string msg = c.type + " chart of " + where(d.data, r) + ": " + std::to_string(c.series.size()) + " series x " +
                                  std::to_string(c.categories.size()) + " categories";
                std::string series;
                for (const auto& s : c.series) series += (series.empty() ? "" : ", ") + s.name;
                msg += " (" + series + ")";
                Result res = Result::success(msg);
                if (has(p, "path")) {
                    const std::string path = stringParam(p, "path");
                    const std::string ext = extOf(path);
                    if (ext == "png") writeBytes(path, raster::encodePng(img));
                    else if (ext == "jpg" || ext == "jpeg") writeBytes(path, raster::encodeJpeg(img, 92));
                    else return Result::failure("charts are written as .png or .jpg");
                    res.message += "; wrote " + path;
                    res.value = path;
                    res.with("path", path);
                }
                res.with("series", static_cast<std::int64_t>(c.series.size())).with("categories", static_cast<std::int64_t>(c.categories.size()));
                res.attach(png(img, "chart.png"));
                return res;
            }));

    // ---- editing --------------------------------------------------------------------------

    reg.add({"office.sheet_set", "Set Cells",
             "Write cells. value= goes into every cell of range: a number, text, TRUE/FALSE, 15%, $1,200, a 2026-03-01 date or "
             "a =formula (copied down/across a range with relative references moving, like Excel's fill). csv= pastes a block of "
             "comma- or tab-separated rows starting at the range's top-left cell. Everything is recalculated.",
             {doc(), str("range", "target cell or range, e.g. B2 or D2:D13 or Sheet2!A1", true), str("value", "value or =formula"),
              str("csv", "rows of CSV/TSV text to paste at range's top-left"), str("csv_file", "read the CSV/TSV text from this file instead"), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                if (!has(p, "value") && !hasText(p, "csv")) return Result::failure("give value=<value or =formula> or csv=<rows>");
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                Range written = r;
                if (hasText(p, "csv")) {
                    written = pasteBlock(d.data, r.sheet, r.r1, r.c1, textParam(p, "csv"));
                } else {
                    const std::string input = stringParam(p, "value");
                    if (r.rows() * static_cast<long long>(r.cols()) > 1000000) return Result::failure("that range is too large to fill (over a million cells)");
                    Cell proto;
                    setInput(proto, input);
                    if (proto.hasFormula()) {
                        std::string fe = formulaError(proto.formula);
                        if (!fe.empty()) return Result::failure("formula error in " + input + ": " + fe);
                    }
                    for (int row = r.r1; row <= r.r2; ++row) {
                        for (int col = r.c1; col <= r.c2; ++col) {
                            Cell& c = sh.at(row, col);
                            const std::string keepFormat = c.format;
                            const CellStyle keepStyle = c.style;
                            c = proto;
                            if (proto.hasFormula()) c.formula = offsetFormula(proto.formula, row - r.r1, col - r.c1);
                            c.ast.reset();
                            if (c.format.empty()) c.format = keepFormat;
                            c.style = keepStyle;
                            if (c.blank()) sh.erase(row, col);
                        }
                    }
                }
                std::string note = recalcNote(d.data);
                const Cell* first = sh.get(written.r1, written.c1);
                std::string msg = "set " + where(d.data, written) + (written.rows() * written.cols() > 1 ? " (" + std::to_string(written.rows() * written.cols()) + " cells)" : "");
                msg += "; " + cellName(written.r1, written.c1) + " = " + (first ? display(first) : std::string("(empty)")) + note;
                Result res = Result::success(msg, first ? valueOf(first->value) : Value{std::string()});
                res.with("range", where(d.data, written));
                if (first) res.with("value", valueOf(first->value));
                return withPreview(res, d, p);
            }));

    reg.add({"office.sheet_clear", "Clear Cells", "Clear a range: everything, only the contents, or only the formatting (number format and bold).",
             {doc(), str("range", "cells to clear, e.g. B2:D10", true), sheetSpec(),
              str("what", "what to clear", false, std::string("all"), {"all", "contents", "formats"}), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                const std::string what = stringParam(p, "what", "all");
                int n = 0;
                for (auto it = sh.cells.begin(); it != sh.cells.end();) {
                    const int row = Sheet::rowOf(it->first), col = Sheet::colOf(it->first);
                    if (row >= r.r1 && row <= r.r2 && col >= r.c1 && col <= r.c2) {
                        ++n;
                        Cell& c = it->second;
                        if (what == "contents") {
                            c.value = {};
                            c.formula.clear();
                            c.ast.reset();
                        } else if (what == "formats") {
                            c.format.clear();
                            c.style = CellStyle{};
                        }
                        if (what == "all" || c.blank()) {
                            it = sh.cells.erase(it);
                            continue;
                        }
                    }
                    ++it;
                }
                std::string note = recalcNote(d.data);
                return withPreview(Result::success("cleared " + what + " of " + where(d.data, r) + " (" + std::to_string(n) + " cells)" + note), d, p);
            }));

    auto rowsCommand = [&](const char* id, const char* title, const char* desc, bool insert, bool rows) {
        reg.add({id, title, desc,
                 {doc(), str("at", rows ? "first row number, e.g. 5" : "first column, as a letter (C) or number (3)", true),
                  integer("count", "how many", false, std::int64_t{1}), sheetSpec(), previewSpec()},
                 true},
                edit(session, [insert, rows](SheetDoc& d, const Params& p, const std::string&) -> Result {
                    int sheet;
                    std::string error;
                    if (!sheetArg(d.data, p, sheet, error)) return Result::failure(error);
                    const std::string at = upper(trimCopy(stringParam(p, "at")));
                    int index = -1;
                    if (!at.empty() && std::all_of(at.begin(), at.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) index = std::atoi(at.c_str()) - 1;
                    else if (!rows) index = colIndex(at);
                    const int limit = rows ? kMaxRows : kMaxCols;
                    if (index < 0 || index >= limit) return Result::failure("bad position '" + at + "'");
                    const int count = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "count", 1), 1, limit));
                    if (insert && rows) d.data.insertRows(sheet, index, count);
                    else if (insert) d.data.insertCols(sheet, index, count);
                    else if (rows) d.data.deleteRows(sheet, index, count);
                    else d.data.deleteCols(sheet, index, count);
                    std::string note = recalcNote(d.data);
                    std::string what = rows ? (count == 1 ? "row " + std::to_string(index + 1) : "rows " + std::to_string(index + 1) + "-" + std::to_string(index + count))
                                            : (count == 1 ? "column " + colName(index) : "columns " + colName(index) + "-" + colName(index + count - 1));
                    return withPreview(Result::success(std::string(insert ? "inserted " : "deleted ") + what + " in " +
                                                       d.data.sheets[static_cast<std::size_t>(sheet)].name + "; formulas adjusted" + note),
                                       d, p);
                }));
    };
    rowsCommand("office.sheet_insert_rows", "Insert Rows", "Insert empty rows before row `at`; formulas that point below move down.", true, true);
    rowsCommand("office.sheet_delete_rows", "Delete Rows", "Delete rows starting at row `at`; references to deleted cells become #REF!.", false, true);
    rowsCommand("office.sheet_insert_cols", "Insert Columns", "Insert empty columns before column `at`; formulas that point right move.", true, false);
    rowsCommand("office.sheet_delete_cols", "Delete Columns", "Delete columns starting at `at`; references to deleted cells become #REF!.", false, false);

    reg.add({"office.sheet_sort", "Sort",
             "Sort the rows of a range by one or more columns. by= takes column letters or header names, each optionally followed "
             "by asc or desc: by=\"C desc, A\". The header row stays on top (detected, or set header=). Numbers sort before text, "
             "blanks go last; formulas move with their rows.",
             {doc(), str("range", "rows to sort (default: the used area)"), str("by", "sort keys, e.g. B or \"Sales desc, Region\"", true),
              flag("descending", "default direction for keys without asc/desc", false), flag("header", "the first row is a header (default: detected)"),
              sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                const bool header = has(p, "header") ? boolParam(p, "header") : looksLikeHeader(sh, r);
                struct Key {
                    int col;
                    bool desc;
                };
                std::vector<Key> keys;
                std::string spec = stringParam(p, "by");
                std::string item;
                for (std::size_t i = 0; i <= spec.size(); ++i) {
                    if (i < spec.size() && spec[i] != ',' && spec[i] != ';') {
                        item += spec[i];
                        continue;
                    }
                    std::string t = trimCopy(item);
                    item.clear();
                    if (t.empty()) continue;
                    bool desc = boolParam(p, "descending", false);
                    std::string lw = lower(t);
                    for (const char* suffix : {" desc", " descending", " asc", " ascending"}) {
                        if (endsWith(lw, suffix)) {
                            desc = std::string(suffix).find("desc") != std::string::npos;
                            t = trimCopy(t.substr(0, t.size() - std::char_traits<char>::length(suffix)));
                            break;
                        }
                    }
                    Key k{0, desc};
                    if (!columnArg(d.data, r, t, header, k.col, error)) return Result::failure(error);
                    keys.push_back(k);
                }
                if (keys.empty()) return Result::failure("no sort columns given");
                const int first = r.r1 + (header ? 1 : 0);
                if (first > r.r2) return Result::failure("nothing to sort below the header");
                std::vector<int> order;
                for (int row = first; row <= r.r2; ++row) order.push_back(row);
                std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
                    for (const auto& k : keys) {
                        const Cell* ca = sh.get(a, k.col);
                        const Cell* cb = sh.get(b, k.col);
                        CellValue va = ca ? ca->value : CellValue{}, vb = cb ? cb->value : CellValue{};
                        if (va.empty() != vb.empty()) return vb.empty();  // blanks last either way
                        int c = compareForSort(va, vb);
                        if (c != 0) return k.desc ? c > 0 : c < 0;
                    }
                    return false;
                });
                // move the cells
                std::vector<std::map<int, Cell>> rows(order.size());
                for (std::size_t i = 0; i < order.size(); ++i) {
                    for (int col = r.c1; col <= r.c2; ++col) {
                        if (const Cell* c = sh.get(order[i], col)) rows[i][col] = *c;
                        sh.erase(order[i], col);
                    }
                }
                for (std::size_t i = 0; i < order.size(); ++i) {
                    const int dest = first + static_cast<int>(i);
                    for (auto& [col, c] : rows[i]) {
                        Cell& out = sh.at(dest, col);
                        out = c;
                        if (out.hasFormula()) {
                            out.formula = offsetFormula(c.formula, dest - order[i], 0);
                            out.ast.reset();
                        }
                    }
                }
                std::string note = recalcNote(d.data);
                std::string by;
                for (const auto& k : keys) by += (by.empty() ? "" : ", ") + colName(k.col) + (k.desc ? " desc" : "");
                return withPreview(Result::success("sorted " + std::to_string(order.size()) + " rows of " + where(d.data, r) + " by " + by +
                                                   (header ? " (header kept)" : "") + note),
                                   d, p);
            }));

    reg.add({"office.sheet_filter", "Filter Rows",
             "Keep the rows where a column matches a condition. criteria works like COUNTIF: \">100\", \"<=0\", \"North\", "
             "\"<>done\", \"a*\" (wildcards), \"\" (blank). mode=copy (default) writes the header and matching rows as values to a new "
             "sheet; mode=delete removes the other rows in place; mode=count only counts.",
             {doc(), str("range", "table to filter including its header (default: the used area)"),
              str("column", "column letter or header name to test", true), str("criteria", "condition, e.g. >100 or North", true),
              str("mode", "what to do with matches", false, std::string("copy"), {"copy", "delete", "count"}),
              str("to", "mode=copy: name of the new sheet (default: <sheet> filtered)"), flag("header", "the first row is a header (default: detected)"),
              sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, true)) return Result::failure(error);
                const Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                const bool header = has(p, "header") ? boolParam(p, "header") : looksLikeHeader(sh, r);
                int col;
                if (!columnArg(d.data, r, stringParam(p, "column"), header, col, error)) return Result::failure(error);
                const std::string criteria = stringParam(p, "criteria");
                std::vector<int> keep, drop;
                for (int row = r.r1 + (header ? 1 : 0); row <= r.r2; ++row) {
                    const Cell* c = sh.get(row, col);
                    (matchesCriteria(c ? c->value : CellValue{}, criteria) ? keep : drop).push_back(row);
                }
                const std::string mode = stringParam(p, "mode", "copy");
                const std::string what = std::to_string(keep.size()) + " of " + std::to_string(keep.size() + drop.size()) + " rows match " + colName(col) + " " + criteria;
                if (mode == "count") return Result::success(what, static_cast<std::int64_t>(keep.size())).with("matches", static_cast<std::int64_t>(keep.size()));
                if (mode == "delete") {
                    for (auto it = drop.rbegin(); it != drop.rend(); ++it) d.data.deleteRows(r.sheet, *it, 1);
                    std::string note = recalcNote(d.data);
                    return withPreview(Result::success(what + "; deleted the other " + std::to_string(drop.size()) + " rows" + note, static_cast<std::int64_t>(keep.size()))
                                           .with("matches", static_cast<std::int64_t>(keep.size())),
                                       d, p);
                }
                std::string name = has(p, "to") ? stringParam(p, "to") : (sh.name + " filtered").substr(0, 31);
                if (!has(p, "to")) {
                    std::string base = name;
                    for (int n = 2; d.data.sheetIndex(name) >= 0; ++n) name = base.substr(0, 28) + " " + std::to_string(n);
                }
                if (!validSheetName(d.data, name, -1, error)) return Result::failure(error);
                Sheet out{name, {}, sh.colWidths};
                std::vector<int> rows = keep;
                if (header) rows.insert(rows.begin(), r.r1);
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    for (int c = r.c1; c <= r.c2; ++c) {
                        const Cell* src = sh.get(rows[i], c);
                        if (!src) continue;
                        Cell& dst = out.at(static_cast<int>(i), c - r.c1);
                        dst.value = src->value;
                        dst.format = src->format;
                        dst.style = src->style;
                    }
                }
                d.data.sheets.push_back(std::move(out));
                d.data.active = static_cast<int>(d.data.sheets.size()) - 1;
                return withPreview(Result::success(what + "; copied them to the new sheet " + name + " (now active)", static_cast<std::int64_t>(keep.size()))
                                       .with("matches", static_cast<std::int64_t>(keep.size()))
                                       .with("sheet", name),
                                   d, p);
            }));

    reg.add({"office.sheet_fill", "Fill",
             "Fill a range from its first cell (or first row/column): formulas are copied with relative references moving, values "
             "are repeated. With step= the first cell's number (or date) grows by step each cell: a series 1, 2, 3... or dates a "
             "week apart (step=7).",
             {doc(), str("range", "range whose first cell/row is the source, e.g. D2:D13", true),
              str("direction", "fill down or right (default: down when the range is taller than wide)", false, {}, {"down", "right"}),
              number("step", "make a series that grows by this much per cell"), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                if (r.rows() * static_cast<long long>(r.cols()) > 1000000) return Result::failure("that range is too large to fill");
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                const bool down = has(p, "direction") ? stringParam(p, "direction") == "down" : r.rows() >= r.cols();
                const int lines = down ? r.cols() : r.rows();
                const int len = down ? r.rows() : r.cols();
                if (len < 2) return Result::failure("the range needs at least two cells in the fill direction");
                int filled = 0;
                for (int l = 0; l < lines; ++l) {
                    const int sr = down ? r.r1 : r.r1 + l, sc = down ? r.c1 + l : r.c1;
                    const Cell* srcp = sh.get(sr, sc);
                    if (!srcp) continue;
                    const Cell src = *srcp;
                    for (int k = 1; k < len; ++k) {
                        const int row = down ? sr + k : sr, col = down ? sc : sc + k;
                        Cell& c = sh.at(row, col);
                        c = src;
                        c.ast.reset();
                        if (has(p, "step")) {
                            if (src.value.type != CellValue::Number || src.hasFormula()) return Result::failure("step needs a number or date in " + cellName(sr, sc));
                            c.value = CellValue::num(src.value.number + numberParam(p, "step") * k);
                        } else if (src.hasFormula()) {
                            c.formula = offsetFormula(src.formula, row - sr, col - sc);
                        }
                        ++filled;
                    }
                }
                if (!filled) return Result::failure("nothing to fill from: the first cell" + std::string(lines > 1 ? "s are" : " is") + " empty");
                std::string note = recalcNote(d.data);
                return withPreview(Result::success("filled " + std::to_string(filled) + " cells of " + where(d.data, r) + (down ? " down" : " right") + note), d, p);
            }));

    std::string presetList;
    for (const auto& [k, v] : formatPresets()) presetList += (presetList.empty() ? "" : ", ") + k;
    reg.add({"office.sheet_format", "Format Cells",
             "Format a range: a number format (a preset - " + presetList + " - or a code like #,##0.00, 0.0%, $#,##0, yyyy-mm-dd, "
             "\"0 kg\"), bold on/off, column width in characters, or autofit widths to the contents.",
             {doc(), str("range", "cells to format, e.g. B2:D13 or C:C", true), str("format", "number format preset or code"),
              flag("bold", "bold text"), number("width", "column width in characters (Excel units, default columns are 8.43)"),
              flag("autofit", "set column widths to fit their contents", false), sheetSpec(), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                Range r;
                std::string error;
                if (!rangeArg(d.data, p, r, error, false)) return Result::failure(error);
                if (!has(p, "format") && !has(p, "bold") && !has(p, "width") && !boolParam(p, "autofit", false))
                    return Result::failure("give format=, bold=, width= or autofit=true");
                Sheet& sh = d.data.sheets[static_cast<std::size_t>(r.sheet)];
                Range used = usedRange(d.data, r.sheet);
                const int r2 = std::min(r.r2, std::max(used.r2, r.r1)), c2 = std::min(r.c2, std::max(used.c2, r.c1));
                std::vector<std::string> done;
                if (has(p, "format") || has(p, "bold")) {
                    std::string fmt;
                    if (has(p, "format")) {
                        fmt = stringParam(p, "format");
                        auto it = formatPresets().find(lower(trimCopy(fmt)));
                        if (it != formatPresets().end()) fmt = it->second;
                    }
                    const bool whole = r.r2 - r.r1 >= kMaxRows - 1 || r.c2 - r.c1 >= kMaxCols - 1;
                    int n = 0;
                    for (int row = r.r1; row <= (whole ? r2 : r.r2); ++row) {
                        for (int col = r.c1; col <= (whole ? c2 : r.c2); ++col) {
                            Cell* c = nullptr;
                            if (whole) {
                                if (!sh.get(row, col)) continue;
                            }
                            c = &sh.at(row, col);
                            if (has(p, "format")) c->format = fmt;
                            if (has(p, "bold")) c->style.bold = boolParam(p, "bold");
                            if (c->blank()) sh.erase(row, col);
                            ++n;
                        }
                    }
                    if (has(p, "format")) done.push_back("format " + (fmt.empty() ? std::string("General") : fmt));
                    if (has(p, "bold")) done.push_back(boolParam(p, "bold") ? "bold" : "not bold");
                    (void)n;
                }
                if (has(p, "width")) {
                    const double w = std::clamp(numberParam(p, "width"), 0.5, 255.0);
                    for (int col = r.c1; col <= std::min(r.c2, r.c1 + 1000); ++col) sh.colWidths[col] = w;
                    done.push_back("width " + textNumber(w));
                }
                if (boolParam(p, "autofit", false)) {
                    for (int col = r.c1; col <= std::min(c2, r.c1 + 1000); ++col) {
                        double best = 0;
                        for (int row = 0; row <= used.r2; ++row) {
                            const Cell* c = sh.get(row, col);
                            if (!c) continue;
                            const pdf::Font f{pdf::Family::Sans, c->style.bold, false};
                            best = std::max(best, pdf::textWidth(f, display(c), 11) / 6.0);
                        }
                        sh.colWidths[col] = std::clamp(std::ceil(best + 1.5), 4.0, 80.0);
                    }
                    done.push_back("autofit widths");
                }
                std::string list;
                for (const auto& x : done) list += (list.empty() ? "" : ", ") + x;
                return withPreview(Result::success("formatted " + where(d.data, r) + ": " + list), d, p);
            }));

    reg.add({"office.sheet_replace", "Replace in Spreadsheet",
             "Replace text inside text cells (and formulas with formulas=true) across the workbook or one sheet.",
             {doc(), str("find", "text to replace", true), str("replace", "replacement (may be empty)", false, std::string()),
              str("sheet", "only this sheet (default: all)"), flag("match_case", "case-sensitive", false),
              flag("formulas", "also replace inside formulas", false), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                const std::string find = stringParam(p, "find"), rep = stringParam(p, "replace");
                if (find.empty()) return Result::failure("find is empty");
                const bool mc = boolParam(p, "match_case", false);
                int only = -1;
                std::string error;
                if (has(p, "sheet") && !sheetArg(d.data, p, only, error)) return Result::failure(error);
                auto replaceIn = [&](std::string& s) {
                    int n = 0;
                    std::string hay = mc ? s : lower(s), needle = mc ? find : lower(find), out;
                    std::size_t pos = 0;
                    for (;;) {
                        std::size_t at = hay.find(needle, pos);
                        if (at == std::string::npos) break;
                        out += s.substr(pos, at - pos) + rep;
                        pos = at + find.size();
                        ++n;
                    }
                    if (n) s = out + s.substr(pos);
                    return n;
                };
                int count = 0, cells = 0;
                for (std::size_t si = 0; si < d.data.sheets.size(); ++si) {
                    if (only >= 0 && static_cast<int>(si) != only) continue;
                    for (auto& [k, c] : d.data.sheets[si].cells) {
                        int n = 0;
                        if (c.hasFormula()) {
                            if (boolParam(p, "formulas", false)) {
                                n = replaceIn(c.formula);
                                if (n) c.ast.reset();
                            }
                        } else if (c.value.type == CellValue::Text) {
                            std::string t = c.value.text;
                            n = replaceIn(t);
                            if (n) setInput(c, t.empty() ? std::string() : (t[0] == '=' ? "'" + t : t));
                        }
                        count += n;
                        cells += n ? 1 : 0;
                    }
                }
                std::string note = recalcNote(d.data);
                return withPreview(Result::success("replaced " + std::to_string(count) + " occurrence(s) in " + std::to_string(cells) + " cell(s)" + note,
                                                   static_cast<std::int64_t>(count))
                                       .with("count", static_cast<std::int64_t>(count)),
                                   d, p);
            }));

    // ---- sheets ---------------------------------------------------------------------------

    reg.add({"office.sheet_add", "Add Sheet", "Add a sheet (and make it active), optionally filled from CSV/TSV text.",
             {doc(), str("name", "new sheet name (default: SheetN)"), integer("at", "position, 1 = first (default: last)"),
              str("csv", "initial cells as CSV or tab-separated text"), str("csv_file", "read the CSV/TSV text from this file instead"), previewSpec()},
             true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                std::string name = stringParam(p, "name");
                if (name.empty()) {
                    for (int n = static_cast<int>(d.data.sheets.size()) + 1;; ++n) {
                        name = "Sheet" + std::to_string(n);
                        if (d.data.sheetIndex(name) < 0) break;
                    }
                }
                std::string error;
                if (!validSheetName(d.data, name, -1, error)) return Result::failure(error);
                int at = has(p, "at") ? static_cast<int>(intParam(p, "at")) - 1 : static_cast<int>(d.data.sheets.size());
                at = std::clamp(at, 0, static_cast<int>(d.data.sheets.size()));
                d.data.sheets.insert(d.data.sheets.begin() + at, Sheet{name, {}, {}});
                d.data.active = at;
                if (hasText(p, "csv")) pasteBlock(d.data, at, 0, 0, textParam(p, "csv"));
                std::string note = recalcNote(d.data);
                return withPreview(Result::success("added sheet " + name + " at position " + std::to_string(at + 1) + note, name).with("sheet", name), d, p);
            }));

    reg.add({"office.sheet_rename", "Rename Sheet", "Rename a sheet; formulas that refer to it are updated.",
             {doc(), str("sheet", "sheet to rename (name or 1-based number)", true), str("name", "new name", true)}, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                const std::string name = trimCopy(stringParam(p, "name"));
                if (!validSheetName(d.data, name, s, error)) return Result::failure(error);
                const std::string old = d.data.sheets[static_cast<std::size_t>(s)].name;
                d.data.renameSheet(s, name);
                d.data.recalc();
                return Result::success("renamed " + old + " to " + name, name);
            }));

    reg.add({"office.sheet_delete", "Delete Sheet", "Delete a sheet; references to it from other sheets become #REF!.",
             {doc(), str("sheet", "sheet to delete (name or 1-based number)", true)}, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                if (d.data.sheets.size() == 1) return Result::failure("a workbook needs at least one sheet");
                const std::string old = d.data.sheets[static_cast<std::size_t>(s)].name;
                d.data.deleteSheet(s);
                std::string note = recalcNote(d.data);
                return Result::success("deleted sheet " + old + note);
            }));

    reg.add({"office.sheet_select", "Select Sheet", "Make a sheet the active one (the default for other commands and for CSV saves).",
             {doc(), str("sheet", "sheet name or 1-based number", true)}, true},
            edit(session, [](SheetDoc& d, const Params& p, const std::string&) -> Result {
                int s;
                std::string error;
                if (!sheetArg(d.data, p, s, error)) return Result::failure(error);
                d.data.active = s;
                return Result::success("active sheet is " + d.data.sheets[static_cast<std::size_t>(s)].name);
            }));
}

}  // namespace wetwaresuite::office
