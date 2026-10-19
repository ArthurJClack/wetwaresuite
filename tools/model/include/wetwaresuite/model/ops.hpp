// Polygon modeling operations on a PolyMesh: primitives, selection tools and
// the edit-mode operators (extrude, inset, bevel, loop cut, bisect, knife,
// subdivide, merge, dissolve, delete, bridge, fill, spin, mirror, normals...),
// plus Catmull-Clark subdivision and UV projections. Free of the command
// layer, so they're tested and reused directly.
//
// Operators read the selection flags and leave the result selected (new faces
// for extrude, inset and bevel, the new loop for loop cut, ...). All of them
// keep the mesh valid (see valid()) and, given a manifold mesh, manifold.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/model/poly.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::model {

// ---- primitives (Blender's defaults: 2 units across, centred, Z up) ----

struct PrimitiveOptions {
    double size = 2;          // cube edge, plane/grid side
    double radius = 1;        // circle, spheres, cylinder, cone bottom
    double radius2 = 0;       // cone top (0 = a point)
    double depth = 2;         // cylinder and cone height
    int vertices = 32;        // circle, cylinder and cone sides; UV sphere segments
    int rings = 16;           // UV sphere rings
    int subdivisions = 2;     // ico sphere
    int xSubdivisions = 10;   // grid
    int ySubdivisions = 10;
    double majorRadius = 1;   // torus
    double minorRadius = 0.25;
    int majorSegments = 48;
    int minorSegments = 12;
    std::string fill = "ngon";  // circle, cylinder and cone caps: ngon, fan or none
};
const std::vector<std::string>& primitiveNames();  // cube plane grid circle uv_sphere ico_sphere cylinder cone torus
// With UVs (except the ico sphere). Throws std::invalid_argument for bad sizes or counts.
PolyMesh primitive(const std::string& type, const PrimitiveOptions& o = {});

// ---- selection ----

// Edge loop through `e` (stops at poles: vertices that don't have four edges),
// or the loop along the boundary when `e` is a boundary edge.
std::vector<EdgeKey> edgeLoop(const PolyMesh& m, const Topology& t, EdgeKey e);
// Edge ring: across quads, the opposite edge, both ways from `e`.
std::vector<EdgeKey> edgeRing(const PolyMesh& m, const Topology& t, EdgeKey e);
// Everything connected to the selection (through edges; seams stop it when delimit is on).
void selectLinked(PolyMesh& m, SelectMode mode, bool delimitSeams = false);
// steps > 0 grows the selection by that many rings of neighbours, < 0 shrinks it.
void growSelection(PolyMesh& m, SelectMode mode, int steps);
// Faces whose normal is within `degrees` of dir.
std::size_t selectByNormal(PolyMesh& m, const Vec3& dir, double degrees);
// Boundary and non-manifold edges.
std::size_t selectNonManifold(PolyMesh& m);
// Edges whose faces meet at a sharper angle than `degrees`.
std::size_t selectSharpEdges(PolyMesh& m, double degrees);
std::size_t selectRandom(PolyMesh& m, SelectMode mode, double ratio, unsigned seed);
std::size_t selectMaterial(PolyMesh& m, int slot);

// ---- extrude, inset, bevel ----

enum class ExtrudeMode { Region, Individual };
// Extrudes selected faces (region: one piece per connected area, moved along
// its average normal; individual: every face along its own normal) by
// `offset`, or by `direction` when given. Without selected faces it extrudes
// selected edges into quads and lone selected vertices into edges, moved by
// `direction` (default: offset along the neighbouring faces' normal, or +Z).
// Returns the number of new faces (or edges for vertices).
std::size_t extrude(PolyMesh& m, ExtrudeMode mode, double offset, const std::optional<Vec3>& direction = std::nullopt);
// Insets selected faces (as one region or each face) by `thickness`, then
// moves the inner faces `depth` along the normal. Returns new faces.
std::size_t inset(PolyMesh& m, double thickness, double depth, bool individual);
// Bevels selected edges (both faces needed) with `segments` rounded steps, or
// selected vertices when vertexOnly. width is the offset along each face.
// Returns new faces.
std::size_t bevel(PolyMesh& m, double width, int segments, bool vertexOnly);

// ---- cutting ----

// Loop cut: `cuts` new edge loops across the ring of quads through edge e
// (factor places a single cut along it, 0..1). The new loops end up selected.
std::size_t loopCut(PolyMesh& m, EdgeKey e, int cuts, double factor = 0.5);
// Cuts faces with the plane through `point` facing `normal`. clearInner
// deletes what's behind the plane (opposite to normal), clearOuter what's in
// front; fill caps the cut where a side was cleared. Only faces with
// filter[f] set are cut when filter is given. Returns new edges along the cut.
std::size_t bisect(PolyMesh& m, const Vec3& point, const Vec3& normal, bool clearInner, bool clearOuter, bool fill,
                   const std::vector<char>* filter = nullptr);
// Knife: cuts along the line from a to b as seen looking along `view`
// (cuts the faces facing the viewer that the line crosses; through=true cuts
// the back faces too). Returns new edges.
std::size_t knife(PolyMesh& m, const Vec3& a, const Vec3& b, const Vec3& view, bool through, bool selectedOnly);
// Splits each selected face: quads into a (cuts+1)^2 grid, triangles into
// (cuts+1)^2 triangles, n-gons into one face per corner around a centre point.
std::size_t subdivide(PolyMesh& m, int cuts);

// ---- merging, dissolving, deleting ----

enum class MergeMode { Distance, Center, First, Last, Cursor, Collapse };
std::optional<MergeMode> parseMergeMode(const std::string& s);
// Distance merges selected vertices closer than `distance` (all vertices when
// allVerts); the others merge the selection into one vertex (collapse: one
// per connected piece). Returns how many vertices were removed.
std::size_t merge(PolyMesh& m, MergeMode mode, double distance, const Vec3& cursor = {}, bool allVerts = false);
// Removes selected edges and joins the faces on both sides; removes selected
// vertices, joining their faces; joins each connected area of selected faces
// into one n-gon. Leftover vertices with only two edges go too when clean is
// on. Returns how many elements were dissolved.
std::size_t dissolveEdges(PolyMesh& m, bool clean = true);
std::size_t dissolveVerts(PolyMesh& m);
std::size_t dissolveFaces(PolyMesh& m, bool clean = true);
// Dissolves edges between faces flatter than `degrees` (Blender's limited dissolve).
std::size_t limitedDissolve(PolyMesh& m, double degrees, bool selectedOnly);
enum class DeleteMode { Verts, Edges, Faces, OnlyFaces };
std::optional<DeleteMode> parseDeleteMode(const std::string& s);  // verts edges faces only_faces
std::size_t remove(PolyMesh& m, DeleteMode mode);

// ---- building ----

// Bridges two (or more) selected edge loops or open chains with quads. With
// selected faces, their areas are removed and their outlines bridged.
// Loops need the same number of edges. twist rotates the pairing.
// Returns new faces; throws std::runtime_error explaining a bad selection.
std::size_t bridge(PolyMesh& m, int twist = 0);
// Makes a face from the selection: from a closed loop of selected edges, or
// selected vertices in order around their middle; two vertices make an edge.
std::size_t fill(PolyMesh& m);
// Caps holes (boundary loops) with one face each; only fully selected loops when selectedOnly.
std::size_t fillHoles(PolyMesh& m, bool selectedOnly);
// Spins the selection around an axis through `center`: `steps` copies over
// `degrees`, joined by faces (a profile of edges becomes a surface of
// revolution; faces become a solid). 360 degrees closes up.
std::size_t spin(PolyMesh& m, const Vec3& axis, const Vec3& center, double degrees, int steps);
// Copies the selection (everything when nothing is selected) mirrored across
// the plane through center, perpendicular to axis 0/1/2; vertices within
// mergeDistance of the plane are shared. duplicate=false mirrors in place.
std::size_t mirror(PolyMesh& m, int axis, const Vec3& center, bool duplicate, double mergeDistance);
// Copies the selected faces and loose edges; the copy ends up selected.
std::size_t duplicate(PolyMesh& m);

// ---- normals, shading, shape ----

// Makes neighbouring faces wind the same way, then points closed pieces out
// (inside=true points them in). Returns faces flipped.
std::size_t recalcNormals(PolyMesh& m, bool inside, bool selectedOnly);
std::size_t flipNormals(PolyMesh& m, bool selectedOnly);
std::size_t triangulateFaces(PolyMesh& m, bool selectedOnly);
// Joins pairs of triangles into quads where they're flatter than `degrees`.
std::size_t trisToQuads(PolyMesh& m, double degrees, bool selectedOnly);
// Laplacian smoothing of the selected vertices (boundaries slide along themselves).
std::size_t smoothVertices(PolyMesh& m, double factor, int iterations, bool selectedOnly);
// Applies t to the selected vertices; within `proportional` (> 0) of the
// selection, unselected vertices follow with a smooth falloff.
std::size_t transformSelected(PolyMesh& m, const Mat4& t, double proportional = 0);
// Moves selected vertices along their normals (shrink/fatten).
std::size_t shrinkFatten(PolyMesh& m, double offset);

// ---- subdivision surfaces ----

// Catmull-Clark: every face becomes quads, `levels` times. Boundary edges and
// sharp edges stay crisp. Throws std::length_error past maxFaces.
PolyMesh catmullClark(const PolyMesh& m, int levels, std::size_t maxFaces = 2'000'000);

// ---- UVs ----

enum class UvMethod { Smart, Cube, Cylinder, Sphere, Planar, Unwrap };
std::optional<UvMethod> parseUvMethod(const std::string& s);  // smart cube cylinder sphere planar unwrap
struct UvOptions {
    UvMethod method = UvMethod::Smart;
    double angle = 66;     // smart: faces closer than this to a projection direction share it
    double margin = 0.02;  // gap between islands when packing
    int axis = 2;          // planar, cylinder and sphere: the projection axis
    double scale = 1;      // cube/planar: UV units per model unit (before packing)
    bool pack = true;      // fit the islands into 0..1
    bool selectedOnly = false;
};
// Writes face UVs. Unwrap flattens each island cut out by seams (least-squares
// conformal maps); islands without a boundary fall back to smart projection.
// Returns the number of islands.
std::size_t unwrap(PolyMesh& m, const UvOptions& o);
// Packs the existing UV islands into 0..1 (keeping their relative sizes).
std::size_t packIslands(PolyMesh& m, double margin, bool selectedOnly);
// Seams along sharp edges (and boundaries) for unwrap.
std::size_t seamsFromSharp(PolyMesh& m, double degrees);
// The UV layout as a picture: islands filled light, edges drawn dark.
raster::Image uvLayout(const PolyMesh& m, int size, bool selectedOnly);

}  // namespace wetwaresuite::model
