# Paint: brushes, pressure and layers

`paint` is the suite's digital painting tool: brush strokes with pen pressure, erasing, bucket fill, shapes, gradients and patterns; retouch brushes (clone stamp, dodge, burn, sponge, smudge, blur and sharpen, colour replacement); brush tips, textures, scatter and symmetry; and a full layer stack like Photoshop's or GIMP's, with all 27 blend modes, groups, clipping masks, locked transparency, fill opacity, layer styles (shadows, glows, stroke, overlays, bevel), editable text layers and editable shape layers. Paintings save as layered OpenRaster (`.ora`, which keeps everything and opens in Krita, GIMP and MyPaint), as layered Photoshop `.psd`, or flattened to PNG and JPEG.

In scripts and for Claude you paint by giving commands the points a brush passes through. The window (`wetwaresuite-image`, see [the window tutorial](gui.md)) turns your mouse or tablet movements into exactly these commands: a stroke is a list of points plus, from a tablet, a pressure for each point. Each task below is shown three ways: a batch script (or the CLI), a prompt for Claude over MCP, and the window.

Paint and [photo](photo.md) work on the same documents. You will use a few `photo.*` commands here: `photo.save` to save, `photo.open` to reopen an `.ora`, `photo.info` to list layers and `photo.preview` to look at the result.

New to the CLI? Start with [getting started](getting-started.md). Painting is always multi-step, so it happens in a [batch script](headless-and-batch.md) or a Claude session, not in a single `wetwaresuite run`.

## The basics

- `paint.new width=800 height=500` makes a canvas with two layers: **0 "Background"** (filled with `background`, white by default) and **1 "Layer 1"**, empty and active. Drawing goes to the active layer, so the background stays untouched.
- Coordinates are pixels from the top-left corner.
- Points are a flat list `x1,y1,x2,y2,...`: `points=40,80,200,40,360,100` is three points.
- Layers are numbered from 0 at the bottom. Any drawing command takes `layer=N` to draw somewhere other than the active layer; the layer you draw on becomes the active one, so new layers then appear just above it.

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
wetwaresuite batch pressure.txt
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
| `chalk` | 24 | 90 | 70 | 12 | opacity (a grainy `noise` paper texture) |
| `spray` | 12 | 60 | 50 | 25 | nothing (dabs scattered 150% of the size) |
| `square` | 16 | 100 | 100 | 10 | nothing (a square tip) |

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

Those are the everyday ones. All 27 modes from Photoshop work, in its order: `normal`, `dissolve`; `darken`, `multiply`, `color_burn`, `linear_burn`, `darker_color`; `lighten`, `screen`, `color_dodge`, `add` (linear dodge), `lighter_color`; `overlay`, `soft_light`, `hard_light`, `vivid_light`, `linear_light`, `pin_light`, `hard_mix`; `difference`, `exclusion`, `subtract`, `divide`; `hue`, `saturation`, `color`, `luminosity`. They work for layers, groups, layer styles, fills and gradients alike, and are saved in both `.ora` and `.psd`.

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

- `paint.layer_duplicate` puts the copy (named "Ball copy") just above and makes it active. Duplicating a group copies everything in it.
- `paint.layer_merge_down` paints a layer into the one below using its blend mode, opacity, mask, clipping and layer style, then removes it. An adjustment layer merged down is applied to the layer below; a group is merged with everything in it first.
- `paint.merge_visible` merges every visible layer into one and leaves hidden layers alone; `paint.flatten` merges everything visible into one "Background" layer.
- `paint.layer_delete` removes a layer (a group with everything in it, or just the group with `keep_contents=true`); an image always keeps at least one.

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

## Task 5: groups, clipping, locks and fill opacity

**Groups** keep related layers together: they fold up in the layers panel, and hide, fade, move and blend as one. **Clipping** (`clip=true`) shows a layer only where the layer under it has paint, like a pattern inside a shape. **Lock transparency** (`lock_alpha=true`) lets you paint only where a layer already has paint, so you can recolour a shape without going over its edge. **Fill** opacity fades a layer's pixels but not its layer style (a stroke or shadow stays at full strength).

```text
# clip.txt: stripes clipped to an ellipse, then a soft orange glow painted inside it
paint.new width=400 height=300 -> c
paint.shape doc=$c kind=ellipse x=50 y=50 width=300 height=200 color=#3355aa
paint.layer_add doc=$c name=Stripes
paint.pattern_fill doc=$c pattern=stripes color=#ffffff background=#00000000 size=20
# show the stripes only where the ellipse (the layer below) has paint
paint.layer_set doc=$c clip=true
# back to the ellipse: lock its transparency and paint across it; nothing lands outside
paint.layer_select doc=$c layer=1
paint.layer_set doc=$c lock_alpha=true
paint.stroke doc=$c brush=soft size=120 color=#ff8800 points=60,150,340,150
photo.save doc=$c path=clip.png
```

Groups take a range of layer indexes that sit side by side:

```text
# put layers 2 and 3 into a group called Title (it becomes layer 4 and is active)
paint.group doc=$art from=2 to=3 name=Title
# fade and blend the whole group; pass_through=false blends it as one picture
paint.layer_set doc=$art layer=4 opacity=80 blend=multiply pass_through=false
# fold it in the layers panel, or take the layers back out
paint.layer_set doc=$art layer=4 collapsed=true
paint.ungroup doc=$art layer=4
```

- `photo.info` shows the stack with group members indented, plus each layer's kind, blend mode, opacity, fill, locks, settings and style.
- `paint.layer_move` moves a group with everything in it. Moving a layer to an index just under a group's header (or between its members) puts it into the group; moving it past the header takes it out.
- A new layer added while a group is active goes into the top of that group.
- Groups are **pass-through** by default (the members blend straight into the layers below, like Photoshop's default). `pass_through=false` composes the group first and then blends the result with the group's blend mode.

**Ask Claude.**

> In /home/me/poster.ora, put the title text and its star into a group called Title, make the group 80% opacity, and clip the stripes layer to the circle under it.

Claude calls `photo_open`, `photo_info` (to find the indexes), `paint_group`, `paint_layer_set` for the group's opacity and the stripes' `clip=true`, `photo_preview` and `photo_save`.

**In the window.** The **Layers** section on the right lists the stack top first. Tick the box to show or hide a layer, click a name to make it active, and click **-**/**+** to fold or unfold a group. Members are indented; **->** marks a clipped layer, **[mask]** a mask, **fx** a style and **[lock]** a locked layer. Under the list:

- **New**, **Group** (puts the active layer in a new group), **Ungroup**, **Copy**, **Delete**, **Up**, **Down**, **Merge down**, **Merge visible**, **Flatten**, **Rasterize** and **Add mask**.
- For the active layer: the **Blend** mode, **Opacity** and **Fill** sliders (the change runs when you let go of the slider, as one undo step), **Lock transparency**, **Clip to below** and, for groups, **Pass through**.

## Task 6: text, shape layers and layer styles

**Text layers** and **shape layers** stay editable: change the words, font, size, colour or the shape's corners later and they redraw sharply. **Layer styles** are effects drawn live around a layer's pixels, so they follow the layer as you edit or move it: `drop_shadow`, `inner_shadow`, `outer_glow`, `inner_glow`, `bevel` (bevel and emboss), `color_overlay`, `gradient_overlay` and `stroke`.

```text
# poster.txt: a gradient, an editable star and title, styles and a group
paint.new width=800 height=500 -> art
paint.gradient doc=$art layer=0 x1=0 y1=0 x2=800 y2=500 preset=sunset
# drawing on layer 0 made it active: go back to Layer 1 so new layers go above it
paint.layer_select doc=$art layer=1
paint.shape_layer doc=$art shape=star x=300 y=40 width=200 height=200 fill=#ffd23f stroke=#000000 stroke_width=4
paint.layer_style doc=$art effect=drop_shadow distance=10 size=8
paint.text doc=$art text="Summer Fair" x=400 y=300 size=72 align=center color=white
paint.layer_style doc=$art effect=stroke size=3 color=#222222
paint.layer_style doc=$art effect=outer_glow color=#ffcc00 size=12
paint.group doc=$art from=2 to=3 name=Title
# change your mind later: the star turns red and tilts, the title gets a year
paint.shape_edit doc=$art layer=2 fill=#ff6b6b angle=15
paint.text_edit doc=$art layer=3 text="Summer Fair 2026" size=60
photo.save doc=$art path=poster.ora
photo.save doc=$art path=poster.psd
photo.save doc=$art path=poster.png
```

- **Text**: `font` is `sans` (built in, identical on every computer), a family such as `serif`, `mono`, `Arial` or `DejaVu Sans` looked up on this computer, or a `.ttf` file. `bold`, `italic`, `align` (`left`, `center`, `right`: `x` is the left edge, middle or right edge), `line_height` (% of the size), `tracking` (extra letter spacing in 1/1000 em) and `width` (wrap lines at this many pixels) are all optional. `\n` in `text` starts a new line.
- **Shapes**: `rect`, `rounded_rect` (`radius`), `ellipse`, `polygon` (`sides`), `star` (`sides` points, `inner` radius as % of the outer) and `line`, with a `fill` and an outline (`stroke`, `stroke_width`; `transparent` for none), turned by `angle` degrees.
- **Styles**: each `paint.layer_style` call adds or changes one effect and only the settings you give change; `on=false` turns that effect off and `effect=all on=false` clears the style. The result's `style` text can be passed to another layer as `style=` to copy the whole style. Settings: `color` (and `to` for a gradient overlay), `opacity`, `angle`, `distance`, `size`, `spread`, `blend`, `radial`, `position` (stroke: `outside`, `inside`, `center`), and for bevel `depth`, `altitude`, `highlight`, `shadow` and `down` (pressed in).
- Painting on a text or shape layer, transforming or resizing the image, or `paint.rasterize` turns it into plain pixels first. `paint.rasterize` also draws a style (and fill opacity) into the pixels, and turns a group into one layer.
- `.ora` keeps text, shapes and styles editable. `.psd` keeps layers, groups, masks, blend modes, opacity and clipping, but draws styles into the pixels and saves text and shape layers as pixels; `photo.save` lists each of these as a `note:` in its result.

**Ask Claude.**

> Make an 800x500 poster for a summer fair: a sunset gradient, a yellow star with a drop shadow, and the title "Summer Fair 2026" in white with a dark outline. Keep the text editable and save /home/me/poster.ora and a PNG.

Claude calls `paint_new`, `paint_gradient`, `paint_shape_layer`, `paint_text`, `paint_layer_style` (drop_shadow, stroke), `photo_preview` and `photo_save` twice.

**In the window.** With a text layer active, the **Layers** tab on the right shows **Text** (type and press Enter), **Font size** and **Text colour**. With any other layer active, type in the box under the buttons and press Enter to add a text layer in the brush colour. Open **Layer style** to tick effects on and off and set each one's colour, size, distance and opacity. Text, shape and style commands are also in **Actions** (switch to **Advanced**) and in the search box (Ctrl+K: type `text`, `shape` or `style`).

## Task 7: brush tips, textures, scatter and symmetry

Every brush command (`paint.stroke`, `paint.erase` and the retouch brushes in Task 8) takes the same brush settings:

| Setting | What it does |
| --- | --- |
| `hardness` 0..100 | crisp edge (100) or soft glow (0) |
| `flow` 0..100 | paint per dab: low flow builds up where dabs overlap |
| `spacing` | gap between dabs, % of the size: big values make a dotted line of tips |
| `scatter` 0..400 | throw each dab off the path by up to this % of the size |
| `tip` | the dab's shape: `square`, `star`, `leaf`, `splatter`, `layer:N` (the pixels of layer N, by brightness and transparency) or an image file. Tips keep their own proportions |
| `texture` | paper grain: a pattern (`noise`, `checker`, `stripes`, `diagonal`, `dots`, `grid`, `bricks`, `hexagons`), `layer:N` or an image file; `texture_scale` (%) and `texture_depth` (0..100) |
| `symmetry` | also paint mirrored copies: `vertical` (left/right), `horizontal` (top/bottom), `both`, or `radial` with `symmetry_count` copies around the centre (`symmetry_x`, `symmetry_y`, default the middle) |

```text
# brushes.txt
paint.new width=600 height=400 -> b
# a spray of scattered dabs
paint.stroke doc=$b brush=spray color=#2a7 size=40 scatter=150 points=40,60,560,60
# a row of stars: a star tip spaced wider than its size
paint.stroke doc=$b tip=star size=30 spacing=120 color=#e33 points=40,140,560,140
# chalk on rough paper
paint.stroke doc=$b brush=chalk texture=noise texture_depth=80 size=40 points=40,220,560,220
# a mandala: one short stroke, eight copies around the middle
paint.stroke doc=$b size=8 color=#225 symmetry=radial symmetry_count=8 points=300,300,340,330,380,320
# low flow, tight spacing: paint builds up smoothly
paint.stroke doc=$b size=20 hardness=100 flow=30 spacing=5 points=40,370,250,370
photo.save doc=$b path=brushes.png
```

Scatter and textured strokes are random but repeatable: the same stroke gives the same result every time.

**Ask Claude.** *"On a 600x400 canvas, paint a snowflake: a short blue stroke repeated 6 times around the centre."* Claude uses `paint_stroke` with `symmetry=radial` and `symmetry_count=6`.

**In the window.** The **Brush** tool's settings above the picture set the preset, size, opacity and colour; the other settings above are available in the **Brush stroke** action (Ctrl+K, `stroke`) for exact strokes.

## Task 8: retouch brushes (clone, dodge, burn, sponge, smudge, blur, colour replace)

These brushes change what's already there instead of laying down colour. Each takes points like `paint.stroke` and all the brush settings of Task 7, works on the active layer (or `layer=`), respects the selection and lock transparency, and each stroke is one undo step.

| Brush | Command | What it does |
| --- | --- | --- |
| Clone stamp | `paint.clone source_x= source_y=` | paints with a copy of another spot. The source lines up with the first point and moves along with the brush. `sample=image` copies what you see (all layers) instead of just this layer |
| Dodge / burn | `paint.dodge tool=dodge` or `tool=burn` | lightens or darkens; `range` (`shadows`, `midtones`, `highlights`) picks which tones change most, `exposure` 0..100 the strength |
| Sponge | `paint.dodge tool=sponge` | `sponge_mode=desaturate` washes colour out, `saturate` makes it stronger |
| Smudge | `paint.smudge` | drags colours along like a finger in wet paint (`strength` 0..100); `finger_paint=true` starts with `color` on the finger |
| Blur / sharpen | `paint.blur_brush` | `mode=blur` softens, `mode=sharpen` crisps up, `strength` 0..100 |
| Colour replacement | `paint.color_replace color=` | pixels like the one under the first point (within `tolerance`) take the hue and saturation of `color` but keep their own light and shade: eyes, a shirt, a car |

```text
# retouch-brushes.txt
photo.open path=poster.png -> r
# copy from (600,400) to (100,100), and onwards along the stroke
paint.clone doc=$r points=100,100,140,120 source_x=600 source_y=400 size=40
# lift the shadows along the bottom, darken the bright corner, wash out a patch
paint.dodge doc=$r tool=dodge range=shadows exposure=40 points=50,450,300,450 size=60
paint.dodge doc=$r tool=burn range=highlights points=500,50,750,50 size=60
paint.dodge doc=$r tool=sponge sponge_mode=desaturate points=50,250,200,250 size=60
paint.smudge doc=$r strength=70 points=380,150,460,180 size=30
paint.blur_brush doc=$r mode=sharpen strength=60 points=300,320,500,320 size=50
# the star turns blue but keeps its shading
paint.color_replace doc=$r color=#3399ff tolerance=60 points=400,130,410,140 size=80
photo.save doc=$r path=retouched.png
```

**Ask Claude.**

> In /home/me/portrait.jpg, darken the bright patch on the forehead a little, make the eyes green, and soften the background on the left.

Claude calls `photo_open`, `photo_preview` to find the spots, `paint_dodge` with `tool=burn range=highlights`, `paint_color_replace` with a green `color` over each eye, `paint_blur_brush` along the background, `photo_preview` to check and `photo_save`.

**In the window.** Above the picture, next to Brush, Eraser, Fill, Heal and Remove, are **Clone**, **Dodge**, **Burn**, **Sponge**, **Smudge** and **Blur**. Hold the mouse over any of them to read what it does.

1. Click the tool and set **Size** (and **Exposure** or **Strength**; Dodge and Burn also have a **Range**, Blur has a **Sharpen** box).
2. Paint over the picture. When you let go, the stroke runs as one step: **Ctrl+Z** takes it back.
3. **Clone** needs a spot to copy from first: hold **Alt** (**Option** on a Mac) and click it, or press **Pick source** and then click it (handy with a pen or touch screen). Then paint where the copy should go. A white circle shows where it's copying from and moves along with your brush.

## Task 9: gradients and pattern fills

`paint.gradient` paints from one colour to another, through a list of colours (`colors=#000000,#ff8800@30,#ffffff`, where `@30` puts that colour 30% of the way along), or a preset: `black_white`, `rainbow`, `sunset`, `ocean`, `fire`, `chrome`, `copper`, `pastel`, `violet_orange`, `transparent_stripes`. `kind` picks the shape: `linear`, `radial` (centre `x1,y1`, edge `x2,y2`), `angle` (sweeps around `x1,y1`), `reflected` (mirrored both ways) or `diamond`. `reverse=true` runs it the other way, and `opacity` and `blend` say how it goes onto the layer.

`paint.pattern_fill` fills with a repeating pattern: a built-in one in two colours (`checker`, `stripes`, `diagonal`, `dots`, `grid`, `bricks`, `noise`, `hexagons`, cell `size` in px), another layer's pixels (`pattern_layer=N`) or an image file (`pattern_path=`), at `scale` % and shifted by `offset_x`/`offset_y`. It fills the selection when there is one, the area around `x,y` like a paint bucket when you give a point (`tolerance` as for `paint.fill`), or else the whole layer.

```text
# fills.txt
paint.new width=400 height=300 -> f
paint.gradient doc=$f x1=200 y1=150 x2=380 y2=150 preset=rainbow kind=angle
paint.gradient doc=$f x1=0 y1=0 x2=400 y2=0 colors=#000000,#ff8800@30,#ffffff opacity=50 blend=overlay
paint.layer_add doc=$f name=Tiles
paint.pattern_fill doc=$f pattern=bricks size=24 color=#aa4422 background=#ddccbb opacity=60
photo.save doc=$f path=fills.png
```

**Ask Claude.** *"Fill the background of /home/me/card.ora with a pale blue dotted pattern and put a soft ocean gradient behind the title."* Claude calls `paint_pattern_fill` (`pattern=dots`), `paint_gradient` (`preset=ocean`) and `photo_save`.

**In the window.** Ctrl+K and type `gradient` or `pattern`, fill in the form, and press **Run**.

## Saving your work

- `photo.save path=art.ora` keeps everything: every layer, name, opacity, fill, blend mode, visibility, group, mask, clipping, lock, layer style, and editable text, shape and adjustment layers. Reopen it with `photo.open`.
- `photo.save path=art.psd` writes a layered Photoshop file (also opened by GIMP, Krita and Photopea) with layers, groups, masks, blend modes, opacity and clipping. Styles are drawn into the pixels, text and shape layers become pixels, and adjustment layers become empty hidden layers; the result lists each of these as a `note:`. `photo.open` reads layered `.psd` files back.
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
| `brush` | `airbrush`, `chalk`, `ink`, `marker`, `pen`, `pencil`, `round`, `soft`, `spray`, `square` | no | `round` | preset (see the presets table) |
| `size` | number | no | preset | diameter in px |
| `hardness` | number | no | preset | 0..100 |
| `opacity` | number | no | 100 | 0..100 |
| `flow` | number | no | preset | 0..100 |
| `spacing` | number | no | preset | % of size between dabs |
| `smooth` | number | no | 0 | stabilizer 0..90 |
| `scatter` | number | no | preset | 0..400, % of size |
| `tip` | string | no | round | `square`, `star`, `leaf`, `splatter`, `layer:N` or an image file |
| `texture` | string | no | none | a pattern name, `layer:N` or an image file |
| `texture_scale`, `texture_depth` | number | no | 100, 100 | texture size % and strength 0..100 |
| `symmetry` | `none`, `vertical`, `horizontal`, `both`, `radial` | no | `none` | mirrored copies |
| `symmetry_count` | integer | no | 6 | radial copies |
| `symmetry_x`, `symmetry_y` | number | no | middle | symmetry centre |
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
Gradient across the layer.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `x1`, `y1` | number | yes | | start (radial, angle, diamond: centre) |
| `x2`, `y2` | number | yes | | end (radial: edge) |
| `from` | colour | no | `#000000` | start colour |
| `to` | colour | no | `#ffffff` | end colour |
| `colors` | string | no | | several colours instead, `#c` or `#c@percent`, comma-separated |
| `preset` | `black_white`, `rainbow`, `sunset`, `ocean`, `fire`, `chrome`, `copper`, `pastel`, `violet_orange`, `transparent_stripes` | no | | a built-in gradient instead |
| `kind` | `linear`, `radial`, `angle`, `reflected`, `diamond` | no | `linear` | shape |
| `reverse` | boolean | no | false | run it the other way |
| `opacity` | number | no | 100 | 0..100 |
| `blend` | blend mode | no | `normal` | |
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
Delete a layer (the last one cannot be deleted). Deleting a group deletes everything in it.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `layer` | integer | no | active | layer to delete |
| `keep_contents` | boolean | no | false | groups: keep the layers inside (ungroup) |

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
Move a layer (a group with everything in it) so it ends up at index `to`; it becomes active. Landing just under a group's header or between its members puts it into that group.

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
| `fill` | number | no | | fill opacity 0..100: fades the pixels but not the layer style |
| `blend` | any of the 27 blend modes | no | | blend mode |
| `visible` | boolean | no | | show or hide |
| `clip` | boolean | no | | clip to the layer below (clipping mask) |
| `lock_alpha` | boolean | no | | paint only where there is paint already |
| `pass_through` | boolean | no | | groups: members blend straight through (false: the group blends as one picture) |
| `collapsed` | boolean | no | | groups: folded in the layers panel |
| `preview` | boolean | no | false | |

### paint.flatten
Merge all visible layers into one.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `preview` | boolean | no | false | |

### paint.merge_visible
Merge every visible layer into one; hidden layers stay as they are. Takes `preview`.

### paint.group
Put layers `from`..`to` (side by side, in the same group; default the active layer) into a new group, which becomes active. `name`, and `pass_through` (default true).

### paint.ungroup
Remove the group `layer` (default the active one) but keep its layers.

### paint.rasterize
Turn `layer` into plain pixels: text and shape layers stop being editable, the style and fill opacity are drawn in, and a group becomes one layer.

### paint.layer_style
Add, change or remove a layer effect. See Task 6.

| Parameter | Type | Description |
| --- | --- | --- |
| `effect` | `drop_shadow`, `inner_shadow`, `outer_glow`, `inner_glow`, `bevel`, `color_overlay`, `gradient_overlay`, `stroke`, `all` | which effect (`all` with `on=false` clears the style) |
| `on` | boolean | false turns it off (default true) |
| `style` | string | a whole style as text, as this command returns it, to copy a style |
| `color`, `to` | colour | the colour (gradient overlay: start and end) |
| `opacity`, `spread`, `highlight`, `shadow` | number | 0..100 |
| `angle`, `altitude` | number | degrees |
| `distance`, `size` | number | px |
| `blend` | blend mode | how the effect is blended |
| `radial` | boolean | gradient overlay: radial |
| `position` | `outside`, `inside`, `center` | stroke |
| `depth`, `down` | number, boolean | bevel strength (1 = normal), pressed in |

Returns `style`, the layer's whole style as text.

### paint.text and paint.text_edit
`paint.text` adds an editable text layer above the active layer (`text` is required); `paint.text_edit` changes the text layer `layer` (only what you pass changes). Both take `text`, `x`, `y` (top of the first line), `size` (px), `font`, `bold`, `italic`, `color`, `align` (`left`, `center`, `right`), `line_height` (% of size), `tracking` (1/1000 em) and `width` (wrap width, 0 = only at `\n`); `paint.text` also takes `name`.

### paint.shape_layer and paint.shape_edit
`paint.shape_layer` adds an editable shape layer; `paint.shape_edit` changes one (only what you pass changes). Both take `shape` (`rect`, `rounded_rect`, `ellipse`, `polygon`, `star`, `line`), `x`, `y`, `width`, `height` (line: start and offset), `fill`, `stroke`, `stroke_width`, `radius` (rounded_rect), `sides` (polygon corners, star points), `inner` (star inner radius %) and `angle` (degrees clockwise); `paint.shape_layer` also takes `name`.

### Retouch brushes
`paint.clone`, `paint.dodge`, `paint.smudge`, `paint.blur_brush` and `paint.color_replace` take `points`, `pressures` and every brush setting of `paint.stroke` (default preset `soft`; `round` for colour replacement), plus:

| Command | Parameters |
| --- | --- |
| `paint.clone` | `source_x`, `source_y` (required: the spot that matches the first point), `sample` (`layer` or `image`) |
| `paint.dodge` | `tool` (`dodge`, `burn`, `sponge`), `range` (`shadows`, `midtones`, `highlights`), `exposure` 0..100 (default 50), `sponge_mode` (`saturate`, `desaturate`) |
| `paint.smudge` | `strength` 0..100 (default 50), `finger_paint` (start with `color`) |
| `paint.blur_brush` | `mode` (`blur`, `sharpen`), `strength` 0..100 (default 50) |
| `paint.color_replace` | `color`, `tolerance` 0..255 (default 40) |

### paint.pattern_fill
Fill with a repeating pattern. See Task 9.

| Parameter | Type | Default | Description |
| --- | --- | --- | --- |
| `pattern` | `checker`, `stripes`, `diagonal`, `dots`, `grid`, `bricks`, `noise`, `hexagons` | `checker` | built-in pattern |
| `pattern_layer`, `pattern_path` | integer, string | | a layer's pixels or an image file as the tile instead |
| `size` | integer | 16 | built-in cell size in px |
| `color`, `background` | colour | `#000000`, `#ffffff` | built-in pattern colours |
| `scale` | number | 100 | tile size % |
| `offset_x`, `offset_y` | integer | 0 | shift the tiles |
| `x`, `y`, `tolerance` | integer | | fill only the area around this point |
| `opacity`, `blend` | | 100, `normal` | how it goes onto the layer |

For opening, saving, previewing and inspecting, see `photo.open`, `photo.save`, `photo.preview` and `photo.info` in the [photo reference](photo.md#command-reference).
