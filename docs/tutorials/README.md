# wetwaresuite tutorials

Hands-on guides to the suite. Every example was run against the real binaries: the all-in-one `wetwaresuite` or the per-app programs like `wetwaresuite-image`, which share the same commands. There is no GUI yet: everything is a command, and the coming GUI will call the same commands.

- [Getting started](getting-started.md): build from source, find commands with `list` and `help`, write arguments, and understand document ids and undo.
- [Headless and batch](headless-and-batch.md): batch scripts with variables and comments, shell loops, CI and cron, exit codes.
- [The apps](apps.md): which app does what, installing only the ones you need, building only some.
- [Workflows](workflows.md): moving work between apps step by step: a texture from image onto a model and into a game, a cad part sculpted and printed, music for a video, a logo on a poster.
- [Offline editions](offline-editions.md): everything on a USB stick (1 to 256 GB) with fonts, starter files, materials and world maps; install with no internet.
- [Using wetwaresuite with Claude (MCP)](mcp.md): connect Claude Code or Claude Desktop, what Claude sees, example prompts, troubleshooting.
- [Photo](photo.md): resize folders for the web, fix dark photos, crop, rotate, frame, effects.
- [Paint](paint.md): brush strokes with tablet pressure, presets, layers, blend modes, fills, saving `.ora`.
- [Vector](vector.md): drawing shapes and paths, styling, SVG in and out, a full logo.
- [Blueprint](blueprint.md): floor plans with walls, doors and dimensions printed to scale as PDF, opening and measuring DXF files, part drawings.
- [CAD](cad.md): parametric parts with expressions, holes, patterns and booleans; a bracket you resize by changing one number, STL export, measuring.
- [Convert](convert.md): images, SVG, audio, video, office documents and markup, one file or a whole folder.
- [Asset forge](forge.md): game assets: models to glb with the right up axis, scale and LODs, sprite sheets and atlases, tileable textures and normal maps, pixel art.
- [Audio](audio.md): clean up a voice recording to podcast loudness, build an episode from music and voice tracks with ducking and fades, batch-normalize samples, spectrograms, pitch and tempo changes.
- [Office](office.md): a budget spreadsheet with formulas and a chart, CSV cleanup with VLOOKUP, a report exported to PDF, a slide deck from bullet points, merging and splitting PDFs.
- [Slicer](slicer.md): 3D models to G-code: printer and filament presets, infill, brim and supports, layer previews, parts from cad.
- [Layout](layout.md): print layout: an event poster, a tri-fold flyer, business cards 10-up, a booklet from Markdown, preflight and print PDFs with bleed and crop marks.
- [Cut](cut.md): video editing: a talking head into a short with a title and music, picture-in-picture, crossfades, GIFs, burned-in subtitles, quick trims and joins.
- [Capture](capture.md): screenshots of a screen, monitor or region, screen recordings with a microphone, webcam snapshots, and how Claude looks at your screen.
- [Toolkit](toolkit.md): identify any file from its contents, hex dumps, the standalone `wetwaresuite-toolkit`.
- [Library](library.md): index an asset drive, tag and rate, find duplicates and similar images, smart collections, thumbnails and contact sheets, asking Claude to find files.
- [Levels](levels.md): game levels and tilemaps: caves, dungeons and overworlds from a seed, auto-tiling, painting and stamping, entities with properties, collision, Tiled TMX/TMJ, LDtk, JSON and CSV.
- [Automate](automate.md): scripts with variables, pipelines over folders, watch folders that process dropped files, the Python client, cron and Task Scheduler, pipelines from Claude.
- [Sculpt](sculpt.md): a creature head from a sphere with mirrored strokes, a cad part made organic and sent to the slicer, retopology and LODs for games, relief tiles from heightmaps, aiming strokes on a preview.
- [Beats](beats.md): music making: generate a beat from a genre and key, program drums on a step grid, bass and chords in the piano roll, synth sound design and effects, your own samples, MIDI in and out, stems and mastering.
- [Animate](animate.md): 2D and 3D animation: a bouncing ball with easing and squash, a logo sting, a cutout rig on bones, hand-drawn cels with onion skin, a 3D turntable; GIFs, sprite sheets with JSON atlases, PNG sequences and video.
- [Render](render.md): 3D scenes and pictures: a model to a picture in one command, a still life in metal, glass and textures, studio product shots on transparent backgrounds, sunsets, cad parts, turntable GIFs and MP4s.
- [Circuit](circuit.md): electronics from schematic to fab: an LED board, a 555 blinker to Gerbers, an Arduino Nano carrier with ground pours, hand routing, KiCad netlists, simulating an RC filter.
- [Flash](flash.md): put firmware on Arduino, ESP32/ESP8266 and STM32 boards headless: list and identify serial ports, detect a board, inspect a HEX/BIN/ELF/ESP image, flash and verify with the built-in bootloader protocols (or avrdude/esptool/stm32prog), a serial monitor and send, and simulated boards for trying it all with no hardware.
- [Board](board.md): whiteboards and diagrams: a flowchart from Mermaid, a hand-drawn architecture sketch, sequence diagrams, mind maps from an outline, a retro wall of sticky notes, Excalidraw and draw.io in and out.
- [Notes](notes.md): an Obsidian-compatible Markdown vault from the command line: meeting notes from templates, daily notes, tasks with due dates, ranked search, backlinks and rename-safe wikilinks, a link graph, HTML/PDF and a static site.
- [Maps](maps.md): offline maps and GIS: a world map coloured from a CSV, distances and UTM coordinates, buffers and heatmaps of places, GPX hike stats and elevation profiles, GeoTIFFs and world files, format conversion, point-in-polygon and spatial joins, OpenStreetMap tiles.
- [Type](type.md): font design: a display font drawn from shapes and SVG, accented letters from components, a font from your handwriting, opening, proofing, subsetting and converting TTF/OTF/WOFF, bold and oblique styles, kerning.
- [Share](share.md): share a folder with a phone or laptop in one command, live-sync a code project between machines, send files, go over the internet with cloudflared or tailscale, and drive it from Claude.
