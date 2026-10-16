// Helpers shared by the maps sources: files, text, times, base64, a tiny XML reader.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/maps/maps.hpp"
#include "wetwaresuite/maps/tiles.hpp"

namespace wetwaresuite::maps::detail {

std::string readFile(const std::filesystem::path& path);                         // throws std::runtime_error
void writeFile(const std::filesystem::path& path, const std::string& bytes);     // creates parent folders; throws
std::string lower(std::string s);
std::string trim(const std::string& s);
std::string fmt(double v, int maxDecimals = 10);  // compact decimal, no exponent for normal map values
std::string jsonEscape(const std::string& s);
std::string xmlEscape(const std::string& s);
bool validUtf8(const std::string& s);
std::string latin1ToUtf8(const std::string& s);

// ISO 8601 ("2024-05-01T08:30:00Z", fractional seconds and +hh:mm offsets) <-> Unix seconds.
double parseTime(const std::string& iso);  // NaN when unreadable
std::string formatTime(double unixSeconds);
std::int64_t daysFromCivil(int y, unsigned m, unsigned d);
void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d);

std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);

// A small XML DOM: element names without namespace prefixes, attributes, text and CDATA.
struct XmlNode {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<std::unique_ptr<XmlNode>> children;

    const XmlNode* child(const std::string& n) const;
    std::vector<const XmlNode*> all(const std::string& n) const;  // direct children called n
    std::string childText(const std::string& n) const;           // trimmed, "" when missing
    std::string attr(const std::string& n) const;
};
std::unique_ptr<XmlNode> parseXml(const std::string& text);  // throws std::runtime_error

// Polygon rings with no orientation info: outer rings and holes by nesting depth.
std::vector<Polygon> assembleRings(std::vector<Ring> rings);

// ---- the built-in style for Protomaps basemap vector tiles (vector_style.cpp) ----

struct StyledPath {
    std::vector<std::vector<std::pair<double, double>>> parts;  // pixels
    bool closed = false;
    std::optional<Color> fill, stroke;
    double width = 1;
    std::string dash;  // "", "dash" or "dot"
    int order = 0;     // drawing order across all tiles (earth first, boundaries last)
};
struct StyledLabel {
    double x = 0, y = 0;
    std::string text;
    double size = 11;
    Color color;
    bool bold = false;
    double rank = 0;  // higher places first
};
// Tile units (0..extent) -> pixels.
using TileToPixels = std::function<std::pair<double, double>(double, double)>;
void styleVectorTile(const std::vector<MvtLayer>& layers, double zoom, const TileToPixels& toPx, int width, int height,
                     std::vector<StyledPath>& paths, std::vector<StyledLabel>& labels);
Color vectorWaterColor();

}  // namespace wetwaresuite::maps::detail
