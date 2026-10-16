# Slicer: from a 3D model to G-code

`slicer` turns a 3D model (STL, 3MF, OBJ, PLY, OFF, glTF) into G-code for an FDM 3D printer. It knows a few common printers and filaments, works out walls, top and bottom skins, infill, skirt, brim and simple supports, and tells you how long the print takes and how much filament it uses. The simplest use is one command:

```sh
wetwaresuite slicer.slice_file input=3DBenchy.stl printer=prusa_mk4 filament=pla
```

There is no GUI yet; the GUI that comes later will call these same commands. Each task below shows the CLI way, the batch-script way and the "ask Claude" way. Every CLI and batch example here was run against the real binary (the outputs are copied from those runs; some long `key: value` lists are shortened with `...`).

## How it works

A slice job is a **print job document** (`print1`, `print2`, ...) holding three things: the model, placed on the bed; the printer, filament and print settings; and, once sliced, the toolpaths and estimates.

- **Bed coordinates.** The origin is the front-left corner of the bed, Z is up, units are millimetres. Loading a model centres it on the bed and drops its lowest point to z = 0. Moving, rotating and scaling always keep it sitting on the bed.
- **Layers.** The first layer is `first_layer_height` thick, every other one `layer_height`. A 20 mm cube at 0.2 mm has exactly 100 layers. Each layer is cut through its middle.
- **Walls** (`perimeters`) are exact inward offsets of the layer outline, one line width apart, printed inside-out so the outer wall goes on last. Holes get walls too.
- **Top and bottom skins.** A spot is printed as solid infill unless every layer within `bottom_layers` below and `top_layers` above also has infill there, so any surface facing up or down (also sloped ones and the floor of a cavity) gets solid layers, not only the very top and bottom of the part.
- **Infill** is straight lines clipped to the area inside the walls: `lines` (one direction per layer, alternating 45 and 135 degrees), `grid` (both every layer) or `triangles` (0, 60 and 120 degrees). Density sets the spacing; 100 % is solid.
- **Skirt and brim.** A skirt is `skirt` loops around everything on the first layer, `skirt_distance` mm away. A brim is `brim` mm of loops touching the outside of the part on the first layer to help it stick.
- **Supports** (`supports=everywhere` or `buildplate`) are vertical columns of lines under any overhang steeper than `support_angle` from vertical, kept a gap away from the part sideways and one layer below it. `buildplate` keeps only the columns that stand on the bed. There are no tree supports, no support interfaces and no bridge detection.
- **G-code** is Marlin flavour: absolute positioning and absolute extrusion (`M82`), with `G92 E0` at the start of every layer, `;LAYER:n` and `;TYPE:` comments (the names Cura uses, so G-code viewers colour them), retraction on travel moves longer than 1.5 mm, the fan off for the first layer, and the first-layer temperature dropping to the normal one on layer 1. Extrusion is line width x layer height x length / filament cross-section (1.75 mm filament).
- **Estimates.** Print time is the sum of every move's length divided by its speed, with no acceleration, so real printers usually take 1.3 to 2 times longer on small, detailed parts. Filament is the length pushed through the nozzle; grams use the filament's density.

Slicing a 3DBenchy (225 000 triangles, 240 layers) takes about a second on a 4-core laptop in a Release build.

## Presets

```text
$ wetwaresuite slicer.presets
printers:
  ender3: 220 x 220 x 250 mm, 0.40 mm nozzle
  prusa_mk4: 250 x 210 x 220 mm, 0.40 mm nozzle
  bambu_a1: 256 x 256 x 256 mm, 0.40 mm nozzle
  custom: 220 x 220 x 250 mm, 0.40 mm nozzle (set bed_x, bed_y, bed_z, nozzle)
filaments:
  pla: nozzle 205 C (first layer 210), bed 60 C, fan 100%, 1.24 g/cm3
  petg: nozzle 235 C (first layer 240), bed 80 C, fan 50%, 1.27 g/cm3
  abs: nozzle 245 C (first layer 250), bed 100 C, fan 0%, 1.04 g/cm3
  tpu: nozzle 225 C (first layer 225), bed 50 C, fan 50%, 1.21 g/cm3, max 25 mm/s
quality:
  draft: 0.28 mm layers (first 0.28), 2 walls, 3 top / 3 bottom layers, 60 mm/s
  normal: 0.20 mm layers (first 0.20), 2 walls, 4 top / 4 bottom layers, 50 mm/s
  fine: 0.12 mm layers (first 0.20), 3 walls, 7 top / 6 bottom layers, 40 mm/s
```

Without any presets a job uses `ender3`, `pla` and `normal`. Each printer has its own start G-code (heat, home, purge line) and the same end G-code (retract, lift, present the bed, heaters and motors off):

- `ender3` (and any generic Marlin printer of that size, such as Ender 3 V2/S1, Sovol, Artillery): purge line along the left edge.
- `prusa_mk4`: checks the printer model (`M862.3 P "MK4"`), runs mesh bed levelling (`G29`), purges along the front edge.
- `bambu_a1`: generic Marlin start with a front purge. Bambu printers normally take G-code from Bambu Studio with its own long start sequence; to print from this G-code, replace the start with your printer's own using `start_gcode` (see task 6).

## Task 1: slice a downloaded STL for a Prusa MK4 in PLA

You downloaded [3DBenchy](https://www.3dbenchy.com) as `3DBenchy.stl` and want G-code for a Prusa MK4 with PLA at normal quality.

**CLI.**

```sh
$ wetwaresuite slicer.slice_file input=3DBenchy.stl printer=prusa_mk4 filament=pla
sliced 3DBenchy.stl for the prusa_mk4 in pla: 240 layers, about 45m 34s, 4.03 m / 12.0 g of filament; wrote 3DBenchy.gcode
output: 3DBenchy.gcode
layers: 240
time: 45m 34s
seconds: 2734
filament_m: 4.032
filament_g: 12
```

- Without `output=` the G-code goes next to the model with a `.gcode` extension. An existing file is replaced.
- Any setting from the table at the end works here too, e.g. `quality=fine infill=15 brim=4`.
- The command fails (exit code 1) when the model doesn't fit the printer, saying why:

```sh
$ wetwaresuite slicer.slice_file input=3DBenchy.stl scale=5
can't slice: the model is 300.0 x 155.0 x 240.0 mm but the ender3 prints at most 220 x 220 x 250 mm
```

The top of the G-code carries the estimates and the settings, then the printer's start G-code:

```text
;FLAVOR:Marlin
;Generated by the wetwaresuite slicer
;TIME_ESTIMATE:2734
;TIME:2734
;Filament used: 4.0324m
;FILAMENT_USED_MM:4032.4
;FILAMENT_USED_G:12.03
;LAYER_COUNT:240
;Layer height: 0.2
;MODEL:3DBenchy.stl
;PRINTER:prusa_mk4 bed 250x210x220 nozzle 0.4
;FILAMENT:pla 205C (first layer 210C) bed 60C fan 100%
;SETTINGS: quality normal, line width 0.45, perimeters 2, top 4, bottom 4, infill 20% grid, supports none
;MINX:93.903
...
M862.3 P "MK4" ; printer model check
G90 ; absolute positioning
M82 ; absolute extrusion
M140 S60
...
```

and each layer starts like this (layer 1 also switches to the normal nozzle temperature and turns the fan on):

```text
;LAYER:1
;Z:0.4
M104 S205
M106 S255
G1 E43.65344 F2100
G92 E0
G0 Z0.4 F600
;TYPE:WALL-INNER
...
```

**Batch.** A script keeps the job open between steps, so you can check it before writing the file. `-> boat` stores the new job's id (`print1`) and `$boat` uses it:

```text
# benchy.txt: 3DBenchy on a Prusa MK4 in PLA
slicer.load path=3DBenchy.stl printer=prusa_mk4 filament=pla -> boat
slicer.slice doc=$boat
slicer.export_gcode doc=$boat path=benchy-mk4.gcode
echo sliced $boat
```

```sh
$ wetwaresuite batch benchy.txt
loaded 3DBenchy.stl as print1 (225154 triangles) on the prusa_mk4
doc: print1
size: 60.001,31.004,48
center: 125,105
fits: true
sliced 3DBenchy.stl: 240 layers, about 45m 34s, 4.03 m / 12.0 g of filament
...
wrote benchy-mk4.gcode: 240 layers, about 45m 34s, 4.03 m / 12.0 g of filament
doc: print1
output: benchy-mk4.gcode
bytes: 2652501
...
sliced print1
```

`slicer.export_gcode` slices by itself if needed, so the `slicer.slice` line is only there to show the estimates first.

**Claude.**

> Slice ~/Downloads/3DBenchy.stl for my Prusa MK4 in PLA and save the G-code next to it. How long will it take?

Claude calls `slicer_slice_file` with `input`, `printer=prusa_mk4` and `filament=pla` and reads `time` and `filament_g` from the result. If you want to look first, it uses `slicer_load`, then `slicer_slice` with `preview=true` to see a layer, then `slicer_export_gcode`.

## Task 2: does it fit? Placing, turning and scaling

`slicer.info` says how big the model is, whether it fits, how much it would weigh solid and what's set:

```sh
$ wetwaresuite batch fit.txt
```

with

```text
# fit.txt: a model too big for the bed, turned and shrunk until it fits
slicer.load path=3DBenchy.stl scale=4 -> big
slicer.rotate doc=$big z=45
slicer.info doc=$big
slicer.scale doc=$big factor=0.5
undo
slicer.settings doc=$big printer=bambu_a1
slicer.preview doc=$big output=big-on-bed.png
```

prints (shortened):

```text
loaded 3DBenchy.stl as print1 (225154 triangles) on the ender3; warning: the model is 240.0 x 124.0 x 192.0 mm but the ender3 prints at most 220 x 220 x 250 mm
doc: print1
size: 240.004,124.016,192
center: 110,110
fits: false
problem: the model is 240.0 x 124.0 x 192.0 mm but the ender3 prints at most 220 x 220 x 250 mm
rotated
doc: print1
size: 203.305,203.305,192
center: 110,110
fits: true
3DBenchy.stl: 203.3 x 203.3 x 192.0 mm, 995.23 cm3 (1234.1 g if printed solid in pla); fits the ender3
ender3 (220 x 220 x 250 mm, 0.40 mm nozzle), pla 205/60 C, normal: 0.20 mm layers, 2 walls, 4 top / 4 bottom, 20% grid infill, 50 mm/s, supports none, skirt 1
...
scaled
doc: print1
size: 101.652,101.652,96
center: 110,110
fits: true
undone
bambu_a1 (256 x 256 x 256 mm, 0.40 mm nozzle), pla 205/60 C, normal: ...
fits: true
preview of 3DBenchy.stl on the bambu_a1
doc: print1
output: big-on-bed.png
attachment: print.png (40345 bytes)
```

- Turning the 240 mm boat 45 degrees makes it fit diagonally (`fits: true`). Scaling and `undo` work like everywhere else in the suite.
- Changing to a printer with a different bed centres the model on the new bed.
- A job that doesn't fit can still be edited; only slicing and exporting refuse.
- `slicer.preview` renders the model on the bed (`view=top`, `front`, ... like other 3D previews).
- `slicer.place doc=print1` re-centres; `slicer.place doc=print1 x=60 y=60` puts the centre at (60, 60). `slicer.move doc=print1 x=10` slides it 10 mm right (add `absolute=true` to give the centre instead).
- `slicer.lay_flat` turns the largest flat outside face down: handy for parts exported standing up or at an angle.

**CLI one-shot:** `slicer.slice_file` takes `scale`, `rotate_z` and `lay_flat`: `wetwaresuite slicer.slice_file input=3DBenchy.stl scale=4 rotate_z=45 output=big.gcode`.

**Claude.**

> Will this 3DBenchy at 400 % fit on my Ender 3? If not, make it fit without shrinking it.

Claude calls `slicer_load` with `scale=4`, sees `fits: false` and the reason, tries `slicer_rotate` with `z=45`, and confirms with `slicer_info` (or `slicer_preview` to look at it on the bed).

## Task 3: tweak infill, walls, brim and supports

Settings can be passed to `slicer.load` and `slicer.slice_file`, or changed later with `slicer.settings`. Presets (`printer`, `filament`, `quality`) apply first and individual values after, so `quality=fine infill=30` gives fine layers with 30 % infill.

**Batch.** Compare a few variants on one open job:

```text
# tweak.txt: try a few setting combinations and compare the estimates
slicer.load path=3DBenchy.stl printer=ender3 filament=petg -> boat
slicer.settings doc=$boat infill=10 infill_pattern=lines
slicer.slice doc=$boat
slicer.settings doc=$boat infill=40 infill_pattern=triangles perimeters=3
slicer.slice doc=$boat
slicer.settings doc=$boat brim=5 supports=buildplate support_angle=55
slicer.slice doc=$boat
slicer.export_gcode doc=$boat path=benchy-strong.gcode
```

The summary lines of `wetwaresuite batch tweak.txt`:

```text
ender3 (220 x 220 x 250 mm, 0.40 mm nozzle), petg 235/80 C, normal: 0.20 mm layers, 2 walls, 4 top / 4 bottom, 10% lines infill, 50 mm/s, supports none, skirt 1
sliced 3DBenchy.stl: 240 layers, about 42m 2s, 3.73 m / 11.4 g of filament
ender3 (220 x 220 x 250 mm, 0.40 mm nozzle), petg 235/80 C, normal: 0.20 mm layers, 3 walls, 4 top / 4 bottom, 40% triangles infill, 50 mm/s, supports none, skirt 1
sliced 3DBenchy.stl: 240 layers, about 52m 42s, 4.86 m / 14.8 g of filament
ender3 (220 x 220 x 250 mm, 0.40 mm nozzle), petg 235/80 C, normal: 0.20 mm layers, 3 walls, 4 top / 4 bottom, 40% triangles infill, 50 mm/s, supports buildplate, brim 5.0 mm, skirt 1
sliced 3DBenchy.stl: 240 layers, about 53m 46s, 4.91 m / 15.0 g of filament
wrote benchy-strong.gcode: 240 layers, about 53m 46s, 4.91 m / 15.0 g of filament
```

Every `slicer.settings` is one undo step, and a bad value is refused with the reason (e.g. `layer_height=0.5` on a 0.4 mm nozzle: `layer height 0.50 mm is too thick for a 0.40 mm nozzle (keep it under 0.32)`).

**CLI.**

```sh
wetwaresuite slicer.slice_file input=3DBenchy.stl output=benchy-petg.gcode printer=bambu_a1 filament=petg quality=fine infill=15 brim=4
# sliced 3DBenchy.stl for the bambu_a1 in petg: 400 layers, about 1h 33m, 4.22 m / 12.9 g of filament; wrote benchy-petg.gcode
wetwaresuite slicer.slice_file input=3DBenchy.stl output=benchy-tree.gcode supports=buildplate infill=25 infill_pattern=triangles skirt=2
# sliced 3DBenchy.stl for the ender3 in pla: 240 layers, about 47m 6s, 4.19 m / 12.5 g of filament; wrote benchy-tree.gcode
```

Supports: `none` (default), `everywhere` (columns may stand on the part itself) or `buildplate` (only columns that reach the bed). `support_angle` is measured from vertical: overhangs flatter than it are supported (default 50). `support_density` is the line density in percent (default 15).

**Claude.**

> The Benchy's hull peeled off the bed last time. Re-slice print1 with a 5 mm brim and PETG, and make it a bit stronger.

Claude calls `slicer_settings` with `doc=print1`, `filament=petg`, `brim=5` and something like `perimeters=3 infill=30`, then `slicer_slice` to report the new time and weight, and `slicer_export_gcode`.

## Task 4: look at a layer before printing

`slicer.preview_layer` draws one layer from above: red outer wall, green inner walls, yellow solid infill, orange sparse infill, cyan support, grey skirt and brim, thin blue lines for travel moves. Layers count from 0 (the first layer); negative numbers count from the top (`-1` is the last). The result also says how many millimetres of each kind the layer has.

**Batch.**

```text
# layers.txt: pictures of the first layer, the deck and the top
slicer.load path=3DBenchy.stl brim=3 -> boat
slicer.preview_layer doc=$boat layer=0 output=layer-first.png frame=bed
slicer.preview_layer doc=$boat layer=45 output=layer-deck.png
slicer.preview_layer doc=$boat layer=-1 output=layer-top.png travel=false
```

```text
$ wetwaresuite batch layers.txt
loaded 3DBenchy.stl as print1 (225154 triangles) on the ender3
...
layer 0 of 240 at z=0.20 mm: skirt 148 mm, brim 742 mm, outer wall 267 mm, inner wall 245 mm, solid infill 525 mm (red outer wall, green inner walls, yellow solid infill, orange infill, cyan support, grey skirt/brim, blue travel)
...
layer 45 of 240 at z=9.20 mm: outer wall 353 mm, inner wall 341 mm, solid infill 181 mm, infill 115 mm (...)
...
layer 239 of 240 at z=48.00 mm: outer wall 31 mm, inner wall 31 mm, solid infill 3 mm (...)
```

- `frame=bed` shows the whole bed (to check placement and the skirt); the default `frame=part` zooms to the print, framed the same for every layer so you can flip through them.
- `size=1024` for a bigger picture, `travel=false` to hide travel moves.
- `slicer.preview doc=print1 output=on-bed.png` renders the 3D model on the bed instead.

**CLI.** A preview needs a job, so on the command line put the two steps in a script, or pipe them: `printf 'slicer.load path=3DBenchy.stl -> b\nslicer.preview_layer doc=$b layer=0 output=first.png\n' | wetwaresuite batch`.

**Claude.**

> Show me the first layer of the Benchy with a 3 mm brim, and the layer where the deck starts.

Claude calls `slicer_load` with `brim=3`, then `slicer_preview_layer` with `layer=0` and looks at the picture (MCP returns it as an image), then a few more layers to find the deck. `slicer_slice` with `preview=true` attaches a picture too (the middle layer, or the one given by `layer`).

## Task 5: print a part made in cad

The [cad](cad.md) tool exports its parts as meshes: `cad.save path=part.stl` (or `.3mf`). The slicer then takes it like any downloaded file. Below, `part.stl` is an L bracket, 40 x 30 x 12 mm, 5 mm thick, exported lying on its L-shaped face. To make it strong across the bend it should stand on its long 40 x 12 mm edge, with more walls, in PETG.

**Batch.**

```text
# cad.txt: model the bracket in cad, then print it
# ... cad commands that build the bracket (see the cad tutorial) ...
cad.save path=part.stl
slicer.slice_file input=part.stl output=part.gcode filament=petg perimeters=4 infill=40 lay_flat=true
```

The same job, step by step, and going through 3MF with [convert](convert.md) on the way (convert changes mesh formats: STL, OBJ, PLY, OFF, glTF, 3MF, and can render a mesh to PNG):

```text
# part.txt: print a cad part (part.stl) strong, in PETG, on its side
convert.file input=part.stl output=part.3mf overwrite=true
slicer.load path=part.3mf filament=petg perimeters=4 infill=40 -> job
slicer.rotate doc=$job x=90
slicer.lay_flat doc=$job
slicer.info doc=$job
slicer.export_gcode doc=$job path=part.gcode
```

```text
$ wetwaresuite batch part.txt
converted part.stl to part.3mf
...
loaded part.3mf as print1 (20 triangles) on the ender3
doc: print1
size: 40,30,12
center: 110,110
fits: true
rotated
doc: print1
size: 40,12,30
center: 110,110
fits: true
already lying on its largest flat face
...
part.3mf: 40.0 x 12.0 x 30.0 mm, 3.90 cm3 (5.0 g if printed solid in petg); fits the ender3
ender3 (220 x 220 x 250 mm, 0.40 mm nozzle), petg 235/80 C, normal: 0.20 mm layers, 4 walls, 4 top / 4 bottom, 40% grid infill, 50 mm/s, supports none, skirt 1
...
wrote part.gcode: 150 layers, about 8m 5s, 0.74 m / 2.3 g of filament
```

`slicer.lay_flat` agrees: after the turn the bracket already stands on its largest face (40 x 12 = 480 mm2, more than the L face's 325 mm2).

In one session (a batch script, or Claude over MCP) the slicer can also take a mesh that's still open instead of a file: `slicer.load mesh=mesh1` loads any open mesh document by its id, so a part can go from cad to the slicer without saving it first, if the part is open as a mesh.

The cad lines above come from the cad tool; everything from `convert.file` on was run as shown.

**Claude.**

> Make a 40 x 30 x 12 mm L bracket, 5 mm thick, in cad, then slice it for my Ender 3 in PETG standing on its long edge, with 4 walls.

Claude builds the part with the cad tools, saves it with `cad_save` (`path=part.stl`), then calls `slicer_load`, `slicer_rotate` (`x=90`), `slicer_preview` to check it stands right, `slicer_settings` (`filament=petg`, `perimeters=4`) and `slicer_export_gcode`.

## Task 6: your own printer

Any printer works with `printer=custom` plus its size and nozzle. A custom start G-code replaces the preset one: separate lines with `|`, and `{bed_temp}`, `{nozzle_temp}`, `{first_layer_temp}`, `{bed_x}`, `{bed_y}` and `{park_z}` are filled in. The same goes for `end_gcode`.

```text
# custom.txt: a home-built 300 x 300 x 400 printer with a 0.6 mm nozzle
slicer.load path=part.stl printer=custom bed_x=300 bed_y=300 bed_z=400 nozzle=0.6 layer_height=0.3 first_layer_height=0.3 -> job
slicer.settings doc=$job start_gcode="G90|M82|M190 S{bed_temp}|M109 S{first_layer_temp}|G28|G92 E0" retraction=1.5
slicer.export_gcode doc=$job path=part-big-nozzle.gcode
```

```text
$ wetwaresuite batch custom.txt
loaded part.stl as print1 (20 triangles) on the custom
...
custom (300 x 300 x 400 mm, 0.60 mm nozzle), pla 205/60 C, normal: 0.30 mm layers, 2 walls, 4 top / 4 bottom, 20% grid infill, 50 mm/s, supports none, skirt 1
...
wrote part-big-nozzle.gcode: 40 layers, about 6m 20s, 1.20 m / 3.6 g of filament
```

The line width follows the nozzle (0.6 x 1.125 = 0.675 mm) unless you set `line_width`. One-shot: `wetwaresuite slicer.slice_file input=part.stl printer=custom bed_x=300 bed_y=300 bed_z=400 nozzle=0.6 layer_height=0.3`. `bed_x`, `bed_y`, `bed_z` and `nozzle` also adjust a named preset (e.g. an Ender 3 with a 0.6 mm nozzle: `printer=ender3 nozzle=0.6`).

**Claude.**

> My printer is a Voron with a 350 mm bed and a 0.4 nozzle, it uses `PRINT_START BED={bed_temp} EXTRUDER={first_layer_temp}` as its start macro. Slice part.stl for it in ABS.

Claude calls `slicer_slice_file` with `printer=custom`, `bed_x=350 bed_y=350 bed_z=340`, `filament=abs` and `start_gcode="PRINT_START BED={bed_temp} EXTRUDER={first_layer_temp}"`.

## Commands

| Command | What it does |
| --- | --- |
| `slicer.load` | Start a print job from `path=` (any mesh format) or `mesh=` (an open mesh document); centres it on the bed. Takes `scale`, `lay_flat`, `preview` and every setting below. Returns `print1`, ... |
| `slicer.slice_file` | One shot: `input=` model, `output=` G-code (default: next to the model), plus `scale`, `rotate_z`, `lay_flat` and every setting. |
| `slicer.place` | Drop onto the bed and centre it, or put its centre at `x`, `y`. |
| `slicer.move` | Slide by `x`, `y` mm (`absolute=true`: to that centre). |
| `slicer.rotate` | Rotate by `x`, then `y`, then `z` degrees about its centre, then drop back onto the bed. |
| `slicer.scale` | `factor` for all axes, or `x`, `y`, `z` factors. |
| `slicer.lay_flat` | Turn the largest flat outside face down. |
| `slicer.settings` | Change the printer, filament, quality or any setting (shows them all when given none). |
| `slicer.info` | Size, position, fits or not (and why), volume, solid weight, settings, and the estimates once sliced. Read-only. |
| `slicer.presets` | The printer, filament and quality presets. Read-only. |
| `slicer.slice` | Slice and estimate. `preview=true` attaches a layer picture (`layer=` to pick one). |
| `slicer.export_gcode` | Write the G-code to `path=` (slices first if needed). |
| `slicer.preview_layer` | Picture of one layer (`layer`, `size`, `travel`, `frame=part|bed`, `output=` to save it). |
| `slicer.preview` | The model on the bed (`view`, `size`, `output`). |
| `slicer.close` | Close a job (undo reopens it). |

Moves, rotations, scaling, laying flat and settings are undoable (`undo`, `redo`); every edit clears the old slice, so estimates are always for the current state. All commands that change a model accept `preview=true` to attach a picture of it on the bed.

## Settings

Values marked "preset" come from the printer, filament or quality preset.

| Setting | Meaning | Default |
| --- | --- | --- |
| `printer` | `ender3`, `prusa_mk4`, `bambu_a1`, `custom` | `ender3` |
| `bed_x`, `bed_y`, `bed_z` | printable size in mm | preset |
| `nozzle` | nozzle diameter, mm | 0.4 |
| `filament` | `pla`, `petg`, `abs`, `tpu` | `pla` |
| `nozzle_temp` | nozzle C from layer 1 (setting it alone also sets the first layer to +5) | preset |
| `first_layer_temp` | nozzle C on layer 0 | preset |
| `bed_temp` | bed C | preset |
| `fan` | part fan percent from layer 1 (off on layer 0) | preset |
| `quality` | `draft` (0.28), `normal` (0.2), `fine` (0.12) | `normal` |
| `layer_height` | mm, at most 0.8 x nozzle | 0.2 |
| `first_layer_height` | mm | 0.2 |
| `line_width` | extrusion width, mm | nozzle x 1.125 |
| `perimeters` | walls (0..20) | 2 |
| `top_layers`, `bottom_layers` | solid layers over and under surfaces | 4, 4 |
| `infill` | sparse infill percent, 0..100 (100 = solid) | 20 |
| `infill_pattern` | `lines`, `grid`, `triangles` | `grid` |
| `speed` | print speed mm/s (outer walls 75 %) | 50 |
| `first_layer_speed` | mm/s | 20 |
| `travel_speed` | mm/s | 150 |
| `retraction` | mm pulled back on travels over 1.5 mm and at layer changes (0 = off) | 0.8 |
| `retraction_speed` | mm/s | 35 |
| `skirt` | skirt loops (0 = none) | 1 |
| `skirt_distance` | mm between skirt and part (or brim) | 5 |
| `brim` | brim width, mm (0 = none) | 0 |
| `supports` | `none`, `everywhere`, `buildplate` | `none` |
| `support_angle` | degrees from vertical that need support | 50 |
| `support_density` | support line percent | 15 |
| `seam` | where wall loops start: `nearest` (shortest travel), `rear` (back of the part), `random` | `nearest` |
| `start_gcode`, `end_gcode` | custom G-code, `|` between lines, with `{placeholders}` | preset |

TPU caps every speed at 25 mm/s.

## Limitations

- One model per job; no arranging several parts on one bed (load and slice them separately).
- Walls follow the outline exactly; walls thinner than two lines get fewer walls and no gap fill, so very thin features can come out hollow or missing.
- No bridge detection, ironing, z-hop, coasting, wipe, adaptive layers, variable line width, tree supports or support interfaces.
- Time estimates ignore acceleration; expect real prints to take longer.
- G-code is Marlin flavour with absolute extrusion. Klipper accepts it; Bambu printers need their own start G-code (task 6).
- Region booleans snap to a 0.1 micron grid; in rare cases a layer can get a slightly wrong skin. Check suspicious layers with `slicer.preview_layer`.
