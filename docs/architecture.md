# architecture

## layers

```
hub (Rust, standalone)        installs tools, writes MCP config
  │
tools/<name> (C++, Qt UI later)   one executable per tool: CLI, MCP mode, GUI
  │
mcp/                          JSON-RPC over stdio, exposes a Session's commands
core/                         commands, registry, undo history
  │
shared libraries              FFmpeg, libvips, OpenCASCADE, Assimp, ... (added per phase)
```

## the rule: everything is a command

every action a tool can do is registered as a command (`CommandInfo` + a factory that builds an `Action`). the GUI, the CLI, scripts and MCP all go through `Session::run`, so:

- anything a person can click, claude can call
- every undoable change is one labeled step in the history
- parameters are validated in one place, with the same errors everywhere

read-only commands (inspect, export) set `undoable = false` and stay out of the history.

## MCP

`mcp::Server` maps command `toolkit.inspect` to MCP tool `toolkit_inspect`, builds the input schema from the command's params, and adds `undo`, `redo` and `history`. every tool binary runs it with `<tool> mcp`. later the hub will run a gateway that merges all installed tools into one connection.

## staying light

- the core has no dependencies; libraries get pulled in by the tool that needs them
- Qt arrives with the first GUI (phase 2) and is shared by every tool after that
- the hub is a dependency-free Rust binary (~300 KB) so it runs before anything else is installed

## phases

1. foundation: core, MCP, hub, toolkit, CI on windows/mac/linux (this)
2. 2D creative: photo, paint, vector on a shared canvas engine
3. precision and 3D: cad, blueprint, slicer, asset forge
4. media, office and the rest
