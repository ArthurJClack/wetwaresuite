#include <cmath>
#include <stdexcept>

#include "achclwss/pdf/page.hpp"
#include "achclwss/pdf/reader.hpp"
#include "check.hpp"

using namespace achclwss;
using namespace achclwss::pdf;

namespace {

void testFonts() {
    CHECK(charWidth({Family::Sans}, 'A') == 667);
    CHECK(charWidth({Family::Serif}, 'A') == 722);
    CHECK(charWidth({Family::Mono, true}, 'i') == 600);
    CHECK(charWidth({Family::Sans, true}, 'a') == 556);
    CHECK(std::abs(textWidth({Family::Sans}, "AA", 10) - 13.34) < 1e-9);
    CHECK(toWinAnsi(0x20AC) == 0x80);  // euro
    CHECK(toWinAnsi(0x4E2D) == '?');
    CHECK(winAnsi("caf\xC3\xA9") == "caf\xE9");
    CHECK(utf8Encode(0xE9) == "\xC3\xA9");
    CHECK(fromWinAnsi(0x93) == 0x201C);
    CHECK(baseFontName({Family::Serif, true, true}) == std::string("Times-BoldItalic"));
}

void testZlib() {
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "hello world " + std::to_string(i % 17);
    std::string z = deflate(text);
    CHECK(z.size() < text.size() / 3);
    CHECK(inflate(z) == text);
    bool threw = false;
    try {
        inflate("not zlib at all");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

std::vector<Page> samplePages(int n) {
    std::vector<Page> pages;
    for (int i = 0; i < n; ++i) {
        Page p;
        p.text(72, 100, "Page " + std::to_string(i + 1) + " (with parens) caf\xC3\xA9", {Family::Sans, true}, 18);
        p.text(72, 130, "second line", {Family::Serif}, 11);
        p.rect(72, 150, 100, 40, Color{200, 30, 30, 255}, Color{0, 0, 0, 255}, 1);
        p.line(72, 200, 300, 200, {0, 0, 255, 255}, 2);
        pages.push_back(p);
    }
    return pages;
}

void testWriteReadText() {
    std::string bytes = writePdf(samplePages(3), {"Test doc", "Ada", ""});
    CHECK(bytes.rfind("%PDF-1.4", 0) == 0);
    CHECK(bytes.find("/BaseFont /Helvetica-Bold") != std::string::npos);
    PdfFile f(bytes);
    CHECK(f.pageCount() == 3);
    CHECK(f.infoField("Title") == "Test doc");
    CHECK(f.infoField("Author") == "Ada");
    CHECK(std::abs(f.pages()[0].width - 595.28) < 0.01);
    std::string t = f.pageText(1);
    CHECK(t.find("Page 2 (with parens) caf\xC3\xA9") != std::string::npos);
    CHECK(t.find("second line") != std::string::npos);
    CHECK(t.find("\n") != std::string::npos);

    // merge pages from two files in a new order, with rotation
    PdfFile g(writePdf(samplePages(2)));
    std::string merged = assemblePdf({{&f, 2, 0}, {&g, 0, 90}, {&f, 0, 0}});
    PdfFile m(merged);
    CHECK(m.pageCount() == 3);
    CHECK(m.pageText(0).find("Page 3") != std::string::npos);
    CHECK(m.pageText(1).find("Page 1") != std::string::npos);
    CHECK(m.pages()[1].rotate == 90);
    CHECK(m.pages()[0].rotate == 0);
    // re-assembling an assembled file still works
    PdfFile again(assemblePdf({{&m, 1, 90}}));
    CHECK(again.pageCount() == 1 && again.pages()[0].rotate == 180);
}

void testPageList() {
    auto v = parsePageList("1-3,5,8-", 9);
    CHECK((v == std::vector<int>{0, 1, 2, 4, 7, 8}));
    CHECK((parsePageList("3-1", 5) == std::vector<int>{2, 1, 0}));
    CHECK(parsePageList("", 4).size() == 4);
    CHECK((parsePageList("last", 4) == std::vector<int>{3}));
    bool threw = false;
    try {
        parsePageList("7", 4);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

void testRender() {
    Page p = samplePages(1)[0];
    raster::Image img = renderPage(p, 1.0);
    CHECK(img.width() == 595 && img.height() == 842);
    // red rectangle and dark text pixels are where they should be
    raster::Color c = img.at(120, 170);
    CHECK(c.r > 180 && c.g < 60);
    int dark = 0;
    for (int y = 82; y < 104; ++y) {
        for (int x = 72; x < 200; ++x) dark += img.at(x, y).r < 100 ? 1 : 0;
    }
    CHECK(dark > 100);
    CHECK(textWidthPx("abc", {}, 20) > 20);
}

void testImages() {
    raster::Image px(4, 3, {10, 200, 30, 255});
    px.set(0, 0, {0, 0, 0, 0});
    auto data = std::make_shared<ImageData>();
    data->pixels = px;
    Page p;
    p.image(10, 10, 40, 30, data);
    std::string bytes = writePdf({p});
    CHECK(bytes.find("/SMask") != std::string::npos);
    PdfFile f(bytes);
    CHECK(f.pageCount() == 1);
    raster::Image img = renderPage(p, 1.0);
    CHECK(img.at(30, 30).g > 150);
}

void testHandWritten() {
    // a classic uncompressed file with an inherited MediaBox and a ToUnicode-less font
    std::string pdf =
        "%PDF-1.3\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 300 200] >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
        "4 0 obj << /Length 60 >> stream\nBT /F1 12 Tf 10 100 Td [(Hel) -20 (lo) -500 (there)] TJ ET\nendstream endobj\n"
        "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    PdfFile f(pdf);
    CHECK(f.pageCount() == 1);
    CHECK(f.pages()[0].width == 300 && f.pages()[0].height == 200);
    CHECK(f.pageText(0) == "Hello there");
}

}  // namespace

int main() {
    testFonts();
    testZlib();
    testWriteReadText();
    testPageList();
    testRender();
    testImages();
    testHandWritten();
    return check::summary("pdf");
}
