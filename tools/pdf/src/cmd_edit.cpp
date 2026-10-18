// pdf.* commands that add to pages: text, replacing text, highlights, notes,
// drawing, shapes, white-out, redaction, pictures and signatures.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

#include "commands.hpp"

namespace wetwaresuite::pdf::tool {

namespace {

ParamSpec xSpec() { return number("x", "left edge, points from the page's left", true); }
ParamSpec ySpec() { return number("y", "top edge, points from the page's top", true); }
ParamSpec wSpec(bool required = true) { return number("w", "width in points", required); }
ParamSpec hSpec(bool required = true) { return number("h", "height in points", required); }
ParamSpec colorSpec(const char* fallback) { return str("color", "colour: a name, #rrggbb or rgb(r,g,b)", false, std::string(fallback)); }
ParamSpec opacitySpec() { return number("opacity", "0 to 1", false, 1.0); }

double opacityParam(const Params& p) { return std::clamp(numberParam(p, "opacity", 1), 0.0, 1.0); }

// "x y x y; x y x y" -> strokes
std::vector<std::vector<double>> parseStrokes(const std::string& text) {
    std::vector<std::vector<double>> out;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, ';')) {
        std::replace(part.begin(), part.end(), ',', ' ');
        std::stringstream ps(part);
        std::vector<double> pts;
        double v;
        while (ps >> v) pts.push_back(v);
        if (pts.size() >= 2) {
            if (pts.size() % 2) pts.pop_back();
            out.push_back(std::move(pts));
        }
    }
    return out;
}

std::vector<double> pointsToBase(const PageGeom& g, const std::vector<double>& pts) {
    std::vector<double> out;
    for (std::size_t k = 0; k + 1 < pts.size(); k += 2) {
        double bx, by;
        toBase(g, pts[k], pts[k + 1], bx, by);
        out.push_back(bx);
        out.push_back(by);
    }
    return out;
}

// The text boxes a highlight/redaction covers: text= matches, else the text inside the box
// (or the bare box when no text is there).
struct Target {
    std::vector<int> chars;
    std::vector<std::array<double, 4>> boxes;  // base page points
};

std::vector<std::pair<int, Target>> targets(const DocData& d, const Params& p, bool needText) {
    std::vector<std::pair<int, Target>> out;
    if (has(p, "text")) {
        const auto onePage = intParam(p, "page", 0);
        for (std::size_t i = 0; i < d.pages.size(); ++i) {
            if (onePage > 0 && static_cast<std::int64_t>(i) != onePage - 1) continue;
            const auto chars = charsOf(d, static_cast<int>(i));
            for (const auto& m : findText(chars, stringParam(p, "text"), boolParam(p, "match_case", false))) {
                Target t;
                t.chars = m.chars;
                t.boxes = m.boxes;
                out.emplace_back(static_cast<int>(i), std::move(t));
            }
        }
        if (out.empty()) throw std::runtime_error("\"" + stringParam(p, "text") + "\" isn't on " + (has(p, "page") && intParam(p, "page", 0) > 0 ? "that page" : "any page"));
        return out;
    }
    if (!has(p, "x") || !has(p, "y") || !has(p, "w") || !has(p, "h")) throw std::runtime_error("give text=, or the area as x, y, w and h");
    const int page = pageParam(d, p);
    const PageGeom g = geometry(d, page);
    const auto box = boxToBase(g, numberParam(p, "x"), numberParam(p, "y"), numberParam(p, "w"), numberParam(p, "h"));
    Target t;
    const auto chars = charsOf(d, page);
    t.chars = charsInBox(chars, box);
    if (!t.chars.empty() && needText) t.boxes = lineBoxes(chars, t.chars);
    else t.boxes = {box};
    out.emplace_back(page, std::move(t));
    return out;
}

ParamSpec areaPage() { return {"page", ParamType::Int, "page number (with text=: 0 = every page)", false, {}, std::int64_t{1}}; }

}  // namespace

void registerEditCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"pdf.add_text", "Add Text",
             "Write text on a page at x, y (its top-left corner, points from the page's top-left). Lines break at \\n; w= wraps to a width.",
             {docSpec(), pageSpec(), xSpec(), ySpec(), str("text", "what to write", true), number("size", "font size in points", false, 12.0),
              str("font", "typeface", false, std::string("sans"), {"sans", "serif", "mono"}), flag("bold", "bold", false), flag("italic", "italic", false),
              colorSpec("black"), number("w", "wrap to this width (0 = no wrapping)", false, 0.0), opacitySpec()},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const PageGeom g = geometry(d.data, page);
                           Mark m;
                           m.kind = "text";
                           toBase(g, numberParam(p, "x"), numberParam(p, "y"), m.x, m.y);
                           m.text = stringParam(p, "text");
                           for (std::size_t at = m.text.find("\\n"); at != std::string::npos; at = m.text.find("\\n", at + 1)) m.text.replace(at, 2, "\n");
                           m.size = std::clamp(numberParam(p, "size", 12), 1.0, 500.0);
                           m.font = fontParam(p);
                           m.color = colorParam(p, "color", {0, 0, 0, 255});
                           m.w = std::max(0.0, numberParam(p, "w", 0));
                           m.opacity = opacityParam(p);
                           double wmax = 0;
                           for (const auto& l : markLines(m)) wmax = std::max(wmax, textWidth(m.font, l, m.size));
                           if (m.w <= 0) m.w = 0;
                           m.h = static_cast<double>(markLines(m).size()) * lineHeight(m);
                           const Mark& added = addMark(d.data, page, m);
                           return markAdded(d.data, page, added, "added text").with("width", wmax).with("height", m.h);
                       },
                       true));

    reg.add({"pdf.replace_text", "Replace Text",
             "Edit the text of the page itself: every place `find` appears is taken out of the page and `replace` is written there "
             "in the closest built-in face, size and colour. page=0 does every page.",
             {docSpec(), str("find", "the text to change", true), str("replace", "what to put instead (empty deletes it)", false, std::string()),
              integer("page", "only this page (0 = all)", false, std::int64_t{0}), flag("match_case", "Aa must match", false),
              flag("all", "every match (false: only the first)", true)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const std::string find = stringParam(p, "find"), repl = stringParam(p, "replace");
                           const auto onePage = intParam(p, "page", 0);
                           const bool all = boolParam(p, "all", true);
                           int count = 0, stuck = 0;
                           for (std::size_t i = 0; i < d.data.pages.size(); ++i) {
                               if (onePage > 0 && static_cast<std::int64_t>(i) != onePage - 1) continue;
                               if (!all && count > 0) break;
                               const int page = static_cast<int>(i);
                               const auto chars = charsOf(d.data, page);
                               auto matches = findText(chars, find, boolParam(p, "match_case", false));
                               if (matches.empty()) continue;
                               if (!all) matches.resize(1);
                               std::vector<int> drop;
                               for (const auto& m : matches) {
                                   drop.insert(drop.end(), m.chars.begin(), m.chars.end());
                                   const TextChar& c = chars[static_cast<std::size_t>(m.chars.front())];
                                   const bool inForm = std::any_of(m.chars.begin(), m.chars.end(), [&](int k) { return chars[static_cast<std::size_t>(k)].op < 0; });
                                   if (inForm) {  // can't take it out: cover it
                                       Mark cover;
                                       cover.kind = "whiteout";
                                       cover.x = m.boxes[0][0] - 0.5;
                                       cover.y = m.boxes[0][1] - 0.5;
                                       cover.w = m.boxes[0][2] + 1;
                                       cover.h = m.boxes[0][3] + 1;
                                       cover.fill = Color{255, 255, 255, 255};
                                       addMark(d.data, page, cover);
                                       ++stuck;
                                   }
                                   if (!repl.empty()) {
                                       Mark t;
                                       t.kind = "text";
                                       t.text = repl;
                                       t.size = c.size > 0 ? c.size : 12;
                                       t.font = c.font;
                                       t.color = c.color;
                                       t.x = c.ox;
                                       t.y = c.oy - t.size * 0.8;
                                       t.h = lineHeight(t);
                                       addMark(d.data, page, t);
                                   }
                                   ++count;
                               }
                               dropGlyphs(d.data, page, chars, drop);
                           }
                           if (count == 0) return Result::failure("\"" + find + "\" isn't in the document's text");
                           std::string msg = "replaced " + std::to_string(count) + " place" + (count == 1 ? "" : "s");
                           if (stuck) msg += " (" + std::to_string(stuck) + " sat inside a form object and were covered with white instead)";
                           return Result::success(msg, static_cast<std::int64_t>(count)).with("count", static_cast<std::int64_t>(count));
                       },
                       true));

    reg.add({"pdf.highlight", "Highlight",
             "Highlight, underline or strike out text: text= marks every match, or x, y, w, h marks the text inside that area. "
             "Saved as real PDF comments (comment= adds a note to it).",
             {docSpec(), areaPage(), str("text", "text to mark", false), number("x", "area left", false), number("y", "area top", false), wSpec(false),
              hSpec(false), str("kind", "how to mark it", false, std::string("highlight"), {"highlight", "underline", "strikeout"}),
              str("color", "colour (default yellow; red for strikeout)", false), str("comment", "a note shown with it", false), flag("match_case", "Aa must match", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const std::string kind = stringParam(p, "kind", "highlight");
                           const Color def = kind == "highlight" ? Color{255, 225, 0, 255} : kind == "strikeout" ? Color{220, 30, 30, 255} : Color{20, 110, 220, 255};
                           int count = 0, last = 0, lastPage = 0;
                           for (auto& [page, t] : targets(d.data, p, true)) {
                               Mark m;
                               m.kind = kind;
                               m.boxes = t.boxes;
                               m.color = colorParam(p, "color", def);
                               m.text = stringParam(p, "comment");
                               last = addMark(d.data, page, m).id;
                               lastPage = page;
                               ++count;
                           }
                           return Result::success(kind + " x" + std::to_string(count) + " (last edit " + std::to_string(last) + " on page " + std::to_string(lastPage + 1) + ")",
                                                  static_cast<std::int64_t>(last))
                               .with("mark", static_cast<std::int64_t>(last))
                               .with("count", static_cast<std::int64_t>(count));
                       },
                       true));

    reg.add({"pdf.note", "Sticky Note", "Pin a comment (sticky note) on a page at x, y.",
             {docSpec(), pageSpec(), xSpec(), ySpec(), str("text", "the comment", true), colorSpec("yellow"), str("author", "who wrote it", false)}, true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           Mark m;
                           m.kind = "note";
                           toBase(geometry(d.data, page), numberParam(p, "x"), numberParam(p, "y"), m.x, m.y);
                           m.text = stringParam(p, "text");
                           m.color = colorParam(p, "color", {255, 210, 0, 255});
                           m.author = stringParam(p, "author");
                           return markAdded(d.data, page, addMark(d.data, page, m), "added a note");
                       },
                       true));

    reg.add({"pdf.draw", "Draw", "Freehand ink: points=[x1,y1,x2,y2,...] for one stroke, or strokes=\"x y x y; x y x y\" for several.",
             {docSpec(), pageSpec(), list("points", "one stroke: x1,y1,x2,y2,...", false), str("strokes", "several strokes separated by ;", false), colorSpec("red"),
              number("width", "line width in points", false, 2.0), opacitySpec()},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const PageGeom g = geometry(d.data, page);
                           Mark m;
                           m.kind = "ink";
                           if (has(p, "points")) m.strokes.push_back(pointsToBase(g, listParam(p, "points")));
                           for (const auto& s : parseStrokes(stringParam(p, "strokes"))) m.strokes.push_back(pointsToBase(g, s));
                           m.strokes.erase(std::remove_if(m.strokes.begin(), m.strokes.end(), [](const auto& s) { return s.size() < 2; }), m.strokes.end());
                           if (m.strokes.empty()) return Result::failure("give points=[x1,y1,x2,y2,...] or strokes=\"x y x y; ...\"");
                           m.color = colorParam(p, "color", {220, 30, 30, 255});
                           m.width = std::clamp(numberParam(p, "width", 2), 0.1, 100.0);
                           m.opacity = opacityParam(p);
                           return markAdded(d.data, page, addMark(d.data, page, m), "drew " + std::to_string(m.strokes.size()) + " stroke(s)");
                       },
                       true));

    reg.add({"pdf.shape", "Shape", "A rectangle, ellipse, line or arrow. Lines and arrows go from (x, y) to (x + w, y + h).",
             {docSpec(), pageSpec(), str("kind", "shape", false, std::string("rect"), {"rect", "ellipse", "line", "arrow"}), xSpec(), ySpec(), wSpec(), hSpec(),
              colorSpec("red"), str("fill", "fill colour (none by default)", false), number("width", "line width in points (0 = no outline)", false, 2.0),
              opacitySpec()},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const PageGeom g = geometry(d.data, page);
                           Mark m;
                           m.kind = stringParam(p, "kind", "rect");
                           const double x = numberParam(p, "x"), y = numberParam(p, "y"), w = numberParam(p, "w"), h = numberParam(p, "h");
                           if (m.kind == "line" || m.kind == "arrow") {
                               m.points = pointsToBase(g, {x, y, x + w, y + h});
                           } else {
                               const auto b = boxToBase(g, x, y, w, h);
                               m.x = b[0];
                               m.y = b[1];
                               m.w = b[2];
                               m.h = b[3];
                           }
                           m.color = colorParam(p, "color", {220, 30, 30, 255});
                           m.fill = optColorParam(p, "fill");
                           m.width = std::clamp(numberParam(p, "width", 2), 0.0, 100.0);
                           m.opacity = opacityParam(p);
                           return markAdded(d.data, page, addMark(d.data, page, m), "added a " + m.kind);
                       },
                       true));

}

void registerCoverCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"pdf.whiteout", "White-out", "Cover an area with white (or another colour), e.g. to hide something before writing over it. "
                                           "The text underneath stays in the file: use pdf.redact to remove it.",
             {docSpec(), pageSpec(), xSpec(), ySpec(), wSpec(), hSpec(), colorSpec("white")}, true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const auto b = boxToBase(geometry(d.data, page), numberParam(p, "x"), numberParam(p, "y"), numberParam(p, "w"), numberParam(p, "h"));
                           Mark m;
                           m.kind = "whiteout";
                           m.x = b[0];
                           m.y = b[1];
                           m.w = b[2];
                           m.h = b[3];
                           m.fill = colorParam(p, "color", {255, 255, 255, 255});
                           return markAdded(d.data, page, addMark(d.data, page, m), "whited out an area");
                       },
                       true));

    reg.add({"pdf.redact", "Redact",
             "Black out text for good: text= finds every match, or x, y, w, h covers an area. The text is taken out of the file, not "
             "just covered (pictures under the box are only covered).",
             {docSpec(), areaPage(), str("text", "text to remove everywhere", false), number("x", "area left", false), number("y", "area top", false), wSpec(false),
              hSpec(false), colorSpec("black"), flag("match_case", "Aa must match", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           int count = 0, stuck = 0;
                           for (auto& [page, t] : targets(d.data, p, false)) {
                               const auto chars = charsOf(d.data, page);
                               // the area's own glyphs: for text= matches, the matched ones
                               std::vector<int> which = t.chars;
                               if (!has(p, "text")) which = charsInBox(chars, t.boxes[0]);
                               stuck += dropGlyphs(d.data, page, chars, which);
                               for (const auto& b : t.boxes) {
                                   Mark m;
                                   m.kind = "redact";
                                   m.x = b[0] - 0.5;
                                   m.y = b[1] - 0.5;
                                   m.w = b[2] + 1;
                                   m.h = b[3] + 1;
                                   m.fill = colorParam(p, "color", {0, 0, 0, 255});
                                   addMark(d.data, page, m);
                               }
                               ++count;
                           }
                           std::string msg = "redacted " + std::to_string(count) + " place" + (count == 1 ? "" : "s");
                           if (stuck) msg += " (" + std::to_string(stuck) + " glyph(s) inside form objects are covered but still in the file)";
                           return Result::success(msg, static_cast<std::int64_t>(count)).with("count", static_cast<std::int64_t>(count));
                       },
                       true));

    reg.add({"pdf.image", "Place Picture", "Put a picture (PNG, JPEG, ...) on a page; w or h alone keeps its shape, neither uses 72 dpi.",
             {docSpec(), pageSpec(), str("path", "the picture file", true), xSpec(), ySpec(), number("w", "width in points", false, 0.0),
              number("h", "height in points", false, 0.0), opacitySpec()},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           Mark m;
                           m.kind = "image";
                           m.image = loadPicture(stringParam(p, "path"));
                           const double iw = m.image->pixels.width(), ih = m.image->pixels.height();
                           double w = numberParam(p, "w", 0), h = numberParam(p, "h", 0);
                           if (w <= 0 && h <= 0) {
                               w = iw;
                               h = ih;
                           } else if (w <= 0) {
                               w = h * iw / ih;
                           } else if (h <= 0) {
                               h = w * ih / iw;
                           }
                           const auto b = boxToBase(geometry(d.data, page), numberParam(p, "x"), numberParam(p, "y"), w, h);
                           m.x = b[0];
                           m.y = b[1];
                           m.w = b[2];
                           m.h = b[3];
                           m.opacity = opacityParam(p);
                           return markAdded(d.data, page, addMark(d.data, page, m), "placed a picture");
                       },
                       true));

    reg.add({"pdf.sign", "Sign",
             "Put a signature on a page: name= writes it in a signature style, image= uses a picture of your signature, or strokes= "
             "draws it (\"x y x y; ...\" in points, relative to x, y). date=true writes today's date under it. (A drawn signature, "
             "not a certificate-based digital signature.)",
             {docSpec(), pageSpec(), xSpec(), ySpec(), number("w", "width in points", false, 160.0), number("h", "height in points", false, 50.0),
              str("name", "type the signature", false), str("image", "a picture of the signature", false), str("strokes", "drawn signature strokes", false),
              colorSpec("navy"), flag("date", "write today's date under it", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const PageGeom g = geometry(d.data, page);
                           const double x = numberParam(p, "x"), y = numberParam(p, "y");
                           const double w = std::max(10.0, numberParam(p, "w", 160)), h = std::max(10.0, numberParam(p, "h", 50));
                           Mark m;
                           m.kind = "signature";
                           m.color = colorParam(p, "color", {20, 40, 110, 255});
                           const auto b = boxToBase(g, x, y, w, h);
                           m.x = b[0];
                           m.y = b[1];
                           m.w = b[2];
                           m.h = b[3];
                           if (has(p, "image")) {
                               m.image = loadPicture(stringParam(p, "image"));
                               const double ar = static_cast<double>(m.image->pixels.width()) / std::max(1, m.image->pixels.height());
                               if (m.w / m.h > ar) m.w = m.h * ar;
                               else m.h = m.w / ar;
                           } else if (has(p, "strokes")) {
                               for (auto s : parseStrokes(stringParam(p, "strokes"))) {
                                   for (std::size_t k = 0; k + 1 < s.size(); k += 2) {
                                       s[k] += x;
                                       s[k + 1] += y;
                                   }
                                   m.strokes.push_back(pointsToBase(g, s));
                               }
                               m.width = 1.6;
                           } else if (has(p, "name")) {
                               m.text = stringParam(p, "name");
                               const Font f{Family::Serif, false, true};
                               m.size = std::min(h * 0.7, 36.0);
                               const double tw = textWidth(f, m.text, m.size);
                               if (tw > w && tw > 0) m.size *= w / tw;
                           } else {
                               return Result::failure("give name= (typed), image= (a picture) or strokes= (drawn)");
                           }
                           const Mark& added = addMark(d.data, page, m);
                           Result r = markAdded(d.data, page, added, "signed");
                           if (boolParam(p, "date", false)) {
                               std::time_t now = std::time(nullptr);
                               std::tm tmv{};
#ifdef _WIN32
                               localtime_s(&tmv, &now);
#else
                               localtime_r(&now, &tmv);
#endif
                               char buf[32];
                               std::strftime(buf, sizeof buf, "%Y-%m-%d", &tmv);
                               Mark t;
                               t.kind = "text";
                               t.text = buf;
                               t.size = 9;
                               t.color = {80, 80, 80, 255};
                               toBase(g, x, y + h + 2, t.x, t.y);
                               t.h = lineHeight(t);
                               addMark(d.data, page, t);
                           }
                           return r;
                       },
                       true));
}

}  // namespace wetwaresuite::pdf::tool
