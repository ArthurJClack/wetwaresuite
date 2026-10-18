// The timeline document: tracks, clips, .achcut JSON and snapshot undo.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "achclwss/cut/cut.hpp"

namespace achclwss::cut {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool validTrackId(const std::string& id) {
    if (id.size() < 2 || id.size() > 4 || (id[0] != 'v' && id[0] != 'a')) return false;
    for (std::size_t i = 1; i < id.size(); ++i) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return id[1] != '0';
}

int trackNumber(const std::string& id) { return std::atoi(id.c_str() + 1); }

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

}  // namespace

std::string Timeline::summary() const {
    int clips = 0;
    for (const auto& t : tracks) clips += static_cast<int>(t.clips.size());
    char fps[32];
    std::snprintf(fps, sizeof fps, "%g", settings.fps);
    return std::to_string(settings.width) + "x" + std::to_string(settings.height) + " " + fps + "fps, " +
           std::to_string(tracks.size()) + (tracks.size() == 1 ? " track, " : " tracks, ") + std::to_string(clips) +
           (clips == 1 ? " clip, " : " clips, ") + num(duration()) + "s" + (path.empty() ? "" : ", " + path);
}

double Timeline::duration() const {
    double d = 0;
    for (const auto& t : tracks) {
        for (const auto& c : t.clips) d = std::max(d, c.end());
    }
    return d;
}

Track* Timeline::track(const std::string& id) {
    for (auto& t : tracks) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

const Track* Timeline::track(const std::string& id) const {
    for (const auto& t : tracks) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

Track& Timeline::ensureTrack(const std::string& id) {
    if (Track* t = track(id)) return *t;
    if (!validTrackId(id)) throw std::runtime_error("track names are v1, v2, ... for video and a1, a2, ... for audio (got '" + id + "')");
    Track t;
    t.id = id;
    t.kind = id[0] == 'v' ? "video" : "audio";
    tracks.push_back(t);
    // video tracks first (bottom-up by number), then audio tracks
    std::stable_sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) {
        if (a.kind != b.kind) return a.kind == "video";
        return trackNumber(a.id) < trackNumber(b.id);
    });
    return *track(id);
}

Clip* Timeline::findClip(const std::string& id, Track** owner) {
    for (auto& t : tracks) {
        for (auto& c : t.clips) {
            if (c.id == id) {
                if (owner) *owner = &t;
                return &c;
            }
        }
    }
    return nullptr;
}

const Clip* Timeline::findClip(const std::string& id) const {
    for (const auto& t : tracks) {
        for (const auto& c : t.clips) {
            if (c.id == id) return &c;
        }
    }
    return nullptr;
}

void Timeline::sortClips() {
    for (auto& t : tracks) {
        std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    }
}

double Timeline::trackEnd(const std::string& id) const {
    double e = 0;
    if (const Track* t = track(id)) {
        for (const auto& c : t->clips) e = std::max(e, c.end());
    }
    return e;
}

const Clip* Timeline::previousMedia(const Track& t, const Clip& clip) const {
    const Clip* best = nullptr;
    for (const auto& c : t.clips) {
        if (c.id == clip.id || !c.media() || c.start > clip.start + 1e-9) continue;
        if (c.start == clip.start && &c > &clip) continue;
        if (!best || c.start >= best->start) best = &c;
    }
    return best;
}

Result TimelineEdit::apply() {
    before_ = {doc_->settings, doc_->tracks, doc_->nextClip};
    Result r;
    try {
        r = fn_(*doc_);
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (!r.ok) {
        revert();
        return r;
    }
    doc_->sortClips();
    return r;
}

void TimelineEdit::revert() {
    doc_->settings = before_.settings;
    doc_->tracks = before_.tracks;
    doc_->nextClip = before_.nextClip;
}

namespace {

json clipJson(const Clip& c) {
    json j = {{"id", c.id}, {"kind", c.kind}, {"start", c.start}, {"in", c.in}, {"out", c.out}};
    const Clip d;
    auto put = [&](const char* key, auto value, auto fallback) {
        if (value != fallback) j[key] = value;
    };
    put("source", c.source, d.source);
    put("speed", c.speed, d.speed);
    put("volume", c.volume, d.volume);
    put("mute", c.mute, d.mute);
    put("fade_in", c.fadeIn, d.fadeIn);
    put("fade_out", c.fadeOut, d.fadeOut);
    put("opacity", c.opacity, d.opacity);
    put("x", c.x, d.x);
    put("y", c.y, d.y);
    put("width", c.width, d.width);
    put("height", c.height, d.height);
    put("fit", c.fit, d.fit);
    if (c.cropW > 0) j["crop"] = {c.cropX, c.cropY, c.cropW, c.cropH};
    put("brightness", c.brightness, d.brightness);
    put("contrast", c.contrast, d.contrast);
    put("saturation", c.saturation, d.saturation);
    put("gamma", c.gamma, d.gamma);
    put("grayscale", c.grayscale, d.grayscale);
    put("lut", c.lut, d.lut);
    if (!c.transition.empty()) {
        j["transition"] = c.transition;
        j["transition_duration"] = c.transitionDuration;
    }
    if (c.kind == "title") {
        j["text"] = c.text;
        j["font_size"] = c.fontSize;
        j["color"] = c.color;
        j["position"] = c.position;
        put("font", c.font, d.font);
        put("box", c.box, d.box);
        put("box_color", c.boxColor, d.boxColor);
    }
    if (c.kind != "title") {
        json src = {{"duration", c.sourceDuration}, {"width", c.sourceWidth}, {"height", c.sourceHeight},
                    {"fps", c.sourceFps}, {"has_audio", c.hasAudio}};
        if (!c.videoCodec.empty()) src["video_codec"] = c.videoCodec;
        if (!c.audioCodec.empty()) src["audio_codec"] = c.audioCodec;
        j["probe"] = src;
    }
    return j;
}

Clip clipFrom(const json& j, const fs::path& baseDir) {
    Clip c;
    const Clip d;
    c.id = j.at("id").get<std::string>();
    c.kind = j.at("kind").get<std::string>();
    if (c.kind != "video" && c.kind != "image" && c.kind != "title" && c.kind != "audio") {
        throw std::runtime_error("unknown clip kind '" + c.kind + "'");
    }
    c.source = j.value("source", d.source);
    if (!c.source.empty()) {
        fs::path p = fs::path(c.source);
        if (p.is_relative() && !baseDir.empty()) c.source = (baseDir / p).lexically_normal().string();
    }
    c.start = j.value("start", 0.0);
    c.in = j.value("in", 0.0);
    c.out = j.value("out", 0.0);
    c.speed = j.value("speed", d.speed);
    c.volume = j.value("volume", d.volume);
    c.mute = j.value("mute", d.mute);
    c.fadeIn = j.value("fade_in", d.fadeIn);
    c.fadeOut = j.value("fade_out", d.fadeOut);
    c.opacity = j.value("opacity", d.opacity);
    c.x = j.value("x", d.x);
    c.y = j.value("y", d.y);
    c.width = j.value("width", d.width);
    c.height = j.value("height", d.height);
    c.fit = j.value("fit", d.fit);
    if (j.contains("crop") && j["crop"].is_array() && j["crop"].size() == 4) {
        c.cropX = j["crop"][0].get<double>();
        c.cropY = j["crop"][1].get<double>();
        c.cropW = j["crop"][2].get<double>();
        c.cropH = j["crop"][3].get<double>();
    }
    c.brightness = j.value("brightness", d.brightness);
    c.contrast = j.value("contrast", d.contrast);
    c.saturation = j.value("saturation", d.saturation);
    c.gamma = j.value("gamma", d.gamma);
    c.grayscale = j.value("grayscale", d.grayscale);
    c.lut = j.value("lut", d.lut);
    c.transition = j.value("transition", d.transition);
    c.transitionDuration = j.value("transition_duration", d.transitionDuration);
    c.text = j.value("text", d.text);
    c.font = j.value("font", d.font);
    c.fontSize = j.value("font_size", d.fontSize);
    c.color = j.value("color", d.color);
    c.position = j.value("position", d.position);
    c.box = j.value("box", d.box);
    c.boxColor = j.value("box_color", d.boxColor);
    if (j.contains("probe")) {
        const json& s = j["probe"];
        c.sourceDuration = s.value("duration", 0.0);
        c.sourceWidth = s.value("width", 0);
        c.sourceHeight = s.value("height", 0);
        c.sourceFps = s.value("fps", 0.0);
        c.hasAudio = s.value("has_audio", false);
        c.videoCodec = s.value("video_codec", std::string());
        c.audioCodec = s.value("audio_codec", std::string());
    }
    if (!(c.speed > 0) || c.out < c.in) throw std::runtime_error("clip " + c.id + " has a bad speed or in/out range");
    return c;
}

}  // namespace

std::string toJson(const Timeline& tl) {
    json tracks = json::array();
    for (const auto& t : tl.tracks) {
        json clips = json::array();
        for (const auto& c : t.clips) clips.push_back(clipJson(c));
        json tj = {{"id", t.id}, {"kind", t.kind}, {"clips", clips}};
        if (t.muted) tj["muted"] = true;
        if (t.hidden) tj["hidden"] = true;
        tracks.push_back(tj);
    }
    json j = {{"format", "achcut"},
              {"version", 1},
              {"settings",
               {{"width", tl.settings.width},
                {"height", tl.settings.height},
                {"fps", tl.settings.fps},
                {"background", tl.settings.background}}},
              {"next_clip", tl.nextClip},
              {"tracks", tracks}};
    return j.dump(2) + "\n";
}

std::shared_ptr<Timeline> fromJson(const std::string& text, const fs::path& baseDir) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a valid .achcut file: ") + e.what());
    }
    if (!j.is_object() || j.value("format", std::string()) != "achcut") {
        throw std::runtime_error("not an .achcut timeline (missing \"format\": \"achcut\")");
    }
    auto tl = std::make_shared<Timeline>();
    try {
        if (j.contains("settings")) {
            const json& s = j["settings"];
            tl->settings.width = s.value("width", 1920);
            tl->settings.height = s.value("height", 1080);
            tl->settings.fps = s.value("fps", 30.0);
            tl->settings.background = s.value("background", std::string("black"));
        }
        int highest = 0;
        for (const auto& tj : j.value("tracks", json::array())) {
            Track& t = tl->ensureTrack(tj.at("id").get<std::string>());
            t.muted = tj.value("muted", false);
            t.hidden = tj.value("hidden", false);
            for (const auto& cj : tj.value("clips", json::array())) {
                Clip c = clipFrom(cj, baseDir);
                if (tl->findClip(c.id)) throw std::runtime_error("two clips are called " + c.id);
                if ((t.kind == "audio") != (c.kind == "audio")) {
                    throw std::runtime_error("clip " + c.id + " is on the wrong kind of track");
                }
                if (c.id.size() > 1 && c.id[0] == 'c') highest = std::max(highest, std::atoi(c.id.c_str() + 1));
                t.clips.push_back(c);
            }
        }
        tl->nextClip = std::max(j.value("next_clip", 1), highest + 1);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("broken .achcut file: ") + e.what());
    }
    if (tl->settings.width < 16 || tl->settings.height < 16 || !(tl->settings.fps > 0)) {
        throw std::runtime_error("broken .achcut file: bad resolution or fps");
    }
    tl->sortClips();
    return tl;
}

const std::vector<std::string>& transitionNames() {
    static const std::vector<std::string> names = {
        "none", "crossfade", "dissolve", "fadeblack", "fadewhite", "wipeleft", "wiperight", "wipeup", "wipedown",
        "slideleft", "slideright", "slideup", "slidedown", "circleopen", "circleclose", "radial", "pixelize",
        "smoothleft", "smoothright", "distance"};
    return names;
}

std::string xfadeName(const std::string& type) {
    if (type == "crossfade" || type == "fade") return "fade";
    if (type == "none") return {};
    for (const auto& n : transitionNames()) {
        if (n == type) return n;
    }
    return {};
}

MediaInfo parseProbe(const std::string& text) {
    json j = json::parse(text);
    MediaInfo m;
    auto number = [](const json& v) -> double {
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) {
            try {
                return std::stod(v.get<std::string>());
            } catch (const std::exception&) {
                return 0;
            }
        }
        return 0;
    };
    auto rate = [](const std::string& r) -> double {
        auto slash = r.find('/');
        try {
            if (slash == std::string::npos) return std::stod(r);
            double a = std::stod(r.substr(0, slash)), b = std::stod(r.substr(slash + 1));
            return b > 0 ? a / b : 0;
        } catch (const std::exception&) {
            return 0;
        }
    };
    if (j.contains("format")) {
        m.format = j["format"].value("format_name", std::string());
        if (j["format"].contains("duration")) m.duration = number(j["format"]["duration"]);
    }
    for (const auto& s : j.value("streams", json::array())) {
        StreamInfo si;
        si.type = s.value("codec_type", std::string());
        si.codec = s.value("codec_name", std::string());
        if (si.type == "video") {
            si.width = s.value("width", 0);
            si.height = s.value("height", 0);
            si.fps = rate(s.value("avg_frame_rate", std::string()));
            if (!(si.fps > 0)) si.fps = rate(s.value("r_frame_rate", std::string()));
            const bool cover = s.contains("disposition") && s["disposition"].value("attached_pic", 0) == 1;
            if (!m.hasVideo && !cover) {
                m.hasVideo = true;
                m.width = si.width;
                m.height = si.height;
                m.fps = si.fps;
                m.videoCodec = si.codec;
            }
        } else if (si.type == "audio") {
            si.sampleRate = static_cast<int>(number(s.value("sample_rate", json("0"))));
            si.channels = s.value("channels", 0);
            if (!m.hasAudio) {
                m.hasAudio = true;
                m.audioCodec = si.codec;
            }
        }
        if (!(m.duration > 0) && s.contains("duration")) m.duration = number(s["duration"]);
        m.streams.push_back(si);
    }
    static const std::vector<std::string> stillCodecs = {"png", "mjpeg", "bmp", "webp", "tiff", "gif", "targa", "ppm", "jpegls", "pam"};
    const bool imageFormat = m.format.find("image2") != std::string::npos || m.format.find("_pipe") != std::string::npos ||
                             m.format == "png" || m.format == "webp";
    m.still = m.hasVideo && !m.hasAudio && imageFormat &&
              std::find(stillCodecs.begin(), stillCodecs.end(), m.videoCodec) != stillCodecs.end();
    return m;
}

}  // namespace achclwss::cut
