// Slide decks: slides with a layout preset, title, bullets, an image and
// speaker notes, plus a theme colour. One layout engine places everything,
// and the same placement goes to PDF/PNG and to PowerPoint (.pptx).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/pdf/page.hpp"

namespace wetwaresuite::office {

struct Slide {
    std::string layout = "content";  // see slideLayouts()
    std::string title, subtitle;
    std::vector<ListItem> bullets;
    std::vector<ListItem> right;  // second column of two_column
    std::string image;            // PNG or JPEG bytes
    std::shared_ptr<const pdf::ImageData> pixels;
    std::string notes;
    bool numbered = false;  // top-level bullets are numbered 1. 2. 3. instead of dots
};

struct Theme {
    std::string name = "blue";
    pdf::Color accent{0x2B, 0x57, 0x9A, 255};
    pdf::Color background{255, 255, 255, 255};
    pdf::Color text{0x22, 0x22, 0x22, 255};
    pdf::Family font = pdf::Family::Sans;
};

struct Deck {
    std::string title, author;
    std::vector<Slide> slides;
    Theme theme;
    double width = 960, height = 540;  // 16:9 in points (13.333 x 7.5 in)
};

// title, section, content, two_column, image, image_right, title_only, blank
const std::vector<std::string>& slideLayouts();
// Named presets (blue, teal, green, orange, red, purple, slate, dark) or any "#rrggbb" accent.
bool parseTheme(const std::string& spec, Theme& out, std::string& error);
const std::vector<std::string>& themeNames();

// Bullet text: one item per line; "- ", "* " or "1. " markers are optional and two spaces
// (or a tab) of indent make a sub-bullet. Inline Markdown is kept.
// numbered (when given) is set when the top-level lines are written "1. ...".
std::vector<ListItem> parseBullets(const std::string& text, bool* numbered = nullptr);
std::string bulletText(const std::vector<ListItem>& items, bool numbered = false);

void setSlideImage(Slide& s, const std::string& bytes);  // throws std::runtime_error

pdf::Page layoutSlide(const Deck& deck, std::size_t index);
std::vector<pdf::Page> layoutDeck(const Deck& deck);
// Text frames that do not fit even when shrunk ("title", "subtitle", "bullets", "right"); empty when all fit.
std::vector<std::string> slideOverflow(const Deck& deck, std::size_t index);

std::string writePptx(const Deck& deck);
Deck readPptx(const std::string& bytes);  // titles, bullets, first picture and notes of each slide

}  // namespace wetwaresuite::office
