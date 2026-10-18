// Text shapes: lines of text laid out with a TrueType font into path data, so
// they render, convert to paths and take part in booleans like any other shape.
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>

#include "wetwaresuite/pdfdoc/fonts.hpp"
#include "wetwaresuite/vector/vector.hpp"

namespace wetwaresuite::vector {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::shared_ptr<pdfdoc::TrueType> fontFor(const Shape& s) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<pdfdoc::TrueType>> cache;
    const std::string name = lower(s.font);
    const std::string key = s.font + (s.bold ? "|b" : "|") + (s.italic ? "i" : "");
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    std::shared_ptr<pdfdoc::TrueType> font;
    const std::filesystem::path file(s.font);
    const std::string ext = lower(file.extension().string());
    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") {
        try {
            font = pdfdoc::TrueType::load(file);
        } catch (const std::exception&) {
        }
    } else if (!name.empty() && name != "sans" && name != "roboto" && name != "sans-serif") {
        if (auto family = pdfdoc::standardFamily(name)) font = pdfdoc::systemFont(*family, s.bold, s.italic);
    }
    if (!font) font = pdfdoc::builtinSans();
    cache[key] = font;
    return font;
}

}  // namespace

std::string textPathData(const Shape& s) {
    auto font = fontFor(s);
    auto fallback = pdfdoc::builtinSans();
    const double size = s.fontSize;
    std::string d;
    double y = s.y;
    std::size_t start = 0;
    while (start <= s.text.size()) {
        std::size_t end = s.text.find('\n', start);
        if (end == std::string::npos) end = s.text.size();
        const std::vector<unsigned> cps = pdfdoc::utf8Decode(s.text.substr(start, end - start));
        // each character from the font that has it: the chosen one, else the built-in one, else '?'
        std::vector<std::pair<unsigned, const pdfdoc::TrueType*>> glyphs;
        for (unsigned cp : cps) {
            if (cp == '\r') continue;
            if (font->hasGlyph(cp) || cp == ' ') glyphs.push_back({cp, font.get()});
            else if (fallback->hasGlyph(cp)) glyphs.push_back({cp, fallback.get()});
            else glyphs.push_back({'?', font.get()});
        }
        auto advance = [&](std::size_t i) {
            double a = glyphs[i].second->advance(glyphs[i].first) * size / 1000;
            if (i + 1 < glyphs.size() && glyphs[i + 1].second == glyphs[i].second) {
                a += glyphs[i].second->kern(glyphs[i].first, glyphs[i + 1].first) * size / 1000;
            }
            return a + (i + 1 < glyphs.size() ? s.letterSpacing : 0);
        };
        double width = 0;
        for (std::size_t i = 0; i < glyphs.size(); ++i) width += advance(i);
        double x = s.x;
        if (s.anchor == "middle") x -= width / 2;
        else if (s.anchor == "end") x -= width;
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            d += glyphs[i].second->svgPath(glyphs[i].first, x, y, size);
            x += advance(i);
        }
        y += size * s.lineHeight;
        start = end + 1;
    }
    return d;
}

std::string toPathData(const Shape& s, double detail) {
    if (s.kind == ShapeKind::Text) return textPathData(s);
    if (s.kind == ShapeKind::Path) return s.d;
    return outlinePathData(localOutline(s, detail));
}

}  // namespace wetwaresuite::vector
