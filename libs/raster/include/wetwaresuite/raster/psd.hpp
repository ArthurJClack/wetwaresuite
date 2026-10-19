// Layered Photoshop files (.psd, 8-bit RGB): layer names, pixels, opacity, fill, blend modes,
// visibility, alpha lock, layer masks, clipping and groups go both ways. Photoshop's own
// adjustment layers, layer effects and live text need Photoshop's internal formats, so:
// on save, layer styles are drawn into the layer's pixels, text and shape layers are saved as
// their pixels and adjustment layers can't be kept (see the notes savePsd returns); on open,
// those layers come in as the pixels Photoshop stored for them.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/raster/ora.hpp"

namespace wetwaresuite::raster {

// Returns plain-word notes about anything the file couldn't keep (empty when it kept everything).
std::vector<std::string> savePsd(const LayeredImage& image, const std::filesystem::path& path);
std::string encodePsd(const LayeredImage& image, std::vector<std::string>* notes = nullptr);
// Throws std::runtime_error for files it can't read layer by layer (16-bit, CMYK, ZIP-compressed
// layers...); callers then fall back to the flattened picture.
LayeredImage loadPsd(const std::filesystem::path& path);
LayeredImage decodePsd(const std::string& bytes);

}  // namespace wetwaresuite::raster
