// Reading and writing raster files. Single-image formats go through stb;
// layered OpenRaster (.ora) lives in ora.hpp.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

// Lowercase extensions without the dot.
const std::vector<std::string>& readableExtensions();  // png jpg jpeg bmp tga gif psd hdr pic ppm pgm pnm
const std::vector<std::string>& writableExtensions();  // png jpg jpeg bmp tga ppm

bool canRead(const std::filesystem::path& path);
bool canWrite(const std::filesystem::path& path);

// Throws std::runtime_error with a readable reason.
Image load(const std::filesystem::path& path);
Image decode(const std::string& bytes);

// Format is picked from the extension. quality applies to JPEG (1..100).
void save(const Image& image, const std::filesystem::path& path, int quality = 90);

std::string encodePng(const Image& image);
std::string encodeJpeg(const Image& image, int quality = 85);

// Image scaled to fit in maxSize x maxSize, encoded as PNG: what previews use.
std::string previewPng(const Image& image, int maxSize = 512);

}  // namespace wetwaresuite::raster
