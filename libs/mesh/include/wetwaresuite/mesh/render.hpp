// Software preview renderer: orthographic, z-buffered, lambert-shaded, with
// optional crease/silhouette outlines and 2x supersampling. The camera
// auto-fits the mesh. Used for MCP previews and thumbnails, so it favours
// speed and a clean CAD look over realism.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::mesh {

// Iso looks from the front-right-top; front looks along +Y, right along -X, top down -Z.
enum class View { Iso, Front, Back, Left, Right, Top, Bottom };

const std::vector<std::string>& viewNames();  // iso front back left right top bottom ("side" parses as right)
std::optional<View> parseView(const std::string& name);

// A free camera for orbiting: yaw turns around the vertical (Z) axis, pitch tilts
// up and down (degrees). yaw 0 pitch 0 is the front view; yaw 45 pitch 35.26 is iso.
// The model's bounding sphere is fitted, so its size stays put while orbiting.
struct Orbit {
    double yaw = 45;
    double pitch = 35.26;
    double zoom = 1;  // 2 = twice as big
    double panX = 0;  // shift in model radii, right and up on screen
    double panY = 0;
};

struct RenderOptions {
    View view = View::Iso;
    std::optional<Orbit> orbit;  // overrides view
    raster::Color background{246, 247, 249, 255};  // alpha 0 gives a transparent background
    raster::Color color{122, 158, 204, 255};       // surface colour
    bool smooth = true;     // smooth normals across shallow edges; false = flat facets
    bool outline = true;    // dark lines on silhouettes and sharp edges
    bool antialias = true;  // render at 2x and downsample
    double margin = 0.06;   // empty border, as a fraction of the image size
    // one colour per triangle (overrides `color`), for highlighting parts or
    // gap maps; ignored unless it has exactly triangleCount() entries
    std::vector<raster::Color> faceColors;
};

raster::Image render(const Mesh& mesh, int width, int height, const RenderOptions& options = {});

}  // namespace wetwaresuite::mesh
