// Seeing and exporting: previews and single frames from the compositor (no
// ffmpeg), and exports with presets, ranges and burned-in captions: frames
// piped to ffmpeg with the mixed sound, stream copy for plain cuts, or WAV
// written directly.
#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "media.hpp"
#include "render_internal.hpp"
#include "wetwaresuite/audio/io.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

bool imageExt(const std::string& e) {
    return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "bmp" || e == "tga" || e == "tif" || e == "tiff";
}

std::string missingSource(const Timeline& tl) {
    std::error_code ec;
    for (const auto& t : tl.tracks) {
        for (const auto& c : t.clips) {
            if (c.kind == "nest" && c.nested) {
                if (std::string m = missingSource(*c.nested); !m.empty()) return m;
            } else if (!c.source.empty() && c.kind != "nest" && !fs::is_regular_file(c.source, ec)) {
                return "clip " + c.id + "'s source is missing: " + c.source;
            }
        }
    }
    return {};
}

Result writeFrame(const Timeline& tl, double at, const fs::path& output, int width, bool transparent) {
    if (tl.duration() <= 0) return Result::failure("the timeline is empty");
    if (at < 0 || at > tl.duration()) return Result::failure("t must be between 0 and " + num(tl.duration()) + "s");
    at = std::min(at, std::max(0.0, tl.duration() - 0.5 / tl.settings.fps));
    FrameRequest rq;
    rq.t = at;
    rq.width = width > 0 ? width : 0;
    rq.transparent = transparent;
    FrameContext ctx;
    raster::Image img = renderFrame(tl, rq, &ctx);
    raster::save(img, output);
    Result r = Result::success("saved the frame at " + num(at) + "s to " + output.string() + " (" + std::to_string(img.width()) + "x" +
                                   std::to_string(img.height()) + ")",
                               output.string());
    std::error_code ec;
    return r.with("output", output.string()).with("t", at).with("width", static_cast<std::int64_t>(img.width()))
        .with("height", static_cast<std::int64_t>(img.height())).with("bytes", static_cast<std::int64_t>(fs::file_size(output, ec)));
}

}  // namespace

Result exportTimeline(const Timeline& tl, const fs::path& output, const Params& p, ExportRequest rq) {
    if (std::string m = missingSource(tl); !m.empty()) return Result::failure(m);
    const std::string ext = extOf(output);
    if (std::string err = checkOutput(output, p); !err.empty()) return Result::failure(err);
    if (imageExt(ext)) return writeFrame(tl, numberParam(p, "at", rq.start), output, rq.width, false);
    // captions from a file are drawn by the compositor like any caption
    const Timeline* src = &tl;
    Timeline withCaptions;
    if (!rq.subtitles.empty()) {
        withCaptions = tl;
        addCaptions(withCaptions, parseCaptions(readFile(rq.subtitles)), titleTrack(withCaptions, true), 0, {});
        src = &withCaptions;
    }
    std::error_code ec;
    rq.output = fs::absolute(output, ec).string();
    const double total = src->duration();
    const double from = std::clamp(rq.start, 0.0, total), to = rq.end < 0 ? total : std::min(rq.end, total);
    // WAV needs no encoder: the mix is written directly
    if (ext == "wav" && (rq.preset.empty() || rq.preset == "audio_wav")) {
        if (to <= from + 1e-3) return Result::failure("the range to export is empty");
        if (!hasSound(*src)) return Result::failure("the timeline has no sound to export (every clip is silent or muted)");
        audio::save(mixAudio(*src, from, to), output);
        Result r = written("exported the sound of " + num(from) + "-" + num(to) + "s to " + output.string(), output);
        return r.with("mode", std::string("mix"));
    }
    if (!ffmpegPath()) return Result::failure(missingFfmpeg());
    TempDir tmp;
    rq.listPath = (tmp.path() / "list.txt").string();
    rq.audioPath = (tmp.path() / "mix.wav").string();
    EncodePlan plan;
    try {
        plan = planExport(*src, rq, hasSound(*src));
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
    if (plan.streamCopy) {
        if (!writeFile(rq.listPath, plan.concatList)) return Result::failure("can't write a temporary file");
        if (std::string err = runFfmpeg(plan.args); !err.empty()) return Result::failure(err);
    } else {
        if (plan.audio) {
            audio::SaveOptions o;
            o.format = audio::SampleFormat::Float32;
            audio::save(mixAudio(*src, plan.start, plan.start + plan.duration), rq.audioPath, o);
        }
        if (!plan.video) {
            if (std::string err = runFfmpeg(plan.args); !err.empty()) return Result::failure(err);
        } else {
            Stream enc;
            std::string err;
            if (!enc.start(plan.args, true, false, err)) return Result::failure(err);
            FrameContext ctx;
            const std::size_t bytes = static_cast<std::size_t>(plan.width) * plan.height * 4;
            bool ok = true;
            for (long i = 0; i < plan.frames && ok; ++i) {
                FrameRequest fr;
                fr.t = plan.start + static_cast<double>(i) / plan.fps;
                fr.width = plan.width;
                fr.height = plan.height;
                fr.reframe = plan.reframe;
                raster::Image img;
                try {
                    img = renderFrame(*src, fr, &ctx);
                } catch (const std::exception& e) {
                    enc.stop();
                    return Result::failure(std::string("frame at ") + num(fr.t) + "s: " + e.what());
                }
                if (img.width() != plan.width || img.height() != plan.height) {
                    raster::Image fixed(plan.width, plan.height, {0, 0, 0, 255});
                    for (int y = 0; y < std::min(plan.height, img.height()); ++y) {
                        std::copy_n(img.pixel(0, y), std::min(plan.width, img.width()) * 4, fixed.pixel(0, y));
                    }
                    img = std::move(fixed);
                }
                ok = enc.write(img.data(), bytes);
            }
            enc.closeInput();
            const int code = enc.wait();
            if (!ok || code != 0) return Result::failure("ffmpeg failed: " + enc.error());
        }
    }
    Result r = written("exported " + output.string() + (plan.streamCopy ? " by stream copy (no re-encoding)" : ""), output);
    r.with("mode", std::string(plan.streamCopy ? "copy" : "encode"));
    if (plan.video) r.with("frames", static_cast<std::int64_t>(plan.frames));
    return r;
}

void registerExportCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.preview", "Preview Frame",
             "Render the frame at time t (default the playhead) to a PNG and attach it, so you can see the edit: "
             "layers, picture-in-picture, titles, colour, keys, transitions. Drawn by the built-in compositor; only "
             "video files need ffmpeg to decode.",
             {docParam(), {"t", ParamType::Number, "time in seconds (default the playhead)", false},
              {"width", ParamType::Int, "preview width in px", false, {}, std::int64_t{640}}},
             false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const double at = numberParam(p, "t", tl.cursor);
                if (tl.duration() <= 0) return Result::failure("the timeline is empty");
                if (at < 0 || at >= tl.duration()) return Result::failure("t must be between 0 and " + num(tl.duration()) + "s");
                const int width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 640), 32, tl.settings.width));
                Result r = Result::success("frame at " + num(at) + "s of " + stringParam(p, "doc"));
                r.attach({"image/png", framePng(tl, at, width), stringParam(p, "doc") + "-" + num(at) + "s.png"});
                return r.with("t", at);
            }, false));

    reg.add({"cut.export_frame", "Export Frame",
             "Save one frame (default the playhead) as an image: .png, .jpg, .webp, .bmp, .tga or .tiff. "
             "transparent=true leaves the background out (PNG/WebP keep it). No ffmpeg needed for stills and titles.",
             {docParam(), {"output", ParamType::String, "image file to write", true},
              {"t", ParamType::Number, "time in seconds (default the playhead)", false},
              {"width", ParamType::Int, "image width (default the timeline's)", false},
              {"transparent", ParamType::Bool, "no background", false, {}, false}, overwriteParam()},
             false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const fs::path out = stringParam(p, "output");
                if (!imageExt(extOf(out))) return Result::failure("export a frame as .png, .jpg, .webp, .bmp, .tga or .tiff");
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                return writeFrame(tl, numberParam(p, "t", tl.cursor), out, static_cast<int>(intParam(p, "width", 0)), boolParam(p, "transparent", false));
            }, false));

    reg.add({"cut.presets", "Export Presets", "List the export presets cut.render takes (preset=...): size, shape, codecs and extension.",
             {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::string text;
                    std::string names;
                    for (const auto& pr : presets()) {
                        text += (text.empty() ? "" : "\n") + pr.name + " (." + pr.ext + "): " + pr.description;
                        names += (names.empty() ? "" : ",") + pr.name;
                    }
                    Result r = Result::success(text);
                    return r.with("presets", names);
                });
            });

    std::vector<std::string> presetNames;
    for (const auto& pr : presets()) presetNames.push_back(pr.name);
    reg.add({"cut.render", "Export",
             "Export the timeline: .mp4/.mov/.mkv (H.264 + AAC), .webm (VP9 + Opus), .gif, sound only (.wav needs no "
             "ffmpeg; .mp3, .m4a, .flac, .ogg) or one frame (.png/.jpg at `at`). preset picks ready-made settings "
             "(youtube_1080p, youtube_4k, vertical 9:16 for shorts/reels, square, high_quality, prores, gif, draft, "
             "audio_*; cut.presets lists them). range=in_out (or start/end) exports a part. Frames come from the "
             "built-in compositor and are encoded by ffmpeg; mode=auto stream-copies when the timeline is only plain "
             "cuts of identical sources (fast, lossless, cuts snap to keyframes).",
             {docParam(), {"output", ParamType::String, "file to write; the extension picks the format", true},
              {"preset", ParamType::String, "ready-made export settings", false, presetNames},
              {"quality", ParamType::String, "encoding effort: draft (fast, small), good, best (slow)", false, {"draft", "good", "best"}, std::string("good")},
              {"mode", ParamType::String, "auto, copy (fail unless stream copy is possible) or encode", false, {"auto", "copy", "encode"}, std::string("auto")},
              {"range", ParamType::String, "all, or in_out (between the in and out points)", false, {"all", "in_out"}, std::string("all")},
              {"start", ParamType::Number, "export from this time", false}, {"end", ParamType::Number, "export up to this time", false},
              {"width", ParamType::Int, "output width (default the preset's or the timeline's)", false},
              {"height", ParamType::Int, "output height", false},
              {"reframe", ParamType::String, "when the shape differs: fit (letterbox) or fill (crop)", false, {"fit", "fill"}},
              {"at", ParamType::Number, "png/jpg: time of the frame", false, {}, 0.0},
              {"subtitles", ParamType::String, "burn the captions of this .srt/.vtt file into the picture", false},
              {"gif_fps", ParamType::Int, "gif: frames per second", false, {}, std::int64_t{15}},
              {"gif_width", ParamType::Int, "gif: width in px", false, {}, std::int64_t{480}}, overwriteParam()},
             false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                ExportRequest rq;
                rq.preset = stringParam(p, "preset");
                rq.quality = stringParam(p, "quality", "good");
                rq.mode = stringParam(p, "mode", "auto");
                if (stringParam(p, "range", "all") == "in_out") {
                    if (tl.inPoint < 0 && tl.outPoint < 0) return Result::failure("there are no in/out points (cut.set_in_out sets them)");
                    rq.start = std::max(0.0, tl.inPoint);
                    rq.end = tl.outPoint;
                }
                if (has(p, "start")) rq.start = numberParam(p, "start");
                if (has(p, "end")) rq.end = numberParam(p, "end");
                rq.width = static_cast<int>(intParam(p, "width", 0));
                rq.height = static_cast<int>(intParam(p, "height", 0));
                rq.reframe = stringParam(p, "reframe");
                rq.gifFps = static_cast<int>(intParam(p, "gif_fps", 15));
                rq.gifWidth = static_cast<int>(intParam(p, "gif_width", 480));
                if (has(p, "subtitles")) rq.subtitles = requireInput(p, "subtitles");
                fs::path out = stringParam(p, "output");
                if (out.extension().empty() && !rq.preset.empty()) {
                    if (const Preset* pr = findPreset(rq.preset)) out += "." + pr->ext;
                }
                return exportTimeline(tl, out, p, rq);
            }, false));
}

}  // namespace wetwaresuite::cut
