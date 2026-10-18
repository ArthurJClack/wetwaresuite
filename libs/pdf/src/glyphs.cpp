#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "wetwaresuite/pdf/page.hpp"
#include "internal.hpp"

namespace wetwaresuite::pdf::detail {

namespace {
#include "fontdata.inc"

const std::string& blob() {
    static const std::string data = inflate(std::string(reinterpret_cast<const char*>(kGlyphBlob), sizeof kGlyphBlob));
    return data;
}

}  // namespace

double stdWidth(int fontIndex, unsigned char b) {
    if (fontIndex < 0 || fontIndex > 11 || b < 32) return 0;
    return kWidths[fontIndex][b - 32];
}

int glyphEm() { return kGlyphEm; }

Glyph glyph(int rasterFont, unsigned char b) {
    Glyph g;
    if (rasterFont < 0 || rasterFont > 5 || b < 32) return g;
    const int* e = kGlyphs[rasterFont][b - 32];
    if (e[2] <= 0 || e[3] <= 0) return g;
    const std::string& data = blob();
    if (static_cast<std::size_t>(e[4]) + static_cast<std::size_t>(e[2]) * static_cast<std::size_t>(e[3]) > data.size()) return g;
    g.left = e[0];
    g.top = e[1];
    g.width = e[2];
    g.height = e[3];
    g.coverage = reinterpret_cast<const unsigned char*>(data.data()) + e[4];
    return g;
}

std::string num(double v) {
    if (!std::isfinite(v)) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0" || s.empty()) s = "0";
    return s;
}

}  // namespace wetwaresuite::pdf::detail
