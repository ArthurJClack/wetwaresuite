// A mipmap pyramid of a layered image for viewers: level 0 is the flattened picture, each next
// level is half the size of the one before (2x2 average), down to 1 px. When zoomed out a viewer
// draws from a small level instead of the full picture, and after an edit only the changed area is
// re-flattened and shrunk again, so pan and zoom stay smooth on giant images.
#pragma once

#include <vector>

#include "wetwaresuite/raster/ora.hpp"

namespace wetwaresuite::raster {

class Pyramid {
public:
    // Flattens the whole image and builds every level.
    void build(const LayeredImage& image);
    // Re-flattens `area` (in full-size pixels) and refreshes it in every level. Returns false
    // (and does nothing) when the image size no longer matches, so the caller should build().
    bool update(const LayeredImage& image, const PixelRect& area);

    int levels() const { return static_cast<int>(levels_.size()); }
    const Image& level(int i) const { return levels_.at(static_cast<std::size_t>(i)); }
    // The area of level i that changed in the last build() or update().
    PixelRect changed(int i) const { return changed_.at(static_cast<std::size_t>(i)); }

    // The level to draw at `zoom` (screen px per picture px): the smallest one that still has at
    // least one pixel per screen pixel, and whose sides fit in maxSide (e.g. the GPU texture limit).
    int levelFor(double zoom, int maxSide) const;

private:
    std::vector<Image> levels_;
    std::vector<PixelRect> changed_;
};

// Halves `src` into `dst` (sized (w+1)/2 x (h+1)/2) over the dst rectangle `area`, averaging
// each 2x2 block with alpha weighting; odd edges repeat the last row or column.
void halveInto(const Image& src, Image& dst, const PixelRect& area);

}  // namespace wetwaresuite::raster
