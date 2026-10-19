// Running ffmpeg/ffprobe for the cut tool (never through a shell).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wetwaresuite::cut {

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

// A helper program whose stdin we write and/or whose stdout we read as raw bytes
// (frames to an encoder, frames from a decoder). Its stderr goes to a file so the
// data stays clean; error() returns the end of it. Never through a shell.
class Stream {
public:
    Stream();
    ~Stream();  // stops it if it still runs
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    // args[0] is found on PATH. False with `error` set when it can't start.
    bool start(const std::vector<std::string>& args, bool writeInput, bool readOutput, std::string& error);
    bool write(const void* data, std::size_t bytes);  // false once the program has gone
    // Reads exactly `bytes`; false at the end of its output (or on an error).
    bool read(void* data, std::size_t bytes);
    void closeInput();
    // Waits for it to end; its exit code (-1 when it couldn't be waited for).
    int wait();
    void stop();
    std::string error() const;  // the tail of what it printed on stderr

private:
    struct Impl;
    Impl* impl_;
};

std::string readFile(const std::filesystem::path& path);
bool writeFile(const std::filesystem::path& path, const std::string& bytes);

}  // namespace wetwaresuite::cut
