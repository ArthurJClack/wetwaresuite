# Convert: change any file's type

`convert` turns one file format into another: images, SVG drawings, audio, video, office documents and markup. You name the output file and its extension picks the format, so converting is usually one command:

```sh
wetwaresuite convert.file input=interview.wav output=interview.mp3
```

There is no GUI yet; the GUI that comes later will call these same commands. Each task below shows the CLI way, the batch-script way and the "ask Claude" way.

## How it works: backends

wetwaresuite converts images and SVG itself. For everything else it hands the job to a free, well-known program if it is installed. These are optional: install only the ones you need.

| Files | Backend | Included? |
| --- | --- | --- |
| PNG, JPEG, BMP, TGA, GIF, PSD, HDR, PNM, ORA to PNG, JPEG, BMP, TGA, PPM, ORA | built-in raster engine | yes |
| SVG to PNG, JPEG, BMP, TGA, PPM (or a cleaned-up SVG) | the suite's [vector](vector.md) tool | yes |
| audio, video, and extra image formats (AVIF, HEIC, ICO, JXL, EXR) | [ffmpeg](https://ffmpeg.org) | optional |
| Word, Excel, PowerPoint, OpenDocument, RTF and friends | [LibreOffice](https://www.libreoffice.org) (`soffice`) | optional |
| Markdown, HTML, reStructuredText, LaTeX, Org, EPUB, plain text | [pandoc](https://pandoc.org) | optional |
| 3D: STEP, STL, OBJ, PLY, OFF, glTF/GLB, 3MF, FBX (to each other, or to a picture) | built-in mesh engine | yes |

The helper programs just need to be on your `PATH`. On Debian or Ubuntu: `sudo apt install ffmpeg libreoffice-writer pandoc` (add `libreoffice-calc` and `libreoffice-impress` for spreadsheets and slides). On macOS with Homebrew: `brew install ffmpeg pandoc` and `brew install --cask libreoffice`. On Windows: `winget install ffmpeg`, `winget install TheDocumentFoundation.LibreOffice` and `winget install pandoc`.

STEP files from other CAD programs come in as fine triangles (curved faces tessellated), and a mesh written to STEP keeps flat faces whole, so `convert.file input=part.stl output=part.step` gives FreeCAD or Fusion a solid. IGES, Collada, USD, Blender and 3DS files aren't built in; export them to STEP, glTF or STL from the program that made them.

### What is available on this machine?

```sh
$ wetwaresuite convert.formats
images (built in, ready): bmp, gif, hdr, jpeg, jpg, ora, pgm, pic, png, pnm, ppm, psd, tga, tif, tiff, webp -> bmp, jpeg, jpg, ora, png, ppm, tga, tif, tiff, webp
svg (vector tool, ready): svg -> bmp, jpeg, jpg, png, ppm, svg, tga
audio/video/more images (ffmpeg, ready): aac, ac3, aif, aiff, amr, flac, m4a, mp3, ogg, opus, wav, wma, 3gp, avi, flv, gif, m4v, mkv, mov, mp4, mpeg, mpg, ts, webm, wmv, avif, exr, heic, heif, ico, jxl, tif, tiff, webp
office documents (LibreOffice, ready): doc, docx, key, numbers, odp, ods, odt, pages, ppt, pptx, rtf, wpd, xls, xlsx -> csv, doc, docx, epub, html, odp, ods, odt, pdf, ppt, pptx, rtf, txt, xls, xlsx
markup (pandoc, ready): docx, epub, htm, html, markdown, md, odt, org, rst, tex, txt -> docx, epub, html, md, odt, org, pdf, rst, tex, txt
3D meshes (built in, ready): 3mf, fbx, glb, gltf, obj, off, ply, step, stl, stp -> 3mf, fbx, glb, gltf, obj, off, ply, step, stl, stp, or rendered to bmp, jpeg, jpg, png, ppm, tga
other 3D and CAD (3ds, blend, dae, iges, igs, usd, usda, usdc, usdz, wrl, x3d): not built in yet
ffmpeg: true
libreoffice: true
pandoc: true
vector: true
```

A backend that is missing shows `not installed` and its `key: value` line says `false`. For ffmpeg, the formats listed are both inputs and outputs: any of them can become any other where that makes sense.

### Will this one conversion work?

`convert.check` answers without converting anything (it looks only at the names, so the file does not need to exist yet):

```sh
$ wetwaresuite convert.check input=report.docx to=pdf
libreoffice can do it
backend: libreoffice
ready: true

$ wetwaresuite convert.check input=model.obj to=stl
no converter to .stl for model.obj
```

If the backend exists but the program is missing, it says `libreoffice would do it, but isn't installed` and `ready: false`. The exit code is 0 when a backend exists (even if it is not installed) and 1 when there is none, so for scripts, check `ready` too.

## Task 1: WAV to MP3 (and other audio)

**CLI.**

```sh
$ wetwaresuite convert.file input=interview.wav output=interview.mp3 quality=80
converted interview.wav to interview.mp3 with ffmpeg
output: interview.mp3
backend: ffmpeg
bytes: 31171
```

- `quality` 1..100 maps to MP3 VBR quality (higher is better and bigger). Leave it out for ffmpeg's default.
- Trim while converting: `start=` and `duration=` are in seconds.

```sh
wetwaresuite convert.file input=interview.wav output=clip.mp3 start=60 duration=30
```

- The output's extension chooses the codec: `.flac`, `.ogg`, `.opus`, `.m4a`, `.wav`, ...
- Video in, audio out drops the picture: `wetwaresuite convert.file input=talk.mp4 output=talk.mp3`.

**Batch** (the whole folder): `wetwaresuite convert.batch input_dir=recordings to=mp3 output_dir=mp3 quality=70`

**Claude.**

> Convert /home/me/podcast/ep12.wav to an MP3 at good quality, and also make a 30 second preview clip starting at 1:00.

Claude calls `convert_check` (or `convert_formats`) to confirm ffmpeg is there, then `convert_file` twice, the second with `start=60` and `duration=30`.

## Task 2: video

```sh
# smaller copy: 640 px wide (height follows), lower quality
wetwaresuite convert.file input=talk.mp4 output=talk-small.mp4 width=640 quality=60
# a different container and codec, picked by extension
wetwaresuite convert.file input=talk.mp4 output=talk.webm width=480 quality=50
# an animated GIF of two seconds starting at 0:01
wetwaresuite convert.file input=talk.mp4 output=talk.gif width=320 start=1 duration=2
```

For video, `quality` maps to the encoder's CRF (higher quality, bigger file).

Grab a single frame as an image by picking the time with `start`:

```sh
wetwaresuite convert.file input=talk.mp4 output=frame.png start=3
```

## Task 3: DOCX to PDF (and other documents)

**CLI.**

```sh
$ wetwaresuite convert.file input=report.docx output=report.pdf
converted report.docx to report.pdf with LibreOffice
output: report.pdf
backend: libreoffice
bytes: 18406
```

LibreOffice runs headless in the background; expect a second or two per document. The same command handles `.xlsx` to `.pdf` or `.csv`, `.pptx` to `.pdf`, `.doc` to `.docx`, `.odt` to `.docx`, and so on.

Markup goes through pandoc:

```sh
wetwaresuite convert.file input=notes.md output=notes.docx
wetwaresuite convert.file input=notes.md output=notes.html
```

Markdown to PDF needs pandoc and LibreOffice: pandoc lays the Markdown out as a Word document and LibreOffice prints that to PDF, all in one step:

```sh
wetwaresuite convert.file input=notes.md output=notes.pdf
```

**Batch.** A whole folder of documents to PDF:

```sh
wetwaresuite convert.batch input_dir=docs to=pdf output_dir=pdf
```

(this picks up every file that can become a PDF, including `.md` files, with the caveat above).

**Claude.**

> Turn every Word document in /home/me/contracts into a PDF in /home/me/contracts/pdf.

`convert_batch` with `to=pdf` and `output_dir`.

## Task 4: PNG to JPEG for a whole folder

**CLI.**

```sh
$ wetwaresuite convert.batch input_dir=shots to=jpg output_dir=shots-jpg quality=85
converted 2 of 2 files to .jpg
converted: 2
failed: 0
```

- Files that are already the target format, or that cannot become it (such as `readme.txt`), are skipped.
- Without `output_dir`, results are written next to each original.
- `recursive=true` includes subfolders and keeps their structure in `output_dir`.
- `width=` resizes as it converts (height follows).
- Existing outputs are never replaced unless you pass `overwrite=true`; running the same command twice reports each file as `already exists` and exits with 1.
- If a folder holds `photo.png` and `photo.jpg`, both map to the same output name; the second one is reported as `already exists`.

Transparency becomes white in JPEG. To WebP or TIFF instead, just change `to=` (both built in); AVIF uses ffmpeg.

**Batch.** In a script, mix conversions with other steps:

```text
convert.batch input_dir=shots to=jpg output_dir=web quality=85 overwrite=true
convert.file input=logo.svg output=web/logo@2x.png scale=2 overwrite=true
```

**Claude.**

> Convert all the PNGs in /home/me/screenshots to JPEG at quality 85, into /home/me/screenshots/jpg.

`convert_batch` with `to=jpg`, `quality=85`, `output_dir`.

## Task 5: SVG to PNG

SVG rendering uses the built-in [vector](vector.md) tool. `scale` sets pixels per SVG unit:

```sh
wetwaresuite convert.file input=logo.svg output=logo.png
wetwaresuite convert.file input=logo.svg output=logo@2x.png scale=2
```

Use `scale` rather than `width` or `height`: for SVG input those are currently ignored and the image comes out at the drawing's own size. `vector.convert input=... output=... scale=... quality=...` does the same job from the vector tool directly. SVG can be rendered to PNG, JPEG, BMP, TGA and PPM; for WebP, render a PNG first and convert that.

## Tips

- `convert.file` never overwrites unless `overwrite=true`, and refuses to write over its own input.
- It creates missing output folders for you.
- The output needs an extension (`output=out` fails with a hint).
- [photo](photo.md) opens only the built-in image formats (WebP and TIFF included); to edit a HEIC or AVIF, convert it to PNG first.
- `convert` commands do not open documents, so they are not part of undo history.

## Command reference

### convert.file
Convert one file; the output extension picks the format and backend.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `input` | string | yes | | file to convert |
| `output` | string | yes | | file to write; its extension sets the format |
| `quality` | integer | no | backend default (JPEG 90) | 1..100 for JPEG, MP3 and video |
| `width` | integer | no | | resize images, SVG and video to this width; height follows unless given |
| `height` | integer | no | | resize images, SVG and video to this height |
| `scale` | number | no | 1 | SVG to raster: pixels per unit |
| `start` | number | no | | audio/video: start time in seconds |
| `duration` | number | no | | audio/video: length in seconds |
| `overwrite` | boolean | no | false | replace the output if it exists |

Returns `output`, `backend` (`raster`, `vector`, `ffmpeg`, `libreoffice` or `pandoc`) and `bytes`.

### convert.batch
Convert every suitable file in a folder.

| Parameter | Type | Required | Default | Description |
| --- | --- | --- | --- | --- |
| `input_dir` | string | yes | | folder with files to convert |
| `to` | string | yes | | target extension, e.g. `jpg`, `mp3`, `pdf` (a leading dot and capitals are fine) |
| `output_dir` | string | no | next to each original | where results go |
| `recursive` | boolean | no | false | include subfolders |
| `quality` | integer | no | | 1..100 for lossy formats |
| `width` | integer | no | | resize images and video to this width |
| `overwrite` | boolean | no | false | replace existing outputs |

Returns `converted` and `failed` counts. Succeeds if at least one file converted.

### convert.formats
List the conversions available on this machine. No parameters. Returns `ffmpeg`, `libreoffice`, `pandoc` and `vector` as true or false.

### convert.check
Say whether a conversion is possible and which backend would do it, without converting.

| Parameter | Type | Required | Description |
| --- | --- | --- | --- |
| `input` | string | yes | file name (only its extension is used) |
| `to` | string | yes | target extension |

Returns `backend` and `ready`.
