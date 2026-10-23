// Reading and writing raster files. Most single-image formats go through stb,
// TIFF has its own reader/writer and WebP uses libwebp; layered OpenRaster (.ora)
// lives in ora.hpp.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

// Lowercase extensions without the dot.
const std::vector<std::string>& readableExtensions();  // png jpg jpeg bmp tga gif psd hdr pic ppm pgm pnm tif tiff webp
const std::vector<std::string>& writableExtensions();  // png jpg jpeg bmp tga ppm tif tiff webp

bool canRead(const std::filesystem::path& path);
bool canWrite(const std::filesystem::path& path);

// Throws std::runtime_error with a readable reason.
Image load(const std::filesystem::path& path);
Image decode(const std::string& bytes);

// Format is picked from the extension. quality applies to JPEG and WebP (1..100; WebP at 100 is lossless).
void save(const Image& image, const std::filesystem::path& path, int quality = 90);

std::string encodePng(const Image& image);
std::string encodeJpeg(const Image& image, int quality = 85);

// TIFF: the first page of baseline files (see tiff.cpp); written as 8-bit RGB(A), Deflate.
Image decodeTiff(const std::string& bytes);
std::string encodeTiff(const Image& image);

// WebP (still images). webpSupported() is false when the build has no libwebp; then these throw.
bool webpSupported();
Image decodeWebp(const std::string& bytes);
std::string encodeWebp(const Image& image, int quality = 90);

// zlib streams (used by TIFF, FBX, ...). inflateZlib throws std::runtime_error on bad data.
std::string inflateZlib(const std::string& data);
std::string deflateZlib(const std::string& data, int level = 6);

// Image scaled to fit in maxSize x maxSize, encoded as PNG: what previews use.
std::string previewPng(const Image& image, int maxSize = 512);

}  // namespace wetwaresuite::raster
