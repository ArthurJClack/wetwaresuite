// Word wrap with mixed styles, shared by the document and slide layouts.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/office/doc.hpp"

namespace wetwaresuite::office::textlayout {

struct Piece {
    std::string text;  // a word, a run of spaces, or "\n"; empty for an indent (width only)
    pdf::Font font;
    double size = 11;
    pdf::Color color{0x22, 0x22, 0x22, 255};
    bool underline = false, strike = false;
    std::optional<pdf::Color> highlight;
    double rise = 0;     // baseline shift up (superscript) or down (negative), pt
    std::string link;
    std::string note;    // a footnote reference: the note's text (the piece shows its number)
    double width = 0;
    bool space() const { return !text.empty() && text[0] == ' '; }
    bool newline() const { return text == "\n"; }
};

struct Line {
    std::vector<Piece> pieces;
    std::vector<double> xs;  // x offset of each piece
    double width = 0;
    double size = 11;  // largest font size on the line
    bool hardBreak = false;  // ended by a line break (not justified)
};

struct Style {
    pdf::Font font;
    double size = 11;
    pdf::Color color{0x22, 0x22, 0x22, 255};
    pdf::Color link{0x05, 0x63, 0xC1, 255};
    bool underline = false;
    const TextDocument* doc = nullptr;  // for tracked changes and comments (markup view)
};

std::vector<Piece> pieces(const Runs& runs, const Style& st);
std::vector<Line> wrap(const std::vector<Piece>& in, double maxWidth, double baseSize);
// Adds the text ops of one line with its top at y (baseline at y + size * 0.92).
void drawLine(pdf::Page& page, const Line& l, double x, double y);
// Same, placed in a column `width` wide: align 'l', 'c', 'r' or 'j' (justified unless `last`).
void drawLine(pdf::Page& page, const Line& l, double x, double y, double width, char align, bool last);

}  // namespace wetwaresuite::office::textlayout
