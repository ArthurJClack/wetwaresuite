// The PowerPoint-style deck features and office.deck_* commands: text boxes, shapes,
// lines and pictures on slides and on the master, footer and slide numbers, themes,
// transitions and timings, hidden slides, notes pages, the HTML slide show, and the
// PPTX round trip of all of it. Headless; needs no LibreOffice.
#include <cstdio>
#include <filesystem>
#include <random>

#include "wetwaresuite/office/deck.hpp"
#include "wetwaresuite/office/office.hpp"
#include "wetwaresuite/pdf/reader.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ora.hpp"
#include "check.hpp"
#include "commands.hpp"
#include "text.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::office;
namespace fs = std::filesystem;

namespace {

fs::path tmpDir() {
    static const fs::path dir = fs::temp_directory_path() / ("wetwaresuite_deckcmd_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    return dir;
}

Params P(std::initializer_list<std::pair<const std::string, Value>> l) { return Params(l); }
Value S(const char* s) { return std::string(s); }
Value S(const std::string& s) { return s; }
Value I(std::int64_t v) { return v; }
Value N(double v) { return v; }
Value B(bool v) { return v; }

Deck& deckOf(Session& s, const char* id = "deck1") { return s.workspace().getAs<DeckDoc>(id)->data; }

Result run(Session& s, const std::string& id, Params p) {
    Result r = s.run(id, p);
    if (!r.ok) std::printf("  %s failed: %s\n", id.c_str(), r.message.c_str());
    return r;
}

std::string pngFile(const std::string& name, int w, int h, raster::Color c) {
    const std::string path = (tmpDir() / name).string();
    writeBytes(path, raster::encodePng(raster::Image(w, h, c)));
    return path;
}

std::string pageText(const pdf::Page& page) {
    pdf::PdfFile f(pdf::writePdf({page}));
    return f.pageText(0);
}

template <typename Op>
int count(const pdf::Page& page) {
    int n = 0;
    for (const auto& op : page.ops) n += std::holds_alternative<Op>(op) ? 1 : 0;
    return n;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void testModel() {
    for (const auto& name : shapeNames()) {
        auto pts = shapeOutline(name, 100, 50, 200, 100);
        CHECK(pts.size() >= 3);
        bool inside = true;
        for (const auto& [x, y] : pts) inside = inside && x >= 99.9 && x <= 300.1 && y >= 49.9 && y <= (name == "callout" ? 163 : 150.1);
        if (!inside) std::printf("  shape %s leaves its box\n", name.c_str());
        CHECK(inside);
    }
    std::string out;
    CHECK(parsePaint("none", out) && out.empty());
    CHECK(parsePaint("Accent", out) && out == "accent");
    CHECK(parsePaint("red", out) && out == "#ff0000");
    CHECK(parsePaint("#1F6F3F", out) && out == "#1f6f3f");
    CHECK(!parsePaint("nonsense", out));

    // objects draw over the layout: a shape is a path, its text is text
    Deck d;
    Slide s;
    s.layout = "blank";
    SlideObject o;
    o.kind = "shape";
    o.shape = "ellipse";
    o.x = 100, o.y = 100, o.w = 200, o.h = 100;
    o.fill = "accent";
    o.text = "Inside";
    s.objects.push_back(o);
    SlideObject t;
    t.kind = "text";
    t.x = 400, t.y = 100, t.w = 300, t.h = 0;
    t.text = "Auto height\nsecond line";
    s.objects.push_back(t);
    d.slides.push_back(s);
    pdf::Page page = layoutSlide(d, 0);
    CHECK(count<pdf::PathOp>(page) == 1);
    CHECK(contains(pageText(page), "Inside") && contains(pageText(page), "second line"));
    CHECK(objectHeight(d, t) > 40 && objectHeight(d, t) < 80);
    // text that can't fit is reported
    SlideObject tiny = o;
    tiny.w = 40, tiny.h = 20;
    tiny.text = std::string(400, 'x') + " words that will not fit";
    d.slides[0].objects.push_back(tiny);
    CHECK(objectOverflows(d, tiny));
    CHECK(slideOverflow(d, 0) == std::vector<std::string>{"object 3"});
    // the PDF writer draws paths
    pdf::PdfFile f(pdf::writePdf({page}));
    CHECK(f.pageCount() == 1);
    raster::Image img = pdf::renderPage(page, 1);
    CHECK(img.at(200, 150) == raster::Color(d.theme.accent));  // the ellipse's middle
    CHECK(img.at(105, 105) == raster::Color(d.theme.background));  // outside its curve
}

void build(Session& s, const std::string& logo, const std::string& pic) {
    run(s, "office.deck_new", P({{"title", S("Launch")}, {"theme", S("teal")}}));
    run(s, "office.deck_add_slide", P({{"layout", S("title")}, {"title", S("Launch")}, {"subtitle", S("Q4")}}));
    run(s, "office.deck_add_slide", P({{"title", S("Steps")}, {"bullets", S("Build\nTest\nShip")}, {"notes", S("Say </script> safely")}}));
    run(s, "office.deck_add_slide", P({{"layout", S("blank")}}));
    run(s, "office.deck_shape", P({{"slide", I(3)}, {"shape", S("chevron")}, {"x", N(60)}, {"y", N(200)}, {"w", N(260)}, {"h", N(110)}, {"text", S("Build")}}));
    run(s, "office.deck_shape", P({{"slide", I(3)}, {"shape", S("rounded")}, {"text", S("**Ship**")}, {"fill", S("#2e7d32")}, {"line", S("#114411")},
                                    {"name", S("ship")}, {"position", S("right")}}));
    run(s, "office.deck_text_box", P({{"slide", I(3)}, {"text", S("Six weeks")}, {"position", S("top")}, {"size", N(32)}, {"bold", B(true)}, {"align", S("center")}}));
    run(s, "office.deck_line", P({{"slide", I(3)}, {"x", N(100)}, {"y", N(420)}, {"x2", N(840)}, {"y2", N(420)}, {"arrow", B(true)}, {"color", S("accent")}}));
    run(s, "office.deck_image", P({{"slide", I(2)}, {"path", S(pic)}, {"x", N(560)}, {"y", N(140)}, {"w", N(300)}}));
    run(s, "office.deck_image", P({{"master", B(true)}, {"path", S(logo)}, {"x", N(860)}, {"y", N(20)}, {"w", N(60)}, {"name", S("logo")}}));
    run(s, "office.deck_master", P({{"footer", S("Launch | Confidential")}}));
    run(s, "office.deck_transition", P({{"all", B(true)}, {"type", S("fade")}}));
    run(s, "office.deck_transition", P({{"slide", I(3)}, {"type", S("push")}, {"direction", S("up")}, {"duration", N(1)}, {"advance_after", N(5)}}));
}

void testCommands() {
    Session s;
    registerCommands(s);
    const std::string logo = pngFile("logo.png", 120, 60, {230, 120, 30, 255});
    const std::string pic = pngFile("pic.png", 400, 300, {60, 140, 200, 255});
    build(s, logo, pic);
    Deck& d = deckOf(s);
    CHECK(d.slides.size() == 3);
    CHECK(d.slides[2].objects.size() == 4);
    if (d.slides[2].objects.size() == 4) {
        const auto& ship = d.slides[2].objects[1];
        CHECK(ship.name == "ship" && ship.shape == "rounded" && ship.fill == "#2e7d32" && ship.line == "#114411");
        CHECK(std::abs(ship.x - (960 - 48 - 200)) < 0.01 && std::abs(ship.y - (540 - 120) / 2.0) < 0.01);  // position=right
        const auto& box = d.slides[2].objects[2];
        CHECK(box.kind == "text" && box.h == 0 && box.bold && box.align == 1 && std::abs(box.y - 48) < 0.01);
        const auto& line = d.slides[2].objects[3];
        CHECK(line.kind == "line" && line.arrow && std::abs(line.w - 740) < 0.01 && line.h == 0 && line.line == "accent");
    }
    CHECK(d.slides[1].objects.size() == 1 && std::abs(d.slides[1].objects[0].h - 225) < 0.01);  // keeps proportions
    CHECK(d.master.size() == 1 && d.footer == "Launch | Confidential");
    CHECK(d.slides[0].transition == "fade" && d.slides[2].transition == "push" && d.slides[2].direction == "up" && d.slides[2].advanceAfter == 5);

    // listing, editing by name, order, delete and undo
    Result r = run(s, "office.deck_objects", P({{"slide", I(3)}}));
    CHECK(r.ok && contains(r.message, "2. rounded \"ship\"") && contains(r.message, "line from 100,420 to 840,420"));
    r = run(s, "office.deck_object_edit", P({{"slide", I(3)}, {"object", S("SHIP")}, {"text", S("Done")}, {"order", S("back")}, {"color", S("yellow")}}));
    CHECK(r.ok && d.slides[2].objects[0].name == "ship" && d.slides[2].objects[0].text == "Done" && d.slides[2].objects[0].color == "#ffff00");
    s.history().undo();
    CHECK(d.slides[2].objects[1].name == "ship" && d.slides[2].objects[1].text == "**Ship**");
    r = run(s, "office.deck_object_edit", P({{"slide", I(3)}, {"object", S("4")}, {"x2", N(500)}, {"y2", N(300)}}));
    CHECK(r.ok && std::abs(d.slides[2].objects[3].w - 400) < 0.01 && std::abs(d.slides[2].objects[3].h + 120) < 0.01);
    r = run(s, "office.deck_object_edit", P({{"slide", I(3)}, {"object", S("1")}, {"shape", S("star")}, {"fill", S("none")}, {"line", S("accent")}}));
    CHECK(r.ok && d.slides[2].objects[0].shape == "star" && d.slides[2].objects[0].fill.empty());
    CHECK(!s.run("office.deck_object_edit", P({{"slide", I(3)}, {"object", S("nope")}, {"x", N(1)}})).ok);
    CHECK(!s.run("office.deck_shape", P({{"slide", I(3)}, {"fill", S("plaid")}})).ok);
    CHECK(!s.run("office.deck_shape", P({{"shape", S("rect")}})).ok);  // no slide
    CHECK(!s.run("office.deck_line", P({{"slide", I(3)}, {"x", N(1)}, {"y", N(1)}, {"x2", N(1)}, {"y2", N(1)}})).ok);
    r = run(s, "office.deck_object_delete", P({{"slide", I(3)}, {"object", S("1")}}));
    CHECK(r.ok && d.slides[2].objects.size() == 3);
    s.history().undo();
    CHECK(d.slides[2].objects.size() == 4);

    // the master shows on content slides with the footer and number, not on the title slide
    pdf::Page title = layoutSlide(d, 0), content = layoutSlide(d, 1);
    CHECK(count<pdf::ImageOp>(title) == 0 && count<pdf::ImageOp>(content) == 2);
    CHECK(!contains(pageText(title), "Confidential") && contains(pageText(content), "Launch | Confidential"));
    run(s, "office.deck_master", P({{"slide_numbers", B(false)}, {"on_title", B(true)}}));
    CHECK(count<pdf::ImageOp>(layoutSlide(d, 0)) == 1 && contains(pageText(layoutSlide(d, 0)), "Confidential"));
    CHECK(!contains(pageText(layoutSlide(d, 1)), "Confidential 2"));
    s.history().undo();

    // slides: duplicate, hide, background
    r = run(s, "office.deck_duplicate_slide", P({{"slide", I(3)}}));
    CHECK(r.ok && d.slides.size() == 4 && d.slides[3].objects.size() == 4 && d.slides[3].transition == "push");
    run(s, "office.deck_edit_slide", P({{"slide", I(4)}, {"hidden", B(true)}, {"background", S("#101820")}}));
    CHECK(d.slides[3].hidden && d.slides[3].background == "#101820");
    CHECK(layoutDeck(d).size() == 3);
    raster::Image dark = pdf::renderPage(layoutSlide(d, 3), 0.25);
    CHECK(dark.at(2, 2) == raster::Color(0x10, 0x18, 0x20, 255));
    r = run(s, "office.deck_info", P({}));
    CHECK(contains(r.message, "4. blank: (no title), 4 object(s), background #101820, push transition, advances after 5 s (hidden)"));
    CHECK(contains(r.message, "master: slide numbers, footer \"Launch | Confidential\", 1 object(s)"));

    // theme colours
    r = run(s, "office.deck_theme", P({{"background", S("#fdf6e3")}, {"text_color", S("#073642")}}));
    CHECK(r.ok && d.theme.background == pdf::Color(0xfd, 0xf6, 0xe3, 255) && contains(r.message, "background #fdf6e3"));
    s.history().undo();

    // the window's view: a slide sorter
    auto sorter = s.workspace().getAs<DeckDoc>("deck1")->preview();
    CHECK(sorter && sorter->name == "slides.png");
    if (sorter) writeBytes((tmpDir() / "sorter.png").string(), sorter->bytes);

    // notes, notes pages, the slide show
    r = run(s, "office.deck_notes", P({}));
    CHECK(r.ok && contains(r.message, "2. Steps: Say </script> safely") && contains(r.message, "1 of 4"));
    const std::string notesPdf = (tmpDir() / "notes.pdf").string();
    CHECK(run(s, "office.deck_export", P({{"path", S(notesPdf)}, {"notes", B(true)}})).ok);
    pdf::PdfFile nf = pdf::PdfFile::load(notesPdf);
    CHECK(nf.pageCount() == 3 && contains(nf.pageText(1), "Say </script> safely") && contains(nf.pageText(1), "Steps"));
    const std::string show = (tmpDir() / "show.html").string();
    r = run(s, "office.deck_present", P({{"path", S(show)}, {"width", I(640)}}));
    CHECK(r.ok && contains(r.message, "3 slide(s)"));
    const std::string html = readBytes(show);
    CHECK(contains(html, "data-t=\"push\" data-d=\"up\" data-s=\"1.00\" data-a=\"5.00\""));
    CHECK(contains(html, "data-t=\"fade\""));
    CHECK(contains(html, "Say \\u003c/script> safely") && !contains(html, "Say </script>"));
    CHECK(contains(html, "\"count\":3") && contains(html, "data:image/jpeg;base64,") && contains(html, "data:image/png;base64,"));
    CHECK(!contains(html, "data-n=\"4\""));  // hidden
    CHECK(!s.run("office.deck_present", P({})).ok);  // never saved: needs path=
    const std::string html2 = (tmpDir() / "export.html").string();
    CHECK(run(s, "office.deck_export", P({{"path", S(html2)}, {"loop", B(true)}, {"width", I(480)}})).ok && contains(readBytes(html2), "\"loop\":true"));
}

void testPptx() {
    Session s;
    registerCommands(s);
    const std::string logo = pngFile("logo2.png", 120, 60, {230, 120, 30, 255});
    const std::string pic = pngFile("pic2.png", 400, 300, {60, 140, 200, 255});
    build(s, logo, pic);
    run(s, "office.deck_text_box", P({{"slide", I(2)}, {"text", S("- one\n  - two\n- three")}, {"bullets", B(true)}, {"x", N(48)}, {"y", N(400)},
                                       {"w", N(300)}, {"h", N(100)}, {"italic", B(true)}, {"fill", S("background")}, {"color", S("accent")}}));
    run(s, "office.deck_edit_slide", P({{"slide", I(1)}, {"hidden", B(true)}}));
    run(s, "office.deck_master", P({{"slide_numbers", B(false)}}));
    run(s, "office.deck_theme", P({{"background", S("#fdf6e3")}}));
    const Deck& d = deckOf(s);
    const std::string bytes = writePptx(d);
    Deck back = readPptx(bytes);
    CHECK(back.slides.size() == 3 && back.master.size() == 1);
    CHECK(back.footer == "Launch | Confidential" && !back.slideNumbers && !back.masterOnTitle);
    CHECK(back.theme.background == pdf::Color(0xfd, 0xf6, 0xe3, 255) && back.theme.accent == d.theme.accent);
    if (back.master.size() == 1) CHECK(back.master[0].kind == "image" && back.master[0].name == "logo" && std::abs(back.master[0].w - 60) < 0.01);
    if (back.slides.size() == 3) {
        CHECK(back.slides[0].hidden && !back.slides[1].hidden);
        CHECK(back.slides[0].transition == "fade" && std::abs(back.slides[0].transitionSeconds - 0.75) < 0.01);
        CHECK(back.slides[2].transition == "push" && back.slides[2].direction == "up" && back.slides[2].advanceAfter == 5 && back.slides[2].transitionSeconds == 1.0);
        CHECK(back.slides[1].bullets.size() == 3 && back.slides[1].pixels == nullptr && back.slides[1].layout == "content");
        const auto& o2 = back.slides[1].objects;
        CHECK(o2.size() == 2);
        if (o2.size() == 2) {
            CHECK(o2[0].kind == "image" && o2[0].pixels && o2[0].pixels->pixels.width() == 400 && std::abs(o2[0].x - 560) < 0.01);
            CHECK(o2[1].kind == "text" && o2[1].bullets && o2[1].italic && o2[1].fill == "background" && o2[1].color == "accent");
            CHECK(o2[1].text == "- one\n  - two\n- three");
        }
        const auto& o3 = back.slides[2].objects;
        CHECK(o3.size() == 4);
        if (o3.size() == 4) {
            CHECK(o3[0].kind == "shape" && o3[0].shape == "chevron" && o3[0].fill == "accent" && o3[0].line.empty() && o3[0].text == "Build");
            CHECK(o3[1].name == "ship" && o3[1].shape == "rounded" && o3[1].text == "Ship" && o3[1].bold && o3[1].align == 1 && o3[1].anchor == 1);
            CHECK(o3[2].kind == "text" && o3[2].h == 0 && o3[2].size == 32 && o3[2].text == "Six weeks");
            CHECK(o3[3].kind == "line" && o3[3].arrow && std::abs(o3[3].w - 740) < 0.01 && o3[3].line == "accent" && o3[3].lineWidth == 2);
        }
    }
    // the reread deck draws like the original
    CHECK(pageText(layoutSlide(back, 2)) == pageText(layoutSlide(d, 2)));

    // a line drawn right to left and a group, the way other programs write them
    auto entries = raster::readZip(bytes);
    for (auto& e : entries) {
        if (e.name == "docProps/app.xml") e.data = "<Properties><Application>Microsoft Office PowerPoint</Application></Properties>";
        if (e.name == "ppt/slides/slide3.xml") {
            const std::string grp = "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"90\" name=\"Group 1\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm>"
                                    "<a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"2540000\" cy=\"1270000\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"1270000\" cy=\"635000\"/>"
                                    "</a:xfrm></p:grpSpPr><p:sp><p:nvSpPr><p:cNvPr id=\"91\" name=\"Oval 2\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm>"
                                    "<a:off x=\"0\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm><a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom></p:spPr>"
                                    "<p:style><a:lnRef idx=\"2\"><a:schemeClr val=\"accent1\"/></a:lnRef><a:fillRef idx=\"1\"><a:schemeClr val=\"accent1\"/></a:fillRef>"
                                    "<a:effectRef idx=\"0\"><a:schemeClr val=\"accent1\"/></a:effectRef><a:fontRef idx=\"minor\"><a:schemeClr val=\"lt1\"/></a:fontRef></p:style>"
                                    "</p:sp><p:cxnSp><p:nvCxnSpPr><p:cNvPr id=\"92\" name=\"Connector 3\"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>"
                                    "<a:xfrm flipH=\"1\"><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"0\"/></a:xfrm><a:prstGeom prst=\"line\"><a:avLst/></a:prstGeom>"
                                    "</p:spPr></p:cxnSp></p:grpSp>";
            const auto at = e.data.find("</p:spTree>");
            if (at != std::string::npos) e.data.insert(at, grp);
        }
    }
    Deck foreign = readPptx(raster::writeZip(entries));
    CHECK(foreign.slides.size() == 3);
    if (foreign.slides.size() == 3) {
        // pictures of other programs keep their place as objects
        CHECK(foreign.slides[1].pixels == nullptr && foreign.slides[1].objects.size() == 2);
        const auto& o = foreign.slides[2].objects;
        CHECK(o.size() == 6);
        if (o.size() == 6) {
            CHECK(o[4].shape == "ellipse" && o[4].fill == "accent" && o[4].line == "accent" && o[4].name == "Oval 2");
            CHECK(std::abs(o[4].x - 100) < 0.01 && std::abs(o[4].y - 100) < 0.01 && std::abs(o[4].w - 100) < 0.01 && std::abs(o[4].h - 100) < 0.01);
            CHECK(o[5].kind == "line" && std::abs(o[5].x - 300) < 0.01 && std::abs(o[5].w + 200) < 0.01 && o[5].line == "text");
        }
    }

    // saving through the command and opening again
    const std::string path = (tmpDir() / "talk.pptx").string();
    CHECK(run(s, "office.deck_save", P({{"path", S(path)}})).ok);
    Result r = run(s, "office.deck_objects", P({{"file", S(path)}, {"master", B(true)}}));
    CHECK(r.ok && contains(r.message, "1. image \"logo\" at 860,20 size 60x30"));
}

}  // namespace

int main() {
    testModel();
    testCommands();
    testPptx();
    return check::summary("office deck commands");
}
