// Shared bits of the pdf library that are not public API.
#pragma once

#include <string>

namespace wetwaresuite::pdf::detail {

double stdWidth(int fontIndex, unsigned char winAnsiByte);  // 1/1000 em

struct Glyph {
    int left = 0, top = 0, width = 0, height = 0;  // in pixels at glyphEm(), top is above the baseline
    const unsigned char* coverage = nullptr;       // width * height values 0..15
};
int glyphEm();
// rasterFont: 0 sans, 1 sans bold, 2 serif, 3 serif bold, 4 mono, 5 mono bold
Glyph glyph(int rasterFont, unsigned char winAnsiByte);

// Compact number text for PDF syntax: up to three decimals, no trailing zeros.
std::string num(double v);

}  // namespace wetwaresuite::pdf::detail
