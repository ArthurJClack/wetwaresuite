// Helpers shared by forge's image and sprite commands.
#pragma once

#include <optional>
#include <string>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::forge::detail {

// An open image id (img1) or an image file, flattened.
std::optional<raster::Image> resolveImage(Session& s, const std::string& ref, std::string& error);
// The image a command reads: its "doc" param, else its "input" param.
std::optional<raster::Image> sourceImage(Session& s, const Params& p, std::string& error, std::string* name = nullptr);
// Writes an image file (format from the extension), creating folders. Empty string = ok, else the error.
std::string writeImage(const raster::Image& img, const std::string& path);
// Finishes a command that made a new image: opens it as a new document,
// writes "output" when given, adds doc/width/height/output fields and a
// preview when asked (or when previewByDefault).
Result generated(Session& s, raster::Image img, Result r, const Params& p, bool previewByDefault = false);

ParamSpec docSourceParam();
ParamSpec inputParam();
ParamSpec outputParam(const std::string& description = "image file to write the result to");
ParamSpec imagePreviewParam(bool fallback = false);

}  // namespace wetwaresuite::forge::detail
