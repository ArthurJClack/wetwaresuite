// Shared bits of the audio tool's command files.
#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/analysis.hpp"
#include "wetwaresuite/audio/document.hpp"
#include "wetwaresuite/audio/dsp.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::audio::tool {

namespace fs = std::filesystem;

// Per-session state shared by the commands.
struct State {
    Buffer clipboard;
    bool hasClipboard = false;
};

inline Result failed(const std::string& message) { return Result::failure(message); }

inline std::string num(double v, int precision = 4) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    return buf;
}

inline std::string dbText(double db) {
    if (db <= -200) return "-inf dB";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f dB", db);
    return buf;
}

inline std::vector<ParamSpec> concat(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// start, end (+ channels) (+ preview)
inline std::vector<ParamSpec> regionParams(bool channels = true, bool preview = true) {
    std::vector<ParamSpec> v = {startParam(), endParam()};
    if (channels) v.push_back(channelsParam());
    if (preview) v.push_back(previewParam());
    return v;
}

std::string lowerExt(const fs::path& p);
// "16", "24", "32f"... from the bits param (default 16), plus bitrate and dither.
bool saveOptionsArg(const Params& p, SaveOptions& out, std::string& error);
// Adds peak_db, true_peak_db, rms_db, lufs... fields and returns a readable report.
std::string levelsReport(Result& r, const Levels& lv);
// A command editing one audio document over a time range and channels.
using RegionEdit = std::function<Result(AudioDoc&, const Params&, Range, const Channels&)>;
ActionFactory regionCommand(Session& session, RegionEdit fn, bool undoable = true);
// Replaces frames r of the buffer with `with` (which may be a different length).
void replaceRange(Buffer& b, Range r, const Buffer& with);
// Every file in a folder that can be opened as audio, sorted.
std::vector<fs::path> audioFiles(const fs::path& dir, bool recursive);

void registerFileCommands(Session& session, const std::shared_ptr<State>& state);
void registerEditCommands(Session& session, const std::shared_ptr<State>& state);
void registerProjectCommands(Session& session);
void registerPlayCommands(Session& session);

}  // namespace wetwaresuite::audio::tool
