# wetwaresuite

an all-in-one, lightweight creative/office suite. plan and decisions: docs/architecture.md and the plan doc linked in README.md.

- C++20 + CMake for core, mcp and every tool; Rust (no dependencies) for `hub/` and `share/` (std networking keeps it portable)
- every user action is a command in `core` (`CommandRegistry` + `Action`); new features add commands, never bypass `Session::run`
- every tool binary supports `<tool> mcp` and should register all its commands so Claude can drive it
- speed and ease of use win over quality when they conflict; keep dependencies minimal and per tool
- license is GPL-3.0; only add dependencies with GPL-3.0-compatible licenses

checks before pushing:

```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
cd hub && cargo test && cargo clippy -- -D warnings
cd share && cargo test && cargo clippy --all-targets -- -D warnings
```
