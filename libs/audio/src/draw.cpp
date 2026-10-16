#include "draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace achclwss::audio::detail {

using raster::Color;
using raster::Image;


void blendPixel(Image& img, int x, int y, Color c, float alpha) {
    if (!img.contains(x, y)) return;
    std::uint8_t* p = img.pixel(x, y);
    const float a = alpha * c.a / 255.0f;
    p[0] = static_cast<std::uint8_t>(p[0] + (c.r - p[0]) * a);
    p[1] = static_cast<std::uint8_t>(p[1] + (c.g - p[1]) * a);
    p[2] = static_cast<std::uint8_t>(p[2] + (c.b - p[2]) * a);
    p[3] = 255;
}

void vline(Image& img, int x, int y0, int y1, Color c, float alpha) {
    if (y0 > y1) std::swap(y0, y1);
    for (int y = y0; y <= y1; ++y) blendPixel(img, x, y, c, alpha);
}

void hline(Image& img, int x0, int x1, int y, Color c, float alpha, int dash) {
    for (int x = x0; x <= x1; ++x) {
        if (dash > 0 && (x / dash) % 2) continue;
        blendPixel(img, x, y, c, alpha);
    }
}

// 3x5 pixel font for axis labels.
static const std::map<char, const char*>& tinyFont() {
    static const std::map<char, const char*> f = {
        {'0', "111101101101111"}, {'1', "010110010010111"}, {'2', "111001111100111"}, {'3', "111001111001111"},
        {'4', "101101111001001"}, {'5', "111100111001111"}, {'6', "111100111101111"}, {'7', "111001010010010"},
        {'8', "111101111101111"}, {'9', "111101111001111"}, {'k', "100101110101101"}, {'H', "101101111101101"},
        {'z', "000111010100111"}, {'s', "000011110001110"}, {'.', "000000000000010"}, {':', "000010000010000"},
        {'-', "000000111000000"}, {'m', "000110111101101"}, {'d', "001001111101111"}, {'B', "110101110101110"},
        {' ', "000000000000000"}};
    return f;
}

void text(Image& img, int x, int y, const std::string& s, Color c, int scale) {
    // dark backing so labels read on any background
    const int w = static_cast<int>(s.size()) * 4 * scale, h = 5 * scale;
    for (int yy = y - 1; yy < y + h + 1; ++yy) {
        for (int xx = x - 1; xx < x + w; ++xx) blendPixel(img, xx, yy, {0, 0, 0, 255}, 0.55f);
    }
    for (char ch : s) {
        auto it = tinyFont().find(ch);
        if (it != tinyFont().end()) {
            for (int gy = 0; gy < 5; ++gy) {
                for (int gx = 0; gx < 3; ++gx) {
                    if (it->second[gy * 3 + gx] != '1') continue;
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) blendPixel(img, x + gx * scale + sx, y + gy * scale + sy, c);
                    }
                }
            }
        }
        x += 4 * scale;
    }
}

static std::string timeLabel(double s, double step) {
    char buf[32];
    if (step >= 60 && s >= 60) std::snprintf(buf, sizeof buf, "%d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    else if (step >= 1) std::snprintf(buf, sizeof buf, "%gs", std::round(s));
    else if (step >= 0.1) std::snprintf(buf, sizeof buf, "%.1fs", s);
    else std::snprintf(buf, sizeof buf, "%.2fs", s);
    return buf;
}

static double tickStep(double seconds, int width) {
    const double steps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600};
    for (double s : steps) {
        if (seconds / s * 90.0 <= width) return s;
    }
    return 7200;
}

void timeGrid(Image& img, double startSeconds, double seconds, Color grid, bool labels) {
    if (seconds <= 0) return;
    const int W = img.width();
    const double step = tickStep(seconds, W);
    for (double t = std::ceil(startSeconds / step) * step; t <= startSeconds + seconds + 1e-9; t += step) {
        const int x = static_cast<int>(std::lround((t - startSeconds) / seconds * (W - 1)));
        vline(img, x, 0, img.height() - 1, grid, 0.6f);
        if (labels && x + 40 < W) text(img, x + 3, img.height() - 13, timeLabel(t, step), {200, 205, 215, 255});
    }
}

}  // namespace achclwss::audio::detail
