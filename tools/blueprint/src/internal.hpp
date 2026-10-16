// Helpers shared by the blueprint sources but not part of the public API.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/blueprint/blueprint.hpp"

namespace wetwaresuite::blueprint::detail {

std::vector<unsigned> utf8Decode(const std::string& text);  // invalid bytes are read as Latin-1
std::string utf8Encode(unsigned codepoint);
double glyphWidth(unsigned codepoint);  // Helvetica advance, 1/1000 em

// Border frame and title block, in paper mm (y up from the bottom-left corner).
struct SheetItem {
    Entity e;  // line, polyline or text; text height is the cap height in mm
    double weight = 0.25;
};
std::vector<SheetItem> sheetItems(const Drawing& doc);

// Compact number text for messages and files: up to `decimals`, no trailing zeros.
std::string num(double v, int decimals = 4);

}  // namespace wetwaresuite::blueprint::detail
