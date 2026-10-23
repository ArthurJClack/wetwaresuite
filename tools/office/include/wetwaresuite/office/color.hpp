// Colours shared by spreadsheets, documents and slides.
#pragma once

#include <cstdint>
#include <string>

namespace wetwaresuite::office {

// Colours are 0xRRGGBB with kColorSet added, so black is a colour too; 0 means automatic / none.
constexpr std::uint32_t kColorSet = 0x1000000;
inline bool hasColor(std::uint32_t c) { return (c & kColorSet) != 0; }
inline std::uint32_t rgbOf(std::uint32_t c) { return c & 0xFFFFFF; }
// "#ff0000", "ff0000", "red", "none" -> colour (0 for none); false when it isn't one.
bool parseColor(const std::string& text, std::uint32_t& out);
std::string colorName(std::uint32_t c);  // "#rrggbb" or "none"

}  // namespace wetwaresuite::office
