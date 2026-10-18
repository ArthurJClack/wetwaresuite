// The shared colour palette: reading colours people type, the settings file,
// and the shades derived from any three colours staying readable. No window.
#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "wetwaresuite/gui/palette.hpp"

using namespace wetwaresuite::gui;
namespace fs = std::filesystem;

namespace {

void testParse() {
    std::string err;
    auto c = parsePaletteColour("#3D7EFF", "primary", err);
    CHECK(c && hex6(*c) == "#3d7eff");
    CHECK(hex6(*parsePaletteColour("3d7eff", "primary", err)) == "#3d7eff");  // the # is optional
    CHECK(hex6(*parsePaletteColour("  #0f8 ", "primary", err)) == "#00ff88");  // short form, spaces
    CHECK(hex6(*parsePaletteColour("White", "primary", err)) == "#ffffff");   // plain names
    for (const char* bad : {"", "#12345", "#ggg000", "blue-ish", "#3d7eff00", "rgb(1,2,3)"}) {
        err.clear();
        CHECK(!parsePaletteColour(bad, "secondary", err));
        CHECK(err.find("secondary colour") != std::string::npos && err.find("#3d7eff") != std::string::npos);
    }
    parsePaletteColour("#3d7eff80", "tertiary", err);
    CHECK(err.find("see-through") != std::string::npos);
}

void testFile() {
    Palette p{*parseHex("#ff8800"), *parseHex("#112233"), *parseHex("#abcdef")};
    auto back = parsePaletteFile(formatPaletteFile(p));
    CHECK(back && *back == p);
    // CRLF, notes, blanks, odd spacing and case, unknown keys
    auto crlf = parsePaletteFile("# notes\r\n\r\n; more\r\n PRIMARY = #FF8800\r\nsecondary=#112233\r\nfont=big\r\ntertiary=#abcdef\r\n");
    CHECK(crlf && *crlf == p);
    CHECK(!parsePaletteFile("primary=#ff8800\nsecondary=#112233\n"));  // one missing
    CHECK(!parsePaletteFile("primary=#ff8800\nsecondary=nope\ntertiary=#abcdef\n"));
    CHECK(!parsePaletteFile(""));

    fs::path dir = fs::temp_directory_path() / "wetwaresuite_test_palette";
    fs::remove_all(dir);
    fs::path file = dir / "sub" / "palette.txt";
    CHECK(!loadPalette(file));
    std::string err;
    CHECK(savePalette(file, p, err));  // makes the folder
    CHECK(loadPalette(file) && *loadPalette(file) == p);
    Palette q = p;
    q.primary = *parseHex("#000000");
    CHECK(savePalette(file, q, err));  // replaces the old file
    CHECK(*loadPalette(file) == q);
    CHECK(!fs::exists(dir / "sub" / "palette.txt.tmp"));
    fs::remove_all(dir);
}

void testContrast() {
    const Rgba black{0, 0, 0, 1}, white{1, 1, 1, 1};
    CHECK(contrastRatio(black, white) > 20.9f && contrastRatio(black, white) < 21.1f);
    CHECK(contrastRatio(white, white) < 1.01f);
    CHECK(readableOn(black).r == 1 && readableOn(white).r == 0);
    CHECK(readableOn(*parseHex("#ffd400")).r == 0);  // yellow gets black text
    CHECK(readableOn(*parseHex("#1f5fd1")).r == 1);  // blue gets white text
    const Rgba grey = *parseHex("#777777");
    const Rgba fixed = ensureContrast(grey, *parseHex("#808080"), 4.5f);
    CHECK(contrastRatio(fixed, *parseHex("#808080")) >= 4.5f);
    CHECK(hex6(ensureContrast(white, black, 4.5f)) == "#ffffff");  // already fine: unchanged
    const Rgba m = mixReadable(black, white, 0.9f, white, 4.5f);  // can't go all the way and keep white text readable
    CHECK(contrastRatio(m, white) >= 4.5f && m.r > 0.2f);
}

// Any three colours give a readable window.
void checkTheme(const Palette& p) {
    const Theme th = deriveTheme(p);
    for (const Rgba* s : {&th.background, &th.popup, &th.frame, &th.frameHover, &th.frameActive, &th.button, &th.buttonHover,
                          &th.buttonActive, &th.header, &th.headerHover, &th.headerActive})
        CHECK(contrastRatio(th.text, *s) >= kTextContrast);
    for (const Rgba* s : {&th.background, &th.popup, &th.frame}) CHECK(contrastRatio(th.textMuted, *s) >= kTextContrast);
    for (const Rgba* s : {&th.accent, &th.accentHover, &th.accentActive}) CHECK(contrastRatio(th.accentText, *s) >= kTextContrast);
    CHECK(contrastRatio(th.title, th.background) >= kTextContrast);
    CHECK(contrastRatio(th.link, th.background) >= kTextContrast);
    CHECK(contrastRatio(th.border, th.background) >= kUiContrast);
    CHECK(contrastRatio(th.focus, th.background) >= kUiContrast);
    CHECK(contrastRatio(th.highlight, th.frame) >= kUiContrast);
    CHECK(th.selection.a > 0.1f && th.selection.a < 0.9f);
}

void testThemes() {
    for (const auto& preset : palettePresets()) checkTheme(preset.palette);
    checkTheme(defaultPalette(0x3d7eff));
    // a sweep of awkward picks: same colour three times, mid greys, pure primaries
    const std::uint32_t picks[] = {0x000000, 0xffffff, 0x777777, 0x808080, 0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0x3d7eff, 0x5a5a5a};
    for (auto a : picks)
        for (auto b : picks)
            for (auto c : {picks[0], picks[2], picks[4], picks[7]}) checkTheme({fromRgb24(a), fromRgb24(b), fromRgb24(c)});
    CHECK(deriveTheme(findPreset("light")->palette).dark == false);
    CHECK(deriveTheme(findPreset("high-contrast")->palette).dark == true);
    CHECK(hex6(deriveTheme(findPreset("high-contrast")->palette).accent) == "#ffd400");  // a readable primary is kept as it is
}

void testPresets() {
    auto names = presetNames();
    CHECK(names.size() >= 5 && names[0] == "default");
    for (const char* n : {"high-contrast", "light", "colour-blind"}) CHECK(findPreset(n) != nullptr);
    CHECK(findPreset("Light") != nullptr);
    CHECK(findPreset("default") == nullptr);  // default means each app's own colours, not a set
    CHECK(defaultPalette(0xe5484d).primary.r > 0.89f);
}

}  // namespace

int main() {
    testParse();
    testFile();
    testContrast();
    testThemes();
    testPresets();
    return check::summary("gui_palette");
}
