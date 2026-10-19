// Helpers shared by the office.sheet_* command sources (office-internal).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "commands.hpp"

namespace wetwaresuite::office::sheetcmd {

using Edit = std::function<Result(SheetDoc&, const Params&, const std::string&)>;

ActionFactory edit(Session& s, Edit fn);  // undoable (snapshot)
ActionFactory read(Session& s, Edit fn);  // not undoable; file= works too

ParamSpec doc();
ParamSpec sheetSpec();

Value valueOf(const CellValue& v);
std::string typeName(const CellValue& v);
// sheet=... (name or 1-based number), or the active sheet.
bool sheetArg(const Workbook& wb, const Params& p, int& out, std::string& error, const char* key = "sheet");
Range usedRange(const Workbook& wb, int sheet);
// range=... (with an optional sheet= default); missing means the used range. clip trims whole rows/columns to the used area.
bool rangeArg(const Workbook& wb, const Params& p, Range& r, std::string& error, bool clip, const char* key = "range");
std::string where(const Workbook& wb, const Range& r);
std::string recalcNote(Workbook& wb);
std::string display(const Cell* c);
bool columnArg(const Workbook& wb, const Range& r, const std::string& text, bool header, int& col, std::string& error);
bool looksLikeHeader(const Sheet& sh, const Range& r);
bool validSheetName(const Workbook& wb, const std::string& name, int except, std::string& error);
Result withPreview(Result r, const SheetDoc& d, const Params& p);
int compareForSort(const CellValue& a, const CellValue& b);
// A fresh sheet name based on `base` ("Pivot", "Pivot 2", ...).
std::string freshSheetName(const Workbook& wb, const std::string& base);

// Style params shared by sheet_style and the conditional-format command: bold=, italic=, color=, fill=, ...
std::vector<ParamSpec> styleSpecs();
// Applies the style params that are present; false with a reason on a bad value.
bool applyStyleParams(const Params& p, CellStyle& s, std::vector<std::string>& done, std::string& error);
std::string describeStyle(const CellStyle& s);  // "bold, fill #ffeb9c, ..." ("plain" when nothing is set)

// Checks cells against the data validation rules covering them. Returns false with the message when a
// stop rule rejects one; warnings (warning/information rules) are added to `notes`.
bool checkValidation(const Workbook& wb, int sheet, const Range& r, std::string& error, std::string& notes);

// CSV import params (encoding=, decimal=, dates=, text_columns=, skip_rows=, delimiter=) and their parser.
std::vector<ParamSpec> csvSpecs();
bool csvOptions(const Params& p, CsvOptions& o, std::string& error);

void registerSheetFileCommands(Session& session);
void registerSheetReadCommands(Session& session);
void registerSheetEditCommands(Session& session);
void registerSheetTableCommands(Session& session);
void registerSheetStyleCommands(Session& session);
void registerSheetLayoutCommands(Session& session);
void registerSheetRuleCommands(Session& session);
void registerSheetObjectCommands(Session& session);
void registerSheetDataCommands(Session& session);

}  // namespace wetwaresuite::office::sheetcmd
