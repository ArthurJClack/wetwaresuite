// An editable polygon mesh for box modeling, the way Blender's edit mode
// keeps one: vertices, faces of any size (quads and n-gons stay as they are),
// loose edges (edges with no face, for profiles to spin or extrude), and
// selection flags on vertices, edges and faces.
//
// Faces are vertex loops wound counter-clockwise seen from outside. Edges
// aren't stored: an edge is a pair of vertex indices (EdgeKey), and the
// connectivity (which faces meet at an edge, which edges meet at a vertex,
// half-edges and their twins) is rebuilt on demand by Topology. That keeps
// every operation a plain rewrite of the face list, so a half-done edit can
// never leave dangling pointers behind, at O(n log n) per operation.
//
// Every function here works in the mesh's own (object) coordinates and on
// the current selection. None of them throw for an empty selection; they
// just do nothing and say so through their return value.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "wetwaresuite/mesh/math.hpp"
#include "wetwaresuite/mesh/mesh.hpp"

namespace wetwaresuite::model {

using mesh::Mat4;
using mesh::Vec2;
using mesh::Vec3;

// ---- edges ----

using EdgeKey = std::uint64_t;  // smaller vertex index in the high half
inline EdgeKey edgeKey(std::uint32_t a, std::uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<EdgeKey>(a) << 32) | b;
}
inline std::uint32_t keyA(EdgeKey k) { return static_cast<std::uint32_t>(k >> 32); }
inline std::uint32_t keyB(EdgeKey k) { return static_cast<std::uint32_t>(k & 0xffffffffu); }

// ---- the mesh ----

struct Vertex {
    Vec3 p;
    bool sel = false;
    Vec3 color{1, 1, 1};  // vertex colour (sRGB 0..1), used when PolyMesh::colors is on
};

struct Face {
    std::vector<std::uint32_t> v;  // vertex loop, counter-clockwise from outside
    std::vector<Vec2> uv;          // empty, or one per corner (so seams are free)
    int material = 0;              // the object's material slot
    bool smooth = false;           // smooth shading (false = flat)
    bool sel = false;
};

enum class SelectMode { Vertex, Edge, Face };
const char* toString(SelectMode m);
std::optional<SelectMode> parseSelectMode(const std::string& s);  // vert/vertex, edge, face

struct PolyMesh {
    std::vector<Vertex> verts;
    std::vector<Face> faces;
    std::vector<std::array<std::uint32_t, 2>> loose;  // edges that belong to no face
    std::set<EdgeKey> esel;   // selected edges
    std::set<EdgeKey> seams;  // UV seams
    std::set<EdgeKey> sharp;  // hard edges (flat shading across them, bevel modifier limit)
    bool colors = false;      // vertex colours are in use

    std::uint32_t addVertex(const Vec3& p) {
        verts.push_back({p});
        return static_cast<std::uint32_t>(verts.size() - 1);
    }
    std::size_t addFace(std::vector<std::uint32_t> v, int material = 0, bool smooth = false) {
        Face f;
        f.v = std::move(v);
        f.material = material;
        f.smooth = smooth;
        faces.push_back(std::move(f));
        return faces.size() - 1;
    }
    bool hasUvs() const;
};

// ---- connectivity, rebuilt per operation ----

struct HalfRef {
    std::uint32_t face = 0, corner = 0;  // face.v[corner] -> face.v[corner + 1]
};

struct Topology {
    std::vector<EdgeKey> edges;                         // sorted; face edges and loose edges
    std::unordered_map<EdgeKey, std::uint32_t> index;   // edge -> position in edges
    std::vector<std::vector<HalfRef>> edgeFaces;        // per edge: the faces using it
    std::vector<std::vector<std::uint32_t>> vertFaces;  // per vertex: faces using it
    std::vector<std::vector<std::uint32_t>> vertEdges;  // per vertex: edge indices

    explicit Topology(const PolyMesh& m);
    int edge(std::uint32_t a, std::uint32_t b) const;  // index, or -1
    std::size_t faceCount(EdgeKey k) const;            // faces on an edge (0 for loose)
    bool boundary(EdgeKey k) const { return faceCount(k) == 1; }
};

// Every edge as an EdgeKey, sorted (the order edge indices refer to).
std::vector<EdgeKey> allEdges(const PolyMesh& m);

// ---- geometry ----

Vec3 faceNormal(const PolyMesh& m, const Face& f);  // unit (Newell), zero when degenerate
Vec3 faceCenter(const PolyMesh& m, const Face& f);
double faceArea(const PolyMesh& m, const Face& f);
mesh::Bounds bounds(const PolyMesh& m);
// Area-weighted unit normal per vertex, never split.
std::vector<Vec3> vertexNormals(const PolyMesh& m);

// ---- selection ----

// Makes the other two element kinds follow `mode`'s selection, the way
// Blender does: vertex mode selects the edges and faces whose vertices are all
// selected; edge mode selects the vertices of selected edges and faces whose
// edges are all selected; face mode selects the edges and vertices of selected faces.
void flushSelection(PolyMesh& m, SelectMode mode);
void selectAll(PolyMesh& m, bool on);
void invertSelection(PolyMesh& m, SelectMode mode);
std::vector<std::uint32_t> selectedVerts(const PolyMesh& m);
std::vector<std::uint32_t> selectedFaces(const PolyMesh& m);
std::vector<EdgeKey> selectedEdges(const PolyMesh& m);  // sorted
struct SelectionCount {
    std::size_t verts = 0, edges = 0, faces = 0;
};
SelectionCount countSelection(const PolyMesh& m);

// ---- clean-up ----

// Drops repeated corners, faces with fewer than three distinct corners,
// loose edges that are degenerate, doubled or also a face edge, and vertices
// nothing uses; remaps selection, seams and sharp edges.
void cleanup(PolyMesh& m);
// Removes the given vertices (and every face and loose edge that uses one).
void removeVertices(PolyMesh& m, const std::vector<char>& kill);
// Removes faces by flag; their vertices go too when nothing else uses them.
void removeFaces(PolyMesh& m, const std::vector<char>& kill);

// ---- checks ----

struct Stats {
    std::size_t verts = 0, edges = 0, faces = 0;
    std::size_t tris = 0, quads = 0, ngons = 0, triangles = 0;  // triangles = after triangulating
    std::size_t looseEdges = 0, boundaryEdges = 0, nonManifoldEdges = 0;  // > 2 faces
    bool manifold = false;    // no edge with more than two faces, no flipped neighbours
    bool closed = false;      // manifold with no boundary edges
    bool consistent = false;  // neighbouring faces wind the same way
    long long euler = 0;      // V - E + F
};
Stats stats(const PolyMesh& m);
// Structural sanity: indices in range, faces of 3+ distinct corners, uv sizes.
bool valid(const PolyMesh& m, std::string* why = nullptr);

// ---- triangles in and out ----

struct TriangulateOptions {
    double autoSmooth = 180;  // smooth faces split their normals where they meet at a sharper angle
    bool uvs = true;
};
struct Triangles {
    mesh::Mesh mesh;                    // with normals, and uvs when the faces have them
    std::vector<std::uint32_t> face;    // per triangle: the polygon it came from
    std::vector<int> material;          // per triangle: material slot
    std::vector<Vec3> color;            // per vertex of `mesh`, when colours are on
};
// Ear-clips each face in its own plane, so concave n-gons triangulate right.
Triangles triangulate(const PolyMesh& m, const TriangulateOptions& o = {});
// Triangle indices (into f.v) for one face.
std::vector<std::uint32_t> triangulateFace(const PolyMesh& m, const Face& f);
// Welds the triangles by exact position (keeping uvs per corner).
PolyMesh fromTriangles(const mesh::Mesh& tris);

// Applies a transform to every vertex; mirroring transforms flip the winding.
void transform(PolyMesh& m, const Mat4& t);
// Appends src (transformed by t) into dst; returns the first new vertex index.
std::uint32_t append(PolyMesh& dst, const PolyMesh& src, const Mat4& t = Mat4::identity(), int materialOffset = 0);
// Copies the selected faces (and loose edges with both ends selected) into a new mesh.
PolyMesh extractSelected(const PolyMesh& m);
// Splits into connected pieces (vertices joined by any edge).
std::vector<PolyMesh> looseParts(const PolyMesh& m);

}  // namespace wetwaresuite::model
