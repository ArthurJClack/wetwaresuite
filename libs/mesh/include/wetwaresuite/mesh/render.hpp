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

struct RenderOptions {
    View view = View::Iso;
    raster::Color background{246, 247, 249, 255};  // alpha 0 gives a transparent background
    raster::Color color{122, 158, 204, 255};       // surface colour
    bool smooth = true;     // smooth normals across shallow edges; false = flat facets
    bool outline = true;    // dark lines on silhouettes and sharp edges
    bool antialias = true;  // render at 2x and downsample
    double margin = 0.06;   // empty border, as a fraction of the image size
};

raster::Image render(const Mesh& mesh, int width, int height, const RenderOptions& options = {});

}  // namespace wetwaresuite::mesh
