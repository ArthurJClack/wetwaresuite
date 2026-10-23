// Titles, lower thirds and captions (.srt/.vtt in and out). They're drawn by
// the compositor, so captions on the timeline are burned into every export.
#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "media.hpp"

namespace wetwaresuite::cut {

namespace {

const std::vector<std::string> kCaptionStyle = {"font", "font_size", "color", "box", "box_color", "position", "outline", "outline_color",
                                                "shadow", "bold", "italic"};

Params styleOnly(const Params& p) {
    Params q;
    for (const auto& k : kCaptionStyle) {
        if (has(p, k)) q[k] = p.at(k);
    }
    return q;
}

std::vector<ParamSpec> titleSpecs(std::vector<ParamSpec> head, const std::vector<std::string>& names) {
    for (auto& x : propSpecs(names)) head.push_back(x);
    head.push_back(previewParam());
    return head;
}

Result addTitleClip(Timeline& tl, const Params& p, Clip c, double duration, bool captions, const std::string& what) {
    std::string track = stringParam(p, "track");
    if (track.empty()) track = titleTrack(tl, captions);
    Params q = p;
    q["track"] = track;
    if (!has(q, "start")) q["start"] = tl.cursor;
    if (!has(q, "duration")) q["duration"] = duration;
    return addClip(tl, q, c, track, what);
}

}  // namespace

Clip makeCaption(const Timeline& tl, const std::string& text, double start, double end) {
    Clip c;
    c.kind = "title";
    c.caption = true;
    c.text = text;
    c.start = start;
    c.out = std::max(0.05, end - start);
    c.position = "bottom";
    c.box = true;
    c.boxColor = "black@0.6";
    c.fontSize = std::max(16.0, std::round(tl.settings.height / 18.0));
    return c;
}

int addCaptions(Timeline& tl, const std::vector<Cue>& cues, const std::string& track, double offset, const Params& p) {
    tl.ensureTrack(track);
    const Params style = styleOnly(p);
    int n = 0;
    for (const auto& cue : cues) {
        if (cue.end + offset <= 0 || cue.text.empty()) continue;
        Clip c = makeCaption(tl, cue.text, std::max(0.0, cue.start + offset), cue.end + offset);
        if (std::string err = applyProps(c, style); !err.empty()) throw std::runtime_error(err);
        if (std::string err = validate(c); !err.empty()) throw std::runtime_error("caption at " + num(cue.start) + "s: " + err);
        c.id = tl.newClipId();
        tl.track(track)->clips.push_back(c);
        ++n;
    }
    return n;
}

void registerTitleCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.add_title", "Add Title",
             "Draw text over the video for a while: a heading, a credit, a quote. position places it (center, top, "
             "bottom, lower-third, corners, or custom with x,y); outline and shadow make it readable on busy "
             "pictures; anim_in/anim_out animate it (fade, slides, zoom, typewriter, wipe); x, y, scale, rotation and "
             "opacity can be keyframed. By default it starts at the playhead on a titles track above everything.",
             titleSpecs({docParam(), {"text", ParamType::String, "what the title says; \\n starts a new line", true},
                         {"track", ParamType::String, "video track (default: a titles track on top of everything)", false},
                         {"start", ParamType::Number, "when it appears, in seconds (default the playhead)", false},
                         {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 3.0}},
                        {"position", "font_size", "color", "font", "bold", "italic", "align", "box", "box_color", "outline", "outline_color",
                         "shadow", "shadow_color", "anim_in", "anim_out", "anim_duration", "x", "y", "fade_in", "fade_out", "opacity", "blend"}),
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip c;
                c.kind = "title";
                c.out = 3;
                return addTitleClip(tl, p, c, 3, false, "title");
            }));

    reg.add({"cut.add_lower_third", "Add Lower Third",
             "A name strap in the lower left: the name, a smaller second line (a job, a place) on a box with an "
             "accent bar, sliding in and fading out. Change its look afterwards with cut.set_clip.",
             titleSpecs({docParam(), {"text", ParamType::String, "the main line, e.g. a name", true},
                         {"subtitle", ParamType::String, "the smaller second line", false},
                         {"track", ParamType::String, "video track (default: a titles track on top)", false},
                         {"start", ParamType::Number, "when it appears (default the playhead)", false},
                         {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 5.0}},
                        {"accent_color", "color", "font", "font_size", "box_color", "anim_in", "anim_out", "anim_duration"}),
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip c;
                c.kind = "title";
                c.style = "lower_third";
                c.position = "lower-third";
                c.align = "left";
                c.box = true;
                c.boxColor = "black@0.65";
                c.fontSize = std::max(20.0, std::round(tl.settings.height / 20.0));
                c.animIn = "slide_right";
                c.animOut = "fade";
                c.out = 5;
                return addTitleClip(tl, p, c, 5, false, "lower third");
            }));

    reg.add({"cut.add_caption", "Add Caption",
             "Add one caption (subtitle) line: boxed text at the bottom on the captions track. cut.export_captions "
             "writes all of them as .srt or .vtt; every export shows them burned in.",
             titleSpecs({docParam(), {"text", ParamType::String, "what is said", true},
                         {"start", ParamType::Number, "when it appears (default the playhead)", false},
                         {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 3.0},
                         {"track", ParamType::String, "video track (default the captions track)", false}},
                        kCaptionStyle),
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip c = makeCaption(tl, stringParam(p, "text"), 0, 3);
                return addTitleClip(tl, p, c, 3, true, "caption");
            }));

    reg.add({"cut.import_captions", "Import Captions",
             "Read a SubRip (.srt) or WebVTT (.vtt) file and put each cue on the captions track as a caption clip, "
             "styled by the options given. replace=true removes the captions already there first.",
             titleSpecs({docParam(), {"path", ParamType::String, ".srt or .vtt file", true},
                         {"offset", ParamType::Number, "shift every cue by this many seconds", false, {}, 0.0},
                         {"track", ParamType::String, "video track (default the captions track)", false},
                         {"replace", ParamType::Bool, "remove the existing captions first", false, {}, false}},
                        kCaptionStyle),
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const std::string path = requireInput(p, "path");
                const auto cues = parseCaptions(readFile(path));
                if (boolParam(p, "replace", false)) {
                    for (auto& t : tl.tracks) {
                        if (t.locked) continue;
                        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [](const Clip& c) { return c.kind == "title" && c.caption; }), t.clips.end());
                    }
                }
                std::string track = stringParam(p, "track");
                if (track.empty()) track = titleTrack(tl, true);
                const int n = addCaptions(tl, cues, track, numberParam(p, "offset", 0), p);
                Result r = Result::success("imported " + std::to_string(n) + " captions from " + path + " onto " + track + " (timeline " + num(tl.duration()) + "s)");
                return r.with("doc", stringParam(p, "doc")).with("captions", static_cast<std::int64_t>(n)).with("track", track);
            }));

    reg.add({"cut.export_captions", "Export Captions",
             "Write the timeline's captions (caption clips, in time order) as a SubRip .srt or WebVTT .vtt file to "
             "upload next to the video.",
             {docParam(), {"output", ParamType::String, ".srt or .vtt file to write", true}, overwriteParam()}, false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const std::filesystem::path out = stringParam(p, "output");
                const std::string ext = extOf(out);
                if (ext != "srt" && ext != "vtt") return Result::failure("captions export as .srt or .vtt");
                std::vector<Cue> cues;
                for (const auto& t : tl.tracks) {
                    if (!tl.shows(t)) continue;
                    for (const auto& c : t.clips) {
                        if (c.kind == "title" && c.caption) cues.push_back({c.start, c.end(), c.text});
                    }
                }
                if (cues.empty()) return Result::failure("there are no captions (cut.add_caption or cut.import_captions add them)");
                std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
                if (std::string err = checkOutput(out, p); !err.empty()) return Result::failure(err);
                if (!writeFile(out, ext == "srt" ? writeSrt(cues) : writeVtt(cues))) return Result::failure("can't write " + out.string());
                Result r = Result::success("wrote " + std::to_string(cues.size()) + " captions to " + out.string(), out.string());
                return r.with("output", out.string()).with("captions", static_cast<std::int64_t>(cues.size()));
            }, false));
}

}  // namespace wetwaresuite::cut
