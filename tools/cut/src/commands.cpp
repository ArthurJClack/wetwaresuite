// Every cut.* command. Timeline edits run inside a TimelineEdit, so each is
// one undo step and a failed edit leaves the timeline as it was. Rendering and
// the one-shot file helpers run ffmpeg and stay out of the history.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "wetwaresuite/cut/cut.hpp"
#include "media.hpp"

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

constexpr double kEps = 1e-6;

std::string num(double v) {
    if (std::abs(v) < 5e-7) return "0";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const fs::path& p) {
    std::string e = p.extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open timeline, e.g. cut1", true}; }
ParamSpec clipParam(const std::string& what = "clip id, e.g. c2 (cut.info lists them)") {
    return {"clip", ParamType::String, what, true};
}
ParamSpec previewParam() {
    return {"preview", ParamType::Bool, "attach a PNG of the frame in the middle of the clip (needs ffmpeg)", false, {}, false};
}
ParamSpec overwriteParam() { return {"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, false}; }

const std::vector<std::string> kFits = {"fit", "fill", "stretch"};
const std::vector<std::string> kPositions = {"center", "top", "bottom", "lower-third", "top-left", "top-right",
                                             "bottom-left", "bottom-right", "custom"};

// The properties a clip can take, as params. Which ones a command offers is picked by name.
std::vector<ParamSpec> propSpecs(const std::vector<std::string>& names) {
    static const std::vector<ParamSpec> all = {
        {"start", ParamType::Number, "where the clip begins on the timeline, in seconds", false},
        {"in", ParamType::Number, "source time the clip starts from, in seconds", false},
        {"out", ParamType::Number, "source time the clip stops at, in seconds", false},
        {"duration", ParamType::Number, "length on the timeline in seconds (sets out = in + duration x speed)", false},
        {"speed", ParamType::Number, "playback speed: 2 = twice as fast, 0.5 = slow motion (0.05..20)", false},
        {"volume", ParamType::Number, "sound level: 1 = unchanged, 0.3 = quiet, 2 = louder", false},
        {"mute", ParamType::Bool, "silence this clip's sound", false},
        {"fade_in", ParamType::Number, "fade in over this many seconds (picture from transparent, sound from silence)", false},
        {"fade_out", ParamType::Number, "fade out over this many seconds", false},
        {"opacity", ParamType::Number, "0 (invisible) .. 1 (solid)", false},
        {"x", ParamType::Number, "left edge of the picture box in px (picture-in-picture; titles with position=custom)", false},
        {"y", ParamType::Number, "top edge of the picture box in px", false},
        {"width", ParamType::Number, "picture box width in px; 0 = the whole frame (set it for picture-in-picture)", false},
        {"height", ParamType::Number, "picture box height in px; 0 = from width and the source's aspect", false},
        {"fit", ParamType::String, "how the picture fills its box", false, kFits},
        {"crop", ParamType::NumberList, "crop the source first: x,y,width,height in source px (0,0,0,0 = no crop)", false},
        {"brightness", ParamType::Number, "-1 .. 1, 0 = unchanged", false},
        {"contrast", ParamType::Number, "0 .. 3, 1 = unchanged", false},
        {"saturation", ParamType::Number, "0 (grey) .. 3, 1 = unchanged", false},
        {"gamma", ParamType::Number, "0.1 .. 10, 1 = unchanged", false},
        {"grayscale", ParamType::Bool, "black and white", false},
        {"lut", ParamType::String, "colour grade with a .cube 3D LUT file (empty to remove)", false},
        {"text", ParamType::String, "title text", false},
        {"font", ParamType::String, "title font: a .ttf/.otf file, or a family name if ffmpeg has fontconfig", false},
        {"font_size", ParamType::Number, "title size in px", false},
        {"color", ParamType::String, "title colour: a name (white, yellow), #rrggbb, or with alpha like white@0.8", false},
        {"position", ParamType::String, "where the title sits (custom uses x and y)", false, kPositions},
        {"box", ParamType::Bool, "draw a box behind the title", false},
        {"box_color", ParamType::String, "colour of the title box, e.g. black@0.5", false},
    };
    std::vector<ParamSpec> out;
    for (const auto& n : names) {
        for (const auto& s : all) {
            if (s.name == n) out.push_back(s);
        }
    }
    return out;
}

const std::vector<std::string> kPictureProps = {"opacity", "x", "y", "width", "height", "fit", "crop", "fade_in", "fade_out"};

std::vector<std::string> allPropNames() {
    std::vector<std::string> names;
    for (const auto& s : propSpecs({"start", "in", "out", "duration", "speed", "volume", "mute", "fade_in", "fade_out", "opacity",
                                    "x", "y", "width", "height", "fit", "crop", "brightness", "contrast", "saturation", "gamma",
                                    "grayscale", "lut", "text", "font", "font_size", "color", "position", "box", "box_color"})) {
        names.push_back(s.name);
    }
    return names;
}

// Applies whichever properties are present. Returns an error, empty when fine.
std::string applyProps(Clip& c, const Params& p) {
    auto n = [&](const char* key, double& field) {
        if (has(p, key)) field = numberParam(p, key);
    };
    auto b = [&](const char* key, bool& field) {
        if (has(p, key)) field = boolParam(p, key);
    };
    auto s = [&](const char* key, std::string& field) {
        if (has(p, key)) field = stringParam(p, key);
    };
    const bool timed = c.kind == "video" || c.kind == "audio";
    if (has(p, "speed")) {
        if (!timed) return "only video and audio clips have a speed";
        c.speed = numberParam(p, "speed");
    }
    n("start", c.start);
    if (has(p, "in")) {
        if (!timed) return "stills and titles have no in point; set duration instead";
        c.in = numberParam(p, "in");
    }
    if (has(p, "out")) {
        if (!timed) return "stills and titles have no out point; set duration instead";
        c.out = numberParam(p, "out");
    }
    if (has(p, "duration")) {
        const double d = numberParam(p, "duration");
        if (d <= 0) return "duration must be more than 0";
        c.out = c.in + d * (timed ? c.speed : 1);
    }
    n("volume", c.volume);
    b("mute", c.mute);
    n("fade_in", c.fadeIn);
    n("fade_out", c.fadeOut);
    n("opacity", c.opacity);
    n("x", c.x);
    n("y", c.y);
    n("width", c.width);
    n("height", c.height);
    s("fit", c.fit);
    if (has(p, "crop")) {
        NumberList l = listParam(p, "crop");
        if (l.size() != 4) return "crop takes four numbers: x,y,width,height";
        c.cropX = l[0];
        c.cropY = l[1];
        c.cropW = l[2];
        c.cropH = l[3];
    }
    n("brightness", c.brightness);
    n("contrast", c.contrast);
    n("saturation", c.saturation);
    n("gamma", c.gamma);
    b("grayscale", c.grayscale);
    s("lut", c.lut);
    if (c.kind == "title") {
        s("text", c.text);
        s("font", c.font);
        n("font_size", c.fontSize);
        s("color", c.color);
        s("position", c.position);
        b("box", c.box);
        s("box_color", c.boxColor);
        if ((has(p, "x") || has(p, "y")) && !has(p, "position")) c.position = "custom";
    } else {
        for (const char* key : {"text", "font", "font_size", "color", "position", "box", "box_color"}) {
            if (has(p, key)) return std::string(key) + " only applies to titles";
        }
    }
    if (c.kind == "audio") {
        for (const auto& key : {"opacity", "x", "y", "width", "height", "fit", "crop", "brightness", "contrast", "saturation",
                                "gamma", "grayscale", "lut"}) {
            if (has(p, key)) return std::string(key) + " only applies to pictures, not audio clips";
        }
    }
    return {};
}

std::string validate(const Clip& c) {
    if (!(c.speed >= 0.05 && c.speed <= 20)) return "speed must be between 0.05 and 20";
    if (c.start < -kEps) return "start can't be before 0";
    if (c.in < -kEps) return "in can't be before 0";
    if (c.out <= c.in + 1e-3) return "the clip must be longer than that (out must be after in)";
    if (c.sourceDuration > 0 && (c.kind == "video" || c.kind == "audio") && c.out > c.sourceDuration + 0.05) {
        return "out (" + num(c.out) + "s) is past the end of the source, which is " + num(c.sourceDuration) + "s long";
    }
    if (c.volume < 0 || c.volume > 10) return "volume must be between 0 and 10";
    if (c.opacity < 0 || c.opacity > 1) return "opacity must be between 0 and 1";
    if (c.fadeIn < 0 || c.fadeOut < 0) return "fades can't be negative";
    if (c.fadeIn + c.fadeOut > c.length() + 1e-3) return "fade_in + fade_out is longer than the clip (" + num(c.length()) + "s)";
    if (c.width < 0 || c.height < 0 || c.width > 16384 || c.height > 16384) return "width and height must be 0..16384";
    if (c.cropW < 0 || c.cropH < 0 || c.cropX < 0 || c.cropY < 0) return "crop values can't be negative";
    if (c.cropW > 0 && c.sourceWidth > 0 && (c.cropX + c.cropW > c.sourceWidth + 0.5 || c.cropY + c.cropH > c.sourceHeight + 0.5)) {
        return "the crop goes outside the " + std::to_string(c.sourceWidth) + "x" + std::to_string(c.sourceHeight) + " source";
    }
    if ((c.cropW > 0) != (c.cropH > 0)) return "crop needs both a width and a height";
    if (c.brightness < -1 || c.brightness > 1) return "brightness must be -1..1";
    if (c.contrast < 0 || c.contrast > 3) return "contrast must be 0..3";
    if (c.saturation < 0 || c.saturation > 3) return "saturation must be 0..3";
    if (c.gamma < 0.1 || c.gamma > 10) return "gamma must be 0.1..10";
    std::error_code ec;
    if (!c.lut.empty() && !fs::is_regular_file(c.lut, ec)) return "LUT file not found: " + c.lut;
    if (c.kind == "title" && c.text.empty()) return "a title needs text";
    if (c.kind == "title" && (c.fontSize < 4 || c.fontSize > 1000)) return "font_size must be 4..1000";
    return {};
}

std::string describe(const Timeline& tl, const Track& track, const Clip& c) {
    const Clip d;
    std::string s = c.id + " " + c.kind;
    if (c.kind == "title") s += " \"" + (c.text.size() > 40 ? c.text.substr(0, 37) + "..." : c.text) + "\"";
    else s += " " + fs::path(c.source).filename().string();
    s += " at " + num(c.start) + "-" + num(c.end()) + "s (" + num(c.length()) + "s)";
    if (c.kind == "video" || c.kind == "audio") s += " source " + num(c.in) + "-" + num(c.out);
    auto add = [&](const std::string& key, double v, double def) {
        if (std::abs(v - def) > kEps) s += " " + key + "=" + num(v);
    };
    add("speed", c.speed, 1);
    add("volume", c.volume, 1);
    if (c.mute) s += " muted";
    add("fade_in", c.fadeIn, 0);
    add("fade_out", c.fadeOut, 0);
    add("opacity", c.opacity, 1);
    if (c.kind != "title" && (c.width > 0 || c.height > 0)) {
        s += " box=" + num(c.x) + "," + num(c.y) + " " + num(c.width) + "x" + num(c.height);
    }
    if (c.fit != d.fit) s += " fit=" + c.fit;
    if (c.cropW > 0) s += " crop=" + num(c.cropX) + "," + num(c.cropY) + "," + num(c.cropW) + "," + num(c.cropH);
    add("brightness", c.brightness, 0);
    add("contrast", c.contrast, 1);
    add("saturation", c.saturation, 1);
    add("gamma", c.gamma, 1);
    if (c.grayscale) s += " grayscale";
    if (!c.lut.empty()) s += " lut=" + fs::path(c.lut).filename().string();
    if (c.kind == "title") {
        s += " size=" + num(c.fontSize) + " color=" + c.color + " position=" + c.position;
        if (c.position == "custom") s += " (" + num(c.x) + "," + num(c.y) + ")";
        if (c.box) s += " box=" + c.boxColor;
    }
    if (!c.transition.empty()) {
        const Clip* prev = tl.previousMedia(track, c);
        const double overlap = prev ? prev->end() - c.start : 0;
        if (prev && overlap > 1e-3 && overlap <= std::min(prev->length(), c.length()) + kEps) {
            s += " transition=" + c.transition + " " + num(overlap) + "s from " + prev->id;
        } else {
            s += " transition=" + c.transition + " (inactive: doesn't overlap the clip before)";
        }
    }
    if (c.kind == "video" && !c.hasAudio) s += " (no sound)";
    return s;
}

// Renders one frame at `at`; returns the PNG bytes or throws.
std::string stillPng(const Timeline& tl, double at, int width) {
    if (!ffmpegPath()) throw std::runtime_error(missingFfmpeg());
    TempDir tmp;
    RenderRequest rq;
    rq.output = (tmp.path() / "frame.png").string();
    rq.at = at;
    rq.stillWidth = width;
    rq.fontFile = findFont();
    RenderPlan plan = planRender(tl, rq);
    std::string err = runFfmpeg(plan.args);
    if (!err.empty()) throw std::runtime_error(err);
    std::string bytes = readFile(rq.output);
    if (bytes.empty()) throw std::runtime_error("ffmpeg made no frame at " + num(at) + "s");
    return bytes;
}

Result& maybePreview(Result& r, const Timeline& tl, const Params& p, double at) {
    if (!r.ok || !boolParam(p, "preview", false)) return r;
    try {
        r.attach({"image/png", stillPng(tl, at, std::min(tl.settings.width, 640)), "preview.png"});
    } catch (const std::exception& e) {
        r.message += " (no preview: " + std::string(e.what()) + ")";
    }
    return r;
}

using Edit = std::function<Result(Timeline&, const Params&)>;

ActionFactory cutCommand(Session& session, Edit edit, bool undoable = true) {
    return [&session, edit, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<Timeline>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>(
                [id] { return Result::failure("no open timeline called '" + id + "' (use cut.new or cut.open first)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, edit, p] {
                try {
                    return edit(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<TimelineEdit>(doc, [edit, p](Timeline& d) { return edit(d, p); });
    };
}

// The reply to an edit of one clip: its id, place and the timeline's length, plus a preview if asked.
Result clipResult(const std::string& message, Timeline& tl, const Params& p, const std::string& clipId) {
    tl.sortClips();
    const Clip* c = tl.findClip(clipId);
    Result r = Result::success(message + " (timeline " + num(tl.duration()) + "s)", clipId);
    r.with("clip", clipId);
    if (c) r.with("start", c->start).with("end", c->end()).with("length", c->length());
    r.with("duration", tl.duration());
    return maybePreview(r, tl, p, c ? c->start + c->length() / 2 : 0);
}

Clip* requireClip(Timeline& tl, const Params& p, Track** owner = nullptr) {
    const std::string id = stringParam(p, "clip");
    Clip* c = tl.findClip(id, owner);
    if (!c) throw std::runtime_error("no clip called '" + id + "' (cut.info lists the clips)");
    return c;
}

void fillFromProbe(Clip& c, const MediaInfo& m) {
    c.sourceDuration = m.duration;
    c.sourceWidth = m.width;
    c.sourceHeight = m.height;
    c.sourceFps = m.fps;
    c.hasAudio = m.hasAudio;
    c.videoCodec = m.videoCodec;
    c.audioCodec = m.audioCodec;
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    fs::path a = fs::absolute(path, ec);
    return ec ? path : a.lexically_normal().string();
}

// Adds a clip to a track, checking it and giving it an id.
Result addClip(Timeline& tl, const Params& p, Clip c, const std::string& defaultTrack, const std::string& what) {
    const std::string trackId = stringParam(p, "track", defaultTrack);
    Track& track = tl.ensureTrack(trackId);
    if ((track.kind == "audio") != (c.kind == "audio")) {
        throw std::runtime_error(c.kind == "audio" ? "audio goes on an audio track (a1, a2, ...)"
                                                   : "pictures go on a video track (v1, v2, ...)");
    }
    c.start = tl.trackEnd(trackId);
    std::string err = applyProps(c, p);
    if (err.empty()) err = validate(c);
    if (!err.empty()) throw std::runtime_error(err);
    c.id = tl.newClipId();
    track.clips.push_back(c);
    return clipResult("added " + what + " " + c.id + " on " + trackId + " at " + num(c.start) + "s", tl, p, c.id);
}

std::string checkOutput(const fs::path& output, const Params& p) {
    std::error_code ec;
    if (fs::exists(output, ec) && !boolParam(p, "overwrite", false)) {
        return output.string() + " already exists (pass overwrite=true to replace it)";
    }
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), ec);
    return {};
}

Result written(const std::string& message, const fs::path& output) {
    std::error_code ec;
    auto bytes = static_cast<std::int64_t>(fs::file_size(output, ec));
    Result r = Result::success(message, output.string());
    r.with("output", output.string()).with("bytes", ec ? std::int64_t{0} : bytes);
    try {
        MediaInfo m = probe(output);
        if (m.duration > 0 && !m.still) r.with("duration", m.duration);
        if (m.width > 0) r.with("width", static_cast<std::int64_t>(m.width)).with("height", static_cast<std::int64_t>(m.height));
    } catch (const std::exception&) {
    }
    return r;
}

// Renders a timeline to a file: the work behind cut.render and most one-shot helpers.
Result renderTimeline(const Timeline& tl, const fs::path& output, const Params& p, RenderRequest rq) {
    if (!ffmpegPath()) return Result::failure(missingFfmpeg());
    std::error_code ec;
    for (const auto& t : tl.tracks) {
        for (const auto& c : t.clips) {
            if (!c.source.empty() && !fs::is_regular_file(c.source, ec)) {
                return Result::failure("clip " + c.id + "'s source is missing: " + c.source);
            }
        }
    }
    if (std::string err = checkOutput(output, p); !err.empty()) return Result::failure(err);
    TempDir tmp;
    rq.output = fs::absolute(output, ec).string();
    rq.listPath = (tmp.path() / "list.txt").string();
    if (rq.fontFile.empty()) rq.fontFile = findFont();
    RenderPlan plan;
    try {
        plan = planRender(tl, rq);
    } catch (const std::exception& e) {
        return Result::failure(e.what());
    }
    if (plan.streamCopy && !writeFile(rq.listPath, plan.concatList)) return Result::failure("can't write a temporary file");
    std::string err = runFfmpeg(plan.args);
    if (!err.empty()) return Result::failure(err);
    Result r = written("rendered " + output.string() + (plan.streamCopy ? " by stream copy (no re-encoding)" : ""), output);
    const std::string ext = extOf(output);
    if (ext != "png" && ext != "jpg" && ext != "jpeg") r.with("mode", std::string(plan.streamCopy ? "copy" : "encode"));
    return r;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    const char sep = text.find('\n') != std::string::npos ? '\n' : (text.find('|') != std::string::npos ? '|' : ',');
    std::string cur;
    auto flush = [&] {
        auto a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (char ch : text) {
        if (ch == sep) flush();
        else cur += ch;
    }
    flush();
    return out;
}

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

std::string requireInput(const Params& p, const char* key = "input") {
    std::string in = stringParam(p, key);
    std::error_code ec;
    if (!fs::is_regular_file(in, ec)) throw std::runtime_error("file not found: " + in);
    return in;
}

class CloseAction : public Action {
public:
    CloseAction(Session& session, std::string id) : session_(session), id_(std::move(id)) {}
    Result apply() override {
        doc_ = session_.workspace().take(id_);
        if (!doc_) return Result::failure("no open document called '" + id_ + "'");
        return Result::success("closed " + id_);
    }
    void revert() override {
        if (doc_) session_.workspace().put(id_, doc_);
    }

private:
    Session& session_;
    std::string id_;
    std::shared_ptr<Document> doc_;
};

Result docCreated(Session& session, std::shared_ptr<Timeline> doc, const std::string& verb) {
    std::string id = session.workspace().add(doc, "cut");
    Result r = Result::success(verb + " " + id + " (" + doc->summary() + ")", id);
    r.with("doc", id)
        .with("width", static_cast<std::int64_t>(doc->settings.width))
        .with("height", static_cast<std::int64_t>(doc->settings.height))
        .with("fps", doc->settings.fps)
        .with("duration", doc->duration());
    return r;
}

std::string settingsError(const Settings& s) {
    if (s.width < 16 || s.height < 16 || s.width > 8192 || s.height > 8192) return "width and height must be 16..8192";
    if (s.width % 2 || s.height % 2) return "width and height must be even (video encoders need it)";
    if (!(s.fps >= 1 && s.fps <= 240)) return "fps must be 1..240";
    if (s.background.empty()) return "background needs a colour";
    return {};
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

void registerCommands(Session& session) {
    auto& reg = session.registry();

    // ---- documents ----
    reg.add({"cut.new", "New Timeline",
             "Create an empty video timeline and return its id (e.g. cut1). Add clips with cut.add_clip, stills with "
             "cut.add_image, titles with cut.add_title and music with cut.add_audio, then cut.render. Video track v1 is "
             "the bottom layer; v2 and up sit on top (picture-in-picture, titles).",
             {{"width", ParamType::Int, "frame width in px (even)", false, {}, std::int64_t{1920}},
              {"height", ParamType::Int, "frame height in px (even); 1920x1080 landscape, 1080x1920 for shorts", false, {}, std::int64_t{1080}},
              {"fps", ParamType::Number, "frames per second", false, {}, 30.0},
              {"background", ParamType::String, "colour behind everything: a name, #rrggbb", false, {}, std::string("black")}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto tl = std::make_shared<Timeline>();
                    tl->settings.width = static_cast<int>(intParam(p, "width", 1920));
                    tl->settings.height = static_cast<int>(intParam(p, "height", 1080));
                    tl->settings.fps = numberParam(p, "fps", 30);
                    tl->settings.background = stringParam(p, "background", "black");
                    if (std::string err = settingsError(tl->settings); !err.empty()) return Result::failure(err);
                    return docCreated(session, tl, "created");
                });
            });

    reg.add({"cut.open", "Open Timeline", "Open a .wwcut timeline saved by cut.save (older .achcut files open too). Returns the document id.",
             {{"path", ParamType::String, "path to a .wwcut file", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const fs::path path = stringParam(p, "path");
                    std::error_code ec;
                    if (!fs::is_regular_file(path, ec)) return Result::failure("file not found: " + path.string());
                    const std::string ext = extOf(path);
                    if (ext != "wwcut" && ext != "achcut") {  // achcut: the name before the rename
                        return Result::failure(path.string() + " isn't a .wwcut timeline; to edit a video file, "
                                                               "cut.new then cut.add_clip path=" + path.string());
                    }
                    try {
                        auto tl = fromJson(readFile(path), fs::absolute(path, ec).parent_path());
                        tl->path = path.string();
                        return docCreated(session, tl, "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"cut.save", "Save Timeline",
             "Save a timeline as a .wwcut project file (JSON: settings, tracks, clips). It refers to the media files "
             "by absolute path; it doesn't copy them. To make a video file, use cut.render.",
             {docParam(), {"path", ParamType::String, "where to save, e.g. edit.wwcut", true}}, false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const fs::path path = stringParam(p, "path");
                const std::string ext = extOf(path);
                if (ext != "wwcut" && ext != "achcut") return Result::failure("timelines save as .wwcut; to export a video, use cut.render");
                std::error_code ec;
                if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
                if (!writeFile(path, toJson(tl))) return Result::failure("can't write " + path.string());
                tl.path = path.string();
                Result r = Result::success("saved " + path.string(), path.string());
                return r.with("path", path.string());
            }, false));

    reg.add({"cut.close", "Close Timeline", "Close a timeline (undo reopens it).", {docParam()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                const std::string id = stringParam(p, "doc");
                if (!session.workspace().getAs<Timeline>(id)) {
                    return std::make_unique<FunctionAction>([id] { return Result::failure("no open timeline called '" + id + "'"); });
                }
                return std::make_unique<CloseAction>(session, id);
            });

    reg.add({"cut.info", "Timeline Info",
             "Describe a timeline: size, fps, length, and every track with its clips in order (id, kind, source, "
             "place on the timeline, source in/out and every changed property, transitions).",
             {docParam()}, false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                char fps[32];
                std::snprintf(fps, sizeof fps, "%g", tl.settings.fps);
                std::string text = stringParam(p, "doc") + ": " + std::to_string(tl.settings.width) + "x" +
                                   std::to_string(tl.settings.height) + " " + fps + "fps, background " +
                                   tl.settings.background + ", " + num(tl.duration()) + "s";
                std::int64_t clips = 0;
                if (tl.tracks.empty()) text += "\nno tracks yet (add clips with cut.add_clip)";
                for (const auto& t : tl.tracks) {
                    text += "\n" + t.id + " (" + t.kind + (t.muted ? ", muted" : "") + (t.hidden ? ", hidden" : "") + ")";
                    if (t.clips.empty()) text += ": empty";
                    for (const auto& c : t.clips) {
                        text += "\n  " + describe(tl, t, c);
                        ++clips;
                    }
                }
                Result r = Result::success(text);
                return r.with("doc", stringParam(p, "doc"))
                    .with("width", static_cast<std::int64_t>(tl.settings.width))
                    .with("height", static_cast<std::int64_t>(tl.settings.height))
                    .with("fps", tl.settings.fps)
                    .with("duration", tl.duration())
                    .with("tracks", static_cast<std::int64_t>(tl.tracks.size()))
                    .with("clips", clips);
            }, false));

    reg.add({"cut.settings", "Timeline Settings",
             "Change a timeline's frame size, frame rate or background colour. Clips keep their places; full-frame "
             "clips refit to the new size.",
             {docParam(),
              {"width", ParamType::Int, "frame width in px (even)", false},
              {"height", ParamType::Int, "frame height in px (even)", false},
              {"fps", ParamType::Number, "frames per second", false},
              {"background", ParamType::String, "colour behind everything", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                if (has(p, "width")) tl.settings.width = static_cast<int>(intParam(p, "width"));
                if (has(p, "height")) tl.settings.height = static_cast<int>(intParam(p, "height"));
                if (has(p, "fps")) tl.settings.fps = numberParam(p, "fps");
                if (has(p, "background")) tl.settings.background = stringParam(p, "background");
                if (std::string err = settingsError(tl.settings); !err.empty()) return Result::failure(err);
                Result r = Result::success("timeline is now " + tl.summary());
                return r.with("doc", stringParam(p, "doc"));
            }));

    reg.add({"cut.set_track", "Set Track",
             "Mute an audio or video track's sound, or hide a video track's picture, for every clip on it.",
             {docParam(),
              {"track", ParamType::String, "track id: v1, v2, a1, ...", true},
              {"muted", ParamType::Bool, "silence the track", false},
              {"hidden", ParamType::Bool, "don't show the track's picture", false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Track* t = tl.track(stringParam(p, "track"));
                if (!t) return Result::failure("no track called '" + stringParam(p, "track") + "'");
                if (has(p, "muted")) t->muted = boolParam(p, "muted");
                if (has(p, "hidden")) {
                    if (t->kind != "video") return Result::failure("only video tracks can be hidden");
                    t->hidden = boolParam(p, "hidden");
                }
                Result r = Result::success(t->id + (t->muted ? " muted" : " has sound") + (t->hidden ? ", hidden" : ""));
                return r.with("doc", stringParam(p, "doc")).with("track", t->id);
            }));

    reg.add({"cut.probe", "Probe Media",
             "Ask ffprobe about a media file: duration, container, and every stream with its codec, size, frame rate, "
             "sample rate and channels.",
             {{"path", ParamType::String, "video, audio or image file", true}}, false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    try {
                        const std::string path = stringParam(p, "path");
                        MediaInfo m = probe(path);
                        std::string text = path + ": " + m.format + ", " + num(m.duration) + "s";
                        if (m.still) text += " (still image)";
                        int i = 0;
                        for (const auto& s : m.streams) {
                            text += "\n  stream " + std::to_string(i++) + ": " + s.type + " " + s.codec;
                            if (s.type == "video") text += " " + std::to_string(s.width) + "x" + std::to_string(s.height) + " " + num(s.fps) + "fps";
                            if (s.type == "audio") text += " " + std::to_string(s.sampleRate) + "Hz " + std::to_string(s.channels) + "ch";
                        }
                        Result r = Result::success(text);
                        r.with("path", path).with("duration", m.duration).with("format", m.format).with("has_video", m.hasVideo)
                            .with("has_audio", m.hasAudio).with("still", m.still);
                        if (m.hasVideo) {
                            r.with("width", static_cast<std::int64_t>(m.width)).with("height", static_cast<std::int64_t>(m.height))
                                .with("fps", m.fps).with("video_codec", m.videoCodec);
                        }
                        if (m.hasAudio) r.with("audio_codec", m.audioCodec);
                        r.with("streams", static_cast<std::int64_t>(m.streams.size()));
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    // ---- adding ----
    {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "video file", true},
                                 {"track", ParamType::String, "video track: v1 (bottom, default), v2, ...", false, {}, std::string("v1")}};
        for (auto& x : propSpecs({"start", "in", "out", "duration", "speed", "volume", "mute"})) s.push_back(x);
        for (auto& x : propSpecs(kPictureProps)) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"cut.add_clip", "Add Clip",
                 "Put a video file on the timeline. By default it goes at the end of track v1 and plays the whole "
                 "file; in/out pick a part of the source, start places it. Its sound comes along (volume, mute). "
                 "For picture-in-picture put it on v2 with width (and x, y). Returns the clip id, e.g. c1.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    const std::string path = stringParam(p, "path");
                    MediaInfo m = probe(path);
                    if (!m.hasVideo) throw std::runtime_error(path + " has no picture; use cut.add_audio for sound files");
                    Clip c;
                    c.kind = m.still ? "image" : "video";
                    c.source = absolute(path);
                    fillFromProbe(c, m);
                    if (m.still) {
                        c.out = 5;
                        return addClip(tl, p, c, "v1", "still");
                    }
                    if (!(m.duration > 0)) throw std::runtime_error("ffprobe couldn't tell how long " + path + " is");
                    c.out = m.duration;
                    return addClip(tl, p, c, "v1", "clip");
                }));
    }
    {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "image file (png, jpg, webp, ...)", true},
                                 {"track", ParamType::String, "video track", false, {}, std::string("v1")},
                                 {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 5.0}};
        for (auto& x : propSpecs({"start"})) s.push_back(x);
        for (auto& x : propSpecs(kPictureProps)) s.push_back(x);
        for (auto& x : propSpecs({"brightness", "contrast", "saturation", "grayscale"})) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"cut.add_image", "Add Image",
                 "Put a still image on the timeline for a while (a photo, a slide, a logo; PNG transparency is kept). "
                 "Same placing and picture options as cut.add_clip.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    const std::string path = stringParam(p, "path");
                    MediaInfo m = probe(path);
                    if (!m.hasVideo) throw std::runtime_error(path + " isn't an image");
                    Clip c;
                    c.kind = "image";
                    c.source = absolute(path);
                    fillFromProbe(c, m);
                    c.hasAudio = false;
                    c.out = 5;
                    Params q = p;
                    if (!has(q, "duration")) q["duration"] = 5.0;
                    return addClip(tl, q, c, "v1", "image");
                }));
    }
    {
        std::vector<ParamSpec> s{docParam(), {"text", ParamType::String, "what the title says", true},
                                 {"track", ParamType::String, "video track (default: a titles track on top of everything)", false},
                                 {"start", ParamType::Number, "when it appears, in seconds", false, {}, 0.0},
                                 {"duration", ParamType::Number, "how long it shows, in seconds", false, {}, 3.0}};
        for (auto& x : propSpecs({"position", "font_size", "color", "font", "box", "box_color", "x", "y", "fade_in", "fade_out", "opacity"})) {
            s.push_back(x);
        }
        s.back().description = "0..1 for see-through text";
        s.push_back(previewParam());
        reg.add({"cut.add_title", "Add Title",
                 "Draw text over the video for a while: a heading, a caption, a lower third. position places it "
                 "(center, top, bottom, lower-third, corners, or custom with x,y); fades make it ease in and out.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip c;
                    c.kind = "title";
                    c.out = 3;
                    std::string track = stringParam(p, "track");
                    if (track.empty()) {
                        // the topmost video track holding only titles, or a new one above everything
                        int highest = 0;
                        for (const auto& t : tl.tracks) {
                            if (t.kind != "video") continue;
                            highest = std::max(highest, std::atoi(t.id.c_str() + 1));
                            const bool titlesOnly = std::all_of(t.clips.begin(), t.clips.end(), [](const Clip& x) { return x.kind == "title"; });
                            if (titlesOnly && !t.clips.empty()) track = t.id;
                        }
                        if (track.empty() || std::atoi(track.c_str() + 1) != highest) track = "v" + std::to_string(highest + 1);
                    }
                    Params q = p;
                    q["track"] = track;
                    if (!has(q, "start")) q["start"] = 0.0;
                    if (!has(q, "duration")) q["duration"] = 3.0;
                    return addClip(tl, q, c, track, "title");
                }));
    }
    {
        std::vector<ParamSpec> s{docParam(), {"path", ParamType::String, "audio file (or a video, for its sound)", true},
                                 {"track", ParamType::String, "audio track: a1 (default), a2, ...", false, {}, std::string("a1")}};
        for (auto& x : propSpecs({"start", "in", "out", "duration", "volume", "fade_in", "fade_out", "speed"})) s.push_back(x);
        reg.add({"cut.add_audio", "Add Audio",
                 "Put music, a voice-over or sound effects on an audio track. By default it starts at the end of the "
                 "track (0 for the first); in/out and duration trim it, volume sets the level (0.3 for background "
                 "music), fade_in/fade_out ease it.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    const std::string path = stringParam(p, "path");
                    MediaInfo m = probe(path);
                    if (!m.hasAudio) throw std::runtime_error(path + " has no sound");
                    Clip c;
                    c.kind = "audio";
                    c.source = absolute(path);
                    fillFromProbe(c, m);
                    c.hasAudio = true;
                    c.out = m.duration;
                    if (!(m.duration > 0)) throw std::runtime_error("ffprobe couldn't tell how long " + path + " is");
                    return addClip(tl, p, c, "a1", "audio");
                }));
    }

    // ---- editing ----
    reg.add({"cut.move_clip", "Move Clip", "Move a clip to a new start time and/or another track of the same kind.",
             {docParam(), clipParam(), {"start", ParamType::Number, "new start on the timeline, in seconds", false},
              {"track", ParamType::String, "move to this track (v<n> for pictures, a<n> for audio)", false}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Track* owner = nullptr;
                Clip* c = requireClip(tl, p, &owner);
                if (!has(p, "start") && !has(p, "track")) return Result::failure("give a new start and/or track");
                Clip moved = *c;
                if (has(p, "start")) moved.start = numberParam(p, "start");
                if (moved.start < 0) return Result::failure("start can't be before 0");
                std::string where = owner->id;
                if (has(p, "track") && stringParam(p, "track") != owner->id) {
                    const std::string from = owner->id;
                    Track& dest = tl.ensureTrack(stringParam(p, "track"));
                    owner = tl.track(from);  // ensureTrack may have reordered the tracks
                    if ((dest.kind == "audio") != (moved.kind == "audio")) return Result::failure("a " + moved.kind + " clip can't go on " + dest.id);
                    owner->clips.erase(std::find_if(owner->clips.begin(), owner->clips.end(), [&](const Clip& x) { return x.id == moved.id; }));
                    dest.clips.push_back(moved);
                    where = dest.id;
                } else {
                    *c = moved;
                }
                return clipResult("moved " + moved.id + " to " + where + " at " + num(moved.start) + "s", tl, p, moved.id);
            }));

    reg.add({"cut.trim_clip", "Trim Clip",
             "Change which part of the source a clip plays: in and out are source times in seconds, or give duration "
             "(timeline seconds). The clip keeps its start; stills and titles take duration only.",
             {docParam(), clipParam(), propSpecs({"in"})[0], propSpecs({"out"})[0], propSpecs({"duration"})[0], previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (!has(p, "in") && !has(p, "out") && !has(p, "duration")) return Result::failure("give in, out or duration");
                Clip copy = *c;
                if (std::string err = applyProps(copy, p); !err.empty()) return Result::failure(err);
                if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                *c = copy;
                return clipResult("trimmed " + copy.id + " to " + num(copy.length()) + "s", tl, p, copy.id);
            }));

    reg.add({"cut.split_clip", "Split Clip",
             "Cut a clip in two at a timeline time. The first part keeps the id; the second gets a new one (returned).",
             {docParam(), clipParam(), {"at", ParamType::Number, "timeline time to cut at, in seconds (inside the clip)", true}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Track* owner = nullptr;
                Clip* c = requireClip(tl, p, &owner);
                const double at = numberParam(p, "at");
                if (at <= c->start + 1e-3 || at >= c->end() - 1e-3) {
                    return Result::failure("at must be inside the clip, between " + num(c->start) + " and " + num(c->end()) + "s");
                }
                Clip second = *c;
                const double rate = c->kind == "video" || c->kind == "audio" ? c->speed : 1;
                c->out = c->in + (at - c->start) * rate;
                c->fadeOut = 0;
                second.id = tl.newClipId();
                second.in = c->out;
                second.start = at;
                second.fadeIn = 0;
                second.transition.clear();
                second.transitionDuration = 0;
                const std::string first = c->id;
                owner->clips.push_back(second);
                Result r = clipResult("split " + first + " at " + num(at) + "s into " + first + " and " + second.id, tl, p, second.id);
                r.with("first", first).with("second", second.id);
                return r;
            }));

    reg.add({"cut.delete_clip", "Delete Clip",
             "Remove a clip. With ripple=true the clips after it on the same track move left to close the gap.",
             {docParam(), clipParam(), {"ripple", ParamType::Bool, "close the gap it leaves", false, {}, false}}, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Track* owner = nullptr;
                Clip* c = requireClip(tl, p, &owner);
                const Clip gone = *c;
                const Clip* prev = gone.media() ? tl.previousMedia(*owner, gone) : nullptr;
                double shift = gone.length();
                if (prev && !gone.transition.empty() && prev->end() > gone.start) shift -= std::min(prev->end() - gone.start, shift);
                owner->clips.erase(std::find_if(owner->clips.begin(), owner->clips.end(), [&](const Clip& x) { return x.id == gone.id; }));
                bool first = true;
                for (auto& x : owner->clips) {
                    if (x.start < gone.start + kEps) continue;
                    if (first && x.media()) {
                        // a transition from the deleted clip has nothing to blend with any more
                        x.transition.clear();
                        x.transitionDuration = 0;
                        first = false;
                    }
                    if (boolParam(p, "ripple", false)) x.start = std::max(0.0, x.start - shift);
                }
                Result r = Result::success("deleted " + gone.id + " (timeline " + num(tl.duration()) + "s)");
                return r.with("doc", stringParam(p, "doc")).with("duration", tl.duration());
            }));

    {
        std::vector<ParamSpec> s{docParam(), clipParam()};
        for (auto& x : propSpecs(allPropNames())) s.push_back(x);
        s.push_back(previewParam());
        reg.add({"cut.set_clip", "Set Clip",
                 "Change any property of a clip: timing (start, in, out, duration, speed), sound (volume, mute), "
                 "fades, opacity, picture box (x, y, width, height, fit, crop), colour (brightness, contrast, "
                 "saturation, gamma, grayscale, lut) and, for titles, text and styling. Only the given ones change.",
                 s, true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip* c = requireClip(tl, p);
                    Clip copy = *c;
                    if (std::string err = applyProps(copy, p); !err.empty()) return Result::failure(err);
                    if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                    *c = copy;
                    return clipResult("updated " + copy.id, tl, p, copy.id);
                }));
    }

    reg.add({"cut.transition", "Transition",
             "Blend a clip with the clip before it on the same track: crossfade, dissolve, fadeblack (fade through "
             "black), fadewhite, wipes, slides, circles and more (ffmpeg xfade). The clip moves so the two overlap by "
             "duration, and later clips on the track move with it; the sound crossfades too. type=none removes it.",
             {docParam(), clipParam("the later of the two clips"),
              {"type", ParamType::String, "transition style", false, transitionNames(), std::string("crossfade")},
              {"duration", ParamType::Number, "seconds of overlap", false, {}, 1.0}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Track* owner = nullptr;
                Clip* c = requireClip(tl, p, &owner);
                if (!c->media()) return Result::failure("transitions go between video or image clips");
                const Clip* prev = tl.previousMedia(*owner, *c);
                if (!prev) return Result::failure(c->id + " is the first clip on " + owner->id + "; there's nothing before it to blend with");
                const std::string type = stringParam(p, "type", "crossfade");
                const double d = type == "none" ? 0 : numberParam(p, "duration", 1.0);
                if (type != "none" && d <= 0.04) return Result::failure("duration must be more than 0.04s");
                if (d > std::min(prev->length(), c->length()) - 1e-3) {
                    return Result::failure("a " + num(d) + "s transition is longer than " + (prev->length() < c->length() ? prev->id : c->id) +
                                           " (" + num(std::min(prev->length(), c->length())) + "s)");
                }
                const double newStart = prev->end() - d, oldStart = c->start, delta = newStart - oldStart;
                const std::string id = c->id, prevId = prev->id;
                for (auto& x : owner->clips) {
                    if (x.id == id) continue;
                    if (x.start > oldStart + kEps) x.start = std::max(0.0, x.start + delta);
                }
                c->start = newStart;
                c->transition = type == "none" ? std::string() : type;
                c->transitionDuration = d;
                if (type == "none") return clipResult("removed the transition into " + id, tl, p, id);
                Result r = Result::success(type + " from " + prevId + " to " + id + " over " + num(d) + "s (" + id + " now starts at " +
                                           num(newStart) + "s)", id);
                r.with("clip", id).with("start", newStart).with("duration", tl.duration());
                return maybePreview(r, tl, p, newStart + d / 2);
            }));

    reg.add({"cut.filter", "Colour Filter",
             "Adjust a clip's colour, or every picture clip's when clip is left out: brightness, contrast, "
             "saturation, gamma, grayscale, or a .cube LUT colour grade. reset=true clears them first.",
             {docParam(), {"clip", ParamType::String, "clip id; leave out for every video and image clip", false},
              propSpecs({"brightness"})[0], propSpecs({"contrast"})[0], propSpecs({"saturation"})[0], propSpecs({"gamma"})[0],
              propSpecs({"grayscale"})[0], propSpecs({"lut"})[0],
              {"reset", ParamType::Bool, "clear existing colour filters first", false, {}, false}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                std::vector<Clip*> targets;
                if (has(p, "clip")) {
                    Clip* c = requireClip(tl, p);
                    if (!c->media()) return Result::failure("colour filters apply to video and image clips");
                    targets.push_back(c);
                } else {
                    for (auto& t : tl.tracks) {
                        for (auto& c : t.clips) {
                            if (c.media()) targets.push_back(&c);
                        }
                    }
                }
                if (targets.empty()) return Result::failure("there are no video or image clips to filter");
                for (Clip* c : targets) {
                    Clip copy = *c;
                    if (boolParam(p, "reset", false)) {
                        const Clip d;
                        copy.brightness = d.brightness;
                        copy.contrast = d.contrast;
                        copy.saturation = d.saturation;
                        copy.gamma = d.gamma;
                        copy.grayscale = d.grayscale;
                        copy.lut.clear();
                    }
                    Params only;
                    for (const char* k : {"brightness", "contrast", "saturation", "gamma", "grayscale", "lut"}) {
                        if (has(p, k)) only[k] = p.at(k);
                    }
                    if (std::string err = applyProps(copy, only); !err.empty()) return Result::failure(err);
                    if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                    *c = copy;
                }
                const std::string first = targets.front()->id;
                Result r = clipResult("filtered " + (targets.size() == 1 ? first : std::to_string(targets.size()) + " clips"), tl, p, first);
                r.with("clips", static_cast<std::int64_t>(targets.size()));
                return r;
            }));

    // ---- seeing and exporting ----
    reg.add({"cut.preview", "Preview Frame",
             "Render the frame at time t (seconds) to a PNG and attach it, so you can see the edit: layers, "
             "picture-in-picture, titles, filters, transitions. Fast: only the clips on screen at t are decoded.",
             {docParam(), {"t", ParamType::Number, "time in seconds", false, {}, 0.0},
              {"width", ParamType::Int, "preview width in px", false, {}, std::int64_t{640}}},
             false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                const double at = numberParam(p, "t", 0);
                if (tl.duration() <= 0) return Result::failure("the timeline is empty");
                if (at < 0 || at >= tl.duration()) return Result::failure("t must be between 0 and " + num(tl.duration()) + "s");
                const int width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 640), 32, tl.settings.width));
                Result r = Result::success("frame at " + num(at) + "s of " + stringParam(p, "doc"));
                r.attach({"image/png", stillPng(tl, at, width), stringParam(p, "doc") + "-" + num(at) + "s.png"});
                return r.with("t", at);
            }, false));

    reg.add({"cut.render", "Render",
             "Export the timeline with ffmpeg: .mp4/.mov/.mkv (H.264 + AAC), .webm (VP9 + Opus), .gif, sound only "
             "(.mp3, .wav, .m4a, .flac, .ogg) or one frame (.png/.jpg at time `at`). The whole edit is one ffmpeg "
             "filter graph. mode=auto stream-copies when the timeline is only plain cuts of identical sources (fast, "
             "lossless, cuts snap to keyframes) and re-encodes otherwise.",
             {docParam(), {"output", ParamType::String, "file to write; the extension picks the format", true},
              {"quality", ParamType::String, "encoding preset: draft (fast, small), good, best (slow)", false, {"draft", "good", "best"}, std::string("good")},
              {"mode", ParamType::String, "auto, copy (fail unless stream copy is possible) or encode", false, {"auto", "copy", "encode"}, std::string("auto")},
              {"at", ParamType::Number, "png/jpg: time of the frame", false, {}, 0.0},
              {"width", ParamType::Int, "png/jpg: image width (default the timeline's)", false},
              {"subtitles", ParamType::String, "burn this .srt/.ass subtitle file into the picture", false},
              {"gif_fps", ParamType::Int, "gif: frames per second", false, {}, std::int64_t{15}},
              {"gif_width", ParamType::Int, "gif: width in px", false, {}, std::int64_t{480}},
              overwriteParam()},
             false},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                RenderRequest rq;
                rq.quality = stringParam(p, "quality", "good");
                rq.mode = stringParam(p, "mode", "auto");
                rq.at = numberParam(p, "at", 0);
                rq.stillWidth = static_cast<int>(intParam(p, "width", 0));
                rq.gifFps = static_cast<int>(intParam(p, "gif_fps", 15));
                rq.gifWidth = static_cast<int>(intParam(p, "gif_width", 480));
                if (has(p, "subtitles")) {
                    rq.subtitles = requireInput(p, "subtitles");
                }
                return renderTimeline(tl, stringParam(p, "output"), p, rq);
            }, false));

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
                RenderRequest rq;
                rq.mode = stringParam(p, "mode", "auto");
                rq.quality = stringParam(p, "quality", "good");
                Result r = renderTimeline(*tl, stringParam(p, "output"), p, rq);
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
                RenderRequest rq;
                rq.mode = "encode";
                rq.quality = stringParam(p, "quality", "good");
                Result r = renderTimeline(*tl, stringParam(p, "output"), p, rq);
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
                if (std::string err = runFfmpeg(a); !err.empty()) return Result::failure(err);
                return written("burned " + subs + " into " + out.string(), out);
            }));
}

}  // namespace wetwaresuite::cut
