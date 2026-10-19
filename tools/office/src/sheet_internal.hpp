// Spreadsheet helpers shared between the office sources but not public.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/office/sheet.hpp"

namespace wetwaresuite::office {

// Formula text as stored in .xlsx files (newer functions carry an "_xlfn." prefix) and back.
std::string fileFormula(const std::string& formula, bool toFile);

// An OpenDocument formula ("of:=SUM([.A1:.B2])") in A1 syntax ("SUM(A1:B2)").
std::string odfFormula(const std::string& formula);

// COUNTIF-style criteria: ">100", "<>done", "North", "a*" (wildcards), "" (blank).
bool matchesCriteria(const CellValue& v, const std::string& criteria);

// How office.sheet_fill continues a series.
struct FillSpec {
    std::string series = "auto";  // auto, copy, linear, growth
    std::string unit;             // dates: "" / day, weekday, month, year
    bool hasStep = false;
    double step = 1;              // linear: added per cell; growth: multiplied
};
// The `count` cells that follow the `src` cells. copiedFrom[k] is the source index a cell repeats (so
// formulas can be shifted) or -1 for a computed value. false with a reason when the series can't be made.
bool fillSeries(const std::vector<Cell>& src, int count, const FillSpec& spec, std::vector<Cell>& out, std::vector<int>& copiedFrom, std::string& error);

}  // namespace wetwaresuite::office
