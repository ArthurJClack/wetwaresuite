// Helpers shared by the maps sources: files, text, times, base64, a tiny XML reader.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "achclwss/maps/maps.hpp"

namespace achclwss::maps::detail {

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

}  // namespace achclwss::maps::detail
