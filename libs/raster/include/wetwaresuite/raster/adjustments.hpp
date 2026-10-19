// Photoshop-style adjustments (levels, curves, hue/saturation, colour balance, black & white,
// exposure, vibrance, gradient map, invert, posterize, threshold, selective colour, channel
// mixer, photo filter, brightness/contrast). The same code runs for an adjustment layer (while
// compositing, so nothing below is changed for good) and for a one-off edit of a layer's pixels.
// Settings are text key/value pairs so they live in a layer's settings and in .ora files.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

using Settings = std::map<std::string, std::string>;

// One setting an adjustment type reads.
struct AdjustmentField {
    enum class Kind { Number, List, Bool, Color, Colors, Choice };
    std::string key;
    Kind kind = Kind::Number;
    std::string description;
    double min = 0, max = 0;  // Number: the useful range
    std::string fallback;      // default, as text
    std::vector<std::string> choices;  // Choice
};

// "levels", "curves", "hue_saturation", ... in the order of Photoshop's adjustment menu.
const std::vector<std::string>& adjustmentLayerTypes();
// The settings `type` reads (empty for an unknown type).
const std::vector<AdjustmentField>& adjustmentFields(const std::string& type);
// Every key any type reads, each once, for building command params.
const std::vector<AdjustmentField>& allAdjustmentFields();
// A readable reason when a setting is out of range or malformed; empty when they're fine.
std::string checkAdjustment(const std::string& type, const Settings& settings);
// Changes RGB (alpha is kept). Unknown types do nothing.
void applyAdjustment(Image& img, const std::string& type, const Settings& settings);

// Selective colour keeps one set of cyan/magenta/yellow/black per colour range, stored as
// "<range>.cyan" etc. These are the ranges.
const std::vector<std::string>& selectiveColorRanges();

// Helpers for reading settings.
double settingNumber(const Settings& s, const std::string& key, double fallback);
bool settingBool(const Settings& s, const std::string& key, bool fallback);
std::vector<double> settingList(const Settings& s, const std::string& key);
// Colours separated by commas or spaces ("#000,#f00, rgb(0,0,255)"); false on a bad one.
bool parseColorList(const std::string& text, std::vector<Color>& out);

// The ready-made tone curves, as (input, output) points: contrast, strong_contrast, brighten,
// darken, fade, invert. Empty for an unknown name.
std::vector<std::pair<double, double>> curvePreset(const std::string& name);

}  // namespace wetwaresuite::raster
