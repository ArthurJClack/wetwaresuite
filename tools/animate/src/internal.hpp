// Helpers shared by the animate sources.
#pragma once

#include <filesystem>
#include <string>

#include "achclwss/animate/animate.hpp"

namespace achclwss::animate::detail {

std::string lower(std::string s);
std::string num(double v);  // 3 decimals at most, no trailing zeros

std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);  // skips anything that isn't base64

// Text elements: the box their lines fill at `size` px, and the lines drawn onto a transparent image
// at `size` x `k` px (so the image is k times the box).
void textBox(const Element& e, double size, double& w, double& h);
Image renderText(const Element& e, double size, Color color, double k);

std::string readFile(const std::filesystem::path& path);
bool writeFile(const std::filesystem::path& path, const std::string& bytes);

// A fresh temporary folder, removed when this goes out of scope.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace achclwss::animate::detail
