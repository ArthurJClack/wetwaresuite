// Small drawing helpers for waveform, spectrogram and project pictures.
#pragma once

#include <string>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::audio::detail {

void blendPixel(raster::Image& img, int x, int y, raster::Color c, float alpha = 1.0f);
void vline(raster::Image& img, int x, int y0, int y1, raster::Color c, float alpha = 1.0f);
void hline(raster::Image& img, int x0, int x1, int y, raster::Color c, float alpha = 1.0f, int dash = 0);
// Labels in a 3x5 pixel font (digits, ".:-", "kHzsmdB"), on a dark backing.
void text(raster::Image& img, int x, int y, const std::string& s, raster::Color c, int scale = 2);
// Vertical lines with time labels along the bottom.
void timeGrid(raster::Image& img, double startSeconds, double seconds, raster::Color grid, bool labels);

}  // namespace wetwaresuite::audio::detail
