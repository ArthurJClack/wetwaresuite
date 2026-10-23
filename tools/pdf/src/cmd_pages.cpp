// pdf.* commands for the edits themselves (list, move, change, delete), the
// file's own comments, and pages (turn, delete, move, insert).
#include <algorithm>
#include <cmath>

#include "commands.hpp"

namespace wetwaresuite::pdf::tool {

namespace {

ParamSpec markSpec() { return integer("mark", "the edit's number (pdf.marks lists them)", true); }

Mark& markParam(DocData& d, const Params& p, int* page) {
    const auto id = intParam(p, "mark", 0);
    Mark* m = findMark(d, static_cast<int>(id), page);
    if (!m) throw std::runtime_error("no edit " + std::to_string(id) + " (pdf.marks lists them)");
    return *m;
}

// The mark's box as shown now: x, y, w, h.
std::array<double, 4> shownBox(const DocData& d, int page, const Mark& m) {
    double x0 = m.x, y0 = m.y, x1 = m.x + m.w, y1 = m.y + m.h;
    auto add = [&](double x, double y, bool first) {
        if (first) {
            x0 = x1 = x;
            y0 = y1 = y;
        }
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    };
    bool first = true;
    for (const auto& b : m.boxes) {
        add(b[0], b[1], first);
        add(b[0] + b[2], b[1] + b[3], false);
        first = false;
    }
    for (const auto& s : m.strokes) {
        for (std::size_t k = 0; k + 1 < s.size(); k += 2) {
            add(s[k], s[k + 1], first);
            first = false;
        }
    }
    for (std::size_t k = 0; k + 1 < m.points.size(); k += 2) {
        add(m.points[k], m.points[k + 1], first);
        first = false;
    }
    if (m.kind == "note") {
        x1 = x0 + 18;
        y1 = y0 + 18;
    }
    if (m.kind == "text" && m.w <= 0) {
        double wmax = 0;
        for (const auto& l : markLines(m)) wmax = std::max(wmax, textWidth(m.font, l, m.size));
        x1 = x0 + wmax;
    }
    return boxFromBase(geometry(d, page), x0, y0, x1 - x0, y1 - y0);
}

void moveMark(Mark& m, double dx, double dy) {
    m.x += dx;
    m.y += dy;
    for (auto& b : m.boxes) {
        b[0] += dx;
        b[1] += dy;
    }
    for (auto& s : m.strokes) {
        for (std::size_t k = 0; k + 1 < s.size(); k += 2) {
            s[k] += dx;
            s[k + 1] += dy;
        }
    }
    for (std::size_t k = 0; k + 1 < m.points.size(); k += 2) {
        m.points[k] += dx;
        m.points[k + 1] += dy;
    }
}

}  // namespace

void registerMarkCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"pdf.marks", "List Edits", "The edits made here (text, highlights, notes, drawings, ...): number, page, kind, x, y, w, h, text (rows).",
             {docSpec(), integer("page", "only this page (0 = all)", false, std::int64_t{0})}, false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const auto only = intParam(p, "page", 0);
                           std::string out, rows;
                           int n = 0;
                           for (std::size_t i = 0; i < d.data.pages.size(); ++i) {
                               if (only > 0 && static_cast<std::int64_t>(i) != only - 1) continue;
                               for (const auto& m : d.data.pages[i].marks) {
                                   const auto b = shownBox(d.data, static_cast<int>(i), m);
                                   std::string text = m.text;
                                   std::replace(text.begin(), text.end(), '\n', ' ');
                                   std::replace(text.begin(), text.end(), '\t', ' ');
                                   rows += std::to_string(m.id) + "\t" + std::to_string(i + 1) + "\t" + m.kind + "\t" + fmt(b[0]) + "\t" + fmt(b[1]) + "\t" + fmt(b[2]) +
                                           "\t" + fmt(b[3]) + "\t" + text + "\n";
                                   if (n < 60) {
                                       out += "\n  " + std::to_string(m.id) + ": page " + std::to_string(i + 1) + " " + m.kind + " at " + fmt(b[0]) + ", " + fmt(b[1]) +
                                              (text.empty() ? "" : " \"" + text.substr(0, 40) + "\"");
                                   }
                                   ++n;
                               }
                           }
                           return Result::success(std::to_string(n) + " edit(s)" + out, static_cast<std::int64_t>(n)).with("total", static_cast<std::int64_t>(n)).with("rows", rows);
                       },
                       false));

    reg.add({"pdf.mark_move", "Move Edit", "Move an edit: x, y is its new top-left corner, or dx, dy shift it. page= moves it to another page.",
             {docSpec(), markSpec(), number("x", "new left", false), number("y", "new top", false), number("dx", "shift right", false), number("dy", "shift down", false),
              integer("page", "move it to this page", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           int page = 0;
                           Mark& m = markParam(d.data, p, &page);
                           const auto b = shownBox(d.data, page, m);
                           double nx = b[0] + numberParam(p, "dx", 0), ny = b[1] + numberParam(p, "dy", 0);
                           if (has(p, "x")) nx = numberParam(p, "x");
                           if (has(p, "y")) ny = numberParam(p, "y");
                           const PageGeom g = geometry(d.data, page);
                           double bx0, by0, bx1, by1;
                           toBase(g, b[0], b[1], bx0, by0);
                           toBase(g, nx, ny, bx1, by1);
                           moveMark(m, bx1 - bx0, by1 - by0);
                           const int id = m.id;
                           if (has(p, "page")) {
                               const int to = pageParam(d.data, p);
                               if (to != page) {
                                   auto& from = d.data.pages[static_cast<std::size_t>(page)].marks;
                                   auto it = std::find_if(from.begin(), from.end(), [&](const Mark& k) { return k.id == id; });
                                   Mark copy = *it;
                                   from.erase(it);
                                   d.data.pages[static_cast<std::size_t>(to)].marks.push_back(std::move(copy));
                                   page = to;
                               }
                           }
                           return Result::success("moved edit " + std::to_string(id) + " to " + fmt(nx) + ", " + fmt(ny) + " on page " + std::to_string(page + 1));
                       },
                       true));

    reg.add({"pdf.mark_edit", "Change Edit", "Change an edit's text, colour, fill, size, line width or opacity.",
             {docSpec(), markSpec(), str("text", "new text", false), str("color", "colour", false), str("fill", "fill colour (none = no fill)", false),
              number("size", "font size", false), number("width", "line width, or the text box width", false), number("h", "height (boxes, pictures)", false),
              number("opacity", "0 to 1", false)},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           int page = 0;
                           Mark& m = markParam(d.data, p, &page);
                           if (has(p, "text")) m.text = stringParam(p, "text");
                           if (has(p, "color")) m.color = colorParam(p, "color", m.color);
                           if (has(p, "fill")) {
                               if (utf8Lower(stringParam(p, "fill")) == "none") m.fill.reset();
                               else m.fill = optColorParam(p, "fill");
                           }
                           if (has(p, "size")) m.size = std::clamp(numberParam(p, "size"), 1.0, 500.0);
                           if (has(p, "width")) {
                               if (m.kind == "text" || m.kind == "image" || m.kind == "signature" || m.kind == "rect" || m.kind == "ellipse" ||
                                   m.kind == "whiteout" || m.kind == "redact")
                                   m.w = std::max(0.0, numberParam(p, "width"));
                               else m.width = std::max(0.0, numberParam(p, "width"));
                           }
                           if (has(p, "h")) m.h = std::max(0.0, numberParam(p, "h"));
                           if (has(p, "opacity")) m.opacity = std::clamp(numberParam(p, "opacity"), 0.0, 1.0);
                           if (m.kind == "text") m.h = static_cast<double>(markLines(m).size()) * lineHeight(m);
                           return Result::success("changed edit " + std::to_string(m.id));
                       },
                       true));

    reg.add({"pdf.mark_delete", "Delete Edit", "Remove an edit made here (pdf.marks lists them; mark=all clears a page or the whole document).",
             {docSpec(), str("mark", "the edit's number, or all", true), integer("page", "with mark=all: only this page (0 = every page)", false, std::int64_t{0})},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const std::string which = stringParam(p, "mark");
                           if (utf8Lower(which) == "all") {
                               const auto only = intParam(p, "page", 0);
                               std::size_t n = 0;
                               for (std::size_t i = 0; i < d.data.pages.size(); ++i) {
                                   if (only > 0 && static_cast<std::int64_t>(i) != only - 1) continue;
                                   n += d.data.pages[i].marks.size();
                                   d.data.pages[i].marks.clear();
                               }
                               return Result::success("removed " + std::to_string(n) + " edit(s)");
                           }
                           int id = 0;
                           try {
                               id = std::stoi(which);
                           } catch (const std::exception&) {
                               return Result::failure("mark is an edit number or all");
                           }
                           for (auto& pg : d.data.pages) {
                               auto it = std::find_if(pg.marks.begin(), pg.marks.end(), [&](const Mark& m) { return m.id == id; });
                               if (it != pg.marks.end()) {
                                   pg.marks.erase(it);
                                   return Result::success("removed edit " + std::to_string(id));
                               }
                           }
                           return Result::failure("no edit " + which + " (pdf.marks lists them)");
                       },
                       true));

    reg.add({"pdf.annotations", "File Comments",
             "Comments and other annotations already in the file on a page: id, kind, text (rows). pdf.annotation_delete removes one.",
             {docSpec(), pageSpec()}, false, true},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           const PageEntry& e = d.data.pages[static_cast<std::size_t>(page)];
                           if (e.file < 0) return Result::success("no comments (a new page)", std::int64_t{0}).with("rows", std::string());
                           const PdfFile& f = *d.data.files[static_cast<std::size_t>(e.file)];
                           const Object* a = f.pages()[static_cast<std::size_t>(e.index)].dict.get("Annots");
                           std::string out, rows;
                           int n = 0;
                           if (a) {
                               for (const auto& it : f.resolve(*a).items) {
                                   if (it.kind != Object::Ref || e.dropAnnots.count(static_cast<int>(it.number))) continue;
                                   const Object& an = f.resolve(it);
                                   const std::string sub = an.get("Subtype") ? f.resolve(*an.get("Subtype")).str : "";
                                   if (sub == "Popup") continue;
                                   std::string text;
                                   if (const Object* c = an.get("Contents")) text = decodeTextString(f.resolve(*c).str);
                                   if (sub == "Link") {
                                       if (const Object* act = an.get("A")) {
                                           if (const Object* uri = f.resolve(*act).get("URI")) text = f.resolve(*uri).str;
                                       }
                                   }
                                   std::replace(text.begin(), text.end(), '\n', ' ');
                                   std::replace(text.begin(), text.end(), '\t', ' ');
                                   rows += std::to_string(static_cast<int>(it.number)) + "\t" + sub + "\t" + text + "\n";
                                   out += "\n  " + std::to_string(static_cast<int>(it.number)) + ": " + sub + (text.empty() ? "" : " \"" + text.substr(0, 60) + "\"");
                                   ++n;
                               }
                           }
                           return Result::success(std::to_string(n) + " annotation(s) on page " + std::to_string(page + 1) + out, static_cast<std::int64_t>(n))
                               .with("total", static_cast<std::int64_t>(n))
                               .with("rows", rows);
                       },
                       false));

    reg.add({"pdf.annotation_delete", "Delete File Comment", "Remove a comment, link or other annotation the file already had (pdf.annotations lists them).",
             {docSpec(), pageSpec(), integer("id", "its id from pdf.annotations", true)}, true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int page = pageParam(d.data, p);
                           PageEntry& e = d.data.pages[static_cast<std::size_t>(page)];
                           const int id = static_cast<int>(intParam(p, "id", 0));
                           if (e.file < 0) return Result::failure("that page has no comments of its own");
                           const PdfFile& f = *d.data.files[static_cast<std::size_t>(e.file)];
                           bool found = false;
                           if (const Object* a = f.pages()[static_cast<std::size_t>(e.index)].dict.get("Annots")) {
                               for (const auto& it : f.resolve(*a).items) found = found || (it.kind == Object::Ref && static_cast<int>(it.number) == id);
                           }
                           if (!found) return Result::failure("page " + std::to_string(page + 1) + " has no annotation " + std::to_string(id));
                           e.dropAnnots.insert(id);
                           // its popup goes with it
                           const Object& an = f.object(id);
                           if (const Object* pop = an.get("Popup")) {
                               if (pop->kind == Object::Ref) e.dropAnnots.insert(static_cast<int>(pop->number));
                           }
                           return Result::success("removed annotation " + std::to_string(id));
                       },
                       true));
}

void registerPageCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"pdf.page_rotate", "Turn Pages", "Turn pages clockwise (90, 180, 270 or -90).",
             {docSpec(), str("pages", "which pages, e.g. 1-3,5 or all", false, std::string("all")), integer("angle", "degrees clockwise", false, std::int64_t{90})},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const auto angle = intParam(p, "angle", 90);
                           if (angle % 90 != 0) return Result::failure("angle must be a multiple of 90");
                           const auto pages = pagesParam(d.data, p);
                           for (int i : pages) {
                               auto& e = d.data.pages[static_cast<std::size_t>(i)];
                               e.rotate = static_cast<int>((((e.rotate + angle) % 360) + 360) % 360);
                           }
                           return Result::success("turned " + std::to_string(pages.size()) + " page(s) by " + std::to_string(angle));
                       },
                       true));

    reg.add({"pdf.page_delete", "Delete Pages", "Take pages out of the document.", {docSpec(), str("pages", "which pages, e.g. 2 or 4-6", true)}, true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           auto pages = pagesParam(d.data, p);
                           std::sort(pages.begin(), pages.end());
                           pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
                           if (pages.size() >= d.data.pages.size()) return Result::failure("that would leave no pages");
                           for (auto it = pages.rbegin(); it != pages.rend(); ++it) d.data.pages.erase(d.data.pages.begin() + *it);
                           return Result::success("deleted " + std::to_string(pages.size()) + " page(s); " + std::to_string(d.data.pages.size()) + " left")
                               .with("pages", static_cast<std::int64_t>(d.data.pages.size()));
                       },
                       true));

    reg.add({"pdf.page_move", "Move Page", "Move a page so it becomes page `to`.",
             {docSpec(), pageSpec(true), integer("to", "its new page number", true)}, true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           const int from = pageParam(d.data, p);
                           const int to = pageParam(d.data, p, "to");
                           PageEntry e = d.data.pages[static_cast<std::size_t>(from)];
                           d.data.pages.erase(d.data.pages.begin() + from);
                           d.data.pages.insert(d.data.pages.begin() + to, std::move(e));
                           return Result::success("moved page " + std::to_string(from + 1) + " to " + std::to_string(to + 1));
                       },
                       true));

    reg.add({"pdf.page_insert", "Insert Pages",
             "Insert pages from another PDF (path=, pages= picks some) or blank pages (count=) after page `after` (0 = at the start).",
             {docSpec(), integer("after", "insert after this page (0 = before the first)", false, std::int64_t{-1}), str("path", "a PDF to take pages from", false),
              str("pages", "which of its pages (default all)", false, std::string("all")), integer("count", "blank pages to add (without path)", false, std::int64_t{1})},
             true, false},
            docCommand(session,
                       [](PdfDoc& d, const Params& p, const std::string&) -> Result {
                           auto after = intParam(p, "after", -1);
                           if (after < 0 || after > static_cast<std::int64_t>(d.data.pages.size())) after = static_cast<std::int64_t>(d.data.pages.size());
                           std::vector<PageEntry> add;
                           if (has(p, "path")) {
                               auto f = std::make_shared<const PdfFile>(PdfFile::load(pathFromUtf8(stringParam(p, "path"))));
                               if (f->encrypted()) return Result::failure("that PDF is password-protected");
                               const int fi = static_cast<int>(d.data.files.size());
                               d.data.files.push_back(f);
                               for (int i : parsePageList(stringParam(p, "pages", "all"), f->pageCount())) {
                                   PageEntry e;
                                   e.file = fi;
                                   e.index = i;
                                   add.push_back(std::move(e));
                               }
                           } else {
                               const auto n = intParam(p, "count", 1);
                               if (n < 1 || n > 500) return Result::failure("count must be 1 to 500");
                               const PageGeom g = geometry(d.data, std::max(0, static_cast<int>(after) - 1));
                               for (std::int64_t i = 0; i < n; ++i) {
                                   PageEntry e;
                                   e.file = -1;
                                   e.width = g.width;
                                   e.height = g.height;
                                   add.push_back(e);
                               }
                           }
                           d.data.pages.insert(d.data.pages.begin() + after, add.begin(), add.end());
                           return Result::success("inserted " + std::to_string(add.size()) + " page(s) after page " + std::to_string(after) + "; " +
                                                  std::to_string(d.data.pages.size()) + " in all")
                               .with("pages", static_cast<std::int64_t>(d.data.pages.size()));
                       },
                       true));
}

}  // namespace wetwaresuite::pdf::tool
