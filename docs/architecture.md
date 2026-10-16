# architecture

## layers

```
hub (Rust, standalone)        installs apps offline, makes offline editions, writes MCP config
share (Rust, standalone)      wetwaresuite-share: share/sync folders over LAN or internet, own MCP server
  │
apps/app                      wetwaresuite-<app>: one binary per app (image, draw, model, ...), its tools only
apps/wetwaresuite             wetwaresuite: every tool in one binary; wetwaresuite-gui launcher
apps/common                   app table, file routing, handoff folder, app.* commands, shared CLI
tools/<name> (C++)            each tool is a library of commands
gui/                          desktop window over a Session (ImGui + SDL3), linked into apps
  │
mcp/                          JSON-RPC over stdio, exposes a Session's commands
core/                         commands, registry, undo history, workspace, batch, processes
libs/raster                   shared 2D engine (stb for codecs, vendored)
libs/mesh                     shared 3D engine: meshes, primitives, CSG, slicing, mesh formats, preview renders
  │
external programs             ffmpeg, LibreOffice, pandoc: optional, found on PATH
```

## the rule: everything is a command

every action a tool can do is registered as a command (`CommandInfo` + a factory that builds an `Action`). the GUI, the CLI, scripts and MCP all go through `Session::run`, so:

- anything a person can click, claude can call
- every undoable change is one labeled step in the history
- parameters are validated in one place, with the same errors everywhere

read-only commands (inspect, export) set `undoable = false` and stay out of the history.

open files live in the session's `Workspace` as documents with short ids (`img1`, `vec1`). commands take `doc=img1`, so a script or Claude can juggle several files at once. image edits keep only what they changed for undo (see below).

### image undo and big canvases

`raster::ImageEdit` wraps every image edit. it never keeps whole-image snapshots for ordinary edits, so a 200-step history on a huge canvas stays small:

- **diff mode** (default, works for any edit): copy the layers, run the edit, compare 64x64 tiles (in parallel, with `memcmp`) and keep only the old tiles that differ; the full copy is dropped straight away. if the edit changed the canvas size, a layer's size or the number of layers (resize, crop, add/remove layer) the step keeps the whole old image instead.
- **declared mode** (brush strokes and erasing): the edit calls `ImageDoc::touch(layer, rect)` before writing pixels, using the same bounds the brush code clips to (`strokeBounds`), and only those rects are copied, with no full copy at all. overlapping touches are restored newest first, so the oldest copy wins.
- layer names, opacity, blend and visibility are always saved (they're tiny). undo restores the saved pixels and frees them; redo re-runs the edit, exactly as before. history length is still 200 steps.
- `raster::undoMemoryBytes()` reports what every live edit holds; a test paints 100 strokes on a 4000x4000 canvas and checks it stays under 3 MB (a whole-image snapshot would be 128 MB per step).

every edit also records the area it changed on the document (`ImageDoc::takeChanges()`). the GUI canvas keeps a mipmap pyramid of the flattened picture (`raster::Pyramid`: full size, half, quarter, ... down to 1 px); zoomed out it draws from the smallest level that still has a pixel per screen pixel (and fits the GPU's texture limit), and after an edit it re-flattens only the changed area, shrinks it down the chain and re-uploads just that part of the texture.

the heavy pixel loops (box blur, resize including Lanczos-3, layer blending, adjustments, the pyramid) run on `raster::parallelRows`, a small std::thread parallel-for over row slices. every row is computed by the same code whichever thread runs it and blur sums are exact integers, so output is identical on any thread count. the hottest loops (blending, box blur, bilinear and Lanczos resize, saturation, the pyramid) also have hand-written SIMD twins that hold one RGBA pixel per register (`libs/raster/src/simd_ops.hpp`: SSE2 on x86-64, NEON on arm64, both baseline so there's no runtime detection); they do the same float operations in the same order as the scalar loops and the library builds with `-ffp-contract=off`, so `raster::setSimdEnabled(false)` gives the same bytes and `test_raster` checks it. `photo.benchmark` times them (`compare=true` shows scalar vs SIMD).

## apps

people install apps, not tools. an app is a few related tools in one binary:

| app | tools |
| --- | --- |
| image | photo, paint |
| draw | vector, type, board |
| model | sculpt, render, animate |
| game | forge, levels |
| cad | cad, blueprint, slicer |
| electronics | circuit, flash |
| sound | audio, beats |
| video | cut, capture |
| office | office, layout, notes |
| maps | maps |
| system | calc, code, explorer, terminal, monitor, clock |
| files | toolkit, convert, library, automate (always installed) |
| share | the Rust `wetwaresuite-share` |

the table lives in `apps/common/apps.cpp` (C++) and `hub/src/catalog.rs` (Rust); `test_apps` fails if they disagree, and `test_suite` checks every tool is in an app.

- every `wetwaresuite-<app>` is built from `apps/app/main.cpp` with `WW_TOOL_<NAME>` defines, and links only its tools' libraries (image is ~2 MB, the all-in-one ~23 MB). apps don't carry the files tools; files is a separate small app installed alongside.
- CMake: `WETWARESUITE_APPS="image;model"` or `WETWARESUITE_APP_<NAME>=OFF` picks apps, `WETWARESUITE_BUILD_SUITE` the all-in-one; only the tools the chosen apps need (and the tools they link against) are added to the build. the app binaries go to `<build>/bin` so they find each other.
- every app has the same CLI as `wetwaresuite` (list, help, run, batch, mcp) plus the `app.*` commands; a ctest per app checks it lists only its own tools.

### handoff between apps

`app.send doc=<id> to=<app>` looks up a route for the document's kind and the target (`sendRoutes()` in `apps.cpp`: image->model/game as a PNG texture, sculpt->game as glb, sculpt/mesh->cad and cad->model as STL, vector/board->office as SVG, audio/song->video as WAV, ...), runs that tool's own save command into the per-user handoff folder, and appends an entry to `handoff/manifest.json` (from, to, kind, file, the command to run next, the target's open command, a hint). `app.receive` in the target picks the newest entry for it and runs the open command; `app.inbox` lists them; `app.open_with` maps a file extension to an app and its open command; `app.installed` looks for app binaries next to itself, in `<data>/bin` and on PATH. in the all-in-one binary the target's tools are in the same session, so `app.send` opens the result right away. all of it goes through `Session::run`.

the per-user folder is `$WETWARESUITE_HOME`, else `$XDG_DATA_HOME/wetwaresuite` (~/.local/share), `~/Library/Application Support/wetwaresuite` on macOS, `%APPDATA%\wetwaresuite` on windows; it holds `bin/` (hub installs), `handoff/` and `packs/`. hub and the apps compute it the same way.

for AI, each app's MCP instructions name the other installed apps (and the missing ones) and explain app_send / app_receive, so claude can move work across separate MCP servers.

## installs and offline editions

`hub install <app>...` copies binaries from a local folder or archive (`--from`, default: the folder hub is in); it never downloads. `.tar` is read natively, other archives go through the system `tar`. tool names are aliases (`hub install paint` = image) and `files` always comes along.

`hub bundle --size <GB>|auto --out <dir>` makes an offline edition: the app binaries, tutorials and content packs (`hub/src/packs.rs`: fonts, CC0 samples made by the apps themselves, CC0 materials in 1K/2K/4K tiers, CC0 sounds, ODbL map tiles as PMTiles in zoom tiers up to the full planet). editions are powers of two in GB; each holds the required packs plus optional packs by priority, skipping what doesn't fit, with a bigger tier replacing the smaller ones. `auto` starts at 32 GB and doubles or halves to the smallest edition that holds the request. bundling may download; `hub install --from <edition>` is fully offline.

## headless

`wetwaresuite` runs any command (`wetwaresuite photo.resize doc=... width=800`), a batch script (one command per line, `-> name` captures a result, `$name` reuses it, `undo`/`redo`/`echo` builtins), or an MCP server. every feature works without a screen, so it runs on servers, in CI and from cron.

## MCP

`mcp::Server` maps command `toolkit.inspect` to MCP tool `toolkit_inspect` and builds the input schema from the command's params (types, enums, defaults, arrays). it adds `undo`, `redo`, `history` and `documents`, returns `structuredContent` for every result, attaches PNG previews as image content when a command is called with `preview=true`, and lists open documents as resources (`wetwaresuite://doc/img1`). `wetwaresuite mcp` serves every tool in one connection; `--tools photo,paint` narrows it. start-up stays small without lazy loading: registering all ~700 commands is ~4 ms, so the cost that mattered was `tools/list`. it's written straight to text (`mcp::toolsListJson`, byte for byte what nlohmann's `dump()` gives; `test_suite` compares the two over every real command) and cached until the registry's `revision()` changes, and `tools/call` finds its command through a name map instead of copying the registry. `--version` and `mcp --tools` skip the full registration. each `wetwaresuite-<app> mcp` serves just that app.

## staying light

- the core has no dependencies; libraries get pulled in by the tool that needs them
- image codecs are vendored single-header stb; heavy formats go through ffmpeg/LibreOffice/pandoc only if installed
- the GUI (`gui/`: Dear ImGui on SDL3's 2D renderer, shared by every app) is a thin layer over the commands: forms are generated from each command's params, and it falls back to software drawing without a GPU. every app binary links it: no arguments (or `open`/`gui`, or only existing files) opens the window, anything else is the CLI (`apps::decideLaunch`); the window hands work between apps only through `app.targets`/`app.send`/`app.inbox`/`app.receive`/`app.open_with`/`app.launch`
- the hub is a dependency-free Rust binary (~300 KB) so it runs before anything else is installed
- each app binary links only its own tools, so installing image doesn't pull in video or cad code
- `share` is a dependency-free Rust binary too (~1 MB): networking is all in Rust's std (TCP, UDP), so there's no per-OS socket code; it's a separate binary with its own `mcp` because it doesn't need the C++ core, and `hub mcp-config` emits an entry for it

## phases

1. foundation: core, MCP, hub, toolkit, CI on windows/mac/linux
2. 2D creative: photo, paint, vector, convert on a shared raster engine, one headless `wetwaresuite` binary
3. precision and 3D: cad, blueprint, slicer, asset forge on a shared mesh engine (`libs/mesh`)
4. media, office and the rest: audio (`libs/audio`), cut, capture, office (`libs/pdf`), layout (`libs/pdfdoc`), library, automate (plus a Python client in `python/`)
5. 3D art and games: sculpt (shared `libs/mesh` topology helpers), render, animate, levels, beats
6. making and organising: circuit, flash, maps (Natural Earth countries built in), board, notes, type
