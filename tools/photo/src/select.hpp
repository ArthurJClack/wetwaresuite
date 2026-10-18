// Selections, layer masks, clipping masks, curves, colour balance and free
// transform (select.cpp).
#pragma once

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/document.hpp"

namespace wetwaresuite::photo {

void registerSelectCommands(Session& session);

// Rotates the whole image by any angle. mode: "expand" grows the canvas to fit, "keep" keeps the
// size (corners cut off), "crop" (straighten) cuts to the biggest rectangle with no empty corners.
// The bottom layer's new corners are filled with `fill`.
void rotateFree(raster::ImageDoc& doc, double degrees, const std::string& mode, raster::Color fill);

}  // namespace wetwaresuite::photo
