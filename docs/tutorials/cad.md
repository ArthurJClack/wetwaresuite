# cad tutorial: precise parametric parts

`cad` is the suite's parametric CAD tool, in the spirit of FreeCAD's Part
Design or OpenSCAD, built headless first. A part is a list of named
**parameters** (`width = 40`, `margin = width/5`) and an ordered **feature
tree** (an extruded profile, a hole, a pattern of that hole, ...). Every number
in every feature can be an expression over the parameters, and the solid is
rebuilt from the tree whenever anything changes. Change `width` and the plate,
the hole spacing and the rib all follow.

Everything is a command, so the same part can be built three ways: typed at the
shell, written as a batch script, or described to Claude, which calls the same
commands over MCP and sees a picture after each step. Every example in this
tutorial was run against the real `wetwaresuite` binary, and the outputs shown are
what it printed (long outputs are shortened with `...`).

## contents

1. [the ideas you need](#1-the-ideas-you-need)
2. [setting up](#2-setting-up)
3. [task: a parametric mounting bracket](#3-task-a-parametric-mounting-bracket)
4. [task: change one parameter to resize everything](#4-task-change-one-parameter-to-resize-everything)
5. [task: export for 3D printing](#5-task-export-for-3d-printing)
6. [task: measure a part](#6-task-measure-a-part)
7. [task: a flange with a circular bolt pattern](#7-task-a-flange-with-a-circular-bolt-pattern)
8. [editing the tree: suppress, rename, delete, undo](#8-editing-the-tree-suppress-rename-delete-undo)
9. [expressions](#9-expressions)
10. [how features combine](#10-how-features-combine)
11. [command reference](#11-command-reference)
12. [limits and troubleshooting](#12-limits-and-troubleshooting)

## 1. the ideas you need

**A part is a document.** `cad.new` (or `cad.open`) gives back an id like
`cad1`; every other command names it with `doc=cad1`. Several parts can be open
at once.

**Units are millimetres unless you say otherwise.** `cad.new units=in` (or
`cm`, `m`) only changes the label and the mass calculation; numbers are used as
you type them. STL and 3MF files are read by slicers as millimetres.

**Z is up.** The XY plane is the floor. A box's `width` runs along X, `depth`
along Y and `height` along Z.

**Parameters are named numbers.** `cad.param_set name=width value=40`. A value
can use other parameters: `value=width/5`. Parameters are evaluated every time
the part regenerates, so changing one updates everything that uses it.

**Features are the steps that build the solid.** Each has a name (you pick it
with `name=`, or you get `box1`, `hole2`, ...). Primitive and sketch features
(box, cylinder, extrude, ...) make new bodies. Operations (boolean, hole,
pattern, mirror, transform) take earlier features by name and replace them with
their result. Whatever is left unconsumed at the end of the tree is a *body*;
the bodies together are the part you preview and export.

**Every numeric field takes an expression.** `x=(foot+t+gusset)/2`,
`dy=width-2*margin`, `countersink_diameter=bolt*2`. Point lists too:
`points="0,0, foot,0, foot,t, t,t, t,rise, 0,rise"`. Quote a value that contains
spaces.

**Every edit is one undo step**, and an edit that would break the part (a
negative size, an unknown name, a hole that can't be cut) is refused with the
reason and changes nothing. Reading commands (`cad.info`, `cad.preview`,
`cad.measure_*`) never touch the history.

**The tree is saved, not just the mesh.** `.wwcad` files keep the parameters
and features, so you can reopen a part next month and change one number. Mesh
formats (STL, OBJ, PLY, OFF, glTF, 3MF) and images are exports. Parts saved
before the rename (`.achcad`) still open with `cad.open`.

## 2. setting up

Build the suite once (see the README for compilers):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
alias wetwaresuite="$PWD/build/apps/wetwaresuite/wetwaresuite"
```

See the cad commands and the parameters of one:

```sh
wetwaresuite list cad
wetwaresuite help cad.hole
```

```
cad.hole: Hole
Drill a round hole into a feature: diameter, depth (0 = through all), along axis (-z drills downwards). x/y/z is the entry point (0 if left out), except that the coordinate along the axis defaults to the entry face of the target. ...

usage: cad.hole doc=<string> target=<string> diameter=<string> [depth=<string>] [axis=-z|z|-x|x|-y|y] [x=<string>] ...
  doc (string, required)  id of an open cad document, e.g. cad1
  target (string, required)  feature to drill (name of an earlier feature)
  diameter (string, required)  hole diameter (number or expression)
  depth (string)  hole depth, 0 = through all (number or expression) (default 0)
  ...
```

Numeric fields are typed `string` because they accept expressions; a plain
number like `40` is fine everywhere.

### three ways to drive it

- **CLI**: `wetwaresuite <command> key=value ...` runs one command in a fresh
  process. Open documents live only inside one process, so a one-shot command
  can create or inspect a part (`wetwaresuite cad.open path=bracket.wwcad`) but
  can't keep editing it. For edits, pipe a few lines into `wetwaresuite batch` from
  the shell (shown in every task below) or use a script file.
- **Batch script**: a text file with one command per line, run with
  `wetwaresuite batch part.txt`. `-> name` at the end of a line saves the result
  (here the document id) and `$name` reuses it. `#` starts a comment. `undo` and
  `redo` work as lines. The script stops at the first failing line unless you
  add `--keep-going`.
- **Claude over MCP**: `wetwaresuite mcp` (or `wetwaresuite mcp --tools cad`) serves
  every command as a tool named with `_` instead of `.` (`cad_hole`). Connect
  it once (see [mcp.md](mcp.md)):

  ```sh
  claude mcp add wetwaresuite -- /path/to/wetwaresuite/build/apps/wetwaresuite/wetwaresuite mcp --tools cad
  ```

  Every edit tool takes `preview=true` (and `view=iso|front|top|...`), which
  attaches a PNG render of the part to the result, so Claude sees what it
  built after each step.

## 3. task: a parametric mounting bracket

The goal: an L-shaped bracket, 40 mm wide, with a 50 mm foot and a 45 mm
upright, 5 mm thick, a triangular rib for stiffness, two M5 clearance holes in
the foot and two countersunk M5 holes in the upright. Every dimension is a
parameter, so the same file makes the next size up.

### as a batch script

Save this as `bracket.txt`:

```text
# bracket.txt: a parametric L-shaped mounting bracket
cad.new units=mm -> part

# the numbers that define the part
cad.param_set doc=$part name=width value=40      # along Y
cad.param_set doc=$part name=foot value=50       # base length along X
cad.param_set doc=$part name=rise value=45       # upright height along Z
cad.param_set doc=$part name=t value=5           # wall thickness
cad.param_set doc=$part name=bolt value=5.5      # M5 clearance hole
cad.param_set doc=$part name=margin value=width/5
cad.param_set doc=$part name=gusset value=min(foot,rise)/2

# the L profile, drawn in the XZ plane and extruded along +Y
cad.extrude doc=$part name=body plane=xz points="0,0, foot,0, foot,t, t,t, t,rise, 0,rise" height=width
# a triangular rib in the middle, overlapping both walls a little
cad.extrude doc=$part name=rib plane=xz points="t/2,t/2, t+gusset,t/2, t/2,t+gusset" height=t y=(width-t)/2
cad.boolean doc=$part name=bracket op=union a=body b=rib

# two bolt holes through the foot, then two countersunk ones through the upright
cad.hole doc=$part name=foot_hole target=bracket diameter=bolt x=(foot+t+gusset)/2 y=margin
cad.pattern_linear doc=$part name=foot_holes target=foot_hole count=2 dy=width-2*margin
cad.hole doc=$part name=wall_hole target=foot_holes diameter=bolt axis=x y=margin z=rise-margin countersink_diameter=bolt*2
cad.pattern_linear doc=$part name=wall_holes target=wall_hole count=2 dy=width-2*margin

cad.info doc=$part
cad.save doc=$part path=bracket.wwcad
cad.save doc=$part path=bracket.stl
cad.save doc=$part path=bracket.png size=800
```

```sh
wetwaresuite batch bracket.txt
```

Each edit prints what it did and the state of the model:

```
created cad1 (0 features, 0 params, 0 bodies, mm)
...
added extrude body (1 body, volume 18000 mm^3)
added extrude rib (2 bodies, volume 19562.5 mm^3)
added boolean bracket (1 body, volume 19000 mm^3)
added hole foot_hole (1 body, volume 18881.4 mm^3)
added pattern_linear foot_holes (1 body, volume 18762.8 mm^3)
added hole wall_hole (1 body, volume 18557.2 mm^3)
added pattern_linear wall_holes (1 body, volume 18351.6 mm^3)
```

and `cad.info` describes the whole part:

```
cad1: mm, 7 params, 7 features, 1 bodies
parameters:
  width = 40
  foot = 50
  rise = 45
  t = 5
  bolt = 5.5
  margin = width/5 (=8)
  gusset = min(foot,rise)/2 (=22.5)
features:
  body extrude [used by bracket] height=width (=40) plane=xz points=0,0, foot,0, foot,t, t,t, t,rise, 0,rise bounds=(0,0,0)-(50,40,45)
  rib extrude [used by bracket] height=t (=5) plane=xz points=t/2,t/2, t+gusset,t/2, t/2,t+gusset y=(width-t)/2 (=17.5) bounds=(2.5,17.5,2.5)-(27.5,22.5,27.5)
  bracket boolean [used by foot_hole] a=body b=rib op=union bounds=(0,0,0)-(50,40,45)
  foot_hole hole [used by foot_holes] diameter=bolt (=5.5) target=bracket x=(foot+t+gusset)/2 (=38.75) y=margin (=8) bounds=(0,0,0)-(50,40,45)
  foot_holes pattern_linear [used by wall_hole] count=2 dy=width-2*margin (=24) target=foot_hole bounds=(0,0,0)-(50,40,45)
  wall_hole hole [used by wall_holes] axis=x countersink_diameter=bolt*2 (=11) diameter=bolt (=5.5) target=foot_holes y=margin (=8) z=rise-margin (=37) bounds=(0,0,0)-(50,40,45)
  wall_holes pattern_linear [body] count=2 dy=width-2*margin (=24) target=wall_hole bounds=(0,0,0)-(50,40,45)
bodies:
  wall_holes: volume 18351.6, 1328 triangles, 658 vertices, size 50 x 40 x 45 at (0, 0, 0), watertight
model: size 50 x 40 x 45 mm, min (0, 0, 0), max (50, 40, 45), volume 18351.6 mm^3, area 8961.17 mm^2, watertight
```

What each step did:

1. **The profile.** `plane=xz` draws the sketch with sketch x along X and sketch
   y along Z, and extrudes it along +Y by `height=width`. The six points walk
   round the L: along the floor, up the end of the foot, back to the inside
   corner, up the upright and down its back.
2. **The rib** is a triangle in the same plane, `t` thick, centred on the width
   with `y=(width-t)/2`. Its corner starts at `t/2,t/2`, inside the walls, so
   the union below welds it in rather than just touching.
3. **`cad.boolean op=union`** joins `body` and `rib` into one body called
   `bracket`. Both inputs are now "used by bracket".
4. **The first foot hole.** `cad.hole` drills along `axis=-z` (the default:
   downwards) from the entry point. Only `x` and `y` are given; `z` defaults to
   the top face of the target. `depth` defaults to 0, meaning through all.
5. **`cad.pattern_linear` on a hole repeats the hole**, not the whole bracket:
   two copies, `dy=width-2*margin` apart.
6. **The upright holes** drill along `+x` from the back face (`x` is left out,
   so it starts at the face the drill enters: the back of the upright at x=0),
   with a 90 degree countersink of twice the bolt diameter on that side.
7. **`cad.save`** writes the tree to `bracket.wwcad`, the solid to
   `bracket.stl`, and an 800x800 iso render to `bracket.png`.

### from the shell

The same thing without a script file: pipe the lines into `batch`.

```sh
wetwaresuite batch <<'EOF'
cad.new -> p
cad.param_set doc=$p name=width value=40
cad.param_set doc=$p name=t value=5
cad.extrude doc=$p name=body plane=xz points="0,0, 50,0, 50,t, t,t, t,45, 0,45" height=width
cad.hole doc=$p name=foot_hole target=body diameter=5.5 x=38 y=8
cad.pattern_linear doc=$p target=foot_hole count=2 dy=width-16
cad.save doc=$p path=quick-bracket.wwcad
EOF
```

Single commands are for things that fit in one step. Open a saved part and
print its summary:

```sh
wetwaresuite cad.open path=bracket.wwcad
```

```
opened cad1 (7 features, 7 params, 1 bodies, mm, 1328 triangles, 658 vertices, size 50 x 40 x 45 at (0, 0, 0), watertight, bracket.wwcad)
cad1
doc: cad1
units: mm
features: 7
bodies: 1
```

### asking Claude

With the MCP server connected, describe the part:

> Design a parametric L mounting bracket in cad: 40 mm wide, 50 mm foot, 45 mm
> upright, 5 mm walls, a triangular rib in the middle, two M5 clearance holes
> in the foot and two countersunk M5 holes in the upright, 8 mm from the edges.
> Make every dimension a parameter. Show me a preview, then save it as
> bracket.wwcad and bracket.stl.

Claude works through the same commands, roughly:

1. `cad_new` with `units: "mm"` gets `cad1`.
2. `cad_param_set` once per parameter (`width`, `foot`, `rise`, `t`, `bolt`,
   `margin`, ...).
3. `cad_extrude` with `plane: "xz"`, the L `points` and `height: "width"`,
   then `cad_extrude` for the rib and `cad_boolean` to join them, passing
   `preview: true` to check the shape.
4. `cad_hole` and `cad_pattern_linear` for each pair of holes
   (`countersink_diameter` on the upright).
5. `cad_info` to confirm one watertight body, `cad_preview` with
   `view: "front"` or `"top"` to check the hole positions.
6. `cad_save` twice.

Numeric arguments can be sent as numbers (`"width": 40`) or as expression
strings (`"width": "foot-10"`).

## 4. task: change one parameter to resize everything

The bracket needs to be 60 mm wide and 6 mm thick, with three foot holes.
Nothing else needs touching: the margin is `width/5`, the hole spacing is
`width-2*margin`, the rib follows `t`.

### as a batch script

```text
# resize.txt: make the bracket wider and thicker, with three foot holes
cad.open path=bracket.wwcad -> part
cad.param_set doc=$part name=width value=60
cad.param_set doc=$part name=t value=6
cad.feature_set doc=$part feature=foot_holes values="count=3; dy=(width-2*margin)/2"
cad.info doc=$part
cad.save doc=$part path=bracket-wide.wwcad
cad.save doc=$part path=bracket-wide.stl
```

```
opened cad1 (7 features, 7 params, 1 bodies, mm, 1328 triangles, 658 vertices, size 50 x 40 x 45 at (0, 0, 0), watertight, bracket.wwcad)
width = 60 (1 body, volume 27351.6 mm^3)
t = 6 (1 body, volume 32437.5 mm^3)
set foot_holes count=3, dy=(width-2*margin)/2 (1 body, volume 32295.2 mm^3)
...
model: size 50 x 60 x 45 mm, min (0, 0, 0), max (50, 60, 45), volume 32295.2 mm^3, area 13024.7 mm^2, watertight
saved bracket-wide.wwcad
saved bracket-wide.stl
```

`cad.feature_set` changes fields of an existing feature. `param=count
value=3` changes one; `values="count=3; dy=..."` changes several in one undo
step. Only the features after the change are rebuilt: results are cached by
their inputs, so the profile and rib aren't recomputed when only a hole moves.

### from the shell

A whole family of sizes in one loop, each exported to its own STL, with the
mass of each (the default density is PLA, 1.25 g/cm^3):

```sh
for w in 30 40 60; do
  wetwaresuite batch <<EOF | grep '^mass_g'
cad.open path=bracket.wwcad -> p
cad.param_set doc=\$p name=width value=$w
cad.save doc=\$p path=bracket-$w.stl
cad.measure_mass doc=\$p
EOF
done
```

```
mass_g: 17.3146
mass_g: 22.9396
mass_g: 34.1896
```

(The `$p` is escaped because this heredoc is unquoted so the shell can fill in
`$w`.)

### asking Claude

> Open bracket.wwcad, make it 60 mm wide and 6 mm thick, and put three holes
> in the foot instead of two, evenly spaced. Show me the top view and save it
> as bracket-wide.stl.

Claude calls `cad_open`, `cad_param_list` (to see the names), `cad_param_set`
for `width` and `t`, `cad_feature_set` on `foot_holes` with
`values: "count=3; dy=(width-2*margin)/2"`, `cad_preview` with `view: "top"`
and `cad_save`. If a change breaks the part, the tool result says why (for
example `feature body: extrude height must be positive`) and nothing changes,
so Claude can correct it.

## 5. task: export for 3D printing

STL is what every slicer reads; 3MF is the newer format that keeps units. The
extension picks the format:

```text
cad.save doc=$part path=bracket.stl            # binary STL
cad.save doc=$part path=bracket.stl ascii=true # text STL
cad.save doc=$part path=bracket.3mf
cad.save doc=$part path=bracket.obj
cad.save doc=$part path=bracket.glb            # glTF binary, for viewers and game engines
cad.save doc=$part path=bracket.png size=1024 view=iso
```

A mesh export reports whether the solid is watertight, which slicers need:

```
saved bracket.stl
bracket.stl
path: bracket.stl
triangles: 1328
watertight: true
```

Before printing, check the part's size and that it's one closed body
(`cad.info`), and estimate the filament with `cad.measure_mass` (section 6).

From the shell, export and render a preview of the export in one go (the
`convert` tool renders any mesh to an image):

```sh
wetwaresuite batch <<'EOF'
cad.open path=bracket.wwcad -> p
cad.param_set doc=$p name=width value=80
cad.save doc=$p path=bracket-80.stl
EOF
wetwaresuite convert.file input=bracket-80.stl output=bracket-80.png
```

```
...
saved bracket-80.stl
bracket-80.stl
path: bracket-80.stl
triangles: 1328
watertight: true
rendered bracket-80.stl to bracket-80.png
...
```

Asking Claude: *"Export the bracket as STL for printing and tell me how much
PETG it takes."* Claude calls `cad_info` (checks `watertight: true`),
`cad_save` with `path: "bracket.stl"` and `cad_measure_mass` with
`density: "1.27"`.

## 6. task: measure a part

### as a batch script

```text
# measure.txt: check the bracket before printing it
cad.open path=bracket.wwcad -> part
cad.measure_bounds doc=$part
cad.measure_mass doc=$part density=1.27
cad.measure_section doc=$part z=t/2
cad.measure_section doc=$part z=rise-margin
cad.measure_distance doc=$part from="(foot+t+gusset)/2, margin, t" to="(foot+t+gusset)/2, width-margin, t"
cad.measure_distance doc=$part from=body to=rib
cad.measure_bounds doc=$part feature=rib
```

```
size 50 x 40 x 45 mm, min (0, 0, 0), max (50, 40, 45), centre (25, 20, 22.5)
...
volume 18351.6 mm^3 (18.3516 cm^3), area 8961.17 mm^2, mass 23.3066 g at 1.27 g/cm^3
...
section at z=2.5: area 1952.56 mm^2, 3 loops, perimeter 214.544 mm
...
section at z=37: area 129.875 mm^2, 3 loops, perimeter 81.5563 mm
...
distance 24 mm (dx 0, dy 24, dz 0)
...
distance 12.5 mm (dx -10, dy 0, dz -7.5)
...
size 25 x 5 x 25 mm, min (2.5, 17.5, 2.5), max (27.5, 22.5, 27.5), centre (15, 20, 15)
```

Reading the results:

- **Bounds**: the whole part fits in 50 x 40 x 45 mm.
- **Mass**: 18.35 cm^3 of PETG (1.27 g/cm^3) is about 23.3 g at 100% infill.
  Common densities: PLA 1.25, PETG 1.27, ABS 1.04, aluminium 2.70, steel 7.85.
- **Sections** cut the part with a horizontal plane. Through the foot
  (`z=t/2`) the cross-section is the 50 x 40 plate minus the two holes: 3 loops
  (the outline and two holes) and 1952.56 mm^2, which is 2000 minus two 5.5 mm
  holes. Through the upright at the hole height, the two holes cut the wall
  into 3 separate pieces.
- **Distances** take two `x,y,z` points (expressions allowed) or feature names,
  which measure between the centres of their bounding boxes. The foot holes
  are 24 mm apart, centre to centre.
- **`feature=`** measures one feature's own result instead of the whole part:
  here the rib before it was joined.

Measurements change nothing and never enter the undo history, so they're safe
to run any time.

### from the shell

```sh
wetwaresuite batch <<'EOF' | grep '^volume\|^mass_g'
cad.open path=bracket.wwcad -> p
cad.measure_mass doc=$p density=1.27
EOF
```

```
volume 18351.6 mm^3 (18.3516 cm^3), area 8961.17 mm^2, mass 23.3066 g at 1.27 g/cm^3
volume: 18351.6
volume_cm3: 18.3516
mass_g: 23.3066
```

### asking Claude

> How heavy is bracket.wwcad in aluminium, what's the cross-section area of
> the foot, and how far apart are the foot holes?

Claude calls `cad_open`, `cad_measure_mass` with `density: "2.70"`,
`cad_measure_section` with `z: "t/2"` and `cad_measure_distance` between the
two hole centres. The numbers come back as structured fields (`mass_g`,
`area`, `distance`), so it can quote or compare them exactly.

## 7. task: a flange with a circular bolt pattern

A pipe flange: a 90 mm ring with a 40 mm bore, 8 mm thick, and six
counterbored M6 holes on a bolt circle halfway between the bore and the rim.

```text
# flange.txt: a round pipe flange with a circular bolt pattern
cad.new -> f
cad.param_set doc=$f name=od value=90          # outside diameter
cad.param_set doc=$f name=bore value=40        # pipe bore
cad.param_set doc=$f name=thick value=8
cad.param_set doc=$f name=pcd value=(od+bore)/2  # bolt circle diameter
cad.param_set doc=$f name=bolts value=6
cad.param_set doc=$f name=bolt value=6.6       # M6 clearance

cad.extrude_circle doc=$f name=disc diameter=od inner_diameter=bore height=thick segments=96
cad.hole doc=$f name=bolt_hole target=disc diameter=bolt x=pcd/2 counterbore_diameter=11 counterbore_depth=3
cad.pattern_circular doc=$f name=bolt_circle target=bolt_hole count=bolts
cad.measure_section doc=$f z=1
cad.measure_mass doc=$f density=2.70
cad.save doc=$f path=flange.wwcad
cad.save doc=$f path=flange.3mf
cad.save doc=$f path=flange.png view=top size=600
```

```
added extrude_circle disc (1 body, volume 40811.6 mm^3)
added hole bolt_hole (1 body, volume 40356.1 mm^3)
added pattern_circular bolt_circle (1 body, volume 38079 mm^3)
section at z=1: area 4896.5 mm^2, 8 loops, perimeter 532.691 mm
volume 38079 mm^3 (38.079 cm^3), area 14303.2 mm^2, mass 102.813 g at 2.7 g/cm^3
saved flange.wwcad
saved flange.3mf
saved flange.png
```

The section has 8 loops: the rim, the bore and six holes. `count=bolts` is an
expression too, so `cad.param_set doc=$f name=bolts value=8` re-spaces the
pattern to eight holes at 45 degrees. `pattern_circular` turns about Z through
the origin by default (`axis`, `cx`, `cy`, `cz` move it), and `angle` smaller
than 360 spreads the copies over an arc instead.

Asking Claude: *"Make a 90 mm aluminium flange for a 40 mm pipe, 8 mm thick,
six counterbored M6 holes on the bolt circle; tell me its weight."* It calls
`cad_new`, `cad_param_set` for each dimension, `cad_extrude_circle`,
`cad_hole`, `cad_pattern_circular` with `preview: true` and `view: "top"`, then
`cad_measure_mass` with `density: "2.70"`.

## 8. editing the tree: suppress, rename, delete, undo

```text
# edit.txt: change the tree, see what is refused, undo
cad.open path=bracket.wwcad -> p
cad.param_set doc=$p name=t value=-2
cad.param_delete doc=$p name=bolt
cad.feature_delete doc=$p feature=rib
cad.feature_suppress doc=$p feature=rib
cad.feature_list doc=$p
undo
cad.rename doc=$p feature=wall_holes to=finished
cad.feature_set doc=$p feature=wall_hole param=countersink_diameter value=
cad.feature_set doc=$p feature=wall_hole values="counterbore_diameter=10; counterbore_depth=2"
undo
undo
cad.param_list doc=$p
```

```sh
wetwaresuite batch --keep-going edit.txt
```

```
line 3: feature rib: extrude height must be positive
line 4: can't delete bolt: used by feature foot_hole, feature wall_hole
line 5: can't delete rib: used by bracket
suppressed rib (1 body, volume 17351.6 mm^3)
...
rib extrude [suppressed] height=t (=5) plane=xz points=t/2,t/2, t+gusset,t/2, t/2,t+gusset y=(width-t)/2 (=17.5)
...
undone
renamed wall_holes to finished (1 body, volume 18351.6 mm^3)
set wall_hole countersink_diameter=(default) (1 body, volume 18525.6 mm^3)
set wall_hole counterbore_diameter=10, counterbore_depth=2 (1 body, volume 18306.8 mm^3)
undone
undone
width = 40
...
```

The three refused lines are on purpose, so the script exits with status 1
(any failed line does, which is what lets shell scripts and CI notice).

- A **negative thickness** is refused: the rib's extrusion height is `t`. The
  part is left exactly as it was.
- A **parameter in use** can't be deleted, and neither can a **feature that a
  later feature uses**. Delete or change the user first.
- **Suppressing** a feature switches it off without deleting it. The union that
  used the rib now passes the body straight through, so the bracket has no rib
  (1000 mm^3 lighter). A suppressed hole leaves its target undrilled; a
  suppressed boolean leaves both inputs as separate bodies.
- **Renaming** updates every feature that refers to the old name.
- **An empty value** (`value=`) resets a field to its default: no countersink.
- `undo` and `redo` step through every edit, including closing a part.

Asking Claude: *"Take the rib off the bracket for now and switch the upright
holes to 10 mm counterbores, 2 mm deep."* Claude calls `cad_feature_suppress`
with `feature: "rib"` and `cad_feature_set` with `feature: "wall_hole"`,
`values: "countersink_diameter=; counterbore_diameter=10; counterbore_depth=2"`.
To go back it calls `undo`.

## 9. expressions

Every numeric field and every parameter value is an expression:

| syntax | meaning |
|---|---|
| `40`, `2.5`, `.5`, `1e3` | numbers |
| `width`, `hole_d` | a parameter (letters, digits and `_`, starting with a letter) |
| `+ - * /` | arithmetic; `%` is the remainder |
| `^` | power, right-associative: `2^3^2` = 512; `-2^2` = -4 |
| `( )` | grouping |
| `pi` | 3.14159... |
| `sin cos tan` | trig in **degrees**: `sin(30)` = 0.5 |
| `asin acos atan atan2(y, x)` | inverse trig, answering in degrees |
| `sqrt abs floor ceil round` | one argument |
| `min(a, b, ...)`, `max(a, b, ...)` | any number of arguments |
| `pow(a, b)` | same as `a^b` |

A parameter can use any parameter that already exists (`margin = width/5`
once `width` is set), and is re-evaluated whenever that one changes; a loop
(`a = b+1` then `b = a`) is refused. Division by zero, unknown
names and anything that isn't a finite number are errors that name the field,
e.g. `feature plate: width: bad expression 'wdth': unknown name 'wdth' (define
it with cad.param_set)`.

Point lists (`points`, `hole_points`) are comma-separated expressions, read as
x,y pairs: `points="0,0, w,0, w,h, 0,h"`. Commas inside function calls don't
split: `points="0,0, min(a,b),0, 0,5"`.

## 10. how features combine

- **Order matters.** Operations can only use features earlier in the tree.
  The tree is rebuilt from the top whenever something changes.
- **Consumed features stop being bodies.** After `cad.boolean a=body b=rib`,
  `body` and `rib` are "used by" the boolean, and only the boolean is a body.
  Their own results are still there for `cad.measure_* feature=...` and
  `cad.preview feature=...`.
- **Holes are cuts on a body.** `target` is the feature to drill. Patterns and
  mirrors of a hole repeat the cut on the same body, which is how you make bolt
  patterns. Patterns and mirrors of anything else copy the body (copies that
  touch or overlap are unioned into one solid).
- **Through holes** (`depth=0`) go through everything in the target along the
  axis. Blind holes have flat bottoms. The hole's entry point defaults to the
  target's face on the side the drill comes from, so `axis=-z` starts at the
  top and `axis=x` at the -X face.
- **Round things are polygons.** Cylinders, holes and arcs have `segments`
  sides (64 by default) with their corners exactly on the radius, so a hole's
  area is slightly smaller than the true circle (0.16% at 64 segments). Raise
  `segments` for more accuracy, lower it for speed.
- **Sketch planes.** `plane=xy` (default) draws on the floor and extrudes up;
  `plane=xz` maps sketch x to X and y to Z and extrudes along +Y; `plane=yz`
  maps sketch x to Y and y to Z and extrudes along +X. `x y z rx ry rz` then
  move and rotate the result (rotation about X, then Y, then Z).
- **Imported meshes** (`cad.open` of an STL, or `cad.import`) become a fixed
  `imported` feature that you can drill, pattern or combine with booleans like
  any other.
- **The regenerated solid is a mesh.** A cad document is also a mesh
  document, so tools that read meshes can take `doc=cad1` and see the current
  solid (edits they make to the mesh are replaced at the next regeneration).

## 11. command reference

Every edit command also takes `preview=true` (attach a PNG render) and
`view=iso|front|back|left|right|top|bottom`. Primitive and sketch features take
the placement fields `x y z` (move, default 0) and `rx ry rz` (rotate in
degrees about the feature's own origin, X first, then Y, then Z). Fields marked
required must be given; every other field has the default shown or is optional.

### Documents

#### `cad.new`: New Part

Create an empty parametric part and return its id (e.g. cad1). Then define parameters with cad.param_set and add features (cad.box, cad.extrude_rect, cad.hole, ...). Z is up.

| param | required | default | meaning |
|---|---|---|---|
| `units` |  | `mm` | length unit, used for mass [mm\|cm\|m\|in] |

#### `cad.open`: Open Part

Open a .wwcad part (parameters and feature tree), or import a mesh file (stl, obj, ply, off, glb, gltf, 3mf) as a new part with one fixed 'imported' feature. Returns the document id.

| param | required | default | meaning |
|---|---|---|---|
| `path` | yes |  | path to a .wwcad or mesh file |

#### `cad.import`: Import Mesh

Add a mesh file (stl, obj, ply, off, glb, gltf, 3mf) to a part as a fixed 'imported' feature, which booleans, holes and patterns can use like any other.

| param | required | default | meaning |
|---|---|---|---|
| `path` | yes |  | mesh file |
| `scale` |  | `1` | uniform scale |
| `name` |  | `imported1, ...` | feature name |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.save`: Save Part

Save a part. .wwcad keeps the parameters and feature tree (reopen with cad.open); stl, obj, ply, off, glb, gltf or 3mf export the solid; png, jpg, bmp, tga or ppm render a picture (view, size).

| param | required | default | meaning |
|---|---|---|---|
| `path` | yes |  | output file; the extension picks the format |
| `ascii` |  | `false` | STL/PLY: write text instead of binary |
| `size` |  | `1024` | image exports: width and height in px |

#### `cad.close`: Close Part

Close a part (undo reopens it).


#### `cad.info`: Part Info

Describe a part: units, every parameter with its value, every feature in order with its status (body, used by another feature, suppressed), the resulting bodies and the whole model's bounding box, volume, surface area and whether it is watertight.


#### `cad.preview`: Preview Part

Render the part (or one feature's own result) to a PNG so you can see it: view iso, front, back, left, right (or side), top, bottom.

| param | required | default | meaning |
|---|---|---|---|
| `size` |  | `512` | image width and height in px |
| `feature` |  |  | render only this feature's result |

### Parameters

#### `cad.param_set`: Set Parameter

Create or change a named parameter. value is a number or an expression over other parameters (e.g. width/2+3, sqrt(a^2+b^2), min(w,h)). Every feature using it regenerates; if the new value breaks a feature the change is refused with the reason.

| param | required | default | meaning |
|---|---|---|---|
| `name` | yes |  | parameter name: letters, digits, _ (e.g. width) |
| `value` | yes |  | number or expression |

#### `cad.param_list`: List Parameters

List every parameter with its expression and current value.


#### `cad.param_delete`: Delete Parameter

Delete a parameter. Refused while any feature or parameter uses it.

| param | required | default | meaning |
|---|---|---|---|
| `name` | yes |  | parameter name |

### Primitive features

#### `cad.box`: Box

Add a box (cuboid) with exact sizes: width along X, depth along Y, height along Z. By default its min corner sits at x/y/z; center=true centres it there instead. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `width` | yes |  | size along X |
| `depth` | yes |  | size along Y |
| `height` | yes |  | size along Z |
| `center` |  | `false` | centre the box on x/y/z |
| `name` |  |  | name for the feature (default: type + number, e.g. box1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.cylinder`: Cylinder

Add a cylinder along Z. Give diameter or radius. Its base circle is centred on x/y/z (center=true centres it in height too). Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `diameter` |  |  | diameter (or give radius) |
| `radius` |  |  | radius (or give diameter) |
| `height` | yes |  | height along Z |
| `segments` |  | `64` | sides of the polygon |
| `center` |  | `false` | centre in height as well |
| `name` |  |  | name for the feature (default: type + number, e.g. cylinder1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.cone`: Cone

Add a cone or frustum along Z from bottom diameter d1 to top diameter d2 (either may be 0; or give r1/r2). Base centred on x/y/z. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `d1` |  |  | bottom diameter |
| `d2` |  |  | top diameter |
| `r1` |  |  | bottom radius (instead of d1) |
| `r2` |  |  | top radius (instead of d2) |
| `height` | yes |  | height along Z |
| `segments` |  | `64` | sides of the polygon |
| `center` |  | `false` | centre in height as well |
| `name` |  |  | name for the feature (default: type + number, e.g. cone1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.sphere`: Sphere

Add a sphere centred on x/y/z. Give diameter or radius. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `diameter` |  |  | diameter (or give radius) |
| `radius` |  |  | radius (or give diameter) |
| `segments` |  | `64` | segments around Z |
| `rings` |  | `32` | rings from pole to pole |
| `name` |  |  | name for the feature (default: type + number, e.g. sphere1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.torus`: Torus

Add a torus (ring) around Z centred on x/y/z: major = distance to the tube centre, minor = tube radius. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `major` | yes |  | ring radius to the tube centre |
| `minor` | yes |  | tube radius |
| `segments` |  | `64` | segments around the ring |
| `tube_segments` |  | `24` | segments around the tube |
| `name` |  |  | name for the feature (default: type + number, e.g. torus1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

### Sketch features

#### `cad.extrude`: Extrude Sketch

Extrude a 2D polygon sketch by height. points is x,y,x,y,... (each may be an expression, e.g. "0,0, w,0, w,h, 0,h"); hole_points cuts one hole, cad.add_hole adds more. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `points` | yes |  | outer outline x,y,x,y,... (at least 3 points) |
| `height` | yes |  | extrusion distance |
| `hole_points` |  |  | optional hole outline x,y,x,y,... |
| `plane` |  | `xy` | sketch plane: xy extrudes along +Z, xz (x->X, y->Z) along +Y, yz (x->Y, y->Z) along +X [xy\|xz\|yz] |
| `name` |  |  | name for the feature (default: type + number, e.g. extrude1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.add_hole`: Add Sketch Hole

Add another hole outline to an extrude feature's sketch (the first can be given as hole_points).

| param | required | default | meaning |
|---|---|---|---|
| `feature` | yes |  | an extrude feature |
| `points` | yes |  | hole outline x,y,x,y,... (numbers or expressions) |

#### `cad.revolve`: Revolve Sketch

Revolve a closed 2D profile about the Z axis: points are r,z pairs (r >= 0 is the distance from the axis). angle < 360 gives a partial revolve with flat ends. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `points` | yes |  | profile r,z,r,z,... (at least 3 points) |
| `angle` |  | `360` | sweep in degrees, (0, 360] |
| `segments` |  | `64` | segments for a full turn |
| `name` |  |  | name for the feature (default: type + number, e.g. revolve1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.extrude_rect`: Extrude Rectangle

Extrude a rectangle (radius > 0 rounds its corners) by height. Its min corner sits at x/y (or its centre with center=true). Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `width` | yes |  | size along sketch x |
| `depth` | yes |  | size along sketch y |
| `height` | yes |  | extrusion distance |
| `radius` |  | `0` | corner radius, 0 = sharp |
| `segments` |  | `8` | segments per rounded corner |
| `center` |  | `false` | centre the rectangle on x/y |
| `plane` |  | `xy` | sketch plane: xy extrudes along +Z, xz (x->X, y->Z) along +Y, yz (x->Y, y->Z) along +X [xy\|xz\|yz] |
| `name` |  |  | name for the feature (default: type + number, e.g. extrude_rect1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.extrude_circle`: Extrude Circle

Extrude a circle (or a ring with inner_diameter) centred on x/y by height. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `diameter` |  |  | outer diameter (or give radius) |
| `radius` |  |  | outer radius (or give diameter) |
| `height` | yes |  | extrusion distance |
| `inner_diameter` |  | `0` | makes a tube; 0 = solid |
| `segments` |  | `64` | sides of the polygon |
| `plane` |  | `xy` | sketch plane: xy extrudes along +Z, xz (x->X, y->Z) along +Y, yz (x->Y, y->Z) along +X [xy\|xz\|yz] |
| `name` |  |  | name for the feature (default: type + number, e.g. extrude_circle1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

#### `cad.extrude_slot`: Extrude Slot

Extrude a slot (stadium: a rectangle with round ends) centred on x/y, long along sketch x, by height. length is the overall length, width the slot width. Placed by x/y/z and rotated by rx/ry/rz (degrees, about its own origin). Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `length` | yes |  | overall length along sketch x |
| `width` | yes |  | slot width |
| `height` | yes |  | extrusion distance |
| `segments` |  | `32` | segments per round end |
| `plane` |  | `xy` | sketch plane: xy extrudes along +Z, xz (x->X, y->Z) along +Y, yz (x->Y, y->Z) along +X [xy\|xz\|yz] |
| `name` |  |  | name for the feature (default: type + number, e.g. extrude_slot1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

### Operations

#### `cad.boolean`: Boolean

Combine two earlier features into one body: union, difference (a minus b) or intersection. Both inputs stop being separate bodies. Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `op` |  | `union` | union, difference (a - b) or intersection [union\|difference\|intersection] |
| `a` | yes |  | first feature (the one kept for difference) |
| `b` | yes |  | second feature (the cutter for difference) |
| `name` |  |  | name for the feature (default: type + number, e.g. boolean1) |

#### `cad.hole`: Hole

Drill a round hole into a feature: diameter, depth (0 = through all), along axis (-z drills downwards). x/y/z is the entry point (0 if left out), except that the coordinate along the axis defaults to the entry face of the target. Optional countersink (countersink_diameter, countersink_angle) or counterbore (counterbore_diameter, counterbore_depth). Patterning or mirroring a hole repeats the hole. Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `target` | yes |  | feature to drill |
| `diameter` | yes |  | hole diameter |
| `depth` |  | `0` | hole depth, 0 = through all |
| `axis` |  | `-z` | drilling direction [-z\|z\|-x\|x\|-y\|y] |
| `x` |  |  | entry point X |
| `y` |  |  | entry point Y |
| `z` |  |  | entry point Z |
| `countersink_diameter` |  | `0` | countersink top diameter, 0 = none |
| `countersink_angle` |  | `90` | countersink included angle in degrees |
| `counterbore_diameter` |  | `0` | counterbore diameter, 0 = none |
| `counterbore_depth` |  | `0` | counterbore depth |
| `segments` |  | `64` | sides of the hole polygon |
| `name` |  |  | name for the feature (default: type + number, e.g. hole1) |

#### `cad.pattern_linear`: Linear Pattern

Repeat a feature count times, each copy offset by dx/dy/dz; count2 with dx2/dy2/dz2 makes a grid. On a hole feature it repeats the hole. Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `target` | yes |  | feature to repeat |
| `count` | yes |  | number of copies including the original |
| `dx` |  | `0` | step along X |
| `dy` |  | `0` | step along Y |
| `dz` |  | `0` | step along Z |
| `count2` |  | `1` | copies in the second direction (grid) |
| `dx2` |  | `0` | second step X |
| `dy2` |  | `0` | second step Y |
| `dz2` |  | `0` | second step Z |
| `name` |  |  | name for the feature (default: type + number, e.g. pattern_linear1) |

#### `cad.pattern_circular`: Circular Pattern

Repeat a feature count times around an axis through cx/cy/cz. angle=360 spaces them evenly; a smaller angle spreads them from the original to angle. On a hole feature it repeats the hole. Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `target` | yes |  | feature to repeat |
| `count` | yes |  | number of copies including the original |
| `angle` |  | `360` | total sweep in degrees |
| `axis` |  | `z` | rotation axis [z\|x\|y] |
| `cx` |  | `0` | axis point X |
| `cy` |  | `0` | axis point Y |
| `cz` |  | `0` | axis point Z |
| `name` |  |  | name for the feature (default: type + number, e.g. pattern_circular1) |

#### `cad.mirror`: Mirror

Mirror a feature across a plane (yz flips X, xz flips Y, xy flips Z) at offset along its normal. keep=true keeps the original too and joins them. On a hole feature it mirrors the hole. Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `target` | yes |  | feature to mirror |
| `plane` |  | `yz` | mirror plane [yz\|xz\|xy] |
| `offset` |  | `0` | plane position along its normal |
| `keep` |  | `true` | keep the original as well |
| `name` |  |  | name for the feature (default: type + number, e.g. mirror1) |

#### `cad.transform`: Transform

Move, rotate and scale an earlier feature's body: scale and rotate (rx, ry, rz degrees) about the pivot cx/cy/cz, then move by x/y/z. Returns the feature name.

| param | required | default | meaning |
|---|---|---|---|
| `target` | yes |  | feature to transform |
| `scale` |  | `1` | uniform scale |
| `cx` |  | `0` | pivot X |
| `cy` |  | `0` | pivot Y |
| `cz` |  | `0` | pivot Z |
| `name` |  |  | name for the feature (default: type + number, e.g. transform1) |
| `x` `y` `z` `rx` `ry` `rz` | | `0` | placement (see above) |

### Editing the tree

#### `cad.feature_set`: Edit Feature

Change parameters of an existing feature, then regenerate everything after it: param=width value=60, or several at once with values="width=60; height=5". An empty value resets a field to its default. Refused (and rolled back) if the tree no longer builds.

| param | required | default | meaning |
|---|---|---|---|
| `feature` | yes |  | feature name, e.g. plate or hole1 (cad.feature_list shows them) |
| `param` |  |  | field to change, e.g. width, diameter, count |
| `value` |  |  | new value: number, expression, feature name or choice |
| `values` |  |  | several changes: key=value pairs separated by ';' |

#### `cad.feature_delete`: Delete Feature

Remove a feature. Refused while a later feature uses it (delete that first).

| param | required | default | meaning |
|---|---|---|---|
| `feature` | yes |  | feature name, e.g. plate or hole1 (cad.feature_list shows them) |

#### `cad.feature_suppress`: Suppress Feature

Turn a feature off without deleting it: it produces nothing, operations on it are skipped, and a boolean whose cutter is suppressed keeps its first input.

| param | required | default | meaning |
|---|---|---|---|
| `feature` | yes |  | feature name, e.g. plate or hole1 (cad.feature_list shows them) |

#### `cad.feature_unsuppress`: Unsuppress Feature

Turn a suppressed feature back on.

| param | required | default | meaning |
|---|---|---|---|
| `feature` | yes |  | feature name, e.g. plate or hole1 (cad.feature_list shows them) |

#### `cad.rename`: Rename Feature

Rename a feature; features that refer to it follow.

| param | required | default | meaning |
|---|---|---|---|
| `feature` | yes |  | feature name, e.g. plate or hole1 (cad.feature_list shows them) |
| `to` | yes |  | new name (letters, digits, _ and -) |

#### `cad.feature_list`: List Features

List the feature tree in order: name, type, status, every set field (with evaluated values) and bounds.


### Measuring

#### `cad.measure_distance`: Measure Distance

Distance between two points. Each end is an x,y,z point (numbers or expressions) or a feature name, which means the centre of its bounding box.

| param | required | default | meaning |
|---|---|---|---|
| `from` | yes |  | x,y,z or a feature name |
| `to` | yes |  | x,y,z or a feature name |

#### `cad.measure_bounds`: Measure Bounds

Bounding box of the model or of one feature: min, max, size and centre.

| param | required | default | meaning |
|---|---|---|---|
| `feature` |  |  | measure just this feature's own result (default: the whole model) |

#### `cad.measure_mass`: Measure Mass

Volume, surface area and mass of the model or one feature. density is in g/cm^3 (default 1.25, PLA; PETG 1.27, ABS 1.04, aluminium 2.70, steel 7.85) and may be an expression.

| param | required | default | meaning |
|---|---|---|---|
| `feature` |  |  | measure just this feature's own result (default: the whole model) |
| `density` |  | `1.25` | g/cm^3, number or expression |

#### `cad.measure_section`: Measure Section

Cut the model (or one feature) with the horizontal plane Z = z and report the cross-section: area (holes subtracted), number of loops and perimeter.

| param | required | default | meaning |
|---|---|---|---|
| `z` | yes |  | height of the cut, number or expression |
| `feature` |  |  | measure just this feature's own result (default: the whole model) |

## 12. limits and troubleshooting

**Not there yet.** Fillets, chamfers and shells (hollowing) aren't available;
round an outline with `cad.extrude_rect radius=...` or `cad.extrude_slot`, or
build a rounded profile and `cad.revolve` it. There are no sketch constraints
(sketches are point lists with expressions), no threads (model the clearance
hole and use a tap or a heat-set insert), and no drill-point cone at the bottom
of blind holes.

**Solids are meshes.** Booleans are BSP-tree CSG on the triangles; results are
checked to be closed and watertight, flat faces are re-triangulated cleanly,
and a cut that comes out open is retried with the cutter nudged by a millionth
of the part's size. Round features are polygons (section 10). If `cad.info`
ever reports "not watertight" after a boolean of exactly coincident faces,
change one value by a hair (0.01 mm) or let the two solids overlap a little
instead of meeting face to face, as the bracket's rib does.

**"no open cad document called 'cad1'"**: each `wetwaresuite <command>` run is a
new process with nothing open. Use a batch script or the MCP server for
anything with more than one step.

**"bad expression ..."**: the message names the feature and field. Quote
expressions that contain spaces: `x="(a + b) / 2"`.

**"... refers to 'name', which comes later in the tree"**: operations can only
use features defined before them. Delete the operation and add it again after
its inputs.

**"can't delete ...: used by ..."**: delete or change the features (or
parameters) listed first, or suppress the feature instead.

**A hole doesn't appear**: check `axis` (the default `-z` drills down from
the top face) and that `x y z` are inside the target; `cad.measure_section` at
the hole's height shows how many loops the cut made, and `cad.preview
view=top` shows where it went.

**Big parts are slow to regenerate**: lower `segments` on cylinders and holes
while designing (e.g. 24), then raise them for the final export. Only features
after a change are recomputed.
