// Internal: the parts of compositing shared by composite.cpp and styles.cpp.
#pragma once

#include <cstdint>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster::detail {

inline bool usesMask(const Layer& l) { return !l.mask.empty() && l.maskEnabled; }

// The layer's alpha over the w x h area at (x, y) after its own mask, 0..255. Layers without
// pixels (groups, adjustments) count as fully there except where their mask hides them.
std::vector<std::uint8_t> layerAlpha(const Layer& l, int x, int y, int w, int h);

// Draws a layer that has a style (shadows, glows, bevel, overlays, stroke) onto `out`, which
// covers the area at (x, y). clipAlpha (one byte per out pixel, or null) is the clipping base.
void drawStyledLayer(Image& out, const Layer& l, int x, int y, const std::uint8_t* clipAlpha);

}  // namespace wetwaresuite::raster::detail
