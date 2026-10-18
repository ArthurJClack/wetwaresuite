# Dear ImGui (vendored)

- version: v1.92.3 (commit bf75bfec48fc00f532af8926130b70c0e26eb099), https://github.com/ocornut/imgui
- license: MIT, see LICENSE.txt (GPL-3.0 compatible)
- kept: the core sources, `misc/cpp/imgui_stdlib.*`, and the SDL3 + SDL_Renderer backends
  (`backends/imgui_impl_sdl3.*`, `backends/imgui_impl_sdlrenderer3.*`). Demo, examples and docs are left out.
- `fonts/Roboto-Medium.ttf` comes from imgui's `misc/fonts`: Roboto by Christian Robertson,
  Apache License 2.0 (GPL-3.0 compatible, text in `fonts/LICENSE-Roboto.txt`). It is embedded into the GUI at build time.

To update: copy the same files from a newer tag and change the version above. No local edits.
