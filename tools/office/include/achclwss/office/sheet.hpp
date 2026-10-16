// Spreadsheets: workbooks of sheets of cells, a formula engine with
// dependency-ordered recalculation and cycle detection, number formats, and
// CSV/TSV, XLSX and ODS files.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace achclwss::office {

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

struct Cell {
    CellValue value;      // the literal, or a formula's last result
    std::string formula;  // without the leading '='; empty for plain values
    std::string format;   // number format code; empty means General
    bool bold = false;
    std::shared_ptr<const FormulaNode> ast;

    bool hasFormula() const { return !formula.empty(); }
    bool blank() const { return value.empty() && formula.empty() && format.empty() && !bold; }
};

constexpr int kMaxRows = 1048576;
constexpr int kMaxCols = 16384;

struct Sheet {
    std::string name;
    std::map<std::int64_t, Cell> cells;  // key: row * kMaxCols + col, both 0-based (row-major order)
    std::map<int, double> colWidths;     // column -> width in characters

    static std::int64_t key(int row, int col) { return static_cast<std::int64_t>(row) * kMaxCols + col; }
    static int rowOf(std::int64_t k) { return static_cast<int>(k / kMaxCols); }
    static int colOf(std::int64_t k) { return static_cast<int>(k % kMaxCols); }

    const Cell* get(int row, int col) const;
    Cell& at(int row, int col);
    void erase(int row, int col);
    // Used range (0-based, inclusive); false when the sheet is empty.
    bool bounds(int& r1, int& c1, int& r2, int& c2) const;
};

struct Range {
    int sheet = 0;
    int r1 = 0, c1 = 0, r2 = 0, c2 = 0;  // 0-based, inclusive
    int rows() const { return r2 - r1 + 1; }
    int cols() const { return c2 - c1 + 1; }
};

struct Workbook {
    std::vector<Sheet> sheets;
    int active = 0;

    int sheetIndex(const std::string& name) const;  // case-insensitive; -1 if missing

    struct Stats {
        int formulas = 0;
        int cycles = 0;  // cells that are part of (or depend on) a circular reference
        int errors = 0;  // formula results that are error values
    };
    // Recalculates every formula in dependency order. Cells in a cycle get #CYCLE!.
    Stats recalc();
    // Evaluates a formula (without '=') as if it sat in the given cell, without storing it.
    CellValue evaluate(const std::string& formula, int sheet, int row = 0, int col = 0) const;

    // Structural edits; formulas everywhere are adjusted (deleted references become #REF!).
    void insertRows(int sheet, int at, int count);
    void deleteRows(int sheet, int at, int count);
    void insertCols(int sheet, int at, int count);
    void deleteCols(int sheet, int at, int count);
    void renameSheet(int sheet, const std::string& name);
    void deleteSheet(int sheet);
};

// ---- A1 notation -------------------------------------------------------------

std::string colName(int col);                 // 0 -> "A", 27 -> "AB"
int colIndex(const std::string& letters);     // "AB" -> 27, -1 if invalid
std::string cellName(int row, int col);       // (0, 0) -> "A1"
std::string rangeName(const Range& r);        // "A1:C3" or "B2"
std::string quoteSheet(const std::string& name);  // adds '...' when the name needs it
// "B2", "A1:C3", "A:A", "2:4", "Sheet2!A1:B3", "'My sheet'!A1". false with a reason on error.
bool parseRange(const Workbook& wb, const std::string& text, int defaultSheet, Range& out, std::string& error);

// ---- formulas ----------------------------------------------------------------

std::shared_ptr<const FormulaNode> parseFormula(const std::string& formula);  // never throws; bad syntax evaluates to an error
std::string formulaError(const std::string& formula);  // empty when the formula parses

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
std::string generalNumber(double v);  // General format, up to 10 significant digits
bool isDateFormat(const std::string& format);

double dateSerial(int year, int month, int day);  // Excel 1900 date system
void serialDate(double serial, int& year, int& month, int& day);

// ---- files ---------------------------------------------------------------------

Workbook readCsv(const std::string& text, char delimiter = ',');  // values typed like setInput
std::string writeCsv(const Sheet& sheet, char delimiter = ',');   // computed values, not formulas
char sniffDelimiter(const std::string& text);                     // ',', ';', '\t' or '|'
Workbook readXlsx(const std::string& bytes);  // throws std::runtime_error
std::string writeXlsx(const Workbook& wb);
Workbook readOds(const std::string& bytes);   // throws std::runtime_error

}  // namespace achclwss::office
