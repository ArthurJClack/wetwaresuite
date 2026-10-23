// slicer: turn a 3D model into Marlin G-code for an FDM printer. The model
// sits on the bed in bed coordinates (origin at the front-left corner, Z up,
// millimetres). Every user action is a `slicer.*` command; the functions
// below are the engine those commands (and tests) use.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/raster/image.hpp"
#include "wetwaresuite/slicer/geometry.hpp"

namespace wetwaresuite::slicer {

void registerCommands(Session& session);

struct Printer {
    std::string name = "ender3";
    double bedX = 220, bedY = 220, bedZ = 250;
    double nozzle = 0.4;
    // Marlin G-code with {placeholders}: nozzle_temp, bed_temp, first_layer_temp, bed_x, bed_y, park_z.
    std::string startGcode, endGcode;
};

struct Filament {
    std::string name = "pla";
    double nozzleTemp = 205, firstLayerTemp = 210, bedTemp = 60;
    double fan = 100;          // percent, from the second layer on
    double density = 1.24;     // g/cm3
    double diameter = 1.75;    // mm
    double maxSpeed = 0;       // mm/s cap for flexible filaments, 0 = none
};

struct Settings {
    std::string quality = "normal";
    double layerHeight = 0.2, firstLayerHeight = 0.2;
    double lineWidth = 0;  // 0 = nozzle x 1.125
    int perimeters = 2, topLayers = 4, bottomLayers = 4;
    double infill = 20;  // percent
    std::string infillPattern = "grid";  // lines, grid, triangles, cubic, gyroid, concentric
    double speed = 50, firstLayerSpeed = 20, travelSpeed = 150;  // mm/s
    double retraction = 0.8, retractionSpeed = 35, retractMinTravel = 1.5;
    double zHop = 0;  // mm the nozzle lifts on retracted travels
    int skirt = 1;
    double skirtDistance = 5, brim = 0;  // mm
    std::string supports = "none";       // none, everywhere, buildplate
    std::string supportStyle = "columns";  // columns (straight down) or tree (branches that merge)
    double supportAngle = 50, supportDensity = 15;
    std::string seam = "nearest";  // nearest, rear, random
};

struct Config {
    Printer printer;
    Filament filament;
    Settings settings;
    double lineWidth() const { return settings.lineWidth > 0 ? settings.lineWidth : printer.nozzle * 1.125; }
};

// Presets. Names are lowercase; unknown names give nullopt / false.
const std::vector<std::string>& printerNames();   // ender3 prusa_mk4 bambu_a1 custom
const std::vector<std::string>& filamentNames();  // pla petg abs tpu
const std::vector<std::string>& qualityNames();   // draft normal fine
std::optional<Printer> printerPreset(const std::string& name);
std::optional<Filament> filamentPreset(const std::string& name);
bool applyQuality(Settings& settings, const std::string& name);

// Placement. All keep the model's shape; they only move it.
void dropToBed(mesh::Mesh& m);                                // min z = 0
void centerOnBed(mesh::Mesh& m, const Printer& printer);      // XY centre at the bed centre, dropped
void moveCenterTo(mesh::Mesh& m, double x, double y);         // XY centre at (x, y), dropped
// Rotates so the largest flat outside face points down. Returns false if nothing changed.
bool layFlat(mesh::Mesh& m);

struct Fit {
    bool fits = true;
    std::string reason;  // empty when it fits
};
Fit checkFit(const mesh::Mesh& m, const Printer& printer);

enum class PathType { Skirt, Brim, OuterWall, InnerWall, SolidInfill, Infill, Support };
const char* toString(PathType type);

// Packs parts (their XY footprints, as bounding boxes) onto the bed in rows,
// `spacing` mm apart, the whole group centred. Returns the new centre of
// each part, or nullopt when they don't all fit.
std::optional<std::vector<Vec2>> arrange(const std::vector<mesh::Bounds>& parts, const Printer& printer, double spacing);

struct Path {
    PathType type = PathType::OuterWall;
    std::vector<Vec2> points;
    bool closed = false;  // a loop: extrude back to the first point
};

struct Layer {
    double z = 0;       // top of the layer (where the nozzle is)
    double height = 0;  // thickness
    std::vector<Path> paths;  // in print order
};

struct SliceResult {
    std::vector<Layer> layers;
    std::vector<std::string> warnings;
};

// Slices a mesh that is already placed on the bed. Throws std::runtime_error
// for settings that can't work (e.g. a layer height of 0).
SliceResult slice(const mesh::Mesh& placed, const Config& config);

struct Gcode {
    std::string text;
    double seconds = 0;        // accel-free estimate
    double filamentMm = 0;     // length of filament pushed
    double filamentGrams = 0;
    double extrudedVolume = 0; // mm3
};
Gcode toGcode(const SliceResult& sliced, const Config& config, const std::string& modelName = {});

// Top-down picture of one layer's toolpaths, coloured by type, with travel
// moves as thin lines. `wholeBed` frames the bed instead of the part.
raster::Image renderLayer(const SliceResult& sliced, std::size_t layer, const Config& config, int size, bool travel = true,
                          bool wholeBed = false);

}  // namespace wetwaresuite::slicer
