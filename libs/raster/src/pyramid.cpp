#include "wetwaresuite/raster/pyramid.hpp"

#include <algorithm>
#include <cmath>

#include "wetwaresuite/raster/parallel.hpp"
#include "wetwaresuite/raster/simd.hpp"
#include "simd_ops.hpp"

namespace wetwaresuite::raster {

void halveInto(const Image& src, Image& dst, const PixelRect& area) {
    const int sw = src.width(), sh = src.height();
    const int x0 = std::max(0, area.x0), x1 = std::min(dst.width(), area.x1);
    const int y0 = std::max(0, area.y0), y1 = std::min(dst.height(), area.y1);
    if (x1 <= x0 || y1 <= y0) return;
    parallelRows(y1 - y0, static_cast<std::size_t>(x1 - x0) * 4, [&](int b, int e) {
        for (int y = y0 + b; y < y0 + e; ++y) {
            const std::uint8_t* r0 = src.pixel(0, std::min(sh - 1, 2 * y));
            const std::uint8_t* r1 = src.pixel(0, std::min(sh - 1, 2 * y + 1));
            std::uint8_t* o = dst.pixel(x0, y);
#if WW_SIMD
            if (simdEnabled()) {
                // sums stay below 2^24, so float division truncates to exactly the integer quotient
                using namespace simd;
                for (int x = x0; x < x1; ++x, o += 4) {
                    const int xa = std::min(sw - 1, 2 * x) * 4, xb = std::min(sw - 1, 2 * x + 1) * 4;
                    const U4 acc = premul(r0 + xa) + premul(r0 + xb) + premul(r1 + xa) + premul(r1 + xb);
                    const std::uint32_t a = lastU(acc);
                    if (a == 0) {
                        o[0] = o[1] = o[2] = o[3] = 0;
                        continue;
                    }
                    const float div = static_cast<float>(a);
                    storeU8(o, toFloat(acc + setU(a / 2, a / 2, a / 2, 2)) / set(div, div, div, 4.0f));
                }
                continue;
            }
#endif
            for (int x = x0; x < x1; ++x, o += 4) {
                const int xa = std::min(sw - 1, 2 * x) * 4, xb = std::min(sw - 1, 2 * x + 1) * 4;
                const std::uint8_t* q[4] = {r0 + xa, r0 + xb, r1 + xa, r1 + xb};
                std::uint32_t acc[4] = {0, 0, 0, 0};
                for (const auto* p : q) {
                    acc[0] += p[0] * std::uint32_t{p[3]};
                    acc[1] += p[1] * std::uint32_t{p[3]};
                    acc[2] += p[2] * std::uint32_t{p[3]};
                    acc[3] += p[3];
                }
                const std::uint32_t a = acc[3], half = a / 2, div = a ? a : 1;
                for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>(a ? (acc[c] + half) / div : 0);
                o[3] = static_cast<std::uint8_t>((a + 2) / 4);
            }
        }
    });
}

void Pyramid::build(const LayeredImage& image) {
    levels_.clear();
    changed_.clear();
    levels_.push_back(composite(image.layers, image.width, image.height));
    while (levels_.back().width() > 1 || levels_.back().height() > 1) {
        const Image& prev = levels_.back();
        Image next((prev.width() + 1) / 2, (prev.height() + 1) / 2);
        halveInto(prev, next, {0, 0, next.width(), next.height()});
        levels_.push_back(std::move(next));
    }
    for (const auto& l : levels_) changed_.push_back({0, 0, l.width(), l.height()});
}

bool Pyramid::update(const LayeredImage& image, const PixelRect& area) {
    if (levels_.empty() || levels_[0].width() != image.width || levels_[0].height() != image.height) return false;
    for (auto& c : changed_) c = {};
    PixelRect r{std::max(0, area.x0), std::max(0, area.y0), std::min(image.width, area.x1), std::min(image.height, area.y1)};
    if (r.empty()) return true;
    Image part = compositeRegion(image.layers, r.x0, r.y0, r.width(), r.height());
    Image& base = levels_[0];
    for (int y = 0; y < r.height(); ++y) std::copy_n(part.pixel(0, y), static_cast<std::size_t>(r.width()) * 4, base.pixel(r.x0, r.y0 + y));
    changed_[0] = r;
    for (std::size_t i = 1; i < levels_.size(); ++i) {
        r = {r.x0 / 2, r.y0 / 2, (r.x1 + 1) / 2, (r.y1 + 1) / 2};
        halveInto(levels_[i - 1], levels_[i], r);
        changed_[i] = r;
    }
    return true;
}

int Pyramid::levelFor(double zoom, int maxSide) const {
    if (levels_.empty()) return 0;
    int level = 0;
    if (zoom > 0 && zoom < 1) level = static_cast<int>(std::floor(std::log2(1.0 / zoom) + 1e-9));
    level = std::clamp(level, 0, levels() - 1);
    while (level + 1 < levels() && maxSide > 0 && std::max(levels_[level].width(), levels_[level].height()) > maxSide) ++level;
    return level;
}

}  // namespace wetwaresuite::raster
