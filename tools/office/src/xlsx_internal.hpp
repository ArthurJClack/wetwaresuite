// Pieces shared by the .xlsx reader and writer (office-internal).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wetwaresuite/office/sheet.hpp"
#include "xml.hpp"

namespace wetwaresuite::office::xlsx {

inline const char* kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
inline const char* kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline const char* kPkgRelNs = "http://schemas.openxmlformats.org/package/2006/relationships";
inline const char* kChartNs = "http://schemas.openxmlformats.org/drawingml/2006/chart";
inline const char* kDrawingNs = "http://schemas.openxmlformats.org/drawingml/2006/main";
inline const char* kSheetDrawingNs = "http://schemas.openxmlformats.org/drawingml/2006/spreadsheetDrawing";
inline const char* kRelType = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";

using Files = std::map<std::string, std::string>;

const std::map<int, std::string>& builtinFormats();
std::string zipEntry(const Files& files, const std::string& name);
// A relationship target resolved against the folder of the part that owns it.
std::string resolveTarget(const std::string& base, const std::string& target);
std::map<std::string, std::string> readRels(const Files& files, const std::string& part);  // id -> part path
bool cellRef(const std::string& ref, int& row, int& col);  // "B12" -> 0-based
bool rangeRef(const std::string& ref, Range& out);        // "A1:C3" or "B2" (no sheet)

std::string argb(std::uint32_t color);  // "FFRRGGBB"
// A <color rgb=".." theme=".." indexed=".." tint=".."/> element; 0 when automatic.
std::uint32_t readColor(const xml::Node* n);
const char* borderName(int code);  // "thin", ... ("" for none)
int borderCode(const std::string& name);

// Excel's names for the comparison operators and back.
std::string opName(const std::string& op);  // ">" -> "greaterThan"
std::string opFromName(const std::string& name);

// A rule value as an .xlsx formula ("North" -> "\"North\"", 2026-01-05 -> 46027) and back.
std::string ruleFormula(const std::string& value);
std::string ruleValue(const std::string& formula, bool date);

// Charts in a sheet: the drawing part, one chart part per chart and their relationships.
struct ChartParts {
    std::string drawing, drawingRels;
    std::vector<std::string> charts;  // chartspace xml, in order
};
ChartParts writeCharts(const Workbook& wb, int sheet, int firstChartNumber);
// Reads the charts of a drawing part back into the sheet (once every sheet is loaded, so references resolve).
void readCharts(const Files& files, const std::string& drawingPath, Workbook& wb, int sheet);

// Column widths in pixels, as Excel's default font measures them.
double colPixels(const Sheet& sh, int col);
double rowPixels(const Sheet& sh, int row);

}  // namespace wetwaresuite::office::xlsx
