// animate: headless 2D and 3D animation. A document has a canvas size, a
// frame rate and a frame count, and a stack of layers. 2D layers hold shapes,
// text, pictures, frame-by-frame drawn cels and bones; 3D layers hold meshes
// and a camera. Any number or colour property can be keyframed with an easing
// curve, and elements can be parented to each other (cutout rigs), so a frame
// is: evaluate every property at that frame, walk the parent chain, draw.
// Rendering is all in-process (libs/raster for 2D, libs/mesh's preview
// renderer for 3D); only mp4/webm/mov export calls ffmpeg.
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "achclwss/core/command.hpp"
#include "achclwss/mesh/mesh.hpp"
#include "achclwss/raster/image.hpp"

namespace achclwss::animate {

using raster::Color;
using raster::Image;

// ---- easing ----

enum class Ease { Linear, EaseIn, EaseOut, EaseInOut, Step, Bezier };

const std::vector<std::string>& easeNames();  // linear ease-in ease-out ease-in-out step bezier
std::optional<Ease> parseEase(const std::string& name);  // also takes "hold" for step
std::string toString(Ease ease);
// CSS cubic-bezier(x1, y1, x2, y2) at progress x in 0..1.
double cubicBezier(double x1, double y1, double x2, double y2, double x);
// Eased progress for t in 0..1. bezier is only used by Ease::Bezier.
double ease(Ease e, double t, const std::array<double, 4>& bezier = {0.25, 0.1, 0.25, 1.0});

// ---- the document ----

struct Key {
    int frame = 0;
    double number = 0;          // number properties
    Color color;                // colour properties
    Ease ease = Ease::Linear;   // how the value travels into this key from the one before
    std::array<double, 4> bezier{0.25, 0.1, 0.25, 1.0};
};

// Element kinds: rect ellipse polygon line text image cel bone (2D layers), mesh camera (3D layers).
const std::vector<std::string>& kinds2d();
const std::vector<std::string>& kinds3d();
bool is3dKind(const std::string& kind);
// The number and colour properties an element kind has (all keyframeable), and their defaults.
const std::vector<std::string>& numberProps(const std::string& kind);
const std::vector<std::string>& colorProps(const std::string& kind);
bool isColorProp(const std::string& prop);
// Mesh primitives a mesh element can be instead of a file.
const std::vector<std::string>& primitiveNames();  // box sphere cylinder cone torus plane

struct Element {
    std::string id;    // e1, e2, ... unique in the document
    std::string kind;
    std::string name;
    std::string parent;  // id of the element this one moves with; empty = none
    bool visible = true;

    std::map<std::string, double> numbers;  // base values (used when a property has no keys)
    std::map<std::string, Color> colors;
    std::map<std::string, std::vector<Key>> keys;  // per property, sorted by frame

    std::vector<double> points;  // polygon/line: x,y pairs in local px
    std::string text;            // text: may hold \n for several lines
    std::string font = "sans";   // sans, serif, mono
    bool bold = false, italic = false;
    std::string source;          // image/mesh file (absolute) or a primitive name
    std::vector<double> size;    // primitive dimensions

    std::map<int, std::shared_ptr<const Image>> cels;  // cel drawings by the frame they start on
    std::shared_ptr<const Image> picture;              // image: loaded from source
    std::shared_ptr<const mesh::Mesh> geometry;        // mesh: loaded or built, with normals
};

struct Layer {
    std::string id;  // L1, L2, ...
    std::string name;
    std::string kind = "2d";  // 2d or 3d
    bool visible = true;
    double opacity = 1;
    std::string blend = "normal";  // a raster blend mode name
    std::vector<Element> elements;  // bottom first
};

struct Settings {
    int width = 640;
    int height = 360;
    double fps = 24;
    int frames = 48;
    Color background{255, 255, 255, 255};
};

class AnimDoc : public Document {
public:
    Settings settings;
    std::vector<Layer> layers;  // bottom first
    int nextElement = 1;
    int nextLayer = 1;

    std::string kind() const override { return "animation"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // frame 0, up to 512 px

    Layer* layer(const std::string& id);
    const Layer* layer(const std::string& id) const;
    Element* element(const std::string& id, Layer** owner = nullptr);
    const Element* element(const std::string& id, const Layer** owner = nullptr) const;
    std::size_t elementCount() const;
};

// A new element of `kind` with every property at its default (canvas-sized for cels).
Element makeElement(const std::string& kind, const Settings& settings);
// Loads an image/mesh element's source into picture/geometry. Throws std::runtime_error.
void loadSource(Element& e);

// ---- evaluating properties ----

double numberAt(const Element& e, const std::string& prop, double frame);
Color colorAt(const Element& e, const std::string& prop, double frame);
// Inserts or replaces the key at key.frame.
void setKey(Element& e, const std::string& prop, const Key& key);
// The cel drawing showing at `frame` (the latest drawing at or before it), or null.
std::shared_ptr<const Image> celAt(const Element& e, int frame, int* startFrame = nullptr);

// 2D affine transform: x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Affine operator*(const Affine& o) const;
    void apply(double x, double y, double& ox, double& oy) const;
    Affine inverse() const;
    double scale() const;  // sqrt(|det|)
};

// Natural size of a 2D element's box (rect/image/text/cel), in local px.
void elementSize(const Element& e, double frame, double& w, double& h);
// The element's transform to canvas px at `frame`, through its parents.
Affine worldTransform2d(const AnimDoc& doc, const Element& e, double frame);
// The 3D element's object-to-world matrix at `frame`, through its parents.
mesh::Mat4 worldTransform3d(const AnimDoc& doc, const Element& e, double frame);

// ---- rendering ----

struct FrameOptions {
    double scale = 1;     // output size = canvas x scale
    int onion = 0;        // cels: show this many drawings before (red) and after (green)
    double onionOpacity = 0.35;
    bool bones = false;   // draw bones and their joints on top
};

Image renderFrame(const AnimDoc& doc, int frame, const FrameOptions& options = {});

// ---- files ----

std::string toJson(const AnimDoc& doc);
// Throws std::runtime_error. Relative source paths resolve against baseDir.
std::shared_ptr<AnimDoc> fromJson(const std::string& json, const std::filesystem::path& baseDir = {});

// Animated GIF from frames of equal size. delays in 1/100 s per frame; loop 0 = forever.
// Pixels with alpha < 128 become transparent. Each frame gets its own median-cut palette.
std::string encodeGif(const std::vector<Image>& frames, const std::vector<int>& delays, int loop = 0, bool dither = false);

// ---- undo ----

// One undo step: snapshots the document, runs the edit, restores it on failure or undo.
class AnimEdit : public Action {
public:
    AnimEdit(std::shared_ptr<AnimDoc> doc, std::function<Result(AnimDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    struct Snapshot {
        Settings settings;
        std::vector<Layer> layers;
        int nextElement = 1, nextLayer = 1;
    };
    std::shared_ptr<AnimDoc> doc_;
    std::function<Result(AnimDoc&)> fn_;
    Snapshot before_;
};

void registerCommands(Session& session);

}  // namespace achclwss::animate
