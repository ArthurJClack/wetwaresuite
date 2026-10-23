// Scenes: the model, its JSON, drawing it (through the cut compositor) and
// planning the one ffmpeg run that records it.
#include "wetwaresuite/capture/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <nlohmann/json.hpp>

#include "plan_internal.hpp"
#include "wetwaresuite/cut/cut.hpp"
#include "wetwaresuite/cut/render.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::capture {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string fixed(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string extOf(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string label(const Source& s) {
    if (!s.name.empty()) return s.name;
    if (s.kind == "screen") return s.monitor >= 0 ? "Screen " + std::to_string(s.monitor) : "Screen";
    if (s.kind == "window") return "Window " + s.window;
    if (s.kind == "webcam") return "Webcam";
    return s.kind;
}

// A scene source as cut clips on their own track (a still or matte plus, for live placeholders, a label).
void addClips(cut::Timeline& tl, const Scene& sc, const Source& s, int& track, const std::map<std::string, std::string>& liveFrames) {
    cut::Clip c;
    c.start = 0;
    c.out = 1;
    c.opacity = std::clamp(s.opacity, 0.0, 1.0);
    c.x = s.x;
    c.y = s.y;
    c.width = s.width;
    c.height = s.height;
    auto put = [&](cut::Clip clip) {
        clip.id = tl.newClipId();
        tl.ensureTrack("v" + std::to_string(track++)).clips.push_back(clip);
    };
    if (s.kind == "image" || (s.live() && liveFrames.count(s.id))) {
        const std::string path = s.kind == "image" ? s.path : liveFrames.at(s.id);
        raster::Image img = raster::load(path);
        c.kind = "image";
        c.source = path;
        c.sourceWidth = img.width();
        c.sourceHeight = img.height();
        if (s.kind == "image" && c.width <= 0 && c.height <= 0) {
            c.width = img.width();
            c.height = img.height();
        }
        if (s.kind == "image") c.fit = "stretch";
        if (s.live() && c.width > 0 && c.height > 0) c.fit = "stretch";
        put(c);
    } else if (s.kind == "text") {
        c.kind = "title";
        c.text = s.text.empty() ? std::string(" ") : s.text;
        c.position = "custom";
        c.fontSize = s.fontSize;
        c.color = s.color;
        c.font = s.font;
        c.outline = s.outline;
        c.outlineColor = s.outlineColor;
        c.box = s.box;
        c.align = "left";
        put(c);
    } else if (s.kind == "color") {
        c.kind = "color";
        c.color = s.color;
        put(c);
    } else {
        // a live source without a frame: a grey box with its name
        c.kind = "color";
        c.color = "#2b2f36";
        put(c);
        cut::Clip t;
        t.kind = "title";
        t.start = 0;
        t.out = 1;
        t.text = label(s);
        t.position = "custom";
        const double w = s.width > 0 ? s.width : sc.width, h = s.height > 0 ? s.height : (s.width > 0 ? s.width * 9 / 16 : sc.height);
        t.fontSize = std::max(10.0, std::min(w, h) / 6);
        t.x = s.x + 0.06 * w;
        t.y = s.y + 0.06 * h;
        t.color = "#c8ccd4";
        t.opacity = c.opacity;
        put(t);
    }
}

std::string pngOf(const cut::Timeline& tl, int width, bool transparent) {
    cut::FrameRequest rq;
    rq.t = 0;
    rq.width = width;
    rq.transparent = transparent;
    return raster::encodePng(cut::renderFrame(tl, rq));
}

cut::Timeline canvas(const Scene& sc) {
    cut::Timeline tl;
    tl.settings.width = sc.width;
    tl.settings.height = sc.height;
    tl.settings.fps = sc.fps;
    tl.settings.background = sc.background;
    return tl;
}

std::string hexColour(const std::string& c) {
    auto col = raster::Color::parse(c);
    if (!col) return {};
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%02X%02X%02X", col->r, col->g, col->b);
    return buf;
}

}  // namespace

std::string Scene::summary() const {
    return name + ": " + std::to_string(width) + "x" + std::to_string(height) + " " + std::to_string(fps) + "fps, " + std::to_string(sources.size()) +
           (sources.size() == 1 ? " source" : " sources") + (path.empty() ? "" : ", " + path);
}

Source* Scene::find(const std::string& id) {
    for (auto& s : sources) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::unique_ptr<Attachment> Scene::preview() const {
    try {
        auto a = std::make_unique<Attachment>();
        a->mime = "image/png";
        a->bytes = renderScene(*this, {}, std::min(width, 960));
        a->name = "scene.png";
        return a;
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::string renderScene(const Scene& sc, const std::map<std::string, std::string>& liveFrames, int width) {
    cut::Timeline tl = canvas(sc);
    int track = 1;
    for (const auto& s : sc.sources) {
        if (s.visible) addClips(tl, sc, s, track, liveFrames);
    }
    if (tl.tracks.empty()) {
        // nothing on it yet: the background alone
        cut::Clip c;
        c.kind = "color";
        c.color = sc.background;
        c.out = 1;
        c.id = tl.newClipId();
        tl.ensureTrack("v1").clips.push_back(c);
    }
    return pngOf(tl, width, false);
}

std::string renderStaticSource(const Scene& sc, const Source& s) {
    cut::Timeline tl = canvas(sc);
    int track = 1;
    addClips(tl, sc, s, track, {});
    return pngOf(tl, 0, true);
}

std::string toJson(const Scene& sc) {
    json j = {{"format", "wwscene"}, {"version", 1}, {"name", sc.name}, {"width", sc.width}, {"height", sc.height}, {"fps", sc.fps},
              {"background", sc.background}, {"next_source", sc.nextSource}};
    json list = json::array();
    const Source d;
    for (const auto& s : sc.sources) {
        json o = {{"id", s.id}, {"kind", s.kind}, {"x", s.x}, {"y", s.y}, {"width", s.width}, {"height", s.height}};
        if (!s.name.empty()) o["name"] = s.name;
        if (s.opacity != d.opacity) o["opacity"] = s.opacity;
        if (!s.visible) o["visible"] = false;
        if (s.monitor >= 0) o["monitor"] = s.monitor;
        if (s.region.set()) o["region"] = {s.region.x, s.region.y, s.region.width, s.region.height};
        if (!s.window.empty()) o["window"] = s.window;
        if (s.cursor != d.cursor) o["cursor"] = s.cursor;
        if (!s.device.empty()) o["device"] = s.device;
        if (s.captureWidth > 0) o["capture_size"] = {s.captureWidth, s.captureHeight};
        if (!s.keyColor.empty()) o["key_color"] = s.keyColor, o["key_similarity"] = s.keySimilarity;
        if (!s.path.empty()) o["path"] = s.path;
        if (s.kind == "text") o["text"] = s.text, o["font_size"] = s.fontSize;
        if (!s.font.empty()) o["font"] = s.font;
        if (s.color != d.color) o["color"] = s.color;
        if (s.outline > 0) o["outline"] = s.outline, o["outline_color"] = s.outlineColor;
        if (s.box) o["box"] = true;
        list.push_back(o);
    }
    j["sources"] = list;
    return j.dump(2) + "\n";
}

std::shared_ptr<Scene> sceneFromJson(const std::string& text, const fs::path& baseDir) {
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("not a scene file: ") + e.what());
    }
    if (!j.is_object() || j.value("format", "") != "wwscene") throw std::runtime_error("not a .wwscene file");
    auto sc = std::make_shared<Scene>();
    sc->name = j.value("name", "Scene");
    sc->width = j.value("width", 1920);
    sc->height = j.value("height", 1080);
    sc->fps = j.value("fps", 30);
    sc->background = j.value("background", "black");
    sc->nextSource = j.value("next_source", 1);
    for (const auto& o : j.value("sources", json::array())) {
        Source s;
        s.id = o.value("id", "");
        s.kind = o.value("kind", "");
        if (s.id.empty() || (s.kind != "screen" && s.kind != "window" && s.kind != "webcam" && s.kind != "image" && s.kind != "text" && s.kind != "color")) {
            throw std::runtime_error("a source in the scene has no id or an unknown kind");
        }
        s.name = o.value("name", "");
        s.x = o.value("x", 0.0);
        s.y = o.value("y", 0.0);
        s.width = o.value("width", 0.0);
        s.height = o.value("height", 0.0);
        s.opacity = o.value("opacity", 1.0);
        s.visible = o.value("visible", true);
        s.monitor = o.value("monitor", -1);
        if (o.contains("region") && o["region"].is_array() && o["region"].size() == 4) {
            s.region = {o["region"][0].get<int>(), o["region"][1].get<int>(), o["region"][2].get<int>(), o["region"][3].get<int>()};
        }
        s.window = o.value("window", "");
        s.cursor = o.value("cursor", true);
        s.device = o.value("device", "");
        if (o.contains("capture_size") && o["capture_size"].is_array() && o["capture_size"].size() == 2) {
            s.captureWidth = o["capture_size"][0].get<int>();
            s.captureHeight = o["capture_size"][1].get<int>();
        }
        s.keyColor = o.value("key_color", "");
        s.keySimilarity = o.value("key_similarity", 0.3);
        s.path = o.value("path", "");
        if (!s.path.empty() && fs::path(s.path).is_relative() && !baseDir.empty()) s.path = (baseDir / s.path).lexically_normal().string();
        s.text = o.value("text", "");
        s.fontSize = o.value("font_size", 48.0);
        s.font = o.value("font", "");
        s.color = o.value("color", "white");
        s.outline = o.value("outline", 0.0);
        s.outlineColor = o.value("outline_color", "black");
        s.box = o.value("box", false);
        sc->sources.push_back(s);
        const int n = std::atoi(s.id.c_str() + 1);
        sc->nextSource = std::max(sc->nextSource, n + 1);
    }
    return sc;
}

Plan planSceneRecord(const Environment& env, const Scene& sc, const SceneRecordRequest& rq, const std::map<std::string, std::string>& stills) {
    Plan plan;
    const std::string ext = extOf(rq.output);
    if (ext != "mp4" && ext != "mkv" && ext != "mov" && ext != "webm" && ext != "gif") {
        plan.error = "recordings save as .mp4, .mkv, .mov, .webm or .gif";
        return plan;
    }
    if (rq.duration < 0 || rq.duration > 4 * 3600) {
        plan.error = "duration must be 0..4 hours (in seconds)";
        return plan;
    }
    if (rq.duration <= 0 && !rq.forStop) {
        plan.error = "give a duration in seconds, or use capture.start_recording and capture.stop_recording";
        return plan;
    }
    if (sc.width < 16 || sc.height < 16 || sc.width % 2 || sc.height % 2) {
        plan.error = "the canvas needs an even width and height of at least 16";
        return plan;
    }
    if (!env.hasFfmpeg) {
        plan.error = "recording needs ffmpeg; install it (it's free): Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`";
        return plan;
    }
    plan.args = {"ffmpeg", "-hide_banner"};
    if (!rq.forStop) plan.args.push_back("-nostdin");
    plan.args.insert(plan.args.end(), {"-nostats", "-loglevel", "error", "-y"});
    const std::string fps = std::to_string(sc.fps);
    const std::string W = std::to_string(sc.width), H = std::to_string(sc.height);
    std::string graph = "color=c=" + (hexColour(sc.background).empty() ? std::string("black") : hexColour(sc.background)) + ":s=" + W + "x" + H + ":r=" + fps + "[b0]";
    int input = 0, layer = 0;
    std::vector<std::string> backends;
    for (const auto& s : sc.sources) {
        if (!s.visible) continue;
        std::vector<std::string> filters;
        if (s.kind == "screen") {
            const std::string backend = pickBackend(env, "auto", false, plan.error);
            if (!plan.ok()) return plan;
            if (!screenInput(env, backend, s.monitor, s.region, s.cursor, sc.fps, rq.duration, false, {}, plan.args, filters, plan.error)) return plan;
            backends.push_back(backend);
        } else if (s.kind == "window") {
            if (env.platform != Platform::Windows) {
                plan.error = "window capture by title works on Windows; on Linux and macOS add a screen source with a capture region instead";
                return plan;
            }
            plan.args.insert(plan.args.end(), {"-f", "gdigrab", "-draw_mouse", s.cursor ? "1" : "0", "-framerate", fps});
            if (rq.duration > 0) plan.args.insert(plan.args.end(), {"-t", fixed(rq.duration)});
            plan.args.insert(plan.args.end(), {"-i", "title=" + s.window});
            backends.push_back("gdigrab");
        } else if (s.kind == "webcam") {
            std::vector<std::string> size;
            if (s.captureWidth > 0) size = {"-video_size", std::to_string(s.captureWidth) + "x" + std::to_string(s.captureHeight)};
            if (env.platform == Platform::Linux) {
                plan.args.insert(plan.args.end(), {"-f", "v4l2"});
                plan.args.insert(plan.args.end(), size.begin(), size.end());
                plan.args.insert(plan.args.end(), {"-i", s.device.empty() ? "/dev/video0" : s.device});
                backends.push_back("v4l2");
            } else if (env.platform == Platform::Windows) {
                if (s.device.empty()) {
                    plan.error = "the webcam source needs device (a camera name from capture.list_devices)";
                    return plan;
                }
                plan.args.insert(plan.args.end(), {"-f", "dshow"});
                plan.args.insert(plan.args.end(), size.begin(), size.end());
                plan.args.insert(plan.args.end(), {"-i", "video=" + s.device});
                backends.push_back("dshow");
            } else {
                plan.args.insert(plan.args.end(), {"-f", "avfoundation", "-framerate", "30"});
                plan.args.insert(plan.args.end(), size.begin(), size.end());
                plan.args.insert(plan.args.end(), {"-i", (s.device.empty() ? std::string("0") : s.device) + ":none"});
                backends.push_back("avfoundation");
            }
        } else {
            auto it = stills.find(s.id);
            if (it == stills.end()) {
                plan.error = "source " + s.id + " wasn't drawn";
                return plan;
            }
            plan.args.insert(plan.args.end(), {"-loop", "1", "-framerate", fps, "-i", it->second});
        }
        std::string chain = "[" + std::to_string(input) + ":v]";
        for (const auto& f : filters) chain += f + ",";
        std::string x = "0", y = "0";
        if (s.live()) {
            const int w = static_cast<int>(std::lround(s.width)), h = static_cast<int>(std::lround(s.height));
            const std::string scale = w > 0 && h > 0 ? "scale=" + std::to_string(w) + ":" + std::to_string(h)
                                      : w > 0       ? "scale=" + std::to_string(w) + ":-2"
                                                    : "scale=" + W + ":" + H;
            chain += scale + ",format=rgba";
            if (!s.keyColor.empty()) {
                const std::string hex = hexColour(s.keyColor);
                if (hex.empty()) {
                    plan.error = "key colour '" + s.keyColor + "' isn't a colour";
                    return plan;
                }
                chain += ",colorkey=" + hex + ":" + fixed(std::clamp(s.keySimilarity, 0.01, 1.0)) + ":0.1";
            }
            if (s.opacity < 1) chain += ",colorchannelmixer=aa=" + fixed(std::clamp(s.opacity, 0.0, 1.0));
            x = std::to_string(static_cast<int>(std::lround(s.x)));
            y = std::to_string(static_cast<int>(std::lround(s.y)));
        } else {
            chain += "format=rgba";
        }
        const std::string in = "[b" + std::to_string(layer) + "]", out = "[b" + std::to_string(layer + 1) + "]";
        graph += ";" + chain + "[s" + std::to_string(layer) + "];" + in + "[s" + std::to_string(layer) + "]overlay=x=" + x + ":y=" + y +
                 ":eof_action=repeat:shortest=0" + out;
        ++input;
        ++layer;
    }
    const bool gif = ext == "gif";
    const bool audio = rq.audio && !gif;
    int audioInput = -1;
    if (audio) {
        audioInput = input;
        if (env.platform == Platform::Linux) {
            plan.args.insert(plan.args.end(), {"-thread_queue_size", "1024", "-f", "pulse", "-i", rq.audioDevice.empty() ? "default" : rq.audioDevice});
        } else if (env.platform == Platform::Windows) {
            if (rq.audioDevice.empty()) {
                plan.error = "recording sound on Windows needs audio_device (a name from capture.list_devices)";
                return plan;
            }
            plan.args.insert(plan.args.end(), {"-thread_queue_size", "1024", "-f", "dshow", "-i", "audio=" + rq.audioDevice});
        } else {
            plan.args.insert(plan.args.end(), {"-f", "avfoundation", "-i", ":" + (rq.audioDevice.empty() ? std::string("0") : rq.audioDevice)});
        }
    }
    const std::string last = "[b" + std::to_string(layer) + "]";
    if (gif) {
        graph += ";" + last + "fps=" + std::to_string(std::min(sc.fps, 15)) +
                 ",scale='min(iw,960)':-1:flags=lanczos,split[g1][g2];[g1]palettegen=stats_mode=diff[p];[g2][p]paletteuse=dither=bayer:bayer_scale=4[out]";
    } else {
        graph += ";" + last + "format=yuv420p[out]";
    }
    plan.args.insert(plan.args.end(), {"-filter_complex", graph, "-map", "[out]"});
    if (audio) plan.args.insert(plan.args.end(), {"-map", std::to_string(audioInput) + ":a"});
    if (gif) {
        plan.args.insert(plan.args.end(), {"-loop", "0"});
    } else if (ext == "webm") {
        const std::string crf = rq.quality == "draft" ? "42" : rq.quality == "best" ? "28" : "35";
        plan.args.insert(plan.args.end(), {"-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-crf", crf, "-b:v", "0", "-row-mt", "1"});
        if (audio) plan.args.insert(plan.args.end(), {"-c:a", "libopus", "-b:a", "128k"});
    } else {
        const std::string preset = rq.quality == "draft" ? "ultrafast" : rq.quality == "best" ? "medium" : "veryfast";
        const std::string crf = rq.quality == "draft" ? "28" : rq.quality == "best" ? "18" : "23";
        plan.args.insert(plan.args.end(), {"-c:v", "libx264", "-preset", preset, "-crf", crf});
        if (audio) plan.args.insert(plan.args.end(), {"-c:a", "aac", "-b:a", "160k"});
        // mkv survives being stopped hard; mp4/mov get their index written when ffmpeg is told to stop (q)
        if (ext != "mkv") plan.args.insert(plan.args.end(), {"-movflags", "+faststart"});
    }
    if (!audio) plan.args.push_back("-an");
    if (rq.duration > 0) plan.args.insert(plan.args.end(), {"-t", fixed(rq.duration)});
    plan.args.push_back(rq.output);
    std::string b;
    for (const auto& x : backends) {
        if (b.find(x) == std::string::npos) b += (b.empty() ? "" : "+") + x;
    }
    plan.backend = b.empty() ? "stills" : b;
    return plan;
}

}  // namespace wetwaresuite::capture
