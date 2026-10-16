// asset forge: game assets as commands. Meshes (open, transform, decimate,
// normals, UVs, LOD chains, folder batches) on libs/mesh documents, and
// textures and 2D game art (sprite sheets and atlases, normal maps, tileable
// textures, pixel-art palettes, channel packing, trimming) on raster images.
#pragma once

#include "achclwss/core/command.hpp"

namespace achclwss::forge {

// Registers every forge.* command. Image commands work on images opened with
// photo.open (or on files directly, with input= and output=).
void registerCommands(Session& session);

}  // namespace achclwss::forge
