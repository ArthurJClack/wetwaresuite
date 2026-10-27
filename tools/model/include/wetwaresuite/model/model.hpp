// model: polygon modeling the way Blender does it, headless. A model
// document is a scene of objects (meshes, empties, lights, cameras) with
// transforms, parents, collections, material slots, keyframes and a stack of
// non-destructive modifiers on each mesh object. Mesh objects point at a mesh
// (a PolyMesh, see poly.hpp), so linked duplicates share one mesh.
//
// Everything is a model.* command, and every edit is one undo step: the
// scene is snapshotted, and meshes are copied only when an edit changes
// them (they're shared between snapshots otherwise).
//
// A model document is also a libs/mesh document: its `mesh` is the whole
// scene evaluated (modifiers applied, in world space), so app.view, render.add
// mesh=, slicer.load mesh=, forge and the other mesh tools read it directly.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/model/ops.hpp"

namespace wetwaresuite::model {

// ---- scene contents ----

struct Material {
    std::string name;
    Vec3 color{0.8, 0.8, 0.8};  // base colour, sRGB 0..1
    double roughness = 0.5;
    double metallic = 0;
    Vec3 emission{0, 0, 0};     // sRGB 0..1
    double emissionStrength = 0;
    double transmission = 0;    // 1 = glass
    double ior = 1.45;
    double alpha = 1;
    std::string texture;        // image multiplied into the colour, through the faces' UVs
    double textureScale = 1;
};

const std::vector<std::string>& modifierTypes();  // mirror array subsurf solidify bevel boolean decimate smooth displace weld triangulate
struct Modifier {
    std::string type;
    std::string name;
    bool enabled = true;
    std::map<std::string, double> num;        // numeric settings (see modifierTypes and the model.modifier_add help)
    std::map<std::string, std::string> text;  // text settings: axis, object, operation, texture, mode
    double get(const std::string& key, double fallback) const;
    std::string str(const std::string& key, const std::string& fallback) const;
};
// Default settings for a modifier type (empty type when unknown).
Modifier makeModifier(const std::string& type);

struct Keyframe {
    int frame = 1;
    double value = 0;
    std::string interp = "bezier";  // bezier (ease in and out), linear, constant
};
// Keyable channels: location.x .. scale.z.
const std::vector<std::string>& channelNames();

struct Object {
    std::string name;
    std::string type = "mesh";  // mesh, empty, light, camera
    std::string data;           // mesh objects: the mesh they show (shared by linked duplicates)
    std::string parent;
    std::string collection = "Collection";
    Vec3 location{0, 0, 0};
    Vec3 rotation{0, 0, 0};     // degrees about X, then Y, then Z
    Vec3 scale{1, 1, 1};
    bool visible = true;
    bool selected = false;
    std::vector<std::string> materials;  // slots: names in the scene's materials
    std::vector<Modifier> modifiers;
    double autoSmooth = 180;  // smooth faces split their normals where sharper than this (degrees)
    std::map<std::string, std::vector<Keyframe>> keys;
    // lights and cameras
    std::string lightType = "point";  // point, sun, area
    Vec3 lightColor{1, 1, 1};
    double power = 1000;  // point/area watts-ish; sun strength
    double size = 0.1;    // point radius, area width, sun angle
    double fov = 40;      // camera vertical field of view

    Mat4 local() const;  // translate * rotate * scale
};

struct Collection {
    std::string name;
    bool visible = true;
};

struct Scene {
    std::map<std::string, std::shared_ptr<const PolyMesh>> meshes;
    std::vector<Object> objects;
    std::vector<Material> materials;
    std::vector<Collection> collections{{"Collection", true}};
    std::string active;
    SelectMode selectMode = SelectMode::Vertex;
    bool editMode = false;
    Vec3 cursor{0, 0, 0};
    int frame = 1, frameStart = 1, frameEnd = 48;
    double fps = 24;
};

// One object evaluated: modifiers applied, triangulated, in world space.
struct Evaluated {
    Triangles tris;               // mesh in world space
    std::vector<std::string> errors;  // modifiers that couldn't run
    std::size_t faces = 0;        // polygons after the modifiers
};

class ModelDoc : public mesh::MeshDoc {
public:
    Scene scene;

    std::string kind() const override { return "model"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;
    bool renderView(const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba) const override;
    bool viewCamera(const OrbitView& view, int width, int height, ViewCamera& camera) const override;

    Object* object(const std::string& name);
    const Object* object(const std::string& name) const;
    Object* activeObject() { return object(scene.active); }
    const Object* activeObject() const { return object(scene.active); }
    const Material* material(const std::string& name) const;
    // Object-to-world through the parent chain.
    Mat4 world(const Object& o) const;
    bool shown(const Object& o) const;  // visible and in a visible collection
    const PolyMesh* meshOf(const Object& o) const;
    // A private copy of the object's mesh to edit (other objects sharing it see the change).
    PolyMesh& editMesh(Object& o);
    std::string uniqueName(const std::string& base) const;      // objects
    std::string uniqueMeshName(const std::string& base) const;  // meshes

    // Re-evaluates what changed into `mesh` (the whole scene in world space).
    void refresh();
    const Evaluated& evaluated(const std::string& object) const;  // after refresh()
    // per triangle of `mesh`: the object index, and the polygon in that object's evaluated mesh
    std::vector<int> triObject;
    std::vector<std::uint32_t> triFace;

private:
    struct CacheEntry {
        std::string stamp;
        std::shared_ptr<const Evaluated> value;
        std::vector<std::shared_ptr<const PolyMesh>> inputs;  // kept alive so their addresses stay unique
    };
    mutable std::map<std::string, CacheEntry> cache_;
};

// The mesh with modifiers applied, in object space. Problems go in errors.
PolyMesh evaluate(const ModelDoc& doc, const Object& o, std::vector<std::string>* errors = nullptr);
// One modifier run on `m` (object space). Throws std::runtime_error when it can't.
PolyMesh applyModifier(const ModelDoc& doc, const Object& o, const PolyMesh& m, const Modifier& mod);
// The default scene: a 2 m cube called Cube, a light and a camera.
std::shared_ptr<ModelDoc> newModel(bool withCube = true);
// Adds a mesh object (its own new mesh) and makes it active; returns its name.
std::string addMeshObject(ModelDoc& doc, const std::string& name, PolyMesh mesh);

// ---- the view (shared by renderView, picking and box selection) ----

// Projects world points the way the orbit renderer draws them.
struct Projector {
    Vec3 right, up, forward;
    Vec3 centre;
    double radius = 1;
    double scale = 1, cx = 0, cy = 0;
    int width = 0, height = 0;
    Vec2 toScreen(const Vec3& p) const;  // pixels, y down
    double depth(const Vec3& p) const { return mesh::dot(p, forward); }
    Vec3 ray(double x, double y) const;  // a point on the near side under pixel (x, y)
};
Projector projector(const mesh::Mesh& scene, const OrbitView& view, int width, int height);
// Orbit angles for a named view (iso front back left right top bottom).
bool presetView(const std::string& name, double& yaw, double& pitch);

// What's under each pixel of a view: depth, object index (-1 = nothing) and
// polygon. With cage=true (edit mode) the active object is its editable mesh
// (faces are its polygons); otherwise faces index the evaluated meshes.
struct ViewBuffer {
    Projector proj;
    std::vector<float> depth;
    std::vector<int> object;
    std::vector<int> face;
    bool visible(const Vec3& world, double slack = 0) const;  // not hidden behind a surface
    int at(int x, int y) const { return (x < 0 || y < 0 || x >= proj.width || y >= proj.height) ? -1 : y * proj.width + x; }
};
ViewBuffer viewBuffer(const ModelDoc& doc, const OrbitView& view, int width, int height, bool cage);

// ---- files ----

// .wwmodel is JSON: objects, meshes (polygons, UVs, seams), materials, keys.
std::string toJson(const ModelDoc& doc);
void fromJson(ModelDoc& doc, const std::string& json);  // throws std::runtime_error
void saveModel(ModelDoc& doc, const std::filesystem::path& path);
std::shared_ptr<ModelDoc> loadModel(const std::filesystem::path& path);

struct ExportOptions {
    bool modifiers = true;      // apply them
    bool selectedOnly = false;  // only selected objects
    std::vector<std::string> objects;  // only these (empty = all shown)
    bool ascii = false;
};
// OBJ keeps polygons, UVs, smoothing, object names and materials (with a .mtl
// beside it); the other mesh formats get the triangulated scene.
// Returns a note on what was written.
std::string exportFile(const ModelDoc& doc, const std::filesystem::path& path, const ExportOptions& o = {});
// Adds the objects in a file: .wwmodel, OBJ (polygons, UVs, groups and
// materials kept) or any format libs/mesh reads (triangles; quads=true joins
// them back into quads). Returns the names of the new objects.
std::vector<std::string> importFile(ModelDoc& doc, const std::filesystem::path& path, bool quads = true);

// ---- animation ----

double channelValue(const Object& o, const std::string& channel);
void setChannel(Object& o, const std::string& channel, double v);
// The channel's keyed value at a frame (fallback when it has no keys).
double channelAt(const Object& o, const std::string& channel, double frame, double fallback);
// Moves every keyed object to where its keys put it at `frame`.
void applyFrame(ModelDoc& doc, int frame);

// ---- undo ----

// Snapshots the scene, runs the edit, refreshes; restores on failure and undo.
class ModelEdit : public Action {
public:
    ModelEdit(std::shared_ptr<ModelDoc> doc, std::function<Result(ModelDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<ModelDoc> doc_;
    std::function<Result(ModelDoc&)> fn_;
    Scene before_;
};

// Registers every model.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::model
