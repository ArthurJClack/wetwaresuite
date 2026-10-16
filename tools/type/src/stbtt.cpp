// CFF outlines through stb_truetype (already vendored for the PDF writer), so
// OpenType fonts with PostScript outlines open without a CFF interpreter here.
#include <string>
#include <vector>

#include "internal.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace achclwss::typeface {

bool cffOutlines(const std::string& file, std::size_t offset, std::vector<std::vector<Contour>>& glyphs, std::string& error) {
    stbtt_fontinfo info;
    const auto* data = reinterpret_cast<const unsigned char*>(file.data());
    if (!stbtt_InitFont(&info, data, static_cast<int>(offset))) {
        error = "stb_truetype couldn't read the font";
        return false;
    }
    glyphs.assign(static_cast<std::size_t>(info.numGlyphs), {});
    for (int g = 0; g < info.numGlyphs; ++g) {
        stbtt_vertex* v = nullptr;
        const int n = stbtt_GetGlyphShape(&info, g, &v);
        auto& out = glyphs[static_cast<std::size_t>(g)];
        Contour cur;
        auto finish = [&] {
            auto& pts = cur.points;
            if (pts.size() >= 2 && pts.back().on() && pts.back().x == pts.front().x && pts.back().y == pts.front().y) pts.pop_back();
            if (pts.size() >= 2) out.push_back(std::move(cur));
            cur = Contour{};
        };
        for (int i = 0; i < n; ++i) {
            const stbtt_vertex& p = v[i];
            switch (p.type) {
            case STBTT_vmove:
                finish();
                cur.points.push_back({static_cast<double>(p.x), static_cast<double>(p.y), PointType::On});
                break;
            case STBTT_vline:
                cur.points.push_back({static_cast<double>(p.x), static_cast<double>(p.y), PointType::On});
                break;
            case STBTT_vcurve:
                cur.points.push_back({static_cast<double>(p.cx), static_cast<double>(p.cy), PointType::Quad});
                cur.points.push_back({static_cast<double>(p.x), static_cast<double>(p.y), PointType::On});
                break;
            case STBTT_vcubic:
                cur.points.push_back({static_cast<double>(p.cx), static_cast<double>(p.cy), PointType::Cubic});
                cur.points.push_back({static_cast<double>(p.cx1), static_cast<double>(p.cy1), PointType::Cubic});
                cur.points.push_back({static_cast<double>(p.x), static_cast<double>(p.y), PointType::On});
                break;
            default:
                break;
            }
        }
        finish();
        if (v) stbtt_FreeShape(&info, v);
    }
    return true;
}

}  // namespace achclwss::typeface
