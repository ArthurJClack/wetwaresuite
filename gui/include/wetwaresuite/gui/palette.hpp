// The colour palette every app shares: three colours a person picks, from
// which the window derives every shade it draws with.
//
//   primary    the main colour: buttons, headings, highlights
//   secondary  the surfaces: panels, boxes, popups (dark secondary = dark look)
//   tertiary   the details: links, borders, text selection, keyboard focus
//
// With no palette saved, each app keeps its own look (its accent colour on the
// standard dark or light surfaces). Once a palette is saved, every app uses it
// until it is reset. The file lives in the per-user wetwaresuite folder
// (see apps/common/handoff.hpp) as palette.txt; the app.palette_* commands
// read and write it. Everything here is window-free so it is unit tested.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/gui/logic.hpp"

namespace wetwaresuite::gui {

struct Palette {
    Rgba primary, secondary, tertiary;
};
bool operator==(const Palette& a, const Palette& b);

// The colours an app has when nobody picked any: its accent, the standard dark
// panel grey and the standard border grey.
Palette defaultPalette(std::uint32_t accent);

// A colour a person typed: "#3d7eff", "3d7eff", "#37f" or a plain name
// ("red", "white", ...). On failure returns nullopt and puts a friendly
// sentence naming `role` in `error`.
std::optional<Rgba> parsePaletteColour(const std::string& text, const std::string& role, std::string& error);
std::string hex6(const Rgba& c);  // "#rrggbb" (alpha ignored)

struct PalettePreset {
    std::string name;         // "high-contrast"
    std::string title;        // "High contrast"
    std::string description;  // plain words
    Palette palette;
};
// The built-in sets ("default" isn't one: it means each app's own colours).
const std::vector<PalettePreset>& palettePresets();
const PalettePreset* findPreset(const std::string& name);
std::vector<std::string> presetNames();  // "default" first, then the built-in sets

// ---- the settings file ------------------------------------------------------
// primary=#rrggbb / secondary=... / tertiary=... lines; lines starting with
// '#' or ';' are notes. CRLF and blanks are fine. nullopt if a colour is
// missing or unreadable.
std::optional<Palette> parsePaletteFile(const std::string& text);
std::string formatPaletteFile(const Palette& palette);
std::optional<Palette> loadPalette(const std::filesystem::path& file);
// Writes it atomically (temp file + rename), making the folder. False + error on failure.
bool savePalette(const std::filesystem::path& file, const Palette& palette, std::string& error);

// ---- contrast and shades ----------------------------------------------------

Rgba mixColour(const Rgba& a, const Rgba& b, float t);  // t=0 -> a, t=1 -> b
float relativeLuminance(const Rgba& c);                  // WCAG 2 relative luminance, 0..1
float contrastRatio(const Rgba& a, const Rgba& b);       // WCAG 2 ratio, 1..21
Rgba readableOn(const Rgba& background);                 // black or white, whichever reads better
// `c` moved toward black or white (whichever is further from `against`) just
// far enough to reach `minRatio` against it.
Rgba ensureContrast(const Rgba& c, const Rgba& against, float minRatio);
// mixColour(base, toward, t), with t lowered as needed so `text` keeps `minRatio` on it.
Rgba mixReadable(const Rgba& base, const Rgba& toward, float t, const Rgba& text, float minRatio);

// Every colour the window draws with, worked out from a palette.
struct Theme {
    bool dark = true;  // light text on dark surfaces
    Rgba background, popup, frame, frameHover, frameActive;
    Rgba text, textMuted;
    Rgba button, buttonHover, buttonActive;
    Rgba header, headerHover, headerActive;
    Rgba accent, accentHover, accentActive, accentText;  // filled main buttons and the text on them
    Rgba title;                                          // primary as text on the background (headings)
    Rgba highlight;                                      // check marks and slider handles
    Rgba border, link, focus, selection;
};
Theme deriveTheme(const Palette& palette);

// Contrast targets deriveTheme keeps (WCAG AA): body text 4.5, borders and focus 3.
inline constexpr float kTextContrast = 4.5f;
inline constexpr float kUiContrast = 3.0f;

}  // namespace wetwaresuite::gui
