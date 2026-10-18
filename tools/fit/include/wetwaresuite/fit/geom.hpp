// Scan fit algorithms on libs/mesh meshes, free of the command layer so they
// can be tested and reused directly: reading phone scans, cleaning them up,
// fitting clean shapes (planes, cylinders, spheres, circles, curves) to noisy
// patches, and building printable parts that hug the scan with a set gap.
//
// Units are millimetres, Z is up. Nothing here trusts single triangles: every
// fit uses a patch of the scan with outliers thrown away, and every builder
// samples the scan on a grid, fills what's missing and smooths the result.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::fit {

using mesh::Bounds;
using mesh::Mat4;
using mesh::Mesh;
using mesh::Vec2;
using mesh::Vec3;

constexpr std::size_t kDefaultBudget = 300'000;  // scan triangles kept on open
constexpr std::size_t kMaxGridCells = 250'000;   // builder sample grid
constexpr int kMaxCloudResolution = 400;         // point cloud voxels along the longest side

// ---- spatial index ----

// Bounding volume hierarchy over a mesh's triangles, for ray casts and
// closest-point queries. Keeps the mesh alive through the shared pointer.
class Bvh {
public:
    Bvh() = default;
    explicit Bvh(std::shared_ptr<const Mesh> mesh);

    struct Hit {
        double t = 0;
        Vec3 point;
        std::size_t tri = 0;
    };
    // Nearest hit along origin + t * dir (dir unit length), 0 <= t <= maxT, both sides of triangles.
    std::optional<Hit> ray(const Vec3& origin, const Vec3& dir, double maxT = std::numeric_limits<double>::infinity()) const;
    // Every hit along the ray, nearest first.
    std::vector<Hit> rayAll(const Vec3& origin, const Vec3& dir, double maxT = std::numeric_limits<double>::infinity()) const;

    struct Nearest {
        Vec3 point;
        double distance = std::numeric_limits<double>::infinity();
        std::size_t tri = 0;
    };
    Nearest nearest(const Vec3& p) const;

    const Mesh& mesh() const { return *mesh_; }
    bool empty() const { return !mesh_ || mesh_->empty(); }

private:
    struct Node {
        Bounds box;
        std::uint32_t first = 0, count = 0;  // leaf: tris_[first .. first + count)
        std::uint32_t left = 0, right = 0;   // inner (count == 0)
    };
    std::shared_ptr<const Mesh> mesh_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> tris_;
};

// ---- small linear algebra ----

// Eigen decomposition of a symmetric 3x3 matrix (row-major). Values ascend;
// vectors[i] goes with values[i].
struct Eigen3 {
    double values[3];
    Vec3 vectors[3];
};
Eigen3 eigenSymmetric(const double m[9]);

// Rotation turning unit vector a onto unit vector b.
Mat4 rotationBetween(const Vec3& a, const Vec3& b);

// Right-handed frame: u x v = n.
struct Frame {
    Vec3 origin, u{1, 0, 0}, v{0, 1, 0}, n{0, 0, 1};
    Vec3 toWorld(double x, double y, double z = 0) const { return origin + u * x + v * y + n * z; }
    Vec3 toLocal(const Vec3& p) const {
        const Vec3 d = p - origin;
        return {mesh::dot(d, u), mesh::dot(d, v), mesh::dot(d, n)};
    }
    Mat4 matrix() const;  // local -> world
};
// Frame with normal n at origin; u is world X projected onto the plane (world Y
// when X is too close to n), turned by angleDegrees about n.
Frame frameFor(const Vec3& origin, const Vec3& n, double angleDegrees = 0);

// ---- reading scans ----

struct ScanOptions {
    std::string units = "auto";  // auto, mm, cm, m, in: what the file's numbers are
    std::string up = "auto";     // auto, y, z: which axis is up in the file
    std::size_t budget = kDefaultBudget;  // triangles; bigger scans are simplified (0 = never)
    double cloudCell = 0;        // point clouds: voxel size in mm (0 = automatic)
};

struct ScanInfo {
    double scale = 1;         // applied to reach millimetres
    std::string units;        // what the file was taken to be in
    bool yUp = false;         // rotated from Y-up to Z-up
    bool pointCloud = false;  // the file had points and no triangles
    std::size_t points = 0;
    std::size_t fileTriangles = 0;
    bool simplified = false;
};

// Point lists: one point per line, x y z first (spaces, commas, tabs or
// semicolons), anything after ignored; lines that don't start with three
// numbers (headers, comments) are skipped. For .xyz, .pts, .txt, .csv, .asc.
std::vector<Vec3> readPoints(const std::string& text);
bool isPointFile(const std::filesystem::path& path);
// Surface from a point cloud on a voxel grid of `cell` (occupied cells,
// closed and smoothed). Throws std::length_error past kMaxCloudResolution.
Mesh pointsToSurface(const std::vector<Vec3>& points, double cell);

// Loads a scan: meshes (stl obj ply off glb gltf 3mf) or point clouds, in mm and Z-up.
// Throws std::runtime_error with a readable reason.
Mesh loadScan(const std::filesystem::path& path, const ScanOptions& options, ScanInfo& info);
double unitScale(const std::string& units);  // mm per unit; 0 for unknown names

// ---- cleaning up ----

// Connected pieces (triangles sharing vertices), biggest area first; each is a list of triangles.
std::vector<std::vector<std::uint32_t>> pieces(const Mesh& m);
// Keeps the `keep` biggest pieces plus any at least `minFraction` of the
// biggest's area. Returns triangles removed.
std::size_t keepLargest(Mesh& m, int keep, double minFraction);
// Keeps triangles whose centre is inside the box (or outside, keepInside=false). Returns triangles removed.
std::size_t cropBox(Mesh& m, const Bounds& box, bool keepInside);
// Keeps triangles whose centre is on the side `normal` points to. Returns triangles removed.
std::size_t cropPlane(Mesh& m, const Vec3& point, const Vec3& normal);
// Drops triangles by index (and unused vertices).
void dropTriangles(Mesh& m, const std::vector<bool>& drop);

struct FloorResult {
    bool found = false;
    Vec3 point, normal;
    double area = 0;
    std::size_t removed = 0;
};
// Finds the lowest big near-horizontal flat area and removes it and
// everything under it. tolerance <= 0 picks one from the scan size.
FloorResult removeFloor(Mesh& m, double tolerance);

// Open boundary loops (vertex indices, in the winding of the triangles beside them).
std::vector<std::vector<std::uint32_t>> boundaryLoops(const Mesh& m);
struct HoleStats {
    std::size_t filled = 0, skipped = 0;
    double largestFilled = 0;  // estimated diameter
};
// Patches holes up to maxSize mm across (perimeter / pi) with a fan around their centre.
HoleStats fillHoles(Mesh& m, double maxSize);

// Typical distance of vertices from the average of their neighbours, a measure of scanner noise.
double noiseLevel(const Mesh& m);
double meanEdgeLength(const Mesh& m);

// ---- aligning ----

// Rotation (about the area centroid) that lines the scan's main directions up
// with X (longest), Y and Z, keeping Z on the side that was up.
Mat4 principalAlignment(const Mesh& m);

// ---- measuring ----

// Cross-section with a plane: polylines in world space. closed[i] says
// whether line i is a loop (its last point joins the first).
struct Section {
    std::vector<std::vector<Vec3>> lines;
    std::vector<bool> closed;
};
Section section(const Mesh& m, const Vec3& point, const Vec3& normal);
double polylineLength(const std::vector<Vec3>& line, bool closed);

// Vertices within `radius` of centre, with their (unsplit) normals.
struct Patch {
    std::vector<Vec3> points, normals;
};
Patch patchAround(const Mesh& m, const std::vector<Vec3>& vertexNormals, const Vec3& center, double radius);

// ---- fitting ----

struct PlaneFit {
    Vec3 point, normal;
    double rms = 0, maxDeviation = 0;
    std::size_t used = 0, total = 0;
};
// RANSAC, then least squares on the inliers. The normal faces the side the
// patch's triangles face. Throws std::runtime_error with fewer than 3 points.
PlaneFit fitPlane(const Patch& patch);

struct SphereFit {
    Vec3 center;
    double radius = 0, rms = 0;
    std::size_t used = 0, total = 0;
};
SphereFit fitSphere(const Patch& patch);

struct CylinderFit {
    Vec3 center, axis;  // center is the middle of the fitted length
    double radius = 0, length = 0, rms = 0;
    std::size_t used = 0, total = 0;
};
// Works from a partial view too (a pipe seen from one side).
CylinderFit fitCylinder(const Patch& patch);

struct CircleFit {
    Vec3 center, normal;
    double radius = 0, rms = 0;
    std::size_t used = 0, total = 0;
};
// Circle through 3D points: fits their plane (or uses `normal` when given), then a circle in it.
CircleFit fitCircle(const std::vector<Vec3>& points, const std::optional<Vec3>& normal = std::nullopt);

// Smooths a polyline (moving average, ends kept) and resamples it every `spacing`.
std::vector<Vec3> smoothCurve(const std::vector<Vec3>& line, int passes, double spacing);

// ---- building parts ----

// A 2D outline in a frame's (u, v) plane, millimetres. Rings are closed implicitly.
struct Footprint {
    std::vector<Vec2> outer;
    std::vector<std::vector<Vec2>> holes;
};
Footprint rectFootprint(double width, double height, double corner = 0, int cornerSegments = 8);
Footprint circleFootprint(double diameter, int segments = 96);
Footprint ovalFootprint(double width, double height, int segments = 96);
// Signed distance to the outline: negative inside.
double footprintDistance(const Footprint& f, const Vec2& p);

struct ConformSpec {
    Frame frame;          // origin on the scan, n pointing away from it (where the part goes)
    Footprint footprint;  // in the frame's u, v
    double gap = 0.4;         // clearance between the scan and the part
    double thickness = 3;     // plate: everywhere; block: at the thinnest point
    bool block = false;       // false: plate that follows the surface; true: flat top
    double tiltU = 0, tiltV = 0;  // block: tilt of the flat top, degrees about u and v
    double resolution = 0;    // grid spacing in mm (0 = automatic)
    int smoothing = 2;        // blur passes on the sampled surface
    double reach = 0;         // how far above/below the origin to look for the surface (0 = automatic)
    std::size_t targetTriangles = 8000;  // simplify the result to about this (0 = keep the grid)
};

struct ConformStats {
    std::size_t samples = 0, missed = 0;  // grid points, and those that didn't hit the scan (filled in)
    double minHeight = 0, maxHeight = 0;  // sampled surface relative to the frame, along n
    double resolution = 0;
    double maxThickness = 0;              // block: thickest point
};
// Builds a closed, outward-wound solid whose underside follows the scan at
// `gap`. Throws std::runtime_error when the footprint is mostly off the scan.
Mesh conformPart(const Bvh& scan, const ConformSpec& spec, ConformStats* stats = nullptr);

// Ring around an axis (local Z) from radius inner to outer, `width` long, centred on z = 0.
Mesh ringSolid(double inner, double outer, double width, int segments = 64);
// Cup along local +Z: outer radius, wall, depth, floor (0 = open tube).
Mesh cupSolid(double outerRadius, double wall, double depth, double floor, int segments = 64);

struct ClampSpec {
    Vec3 center, axis;
    double radius = 10;      // fitted cylinder radius
    double gap = 0.3, thickness = 4, width = 15;
    int pieces = 1;          // 1: one ring with a slot and a bolt; 2: two halves bolted together
    double boltDiameter = 4.5;  // clearance hole through the tabs (0 = no tabs)
    double slot = 2;         // pieces=1: width of the split
    double angle = 0;        // where the tabs point, degrees about the axis from the frame's u
};
// One mesh per piece, closed and outward-wound.
std::vector<Mesh> clampParts(const ClampSpec& spec);

// A strip following a curve: the curve offset by gap toward `side`, then
// `thickness` thick (in the section plane) and `width` wide (along the plane normal).
Mesh stripPart(const std::vector<Vec3>& curve, const Vec3& planeNormal, const Vec3& side, double gap, double thickness, double width);

// ---- standard mounts and holes ----

// Mount solids in a local frame: base at z = 0 (sunk 1 mm below for a clean
// union), growing along +Z.
Mesh ballMount(double ballDiameter = 17, double neckDiameter = 9, double neckLength = 8);
Mesh cameraMount(double bossDiameter = 22, double height = 9);  // 1/4"-20 nut trap
Mesh goproMount();                                              // 3-prong base
// Clearance diameter for a metric screw (M2 .. M10) and its 90 degree countersink head; 0 if unknown.
double screwClearance(const std::string& size);
double countersinkDiameter(const std::string& size);

// Closed: every edge has a partner going the other way (no holes). Booleans can
// leave edges shared by four triangles where two walls touch; slicers are fine
// with that, so this is the printability test rather than strict manifoldness.
bool isClosed(const Mesh& m);

// Boolean helpers that keep the result printable: when the boolean comes out
// broken, unite appends the two bodies (slicers merge overlapping bodies) and
// subtract leaves `a` as it was; `ok` says which happened.
Mesh unite(const Mesh& a, const Mesh& b, bool* ok = nullptr);
Mesh subtract(const Mesh& a, const Mesh& b, bool* ok = nullptr);

// ---- checking fit and printability ----

struct FitReport {
    double minGap = 0, maxGap = 0, meanGap = 0;  // over part vertices within `range` of the scan
    double contact = 0;    // fraction of those within gap + tolerance
    double inside = 0;     // fraction of those poking into the scan
    std::size_t near = 0, total = 0;
    std::vector<double> vertexGap;  // signed per part vertex (negative = inside the scan)
};
FitReport checkFit(const Bvh& scan, const Mesh& part, double range, double expectedGap);

struct PrintReport {
    bool watertight = false;  // closed (isClosed)
    double volume = 0, area = 0;
    Vec3 size;
    double thinnest = 0;  // wall thickness: 2nd percentile of inward rays (lone rays grazing edges don't count)
    Vec3 down;                        // suggested side to put on the bed
    double overhang = 0;              // fraction of area overhanging past 45 degrees that way
};
PrintReport checkPrint(const Mesh& part, const std::vector<Vec3>& candidateDowns = {});

// Per-triangle colour ramp for gap maps: inside purple, touching blue, far red.
struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
};
Rgb gapColor(double gap, double expectedGap);

}  // namespace wetwaresuite::fit
