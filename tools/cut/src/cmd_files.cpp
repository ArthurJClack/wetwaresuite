// One-shot helpers on media files (no timeline needed): trim, join, extract
// the sound, contact sheets, GIFs, speed, removing sound and burning subtitles.
// They run ffmpeg directly (or the timeline exporter) and stay out of the history.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "internal.hpp"
#include "media.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

// A one-clip timeline the size and rate of the file: how the one-shot helpers reuse the renderer.
std::shared_ptr<Timeline> timelineFor(const std::vector<std::string>& files, std::vector<MediaInfo>& infos) {
    auto tl = std::make_shared<Timeline>();
    double at = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        MediaInfo m = probe(files[i]);
        if (!m.hasVideo || m.still) throw std::runtime_error(files[i] + " isn't a video");
        if (i == 0) {
            tl->settings.width = std::max(16, m.width + (m.width % 2));
            tl->settings.height = std::max(16, m.height + (m.height % 2));
            tl->settings.fps = m.fps > 0 && m.fps <= 240 ? m.fps : 30;
        }
        Clip c;
        c.kind = "video";
        c.source = absolute(files[i]);
        fillFromProbe(c, m);
        c.out = m.duration;
        c.start = at;
        c.id = tl->newClipId();
        tl->ensureTrack("v1").clips.push_back(c);
        at = c.end();
        infos.push_back(m);
    }
    return tl;
}

// Wraps a one-shot helper so exceptions become failures.
ActionFactory oneShot(std::function<Result(const Params&)> fn) {
    return [fn](const Params& p) -> std::unique_ptr<Action> {
        return std::make_unique<FunctionAction>([fn, p] {
            try {
                if (!ffmpegPath()) return Result::failure(missingFfmpeg());
                return fn(p);
            } catch (const std::exception& e) {
                return Result::failure(e.what());
            }
        });
    };
}

std::vector<std::string> ffHead() { return {"ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-y"}; }

std::vector<std::string> audioCodecFor(const std::string& ext, const std::string& sourceCodec) {
    if (ext == "mp3") return sourceCodec == "mp3" ? std::vector<std::string>{"-c:a", "copy"} : std::vector<std::string>{"-c:a", "libmp3lame", "-q:a", "2"};
    if (ext == "wav") return {"-c:a", "pcm_s16le"};
    if (ext == "flac") return {"-c:a", "flac"};
    if (ext == "ogg" || ext == "opus") return sourceCodec == "opus" ? std::vector<std::string>{"-c:a", "copy"} : std::vector<std::string>{"-c:a", "libopus", "-b:a", "160k"};
    if (ext == "m4a" || ext == "aac") return sourceCodec == "aac" ? std::vector<std::string>{"-c:a", "copy"} : std::vector<std::string>{"-c:a", "aac", "-b:a", "192k"};
    if (ext == "mka") return {"-c:a", "copy"};
    throw std::runtime_error("extract audio to .mp3, .wav, .flac, .m4a, .aac, .ogg, .opus or .mka");
}

}  // namespace

void registerFileCommands(Session& session) {
    auto& reg = session.registry();
    // ---- one-shot helpers on files ----
    reg.add({"cut.trim_file", "Trim File",
             "Cut a piece out of a video or audio file into a new file. copy=true is instant and lossless but starts "
             "on the nearest keyframe; the default re-encodes for a frame-exact cut.",
             {{"input", ParamType::String, "source file", true}, {"output", ParamType::String, "file to write", true},
              {"start", ParamType::Number, "start time in seconds", false, {}, 0.0},
              {"end", ParamType::Number, "end time in seconds (or give duration)", false},
              {"duration", ParamType::Number, "length in seconds", false},
              {"copy", ParamType::Bool, "stream copy (no re-encode)", false, {}, false}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                const fs::path out = stringParam(p, "output");
                MediaInfo m = probe(in);
                const double start = numberParam(p, "start", 0);
                double end = has(p, "end") ? numberParam(p, "end") : has(p, "duration") ? start + numberParam(p, "duration") : m.duration;
                if (m.duration > 0) end = std::min(end, m.duration);
                if (start < 0 || end <= start + 1e-3) return Result::failure("the range is empty: start " + num(start) + "s, end " + num(end) + "s");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                std::vector<std::string> a = ffHead();
                a.insert(a.end(), {"-ss", num(start), "-i", in, "-t", num(end - start)});
                if (boolParam(p, "copy", false)) a.insert(a.end(), {"-map", "0", "-c", "copy", "-avoid_negative_ts", "make_zero"});
                else if (m.hasVideo && !m.still) a.insert(a.end(), {"-pix_fmt", "yuv420p"});
                a.push_back(fs::absolute(out).string());
                if (std::string err = runFfmpeg(a); !err.empty()) return Result::failure(err);
                return written("trimmed " + num(start) + "-" + num(end) + "s of " + in + " into " + out.string(), out);
            }));

    reg.add({"cut.concat_files", "Join Files",
             "Join video files end to end. Files with the same codecs, size and frame rate are joined by stream copy "
             "(instant, lossless); otherwise they're re-encoded to the first file's size and rate (letterboxed).",
             {{"inputs", ParamType::String, "the files in order, separated by commas (or | or new lines if names contain commas)", true},
              {"output", ParamType::String, "file to write", true},
              {"mode", ParamType::String, "auto, copy or encode", false, {"auto", "copy", "encode"}, std::string("auto")},
              {"quality", ParamType::String, "when re-encoding", false, {"draft", "good", "best"}, std::string("good")},
              overwriteParam()},
             false},
            oneShot([](const Params& p) {
                auto files = splitList(stringParam(p, "inputs"));
                if (files.size() < 2) return Result::failure("give at least two files to join, separated by commas");
                std::error_code ec;
                for (const auto& f : files) {
                    if (!fs::is_regular_file(f, ec)) return Result::failure("file not found: " + f);
                }
                std::vector<MediaInfo> infos;
                auto tl = timelineFor(files, infos);
                ExportRequest rq;
                rq.mode = stringParam(p, "mode", "auto");
                rq.quality = stringParam(p, "quality", "good");
                Result r = exportTimeline(*tl, stringParam(p, "output"), p, rq);
                if (r.ok) r.message = "joined " + std::to_string(files.size()) + " files into " + stringParam(p, "output") +
                                      (toText(*r.field("mode")) == "copy" ? " by stream copy" : " (re-encoded)");
                return r;
            }));

    reg.add({"cut.extract_audio", "Extract Audio",
             "Save a video's sound as an audio file (.mp3, .wav, .flac, .m4a, .aac, .ogg, .opus, .mka). The sound is "
             "copied without re-encoding when the format allows.",
             {{"input", ParamType::String, "video file", true}, {"output", ParamType::String, "audio file to write", true}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                const fs::path out = stringParam(p, "output");
                MediaInfo m = probe(in);
                if (!m.hasAudio) return Result::failure(in + " has no sound");
                auto codec = audioCodecFor(extOf(out), m.audioCodec);
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                std::vector<std::string> a = ffHead();
                a.insert(a.end(), {"-i", in, "-vn", "-map", "0:a:0"});
                a.insert(a.end(), codec.begin(), codec.end());
                a.push_back(fs::absolute(out).string());
                if (std::string err = runFfmpeg(a); !err.empty()) return Result::failure(err);
                return written("extracted the sound of " + in + " to " + out.string(), out);
            }));

    reg.add({"cut.thumbnails", "Contact Sheet",
             "Make a contact sheet: a grid of frames taken evenly through a video, as one image. Handy to see what "
             "a video contains at a glance (preview=true attaches it).",
             {{"input", ParamType::String, "video file", true}, {"output", ParamType::String, "image to write (.png or .jpg)", true},
              {"columns", ParamType::Int, "frames across", false, {}, std::int64_t{4}},
              {"rows", ParamType::Int, "frames down", false, {}, std::int64_t{3}},
              {"width", ParamType::Int, "width of each frame in px", false, {}, std::int64_t{320}},
              {"preview", ParamType::Bool, "attach the sheet", false, {}, false}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                const fs::path out = stringParam(p, "output");
                const auto cols = std::clamp<std::int64_t>(intParam(p, "columns", 4), 1, 20);
                const auto rows = std::clamp<std::int64_t>(intParam(p, "rows", 3), 1, 20);
                const auto width = std::clamp<std::int64_t>(intParam(p, "width", 320), 16, 2000);
                MediaInfo m = probe(in);
                if (!m.hasVideo) return Result::failure(in + " has no picture");
                if (!(m.duration > 0)) return Result::failure("ffprobe couldn't tell how long " + in + " is");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                const double n = static_cast<double>(cols * rows);
                char rate[64];
                std::snprintf(rate, sizeof rate, "%.6f", n / m.duration);
                const std::string vf = filter("fps", {{"fps", rate}}) + "," +
                                       filter("scale", {{"w", std::to_string(width)}, {"h", "-2"}}) + "," +
                                       filter("tile", {{"layout", std::to_string(cols) + "x" + std::to_string(rows)},
                                                       {"padding", "4"}, {"margin", "4"}, {"color", "black"}});
                std::vector<std::string> a = ffHead();
                a.insert(a.end(), {"-i", in, "-an", "-vf", vf, "-frames:v", "1", "-update", "1"});
                if (extOf(out) == "jpg" || extOf(out) == "jpeg") a.insert(a.end(), {"-q:v", "3"});
                a.push_back(fs::absolute(out).string());
                if (std::string err = runFfmpeg(a); !err.empty()) return Result::failure(err);
                Result r = written("contact sheet of " + in + ": " + std::to_string(cols) + "x" + std::to_string(rows) + " frames in " + out.string(), out);
                if (boolParam(p, "preview", false)) {
                    r.attach({extOf(out) == "png" ? "image/png" : "image/jpeg", readFile(out), out.filename().string()});
                }
                return r;
            }));

    reg.add({"cut.gif", "Make GIF",
             "Turn part of a video into an animated GIF with a proper palette (good colours, small file).",
             {{"input", ParamType::String, "video file", true}, {"output", ParamType::String, ".gif to write", true},
              {"start", ParamType::Number, "start time in seconds", false, {}, 0.0},
              {"duration", ParamType::Number, "length in seconds", false, {}, 3.0},
              {"fps", ParamType::Int, "frames per second", false, {}, std::int64_t{12}},
              {"width", ParamType::Int, "width in px (height keeps the aspect)", false, {}, std::int64_t{480}}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                const fs::path out = stringParam(p, "output");
                if (extOf(out) != "gif") return Result::failure("the output must be a .gif");
                const double start = numberParam(p, "start", 0), dur = numberParam(p, "duration", 3);
                if (start < 0 || dur <= 0 || dur > 120) return Result::failure("give a start >= 0 and a duration of 0..120 seconds");
                const auto fps = std::clamp<std::int64_t>(intParam(p, "fps", 12), 1, 50);
                const auto width = std::clamp<std::int64_t>(intParam(p, "width", 480), 16, 1920);
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                const std::string graph = "[0:v]" + filter("fps", {{"fps", std::to_string(fps)}}) + "," +
                                          filter("scale", {{"w", std::to_string(width)}, {"h", "-1"}, {"flags", "lanczos"}}) +
                                          ",split[a][b];[a]" + filter("palettegen", {{"stats_mode", "diff"}}) +
                                          "[pal];[b][pal]" + filter("paletteuse", {{"dither", "bayer"}, {"bayer_scale", "4"}}) + "[out]";
                std::vector<std::string> a = ffHead();
                a.insert(a.end(), {"-ss", num(start), "-t", num(dur), "-i", in, "-filter_complex", graph, "-map", "[out]", "-loop", "0",
                                   fs::absolute(out).string()});
                if (std::string err = runFfmpeg(a); !err.empty()) return Result::failure(err);
                return written("made " + out.string() + " from " + num(start) + "-" + num(start + dur) + "s of " + in, out);
            }));

    reg.add({"cut.speed", "Change Speed",
             "Speed a whole video up or slow it down (picture and sound; the sound keeps its pitch).",
             {{"input", ParamType::String, "video file", true}, {"output", ParamType::String, "file to write", true},
              {"factor", ParamType::Number, "2 = twice as fast, 0.5 = half speed (0.05..20)", true},
              {"quality", ParamType::String, "encoding preset", false, {"draft", "good", "best"}, std::string("good")}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                std::vector<MediaInfo> infos;
                auto tl = timelineFor({in}, infos);
                Clip& c = tl->tracks.front().clips.front();
                c.speed = numberParam(p, "factor");
                if (std::string err = validate(c); !err.empty()) return Result::failure(err);
                ExportRequest rq;
                rq.mode = "encode";
                rq.quality = stringParam(p, "quality", "good");
                Result r = exportTimeline(*tl, stringParam(p, "output"), p, rq);
                if (r.ok) r.message = "changed the speed of " + in + " by " + num(c.speed) + "x into " + stringParam(p, "output");
                return r;
            }));

    reg.add({"cut.mute", "Remove Sound",
             "Copy a video without its sound track (the picture isn't re-encoded).",
             {{"input", ParamType::String, "video file", true}, {"output", ParamType::String, "file to write", true}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                const fs::path out = stringParam(p, "output");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                std::vector<std::string> a = ffHead();
                a.insert(a.end(), {"-i", in, "-map", "0:v", "-c", "copy", "-an", fs::absolute(out).string()});
                if (std::string err = runFfmpeg(a); !err.empty()) return Result::failure(err);
                return written("copied " + in + " without sound to " + out.string(), out);
            }));

    reg.add({"cut.subtitles", "Burn Subtitles",
             "Draw an .srt (or .ass) subtitle file permanently into a video's picture, so they show everywhere.",
             {{"input", ParamType::String, "video file", true}, {"subtitles", ParamType::String, ".srt or .ass file", true},
              {"output", ParamType::String, "file to write", true},
              {"font_size", ParamType::Int, "subtitle size (libass points, roughly px at 288p; 0 = default)", false, {}, std::int64_t{0}},
              {"color", ParamType::String, ".srt only: text colour as #rrggbb", false},
              {"quality", ParamType::String, "encoding preset", false, {"draft", "good", "best"}, std::string("good")}, overwriteParam()},
             false},
            oneShot([](const Params& p) {
                const std::string in = requireInput(p);
                const std::string subs = requireInput(p, "subtitles");
                const fs::path out = stringParam(p, "output");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                std::vector<std::pair<std::string, std::string>> o = {{"filename", fs::absolute(subs).generic_string()}};
                const std::string font = findFont();
                if (!font.empty()) o.push_back({"fontsdir", fs::path(font).parent_path().generic_string()});
                std::string style;
                if (intParam(p, "font_size", 0) > 0) style += "FontSize=" + std::to_string(intParam(p, "font_size"));
                if (has(p, "color")) {
                    std::string hex = stringParam(p, "color");
                    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
                    if (hex.size() != 6) return Result::failure("color takes #rrggbb");
                    // ASS colours are &HBBGGRR
                    style += std::string(style.empty() ? "" : ",") + "PrimaryColour=&H00" + hex.substr(4, 2) + hex.substr(2, 2) + hex.substr(0, 2);
                }
                if (!style.empty()) o.push_back({"force_style", style});
                std::vector<std::string> a = ffHead();
                const std::string quality = stringParam(p, "quality", "good");
                const std::string preset = quality == "draft" ? "ultrafast" : quality == "best" ? "slow" : "veryfast";
                const std::string crf = quality == "draft" ? "28" : quality == "best" ? "18" : "22";
                a.insert(a.end(), {"-i", in, "-filter_complex", "[0:v]" + filter("subtitles", o) + "[v]", "-map", "[v]", "-map", "0:a?",
                                   "-c:v", "libx264", "-preset", preset, "-crf", crf, "-pix_fmt", "yuv420p", "-c:a", "copy",
                                   fs::absolute(out).string()});
                std::string err = runFfmpeg(a);
                const std::string ext = extOf(subs);
                if (err.find("No such filter: 'subtitles'") != std::string::npos && (ext == "srt" || ext == "vtt")) {
                    // ffmpeg built without libass (Homebrew's): the compositor draws the captions instead
                    std::error_code ec;
                    fs::remove(out, ec);  // in case the failed run left a stub
                    std::vector<MediaInfo> infos;
                    auto tl = timelineFor({in}, infos);
                    ExportRequest rq;
                    rq.mode = "encode";
                    rq.quality = quality;
                    rq.subtitles = subs;
                    Result r = exportTimeline(*tl, out, p, rq);
                    if (r.ok) r.message = "burned " + subs + " into " + out.string();
                    return r;
                }
                if (!err.empty()) return Result::failure(err);
                return written("burned " + subs + " into " + out.string(), out);
            }));
}

}  // namespace wetwaresuite::cut
