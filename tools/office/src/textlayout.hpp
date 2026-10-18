// Word wrap with mixed styles, shared by the document and slide layouts.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/office/doc.hpp"

namespace wetwaresuite::office::textlayout {

struct Piece {
    std::string text;  // a word, a run of spaces, or "\n"
    pdf::Font font;
    double size = 11;
    pdf::Color color{0x22, 0x22, 0x22, 255};
    bool underline = false;
    std::string link;
    double width = 0;
    bool space() const { return !text.empty() && text[0] == ' '; }
    bool newline() const { return text == "\n"; }
};

struct Line {
    std::vector<Piece> pieces;
    std::vector<double> xs;  // x offset of each piece
    double width = 0;
    double size = 11;  // largest font size on the line
};

struct Style {
    pdf::Font font;
    double size = 11;
    pdf::Color color{0x22, 0x22, 0x22, 255};
    pdf::Color link{0x05, 0x63, 0xC1, 255};
};

std::vector<Piece> pieces(const Runs& runs, const Style& st);
std::vector<Line> wrap(const std::vector<Piece>& in, double maxWidth, double baseSize);
// Adds the text ops of one line with its top at y (baseline at y + size * 0.92).
void drawLine(pdf::Page& page, const Line& l, double x, double y);

}  // namespace wetwaresuite::office::textlayout
