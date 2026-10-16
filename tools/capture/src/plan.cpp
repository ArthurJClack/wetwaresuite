// Pure planning: which program and arguments capture what was asked, per OS.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <regex>

#include "achclwss/capture/capture.hpp"

namespace achclwss::capture {

namespace {

std::vector<std::string> head() { return {"ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-y"}; }

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    return lower(e);
}

std::string size(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

std::string fixed(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// Region in absolute desktop coordinates (relative to the monitor for ddagrab and avfoundation).
bool resolve(const Environment& env, int monitor, const Region& region, bool relative, Region& out, std::string& error) {
    out = region;
    if (region.width < 0 || region.height < 0) {
        error = "width and height can't be negative";
        return false;
    }
    if ((region.width > 0) != (region.height > 0)) {
        error = "a region needs both width and height";
        return false;
    }
    if (monitor < 0) return true;
    const Monitor* m = nullptr;
    for (const auto& x : env.monitors) {
        if (x.index == monitor) m = &x;
    }
    if (!m) {
        if (relative || (monitor == 0 && env.monitors.empty())) return true;  // the device picks the monitor itself
        error = "there's no monitor " + std::to_string(monitor) + (env.monitors.empty() ? " (the monitor layout is unknown here; leave monitor out to capture the whole screen)" : " (capture.list_displays shows the screens)");
        return false;
    }
    if (region.set()) {
        if (m->width > 0 && (region.x < 0 || region.y < 0 || region.x + region.width > m->width || region.y + region.height > m->height)) {
            error = "the region goes outside monitor " + std::to_string(monitor) + " (" + size(m->width, m->height) + ")";
            return false;
        }
        if (!relative) {
            out.x += m->x;
            out.y += m->y;
        }
    } else if (!relative) {
        out = {m->x, m->y, m->width, m->height};
    }
    return true;
}

std::string pickBackend(const Environment& env, const std::string& asked, bool still, std::string& error) {
    if (asked != "auto" && !asked.empty()) {
        const bool fits = (env.platform == Platform::Linux && (asked == "x11grab" || asked == "grim")) ||
                          (env.platform == Platform::Windows && (asked == "gdigrab" || asked == "ddagrab")) ||
                          (env.platform == Platform::Mac && asked == "avfoundation");
        if (!fits) {
            error = "backend " + asked + " isn't available on " + platformName(env.platform);
            return {};
        }
        if (asked == "grim" && !still) {
            error = "grim only takes screenshots; recording on Wayland needs XWayland (x11grab) or a tool like wf-recorder";
            return {};
        }
        if (asked == "grim" && !env.hasGrim) {
            error = "grim isn't installed (it takes screenshots on Wayland: `sudo apt install grim`)";
            return {};
        }
        if (asked == "x11grab" && env.display.empty()) {
            error = noDisplayMessage();
            return {};
        }
        return asked;
    }
    switch (env.platform) {
        case Platform::Windows: return "gdigrab";
        case Platform::Mac: return "avfoundation";
        case Platform::Linux: break;
    }
    const bool wayland = !env.waylandDisplay.empty();
    if (wayland && still && env.hasGrim) return "grim";
    if (!env.display.empty()) return "x11grab";
    if (wayland) {
        error = still ? "this is a Wayland session without XWayland; install grim for screenshots (`sudo apt install grim`)"
                      : "screen recording on Wayland needs XWayland (DISPLAY) for ffmpeg's x11grab; or record with wf-recorder";
        return {};
    }
    error = noDisplayMessage();
    return {};
}

// The input half of an ffmpeg command line: the screen as a video source. audioIndex is used on macOS.
bool screenInput(const Environment& env, const std::string& backend, int monitor, const Region& asked, bool cursor, int fps,
                 double duration, bool audio, const std::string& audioDevice, std::vector<std::string>& args, std::vector<std::string>& filters,
                 std::string& error) {
    Region r;
    const bool relative = backend == "ddagrab" || backend == "avfoundation";
    if (!resolve(env, monitor, asked, relative, r, error)) return false;
    const std::string mouse = cursor ? "1" : "0";
    // an input time limit, so filters that need the end of the stream (GIF palettes) get it
    std::vector<std::string> limit;
    if (duration > 0) limit = {"-t", fixed(duration)};
    if (backend == "x11grab") {
        if (env.display.empty()) {
            error = noDisplayMessage();
            return false;
        }
        args.insert(args.end(), {"-f", "x11grab", "-draw_mouse", mouse, "-framerate", std::to_string(fps)});
        if (audio) args.insert(args.end(), {"-thread_queue_size", "1024"});
        if (r.set()) args.insert(args.end(), {"-video_size", size(r.width, r.height)});
        args.insert(args.end(), limit.begin(), limit.end());
        args.insert(args.end(), {"-i", env.display + (r.set() ? "+" + std::to_string(r.x) + "," + std::to_string(r.y) : "")});
    } else if (backend == "gdigrab") {
        args.insert(args.end(), {"-f", "gdigrab", "-draw_mouse", mouse, "-framerate", std::to_string(fps)});
        if (audio) args.insert(args.end(), {"-thread_queue_size", "1024"});
        if (r.set()) {
            args.insert(args.end(), {"-offset_x", std::to_string(r.x), "-offset_y", std::to_string(r.y), "-video_size", size(r.width, r.height)});
        }
        args.insert(args.end(), limit.begin(), limit.end());
        args.insert(args.end(), {"-i", "desktop"});
    } else if (backend == "ddagrab") {
        std::string src = "ddagrab=output_idx=" + std::to_string(std::max(0, monitor)) + ":draw_mouse=" + mouse +
                          ":framerate=" + std::to_string(fps);
        if (r.set()) {
            src += ":video_size=" + size(r.width, r.height) + ":offset_x=" + std::to_string(r.x) + ":offset_y=" + std::to_string(r.y);
        }
        args.insert(args.end(), {"-f", "lavfi"});
        args.insert(args.end(), limit.begin(), limit.end());
        args.insert(args.end(), {"-i", src});
        filters.push_back("hwdownload");
        filters.push_back("format=bgra");
    } else if (backend == "avfoundation") {
        const std::string screen = "Capture screen " + std::to_string(std::max(0, monitor));
        const std::string sound = audio ? (audioDevice.empty() ? std::string("0") : audioDevice) : std::string("none");
        args.insert(args.end(), {"-f", "avfoundation", "-capture_cursor", mouse, "-framerate", std::to_string(fps)});
        args.insert(args.end(), limit.begin(), limit.end());
        args.insert(args.end(), {"-i", screen + ":" + sound});
        if (r.set()) {
            filters.push_back("crop=" + std::to_string(r.width) + ":" + std::to_string(r.height) + ":" + std::to_string(r.x) + ":" +
                              std::to_string(r.y));
        }
    } else {
        error = "unknown capture backend " + backend;
        return false;
    }
    return true;
}

std::string joined(const std::vector<std::string>& f) {
    std::string s;
    for (const auto& x : f) s += (s.empty() ? "" : ",") + x;
    return s;
}

}  // namespace

const char* platformName(Platform p) {
    switch (p) {
        case Platform::Linux: return "Linux";
        case Platform::Windows: return "Windows";
        case Platform::Mac: return "macOS";
    }
    return "?";
}

Platform currentPlatform() {
#ifdef _WIN32
    return Platform::Windows;
#elif defined(__APPLE__)
    return Platform::Mac;
#else
    return Platform::Linux;
#endif
}

std::string noDisplayMessage() {
    return "there's no screen to capture here: DISPLAY and WAYLAND_DISPLAY aren't set, so this looks like a server, "
           "container or SSH session. Run it from a terminal inside your desktop session, or give a headless machine "
           "a virtual screen: `Xvfb :99 -screen 0 1920x1080x24 &` and pass display=:99";
}

Plan planScreenshot(const Environment& env, const ScreenshotRequest& rq) {
    Plan plan;
    const std::string ext = extOf(rq.output);
    if (ext != "png" && ext != "jpg" && ext != "jpeg") {
        plan.error = "screenshots save as .png or .jpg";
        return plan;
    }
    plan.backend = pickBackend(env, rq.backend, true, plan.error);
    if (!plan.ok()) return plan;
    if (plan.backend == "grim") {
        Region r;
        if (!resolve(env, rq.monitor, rq.region, false, r, plan.error)) return plan;
        plan.args = {"grim"};
        if (r.set()) {
            plan.args.insert(plan.args.end(), {"-g", std::to_string(r.x) + "," + std::to_string(r.y) + " " + size(r.width, r.height)});
        }
        if (rq.cursor) plan.args.push_back("-c");
        if (ext != "png") plan.args.insert(plan.args.end(), {"-t", "jpeg"});
        plan.args.push_back(rq.output);
        return plan;
    }
    if (!env.hasFfmpeg) {
        plan.error = "screenshots need ffmpeg; install it (it's free): Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`";
        return plan;
    }
    plan.args = head();
    std::vector<std::string> filters;
    if (!screenInput(env, plan.backend, rq.monitor, rq.region, rq.cursor, 30, 0, false, {}, plan.args, filters, plan.error)) return plan;
    if (!filters.empty()) plan.args.insert(plan.args.end(), {"-vf", joined(filters)});
    plan.args.insert(plan.args.end(), {"-frames:v", "1", "-update", "1"});
    if (ext != "png") plan.args.insert(plan.args.end(), {"-q:v", "2"});
    plan.args.push_back(rq.output);
    return plan;
}

Plan planRecord(const Environment& env, const RecordRequest& rq) {
    Plan plan;
    const std::string ext = extOf(rq.output);
    if (ext != "mp4" && ext != "mkv" && ext != "mov" && ext != "webm" && ext != "gif") {
        plan.error = "recordings save as .mp4, .mkv, .mov, .webm or .gif";
        return plan;
    }
    if (!(rq.duration > 0) || rq.duration > 4 * 3600) {
        plan.error = "duration must be more than 0 and at most 4 hours (in seconds)";
        return plan;
    }
    if (rq.fps < 1 || rq.fps > 120) {
        plan.error = "fps must be 1..120";
        return plan;
    }
    plan.backend = pickBackend(env, rq.backend, false, plan.error);
    if (!plan.ok()) return plan;
    if (!env.hasFfmpeg) {
        plan.error = "screen recording needs ffmpeg; install it (it's free): Debian/Ubuntu `sudo apt install ffmpeg`, macOS `brew install ffmpeg`, Windows `winget install ffmpeg`";
        return plan;
    }
    const bool gif = ext == "gif";
    const bool audio = rq.audio && !gif;
    if (audio && env.platform == Platform::Windows && rq.audioDevice.empty()) {
        plan.error = "recording sound on Windows needs audio_device (a name from capture.list_devices)";
        return plan;
    }
    plan.args = head();
    std::vector<std::string> filters;
    if (!screenInput(env, plan.backend, rq.monitor, rq.region, rq.cursor, rq.fps, rq.duration, audio, rq.audioDevice, plan.args, filters, plan.error)) {
        return plan;
    }
    if (audio && env.platform == Platform::Linux) {
        plan.args.insert(plan.args.end(), {"-thread_queue_size", "1024", "-f", "pulse", "-t", fixed(rq.duration), "-i", rq.audioDevice.empty() ? "default" : rq.audioDevice});
    } else if (audio && env.platform == Platform::Windows) {
        plan.args.insert(plan.args.end(), {"-thread_queue_size", "1024", "-f", "dshow", "-t", fixed(rq.duration), "-i", "audio=" + rq.audioDevice});
    }
    if (gif) {
        filters.push_back("fps=" + std::to_string(std::min(rq.fps, 15)));
        filters.push_back("scale='min(iw,960)':-1:flags=lanczos");
        plan.args.insert(plan.args.end(), {"-vf", joined(filters) + ",split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4"});
        plan.args.insert(plan.args.end(), {"-an", "-loop", "0"});
    } else {
        filters.push_back("scale=trunc(iw/2)*2:trunc(ih/2)*2");
        filters.push_back("format=yuv420p");
        plan.args.insert(plan.args.end(), {"-vf", joined(filters)});
        if (ext == "webm") {
            const std::string crf = rq.quality == "draft" ? "42" : rq.quality == "best" ? "28" : "35";
            plan.args.insert(plan.args.end(), {"-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-crf", crf, "-b:v", "0", "-row-mt", "1"});
            if (audio) plan.args.insert(plan.args.end(), {"-c:a", "libopus", "-b:a", "128k"});
        } else {
            const std::string preset = rq.quality == "draft" ? "ultrafast" : rq.quality == "best" ? "medium" : "veryfast";
            const std::string crf = rq.quality == "draft" ? "28" : rq.quality == "best" ? "18" : "23";
            plan.args.insert(plan.args.end(), {"-c:v", "libx264", "-preset", preset, "-crf", crf});
            if (audio) plan.args.insert(plan.args.end(), {"-c:a", "aac", "-b:a", "160k"});
            if (ext != "mkv") plan.args.insert(plan.args.end(), {"-movflags", "+faststart"});
        }
        if (!audio) plan.args.push_back("-an");
    }
    plan.args.insert(plan.args.end(), {"-t", fixed(rq.duration), rq.output});
    return plan;
}

Plan planWebcam(const Environment& env, const WebcamRequest& rq) {
    Plan plan;
    const std::string ext = extOf(rq.output);
    if (ext != "png" && ext != "jpg" && ext != "jpeg") {
        plan.error = "webcam snapshots save as .png or .jpg";
        return plan;
    }
    if (!env.hasFfmpeg) {
        plan.error = "webcam snapshots need ffmpeg; install it (it's free)";
        return plan;
    }
    if ((rq.width > 0) != (rq.height > 0)) {
        plan.error = "give both width and height, or neither";
        return plan;
    }
    plan.args = head();
    std::vector<std::string> sizeArgs;
    if (rq.width > 0) sizeArgs = {"-video_size", size(rq.width, rq.height)};
    switch (env.platform) {
        case Platform::Linux:
            plan.backend = "v4l2";
            plan.args.insert(plan.args.end(), {"-f", "v4l2"});
            plan.args.insert(plan.args.end(), sizeArgs.begin(), sizeArgs.end());
            plan.args.insert(plan.args.end(), {"-i", rq.device.empty() ? "/dev/video0" : rq.device});
            break;
        case Platform::Windows:
            if (rq.device.empty()) {
                plan.error = "pass device (a camera name from capture.list_devices)";
                return plan;
            }
            plan.backend = "dshow";
            plan.args.insert(plan.args.end(), {"-f", "dshow"});
            plan.args.insert(plan.args.end(), sizeArgs.begin(), sizeArgs.end());
            plan.args.insert(plan.args.end(), {"-i", "video=" + rq.device});
            break;
        case Platform::Mac:
            plan.backend = "avfoundation";
            plan.args.insert(plan.args.end(), {"-f", "avfoundation", "-framerate", "30"});
            plan.args.insert(plan.args.end(), sizeArgs.begin(), sizeArgs.end());
            plan.args.insert(plan.args.end(), {"-i", (rq.device.empty() ? std::string("0") : rq.device) + ":none"});
            break;
    }
    if (rq.warmup > 0) plan.args.insert(plan.args.end(), {"-ss", fixed(rq.warmup)});
    plan.args.insert(plan.args.end(), {"-frames:v", "1", "-update", "1"});
    if (ext != "png") plan.args.insert(plan.args.end(), {"-q:v", "2"});
    plan.args.push_back(rq.output);
    return plan;
}

std::vector<std::string> listDevicesArgs(Platform platform) {
    switch (platform) {
        case Platform::Windows: return {"ffmpeg", "-hide_banner", "-nostdin", "-list_devices", "true", "-f", "dshow", "-i", "dummy"};
        case Platform::Mac: return {"ffmpeg", "-hide_banner", "-nostdin", "-f", "avfoundation", "-list_devices", "true", "-i", ""};
        case Platform::Linux: break;
    }
    return {};
}

std::vector<Monitor> parseXrandrMonitors(const std::string& text) {
    std::vector<Monitor> out;
    static const std::regex line(R"(^\s*(\d+):\s+\+?(\*?)(\S+)\s+(\d+)/\d+x(\d+)/\d+\+(-?\d+)\+(-?\d+))");
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string l = text.substr(start, end - start);
        std::smatch m;
        if (std::regex_search(l, m, line)) {
            Monitor mon;
            mon.index = std::stoi(m[1].str());
            mon.primary = m[2].str() == "*";
            mon.name = m[3].str();
            mon.width = std::stoi(m[4].str());
            mon.height = std::stoi(m[5].str());
            mon.x = std::stoi(m[6].str());
            mon.y = std::stoi(m[7].str());
            out.push_back(mon);
        }
        start = end + 1;
    }
    return out;
}

DeviceList parseDeviceListing(Platform platform, const std::string& text) {
    DeviceList d;
    std::string section;
    std::size_t start = 0;
    static const std::regex quoted("\"([^\"]+)\"(\\s*\\((video|audio|none)\\))?");
    static const std::regex indexed(R"(\[(\d+)\] (.+)$)");
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string l = text.substr(start, end - start);
        start = end + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const std::string low = lower(l);
        if (low.find("video devices") != std::string::npos) {
            section = "video";
            continue;
        }
        if (low.find("audio devices") != std::string::npos) {
            section = "audio";
            continue;
        }
        if (platform == Platform::Windows) {
            if (low.find("alternative name") != std::string::npos) continue;
            std::smatch m;
            if (!std::regex_search(l, m, quoted)) continue;
            const std::string kind = m[3].matched ? m[3].str() : section;
            if (kind == "video") d.video.push_back(m[1].str());
            else if (kind == "audio") d.audio.push_back(m[1].str());
        } else if (platform == Platform::Mac) {
            std::smatch m;
            if (!std::regex_search(l, m, indexed)) continue;
            const std::string name = m[2].str();
            if (section == "video") {
                if (name.rfind("Capture screen", 0) == 0) d.screens.push_back(name);
                else d.video.push_back(name);
            } else if (section == "audio") {
                d.audio.push_back(name);
            }
        }
    }
    return d;
}

}  // namespace achclwss::capture
