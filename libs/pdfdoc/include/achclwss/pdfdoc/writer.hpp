// A small PDF writer: numbered objects, (compressed) streams, images, the
// standard fonts and embedded TrueType fonts. Callers build content streams
// with the PDF operators themselves; this keeps the file structure right.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "achclwss/pdfdoc/fonts.hpp"
#include "achclwss/raster/image.hpp"

namespace achclwss::pdfdoc {

class Writer {
public:
    int reserve();                          // a new object number to fill in later
    void set(int id, std::string body);     // body without "n 0 obj"/"endobj"
    int add(std::string body);
    // A stream object. `dict` holds extra entries (without << >> or /Length); data is
    // Flate-compressed when `compress` is true.
    int addStream(const std::string& dict, const std::string& data, bool compress = true);
    // The finished file. `info` may be 0.
    std::string finish(int root, int info, const std::string& version = "1.4") const;

private:
    std::vector<std::string> objects_;
};

std::string num(double v);                     // compact: up to 4 decimals, no trailing zeros
std::string literal(const std::string& bytes);  // (escaped string) of raw bytes
std::string text(const std::string& utf8);      // literal() of the WinAnsi bytes
std::string deflate(const std::string& data);   // zlib stream

// Image XObjects. RGB with an /SMask when any pixel is not opaque.
int addImage(Writer& w, const raster::Image& image);
// Embeds a baseline/progressive JPEG file unchanged (DCTDecode). Returns 0 if the bytes
// aren't a JPEG this can pass through (then decode and use addImage).
int addJpeg(Writer& w, const std::string& bytes, int* width = nullptr, int* height = nullptr);

// Font dictionaries using WinAnsiEncoding.
int addStandardFont(Writer& w, Standard font);
// Embeds the whole TrueType file. `widthOf` (1/1000 em per WinAnsi code) overrides the
// font's own advances, so a substitute font keeps another font's spacing.
int addTrueTypeFont(Writer& w, const TrueType& font, const std::function<double(int)>& widthOf = nullptr,
                    const std::string& nameSuffix = {});

}  // namespace achclwss::pdfdoc
