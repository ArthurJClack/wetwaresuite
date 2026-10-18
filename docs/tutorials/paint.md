# Paint: brushes, pressure and layers

`paint` is the suite's digital painting tool: brush strokes with pen pressure, erasing, bucket fill, shapes, gradients, and a full layer stack with opacity and blend modes. Paintings save as layered OpenRaster (`.ora`), which Krita, GIMP and MyPaint also open, or flattened to PNG and JPEG.

There is no GUI yet. Today you paint by giving commands the points a brush passes through; the GUI that comes later will turn your mouse or tablet movements into exactly these commands (a stroke is a list of points plus, from a tablet, a pressure for each point). Each task below is shown three ways: the CLI, a batch script, and a prompt for Claude over MCP.

Paint and [photo](photo.md) work on the same documents. You will use a few `photo.*` commands here: `photo.save` to save, `photo.open` to reopen an `.ora`, `photo.info` to list layers and `photo.preview` to look at the result.

New to the CLI? Start with [getting started](getting-started.md). Painting is always multi-step, so it happens in a [batch script](headless-and-batch.md) or a Claude session, not in a single `achclwss run`.

## The basics

- `paint.new width=800 height=500` makes a canvas with two layers: **0 "Background"** (filled with `background`, white by default) and **1 "Layer 1"**, empty and active. Drawing goes to the active layer, so the background stays untouched.
- Coordinates are pixels from the top-left corner.
- Points are a flat list `x1,y1,x2,y2,...`: `points=40,80,200,40,360,100` is three points.
- Layers are numbered from 0 at the bottom. Any drawing command takes `layer=N` to draw somewhere other than the active layer.

## Task 1: a stroke with tablet pressure

Pressure is a number from 0 (barely touching) to 1 (full pressure) for each point. Depending on the brush, pressure changes the stroke's width, its opacity, or both.

```text
# pressure.txt: the same path, with and without pen pressure
paint.new width=600 height=300 -> p
# no pressure: an even line
paint.stroke doc=$p brush=ink color=black size=16 points=40,80,200,40,360,100,560,60
# a tablet-style stroke: light, pressing harder, lifting off
paint.stroke doc=$p brush=ink color=black size=16 points=40,200,200,160,360,220,560,180 pressures=0.1,0.7,1.0,0.2
photo.save doc=$p path=pressure.png
```

```sh
achclwss batch pressure.txt
```

The first stroke is the same width all the way; the second starts as a hairline, swells in the middle and tapers off at the end, like a real pen. The number of pressures must match the number of points; otherwise you get `pressures needs one value (0..1) per point: got 2 for 3 points`.

### Brush presets

Pick a starting point with `brush=`, then override any part of it with `size`, `hardness`, `flow` or `spacing`:

| Preset | Size (px) | Hardness | Flow | Spacing | Pressure changes |
| --- | --- | --- | --- | --- | --- |
| `round` (default) | 20 | 80 | 100 | 15 | width |
| `pen` | 6 | 95 | 100 | 10 | width |
| `pencil` | 3 | 100 | 80 | 8 | opacity |
| `ink` | 8 | 98 | 100 | 5 | width and opacity |
| `marker` | 18 | 70 | 60 | 10 | nothing (even strokes) |
| `soft` | 40 | 20 | 40 | 10 | width and opacity |
| `airbrush` | 60 | 0 | 8 | 8 | opacity |

- **size**: brush diameter in px.
- **hardness** 0..100: 100 is a crisp edge, 0 a soft glow.
- **flow** 0..100: how much paint each dab lays down; low flow builds up gradually, like an airbrush.
- **spacing**: distance between dabs as a % of the size. Small values give smoother strokes.
- **opacity** 0..100: the most the stroke can cover (default 100).

### Steadying a shaky line

Mouse strokes wobble. `smooth` (0..90) averages the points out before painting:

```text
paint.stroke doc=$p brush=marker color=white opacity=60 smooth=60 points=500,100,520,96,540,104,560,97,580,103,600,98
```

### Ask Claude

> Make a 600x300 white canvas and draw a calligraphy-style swoosh across it with an ink brush: thin at the start, thick in the middle, thin at the end. Show me a preview and save it as /home/me/swoosh.png.

Claude calls `paint_new`, then `paint_stroke` with `brush=ink`, a `points` array and a matching `pressures` array, and `preview=true` to check the shape. It may add more strokes, `undo` ones it does not like, and finally `photo_save`.

## Task 2: a sunset in layers

Keeping each part of a picture on its own layer lets you change it later without repainting everything.

```text
# sunset.txt: a sunset over hills, painted entirely from a script
paint.new width=800 height=500 -> art

# sky: a gradient on the background (layer 0)
paint.gradient doc=$art layer=0 x1=0 y1=0 x2=0 y2=500 from=#1d2b64 to=#f8a95b

# sun on its own layer, blended with screen so it glows
paint.layer_add doc=$art name=Sun
paint.shape doc=$art kind=ellipse x=330 y=230 width=140 height=140 color=gold
paint.layer_set doc=$art blend=screen opacity=90

# hills: a filled polygon on another layer, in front of the sun
paint.layer_add doc=$art name=Hills
paint.polygon doc=$art points=0,500,0,380,150,330,300,370,480,310,650,360,800,330,800,500 color=#2d3a2e

# birds: an ink stroke with pressure, and a soft cloud
paint.layer_add doc=$art name=Ink
paint.stroke doc=$art brush=ink color=black size=10 points=60,120,160,90,260,110,360,80 pressures=0.2,0.9,1.0,0.3
paint.stroke doc=$art brush=marker color="rgb(255,255,255)" opacity=60 smooth=60 points=500,100,520,96,540,104,560,97,580,103,600,98

photo.info doc=$art
photo.save doc=$art path=sunset.ora
photo.save doc=$art path=sunset.png
```

`photo.info` shows the stack, top first:

```
800x500, 5 layers, active layer 4 "Ink"
  4: Ink (normal, opacity 100%, active)
  3: Hills (normal, opacity 100%)
  2: Sun (screen, opacity 90%)
  1: Layer 1 (normal, opacity 100%)
  0: Background (normal, opacity 100%)
```

`paint.layer_add` always puts the new layer just above the active one and makes it active, which is why each new part lands on top. `sunset.ora` keeps all five layers; `sunset.png` is the flattened picture.

### Ask Claude

> Paint an 800x500 sunset: a dark blue to orange sky gradient, a glowing sun half behind dark green hills, and a few birds. Put sky, sun, hills and birds on separate layers. Save /home/me/sunset.ora and a PNG copy.

Expect `paint_new`, `paint_gradient`, several `paint_layer_add`, `paint_shape`, `paint_polygon`, `paint_stroke`, `paint_layer_set` (for `blend=screen`), `photo_preview` along the way, and `photo_save` twice.

## Task 3: layers and blend modes

This example builds a ball with a shadow and a highlight, and shows every layer operation.

```text
# ball.txt: layers and blend modes
paint.new width=400 height=400 background=#e8e4d8 -> art
# layer 1 ("Layer 1") is active: rename it and draw the ball
paint.layer_set doc=$art name=Ball
paint.shape doc=$art kind=ellipse x=100 y=80 width=200 height=200 color=#d33682

# a shadow on its own layer, darkened with multiply
paint.layer_add doc=$art name=Shadow
paint.shape doc=$art kind=ellipse x=110 y=300 width=180 height=40 color=#555555
paint.layer_set doc=$art blend=multiply opacity=60
# shadows belong under the ball: move it from index 2 down to index 1
paint.layer_move doc=$art to=1

# a highlight on top, lightened with screen
paint.layer_select doc=$art layer=2
paint.layer_add doc=$art name=Shine
paint.stroke doc=$art brush=airbrush color=white size=80 points=160,140,170,135
paint.layer_set doc=$art blend=screen opacity=80

photo.info doc=$art
photo.save doc=$art path=ball.ora
photo.save doc=$art path=ball.png
```

The result:

```
400x400, 4 layers, active layer 3 "Shine"
  3: Shine (screen, opacity 80%, active)
  2: Ball (normal, opacity 100%)
  1: Shadow (multiply, opacity 60%)
  0: Background (normal, opacity 100%)
```

Watch the indexes: moving, adding and deleting layers renumbers the ones above. When in doubt, run `photo.info` (or ask Claude to) before using `layer=N`. Commands with no `layer=` use the active layer, and `paint.layer_move` makes the moved layer active.

### Blend modes

| Mode | Effect | Good for |
| --- | --- | --- |
| `normal` | covers what is below | most painting |
| `multiply` | darkens; white has no effect | shadows, ink over colour |
| `screen` | lightens; black has no effect | glows, highlights |
| `overlay` | boosts contrast: darkens darks, lightens lights | texture, colour grading |
| `add` | adds brightness, can burn out to white | fire, lasers, sparkles |
| `darken` | keeps the darker of the two | |
| `lighten` | keeps the lighter of the two | |
| `difference` | subtracts; same colours become black | comparing two versions |

### Reopen, hide, duplicate, merge, flatten

```text
photo.open path=ball.ora -> art
# hide the highlight and save a version without it
paint.layer_set doc=$art layer=3 visible=false
photo.save doc=$art path=ball-no-shine.png
paint.layer_set doc=$art layer=3 visible=true
# duplicate the ball, tint the copy with a blend mode, merge it back down
paint.layer_duplicate doc=$art layer=2
paint.layer_set doc=$art blend=difference opacity=30
paint.layer_merge_down doc=$art
# one layer for export
paint.flatten doc=$art
photo.save doc=$art path=ball-flat.png
```

- `paint.layer_duplicate` puts the copy (named "Ball copy") just above and makes it active.
- `paint.layer_merge_down` paints a layer into the one below using its blend mode and opacity, then removes it. Hidden layers are dropped without being painted in.
- `paint.flatten` merges every visible layer into one "Background" layer.
- `paint.layer_delete` removes a layer; an image always keeps at least one.

### Ask Claude

> Open /home/me/ball.ora, hide the Shine layer, and set the Shadow layer to 40% opacity. Then show me what it looks like.

`photo_open`, `photo_info` to find the layer indexes, `paint_layer_set` twice, `photo_preview`.

## Task 4: fills, shapes and gradients

```text
# fill.txt: outlines, then bucket fills that stop at the lines
paint.new width=300 height=200 -> c
paint.shape doc=$c kind=ellipse x=20 y=20 width=160 height=160 filled=false line_width=4
paint.shape doc=$c kind=rect x=200 y=60 width=80 height=80 filled=false line_width=4
paint.shape doc=$c kind=line x=20 y=100 width=260 height=0 line_width=2 color=gray
paint.fill doc=$c x=100 y=60 color=gold
paint.fill doc=$c x=240 y=80 color=#2aa198 tolerance=0
paint.polygon doc=$c points=230,20,250,50,210,50 color=red
photo.save doc=$c path=fill.png
```

- `paint.shape` draws `rect`, `ellipse` or `line` in the box `x,y,width,height`. For a line, `width` and `height` are the offset to the end point (negative values are fine). `filled=false` draws only the outline, `line_width` thick.
- `paint.fill` floods outwards from `x,y` over pixels similar to the one clicked (`tolerance` 0..255, default 32) and stops at lines. It returns how many pixels it filled. `contiguous=false` recolours every matching pixel on the layer, connected or not.
- `paint.polygon` fills the shape through three or more points.
- `paint.gradient` blends `from` to `to` between two points; `kind=radial` makes a circle from the centre `x1,y1` out to `x2,y2`. Colours with alpha (such as `#ffffff00`) fade to transparent.
- `paint.clear` wipes a layer to transparent, or fills it with `color=`.
- `paint.erase` takes the same brush parameters as `paint.stroke` and makes the layer transparent along the path. Use it on a paint layer: erasing the background shows transparency (white in a JPEG).

## Saving your work

- `photo.save path=art.ora` keeps every layer, name, opacity, blend mode and visibility. Reopen it with `photo.open`.
- `photo.save path=art.png` (or `.jpg`, `.bmp`, `.tga`, `.ppm`) saves the flattened picture without changing your layers.
- `photo.save` creates missing folders for you.
- Everything is undoable until you close the document: `undo` and `redo` in scripts, the `undo` and `redo` tools for Claude.

## Command reference

Every command takes `doc` (the id of an open image, such as `img1`), and every drawing command takes `layer` (index, 0 = bottom, default the active layer) and `preview` (boolean, default false, attaches a 512 px PNG for Claude). They are listed once here instead of in every table.

### paint.new
Create a canvas with a Background layer and an empty "Layer 1" (active). Not undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `width` | integer | yes | | 1..30000 px |
| `height` | integer | yes | | 1..30000 px |
| `background` | colour | no | `#ffffff` | `transparent` for none |

### paint.stroke
Paint a brush stroke through a list of points.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `points` | array | yes | | `x1,y1,x2,y2,...` in px |
| `pressures` | array | no | all 1 | one value 0..1 per point |
| `color` | colour | no | `#000000` | paint colour |
| `brush` | `airbrush`, `ink`, `marker`, `pen`, `pencil`, `round`, `soft` | no | `round` | preset (see the presets table) |
| `size` | number | no | preset | diameter in px |
| `hardness` | number | no | preset | 0..100 |
| `opacity` | number | no | 100 | 0..100 |
| `flow` | number | no | preset | 0..100 |
| `spacing` | number | no | preset | % of size between dabs |
| `smooth` | number | no | 0 | stabilizer 0..90 |
| `layer`, `preview` | | no | | see above |

### paint.erase
Erase along a list of points. Same parameters as `paint.stroke` except `color`.

### paint.fill
Bucket fill.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `x` | integer | yes | | start x |
| `y` | integer | yes | | start y |
| `color` | colour | yes | | fill colour |
| `tolerance` | integer | no | 32 | 0..255, how different a colour may be and still fill |
| `contiguous` | boolean | no | true | false fills every matching pixel on the layer |
| `layer`, `preview` | | no | | see above |

Returns `pixels` (how many were filled).

### paint.shape
Rectangle, ellipse or line.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `kind` | `rect`, `ellipse`, `line` | yes | | shape |
| `x` | number | yes | | left (line: start x) |
| `y` | number | yes | | top (line: start y) |
| `width` | number | yes | | box width (line: dx) |
| `height` | number | yes | | box height (line: dy) |
| `color` | colour | no | `#000000` | |
| `filled` | boolean | no | true | false draws the outline only (ignored for lines) |
| `line_width` | number | no | 2 | outline or line width in px |
| `layer`, `preview` | | no | | see above |

### paint.polygon
Fill a polygon.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `points` | array | yes | | corners as x,y pairs, at least 3 |
| `color` | colour | no | `#000000` | fill colour |
| `layer`, `preview` | | no | | see above |

### paint.gradient
Linear or radial gradient across the layer.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `x1`, `y1` | number | yes | | start (radial: centre) |
| `x2`, `y2` | number | yes | | end (radial: edge) |
| `from` | colour | no | `#000000` | start colour |
| `to` | colour | no | `#ffffff` | end colour |
| `kind` | `linear`, `radial` | no | `linear` | shape |
| `layer`, `preview` | | no | | see above |

### paint.clear
Wipe a layer, or fill it with one colour.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `color` | colour | no | `transparent` | colour to fill with |
| `layer`, `preview` | | no | | see above |

### paint.layer_add
Add a layer just above the active one and make it active.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `name` | string | no | `Layer N` | layer name |
| `fill` | colour | no | transparent | start filled with this colour |

### paint.layer_delete
Delete a layer (the last one cannot be deleted).

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `layer` | integer | no | active | layer to delete |

### paint.layer_duplicate
Copy a layer just above itself; the copy becomes active.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `layer` | integer | no | active | layer to copy |

### paint.layer_merge_down
Merge a layer into the one below (not the bottom layer).

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `layer` | integer | no | active | layer to merge |
| `preview` | boolean | no | false | |

### paint.layer_move
Move a layer to a new index; it becomes active.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `layer` | integer | no | active | layer to move |
| `to` | integer | yes | | new index, 0 = bottom |
| `preview` | boolean | no | false | |

### paint.layer_select
Make a layer active.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `layer` | integer | yes | layer index |

### paint.layer_set
Change a layer's properties. Only the ones you pass change.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `layer` | integer | no | active | layer to change |
| `name` | string | no | | new name |
| `opacity` | number | no | | 0..100 |
| `blend` | `normal`, `multiply`, `screen`, `overlay`, `add`, `darken`, `lighten`, `difference` | no | | blend mode |
| `visible` | boolean | no | | show or hide |
| `preview` | boolean | no | false | |

### paint.flatten
Merge all visible layers into one.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `preview` | boolean | no | false | |

For opening, saving, previewing and inspecting, see `photo.open`, `photo.save`, `photo.preview` and `photo.info` in the [photo reference](photo.md#command-reference).
