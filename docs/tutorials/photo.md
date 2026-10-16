# Photo: editing images

`photo` is the suite's image editor: open, adjust, crop, resize, rotate, apply effects, batch-process folders and save. It reads PNG, JPEG, BMP, TGA, GIF, PSD (flattened), HDR, PNM and layered OpenRaster (`.ora`), and saves PNG, JPEG, BMP, TGA, PPM and `.ora`.

There is no GUI yet. Every task below is done with commands, and the GUI that comes later will call exactly these commands from its buttons and sliders. Each task is shown three ways:

- **CLI**: a single `achclwss` command, for jobs that read files and write files.
- **Batch**: a script for multi-step edits (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP and the tools it will call (see [mcp.md](mcp.md)).

New to the CLI? Start with [getting started](getting-started.md).

## Task 1: resize a folder of photos for the web

You have a folder of 6000-pixel camera JPEGs and want light copies for a website.

**CLI.** `photo.batch` does the whole folder in one command. Originals stay as they are; results go to the output folder, which is created if needed:

```sh
$ achclwss photo.batch input_dir=photos output_dir=web max_size=1600 quality=82
processed 3 of 3 images into web
processed: 3
failed: 0
```

`max_size` fits each image within 1600 px on its longest side (portrait and landscape alike) and never enlarges smaller images. Add `recursive=true` to include subfolders (the folder structure is kept), `auto_contrast=true` to give flat photos some punch, or an effect:

```sh
achclwss photo.batch input_dir=photos output_dir=thumbs max_size=300 format=png effect=grayscale
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
achclwss batch fix-dark.txt
```

Other useful tools for this job:

- `photo.levels doc=$pic black=10 white=240 gamma=1.2` for manual control: pixels at or below `black` become black, at or above `white` become white, and `gamma` above 1 lifts the midtones without touching the extremes.
- `photo.adjust kind=gamma amount=1.3` brightens shadows and midtones only.
- `photo.adjust kind=temperature amount=15` warms a cold indoor shot (negative values cool it down).
- `photo.pick_color doc=$pic x=600 y=400` prints a pixel's colour, handy to check a highlight is not blown (`#ffffffff`).

**CLI.** For a whole folder of dark photos, `photo.batch` has auto contrast built in:

```sh
achclwss photo.batch input_dir=party output_dir=party-fixed auto_contrast=true
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

**CLI** (a whole folder): `achclwss photo.batch input_dir=in output_dir=out effect=sharpen effect_amount=1`

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

## Saving

The extension of `path` picks the format:

| Extension | Keeps layers | Notes |
| --- | --- | --- |
| `.ora` | yes | OpenRaster; opens in the suite, Krita, GIMP and MyPaint |
| `.png` | no | lossless, keeps transparency |
| `.jpg` / `.jpeg` | no | `quality` 1..100 (default 90); transparency becomes white |
| `.bmp`, `.tga`, `.ppm` | no | |

Other formats (WebP, TIFF, AVIF, HEIC) go through [convert](convert.md): save a PNG, then `achclwss convert.file input=out.png output=out.webp`.

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
| `filter` | `bilinear` or `nearest` | no | `bilinear` | `nearest` keeps hard pixel edges, good for pixel art |
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
