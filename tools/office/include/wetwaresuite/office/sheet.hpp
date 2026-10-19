// Spreadsheets: workbooks of sheets of cells, a formula engine with
// dependency-ordered recalculation, cycle detection and dynamic arrays that
// spill, named ranges, cell styles, merged cells, conditional formatting,
// data validation, in-sheet charts, and CSV/TSV, XLSX and ODS files.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "wetwaresuite/office/color.hpp"

namespace wetwaresuite::office {

struct CellValue {
    enum Type { Empty, Number, Text, Bool, Error };
    Type type = Empty;
    double number = 0;  // Number, or 0/1 for Bool
    std::string text;   // Text, or the error code such as "#DIV/0!"

    static CellValue num(double v);
    static CellValue str(std::string s);
    static CellValue boolean(bool b);
    static CellValue error(std::string code);
    bool isError() const { return type == Error; }
    bool empty() const { return type == Empty; }
    bool operator==(const CellValue& o) const;
};

struct FormulaNode;  // parsed formula, cached on the cell


// Borders per side: 0 none, 1 thin, 2 medium, 3 thick, 4 double, 5 dashed, 6 dotted.
enum BorderSide { BorderLeft, BorderRight, BorderTop, BorderBottom };
const std::vector<std::string>& borderStyles();  // index = border code

struct CellStyle {
    bool bold = false, italic = false, underline = false, strike = false, wrap = false;
    char align = 0;   // 0 general (numbers right, text left), 'l', 'c', 'r'
    char valign = 0;  // 0 bottom, 't' top, 'm' middle
    std::uint8_t indent = 0;
    float size = 0;  // font size in points; 0 = the default 11
    std::uint32_t color = 0, fill = 0, borderColor = 0;
    std::uint8_t border[4] = {0, 0, 0, 0};
    bool operator==(const CellStyle& o) const;
    bool plain() const { return *this == CellStyle{}; }
};

struct Cell {
    CellValue value;      // the literal, or a formula's last result
    std::string formula;  // without the leading '='; empty for plain values
    std::string format;   // number format code; empty means General
    CellStyle style;
    std::shared_ptr<const FormulaNode> ast;
    bool spilled = false;              // the value was spilled here by a dynamic-array formula nearby
    int spillRows = 0, spillCols = 0;  // on a formula whose array result spilled: its size

    bool hasFormula() const { return !formula.empty(); }
    bool hasContent() const { return !value.empty() || !formula.empty(); }
    bool blank() const { return value.empty() && formula.empty() && format.empty() && style.plain(); }
};

constexpr int kMaxRows = 1048576;
constexpr int kMaxCols = 16384;

struct Range {
    int sheet = 0;
    int r1 = 0, c1 = 0, r2 = 0, c2 = 0;  // 0-based, inclusive
    int rows() const { return r2 - r1 + 1; }
    int cols() const { return c2 - c1 + 1; }
    bool contains(int row, int col) const { return row >= r1 && row <= r2 && col >= c1 && col <= c2; }
    bool overlaps(const Range& o) const { return r1 <= o.r2 && o.r1 <= r2 && c1 <= o.c2 && o.c1 <= c2; }
};

// A conditional format: cells in `range` that meet the rule are drawn with `style` (fill, font colour,
// bold, ...), or shaded along a colour scale / data bar.
struct CondFormat {
    Range range;
    // cell (op value1 [value2]), formula (value1 is TRUE for the top-left cell; relative refs move),
    // text (contains value1), begins, ends, top / bottom (value1 = N, percent), above_average, below_average,
    // duplicate, unique, blank, not_blank, errors, color_scale, data_bar
    std::string type = "cell";
    std::string op = ">";  // >, <, >=, <=, =, <>, between, not_between
    std::string value1, value2;
    bool percent = false;
    CellStyle style;
    std::uint32_t minColor = 0, midColor = 0, maxColor = 0;  // color_scale (mid optional); data_bar uses maxColor
};
const std::vector<std::string>& condFormatTypes();

// Data validation: what may be typed into `range`.
struct Validation {
    Range range;
    std::string type = "list";   // list, whole, decimal, date, time, length, custom
    std::string op = "between";  // between, not_between, =, <>, >, <, >=, <=
    std::string value1, value2;  // list: "a,b,c" or a range like $F$1:$F$9; custom: a formula; others numbers or formulas
    bool allowBlank = true;
    bool dropdown = true;
    std::string errorStyle = "stop";  // stop (reject), warning, information (accept but report)
    std::string errorTitle, error, promptTitle, prompt;
};
const std::vector<std::string>& validationTypes();

// A chart that lives in the sheet (saved as a real chart in .xlsx and drawn in previews and PDFs).
struct SheetChart {
    std::string type = "bar";  // bar, hbar, line, pie, scatter, area
    std::string title;
    std::string range;  // data, e.g. A1:C13 (header row of series names, first column of categories)
    bool byRows = false;
    int row = 0, col = 0;           // top-left cell it sits on
    int width = 480, height = 288;  // pixels
};

struct Sheet {
    std::string name;
    std::map<std::int64_t, Cell> cells;  // key: row * kMaxCols + col, both 0-based (row-major order)
    std::map<int, double> colWidths;     // column -> width in characters
    std::map<int, double> rowHeights;    // row -> height in points (default 15)
    std::set<int> hiddenRows, hiddenCols;
    std::vector<Range> merges;  // merged areas; the top-left cell holds the value
    int freezeRows = 0, freezeCols = 0;
    std::vector<CondFormat> condFormats;
    std::vector<Validation> validations;
    std::vector<SheetChart> charts;
    bool hasAutoFilter = false;
    Range autoFilter;  // the header row + data of the filtered table

    static std::int64_t key(int row, int col) { return static_cast<std::int64_t>(row) * kMaxCols + col; }
    static int rowOf(std::int64_t k) { return static_cast<int>(k / kMaxCols); }
    static int colOf(std::int64_t k) { return static_cast<int>(k % kMaxCols); }

    const Cell* get(int row, int col) const;
    Cell& at(int row, int col);
    void erase(int row, int col);
    // Used range (0-based, inclusive); false when the sheet is empty.
    bool bounds(int& r1, int& c1, int& r2, int& c2) const;
    // The merged area holding (row, col), or nullptr.
    const Range* mergeAt(int row, int col) const;
};

// A workbook-level name: "TaxRate" -> "Sheet1!$B$1", "Sales" -> "Data!$C$2:$C$100", or any formula.
struct DefinedName {
    std::string name;     // as written
    std::string formula;  // without '='
    std::string comment;
    std::shared_ptr<const FormulaNode> ast;
};

struct Workbook {
    std::vector<Sheet> sheets;
    int active = 0;
    std::map<std::string, DefinedName> names;  // key: upper-case name

    int sheetIndex(const std::string& name) const;  // case-insensitive; -1 if missing
    const DefinedName* name(const std::string& n) const;  // case-insensitive; nullptr if missing

    struct Stats {
        int formulas = 0;
        int cycles = 0;  // cells that are part of (or depend on) a circular reference
        int errors = 0;  // formula results that are error values
        int spills = 0;  // formulas whose array result spilled over neighbouring cells
    };
    // Recalculates every formula in dependency order. Cells in a cycle get #CYCLE!; array results
    // spill into the cells below/right (#SPILL! when something is in the way).
    Stats recalc();
    // Evaluates a formula (without '=') as if it sat in the given cell, without storing it.
    CellValue evaluate(const std::string& formula, int sheet, int row = 0, int col = 0) const;
    // The same, keeping an array result (rows of values); a single value is a 1x1 array.
    std::vector<std::vector<CellValue>> evaluateArray(const std::string& formula, int sheet, int row = 0, int col = 0) const;

    // Structural edits; formulas everywhere are adjusted (deleted references become #REF!).
    void insertRows(int sheet, int at, int count);
    void deleteRows(int sheet, int at, int count);
    void insertCols(int sheet, int at, int count);
    void deleteCols(int sheet, int at, int count);
    void renameSheet(int sheet, const std::string& name);
    void deleteSheet(int sheet);
    // Cut and paste: the cells of `from` (values, formulas, formats, merges) move so the top-left lands on
    // (toSheet, toRow, toCol); references anywhere that pointed into the block follow it.
    void moveRange(const Range& from, int toSheet, int toRow, int toCol);
};

// ---- A1 notation -------------------------------------------------------------

std::string colName(int col);                 // 0 -> "A", 27 -> "AB"
int colIndex(const std::string& letters);     // "AB" -> 27, -1 if invalid
std::string cellName(int row, int col);       // (0, 0) -> "A1"
std::string rangeName(const Range& r);        // "A1:C3" or "B2"
std::string quoteSheet(const std::string& name);  // adds '...' when the name needs it
// "B2", "A1:C3", "A:A", "2:4", "Sheet2!A1:B3", "'My sheet'!A1", or a defined name. false with a reason on error.
bool parseRange(const Workbook& wb, const std::string& text, int defaultSheet, Range& out, std::string& error);

// ---- formulas ----------------------------------------------------------------

std::shared_ptr<const FormulaNode> parseFormula(const std::string& formula);  // never throws; bad syntax evaluates to an error
std::string formulaError(const std::string& formula);  // empty when the formula parses
std::vector<std::string> functionNames();               // every function the engine knows, sorted
bool validName(const std::string& name);                // usable as a defined name (not a cell ref, no spaces)

// A reference inside a formula, as rewriting callbacks see it.
struct FormulaRef {
    enum Kind { CellRef, RangeRef, ColsRef, RowsRef };
    Kind kind = CellRef;
    std::string sheet;  // as written, unquoted; empty when the ref names no sheet
    int r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    bool absR1 = false, absC1 = false, absR2 = false, absC2 = false;
};
// Rewrites every reference; the callback edits it in place or returns false to make it #REF!.
std::string rewriteFormula(const std::string& formula, const std::function<bool(FormulaRef&)>& fn);
// The formula as it would read when copied dr rows and dc columns away (relative refs move).
std::string offsetFormula(const std::string& formula, int dr, int dc);
// Sheet-qualified references a formula reads, resolved against the workbook (for dependents/precedents).
std::vector<Range> formulaReferences(const Workbook& wb, const std::string& formula, int sheet);

// ---- values, input and number formats ----------------------------------------

// Fills a cell from typed input: "=SUM(A1:A3)", "12.5", "TRUE", "15%", "2024-03-01", "'007" (forced text).
void setInput(Cell& cell, const std::string& input);
// What you would type to recreate the cell: "=..." for formulas, otherwise the raw value.
std::string inputText(const Cell& cell);
// The value as displayed with a number format ("#,##0.00", "0%", "yyyy-mm-dd", "$#,##0", ...).
std::string formatValue(const CellValue& v, const std::string& format);
// The [Red]/[Blue]/... colour a format section asks for, as a colour (0 when none).
std::uint32_t formatColor(const CellValue& v, const std::string& format);
std::string generalNumber(double v);  // General format, up to 10 significant digits
bool isDateFormat(const std::string& format);

double dateSerial(int year, int month, int day);  // Excel 1900 date system
void serialDate(double serial, int& year, int& month, int& day);

// ---- formatting rules ----------------------------------------------------------

// What cells look like once conditional formats apply. Build one per sheet and drawing pass: it works
// out each rule's statistics (averages, top-N thresholds, colour-scale ends) once.
class CellPainter {
public:
    CellPainter(const Workbook& wb, int sheet);
    ~CellPainter();
    CellPainter(const CellPainter&) = delete;
    CellPainter& operator=(const CellPainter&) = delete;
    // `bar` gets a data bar's length (0..1) or -1 when there is none.
    CellStyle style(int row, int col, double* bar = nullptr, std::uint32_t* barColor = nullptr) const;
    // Does rule i match the cell? (for listing which cells a rule highlights)
    bool matches(std::size_t rule, int row, int col) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
CellStyle effectiveStyle(const Workbook& wb, int sheet, int row, int col, double* bar = nullptr, std::uint32_t* barColor = nullptr);
// Checks the cell's current value against the validation rule covering it. false with the message when
// it fails; `reject` is set when the rule's style is stop.
bool validateCell(const Workbook& wb, int sheet, int row, int col, std::string& message, bool& reject);
const Validation* validationAt(const Workbook& wb, int sheet, int row, int col);
// The allowed values of a list validation, in order.
std::vector<std::string> validationList(const Workbook& wb, int sheet, const Validation& v);

// ---- files ---------------------------------------------------------------------

struct CsvOptions {
    char delimiter = 0;          // 0 = detect
    int skipRows = 0;            // lines to drop at the top
    bool detectTypes = true;     // false: every field stays text
    bool decimalComma = false;   // 1.234,5 means 1234.5
    std::string dateOrder;       // "", "dmy" or "mdy": how 03/04/2026 reads
    std::set<int> textColumns;   // 0-based columns kept as text (leading zeros, ids)
    std::string encoding;        // "", "utf-8" or "latin1"
};
Workbook readCsv(const std::string& text, char delimiter = ',');  // values typed like setInput
Workbook readCsv(const std::string& text, const CsvOptions& options);
std::string writeCsv(const Sheet& sheet, char delimiter = ',');   // computed values, not formulas
char sniffDelimiter(const std::string& text);                     // ',', ';', '\t' or '|'
Workbook readXlsx(const std::string& bytes);  // throws std::runtime_error
std::string writeXlsx(const Workbook& wb);
Workbook readOds(const std::string& bytes);   // throws std::runtime_error
std::string writeOds(const Workbook& wb);

}  // namespace wetwaresuite::office
