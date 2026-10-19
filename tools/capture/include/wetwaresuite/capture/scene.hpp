// Scenes, the way OBS has them: a canvas with sources stacked on it (the screen
// or part of it, a window, a webcam, images, text, colour boxes), each placed,
// sized, faded or hidden, recorded to a file in one ffmpeg run. Text, images
// and colour boxes are drawn by the cut compositor; live sources are read by
// ffmpeg's capture devices. Planning is pure, so it's tested without a screen.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/capture/capture.hpp"
#include "wetwaresuite/core/command.hpp"

namespace wetwaresuite::capture {

struct Source {
    std::string id;    // s1, s2, ...
    std::string kind;  // screen, window, webcam, image, text, color
    std::string name;  // shown in lists
    // where it sits on the canvas (width 0 = the whole canvas)
    double x = 0, y = 0, width = 0, height = 0;
    double opacity = 1;
    bool visible = true;
    // screen and window: what to grab
    int monitor = -1;
    Region region;
    std::string window;  // window title (Windows)
    bool cursor = true;
    // webcam
    std::string device;
    int captureWidth = 0, captureHeight = 0;
    std::string keyColor;  // green screen: this colour becomes see-through ("" = off)
    double keySimilarity = 0.3;
    // image
    std::string path;
    // text (and the colour of a colour box)
    std::string text;
    std::string font;
    double fontSize = 48;
    std::string color = "white";
    double outline = 0;
    std::string outlineColor = "black";
    bool box = false;
    bool live() const { return kind == "screen" || kind == "window" || kind == "webcam"; }
};

class Scene : public Document {
public:
    std::string name = "Scene";
    int width = 1920, height = 1080, fps = 30;
    std::string background = "black";
    std::vector<Source> sources;  // bottom first
    int nextSource = 1;

    std::string kind() const override { return "scene"; }
    std::string summary() const override;
    // The canvas with placeholders for the live sources (no capture devices touched).
    std::unique_ptr<Attachment> preview() const override;

    Source* find(const std::string& id);
};

std::string toJson(const Scene& scene);
// Throws std::runtime_error with a readable reason.
std::shared_ptr<Scene> sceneFromJson(const std::string& json, const std::filesystem::path& baseDir = {});

// The scene as a picture: text, images and colour boxes drawn, each live source replaced by
// `liveFrames[id]` (a PNG/JPEG file) when given, else by a labelled placeholder. PNG bytes.
std::string renderScene(const Scene& scene, const std::map<std::string, std::string>& liveFrames, int width = 0);
// One still source alone on a transparent canvas, as PNG bytes (what recording overlays).
std::string renderStaticSource(const Scene& scene, const Source& source);

struct SceneRecordRequest {
    std::string output;     // mp4, mkv, mov, webm or gif
    double duration = 0;    // seconds; 0 = until stopped (capture.stop_recording)
    std::string quality = "good";
    bool audio = false;
    std::string audioDevice;
    bool forStop = false;   // stopped by writing q to ffmpeg's stdin (so no -nostdin)
};

// The ffmpeg run that records the scene. `stills` maps each visible still source (image, text,
// colour) to the PNG renderStaticSource made for it. Sets error when it can't be done here.
Plan planSceneRecord(const Environment& env, const Scene& scene, const SceneRecordRequest& request,
                     const std::map<std::string, std::string>& stills);

void registerSceneCommands(Session& session);

}  // namespace wetwaresuite::capture
