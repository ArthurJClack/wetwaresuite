// Running ffmpeg/ffprobe for the cut tool (never through a shell).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace achclwss::cut {

std::optional<std::string> ffmpegPath();
std::optional<std::string> ffprobePath();
std::string missingFfmpeg();  // the "install ffmpeg" message

// Runs ffmpeg with args (args[0] is replaced by the real path). Returns an error message, empty on success.
std::string runFfmpeg(std::vector<std::string> args);

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

std::string readFile(const std::filesystem::path& path);
bool writeFile(const std::filesystem::path& path, const std::string& bytes);

}  // namespace achclwss::cut
