# Optional container for wetwaresuite: the whole suite, served to an MCP client
# over HTTP. Nothing in wetwaresuite needs Docker; this is just one more way to
# run it (a server, a NAS, a locked-down machine). See docs/tutorials/docker.md.
#
#   docker build -t wetwaresuite .
#   docker run --rm -p 127.0.0.1:8765:8765 -v "$PWD:/work" -e WETWARESUITE_TOKEN=change-me wetwaresuite
#
# Building needs the internet once (base images and the pinned nlohmann/json
# source); the image itself never goes online.

# ---- C++ suite ---------------------------------------------------------------
FROM debian:trixie-slim AS cpp
RUN apt-get update \
 && apt-get install -y --no-install-recommends cmake g++ make ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DWETWARESUITE_BUILD_TESTS=OFF \
 && cmake --build /build --parallel 4 \
 && cmake --install /build --prefix /out --strip

# ---- Rust hub and share (no crates.io dependencies, so no downloads) ---------
FROM rust:1.87-slim-bookworm AS rust
WORKDIR /src
COPY hub hub
COPY share share
RUN cargo build --release --locked --manifest-path hub/Cargo.toml \
 && cargo build --release --locked --manifest-path share/Cargo.toml \
 && mkdir -p /out/bin \
 && cp hub/target/release/hub share/target/release/wetwaresuite-share /out/bin/

# ---- runtime: just the programs ----------------------------------------------
FROM debian:trixie-slim
RUN useradd --create-home --uid 10001 --shell /usr/sbin/nologin wetware \
 && mkdir -p /work \
 && chown wetware:wetware /work
COPY --from=cpp /out/bin/ /usr/local/bin/
COPY --from=rust /out/bin/ /usr/local/bin/
USER wetware
WORKDIR /work
VOLUME ["/work"]
EXPOSE 8765
# Set WETWARESUITE_TOKEN to choose the bearer token clients must send; leave it
# empty and the server makes one up and prints it at startup.
ENV WETWARESUITE_TOKEN=""
CMD ["wetwaresuite", "mcp", "--http", "--host", "0.0.0.0", "--port", "8765"]
