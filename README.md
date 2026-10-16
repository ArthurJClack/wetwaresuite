# wetwaresuite


a lightweight, high-compatibility software suite: cad, blueprints, photo, paint, audio, video, game assets, office and a universal file toolkit, all on one shared core, installed tool by tool through the hub. every tool can be driven by claude over MCP.

- plan: https://claude.ai/code/artifact/5ec50df4-666c-485e-bf88-a79fefd12930
- how it fits together: [docs/architecture.md](docs/architecture.md)
- license: GPL-3.0 (see `LICENSE`)

## what works today (phase 1)

| piece | what it is |
| --- | --- |
| `core/` | the command system: typed commands, a registry, undo/redo history |
| `mcp/` | an MCP server that turns any tool's commands into MCP tools |
| `tools/toolkit/` | `achclwss-toolkit`: says what any file is and which tool opens it, hex dumps, MCP mode |
| `hub/` | `hub`: lists tools, prints the MCP config for Claude |

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
./build/tools/toolkit/achclwss-toolkit inspect photo.cr2
./build/tools/toolkit/achclwss-toolkit hex photo.cr2 0 64
./hub/target/release/hub list
./hub/target/release/hub mcp-config --bin-dir "$PWD/build/tools/toolkit"
```

paste the `mcp-config` output into Claude Desktop's config (or add it with `claude mcp add`) and Claude can use the toolkit directly.
