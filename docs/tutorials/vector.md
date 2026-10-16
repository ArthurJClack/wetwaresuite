# vector tutorial: drawing with commands

`vector` is the suite's Illustrator/Inkscape-style drawing tool. It is built
headless first: every drawing action is a command you can type, put in a
script, or let Claude call over MCP. This tutorial goes from your first
rectangle to a complete logo, then shows how Claude drives the same commands.

You don't need to know SVG beforehand. Everything here was run against the
real tool, and the outputs shown are what it prints.

## contents

1. [the five ideas you need](#1-the-five-ideas-you-need)
2. [setting up](#2-setting-up)
3. [one-shot commands](#3-one-shot-commands)
4. [your first drawing, step by step](#4-your-first-drawing-step-by-step)
5. [a full batch script: a logo](#5-a-full-batch-script-a-logo)
6. [path data, the pen tool in text form](#6-path-data-the-pen-tool-in-text-form)
7. [styling, moving, stacking](#7-styling-moving-stacking)
8. [opening existing SVG files](#8-opening-existing-svg-files)
9. [exporting](#9-exporting)
10. [letting Claude draw over MCP](#10-letting-claude-draw-over-mcp)
11. [command reference](#11-command-reference)
12. [troubleshooting](#12-troubleshooting)

## 1. the five ideas you need

**Documents have ids.** `vector.new` and `vector.open` give back an id like
`vec1`. Every later command names the document it works on with `doc=vec1`.

**Shapes have ids too.** Every shape you add gets an id like `s1`, `s2`, ...
in the order you add them. You can also give a shape a `name=` and use that
name wherever a shape id is expected (`shape=sun`).

**Coordinates are pixels from the top-left.** `x` grows to the right, `y`
grows *downwards*. On an 800x600 canvas the bottom-right corner is `800,600`.

**Shapes stack bottom first.** The first shape you add is at the bottom; each
new shape is drawn on top. `vector.reorder` changes that.

**Every edit can be undone.** Adding, styling, moving, deleting, reordering
and even closing a document are single undo steps. Reading commands
(`vector.info`, `vector.preview`, `vector.save`) don't touch the history.

Colours are written as `#rrggbb` (`#ff0000`), `#rrggbbaa` with alpha
(`#ff000080` is half-transparent red), `#rgb`, `rgb(255,0,0)`, a name like
`red`, `orange`, `white`, or `none` for no paint at all.

## 2. setting up

Build the suite once (see the README for compilers):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The umbrella binary is `build/apps/achclwss/achclwss`. To keep the commands
below short, either add that folder to your `PATH` or make an alias:

```sh
alias achclwss="$PWD/build/apps/achclwss/achclwss"
```

Check that the vector commands are there, and ask for help on one:

```sh
achclwss list vector
achclwss help vector.add_rect
```

`help` prints every parameter with its type, whether it's required, and its
default, for example:

```
vector.add_rect: Add Rectangle
Add a rectangle (optionally with rounded corners) on top of the drawing. Returns the new shape id.

usage: vector.add_rect doc=<string> x=<number> y=<number> width=<number> height=<number> [radius=<number>] [fill=<string>] ...
```

Parameters are always written `key=value`. Put quotes around a value that has
spaces in it: `d="M10 10 L90 90"`, `path="my drawing.svg"`.

## 3. one-shot commands

`achclwss run <command> key=value ...` runs a single command and exits
(`achclwss <command> ...` without `run` does the same):

```sh
achclwss run vector.new width=800 height=600
```

```
created vec1 (800x600, 0 shapes)
vec1
doc: vec1
width: 800
height: 600
shapes: 0
```

Each one-shot run starts with nothing open, so the document above is gone as
soon as the command finishes. One-shots are for jobs that fit in one command.
The most useful one is `vector.convert`, which turns an SVG file into a PNG
(or JPEG, BMP, TGA, PPM, or a cleaned-up SVG) without opening it first:

```sh
achclwss run vector.convert input=logo.svg output=logo.png
achclwss run vector.convert input=logo.svg output=logo@2x.png scale=2
achclwss run vector.convert input=logo.svg output=thumb.jpg scale=0.25 quality=80
```

For anything with more than one step, use a batch script.

## 4. your first drawing, step by step

A batch script is a text file with one command per line. Make a file called
`first.txt`:

```text
vector.new width=200 height=200 -> pic
vector.add_rect doc=$pic x=50 y=50 width=100 height=100 fill=red
vector.save doc=$pic path=first.png
```

and run it:

```sh
achclwss batch first.txt
```

What happened, line by line:

1. `vector.new width=200 height=200` made a 200x200 white canvas. The
   `-> pic` at the end saved its id (`vec1`) in a variable called `pic`.
2. `doc=$pic` was replaced by `doc=vec1` before the command ran. A red
   100x100 square went in the middle (its top-left corner is at 50,50).
3. `vector.save` rendered the drawing to `first.png`.

You can also pipe a script in instead of making a file:

```sh
echo 'vector.new width=200 height=200 -> pic
vector.add_ellipse doc=$pic cx=100 cy=100 rx=80 fill=orange stroke=black stroke_width=4
vector.save doc=$pic path=circle.svg' | achclwss batch
```

Leaving out `ry` on `vector.add_ellipse` gives a circle.

The batch rules, all in one place:

| rule | example |
| --- | --- |
| one command per line | `vector.add_rect doc=$pic x=0 y=0 width=10 height=10` |
| `#` starts a comment (at the start of a line or after a space) | `# the background` |
| `-> name` saves the result: the doc id for new/open, the shape id for add/duplicate | `vector.add_rect ... -> box` |
| `$name` is replaced by the saved value | `shape=$box` |
| `undo` and `redo` work as commands | `undo` |
| `echo text` prints text | `echo done` |
| the script stops at the first failing line | add `--keep-going` to carry on |

## 5. a full batch script: a logo

This script draws a badge with a mountain range, a sun and a caption bar. Save
it as `logo.txt`:

```text
# logo.txt: a badge with a mountain, a sun and a caption bar
# one command per line; "-> name" saves the result, "$name" uses it

vector.new width=400 height=400 background=none -> logo

# the round badge
vector.add_ellipse doc=$logo cx=200 cy=200 rx=180 fill=#1d2b53 stroke=#ffa300 stroke_width=8 name=badge

# the sun, slightly see-through
vector.add_ellipse doc=$logo cx=270 cy=150 rx=40 fill=#ffa300 opacity=0.9 name=sun

# two mountains; the second is a copy of the first, moved and recoloured
vector.add_polygon doc=$logo points=80,290,170,150,260,290 fill=#29adff name=peak -> peak
vector.duplicate doc=$logo shape=$peak dx=90 dy=10 -> peak2
vector.style doc=$logo shape=$peak2 fill=#83769c
vector.transform doc=$logo shape=$peak2 scale=0.8

# a snow cap drawn with path data (relative commands)
vector.add_path doc=$logo d="M170 150 l-30 46 l15 -8 l15 10 l15 -10 l15 8 z" fill=white name=snow

# a caption bar with rounded corners, then put the sun behind the mountains
vector.add_rect doc=$logo x=90 y=300 width=220 height=40 radius=20 fill=#ff004d -> bar
vector.reorder doc=$logo shape=sun to=back
vector.reorder doc=$logo shape=sun to=up

# a wavy line under the caption bar
vector.add_path doc=$logo d="M125 352 q19 -12 38 0 t38 0 t38 0 t38 0" fill=none stroke=#ffa300 stroke_width=4

vector.info doc=$logo
vector.save doc=$logo path=logo.svg
vector.save doc=$logo path=logo.png scale=2
```

Run it:

```sh
achclwss batch logo.txt
```

You get `logo.svg` (a real SVG you can open in Inkscape, Illustrator or a
browser) and `logo.png` at 800x800, because `scale=2` renders two pixels per
unit. The background outside the badge is transparent because of
`background=none`.

Things worth noticing in the script:

- **Captures hold shape ids too.** `-> peak` saved `s3`, so `shape=$peak`
  means `shape=s3`. `-> peak2` saved the id of the copy (`s4`).
- **Names work like ids.** `shape=sun` finds the ellipse named `sun`.
- **Points are a flat list.** `points=80,290,170,150,260,290` is the three
  corners (80,290), (170,150) and (260,290). Spaces or semicolons work as
  separators too, if you quote the value.
- **Transforms add up.** The copy was moved by `dx=90 dy=10` when it was
  duplicated, then scaled by 0.8 around its own centre.
- **Reordering is relative.** `to=back` put the sun under everything, then
  `to=up` moved it one step up, so it sits on the badge but behind the peaks.

`vector.info` describes every shape bottom to top, with its bounding box on
the canvas after transforms:

```
vec1: 400x400, background none, 7 shapes (bottom first)
s1 ellipse "badge" cx=200 cy=200 rx=180 ry=180 fill=#1d2b53 stroke=#ffa300 stroke_width=8 bounds=20,20 360x360
s2 ellipse "sun" cx=270 cy=150 rx=40 ry=40 fill=#ffa300 stroke=none opacity=0.9 bounds=230,110 80x80
s3 polygon "peak" points=80,290 170,150 260,290 fill=#29adff stroke=none bounds=80,150 180x140
s4 polygon "peak copy" points=80,290 170,150 260,290 fill=#83769c stroke=none transform=(move 90,10 scale 0.8,0.8) bounds=188,174 144x112
s5 path "snow" d="M170 150 l-30 46 l15 -8 l15 10 l15 -10 l15 8 z" fill=#ffffff stroke=none bounds=140,150 60x48
s6 rect "rect 6" x=90 y=300 w=220 h=40 radius=20 fill=#ff004d stroke=none bounds=90,300 220x40
s7 path "path 7" d="M125 352 q19 -12 38 0 t38 0 t38 0 t38 0" fill=none stroke=#ffa300 stroke_width=4 bounds=125,346.05 152x11.901
```

Try changing things and running it again: swap colours, add `rotate=-10` to
the `vector.transform` line, or put `undo` on its own line after any command
to see that step disappear.

## 6. path data, the pen tool in text form

`vector.add_path` takes SVG path data in `d=`. It's a list of commands, each
a letter followed by numbers. **Uppercase letters use absolute coordinates;
lowercase letters are relative to where the pen currently is.**

| command | numbers | what it does |
| --- | --- | --- |
| `M x y` / `m dx dy` | 2 | move the pen without drawing (starts a new piece) |
| `L x y` / `l` | 2 | straight line |
| `H x` / `h` | 1 | horizontal line |
| `V y` / `v` | 1 | vertical line |
| `C x1 y1 x2 y2 x y` / `c` | 6 | cubic curve with two control points |
| `S x2 y2 x y` / `s` | 4 | smooth cubic: the first control point mirrors the last one |
| `Q x1 y1 x y` / `q` | 4 | quadratic curve with one control point |
| `T x y` / `t` | 2 | smooth quadratic: the control point mirrors the last one |
| `A rx ry rotation large sweep x y` / `a` | 7 | elliptical arc to x,y |
| `Z` / `z` | 0 | close the piece back to where it started |

Some examples to paste into a script (each needs a `doc=`):

```text
# a triangle
vector.add_path doc=$d d="M50 10 L90 90 L10 90 Z" fill=teal

# the same triangle with relative moves
vector.add_path doc=$d d="M50 10 l40 80 h-80 z" fill=teal

# a heart from two arcs and two curves
vector.add_path doc=$d d="M50 30 A20 20 0 0 1 90 30 Q90 60 50 90 Q10 60 10 30 A20 20 0 0 1 50 30 Z" fill=crimson

# a ring: a second piece inside the first makes a hole
vector.add_path doc=$d d="M30 50 a20 20 0 1 0 40 0 a20 20 0 1 0 -40 0 Z M40 50 a10 10 0 1 0 20 0 a10 10 0 1 0 -20 0 Z" fill=gold

# an open wave: no fill, just an outline
vector.add_path doc=$d d="M10 50 q10 -20 20 0 t20 0 t20 0 t20 0" fill=none stroke=navy stroke_width=3
```

Arcs explained: `A rx ry rotation large sweep x y` draws part of an ellipse
with radii rx, ry from the pen to x,y. Two ellipses (and four arcs) can join
two points; `large=1` picks the longer way round and `sweep=1` picks the
clockwise one. If the radii are too small to reach, they're scaled up.

Fills use the **even-odd** rule: a point is filled when a line from it to
outside crosses the outline an odd number of times. That's why a piece
inside another piece becomes a hole.

Numbers can be written compactly like other SVG tools write them:
`M10-5L.5.5` is `M 10 -5 L 0.5 0.5`. Bad path data is refused with the
position of the problem, so nothing broken gets added.

## 7. styling, moving, stacking

All of these take `doc=` and `shape=` (an id like `s3` or a name). Add
`preview=true` to any edit to get a PNG of the result back (handy over MCP).

**Style.** Change only what you pass:

```text
vector.style doc=$d shape=s2 fill=none stroke=#000 stroke_width=3
vector.style doc=$d shape=sun opacity=0.5 name=pale-sun
vector.style doc=$d shape=s4 visible=false
```

`opacity` fades the whole shape, fill and outline together. To fade only the
fill, give it an alpha colour: `fill=#ff000080`.

**Transform.** Moves, turns and scales add on to what's already there:

```text
vector.transform doc=$d shape=s1 dx=20 dy=-10     # 20 right, 10 up
vector.transform doc=$d shape=s1 rotate=45        # 45 degrees clockwise around its centre
vector.transform doc=$d shape=s1 scale=1.5        # 150% around its centre
vector.transform doc=$d shape=s1 scale_x=2        # stretch sideways
vector.transform doc=$d shape=s1 reset=true       # back to how it was drawn
```

**Align** a shape's bounding box to the canvas:

```text
vector.align doc=$d shape=title horizontal=center vertical=top margin=20
```

**Stacking order:**

```text
vector.reorder doc=$d shape=s5 to=front   # top of the pile
vector.reorder doc=$d shape=s5 to=back    # bottom
vector.reorder doc=$d shape=s5 to=up      # one step up
vector.reorder doc=$d shape=s5 to=down    # one step down
```

**Duplicate and delete:**

```text
vector.duplicate doc=$d shape=s2 dx=30 -> copy   # the copy sits just above the original
vector.delete doc=$d shape=$copy
```

**Canvas.** Resize it or change the background (shapes stay put):

```text
vector.canvas doc=$d width=1024 height=512 background=#eeeeee
```

## 8. opening existing SVG files

```text
vector.open path=drawing.svg -> d
vector.info doc=$d
```

The importer reads the parts of SVG that describe shapes:

- `rect` (with `rx`/`ry` rounded corners), `circle`, `ellipse`, `line`,
  `polyline`, `polygon`, `path`
- groups (`g`), whose styles, opacity and transforms pass down to the
  shapes inside
- `fill`, `stroke`, `stroke-width`, `opacity`, `fill-opacity`,
  `stroke-opacity`, `display`, `visibility`, as attributes or in
  `style="..."`
- `transform` with `translate`, `rotate`, `scale`, `matrix`, `skewX`/`skewY`
- `width`/`height` in px, mm, cm, in, pt, plus `viewBox` scaling

Text, images, gradients, filters, `<use>` and anything inside `<defs>` are
skipped rather than failing the whole file. A gradient fill turns into its
fallback colour, or grey. Skew can't be represented on a shape, so it's
dropped. Imported shapes get fresh ids (`s1`, `s2`, ...) in file order and
keep their `id` (or Inkscape label) as their name.

Files saved by `vector.save` open again with the same ids, names, colours
and transforms.

## 9. exporting

```text
vector.save doc=$d path=out.svg               # real SVG
vector.save doc=$d path=out.png               # 1 pixel per unit
vector.save doc=$d path=out@3x.png scale=3    # 3x resolution
vector.save doc=$d path=out.jpg quality=85    # JPEG; transparent areas become white
```

The extension picks the format: `svg`, `png`, `jpg`/`jpeg`, `bmp`, `tga`,
`ppm`. Saving as SVG also remembers the path on the document.

`vector.preview doc=$d size=256` gives back a PNG without writing a file. The
CLI only reports its size; over MCP Claude actually sees the image.

## 10. letting Claude draw over MCP

The same commands are MCP tools. Register the suite with Claude Code:

```sh
claude mcp add achclwss -- "$PWD/build/apps/achclwss/achclwss" mcp
```

or only the vector tools (a shorter tool list for Claude to read):

```sh
claude mcp add achclwss-vector -- "$PWD/build/apps/achclwss/achclwss" mcp --tools vector
```

For Claude Desktop, put the same command in its config file
(`claude_desktop_config.json`), or let the hub print it for you with
`hub mcp-config`:

```json
{
  "mcpServers": {
    "achclwss": { "command": "/full/path/to/build/apps/achclwss/achclwss", "args": ["mcp"] }
  }
}
```

**Tool names are command ids with dots turned into underscores:**
`vector.add_rect` is the tool `vector_add_rect`, `vector.new` is
`vector_new`. Parameters are the same names; point lists are JSON arrays of
numbers. The server also adds four tools of its own: `undo`, `redo`,
`history` and `documents` (lists what's open).

Now you can just ask:

> Draw me a 512x512 app icon: a rounded dark blue square with a white
> lightning bolt in the middle. Show me a preview, then save it as
> icon.svg and icon.png at 2x.

Behind the scenes Claude makes calls like these:

```json
{"name": "vector_new", "arguments": {"width": 512, "height": 512, "background": "none"}}
```

which returns `{"doc": "vec1", "width": 512, "height": 512, "shapes": 0}` as
structured content, then

```json
{"name": "vector_add_rect", "arguments": {"doc": "vec1", "x": 32, "y": 32, "width": 448, "height": 448, "radius": 96, "fill": "#1d2b53"}}
{"name": "vector_add_polygon", "arguments": {"doc": "vec1", "points": [280, 80, 160, 280, 250, 280, 220, 432, 352, 220, 262, 220], "fill": "white", "preview": true}}
```

Each add returns `{"shape": "s1"}` (then `s2`, ...). With `preview: true` the
result includes the rendered PNG, so Claude can see the bolt and fix it, for
example with `vector_transform` (`{"doc": "vec1", "shape": "s2", "dx": -8}`)
or by calling `undo` and trying again. Finally:

```json
{"name": "vector_save", "arguments": {"doc": "vec1", "path": "icon.svg"}}
{"name": "vector_save", "arguments": {"doc": "vec1", "path": "icon.png", "scale": 2}}
```

Tips for working with Claude:

- Ask it to call `vector_info` when you want to talk about specific shapes;
  the ids and bounding boxes make "move the sun left a bit" unambiguous.
- Ask for `preview=true` (or `vector_preview`) after big changes so it checks
  its own work.
- Paths in `vector_open`/`vector_save` are relative to where the server was
  started; use full paths if in doubt.
- You can hand it an existing file: "open ~/art/logo.svg, make the red parts
  teal and export a 1024px PNG".

## 11. command reference

`achclwss help <command>` shows every parameter. In short (shape commands
take `doc` and `shape`, adds take `doc`; ✓ = undoable):

| command | what it does | main parameters |
| --- | --- | --- |
| `vector.new` | new drawing, returns `doc` | `width` (800), `height` (600), `background` (white) |
| `vector.open` | open an SVG, returns `doc` | `path` |
| `vector.save` | write svg/png/jpg/bmp/tga/ppm | `doc`, `path`, `scale` (1), `quality` (90) |
| `vector.convert` | SVG file to another format in one step | `input`, `output`, `scale`, `quality` |
| `vector.close` ✓ | close a drawing | `doc` |
| `vector.info` | list the canvas and every shape | `doc` |
| `vector.preview` | PNG of the drawing | `doc`, `size` (512) |
| `vector.canvas` ✓ | resize the canvas, change the background | `width`, `height`, `background` |
| `vector.add_rect` ✓ | rectangle, returns `shape` | `x`, `y`, `width`, `height`, `radius` |
| `vector.add_ellipse` ✓ | ellipse or circle | `cx`, `cy`, `rx`, `ry` (= rx) |
| `vector.add_line` ✓ | straight line | `x1`, `y1`, `x2`, `y2` |
| `vector.add_polygon` ✓ | closed shape through points | `points` (x,y,x,y,...) |
| `vector.add_polyline` ✓ | open line through points | `points` |
| `vector.add_path` ✓ | shape from SVG path data | `d` |
| `vector.style` ✓ | fill, stroke, width, opacity, name, visible | any of those |
| `vector.transform` ✓ | move, rotate, scale | `dx`, `dy`, `rotate`, `scale`, `scale_x`, `scale_y`, `reset` |
| `vector.align` ✓ | align to the canvas | `horizontal`, `vertical`, `margin` |
| `vector.reorder` ✓ | stacking order | `to` = front, back, up, down |
| `vector.duplicate` ✓ | copy just above the original, returns `shape` | `dx`, `dy`, `name` |
| `vector.delete` ✓ | remove a shape | |

Every add command also takes `fill`, `stroke`, `stroke_width`, `opacity`,
`name` and `preview`. Defaults: closed shapes are filled black with no
outline; lines and polylines have a 2px black outline and no fill.

## 12. troubleshooting

**`no open vector document called 'vec1'`.** Each `achclwss run` starts
fresh, so a document made in one run isn't there in the next. Put the steps
in one batch script, or use MCP, where documents stay open until closed.

**`unknown parameter 'colour'`.** The names are `fill` and `stroke`. Run
`achclwss help vector.add_rect` to see them all.

**`'blurple' isn't a colour`.** Use a hex code (`#5865f2`), `rgb(...)`, or
a basic name (red, green, blue, orange, purple, pink, ...).

**`points must be x,y pairs`.** The list needs an even count of numbers,
with at least 3 points for a polygon and 2 for a polyline.

**My shape vanished after `vector.reorder` / `vector.style`.** Something is
on top of it, or it's hidden (`visible=false`), or it's the same colour as
the background. `vector.info` shows the order and whether a shape is
`hidden`.

**The PNG looks small or blurry.** Save with `scale=2` (or more). SVG output
never loses quality.

**My JPEG has a white background I didn't ask for.** JPEG can't store
transparency, so transparent areas are flattened onto white. Use PNG to keep
transparency, or set the colour you want: `vector.canvas doc=$d background=black`.
