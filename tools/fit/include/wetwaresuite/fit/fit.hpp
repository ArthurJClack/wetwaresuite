// fit: design car parts and mounts that fit a phone 3D scan. A fit document
// holds the scan (in mm, Z up), named reference shapes fitted to it (planes,
// cylinders, spheres, circles, curves, points) and the parts built on them.
// Every edit is recorded, so a project (.wwfit) replays from the scan file and
// fit.rebuild can change one part's gap or size and rebuild everything after it.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/fit/geom.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::fit {

enum class RefKind { Point, Plane, Cylinder, Sphere, Circle, Curve };
const char* refKindName(RefKind k);

// A clean shape fitted to the scan. point/dir mean: plane point + normal,
// cylinder centre + axis, sphere centre, circle centre + normal, curve plane normal.
struct Ref {
    RefKind kind = RefKind::Point;
    Vec3 point, dir;
    double radius = 0, length = 0, rms = 0;
    std::vector<Vec3> curve;
};

struct Part {
    std::shared_ptr<const Mesh> mesh;
    std::string kind;   // plate, block, clamp, insert, strip, combined, ...
    Vec3 normal;        // the direction away from the scan it was built along (zero if none)
    double gap = 0.4;   // the clearance it was built with, for fit checks
    std::size_t step = 0;  // index of the log entry that built it (what fit.rebuild changes)
};

struct FitState {
    std::shared_ptr<const Mesh> scan;
    std::shared_ptr<const Mesh> original;  // as opened, what replays start from
    std::uint64_t version = 0;             // changes whenever the scan does
    ScanInfo info;
    std::string scanPath;
    Params openParams;
    std::map<std::string, Ref> refs;
    std::map<std::string, Part> parts;
    std::vector<std::pair<std::string, Params>> log;  // edits since opening
    std::map<std::string, int> counters;               // for names like plane2
};

class FitDoc : public Document {
public:
    FitState state;

    std::string kind() const override { return "fit"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // 512x512 PNG of everything

    // Index over the current scan, rebuilt when the scan changes.
    const Bvh& bvh() const;

private:
    mutable std::shared_ptr<Bvh> bvh_;
    mutable std::uint64_t bvhVersion_ = ~0ull;
};

// What a picture shows.
struct SceneOptions {
    bool scan = true, parts = true, refs = true;
    std::string heatPart;  // colour this part by its gap to the scan
};
// The scene as one mesh with a colour per triangle (what previews draw and picks hit).
Mesh sceneMesh(const FitDoc& doc, const SceneOptions& o, std::vector<raster::Color>* colors);

// Registers every fit.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::fit
