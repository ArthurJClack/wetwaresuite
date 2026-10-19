// capture.* commands: plan with the pure functions, then run ffmpeg/grim.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>

#include "wetwaresuite/capture/capture.hpp"
#include "wetwaresuite/capture/scene.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/raster/codec.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace wetwaresuite::capture {

namespace fs = std::filesystem;

namespace {

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

#ifndef _WIN32
std::string envVar(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
#endif

#ifdef _WIN32
BOOL CALLBACK addMonitor(HMONITOR handle, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<Monitor>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return TRUE;
    Monitor m;
    m.index = static_cast<int>(list->size());
    std::wstring w = info.szDevice;
    for (wchar_t c : w) m.name += c < 128 ? static_cast<char>(c) : '?';
    m.x = info.rcMonitor.left;
    m.y = info.rcMonitor.top;
    m.width = info.rcMonitor.right - info.rcMonitor.left;
    m.height = info.rcMonitor.bottom - info.rcMonitor.top;
    m.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(m);
    return TRUE;
}
#endif

std::string runError(const ProcessResult& r, const std::string& what) {
    std::string out = r.output;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (out.size() > 1200) out = "..." + out.substr(out.size() - 1200);
    if (r.exitCode < 0) return "couldn't start " + what;
    return what + " failed (exit " + std::to_string(r.exitCode) + ")" + (out.empty() ? "" : ": " + out);
}

// Runs a plan (args[0] is a program name); returns an error, empty on success.
std::string execute(Plan plan) {
    auto exe = findProgram(plan.args[0]);
    if (!exe) return plan.args[0] + " isn't installed";
    const std::string name = plan.args[0];
    plan.args[0] = exe->string();
    ProcessResult r = runProgram(plan.args);
    if (r.exitCode != 0) {
        std::string err = runError(r, name);
        if (plan.backend == "x11grab" && (err.find("annot open display") != std::string::npos || err.find("Cannot open") != std::string::npos ||
                                          err.find("Input/output error") != std::string::npos)) {
            err += "\n(the X display couldn't be opened; check DISPLAY or pass display=:0)";
        }
        const bool sound = std::find(plan.args.begin(), plan.args.end(), "pulse") != plan.args.end() ||
                           std::any_of(plan.args.begin(), plan.args.end(), [](const std::string& a) { return a.rfind("audio=", 0) == 0; });
        if (sound && err.find("rror opening input") != std::string::npos) {
            err += "\n(the sound input couldn't be opened: is PulseAudio/PipeWire running? capture.list_devices lists inputs; "
                   "audio=false records without sound)";
        }
        return err;
    }
    return {};
}

fs::path tempFile(const std::string& ext) {
    std::random_device rd;
    return fs::temp_directory_path() / ("wetwaresuite-capture-" + std::to_string(rd()) + std::to_string(rd()) + "." + ext);
}

std::string checkOutput(const std::string& output, const Params& p) {
    std::error_code ec;
    if (fs::exists(output, ec) && !boolParam(p, "overwrite", true)) return output + " already exists (pass overwrite=true)";
    fs::path parent = fs::path(output).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    return {};
}

Region regionOf(const Params& p) {
    Region r;
    r.x = static_cast<int>(intParam(p, "x", 0));
    r.y = static_cast<int>(intParam(p, "y", 0));
    r.width = static_cast<int>(intParam(p, "width", 0));
    r.height = static_cast<int>(intParam(p, "height", 0));
    return r;
}

std::vector<ParamSpec> regionParams() {
    return {{"monitor", ParamType::Int, "which screen (0, 1, ...; capture.list_displays shows them); leave out for all of them", false},
            {"x", ParamType::Int, "region left edge in px (relative to the monitor when one is given)", false},
            {"y", ParamType::Int, "region top edge in px", false},
            {"width", ParamType::Int, "region width in px (leave out for the whole screen)", false},
            {"height", ParamType::Int, "region height in px", false},
            {"display", ParamType::String, "Linux: X display to use instead of $DISPLAY, e.g. :0 or :99", false}};
}

ParamSpec backendParam(bool still) {
    std::vector<std::string> choices = {"auto", "x11grab", "gdigrab", "ddagrab", "avfoundation"};
    if (still) choices.insert(choices.begin() + 2, "grim");
    return {"backend", ParamType::String, "capture method; auto picks x11grab or grim (Linux), gdigrab (Windows), avfoundation (macOS)",
            false, choices, std::string("auto")};
}

// The first audio/video device name the OS lists, for Windows and macOS defaults.
std::string firstDevice(Platform platform, bool audio) {
    auto args = listDevicesArgs(platform);
    if (args.empty()) return {};
    auto exe = findProgram("ffmpeg");
    if (!exe) return {};
    args[0] = exe->string();
    DeviceList d = parseDeviceListing(platform, runProgram(args).output);
    const auto& list = audio ? d.audio : d.video;
    if (list.empty()) return {};
    return platform == Platform::Mac ? std::string("0") : list.front();
}

Result imageResult(const std::string& message, const std::string& output, const Plan& plan, const Params& p, bool attach) {
    Result r = Result::success(message, output);
    try {
        raster::Image img = raster::load(output);
        r.with("width", static_cast<std::int64_t>(img.width())).with("height", static_cast<std::int64_t>(img.height()));
        if (attach) {
            const int maxSize = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "max_size", 1280), 64, 4096));
            r.attach({"image/png", raster::previewPng(img, maxSize), fs::path(output).filename().string()});
        }
    } catch (const std::exception& e) {
        r.message += " (couldn't read it back: " + std::string(e.what()) + ")";
    }
    return r.with("backend", plan.backend);
}

}  // namespace

Environment detect(const std::string& displayOverride) {
    Environment env;
    env.platform = currentPlatform();
    env.hasFfmpeg = findProgram("ffmpeg").has_value();
#ifdef _WIN32
    EnumDisplayMonitors(nullptr, nullptr, addMonitor, reinterpret_cast<LPARAM>(&env.monitors));
#else
    env.display = displayOverride.empty() ? envVar("DISPLAY") : displayOverride;
    if (displayOverride.empty()) env.waylandDisplay = envVar("WAYLAND_DISPLAY");
    if (env.platform == Platform::Linux) {
        env.hasGrim = findProgram("grim").has_value();
        if (!env.display.empty()) {
            if (auto xrandr = findProgram("xrandr")) {
                ProcessResult r = runProgram({xrandr->string(), "--display", env.display, "--listmonitors"});
                if (r.exitCode == 0) env.monitors = parseXrandrMonitors(r.output);
            }
        }
    }
#endif
    (void)displayOverride;
    return env;
}

void registerCommands(Session& session) {
    auto& reg = session.registry();

    {
        std::vector<ParamSpec> s = {{"output", ParamType::String, ".png or .jpg to write; leave out to only attach the image (how Claude looks at the screen)", false}};
        for (auto& x : regionParams()) s.push_back(x);
        s.push_back({"delay", ParamType::Number, "wait this many seconds first (to open a menu, say)", false, {}, 0.0});
        s.push_back({"cursor", ParamType::Bool, "include the mouse pointer", false, {}, false});
        s.push_back(backendParam(true));
        s.push_back({"preview", ParamType::Bool, "attach the screenshot (always on when there's no output)", false, {}, false});
        s.push_back({"max_size", ParamType::Int, "longest side of the attached image in px", false, {}, std::int64_t{1280}});
        s.push_back({"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, true});
        reg.add({"capture.screenshot", "Screenshot",
                 "Take a screenshot of every screen, one monitor, or a region (x, y, width, height), optionally after a "
                 "delay. Without output the picture is only attached, which is how Claude can see the screen. Uses "
                 "ffmpeg (x11grab, gdigrab, avfoundation) or grim on Wayland. Fails with a clear message on machines "
                 "without a display.",
                 s, false},
                [](const Params& p) -> std::unique_ptr<Action> {
                    return std::make_unique<FunctionAction>([p] {
                        Environment env = detect(stringParam(p, "display"));
                        ScreenshotRequest rq;
                        const bool keep = has(p, "output");
                        const fs::path temp = tempFile("png");
                        rq.output = keep ? stringParam(p, "output") : temp.string();
                        rq.monitor = has(p, "monitor") ? static_cast<int>(intParam(p, "monitor")) : -1;
                        rq.region = regionOf(p);
                        rq.cursor = boolParam(p, "cursor", false);
                        rq.backend = stringParam(p, "backend", "auto");
                        Plan plan = planScreenshot(env, rq);
                        if (!plan.ok()) return Result::failure(plan.error);
                        if (keep) {
                            if (std::string err = checkOutput(rq.output, p); !err.empty()) return Result::failure(err);
                        }
                        const double delay = std::clamp(numberParam(p, "delay", 0), 0.0, 600.0);
                        if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(delay * 1000)));
                        if (std::string err = execute(plan); !err.empty()) return Result::failure(err);
                        std::error_code ec;
                        if (!fs::is_regular_file(rq.output, ec)) return Result::failure(plan.backend + " finished but wrote no image");
                        Result r = imageResult(keep ? "saved a screenshot to " + rq.output : "took a screenshot", rq.output, plan, p,
                                               !keep || boolParam(p, "preview", false));
                        if (keep) {
                            r.with("output", rq.output);
                        } else {
                            fs::remove(temp, ec);
                            r.value = {};
                            for (auto& at : r.attachments) at.name = "screenshot.png";
                        }
                        return r;
                    });
                });
    }

    {
        std::vector<ParamSpec> s = {{"output", ParamType::String, ".mp4, .mkv, .mov, .webm or .gif to write", true},
                                    {"duration", ParamType::Number, "seconds to record", false, {}, 10.0},
                                    {"fps", ParamType::Int, "frames per second", false, {}, std::int64_t{30}}};
        for (auto& x : regionParams()) s.push_back(x);
        s.push_back({"audio", ParamType::Bool, "record sound from a microphone/input too (not for .gif)", false, {}, false});
        s.push_back({"audio_device", ParamType::String, "sound input: a PulseAudio source on Linux (default: default), a device name on Windows, an index on macOS (capture.list_devices)", false});
        s.push_back({"cursor", ParamType::Bool, "include the mouse pointer", false, {}, true});
        s.push_back({"quality", ParamType::String, "encoding preset", false, {"draft", "good", "best"}, std::string("good")});
        s.push_back(backendParam(false));
        s.push_back({"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, true});
        reg.add({"capture.record", "Record Screen",
                 "Record the screen (or a monitor or region) for a number of seconds to a video or GIF, optionally with "
                 "sound from a microphone. Blocks until the recording is done. Uses ffmpeg's x11grab (Linux/X11 and "
                 "XWayland), gdigrab (Windows) or avfoundation (macOS).",
                 s, false},
                [](const Params& p) -> std::unique_ptr<Action> {
                    return std::make_unique<FunctionAction>([p] {
                        Environment env = detect(stringParam(p, "display"));
                        RecordRequest rq;
                        rq.output = stringParam(p, "output");
                        rq.duration = numberParam(p, "duration", 10);
                        rq.fps = static_cast<int>(intParam(p, "fps", 30));
                        rq.monitor = has(p, "monitor") ? static_cast<int>(intParam(p, "monitor")) : -1;
                        rq.region = regionOf(p);
                        rq.audio = boolParam(p, "audio", false);
                        rq.audioDevice = stringParam(p, "audio_device");
                        rq.cursor = boolParam(p, "cursor", true);
                        rq.quality = stringParam(p, "quality", "good");
                        rq.backend = stringParam(p, "backend", "auto");
                        if (rq.audio && rq.audioDevice.empty() && env.platform == Platform::Windows && env.hasFfmpeg) {
                            rq.audioDevice = firstDevice(env.platform, true);
                            if (rq.audioDevice.empty()) return Result::failure("no microphone found (capture.list_devices lists inputs)");
                        }
                        Plan plan = planRecord(env, rq);
                        if (!plan.ok()) return Result::failure(plan.error);
                        if (std::string err = checkOutput(rq.output, p); !err.empty()) return Result::failure(err);
                        if (std::string err = execute(plan); !err.empty()) return Result::failure(err);
                        std::error_code ec;
                        const auto bytes = static_cast<std::int64_t>(fs::file_size(rq.output, ec));
                        if (ec || bytes == 0) return Result::failure("ffmpeg finished but wrote nothing to " + rq.output);
                        Result r = Result::success("recorded " + std::to_string(static_cast<int>(rq.duration + 0.5)) + "s of the screen to " +
                                                   rq.output + (rq.audio && fs::path(rq.output).extension() != ".gif" ? " with sound" : ""),
                                                   rq.output);
                        return r.with("output", rq.output).with("bytes", bytes).with("duration", rq.duration).with("backend", plan.backend);
                    });
                });
    }

    reg.add({"capture.webcam", "Webcam Snapshot",
             "Take one picture with a webcam (v4l2 on Linux, DirectShow on Windows, avfoundation on macOS).",
             {{"output", ParamType::String, ".png or .jpg to write", true},
              {"device", ParamType::String, "camera: /dev/video0 (Linux), a name (Windows), an index (macOS); default the first", false},
              {"width", ParamType::Int, "ask the camera for this width", false},
              {"height", ParamType::Int, "ask the camera for this height", false},
              {"warmup", ParamType::Number, "seconds to let the camera adjust exposure first", false, {}, 0.5},
              {"preview", ParamType::Bool, "attach the picture", false, {}, false},
              {"max_size", ParamType::Int, "longest side of the attached image in px", false, {}, std::int64_t{1280}},
              {"overwrite", ParamType::Bool, "replace the output if it exists", false, {}, true}},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    Environment env = detect();
                    WebcamRequest rq;
                    rq.output = stringParam(p, "output");
                    rq.device = stringParam(p, "device");
                    rq.width = static_cast<int>(intParam(p, "width", 0));
                    rq.height = static_cast<int>(intParam(p, "height", 0));
                    rq.warmup = std::clamp(numberParam(p, "warmup", 0.5), 0.0, 10.0);
                    std::error_code ec;
                    if (env.platform == Platform::Linux && rq.device.empty() && !fs::exists("/dev/video0", ec)) {
                        return Result::failure("no webcam found (/dev/video0 doesn't exist; capture.list_devices lists cameras)");
                    }
                    if (env.platform == Platform::Windows && rq.device.empty() && env.hasFfmpeg) {
                        rq.device = firstDevice(env.platform, false);
                        if (rq.device.empty()) return Result::failure("no webcam found (capture.list_devices lists cameras)");
                    }
                    Plan plan = planWebcam(env, rq);
                    if (!plan.ok()) return Result::failure(plan.error);
                    if (std::string err = checkOutput(rq.output, p); !err.empty()) return Result::failure(err);
                    if (std::string err = execute(plan); !err.empty()) return Result::failure(err);
                    Result r = imageResult("saved a webcam picture to " + rq.output, rq.output, plan, p, boolParam(p, "preview", false));
                    return r.with("output", rq.output);
                });
            });

    reg.add({"capture.list_displays", "List Displays",
             "Show what can be captured here: the platform, the capture method that would be used, the display, and "
             "each monitor with its position and size (use the index as monitor=).",
             {{"display", ParamType::String, "Linux: X display to look at instead of $DISPLAY", false}}, false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    Environment env = detect(stringParam(p, "display"));
                    std::string text = std::string("platform: ") + platformName(env.platform) + "\nffmpeg: " +
                                       (env.hasFfmpeg ? "installed" : "not installed (needed for capture)");
                    if (env.platform == Platform::Linux) {
                        text += "\nDISPLAY: " + (env.display.empty() ? std::string("not set") : env.display) +
                                "\nWAYLAND_DISPLAY: " + (env.waylandDisplay.empty() ? std::string("not set") : env.waylandDisplay) +
                                "\ngrim: " + (env.hasGrim ? "installed" : "not installed");
                    }
                    std::vector<Monitor> monitors = env.monitors;
                    if (env.platform == Platform::Mac && env.hasFfmpeg) {
                        auto args = listDevicesArgs(env.platform);
                        args[0] = findProgram("ffmpeg")->string();
                        DeviceList d = parseDeviceListing(env.platform, runProgram(args).output);
                        for (std::size_t i = 0; i < d.screens.size(); ++i) monitors.push_back({static_cast<int>(i), d.screens[i]});
                    }
                    // without xrandr, measure the whole X screen by grabbing one frame
                    if (monitors.empty() && env.platform == Platform::Linux && !env.display.empty() && env.hasFfmpeg) {
                        const fs::path tmp = tempFile("png");
                        ScreenshotRequest rq;
                        rq.output = tmp.string();
                        rq.backend = "x11grab";
                        Plan plan = planScreenshot(env, rq);
                        if (plan.ok() && execute(plan).empty()) {
                            try {
                                raster::Image img = raster::load(tmp);
                                monitors.push_back({0, "screen", 0, 0, img.width(), img.height(), true});
                            } catch (const std::exception&) {
                            }
                        }
                        std::error_code ec;
                        fs::remove(tmp, ec);
                    }
                    std::string err;
                    std::string backend;
                    {
                        ScreenshotRequest probe;
                        probe.output = "x.png";
                        Plan plan = planScreenshot(env, probe);
                        backend = plan.ok() ? plan.backend : std::string();
                        err = plan.error;
                    }
                    text += "\nscreenshot method: " + (backend.empty() ? "none (" + err + ")" : backend);
                    text += "\nmonitors:";
                    if (monitors.empty()) text += " unknown";
                    for (const auto& m : monitors) {
                        text += "\n  " + std::to_string(m.index) + ": " + m.name;
                        if (m.width > 0) text += " " + std::to_string(m.width) + "x" + std::to_string(m.height) + " at " +
                                                 std::to_string(m.x) + "," + std::to_string(m.y);
                        if (m.primary) text += " (primary)";
                    }
                    Result r = backend.empty() ? Result::failure(text) : Result::success(text);
                    r.with("platform", std::string(platformName(env.platform)))
                        .with("backend", backend)
                        .with("monitors", static_cast<std::int64_t>(monitors.size()));
                    if (!monitors.empty() && monitors.front().width > 0) {
                        r.with("width", static_cast<std::int64_t>(monitors.front().width))
                            .with("height", static_cast<std::int64_t>(monitors.front().height));
                    }
                    return r;
                });
            });

    reg.add({"capture.list_devices", "List Devices",
             "List cameras and sound inputs that capture.webcam and capture.record (audio_device) can use.", {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    const Platform platform = currentPlatform();
                    DeviceList d;
                    if (platform == Platform::Linux) {
                        std::error_code ec;
                        if (fs::is_directory("/dev", ec)) {
                            for (const auto& e : fs::directory_iterator("/dev", ec)) {
                                const std::string n = e.path().filename().string();
                                if (n.rfind("video", 0) == 0) d.video.push_back(e.path().string());
                            }
                        }
                        std::sort(d.video.begin(), d.video.end());
                        d.audio.push_back("default");
                        if (auto pactl = findProgram("pactl")) {
                            ProcessResult r = runProgram({pactl->string(), "list", "short", "sources"});
                            std::size_t start = 0;
                            while (r.exitCode == 0 && start < r.output.size()) {
                                std::size_t end = r.output.find('\n', start);
                                if (end == std::string::npos) end = r.output.size();
                                const std::string line = r.output.substr(start, end - start);
                                start = end + 1;
                                auto a = line.find('\t');
                                if (a == std::string::npos) continue;
                                auto b = line.find('\t', a + 1);
                                d.audio.push_back(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
                            }
                        }
                    } else {
                        auto exe = findProgram("ffmpeg");
                        if (!exe) return Result::failure("listing devices needs ffmpeg; install it (it's free)");
                        auto args = listDevicesArgs(platform);
                        args[0] = exe->string();
                        d = parseDeviceListing(platform, runProgram(args).output);
                    }
                    std::string text = "cameras:";
                    if (d.video.empty()) text += " none found";
                    for (std::size_t i = 0; i < d.video.size(); ++i) {
                        text += "\n  " + (platform == Platform::Mac ? std::to_string(i) + ": " : std::string()) + d.video[i];
                    }
                    text += "\nsound inputs:";
                    if (d.audio.empty()) text += " none found";
                    for (std::size_t i = 0; i < d.audio.size(); ++i) {
                        text += "\n  " + (platform == Platform::Mac ? std::to_string(i) + ": " : std::string()) + d.audio[i];
                    }
                    if (!d.screens.empty()) {
                        text += "\nscreens:";
                        for (const auto& s : d.screens) text += "\n  " + s;
                    }
                    Result r = Result::success(text);
                    return r.with("cameras", static_cast<std::int64_t>(d.video.size())).with("audio_inputs", static_cast<std::int64_t>(d.audio.size()));
                });
            });
    registerSceneCommands(session);
}

}  // namespace wetwaresuite::capture
