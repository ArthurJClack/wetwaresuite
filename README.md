<picture>
  <source media="(prefers-color-scheme: dark)" srcset="branding/logo-on-dark.svg">
  <img alt="wetwaresuite" src="branding/logo-on-light.svg" width="420">
</picture>

_the all-in-one lightweight software suite_

a lightweight, high-compatibility software suite: cad, blueprints, photo, paint, audio, video, game assets, office and a universal file toolkit, all on one shared core. it comes as small standalone apps, so you install only what you need, and they hand work to each other (a texture from image onto a model, a model into a game). everything works offline with no AI connected; claude can drive every app over MCP if you want.

- plan: https://claude.ai/code/artifact/5ec50df4-666c-485e-bf88-a79fefd12930
- how it fits together: [docs/architecture.md](docs/architecture.md)
- license: GPL-3.0 (see `LICENSE`)

## the apps

install only what you need (`hub install image model --from <release folder>`); each is one small program with the same command line. see [docs/tutorials/apps.md](docs/tutorials/apps.md).

| app | program | for | tools |
| --- | --- | --- | --- |
| image | `wetwaresuite-image` | photos, painting, textures | photo, paint |
| draw | `wetwaresuite-draw` | vector art, fonts, whiteboards | vector, type, board |
| model | `wetwaresuite-model` | 3D modeling, renders, animation | sculpt, render, animate |
| game | `wetwaresuite-game` | game assets and levels | forge, levels |
| cad | `wetwaresuite-cad` | engineering, drafting, 3D printing, parts that fit 3D scans | cad, blueprint, slicer, fit |
| electronics | `wetwaresuite-electronics` | circuits and microcontrollers | circuit, flash |
| sound | `wetwaresuite-sound` | audio editing and music | audio, beats |
| video | `wetwaresuite-video` | video editing and screen capture | cut, capture |
| office | `wetwaresuite-office` | documents, sheets, slides, print layout, notes | office, layout, notes |
| maps | `wetwaresuite-maps` | offline maps and GIS | maps |
| files | `wetwaresuite-files` | open, convert, find anything (always installed) | toolkit, convert, library, automate |
| share | `wetwaresuite-share` | share and sync folders | share |

`wetwaresuite` is every app in one binary, for power users, scripts and claude. apps pass work along with `app.send` / `app.receive` through a handoff folder ([docs/tutorials/workflows.md](docs/tutorials/workflows.md)), and [offline editions](docs/tutorials/offline-editions.md) put everything plus fonts, materials and world maps on a USB stick.

## what works today (phases 1 to 6)

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
| `tools/cad/` | parametric CAD: feature tree, expressions, holes, patterns, booleans, fillets, chamfers, shells, measuring, STEP import/export, STL/3MF export |
| `tools/slicer/` | 3D printing: printer/filament presets, perimeters, infill, supports, G-code with time and filament estimates |
| `tools/forge/` | game assets: mesh cleanup, LODs, UVs, sprite sheets and atlases, normal maps, tileable textures, pixel art |
| `tools/layout/` | print layout and publishing: posters, flyers, cards, booklets; linked text frames, styles, masters, CMYK, preflight, PDF with bleed and crop marks, N-up imposition |
| `tools/audio/` | audio editing on `libs/audio`: noise reduction, EQ, compression, loudness (LUFS), pitch and tempo, multitrack mixing with ducking, spectrograms, folder batches |
| `tools/cut/` | video editing: timeline of tracks and clips, trims, speed, picture-in-picture, transitions, titles, colour, music; one-shot trim/join/GIF/subtitles (via ffmpeg) |
| `tools/capture/` | screenshots of a screen, monitor or region, screen recordings with sound, webcam snapshots (via ffmpeg, grim on Wayland) |
| `tools/library/` | asset browser for every format: index folders, search by type, size, tags and ratings, duplicates, similar images, thumbnails and contact sheets, collections |
| `tools/automate/` | scripting across tools: batch scripts with variables, pipelines that loop over files, folder watching, and a Python client (`python/wetwaresuite`) |
| `tools/office/` | office suite: spreadsheets with formulas and charts (XLSX/CSV), documents (DOCX/Markdown/PDF), slide decks (PPTX/PDF), PDF merge/split/rotate/text |
| `tools/convert/` | change a file's type: images and 3D meshes built in, audio/video via ffmpeg, docs via LibreOffice/pandoc |
| `tools/levels/` | game levels and tilemaps: tilesets, tile and object layers, painting, auto-tiling, scatter, cave/dungeon/noise generators, collision, Tiled TMX/TMJ, LDtk, JSON, CSV, PNG |
| `tools/fit/` | scan fit: phone 3D scans (OBJ/PLY/STL/glTF/points, metres and Y-up handled) cleaned up, robust plane/cylinder/circle/curve references, parts that hug the scan at a set gap (plates, blocks, clamps, strips, phone mounts, gauge pods, switch panels, speaker rings), gap checks, test slices, replayable `.wwfit` projects |
| `tools/sculpt/` | digital sculpting: brush strokes with symmetry and masks, subdivision, voxel remesh, quadric decimation, quad retopology, heightmap displacement, STL/OBJ/PLY/glTF out |
| `tools/beats/` | music making: step sequencer and piano roll, subtractive synth with presets, synthesized drum kit with sample slots, pitched sampler, arrangement by bar, effects, WAV/MP3 render, MIDI import/export, genre-based beat generator |
| `tools/animate/` | 2D and 3D animation: keyframes with easing, shapes, text, images, frame-by-frame cels with onion skin, bone rigs, 3D meshes and camera; PNG sequences, sprite sheets with JSON atlases, GIF, mp4/webm (via ffmpeg) |
| `tools/render/` | 3D scenes and renders: primitives and models with materials (metal, glass, glow, textures), lights and sky; a path tracer with denoising and a fast preview rasterizer; turntables to GIF/MP4 |
| `tools/circuit/` | electronics: schematics from a built-in symbol library (passives, semiconductors, op-amps, regulators, Arduino/ESP32/Blue Pill boards) with ERC, netlists and BOMs; two-layer PCBs with autorouting, ground pours, DRC and Gerber/Excellon output; KiCad netlist import; DC and transient simulation |
| `tools/flash/` | flash firmware to Arduino (AVR), ESP32/ESP8266 and STM32 headless: list and identify serial ports, detect boards, inspect HEX/BIN/ELF/ESP images, native STK500v1/v2, AVR109, ESP ROM and STM32 USART bootloaders (no external tools) with avrdude/esptool/stm32prog fallbacks, serial monitor and send |
| `tools/board/` | whiteboards and diagrams: shapes, sticky notes, attached connectors (straight, elbow, curved), freehand ink, layers, themes incl. a sketchy look, auto-layout, Mermaid and outlines in, Excalidraw and draw.io in/out, PNG/SVG/PDF |
| `tools/notes/` | markdown notes and knowledge base: Obsidian-compatible vaults, wikilinks with rename-safe rewriting, backlinks, tags, frontmatter, templates, daily notes, tasks with due dates, ranked full-text search, link graph (JSON/DOT/PNG/SVG), HTML/PDF render and static site export |
| `tools/maps/` | offline maps and lightweight GIS: GeoJSON/KML/GPX/CSV/Shapefile in and out, GeoTIFF and world-file rasters, WGS84/Web Mercator/UTM, geodesic measuring, buffer/simplify/clip/filter/joins, heatmaps, GPX track stats, styled PNG/SVG/PDF maps over built-in Natural Earth countries or OSM tiles |
| `tools/type/` | font design: glyphs from shapes, SVG path data, SVG files or traced handwriting; components, transforms, oblique, embolden, overlap removal; auto-spacing and kerning; TTF/OTF/WOFF/UFO in, TTF/WOFF/SVG font out; proofs as PNG and PDF |
| `apps/app/` | `wetwaresuite-<app>`: one small binary per app (image, draw, model...), built from one shared main; each links only its own tools |
| `apps/common/` | what every app shares: the app table, which app opens which file, the handoff folder and the `app.*` commands |
| `apps/wetwaresuite/` | `wetwaresuite`: every tool in one binary (the window with no arguments; commands, batch scripts or an MCP server with them), plus the `wetwaresuite-gui` launcher |
| `share/` | `wetwaresuite-share`: share a folder with any browser (link, QR, code), live-sync code between machines, send files; LAN or internet |
| `hub/` | `hub`: lists and installs apps offline from a folder or archive, makes offline editions (1-256 GB), prints the MCP config for Claude |

every app opens its window when double-clicked (Dear ImGui on SDL3, works offline and without a GPU) and stays a headless CLI when given arguments. the window is a thin layer that calls the same commands, including "Send to..." and the handoff inbox between apps.

## build

needs CMake 3.21+, a C++20 compiler (gcc 11+, clang 14+, MSVC 2022) and, for the hub, Rust.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release
# the apps are in build/bin/, the all-in-one binary in build/apps/wetwaresuite/
# build only some apps: -DWETWARESUITE_APPS="image;model" -DWETWARESUITE_BUILD_SUITE=OFF

(cd hub && cargo build --release)
(cd share && cargo build --release)
```

## try it

```sh
wetwaresuite=./build/apps/wetwaresuite/wetwaresuite
$wetwaresuite list
$wetwaresuite help photo.resize
$wetwaresuite convert.file input=song.wav output=song.mp3
$wetwaresuite photo.batch input_dir=holiday output_dir=web max_size=1600 format=jpg
echo 'paint.new width=200 height=120 -> art
paint.stroke doc=$art points=10,60,190,60 brush=ink size=8
photo.save doc=$art path=line.png' | $wetwaresuite batch
```

or use just one app, and hand its work to another:

```sh
printf 'paint.new width=256 height=256 -> t\napp.send doc=$t to=model name=crate\n' | ./build/bin/wetwaresuite-image batch
./build/bin/wetwaresuite-model app.receive
```

hook it up to Claude:

```sh
claude mcp add wetwaresuite -- "$PWD/build/apps/wetwaresuite/wetwaresuite" mcp
```

share and sync folders (see [docs/tutorials/share.md](docs/tutorials/share.md)):

```sh
cd share && cargo build --release
./target/release/wetwaresuite-share serve ~/some/folder      # prints a link, QR code and pairing code
claude mcp add wetwaresuite-share -- "$PWD/target/release/wetwaresuite-share" mcp
```

or run `hub mcp-config --bin-dir "$PWD/build/apps/wetwaresuite"` and paste the output into Claude Desktop's config.

## tutorials

step-by-step guides for every tool, covering the command line, batch scripts and Claude: [docs/tutorials](docs/tutorials/README.md).
