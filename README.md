# ACHCLWSS

a lightweight, high-compatibility software suite: cad, blueprints, photo, paint, audio, video, game assets, office and a universal file toolkit, all on one shared core, installed tool by tool through the hub. every tool can be driven by claude over MCP.

- plan: https://claude.ai/code/artifact/5ec50df4-666c-485e-bf88-a79fefd12930
- how it fits together: [docs/architecture.md](docs/architecture.md)
- license: GPL-3.0 (see `LICENSE`)

## what works today (phases 1 to 3)

| piece | what it is |
| --- | --- |
| `core/` | the command system: typed commands, a registry, undo/redo, open documents, batch scripting |
| `mcp/` | an MCP server that turns any tool's commands into MCP tools, with image previews and resources |
| `libs/raster/` | the shared 2D image engine: layers, blend modes, brushes, filters, PNG/JPEG/BMP/TGA/PPM/OpenRaster |
| `libs/mesh/` | the shared 3D engine: meshes, primitives, CSG booleans, slicing, STL/OBJ/PLY/OFF/glTF/3MF, preview renders |
| `tools/toolkit/` | says what any file is and which tool opens it, hex dumps |
| `tools/photo/` | photo editing: open, resize, crop, rotate, adjust, levels, effects, batch a whole folder |
| `tools/paint/` | painting: pressure brushes, eraser, fills, shapes, gradients, layers |
| `tools/vector/` | vector design: shapes, paths, text, SVG in and out |
| `tools/blueprint/` | technical drafting and floor plans: walls, doors, dimensions, layers, DXF in/out, PDF/SVG to scale |
| `tools/cad/` | parametric CAD: feature tree, expressions, holes, patterns, booleans, measuring, STL/3MF export |
| `tools/slicer/` | 3D printing: printer/filament presets, perimeters, infill, supports, G-code with time and filament estimates |
| `tools/forge/` | game assets: mesh cleanup, LODs, UVs, sprite sheets and atlases, normal maps, tileable textures, pixel art |
| `tools/convert/` | change a file's type: images and 3D meshes built in, audio/video via ffmpeg, docs via LibreOffice/pandoc |
| `apps/achclwss/` | `achclwss`: one headless binary that runs every command, batch scripts, or an MCP server |
| `hub/` | `hub`: lists tools, prints the MCP config for Claude |

everything runs headless today. the GUI is a thin layer that calls the same commands and lands after the command set for each phase.

## build

needs CMake 3.21+, a C++20 compiler (gcc 11+, clang 14+, MSVC 2022) and, for the hub, Rust.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release

cd hub && cargo build --release
```

## try it

```sh
achclwss=./build/apps/achclwss/achclwss
$achclwss list
$achclwss help photo.resize
$achclwss convert.file input=song.wav output=song.mp3
$achclwss photo.batch input_dir=holiday output_dir=web max_size=1600 format=jpg
echo 'paint.new width=200 height=120 -> art
paint.stroke doc=$art points=10,60,190,60 brush=ink size=8
photo.save doc=$art path=line.png' | $achclwss batch
```

hook it up to Claude:

```sh
claude mcp add achclwss -- "$PWD/build/apps/achclwss/achclwss" mcp
```

or run `hub mcp-config --bin-dir "$PWD/build/apps/achclwss"` and paste the output into Claude Desktop's config.

## tutorials

step-by-step guides for every tool, covering the command line, batch scripts and Claude: [docs/tutorials](docs/tutorials/README.md).
