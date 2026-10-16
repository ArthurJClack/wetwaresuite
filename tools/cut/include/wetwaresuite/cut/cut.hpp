// cut: a headless video editor. A timeline document holds video and audio
// tracks of clips (video files, stills, titles, audio) with trims, speed,
// volume, fades, opacity, picture-in-picture geometry, colour filters and
// transitions. Rendering builds one ffmpeg filter_complex graph from the
// timeline (or stream-copies a plain cut-and-concat), so ffmpeg does the
// decoding and encoding and this library only plans the work.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::cut {

struct Settings {
    int width = 1920;
    int height = 1080;
    double fps = 30;
    std::string background = "black";
};

// Everything a clip can be: a video file, a still image, a title or an audio file.
struct Clip {
    std::string id;     // c1, c2, ...
    std::string kind;   // video, image, title, audio
    std::string source; // absolute path (empty for titles)

    // timing: the clip plays source seconds [in, out) starting at `start` on the timeline
    double in = 0, out = 0, start = 0;
    double speed = 1;
    double volume = 1;
    bool mute = false;
    double fadeIn = 0, fadeOut = 0;  // seconds on the timeline
    double opacity = 1;

    // picture: a box on the frame (0 width = the whole frame) and how the picture fills it
    double x = 0, y = 0, width = 0, height = 0;
    std::string fit = "fit";  // fit (letterbox), fill (crop to fill), stretch
    double cropX = 0, cropY = 0, cropW = 0, cropH = 0;  // source pixels, cropW 0 = no crop

    // colour
    double brightness = 0, contrast = 1, saturation = 1, gamma = 1;
    bool grayscale = false;
    std::string lut;  // .cube file

    // transition from the previous clip on the same track (they overlap by transitionDuration)
    std::string transition;
    double transitionDuration = 0;

    // titles
    std::string text, font, color = "white", position = "center", boxColor = "black@0.5";
    double fontSize = 72;
    bool box = false;

    // what the source is (filled by ffprobe when added, saved in the document)
    double sourceDuration = 0;
    int sourceWidth = 0, sourceHeight = 0;
    double sourceFps = 0;
    bool hasAudio = false;
    std::string videoCodec, audioCodec;

    double length() const { return speed > 0 ? (out - in) / speed : 0; }
    double end() const { return start + length(); }
    bool visual() const { return kind == "video" || kind == "image" || kind == "title"; }
    bool media() const { return kind == "video" || kind == "image"; }
    bool sound() const { return kind == "audio" || (kind == "video" && hasAudio); }
};

struct Track {
    std::string id;    // v1, v2 (v1 is at the bottom), a1, a2
    std::string kind;  // video or audio
    bool muted = false;
    bool hidden = false;
    std::vector<Clip> clips;  // kept sorted by start
};

class Timeline : public Document {
public:
    Settings settings;
    std::vector<Track> tracks;  // video tracks bottom-up, then audio tracks
    int nextClip = 1;

    std::string kind() const override { return "timeline"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    double duration() const;
    Track* track(const std::string& id);
    const Track* track(const std::string& id) const;
    // Finds or creates a track named v<n> or a<n>. Throws std::runtime_error for other names.
    Track& ensureTrack(const std::string& id);
    Clip* findClip(const std::string& id, Track** owner = nullptr);
    const Clip* findClip(const std::string& id) const;
    std::string newClipId() { return "c" + std::to_string(nextClip++); }
    void sortClips();
    // End of the last clip on a track (0 when empty or missing).
    double trackEnd(const std::string& id) const;
    // The media clip right before `clip` on its track, if any.
    const Clip* previousMedia(const Track& track, const Clip& clip) const;
};

std::string toJson(const Timeline& timeline);
// Throws std::runtime_error with a readable reason.
std::shared_ptr<Timeline> fromJson(const std::string& json, const std::filesystem::path& baseDir = {});

// One undo step: snapshots the timeline, runs the edit, restores it on failure or undo.
class TimelineEdit : public Action {
public:
    TimelineEdit(std::shared_ptr<Timeline> doc, std::function<Result(Timeline&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    struct Snapshot {
        Settings settings;
        std::vector<Track> tracks;
        int nextClip = 1;
    };
    std::shared_ptr<Timeline> doc_;
    std::function<Result(Timeline&)> fn_;
    Snapshot before_;
};

// What ffprobe says about a media file.
struct StreamInfo {
    std::string type;  // video, audio, subtitle, data
    std::string codec;
    int width = 0, height = 0;
    double fps = 0;
    int sampleRate = 0, channels = 0;
};

struct MediaInfo {
    double duration = 0;
    std::string format;
    bool hasVideo = false, hasAudio = false, still = false;
    int width = 0, height = 0;
    double fps = 0;
    std::string videoCodec, audioCodec;
    std::vector<StreamInfo> streams;
};

// Parses `ffprobe -print_format json -show_format -show_streams` output. Throws on bad JSON.
MediaInfo parseProbe(const std::string& json);
// Runs ffprobe. Throws std::runtime_error (including "install ffmpeg" when it is missing).
MediaInfo probe(const std::filesystem::path& path);

struct RenderRequest {
    std::string output;            // file to write; its extension picks the format
    std::string quality = "good";  // draft, good, best
    std::string mode = "auto";     // auto, copy (stream copy, no re-encode), encode
    double at = 0;                 // png/jpg output and previews: the time of the frame
    int stillWidth = 0;            // png/jpg output: scale to this width (0 = timeline size)
    std::string fontFile;          // for titles; empty = let ffmpeg/fontconfig pick
    std::string subtitles;         // .srt/.ass file to burn into the whole render
    int gifFps = 15, gifWidth = 480;
    std::string listPath;          // where the caller will write the concat list for stream copy
};

struct RenderPlan {
    std::vector<std::string> args;  // argv; args[0] is "ffmpeg", replace it with the real path
    bool streamCopy = false;
    std::string concatList;         // contents for request.listPath when streamCopy
    std::string graph;              // the filter_complex graph (empty for stream copy)
    double duration = 0;
};

// Plans an ffmpeg run for the timeline. Pure: touches no files. Throws std::runtime_error.
RenderPlan planRender(const Timeline& timeline, const RenderRequest& request);
// Why the timeline can't be rendered by stream copy to this extension, or empty if it can.
std::string streamCopyBlocker(const Timeline& timeline, const std::string& outputExt);

// ffmpeg filter option escaping (both levels: option value, then filtergraph).
std::string escapeOption(const std::string& value);
std::string escapeGraph(const std::string& text);
// name=k1=v1:k2=v2 with every value escaped for use inside -filter_complex.
std::string filter(const std::string& name, const std::vector<std::pair<std::string, std::string>>& options = {});

// A TrueType/OpenType font file usable by drawtext, or empty if none was found.
std::string findFont();

// The xfade transition name for a user-facing name (crossfade -> fade), empty if unknown.
std::string xfadeName(const std::string& type);
const std::vector<std::string>& transitionNames();

void registerCommands(Session& session);

}  // namespace wetwaresuite::cut
