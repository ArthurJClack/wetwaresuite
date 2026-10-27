// The open-mesh document cad, slicer and asset forge edit, plus the helpers
// their commands share: snapshot undo, the doc param, previews. Mirrors
// raster's ImageDoc so a tool can register a command in a few lines:
//
//   reg.add({"forge.flip", "Flip normals", "...", {mesh::docParam(), mesh::previewParam()}, true},
//           mesh::meshCommand(session, [](mesh::MeshDoc& d, const Params& p) {
//               mesh::flip(d.mesh);
//               return mesh::meshResult("flipped", d, p);
//           }));
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/mesh/io.hpp"
#include "wetwaresuite/mesh/mesh.hpp"
#include "wetwaresuite/mesh/render.hpp"

namespace wetwaresuite::mesh {

class MeshDoc : public Document {
public:
    Mesh mesh;
    RenderOptions previewOptions;  // how preview() draws it

    std::string kind() const override { return "mesh"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;  // 512x512 PNG
    bool renderView(const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba) const override;
    bool viewCamera(const OrbitView& view, int width, int height, ViewCamera& camera) const override;
};

// Draws a mesh from an orbit camera into rgba (how 3D documents implement renderView).
bool renderOrbit(const Mesh& mesh, const OrbitView& view, int width, int height, std::vector<std::uint8_t>& rgba,
                 RenderOptions options = {});
// The camera renderOrbit draws with (how 3D documents implement viewCamera), and the conversions.
ViewCamera orbitCamera(const Mesh& mesh, const OrbitView& view, int width, int height, RenderOptions options = {});
ViewCamera toViewCamera(const Projection& p);
Projection toProjection(const ViewCamera& c);

// One line: "12 triangles, 8 vertices, size 10 x 10 x 10 at (-5, -5, -5), watertight".
std::string describe(const Mesh& mesh);

std::shared_ptr<MeshDoc> newMesh(Mesh mesh = {});
std::shared_ptr<MeshDoc> openMesh(const std::filesystem::path& path);  // throws std::runtime_error
void saveMesh(const MeshDoc& doc, const std::filesystem::path& path, const SaveOptions& options = {});

// Undoable edit: snapshots the mesh before running `fn`, restores it on undo
// (and when fn fails).
class MeshEdit : public Action {
public:
    MeshEdit(std::shared_ptr<MeshDoc> doc, std::function<Result(MeshDoc&)> fn) : doc_(std::move(doc)), fn_(std::move(fn)) {}
    Result apply() override;
    void revert() override;

private:
    std::shared_ptr<MeshDoc> doc_;
    std::function<Result(MeshDoc&)> fn_;
    Mesh before_;
};

// Shared param specs.
ParamSpec docParam();      // "doc": id of an open mesh, e.g. mesh1
ParamSpec previewParam();  // "preview": attach a PNG render to the result
ParamSpec viewParam();     // "view": iso, front, back, left, right, top, bottom

// Action factory for a command that edits one mesh document ("doc" param).
// Runs `edit` inside a MeshEdit, or plainly when undoable is false; a missing
// doc becomes a failed result, and exceptions become failures too.
ActionFactory meshCommand(Session& session, std::function<Result(MeshDoc&, const Params&)> edit, bool undoable = true);

// Success result carrying doc, triangles, vertices and size fields, plus a
// preview when the "preview" param is true.
Result meshResult(const std::string& message, const MeshDoc& doc, const Params& p);
// Adds a preview attachment when the "preview" param is true (honours "view").
Result& maybePreview(Result& r, const MeshDoc& doc, const Params& p);

}  // namespace wetwaresuite::mesh
