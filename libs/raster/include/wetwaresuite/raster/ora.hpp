// OpenRaster (.ora): the layered format shared with Krita, GIMP and MyPaint.
// It's a ZIP of PNG layers plus stack.xml, so the suite's native layered files
// open in other painting apps too.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

struct LayeredImage {
    int width = 0;
    int height = 0;
    std::vector<Layer> layers;  // bottom first
};

void saveOra(const LayeredImage& image, const std::filesystem::path& path);
LayeredImage loadOra(const std::filesystem::path& path);  // throws std::runtime_error

// Minimal ZIP support (stored on write; stored or deflate on read), used by ORA.
struct ZipEntry {
    std::string name;
    std::string data;
};
std::string writeZip(const std::vector<ZipEntry>& entries);
std::vector<ZipEntry> readZip(const std::string& bytes);

}  // namespace wetwaresuite::raster
