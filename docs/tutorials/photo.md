# Photo: editing images

`photo` is the suite's image editor: open, adjust, crop, resize, rotate, apply effects, batch-process folders and save. It reads PNG, JPEG, BMP, TGA, GIF, PSD (flattened), HDR, PNM and layered OpenRaster (`.ora`), and saves PNG, JPEG, BMP, TGA, PPM and `.ora`.

Every task below is a command, and the desktop window (see [the window tutorial](gui.md)) runs exactly these commands from its buttons, sliders and search box. Each task is shown three ways:

- **CLI**: a single `wetwaresuite` command, for jobs that read files and write files.
- **Batch**: a script for multi-step edits (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it will call (see [mcp.md](mcp.md)).

New to the CLI? Start with [getting started](getting-started.md).

## Task 1: resize a folder of photos for the web

You have a folder of 6000-pixel camera JPEGs and want light copies for a website.

**CLI.** `photo.batch` does the whole folder in one command. Originals stay as they are; results go to the output folder, which is created if needed:

```sh
$ wetwaresuite photo.batch input_dir=photos output_dir=web max_size=1600 quality=82
processed 3 of 3 images into web
processed: 3
failed: 0
```

`max_size` fits each image within 1600 px on its longest side (portrait and landscape alike) and never enlarges smaller images. Add `recursive=true` to include subfolders (the folder structure is kept), `auto_contrast=true` to give flat photos some punch, or an effect:

```sh
wetwaresuite photo.batch input_dir=photos output_dir=thumbs max_size=300 format=png effect=grayscale
```

Run it again and results that already exist are skipped; pass `overwrite=true` to redo them. A source image is never replaced, even if `output_dir` is the input folder.

**Batch.** Use a script when each photo needs different treatment, or more than `photo.batch` offers, such as a crop:

```text
photo.open path=photos/holiday-1.jpg -> p
photo.crop doc=$p x=500 y=0 width=2000 height=2000
photo.resize doc=$p width=1080
photo.save doc=$p path=square/holiday-1.jpg quality=85
```

The [batch tutorial](headless-and-batch.md#generating-scripts-from-the-shell) shows how to run this for every file with a shell loop.

**Claude.**

> Make web copies of every photo in /home/me/Pictures/trip: longest side 1600 px, JPEG quality 82, into /home/me/Pictures/trip-web.

Claude calls `photo_batch` with `input_dir`, `output_dir`, `max_size=1600` and `quality=82`.

## Task 2: fix an underexposed photo

The picture came out dark and flat.

**Batch.** Work from the gentlest fix to the strongest, and check the result:

```text
# fix-dark.txt
photo.open path=dark.jpg -> pic
# 1. stretch the tones so the darkest pixel is black and the brightest is white
photo.levels doc=$pic auto=true
# 2. brighten by half a stop
photo.adjust doc=$pic kind=exposure amount=0.5
# 3. a little extra colour, mostly in the dull areas
photo.adjust doc=$pic kind=vibrance amount=20
photo.save doc=$pic path=dark-fixed.jpg quality=90
```

```sh
wetwaresuite batch fix-dark.txt
```

Other useful tools for this job:

- `photo.levels doc=$pic black=10 white=240 gamma=1.2` for manual control: pixels at or below `black` become black, at or above `white` become white, and `gamma` above 1 lifts the midtones without touching the extremes.
- `photo.adjust kind=gamma amount=1.3` brightens shadows and midtones only.
- `photo.adjust kind=temperature amount=15` warms a cold indoor shot (negative values cool it down).
- `photo.pick_color doc=$pic x=600 y=400` prints a pixel's colour, handy to check a highlight is not blown (`#ffffffff`).

**CLI.** For a whole folder of dark photos, `photo.batch` has auto contrast built in:

```sh
wetwaresuite photo.batch input_dir=party output_dir=party-fixed auto_contrast=true
```

**Claude.**

> /home/me/Pictures/party.jpg is too dark. Fix the exposure without blowing the highlights, show me a preview, and save it as party-fixed.jpg in the same folder.

Typical calls: `photo_open`, `photo_levels` with `auto=true` and `preview=true`, `photo_adjust` (`kind=exposure` or `gamma`, with `preview=true` to compare), maybe `photo_pick_color` on the brightest area, then `photo_save`. If you do not like a step, say "undo that" and Claude calls `undo`.

## Task 3: crop, straighten and frame a photo

**Batch.**

```text
# frame.txt: crop to the subject, turn it upright, add a cream mat
photo.open path=photos/holiday-2.jpg -> p
# keep a 2000x1500 area starting 500 px from the left, 250 px from the top
photo.crop doc=$p x=500 y=250 width=2000 height=1500
# the camera was on its side: turn 90 degrees counter-clockwise
photo.rotate doc=$p angle=-90
photo.flip doc=$p direction=horizontal
photo.resize doc=$p width=1200
# grow the canvas by 60 px on each side; the new area is transparent
photo.canvas_size doc=$p width=1320 height=1720
# put a cream layer underneath so the border has a colour
paint.layer_add doc=$p name=Mat fill=#f4f1ea
paint.layer_move doc=$p to=0
photo.save doc=$p path=framed.jpg quality=88
```

Notes:

- `photo.rotate` only does quarter turns (90, 180, 270, or negative for counter-clockwise). Free-angle straightening is not available yet.
- `photo.canvas_size` keeps the pixels where `anchor` says (default `center`; also `top-left`, `bottom`, and so on) and fills the new area with transparency. Formats without transparency, such as JPEG, show it as white; the two `paint.layer_*` lines give it a colour instead. Photo and paint work on the same documents, so mixing them is normal.
- Crop, resize, rotate, flip and canvas size apply to every layer, so layers stay aligned.

**Claude.**

> Crop /home/me/scan.jpg to just the photo in the middle, turn it upright, put a 60 px cream border around it and save it as scan-framed.jpg.

Claude calls `photo_open`, `photo_preview` to find the edges, `photo_crop`, `photo_rotate`, `photo_canvas_size`, `paint_layer_add` with `fill`, `paint_layer_move`, `photo_preview` to check, and `photo_save`.

## Task 4: effects

`photo.effect` applies one filter to a layer (or `all_layers=true`). `amount` means something different for each effect:

| kind | amount |
| --- | --- |
| `grayscale`, `invert`, `sepia` | ignored |
| `blur`, `sharpen` | radius in px (try 1 to 3 for sharpen) |
| `threshold` | level 0..255; brighter pixels become white, the rest black |
| `posterize` | number of levels per channel (try 4) |
| `pixelate` | block size in px |
| `noise` | strength 0..100 |

**Batch.**

```text
photo.open path=portrait.jpg -> p
photo.effect doc=$p kind=sepia
photo.effect doc=$p kind=noise amount=8
photo.save doc=$p path=portrait-old.jpg
```

**CLI** (a whole folder): `wetwaresuite photo.batch input_dir=in output_dir=out effect=sharpen effect_amount=1`

**Claude.** "Give /home/me/portrait.jpg an old-photo look and save it as portrait-old.jpg" leads to `photo_open`, `photo_effect` (`sepia`, then `noise`) with previews, and `photo_save`.

## Task 5: check an image and its layers

```text
photo.open path=artwork.ora -> a
photo.info doc=$a
```

```
800x500, 5 layers, active layer 4 "Ink", artwork.ora
  4: Ink (normal, opacity 100%, active)
  3: Hills (normal, opacity 100%)
  2: Sun (screen, opacity 90%)
  1: Layer 1 (normal, opacity 100%)
  0: Background (normal, opacity 100%)
```

Layers are listed top first and numbered from 0 at the bottom. Edits such as `photo.adjust` apply to the active layer unless you pass `layer=` or `all_layers=true`. To work with layers (add, blend modes, merge), see [paint](paint.md).

## Task 6: sharper resizing, and how fast is my computer?

**Choosing a resize filter.** `photo.resize` has four ways to work out the new pixels. You only need to pick one when the default doesn't look right:

| Filter | Looks like | Use it for |
| --- | --- | --- |
| `bilinear` (default) | smooth | everyday resizing; when shrinking a lot (more than half) it automatically averages whole areas so there is no shimmer or jagged edges |
| `area` | smooth, a little soft | making small thumbnails from big photos |
| `lanczos` | crisp | photos you care about, shrinking or enlarging: edges and text stay sharp. It's a bit slower than the others |
| `nearest` | blocky | pixel art and icons, where you want hard square pixels |

**CLI.**

```sh
wetwaresuite photo.resize doc=$p width=1600 filter=lanczos
```

**Batch.**

```text
photo.open path=poster.png -> p
photo.resize doc=$p percent=50 filter=lanczos
photo.save doc=$p path=poster-half.png
```

**Claude.**

> Shrink poster.png to half size, keeping it as sharp as possible.

Claude calls `photo_open`, then `photo_resize` with `percent=50` and `filter=lanczos`, then `photo_save`.

**Speed test.** `photo.benchmark` makes a test picture in memory (nothing is opened or saved) and times the heavy operations: blur, the resize filters, blending three layers, and a colour adjustment. It's handy for comparing two computers, or for seeing what a big canvas will feel like before you start:

```sh
$ wetwaresuite photo.benchmark size=4096
4096x4096 px on 8 threads:
  blur (radius 8) 210.4 ms
  resize to half, bilinear 31.2 ms
  ...
```

(Your numbers will differ.) `size` is the test picture's width and height (default 2048); `threads=1` shows how fast it is on one CPU core, and the default `threads=0` uses all of them. The picture operations split their work across every core, and the result is exactly the same whatever the number of threads.

**Big pictures and undo.** Undo only remembers the parts of a picture an edit changed (in small 64x64 squares, or the exact area of a brush stroke), not the whole picture each time. So even on a very large canvas, hundreds of brush strokes take only a few megabytes of undo memory. Edits that change the size or the layer list (resize, crop, canvas size, adding or removing a layer) still remember the whole picture for that one step.

## Task 7: retouching (remove spots, blend in, erase objects)

Three commands clean up pictures. They work entirely on your computer, with no internet and no AI needed, and each one is a single undo step.

| You want to | Use | How it works, in plain words |
| --- | --- | --- |
| hide a spot, a pimple, dust on the lens or a scratch | **Healing brush** (`photo.heal`) | borrows texture from a clean spot you point at, then matches it to the colour and brightness around the blemish, so there's no visible edge |
| paste part of one picture into another so it looks like it belongs | **Seamless clone** (`photo.clone_seamless`) | keeps the detail of the pasted part but lets it take on the colours and light of where it lands |
| make something disappear: a person in the background, a sign, a power line, a date stamp | **Remove object** (`photo.remove_object`) | fills the gap with small pieces copied from around it, continuing lines and patterns into the hole |

### Marking the area

All three need to know *where*. Coordinates are pixels from the top-left corner of the picture.

- **Rectangle** (`shape=rect`, the default): `x` and `y` of the top-left corner plus `width` and `height`.
- **Ellipse** (`shape=ellipse`): the same four numbers describe the box the ellipse fits in. Good for round spots.
- **Brush stroke** (`shape=stroke`): a list of points `x1,y1,x2,y2,...` along the line, and a `radius` in pixels. Good for scratches, wires and hairs. One point (`points=120,80`) makes a single round dab.

Make the area a little bigger than the thing you're fixing: a few pixels of margin all round give the best result.

### In the window: just paint over it

The easiest way: no numbers at all. Open your picture (**Open a file** on the home screen). Above the picture you'll see the tools **Move, Brush, Eraser, Fill, Heal, Remove**. Hold the mouse still over any of them to read what it does.

**Heal: make a spot, pimple or scratch disappear**

1. Click **Heal**.
2. Set **Size** so the circle under your mouse is a little bigger than the spot. (Scroll the mouse wheel over the picture to zoom in first; it helps.)
3. Paint over the spot: press, drag over it, let go. A light blue line shows where you painted. When you let go, the window shows **Working...** for a moment and the spot is gone.

That fills the spot smoothly from the colours around it, which is perfect for skin, sky and other smooth areas. For textured areas (grass, fabric, a brick wall) tell it where to copy texture from:

4. Hold **Alt** (**Option** on a Mac) and click a clean spot nearby that looks like what should be there. If you use a pen or a touch screen, press **Pick source** instead, then tap the clean spot.
5. Now paint over the blemish. A white circle shows where the texture is being copied from; it moves along with your brush, staying the same distance away (just like the clone or healing brush in other photo editors).
6. To go back to smooth filling, press **Forget source**.

If the window says the spot to copy from is too close to the edge, Alt+click a spot further inside the picture.

**Remove: make a whole object disappear**

1. Click **Remove**.
2. Set **Size**, then paint over the thing you want gone (a person in the background, a sign, a wire) in one go. It turns see-through red so you can see what you've covered. Cover it completely, with a little margin.
3. Not right? Press **Clear** and paint again (or just paint again: the new stroke replaces the old one).
4. Press **Remove**. The window shows **Working...**, then the red area is filled with picture from around it.

Tick **Remove when I let go** to skip step 4: it removes as soon as you finish painting.

Remove works on small and medium areas (about 60000 pixels, roughly 245 by 245). If you paint a bigger area, the window tells you so in plain words; remove the thing in a few smaller parts instead.

**Every heal stroke and every Remove is one step.** Press **Ctrl+Z** to take back just the last one, and **Ctrl+Y** to redo it.

### In the window: with exact numbers

Prefer to type the area in?

1. Press **Ctrl+K** and type `heal`, `remove` or `seamless`. Pick **Healing brush**, **Remove object** or **Seamless clone**.
2. Fill in the area: choose the **shape**, then the position and size. For a healing brush, also fill in **from x** and **from y**: a clean spot nearby that has the right kind of texture (skin next to the pimple, sky next to the dust spot). Leave them on **Automatic** to fill smoothly from the edges instead, which is fine for spots on smooth areas.
3. Press **Run**. Not happy? Press **Ctrl+Z**, change the numbers, and run again.

Seamless clone has no painting tool yet; use it this way.

### On the command line and in scripts

```text
# retouch.txt
photo.open path=portrait.jpg -> p
# a spot at about (412, 300): heal it with skin texture from 30 px to its right
photo.heal doc=$p shape=ellipse x=402 y=290 width=20 height=20 from_x=442 from_y=300
# a scratch across the sky: a brush stroke, filled smoothly
photo.heal doc=$p shape=stroke points=100,80,260,95,410,90 radius=4
# a tourist in the background: content-aware fill
photo.remove_object doc=$p x=820 y=410 width=90 height=210
photo.save doc=$p path=portrait-clean.jpg
```

```sh
wetwaresuite batch retouch.txt
```

Seamless clone with two pictures: put the moon from `moon.jpg` into the sky of `night.jpg`.

```text
photo.open path=night.jpg -> night
photo.open path=moon.jpg -> moon
photo.clone_seamless doc=$night source_doc=$moon from_x=40 from_y=40 width=180 height=180 to_x=900 to_y=120
photo.save doc=$night path=night-with-moon.jpg
```

`from_x`, `from_y`, `width` and `height` pick the part of the source to copy; `to_x`, `to_y` say where its top-left lands. The pasted part is an ellipse unless you pass `shape=rect`. Leave out `source_doc` to copy within the same picture (for example, to duplicate a cloud).

### With Claude

> Remove the dust spot near the top-left of /home/me/scan.jpg and the wire across the sky, then save it as scan-clean.jpg.

Claude calls `photo_open`, `photo_preview` to find the spot and the wire, `photo_heal` for the spot (with an ellipse and a clean `from_x`/`from_y` nearby), `photo_remove_object` with `shape=stroke` along the wire, `photo_preview` again to check, and `photo_save`.

### Tips and limits

- **Healing brush** works on any size (up to about 2000x2000 pixels), and brush-sized areas take a few milliseconds. It keeps the edges of the area exactly as they were, so the borrowed texture can't leave a seam. If the result looks blotchy, the spot you borrowed from had a different texture; pick another `from_x`/`from_y`.
- **Remove object** is for small and medium things: up to 60000 pixels (about 245x245). Bigger areas would take too long, so you get a plain message asking you to select less. Remove a big object in a few overlapping parts instead. A 100x100 pixel area takes a fraction of a second.
- `patch` (default 9) is the size of the pieces Remove object copies. Use 5 to 7 for fine detail, 13 to 21 for coarse textures like gravel or foliage.
- All three work on the active layer (or `layer=`), and `preview=true` attaches a picture of the result.
- Each result says how many pixels changed (`pixels`); the healing brush and seamless clone also report `iterations` and `converged` (whether the blending finished fully; it practically always does).

## Saving

The extension of `path` picks the format:

| Extension | Keeps layers | Notes |
| --- | --- | --- |
| `.ora` | yes | OpenRaster; opens in the suite, Krita, GIMP and MyPaint |
| `.png` | no | lossless, keeps transparency |
| `.jpg` / `.jpeg` | no | `quality` 1..100 (default 90); transparency becomes white |
| `.bmp`, `.tga`, `.ppm` | no | |

Other formats (WebP, TIFF, AVIF, HEIC) go through [convert](convert.md): save a PNG, then `wetwaresuite convert.file input=out.png output=out.webp`.

In a CLI run, `photo.preview` (and `preview=true`) only reports the size of the PNG it made; previews are meant for Claude. To look at a result yourself, save it.

## Command reference

All commands take `doc` (the id of an open image, such as `img1`) unless noted. `preview=true` attaches a 512 px PNG of the result, which Claude sees as an image.

### photo.open
Open an image file as a new document and return its id. Not undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `path` | string | yes | | image file: png, jpg, bmp, tga, gif, psd, hdr, pnm/ppm/pgm, ora |

Returns `doc`, `width`, `height`, `layers`.

### photo.new
Create a blank image. Not undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `width` | integer | yes | | 1..30000 px |
| `height` | integer | yes | | 1..30000 px |
| `background` | string (colour) | no | `#ffffff` | fill colour; `transparent` for none |

### photo.info
Size, layers (index, name, blend, opacity, visibility) and active layer. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | image id |

Returns `width`, `height`, `layers`, `active_layer`.

### photo.preview
Render all visible layers to a PNG. Read-only.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `max_size` | integer | no | 512 | longest side in px (16..4096) |

### photo.save
Save to a file; the extension picks the format. Not undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `path` | string | yes | | output file (`.ora`, `.png`, `.jpg`, `.jpeg`, `.bmp`, `.tga`, `.ppm`); the folder must exist |
| `quality` | integer | no | 90 | JPEG quality 1..100 |

Returns `path`, `bytes`.

### photo.close
Close a document; unsaved changes are lost.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | image id |

### photo.resize
Scale the whole image (all layers).

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `width` | integer | no | | new width; with no height, keeps the aspect ratio |
| `height` | integer | no | | new height; with no width, keeps the aspect ratio |
| `percent` | number | no | | scale by a percentage instead, e.g. 50 (wins over width/height) |
| `filter` | `bilinear`, `area`, `lanczos` or `nearest` | no | `bilinear` | `bilinear` is smooth and quick (and averages whole areas when shrinking more than 2x); `area` always averages; `lanczos` is the sharpest; `nearest` keeps hard pixel edges, good for pixel art |
| `preview` | boolean | no | false | attach a preview |

Give at least one of `width`, `height` or `percent`.

### photo.crop
Keep a rectangle, in pixels from the top-left.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | image id |
| `x` | integer | yes | left edge |
| `y` | integer | yes | top edge |
| `width` | integer | yes | width to keep (at least 1) |
| `height` | integer | yes | height to keep (at least 1) |
| `preview` | boolean | no | attach a preview |

### photo.canvas_size
Change the canvas without scaling; new area is transparent.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `width` | integer | yes | | new canvas width (1..30000) |
| `height` | integer | yes | | new canvas height (1..30000) |
| `anchor` | `center`, `top-left`, `top`, `top-right`, `left`, `right`, `bottom-left`, `bottom`, `bottom-right` | no | `center` | where the existing image sits |
| `preview` | boolean | no | false | attach a preview |

### photo.rotate
Rotate clockwise by quarter turns.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | image id |
| `angle` | integer | yes | 90, 180 or 270; negative for counter-clockwise; must be a multiple of 90 |
| `preview` | boolean | no | attach a preview |

### photo.flip
Mirror the whole image.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | image id |
| `direction` | `horizontal` or `vertical` | yes | which way to flip |
| `preview` | boolean | no | attach a preview |

### photo.adjust
Tonal or colour adjustment.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `kind` | `brightness`, `contrast`, `saturation`, `hue`, `exposure`, `gamma`, `temperature`, `vibrance` | yes | | what to adjust |
| `amount` | number | yes | | brightness, contrast, saturation, vibrance, temperature: -100..100; hue: degrees; exposure: stops (0.5 = half a stop brighter); gamma: 0.1..10 (1 = unchanged) |
| `layer` | integer | no | active layer | layer index, 0 = bottom |
| `all_layers` | boolean | no | false | apply to every layer |
| `preview` | boolean | no | false | attach a preview |

### photo.levels
Remap tones, or auto contrast.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `black` | number | no | 0 | input black point 0..255 |
| `white` | number | no | 255 | input white point 0..255 |
| `gamma` | number | no | 1 | midtone gamma; above 1 is brighter |
| `auto` | boolean | no | false | auto contrast instead of manual levels |
| `layer` | integer | no | active layer | layer index |
| `all_layers` | boolean | no | false | apply to every layer |
| `preview` | boolean | no | false | attach a preview |

### photo.effect
Apply a filter (see the table in task 4 for `amount`).

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| `kind` | `grayscale`, `invert`, `sepia`, `threshold`, `posterize`, `blur`, `sharpen`, `pixelate`, `noise` | yes | | which effect |
| `amount` | number | no | 0 | strength |
| `layer` | integer | no | active layer | layer index |
| `all_layers` | boolean | no | false | apply to every layer |
| `preview` | boolean | no | false | attach a preview |

### photo.pick_color
Read one pixel. Read-only.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `doc` | string | yes | image id |
| `x` | integer | yes | x in px |
| `y` | integer | yes | y in px |
| `layer` | integer | no | read this layer instead of the flattened image |

Returns `color` (`#rrggbbaa`), `r`, `g`, `b`, `a`.

### photo.benchmark
Time blur, resize (bilinear, area, lanczos), 3-layer blending and a colour adjustment on a generated test image. Read-only; nothing is opened or saved.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `size` | integer | no | 2048 | test image width and height in px, 16..16384 |
| `threads` | integer | no | 0 | threads to use; 0 = all CPU cores |

Returns `blur_ms`, `resize_bilinear_ms`, `resize_area_ms`, `resize_lanczos_ms`, `composite_ms`, `adjust_ms`, `size` and `threads`.

### photo.batch
Process every image in a folder. Does not use open documents; not undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `input_dir` | string | yes | | folder with source images |
| `output_dir` | string | yes | | folder for results, created if missing; must differ from `input_dir` |
| `format` | `png`, `jpg`, `bmp`, `tga`, `ppm`, `ora` | no | `jpg` | output format |
| `max_size` | integer | no | 0 | fit within this many px on the longest side; 0 keeps the size |
| `quality` | integer | no | 90 | JPEG quality 1..100 |
| `auto_contrast` | boolean | no | false | apply auto contrast |
| `effect` | effect kind (as `photo.effect`) | no | | optional effect |
| `effect_amount` | number | no | | effect strength |
| `recursive` | boolean | no | false | include subfolders |
| `overwrite` | boolean | no | false | replace results that already exist (sources are never replaced) |

Returns `processed` and `failed` counts. Fails if no images are found.

### Area parameters (photo.heal and photo.remove_object)

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `shape` | `rect`, `ellipse`, `stroke` | no | `rect` | shape of the area |
| `x`, `y` | integer | for rect/ellipse | | top-left of the area (or of the ellipse's box) |
| `width`, `height` | integer | for rect/ellipse | | size of the area |
| `points` | number list | for stroke | | `x1,y1,x2,y2,...` along the brush stroke |
| `radius` | number | no | 10 | brush radius in px (stroke) |

### photo.heal
Healing brush: hide a blemish with texture from a clean spot, matched to the colours around it (Poisson image editing). Undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| area parameters | | yes | | see above |
| `from_x`, `from_y` | integer | no | | clean spot the middle of the area borrows texture from; leave out to fill smoothly from the edges |
| `layer` | integer | no | active layer | layer index |
| `preview` | boolean | no | false | attach a preview |

Returns `pixels`, `iterations` and `converged`.

### photo.clone_seamless
Paste a rectangle or ellipse from an open image (or the same one) so it blends into its new place. Undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image to paste into |
| `source_doc` | string | no | same as `doc` | image to copy from |
| `source_layer` | integer | no | its active layer | layer of the source to copy |
| `from_x`, `from_y` | integer | yes | | top-left of the area to copy, in the source |
| `width`, `height` | integer | yes | | size of the area |
| `to_x`, `to_y` | integer | yes | | where the top-left lands in `doc` |
| `shape` | `ellipse`, `rect` | no | `ellipse` | shape of the pasted area |
| `layer` | integer | no | active layer | layer of `doc` to paste into |
| `preview` | boolean | no | false | attach a preview |

### photo.remove_object
Content-aware fill: erase an area and fill it from its surroundings (exemplar-based inpainting). At most 60000 pixels. Undoable.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `doc` | string | yes | | image id |
| area parameters | | yes | | see above |
| `patch` | integer | no | 9 | size of the copied pieces, 3..31 px |
| `layer` | integer | no | active layer | layer index |
| `preview` | boolean | no | false | attach a preview |

Returns `pixels`.
