// Spreadsheet helpers shared between the office sources but not public.
#pragma once

#include <string>

#include "achclwss/office/sheet.hpp"

namespace achclwss::office {

// Formula text as stored in .xlsx files (newer functions carry an "_xlfn." prefix) and back.
std::string fileFormula(const std::string& formula, bool toFile);

// An OpenDocument formula ("of:=SUM([.A1:.B2])") in A1 syntax ("SUM(A1:B2)").
std::string odfFormula(const std::string& formula);

// COUNTIF-style criteria: ">100", "<>done", "North", "a*" (wildcards), "" (blank).
bool matchesCriteria(const CellValue& v, const std::string& criteria);

}  // namespace achclwss::office
