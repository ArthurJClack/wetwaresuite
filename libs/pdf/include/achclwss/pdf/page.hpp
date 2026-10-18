// A page as a display list (text, rectangles, lines, images) in points with
// the origin at the top-left corner. The same pages go to a PDF file and to
// a raster preview, so what Claude sees in a preview is what the PDF holds.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "achclwss/pdf/fonts.hpp"
#include "achclwss/raster/image.hpp"

namespace achclwss::pdf {

using raster::Color;

struct TextOp {
    double x = 0, y = 0;  // start of the baseline
    Font font;
    double size = 11;
    Color color{0, 0, 0, 255};
    std::string text;  // UTF-8, one line
    bool underline = false;
    std::string link;  // URI: the text becomes a clickable link in the PDF
};

struct RectOp {
    double x = 0, y = 0, w = 0, h = 0;
    std::optional<Color> fill;
    std::optional<Color> stroke;
    double lineWidth = 1;
};

struct LineOp {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    Color color{0, 0, 0, 255};
    double width = 1;
};

// Pixels for drawing previews, plus the original JPEG bytes when there are
// any so the PDF embeds them unchanged.
struct ImageData {
    raster::Image pixels;
    std::string jpeg;
};

struct ImageOp {
    double x = 0, y = 0, w = 0, h = 0;
    std::shared_ptr<const ImageData> image;
};

using Op = std::variant<TextOp, RectOp, LineOp, ImageOp>;

struct Page {
    double width = 595.28, height = 841.89;  // A4 portrait
    std::vector<Op> ops;

    void text(double x, double y, const std::string& s, Font font, double size, Color color = {0, 0, 0, 255});
    void rect(double x, double y, double w, double h, std::optional<Color> fill, std::optional<Color> stroke = std::nullopt,
              double lineWidth = 1);
    void line(double x1, double y1, double x2, double y2, Color color, double width = 1);
    void image(double x, double y, double w, double h, std::shared_ptr<const ImageData> img);
};

struct DocInfo {
    std::string title, author, subject;
};

// Loads PNG/JPEG/BMP/... bytes for an ImageOp (JPEG bytes are kept as-is). Throws std::runtime_error.
std::shared_ptr<ImageData> loadImageData(const std::string& bytes);

// A complete PDF 1.4 file: standard fonts, Flate-compressed content, JPEG or Flate images, link annotations.
std::string writePdf(const std::vector<Page>& pages, const DocInfo& info = {});

// Draws a page at `scale` pixels per point onto a new image.
raster::Image renderPage(const Page& page, double scale, Color background = {255, 255, 255, 255});

// Draws one line of text onto any image, at pixel coordinates (x, y = baseline), with the
// preview font (FreeSans/FreeSerif/FreeMono glyphs on the standard-font advances).
void drawText(raster::Image& img, double x, double y, const std::string& text, Font font, double sizePx, Color color);
double textWidthPx(const std::string& text, Font font, double sizePx);

// zlib streams (with header), shared by PDF, PNG-like and ZIP code.
std::string deflate(const std::string& data, int level = 6);
std::string inflate(const std::string& data);  // throws std::runtime_error on corrupt data

}  // namespace achclwss::pdf
