# architecture

## layers

```
hub (Rust, standalone)        installs tools, writes MCP config
  │
apps/achclwss                 one headless binary: run, batch, mcp over every tool
tools/<name> (C++)            each tool is a library of commands (+ GUI later)
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

open files live in the session's `Workspace` as documents with short ids (`img1`, `vec1`). commands take `doc=img1`, so a script or Claude can juggle several files at once. image edits snapshot the document for undo.

## headless

`achclwss` runs any command (`achclwss photo.resize doc=... width=800`), a batch script (one command per line, `-> name` captures a result, `$name` reuses it, `undo`/`redo`/`echo` builtins), or an MCP server. every feature works without a screen, so it runs on servers, in CI and from cron.

## MCP

`mcp::Server` maps command `toolkit.inspect` to MCP tool `toolkit_inspect` and builds the input schema from the command's params (types, enums, defaults, arrays). it adds `undo`, `redo`, `history` and `documents`, returns `structuredContent` for every result, attaches PNG previews as image content when a command is called with `preview=true`, and lists open documents as resources (`achclwss://doc/img1`). `achclwss mcp` serves every tool in one connection; `--tools photo,paint` narrows it.

## staying light

- the core has no dependencies; libraries get pulled in by the tool that needs them
- image codecs are vendored single-header stb; heavy formats go through ffmpeg/LibreOffice/pandoc only if installed
- the GUI (Qt, shared by every tool) is a thin layer over the commands
- the hub is a dependency-free Rust binary (~300 KB) so it runs before anything else is installed

## phases

1. foundation: core, MCP, hub, toolkit, CI on windows/mac/linux
2. 2D creative: photo, paint, vector, convert on a shared raster engine, one headless `achclwss` binary
3. precision and 3D: cad, blueprint, slicer, asset forge on a shared mesh engine (`libs/mesh`)
4. media, office and the rest: audio (`libs/audio`), cut, capture, office (`libs/pdf`), layout (`libs/pdfdoc`), library, automate (plus a Python client in `python/`)
