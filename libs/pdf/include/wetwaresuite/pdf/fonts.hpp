// The PDF standard fonts (Helvetica, Times, Courier and their bold/italic
// faces): advance widths for laying text out, WinAnsi encoding, and UTF-8
// helpers. Characters those fonts lack (Greek, Cyrillic, ...) are drawn and
// embedded with a built-in Unicode font; ones it lacks too become '?'.
#pragma once

#include <string>
#include <vector>

namespace wetwaresuite::pdf {

enum class Family { Sans, Serif, Mono };  // Helvetica, Times, Courier

struct Font {
    Family family = Family::Sans;
    bool bold = false;
    bool italic = false;
    bool operator==(const Font&) const = default;
};

int fontIndex(Font font);            // 0..11, the order of the standard-14 table
const char* baseFontName(Font font);  // "Helvetica-Bold", "Times-Italic", ...

std::vector<unsigned> utf8Decode(const std::string& text);  // invalid bytes read as Latin-1
std::string utf8Encode(unsigned codepoint);

unsigned char toWinAnsi(unsigned codepoint);  // '?' when it has no WinAnsi byte
unsigned fromWinAnsi(unsigned char byte);     // the Unicode code point of a WinAnsi byte
std::string winAnsi(const std::string& utf8);  // the whole string as WinAnsi bytes

double charWidth(Font font, unsigned char winAnsiByte);       // advance in 1/1000 em
double textWidth(Font font, const std::string& utf8, double size);  // advance in points, Unicode fallback included

}  // namespace wetwaresuite::pdf
