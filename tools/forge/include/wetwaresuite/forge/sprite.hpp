// Animated pixel-art sprites, Aseprite style: a canvas size, layers, frames
// with their own durations, a cel (picture) per layer and frame, animation
// tags and a palette, optionally in indexed colour. Reads and writes
// .aseprite/.ase files, exports sprite sheets with Aseprite or TexturePacker
// style JSON, Godot SpriteFrames and animated GIFs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::forge {

using raster::Color;
using raster::Image;

struct SpriteLayer {
    std::string name;
    bool visible = true;
    int opacity = 255;   // 0..255
    int blend = 0;       // Aseprite blend mode number (0 normal, 1 multiply, 2 screen, ...)
    bool group = false;  // a folder of the layers after it with a deeper level
    int level = 0;       // nesting depth (Aseprite's child level)
};

struct SpriteTag {
    std::string name;
    int from = 0, to = 0;                 // frames, inclusive
    std::string direction = "forward";    // forward, reverse, pingpong, pingpong_reverse
    int repeat = 0;                       // 0 = forever
    Color color{0, 0, 0, 255};
};

struct Sprite {
    int width = 0, height = 0;
    std::vector<SpriteLayer> layers;           // bottom first
    std::vector<int> durations;                // ms per frame
    std::vector<std::vector<Image>> cels;      // [layer][frame]; an empty Image means no cel
    std::vector<SpriteTag> tags;
    std::vector<Color> palette;
    bool indexed = false;                      // pixels must come from the palette
    int transparentIndex = 0;                  // indexed: the palette entry that means "clear"

    int frames() const { return static_cast<int>(durations.size()); }
    // The cel at (layer, frame), created transparent when missing.
    Image& cel(int layer, int frame);
    const Image* celIf(int layer, int frame) const;
    // Visible layers flattened (hidden groups hide their children).
    Image flatten(int frame) const;
    // Layer index by name or number; -1 when missing.
    int layerIndex(const std::string& nameOrIndex) const;
    int tagIndex(const std::string& name) const;
    // The frames a tag plays, in order (pingpong goes there and back).
    std::vector<int> tagFrames(const SpriteTag& tag) const;
};

// A width x height sprite with `frames` frames of `duration` ms and one layer.
Sprite newSprite(int width, int height, int frames = 1, int duration = 100);

// Aseprite files. Throws std::runtime_error. Tilemap layers come in empty;
// linked cels become copies; cel opacity is multiplied into the pixels.
Sprite readAseprite(const std::string& bytes);
std::string writeAseprite(const Sprite& sprite);

// ---- palettes and indexed colour ----

int nearestIndex(const std::vector<Color>& palette, Color c, int skip = -1);
// Every opaque-ish pixel of every cel snapped to its nearest palette colour
// (alpha below 128 becomes clear), so the sprite can be saved as indexed.
void snapToPalette(Sprite& sprite);
// Distinct colours used across all cels.
std::vector<Color> spriteColors(const Sprite& sprite);
// Built-in palettes: pico8, db16, db32, gameboy, grayscale4.
std::vector<Color> presetPalette(const std::string& name);
// GIMP .gpl / JASC .pal / hex-per-line files.
std::vector<Color> loadPalette(const std::string& path);
std::string paletteGpl(const std::vector<Color>& palette, const std::string& name);

// ---- drawing on one cel (pixel exact, no antialiasing) ----

void drawPixelLine(Image& img, int x0, int y0, int x1, int y1, Color c);
void drawPixelRect(Image& img, int x0, int y0, int x1, int y1, Color c, bool filled);
void drawPixelEllipse(Image& img, int x0, int y0, int x1, int y1, Color c, bool filled);
// Exact-colour flood fill (contiguous, 4-way); returns pixels changed.
int bucketFill(Image& img, int x, int y, Color c);

// ---- export ----

struct SheetOptions {
    std::string layout = "horizontal";  // horizontal, vertical, grid, packed
    int columns = 0;                     // grid: 0 = about square
    int padding = 0;                     // px between and around frames
    bool trim = false;                   // packed/any: cut transparent borders
    int scale = 1;                       // integer upscale
    std::vector<int> frames;             // which frames, in order (empty = all)
    std::string format = "hash";         // JSON: hash or array
    std::string imageName;               // meta.image
    std::string title = "sprite";        // frame names: "<title> <n>.aseprite"
};

struct SheetResult {
    Image image;
    std::string json;  // Aseprite-style JSON (frames, meta.frameTags, meta.layers)
    struct Placed {
        int frame = 0;
        int x = 0, y = 0, w = 0, h = 0;      // on the sheet
        int offX = 0, offY = 0;              // trimmed offset in the frame
    };
    std::vector<Placed> placed;
};

SheetResult exportSheet(const Sprite& sprite, const SheetOptions& options);
// A Godot 4 SpriteFrames resource: one animation per tag (or "default"),
// AtlasTextures cut from the sheet at `texturePath`.
std::string godotSpriteFrames(const Sprite& sprite, const SheetResult& sheet, const std::string& texturePath, int scale);
// An animated GIF of the given frames (all when empty), looping forever.
std::string encodeSpriteGif(const Sprite& sprite, const std::vector<int>& frames, int scale = 1);
// The frame with `before` previous frames tinted red and `after` next frames
// tinted blue underneath, fading with distance (Aseprite's onion skin).
Image onionSkin(const Sprite& sprite, int frame, int before, int after, int opacity = 96);

// ---- documents ----

class SpriteDoc : public Document {
public:
    Sprite sprite;
    std::string kind() const override { return "sprite"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
};

// Undoable edit: snapshots the sprite before `fn`, restores it on undo or failure.
class SpriteEdit : public Action {
public:
    SpriteEdit(std::shared_ptr<SpriteDoc> doc, std::function<Result(SpriteDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<SpriteDoc> doc_;
    std::function<Result(SpriteDoc&)> fn_;
    Sprite before_;
};

}  // namespace wetwaresuite::forge
