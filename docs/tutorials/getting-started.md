# Getting started

This tutorial takes you from a fresh checkout to running your first commands. By the end you will know how to build wetwaresuite, find any command and its parameters, pass arguments correctly, and understand how documents, ids and undo work.

There is no graphical interface yet. Everything in the suite is a *command* (for example `photo.resize` or `paint.stroke`), and the GUI that arrives later will be a thin layer of buttons that call exactly these commands. So what you learn here keeps working when the GUI lands: the same commands drive the CLI, batch scripts, Claude over MCP, and (later) the GUI.

## 1. Build from source

You need:

- CMake 3.21 or newer
- a C++20 compiler (gcc 11+, clang 14+ or MSVC 2022)
- Rust, only if you also want the `hub` install manager

From the repository root:

```sh
cmake -S . -B build
cmake --build build
```

For a faster binary, ask for a release build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Optionally run the tests to make sure everything works on your machine:

```sh
ctest --test-dir build --output-on-failure
```

The build produces one binary that contains the whole suite:

```
build/apps/wetwaresuite/wetwaresuite          (Linux and macOS)
build/apps/wetwaresuite/Release/wetwaresuite.exe   (Windows with Visual Studio)
```

It also makes one small program per app in `build/bin/` (`wetwaresuite-image`, `wetwaresuite-model`, ...). They have the same commands as the all-in-one binary, just fewer of them; see [the apps](apps.md) to install or build only the ones you need.

The rest of the tutorials just write `wetwaresuite`. Either put the folder on your `PATH`:

```sh
export PATH="$PWD/build/apps/wetwaresuite:$PATH"
```

or call it by its full path. Check that it runs:

```sh
$ wetwaresuite --version
wetwaresuite 0.2.0
```

There is also a small standalone `build/tools/toolkit/wetwaresuite-toolkit` binary; see the [toolkit tutorial](toolkit.md).

## 2. Find commands with `list`

```sh
$ wetwaresuite list
```

prints every command, grouped by tool. To see one tool only, name it:

```sh
$ wetwaresuite list photo
photo
  photo.adjust  Adjust colours
  photo.batch  Batch process folder  (read-only/output)
  photo.canvas_size  Canvas size
  photo.close  Close image  (read-only/output)
  ...
```

The tools available today are `photo`, `paint`, `vector`, `convert` and `toolkit`. Commands marked `(read-only/output)` do not change an open document, so they are not recorded in the undo history: they open or create documents, save files, convert files or just report information. Everything else edits a document and can be undone.

## 3. Read a command's parameters with `help`

```sh
$ wetwaresuite help photo.resize
photo.resize: Resize image
Scale the whole image. Give width, height or both; with only one, the other keeps the aspect ratio.

usage: photo.resize doc=<string> [width=<integer>] [height=<integer>] [percent=<number>] [filter=bilinear|nearest] [preview=<boolean>]
  doc (string, required)  id of an open image, e.g. img1
  width (integer)  new width in px (0 = keep aspect)
  height (integer)  new height in px (0 = keep aspect)
  percent (number)  scale by a percentage instead, e.g. 50
  filter (string)  resampling filter [bilinear|nearest] (default bilinear)
  preview (boolean)  attach a PNG preview of the result (default false)
```

How to read it:

- Parameters in `[square brackets]` in the usage line are optional; the others are required.
- `a|b|c` means you must pick one of those words.
- The type in parentheses tells you what to write (see the next section).
- `(default ...)` is what you get if you leave the parameter out.

`help` is always the ground truth. If a tutorial and `help` ever disagree, trust `help`.

## 4. Run a command

There are two equivalent ways:

```sh
wetwaresuite run toolkit.inspect path=photo.jpg
wetwaresuite toolkit.inspect path=photo.jpg
```

Output looks like this:

```
$ wetwaresuite toolkit.inspect path=dark.jpg
format: JPEG image
category: image
opens in: photo
mime: image/jpeg
size: 42796 bytes
```

Most commands print a one-line message followed by `key: value` lines with the details (the same details Claude receives as structured data over MCP). On success, output goes to stdout; on failure, the error message goes to stderr and the exit code is non-zero.

## 5. Writing arguments

Every argument is `key=value`, with no spaces around the `=`. Unknown keys are rejected with a hint showing the correct usage, which catches typos early:

```
$ wetwaresuite run photo.open file=dark.jpg
error: unknown parameter 'file' for photo.open (try: photo.open path=<string>)
```

### Types

| Type in `help` | What to write | Examples |
| --- | --- | --- |
| string | any text | `path=out.png`, `kind=exposure` |
| integer | a whole number (hex like `0x20` also works) | `width=800`, `angle=-90` |
| number | a decimal number | `amount=0.5`, `opacity=60` |
| boolean | `true`/`false`, `yes`/`no`, `on`/`off` or `1`/`0` | `auto=true`, `overwrite=yes` |
| array | numbers separated by commas, semicolons or spaces | `points=10,20,30,40` |

### Lists of numbers

Strokes, polygons and pen pressures take a flat list of numbers. For points, the list is `x1,y1,x2,y2,...` in pixels from the top-left corner:

```sh
points=10,20,30,40,50,20          # three points: (10,20) (30,40) (50,20)
points="10 20, 30 40, 50 20"      # the same, with spaces (quoted for the shell)
pressures=0.2,0.9,0.4             # one pressure per point, 0..1
```

### Colours

Anywhere a command asks for a colour you can write:

| Form | Example | Notes |
| --- | --- | --- |
| `#rgb` / `#rrggbb` | `#f80`, `#268bd2` | fully opaque |
| `#rgba` / `#rrggbbaa` | `#ffffff22` | last pair is alpha, `00` = invisible |
| `rgb(r,g,b)` | `rgb(255,128,0)` | 0..255 per channel |
| `rgba(r,g,b,a)` | `rgba(0,128,0,0.5)` | alpha 0..1 (values above 1 are read as 0..255) |
| a name | `red`, `gold` | `black white red green lime blue yellow cyan magenta gray grey orange gold purple pink brown transparent` |

Anything else is rejected with a clear message, for example `'navy' isn't a colour`.

### Quoting

On the command line your shell splits words before wetwaresuite sees them, so quote values that contain spaces or characters the shell treats specially, such as parentheses:

```sh
wetwaresuite convert.file input="My Photos/cat.png" output="My Photos/cat.jpg"
wetwaresuite photo.batch input_dir=raw output_dir=web effect=sepia     # no quotes needed
```

and in a shell, `background="rgb(10,20,30)"` needs the quotes because of the parentheses. Batch scripts have their own simple quoting rules; see [headless and batch](headless-and-batch.md).

## 6. Documents and ids

Commands that open or create something give you a document id:

```
$ wetwaresuite run photo.open path=dark.jpg
opened dark.jpg as img1
doc: img1
width: 1200
height: 800
layers: 1
```

You then pass that id as `doc=img1` to every command that edits it. Images get ids `img1`, `img2`, ... and vector drawings get `vec1`, `vec2`, ... (see the [vector tutorial](vector.md)). Ids are handed out in order and are not reused after a document is closed, so after closing `img1` the next one is `img2`. Photo and paint share the same kind of document: you can open a photo and paint on it, or paint a picture and run photo effects on it.

Important: open documents live in memory inside one running `wetwaresuite` process. A single `wetwaresuite run ...` starts, runs one command and exits, so a document it opened is gone by the next command:

```
$ wetwaresuite run photo.open path=dark.jpg
opened dark.jpg as img1
...
$ wetwaresuite run photo.resize doc=img1 width=10
no open image called 'img1' (use photo.open or paint.new first)
```

That gives you a simple rule of thumb:

- **One-shot commands** that read a file and write a file work great with `run`: everything in `convert.*`, `photo.batch`, `toolkit.*`.
- **Multi-step editing** (open, adjust, crop, save) happens inside a single process: a [batch script](headless-and-batch.md) or a [Claude session over MCP](mcp.md).

Here is the multi-step version as a batch script, fed on stdin:

```sh
wetwaresuite batch <<'EOF'
photo.open path=dark.jpg -> pic
photo.levels doc=$pic auto=true
photo.resize doc=$pic width=800
photo.save doc=$pic path=dark-web.jpg quality=85
EOF
```

`-> pic` stores the new id in a variable and `$pic` uses it, so the script does not need to know it is `img1`.

## 7. Undo and redo

Every editing command is recorded in one shared history for the whole session (across all open documents). You can step back and forward through it:

```sh
wetwaresuite batch <<'EOF'
photo.new width=100 height=50 background=white -> img
photo.effect doc=$img kind=invert
photo.pick_color doc=$img x=5 y=5
undo
photo.pick_color doc=$img x=5 y=5
redo
photo.pick_color doc=$img x=5 y=5
EOF
```

prints `#000000ff` (inverted), then `undone` and `#ffffffff` (back to white), then `redone` and `#000000ff` again.

Things to know:

- `undo` and `redo` are built into batch scripts; over MCP they are tools called `undo` and `redo`, and `history` lists what can be undone.
- Opening, creating, saving and closing are not edits, so they are not undone. Undo after a save changes the document in memory, not the file on disk.
- Running a new edit after an undo discards the redo steps, as in any editor.
- `undo` with nothing to undo is an error (`nothing to undo`).

## 8. Exit codes

| Code | Meaning |
| --- | --- |
| 0 | success |
| 1 | the command failed (bad parameter value, missing file, nothing to undo, ...) or a batch script had at least one failing line |
| 2 | usage problem: no arguments, an unknown command name, or a batch file that cannot be opened |

## Where next

- [Headless and batch](headless-and-batch.md): scripts, variables, CI and cron.
- [Using wetwaresuite with Claude (MCP)](mcp.md): let Claude drive every command.
- Per tool: [photo](photo.md), [paint](paint.md), [vector](vector.md), [convert](convert.md), [toolkit](toolkit.md).
