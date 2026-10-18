// Shared bits of the beats command files: the song document, snapshot undo,
// param specs and result helpers.
#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/beats/beats.hpp"
#include "wetwaresuite/beats/song.hpp"
#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::beats {

namespace tool {

inline Result failed(const std::string& m) { return Result::failure(m); }

inline std::string num(double v, int precision = 4) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    return buf;
}

ParamSpec songParam();
ParamSpec songOrFileParam();  // optional song=, used with inputParam()
ParamSpec inputParam();
ParamSpec trackParam(bool required = true);
ParamSpec patternParam(bool required = true);
ParamSpec previewParam(const std::string& what = "the arrangement");

// Missing songs and exceptions become failed results; undoable commands snapshot the song.
ActionFactory songCommand(Session& session, std::function<Result(SongDoc&, const Params&)> fn, bool undoable = true);

// Track index from the "track" param (number or name); sets error when there's none.
bool trackArg(const Song& s, const Params& p, int& out, std::string& error);
Pattern* patternArg(Song& s, const Params& p, std::string& error, const std::string& key = "pattern");

// Success with song, tracks, patterns, bars, seconds (+ arrangement PNG when preview=true).
Result songResult(const std::string& message, const SongDoc& d, const Params& p);
// The same with the pattern's step grid / piano roll as the preview.
Result patternResult(const std::string& message, const SongDoc& d, const Pattern& pat, const Params& p);

std::string pngOf(const raster::Image& im);
// Readable listing of tracks, patterns and the arrangement.
std::string listing(const Song& s);
std::string sigText(const Song& s);
bool parseSignature(const std::string& text, int& top, int& bottom);
// "kick: x...x...; snare: ....x..." -> lanes. False with error on bad input.
bool parseLanes(const std::string& text, std::vector<std::pair<std::string, std::string>>& out, std::string& error);
// Sets one lane (replacing it, or removing it when steps is empty).
void setLane(Pattern& p, const std::string& lane, const std::string& steps);
// The kind of the first track a pattern is placed on (drum if it only has step lanes of drum voices).
TrackKind patternKind(const Song& s, const Pattern& p);

void registerSongCommands(Session& session);
void registerOutputCommands(Session& session);
void registerTrackCommands(Session& session);
void registerPatternCommands(Session& session);
void registerArrangeCommands(Session& session);

}  // namespace tool
}  // namespace wetwaresuite::beats
