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
| `tools/layout/` | print layout and publishing: posters, flyers, cards, booklets; linked text frames, styles, masters, CMYK, preflight, PDF with bleed and crop marks, N-up imposition |
| `tools/audio/` | audio editing on `libs/audio`: noise reduction, EQ, compression, loudness (LUFS), pitch and tempo, multitrack mixing with ducking, spectrograms, folder batches |
| `tools/cut/` | video editing: timeline of tracks and clips, trims, speed, picture-in-picture, transitions, titles, colour, music; one-shot trim/join/GIF/subtitles (via ffmpeg) |
| `tools/capture/` | screenshots of a screen, monitor or region, screen recordings with sound, webcam snapshots (via ffmpeg, grim on Wayland) |
| `tools/library/` | asset browser for every format: index folders, search by type, size, tags and ratings, duplicates, similar images, thumbnails and contact sheets, collections |
| `tools/automate/` | scripting across tools: batch scripts with variables, pipelines that loop over files, folder watching, and a Python client (`python/achclwss`) |
| `tools/office/` | office suite: spreadsheets with formulas and charts (XLSX/CSV), documents (DOCX/Markdown/PDF), slide decks (PPTX/PDF), PDF merge/split/rotate/text |
| `tools/convert/` | change a file's type: images and 3D meshes built in, audio/video via ffmpeg, docs via LibreOffice/pandoc |
| `tools/levels/` | game levels and tilemaps: tilesets, tile and object layers, painting, auto-tiling, scatter, cave/dungeon/noise generators, collision, Tiled TMX/TMJ, LDtk, JSON, CSV, PNG |
| `tools/sculpt/` | digital sculpting: brush strokes with symmetry and masks, subdivision, voxel remesh, quadric decimation, quad retopology, heightmap displacement, STL/OBJ/PLY/glTF out |
| `tools/beats/` | music making: step sequencer and piano roll, subtractive synth with presets, synthesized drum kit with sample slots, pitched sampler, arrangement by bar, effects, WAV/MP3 render, MIDI import/export, genre-based beat generator |
| `tools/animate/` | 2D and 3D animation: keyframes with easing, shapes, text, images, frame-by-frame cels with onion skin, bone rigs, 3D meshes and camera; PNG sequences, sprite sheets with JSON atlases, GIF, mp4/webm (via ffmpeg) |
| `tools/render/` | 3D scenes and renders: primitives and models with materials (metal, glass, glow, textures), lights and sky; a path tracer with denoising and a fast preview rasterizer; turntables to GIF/MP4 |
| `tools/board/` | whiteboards and diagrams: shapes, sticky notes, attached connectors (straight, elbow, curved), freehand ink, layers, themes incl. a sketchy look, auto-layout, Mermaid and outlines in, Excalidraw and draw.io in/out, PNG/SVG/PDF |
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
