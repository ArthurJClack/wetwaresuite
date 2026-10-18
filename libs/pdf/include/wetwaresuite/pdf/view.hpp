// Drawing the pages of any PDF file (not only ones made here), finding where
// their text sits, and taking glyphs out of a page's content. Text is drawn
// with the built-in face closest to the file's own font (sans, serif or mono,
// bold, italic) at each glyph's real position, so pages keep their layout but
// not their exact typefaces. Shadings draw as one flat colour; JPEG 2000,
// JBIG2 and fax images draw as grey boxes.
//
// "Page points" below are points from the top-left corner of the page as it is
// shown: the crop box, turned by the page's /Rotate.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "wetwaresuite/pdf/fonts.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::pdf {

// A PDF matrix: x' = a x + c y + e, y' = b x + d y + f.
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    void apply(double x, double y, double& ox, double& oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    Matrix inverse() const;
};
// `first`, then `then` (PDF order: the new cm goes on the left).
Matrix operator*(const Matrix& first, const Matrix& then);

struct PageBox {
    double width = 0, height = 0;  // as shown, in points
    int rotate = 0;                // the page's own /Rotate (0, 90, 180, 270)
    Matrix toPage;                 // PDF user space -> page points
};
PageBox pageBox(const PdfFile& file, int index);

struct TextChar {
    std::string text;  // UTF-8, usually one character
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // page points
    double ox = 0, oy = 0;  // where the glyph starts on its baseline, page points
    double size = 0;   // font size in points
    Font font;         // the closest standard face
    raster::Color color{0, 0, 0, 255};
    int op = -1;    // the text-showing operator in the page's own content (-1: inside a form or pattern)
    int glyph = 0;  // the glyph within that operator
};

struct RenderOptions {
    raster::Color background{255, 255, 255, 255};
    bool annotations = true;
    // true leaves that annotation out (the object number is 0 for direct annotations)
    std::function<bool(int object, const Object& annot)> skipAnnotation;
    const std::string* content = nullptr;  // drawn instead of the page's own content (e.g. from removeGlyphs)
};

// The page as shown, `scale` pixels per point.
raster::Image renderPdfPage(const PdfFile& file, int index, double scale, const RenderOptions& options = {});

// Drawing on a rendered page, in pixels: filled outlines (nonzero winding, anti-aliased)
// and stroked polylines. Alpha 0..1 multiplies the colour's own alpha.
using Polygon = std::vector<std::array<double, 2>>;
void fillShapes(raster::Image& img, const std::vector<Polygon>& shapes, raster::Color color, double alpha = 1, bool evenOdd = false);
void strokeLines(raster::Image& img, const std::vector<Polygon>& lines, bool closed, double width, raster::Color color, double alpha = 1);

// Every glyph of the page in reading order of the content (not sorted).
// `content` stands in for the page's own content when given.
std::vector<TextChar> pageChars(const PdfFile& file, int index, const std::string* content = nullptr);

// The page's decoded content streams joined into one.
std::string pageContent(const PdfFile& file, int index);

// The page's content with these glyphs (TextChar op/glyph pairs) taken out; everything
// else stays where it was. Glyphs inside forms can't be taken out and are skipped.
std::string removeGlyphs(const PdfFile& file, int index, const std::vector<std::pair<int, int>>& glyphs,
                         const std::string* content = nullptr);

// Lines of text with the characters they came from: neighbouring glyphs joined, a space
// where there is a gap. `chars` maps each byte of `text` to its TextChar (-1 for added spaces).
struct TextLine {
    std::string text;
    std::vector<int> chars;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::vector<TextLine> textLines(const std::vector<TextChar>& chars);

struct TextMatch {
    int line = 0;
    std::vector<int> chars;                      // TextChar indices
    std::vector<std::array<double, 4>> boxes;    // x, y, w, h in page points (one per line piece)
    std::string text;
};
// Every place `query` appears (case-insensitive for ASCII unless matchCase).
std::vector<TextMatch> findText(const std::vector<TextChar>& chars, const std::string& query, bool matchCase = false);

}  // namespace wetwaresuite::pdf
