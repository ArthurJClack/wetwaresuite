// capture.* scene commands (OBS-style scenes with sources) and recordings that
// run in the background until stopped.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>

#include "wetwaresuite/capture/scene.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::capture {

namespace fs = std::filesystem;

namespace {

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// One undo step on a scene: snapshot, change, restore on failure or undo.
class SceneEdit : public Action {
public:
    SceneEdit(std::shared_ptr<Scene> doc, std::function<Result(Scene&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override {
        before_ = *doc_;
        Result r;
        try {
            r = fn_(*doc_);
        } catch (const std::exception& e) {
            r = Result::failure(e.what());
        }
        if (!r.ok) revert();
        return r;
    }
    void revert() override {
        doc_->name = before_.name;
        doc_->width = before_.width;
        doc_->height = before_.height;
        doc_->fps = before_.fps;
        doc_->background = before_.background;
        doc_->sources = before_.sources;
        doc_->nextSource = before_.nextSource;
    }

private:
    std::shared_ptr<Scene> doc_;
    std::function<Result(Scene&)> fn_;
    Scene before_;
};

using SceneFn = std::function<Result(Scene&, const Params&)>;

ActionFactory sceneCommand(Session& session, SceneFn fn, bool undoable = true) {
    return [&session, fn, undoable](const Params& p) -> std::unique_ptr<Action> {
        const std::string id = stringParam(p, "doc");
        auto doc = session.workspace().getAs<Scene>(id);
        if (!doc) {
            return std::make_unique<FunctionAction>([id] { return Result::failure("no open scene called '" + id + "' (capture.scene_new makes one)"); });
        }
        if (!undoable) {
            return std::make_unique<FunctionAction>([doc, fn, p] {
                try {
                    return fn(*doc, p);
                } catch (const std::exception& e) {
                    return Result::failure(e.what());
                }
            });
        }
        return std::make_unique<SceneEdit>(doc, [fn, p](Scene& s) { return fn(s, p); });
    };
}

ParamSpec docParam() { return {"doc", ParamType::String, "id of an open scene, e.g. scene1", true}; }

const std::vector<std::string> kKinds = {"screen", "window", "webcam", "image", "text", "color"};

std::vector<ParamSpec> sourceSpecs() {
    return {{"name", ParamType::String, "what to call it", false},
            {"x", ParamType::Number, "left edge on the canvas, px", false},
            {"y", ParamType::Number, "top edge on the canvas, px", false},
            {"width", ParamType::Number, "width on the canvas, px (0 = the whole canvas; images: their own size)", false},
            {"height", ParamType::Number, "height on the canvas, px (0 = from the width)", false},
            {"opacity", ParamType::Number, "0 (invisible) .. 1 (solid)", false},
            {"visible", ParamType::Bool, "show it (hidden sources aren't recorded)", false},
            {"monitor", ParamType::Int, "screen: which monitor (capture.list_displays)", false},
            {"capture_region", ParamType::NumberList, "screen: grab only x,y,width,height of the screen", false},
            {"window", ParamType::String, "window: the window's title (Windows)", false},
            {"cursor", ParamType::Bool, "screen/window: show the mouse pointer", false},
            {"device", ParamType::String, "webcam: /dev/video0, a camera name (Windows) or an index (macOS)", false},
            {"capture_size", ParamType::NumberList, "webcam: ask the camera for width,height", false},
            {"key_color", ParamType::String, "webcam: green screen, make this colour see-through (e.g. #00ff00; empty = off)", false},
            {"key_similarity", ParamType::Number, "webcam: how close a colour must be to vanish, 0.01..1", false},
            {"path", ParamType::String, "image: the picture file", false},
            {"text", ParamType::String, "text: what it says", false},
            {"font", ParamType::String, "text: a .ttf/.otf file or a family name", false},
            {"font_size", ParamType::Number, "text: size in px", false},
            {"color", ParamType::String, "text or colour box: the colour", false},
            {"outline", ParamType::Number, "text: outline width in px", false},
            {"outline_color", ParamType::String, "text: outline colour", false},
            {"box", ParamType::Bool, "text: a box behind it", false}};
}

std::string applySource(Source& s, const Params& p) {
    auto n = [&](const char* k, double& f) {
        if (has(p, k)) f = numberParam(p, k);
    };
    auto b = [&](const char* k, bool& f) {
        if (has(p, k)) f = boolParam(p, k);
    };
    auto t = [&](const char* k, std::string& f) {
        if (has(p, k)) f = stringParam(p, k);
    };
    t("name", s.name);
    n("x", s.x);
    n("y", s.y);
    n("width", s.width);
    n("height", s.height);
    n("opacity", s.opacity);
    b("visible", s.visible);
    if (has(p, "monitor")) s.monitor = static_cast<int>(intParam(p, "monitor"));
    if (has(p, "capture_region")) {
        NumberList l = listParam(p, "capture_region");
        if (l.size() != 4 && !l.empty()) return "capture_region takes x,y,width,height";
        s.region = l.empty() ? Region{} : Region{static_cast<int>(l[0]), static_cast<int>(l[1]), static_cast<int>(l[2]), static_cast<int>(l[3])};
    }
    t("window", s.window);
    b("cursor", s.cursor);
    t("device", s.device);
    if (has(p, "capture_size")) {
        NumberList l = listParam(p, "capture_size");
        if (l.size() != 2) return "capture_size takes width,height";
        s.captureWidth = static_cast<int>(l[0]);
        s.captureHeight = static_cast<int>(l[1]);
    }
    t("key_color", s.keyColor);
    n("key_similarity", s.keySimilarity);
    if (has(p, "path")) {
        std::error_code ec;
        s.path = fs::absolute(stringParam(p, "path"), ec).lexically_normal().string();
    }
    t("text", s.text);
    t("font", s.font);
    n("font_size", s.fontSize);
    t("color", s.color);
    n("outline", s.outline);
    t("outline_color", s.outlineColor);
    b("box", s.box);
    // checks
    if (s.opacity < 0 || s.opacity > 1) return "opacity must be 0..1";
    if (s.width < 0 || s.height < 0) return "width and height can't be negative";
    if (s.kind == "image") {
        std::error_code ec;
        if (s.path.empty() || !fs::is_regular_file(s.path, ec)) return "an image source needs path (a picture file that exists)";
    }
    if (s.kind == "text" && s.text.empty()) return "a text source needs text";
    if (s.kind == "text" && (s.fontSize < 4 || s.fontSize > 1000)) return "font_size must be 4..1000";
    if (s.kind == "window" && s.window.empty()) return "a window source needs window (its title)";
    if (!raster::Color::parse(s.color) && s.color.find('@') == std::string::npos) return "'" + s.color + "' isn't a colour";
    if (!s.keyColor.empty() && !raster::Color::parse(s.keyColor)) return "key_color '" + s.keyColor + "' isn't a colour";
    if (s.keySimilarity < 0.01 || s.keySimilarity > 1) return "key_similarity must be 0.01..1";
    return {};
}

std::string describe(const Source& s) {
    std::string d = s.id + " " + s.kind;
    if (!s.name.empty()) d += " \"" + s.name + "\"";
    if (s.kind == "screen") d += s.monitor >= 0 ? " monitor " + std::to_string(s.monitor) : " (all screens)";
    if (s.region.set()) d += " region " + std::to_string(s.region.x) + "," + std::to_string(s.region.y) + " " + std::to_string(s.region.width) + "x" +
                             std::to_string(s.region.height);
    if (s.kind == "window") d += " \"" + s.window + "\"";
    if (s.kind == "webcam") d += " " + (s.device.empty() ? std::string("default camera") : s.device) + (s.keyColor.empty() ? "" : " keyed " + s.keyColor);
    if (s.kind == "image") d += " " + fs::path(s.path).filename().string();
    if (s.kind == "text") d += " \"" + s.text + "\" " + num(s.fontSize) + "px " + s.color;
    if (s.kind == "color") d += " " + s.color;
    d += " at " + num(s.x) + "," + num(s.y);
    if (s.width > 0) d += " " + num(s.width) + "x" + num(s.height);
    if (s.opacity < 1) d += " opacity " + num(s.opacity);
    if (!s.visible) d += " (hidden)";
    return d;
}

fs::path freshDir() {
    std::random_device rd;
    fs::path d = fs::temp_directory_path() / ("wetwaresuite-scene-" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(d);
    return d;
}

// Draws the still sources for recording into `dir`.
std::map<std::string, std::string> drawStills(const Scene& sc, const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& s : sc.sources) {
        if (!s.visible || s.live()) continue;
        const fs::path png = dir / (s.id + ".png");
        std::ofstream f(png, std::ios::binary);
        const std::string bytes = renderStaticSource(sc, s);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out[s.id] = png.string();
    }
    return out;
}

struct Recording {
    std::unique_ptr<ChildProcess> proc;
    std::string output, what;
    fs::path stills;
    std::chrono::steady_clock::time_point started;
};

std::mutex& recLock() {
    static std::mutex m;
    return m;
}
Result stopOne(const std::string& id, Recording& rec);

// Recordings still running when the program ends (a batch script's last line, a closed MCP
// session) are stopped properly, so their files are finished rather than cut off.
struct RecordingList {
    std::map<std::string, Recording> list;
    RecordingList() = default;
    RecordingList(const RecordingList&) = delete;
    RecordingList& operator=(const RecordingList&) = delete;
    ~RecordingList() {
        for (auto& [id, rec] : list) {
            if (rec.proc && rec.proc->running()) stopOne(id, rec);
        }
    }
};

std::map<std::string, Recording>& recordings() {
    static RecordingList r;
    return r.list;
}
int& recCounter() {
    static int n = 0;
    return n;
}

Result stopOne(const std::string& id, Recording& rec) {
    std::string out;
    if (rec.proc->running()) {
        rec.proc->write("q");
        rec.proc->closeInput();
        if (!rec.proc->wait(15, out)) {
            rec.proc->kill();
            rec.proc->wait(5, out);
        }
    } else {
        rec.proc->wait(1, out);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - rec.started).count();
    std::error_code ec;
    if (!rec.stills.empty()) fs::remove_all(rec.stills, ec);
    const auto bytes = static_cast<std::int64_t>(fs::file_size(rec.output, ec));
    if (ec || bytes == 0) {
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return Result::failure("recording " + id + " wrote nothing to " + rec.output + (out.empty() ? "" : ": " + out.substr(out.size() > 800 ? out.size() - 800 : 0)));
    }
    Result r = Result::success("stopped " + id + " after " + num(std::round(secs * 10) / 10) + "s: " + rec.output, rec.output);
    return r.with("recording", id).with("output", rec.output).with("bytes", bytes).with("seconds", secs);
}

Result startProcess(Plan plan, const std::string& output, const std::string& what, const fs::path& stills) {
    auto exe = findProgram(plan.args[0]);
    if (!exe) return Result::failure(plan.args[0] + " isn't installed");
    plan.args[0] = exe->string();
    auto proc = std::make_unique<ChildProcess>();
    std::string err;
    if (!proc->start(plan.args, {}, err)) return Result::failure("couldn't start ffmpeg: " + err);
    // a capture device that can't open fails at once: give it a moment to say so
    std::string early = proc->readFor(0.6);
    if (!proc->running()) {
        std::error_code ec;
        if (!stills.empty()) fs::remove_all(stills, ec);
        while (!early.empty() && (early.back() == '\n' || early.back() == '\r')) early.pop_back();
        return Result::failure("the recording stopped at once" + (early.empty() ? std::string() : ": " + early));
    }
    std::lock_guard<std::mutex> g(recLock());
    const std::string id = "rec" + std::to_string(++recCounter());
    recordings()[id] = {std::move(proc), output, what, stills, std::chrono::steady_clock::now()};
    Result r = Result::success("recording " + what + " to " + output + " as " + id + " (capture.stop_recording recording=" + id + " ends it)", id);
    return r.with("recording", id).with("output", output).with("backend", plan.backend);
}

std::string prepareOutput(const std::string& output, const Params& p) {
    std::error_code ec;
    if (fs::exists(output, ec) && !boolParam(p, "overwrite", true)) return output + " already exists (pass overwrite=true)";
    const fs::path parent = fs::path(output).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    return {};
}

std::vector<ParamSpec> recordSpecs(bool timed) {
    std::vector<ParamSpec> s = {{"output", ParamType::String, ".mp4, .mkv, .mov, .webm or .gif to write", true}};
    if (timed) s.push_back({"duration", ParamType::Number, "seconds to record", false, {}, 10.0});
    s.push_back({"quality", ParamType::String, "encoding preset", false, {"draft", "good", "best"}, std::string("good")});
    s.push_back({"audio", ParamType::Bool, "also record a microphone / sound input", false, {}, false});
    s.push_back({"audio_device", ParamType::String, "the sound input (capture.list_devices); default the system's", false});
    s.push_back({"display", ParamType::String, "Linux: X display to use instead of $DISPLAY", false});
    s.push_back({"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, true});
    return s;
}

void registerSourceCommands(Session& session) {
    auto& reg = session.registry();
    {
        std::vector<ParamSpec> s = {docParam(), {"kind", ParamType::String, "what it shows", true, kKinds}};
        for (auto& x : sourceSpecs()) s.push_back(x);
        reg.add({"capture.add_source", "Add Source",
                 "Put a source on top of a scene: screen (all, a monitor or a capture_region), window (by title, "
                 "Windows), webcam (with an optional green-screen key), image, text or a colour box. x, y, width and "
                 "height place it on the canvas, e.g. a webcam in the corner over the screen.",
                 s, true},
                sceneCommand(session, [](Scene& sc, const Params& p) {
                    Source src;
                    src.kind = stringParam(p, "kind");
                    if (std::find(kKinds.begin(), kKinds.end(), src.kind) == kKinds.end()) return Result::failure("unknown source kind '" + src.kind + "'");
                    if (src.kind == "color") src.color = "black";
                    if (std::string err = applySource(src, p); !err.empty()) return Result::failure(err);
                    src.id = "s" + std::to_string(sc.nextSource++);
                    sc.sources.push_back(src);
                    Result r = Result::success("added " + describe(src), src.id);
                    return r.with("source", src.id).with("sources", static_cast<std::int64_t>(sc.sources.size()));
                }));
    }
    {
        std::vector<ParamSpec> s = {docParam(), {"source", ParamType::String, "source id, e.g. s2", true},
                                    {"order", ParamType::String, "move it in the stack", false, {"up", "down", "top", "bottom"}}};
        for (auto& x : sourceSpecs()) s.push_back(x);
        reg.add({"capture.set_source", "Change Source", "Move, resize, fade, hide, restack or reconfigure a scene source.", s, true},
                sceneCommand(session, [](Scene& sc, const Params& p) {
                    const std::string id = stringParam(p, "source");
                    Source* src = sc.find(id);
                    if (!src) return Result::failure("no source called '" + id + "' (capture.scene_info lists them)");
                    Source copy = *src;
                    if (std::string err = applySource(copy, p); !err.empty()) return Result::failure(err);
                    *src = copy;
                    if (has(p, "order")) {
                        const std::string o = stringParam(p, "order");
                        auto it = std::find_if(sc.sources.begin(), sc.sources.end(), [&](const Source& x) { return x.id == id; });
                        const Source moved = *it;
                        const auto pos = static_cast<std::size_t>(it - sc.sources.begin());
                        sc.sources.erase(it);
                        std::size_t to = pos;
                        if (o == "top") to = sc.sources.size();
                        else if (o == "bottom") to = 0;
                        else if (o == "up") to = std::min(pos + 1, sc.sources.size());
                        else if (o == "down") to = pos == 0 ? 0 : pos - 1;
                        sc.sources.insert(sc.sources.begin() + static_cast<std::ptrdiff_t>(to), moved);
                    }
                    Result r = Result::success("changed " + describe(*sc.find(id)));
                    return r.with("source", id);
                }));
    }
    reg.add({"capture.remove_source", "Remove Source", "Take a source off a scene.",
             {docParam(), {"source", ParamType::String, "source id", true}}, true},
            sceneCommand(session, [](Scene& sc, const Params& p) {
                const std::string id = stringParam(p, "source");
                auto it = std::find_if(sc.sources.begin(), sc.sources.end(), [&](const Source& x) { return x.id == id; });
                if (it == sc.sources.end()) return Result::failure("no source called '" + id + "'");
                sc.sources.erase(it);
                return Result::success("removed " + id);
            }));
    reg.add({"capture.scene_info", "Scene Info", "Describe a scene: canvas size, frame rate and every source from bottom to top.",
             {docParam()}, false, true},
            sceneCommand(session, [](Scene& sc, const Params& p) {
                std::string text = stringParam(p, "doc") + ": " + sc.summary() + ", background " + sc.background;
                for (const auto& s : sc.sources) text += "\n  " + describe(s);
                if (sc.sources.empty()) text += "\n  no sources yet (capture.add_source adds one)";
                Result r = Result::success(text);
                return r.with("sources", static_cast<std::int64_t>(sc.sources.size())).with("width", static_cast<std::int64_t>(sc.width))
                    .with("height", static_cast<std::int64_t>(sc.height));
            }, false));
    reg.add({"capture.preview_scene", "Preview Scene",
             "Draw the scene as a PNG and attach it. Live sources show as labelled boxes, or with live=true as a "
             "screenshot / webcam frame grabbed right now (needs ffmpeg and a screen).",
             {docParam(), {"live", ParamType::Bool, "grab the screen and webcam for real", false, {}, false},
              {"width", ParamType::Int, "preview width in px", false, {}, std::int64_t{960}}},
             false, true},
            sceneCommand(session, [](Scene& sc, const Params& p) {
                std::map<std::string, std::string> frames;
                fs::path dir;
                std::string notes;
                if (boolParam(p, "live", false)) {
                    dir = freshDir();
                    Environment env = detect();
                    for (const auto& s : sc.sources) {
                        if (!s.visible || !s.live()) continue;
                        const std::string out = (dir / (s.id + ".png")).string();
                        Plan plan;
                        if (s.kind == "webcam") {
                            WebcamRequest w;
                            w.output = out;
                            w.device = s.device;
                            plan = planWebcam(env, w);
                        } else {
                            ScreenshotRequest q;
                            q.output = out;
                            q.monitor = s.monitor;
                            q.region = s.region;
                            q.cursor = s.cursor;
                            plan = planScreenshot(env, q);
                        }
                        if (!plan.ok()) {
                            notes += "; " + s.id + ": " + plan.error;
                            continue;
                        }
                        auto exe = findProgram(plan.args[0]);
                        if (!exe) continue;
                        plan.args[0] = exe->string();
                        if (runProgram(plan.args).exitCode == 0) frames[s.id] = out;
                        else notes += "; " + s.id + " couldn't be grabbed";
                    }
                }
                const int width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 960), 32, sc.width));
                std::string png;
                try {
                    png = renderScene(sc, frames, width);
                } catch (...) {
                    std::error_code ec;
                    if (!dir.empty()) fs::remove_all(dir, ec);
                    throw;
                }
                std::error_code ec;
                if (!dir.empty()) fs::remove_all(dir, ec);
                Result r = Result::success("scene " + stringParam(p, "doc") + notes);
                r.attach({"image/png", png, stringParam(p, "doc") + ".png"});
                return r;
            }, false));
}

}  // namespace

void registerSceneCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"capture.scene_new", "New Scene",
             "Make a scene, like in OBS: a canvas to stack sources on (capture.add_source: the screen, a window, a "
             "webcam, images, text, colour boxes), then record it with capture.record_scene or "
             "capture.start_recording. Returns its id, e.g. scene1.",
             {{"name", ParamType::String, "what to call it", false, {}, std::string("Scene")},
              {"width", ParamType::Int, "canvas width (even)", false, {}, std::int64_t{1920}},
              {"height", ParamType::Int, "canvas height (even)", false, {}, std::int64_t{1080}},
              {"fps", ParamType::Int, "frames per second", false, {}, std::int64_t{30}},
              {"background", ParamType::String, "colour behind the sources", false, {}, std::string("black")}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    auto sc = std::make_shared<Scene>();
                    sc->name = stringParam(p, "name", "Scene");
                    sc->width = static_cast<int>(intParam(p, "width", 1920));
                    sc->height = static_cast<int>(intParam(p, "height", 1080));
                    sc->fps = static_cast<int>(intParam(p, "fps", 30));
                    sc->background = stringParam(p, "background", "black");
                    if (sc->width < 16 || sc->height < 16 || sc->width > 8192 || sc->height > 8192 || sc->width % 2 || sc->height % 2) {
                        return Result::failure("width and height must be even, 16..8192");
                    }
                    if (sc->fps < 1 || sc->fps > 120) return Result::failure("fps must be 1..120");
                    if (!raster::Color::parse(sc->background)) return Result::failure("background needs a colour");
                    const std::string id = session.workspace().add(sc, "scene");
                    Result r = Result::success("created " + id + " (" + sc->summary() + ")", id);
                    return r.with("doc", id);
                });
            });
    reg.add({"capture.scene_open", "Open Scene", "Open a .wwscene file saved by capture.scene_save.",
             {{"path", ParamType::String, ".wwscene file", true}}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    const fs::path path = stringParam(p, "path");
                    std::error_code ec;
                    if (!fs::is_regular_file(path, ec)) return Result::failure("file not found: " + path.string());
                    try {
                        auto sc = sceneFromJson(readAll(path), fs::absolute(path, ec).parent_path());
                        sc->path = path.string();
                        const std::string id = session.workspace().add(sc, "scene");
                        Result r = Result::success("opened " + id + " (" + sc->summary() + ")", id);
                        return r.with("doc", id);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
    reg.add({"capture.scene_save", "Save Scene", "Save a scene as a .wwscene file (JSON) to open again later.",
             {docParam(), {"path", ParamType::String, "where to save, e.g. stream.wwscene", true}}, false},
            sceneCommand(session, [](Scene& sc, const Params& p) {
                const fs::path path = stringParam(p, "path");
                if (path.extension() != ".wwscene") return Result::failure("scenes save as .wwscene");
                std::error_code ec;
                if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
                std::ofstream f(path, std::ios::binary);
                f << toJson(sc);
                if (!f) return Result::failure("can't write " + path.string());
                sc.path = path.string();
                return Result::success("saved " + path.string(), path.string());
            }, false));
    registerSourceCommands(session);

    {
        std::vector<ParamSpec> s = {docParam()};
        for (auto& x : recordSpecs(true)) s.push_back(x);
        reg.add({"capture.record_scene", "Record Scene",
                 "Record a scene for a number of seconds: every visible source composited in its place, in one "
                 "ffmpeg run. Blocks until done (capture.start_recording records until you stop it).",
                 s, false},
                sceneCommand(session, [](Scene& sc, const Params& p) {
                    Environment env = detect(stringParam(p, "display"));
                    SceneRecordRequest rq;
                    rq.output = stringParam(p, "output");
                    rq.duration = numberParam(p, "duration", 10);
                    rq.quality = stringParam(p, "quality", "good");
                    rq.audio = boolParam(p, "audio", false);
                    rq.audioDevice = stringParam(p, "audio_device");
                    const fs::path dir = freshDir();
                    Plan plan = planSceneRecord(env, sc, rq, drawStills(sc, dir));
                    std::error_code ec;
                    if (!plan.ok() || !prepareOutput(rq.output, p).empty()) {
                        fs::remove_all(dir, ec);
                        return Result::failure(plan.ok() ? prepareOutput(rq.output, p) : plan.error);
                    }
                    auto exe = findProgram("ffmpeg");
                    plan.args[0] = exe ? exe->string() : "ffmpeg";
                    ProcessResult pr = runProgram(plan.args);
                    fs::remove_all(dir, ec);
                    const auto bytes = static_cast<std::int64_t>(fs::file_size(rq.output, ec));
                    if (pr.exitCode != 0 || ec || bytes == 0) {
                        std::string out = pr.output;
                        if (out.size() > 1000) out = "..." + out.substr(out.size() - 1000);
                        return Result::failure("recording failed: " + out);
                    }
                    Result r = Result::success("recorded " + num(rq.duration) + "s of " + sc.name + " to " + rq.output, rq.output);
                    return r.with("output", rq.output).with("bytes", bytes).with("duration", rq.duration).with("backend", plan.backend);
                }, false));
    }
    {
        std::vector<ParamSpec> s = {{"doc", ParamType::String, "the scene to record; leave out to record the screen", false},
                                    {"monitor", ParamType::Int, "without a scene: which screen", false},
                                    {"fps", ParamType::Int, "without a scene: frames per second", false, {}, std::int64_t{30}}};
        for (auto& x : recordSpecs(false)) s.push_back(x);
        reg.add({"capture.start_recording", "Start Recording",
                 "Start recording a scene (or the screen) in the background, like OBS's Start Recording; returns at "
                 "once with a recording id. capture.stop_recording ends it and finishes the file.",
                 s, false},
                [&session](const Params& p) -> std::unique_ptr<Action> {
                    return std::make_unique<FunctionAction>([&session, p] {
                        try {
                            Environment env = detect(stringParam(p, "display"));
                            const std::string output = stringParam(p, "output");
                            if (std::string err = prepareOutput(output, p); !err.empty()) return Result::failure(err);
                            if (has(p, "doc")) {
                                auto sc = session.workspace().getAs<Scene>(stringParam(p, "doc"));
                                if (!sc) return Result::failure("no open scene called '" + stringParam(p, "doc") + "'");
                                SceneRecordRequest rq;
                                rq.output = output;
                                rq.quality = stringParam(p, "quality", "good");
                                rq.audio = boolParam(p, "audio", false);
                                rq.audioDevice = stringParam(p, "audio_device");
                                rq.forStop = true;
                                const fs::path dir = freshDir();
                                Plan plan = planSceneRecord(env, *sc, rq, drawStills(*sc, dir));
                                if (!plan.ok()) {
                                    std::error_code ec;
                                    fs::remove_all(dir, ec);
                                    return Result::failure(plan.error);
                                }
                                return startProcess(plan, output, sc->name, dir);
                            }
                            RecordRequest rq;
                            rq.output = output;
                            rq.duration = 4 * 3600;
                            rq.fps = static_cast<int>(intParam(p, "fps", 30));
                            rq.monitor = has(p, "monitor") ? static_cast<int>(intParam(p, "monitor")) : -1;
                            rq.audio = boolParam(p, "audio", false);
                            rq.audioDevice = stringParam(p, "audio_device");
                            rq.quality = stringParam(p, "quality", "good");
                            Plan plan = planRecord(env, rq);
                            if (!plan.ok()) return Result::failure(plan.error);
                            plan.args.erase(std::remove(plan.args.begin(), plan.args.end(), "-nostdin"), plan.args.end());
                            return startProcess(plan, output, "the screen", {});
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                    });
                });
    }
    reg.add({"capture.stop_recording", "Stop Recording", "Stop a recording started with capture.start_recording (or all of them) and finish its file.",
             {{"recording", ParamType::String, "recording id, e.g. rec1; leave out to stop every one", false}}, false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    std::map<std::string, Recording> stopping;
                    {
                        std::lock_guard<std::mutex> g(recLock());
                        if (has(p, "recording")) {
                            auto it = recordings().find(stringParam(p, "recording"));
                            if (it == recordings().end()) return Result::failure("no recording called '" + stringParam(p, "recording") + "'");
                            stopping.insert(recordings().extract(it));
                        } else {
                            stopping.swap(recordings());
                        }
                    }
                    if (stopping.empty()) return Result::failure("nothing is recording");
                    Result last;
                    std::string text;
                    for (auto& [id, rec] : stopping) {
                        last = stopOne(id, rec);
                        text += (text.empty() ? "" : "\n") + last.message;
                        if (!last.ok) return last;
                    }
                    last.message = text;
                    return last;
                });
            });
    reg.add({"capture.recordings", "Recordings", "List the recordings running in the background.", {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::lock_guard<std::mutex> g(recLock());
                    std::string text;
                    for (auto& [id, rec] : recordings()) {
                        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - rec.started).count();
                        text += (text.empty() ? "" : "\n") + id + ": " + rec.what + " -> " + rec.output + ", " + num(std::round(secs)) + "s" +
                                (rec.proc->running() ? "" : " (ended)");
                    }
                    Result r = Result::success(text.empty() ? "nothing is recording" : text);
                    return r.with("count", static_cast<std::int64_t>(recordings().size()));
                });
            });
}

}  // namespace wetwaresuite::capture
