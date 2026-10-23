// The photo command files beyond photo.cpp, and what they share.
#pragma once

#include <functional>
#include <string>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/document.hpp"

namespace wetwaresuite::photo {

void registerAdjustmentCommands(Session& session);  // adjust_cmds.cpp: adjustment layers
void registerFilterCommands(Session& session);      // filter_cmds.cpp: blur, sharpen, noise, stylize, render
void registerDistortCommands(Session& session);     // filter_cmds.cpp: displace, lens, warp, liquify
void registerSubjectCommands(Session& session);     // filter_cmds.cpp: select subject, refine edge

// Runs fn on the chosen layer's pixels, or every pixel layer's with all_layers=true. Text and
// shape layers become pixels; groups and adjustment layers give an error (or are skipped).
Result forPixelLayers(raster::ImageDoc& doc, const Params& p, const std::function<void(raster::Image&)>& fn);

}  // namespace wetwaresuite::photo
