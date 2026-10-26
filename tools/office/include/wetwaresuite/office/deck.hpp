// Slide decks: slides with a layout preset, title, bullets, an image, speaker
// notes and free objects (text boxes, shapes, pictures, lines), a master, transitions
// and a theme. One layout engine places everything,
// and the same placement goes to PDF/PNG and to PowerPoint (.pptx).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/office/doc.hpp"
#include "wetwaresuite/pdf/page.hpp"

namespace wetwaresuite::office {

// Something placed freely on a slide (or on the master, so it shows on every slide):
// a text box, a shape (with optional text), a picture or a line. Positions are in points
// from the slide's top-left corner; a line runs from (x, y) to (x + w, y + h).
struct SlideObject {
    std::string kind = "shape";  // text, shape, image, line
    std::string name;            // optional label, unique on its slide
    std::string shape = "rect";  // see shapeNames()
    double x = 0, y = 0, w = 0, h = 0;
    // Paints: "" = none, "accent", "text", "background" (the theme's colours) or "#rrggbb".
    std::string fill, line;
    double lineWidth = 1.5;
    bool arrow = false;  // line: arrowhead at the end
    // text (inline Markdown, one paragraph per line; with bullets, the deck's bullet rules)
    std::string text;
    bool bullets = false;
    double size = 20;
    std::string color;  // "" = automatic (theme text, or readable on the fill)
    bool bold = false, italic = false;
    int align = 0;   // 0 left, 1 centre, 2 right
    int anchor = 0;  // 0 top, 1 middle, 2 bottom
    // picture (kind image): PNG or JPEG bytes
    std::string image;
    std::shared_ptr<const pdf::ImageData> pixels;
};

struct Slide {
    std::string layout = "content";  // see slideLayouts()
    std::string title, subtitle;
    std::vector<ListItem> bullets;
    std::vector<ListItem> right;  // second column of two_column
    std::string image;            // PNG or JPEG bytes
    std::shared_ptr<const pdf::ImageData> pixels;
    std::string notes;
    bool numbered = false;  // top-level bullets are numbered 1. 2. 3. instead of dots
    std::vector<SlideObject> objects;  // drawn over the layout, first one at the back
    std::string background;            // paint over the layout's own background ("" = the layout's)
    std::string transition;            // "" (none), fade, push, wipe, cover, zoom
    std::string direction = "left";    // push/wipe/cover: left, right, up, down
    double transitionSeconds = 0.75;
    double advanceAfter = 0;  // seconds before moving on by itself in a show; 0 = on click
    bool hidden = false;      // left out of shows and PDF exports
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
    // the master: shown on every slide except title and section slides (unless masterOnTitle)
    std::vector<SlideObject> master;
    std::string footer;        // footer text, bottom left
    bool slideNumbers = true;  // number in the bottom right corner
    bool masterOnTitle = false;
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
void setObjectImage(SlideObject& o, const std::string& bytes);  // throws std::runtime_error

// rect, rounded, ellipse, triangle, right_triangle, diamond, pentagon, hexagon, octagon, star,
// arrow_right, arrow_left, arrow_up, arrow_down, chevron, parallelogram, trapezoid, plus, callout
const std::vector<std::string>& shapeNames();
// Transition effects for shows and PowerPoint: fade, push, wipe, cover, zoom.
const std::vector<std::string>& transitionNames();
// A paint for SlideObject::fill/line/color: "none", "accent", "text", "background" or any colour; false when it isn't one.
bool parsePaint(const std::string& text, std::string& out);
// The height an object takes on a slide of `deck` (auto-height text boxes are measured).
double objectHeight(const Deck& deck, const SlideObject& o);
// Text that doesn't fit in the object even when shrunk.
bool objectOverflows(const Deck& deck, const SlideObject& o);
// The outline of a shape preset as points (closed polygon).
std::vector<std::pair<double, double>> shapeOutline(const std::string& shape, double x, double y, double w, double h);

pdf::Page layoutSlide(const Deck& deck, std::size_t index);
std::vector<pdf::Page> layoutDeck(const Deck& deck);  // every slide that isn't hidden
// Notes pages: the slide at the top, its speaker notes below (one portrait page per slide).
std::vector<pdf::Page> layoutNotesPages(const Deck& deck);
// A self-contained HTML slide show: arrow keys, click or space to advance, transitions,
// timings, F for full screen, N for speaker notes, P for a presenter window. Slides are
// drawn as images at `width` pixels.
std::string writeSlideShow(const Deck& deck, int width = 1600, bool loop = false);
// Text frames that do not fit even when shrunk ("title", "subtitle", "bullets", "right"); empty when all fit.
std::vector<std::string> slideOverflow(const Deck& deck, std::size_t index);

// What's at (x, y) points on a slide, for clicking it in a window: the topmost free object, else
// the layout's title, subtitle, bullets or right column (also where an empty one would go, like
// PowerPoint's "click to add title"). kind is "object", "title", "subtitle", "bullets", "right"
// or "" for nothing; object is the 1-based object number; text is what to edit.
struct SlideHit {
    std::string kind;
    int object = 0;
    double x = 0, y = 0, w = 0, h = 0;
    std::string text;
};
SlideHit slideHit(const Deck& deck, std::size_t index, double x, double y);

std::string writePptx(const Deck& deck);
// Titles, bullets, pictures, text boxes, shapes and lines, notes, transitions, hidden slides,
// the theme colours and fonts, and the master's shapes and footer.
Deck readPptx(const std::string& bytes);

}  // namespace wetwaresuite::office
