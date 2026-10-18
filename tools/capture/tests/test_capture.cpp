#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "wetwaresuite/capture/capture.hpp"
#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::capture;
namespace fs = std::filesystem;

namespace {

using Args = std::vector<std::string>;

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

bool has(const Args& args, const Args& run) {
    if (run.size() > args.size()) return false;
    for (std::size_t i = 0; i + run.size() <= args.size(); ++i) {
        bool all = true;
        for (std::size_t j = 0; j < run.size() && all; ++j) all = args[i + j] == run[j];
        if (all) return true;
    }
    return false;
}

std::string after(const Args& args, const std::string& flag) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return {};
}

Environment linux(const std::string& display = ":0") {
    Environment e;
    e.platform = Platform::Linux;
    e.display = display;
    e.hasFfmpeg = true;
    e.monitors = {{0, "DP-1", 0, 0, 1920, 1080, true}, {1, "HDMI-1", 1920, 0, 1280, 1024, false}};
    return e;
}

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -1;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    if (auto d = std::get_if<double>(v)) return *d;
    return -1;
}

void testScreenshotPlans() {
    // the whole X screen
    ScreenshotRequest rq;
    rq.output = "shot.png";
    Plan p = planScreenshot(linux(), rq);
    CHECK(p.ok() && p.backend == "x11grab");
    CHECK(p.args == Args({"ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-y", "-f", "x11grab", "-draw_mouse", "0",
                          "-framerate", "30", "-i", ":0", "-frames:v", "1", "-update", "1", "shot.png"}));
    // a region of the second monitor: offsets add up
    rq.monitor = 1;
    rq.region = {100, 50, 640, 480};
    rq.cursor = true;
    p = planScreenshot(linux(), rq);
    CHECK(p.ok() && has(p.args, {"-video_size", "640x480", "-i", ":0+2020,50"}) && has(p.args, {"-draw_mouse", "1"}));
    // a whole monitor
    rq.region = {};
    p = planScreenshot(linux(), rq);
    CHECK(has(p.args, {"-video_size", "1280x1024", "-i", ":0+1920,0"}));
    // regions that don't fit, unknown monitors, half a region
    rq.region = {1000, 0, 640, 480};
    CHECK(!planScreenshot(linux(), rq).ok());
    rq.monitor = 5;
    rq.region = {};
    CHECK(contains(planScreenshot(linux(), rq).error, "no monitor 5"));
    rq.monitor = -1;
    rq.region = {0, 0, 100, 0};
    CHECK(contains(planScreenshot(linux(), rq).error, "both width and height"));
    rq.region = {};
    rq.output = "shot.jpg";
    p = planScreenshot(linux(), rq);
    CHECK(after(p.args, "-q:v") == "2");
    rq.output = "shot.bmp";
    CHECK(!planScreenshot(linux(), rq).ok());

    // no display at all: a clear message, nothing to run
    rq.output = "shot.png";
    Environment none = linux("");
    p = planScreenshot(none, rq);
    CHECK(!p.ok() && p.args.empty() && contains(p.error, "no screen to capture") && contains(p.error, "Xvfb"));
    // Wayland: grim when present, otherwise XWayland, otherwise a hint
    Environment wl = linux("");
    wl.waylandDisplay = "wayland-0";
    wl.hasGrim = true;
    rq.cursor = false;
    rq.region = {10, 20, 300, 200};
    p = planScreenshot(wl, rq);
    CHECK(p.backend == "grim" && p.args == Args({"grim", "-g", "10,20 300x200", "shot.png"}));
    wl.hasGrim = false;
    CHECK(contains(planScreenshot(wl, rq).error, "grim"));
    wl.display = ":1";
    CHECK(planScreenshot(wl, rq).backend == "x11grab");
    rq.backend = "gdigrab";
    CHECK(contains(planScreenshot(wl, rq).error, "isn't available on Linux"));
    rq.backend = "auto";

    // Windows: gdigrab over the virtual desktop, ddagrab per monitor
    Environment win;
    win.platform = Platform::Windows;
    win.hasFfmpeg = true;
    win.monitors = {{0, "\\\\.\\DISPLAY1", 0, 0, 2560, 1440, true}, {1, "\\\\.\\DISPLAY2", -1920, 0, 1920, 1080, false}};
    rq.monitor = 1;
    rq.region = {};
    p = planScreenshot(win, rq);
    CHECK(p.backend == "gdigrab" && has(p.args, {"-offset_x", "-1920", "-offset_y", "0", "-video_size", "1920x1080", "-i", "desktop"}));
    rq.backend = "ddagrab";
    rq.region = {0, 0, 800, 600};
    p = planScreenshot(win, rq);
    CHECK(has(p.args, {"-f", "lavfi", "-i", "ddagrab=output_idx=1:draw_mouse=0:framerate=30:video_size=800x600:offset_x=0:offset_y=0"}));
    CHECK(after(p.args, "-vf") == "hwdownload,format=bgra");
    rq.backend = "auto";

    // macOS: avfoundation screens by name, regions by crop
    Environment mac;
    mac.platform = Platform::Mac;
    mac.hasFfmpeg = true;
    rq.monitor = 0;
    rq.region = {10, 20, 400, 300};
    rq.cursor = false;
    p = planScreenshot(mac, rq);
    CHECK(p.backend == "avfoundation" && has(p.args, {"-f", "avfoundation", "-capture_cursor", "0"}));
    CHECK(after(p.args, "-i") == "Capture screen 0:none" && after(p.args, "-vf") == "crop=400:300:10:20");

    Environment noFfmpeg = linux();
    noFfmpeg.hasFfmpeg = false;
    rq.monitor = -1;
    rq.region = {};
    CHECK(contains(planScreenshot(noFfmpeg, rq).error, "ffmpeg"));
}

void testRecordPlans() {
    RecordRequest rq;
    rq.output = "rec.mp4";
    rq.duration = 30;
    rq.fps = 25;
    rq.audio = true;
    Plan p = planRecord(linux(), rq);
    CHECK(p.ok());
    CHECK(has(p.args, {"-f", "x11grab", "-draw_mouse", "1", "-framerate", "25", "-thread_queue_size", "1024", "-t", "30", "-i", ":0"}));
    CHECK(has(p.args, {"-thread_queue_size", "1024", "-f", "pulse", "-t", "30", "-i", "default"}));
    CHECK(has(p.args, {"-c:v", "libx264", "-preset", "veryfast", "-crf", "23"}) && has(p.args, {"-c:a", "aac"}));
    CHECK(after(p.args, "-vf") == "scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p");
    CHECK(p.args.back() == "rec.mp4" && has(p.args, {"-movflags", "+faststart", "-t", "30", "rec.mp4"}));
    rq.audioDevice = "alsa_input.usb-mic";
    CHECK(after(planRecord(linux(), rq).args, "pulse") == "-t");
    CHECK(has(planRecord(linux(), rq).args, {"-i", "alsa_input.usb-mic"}));

    rq.audio = false;
    rq.output = "rec.gif";
    rq.region = {0, 0, 640, 360};
    p = planRecord(linux(), rq);
    CHECK(p.ok() && has(p.args, {"-an", "-loop", "0"}) && contains(after(p.args, "-vf"), "palettegen") && contains(after(p.args, "-vf"), "fps=15"));
    rq.output = "rec.webm";
    rq.quality = "draft";
    p = planRecord(linux(), rq);
    CHECK(has(p.args, {"-c:v", "libvpx-vp9"}) && has(p.args, {"-crf", "42"}) && has(p.args, {"-an"}));
    rq.output = "rec.avi";
    CHECK(!planRecord(linux(), rq).ok());
    rq.output = "rec.mp4";
    rq.duration = 0;
    CHECK(!planRecord(linux(), rq).ok());
    rq.duration = 5;
    CHECK(contains(planRecord(linux(""), rq).error, "no screen"));
    Environment wl = linux("");
    wl.waylandDisplay = "wayland-0";
    wl.hasGrim = true;
    CHECK(contains(planRecord(wl, rq).error, "Wayland"));

    Environment win;
    win.platform = Platform::Windows;
    win.hasFfmpeg = true;
    rq.audio = true;
    rq.audioDevice.clear();
    rq.region = {};
    CHECK(contains(planRecord(win, rq).error, "audio_device"));
    rq.audioDevice = "Microphone (Realtek)";
    p = planRecord(win, rq);
    CHECK(has(p.args, {"-f", "gdigrab"}) && has(p.args, {"-f", "dshow", "-t", "5", "-i", "audio=Microphone (Realtek)"}));

    Environment mac;
    mac.platform = Platform::Mac;
    mac.hasFfmpeg = true;
    rq.audioDevice.clear();
    p = planRecord(mac, rq);
    CHECK(after(p.args, "-i") == "Capture screen 0:0");
}

void testWebcamAndParsing() {
    WebcamRequest rq;
    rq.output = "me.jpg";
    rq.width = 1280;
    rq.height = 720;
    Plan p = planWebcam(linux(), rq);
    CHECK(p.backend == "v4l2" && has(p.args, {"-f", "v4l2", "-video_size", "1280x720", "-i", "/dev/video0", "-ss", "0.5", "-frames:v", "1"}));
    Environment win;
    win.platform = Platform::Windows;
    win.hasFfmpeg = true;
    CHECK(!planWebcam(win, rq).ok());
    rq.device = "Integrated Camera";
    CHECK(has(planWebcam(win, rq).args, {"-i", "video=Integrated Camera"}));
    Environment mac;
    mac.platform = Platform::Mac;
    mac.hasFfmpeg = true;
    rq.device.clear();
    CHECK(after(planWebcam(mac, rq).args, "-i") == "0:none");
    rq.height = 0;
    CHECK(!planWebcam(linux(), rq).ok());

    auto mons = parseXrandrMonitors("Monitors: 2\n 0: +*eDP-1 1920/344x1080/194+0+0  eDP-1\n 1: +HDMI-1 2560/597x1440/336+1920+-200  HDMI-1\n");
    CHECK(mons.size() == 2);
    CHECK(mons[0].name == "eDP-1" && mons[0].primary && mons[0].width == 1920 && mons[0].height == 1080);
    CHECK(mons[1].name == "HDMI-1" && !mons[1].primary && mons[1].x == 1920 && mons[1].y == -200 && mons[1].width == 2560);
    CHECK(parseXrandrMonitors("Monitors: 1\n 0: +*screen 640/170x480/127+0+0  screen\n").size() == 1);

    DeviceList d = parseDeviceListing(Platform::Windows,
                                      "[dshow @ 000001] \"Integrated Camera\" (video)\n"
                                      "[dshow @ 000001]   Alternative name \"@device_pnp_\\\\?\\usb#vid\"\n"
                                      "[dshow @ 000001] \"Microphone Array (Realtek(R) Audio)\" (audio)\n"
                                      "[dshow @ 000001]   Alternative name \"@device_cm_{33D9A762}\"\n"
                                      "dummy: Immediate exit requested\n");
    CHECK(d.video.size() == 1 && d.video[0] == "Integrated Camera");
    CHECK(d.audio.size() == 1 && d.audio[0] == "Microphone Array (Realtek(R) Audio)");
    // older ffmpeg: sections instead of (video)/(audio) tags
    d = parseDeviceListing(Platform::Windows,
                           "[dshow @ 0] DirectShow video devices (some may be both video and audio devices)\n[dshow @ 0]  \"USB Cam\"\n"
                           "[dshow @ 0] DirectShow audio devices\n[dshow @ 0]  \"Line In\"\n");
    CHECK(d.video.size() == 1 && d.video[0] == "USB Cam" && d.audio.size() == 1 && d.audio[0] == "Line In");
    d = parseDeviceListing(Platform::Mac,
                           "[AVFoundation indev @ 0x7f8] AVFoundation video devices:\n"
                           "[AVFoundation indev @ 0x7f8] [0] FaceTime HD Camera\n"
                           "[AVFoundation indev @ 0x7f8] [1] Capture screen 0\n"
                           "[AVFoundation indev @ 0x7f8] [2] Capture screen 1\n"
                           "[AVFoundation indev @ 0x7f8] AVFoundation audio devices:\n"
                           "[AVFoundation indev @ 0x7f8] [0] MacBook Pro Microphone\n"
                           ": Input/output error\n");
    CHECK(d.video.size() == 1 && d.video[0] == "FaceTime HD Camera");
    CHECK(d.screens.size() == 2 && d.screens[1] == "Capture screen 1");
    CHECK(d.audio.size() == 1 && d.audio[0] == "MacBook Pro Microphone");
    CHECK(listDevicesArgs(Platform::Linux).empty() && listDevicesArgs(Platform::Windows).size() > 3);
}

void testCommands() {
    Session s;
    registerCommands(s);
    for (const char* id : {"capture.screenshot", "capture.record", "capture.webcam", "capture.list_displays", "capture.list_devices"}) {
        CHECK(s.registry().find(id) != nullptr);
    }
    CHECK(s.registry().find("capture.list_displays")->readOnly);
    CHECK(!s.registry().find("capture.screenshot")->readOnly);

    mcp::Server server(s, "test", "0");
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"capture_screenshot", "capture_record", "capture_webcam", "capture_list_displays", "capture_list_devices"}) {
        CHECK(contains(list, std::string("\"") + tool + "\""));
    }
    std::string r = server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"capture_list_devices","arguments":{}}})").value_or("");
    // off linux, listing devices asks ffmpeg; without it the tool says how to get it
    if (currentPlatform() == Platform::Linux || findProgram("ffmpeg")) {
        CHECK(contains(r, "\"isError\":false") && contains(r, "cameras"));
    } else {
        CHECK(contains(r, "\"isError\":true") && contains(r, "needs ffmpeg"));
    }
    // on a machine without a screen, a screenshot fails with the explanation
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    if (currentPlatform() == Platform::Linux && (!display || !*display) && (!wayland || !*wayland)) {
        r = server.handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"capture_screenshot","arguments":{}}})").value_or("");
        CHECK(contains(r, "\"isError\":true") && contains(r, "no screen to capture"));
        Result rec = s.run("capture.record", {{"output", std::string("x.mp4")}, {"duration", 1.0}});
        CHECK(!rec.ok && contains(rec.message, "no screen"));
    }
}

// A real screenshot and recording on a virtual X screen, when Xvfb and ffmpeg are installed.
void testXvfb() {
#ifndef _WIN32
    if (!findProgram("Xvfb") || !findProgram("ffmpeg")) {
        std::printf("Xvfb or ffmpeg not found: skipping the real capture test\n");
        return;
    }
    const fs::path dir = fs::temp_directory_path() / "wetwaresuite_capture_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    int number = 0;
    for (int n = 120; n < 200 && !number; ++n) {
        if (!fs::exists("/tmp/.X11-unix/X" + std::to_string(n)) && !fs::exists("/tmp/.X" + std::to_string(n) + "-lock")) number = n;
    }
    const std::string display = ":" + std::to_string(number);
    const std::string pidFile = (dir / "xvfb.pid").string();
    const std::string start = "Xvfb " + display + " -screen 0 320x240x24 -nolisten tcp >/dev/null 2>&1 & echo $! > " + pidFile;
    if (number == 0 || std::system(start.c_str()) != 0) {
        std::printf("couldn't start Xvfb: skipping\n");
        return;
    }
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        ready = fs::exists("/tmp/.X11-unix/X" + std::to_string(number));
        if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Session s;
        registerCommands(s);
        cli::Batch batch(s);
        const std::string shot = (dir / "shot.png").string();
        Result r = batch.runLine("capture.screenshot display=" + display + " output=\"" + shot + "\"");
        CHECK(r.ok && num(r, "width") == 320 && num(r, "height") == 240);
        r = batch.runLine("capture.screenshot display=" + display + " output=\"" + (dir / "part.jpg").string() + "\" x=10 y=20 width=100 height=60");
        CHECK(r.ok && num(r, "width") == 100 && num(r, "height") == 60);
        r = batch.runLine("capture.screenshot display=" + display + " width=64 height=48 max_size=32");
        CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png" && r.attachments[0].name == "screenshot.png");
        r = batch.runLine("capture.list_displays display=" + display);
        CHECK(r.ok && num(r, "width") == 320);
        r = batch.runLine("capture.record display=" + display + " output=\"" + (dir / "rec.mp4").string() + "\" duration=1 fps=10");
        CHECK(r.ok && num(r, "bytes") > 0);
        r = batch.runLine("capture.record display=" + display + " output=\"" + (dir / "rec.gif").string() + "\" duration=0.5 fps=10");
        CHECK(r.ok && num(r, "bytes") > 0);
        if (!r.ok) std::printf("  %s\n", r.message.c_str());
    } else {
        std::printf("Xvfb didn't come up: skipping\n");
    }
    std::ifstream pid(pidFile);
    std::string id;
    pid >> id;
    if (!id.empty() && id.find_first_not_of("0123456789") == std::string::npos) {
        const std::string kill = "kill " + id + " 2>/dev/null";
        CHECK(std::system(kill.c_str()) == 0);
    }
    fs::remove_all(dir);
#endif
}

}  // namespace

int main() {
    testScreenshotPlans();
    testRecordPlans();
    testWebcamAndParsing();
    testCommands();
    testXvfb();
    return check::summary("capture");
}
