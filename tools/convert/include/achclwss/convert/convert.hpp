// convert: change any file into another format. Picks a backend per pair:
// the built-in raster engine for images, the mesh library for 3D models
// (and mesh -> image renders), the vector tool for SVG, ffmpeg for
// audio and video, LibreOffice for office documents, pandoc for markup.
#pragma once

#include <filesystem>
#include <string>

#include "achclwss/core/command.hpp"

namespace achclwss::convert {

void registerCommands(Session& session);

// Which backend would handle input -> output, e.g. "raster", "mesh", "ffmpeg". Empty if none.
std::string backendFor(const Session& session, const std::filesystem::path& input, const std::string& targetExt);

// Converts one file. Returns a failed result with a reason instead of throwing.
Result convertFile(Session& session, const std::filesystem::path& input, const std::filesystem::path& output,
                   const Params& options);

}  // namespace achclwss::convert
