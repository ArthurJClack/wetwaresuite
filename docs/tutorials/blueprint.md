# blueprint tutorial: floor plans and technical drawings

`blueprint` is the suite's 2D drafting tool, in the spirit of LibreCAD or
AutoCAD LT: real-world units, layers, dimensions that measure themselves,
walls/doors/windows/rooms for floor plans, a title block, DXF in and out,
and PDF/SVG/PNG sheets that print to scale. Like every ACHCLWSS tool it is
headless: each action is a command you can type once, put in a batch
script, or let Claude call over MCP.

The tutorial is built around three real jobs, each shown three ways:

- **task 1**: draw a one-bedroom flat with walls, doors, windows, room areas
  and dimensions, and print it at 1:50 on A3 as a PDF;
- **task 2**: open a DXF someone sent you, find things in it, measure
  areas, distances and angles, and print an overview;
- **task 3**: a dimensioned machine part (a mounting plate) at 1:1 on A4.

Everything below was run against the real tool; the outputs shown are what
it printed.

## contents

1. [the ideas you need](#1-the-ideas-you-need)
2. [setting up](#2-setting-up)
3. [task 1: a floor plan printed to scale](#3-task-1-a-floor-plan-printed-to-scale)
4. [task 2: open a DXF and measure it](#4-task-2-open-a-dxf-and-measure-it)
5. [task 3: a dimensioned part drawing](#5-task-3-a-dimensioned-part-drawing)
6. [editing: move, copy, rotate, mirror, offset, change](#6-editing-move-copy-rotate-mirror-offset-change)
7. [exporting and printing](#7-exporting-and-printing)
8. [DXF: what goes in and out](#8-dxf-what-goes-in-and-out)
9. [command and parameter reference](#9-command-and-parameter-reference)
10. [troubleshooting and limitations](#10-troubleshooting-and-limitations)

## 1. the ideas you need

**You draw in real units, not paper units.** `blueprint.new units=mm`
means a wall from `0,0` to `8000,0` is eight metres long. Units can be `mm`,
`cm`, `m`, `in` or `ft`. Lengths you type (thickness, width, offset, text
height) are in the same units.

**y goes up.** Like every CAD program, `x` grows to the right and `y` grows
upwards. That is the opposite of the photo and vector tools.

**The scale puts the drawing on paper.** `scale=1:50` means 1 mm on paper
is 50 mm in the drawing. The paper (`A4`..`A0`, `letter`, `legal`,
`tabloid`, `landscape` or `portrait`) and the scale only matter when you
export or print; they never change your coordinates. `blueprint.page`
changes them at any time, and `scale=fit` picks the largest standard scale
(10:1, 5:1, 2:1, 1:1, 1:2, 1:5, 1:10, 1:20, 1:25, 1:50, 1:75, 1:100, 1:200, ...) that fits.
`center=true` centres the drawing in the space above the title block.

**Some sizes are paper sizes.** Dimension text and arrowheads
(`blueprint.dimstyle`), layer line weights and hatch spacing are in paper
millimetres, so they look the same at 1:1 and at 1:100. Text height on
`blueprint.text` is in drawing units (default: 3.5 mm on paper at the
current scale).

**Documents and entities have ids.** `blueprint.new` and `blueprint.open`
return a document id like `bp1`; every later command names it with
`doc=bp1`. Every entity you add gets an id `e1`, `e2`, ... in order.

**Selections.** Commands that take `entities=` accept a comma-separated mix
of ids (`e3,e7`), `all`, `last`, `layer:WALLS` and `type:door` (types:
`line polyline circle arc text point dimension hatch wall door window room`).

**Layers carry the pens.** Each layer has a colour, a line weight in paper
mm, a line type (`continuous`, `dashed`, `center`, `hidden`) and can be
hidden or locked. New entities go on the current layer unless you pass
`layer=`; a layer named in `layer=` is created if it doesn't exist.

**Dimensions are measured, never typed.** You give the points; the tool
measures them and formats the value in the drawing's units (`4625`,
`12'-6"`, `Ø40`, `106.26°`). `text="4x <> THRU"` adds words around the
value (`<>` is the measured value).

**Everything is undoable.** Every edit, including `blueprint.close`, is one
undo step (`undo`/`redo` in the shell and over MCP). Reading and output
commands (`info`, `list`, `measure`, `preview`, `save`, `export`) don't
touch the history.

Points are written `x,y,x,y,...` (`points=0,0,8000,0,8000,6000`). Colours
are `#rrggbb`, names (`red`, `blue`, `teal`, `darkgray`, ...) or AutoCAD
colour numbers (`aci:5`).

## 2. setting up

Build the suite once (see the README for compilers):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
alias achclwss="$PWD/build/apps/achclwss/achclwss"
```

List the blueprint commands and ask for help on one:

```sh
achclwss list blueprint
achclwss help blueprint.wall
```

```
blueprint.wall: Wall
Draw a wall along points (x,y,...) with a thickness (default 200 mm), as two parallel faces with mitred corners. closed=true runs it all the way round (an outer wall). justify says whether the points are the wall's centre line, its left face or its right face (left/right as you walk from point to point). Doors and windows cut openings into it.

usage: blueprint.wall doc=<string> points=<array> [thickness=<number>] [closed=<boolean>] [justify=center|left|right] [fill=none|solid|lines] [layer=<string>] [preview=<boolean>]
  doc (string, required)  id of an open blueprint, e.g. bp1
  points (array, required)  path as x,y,x,y,...
  thickness (number)  wall thickness (default 200 mm)
  ...
```

There are three ways to run commands:

- **one-shot**: `achclwss <command> key=value ...` (or `achclwss run
  <command> ...`) runs one command and exits. Nothing stays open afterwards,
  so one-shots are for jobs that fit in one command: the commands that take
  `file=` (`blueprint.info`, `blueprint.list`, `blueprint.measure`) read a
  DXF directly, and `blueprint.convert` turns a DXF into a PDF, SVG or PNG.
- **batch**: `achclwss batch plan.txt` runs a text file of commands, one
  per line (`#` starts a comment). End a line with `-> name` to remember
  its result, and write `$name` later to use it: `blueprint.new ... -> plan`
  then `doc=$plan`; `blueprint.wall ... -> outer` then `wall=$outer`.
- **MCP**: `achclwss mcp` serves every command as a tool for Claude (see
  the end of each task).

Each command prints a one-line summary, its value, then its fields as
`key: value` lines. The batch outputs below show only the summary lines;
the value and field lines repeat what the summary says.

## 3. task 1: a floor plan printed to scale

The job: an 8 x 6 m one-bedroom flat with a 250 mm outer wall, 100 mm
inside walls, three doors, three windows, room names with areas, overall
and room dimensions, and a title block, printed on A3 at 1:50.

### the batch script

Save this as `flat.txt`:

```
# flat.txt: a one-bedroom flat, 8 x 6 m, printed on A3 at 1:50
blueprint.new units=mm scale=1:50 paper=A3 orientation=landscape title="Flat 2, ground floor" author="A. Clack" date=2026-10-16 -> plan

# layers: pen colour, pen width on paper (mm), line type
blueprint.layer_add doc=$plan name=WALLS color=black weight=0.5 current=true
blueprint.layer_add doc=$plan name=DOORS color=darkgray weight=0.25
blueprint.layer_add doc=$plan name=WINDOWS color=teal weight=0.25
blueprint.layer_add doc=$plan name=ROOMS color=gray weight=0.18
blueprint.layer_add doc=$plan name=DIMS color=blue weight=0.18

# walls: points are the centre line; the outer wall goes all the way round
blueprint.wall doc=$plan points=0,0,8000,0,8000,6000,0,6000 closed=true thickness=250 -> outer
blueprint.wall doc=$plan points=4500,125,4500,5875 thickness=100 -> split
blueprint.wall doc=$plan points=4500,2600,7875,2600 thickness=100 -> bathwall

# doors: at = distance along the wall from its first point to the door's centre
blueprint.door doc=$plan wall=$outer at=2000 width=1000 layer=DOORS
blueprint.door doc=$plan wall=$split at=1500 width=800 side=right layer=DOORS
blueprint.door doc=$plan wall=$bathwall at=1200 width=800 hinge=end side=left layer=DOORS

# windows: x,y picks the nearest spot on the wall
blueprint.window doc=$plan wall=$outer x=2250 y=6000 width=1800 layer=WINDOWS
blueprint.window doc=$plan wall=$outer x=8000 y=4500 width=1200 layer=WINDOWS
blueprint.window doc=$plan wall=$outer x=8000 y=1500 width=600 layer=WINDOWS

# rooms: the inside faces of the walls, labelled with name and area
blueprint.room doc=$plan points=125,125,4450,125,4450,5875,125,5875 name=Living outline=false layer=ROOMS -> living
blueprint.room doc=$plan points=4550,2650,7875,2650,7875,5875,4550,5875 name=Bedroom outline=false layer=ROOMS
blueprint.room doc=$plan points=4550,125,7875,125,7875,2550,4550,2550 name=Bath outline=false layer=ROOMS

# dimensions: the value is measured, never typed
blueprint.layer_current doc=$plan name=DIMS
blueprint.dimension doc=$plan kind=horizontal x1=-125 y1=-125 x2=8125 y2=-125 offset=-1200
blueprint.dimension doc=$plan kind=horizontal x1=-125 y1=-125 x2=4500 y2=-125 offset=-600
blueprint.dimension doc=$plan kind=horizontal x1=4500 y1=-125 x2=8125 y2=-125 offset=-600
blueprint.dimension doc=$plan kind=vertical x1=8125 y1=-125 x2=8125 y2=6125 offset=1200
blueprint.dimension doc=$plan kind=vertical x1=8125 y1=-125 x2=8125 y2=2600 offset=600
blueprint.dimension doc=$plan kind=vertical x1=8125 y1=2600 x2=8125 y2=6125 offset=600

# the sheet: centre the plan above the title block, then save and print
blueprint.titleblock doc=$plan sheet=A-101
blueprint.page doc=$plan center=true
blueprint.measure doc=$plan entity=$living
blueprint.save doc=$plan path=flat.dxf
blueprint.export doc=$plan path=flat.pdf
blueprint.export doc=$plan path=flat.png dpi=100
```

Run it:

```sh
achclwss batch flat.txt
```

```
created bp1 (A3 landscape 1:50, mm, 0 entities, 1 layer)
added layer WALLS: color #000000, weight 0.5mm, continuous (current)
added layer DOORS: color #404040, weight 0.25mm, continuous
added layer WINDOWS: color #008080, weight 0.25mm, continuous
added layer ROOMS: color #808080, weight 0.18mm, continuous
added layer DIMS: color #0000ff, weight 0.18mm, continuous
added e1 wall [WALLS] closed points=0,0 8000,0 8000,6000 0,6000 thickness 250 length 28000
added e2 wall [WALLS] open points=4500,125 4500,5875 thickness 100 length 5750
added e3 wall [WALLS] open points=4500,2600 7875,2600 thickness 100 length 3375
added e4 door [DOORS] in e1 at 2000 width 1000 hinge start swing left
added e5 door [DOORS] in e2 at 1500 width 800 hinge start swing right
added e6 door [DOORS] in e3 at 1200 width 800 hinge end swing left
added e7 window [WINDOWS] in e1 at 19750 width 1800
added e8 window [WINDOWS] in e1 at 12500 width 1200
added e9 window [WINDOWS] in e1 at 9500 width 600
added e10 room [ROOMS] "Living" closed points=125,125 4450,125 4450,5875 125,5875 area 24.87 m² perimeter 20150
added e11 room [ROOMS] "Bedroom" closed points=4550,2650 7875,2650 7875,5875 4550,5875 area 10.72 m² perimeter 13100
added e12 room [ROOMS] "Bath" closed points=4550,125 7875,125 7875,2550 4550,2550 area 8.06 m² perimeter 11500
current layer DIMS
added e13 dimension [DIMS] horizontal -125,-125 -> 8125,-125 = 8250
added e14 dimension [DIMS] horizontal -125,-125 -> 4500,-125 = 4625
added e15 dimension [DIMS] horizontal 4500,-125 -> 8125,-125 = 3625
added e16 dimension [DIMS] vertical 8125,-125 -> 8125,6125 = 6250
added e17 dimension [DIMS] vertical 8125,-125 -> 8125,2600 = 2725
added e18 dimension [DIMS] vertical 8125,2600 -> 8125,6125 = 3525
title block on: "Flat 2, ground floor", A. Clack, 2026-10-16, sheet A-101, scale 1:50
page A3 landscape at 1:50, origin -5850,-5825, drawing fits on the paper
e10 area 24868750 (24.87 m²), perimeter 20150
saved flat.dxf (18 entities)
exported flat.pdf (A3 landscape at 1:50)
exported flat.png (A3 landscape at 1:50)
```

What happened, step by step:

- **Walls** are drawn along their centre line (`justify=left|right` makes
  the points one face instead). The closed outer wall gets mitred corners.
  The two inside walls start at the outer wall's inside face (y=125 is half
  of 250) and at the split wall's face (x=7875); an open wall end that
  butts into another wall is joined to it: no end cap, and the other
  wall's face is opened up. That's why the T-junctions in the PDF are clean.
- **Doors and windows** belong to a wall (`wall=$outer`) and cut an opening
  in it. `at` is the distance along the wall from its first point to the
  middle of the opening; for the closed outer wall the distance runs
  round the corners (the window "at 19750" is on the top side). Or pass
  `x,y` and the tool picks the nearest spot. A door draws its leaf and a
  90° swing; `hinge=start|end` picks the jamb, `side=left|right` the side it
  opens into (looking along the wall from its first point). An opening that
  would run over a corner, off the end, or overlap another opening is
  refused with a reason.
- **Rooms** are closed polygons with a name and the floor area, which is
  recomputed whenever the room changes. `wall=$outer` instead of `points=`
  uses a closed wall's inside face. `outline=false` hides the polygon
  (the walls are the outline here); `area=false` hides the area.
- **Dimensions** measure from the outside faces of the wall (-125 and
  8125). `offset` moves the dimension line away from the measured points:
  negative is below for horizontal dimensions, positive is right for
  vertical ones.
- **Delete a wall** and its doors and windows go with it (one undo brings
  all of them back).

The result is three files: `flat.pdf` (vector, prints at 1:50 on A3),
`flat.png` (1654 x 1169 px at 100 dpi) and `flat.dxf`, which opens in
LibreCAD, QCAD, FreeCAD or AutoCAD and back in blueprint with the walls,
doors and rooms still editable.

### one-shot

A one-shot can't build a plan in one command, but the jobs you do with a
finished plan are one-shots: print it again at another size, or make an SVG
or a high-resolution PNG.

```sh
achclwss blueprint.convert input=flat.dxf output=flat-1to100.pdf paper=A4 scale=1:100 title="Flat 2 (reduced)"
achclwss blueprint.convert input=flat.dxf output=flat.svg
achclwss blueprint.convert input=flat.dxf output=flat-300.png dpi=300
```

```
converted flat.dxf to flat-1to100.pdf (A4 at 1:100, 18 entities)
flat-1to100.pdf
path: flat-1to100.pdf
scale: 1:100
converted flat.dxf to flat.svg (A3 at 1:50, 18 entities)
...
converted flat.dxf to flat-300.png (A3 at 1:50, 18 entities)
```

Without `paper`/`scale` the sheet saved in the DXF is used (A3 at 1:50
here). Changing the paper or scale re-centres the drawing; `center=false`
keeps the saved position.

### MCP

With the suite registered as an MCP server (see [the MCP setup](#mcp-setup)),
you can ask:

> Draw a floor plan for a one-bedroom flat, 8 by 6 metres outside, with
> 250 mm outer walls. Split it with a 100 mm wall at 4.5 m so the living
> room is on the left; on the right a bedroom at the back and a bathroom at
> the front. Put the entrance door in the front wall of the living room,
> doors from the living room to the bedroom and from the bedroom to the
> bath, and windows in each room. Label the rooms with their areas,
> dimension everything, and give me an A3 PDF at 1:50.

Claude works through the same commands as tools (dots become underscores):

```json
{"name": "blueprint_new", "arguments": {"units": "mm", "scale": "1:50", "paper": "A3", "title": "Flat 2, ground floor"}}
{"name": "blueprint_layer_add", "arguments": {"doc": "bp1", "name": "WALLS", "weight": 0.5, "current": true}}
{"name": "blueprint_wall", "arguments": {"doc": "bp1", "points": [0, 0, 8000, 0, 8000, 6000, 0, 6000], "closed": true, "thickness": 250}}
{"name": "blueprint_wall", "arguments": {"doc": "bp1", "points": [4500, 125, 4500, 5875], "thickness": 100}}
{"name": "blueprint_door", "arguments": {"doc": "bp1", "wall": "e1", "at": 2000, "width": 1000, "layer": "DOORS"}}
{"name": "blueprint_window", "arguments": {"doc": "bp1", "wall": "e1", "x": 2250, "y": 6000, "width": 1800, "layer": "WINDOWS"}}
{"name": "blueprint_room", "arguments": {"doc": "bp1", "points": [125, 125, 4450, 125, 4450, 5875, 125, 5875], "name": "Living", "outline": false}}
{"name": "blueprint_dimension", "arguments": {"doc": "bp1", "kind": "horizontal", "x1": -125, "y1": -125, "x2": 8125, "y2": -125, "offset": -1200}}
{"name": "blueprint_preview", "arguments": {"doc": "bp1", "view": "sheet"}}
{"name": "blueprint_page", "arguments": {"doc": "bp1", "center": true}}
{"name": "blueprint_export", "arguments": {"doc": "bp1", "path": "flat.pdf"}}
```

Each result carries structured content Claude reads back, for example
`blueprint_room` returns
`{"entity": "e3", "layer": "0", "area": 10972500, "area_text": "10.97 m²", "perimeter": 13400}`.
`blueprint_preview` (or `preview: true` on any drawing command) attaches a
PNG so Claude can look at the plan and fix a door that swings the wrong way
with `blueprint_set` (`{"doc": "bp1", "entities": "e5", "side": "right"}`)
or `undo`.

## 4. task 2: open a DXF and measure it

The job: someone sends you `flat.dxf`. What's in it, how big is the living
room, how long is the diagonal, and can you print an A4 overview?

### one-shot

The read-only commands take `file=` instead of `doc=`, so you don't need to
open anything:

```sh
achclwss blueprint.info file=flat.dxf
```

```
flat.dxf: A3 landscape (420 x 297 mm), scale 1:50, units mm
the paper shows 21000 x 14850 mm; drawing origin at paper bottom-left is -5850,-5825
layers:
  0 color #000000 weight 0.25mm continuous
  WALLS color #000000 weight 0.5mm continuous
  DOORS color #404040 weight 0.25mm continuous
  WINDOWS color #008080 weight 0.25mm continuous
  ROOMS color #808080 weight 0.18mm continuous
  DIMS (current) color #0000ff weight 0.18mm continuous
entities: 18 (6 dimension, 3 door, 3 room, 3 wall, 3 window)
extents: -125,-1425 to 9425,6125 (9550 x 7550)
grid off, snap off
title block on: "Flat 2, ground floor" by A. Clack, 2026-10-16, sheet A-101
...
```

```sh
achclwss blueprint.list file=flat.dxf entities=type:room
```

```
3 of 18 entities in flat.dxf:
e10 room [ROOMS] "Living" closed points=125,125 4450,125 4450,5875 125,5875 area 24.87 m² perimeter 20150
e11 room [ROOMS] "Bedroom" closed points=4550,2650 7875,2650 7875,5875 4550,5875 area 10.72 m² perimeter 13100
e12 room [ROOMS] "Bath" closed points=4550,125 7875,125 7875,2550 4550,2550 area 8.06 m² perimeter 11500
count: 3
ids: e10,e11,e12
```

Measure an entity (area and perimeter of a closed shape or room, length of
a line, polyline or wall, radius/area of a circle, value of a dimension):

```sh
achclwss blueprint.measure file=flat.dxf entity=e10
```

```
e10 area 24868750 (24.87 m²), perimeter 20150
24868750
area: 24868750
area_text: 24.87 m²
perimeter: 20150
```

Measure between points: two points give a distance, three points give the
angle at the middle one with `kind=angle`, three or more give an area with
`kind=area` (the default `auto` picks distance for 2 points and area for
more):

```sh
achclwss blueprint.measure file=flat.dxf points=125,125,4450,5875
achclwss blueprint.measure file=flat.dxf points=4450,125,125,125,4450,5875 kind=angle
```

```
distance 7195 (7195.0069), dx 4325, dy 5750, angle 53.05°
7195.01
distance: 7195.01
dx: 4325
dy: 5750
angle: 53.0505
angle at 125,125: 53.05°
53.0505
angle: 53.0505
```

The first number is formatted the way the drawing's dimensions are (whole
millimetres here); the exact value follows in brackets.

And print an A4 overview at whatever scale fits:

```sh
achclwss blueprint.convert input=flat.dxf output=flat-a4.pdf paper=A4 scale=fit
```

```
converted flat.dxf to flat-a4.pdf (A4 at 1:75, 18 entities)
flat-a4.pdf
path: flat-a4.pdf
scale: 1:75
```

A DXF from another program works the same way. It has no saved sheet, so
it is opened in `mm` (or the units in its `$INSUNITS` header) and fitted to
the paper with `scale=fit` and centred; pass `units=`, `paper=` and `scale=`
to `blueprint.open` or `blueprint.convert` to choose yourself. Blocks
(INSERT), splines and ellipses are skipped; `blueprint.open` says how many.

### the batch script

To add something to the drawing before printing (here a dimension along the
living-room diagonal and a new title), open it in a batch. Save as
`measure.txt`:

```
# measure.txt: open a DXF, find things, measure them, print an overview
blueprint.open path=flat.dxf -> plan
blueprint.info doc=$plan
blueprint.list doc=$plan entities=type:room,type:door

# areas, lengths, distances, angles
blueprint.measure doc=$plan entity=e10
blueprint.measure doc=$plan entity=e1
blueprint.measure doc=$plan points=125,125,4450,5875
blueprint.measure doc=$plan points=4450,125,125,125,4450,5875 kind=angle
blueprint.measure doc=$plan points=125,125,4450,125,4450,5875,125,5875

# mark the living-room diagonal and print an A4 overview at whatever scale fits
blueprint.dimension doc=$plan x1=125 y1=125 x2=4450 y2=5875 offset=900 layer=DIMS -> diag
blueprint.page doc=$plan paper=A4 orientation=portrait scale=fit
blueprint.titleblock doc=$plan title="Flat 2, survey check" sheet=S-1
blueprint.export doc=$plan path=flat-check.pdf
```

```sh
achclwss batch measure.txt
```

```
opened bp1 (A3 landscape 1:50, mm, 18 entities, 6 layers, flat.dxf)
bp1: A3 landscape (420 x 297 mm), scale 1:50, units mm
...
6 of 18 entities in bp1:
e10 room [ROOMS] "Living" closed points=125,125 4450,125 4450,5875 125,5875 area 24.87 m² perimeter 20150
e11 room [ROOMS] "Bedroom" closed points=4550,2650 7875,2650 7875,5875 4550,5875 area 10.72 m² perimeter 13100
e12 room [ROOMS] "Bath" closed points=4550,125 7875,125 7875,2550 4550,2550 area 8.06 m² perimeter 11500
e4 door [DOORS] in e1 at 2000 width 1000 hinge start swing left
e5 door [DOORS] in e2 at 1500 width 800 hinge start swing right
e6 door [DOORS] in e3 at 1200 width 800 hinge end swing left
e10 area 24868750 (24.87 m²), perimeter 20150
e1 centre-line length 28000
distance 7195 (7195.0069), dx 4325, dy 5750, angle 53.05°
angle at 125,125: 53.05°
area 24868750 (24.87 m²), perimeter 20150
added e19 dimension [DIMS] aligned 125,125 -> 4450,5875 = 7195
page A4 portrait at 1:75, origin -3494.5618,-9746.9166, drawing fits on the paper
title block on: "Flat 2, survey check", A. Clack, 2026-10-16, sheet S-1, scale 1:75
exported flat-check.pdf (A4 portrait at 1:75)
```

`kind` defaults to `aligned`, which measures the true distance between the
points at any angle; the title block's scale follows the page unless you
set `scale=` on `blueprint.titleblock` yourself.

### MCP

> Open ~/jobs/flat.dxf. What rooms are in it and how big is each? How far
> is it corner to corner across the living room? Add that as a dimension
> and make me an A4 portrait PDF that fits.

Claude calls:

```json
{"name": "blueprint_open", "arguments": {"path": "/home/me/jobs/flat.dxf"}}
{"name": "blueprint_list", "arguments": {"doc": "bp1", "entities": "type:room"}}
{"name": "blueprint_measure", "arguments": {"doc": "bp1", "points": [125, 125, 4450, 5875]}}
{"name": "blueprint_dimension", "arguments": {"doc": "bp1", "x1": 125, "y1": 125, "x2": 4450, "y2": 5875, "offset": 900, "layer": "DIMS"}}
{"name": "blueprint_page", "arguments": {"doc": "bp1", "paper": "A4", "orientation": "portrait", "scale": "fit"}}
{"name": "blueprint_export", "arguments": {"doc": "bp1", "path": "/home/me/jobs/flat-check.pdf"}}
```

`blueprint_list` gives each room's corners, so Claude can pick the diagonal
from the data instead of guessing; `blueprint_measure` returns
`{"distance": 7195.0069..., "dx": 4325, "dy": 5750, "angle": 53.05...}`.
For a quick question it can skip opening and call
`blueprint_measure` with `"file": "flat.dxf"`.

## 5. task 3: a dimensioned part drawing

The job: a 160 x 100 mm aluminium mounting plate with a V notch, four Ø9
holes, a Ø40 bore with a Ø52 counterbore, centre lines, a hatched section
and a note, full size on A4.

### the batch script

Save as `plate.txt`:

```
# plate.txt: a 160 x 100 mounting plate, full size (1:1) on A4
blueprint.new units=mm scale=1:1 paper=A4 orientation=landscape title="Mounting plate MP-3" author="A. Clack" date=2026-10-16 -> part
blueprint.layer_add doc=$part name=OUTLINE weight=0.5 current=true
blueprint.layer_add doc=$part name=CENTER color=red weight=0.18 linetype=center
blueprint.layer_add doc=$part name=DIMS color=blue weight=0.18
blueprint.layer_add doc=$part name=SECTION weight=0.35
blueprint.dimstyle doc=$part text_height=3 arrow_size=3

# outline with a V notch in the top edge
blueprint.polyline doc=$part points=0,0,160,0,160,100,100,100,80,85,60,100,0,100 closed=true -> outline

# four Ø9 holes: draw one, copy it along, mirror the pair
blueprint.circle doc=$part cx=15 cy=15 diameter=9 -> hole
blueprint.copy doc=$part entities=$hole dx=130 -> hole2
blueprint.mirror doc=$part entities=$hole,$hole2 x1=0 y1=50 x2=160 y2=50 copy=true

# centre bore Ø40 with a counterbore ring 6 mm outside it
blueprint.circle doc=$part cx=80 cy=45 diameter=40 -> bore
blueprint.offset doc=$part entity=$bore distance=6 -> cbore
blueprint.line doc=$part x1=50 y1=45 x2=110 y2=45 layer=CENTER
blueprint.line doc=$part x1=80 y1=15 x2=80 y2=75 layer=CENTER

# dimensions
blueprint.layer_current doc=$part name=DIMS
blueprint.dimension doc=$part kind=horizontal x1=0 y1=0 x2=160 y2=0 offset=-15
blueprint.dimension doc=$part kind=horizontal x1=15 y1=85 x2=145 y2=85 offset=25
blueprint.dimension doc=$part kind=vertical x1=160 y1=0 x2=160 y2=100 offset=15
blueprint.dimension doc=$part kind=vertical x1=145 y1=15 x2=145 y2=85 offset=8
blueprint.dimension doc=$part kind=diameter entity=$bore angle=30
blueprint.dimension doc=$part kind=diameter entity=$cbore angle=150
blueprint.dimension doc=$part kind=diameter entity=$hole angle=45 text="4x <> THRU"
blueprint.dimension doc=$part kind=angular cx=80 cy=85 x1=60 y1=100 x2=100 y2=100 offset=12
blueprint.text doc=$part x=0 y=-35 text="MATERIAL: AL 6082-T6, 10 THICK. BREAK ALL EDGES." height=3 layer=0

# a section through the plate, hatched
blueprint.rect doc=$part x=180 y=0 width=10 height=100 layer=SECTION -> sect
blueprint.hatch doc=$part entity=$sect pattern=lines angle=45 spacing=2 layer=SECTION
blueprint.text doc=$part x=185 y=106 text="A-A" height=4 align=center layer=SECTION

blueprint.page doc=$part center=true
blueprint.measure doc=$part entity=$outline
blueprint.save doc=$part path=plate.dxf
blueprint.export doc=$part path=plate.pdf
blueprint.export doc=$part path=plate.svg
```

```sh
achclwss batch plate.txt
```

```
created bp1 (A4 landscape 1:1, mm, 0 entities, 1 layer)
added layer OUTLINE: color #000000, weight 0.5mm, continuous (current)
added layer CENTER: color #ff0000, weight 0.18mm, center
added layer DIMS: color #0000ff, weight 0.18mm, continuous
added layer SECTION: color #000000, weight 0.35mm, continuous
dimension style: text 3mm, arrow 3mm, precision -1
added e1 polyline [OUTLINE] closed points=0,0 160,0 160,100 100,100 80,85 60,100 0,100 area 15700 mm² perimeter 530
added e2 circle [OUTLINE] centre 15,15 radius 4.5
copied as e3
mirrored e4,e5
added e6 circle [OUTLINE] centre 80,45 radius 20
added e7 circle [OUTLINE] centre 80,45 radius 26
added e8 line [CENTER] 50,45 -> 110,45 length 60 angle 0°
added e9 line [CENTER] 80,15 -> 80,75 length 60 angle 90°
current layer DIMS
added e10 dimension [DIMS] horizontal 0,0 -> 160,0 = 160
added e11 dimension [DIMS] horizontal 15,85 -> 145,85 = 130
added e12 dimension [DIMS] vertical 160,0 -> 160,100 = 100
added e13 dimension [DIMS] vertical 145,15 -> 145,85 = 70
added e14 dimension [DIMS] diameter centre 80,45 = Ø40
added e15 dimension [DIMS] diameter centre 80,45 = Ø52
added e16 dimension [DIMS] diameter centre 15,15 = 4x Ø9 THRU
added e17 dimension [DIMS] angular vertex 80,85 rays to 60,100 and 100,100 = 106.26°
added e18 text [0] "MATERIAL: AL 6082-T6, 10 THICK. BREAK ALL EDGES." at 0,-35 height 3
added e19 polyline [SECTION] closed points=180,0 190,0 190,100 180,100 area 1000 mm² perimeter 220
added e20 hatch [SECTION] closed points=180,0 190,0 190,100 180,100 area 1000 mm² perimeter 220 pattern lines 45° 2mm
added e21 text [SECTION] "A-A" at 185,106 height 4
page A4 landscape at 1:1, origin -53.5,-80.875, drawing fits on the paper
e1 area 15700 mm², perimeter 530
saved plate.dxf (21 entities)
exported plate.pdf (A4 landscape at 1:1)
exported plate.svg (A4 landscape at 1:1)
```

Notes:

- `copy` and `mirror copy=true` return the new ids, so `-> hole2` captures
  them for the next line. `copy count=3 dx=20` makes three copies spaced
  20 apart.
- `offset` on a circle makes a concentric circle; positive goes outside a
  closed shape, or left of a line's direction. `x,y` picks the side by a
  point instead.
- The CENTER layer's line type is drawn as long-dash/short-dash in every
  output; the pattern lengths are in paper mm.
- The angular dimension measures the angle between the rays from `cx,cy`
  through the two points; it always takes the side under 180° unless you
  pass `x,y` on the arc you want. `entity=e3,e4` measures between two lines.
- Diameter labels sit outside the circle along `angle`, so concentric
  circles don't collide.

### one-shot

```sh
achclwss blueprint.measure file=plate.dxf entity=e1
achclwss blueprint.list file=plate.dxf entities=layer:DIMS
achclwss blueprint.convert input=plate.dxf output=plate-copy.pdf
```

```
e1 area 15700 mm², perimeter 530
15700
area: 15700
area_text: 15700 mm²
perimeter: 530
8 of 21 entities in plate.dxf:
e10 dimension [DIMS] horizontal 0,0 -> 160,0 = 160
e11 dimension [DIMS] horizontal 15,85 -> 145,85 = 130
e12 dimension [DIMS] vertical 160,0 -> 160,100 = 100
e13 dimension [DIMS] vertical 145,15 -> 145,85 = 70
e14 dimension [DIMS] diameter centre 80,45 = Ø40
e15 dimension [DIMS] diameter centre 80,45 = Ø52
e16 dimension [DIMS] diameter centre 15,15 = 4x Ø9 THRU
e17 dimension [DIMS] angular vertex 80,85 rays to 60,100 and 100,100 = 106.26°
count: 8
ids: e10,e11,e12,e13,e14,e15,e16,e17
converted plate.dxf to plate-copy.pdf (A4 at 1:1, 21 entities)
```

### MCP

> Make a full-size A4 drawing of a 160 x 100 mm aluminium plate with a V
> notch 20 mm deep in the middle of the top edge, Ø9 holes 15 mm in from
> each corner, a Ø40 bore at 80,45 with a Ø52 counterbore, centre lines on
> a red centre-line layer, all the dimensions, and a hatched section. Send
> me the PDF.

```json
{"name": "blueprint_new", "arguments": {"units": "mm", "scale": "1:1", "paper": "A4", "title": "Mounting plate MP-3"}}
{"name": "blueprint_layer_add", "arguments": {"doc": "bp1", "name": "CENTER", "color": "red", "weight": 0.18, "linetype": "center"}}
{"name": "blueprint_polyline", "arguments": {"doc": "bp1", "points": [0, 0, 160, 0, 160, 100, 100, 100, 80, 85, 60, 100, 0, 100], "closed": true}}
{"name": "blueprint_circle", "arguments": {"doc": "bp1", "cx": 15, "cy": 15, "diameter": 9}}
{"name": "blueprint_copy", "arguments": {"doc": "bp1", "entities": "e2", "dx": 130}}
{"name": "blueprint_mirror", "arguments": {"doc": "bp1", "entities": "e2,e3", "x1": 0, "y1": 50, "x2": 160, "y2": 50, "copy": true}}
{"name": "blueprint_dimension", "arguments": {"doc": "bp1", "kind": "diameter", "entity": "e2", "angle": 45, "text": "4x <> THRU"}}
{"name": "blueprint_hatch", "arguments": {"doc": "bp1", "entity": "e19", "pattern": "lines", "angle": 45}}
{"name": "blueprint_preview", "arguments": {"doc": "bp1", "view": "sheet"}}
{"name": "blueprint_export", "arguments": {"doc": "bp1", "path": "plate.pdf"}}
```

### imperial units

The same commands work in feet or inches; lengths are formatted
feet-and-inches and areas in ft²:

```
# shed.txt: a 12 x 8 ft shed in feet, 1:48 (quarter inch to the foot) on letter paper
blueprint.new units=ft scale=1:48 paper=letter title="Garden shed" -> shed
blueprint.wall doc=$shed points=0,0,12,0,12,8,0,8 closed=true thickness=0.5 -> w
blueprint.door doc=$shed wall=$w at=6 width=3
blueprint.room doc=$shed wall=$w name=Shed
blueprint.dimension doc=$shed kind=horizontal x1=-0.25 y1=-0.25 x2=12.25 y2=-0.25 offset=-2.5
blueprint.measure doc=$shed points=0,0,12,8
blueprint.page doc=$shed center=true
blueprint.export doc=$shed path=shed.pdf
```

```
added e3 room [0] "Shed" closed points=0.25,0.25 11.75,0.25 11.75,7.75 0.25,7.75 area 86.25 ft² perimeter 38
added e4 dimension [0] horizontal -0.25,-0.25 -> 12.25,-0.25 = 12'-6"
distance 14'-5 1/16" (14.4222), dx 12, dy 8, angle 33.69°
```

<a id="mcp-setup"></a>
### MCP setup

Register the suite with Claude Code (all tools, or only blueprint's):

```sh
claude mcp add achclwss -- "$PWD/build/apps/achclwss/achclwss" mcp
claude mcp add achclwss-blueprint -- "$PWD/build/apps/achclwss/achclwss" mcp --tools blueprint
```

For Claude Desktop put the same command in `claude_desktop_config.json`, or
let `hub mcp-config` print it. Tool names are command ids with dots turned
into underscores (`blueprint.wall` is `blueprint_wall`); point lists are
JSON arrays of numbers. The server adds `undo`, `redo`, `history` and
`documents`. Paths are relative to where the server was started, so give
full paths when in doubt.

Tips: ask Claude to `blueprint_list` before talking about specific
entities ("move the bedroom door 300 mm towards the corner" then becomes
`blueprint_set` with `at`), and to preview the sheet before exporting.

## 6. editing: move, copy, rotate, mirror, offset, change

All of these take a selection (`entities=e3,e5`, `all`, `last`,
`layer:DOORS`, `type:wall`) and are one undo step each:

| command | what it does |
|---|---|
| `blueprint.move entities=... dx= dy=` | move; doors and windows follow their wall |
| `blueprint.copy entities=... dx= dy= count=` | copies, each `dx,dy` further on |
| `blueprint.rotate entities=... angle= cx= cy= copy=` | rotate counter-clockwise around `cx,cy` (default: the selection's centre) |
| `blueprint.scale entities=... factor= cx= cy= copy=` | scale around a point |
| `blueprint.mirror entities=... x1= y1= x2= y2= copy=` | mirror about the line through two points |
| `blueprint.offset entity= distance= x= y=` | parallel copy of a line, polyline, circle or arc |
| `blueprint.delete entities=...` | delete; deleting a wall deletes its openings |
| `blueprint.set entities=... layer= color= text= ...` | change properties: layer, colour, text/height/rotation, room name/area/outline, opening at/width/hinge/side, wall thickness/fill, hatch pattern, closed |

`blueprint.grid snap=50` rounds every coordinate you give to drawing
commands to the nearest 50; `spacing=` and `show=true` draw a grid in
previews. Locked layers refuse edits; hidden layers don't print.

## 7. exporting and printing

`blueprint.export path=...` writes the whole sheet: the drawing at scale,
the border and the title block. The extension picks the format:

- **.pdf**: vector, one page the size of the paper. Lines keep their layer
  weights and line types, arcs are true curves, text is Helvetica. Print it
  at **100% / actual size** (not "fit to page") and a 1:50 drawing measures
  correctly with a scale rule.
- **.svg**: vector, `width`/`height` in real millimetres, one group per
  layer (named so Inkscape shows them as layers).
- **.png, .jpg, .bmp**: rendered at `dpi=` (default 150; A3 at 300 dpi is
  4961 x 3508 px). Text uses a built-in stroke font.

`blueprint.preview` renders a PNG for a quick look: `view=extents` (default)
fits the drawing, `view=sheet` shows the page as it will print. Any drawing
command also takes `preview=true`.

`blueprint.titleblock` sets the title, author, date, sheet number and an
optional scale text; `show=false` turns the block (and the border) off.

## 8. DXF: what goes in and out

`blueprint.save` writes an AutoCAD 2000 (AC1015) ASCII DXF with real
tables (layers with colour, true colour, line weight and line type; line
types; text and dimension styles), real DIMENSION entities with their
geometry blocks, HATCH entities, and the sheet settings, so it opens in
LibreCAD, QCAD, FreeCAD, AutoCAD and other DXF readers. Walls, doors,
windows and rooms are written as the lines, arcs and text you see, tagged
with extended data so blueprint turns them back into editable walls and
rooms when you reopen the file. `sheet=true` also writes the border and
title block on a layer `BP_SHEET` (ignored when reading back).

`blueprint.open` reads LINE, LWPOLYLINE (arc segments are flattened),
POLYLINE/VERTEX, CIRCLE, ARC, POINT, TEXT, MTEXT (formatting stripped),
SOLID/TRACE, HATCH (outer boundary) and DIMENSION (linear, aligned, angular,
radius, diameter), plus layers with their colours, weights and line types.
INSERT (blocks), ELLIPSE, SPLINE and other types are skipped and counted in
the message.

## 9. command and parameter reference

Every command and parameter, as `achclwss help <command>` prints them.
Required parameters are in **bold**. Most drawing commands also take
`layer=` and `preview=true`.

| command | parameters | notes |
|---|---|---|
| `blueprint.new` | `units` (mm/cm/m/in/ft, default mm), `paper` (A4 A3 A2 A1 A0 letter legal tabloid, default A4), `orientation` (landscape/portrait), `scale` (default 1:1), `title`, `author`, `date` | returns `doc` |
| `blueprint.open` | **`path`**, `units`, `paper`, `orientation`, `scale` (or `fit`), `center` | opens a DXF, returns `doc` |
| `blueprint.save` | **`doc`**, **`path`**, `sheet` | DXF |
| `blueprint.export` | **`doc`**, **`path`**, `dpi` (150), `quality` (90) | .pdf .svg .png .jpg .bmp |
| `blueprint.convert` | **`input`**, **`output`**, `paper`, `orientation`, `scale`, `center`, `dpi`, `title` | DXF to PDF/SVG/PNG/DXF in one go |
| `blueprint.close` | **`doc`** | undoable |
| `blueprint.info` | `doc` or `file` | sheet, layers, counts, extents |
| `blueprint.preview` | **`doc`**, `size` (px), `view` (extents/sheet), `grid` | PNG attachment |
| `blueprint.page` | **`doc`**, `paper`, `orientation`, `scale` (`1:50`, `2:1`, `fit`), `origin_x`, `origin_y`, `center` | origin = drawing point at the paper's bottom-left |
| `blueprint.titleblock` | **`doc`**, `title`, `author`, `date`, `sheet`, `scale`, `show` | |
| `blueprint.dimstyle` | **`doc`**, `text_height`, `arrow_size` (paper mm), `arrows` (arrow/tick/dot), `precision` (-1 = by unit), `suffix`, `ext_offset`, `ext_beyond` | applies to all dimensions |
| `blueprint.grid` | **`doc`**, `spacing`, `snap`, `show` | drawing units, 0 = off |
| `blueprint.layer_add` | **`doc`**, **`name`**, `color`, `weight` (paper mm), `linetype` (continuous/dashed/center/hidden), `current` | |
| `blueprint.layer_set` | **`doc`**, **`name`**, `color`, `weight`, `linetype`, `visible`, `locked`, `rename` | |
| `blueprint.layer_current` | **`doc`**, **`name`** | creates the layer if missing |
| `blueprint.layers` | **`doc`** | list |
| `blueprint.line` | **`doc`**, **`x1`**, **`y1`**, `x2`, `y2` or `length` + `angle` | |
| `blueprint.polyline` | **`doc`**, **`points`**, `closed` | |
| `blueprint.rect` | **`doc`**, **`x`**, **`y`**, **`width`**, **`height`** | closed polyline |
| `blueprint.circle` | **`doc`**, **`cx`**, **`cy`**, `radius` or `diameter` | |
| `blueprint.arc` | **`doc`**, **`cx`**, **`cy`**, **`radius`**, **`start`**, **`end`** | degrees, counter-clockwise from +x |
| `blueprint.text` | **`doc`**, **`x`**, **`y`**, **`text`**, `height` (drawing units), `rotation`, `align` (left/center/right), `valign` (baseline/middle/top) | single line |
| `blueprint.point` | **`doc`**, **`x`**, **`y`** | |
| `blueprint.dimension` | **`doc`**, `kind` (aligned/horizontal/vertical/radius/diameter/angular), `x1 y1 x2 y2`, `x y` or `offset`, `entity`, `cx cy`, `radius`, `angle`, `text` | see task 3 |
| `blueprint.hatch` | **`doc`**, `points` or `entity`, `pattern` (solid/lines/cross), `angle` (45), `spacing` (paper mm, 2) | |
| `blueprint.wall` | **`doc`**, **`points`**, `thickness` (200 mm), `closed`, `justify` (center/left/right), `fill` (none/solid/lines) | |
| `blueprint.door` | **`doc`**, **`wall`**, `at` or `x y`, `width` (900 mm), `hinge` (start/end), `side` (left/right) | |
| `blueprint.window` | **`doc`**, **`wall`**, `at` or `x y`, `width` (1200 mm) | |
| `blueprint.room` | **`doc`**, `points` or `wall`, `name`, `area`, `outline`, `label_x`, `label_y` | area label in m² or ft² |
| `blueprint.move` | **`doc`**, **`entities`**, `dx`, `dy` | |
| `blueprint.copy` | **`doc`**, **`entities`**, `dx`, `dy`, `count` | returns the new ids |
| `blueprint.rotate` | **`doc`**, **`entities`**, **`angle`**, `cx`, `cy`, `copy` | |
| `blueprint.scale` | **`doc`**, **`entities`**, **`factor`**, `cx`, `cy`, `copy` | |
| `blueprint.mirror` | **`doc`**, **`entities`**, **`x1 y1 x2 y2`**, `copy` | |
| `blueprint.offset` | **`doc`**, **`entity`**, **`distance`**, `x`, `y` | |
| `blueprint.delete` | **`doc`**, **`entities`** | |
| `blueprint.set` | **`doc`**, **`entities`**, `layer`, `color`, `text`, `height`, `rotation`, `name`, `area`, `outline`, `at`, `width`, `hinge`, `side`, `thickness`, `fill`, `pattern`, `angle`, `spacing`, `closed` | change properties |
| `blueprint.list` | `doc` or `file`, `entities` (selection, default all) | |
| `blueprint.measure` | `doc` or `file`; `entity` (one entity, or two lines for the angle between them) or `points` with `kind` (auto/distance/area/angle) | |

## 10. troubleshooting and limitations

- **"doesn't fit inside one straight run of the wall"** (door/window):
  openings can't wrap round a corner or hang off a wall's end. Move `at`, or pick the spot with `x,y`.
- **The plan is off the paper**: `blueprint.page doc=... center=true`, or
  `scale=fit`. `blueprint.info` prints how much of the drawing the paper
  shows.
- **Text is tiny or huge**: `blueprint.text height=` is in drawing units;
  at 1:50 a 3.5 mm-on-paper label is `height=175`. Leave `height` out to get
  that automatically.
- **Separate walls meeting at a corner overlap**: draw a corner as one
  wall with several points (`points=` with the corner in the middle) so it
  gets a mitred joint. Only open wall *ends* butting into another wall's
  side are joined (T-junctions).
- **Dimensions don't follow edits**: a dimension stores its own points.
  Move it together with the geometry (`entities=e3,e14`), or delete it and
  add it again; the value is always measured from its points.
- **Printing is not to scale**: print the PDF at 100% / actual size.

Known limitations of this version:

- no trim/extend, fillet/chamfer, splines or ellipses; text is single-line
  and Helvetica only (PNG export uses a built-in stroke font);
- DXF import skips blocks (INSERT), ELLIPSE and SPLINE; polyline arc
  segments are flattened to short lines; a hatch keeps only its outer
  boundary (no holes); per-entity line type overrides are ignored (layer
  line types are kept); hatch spacing from foreign files is converted to
  paper mm;
- the DXF writer has been checked with ezdxf's audit and by reading
  it back into blueprint, not with AutoCAD; it writes no LAYOUT
  objects, which AutoCAD rebuilds on open.
