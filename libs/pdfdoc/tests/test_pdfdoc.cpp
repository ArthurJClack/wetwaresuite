#include "achclwss/pdfdoc/fonts.hpp"
#include "achclwss/pdfdoc/writer.hpp"
#include "check.hpp"

using namespace achclwss;
using namespace achclwss::pdfdoc;

int main() {
    // encoding
    CHECK(winAnsi('A') == 'A');
    CHECK(winAnsi(0x20AC) == 128);   // euro
    CHECK(winAnsi(0x2014) == 151);   // em dash
    CHECK(winAnsi(0xE9) == 0xE9);    // é
    CHECK(winAnsi(0x4E2D) == -1);
    CHECK(unicodeOf(147) == 0x201C);
    CHECK(toWinAnsi("caf\xC3\xA9 \xE2\x80\x94") == "caf\xE9 \x97");
    CHECK(utf8Decode(utf8Encode(0x2019)).at(0) == 0x2019);

    // standard fonts
    CHECK(standardFamily("Arial") == std::optional<std::string>("helvetica"));
    CHECK(standardFamily("serif") == std::optional<std::string>("times"));
    CHECK(standardFamily("Mono") == std::optional<std::string>("courier"));
    CHECK(!standardFamily("Comic Sans"));
    CHECK(std::string(baseFontName(standardFont("times", true, true))) == "Times-BoldItalic");
    CHECK(std::string(baseFontName(standardFont("helvetica", false, true))) == "Helvetica-Oblique");
    CHECK(standardWidth(Standard::Helvetica, 'W') == 944);
    CHECK(standardWidth(Standard::TimesRoman, 'a') == 444);
    CHECK(standardWidth(Standard::CourierBold, 'i') == 600);
    CHECK(standardWidth(Standard::HelveticaBold, 'b') > standardWidth(Standard::Helvetica, 'b'));
    CHECK(standardMetrics(Standard::TimesRoman).ascent == 683);

    // writer
    Writer w;
    int pages = w.reserve();
    int font = addStandardFont(w, Standard::Helvetica);
    int content = w.addStream("", "BT /F1 12 Tf 72 720 Td " + text("Hello \xE2\x80\x94 (world)") + " Tj ET");
    int page = w.add("<< /Type /Page /Parent " + std::to_string(pages) + " 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 " +
                     std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
    w.set(pages, "<< /Type /Pages /Kids [" + std::to_string(page) + " 0 R] /Count 1 >>");
    int root = w.add("<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    std::string pdf = w.finish(root, 0);
    CHECK(pdf.rfind("%PDF-1.4", 0) == 0);
    CHECK(pdf.find("/FlateDecode") != std::string::npos);
    CHECK(pdf.find("startxref") != std::string::npos);
    CHECK(pdf.find("/BaseFont /Helvetica") != std::string::npos);
    CHECK(literal("a(b)\\") == "(a\\(b\\)\\\\)");
    CHECK(num(1.5) == "1.5" && num(2.0) == "2" && num(-0.00001) == "0");
    std::string z = deflate(std::string(1000, 'x'));
    CHECK(z.size() < 100 && static_cast<unsigned char>(z[0]) == 0x78);

    // images: an alpha channel adds a soft mask
    raster::Image img(4, 3, {255, 0, 0, 128});
    Writer wi;
    addImage(wi, img);
    std::string ipdf = wi.finish(1, 0);
    CHECK(ipdf.find("/SMask") != std::string::npos);
    CHECK(addJpeg(wi, "not a jpeg") == 0);

    // system fonts are optional; when present they must work
    if (auto tt = systemFont("helvetica", false, false)) {
        CHECK(tt->hasGlyph('A'));
        CHECK(tt->advance('A') > 400 && tt->advance('A') < 900);
        CHECK(!tt->outline('O').empty());
        CHECK(tt->outline('O').size() >= 2);  // outer and inner contour
        CHECK(!tt->postscriptName().empty());
        if (tt->embeddable()) {
            Writer wf;
            addTrueTypeFont(wf, *tt);
            std::string fpdf = wf.finish(1, 0);
            CHECK(fpdf.find("/FontFile2") != std::string::npos);
            CHECK(fpdf.find("/Subtype /TrueType") != std::string::npos);
        }
    }
    return check::summary("pdfdoc");
}
