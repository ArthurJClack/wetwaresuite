// Part-building helpers shared by cmd_parts.cpp and cmd_presets.cpp (internal to the fit tool).
#pragma once

#include <string>
#include <vector>

#include "internal.hpp"

namespace wetwaresuite::fit::detail {

// Frame for a part: at= snapped onto the scan with the surface direction
// fitted around it (or plane=), turned by angle=, flipped by flip=.
Frame placeOn(FitDoc& doc, const Params& p, double size, std::string* note = nullptr);
Footprint footprintOf(const Params& p);
double footprintSize(const Footprint& f);
// Builds a conforming plate or block from the params (outline, gap,
// thickness, tilt, smooth, resolution, test_slice) with the given outline,
// stores it, and returns "<name>\n<details>".
std::string buildConform(FitDoc& doc, const Params& p, bool block, const std::string& prefix, const Footprint* outline = nullptr,
                         const Frame* frame = nullptr);
std::string fitSummary(const FitDoc& doc, const Mesh& part, double gap);
Result partResult(const std::string& name, FitDoc& doc, const Params& p, const std::string& extra);
// Cuts a hole of diameter d into `part` at `at` going in along -n (depth 0 = through), with a countersink when head > d.
Mesh drillHole(const Mesh& part, const Vec3& at, const Vec3& n, double d, double depth, double head, bool* ok);

std::vector<ParamSpec> placementParams();
std::vector<ParamSpec> outlineParams();
std::vector<ParamSpec> buildParams(double thickness);

}  // namespace wetwaresuite::fit::detail
