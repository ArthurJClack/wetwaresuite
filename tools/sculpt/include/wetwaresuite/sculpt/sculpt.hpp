// sculpt: digital sculpting and retopology as commands. Sculpt documents are
// libs/mesh documents (SculptDoc is a MeshDoc), so forge, convert and the
// other mesh tools work on them directly, and sculpt's commands work on any
// open mesh (forge.mesh_open, cad parts) too.
#pragma once

#include <memory>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/mesh/document.hpp"
#include "wetwaresuite/sculpt/ops.hpp"

namespace wetwaresuite::sculpt {

class SculptDoc : public mesh::MeshDoc {
public:
    Mask mask;           // per vertex, empty = nothing masked
    bool quads = false;  // triangles 2k and 2k+1 form quad k (after remesh/retopo); OBJ export writes quads

    std::string kind() const override { return "sculpt"; }
    std::string summary() const override;
};

// Registers every sculpt.* command.
void registerCommands(Session& session);

}  // namespace wetwaresuite::sculpt
