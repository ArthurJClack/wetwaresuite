# Working offline

wetwaresuite is built to work with **no internet and no AI connected**. Every tool runs on your computer, on your files, and nothing a person needs depends on a network connection, an account or Claude. The AI connection (MCP) and the few internet features are optional extras you switch on yourself.

This page lists every feature that could ever touch a network, what it does by default, and how to keep it fully offline.

## The short version

- **Nothing goes online unless you ask for it by name, apart from updates.** There is no telemetry, no account and no licence server. The one thing that runs by itself is the daily [update check](updates.md) an installed app's window makes (one request to GitHub, nothing about you sent); offline it quietly tries again later, and one click (or `WETWARESUITE_NO_UPDATE=1`) turns it off.
- **Using Claude is optional.** Every command you could ask Claude to run, you can run yourself with `wetwaresuite <command>` or a batch script. `wetwaresuite mcp` only starts when an MCP client (like Claude Desktop) starts it, and it only talks to that client on your computer.
- The only other features that can use the internet are: OpenStreetMap tiles in `maps` (`basemap=osm`), `share --internet`, and Claude itself if you connect it. Each one is off until you turn it on.

## Feature by feature

| Feature | Uses the network? | Default | Offline way |
| --- | --- | --- | --- |
| Every tool's commands (photo, paint, vector, cad, audio, cut, office, levels, render, ...) | Never | Offline | Works as is |
| `wetwaresuite batch`, `automate` pipelines and watch folders | Never | Offline | Works as is |
| Updates (`app.update_*`) | Yes: once a day an installed app's window asks GitHub for the newest release and installs it (never from the command line, scripts or MCP unless you run `app.update_install`). Builds from source and preview builds never check by themselves | On for installed copies | Untick **Update by itself** in the Aa menu, `app.update_settings auto=false`, or set `WETWARESUITE_NO_UPDATE=1`. Offline it fails quietly and tries again an hour later. See [updates.md](updates.md) |
| Python client (`python/`) | Never: it starts the local `wetwaresuite` program | Offline | Works as is. To install with no internet see [below](#installing-the-python-client-offline) |
| `wetwaresuite mcp` (stdio, for Claude Desktop / Claude Code) | No network at all: it talks through stdin/stdout to the app that started it | Only runs when an MCP client starts it | Not needed to use the suite |
| `wetwaresuite mcp --http` | Listens on **127.0.0.1** (this computer only). Only listens on your network if you pass `--host`, and then always requires a token | Off | Not needed to use the suite. See [ai-connect.md](ai-connect.md) |
| Claude (Desktop, Code, or any AI) | Yes, Claude itself runs online | Not connected | Everything works without it |
| `maps`: countries basemap, GeoJSON, KML, GPX, Shapefile, CSV, GeoTIFF | Never | `basemap=auto`: an installed map pack, else the built-in countries (Natural Earth, in `third_party/naturalearth`) | Works as is |
| `maps`: map packs (street-level world maps, `.pmtiles` files) | Never: read straight from your disk | Used by `basemap=auto` once installed | Install one from an [offline edition](offline-editions.md) with `hub install maps --from <edition>`, or pass any `.pmtiles` file as `basemap=`. See [maps.md](maps.md#task-8-street-maps-with-no-internet-map-packs) |
| `maps`: OpenStreetMap tiles | Only with `basemap=osm` and a `http(s)` `tile_url`; uses `curl` | Off | `offline=true` uses only cached tiles; `tile_url=D:/tiles/{z}/{x}/{y}.png` reads a tile folder on your disk and never downloads. See [maps.md](maps.md#task-9-openstreetmap-tiles) |
| `share` (serve, sync, send, receive) | Your **local network** only (LAN): an HTTP server and a small UDP "I'm here" announcement so devices find each other | LAN only, nothing on the internet | `--local` keeps it to this computer; `--no-announce` stops the LAN announcement. See [share.md](share.md) |
| `share --internet` | Yes: starts `cloudflared` (a public tunnel) or uses `tailscale` | Off | Leave the flag off |
| `hub list`, `hub mcp-config`, `hub connect` | Never: they only read and write files on this computer | Offline | Works as is |
| `hub install <tool> --from FOLDER-OR-ARCHIVE` | Never: copies from a folder, program file or archive (`.zip`, `.tar.gz`, ...) already on your disk | Offline | Copy a release folder or archive onto a USB stick, then `hub install photo --from E:/wetwaresuite` |
| `hub install <tool>` (no `--from`) | Never: today it only explains how to build or install from a local folder | Offline | Use `--from` |
| `convert`, `cut`, `audio`, `capture` with video or exotic formats | Never: they run `ffmpeg`/`ffprobe` if installed on your computer | Offline | Install ffmpeg once; nothing is fetched |
| `convert` office documents (docx, odt, pptx ... to PDF) | Never: runs LibreOffice (`soffice`) if installed; `pdftoppm`/`pdfinfo` for PDF pages | Offline | Install LibreOffice/poppler once |
| `flash` | Never: serial ports and USB on your computer; may run `avrdude`, `esptool` or `stm32prog` if you installed them | Offline | Built-in bootloader protocols need nothing extra; `sim:` boards need no hardware |
| `notes` (vault, search, HTML and static site export) | Never: links in notes are just written into the HTML, never fetched | Offline | Works as is |
| `type` (fonts) and text rendering | Never: uses the bundled GNU FreeFont (`cut` titles may ask `fc-match` for a font already installed) | Offline | Works as is |
| `library` (indexing, duplicates, thumbnails) | Never | Offline | Works as is |
| Docker image (optional) | The image never goes online. *Building* it downloads base images once | Not used unless you build it | See [docker.md](docker.md) |

## Building from source with no internet

Running the suite never downloads anything. **Building** it from source downloads one thing the first time: the pinned [nlohmann/json](https://github.com/nlohmann/json) source (the URL and SHA-256 are in the top-level `CMakeLists.txt`). Everything else (stb, dr_libs, FreeFont, Natural Earth) is already in `third_party/`. The hub and share are Rust with no crates, so `cargo build --locked` downloads nothing.

To build on a machine with no internet:

1. On any computer that is online, download `json.tar.xz` from the nlohmann/json v3.11.3 release and unpack it, for example to `C:\offline\json` or `~/offline/json`.
2. Copy that folder (and the wetwaresuite source) to the offline machine.
3. Point CMake at it:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$HOME/offline/json
cmake --build build
```

Once a `build` folder has been configured, it keeps its copy, so later builds in it work offline anyway. Add `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` to make CMake refuse to touch the network at all.

## Installing the Python client offline

`pip install ./python` normally downloads `setuptools` to build the package. With no internet, either:

- skip installing: `PYTHONPATH=/path/to/wetwaresuite/python python3 your_script.py` (on Windows: `set PYTHONPATH=C:\path\to\wetwaresuite\python`), or
- install with the setuptools you already have: `pip install --no-build-isolation --no-index ./python`.

The client has no dependencies of its own.

## Installing tools from a USB stick

On an online computer, copy a build or release folder (or its `.zip`/`.tar.gz`) onto a USB stick. On the offline computer:

```sh
hub install photo --from E:/wetwaresuite-0.6.0-windows.zip      # Windows
hub install photo --from /media/usb/wetwaresuite                # Linux
hub install share --from ~/Downloads/wetwaresuite-share         # a single program works too
```

The hub looks for the tool's program inside the folder or archive (a few folders deep), copies it next to the hub (or to `--to FOLDER`), and says so. Running it again just copies it again. Archives are unpacked with the `tar` that comes with Windows 10 and later, macOS and Linux (`unzip` is tried for `.zip` on Linux).

## Checking for yourself

Pull the network cable or turn on airplane mode, then try a few things, for example:

```sh
printf 'photo.new width=64 height=64 -> img\nphoto.save doc=$img path=test.png\n' | wetwaresuite batch
ctest --test-dir build        # the whole test suite runs offline
```

The tests are written to pass with no network (only loopback, for the MCP HTTP server test), no screen, no ffmpeg and no Claude.
