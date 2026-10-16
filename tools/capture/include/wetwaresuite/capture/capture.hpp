// capture: screenshots, screen recordings and webcam snapshots through
// ffmpeg's capture devices (x11grab on Linux, with grim for Wayland; gdigrab
// or ddagrab on Windows; avfoundation on macOS). The ffmpeg command lines are
// planned by pure functions so they can be tested without a screen.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::capture {

enum class Platform { Linux, Windows, Mac };
Platform currentPlatform();
const char* platformName(Platform p);

struct Region {
    int x = 0, y = 0, width = 0, height = 0;
    bool set() const { return width > 0 && height > 0; }
};

struct Monitor {
    int index = 0;
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
    bool primary = false;
};

// What this machine offers for capturing.
struct Environment {
    Platform platform = Platform::Linux;
    std::string display;         // X11 DISPLAY, e.g. ":0"
    std::string waylandDisplay;  // WAYLAND_DISPLAY
    bool hasFfmpeg = false;
    bool hasGrim = false;
    std::vector<Monitor> monitors;  // empty when unknown
};

// Reads the environment, PATH and (where it can) the monitor layout.
Environment detect(const std::string& displayOverride = {});

struct ScreenshotRequest {
    std::string output;   // png or jpg
    int monitor = -1;     // -1 = every screen
    Region region;        // relative to the monitor when one is given
    std::string backend = "auto";  // auto, x11grab, grim, gdigrab, ddagrab, avfoundation
    bool cursor = false;
};

struct RecordRequest {
    std::string output;   // mp4, mkv, mov, webm or gif
    double duration = 10;
    int fps = 30;
    int monitor = -1;
    Region region;
    bool audio = false;
    std::string audioDevice;  // pulse source / dshow name / avfoundation index; empty = default
    std::string backend = "auto";
    bool cursor = true;
    std::string quality = "good";  // draft, good, best
};

struct WebcamRequest {
    std::string output;
    std::string device;  // /dev/video0, a dshow name, an avfoundation index; empty = first
    int width = 0, height = 0;
    double warmup = 0.5;  // seconds of frames to skip while the camera adjusts
};

// An ffmpeg (or grim) command line; args[0] is the program name. error is set instead when it can't be done.
struct Plan {
    std::vector<std::string> args;
    std::string backend;
    std::string error;
    bool ok() const { return error.empty(); }
};

Plan planScreenshot(const Environment& env, const ScreenshotRequest& request);
Plan planRecord(const Environment& env, const RecordRequest& request);
Plan planWebcam(const Environment& env, const WebcamRequest& request);
// ffmpeg arguments that print the capture devices (dshow, avfoundation); empty on Linux.
std::vector<std::string> listDevicesArgs(Platform platform);

struct DeviceList {
    std::vector<std::string> video;    // cameras
    std::vector<std::string> audio;    // microphones / sources
    std::vector<std::string> screens;  // avfoundation "Capture screen N"
};

// Parses `xrandr --listmonitors`.
std::vector<Monitor> parseXrandrMonitors(const std::string& text);
// Parses ffmpeg's -list_devices output for dshow (Windows) or avfoundation (macOS).
DeviceList parseDeviceListing(Platform platform, const std::string& text);

// The message for a machine without a screen.
std::string noDisplayMessage();

void registerCommands(Session& session);

}  // namespace wetwaresuite::capture
